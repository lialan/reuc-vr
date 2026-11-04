# Latest continuation: cabin missing surfaces are still unresolved

Updated 2026-11-04 after the owner's follow-up. **Clarity is accepted; missing or
black patches on cabin surfaces are not fixed or accepted.** Keep native
2064x2208. Detailed evidence and failed controls are at the top of
[S19](docs/spikes/S19-quest-clarity.md).

- Current source adds actual XR swapchain capture (`UC_CAPTURE_XR=1`) and a fixed
  diagnostic yaw (`UC_VR_TEST_YAW`). Ordinary screenshots take a different path
  from normal direct XR presentation; earlier clean screenshots alone were
  insufficient. Fixed-pose images still do not establish tracking correctness.
- Corrected the XR screen pipeline's missing disabled depth/stencil state.
  This is a confirmed Vulkan validity error, **not a confirmed cabin fix**.
  Matched before/after scene pixels are unchanged except the stats display.
- Inspected current actual-XR output in the first cabin, stairs and dining car,
  forward/down/side views; two live dining-car recordings and a 25-second live
  first-cabin check. No large missing patches were identified in the sampled
  frames. This does not override the owner's report.
- The normal player's settings were read without changing them: native eyes
  and render scale, **medium foveation**, 90 Hz, scale 10, screen 3. A retained
  normal-play log starts with foveation off, then switches to low/medium, and
  reaches first-cabin room `r000c`. All 2,713 installed shader/data assets match
  the APK byte-for-byte. Its 78 pipeline warm-up errors also occur in isolated
  runs; no visual causality established. Temporary export code was removed.
- Remaining information needed: identify a missing patch in
  `priority-fix/cabin-early/sheet.jpg`, or get a short recording of the owner's
  faulty view. Async questions about the exact scene and these captures have
  not been answered yet. Do not repeat the same broad flag sweeps.
- Evidence root: `/Users/lialan/workspace/reuc-vr-glitch/priority-fix/`.
  `player-log-check/logs/player-uc.log` is the untouched normal-play log;
  `cabin-early-live/sheet.jpg` covers the live first cabin and zombie attacks.
- Current APK is preserved as `cabin-diagnostics.apk`, SHA256
  `c22b70bffa1780510f1b25e1e63c161106c038f4eb488d7856f0c07914420854`.
  Installed readback/cleanup is recorded in `cabin-installed-final.json`.
  The accepted clarity build `clarity-fixed.apk` remains available.
- Quest/host renderer builds, three focused CTests and diff checks passed.
  Temporary archives, export code and private overrides were cleaned. Useful
  contact sheets, selected raw images, logs and metrics remain; redundant new
  captures were pruned. App/device automation stopped after verification.

Quest authorization was given for this task and remains valid unless the owner
takes over or revokes it. Use quest3-lock and isolated saves. Push only to
`lialan`, never `origin`.

---

# Earlier continuation: native clarity and the confirmed doubled-edge glitch

Updated 2026-11-04. This section supersedes the hypotheses and defaults in the
historical handoff below. Detailed source/evidence notes: [S19](docs/spikes/S19-quest-clarity.md).

**Owner follow-up: clarity is good, but cabin rendering is still incorrect.**
The remaining symptom is missing or black patches on surfaces. The refraction
comparison below establishes only that one effect was removed; overall 3D
correctness is not accepted. Keep native clarity while investigating the missing
surfaces.

## Owner's confirmed priorities

- Glitch is in 3D, not 2D: flickering/doubled edges, visible with the head still.
- Blurry/low-resolution output: **native resolution takes priority over FPS**.
- Owner confirmed the displaced, wavering carpet/curtain copies during the
  close-range leech attack are the reported glitch.
- Quest use was authorized for this task. All operations used quest3-lock.
  Automation is now stopped; do not restart if the owner takes the headset.
- Clean temporary files each iteration. Do not push to origin; use lialan only.

## Result and current state

Native defaults restored on Android: 2064x2208 per eye and 3D scene, render scale
native, foveation off; settings generation 11 migrates the old performance
profile. Native remains roughly 20-37 rendered FPS in this replay's combat,
depending on the attack; this is not a 60 FPS performance result.

