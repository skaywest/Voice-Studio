#pragma once

#include "WorkerControlAbi.h"

#include <cstdint>
#include <limits>

namespace voice_studio::ipc
{
// Control-thread-only lifecycle policy. This class owns no OS handles and
// performs no IPC; callers execute returned actions off the audio thread.
// The audio callback should consult only an independently published atomic
// availability flag, never call this class.
enum class WorkerLifecycleState : std::uint8_t
{
    disabled,
    starting,
    ready,
    enabling,
    running,
    coolingDown,
    unloading,
    stopping,
    faulted
};

enum class WorkerLifecycleAction : std::uint8_t
{
    none,
    launchWorker,
    sendStart,
    sendConfigureModel,
    sendConfigureRuntimeManifest,
    sendStop,
    sendUnloadNow,
    terminateWorker
};

class WorkerLifecycle final
{
public:
    static constexpr std::uint64_t kCooldownNs = 30'000'000'000ull;
    static constexpr std::uint64_t kHeartbeatTimeoutNs = 3'000'000'000ull;

    WorkerLifecycleAction userStart(std::uint64_t nowNs) noexcept
    {
        if (state != WorkerLifecycleState::disabled &&
            state != WorkerLifecycleState::faulted)
            return WorkerLifecycleAction::none;

        ++generation;
        if (generation == 0)
            ++generation;
        ++requestId;
        if (requestId == 0)
            ++requestId;
        lastHeartbeatNs = nowNs;
        state = WorkerLifecycleState::starting;
        return WorkerLifecycleAction::launchWorker;
    }

    bool acceptStatus(const WorkerStatusMessage& status) noexcept
    {
        // Once a session is disabled or faulted, late pipe messages from that
        // generation must never revive it or refresh its heartbeat.
        if (state == WorkerLifecycleState::disabled ||
            state == WorkerLifecycleState::faulted)
            return false;
        if (!hasValidWorkerStatus(status) || status.generation != generation ||
            status.requestId != requestId)
            return false;
        if (status.heartbeatMonotonicNs < lastHeartbeatNs)
            return false;

        lastHeartbeatNs = status.heartbeatMonotonicNs;
        const auto workerState = static_cast<WorkerState>(status.state);

        if (status.result != static_cast<std::uint32_t>(WorkerResult::ok) ||
            workerState == WorkerState::error)
        {
            state = WorkerLifecycleState::faulted;
            return true;
        }

        switch (workerState)
        {
            case WorkerState::ready:
                // A stop acknowledgement means processing has stopped, but the
                // worker and its model may remain resident during the cooldown.
                if (state == WorkerLifecycleState::starting ||
                    state == WorkerLifecycleState::enabling ||
                    state == WorkerLifecycleState::running ||
                    state == WorkerLifecycleState::stopping ||
                    state == WorkerLifecycleState::unloading)
                    state = WorkerLifecycleState::ready;
                // Do not discard the cooldown deadline when Stop is acked.
                break;
            case WorkerState::processing:
                if (state == WorkerLifecycleState::enabling ||
                    state == WorkerLifecycleState::running)
                    state = WorkerLifecycleState::running;
                break;
            case WorkerState::stopping:
                if (state != WorkerLifecycleState::coolingDown)
                    state = WorkerLifecycleState::stopping;
                break;
            case WorkerState::stopped:
                // "stopped" is an engine state, not proof that the process
                // exited. Keep the supervisor session available for restart.
                if (state == WorkerLifecycleState::starting ||
                    state == WorkerLifecycleState::enabling ||
                    state == WorkerLifecycleState::running ||
                    state == WorkerLifecycleState::unloading ||
                    state == WorkerLifecycleState::stopping)
                    state = WorkerLifecycleState::ready;
                break;
            case WorkerState::starting:
            case WorkerState::loadingModel:
            case WorkerState::error:
                break;
        }
        return true;
    }

