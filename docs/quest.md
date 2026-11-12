# Meta Quest 3 (VR)

The Quest app (`reuc-*.apk`) contains the port only. The game comes from **your own disc image** of the North American release (`RBUE08`). The app reads its files straight from the image every time, so it does not need to be unpacked.

## Install

1. Install the APK with SideQuest or `adb install reuc-release.apk`.
2. Connect the headset to the computer over USB and copy your image (`.wbfs` or `.iso`) into the folder **`UmbrellaChronicles`** at the top of the headset's internal shared storage, so that it is at

   ```
   /sdcard/UmbrellaChronicles/RBUE08.wbfs
   ```

   (any file name ending in `.wbfs` or `.iso` works; `/sdcard/Download/` is searched as well).
3. **The Wii common key.** Wii discs are encrypted. Unless your APK was built with the key in it (see below), also copy the Wii common key, a 16-byte file named **`common-key.bin`**, into the same folder. The port does not include this key.
4. Start **Umbrella Chronicles VR** from the library (Unknown Sources). On the first start, allow the app to **access all files** when it asks, then start it again.

If something is missing or wrong, the app shows a message and writes `HOW-TO-ADD-THE-GAME.txt` into `/sdcard/Android/data/org.uc.umbrellachronicles.vr/files/`. These images are refused: other regions (`RBUP08`, `RBUJ08`), a `main.dol` that differs from the one the APK was built for, and the `.rvz`, `.ciso`, `.wia` and split-`.wbfs` formats. Convert those to a plain `.iso` or one `.wbfs` first (for example with Dolphin or Wit).

An unpacked disc (`/sdcard/UmbrellaChronicles/game/DATA/sys/main.dol`, as older builds needed) still works. The app uses it only when it finds no image.

## Space warp

**Display > Space warp** (off by default) turns on Meta's Application SpaceWarp: the game runs at half the display rate (45 fps at
90 Hz) and the headset fills in every other frame from the game's motion vectors and depth. The display then gets a new picture every
refresh with an even cadence, at the full eye resolution. Without it, heavy scenes run at about 50 fps and the picture alternates between
one and two refreshes. Details: `docs/spikes/S23-quest-spacewarp.md`.

The Display page's first three rows are presets. Each sets the display rate, space warp, render scale and foveation (high) together,
saves them and restarts the game, so the eye images take the preset's size. The row in use shows "In use"; it does nothing when chosen:

| Preset | Display | Game | Render scale |
|---|---|---|---|
| 90 Hz, space warp | 90 Hz | 45 fps | native |
| 120 Hz, space warp | 120 Hz | 60 fps | 2.25x |
| 72 Hz, no space warp | 72 Hz | 72 fps | 2.25x |

The Quest 3 has no 60 Hz mode (72 is its lowest), so the preset without space warp runs the game at 72 to match the display.

## Building the APK

See the header of `scripts/build-quest.sh`. The build translates `main.dol` from `UC_INSTALL_DIR` and records its SHA-256 in the APK. At start-up the game checks the `main.dol` inside the player's image against it.

`UC_WII_COMMON_KEY_FILE` (in `local.env`, a 16-byte file on the build machine) builds the common key into that APK, so players need only the image. The key never enters the repository. A default build contains no key.

On a desktop build, `--game <image.wbfs>` reads an image the same way, with `common-key.bin` next to the image or next to the executable. `UC_WII_COMMON_KEY` (32 hex digits) in the environment also works.
