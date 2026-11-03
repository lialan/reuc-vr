#pragma once
// Byte swaps, rotates and the compiler barrier of the PowerPC runtime on every host compiler (clang-cl, Apple clang, the Android NDK).
// The builtins lower to one instruction: bswap/rol on x86-64, rev/rev16/ror on arm64.
#include <atomic>
#include <bit>
#include <cstdint>

static_assert(std::endian::native == std::endian::little, "the guest memory accessors byte-swap big-endian data into a little-endian host");

constexpr uint16_t uc_bswap16(uint16_t v) { return __builtin_bswap16(v); }
constexpr uint32_t uc_bswap32(uint32_t v) { return __builtin_bswap32(v); }
constexpr uint64_t uc_bswap64(uint64_t v) { return __builtin_bswap64(v); }
constexpr uint32_t uc_rotl32(uint32_t v, int n) { return std::rotl(v, n); }
// Orders the compiler's memory accesses only (sync/eieio between guest instructions: one guest thread runs at a time, see sched.cpp).
inline void uc_compiler_barrier() { std::atomic_signal_fence(std::memory_order_seq_cst); }
// Data the translated code touches on every guest store: one image (the game executable or libmain.so) holds every user, so the accesses
// can be PC-relative instead of going through the GOT (a default-visibility variable in a shared library costs an extra load per use).
#if defined(__ELF__) || defined(__APPLE__)
#define UC_IMAGE_LOCAL __attribute__((visibility("hidden")))
#else
#define UC_IMAGE_LOCAL
#endif
