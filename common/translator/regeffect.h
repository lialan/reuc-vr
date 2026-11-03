#pragma once
// Guest register state that the translator keeps in function-local copies (emit.cpp, "register cache"): which of it one instruction reads
// and writes. tests/test_reg_effects.cpp checks this model against the runtime's op_* implementations.
#include "ppc/isa.h"
#include <bitset>
#include <cstdint>
#include <string>

namespace uc {

// One bit per cached piece of ppc::Ctx (POLL: poll_count, counted by loop back edges). msr, spr[], native_depth and mem are not cached (the emitter accesses them in the real
// context or keeps them constant).
namespace res {
constexpr int R = 0, F = 32, P = 64, CR = 96, CA = 104, OV = 105, SO = 106, LR = 107, CTR = 108, GQR = 109, FPSCR = 117, RES = 118, POLL = 119, COUNT = 120;
}
using RegSet = std::bitset<128>;

struct RegEffect {
    RegSet use;    // read (over-approximation is safe)
    RegSet may;    // possibly written (over-approximation is safe)
    RegSet must;   // always written, so the old value is dead (under-approximation is safe); must is a subset of may
};

// Effect of the non-branch instruction `w` as the emitter writes it: `rc` = the record form's CR write is kept. The instruction must be one
// the emitter turns into a ppc::op_* call (not a branch, sc, trap or unsupported form); `ok` is false otherwise.
RegEffect reg_effect(const ppc::OpInfo& op, uint32_t w, bool rc, bool& ok);

// The C++ lvalue of resource `i` on context `ctx` ("c.r[3]", "c.gqr[2]").
std::string reg_field(int i, const std::string& ctx);

}  // namespace uc

namespace uc {
// A translated function that cannot reach a safe point or another function (no calls, no loops, no indirect or tail branches): every
// guest register it can load or store in the real context, and those it can store. A call of one only syncs those (translator/emit.cpp).
struct LeafRegs { RegSet touched, written; };
}  // namespace uc
