# Quest clarity and scene-copy distortion, 2026-11-04

## Cabin floor and seat fix: recorded-pose reproduction

The owner subsequently confirmed the texture issue is fixed and requested the
latest installation for manual testing. Native 2064x2208 remains unchanged.
The owner also reports an aim offset, still pending investigation.

All paths below are relative to `/Users/lialan/workspace/reuc-vr-glitch/priority-fix/`.

1. `cabin-video-tracked/headset.mp4` and `cabin-video-tracked-novrs/headset.mp4`
   clearly show large flashing black floor/seat polygons before attack effects.
   `cabin-video-tracked/flashing-floor-seats.mp4` is the short upright example.
   This supersedes the earlier no-reproduction status below.
2. Earlier unpaired live tests of D32, forced STORE, disabled prepass, precise
   shader arithmetic and ordered dot products were inconclusive: restoring the
   old code/APK did not restore the same large faults. All speculative source
   changes from those tests were removed.
3. `UC_VR_POSE_DUMP=N` records the final renderer eye transforms;
   `UC_VR_TEST_POSE_FILE=<file>` replays 22 whitespace-separated floats (two eyes,
   each orientation xyzw / position xyz / tangents left,right,down,up). It checks
   finite values, unit quaternions and positive tangents. Like the older fixed
   pose, it changes rendering only, not the runtime's submitted tracking pose.
4. Replaying `cabin.pose` with `close.demo` gives actual XR captures at
   3400/3800/4200/4600 with clearly missing floor and seat surfaces. See
   `cabin-pose-baseline/sheet.jpg`. Medium foveation/native resolution are fixed.

| Matched comparison | Result |
| --- | --- |
| `cabin-pose-store` | Forced STORE retains the same large holes at 3400; 894 changed pixels are tiny display differences. Final-stop capture at 4200 did not complete. |
| `cabin-pose-noxform` | Disabling prepass retains holes at 3400/4200; only 1,693/1,812 pixels differ. |
| `cabin-pose-d32` | D32 retains holes at both frames; 16,340/15,591 pixels differ, mostly small edge/depth regions. |
| `cabin-pose-direct-view` | Direct view-space projection restores floor/seat surfaces at both frames, prepass disabled. |
| `cabin-pose-nocull` | Same intermediate APK, original projection, no CPU culling: holes return; only 1,019/777 pixels differ from baseline. |
| `cabin-pose-direct-prepass` | Direct projection on the normal prepass restores all four sampled frames. |
| `cabin-pose-prepass-control` | Stopped for owner takeover; not a completed validation run. |

The fix keeps both view and clip positions (32 bytes/vertex) in the prepass.
Perspective world draws use `C * eyeProjection * eyeView` on view positions;
other draw classes use their existing clip mapping. This avoids the lossy
guest-projection/inverse-projection round trip when applying eye translation.
The corrected path is the default; `UC_VR_CLIP_REPROJECT=1` restores the original
path for diagnosis. Depth formats, depth STORE policy and foveation are unchanged.
The new `test_stereo_projection` analytically checks translated asymmetric eyes,
Vulkan Y/depth conversion, pose invalidation and a tiny near plane. Six scoped
CTests pass; these supplement, rather than replace, the device image evidence.

Full-chapter acceptance and a new performance measurement are not claimed.
Normal tracked-headset acceptance is the owner's next manual test. Automated
device testing stops after the requested installation. See HANDOFF.md and
`cabin-fixed-installed.json` for latest source/install state.

## Follow-up: missing cabin surfaces remain unresolved

The owner accepts clarity but reports **missing or black patches on surfaces**.
The refraction result below is not acceptance of the cabin's overall rendering.
Native resolution is retained. Further evidence, all under `priority-fix/`:

- The normal player's `vr.cfg` was read through a temporary, read-only app-side
  export. It has native 2064x2208, native render scale, **medium foveation**, 90 Hz,
  scale 10 and screen distance 3. The normal folder has no readable config.json,
  env.txt or args.txt. This supersedes the earlier inability to inspect settings.
  The export is `player-config-check/logs/player-vr.cfg`; normal files were not
  changed. The export code was removed after use.
- A second read-only export retained the normal-play log at
  `player-log-check/logs/player-uc.log`: the session starts with foveation off,
  switches to low/medium, and reaches room r000c (the first train cabin). All
  2,713 normal installed shader/data files match the APK bytes. The 78 background
  pipeline compilation failures also occur in the isolated runs; they are not
  evidence of a normal-folder-only failure. Temporary export code was removed.
- Ordinary screenshot requests set `keep_frame_`, disabling the direct XR
  presentation path. Thus the earlier raw captures did not cover the normal
  direct EFB/HUD composite. `UC_CAPTURE_XR=1` now preserves that path and copies
  the actual two-eye swapchain output after composition, including FXAA and HUD.
  It requires native-sized RGBA8 eyes and requests transfer-source usage only
  when enabled. The copy preserves sRGB-encoded bytes rather than blitting them
  through an extra sRGB decode. This is a diagnostic, not a visual fix.
