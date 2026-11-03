#include "translator/emit.h"
#include "translator/regeffect.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <algorithm>
#include <cstdio>
#include <set>
#include <sstream>

namespace uc {

static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
static std::string num(long long v) { return std::to_string(v); }
static std::string fn_name(uint32_t a) { return "fn_" + hex8(a); }
static std::string sec_name(uint32_t owner, uint32_t t) { return "fn_" + hex8(owner) + "_" + hex8(t); }

void add_resume_entries(Program& p) {
    for (const auto& f:p.an.functions) {
        if(p.cfg.hle.count(f.entry)) continue;
        for(uint32_t pc=f.entry;pc+4<f.end;pc+=4) {
            uint32_t word=0;
            if(!p.img.word(pc,word) || !ppc::lk(word)) continue;
            const auto* op=ppc::decode(word);
            if(op && (op->kind==ppc::Kind::BR || op->kind==ppc::Kind::BC ||
                      op->kind==ppc::Kind::BCLR || op->kind==ppc::Kind::BCCTR))
                p.an.secondary[f.entry].insert(pc+4);
        }
    }
}

static std::string callee(const Program& p, uint32_t target) {
    auto it = p.cfg.hle.find(target);
    return it != p.cfg.hle.end() ? "uc::invoke_hle<hle_" + it->second + ">" : fn_name(target);
}

static std::string trap_str(uint32_t pc, const std::string& what) { return "ppc::trap(0x" + hex8(pc) + ", \"" + what + "\");"; }

static std::string op_args(const ppc::OpInfo& op, uint32_t w) {
    using ppc::Kind;
    std::ostringstream o;
    auto n = [](auto v) { return std::to_string(static_cast<long long>(v)); };
    switch (op.kind) {
        case Kind::NONE: break;
        case Kind::RRR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::RRS: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::simm(w)); break;
        case Kind::RRU: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::uimm(w)) << "u"; break;
        case Kind::RR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)); break;
        case Kind::AB: o << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::RB: o << n(ppc::t(w)) << ", " << n(ppc::b(w)); break;
        case Kind::R: o << n(ppc::t(w)); break;
        case Kind::CRF: o << n(ppc::crfd(w)); break;
        case Kind::RRM: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::sh(w)) << ", " << n(ppc::mb(w)) << ", " << n(ppc::me(w)); break;
        case Kind::RRRM: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n(ppc::mb(w)) << ", " << n(ppc::me(w)); break;
        case Kind::CMP: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::CMPS: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::simm(w)); break;
        case Kind::CMPU: o << n(ppc::crfd(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::uimm(w)) << "u"; break;
        case Kind::CRB: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)); break;
        case Kind::MCRF:
            if (std::string(op.name) == "mtfsfi") o << n(ppc::crfd(w)) << ", " << n((w >> 12) & 0xF);
            else o << n(ppc::crfd(w)) << ", " << n(ppc::crfs(w));
            break;
        case Kind::SPR: o << n(ppc::t(w)) << ", " << n(ppc::spr(w)); break;   // mfspr, mtspr, mftb share the swapped 10-bit field
        case Kind::MTCRF:
            if (std::string(op.name) == "mtfsf") o << n((w >> 17) & 0xFF) << ", " << n(ppc::b(w));
            else o << n(ppc::crm(w)) << ", " << n(ppc::t(w));
            break;
        case Kind::RRRR: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n(ppc::c(w)); break;
        case Kind::RAC: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::c(w)); break;
        case Kind::PSQ: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(int32_t(w << 20) >> 20) << ", " << n((w >> 15) & 1) << ", " << n((w >> 12) & 7); break;
        case Kind::PSQX: o << n(ppc::t(w)) << ", " << n(ppc::a(w)) << ", " << n(ppc::b(w)) << ", " << n((w >> 10) & 1) << ", " << n((w >> 7) & 7); break;
        default: break;
    }
    return o.str();
}

// ---- control flow ----
struct Target { enum Class { Internal, Entry, Secondary, Outside } kind; uint32_t owner; };
static Target classify(const Program& p, const Function& f, uint32_t t) {
    if (t >= f.entry && t < f.end) return {Target::Internal, 0};
    if (p.an.is_entry(t)) return {Target::Entry, 0};
    if (const Function* g = p.an.find_containing(t)) {
        auto it = p.an.secondary.find(g->entry);
        if (!p.cfg.hle.count(g->entry) && it != p.an.secondary.end() && it->second.count(t)) return {Target::Secondary, g->entry};
    }
    return {Target::Outside, 0};
}

static std::string cond_expr(uint32_t bo, uint32_t bi, std::string& pre) {
    std::string e;
    if (!(bo & 4)) {
        pre += "--c.ctr; ";
        e = (bo & 2) ? "c.ctr == 0" : "c.ctr != 0";
    }
    if (!(bo & 16)) {
        std::string cr = "ppc::crbit(c, " + num(bi) + ") == " + ((bo & 8) ? "true" : "false");
        e = e.empty() ? cr : e + " && " + cr;
    }
    return e.empty() ? "true" : e;
}

// Sync code of one instruction in register-cache mode (CrHints::local); empty strings and the context `c` otherwise.
struct SyncText {
    std::string cx = "c", flush, reload, not_taken;
    const std::map<uint32_t, std::string>* edges = nullptr;
    const CrHints* h = nullptr;
    explicit SyncText(const CrHints* hints) {
        if (hints && hints->local) {
            cx = "ctx"; h = hints; flush = h->flush; reload = h->reload + h->after_call; not_taken = h->not_taken; edges = &h->edge;
        }
    }
    bool local() const { return cx == "ctx"; }
    // the else branch of a conditional call: what the path that skips the call flushes (code after the call would also run after it)
    std::string otherwise(bool lk) const { return lk && !not_taken.empty() ? " else { " + not_taken + "}" : ""; }
    std::string edge(uint32_t target) const {   // a forward jump to an internal label
        if (!edges) return "";
        auto it = edges->find(target);
        return it == edges->end() ? "" : it->second;
    }
};

// The SDK's register save/restore helpers (_savegpr_N, _restgpr_N, _savefpr_N, _restfpr_N; HLE entries SaveGprN ... RestFprN, see
// runtime/hle/sdk_regs.cpp): a call of one does what the helper does (ppc::save_gpr<N> ..., ppc_rt.h), so in register-cache mode the translator
// writes them in place of the call; no sync of the register copy around every prologue and epilogue.
struct RegHelper { char kind = 0; uint32_t first = 0; };   // 'S' SaveGpr, 'R' RestGpr, 's' SaveFpr, 'r' RestFpr
static RegHelper reg_helper(const Program& p, uint32_t target) {
    auto it = p.cfg.hle.find(target);
    if (it == p.cfg.hle.end() || it->second.size() < 8) return {};
    const std::string& n = it->second;
    const std::string head = n.substr(0, 7);
    const char kind = head == "SaveGpr" ? 'S' : head == "RestGpr" ? 'R' : head == "SaveFpr" ? 's' : head == "RestFpr" ? 'r' : 0;
    if (!kind || n.find_first_not_of("0123456789", 7) != std::string::npos) return {};
    const uint32_t first = uint32_t(std::stoul(n.substr(7)));
    return first >= 14 && first <= 31 ? RegHelper{kind, first} : RegHelper{};
}
static std::string reg_helper_text(const RegHelper& h) {   // the helper's own code (ppc_rt.h), in the local register copy
    const char* fn = h.kind == 'S' ? "save_gpr" : h.kind == 'R' ? "rest_gpr" : h.kind == 's' ? "save_fpr" : "rest_fpr";
    return std::string("ppc::") + fn + "<" + num(h.first) + ">(c); ";
}
static RegEffect reg_helper_effect(const RegHelper& h) {
    RegEffect e;
    e.use.set(size_t(res::R + 11));
    for (uint32_t i = h.first; i < 32; ++i) {
        const size_t r = size_t((h.kind == 'S' || h.kind == 'R' ? res::R : res::F) + int(i));
        if (h.kind == 'S' || h.kind == 's') e.use.set(r); else { e.may.set(r); e.must.set(r); }
    }
    return e;
}

