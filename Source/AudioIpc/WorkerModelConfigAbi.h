#pragma once
#include <cstddef>
#include <cstdint>
namespace voice_studio::ipc {
inline constexpr std::uint32_t kWorkerModelConfigMagic = 0x56534d31u;
inline constexpr std::uint16_t kWorkerModelConfigVersion = 1;
inline constexpr std::uint32_t kWorkerModelConfigManifestVersion = 1;
inline constexpr std::size_t kWorkerModelPathCapacity = 1024;
inline constexpr std::uint64_t kWorkerModelMaxEntrypointBytes = 64ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kWorkerModelMaxDataFileBytes = 8ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint32_t kWorkerModelConfigFlagSourceTrusted = 1u << 0;
inline constexpr std::uint32_t kWorkerModelConfigFlagVoiceRightsConfirmed = 1u << 1;
inline constexpr std::uint32_t kWorkerModelConfigFlagHasIndex = 1u << 2;
inline constexpr std::uint32_t kWorkerModelConfigKnownFlags = 7u;
// Pointer-free fixed-size record used by the bounded model-selection IPC ABI.
struct WorkerModelConfigMessage {
    std::uint32_t magic = kWorkerModelConfigMagic;
    std::uint16_t version = kWorkerModelConfigVersion;
    std::uint16_t size = 0;
    std::uint32_t manifestVersion = kWorkerModelConfigManifestVersion;
    std::uint32_t flags = 0;
    std::uint64_t generation = 0;
    std::uint64_t requestId = 0;
    std::uint64_t entrypointSizeBytes = 0;
    std::uint64_t modelSizeBytes = 0;
    std::uint64_t indexSizeBytes = 0;
    std::uint8_t entrypointSha256[32]{};
    std::uint8_t modelSha256[32]{};
    std::uint8_t indexSha256[32]{};
    std::uint16_t rootPathLength = 0;
    std::uint16_t entrypointPathLength = 0;
    std::uint16_t modelPathLength = 0;
    std::uint16_t indexPathLength = 0;
    std::uint32_t reserved = 0;
    char16_t rootPath[kWorkerModelPathCapacity]{};
    char16_t entrypointPath[kWorkerModelPathCapacity]{};
    char16_t modelPath[kWorkerModelPathCapacity]{};
    char16_t indexPath[kWorkerModelPathCapacity]{};
};
static_assert(sizeof(char16_t) == 2, "Model config ABI requires UTF-16 code units");
static_assert(sizeof(WorkerModelConfigMessage) <= 9000, "Model config exceeds bounded pipe message limit");
inline bool modelConfigHashPresent(const std::uint8_t (&hash)[32]) noexcept {
    std::uint8_t aggregate = 0;
    for (const auto byte : hash) aggregate |= byte;
    return aggregate != 0;
}
inline char16_t modelConfigLower(char16_t value) noexcept {
    return value >= u'A' && value <= u'Z' ? static_cast<char16_t>(value + (u'a' - u'A')) : value;
}
inline bool hasSafeLocalAbsoluteWindowsPath(const char16_t* path, std::uint16_t length) noexcept {
    if (path == nullptr || length < 3 || length >= kWorkerModelPathCapacity) return false;
    if (!((path[0] >= u'A' && path[0] <= u'Z') || (path[0] >= u'a' && path[0] <= u'z')) ||
        path[1] != u':' || path[2] != u'\\') return false;
    for (std::size_t i = 0; i < length; ++i) {
        const auto ch = path[i];
        if (ch == 0 || ch < 0x20 || ch == u'/' || ch == u'<' || ch == u'>' ||
            ch == u'"' || ch == u'|' || ch == u'?' || ch == u'*') return false;
        if (ch == u':' && i != 1) return false;
        if (ch == u'\\' && i > 2) {
            if (path[i - 1] == u'\\') return false;
            std::size_t start = i;
            while (start > 0 && path[start - 1] != u'\\') --start;
            const auto n = i - start;
            if ((n == 1 && path[start] == u'.') ||
                (n == 2 && path[start] == u'.' && path[start + 1] == u'.')) return false;
        }
    }
    std::size_t finalStart = length;
    while (finalStart > 0 && path[finalStart - 1] != u'\\') --finalStart;
    const auto finalLength = static_cast<std::size_t>(length) - finalStart;
    if ((finalLength == 1 && path[finalStart] == u'.') ||
        (finalLength == 2 && path[finalStart] == u'.' && path[finalStart + 1] == u'.')) return false;
    return path[length - 1] != u'\\' && path[length] == 0;
}
inline bool isModelPathWithinRoot(const char16_t* root, std::uint16_t rootLength,
                                  const char16_t* path, std::uint16_t pathLength) noexcept {
    if (root == nullptr || path == nullptr || rootLength < 3 ||
        pathLength <= rootLength || root[rootLength - 1] == u'\\') return false;
    for (std::size_t i = 0; i < rootLength; ++i)
        if (modelConfigLower(root[i]) != modelConfigLower(path[i])) return false;
    return path[rootLength] == u'\\';
}
inline bool hasExpectedRvcEntrypoint(const WorkerModelConfigMessage& m) noexcept {
    constexpr char16_t suffix[] = u"\\RVCRealtimeVST\\worker\\rvc_worker.py";
    constexpr std::size_t n = sizeof(suffix) / sizeof(suffix[0]) - 1;
    const auto expected = static_cast<std::size_t>(m.rootPathLength) + n;
    if (expected != m.entrypointPathLength || expected >= kWorkerModelPathCapacity) return false;
    for (std::size_t i = 0; i < m.rootPathLength; ++i)
        if (modelConfigLower(m.rootPath[i]) != modelConfigLower(m.entrypointPath[i])) return false;
    for (std::size_t i = 0; i < n; ++i)
        if (modelConfigLower(suffix[i]) != modelConfigLower(m.entrypointPath[m.rootPathLength + i])) return false;
    return true;
}
inline bool hasModelConfigExtension(const char16_t* path, std::uint16_t length,
                                    const char16_t* ext, std::size_t extLength) noexcept {
    if (length <= extLength) return false;
    const auto offset = static_cast<std::size_t>(length) - extLength;
    for (std::size_t i = 0; i < extLength; ++i)
        if (modelConfigLower(path[offset + i]) != modelConfigLower(ext[i])) return false;
    return true;
}
inline bool matchesWorkerModelConfigRequest(const WorkerModelConfigMessage& m,
                                            std::uint64_t generation,
                                            std::uint64_t requestId) noexcept {
    return generation != 0 && requestId != 0 &&
           m.generation == generation && m.requestId == requestId;
}
inline bool hasValidWorkerModelConfig(const WorkerModelConfigMessage& m) noexcept {
    if (m.magic != kWorkerModelConfigMagic || m.version != kWorkerModelConfigVersion ||
        m.size != sizeof(WorkerModelConfigMessage) ||
        m.manifestVersion != kWorkerModelConfigManifestVersion || m.generation == 0 ||
        m.requestId == 0 || m.reserved != 0 || (m.flags & ~kWorkerModelConfigKnownFlags) != 0 ||
        (m.flags & (kWorkerModelConfigFlagSourceTrusted | kWorkerModelConfigFlagVoiceRightsConfirmed)) !=
            (kWorkerModelConfigFlagSourceTrusted | kWorkerModelConfigFlagVoiceRightsConfirmed) ||
        m.entrypointSizeBytes == 0 || m.entrypointSizeBytes > kWorkerModelMaxEntrypointBytes ||
        m.modelSizeBytes == 0 || m.modelSizeBytes > kWorkerModelMaxDataFileBytes ||
        !modelConfigHashPresent(m.entrypointSha256) || !modelConfigHashPresent(m.modelSha256)) return false;
    if (!hasSafeLocalAbsoluteWindowsPath(m.rootPath, m.rootPathLength) ||
        !hasSafeLocalAbsoluteWindowsPath(m.entrypointPath, m.entrypointPathLength) ||
        !hasSafeLocalAbsoluteWindowsPath(m.modelPath, m.modelPathLength) ||
        !hasExpectedRvcEntrypoint(m) ||
        !isModelPathWithinRoot(m.rootPath, m.rootPathLength, m.entrypointPath, m.entrypointPathLength) ||
        !isModelPathWithinRoot(m.rootPath, m.rootPathLength, m.modelPath, m.modelPathLength) ||
        !hasModelConfigExtension(m.modelPath, m.modelPathLength, u".pth", 4)) return false;
    const bool hasIndex = (m.flags & kWorkerModelConfigFlagHasIndex) != 0;
    if (hasIndex) {
        if (!hasSafeLocalAbsoluteWindowsPath(m.indexPath, m.indexPathLength) ||
            !isModelPathWithinRoot(m.rootPath, m.rootPathLength, m.indexPath, m.indexPathLength) ||
            !hasModelConfigExtension(m.indexPath, m.indexPathLength, u".index", 6) ||
            m.indexSizeBytes == 0 || m.indexSizeBytes > kWorkerModelMaxDataFileBytes ||
            !modelConfigHashPresent(m.indexSha256)) return false;
    } else {
        if (m.indexPathLength != 0 || m.indexSizeBytes != 0 || m.indexPath[0] != 0) return false;
        for (const auto byte : m.indexSha256) if (byte != 0) return false;
    }
    return true;
}
} // namespace voice_studio::ipc
