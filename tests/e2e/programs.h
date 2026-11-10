#pragma once
#include "dol_builder.h"
#include "ppc_enc.h"
#include "ppc_rt.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace e2e {
using namespace enc;

struct Program {
    const char* name;
    std::vector<uint8_t> (*build)();
    void (*init)(ppc::Ctx&);
    bool (*check)(ppc::Ctx&);
};

inline std::vector<uint8_t> loop_dol() {
    Asm a;
    a.emit(li(3, 0)); a.emit(li(5, 0)); a.emit(li(4, 10)); a.emit(mtctr(4));
    uint32_t top = a.here();
    a.emit(addi(5, 5, 1)); a.emit(add(3, 3, 5)); a.bc_to(16, 0, top); a.emit(blr);
    return make_dol(a.w);
}

inline std::vector<uint8_t> calls_dol() {
    Asm a;
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(stw(0, 20, 1)); a.emit(li(3, 1));
    size_t c1 = a.emit(0);
    a.emit(lwz(0, 20, 1)); a.emit(addi(1, 1, 16)); a.emit(mtlr(0)); a.emit(blr);
    uint32_t f = a.here();
    a.emit(stwu(1, -16, 1)); a.emit(mflr(0)); a.emit(stw(0, 20, 1));
    size_t c2 = a.emit(0), c3 = a.emit(0);
    a.emit(lwz(0, 20, 1)); a.emit(addi(1, 1, 16)); a.emit(mtlr(0)); a.emit(blr);
    uint32_t g = a.here();
    a.emit(addi(3, 3, 10)); a.emit(blr);
    a.patch_bl(c1, f); a.patch_bl(c2, g); a.patch_bl(c3, g);
    return make_dol(a.w);
}

