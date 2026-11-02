#include "AudioIpc/SpscAudioQueue.h"

#include <cmath>
#include <cstdint>
#include <iostream>

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
}

int main()
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
           "interleaved channel/frame data survives queue transfer");

    expect(queue.tryPush(a), "queue accepts data after consumer frees a slot");
    expect(queue.tryPop(received) && received.sequence == 11, "FIFO order preserved for second block");
    expect(queue.tryPop(received) && received.sequence == 12, "FIFO order preserved for third block");
    expect(queue.tryPop(received) && received.sequence == 10, "queue wraps around and preserves order");
    expect(!queue.tryPop(received), "queue is empty after draining");

    if (failures != 0)
    {
        std::cerr << failures << " audio queue test(s) failed\n";
        return 1;
    }

    std::cout << "All audio queue tests passed\n";
    return 0;
}
