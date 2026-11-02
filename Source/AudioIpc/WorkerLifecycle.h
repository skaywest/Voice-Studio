#pragma once

#include "WorkerControlAbi.h"

#include <cstdint>

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
    running,
    coolingDown,
    stopping,
    faulted
};

enum class WorkerLifecycleAction : std::uint8_t
{
    none,
    launchWorker,
    sendStart,
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
                if (state == WorkerLifecycleState::starting ||
                    state == WorkerLifecycleState::stopping)
                    state = WorkerLifecycleState::ready;
                break;
            case WorkerState::processing:
                if (state == WorkerLifecycleState::ready ||
                    state == WorkerLifecycleState::running)
                    state = WorkerLifecycleState::running;
                break;
            case WorkerState::stopping:
                state = WorkerLifecycleState::stopping;
                break;
            case WorkerState::stopped:
                state = WorkerLifecycleState::disabled;
                break;
            case WorkerState::starting:
            case WorkerState::loadingModel:
            case WorkerState::error:
                break;
        }
        return true;
    }

    WorkerLifecycleAction userEnableAi() noexcept
    {
        if (state != WorkerLifecycleState::ready &&
            state != WorkerLifecycleState::coolingDown)
            return WorkerLifecycleAction::none;
        ++requestId;
        if (requestId == 0)
            ++requestId;
        state = WorkerLifecycleState::running;
        return WorkerLifecycleAction::sendStart;
    }

    WorkerLifecycleAction userDisableAi(std::uint64_t nowNs) noexcept
    {
        if (state == WorkerLifecycleState::running)
        {
            ++requestId;
            if (requestId == 0)
                ++requestId;
            cooldownDeadlineNs = nowNs + kCooldownNs;
            state = WorkerLifecycleState::coolingDown;
            return WorkerLifecycleAction::sendStop;
        }
        return WorkerLifecycleAction::none;
    }

    WorkerLifecycleAction tick(std::uint64_t nowNs) noexcept
    {
        if ((state == WorkerLifecycleState::starting ||
             state == WorkerLifecycleState::ready ||
             state == WorkerLifecycleState::running ||
             state == WorkerLifecycleState::coolingDown) &&
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
            state = WorkerLifecycleState::stopping;
            return WorkerLifecycleAction::sendUnloadNow;
        }
        return WorkerLifecycleAction::none;
    }

    WorkerLifecycleAction unloadNow() noexcept
    {
        if (state == WorkerLifecycleState::disabled ||
            state == WorkerLifecycleState::faulted)
            return WorkerLifecycleAction::none;
        state = WorkerLifecycleState::stopping;
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
