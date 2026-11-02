#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "AudioIpc/WindowsWorkerControlPipe.h"

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

int runWorker(const std::wstring& pipeName, std::uint64_t generation,
              std::uint64_t requestId) noexcept
{
    WindowsWorkerControlPipe pipe;
    if (!pipe.connectClient(pipeName, 5000))
        return 10;

    if (!pipe.sendStatus(makeStatus(WorkerState::ready, WorkerResult::ok,
                                    generation, requestId), 3000))
        return 11;

    std::uint64_t activeRequestId = requestId;
    std::uint64_t lastHeartbeatNs = monotonicNs();

    for (;;)
    {
        WorkerCommandMessage command{};
        if (!pipe.receiveCommand(command, 250))
        {
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
        if (operation == WorkerCommand::start)
        {
            // Deliberately fail closed: this protocol Worker has no inference
            // engine yet and must never claim that AI conversion is running.
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
    if (argc != 5 || std::wstring(argv[1]) != L"--voice-studio-worker")
        return 2;

    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    if (!parseUint64(argv[3], generation) || !parseUint64(argv[4], requestId))
        return 3;

    return runWorker(argv[2], generation, requestId);
}
