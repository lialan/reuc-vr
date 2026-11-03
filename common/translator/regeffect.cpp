#include "translator/regeffect.h"
#include "ppc/insn.h"
#include <cstring>

namespace uc {

namespace {
struct Builder {
    RegEffect e;
    void use(int i) { e.use.set(size_t(i)); }
    void def(int i) { e.may.set(size_t(i)); e.must.set(size_t(i)); }
    void r(uint32_t i) { use(res::R + int(i)); }
    void r0(uint32_t i) { if (i) r(i); }   // (rA|0): register 0 reads as the literal 0
    void dr(uint32_t i) { def(res::R + int(i)); }
    void f(uint32_t i) { use(res::F + int(i)); }
    void fp(uint32_t i) { use(res::F + int(i)); use(res::P + int(i)); }   // both paired-single halves
    void df(uint32_t i) { def(res::F + int(i)); }
    void dfp(uint32_t i) { def(res::F + int(i)); def(res::P + int(i)); }
    void cr0() { def(res::CR); use(res::SO); }                            // integer record form: cr0 also copies XER[SO]
    void cr1() { def(res::CR + 1); use(res::FPSCR); }                     // floating-point record form: cr1 = FPSCR[0:3]
};
bool starts(const char* s, const char* p) { return std::strncmp(s, p, std::strlen(p)) == 0; }
bool is(const char* s, const char* p) { return std::strcmp(s, p) == 0; }
}  // namespace

RegEffect reg_effect(const ppc::OpInfo& op, uint32_t w, bool rc, bool& ok) {
    using ppc::Kind;
    Builder b;
    ok = true;
    const char* n = op.name;
    const uint32_t T = ppc::t(w), A = ppc::a(w), B = ppc::b(w), C = ppc::c(w);
    const size_t len = std::strlen(n);
    switch (op.kind) {
        case Kind::BR: case Kind::BC: case Kind::BCLR: case Kind::BCCTR: case Kind::SC: case Kind::UNSUP: ok = false; return b.e;
        default: break;
    }
    // ---- loads and stores ----
    if (is(n, "lmw")) { b.r0(A); for (uint32_t i = T; i < 32; ++i) b.dr(i); return b.e; }
    if (is(n, "stmw")) { b.r0(A); for (uint32_t i = T; i < 32; ++i) b.r(i); return b.e; }
    if (is(n, "lwarx")) { b.r0(A); b.r(B); b.dr(T); b.def(res::RES); return b.e; }
    if (is(n, "stwcx_")) { b.r0(A); b.r(B); b.r(T); b.use(res::RES); b.def(res::RES); b.cr0(); return b.e; }
    if (is(n, "lwbrx") || is(n, "lhbrx")) { b.r0(A); b.r(B); b.dr(T); return b.e; }
    if (is(n, "stwbrx") || is(n, "sthbrx")) { b.r0(A); b.r(B); b.r(T); return b.e; }
    if (is(n, "stfiwx")) { b.r0(A); b.r(B); b.f(T); return b.e; }
    if (op.kind == Kind::AB) { if (is(n, "dcbz") || is(n, "dcbz_l")) { b.r0(A); b.r(B); } return b.e; }   // cache control: no register effect
    const bool psq = op.kind == Kind::PSQ || op.kind == Kind::PSQX;
    const bool load = starts(n, "lwz") || starts(n, "lbz") || starts(n, "lhz") || starts(n, "lha") || starts(n, "lfs") || starts(n, "lfd") || starts(n, "psq_l");
    const bool store = starts(n, "stw") || starts(n, "stb") || starts(n, "sth") || starts(n, "stfs") || starts(n, "stfd") || starts(n, "psq_st");
    if (load || store) {
        // update forms end in 'u' (lwzu) or 'ux' (lwzux); indexed forms in 'x' (psq_lx/psq_lux: index register in b)
        const bool idx = op.kind == Kind::PSQX || (op.kind == Kind::RRR && n[len - 1] == 'x');
        const bool upd = (idx ? len >= 2 && n[len - 2] == 'u' : n[len - 1] == 'u');
        if (upd) { b.r(A); b.dr(A); } else b.r0(A);
        if (idx) b.r(B);
        if (psq) b.use(res::GQR + int(op.kind == Kind::PSQ ? (w >> 12) & 7 : (w >> 7) & 7));
        if (load) {
            if (psq || starts(n, "lfs")) b.dfp(T);   // single loads fill both halves (psq_l with W=1: ps1 = 1.0)
            else if (starts(n, "lfd")) b.df(T);
            else b.dr(T);
            if (upd && T == A) { b.e.must.reset(size_t(res::R + int(A))); }   // invalid form: which write lands last is the runtime's choice
        } else {
            if (psq) b.fp(T); else if (starts(n, "stf")) b.f(T); else b.r(T);
        }
        return b.e;
    }
    // ---- integer arithmetic ----
    if (is(n, "addi") || is(n, "addis")) { b.r0(A); b.dr(T); return b.e; }
    if (is(n, "addic") || is(n, "subfic")) { b.r(A); b.dr(T); b.def(res::CA); return b.e; }
    if (is(n, "addic_")) { b.r(A); b.dr(T); b.def(res::CA); b.cr0(); return b.e; }
    if (is(n, "mulli")) { b.r(A); b.dr(T); return b.e; }
    if (op.sub == ppc::Sub::X9) {   // XO-form: t = rD, a, b
        b.r(A);
        if (op.kind == Kind::RRR) b.r(B);
        const bool carry_in = is(n, "adde") || is(n, "subfe") || is(n, "addze") || is(n, "addme") || is(n, "subfze") || is(n, "subfme");
        const bool carry_out = carry_in || is(n, "addc") || is(n, "subfc");
        if (carry_in) b.use(res::CA);
        if (carry_out) b.def(res::CA);
        b.dr(T);
        if (rc) b.cr0();
        return b.e;
    }
    // ---- logical, shift, rotate: t = rS, a = rA (destination) ----
    if (op.kind == Kind::RRR && op.primary == 31 && op.rc) {   // and andc or orc xor nor nand eqv slw srw sraw srawi
        b.r(T);
        if (!is(n, "srawi")) b.r(B);
        b.dr(A);
        if (is(n, "sraw") || is(n, "srawi")) b.def(res::CA);
        if (rc) b.cr0();
        return b.e;
    }
    if (op.kind == Kind::RR && op.primary == 31) {   // extsb extsh cntlzw (neg/addze... are X9 above)
        b.r(T); b.dr(A); if (rc) b.cr0(); return b.e;
    }
    if (op.kind == Kind::RRU) {   // ori oris xori xoris andi. andis.
        b.r(T); b.dr(A);
        if (is(n, "andi_") || is(n, "andis_")) b.cr0();
        return b.e;
    }
    if (op.kind == Kind::RRM) { b.r(T); if (is(n, "rlwimi")) b.r(A); b.dr(A); if (rc) b.cr0(); return b.e; }
    if (op.kind == Kind::RRRM) { b.r(T); b.r(B); b.dr(A); if (rc) b.cr0(); return b.e; }
    // ---- compares ----
    if (is(n, "cmp") || is(n, "cmpl")) { b.r(A); b.r(B); b.use(res::SO); b.def(res::CR + int(ppc::crfd(w))); return b.e; }
    if (is(n, "cmpi") || is(n, "cmpli")) { b.r(A); b.use(res::SO); b.def(res::CR + int(ppc::crfd(w))); return b.e; }
    if (is(n, "fcmpu") || is(n, "fcmpo") || is(n, "ps_cmpu0") || is(n, "ps_cmpo0")) { b.f(A); b.f(B); b.def(res::CR + int(ppc::crfd(w))); return b.e; }
    if (is(n, "ps_cmpu1") || is(n, "ps_cmpo1")) { b.use(res::P + int(A)); b.use(res::P + int(B)); b.def(res::CR + int(ppc::crfd(w))); return b.e; }
    // ---- condition register ----
    if (op.kind == Kind::CRB) { b.use(res::CR + int(T >> 2)); b.use(res::CR + int(A >> 2)); b.use(res::CR + int(B >> 2)); b.def(res::CR + int(T >> 2)); return b.e; }
    if (is(n, "mcrf")) { b.use(res::CR + int(ppc::crfs(w))); b.def(res::CR + int(ppc::crfd(w))); return b.e; }
    if (is(n, "mfcr")) { for (int i = 0; i < 8; ++i) b.use(res::CR + i); b.dr(T); return b.e; }
    if (is(n, "mtcrf")) { b.r(T); for (int i = 0; i < 8; ++i) if (ppc::crm(w) & (0x80u >> i)) b.def(res::CR + i); return b.e; }
    if (is(n, "mcrxr")) { b.use(res::CA); b.use(res::OV); b.use(res::SO); b.def(res::CR + int(ppc::crfd(w))); b.def(res::CA); b.def(res::OV); b.def(res::SO); return b.e; }
    // ---- special registers ----
    if (is(n, "mfspr") || is(n, "mtspr")) {
        const uint32_t s = ppc::spr(w);
        const bool from = is(n, "mfspr");
        if (from) b.dr(T); else b.r(T);
        auto field = [&](int i) { if (from) b.use(i); else b.def(i); };
        if (s == 1) { field(res::CA); field(res::OV); field(res::SO); }
        else if (s == 8) field(res::LR);
        else if (s == 9) field(res::CTR);
        else if (s >= 912 && s <= 919) field(res::GQR + int(s - 912));
        return b.e;   // other SPRs live in the real context (not cached)
    }
    if (is(n, "mftb") || is(n, "mfmsr")) { b.dr(T); return b.e; }
    if (is(n, "mtmsr")) { b.r(T); return b.e; }
    if (op.kind == Kind::NONE || is(n, "tw") || is(n, "twi")) return b.e;   // sync isync eieio; conditional traps are no-ops
    // ---- floating point status ----
    if (is(n, "mffs")) { b.use(res::FPSCR); b.df(T); if (rc) b.cr1(); return b.e; }
    if (is(n, "mtfsf")) { b.f(B); b.use(res::FPSCR); b.def(res::FPSCR); if (rc) b.cr1(); return b.e; }
    if (is(n, "mtfsfi") || is(n, "mtfsb0") || is(n, "mtfsb1")) { b.use(res::FPSCR); b.def(res::FPSCR); if (rc) b.cr1(); return b.e; }
    if (is(n, "mcrfs")) { b.use(res::FPSCR); b.def(res::FPSCR); b.def(res::CR + int(ppc::crfd(w))); return b.e; }
    // ---- floating point and paired singles: t = frD ----
    const bool ps = starts(n, "ps_");
    // single-precision results are written to both halves (fabs/fnabs end in 's' but are double operations)
    static const char* const kSingle[] = {"fadds", "fsubs", "fdivs", "fmuls", "fmadds", "fmsubs", "fnmadds", "fnmsubs", "fres", "fsqrts", "frsp"};
    bool single = ps;
    for (const char* s : kSingle) single = single || is(n, s);
    auto src = [&](uint32_t i) { if (ps) b.fp(i); else b.f(i); };
    switch (op.kind) {
        case Kind::RRR: src(A); src(B); break;               // fadd fsub fdiv ps_add ps_sub ps_div ps_merge*
        case Kind::RAC: src(A); src(C); break;               // fmul ps_mul ps_muls0/1
        case Kind::RRRR: src(A); src(B); src(C); break;      // fmadd... fsel ps_madd... ps_sel ps_sum0/1
        case Kind::RB: src(B); break;                        // fmr fneg fabs fnabs frsp fres frsqrte fsqrts fctiw ps_mr ps_res...
        default: ok = false; return b.e;
    }
    if (single) b.dfp(T); else b.df(T);
    if (rc) b.cr1();
    return b.e;
}

std::string reg_field(int i, const std::string& ctx) {
    auto idx = [&](const char* arr, int k) { return ctx + "." + arr + "[" + std::to_string(k) + "]"; };
    if (i < res::F) return idx("r", i - res::R);
    if (i < res::P) return idx("f", i - res::F);
    if (i < res::CR) return idx("ps1", i - res::P);
    if (i < res::CA) return idx("cr", i - res::CR);
    if (i >= res::GQR && i < res::FPSCR) return idx("gqr", i - res::GQR);
    switch (i) {
        case res::CA: return ctx + ".xer_ca";
        case res::OV: return ctx + ".xer_ov";
        case res::SO: return ctx + ".xer_so";
        case res::LR: return ctx + ".lr";
        case res::CTR: return ctx + ".ctr";
        case res::FPSCR: return ctx + ".fpscr";
        case res::RES: return ctx + ".reserve_addr";
        case res::POLL: return ctx + ".poll_count";
        default: return "";
    }
}

}  // namespace uc
