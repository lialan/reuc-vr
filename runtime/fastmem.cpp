#include "fastmem.h"
#include "ppc_rt.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <mutex>
#include <vector>

#if UC_FASTMEM
#include "dispatch.h"
#include "guest_mem.h"
#include "trace.h"
#include <csignal>
#include <cstddef>
#include <dlfcn.h>
#include <pthread.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#else
#include <ucontext.h>
#endif
#endif

namespace uc {
namespace {
int64_t sign_extend(uint64_t v, unsigned bits) { return int64_t(v << (64 - bits)) >> (64 - bits); }

// The width, direction and extension of the single-register forms (size, V and opc fields).
bool register_type(uint32_t size, uint32_t vector, uint32_t opc, HostAccess& a) {
    a.vector = vector != 0;
    if (!vector) {
        a.bytes = uint8_t(1u << size);
        switch (opc) {
            case 0: a.load = false; break;
            case 1: a.load = true; a.wide = size == 3; break;
            case 2: if (size == 3) return false; a.load = true; a.sign = true; a.wide = true; break;    // PRFM; LDRSB/LDRSH/LDRSW Xt
            default: if (size >= 2) return false; a.load = true; a.sign = true; a.wide = false; break; // LDRSB/LDRSH Wt
        }
        return true;
    }
    a.load = opc & 1;
    if (opc & 2) { if (size != 0) return false; a.bytes = 16; }   // Q
    else a.bytes = uint8_t(1u << size);                            // B, H, S, D
    return true;
}
unsigned log2_bytes(unsigned bytes) { return bytes == 1 ? 0 : bytes == 2 ? 1 : bytes == 4 ? 2 : bytes == 8 ? 3 : 4; }

// The bytes of an access of `bytes` at guest `ea` as guest memory holds them (big-endian): one access of 1, 2 or 4 bytes, wider ones as
// 4-byte accesses in ascending order.
void mmio_load(uint32_t ea, unsigned bytes, uint8_t* out, MmioReadFn read) {
    if (bytes <= 4) {
        const uint32_t v = read(ea, int(bytes));
        for (unsigned i = 0; i < bytes; ++i) out[i] = uint8_t(v >> (8 * (bytes - 1 - i)));
        return;
    }
    for (unsigned k = 0; k < bytes; k += 4) {
        const uint32_t v = read(ea + k, 4);
        for (unsigned i = 0; i < 4; ++i) out[k + i] = uint8_t(v >> (8 * (3 - i)));
    }
}
void mmio_store(uint32_t ea, unsigned bytes, const uint8_t* in, MmioWriteFn write) {
    const unsigned piece = bytes <= 4 ? bytes : 4;
    for (unsigned k = 0; k < bytes; k += piece) {
        uint32_t v = 0;
        for (unsigned i = 0; i < piece; ++i) v = v << 8 | in[k + i];
        write(ea + k, int(piece), v);
    }
}
}  // namespace

bool fastmem_decode(uint32_t insn, HostAccess& a) {
    a = HostAccess{};
    const uint32_t size = insn >> 30, vector = (insn >> 26) & 1, opc = (insn >> 22) & 3;
    a.n = uint8_t((insn >> 5) & 31);
    a.t = uint8_t(insn & 31);
    if ((insn & 0x3B000000u) == 0x39000000u) {            // LDR/STR (unsigned immediate)
        if (!register_type(size, vector, opc, a)) return false;
        a.imm = int64_t((insn >> 10) & 0xFFF) << log2_bytes(a.bytes);
        return true;
    }
    if ((insn & 0x3B200000u) == 0x38000000u) {            // LDUR/STUR, post-index, unprivileged, pre-index (imm9)
        if (!register_type(size, vector, opc, a)) return false;
        const uint32_t idx = (insn >> 10) & 3;
        if (idx == 2 && vector) return false;
        a.mode = idx == 1 ? HostAccess::PostIndex : idx == 3 ? HostAccess::PreIndex : HostAccess::Offset;
        a.imm = sign_extend((insn >> 12) & 0x1FF, 9);
        return true;
    }
    if ((insn & 0x3B200C00u) == 0x38200800u) {            // LDR/STR (register offset)
        if (!register_type(size, vector, opc, a)) return false;
        const uint32_t option = (insn >> 13) & 7;
        if (!(option & 2)) return false;
        a.m = uint8_t((insn >> 16) & 31);
        a.m_option = uint8_t(option);
        a.m_shift = uint8_t(((insn >> 12) & 1) ? log2_bytes(a.bytes) : 0);
        return true;
    }
    if ((insn & 0x3A000000u) == 0x28000000u) {            // LDP/STP/LDPSW/LDNP/STNP
        const uint32_t pair_opc = insn >> 30, mode = (insn >> 23) & 3;
        a.load = (insn >> 22) & 1;
        a.vector = vector != 0;
        if (vector) {
            if (pair_opc == 3) return false;
            a.bytes = uint8_t(4u << pair_opc);           // S, D, Q
        } else if (pair_opc == 0) a.bytes = 4;
        else if (pair_opc == 2) { a.bytes = 8; a.wide = true; }
        else if (pair_opc == 1 && a.load) { if (mode == 0) return false; a.bytes = 4; a.sign = true; a.wide = true; }   // LDPSW
        else return false;                                 // STGP, unallocated
        a.t2 = uint8_t((insn >> 10) & 31);
        a.mode = mode == 1 ? HostAccess::PostIndex : mode == 3 ? HostAccess::PreIndex : HostAccess::Offset;
        a.imm = sign_extend((insn >> 15) & 0x7F, 7) * int64_t(a.bytes);
        return true;
    }
    // LD1/ST1 (multiple structures), one register: 8 or 16 bytes, in memory order on a little-endian host whatever the element size.
    const bool ld1 = (insn & 0xBFBFF000u) == 0x0C007000u, ld1_post = (insn & 0xBFA0F000u) == 0x0C807000u;
    if (ld1 || ld1_post) {
        a.vector = true;
        a.load = (insn >> 22) & 1;
        a.bytes = (insn >> 30) & 1 ? 16 : 8;
        if (ld1_post) {
            a.mode = HostAccess::PostIndex;
            const uint32_t rm = (insn >> 16) & 31;
            if (rm == 31) a.imm = a.bytes; else a.m = uint8_t(rm);
        }
        return true;
    }
    return false;
}

bool fastmem_emulate(uint32_t insn, const HostRegisters& r, const uint8_t* mem, MmioReadFn read, MmioWriteFn write) {
    HostAccess a;
    if (!fastmem_decode(insn, a)) return false;
    const uint64_t base = r.x[a.n];   // x[31] is SP: the base register of a load/store
    uint64_t offset = uint64_t(a.imm);
    if (a.m != 0xFF && a.mode == HostAccess::Offset) {
        uint64_t rm = a.m == 31 ? 0 : r.x[a.m];   // XZR in a register offset
        switch (a.m_option) {
            case 2: rm = uint32_t(rm); break;
            case 6: rm = uint64_t(int64_t(int32_t(uint32_t(rm)))); break;
            default: break;
        }
        offset = rm << a.m_shift;
    }
    const uint64_t address = a.mode == HostAccess::PostIndex ? base : base + offset;
    const uint64_t guest = address - reinterpret_cast<uint64_t>(mem);
    if (guest - 0xCC000000u >= 0x02000000u) return false;   // only accesses that start in the hardware-register window
    const uint32_t ea = uint32_t(guest);
    const unsigned count = a.t2 == 0xFF ? 1 : 2;
    const uint8_t regs[2] = {a.t, a.t2};
    uint8_t data[2][16] = {};
    if (!a.load)
        for (unsigned i = 0; i < count; ++i) {
            if (a.vector) std::memcpy(data[i], r.v[regs[i]], a.bytes);
            else { const uint64_t v = regs[i] == 31 ? 0 : r.x[regs[i]]; std::memcpy(data[i], &v, a.bytes); }   // WZR/XZR
        }
    for (unsigned i = 0; i < count; ++i) {
        if (a.load) mmio_load(ea + i * a.bytes, a.bytes, data[i], read);
        else mmio_store(ea + i * a.bytes, a.bytes, data[i], write);
    }
    if (a.mode == HostAccess::PreIndex) r.x[a.n] = base + offset;
    else if (a.mode == HostAccess::PostIndex) r.x[a.n] = base + (a.m != 0xFF ? (a.m == 31 ? 0 : r.x[a.m]) : uint64_t(a.imm));
    if (a.load)
        for (unsigned i = 0; i < count; ++i) {
            if (a.vector) { std::memset(r.v[regs[i]], 0, 16); std::memcpy(r.v[regs[i]], data[i], a.bytes); continue; }
            if (regs[i] == 31) continue;   // a load into XZR
            uint64_t v = 0; std::memcpy(&v, data[i], a.bytes);
            if (a.sign) v = uint64_t(sign_extend(v, 8u * a.bytes));
            if (!a.wide) v &= 0xFFFFFFFFu;
            r.x[regs[i]] = v;
        }
    *r.pc += 4;
    return true;
}

namespace {
std::atomic<uint64_t> g_reads{0}, g_writes{0};
struct Site { uint32_t ea; uint8_t bytes; bool load; uintptr_t pc; uint64_t count; };
std::mutex g_sites_lock;
std::vector<Site>* g_sites;   // UC_FASTMEM_LOG only
bool log_enabled() { static const bool on = [] { const char* e = std::getenv("UC_FASTMEM_LOG"); return e && *e && *e != '0'; }(); return on; }
}  // namespace

FastmemStats fastmem_stats() { return {g_reads.load(std::memory_order_relaxed), g_writes.load(std::memory_order_relaxed)}; }

#if UC_FASTMEM
namespace {
struct sigaction g_previous_segv, g_previous_bus;

// The ucontext layouts: x0..x30 and SP are consecutive in both, so x[31] = SP.
#if defined(__APPLE__)
static_assert(offsetof(__darwin_arm_thread_state64, __sp) == 31 * 8, "x0..x28, fp, lr, sp");
HostRegisters host_registers(void* context) {
    mcontext_t mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    return {&mc->__ss.__x[0], reinterpret_cast<uint8_t(*)[16]>(&mc->__ns.__v[0]), &mc->__ss.__pc};
}
#else
static_assert(offsetof(mcontext_t, sp) == offsetof(mcontext_t, regs) + 31 * 8, "regs[31], sp");
struct ContextHead { uint32_t magic, size; };
struct FpsimdContext { ContextHead head; uint32_t fpsr, fpcr; alignas(16) uint8_t vregs[32][16]; };
constexpr uint32_t kFpsimdMagic = 0x46508001;
HostRegisters host_registers(void* context) {
    mcontext_t& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
    uint8_t (*v)[16] = nullptr;
    for (size_t at = 0; at + sizeof(ContextHead) <= sizeof(mc.__reserved);) {   // the records of the signal frame's extension area
        ContextHead head; std::memcpy(&head, reinterpret_cast<uint8_t*>(mc.__reserved) + at, sizeof head);
        if (!head.magic || head.size < sizeof head) break;
        if (head.magic == kFpsimdMagic) { v = reinterpret_cast<FpsimdContext*>(reinterpret_cast<uint8_t*>(mc.__reserved) + at)->vregs; break; }
        at += head.size;
    }
    return {reinterpret_cast<uint64_t*>(mc.regs), v, reinterpret_cast<uint64_t*>(&mc.pc)};
}
#endif

// UC_FASTMEM_LOG: one entry per host instruction, with the guest address of its first emulated access.
void note_site(const HostAccess& a, uintptr_t pc, uint32_t ea) {
    std::lock_guard<std::mutex> hold(g_sites_lock);
    if (!g_sites) g_sites = new std::vector<Site>;
    for (auto& s : *g_sites) if (s.pc == pc) { ++s.count; return; }
    if (g_sites->size() < 4096) g_sites->push_back({ea, a.bytes, a.load, pc, 1});
}

// Counting wrappers: the guest address of the site is the first access the emulation makes.
thread_local uint32_t t_first_ea;
thread_local bool t_first_seen;
uint32_t counted_read(uint32_t ea, int size) {
    if (!t_first_seen) { t_first_ea = ea; t_first_seen = true; }
    return ppc::mmio_read(ea, size);
}
void counted_write(uint32_t ea, int size, uint32_t v) {
    if (!t_first_seen) { t_first_ea = ea; t_first_seen = true; }
    ppc::mmio_write(ea, size, v);
}

void chain(int signal, siginfo_t* info, void* context) {
    const struct sigaction& previous = signal == SIGBUS ? g_previous_bus : g_previous_segv;
    // The previous handler runs as the kernel would have run it: with its mask and its own signal blocked unless it asked for SA_NODEFER.
    sigset_t mask = previous.sa_mask;
    if (!(previous.sa_flags & SA_NODEFER)) sigaddset(&mask, signal);
    pthread_sigmask(SIG_BLOCK, &mask, nullptr);
    if (previous.sa_flags & SA_SIGINFO) { if (previous.sa_sigaction) previous.sa_sigaction(signal, info, context); return; }
    if (previous.sa_handler == SIG_DFL || previous.sa_handler == SIG_IGN) {
        // The default action: restored, the instruction faults again on return (the mask of the interrupted code is restored with it).
        struct sigaction fallback{}; fallback.sa_handler = SIG_DFL; sigemptyset(&fallback.sa_mask);
        sigaction(signal, &fallback, nullptr);
        return;
    }
    previous.sa_handler(signal);
}

void fault(int signal, siginfo_t* info, void* context) {
    const uint8_t* mem = guest_mem_base();
    const uint64_t at = reinterpret_cast<uint64_t>(info->si_addr) - reinterpret_cast<uint64_t>(mem);
    if (mem && at < 0x100000000ull) {
        const HostRegisters r = host_registers(context);
        if (r.v) {
            const uint32_t insn = *reinterpret_cast<const uint32_t*>(uintptr_t(*r.pc));
            // SA_NODEFER: a fault inside the device code (a real crash) reaches this handler again and goes on to the crash report.
            const bool log = log_enabled();
            t_first_seen = false;
            HostAccess a;
            bool done = false;
            // A C++ exception cannot leave a signal handler into the translated code (no unwind information at a faulting load or store):
            // it ends the process here as an uncaught one would (the terminate handler reports it).
            try { done = fastmem_decode(insn, a) && fastmem_emulate(insn, r, mem, log ? counted_read : ppc::mmio_read, log ? counted_write : ppc::mmio_write); }
            catch (...) { std::terminate(); }
            if (done) {
                (a.load ? g_reads : g_writes).fetch_add(1, std::memory_order_relaxed);
                if (log) note_site(a, uintptr_t(*r.pc - 4), t_first_ea);
                return;
            }
        }
    }
    chain(signal, info, context);
}
}  // namespace

void fastmem_install() {
    struct sigaction action{};
    action.sa_sigaction = fault;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    for (int signal : {SIGSEGV, SIGBUS}) {
        struct sigaction current{};
        sigaction(signal, nullptr, &current);
        if ((current.sa_flags & SA_SIGINFO) && current.sa_sigaction == fault) continue;
        sigaction(signal, &action, signal == SIGBUS ? &g_previous_bus : &g_previous_segv);
    }
}

void fastmem_report() {
    const FastmemStats s = fastmem_stats();
    std::fprintf(stderr, "fastmem: %llu MMIO reads and %llu writes through the fault handler\n", (unsigned long long)s.reads, (unsigned long long)s.writes);
    std::lock_guard<std::mutex> hold(g_sites_lock);
    if (!g_sites) return;
    std::vector<Site> sites = *g_sites;
    std::sort(sites.begin(), sites.end(), [](const Site& a, const Site& b) { return a.count > b.count; });
    for (size_t i = 0; i < sites.size() && i < 40; ++i) {
        const Site& s = sites[i];
        std::fprintf(stderr, "fastmem: %10llu x %s%u at %08X  pc %p", (unsigned long long)s.count, s.load ? "load" : "store", unsigned(s.bytes), s.ea,
                     reinterpret_cast<void*>(s.pc));
        Dl_info module{};
        if (dladdr(reinterpret_cast<void*>(s.pc), &module) && module.dli_sname)
            std::fprintf(stderr, " %s+0x%llX", module.dli_sname, (unsigned long long)(s.pc - uintptr_t(module.dli_saddr)));
        if (const uint32_t guest = guest_function_for_host(reinterpret_cast<void*>(s.pc))) std::fprintf(stderr, " guest %08X [%s]", guest, symbol_name(guest).c_str());
        std::fprintf(stderr, "\n");
    }
}
#else
void fastmem_install() {}
void fastmem_report() {}
#endif

}  // namespace uc
