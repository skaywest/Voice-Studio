#include "AudioIpc/WorkerLifecycle.h"
#include "AudioIpc/WorkerCommandFactory.h"

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

    WorkerCommandMessage command{};
    assert(lifecycle.userConfigureModel() == WorkerLifecycleAction::sendConfigureModel);
    assert(lifecycle.currentState() == WorkerLifecycleState::ready);
    assert(lifecycle.userConfigureRuntimeManifest() == WorkerLifecycleAction::sendConfigureRuntimeManifest);
    assert(makeWorkerCommand(WorkerLifecycleAction::sendConfigureRuntimeManifest, lifecycle, 5000, command));
    assert(command.command == static_cast<std::uint16_t>(WorkerCommand::configureRuntimeManifest));
    assert(makeWorkerCommand(WorkerLifecycleAction::sendConfigureModel, lifecycle, 5000, command));
    assert(command.command == static_cast<std::uint16_t>(WorkerCommand::configureModel));
    assert(lifecycle.currentState() == WorkerLifecycleState::ready);
    assert(lifecycle.acceptStatus(statusFor(lifecycle, WorkerState::ready, 250)));
    assert(!lifecycle.isAudioConversionAvailable());

    assert(lifecycle.userEnableAi() == WorkerLifecycleAction::sendStart);
    assert(makeWorkerCommand(WorkerLifecycleAction::sendStart, lifecycle, 5000, command));
    assert(hasValidWorkerCommand(command));
    assert(command.command == static_cast<std::uint16_t>(WorkerCommand::start));
    assert(command.generation == lifecycle.currentGeneration());
    assert(command.requestId == lifecycle.currentRequestId());
    assert(command.timeoutMs == 5000);
    assert(!makeWorkerCommand(WorkerLifecycleAction::launchWorker, lifecycle, 5000, command));
    assert(!makeWorkerCommand(WorkerLifecycleAction::sendStart, lifecycle,
                              kWorkerControlMaxTimeoutMs + 1, command));
    assert(lifecycle.currentState() == WorkerLifecycleState::enabling);
    assert(!lifecycle.isAudioConversionAvailable());
    assert(lifecycle.acceptStatus(statusFor(lifecycle, WorkerState::processing, 300)));
    assert(lifecycle.currentState() == WorkerLifecycleState::running);
    assert(lifecycle.isAudioConversionAvailable());

    assert(lifecycle.userDisableAi(400) == WorkerLifecycleAction::sendStop);
    assert(makeWorkerCommand(WorkerLifecycleAction::sendStop, lifecycle, 5000, command));
    assert(command.command == static_cast<std::uint16_t>(WorkerCommand::stop));
    assert(command.requestId == lifecycle.currentRequestId());
    assert(lifecycle.currentState() == WorkerLifecycleState::coolingDown);
    assert(!lifecycle.isAudioConversionAvailable());
    // A successful Stop acknowledgement must not cancel the 30-second
    // model-retention window or make the conversion path available.
    assert(lifecycle.acceptStatus(statusFor(lifecycle, WorkerState::ready, 500)));
    assert(lifecycle.currentState() == WorkerLifecycleState::coolingDown);
    assert(!lifecycle.isAudioConversionAvailable());
    assert(lifecycle.tick(400 + WorkerLifecycle::kCooldownNs - 1) == WorkerLifecycleAction::none);
    assert(lifecycle.tick(400 + WorkerLifecycle::kCooldownNs) == WorkerLifecycleAction::sendUnloadNow);
    assert(lifecycle.currentState() == WorkerLifecycleState::unloading);
    assert(makeWorkerCommand(WorkerLifecycleAction::sendUnloadNow, lifecycle, 5000, command));
    assert(command.command == static_cast<std::uint16_t>(WorkerCommand::unloadModel));
    assert(command.requestId == lifecycle.currentRequestId());
    assert(lifecycle.acceptStatus(statusFor(lifecycle, WorkerState::stopped,
                                            400 + WorkerLifecycle::kCooldownNs + 1)));
    assert(lifecycle.currentState() == WorkerLifecycleState::ready);

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
