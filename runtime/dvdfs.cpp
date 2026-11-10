#include "dvdfs.h"
#include "wii_disc.h"
#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;
namespace uc {

static std::string lower(std::string s) { for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c))); return s; }
// path::string() throws for names outside the ANSI code page (the game tree contains such names); keep UTF-8 instead.
static std::string utf8(const fs::path& p) { auto s = p.u8string(); return std::string(s.begin(), s.end()); }

// A folder named <archive>.arc! holds members of that archive (mods put them next to the archive; the port builds the changed archive from them).
static bool is_member_folder(const std::string& name) { return name.size() > 5 && lower(name.substr(name.size() - 5)) == ".arc!"; }

void DvdFs::walk(const fs::path& dir, const std::string& rel) {
    std::vector<fs::directory_entry> kids;
    for (const auto& k : fs::directory_iterator(dir)) kids.push_back(k);
    std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) { return lower(utf8(a.path().filename())) < lower(utf8(b.path().filename())); });
    for (const auto& k : kids) {
        std::string name = utf8(k.path().filename());
        std::string r = rel.empty() ? name : rel + "/" + name;
        bool is_dir = k.is_directory();
        e_.push_back({r, k.path(), is_dir, is_dir ? 0u : uint32_t(k.file_size())});
        index_[lower(r)] = int(e_.size()) - 1;
        if (is_dir) walk(k.path(), r);   // depth first, like an FST
    }
}

bool DvdFs::open_root(const fs::path& root) {
    std::unique_lock lock(m_);
    e_.clear(); index_.clear(); undo_.clear(); hidden_.clear(); root_.clear(); disc_.reset(); disc_files_.clear();
    if (!fs::is_directory(root)) return false;
    root_ = root;
    e_.push_back({"", root, true, 0});
    index_[""] = 0;
    walk(root, "");
    return true;
}

bool DvdFs::open_disc(std::shared_ptr<const WiiDisc> disc) {
    std::unique_lock lock(m_);
    e_.clear(); index_.clear(); undo_.clear(); hidden_.clear(); root_.clear(); disc_.reset(); disc_files_.clear();
    if (!disc) return false;
    // The FST's own order is not the folder walk's: list each folder's children sorted as walk() sorts them, so an entry number names the
    // same file whichever way the game is supplied.
    std::map<std::string, std::vector<const WiiDisc::File*>> children;   // lower-case folder path -> its entries
    for (const auto& file : disc->files()) {
        const size_t slash = file.path.rfind('/');
        children[lower(slash == std::string::npos ? std::string() : file.path.substr(0, slash))].push_back(&file);
        if (!file.dir) disc_files_[lower(file.path)] = {file.offset, file.size};
    }
    for (auto& [folder, list] : children)
        std::sort(list.begin(), list.end(), [](const auto* a, const auto* b) {
            const auto name = [](const std::string& p) { const size_t slash = p.rfind('/'); return lower(slash == std::string::npos ? p : p.substr(slash + 1)); };
            return name(a->path) < name(b->path);
        });
    disc_ = std::move(disc);
    e_.push_back({"", {}, true, 0});
    index_[""] = 0;
    std::vector<std::pair<const std::vector<const WiiDisc::File*>*, size_t>> stack;   // depth first, like walk()
    if (const auto root = children.find(""); root != children.end()) stack.push_back({&root->second, 0});
    while (!stack.empty()) {
        auto& [list, next] = stack.back();
        if (next == list->size()) { stack.pop_back(); continue; }
        const WiiDisc::File& file = *(*list)[next++];
        e_.push_back({file.path, {}, file.dir, file.dir ? 0u : file.size, false, !file.dir, file.offset});
        index_[lower(file.path)] = int(e_.size()) - 1;
        if (file.dir) if (const auto kids = children.find(lower(file.path)); kids != children.end()) stack.push_back({&kids->second, 0});
    }
    return true;
}

std::optional<std::vector<uint8_t>> DvdFs::disc_file(const std::string& rel, uint64_t max_bytes) const {
    std::shared_lock lock(m_);
    if (disc_) {
        const auto at = disc_files_.find(lower(rel));
        if (at == disc_files_.end() || at->second.second == 0 || at->second.second > max_bytes) return std::nullopt;
        std::vector<uint8_t> bytes(at->second.second);
        if (!disc_->read(at->second.first, bytes.data(), bytes.size())) return std::nullopt;
        return bytes;
    }
    if (root_.empty()) return std::nullopt;
    const fs::path file = root_ / fs::path(std::u8string(rel.begin(), rel.end()));
    std::error_code error;
    const uintmax_t size = fs::is_regular_file(file, error) ? fs::file_size(file, error) : 0;
    if (size == 0 || size > max_bytes) return std::nullopt;
    std::vector<uint8_t> bytes(size);
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(size))) return std::nullopt;
    return bytes;
}

size_t DvdFs::add_entry(Entry entry) {
    const std::string key = lower(entry.rel);
    size_t slot;
    const auto hidden = hidden_.find(key);
    if (hidden != hidden_.end() && e_[hidden->second].dir == entry.dir) {   // the same path (and kind) was added before and taken back: same number again
        slot = hidden->second;
        hidden_.erase(hidden);
        e_[slot] = std::move(entry);
    } else {   // (a path that comes back as another kind gets a new number: a held number never changes meaning)
        e_.push_back(std::move(entry));
        slot = e_.size() - 1;
    }
    index_[key] = int(slot);
    undo_.push_back({slot, true, {}});
    return slot;
}

