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


void testProducerConsumerPublicationStress()
{
    AudioBlockSlot slot {};
    constexpr std::uint64_t blockCount = 100000;
    std::atomic<bool> producerDone { false };
    std::atomic<bool> valid { true };

    std::thread producer([&]
    {
        for (std::uint64_t sequence = 1; sequence <= blockCount; ++sequence)
        {
            while (!tryBeginAudioBlockWrite(slot))
                std::this_thread::yield();

            slot.sequence = sequence;
            slot.generation = 7;
            slot.deadlineMonotonicNs = sequence + 100;
            slot.sampleRate = 48000;
            slot.channels = 2;
            slot.frames = 1;
            slot.payloadSamples = 2;
            slot.samples[0] = static_cast<float>(sequence);
            slot.samples[1] = -static_cast<float>(sequence);

            while (!publishAudioBlockWrite(slot))
                std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::uint64_t consumed = 0;
    while (consumed < blockCount)
    {
        if (!tryBeginAudioBlockRead(slot))
        {
            if (producerDone.load(std::memory_order_acquire) &&
                slot.state == static_cast<std::int32_t>(AudioBlockSlotState::free))
                break;
            std::this_thread::yield();
            continue;
        }

        const auto expected = consumed + 1;
        if (slot.sequence != expected ||
            slot.generation != 7 ||
            slot.payloadSamples != 2 ||
            slot.samples[0] != static_cast<float>(expected) ||
            slot.samples[1] != -static_cast<float>(expected))
            valid.store(false, std::memory_order_relaxed);

        if (!finishAudioBlockRead(slot))
            valid.store(false, std::memory_order_relaxed);
        ++consumed;
    }

    producer.join();
    CHECK(valid.load(std::memory_order_relaxed));
    CHECK(consumed == blockCount);
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
    testProducerConsumerPublicationStress();
    testRecoveryAfterWorkerExit();
    return 0;
}
