#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace voice_studio::ipc
{
inline constexpr std::uint64_t kSharedMemoryMagic = 0x5653545544494F31ull;
inline constexpr std::uint32_t kSharedMemoryAbiVersion = 2;
inline constexpr std::size_t kSharedMemorySlotCount = 8;
inline constexpr std::size_t kSharedMemorySampleCount = 64;

struct SharedMemorySlot
{
    alignas(8) volatile std::int64_t sequence;
    std::uint64_t block;
    std::array<float, kSharedMemorySampleCount> samples;
};

struct SharedMemoryRegion
{
    std::uint64_t magic;
    std::uint32_t version;
    std::uint32_t headerSize;
    std::uint32_t slotCount;
    std::uint32_t slotSize;
    std::uint32_t sampleCount;
    std::uint32_t reserved;
    SharedMemorySlot slots[kSharedMemorySlotCount];
};

static_assert(alignof(SharedMemorySlot) >= 8, "Interlocked sequence must be 8-byte aligned");
static_assert(alignof(SharedMemoryRegion) >= 8, "Shared region must preserve Interlocked alignment");
static_assert(offsetof(SharedMemorySlot, sequence) == 0, "Slot sequence offset is part of the ABI");
static_assert(offsetof(SharedMemorySlot, block) == 8, "Slot block offset is part of the ABI");
static_assert(offsetof(SharedMemorySlot, samples) == 16, "Slot payload offset is part of the ABI");
static_assert(sizeof(SharedMemorySlot) == 16 + sizeof(float) * kSharedMemorySampleCount,
              "Slot size is part of the ABI");
static_assert(offsetof(SharedMemoryRegion, magic) == 0, "Header magic offset is part of the ABI");
static_assert(offsetof(SharedMemoryRegion, version) == 8, "Header version offset is part of the ABI");
static_assert(offsetof(SharedMemoryRegion, reserved) == 28, "Header reserved offset is part of the ABI");
static_assert(offsetof(SharedMemoryRegion, slots) == 32, "Header size is part of the ABI");
static_assert(std::is_trivially_copyable<SharedMemorySlot>::value, "Slot ABI must be trivially copyable");
static_assert(std::is_trivially_copyable<SharedMemoryRegion>::value, "Shared ABI must be trivially copyable");
static_assert(std::is_standard_layout<SharedMemorySlot>::value, "Slot ABI must be standard layout");
static_assert(std::is_standard_layout<SharedMemoryRegion>::value, "Shared ABI must be standard layout");
static_assert(offsetof(SharedMemoryRegion, slots) ==
                  sizeof(SharedMemoryRegion) - sizeof(SharedMemorySlot) * kSharedMemorySlotCount,
              "Shared header must have a stable contiguous layout");
static_assert(sizeof(SharedMemorySlot) <= (std::numeric_limits<std::uint32_t>::max)(),
              "Slot size must fit the ABI field");
static_assert(sizeof(SharedMemoryRegion) <= (std::numeric_limits<std::uint32_t>::max)(),
              "Region size must fit the ABI field");

inline bool hasValidSharedMemoryHeader(const SharedMemoryRegion& region) noexcept
{
    return region.magic == kSharedMemoryMagic &&
           region.version == kSharedMemoryAbiVersion &&
           region.headerSize == offsetof(SharedMemoryRegion, slots) &&
           region.slotCount == kSharedMemorySlotCount &&
           region.slotSize == sizeof(SharedMemorySlot) &&
           region.sampleCount == kSharedMemorySampleCount &&
           region.reserved == 0;
}
} // namespace voice_studio::ipc
