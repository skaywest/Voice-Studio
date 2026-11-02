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
#include <vector>

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

class UniqueHandle
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) noexcept : value_(value) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr && value_ != INVALID_HANDLE_VALUE; }
    HANDLE release() noexcept { HANDLE result = value_; value_ = nullptr; return result; }
    void reset(HANDLE value = nullptr) noexcept
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_ = nullptr;
};

template <typename T>
class UniqueLocal
{
public:
    UniqueLocal() = default;
    explicit UniqueLocal(T* value) noexcept : value_(value) {}
    ~UniqueLocal() { reset(); }
    UniqueLocal(const UniqueLocal&) = delete;
    UniqueLocal& operator=(const UniqueLocal&) = delete;
    UniqueLocal(UniqueLocal&& other) noexcept : value_(other.release()) {}
    UniqueLocal& operator=(UniqueLocal&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    T* get() const noexcept { return value_; }
    T* release() noexcept { T* result = value_; value_ = nullptr; return result; }
    void reset(T* value = nullptr) noexcept
    {
        if (value_) LocalFree(value_);
        value_ = value;
    }
private:
    T* value_ = nullptr;
};

struct Security
{
    UniqueLocal<void> descriptor;
    SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE };

    bool initialize()
    {
        HANDLE rawToken = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
            return false;
        UniqueHandle token(rawToken);

        DWORD bytes = 0;
        GetTokenInformation(token.get(), TokenUser, nullptr, 0, &bytes);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes == 0)
            return false;

        // TOKEN_USER contains pointers and must be suitably aligned. A byte
        // string only guarantees char alignment, so use max_align_t storage.
        const std::size_t wordCount =
            (static_cast<std::size_t>(bytes) + sizeof(std::max_align_t) - 1) /
            sizeof(std::max_align_t);
        std::vector<std::max_align_t> storage(wordCount);
        auto* user = reinterpret_cast<TOKEN_USER*>(storage.data());
        if (!GetTokenInformation(token.get(), TokenUser, user, bytes, &bytes))
            return false;

        LPWSTR rawSidText = nullptr;
        if (!ConvertSidToStringSidW(user->User.Sid, &rawSidText))
            return false;
        UniqueLocal<wchar_t> sidText(rawSidText);

        const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sidText.get()) + L")(A;;GA;;;SY)";
        PSECURITY_DESCRIPTOR rawDescriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &rawDescriptor, nullptr))
            return false;

        descriptor.reset(rawDescriptor);
        attributes.lpSecurityDescriptor = descriptor.get();
        return true;
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

int consume(const wchar_t* mappingName, const wchar_t* readyName, const wchar_t* progressName,
            DWORD parentPid, bool exitAfterReady)
{
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent)
        return 10;

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName);
    if (!mapping)
    {
        CloseHandle(parent);
        return 11;
    }

    auto* shared = static_cast<SharedRegion*>(
        MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRegion)));
    if (!shared)
    {
        CloseHandle(mapping);
        CloseHandle(parent);
        return 12;
    }

    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName);
    if (!ready)
    {
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        CloseHandle(parent);
        return 16;
    }
    if (shared->magic != kMagic || shared->version != 1 ||
        shared->slotCount != kCapacity)
    {
        CloseHandle(ready);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        CloseHandle(parent);
        return 13;
    }
    HANDLE progress = OpenEventW(EVENT_MODIFY_STATE, FALSE, progressName);
    if (!progress)
    {
        CloseHandle(ready);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        CloseHandle(parent);
        return 17;
    }
    if (!SetEvent(ready))
    {
        CloseHandle(progress);
        CloseHandle(ready);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        CloseHandle(parent);
        return 14;
    }

    if (exitAfterReady)
    {
        UnmapViewOfFile(shared);
        CloseHandle(progress);
        CloseHandle(ready);
        CloseHandle(mapping);
        CloseHandle(parent);
        return 20;
    }

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
        if (position == 1 && !SetEvent(progress))
        {
            valid = false;
            break;
        }
    }

    UnmapViewOfFile(shared);
    CloseHandle(progress);
    CloseHandle(ready);
    CloseHandle(mapping);
    CloseHandle(parent);
    return valid && position == kBlockCount ? 0 : 15;
}

std::wstring quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

