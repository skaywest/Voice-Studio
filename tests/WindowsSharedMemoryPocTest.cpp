#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cmath>
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
    std::uint32_t headerSize;
    std::uint32_t slotCount;
    std::uint32_t slotSize;
    std::uint32_t sampleCount;
    std::uint32_t reserved;
    Slot slots[kCapacity];
};
static_assert(alignof(Slot) >= 8, "Interlocked sequence must be 8-byte aligned");
static_assert(offsetof(Slot, sequence) % 8 == 0, "Sequence offset must be aligned");
static_assert(std::is_standard_layout<Slot>::value, "Slot ABI must be standard layout");
static_assert(std::is_standard_layout<SharedRegion>::value, "Shared ABI must be standard layout");
static_assert(offsetof(SharedRegion, slots) == sizeof(SharedRegion) - sizeof(Slot) * kCapacity,
              "Shared header must have a stable contiguous layout");
static_assert(sizeof(Slot) <= (std::numeric_limits<std::uint32_t>::max)(),
              "Slot size must fit the ABI field");
static_assert(sizeof(SharedRegion) <= (std::numeric_limits<std::uint32_t>::max)(),
              "Region size must fit the ABI field");

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

class UniqueView
{
public:
    UniqueView() = default;
    explicit UniqueView(void* value) noexcept : value_(value) {}
    ~UniqueView() { reset(); }
    UniqueView(const UniqueView&) = delete;
    UniqueView& operator=(const UniqueView&) = delete;
    UniqueView(UniqueView&& other) noexcept : value_(other.release()) {}
    UniqueView& operator=(UniqueView&& other) noexcept
    {
        if (this != &other) reset(other.release());
        return *this;
    }
    void* get() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }
    void* release() noexcept { void* result = value_; value_ = nullptr; return result; }
    void reset(void* value = nullptr) noexcept
    {
        if (value_) UnmapViewOfFile(value_);
        value_ = value;
    }
private:
    void* value_ = nullptr;
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
    UniqueHandle parent(OpenProcess(SYNCHRONIZE, FALSE, parentPid));
    if (!parent) return 10;

    UniqueHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mappingName));
    if (!mapping) return 11;

    UniqueView view(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRegion)));
    if (!view) return 12;
    auto* shared = static_cast<SharedRegion*>(view.get());

    UniqueHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, readyName));
    if (!ready) return 16;

    if (shared->magic != kMagic || shared->version != 2 ||
        shared->headerSize != offsetof(SharedRegion, slots) ||
        shared->slotCount != kCapacity ||
        shared->slotSize != sizeof(Slot) ||
        shared->sampleCount != kSamples ||
        shared->reserved != 0)
    {
        std::fwprintf(stderr, L"Rejected ABI: version=%u header=%u slots=%u slotSize=%u samples=%u reserved=%u\\n",
                      shared->version, shared->headerSize, shared->slotCount,
                      shared->slotSize, shared->sampleCount, shared->reserved);
        return 13;
    }

    UniqueHandle progress(OpenEventW(EVENT_MODIFY_STATE, FALSE, progressName));
    if (!progress) return 17;
    if (!SetEvent(ready.get())) return 14;
    if (exitAfterReady) return 20;

    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    std::uint64_t position = 0;
    bool valid = true;
    while (position < kBlockCount)
    {
        if (GetTickCount64() >= deadline ||
            WaitForSingleObject(parent.get(), 0) != WAIT_TIMEOUT)
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
        if (!valid) break;

        InterlockedExchange64(&slot.sequence,
                              static_cast<LONG64>(position + kCapacity));
        ++position;
        if (position == 1 && !SetEvent(progress.get()))
        {
            valid = false;
            break;
        }
    }

    return valid && position == kBlockCount ? 0 : 15;
}

std::wstring quote(const std::wstring& value)
{
    return L"\"" + value + L"\"";
}

