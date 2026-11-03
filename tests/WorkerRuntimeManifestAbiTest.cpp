#include "AudioIpc/WorkerRuntimeManifestAbi.h"
#include <cassert>
#include <cstdint>
#include <cstddef>
using namespace voice_studio::ipc;

static void setPath(WorkerRuntimeManifestEntry& e, const char16_t* value)
{
    std::size_t n = 0;
    while (value[n] != 0) ++n;
    assert(n < kWorkerRuntimeManifestPathCapacity);
    for (std::size_t i = 0; i <= n; ++i) e.relativePath[i] = value[i];
    e.pathLength = static_cast<std::uint16_t>(n);
}
static WorkerRuntimeManifestEntry validEntry(const char16_t* path, WorkerRuntimeFileRole role)
{
    WorkerRuntimeManifestEntry e{};
    e.role = static_cast<std::uint16_t>(role);
    e.sizeBytes = 42;
    e.sha256[0] = 1;
    setPath(e, path);
    return e;
}
int main()
{
    WorkerRuntimeManifestHeader h{};
    h.size = static_cast<std::uint16_t>(sizeof(WorkerRuntimeManifestHeader));
    h.entryCount = 2;
    h.totalBytes = 84;
    h.manifestSha256[0] = 1;
    assert(hasValidWorkerRuntimeManifestHeader(h));
    auto badHeader = h; badHeader.entryCount = 0;
    assert(!hasValidWorkerRuntimeManifestHeader(badHeader));
    badHeader = h; badHeader.entryCount = kWorkerRuntimeManifestMaxEntries + 1;
    assert(!hasValidWorkerRuntimeManifestHeader(badHeader));
    badHeader = h; badHeader.reserved = 1;
    assert(!hasValidWorkerRuntimeManifestHeader(badHeader));
    badHeader = h; badHeader.manifestSha256[0] = 0;
    assert(!hasValidWorkerRuntimeManifestHeader(badHeader));
    badHeader = h; badHeader.totalBytes = kWorkerRuntimeManifestMaxTotalBytes + 1;
    assert(!hasValidWorkerRuntimeManifestHeader(badHeader));

    WorkerRuntimeManifestEntry entries[2] = {
        validEntry(u"rvc/infer/lib/infer.py", WorkerRuntimeFileRole::pythonSource),
        validEntry(u"models/voice.pth", WorkerRuntimeFileRole::checkpoint)
    };
    assert(hasValidWorkerRuntimeManifestEntry(entries[0]));
    assert(areWorkerRuntimeManifestPathsUnique(entries, 2));
    auto invalid = entries[0]; setPath(invalid, u"../escape.py");
    assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"rvc//infer.py"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"rvc/./infer.py"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"rvc/../infer.py"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"C:/temp/model.pth"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"\\\\server/share/model.pth"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"rvc\\infer.py"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"rvc/model.pth:stream"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    setPath(invalid, u"rvc/infer.py/"); assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    invalid = entries[0]; invalid.role = 0; assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    invalid = entries[0]; invalid.reserved = 1; assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    invalid = entries[0]; invalid.sha256[0] = 0; assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    invalid = entries[0]; invalid.sizeBytes = 0; assert(!hasValidWorkerRuntimeManifestEntry(invalid));
    invalid = entries[0]; invalid.sizeBytes = kWorkerRuntimeManifestMaxFileBytes + 1;
    assert(!hasValidWorkerRuntimeManifestEntry(invalid));

    WorkerRuntimeManifestEntry duplicate[2] = { entries[0], entries[0] };
    assert(!areWorkerRuntimeManifestPathsUnique(duplicate, 2));
    setPath(duplicate[1], u"RVC/INFER/LIB/INFER.PY");
    assert(!areWorkerRuntimeManifestPathsUnique(duplicate, 2));
    assert(!areWorkerRuntimeManifestPathsUnique(nullptr, 1));
    assert(!areWorkerRuntimeManifestPathsUnique(entries, kWorkerRuntimeManifestMaxEntries + 1));
    return 0;
}
