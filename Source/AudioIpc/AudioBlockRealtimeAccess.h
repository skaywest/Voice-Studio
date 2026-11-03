#pragma once

#include "AudioBlockAbi.h"

#include <atomic>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace voice_studio::ipc
{
// Pins the mapped region while an audio callback uses it. Acquiring/releasing
// is lock-free; only the control thread may attach or detach a mapping.
// detach() must never be called from the audio callback: it waits for all
// outstanding AudioAccess leases before the owner unmaps shared memory.
class AudioBlockRealtimeAccess final
{
public:
    class AudioAccess final
    {
    public:
        AudioAccess() noexcept = default;
        AudioAccess(const AudioAccess&) = delete;
        AudioAccess& operator=(const AudioAccess&) = delete;

        AudioAccess(AudioAccess&& other) noexcept
            : owner(other.owner), region(other.region)
        {
            other.owner = nullptr;
            other.region = nullptr;
        }

        AudioAccess& operator=(AudioAccess&& other) noexcept
        {
            if (this != &other)
            {
                release();
                owner = other.owner;
                region = other.region;
                other.owner = nullptr;
                other.region = nullptr;
            }
            return *this;
        }

        ~AudioAccess() { release(); }

        explicit operator bool() const noexcept { return region != nullptr; }
        AudioBlockRegion* get() const noexcept { return region; }
        AudioBlockRegion& operator*() const noexcept { return *region; }
        AudioBlockRegion* operator->() const noexcept { return region; }

        void release() noexcept
        {
            if (owner != nullptr)
            {
                owner->activeReaders.fetch_sub(1, std::memory_order_seq_cst);
                owner = nullptr;
                region = nullptr;
            }
        }

    private:
        friend class AudioBlockRealtimeAccess;
        AudioAccess(AudioBlockRealtimeAccess* bridge, AudioBlockRegion* mapped) noexcept
            : owner(bridge), region(mapped) {}

        AudioBlockRealtimeAccess* owner = nullptr;
        AudioBlockRegion* region = nullptr;
    };

    AudioBlockRealtimeAccess() = default;
    AudioBlockRealtimeAccess(const AudioBlockRealtimeAccess&) = delete;
    AudioBlockRealtimeAccess& operator=(const AudioBlockRealtimeAccess&) = delete;

    // Control thread only. The mapping owner must keep the region alive until
    // detach() has returned.
    bool attach(AudioBlockRegion* mappedRegion) noexcept
    {
        if (mappedRegion == nullptr || activeReaders.load(std::memory_order_seq_cst) != 0)
            return false;
        AudioBlockRegion* expected = nullptr;
        return region.compare_exchange_strong(expected, mappedRegion,
                                               std::memory_order_seq_cst,
                                               std::memory_order_seq_cst);
    }

    // Audio thread: one bounded increment and one pointer load; no locks,
    // allocation, IPC, waits, or OS calls.
    AudioAccess tryAcquire() noexcept
    {
        activeReaders.fetch_add(1, std::memory_order_seq_cst);
        auto* mapped = region.load(std::memory_order_seq_cst);
        if (mapped == nullptr)
        {
            activeReaders.fetch_sub(1, std::memory_order_seq_cst);
            return {};
        }
        return AudioAccess(this, mapped);
    }

    // Control thread only. Clears publication first, then waits until every
    // callback that could have observed the old pointer has released its lease.
    AudioBlockRegion* detach() noexcept
    {
        auto* previous = region.exchange(nullptr, std::memory_order_seq_cst);
        while (activeReaders.load(std::memory_order_seq_cst) != 0)
            std::this_thread::yield();
        return previous;
    }

    bool isAttached() const noexcept
    {
        return region.load(std::memory_order_seq_cst) != nullptr;
    }

    static constexpr bool atomicsAreAlwaysLockFree() noexcept
    {
        return std::atomic<AudioBlockRegion*>::is_always_lock_free &&
               std::atomic<std::uint32_t>::is_always_lock_free;
    }

private:
    std::atomic<AudioBlockRegion*> region { nullptr };
    std::atomic<std::uint32_t> activeReaders { 0 };
};

static_assert(AudioBlockRealtimeAccess::atomicsAreAlwaysLockFree(),
              "Realtime mapping access requires lock-free pointer and reader-count atomics");
} // namespace voice_studio::ipc
