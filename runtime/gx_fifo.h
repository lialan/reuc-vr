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
inline void gx_fifo_store(int size, uint32_t value) {
    auto& window = gx_fifo_window;
    if (window.end - window.at > size) [[likely]] {
        const uint32_t big = uc_bswap32(value << ((32 - 8 * size) & 31));   // the size low bytes of value, most significant first
        std::memcpy(window.at, &big, 4);
        window.at += size;
        return;
    }
    gx_fifo_write(size, value);
}
}
