#include "translator/emit.h"
#include "translator/regeffect.h"
#include "ppc/insn.h"
#include "ppc/isa.h"
#include <algorithm>
#include <cstdio>
#include <functional>
#include <set>
#include <sstream>

namespace uc {

static std::string hex8(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "%08X", v); return b; }
static std::string num(long long v) { return std::to_string(v); }
static std::string fn_name(uint32_t a) { return "fn_" + hex8(a); }
static std::string sec_name(uint32_t owner, uint32_t t) { return "fn_" + hex8(owner) + "_" + hex8(t); }
// game_decl.h: true when no native code runs at the entry of any function in the call tree of a closed function (one per component)
static std::string plain_tree_name(uint32_t tree) { return "uc_plain_tree_" + hex8(tree); }

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
        // a loop polls for interrupts; what runs there (events on the interrupt context, another thread) does not read this thread's
        // registers, so the poll syncs only r2/r13 (the interrupt context copies them) and keeps the local copy
        if (s.local()) return "if ((++c.poll_count & 0x7FFu) == 0) [[unlikely]] { " + s.flush + "ppc::poll_slow(ctx); " + s.reload + (s.h ? s.h->frame_note : std::string()) + "} goto L_" + hex8(target) + ";";
        return "ppc::poll(c); goto L_" + hex8(target) + ";";
    }
    if (tk.kind == Target::Outside || (tk.kind == Target::Internal && lk && target != f.entry)) {
        ++st.traps_midfunc;
        return s.flush + trap_str(pc, "branch target " + hex8(target) + " is not inside any translated function");
    }
    std::string fn = tk.kind == Target::Secondary ? sec_name(tk.owner, target) : callee(p, target);
    if (lk && s.h && s.h->leaf_call) {   // partial sync unless native code runs at the entry of a function in the callee's call tree
        const std::string lr = "c.lr = 0x" + hex8(pc + 4) + "u; ";
        return "if constexpr (" + plain_tree_name(s.h->leaf_tree) + ") { " + lr + s.flush + fn + "(ctx); " + s.reload + "} else { " + lr + s.h->full_flush +
               fn + "(ctx); " + s.h->full_reload + s.h->after_call + "}";
    }
    if (lk) return "c.lr = 0x" + hex8(pc + 4) + "u; " + s.flush + fn + "(" + s.cx + ");" + (s.reload.empty() ? "" : " " + s.reload);
    return s.flush + fn + "(" + s.cx + "); return;";
}

// Branch condition from a compare that was not written to CR: bit 0..3 of BI = LT GT EQ SO of that compare.
static std::string fused_cond(const FusedCmp& cmp, uint32_t bo, uint32_t bi) {
    const unsigned bit = bi & 3;
    std::string e;
    if (cmp.equal_op) e = "(" + cmp.lhs + " " + cmp.equal_op + " " + cmp.rhs + ")";   // false for a NaN operand, as the bit cror builds
    else if (bit == 3) e = cmp.fp ? "(" + cmp.lhs + " != " + cmp.lhs + " || " + cmp.rhs + " != " + cmp.rhs + ")" : "c.xer_so != 0";
    else e = "(" + cmp.lhs + (bit == 0 ? " < " : bit == 1 ? " > " : " == ") + cmp.rhs + ")";   // all false for a NaN operand, as fcmp
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
                o += name == "lmw" ? " c.r[" + num(i) + "] = ppc::rd32(c, " + at + ");"
                                   : std::string(hints && hints->framed ? " ppc::wr32_framed(c, " : " ppc::wr32(c, ") + at + ", c.r[" + num(i) + "]);";
            }
            return o;
        }
    }
    std::string a = op_args(*op, w);
    if (op->rc) a += (a.empty() ? "" : ", ") + std::string(ppc::rc(w) && !(hints && hints->drop_rc) ? "true" : "false");
    const std::string text = "ppc::op_" + name + (hints && hints->io ? "<true>" : hints && (hints->framed || hints->grouped) ? "<false, true>" : "") + "(c" + (a.empty() ? "" : ", " + a) + ");";
    if (hints && hints->fifo) {   // a store of a FIFO run (mark_fifo_runs)
        const std::string t = num(ppc::t(w));
        const std::string value = name == "stfs" ? "ppc::single_bits(c.f[" + t + "])" : "c.r[" + t + "]";
        const std::string decide = hints->fifo == 1 ? "fifo_ = uc::gx_fifo_burst(c.r[" + num(ppc::a(w)) + "] + uint32_t(" + num(ppc::simm(w)) + "), " +
                                                          num(hints->fifo_bytes) + "u, " + num(hints->fifo_stores) + "u); " : "";
        return decide + "if (fifo_) uc::gx_fifo_put(" + std::string(name == "stb" ? "1" : name == "sth" ? "2" : "4") + ", " + value + "); else " + text;
    }
    return text;
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

