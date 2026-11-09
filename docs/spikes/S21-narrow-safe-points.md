# S21: narrow safe points and FIFO store runs (2026-11-08, branch narrow-safe-points)

Question: the guest thread is the headset's CPU wall (S17: ~25 ms a game frame, 58% of it translated code). Wiicompiled (a Mario Kart Wii
static recomp) keeps guest registers in host registers across calls because its translated code has almost no safe points (guest threads
are fibres that switch only at OS calls). S17 item 3 found REUC's cross-call register summaries unsound beyond "leaf" callees because
every loop poll and every call was treated as a point where the whole guest state must be in the context. Which of those points really
need it?

## What needs the context

- **Loop polls** (`ppc::poll_slow` -> `Sched::safe_point`): no. Due events run on `irq_ctx_` (only r2/r13 are copied from the interrupted
  thread); a thread switch parks the host thread with its stack, so its local register copy survives; no HLE reads another thread's
  registers (the switch hook writes only the current-thread word); a rollback checkpoint accepts other threads only while they are parked
  in an SDK call (`Sched::checkpoint` throws otherwise; `preempt_check` refuses to park a non-main thread without a continuation).
- **HLE calls**: yes, an SDK call may park the thread for a checkpoint; `Sched::restore` rebuilds the frames from the context through the
  guest LR chain (resume entries), and the main thread reloads after the call that restored. Indirect calls and `sc` count the same.
- **Calls whose whole call tree reaches none of these**: no. Nothing can park inside them, so they need neither a full sync nor a resume
  entry after them.

## Changes (translator/emit.cpp)

1. A poll stores only r2/r13 when dirty and reloads nothing.
2. "Closed" functions replace leaf functions: no HLE but the leaf ones (`Leaf_*`, the native PS math), no indirect call, no `sc`, no jump
   into another function's middle; loops, jump tables, direct tail jumps, the fall into a closed next function and calls of closed
   functions are fine; summaries are the union over the tree (fixpoint, recursion included). A mod hook or probe at any function of the tree
   forces the full form: game_decl.h has one `uc_plain_tree_<component>` constant per call-graph component (Tarjan, callees first), the
   AND of `UC_PLAIN_ENTRY` over the component and the components it calls.
3. `add_resume_entries` drops the resume entry after a call of a closed function or a save/restore helper; entries the analyzer made for
   jumps from other functions stay. Native code that calls guest code with a hand-set LR was checked: every hard-coded target is still in
   the table; the one hand-set return address that lost its entry (0x801C44EC, `hle_FlightPathStep`) follows a call of a closed function.
4. FIFO runs (Wiicompiled's `GpuFifoBurst`): two or more FIFO stores (stb/sth/stw/stfs from an MMIO-marked base) in one block with the same
   base and offset, with only register operations and plain loads between them. The first store decides the run once
   (`uc::gx_fifo_burst`: the FIFO port, no audit device, room in the open window for every byte); each store then appends in place
   (`uc::gx_fifo_put`), exactly what its own `mmio_write` would have done; otherwise every store takes the store path as before. The FIFO
   counters moved to gx_fifo.h so the run keeps them exact.

main.dol: resume entries 76891 -> 36747, register-cache entry loads 403668 -> 247205, reloads 288201 -> 181086, flushes 753839 -> 669882,
closed callees 3679 -> 7001, partial-sync calls 22200 -> 36362; 351 FIFO runs with 1981 of the 2370 MMIO-base sites. Quest APK 41.8 ->
39.4 MB. game_decl.h gains ~660 KB of plain constants (at most 18 terms each).

## Checks

- 208/208 tests; test_emit covers the light poll (and the r13 case), transitive closed calls with a loop, the plain constants and their
  order, an HLE breaking closedness (full sync, resume entries kept), mutual recursion (one constant), resume pruning, FIFO runs.
- Determinism: `fightmac` replayed with `--det-out` (`UC_DET_EVERY=30`, 307 records to retrace 9226): guest memory identical to 7011f82
  for the safe-point change alone and with the FIFO runs; `fifo_writes` per 60 retraces identical.
- Mac (`uctest bench --demo fightmac --runs 3`, fixed order base / safe points / + FIFO each round): guest Mcycles 4967/4864/5005,
  4842/4915/4877, 4856/5056/4820: means -1.4% and -0.7%, within the noise (wall time inconclusive: the replay's draw-done waits for the
  GPU). The M4's store-to-load forwarding hides most of the context traffic; the headset, frontend- and instruction-bound (S17), is where
  it should show.
- Headset: the owner tests the installed build live (no A/B replay run).

## Not done

- A thread preempted at a poll inside a guest callback of a parking HLE (`call_guest` keeps `native_depth` at 1) could in principle be
  snapshotted with a register copy that is not current; before this change the snapshot held the callback's registers, also not the
  caller's. The parking HLEs (waits, sleep, draw-done, retrace) deliver their callbacks on the interrupt context.
- The crash bundle's thread registers (`dump_contexts`) can be stale for a thread parked at a poll.
- Passing guest registers as host arguments (Wiicompiled's state-free ABI) on top of the closed calls: the partial sync still goes through
  the context in memory.
