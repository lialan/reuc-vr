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
        lp.reg_cache = true;   // the poll's slow path syncs the register cache with the real context around the safe point
        t = uc::emit_function(lp, lp.an.functions[0], st);
        CHECK(t.find("if ((++c.poll_count & 0x7FFu) == 0) [[unlikely]] { ctx.r[3] = c.r[3]; ctx.poll_count = c.poll_count; ppc::poll_slow(ctx); "
                     "c.r[3] = ctx.r[3]; c.poll_count = ctx.poll_count; } goto L_80003100;") != std::string::npos);        auto fw = make_prog({0x48000008 /* b +8 */, addi(3, 3, 1), blr});
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
    {   // a call of a register save/restore helper becomes its stores in place (no call, no sync); without the cache it stays an HLE call
        Asm a;
        size_t call = a.emit(0); a.emit(blr);
        uint32_t g = a.here(); a.emit(0x93ABFFF4u); a.emit(0x93CBFFF8u); a.emit(0x93EBFFFCu); a.emit(blr);   // stw r29..r31,-12..-4(r11)
        a.patch_bl(call, g);
        uc::Config cfg; cfg.hle[g] = "SaveGpr29";
        auto q = make_prog(a.w, cfg);
        std::string t = uc::emit_function(q, q.an.functions[0], st);
        char buf[200];
        std::snprintf(buf, sizeof buf, "c.lr = 0x%08Xu; ppc::op_stw(c, 29, 11, -12); ppc::op_stw(c, 30, 11, -8); ppc::op_stw(c, 31, 11, -4); ", a.addr(call) + 4);
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
    {   // Resume after direct/indirect calls without splitting the original function.
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
        CHECK(q.an.secondary.at(a.addr(0)).count(a.addr(1))==1);
        CHECK(q.an.secondary.at(a.addr(0)).count(a.addr(2))==1);
        CHECK(q.an.secondary.at(a.addr(0)).count(a.addr(3))==0);
        const auto before=q.an.secondary;
        uc::add_resume_entries(q);
        CHECK(q.an.secondary==before);
        auto emitted=uc::emit_program(q,150,st);
        bool registered=false;
        for (const auto& [name,source]:emitted.files)
            if(name=="game_table.cpp") registered=source.find("{0x80003104u, fn_80003100_80003104}")!=std::string::npos;
        CHECK(registered);
    }
    return 0;
}
