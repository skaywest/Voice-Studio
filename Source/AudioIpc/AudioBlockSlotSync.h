#pragma once

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "AudioIpc/AudioBlockAbi.h"

#include <cstdint>

namespace voice_studio::ipc
{
// All transitions are single, non-waiting Interlocked operations. The Windows
// Interlocked primitives provide the required cross-process ordering for the
// shared mapping; do not replace them with std::atomic in this wire ABI.
inline bool tryBeginAudioBlockWrite(AudioBlockSlot& slot) noexcept
{
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&slot.state),
        static_cast<LONG>(AudioBlockSlotState::writing),
        static_cast<LONG>(AudioBlockSlotState::free)) ==
        static_cast<LONG>(AudioBlockSlotState::free);
}

inline bool publishAudioBlockWrite(AudioBlockSlot& slot) noexcept
{
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&slot.state),
        static_cast<LONG>(AudioBlockSlotState::ready),
        static_cast<LONG>(AudioBlockSlotState::writing)) ==
        static_cast<LONG>(AudioBlockSlotState::writing);
}

inline bool tryBeginAudioBlockRead(AudioBlockSlot& slot) noexcept
{
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&slot.state),
        static_cast<LONG>(AudioBlockSlotState::reading),
        static_cast<LONG>(AudioBlockSlotState::ready)) ==
        static_cast<LONG>(AudioBlockSlotState::ready);
}

inline bool finishAudioBlockRead(AudioBlockSlot& slot) noexcept
{
    return InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&slot.state),
        static_cast<LONG>(AudioBlockSlotState::free),
        static_cast<LONG>(AudioBlockSlotState::reading)) ==
        static_cast<LONG>(AudioBlockSlotState::reading);
}

// Call only on the control thread after the worker process handle is signaled.
// The process-death proof is a precondition: reclaiming while a worker is alive
// could revoke a slot that it is still writing or reading. This invalidates
// READY blocks too; the next worker session must use a new generation.
inline void reclaimAudioBlockSlotsAfterWorkerExit(AudioBlockRegion& region) noexcept
{
    for (auto& slot : region.slots)
        InterlockedExchange(
            reinterpret_cast<volatile LONG*>(&slot.state),
            static_cast<LONG>(AudioBlockSlotState::free));
}
} // namespace voice_studio::ipc
