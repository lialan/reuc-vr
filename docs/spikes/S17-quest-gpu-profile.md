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

### Fix 3: destination alpha in one draw (bc66d9e)

Almost every main-scene draw has GX destination alpha; the renderer drew each such draw twice (colour, then the constant alpha alone
through a second pipeline): two pipeline binds per draw or merged run, every fragment shaded twice. A non-blending draw is now drawn
once with the constant alpha (`UC_NO_FUSED_ALPHA=1` off). Pipeline binds in the fight: 28.3k -> 7.7k per 2 s on the headset.

| fightvr replay, main scene ms a frame | scale 1 (545 MHz) | scale 3.07 (640 MHz) |
|---|---|---|
| same build, `UC_NO_FUSED_ALPHA=1` | 11.2 | 23.2 |
| destination alpha fused | 5.9 | 12.5 |
| GPU per presented frame (all packets) | 21.4 -> 16.1 | 59.9 -> 50.2 |

Also measured and not kept as a speed-up: descriptor sets shared by texture combination with the uniform and eye blocks as dynamic
offsets (8c59b8f, kept for the fewer descriptor writes): 11.1 -> 10.9 ms at scale 1, within noise. Cull and depth state static in the
pipelines (`UC_STATIC_DEPTH=1`): 23.4 ms at 3.07x either way; the trace counts no LRZ-killed pixels with or without it.

### Where the frame stands (fightvr replay, all fixes)

Main scene, before -> now: scale 1 32.6 -> 5.9 ms, scale 3.07 51.3 -> 12.5 ms. GPU per presented frame: scale 1 ~16 ms (present 4.0,
heat-haze packet 2.0, HUD/overlay ~1.7), scale 3.07 ~50 ms (main scene 12.5, HUD/overlay 10.1, heat-haze 10, copy chain, present).
At native resolution the other packets now cost as much as the main scene; all of them are pixel-bound there.

Next, by expected gain:
- The HUD/overlay packet at 3.07x (~10 ms: ~1.5 eye-screens of full-screen quads at ~210 instructions a fragment in a pass over
  the whole 640x480 EFB) and the heat-haze packet (~10 ms, ~6x overdraw of translucent quads through the indirect path).
- Per-draw cost that is left: a run still ends on a texture change (55%) or a pipeline change (28%); bindless textures (an
  index per draw into a texture array) would let more draws merge. Only ~3% of the scene's draws are free to reorder.
- `UC_RELAXED_TEV=1` (-5% on the main scene) once its pictures are compared on the headset.
- The game's widened view is short vertically for the Quest's eyes (1.62x reach): objects pop at the top and bottom edges.

## Copy and blur chain (2026-11-02, branch quest-copy-chain-b)