template <int Sel> std::vector<uint8_t> jt_dol() {
    Asm a;
    a.emit(li(3, Sel)); a.emit(cmplwi(3, 2));
    size_t bg = a.emit(0);
    a.emit(lis(4, 0x8040)); a.emit(addi(4, 4, 0)); a.emit(slwi(0, 3, 2)); a.emit(lwzx(0, 4, 0)); a.emit(mtctr(0)); a.emit(bctr);
    uint32_t c0 = a.here(); a.emit(li(3, 100)); a.emit(blr);
    uint32_t c1 = a.here(); a.emit(li(3, 200)); a.emit(blr);
    uint32_t c2 = a.here(); a.emit(li(3, 300)); a.emit(blr);
    uint32_t d = a.here();  a.emit(li(3, -1)); a.emit(blr);
    a.patch_bc(bg, 12, 1, d);
    std::vector<uint8_t> data; be32(data, c0); be32(data, c1); be32(data, c2);
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> indirect_dol() {
    Asm a;
    a.emit(lis(4, 0x8040)); a.emit(addi(4, 4, 0)); a.emit(lwz(12, 0, 4)); a.emit(mtctr(12)); a.emit(0x4E800421u);  // bctrl
    a.emit(addi(3, 3, 1)); a.emit(blr);
    uint32_t callee = a.here(); a.emit(li(3, 77)); a.emit(blr);
    std::vector<uint8_t> data; be32(data, callee);
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> float_dol() {
    Asm a;
    a.emit(lis(4, 0x8040)); a.emit(lfs(1, 0, 4)); a.emit(fadds(2, 1, 1)); a.emit(fctiwz(3, 2));
    a.emit(lis(5, 0x8050)); a.emit(li(6, 0)); a.emit(stfiwx(3, 5, 6)); a.emit(lwz(3, 0, 5)); a.emit(blr);
    std::vector<uint8_t> data; be32(data, 0x3FC00000);   // 1.5f
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> ps_dol() {
    Asm a;
    a.emit(lis(4, 0x8040)); a.emit(psq_l(1, 0, 4, 0, 0)); a.emit(ps_add(2, 1, 1)); a.emit(lis(5, 0x8050));
    a.emit(psq_st(2, 0, 5, 0, 0)); a.emit(lwz(3, 0, 5)); a.emit(lwz(6, 4, 5)); a.emit(blr);
    std::vector<uint8_t> data; be32(data, 0x3F800000); be32(data, 0x40000000);   // 1.0f, 2.0f
    return make_dol(a.w, data);
}

inline std::vector<uint8_t> datawords_dol() {
    Asm a;
    a.emit(li(3, 5));
    size_t br = a.emit(0);
    a.emit(0x14000000u); a.emit(0x00000000u);            // illegal (primary 5) and zero words, never executed
    uint32_t skip = a.here();
    a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_b(br, skip);
    return make_dol(a.w);
}

inline std::vector<uint8_t> badbctr_dol() {
    Asm a;
    a.emit(lis(12, 0x8000)); a.emit(ori(12, 12, 0x3FFC)); a.emit(mtctr(12)); a.emit(0x4E800421u); a.emit(blr);
    return make_dol(a.w);
}

inline std::vector<uint8_t> illegal_exec_dol() {
    Asm a;
    a.emit(li(3, 5)); a.emit(0x14000000u); a.emit(blr);
    return make_dol(a.w);
}

// A branch into the middle of another function runs the code from there (secondary entry), not from the function start.
inline std::vector<uint8_t> secondary_dol() {
    Asm a;
    size_t c1 = a.emit(0);                 // bl G   (r3 = 99 + 1)
    a.emit(li(3, 5));
    size_t br = a.emit(0);                 // b Gmid (skips "li r3,99")
    a.emit(blr);
    uint32_t g = a.here();
    a.emit(li(3, 99));
    uint32_t gmid = a.here();
    a.emit(addi(3, 3, 1)); a.emit(blr);
    a.patch_bl(c1, g); a.patch_b(br, gmid);
    return make_dol(a.w);
}

// A function whose end falls into the next function (an entry because it is called) continues there.
inline std::vector<uint8_t> fallthrough_dol() {
    Asm a;
    size_t c1 = a.emit(0);                 // bl F
    a.emit(mr(4, 3)); a.emit(li(3, 0));
    size_t c2 = a.emit(0);                 // bl G
    a.emit(add(3, 3, 4)); a.emit(blr);
    uint32_t f = a.here(); a.emit(li(3, 1));
    uint32_t g = a.here(); a.emit(addi(3, 3, 41)); a.emit(blr);
    a.patch_bl(c1, f); a.patch_bl(c2, g);
    return make_dol(a.w);
}

inline std::vector<uint8_t> outside_dol() {
    Asm a;
    a.emit(b(0x100000)); a.emit(blr);
    return make_dol(a.w);
}

// Condition-register writes the translator may drop or fold into the branch must not change what the program computes: every branch below
// reads a CR field that is still live, or one that is dead after the branch (r3 collects the paths that run: 1 + 10 + 5).
inline std::vector<uint8_t> crflow_dol() {
    Asm a;
    a.emit(li(3, 0)); a.emit(li(4, 5));
    a.emit(cmpwi(4, 5)); size_t s1 = a.emit(0);                       // bne: not taken, cr0 overwritten before any other read
    a.emit(addi(3, 3, 1));
    a.patch_bc(s1, 4, 2, a.here());
    a.emit(cmpwi(4, 7)); size_t s2 = a.emit(0);                       // bne: taken
    a.emit(addi(3, 3, 100));
    a.patch_bc(s2, 4, 2, a.here());
    a.emit(cmpwi(4, 3)); size_t g1 = a.emit(0);                       // bgt: taken; the compare stays because blt reads cr0 again
    a.emit(addi(3, 3, 1000));
    a.patch_bc(g1, 12, 1, a.here());
    size_t l1 = a.emit(0);                                            // blt: not taken
    a.emit(addi(3, 3, 10));
    a.patch_bc(l1, 12, 0, a.here());
    a.emit(cmpwi(4, 100));                                            // never read: replaced below
    a.emit(cmpwi(4, 5)); size_t e1 = a.emit(0);                       // beq: taken
    a.emit(addi(3, 3, 10000));
    a.patch_bc(e1, 12, 2, a.here());
    a.emit(cmpwi(4, 5)); size_t c1 = a.emit(0);                       // cr0 read after a call that leaves it alone
    size_t e2 = a.emit(0);                                            // beq: taken
    a.emit(addi(3, 3, 100000));
    a.patch_bc(e2, 12, 2, a.here());
    a.emit(cmpwi_cr(2, 4, 5)); size_t c2 = a.emit(0);                 // cr2 survives the call too
    size_t e3 = a.emit(0);                                            // beq cr2: taken
    a.emit(addi(3, 3, 200000));
    a.patch_bc(e3, 12, 10, a.here());
    a.emit(li(6, -1)); a.emit(add_rc(7, 6, 6)); size_t r1 = a.emit(0);   // blt reads the record form's cr0: taken
    a.emit(addi(3, 3, 1000000));
    a.patch_bc(r1, 12, 0, a.here());
    a.emit(add_rc(7, 6, 6)); a.emit(cmpwi(4, 5)); size_t x = a.emit(0);   // the record form's cr0 is overwritten unread; bne: not taken
    a.emit(addi(3, 3, 5));
    a.patch_bc(x, 4, 2, a.here());
    a.emit(blr);
    uint32_t callee = a.here(); a.emit(addi(5, 5, 1)); a.emit(blr);
    a.patch_bl(c1, callee); a.patch_bl(c2, callee);
    return make_dol(a.w);
}

// Compare fusion (translator/emit.cpp plan_function): every folded form of fcmp, cror, mfcr+rlwinm, a delayed integer compare and a record
// form tested by its branch, over a table of operand pairs (zeros of both signs, infinities, NaN, extreme integers). Each pair leaves one word
// of result bits; cmpfuse_check computes them from the PowerPC definitions. e2e_cmpfuse_nocr runs the same program translated without the
// CR optimisation.
namespace fuse {
inline const double kDoubles[] = {0.0, -0.0, 1.0, -1.0, 2.5, 1e300, __builtin_inf(), -__builtin_inf(), __builtin_nan("")};
inline const uint32_t kInts[] = {0, 1, 0xFFFFFFFFu, 5, 0x80000000u, 0x7FFFFFFFu, 2};
constexpr size_t kNd = sizeof kDoubles / sizeof kDoubles[0], kNi = sizeof kInts / sizeof kInts[0], kPairs = kNd * kNd;
inline uint32_t int_a(size_t k) { return kInts[k % kNi]; }
inline uint32_t int_b(size_t k) { return kInts[(k / kNi + k) % kNi]; }
inline uint32_t X(int op, int t, int a, int b, int xo, int rc = 0) { return enc::X(uint32_t(op), t, a, b, uint32_t(xo), uint32_t(rc)); }
inline uint32_t rlwinm(int a, int s, int sh, int mb, int me, int rc = 0) { return 21u << 26 | u(s) << 21 | u(a) << 16 | u(sh) << 11 | u(mb) << 6 | u(me) << 1 | u(rc); }
}  // namespace fuse
inline std::vector<uint8_t> cmpfuse_dol() {
    using namespace fuse;
    Asm a;
    const uint32_t skip = 8;   // a branch over the next instruction
    a.emit(lis(4, 0x8040)); a.emit(lis(5, 0x8050)); a.emit(li(6, int(kPairs))); a.emit(mtctr(6));
    const uint32_t top = a.here();
    a.emit(D(50, 1, 4, 0)); a.emit(D(50, 2, 4, 8)); a.emit(lwz(8, 16, 4)); a.emit(lwz(9, 20, 4)); a.emit(li(3, 0));
    a.emit(X(63, 0, 1, 2, 0)); a.emit(bc(4, 0, skip)); a.emit(ori(3, 3, 1));                        // fcmpu; bge: bit 0 = lt
    a.emit(X(63, 4, 1, 2, 0)); a.emit(X(19, 6, 4, 6, 449)); a.emit(bc(4, 6, skip)); a.emit(ori(3, 3, 2));   // cror EQ = LT|EQ; bne cr1: bit 1 = le
    a.emit(X(63, 0, 1, 2, 32)); a.emit(X(19, 2, 1, 2, 449)); a.emit(X(31, 6, 0, 0, 19)); a.emit(rlwinm(6, 6, 3, 31, 31));   // fcmpo, cror GT|EQ, mfcr, bit 2
    a.emit(slwi(6, 6, 2)); a.emit(X(31, 3, 3, 6, 444));                                               // bit 2 = ge
    a.emit(X(63, 0, 1, 2, 0)); a.emit(X(31, 7, 0, 0, 19)); a.emit(rlwinm(7, 7, 2, 31, 31, 1));        // mfcr, rlwinm. of GT
    a.emit(bc(12, 2, skip)); a.emit(ori(3, 3, 8)); a.emit(slwi(7, 7, 4)); a.emit(X(31, 3, 3, 7, 444));   // beq: bit 3 = gt, bit 4 = the register
    a.emit(X(63, 8, 1, 2, 0)); a.emit(bc(4, 11, skip)); a.emit(ori(3, 3, 32));                       // bns cr2: bit 5 = unordered
    a.emit(X(63, 12, 1, 2, 0)); a.emit(bc(4, 14, skip)); a.emit(ori(3, 3, 64));                      // bne cr3: bit 6 = eq
    a.emit(X(31, 0, 8, 9, 0)); a.emit(addi(8, 8, 1)); a.emit(addi(10, 9, 0)); a.emit(bc(4, 0, skip)); a.emit(ori(3, 3, 128));   // cmpw, its operand changes before the bge: bit 7
    a.emit(addi(8, 8, -1));
    a.emit(X(31, 16, 8, 9, 32)); a.emit(mr(11, 8)); a.emit(li(8, 0)); a.emit(bc(12, 17, skip)); a.emit(ori(3, 3, 256));   // cmplw cr4, bgt cr4: bit 8 = !gtu
    a.emit(mr(8, 11));
    a.emit(X(31, 8, 12, 9, 28, 1)); a.emit(bc(12, 2, skip)); a.emit(ori(3, 3, 512));                 // and.; beq: bit 9 = (a & b) != 0
    a.emit(D(28, 8, 12, 1)); a.emit(bc(4, 2, skip)); a.emit(ori(3, 3, 1024));                        // andi. 1; bne: bit 10 = a even
    a.emit(X(31, 12, 9, 8, 40, 1)); a.emit(bc(12, 0, skip)); a.emit(ori(3, 3, 2048));                // subf. a - b; blt: bit 11
    a.emit(cmpwi_cr(5, 8, 0)); a.emit(addi(12, 8, 0)); a.emit(bc(4, 22, skip)); a.emit(ori(3, 3, 4096));   // cmpwi cr5 a few before bne cr5: bit 12 = a == 0
    a.emit(stw(3, 0, 5)); a.emit(addi(4, 4, 24)); a.emit(addi(5, 5, 4)); a.bc_to(16, 0, top); a.emit(blr);
    std::vector<uint8_t> data;
    for (size_t k = 0; k < kPairs; ++k) {
        uint64_t x, y; std::memcpy(&x, &kDoubles[k / kNd], 8); std::memcpy(&y, &kDoubles[k % kNd], 8);
        be32(data, uint32_t(x >> 32)); be32(data, uint32_t(x)); be32(data, uint32_t(y >> 32)); be32(data, uint32_t(y));
        be32(data, int_a(k)); be32(data, int_b(k));
    }
    return make_dol(a.w, data);
}
inline bool cmpfuse_check(ppc::Ctx& c) {
    using namespace fuse;
    for (size_t k = 0; k < kPairs; ++k) {
        const double x = kDoubles[k / kNd], y = kDoubles[k % kNd];
        const bool un = x != x || y != y, lt = !un && x < y, gt = !un && x > y, eq = !un && !lt && !gt;
        const uint32_t ia = int_a(k), ib = int_b(k);
        uint32_t want = 0;
        want |= lt ? 1u : 0u; want |= lt || eq ? 2u : 0u; want |= gt || eq ? 4u : 0u; want |= gt ? 8u | 16u : 0u;
        want |= un ? 32u : 0u; want |= eq ? 64u : 0u; want |= int32_t(ia) < int32_t(ib) ? 128u : 0u; want |= !(ia > ib) ? 256u : 0u;
        want |= (ia & ib) ? 512u : 0u; want |= !(ia & 1) ? 1024u : 0u; want |= !(int32_t(ia - ib) < 0) ? 2048u : 0u; want |= ia == 0 ? 4096u : 0u;
        const uint32_t got = ppc::rd32(c, 0x80500000u + 4 * uint32_t(k));
        if (got != want) { std::fprintf(stderr, "pair %zu (%g, %g, %08X, %08X): %04X, expected %04X\n", k, x, y, ia, ib, got, want); return false; }
    }
    return true;
}

template <uint32_t Expect> bool r3_is(ppc::Ctx& c) { return c.r[3] == Expect; }

inline const std::vector<Program>& all() {
    static const std::vector<Program> v = {
        {"loop", loop_dol, nullptr, r3_is<55>},
        {"calls", calls_dol, nullptr, [](ppc::Ctx& c) { return c.r[3] == 21 && c.r[1] == 0x817FFF00; }},
        {"jt0", jt_dol<0>, nullptr, r3_is<100>},
        {"jt1", jt_dol<1>, nullptr, r3_is<200>},
        {"jt2", jt_dol<2>, nullptr, r3_is<300>},
        {"jt9", jt_dol<9>, nullptr, r3_is<0xFFFFFFFFu>},
        {"indirect", indirect_dol, nullptr, r3_is<78>},
        {"float", float_dol, nullptr, r3_is<3>},
        {"ps", ps_dol, nullptr, [](ppc::Ctx& c) { return c.r[3] == 0x40000000 && c.r[6] == 0x40800000; }},
        {"datawords", datawords_dol, nullptr, r3_is<6>},
        {"badbctr", badbctr_dol, nullptr, [](ppc::Ctx&) { return false; }},
        {"illegal_exec", illegal_exec_dol, nullptr, [](ppc::Ctx&) { return false; }},
        {"secondary", secondary_dol, nullptr, r3_is<6>},
        {"fallthrough", fallthrough_dol, nullptr, r3_is<83>},
        {"outside", outside_dol, nullptr, [](ppc::Ctx&) { return false; }},
        {"crflow", crflow_dol, nullptr, r3_is<16>},
        {"cmpfuse", cmpfuse_dol, nullptr, cmpfuse_check},
        {"cmpfuse_nocr", cmpfuse_dol, nullptr, cmpfuse_check},
    };
    return v;
}
inline const Program* find(const char* name) {
    for (auto& p : all()) if (!std::strcmp(p.name, name)) return &p;
    return nullptr;
}
}  // namespace e2e
