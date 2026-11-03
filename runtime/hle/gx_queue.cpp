#include "gx_queue.h"
#include "gx_watch_stamp.h"
#include <chrono>

namespace gx {
void CommandQueue::start(std::function<void()> on_start, std::function<void()> on_failure) {
    if (worker_.joinable()) return;
    on_start_ = std::move(on_start); on_failure_ = std::move(on_failure);
    stop_ = false; failed_ = false; failure_ = nullptr; sleeping_ = false;
    current_ = take_free(); tail_ = 0; read_chunk_ = current_;
    done_.store(emitted_);
    worker_ = std::thread([this] { loop(); });
}
void CommandQueue::stop() {
    if (!worker_.joinable()) return;
    if (!failed_) { try { drain(); } catch (...) {} }
    { std::lock_guard lock(mutex_); stop_ = true; sleeping_ = false; }
    work_.notify_all();
    worker_.join();
    for (Chunk* c = read_chunk_; c;) { Chunk* n = c->next.load(); delete c; c = n; }   // the chunk chain from the builder's position (commands already run or dropped)
    for (Chunk* c : free_) delete c;
    free_.clear(); current_ = read_chunk_ = nullptr; tail_ = 0;
}
CommandQueue::Chunk* CommandQueue::take_free() {
    {
        std::lock_guard lock(mutex_);
        if (!free_.empty()) { Chunk* c = free_.back(); free_.pop_back(); c->published.store(0, std::memory_order_relaxed); c->next.store(nullptr, std::memory_order_relaxed); return c; }
    }
    return new Chunk;
}
void CommandQueue::next_chunk() {
    Chunk* fresh = take_free();
    current_->next.store(fresh, std::memory_order_release);   // after the old chunk's last publish: the builder re-reads `published` once it sees `next`
    current_ = fresh; tail_ = 0;
}
void CommandQueue::wake() {
    { std::lock_guard lock(mutex_); sleeping_.store(false, std::memory_order_seq_cst); }
    work_.notify_one();
}
void CommandQueue::rethrow() {
    std::exception_ptr failure;
    { std::lock_guard lock(mutex_); failure = failure_; }
    std::rethrow_exception(failure);
}
void CommandQueue::wait_until(uint64_t target) {
    // The builder is usually busy: spin briefly before sleeping (a futex wake-up costs tens of microseconds on Android).
    for (int i = 0; i < 2000; ++i) {
        if (done_.load(std::memory_order_seq_cst) >= target) { if (failed_.load(std::memory_order_seq_cst)) rethrow(); return; }   // a failure is set before the done count passes it
        if (failed_.load(std::memory_order_acquire)) rethrow();
        if (sleeping_.load(std::memory_order_relaxed)) wake();
        std::this_thread::yield();
    }
    std::unique_lock lock(mutex_);
    drain_waiting_.store(true, std::memory_order_seq_cst);
    drained_.wait(lock, [&] { return done_.load(std::memory_order_seq_cst) >= target || failed_.load(std::memory_order_seq_cst); });
    drain_waiting_.store(false, std::memory_order_relaxed);
    if (failed_.load(std::memory_order_seq_cst)) { lock.unlock(); rethrow(); }
}
void CommandQueue::loop() {
    command_stamp = 1;   // older than every store: a check before the first stamped command compares the bytes
    if (on_start_) on_start_();
    Chunk* chunk = read_chunk_;
    uint32_t read = 0;
    int idle = 0;
    for (;;) {
        uint32_t published = chunk->published.load(std::memory_order_acquire);
        if (read == published) {
            if (Chunk* next = chunk->next.load(std::memory_order_acquire)) {
                published = chunk->published.load(std::memory_order_acquire);
                if (read == published) {
                    { std::lock_guard lock(mutex_); free_.push_back(chunk); read_chunk_ = next; }
                    chunk = next; read = 0; continue;
                }
            }
        }
        if (read < published) {
            idle = 0;
            uint64_t ran = 0;
            while (read < published) {
                auto* header = reinterpret_cast<Header*>(chunk->data + read);
                const uint32_t size = header->size;
                const bool execute = !failed_.load(std::memory_order_relaxed);
                command_stamp = header->stamp ? header->stamp : command_stamp;
                try { header->run(header, execute); }
                catch (...) {
                    { std::lock_guard lock(mutex_); failure_ = std::current_exception(); }
                    if (on_failure_) { try { on_failure_(); } catch (...) {} }
                    { std::lock_guard lock(mutex_); failed_.store(true, std::memory_order_seq_cst); }
                    drained_.notify_all();
                }
                read += size; ran += size;
            }
            done_.fetch_add(ran, std::memory_order_seq_cst);
            if (drain_waiting_.load(std::memory_order_seq_cst)) { std::lock_guard lock(mutex_); drained_.notify_all(); }
            continue;
        }
        if (stop_.load(std::memory_order_acquire)) return;
        if (++idle < 256) { std::this_thread::yield(); continue; }   // about 50-100 us of polling before sleeping
        std::unique_lock lock(mutex_);
        sleeping_.store(true, std::memory_order_seq_cst);
        if (chunk->published.load(std::memory_order_seq_cst) != read || chunk->next.load(std::memory_order_seq_cst)) { sleeping_.store(false); continue; }
        work_.wait(lock, [&] { return !sleeping_.load(std::memory_order_relaxed) || stop_.load(std::memory_order_relaxed); });
        idle = 0;
    }
}
}