void DvdFs::clear_locked() {
    for (size_t i = undo_.size(); i-- > 0;) {
        Undo& undo = undo_[i];
        if (undo.added) {
            Entry& entry = e_[undo.slot];
            const std::string key = lower(entry.rel);
            index_.erase(key);
            hidden_[key] = undo.slot;
            entry.hidden = true; entry.size = 0; entry.full.clear();   // the slot stays: a number the game holds must never point at another file
        } else {
            e_[undo.slot] = std::move(undo.original);
        }
    }
    undo_.clear();
}

void DvdFs::clear_overlays() {
    std::unique_lock lock(m_);
    clear_locked();
}

// The regular files below files_root (no symlinks or junctions): relative path (UTF-8, '/'), full path, size. Takes no lock: the scan can be slow.
std::vector<DvdFs::Found> DvdFs::scan(const fs::path& files_root) {
    std::vector<Found> files;
    std::error_code error;
    for (fs::recursive_directory_iterator it(files_root, error), last; !error && it != last; it.increment(error)) {
        std::error_code entry_error;
        const auto type = it->symlink_status(entry_error).type();
#ifdef _WIN32
        const bool link = type == fs::file_type::symlink || type == fs::file_type::junction;   // junction: Microsoft STL only
#else
        const bool link = type == fs::file_type::symlink;
#endif
        if (link) { it.disable_recursion_pending(); continue; }
        if (type == fs::file_type::directory && is_member_folder(utf8(it->path().filename()))) { it.disable_recursion_pending(); continue; }
        if (type != fs::file_type::regular) continue;
        const auto measured = it->file_size(entry_error);
        if (entry_error || measured > UINT32_MAX) continue;
        const auto rel = it->path().lexically_relative(files_root).generic_u8string();
        files.push_back({std::string(rel.begin(), rel.end()), it->path(), uint32_t(measured)});
    }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return lower(a.rel) < lower(b.rel); });
    return files;
}

size_t DvdFs::apply_locked(const fs::path& files_root, const std::vector<Found>& files) {
    size_t count = 0;
    for (const auto& [rel, full, size] : files) {
        if (const auto at = index_.find(lower(rel)); at != index_.end()) {
            Entry& entry = e_[size_t(at->second)];
            if (entry.dir) continue;
            undo_.push_back({size_t(at->second), false, entry});
            entry.full = full; entry.size = size; entry.on_disc = false; ++count;
            continue;
        }
        for (size_t slash = rel.find('/'); slash != std::string::npos; slash = rel.find('/', slash + 1)) {   // missing parent folders
            const std::string folder = rel.substr(0, slash);
            if (index_.count(lower(folder))) continue;
            add_entry({folder, files_root / fs::path(std::u8string(folder.begin(), folder.end())), true, 0});
        }
        add_entry({rel, full, false, size});
        ++count;
    }
    return count;
}

size_t DvdFs::overlay(const fs::path& files_root) {
    const auto files = scan(files_root);
    std::unique_lock lock(m_);
    if (e_.empty()) return 0;
    return apply_locked(files_root, files);
}

size_t DvdFs::set_overlays(const std::vector<fs::path>& files_roots) {
    std::vector<std::vector<Found>> scanned;
    for (const auto& root : files_roots) scanned.push_back(scan(root));
    std::unique_lock lock(m_);   // one swap: a reader sees the old overlays or the new ones, never the disc's own files in between
    clear_locked();
    if (e_.empty()) return 0;
    size_t count = 0;
    for (size_t i = 0; i < files_roots.size(); ++i) count += apply_locked(files_roots[i], scanned[i]);
    return count;
}

int DvdFs::entry_of(std::string path) const {
    std::shared_lock lock(m_);
    for (char& c : path) if (c == '\\') c = '/';
    std::string norm;
    size_t i = 0;
    while (i < path.size()) {
        size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string part = path.substr(i, j - i);
        if (part == "..") return -1;                       // never leave the root
        if (!part.empty() && part != ".") norm += (norm.empty() ? "" : "/") + part;
        i = j + 1;
    }
    auto it = index_.find(lower(norm));
    return it == index_.end() ? -1 : it->second;
}

std::optional<DvdFs::Snapshot> DvdFs::snapshot(int entry) const {
    std::shared_lock lock(m_);
    if (!valid(entry) || e_[size_t(entry)].dir) return std::nullopt;
    const Entry& e = e_[size_t(entry)];
    return Snapshot{entry, e.full, e.size, e.on_disc, e.disc_at};
}

int64_t DvdFs::read_bytes(const fs::path& full, bool on_disc, uint64_t disc_at, uint32_t size, void* dst, uint32_t len, uint32_t offset) const {
    if (len == 0 || offset >= size) return 0;
    const uint32_t n = std::min<uint32_t>(len, size - offset);
    if (on_disc) return disc_ && disc_->read(disc_at + offset, dst, n) ? int64_t(n) : -1;
    std::ifstream f(full, std::ios::binary);
    if (!f) return -1;
    f.seekg(offset);
    f.read(static_cast<char*>(dst), n);
    return int64_t(f.gcount());
}

int64_t DvdFs::read(const Snapshot& file, void* dst, uint32_t len, uint32_t offset) const {
    return read_bytes(file.full, file.on_disc, file.disc_at, file.size, dst, len, offset);
}

int64_t DvdFs::read(int entry, void* dst, uint32_t len, uint32_t offset) const {
    std::shared_lock lock(m_);   // held across the file read: a swap of the overlays waits for the read in flight, so a read sees one whole version
    if (!valid(entry) || e_[size_t(entry)].dir) return -1;
    const Entry& e = e_[size_t(entry)];
    return read_bytes(e.full, e.on_disc, e.disc_at, e.size, dst, len, offset);
}

}  // namespace uc