// The CR fields an mfcr rT really reads: all of them, unless the next instruction is rlwinm rT,rT,sh,mb,me, which overwrites rT with the
// bits under its mask (rT's whole mfcr value dies there, so only the fields those bits come from are read).
uint8_t mfcr_fields(const Program& p, uint32_t pc, uint32_t w) {
    uint32_t next = 0;
    const ppc::OpInfo* nop = p.img.word(pc + 4, next) ? ppc::decode(next) : nullptr;
    if (!nop || std::string(nop->name) != "rlwinm" || ppc::t(next) != ppc::t(w) || ppc::a(next) != ppc::t(w)) return kAllCr;
    const uint32_t mb = ppc::mb(next), me = ppc::me(next), sh = ppc::sh(next) & 31;
    const uint32_t mask = mb <= me ? (0xFFFFFFFFu >> mb) & (0xFFFFFFFFu << (31 - me)) : ((0xFFFFFFFFu >> mb) | (0xFFFFFFFFu << (31 - me)));
    const uint32_t bits = sh ? (mask >> sh) | (mask << (32 - sh)) : mask;   // the CR word bits that survive the rotate and mask
    uint8_t used = 0;
    for (unsigned i = 0; i < 8; ++i) if (bits & (0xFu << (28 - 4 * i))) used |= fld(i);
    return used;
}
// The register an integer record-form instruction compares with zero (cr0 = its sign, and XER[SO]), -1 for forms this does not know.
int rc_dest(const std::string& n, uint32_t w) {
    static const std::set<std::string> arithmetic{"add", "addc", "adde", "addze", "addme", "subf", "subfc", "subfe", "subfze", "subfme", "neg", "mullw", "mulhw", "mulhwu", "divw", "divwu"};
    static const std::set<std::string> logical{"and", "andc", "or", "orc", "xor", "nor", "nand", "eqv", "slw", "srw", "sraw", "srawi", "cntlzw", "extsb", "extsh", "rlwinm", "rlwnm", "rlwimi"};
    if (arithmetic.count(n)) return int(ppc::t(w));   // rD
    if (logical.count(n)) return int(ppc::a(w));      // rA
    return -1;
}

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
        if (name == "mfcr") nd.use = mfcr_fields(p, pc, w);
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
    RegSet pass;               // a call of a closed function: dirty registers its call tree neither loads nor stores (they stay dirty across it)
    RegSet call_writes;        // the registers the call can change in the real context (all, unless a closed function)
    bool tail = false;         // the exit is a jump into another function
    uint32_t callee = 0;       // the entry a direct call or tail jump goes to (0: indirect, sc, or not a function entry)
};

