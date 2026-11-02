#include "crash_report.h"
#include "path_print.h"
#include "dispatch.h"
#include "trace.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <thread>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include "host_context.h"
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <execinfo.h>
#include <pthread.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#endif

namespace uc {
namespace {
std::filesystem::path g_directory;
std::function<std::string()> g_stats;
std::function<void(const std::filesystem::path&)> g_bundle;
std::atomic<bool> g_written{false};
std::atomic<bool> g_done{false};
// Stack of the most recent C++ throw on this thread: fatal() reports run after the catch unwound it (Windows: a vectored handler sees every throw).
thread_local std::array<void*, 48> t_throw_frames{};
thread_local unsigned short t_throw_count = 0;

#ifdef _WIN32
LONG CALLBACK record_throw(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == 0xE06D7363) // MSVC C++ exception
        t_throw_count = RtlCaptureStackBackTrace(1, ULONG(t_throw_frames.size()), t_throw_frames.data(), nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}
void write_module(std::FILE* out, void* pc) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(pc), &module)) {
        wchar_t path[MAX_PATH]{};
        if (GetModuleFileNameW(module, path, MAX_PATH)) {
            const wchar_t* name = wcsrchr(path, L'\\'); name = name ? name + 1 : path;
            std::fprintf(out, " %ls+0x%llX", name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(module)));
        }
    }
}
unsigned long process_id() { return GetCurrentProcessId(); }
unsigned long long thread_id() { return GetCurrentThreadId(); }
std::tm local_time(std::time_t now) { std::tm local{}; localtime_s(&local, &now); return local; }
std::FILE* open_report(const std::filesystem::path& path) { return _wfopen(path.c_str(), L"w"); }
void wait_a_moment() { Sleep(10); }
#else
void write_module(std::FILE* out, void* pc) {
    Dl_info module{};
    if (dladdr(pc, &module) && module.dli_fname) {
        const char* name = std::strrchr(module.dli_fname, '/'); name = name ? name + 1 : module.dli_fname;
        std::fprintf(out, " %s+0x%llX", name, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(pc) - reinterpret_cast<uintptr_t>(module.dli_fbase)));
    }
}
unsigned long process_id() { return static_cast<unsigned long>(getpid()); }
unsigned long long thread_id() {
#if defined(__APPLE__)
    uint64_t id = 0; pthread_threadid_np(nullptr, &id); return id;
#elif defined(__linux__)
    return static_cast<unsigned long long>(syscall(SYS_gettid));
#else
    return 0;
#endif
}
std::tm local_time(std::time_t now) { std::tm local{}; localtime_r(&now, &local); return local; }
std::FILE* open_report(const std::filesystem::path& path) { return std::fopen(path.c_str(), "w"); }
void wait_a_moment() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
#endif

void write_frame(std::FILE* out, void* pc) {
    std::fprintf(out, "  %p", pc);
    write_module(out, pc);
    if (const uint32_t guest = guest_function_for_host(pc)) {
        std::fprintf(out, "  guest %08X", guest);
        const auto symbol = symbol_name(guest);
        if (!symbol.empty()) std::fprintf(out, " [%s]", symbol.c_str());
    }
    std::fprintf(out, "\n");
}

// The faulting thread's own frames, from its machine state.
void write_context_frames(std::FILE* out, const void* host_context) {
#ifdef _WIN32
    const auto* context = static_cast<const CONTEXT*>(host_context);
    std::fprintf(out, "faulting rip: %p\n", reinterpret_cast<void*>(context->Rip));
    std::fprintf(out, "\nhost stack (guest = translated function containing the frame):\n");
    // Unwound from the exception context (a call through a null pointer: start at [rsp]).
    CONTEXT walk = *context;
    if (!walk.Rip && walk.Rsp) { walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp); walk.Rsp += 8; }
    for (int i = 0; i < 32 && walk.Rip; ++i) {
        write_frame(out, reinterpret_cast<void*>(walk.Rip));
        DWORD64 image = 0;
        auto* function = RtlLookupFunctionEntry(walk.Rip, &image, nullptr);
        if (!function) { walk.Rip = *reinterpret_cast<const DWORD64*>(walk.Rsp); walk.Rsp += 8; continue; }   // leaf function
        void* handler_data = nullptr; DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, walk.Rip, function, &walk, &handler_data, &frame, nullptr);
    }
#else
    // A signal handler's backtrace() continues through the signal trampoline into the faulting frames, so the handler stack below
    // holds them; here only the faulting instruction itself.
    const uintptr_t pc = host_context_pc(host_context);
    std::fprintf(out, "faulting pc: %p  sp: %p\n", reinterpret_cast<void*>(pc), reinterpret_cast<void*>(host_context_sp(host_context)));
    std::fprintf(out, "\nhost stack (guest = translated function containing the frame):\n");
    write_frame(out, reinterpret_cast<void*>(pc));
