# macOS (Apple Silicon) Vulkan Build Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `uc` (the game) builds on macOS arm64 with the existing Vulkan renderer, boots to the title screen in a 1280x720 window on KosmicKrisp, and plays chapter 1 with keyboard, mouse or gamepad and sound. This is the first step toward a Meta Quest 3 build.

**Architecture:** Leave the Windows code where it is. Each Win32-only unit gets a sibling `*_posix.cpp` that CMake picks with `if(WIN32)`. A few small call sites get `#ifdef _WIN32` instead. The POSIX files must also compile for Android/bionic, so the later Quest port reuses them. CMake options switch off what has no Mac path in milestone 1: DirectX, the NVIDIA/AMD vendor SDKs, the launcher, the updater and native mods. The translated game code stays portable C++ and is not changed.

**Tech Stack:**
- Toolchain: Apple clang 21 (Command Line Tools), CMake 3.25+ and Ninja (Homebrew).
- Shaders: Slang 2026.20 `slangc`, producing SPIR-V 1.5.
- Libraries: SDL3 3.2.0 (fetched), Dear ImGui 1.92.9b (fetched).
- Vulkan: Homebrew loader and headers, with the Mesa 26.2.6 KosmicKrisp driver.

**Spec:** The user's request: "build it on Mac with Vulkan first, then think about how to port to Quest 3." The investigation behind this plan is summarized in [Findings](#findings).

## Global Constraints

- **Host:** macOS 27, arm64 (M4 Pro), Apple clang 21. No Xcode IDE is needed.
- **Driver:** KosmicKrisp only: `VK_DRIVER_FILES=/opt/homebrew/share/vulkan/icd.d/kosmickrisp_mesa_icd.aarch64.json`. MoltenVK is not supported: it has no `logicOp`, and the renderer throws on GX logic ops (`renderer/vulkan_renderer.cpp:510`).
- **Tools and game data (this machine):**
  - `slangc`: `~/workspace/tools/slang/bin/slangc`, passed as `-DUC_SLANGC=...`.
  - `wit`: `~/workspace/tools/wiimms-iso-tools/project/wit`.
  - Game: `UC_INSTALL_DIR=~/workspace/reuc-install`, with `game/DATA/sys/main.dol` SHA-1 `7ed67efe2ba9b0de1cef0bbb2ea157d7a43d7f25`.
- **Never commit** `generated/`, game files, the disc image or anything derived from them. The repo `.gitignore` already covers `generated/`.
- **Windows must not change.** The Windows build must behave exactly as before. Windows-only code moves only into `#ifdef _WIN32` blocks or stays in its own file; no Windows logic is rewritten.
- **Floating point:**
  - No `-ffast-math`. Use `-ffp-contract=on` for `uc_game` and `uc_runtime`; this is clang's default and the same as the Windows build.
  - Never set FPCR.FZ or FPCR.DN.
  - Keep an explicit clamp before every float→int cast: arm64 saturates where x86 gives `0x80000000`.
- **Stacks:** guest threads get 8 MiB stacks (`runtime/sched.h:52`). Never `std::thread`, whose stack is 512 KiB on macOS.
- **Testing runs:** a 1280x720 window, never fullscreen (`AGENTS.md`). Never use the real saves: pass `--save-root build/autotest/<run>/saves`.

## Findings

What already works on a Mac, checked this session:
- The disc is unpacked with a natively built `wit 3.05a`, using the launcher's own command line (`wit extract <img> game --pmode name`).
- `slangc` 2026.20 builds `smoke.slang` (vertex shader and ubershader), and both pass `spirv-val --target-env vulkan1.3`.
- KosmicKrisp reports Vulkan 1.4, `logicOp`, `dualSrcBlend`, `shaderFloat16`, `shaderStorageImageExtendedFormats`, BC textures, 8 colour attachments, `VK_KHR_portability_enumeration` and `VK_KHR_swapchain_maintenance1`.
- It lacks:
  - `VK_EXT_dynamic_rendering_unused_attachments`. The renderer then turns post-effects and upscaling off (`vulkan_renderer.cpp:242-248`); that is acceptable for this plan.
  - Ray query, which switches itself off.
  - Geometry shaders and fp64, which the renderer does not use.

Portable already (no changes needed):
- The translator's emitted code: plain C++ with `ppc::op_*` helpers, no asm, SEH or intrinsics.
- Paired singles: clang vector extensions, which compile to NEON.
- The DVD filesystem: a `std::filesystem` file index, already case-insensitive.
- The scheduler's baton logic.
- From SDL3: the window, gamepad, mouse and audio.
- The ImGui SDL3+Vulkan overlay.
- About 50 pure-logic tests.

