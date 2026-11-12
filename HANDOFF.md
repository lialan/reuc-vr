# Latest: Application SpaceWarp — accepted in the headset; owner set to 120 Hz, render scale 2.25

Updated 2026-11-12. Branch `worktree-appsw` (from `main` f280916). Details and numbers: [S23](docs/spikes/S23-quest-spacewarp.md).

- Display page **Space warp** (vr.cfg `space_warp`, default **off**; `UC_SPACE_WARP=1` forces it for harness runs). On: `XR_FB_space_warp`
  with motion vectors and depth from a motion pass over the pre-pass's positions; the game steps on a 45 Hz timer.
- Live fight, native eyes, same APK: off 46-54/90 with 36-46 stale refreshes a second (GPU 99%); on 45/90, 0 stale, 22.2 ms frames,
  motion pass ~free after merging draws (55k world draws per 2 s in ~4k calls).
- Owner verdict (2026-11-12): real depth ghosted on head movement; vectors + flat depth are clean, now what On sends. Vectors verified
  from captures (UC_CAPTURE_WARP). At 120 Hz only render scale 2.25 holds 60 (0 stale, GPU 85%); 2.5 dips; native 41-46. See S23.
- Superseded note:
  how it looks and feels worn. The owner should try it, ideally on a moving-camera rail section, and compare with Space warp off.
- Diagnostics: `UC_SPACE_WARP_VECTORS=0` (head pose only), `UC_SPACE_WARP_FLIP_Y=1` (y negated), `[space-warp]` stats line.
- Test builds were installed as versionName `0.1.5-appsw`, versionCode 105 (equal to the owner's release so either installs over the
  other). The owner's previous APK is kept at `~/workspace/reuc-gpu-efficiency/appsw/pre-appsw-installed.apk` (SHA-256 0038f2f5...).

---

# Earlier: mesh muzzle alignment, clean reset, expanded scales verified

Updated 2026-11-04 21:29 UTC. The owner accepted audio and requested a commit.
The accepted baseline is committed as `78e6c57`; this follow-up keeps that audio
path. No push was requested or performed.

- Laser and projectile now originate at the gun mesh's barrel centre, including
  the rendered recoil translation. The TP9 muzzle is local Z -0.206 m; the old
  hard-coded -0.260 m / Y -0.006 m put the ray ahead of and below its barrel.
  Scene intersection also begins at the barrel, so geometry between the grip
  and muzzle cannot turn a close-range shot backwards. Zero-spread shots follow
  the laser; the game's original pellet spread remains intact.
- Reset requests the ordinary SDL quit path, waits for guest/file/audio/Vulkan/
  OpenXR teardown, then relaunches from the Android UI thread and exits with
  status 0. It preserves the launch intent, including isolated diagnostic saves.
  The helper no longer kills itself. The harness records changing PIDs and has
  an explicit restart grace period so it does not stop a legitimate relaunch.
- Added 2.75x, 2.5x and 2.25x to the live render-scale cycle.
- Foveation was already active in the custom Vulkan renderer. Live panel changes
  through High/Off/Low/Medium/High were verified on Quest at native 2064x2208 per
  eye. Matched Off/High captures at retrace 9060 have a byte-identical 240x240
  optical-centre RGB crop and different peripheral shading (400x400 corner crop
  PSNR 30.246594 dB). No foveation algorithm change or FPS improvement is claimed.
  Android's default remains Medium; the owner's saved High setting is preserved.

Validation:
- Ten focused CTests passed: xr_shot, input, aim_geometry, fovea_map,
  stereo_projection, late_aim, audio_startup, audio_startup_restore, game_input,
  hle_restore. The added aim cases cover all mesh models, translated/rotated
  hands, recoil, 2 cm / 5 cm targets, and a wall behind the barrel.
- Quest gameplay reset changed PID 24874 -> 25542. Log order is orderly shutdown,
  all workers joined, native shutdown complete, System.exit(0), new private game
  launch. Android exit-info records EXIT_SELF status 0, not a signal/crash.
  The new process then reached gameplay and finished the replay normally.
- The same new process applied every scale through the actual panel callback,
  including 2.75x/2.5x/2.25x, returned to native, and exercised all foveation levels.
- Close-range aiming feel with hand movement still needs owner headset feedback;
  the tests establish ray/model/collision alignment, not subjective acceptance.

