#include "AudioIpc/WorkerLifecycle.h"

#include <cassert>
#include <cstdint>

using namespace voice_studio::ipc;

static WorkerStatusMessage statusFor(const WorkerLifecycle& lifecycle,
                                     WorkerState state,
                                     std::uint64_t heartbeat)
{
    WorkerStatusMessage status{};
    status.magic = kWorkerControlMagic;
    status.version = kWorkerControlVersion;
    status.size = sizeof(status);
    status.state = static_cast<std::uint16_t>(state);
    status.result = static_cast<std::uint32_t>(WorkerResult::ok);
    status.generation = lifecycle.currentGeneration();
    status.requestId = lifecycle.currentRequestId();
    status.heartbeatMonotonicNs = heartbeat;
    return status;
}

int main()
{
    WorkerLifecycle lifecycle;
    assert(lifecycle.currentState() == WorkerLifecycleState::disabled);
    assert(!lifecycle.isAudioConversionAvailable());

    assert(lifecycle.userEnableAi() == WorkerLifecycleAction::none);
    assert(lifecycle.userStart(100) == WorkerLifecycleAction::launchWorker);
    const auto firstGeneration = lifecycle.currentGeneration();
    assert(firstGeneration != 0);
    assert(lifecycle.currentState() == WorkerLifecycleState::starting);

    auto stale = statusFor(lifecycle, WorkerState::ready, 200);
    stale.generation--;
    assert(!lifecycle.acceptStatus(stale));

    auto malformed = statusFor(lifecycle, WorkerState::ready, 200);
    malformed.magic = 0;
    assert(!lifecycle.acceptStatus(malformed));

    assert(lifecycle.acceptStatus(statusFor(lifecycle, WorkerState::ready, 200)));
    assert(lifecycle.currentState() == WorkerLifecycleState::ready);
    assert(!lifecycle.isAudioConversionAvailable());

    assert(lifecycle.userEnableAi() == WorkerLifecycleAction::sendStart);
    assert(lifecycle.currentState() == WorkerLifecycleState::running);
    assert(lifecycle.isAudioConversionAvailable());
    assert(lifecycle.acceptStatus(statusFor(lifecycle, WorkerState::processing, 300)));

    assert(lifecycle.userDisableAi(400) == WorkerLifecycleAction::sendStop);
    assert(lifecycle.currentState() == WorkerLifecycleState::coolingDown);
    assert(!lifecycle.isAudioConversionAvailable());
    assert(lifecycle.tick(400 + WorkerLifecycle::kCooldownNs - 1) == WorkerLifecycleAction::none);
    assert(lifecycle.tick(400 + WorkerLifecycle::kCooldownNs) == WorkerLifecycleAction::sendUnloadNow);
    assert(lifecycle.currentState() == WorkerLifecycleState::stopping);

    assert(lifecycle.workerExited() == WorkerLifecycleAction::none);
    assert(lifecycle.currentState() == WorkerLifecycleState::faulted);
    assert(!lifecycle.isAudioConversionAvailable());

    assert(lifecycle.userStart(10'000) == WorkerLifecycleAction::launchWorker);
    assert(lifecycle.currentGeneration() != firstGeneration);

    // Heartbeat expiry requests process termination and immediately disables
    // conversion; it never blocks waiting for the worker.
    assert(lifecycle.tick(10'000 + WorkerLifecycle::kHeartbeatTimeoutNs + 1)
           == WorkerLifecycleAction::terminateWorker);
    assert(lifecycle.currentState() == WorkerLifecycleState::faulted);
    assert(!lifecycle.isAudioConversionAvailable());

    // A failed worker status faults the session.
    assert(lifecycle.userStart(20'000) == WorkerLifecycleAction::launchWorker);
    auto error = statusFor(lifecycle, WorkerState::error, 20'100);
    error.result = static_cast<std::uint32_t>(WorkerResult::failed);
    assert(lifecycle.acceptStatus(error));
    assert(lifecycle.currentState() == WorkerLifecycleState::faulted);

    return 0;
}