Blocked on Windows APIs (with milestone-1 handling):
- **Runtime:**
  - `runtime/guest_mem.cpp`: section views, 7 aliases in a 4 GiB span.
  - `runtime/sched.cpp`: `CreateThread`, affinity, cycle time.
  - `runtime/clock.cpp`: QPC.
  - `runtime/dispatch.cpp`: `RtlLookupFunctionEntry`.
  - `runtime/fatal.cpp` and `runtime/crash_report.cpp`: SEH, a vectored handler, `CONTEXT`.
  - `runtime/savefs.cpp`: NT file API.
  - `runtime/ppc_rt.h`: `<intrin.h>`, `_byteswap_*`, `_rotl`, `_ReadWriteBarrier`; included by every generated file.
- **App shell:**
  - `game/uc_main.cpp`: `wmain`, timers, affinity, `ShellExecuteW`.
  - `common/netplay.cpp`: Winsock; netplay is linked in unconditionally.
  - `modding/runtime_native.cpp`: `LoadLibrary`, SEH, BCrypt.
  - `common/input_config.cpp` and `file_rename.h`: NT rename.
  - `common/client_update_lock.cpp`: `LockFileEx`.
  - `game/test_pipe.cpp`: named pipes.
  - `game/wiimote_hid.cpp`: SetupDi/HidD.
  - Fonts read from `%WINDIR%\Fonts`: `launcher/ui_fonts.cpp`, `renderer/prompt_picture.cpp`.
  - `renderer/png.cpp`: WIC.
- **Build:**
  - `project(... RC)`.
  - `/clang:`, `/O2`, `/FI`, `/MAP` and `/lldltocache` flags.
  - Windows libraries: `ws2_32`, `dxgi`, `winmm`, `shell32`, `d3d12`, `windowscodecs`.
  - `.bat` build scripts.
  - Streamline, NGX and FidelityFX: Windows-only SDKs.

## Review Focus

1. **Guest memory aliases.** A store through `0x80001234` must be readable at `0x00001234` and `0xC0001234`; the same holds for MEM2 at `0x9xxxxxxx`/`0x1xxxxxxx`/`0xDxxxxxxx`. If the aliases are wrong, the game boots and then fails in strange ways. Pinned in Task 3.
2. **Deep guest call chains.** A guest thread that recurses 7 MiB deep must not crash, because macOS threads default to 512 KiB. Pinned in Task 4.
3. **Float→int on NaN and out-of-range values.** `fctiw`/`fctiwz` of NaN, +1e10 and -1e10 must give `0x80000000`, `0x7FFFFFFF` and `0x80000000`, as on PowerPC; plain arm64 casts saturate. The `gx.cpp:1770` cast may see NaN. Pinned in Task 7.
4. **Saves on APFS.** A symlink planted inside the save folder must be refused, not followed. A save interrupted between write and rename must leave the old file intact. Pinned in Task 6.
5. **Retina window.** The swapchain must use the window's pixel size (`SDL_GetWindowSizeInPixels`), not its point size. Otherwise a 1280x720 window shows a quarter-size image or a blurry one. Pinned in Task 10.

---

## Phase A: Build system

### Task 1: CMake configures on macOS; the translator builds; the `uc_common` tests pass

**Files:**
- Modify: `CMakeLists.txt:2-38`
  - `project(uc LANGUAGES C CXX)`, plus `if(WIN32) enable_language(RC)`.
  - Guard the MSVC-style flags on `CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC"`. Use `-mcpu=apple-m1` when `APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL arm64`.
  - ThinLTO: `-flto=thin` with `-Wl,-cache_path_lto,<dir>` on Apple.
  - Add `add_subdirectory(updater)`, `mods-examples/hello_native` and the updater-runtime lists only under `UC_BUILD_UPDATER`.
- Add options in the root `CMakeLists.txt`: `UC_RENDERER_DIRECTX`, `UC_VENDOR_UPSCALERS`, `UC_BUILD_LAUNCHER`, `UC_BUILD_UPDATER`, `UC_NATIVE_MODS`. Each defaults to `${WIN32}`.
- Modify: `common/CMakeLists.txt`, `renderer/CMakeLists.txt`, `launcher/CMakeLists.txt`, `game/CMakeLists.txt`, `modding/CMakeLists.txt`, `tests/CMakeLists.txt`.
  - Wrap Windows-only targets and libraries in the options above.
  - `renderer/CMakeLists.txt`: `find_program(UC_SLANGC slangc HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin" REQUIRED)`.
  - Move `uc_imgui`, `uc_imgui_vulkan`, `uc_ui_style`, `uc_launcher_locale` and `uc_gamepad_panel` out of the launcher guard: they are linked into `uc`.
  - Use `-O2` and `-include` in place of `/O2` and `/FI` when the compiler frontend is not MSVC.
