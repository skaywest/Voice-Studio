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

        std::thread producer([&queue]
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

        std::thread consumer([&queue, &consumerFailed]
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
