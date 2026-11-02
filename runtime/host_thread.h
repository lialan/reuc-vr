#pragma once
// Host threads for the guest scheduler and the per-thread host settings the game uses (Win32 on Windows, pthreads elsewhere).
#include <cstddef>
#include <cstdint>

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
}  // namespace uc
