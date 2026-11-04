#include "AudioIpc/WorkerRuntimeManifestVerifier.h"
#ifdef NDEBUG
 #undef NDEBUG
#endif
#include <cassert>
#include <cstddef>
#include <cstdint>
using namespace voice_studio::ipc;
static void setPath(WorkerRuntimeManifestEntry& e,const char16_t* s)
{ std::size_t n=0; while(s[n]) ++n; assert(n<kWorkerRuntimeManifestPathCapacity);
  for(std::size_t i=0;i<=n;++i)e.relativePath[i]=s[i]; e.pathLength=static_cast<std::uint16_t>(n); }
static WorkerRuntimeManifestEntry makeEntry(const char16_t* p,WorkerRuntimeFileRole role,std::uint8_t hb)
{ WorkerRuntimeManifestEntry e{}; e.role=static_cast<std::uint16_t>(role); e.sizeBytes=42; e.sha256[0]=hb; setPath(e,p); return e; }
int main()
{
    WorkerRuntimeManifestHeader h{}; h.size=static_cast<std::uint16_t>(sizeof(h));
    h.entryCount=2; h.totalBytes=84; h.manifestSha256[0]=1;
    WorkerRuntimeManifestEntry entries[2]={
        makeEntry(u"runtime/infer.py",WorkerRuntimeFileRole::pythonSource,0x11),
        makeEntry(u"models/voice.pth",WorkerRuntimeFileRole::checkpoint,0x22)};
    std::uint8_t digest[32]{};
    assert(computeWorkerRuntimeManifestSha256(h,entries,2,digest));
    for(std::size_t i=0;i<32;++i) h.manifestSha256[i]=digest[i];
    assert(hasValidWorkerRuntimeManifestDigest(h,entries,2));

    WorkerRuntimeManifestEntry tampered[2]={entries[0],entries[1]};
    tampered[0].role=static_cast<std::uint16_t>(WorkerRuntimeFileRole::runtimeSupport);
    assert(!hasValidWorkerRuntimeManifestDigest(h,tampered,2));
    tampered[0]=entries[0]; ++tampered[0].sizeBytes;
    assert(!hasValidWorkerRuntimeManifestDigest(h,tampered,2));
    tampered[0]=entries[0]; tampered[0].relativePath[0]=u'R';
    assert(!hasValidWorkerRuntimeManifestDigest(h,tampered,2));
    tampered[0]=entries[0]; tampered[0].sha256[0]^=0xff;
    assert(!hasValidWorkerRuntimeManifestDigest(h,tampered,2));
    tampered[0]=entries[1]; tampered[1]=entries[0];
    assert(!hasValidWorkerRuntimeManifestDigest(h,tampered,2));
    auto bad=h; ++bad.totalBytes;
    assert(!hasValidWorkerRuntimeManifestDigest(bad,entries,2));
    bad=h; bad.manifestSha256[0]^=0x80;
    assert(!hasValidWorkerRuntimeManifestDigest(bad,entries,2));
    return 0;
}
