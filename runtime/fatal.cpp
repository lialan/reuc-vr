#include "fatal.h"
#include "crash_report.h"
#include "guest_mem.h"
#include "trace.h"
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include "host_context.h"
#include <csignal>
#include <cstring>
#include <dlfcn.h>
#include <unistd.h>
#endif

namespace uc {
static void (*g_hook)();
void set_exit_hook(void (*fn)()) { g_hook = fn; }

[[noreturn]] static void die(int code) {
    if (g_hook) g_hook();
    std::fflush(stderr);
#ifdef _WIN32
    ExitProcess(UINT(code));   // kills the other host threads too
#else
    std::fflush(nullptr);
    _exit(code);               // kills the other host threads too, without running destructors under them
#endif
}

// A fault in the guest's 4 GiB range: the guest address (cached alias) for the first 1 GiB, the RAM blocks.
static void print_guest_address(uintptr_t host) {
    if (!guest_mem_base()) return;
    const uint64_t off = uint64_t(host) - reinterpret_cast<uint64_t>(guest_mem_base());
    if (off < 0x40000000u) std::fprintf(stderr, " = guest %08X", 0x80000000u | uint32_t(off));
}

#ifdef _WIN32
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* r = ep->ExceptionRecord;
    char reason[256];
    std::snprintf(reason, sizeof reason, "host exception %08lX at %p%s %p", r->ExceptionCode, r->ExceptionAddress,
                  r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1 ? (r->ExceptionInformation[0] ? " writing" : " reading") : "",
                  r->NumberParameters > 1 ? reinterpret_cast<void*>(r->ExceptionInformation[1]) : nullptr);
    crash_report_write(reason, ep->ContextRecord);
    std::fprintf(stderr, "host exception %08lX at %p", r->ExceptionCode, r->ExceptionAddress);
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(r->ExceptionAddress), &module)) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(module,path,MAX_PATH))
            std::fprintf(stderr," [%ls+0x%llX]",path,static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(r->ExceptionAddress)-reinterpret_cast<uintptr_t>(module)));
    }
    if ((r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) && r->NumberParameters > 1)
        std::fprintf(stderr, " (%s address %p)", r->ExceptionInformation[0] ? "write to" : "read of", reinterpret_cast<void*>(r->ExceptionInformation[1]));
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters > 1) print_guest_address(uintptr_t(r->ExceptionInformation[1]));
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
// SIGSEGV/SIGBUS (a stray access: macOS reports some as SIGBUS), SIGILL, SIGFPE, SIGTRAP (__builtin_trap). The handler runs on the alternate
// stack crash_guard_thread installs, so a stack overflow is reported too. SA_RESETHAND: a fault inside the handler ends the process.
static void crash_signal(int signal, siginfo_t* info, void* context) {
    const uintptr_t pc = host_context_pc(context);
    const bool access = signal == SIGSEGV || signal == SIGBUS;
    char reason[256];
    std::snprintf(reason, sizeof reason, "host signal %d (%s) at %p%s %p", signal, strsignal(signal), reinterpret_cast<void*>(pc),
                  access ? " accessing" : "", access ? info->si_addr : nullptr);
    crash_report_write(reason, context);
    std::fprintf(stderr, "host signal %d (%s) at %p", signal, strsignal(signal), reinterpret_cast<void*>(pc));
    Dl_info module{};
    if (pc && dladdr(reinterpret_cast<void*>(pc), &module) && module.dli_fname)
        std::fprintf(stderr, " [%s+0x%llX]", module.dli_fname, static_cast<unsigned long long>(pc - reinterpret_cast<uintptr_t>(module.dli_fbase)));
    if (access) {
        std::fprintf(stderr, " (access of address %p)", info->si_addr);
        print_guest_address(reinterpret_cast<uintptr_t>(info->si_addr));
    }
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
}
#endif

static void terminate_handler() {
    try {
        if (auto e = std::current_exception()) std::rethrow_exception(e);
        std::fprintf(stderr, "terminate called without an exception\n");
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "terminate: uncaught C++ exception: %s\n", ex.what());
        char reason[512];  // no heap allocation: this path also runs for bad_alloc
        std::snprintf(reason, sizeof reason, "uncaught C++ exception: %s", ex.what());
        crash_report_write(reason, nullptr);
    } catch (...) {
        crash_report_write("uncaught non-standard exception", nullptr);
        std::fprintf(stderr, "terminate: uncaught non-standard exception\n");
    }
    trace_dump(stderr);
    die(3);
}
void install_crash_handlers() {
    crash_guard_thread();
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_filter);
#else
    struct sigaction action{};
    action.sa_sigaction = crash_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP}) sigaction(signal, &action, nullptr);
#endif
    std::set_terminate(terminate_handler);
}

void fatal(const char* fmt, ...) {
    std::va_list ap; va_start(ap, fmt);
    char message[1024];
    std::vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "fatal: %s\n", message);
    char reason[1100];
    std::snprintf(reason, sizeof reason, "fatal: %s", message);
    crash_report_write(reason, nullptr);
    trace_dump(stderr);
    die(3);
}
void fatal_guest(uint32_t addr, const char* what) {
    std::fprintf(stderr, "guest trap at %08X: %s", addr, what);
    std::string s = symbol_name(addr);
    if (!s.empty()) std::fprintf(stderr, "  [%s]", s.c_str());
    char reason[512];
    std::snprintf(reason, sizeof reason, "guest trap at %08X: %s%s%s", addr, what, s.empty() ? "" : " ", s.c_str());
    crash_report_write(reason, nullptr);
    std::fprintf(stderr, "\nlast guest functions on this thread:\n");
    trace_dump(stderr);
    die(3);
}
}  // namespace uc