- Create: `common/host_compat.h`
  - `uint16_t uc_bswap16(uint16_t)`, `uint32_t uc_bswap32(uint32_t)`, `uint64_t uc_bswap64(uint64_t)`: `__builtin_bswap*` on every compiler.
  - `uint32_t uc_rotl32(uint32_t, int)`: `std::rotl`.
  - `void uc_compiler_barrier()`: `std::atomic_signal_fence(std::memory_order_seq_cst)`.
  - `static_assert(std::endian::native == std::endian::little)`.
- Modify to use the new header:
  - `runtime/ppc_rt.h:6,56-96,209,257-258`.
  - `runtime/hle/sdk_math.cpp:76,78,386`.
  - `runtime/hle/gx.cpp:210`.
  - `tools/leaf_hle.py:325-332`, which emits `hle/leaf_gen.cpp`; regenerate that file.
- Modify: `tools/translator/main.cpp:13,88,100`. Use `getpid()` and `std::filesystem::rename` under `#ifndef _WIN32`.
- Modify: `common/cpu_topology.cpp:30-34` and `common/diagnostic_log.cpp:139-140`. Read the CPU brand with `sysctlbyname("machdep.cpu.brand_string")` on Apple and from `/proc/cpuinfo` elsewhere.

**Interfaces:**
- Produces: `common/host_compat.h` with the five functions above, used by all later tasks.
- Produces: the CMake options above. Later tasks add their `*_posix.cpp` files under `if(NOT WIN32)`.

- [ ] **Step 1:** Configure.
  - Run: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUC_SLANGC=$HOME/workspace/tools/slang/bin/slangc`.
  - Expected: configure completes, and the output names no Windows-only target.
- [ ] **Step 2:** `cmake --build build --target translator test_dol test_dol_scan test_isa test_analyzer test_isa_roundtrip test_emit test_dsy test_format opcheck_gen`. Expected: builds with no errors.
- [ ] **Step 3:** `ctest --test-dir build -R '^(test_dol|test_dol_scan|test_isa|test_analyzer|test_isa_roundtrip|test_emit|test_dsy|test_format)$' --output-on-failure`. Expected: 8/8 pass.
- [ ] **Step 4:** Commit: `macOS: CMake configures with Apple clang; the translator and uc_common tests build`.

### Task 2: `scripts/build.sh` translates the game and runs the build like `build.bat`

**Files:**
- Create: `scripts/build.sh`, `scripts/translate.sh`. Bash, `set -euo pipefail`, mirroring `build.bat` and `scripts/translate.bat`.
- Modify: `local.env.example`: document `UC_SLANGC` and `VK_DRIVER_FILES` for macOS.

**Interfaces:**
- `scripts/build.sh [--no-install] [--translate-only] [--no-tests]`, in this order:
  - Read `local.env` (`KEY=VALUE` lines; variables already set in the shell win).
  - Check `$UC_INSTALL_DIR/game/DATA/sys/main.dol`.
  - Configure, then build `translator`.
  - `python3 -I tools/hle_cfg.py project`.
  - Compute the cache key with `python3 -I tools/translate_key.py`, using the same `GAME_DOL`/`TRANSLATOR`/`CFG`/`HLECFG` environment variables as `translate.bat`.
  - Skip the translation when `generated/game_sources.cmake` contains the line `# translation-input: <KEY>`.
  - Otherwise run `build/tools/translator/translator --dol ... --cfg project/uc.cfg --cfg project/hle.cfg --out generated --shard 150 --cache-key <KEY>`, then `prune_generations.py`.
  - Reconfigure, then `cmake --build build`.
  - `ctest --test-dir build -j$(sysctl -n hw.ncpu)`, skipped with `--no-tests`.
  - `cmake --install build --prefix dist`.
  - `rsync -a dist/ "$UC_INSTALL_DIR/"`, skipped with `--no-install`.

- [ ] **Step 1:** `scripts/build.sh --translate-only`.
  - Expected: `generated/game_sources.cmake` exists, holds the `# translation-input:` line, and lists about 100 `game_NNN.cpp` files. The T1 baseline is 14,620 functions.
- [ ] **Step 2:** Run it again. Expected: it prints that the translation is cached and finishes in under 5 s.
- [ ] **Step 3:** Commit: `macOS: scripts/build.sh and translate.sh (the build.bat steps for POSIX)`.

## Phase B: Runtime POSIX layer

### Task 3: Guest memory on `mmap`

