# Quest clarity and scene-copy distortion, 2026-11-04

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
