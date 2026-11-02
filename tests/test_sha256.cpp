// SHA-256 against the FIPS 180-2 test vectors, in one piece and fed in uneven chunks.
#include "sha256.h"
#include <cstdio>
#include <string>
#include <string_view>

static int failures;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

static std::string hex_of(std::string_view text) {
    return uc::sha256_hex(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

int main() {
    CHECK(hex_of("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(hex_of("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(hex_of("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // One million 'a' in chunks of 997 bytes: block boundaries never line up with the chunks.
    uc::Sha256 h;
    const std::string chunk(997, 'a');
    size_t left = 1000000;
    while (left) { const size_t n = left < chunk.size() ? left : chunk.size(); h.update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(chunk.data()), n)); left -= n; }
    CHECK(uc::to_hex(h.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    if (failures) return 1;
    std::printf("test_sha256: ok\n");
    return 0;
}
