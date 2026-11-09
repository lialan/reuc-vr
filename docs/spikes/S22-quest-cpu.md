# S22: the Quest's frame rate after the GPU work: CPU and pacing (2026-11-09, branch display-menu)

Starting point: the owner's play sessions after the Display page (S21 branch + display-menu) show the GPU 60-75% busy and 44-48 fps in
heavy scenes (`frames.csv`, `[stats]`). GPU 4-7 ms a frame, the guest never waited on the renderer's packet window.

## 1. CPU clock: boost changes nothing

`UC_CPU_BOOST=1` (XR_EXT_performance_settings CPU domain, boost instead of sustained high), fightvr replay, alternating pairs, identical
work (2,177,981 draws): guest 23983 / 23953 Mcycles default vs 24015 / 24432 boost; wall 33.0 / 33.2 s vs 33.2 / 34.2 s. Sustained high
already runs the cores at full speed. No change.

## 2. Where the guest thread goes (simpleperf, fightvr replay, 15 s in the fight window)

Method: `simpleperf record --app <package> --call-graph fp -f 4000` from `/data/local/tmp` (the shell user may not attach by pid; the
output must be in a shell-writable directory), symbols from the unstripped `build-android/game/libmain.so` through the NDK's
`binary_cache_builder.py`. Threads by CPU over the 15 s: guest 13.0 s, render thread 10.4 s, gx-builder 8.0 s.

