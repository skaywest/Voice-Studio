#include "AudioIpc/WorkerModelFileVerifier.h"
#include <array>
#include <cstdint>
#include <string>
#include <windows.h>

using namespace voice_studio::ipc;

int wmain()
{
    wchar_t tempPath[MAX_PATH]{}, filePath[MAX_PATH]{};
    if (GetTempPathW(MAX_PATH, tempPath) == 0 ||
        GetTempFileNameW(tempPath, L"vsm", 0, filePath) == 0)
        return 1;

    HANDLE file = CreateFileW(filePath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 2;
    constexpr char payload[] = "abc";
    DWORD written = 0;
    const bool wrote = WriteFile(file, payload, 3, &written, nullptr) != FALSE && written == 3;
    CloseHandle(file);
    if (!wrote) { DeleteFileW(filePath); return 3; }

    constexpr std::uint8_t abcSha256[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
    };
    const auto* path = reinterpret_cast<const char16_t*>(filePath);
    std::uint16_t length = 0;
    while (filePath[length] != L'\\' && length < MAX_PATH) ++length;
    // Use the full path length; the temporary path is a normal local drive path.
    length = static_cast<std::uint16_t>(wcslen(filePath));

    const bool correct = verifyModelFile(path, length, 3, abcSha256);
    const bool wrongSize = !verifyModelFile(path, length, 4, abcSha256);
    auto wrongHash = std::array<std::uint8_t, 32>{};
    const bool rejectedHash = !verifyModelFile(path, length, 3, wrongHash);
    DeleteFileW(filePath);
    return correct && wrongSize && rejectedHash ? 0 : 4;
}
