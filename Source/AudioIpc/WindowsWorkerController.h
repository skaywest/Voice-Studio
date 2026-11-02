#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "WindowsWorkerControlPipe.h"
#include "WindowsWorkerProcess.h"
#include "WorkerControlCoordinator.h"

#include <string>

namespace voice_studio::ipc
{
// Owns the real Windows control plane for one Worker generation. Every method
// except audioConversionAvailable() is control-thread-only and may block for
// its explicit timeout. The audio callback must never call this class.
class WindowsWorkerController final
{
public:
    WindowsWorkerController() = default;
    WindowsWorkerController(const WindowsWorkerController&) = delete;
    WindowsWorkerController& operator=(const WindowsWorkerController&) = delete;

    ~WindowsWorkerController() { shutdown(); }

    bool launch(const std::wstring& executablePath,
                const std::wstring& pipeName,
                std::uint64_t nowNs,
                DWORD connectTimeoutMs = 5000) noexcept
    {
        if (active || executablePath.empty() || pipeName.empty())
            return false;

        const auto decision = coordinator.start(nowNs);
        if (decision.action != WorkerLifecycleAction::launchWorker)
            return false;

        if (!pipe.createServer(pipeName))
        {
            coordinator.workerExited();
            return false;
        }

        // The Worker receives only its private pipe endpoint and the session
        // identity. It must echo both values in its initial READY status.
        const std::wstring args = L"--voice-studio-worker \"" + pipeName +
            L"\" " + std::to_wstring(coordinator.currentGeneration()) +
            L" " + std::to_wstring(coordinator.currentRequestId());
        if (!process.launch(executablePath, args))
        {
            pipe.close();
            coordinator.workerExited();
            return false;
        }

        active = true;
        if (!pipe.waitForClient(connectTimeoutMs))
            return failAndTerminate();

        WorkerStatusMessage ready{};
        if (!pipe.receiveStatus(ready, connectTimeoutMs) ||
            ready.state != static_cast<std::uint16_t>(WorkerState::ready) ||
            !coordinator.acceptStatus(ready))
            return failAndTerminate();

        return true;
    }

    bool enableAi(std::uint32_t timeoutMs = 30000) noexcept
    {
        return execute(coordinator.enableAi(timeoutMs));
    }

    bool disableAi(std::uint64_t nowNs, std::uint32_t timeoutMs = 30000) noexcept
    {
        return execute(coordinator.disableAi(nowNs, timeoutMs));
    }

    // A false result means no valid status was received (timeout, disconnect,
    // or malformed message). The caller can distinguish process death with
    // workerHasExited(); a timeout alone does not block the audio path.
    bool pollStatus(DWORD timeoutMs = 100) noexcept
    {
        if (!active || !process.isRunning())
        {
            handleWorkerExit();
            return false;
        }

        WorkerStatusMessage status{};
        if (!pipe.receiveStatus(status, timeoutMs))
        {
            if (process.hasExited())
                handleWorkerExit();
            return false;
        }

        if (!coordinator.acceptStatus(status))
            return failAndTerminate();

        if (coordinator.currentState() == WorkerLifecycleState::faulted)
            return failAndTerminate();
        return true;
    }

    bool tick(std::uint64_t nowNs, std::uint32_t timeoutMs = 30000) noexcept
    {
        if (!active)
            return false;
        if (process.hasExited())
        {
            handleWorkerExit();
            return false;
        }
        return execute(coordinator.tick(nowNs, timeoutMs));
    }

    bool unloadNow(std::uint32_t timeoutMs = 30000) noexcept
    {
        if (!active)
            return false;
        return execute(coordinator.unloadNow(), timeoutMs);
    }

    bool workerHasExited() const noexcept { return process.hasExited(); }
    bool audioConversionAvailable() const noexcept
    {
        return coordinator.audioConversionAvailable();
    }
    WorkerLifecycleState currentState() const noexcept
    {
        return coordinator.currentState();
    }
    std::uint64_t generation() const noexcept
    {
        return coordinator.currentGeneration();
    }
    DWORD workerProcessId() const noexcept { return process.id(); }

    // Explicit shutdown is intentionally off the audio thread. Job Object
    // closure contains descendants and waits for process death before handles
    // are released.
    void shutdown() noexcept
    {
        if (active)
        {
            coordinator.workerExited();
            pipe.close();
            process.shutdown();
            active = false;
        }
    }

private:
    bool execute(const WorkerControlDecision& decision) noexcept
    {
        if (decision.action == WorkerLifecycleAction::none)
            return true;
        if (decision.action == WorkerLifecycleAction::terminateWorker)
            return terminateAndFault();
        if (decision.action == WorkerLifecycleAction::launchWorker)
            return false;
        if (!decision.hasCommand || !pipe.sendCommand(decision.command, 3000))
            return terminateAndFault();
        return true;
    }

    bool failAndTerminate() noexcept
    {
        coordinator.workerExited();
        return terminateAndFault();
    }

    bool terminateAndFault() noexcept
    {
        coordinator.workerExited();
        pipe.close();
        if (process.nativeProcessHandle() != nullptr)
        {
            if (!process.hasExited() && !process.terminate())
                return false;
            process.closeAfterExit();
        }
        active = false;
        return false;
    }

    void handleWorkerExit() noexcept
    {
        coordinator.workerExited();
        pipe.close();
        if (process.hasExited())
            process.closeAfterExit();
        active = false;
    }

    WorkerControlCoordinator coordinator;
    WindowsWorkerControlPipe pipe;
    WindowsWorkerProcess process;
    bool active = false;
};
} // namespace voice_studio::ipc