The confirmed displaced copies are removed in matched Quest captures. The VR
adaptation disables indirect displacement only for orthographic scene-copy
refraction overlays, preserving masks/tint, perspective particles, HUD and
ordinary bloom. **This removes a flat-screen refraction effect in VR; it is not
complete stereo reconstruction of that effect.** Desktop behavior is retained.
Logical EFB-copy dimensions now drive stereo indirect math, matching Dolphin's
native-size distinction. That math correction alone was insufficient.

- Worktree/branch unchanged: macos-vulkan / quest3-vr, based on 3cea3b8.
- Continuation commit title: `Restore native Quest clarity and remove screen-space refraction ghosting`.
- Final installed APK was read back and SHA256 matched the build:
  `7eeeb5c41361e8323b6c1795357d0271f1fee83c96c6529f6c2b0500cda1e859`.
- Preserved final APK: `/Users/lialan/workspace/reuc-vr-glitch/priority-fix/clarity-fixed.apk`.
- Preserved baseline installed APK SHA256:
  `fd8f1a1d2231affed902275869f31bc7e4669419603b947f20e3f7188cf6ff92`.
- App stopped; private env.txt, args.txt, fightvr.demo, captures and logs absent.
  Installed-package/cleanup verification: `priority-fix/installed-final.json`.
- Normal-launch external config files were unreadable through adb/run-as.
  Defaults/migration and the sandbox's native settings are verified; unrelated
  explicit config/env overrides in the normal app folder were not overwritten.

## Evidence to use next

Evidence base: `/Users/lialan/workspace/reuc-vr-glitch/priority-fix/`.

- `before-after.jpg`: same eye, same game frame, native resolution in both.
- `close-final/sheet.jpg`: room at retrace 7000 and attack 8100..8800, nine
  inspected captures; game exited 0. Three repeated attack frames are exactly
  identical to the first adapted run. The early room is unchanged.
- `close-original-control`: final APK with `UC_VR_SCREEN_REFRACTION=1` and
  `UC_INDIRECT_PHYSICAL_SIZE=1` exactly reproduces both original faulty frames
  at 8600/8800, zero differing pixels. Duplicate PNGs pruned; metrics retained.
- `close-nomerge`: all eight original attack captures byte-identical with
  draw merging disabled. Do not retry merging as the cause of this reproduction.
- `close.demo` SHA256:
  `0b43515b9f380b990ba0c994b4f46470ad201ac13cbececb45fb09ca12f68785`.
- `replay.py`: run under quest3-lock, uses the private sandbox and isolated
  saves, hashes APK, pulls useful captures/logs, deletes temporary archives and
  private overrides. Set REUC_REPLAY_DEMO to close.demo, REUC_REPLAY_STOP=8811,
  UC_VR_TEST_PITCH=-30 and UC_CAPTURE_RETRACES as desired. Fixed pose is for raw
  EFB comparison only, not tracking acceptance.

Five focused CTests, seven Python client checks, four settings migration cases,
Quest/macOS renderer builds and SPIR-V layout checks passed. Full-chapter and
worn-headset acceptance are not claimed. Original pre-task evidence is retained;
redundant new captures were pruned after recording metrics/contact sheets.

One diagnostic capturing 24 full-resolution passes hit the deterministic
scheduler's async-result watchdog after seven PNGs. The serial capture diagnostic
now captures one packet; that failed run is not gameplay evidence. See S19 for
failed D32/HUD-layer leads and the corrected Quest god/tutorial flag forwarding.

---

# Historical handoff (pre-continuation)

# Handoff: the Quest 3 VR 3D rendering glitch (REUC)

Written 2026-11-04 at the end of a session that bisected it. Start by reading this file, then `AGENTS.md`.

## Goal

Find and fix the 3D rendering glitches in the Quest 3 VR build of Resident Evil: The Umbrella Chronicles (Wii recomp, Vulkan + OpenXR):

1. **The colour rectangle** (reported by the user, the original complaint): a coloured rectangle in the view. Where and when it
   shows (stage, moment, 3D scene or HUD) is **not known yet**, and no recording has caught it. Ask the user first.
2. **Black holes in the 3D scene**: stair-stepped black holes and wedges cut into the floor and rug, in both eyes, lasting seconds.
   Reproduced clearly with `UC_NO_HUD_LAYER=1`; possibly the same fault at a low rate on the defaults (a few small black slivers).

A related bug was already fixed: the small-draw ubershader on the HUD layer drew a panel of garbage texels mid-view, so commit
`185f7f8` limits the ubershader to the EFB passes by default (`renderer/vulkan_renderer.cpp:440`).

