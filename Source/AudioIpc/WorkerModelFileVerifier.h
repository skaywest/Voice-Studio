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
struct VerifiedModelFiles
{
    bool entrypoint = false;
    bool model = false;
    bool index = false;
};

inline bool hasNoReparsePathComponents(const std::wstring& path, bool finalIsDirectory) noexcept
{
    if (path.size() < 3 || path[1] != L':' || path[2] != L'\\')
        return false;

    // Check the drive root, each directory component, and the final object.
    // FILE_FLAG_OPEN_REPARSE_POINT makes the check inspect the link itself
    // instead of silently following a junction/symlink to an external target.
    std::size_t componentEnd = 3;
    while (componentEnd <= path.size())
    {
        const bool final = componentEnd == path.size();
        if (!final && path[componentEnd] != L'\\')
        {
            ++componentEnd;
            continue;
        }

        const std::wstring component = path.substr(0, componentEnd);
        const DWORD flags = FILE_FLAG_OPEN_REPARSE_POINT |
                            ((final && !finalIsDirectory) ? 0 : FILE_FLAG_BACKUP_SEMANTICS);
        HANDLE handle = CreateFileW(component.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, flags, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;

        FILE_ATTRIBUTE_TAG_INFO info{};
        const bool queried = GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
            &info, sizeof(info)) != FALSE;
        CloseHandle(handle);
        if (!queried || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return false;
        if (!final && (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0)
            return false;

        if (final)
            break;
        ++componentEnd;
    }
    return true;
}

inline bool verifyModelFile(const char16_t* path, std::uint16_t pathLength,
                            std::uint64_t expectedSize,
                            const std::uint8_t (&expectedHash)[32]) noexcept
{
    if (path == nullptr || pathLength == 0 || pathLength >= kWorkerModelPathCapacity ||
        expectedSize == 0)
        return false;

    std::wstring filePath;
    try { filePath.assign(reinterpret_cast<const wchar_t*>(path), pathLength); }
    catch (...) { return false; }
    if (!hasNoReparsePathComponents(filePath, false))
        return false;

    // Deny write/delete sharing while hashing so the bytes cannot be replaced
    // through another handle during this verification pass.
    HANDLE file = CreateFileW(filePath.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL |
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    FILE_ATTRIBUTE_TAG_INFO fileInfo{};
    if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &fileInfo, sizeof(fileInfo)) ||
        (fileInfo.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0)
    {
        CloseHandle(file);
        return false;
    }

    bool ok = false;
    LARGE_INTEGER fileSize{};
    if (!GetFileSizeEx(file, &fileSize) || fileSize.QuadPart < 0 ||
        static_cast<std::uint64_t>(fileSize.QuadPart) != expectedSize)
    {
        CloseHandle(file);
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0, returned = 0;
    std::vector<UCHAR> hashObject;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        goto cleanup;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &returned, 0) < 0 || returned != sizeof(objectBytes) || objectBytes == 0)
        goto cleanup;
    try { hashObject.resize(objectBytes); }
    catch (...) { goto cleanup; }
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectBytes, nullptr, 0, 0) < 0)
        goto cleanup;

    {
        std::array<UCHAR, 64 * 1024> buffer{};
        for (;;)
        {
            DWORD bytesRead = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr))
                goto cleanup;
            if (bytesRead == 0)
                break;
            if (BCryptHashData(hash, buffer.data(), bytesRead, 0) < 0)
                goto cleanup;
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
    CloseHandle(file);
    return ok;
}

inline bool verifyWorkerModelConfigFiles(const WorkerModelConfigMessage& config) noexcept
{
    if (!hasValidWorkerModelConfig(config))
        return false;

    std::wstring root;
    try { root.assign(reinterpret_cast<const wchar_t*>(config.rootPath), config.rootPathLength); }
    catch (...) { return false; }
    if (!hasNoReparsePathComponents(root, true))
        return false;

    // Reopen each file by its explicit local path and verify both exact size
    // and SHA-256 immediately before any future loader is permitted to use it.
    if (!verifyModelFile(config.entrypointPath, config.entrypointPathLength,
                         config.entrypointSizeBytes, config.entrypointSha256) ||
        !verifyModelFile(config.modelPath, config.modelPathLength,
                         config.modelSizeBytes, config.modelSha256))
        return false;

    if ((config.flags & kWorkerModelConfigFlagHasIndex) != 0 &&
        !verifyModelFile(config.indexPath, config.indexPathLength,
                         config.indexSizeBytes, config.indexSha256))
        return false;
    return true;
}
} // namespace voice_studio::ipc