static std::string branch_action(const Program& p, const Function& f, uint32_t pc, uint32_t target, bool lk, EmitStats& st, const SyncText& s) {
    Target tk = classify(p, f, target);
    if (s.local() && tk.kind == Target::Entry) {
        if (const RegHelper h = reg_helper(p, target); h.kind) {
            if (lk) return "c.lr = 0x" + hex8(pc + 4) + "u; " + reg_helper_text(h);
            return reg_helper_text(h) + s.flush + "return;";
        }
    }
    if (tk.kind == Target::Internal && !lk) {
        if (target > pc) return s.edge(target) + "goto L_" + hex8(target) + ";";
        // a loop is a safe point; the interrupts that may run there see the registers in the real context
        if (s.local()) return "if ((++c.poll_count & 0x7FFu) == 0) [[unlikely]] { " + s.flush + "ppc::poll_slow(ctx); " + s.reload + "} goto L_" + hex8(target) + ";";
        return "ppc::poll(c); goto L_" + hex8(target) + ";";
    }
    if (tk.kind == Target::Outside || (tk.kind == Target::Internal && lk && target != f.entry)) {
        ++st.traps_midfunc;
        return s.flush + trap_str(pc, "branch target " + hex8(target) + " is not inside any translated function");
    }
    std::string fn = tk.kind == Target::Secondary ? sec_name(tk.owner, target) : callee(p, target);
    if (lk && s.h && s.h->leaf_call) {   // partial sync unless native code runs at the callee's entry
        const std::string lr = "c.lr = 0x" + hex8(pc + 4) + "u; ";
        return "if constexpr (UC_PLAIN_ENTRY(0x" + hex8(target) + "u)) { " + lr + s.flush + fn + "(ctx); " + s.reload + "} else { " + lr + s.h->full_flush +
               fn + "(ctx); " + s.h->full_reload + s.h->after_call + "}";
    }
    if (lk) return "c.lr = 0x" + hex8(pc + 4) + "u; " + s.flush + fn + "(" + s.cx + ");" + (s.reload.empty() ? "" : " " + s.reload);
    return s.flush + fn + "(" + s.cx + "); return;";
}

// Branch condition from a compare that was not written to CR: bit 0..3 of BI = LT GT EQ SO of that compare.
static std::string fused_cond(const FusedCmp& cmp, uint32_t bo, uint32_t bi) {
    const unsigned bit = bi & 3;
    std::string e = bit == 3 ? "c.xer_so != 0" : "(" + cmp.lhs + (bit == 0 ? " < " : bit == 1 ? " > " : " == ") + cmp.rhs + ")";
    return (bo & 8) ? e : "!(" + e + ")";
}

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t w, EmitStats& st, const CrHints* hints) {
    ++st.instructions;
    const SyncText s(hints);
    const ppc::OpInfo* op = ppc::decode(w);
    if (!op) {
        ++st.invalid;
        return s.flush + trap_str(pc, "illegal or data word " + hex8(w));
    }
    using ppc::Kind;
    std::string name = op->name;
    if ((name == "twi" || name == "tw") && ppc::t(w) == 31) return s.flush + trap_str(pc, "unconditional trap");
    if (op->sub == ppc::Sub::X9 && ppc::oe(w)) { ++st.oe_traps; return s.flush + trap_str(pc, "OE (overflow-enabled) form of " + name + " is not implemented"); }
    if (op->kind == Kind::UNSUP) { ++st.unsupported; return s.flush + trap_str(pc, "unsupported instruction " + name); }

    switch (op->kind) {
        case Kind::SC: return s.flush + "uc::hle_sc(" + s.cx + ", 0x" + hex8(pc) + "u);" + (s.reload.empty() ? "" : " " + s.reload);
        case Kind::BR: {
            int32_t li = ppc::li(w);
            uint32_t target = ppc::aa(w) ? uint32_t(li) : pc + uint32_t(li);
            return branch_action(p, f, pc, target, ppc::lk(w), st, s);
        }
        case Kind::BC: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            uint32_t target = ppc::aa(w) ? uint32_t(ppc::bd(w)) : pc + uint32_t(ppc::bd(w));
            std::string pre;
            std::string cond = hints && hints->fused ? fused_cond(*hints->fused, bo, bi) : cond_expr(bo, bi, pre);
            return "{ " + pre + "if (" + cond + ") { " + branch_action(p, f, pc, target, ppc::lk(w), st, s) + " }" + s.otherwise(ppc::lk(w)) + " }";
        }
        case Kind::BCLR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = hints && hints->fused ? fused_cond(*hints->fused, bo, bi) : cond_expr(bo, bi, pre);
            std::string act = ppc::lk(w) ? "{ uint32_t t_ = c.lr; c.lr = 0x" + hex8(pc + 4) + "u; " + s.flush + "uc::call_indirect(" + s.cx + ", t_); " + s.reload + "}"
                                         : s.flush + "return;";
            if (cond == "true" && pre.empty()) return act;
            return "{ " + pre + "if (" + cond + ") { " + act + " }" + s.otherwise(ppc::lk(w)) + " }";
        }
        case Kind::BCCTR: {
            uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
            std::string pre;
            std::string cond = cond_expr(bo | 4, bi, pre);   // bcctr never decrements CTR
            if (ppc::lk(w)) {
                std::string act = "c.lr = 0x" + hex8(pc + 4) + "u; " + s.flush + "uc::call_indirect(" + s.cx + ", c.ctr);" + (s.reload.empty() ? "" : " " + s.reload);
                return cond == "true" ? act : "if (" + cond + ") { " + act + " }" + s.otherwise(true);
            }
            for (const auto& jt : p.an.jump_tables) {
                if (jt.bctr_addr != pc) continue;
                std::set<uint32_t> seen;
                std::string sw = "switch (c.ctr) { ";
                for (uint32_t t : jt.targets) if (seen.insert(t).second) sw += "case 0x" + hex8(t) + "u: " + s.edge(t) + "goto L_" + hex8(t) + "; ";
                sw += "default: " + s.flush + trap_str(pc, "jump table index out of range") + " }";
                return sw;
            }
            if (std::find(p.an.unresolved_bctr.begin(), p.an.unresolved_bctr.end(), pc) != p.an.unresolved_bctr.end()) ++st.unresolved_bctr;
            else ++st.indirect_tail;
            std::string act = s.flush + "uc::call_indirect(" + s.cx + ", c.ctr); return;";
            return cond == "true" ? "{ " + act + " }" : "if (" + cond + ") { " + act + " }";
        }
        default: break;
    }
    if (s.local()) {
        // State the register cache does not hold lives in the real context; lmw/stmw become one access per register so every register index
        // stays a compile-time constant (the local copy is then kept in host registers).
        const uint32_t T = ppc::t(w), spr = ppc::spr(w);
        const bool cached_spr = spr == 1 || spr == 8 || spr == 9 || (spr >= 912 && spr <= 919);
        if (name == "mfspr" && !cached_spr) return "c.r[" + num(T) + "] = ctx.spr[" + num(spr & 1023) + "];";
        if (name == "mtspr" && !cached_spr) return "ctx.spr[" + num(spr & 1023) + "] = c.r[" + num(T) + "];";
        if (name == "mfmsr") return "c.r[" + num(T) + "] = ctx.msr;";
        if (name == "mtmsr") return "ctx.msr = c.r[" + num(T) + "];";
        if (name == "lmw" || name == "stmw") {
            const uint32_t A = ppc::a(w);
            std::string o = "const uint32_t e_ = " + (A ? "c.r[" + num(A) + "] + " : std::string()) + "uint32_t(" + num(ppc::simm(w)) + ");";
            for (uint32_t i = T; i < 32; ++i) {
                const std::string at = "e_ + " + num(4 * (i - T)) + "u";
                o += name == "lmw" ? " c.r[" + num(i) + "] = ppc::rd32(c, " + at + ");" : " ppc::wr32(c, " + at + ", c.r[" + num(i) + "]);";
            }
            return o;
        }
    }
    std::string a = op_args(*op, w);
    if (op->rc) a += (a.empty() ? "" : ", ") + std::string(ppc::rc(w) && !(hints && hints->drop_rc) ? "true" : "false");
    return "ppc::op_" + name + (hints && hints->io ? "<true>" : "") + "(c" + (a.empty() ? "" : ", " + a) + ");";
}

