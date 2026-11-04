#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace voice_studio::ipc
{
// Versioned, fixed-size control messages. These structs are wire-format PODs;
// transport (named pipe) and process ownership are implemented separately.
inline constexpr std::uint32_t kWorkerControlMagic = 0x56534331u; // "VSC1"
inline constexpr std::uint16_t kWorkerControlVersion = 1;
inline constexpr std::uint32_t kWorkerControlMaxTimeoutMs = 30000;
inline constexpr std::uint32_t kWorkerControlKnownFlags = 0;

enum class WorkerCommand : std::uint16_t
{
    none = 0,
    start = 1,
    stop = 2,
    unloadModel = 3,
    ping = 4,
    configureModel = 5,
    configureRuntimeManifest = 6
};

enum class WorkerState : std::uint16_t
{
    stopped = 0,
    starting = 1,
    ready = 2,
    loadingModel = 3,
    processing = 4,
    stopping = 5,
    error = 6
};

enum class WorkerResult : std::uint32_t
{
    ok = 0,
    unsupported = 1,
    invalidState = 2,
    invalidRequest = 3,
    failed = 4
};

struct WorkerCommandMessage
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint16_t command;
    std::uint16_t reserved16;
    std::uint32_t flags;
    std::uint64_t generation;
    std::uint64_t requestId;
    std::uint32_t timeoutMs;
    std::uint32_t reserved32;
};

struct WorkerStatusMessage
{
    std::uint32_t magic;
    std::uint16_t version;
    std::uint16_t size;
    std::uint16_t state;
    std::uint16_t reserved16;
    std::uint32_t result;
    std::uint32_t flags;
    std::uint64_t generation;
    std::uint64_t requestId;
    std::uint64_t heartbeatMonotonicNs;
    std::uint32_t reserved32;
};

static_assert(sizeof(WorkerCommandMessage) == 40, "Worker command wire size is fixed");
static_assert(sizeof(WorkerStatusMessage) == 56, "Worker status wire size is fixed");
static_assert(std::is_standard_layout<WorkerCommandMessage>::value, "Command must be standard layout");
static_assert(std::is_trivially_copyable<WorkerCommandMessage>::value, "Command must be trivially copyable");
static_assert(std::is_standard_layout<WorkerStatusMessage>::value, "Status must be standard layout");
static_assert(std::is_trivially_copyable<WorkerStatusMessage>::value, "Status must be trivially copyable");

inline bool isKnownWorkerCommand(std::uint16_t value) noexcept
{
    return value >= static_cast<std::uint16_t>(WorkerCommand::start) &&
           value <= static_cast<std::uint16_t>(WorkerCommand::configureRuntimeManifest);
}

inline bool isKnownWorkerState(std::uint16_t value) noexcept
{
    return value <= static_cast<std::uint16_t>(WorkerState::error);
}

inline bool hasValidWorkerCommand(const WorkerCommandMessage& message) noexcept
{
    return message.magic == kWorkerControlMagic &&
           message.version == kWorkerControlVersion &&
           message.size == sizeof(WorkerCommandMessage) &&
           isKnownWorkerCommand(message.command) &&
           message.reserved16 == 0 && message.reserved32 == 0 &&
           message.flags == kWorkerControlKnownFlags &&
           message.generation != 0 && message.requestId != 0 &&
           message.timeoutMs <= kWorkerControlMaxTimeoutMs;
}

inline bool hasValidWorkerStatus(const WorkerStatusMessage& message) noexcept
{
    return message.magic == kWorkerControlMagic &&
           message.version == kWorkerControlVersion &&
           message.size == sizeof(WorkerStatusMessage) &&
           isKnownWorkerState(message.state) &&
           message.reserved16 == 0 && message.reserved32 == 0 &&
           message.flags == kWorkerControlKnownFlags &&
           message.generation != 0 && message.requestId != 0 &&
           message.heartbeatMonotonicNs != 0;
}
} // namespace voice_studio::ipc
