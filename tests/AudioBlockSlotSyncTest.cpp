#include "AudioIpc/AudioBlockSlotSync.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

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

using namespace voice_studio::ipc;

void testSingleProducerConsumerTransitions()
{
    AudioBlockSlot slot {};
    CHECK(tryBeginAudioBlockWrite(slot));
    CHECK(!tryBeginAudioBlockWrite(slot));
    slot.sequence = 1;
    slot.generation = 1;
    slot.deadlineMonotonicNs = 100;
    slot.sampleRate = 48000;
    slot.channels = 2;
    slot.frames = 960;
    slot.payloadSamples = 1920;
    slot.samples[0] = 0.25f;

    CHECK(publishAudioBlockWrite(slot));
    CHECK(!publishAudioBlockWrite(slot));
    CHECK(tryBeginAudioBlockRead(slot));
    CHECK(!tryBeginAudioBlockRead(slot));
    CHECK(slot.samples[0] == 0.25f);
    CHECK(finishAudioBlockRead(slot));
    CHECK(!finishAudioBlockRead(slot));
    CHECK(slot.state == static_cast<std::int32_t>(AudioBlockSlotState::free));
}

void testOnlyOneConcurrentWriterWins()
{
    AudioBlockSlot slot {};
    constexpr int threadCount = 32;
    std::atomic<bool> start { false };
    std::atomic<int> winners { 0 };
    std::vector<std::thread> threads;
    threads.reserve(threadCount);

    for (int i = 0; i < threadCount; ++i)
    {
        threads.emplace_back([&]
        {
            while (!start.load(std::memory_order_acquire))
                std::this_thread::yield();
            if (tryBeginAudioBlockWrite(slot))
                winners.fetch_add(1, std::memory_order_relaxed);
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& thread : threads)
        thread.join();

    CHECK(winners.load(std::memory_order_relaxed) == 1);
    CHECK(slot.state == static_cast<std::int32_t>(AudioBlockSlotState::writing));
}

void testRecoveryAfterWorkerExit()
{
    AudioBlockRegion region {};
    region.slots[0].state = static_cast<std::int32_t>(AudioBlockSlotState::writing);
    region.slots[1].state = static_cast<std::int32_t>(AudioBlockSlotState::ready);
    region.slots[2].state = static_cast<std::int32_t>(AudioBlockSlotState::reading);
    region.slots[3].state = static_cast<std::int32_t>(AudioBlockSlotState::free);

    // Unit-level state reset. Production callers must first observe the worker
    // process handle become signaled; this test does not simulate that OS event.
    reclaimAudioBlockSlotsAfterWorkerExit(region);
    for (const auto& slot : region.slots)
        CHECK(slot.state == static_cast<std::int32_t>(AudioBlockSlotState::free));
}
int main()
{
    testSingleProducerConsumerTransitions();
    testOnlyOneConcurrentWriterWins();
    testRecoveryAfterWorkerExit();
    return 0;
}
