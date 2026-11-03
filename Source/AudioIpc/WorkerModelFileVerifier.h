#pragma once
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include "WorkerModelConfigAbi.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace voice_studio::ipc
{
// Owns the exact file handles whose bytes were verified. Handles remain open
// for the Worker lifetime, denying write/delete sharing to prevent replacement.
struct VerifiedModelFiles
{
    HANDLE root = INVALID_HANDLE_VALUE;
    HANDLE entrypoint = INVALID_HANDLE_VALUE;
    HANDLE model = INVALID_HANDLE_VALUE;
    HANDLE index = INVALID_HANDLE_VALUE;

    VerifiedModelFiles() = default;
    VerifiedModelFiles(const VerifiedModelFiles&) = delete;
    VerifiedModelFiles& operator=(const VerifiedModelFiles&) = delete;
    ~VerifiedModelFiles() { reset(); }

    void reset() noexcept
    {
        if (index != INVALID_HANDLE_VALUE) CloseHandle(index);
        if (model != INVALID_HANDLE_VALUE) CloseHandle(model);
        if (entrypoint != INVALID_HANDLE_VALUE) CloseHandle(entrypoint);
        if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
        index = model = entrypoint = root = INVALID_HANDLE_VALUE;
    }
};

inline bool finalPath(HANDLE handle, std::wstring& output) noexcept
{
    DWORD capacity = 512;
    try
    {
        for (;;)
        {
            std::wstring buffer(capacity, L'\0');
            const DWORD length = GetFinalPathNameByHandleW(handle, buffer.data(),
                capacity, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
            if (length == 0) return false;
            if (length < capacity)
            {
                buffer.resize(length);
                output.swap(buffer);
                return true;
            }
            if (length > 32768) return false;
            capacity = length + 1;
        }
    }
    catch (...) { return false; }
}

inline bool isContainedFinalPath(const std::wstring& root,
                                 const std::wstring& candidate) noexcept
{
    if (root.empty() || candidate.size() <= root.size()) return false;
    // GetFinalPathNameByHandleW returns normalized DOS paths (usually \\?\C:\...).
    if (CompareStringOrdinal(root.data(), static_cast<int>(root.size()),
                             candidate.data(), static_cast<int>(root.size()), TRUE) != CSTR_EQUAL)
        return false;
    // A root ending in a separator already establishes the component boundary
    // (for example, a volume root such as \\\\?\\C:\\). Otherwise the next
    // candidate character must be a separator, not a sibling-name prefix.
    return root.back() == L'\\' || candidate[root.size()] == L'\\';
}

inline bool openVerifiedFile(const char16_t* path, std::uint16_t pathLength,
                             std::uint64_t expectedSize,
                             const std::uint8_t (&expectedHash)[32],
                             const std::wstring& canonicalRoot,
                             HANDLE& retained) noexcept
{
    if (path == nullptr || pathLength == 0 || pathLength >= kWorkerModelPathCapacity ||
        expectedSize == 0 || retained != INVALID_HANDLE_VALUE)
        return false;

    std::wstring filePath;
    try { filePath.assign(reinterpret_cast<const wchar_t*>(path), pathLength); }
    catch (...) { return false; }

    // No write/delete sharing: the verified file cannot be replaced while held.
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    // Deny writes through already-open compatible handles as well as new opens.
    // Share flags alone do not stop a writer that was opened before this verifier.
    OVERLAPPED fileLock{};
    if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 0xffffffffu, 0xffffffffu, &fileLock))
    {
        CloseHandle(file);
        return false;
    }

    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) ||
        (info.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        CloseHandle(file); return false;
    }

    std::wstring canonicalFile;
    if (!finalPath(file, canonicalFile) || !isContainedFinalPath(canonicalRoot, canonicalFile))
    {
        CloseHandle(file); return false;
    }

    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
        static_cast<std::uint64_t>(fileSize.QuadPart) != expectedSize)
    {
        CloseHandle(file); return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, returned = 0;
    std::vector<UCHAR> hashObject;
    bool ok = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        goto cleanup;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0 ||
        returned != sizeof(objectBytes) || objectBytes == 0)
        goto cleanup;
    try { hashObject.resize(objectBytes); } catch (...) { goto cleanup; }
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectBytes, nullptr, 0, 0) < 0)
        goto cleanup;
    {
        std::array<UCHAR, 64 * 1024> buffer{};
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                goto cleanup;
            if (read == 0) break;
            if (BCryptHashData(hash, buffer.data(), read, 0) < 0) goto cleanup;
        }
    }
    {
        std::array<UCHAR, 32> actual{};
        if (BCryptFinishHash(hash, actual.data(), static_cast<ULONG>(actual.size()), 0) < 0)
            goto cleanup;
        UCHAR difference = 0;
        for (std::size_t i = 0; i < actual.size(); ++i)
            difference |= static_cast<UCHAR>(actual[i] ^ expectedHash[i]);
        ok = difference == 0;
    }
