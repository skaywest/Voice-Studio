#include "AudioIpc/WorkerModelFileVerifier.h"
#include <cstdint>
#include <cwchar>
#include <string>
#include <windows.h>
using namespace voice_studio::ipc;

int wmain()
{
    if (!isContainedFinalPath(L"\\\\?\\C:\\", L"\\\\?\\C:\\model.pth") ||
        !isContainedFinalPath(L"\\\\?\\C:\\models\\", L"\\\\?\\C:\\models\\voice.pth") ||
        isContainedFinalPath(L"\\\\?\\C:\\models", L"\\\\?\\C:\\models-old\\voice.pth") ||
        isContainedFinalPath(L"\\\\?\\C:\\models", L"\\\\?\\D:\\models\\voice.pth")) return 10;

    wchar_t tempPath[MAX_PATH]{}, filePath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) == 0 ||
        GetTempFileNameW(tempPath, L"vsm", 0, filePath) == 0) return 1;

    HANDLE writer = CreateFileW(filePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (writer == INVALID_HANDLE_VALUE) { DeleteFileW(filePath); return 2; }
    constexpr char payload[] = "abc";
    DWORD written = 0;
    const bool wrote = WriteFile(writer, payload, 3, &written, nullptr) != FALSE && written == 3;
    if (!wrote) { CloseHandle(writer); DeleteFileW(filePath); return 3; }

    constexpr std::uint8_t abcSha256[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    const auto* path = reinterpret_cast<const char16_t*>(filePath);
    const auto length = static_cast<std::uint16_t>(std::wcslen(filePath));

    // Even a pre-existing writer that allows broad sharing must prevent the
    // verifier's stricter FILE_SHARE_READ open from succeeding.
    std::wstring canonicalRoot;
    HANDLE root = CreateFileW(tempPath, FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (root == INVALID_HANDLE_VALUE) { CloseHandle(writer); DeleteFileW(filePath); return 4; }
    const bool rootOk = finalPath(root, canonicalRoot);
    CloseHandle(root);
    if (!rootOk) { CloseHandle(writer); DeleteFileW(filePath); return 5; }

    HANDLE verified = INVALID_HANDLE_VALUE;
    const bool rejectedPreexistingWriter =
        !openVerifiedFile(path, length, 3, abcSha256, canonicalRoot, verified);
    CloseHandle(writer);

    const bool correct =
        openVerifiedFile(path, length, 3, abcSha256, canonicalRoot, verified);
    HANDLE secondWriter = CreateFileW(filePath, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool blockedNewWriter = secondWriter == INVALID_HANDLE_VALUE;
    if (secondWriter != INVALID_HANDLE_VALUE) CloseHandle(secondWriter);

    if (verified != INVALID_HANDLE_VALUE) CloseHandle(verified);
    HANDLE writerAfterRelease = CreateFileW(filePath, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool writableAfterRelease = writerAfterRelease != INVALID_HANDLE_VALUE;
    if (writerAfterRelease != INVALID_HANDLE_VALUE) CloseHandle(writerAfterRelease);
    DeleteFileW(filePath);
    return rejectedPreexistingWriter && correct && blockedNewWriter && writableAfterRelease ? 0 : 6;
}
