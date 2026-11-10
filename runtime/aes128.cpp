#include "aes128.h"
#include <cstring>
#if defined(__aarch64__) && defined(__ARM_FEATURE_AES)
#include <arm_neon.h>
#define UC_AES_ARM 1
#endif

namespace uc {
namespace {

uint8_t xtime(uint8_t x) { return uint8_t((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
uint8_t gmul(uint8_t a, uint8_t b) {
    uint8_t p = 0;
    for (; b; b >>= 1, a = xtime(a)) if (b & 1) p ^= a;
    return p;
}

struct Tables {
    uint8_t sbox[256], inv[256];
    Tables() {
        for (int x = 0; x < 256; ++x) {
            uint8_t inverse = 0;   // multiplicative inverse in GF(2^8); 0 maps to 0
            if (x) for (int y = 1; y < 256; ++y) if (gmul(uint8_t(x), uint8_t(y)) == 1) { inverse = uint8_t(y); break; }
            uint8_t s = inverse;
            for (int r = 1; r <= 4; ++r) s ^= uint8_t((inverse << r) | (inverse >> (8 - r)));
            s ^= 0x63;
            sbox[x] = s;
            inv[s] = uint8_t(x);
        }
    }
};
const Tables& tables() { static const Tables t; return t; }

void add_key(uint8_t s[16], const uint8_t* k) { for (int i = 0; i < 16; ++i) s[i] ^= k[i]; }
void mix_column(uint8_t* c) {
    const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
    c[0] = uint8_t(gmul(a0, 2) ^ gmul(a1, 3) ^ a2 ^ a3);
    c[1] = uint8_t(a0 ^ gmul(a1, 2) ^ gmul(a2, 3) ^ a3);
    c[2] = uint8_t(a0 ^ a1 ^ gmul(a2, 2) ^ gmul(a3, 3));
    c[3] = uint8_t(gmul(a0, 3) ^ a1 ^ a2 ^ gmul(a3, 2));
}
void inv_mix_column(uint8_t* c) {
    const uint8_t a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
    c[0] = uint8_t(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
    c[1] = uint8_t(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
    c[2] = uint8_t(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
    c[3] = uint8_t(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
}

#ifndef UC_AES_ARM
// The decryption round's multiplications by 9, 11, 13, 14 as tables (the reader decrypts megabytes; gmul's loop per byte would be slow).
struct InvMix { uint8_t m9[256], m11[256], m13[256], m14[256]; uint8_t m2[256], m3[256];
    InvMix() { for (int x = 0; x < 256; ++x) { m9[x] = gmul(uint8_t(x), 9); m11[x] = gmul(uint8_t(x), 11); m13[x] = gmul(uint8_t(x), 13); m14[x] = gmul(uint8_t(x), 14);
                                               m2[x] = gmul(uint8_t(x), 2); m3[x] = gmul(uint8_t(x), 3); } } };
const InvMix& inv_mix() { static const InvMix t; return t; }
#endif

}  // namespace

Aes128::Aes128(const uint8_t key[16]) {
    const auto& t = tables();
    std::memcpy(round_keys_.data(), key, 16);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; ++i) {
        uint8_t w[4];
        std::memcpy(w, &round_keys_[size_t(i - 1) * 4], 4);
        if (i % 4 == 0) {
            const uint8_t first = w[0];
            w[0] = uint8_t(t.sbox[w[1]] ^ rcon); w[1] = t.sbox[w[2]]; w[2] = t.sbox[w[3]]; w[3] = t.sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j) round_keys_[size_t(i) * 4 + size_t(j)] = uint8_t(round_keys_[size_t(i - 4) * 4 + size_t(j)] ^ w[j]);
    }
    for (int r = 0; r <= 10; ++r) std::memcpy(&inverse_keys_[size_t(r) * 16], &round_keys_[size_t(10 - r) * 16], 16);
    for (int r = 1; r < 10; ++r) for (int c = 0; c < 4; ++c) inv_mix_column(&inverse_keys_[size_t(r) * 16 + size_t(c) * 4]);
}

void Aes128::encrypt_block(const uint8_t in[16], uint8_t out[16]) const {
#ifdef UC_AES_ARM
    uint8x16_t s = vld1q_u8(in);
    for (int r = 0; r < 9; ++r) s = vaesmcq_u8(vaeseq_u8(s, vld1q_u8(&round_keys_[size_t(r) * 16])));
    s = veorq_u8(vaeseq_u8(s, vld1q_u8(&round_keys_[144])), vld1q_u8(&round_keys_[160]));
    vst1q_u8(out, s);
#else
    const auto& t = tables();
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_key(s, &round_keys_[0]);
    for (int r = 1; r <= 10; ++r) {
        uint8_t n[16];
        for (int c = 0; c < 4; ++c) for (int row = 0; row < 4; ++row) n[c * 4 + row] = t.sbox[s[((c + row) % 4) * 4 + row]];   // SubBytes + ShiftRows
        if (r != 10) for (int c = 0; c < 4; ++c) mix_column(&n[c * 4]);
        add_key(n, &round_keys_[size_t(r) * 16]);
        std::memcpy(s, n, 16);
    }
    std::memcpy(out, s, 16);
#endif
}

void Aes128::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
#ifdef UC_AES_ARM
    uint8x16_t s = vld1q_u8(in);
    for (int r = 0; r < 9; ++r) s = vaesimcq_u8(vaesdq_u8(s, vld1q_u8(&inverse_keys_[size_t(r) * 16])));
    s = veorq_u8(vaesdq_u8(s, vld1q_u8(&inverse_keys_[144])), vld1q_u8(&inverse_keys_[160]));
    vst1q_u8(out, s);
#else
    const auto& t = tables();
    const auto& m = inv_mix();
    uint8_t s[16];
    std::memcpy(s, in, 16);
    add_key(s, &round_keys_[160]);
    for (int r = 9; r >= 0; --r) {
        uint8_t n[16];
        for (int c = 0; c < 4; ++c) for (int row = 0; row < 4; ++row) n[((c + row) % 4) * 4 + row] = t.inv[s[c * 4 + row]];   // InvShiftRows + InvSubBytes
        add_key(n, &round_keys_[size_t(r) * 16]);
        if (r != 0) for (int c = 0; c < 4; ++c) {
            uint8_t* col = &n[c * 4];
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = uint8_t(m.m14[a0] ^ m.m11[a1] ^ m.m13[a2] ^ m.m9[a3]);
            col[1] = uint8_t(m.m9[a0] ^ m.m14[a1] ^ m.m11[a2] ^ m.m13[a3]);
            col[2] = uint8_t(m.m13[a0] ^ m.m9[a1] ^ m.m14[a2] ^ m.m11[a3]);
            col[3] = uint8_t(m.m11[a0] ^ m.m13[a1] ^ m.m9[a2] ^ m.m14[a3]);
        }
        std::memcpy(s, n, 16);
    }
    std::memcpy(out, s, 16);
#endif
}

void Aes128::cbc_decrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t size) const {
    uint8_t chain[16], next[16], plain[16];
    std::memcpy(chain, iv, 16);
    for (size_t at = 0; at + 16 <= size; at += 16) {
        std::memcpy(next, in + at, 16);   // before out is written: in and out may alias
        decrypt_block(next, plain);
        for (int i = 0; i < 16; ++i) out[at + size_t(i)] = uint8_t(plain[i] ^ chain[i]);
        std::memcpy(chain, next, 16);
    }
}

void Aes128::cbc_encrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t size) const {
    uint8_t chain[16];
    std::memcpy(chain, iv, 16);
    for (size_t at = 0; at + 16 <= size; at += 16) {
        uint8_t block[16];
        for (int i = 0; i < 16; ++i) block[i] = uint8_t(in[at + size_t(i)] ^ chain[i]);
        encrypt_block(block, chain);
        std::memcpy(out + at, chain, 16);
    }
}

}  // namespace uc
