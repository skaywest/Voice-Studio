#include "AudioIpc/AudioBlockSlotSync.h"
#include "AudioIpc/AudioBlockTransport.h"

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


void initialiseAudioRegion(AudioBlockRegion& region)
{
    region.header.magic = kAudioBlockMagic;
    region.header.version = kAudioBlockAbiVersion;
    region.header.headerSize = sizeof(AudioBlockRegionHeader);
    region.header.slotCount = kAudioBlockSlotCount;
    region.header.slotSize = sizeof(AudioBlockSlot);
    region.header.maxFrames = kAudioBlockMaxFrames;
    region.header.maxChannels = kAudioBlockMaxChannels;
    region.header.sampleFormat = kAudioBlockFloat32;
    region.header.reserved = 0;
}

void testRealtimeTransportPublishAndConsume()
{
    AudioBlockRegion region {};
    initialiseAudioRegion(region);
    const float input[] = { -0.8f, -0.4f, 0.0f, 0.4f, 0.8f, 0.2f };
    CHECK(tryPublishInputAudioBlock(region, input, 3, 2, 48000, 1, 9, 1000));
    auto& source = region.slots[kAudioBlockInputSlotBegin];
    CHECK(tryBeginAudioBlockRead(source));
    CHECK(hasFiniteAudioBlockPayload(source));
    CHECK(source.sequence == 1 && source.generation == 9);
    auto& output = region.slots[kAudioBlockOutputSlotBegin];
    CHECK(tryBeginAudioBlockWrite(output));
    output.flags = source.flags;
    output.sequence = source.sequence;
    output.generation = source.generation;
    output.deadlineMonotonicNs = source.deadlineMonotonicNs;
    output.sampleRate = source.sampleRate;
    output.channels = source.channels;
    output.frames = source.frames;
    output.payloadSamples = source.payloadSamples;
    for (std::uint32_t i = 0; i < source.payloadSamples; ++i)
        output.samples[i] = source.samples[i];
    CHECK(publishAudioBlockWrite(output));
    CHECK(finishAudioBlockRead(source));

    float result[6] {};
    CHECK(tryConsumeOutputAudioBlock(region, result, 6, 3, 2, 9, 999));
    for (std::size_t i = 0; i < 6; ++i)
        CHECK(result[i] == input[i]);
    CHECK(!tryConsumeOutputAudioBlock(region, result, 6, 3, 2, 9, 999));
}

void testRealtimeTransportRejectsInvalidAndStaleData()
{
    AudioBlockRegion region {};
    initialiseAudioRegion(region);
    const float samples[] = { 0.1f, -0.1f };
    CHECK(!tryPublishInputAudioBlock(region, samples, 1, 2, 48000, 0, 1, 100));
    CHECK(tryPublishInputAudioBlock(region, samples, 1, 2, 48000, 1, 1, 100));
    float result[2] { 7.0f, 8.0f };
    CHECK(!tryConsumeOutputAudioBlock(region, result, 2, 1, 2, 1, 50));
    CHECK(!tryConsumeOutputAudioBlock(region, result, 2, 1, 2, 2, 50));
    CHECK(result[0] == 7.0f && result[1] == 8.0f);

    auto& output = region.slots[kAudioBlockOutputSlotBegin];
    CHECK(tryBeginAudioBlockWrite(output));
    output.flags = 0;
    output.sequence = 2;
    output.generation = 1;
    output.deadlineMonotonicNs = 99;
    output.sampleRate = 48000;
    output.channels = 2;
    output.frames = 1;
    output.payloadSamples = 2;
    output.samples[0] = 0.3f;
    output.samples[1] = -0.3f;
    CHECK(publishAudioBlockWrite(output));
    CHECK(!tryConsumeOutputAudioBlock(region, result, 2, 1, 2, 1, 100));
    CHECK(output.state == static_cast<std::int32_t>(AudioBlockSlotState::free));
    CHECK(result[0] == 7.0f && result[1] == 8.0f);
}

void testRealtimeTransportQueueOverflowIsNonDestructive()
{
    AudioBlockRegion region {};
    initialiseAudioRegion(region);
    const float sample[] = { 0.5f };
    for (std::uint64_t sequence = 1; sequence <= kAudioBlockInputSlotCount; ++sequence)
        CHECK(tryPublishInputAudioBlock(region, sample, 1, 1, 48000, sequence, 3, 1000));
    CHECK(!tryPublishInputAudioBlock(region, sample, 1, 1, 48000, 99, 3, 1000));
    for (std::uint32_t i = 0; i < kAudioBlockInputSlotCount; ++i)
        CHECK(region.slots[i].sequence == i + 1);
}

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
    testRealtimeTransportPublishAndConsume();
    testRealtimeTransportRejectsInvalidAndStaleData();
    testRealtimeTransportQueueOverflowIsNonDestructive();
    testSingleProducerConsumerTransitions();
    testOnlyOneConcurrentWriterWins();
    testProducerConsumerPublicationStress();
    testRecoveryAfterWorkerExit();
    return 0;
}
