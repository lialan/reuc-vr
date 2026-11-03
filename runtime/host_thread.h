#pragma once
// Host threads for the guest scheduler and the per-thread host settings the game uses (Win32 on Windows, pthreads elsewhere).
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>
#if defined(__linux__)
#include <unistd.h>
#endif

namespace uc {
struct HostThread { void* handle = nullptr; };

// Starts fn(arg) on a new thread with stack_bytes of stack (reserved on Windows). handle is null when the thread cannot be created.
HostThread host_thread_start(void (*fn)(void*), void* arg, size_t stack_bytes);
// Waits for the thread to end, at most timeout_ms, and releases it; false when it did not end in time (the thread is left running then).
bool host_thread_join(HostThread thread, unsigned timeout_ms = 5000);
// CPU time the calling thread has used, without waits or preemption: CPU cycles on Windows (QueryThreadCycleTime), nanoseconds elsewhere.
// Only differences of one thread's values are meaningful.
uint64_t host_thread_cpu_time();
// Pins a started thread to one logical CPU (Windows, Linux/Android); macOS has no hard affinity and ignores it.
void host_thread_pin(HostThread thread, int cpu);
// The calling thread does background work (the frame log writer): below-normal priority / QoS class utility.
void host_thread_set_background();

// The host threads that run guest code (the scheduler marks each when it starts). The headset (Android) tells the XR runtime about them as
// the application's main threads, so they get the performance-critical scheduling the render thread gets. Thread ids exist on Linux only.
struct GuestThreadIds { std::mutex m; std::vector<int> tids; };
inline GuestThreadIds& guest_thread_ids() { static GuestThreadIds ids; return ids; }
inline void host_thread_mark_guest() {
#if defined(__linux__)
    auto& ids = guest_thread_ids(); std::lock_guard lock(ids.m); ids.tids.push_back(int(gettid()));
#endif
}
// The guest thread ids marked from index `seen` on, appended to out; returns the new count.
inline size_t host_thread_guest_tids(size_t seen, std::vector<int>& out) {
    auto& ids = guest_thread_ids(); std::lock_guard lock(ids.m);
    for (size_t i = seen; i < ids.tids.size(); ++i) out.push_back(ids.tids[i]);
    return ids.tids.size();
}
}  // namespace uc
