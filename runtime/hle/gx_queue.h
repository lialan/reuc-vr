#pragma once
// The GX builder queue (Dolphin's dual-core idea): the guest thread (the "front", gx.cpp) does what the guest can observe (SDK block writes,
// guest calls, display-list bytes, validation) and appends a command per GX call; the builder thread (the "back") runs the commands in order
// and turns them into DrawPackets for the renderer. Without the builder thread, emit() runs each command at once on the calling thread (unit
// tests, UC_GX_SYNC=1): the behaviour of the code before the queue existed.
//
// Commands are lambdas placed in 1 MiB chunks (one allocation per chunk, recycled); the front publishes after every command with one release
// store and wakes the builder only when it sleeps. A command that throws stops the builder: the remaining commands are dropped and the front
// rethrows the error at its next emit() or drain(), or from a scheduler event (on_failure) when the guest thread is blocked.
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <cstring>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace gx {
class CommandQueue {
public:
    CommandQueue() = default;
    CommandQueue(const CommandQueue&) = delete;
    CommandQueue& operator=(const CommandQueue&) = delete;
    ~CommandQueue() { stop(); }

    // Starts the builder thread (idempotent). on_failure runs on the builder thread right after a command failed (gx.cpp posts a scheduler
    // event that rethrows on the guest thread); on_start runs first on the builder thread.
    void start(std::function<void()> on_start, std::function<void()> on_failure);
    void stop();   // drains, then ends the builder thread; emit() runs inline afterwards
    bool threaded() const { return worker_.joinable(); }

    // Front only. `stamp` is the write-watch stamp the command's cache checks record (gx_watch_stamp.h).
    template<class F> void emit(F&& f, uint32_t stamp) {
        using T = std::decay_t<F>;
        if (!worker_.joinable()) { f(); return; }
        if (failed_.load(std::memory_order_relaxed)) [[unlikely]] rethrow();
        static_assert(alignof(T) <= kAlign, "command capture over-aligned");
        constexpr uint32_t need = uint32_t((sizeof(Header) + sizeof(T) + kAlign - 1) & ~(kAlign - 1));
        static_assert(need <= kChunkBytes, "command capture too large");
        if (tail_ + need > kChunkBytes) [[unlikely]] next_chunk();
        std::byte* at = current_->data + tail_;
        auto* header = new (at) Header{&run_command<T>, need, stamp};
        new (at + sizeof(Header)) T(std::forward<F>(f));
        (void)header;
        tail_ += need; emitted_ += need;
        current_->published.store(tail_, std::memory_order_seq_cst);   // seq_cst: pairs with the builder's sleeping_ store (no lost wake-up)
        if (sleeping_.load(std::memory_order_seq_cst)) [[unlikely]] wake();
        if (emitted_ - done_.load(std::memory_order_relaxed) > kMaxBacklog) [[unlikely]] wait_until(emitted_ - kMaxBacklog / 2);
    }
    // emit() for a command that takes a run of bytes (copied after the capture): f(std::span<const std::byte>).
    template<class F> void emit_bytes(F&& f, const std::byte* bytes, uint32_t count, uint32_t stamp) {
        using T = std::decay_t<F>;
        if (!worker_.joinable()) { f(std::span<const std::byte>(bytes, count)); return; }
        if (failed_.load(std::memory_order_relaxed)) [[unlikely]] rethrow();
        static_assert(alignof(T) <= kAlign, "command capture over-aligned");
        const uint64_t need = (sizeof(Header) + sizeof(T) + sizeof(uint32_t) + uint64_t(count) + kAlign - 1) & ~uint64_t(kAlign - 1);
        if (need > kChunkBytes) throw std::length_error("GX builder command too large");
        if (tail_ + need > kChunkBytes) [[unlikely]] next_chunk();
        std::byte* at = current_->data + tail_;
        new (at) Header{&run_bytes_command<T>, uint32_t(need), stamp};
        new (at + sizeof(Header)) T(std::forward<F>(f));
        std::memcpy(at + sizeof(Header) + sizeof(T), &count, sizeof count);
        if (count) std::memcpy(at + sizeof(Header) + sizeof(T) + sizeof count, bytes, count);
        tail_ += uint32_t(need); emitted_ += need;
        current_->published.store(tail_, std::memory_order_seq_cst);
        if (sleeping_.load(std::memory_order_seq_cst)) [[unlikely]] wake();
        if (emitted_ - done_.load(std::memory_order_relaxed) > kMaxBacklog) [[unlikely]] wait_until(emitted_ - kMaxBacklog / 2);
    }
    static constexpr uint32_t kMaxBytes = (1u << 20) - 4096;   // the most emit_bytes() takes
    void drain() { if (worker_.joinable()) wait_until(emitted_); }   // front: every emitted command has run
    void check() { if (failed_.load(std::memory_order_relaxed)) rethrow(); }   // front
    [[noreturn]] void rethrow();
    bool failed() const { return failed_.load(std::memory_order_acquire); }
    uint64_t backlog() const { return emitted_ - done_.load(std::memory_order_relaxed); }   // front: bytes of commands not yet run

private:
    static constexpr size_t kAlign = 16;
    static constexpr uint32_t kChunkBytes = 1u << 20;
    static constexpr uint64_t kMaxBacklog = 48ull << 20;   // the front waits when the builder is this far behind
    struct Header { void (*run)(Header*, bool execute); uint32_t size, stamp; };
    static_assert(sizeof(Header) % kAlign == 0);
    struct Chunk {
        std::atomic<uint32_t> published{0};
        std::atomic<Chunk*> next{nullptr};
        alignas(64) std::byte data[kChunkBytes];
    };
    template<class T> static void run_command(Header* header, bool execute) {
        T* command = std::launder(reinterpret_cast<T*>(reinterpret_cast<std::byte*>(header) + sizeof(Header)));
        struct Destroy { T* c; ~Destroy() { c->~T(); } } destroy{command};
        if (execute) (*command)();
    }
    template<class T> static void run_bytes_command(Header* header, bool execute) {
        std::byte* at = reinterpret_cast<std::byte*>(header) + sizeof(Header);
        T* command = std::launder(reinterpret_cast<T*>(at));
        struct Destroy { T* c; ~Destroy() { c->~T(); } } destroy{command};
        uint32_t count; std::memcpy(&count, at + sizeof(T), sizeof count);
        if (execute) (*command)(std::span<const std::byte>(at + sizeof(T) + sizeof count, count));
    }
    void next_chunk();
    void wake();
    void wait_until(uint64_t target);
    void loop();
    Chunk* take_free();

    // front
    Chunk* current_ = nullptr;
    uint32_t tail_ = 0;
    uint64_t emitted_ = 0;
    // shared
    std::atomic<uint64_t> done_{0};
    std::atomic<bool> sleeping_{false}, stop_{false}, failed_{false}, drain_waiting_{false};
    std::exception_ptr failure_;
    std::mutex mutex_;
    std::condition_variable work_, drained_;
    std::vector<Chunk*> free_;
    Chunk* read_chunk_ = nullptr;   // builder
    std::function<void()> on_start_, on_failure_;
    std::thread worker_;
};
}
