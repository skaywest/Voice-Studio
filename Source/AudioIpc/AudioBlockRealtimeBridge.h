#pragma once

#include "AudioBlockRealtimeAccess.h"
#include "AudioBlockTransport.h"

#include <array>
#include <cstdint>

namespace voice_studio::ipc
{
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

    // One instance per processor. Call only from the audio thread. Input is
    // snapshotted before touching output because JUCE commonly aliases them.
    // Publish current input before consuming prior output so in-place buffers
    // cannot accidentally feed converted output back into the Worker.
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

        // Preserve original input before writing dry output: input/output may alias.
        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            scratch[frame * channels] = inputLeft[frame];
            if (channels == 2)
                scratch[frame * channels + 1] = inputRight[frame];
        }

        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            outputLeft[frame] = scratch[frame * channels];
            if (channels == 2)
                outputRight[frame] = scratch[frame * channels + 1];
        }

        auto lease = access.tryAcquire();
        if (!lease)
            return result;

        result.mappingAvailable = true;
        auto& region = *lease;

        // Publish this callback's untouched input first. The Worker may return
        // it later; sequence matching preserves future output for its callback.
        result.inputPublished = tryPublishInputAudioBlock(
            region, scratch.data(), frames, channels, sampleRate,
            inputSequence, generation, deadlineMonotonicNs);

        if (expectedOutputSequence != 0 &&
            tryConsumeOutputAudioBlock(region, scratch.data(),
                                       static_cast<std::uint32_t>(scratch.size()),
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

        return result;
    }

private:
    // 60 ms at 48 kHz, stereo Float32. No callback-time allocation.
    std::array<float, kAudioBlockMaxFrames * kAudioBlockMaxChannels> scratch{};
};
} // namespace voice_studio::ipc
