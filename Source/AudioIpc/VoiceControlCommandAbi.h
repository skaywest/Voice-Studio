#pragma once

#include <cstddef>
#include <cstdint>

namespace voice_studio::control
{
// Stable identifiers are part of the external-control contract. Never renumber
// or reuse a value; append new identifiers and keep old meanings unchanged.
enum class VoicePresetId : std::uint32_t
{
    original = 0,
    robot = 1,
    recurrent = 2,
    radioOperator = 3,
    deepMale = 4,
    female = 5,
    demon = 6,
    anime = 7
};

constexpr const char* voicePresetName(VoicePresetId id) noexcept
{
    switch (id)
    {
        case VoicePresetId::original: return "original";
        case VoicePresetId::robot: return "robot";
        case VoicePresetId::recurrent: return "recurrent";
        case VoicePresetId::radioOperator: return "radio_operator";
        case VoicePresetId::deepMale: return "deep_male";
        case VoicePresetId::female: return "female";
        case VoicePresetId::demon: return "demon";
        case VoicePresetId::anime: return "anime";
    }
    return "unknown";
}

constexpr bool isValidVoicePresetId(std::uint32_t value) noexcept
{
    return value <= static_cast<std::uint32_t>(VoicePresetId::anime);
}

enum class VoiceControlCommandType : std::uint32_t
{
    selectPreset = 1,
    setDspEnabled = 2,
    setMuted = 3,
    setOutputGainDb = 4,
    setAiEnabled = 5
};

constexpr std::uint32_t kVoiceControlMagic = 0x4d435356u; // "VSCM"
constexpr std::uint16_t kVoiceControlVersion = 1;
constexpr std::uint16_t kVoiceControlCommandSize = 40;
constexpr float kMinimumOutputGainDb = -18.0f;
constexpr float kMaximumOutputGainDb = 12.0f;

// Fixed-size local-control message. Intended for a future owner-only Windows
// named-pipe endpoint; never read or written by the audio thread.
struct VoiceControlCommand final
{
    std::uint32_t magic = kVoiceControlMagic;
    std::uint16_t version = kVoiceControlVersion;
    std::uint16_t size = kVoiceControlCommandSize;
    std::uint64_t requestId = 0;
    std::uint32_t command = 0;
    std::uint32_t presetId = 0;
    float value = 0.0f;
    std::uint32_t flags = 0;
    std::uint32_t reserved = 0;
};

static_assert(sizeof(VoiceControlCommand) == kVoiceControlCommandSize,
              "Voice control ABI layout changed");
static_assert(offsetof(VoiceControlCommand, requestId) == 8,
              "Voice control ABI offset changed");
static_assert(offsetof(VoiceControlCommand, command) == 16,
              "Voice control ABI offset changed");

inline bool hasValidVoiceControlCommand(const VoiceControlCommand& message) noexcept
{
    if (message.magic != kVoiceControlMagic ||
        message.version != kVoiceControlVersion ||
        message.size != kVoiceControlCommandSize ||
        message.requestId == 0 ||
        message.flags != 0 ||
        message.reserved != 0)
        return false;

    switch (static_cast<VoiceControlCommandType>(message.command))
    {
        case VoiceControlCommandType::selectPreset:
            return isValidVoicePresetId(message.presetId) && message.value == 0.0f;
        case VoiceControlCommandType::setDspEnabled:
        case VoiceControlCommandType::setMuted:
        case VoiceControlCommandType::setAiEnabled:
            return message.presetId == 0 &&
                   (message.value == 0.0f || message.value == 1.0f);
        case VoiceControlCommandType::setOutputGainDb:
            return message.presetId == 0 &&
                   message.value >= kMinimumOutputGainDb &&
                   message.value <= kMaximumOutputGainDb;
    }
    return false;
}
} // namespace voice_studio::control
