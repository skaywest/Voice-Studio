#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "AudioIpc/AudioBlockSlotSync.h"
#include "AudioIpc/WindowsAudioBlockMapping.h"
#include "AudioIpc/WindowsWorkerControlPipe.h"
#include "AudioIpc/WindowsWorkerProcess.h"

#include <cstdint>
#include <cstdio>
#include <string>

using namespace voice_studio::ipc;

namespace
{
int fail(int code) noexcept
{
    std::fprintf(stderr, "WindowsAudioBlockWorkerProcessTest failed at checkpoint %d\\n", code);
    return code;
}

std::uint64_t monotonicNs() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64()) * 1'000'000ull;
}

WorkerCommandMessage makeCommand(WorkerCommand command, std::uint64_t generation,
                                 std::uint64_t requestId) noexcept
{
    WorkerCommandMessage message{};
    message.magic = kWorkerControlMagic;
    message.version = kWorkerControlVersion;
    message.size = sizeof(message);
    message.command = static_cast<std::uint16_t>(command);
    message.generation = generation;
    message.requestId = requestId;
    message.timeoutMs = 1000;
    return message;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return fail(1);

    const auto suffix = std::to_wstring(GetCurrentProcessId());
    const std::wstring pipeName = L"\\\\.\\pipe\\VoiceStudioAudioWorkerTest-" + suffix;
    const std::wstring mappingName = L"Local\\VoiceStudioAudioWorkerTest-" + suffix;
    constexpr std::uint64_t generation = 77;
    constexpr std::uint64_t initialRequestId = 1;

    WindowsAudioBlockMapping mapping;
    if (!mapping.createServer(mappingName))
        return fail(2);

    WindowsWorkerControlPipe pipe;
    if (!pipe.createServer(pipeName))
        return fail(3);

    WindowsWorkerProcess worker;
    const std::wstring arguments = L"--voice-studio-worker \"" + pipeName +
        L"\" \"" + mappingName + L"\" " + std::to_wstring(generation) +
        L" " + std::to_wstring(initialRequestId);
    if (!worker.launch(argv[1], arguments))
        return fail(4);

    if (!pipe.waitForClient(5000))
    {
        worker.shutdown();
        return fail(5);
    }

    WorkerStatusMessage ready{};
    if (!pipe.receiveStatus(ready, 5000) ||
        ready.state != static_cast<std::uint16_t>(WorkerState::ready) ||
        ready.generation != generation || ready.requestId != initialRequestId)
    {
        worker.shutdown();
        return fail(6);
    }

    auto& input = mapping.get()->slots[kAudioBlockInputSlotBegin];
    if (!tryBeginAudioBlockWrite(input))
    {
        worker.shutdown();
        return fail(7);
    }

    input.flags = 0;
    input.sequence = 42;
    input.generation = generation;
    input.deadlineMonotonicNs = monotonicNs() + 5'000'000'000ull;
    input.sampleRate = 48000;
    input.channels = 2;
    input.frames = 4;
    input.payloadSamples = 8;
    const float samples[8] = {-0.75f, -0.5f, -0.25f, 0.0f, 0.125f, 0.25f, 0.5f, 0.875f};
    for (std::uint32_t i = 0; i < 8; ++i)
        input.samples[i] = samples[i];

    if (!publishAudioBlockWrite(input))
    {
        worker.shutdown();
        return fail(8);
    }

    bool received = false;
    for (int attempt = 0; attempt < 3000 && !received; ++attempt)
    {
        auto& output = mapping.get()->slots[kAudioBlockOutputSlotBegin];
        if (tryBeginAudioBlockRead(output))
        {
            received = output.sequence == 42 &&
                       output.generation == generation &&
                       output.sampleRate == 48000 &&
                       output.channels == 2 &&
                       output.frames == 4 &&
                       hasFiniteAudioBlockPayload(output);
            for (std::uint32_t i = 0; i < 8 && received; ++i)
                received = output.samples[i] == samples[i];
            if (!finishAudioBlockRead(output))
                received = false;
            break;
        }
        Sleep(1);
    }

    if (!received)
    {
        worker.shutdown();
        return fail(9);
    }

    const auto stop = makeCommand(WorkerCommand::stop, generation, 2);
    WorkerStatusMessage stopped{};
    if (!pipe.sendCommand(stop, 1000) ||
        !pipe.receiveStatus(stopped, 3000) ||
        stopped.state != static_cast<std::uint16_t>(WorkerState::ready) ||
        stopped.requestId != 2)
    {
        worker.shutdown();
        return fail(10);
    }

    if (!worker.terminate() || !worker.hasExited())
        return fail(11);
    if (!worker.closeAfterExit())
        return fail(12);

    reclaimAudioBlockSlotsAfterWorkerExit(*mapping.get());
    pipe.close();
    mapping.close();
    return 0;
}
