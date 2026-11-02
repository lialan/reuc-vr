// Host threads on Win32: the calls sched.cpp and frame_log.cpp made directly before the POSIX port.
#include "host_thread.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace uc {
namespace {
struct WinStart { void (*fn)(void*); void* arg; };
DWORD WINAPI trampoline(LPVOID p) {
    const WinStart start = *static_cast<WinStart*>(p);
    delete static_cast<WinStart*>(p);
    start.fn(start.arg);
    return 0;
}
}  // namespace

HostThread host_thread_start(void (*fn)(void*), void* arg, size_t stack_bytes) {
    auto* start = new WinStart{fn, arg};
    HANDLE h = CreateThread(nullptr, stack_bytes, trampoline, start, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!h) { delete start; return {}; }
    return {h};
}

bool host_thread_join(HostThread thread, unsigned timeout_ms) {
    if (WaitForSingleObject(static_cast<HANDLE>(thread.handle), timeout_ms) != WAIT_OBJECT_0) return false;
    return CloseHandle(static_cast<HANDLE>(thread.handle)) != 0;
}

uint64_t host_thread_cpu_time() { ULONG64 cycles = 0; QueryThreadCycleTime(GetCurrentThread(), &cycles); return cycles; }

void host_thread_pin(HostThread thread, int cpu) {
    if (cpu >= 0 && cpu < 64) SetThreadAffinityMask(static_cast<HANDLE>(thread.handle), DWORD_PTR(1) << cpu);
}

void host_thread_set_background() { SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); }
}  // namespace uc
