#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>
#include "host_compat.h"

namespace uc {
// Guest baton owns this tracker, including native HLE writes. Independent of GX cache epochs.
UC_IMAGE_LOCAL inline uint8_t* rollback_memory_base=nullptr;
UC_IMAGE_LOCAL inline std::array<uint8_t,1u<<18> rollback_dirty{};
// The per-store marks do not test whether a tracker runs (a load and a branch at every guest store): marking costs one store, and a
// tracker clears the table when it starts, so marks made while none runs are dropped.
inline void rollback_note_write(uint32_t address) {
    rollback_dirty[(address&0x3FFFFFFFu)>>12]=1;
}
inline void rollback_note_range(uint32_t address,size_t size) {
    if(!rollback_memory_base || !size) return;
    const uint64_t first=(address&0x3FFFFFFFu)>>12,last=((address&0x3FFFFFFFu)+uint64_t(size)-1)>>12;
    for(uint64_t page=first;page<=last && page<rollback_dirty.size();++page) rollback_dirty[page]=1;
}
// rollback_note_range for one guest access of 1..8 bytes (at most two pages): the general loop became a memset call at every stack store
// of the translated code, which kept a call (and its register saves) on the path of every function's prologue.
inline void rollback_note_access(uint32_t address,uint32_t size) {
    const uint32_t first=(address&0x3FFFFFFFu)>>12;
    rollback_dirty[first]=1;
    if((address&0xFFFu)+size-1>0xFFFu && first+1<rollback_dirty.size()) [[unlikely]] rollback_dirty[first+1]=1;   // the access ends in the next page
}
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
private:
    uint8_t* memory_;
    std::shared_ptr<const Snapshot> current_;
};
}
