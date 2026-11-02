#pragma once

#include "WorkerCommandFactory.h"

#include <atomic>
#include <cstdint>

namespace voice_studio::ipc
{
static_assert(std::atomic<bool>::is_always_lock_free,
              "The audio-thread availability flag must be lock-free");
// Control-thread facade joining lifecycle policy, command serialization and
// real-time availability publication. All mutating methods are control-thread
// only. The audio callback may call only audioConversionAvailable().
struct WorkerControlDecision
{
    WorkerLifecycleAction action = WorkerLifecycleAction::none;
    WorkerCommandMessage command{};
    bool hasCommand = false;
};

class WorkerControlCoordinator final
{
public:
    WorkerControlCoordinator() = default;
    WorkerControlCoordinator(const WorkerControlCoordinator&) = delete;
    WorkerControlCoordinator& operator=(const WorkerControlCoordinator&) = delete;

    WorkerControlDecision start(std::uint64_t nowNs) noexcept
    {
        return apply(lifecycle.userStart(nowNs));
    }

    WorkerControlDecision enableAi(std::uint32_t timeoutMs = 30000) noexcept
    {
        return apply(lifecycle.userEnableAi(), timeoutMs);
    }

    WorkerControlDecision disableAi(std::uint64_t nowNs,
                                    std::uint32_t timeoutMs = 30000) noexcept
    {
        return apply(lifecycle.userDisableAi(nowNs), timeoutMs);
    }

    WorkerControlDecision tick(std::uint64_t nowNs,
                               std::uint32_t timeoutMs = 30000) noexcept
    {
        return apply(lifecycle.tick(nowNs), timeoutMs);
    }

    bool acceptStatus(const WorkerStatusMessage& status) noexcept
    {
        const bool accepted = lifecycle.acceptStatus(status);
        if (accepted)
            publishAvailability();
        return accepted;
    }

    void workerExited() noexcept
    {
        lifecycle.workerExited();
        publishAvailability();
    }

    bool audioConversionAvailable() const noexcept
    {
        return audioAvailable.load(std::memory_order_acquire);
    }

    WorkerLifecycleState currentState() const noexcept
    {
        return lifecycle.currentState();
    }

    std::uint64_t currentGeneration() const noexcept
    {
        return lifecycle.currentGeneration();
    }

    std::uint64_t currentRequestId() const noexcept
    {
        return lifecycle.currentRequestId();
    }

private:
    WorkerControlDecision apply(WorkerLifecycleAction action,
                                std::uint32_t timeoutMs = 30000) noexcept
    {
        WorkerControlDecision decision{};
        decision.action = action;
        if (action == WorkerLifecycleAction::sendStart ||
            action == WorkerLifecycleAction::sendStop ||
            action == WorkerLifecycleAction::sendUnloadNow)
        {
            decision.hasCommand = makeWorkerCommand(action, lifecycle,
                                                    timeoutMs, decision.command);
            if (!decision.hasCommand)
            {
                // A serialization failure must fail closed. The caller sees
                // no command and must tear down/fault the control session.
                lifecycle.workerExited();
                decision.action = WorkerLifecycleAction::terminateWorker;
            }
        }
        publishAvailability();
        return decision;
    }

    void publishAvailability() noexcept
    {
        audioAvailable.store(lifecycle.isAudioConversionAvailable(),
                             std::memory_order_release);
    }

    WorkerLifecycle lifecycle;
    std::atomic<bool> audioAvailable{false};
};
} // namespace voice_studio::ipc