## Where things are

- Code: `/Users/lialan/workspace/reuc-recomp/.claude/worktrees/macos-vulkan`, branch `quest3-vr`, tip `3cea3b8`, clean.
  Pushed to the private remote `lialan` (github.com/lialan/reuc-recomp). **Never push to `origin`** (the upstream solleo95 repo).
- Headset: Quest 3, adb serial `2G97C5ZJ0G02DQ`, package `org.uc.umbrellachronicles.vr`. The latest build (`3cea3b8`) is installed.
- Bisect evidence (durable copy, 209 MB): `/Users/lialan/workspace/reuc-vr-glitch/`
  - `rec-b-<variant>.mp4`: 12 s headset screen recordings (both eyes side by side), plus `.fight.log` each
  - `sheet-b-<variant>.png`: 4x3 contact sheets, one frame a second
  - `f-nohud-3s.png`: the black holes (HUD layer off, 3 s in). `f-def-6s.png`: a defaults frame with a sliver
  - scripts: `rec.sh`, `plan_bisect.sh`, `qlock.sh`, `sheet.sh`, `flick2.py`, `dark.py` (described below)

## Rules (from the user; they are not negotiable)

- **Touch the Quest only when the user says so in that message.** Never run scripts on it while the user is playing. Always run under
  `quest3-lock` (the headset is shared; exit 75 = busy, `qlock.sh` waits its turn).
- **Disk:** check `df -h /` before and between batches of runs. Each headset run archive is ~1.9 GB in
  `build-android/quest-runs/uctest-fight-*`; delete it once its result is summarized (`rec.sh` already does). The disk filled
  completely once (54 GB of archives) and stopped all work.
- Keep anything you still need out of `/tmp`: the Mac rebooted once and `/tmp` lost all the recordings.
- Re-check that the APK is built from the current tip before a run (`quest/reuc/build/outputs/apk/debug/reuc-debug.apk`).
- Commit on `quest3-vr`, push only to `lialan`.

## How to build and run

```sh
cd /Users/lialan/workspace/reuc-recomp/.claude/worktrees/macos-vulkan
scripts/build-quest.sh                 # -> quest/reuc/build/outputs/apk/debug/reuc-debug.apk (~2 min incremental)

# one recorded headset run of a variant (installs the APK, plays the fight, records 12 s once the fight measurement starts):
/Users/lialan/workspace/reuc-vr-glitch/qlock.sh /Users/lialan/workspace/reuc-vr-glitch/rec.sh <label> [--quest-set NAME=VALUE ...]
/Users/lialan/workspace/reuc-vr-glitch/sheet.sh <label>      # contact sheet
python3 /Users/lialan/workspace/reuc-vr-glitch/flick2.py /Users/lialan/workspace/reuc-vr-glitch/rec-<label>.mp4
```

`rec.sh` runs `python3 -u -m uctest run fight --quest --god --measure 25 <args>` from `scripts/`. That reinstalls the APK, stages
the game into the app's private test folder, writes `--quest-set` values into that folder's `env.txt` (a normal launch by the user
never reads it), autoplays to the fight's saved slowdown point (game second 627, 30-actor wave), and measures 25 s at a 60 FPS limit.
When the log prints `fight: slowdown at game second`, it waits 3 s and runs `adb shell screenrecord --time-limit 12`.
One run takes ~4 min. The whole six-variant `plan_bisect.sh` took ~25 min.

What to expect:
- The log prints `fight 1/1 PASS` and `fight exit=0`, and the mp4 is ~30 MB with ~950 frames (the recording runs at ~80 fps).
  The measure windows say `active` for the first ~20 s and `game_over` after that, so the 12 s clip is live gameplay.
- `flick2.py` counts frames where >0.2% of each eye's central lens area turns black for one frame (dark in this frame, bright in the
  frames before and after). It must decode with `-fps_mode passthrough`: the recordings declare a 90000 fps timebase, and without
  it ffmpeg pads a clip to ~1 M frames and stalls.
- `dark.py` (near-black area per frame) is useless here: the scene is dark enough that it can't tell the variants apart.
- The autoplay takes a different path each run, so a single clean run proves little.

## Headset defaults (Android only; the Mac's VR preview opts in)