What the packets after the scene are (`UC_POST_DUMP=N`: every Nth presented frame, each packet's copy, pass render area and load/store
ops, and every draw's viewport, clip-space box, blend, TEV words, vertices, textures with sampler mode and folded module). Each frame of
the fight: the scene copy (640x360 -> half-size texture), then the game's post chain of 2D quads drawn with the full 640x360 viewport
into the top-left 320x180 / 160x90 corner of the EFB and copied out (bright pass, a 4-tap alpha-tested cross filter, blurs at 160x90),
the composite (scene copy + additive bloom, copied at full size), a 4-stage colour pass over that full-size copy (copied at half size
for the next frame), then the HUD packet, whose first four draws are near full-screen additive lens glows (80357A68: a 320x180 copy
times two 96x96 masks).

Why it cost what it cost (3.07x, 2064x2208 per eye, both eyes):
- **Pixels written, not tile loads/stores.** Every pass's render area was the full 640x360 region even for a 160x90 quad. Shrinking it
  to the draws' clip-space boxes (an experiment) cut `[gpu-area]` traffic 341 -> 198 Mpx/frame but no packet got faster: these 1-9-draw
  passes are bound by the fragments they shade and write. A full-screen quad (9.1 Mpx for both eyes) costs ~1.5-2.5 ms whatever it
  samples; that is the floor of the composite, the colour pass and the HUD's glows.
- **Every EFB copy built a mip chain to 1x1** (8-10 serial blits, a barrier each; gx.cpp gives every copy texture a GX mip chain),
  though the game samples none of its copies with a mip filter (sampler modes 0x00/0x90).
- **The bloom chain was shaded at the full 3.2 x 6.1 device pixels per guest pixel**, though it draws blurred 320x180 / 160x90 copies
  magnified or 1:1 in guest pixels.
- **30 of the fight's pipeline keys had no folded TEV module** (`project/pipeline_keys.quest` was referenced by the build but missing):
  they ran the driver-specialized 600 KB module, among them the cross filter, a bright-pass draw, HUD text and the scene's opaque draws.

Changes (Quest 3, 640 MHz, GPU ms per packet; frames differ run to run in which packets appear, so per-packet averages are compared):

| packet | baseline | lazy copy mips (cdef265) | + coarse shading (a8a3185) | + folded keys (8076214) |
|---|---|---|---|---|
| scene copy, 640x360 -> half, no draws | 1.0 | 0.6 | 0.5 | 0.5 |
| 320x180 blur passes (4 alpha-tested taps / 1 magnify) | 3.0 each | 2.9 | 1.45 / 0.23 | 0.65 / 0.27 |
| 319x179 bright pass | 2.1 | 2.05 | 0.8 | 0.5 |
| 160x90 blur passes | 0.20 each | 0.15 | 0.06-0.2 | 0.07-0.2 |
| composite + full-size copy (2 draws) | ~3 | ~2.6 | 2.1 | 2.1 |
| colour pass over the full-size copy (4-stage TEV, full screen, opaque) | ~3 | ~2.6 | 2.4 | 2.4 |
| HUD packet (four full-screen lens glows + HUD) | 9.9 | 8.6 | 5.1 | 5.6 |
| **copy chain + HUD per presented frame** | **26.8** | **23.4** | **13.5** | **~13** |

- **cdef265, lazy copy mips**: levels 1.. of a copy are made only before the first packet that samples it with a mip filter
  (`record_copy_mips`; `UC_EAGER_COPY_MIPS=1` restores the old way). Mac VR preview: chain 3.7 -> 2.0 ms.
- **a8a3185, coarse shading of the bloom chain** (VK_KHR_fragment_shading_rate, pipeline rate as dynamic state): a 2D draw outside the
  scene, drawn the same in both eyes (stereo class pass or fill), whose every texture is magnified onto it (EFB copies of at most 320
  wide at no more than one texel per guest pixel, other textures at two guest pixels a texel or more) is shaded at the coarsest rate
  that keeps 1.5 shading points per guest pixel on each axis: 2x4 on the Quest. The scene, the HUD, the reticle and the full-size
  copies stay 1x1. Default on the headset (`UC_NO_VRS=1` off), desktop opt-in `UC_VRS=1` (KosmicKrisp has no shading rates).
  Captures show the same glow, no blocks; the HUD stays sharp.
- **8076214, folded keys**: `project/pipeline_keys.quest` (the 40 keys a VR-preview fight recorded beyond the seed): folded modules
  291 used / 30 missing -> 321 / 0.

Tried, no gain: render area cut to the draws' clip-space boxes for post-chain passes (above); vkCmdCopyImage instead of the blit for
the 1:1 full-size copy (0.96 -> 0.94 ms). Not a no-op: the full-screen redraw of
the composite's full-size copy is the game's 4-stage colour pass (stage words D5D5C0E4..), not a restore, so it cannot be skipped.

Next steps, by size: the present (4.8 ms: an EFB -> slot blit, then the FXAA screen pass over 2 x 2064x2208; sampling the EFB directly
saves the blit, FXAA at native scale is a quality call: `UC_XR_FXAA=0` took the present 4.8 -> 4.0 ms); the full-screen passes' floor (~2 ms each: composite, colour pass, glows) only
moves with fewer pixels (a lower EFB scale for the whole post/HUD part or fragment density maps); the particle packet (copy 320x180,
50-199 perspective draws, ~5.5 ms) and the transparent effects packet belong to the scene work.

### On the merged build (134f541 + ccc8f61, 2026-11-03)

After the main-scene work (vertex pre-pass, merged draw runs) the frame is ~37.4 ms GPU at 3.07x (640 MHz; 20 fps presented, the
GPU 75-80% busy). Two more changes on this branch (ccc8f61):
- **The eyes read the EFB in place.** The present copied the displayed EFB region into the slot image and the XR screen pass sampled
  that copy. The screen pass now samples the region of the EFB itself (`xr_screen.slang` gets the shown rect; taps clamp to its texel
  centres as the copy's edge did), recorded before the clear after the present. The copy is made only for a frame a capture may read
  (`GameRenderer::keep_frame`: a screenshot request, the GX audit, `UC_SCENE_DUMP`), a held reticle-layer frame or a blanked one.
  `UC_XR_VIA_SLOT=1` restores the copy. Present 4.94 -> 4.00 ms (A/B on the merged build).
- **Translucent world sprites shade coarsely too**: perspective, blended, no depth write, no EFB copy, every texture covering two guest
  pixels a texel or more by the draw's box (the fight's smoke, dust and heat-haze quads, 64x64 sprites with ~6x overdraw). The particle
  packet (`copy 320x180`, 50-199 draws) 4.7 -> 2.8 ms; ~400 world runs coarse per 2 s. `UC_NO_VRS_WORLD=1` leaves them at 1x1.

Where the post-scene part stands (GPU ms per presented frame, run uctest-fight-20261103-031309): HUD packet 4.5 (per-draw cost of
~55 tiny HUD quads ~2.2 ms, lens glows ~1.1 ms coarse), present 4.0 (screen pass over 2 x 2064x2208 reading the EFB, FXAA 0.8 of it),
particles 2.8, colour pass 2.7, transparent effects 2.2, composite + full copy 1.3, the rest of the chain ~3.

Measured and dropped: the clear after the present costs ~0.1 ms (skipping it, a diagnostic, changed nothing); shrinking the HUD pass's
render area to the displayed rows (no gain, as for the post chain).

Per-draw cost (found on the HUD packet, handed to the scene work): ~40 us per tiny draw on the Quest; binding the pipeline once for the
whole packet (a diagnostic) saved 1.3 of the 2.2 ms, viewport/push constants/dynamic state 0.7, descriptor sets nothing.

Harness note: frame captures accumulate in the app's private folder on the headset across runs (3.8 GB here); past ~600 MB the pull
in `scripts/quest/run.py` times out and runs come back without `captures.tar`. Clear `files/.codex-xr-<id>/captures` now and then.

## CPU: host side (waits, IO, HLE) (2026-11-03, branch quest-cpu-host)

Question: the game is CPU-bound on the headset (guest thread ~31 ms a game frame). How much of that is host code (HLE, GX recording,
scheduling, audio, IO, locks, copies) and the render thread, and what of it is unnecessary? (The translated code itself is the translator
work, not this section.)

Method: the deterministic `fightvr` replay at render scale 1 (CPU-bound: identical guest work, "wall ms per game second" over retraces
8450-9352, base 3571/3573 ms, run-to-run ~0.1-1%) with `simpleperf record --call-graph fp` (4 kHz, 15 s) and `--call-graph dwarf`
(1 kHz, 8 s) inside the fight window, per-thread `/proc/<pid>/task/<tid>/{stat,schedstat,status}` 15 s apart, and the game's `[gxtime]`,
`[sched]`, `[profile]` lines. Scripts (not in the repository): `/tmp/qch/dbench.sh`, `dprof.sh`, `dstat.sh`, `ab.sh`, `detcmp.py`.

### Where the CPU goes (8dcb08f)

- The app runs on cores 3-5 (3 of the 6). In the window the guest thread is on a core ~87% of the time (runqueue wait 2%, kernel time 1%,
  ~1000 minor page faults a second), the render thread ~52%; nothing else is above 2%. The cores are not saturated (37-56% each).
- The guest holds the baton the whole window (`[sched]` #1 ~970 ms a second), but `[detbench]` counts only ~2900 ms of guest CPU per
  ~3600-4000 ms game second: `[sched]` counts the deterministic idle wait (below) as run time.
- Guest thread by code (fp profile, self time): translated code 58%, host code 27%, libc 9% (memcpy 4.1, memcmp 1.9, malloc/free ~2.5,
  mostly called by the host code), SDK math HLE 4%. Host code by entry (dwarf profile, inclusive): `hle_ResMeshDraw` 12.5% (17k calls a
  second, ~5 us each: mesh-cache and texture-cache hash lookups, copy-on-write of the 1-2 KB state/transform/uniform blocks, the shared_ptr
  reference counts of every draw), `hle_GXCallDisplayList` 6.1% (540 calls, 12k draws, 64k vertices a second, ~80 us a call: vertex
  decode, per-draw blocks), the FIFO write path ~5.5% (on the order of a million guest stores a second, by its share and cost per store, through `mmio_write` -> `FifoSink` -> `gx_fifo_write`,
  most of them display-list recording), `hle_GXBegin` 4.3%, the `_savegpr/_restgpr` helpers 4.2%, texture snapshots 2.0%, material and state
  snapshots 3.5%, `hle_GXEndDisplayList` 1.4% (mostly freeing the replaced recording), the AX mixer ~2% in live play (muted replays mix too).
- **The game records its display lists again every frame and calls each once.** A cache of decoded list geometry (tried) never hit
  (0 of 12k draws a second) and cost ~20 us per call to fill; removed. So every recorded primitive is paid for three times a frame:
  recording (FIFO path + a 5 KB snapshot per primitive), the copy at GXEndDisplayList, the import at the call.
- Render thread in the replays: `draw_bounds_ndc` 16% and `eye_extent` 3.6% are the `UC_GPU_PACKETS` diagnostics that `dbench.sh` turns on;
  `test_frame_json` 8% (the test server formatted the frame state, dozens of floats, on every main-loop pass, ~800 a second; bionic's float
  formatting takes a global lock, which is most of the render thread's `pthread_mutex_*` samples); `pipeline_for` 2.7% (a mutex and a
  `std::map` search over 280-byte keys per draw); the per-packet texture set 4.5% (hash inserts, 8 per draw).
- Waits: the guest never blocks on the packet window or the frames-in-flight limit in the replay (`[guest-blocked]` ~0 ms in the window).
  The wait that is left is the deterministic draw-done: a replay delivers `GXDrawDone` when the GPU completes the packet, live and network
  play when the render thread submits it (S15). **It is ~14% of the replay's wall time**: the same build with `UC_FINISH_AT_SUBMIT=1`
  (same guest work: 151960 draws in the last game second either way, guest CPU 2903 / 2966 ms) takes 2999 instead of 3497 ms a game
  second, and wall is then ~91% guest CPU. The replay's render thread is on that critical path, and with `UC_GPU_PACKETS` (dbench.sh)
  it spends ~24% in the diagnostics (`draw_bounds_ndc`, `eye_extent`). So the replay overstates the live frame and understates what
  render-thread savings give live; for live 60 fps the guest's CPU per frame is the number (~26 ms now).
- Thread switches: ~920 a second in live play (the audio and streaming threads), each ~19 us from notify to running (`[sched] avg wake`):
  ~1.8% of the guest's wall time goes to handing the baton between host threads. In the replay (muted) ~180 a second.
- IO: DVD reads are asynchronous on their own worker thread (one `ifstream` per read, ~700 reads and 69 MB in the whole replay); nothing in
  the fight window. Logging: no per-frame formatting on the guest thread outside `--profile` (`[gxtime]` timers are two clock reads per
  native draw call with `--profile`, which the replays use).

### Changes

| commit | what |
|---|---|
| 4231bad | display-list recording into presized buffers (memcpy instead of per-byte `push_back`), the recorded draw filled in place, replaced recordings freed on a helper thread and only overlapping ones searched; `_savegpr` family: one range check and one write-watch mark per save area; AX mixer on locals (the parameter block read once and written back once); `gx::Memory::read` fast path inline; `written_since` as a vectorisable maximum; audit flags tested inline; default `gx::Shared` without a reference count; TextureCache tries its two most recent entries before hashing; draw-queue arena chunks pooled |
| a43bf7c | condition variables notified only when a thread waits (packets ready/space, scheduler external events): a notify is a futex system call on Android |
| 51ba440 | test-server frame state built only when sent; `pipeline_for` memo of the last specialized pipeline; fewer texture-set inserts; `UC_FINISH_AT_SUBMIT` diagnostic |

Results, fightvr replay at render scale 1 (wall ms per game second, alternating runs on the headset, GPU at 545 MHz in all):

| build | runs | mean |
|---|---|---|
| 8dcb08f | 3571, 3573, 3568, 3568, 3570 | 3570 |
| 4231bad..51ba440 (v7) | 3512, 3497 | 3505 (**-1.8%**, outside the base's 0.1% spread) |
| an intermediate (v2: recording buffer, mixer, Memory::read, written_since, Shared, test server, plus the list cache) | 3595, 3567 | 3581 (+0.3%: the list cache cost what the rest saved) |

`[gxtime]` (with --profile): display-list calls 77.6 -> 75.0 us, mesh calls 5.13 -> 5.11 us. Guest thread profile after (v7, same
method): host code 26.8% (27.2), `_savegpr` family 3.1% (4.2), `hle_ResMeshDraw` self 3.6% (4.4), `gx_fifo_write` 2.8% (3.7),
`gx::Memory::read` and `hle_GXEndDisplayList` gone from the list, render thread without `test_frame_json`. Mac `sample` of the same window
(fightmac, 10 s, top-of-stack samples): `gx_fifo_write` 108 -> 90, `import_resource` 62 -> 39, `hle_ResMeshDraw` 59 -> 45, SaveGpr25/26
36 -> 17, `pipeline_for` 38 -> 15, `test_frame_json` 39 -> 7.

Live train fight on the headset (`uctest run fight --quest --god --measure 30 --fight-fps-limit 0`, render scale 1, audio on), harness
FPS over the active window, two alternating pairs: base 29.2 / 28.8, this branch 36.4 / 29.7 (p50 20 or 40). Live runs vary 7-13% and
the frame rate is quantised (20/30/40 at the game's pacing): no live gain can be claimed beyond the replay's ~2%. The render-thread part
(pipeline lookups; the test-server JSON exists only in harness runs) can matter more live than in the
replay because live play waits for the render thread's submission of the draw-done packet.

Determinism: `fightmac` replayed on the Mac with `--det-out` (guest memory hashed every retrace) by 8dcb08f and by this branch: identical
over all 9226 retraces. 198/198 tests (test_ax_audio compares the new mixer with the old one on 3000 random voices; test_sdk_regs_hle 0
mismatches in 1440 runs). Mac (`uctest bench --demo fightmac --runs 3`, rounds 2-3): 15.40/15.35 -> 15.05/15.04 s wall, guest Mcycles
8219/8197 -> 8003/8026 (-2.4%).

Tried and dropped: the decoded display-list geometry cache (above). Not done (each under ~0.3% by the profile): lazily copied palette and
EFB-copy maps in `GXCallDisplayList`, a cheaper state snapshot, an LRU without list splices in the mesh cache.

### What this means for 60 fps

- The guest's translated code (with the register helpers) is ~58% of its thread and the host code ~27% (libc 9%, SDK math 4%): with
  every host cost gone the guest would still be ~16 ms of its ~26 ms a frame at this fight. 60 fps needs the translator work and the structural host items below together.
- Display lists recorded every frame and called once: the recording (FIFO path, a 5 KB snapshot per primitive, the copy at the end) and
  the import at the call are the same work for the same bytes each frame. Recognising a re-recorded list whose bytes and recorded state
  equal the previous recording, and reusing its native draws, would remove most of `GXBegin` (recording), `gx_fifo_write` and
  `GXCallDisplayList` (~10-15% of the guest thread together).
- FIFO stores: each of them (about a million a second) goes translated store -> `mmio_write` -> virtual `FifoSink::write` -> `gx_fifo_write`. A translator-side
  direct call for stores to 0xCC008000 (or batching consecutive stores) would cut the per-store overhead.
- Per-draw state blocks: `gx::State` 1 KB, `Transforms` 1.9 KB, `DrawUniform` 1.8 KB are compared (memcmp 1.8%) and copied (part of
  memcpy 3.9%) per mesh, and each draw holds ~6-8 shared_ptrs whose reference counts both the guest (building) and the render thread
  (destroying) touch. Smaller blocks (the rows that changed) or frame-owned indices instead of shared_ptrs are the next step there.
- Guest threads on one host thread (fibres instead of a host thread each): the ~19 us baton handoff, ~920 a second live.
- The replay bench: without `UC_GPU_PACKETS` (or with the per-packet area estimate made cheap) its render thread stops inflating the
  draw-done wait; `UC_FINISH_AT_SUBMIT=1` measures the guest+render CPU path as live play has it.


## Foveation and render scale options (2026-11-03, branch quest-foveation)

The owner's ask: foveated rendering to cut the GPU's work, switchable and with a strength, and a render scale, both settable in the
headset.

### What the Quest's GPU offers

The Adreno 740 driver (`[vulkan] shading rates` line) has VK_KHR_fragment_shading_rate with pipeline, primitive and **attachment**
rates: attachment texels of exactly 8x8, layered attachments (multiview takes each eye's layer by its view index), the non-trivial
combiners (MAX), rates 1x1 1x2 2x1 2x2 4x2 4x4. The runtime's eye fields of view (tangents left/right/down/up): left eye 1.376 /
0.839 / 1.428 / 0.966 (54 / 40 / 55 / 44 degrees), the right eye mirrored: each eye's centre of projection lies 1/8 of the picture
off its middle towards the nose, and above the middle.

### The approach: a shading-rate attachment over the EFB and the eye images

`renderer/fovea_map.h` makes the map: for each 8x8 texel the angle from the eye's forward direction (through that eye's tangents onto
the shown rectangle), 1x1 inside a first angle, 2x2 to a second, 4x4 beyond and outside the shown rows. Levels (vr.cfg `foveation`):

| level | 1x1 up to | 2x2 up to | eye pixels at 1x1 / 2x2 / 4x4 (the Quest 3's fields of view, `[vr] foveation` lines) |
|---|---|---|---|
| Low | 35 deg | 50 deg | 29 / 43 / 28% |
| Medium | 30 deg | 42 deg | 20 / 28 / 52% |
| High | 25 deg | 36 deg | 13 / 18 / 69% |

In tangent space (the eye image) the periphery takes most pixels, which is why even Low coarsens 70% of them: a full-screen pass's
fragment work falls to about 41 / 30 / 22%.

- **EFB**: one map, a layer per eye, bound (`VkRenderingFragmentShadingRateAttachmentInfoKHR`) to every EFB pass while a level is on.
  Which draws take it (combiner MAX with their own pipeline rate, so the bloom chain's 2x4 stays at least 2x4): world draws of the
  eyes' scene (not a perspective picture for a small copy), full-screen fades (FILL) and 2D passes over full-size copies (composite,
  colour pass). The HUD (text stays sharp), the aim reticle and late-aimed quads keep 1x1; packets that draw a picture for a small
  copy (the bloom chain's 320x180 / 160x90 corners) keep their own rate, since what is drawn there is not what the eye sees there.
- **Eye images**: a second map of the eye image (2064x2208), bound to the compositor's screen pass (its pipeline: REPLACE); the
  overlay pipelines (guns, beams, panels) have no rate state and ignore it. `UC_NO_EYE_FOVEATION=1` leaves the screen pass at 1x1.
- Every EFB pipeline is created with `VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR` when the device has
  attachments, so the level changes live with no pipeline rebuild; level off binds no map. A map is rebuilt (device idle, ~0.1 MB
  upload) only when the level, the EFB size, the shown rectangle or the eye tangents change.
- Diagnostics: `UC_VR_FOVEATION=0..3` overrides vr.cfg (the harness), `UC_NO_FOVEATION=1` disables the attachment entirely,
  `UC_VR_FOVEA_CAP=0` lifts the cap below native (below), `UC_VR_SCALE_SWITCH=<frame>:<scale>,...` changes the render scale live as
  the panel does; `[vr] foveation level N (coarsest WxH): map ...` logs each map with its shares, `[vrs]` (UC_GPU_PACKETS) counts the
  foveated runs.

The subsampled fragment-density-map variant was not needed: the attachment route works and needs no change to how the EFB is copied or
sampled (subsampled images would have to be read through subsampled samplers by every copy and by the screen pass).

### Measured (fightvr replay, GPU ms per presented frame, all at 545 MHz)

| render scale | off | Low | Medium | High |
|---|---|---|---|---|
| native 3.07x (2064x2208 an eye) | 43.6 / 43.9 | 32.7 | 30.7 | 29.0 |
| 1.5x (1010x1080 an eye) | 19.3 | | 15.3 (14.8 uncapped) | 15.0 |
| 1.0x (673x720 an eye) | 15.2 | | | 14.1 (eye images only: present 3.7 -> 2.5) |

- The foveation build with the level off: 43.5 ms (the pipeline flag costs nothing). The final build (with the cap) repeats native
  Medium at 30.9 ms. Medium at native without the eye-image map:
  31.2 ms (the eye map takes the present 4.0 -> 3.3 ms).
- Per packet at native (off -> Low / Medium / High): main scene 13.4 -> 11.2 / 10.8 / 10.6 (the rest is its per-draw floor), HUD
  and overlay packet (lens glows, transparent effects) 9.4 -> 5.4 / 4.7 / 4.0, particles (`copy 320x180`: a full view copied at half
  size) 8.0 -> 5.3 / 4.7 / 4.3, present 4.0 -> 3.4 / 3.3 / 3.2, composite + full-size copy 2.6 -> 1.8 / 1.6 / 1.5.
- At 1.5x Medium (off -> on): main scene 7.0 -> 6.6, present 3.7 -> 2.6, particles 2.7 -> 1.9, HUD/overlay 2.8 -> 1.6.

The coarsest EFB rate is capped below native (`[vr] foveation level N (coarsest WxH)`): a shading point covers at most about 4 eye
pixels per axis, as 4x4 does at native. Uncapped, 4x4 at 1.5x spans 8 eye pixels and the replay's captures showed 8-pixel stair
steps on the train's railings at the eyes' lower edges; capped (2x2 at most at 1.5x and 2x, none at 1x and 1.25x, where an EFB
pixel already spans 2.4-3 eye pixels) the periphery is hard to tell from level off in the same captures, for 0.5 ms of the 4.5 ms
gained (`UC_VR_FOVEA_CAP=0` lifts the cap). At 1x only the eye images' screen pass is foveated.

Pictures (UC_CAPTURE_RETRACES=8700,9060,9300 in the replay; the captures are the EFB's shown region, before the screen pass): the
centre of each eye, the HUD, its text and the reticle are identical to level off at every level; at native High the lower corners
show 4x4 blocks in the train roof's texture when the capture is viewed 1:1, geometry edges stay sharp (coverage stays per pixel).
Whether the periphery is acceptable through the lenses is the owner's call; Low already takes a quarter off the frame at native.

Live switching (`UC_VR_SCALE_SWITCH=<frame>:<scale>,...`, a diagnostic that does what the panel does, with vr.cfg's keys set through
`LINDBERGH_XR_SET=foveation=high,render_scale=1.5`): 1.5x -> native -> 1x in one replay, the EFB and both maps remade, the 1x capture
after the switches identical to a run started at 1x.

### The settings panel

HOTD4's VR panel has "Resolution per eye" (on restart) and "Foveation" (Off / Low / Medium / High: the runtime's FB swapchain
foveation, which this game's build never enabled, so the row did nothing). Here:
- **Foveation** (same row, names and order as HOTD4): now this map; applied the next frame; vr.cfg `foveation off|low|medium|high`,
  default off (the panel's saved vr.cfg files say off already).
- **Render scale** (new row after "Resolution per eye"): Native (3.07x) -> 2x -> 1.5x -> 1.25x -> 1x -> Native; applied live (the
  renderer's set_render_scale remakes the EFB between two packets, no restart); vr.cfg `render_scale native|<720p multiplier>`.
  An explicit `render_scale` in config.json or `--render-scale` still decides at start (the harness uses it); otherwise vr.cfg does,
  at every renderer start.

## CPU item 1: fastmem (2026-11-03, branch cpu-fastmem)

Every guest load and store the translator could not prove to be RAM (base other than r1/r2/r13) tested `ea - 0xCC000000 < 0x02000000`
and branched to `mmio_read`/`mmio_write` on a cold path; the call kept spill code and a compare+branch at every access.

### Design

- The hardware-register window was already part of the PROT_NONE 4 GiB reservation (`guest_mem_posix.cpp`), so on macOS and Android
  arm64 (`UC_FASTMEM`, default ON; `-DUC_FASTMEM=OFF` restores the test; Windows and x64 keep the explicit test, same code as before)
  an ordinary access carries no test. One that reaches the window faults; `runtime/fastmem.cpp` (SIGSEGV/SIGBUS, installed in front
  of the crash handler by `install_crash_handlers`, SA_ONSTACK|SA_NODEFER) decodes the faulting arm64 instruction (LDR/STR of every
  size and signedness in unsigned-offset, unscaled, pre/post-index and register-offset forms, LDP/STP/LDPSW/LDNP/STNP integer and
  SIMD&FP, LD1/ST1 of one register; WZR/XZR and SP bases), performs it through `mmio_read`/`mmio_write` with the guest's byte order
  (one 1/2/4-byte access, 8 and 16 bytes as 4-byte accesses in ascending order as `rdf64`/`wrf64` and the psq slow path did), writes
  the result register(s) and the base writeback in the signal context and resumes at pc+4. Any other fault (outside the window, an
  undecoded form, an access that starts outside the window) goes to the previous handler unchanged. A C++ exception from the device
  code cannot unwind out of a signal handler: it ends the process through the terminate handler (as an uncaught one).
- The hot MMIO traffic stays an explicit call: (a) where the compiler proves the address (`__builtin_constant_p` of the test after
  inlining: `lis r3,0xCC01; stw r4,-0x8000(r3)` in the same block), and (b) where the translator's new "may hold a hardware address"
  dataflow (`mark_mmio_bases` in emit.cpp: `lis rX,0xCC00..0xCE00` through addi/addis/ori/mr/update forms, ABI-volatile registers
  dropped at calls) marks the base register: those sites are emitted as `ppc::op_*<true>` (2370 loads/stores in the game). (b) is
  needed because a loop's interrupt poll reloads the register cache from the context, so the compiler loses the constant: without it
  46 million FIFO stores of the replay faulted. The quantized psq slow path always tests (it carries the FIFO's quantized stores).
- Untested accesses are `volatile` (`guest_load`/`guest_store`, may_alias): the compiler never merges, splits, drops or reorders them,
  so a device sees exactly the guest's accesses; a store is a compiler barrier (the device code may write memory, as the call could).
- Faults are handled on every host thread (guest threads, HLE code using the accessors); `UC_FASTMEM_LOG=1` lists the emulated sites
  at exit (host pc, translated function, guest address). `[detbench]` lines now carry the game second's `mmio_reads`, `mmio_writes`,
  `fifo_writes` and `mmio_faults`.

### How often MMIO is hit

fightmac replay (Mac), per game second of the fight window: ~4.2-4.3 million FIFO writes, ~10 thousand other register writes, 0 reads;
faults: 0 in the fight window, 43 in the whole replay (one-off CP/PI/PE/SI register setup at boot, ~5 reads + 38 writes). Before (b)
the fault path took 46 M writes (the GX vertex loops in body_80284D58, body_800BA398, body_8035748C ...) and the psq slow path 665 k.

### Results

Determinism: fightmac replayed with `--det-out`, guest memory hashed every 30 retraces (UC_DET_EVERY=30, 307 records to retrace 9226):
identical to a129833. 201/201 tests (new test_fastmem: decode+emulate of every form against the assembler's encodings, writeback, edges
of the window, rejected forms, the signal path through the accessors of ppc_rt.h on this and another thread, chaining to the previous
handler; test_emit: the MMIO-base marking).

Host instructions of hot functions (static, whole function; guest instructions in brackets):

| function | Quest base | Quest fastmem | Mac base | Mac fastmem |
|---|---|---|---|---|
| body_800A4434 (308) | 7208 (23.4/guest) | 3057 (9.9) | 7412 | 3125 |
| body_80376904 (171) | 2148 (12.6) | 1177 (6.9) | 2482 | 1261 |
| body_80284D58 (GX FIFO) | 1356 | 1100 | 1624 | 1560 |
| body_800BA398 | 9613 | 7878 | 9768 | 8417 |
| fn_802647AC | 2568 | 1721 | 2709 | 1817 |

Compares in body_800A4434 224 -> 99, calls 148 -> 32 (the cold MMIO calls were out-of-line but inside the function).

Mac (`uctest bench dist/uc_base dist/uc_fm3 --demo fightmac --runs 3`): guest Mcycles 7444/7443/7663 -> 7020/7043/7087 (**-6.2%**),
wall -2.8% (CI [-6.9%, +1.4%], inconclusive at 3 rounds; the Mac replay is partly GPU-bound).