Evidence: `/Users/lialan/workspace/reuc-vr-glitch/aim-reset-foveation/`.
`validation.json`, `native-off/`, `native-panel-reset/`,
`native-foveation-comparison.json` and `installed-final.json` hold the results.
The final APK is preserved as `muzzle-reset-scales.apk`, SHA256
`ca48788239e7ec0185f54d7e73901122004987a0a6db3fa73a2d963d6b568d22`.
Installed APK was read back and matches exactly. The accepted baseline APK
`b3fc5cc723e834733abcc024938e67b1cb9129aac4dc91af681e97a65f5a5724`
is retained as `accepted-baseline.apk`.

Quest released to owner: no test process/automation running; private env/args,
demo/pose files and panel command removed; Guardian/proximity automation restored.
Normal launch PID 26511 uses the owner's saved native-sized eyes, 2x render scale,
High foveation, crosshair on, and HUD pitch -30. Shared env.txt/args.txt are absent.
AAudio opened normally. PLAY_AUDIO is ignored while off-body, as in the earlier
lifecycle diagnosis; do not undo Horizon's presence policy or reopen audio work
without new owner evidence. Audio was explicitly accepted by the owner.

---

# Accepted baseline: audio good, tracked muzzle aiming mostly correct

2026-11-04 owner feedback: **"audio is good now. commit"**. Aiming is mostly
correct, with some offset in narrow spaces. Installed APK was read back again
and is `b3fc5cc723e834733abcc024938e67b1cb9129aac4dc91af681e97a65f5a5724`.
It is preserved at `reuc-vr-glitch/aim-reset-foveation/accepted-baseline.apk`.
This supersedes the provisional audio diagnosis below. The untested package
attribution experiment was removed; the accepted build uses standard SDL audio.

The pending follow-up covers near-wall muzzle/laser alignment, Reset game,
actual foveation behavior, and 2.25x/2.5x/2.75x scale choices. Device access
remains authorized and USB has returned. No new audio changes are needed.

The baseline passes nine focused CTests and the 281-shot fixed-ray Quest
replay; owner confirms audio and mostly-correct aiming. Medium is the default
foveation setting; its rendering effect still needs investigation. No FPS claim.

---

# Latest continuation: projectile ray fixed; audio wake-up diagnosis pending USB

Updated 2026-11-04, 21:07 UTC. Owner reported that the new crosshair helped but
laser and bullets still differed, and sound was still missing. Owner authorized
Quest use: **"Yes, available now."** USB disappeared during the final audio
lifecycle diagnosis; ADB and mDNS currently show no device. An async request to
reconnect USB is pending. No test is running. Do not claim audio fixed or final
candidate installed yet.

- Aiming: game projectiles used the original camera origin (`object+80`) and
  direction (`+92`), while their visible trail added a flat-screen muzzle offset
  (`+104`). `game/xr_shot.cpp` now hooks the return from RBUE08 initializer
  `80069210` only for player-shot caller LR `8006B97C`. It transforms the consumed
  tracked laser muzzle/direction through inverse camera `8044B4E0`, rotates the
  original spread cone to that ray, and initializes collision/visible/history
  positions together. Weapon speed, ammo, RNG and flags remain guest-owned.
  NativeInput freezes the ray with the consumed pointer; demos/network input
  disable live ray overrides. ControllerSnapshot takes the exact overlay
  muzzle-to-end ray, including its small local muzzle offset.
- Nine rebuilt focused CTests pass (`trajectory-audio/tests.txt`), including
  translated/rotated camera, near/far points, preserved spread, invalid input,
  consumed-pose timing, caller guard, and actual overlay/shot equality.
- Quest fixed-ray replay completed, exit 0: 281 projectile initializations,
  zero rejects, maximum normalized direction error 2e-7. Cabin and close-attack
  captures were inspected in both eyes. This uses `UC_XR_TEST_SHOT` and is not
  moving-controller acceptance or a matched laser/bullet video. No FPS claim.
- Foveation remains medium by default (generation 12) and native 2064x2208.
- **Audio diagnosis supersedes the earlier spatialization hypothesis below.**
  Initial `PLAY_AUDIO: ignore` and `mutedState:appOps` were observed. Setting
  allow and launching with the harness produced nonzero PCM and an unmuted
  speaker stream, but normal off-body launch became ignored again. A proximity
  close event made appOps allow while the existing AAudio player stayed muted.
  Thus a manual appOps change alone is NOT the fix. Normal launch logs also
  show AAudio `opPackageName=(null)` / empty package registration.
- The speculative spatialization NEVER patch was removed. A new candidate
  explicitly sets SDL application metadata to the Android applicationId and
  patches pinned SDL AAudio to call optional API-31 `setPackageName` before
  stream creation. This builds, but its wake-up behavior is **UNTESTED**.
