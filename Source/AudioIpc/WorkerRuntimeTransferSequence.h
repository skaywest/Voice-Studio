#pragma once

#include "WorkerRuntimeFileChunkSequence.h"
#include "WorkerRuntimeManifestVerifier.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace voice_studio::ipc
{
// Tracks the complete ordered transfer described by an already authenticated
// manifest. The caller must verify the manifest digest before begin() and must
// still verify each completed file hash before making staged files usable.
class WorkerRuntimeTransferSequence final
{
public:
    bool begin(const WorkerRuntimeManifestHeader& header,
               const WorkerRuntimeManifestEntry* entries,
               std::size_t count) noexcept
    {
        if (active || !hasValidWorkerRuntimeManifestDigest(header, entries, count))
            return false;

        for (std::size_t i = 0; i < count; ++i)
            expectedBytes[i] = entries[i].sizeBytes;

        entryCount = static_cast<std::uint32_t>(count);
        currentEntry = 0;
        nextOffset = 0;
        active = true;
        return true;
    }

    bool accept(const WorkerRuntimeFileChunkHeader& chunk) noexcept
    {
        if (!active || currentEntry >= entryCount)
            return false;

        WorkerRuntimeFileChunkSequence file{
            currentEntry, expectedBytes[currentEntry], nextOffset, false
        };
        if (!file.accept(chunk))
            return false;

        nextOffset = file.nextOffset;
        if (file.complete)
        {
            ++currentEntry;
            nextOffset = 0;
        }
        return true;
    }

    bool isActive() const noexcept { return active; }
    bool isComplete() const noexcept
    {
        return active && entryCount != 0 && currentEntry == entryCount;
    }
    std::uint32_t currentEntryIndex() const noexcept { return currentEntry; }
    std::uint64_t currentFileOffset() const noexcept { return nextOffset; }
    std::uint32_t fileCount() const noexcept { return entryCount; }

private:
    std::array<std::uint64_t, kWorkerRuntimeManifestMaxEntries> expectedBytes{};
    std::uint32_t entryCount = 0;
    std::uint32_t currentEntry = 0;
    std::uint64_t nextOffset = 0;
    bool active = false;
};
} // namespace voice_studio::ipc
