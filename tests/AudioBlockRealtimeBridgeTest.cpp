#include "AudioIpc/AudioBlockRealtimeBridge.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "CHECK failed: %s at %d\n", #expression, __LINE__); std::abort(); } } while (false)

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

void workerEcho(AudioBlockRegion& region, float scale = 1.0f)
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
        output->samples[i] = input->samples[i] * scale;

    CHECK(publishAudioBlockWrite(*output));
    CHECK(finishAudioBlockRead(*input));
}

AudioBlockSlot* findReadyInput(AudioBlockRegion& region, std::uint64_t sequence)
{
    for (std::uint32_t i = kAudioBlockInputSlotBegin;
         i < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++i)
        if (region.slots[i].sequence == sequence &&
            InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&region.slots[i].state),
                                       static_cast<LONG>(AudioBlockSlotState::ready),
                                       static_cast<LONG>(AudioBlockSlotState::ready)) ==
                static_cast<LONG>(AudioBlockSlotState::ready))
            return &region.slots[i];
    return nullptr;
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

    auto first = bridge.process(access, inL, inR, outL, outR, frames, 2, 48000,
                                1, 0, 7, 1000000000, 900000000);
    CHECK(first.mappingAvailable && !first.outputApplied && first.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        CHECK(outL[i] == inL[i]);
        CHECK(outR[i] == inR[i]);
    }

    workerEcho(region);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        inL[i] *= 0.5f;
        inR[i] *= 0.5f;
    }

    auto second = bridge.process(access, inL, inR, outL, outR, frames, 2, 48000,
                                 2, 1, 7, 1100000000, 950000000);
    CHECK(second.mappingAvailable && second.outputApplied && second.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        const float expected = 0.1f + static_cast<float>(i) * 0.001f;
        CHECK(std::abs(outL[i] - expected) < 0.000001f);
        CHECK(std::abs(outR[i] + expected) < 0.000001f);
    }

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
    CHECK(!detached.mappingAvailable && !detached.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        CHECK(outL[i] == inL[i]);
        CHECK(outR[i] == inR[i]);
    }

    // JUCE processBlock commonly uses the same memory for input and output.
    AudioBlockRegion aliasRegion{};
    initialise(aliasRegion);
    AudioBlockRealtimeAccess aliasAccess;
    AudioBlockRealtimeBridge aliasBridge;
    CHECK(aliasAccess.attach(&aliasRegion));
    float inPlaceL[frames]{}, inPlaceR[frames]{};
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        inPlaceL[i] = 0.2f + static_cast<float>(i) * 0.0005f;
        inPlaceR[i] = -inPlaceL[i];
    }
    auto aliasFirst = aliasBridge.process(aliasAccess, inPlaceL, inPlaceR,
                                          inPlaceL, inPlaceR, frames, 2, 48000,
                                          1, 0, 9, 2000000000, 1900000000);
    CHECK(aliasFirst.inputPublished);
    workerEcho(aliasRegion, 0.25f);

    for (std::uint32_t i = 0; i < frames; ++i)
    {
        inPlaceL[i] = 0.8f;
        inPlaceR[i] = -0.8f;
    }
    auto aliasSecond = aliasBridge.process(aliasAccess, inPlaceL, inPlaceR,
                                           inPlaceL, inPlaceR, frames, 2, 48000,
                                           2, 1, 9, 2100000000, 1950000000);
    CHECK(aliasSecond.outputApplied && aliasSecond.inputPublished);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        const float original = 0.2f + static_cast<float>(i) * 0.0005f;
        CHECK(std::abs(inPlaceL[i] - original * 0.25f) < 0.000001f);
        CHECK(std::abs(inPlaceR[i] + original * 0.25f) < 0.000001f);
    }
    auto* publishedCurrentInput = findReadyInput(aliasRegion, 2);
    CHECK(publishedCurrentInput != nullptr);
    for (std::uint32_t i = 0; i < frames; ++i)
    {
        CHECK(publishedCurrentInput->samples[i * 2] == 0.8f);
        CHECK(publishedCurrentInput->samples[i * 2 + 1] == -0.8f);
    }
    CHECK(aliasAccess.detach() == &aliasRegion);
    return 0;
}
