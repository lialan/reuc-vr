// The machine description at the top of a session log, off Windows (system_report.cpp reads the registry and DXGI). The GPU is named by the
// Vulkan renderer's own [vulkan] lines.
#include "system_report.h"
#include <cstdio>
#include <filesystem>
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#include <cstdlib>
#include <chrono>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace uc {
void log_system_report(const std::filesystem::path& exe_dir) {
    utsname system{}; uname(&system);
    std::fprintf(stderr, "[system] %s %s %s %s\n", system.sysname, system.release, system.version, system.machine);
#if defined(__APPLE__)
    char product[64]{}; size_t size = sizeof(product);
    if (sysctlbyname("kern.osproductversion", product, &size, nullptr, 0) == 0) std::fprintf(stderr, "[system] macOS %s\n", product);
    char brand[128]{}; size = sizeof(brand);
    sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0);
    int performance = 0, efficiency = 0; size = sizeof(int);
    sysctlbyname("hw.perflevel0.logicalcpu", &performance, &size, nullptr, 0); size = sizeof(int);
    sysctlbyname("hw.perflevel1.logicalcpu", &efficiency, &size, nullptr, 0);
    uint64_t memory = 0; size = sizeof(memory);
    sysctlbyname("hw.memsize", &memory, &size, nullptr, 0);
    std::fprintf(stderr, "[system] cpu=%s logical_processors=%ld performance=%d efficiency=%d ram_MB=%llu\n", brand, sysconf(_SC_NPROCESSORS_ONLN), performance,
                 efficiency, (unsigned long long)(memory >> 20));
#else
    std::fprintf(stderr, "[system] logical_processors=%ld\n", sysconf(_SC_NPROCESSORS_ONLN));
#endif
    std::fprintf(stderr, "[client] directory=%s\n", exe_dir.string().c_str());
    std::error_code error;
    const auto space = std::filesystem::space(exe_dir, error);
    if (!error) std::fprintf(stderr, "[system] install drive free_GB=%llu total_GB=%llu\n", (unsigned long long)(space.available >> 30), (unsigned long long)(space.capacity >> 30));
    for (const char* name : {"VK_DRIVER_FILES", "VK_ICD_FILENAMES", "VK_LOADER_LAYERS_ENABLE", "VK_INSTANCE_LAYERS"})
        if (const char* value = std::getenv(name)) std::fprintf(stderr, "[vulkan-loader] %s=%s\n", name, value);
}
// Once a second: the process's memory and CPU load (the same [process]/[memory] lines as the Windows report; video memory comes from the
// renderer's [vram] lines).
void log_runtime_sample() {
#if defined(__APPLE__)
    static uint64_t last_cpu_us = 0;
    static auto last_wall = std::chrono::steady_clock::now();
    mach_task_basic_info_data_t task{}; mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, task_info_t(&task), &count) != KERN_SUCCESS) return;
    task_thread_times_info_data_t threads{}; count = TASK_THREAD_TIMES_INFO_COUNT;
    task_info(mach_task_self(), TASK_THREAD_TIMES_INFO, task_info_t(&threads), &count);
    const uint64_t cpu_us = uint64_t(task.user_time.seconds + threads.user_time.seconds + task.system_time.seconds + threads.system_time.seconds) * 1000000ull +
                            uint64_t(task.user_time.microseconds + threads.user_time.microseconds + task.system_time.microseconds + threads.system_time.microseconds);
    const auto now = std::chrono::steady_clock::now();
    const double wall_us = double(std::chrono::duration_cast<std::chrono::microseconds>(now - last_wall).count());
    const double percent = last_cpu_us && wall_us > 0 ? 100.0 * double(cpu_us - last_cpu_us) / wall_us : 0.0;   // of one core
    last_cpu_us = cpu_us; last_wall = now;
    std::fprintf(stderr, "[process] resident_MB=%llu peak_MB=%llu cpu_percent=%.1f\n", (unsigned long long)(task.resident_size >> 20),
                 (unsigned long long)(task.resident_size_max >> 20), percent);
    vm_statistics64_data_t vm{}; count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, host_info64_t(&vm), &count) == KERN_SUCCESS)
        std::fprintf(stderr, "[memory] available_MB=%llu\n", (unsigned long long)((uint64_t(vm.free_count) + vm.inactive_count) * vm_page_size >> 20));
#endif
}
}  // namespace uc
