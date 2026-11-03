#pragma once
#include "analyzer/analyzer.h"
#include "translator/regeffect.h"
#include <cstdint>
#include <string>
#include <map>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uc {

struct EmitStats {
    size_t functions = 0, instructions = 0, invalid = 0, traps_midfunc = 0, unresolved_bctr = 0, oe_traps = 0, unsupported = 0;
    size_t indirect_tail = 0, secondary_entries = 0, fallthrough_tail = 0;
    size_t cr_dead_compares = 0, cr_fused_branches = 0, cr_dropped_records = 0;   // condition-register writes that were never read
    size_t reg_entry_loads = 0, reg_flushes = 0, reg_reloads = 0;                  // register-cache copies emitted
    size_t reg_leaf_calls = 0, reg_leaves = 0;                                     // calls that sync only what a leaf callee touches, such callees
};

struct Program {
    Image img{};
    Analysis an;
    Config cfg;
    bool cr_opt = true;   // drop dead CR writes and fuse compare+branch (translator --no-cr-opt turns it off)
    bool reg_cache = true;   // keep guest registers in function-local copies, synced at calls and exits (translator --no-reg-cache turns it off)
};

struct Output {
    std::vector<std::pair<std::string, std::string>> files;  // name -> content
};

// Resume a parked SDK call through the guest LR chain after its native stack unwinds.
void add_resume_entries(Program& p);

// Condition-register liveness. out[function entry][i] = CR fields (bit f = cr<f>) that may still be read after instruction i of the function.
struct CrLive {
    std::unordered_map<uint32_t, std::vector<uint8_t>> out;
};
CrLive analyze_cr(const Program& p);

// What the emitter may drop or fuse for one instruction (from CrLive); the default emits every CR write.
struct FusedCmp { std::string lhs, rhs; };   // the compare feeding the next conditional branch, as C++ operands
struct CrHints {
    bool drop_rc = false;                 // the record form's CR write is never read
    const FusedCmp* fused = nullptr;      // a conditional branch tests this compare directly instead of reading CR
    // Register cache (Program::reg_cache): `c` is the function's local copy of the guest registers and `ctx` the real context. Before a call,
    // a return or a trap the instruction writes `flush` (local -> ctx); after a call or an interrupt poll it writes `reload` (ctx -> local).
    bool local = false;
    std::string flush, reload;
    std::map<uint32_t, std::string> edge;   // flush on the taken edge to an internal label (where the paths that meet there disagree)
    std::string not_taken;                  // a conditional call: flush on the path that skips the call
    // A call of a leaf function: flush/reload sync only what it touches; when native code runs at its entry (UC_PLAIN_ENTRY false) the
    // call takes the full_flush/full_reload form instead. after_call: an edge flush on the call's own path (both forms).
    bool leaf_call = false;
    std::string full_flush, full_reload, after_call;
};

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t word, EmitStats& st, const CrHints* hints = nullptr);
// leaves: the leaf functions a call may sync with partially (emit_program computes them; without, every call syncs everything)
std::string emit_function(const Program& p, const Function& f, EmitStats& st, const CrLive* live = nullptr,
                          const std::unordered_map<uint32_t, LeafRegs>* leaves = nullptr);
Output emit_program(const Program& p, size_t funcs_per_shard, EmitStats& st);

}  // namespace uc
