#pragma once
// The program counter and stack pointer of a POSIX signal context (the ucontext_t a SA_SIGINFO handler receives).
#ifndef _WIN32
#include <cstdint>
#if defined(__APPLE__)
#include <sys/ucontext.h>   // <ucontext.h> wants _XOPEN_SOURCE on macOS
#else
#include <ucontext.h>
#endif

namespace uc {
inline uintptr_t host_context_pc(const void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    return uintptr_t(uc->uc_mcontext->__ss.__pc);
#elif defined(__APPLE__) && defined(__x86_64__)
    return uintptr_t(uc->uc_mcontext->__ss.__rip);
#elif defined(__linux__) && defined(__aarch64__)
    return uintptr_t(uc->uc_mcontext.pc);
#elif defined(__linux__) && defined(__x86_64__)
    return uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
#else
    (void)uc; return 0;
#endif
}
inline uintptr_t host_context_sp(const void* context) {
    const auto* uc = static_cast<const ucontext_t*>(context);
#if defined(__APPLE__) && defined(__aarch64__)
    return uintptr_t(uc->uc_mcontext->__ss.__sp);
#elif defined(__APPLE__) && defined(__x86_64__)
    return uintptr_t(uc->uc_mcontext->__ss.__rsp);
#elif defined(__linux__) && defined(__aarch64__)
    return uintptr_t(uc->uc_mcontext.sp);
#elif defined(__linux__) && defined(__x86_64__)
    return uintptr_t(uc->uc_mcontext.gregs[REG_RSP]);
#else
    (void)uc; return 0;
#endif
}
}  // namespace uc
#endif
