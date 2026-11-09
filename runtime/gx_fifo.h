#pragma once
// The GX FIFO port: guest stores to 0xCC008000 (about a million a second, mostly display-list recording). gx_fifo_store is the entry point for
// the store path (ppc::mmio_write today; a fastmem fault path can call it the same way): an inline append when gx.cpp has an open run of bytes
// that needs no decision, gx_fifo_write for everything else.
#include "host_compat.h"
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace uc {
void gx_fifo_write(int size, uint32_t value);   // hle/gx.cpp: every FIFO store's full handling (size 1, 2 or 4)
// The bytes the next stores fill without a decision: a display list being recorded (up to its capacity, or the end of its open primitive),
// or an open immediate-mode primitive (up to its last byte). A store that would reach `end` takes gx_fifo_write, which decides and moves the
// window. The buffers have at least 4 bytes after `end` (the 4-byte copy below). Guest baton only.
struct GxFifoWindow { std::byte* at = nullptr; std::byte* end = nullptr; };
inline GxFifoWindow gx_fifo_window;
// Outside a recording and a primitive the bytes are state commands of SDK code that is not replaced (counted, otherwise ignored); only a
// raw display-list call (0x40, then two 4-byte words) needs gx_fifo_write, which follows it through gx_fifo_raw_state.
inline bool gx_fifo_outside = true;
inline uint8_t gx_fifo_raw_state = 0;
inline uint64_t gx_fifo_outside_bytes = 0;   // guest baton writes; the statistics read it racily
// ppc::mmio_write's FIFO branch (platform.cpp): direct = the stores go to gx_fifo_store (no audit device watches them); the counts are
// statistics ([detbench], the FIFO byte snapshot). Guest baton writes; readers read racily.
inline bool gx_fifo_direct = false;
inline uint64_t gx_fifo_direct_bytes = 0, gx_fifo_writes = 0;
// Appends the size low bytes of value, most significant first; the window must have more than size bytes left.
inline void gx_fifo_put(int size, uint32_t value) {
    auto& window = gx_fifo_window;
    const uint32_t big = uc_bswap32(value << ((32 - 8 * size) & 31));
    std::memcpy(window.at, &big, 4);
    window.at += size;
}
inline void gx_fifo_store(int size, uint32_t value) {
    if (gx_fifo_window.end - gx_fifo_window.at > size) [[likely]] { gx_fifo_put(size, value); return; }
    if (gx_fifo_outside && !gx_fifo_raw_state && !(size == 1 && value == 0x40)) { gx_fifo_outside_bytes += uint32_t(size); return; }
    gx_fifo_write(size, value);
}
// A run of `stores` FIFO stores of `bytes` bytes in all that the translator found in one block (translator/emit.cpp, fifo runs), at its
// first store: true when ea is the FIFO port, no audit watches it and the open window takes every byte of the run without a decision. The
// run then appends each store with gx_fifo_put in place of the store, which is what mmio_write would do for each of them one at a time;
// false: every store of the run takes the store path.
inline bool gx_fifo_burst(uint32_t ea, uint32_t bytes, uint32_t stores) {
    if (ea != 0xCC008000u || !gx_fifo_direct || gx_fifo_window.end - gx_fifo_window.at <= std::ptrdiff_t(bytes)) return false;
    gx_fifo_writes += stores;
    gx_fifo_direct_bytes += bytes;
    return true;
}
}
