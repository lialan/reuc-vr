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