#endif
    std::fprintf(out, "  -- handler stack --\n");
}

// The calling thread's stack without capture_stack and its caller (crash_report_write): the frames of whoever reports the crash.
[[gnu::noinline]] unsigned capture_stack(void** frames, unsigned capacity) {
#ifdef _WIN32
    return RtlCaptureStackBackTrace(2, ULONG(capacity), frames, nullptr);
#else
    std::array<void*, 80> all{};
    const int count = backtrace(all.data(), int(all.size()));
    unsigned kept = 0;
    for (int i = 2; i < count && kept < capacity; ++i) frames[kept++] = all[size_t(i)];
    return kept;
#endif
}
}  // namespace

void crash_report_init(std::filesystem::path directory, std::function<std::string()> stats) {
    g_directory = std::move(directory);
    g_stats = std::move(stats);
    g_bundle = nullptr;
    g_written = false;
    g_done = false;
#ifdef _WIN32
    static bool installed = false;
    if (!installed) { AddVectoredExceptionHandler(1, record_throw); installed = true; }
#endif
}

void crash_report_set_bundle(std::function<void(const std::filesystem::path&)> bundle) { g_bundle = std::move(bundle); }

#ifdef _WIN32
void crash_guard_thread() {
    ULONG size = 64 * 1024;
    SetThreadStackGuarantee(&size);
}
#else
namespace {
// The alternate stack of one thread; released (after switching it off) when the thread ends.
struct AltStack {
    void* memory = nullptr;
    ~AltStack() {
        if (!memory) return;
        stack_t off{}; off.ss_flags = SS_DISABLE;
        sigaltstack(&off, nullptr);
        std::free(memory);
    }
};
thread_local AltStack t_alt_stack;
}
void crash_guard_thread() {
    if (t_alt_stack.memory) return;
    const size_t size = 64 * 1024 > size_t(SIGSTKSZ) ? 64 * 1024 : size_t(SIGSTKSZ);
    t_alt_stack.memory = std::malloc(size);
    if (!t_alt_stack.memory) return;
    stack_t stack{};
    stack.ss_sp = t_alt_stack.memory;
    stack.ss_size = size;
    if (sigaltstack(&stack, nullptr) != 0) { std::free(t_alt_stack.memory); t_alt_stack.memory = nullptr; }
}
#endif

bool crash_report_write(std::string_view reason, const void* host_context) noexcept {
    if (g_directory.empty()) return false;
    if (g_written.exchange(true)) {
        // Another thread is writing: let it finish before this thread exits the process.
        for (int i = 0; i < 500 && !g_done; ++i) wait_a_moment();
        return false;
    }
    struct Done { ~Done() { g_done = true; } } done;
    try {
    std::error_code error;
    std::filesystem::create_directories(g_directory, error);
    const std::tm local = local_time(std::time(nullptr));
    char name[96];
    std::snprintf(name, sizeof name, "crash-%04d%02d%02d-%02d%02d%02d-%lu.txt", local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                  local.tm_hour, local.tm_min, local.tm_sec, process_id());
    std::FILE* out = open_report(g_directory / name);
    if (!out) return false;
    std::fprintf(out, "Umbrella Chronicles native port crash report\nreason: %.*s\nthread: %llu\n", int(reason.size()), reason.data(), thread_id());
    if (host_context) write_context_frames(out, host_context);
    else std::fprintf(out, "\nhost stack (guest = translated function containing the frame):\n");
    std::array<void*, 64> frames{};
    const unsigned count = capture_stack(frames.data(), unsigned(frames.size()));
    for (unsigned i = 0; i < count; ++i) write_frame(out, frames[i]);
    if (t_throw_count) {
        std::fprintf(out, "\nlast C++ throw on this thread:\n");
        for (unsigned i = 0; i < t_throw_count; ++i) write_frame(out, t_throw_frames[i]);
    }
    std::fprintf(out, "\nlast guest functions on this thread (UC_TRACE builds only):\n");
    trace_dump(out, 32);
    if (g_stats) {
        try { std::fprintf(out, "\nruntime state:\n%s\n", g_stats().c_str()); }
        catch (...) { std::fprintf(out, "\nruntime state unavailable\n"); }
    }
    auto bundle = g_directory / name; bundle.replace_extension(".dump");
    if (g_bundle) std::fprintf(out, "\nbundle: " UC_PATH_FMT "\n", UC_PATH_ARG(bundle));
    std::fclose(out);
    std::fprintf(stderr, "crash report: " UC_PATH_FMT "\n", UC_PATH_ARG(g_directory / name));
    if (g_bundle) {
        try { g_bundle(bundle); std::fprintf(stderr, "crash bundle: " UC_PATH_FMT "\n", UC_PATH_ARG(bundle)); }
        catch (...) { std::fprintf(stderr, "crash bundle failed\n"); }   // the report is already on disk
    }
    return true;
    } catch (...) { return false; }
}
}
