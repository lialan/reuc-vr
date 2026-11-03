#pragma once
#include "gx_write_watch.h"

namespace gx {
// The stamp a cache of guest memory records after it compared the bytes (TextureCache, the mesh cache). On the guest thread it is a fresh
// watch_stamp(). On the GX builder thread (hle/gx_queue.h) it is the stamp the guest thread took when it issued the command being run: every
// guest store after that command has a larger epoch, so a later written_since() against it never misses a change (it may report one the
// comparison already saw: one extra comparison). The builder never advances write_epoch itself: that counter belongs to the guest's stores.
inline thread_local uint32_t command_stamp = 0;
inline uint32_t verified_stamp() { return command_stamp ? command_stamp : watch_stamp(); }
inline bool on_builder_thread() { return command_stamp != 0; }
}
