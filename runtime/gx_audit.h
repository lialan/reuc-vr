#pragma once
#include "ppc_rt.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace uc {
struct GxResourceAudit {
    uint32_t address = 0;
    size_t size = 0;
    std::vector<std::byte> bytes;
};
struct GxAuditEntry {
    uint32_t address = 0;
    uint64_t calls = 0;
    uint64_t nonempty_lists = 0, list_bytes = 0;
    uint64_t recording_bytes = 0;
    uint32_t first_caller = 0;
    std::array<uint8_t, 32> first_recording_prefix{};
    size_t first_recording_size = 0;
    std::vector<GxResourceAudit> resources;
    std::array<uint32_t, 8> first_gpr{};
    std::array<double, 6> first_fpr{};
};
void gx_audit_enable();
// --gx-audit only. The flag is tested inline: the entry hook runs on every GX call and the FIFO hook on every FIFO write.
inline std::atomic<bool> gx_audit_on{false};
void gx_audit_enter_slow(uint32_t address, const ppc::Ctx& ctx);
void gx_audit_fifo_write_slow(int size, uint32_t value);
inline void gx_audit_enter(uint32_t address, const ppc::Ctx& ctx) { if (gx_audit_on.load(std::memory_order_relaxed)) [[unlikely]] gx_audit_enter_slow(address, ctx); }
inline void gx_audit_fifo_write(int size, uint32_t value) { if (gx_audit_on.load(std::memory_order_relaxed)) [[unlikely]] gx_audit_fifo_write_slow(size, value); }
void gx_audit_recorded_resource(uint32_t address, std::span<const std::byte> bytes);
std::vector<GxAuditEntry> gx_audit_snapshot();
}
