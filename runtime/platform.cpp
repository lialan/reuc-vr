#include "clock.h"
#include "fatal.h"
#include "mmio.h"
#include "gx_fifo.h"
#include "hle/os.h"
#include "ppc_rt.h"

namespace {
uc::MmioDevice* gx_fifo_device;
uint64_t mmio_reads, mmio_writes;   // guest code runs on one host thread at a time: plain counters
}
namespace uc {
void mmio_set_gx_fifo(MmioDevice* device, bool direct) { gx_fifo_device = device; gx_fifo_direct = direct; }
uint64_t mmio_gx_fifo_direct_bytes() { return gx_fifo_direct_bytes; }
MmioCounts mmio_counts() { return {mmio_reads, mmio_writes, gx_fifo_writes}; }
}
namespace ppc {
uint32_t mmio_read(uint32_t ea, int size) { ++mmio_reads; return uc::mmio_bus().read(ea, size); }
void mmio_write(uint32_t ea, int size, uint32_t v) {
    // The FIFO port, from the explicit store path and from fastmem's fault handler alike: the inline GX append (gx_fifo.h) unless an audit
    // watches the stores through the device.
    if (ea == 0xCC008000u) [[likely]] {
        if (uc::gx_fifo_direct) [[likely]] { ++uc::gx_fifo_writes; uc::gx_fifo_direct_bytes += uint32_t(size); uc::gx_fifo_store(size, v); return; }
        if (gx_fifo_device) { ++uc::gx_fifo_writes; gx_fifo_device->write(0, size, v); return; }
    }
    ++mmio_writes;
    uc::mmio_bus().write(ea, size, v);
}
[[noreturn]] void trap(uint32_t addr, const char* what) { uc::fatal_guest(addr, what); }
uint64_t timebase() { return uc::now_ticks(); }
void poll_slow(Ctx&) { uc::os_sched().safe_point(); }
}  // namespace ppc
