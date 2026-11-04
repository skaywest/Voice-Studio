#include "AudioIpc/WindowsWorkerRuntimeStaging.h"

#include <windows.h>

#include <cstdint>
#include <string>

using namespace voice_studio::ipc;

static void setPath(WorkerRuntimeManifestEntry& entry, const char16_t* path)
{
    std::size_t length = 0;
    while (path[length] != 0) ++length;
    entry.pathLength = static_cast<std::uint16_t>(length);
    for (std::size_t i = 0; i <= length; ++i)
        entry.relativePath[i] = path[i];
}

static void setAbcSha256(std::uint8_t (&hash)[32])
{
    const std::uint8_t value[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
        0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
        0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    for (std::size_t i = 0; i < 32; ++i) hash[i] = value[i];
}

static bool makeManifest(WorkerRuntimeManifestHeader& header,
                         WorkerRuntimeManifestEntry& entry,
                         bool correctFileHash)
{
    header = {};
    header.size = static_cast<std::uint16_t>(sizeof(header));
    header.entryCount = 1;
    header.totalBytes = 3;
    entry = {};
    entry.role = static_cast<std::uint16_t>(WorkerRuntimeFileRole::runtimeSupport);
    entry.sizeBytes = 3;
    setPath(entry, u"worker/main.py");
    setAbcSha256(entry.sha256);
    if (!correctFileHash) entry.sha256[0] ^= 0x80u;
    return computeWorkerRuntimeManifestSha256(header, &entry, 1, header.manifestSha256);
}

static WorkerRuntimeFileChunkHeader finalChunk()
{
    WorkerRuntimeFileChunkHeader chunk{};
    chunk.entryIndex = 0;
    chunk.payloadBytes = 3;
    chunk.offsetBytes = 0;
    chunk.flags = kWorkerRuntimeFileChunkFlagFinal;
    chunk.size = static_cast<std::uint16_t>(sizeof(chunk));
    return chunk;
}

int wmain()
{
    WorkerRuntimeManifestHeader header{};
    WorkerRuntimeManifestEntry entry{};
    if (!makeManifest(header, entry, true))
        return 1;

    WindowsWorkerRuntimeStaging staging;
    if (!staging.begin(header, &entry, 1))
        return 2;
    const std::wstring root = staging.rootPath();
    const std::uint8_t abc[3] = { 'a', 'b', 'c' };
    if (!staging.writeChunk(finalChunk(), abc) ||
        !staging.isComplete() || staging.verifiedFileCount() != 1)
        return 3;

    const std::wstring stagedFile = root + L"\\worker\\main.py";
    const DWORD attributes = GetFileAttributesW(stagedFile.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return 4;
    // A handle that denies FILE_SHARE_DELETE simulates a cleanup failure.
    // UNLOAD must not acknowledge READY while the staged tree remains.
    HANDLE blocker = CreateFileW(stagedFile.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (blocker == INVALID_HANDLE_VALUE)
        return 12;
    if (staging.cleanup())
        return 13;
    if (GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES)
        return 14;
    if (!CloseHandle(blocker))
        return 15;
    if (!staging.cleanup())
        return 16;
    if (GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES)
        return 5;
    // The same staging object can start a fresh transfer after UNLOAD cleanup.
    if (!staging.begin(header, &entry, 1))
        return 11;
    if (!staging.cleanup())
        return 17;

    // A self-consistent manifest with a false file digest must not be staged
    // as complete; failure also removes the partial tree.
    WorkerRuntimeManifestHeader wrongHeader{};
    WorkerRuntimeManifestEntry wrongEntry{};
    if (!makeManifest(wrongHeader, wrongEntry, false))
        return 6;
    WindowsWorkerRuntimeStaging wrongDigest;
    if (!wrongDigest.begin(wrongHeader, &wrongEntry, 1))
        return 7;
    const std::wstring wrongRoot = wrongDigest.rootPath();
    if (wrongDigest.writeChunk(finalChunk(), abc) ||
        GetFileAttributesW(wrongRoot.c_str()) != INVALID_FILE_ATTRIBUTES)
        return 8;

    // Traversal is rejected by the manifest ABI before a staging directory is created.
    WorkerRuntimeManifestHeader unsafeHeader{};
    WorkerRuntimeManifestEntry unsafeEntry{};
    unsafeHeader.size = static_cast<std::uint16_t>(sizeof(unsafeHeader));
    unsafeHeader.entryCount = 1;
    unsafeHeader.totalBytes = 3;
    unsafeEntry.role = static_cast<std::uint16_t>(WorkerRuntimeFileRole::runtimeSupport);
    unsafeEntry.sizeBytes = 3;
    setPath(unsafeEntry, u"../escape.bin");
    setAbcSha256(unsafeEntry.sha256);
    if (computeWorkerRuntimeManifestSha256(
            unsafeHeader, &unsafeEntry, 1, unsafeHeader.manifestSha256))
        return 9;
    WindowsWorkerRuntimeStaging traversal;
    if (traversal.begin(unsafeHeader, &unsafeEntry, 1))
        return 10;
    return 0;
}
