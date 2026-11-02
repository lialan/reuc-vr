// Host threads of the guest scheduler: the requested stack is really there (guest call depth becomes host stack depth), join waits,
// and the thread CPU clock advances while a thread computes.
#include "host_thread.h"
#include <atomic>
#include <cstdio>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

// 112 frames of 64 KB: 7 MiB of stack, more than a default macOS thread (512 KB) or the main thread (8 MB minus what is used) gives.
[[gnu::noinline]] static int deep(int frames) {
    volatile char page[64 * 1024];
    page[0] = char(frames);
    page[sizeof(page) - 1] = char(frames);
    return frames == 0 ? page[0] : deep(frames - 1) + page[sizeof(page) - 1];
}

struct Work { std::atomic<int> result{-1}; uint64_t cpu_before = 0, cpu_after = 0; };

static void deep_body(void* p) {
    auto* w = static_cast<Work*>(p);
    w->result = deep(112);
}

static void spin_body(void* p) {
    auto* w = static_cast<Work*>(p);
    w->cpu_before = uc::host_thread_cpu_time();
    volatile uint64_t x = 0;
    for (uint64_t i = 0; i < 50'000'000; ++i) x = x + i;
    w->cpu_after = uc::host_thread_cpu_time();
}

int main() {
    Work deep_work;
    uc::HostThread t = uc::host_thread_start(deep_body, &deep_work, 8u << 20);
    CHECK(t.handle != nullptr);
    CHECK(uc::host_thread_join(t));
    CHECK(deep_work.result.load() == 112 * 113 / 2);   // 1 + 2 + ... + 112: all frames ran without overflowing the stack

    Work spin;
    uc::HostThread s = uc::host_thread_start(spin_body, &spin, 1u << 20);
    uc::host_thread_join(s);
    CHECK(spin.cpu_after > spin.cpu_before);

    // Pinning a started thread to CPU 0 is accepted (ignored where the OS has no hard affinity); the thread still runs and ends.
    Work pinned;
    uc::HostThread p = uc::host_thread_start(deep_body, &pinned, 8u << 20);
    uc::host_thread_pin(p, 0);
    CHECK(uc::host_thread_join(p));
    CHECK(pinned.result.load() == 112 * 113 / 2);
    uc::host_thread_set_background();
    if (failures) return 1;
    std::printf("test_host_thread: ok\n");
    return 0;
}
