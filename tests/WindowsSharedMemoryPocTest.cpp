#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <limits>
#include <string>
#include <type_traits>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "advapi32.lib")

namespace
{
constexpr std::uint64_t kMagic = 0x5653545544494F31ull;
constexpr std::uint64_t kBlockCount = 1'000'000;
constexpr std::uint64_t kCapacity = 8;
constexpr std::size_t kSamples = 64;
constexpr DWORD kTimeoutMs = 30000;

struct Slot
{
    alignas(8) volatile LONG64 sequence;
    std::uint64_t block;
    std::array<float, kSamples> samples;
};

struct SharedRegion
{
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t slotCount;
    Slot slots[kCapacity];
};
static_assert(alignof(Slot) >= 8, "Interlocked sequence must be 8-byte aligned");
static_assert(offsetof(Slot, sequence) % 8 == 0, "Sequence offset must be aligned");
static_assert(std::is_standard_layout<SharedRegion>::value, "Shared ABI must be standard layout");

struct Security
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };

    bool initialize()
    {
        HANDLE token = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
            return false;

        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
        {
            CloseHandle(token);
            return false;
        }

        std::string storage(bytes, '\0');
        auto* user = reinterpret_cast<TOKEN_USER*>(storage.data());
        const bool queried = GetTokenInformation(token, TokenUser, user, bytes, &bytes) != FALSE;
        CloseHandle(token);
        if (!queried)
            return false;

        LPWSTR sidText = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &sidText))
            return false;

        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sidText) + L")(A;;GA;;;SY)";
        LocalFree(sidText);
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return false;

        attributes.lpSecurityDescriptor = descriptor;
        return true;
    }

    ~Security()
    {
        if (descriptor)
            LocalFree(descriptor);
    }
};

bool randomName(std::wstring& mapping, std::wstring& ready)
{
    std::array<unsigned char, 16> nonce {};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return false;

    wchar_t suffix[33] {};
    for (std::size_t i = 0; i < nonce.size(); ++i)
        swprintf_s(suffix + i * 2, 3, L"%02x", nonce[i]);

    mapping = L"Local\\VoiceStudioPoc_" + std::wstring(suffix);
    ready = mapping + L"_ready";
    return true;
}

std::uint64_t expectedSample(std::uint64_t block, std::size_t index)
{
    return (block * 131u + index * 17u) & 0xffffu;
}

bool consume(const wchar_t* mappingName, const wchar_t* readyName, DWORD parentPid)
{
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent)
        return false;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (!mapping)
    {
        CloseHandle(parent);
        return false;
    }

    auto* shared = static_cast<SharedRegion*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRegion)));
    if (!shared)
    {
        CloseHandle(mapping);
        CloseHandle(parent);
        return false;
    }

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    if (!ready || shared->magic != kMagic || shared->version != 1 ||
        shared->slotCount != kCapacity)
    {
        if (ready) CloseHandle(ready);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        CloseHandle(parent);
        return false;
    }
    SetEvent(ready);

    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    std::uint64_t position = 0;
    bool valid = true;
    while (position < kBlockCount)
    {
        if (GetTickCount64() >= deadline ||
            WaitForSingleObject(parent, 0) != WAIT_TIMEOUT)
        {
            valid = false;
            break;
        }

        Slot& slot = shared->slots[position % kCapacity];
        const auto sequence = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&slot.sequence, 0, 0));

        if (sequence != position + 1)
        {
            SwitchToThread();
            continue;
        }

        if (slot.block != position)
        {
            valid = false;
            break;
        }
        for (std::size_t i = 0; i < kSamples; ++i)
        {
            const float expected = static_cast<float>(expectedSample(position, i));
            if (slot.samples[i] != expected)
            {
                valid = false;
                break;
            }
        }
        if (!valid)
            break;

        InterlockedExchange64(&slot.sequence,
                              static_cast<LONG64>(position + kCapacity));
        ++position;
    }

    UnmapViewOfFile(shared);
    CloseHandle(ready);
    CloseHandle(mapping);
    CloseHandle(parent);
    return valid && position == kBlockCount;
}