static bool is_terminator_word(uint32_t w) { return w == 0x4E800020u || w == 0x4E800420u || ((w >> 26) == 18 && !(w & 1)); }

// ---- condition-register liveness ----
// A CR field write is dead when no path reads the field before another write replaces it. Backward dataflow over each function's
// instruction graph; what callers read after a return and what callees read on entry come from whole-program summaries, so no calling
// convention is assumed: a field is kept as long as any call site reads it after the call.
namespace {
constexpr uint8_t kAllCr = 0xFF;
uint8_t fld(uint32_t f) { return uint8_t(1u << (f & 7)); }
bool fp_name(const std::string& n) { return n[0] == 'f' || n == "mffs" || n.compare(0, 3, "ps_") == 0 || n.compare(0, 4, "mtfs") == 0; }

struct CrEffect { uint8_t use = 0, def = 0; };   // fields read, fields fully overwritten
CrEffect cr_effect(const ppc::OpInfo& op, uint32_t w) {
    using ppc::Kind;
    CrEffect e;
    const std::string n = op.name;
    switch (op.kind) {
        case Kind::CMP: case Kind::CMPS: case Kind::CMPU: case Kind::CRF: e.def = fld(ppc::crfd(w)); return e;   // cmp*, fcmp*, ps_cmp*, mcrxr
        case Kind::MCRF:
            if (n == "mtfsfi") { if (ppc::rc(w)) e.def = fld(1); }
            else if (n == "mcrf") { e.use = fld(ppc::crfs(w)); e.def = fld(ppc::crfd(w)); }
            else e.def = fld(ppc::crfd(w));   // mcrfs
            return e;
        case Kind::CRB: e.use = uint8_t(fld(ppc::t(w) >> 2) | fld(ppc::a(w) >> 2) | fld(ppc::b(w) >> 2)); return e;   // the target field is read-modify-write
        case Kind::MTCRF:
            if (n == "mtcrf") { for (unsigned i = 0; i < 8; ++i) if (ppc::crm(w) & (0x80u >> i)) e.def |= fld(i); }
            else if (ppc::rc(w)) e.def = fld(1);   // mtfsf
            return e;
        default: break;
    }
    if (n == "mfcr") { e.use = kAllCr; return e; }
    if (op.rc && ppc::rc(w)) e.def |= fld(fp_name(n) ? 1 : 0);          // record forms: cr0 for integer ops, cr1 for floating point
    else if (!op.rc && n.back() == '_') e.def |= fld(0);                // andi. addic. stwcx. ... always record into cr0
    return e;
}

struct Callee {
    enum Kind : uint8_t { None, Direct, Hle, Indirect } kind = None;
    uint32_t entry = 0;   // address whose entry liveness the call reads
    uint32_t fn = 0;      // function whose return the call joins
};
Callee resolve_callee(const Program& p, const Function& f, uint32_t target) {
    if (p.cfg.hle.count(target)) return {Callee::Hle, target, target};
    const Target tk = classify(p, f, target);
    return {Callee::Direct, target, tk.kind == Target::Secondary ? tk.owner : target};
}

struct CrNode {
    uint8_t use = 0, def = 0;
    bool fall = true;                  // execution can continue at the next instruction (also after a call returns)
    enum Taken : uint8_t { NoJump, Internal, Return, Tail, IndirectTail } taken = NoJump;
    int target = -1;                   // Internal: instruction index
    Callee call;                       // a call made by the instruction
    Callee tail;                       // Tail: the function it jumps to
    std::vector<int> table;            // jump table targets
};

std::vector<CrNode> cr_nodes(const Program& p, const Function& f) {
    using ppc::Kind;
    const size_t n = (f.end - f.entry) / 4;
    std::vector<CrNode> nodes(n);
    auto index = [&](uint32_t t) { return int((t - f.entry) / 4); };
    for (size_t i = 0; i < n; ++i) {
        const uint32_t pc = f.entry + uint32_t(i) * 4;
        uint32_t w = 0;
        CrNode& nd = nodes[i];
        const ppc::OpInfo* op = p.img.word(pc, w) ? ppc::decode(w) : nullptr;
        if (!op) { nd.fall = false; continue; }   // emitted as a trap
        const std::string name = op->name;
        if (((name == "twi" || name == "tw") && ppc::t(w) == 31) || (op->sub == ppc::Sub::X9 && ppc::oe(w)) || op->kind == Kind::UNSUP) { nd.fall = false; continue; }
        const CrEffect fx = cr_effect(*op, w);
        nd.use = fx.use; nd.def = fx.def;
        switch (op->kind) {
            case Kind::SC: nd.call = {Callee::Hle, 0, 0}; break;
            case Kind::BR: case Kind::BC: {
                const bool bc = op->kind == Kind::BC;
                const uint32_t bo = bc ? ppc::bo(w) : 20, bi = bc ? ppc::bi(w) : 0;
                const int32_t disp = bc ? ppc::bd(w) : ppc::li(w);
                const uint32_t target = ppc::aa(w) ? uint32_t(disp) : pc + uint32_t(disp);
                const Target tk = classify(p, f, target);
                if (bc && !(bo & 16)) nd.use |= fld(bi >> 2);
                if (ppc::lk(w)) {
                    if (!(tk.kind == Target::Outside || (tk.kind == Target::Internal && target != f.entry))) nd.call = resolve_callee(p, f, target);
                    nd.fall = true;   // a call continues after it returns; the other path of a conditional call skips it
                } else {
                    if (tk.kind == Target::Internal) { nd.taken = CrNode::Internal; nd.target = index(target); }
                    else if (tk.kind != Target::Outside) { nd.taken = CrNode::Tail; nd.tail = resolve_callee(p, f, target); }
                    nd.fall = bc && !((bo & 4) && (bo & 16));
                }
                break;
            }
            case Kind::BCLR: {
                const uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
                if (!(bo & 16)) nd.use |= fld(bi >> 2);
                if (ppc::lk(w)) nd.call = {Callee::Indirect, 0, 0};
                else { nd.taken = CrNode::Return; nd.fall = !((bo & 4) && (bo & 16)); }
                break;
            }
            case Kind::BCCTR: {
                const uint32_t bo = ppc::bo(w), bi = ppc::bi(w);
                const bool table = !ppc::lk(w) && std::any_of(p.an.jump_tables.begin(), p.an.jump_tables.end(), [&](const auto& jt) { return jt.bctr_addr == pc; });
                if (table) {
                    nd.fall = false;   // the switch ignores the branch condition
                    for (const auto& jt : p.an.jump_tables)
                        if (jt.bctr_addr == pc) for (uint32_t t : jt.targets) if (t >= f.entry && t < f.end) nd.table.push_back(index(t));
                    break;
                }
                if (!(bo & 16)) nd.use |= fld(bi >> 2);
                if (ppc::lk(w)) nd.call = {Callee::Indirect, 0, 0};
                else { nd.taken = CrNode::IndirectTail; nd.fall = !(bo & 16); }
                break;
            }
            default: break;
        }
    }
    return nodes;
}

struct CrFn {
    const Function* f = nullptr;
    std::vector<CrNode> nodes;
    std::vector<uint8_t> in, out;
    Callee fall_tail;   // the code after the last instruction continues into the next function
};
}  // namespace

