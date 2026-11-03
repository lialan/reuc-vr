#pragma once
#include <cstddef>
#include <limits>
#include <map>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <new>
#include <type_traits>
#include <vector>

namespace gx {
// Upstream of the frame arenas: the chunks of a retired packet are kept for the next packets. A frame's ~25 packets ask for the same chunk sizes
// every frame; from the heap they were large allocations (jemalloc extents, purged and faulted in again) made on the guest thread. Any thread:
// the guest allocates, the render thread retires. At most 64 MB stay held.
class ChunkPool final : public std::pmr::memory_resource {
public:
    static ChunkPool& instance() { static ChunkPool* pool = new ChunkPool; return *pool; }   // never destroyed: arenas may retire during exit
private:
    static constexpr size_t kMaxHeld = size_t(64) << 20;
    static bool plain(size_t align) { return align <= __STDCPP_DEFAULT_NEW_ALIGNMENT__; }
    void* do_allocate(size_t bytes, size_t align) override {
        if (!plain(align)) return ::operator new(bytes, std::align_val_t(align));
        {
            std::lock_guard lock(mutex_);
            if (auto found = free_.find(bytes); found != free_.end() && !found->second.empty()) {
                void* chunk = found->second.back(); found->second.pop_back(); held_ -= bytes; return chunk;
            }
        }
        return ::operator new(bytes);
    }
    void do_deallocate(void* chunk, size_t bytes, size_t align) override {
        if (!plain(align)) { ::operator delete(chunk, std::align_val_t(align)); return; }
        {
            std::lock_guard lock(mutex_);
            if (held_ + bytes <= kMaxHeld) { free_[bytes].push_back(chunk); held_ += bytes; return; }
        }
        ::operator delete(chunk);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override { return this == &other; }
    std::mutex mutex_;
    std::map<size_t, std::vector<void*>> free_;   // by chunk size
    size_t held_ = 0;
};
struct FrameArena {
    std::mutex mutex;
    std::pmr::monotonic_buffer_resource memory{&ChunkPool::instance()};
};

// Nodes retire together with their frame; individual deque frees need no shared pool lock.
template<class T>
class FrameAllocator {
    template<class> friend class FrameAllocator;
    std::shared_ptr<FrameArena> arena_=std::make_shared<FrameArena>();
public:
    using value_type=T;
    using propagate_on_container_move_assignment=std::true_type;
    FrameAllocator()=default;
    FrameAllocator(const FrameAllocator&)=default;
    FrameAllocator& operator=(const FrameAllocator&)=default;
    // Keep moved-from containers usable while another thread owns the published frame.
    FrameAllocator(FrameAllocator&& other) noexcept : arena_(other.arena_) {}
    FrameAllocator& operator=(FrameAllocator&& other) noexcept { arena_=other.arena_;return *this; }
    template<class U> FrameAllocator(const FrameAllocator<U>& other) noexcept : arena_(other.arena_) {}
    FrameAllocator select_on_container_copy_construction() const { return {}; }
    T* allocate(size_t n) {
        if(n>std::numeric_limits<size_t>::max()/sizeof(T)) throw std::bad_array_new_length();
        std::lock_guard lock(arena_->mutex);
        return static_cast<T*>(arena_->memory.allocate(n*sizeof(T),alignof(T)));
    }
    void deallocate(T*,size_t) noexcept {}
    template<class U> bool operator==(const FrameAllocator<U>& other) const noexcept { return arena_==other.arena_; }
};
}
