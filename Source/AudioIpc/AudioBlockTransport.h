#pragma once

#include "AudioBlockSlotSync.h"

#include <cstddef>
#include <cstdint>
#include <limits>

namespace voice_studio::ipc
{
// These helpers are bounded and non-waiting. The input producer is the plugin
// audio thread; the output consumer is the plugin audio thread. The Worker is
// the opposite endpoint. Samples are interleaved Float32 in shared memory.
inline bool tryPublishInputAudioBlock(AudioBlockRegion& region,
                                      const float* interleavedSamples,
                                      std::uint32_t frames,
                                      std::uint32_t channels,
                                      std::uint32_t sampleRate,
                                      std::uint64_t sequence,
                                      std::uint64_t generation,
                                      std::uint64_t deadlineMonotonicNs) noexcept
{
    if (interleavedSamples == nullptr ||
        !hasValidAudioBlockHeader(region.header) ||
        frames == 0 || frames > kAudioBlockMaxFrames ||
        channels == 0 || channels > kAudioBlockMaxChannels ||
        sampleRate < 8000 || sampleRate > 192000 ||
        sequence == 0 || generation == 0 || deadlineMonotonicNs == 0)
        return false;

    const auto sampleCount = frames * channels;
    for (std::uint32_t i = 0; i < sampleCount; ++i)
    {
        const float sample = interleavedSamples[i];
        if (!(sample <= (std::numeric_limits<float>::max)() && sample >= -(std::numeric_limits<float>::max)()))
            return false;
    }

    for (std::uint32_t i = kAudioBlockInputSlotBegin;
         i < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++i)
    {
        auto& slot = region.slots[i];
        if (!tryBeginAudioBlockWrite(slot))
            continue;

        slot.flags = 0;
        slot.sequence = sequence;
        slot.generation = generation;
        slot.deadlineMonotonicNs = deadlineMonotonicNs;
        slot.sampleRate = sampleRate;
        slot.channels = channels;
        slot.frames = frames;
        slot.payloadSamples = sampleCount;
        for (std::uint32_t sample = 0; sample < sampleCount; ++sample)
            slot.samples[sample] = interleavedSamples[sample];

        return publishAudioBlockWrite(slot);
    }
    return false;
}

// Returns false when no fresh matching output is available. Invalid, stale,
// or expired output blocks are released and skipped. The caller should retain
// its dry input when this returns false; this function never waits.
inline bool tryConsumeOutputAudioBlock(AudioBlockRegion& region,
                                       float* interleavedDestination,
                                       std::uint32_t destinationCapacitySamples,
                                       std::uint32_t expectedFrames,
                                       std::uint32_t expectedChannels,
                                       std::uint32_t expectedSampleRate,
                                       std::uint64_t expectedGeneration,
                                       std::uint64_t nowMonotonicNs) noexcept
{
    if (interleavedDestination == nullptr ||
        !hasValidAudioBlockHeader(region.header) ||
        expectedFrames == 0 || expectedFrames > kAudioBlockMaxFrames ||
        expectedChannels == 0 || expectedChannels > kAudioBlockMaxChannels ||
        expectedSampleRate < 8000 || expectedSampleRate > 192000 ||
        expectedGeneration == 0 || nowMonotonicNs == 0)
        return false;

    const auto expectedSamples = expectedFrames * expectedChannels;
    if (destinationCapacitySamples < expectedSamples)
        return false;

    for (std::uint32_t i = kAudioBlockOutputSlotBegin;
         i < kAudioBlockOutputSlotBegin + kAudioBlockOutputSlotCount; ++i)
    {
        auto& slot = region.slots[i];
        if (!tryBeginAudioBlockRead(slot))
            continue;

        const bool usable = hasFiniteAudioBlockPayload(slot) &&
            slot.generation == expectedGeneration &&
            slot.deadlineMonotonicNs > nowMonotonicNs &&
            slot.frames == expectedFrames &&
            slot.channels == expectedChannels &&
            slot.sampleRate == expectedSampleRate;

        if (usable)
        {
            for (std::uint32_t sample = 0; sample < expectedSamples; ++sample)
                interleavedDestination[sample] = slot.samples[sample];
        }

        const bool released = finishAudioBlockRead(slot);
        if (usable && released)
            return true;
        if (!released)
            return false;
    }
    return false;
}
} // namespace voice_studio::ipc