**Files:**
- Create: `runtime/guest_mem_posix.cpp`.
  - Reserve `kSpan` (4 GiB) with `mmap(PROT_NONE, MAP_PRIVATE|MAP_ANON)`.
  - Create one shared-memory object per block, with the `kViews` table copied from `guest_mem.cpp:19-23`:
    - macOS: `shm_open` with the name `/uc-<pid>-<n>`, then `shm_unlink` right away, then `ftruncate`.
    - Linux/Android: `memfd_create`.
  - Map each alias with `mmap(base+at, size, PROT_READ|PROT_WRITE, MAP_SHARED|MAP_FIXED, fd, 0)`.
  - On failure: `fprintf(stderr, "fatal: cannot map guest memory (%s)\n", strerror(errno)); exit(4)`, the same exit code as Windows.
- Modify: `runtime/CMakeLists.txt`. Choose `guest_mem.cpp` on WIN32 and `guest_mem_posix.cpp` otherwise.
- Test: create `tests/test_guest_mem.cpp` and register it in the `test_support` foreach at `tests/CMakeLists.txt:516`.

**Interfaces:**
- Consumes: `uc::guest_mem_init()` and `uint8_t* uc::guest_mem_base()` (`runtime/guest_mem.h`), unchanged.

- [ ] **Step 1:** Write `test_guest_mem`.
  - Write `0xA5` at `base+0x80001234`; read the same byte at `base+0x00001234` and `base+0xC0001234`.
  - Do the same for MEM2: write at `0x90002000`, read at `0x10002000` and `0xD0002000`.
  - Locked cache: write at `0xE0000010`, read at `0x20000010`.
  - Calling `guest_mem_init()` twice keeps the same base.
- [ ] **Step 2:** Build and run it. Expected before the implementation: link error, `guest_mem_init` undefined.
- [ ] **Step 3:** Implement `guest_mem_posix.cpp`.
- [ ] **Step 4:** `ctest -R '^(test_guest_mem|test_ppc_mem)$'`. Expected: both pass.
- [ ] **Step 5:** Commit: `macOS: guest memory aliases on mmap (shm/memfd)`.

### Task 4: Host threads and the clock

**Files:**
- Create: `runtime/host_thread.h`, with these functions:
  - `struct HostThread { void* handle; };`
  - `HostThread host_thread_start(void (*fn)(void*), void* arg, size_t stack_bytes);`
  - `void host_thread_join(HostThread);`
  - `uint64_t host_thread_cpu_ns();`
  - `void host_thread_set_affinity(uint64_t mask);` (no-op on Apple)
  - `void host_thread_set_interactive();` (QoS `USER_INTERACTIVE` on Apple, `SetThreadPriority` on Windows)
- Create: `runtime/host_thread_win.cpp`. Move the existing calls there unchanged: `CreateThread`, `WaitForSingleObject`, `CloseHandle`, `QueryThreadCycleTime`, `SetThreadAffinityMask`.
- Create: `runtime/host_thread_posix.cpp`. Uses `pthread_attr_setstacksize`, `pthread_join` and `clock_gettime(CLOCK_THREAD_CPUTIME_ID)`; affinity is `sched_setaffinity` on Linux and a no-op on Apple.
- Modify: `runtime/sched.cpp:13,19,207-209,534,565-567`. Replace the Win32 calls with `host_thread_*`.
  - `host_thread_cpu_ns` returns nanoseconds while Windows `QueryThreadCycleTime` returns cycles. Check every consumer that compares those numbers and keep their units consistent.
- Modify: `runtime/clock.cpp:3,10-15`. Use `mach_absolute_time` and `mach_timebase_info` on Apple and `CLOCK_MONOTONIC` elsewhere, all under `#ifndef _WIN32`.
- Test: add a case to `tests/test_sched.cpp` that starts a guest thread through the scheduler and recurses with 7 MiB of locals (`volatile char[64*1024]` per frame, 112 frames).

- [ ] **Step 1:** Write the deep-stack case and run it unported. Expected: fails to build (Win32 calls).
- [ ] **Step 2:** Implement, then run `ctest -R '^(test_sched|test_clock|test_sched_restore|test_os_hle)$'`. Expected: all pass, including the deep-stack case.
- [ ] **Step 3:** Commit: `macOS: guest threads on pthreads with 8 MiB stacks; monotonic clock`.

### Task 5: Dispatch lookup, fatal errors and crash reports

**Files:**
- Modify: `runtime/dispatch.cpp:23-36`.
  - Add `const FnEntry* dispatch_find_host_pc(uintptr_t pc)`: `std::upper_bound` over the existing sorted `g_by_host`.
  - Use it on every platform. The Windows unwind-data path stays as a second check under `_WIN32`.
