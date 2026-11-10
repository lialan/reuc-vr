// The disc image reader (runtime/wii_disc.h) on small synthetic Wii discs written here: a plain .iso and the same disc in a .wbfs, encrypted
// with a test key passed where the player's common key goes. No game data. With UC_TEST_WII_IMAGE (a real image), UC_TEST_WII_UNPACKED (its
// unpacked DATA folder) and the common key (UC_WII_COMMON_KEY), every file of the image is also compared with the unpacked one.
#include "aes128.h"
#include "check.h"
#include "dvdfs.h"
#include "wii_disc.h"
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace uc;

namespace {

constexpr WiiKey kTestCommon = {0x10, 0x32, 0x54, 0x76, 0x98, 0xBA, 0xDC, 0xFE, 0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
constexpr uint8_t kTitleKey[16] = {0xA5, 0x5A, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14};
constexpr uint64_t kPartition = 0x50000, kDataOffset = 0x20000;

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) { b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v); }

std::vector<uint8_t> pattern(size_t size, uint8_t seed) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t(seed + i * 7 + (i >> 8));
    return bytes;
}

struct Spec { std::string path; std::vector<uint8_t> bytes; };   // a file; a path ending in '/' is a folder
// The files, in an FST order that is not the sorted one (zeta before Alpha), and one file larger than a 0x7C00-byte block.
std::vector<Spec> specs() {
    return {{"Data/", {}}, {"Data/zeta.bin", pattern(100, 1)}, {"Data/Alpha.bin", pattern(0x7C00 * 2 + 123, 2)}, {"Data/sub/", {}},
            {"Data/sub/c.txt", {'s', 'e', 'e'}}, {"top.txt", {'t', 'o', 'p'}}};
}
std::vector<uint8_t> dol_bytes() {   // a DOL header with one text section of 0x40 bytes right after it
    std::vector<uint8_t> dol(0x140, 0);
    put32(dol, 0x00, 0x100); put32(dol, 0x48, 0x80004000); put32(dol, 0x90, 0x40);
    for (size_t i = 0x100; i < dol.size(); ++i) dol[i] = uint8_t(i);
    return dol;
}

// The partition's decrypted data: boot.bin, the DOL at 0x2440, the FST at 0x3000, the files from 0x8000 on.
std::vector<uint8_t> partition_data() {
    std::vector<uint8_t> data(0x40000, 0);
    std::memcpy(data.data(), "RTST01", 6);
    put32(data, 0x18, 0x5D1C9EA3);
    const auto dol = dol_bytes();
    std::memcpy(&data[0x2440], dol.data(), dol.size());
    put32(data, 0x420, 0x2440 >> 2);
    // FST: entries (the root first), then the names. A folder's entry: parent, index one past its last entry.
    const auto files = specs();
    std::vector<uint8_t> entries((files.size() + 1) * 12, 0), names;
    entries[0] = 1; put32(entries, 8, uint32_t(files.size() + 1));
    uint64_t file_at = 0x8000;
    std::map<std::string, size_t> folder_index;
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        const bool dir = f.path.back() == '/';
        const std::string path = dir ? f.path.substr(0, f.path.size() - 1) : f.path;
        const std::string name = path.substr(path.rfind('/') == std::string::npos ? 0 : path.rfind('/') + 1);
        const size_t e = (i + 1) * 12;
        put32(entries, e, uint32_t(names.size()));
        names.insert(names.end(), name.begin(), name.end()); names.push_back(0);
        if (dir) {
            entries[e] = 1;
            size_t end = i + 1;
            while (end < files.size() && files[end].path.compare(0, f.path.size(), f.path) == 0) ++end;
            const size_t parent_slash = path.rfind('/');
            put32(entries, e + 4, parent_slash == std::string::npos ? 0 : uint32_t(folder_index[path.substr(0, parent_slash)]));
            put32(entries, e + 8, uint32_t(end + 1));
            folder_index[path] = i + 1;
        } else {
            put32(entries, e + 4, uint32_t(file_at >> 2));
            put32(entries, e + 8, uint32_t(f.bytes.size()));
            std::memcpy(&data[file_at], f.bytes.data(), f.bytes.size());
            file_at = (file_at + f.bytes.size() + 3) & ~uint64_t(3);
        }
    }
    std::vector<uint8_t> fst = entries;
    fst.insert(fst.end(), names.begin(), names.end());
    while (fst.size() % 4) fst.push_back(0);
    std::memcpy(&data[0x3000], fst.data(), fst.size());
    put32(data, 0x424, 0x3000 >> 2); put32(data, 0x428, uint32_t(fst.size() >> 2));
    data.resize((data.size() + 0x7BFF) / 0x7C00 * 0x7C00);
    return data;
}

