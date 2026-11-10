# S20: indirect TEV specialization and redundant GPU work

2026-11-04, based on `quest3-vr` at `7011f82`. Objective: reduce GPU work toward
native-resolution 60 FPS, with generic GX techniques usable by other games.
The owner authorized Quest use and then requested fewer repeated benchmarks.
Further work should reuse these captures/profiles, batch related improvements,
and run a discriminating correctness/performance check when it changes a decision.

## Implemented

Indirect-texture shader choices now join the existing build-time TEV specialization.
The complete 21-bit IND_CMD and its selected six-bit IREF pair occupy unused bits
of the existing 70-word key. Matrix values and coordinate scales remain uniforms;
unused IREF pairs do not create variants. Reserved command bits retain the dynamic
path. This removes dynamic texture/coordinate selection, image-size queries and
command branches from known shaders without changing their arithmetic.

Quest enables this by default; `UC_SPECIALIZE_INDIRECT=0` restores the old key
selection. Other platforms remain opt-in. Missing specialized modules fall back
to existing uniform-driven folded modules, then the existing shader path. The
profile includes 12 learned keys (10 distinct new shader identities); both ordinary
and texture-table modules are built. There are no new game-address classifiers.

Specialization uses the **effective** indirect state after the existing VR
screen-refraction adaptation. `effective_indirect_command` is shared with uniform
upload so the key cannot silently restore a distortion that the adaptation removed.

Reference reviewed locally: Dolphin `df4bd441dec5d6f3cff606eea76f183963e97a44`,
`Source/Core/VideoCommon/PixelShaderGen.cpp` (indirect commands/IREF in shader identity
and generated stage logic). No Dolphin implementation was copied.

## Evidence

Artifacts: `/Users/lialan/workspace/reuc-gpu-efficiency/`, especially
`validation.json`, `installed-final.json`, `shader-cost.json`, and per-run folders.
Quest 3 `2G97C5ZJ0G02DQ`; deterministic `fightvr.demo`, fixed head/controller pose,
isolated saves, native 2064x2208 per eye and native scene scale, Medium foveation.
CSV window: guest retraces 8450–9350, excluding 12 retraces around captures at
8700/9060/9300; 1653 selected samples per run. GPU sum is completed packet GPU time
per present, not compositor timing or an assertion of live FPS.

| Runs | Mean GPU off / on | Observed saving |
|---|---:|---:|
| `indirect-off-1` / `effective-on-1` | 21.050 / 20.657 ms | 0.393 ms (1.87%) |
| `pair-1-off` / `pair-1-on`, same APK | 21.406 / 20.648 ms | 0.758 ms (3.54%) |

The latter pair's GPU p95 was 25.143 / 23.661 ms; mean presentation interval was
34.360 / 33.170 ms. These are exploratory results, with device drift and no
statistical confidence claim. The remaining two scheduled pairs were cancelled
at the owner's request. Native 60 FPS has **not** been reached.

All three captures in the latter pair match exactly: zero differing pixels out
of 9,114,624 per capture (packet serials 53317, 67016, 76215). Earlier corrected
capture 9060 also matches the baseline. This establishes parity at those recorded
views, not all scenes or tracked gameplay.

Static shader inspection: the 10 new identities have 683–726 SPIR-V instructions
versus 1022–1063 in their dynamic fallbacks; static sample sites fall from 9/10 to
2/3, size-query sites from 9 to 2. These are compiled-code counts, not actual
executed samples per pixel. All 1334 folded modules pass `spirv-val` for Vulkan 1.3.
Six focused CTests passed (pipeline keys, folding, post effects, material state,
merge eligibility, stereo projection). Android build and `git diff --check` pass.

## Failures retained

- The first candidate specialized raw GX state before the VR adaptation and
  reintroduced screen refraction. Its changed images invalidate its timing;
  `indirect-on-1` is a failed experiment, not an accepted build. The shared
  effective-state helper and regression case fix this.
- An attachment-only empty-pass skip found zero eligible passes in this replay.
  It was removed from source; no saving is claimed. The matched-pair APK contained
  this experiment disabled (`UC_SKIP_EMPTY_PASSES=0`).

## First installed artifact