    WorkerLifecycleAction userConfigureModel() noexcept
    {
        if (state != WorkerLifecycleState::ready)
            return WorkerLifecycleAction::none;
        ++requestId;
        if (requestId == 0)
            ++requestId;
        return WorkerLifecycleAction::sendConfigureModel;
    }

    WorkerLifecycleAction userConfigureRuntimeManifest() noexcept
    {
        if (state != WorkerLifecycleState::ready)
            return WorkerLifecycleAction::none;
        ++requestId;
        if (requestId == 0)
            ++requestId;
        return WorkerLifecycleAction::sendConfigureRuntimeManifest;
    }

    WorkerLifecycleAction userEnableAi() noexcept
    {
        if (state != WorkerLifecycleState::ready &&
            state != WorkerLifecycleState::coolingDown)
            return WorkerLifecycleAction::none;
        ++requestId;
        if (requestId == 0)
            ++requestId;
        state = WorkerLifecycleState::enabling;
        return WorkerLifecycleAction::sendStart;
    }

    WorkerLifecycleAction userDisableAi(std::uint64_t nowNs) noexcept
    {
        if (state == WorkerLifecycleState::running ||
            state == WorkerLifecycleState::enabling)
        {
            ++requestId;
            if (requestId == 0)
                ++requestId;
            const auto maxNs = (std::numeric_limits<std::uint64_t>::max)();
            cooldownDeadlineNs = nowNs > maxNs - kCooldownNs
                ? maxNs : nowNs + kCooldownNs;
            state = WorkerLifecycleState::coolingDown;
            return WorkerLifecycleAction::sendStop;
        }
        return WorkerLifecycleAction::none;
    }

    WorkerLifecycleAction tick(std::uint64_t nowNs) noexcept
    {
        if ((state == WorkerLifecycleState::starting ||
             state == WorkerLifecycleState::ready ||
             state == WorkerLifecycleState::enabling ||
             state == WorkerLifecycleState::running ||
             state == WorkerLifecycleState::coolingDown ||
             state == WorkerLifecycleState::unloading) &&
            lastHeartbeatNs != 0 &&
            nowNs >= lastHeartbeatNs &&
            nowNs - lastHeartbeatNs > kHeartbeatTimeoutNs)
        {
            state = WorkerLifecycleState::faulted;
            return WorkerLifecycleAction::terminateWorker;
        }

        if (state == WorkerLifecycleState::coolingDown &&
            nowNs >= cooldownDeadlineNs)
        {
            state = WorkerLifecycleState::unloading;
            ++requestId;
            if (requestId == 0)
                ++requestId;
            return WorkerLifecycleAction::sendUnloadNow;
        }
        return WorkerLifecycleAction::none;
    }

    WorkerLifecycleAction unloadNow() noexcept
    {
        if (state == WorkerLifecycleState::disabled ||
            state == WorkerLifecycleState::faulted)
            return WorkerLifecycleAction::none;
        state = WorkerLifecycleState::unloading;
        ++requestId;
        if (requestId == 0)
            ++requestId;
        return WorkerLifecycleAction::sendUnloadNow;
    }

    WorkerLifecycleAction workerExited() noexcept
    {
        state = WorkerLifecycleState::faulted;
        lastHeartbeatNs = 0;
        cooldownDeadlineNs = 0;
        return WorkerLifecycleAction::none;
    }

    WorkerLifecycleState currentState() const noexcept { return state; }
    std::uint64_t currentGeneration() const noexcept { return generation; }
    std::uint64_t currentRequestId() const noexcept { return requestId; }
    bool isAudioConversionAvailable() const noexcept
    {
        return state == WorkerLifecycleState::running;
    }

private:
    WorkerLifecycleState state = WorkerLifecycleState::disabled;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    std::uint64_t lastHeartbeatNs = 0;
    std::uint64_t cooldownDeadlineNs = 0;
};
} // namespace voice_studio::ipc