cleanup:
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (algorithm != nullptr) BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) { CloseHandle(file); return false; }
    retained = file; // Keep the verified object open; loader must consume this handle.
    return true;
}

// Compatibility helper for isolated hash tests; production uses retained handles below.
inline bool verifyModelFile(const char16_t* path, std::uint16_t pathLength,
                            std::uint64_t expectedSize,
                            const std::uint8_t (&expectedHash)[32]) noexcept
{
    if (path == nullptr || pathLength == 0 || pathLength >= kWorkerModelPathCapacity)
        return false;
    std::wstring p;
    try { p.assign(reinterpret_cast<const wchar_t*>(path), pathLength); }
    catch (...) { return false; }
    const auto slash = p.find_last_of(L'\\');
    if (slash == std::wstring::npos) return false;
    p.resize(slash);
    HANDLE root = CreateFileW(p.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (root == INVALID_HANDLE_VALUE) return false;
    FILE_ATTRIBUTE_TAG_INFO info{};
    std::wstring canonicalRoot;
    const bool valid = GetFileInformationByHandleEx(root, FileAttributeTagInfo, &info, sizeof(info)) &&
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        finalPath(root, canonicalRoot);
    HANDLE held = INVALID_HANDLE_VALUE;
    const bool result = valid && openVerifiedFile(path, pathLength, expectedSize,
        expectedHash, canonicalRoot, held);
    if (held != INVALID_HANDLE_VALUE) CloseHandle(held);
    CloseHandle(root);
    return result;
}

inline bool verifyWorkerModelConfigFiles(const WorkerModelConfigMessage& config,
                                         VerifiedModelFiles& verified) noexcept
{
    verified.reset();
    if (!hasValidWorkerModelConfig(config)) return false;
    std::wstring rootPath;
    try { rootPath.assign(reinterpret_cast<const wchar_t*>(config.rootPath), config.rootPathLength); }
    catch (...) { return false; }

    verified.root = CreateFileW(rootPath.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (verified.root == INVALID_HANDLE_VALUE) return false;

    FILE_ATTRIBUTE_TAG_INFO rootInfo{};
    std::wstring canonicalRoot;
    if (!GetFileInformationByHandleEx(verified.root, FileAttributeTagInfo,
            &rootInfo, sizeof(rootInfo)) ||
        (rootInfo.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        (rootInfo.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        !finalPath(verified.root, canonicalRoot))
    { verified.reset(); return false; }

    if (!openVerifiedFile(config.entrypointPath, config.entrypointPathLength,
            config.entrypointSizeBytes, config.entrypointSha256, canonicalRoot, verified.entrypoint) ||
        !openVerifiedFile(config.modelPath, config.modelPathLength,
            config.modelSizeBytes, config.modelSha256, canonicalRoot, verified.model))
    { verified.reset(); return false; }

    if ((config.flags & kWorkerModelConfigFlagHasIndex) != 0 &&
        !openVerifiedFile(config.indexPath, config.indexPathLength,
            config.indexSizeBytes, config.indexSha256, canonicalRoot, verified.index))
    { verified.reset(); return false; }
    return true;
}
} // namespace voice_studio::ipc
