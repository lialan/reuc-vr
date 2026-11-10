#include "wii_disc.h"
#include "aes128.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace fs = std::filesystem;
namespace uc {
namespace {

constexpr uint64_t kBlock = 0x8000, kBlockData = 0x7C00, kBlockHashes = 0x400;
constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr uint64_t kWiiBlocksPerDisc = 143432 * 2;   // a dual-layer disc, as WBFS counts it
constexpr size_t kCacheBlocks = 64;                  // 2 MB of decrypted blocks

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }

int fseek64(std::FILE* f, uint64_t at) {
#ifdef _WIN32
    return _fseeki64(f, int64_t(at), SEEK_SET);
#else
    return fseeko(f, off_t(at), SEEK_SET);
#endif
}

std::optional<WiiKey> parse_hex_key(const char* text) {
    if (!text) return std::nullopt;
    std::string digits;
    for (const char* c = text; *c; ++c) if (!std::isspace(static_cast<unsigned char>(*c))) digits += *c;
    if (digits.size() != 32) return std::nullopt;
    WiiKey key{};
    for (size_t i = 0; i < 16; ++i) {
        char pair[3] = {digits[i * 2], digits[i * 2 + 1], 0};
        char* end = nullptr;
        const long value = std::strtol(pair, &end, 16);
        if (end != pair + 2) return std::nullopt;
        key[i] = uint8_t(value);
    }
    return key;
}

}  // namespace

bool is_wii_image_name(const fs::path& file) {
    auto ext = file.extension().string();
    for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext == ".wbfs" || ext == ".iso";
}

std::optional<WiiKey> load_wii_common_key(const std::vector<fs::path>& candidates) {
    if (auto key = parse_hex_key(std::getenv("UC_WII_COMMON_KEY"))) return key;
    for (const auto& path : candidates) {
        std::error_code error;
        if (!fs::is_regular_file(path, error) || fs::file_size(path, error) != 16) continue;
        std::ifstream in(path, std::ios::binary);
        WiiKey key{};
        if (in.read(reinterpret_cast<char*>(key.data()), 16)) return key;
    }
#ifdef UC_WII_COMMON_KEY_HEX
    return parse_hex_key(UC_WII_COMMON_KEY_HEX);
#else
    return std::nullopt;
#endif
}

WiiDisc::~WiiDisc() { if (file_) std::fclose(file_); }

bool WiiDisc::raw(uint64_t at, void* dst, size_t len) const {
    auto* out = static_cast<uint8_t*>(dst);
    while (len) {
        uint64_t file_at = at;
        size_t take = len;
        if (wbfs_shift_) {
            const uint64_t size = uint64_t(1) << wbfs_shift_, block = at >> wbfs_shift_, inside = at & (size - 1);
            take = size_t(std::min<uint64_t>(len, size - inside));
            const uint16_t stored = block < wbfs_blocks_.size() ? wbfs_blocks_[block] : 0;
            if (!stored) { std::memset(out, 0, take); out += take; at += take; len -= take; continue; }
            file_at = (uint64_t(stored) << wbfs_shift_) + inside;
        }
        if (fseek64(file_, file_at) != 0 || std::fread(out, 1, take, file_) != take) return false;
        out += take; at += take; len -= take;
    }
    return true;
}

const std::vector<uint8_t>* WiiDisc::cluster(uint64_t index) const {
    if (const auto hit = cache_index_.find(index); hit != cache_index_.end()) {
        cache_.splice(cache_.begin(), cache_, hit->second);
        return &cache_.front().second;
    }
    std::vector<uint8_t> block(kBlock);
    if (!raw(data_start_ + index * kBlock, block.data(), kBlock)) return nullptr;
    aes_->cbc_decrypt(&block[0x3D0], &block[kBlockHashes], &block[kBlockHashes], kBlockData);   // the IV is read from the encrypted hash area
    block.erase(block.begin(), block.begin() + kBlockHashes);
    if (cache_.size() >= kCacheBlocks) { cache_index_.erase(cache_.back().first); cache_.pop_back(); }
    cache_.emplace_front(index, std::move(block));
    cache_index_[index] = cache_.begin();
    return &cache_.front().second;
}

bool WiiDisc::read(uint64_t offset, void* dst, size_t len) const {
    if (offset + len > data_size_ / kBlock * kBlockData) return false;
    std::lock_guard lock(m_);
    auto* out = static_cast<uint8_t*>(dst);
    while (len) {
        const uint64_t index = offset / kBlockData, inside = offset % kBlockData;
        const size_t take = size_t(std::min<uint64_t>(len, kBlockData - inside));
        const auto* block = cluster(index);
        if (!block) return false;
        std::memcpy(out, block->data() + inside, take);
        out += take; offset += take; len -= take;
    }
    return true;
}

std::vector<uint8_t> WiiDisc::boot_bin() const {
    std::vector<uint8_t> bytes(0x440);
    if (!read(0, bytes.data(), bytes.size())) bytes.clear();
    return bytes;
}

std::vector<uint8_t> WiiDisc::main_dol() const {
    uint8_t boot[0x430];
    if (!read(0, boot, sizeof boot)) return {};
    const uint64_t dol_at = uint64_t(be32(boot + 0x420)) << 2;
    uint8_t header[0x100];
    if (!read(dol_at, header, sizeof header)) return {};
    uint64_t end = sizeof header;   // the end of the furthest of the 7 text and 11 data sections
    for (int i = 0; i < 18; ++i) {
        const uint32_t at = be32(header + i * 4), size = be32(header + 0x90 + i * 4);
        if (size) end = std::max<uint64_t>(end, uint64_t(at) + size);
    }
    if (end > (64u << 20)) return {};
    std::vector<uint8_t> dol(end);
    if (!read(dol_at, dol.data(), dol.size())) return {};
    return dol;
}