RegSet cached_mask() { RegSet m; for (int i = 0; i < res::COUNT; ++i) m.set(size_t(i)); return m; }
// What an interrupt poll stores: the small-data bases the interrupt context takes from the interrupted thread (Sched::deliver_events).
// Interrupts run on their own context and a thread switch keeps this thread's host stack, so no other register needs to be current there
// (a rollback checkpoint takes other threads only while they are parked in an SDK call, a full sync point).
RegSet poll_mask() { RegSet m; m.set(size_t(res::R + 2)); m.set(size_t(res::R + 13)); return m; }

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
                 const RegEffect* replaced, const std::unordered_map<uint32_t, LeafRegs>* leaves) {
    using ppc::Kind;
    RegNode nd;
    auto index = [&](uint32_t t) { return int((t - f.entry) / 4); };
    const ppc::OpInfo* op = ppc::decode(w);
    if (comment) return nd;   // a dead or folded compare: no code
    if (replaced) { nd.eff = *replaced; return nd; }   // straight-line code of a fused sequence (FnPlan::code)
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
        if (tk.kind == Target::Entry || (tk.kind == Target::Internal && target == f.entry)) nd.callee = target;
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
    // straight-line code of a fused sequence, emitted instead of the instruction (its register effect in code_eff): a compare's operands
    // copied for a later branch, the GPR bit of a folded fcmp/mfcr/rlwinm, an andi./andis. without its CR write
    std::vector<std::string> code;
    std::vector<RegEffect> code_eff;
    std::vector<std::string> group_note;   // the page marks of a store group, at its first store (mark_store_groups)
    uint8_t cf_used = 0, cf_b_used = 0;   // CR fields whose compare operands wait in cf<field>a (and cf<field>b) for a later branch
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
    auto& code = plan.code; code.assign(n, std::string());
    auto& code_eff = plan.code_eff; code_eff.assign(n, RegEffect{});
    plan.cf_used = plan.cf_b_used = 0;
    for (size_t i = 0; i < n; ++i) p.img.word(f.entry + uint32_t(i) * 4, words[i]);
    uint32_t last = 0;
    p.img.word(f.end - 4, last);
    plan.tail_end = !is_terminator_word(last) && p.an.is_entry(f.end);
    const std::vector<uint8_t>* after = nullptr;   // CR fields still readable after each instruction
    if (live) { auto it = live->out.find(f.entry); if (it != live->out.end()) after = &it->second; }
    if (!after) return;
    // instruction j as part of a straight line from an earlier one: inside the function and not a label (no other path joins there)
    auto inline_op = [&](size_t j) -> const ppc::OpInfo* {
        return j < n && !labels.count(f.entry + uint32_t(j) * 4) ? ppc::decode(words[j]) : nullptr;
    };
    auto plain_op = [&](const ppc::OpInfo* o, uint32_t w) {   // emitted as its ppc::op_* (not a trap)
        return o && o->kind != ppc::Kind::UNSUP && !(o->sub == ppc::Sub::X9 && ppc::oe(w)) && !((std::string(o->name) == "twi" || std::string(o->name) == "tw") && ppc::t(w) == 31);
    };
    // instruction j is a plain conditional branch (no CTR, no link) on `field`, reached only from the instruction before it, and the
    // field's last reader
    auto last_reader = [&](size_t j, uint32_t field) {
        const ppc::OpInfo* b = inline_op(j);
        const uint32_t w = j < n ? words[j] : 0;
        return b && (b->kind == ppc::Kind::BC || b->kind == ppc::Kind::BCLR) && !ppc::lk(w) && (ppc::bo(w) & 4) && !(ppc::bo(w) & 16) &&
               (ppc::bi(w) >> 2) == field && !((*after)[j] & fld(field)) && !hints[j].fused;
    };
    auto fuse = [&](size_t j, FusedCmp cmp, RegSet use) { fused[j] = std::move(cmp); fused_use[j] = use; hints[j].fused = &fused[j]; };
    auto regs = [](std::initializer_list<int> list) { RegSet u; for (int r : list) u.set(size_t(r)); return u; };
    std::vector<char> claimed(n, 0);   // a later instruction of a sequence an earlier one folded
    for (size_t i = 0; i < n; ++i) {
        if (claimed[i]) continue;
        const uint32_t w = words[i];
        const ppc::OpInfo* op = ppc::decode(w);
        if (!plain_op(op, w)) continue;
        const uint8_t live_after = (*after)[i];
        const std::string name = op->name;
        if (name == "cmp" || name == "cmpl" || name == "cmpi" || name == "cmpli") {
            const uint32_t field = ppc::crfd(w);
            const bool is_signed = name == "cmp" || name == "cmpi", reg_rhs = name == "cmp" || name == "cmpl";
            const std::string imm = is_signed ? num(ppc::simm(w)) : num(ppc::uimm(w)) + "u";
            if (!(live_after & fld(field))) { comment[i] = "/* cr" + num(field) + " is never read */"; ++st.cr_dead_compares; }
            else if (last_reader(i + 1, field)) {
                // the only reader is the next branch: test the operands there and skip the CR write
                const std::string a = "c.r[" + num(ppc::a(w)) + "]";
                const std::string lhs = is_signed ? "int32_t(" + a + ")" : a;
                const std::string rhs = reg_rhs ? (is_signed ? "int32_t(c.r[" + num(ppc::b(w)) + "])" : "c.r[" + num(ppc::b(w)) + "]") : imm;
                RegSet u = regs({res::R + int(ppc::a(w)), res::SO});
                if (reg_rhs) u.set(size_t(res::R + int(ppc::b(w))));
                fuse(i + 1, {lhs, rhs}, u);
                comment[i] = "/* cr" + num(field) + " compare is folded into the next branch */";
                ++st.cr_fused_branches;
            } else {
                // The branch that reads the field comes a few instructions later (the scheduler moved work in between): up to 8 straight-line
                // instructions that neither read nor write the field, branch, call or trap. The compare copies its operands into locals and the
                // branch compares those. Not for a branch on SO: that bit is XER[SO] at the compare, which an instruction between may change.
                size_t reader = 0;
                for (size_t j = i + 1; j <= i + 9; ++j) {
                    const ppc::OpInfo* o2 = inline_op(j);
                    if (!o2) break;
                    if (o2->kind == ppc::Kind::BC || o2->kind == ppc::Kind::BCLR) {
                        if (j > i + 1 && last_reader(j, field) && (ppc::bi(words[j]) & 3) != 3) reader = j;
                        break;
                    }
                    if (j == i + 9 || !plain_op(o2, words[j]) || o2->kind == ppc::Kind::BR || o2->kind == ppc::Kind::BCCTR || o2->kind == ppc::Kind::SC) break;
                    const CrEffect fx = cr_effect(*o2, words[j]);
                    if (((fx.use | fx.def) & fld(field)) || std::string(o2->name) == "mfcr") break;
                }
                if (reader) {
                    const std::string var = "cf" + num(field), a = var + "a", b = var + "b";
                    plan.cf_used |= fld(field);
                    if (reg_rhs) plan.cf_b_used |= fld(field);
                    code[i] = a + " = c.r[" + num(ppc::a(w)) + "];" + (reg_rhs ? " " + b + " = c.r[" + num(ppc::b(w)) + "];" : "");
                    code_eff[i].use = regs({res::R + int(ppc::a(w))});
                    if (reg_rhs) code_eff[i].use.set(size_t(res::R + int(ppc::b(w))));
                    const std::string rhs = reg_rhs ? (is_signed ? "int32_t(" + b + ")" : b) : imm;
                    fuse(reader, {is_signed ? "int32_t(" + a + ")" : a, rhs}, RegSet());
                    ++st.cr_fused_branches; ++st.cr_delayed;
                }
            }
        } else if (name == "fcmpu" || name == "fcmpo") {
            // fcmp writes only its CR field (ppc_rt.h fcmp: LT, GT or EQ, or SO alone for a NaN operand; no FPSCR bit). The C++ comparisons of
            // the two doubles give the same bits (each is false for a NaN operand), so a branch, a cror or an mfcr bit that reads it can test
            // the operands instead.
            const uint32_t field = ppc::crfd(w);
            const std::string lhs = "c.f[" + num(ppc::a(w)) + "]", rhs = "c.f[" + num(ppc::b(w)) + "]";
            const RegSet u = regs({res::F + int(ppc::a(w)), res::F + int(ppc::b(w))});
            const ppc::OpInfo* o1 = inline_op(i + 1);
            const uint32_t n1 = i + 1 < n ? words[i + 1] : 0;
            // cror EQ, LT|GT, EQ of this field: EQ becomes "<=" (or ">=")
            const bool is_cror = o1 && std::string(o1->name) == "cror" && ppc::t(n1) == 4 * field + 2 && ppc::b(n1) == 4 * field + 2 &&
                                 (ppc::a(n1) == 4 * field || ppc::a(n1) == 4 * field + 1);
            const char* const cror_op = is_cror ? (ppc::a(n1) == 4 * field ? "<=" : ">=") : nullptr;
            // [cror] mfcr rT; rlwinm[.] rT,rT,sh,31,31 taking one bit of this field: rT = that comparison as 0 or 1
            const size_t m = i + (is_cror ? 2 : 1), rl = m + 1;
            const ppc::OpInfo* o3 = inline_op(m);
            const ppc::OpInfo* o4 = o3 && std::string(o3->name) == "mfcr" ? inline_op(rl) : nullptr;
            const uint32_t n3 = m < n ? words[m] : 0, n4 = rl < n ? words[rl] : 0;
            const uint32_t bit = (ppc::sh(n4) + 31) & 31;   // the CR bit that the rotate brings to bit 31
            const char* bit_op = nullptr;
            if (o4 && std::string(o4->name) == "rlwinm" && ppc::t(n4) == ppc::t(n3) && ppc::a(n4) == ppc::t(n3) && ppc::mb(n4) == 31 && ppc::me(n4) == 31 && bit / 4 == field) {
                if (cror_op) { if (bit == 4 * field + 2) bit_op = cror_op; }
                else if (bit % 4 == 0) bit_op = "<"; else if (bit % 4 == 1) bit_op = ">"; else if (bit % 4 == 2) bit_op = "==";
            }
            const bool bit_rc = bit_op && ppc::rc(n4);
            // the field is not read after the sequence (rlwinm. rewrites cr0 as a whole)
            const bool field_dead = bit_op && ((bit_rc && field == 0) || !((*after)[rl] & fld(field)));
            if (!(live_after & fld(field))) { comment[i] = "/* cr" + num(field) + " is never read */"; ++st.cr_dead_compares; }
            else if (bit_op && field_dead) {
                const uint32_t T = ppc::t(n3);
                const std::string reg = "c.r[" + num(T) + "]";
                const bool cr0_live = (*after)[rl] & fld(0);
                const bool cr0_branch = bit_rc && cr0_live && last_reader(rl + 1, 0);   // the record form's only reader is the next branch
                code[rl] = reg + " = uint32_t(" + lhs + " " + bit_op + " " + rhs + ");";
                code_eff[rl].use = u;
                code_eff[rl].may.set(size_t(res::R + int(T))); code_eff[rl].must.set(size_t(res::R + int(T)));
                if (bit_rc && cr0_live && !cr0_branch) {
                    code[rl] += " ppc::cr0(c, " + reg + ");";
                    code_eff[rl].use.set(size_t(res::SO));
                    code_eff[rl].may.set(size_t(res::CR)); code_eff[rl].must.set(size_t(res::CR));
                } else if (bit_rc) ++st.cr_dropped_records;
                if (cr0_branch) fuse(rl + 1, {"int32_t(" + reg + ")", "0"}, regs({res::R + int(T), res::SO}));
                comment[i] = "/* cr" + num(field) + " compare is folded into the bit test below */";
                if (is_cror) { comment[i + 1] = "/* cror is folded into the bit test below */"; claimed[i + 1] = 1; }
                comment[m] = "/* mfcr is folded into the bit test below */";
                claimed[m] = claimed[rl] = 1;
                ++st.cr_fused_branches; ++st.cr_bit_tests;
            } else if (last_reader(i + 1, field)) {
                fuse(i + 1, {lhs, rhs, true, nullptr}, u);
                comment[i] = "/* cr" + num(field) + " compare is folded into the next branch */";
                ++st.cr_fused_branches; ++st.cr_fused_fp;
            } else if (is_cror && last_reader(i + 2, field) && (ppc::bi(words[i + 2]) & 3) == 2) {
                // fcmp, cror, branch on EQ: one <= (or >=) test; the field is dead after the branch
                fuse(i + 2, {lhs, rhs, true, cror_op}, u);
                comment[i] = "/* cr" + num(field) + " compare, cror and branch are folded into one test */";
                comment[i + 1] = "/* cror is folded into the next branch */";
                claimed[i + 1] = 1;
                ++st.cr_fused_branches; ++st.cr_fused_fp;
            }
        } else if ((name == "andi_" || name == "andis_" || (op->rc && ppc::rc(w) && rc_dest(name, w) >= 0)) && (live_after & fld(0)) && last_reader(i + 1, 0)) {
            // an integer record form whose only reader is the next branch: the branch compares the result register with zero (cr0 is its sign
            // and XER[SO], which nothing between can change)
            const bool andi = name == "andi_" || name == "andis_";
            const int D = andi ? int(ppc::a(w)) : rc_dest(name, w);
            if (andi) {   // always a record form: written as the plain AND
                const std::string imm = num(name == "andi_" ? ppc::uimm(w) : ppc::uimm(w) << 16) + "u";
                code[i] = "c.r[" + num(D) + "] = c.r[" + num(ppc::t(w)) + "] & " + imm + ";";
                code_eff[i].use = regs({res::R + int(ppc::t(w))});
                code_eff[i].may.set(size_t(res::R + D)); code_eff[i].must.set(size_t(res::R + D));
            } else hints[i].drop_rc = true;
            fuse(i + 1, {"int32_t(c.r[" + num(D) + "])", "0"}, regs({res::R + D, res::SO}));
            ++st.cr_dropped_records; ++st.cr_fused_branches; ++st.cr_record_branches;
        } else if (op->rc && ppc::rc(w) && !(live_after & fld(fp_name(name) ? 1 : 0))) {
            hints[i].drop_rc = true;
            ++st.cr_dropped_records;
        }
    }
}



