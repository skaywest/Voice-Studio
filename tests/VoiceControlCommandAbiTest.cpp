#include "AudioIpc/VoiceControlCommandAbi.h"

#include <iostream>
#include <limits>
#include <string>

namespace
{
int failures = 0;
void expect(bool condition, const char* label)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << label << "\n";
        ++failures;
    }
    else
        std::cout << "PASS: " << label << "\n";
}
}

int main()
{
    using namespace voice_studio::control;
    VoiceControlCommand message;
    message.requestId = 1;
    message.command = static_cast<std::uint32_t>(VoiceControlCommandType::selectPreset);
    message.presetId = static_cast<std::uint32_t>(VoicePresetId::robot);
    expect(hasValidVoiceControlCommand(message), "robot preset command validates");
    expect(std::string(voicePresetName(VoicePresetId::recurrent)) == "recurrent",
           "recurrent preset has a stable external identifier");
    expect(std::string(voicePresetName(VoicePresetId::original)) == "original",
           "original preset has a stable external identifier");

    message.presetId = 999;
    expect(!hasValidVoiceControlCommand(message), "unknown preset ID is rejected");
    message.presetId = static_cast<std::uint32_t>(VoicePresetId::robot);
    message.magic = 0;
    expect(!hasValidVoiceControlCommand(message), "bad magic is rejected");
    message.magic = kVoiceControlMagic;
    message.version = 99;
    expect(!hasValidVoiceControlCommand(message), "unsupported protocol version is rejected");
    message.version = kVoiceControlVersion;
    message.requestId = 0;
    expect(!hasValidVoiceControlCommand(message), "zero request ID is rejected");
    message.requestId = 1;

    message.command = static_cast<std::uint32_t>(VoiceControlCommandType::setDspEnabled);
    message.presetId = 0;
    message.value = 1.0f;
    expect(hasValidVoiceControlCommand(message), "DSP enable command validates");
    message.value = 0.5f;
    expect(!hasValidVoiceControlCommand(message), "non-boolean toggle value is rejected");

    message.command = static_cast<std::uint32_t>(VoiceControlCommandType::setOutputGainDb);
    message.value = 6.0f;
    expect(hasValidVoiceControlCommand(message), "valid output gain command validates");
    message.value = std::numeric_limits<float>::quiet_NaN();
    expect(!hasValidVoiceControlCommand(message), "NaN output gain is rejected");
    message.value = 99.0f;
    expect(!hasValidVoiceControlCommand(message), "out-of-range output gain is rejected");

    message.command = 0xffffffffu;
    message.value = 0.0f;
    expect(!hasValidVoiceControlCommand(message), "unknown command is rejected");
    message.command = static_cast<std::uint32_t>(VoiceControlCommandType::setMuted);
    message.flags = 1;
    expect(!hasValidVoiceControlCommand(message), "unknown flags are rejected");

    if (failures != 0)
        return 1;
    std::cout << "All external voice-control ABI tests passed\n";
    return 0;
}