CrLive analyze_cr(const Program& p) {
    std::vector<CrFn> fns;
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry) || f.end <= f.entry) continue;
        CrFn fn;
        fn.f = &f;
        fn.nodes = cr_nodes(p, f);
        fn.in.assign(fn.nodes.size(), 0); fn.out.assign(fn.nodes.size(), 0);
        uint32_t last = 0;
        p.img.word(f.end - 4, last);
        if (!is_terminator_word(last) && p.an.is_entry(f.end)) fn.fall_tail = resolve_callee(p, f, f.end);
        fns.push_back(std::move(fn));
    }
    std::unordered_map<uint32_t, uint8_t> entry_live, ret_live;   // entry/secondary address -> fields read on entry; function -> fields read after it returns
    uint8_t any_entry = 0, any_ret = 0;                           // unknown (indirect) callees / callers
    auto grow = [](uint8_t& dst, uint8_t add) { const uint8_t before = dst; dst |= add; return dst != before; };
    auto call_in = [&](const Callee& c) -> uint8_t {
        if (c.kind == Callee::Direct) { auto it = entry_live.find(c.entry); return it == entry_live.end() ? 0 : it->second; }
        return c.kind == Callee::Indirect ? any_entry : 0;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (auto& fn : fns) {
            const Function& f = *fn.f;
            const size_t n = fn.nodes.size();
            const uint8_t ret_f = uint8_t(ret_live[f.entry] | any_ret);
            const uint8_t end_in = fn.fall_tail.kind == Callee::None ? 0 : uint8_t(call_in(fn.fall_tail) | ret_f);
            for (bool again = true; again;) {
                again = false;
                for (size_t i = n; i-- > 0;) {
                    const CrNode& nd = fn.nodes[i];
                    uint8_t o = 0;
                    if (nd.fall) o |= i + 1 < n ? fn.in[i + 1] : end_in;
                    switch (nd.taken) {
                        case CrNode::Internal: o |= fn.in[size_t(nd.target)]; break;
                        case CrNode::Return: o |= ret_f; break;
                        case CrNode::Tail: o |= uint8_t(call_in(nd.tail) | ret_f); break;
                        case CrNode::IndirectTail: o |= uint8_t(any_entry | ret_f); break;
                        default: break;
                    }
                    for (int t : nd.table) o |= fn.in[size_t(t)];
                    const uint8_t ni = uint8_t(nd.use | (o & ~nd.def) | call_in(nd.call));
                    fn.out[i] = o;
                    if (ni != fn.in[i]) { fn.in[i] = ni; again = true; }
                }
            }
            // publish this function's summary
            if (n) {
                changed |= grow(entry_live[f.entry], fn.in[0]);
                changed |= grow(any_entry, fn.in[0]);
                auto sec = p.an.secondary.find(f.entry);
                if (sec != p.an.secondary.end())
                    for (uint32_t t : sec->second) if (t >= f.entry && t < f.end) { changed |= grow(entry_live[t], fn.in[size_t((t - f.entry) / 4)]); changed |= grow(any_entry, fn.in[size_t((t - f.entry) / 4)]); }
            }
            for (size_t i = 0; i < n; ++i) {
                const CrNode& nd = fn.nodes[i];
                if (nd.call.kind == Callee::Direct) changed |= grow(ret_live[nd.call.fn], fn.out[i]);
                else if (nd.call.kind == Callee::Indirect) changed |= grow(any_ret, fn.out[i]);
                if (nd.taken == CrNode::Tail && nd.tail.kind == Callee::Direct) changed |= grow(ret_live[nd.tail.fn], ret_f);
                else if (nd.taken == CrNode::IndirectTail) changed |= grow(any_ret, ret_f);
            }
            if (fn.fall_tail.kind == Callee::Direct) changed |= grow(ret_live[fn.fall_tail.fn], ret_f);
        }
    }
    CrLive live;
    for (auto& fn : fns) live.out[fn.f->entry] = std::move(fn.out);
    return live;
}

