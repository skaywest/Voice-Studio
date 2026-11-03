#include "AudioIpc/AudioBlockRealtimeAccess.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

#define CHECK(expression) do { if (!(expression)) { std::fprintf(stderr, "CHECK failed: %s at %d\n", #expression, __LINE__); std::abort(); } } while (false)

using namespace voice_studio::ipc;

int main()
{
    AudioBlockRegion region {};
    AudioBlockRealtimeAccess bridge;

    CHECK(!bridge.isAttached());
    CHECK(!bridge.tryAcquire());
    CHECK(!bridge.attach(nullptr));
    CHECK(bridge.attach(&region));
    CHECK(bridge.isAttached());

    {
        auto access = bridge.tryAcquire();
        CHECK(static_cast<bool>(access));
        CHECK(access.get() == &region);
        CHECK(&*access == &region);
        CHECK(!bridge.attach(&region));
    }

    std::atomic<bool> detachStarted { false };
    std::atomic<bool> detachFinished { false };
    AudioBlockRegion* detached = nullptr;

    auto held = bridge.tryAcquire();
    CHECK(static_cast<bool>(held));

    std::thread detacher([&]
    {
        detachStarted.store(true, std::memory_order_release);
        detached = bridge.detach();
        detachFinished.store(true, std::memory_order_release);
    });

    while (!detachStarted.load(std::memory_order_acquire))
        std::this_thread::yield();

    // Wait until detach has unpublished the pointer. A new audio callback must
    // now fail acquisition, while the existing lease keeps the mapping pinned.
    while (bridge.tryAcquire())
        std::this_thread::yield();

    CHECK(!detachFinished.load(std::memory_order_acquire));
    held.release();
    detacher.join();

    CHECK(detachFinished.load(std::memory_order_acquire));
    CHECK(detached == &region);
    CHECK(!bridge.isAttached());
    CHECK(!bridge.tryAcquire());
    CHECK(bridge.attach(&region));
    CHECK(bridge.detach() == &region);
    return 0;
}
