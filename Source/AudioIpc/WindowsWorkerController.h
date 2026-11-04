#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "AudioBlockRealtimeAccess.h"
#include "WindowsAudioBlockMapping.h"
#include "WindowsWorkerControlPipe.h"
#include "WorkerRuntimeManifestVerifier.h"
#include "WindowsWorkerProcess.h"
#include "WorkerControlCoordinator.h"

#include <array>
#include <string>
#include <vector>

namespace voice_studio::ipc
{
// Owns the real Windows control plane for one Worker generation. Every method
// except audioConversionAvailable() is control-thread-only and may block for
// its explicit timeout. The audio callback must never call this class.
class WindowsWorkerController final
{
public:
    WindowsWorkerController() noexcept : realtimeAccess(&ownedRealtimeAccess) {}
    explicit WindowsWorkerController(AudioBlockRealtimeAccess& externalAccess) noexcept
        : realtimeAccess(&externalAccess) {}
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

        const std::wstring audioMappingName = L"Local\\VoiceStudioAudio-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(coordinator.currentGeneration());
        if (!audioMapping.createServer(audioMappingName))
        {
            pipe.close();
            coordinator.workerExited();
            return false;
        }

        // The Worker validates the fixed audio ABI before it announces READY.
        const std::wstring args = L"--voice-studio-worker \"" + pipeName +
            L"\" \"" + audioMappingName + L"\" " +
            std::to_wstring(coordinator.currentGeneration()) +
            L" " + std::to_wstring(coordinator.currentRequestId());
        if (!process.launch(executablePath, args))
        {
            pipe.close();
            audioMapping.close();
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

        if (!realtimeAccess->attach(audioMapping.get()))
            return failAndTerminate();

        return true;
    }

    // Transfers a fixed-size model manifest and verifies every referenced file
    // inside the Worker. This is validation only; it does not load an inference
    // runtime or make audio conversion available.
    bool configureModel(WorkerModelConfigMessage config,
                        std::uint32_t timeoutMs = 30000) noexcept
    {
        if (!active)
            return false;
        config.generation = coordinator.currentGeneration();
        config.requestId = 1; // Replaced with the next lifecycle request below.
        config.size = static_cast<std::uint16_t>(sizeof(config));
        if (!hasValidWorkerModelConfig(config))
            return false;

        const auto decision = coordinator.configureModel();
        if (decision.action != WorkerLifecycleAction::sendConfigureModel ||
            !decision.hasCommand)
            return false;

        config.generation = decision.command.generation;
        config.requestId = decision.command.requestId;
        config.size = static_cast<std::uint16_t>(sizeof(config));
        if (!hasValidWorkerModelConfig(config) ||
            !pipe.sendCommand(decision.command, timeoutMs) ||
            !pipe.sendModelConfig(config, timeoutMs))
            return terminateAndFault();

        WorkerStatusMessage status{};
        if (!pipe.receiveStatus(status, timeoutMs) ||
            !coordinator.acceptStatus(status) ||
            coordinator.currentState() == WorkerLifecycleState::faulted)
            return terminateAndFault();
        return coordinator.currentState() == WorkerLifecycleState::ready;
    }


