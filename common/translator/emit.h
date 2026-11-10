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
    // parts of cr_fused_branches: fcmp folded into a branch (with or without cror), fcmp [cror] mfcr rlwinm folded into one GPR bit, an
    // integer compare whose branch comes a few instructions later (operands in cf<field>a/b), a record form tested by the next branch
    size_t cr_fused_fp = 0, cr_bit_tests = 0, cr_delayed = 0, cr_record_branches = 0;
    size_t reg_entry_loads = 0, reg_flushes = 0, reg_reloads = 0;                  // register-cache copies emitted
    size_t reg_leaf_calls = 0, reg_leaves = 0;                                     // calls that sync only what a leaf callee touches, such callees
    size_t mmio_sites = 0;                                                         // loads/stores from a hardware-register base (explicit MMIO test)
    size_t framed_stores = 0, frame_notes = 0;                                    // r1 stores without a per-store barrier, the frame notes that cover them
    size_t fifo_runs = 0, fifo_run_stores = 0;                                     // runs of FIFO stores decided once (uc::gx_fifo_burst), their stores
    size_t store_groups = 0, grouped_stores = 0;                                   // runs of stores through one base whose pages are marked once, their stores
};

struct Program {
    Image img{};
    Analysis an;
    Config cfg;
    bool cr_opt = true;   // drop dead CR writes and fuse compare+branch (translator --no-cr-opt turns it off)
    bool count_insns = false;   // diagnostic: count every executed guest instruction in ppc::g_guest_insns (translator --count-insns)
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
struct FusedCmp {   // the compare feeding a conditional branch, as C++ operands
    std::string lhs, rhs;
    bool fp = false;                  // a floating-point compare: CR bit 3 is "unordered" (a NaN operand), not XER[SO]
    const char* equal_op = nullptr;   // fcmp + cror (LT|EQ or GT|EQ) + branch on EQ: the operator that replaces bit 2 ("<=" or ">=")
};
struct CrHints {
    bool drop_rc = false;                 // the record form's CR write is never read
    const FusedCmp* fused = nullptr;      // a conditional branch tests this compare directly instead of reading CR
    // Register cache (Program::reg_cache): `c` is the function's local copy of the guest registers and `ctx` the real context. Before a call,
    // a return or a trap the instruction writes `flush` (local -> ctx); after a call or an interrupt poll it writes `reload` (ctx -> local).
    bool local = false;
    std::string flush, reload;
    std::map<uint32_t, std::string> edge;   // flush on the taken edge to an internal label (where the paths that meet there disagree)
    std::string not_taken;                  // a conditional call: flush on the path that skips the call
    // A call of a closed function: flush/reload sync only what its call tree touches; when native code runs at the entry of a function in
    // that tree (game_decl.h's uc_plain_tree_<leaf_tree> false) the call takes the full_flush/full_reload form instead. after_call: an edge
    // flush on the call's own path (both forms).
    bool leaf_call = false;
    uint32_t leaf_tree = 0;
    std::string full_flush, full_reload, after_call;
    bool io = false;   // a load/store whose base register may hold a hardware-register address: ppc::op_*<true> (explicit MMIO test)
    // An r1-based D-form store of a function that marks its frame (emit_function, frame notes): ppc::op_*<false, true>, no per-store barrier.
    // frame_note: the mark (ppc::frame_note) the instruction's interrupt poll writes after the poll's slow path.
    bool framed = false;
    std::string frame_note;
    // A store of a store group (emit.cpp, mark_store_groups): a plain store through a base other than r1 whose pages ppc::note_stores marked
    // before the group's first store; also ppc::op_*<false, true>.
    bool grouped = false;
    // A store of a FIFO run (emit.cpp, fifo_runs): 1 = the run's first store, which decides the whole run (uc::gx_fifo_burst over
    // fifo_bytes bytes in fifo_stores stores), 2 = a later one; each appends in place when the run was taken.
    int fifo = 0;
    uint32_t fifo_bytes = 0, fifo_stores = 0;
};

std::string emit_insn(const Program& p, const Function& f, uint32_t pc, uint32_t word, EmitStats& st, const CrHints* hints = nullptr);
// leaves: the leaf functions a call may sync with partially (emit_program computes them; without, every call syncs everything)
std::string emit_function(const Program& p, const Function& f, EmitStats& st, const CrLive* live = nullptr,
                          const std::unordered_map<uint32_t, LeafRegs>* leaves = nullptr);
Output emit_program(const Program& p, size_t funcs_per_shard, EmitStats& st);

}  // namespace uc
