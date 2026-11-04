#pragma once

#include "WorkerLifecycle.h"

namespace voice_studio::ipc
{
// Translate a lifecycle decision into the versioned wire command. This is
// deliberately transport-agnostic: callers send the returned POD from their
// control thread; process launch/termination remain WindowsWorkerProcess work.
// Returns false for actions that do not represent a Worker IPC command.
inline bool makeWorkerCommand(WorkerLifecycleAction action,
                              const WorkerLifecycle& lifecycle,
                              std::uint32_t timeoutMs,
                              WorkerCommandMessage& output) noexcept
{
    WorkerCommand command = WorkerCommand::none;
    switch (action)
    {
        case WorkerLifecycleAction::sendStart:
            command = WorkerCommand::start;
            break;
        case WorkerLifecycleAction::sendConfigureModel:
            command = WorkerCommand::configureModel;
            break;
        case WorkerLifecycleAction::sendConfigureRuntimeManifest:
            command = WorkerCommand::configureRuntimeManifest;
            break;
        case WorkerLifecycleAction::sendStop:
            command = WorkerCommand::stop;
            break;
        case WorkerLifecycleAction::sendUnloadNow:
            command = WorkerCommand::unloadModel;
            break;
        case WorkerLifecycleAction::none:
        case WorkerLifecycleAction::launchWorker:
        case WorkerLifecycleAction::terminateWorker:
            return false;
    }

    WorkerCommandMessage message{};
    message.magic = kWorkerControlMagic;
    message.version = kWorkerControlVersion;
    message.size = static_cast<std::uint16_t>(sizeof(WorkerCommandMessage));
    message.command = static_cast<std::uint16_t>(command);
    message.generation = lifecycle.currentGeneration();
    message.requestId = lifecycle.currentRequestId();
    message.timeoutMs = timeoutMs;

    if (!hasValidWorkerCommand(message))
        return false;

    output = message;
    return true;
}
} // namespace voice_studio::ipc