| Knob | Default on the headset | Off / alternative | Where |
|---|---|---|---|
| HUD + glows in small layers (HUD layer) | on | `UC_NO_HUD_LAYER=1`: HUD drawn into the EFB | `vulkan_renderer.cpp:2215` |
| Texture table | on | `UC_TEX_MERGE=0` (also turns the ubershader off) | `vulkan_renderer.cpp:433-447` |
| Small-draw ubershader | `scene` = EFB passes only | `UC_UBER_SMALL=0`, or `1` / `hud` | same; `uber_merge.h` |
| Fragment-only uniform merge | on | `UC_MERGE_FRAG_UNIFORM=0` | `vulkan_renderer.cpp:380-390` |
| Copy clear folded into next EFB pass | on | `UC_FOLD_CLEAR=0` | `vulkan_renderer.cpp:4855` |
| EFB depth | 24-bit (X8_D24) | `UC_DEPTH_FORMAT=d32` | `vulkan_renderer.cpp:357` |
| Foveation | high | `LINDBERGH_XR_SET=foveation=off` | `xr/xr_settings.c:46` |

The design notes are in `docs/spikes/S17-quest-gpu-profile.md` and `docs/spikes/S18-dolphin-reference.md`
(section 8: the table and the ubershader).

## What was done: the six-variant bisect (`plan_bisect.sh`, one run each, build `3cea3b8`)

| Variant | Settings | Black-flicker frames (of ~950) | Contact sheet |
|---|---|---|---|
| `b-def` | defaults | 1 | 3 small black slivers near the bottom |
| `b-notable` | `UC_TEX_MERGE=0` (table + ubershader off) | 1 | clean |
| `b-nofu` | `UC_MERGE_FRAG_UNIFORM=0` | 1 | clean |
| **`b-nohud`** | **`UC_NO_HUD_LAYER=1`** | **28** | **big black holes and wedges in the floor and rug** |
| `b-depth` | `UC_DEPTH_FORMAT=d32` + `UC_FOLD_CLEAR=0` | 2 | clean |
| `b-nofov` | foveation off | 0 | clean |

**No run showed the colour rectangle**, not even the defaults. "Clean" means "not seen in one 12 s run", not "fixed".

## The lead (a hypothesis, untested)

With `UC_NO_HUD_LAYER=1`, the HUD's small draws go into the EFB passes, and the small-draw ubershader is on there by default
(`UC_UBER_SMALL` defaults to `scene`). That ubershader is what drew the garbage panel on the HUD layer before `185f7f8`. So the
`b-nohud` holes may be the same ubershader fault, now in the EFB passes, and the defaults' slivers the same fault on small scene draws.
**No run turned the ubershader off alone:** `b-notable` turned off the table and the ubershader together.

## Next steps

1. Ask the user where and when the colour rectangle shows (stage, moment, scene or HUD, how often). Point the recordings there:
   the fight scenario may never reach it.
2. Test the lead on the headset (ask first), at least 2 runs each, since one run proves little:
   - `nohud-nouber`: `--quest-set UC_NO_HUD_LAYER=1 --quest-set UC_UBER_SMALL=0` (if the holes go away, it's the ubershader)
   - `nohud-notable`: `--quest-set UC_NO_HUD_LAYER=1 --quest-set UC_TEX_MERGE=0` (the table alone)
   - `nohud` again, as the control
   - `def-nouber`: `--quest-set UC_UBER_SMALL=0` against `def`, with longer recordings (raise `--time-limit` in `rec.sh`, max 180)
     to see whether the defaults' slivers are the same fault at a lower rate.
3. If it's the ubershader: compare what the merged draw reads against the separate draws. Suspects are the texture-table index
   per draw, the per-vertex attribute fetch, and the selector tree for that draw's TEV and blend state. Also check whether the
   holes match the small draws' screen boxes (`UC_UBER_PX` threshold; try a smaller one) and test `UC_UBER_FAMILY=0`.
   The Mac VR preview (`UC_VR_PREVIEW=1` with `UC_TEX_MERGE=1 UC_UBER_SMALL=1`) may reproduce it without the headset.
   Check that first, since Mac runs don't need the user's permission. Follow `AGENTS.md` for Mac runs (harness, 1280x720 window).
4. When the cause is clear, fix it, rebuild, then run one recorded headset run of the defaults and one with `UC_NO_HUD_LAYER=1`
   to confirm. Commit on `quest3-vr`, push to `lialan`, and delete the run archives.