First final APK: `indirect-final.apk`, SHA256
`7a18ae9e12ca7d04b11752b9b0e8ffbbcac30648065ca81b07b722ec708cae8b`.
Installed package readback matches. Relative to the tested APK
`98d9c3a98cbfd7894d16fd64233b251ecdffb3032e8ff87b5cfea1a08f66029a`, final code
enables the tested path by default and removes the disabled empty-pass experiment;
all 2751 packaged SPIR-V assets are byte-identical. The final binary was built and
installed without another replay, per the owner's instruction to avoid redundant
tests. The accepted starting APK is retained as `baseline.apk`, SHA256
`ca48788239e7ec0185f54d7e73901122004987a0a6db3fa73a2d963d6b568d22`.

No test process remains. Private env/args/demo/pose files are absent; Guardian
pause is 0 and proximity automation is disabled. Normal settings and saves were
not changed. Work is uncommitted; nothing was pushed.

Reuse the packet profile to prioritize the main scene (~7.2 ms), particle copy
packets (~3.5 ms), and presentation (~2.9 ms). Those are approximate tail-window
diagnostics, not additive measurements over the exact CSV window. Examine
unnecessary full-resolution passes/copies and synchronization before spending
more time tuning this small specialization gain. Do not repeat the empty-pass
experiment without a new scene showing eligible passes.

## Follow-up: attachment dependencies and presentation shader

The next batch narrows same-layout depth reuse to early/late depth tests and
depth writes, retaining a broad dependency for GENERAL-layout scene upscaler
outputs. Existing transfer and sampling paths explicitly transition back before
attachment reuse. `UC_BROAD_DEPTH_BARRIER=1` retains the previous broad source
scope. Dolphin's `VideoBackends/Vulkan/VKTexture.cpp::PrepareForRenderPass`
provides the same attachment-stage dependency model; no code was copied.

Direct XR presentation now omits both transitions of the unused slot image and
does not mark that image initialized. Slot copies, captures, held frames and
desktop presentation retain their preparation. XR picture transitions use the
actual colour/transfer producers and fragment/transfer consumers, keeping the
conservative fallback for other layouts.

FXAA's nine taps now share one image-size query and rectangle bounds. The taps,
filtering, edge clamp and colour arithmetic are unchanged. Generated SPIR-V
shrinks from 826 to 678 instructions and 10 image-size queries to 1. Running
`spirv-opt -O` on both still leaves 655 versus 569 instructions and 10 versus 1
queries. Both modules validate; this is static code evidence, not driver ISA or
a measured frame-time saving.

Only two continuation runs were made, with existing controls reused:

| Run | Mean GPU / p95 | Evidence |
|---|---:|---|
| `sync-on-1` | 20.695 / 23.728 ms | All three EFB captures byte-identical to `pair-1-on` |
| `sync-fxaa-xr` | 20.618 / 23.563 ms | Final build; actual XR eye-array captures at all three retraces, stereo scene/HUD visible |

Both exited normally. The final mean presentation interval was 33.047 ms.
These timings are within the prior range; no incremental speedup is established.
XR captures include the compositor, HUD and diagnostic overlay and are not
pixel comparisons against the earlier pre-compositor EFB captures. Capture
windows remain excluded from timing. Android builds, host renderer compilation,
changed-shader validation and diff checks pass; unchanged CTests were not rerun.

**Previously installed:** `sync-fxaa.apk`, SHA256
`39f0e3b1ea49c2d4cbaecd38b470795a6cd066254f8689749c20b50c03af87e6`, matched by
installed readback. Evidence: `sync-validation.json`, `xr-shader-cost.json`,
`sync-fxaa-xr/summary.json`, `sync-fxaa-installed.json` in the artifact directory.
Private overrides are absent, app stopped, Guardian pause 0, proximity automation
disabled. Earlier APKs remain available. No commit or push.

Do not spend further replay rounds resolving these small changes. Focus on
full-resolution scene/particle shading and copies. An uninvestigated possibility
is using runtime visibility masks to avoid final-pass work outside the lenses;
runtime extension availability and benefit are not yet established, and masking
the guest EFB needs care because later effects can sample outside the visible area.


## Rejected: early transparent-subgroup rejection

A narrowly classified TEV chain could compute its final alpha mask before its
indirect scene-texture lookup. The experimental subgroup-vote shader discarded
only entirely transparent subgroups and reused the sampled mask on surviving
ones. It required alpha-over/additive blending with no depth or forced-alpha
writes, preserved the actual TEV alpha arithmetic, and was opt-in. Nine learned
key variants generated 16 unique modules. Classifier/folding tests, Android build
and new-module SPIR-V validation passed.