- `trajectory-audio/lifecycle.py` reuses the repository Quest harness and
  isolated saves, starts with proximity automation disabled, then closes,
  disables and closes proximity again. The first `lifecycle-baseline` used
  `prox_open`, which did not change appOps and is **not lifecycle evidence**.
  The revised baseline-real-proximity run failed before install because USB
  disconnected. Run that revised baseline, then package-attribution candidate;
  require on-body active streams to clear `mutedState:appOps` after both cycles.
  If attribution alone fails, investigate reopening the physical audio device
  on XR visible/focused transitions, with synchronization against audio writes.
- Last installed artifact is the aim-only build (with standard SDL audio):
  `trajectory-audio/trajectory-audio.apk`, SHA256
  `b3fc5cc723e834733abcc024938e67b1cb9129aac4dc91af681e97a65f5a5724`.
  It was hash-read back in finalize.py before the later baseline reinstall.
- Pending APK: `trajectory-audio/package-attribution-candidate.apk`, SHA256
  `fe0c815abf47cfec22ff25d2ecfb72a4ca51f3fdb6c0e725c494b2b025f5960b`.
  `quest/reuc/build/outputs/apk/debug/reuc-debug.apk` is this same candidate.
- The baseline lifecycle harness stopped the app and restored proximity and
  Guardian. Its private `env.txt`/`args.txt` remain and need cleanup. The failed
  `finalize.py` already removed earlier shot/pose overrides, but its assertion
  failed on off-body appOps ignore; it did not write installed-final.json.
  Revise final verification to distinguish off-body policy from stuck on-body
  stream mute. Preserve the normal mute-when-off-body policy and user volume.
- Source is uncommitted on `quest3-vr`, base `316e2f73`. Both older APKs remain:
  crosshair build `0cc0d9ff...` and accepted cabin build `518fdfd6...`.

Evidence root: `/Users/lialan/workspace/reuc-vr-glitch/trajectory-audio/`.
Key files: `pending-audio.json`, `normal-onbody-check.json`,
`normal-onbody-audio.txt`, `normal-logcat.txt`, `projectile-check/`,
`final-standard-audio/`, `validation.json`, `tests.txt`.
All Quest operations must use quest3-lock. Push only `lialan`, never `origin`.

---

# Latest continuation: close-range aim, stereo audio, medium foveation

Updated 2026-11-04. Owner reported aim offset (especially nearby enemies), no
sound, and foveation off. They authorized another Quest diagnostic/install:
**"Yes, the Quest is available."** The candidate is now installed, normal play
is open, and the Quest is **released to the owner**. Do not restart automation
while they test. Listening and close-range controller acceptance remain pending.

- Aim previously intersected a fixed 15 m plane while the reticle was shown at
  3 m. It now intersects depth-writing scene triangles in guest view space and
  uses the same hit depth for game pointer projection, laser and stereo reticle.
  Cached mesh bounds reject misses before triangle tests; no GPU readback.
  Tracked input bypasses Wii smoothing and Wii pointer-motion knife gestures.
  Menus keep their HUD plane; a ray with no scene hit keeps the 15 m fallback.
  This uses visible mesh surfaces, not enemy hitboxes or aim snapping. Alpha
  cutout holes are not sampled by the CPU trace. Harness input stays authoritative.
- Audio was already producing nonzero PCM; device music was unmuted. The AAudio
  AUTO stream was marked spatializable by Horizon's 2D activity routing. The
  pinned SDL dependency now requests optional API-32 spatialization NEVER for
  the game's listener stereo (API-29 fallback retained). Candidate logs show
  can_spatialize=false and the main speaker mixer; actual listening is pending.
  `UC_AUDIO_SPATIALIZATION=1` restores AUTO for diagnosis.
- Android defaults now use medium foveation (2), settings generation 12.
  Generation-11 Off migrates once; an explicit Off saved afterward is retained.
  Native 2064x2208 and native render scale are preserved. The normal player's
  launch confirms native resolution, foveation 2, AAudio stereo, gain 1, paused 0.
- Android APK and host targets build. Eight focused CTests passed, including
  analytic close targets at 0.5/1/2/15 m, asymmetric eyes and translated head,
  transformed/indexed meshes, pointer mapping, stereo projection and audio.
  Six settings migration/override cases passed. `git diff --check` passes.
- Isolated Quest replay `geometry-replay` completed at retrace 8811, exit 0.
  Actual XR captures at 3400/4200/8100/8600 retain the repaired cabin floor/seats
  and close-attack surfaces in the inspected frames. 104 world-depth samples
  range 1.9332..9.7616 m. This fixed-ray replay leaves the game pointer owned by
  the harness; it is not worn-headset aim acceptance or an FPS improvement claim.
