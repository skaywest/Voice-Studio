#include "AudioIpc/WindowsSharedMemoryAbi.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace
{
void check(bool condition, const char* expression, int line)
{
    if (!condition)
    {
        std::fprintf(stderr, "CHECK failed at line %d: %s\\n", line, expression);
        std::abort();
    }
}
}

#define CHECK(expression) check((expression), #expression, __LINE__)

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
    CHECK(voice_studio::ipc::hasValidSharedMemoryHeader(region));
}

void testRejectsEachInvalidField()
{
    auto region = makeValidRegion();
    region.magic ^= 1;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.version;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.headerSize;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.slotCount;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.slotSize;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    ++region.sampleCount;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));

    region = makeValidRegion();
    region.reserved = 1;
    CHECK(!voice_studio::ipc::hasValidSharedMemoryHeader(region));
}
}

int main()
{
    testValidHeader();
    testRejectsEachInvalidField();
    return 0;
}
