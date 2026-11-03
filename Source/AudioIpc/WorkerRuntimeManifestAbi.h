#pragma once
#include <cstddef>
#include <cstdint>

namespace voice_studio::ipc
{
inline constexpr std::uint32_t kWorkerRuntimeManifestMagic = 0x5653524du; // VSRM
inline constexpr std::uint16_t kWorkerRuntimeManifestVersion = 1;
inline constexpr std::size_t kWorkerRuntimeManifestMaxEntries = 512;
inline constexpr std::size_t kWorkerRuntimeManifestPathCapacity = 512;
inline constexpr std::uint64_t kWorkerRuntimeManifestMaxFileBytes = 8ull * 1024ull * 1024ull * 1024ull;
inline constexpr std::uint64_t kWorkerRuntimeManifestMaxTotalBytes = 32ull * 1024ull * 1024ull * 1024ull;

enum class WorkerRuntimeFileRole : std::uint16_t
{
    pythonSource = 1,
    checkpoint = 2,
    faissIndex = 3,
    hubertAsset = 4,
    f0Asset = 5,
    configuration = 6,
    runtimeSupport = 7
};

// Header and entries are separate bounded pipe messages. The manifest digest
// authenticates the canonical ordered entry records; transport integration
// must verify it before any staging write is accepted.
struct WorkerRuntimeManifestHeader
{
    std::uint32_t magic = kWorkerRuntimeManifestMagic;
    std::uint16_t version = kWorkerRuntimeManifestVersion;
    std::uint16_t size = static_cast<std::uint16_t>(sizeof(WorkerRuntimeManifestHeader));
    std::uint32_t entryCount = 0;
    std::uint32_t reserved = 0;
    std::uint64_t totalBytes = 0;
    std::uint8_t manifestSha256[32]{};
};

struct WorkerRuntimeManifestEntry
{
    std::uint16_t role = 0;
    std::uint16_t pathLength = 0;
    std::uint32_t reserved = 0;
    std::uint64_t sizeBytes = 0;
    std::uint8_t sha256[32]{};
    char16_t relativePath[kWorkerRuntimeManifestPathCapacity]{};
};

static_assert(sizeof(char16_t) == 2, "Runtime manifest requires UTF-16 code units");
static_assert(sizeof(WorkerRuntimeManifestHeader) <= 64, "Manifest header must remain bounded");
static_assert(sizeof(WorkerRuntimeManifestEntry) <= 1100, "Manifest entry must remain bounded");

inline bool runtimeManifestHashPresent(const std::uint8_t (&hash)[32]) noexcept
{
    std::uint8_t value = 0;
    for (const auto byte : hash) value |= byte;
    return value != 0;
}

inline char16_t runtimeManifestLower(char16_t value) noexcept
{
    return value >= u'A' && value <= u'Z'
        ? static_cast<char16_t>(value + (u'a' - u'A')) : value;
}

// Paths use normalized forward slashes only. No absolute paths, drive/UNC
// prefixes, empty components, dot segments, ADS, control chars or trailing slash.
inline bool hasSafeRuntimeRelativePath(const char16_t* path, std::uint16_t length) noexcept
{
    if (path == nullptr || length == 0 || length >= kWorkerRuntimeManifestPathCapacity)
        return false;
    if (path[0] == u'/' || path[length - 1] == u'/') return false;
    std::size_t componentStart = 0;
    for (std::size_t i = 0; i <= length; ++i)
    {
        if (i < length)
        {
            const auto ch = path[i];
            if (ch == 0 || ch < 0x20 || ch == u'\\' || ch == u':' ||
                ch == u'<' || ch == u'>' || ch == u'"' || ch == u'|' ||
                ch == u'?' || ch == u'*') return false;
            if (ch != u'/') continue;
        }
        const auto componentLength = i - componentStart;
        if (componentLength == 0 ||
            (componentLength == 1 && path[componentStart] == u'.') ||
            (componentLength == 2 && path[componentStart] == u'.' &&
             path[componentStart + 1] == u'.')) return false;
        componentStart = i + 1;
    }
    return true;
}

inline bool hasValidWorkerRuntimeManifestHeader(const WorkerRuntimeManifestHeader& header) noexcept
{
    return header.magic == kWorkerRuntimeManifestMagic &&
        header.version == kWorkerRuntimeManifestVersion &&
        header.size == sizeof(WorkerRuntimeManifestHeader) &&
        header.entryCount > 0 && header.entryCount <= kWorkerRuntimeManifestMaxEntries &&
        header.reserved == 0 && header.totalBytes > 0 &&
        header.totalBytes <= kWorkerRuntimeManifestMaxTotalBytes &&
        runtimeManifestHashPresent(header.manifestSha256);
}

inline bool hasValidWorkerRuntimeManifestEntry(const WorkerRuntimeManifestEntry& entry) noexcept
{
    const auto role = static_cast<WorkerRuntimeFileRole>(entry.role);
    const bool knownRole = role >= WorkerRuntimeFileRole::pythonSource &&
                           role <= WorkerRuntimeFileRole::runtimeSupport;
    if (!knownRole || entry.reserved != 0 || entry.sizeBytes == 0 ||
        entry.sizeBytes > kWorkerRuntimeManifestMaxFileBytes ||
        !runtimeManifestHashPresent(entry.sha256) ||
        !hasSafeRuntimeRelativePath(entry.relativePath, entry.pathLength)) return false;
    return entry.relativePath[entry.pathLength] == 0;
}

inline bool areWorkerRuntimeManifestPathsUnique(
    const WorkerRuntimeManifestEntry* entries, std::size_t count) noexcept
{
    if (entries == nullptr || count == 0 || count > kWorkerRuntimeManifestMaxEntries)
        return false;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (!hasValidWorkerRuntimeManifestEntry(entries[i])) return false;
        for (std::size_t j = 0; j < i; ++j)
        {
            if (entries[i].pathLength != entries[j].pathLength) continue;
            bool equal = true;
            for (std::size_t k = 0; k < entries[i].pathLength; ++k)
                if (runtimeManifestLower(entries[i].relativePath[k]) !=
                    runtimeManifestLower(entries[j].relativePath[k]))
                { equal = false; break; }
            if (equal) return false;
        }
    }
    return true;
}
} // namespace voice_studio::ipc
