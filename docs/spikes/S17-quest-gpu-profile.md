# S17: where the Quest 3's GPU time goes in the train fight (2026-11-02)

Measured: `uctest run fight --quest --god --measure 30 --quest-set UC_GPU_PACKETS=1` (build d213c6b: native eye images 2064x2208,
120 Hz, multiview, foveation off), once with the default scene scale (3.07x: each eye's scene is the eye image) and once with
`--game-arg=--render-scale --game-arg=1` (687x720 an eye). GPU ms per *presented* frame, averaged over the 30 s window.

| packet (what the frame draws) | 3.07x | 1.0x |
|---|---|---|
| main scene (copy 640x360, 200+ draws, ~160k indices) | 42.2 | 26.9 |
| overlay after the scene (no copy, 50-199 draws) | 10.9 | 1.4 |
| copy and blur chain (640x360 / 320x180 / 160x90 copies, few draws) | 23.8 | 5.4 |
| present: the screen pass into the eye images (FXAA) | 4.9 | 3.8 |
| **GPU per presented frame** | **83.7** | **37.6** |
| presented frames per second (game `[stats]`) | ~11 (9-16) | ~18 (15-33) |
| GPU busy | 88% | 67% |
| tile memory traffic (attachments loaded+stored per frame) | 295 Mpx | 37 Mpx |

What it says:
- **The harness FPS is not the headset's.** `measure_fps` reported a flat 20 at both scales: it counts the guest's frames. The
  headset shows what the GPU completes (`[stats]`, the `present` packet count): ~11 fps at 3.07x. Judge Quest runs by `[stats]`.
- **The main scene has a ~25 ms floor that does not depend on resolution.** 9.4x fewer pixels took it only from 42 to 27 ms; the
  pixel part at 3.07x is ~15 ms. The rest is per-draw / per-vertex work (vertex stage with the per-eye reprojection, Adreno's
  binning pass over ~160k indices for two views, state changes). Not yet split: `ovrgpuprofiler -t` (render-stage trace: binning vs
  rendering vs GMEM load/store) failed to attach ("Failed to start render stage tracing") with `-e` set before the app started;
  try `-e org.uc.umbrellachronicles.vr`.
- **Everything after the main scene is pixel and bandwidth cost** (35.5 -> 6.8 ms): every packet is its own render pass that loads
  and stores the full-size targets at the scene scale.
- **Runtime foveation (vr.cfg) cannot help**: it only reaches the eye images, i.e. the present (5.8%). Foveating the game's own
  targets (VK_EXT_fragment_density_map on the scene and copy passes) would reach the ~15 ms pixel part of the main scene and the
  copy chain at native scale; not the 25 ms floor.
- The GPU clock moved between runs (640 MHz at 3.07x, 545-599 MHz at 1.0x), so the 1.0x numbers are slightly pessimistic.

60 fps needs the whole frame under ~14 ms: even at 1.0x the main scene alone is 27 ms, so its fixed cost is the first target.

## Main scene (2026-11-02, branch quest-main-scene)

Method: the deterministic `fightvr` demo replay on the headset (identical guest work every run; `--deterministic --demo-play`,
`UC_GPU_PACKETS=1`), a 0.25 s render-stage trace (`ovrgpuprofiler -t 0.25 -s="1,7,8,28,29"`; it attaches when `-e <package>` is set
before the app starts and `-s` names metrics) at retrace 9060 (the heavy fight), GPU ms of the main scene packet per presented frame
over the fight window (retraces 8450-9352). Render scale 1 (673x720 an eye, 4 bins of 384x480) unless noted; GPU at 545 MHz in every
run below. Scripts: `/tmp/qms/dbench.sh`, `dsum.py`, `surf.py` (not in the repository).

### What the ~25 ms floor was

The scene packet of the replay: ~527 draws (381 cached meshes), ~106k indices, ~60k vertices, 14 pipelines.

| run (render scale 1) | main scene ms | trace: Binning / Render | Render per bin (ms / M fragments) |
|---|---|---|---|
| before (d213c6b+S17) | 32.0 | 2.7 / 22.8 | 11.1/0.60, 8.6/0.44, 1.9/0.43, 1.3/0.36 |
| `UC_PROBE_VS` (no lighting, no texgen in the vertex stage) | 13.5 | 2.1 / 7.9 | |
| `UC_PROBE_FS` (vertex colour as the fragment) | 18.9 | 2.2 / 13.4 | |
| `UC_PROBE_SETS` (one descriptor set for the pass) | 29.4 | 2.5 / 16.3-24.7 | |

- Fragments are not the cost: 1.8 M fragments a pass; the two top bins took 8-11 ms for the same fragments the bottom bins
  shade in 1-2 ms.
- The vertex stage was: without the GX lighting and texgen the packet fell from 32 to 13.5 ms. On the tiler the vertex stage runs
  in the binning pass and again in every bin a primitive touches, for both views; the game's draws are small (~200 indices) and its
  vertex stage long (8-light GX lighting for two channels, eight texgens), so each repetition is latency, not throughput.
- Descriptor sets per draw cost little (29.4 ms with one set for the whole pass).

### Fix 1: a vertex pre-pass (3c1b260)

A compute dispatch shades every vertex of the packet once (`smoke.slang xformMain`: the vertex stage's GX transform, lighting and
texgen, unchanged) into a clip-position buffer and an attribute buffer; the draws' vertex stage (`smoke.stereo.xform.vert.spv`) only
reads them back and applies the eye transform. The compute pass costs ~0.8-1 ms (trace `Dispatch`). Layered surfaces stay exact (every
pipeline reads the same position bits). `UC_NO_XFORM=1` restores the old path.

Same commit: per-draw culling against both eyes (`renderer/vr_cull.h`), TEV modules folded with `--fold-spec-const-op-composite`
(13 -> 6.6 MB, unused stages no longer behind constant branches) and texture coordinates divided by q only where read.

| fightvr replay, main scene ms a frame | scale 1 (545 MHz) | scale 3.07 (640 MHz) |
|---|---|---|
| before (`UC_NO_XFORM=1 UC_VR_NO_CULL=1`, same build) | 32.6 | 51.3 |
| pre-pass, no culling | 17.8 | |
| pre-pass + culling | 17.3 | 31.8 |
| wall time a game second (GPU bound) | 7208 -> 5483 ms | 12440 -> 10160 ms |

Render-stage trace with the pre-pass (scale 1): Binning 2.0 ms, Render 11.1-12.5 ms, instructions a fragment 262 -> 155-178.

### Culling and the game's widened view

`[vr-cull]` (UC_GPU_PACKETS): of the world draws that reach the culling test, 22% lie outside both eyes and are skipped, 4% of the
scene's indices; worth 0.5 ms at scale 1 (17.8 -> 17.3). The same line measures how far the eyes reach in the game's clip space
(`eye_extent`: the eye frustum's far corners carried back into the game's clip space): on the headset 1.09 x 1.62 of the 1.6x-widened
game view (the VR preview: 0.97 x 1.43). So 1.6x is not too large: it is too small, mostly vertically (the game's picture is 16:9, the
Quest's eye is taller than wide). The guest culls objects the top and bottom of the eyes still see; they pop in and out at those edges
(a likely cause of polygons flashing at the view's edges). Covering the eyes would take about 1.75x horizontally and 2.6x vertically
(more guest draws, so more GPU); the scale also feeds the stereo lens tally (xr/stereo_scene.cpp), so changing it per axis is a
decision for the owner, not made here. Caveat: measured with the headset lying still (its pitch counts).

### The second 200+-draw packet (`copy 320x180 draws 200+`)

Every fight frame has a second big packet: ~295 perspective draws of 6 indices each (callsite 80357A68, 80359464: heat-haze /
distortion particles that themselves sample a 320x180 EFB copy through indirect texturing), drawn over the whole 640x360 viewport,
then a 320x180 copy of the EFB's top-left quarter. Pure pixel cost: 2.0 ms at scale 1, ~10 ms at 3.07x (fightvr replay). Worth its
own look (overdraw of large translucent quads through the indirect path at native resolution); not changed here.

Note for headset A/B runs: the headset is shared and both sessions install the same package, so a `--no-install` run can measure
the other session's build (one did: `vertex pre-pass` missing from its uc.log). Every run here installs its own APK.

### Fix 2: one draw per run of draws that differ only in their vertices (0cc3557)

After the pre-pass the two top bins still took 5.4 and 4.2 ms against ~1 ms for the same fragments in the bottom bins: a fixed cost
per draw in each bin it touches. With the pre-pass, the draws' indices are rebased onto the packet's shaded positions in one index
buffer, so consecutive draws with the same pipeline key, textures, samplers, uniform block, eye block, push constants, viewport and
scissor become one `vkCmdDrawIndexed`. 55% fewer draws issued in the fight (`[vr-cull]` prints issued/merged; `UC_NO_MERGE=1` off).
What still ends a run (VR preview, `UC_MERGE_WHY=1`): a texture change 55%, a pipeline change 28%, a uniform block 7%.

| fightvr replay, main scene ms a frame | scale 1 (545 MHz) | scale 3.07 (640 MHz) |
|---|---|---|
| before (S17 build, same replay) | 32.6 | 51.3 |
| pre-pass + culling (3c1b260) | 17.3 | 31.8 |
| + merged draws (and the 30 missing folded TEV keys, 8076214) | 11.2 | 23.5 |
| same build, `UC_NO_MERGE=1` | 17.2 | |

Trace: Binning 2.0 -> 1.0 ms, Render 11.6 -> 7.9 ms, top bins 3.1/2.6 ms.

### What is left: fragments

With both fixes the fragment stage is most of the main scene: `UC_PROBE_FS` (the vertex colour as the fragment) takes it from 11.2
to 4.8 ms at scale 1, and at 3.07x the pass is fragment-bound (Render 18.4 ms for ~27 M fragments at ~150 instructions; the top
bins still carry ~1 ms of per-draw cost each). Tried and not kept:
- A fragment density map on the EFB (VK_EXT_fragment_density_map on non-subsampled images, full density in the centre, 2x2 / 4x4
  fragments towards the edges; every EFB pass with the map): main scene 23.5 -> 27.1 ms at 3.07x, frame 60.3 -> 72.8 ms. The
  fragment count did not drop (29 M) and the passes gained load/store stages; the bins became 192x992. Patch kept out of the tree.
- Anisotropic filtering off (`--anisotropy 1`, the default is 16): main scene 23.4 -> 23.0 ms at 3.07x (within noise; not worth
  the blurrier floor and ceiling).
- Relaxed precision (`UC_RELAXED_TEV=1`: the folded TEV modules' colour arithmetic marked RelaxedPrecision, texture coordinates and
  the fog's depth kept full precision; tools/spirv_relaxed_precision.py): main scene 23.4 -> 22.2 ms, the heat-haze packet 10.3 ->
  9.9 ms, frame 60.0 -> 58.6 ms at 3.07x. Small; kept opt-in until its pictures are compared on the headset.
