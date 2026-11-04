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
    std::uint16_t size = 0;
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

// Staged names are deliberately restricted to portable ASCII. This avoids
// Unicode case-folding aliases and Windows filename normalization surprises.
// Components also reject DOS device names and trailing dots/spaces.
inline bool isReservedRuntimePathComponent(const char16_t* path,
                                           std::size_t start,
                                           std::size_t length) noexcept
{
    std::size_t baseLength = 0;
    while (baseLength < length && path[start + baseLength] != u'.') ++baseLength;
    if (baseLength == 3)
    {
        const auto a = runtimeManifestLower(path[start]);
        const auto b = runtimeManifestLower(path[start + 1]);
        const auto c = runtimeManifestLower(path[start + 2]);
        if ((a == u'c' && b == u'o' && c == u'n') ||
            (a == u'p' && b == u'r' && c == u'n') ||
            (a == u'a' && b == u'u' && c == u'x') ||
            (a == u'n' && b == u'u' && c == u'l')) return true;
    }
    if (baseLength == 4)
    {
        const auto a = runtimeManifestLower(path[start]);
        const auto b = runtimeManifestLower(path[start + 1]);
        const auto c = runtimeManifestLower(path[start + 2]);
        const auto d = runtimeManifestLower(path[start + 3]);
        const bool prefix = (a == u'c' && b == u'o' && c == u'm') ||
                            (a == u'l' && b == u'p' && c == u't');
        if (prefix && d >= u'1' && d <= u'9') return true;
    }
    return false;
}

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
            const bool asciiAlpha = (ch >= u'A' && ch <= u'Z') ||
                                    (ch >= u'a' && ch <= u'z');
            const bool asciiDigit = ch >= u'0' && ch <= u'9';
            if (!(asciiAlpha || asciiDigit || ch == u'_' || ch == u'-' ||
                  ch == u'.' || ch == u'/')) return false;
            if (ch != u'/') continue;
        }
        const auto componentLength = i - componentStart;
        if (componentLength == 0 ||
            (componentLength == 1 && path[componentStart] == u'.') ||
            (componentLength == 2 && path[componentStart] == u'.' &&
             path[componentStart + 1] == u'.') ||
            path[i - 1] == u'.' ||
            isReservedRuntimePathComponent(path, componentStart, componentLength))
            return false;
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

inline bool hasValidWorkerRuntimeManifestEntries(
    const WorkerRuntimeManifestHeader& header,
    const WorkerRuntimeManifestEntry* entries,
    std::size_t count) noexcept
{
    if (!hasValidWorkerRuntimeManifestHeader(header) || entries == nullptr ||
        count != header.entryCount || !areWorkerRuntimeManifestPathsUnique(entries, count))
        return false;
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (entries[i].sizeBytes > kWorkerRuntimeManifestMaxTotalBytes - total)
            return false;
        total += entries[i].sizeBytes;
    }
    return total == header.totalBytes;
}
