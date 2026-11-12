# S23: Application SpaceWarp on the Quest (2026-11-12, branch worktree-appsw)

Goal (owner): a higher headset frame rate without giving up native eyes (2064x2208). Decision: 90 Hz display, the game at 45 Hz, Meta
Application SpaceWarp (`XR_FB_space_warp`) synthesizes every other displayed frame.

## What is built

- **Compositor** (`renderer/xr_compositor.*`): enables `XR_FB_space_warp` when the runtime offers it; a two-layer RGBA16F motion
  swapchain and a depth swapchain (D24S8 here) at the runtime's recommended size (**420x440** on the Quest 3), both `SAMPLED` as the spec
  asks; with the Display page's **Space warp** on (vr.cfg `space_warp on|off`, default off; `UC_SPACE_WARP=0/1` forces it) each
  projection view carries an `XrCompositionLayerSpaceWarpInfoFB` (identity `appSpaceDeltaPose`, depth 0..1, nearZ/farZ in metres).
  A frame without a motion pass (menus) gets cleared images: no motion, everything at the far plane.
- **Game step** (`game/game_pacing.*`, `game/uc_main.cpp`): `pacing_with_space_warp` replaces the step of every frame limit above 30
  (and none/unlimited) with a timer step at half the display rate (`--game-hz` now accepts 31..480). A limit of 30 keeps the original
  pacing: the harness reaches the fight there (its fast-forward speeds up the retrace clock, which a timer step ignores: the first try
  sat in the opening movies) and measures at 60, which space warp turns into 45.
- **Vectors** (`renderer/shaders/xr_motion.slang`, `VulkanRenderer::record_warp_pass`): `gx::MotionTracker` now also runs on the
  headset (space warp frames only) for depth-writing perspective draws; the vertex pre-pass (`smoke.slang xformMain`) writes last
  frame's view position of a matched draw's vertices (`XformJob.motionKind/prevPalette`, binding 10, `slot.xform_previous`). After a
  packet's EFB passes, the motion pass draws its depth-writing world triangles again (the draws that get `kEffectStereoView`, not
  culled) into the motion and depth images: current and previous game-view positions through this frame's eye, so the vectors hold the
  game camera's and the actors' motion and not the head's (the runtime adds that). Draws without a match use the camera's motion
  (`view_to_prev_view`). Consecutive draws are one index range: 55k world draws per 2 s in ~4.2k draw calls.
- **Depth planes** (`renderer/space_warp.h`): the frame's first world draw's lens, its near plane raised to 10 cm. The game's world lens
  has its near plane at 1 mm (far ~175 m): a 24-bit depth image would resolve ~1.5 m at 5 m.
- **Convention**: current minus previous NDC (spec); Vulkan NDC, as `XR_EXT_frame_synthesis` states for its vectors. GL and Vulkan both
  put NDC y = -1 at the image's row 0, so the runtime sees the same direction either way. `UC_SPACE_WARP_FLIP_Y=1` negates y
  (diagnostics); `UC_SPACE_WARP_VECTORS=0` skips the motion pass (head pose only, for A/B).

## Measurements (Quest 3, live train fight, `uctest run fight --quest --god --measure 30`, native eyes, same APK, env toggles)

| | runtime (VrApi, per second) | harness active windows | app GPU (VrApi App) |
|---|---|---|---|
| space warp off | FPS **46-54/90**, Stale **36-46** (old frame re-shown), VSnc 1, GPU 99% busy | mean 50.9, p50 60, p5 40 | 15.4-18.2 ms a frame |
| space warp on, no vectors | FPS **45/90**, Stale **0**, VSnc 2 | mean 44.7, p5 40; frame time 22.2 ms, p99 23-26 | ~15.3-16.8 ms |
| space warp on, vectors (merged draws) | FPS 44-46/90, Stale 0 (a few seconds 2-10) | mean 44.2 (no vectors, same APK: 44.4) | ~16-19 ms (same as without) |

Off, the game presents 46-54 frames a second into 90 refreshes with the GPU saturated (on: 91-98% busy): ~40 refreshes a second show an old frame again, so the cadence alternates
between one and two refreshes (the judder). On, every app frame is 22.2 ms apart and the runtime synthesizes the other 45.

Cadence of what the headset shows (`screenrecord` 12 s at the fight, left eye's centre, consecutive-frame difference):

| | repeated frames | lag-1 autocorrelation of the differences |
|---|---|---|
| no vectors (head pose only) | 32.9% | -0.28 (big/small alternation) |
| vectors | 12.9% | +0.06 |
| vectors, y negated | 8.5% | +0.22 |

With the depth planes fixed (near 10 cm, the last build): 4.8% repeated frames, lag-1 +0.11. The recorded runs sit at ~40/90
because `screenrecord` itself takes GPU time (every recorded variant shows it); unrecorded runs hold 44-46.

The vectors make the synthesized frames move. This measure cannot tell the y conventions apart, and the fight's camera barely moves
(global shift ~0.05 px), so a whole-frame direction test was inconclusive: the convention rests on the reasoning above and the owner's
look in the headset.

## Not done / follow-ups

- Controller and gun overlay: zero vectors (hand motion updates at 45 Hz).
- Alpha-tested cutouts are drawn opaque in the motion pass; transparent effects get no vectors (as Meta advises).
- `MotionTracker` pairing is heuristic (TD-059, ~0.5% wrong pairs): a wrong pair gives a wrong vector for one frame.
- Without the vertex pre-pass (`UC_NO_XFORM`) there are no vectors (head pose only).
- Default stays off until the owner has worn it.
