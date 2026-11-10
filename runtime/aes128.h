#pragma once
// AES-128 (FIPS 197) and CBC mode, for the Wii disc reader (runtime/wii_disc.h): the title key and every 0x7C00-byte data block of a
// partition are AES-128-CBC encrypted. The S-boxes are computed at start-up (GF(2^8) inverse + affine map) instead of being written out.
// On arm64 with the crypto extension the block rounds use the AESD/AESIMC instructions.
#include <array>
#include <cstddef>
#include <cstdint>

namespace uc {

class Aes128 {
public:
    explicit Aes128(const uint8_t key[16]);
    void encrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    // CBC over whole 16-byte blocks (size must be a multiple of 16); in and out may be the same buffer. iv is not changed.
    void cbc_decrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t size) const;
    void cbc_encrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t size) const;
private:
    std::array<uint8_t, 176> round_keys_{};   // 11 round keys, encryption order
    std::array<uint8_t, 176> inverse_keys_{}; // decryption order, InvMixColumns applied to the middle ones (equivalent inverse cipher)
};

}  // namespace uc
