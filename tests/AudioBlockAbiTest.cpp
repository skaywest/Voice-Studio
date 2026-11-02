#include "AudioIpc/AudioBlockAbi.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace
{
void check(bool condition, const char* expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "CHECK failed at line %d: %s\n", line, expression);
        std::abort();
    }
}
}
#define CHECK(expression) check((expression), #expression, __LINE__)

using namespace voice_studio::ipc;

AudioBlockRegionHeader validHeader()
{
    return { kAudioBlockMagic, kAudioBlockAbiVersion,
             static_cast<std::uint32_t>(sizeof(AudioBlockRegionHeader)),
             kAudioBlockSlotCount, static_cast<std::uint32_t>(sizeof(AudioBlockSlot)),
             kAudioBlockMaxFrames, kAudioBlockMaxChannels, kAudioBlockFloat32, 0 };
}

AudioBlockSlot validSlot()
{
    AudioBlockSlot slot {};
    slot.state = static_cast<std::int32_t>(AudioBlockSlotState::ready);
    slot.sequence = 1;
    slot.generation = 1;
    slot.deadlineMonotonicNs = 100;
    slot.sampleRate = 48000;
    slot.channels = 2;
    slot.frames = 960;
    slot.payloadSamples = slot.frames * slot.channels;
    return slot;
}

void testHeaderValidation()
{
    auto header = validHeader();
    CHECK(hasValidAudioBlockHeader(header));
    header.version++;
    CHECK(!hasValidAudioBlockHeader(header));
    header = validHeader(); header.slotSize++;
    CHECK(!hasValidAudioBlockHeader(header));
    header = validHeader(); header.reserved = 1;
    CHECK(!hasValidAudioBlockHeader(header));
}

void testSupportedBlockShapes()
{
    constexpr std::uint32_t supportedFrames[] = { 960u, 1920u, 2880u };
    for (const auto frames : supportedFrames)
    {
        auto slot = validSlot();
        slot.frames = frames;
        slot.payloadSamples = frames * slot.channels;
        CHECK(hasValidAudioBlockMetadata(slot));
    }
    auto mono = validSlot();
    mono.channels = 1;
    mono.payloadSamples = mono.frames;
    CHECK(hasValidAudioBlockMetadata(mono));
}

void testRejectsMalformedMetadata()
{
    auto slot = validSlot(); slot.sequence = 0;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.generation = 0;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.deadlineMonotonicNs = 0;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.sampleRate = 384000;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.channels = 3;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.frames = kAudioBlockMaxFrames + 1;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.payloadSamples--;
    CHECK(!hasValidAudioBlockMetadata(slot));
    slot = validSlot(); slot.state = static_cast<std::int32_t>(AudioBlockSlotState::writing);
    CHECK(!hasValidAudioBlockMetadata(slot));
}

void testRejectsNonFiniteSamples()
{
    auto slot = validSlot();
    CHECK(hasFiniteAudioBlockPayload(slot));
    slot.state = static_cast<std::int32_t>(AudioBlockSlotState::reading);
    CHECK(hasFiniteAudioBlockPayload(slot));
    slot.samples[0] = std::numeric_limits<float>::quiet_NaN();
    CHECK(!hasFiniteAudioBlockPayload(slot));
    slot = validSlot();
    slot.state = static_cast<std::int32_t>(AudioBlockSlotState::reading);
    slot.samples[0] = std::numeric_limits<float>::infinity();
    CHECK(!hasFiniteAudioBlockPayload(slot));
}

int main()
{
    testHeaderValidation();
    testSupportedBlockShapes();
    testRejectsMalformedMetadata();
    testRejectsNonFiniteSamples();
    return 0;
}
