#pragma once

#include "WorkerRuntimeManifestAbi.h"
#include <cstdint>

namespace voice_studio::ipc
{
// Per-file protocol state. The caller creates one instance for the manifest
// entry currently being received and accepts chunks only on the control thread.
struct WorkerRuntimeFileChunkSequence
{
    std::uint32_t entryIndex = 0;
    std::uint64_t expectedBytes = 0;
    std::uint64_t nextOffset = 0;
    bool complete = false;

    bool isValid() const noexcept
    {
        return entryIndex < kWorkerRuntimeManifestMaxEntries &&
               expectedBytes > 0 &&
               expectedBytes <= kWorkerRuntimeManifestMaxFileBytes &&
               nextOffset <= expectedBytes;
    }

    bool accept(const WorkerRuntimeFileChunkHeader& chunk) noexcept
    {
        if (!isValid() || complete ||
            !hasValidWorkerRuntimeFileChunkHeader(chunk) ||
            chunk.entryIndex != entryIndex ||
            chunk.offsetBytes != nextOffset ||
            chunk.payloadBytes > expectedBytes - nextOffset)
            return false;

        const auto newOffset = nextOffset + chunk.payloadBytes;
        const bool reachesEnd = newOffset == expectedBytes;
        const bool markedFinal = (chunk.flags & kWorkerRuntimeFileChunkFlagFinal) != 0;
        if (reachesEnd != markedFinal)
            return false;

        nextOffset = newOffset;
        complete = reachesEnd;
        return true;
    }
};
} // namespace voice_studio::ipc
