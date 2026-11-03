// The SDK's register save/restore helpers (the compiler's _savegpr_N, _restgpr_N, _savefpr_N, _restfpr_N, N = 14..31): every function prologue and epilogue of the
// game calls one, with r11 pointing just above the save area. In the translated code each is a chain of one-instruction functions that fall into each other (up to
// 18 nested calls); here an entry is one native function running the same load/store instructions in the same order, so the guest memory, the registers and the
// write watch behave exactly as before. project/hle_map.txt binds the 72 entry addresses; tests/test_sdk_regs_hle.cpp checks them against the instruction sequences.
#include "ppc_rt.h"
#include <cstring>
#include <utility>

// The fast block form and the per-instruction fallback live in ppc_rt.h (ppc::save_gpr<N> ...), shared with the translated code, which
// writes these helpers in place of a call.
using ppc::Ctx;

#define UC_REG_HLE(N) \
    void hle_SaveGpr##N(Ctx& c) { ppc::save_gpr<N>(c); } \
    void hle_RestGpr##N(Ctx& c) { ppc::rest_gpr<N>(c); } \
    void hle_SaveFpr##N(Ctx& c) { ppc::save_fpr<N>(c); } \
    void hle_RestFpr##N(Ctx& c) { ppc::rest_fpr<N>(c); }
UC_REG_HLE(14) UC_REG_HLE(15) UC_REG_HLE(16) UC_REG_HLE(17) UC_REG_HLE(18) UC_REG_HLE(19) UC_REG_HLE(20) UC_REG_HLE(21) UC_REG_HLE(22)
UC_REG_HLE(23) UC_REG_HLE(24) UC_REG_HLE(25) UC_REG_HLE(26) UC_REG_HLE(27) UC_REG_HLE(28) UC_REG_HLE(29) UC_REG_HLE(30) UC_REG_HLE(31)
