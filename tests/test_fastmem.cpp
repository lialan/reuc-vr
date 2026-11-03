// fastmem (runtime/fastmem.cpp): the decoder and emulator of the faulting arm64 loads/stores, checked on the instructions as the assembler
// encodes them, then the signal path end to end (the accessors of ppc_rt.h with addresses the compiler cannot see, other threads, chaining).
#include "check.h"
#include "fastmem.h"
#include "ppc_fixture.h"
#include <csetjmp>
#include <csignal>
#include <cstring>
#include <thread>
#include <vector>

extern uint32_t g_mmio_last_addr, g_mmio_last_val;
extern int g_mmio_last_size;

#if UC_FASTMEM
namespace {
// Every form the handler decodes, assembled here so the test sees the encodings the compiler emits. x0 = base, x2 = index, x1/x3 = data.
#define FM_INSNS(X) \
    X(ldr_w, "ldr w1, [x0]") X(ldr_x, "ldr x1, [x0, #8]") X(ldrh, "ldrh w1, [x0, #2]") X(ldrsh_w, "ldrsh w1, [x0]") X(ldrsh_x, "ldrsh x1, [x0]") \
    X(ldrb, "ldrb w1, [x0, #1]") X(ldrsb_w, "ldrsb w1, [x0]") X(ldrsb_x, "ldrsb x1, [x0]") X(ldrsw, "ldrsw x1, [x0, #4]") \
    X(ldur, "ldur w1, [x0, #-4]") X(ldr_pre, "ldr w1, [x0, #4]!") X(ldr_post, "ldr w1, [x0], #4") X(ldrh_post, "ldrh w1, [x0], #-2") \
    X(ldr_reg, "ldr w1, [x0, x2]") X(ldr_sxtw, "ldr w1, [x0, w2, sxtw]") X(ldr_lsl, "ldr w1, [x0, x2, lsl #2]") X(ldr_uxtw, "ldr w1, [x0, w2, uxtw #2]") \
    X(ldrh_reg, "ldrh w1, [x0, x2, lsl #1]") X(ldr_sp, "ldr w1, [sp, #8]") \
    X(ldr_b, "ldr b0, [x0]") X(ldr_h, "ldr h0, [x0]") X(ldr_s, "ldr s0, [x0]") X(ldr_d, "ldr d0, [x0]") X(ldr_q, "ldr q0, [x0]") X(ldur_d, "ldur d0, [x0, #-8]") \
    X(ldr_s_reg, "ldr s0, [x0, x2]") X(ldr_d_post, "ldr d0, [x0], #8") \
    X(ldp_w, "ldp w1, w3, [x0]") X(ldp_x, "ldp x1, x3, [x0, #16]") X(ldpsw, "ldpsw x1, x3, [x0]") X(ldp_s, "ldp s0, s1, [x0]") \
    X(ldp_d_post, "ldp d0, d1, [x0], #16") X(ldp_q_pre, "ldp q0, q1, [x0, #-32]!") X(ldnp_w, "ldnp w1, w3, [x0]") \
    X(ld1_8b, "ld1 {v0.8b}, [x0]") X(ld1_16b, "ld1 {v0.16b}, [x0]") X(ld1_2s_post, "ld1 {v0.2s}, [x0], #8") X(ld1_4s_reg, "ld1 {v0.4s}, [x0], x2") \
    X(str_w, "str w1, [x0]") X(str_wzr, "str wzr, [x0, #4]") X(strh, "strh w1, [x0]") X(strb, "strb w1, [x0, #3]") X(str_x, "str x1, [x0]") \
    X(stur, "stur w1, [x0, #-4]") X(str_pre, "str w1, [x0, #4]!") X(str_post, "str w1, [x0], #-4") X(str_reg, "str w1, [x0, x2]") \
    X(str_s, "str s0, [x0]") X(str_d, "str d0, [x0]") X(str_q, "str q0, [x0]") X(stp_w, "stp w1, w3, [x0]") X(stp_x, "stp x1, x3, [x0, #-16]!") \
    X(stp_xzr, "stp xzr, xzr, [x0]") X(stp_d, "stp d0, d1, [x0]") X(st1_8b, "st1 {v0.8b}, [x0]") X(st1_16b_post, "st1 {v0.16b}, [x0], #16") \
    X(ldxr, "ldxr w1, [x0]") X(stxr, "stxr w4, w1, [x0]") X(ldar, "ldar w1, [x0]") X(prfm, "prfm pldl1keep, [x0]") X(ld2, "ld2 {v0.4s, v1.4s}, [x0]") \
    X(ld1_lane, "ld1 {v0.s}[1], [x0]")
#define FM_ASM(name, text) text "\n"
#define FM_ENUM(name, text) name,
enum Insn { FM_INSNS(FM_ENUM) kInsnCount };
asm(".text\n.p2align 2\n.globl fm_insns\nfm_insns:\n" FM_INSNS(FM_ASM));
extern "C" const uint32_t fm_insns[] asm("fm_insns");

// The fake guest: byte a of the hardware window holds gb(a) for reads; writes are logged.
uint8_t gb(uint32_t a) { return uint8_t(a * 7 + 3); }
struct Op { bool write; uint32_t ea; int size; uint32_t value; };
std::vector<Op> g_log;
uint32_t log_read(uint32_t ea, int size) {
    uint32_t v = 0;
    for (int i = 0; i < size; ++i) v = v << 8 | gb(ea + uint32_t(i));
    g_log.push_back({false, ea, size, v});
    return v;
}
void log_write(uint32_t ea, int size, uint32_t v) { g_log.push_back({true, ea, size, v}); }

uint8_t* const kMem = reinterpret_cast<uint8_t*>(uintptr_t(0x300000000ull));   // never dereferenced
constexpr uint32_t kEa = 0xCC006800u;
struct Regs {
    uint64_t x[32] = {};
    uint8_t v[32][16] = {};
    uint64_t pc = 0x1000;
    uc::HostRegisters view() { return {x, v, &pc}; }
};
Regs fresh() {
    Regs r;
    for (int i = 0; i < 31; ++i) r.x[i] = 0xDEAD0000DEAD0000ull + uint64_t(i);
    for (int i = 0; i < 32; ++i) std::memset(r.v[i], 0xEE, 16);
    r.x[0] = uint64_t(uintptr_t(kMem)) + kEa;
    return r;
}
bool run(Insn insn, Regs& r) { g_log.clear(); return uc::fastmem_emulate(fm_insns[insn], r.view(), kMem, log_read, log_write); }
// The little-endian value of the guest bytes at ea (what a load leaves in the register before the translated code's byte swap).
uint64_t le(uint32_t ea, unsigned n) { uint64_t v = 0; for (unsigned i = n; i-- > 0;) v = v << 8 | gb(ea + i); return v; }
bool reads(std::initializer_list<std::pair<uint32_t, int>> want) {
    if (g_log.size() != want.size()) return false;
    size_t i = 0;
    for (auto [ea, size] : want) { if (g_log[i].write || g_log[i].ea != ea || g_log[i].size != size) return false; ++i; }
    return true;
}
bool writes(std::initializer_list<Op> want) {
    if (g_log.size() != want.size()) return false;
    size_t i = 0;
    for (const Op& w : want) { if (!g_log[i].write || g_log[i].ea != w.ea || g_log[i].size != w.size || g_log[i].value != w.value) return false; ++i; }
    return true;
}
bool vec_is(const Regs& r, int reg, uint32_t ea, unsigned n) {
    for (unsigned i = 0; i < 16; ++i) if (r.v[reg][i] != (i < n ? gb(ea + i) : 0)) return false;
    return true;
}

void decode_and_emulate() {
    Regs r = fresh();
    // integer loads: zero/sign extension, W results clear the upper half
    CHECK(run(ldr_w, r) && reads({{kEa, 4}}) && r.x[1] == le(kEa, 4) && r.pc == 0x1004);
    r = fresh(); CHECK(run(ldr_x, r) && reads({{kEa + 8, 4}, {kEa + 12, 4}}) && r.x[1] == le(kEa + 8, 8));
    r = fresh(); CHECK(run(ldrh, r) && reads({{kEa + 2, 2}}) && r.x[1] == le(kEa + 2, 2));
    r = fresh(); CHECK(run(ldrsh_w, r) && r.x[1] == uint32_t(int32_t(int16_t(le(kEa, 2)))));
    r = fresh(); CHECK(run(ldrsh_x, r) && r.x[1] == uint64_t(int64_t(int16_t(le(kEa, 2)))));
    r = fresh(); CHECK(run(ldrb, r) && reads({{kEa + 1, 1}}) && r.x[1] == gb(kEa + 1));
    r = fresh(); CHECK(run(ldrsb_w, r) && r.x[1] == uint32_t(int32_t(int8_t(gb(kEa)))));
    r = fresh(); CHECK(run(ldrsb_x, r) && r.x[1] == uint64_t(int64_t(int8_t(gb(kEa)))));
    r = fresh(); CHECK(run(ldrsw, r) && reads({{kEa + 4, 4}}) && r.x[1] == uint64_t(int64_t(int32_t(le(kEa + 4, 4)))));
    // addressing: unscaled, pre/post-index writeback, register offsets, SP base
    r = fresh(); r.x[0] += 4; CHECK(run(ldur, r) && reads({{kEa, 4}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 4);
    r = fresh(); CHECK(run(ldr_pre, r) && reads({{kEa + 4, 4}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 4 && r.x[1] == le(kEa + 4, 4));
    r = fresh(); CHECK(run(ldr_post, r) && reads({{kEa, 4}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 4 && r.x[1] == le(kEa, 4));
    r = fresh(); CHECK(run(ldrh_post, r) && reads({{kEa, 2}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa - 2);
    r = fresh(); r.x[2] = 0x10; CHECK(run(ldr_reg, r) && reads({{kEa + 0x10, 4}}));
    r = fresh(); r.x[0] += 8; r.x[2] = 0xFFFFFFFFFFFFFFF8ull & 0xFFFFFFFFu; CHECK(run(ldr_sxtw, r) && reads({{kEa, 4}}));   // w2 = -8
    r = fresh(); r.x[2] = 3; CHECK(run(ldr_lsl, r) && reads({{kEa + 12, 4}}));
    r = fresh(); r.x[2] = 0xFFFFFFFF00000005ull; CHECK(run(ldr_uxtw, r) && reads({{kEa + 20, 4}}));   // the upper half of x2 is ignored
    r = fresh(); r.x[2] = 3; CHECK(run(ldrh_reg, r) && reads({{kEa + 6, 2}}));
    r = fresh(); r.x[31] = uint64_t(uintptr_t(kMem)) + kEa; CHECK(run(ldr_sp, r) && reads({{kEa + 8, 4}}) && r.x[1] == le(kEa + 8, 4));
    // SIMD&FP loads: the rest of the register is zeroed; 8 and 16 bytes as 4-byte reads in ascending order
    r = fresh(); CHECK(run(ldr_b, r) && reads({{kEa, 1}}) && vec_is(r, 0, kEa, 1));
    r = fresh(); CHECK(run(ldr_h, r) && reads({{kEa, 2}}) && vec_is(r, 0, kEa, 2));
    r = fresh(); CHECK(run(ldr_s, r) && reads({{kEa, 4}}) && vec_is(r, 0, kEa, 4));
    r = fresh(); CHECK(run(ldr_d, r) && reads({{kEa, 4}, {kEa + 4, 4}}) && vec_is(r, 0, kEa, 8));
    r = fresh(); CHECK(run(ldr_q, r) && reads({{kEa, 4}, {kEa + 4, 4}, {kEa + 8, 4}, {kEa + 12, 4}}) && vec_is(r, 0, kEa, 16));
    r = fresh(); r.x[0] += 8; CHECK(run(ldur_d, r) && reads({{kEa, 4}, {kEa + 4, 4}}) && vec_is(r, 0, kEa, 8));
    r = fresh(); r.x[2] = 4; CHECK(run(ldr_s_reg, r) && reads({{kEa + 4, 4}}) && vec_is(r, 0, kEa + 4, 4));
    r = fresh(); CHECK(run(ldr_d_post, r) && vec_is(r, 0, kEa, 8) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 8);
    // pairs: the first register at the lower address, read first
    r = fresh(); CHECK(run(ldp_w, r) && reads({{kEa, 4}, {kEa + 4, 4}}) && r.x[1] == le(kEa, 4) && r.x[3] == le(kEa + 4, 4));
    r = fresh(); CHECK(run(ldp_x, r) && reads({{kEa + 16, 4}, {kEa + 20, 4}, {kEa + 24, 4}, {kEa + 28, 4}}) && r.x[1] == le(kEa + 16, 8) && r.x[3] == le(kEa + 24, 8));
    r = fresh(); CHECK(run(ldpsw, r) && r.x[1] == uint64_t(int64_t(int32_t(le(kEa, 4)))) && r.x[3] == uint64_t(int64_t(int32_t(le(kEa + 4, 4)))));
    r = fresh(); CHECK(run(ldp_s, r) && vec_is(r, 0, kEa, 4) && vec_is(r, 1, kEa + 4, 4));
    r = fresh(); CHECK(run(ldp_d_post, r) && vec_is(r, 0, kEa, 8) && vec_is(r, 1, kEa + 8, 8) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 16);
    r = fresh(); r.x[0] += 32; CHECK(run(ldp_q_pre, r) && vec_is(r, 0, kEa, 16) && vec_is(r, 1, kEa + 16, 16) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa);
    r = fresh(); CHECK(run(ldnp_w, r) && reads({{kEa, 4}, {kEa + 4, 4}}));
    // LD1 of one register
    r = fresh(); CHECK(run(ld1_8b, r) && reads({{kEa, 4}, {kEa + 4, 4}}) && vec_is(r, 0, kEa, 8));
    r = fresh(); CHECK(run(ld1_16b, r) && vec_is(r, 0, kEa, 16));
    r = fresh(); CHECK(run(ld1_2s_post, r) && vec_is(r, 0, kEa, 8) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 8);
    r = fresh(); r.x[2] = 0x40; CHECK(run(ld1_4s_reg, r) && vec_is(r, 0, kEa, 16) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 0x40);
    // stores: the register's bytes in memory order reach the device as big-endian values
    r = fresh(); r.x[1] = 0x1122334455667788ull; CHECK(run(str_w, r) && writes({{true, kEa, 4, 0x88776655u}}));
    r = fresh(); CHECK(run(str_wzr, r) && writes({{true, kEa + 4, 4, 0}}));
    r = fresh(); r.x[1] = 0xABCD; CHECK(run(strh, r) && writes({{true, kEa, 2, 0xCDABu}}));
    r = fresh(); r.x[1] = 0x1F2; CHECK(run(strb, r) && writes({{true, kEa + 3, 1, 0xF2u}}));
    r = fresh(); r.x[1] = 0x1122334455667788ull; CHECK(run(str_x, r) && writes({{true, kEa, 4, 0x88776655u}, {true, kEa + 4, 4, 0x44332211u}}));
    r = fresh(); r.x[0] += 4; r.x[1] = 0x01020304; CHECK(run(stur, r) && writes({{true, kEa, 4, 0x04030201u}}));
    r = fresh(); r.x[1] = 0x01020304; CHECK(run(str_pre, r) && writes({{true, kEa + 4, 4, 0x04030201u}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 4);
    r = fresh(); r.x[1] = 0x01020304; CHECK(run(str_post, r) && writes({{true, kEa, 4, 0x04030201u}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa - 4);
    r = fresh(); r.x[1] = 0x01020304; r.x[2] = 8; CHECK(run(str_reg, r) && writes({{true, kEa + 8, 4, 0x04030201u}}));
    r = fresh(); for (int i = 0; i < 16; ++i) r.v[0][i] = uint8_t(0x10 + i);
    CHECK(run(str_s, r) && writes({{true, kEa, 4, 0x10111213u}}));
    CHECK(run(str_d, r) && writes({{true, kEa, 4, 0x10111213u}, {true, kEa + 4, 4, 0x14151617u}}));
    CHECK(run(str_q, r) && writes({{true, kEa, 4, 0x10111213u}, {true, kEa + 4, 4, 0x14151617u}, {true, kEa + 8, 4, 0x18191A1Bu}, {true, kEa + 12, 4, 0x1C1D1E1Fu}}));
    CHECK(run(st1_8b, r) && writes({{true, kEa, 4, 0x10111213u}, {true, kEa + 4, 4, 0x14151617u}}));
    { Regs q = r; CHECK(run(st1_16b_post, q) && g_log.size() == 4 && q.x[0] == uint64_t(uintptr_t(kMem)) + kEa + 16); }
    r = fresh(); r.x[1] = 0xA1A2A3A4; r.x[3] = 0xB1B2B3B4; CHECK(run(stp_w, r) && writes({{true, kEa, 4, 0xA4A3A2A1u}, {true, kEa + 4, 4, 0xB4B3B2B1u}}));
    r = fresh(); r.x[0] += 16; r.x[1] = 1; r.x[3] = 2;
    CHECK(run(stp_x, r) && writes({{true, kEa, 4, 0x01000000u}, {true, kEa + 4, 4, 0}, {true, kEa + 8, 4, 0x02000000u}, {true, kEa + 12, 4, 0}}) && r.x[0] == uint64_t(uintptr_t(kMem)) + kEa);
    r = fresh(); CHECK(run(stp_xzr, r) && g_log.size() == 4 && g_log[3].value == 0);
    r = fresh(); for (int i = 0; i < 8; ++i) { r.v[0][i] = uint8_t(i); r.v[1][i] = uint8_t(0x80 + i); }
    CHECK(run(stp_d, r) && writes({{true, kEa, 4, 0x00010203u}, {true, kEa + 4, 4, 0x04050607u}, {true, kEa + 8, 4, 0x80818283u}, {true, kEa + 12, 4, 0x84858687u}}));
    // not handled: exclusives, acquire loads, prefetches, structure forms other than one register; nothing changes
    for (Insn insn : {ldxr, stxr, ldar, prfm, ld2, ld1_lane}) {
        r = fresh(); const Regs before = r;
        uc::HostAccess a;
        CHECK(!uc::fastmem_decode(fm_insns[insn], a) && !run(insn, r) && g_log.empty() && std::memcmp(&before, &r, sizeof r) == 0);
    }
    // outside the window (RAM, the byte below it, the byte after it): not emulated
    for (uint32_t ea : {0x80001000u, 0xCBFFFFFFu, 0xCE000000u}) {
        r = fresh(); r.x[0] = uint64_t(uintptr_t(kMem)) + ea; const Regs before = r;
        CHECK(!run(ldr_w, r) && !run(str_w, r) && g_log.empty() && std::memcmp(&before, &r, sizeof r) == 0);
    }
    // the window's first and last byte, and an access that starts in it and leaves it (as rdf64 did: two reads, the second past the end)
    r = fresh(); r.x[0] = uint64_t(uintptr_t(kMem)) + 0xCC000000u; CHECK(run(ldr_w, r) && reads({{0xCC000000u, 4}}));
    r = fresh(); r.x[0] = uint64_t(uintptr_t(kMem)) + 0xCDFFFFFCu; r.x[1] = 0x5A; CHECK(run(strb, r) && writes({{true, 0xCDFFFFFFu, 1, 0x5Au}}));
    r = fresh(); r.x[0] = uint64_t(uintptr_t(kMem)) + 0xCDFFFFFCu; CHECK(run(ldr_d, r) && reads({{0xCDFFFFFCu, 4}, {0xCE000000u, 4}}));
}

// The signal path: the accessors with addresses the optimiser cannot see go to the fault handler and the stub hooks (tests/mmio_stub.cpp:
// a read returns A5A5A5A5, the last access is recorded).
volatile uint32_t g_opaque = 0;
// The hooks run inside the signal handler, which the optimiser does not see: read what they recorded through volatile accesses.
#define LAST(x) (*reinterpret_cast<volatile decltype(x)*>(&(x)))
uint32_t opaque(uint32_t ea) { g_opaque = ea; return g_opaque; }

sigjmp_buf g_escape;
volatile int g_previous_calls = 0;
void previous_handler(int) { g_previous_calls = g_previous_calls + 1; siglongjmp(g_escape, 1); }

void signal_path() {
    Fix f; ppc::Ctx& c = f.c;
    const uc::FastmemStats before = uc::fastmem_stats();
    const uint32_t ea = opaque(0xCC006804u);
    LAST(g_mmio_last_addr) = 0;
    CHECK(ppc::rd32(c, ea) == 0xA5A5A5A5u && LAST(g_mmio_last_addr) == ea && LAST(g_mmio_last_size) == 4);
    CHECK(ppc::rd16(c, ea + 2) == 0xA5A5u && LAST(g_mmio_last_addr) == ea + 2 && LAST(g_mmio_last_size) == 2);
    CHECK(ppc::rd8(c, ea + 1) == 0xA5u && LAST(g_mmio_last_size) == 1);
    ppc::wr32(c, ea, 0x11223344u); CHECK(LAST(g_mmio_last_addr) == ea && LAST(g_mmio_last_size) == 4 && LAST(g_mmio_last_val) == 0x11223344u);
    ppc::wr16(c, ea, 0x12345u); CHECK(LAST(g_mmio_last_size) == 2 && LAST(g_mmio_last_val) == 0x2345u);
    ppc::wr8(c, ea + 3, 0x1FFu); CHECK(LAST(g_mmio_last_addr) == ea + 3 && LAST(g_mmio_last_size) == 1 && LAST(g_mmio_last_val) == 0xFFu);
    // 8 bytes: two 4-byte accesses, the high word first (the explicit path's order)
    uint64_t bits; const double d = ppc::rdf64(c, ea); std::memcpy(&bits, &d, 8); CHECK(bits == 0xA5A5A5A5A5A5A5A5ull && LAST(g_mmio_last_addr) == ea + 4);
    double v; bits = 0x1122334455667788ull; std::memcpy(&v, &bits, 8);
    ppc::wrf64(c, ea, v); CHECK(LAST(g_mmio_last_addr) == ea + 4 && LAST(g_mmio_last_val) == 0x55667788u);
    // the instruction forms (update forms write rA back)
    c.r[3] = opaque(0xCC006800u); c.r[5] = 0xCAFEF00Du;
    ppc::op_stwu(c, 5, 3, 8); CHECK(c.r[3] == 0xCC006808u && LAST(g_mmio_last_addr) == 0xCC006808u && LAST(g_mmio_last_val) == 0xCAFEF00Du);
    c.r[3] = opaque(c.r[3]); ppc::op_lhau(c, 6, 3, 2); CHECK(c.r[6] == 0xFFFFA5A5u && c.r[3] == 0xCC00680Au);
    c.r[3] = opaque(c.r[3]); ppc::op_lbz(c, 7, 3, 1); CHECK(c.r[7] == 0xA5u);
    c.r[3] = opaque(c.r[3]); c.r[4] = opaque(4); ppc::op_stfdx(c, 1, 3, 4); CHECK(LAST(g_mmio_last_addr) == 0xCC006812u);
    c.r[3] = opaque(c.r[3]); ppc::op_lfs(c, 2, 3, 0); { const uint32_t raw = 0xA5A5A5A5u; float fl; std::memcpy(&fl, &raw, 4); CHECK(c.f[2] == double(fl)); }
    // paired singles: a float pair is one 8-byte access in the fast path
    c.gqr[0] = 0; c.r[3] = opaque(0xCC006800u);
    ppc::op_psq_l(c, 3, 3, 0, 0, 0); { const uint32_t raw = 0xA5A5A5A5u; float fl; std::memcpy(&fl, &raw, 4); CHECK(c.f[3] == double(fl) && c.ps1[3] == double(fl)); }
    CHECK(LAST(g_mmio_last_addr) == 0xCC006804u);
    c.r[3] = opaque(c.r[3]); c.f[4] = 1.0; c.ps1[4] = 2.0; ppc::op_psq_st(c, 4, 3, 16, 0, 0); CHECK(LAST(g_mmio_last_addr) == 0xCC006814u && LAST(g_mmio_last_val) == 0x40000000u);
    c.r[3] = opaque(c.r[3]); ppc::op_psq_st(c, 4, 3, 24, 1, 0); CHECK(LAST(g_mmio_last_addr) == 0xCC006818u && LAST(g_mmio_last_val) == 0x3F800000u);
    const uc::FastmemStats after = uc::fastmem_stats();
    CHECK(after.reads - before.reads == 8 && after.writes - before.writes == 8);   // one fault per access (rdf64/wrf64: one 8-byte access)
    // RAM is untouched by any of it and still plain memory
    ppc::wr32(c, opaque(0x80001000u), 0x01020304u); CHECK(ppc::rd32(c, 0x80001000u) == 0x01020304u);
    // a guest thread on another host thread
    std::thread other([&] {
        ppc::Ctx t{}; t.mem = c.mem;
        CHECK(ppc::rd32(t, opaque(0xCC003000u)) == 0xA5A5A5A5u);
        ppc::wr16(t, opaque(0xCC003002u), 0x7777u);
    });
    other.join();
    CHECK(LAST(g_mmio_last_addr) == 0xCC003002u && LAST(g_mmio_last_val) == 0x7777u);
    // a fault outside the window goes on to the handler installed before (the crash report in the game), unchanged
    struct sigaction mine{}; mine.sa_handler = previous_handler; sigemptyset(&mine.sa_mask);
    sigaction(SIGSEGV, &mine, nullptr); sigaction(SIGBUS, &mine, nullptr);
    uc::fastmem_install();
    for (uint32_t stray : {0xCB000000u, 0xCE000000u, 0x70000000u}) {
        const int calls = g_previous_calls;
        if (sigsetjmp(g_escape, 1) == 0) { (void)ppc::rd32(c, opaque(stray)); CHECK(!"the stray access did not fault"); }
        CHECK(g_previous_calls == calls + 1);
    }
    CHECK(ppc::rd32(c, opaque(0xCC000004u)) == 0xA5A5A5A5u);   // and the handler still serves the window afterwards
}
}  // namespace

int main() {
    CHECK(kInsnCount > 0);
    decode_and_emulate();
    signal_path();
    std::printf("test_fastmem: ok\n");
    return 0;
}
#else
int main() { std::printf("test_fastmem: fastmem not built on this platform (explicit MMIO test)\n"); return 0; }
#endif
