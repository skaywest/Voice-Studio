#include "AudioIpc/WorkerControlAbi.h"

#include <cassert>
#include <cstdint>
#include <limits>

using namespace voice_studio::ipc;

int main()
{
    WorkerCommandMessage command{};
    command.magic = kWorkerControlMagic;
    command.version = kWorkerControlVersion;
    command.size = sizeof(command);
    command.command = static_cast<std::uint16_t>(WorkerCommand::start);
    command.generation = 7;
    command.requestId = 1;
    command.timeoutMs = 5000;
    assert(hasValidWorkerCommand(command));

    auto invalidCommand = command;
    invalidCommand.magic ^= 1u;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.version++;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.size--;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.command = 99;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.reserved16 = 1;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.flags = 1;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.generation = 0;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.requestId = 0;
    assert(!hasValidWorkerCommand(invalidCommand));
    invalidCommand = command;
    invalidCommand.timeoutMs = kWorkerControlMaxTimeoutMs + 1;
    assert(!hasValidWorkerCommand(invalidCommand));

    WorkerStatusMessage status{};
    status.magic = kWorkerControlMagic;
    status.version = kWorkerControlVersion;
    status.size = sizeof(status);
    status.state = static_cast<std::uint16_t>(WorkerState::ready);
    status.result = static_cast<std::uint32_t>(WorkerResult::ok);
    status.generation = 7;
    status.requestId = 1;
    status.heartbeatMonotonicNs = 123456;
    assert(hasValidWorkerStatus(status));

    auto invalidStatus = status;
    invalidStatus.state = 99;
    assert(!hasValidWorkerStatus(invalidStatus));
    invalidStatus = status;
    invalidStatus.reserved32 = 1;
    assert(!hasValidWorkerStatus(invalidStatus));
    invalidStatus = status;
    invalidStatus.heartbeatMonotonicNs = 0;
    assert(!hasValidWorkerStatus(invalidStatus));
    invalidStatus = status;
    invalidStatus.requestId = 0;
    assert(!hasValidWorkerStatus(invalidStatus));

    // A response from an earlier worker generation must never be accepted
    // as the current worker's response.
    const std::uint64_t activeGeneration = 8;
    assert(status.generation != activeGeneration);

    return 0;
}
