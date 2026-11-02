#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include "AudioIpc/AudioBlockSlotSync.h"

#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <string>

using namespace voice_studio::ipc;

namespace
{
constexpr DWORD kTimeoutMs = 15000;
constexpr std::uint64_t kBlocks = 50000;

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

struct View
{
    void* value = nullptr;
    ~View() { if (value) UnmapViewOfFile(value); }
    explicit View(void* p = nullptr) : value(p) {}
    View(const View&) = delete;
    View& operator=(const View&) = delete;
};

bool waitForState(AudioBlockSlot& slot, AudioBlockSlotState expected, HANDLE process)
{
    const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
    while (GetTickCount64() < deadline)
    {
        if (WaitForSingleObject(process, 0) != WAIT_TIMEOUT)
            return false;
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&slot.state), 0, 0) ==
            static_cast<LONG>(expected))
            return true;
        SwitchToThread();
    }
    return false;
}

int childMain(const wchar_t* name, bool crashWhileReading)
{
    Handle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name));
    if (!mapping.value) return 10;
    View view(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioBlockRegion)));
    if (!view.value) return 11;
    auto* region = static_cast<AudioBlockRegion*>(view.value);
    if (!hasValidAudioBlockHeader(region->header)) return 12;

    auto& slot = region->slots[0];
    if (crashWhileReading)
    {
        const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
        while (GetTickCount64() < deadline)
        {
            if (tryBeginAudioBlockRead(slot))
                return 77; // Simulated abrupt worker exit while owning READING.
            SwitchToThread();
        }
        return 13;
    }

    for (std::uint64_t expected = 1; expected <= kBlocks; ++expected)
    {
        const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
        while (!tryBeginAudioBlockRead(slot))
        {
            if (GetTickCount64() >= deadline) return 14;
            SwitchToThread();
        }
        const bool valid = slot.sequence == expected &&
            slot.generation == 3 && slot.sampleRate == 48000 &&
            slot.channels == 1 && slot.frames == 1 &&
            slot.payloadSamples == 1 &&
            slot.samples[0] == static_cast<float>(expected);
        if (!valid || !finishAudioBlockRead(slot)) return 15;
    }
    return 0;
}

bool initializeRegion(AudioBlockRegion& region)
{
    ZeroMemory(&region, sizeof(region));
    region.header.magic = kAudioBlockMagic;
    region.header.version = kAudioBlockAbiVersion;
    region.header.headerSize = sizeof(AudioBlockRegionHeader);
    region.header.slotCount = kAudioBlockSlotCount;
    region.header.slotSize = sizeof(AudioBlockSlot);
    region.header.maxFrames = kAudioBlockMaxFrames;
    region.header.maxChannels = kAudioBlockMaxChannels;
    region.header.sampleFormat = kAudioBlockFloat32;
    return hasValidAudioBlockHeader(region.header);
}

bool runScenario(bool crash)
{
    const std::wstring name = L"Local\\VoiceStudioAudioBlock_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    Handle mapping(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
        0, static_cast<DWORD>(sizeof(AudioBlockRegion)), name.c_str()));
    if (!mapping.value || GetLastError() == ERROR_ALREADY_EXISTS) return false;
    View view(MapViewOfFile(mapping.value, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioBlockRegion)));
    if (!view.value) return false;
    auto* region = static_cast<AudioBlockRegion*>(view.value);
    if (!initializeRegion(*region)) return false;

    wchar_t executable[32768] {};
    const DWORD length = GetModuleFileNameW(nullptr, executable, 32768);
    if (length == 0 || length >= 32768) return false;
    std::wstring command = L"\"" + std::wstring(executable) + L"\" " +
        (crash ? L"--crash " : L"--consume ") + L"\"" + name + L"\"";
    STARTUPINFOW startup { sizeof(startup) };
    PROCESS_INFORMATION processInfo {};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo))
        return false;
    Handle process(processInfo.hProcess);
    Handle thread(processInfo.hThread);
    auto& slot = region->slots[0];

    bool ok = true;
    if (crash)
    {
        slot.sequence = 1;
        slot.generation = 3;
        slot.deadlineMonotonicNs = 1;
        slot.sampleRate = 48000;
        slot.channels = 1;
        slot.frames = 1;
        slot.payloadSamples = 1;
        slot.samples[0] = 1.0f;
        if (!publishAudioBlockWrite(slot)) ok = false;
        if (ok && !waitForState(slot, AudioBlockSlotState::free, process))
        {
            // Child may claim the slot before the parent observes READING;
            // wait for its process exit, which is the required reclaim proof.
            if (WaitForSingleObject(process.value, kTimeoutMs) != WAIT_OBJECT_0)
                ok = false;
        }
        else if (ok)
        {
            if (WaitForSingleObject(process.value, kTimeoutMs) != WAIT_OBJECT_0)
                ok = false;
        }
        DWORD exitCode = 0;
        if (ok && (!GetExitCodeProcess(process.value, &exitCode) || exitCode != 77))
            ok = false;
        if (ok)
        {
            reclaimAudioBlockSlotsAfterWorkerExit(*region);
            ok = slot.state == static_cast<LONG>(AudioBlockSlotState::free);
        }
    }
    else
    {
        for (std::uint64_t sequence = 1; ok && sequence <= kBlocks; ++sequence)
        {
            const ULONGLONG deadline = GetTickCount64() + kTimeoutMs;
            while (!tryBeginAudioBlockWrite(slot))
            {
                if (GetTickCount64() >= deadline ||
                    WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT)
                { ok = false; break; }
                SwitchToThread();
            }
            if (!ok) break;
            slot.sequence = sequence;
            slot.generation = 3;
            slot.deadlineMonotonicNs = sequence;
            slot.sampleRate = 48000;
            slot.channels = 1;
            slot.frames = 1;
            slot.payloadSamples = 1;
            slot.samples[0] = static_cast<float>(sequence);
            if (!publishAudioBlockWrite(slot)) ok = false;
        }
        if (ok && WaitForSingleObject(process.value, kTimeoutMs) != WAIT_OBJECT_0)
            ok = false;
        DWORD exitCode = 1;
        if (ok && (!GetExitCodeProcess(process.value, &exitCode) || exitCode != 0))
            ok = false;
    }
    if (!ok && WaitForSingleObject(process.value, 0) == WAIT_TIMEOUT)
    {
        TerminateProcess(process.value, 2);
        WaitForSingleObject(process.value, 5000);
    }
    if (ok)
        std::wprintf(crash
            ? L"PASS: child process exited while owning READING; parent reclaimed slots after process signal\n"
            : L"PASS: %llu audio blocks published/consumed across processes\n",
            static_cast<unsigned long long>(kBlocks));
    else
        std::fwprintf(stderr, L"FAIL: cross-process audio slot scenario\n");
    return ok;
}
}

int wmain(int argc, wchar_t** argv)
{
    if (argc == 3 && (std::wcscmp(argv[1], L"--consume") == 0 ||
                      std::wcscmp(argv[1], L"--crash") == 0))
        return childMain(argv[2], std::wcscmp(argv[1], L"--crash") == 0);
    if (argc == 2 && std::wcscmp(argv[1], L"--worker-crash-recovery") == 0)
        return runScenario(true) ? 0 : 1;
    if (argc != 1) return 2;
    return runScenario(false) ? 0 : 1;
}
