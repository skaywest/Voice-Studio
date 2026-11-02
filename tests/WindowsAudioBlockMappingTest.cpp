#include "AudioIpc/WindowsAudioBlockMapping.h"

#include <cmath>
#include <cstdint>
#include <string>

using namespace voice_studio::ipc;

int main()
{
    const std::wstring name = L"Local\\VoiceStudioAudioMappingTest-" +
                              std::to_wstring(GetCurrentProcessId());

    WindowsAudioBlockMapping owner;
    if (!owner.createServer(name) || owner.get() == nullptr || !owner.isOwner())
        return 1;

    WindowsAudioBlockMapping client;
    if (!client.openClient(name) || client.get() == nullptr || client.isOwner())
        return 2;

    if (!hasValidAudioBlockHeader(client.get()->header))
        return 3;

    auto& producerSlot = owner.get()->slots[0];
    if (!tryBeginAudioBlockWrite(producerSlot))
        return 4;

    producerSlot.flags = 0;
    producerSlot.sequence = 1;
    producerSlot.generation = 7;
    producerSlot.deadlineMonotonicNs = 123456789;
    producerSlot.sampleRate = 48000;
    producerSlot.channels = 1;
    producerSlot.frames = 4;
    producerSlot.payloadSamples = 4;
    producerSlot.samples[0] = -0.5f;
    producerSlot.samples[1] = 0.0f;
    producerSlot.samples[2] = 0.25f;
    producerSlot.samples[3] = 0.75f;

    if (!publishAudioBlockWrite(producerSlot))
        return 5;

    auto& consumerSlot = client.get()->slots[0];
    if (!tryBeginAudioBlockRead(consumerSlot))
        return 6;

    const bool valid = consumerSlot.sequence == 1 &&
                       consumerSlot.generation == 7 &&
                       consumerSlot.frames == 4 &&
                       hasFiniteAudioBlockPayload(consumerSlot) &&
                       consumerSlot.samples[0] == -0.5f &&
                       consumerSlot.samples[3] == 0.75f;
    if (!finishAudioBlockRead(consumerSlot) || !valid)
        return 7;

    if (static_cast<AudioBlockSlotState>(InterlockedCompareExchange(
            reinterpret_cast<volatile LONG*>(&producerSlot.state), 0, 0)) !=
        AudioBlockSlotState::free)
        return 8;

    client.close();
    owner.close();

    WindowsAudioBlockMapping invalidClient;
    if (invalidClient.openClient(name))
        return 9;

    return 0;
}
