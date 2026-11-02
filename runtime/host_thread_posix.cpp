// Host threads on pthreads (macOS, Linux, Android).
#include "host_thread.h"
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#elif defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

namespace uc {
namespace {
// pthread_join has no timeout on macOS: the thread reports its end through `done`, the join waits for that, then reaps the thread.
struct PosixThread {
    pthread_t id{};
    void (*fn)(void*);
    void* arg;
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
};
void* trampoline(void* p) {
    auto* t = static_cast<PosixThread*>(p);
    t->fn(t->arg);
    { std::lock_guard lock(t->m); t->done = true; }
    t->cv.notify_all();
    return nullptr;
}
}  // namespace

HostThread host_thread_start(void (*fn)(void*), void* arg, size_t stack_bytes) {
    auto* t = new PosixThread;
    t->fn = fn;
    t->arg = arg;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    const size_t page = size_t(sysconf(_SC_PAGESIZE));
    size_t stack = (stack_bytes + page - 1) / page * page;
    if (stack < size_t(PTHREAD_STACK_MIN)) stack = size_t(PTHREAD_STACK_MIN);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    // The guest CPU and the game's own threads are latency-bound: user-interactive QoS keeps them on the performance cores of Apple Silicon.
    pthread_attr_set_qos_class_np(&attr, QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    const int error = pthread_create(&t->id, &attr, trampoline, t);
    pthread_attr_destroy(&attr);
    if (error != 0) { delete t; return {}; }
    return {t};
}

bool host_thread_join(HostThread thread, unsigned timeout_ms) {
    auto* t = static_cast<PosixThread*>(thread.handle);
    if (!t) return false;
    {
        std::unique_lock lock(t->m);
        if (!t->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return t->done; })) return false;
    }
    pthread_join(t->id, nullptr);   // the thread has only its return left to run
    delete t;
    return true;
}

uint64_t host_thread_cpu_time() {
#if defined(__APPLE__)
    return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID);
#else
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1'000'000'000ull + uint64_t(ts.tv_nsec);
#endif
}

void host_thread_pin(HostThread thread, int cpu) {
#if defined(__linux__)
    auto* t = static_cast<PosixThread*>(thread.handle);
    if (!t || cpu < 0 || cpu >= CPU_SETSIZE) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
#if defined(__ANDROID__)
    sched_setaffinity(pthread_gettid_np(t->id), sizeof(set), &set);   // bionic has no pthread_setaffinity_np
#else
    pthread_setaffinity_np(t->id, sizeof(set), &set);
#endif
#else
    (void)thread; (void)cpu;   // macOS: no hard affinity (the QoS class of host_thread_start picks the core type)
#endif
}

void host_thread_set_background() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#elif defined(__linux__)
    setpriority(PRIO_PROCESS, pid_t(syscall(SYS_gettid)), 5);   // Linux: the nice value of one thread
#endif
}
}  // namespace uc
