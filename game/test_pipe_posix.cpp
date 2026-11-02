// The test harness transport on POSIX: the two named pipes of test_pipe.cpp become two UNIX domain sockets, $TMPDIR/uc-test-<name>-cmd.sock
// (commands in) and -evt.sock (events out), each taking one client, carrying the same JSON lines (scripts/uctest/transport.py).
// accept and read wait in poll() with a short timeout, so the destructor can stop both threads (closing a socket does not wake them on macOS).
#include "test_pipe.h"
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace uc {
namespace {
constexpr size_t kMaxLine = 4096, kMaxOut = 4096;
std::string socket_path(const std::string& name, const char* suffix) {
#ifdef __ANDROID__
    // The abstract namespace (a leading NUL): reachable from the host with adb forward localabstract:uc-test-<name>-cmd (scripts/uctest --quest);
    // an app's own folder is not reachable by adbd.
    return std::string(1, '\0') + "uc-test-" + name + suffix;
#endif
    const char* dir = std::getenv("TMPDIR");
    std::string path = dir && *dir ? dir : "/tmp";
    if (path.back() != '/') path += '/';
    return path + "uc-test-" + name + suffix + ".sock";
}
int listen_at(const std::string& path) {
    sockaddr_un address{};
    if (path.size() >= sizeof(address.sun_path)) throw std::runtime_error("test socket path too long: " + path);
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("cannot create test socket");
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    const bool abstract = !path.empty() && path[0] == '\0';
    if (!abstract) { address.sun_path[path.size()] = 0; unlink(path.c_str()); }   // a socket file left by a crashed run
    const socklen_t length = abstract ? socklen_t(offsetof(sockaddr_un, sun_path) + path.size()) : socklen_t(sizeof(address));
    if (bind(fd, reinterpret_cast<const sockaddr*>(&address), length) != 0 || listen(fd, 1) != 0) {
        close(fd);
        throw std::runtime_error("test socket already in use: " + path);
    }
    return fd;
}
// Waits for fd to become readable (or the peer to go), giving up when stop is set; false then.
bool wait_readable(int fd, const std::atomic<bool>& stop) {
    while (!stop) {
        pollfd p{fd, POLLIN, 0};
        const int r = poll(&p, 1, 100);
        if (r > 0) return true;
        if (r < 0 && errno != EINTR) return false;
    }
    return false;
}
// cmd/evt are set by the reader/writer threads and read by the destructor, hence atomic.
struct Sockets { int cmd_listen = -1, evt_listen = -1; std::atomic<int> cmd{-1}, evt{-1}; std::string cmd_path, evt_path; };
Sockets& sockets(void* p) { return *static_cast<Sockets*>(p); }
}  // namespace

TestPipes::TestPipes(const std::string& name) {
    auto* s = new Sockets;
    s->cmd_path = socket_path(name, "-cmd");
    s->evt_path = socket_path(name, "-evt");
    try {
        s->cmd_listen = listen_at(s->cmd_path);
        s->evt_listen = listen_at(s->evt_path);
    } catch (...) {
        if (s->cmd_listen >= 0) { close(s->cmd_listen); unlink(s->cmd_path.c_str()); }
        delete s;
        throw;
    }
    cmd_ = s;   // evt_ unused: one block holds both ends
    reader_ = std::thread([this] { read_loop(); });
    writer_ = std::thread([this] { write_loop(); });
}
TestPipes::~TestPipes() {
    {   // let queued replies (a final quit reply) reach a connected client
        const auto limit = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (evt_connected_ && !disconnected_ && std::chrono::steady_clock::now() < limit) {
            { std::lock_guard g(out_mutex_); if (out_.empty()) break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    auto& s = sockets(cmd_);
    { std::lock_guard g(out_mutex_); stop_ = true; }   // under the lock: the writer cannot miss the wake-up between its check and its wait
    out_cv_.notify_all();
    // A writer blocked in send() on a full buffer (the harness stopped reading) only returns once the socket is shut down.
    for (int fd : {s.cmd.load(), s.evt.load()}) if (fd >= 0) shutdown(fd, SHUT_RDWR);
    reader_.join(); writer_.join();
    for (int fd : {s.cmd.load(), s.evt.load(), s.cmd_listen, s.evt_listen}) if (fd >= 0) close(fd);
    unlink(s.cmd_path.c_str()); unlink(s.evt_path.c_str());
    delete &s;
}
void TestPipes::read_loop() {
    auto& s = sockets(cmd_);
    std::string partial; bool discarding = false;
    if (wait_readable(s.cmd_listen, stop_) && (s.cmd = accept(s.cmd_listen, nullptr, nullptr)) >= 0) {
        cmd_connected_ = true;
        char buffer[4096];
        while (wait_readable(s.cmd, stop_)) {
            const ssize_t n = read(s.cmd, buffer, sizeof buffer);
            if (n <= 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                const char c = buffer[i];
                if (c == '\n') {
                    if (!partial.empty() && partial.back() == '\r') partial.pop_back();
                    std::lock_guard g(in_mutex_);
                    in_lines_.push_back(discarding ? "\x01too-long" : std::move(partial));
                    partial.clear(); discarding = false;
                } else if (!discarding) {
                    partial += c;
                    if (partial.size() > kMaxLine) { discarding = true; partial.clear(); }
                }
            }
        }
        if (!stop_) disconnected_ = true;
    }
    reader_done_ = true;
}
void TestPipes::write_loop() {
    auto& s = sockets(cmd_);
    if (wait_readable(s.evt_listen, stop_) && (s.evt = accept(s.evt_listen, nullptr, nullptr)) >= 0) {
#ifdef SO_NOSIGPIPE
        const int one = 1; setsockopt(s.evt, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));   // a harness that went away must not end the game
#endif
        evt_connected_ = true;
        while (!stop_) {
            std::string line;
            {
                std::unique_lock lk(out_mutex_);
                out_cv_.wait(lk, [this] { return stop_ || !out_.empty(); });
                if (stop_) break;
                line = std::move(out_.front().first); out_.pop_front();
            }
            line += '\n';
            size_t sent = 0;
            while (sent < line.size()) {
#ifdef MSG_NOSIGNAL
                const ssize_t n = ::send(s.evt, line.data() + sent, line.size() - sent, MSG_NOSIGNAL);
#else
                const ssize_t n = ::send(s.evt, line.data() + sent, line.size() - sent, 0);
#endif
                if (n <= 0) { if (errno == EINTR) continue; break; }
                sent += size_t(n);
            }
            if (sent != line.size()) { if (!stop_) disconnected_ = true; break; }
        }
    }
    writer_done_ = true;
}
std::vector<std::string> TestPipes::take_lines() { std::lock_guard g(in_mutex_); return std::exchange(in_lines_, {}); }
void TestPipes::send(std::string line, bool droppable) {
    {
        std::lock_guard g(out_mutex_);
        if (out_.size() >= kMaxOut) {
            for (auto it = out_.begin(); it != out_.end(); ++it) if (it->second) { out_.erase(it); ++dropped_; break; }
        }
        out_.emplace_back(std::move(line), droppable);   // replies are kept even when no frame line could be dropped
    }
    out_cv_.notify_one();
}
bool TestPipes::connected() const { return cmd_connected_ && evt_connected_; }
bool TestPipes::disconnected() const { return disconnected_; }
uint64_t TestPipes::dropped() const { return dropped_; }
}  // namespace uc
