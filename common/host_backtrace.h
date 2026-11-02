#pragma once
// The calling thread's return addresses, innermost first, on POSIX hosts: backtrace() where libc has it, the unwinder on Android
// (bionic has no backtrace()).
#if defined(__ANDROID__)
#include <cstdint>
#include <unwind.h>
#else
#include <execinfo.h>
#endif

namespace uc {
inline int host_backtrace(void** frames, int capacity) {
#if defined(__ANDROID__)
    struct Walk { void** frames; int count, capacity; } walk{frames, 0, capacity};
    _Unwind_Backtrace([](_Unwind_Context* context, void* arg) {
        auto& w = *static_cast<Walk*>(arg);
        if (w.count >= w.capacity) return _URC_END_OF_STACK;
        if (const uintptr_t ip = _Unwind_GetIP(context)) w.frames[w.count++] = reinterpret_cast<void*>(ip);
        return _URC_NO_REASON;
    }, &walk);
    return walk.count;
#else
    return backtrace(frames, capacity);
#endif
}
}  // namespace uc