std::unique_ptr<WiiDisc> WiiDisc::open(const fs::path& image, const WiiKey& common_key, std::string& error) {
    std::unique_ptr<WiiDisc> disc(new WiiDisc());
#ifdef _WIN32
    disc->file_ = _wfopen(image.c_str(), L"rb");
#else
    disc->file_ = std::fopen(image.c_str(), "rb");
#endif
    if (!disc->file_) { error = "cannot open the image"; return nullptr; }
    uint8_t head[0x200] = {};
    if (std::fread(head, 1, sizeof head, disc->file_) != sizeof head) { error = "the file is too short for a disc image"; return nullptr; }
    if (std::memcmp(head, "WBFS", 4) == 0) {   // header: magic, sector count, log2 sector size, log2 WBFS block size; disc 0's table one sector in
        const uint32_t sector_shift = head[8], block_shift = head[9];
        if (sector_shift < 9 || sector_shift > 16 || block_shift < 15 || block_shift > 30) { error = "unsupported WBFS layout"; return nullptr; }
        disc->wbfs_shift_ = block_shift;
        const size_t count = size_t(kWiiBlocksPerDisc >> (block_shift - 15));
        std::vector<uint8_t> table(0x100 + count * 2);
        if (fseek64(disc->file_, uint64_t(1) << sector_shift) != 0 || std::fread(table.data(), 1, table.size(), disc->file_) != table.size()) {
            error = "the WBFS disc table cannot be read"; return nullptr;
        }
        disc->wbfs_blocks_.resize(count);
        for (size_t i = 0; i < count; ++i) disc->wbfs_blocks_[i] = be16(&table[0x100 + i * 2]);
    }
    uint8_t header[0x20];
    if (!disc->raw(0, header, sizeof header)) { error = "the disc header cannot be read"; return nullptr; }
    if (be32(header + 0x18) != kWiiMagic) { error = "not a Wii disc image"; return nullptr; }
    disc->game_id_.assign(reinterpret_cast<const char*>(header), 6);

    uint8_t groups[32];   // 4 partition groups: count, table offset >> 2
    if (!disc->raw(0x40000, groups, sizeof groups)) { error = "the partition table cannot be read"; return nullptr; }
    uint64_t partition = 0;
    for (int g = 0; g < 4 && !partition; ++g) {
        const uint32_t count = be32(groups + g * 8);
        const uint64_t table = uint64_t(be32(groups + g * 8 + 4)) << 2;
        for (uint32_t i = 0; i < count && i < 64; ++i) {
            uint8_t entry[8];
            if (!disc->raw(table + i * 8, entry, sizeof entry)) break;
            if (be32(entry + 4) == 0) { partition = uint64_t(be32(entry)) << 2; break; }   // type 0: the game's data
        }
    }
    if (!partition) { error = "the image has no data partition"; return nullptr; }
    uint8_t ticket[0x2C0];
    if (!disc->raw(partition, ticket, sizeof ticket)) { error = "the partition header cannot be read"; return nullptr; }
    if (ticket[0x1F1] != 0) { error = "the partition needs another common key than the Wii's (a Korean disc?)"; return nullptr; }
    uint8_t iv[16] = {};
    std::memcpy(iv, ticket + 0x1DC, 8);   // the title ID
    Aes128(common_key.data()).cbc_decrypt(iv, ticket + 0x1BF, disc->title_key_.data(), 16);
    disc->aes_ = std::make_unique<Aes128>(disc->title_key_.data());
    disc->data_start_ = partition + (uint64_t(be32(ticket + 0x2B8)) << 2);
    disc->data_size_ = uint64_t(be32(ticket + 0x2BC)) << 2;

    uint8_t boot[0x440];
    if (!disc->read(0, boot, sizeof boot)) { error = "the partition's data cannot be read"; return nullptr; }
    if (be32(boot + 0x18) != kWiiMagic || std::memcmp(boot, header, 6) != 0) { error = "the data partition does not decrypt (wrong common key?)"; return nullptr; }
    const uint64_t fst_at = uint64_t(be32(boot + 0x424)) << 2, fst_size = uint64_t(be32(boot + 0x428)) << 2;
    if (fst_size < 12 || fst_size > (32u << 20)) { error = "the file table is invalid"; return nullptr; }
    std::vector<uint8_t> fst(fst_size);
    if (!disc->read(fst_at, fst.data(), fst.size())) { error = "the file table cannot be read"; return nullptr; }
    const uint32_t count = be32(&fst[8]);
    if (count == 0 || uint64_t(count) * 12 > fst.size()) { error = "the file table is invalid"; return nullptr; }
    const size_t names = size_t(count) * 12;
    std::vector<std::pair<uint32_t, std::string>> folders;   // (index one past the folder's last entry, its path)
    for (uint32_t i = 1; i < count; ++i) {
        while (!folders.empty() && i >= folders.back().first) folders.pop_back();
        const uint8_t* e = &fst[size_t(i) * 12];
        const size_t name_at = names + (be32(e) & 0xFFFFFF);
        if (name_at >= fst.size()) { error = "the file table is invalid"; return nullptr; }
        const auto* name = reinterpret_cast<const char*>(&fst[name_at]);
        const std::string path = (folders.empty() ? std::string() : folders.back().second + "/") + std::string(name, strnlen(name, fst.size() - name_at));
        const bool dir = e[0] != 0;
        if (dir) {
            disc->files_.push_back({path, 0, 0, true});
            folders.emplace_back(be32(e + 8), path);
        } else {
            disc->files_.push_back({path, uint64_t(be32(e + 4)) << 2, be32(e + 8), false});
        }
    }
    return disc;
}

}  // namespace uc
