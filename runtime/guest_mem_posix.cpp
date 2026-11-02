// Guest memory on POSIX (macOS, Linux, Android): the layout of guest_mem.cpp on mmap.
#include "guest_mem.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

namespace uc {
static uint8_t* g_base;

// Guest addresses index the 4 GiB range directly (host address = base + guest address), so translated loads and stores need no address mask. The
// three RAM blocks are shared-memory objects mapped at every address the console reaches them by:
//   MEM1 24 MB: physical 0x00000000, cached 0x80000000, uncached 0xC0000000      MEM2 64 MB: 0x10000000, 0x90000000, 0xD0000000
//   locked cache 64 KB (16 KB on the console; dcbz_l, psq and lfs in the SDK use it): 0x20000000 (physical alias), 0xE0000000
// Everything else (the hardware registers at 0xCC000000 included) stays reserved without access; the translated code tests for the registers
// before it touches memory. Every size and address is a multiple of 16 KB, the page size of Apple Silicon (and of some Android kernels).
namespace {
struct View { const char* what; uint64_t size; uint64_t at[3]; unsigned count; };
constexpr uint64_t kSpan = 0x100000000ull;
const View kViews[] = {
    {"MEM1", 0x01800000, {0x00000000, 0x80000000, 0xC0000000}, 3},
    {"MEM2", 0x04000000, {0x10000000, 0x90000000, 0xD0000000}, 3},
    {"locked cache", 0x00010000, {0x20000000, 0xE0000000, 0}, 2},
};

[[noreturn]] void fail(const char* what, const char* step) {
    std::fprintf(stderr, "fatal: cannot map guest memory (%s: %s: %s)\n", what, step, std::strerror(errno));
    std::exit(4);
}

// An anonymous shared-memory object of `size` bytes: mapping the one object at several addresses makes those addresses alias.
int make_block(const View& view, unsigned index) {
#if defined(__linux__)
    // memfd_create through syscall(): bionic only declares the wrapper from API 30.
    const int fd = int(syscall(SYS_memfd_create, view.what, 1u /* MFD_CLOEXEC */));
    if (fd < 0) fail(view.what, "memfd_create");
#else
    // macOS has no memfd: a named POSIX object, unlinked at once (the name only lives for this call; it is at most 31 characters).
    const std::string name = "/uc-" + std::to_string(getpid()) + "-" + std::to_string(index);
    const int fd = shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) fail(view.what, "shm_open");
    shm_unlink(name.c_str());
#endif
    if (ftruncate(fd, off_t(view.size)) != 0) fail(view.what, "ftruncate");
    return fd;
}
}  // namespace

void guest_mem_init() {
    if (g_base) return;
    // The reservation keeps the whole 4 GiB range to the guest: nothing else can be placed between the views, and a stray access faults.
    void* span = mmap(nullptr, kSpan, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (span == MAP_FAILED) fail("4 GiB range", "mmap");
    auto* base = static_cast<uint8_t*>(span);
    for (unsigned v = 0; v < 3; ++v) {
        const int fd = make_block(kViews[v], v);
        for (unsigned i = 0; i < kViews[v].count; ++i) {
            // MAP_FIXED replaces that part of the reservation in one step.
            void* at = mmap(base + kViews[v].at[i], size_t(kViews[v].size), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
            if (at != base + kViews[v].at[i]) fail(kViews[v].what, "mmap view");
        }
        close(fd);   // the mappings keep the object alive
    }
    g_base = base;
}
uint8_t* guest_mem_base() { return g_base; }
}  // namespace uc
