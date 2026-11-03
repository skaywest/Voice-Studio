#pragma once

#include "AudioBlockRealtimeAccess.h"
#include "AudioBlockTransport.h"

#include <array>
#include <cstdint>

namespace voice_studio::ipc
{
// One instance per plugin processor. The fixed scratch buffer is allocated as
// part of the processor, never by process(). Call only from the audio thread.
// The caller supplies the output sequence expected for this callback (normally
// the preceding input block); missing/late AI output leaves the dry copy intact.
struct AudioBlockRealtimeResult
{
    bool mappingAvailable = false;
    bool outputApplied = false;
    bool inputPublished = false;
};

class AudioBlockRealtimeBridge final
{
public:
    AudioBlockRealtimeBridge() = default;
    AudioBlockRealtimeBridge(const AudioBlockRealtimeBridge&) = delete;
    AudioBlockRealtimeBridge& operator=(const AudioBlockRealtimeBridge&) = delete;

    AudioBlockRealtimeResult process(AudioBlockRealtimeAccess& access,
                                     const float* inputLeft,
                                     const float* inputRight,
                                     float* outputLeft,
                                     float* outputRight,
                                     std::uint32_t frames,
                                     std::uint32_t channels,
                                     std::uint32_t sampleRate,
                                     std::uint64_t inputSequence,
                                     std::uint64_t expectedOutputSequence,
                                     std::uint64_t generation,
                                     std::uint64_t deadlineMonotonicNs,
                                     std::uint64_t nowMonotonicNs) noexcept
    {
        AudioBlockRealtimeResult result{};
        if (inputLeft == nullptr || outputLeft == nullptr ||
            (channels == 2 && (inputRight == nullptr || outputRight == nullptr)) ||
            (channels != 1 && channels != 2) || frames == 0 ||
            frames > kAudioBlockMaxFrames)
            return result;

        // Fail-safe first: every invalid/missing/late Worker result leaves dry audio.
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            outputLeft[frame] = inputLeft[frame];
            if (channels == 2)
                outputRight[frame] = inputRight[frame];
        }

        auto lease = access.tryAcquire();
        if (!lease)
            return result;

        result.mappingAvailable = true;
        auto& region = *lease;

        if (expectedOutputSequence != 0 &&
            tryConsumeOutputAudioBlock(region, scratch.data(), scratch.size(),
                                       frames, channels, sampleRate,
                                       expectedOutputSequence, generation,
                                       nowMonotonicNs))
        {
            for (std::uint32_t frame = 0; frame < frames; ++frame)
            {
                outputLeft[frame] = scratch[frame * channels];
                if (channels == 2)
                    outputRight[frame] = scratch[frame * channels + 1];
            }
            result.outputApplied = true;
        }

        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            scratch[frame * channels] = inputLeft[frame];
            if (channels == 2)
                scratch[frame * channels + 1] = inputRight[frame];
        }

        result.inputPublished = tryPublishInputAudioBlock(
            region, scratch.data(), frames, channels, sampleRate,
            inputSequence, generation, deadlineMonotonicNs);
        return result;
    }

private:
    // 60 ms at 48 kHz, stereo Float32. No callback-time allocation.
    std::array<float, kAudioBlockMaxFrames * kAudioBlockMaxChannels> scratch{};
};
} // namespace voice_studio::ipc
