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