One Quest candidate run (`alpha-guard-on`, reusing `sync-on-1` as control) exited
normally, reported the guard enabled, and produced three pixel-identical EFB
captures. Mean GPU time was **20.682 ms**, p95 23.679 ms, within the existing
20.618–20.695 ms range. No speedup is established. The experiment was removed
from runtime, key seeds and build rules; do not repeat it without new evidence
of profitable fully transparent subgroups. Its patch, APK and results remain in
`alpha-guard-experiment.patch`, `alpha-guard.apk`, `alpha-validation.json`.
The subgroup/derivative portability question was not resolved because the path
was rejected on cost; the saved patch is not an accepted implementation.
The prior `sync-fxaa.apk` was restored and its installed hash/cleanup verified.


## Follow-up: specialize vertex coordinate outputs

The pre-pass draw vertex shader previously traversed eight coordinate slots,
including packed-offset bookkeeping, for every vertex and each tile. A new
vertex-only specialization constant selects the coordinates the keyed TEV shader
can consume. Direct stages use their order coordinate; specialized indirect
stages also include their selected IREF coordinate. Unknown indirect stages and
ubershaders retain all eight outputs. No new pipeline-key fields or profile
variants are needed: the mask derives from the existing material key.

Packed input layout is unchanged. Each output's address is computed from the
**runtime** packed mask, so unused earlier coordinates and the VR adaptation's
extra raw indirect inputs cannot shift a live coordinate or texture-table index.
The small-draw ubershader retains its existing loop and per-vertex selection.
`UC_DYNAMIC_XFORM_COORDS=1` selects all coordinates for diagnosis.

Slang's `[unroll]` emitted a decorated SPIR-V loop rather than expanding it.
Explicit constant-index calls allow specialization to remove unused coordinate
loads and selection. With mask 1, optimized plain/table vertex modules have
283/295 instructions and no loop, versus 300/308 and one eight-iteration loop in
the old modules. Mask 3 has 309/321 static instructions and no loop; these static
counts do not compare dynamic work or establish GPU savings. Original and
specialized masks 1/3/255 pass `spirv-val`; the focused key/folding CTests pass.
The Android build completed. Only the two pre-pass draw vertex modules changed
among all 2751 packaged SPIR-V assets; the fragments and presentation shaders
are byte-identical to `sync-fxaa.apk`.


One native/Medium candidate replay (`xform-coords-on`, no new control run) exited
normally and matched all three `sync-on-1` EFB captures exactly. Mean GPU was
**20.462 ms**, p95 **23.412 ms**, mean presentation interval **33.006 ms**.
Recent controls were 20.618–20.695 ms: this small exploratory reduction is not a
statistically established speedup. Tail-window scene cost was ~6.99 ms versus
~7.23 ms previously; the different sampling windows preclude treating that as an
isolated measurement. No repeat was scheduled. Native 60 FPS remains open.

**Previous installed artifact:** `xform-coords.apk`, SHA256
`00446eded75c7cc28506b8016f57735b80c40f8194b3751eca55262d55b87556`.
The specialization is active by default; no diagnostic override is required.
Evidence: `xform-validation.json`, `xform-comparison.json`,
`xform-shader-cost.json`, `xform-shader-manifest.json`, and
`xform-coords-installed.json`. Prior accepted APKs are preserved.

Next work should target the remaining scene geometry/tile cost (~7 ms), particle
overdraw (~3.5 ms), and full-resolution post/presentation work. Avoid redoing
transparent-subgroup rejection or the empty-pass probe without new qualifying
workload evidence. Runtime visibility masking, copy lifetime analysis, and
material-specific vertex work are still candidates, not demonstrated savings.


## Render-stage trace and reticle attachment bounds

A single 0.5-second `ovrgpuprofiler` trace of `xform-coords.apk` started after
retrace 9000, with 17 accepted metrics. It contains 246 surface executions.
Artifacts: `xform-current-profile/render-stage-trace.txt`, the parsed JSON and
`trace-summary.json`. Detailed profiling was disabled afterwards and queried to
confirm it. Do not use this instrumented run as another performance control:
profiling changes timings, and counters during preemption can include other work.
Surface categories are inferred from dimensions/order/phase patterns.

The trace shows substantial colour/depth attachment traffic. The recurring
2064x2944 HUD/effects surface loads and stores the full target although its main
viewport is 2064x2208. Main-scene LRZ kills are nonzero in this trace, superseding
the older zero-LRZ observation for this build; that is not a reason to rerun old
LRZ experiments. The main compute pre-pass remains a candidate (~0.87 ms median
instrumented Dispatch), but its timing is not comparable to uninstrumented GPU
frame measurements. Particle work remains mostly fragment shading plus attachment
loads/stores. The next priority is avoiding unnecessary tile traffic/pass breaks.