// The whole disc as a plain image: header, partition table, the ticket and the encrypted data blocks.
std::vector<uint8_t> disc_image(const WiiKey& common) {
    const auto data = partition_data();
    const size_t blocks = data.size() / 0x7C00;
    std::vector<uint8_t> disc(kPartition + kDataOffset + blocks * 0x8000, 0);
    std::memcpy(disc.data(), "RTST01", 6);
    put32(disc, 0x18, 0x5D1C9EA3);
    put32(disc, 0x40000, 1); put32(disc, 0x40004, 0x40020 >> 2);
    put32(disc, 0x40020, uint32_t(kPartition >> 2)); put32(disc, 0x40024, 0);
    uint8_t iv[16] = {};
    std::memcpy(iv, "TITLEID1", 8);
    std::memcpy(&disc[kPartition + 0x1DC], iv, 8);
    Aes128(common.data()).cbc_encrypt(iv, kTitleKey, &disc[kPartition + 0x1BF], 16);
    put32(disc, kPartition + 0x2B8, uint32_t(kDataOffset >> 2)); put32(disc, kPartition + 0x2BC, uint32_t((blocks * 0x8000) >> 2));
    const Aes128 title(kTitleKey);
    for (size_t b = 0; b < blocks; ++b) {
        uint8_t* block = &disc[kPartition + kDataOffset + b * 0x8000];
        for (size_t i = 0; i < 0x400; ++i) block[i] = uint8_t(b * 31 + i);   // stand-in hashes; 0x3D0..0x3DF is the IV
        title.cbc_encrypt(block + 0x3D0, &data[b * 0x7C00], block + 0x400, 0x7C00);
    }
    return disc;
}

// The same disc in a WBFS file with 256 KiB blocks (block 0 holds the header and the table); disc blocks of zeros are not stored.
std::vector<uint8_t> wbfs_image(const std::vector<uint8_t>& disc) {
    constexpr uint32_t shift = 18, size = 1u << shift;
    const size_t count = (143432 * 2) >> (shift - 15);
    std::vector<uint8_t> out(size, 0);   // block 0: the WBFS header and disc 0's table
    std::memcpy(out.data(), "WBFS", 4);
    out[8] = 9; out[9] = uint8_t(shift);
    CHECK(0x200 + 0x100 + count * 2 <= size);
    std::memcpy(&out[0x200], disc.data(), 0x100);
    uint16_t next = 1;
    for (size_t b = 0; b * size < disc.size(); ++b) {
        const size_t take = std::min<size_t>(size, disc.size() - b * size);
        bool zero = true;
        for (size_t i = 0; i < take && zero; ++i) zero = disc[b * size + i] == 0;
        if (zero) continue;
        out[0x300 + b * 2] = uint8_t(next >> 8); out[0x301 + b * 2] = uint8_t(next);
        out.resize(size_t(next + 1) * size, 0);
        std::memcpy(&out[size_t(next) * size], &disc[b * size], take);
        ++next;
    }
    return out;
}

