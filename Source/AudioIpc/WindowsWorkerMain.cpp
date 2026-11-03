#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "AudioIpc/WindowsAudioBlockMapping.h"
#include "AudioIpc/WindowsWorkerControlPipe.h"
#include "AudioIpc/WorkerModelFileVerifier.h"

#include <cerrno>
#include <cstdint>
#include <string>

using namespace voice_studio::ipc;

namespace
{
std::uint64_t monotonicNs() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64()) * 1'000'000ull;
}

bool parseUint64(const wchar_t* value, std::uint64_t& output) noexcept
{
    if (value == nullptr || *value == L'\0')
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = _wcstoui64(value, &end, 10);
    if (errno != 0 || end == value || *end != L'\0' || parsed == 0)
        return false;
    output = static_cast<std::uint64_t>(parsed);
    return true;
}

WorkerStatusMessage makeStatus(WorkerState state, WorkerResult result,
                               std::uint64_t generation, std::uint64_t requestId) noexcept
{
    WorkerStatusMessage status{};
    status.magic = kWorkerControlMagic;
    status.version = kWorkerControlVersion;
    status.size = sizeof(status);
    status.state = static_cast<std::uint16_t>(state);
    status.result = static_cast<std::uint32_t>(result);
    status.generation = generation;
    status.requestId = requestId;
    status.heartbeatMonotonicNs = monotonicNs();
    return status;
}

// Transport-only echo path used to prove cross-process audio ownership and
// payload visibility. This is deliberately not voice conversion or AI.
void transferAvailableAudioBlocks(AudioBlockRegion& region, std::uint64_t generation, std::uint64_t nowNs) noexcept
{
    for (std::uint32_t inputIndex = kAudioBlockInputSlotBegin;
         inputIndex < kAudioBlockInputSlotBegin + kAudioBlockInputSlotCount; ++inputIndex)
    {
        auto& input = region.slots[inputIndex];
        if (!tryBeginAudioBlockRead(input))
            continue;
        if (!hasFiniteAudioBlockPayload(input) || input.generation != generation || input.deadlineMonotonicNs <= nowNs)
        {
            finishAudioBlockRead(input);
            continue;
        }
        AudioBlockSlot* output = nullptr;
        for (std::uint32_t outputIndex = kAudioBlockOutputSlotBegin;
             outputIndex < kAudioBlockOutputSlotBegin + kAudioBlockOutputSlotCount; ++outputIndex)
        {
            auto& candidate = region.slots[outputIndex];
            if (tryBeginAudioBlockWrite(candidate))
            {
                output = &candidate;
                break;
            }
        }
        if (output != nullptr)
        {
            output->flags = input.flags;
            output->sequence = input.sequence;
            output->generation = input.generation;
            output->deadlineMonotonicNs = input.deadlineMonotonicNs;
            output->sampleRate = input.sampleRate;
            output->channels = input.channels;
            output->frames = input.frames;
            output->payloadSamples = input.payloadSamples;
            for (std::uint32_t sample = 0; sample < input.payloadSamples; ++sample)
                output->samples[sample] = input.samples[sample];
            publishAudioBlockWrite(*output);
        }
        finishAudioBlockRead(input);
    }
}

int runWorker(const std::wstring& pipeName, const std::wstring& mappingName,
              std::uint64_t generation, std::uint64_t requestId) noexcept
{
    WindowsAudioBlockMapping audioMapping;
    if (!audioMapping.openClient(mappingName))
        return 9;

    WindowsWorkerControlPipe pipe;
    if (!pipe.connectClient(pipeName, 5000))
        return 10;

    if (!pipe.sendStatus(makeStatus(WorkerState::ready, WorkerResult::ok,
                                    generation, requestId), 3000))
        return 11;

    std::uint64_t activeRequestId = requestId;
    bool modelFilesVerified = false;
    std::uint64_t lastHeartbeatNs = monotonicNs();

    for (;;)
    {
        WorkerCommandMessage command{};
        if (!pipe.receiveCommand(command, 10))
        {
            transferAvailableAudioBlocks(*audioMapping.get(), generation, monotonicNs());
            const auto now = monotonicNs();
            if (now - lastHeartbeatNs >= 500'000'000ull)
            {
                if (!pipe.sendStatus(makeStatus(WorkerState::ready, WorkerResult::ok,
                                                generation, activeRequestId), 1000))
                    return 12;
                lastHeartbeatNs = now;
            }
            continue;
        }

        if (command.generation != generation ||
            command.requestId <= activeRequestId)
            return 13;

        activeRequestId = command.requestId;
        lastHeartbeatNs = monotonicNs();

        const auto operation = static_cast<WorkerCommand>(command.command);
        if (operation == WorkerCommand::configureModel)
        {
            WorkerModelConfigMessage config{};
            if (!pipe.receiveModelConfig(config, command.timeoutMs) ||
                !matchesWorkerModelConfigRequest(config, generation, activeRequestId) ||
                !verifyWorkerModelConfigFiles(config))
            {
                pipe.sendStatus(makeStatus(WorkerState::error, WorkerResult::invalidRequest,
                                           generation, activeRequestId), 1000);
                return 17;
            }

            modelFilesVerified = true;
            if (!pipe.sendStatus(makeStatus(WorkerState::ready, WorkerResult::ok,
                                            generation, activeRequestId), 1000))
                return 18;
            continue;
        }

        if (operation == WorkerCommand::start)
        {
            // File verification is not inference. Keep AI unavailable until a
            // real, audited RVC backend is present, even after a valid manifest.
            (void) modelFilesVerified;
            return pipe.sendStatus(makeStatus(WorkerState::error,
                                              WorkerResult::unsupported,
                                              generation, activeRequestId), 1000)
                ? 20 : 14;
        }

        if (operation == WorkerCommand::stop ||
            operation == WorkerCommand::unloadModel ||
            operation == WorkerCommand::ping)
        {
            if (!pipe.sendStatus(makeStatus(WorkerState::ready, WorkerResult::ok,
                                            generation, activeRequestId), 1000))
                return 15;
            continue;
        }

        return 16;
    }
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 6 || std::wstring(argv[1]) != L"--voice-studio-worker")
        return 2;

    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    if (!parseUint64(argv[4], generation) || !parseUint64(argv[5], requestId))
        return 3;

    return runWorker(argv[2], argv[3], generation, requestId);
}
