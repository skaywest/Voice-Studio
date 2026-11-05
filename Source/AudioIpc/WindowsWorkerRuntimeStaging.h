#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include "WorkerRuntimeTransferSequence.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace voice_studio::ipc
{
// Worker-owned, local-only staging sink. It accepts data only after the
// manifest digest has been verified and verifies each completed file against
// the manifest before reporting completion. It is not a same-user malware
// isolation boundary; the caller must keep the Worker session private.
class WindowsWorkerRuntimeStaging final
{
public:
    WindowsWorkerRuntimeStaging() = default;
    WindowsWorkerRuntimeStaging(const WindowsWorkerRuntimeStaging&) = delete;
    WindowsWorkerRuntimeStaging& operator=(const WindowsWorkerRuntimeStaging&) = delete;
    ~WindowsWorkerRuntimeStaging() { cleanup(); }

    bool begin(const WorkerRuntimeManifestHeader& header,
               const WorkerRuntimeManifestEntry* entries,
               std::size_t count) noexcept
    {
        if (active || !root.empty() || !createdFiles.empty() ||
            !createdDirectories.empty() || entries == nullptr ||
            !hasValidWorkerRuntimeManifestDigest(header, entries, count))
            return false;
        try
        {
            manifestEntries.assign(entries, entries + count);
            createdFiles.reserve(count);
            createdDirectories.reserve(count * 8);
            std::size_t directoryHandleCapacity = 0;
            for (std::size_t i = 0; i < count; ++i)
            {
                const auto length = static_cast<std::size_t>(entries[i].pathLength);
                if (length > static_cast<std::size_t>(-1) - directoryHandleCapacity)
                    return false;
                directoryHandleCapacity += length;
            }
            outstandingDirectoryHandles.reserve(directoryHandleCapacity);
        }
        catch (...) { manifestEntries.clear(); return false; }

        if (!createPrivateRoot())
        {
            manifestEntries.clear();
            return false;
        }
        if (!sequence.begin(header, manifestEntries.data(), manifestEntries.size()))
        {
            cleanup();
            return false;
        }
        active = true;
        return true;
    }

    bool writeChunk(const WorkerRuntimeFileChunkHeader& chunk,
                    const void* payload) noexcept
    {
        if (!active)
            return false;
        if (payload == nullptr || sequence.isComplete())
            return fail();
        const auto index = sequence.currentEntryIndex();
        if (index >= manifestEntries.size() || chunk.entryIndex != index)
            return fail();
        if (file == INVALID_HANDLE_VALUE && !openCurrentFile())
            return fail();
        if (!sequence.accept(chunk))
            return fail();

        DWORD written = 0;
        if (!WriteFile(file, payload, chunk.payloadBytes, &written, nullptr) ||
            written != chunk.payloadBytes)
            return fail();

        if (sequence.currentEntryIndex() != index)
        {
            if (!verifyAndCloseCurrentFile())
                return fail();
            ++verifiedFiles;
        }
        return true;
    }

    bool isComplete() const noexcept
    {
        return active && sequence.isComplete() &&
               verifiedFiles == manifestEntries.size() &&
               file == INVALID_HANDLE_VALUE;
    }

    const std::wstring& rootPath() const noexcept { return root; }
    std::size_t verifiedFileCount() const noexcept { return verifiedFiles; }

    bool cleanup() noexcept
    {
        bool handlesClosed = true;
        for (std::size_t i = outstandingDirectoryHandles.size(); i > 0; --i)
        {
            HANDLE directory = outstandingDirectoryHandles[i - 1];
            if (CloseHandle(directory))
                outstandingDirectoryHandles.erase(outstandingDirectoryHandles.begin() +
                    static_cast<std::ptrdiff_t>(i - 1));
            else
                handlesClosed = false;
        }
        if (!handlesClosed)
            return false;

        if (file != INVALID_HANDLE_VALUE)
        {
            if (!CloseHandle(file))
                return false;
            file = INVALID_HANDLE_VALUE;
        }

        bool removedAll = true;
        for (std::size_t i = createdFiles.size(); i > 0; --i)
        {
            const auto& path = createdFiles[i - 1];
            if (DeleteFileW(path.c_str()))
                createdFiles.erase(createdFiles.begin() + static_cast<std::ptrdiff_t>(i - 1));
            else
            {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    createdFiles.erase(createdFiles.begin() + static_cast<std::ptrdiff_t>(i - 1));
                else
                    removedAll = false;
            }
        }
        for (std::size_t i = createdDirectories.size(); i > 0; --i)
        {
            const auto& path = createdDirectories[i - 1];
            if (RemoveDirectoryW(path.c_str()))
                createdDirectories.erase(createdDirectories.begin() + static_cast<std::ptrdiff_t>(i - 1));
            else
            {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    createdDirectories.erase(createdDirectories.begin() + static_cast<std::ptrdiff_t>(i - 1));
                else
                    removedAll = false;
            }
        }
        if (!root.empty())
        {
            if (RemoveDirectoryW(root.c_str()))
                root.clear();
            else
            {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                    root.clear();
                else
                    removedAll = false;
            }
        }

        manifestEntries.clear();
        sequence = WorkerRuntimeTransferSequence{};
        active = false;
        verifiedFiles = 0;
        currentFilePath.clear();
        return removedAll && root.empty() && createdFiles.empty() && createdDirectories.empty();
    }

private:
    bool fail() noexcept
    {
        cleanup();
        return false;
    }

    bool createPrivateRoot() noexcept
    {
        wchar_t temp[MAX_PATH + 1]{};
        const DWORD length = GetTempPathW(MAX_PATH, temp);
        if (length == 0 || length >= MAX_PATH)
            return false;
        try
        {
            static volatile LONG counter = 0;
            for (unsigned attempt = 0; attempt < 32; ++attempt)
            {
                const LONG serial = InterlockedIncrement(&counter);
                std::wstring candidate(temp, length);
                candidate += L"VoiceStudioRuntime-";
                candidate += std::to_wstring(GetCurrentProcessId());
                candidate += L"-";
                candidate += std::to_wstring(GetTickCount64());
                candidate += L"-";
                candidate += std::to_wstring(serial);
                // Do not inherit broad permissions from the shared temp
                // directory. Keep the staging tree protected and inheritable:
                // SYSTEM and the directory owner (the Worker identity) only.
                PSECURITY_DESCRIPTOR descriptor = nullptr;
                if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                        L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;OW)",
                        SDDL_REVISION_1, &descriptor, nullptr))
                    return false;
                SECURITY_ATTRIBUTES security{};
                security.nLength = sizeof(security);
                security.lpSecurityDescriptor = descriptor;
                security.bInheritHandle = FALSE;
                const BOOL created = CreateDirectoryW(candidate.c_str(), &security);
                const DWORD createError = created ? ERROR_SUCCESS : GetLastError();
                LocalFree(descriptor);
                if (created)
                {
                    root.swap(candidate);
                    return true;
                }
                if (createError != ERROR_ALREADY_EXISTS)
                    return false;
                if (GetLastError() != ERROR_ALREADY_EXISTS)
                    return false;
            }
        }
        catch (...) { return false; }
        return false;
    }

    bool openCurrentFile() noexcept
    {
        if (sequence.currentEntryIndex() >= manifestEntries.size())
            return false;
        const auto& entry = manifestEntries[sequence.currentEntryIndex()];
        if (!hasSafeRuntimeRelativePath(entry.relativePath, entry.pathLength))
            return false;

        try
        {
            std::wstring path = root;
            std::size_t componentStart = 0;
            for (std::size_t i = 0; i <= entry.pathLength; ++i)
            {
                if (i != entry.pathLength && entry.relativePath[i] != u'/')
                    continue;
                path += L"\\";
                for (std::size_t j = componentStart; j < i; ++j)
                    path.push_back(static_cast<wchar_t>(entry.relativePath[j]));
                if (i != entry.pathLength)
                {
                    // Track before creation: allocation failure must not leave
                    // an untracked directory behind. Existing components are
                    // accepted only if they are already in our private root.
                    createdDirectories.push_back(path);
                    if (!CreateDirectoryW(path.c_str(), nullptr))
                    {
                        const DWORD error = GetLastError();
                        createdDirectories.pop_back();
                        if (error != ERROR_ALREADY_EXISTS)
                            return false;
                    }
                    HANDLE directory = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (directory == INVALID_HANDLE_VALUE)
                        return false;
                    FILE_ATTRIBUTE_TAG_INFO info{};
                    const bool safe = GetFileInformationByHandleEx(
                        directory, FileAttributeTagInfo, &info, sizeof(info)) != FALSE &&
                        (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
                        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
                    if (!CloseHandle(directory))
                    {
                        // Capacity is reserved from manifest path lengths,
                        // so the failed handle remains tracked for cleanup retry.
                        outstandingDirectoryHandles.push_back(directory);
                        return false;
                    }
                    if (!safe)
                        return false;
                    componentStart = i + 1;
                }
                else
                {
                    currentFilePath = path;
                }
            }
            // Track before CREATE_NEW so an allocation failure cannot leave
            // an untracked file in the staging tree.
            createdFiles.push_back(currentFilePath);
            file = CreateFileW(currentFilePath.c_str(), GENERIC_READ | GENERIC_WRITE,
                0, nullptr, CREATE_NEW,
                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_OPEN_REPARSE_POINT |
                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (file == INVALID_HANDLE_VALUE)
            {
                createdFiles.pop_back();
                return false;
            }
            FILE_ATTRIBUTE_TAG_INFO info{};
            if (!GetFileInformationByHandleEx(file, FileAttributeTagInfo, &info, sizeof(info)) ||
                (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
                (info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
                return false;
            return true;
        }
        catch (...) { return false; }
    }

    bool verifyAndCloseCurrentFile() noexcept
    {
        if (file == INVALID_HANDLE_VALUE ||
            sequence.currentEntryIndex() == 0 ||
            verifiedFiles >= manifestEntries.size())
            return false;
        const auto index = verifiedFiles;
        const auto& entry = manifestEntries[index];
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
            static_cast<std::uint64_t>(size.QuadPart) != entry.sizeBytes)
            return false;

        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        DWORD objectBytes = 0, returned = 0;
        std::vector<UCHAR> object;
        bool ok = false;
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            goto done;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes), &returned, 0) < 0 ||
            returned != sizeof(objectBytes) || objectBytes == 0)
            goto done;
        try { object.resize(objectBytes); } catch (...) { goto done; }
        if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes, nullptr, 0, 0) < 0)
            goto done;
        {
            LARGE_INTEGER zero{};
            if (!SetFilePointerEx(file, zero, nullptr, FILE_BEGIN))
                goto done;
            std::array<UCHAR, 64 * 1024> buffer{};
            for (;;)
            {
                DWORD read = 0;
                if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
                    goto done;
                if (read == 0)
                    break;
                if (BCryptHashData(hash, buffer.data(), read, 0) < 0)
                    goto done;
            }
        }
        {
            std::array<UCHAR, 32> digest{};
            if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
                goto done;
            UCHAR difference = 0;
            for (std::size_t i = 0; i < digest.size(); ++i)
                difference |= static_cast<UCHAR>(digest[i] ^ entry.sha256[i]);
            ok = difference == 0;
        }
    done:
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        // Keep ownership of the handle if CloseHandle fails. The caller's
        // failure path will retry cleanup instead of losing the only handle
        // that can still refer to the staged file.
        if (!CloseHandle(file))
            return false;
        file = INVALID_HANDLE_VALUE;
        if (!ok)
            return false;
        currentFilePath.clear();
        return true;
    }

    std::wstring root;
    std::wstring currentFilePath;
    std::vector<std::wstring> createdDirectories;
    std::vector<std::wstring> createdFiles;
    std::vector<HANDLE> outstandingDirectoryHandles;
    std::vector<WorkerRuntimeManifestEntry> manifestEntries;
    WorkerRuntimeTransferSequence sequence;
    HANDLE file = INVALID_HANDLE_VALUE;
    std::size_t verifiedFiles = 0;
    bool active = false;
};
} // namespace voice_studio::ipc
