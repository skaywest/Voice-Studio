#include "AudioIpc/RvcAudioStreamAdapter.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "CHECK failed: %s at %d\n", #expression, __LINE__); std::abort(); } } while (false)

using namespace voice_studio::ipc;

int main()
{
    RvcAudioStreamAdapter adapter;
    constexpr auto block = RvcAudioStreamAdapter::kBlockFrames;

    // Arbitrary host callback sizes accumulate into exactly one RVC block.
    std::vector<float> left(1024, 0.4f), right(1024, 0.2f);
    CHECK(adapter.pushHostInput(left.data(), right.data(), 1024, 2, 1));
    CHECK(adapter.pendingInputBlocks() == 0);
    CHECK(adapter.pushHostInput(left.data(), right.data(), 1024, 2, 1));
    CHECK(adapter.pendingInputBlocks() == 0);
    CHECK(adapter.pushHostInput(left.data(), right.data(), 832, 2, 1));
    CHECK(adapter.pendingInputBlocks() == 1);

    RvcAudioStreamAdapter::InputBlock input{};
    CHECK(adapter.tryPopInputBlock(input));
    CHECK(input.sequence == 1 && input.generation == 1);
    for (const auto sample : input.samples)
        CHECK(std::abs(sample - 0.3f) < 0.000001f);
    CHECK(!adapter.tryPopInputBlock(input));

    // Mono input also works, and a generation change discards partial old audio.
    // The backing buffer must cover the largest frame count passed below.
    std::vector<float> mono(1880, 0.75f);
    CHECK(adapter.pushHostInput(mono.data(), nullptr, 1200, 1, 1));
    CHECK(adapter.pushHostInput(mono.data(), nullptr, 1000, 1, 2));
    CHECK(adapter.pushHostInput(mono.data(), nullptr, 1880, 1, 2));
    CHECK(adapter.tryPopInputBlock(input));
    CHECK(input.sequence == 2 && input.generation == 2);
    for (const auto sample : input.samples)
        CHECK(std::abs(sample - 0.75f) < 0.000001f);

    // Bounded input queue drops complete blocks rather than growing a backlog.
    RvcAudioStreamAdapter overflow;
    std::vector<float> full(block, 0.1f);
    CHECK(overflow.pushHostInput(full.data(), nullptr, block, 1, 3));
    CHECK(overflow.pushHostInput(full.data(), nullptr, block, 1, 3));
    CHECK(overflow.pushHostInput(full.data(), nullptr, block, 1, 3));
    CHECK(overflow.pendingInputBlocks() == RvcAudioStreamAdapter::kQueueCapacity);
    CHECK(overflow.droppedInputBlockCount() == 1);
    CHECK(overflow.tryPopInputBlock(input));
    CHECK(input.sequence == 1);
    CHECK(overflow.tryPopInputBlock(input));
    CHECK(input.sequence == 2);
    CHECK(!overflow.tryPopInputBlock(input));

    // Non-finite microphone input is rejected and the partial block is reset.
    RvcAudioStreamAdapter invalid;
    std::vector<float> bad(64, 0.2f);
    bad[12] = std::numeric_limits<float>::infinity();
    CHECK(!invalid.pushHostInput(bad.data(), nullptr, 64, 1, 4));
    CHECK(invalid.invalidInputBlockCount() == 1);
    CHECK(invalid.pushHostInput(full.data(), nullptr, block, 1, 4));
    CHECK(invalid.tryPopInputBlock(input));
    CHECK(input.sequence == 1);

    // Output is delivered across callback sizes, preserving order and sequence.
    RvcAudioStreamAdapter output;
    std::vector<float> converted(block, 0.5f);
    CHECK(output.publishConvertedBlock(converted.data(), block, 11, 7));
    std::vector<float> first(128), second(512), rest(block - 640);
    auto a = output.pullConvertedOutput(first.data(), 128, 7);
    auto b = output.pullConvertedOutput(second.data(), 512, 7);
    auto c = output.pullConvertedOutput(rest.data(), block - 640, 7);
    CHECK(a.frames == 128 && a.sequence == 11);
    CHECK(b.frames == 512 && b.sequence == 11);
    CHECK(c.frames == block - 640 && c.sequence == 11);
    for (const auto sample : first) CHECK(sample == 0.5f);
    for (const auto sample : second) CHECK(sample == 0.5f);
    for (const auto sample : rest) CHECK(sample == 0.5f);
    CHECK(output.pendingOutputBlocks() == 0);

    // Underflow is non-blocking and reports no converted samples.
    auto underflow = output.pullConvertedOutput(first.data(), 128, 7);
    CHECK(underflow.frames == 0);

    // Old-generation output is discarded; it is never replayed into a new stream.
    CHECK(output.publishConvertedBlock(converted.data(), block, 12, 7));
    CHECK(output.publishConvertedBlock(converted.data(), block, 13, 8));
    auto fresh = output.pullConvertedOutput(first.data(), 128, 8);
    CHECK(fresh.frames == 128 && fresh.sequence == 13);
    CHECK(output.discardedStaleOutputBlockCount() == 1);

    // Switching generation while an output block is only partially consumed
    // must discard its remainder instead of leaking old voice audio.
    RvcAudioStreamAdapter midBlockReset;
    CHECK(midBlockReset.publishConvertedBlock(converted.data(), block, 21, 10));
    std::vector<float> partial(256), afterReset(256);
    auto oldPart = midBlockReset.pullConvertedOutput(partial.data(), 256, 10);
    CHECK(oldPart.frames == 256 && oldPart.sequence == 21);
    CHECK(midBlockReset.publishConvertedBlock(converted.data(), block, 22, 11));
    auto newPart = midBlockReset.pullConvertedOutput(afterReset.data(), 256, 11);
    CHECK(newPart.frames == 256 && newPart.sequence == 22);
    CHECK(midBlockReset.discardedStaleOutputBlockCount() == 1);

    // Oversized host callbacks are rejected to keep audio-thread work bounded.
    CHECK(!midBlockReset.pushHostInput(full.data(), nullptr,
                                       RvcAudioStreamAdapter::kMaxHostCallbackFrames + 1,
                                       1, 11));

    // Output queue capacity is bounded and rejects malformed lengths.
    RvcAudioStreamAdapter outputOverflow;
    CHECK(!outputOverflow.publishConvertedBlock(converted.data(), block - 1, 1, 1));
    CHECK(outputOverflow.publishConvertedBlock(converted.data(), block, 1, 1));
    CHECK(outputOverflow.publishConvertedBlock(converted.data(), block, 2, 1));
    CHECK(!outputOverflow.publishConvertedBlock(converted.data(), block, 3, 1));
    CHECK(outputOverflow.droppedOutputBlockCount() == 1);
    // Exercise the input SPSC queue with a real producer/consumer handoff.
    // The producer waits for each block to be consumed so this tests publication
    // ordering without turning the test into a queue-throughput benchmark.
    RvcAudioStreamAdapter concurrentInput;
    std::vector<float> concurrentSamples(block, 0.0f);
    std::atomic<bool> inputThreadFailed{false};
    constexpr std::uint64_t concurrentBlocks = 128;
    std::thread inputConsumer([&]
    {
        RvcAudioStreamAdapter::InputBlock received{};
        for (std::uint64_t expected = 1; expected <= concurrentBlocks; ++expected)
        {
            while (!concurrentInput.tryPopInputBlock(received))
                std::this_thread::yield();
            const float expectedSample = static_cast<float>(expected) / 256.0f;
            if (received.sequence != expected || received.generation != 99)
                inputThreadFailed.store(true, std::memory_order_relaxed);
            for (const auto sample : received.samples)
                if (sample != expectedSample)
                    inputThreadFailed.store(true, std::memory_order_relaxed);
        }
    });
    for (std::uint64_t sequence = 1; sequence <= concurrentBlocks; ++sequence)
    {
        const float value = static_cast<float>(sequence) / 256.0f;
        std::fill(concurrentSamples.begin(), concurrentSamples.end(), value);
        while (concurrentInput.pendingInputBlocks() != 0)
            std::this_thread::yield();
        CHECK(concurrentInput.pushHostInput(concurrentSamples.data(), nullptr, block, 1, 99));
    }
    inputConsumer.join();
    CHECK(!inputThreadFailed.load(std::memory_order_relaxed));
    CHECK(concurrentInput.droppedInputBlockCount() == 0);

    // Exercise the reverse SPSC direction: Worker producer to audio consumer.
    RvcAudioStreamAdapter concurrentOutput;
    std::vector<float> workerSamples(block, 0.0f);
    std::atomic<bool> outputThreadFailed{false};
    std::thread outputProducer([&]
    {
        for (std::uint64_t sequence = 1; sequence <= concurrentBlocks; ++sequence)
        {
            const float value = static_cast<float>(sequence) / 256.0f;
            std::fill(workerSamples.begin(), workerSamples.end(), value);
            while (concurrentOutput.pendingOutputBlocks() >= RvcAudioStreamAdapter::kQueueCapacity)
                std::this_thread::yield();
            if (!concurrentOutput.publishConvertedBlock(workerSamples.data(), block, sequence, 100))
                outputThreadFailed.store(true, std::memory_order_relaxed);
        }
    });
    std::vector<float> receivedOutput(block);
    for (std::uint64_t expected = 1; expected <= concurrentBlocks; ++expected)
    {
        RvcAudioStreamAdapter::OutputRead read{};
        while (read.frames == 0)
        {
            read = concurrentOutput.pullConvertedOutput(receivedOutput.data(), block, 100);
            if (read.frames == 0)
                std::this_thread::yield();
        }
        const float expectedSample = static_cast<float>(expected) / 256.0f;
        CHECK(read.frames == block && read.sequence == expected);
        for (const auto sample : receivedOutput)
            CHECK(sample == expectedSample);
    }
    outputProducer.join();
    CHECK(!outputThreadFailed.load(std::memory_order_relaxed));
    CHECK(concurrentOutput.pendingOutputBlocks() == 0);

    return 0;
}
