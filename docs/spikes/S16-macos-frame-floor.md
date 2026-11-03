# S16: what the ~10 ms frame floor on macOS is made of (2026-11-02)

Question: the Mac (M4 Pro, KosmicKrisp, 2560x1440 swapchain, 1280x720 window) does not go below ~10-11 ms per frame in the heavy train fight. What fills those milliseconds?

Measured: `uctest run fight --measure 30` at speed 1 (idle god-mode player), with `UC_FRAME_LOG` (per-frame CSV), `UC_GPU_PACKETS` (GPU time per packet kind), a 10 s `sample <pid>` of every thread, and a second 15 s run with a `DYLD_INSERT_LIBRARIES` probe that timed every `vkGetFenceStatus` / `vkWaitForFences` call.

## Result: the floor is guest work + the guest waiting for the render thread

Present interval in the fight: p10 10.8 ms, p50 11.5 ms (~87 FPS). Per ~11.5 ms frame:

| Guest main thread (`hle_GameLoop`, one host thread) | ms | share |
|---|---|---|
| translated game code | ~4.6 | 40% |
| GX recording (`hle_ResMeshDraw`, `gx_fifo_write`, `import_resource`, vertex decode, texture snapshots) | ~2.0 | 17% |
| other HLE (PSMTX math, `SegmentTriangleHit`, `HitBufferConstruct`) | ~0.7 | 6% |
| **blocked in `GXWaitDrawDone`** | **~3.6** | **31%** |

| Render thread (the main thread) | ms | share |
|---|---|---|
| real work: recording ~13 packets (Metal encoder creation in `record_texture_copy` ~15%, `draw_bounds_ndc` ~4%, `pipeline_for`, draws) | ~5.4 | 47% |
| **`vkGetFenceStatus` polling** | **~4.1** | **36%** |
| `vkWaitForFences` (waiting for a free slot / for work) | ~1.5 | 13.5% |

| GPU | |
|---|---|
| busy per frame (sum of the ~13 submissions' timestamps) | ~8.7 ms (~80% of the frame) |
| main scene pass (`copy 640x360 draws 200+`) | ~2.0 ms |
| 6 x `copy 160x90` (blur chain) | ~1.3 ms |
| `copy 320x180 draws 50-199` | ~1.2 ms |
| fixed cost of a packet with no draws | 0.2-0.4 ms each |

## Why the guest waits

In real-time play `GXWaitDrawDone` ends when the render thread **submits** the draw-done packet (`finish_at_submit`, S15), not when the GPU finishes it. The 3.6 ms are the guest waiting for the render thread to get through the frame's packets.

## Why the render thread is slow: fence-status checks are expensive on KosmicKrisp

- `vkGetFenceStatus` is a kernel call on KosmicKrisp (`kk_timeline_wait` -> `-[IOSurfaceSharedEvent waitUntilSignaledValue:timeoutMS:0]` -> `iokit_user_client_trap`). It never blocks, but it costs ~6 us on an idle GPU and ~10 us under load (standalone probe: 5.7 us unsignalled, 9.5 us right after a submit, 0 us once signalled).
- `VulkanRenderer::collect()` called it for **every** submitted, unreported slot (16 slots, ~11 in flight), and `collect()` runs on every main-loop pass (`poll_completions`) and inside every `render()` call.
- In-game probe: ~35,000 calls per second (~400 per frame), 10.3 us average, only ~10% found the fence signalled: ~365 ms per second of render-thread time, ~4 ms per frame.

So the render thread spends ~4 ms per frame asking "done yet?", and that time is on the guest's critical path (the guest waits for the draw-done packet to be submitted) instead of overlapping with the GPU's ~8.7 ms.

## What this means for optimisation (order)

1. Remove the fence-polling cost so the render thread's work overlaps the GPU (expected: most of the ~4 ms comes back to the render thread, the guest wait shrinks).
2. Then the GPU (~8.7 ms per frame) is the limit (~115 FPS): per-packet fixed cost, the blur chain, the main scene pass.
3. Guest CPU (~7.3 ms) is the floor after that.

## Fix 1: `collect()` polls the oldest fence only (2026-11-02)

`collect()` walks the ring from `next_slot_` (the oldest submission) and stops at the first unfinished fence: one queue, submissions in ring order, so a later fence cannot be reported earlier than it would have been anyway.

Live fight, 18 s at speed 1, two runs each, alternating (after, before, after, before):

| | before | after |
|---|---|---|
| present interval mean | 11.43 / 11.27 ms (87.5 / 88.7 FPS) | 10.17 / 10.48 ms (98.4 / 95.4 FPS) |
| interval p10 | 10.76 / 10.59 ms | 9.31 / 9.60 ms |
| `vkGetFenceStatus` calls per second | 34,931 / 34,649 | 9,226 / 9,227 |
| time in `vkGetFenceStatus` per second | 367 ms | 77 / 76 ms |
| `vkWaitForFences` per second (render thread idle on the GPU) | 102 / 104 ms | 323 / 331 ms |
| render thread CPU per frame (`cpu_mcycles`, p50) | 6.8 / 6.7 ms | 6.2 / 6.4 ms |
| guest idle per frame (`idle_ms`, p50) | 4.4 / 4.1 ms | 3.1 / 3.1 ms |
| render() refused for a busy slot (`[profile] retries`) | 0-4 per second | ~250 per second |
| GPU per frame (`gpu_sum_ms`, p50) | 8.3 ms | 7.4 / 7.8 ms |

The deterministic replay (`bench --demo fightmac --runs 3`, recorded on the Mac) gives -0.7%, CI [-1.6%, +0.1%]: in deterministic runs the draw-done ends at GPU completion, so fence polling is not on its critical path. Live play is where the fix matters.

After the fix the render thread has slack (it waits for the GPU three times as long) and every slot is busy (~250 refused renders per second, up to 17 packets outstanding): **the GPU is the limit now**. The guest's remaining ~3 ms wait in `GXWaitDrawDone` is the draw-done packet waiting for a free slot. With a full queue the GPU timestamps add up to only ~7.6 ms of a ~10.2 ms frame, so ~25% of the GPU timeline is between submissions (per-command-buffer and per-render-pass cost of the ~13 packets of a frame): the first GPU target.

## Tools

The frame CSV columns `interval_ms idle_ms t1_ms cpu_mcycles gpu_sum_ms outstanding` (`cpu_mcycles` is thread CPU time in ms on macOS); `UC_GPU_PACKETS=1` for `[gpu-packets]` lines; `sample <pid> 10 1 -file out.txt` attached once the run is at speed 1 (the `[fps]` lines show ~60 retraces per second); `iokit_user_client_trap` under `waitUntilSignaledValue` in the main thread is fence polling, under `hle_GXWaitDrawDone` in the guest thread is the draw-done wait.