std::wstring quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

bool produce()
{
    Security security;
    if (!security.initialize())
    {
        std::fwprintf(stderr, L"Could not create current-user security descriptor\n");
        return false;
    }

    std::wstring mappingName, readyName;
    if (!randomName(mappingName, readyName))
        return false;

    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security.attributes,
                                        PAGE_READWRITE, 0, sizeof(SharedRegion),
                                        mappingName.c_str());
    if (!mapping || GetLastError() == ERROR_ALREADY_EXISTS)
    {
        if (mapping) CloseHandle(mapping);
        return false;
    }

    auto* shared = static_cast<SharedRegion*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRegion)));
    if (!shared)
    {
        CloseHandle(mapping);
        return false;
    }

    ZeroMemory(shared, sizeof(*shared));
    shared->magic = kMagic;
    shared->version = 1;
    shared->slotCount = static_cast<std::uint32_t>(kCapacity);
    for (std::uint64_t i = 0; i < kCapacity; ++i)
        InterlockedExchange64(&shared->slots[i].sequence, static_cast<LONG64>(i));

    HANDLE ready = CreateEventW(&security.attributes, TRUE, FALSE, readyName.c_str());
    if (!ready)
    {
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return false;
    }

    std::array<wchar_t, 32768> executable {};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
    {
        CloseHandle(ready);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return false;
    }

    const std::wstring command = quote(executable.data()) + L" --consume " +
                                 quote(mappingName) + L" " + quote(readyName) + L" " +
                                 std::to_wstring(GetCurrentProcessId());
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(executable.data(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        CloseHandle(ready);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return false;
    }

    bool ok = WaitForSingleObject(ready, kTimeoutMs) == WAIT_OBJECT_0;
    const ULONGLONG transferDeadline = GetTickCount64() + kTimeoutMs;
    std::uint64_t position = 0;
    while (ok && position < kBlockCount)
    {
        if (GetTickCount64() >= transferDeadline ||
            WaitForSingleObject(process.hProcess, 0) != WAIT_TIMEOUT)
        {
            ok = false;
            break;
        }

        Slot& slot = shared->slots[position % kCapacity];
        const auto sequence = static_cast<std::uint64_t>(
            InterlockedCompareExchange64(&slot.sequence, 0, 0));
        if (sequence != position)
        {
            SwitchToThread();
            continue;
        }

        slot.block = position;
        for (std::size_t i = 0; i < kSamples; ++i)
            slot.samples[i] = static_cast<float>(expectedSample(position, i));

        InterlockedExchange64(&slot.sequence, static_cast<LONG64>(position + 1));
        ++position;
    }

    const DWORD waitResult = WaitForSingleObject(process.hProcess, kTimeoutMs);
    if (waitResult != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 2);
        WaitForSingleObject(process.hProcess, 5000);
        ok = false;
    }
    else
    {
        DWORD exitCode = 1;
        ok = ok && GetExitCodeProcess(process.hProcess, &exitCode) && exitCode == 0;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(ready);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);

    if (ok)
        std::wprintf(L"PASS: %llu blocks transferred and verified across two processes\n",
                     static_cast<unsigned long long>(kBlockCount));
    else
        std::fwprintf(stderr, L"FAIL: cross-process transfer did not complete cleanly\n");
    return ok;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc == 5 && std::wcscmp(argv[1], L"--consume") == 0)
    {
        wchar_t* end = nullptr;
        const unsigned long parentPid = std::wcstoul(argv[4], &end, 10);
        if (!end || *end != L'\0' || parentPid == 0 || parentPid > MAXDWORD)
            return 2;
        return consume(argv[2], argv[3], static_cast<DWORD>(parentPid)) ? 0 : 1;
    }

    if (argc != 1)
    {
        std::fwprintf(stderr, L"Usage: VoiceStudioSharedMemoryPocTest.exe\n");
        return 2;
    }
    return produce() ? 0 : 1;
}