// ---- register cache ----
// In register-cache mode every translated function works on a local ppc::Ctx `c` (only the parts it uses survive the optimiser, as host
// registers) and syncs it with the real context `ctx` where other code can look: before a call, a return, a tail call, a trap or an interrupt
// poll it stores the registers written since the last sync ("dirty", forward dataflow), and after a call or a poll it reloads the registers
// that are read before being written again ("live", backward dataflow; a later flush counts as a read, so a flushed copy is always current).
// Guest memory stores can no longer alias the registers, so the compiler keeps them in host registers across loads and stores.
namespace {
struct RegNode {
    RegEffect eff;
    bool fall = true;          // continues at the next instruction on a path without a sync
    bool call = false;         // a call on some path, after which execution continues at the next instruction
    bool exit = false;         // a path that flushes and leaves (return, tail call, trap)
    int target = -1;           // internal branch target (instruction index; n = past the end)
    bool poll = false;         // the internal branch is a loop back edge (an interrupt poll on its slow path)
    std::vector<int> table;    // jump table targets
    RegSet pass;               // a call of a leaf function: dirty registers it neither loads nor stores (they stay dirty across it)
    RegSet call_writes;        // the registers the call can change in the real context (all, unless a leaf)
    bool tail = false;         // the exit is a jump into another function
};

RegSet cached_mask() { RegSet m; for (int i = 0; i < res::COUNT; ++i) m.set(size_t(i)); return m; }

std::string copy_text(const RegSet& set, bool to_ctx) {
    std::string s;
    for (int i = 0; i < res::COUNT; ++i) {
        if (!set.test(size_t(i))) continue;
        s += to_ctx ? reg_field(i, "ctx") + " = " + reg_field(i, "c") + "; " : reg_field(i, "c") + " = " + reg_field(i, "ctx") + "; ";
    }
    return s;
}

struct FnPlanView { const std::vector<uint32_t>* words; const std::vector<std::string>* comment; };

// Control flow of one instruction as emit_insn writes it.
RegNode reg_node(const Program& p, const Function& f, uint32_t pc, uint32_t w, const CrHints& h, const RegSet& fused_use, bool comment,
                 const std::unordered_map<uint32_t, LeafRegs>* leaves) {
    using ppc::Kind;
    RegNode nd;
    auto index = [&](uint32_t t) { return int((t - f.entry) / 4); };
    const ppc::OpInfo* op = ppc::decode(w);
    if (comment) return nd;   // a dead or folded compare: no code
    if (!op || ((std::string(op->name) == "twi" || std::string(op->name) == "tw") && ppc::t(w) == 31) || (op->sub == ppc::Sub::X9 && ppc::oe(w)) ||
        op->kind == Kind::UNSUP) { nd.fall = false; nd.exit = true; return nd; }
    auto use = [&](int i) { nd.eff.use.set(size_t(i)); };
    auto def = [&](int i, bool always) { nd.eff.may.set(size_t(i)); if (always) nd.eff.must.set(size_t(i)); };
    // a taken branch to `target` (lk: a call): the same classification as branch_action; true when the taken path goes on at the next
    // instruction without a sync (a save/restore helper written in place)
    auto action = [&](uint32_t target, bool lk, bool always) {
        const Target tk = classify(p, f, target);
        if (tk.kind == Target::Internal && !lk) {
            nd.target = index(target); nd.poll = target <= pc;
            if (nd.poll) { use(res::POLL); def(res::POLL, false); }   // the back edge counts in the local copy of poll_count
            return false;
        }
        if (tk.kind == Target::Outside || (tk.kind == Target::Internal && lk && target != f.entry)) { nd.exit = true; return false; }
        if (tk.kind == Target::Entry) {
            if (const RegHelper rh = reg_helper(p, target); rh.kind) {
                const RegEffect he = reg_helper_effect(rh);
                nd.eff.use |= he.use; nd.eff.may |= he.may;
                if (always) nd.eff.must |= he.must;
                if (lk) { def(res::LR, always); return true; }
                nd.exit = true;
                return false;
            }
        }
        if (lk) {
            def(res::LR, always); nd.call = true;
            auto leaf = leaves && tk.kind == Target::Entry ? leaves->find(target) : decltype(leaves->end()){};
            if (leaves && tk.kind == Target::Entry && leaf != leaves->end()) { nd.pass = cached_mask() & ~leaf->second.touched; nd.call_writes = leaf->second.written; }
            else nd.call_writes = cached_mask();
            return false;
        }
        nd.exit = true; nd.tail = true;
        return false;
    };
    // the condition of a conditional branch; true when it is always taken
    auto condition = [&](uint32_t bo, uint32_t bi, bool ctr_ok) {
        if (!(bo & 4) && ctr_ok) { use(res::CTR); def(res::CTR, true); }
        if (!(bo & 16)) { if (h.fused) nd.eff.use |= fused_use; else use(res::CR + int(bi >> 2)); }
        return (bo & 16) && ((bo & 4) || !ctr_ok);
    };
    switch (op->kind) {
        case Kind::SC: nd.fall = false; nd.call = true; nd.call_writes = cached_mask(); return nd;
        case Kind::BR: {
            const int32_t li = ppc::li(w);
            nd.fall = action(ppc::aa(w) ? uint32_t(li) : pc + uint32_t(li), ppc::lk(w), true);
            return nd;
        }
        case Kind::BC: {
            const bool always = condition(ppc::bo(w), ppc::bi(w), true);
            const bool goes_on = action(ppc::aa(w) ? uint32_t(ppc::bd(w)) : pc + uint32_t(ppc::bd(w)), ppc::lk(w), always);
            nd.fall = !always || goes_on;
            return nd;
        }
        case Kind::BCLR: {
            const bool always = condition(ppc::bo(w), ppc::bi(w), true);
            if (ppc::lk(w)) { use(res::LR); def(res::LR, always); nd.call = true; nd.call_writes = cached_mask(); }
            else nd.exit = true;
            nd.fall = !always;
            return nd;
        }
        case Kind::BCCTR: {
            const bool table = !ppc::lk(w) && std::any_of(p.an.jump_tables.begin(), p.an.jump_tables.end(), [&](const auto& jt) { return jt.bctr_addr == pc; });
            use(res::CTR);
            if (table) {
                for (const auto& jt : p.an.jump_tables)
                    if (jt.bctr_addr == pc) for (uint32_t t : jt.targets) if (t >= f.entry && t < f.end) nd.table.push_back(index(t));
                nd.fall = false; nd.exit = true;   // the default case traps
                return nd;
            }
            const bool always = condition(ppc::bo(w) | 4, ppc::bi(w), false);
            if (ppc::lk(w)) { def(res::LR, always); nd.call = true; nd.call_writes = cached_mask(); }
            else { nd.exit = true; nd.tail = true; }
            nd.fall = !always;
            return nd;
        }
        default: break;
    }
    bool ok = false;
    nd.eff = reg_effect(*op, w, op->rc && ppc::rc(w) && !h.drop_rc, ok);
    if (!ok) { nd.eff.use = cached_mask(); }   // not expected: read everything, write nothing
    return nd;
}

// The guest loads and stores whose base register may hold an address in the hardware-register window (0xCC000000..0xCDFFFFFF): a
// forward "may" dataflow over the function from `lis rX,0xCC00..0xCE00` through addi/addis/ori/mr and update-form writebacks; a call keeps
// only the registers the ABI preserves (r1, r2, r13..r31); any other write ends it. A load or store from such a base is emitted with
// the explicit MMIO test (ppc::op_*<true>): with fastmem the others carry no test, and an MMIO access among them costs a fault. Only a
// heuristic for speed: either form is correct for any address.
const std::set<std::string>& guest_memory_ops() {
    static const std::set<std::string> ops = {
        "lbz", "lbzu", "lbzx", "lbzux", "lhz", "lhzu", "lhzx", "lhzux", "lha", "lhau", "lhax", "lhaux", "lwz", "lwzu", "lwzx", "lwzux",
        "stb", "stbu", "stbx", "stbux", "sth", "sthu", "sthx", "sthux", "stw", "stwu", "stwx", "stwux",
        "lfs", "lfsu", "lfsx", "lfsux", "lfd", "lfdu", "lfdx", "lfdux", "stfs", "stfsu", "stfsx", "stfsux", "stfd", "stfdu", "stfdx", "stfdux", "stfiwx",
        "psq_l", "psq_lu", "psq_lx", "psq_lux", "psq_st", "psq_stu", "psq_stx", "psq_stux"};
    return ops;
}
size_t mark_mmio_bases(const FnPlanView& plan, const std::vector<RegNode>& nodes, std::vector<CrHints>& hints) {
    using Gprs = std::bitset<32>;
    const size_t n = nodes.size();
    std::vector<Gprs> in(n + 1);
    std::vector<bool> reached(n + 1, false);
    auto transfer = [&](size_t i, Gprs g) {
        if (!(*plan.comment)[i].empty()) return g;   // a folded compare: no code
        const uint32_t w = (*plan.words)[i];
        const ppc::OpInfo* op = ppc::decode(w);
        const RegNode& nd = nodes[i];
        if (nd.call) { g.reset(0); for (int r = 3; r <= 12; ++r) g.reset(size_t(r)); }
        if (!op) return g;
        const std::string name = op->name;
        const uint32_t T = ppc::t(w), A = ppc::a(w), B = ppc::b(w);
        auto clear_written = [&] { for (int r = 0; r < 32; ++r) if (nd.eff.may.test(size_t(res::R + r))) g.reset(size_t(r)); };
        if (name == "addis" && A == 0) { const uint32_t hi = ppc::uimm(w); g.set(T, hi >= 0xCC00 && hi <= 0xCE00); return g; }
        if (name == "addi" || name == "addis" || name == "addic") { const bool keep = A && g.test(A); g.set(T, keep); return g; }
        if (name == "ori" || name == "oris") { const bool keep = g.test(T); g.set(A, keep); return g; }   // rA = rS | imm
        if (name == "or" && T == B) { const bool keep = g.test(T); clear_written(); g.set(A, keep); return g; }   // mr rA, rS
        if (guest_memory_ops().count(name)) {
            const bool update = name.back() == 'u' || (name.size() > 2 && name.compare(name.size() - 2, 2, "ux") == 0);
            const bool keep_a = A && g.test(A);
            clear_written();
            if (update) g.set(A, keep_a);   // rA = the effective address
            return g;
        }
        clear_written();
        return g;
    };
    reached[0] = true;
    for (bool changed = true; changed;) {
        changed = false;
        for (size_t i = 0; i < n; ++i) {
            if (!reached[i]) continue;
            const Gprs out = transfer(i, in[i]);
            auto flow = [&](int to) {
                if (to < 0 || size_t(to) > n) return;
                const Gprs merged = in[size_t(to)] | out;
                if (!reached[size_t(to)] || merged != in[size_t(to)]) { in[size_t(to)] = merged; reached[size_t(to)] = true; changed = true; }
            };
            if (nodes[i].fall || nodes[i].call) flow(int(i + 1));
            if (nodes[i].target >= 0) flow(nodes[i].target);
            for (int t : nodes[i].table) flow(t);
        }
    }
    size_t marked = 0;
    for (size_t i = 0; i < n; ++i) {
        if (!(*plan.comment)[i].empty()) continue;
        const uint32_t w = (*plan.words)[i];
        const ppc::OpInfo* op = ppc::decode(w);
        if (!op || !guest_memory_ops().count(op->name)) continue;
        const std::string name = op->name;
        const uint32_t A = ppc::a(w), B = ppc::b(w);
        const bool indexed = name.back() == 'x';
        if ((A && in[i].test(A)) || (indexed && in[i].test(B))) { hints[i].io = true; ++marked; }
    }
    return marked;
}
}  // namespace

