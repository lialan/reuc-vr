// Guest memory: every console address of a RAM block reaches the same bytes (MEM1, MEM2 and the locked cache are each mapped at
// their physical, cached and uncached addresses), and the hardware register page stays unmapped.
#include "guest_mem.h"
#include <cstdint>
#include <cstdio>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static void check_alias(uint8_t* base, uint32_t write_at, uint32_t a, uint32_t b, uint8_t value) {
    base[write_at] = value;
    CHECK(base[a] == value);
    CHECK(base[b] == value);
    base[a] = uint8_t(value ^ 0xFF);   // and back through another alias
    CHECK(base[write_at] == uint8_t(value ^ 0xFF));
}

int main() {
    uc::guest_mem_init();
    uint8_t* base = uc::guest_mem_base();
    CHECK(base != nullptr);
    if (!base) return 1;
    // MEM1 (24 MB): 0x00000000, 0x80000000, 0xC0000000
    check_alias(base, 0x80001234u, 0x00001234u, 0xC0001234u, 0xA5);
    check_alias(base, 0x817FFFFFu, 0x017FFFFFu, 0xC17FFFFFu, 0x3C);   // last byte of MEM1
    // MEM2 (64 MB): 0x10000000, 0x90000000, 0xD0000000
    check_alias(base, 0x90002000u, 0x10002000u, 0xD0002000u, 0x5A);
    check_alias(base, 0x93FFFFFFu, 0x13FFFFFFu, 0xD3FFFFFFu, 0x77);   // last byte of MEM2
    // The locked cache: 0x20000000 (physical alias) and 0xE0000000
    base[0xE0000010u] = 0x42;
    CHECK(base[0x20000010u] == 0x42);
    // A 32-bit store through one alias is one 32-bit value through the others (no per-alias copies).
    *reinterpret_cast<volatile uint32_t*>(base + 0x80100000u) = 0x11223344u;
    CHECK(*reinterpret_cast<volatile uint32_t*>(base + 0xC0100000u) == 0x11223344u);
    // Idempotent: a second init keeps the same mapping and contents.
    uc::guest_mem_init();
    CHECK(uc::guest_mem_base() == base);
    CHECK(base[0x00001234u] == uint8_t(0xA5 ^ 0xFF));
    if (failures) return 1;
    std::printf("test_guest_mem: ok\n");
    return 0;
}