bool produce(int malformedHeaderField, bool exitAfterReady, bool terminateAfterReady,
             bool terminateDuringTransfer, int corruptSample)
{
    Security security;
    if (!security.initialize())
    {
        std::fwprintf(stderr, L"Could not create current-user security descriptor\\n");
        return false;
    }

    std::wstring mappingName, readyName;
    if (!randomName(mappingName, readyName))
        return false;

    HANDLE rawMapping = CreateFileMappingW(INVALID_HANDLE_VALUE, &security.attributes,
                                                PAGE_READWRITE, 0, sizeof(SharedRegion),
                                                mappingName.c_str());
    // GetLastError must be captured immediately: a pre-existing named mapping
    // is a collision, not a mapping instance owned by this producer.
    const DWORD mappingError = GetLastError();
    UniqueHandle mapping(rawMapping);
    if (!mapping || mappingError == ERROR_ALREADY_EXISTS)
        return false;

    UniqueView view(MapViewOfFile(mapping.get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedRegion)));
    if (!view)
        return false;
    auto* shared = static_cast<SharedRegion*>(view.get());

    ZeroMemory(shared, sizeof(*shared));
    shared->magic = kMagic;
    shared->version = malformedHeaderField == 1 ? 999u : 2u;
    if (malformedHeaderField == 2) shared->slotSize = 0;
    if (malformedHeaderField == 3) shared->sampleCount = 0;
    if (malformedHeaderField == 4) shared->reserved = 0xFFFFFFFFu;
    shared->headerSize = static_cast<std::uint32_t>(offsetof(SharedRegion, slots));
    shared->slotCount = static_cast<std::uint32_t>(kCapacity);
    shared->slotSize = static_cast<std::uint32_t>(sizeof(Slot));
    shared->sampleCount = static_cast<std::uint32_t>(kSamples);
    shared->reserved = 0;
    for (std::uint64_t i = 0; i < kCapacity; ++i)
        InterlockedExchange64(&shared->slots[i].sequence, static_cast<LONG64>(i));

    const std::wstring progressName = mappingName + L"_progress";
    UniqueHandle progress(CreateEventW(&security.attributes, TRUE, FALSE, progressName.c_str()));
    UniqueHandle ready(CreateEventW(&security.attributes, TRUE, FALSE, readyName.c_str()));
    if (!ready || !progress)
        return false;

    std::array<wchar_t, 32768> executable {};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (length == 0 || length >= executable.size())
        return false;

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
        return false;

    UniqueHandle childProcess(process.hProcess);
    UniqueHandle childThread(process.hThread);

    HANDLE handshakeHandles[] { ready.get(), childProcess.get() };
    const DWORD handshake = WaitForMultipleObjects(2, handshakeHandles, FALSE, kTimeoutMs);
    bool ok = handshake == WAIT_OBJECT_0;
    if (ok && terminateAfterReady)
    {
        // The ready event proves the child opened the mapping and validated its
        // header. Kill it abruptly before any transfer to exercise crash detection.
        if (!TerminateProcess(childProcess.get(), 77) ||
            WaitForSingleObject(childProcess.get(), kTimeoutMs) != WAIT_OBJECT_0)
            ok = false;
    }

    const ULONGLONG transferDeadline = GetTickCount64() + kTimeoutMs;
    std::uint64_t position = 0;
    while (ok && malformedHeaderField == 0 && !exitAfterReady && !terminateAfterReady &&
           position < kBlockCount)
    {
        if (GetTickCount64() >= transferDeadline ||
            WaitForSingleObject(childProcess.get(), 0) != WAIT_TIMEOUT)
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
        if (corruptSample != 0 && position == 0)
        {
            if (corruptSample == 1) slot.samples[7] += 1.0f;
            else if (corruptSample == 2) slot.samples[7] = (std::numeric_limits<float>::quiet_NaN)();
            else if (corruptSample == 3) slot.samples[7] = (std::numeric_limits<float>::infinity)();
        }

        InterlockedExchange64(&slot.sequence, static_cast<LONG64>(position + 1));
        ++position;
        if (corruptSample != 0 && position == 1)
            break;
        if (terminateDuringTransfer && position == 1)
        {
            HANDLE progressHandles[] { progress.get(), childProcess.get() };
            const DWORD progressWait = WaitForMultipleObjects(2, progressHandles, FALSE, kTimeoutMs);
            if (progressWait == WAIT_OBJECT_0)
            {
                if (!TerminateProcess(childProcess.get(), 78) ||
                    WaitForSingleObject(childProcess.get(), kTimeoutMs) != WAIT_OBJECT_0)
                    ok = false;
            }
            else
                ok = false;
            break;
        }
    }

    const DWORD waitResult = WaitForSingleObject(childProcess.get(), kTimeoutMs);
    if (waitResult != WAIT_OBJECT_0)
    {
        TerminateProcess(childProcess.get(), 2);
        WaitForSingleObject(childProcess.get(), 5000);
        ok = false;
    }
    else
    {
        DWORD exitCode = 1;
        const BOOL gotExitCode = GetExitCodeProcess(childProcess.get(), &exitCode);
        const DWORD expectedExit = malformedHeaderField != 0 ? 13u :
                                   (exitAfterReady ? 20u :
                                    (terminateAfterReady ? 77u :
                                     (terminateDuringTransfer ? 78u :
                                      (corruptSample != 0 ? 15u : 0u))));
        if (!gotExitCode || exitCode != expectedExit)
        {
            std::fwprintf(stderr, L"Consumer exit mismatch: got=%u, exitCode=%lu, expected=%lu, blocksSent=%llu\\n",
                          static_cast<unsigned>(gotExitCode), static_cast<unsigned long>(exitCode),
                          static_cast<unsigned long>(expectedExit),
                          static_cast<unsigned long long>(position));
            ok = false;
        }
        else if (malformedHeaderField != 0 || exitAfterReady || terminateAfterReady ||
                 terminateDuringTransfer || corruptSample != 0)
        {
            ok = true;
        }
    }

    if (ok && malformedHeaderField != 0)
        std::wprintf(L"PASS: malformed shared-memory header rejected with exit code 13\\n");
    else if (ok && exitAfterReady)
        std::wprintf(L"PASS: worker cleanly exited after handshake with exit code 20\\n");
    else if (ok && terminateAfterReady)
        std::wprintf(L"PASS: worker was forcibly terminated after handshake; exit code 77 observed\\n");
    else if (ok)
        std::wprintf(L"PASS: %llu blocks transferred and verified across two processes\\n",
                     static_cast<unsigned long long>(kBlockCount));
    else
        std::fwprintf(stderr, L"FAIL: shared-memory scenario did not meet its expected outcome\\n");
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
    const bool malformedSlotSize = argc == 2 && std::wcscmp(argv[1], L"--malformed-slot-size") == 0;
    const bool malformedSampleCount = argc == 2 && std::wcscmp(argv[1], L"--malformed-sample-count") == 0;
    const bool malformedReserved = argc == 2 && std::wcscmp(argv[1], L"--malformed-reserved") == 0;
    const bool corruptNan = argc == 2 && std::wcscmp(argv[1], L"--corrupt-nan") == 0;
    const bool corruptInfinity = argc == 2 && std::wcscmp(argv[1], L"--corrupt-infinity") == 0;
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
        !malformedHeader && !malformedSlotSize && !malformedSampleCount && !malformedReserved && !corruptNan && !corruptInfinity && !exitAfterReady && !terminateAfterReady &&
        !terminateDuringTransfer && !corruptFirstBlock && !restartAfterCrash))
    {
        std::fwprintf(stderr, L"Usage: VoiceStudioSharedMemoryPocTest.exe [--malformed-header|--exit-after-ready|--terminate-after-ready|--terminate-during-transfer|--corrupt-first-block|--restart-after-crash]\n");
        return 2;
    }
    if (restartAfterCrash)
    {
        // produce() closes the first session resources before returning.
        if (!produce(0, false, true, false, 0))
            return 1;
        if (!produce(0, false, false, false, 0))
            return 1;
        std::wprintf(L"PASS: fresh shared-memory session started after worker crash\\n");
        return 0;
    }
    const int malformedField = malformedHeader ? 1 : (malformedSlotSize ? 2 : (malformedSampleCount ? 3 : (malformedReserved ? 4 : 0)));
    const int corruptMode = corruptFirstBlock ? 1 : (corruptNan ? 2 : (corruptInfinity ? 3 : 0));
    return produce(malformedField, exitAfterReady, terminateAfterReady, terminateDuringTransfer, corruptMode) ? 0 : 1;
}