// What each instruction of a function becomes (labels, CR liveness decisions). The hints point into the plan: it is filled in place.
struct FnPlan {
    std::set<uint32_t> labels;
    size_t n = 0;
    std::vector<uint32_t> words;
    std::vector<CrHints> hints;
    std::vector<std::string> comment;   // a dead or folded compare: emitted as this comment
    std::vector<FusedCmp> fused;        // compare operands for the branch at i
    std::vector<RegSet> fused_use;
    bool tail_end = false;              // the last instruction falls into the next function
};
static void plan_function(const Program& p, const Function& f, const CrLive* live, EmitStats& st, FnPlan& plan) {
    std::set<uint32_t>& labels = plan.labels;
    labels = {f.entry};
    for (uint32_t pc = f.entry; pc < f.end; pc += 4) {
        uint32_t w = 0;
        if (!p.img.word(pc, w)) continue;
        const ppc::OpInfo* op = ppc::decode(w);
        if (!op) continue;
        if (op->kind == ppc::Kind::BR || op->kind == ppc::Kind::BC) {
            if (ppc::lk(w)) continue;
            int32_t d = op->kind == ppc::Kind::BR ? ppc::li(w) : ppc::bd(w);
            uint32_t t = ppc::aa(w) ? uint32_t(d) : pc + uint32_t(d);
            if (t >= f.entry && t < f.end) labels.insert(t);
        }
    }
    for (const auto& jt : p.an.jump_tables) if (jt.bctr_addr >= f.entry && jt.bctr_addr < f.end) for (uint32_t t : jt.targets) labels.insert(t);
    auto sit = p.an.secondary.find(f.entry);
    if (sit != p.an.secondary.end()) for (uint32_t t : sit->second) labels.insert(t);

    const size_t n = plan.n = (f.end - f.entry) / 4;
    auto& words = plan.words; words.assign(n, 0);
    auto& hints = plan.hints; hints.assign(n, CrHints{});
    auto& comment = plan.comment; comment.assign(n, std::string());
    auto& fused = plan.fused; fused.assign(n, FusedCmp{});
    auto& fused_use = plan.fused_use; fused_use.assign(n, RegSet());
    const std::vector<uint8_t>* after = nullptr;   // CR fields still readable after each instruction
    if (live) { auto it = live->out.find(f.entry); if (it != live->out.end()) after = &it->second; }
    for (size_t i = 0; i < n; ++i) {
        const uint32_t pc = f.entry + uint32_t(i) * 4;
        uint32_t& w = words[i];
        p.img.word(pc, w);
        const ppc::OpInfo* op = after ? ppc::decode(w) : nullptr;
        if (op && op->kind != ppc::Kind::UNSUP && !(op->sub == ppc::Sub::X9 && ppc::oe(w)) && !((std::string(op->name) == "twi" || std::string(op->name) == "tw") && ppc::t(w) == 31)) {
            const uint8_t live_after = (*after)[i];
            const std::string name = op->name;
            if (name == "cmp" || name == "cmpl" || name == "cmpi" || name == "cmpli") {
                const uint32_t field = ppc::crfd(w);
                uint32_t next = 0;
                const ppc::OpInfo* nop = pc + 4 < f.end && !labels.count(pc + 4) && p.img.word(pc + 4, next) ? ppc::decode(next) : nullptr;
                if (!(live_after & fld(field))) { comment[i] = "/* cr" + num(field) + " is never read */"; ++st.cr_dead_compares; }
                else if (nop && (nop->kind == ppc::Kind::BC || nop->kind == ppc::Kind::BCLR) && !ppc::lk(next) && (ppc::bo(next) & 4) && !(ppc::bo(next) & 16) &&
                         (ppc::bi(next) >> 2) == field && !((*after)[i + 1] & fld(field))) {
                    // the only reader is the next branch: test the operands there and skip the CR write
                    const std::string a = "c.r[" + num(ppc::a(w)) + "]";
                    const bool is_signed = name == "cmp" || name == "cmpi";
                    const std::string lhs = is_signed ? "int32_t(" + a + ")" : a;
                    std::string rhs;
                    RegSet u;
                    u.set(size_t(res::R + int(ppc::a(w)))); u.set(size_t(res::SO));
                    if (name == "cmp" || name == "cmpl") { rhs = is_signed ? "int32_t(c.r[" + num(ppc::b(w)) + "])" : "c.r[" + num(ppc::b(w)) + "]"; u.set(size_t(res::R + int(ppc::b(w)))); }
                    else rhs = is_signed ? num(ppc::simm(w)) : num(ppc::uimm(w)) + "u";
                    fused[i + 1] = {lhs, rhs};
                    fused_use[i + 1] = u;
                    hints[i + 1].fused = &fused[i + 1];
                    comment[i] = "/* cr" + num(field) + " compare is folded into the next branch */";
                    ++st.cr_fused_branches;
                }
            } else if (op->rc && ppc::rc(w) && !(live_after & fld(fp_name(name) ? 1 : 0))) {
                hints[i].drop_rc = true;
                ++st.cr_dropped_records;
            }
        }
    }
    uint32_t last = 0;
    p.img.word(f.end - 4, last);
    plan.tail_end = !is_terminator_word(last) && p.an.is_entry(f.end);
}



