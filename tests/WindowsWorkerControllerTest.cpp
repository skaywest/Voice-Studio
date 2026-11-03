#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "AudioIpc/WindowsWorkerController.h"

#include <string>

using namespace voice_studio::ipc;

static std::uint64_t monotonicNs() noexcept
{
    return static_cast<std::uint64_t>(GetTickCount64()) * 1'000'000ull;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc != 2)
        return 1;

    const std::wstring executable = argv[1];
    const std::wstring pipeName = L"\\\\.\\pipe\\VoiceStudioControllerTest-" +
                                  std::to_wstring(GetCurrentProcessId());

    WindowsWorkerController controller;
    if (!controller.launch(executable, pipeName, monotonicNs(), 5000))
        return 2;
    if (controller.currentState() != WorkerLifecycleState::ready ||
        controller.audioConversionAvailable())
        return 3;
    if (!controller.audioRealtimeAccess().isAttached())
        return 6;
    {
        auto audioLease = controller.audioRealtimeAccess().tryAcquire();
        if (!audioLease || audioLease.get() == nullptr)
            return 7;
    }

    if (!controller.enableAi(3000))
        return 4;

    // The protocol Worker explicitly reports "unsupported" for AI start.
    // The controller must fail closed and terminate that Worker generation.
    bool receivedFailure = false;
    for (int i = 0; i < 20; ++i)
    {
        if (!controller.pollStatus(250))
        {
            receivedFailure = controller.currentState() == WorkerLifecycleState::faulted;
            if (receivedFailure)
                break;
        }
    }
    if (!receivedFailure || controller.audioConversionAvailable())
        return 5;
    if (controller.audioRealtimeAccess().isAttached())
        return 8;

    controller.shutdown();
    if (controller.audioRealtimeAccess().isAttached())
        return 9;
    return 0;
}
