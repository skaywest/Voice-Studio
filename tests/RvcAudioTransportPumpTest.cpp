#include "AudioIpc/RvcAudioTransportPump.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "CHECK failed: %s at %d\\n", #expression, __LINE__); std::abort(); } } while (false)
using namespace voice_studio::ipc;

namespace
{
void initialise(AudioBlockRegion& region)
{
    region.header.magic = kAudioBlockMagic;
    region.header.version = kAudioBlockAbiVersion;
    region.header.headerSize = sizeof(AudioBlockRegionHeader);
    region.header.slotCount = kAudioBlockSlotCount;
    region.header.slotSize = sizeof(AudioBlockSlot);
    region.header.maxFrames = kAudioBlockMaxFrames;
    region.header.maxChannels = kAudioBlockMaxChannels;
    region.header.sampleFormat = kAudioBlockFloat32;
    region.header.reserved = 0;
    for (auto& slot : region.slots)
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot.state), static_cast<LONG>(AudioBlockSlotState::free));
}
void workerEchoOne(AudioBlockRegion& region)
{
    AudioBlockSlot* input = nullptr;
    for (std::uint32_t i = kAudioBlockInputSlotBegin; i < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++i)
        if (tryBeginAudioBlockRead(region.slots[i])) { input = &region.slots[i]; break; }
    CHECK(input != nullptr && hasFiniteAudioBlockPayload(*input));
    AudioBlockSlot* output = nullptr;
    for (std::uint32_t i = kAudioBlockOutputSlotBegin; i < kAudioBlockOutputSlotBegin + kAudioBlockOutputSlotCount; ++i)
        if (tryBeginAudioBlockWrite(region.slots[i])) { output = &region.slots[i]; break; }
    CHECK(output != nullptr);
    output->flags = input->flags; output->sequence = input->sequence;
    output->generation = input->generation; output->deadlineMonotonicNs = input->deadlineMonotonicNs;
    output->sampleRate = input->sampleRate; output->channels = input->channels;
    output->frames = input->frames; output->payloadSamples = input->payloadSamples;
    for (std::uint32_t i = 0; i < input->payloadSamples; ++i) output->samples[i] = input->samples[i] * 0.5f;
    CHECK(publishAudioBlockWrite(*output)); CHECK(finishAudioBlockRead(*input));
}
}
int main()
{
    AudioBlockRegion region{}; initialise(region);
    RvcAudioStreamAdapter adapter; RvcAudioTransportPump pump(adapter);
    std::array<float, RvcAudioStreamAdapter::kBlockFrames> input{}, output{};
    for (std::uint32_t i = 0; i < input.size(); ++i) input[i] = 0.2f + static_cast<float>(i) * 0.00001f;
    CHECK(adapter.pushHostInput(input.data(), nullptr, RvcAudioStreamAdapter::kBlockFrames, 1, 7));
    CHECK(pump.publishPendingInput(region, 1'000'000'000ull) == 1);
    CHECK(pump.pendingWorkerBlocks() == 1);
    workerEchoOne(region);
    CHECK(pump.collectReadyOutput(region, 1'010'000'000ull) == 1);
    CHECK(pump.pendingWorkerBlocks() == 0);
    const auto read = adapter.pullConvertedOutput(output.data(), RvcAudioStreamAdapter::kBlockFrames, 7);
    CHECK(read.frames == RvcAudioStreamAdapter::kBlockFrames && read.sequence == 1);
    for (std::uint32_t i = 0; i < output.size(); ++i) CHECK(std::abs(output[i] - input[i] * 0.5f) < 0.000001f);

    CHECK(adapter.pushHostInput(input.data(), nullptr, RvcAudioStreamAdapter::kBlockFrames, 1, 7));
    CHECK(pump.publishPendingInput(region, 2'000'000'000ull, 10'000'000ull) == 1);
    CHECK(pump.collectReadyOutput(region, 2'020'000'000ull) == 0);
    CHECK(pump.pendingWorkerBlocks() == 0 && pump.expiredOutputBlockCount() == 1);
    pump.resetForNewWorkerGeneration();
    CHECK(pump.pendingWorkerBlocks() == 0);

    // A newly launched Worker generation must never receive queued audio
    // captured by the previous generation.
    AudioBlockRegion staleRegion{}; initialise(staleRegion);
    RvcAudioStreamAdapter staleAdapter;
    RvcAudioTransportPump stalePump(staleAdapter);
    CHECK(staleAdapter.pushHostInput(input.data(), nullptr,
        RvcAudioStreamAdapter::kBlockFrames, 1, 8));
    CHECK(stalePump.publishPendingInput(staleRegion, 3'000'000'000ull,
        RvcAudioTransportPump::kDefaultDeadlineBudgetNs, 9) == 0);
    CHECK(stalePump.pendingWorkerBlocks() == 0);
    CHECK(stalePump.droppedInputBlockCount() == 1);
    for (std::uint32_t i = kAudioBlockInputSlotBegin;
         i < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++i)
        CHECK(staleRegion.slots[i].state == static_cast<LONG>(AudioBlockSlotState::free));
    // A queued block from an earlier AI session is rejected even when
    // the Worker generation is unchanged.
    AudioBlockRegion sessionRegion{}; initialise(sessionRegion);
    RvcAudioStreamAdapter sessionAdapter; RvcAudioTransportPump sessionPump(sessionAdapter);
    CHECK(sessionAdapter.pushHostInput(input.data(), nullptr,
        RvcAudioStreamAdapter::kBlockFrames, 1, 12, 1));
    CHECK(sessionPump.publishPendingInput(sessionRegion, 4'000'000'000ull,
        RvcAudioTransportPump::kDefaultDeadlineBudgetNs, 12, 2) == 0);
    CHECK(sessionPump.droppedInputBlockCount() == 1);
    for (std::uint32_t i = kAudioBlockInputSlotBegin;
         i < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++i)
        CHECK(sessionRegion.slots[i].state == static_cast<LONG>(AudioBlockSlotState::free));

    // A late response is consumed from shared memory to reclaim its slot,
    // but is not forwarded to the new AI session.
    CHECK(sessionAdapter.pushHostInput(input.data(), nullptr,
        RvcAudioStreamAdapter::kBlockFrames, 1, 12, 1));
    CHECK(sessionPump.publishPendingInput(sessionRegion, 5'000'000'000ull,
        RvcAudioTransportPump::kDefaultDeadlineBudgetNs, 12, 1) == 1);
    workerEchoOne(sessionRegion);
    CHECK(sessionPump.collectReadyOutput(sessionRegion, 5'010'000'000ull, 2) == 0);
    CHECK(sessionPump.pendingWorkerBlocks() == 0);
    CHECK(sessionAdapter.pendingOutputBlocks() == 0);
    bool outputSlotsFree = true;
    for (std::uint32_t i = kAudioBlockOutputSlotBegin;
         i < kAudioBlockOutputSlotBegin + kAudioBlockOutputSlotCount; ++i)
        outputSlotsFree = outputSlotsFree &&
            sessionRegion.slots[i].state == static_cast<LONG>(AudioBlockSlotState::free);
    CHECK(outputSlotsFree);
    return 0;
}