The renderer previously expanded the attachment render area to the entire EFB
for every reticle/late-aim draw. Triangle clipping still bounds those draws by
their raster viewport. Attachment bounds now share the draw's reticle-scaled
viewport calculation; a late-aim draw keeps its whole viewport because its
scissor may be reset. Line expansion retains the old full-target bound. Clear
rectangles, first use, held-frame rendering and `UC_FULL_RENDER_AREA` keep their
existing conservative handling. `UC_FULL_RETICLE_AREA=1` restores the old reticle
bound for diagnosis. This is viewport-state based, with no game callsite filter.

The Android build and existing late-aim/render-scale CTests pass. All 2751 shader
assets are byte-identical to `xform-coords.apk`. The candidate log confirms the
HUD packet's render area is now **2064x2209**, down from **2064x2944** (735 fewer
rows, 24.97% less logical attachment area). The extra row conservatively covers
rounding at the viewport edge. This reduces the colour load/store region while
preserving attachment contents outside it. Device capture/timing results follow.


Candidate `reticle-area-on` exited normally. All three EFB captures match
`xform-coords-on` exactly. Mean GPU **20.474 ms**, p95 **23.380 ms**, mean
presentation interval **32.911 ms**; previous candidate 20.462 ms. No frame-time
improvement is established. The smaller logical attachment region is verified,
but neither a 25% bandwidth reduction nor a GPU speedup is claimed; the driver
may already avoid moving untouched/compressed tiles. Keep the simpler bounds,
and do not spend more runs on this comparison.

**Latest installed artifact:** `reticle-area.apk`, SHA256
`1ae167eccd944e9478211835175eb12d4c80eee96ce4a6594c54dc19adb9811f`.
Installed readback matches; the app is stopped, private overrides are removed,
Guardian pause is 0 and proximity automation is disabled. Evidence is in
`reticle-area-validation.json` and `reticle-area-installed.json`. No commit/push.
Native 60 FPS remains open. The next substantial opportunities need less actual
fragment work or fewer EFB pass boundaries, rather than more small area/barrier
comparisons. Existing broader merge thresholds already lost performance (S18).

## Affine texture-coordinate specialization

Dolphin's `PixelShaderGen.cpp:211-216` includes texgen projection in shader
identity. Our fragment path still divided S/T by interpolated Q for regular ST
coordinates, although the vertex texgen forces Q=1. The new generic state proof
checks only consumed coordinates: regular texgen, no post normalization, and a
post-matrix Q row that preserves the forced one or supplies a constant one.
Projected/varying Q keeps the original shader path. Unknown indirect state
conservatively checks all eight coordinates; known indirect state includes its
selected IREF coordinate. No new game callsite classifier or approximation.

Stage-zero order bit 24 records this whole-shader property, including zero-stage
texture-zero draws. Folded headset shaders get an affine variant; absent modules
fall back to the general-coordinate variant before the existing indirect
fallbacks. The small-draw ubershader retains its existing divide because its TEV
state varies per draw. Android defaults on, other platforms remain opt-in;
`UC_AFFINE_TEXCOORDS=0` selects general coordinates. Vertex data/layout is unchanged.

The two focused key/state tests and 3939 SPIR-V validations pass. There are 1964 folded module identities,
up from 1334; regular and relaxed variants are packaged. Across 73 affected
profile keys, static SPIR-V removes 1-3 `OpFDiv` operations and 5-15 instructions
per shader (including the Q guard). These are IR counts, not dynamic GPU or ISA
counts. The packaged folded data grows by about 12.3 MB uncompressed / 4.5 MB
compressed across both precision variants. The candidate warmed 2135 pipelines
in 31.4 seconds; cache state prevents a startup speed comparison. Do not expand
the variant space further without a worthwhile benefit.

One replay, `affine-coords-on`, completed normally at native 2064x2208 per eye,
native scene scale and Medium foveation. It reused the stored `reticle-area-on`
control; there was no new control run or repeated A/B pair. All three combat
captures are pixel-identical (9,114,624 pixels each). In the same 1653-frame
measurement window, mean GPU is **20.326 ms**, p95 **23.374 ms**, and mean
presentation interval **32.977 ms**. Control: 20.474 / 23.380 / 32.911 ms.
The observed 0.148 ms reduction is small and exploratory; pacing is unchanged.
Retain the removal of redundant work, but do not claim a proven speedup or spend
more device runs resolving this small difference. Native 60 FPS remains open.

