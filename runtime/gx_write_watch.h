#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include "host_compat.h"

namespace gx {
// Guest write barrier. Every guest store (and every native bulk write into guest RAM) records the current epoch in its 4 KiB page, so a cache
// holding a copy of guest memory can skip re-reading it when no store touched the pages since it last verified them. The rollback tracker
// (rollback_memory.h) reads the same table: a page is dirty for it when its epoch is newer than the tracker's last stamp, so a store's
// bookkeeping is this one write. Written only by the thread holding the guest baton; the native code that reads it runs on that thread
// too, while no guest code executes.
inline constexpr uint32_t kWatchPages = 1u << 18;   // 1 GiB of guest address space / 4 KiB
UC_IMAGE_LOCAL inline uint32_t write_epoch = 2;
UC_IMAGE_LOCAL inline uint32_t page_epoch[kWatchPages];
UC_IMAGE_LOCAL inline uint32_t watch_generation = 0;   // counts watch_reset: a stamp from an earlier generation compares against nothing

inline void note_write(uint32_t address) { page_epoch[(address & 0x3FFFFFFFu) >> 12] = write_epoch; }
// One guest access of 1..8 bytes (at most two pages): the stack stores' barrier (they skip nothing else, the general range loop was a
// memset call at every one).
inline void note_access(uint32_t address, uint32_t size) {
    const uint32_t first = (address & 0x3FFFFFFFu) >> 12;
    page_epoch[first] = write_epoch;
    if ((address & 0xFFFu) + size - 1 > 0xFFFu && first + 1 < kWatchPages) [[unlikely]] page_epoch[first + 1] = write_epoch;   // ends in the next page
}
// A write of 1..4096 bytes (a stack frame, a register save area): its first and last page, the one or two it touches.
inline void note_small(uint32_t address, uint32_t size) {
    const uint32_t at = address & 0x3FFFFFFFu, first = at >> 12, last = (at + size - 1) >> 12;
    page_epoch[first] = write_epoch;
    if (last < kWatchPages) page_epoch[last] = write_epoch;
}
inline void note_write_range(uint32_t address, size_t size) {
    if (!size) return;
    const uint64_t first = (address & 0x3FFFFFFFu) >> 12, last = ((address & 0x3FFFFFFFu) + uint64_t(size) - 1) >> 12;
    for (uint64_t page = first; page <= last && page < kWatchPages; ++page) page_epoch[page] = write_epoch;
}
// "Verified now": every store after this call gets a larger epoch than the returned stamp.
inline uint32_t watch_stamp() { return write_epoch++; }
inline bool written_since(uint32_t address, size_t size, uint32_t stamp) {
    if (!size) return false;
    const uint64_t first = (address & 0x3FFFFFFFu) >> 12, last = ((address & 0x3FFFFFFFu) + uint64_t(size) - 1) >> 12;
    if (last >= kWatchPages) return true;
    // The newest epoch of the range's pages, without an early exit: a texture or vertex array spans dozens of pages, and a plain maximum is
    // vector code; every caller is checking a range nothing wrote to.
    uint32_t newest = 0;
    for (uint64_t page = first; page <= last; ++page) newest = newest > page_epoch[page] ? newest : page_epoch[page];
    return newest > stamp;
}
// The epoch counter is 32 bit; callers drop their stamps and call this long before it wraps.
inline bool watch_exhausted() { return write_epoch > 0xF0000000u; }
#ifdef UC_DIAG_CHECK_FRAMES
// A check build of the frame notes: every thread switch and scheduler safe point counts as a rollback capture (frame_check_cut), and a
// framed stack store (ppc_rt.h) must find its pages marked since the last one; frame_check_fail reports the ones that do not.
UC_IMAGE_LOCAL inline uint32_t frame_check_cap = 0;
UC_IMAGE_LOCAL inline uint64_t frame_check_failures = 0;
inline void frame_check_cut() { frame_check_cap = write_epoch++; }
[[gnu::noinline, gnu::cold]] inline void frame_check_fail(uint32_t address) {
    if (++frame_check_failures <= 20 || frame_check_failures % 100000 == 0)
        std::fprintf(stderr, "[frame-check] unmarked framed store at %08X (%llu so far)\n", address, (unsigned long long)frame_check_failures);
}
inline void frame_check(uint32_t address, uint32_t size) {
    const uint32_t at = address & 0x3FFFFFFFu;
    if (page_epoch[at >> 12] <= frame_check_cap || page_epoch[(at + size - 1) >> 12] <= frame_check_cap) [[unlikely]] frame_check_fail(address);
}
inline void watch_reset() { for (auto& epoch : page_epoch) epoch = 0; write_epoch = 2; ++watch_generation; frame_check_cap = 0; }
#else
inline void frame_check(uint32_t, uint32_t) {}
inline void watch_reset() { for (auto& epoch : page_epoch) epoch = 0; write_epoch = 2; ++watch_generation; }
#endif
}