Guest thread, self time: translated game code 68% (S17: 58%; the host part shrank since), GX recording (HLE GX, FIFO, import) 8.9%,
SDK math HLE (PSVEC/PSMTX/PSQUAT) 5.6%, engine HLE (`hle_Res*`) 5.5%, libc 4% (memcpy 2.3%: the game's own block copies), dispatch 1.5%.
The hottest function is 4.8% (`fn_801371B4`, a closest-point-on-triangle routine built of PSVEC calls); the rest is a long flat tail.
`uc::pacing_call_body` (20% inclusive) is only the wrapper of the step-fixed bodies, not repeated work.

## 3. Compiler: PGO and ThinLTO for the Quest

Neither build had a profile (`project/pgo/` is ignored by git and was absent) and the Quest build had ThinLTO off.

- `UC_QUEST_PGO=generate scripts/build-quest.sh` builds an instrumented app in `build-android-pgo`; `scripts/quest_pgo.sh` plays the
  fightvr replay (boot, menus, chapter 1, the fight) with it and merges `uc.profraw` into `project/pgo/uc-quest.profdata`, which every
  later `build-quest.sh` uses (`UC_QUEST_PGO=off`: none). The app's process outlives the game, so `android_main.cpp` writes the profile
  itself when the game exits (`UC_PGO_GENERATE`).
- ThinLTO is now the Quest build's default (`UC_THINLTO=OFF` turns it off).

fightvr replay, guest Mcycles over the window (alternating pairs): base 24171 / 24216, PGO 22706 / 22371 (**-6.8%**); PGO 22655 / 22346
vs PGO + ThinLTO 22207 / 22112 (**-1.5%** more). Together -8.4%. The profile was trained on the same replay; live play gains somewhat less.
APK: 54.3 MB base, 37.6 MB with PGO (cold code for size), 54.4 MB with PGO + ThinLTO.

## 4. Live play is bound by pacing, not by a saturated thread

Live train fight (`uctest run fight --quest --god --measure 30`, 90 Hz), per-thread CPU from `/proc/<pid>/task/*/stat` every 5 s: guest
58-66% of a core, render thread ~50%, gx-builder 33-38%; nothing saturated, yet 46.6 fps (frames mostly two refreshes, 21.3 ms median).
`--fight-fps-limit 0` (45.4) and `90` (46.7) change nothing: not the game's retrace pacing.

Caveats on the frame log: `t*_ms` (baton time per guest thread) and `[sched]` run ms include waits with the baton held, so they overstate
CPU (title screen: thread 4, the sound thread, 262-276 ms a second of baton but 1.16 s CPU in 15 s). Use `/proc` or simpleperf for CPU.

The guest's idle per frame (5.5 ms) is the draw-done: live play released `GXDrawDone` when the render thread had submitted the packet
(S15), so the render thread's recording of the frame's tail sat on the guest's critical path.

**Change: draw-done at build on the headset.** `request_finish` drains the builder (it has then sealed the frame's packet, which owns
everything the GPU reads, as network play already relies on) and delivers the draw-done at once, instead of at the render thread's
submit. Default on Android, `UC_DRAW_DONE_AT_BUILD=0` restores the submit, `=1` enables it elsewhere; deterministic runs are unchanged.
Same APK, live fight, alternating: off 46.9 / 47.5 fps, on **49.7 / 49.9**; worst frame 42.6 / 39.8 ms -> 25.8 / 29.4 ms; p99 gap 27.3 ->
25.0 / 24.8 ms. Both "on" runs booted and reached the fight (F4's early completion, before the builder existed, never left the boot).
In the harness's fast-forward before the fight (~470 retraces a second) the guest then idles less and the route advanced at ~2700 instead
of ~4700 retraces per 10 s in one of two runs; at the normal 60 retraces a second the guest has slack. A loading screen at normal speed
has not been timed.

## What is left

The guest's own work per frame (~14 ms average in the fight, more in its heaviest moments) plus the remaining builder tail. Next, by size:
the translated code (68%, flat: translator work such as inlining the SDK vector math at its call sites), GX recording (~9%: display lists
re-recorded every frame, S17), the render thread (~11 ms a frame recording, now off the critical path) and the gx-builder.

## 5. Collision natives (2026-11-09)

The fight window's biggest translated cluster was the game's sphere-against-triangle collision: `fn_801371B4` (Ericson's closest point on a
triangle, up to eleven SDK vector calls on stack temporaries, 4.8% self) and its caller `fn_80197AC8` (the sphere test), with their
PSQUATDotProduct / PSVECSubtract / Scale / Add calls (~3%). Both are now natives (`hle_ClosestPtTriangle`, `hle_SphereTriangleHit` in
`runtime/hle/sdk_math.cpp`) on locals with the same rounding, comparisons and dead frame bytes (the game reads stale stack slots), and the
translator keeps them partial-sync calls (`leaf_hle` in emit.cpp). The reference in `tests/test_sdk_math_hle.cpp` is the translated code
itself (generated out of the lifted functions, every call in its partial-sync form): 0 mismatches in 30000 randomised runs over all seven
regions and 5622 sphere hits; a missing frame store or the missing ps1 rounding of the f26..f31 restores fails it.

fightmac guest memory identical over 307 retraces. Quest fightvr replay (alternating, same 2,177,981 draws): guest 21834 / 21951 Mcycles
before, 21327 / 21405 after (**-2.4%**); the native still costs 2.8% of the guest thread (it runs per triangle of the query).

## 6. Where the guest thread goes now, and where live frames are lost

The earlier profiles left a quarter of the samples unnamed: simpleperf's report does not read local symbols (static `body_*` functions,
anonymous-namespace helpers). `llvm-symbolizer` on the unstripped library names them (outermost inline frame): translated code is **58%** of
the guest thread, GX recording 9.6%, NW4R draw HLEs (`hle_Res*`) 7.7%, libc 4.6% (memcpy 2.6%), SDK math 2%. The hottest function is
`fn_80378090` (NW4R material apply, 4.8% with what ThinLTO inlined into it), then the new native (2.8%), `fn_80187018` (box against
triangle, 2.75%, the sphere native inlined), `fn_803949B8` 2.2%, `fn_80045B20` (a bounding-box tree walk, 2.1%). In the replay
`hle_ResMeshDraw` reads the clock twice per mesh (`CallTimer`, `--profile` only): live play does not pay that.

Live train fight (`uctest run fight --quest`, 30 s): at the harness's default eye images (2064x2208) the GPU is 99% busy (~50 fps); at the
owner's 1603x1680 (`--quest-set LINDBERGH_XR_SET=width=1603,resolution=1680`) both d7b2e40 and this build hold 60 fps (59.7 / 60.0 mean,
GPU 81-84%). The owner's slowdown (session s2, chapter 1 room r002, t=65-70 s) is draw-bound: draws go from ~28k to ~73k a second and the
guest main thread's baton time from ~9 to ~14 ms a present (all guest threads share the baton, ~19 ms with the others). The per-draw guest
work (GX recording, the Res* HLEs, the material apply) is what is left to shrink there.
