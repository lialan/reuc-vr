// The session log on POSIX (macOS, Linux, Android): stdout and stderr go through a pipe each; a worker thread writes every line with a time
// prefix to <root>/<app>-<UTC stamp>-<pid>-<n>/runtime.log and mirrors it to the original stream (the terminal or the test harness).
#include "diagnostic_log.h"
#include "exe_path.h"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/utsname.h>
#include <thread>
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace uc {
namespace {
std::filesystem::path directory; // Published before workers start; unchanged until they join.
std::atomic<unsigned> sequence{0};
bool write_all(int fd, const char* data, size_t size) {
    while (size) {
        const ssize_t written = ::write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        data += written; size -= size_t(written);
    }
    return true;
}
uint64_t now_ms() {
    return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::string cpu_brand() {
#if defined(__APPLE__)
    char brand[128]{}; size_t size = sizeof(brand);
    if (sysctlbyname("machdep.cpu.brand_string", brand, &size, nullptr, 0) == 0) return brand;
#endif
    return "unknown";
}
unsigned long long total_memory_mb() {
#if defined(__APPLE__)
    uint64_t bytes = 0; size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) return bytes / (1024 * 1024);
#else
    const long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGESIZE);
    if (pages > 0 && page > 0) return (unsigned long long)pages * (unsigned long long)page / (1024 * 1024);
#endif
    return 0;
}
}  // namespace

struct DiagnosticLog::Impl {
    int file = -1;
    std::mutex mutex;
    uint64_t started = now_ms();
    bool file_failed = false;
    struct Pipe {
        Impl& owner;
        FILE* stream;
        const char* name;
        int read = -1, mirror = -1, saved = -1;
        bool redirected = false;
        std::thread worker;
        Pipe(Impl& owner, FILE* stream, const char* name) : owner(owner), stream(stream), name(name) {}
        void start() {
            std::fflush(stream);
            const int fd = fileno(stream);
            if (fd < 0) throw std::runtime_error("cannot initialize diagnostic stream");
            saved = dup(fd);
            mirror = dup(fd);
            if (saved < 0 || mirror < 0) throw std::runtime_error("cannot preserve diagnostic stream");
            int ends[2];
            if (pipe(ends) != 0) throw std::runtime_error("cannot create diagnostic pipe");
            fcntl(ends[0], F_SETFD, FD_CLOEXEC);
            read = ends[0];
            const int result = dup2(ends[1], fd);
            ::close(ends[1]);
            if (result < 0) throw std::runtime_error("cannot redirect diagnostic stream");
            redirected = true;
            std::setvbuf(stream, nullptr, _IONBF, 0);
            worker = std::thread([this] {
                char bytes[8192];
                std::string pending;
                for (;;) {
                    const ssize_t count = ::read(read, bytes, sizeof(bytes));
                    if (count < 0 && errno == EINTR) continue;
                    if (count <= 0) break;
                    pending.append(bytes, size_t(count));
                    size_t first = 0;
                    for (size_t end = pending.find('\n'); end != std::string::npos; end = pending.find('\n', first)) {
                        owner.append(name, pending.data() + first, end - first + 1, mirror); first = end + 1;
                    }
                    pending.erase(0, first);
                    if (pending.size() >= 65536) { owner.append(name, pending.data(), pending.size(), mirror); pending.clear(); }
                    if (mirror >= 0 && !write_all(mirror, bytes, size_t(count))) { ::close(mirror); mirror = -1; }   // a closed terminal must not stop disk logging
                }
                if (!pending.empty()) owner.append(name, pending.data(), pending.size(), mirror);
            });
        }
        void stop() noexcept {
            if (redirected) {
                std::fflush(stream);
                if (dup2(saved, fileno(stream)) < 0) ::close(fileno(stream));   // releases the pipe writer either way: the worker sees the end
                redirected = false;
            }
            if (saved >= 0) { ::close(saved); saved = -1; }
            if (worker.joinable()) worker.join();
            if (read >= 0) { ::close(read); read = -1; }
            if (mirror >= 0) { ::close(mirror); mirror = -1; }
        }
        ~Pipe() { stop(); }
    } out{*this, stdout, "stdout"}, err{*this, stderr, "stderr"};
    void append(const char* source, const char* text, size_t size, int mirror) {
        std::lock_guard lock(mutex);
        if (file_failed) return;
        char prefix[80]; const int count = std::snprintf(prefix, sizeof(prefix), "[+%llu ms %s] ", (unsigned long long)(now_ms() - started), source);
        if (!write_all(file, prefix, size_t(count)) || !write_all(file, text, size) || (text[size - 1] != '\n' && !write_all(file, "\n", 1))) {
            file_failed = true;
            constexpr char warning[] = "diagnostic log write failed; output remains available on inherited handles\n";
            if (mirror >= 0) write_all(mirror, warning, sizeof(warning) - 1);
        }
    }
    ~Impl() {
        err.stop(); out.stop();
        if (file >= 0) { fsync(file); ::close(file); }
    }
};

DiagnosticLog::DiagnosticLog(std::string_view app, const std::filesystem::path& root) noexcept {
    try {
        if (!directory.empty()) throw std::runtime_error("diagnostics already active");
        const auto wall = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(wall);
        const auto millis = unsigned(std::chrono::duration_cast<std::chrono::milliseconds>(wall.time_since_epoch()).count() % 1000);
        std::tm utc{}; gmtime_r(&seconds, &utc);
        char stamp[96];
        std::snprintf(stamp, sizeof(stamp), "-%04d%02d%02d-%02d%02d%02d-%03u-%lu-%u", utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min,
                      utc.tm_sec, millis, (unsigned long)getpid(), sequence.fetch_add(1));
        std::string label;
        for (char c : app) label += ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ? c : '_';
        const auto session = root / (label + stamp);
        std::filesystem::create_directories(session);
        auto impl = std::make_unique<Impl>();
        impl->file = ::open((session / "runtime.log").c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
        if (impl->file < 0) throw std::runtime_error("cannot create diagnostic log");
        impl->out.start(); impl->err.start();
        directory = session; impl_ = std::move(impl);
        utsname system{}; uname(&system);
        std::fprintf(stderr, "[session] app=%s pid=%lu UTC=%04d-%02d-%02dT%02d:%02d:%02dZ executable=%s\n", label.c_str(), (unsigned long)getpid(), utc.tm_year + 1900,
                     utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec, executable_path().string().c_str());
        std::fprintf(stderr, "[system] %s %s %s CPU=%s logical=%ld RAM_total_MB=%llu build=%s %s\n", system.sysname, system.release, system.machine,
                     cpu_brand().c_str(), sysconf(_SC_NPROCESSORS_ONLN), total_memory_mb(), __DATE__, __TIME__);
    } catch (const std::exception& error) { std::fprintf(stderr, "[diagnostics] unavailable: %s\n", error.what()); }
}
DiagnosticLog::~DiagnosticLog() {
    if (impl_) { std::fprintf(stderr, "[session] log closed normally\n"); impl_.reset(); directory.clear(); }
}
bool DiagnosticLog::active() const noexcept { return bool(impl_); }
const std::filesystem::path& diagnostic_directory() noexcept { return directory; }
}  // namespace uc
