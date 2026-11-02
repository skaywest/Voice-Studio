#include "AudioIpc/SpscAudioQueue.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <thread>

namespace
{
    int failures = 0;

    void expect(bool condition, const char* message)
    {
        if (condition)
            std::cout << "PASS: " << message << '\n';
        else
        {
            std::cerr << "FAIL: " << message << '\n';
            ++failures;
        }
    }

    void testAudioBlockDimensions()
    {
        using Block = voice_studio::AudioBlock<8, 2>;
        Block block;
        expect(!block.hasValidDimensions(), "zero-sized audio block metadata is rejected");

        block.frames = 1;
        block.channels = 1;
        expect(block.hasValidDimensions(), "minimum audio block dimensions are accepted");

        block.frames = 8;
        block.channels = 2;
        expect(block.hasValidDimensions(), "maximum audio block dimensions are accepted");

        block.frames = 9;
        expect(!block.hasValidDimensions(), "frame count above storage capacity is rejected");

        block.frames = 8;
        block.channels = 3;
        expect(!block.hasValidDimensions(), "channel count above storage capacity is rejected");

        block.frames = 0;
        block.channels = 2;
        expect(!block.hasValidDimensions(), "zero frame count is rejected");

        block.frames = 8;
        block.channels = 0;
        expect(!block.hasValidDimensions(), "zero channel count is rejected");
    }

    void testRepeatedOverflowDoesNotOverwrite()
    {
        using Block = voice_studio::AudioBlock<4, 1>;
        voice_studio::SpscQueue<Block, 2> queue;

        Block first;
        first.frames = 4;
        first.channels = 1;
        first.sequence = 100;
        first.at(0, 0) = 1.0f;

        Block second = first;
        second.sequence = 101;
        second.at(0, 0) = 2.0f;

        Block rejected = first;
        rejected.sequence = 999;
        rejected.at(0, 0) = 9.0f;

        expect(queue.tryPush(first) && queue.tryPush(second),
               "producer fills the bounded queue before overflow stress");

        bool allRejected = true;
        for (int attempt = 0; attempt < 10000; ++attempt)
            allRejected = queue.tryPush(rejected) == false && allRejected;

        expect(allRejected, "full queue consistently rejects 10000 overflow attempts");

        Block received;
        const bool firstPopped = queue.tryPop(received);
        expect(firstPopped && received.sequence == 100 && received.at(0, 0) == 1.0f,
               "first unread block remains intact after repeated overflow");

        const bool secondPopped = queue.tryPop(received);
        expect(secondPopped && received.sequence == 101 && received.at(0, 0) == 2.0f,
               "second unread block remains intact after repeated overflow");

        expect(!queue.tryPop(received), "rejected overflow blocks never enter the queue");
        expect(queue.tryPush(rejected), "queue resumes accepting blocks after capacity is freed");
    }

    void testSingleThreadedBehavior()
    {
        using Block = voice_studio::AudioBlock<8, 2>;
        voice_studio::SpscQueue<Block, 3> queue;

        Block output;
        expect(!queue.tryPop(output), "empty queue rejects pop");
        expect(queue.approximateSize() == 0, "empty queue reports zero size");

        Block a;
        a.frames = 8;
        a.channels = 2;
        a.sequence = 10;
        a.at(0, 0) = 0.25f;
        a.at(1, 7) = -0.5f;

        Block b = a;
        b.sequence = 11;
        b.at(0, 0) = 0.75f;

        Block c = a;
        c.sequence = 12;

        expect(queue.tryPush(a), "first block is queued");
        expect(queue.tryPush(b), "second block is queued");
        expect(queue.tryPush(c), "third block fills queue");
        expect(!queue.tryPush(a), "full queue rejects new block without overwriting unread audio");
        expect(queue.approximateSize() == 3, "full queue reports capacity");

        Block received;
        expect(queue.tryPop(received), "pop succeeds on non-empty queue");
        expect(received.sequence == 10 && received.frames == 8 && received.channels == 2,
               "block metadata survives queue transfer");
        expect(std::abs(received.at(0, 0) - 0.25f) < 0.000001f
                   && std::abs(received.at(1, 7) + 0.5f) < 0.000001f,
               "audio sample data survives queue transfer");

        expect(queue.tryPush(a), "queue accepts data after consumer frees a slot");
        expect(queue.tryPop(received) && received.sequence == 11, "FIFO order preserved for second block");
        expect(queue.tryPop(received) && received.sequence == 12, "FIFO order preserved for third block");
        expect(queue.tryPop(received) && received.sequence == 10, "queue wraps around and preserves order");
        expect(!queue.tryPop(received), "queue is empty after draining");
    }

    void testConcurrentProducerConsumer()
    {
        constexpr std::uint64_t blockCount = 1'000'000;
        using Block = voice_studio::AudioBlock<4, 1>;
        voice_studio::SpscQueue<Block, 64> queue;
        std::atomic<bool> consumerFailed { false };

        std::thread producer([&queue, blockCount]
        {
            for (std::uint64_t sequence = 0; sequence < blockCount; ++sequence)
            {
                Block block;
                block.frames = 4;
                block.channels = 1;
                block.sequence = sequence;
                const float marker = static_cast<float>(sequence % 10000);
                for (std::size_t frame = 0; frame < 4; ++frame)
                    block.at(0, frame) = marker;

                while (!queue.tryPush(block))
                    std::this_thread::yield();
            }
        });

        std::thread consumer([&queue, &consumerFailed, blockCount]
        {
            Block block;
            for (std::uint64_t expected = 0; expected < blockCount; ++expected)
            {
                while (!queue.tryPop(block))
                    std::this_thread::yield();

                const float marker = static_cast<float>(expected % 10000);
                if (block.sequence != expected || block.frames != 4 || block.channels != 1)
                    consumerFailed.store(true, std::memory_order_relaxed);

                for (std::size_t frame = 0; frame < 4; ++frame)
                {
                    if (block.at(0, frame) != marker)
                        consumerFailed.store(true, std::memory_order_relaxed);
                }
            }
        });

        producer.join();
        consumer.join();
        expect(!consumerFailed.load(std::memory_order_relaxed),
               "concurrent producer/consumer preserves one million blocks in FIFO order");
        expect(queue.approximateSize() == 0, "concurrent queue is empty after stress test");
    }
}

int main()
{
#if defined(_WIN64) || defined(__x86_64__)
    expect(std::atomic<std::uint64_t>::is_always_lock_free,
           "64-bit atomics are always lock-free on supported x64 builds");
#endif

    testAudioBlockDimensions();
    testRepeatedOverflowDoesNotOverwrite();
    testSingleThreadedBehavior();
    testConcurrentProducerConsumer();

    if (failures != 0)
    {
        std::cerr << failures << " audio queue test(s) failed\n";
        return 1;
    }

    std::cout << "All audio queue tests passed\n";
    return 0;
}