// FIFO runs: two or more FIFO stores (stb/sth/stw/stfs from a hardware-register base, all with the same base register and offset) in one
// block, with only register operations and plain loads between them (no label, branch, call, trap, other store, cache operation, MMIO
// access, SPR write but LR/CTR/XER, or write of the base register). The run's first store decides all of them once (uc::gx_fifo_burst:
// the FIFO port with room for every byte of the run); nothing between the stores can reach the FIFO or move its window, so each store then
// appends in place what its own store would have appended. Returns whether the function has a run (it declares the decision variable).
static bool mark_fifo_runs(const Function& f, FnPlan& plan, EmitStats& st) {
    auto fifo_store = [&](size_t i) -> uint32_t {   // the store's size when it is a FIFO candidate
        if (!plan.comment[i].empty() || !plan.hints[i].io) return 0;
        const uint32_t w = plan.words[i];
        const ppc::OpInfo* op = ppc::decode(w);
        if (!op || ppc::a(w) == 0) return 0;
        const std::string n = op->name;
        return n == "stb" ? 1 : n == "sth" ? 2 : n == "stw" || n == "stfs" ? 4 : 0;
    };
    auto between = [&](size_t i, uint32_t base) {
        if (!plan.comment[i].empty()) return true;   // a folded compare: no code
        const uint32_t w = plan.words[i];
        const ppc::OpInfo* op = ppc::decode(w);
        using ppc::Kind;
        if (!op || plan.hints[i].io || op->kind == Kind::BR || op->kind == Kind::BC || op->kind == Kind::BCLR || op->kind == Kind::BCCTR ||
            op->kind == Kind::SC || op->kind == Kind::UNSUP || (op->sub == ppc::Sub::X9 && ppc::oe(w))) return false;
        const std::string n = op->name;
        if (n == "tw" || n == "twi" || n.compare(0, 2, "st") == 0 || n.compare(0, 6, "psq_st") == 0 || n.compare(0, 2, "dc") == 0 ||
            n.compare(0, 2, "ic") == 0 || n == "mtmsr" || (n == "mtspr" && ppc::spr(w) != 1 && ppc::spr(w) != 8 && ppc::spr(w) != 9)) return false;
        bool ok = false;
        const RegEffect e = reg_effect(*op, w, op->rc && ppc::rc(w) && !plan.hints[i].drop_rc, ok);
        return ok && !e.may.test(size_t(res::R + int(base)));
    };
    bool any = false;
    for (size_t i = 0; i < plan.n;) {
        const uint32_t first = fifo_store(i);
        if (!first) { ++i; continue; }
        const uint32_t base = ppc::a(plan.words[i]);
        const int32_t disp = ppc::simm(plan.words[i]);
        std::vector<size_t> run{i};
        uint32_t bytes = first;
        for (size_t j = i + 1; j < plan.n && !plan.labels.count(f.entry + uint32_t(j) * 4); ++j) {
            if (const uint32_t s = fifo_store(j); s && ppc::a(plan.words[j]) == base && ppc::simm(plan.words[j]) == disp) { run.push_back(j); bytes += s; continue; }
            if (!between(j, base)) break;
        }
        if (run.size() >= 2) {
            for (size_t k : run) { plan.hints[k].fifo = k == run.front() ? 1 : 2; plan.hints[k].fifo_bytes = bytes; plan.hints[k].fifo_stores = uint32_t(run.size()); }
            ++st.fifo_runs; st.fifo_run_stores += run.size();
            any = true;
        }
        i = run.back() + 1;
    }
    return any;
}