- `UC_VR_TEST_YAW=<degrees>` extends the existing fixed-pose diagnostic. Eye
  positions rotate with the head so 64 mm stereo separation remains consistent.
  Runtime projection-layer poses remain tracked; these fixed-pose captures
  still cannot establish worn-headset tracking correctness.
- The XR screen pipeline now supplies an explicit disabled depth/stencil state.
  It declares a depth attachment for the controller overlay and lacks dynamic
  depth/stencil state, so a null state violated
  `VUID-VkGraphicsPipelineCreateInfo-renderPass-09033` (checked against the local
  Khronos validusage registry). `cabin-xr-direct` versus
  `cabin-xr-depth-state` shows only changing stats digits. **This API correction
  has not been shown to fix the reported black patches.**

| Controlled check | Result and limit |
| --- | --- |
| `cabin-no-cull` | Renderer CPU culling disabled: all three captures at 7000/8500/8800 are byte-identical to `close-final`. |
| `cabin-d32` | D32 versus D24 at those checkpoints changes small character/edge regions; no missing surface restored. |
| `cabin-down` | Six checkpoints at pitch -60; no large missing patches identified. Close foreground intersects the camera in some views. |
| `cabin-xr-direct` | Actual XR output at 7000/8500/8800 inspected; no large black patches identified. |
| `cabin-medium-foveation` | Four actual XR captures with the player's medium setting: coarse edge differences, no large patches reproduced. Metrics in comparison.json. |
| `cabin-live`, `cabin-live-medium` | Two 25-second actual tracked-pose videos, foveation off/medium. Both reach active dining-car combat; no large holes identified in inspected frames. Live paths differ; neither pixel parity nor a performance A/B is claimed. |
| `cabin-side` | Native, medium foveation, yaw 60/pitch -15 at 5300/6000/7000/8500; no large holes identified. |
| `cabin-side-wide` | Guest frustum 4x4 changed camera timing and the replay path. Comparison is explicitly invalid for pixel parity; no culling fix established. |
| `cabin-early` | Actual XR output at 2200/2800 (intro) and 3400/4000/4400/4800 (first train cabin), forward pose, medium foveation. Floor, seats and walls present in sampled views. |
| `cabin-early-live` | Live timing, fixed pitch -10, native/medium, unlocked, 25 seconds of first-cabin gameplay. Thirteen actual XR captures include startup, zombies and damage effects; no large missing surfaces identified. Harness passed; no tracking or performance acceptance claim. |

The missing-patch report still needs a matching reproduction. Do not turn these
negative checks into a correctness claim or speculative default changes. The
earlier pre-task `f-nohud-3s.png` contains obvious holes, but its old profile,
unpaired live route and original refraction differ from the current build.
Useful contact sheets, comparison metrics, APK hashes and logs are retained;
redundant raw captures and per-run archives are pruned. All device operations
use quest3-lock and isolated saves. Normal player saves are not used.

Quest and host renderer builds and three focused CTests (post_effects, depth_efb,
hud_layer) pass. Current diagnostic/API-correction APK:
`c22b70bffa1780510f1b25e1e63c161106c038f4eb488d7856f0c07914420854`, preserved as
`cabin-diagnostics.apk`. `cabin-installed-final.json` records installed readback,
stopped app and absent private overrides; `cabin-cleanup*.json` records pruned
captures. The accepted clarity APK remains preserved separately.

## Report and reproduction

The owner clarified: blurry/low-resolution 3D; flickering or doubled edges remain
with the head still. Native resolution takes priority over frame rate. The owner
confirmed that the displaced, wavering carpet and curtain copies in the close
leech attack match the reported glitch.

Evidence lives in `/Users/lialan/workspace/reuc-vr-glitch/priority-fix/`.
`close.demo` SHA256: `0b43515b9f380b990ba0c994b4f46470ad201ac13cbececb45fb09ca12f68785`.
Replay uses isolated saves, deterministic time, god mode, skipped tutorials,
120 Hz game timing, unlocked rendering, and a fixed 64 mm stereo pose pitched
-30 degrees. Captures are the rendered eye images before OpenXR presentation;
fixed-pose compositor recordings are unsuitable for tracking acceptance.

## Changes

- Android defaults generation 11: 2064x2208 eye images and scene, native render
  scale, foveation off. Generation-10 performance defaults (1548x1656 eyes,
  scale 1.5, high foveation) migrate once. Other explicit settings remain.
- VR screen-space refraction overlays sample the scene without indirect
  displacement. This is a VR adaptation: it deliberately removes the original
  screen-space heat/damage refraction. It preserves the overlay masks/tint,
  perspective particles, HUD, and ordinary bloom. It is not a complete stereo
  reconstruction of the game's flat-screen refraction. Desktop output retains
  the original behavior. Selection uses draw state, not a callsite blacklist:
  orthographic, alpha-over, no depth write, an indirect first stage sampling an
  EFB copy, followed only by direct stages. `UC_VR_SCREEN_REFRACTION=1` restores
  the original displacement for diagnostics.