std::string emit_function(const Program& p, const Function& f, EmitStats& st, const CrLive* live, const std::unordered_map<uint32_t, LeafRegs>* leaves) {
    FnPlan plan;
    plan_function(p, f, live, st, plan);
    const std::set<uint32_t>& labels = plan.labels;
    const size_t n = plan.n;
    const auto& words = plan.words;
    auto& hints = plan.hints;
    const auto& comment = plan.comment;
    const auto& fused_use = plan.fused_use;
    const bool tail_end = plan.tail_end;
    auto sit = p.an.secondary.find(f.entry);
    const bool has_sec = sit != p.an.secondary.end();

    // the loads and stores from a hardware-register base keep the explicit MMIO test (fastmem builds)
    {
        std::vector<RegNode> nodes(n);
        for (size_t i = 0; i < n; ++i) nodes[i] = reg_node(p, f, f.entry + uint32_t(i) * 4, words[i], hints[i], fused_use[i], !comment[i].empty(), leaves);
        st.mmio_sites += mark_mmio_bases({&words, &comment}, nodes, hints);
    }

    // pass 2: register cache dataflow
    const bool local = p.reg_cache;
    RegSet entry_load;
    std::vector<RegSet> dirty_after(n + 1), live_in(n + 1);
    std::vector<std::string> fall_flush(n);   // flush on the fall-through edge into the next instruction
    if (local) {
        std::vector<RegNode> nodes(n);
        for (size_t i = 0; i < n; ++i) nodes[i] = reg_node(p, f, f.entry + uint32_t(i) * 4, words[i], hints[i], fused_use[i], !comment[i].empty(), leaves);
        // Control-flow edges. Dirty = written since the last sync on *every* path: where paths with different dirty sets meet over a forward
        // edge, the edge itself flushes the difference (a join after a conditional write stores there, once); over a back edge the loop head
        // keeps the union (the loop runs without stores) and the entry path keeps those registers current instead (the liveness below makes
        // them reloaded at its last sync). So a flush never stores a copy that a path left stale.
        struct Edge { int from, to; bool sync, back, fall; };   // sync: the edge leaves a call (nothing is dirty after it); fall: the fall-through edge
        std::vector<Edge> edges;
        std::vector<std::vector<int>> into(n + 1);
        auto add = [&](int from, int to, bool sync, bool fall = false) { into[size_t(to)].push_back(int(edges.size())); edges.push_back({from, to, sync, from >= 0 && to <= from, fall}); };
        add(-1, 0, true);
        if (has_sec) for (uint32_t t : sit->second) if (t >= f.entry && t < f.end) add(-1, int((t - f.entry) / 4), true);
        for (size_t i = 0; i < n; ++i) {
            const RegNode& nd = nodes[i];
            if (nd.fall) add(int(i), int(i + 1), false, true);
            if (nd.call) add(int(i), int(i + 1), true);
            if (nd.target >= 0) add(int(i), nd.target, false);
            for (int t : nd.table) add(int(i), t, false);
        }
        std::vector<RegSet> dirty_in(n + 1);
        auto edge_out = [&](const Edge& e) {   // a call leaves only what a leaf callee does not touch dirty
            return e.from < 0 ? RegSet() : e.sync ? (dirty_after[size_t(e.from)] & nodes[size_t(e.from)].pass) : dirty_after[size_t(e.from)];
        };
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t m = 0; m <= n; ++m) {
                RegSet fwd, back;
                bool any_fwd = false;
                for (int k : into[m]) {
                    const Edge& e = edges[size_t(k)];
                    if (e.back) back |= edge_out(e);
                    else { fwd = any_fwd ? (fwd & edge_out(e)) : edge_out(e); any_fwd = true; }
                }
                const RegSet in = fwd | back;
                if (in != dirty_in[m]) { dirty_in[m] = in; changed = true; }
                if (m < n) dirty_after[m] = dirty_in[m] | nodes[m].eff.may;
            }
        }
        dirty_after[n] = dirty_in[n];
        std::vector<RegSet> edge_flush(edges.size());
        for (size_t k = 0; k < edges.size(); ++k) if (!edges[k].back) edge_flush[k] = edge_out(edges[k]) & ~dirty_in[size_t(edges[k].to)];
        std::vector<std::vector<int>> out(n);
        for (size_t k = 0; k < edges.size(); ++k) if (edges[k].from >= 0) out[size_t(edges[k].from)].push_back(int(k));
        live_in[n] = dirty_in[n];   // past the end: a tail call or a trap flushes
        for (bool changed = true; changed;) {   // backward: needed (read, or flushed) before the next write
            changed = false;
            for (size_t i = n; i-- > 0;) {
                const RegNode& nd = nodes[i];
                RegSet x;
                if (nd.exit || nd.poll) x |= dirty_after[i];
                // a call flushes what the callee may touch; the copies it cannot change must be current before it
                if (nd.call) x |= (dirty_after[i] & ~nd.pass) | (live_in[i + 1] & ~nd.call_writes);
                for (int k : out[i]) x |= (edges[size_t(k)].sync ? RegSet() : live_in[size_t(edges[size_t(k)].to)]) | edge_flush[size_t(k)];
                const RegSet in = nd.eff.use | (nd.eff.may & ~nd.eff.must) | (x & ~nd.eff.must);
                if (in != live_in[i]) { live_in[i] = in; changed = true; }
            }
        }
        entry_load = live_in[0];
        for (size_t i = 0; i < n; ++i) {
            const RegNode& nd = nodes[i];
            hints[i].local = true;
            const RegSet flush = nd.call ? dirty_after[i] & ~nd.pass : dirty_after[i];
            if (nd.call || nd.exit || nd.poll) { hints[i].flush = copy_text(flush, true); st.reg_flushes += flush.count(); }
            if (nd.call) { hints[i].reload = copy_text(live_in[i + 1] & nd.call_writes, false); st.reg_reloads += (live_in[i + 1] & nd.call_writes).count(); }
            if (nd.call && nd.pass.any()) {
                ++st.reg_leaf_calls;
                hints[i].leaf_call = true;
                hints[i].full_flush = copy_text(dirty_after[i], true);
                hints[i].full_reload = copy_text(live_in[i + 1] | (dirty_after[i] & nd.pass), false);   // copies the leaf path keeps must stay current
            }
            else if (nd.poll) { hints[i].reload = copy_text(live_in[size_t(nd.target)], false); st.reg_reloads += live_in[size_t(nd.target)].count(); }
        }
        for (size_t k = 0; k < edges.size(); ++k) {
            const Edge& e = edges[size_t(k)];
            if (e.from < 0 || e.back || edge_flush[k].none()) continue;
            st.reg_flushes += edge_flush[k].count();
            const std::string text = copy_text(edge_flush[k], true);
            if (e.sync) {   // after a leaf call: on the call's own path
                if (nodes[size_t(e.from)].fall) hints[size_t(e.from)].after_call = text;
                else fall_flush[size_t(e.from)] = text;
            }
            else if (e.fall && nodes[size_t(e.from)].call) hints[size_t(e.from)].not_taken = text;   // a conditional call: in its else branch
            else if (e.fall) fall_flush[size_t(e.from)] = text;   // after the instruction, before the next label
            else hints[size_t(e.from)].edge[f.entry + uint32_t(e.to) * 4] = text;   // on the jump (a jump table: in its case)
        }
        st.reg_entry_loads += entry_load.count();
    }

    // pass 3: text
    std::ostringstream o;
    if (has_sec) o << "static void body_" << hex8(f.entry) << "(ppc::Ctx& c, uint32_t at) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n";
    else o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n";
    if (local) {
        // a resume through a secondary entry (rare) loads what is read from there on; the normal entry only what the entry reads
        o << "  ppc::Ctx& ctx = c;\n  {\n  ppc::Ctx c; c.mem = ctx.mem;\n";
        if (has_sec) {
            o << "  if (at != 0) [[unlikely]] switch (at) { ";
            for (uint32_t t : sit->second) {
                const RegSet load = t >= f.entry && t < f.end ? live_in[(t - f.entry) / 4] : RegSet();
                st.reg_entry_loads += load.count();
                o << "case 0x" << hex8(t) << "u: " << copy_text(load, false) << "goto L_" << hex8(t) << "; ";
            }
            o << "default: break; }\n";
        }
        o << "  " << copy_text(entry_load, false) << "\n";
    } else if (has_sec) {
        o << "  switch (at) { ";
        for (uint32_t t : sit->second) o << "case 0x" << hex8(t) << "u: goto L_" << hex8(t) << "; ";
        o << "default: break; }\n";
    }
    for (size_t i = 0; i < n; ++i) {
        const uint32_t pc = f.entry + uint32_t(i) * 4;
        if (labels.count(pc)) o << "L_" << hex8(pc) << ":;\n";
        std::string code = comment[i];
        if (code.empty()) code = emit_insn(p, f, pc, words[i], st, &hints[i]);
        else ++st.instructions;
        if (p.count_insns) code = "++ppc::g_guest_insns; " + code;
        o << "  /*" << hex8(pc) << "*/ { " << code << " }" << (fall_flush[i].empty() ? "" : " { " + fall_flush[i] + "}") << "\n";
    }
    const std::string end_flush = local ? copy_text(dirty_after[n], true) : "";
    if (local) st.reg_flushes += dirty_after[n].count();
    if (tail_end) {
        ++st.fallthrough_tail;
        o << "  " << end_flush << callee(p, f.end) << (local ? "(ctx)" : "(c)") << "; return;\n";
    } else {
        o << "  " << end_flush << trap_str(f.end, "fell off the end of function " + hex8(f.entry)) << "\n";
    }
    o << (local ? "  }\n}\n" : "}\n");
    if (has_sec) {
        o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) { body_" << hex8(f.entry) << "(c, 0); }\n";
        for (uint32_t t : sit->second) {
            o << "void " << sec_name(f.entry, t) << "(ppc::Ctx& c) { body_" << hex8(f.entry) << "(c, 0x" << hex8(t) << "u); }\n";
            ++st.secondary_entries;
        }
    }
    ++st.functions;
    return o.str();
}