// Store groups: two or more plain stores (stw, sth, stb, stfs, stfd, psq_st) through one base register other than r0 and r1 (r1 stores are
// framed), in a straight line of at most 32 instructions with no label, and spanning at most 4096 bytes (so they touch the page of the span's
// first byte and the page of its last, and only those). The group's write barrier is ppc::note_stores at its first store: those two marks;
// its stores (ppc::op_*<false, true>) mark nothing. Between the stores only instructions that write no guest memory and call no native code
// pass: register operations and loads (a load through the MMIO window reads a plain register file, gx_fifo.h and mmio.h); no other store, no
// cache operation, no branch, call, trap or MMIO-marked access, and nothing that writes the base. So the write epoch (gx_write_watch.h, which
// only native code advances) is the same at the marks as at each store, and nothing reads the marks in between on this thread: the page
// table ends exactly as the per-store marks leave it. A group whose base is in the MMIO window stores to no RAM page (the window is far from
// RAM, and the group spans 4 KiB); its marks land on pages nothing caches.
static size_t store_size(const ppc::OpInfo* op, uint32_t w) {
    if (!op) return 0;
    const std::string n = op->name;
    if (n == "psq_st") return (w >> 15) & 1 ? 4 : 8;   // W = 1: ps0 only; quantized forms store fewer bytes from the same address
    return n == "stw" || n == "stfs" ? 4 : n == "stb" ? 1 : n == "sth" ? 2 : n == "stfd" ? 8 : 0;
}
static void mark_store_groups(const Function& f, FnPlan& plan, EmitStats& st) {
    plan.group_note.assign(plan.n, std::string());
    auto label = [&](size_t i) { return plan.labels.count(f.entry + uint32_t(i) * 4) != 0; };
    auto member = [&](size_t i) -> size_t {   // the store's size when it can be in a group
        if (!plan.comment[i].empty() || !plan.code[i].empty() || plan.hints[i].io || plan.hints[i].fifo || plan.hints[i].framed) return 0;
        const uint32_t w = plan.words[i];
        if (ppc::a(w) == 0 || ppc::a(w) == 1) return 0;
        return store_size(ppc::decode(w), w);
    };
    auto disp = [&](size_t i) { const uint32_t w = plan.words[i]; return (w >> 26) == 60 ? int32_t(w << 20) >> 20 : int32_t(ppc::simm(w)); };
    auto between = [&](size_t i, uint32_t base) {
        if (!plan.comment[i].empty()) return true;   // a folded compare: no code
        if (!plan.code[i].empty()) return !plan.code_eff[i].may.test(size_t(res::R + int(base)));   // register code of a fused sequence
        const uint32_t w = plan.words[i];
        const ppc::OpInfo* op = ppc::decode(w);
        using ppc::Kind;
        if (!op || plan.hints[i].io || plan.hints[i].fifo || op->kind == Kind::BR || op->kind == Kind::BC || op->kind == Kind::BCLR ||
            op->kind == Kind::BCCTR || op->kind == Kind::SC || op->kind == Kind::UNSUP || (op->sub == ppc::Sub::X9 && ppc::oe(w))) return false;
        const std::string n = op->name;
        if (n == "tw" || n == "twi" || n.compare(0, 2, "st") == 0 || n.compare(0, 6, "psq_st") == 0 || n.compare(0, 2, "dc") == 0 ||
            n.compare(0, 2, "ic") == 0) return false;
        bool ok = false;
        const RegEffect e = reg_effect(*op, w, op->rc && ppc::rc(w) && !plan.hints[i].drop_rc, ok);
        return ok && !e.may.test(size_t(res::R + int(base)));
    };
    for (size_t i = 0; i < plan.n;) {
        const size_t first = member(i);
        if (!first) { ++i; continue; }
        const uint32_t base = ppc::a(plan.words[i]);
        int32_t lo = disp(i), hi = disp(i) + int32_t(first);
        std::vector<size_t> group{i};
        for (size_t j = i + 1; j < plan.n && j - i < 32 && !label(j); ++j) {
            if (const size_t s = member(j); s && ppc::a(plan.words[j]) == base) {
                const int32_t nlo = std::min(lo, disp(j)), nhi = std::max(hi, disp(j) + int32_t(s));
                if (nhi - nlo > 4096) break;
                lo = nlo; hi = nhi; group.push_back(j);
                continue;
            }
            if (!between(j, base)) break;
        }
        if (group.size() >= 2) {
            for (size_t k : group) plan.hints[k].grouped = true;
            plan.group_note[i] = "ppc::note_stores(c, " + num(base) + ", " + num(lo) + ", " + num(hi - lo) + "u); ";
            ++st.store_groups; st.grouped_stores += group.size();
        }
        i = group.back() + 1;
    }
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
        for (size_t i = 0; i < n; ++i) nodes[i] = reg_node(p, f, f.entry + uint32_t(i) * 4, words[i], hints[i], fused_use[i], !comment[i].empty(), plan.code[i].empty() ? nullptr : &plan.code_eff[i], leaves);
        st.mmio_sites += mark_mmio_bases({&words, &comment}, nodes, hints);
    }
    const bool fifo_fn = mark_fifo_runs(f, plan, st);
    mark_store_groups(f, plan, st);

    // pass 2: register cache dataflow
    const bool local = p.reg_cache;
    RegSet entry_load;
    std::vector<RegSet> dirty_after(n + 1), live_in(n + 1);
    std::vector<std::string> fall_flush(n);   // flush on the fall-through edge into the next instruction
    std::vector<std::string> frame_at(n);     // a frame note at the start of the instruction (after its label)
    if (local) {
        std::vector<RegNode> nodes(n);
        for (size_t i = 0; i < n; ++i) nodes[i] = reg_node(p, f, f.entry + uint32_t(i) * 4, words[i], hints[i], fused_use[i], !comment[i].empty(), plan.code[i].empty() ? nullptr : &plan.code_eff[i], leaves);
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
                if (nd.exit) x |= dirty_after[i];
                if (nd.poll) x |= dirty_after[i] & poll_mask();
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
            const RegSet flush = nd.call ? dirty_after[i] & ~nd.pass : nd.poll ? dirty_after[i] & poll_mask() : dirty_after[i];
            if (nd.call || nd.exit || nd.poll) { hints[i].flush = copy_text(flush, true); st.reg_flushes += flush.count(); }
            if (nd.call) { hints[i].reload = copy_text(live_in[i + 1] & nd.call_writes, false); st.reg_reloads += (live_in[i + 1] & nd.call_writes).count(); }
            if (nd.call && nd.pass.any()) {
                ++st.reg_leaf_calls;
                hints[i].leaf_call = true;
                hints[i].leaf_tree = leaves->at(nd.callee).tree;
                hints[i].full_flush = copy_text(dirty_after[i], true);
                hints[i].full_reload = copy_text(live_in[i + 1] | (dirty_after[i] & nd.pass), false);   // copies the closed path keeps must stay current
            }
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
        // Frame notes. The r1-based D-form stores skip the per-store write barrier; the function marks the pages of [r1 + lo, r1 + hi) (every
        // such store lies in it) instead: at its entry and its secondary entries, after every call (a closed callee still polls), after every
        // instruction that writes r1, and in its polls' slow path. A rollback capture runs only while this thread sits at a safe point (a call
        // or a poll), so every page such a store writes is marked within the same stretch between two safe points, as the per-store barrier
        // did. stwu and the indexed forms keep their own barrier.
        int32_t lo = INT32_MAX, hi = INT32_MIN;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t w = words[i], opcd = w >> 26;
            if (!comment[i].empty() || hints[i].io || hints[i].fifo || ppc::a(w) != 1) continue;
            const int32_t size = opcd == 36 || opcd == 52 ? 4 : opcd == 38 ? 1 : opcd == 44 ? 2 : opcd == 54 || opcd == 60 ? 8
                               : opcd == 47 ? int32_t(4 * (32 - ppc::t(w))) : 0;   // stmw: rS..r31
            if (!size) continue;
            const int32_t d = opcd == 60 ? int32_t(w << 20) >> 20 : ppc::simm(w);   // psq_st: a 12-bit displacement
            hints[i].framed = true;
            lo = std::min(lo, d); hi = std::max(hi, d + size);
            ++st.framed_stores;
        }
        if (lo < hi) {
            // ahead[i]: a framed store can run from i on before the stretch ends (a call or a write of r1, after which the next instruction
            // marks again, or an exit); a mark is written only where one is. A loop's back edge continues the stretch when its poll does not fire.
            std::vector<char> ahead(n + 1, 0);
            for (bool changed = true; changed;) {
                changed = false;
                for (size_t k = n; k-- > 0;) {
                    const RegNode& nd = nodes[k];
                    bool a = hints[k].framed;
                    const bool ends = nd.call || nd.eff.may.test(1);
                    if (!a && nd.fall && !ends) a = ahead[k + 1];
                    if (!a && nd.target >= 0) a = ahead[size_t(nd.target)];
                    for (int t : nd.table) if (!a && t >= 0) a = ahead[size_t(t)];
                    if (a && !ahead[k]) { ahead[k] = 1; changed = true; }
                }
            }
            const std::string note = "ppc::frame_note(c, " + num(lo) + ", " + num(hi) + "); ";
            for (size_t i = 0; i < n; ++i) {
                if (nodes[i].poll && nodes[i].target >= 0 && ahead[size_t(nodes[i].target)]) hints[i].frame_note = note;
                if (!ahead[i]) continue;
                const uint32_t pc = f.entry + uint32_t(i) * 4;
                const bool entry = i == 0 || (has_sec && std::count(sit->second.begin(), sit->second.end(), pc));
                if (entry || nodes[i - 1].call || nodes[i - 1].eff.may.test(1)) { frame_at[i] = note; ++st.frame_notes; }
            }
        }
    }

    // pass 3: text
    std::string cf_decl;   // compare operands waiting for a later branch (plan_function)
    for (uint32_t field = 0; field < 8; ++field)
        if (plan.cf_used & fld(field)) cf_decl += "  uint32_t cf" + num(field) + "a" + (plan.cf_b_used & fld(field) ? ", cf" + num(field) + "b" : std::string()) + ";\n";
    std::ostringstream o;
    if (has_sec) o << "static void body_" << hex8(f.entry) << "(ppc::Ctx& c, uint32_t at) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n";
    else o << "void " << fn_name(f.entry) << "(ppc::Ctx& c) {\n  UC_ENTER(0x" << hex8(f.entry) << "u);\n";
    if (local) {
        // a resume through a secondary entry (rare) loads what is read from there on; the normal entry only what the entry reads
        o << "  ppc::Ctx& ctx = c;\n  {\n  ppc::Ctx c; c.mem = ctx.mem;\n";
        if (fifo_fn) o << "  bool fifo_ = false;\n";
        o << cf_decl;
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
    } else {
        if (fifo_fn) o << "  bool fifo_ = false;\n";
        o << cf_decl;
    }
    if (!local && has_sec) {
        o << "  switch (at) { ";
        for (uint32_t t : sit->second) o << "case 0x" << hex8(t) << "u: goto L_" << hex8(t) << "; ";
        o << "default: break; }\n";
    }
    for (size_t i = 0; i < n; ++i) {
        const uint32_t pc = f.entry + uint32_t(i) * 4;
        if (labels.count(pc)) o << "L_" << hex8(pc) << ":;\n";
        std::string code = comment[i].empty() ? plan.code[i] : comment[i];
        if (code.empty()) code = emit_insn(p, f, pc, words[i], st, &hints[i]);
        else ++st.instructions;
        code = frame_at[i] + plan.group_note[i] + code;
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

// The functions a call may sync with partially ("closed"): their whole call tree reaches no point where code other than this thread's
// translated code reads or replaces its context. Such points are an HLE call (an SDK call may park the thread for a rollback checkpoint,
// and the resume rebuilds the frames from the context through the guest LR chain; the main thread reloads after the call that restored a
// checkpoint), an indirect call, sc, and a jump into the middle of another function. A loop does not count (its poll, see poll_mask),
// nor a jump table, a direct tail jump or the fall into the next function when that function is closed too. A closed function's loads
// and stores of the real context are its entry loads, its flushes and those of its callees, all within the registers the tree reads or
// writes. The generated Leaf_ HLEs (runtime/hle/leaf_gen.cpp, written by tools/leaf_hle.py from these very instructions) qualify the same
// way: they copy in and out only registers their guest code uses. So do the native SDK matrix/vector routines below: they read the guest
// routine's arguments and write no register but its result (tests/test_sdk_math_hle.cpp checks both), and so do the two collision routines
// built on them, which write no register outside what their guest code writes (r0, r3, r11 and the ps1 rounding of the f26..f31 restores).
static bool leaf_hle(const std::string& name) {
    static const std::set<std::string> math = {"PSMTXIdentity", "PSMTXCopy", "PSMTXConcat", "PSMTXTransApply", "PSMTXScaleApply", "PSMTXMultVec",
        "PSVECAdd", "PSVECSubtract", "PSVECScale", "PSVECNormalize", "PSVECSquareMag", "PSVECMag", "PSQUATDotProduct", "PSVECCrossProduct",
        "ClosestPtTriangle", "SphereTriangleHit"};
    return name.compare(0, 5, "Leaf_") == 0 || math.count(name);
}
struct Closed {
    std::unordered_map<uint32_t, LeafRegs> fns;
    std::string plain;   // game_decl.h: uc_plain_tree_<component> = no native code at the entry of any function the component reaches
};
static Closed analyze_closed(const Program& p, const CrLive* live) {
    struct Candidate { LeafRegs regs; std::vector<uint32_t> callees; };
    std::map<uint32_t, Candidate> cand;   // ordered: deterministic output
    EmitStats scratch;
    for (const auto& f : p.an.functions) {
        if (f.end <= f.entry) continue;
        if (auto h = p.cfg.hle.find(f.entry); h != p.cfg.hle.end() && !leaf_hle(h->second)) continue;
        FnPlan plan;
        plan_function(p, f, live, scratch, plan);
        Candidate c;
        bool ok = true;
        if (plan.tail_end) { if (p.cfg.hle.count(f.entry) || !p.an.is_entry(f.end)) ok = false; else c.callees.push_back(f.end); }
        for (size_t i = 0; i < plan.n && ok; ++i) {
            const RegNode nd = reg_node(p, f, f.entry + uint32_t(i) * 4, plan.words[i], plan.hints[i], plan.fused_use[i], !plan.comment[i].empty(), plan.code[i].empty() ? nullptr : &plan.code_eff[i], nullptr);
            if (nd.call || nd.tail) { if (nd.callee) c.callees.push_back(nd.callee); else ok = false; }
            c.regs.touched |= nd.eff.use | nd.eff.may;
            c.regs.written |= nd.eff.may;
        }
        if (ok) cand.emplace(f.entry, std::move(c));
    }
    // drop every function that reaches one that is not closed
    for (bool changed = true; changed;) {
        changed = false;
        for (auto it = cand.begin(); it != cand.end();) {
            const bool open = std::any_of(it->second.callees.begin(), it->second.callees.end(), [&](uint32_t t) { return !cand.count(t); });
            if (open) { it = cand.erase(it); changed = true; } else ++it;
        }
    }
    for (bool changed = true; changed;) {   // summaries over the call tree
        changed = false;
        for (auto& [entry, c] : cand)
            for (uint32_t t : c.callees) {
                const LeafRegs& d = cand.at(t).regs;
                const LeafRegs before = c.regs;
                c.regs.touched |= d.touched; c.regs.written |= d.written;
                changed |= before.touched != c.regs.touched || before.written != c.regs.written;
            }
    }
    // components of the closed call graph (Tarjan: a component comes out after every component it calls), one plain constant each
    Closed out;
    std::unordered_map<uint32_t, int> index, low;
    std::unordered_map<uint32_t, uint32_t> comp;   // function -> component (its smallest member)
    std::vector<uint32_t> stack;
    std::unordered_map<uint32_t, bool> on;
    int next = 0;
    std::function<void(uint32_t)> visit = [&](uint32_t v) {
        index[v] = low[v] = next++;
        stack.push_back(v); on[v] = true;
        for (uint32_t w : cand.at(v).callees) {
            if (!index.count(w)) { visit(w); low[v] = std::min(low[v], low[w]); }
            else if (on[w]) low[v] = std::min(low[v], index[w]);
        }
        if (low[v] != index[v]) return;
        std::vector<uint32_t> members;
        for (uint32_t w = 0; w != v;) { w = stack.back(); stack.pop_back(); on[w] = false; members.push_back(w); }
        std::sort(members.begin(), members.end());
        for (uint32_t m : members) comp[m] = members.front();
        std::set<uint32_t> reached;
        std::string e;
        for (uint32_t m : members) {
            e += (e.empty() ? "" : " && ") + std::string("UC_PLAIN_ENTRY(0x") + hex8(m) + "u)";
            for (uint32_t w : cand.at(m).callees) if (comp.at(w) != members.front()) reached.insert(comp.at(w));
        }
        for (uint32_t r : reached) e += " && " + plain_tree_name(r);
        out.plain += "inline constexpr bool " + plain_tree_name(members.front()) + " = " + e + ";\n";
    };
    for (const auto& [entry, c] : cand) if (!index.count(entry)) visit(entry);
    for (auto& [entry, c] : cand) { c.regs.tree = comp.at(entry); out.fns.emplace(entry, c.regs); }
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
    const CrLive live = p.cr_opt ? analyze_cr(p) : CrLive{};
    const Closed closed = p.reg_cache ? analyze_closed(p, p.cr_opt ? &live : nullptr) : Closed{};
    const auto& leaves = closed.fns;
    st.reg_leaves = leaves.size();
    decl += closed.plain;
    out.files.push_back({"game_decl.h", decl});

    size_t idx = 0, in_cur = 0;
    std::string cur;
    auto flush = [&] {
        if (cur.empty()) return;
        char nm[32]; std::snprintf(nm, sizeof nm, "game_%03zu.cpp", idx++);
        out.files.push_back({nm, "#include \"game_decl.h\"\n" + cur});
        cur.clear(); in_cur = 0;
    };
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

// Resume points: the return address of every call a parked SDK call can be inside (Sched::restore rebuilds such a thread from its context
// through the guest LR chain, calling these entries). A call of a closed function or of a save/restore helper cannot park, so its return
// address gets none, unless the analyzer already made it an entry (a jump from another function).
void add_resume_entries(Program& p) {
    const auto jumped = p.an.secondary;
    struct Call { uint32_t owner, ret, target; };
    std::vector<Call> direct;
    for (const auto& f : p.an.functions) {
        if (p.cfg.hle.count(f.entry)) continue;
        for (uint32_t pc = f.entry; pc + 4 < f.end; pc += 4) {
            uint32_t w = 0;
            if (!p.img.word(pc, w) || !ppc::lk(w)) continue;
            const auto* op = ppc::decode(w);
            if (!op || (op->kind != ppc::Kind::BR && op->kind != ppc::Kind::BC && op->kind != ppc::Kind::BCLR && op->kind != ppc::Kind::BCCTR)) continue;
            p.an.secondary[f.entry].insert(pc + 4);
            if (op->kind == ppc::Kind::BR || op->kind == ppc::Kind::BC) {
                const int32_t d = op->kind == ppc::Kind::BR ? ppc::li(w) : ppc::bd(w);
                direct.push_back({f.entry, pc + 4, ppc::aa(w) ? uint32_t(d) : pc + uint32_t(d)});
            }
        }
    }
    const CrLive live = p.cr_opt ? analyze_cr(p) : CrLive{};
    const Closed closed = analyze_closed(p, p.cr_opt ? &live : nullptr);
    for (const Call& c : direct) {
        if (!closed.fns.count(c.target) && !reg_helper(p, c.target).kind) continue;
        if (auto j = jumped.find(c.owner); j != jumped.end() && j->second.count(c.ret)) continue;
        auto& entries = p.an.secondary[c.owner];
        entries.erase(c.ret);
        if (entries.empty()) p.an.secondary.erase(c.owner);
    }
}

}  // namespace uc
