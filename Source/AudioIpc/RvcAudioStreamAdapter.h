#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace voice_studio::ipc
{
// Fixed-size, bounded SPSC adapter between arbitrary host callback sizes and
// the first RVC realtime profile (48 kHz, 60 ms, mono). This component is not
// connected to the live processor yet; its producer/consumer methods are
// intended for the audio and Worker threads respectively.
class RvcAudioStreamAdapter final
{
public:
    static constexpr std::uint32_t kBlockFrames = 2880;
    static constexpr std::uint32_t kQueueCapacity = 2;
    static constexpr std::uint32_t kMaxHostCallbackFrames = 8192;

    struct InputBlock
    {
        std::uint64_t sequence = 0;
        std::uint64_t generation = 0;
        std::array<float, kBlockFrames> samples{};
    };

    struct OutputRead
    {
        std::uint32_t frames = 0;
        std::uint64_t sequence = 0;
    };

    RvcAudioStreamAdapter() = default;
    RvcAudioStreamAdapter(const RvcAudioStreamAdapter&) = delete;
    RvcAudioStreamAdapter& operator=(const RvcAudioStreamAdapter&) = delete;

    // Audio-thread producer. Mono uses left; stereo is deterministically
    // averaged. A generation change discards the incomplete prior input block.
    // Complete blocks that encounter a full queue are dropped as a whole.
    bool pushHostInput(const float* left, const float* right,
                       std::uint32_t frames, std::uint32_t channels,
                       std::uint64_t generation) noexcept
    {
        if (left == nullptr || frames == 0 || frames > kMaxHostCallbackFrames || generation == 0 ||
            (channels != 1 && channels != 2) || (channels == 2 && right == nullptr))
            return false;

        if (generation != inputGeneration)
        {
            inputFrames = 0;
            inputGeneration = generation;
        }

        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const float sample = channels == 1 ? left[i] : (left[i] + right[i]) * 0.5f;
            if (!isFinite(sample))
            {
                inputFrames = 0;
                invalidInputBlocks.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            inputAccumulator[inputFrames++] = sample;
            if (inputFrames == kBlockFrames)
            {
                if (!enqueueInput(inputAccumulator.data(), nextSequence, inputGeneration))
                    droppedInputBlocks.fetch_add(1, std::memory_order_relaxed);
                ++nextSequence;
                inputFrames = 0;
            }
        }
        return true;
    }

    // Worker-thread consumer. Copies one complete, ordered mono block.
    bool tryPopInputBlock(InputBlock& destination) noexcept
    {
        const auto read = inputRead.load(std::memory_order_relaxed);
        const auto write = inputWrite.load(std::memory_order_acquire);
        if (read == write)
            return false;

        const auto& source = inputQueue[read % kQueueCapacity];
        destination.sequence = source.sequence;
        destination.generation = source.generation;
        destination.samples = source.samples;
        inputRead.store(read + 1, std::memory_order_release);
        return true;
    }

    // Worker-thread producer. Output must correspond to one complete RVC block.
    bool publishConvertedBlock(const float* samples, std::uint32_t frames,
                               std::uint64_t sequence, std::uint64_t generation) noexcept
    {
        if (samples == nullptr || frames != kBlockFrames ||
            sequence == 0 || generation == 0)
            return false;

        const auto write = outputWrite.load(std::memory_order_relaxed);
        const auto read = outputRead.load(std::memory_order_acquire);
        if (write - read >= kQueueCapacity)
        {
            droppedOutputBlocks.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        auto& destination = outputQueue[write % kQueueCapacity];
        for (std::uint32_t i = 0; i < kBlockFrames; ++i)
        {
            if (!isFinite(samples[i]))
                return false;
        }
        destination.sequence = sequence;
        destination.generation = generation;
        for (std::uint32_t i = 0; i < kBlockFrames; ++i)
            destination.samples[i] = samples[i];
        outputWrite.store(write + 1, std::memory_order_release);
        return true;
    }

    // Audio-thread consumer. Returns only samples from the requested stream
    // generation, retaining a partially consumed block across host callbacks.
    // A missing block is an underflow; caller must keep DSP/dry fallback.
    OutputRead pullConvertedOutput(float* destination, std::uint32_t frames,
                                   std::uint64_t generation) noexcept
    {
        OutputRead result{};
        if (destination == nullptr || frames == 0 || generation == 0)
            return result;

        while (result.frames < frames)
        {
            if (outputBlockActive && activeOutputGeneration != generation)
            {
                const auto staleRead = outputRead.load(std::memory_order_relaxed);
                outputRead.store(staleRead + 1, std::memory_order_release);
                outputBlockActive = false;
                outputOffset = 0;
                activeOutputSequence = 0;
                activeOutputGeneration = 0;
                activeOutputGeneration = 0;
                discardedStaleOutputBlocks.fetch_add(1, std::memory_order_relaxed);
            }

            if (!outputBlockActive)
            {
                const auto read = outputRead.load(std::memory_order_relaxed);
                const auto write = outputWrite.load(std::memory_order_acquire);
                if (read == write)
                    break;

                const auto& candidate = outputQueue[read % kQueueCapacity];
                if (candidate.generation != generation)
                {
                    outputRead.store(read + 1, std::memory_order_release);
                    discardedStaleOutputBlocks.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                outputBlockActive = true;
                outputOffset = 0;
                activeOutputSequence = candidate.sequence;
                activeOutputGeneration = candidate.generation;
            }

            const auto read = outputRead.load(std::memory_order_relaxed);
            const auto& block = outputQueue[read % kQueueCapacity];
            if (result.frames == 0)
                result.sequence = activeOutputSequence;

            const auto available = kBlockFrames - outputOffset;
            const auto requested = frames - result.frames;
            const auto count = available < requested ? available : requested;
            for (std::uint32_t i = 0; i < count; ++i)
                destination[result.frames + i] = block.samples[outputOffset + i];

            result.frames += count;
            outputOffset += count;
            if (outputOffset == kBlockFrames)
            {
                outputRead.store(read + 1, std::memory_order_release);
                outputBlockActive = false;
                outputOffset = 0;
                activeOutputSequence = 0;
            }
        }
        return result;
    }

    std::uint32_t pendingInputBlocks() const noexcept
    {
        return inputWrite.load(std::memory_order_acquire) -
               inputRead.load(std::memory_order_acquire);
    }

    std::uint32_t pendingOutputBlocks() const noexcept
    {
        return outputWrite.load(std::memory_order_acquire) -
               outputRead.load(std::memory_order_acquire);
    }

    std::uint64_t droppedInputBlockCount() const noexcept { return droppedInputBlocks; }
    std::uint64_t invalidInputBlockCount() const noexcept { return invalidInputBlocks; }
    std::uint64_t droppedOutputBlockCount() const noexcept { return droppedOutputBlocks; }
    std::uint64_t discardedStaleOutputBlockCount() const noexcept { return discardedStaleOutputBlocks; }

    static constexpr bool atomicsAreAlwaysLockFree() noexcept
    {
        return std::atomic<std::uint32_t>::is_always_lock_free &&
               std::atomic<std::uint64_t>::is_always_lock_free;
    }

private:
    struct QueuedBlock
    {
        std::uint64_t sequence = 0;
        std::uint64_t generation = 0;
        std::array<float, kBlockFrames> samples{};
    };

    static bool isFinite(float value) noexcept
    {
        return value <= (std::numeric_limits<float>::max)() &&
               value >= -(std::numeric_limits<float>::max)();
    }

    bool enqueueInput(const float* samples, std::uint64_t sequence,
                      std::uint64_t generation) noexcept
    {
        const auto write = inputWrite.load(std::memory_order_relaxed);
        const auto read = inputRead.load(std::memory_order_acquire);
        if (write - read >= kQueueCapacity)
            return false;

        auto& destination = inputQueue[write % kQueueCapacity];
        destination.sequence = sequence;
        destination.generation = generation;
        for (std::uint32_t i = 0; i < kBlockFrames; ++i)
            destination.samples[i] = samples[i];
        inputWrite.store(write + 1, std::memory_order_release);
        return true;
    }

    std::array<float, kBlockFrames> inputAccumulator{};
    std::array<QueuedBlock, kQueueCapacity> inputQueue{};
    std::array<QueuedBlock, kQueueCapacity> outputQueue{};
    std::atomic<std::uint32_t> inputWrite{0}, inputRead{0};
    std::atomic<std::uint32_t> outputWrite{0}, outputRead{0};
    std::uint32_t inputFrames = 0;
    std::uint32_t outputOffset = 0;
    std::uint64_t inputGeneration = 0;
    std::uint64_t nextSequence = 1;
    std::uint64_t activeOutputSequence = 0;
    std::uint64_t activeOutputGeneration = 0;
    bool outputBlockActive = false;
    std::atomic<std::uint64_t> droppedInputBlocks{0};
    std::atomic<std::uint64_t> invalidInputBlocks{0};
    std::atomic<std::uint64_t> droppedOutputBlocks{0};
    std::atomic<std::uint64_t> discardedStaleOutputBlocks{0};
};

static_assert(RvcAudioStreamAdapter::atomicsAreAlwaysLockFree(),
              "RVC audio stream queues require lock-free 32-bit atomics");
} // namespace voice_studio::ipc