- `UC_VR_TEST_AIM=x,y,z,qx,qy,qz,qw` supplies a reproducible controller ray;
  `UC_XR_AIM_LOG=1` logs depth/reach/pointer every 30 frames. Both are absent
  from normal play. Private test env/args/demo/pose/logs/captures were cleaned.
- Preserved candidate: `aim-audio-foveation.apk`, SHA256
  `0cc0d9ff3c3ca43b28b2f6269946cd3e2e2d70219f59b5a1ac233e1e8810cfe0` (installed readback matches).
  Prior accepted cabin APK is retained as `installed-baseline.apk`, SHA256
  `518fdfd633dd85e95ecb4621b58c45bc8b5734d931da082dd823b810cf7508f2`.
- Source changes are uncommitted on `quest3-vr`, based on `316e2f73`.
  Evidence: `installed-final.json`, `validation.json`, `settings-check.json`,
  `normal-launch.log`, `aaudio-routing`, `stereo-candidate`, `geometry-replay`.
  Next: owner checks audible sound and close-range point/fire alignment. If aim
  still differs, capture their ray/target depth and compare the visible mesh to
  the game's hitbox before changing calibration. Preserve the cabin repair.

Evidence root: `/Users/lialan/workspace/reuc-vr-glitch/aim-audio-foveation/`.
Use quest3-lock for all device work. Push only to `lialan`, never `origin`.

---

# Latest continuation: cabin projection fix, owner taking over testing

Updated 2026-11-04. The owner reports **"the texture issue is fixed"** and asks
to install the latest code for manual testing. Keep native **2064x2208**. The
next reported issue is that aiming was a little off; it has not been diagnosed
or changed in this iteration. **Release the Quest after installation; do not
start further automated runs while the owner tests.**

- Captured flashing black floor polygons and seat patches in the first train
  cabin, before attack effects: `priority-fix/cabin-video-tracked/headset.mp4`.
  The short upright clip is `flashing-floor-seats.mp4` in that folder.
- Added `UC_VR_POSE_DUMP=N` and `UC_VR_TEST_POSE_FILE=<file>` (22 floats: each
  eye's quaternion xyzw, position xyz, and left/right/down/up tangents). Recording
  and replaying the actual eye transforms finally reproduces the large holes
  deterministically. The older symmetric fixed pose hid the failure.
- `priority-fix/cabin.pose` with `close.demo`, native/medium, at retraces
  3400/3800/4200/4600: `cabin-pose-baseline/sheet.jpg` shows the faults.
  Forced depth preservation, no vertex prepass, D32, and no CPU culling retain
  the holes in matched frames. Earlier unpaired live improvements were false
  leads; their speculative source changes were reverted.
- The fix retains view positions in the compute prepass and projects world
  geometry directly with the eye projection/view matrix. It avoids recovering
  view coordinates from rounded guest clip depth. HUD/copies retain their clip
  mapping. `UC_VR_CLIP_REPROJECT=1` restores the original path for diagnosis.
  `cabin-pose-direct-view` restores both saved faulty frames with no prepass;
  `cabin-pose-direct-prepass/sheet.jpg` restores all four frames on the normal
  optimized path. The same intermediate APK using the old path still showed
  the holes in `cabin-pose-nocull`; the prepass control was stopped for the
  owner's takeover request and must not be treated as a completed comparison.
- New analytic regression `test_stereo_projection` checks eye translation,
  asymmetric FOV, Vulkan conversion and pose cache invalidation, including a
  near plane small enough that float clip z loses the information needed for
  reprojection. Six focused CTests pass. No new FPS benefit is claimed.
- Latest install provenance is in `priority-fix/cabin-fixed-installed.json`,
  with preserved APK `cabin-fixed.apk`, SHA256
  `518fdfd633dd85e95ecb4621b58c45bc8b5734d931da082dd823b810cf7508f2`.
  Installed hash matches, private test overrides are absent, and the normal app
  was opened for the owner. The original `cabin-diagnostics.apk`
  and `clarity-fixed.apk` are retained. Temporary run archives and private
  device overrides are cleaned after runs; useful evidence is kept.

Evidence root: `/Users/lialan/workspace/reuc-vr-glitch/priority-fix/`.
Use quest3-lock for device work. Push only to `lialan`, never `origin`.

---

# Earlier continuation: cabin missing surfaces were unresolved

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
