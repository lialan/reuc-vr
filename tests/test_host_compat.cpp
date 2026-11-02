// The portable replacements for the MSVC byte-swap, rotate and barrier intrinsics used by the PowerPC runtime.
#include "host_compat.h"
#include <cstdio>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

int main() {
    CHECK(uc_bswap16(0x1234u) == 0x3412u);
    CHECK(uc_bswap32(0x12345678u) == 0x78563412u);
    CHECK(uc_bswap64(0x0123456789ABCDEFull) == 0xEFCDAB8967452301ull);
    CHECK(uc_bswap32(uc_bswap32(0xDEADBEEFu)) == 0xDEADBEEFu);
    CHECK(uc_rotl32(0x80000001u, 1) == 0x00000003u);
    CHECK(uc_rotl32(0x12345678u, 0) == 0x12345678u);
    CHECK(uc_rotl32(0x12345678u, 8) == 0x34567812u);
    CHECK(uc_rotl32(0x12345678u, 31) == 0x091A2B3Cu);
    static_assert(uc_bswap32(0x11223344u) == 0x44332211u, "usable in constant expressions");
    uc_compiler_barrier();
    if (failures) return 1;
    std::printf("test_host_compat: ok\n");
    return 0;
}
