#include "AudioIpc/AudioBlockRealtimeBridge.h"

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
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot.state),
                             static_cast<LONG>(AudioBlockSlotState::free));
}

void workerEcho(AudioBlockRegion& region)
{
    AudioBlockSlot* input = nullptr;
    for (std::uint32_t i = kAudioBlockInputSlotBegin;
         i < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++i)
        if (tryBeginAudioBlockRead(region.slots[i])) { input = &region.slots[i]; break; }
    CHECK(input != nullptr);

    AudioBlockSlot* output = nullptr;
    for (std::uint32_t i = kAudioBlockOutputSlotBegin;
         i < kAudioBlockOutputSlotBegin + kAudioBlockOutputSlotCount; ++i)
        if (tryBeginAudioBlockWrite(region.slots[i])) { output = &region.slots[i]; break; }
    CHECK(output != nullptr);

    output->flags = input->flags;
    output->sequence = input->sequence;
    output->generation = input->generation;
    output->deadlineMonotonicNs = input->deadlineMonotonicNs;
    output->sampleRate = input->sampleRate;
    output->channels = input->channels;
    output->frames = input->frames;
    output->payloadSamples = input->payloadSamples;
    for (std::uint32_t i = 0; i < input->payloadSamples; ++i)
        output->samples[i] = input->samples[i];

    CHECK(publishAudioBlockWrite(*output));
    CHECK(finishAudioBlockRead(*input));
}
}

int main()
{
    constexpr std::uint32_t frames = 128;
    AudioBlockRegion region{};
    initialise(region);
    AudioBlockRealtimeAccess access;
    AudioBlockRealtimeBridge bridge;
    CHECK(access.attach(&region));

    float inL[frames]{}, inR[frames]{}, outL[frames]{}, outR[frames]{};
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        inL[i] = 0.1f + static_cast<float>(i) * 0.001f;
        inR[i] = -inL[i];
    }

    // First callback has no prior AI output: it must stay dry and publish input.
    auto first = bridge.process(access, inL, inR, outL, outR, frames, 2, 48000,
                                1, 0, 7, 1000000000, 900000000);
    CHECK(first.mappingAvailable);
    CHECK(!first.outputApplied);
    CHECK(first.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        CHECK(outL[i] == inL[i]);
        CHECK(outR[i] == inR[i]);
    }

    // Stand in for the separate Worker: consume input block 1 and publish output 1.
    workerEcho(region);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        inL[i] *= 0.5f;
        inR[i] *= 0.5f;
    }

    auto second = bridge.process(access, inL, inR, outL, outR, frames, 2, 48000,
                                 2, 1, 7, 1100000000, 950000000);
    CHECK(second.mappingAvailable);
    CHECK(second.outputApplied);
    CHECK(second.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        const float expected = 0.1f + static_cast<float>(i) * 0.001f;
        CHECK(std::abs(outL[i] - expected) < 0.000001f);
        CHECK(std::abs(outR[i] + expected) < 0.000001f);
    }

    // Expired output must not replace the dry fallback.
    workerEcho(region);
    auto third = bridge.process(access, inL, inR, outL, outR, frames, 2, 48000,
                                3, 2, 7, 1200000000, 1200000000);
    CHECK(!third.outputApplied);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        CHECK(outL[i] == inL[i]);
        CHECK(outR[i] == inR[i]);
    }

    CHECK(access.detach() == &region);
    auto detached = bridge.process(access, inL, inR, outL, outR, frames, 2, 48000,
                                   4, 3, 7, 1300000000, 1250000000);
    CHECK(!detached.mappingAvailable);
    CHECK(!detached.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        CHECK(outL[i] == inL[i]);
        CHECK(outR[i] == inR[i]);
    }
    return 0;
}
