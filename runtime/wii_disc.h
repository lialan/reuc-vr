#pragma once
// The player's own Wii disc image read in place: the game's files come straight out of a .wbfs (or a plain .iso) instead of an unpacked
// game/DATA folder. Only the data partition is read. Its blocks are 0x8000 bytes on the disc: 0x400 bytes of hashes (the block's AES IV is
// at 0x3D0 of them, still encrypted) and 0x7C00 bytes of data, AES-128-CBC with the partition's title key; the title key itself is in the
// partition's ticket, encrypted with the Wii common key. The common key is not part of the port: load_wii_common_key finds the player's.
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace uc {

class Aes128;
using WiiKey = std::array<uint8_t, 16>;

class WiiDisc {
public:
    // nullptr with `error` set when the file is not a readable Wii image or its data partition cannot be opened with this key.
    static std::unique_ptr<WiiDisc> open(const std::filesystem::path& image, const WiiKey& common_key, std::string& error);
    ~WiiDisc();

    std::string game_id() const { return game_id_; }   // "RBUE08"
    // Reads the data partition's decrypted bytes (the space boot.bin, main.dol, the FST and the files live in). False past its end or on a
    // read error of the image.
    bool read(uint64_t offset, void* dst, size_t len) const;

    struct File { std::string path; uint64_t offset; uint32_t size; bool dir; };   // path relative to the disc root, '/' separated
    const std::vector<File>& files() const { return files_; }   // FST order (depth first), the root itself not included
    std::vector<uint8_t> boot_bin() const;   // DATA/sys/boot.bin (0x440 bytes)
    std::vector<uint8_t> main_dol() const;   // DATA/sys/main.dol, its length taken from the DOL header

    WiiDisc(const WiiDisc&) = delete;
    WiiDisc& operator=(const WiiDisc&) = delete;
private:
    WiiDisc() = default;
    bool raw(uint64_t disc_offset, void* dst, size_t len) const;   // the image's disc bytes (WBFS blocks mapped, unused ones read as zero)
    const std::vector<uint8_t>* cluster(uint64_t index) const;     // one decrypted 0x7C00-byte block of the partition (cached), caller holds m_

    mutable std::mutex m_;
    mutable std::FILE* file_ = nullptr;
    uint32_t wbfs_shift_ = 0;              // 0: a plain disc image; else log2 of the WBFS block size
    std::vector<uint16_t> wbfs_blocks_;    // disc block -> image block (0 = not stored: zeros)
    uint64_t data_start_ = 0, data_size_ = 0;   // the partition's encrypted data area on the disc
    std::array<uint8_t, 16> title_key_{};
    std::unique_ptr<Aes128> aes_;          // the title key's expanded rounds
    std::string game_id_;
    std::vector<File> files_;
    mutable std::unordered_map<uint64_t, std::list<std::pair<uint64_t, std::vector<uint8_t>>>::iterator> cache_index_;
    mutable std::list<std::pair<uint64_t, std::vector<uint8_t>>> cache_;   // most recently used first
};

// The common key: UC_WII_COMMON_KEY (32 hex digits) in the environment, else the first of these files that holds exactly 16 bytes, else
// the key a build was given with -DUC_WII_COMMON_KEY_FILE (none in a default build).
std::optional<WiiKey> load_wii_common_key(const std::vector<std::filesystem::path>& candidates);

// A file name ending in .wbfs or .iso (any case).
bool is_wii_image_name(const std::filesystem::path& file);

}  // namespace uc
