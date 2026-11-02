// UC_WRITE_TRAP off Windows: the trap relies on VirtualProtect and the x86 single-step flag (write_trap.cpp); here it only says so once.
#include "write_trap.h"
#include <cstdio>
#include <cstdlib>

namespace uc {
void write_trap_step(const ppc::Ctx&) {
    static const bool requested = [] {
        const bool on = std::getenv("UC_WRITE_TRAP") || std::getenv("UC_READ_TRAP");
        if (on) std::fprintf(stderr, "UC_WRITE_TRAP / UC_READ_TRAP: not supported on this platform (Windows only)\n");
        return on;
    }();
    (void)requested;
}
}  // namespace uc