fs::path write(const char* name, const std::vector<uint8_t>& bytes) {
    const fs::path p = fs::current_path() / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    return p;
}

std::vector<uint8_t> read_file(const DvdFs& dvd, int e) {
    std::vector<uint8_t> bytes(dvd.length(e));
    CHECK(dvd.read(e, bytes.data(), uint32_t(bytes.size()), 0) == int64_t(bytes.size()));
    return bytes;
}

void aes_known_answer() {   // FIPS 197 appendix C.1
    const uint8_t key[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    const uint8_t plain[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    const uint8_t cipher[16] = {0x69, 0xC4, 0xE0, 0xD8, 0x6A, 0x7B, 0x04, 0x30, 0xD8, 0xCD, 0xB7, 0x80, 0x70, 0xB4, 0xC5, 0x5A};
    const Aes128 aes(key);
    uint8_t out[16];
    aes.encrypt_block(plain, out); CHECK(std::memcmp(out, cipher, 16) == 0);
    aes.decrypt_block(cipher, out); CHECK(std::memcmp(out, plain, 16) == 0);
}

void reads_image(const fs::path& image) {
    std::string error;
    auto disc = WiiDisc::open(image, kTestCommon, error);
    if (!disc) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(disc && disc->game_id() == "RTST01");
    CHECK(disc->main_dol() == dol_bytes());
    CHECK(disc->boot_bin().size() == 0x440);
    std::map<std::string, std::vector<uint8_t>> want;
    for (const auto& s : specs()) if (s.path.back() != '/') want[s.path] = s.bytes;
    size_t seen = 0;
    for (const auto& f : disc->files()) {
        if (f.dir) continue;
        std::vector<uint8_t> bytes(f.size);
        CHECK(disc->read(f.offset, bytes.data(), bytes.size()));
        CHECK(want.at(f.path) == bytes);
        ++seen;
    }
    CHECK(seen == want.size());

    // Through DvdFs: the same entries, numbered as an unpacked folder of these files would be.
    DvdFs dvd;
    CHECK(dvd.open_disc(std::move(disc)));
    const fs::path folder = fs::current_path() / "wii_disc_unpacked";
    fs::remove_all(folder);
    for (const auto& s : specs()) {
        if (s.path.back() == '/') { fs::create_directories(folder / s.path); continue; }
        std::ofstream(folder / s.path, std::ios::binary).write(reinterpret_cast<const char*>(s.bytes.data()), std::streamsize(s.bytes.size()));
    }
    DvdFs unpacked;
    CHECK(unpacked.open_root(folder));
    CHECK(dvd.entry_count() == unpacked.entry_count());
    for (int e = 0; e < int(dvd.entry_count()); ++e) {
        CHECK(dvd.path_of(e) == unpacked.path_of(e));
        CHECK(dvd.is_dir(e) == unpacked.is_dir(e));
        if (!dvd.is_dir(e)) CHECK(read_file(dvd, e) == read_file(unpacked, e));
    }
    const int alpha = dvd.entry_of("data/alpha.bin");
    uint8_t piece[300];
    CHECK(dvd.read(alpha, piece, sizeof piece, 0x7C00 - 100) == int64_t(sizeof piece));   // across a block boundary
    CHECK(std::memcmp(piece, &want.at("Data/Alpha.bin")[0x7C00 - 100], sizeof piece) == 0);
    const auto snap = dvd.snapshot(alpha);
    CHECK(snap && snap->on_disc && dvd.read(*snap, piece, 10, 5) == 10);
    CHECK(dvd.disc_file("DATA/SUB/C.TXT", 100) == std::vector<uint8_t>({'s', 'e', 'e'}));
    CHECK(!dvd.disc_file("Data/Alpha.bin", 100));   // above the limit
    // An overlay replaces a disc file and taking it back returns the image's bytes.
    const fs::path mod = fs::current_path() / "wii_disc_mod";
    fs::remove_all(mod);
    fs::create_directories(mod / "Data");
    std::ofstream(mod / "Data" / "zeta.bin") << "mod";
    CHECK(dvd.overlay(mod) == 1);
    CHECK(read_file(dvd, dvd.entry_of("Data/zeta.bin")) == std::vector<uint8_t>({'m', 'o', 'd'}));
    CHECK(dvd.disc_file("Data/zeta.bin", 1000) == want.at("Data/zeta.bin"));   // the game's own file, whatever the overlay holds
    dvd.clear_overlays();
    CHECK(read_file(dvd, dvd.entry_of("Data/zeta.bin")) == want.at("Data/zeta.bin"));
}

void refuses_bad_images() {
    std::string error;
    WiiKey wrong = kTestCommon; wrong[0] ^= 1;
    CHECK(!WiiDisc::open(fs::current_path() / "wii_disc.iso", wrong, error) && error.find("common key") != std::string::npos);
    CHECK(!WiiDisc::open(write("wii_disc_short.iso", std::vector<uint8_t>(100, 0)), kTestCommon, error));
    CHECK(!WiiDisc::open(write("wii_disc_zero.iso", std::vector<uint8_t>(0x1000, 0)), kTestCommon, error) && error == "not a Wii disc image");
    CHECK(!WiiDisc::open(fs::current_path() / "missing.wbfs", kTestCommon, error));
}

void key_loading() {
    const fs::path file = write("wii_disc_key.bin", std::vector<uint8_t>(kTestCommon.begin(), kTestCommon.end()));
    const fs::path bad = write("wii_disc_key_short.bin", std::vector<uint8_t>(15, 1));
    const char* env = std::getenv("UC_WII_COMMON_KEY");
    if (env && *env) return;   // a real key in the environment wins over files; the rest is covered without it
    CHECK(load_wii_common_key({fs::current_path() / "missing.bin", bad, file}) == kTestCommon);
    CHECK(is_wii_image_name("a/RBUE08.WBFS") && is_wii_image_name("x.iso") && !is_wii_image_name("x.rvz"));
}

// Optional, local only: a real image against its unpacked folder.
void real_image() {
    const char* image = std::getenv("UC_TEST_WII_IMAGE");
    const char* unpacked = std::getenv("UC_TEST_WII_UNPACKED");
    if (!image || !unpacked) return;
    const auto key = load_wii_common_key({});
    CHECK(key);
    std::string error;
    std::shared_ptr<const WiiDisc> disc = WiiDisc::open(image, *key, error);
    if (!disc) std::fprintf(stderr, "%s\n", error.c_str());
    CHECK(disc);
    const auto slurp = [](const fs::path& p) { std::ifstream in(p, std::ios::binary); return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), {}); };
    CHECK(disc->main_dol() == slurp(fs::path(unpacked) / "sys" / "main.dol"));
    DvdFs dvd, folder;
    CHECK(dvd.open_disc(disc) && folder.open_root(fs::path(unpacked) / "files"));
    CHECK(dvd.entry_count() == folder.entry_count());
    uint64_t bytes = 0;
    for (int e = 0; e < int(dvd.entry_count()); ++e) {
        CHECK(dvd.path_of(e) == folder.path_of(e));
        if (dvd.is_dir(e)) continue;
        const auto a = read_file(dvd, e);
        CHECK(a == read_file(folder, e));
        bytes += a.size();
    }
    std::printf("real image: %zu entries, %llu bytes identical\n", dvd.entry_count(), (unsigned long long)bytes);
}

}  // namespace

int main() {
    aes_known_answer();
    const auto disc = disc_image(kTestCommon);
    reads_image(write("wii_disc.iso", disc));
    reads_image(write("wii_disc.wbfs", wbfs_image(disc)));
    refuses_bad_images();
    key_loading();
    real_image();
    std::puts("ok");
}
