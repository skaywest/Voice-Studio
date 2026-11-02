#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace voice_studio
{
template <std::size_t MaxFrames, std::size_t MaxChannels>
struct AudioBlock
{
    static_assert(MaxFrames > 0 && MaxChannels > 0, "Audio block dimensions must be non-zero");

    std::uint32_t frames = 0;
    std::uint32_t channels = 0;
    std::uint64_t sequence = 0;
    std::array<float, MaxFrames * MaxChannels> samples {};

    float& at(std::size_t channel, std::size_t frame) noexcept
    {
        return samples[channel * MaxFrames + frame];
    }

    const float& at(std::size_t channel, std::size_t frame) const noexcept
    {
        return samples[channel * MaxFrames + frame];
    }
};

// Single-producer / single-consumer queue. Exactly one thread may call tryPush()
// and exactly one different thread may call tryPop(). No allocation, locks,
// waits, or system calls occur in either operation.
template <typename T, std::size_t Capacity>
class SpscQueue
{
    static_assert(Capacity >= 2, "SPSC queue needs at least two slots");
    static_assert(std::is_trivially_copyable<T>::value,
                  "Queue items must be trivially copyable");

public:
    bool tryPush(const T& item) noexcept
    {
        const auto write = writeIndex.load(std::memory_order_relaxed);
        const auto read = readIndex.load(std::memory_order_acquire);
        if (write - read >= Capacity)
            return false;

        slots[static_cast<std::size_t>(write % Capacity)] = item;
        writeIndex.store(write + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(T& item) noexcept
    {
        const auto read = readIndex.load(std::memory_order_relaxed);
        const auto write = writeIndex.load(std::memory_order_acquire);
        if (read == write)
            return false;

        item = slots[static_cast<std::size_t>(read % Capacity)];
        readIndex.store(read + 1, std::memory_order_release);
        return true;
    }

    // This is a diagnostic snapshot, not a synchronization primitive. The
    // two atomic loads are not a single coherent snapshot: the consumer may
    // advance readIndex after writeIndex was sampled, or the producer may
    // advance writeIndex after readIndex was sampled. Clamp either transient
    // inconsistency to the queue's valid occupancy range.
    std::size_t approximateSize() const noexcept
    {
        const auto write = writeIndex.load(std::memory_order_acquire);
        const auto read = readIndex.load(std::memory_order_acquire);
        if (read > write)
            return 0;

        const auto distance = write - read;
        return distance > Capacity ? Capacity : static_cast<std::size_t>(distance);
    }

private:
    alignas(64) std::array<T, Capacity> slots {};
    alignas(64) std::atomic<std::uint64_t> writeIndex { 0 };
    alignas(64) std::atomic<std::uint64_t> readIndex { 0 };
};

} // namespace voice_studio
