#include "AudioIpc/WorkerControlCoordinator.h"

#include <cassert>
#include <cstdint>
#include <thread>

using namespace voice_studio::ipc;

static WorkerStatusMessage makeStatus(const WorkerControlCoordinator& control,
                                      WorkerState state,
                                      std::uint64_t heartbeat)
{
    WorkerStatusMessage status{};
    status.magic = kWorkerControlMagic;
    status.version = kWorkerControlVersion;
    status.size = sizeof(status);
    status.state = static_cast<std::uint16_t>(state);
    status.result = static_cast<std::uint32_t>(WorkerResult::ok);
    status.generation = control.currentGeneration();
    status.requestId = control.currentRequestId();
    status.heartbeatMonotonicNs = heartbeat;
    return status;
}

int main()
{
    WorkerControlCoordinator control;
    assert(!control.audioConversionAvailable());

    const auto launch = control.start(100);
    assert(launch.action == WorkerLifecycleAction::launchWorker);
    assert(!launch.hasCommand);
    assert(!control.audioConversionAvailable());

    assert(control.acceptStatus(makeStatus(control, WorkerState::ready, 200)));
    assert(control.currentState() == WorkerLifecycleState::ready);
    const auto runtimeConfig = control.configureRuntimeManifest(5000);
    assert(runtimeConfig.action == WorkerLifecycleAction::sendConfigureRuntimeManifest);
    assert(runtimeConfig.hasCommand);
    assert(runtimeConfig.command.command == static_cast<std::uint16_t>(WorkerCommand::configureRuntimeManifest));
    assert(!control.audioConversionAvailable());

    const auto configure = control.configureModel();
    assert(configure.action == WorkerLifecycleAction::sendConfigureModel);
    assert(configure.hasCommand);
    assert(configure.command.command == static_cast<std::uint16_t>(WorkerCommand::configureModel));
    assert(control.acceptStatus(makeStatus(control, WorkerState::ready, 225)));
    assert(!control.audioConversionAvailable());

    const auto start = control.enableAi(2500);
    assert(start.action == WorkerLifecycleAction::sendStart);
    assert(start.hasCommand);
    assert(start.command.command == static_cast<std::uint16_t>(WorkerCommand::start));
    assert(start.command.timeoutMs == 2500);
    assert(!control.audioConversionAvailable());

    assert(control.acceptStatus(makeStatus(control, WorkerState::processing, 300)));
    assert(control.currentState() == WorkerLifecycleState::running);
    assert(control.audioConversionAvailable());

    // The flag is independently readable from a non-control thread. It never
    // requires that thread to touch lifecycle state or perform IPC.
    bool observedAvailable = false;
    std::thread audioObserver([&] {
        observedAvailable = control.audioConversionAvailable();
    });
    audioObserver.join();
    assert(observedAvailable);

    const auto stop = control.disableAi(400, 3500);
    assert(stop.action == WorkerLifecycleAction::sendStop);
    assert(stop.hasCommand);
    assert(stop.command.command == static_cast<std::uint16_t>(WorkerCommand::stop));
    assert(!control.audioConversionAvailable());

    assert(control.acceptStatus(makeStatus(control, WorkerState::ready, 500)));
    assert(control.currentState() == WorkerLifecycleState::coolingDown);
    assert(!control.audioConversionAvailable());

    // Simulate a live Worker heartbeat while testing the longer cooldown.
    const auto beforeDeadline = 400 + WorkerLifecycle::kCooldownNs - 1;
    assert(control.acceptStatus(makeStatus(control, WorkerState::ready, beforeDeadline)));
    const auto beforeUnload = control.tick(beforeDeadline);
    assert(beforeUnload.action == WorkerLifecycleAction::none);
    assert(!beforeUnload.hasCommand);

    assert(control.acceptStatus(makeStatus(control, WorkerState::ready,
                                           400 + WorkerLifecycle::kCooldownNs)));
    const auto unload = control.tick(400 + WorkerLifecycle::kCooldownNs);
    assert(unload.action == WorkerLifecycleAction::sendUnloadNow);
    assert(unload.hasCommand);
    assert(unload.command.command == static_cast<std::uint16_t>(WorkerCommand::unloadModel));

    assert(control.acceptStatus(makeStatus(control, WorkerState::stopped,
                                           400 + WorkerLifecycle::kCooldownNs + 1)));
    assert(control.currentState() == WorkerLifecycleState::ready);
    assert(!control.audioConversionAvailable());

    control.workerExited();
    assert(control.currentState() == WorkerLifecycleState::faulted);
    assert(!control.audioConversionAvailable());

    // A heartbeat timeout requests termination and publishes bypass before
    // the caller performs any blocking process cleanup.
    assert(control.start(10'000).action == WorkerLifecycleAction::launchWorker);
    const auto timeout = control.tick(10'000 + WorkerLifecycle::kHeartbeatTimeoutNs + 1);
    assert(timeout.action == WorkerLifecycleAction::terminateWorker);
    assert(!timeout.hasCommand);
    assert(!control.audioConversionAvailable());

    // Stale status cannot re-enable conversion after the timeout.
    auto stale = makeStatus(control, WorkerState::processing, 20'000);
    assert(!control.acceptStatus(stale));
    assert(!control.audioConversionAvailable());
    return 0;
}
