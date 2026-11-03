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
LONG readSlotState(const AudioBlockSlot& slot) noexcept
{
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(const_cast<volatile std::int32_t*>(&slot.state)), 0, 0);
}

int fail(int code) noexcept
{
    std::fprintf(stderr, "WindowsAudioBlockWorkerProcessTest failed at checkpoint %d\n", code);
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
    auto publishProbeBlock = [&](std::uint64_t sequence, std::uint64_t blockGeneration,
                                 std::uint64_t deadlineNs) noexcept
    {
        if (!tryBeginAudioBlockWrite(input))
            return false;
        input.flags = 0;
        input.sequence = sequence;
        input.generation = blockGeneration;
        input.deadlineMonotonicNs = deadlineNs;
        input.sampleRate = 48000;
        input.channels = 2;
        input.frames = 4;
        input.payloadSamples = 8;
        const float probe[8] = {-0.75f, -0.5f, -0.25f, 0.0f, 0.125f, 0.25f, 0.5f, 0.875f};
        for (std::uint32_t i = 0; i < 8; ++i)
            input.samples[i] = probe[i];
        return publishAudioBlockWrite(input);
    };

    // Stale generations and expired deadlines must never reach the output queue.
    if (!publishProbeBlock(40, generation - 1, monotonicNs() + 5'000'000'000ull))
    {
        worker.shutdown();
        return fail(7);
    }
    Sleep(50);
    if (readSlotState(input) != static_cast<LONG>(AudioBlockSlotState::free) ||
        readSlotState(mapping.get()->slots[kAudioBlockOutputSlotBegin]) != static_cast<LONG>(AudioBlockSlotState::free))
    {
        worker.shutdown();
        return fail(8);
    }

    if (!publishProbeBlock(41, generation, monotonicNs() - 1))
    {
        worker.shutdown();
        return fail(9);
    }
    Sleep(50);
    if (input.state != static_cast<LONG>(AudioBlockSlotState::free) ||
        mapping.get()->slots[kAudioBlockOutputSlotBegin].state != static_cast<LONG>(AudioBlockSlotState::free))
    {
        worker.shutdown();
        return fail(10);
    }

    const float samples[8] = {-0.75f, -0.5f, -0.25f, 0.0f, 0.125f, 0.25f, 0.5f, 0.875f};
    if (!publishProbeBlock(42, generation, monotonicNs() + 5'000'000'000ull))
    {
        worker.shutdown();
        return fail(11);
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
        return fail(12);
    }

    const auto stop = makeCommand(WorkerCommand::stop, generation, 2);
    WorkerStatusMessage stopped{};
    if (!pipe.sendCommand(stop, 1000))
    {
        worker.shutdown();
        return fail(13);
    }

    // The Worker may already have queued a periodic READY heartbeat carrying
    // requestId 1. Drain those older statuses and wait for the STOP ack (2).
    bool receivedStopAck = false;
    for (int attempt = 0; attempt < 6 && !receivedStopAck; ++attempt)
    {
        WorkerStatusMessage status{};
        if (!pipe.receiveStatus(status, 1000) ||
            status.generation != generation ||
            status.requestId > 2)
            break;

        if (status.requestId == 2)
        {
            stopped = status;
            receivedStopAck = true;
        }
    }

    if (!receivedStopAck ||
        stopped.state != static_cast<std::uint16_t>(WorkerState::ready) ||
        stopped.result != static_cast<std::uint32_t>(WorkerResult::ok))
    {
        worker.shutdown();
        return fail(13);
    }

    if (!worker.terminate() || !worker.hasExited())
        return fail(14);
    if (!worker.closeAfterExit())
        return fail(15);

    reclaimAudioBlockSlotsAfterWorkerExit(*mapping.get());
    pipe.close();
    mapping.close();
    return 0;
}
