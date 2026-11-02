#pragma once
// SHA-256 (FIPS 180-4) without a platform library: mod DLL fingerprints on every host (Windows used BCrypt for it).
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>

namespace uc {
class Sha256 {
public:
    void update(std::span<const uint8_t> bytes) {
        total_ += bytes.size();
        size_t at = 0;
        if (fill_) {
            const size_t take = std::min(bytes.size(), block_.size() - fill_);
            std::memcpy(block_.data() + fill_, bytes.data(), take);
            fill_ += take; at = take;
            if (fill_ < block_.size()) return;
            compress(block_.data()); fill_ = 0;
        }
        for (; at + 64 <= bytes.size(); at += 64) compress(bytes.data() + at);
        fill_ = bytes.size() - at;
        if (fill_) std::memcpy(block_.data(), bytes.data() + at, fill_);
    }
    std::array<uint8_t, 32> finish() {
        const uint64_t bits = total_ * 8;
        const uint8_t pad = 0x80;
        update(std::span<const uint8_t>(&pad, 1));
        const uint8_t zero[64] = {};
        update(std::span<const uint8_t>(zero, (fill_ <= 56 ? 56 - fill_ : 120 - fill_)));
        uint8_t length[8];
        for (int i = 0; i < 8; ++i) length[i] = uint8_t(bits >> (56 - 8 * i));
        update(std::span<const uint8_t>(length, 8));
        std::array<uint8_t, 32> digest{};
        for (int i = 0; i < 8; ++i) for (int b = 0; b < 4; ++b) digest[size_t(i * 4 + b)] = uint8_t(state_[size_t(i)] >> (24 - 8 * b));
        return digest;
    }

private:
    std::array<uint32_t, 8> state_{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au, 0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    std::array<uint8_t, 64> block_{};
    size_t fill_ = 0;
    uint64_t total_ = 0;

    void compress(const uint8_t* p) {
        static constexpr uint32_t k[64] = {
            0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu,
            0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau,
            0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u,
            0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
            0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
            0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[4 * i]) << 24 | uint32_t(p[4 * i + 1]) << 16 | uint32_t(p[4 * i + 2]) << 8 | p[4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = std::rotr(w[i - 15], 7) ^ std::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = std::rotr(w[i - 2], 17) ^ std::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3], e = state_[4], f = state_[5], g = state_[6], h = state_[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = h + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const uint32_t t2 = (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d; state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
    }
};

inline std::string to_hex(std::span<const uint8_t> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (uint8_t b : bytes) { text += digits[b >> 4]; text += digits[b & 15]; }
    return text;
}
inline std::string sha256_hex(std::span<const uint8_t> bytes) { Sha256 h; h.update(bytes); return to_hex(h.finish()); }
}  // namespace uc
