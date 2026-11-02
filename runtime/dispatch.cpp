#include "dispatch.h"
#include <algorithm>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__ANDROID__)
#include <unwind.h>
#else
#include <libunwind.h>
#endif
#include <utility>
#include <vector>

namespace uc {
static std::vector<Fn> g_dense;
static uint32_t g_begin, g_bytes;
static std::vector<std::pair<uintptr_t, uint32_t>> g_by_host; // host entry -> guest entry, sorted

void dispatch_init(const FnEntry* table, size_t count, uint32_t text_begin, uint32_t text_end) {
    g_begin = text_begin;
    g_bytes = text_end - text_begin;
    g_dense.assign(g_bytes / 4, nullptr);
    for (size_t i = 0; i < count; ++i) g_dense[(table[i].addr - text_begin) / 4] = table[i].fn;
    g_by_host.clear(); g_by_host.reserve(count);
    for (size_t i = 0; i < count; ++i) g_by_host.push_back({reinterpret_cast<uintptr_t>(table[i].fn), table[i].addr});
    std::sort(g_by_host.begin(), g_by_host.end());
}
// Start of the host function containing pc, from the unwind tables; 0 when pc is not in code with unwind information.
static uintptr_t host_function_start(const void* pc) {
#ifdef _WIN32
    // x64 unwind data names the function containing pc exactly; chained (split hot/cold) entries lead to the primary.
    DWORD64 image = 0;
    auto* entry = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(pc), &image, nullptr);
    for (int depth = 0; entry && depth < 8; ++depth) {
        const auto* info = reinterpret_cast<const uint8_t*>(image + entry->UnwindData);
        if (!((info[0] >> 3) & 4)) break; // UNW_FLAG_CHAININFO
        entry = reinterpret_cast<PRUNTIME_FUNCTION>(const_cast<uint8_t*>(info) + 4 + ((info[2] + 1) & ~1) * 2);
    }
    if (!entry) return 0;
    return uintptr_t(image + entry->BeginAddress);
#elif defined(__ANDROID__)
    // The NDK has no public libunwind API; the unwinder's own lookup finds the function whose unwind entry (DWARF CFI) covers pc.
    return reinterpret_cast<uintptr_t>(_Unwind_FindEnclosingFunction(const_cast<void*>(pc)));
#else
    // libunwind: a cursor moved to pc finds the unwind entry (compact unwind on macOS, DWARF CFI elsewhere) that covers it.
    unw_context_t context;
    unw_cursor_t cursor;
    if (unw_getcontext(&context) != 0 || unw_init_local(&cursor, &context) != 0) return 0;
    if (unw_set_reg(&cursor, UNW_REG_IP, unw_word_t(reinterpret_cast<uintptr_t>(pc))) != 0) return 0;
    unw_proc_info_t info{};
    if (unw_get_proc_info(&cursor, &info) != 0 || info.start_ip == 0) return 0;
    if (reinterpret_cast<uintptr_t>(pc) < info.start_ip || reinterpret_cast<uintptr_t>(pc) >= info.end_ip) return 0;
    return uintptr_t(info.start_ip);
#endif
}
uint32_t guest_function_for_host(const void* pc) {
    const uintptr_t begin = host_function_start(pc);
    if (!begin) return 0;
    auto it = std::lower_bound(g_by_host.begin(), g_by_host.end(), std::pair<uintptr_t, uint32_t>{begin, 0});
    return it != g_by_host.end() && it->first == begin ? it->second : 0;
}
std::vector<uint32_t> function_addresses() {
    std::vector<uint32_t> result;
    for (size_t i = 0; i < g_dense.size(); ++i) if (g_dense[i]) result.push_back(g_begin + uint32_t(i) * 4);
    return result;
}
bool has_function(uint32_t a) { return !(a & 3) && a - g_begin < g_bytes && g_dense[(a - g_begin) >> 2] != nullptr; }
void call_indirect(ppc::Ctx& c, uint32_t a) {
    if (has_function(a)) { g_dense[(a - g_begin) >> 2](c); return; }
    ppc::trap(a, "indirect call or jump to an address that is not a known function entry (add `entry <hex>` to project/uc.cfg)");
}
void hle_sc(ppc::Ctx&, uint32_t pc) { ppc::trap(pc, "sc (system call) is not implemented"); }
}  // namespace uc
