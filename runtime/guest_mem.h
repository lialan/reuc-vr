#pragma once
#include <cstdint>
namespace uc {
void guest_mem_init();          // idempotent; reserves 4 GiB and maps MEM1 (24 MB), MEM2 (64 MB) and the locked cache at all their aliases
uint8_t* guest_mem_base();
}