- Modify: `runtime/crash_report.h:7,15`. Change `const struct _CONTEXT*` to `const void* host_context`. On Windows the argument is still a `CONTEXT*`.
- Create: `runtime/fatal_posix.cpp`.
  - `sigaction` for SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGTRAP, with `SA_SIGINFO|SA_ONSTACK`.
  - A 64 KiB `sigaltstack` per thread, installed from `host_thread_start` and from the main thread.
  - When `si_addr` falls in `[base, base+4 GiB)`, decode it as a guest address, using the same message text as `fatal.cpp`.
  - Then `_exit` with the Windows exit code.
- Create: `runtime/crash_report_posix.cpp`.
  - `backtrace()` for the stack.
  - pc/sp from `ucontext_t`: `uc_mcontext->__ss.__pc` on Apple, `uc_mcontext.pc` on Linux.
  - `localtime_r` and `fopen`.
  - The throw-site capture becomes a no-op.
- Modify: `game/uc_main.cpp:110`. Call `dispatch_find_host_pc`.
- Test: `tests/test_crash_report` (existing) must pass. Add a case that writes to `base+0xCC000000` (unmapped) in a forked child; the parent sees exit code 4 and stderr containing `guest address CC000000`. Copy the exact wording from `fatal.cpp`.

- [ ] **Step 1:** Write the forked-child case. Expected before the implementation: fails (no handler, the child dies with SIGSEGV).
- [ ] **Step 2:** Implement, then run `ctest -R '^(test_crash_report|test_crash_bundle)$'`. Expected: pass.
- [ ] **Step 3:** Commit: `macOS: signal-based fatal handler and crash reports; portable host-PC lookup`.

### Task 6: Save filesystem on POSIX

**Files:**
- Modify: `runtime/savefs.h:6,49,58`. Remove `windows.h`; the pinned directory becomes `using SaveDirHandle = intptr_t;` (a HANDLE on Windows, an fd on POSIX). Fix the includers `hle/save.h` and `save_rollback.h`.
- Create: `runtime/savefs_posix.cpp`, implementing every function `savefs.cpp` exports:
  - Directory fds act as the pins.
  - Open files with `openat(dirfd, name, O_NOFOLLOW|O_CLOEXEC)`; check links with `fstat` and `st_nlink == 1`.
  - Write to a temp file, `fcntl(F_FULLFSYNC)` on Apple (`fsync` elsewhere), then `renameat`. For no-replace semantics use `renameatx_np(RENAME_EXCL)` on Apple and `renameat2(RENAME_NOREPLACE)` on Linux.
  - Map `errno` values to the same ISFS codes as the Win32 mapping in `savefs.cpp:92-108`.
- Modify: `runtime/CMakeLists.txt`. Select the file per platform.
- Test: `tests/test_savefs` (existing) passes. Add two cases:
  - A symlink `save/x.bin -> /tmp/outside` is refused with the same ISFS error code Windows returns for a reparse point.
  - A child process killed between the temp write and the rename leaves the old contents readable.

- [ ] **Step 1:** Write the two new cases. Expected before the implementation: fail.
- [ ] **Step 2:** Implement, then run `ctest -R '^(test_savefs|test_save_rollback|test_hle_restore)$'`. Expected: pass.
- [ ] **Step 3:** Commit: `macOS: savefs on openat/renameat with F_FULLFSYNC`.

### Task 7: Remaining runtime sites; the full CPU/runtime suite passes

**Files:**
- Modify: `runtime/dvdfs.cpp:86` and `modding/arc/arc_mods.cpp:101`. Put `fs::file_type::junction` under `#ifdef _WIN32`.
- Modify: `runtime/native_input.cpp:68,73`. Use `fopen(path.c_str())` off Windows.
- Modify: `runtime/hle/gx.cpp:1770`. Clamp the value, NaN → 0, before the cast, as `gx.cpp:1714` does.
- Test: in `tests/test_ppc_float.cpp`:
  - `op_fctiw` and `op_fctiwz` give `0x80000000` for NaN and -1e10, and `0x7FFFFFFF` for +1e10.
  - `op_fmadds(1+2^-23, 1+2^-23, -1)` equals the single-rounded fused result.

- [ ] **Step 1:** Add the cases, then run `ctest -R '^(test_ppc_int|test_ppc_mem|test_ppc_float|test_clock|test_mmio|test_lowmem|test_sched|test_os_hle|test_dvd|test_vi|test_exi|test_ios|test_dsp|test_gx_audit|test_render_lifecycle|test_sdk_math_hle|test_sdk_regs_hle|test_leaf_hle|test_dvdfs_overlay|test_ax_audio|test_rollback_memory)$' --output-on-failure`, and `cmake --build build --target opcheck_obj` (the generated per-opcode checks must compile). Expected: all pass.
- [ ] **Step 2:** Commit: `macOS: the runtime and CPU test suite passes on arm64`.

