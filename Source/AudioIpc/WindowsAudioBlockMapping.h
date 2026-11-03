#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <sddl.h>

#include "AudioBlockAbi.h"
#include "AudioBlockSlotSync.h"

#include <string>

namespace voice_studio::ipc
{
// Owns or opens the fixed-size audio transport mapping. Creation/opening and
// destruction are control-thread operations only. Audio callbacks must use
// only the already-mapped region and non-waiting slot primitives.
class WindowsAudioBlockMapping final
{
public:
    WindowsAudioBlockMapping() = default;
    WindowsAudioBlockMapping(const WindowsAudioBlockMapping&) = delete;
    WindowsAudioBlockMapping& operator=(const WindowsAudioBlockMapping&) = delete;
    ~WindowsAudioBlockMapping() { close(); }

    bool createServer(const std::wstring& name) noexcept
    {
        if (mapping != nullptr || name.empty())
            return false;

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
            return false;

        SECURITY_ATTRIBUTES attributes { sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE };
        HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes,
                                           PAGE_READWRITE, 0,
                                           static_cast<DWORD>(sizeof(AudioBlockRegion)),
                                           name.c_str());
        const DWORD error = GetLastError();
        LocalFree(descriptor);
        if (handle == nullptr || error == ERROR_ALREADY_EXISTS)
        {
            if (handle != nullptr) CloseHandle(handle);
            return false;
        }

        auto* view = static_cast<AudioBlockRegion*>(
            MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioBlockRegion)));
        if (view == nullptr)
        {
            CloseHandle(handle);
            return false;
        }

        ZeroMemory(view, sizeof(*view));
        view->header.magic = kAudioBlockMagic;
        view->header.version = kAudioBlockAbiVersion;
        view->header.headerSize = static_cast<std::uint32_t>(sizeof(AudioBlockRegionHeader));
        view->header.slotCount = kAudioBlockSlotCount;
        view->header.slotSize = static_cast<std::uint32_t>(sizeof(AudioBlockSlot));
        view->header.maxFrames = kAudioBlockMaxFrames;
        view->header.maxChannels = kAudioBlockMaxChannels;
        view->header.sampleFormat = kAudioBlockFloat32;
        view->header.reserved = 0;
        for (auto& slot : view->slots)
            InterlockedExchange(reinterpret_cast<volatile LONG*>(&slot.state),
                                static_cast<LONG>(AudioBlockSlotState::free));

        mapping = handle;
        region = view;
        owner = true;
        mappingName = name;
        return true;
    }

    bool openClient(const std::wstring& name) noexcept
    {
        if (mapping != nullptr || name.empty())
            return false;

        HANDLE handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name.c_str());
        if (handle == nullptr)
            return false;

        auto* view = static_cast<AudioBlockRegion*>(
            MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(AudioBlockRegion)));
        if (view == nullptr)
        {
            CloseHandle(handle);
            return false;
        }

        if (!hasValidAudioBlockHeader(view->header))
        {
            UnmapViewOfFile(view);
            CloseHandle(handle);
            return false;
        }

        mapping = handle;
        region = view;
        owner = false;
        mappingName = name;
        return true;
    }

    void close() noexcept
    {
        if (region != nullptr)
            UnmapViewOfFile(region);
        if (mapping != nullptr)
            CloseHandle(mapping);
        region = nullptr;
        mapping = nullptr;
        owner = false;
        mappingName.clear();
    }

    AudioBlockRegion* get() noexcept { return region; }
    const AudioBlockRegion* get() const noexcept { return region; }
    const std::wstring& name() const noexcept { return mappingName; }
    bool isOwner() const noexcept { return owner; }

private:
    HANDLE mapping = nullptr;
    AudioBlockRegion* region = nullptr;
    std::wstring mappingName;
    bool owner = false;
};
} // namespace voice_studio::ipc
