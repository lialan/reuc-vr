// UC_CPU_SAMPLE off Windows: the sampler suspends the sampled thread and reads its x64 context (cpu_sampler.cpp). On macOS use Instruments
// or `sample <pid>` instead; here the switch only says so once.
#include "cpu_sampler.h"
#include <cstdio>
#include <cstdlib>

namespace uc {
void cpu_sampler_start_once(const char*) {
    static const bool requested = [] {
        const bool on = std::getenv("UC_CPU_SAMPLE") != nullptr;
        if (on) std::fprintf(stderr, "UC_CPU_SAMPLE: not supported on this platform (use Instruments or `sample <pid>`)\n");
        return on;
    }();
    (void)requested;
}
void cpu_sampler_set_replaying(bool) {}
}  // namespace uc