## Phase C: The translated game links and boots headless

### Task 8: `uc_game` compiles and `uc_smoke` finds the entry point

**Files:**
- Modify: `game/CMakeLists.txt:57-66`. Use the non-MSVC flags from Task 1 (`-O2`, `-include entry_probe.h`) and keep the `-Wno-*` set.
- Modify: `game/CMakeLists.txt`. Make `/MAP` WIN32-only. On Apple use `-Wl,-map,<dir>/uc.map`; the crash tools read this file.

- [ ] **Step 1:** `cmake --build build --target uc_smoke`. Expected: all `game_NNN.cpp` compile. Record the wall time and peak RSS in the commit message for later comparison.
- [ ] **Step 2:** `build/game/uc_smoke ~/workspace/reuc-install/game/DATA/sys/main.dol`. Expected: `functions 14620`-ish, and `entry 8000xxxx known: yes`.
- [ ] **Step 3:** `ctest -R test_step_probe_selfcheck`. Expected: pass.
- [ ] **Step 4:** Commit: `macOS: the translated game compiles and links (uc_smoke)`.

### Task 9: App-shell ports; `uc --headless` runs the game 30 s

**Files (each is a `#ifdef` block or a `*_posix.cpp` sibling; nothing Windows-side changes):**
- `game/uc_main.cpp:1268-1288`: add `int main(int argc, char** argv)` off Windows.
- `uc_main.cpp:177`, `game/options.cpp:17` and `common/public_release.h:17-19`: `fs::path exe_dir()` with `_NSGetExecutablePath` on Apple and `/proc/self/exe` on Linux.
- `uc_main.cpp:229-241,362-372,629,928,1238`:
  - Timers: `timeBeginPeriod`, EcoQoS and the waitable timer become no-ops; sleep with `nanosleep` plus the existing spin.
  - CPU affinity becomes `host_thread_set_affinity`.
  - Thread cycle time becomes `host_thread_cpu_ns`.
  - `ShellExecuteW` becomes `SDL_OpenURL`.
  - `GetTickCount64` becomes `steady_clock`.
- `game/netplay_game.cpp:510,665`: `host_thread_cpu_ns`.
- `game/frame_log.cpp:29`: `host_thread_set_interactive`.
- `game/frame_log.cpp`, `game/det_trace.cpp`, `game/netplay_saves.cpp:42`, `modding/mod_zip.cpp:60`: `fopen` and `getpid` replacements.
- `common/netplay.cpp:19-157`: BSD sockets (`socket`, `bind`, `sendto`, `recvfrom`, `fcntl O_NONBLOCK`, `close`, `errno`), with an `int` socket typedef.
- `common/input_config.cpp:167-182` and `common/file_rename.h`: write a temp file, `fsync`, then `rename`.
- `common/client_update_lock.cpp`: `flock`.
- Stubs, as `*_posix.cpp` files with the same exported signatures:
  - `game/compile_screen` (no popup; the ubershader covers compiles).
  - `game/system_report` (OS version from `uname`, GPU from Vulkan, no DXGI).
  - `game/write_trap` and `game/cpu_sampler` (no-ops; their entry points log "not supported on this platform").
  - `game/test_pipe` (refuses `--test-server` with a clear error until Task 12).
  - `game/wiimote_hid`: `wii_open_first` returns `nullptr`.
  - `modding/runtime_native`: refuses native mods with "native mods are Windows-only".
  - `launcher/launch.cpp`: `query_features` returns `""`.
  - `common/diagnostic_log`: no stdio tee.
- `modding/runtime_native.cpp:64-83` and `modding/menu_ui.cpp:185`: SHA-256 via a small portable implementation `common/sha256.h`: `std::array<uint8_t,32> sha256(std::span<const uint8_t>)`. Test it against the NIST vectors for `""` and `"abc"`.
- `launcher/localization.cpp:35`: `SDL_GetPreferredLocales`.

- [ ] **Step 1:** Add `tests/test_sha256.cpp` with the two NIST vectors; run it. Expected: it fails before the implementation and passes after.
- [ ] **Step 2:** `cmake --build build --target uc`. Expected: links.
- [ ] **Step 3:** `build/game/uc --headless --seconds 30 --game ~/workspace/reuc-install/game --save-root build/autotest/headless/saves --no-mods`.
  - Expected: exit 0.
  - `runtime.log` (with `UC_LOGS=1`) shows retraces and `[dvd] open` lines.
  - The watchdog does not fire. It exits with code 4 if there are no retraces in 15 s.
