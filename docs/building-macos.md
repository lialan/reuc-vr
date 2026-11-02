# Building on macOS (Apple Silicon)

[Back to Building from source](building.md)

The port builds and runs natively on Apple Silicon Macs with the Vulkan renderer, on a Vulkan driver that runs on Metal. What is not there yet on
macOS: the launcher and the updater (start `uc` directly), DLSS/FSR and frame generation (vendor SDKs for Windows only), the DirectX renderers,
native (DLL) mods, Wii Remotes over Bluetooth HID. Lua and JavaScript mods, gamepads, mouse and keyboard, light guns as a mouse, netplay and the
test harness work.

## What you need

| Tool | Notes |
|---|---|
| macOS on Apple Silicon | Built and tested on macOS 27, M4 Pro |
| Command Line Tools (Apple clang) | `xcode-select --install` |
| Homebrew: `cmake ninja python vulkan-headers vulkan-loader vulkan-tools mesa molten-vk` | `mesa` brings KosmicKrisp, `molten-vk` MoltenVK |
| Slang shader compiler | A `slang-<version>-macos-aarch64` release from github.com/shader-slang/slang (tested: 2026.20); point `UC_SLANGC` at its `bin/slangc` |
| Wiimms ISO Tools (`wit`) | To unpack the disc image; build it from github.com/Wiimm/wiimms-iso-tools (`make wit` in `project/`), see below |
| Your NTSC-U disc image | `RBUE08`, `.iso` or `.wbfs` |

**Building `wit` on macOS:** Apple's linker refuses two packed tables of `dclib`. In `project/dclib/dclib-basics.h` drop
`__attribute__ ((packed))` from `sizeof_info_t` and `SaveRestoreTab_t`, and in `project/Makefile` replace the `-static-libgcc` line of the
non-mac branch so it is not used with clang. Then `make wit`.

## Steps

1. Unpack the disc. The `--pmode name` keeps the `DATA` folder of a single-partition image (as the launcher does):

   ```sh
   wit extract "Resident Evil - The Umbrella Chronicles (USA).wbfs" ~/Games/UC/game --pmode name
   shasum -a 1 ~/Games/UC/game/DATA/sys/main.dol   # 7ed67efe2ba9b0de1cef0bbb2ea157d7a43d7f25
   ```

2. `local.env` in the repository root (git-ignored):

   ```sh
   UC_INSTALL_DIR=/Users/me/Games/UC
   UC_SLANGC=/Users/me/tools/slang/bin/slangc
   VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json
   UC_THINLTO=ON
   ```

3. Build, test and install into `UC_INSTALL_DIR`:

   ```sh
   scripts/build.sh            # --no-install, --no-tests, --translate-only
   ```

   The steps are those of `build.bat`: the translator, the cached translation of `main.dol` (about 3 s on an M4 Pro), the build (the 107
   translated files compile in about 40 s), `ctest`, staging `dist/` and copying it next to `game/`.

4. Run it:

   ```sh
   cd ~/Games/UC && VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json ./uc --window-mode windowed --width 1280 --height 720
   ```

   Saves go to `saves/` next to `uc`, logs and crash reports to `logs/` (as on Windows). A fresh install has no saves: the game asks to create
   them. `UC_CAPTURE_EVERY=<seconds>` saves a screenshot of the game every few seconds into `debug/` (or `--capture-dir`).

## Which Vulkan driver

Both drivers translate Vulkan to Metal; the loader picks the one named by `VK_DRIVER_FILES`.

| | KosmicKrisp (Mesa) | MoltenVK |
|---|---|---|
| Chapter 1, 2560x1440 output, M4 Pro | ~83 fps | 120 fps (the display's refresh) |
| Render thread per frame | ~8 ms | ~5.6 ms |
| GPU per frame | ~10 ms | ~5 ms |
| GX logic ops | native | emulated with blending: CLEAR (the only one the game uses), NOOP and COPY are exact; any other op is drawn as COPY and named once in the log |

Neither driver offers `VK_EXT_dynamic_rendering_unused_attachments`, so bloom, ambient occlusion, motion blur and the upscalers are off on macOS for
now; the picture is the game's own. KosmicKrisp has no mailbox present mode; FIFO is used, `UC_PRESENT_MODE=immediate` turns vsync off. The first
start after a change of settings compiles the pipelines (KosmicKrisp ~1 min, MoltenVK ~15 s); later starts load them from the cache next to `uc`.

## Tests

`ctest --test-dir build -j 6` runs the 197 tests registered on macOS. Not registered on macOS: the updater, NAT and launcher tests, the DirectX
and native-mod tests, `test_wiimote_input`, `test_test_server`, `test_diagnostic_log` (the Windows tee), `test_image_compare` and the
`renderer_*_smoke` executables (WIC PNG decoding), `test_build_release` and `translator_publish_atomic` (Windows release and file-locking
tooling).

The harness runs too (`scripts/uctest`, UNIX sockets instead of named pipes):

```sh
cd scripts
python3 -m uctest run create_saves --exe build/game/uc   # a fresh install: creates a save and starts a new game; copy the run's saves/ to UC_INSTALL_DIR
python3 -m uctest run train_gameplay --speed 8 --exe build/game/uc
python3 -m uctest run perf --measure 20 --exe build/game/uc
```

## Platform notes for developers

- Platform code sits next to the Windows code as `*_posix.cpp` files (guest memory, host threads, savefs, the diagnostic log, the system
  report, the test transport, stubs) or behind `#ifdef _WIN32`; the POSIX files are written for Android/bionic as well.
- `runtime/sched.h` is now `runtime/guest_sched.h`: with `runtime/` on the include path it shadowed the system `<sched.h>`.
- No x86 SIMD intrinsics exist in the code: paired singles use clang vector types, which compile to NEON on arm64 (`-mcpu=apple-m1`).
- mimalloc replaces `operator new` on Windows only: on macOS a block allocated inside the system libc++ could reach `mi_free`.
