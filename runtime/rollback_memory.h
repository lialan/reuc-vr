#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "host_compat.h"
#include "gx_write_watch.h"

namespace uc {
// Guest baton owns this tracker, including native HLE writes. Its dirty pages are those whose write epoch (gx_write_watch.h, the one
// mark every guest store makes) is newer than the stamp it took at its last capture or restore; a watch_reset (the texture caches'
// epoch wrap) makes every page dirty once.
UC_IMAGE_LOCAL inline uint8_t* rollback_memory_base=nullptr;
inline void rollback_note_write(uint32_t address) { gx::note_write(address); }
inline void rollback_note_range(uint32_t address,size_t size) { gx::note_write_range(address,size); }
inline void rollback_note_access(uint32_t address,uint32_t size) { gx::note_access(address,size); }
inline void rollback_note_pointer(const uint8_t* pointer,size_t size) {
    if(rollback_memory_base) rollback_note_range(uint32_t(uintptr_t(pointer)-uintptr_t(rollback_memory_base)),size);
}
class RollbackMemory {
public:
    struct Snapshot;
    explicit RollbackMemory(uint8_t* memory);
    ~RollbackMemory();
    RollbackMemory(const RollbackMemory&)=delete;
    RollbackMemory& operator=(const RollbackMemory&)=delete;
    std::shared_ptr<const Snapshot> capture(bool verify_writes=false);
    void restore(const Snapshot&);
    static std::string difference(const Snapshot&,const Snapshot&);
    static uint64_t hash(const Snapshot&);
    // Desync diagnostic: one 32-bit hash per 16 pages (64 KiB) in capture order (0..24 MiB, 0x10000000..+64 MiB, 0x20000000..+64 KiB).
    static std::vector<uint32_t> chunk_hashes(const Snapshot&);
    // Desync diagnostic: the captured RAM in capture order (24 MiB, 64 MiB, 64 KiB), raw.
    static void dump(const Snapshot&,std::FILE*);
    // A store reached the page holding `address` since the last capture or restore.
    bool dirty(uint32_t address) const { return gx::watch_generation!=generation_ || gx::page_epoch[(address&0x3FFFFFFFu)>>12]>stamp_; }
private:
    void mark_clean() { stamp_=gx::watch_stamp(); generation_=gx::watch_generation; }
    uint8_t* memory_;
    std::shared_ptr<const Snapshot> current_;
    uint32_t stamp_=0, generation_=0;
};
}
