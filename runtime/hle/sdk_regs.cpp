// The SDK's register save/restore helpers (the compiler's _savegpr_N, _restgpr_N, _savefpr_N, _restfpr_N, N = 14..31): every function prologue and epilogue of the
// game calls one, with r11 pointing just above the save area. In the translated code each is a chain of one-instruction functions that fall into each other (up to
// 18 nested calls); here an entry is one native function running the same load/store instructions in the same order, so the guest memory, the registers and the
// write watch behave exactly as before. project/hle_map.txt binds the 72 entry addresses; tests/test_sdk_regs_hle.cpp checks them against the instruction sequences.
#include "ppc_rt.h"
#include <cstring>
#include <utility>

namespace {
using ppc::Ctx;
// The save area (always guest RAM: it lies on the stack) is checked once: both ends outside MMIO and no wrap, then the stores go straight to
// memory and the write watch / rollback tracker mark its one or two pages once, which are the pages the per-store barriers mark. Any other
// area takes the instruction-by-instruction path.
inline bool plain_area(uint32_t top, uint32_t size) { return top >= size && !ppc::is_mmio(top - size) && !ppc::is_mmio(top - 1); }
inline void put32(uint8_t* p, uint32_t v) { v = uc_bswap32(v); std::memcpy(p, &v, 4); }
inline uint32_t get32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return uc_bswap32(v); }
inline void put64(uint8_t* p, double d) { uint64_t v; std::memcpy(&v, &d, 8); v = uc_bswap64(v); std::memcpy(p, &v, 8); }
inline double get64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); v = uc_bswap64(v); double d; std::memcpy(&d, &v, 8); return d; }
template<int First, int... I> void save_gpr(Ctx& c, std::integer_sequence<int,I...>) {
    constexpr uint32_t size = 4 * (32 - First);
    if (const uint32_t top = c.r[11]; plain_area(top, size)) {
        gx::note_write_range(top - size, size);
        uint8_t* p = ppc::hp(c, top - size);
        (put32(p + 4 * I, c.r[First + I]), ...);
        return;
    }
    (ppc::op_stw(c, First + I, 11, -4 * (32 - First - I)), ...);
}
template<int First, int... I> void rest_gpr(Ctx& c, std::integer_sequence<int,I...>) {
    constexpr uint32_t size = 4 * (32 - First);
    if (const uint32_t top = c.r[11]; plain_area(top, size)) {
        const uint8_t* p = ppc::hp(c, top - size);
        ((c.r[First + I] = get32(p + 4 * I)), ...);
        return;
    }
    (ppc::op_lwz(c, First + I, 11, -4 * (32 - First - I)), ...);
}
template<int First, int... I> void save_fpr(Ctx& c, std::integer_sequence<int,I...>) {
    constexpr uint32_t size = 8 * (32 - First);
    if (const uint32_t top = c.r[11]; plain_area(top, size)) {
        gx::note_write_range(top - size, size);
        uint8_t* p = ppc::hp(c, top - size);
        (put64(p + 8 * I, c.f[First + I]), ...);
        return;
    }
    (ppc::op_stfd(c, First + I, 11, -8 * (32 - First - I)), ...);
}
template<int First, int... I> void rest_fpr(Ctx& c, std::integer_sequence<int,I...>) {
    constexpr uint32_t size = 8 * (32 - First);
    if (const uint32_t top = c.r[11]; plain_area(top, size)) {
        const uint8_t* p = ppc::hp(c, top - size);
        ((c.f[First + I] = get64(p + 8 * I)), ...);
        return;
    }
    (ppc::op_lfd(c, First + I, 11, -8 * (32 - First - I)), ...);
}
}  // namespace

#define UC_REG_HLE(N) \
    void hle_SaveGpr##N(Ctx& c) { save_gpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); } \
    void hle_RestGpr##N(Ctx& c) { rest_gpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); } \
    void hle_SaveFpr##N(Ctx& c) { save_fpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); } \
    void hle_RestFpr##N(Ctx& c) { rest_fpr<N>(c, std::make_integer_sequence<int, 32 - N>{}); }
UC_REG_HLE(14) UC_REG_HLE(15) UC_REG_HLE(16) UC_REG_HLE(17) UC_REG_HLE(18) UC_REG_HLE(19) UC_REG_HLE(20) UC_REG_HLE(21) UC_REG_HLE(22)
UC_REG_HLE(23) UC_REG_HLE(24) UC_REG_HLE(25) UC_REG_HLE(26) UC_REG_HLE(27) UC_REG_HLE(28) UC_REG_HLE(29) UC_REG_HLE(30) UC_REG_HLE(31)
