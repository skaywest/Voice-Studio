#include "AudioIpc/WindowsSharedMemoryAbi.h"

#include <cassert>
#include <cstdint>

using voice_studio::ipc::SharedMemoryRegion;

namespace
{
SharedMemoryRegion makeValidRegion()
{
    SharedMemoryRegion region {};
    region.magic = voice_studio::ipc::kSharedMemoryMagic;
    region.version = voice_studio::ipc::kSharedMemoryAbiVersion;
    region.headerSize = static_cast<std::uint32_t>(offsetof(SharedMemoryRegion, slots));
    region.slotCount = static_cast<std::uint32_t>(voice_studio::ipc::kSharedMemorySlotCount);
    region.slotSize = static_cast<std::uint32_t>(sizeof(voice_studio::ipc::SharedMemorySlot));
    region.sampleCount = static_cast<std::uint32_t>(voice_studio::ipc::kSharedMemorySampleCount);
    return region;
}

void testValidHeader()
{
    const auto region = makeValidRegion();
    assert(voice_studio::ipc::hasValidSharedMemoryHeader(region));
}

void testRejectsEachInvalidField()
{
    auto region = makeValidRegion();
    region.magic ^= 1;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.version;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.headerSize;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.slotCount;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.slotSize;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.sampleCount;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    region.reserved = 1;
    assert(!voice_studio::ipc::hasValidSharedMemoryHeader(region));
}
}

int main()
{
    testValidHeader();
    testRejectsEachInvalidField();
    return 0;
}