- Stereo indirect texture math now uses logical GX dimensions for scaled EFB
  copies. Previously GetDimensions returned enlarged GPU dimensions, changing
  offsets and wrapping with render scale. Dolphin's local source confirms the
  distinction: `TextureCacheBase::BindTextures` passes `native_width/height` to
  `PixelShaderManager::SetTexDims`. This correction alone did not remove the
  reported ghosting; it made the original refraction stronger at native scale.
  `UC_INDIRECT_PHYSICAL_SIZE=1` restores the old dimension math for diagnostics.
- The extra 64 bytes of per-draw copy dimensions participate in uniform reuse
  and fragment-merge equivalence. SPIR-V UBO offset 2112 and SSBO stride 2176
  match the C++ layout, including the vertex pre-pass and merged fragments.
- The Quest test harness now actually forwards `--god` and `--skip-tutorials`.
  Previously attaching with Game(launch=False) did not forward them, and the
  measured idle player could die after the bot stopped.
- Diagnostics: `UC_VR_TEST_POSE=1`, optional `UC_VR_TEST_PITCH=<degrees>`, and
  `UC_CAPTURE_PASS_SERIAL=<serial>` for one EFB packet capture with pass logs.
  Diagnostic settings are private to the test sandbox and removed after runs.

## Controlled evidence

| Run | Result |
| --- | --- |
| `close-native` | Native-resolution reproduction before the refraction adaptation; eight attack captures. |
| `close-nomerge` | All eight captures byte-identical to close-native. Draw merging was not the cause in this reproduction. Duplicate PNGs removed. |
| `close-no-lens` | Skipping callsite 80357A68 reduced the ghosting, but also removes particles. Diagnostic only; not the shipped fix. |
| `close-logical` | Logical texture dimensions alone leave the displaced scene copies. |
| `close-no-refraction` | Retraces 8100, 8600, 8800; the displaced outlines are removed, geometry/HUD/particles retained. Game exited 0. |
| `close-original-control` | Final APK with both diagnostic restoration flags. Retraces 8600 and 8800 are byte-identical to close-native (zero differing pixels), reproducing the defect. Duplicate PNGs removed. |

`close-final` repeated the final APK with native defaults at retrace 7000 and
8100..8800 every 100 retraces. All nine captures were inspected: the attack's
scene edges remain clear. Retraces 8100, 8600 and 8800 are byte-identical to the
first adapted run; the earlier room at 7000 is byte-identical with/without the
refraction adaptation. Game exited 0. See `close-final/sheet.jpg` and
`final-comparison.json`. Redundant earlier candidate PNGs were removed.

`before-after.jpg` compares the left eye at retrace 8800. Exact comparison
metadata is in `refraction-comparison.json` and `original-control-comparison.json`.
Every run records its APK hash in provenance.json.

Other failed leads: room HUD-layer on/off captures were byte-identical at three
matched frames. D32 changed only a few hundred pixels and did not restore the
suspected holes. Earlier live no-HUD/no-uber comparisons used different paths
and poses and do not establish causality. Do not turn those into default changes.

A diagnostic that captured 24 full-resolution passes stalled long enough to hit
the deterministic scheduler's asynchronous-result watchdog after seven PNGs.
That run (`close-passes`) is not gameplay acceptance. Three useful pass captures
and its failure log remain; the new serial diagnostic captures one packet only.

## Validation and limits

Quest and macOS renderer builds passed. Five focused CTests passed:
post_effects, uber_merge, depth_efb, fovea_map, hud_layer. Seven uctest client
checks passed. A temporary C settings fixture covered fresh defaults, v10
migration, custom settings, and explicit v11 performance settings; it was
removed after saving settings-check.json. Shader layout checks are recorded in
shader-layout.json. git diff --check passed.

Device logs confirm a 2064x2944 EFB whose scene is 2064x2208, two 2064x2208 eye
images, scale 3.067, 90 Hz display, foveation 0. Native quality is heavier: this
replay ranges roughly 20-37 rendered FPS in combat, depending on the attack.
This is a quality choice authorized by the owner, not a 60 FPS performance claim.
The replay evidence does not establish full-chapter parity or worn-headset
tracking acceptance. Normal-launch external config files could not be read
through adb/run-as (permission denied); isolated defaults and migration are
verified, but unrelated explicit user config/env overrides are not overwritten.

Temporary archives and private overrides are cleaned after each run. Duplicate
and exploratory PNGs are pruned after contact sheets/metrics; original pre-task
evidence is retained. Baseline installed APK is preserved with SHA256
`fd8f1a1d2231affed902275869f31bc7e4669419603b947f20e3f7188cf6ff92`.

Final APK (also installed/read back):
`7eeeb5c41361e8323b6c1795357d0271f1fee83c96c6529f6c2b0500cda1e859`.
Preserved as `priority-fix/clarity-fixed.apk`; verification in
`priority-fix/installed-final.json`. The app is stopped and private test
arguments, environment, demo, logs and captures are absent from the device.