- [ ] **Step 4:** `ctest -R '^(test_options|test_options_config|test_frame_limit|test_host_keys|test_live_settings|test_netplay|test_netplay_stream|test_netplay_datagram|test_netplay_frame|test_netplay_saves|test_game_loop|test_input_bindings|test_gamepad_config|test_wiimote_config|test_input_prompts|test_sha256)$'`. Expected: pass.
- [ ] **Step 5:** Commit: `macOS: uc runs the game headless (app-shell POSIX ports and stubs)`.

## Phase D: Vulkan on KosmicKrisp

### Task 10: Vulkan-only renderer build and the portability path

**Files:**
- Create: `renderer/vendor_stubs.cpp`, compiled when `NOT UC_VENDOR_UPSCALERS`:
  - `Streamline::load` returns `nullptr` with the reason `"not available on this platform"`.
  - The DLSS and FSR upscaler and frame-generation factories throw `std::runtime_error("... not available on this platform")`.
  - `make_ngx_backend` returns `nullptr`.
- Modify: `renderer/CMakeLists.txt:174-180`.
  - Drop `streamline.cpp`, `dlss_*.cpp`, `fsr_*.cpp`, `ffx_dx12.cpp`, `ngx_vulkan.cpp`, `native_frame_generation.cpp` and `native_dlss5.cpp` unless the vendor/DX options are on.
  - Link `d3d12`, `dxgi` and `windowscodecs` only on WIN32.
  - Leave out `uc_directx` and the `native_*` smoke targets unless `UC_RENDERER_DIRECTX`.
- Modify: `renderer/png.cpp`. Off Windows, encode with miniz's `tdefl_write_image_to_png_file_in_memory_ex` (miniz is already a dependency).
- Modify: `renderer/game_renderer.h:13-14`. Put the DX11/DX12 cases under `#if UC_RENDERER_DIRECTX`; `--renderer dx11` then reports "not available on this platform".
- Modify: `renderer/vulkan_renderer.cpp`.
  - `:3,13`: put `windows.h` and `vulkan_win32.h` under `_WIN32`.
  - `:233,1607-1609`: exclusive fullscreen only on `_WIN32`.
  - `:110` and `upscaler_probe.cpp:70,118,149,194`: when the loader lists `VK_KHR_portability_enumeration`, add it and set `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`.
  - `:228`: enable `VK_KHR_portability_subset` when the device lists it.
  - `:277`: enable `shaderStorageImageExtendedFormats` when supported.
  - Swapchain extent: use `SDL_GetWindowSizeInPixels`.
  - `:405`: check BC7 support before using replacement textures; skip the replacement with a log line when it is missing.
- Modify: `game/uc_main.cpp:511`. Add `SDL_WINDOW_HIGH_PIXEL_DENSITY`.
- Fonts:
  - Create: `common/system_fonts.h`, with `enum class UiFont { sans, sans_bold, serif };` and `std::vector<fs::path> ui_font_candidates(UiFont)`.
  - Candidates by platform:
    - Windows: `segoeui.ttf`, `segoeuib.ttf` and `georgia.ttf` from `%WINDIR%\Fonts`, as today.
    - Apple: `/System/Library/Fonts/Supplemental/Arial.ttf`, `Arial Bold.ttf` and `Georgia.ttf`.
    - Android: `/system/fonts/Roboto-Regular.ttf`, `Roboto-Bold.ttf` and `NotoSerif-Regular.ttf`.
  - CJK merges: `PingFang.ttc`, `AppleSDGothicNeo.ttc` and `Hiragino Sans GB.ttc` on Apple, all optional.
  - Use it from `launcher/ui_fonts.cpp:14-36`, `renderer/prompt_picture.cpp:165-171` and `game/compile_screen.cpp:78`.
- Modify: `renderer/menu_overlay.cpp:2`, `renderer/prompt_picture.cpp:5` and `game/settings_menu.cpp:8`. Put `<intrin.h>` under `_WIN32`.
- Port the GPU smoke harness `renderer/renderer_smoke.cpp:21,49-57`: `exe_dir()` from Task 9, PNG decode via SDL3's `SDL_LoadPNG` if available or `stb_image`, and drop the HWND `ShowWindow` call.

- [ ] **Step 1:** `VK_DRIVER_FILES=<kosmickrisp json> build/renderer/renderer_smoke`.
  - Expected: it exits 0 and logs device `Apple M4 Pro`, driver KosmicKrisp, and "effects off: no VK_EXT_dynamic_rendering_unused_attachments".
  - The Retina check from Review Focus 5 is part of this test: in a 1280x720-point window on a 2x display, the swapchain extent is 2560x1440.
- [ ] **Step 2:** `ctest -R '^(test_buffer_memory|test_pipeline_keys|test_rt_instances|test_gx_.*|test_rt_.*_math|test_hdr_math|test_bloom_math|test_post_effects|test_upscale_math|test_render_scale)$'`. Expected: pass.
- [ ] **Step 3:** Commit: `macOS: Vulkan-only renderer on KosmicKrisp (portability enumeration, Retina extent, system fonts)`.

