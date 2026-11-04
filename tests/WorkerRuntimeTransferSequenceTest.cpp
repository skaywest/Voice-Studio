#include "AudioIpc/WorkerRuntimeTransferSequence.h"
#ifdef NDEBUG
 #undef NDEBUG
#endif
#include <cassert>
using namespace voice_studio::ipc;

static WorkerRuntimeFileChunkHeader makeChunk(std::uint32_t entry,
                                               std::uint64_t offset,
                                               std::uint32_t bytes,
                                               bool final)
{
    WorkerRuntimeFileChunkHeader chunk{};
    chunk.entryIndex = entry;
    chunk.offsetBytes = offset;
    chunk.payloadBytes = bytes;
    chunk.flags = final ? kWorkerRuntimeFileChunkFlagFinal : 0;
    chunk.size = static_cast<std::uint16_t>(sizeof(chunk));
    return chunk;
}

static void setPath(WorkerRuntimeManifestEntry& entry, const char16_t* path,
                    std::uint16_t length)
{
    entry.pathLength = length;
    for (std::size_t i = 0; i <= length; ++i)
        entry.relativePath[i] = path[i];
}

int main()
{
    WorkerRuntimeManifestHeader manifest{};
    manifest.size = static_cast<std::uint16_t>(sizeof(manifest));
    manifest.entryCount = 2;
    manifest.totalBytes = 9000;

    WorkerRuntimeManifestEntry entries[2]{};
    entries[0].role = static_cast<std::uint16_t>(WorkerRuntimeFileRole::pythonSource);
    entries[0].sizeBytes = 8192;
    entries[0].sha256[0] = 1;
    setPath(entries[0], u"worker/main.py", 14);
    entries[1].role = static_cast<std::uint16_t>(WorkerRuntimeFileRole::runtimeSupport);
    entries[1].sizeBytes = 808;
    entries[1].sha256[0] = 2;
    setPath(entries[1], u"runtime/core.bin", 16);

    WorkerRuntimeTransferSequence transfer;
    assert(transfer.begin(manifest, entries, 2));
    assert(transfer.isActive() && !transfer.isComplete());
    assert(transfer.fileCount() == 2 && transfer.currentEntryIndex() == 0);

    // The transfer cannot skip ahead to the second manifest entry.
    assert(!transfer.accept(makeChunk(1, 0, 808, true)));
    assert(transfer.currentEntryIndex() == 0);

    assert(transfer.accept(makeChunk(0, 0, 4096, false)));
    assert(transfer.currentFileOffset() == 4096);
    assert(!transfer.accept(makeChunk(0, 4097, 4096, true)));
    assert(transfer.accept(makeChunk(0, 4096, 4096, true)));
    assert(transfer.currentEntryIndex() == 1);
    assert(transfer.currentFileOffset() == 0);
    assert(!transfer.isComplete());

    assert(transfer.accept(makeChunk(1, 0, 808, true)));
    assert(transfer.isComplete());
    assert(transfer.currentEntryIndex() == 2);
    assert(!transfer.accept(makeChunk(1, 808, 1, true)));

    // A second begin is rejected; sessions are one-shot and cannot be reset
    // while a previous manifest's transfer state is still live.
    assert(!transfer.begin(manifest, entries, 2));

    WorkerRuntimeTransferSequence invalid;
    auto badEntries = entries;
    badEntries[1].pathLength = entries[0].pathLength;
    for (std::size_t i = 0; i <= entries[0].pathLength; ++i)
        badEntries[1].relativePath[i] = entries[0].relativePath[i];
    assert(!invalid.begin(manifest, badEntries, 2));
    assert(!invalid.isActive());
    return 0;
}
