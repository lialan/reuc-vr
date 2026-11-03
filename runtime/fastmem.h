#pragma once
// fastmem: guest accesses without an address test (UC_FASTMEM builds: macOS and Android on arm64, see ppc_rt.h).
//
// The hardware-register window of the guest (0xCC000000..0xCDFFFFFF) is part of the inaccessible reservation of guest_mem_posix.cpp, so a
// translated load or store whose address the compiler cannot see needs no MMIO test: when it reaches the window the host access faults, and
// the SIGSEGV/SIGBUS handler here decodes the faulting arm64 instruction, performs it as ppc::mmio_read/mmio_write in guest byte order and
// resumes after it. Any other fault goes on to the handler installed before (the crash report) unchanged.
#include <cstdint>

namespace uc {

// One decoded arm64 load/store: the register-file side of the access (what the handler emulates).
struct HostAccess {
    enum Mode : uint8_t { Offset, PreIndex, PostIndex };
    uint8_t n = 0;              // base register (31 = SP)
    uint8_t t = 0, t2 = 0xFF;   // transfer register(s); t2 = 0xFF: one register (31 = XZR/WZR for the integer forms)
    uint8_t bytes = 0;          // bytes per register: 1, 2, 4, 8 or 16
    bool load = false;
    bool vector = false;        // SIMD&FP register (B/H/S/D/Q, LD1/ST1)
    bool sign = false;          // integer load: sign-extend from bytes*8 bits
    bool wide = false;          // integer load: to the 64-bit X register (else W: the upper half is zeroed)
    Mode mode = Offset;
    int64_t imm = 0;            // immediate offset (pre: before the access, post: written back after it)
    uint8_t m = 0xFF;           // register offset Rm (0xFF: none); for a post-indexed LD1/ST1, the increment register
    uint8_t m_option = 3;       // extend of Rm: 2 UXTW, 3 LSL (UXTX), 6 SXTW, 7 SXTX
    uint8_t m_shift = 0;
};
// The loads and stores the compiler emits for guest accesses: LDR/STR (unsigned offset, unscaled, pre/post-index, register offset) of every
// size and signedness, LDP/STP/LDPSW (integer and SIMD&FP, offset, pre/post-index, non-temporal) and LD1/ST1 of one register. False for
// anything else (exclusives, atomics, PRFM, other SIMD structure forms).
bool fastmem_decode(uint32_t insn, HostAccess& access);

// The machine state the emulation reads and writes: x[0..30], x[31] = SP, the 32 SIMD&FP registers (16 bytes each, little-endian), PC.
struct HostRegisters {
    uint64_t* x;
    uint8_t (*v)[16];
    uint64_t* pc;
};
using MmioReadFn = uint32_t (*)(uint32_t ea, int size);
using MmioWriteFn = void (*)(uint32_t ea, int size, uint32_t value);
// Emulates `insn` (at *regs.pc) against guest memory at `mem`: when its first byte is in the MMIO window, every register's bytes go through
// read/write (one access of 1, 2 or 4 bytes; wider ones as 4-byte accesses in ascending address order, as rdf64/wrf64 and the psq slow paths
// split them), the registers and the base writeback are updated and the PC moves past the instruction. False (nothing changed) otherwise.
bool fastmem_emulate(uint32_t insn, const HostRegisters& regs, const uint8_t* mem, MmioReadFn read, MmioWriteFn write);

// Installs the handler for SIGSEGV and SIGBUS in front of the current ones (idempotent). No-op where fastmem is not built.
void fastmem_install();
// Accesses emulated so far (all threads).
struct FastmemStats { uint64_t reads = 0, writes = 0; };
FastmemStats fastmem_stats();
// UC_FASTMEM_LOG=1: the emulated accesses by guest address and host instruction, printed by this call (at exit).
void fastmem_report();

}  // namespace uc