### Task 11: Windowed boot to the title screen; chapter 1 plays

- [ ] **Step 1:** `VK_DRIVER_FILES=... UC_LOGS=1 build/game/uc --game ~/workspace/reuc-install/game --window-mode windowed --width 1280 --height 720 --skip-intro --no-mods --save-root build/autotest/win/saves --capture-dir build/autotest/win/cap`.
  - Expected: the title screen renders, with no Vulkan validation errors when `VK_LOADER_LAYERS_ENABLE=*validation` is set.
  - The capture holds a non-black frame.
  - `[pipeline]` lines show compiles going through the ubershader.
  - Sound plays.
- [ ] **Step 2:** Manual play, done by the user or with the Task 12 harness. Expected:
  - Keyboard and mouse aim, fire and reload.
  - An SDL gamepad navigates the menu.
  - Chapter 1 loads and plays for 2 minutes without a GX error or a hitch storm (`[hitch]` lines in `runtime.log`).
- [ ] **Step 3:** Run the pipeline-seed warm start twice; the second launch must reach the title screen at least 50% faster.
  - Expected: `project/pipeline_keys.seed` is used and the Vulkan pipeline cache is written next to `uc`.
- [ ] **Step 4:** Fix what Steps 1–3 find. Each fix is a separate commit with its own failing test, where the failure can be expressed as one (for example a GX state case in `test_gx_*`).

## Phase E: Harness and docs

### Task 12: `uctest` on POSIX

**Files:**
- Create: `game/test_pipe_posix.cpp`.
  - A UNIX domain socket at `$TMPDIR/uc-test-<name>.sock`, carrying the same JSON-lines protocol as the named pipes (`game/test_pipe.cpp:9-16`).
  - It replaces the Task 9 stub.
- Modify: `scripts/uctest/transport.py:9-10`. Off Windows, connect with `socket.AF_UNIX` to the same path.
- Modify: `scripts/uctest`. Mark the Windows-only parts as skipped off Windows: `window_startup.py` (user32 through ctypes) and `ip_loss.py` (WinDivert).

- [ ] **Step 1:** `cd scripts && VK_DRIVER_FILES=... python3 -m uctest run train_gameplay --speed 8`. Expected: the scenario passes and the screenshots are non-black.
- [ ] **Step 2:** `python3 -m uctest demo check fight2`, if the demo exists. Expected: replay equals the recording. This checks determinism on arm64 against the recording.
- [ ] **Step 3:** Commit: `macOS: the uctest harness over a UNIX socket`.

### Task 13: macOS build guide and the full test run

**Files:**
- Create: `docs/building-macos.md`. It covers:
  - prerequisites (Homebrew list, Slang, Mesa/KosmicKrisp);
  - unpacking with wit (`--pmode name`);
  - `local.env` variables;
  - `scripts/build.sh`;
  - running with `VK_DRIVER_FILES`;
  - the list of tests excluded on macOS, with the reason for each (updater, NAT, launcher, DirectX, NGX, native mods, Wii Remote HID).
- Modify: `docs/building.md`. Add a link to the new guide.

- [ ] **Step 1:** `rm -f build/tests.state.json && scripts/build.sh --no-install`. Expected: every test registered on macOS passes. Record the counts (passed/registered) in the doc.
- [ ] **Step 2:** Commit: `macOS: build guide and test report`.

---

## Not in this plan (next plans)

- **Post-effects and upscaling on KosmicKrisp.** The renderer needs pipelines per attachment count in place of `VK_EXT_dynamic_rendering_unused_attachments`. That unlocks bloom, SMAA, GTAO and motion blur on Apple GPUs.
- **Wii Remote** through `SDL_hid_*`, and **native mods** through `dlopen`.
- **The launcher and updater** on macOS.
- **Quest 3 (Android arm64, Adreno 740)**, a separate plan. This plan keeps that path open in four ways:
  - The POSIX files compile for bionic; guest memory uses `memfd_create`.
  - `scripts/build.sh` keeps the translator a host tool, so a later NDK cross-build runs the Mac-built translator.
  - Fonts have an Android candidate list.
  - The game files must be read from a real directory (`/sdcard/Android/data/<pkg>/files/game`), not from inside the APK.
- **Quest-specific work:**
  - an OpenXR stereo presenter, reusing the lindberghrecomp OpenXR work from gse-recomp;
  - BC7 replacement textures (Adreno has no BC: CPU decode or skip);
  - Android swapchain pre-rotation;
  - thermal and GPU-level tuning, measured on the headset only.
