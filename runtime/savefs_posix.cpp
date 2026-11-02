// The save filesystem on POSIX (macOS, Linux, Android): the rules of savefs.cpp with file descriptors.
// - Every directory of a path is opened without following links (O_NOFOLLOW) before the operation, the "pins" of the Windows version; a
//   symbolic link anywhere in the save tree is refused (-102) as Windows refuses reparse points. POSIX has no share modes, so the pins check the
//   path but cannot stop another process from renaming a directory while the operation runs.
// - Files must have exactly one link; every write, replace and delete first copies the old contents to .backups / .deleted.
// - A flush is F_FULLFSYNC on macOS (fsync there does not flush the drive's cache), fsync elsewhere: FlushFileBuffers on Windows.
#include "savefs.h"
#include "save_rollback.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace fs=std::filesystem;
namespace {
int error(int value) {
    switch (value) {
    case ENOENT: case ENOTDIR: return -106;
    case EEXIST: return -105;
    case EACCES: case EPERM: case EISDIR: case EBUSY: case ELOOP: case ETXTBSY: case EROFS: return -102;
    case ENOSPC: case EDQUOT: return -108;
    case ENOTEMPTY: return -115;
    case EINVAL: case EBADF: case ENAMETOOLONG: return -101;
    default: return -103;
    }
}
int last_error() { return error(errno); }
std::shared_ptr<void> own_fd(int fd) { return std::shared_ptr<void>(new int(fd), [](void* p) { ::close(*static_cast<int*>(p)); delete static_cast<int*>(p); }); }
int flush(int fd) {
#if defined(__APPLE__)
    if (fcntl(fd, F_FULLFSYNC) == 0) return 0;
#endif
    return fsync(fd) == 0 ? 0 : last_error();
}
int check_ancestors(fs::path path) {
    for (;!path.empty();) {
        struct stat st{};
        if (lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode)) return -102;
        const auto parent=path.parent_path();
        if (parent==path) break;
        path=parent;
    }
    return 0;
}
// The names Windows reserves for devices stay invalid on every host, so a save made on one platform opens on the other.
bool reserved(std::string part) {
    for (char& ch:part) if (ch>='a' && ch<='z') ch=char(ch-'a'+'A');
    part=part.substr(0,part.find('.'));
    return part=="CON" || part=="PRN" || part=="AUX" || part=="NUL" ||
        (part.size()==4 && (part.starts_with("COM") || part.starts_with("LPT")) && part[3]>='0' && part[3]<='9');
}
bool internal(std::string name) {
    for (auto& ch:name) if (ch>='A' && ch<='Z') ch=char(ch-'A'+'a');
    return name==".backups" || name==".deleted";
}
// A regular file with one link (the Windows version also refuses reparse points and directories here).
int file_info(int fd, struct stat& st) {
    if (fstat(fd, &st) != 0) return last_error();
    return !S_ISREG(st.st_mode) || st.st_nlink != 1 ? -102 : 0;
}
// Opens a directory without following a link; -102 for a link or a file in its place.
int open_directory(const fs::path& path) {
    const int fd=::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) return fd;
    const int failure=errno;
    struct stat st{};
    if (lstat(path.c_str(), &st) == 0 && (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode))) return -102;
    return error(failure);
}
int rename_no_replace(const fs::path& source, int parent, const char* name) {
#if defined(__APPLE__)
    return renameatx_np(AT_FDCWD, source.c_str(), parent, name, RENAME_EXCL) == 0 ? 0 : last_error();
#elif defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), parent, name, 1u /* RENAME_NOREPLACE */) == 0) return 0;
    if (errno != ENOSYS && errno != EINVAL) return last_error();
    if (faccessat(parent, name, F_OK, AT_SYMLINK_NOFOLLOW) == 0) return -105;   // old kernel: check, then rename
    return renameat(AT_FDCWD, source.c_str(), parent, name) == 0 ? 0 : last_error();
#else
    if (faccessat(parent, name, F_OK, AT_SYMLINK_NOFOLLOW) == 0) return -105;
    return renameat(AT_FDCWD, source.c_str(), parent, name) == 0 ? 0 : last_error();
#endif
}
template<class Pins> int move(const fs::path& source,const fs::path& target,bool replace,Pins& pins) {
    struct stat st{};
    if (lstat(source.c_str(), &st) != 0) return last_error();
    if (S_ISLNK(st.st_mode)) return -102;
    if (!S_ISDIR(st.st_mode) && st.st_nlink != 1) return -102;
    pins.clear();
    const int parent=open_directory(target.parent_path());
    if (parent < 0) return parent;
    auto parent_owner=own_fd(parent);
    const auto name=target.filename();
    if (replace) return renameat(AT_FDCWD, source.c_str(), parent, name.c_str()) == 0 ? 0 : last_error();
    return rename_no_replace(source, parent, name.c_str());
}
uint64_t milliseconds() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
}
namespace uc {
// The root is resolved once (macOS keeps /var and /tmp behind links); below it no link is accepted.
SaveFs::SaveFs(fs::path root):root_(fs::weakly_canonical(fs::absolute(root))) {}
SaveFs::~SaveFs() {
    for (auto& [fd,file]:files_) {
        if ((file.mode&2) && flush(file.handle)) std::fprintf(stderr,"[save] flush fd %d failed: %s\n",fd,std::strerror(errno));
        if (::close(file.handle)!=0) std::fprintf(stderr,"[save] close fd %d failed: %s\n",fd,std::strerror(errno));
    }
}
int SaveFs::pin(const fs::path& directory,Pins& pins) {
    fs::path current;
    for (const auto& part:directory) {
        current/=part;
        if (!current.has_root_directory()) continue;
        if (std::any_of(pins.begin(),pins.end(),[&](const Pin& pinned) { return pinned.path==current; })) continue;
        const int fd=open_directory(current);
        if (fd<0) return fd;
        pins.push_back({current,own_fd(fd)});
    }
    return 0;
}
int SaveFs::initialize() {
    if(rollback_) return 0;
    Pins pins;
    if (auto result=pin(root_.parent_path(),pins)) return result;
    if (mkdir(root_.c_str(),0777)!=0 && errno!=EEXIST) return last_error();
    return pin(root_,pins);
}
int SaveFs::resolve(const std::string& path,fs::path& host,Pins& pins) const {
    if (path.empty() || path[0]!='/' || path.size()>63 || path.starts_with("//")) return -101;
    host=root_;
    for (size_t at=1;at<path.size();) {
        const auto end=path.find('/',at);
        auto part=path.substr(at,end==std::string::npos ? end : end-at);
        if (part.empty() || part=="." || part==".." || part.back()=='.' || part.back()==' ' || reserved(part)) return -101;
        for (unsigned char ch:part) if (ch<32 || ch>=127 || std::string_view("\\:*?\"<>|").find(char(ch))!=std::string_view::npos) return -101;
        if (host==root_ && internal(part)) return -102;
        host/=part;
        if (end==std::string::npos) break;
        at=end+1;
    }
    if (auto result=pin(host==root_ ? host : host.parent_path(),pins)) return result;
    return check_ancestors(host);
}
fs::path SaveFs::revision(const fs::path::value_type* directory,Pins& pins) {
    const auto parent=root_/directory;
    int result=pin(root_,pins);
    if (!result && mkdir(parent.c_str(),0777)!=0 && errno!=EEXIST) result=last_error();
    if (!result) result=pin(parent,pins);
    if (result) throw fs::filesystem_error("unsafe save backup directory",parent,std::error_code(EACCES,std::generic_category()));
    return parent/(std::to_string(getpid())+"-"+std::to_string(milliseconds())+"-"+std::to_string(++revision_));
}
int SaveFs::backup(SaveHandle source) {
    struct stat st{};
    if (fstat(source,&st)!=0) return last_error();
    if (!st.st_size) return 0;
    int result=0;
    try {
        Pins pins;
        const int target=::open(revision(".backups",pins).c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
        if (target<0) return last_error();
        auto owner=own_fd(target);
        std::array<uint8_t,65536> buffer{};
        // pread: the source's file position (the guest's) stays where it is.
        for (off_t offset=0;!result;) {
            const ssize_t count=pread(source,buffer.data(),buffer.size(),offset);
            if (count<0) { result=last_error(); break; }
            if (!count) break;
            const ssize_t written=::write(target,buffer.data(),size_t(count));
            if (written<0) result=last_error();
            else if (written!=count) result=-103;
            offset+=count;
        }
        if (!result) result=flush(target);
    }
    catch (const fs::filesystem_error& failure) { return error(failure.code().value()); }
    return result;
}
int SaveFs::backup(const fs::path& path) {
    const int source=::open(path.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if (source<0) return errno==ELOOP ? -102 : last_error();
    auto owner=own_fd(source);
    struct stat st{};
    if (auto result=file_info(source,st)) return result;
    return backup(source);
}
int SaveFs::create_dir(const std::string& path) {
    if(rollback_) return writable().create(path,true);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    return mkdir(host.c_str(),0777)==0 ? 0 : last_error();
}
int SaveFs::create_file(const std::string& path) {
    if(rollback_) return writable().create(path,false);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    const int file=::open(host.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
    if (file<0) return last_error();
    return ::close(file)==0 ? 0 : last_error();
}
int SaveFs::open(const std::string& path,uint32_t mode) {
    if(rollback_) return writable().open(path,mode);
    if (mode<1 || mode>3 || next_fd_==INT_MAX) return -101;
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    const int file=::open(host.c_str(),(mode&2 ? O_RDWR : O_RDONLY)|O_NOFOLLOW|O_CLOEXEC);
    if (file<0) return errno==ELOOP ? -102 : last_error();
    struct stat st{};
    int result=file_info(file,st);
    if (!result && (mode&2)) result=backup(file);
    if (result) { ::close(file); return result; }
    const auto fd=next_fd_++;
    files_.emplace(fd,File{file,mode,std::move(pins)}); return fd;
}
int SaveFs::close(int fd) {
    if(rollback_) return writable().close(fd);
    auto found=files_.find(fd); if (found==files_.end()) return -101;
    if ((found->second.mode&2)) if (auto result=flush(found->second.handle)) return result;
    if (::close(found->second.handle)!=0) return last_error();
    files_.erase(found); return 0;
}
int SaveFs::read(int fd,std::span<uint8_t> bytes) {
    if(rollback_) return writable().read(fd,bytes);
    auto found=files_.find(fd); if (found==files_.end() || bytes.size()>INT_MAX) return -101;
    if (!(found->second.mode&1)) return -102;
    const ssize_t count=::read(found->second.handle,bytes.data(),bytes.size());
    return count>=0 ? int(count) : last_error();
}
int SaveFs::write(int fd,std::span<const uint8_t> bytes) {
    if(rollback_) return writable().write(fd,bytes);
    auto found=files_.find(fd); if (found==files_.end() || bytes.size()>INT_MAX) return -101;
    auto& file=found->second;
    if (!(file.mode&2)) return -102;
    const ssize_t count=::write(file.handle,bytes.data(),bytes.size());
    return count>=0 ? int(count) : last_error();
}
int SaveFs::seek(int fd,int32_t offset,int32_t whence) {
    if(rollback_) return writable().seek(fd,offset,whence);
    auto found=files_.find(fd); if (found==files_.end() || whence<0 || whence>2) return -101;
    int64_t base=0;
    if (whence==1) { const off_t now=lseek(found->second.handle,0,SEEK_CUR); if (now<0) return last_error(); base=now; }
    if (whence==2) { struct stat st{}; if (fstat(found->second.handle,&st)!=0) return last_error(); base=st.st_size; }
    const int64_t position=base+offset;
    if (position<0 || position>INT_MAX) return -101;
    if (lseek(found->second.handle,off_t(position),SEEK_SET)<0) return last_error();
    return int(position);
}
SaveFs::Stats SaveFs::stats(int fd) {
    if(rollback_) return rollback_->stats(fd);
    auto found=files_.find(fd); if (found==files_.end()) return {-101};
    struct stat st{};
    const off_t position=lseek(found->second.handle,0,SEEK_CUR);
    if (fstat(found->second.handle,&st)!=0 || position<0) return {last_error()};
    if (uint64_t(st.st_size)>UINT32_MAX || uint64_t(position)>UINT32_MAX) return {-101};
    return {0,uint32_t(st.st_size),uint32_t(position)};
}
int SaveFs::attributes(const std::string& path,bool& directory) {
    if(rollback_) return rollback_->attributes(path,directory);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    struct stat st{};
    if (lstat(host.c_str(),&st)!=0) return last_error();
    if (S_ISLNK(st.st_mode)) return -102;
    directory=S_ISDIR(st.st_mode); return 0;
}
SaveFs::Directory SaveFs::read_dir(const std::string& path) {
    if(rollback_) return rollback_->read_dir(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return {result};
    bool directory; if (auto result=attributes(path,directory)) return {result};
    if (!directory) return {-101};
    if (auto result=pin(host,pins)) return {result};
    Directory result{0}; std::error_code ec;
    for (fs::directory_iterator it(host,ec),end;!ec && it!=end;it.increment(ec)) {
        if (host==root_ && internal(it->path().filename().string())) continue;
        if (check_ancestors(it->path())) return {-102};
        result.names.push_back(it->path().filename().string());
    }
    if (ec) return {error(ec.value())};
    std::sort(result.names.begin(),result.names.end()); return result;
}
SaveFs::Usage SaveFs::usage(const std::string& path) {
    if(rollback_) return rollback_->usage(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return {result};
    bool directory; if (auto result=attributes(path,directory)) return {result};
    Usage result{0}; std::error_code ec;
    if (!directory) {
        const int fd=open(path,1); if (fd<0) return {fd};
        const auto info=stats(fd); const int closed=close(fd);
        if (info.result || closed) return {info.result ? info.result : closed};
        return {0,uint32_t((uint64_t(info.length)+16383)/16384),1};
    }
    if (auto result=pin(host,pins)) return {result};
    uint64_t blocks=0,entries=0;
    for (fs::recursive_directory_iterator it(host,ec),end;!ec && it!=end;it.increment(ec)) {
        if (host==root_ && internal(it->path().filename().string()) && it->path().parent_path()==root_) { it.disable_recursion_pending(); continue; }
        if (check_ancestors(it->path())) return {-102};
        ++entries;
        if (it->is_directory(ec)) { if (auto failure=pin(it->path(),pins)) return {failure}; }
        else {
            const int file=::open(it->path().c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
            if (file<0) return {errno==ELOOP ? -102 : last_error()};
            auto owner=own_fd(file);
            struct stat st{};
            if (auto failure=file_info(file,st)) return {failure};
            blocks+=(uint64_t(st.st_size)+16383)/16384;
        }
    }
    if (ec) return {error(ec.value())};
    if (blocks>UINT32_MAX || entries>UINT32_MAX) return {-101};
    return {0,uint32_t(blocks),uint32_t(entries)};
}
static bool exists_no_follow(const fs::path& path) { struct stat st{}; return lstat(path.c_str(),&st)==0; }
int SaveFs::rename(const std::string& from,const std::string& to) {
    if(rollback_) return writable().rename(from,to);
    Pins pins; fs::path source,target; if (auto result=resolve(to,target,pins)) return result; if (auto result=resolve(from,source,pins)) return result;
    if (source==root_ || target==root_) return -102;
    if (source==target) return exists_no_follow(source) ? 0 : last_error();
    if (exists_no_follow(target)) if (auto result=backup(target)) return result;
    return move(source,target,true,pins);
}
int SaveFs::remove(const std::string& path) {
    if(rollback_) return writable().remove(path);
    Pins pins; fs::path host; if (auto result=resolve(path,host,pins)) return result;
    if (host==root_) return -102;
    struct stat st{}; if (lstat(host.c_str(),&st)!=0) return last_error();
    if (S_ISLNK(st.st_mode)) return -102;
    if (S_ISDIR(st.st_mode)) { auto contents=read_dir(path); if (contents.result) return contents.result; if (!contents.names.empty()) return -115; }
    try { const auto target=revision(".deleted",pins); return move(host,target,false,pins); }
    catch (const fs::filesystem_error& failure) { return error(failure.code().value()); }
}
void SaveFs::commit(const Snapshot& saved) {
    if(!rollback_ || saved.owner!=this || saved.revision<committed_->revision)
        throw std::logic_error("invalid confirmed save checkpoint");
    if(saved.revision==committed_->revision) return;
    SaveFs disk(root_);
    const auto require=[](int result,const char* operation) {
        if(result) throw std::runtime_error(std::string("confirmed save ")+operation+" failed: "+std::to_string(result));
    };
    // Children retire first; backups preserve each replaced or deleted version.
    for(auto it=committed_->nodes.rbegin();it!=committed_->nodes.rend();++it) {
        if(it->first=="/") continue;
        const auto next=saved.nodes.find(it->first);
        if(next==saved.nodes.end() || bool(next->second.bytes)!=bool(it->second.bytes)) {
            const int result=disk.remove(it->first);
            if(result!=-106) require(result,"delete");
        }
    }
    for(const auto& [path,node]:saved.nodes) {
        if(path=="/") continue;
        const auto old=committed_->nodes.find(path);
        if(!node.bytes) {
            const int result=disk.create_dir(path);
            if(result!=-105) require(result,"mkdir");
            continue;
        }
        if(old!=committed_->nodes.end() && old->second.bytes &&
           (old->second.bytes==node.bytes || *old->second.bytes==*node.bytes)) continue;
        Pins pins; fs::path target;
        require(disk.resolve(path,target,pins),"resolve");
        if(exists_no_follow(target)) require(disk.backup(target),"backup");
        // The new contents go to a staged file first and replace the target in one rename: a crash leaves the old or the new save, never half.
        const auto temporary=disk.revision(".backups",pins);
        const int handle=::open(temporary.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0666);
        if(handle<0) require(last_error(),"create staged file");
        int result=0;
        const ssize_t written=::write(handle,node.bytes->data(),node.bytes->size());
        if(written<0) result=last_error();
        else if(size_t(written)!=node.bytes->size()) result=-103;
        if(!result) result=flush(handle);
        if(::close(handle)!=0 && !result) result=last_error();
        require(result,"write staged file");
        require(move(temporary,target,true,pins),"publish");
    }
    committed_=std::make_shared<Snapshot>(saved);
}
}
