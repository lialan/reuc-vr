// The GX builder queue (runtime/hle/gx_queue.h): commands run in order on the builder thread, drain() waits for all of them, a failing
// command stops the builder and the error comes back on the front, byte payloads arrive intact, chunks are crossed and recycled.
#include "check.h"
#include "gx_queue.h"
#include "gx_watch_stamp.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

int main() {
    {   // without the builder thread a command runs at once
        gx::CommandQueue queue;
        int value = 0;
        queue.emit([&] { value = 7; }, 0);
        CHECK(value == 7 && !queue.threaded());
        std::byte bytes[3] = {std::byte{1}, std::byte{2}, std::byte{3}};
        size_t seen = 0;
        queue.emit_bytes([&](std::span<const std::byte> b) { seen = b.size() + size_t(b[2]); }, bytes, 3, 0);
        CHECK(seen == 6);
    }
    {   // order, drain, stamps, many chunks
        gx::CommandQueue queue;
        std::atomic<bool> started{false};
        std::thread::id builder;
        queue.start([&] { started = true; builder = std::this_thread::get_id(); }, nullptr);
        CHECK(queue.threaded());
        std::vector<uint32_t> order; order.reserve(300000);
        uint32_t stamps_ok = 1;
        for (uint32_t i = 0; i < 300000; ++i) {
            std::array<uint64_t, 6> padding{};   // ~64-byte commands: 300k of them span many 1 MiB chunks
            padding[0] = i;
            queue.emit([&order, &stamps_ok, padding, i] { order.push_back(uint32_t(padding[0])); if (gx::command_stamp != 100 + i % 7) stamps_ok = 0; }, 100 + i % 7);
            if (i % 50000 == 0) queue.drain();
        }
        std::vector<std::byte> payload(70000);
        for (size_t i = 0; i < payload.size(); ++i) payload[i] = std::byte(uint8_t(i * 31));
        bool payload_ok = false;
        queue.emit_bytes([&](std::span<const std::byte> b) {
            payload_ok = b.size() == 70000;
            for (size_t i = 0; payload_ok && i < b.size(); ++i) payload_ok = b[i] == std::byte(uint8_t(i * 31));
            payload_ok = payload_ok && gx::on_builder_thread() && std::this_thread::get_id() == builder;
        }, payload.data(), uint32_t(payload.size()), 5);
        queue.drain();
        CHECK(started && order.size() == 300000 && payload_ok && stamps_ok);
        for (uint32_t i = 0; i < order.size(); ++i) CHECK(order[i] == i);
        CHECK(queue.backlog() == 0);
        // The builder sleeps when idle and wakes for the next command.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        int late = 0; queue.emit([&] { late = 1; }, 1); queue.drain(); CHECK(late == 1);
        queue.stop();
        CHECK(!queue.threaded());
    }
    {   // a failing command: later commands are dropped (destroyed, not run), the front rethrows, on_failure ran
        gx::CommandQueue queue;
        std::atomic<int> failures{0};
        queue.start(nullptr, [&] { ++failures; });
        int ran = 0;
        auto counted = std::make_shared<int>(0);
        queue.emit([&] { ++ran; }, 1);
        queue.emit([] { throw std::runtime_error("builder failure"); }, 1);
        queue.emit([&, counted] { ++ran; }, 1);
        bool thrown = false;
        try { queue.drain(); } catch (const std::runtime_error& error) { thrown = std::string(error.what()) == "builder failure"; }
        CHECK(thrown && ran == 1 && failures == 1 && queue.failed());
        CHECK(counted.use_count() == 1);   // the dropped command's capture was destroyed
        thrown = false;
        try { queue.emit([] {}, 1); } catch (const std::runtime_error&) { thrown = true; }
        CHECK(thrown);
        queue.stop();
    }
    {   // a command emitted while the builder is going to sleep or asleep runs without a drain, soon (the front publishes without a
        // store-load barrier; a wake-up it misses costs the builder's sleep timeout, never a hang)
        gx::CommandQueue queue;
        queue.start(nullptr, nullptr);
        std::atomic<int> ran{0};
        double worst_ms = 0;
        for (int i = 0; i < 300; ++i) {
            std::this_thread::sleep_for(std::chrono::microseconds(i % 3 == 0 ? 20 : i % 3 == 1 ? 150 : 900));   // around the builder's spin
            const auto sent = std::chrono::steady_clock::now();
            queue.emit([&] { ran.fetch_add(1, std::memory_order_release); }, 1);
            while (ran.load(std::memory_order_acquire) != i + 1 && std::chrono::steady_clock::now() - sent < std::chrono::seconds(2)) std::this_thread::yield();
            worst_ms = std::max(worst_ms, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - sent).count());
            CHECK(ran.load() == i + 1);
        }
        std::printf("test_gx_queue: worst wake-up %.2f ms over 300 sleeps of the builder\n", worst_ms);
        CHECK(worst_ms < 50);
        queue.stop();
    }
    std::puts("test_gx_queue: ok");
    return 0;
}
