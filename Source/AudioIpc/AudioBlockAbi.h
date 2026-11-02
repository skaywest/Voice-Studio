#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace voice_studio::ipc
{
// Production audio-block wire format. Synchronization of state must use
// platform inter-process primitives (Windows Interlocked); never use a mutex
// or wait from the plugin audio callback.
inline constexpr std::uint64_t kAudioBlockMagic = 0x5653545544494F32ull;
inline constexpr std::uint32_t kAudioBlockAbiVersion = 1;
inline constexpr std::uint32_t kAudioBlockSlotCount = 8;
inline constexpr std::uint32_t kAudioBlockMaxFrames = 2880; // 60 ms at 48 kHz
inline constexpr std::uint32_t kAudioBlockMaxChannels = 2;
inline constexpr std::uint32_t kAudioBlockFloat32 = 1;
inline constexpr std::uint32_t kAudioBlockKnownFlags = 0;

enum class AudioBlockSlotState : std::int32_t
{
    free = 0,
    writing = 1,
    ready = 2,
    reading = 3
};

struct AudioBlockRegionHeader
{
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t headerSize;
    std::uint32_t slotCount;
    std::uint32_t slotSize;
    std::uint32_t maxFrames;
    std::uint32_t maxChannels;
    std::uint32_t sampleFormat;
    std::uint32_t reserved;
};

struct AudioBlockSlot
{
    alignas(8) volatile std::int32_t state;
    std::uint32_t flags;
    std::uint64_t sequence;
    std::uint64_t generation;
    std::uint64_t deadlineMonotonicNs;
    std::uint32_t sampleRate;
    std::uint32_t channels;
    std::uint32_t frames;
    std::uint32_t payloadSamples;
    float samples[kAudioBlockMaxFrames * kAudioBlockMaxChannels];
};

struct AudioBlockRegion
{
    AudioBlockRegionHeader header;
    AudioBlockSlot slots[kAudioBlockSlotCount];
};

static_assert(sizeof(AudioBlockRegionHeader) == 40, "Audio block header layout is part of the ABI");
static_assert(offsetof(AudioBlockSlot, state) == 0, "Slot state offset is part of the ABI");
static_assert(offsetof(AudioBlockSlot, sequence) == 8, "Slot sequence offset is part of the ABI");
static_assert(offsetof(AudioBlockSlot, generation) == 16, "Slot generation offset is part of the ABI");
static_assert(offsetof(AudioBlockSlot, deadlineMonotonicNs) == 24, "Slot deadline offset is part of the ABI");
static_assert(offsetof(AudioBlockSlot, samples) == 48, "Slot payload offset is part of the ABI");
static_assert(sizeof(AudioBlockSlot) == 48 + sizeof(float) * kAudioBlockMaxFrames * kAudioBlockMaxChannels,
              "Audio block slot size is part of the ABI");
static_assert(offsetof(AudioBlockRegion, slots) == sizeof(AudioBlockRegionHeader),
              "Audio block slots must immediately follow the header");
static_assert(std::is_standard_layout<AudioBlockSlot>::value, "Audio block slot must be standard layout");
static_assert(std::is_trivially_copyable<AudioBlockSlot>::value, "Audio block slot must be trivially copyable");
static_assert(sizeof(AudioBlockSlot) <= (std::numeric_limits<std::uint32_t>::max)(),
              "Audio block slot size must fit the ABI field");

inline bool hasValidAudioBlockHeader(const AudioBlockRegionHeader& header) noexcept
{
    return header.magic == kAudioBlockMagic &&
           header.version == kAudioBlockAbiVersion &&
           header.headerSize == sizeof(AudioBlockRegionHeader) &&
           header.slotCount == kAudioBlockSlotCount &&
           header.slotSize == sizeof(AudioBlockSlot) &&
           header.maxFrames == kAudioBlockMaxFrames &&
           header.maxChannels == kAudioBlockMaxChannels &&
           header.sampleFormat == kAudioBlockFloat32 &&
           header.reserved == 0;
}

inline bool hasValidAudioBlockMetadata(const AudioBlockSlot& slot) noexcept
{
    const auto state = static_cast<AudioBlockSlotState>(slot.state);
    if ((state != AudioBlockSlotState::ready && state != AudioBlockSlotState::reading) ||
        slot.flags != kAudioBlockKnownFlags ||
        slot.sequence == 0 || slot.generation == 0 || slot.deadlineMonotonicNs == 0 ||
        slot.sampleRate < 8000 || slot.sampleRate > 192000 ||
        slot.channels == 0 || slot.channels > kAudioBlockMaxChannels ||
        slot.frames == 0 || slot.frames > kAudioBlockMaxFrames)
        return false;

    return slot.payloadSamples == slot.frames * slot.channels;
}

inline bool hasFiniteAudioBlockPayload(const AudioBlockSlot& slot) noexcept
{
    if (!hasValidAudioBlockMetadata(slot))
        return false;

    for (std::uint32_t i = 0; i < slot.payloadSamples; ++i)
    {
        const float sample = slot.samples[i];
        if (!(sample <= std::numeric_limits<float>::max() &&
              sample >= -std::numeric_limits<float>::max()))
            return false;
    }
    return true;
}
} // namespace voice_studio::ipc
