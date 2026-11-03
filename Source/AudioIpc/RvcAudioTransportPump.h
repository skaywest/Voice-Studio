#pragma once

#include "AudioIpc/AudioBlockTransport.h"
#include "AudioIpc/RvcAudioStreamAdapter.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace voice_studio::ipc
{
class RvcAudioTransportPump final
{
public:
    static constexpr std::uint32_t kPendingCapacity = 4;
    static constexpr std::uint32_t kSampleRate = 48000;
    static constexpr std::uint64_t kDefaultDeadlineBudgetNs = 200'000'000ull;

    explicit RvcAudioTransportPump(RvcAudioStreamAdapter& adapter) noexcept : stream(adapter) {}
    RvcAudioTransportPump(const RvcAudioTransportPump&) = delete;
    RvcAudioTransportPump& operator=(const RvcAudioTransportPump&) = delete;

    // Control-thread only. Audio thread produces adapter input; this pump
    // transfers complete mono RVC blocks into the shared-memory input slots.
    std::uint32_t publishPendingInput(AudioBlockRegion& region, std::uint64_t nowNs,
        std::uint64_t deadlineBudgetNs = kDefaultDeadlineBudgetNs) noexcept
    {
        if (nowNs == 0 || deadlineBudgetNs == 0)
            return 0;
        std::uint32_t published = 0;
        while (pendingCount < kPendingCapacity)
        {
            RvcAudioStreamAdapter::InputBlock block{};
            if (!stream.tryPopInputBlock(block))
                break;
            const auto maxNs = (std::numeric_limits<std::uint64_t>::max)();
            const auto deadline = nowNs > maxNs - deadlineBudgetNs ? maxNs : nowNs + deadlineBudgetNs;
            if (!tryPublishInputAudioBlock(region, block.samples.data(),
                    RvcAudioStreamAdapter::kBlockFrames, 1, kSampleRate,
                    block.sequence, block.generation, deadline))
            {
                droppedInput.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            pending[(pendingRead + pendingCount) % kPendingCapacity] =
                PendingBlock{ block.sequence, block.generation, deadline };
            ++pendingCount;
            ++published;
        }
        return published;
    }

    // Control-thread only. The single Worker stream is consumed in input order.
    std::uint32_t collectReadyOutput(AudioBlockRegion& region, std::uint64_t nowNs) noexcept
    {
        if (nowNs == 0)
            return 0;
        std::uint32_t collected = 0;
        while (pendingCount != 0)
        {
            const auto expected = pending[pendingRead];
            if (nowNs >= expected.deadlineNs)
            {
                popPending();
                expiredOutput.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            std::array<float, RvcAudioStreamAdapter::kBlockFrames> samples{};
            if (!tryConsumeOutputAudioBlock(region, samples.data(),
                    static_cast<std::uint32_t>(samples.size()),
                    RvcAudioStreamAdapter::kBlockFrames, 1, kSampleRate,
                    expected.sequence, expected.generation, nowNs))
                break;
            if (!stream.publishConvertedBlock(samples.data(),
                    RvcAudioStreamAdapter::kBlockFrames,
                    expected.sequence, expected.generation))
                droppedOutput.fetch_add(1, std::memory_order_relaxed);
            else
                ++collected;
            popPending();
        }
        return collected;
    }

    // Only after the old Worker has exited and its mapping has been detached.
    void resetForNewWorkerGeneration() noexcept { pendingRead = 0; pendingCount = 0; }
    std::uint32_t pendingWorkerBlocks() const noexcept { return pendingCount; }
    std::uint64_t droppedInputBlockCount() const noexcept { return droppedInput.load(std::memory_order_relaxed); }
    std::uint64_t droppedOutputBlockCount() const noexcept { return droppedOutput.load(std::memory_order_relaxed); }
    std::uint64_t expiredOutputBlockCount() const noexcept { return expiredOutput.load(std::memory_order_relaxed); }

private:
    struct PendingBlock { std::uint64_t sequence = 0, generation = 0, deadlineNs = 0; };
    void popPending() noexcept { pendingRead = (pendingRead + 1) % kPendingCapacity; --pendingCount; }

    RvcAudioStreamAdapter& stream;
    std::array<PendingBlock, kPendingCapacity> pending{};
    std::uint32_t pendingRead = 0;
    std::uint32_t pendingCount = 0;
    std::atomic<std::uint64_t> droppedInput{0}, droppedOutput{0}, expiredOutput{0};
};
} // namespace voice_studio::ipc