bool produce(bool malformedHeader, bool exitAfterReady, bool terminateAfterReady,
             bool terminateDuringTransfer, bool corruptFirstBlock)
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
    shared->version = malformedHeader ? 999u : 1u;
    shared->slotCount = static_cast<std::uint32_t>(kCapacity);
    for (std::uint64_t i = 0; i < kCapacity; ++i)
        InterlockedExchange64(&shared->slots[i].sequence, static_cast<LONG64>(i));

    const std::wstring progressName = mappingName + L"_progress";
    HANDLE progress = CreateEventW(&security.attributes, TRUE, FALSE, progressName.c_str());
    HANDLE ready = CreateEventW(&security.attributes, TRUE, FALSE, readyName.c_str());
    if (!ready || !progress)
    {
        if (ready) CloseHandle(ready);
        if (progress) CloseHandle(progress);
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
        CloseHandle(progress);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return false;
    }

    const std::wstring command = quote(executable.data()) + L" --consume " +
                                 quote(mappingName) + L" " + quote(readyName) + L" " +
                                 quote(progressName) + L" " +
                                 std::to_wstring(GetCurrentProcessId()) +
                                 (exitAfterReady ? L" --exit-after-ready" : L"");
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION process {};
    std::wstring mutableCommand = command;
    if (!CreateProcessW(executable.data(), mutableCommand.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process))
    {
        CloseHandle(ready);
        CloseHandle(progress);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        return false;
    }

    HANDLE handshakeHandles[] { ready, process.hProcess };
    const DWORD handshake = WaitForMultipleObjects(2, handshakeHandles, FALSE, kTimeoutMs);
    bool ok = handshake == WAIT_OBJECT_0;
    if (ok && terminateAfterReady)
    {
        // The ready event proves the child opened the mapping and validated its
        // header. Kill it abruptly before any transfer to exercise crash detection.
        if (!TerminateProcess(process.hProcess, 77) ||
            WaitForSingleObject(process.hProcess, kTimeoutMs) != WAIT_OBJECT_0)
            ok = false;
    }

    const ULONGLONG transferDeadline = GetTickCount64() + kTimeoutMs;
    std::uint64_t position = 0;
    while (ok && !malformedHeader && !exitAfterReady && !terminateAfterReady &&
           position < kBlockCount)
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
        if (corruptFirstBlock && position == 0)
            slot.samples[7] += 1.0f;

        InterlockedExchange64(&slot.sequence, static_cast<LONG64>(position + 1));
        ++position;
        if (corruptFirstBlock && position == 1)
            break;
        if (terminateDuringTransfer && position == 1)
        {
            HANDLE progressHandles[] { progress, process.hProcess };
            const DWORD progressWait = WaitForMultipleObjects(2, progressHandles, FALSE, kTimeoutMs);
            if (progressWait == WAIT_OBJECT_0)
            {
                if (!TerminateProcess(process.hProcess, 78) ||
                    WaitForSingleObject(process.hProcess, kTimeoutMs) != WAIT_OBJECT_0)
                    ok = false;
            }
            else
                ok = false;
            break;
        }
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
        const BOOL gotExitCode = GetExitCodeProcess(process.hProcess, &exitCode);
        const DWORD expectedExit = malformedHeader ? 13u :
                                   (exitAfterReady ? 20u :
                                    (terminateAfterReady ? 77u :
                                     (terminateDuringTransfer ? 78u :
                                      (corruptFirstBlock ? 15u : 0u))));
        if (!gotExitCode || exitCode != expectedExit)
        {
            std::fwprintf(stderr, L"Consumer exit mismatch: got=%u, exitCode=%lu, expected=%lu, blocksSent=%llu\\n",
                          static_cast<unsigned>(gotExitCode), static_cast<unsigned long>(exitCode),
                          static_cast<unsigned long>(expectedExit),
                          static_cast<unsigned long long>(position));
            ok = false;
        }
        else if (malformedHeader || exitAfterReady || terminateAfterReady ||
                 terminateDuringTransfer || corruptFirstBlock)
        {
            ok = true;
        }
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(ready);
    CloseHandle(progress);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);

    if (ok && malformedHeader)
        std::wprintf(L"PASS: malformed shared-memory header rejected with exit code 13\n");
    else if (ok && exitAfterReady)
        std::wprintf(L"PASS: worker cleanly exited after handshake with exit code 20\n");
    else if (ok && terminateAfterReady)
        std::wprintf(L"PASS: worker was forcibly terminated after handshake; exit code 77 observed\n");
    else if (ok)
        std::wprintf(L"PASS: %llu blocks transferred and verified across two processes\n",
                     static_cast<unsigned long long>(kBlockCount));
    else
        std::fwprintf(stderr, L"FAIL: shared-memory scenario did not meet its expected outcome\n");
    return ok;
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if ((argc == 6 || argc == 7) && std::wcscmp(argv[1], L"--consume") == 0)
    {
        wchar_t* end = nullptr;
        const unsigned long parentPid = std::wcstoul(argv[5], &end, 10);
        if (!end || *end != L'\0' || parentPid == 0 || parentPid > MAXDWORD)
            return 2;
        const bool exitAfterReady = argc == 7 &&
            std::wcscmp(argv[6], L"--exit-after-ready") == 0;
        if (argc == 7 && !exitAfterReady)
            return 2;
        return consume(argv[2], argv[3], argv[4], static_cast<DWORD>(parentPid), exitAfterReady);
    }

    const bool malformedHeader = argc == 2 &&
        std::wcscmp(argv[1], L"--malformed-header") == 0;
    const bool exitAfterReady = argc == 2 &&
        std::wcscmp(argv[1], L"--exit-after-ready") == 0;
    const bool terminateAfterReady = argc == 2 &&
        std::wcscmp(argv[1], L"--terminate-after-ready") == 0;
    const bool terminateDuringTransfer = argc == 2 &&
        std::wcscmp(argv[1], L"--terminate-during-transfer") == 0;
    const bool corruptFirstBlock = argc == 2 &&
        std::wcscmp(argv[1], L"--corrupt-first-block") == 0;
    const bool restartAfterCrash = argc == 2 &&
        std::wcscmp(argv[1], L"--restart-after-crash") == 0;
    if (argc > 2 || (argc == 2 &&
        !malformedHeader && !exitAfterReady && !terminateAfterReady &&
        !terminateDuringTransfer && !corruptFirstBlock && !restartAfterCrash))
    {
        std::fwprintf(stderr, L"Usage: VoiceStudioSharedMemoryPocTest.exe [--malformed-header|--exit-after-ready|--terminate-after-ready|--terminate-during-transfer|--corrupt-first-block|--restart-after-crash]\n");
        return 2;
    }
    if (restartAfterCrash)
    {
        // produce() closes the first session resources before returning.
        if (!produce(false, false, true, false, false))
            return 1;
        if (!produce(false, false, false, false, false))
            return 1;
        std::wprintf(L"PASS: fresh shared-memory session started after worker crash\\n");
        return 0;
    }
    return produce(malformedHeader, exitAfterReady, terminateAfterReady,
                   terminateDuringTransfer, corruptFirstBlock) ? 0 : 1;
}