    // Streams a complete runtime manifest and its local source files. The
    // Worker validates the received bytes before acknowledging READY.
    bool configureRuntimeFiles(const WorkerRuntimeManifestHeader& header,
                               const WorkerRuntimeManifestEntry* entries,
                               std::size_t count,
                               const std::vector<std::wstring>& sourcePaths,
                               std::uint32_t timeoutMs = 30000) noexcept
    {
        if (!active || entries == nullptr || count == 0 || count != sourcePaths.size() ||
            !hasValidWorkerRuntimeManifestDigest(header, entries, count))
            return false;
        const auto decision = coordinator.configureRuntimeManifest(timeoutMs);
        if (decision.action != WorkerLifecycleAction::sendConfigureRuntimeManifest ||
            !decision.hasCommand || !pipe.sendCommand(decision.command, timeoutMs) ||
            !pipe.sendRuntimeManifest(header, entries, count, timeoutMs))
            return terminateAndFault();

        std::array<std::uint8_t, kWorkerRuntimeFileChunkMaxBytes> payload{};
        for (std::size_t index = 0; index < count; ++index)
        {
            HANDLE source = CreateFileW(sourcePaths[index].c_str(), GENERIC_READ,
                FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (source == INVALID_HANDLE_VALUE)
                return terminateAndFault();
            LARGE_INTEGER sourceSize{};
            if (!GetFileSizeEx(source, &sourceSize) || sourceSize.QuadPart < 0 ||
                static_cast<std::uint64_t>(sourceSize.QuadPart) != entries[index].sizeBytes)
            {
                CloseHandle(source);
                return terminateAndFault();
            }

            std::uint64_t offset = 0;
            bool sent = true;
            while (offset < entries[index].sizeBytes)
            {
                const auto remaining = entries[index].sizeBytes - offset;
                const DWORD requested = static_cast<DWORD>(remaining < payload.size()
                    ? remaining : payload.size());
                DWORD read = 0;
                if (!ReadFile(source, payload.data(), requested, &read, nullptr) || read != requested)
                {
                    sent = false;
                    break;
                }
                WorkerRuntimeFileChunkHeader chunk{};
                chunk.entryIndex = static_cast<std::uint32_t>(index);
                chunk.payloadBytes = read;
                chunk.offsetBytes = offset;
                chunk.size = static_cast<std::uint16_t>(sizeof(chunk));
                if (offset + read == entries[index].sizeBytes)
                    chunk.flags = kWorkerRuntimeFileChunkFlagFinal;
                if (!pipe.sendRuntimeFileChunk(chunk, payload.data(), timeoutMs))
                {
                    sent = false;
                    break;
                }
                offset += read;
            }
            std::uint8_t extra = 0;
            DWORD extraRead = 0;
            const bool noExtra = sent && ReadFile(source, &extra, 1, &extraRead, nullptr) && extraRead == 0;
            CloseHandle(source);
            if (!noExtra)
                return terminateAndFault();
        }

        WorkerStatusMessage status{};
        if (!pipe.receiveStatus(status, timeoutMs) || !coordinator.acceptStatus(status) ||
            coordinator.currentState() == WorkerLifecycleState::faulted)
            return terminateAndFault();
        return coordinator.currentState() == WorkerLifecycleState::ready;
    }

    bool enableAi(std::uint32_t timeoutMs = 30000) noexcept
    {
        return execute(coordinator.enableAi(timeoutMs), timeoutMs);
    }

    bool disableAi(std::uint64_t nowNs, std::uint32_t timeoutMs = 30000) noexcept
    {
        return execute(coordinator.disableAi(nowNs, timeoutMs), timeoutMs);
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
        return execute(coordinator.tick(nowNs, timeoutMs), timeoutMs);
    }

    bool unloadNow(std::uint32_t timeoutMs = 30000) noexcept
    {
        if (!active)
            return false;
        return execute(coordinator.unloadNow(timeoutMs), timeoutMs);
    }

    bool workerHasExited() const noexcept { return process.hasExited(); }
    bool audioConversionAvailable() const noexcept
    {
        return coordinator.audioConversionAvailable();
    }
    // The returned access object may be acquired from the audio thread. Its
    // mapping is detached before any controller path unmaps the shared region.
    AudioBlockRealtimeAccess& audioRealtimeAccess() noexcept
    {
        return *realtimeAccess;
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
            realtimeAccess->detach();
            pipe.close();
            process.shutdown();
            audioMapping.close();
            active = false;
        }
    }

private:
    bool execute(const WorkerControlDecision& decision,
                 std::uint32_t timeoutMs = 3000) noexcept
    {
        if (decision.action == WorkerLifecycleAction::none)
            return true;
        if (decision.action == WorkerLifecycleAction::terminateWorker)
            return terminateAndFault();
        if (decision.action == WorkerLifecycleAction::launchWorker)
            return false;
        if (!decision.hasCommand || !pipe.sendCommand(decision.command, timeoutMs))
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
        realtimeAccess->detach();
        pipe.close();
        audioMapping.close();
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
        realtimeAccess->detach();
        pipe.close();
        audioMapping.close();
        if (process.hasExited())
            process.closeAfterExit();
        active = false;
    }

    WorkerControlCoordinator coordinator;
    WindowsWorkerControlPipe pipe;
    WindowsAudioBlockMapping audioMapping;
    AudioBlockRealtimeAccess ownedRealtimeAccess;
    AudioBlockRealtimeAccess* realtimeAccess;
    WindowsWorkerProcess process;
    bool active = false;
};
} // namespace voice_studio::ipc