// The translated functions a call may sync with partially: no call (the save/restore helpers are written in place), no loop (so no
// interrupt poll: no safe point where the scheduler or an interrupt could look at the context), no jump table, no tail or indirect jump
// and no fall into the next function. Their loads and stores of the real context are their entry loads and flushes, all within the
// registers they read or write. The generated Leaf_ HLEs (runtime/hle/leaf_gen.cpp, written by tools/leaf_hle.py from these very
// instructions) qualify the same way: they copy in and out only registers their guest code uses. So do the native SDK matrix/vector
// routines below: they read the guest routine's arguments and write no register but its result (tests/test_sdk_math_hle.cpp checks both).
static bool leaf_hle(const std::string& name) {
    static const std::set<std::string> math = {"PSMTXIdentity", "PSMTXCopy", "PSMTXConcat", "PSMTXTransApply", "PSMTXScaleApply", "PSMTXMultVec",
        "PSVECAdd", "PSVECSubtract", "PSVECScale", "PSVECNormalize", "PSVECSquareMag", "PSVECMag", "PSQUATDotProduct", "PSVECCrossProduct"};
    return name.compare(0, 5, "Leaf_") == 0 || math.count(name);
}
static std::unordered_map<uint32_t, LeafRegs> analyze_leaves(const Program& p, const CrLive* live) {
    std::unordered_map<uint32_t, LeafRegs> out;
    EmitStats scratch;
    for (const auto& f : p.an.functions) {
        if (f.end <= f.entry) continue;
        if (auto h = p.cfg.hle.find(f.entry); h != p.cfg.hle.end() && !leaf_hle(h->second)) continue;
        FnPlan plan;
        plan_function(p, f, live, scratch, plan);
        if (plan.tail_end) continue;
        LeafRegs regs;
        bool leaf = true;
        for (size_t i = 0; i < plan.n && leaf; ++i) {
            const RegNode nd = reg_node(p, f, f.entry + uint32_t(i) * 4, plan.words[i], plan.hints[i], plan.fused_use[i], !plan.comment[i].empty(), nullptr);
            if (nd.call || nd.poll || nd.tail || !nd.table.empty()) leaf = false;
            regs.touched |= nd.eff.use | nd.eff.may;
            regs.written |= nd.eff.may;
        }
        if (leaf) out[f.entry] = regs;
    }
    return out;
}

Output emit_program(const Program& p, size_t shard, EmitStats& st) {
    Output out;
    std::set<std::string> hle_names;
    for (auto& [a, n] : p.cfg.hle) hle_names.insert(n);

    std::string decl = "#pragma once\n#include \"ppc_rt.h\"\n#include \"dispatch.h\"\n";
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        decl += "void " + fn_name(f.entry) + "(ppc::Ctx&);\n";
        auto it = p.an.secondary.find(f.entry);
        if (it != p.an.secondary.end()) for (uint32_t t : it->second) decl += "void " + sec_name(f.entry, t) + "(ppc::Ctx&);\n";
    }
    for (const auto& n : hle_names) decl += "void hle_" + n + "(ppc::Ctx&);\n";
    out.files.push_back({"game_decl.h", decl});

    size_t idx = 0, in_cur = 0;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        char nm[32]; std::snprintf(nm, sizeof nm, "game_%03zu.cpp", idx++);
        out.files.push_back({nm, "#include \"game_decl.h\"\n" + cur});
        cur.clear(); in_cur = 0;
    };
    const CrLive live = p.cr_opt ? analyze_cr(p) : CrLive{};
    const auto leaves = p.reg_cache ? analyze_leaves(p, p.cr_opt ? &live : nullptr) : std::unordered_map<uint32_t, LeafRegs>{};
    st.reg_leaves = leaves.size();
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        cur += emit_function(p, f, st, p.cr_opt ? &live : nullptr, &leaves);
        if (++in_cur >= shard) flush();
    }
    flush();

    std::string table = "#include \"game_decl.h\"\nextern const uc::FnEntry uc_functions[] = {\n";
    size_t table_count = p.an.functions.size();
    for (const auto& f : p.an.functions) {
        table += "  {0x" + hex8(f.entry) + "u, " + callee(p, f.entry) + "},\n";
        auto it = p.an.secondary.find(f.entry);
        if (it != p.an.secondary.end() && !p.cfg.hle.count(f.entry)) {
            for (uint32_t t : it->second) { table += "  {0x" + hex8(t) + "u, " + sec_name(f.entry, t) + "},\n"; ++table_count; }
        }
    }
    table += "};\nextern const size_t uc_function_count = " + std::to_string(table_count) + ";\n";
    uint32_t tb = 0xFFFFFFFFu, te = 0;
    for (const auto& s : p.img.dol->sections) if (s.is_text) { tb = std::min(tb, s.address); te = std::max(te, s.address + s.size); }
    table += "extern const uint32_t uc_text_begin = 0x" + hex8(tb) + "u;\nextern const uint32_t uc_text_end = 0x" + hex8(te) + "u;\n";
    out.files.push_back({"game_table.cpp", table});
    return out;
}

}  // namespace uc