**Current installed artifact:** `affine-coords.apk`, SHA256
`01ea6f7ea3255241489ddde0b2fd9afb8a0ecb44b73db7b3b6cace5a1d8ef52d`.
Readback matches; app stopped, private overrides removed, Guardian pause 0,
proximity automation disabled. The previous `reticle-area.apk` is preserved.
Artifacts: `affine-validation.json`, `affine-static.json`, `affine-captures.json`,
`affine-coords-installed.json`, and `affine-coordinates-experiment.patch`.

## Visibility-mask capability check

The Quest advertises `XR_KHR_visibility_mask`. A short probe requested OpenXR
1.0.40, matching [Meta's documented API gate](https://developers.meta.com/vr/downloads/package/oculus-openxr-mobile-sdk/).
The runtime returned success but zero vertices/indices for both eyes. A second
short probe checked visible triangles, hidden triangles and the outline before
session start and again in `XR_SESSION_STATE_FOCUSED`; all counts remained zero.
The [OpenXR contract](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrGetVisibilityMaskKHR.html)
allows this when a mask is unavailable. No synthetic lens shape was introduced.

Probe code was removed, and the accepted affine APK was restored and read back
before the next candidate. Keep `visibility-validation.json`, both probe APKs,
and `visibility-probe-experiment.patch`; do not repeat this capability check
without a relevant runtime change. This rules out a supplied visibility mesh
on the tested device/runtime, not on other headsets.

## Uniform image dimensions and zero-direction FXAA

The binding path now fills the existing size block for every map an effective
indirect stage can read: its destination texture and IREF-selected texture.
Scaled EFB copies retain their guest dimensions. Other maps take the dimensions
of the actual resolved image, including aliases and blank fallbacks; the physical
copy-size diagnostic still works. Ordinary direct textures retain their previous
uniform identity, so unrelated image sizes do not split mergeable draws.
Indirect draws with different size inputs are already distinguished by that block.
Stereo `imageSize()` can therefore read uniforms unconditionally. The non-stereo
query path is unchanged. No new shader variants or uniform-layout changes.

Dolphin reference: local `PixelShaderManager.cpp:267` and
`TextureCacheBase.cpp:1203` supply native texture dimensions through constants.
No implementation was copied. In 40 affected recorded shader modules, this
removes either 2 queries / 65 SPIR-V instructions or 9 queries / 121-122
instructions. Static IR reductions are not GPU timing claims. The folded module
count stays 1964; regular folded data falls from 25,632 to 24,072 KiB.

FXAA now returns its center sample when the computed direction is exactly zero.
All four remaining taps would use the same coordinate, and both weighted
candidates equal that sample. This adds no contrast threshold. Explicit LOD
sampling makes the branch independent of derivatives; HUD composition still
runs after it. SPIR-V retains the branch before those four samples.

One combined replay, `dimensions-fxaa-xr`, uses the stored `sync-fxaa-xr`
reference and captures the actual XR eye images. It ran at native 2064x2208 per
eye, native scene scale, Medium foveation and the existing 90 Hz setting. The
three 4128x2208 stereo images differ in 752, 789 and 928 pixels respectively,
all confined to the changing CPU/GPU/memory-counter text (`xr/host_stats.c`).
Difference crops were inspected; every pixel outside those text regions is
identical. No extra control replay or repeated performance pair was run.

The same 1653-frame window averages **20.167 ms GPU**, p95 **23.117 ms**, with
**32.832 ms** presentation interval. Prior affine candidate: 20.326 / 23.374 /
32.977 ms. This is a small exploratory improvement, not an established per-change
speedup. The two changes were deliberately tested together. `test_post_effects`
and 3941 SPIR-V validations pass. Native 60 FPS remains open.

The current 90 Hz setting is intentional: `xr_settings.c` records better live
pacing than 120 Hz for an older, lower-resolution configuration. It is not by
itself a native 60 FPS blocker. Final acceptance must use live submission
behavior; deterministic replay's GPU-completion waits are not live FPS evidence.

**Latest installed artifact:** `dimensions-fxaa.apk`, SHA256
`1fa3e85c9702f1616a1e935f3aba0330ea7445e99a3e9159fadadd28e7ef65f0`.
Readback matches; app stopped, private overrides removed, Guardian pause 0,
proximity automation disabled. Previous accepted APKs remain preserved.
Evidence: `dimensions-validation.json`, `dimensions-static.json`,
`dimensions-xr-captures.json`, `dimensions-diff-locations.json`,
`dimensions-fxaa-installed.json`, and `dimensions-fxaa-experiment.patch`.
