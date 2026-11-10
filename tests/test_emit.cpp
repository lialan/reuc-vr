#include "check.h"
#include "dol_builder.h"
#include "ppc_enc.h"
#include "translator/emit.h"
#include <cstdio>
using namespace enc;

static uc::Program make_prog(const std::vector<uint32_t>& code, uc::Config cfg = {}) {
    static std::vector<uint8_t> dol; static uc::DolResult parsed;   // keep storage alive for the Image spans
    dol = make_dol(code);
    parsed = uc::parse_dol(dol);
    uc::Program p;
    p.img = uc::Image{dol, &parsed.image};
    p.cfg = cfg;
    p.an = uc::analyze(p.img, cfg);
    return p;
}

int main() {
    uc::EmitStats st{};
    auto p = make_prog({addi(3, 0, 5), blr});
    const uc::Function& f = p.an.functions[0];
    CHECK(uc::emit_insn(p, f, 0x80003100, addi(3, 0, 5), st) == "ppc::op_addi(c, 3, 0, 5);");
    CHECK(uc::emit_insn(p, f, 0x80003104, add(3, 4, 5), st) == "ppc::op_add(c, 3, 4, 5, false);");
    CHECK(uc::emit_insn(p, f, 0x80003104, 0x7C642A15, st) == "ppc::op_add(c, 3, 4, 5, true);");
    CHECK(uc::emit_insn(p, f, 0x80003104, blr, st) == "return;");
    CHECK(uc::emit_insn(p, f, 0x80003104, mflr(0), st) == "ppc::op_mfspr(c, 0, 8);");
    // illegal word becomes a trap, not an error
    CHECK(uc::emit_insn(p, f, 0x80003104, 0x14000000, st).find("ppc::trap(0x80003104, \"illegal or data word 14000000\")") == 0);
    CHECK(st.invalid == 1);
    // OE-form arithmetic traps and is counted
    CHECK(uc::emit_insn(p, f, 0x80003104, 0x7C642E14, st).find("ppc::trap") == 0);   // addo r3,r4,r5
    CHECK(st.oe_traps == 1);
    // whole function: label at entry, falls off the end into a trap, deterministic text
    std::string a = uc::emit_function(p, f, st), b = uc::emit_function(p, f, st);
    CHECK(a == b);
    CHECK(a.find("void fn_80003100(ppc::Ctx& c) {") == 0);
    CHECK(a.find("L_80003100:;") != std::string::npos);
    CHECK(a.find("fell off the end") != std::string::npos);
    {   // a backward branch polls for pending interrupts; a forward branch does not
        auto lp = make_prog({addi(3, 3, 1), 0x4BFFFFFC /* b -4 */, blr});
        lp.reg_cache = false;
        std::string t = uc::emit_function(lp, lp.an.functions[0], st);
        CHECK(t.find("ppc::poll(c); goto L_80003100;") != std::string::npos);
        lp.reg_cache = true;   // the poll keeps the register copy: interrupts run on their own context, a thread switch keeps the host stack
        t = uc::emit_function(lp, lp.an.functions[0], st);
        CHECK(t.find("if ((++c.poll_count & 0x7FFu) == 0) [[unlikely]] { ppc::poll_slow(ctx); } goto L_80003100;") != std::string::npos);
        auto sda = make_prog({addi(13, 13, 1), addi(3, 3, 1), 0x4BFFFFFC /* b -4 */, blr});   // only the small-data bases are stored there
        t = uc::emit_function(sda, sda.an.functions[0], st);
        CHECK(t.find("if ((++c.poll_count & 0x7FFu) == 0) [[unlikely]] { ctx.r[13] = c.r[13]; ppc::poll_slow(ctx); } goto L_80003104;") != std::string::npos);
        auto fw = make_prog({0x48000008 /* b +8 */, addi(3, 3, 1), blr});
        std::string u = uc::emit_function(fw, fw.an.functions[0], st);
        CHECK(u.find("goto L_80003108;") != std::string::npos && u.find("ppc::poll") == std::string::npos);
    }
    {   // a branch into a secondary entry calls its stub; the owner gets a body with a switch and one stub per entry
        Asm a;
        size_t br = a.emit(0); a.emit(blr);
        uint32_t g = a.here(); a.emit(li(3, 1)); a.emit(li(3, 2)); a.emit(blr);
        a.patch_b(br, g + 4);
        uc::Config cfg; cfg.extra_entries.insert(g);
        auto q = make_prog(a.w, cfg);
        char buf[64];
        std::snprintf(buf, sizeof buf, "fn_%08X_%08X(c); return;", g, g + 4);
        CHECK(uc::emit_insn(q, q.an.functions[0], a.addr(0), a.w[0], st) == buf);
        std::string gtxt = uc::emit_function(q, q.an.functions[1], st);
        std::snprintf(buf, sizeof buf, "case 0x%08Xu: goto L_%08X;", g + 4, g + 4);
        CHECK(gtxt.find(buf) != std::string::npos);
        std::snprintf(buf, sizeof buf, "void fn_%08X_%08X(ppc::Ctx& c) {", g, g + 4);
        CHECK(gtxt.find(buf) != std::string::npos);
        std::snprintf(buf, sizeof buf, "void fn_%08X(ppc::Ctx& c) {", g);
        CHECK(gtxt.find(buf) != std::string::npos);
    }
    {   // a function whose last instruction is not a terminator and which is followed by another entry tail-calls it
        Asm a;
        a.emit(li(3, 1));
        uint32_t g = a.here(); a.emit(addi(3, 3, 41)); a.emit(blr);
        uc::Config cfg; cfg.extra_entries.insert(g);
        auto q = make_prog(a.w, cfg);
        std::string t = uc::emit_function(q, q.an.functions[0], st);
        char buf[64];
        std::snprintf(buf, sizeof buf, "ctx.r[3] = c.r[3]; fn_%08X(ctx); return;", g);
        CHECK(t.find(buf) != std::string::npos);
        CHECK(t.find("fell off") == std::string::npos);
        q.reg_cache = false;
        t = uc::emit_function(q, q.an.functions[0], st);
        std::snprintf(buf, sizeof buf, "  fn_%08X(c); return;", g);
        CHECK(t.find(buf) != std::string::npos);
    }
    {   // a branch target that is inside no function traps at run time and is counted
        Asm a;
        a.emit(enc::b(0x100000)); a.emit(blr);
        auto q = make_prog(a.w);
        uc::EmitStats s2{};
        std::string t = uc::emit_insn(q, q.an.functions[0], a.addr(0), a.w[0], s2);
        CHECK(t.find("not inside any translated function") != std::string::npos);
        CHECK(s2.traps_midfunc == 1);
    }
    {   // register cache: registers are loaded on entry only when read before written, stored before a call or return only when written
        // since the last sync, and reloaded after a call only when read again before the next write
        Asm a;
        a.emit(addi(3, 3, 1)); a.emit(addi(4, 0, 7));
        size_t call = a.emit(0);
        a.emit(addi(3, 3, 2)); a.emit(addi(5, 4, 0)); a.emit(blr);
        uint32_t g = a.here(); a.emit(blr);
        a.patch_bl(call, g);
        auto q = make_prog(a.w);
        std::string t = uc::emit_function(q, q.an.functions[0], st);
        auto has = [&](const std::string& s) { return t.find(s) != std::string::npos; };
        CHECK(has("ppc::Ctx& ctx = c;"));
        CHECK(has("ppc::Ctx c; c.mem = ctx.mem;\n  c.r[3] = ctx.r[3]; \n"));   // r4 and lr are written before any read
        char buf[200];
        std::snprintf(buf, sizeof buf, "c.lr = 0x%08Xu; ctx.r[3] = c.r[3]; ctx.r[4] = c.r[4]; ctx.lr = c.lr; fn_%08X(ctx); c.r[3] = ctx.r[3]; c.r[4] = ctx.r[4]; ",
                      a.addr(call) + 4, g);
        CHECK(has(buf));
        CHECK(has("{ ctx.r[3] = c.r[3]; ctx.r[5] = c.r[5]; return; }"));   // r4 was not written after the call
        q.reg_cache = false;
        t = uc::emit_function(q, q.an.functions[0], st);
        CHECK(!has("ctx"));
    }
    {   // where a path that wrote a register meets one that did not, the edge flushes it: the call after the join stores only what every
        // path wrote, and nothing is reloaded just to be stored again; a conditional call's skipping path flushes in its else branch
        Asm a;
        size_t br = a.emit(0);
        a.emit(addi(4, 4, 1));
        uint32_t skip = a.here();
        size_t call = a.emit(0);
        size_t ccall = a.emit(0);
        a.emit(addi(5, 4, 0)); a.emit(blr);
        uint32_t g = a.here(); a.emit(blr);
        a.patch_bc(br, 12, 2, skip); a.patch_bl(call, g); a.w[ccall] = enc::bc(12, 2, int32_t(g - a.addr(ccall))) | 1;   // beq skip; bl g; beql g
        auto q = make_prog(a.w);
        std::string t = uc::emit_function(q, q.an.functions[0], st);
        auto has = [&](const std::string& s) { return t.find(s) != std::string::npos; };
        CHECK(has("ppc::op_addi(c, 4, 4, 1); } { ctx.r[4] = c.r[4]; }"));
        char buf[200];
        std::snprintf(buf, sizeof buf, "{ c.lr = 0x%08Xu; ctx.lr = c.lr; fn_%08X(ctx); c.r[4] = ctx.r[4]; c.cr[0] = ctx.cr[0]; c.lr = ctx.lr;  }", a.addr(call) + 4, g);
        CHECK(has(buf));
        std::snprintf(buf, sizeof buf, "{ c.lr = 0x%08Xu; ctx.lr = c.lr; fn_%08X(ctx); c.r[4] = ctx.r[4];  } else { ctx.lr = c.lr; } }", a.addr(ccall) + 4, g);
        CHECK(has(buf));
    }
    {   // a call of a closed function syncs only the registers its tree touches: r20 stays dirty across it and is stored at the return (so
        // is the link register: the callee never reads it); the form for a tree with native probes (uc_plain_tree_ false) syncs everything
        Asm a;
        a.emit(addi(3, 3, 1)); a.emit(addi(20, 20, 5));
        size_t call = a.emit(0);
        a.emit(addi(3, 3, 1)); a.emit(blr);
        uint32_t g = a.here(); a.emit(addi(3, 3, 2)); a.emit(blr);
        a.patch_bl(call, g);
        auto q = make_prog(a.w);
        auto emitted = uc::emit_program(q, 1000, st);
        std::string t;
        for (const auto& [name, source] : emitted.files) if (name == "game_000.cpp") t = source;
        char buf[400];
        std::snprintf(buf, sizeof buf, "if constexpr (uc_plain_tree_%08X) { c.lr = 0x%08Xu; ctx.r[3] = c.r[3]; fn_%08X(ctx); c.r[3] = ctx.r[3]; } "
                      "else { c.lr = 0x%08Xu; ctx.r[3] = c.r[3]; ctx.r[20] = c.r[20]; ctx.lr = c.lr; fn_%08X(ctx); c.r[3] = ctx.r[3]; c.r[20] = ctx.r[20]; c.lr = ctx.lr; }",
                      g, a.addr(call) + 4, g, a.addr(call) + 4, g);
        CHECK(t.find(buf) != std::string::npos);
        CHECK(t.find("{ ctx.r[3] = c.r[3]; ctx.r[20] = c.r[20]; ctx.lr = c.lr; return; }") != std::string::npos);
        std::string decl;
        for (const auto& [name, source] : emitted.files) if (name == "game_decl.h") decl = source;
        std::snprintf(buf, sizeof buf, "inline constexpr bool uc_plain_tree_%08X = UC_PLAIN_ENTRY(0x%08Xu);\n", g, g);
        CHECK(decl.find(buf) != std::string::npos);
    }
    {   // closed is transitive and loops are fine: f calls m (a loop, then a call of l); every call syncs only what the callee's tree touches,
        // and m's plain constant also requires l's
        Asm a;
        size_t fc = a.emit(0); a.emit(addi(3, 3, 1)); a.emit(blr);
        uint32_t m = a.here(); a.emit(addi(4, 4, 1)); a.emit(0x4BFFFFFC /* b -4 */); size_t mc = a.emit(0); a.emit(blr);
        uint32_t l = a.here(); a.emit(addi(5, 5, 1)); a.emit(blr);
        a.patch_bl(fc, m); a.patch_bl(mc, l);
        auto q = make_prog(a.w);
        auto emitted = uc::emit_program(q, 1000, st);
        std::string t, decl;
        for (const auto& [name, source] : emitted.files) { if (name == "game_000.cpp") t = source; if (name == "game_decl.h") decl = source; }
        char buf[400];
        std::snprintf(buf, sizeof buf, "if constexpr (uc_plain_tree_%08X) { c.lr = 0x%08Xu; ctx.lr = c.lr; fn_%08X(ctx); } ", m, a.addr(fc) + 4, m);
        CHECK(t.find(buf) != std::string::npos);   // r3 is not in m's tree: it stays in the local copy
        std::snprintf(buf, sizeof buf, "inline constexpr bool uc_plain_tree_%08X = UC_PLAIN_ENTRY(0x%08Xu) && uc_plain_tree_%08X;\n", m, m, l);
        CHECK(decl.find(buf) != std::string::npos);
        char lname[64];
        std::snprintf(lname, sizeof lname, "inline constexpr bool uc_plain_tree_%08X =", l);
        CHECK(decl.find(lname) < decl.find(buf));   // l's constant comes first
    }
    {   // a function that reaches a (non-leaf) HLE is not closed, nor is its caller: both calls sync everything and keep their resume entries
        Asm a;
        size_t fc = a.emit(0); a.emit(blr);
        uint32_t m = a.here(); size_t mc = a.emit(0); a.emit(blr);
        uint32_t h = a.here(); a.emit(blr);
        a.patch_bl(fc, m); a.patch_bl(mc, h);
        uc::Config cfg; cfg.hle[h] = "OSSleepThread";
        auto q = make_prog(a.w, cfg);
        uc::add_resume_entries(q);
        CHECK(q.an.secondary.count(a.addr(0)) && q.an.secondary.at(a.addr(0)).count(a.addr(fc) + 4));
        CHECK(q.an.secondary.count(m) && q.an.secondary.at(m).count(a.addr(mc) + 4));
        auto emitted = uc::emit_program(q, 1000, st);
        std::string t;
        for (const auto& [name, source] : emitted.files) if (name == "game_000.cpp") t = source;
        CHECK(t.find("uc_plain_tree_") == std::string::npos);
    }
    {   // mutually recursive closed functions share one plain constant
        Asm a;
        size_t ac = a.emit(0); a.emit(blr);
        uint32_t b = a.here(); size_t bc = a.emit(0); a.emit(blr);
        a.patch_bl(ac, b); a.patch_bl(bc, a.addr(0));
        auto q = make_prog(a.w);
        auto emitted = uc::emit_program(q, 1000, st);
        std::string decl;
        for (const auto& [name, source] : emitted.files) if (name == "game_decl.h") decl = source;
        char buf[200];
        std::snprintf(buf, sizeof buf, "inline constexpr bool uc_plain_tree_%08X = UC_PLAIN_ENTRY(0x%08Xu) && UC_PLAIN_ENTRY(0x%08Xu);\n", a.addr(0), a.addr(0), b);
        CHECK(decl.find(buf) != std::string::npos);
        CHECK(st.reg_leaves == 2);
    }
    {   // a call of a register save/restore helper becomes its stores in place (no call, no sync); without the cache it stays an HLE call
        Asm a;
        size_t call = a.emit(0); a.emit(blr);
        uint32_t g = a.here(); a.emit(0x93ABFFF4u); a.emit(0x93CBFFF8u); a.emit(0x93EBFFFCu); a.emit(blr);   // stw r29..r31,-12..-4(r11)
        a.patch_bl(call, g);
        uc::Config cfg; cfg.hle[g] = "SaveGpr29";
        auto q = make_prog(a.w, cfg);
        std::string t = uc::emit_function(q, q.an.functions[0], st);
        char buf[200];
        std::snprintf(buf, sizeof buf, "c.lr = 0x%08Xu; ppc::save_gpr<29>(c); ", a.addr(call) + 4);
        CHECK(t.find(buf) != std::string::npos);
        CHECK(t.find("invoke_hle") == std::string::npos);
        CHECK(t.find("c.r[29] = ctx.r[29]; c.r[30] = ctx.r[30]; c.r[31] = ctx.r[31];") != std::string::npos);   // read on entry
        q.reg_cache = false;
        t = uc::emit_function(q, q.an.functions[0], st);
        CHECK(t.find("uc::invoke_hle<hle_SaveGpr29>(c);") != std::string::npos);
    }
    {   // every translated function starts with the trace hook (a no-op macro unless UC_TRACE is defined)
        std::string t = uc::emit_function(p, f, st);
        CHECK(t.find("UC_ENTER(0x80003100u);") != std::string::npos);
        CHECK(t.find("UC_ENTER") < t.find("L_80003100:;"));
    }
    {   // compare fusion (plan_function); tests/e2e cmpfuse runs every form against the unfused translation
        auto emit_cr = [&](const std::vector<uint32_t>& code) {
            auto q = make_prog(code);
            const uc::CrLive live = uc::analyze_cr(q);
            return uc::emit_function(q, q.an.functions[0], st, &live);
        };
        auto has = [](const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; };
        const uint32_t fcmpu = 0xFC011000u, cror_le = 0x4C401382u, cror_ge = 0x4C411382u, bne = 0x4082000Cu, beq = 0x4182000Cu;
        // fcmp + cror (LT|EQ or GT|EQ) + branch on EQ: one comparison when the branch is the field's last reader; the branch reads the operands
        // from the register copy, so they are loaded at the entry
        std::string t = emit_cr({fcmpu, cror_le, bne, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "if (!((c.f[1] <= c.f[2])))") && !has(t, "op_fcmpu") && !has(t, "op_cror"));
        CHECK(has(t, "c.f[1] = ctx.f[1]; c.f[2] = ctx.f[2];"));
        t = emit_cr({fcmpu, cror_ge, beq, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "if ((c.f[1] >= c.f[2]))"));
        t = emit_cr({fcmpu, cror_le, bne, beq, li(3, 1), blr, li(3, 2), blr});   // a second reader of the field keeps the CR write
        CHECK(has(t, "op_fcmpu") && has(t, "op_cror"));
        t = emit_cr({fcmpu, 0x4180000Cu /* blt */, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "if ((c.f[1] < c.f[2]))") && !has(t, "op_fcmpu"));
        t = emit_cr({fcmpu, 0x4183000Cu /* bso: unordered */, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "c.f[1] != c.f[1] || c.f[2] != c.f[2]") && !has(t, "xer_so"));
        // fcmp, cror, mfcr, rlwinm.: one bit of the compare as 0/1, and the branch on that register
        const uint32_t fcmpo = 0xFC010040u, mfcr0 = 0x7C000026u, rlwinm_dot = 0x54001FFFu;
        t = emit_cr({fcmpo, cror_ge, mfcr0, rlwinm_dot, bne, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "c.r[0] = uint32_t(c.f[1] >= c.f[0]); }") && has(t, "!((int32_t(c.r[0]) == 0))"));
        CHECK(!has(t, "op_mfcr") && !has(t, "op_rlwinm") && !has(t, "cr0("));
        t = emit_cr({fcmpo, cror_ge, mfcr0, rlwinm_dot, 0x40820010u /* bne +16 */, beq, li(3, 1), blr, li(3, 2), blr});   // cr0 read twice
        CHECK(has(t, "c.r[0] = uint32_t(c.f[1] >= c.f[0]); ppc::cr0(c, c.r[0]);"));
        // mfcr + rlwinm reads only the fields under the mask: the cr1 compare is dead
        t = emit_cr({0x2C830000u /* cmpwi cr1,r3,0 */, mfcr0, 0x54001FFEu /* rlwinm r0,r0,3,31,31 */, blr});
        CHECK(has(t, "cr1 is never read") && has(t, "op_mfcr"));
        // an integer record form whose only reader is the next branch: the branch tests the result register
        t = emit_cr({0x5460063Fu /* rlwinm. r0,r3,0,24,31 */, beq, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "ppc::op_rlwinm(c, 3, 0, 0, 24, 31, false);") && has(t, "if ((int32_t(c.r[0]) == 0))"));
        t = emit_cr({0x70600001u /* andi. r0,r3,1 */, beq, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "c.r[0] = c.r[3] & 1u;") && !has(t, "op_andi_"));
        // a compare whose branch comes a few instructions later keeps its operands in locals
        const uint32_t cmpwi0 = 0x2C030000u /* cmpwi r3,0 */, stw0 = 0x90850000u /* stw r4,0(r5) */;
        t = emit_cr({cmpwi0, stw0, beq, li(3, 1), blr, li(3, 2), blr});
        CHECK(has(t, "uint32_t cf0a;") && has(t, "cf0a = c.r[3];") && has(t, "(int32_t(cf0a) == 0)") && !has(t, "op_cmpi"));
        t = emit_cr({cmpwi0, stw0, 0x40820010u /* bne +16 */, beq, li(3, 1), blr, li(3, 2), blr});   // a second reader of cr0
        CHECK(has(t, "op_cmpi") && !has(t, "cf0a"));
        t = emit_cr({cmpwi0, stw0, 0x4183000Cu /* bso */, li(3, 1), blr, li(3, 2), blr});   // SO is XER[SO] at the compare: not delayed
        CHECK(has(t, "op_cmpi") && !has(t, "cf0a"));
        t = emit_cr({cmpwi0, stw0, stw0, stw0, stw0, stw0, stw0, stw0, stw0, beq, li(3, 1), blr, li(3, 2), blr});   // 8 between: still
        CHECK(has(t, "cf0a"));
        t = emit_cr({cmpwi0, stw0, stw0, stw0, stw0, stw0, stw0, stw0, stw0, stw0, beq, li(3, 1), blr, li(3, 2), blr});   // 9: too far
        CHECK(has(t, "op_cmpi") && !has(t, "cf0a"));
    }
    {   // store groups: plain stores through one base (not r1) with only register operations and loads between them mark their pages once
        auto has = [](const std::string& t, const std::string& s) { return t.find(s) != std::string::npos; };
        auto emit = [&](const std::vector<uint32_t>& code) { auto q = make_prog(code); return uc::emit_function(q, q.an.functions[0], st); };
        const size_t groups = st.store_groups;
        std::string t = emit({stw(4, 0, 5), lwz(6, 8, 7), addi(8, 6, 1), stfs(1, 8, 5), stw(6, 4, 5), blr});
        CHECK(has(t, "ppc::note_stores(c, 5, 0, 12u); ppc::op_stw<false, true>(c, 4, 5, 0);") && has(t, "ppc::op_stfs<false, true>(c, 1, 5, 8);"));
        CHECK(has(t, "ppc::op_stw<false, true>(c, 6, 5, 4);") && st.store_groups == groups + 1);
        t = emit({stw(4, 0, 5), stw(6, 0, 7), stw(6, 4, 5), blr});   // another store between: no group
        CHECK(!has(t, "note_stores") && !has(t, "<false, true>"));
        t = emit({stw(4, 0, 5), addi(5, 5, 4), stw(6, 4, 5), blr});   // the base changes
        CHECK(!has(t, "note_stores"));
        t = emit({stw(4, 0, 5), stw(4, 0x1000, 5), blr});   // more than 4096 bytes
        CHECK(!has(t, "note_stores"));
        t = emit({stw(4, 0, 1), stw(4, 4, 1), blr});   // r1: frame notes instead
        CHECK(!has(t, "note_stores") && has(t, "frame_note"));
        t = emit({stw(4, -16, 13), stw(4, 4, 13), blr});
        CHECK(has(t, "ppc::note_stores(c, 13, -16, 24u);"));
    }
    {   // Resume after calls that can reach a parked SDK call (here the indirect one) without splitting the original function; a call of a
        // closed function (the direct one) cannot park, so it gets no resume entry.
        Asm a;
        const auto direct=a.emit(0);
        a.emit(0x4E800421u); // bctrl
        a.emit(addi(3,3,1));
        a.emit(blr);
        const auto callee=a.here(); a.emit(blr);
        a.patch_b(direct,callee); a.w[direct]|=1;
        auto q=make_prog(a.w);
        const auto functions=q.an.functions.size();
        uc::add_resume_entries(q);
        CHECK(q.an.functions.size()==functions);
        CHECK(q.an.secondary.at(a.addr(0)).count(a.addr(1))==0);
        CHECK(q.an.secondary.at(a.addr(0)).count(a.addr(2))==1);
        CHECK(q.an.secondary.at(a.addr(0)).count(a.addr(3))==0);
        const auto before=q.an.secondary;
        uc::add_resume_entries(q);
        CHECK(q.an.secondary==before);
        auto emitted=uc::emit_program(q,150,st);
        bool registered=false;
        for (const auto& [name,source]:emitted.files)
            if(name=="game_table.cpp") registered=source.find("{0x80003108u, fn_80003100_80003108}")!=std::string::npos &&
                                                  source.find("fn_80003100_80003104")==std::string::npos;
        CHECK(registered);
    }
    {   // a FIFO run: the FIFO stores of one block with only register operations and plain loads between them are decided once at the first
        // store (all their bytes), each appends in place when taken; another store ends the run, and a lone FIFO store keeps its store
        const uint32_t stb_r0 = (38u << 26) | (0u << 21) | (3u << 16) | 0x8000u;   // stb r0,-0x8000(r3)
        auto q = make_prog({lis(3, -13311), li(0, 16), stb_r0, lwz(0, 168, 31), stw(0, -32768, 3), stfs(1, -32768, 3),
                            stw(4, 0, 5), stw(0, -32768, 3), blr});
        uc::EmitStats s2{};
        const std::string t = uc::emit_function(q, q.an.functions[0], s2);
        auto has = [&](const char* text) { return t.find(text) != std::string::npos; };
        CHECK(has("  bool fifo_ = false;\n"));
        CHECK(has("{ fifo_ = uc::gx_fifo_burst(c.r[3] + uint32_t(-32768), 9u, 3u); if (fifo_) uc::gx_fifo_put(1, c.r[0]); else ppc::op_stb<true>(c, 0, 3, -32768); }"));
        CHECK(has("{ if (fifo_) uc::gx_fifo_put(4, c.r[0]); else ppc::op_stw<true>(c, 0, 3, -32768); }"));
        CHECK(has("{ if (fifo_) uc::gx_fifo_put(4, ppc::single_bits(c.f[1])); else ppc::op_stfs<true>(c, 1, 3, -32768); }"));
        CHECK(has("/*8000311C*/ { ppc::op_stw<true>(c, 0, 3, -32768); }"));
        CHECK(s2.fifo_runs == 1 && s2.fifo_run_stores == 3);
    }
    {   // loads/stores from a hardware-register base keep the explicit MMIO test (op_*<true>): lis r3,0xCC01 reaches the stores of a loop
        // whose poll reloads r3 (the compiler cannot follow it), through mr and an update form; a RAM base, a base the loop overwrote and
        // the volatile registers after a call do not
        auto q = make_prog({lis(3, -13311), mr(5, 3), mtctr(4),
                            stw(4, -32768, 3), stfs(1, -32768, 5), stwu(6, 4, 5), lwz(7, 0, 5), lwz(8, 0, 9), bdnz(-20),
                            lis(9, 0x8000), stw(4, 0, 9), lis(10, -13312), bl(20), stw(4, 0, 10), lis(3, -13311), stw(4, 0x1000, 3), blr,
                            blr});
        const size_t before = st.mmio_sites;
        const std::string t = uc::emit_function(q, q.an.functions[0], st);
        auto has = [&](const char* text) { return t.find(text) != std::string::npos; };
        CHECK(has("ppc::op_stw<true>(c, 4, 3, -32768);") && has("ppc::op_stfs<true>(c, 1, 5, -32768);"));
        CHECK(has("ppc::op_stwu<true>(c, 6, 5, 4);") && has("ppc::op_lwz<true>(c, 7, 5, 0);"));
        CHECK(has("ppc::op_lwz(c, 8, 9, 0);") && has("ppc::op_stw(c, 4, 9, 0);"));   // r9: never an MMIO base, then RAM
        CHECK(has("ppc::op_stw(c, 4, 10, 0);"));                                         // r10 is volatile across the call
        CHECK(has("ppc::op_stw<true>(c, 4, 3, 4096);"));
        CHECK(st.mmio_sites - before == 5);
    }
    {   // frame notes: the r1-based D-form stores skip their barrier (op_*<false, true>) and the function marks [r1 + lo, r1 + hi) at its entry,
        // after every call and every write of r1, and in its poll's slow path, where a framed store can follow; stwu keeps its own barrier
        auto q = make_prog({stwu(1, -32, 1), mflr(0), stw(0, 36, 1), stw(31, 28, 1), psq_st(1, -8, 1, 0, 0), bl(32), stfs(1, 8, 1), mtctr(4),
                            addi(3, 3, 1), stw(3, 12, 1), bdnz(-8), addi(1, 1, 32), blr, blr});
        uc::EmitStats s3{};
        const std::string t = uc::emit_function(q, q.an.functions[0], s3);
        auto has = [&](const char* text) { return t.find(text) != std::string::npos; };
        auto count = [&](const char* text) { size_t k = 0; for (size_t at = t.find(text); at != std::string::npos; at = t.find(text, at + 1)) ++k; return k; };
        CHECK(has("ppc::op_stw<false, true>(c, 0, 1, 36);") && has("ppc::op_stw<false, true>(c, 31, 1, 28);"));
        CHECK(has("ppc::op_psq_st<false, true>(c, 1, 1, -8, 0, 0);") && has("ppc::op_stfs<false, true>(c, 1, 1, 8);"));
        CHECK(has("ppc::op_stw<false, true>(c, 3, 1, 12);"));
        CHECK(has("/*80003100*/ { ppc::op_stwu(c, 1, 1, -32); }"));   // stwu keeps its barrier and writes r1: no framed store before it
        CHECK(has("/*80003104*/ { ppc::frame_note(c, -8, 40); "));   // after the write of r1
        CHECK(has("/*80003118*/ { ppc::frame_note(c, -8, 40); ppc::op_stfs<false, true>"));         // after the call
        CHECK(has("ppc::poll_slow(ctx); ppc::frame_note(c, -8, 40); }"));                           // the poll's slow path
        const size_t epilogue = t.find("/*80003130*/");   // after addi r1 (the epilogue): no framed store follows, no mark
        CHECK(epilogue != std::string::npos && t.substr(epilogue, t.find('\n', epilogue) - epilogue).find("frame_note") == std::string::npos);
        CHECK(count("frame_note") == 3 && s3.frame_notes == 2 && s3.framed_stores == 5);
    }
    {   // stmw from r1 is framed too: one framed store per register, and the range covers rS..r31
        auto q = make_prog({stwu(1, -64, 1), D(47, 29, 1, 40), blr});   // stmw r29,40(r1)
        uc::EmitStats s4{};
        const std::string t = uc::emit_function(q, q.an.functions[0], s4);
        auto has = [&](const char* text) { return t.find(text) != std::string::npos; };
        CHECK(has("ppc::frame_note(c, 40, 52); const uint32_t e_ = c.r[1] + uint32_t(40); ppc::wr32_framed(c, e_ + 0u, c.r[29]);"));
        CHECK(has("ppc::wr32_framed(c, e_ + 8u, c.r[31]);") && !has("ppc::wr32(c, e_"));
    }
    return 0;
}
