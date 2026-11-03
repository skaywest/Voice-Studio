#include "AudioIpc/WorkerModelConfigAbi.h"
#include <cassert>
#include <cstddef>
using namespace voice_studio::ipc;
static void setPath(char16_t* dst, std::uint16_t& length, const char16_t* src) {
    std::size_t n = 0; while (src[n] != 0) ++n;
    assert(n < kWorkerModelPathCapacity);
    for (std::size_t i = 0; i <= n; ++i) dst[i] = src[i];
    length = static_cast<std::uint16_t>(n);
}
static WorkerModelConfigMessage validConfig() {
    WorkerModelConfigMessage m{};
    m.size = static_cast<std::uint16_t>(sizeof(m)); m.generation = 4; m.requestId = 9;
    m.flags = kWorkerModelConfigFlagSourceTrusted | kWorkerModelConfigFlagVoiceRightsConfirmed;
    setPath(m.rootPath, m.rootPathLength, u"C:\\RVC");
    setPath(m.entrypointPath, m.entrypointPathLength, u"C:\\RVC\\RVCRealtimeVST\\worker\\rvc_worker.py");
    setPath(m.modelPath, m.modelPathLength, u"D:\\Voices\\voice.pth");
    m.entrypointSizeBytes = 100; m.modelSizeBytes = 200;
    m.entrypointSha256[0] = 1; m.modelSha256[0] = 2;
    return m;
}
int main() {
    const auto m = validConfig(); assert(hasValidWorkerModelConfig(m));
    auto x = m; x.generation = 0; assert(!hasValidWorkerModelConfig(x));
    x = m; x.flags &= ~kWorkerModelConfigFlagVoiceRightsConfirmed; assert(!hasValidWorkerModelConfig(x));
    x = m; x.entrypointSha256[0] = 0; assert(!hasValidWorkerModelConfig(x));
    x = m; x.entrypointPath[m.entrypointPathLength - 1] = u'x'; assert(!hasValidWorkerModelConfig(x));
    x = m; setPath(x.modelPath, x.modelPathLength, u"D:\\Voices\\..\\voice.pth"); assert(!hasValidWorkerModelConfig(x));
    x = m; setPath(x.modelPath, x.modelPathLength, u"\\\\server\\share\\voice.pth"); assert(!hasValidWorkerModelConfig(x));
    x = m; setPath(x.modelPath, x.modelPathLength, u"D:\\Voices\\voice.exe"); assert(!hasValidWorkerModelConfig(x));
    x = m; x.indexPathLength = 1; assert(!hasValidWorkerModelConfig(x));
    x = m; x.flags |= kWorkerModelConfigFlagHasIndex; assert(!hasValidWorkerModelConfig(x));
    x = m; x.flags |= 0x80000000u; assert(!hasValidWorkerModelConfig(x));
    auto withIndex = m; withIndex.flags |= kWorkerModelConfigFlagHasIndex;
    setPath(withIndex.indexPath, withIndex.indexPathLength, u"D:\\Voices\\voice.index");
    withIndex.indexSizeBytes = 32; withIndex.indexSha256[0] = 3;
    assert(hasValidWorkerModelConfig(withIndex));
    return 0;
}
