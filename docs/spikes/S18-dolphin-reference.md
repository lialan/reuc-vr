# S18: Dolphin as a technical reference for REUC's GPU (and CPU) costs (2026-11-03)

Source read: Dolphin at commit df4bd44 (`/Users/lialan/workspace/third-party-ref/dolphin`), read only: nothing built or run from it. Dolphin is
GPL-2.0-or-later: this note describes techniques and reasoning in our words, no Dolphin code goes into the repository. References are
`Dolphin file:function` (paths under `Source/Core/`) and ours (`file:line` at quest3-vr b390cb6). Our numbers are S17's (headset at native
3.07x unless noted, fightvr replay; Mac counts from the VR preview).

Starting point (S17): GPU-bound, ~36.7 ms GPU a frame at native, ~14 ms needed. Main scene ~10.7 ms (only ~0.8 scene screens of fragments pass
the depth test; ~443 draws issued a frame, 231-240 with `UC_TEX_MERGE=1`; ~20-40 us fixed per tiny draw), HUD/glow packet (8.7 ms before the
layers), bloom/copy chain, heat-haze particles (~5.4 screens of fragments), present ~4.4 ms.

**The short answer.** Dolphin is built for immediate-mode desktop GPUs. It has no tiler code at all (the only mentions are two TODO comments:
`VideoBackends/OGL/OGLGfx.cpp:SetAndDiscardFramebuffer`, `VideoCommon/FramebufferManager.cpp:ReinterpretPixelData`), it ends a host draw at every
GX state change, texture change and matrix load, and it ends the render pass at every EFB copy. REUC already does more than Dolphin for the
tiler (vertex pre-pass, merged runs, texture table, HUD/glow layers, bloom proxy, render-area shrinking, depth STORE_OP_NONE, VRS, foveation).
What Dolphin does have that we lack is GX *exactness* that also happens to be cheaper: early-Z where GX asks for it (zcomploc), a complete
alpha-test pre-evaluation, the EFB pixel format (no alpha in RGB8), destination alpha in one draw by dual-source blending, the clear folded
into the next pass's load op, and the deliberate use of one ubershader pipeline per fixed-function state. The ranked lists below map those to
our measured costs.

## 1. GX/TEV to GPU work

### What Dolphin does

- **Pipeline identity.** A pipeline is `GXPipelineUid` = vertex declaration + `BlendingState` + `DepthState` + `RasterizationState` + VS/GS/PS
  UIDs, hashed with XXH3 (`VideoCommon/PipelineUtils.cpp:PipelineToHash`, `UpdateHashWithPipeline`). The pixel UID
  (`VideoCommon/PixelShaderGen.h:pixel_shader_uid_data`, filled by `PixelShaderGen.cpp:GetPixelShaderUid`) specializes: stage count, texgen
  count, indirect stage count; per stage the 24-bit colour and alpha combiner words, tev order (texmap, texcoord, enable, colour channel),
  the swap tables actually used, konst *selections*, the full 21-bit `tevind` word; per indirect stage its texmap/texcoord (`tevindref`);
  texgen projection bits; alpha-test compares and logic (only if the test can go either way); fog type, projection and range enable; ztexture
  op; zfreeze; early/late Z mode and whether the shader writes depth; destination-alpha flag; RGBA6 format and dither; logic op (only for the
  framebuffer-fetch path); bounding box. Bits that cannot matter are cleared (`ClearUnusedPixelShaderUidBits`, `num_values` cuts the UID at
  the last used stage). Per-pixel lighting adds `LightingUidData`.
- **Kept dynamic** (uniform blocks): TEV colour registers and konst colours, alpha-test references, texture sizes, z bias, indirect matrices and
  scales, fog colour and parameters (`WritePixelShaderCommonHeader`, the `PSBlock`). In the vertex stage the whole XF matrix memory (positions,
  normals, post-transform), lights and colours are uniforms indexed by the per-vertex matrix index; the VS UID
  (`VertexShaderGen.h:vertex_shader_uid_data`) specializes texgen types/sources/post-matrix/normalize/projection, colour-channel lighting
  config (material/ambient source, diffuse and attenuation function, light mask per channel), vertex components.
- **Ubershaders** (`UberShaderPixel.cpp:GenPixelShader`, `UberShaderVertex.cpp`): the raw BP/XF state in uniforms, one pipeline per
  fixed-function state; used by `ShaderCompilationMode` (`VideoConfig.h`) Synchronous / SynchronousUberShaders / AsynchronousUberShaders /
  AsynchronousSkipRendering, selected per draw in `VertexManagerBase.cpp:UpdatePipelineObject` (specialized when ready,
  `ShaderCache.cpp:GetPipelineForUidAsync` queues it otherwise). The UID cache on disk (`ShaderCache.cpp:LoadPipelineUIDCache`) replays
  known pipelines at start, our key files' equivalent.
- **Destination alpha in one draw** (`PixelShaderGen.cpp:WriteColor`, `RenderState.cpp:BlendingState::Generate`): output 0 carries the colour
  and the *constant* alpha, output 1 the computed alpha. The colour blend reads source alpha from output 1 (dual-source factors), the alpha
  blend is ONE/ZERO on output 0, so the stored alpha is the constant: exact also for blending draws, one draw, one shading. Destination alpha
  only exists with the RGBA6_Z24 EFB format (`GetPixelShaderUid`: `useDstAlpha`); `ApplyDriverBugs` (`PipelineUtils.cpp`) drops dual source
  where unsupported or broken (`BUG_BROKEN_DUAL_SOURCE_BLENDING`: AMD/Windows GL, Intel/macOS; not Qualcomm).
- **EFB pixel format** (`BlendingState::Generate`, `BPFunctions.cpp:ClearScreen`, `TextureConverterShaderGen.cpp`): with RGB8_Z24 (or RGB565)
  the target has no alpha: alpha writes are off, DSTALPHA blend factors become ONE (`RemoveDstAlphaUsage`), clears skip alpha, copies read
  alpha as 255. RGBA6 colour is quantized to 6 bits with the GX 2x2 dither (`WriteColor`, the `dither` UID bit).
- **Alpha test and early Z.** `BPMemory.h:AlphaTest::TestResult` pre-evaluates the test from the compare modes and the logic op (AND, OR, XOR,
  XNOR with ALWAYS/NEVER: Pass, Fail or Undetermined); only Undetermined emits the test. GX zcomploc (BP 0x43 bit 6, `GXSetZCompLoc`) = early:
  with an undetermined alpha test Dolphin forces early fragment tests (`EmulatedZ::ForcedEarly`, `FORCE_EARLY_Z` in
  `GeneratePixelShaderCode`): depth is tested and written before shading and the discard only drops colour, as on the console.
  `ApplyDriverBugs` relaxes ForcedEarly to plain Early when the draw writes no depth. A test that always fails disables colour and alpha
  writes in the blend state (`alpha_test_may_succeed`). Hardware depth is trusted (`bFastDepthCalc`, default on; per-pixel depth only for
  ztextures, zfreeze or the accurate-depth option), which keeps the driver's early depth.
- **zfreeze**: `VertexManagerBase.cpp:CalculateZSlope` stores the reference triangle's depth plane, the shader writes depth from it.
  **Logic ops**: hardware logicOp, else framebuffer fetch (`WriteLogicOp`) or a blend approximation
  (`BlendingState::ApproximateLogicOpWithBlending`). **Fog**: `WriteFog`, type and projection specialized. **Indirect**: `WriteStage` emits
  each stage's indirect path for its own format/bias/matrix/wrap and a fixed texmap (no run-time map choice).
  **Lighting**: per vertex by default (`bEnablePixelLighting` off).
- Quirks worth knowing for picture parity: TEV in integer arithmetic; `if (prev.a == 1) prev.a = 0` after the alpha test (an alpha of 1/255
  passes the test but blends as 0, `GeneratePixelShaderCode`); the Qualcomm workarounds in section 4.

### What REUC does

- `renderer/pipeline_keys.h` (`PipelineKey`, 70 words: blend word, the TEV words as specialization constants 0..65, depth word, topology),
  `tev.slang` (specialization constants, float TEV), modules folded at build time (`tools/fold_tev_shaders.py`), the ubershader as fallback
  only (`uber_key`). Konst colours, registers, alpha references, fog and indirect parameters live in the per-draw `DrawState` uniform block
  (`tev.slang:10`), as in Dolphin.
- Headset: `kTevDirectStage` (`pipeline_keys.h:32`) compiles the indirect path out of direct stages; stages that do read an indirect
  coordinate still take `IND_CMD` and the indirect texmap from the uniform block at run time (`tev.slang:345`, `indirectCoord` at 306).
- Alpha test: key word 0 bit 16 from `gx::alpha_test_can_discard` (`pipeline_keys.h:55`): only ALWAYS-and-ALWAYS (not XOR) counts as
  "cannot discard"; such pipelines take the `EARLY_Z` variant (`smoke.slang:368`, `[earlydepthstencil]`, no discard). Every other alpha-tested
  draw is late Z (discard at `smoke.slang:430`).
- zcomploc and the EFB pixel format are ignored (G21: `GXSetZCompLoc`/`GXSetPixelFmt` -> BP 43, ~185 000 writes).
- Destination alpha: fused into one draw only without blending (`vulkan_renderer.cpp:841 fused_destination_alpha`); a blending draw with
  destination alpha is drawn a second time with the full TEV shader and the constant alpha (`vulkan_renderer.cpp:4124`). In the VR preview
  fight that is ~650 second draws per 2 s against ~70 000 issued (runtime logs of 2026-11-03, `[vr-cull]` line): **about 4 a frame**.
- Fog: run-time branch on `fsel` (`smoke.slang:318 applyFog`). GX lighting: per vertex, in the compute pre-pass (`smoke.slang:167 xformMain`),
  generic over the light mask.

### Changes for REUC (ranked)

| # | change | Dolphin reference | expected effect on our costs | picture risk |
|---|---|---|---|---|
| 1.1 | Honour zcomploc: an alpha-tested draw whose BP 0x43 bit 6 is set gets `[earlydepthstencil]` with the discard kept (depth written before the test), as Dolphin's ForcedEarly; without a depth write plain early. Log first how many alpha-tested scene draws have the bit. | `GetPixelShaderUid`, `GeneratePixelShaderCode` (FORCE_EARLY_Z), `ApplyDriverBugs` | Hidden fragments of those draws are no longer shaded (late Z shades every covered fragment today). Size = their share of main-scene shading; unknown until counted. | Moves the picture *towards* the console: depth is then written where the alpha test fails, which GX does when the game asked for early Z. Check against the G20 Dolphin oracle frame. On KosmicKrisp mind `BUG_BROKEN_DISCARD_WITH_EARLY_Z` (section 4). |
| 1.2 | Pre-evaluate the alpha test fully: Dolphin's AND/OR/XOR/XNOR table with ALWAYS/NEVER, plus reference-aware cases our key can afford because the references are known per draw (GEQUAL 0, LEQUAL 255 always pass; LESS 0, GREATER 255 never). A test that always fails: colour and alpha writes off. | `AlphaTest::TestResult`, `BlendingState::Generate` | More pipelines on the `EARLY_Z` variant (no discard: early rejection, LRZ-friendly). Cheap; count the draws first (`[gpu-packets] scene draws not order-free ... alpha test`). | None: exact. Changes keys (bit 16), so the key files and folded modules follow. |
| 1.3 | Honour the EFB pixel format: RGB8_Z24 means no alpha (no alpha writes, no destination-alpha draws, DSTALPHA -> ONE, copies read alpha 255); RGBA6_Z24 quantizes colour to 6 bits with the 2x2 dither. | `BlendingState::Generate`, `ClearScreen`, `WriteColor` | If the scene runs in RGB8 its destination-alpha work disappears (the ~4 second draws, the alpha writes). If it runs in RGBA6, no gain, but the console's 6-bit banding/dither is then reproduced. | RGB8: none (it is what the hardware does). RGBA6 quantization changes every pixel slightly: make it a separate, owner-visible switch. |
| 1.4 | Destination alpha with blending in one draw by dual-source blending (output 1 = computed alpha, colour factors SRC1_ALPHA, alpha ONE/ZERO). EFB pipelines on the headset have one colour attachment, which `maxFragmentDualSrcAttachments` (1) allows; KosmicKrisp reports `dualSrcBlend` (docs/superpowers/plans/2026-11-01-macos-vulkan.md); check the Quest's `[vulkan]` features line. | `WriteColor`, `BlendingState::Generate`, `ApplyDriverBugs` | ~4 draws a frame and their second shading. Worth it only if those four are large (find their callsites first: a full-screen draw costs 1.5-2.5 ms at native, S17). | None: exact, and simpler than the second pass. |
| 1.5 | Indirect stages fully in the key (format, bias, matrix, wrap, the indirect texmap and texcoord: Dolphin's `tevind`/`tevindref`), so an indirect stage loses its run-time texmap switch and size queries as direct stages did in ed2a42f. | `GetPixelShaderUid`, `WriteStage` | Small for the frame: the glow/heat-haze modules are already 60-76 instructions (S17 cost model); 4dffd6f6 (311 instructions, 16 registers) has few fragments. | None; more keys to fold. |
| 1.6 | Fog type/projection in the key (Dolphin `fog_fsel`, `fog_proj`). | `WriteFog` | Negligible: the sprites with fog need it anyway; registers are already at 6. | None. |

## 2. Draw batching

### What Dolphin does

- `VertexManagerBase.cpp:PrepareForAdditionalData` appends a GX primitive to the open batch and flushes only when the primitive class changes
  (lists/lines/points) or the stream buffer is full; `Flush` issues one `DrawIndexed` for the batch. But every BP write that changes a value
  flushes (`BPStructs.cpp:BPWritten` -> `FlushPipeline`), every XF memory write flushes (`XFStructs.cpp:XFMemWritten`: every matrix load), and
  texture changes are BP writes. So Dolphin issues about one host draw per GX state run: no merging across textures, matrices or TEV setups.
  Draws stay together only inside one state run, e.g. a skinned mesh whose vertices carry matrix indices (`IndexGenerator` turns quads, strips
  and fans into lists, or restart strips where primitive restart works; it is off on Qualcomm, `BUG_PRIMITIVE_RESTART`).
- `CPUCull.cpp` (`VertexManagerBase::AreAllVerticesCulled`) drops primitives that are entirely off-screen or back-facing on the CPU.
- Vulkan: `VideoBackends/Vulkan/StateTracker.cpp:Bind` binds pipeline, viewport, scissor, vertex and index buffers only when dirty; only viewport
  and scissor are dynamic state (`VKPipeline.cpp`). `UpdateGXDescriptorSet` allocates a new set only for the part that changed (UBO set,
  sampler set, SSBO set) from per-command-buffer pools; the UBOs are `UNIFORM_BUFFER_DYNAMIC`, so a new constant upload is a new offset in the
  bind, not a new set. Constants come in three blocks (vertex, geometry, pixel) uploaded only when their manager is dirty
  (`VKVertexManager.cpp:UploadUniforms`): a matrix load costs a new vertex-block offset and leaves the pixel block as it was.
- Host draws per frame: for this game Dolphin would issue roughly the game's GX draws, i.e. our issued + merged count before any merging
  (~990 a frame in the VR preview fight). REUC issues 443, or 231-240 with the texture table.

### What REUC does

Vertex pre-pass (transforms, lighting and texgen baked per vertex, S17 fix 1), runs of draws merged into one `vkCmdDrawIndexed`
(`vulkan_renderer.cpp:3824 issue_run`), texture table (`UC_TEX_MERGE=1`), descriptor sets shared per texture combination with the uniform and
eye blocks as dynamic offsets (`vulkan_renderer.cpp:2984-3073`), cull and depth as dynamic state. What still ends a run (Mac, `UC_TEX_MERGE=1`,
per presented frame): pipeline 112-116, textures 53-55, uniform block 32-33. On the headset S17 measured ~40 us per tiny HUD draw; binding one
pipeline for the whole HUD packet (a diagnostic) saved 1.3 of 2.2 ms, viewport/push constants/dynamic state 0.7, descriptor sets nothing.

### Changes for REUC (ranked)

| # | change | Dolphin reference | expected effect | picture risk |
|---|---|---|---|---|
| 2.1 | **Ubershader as a merge vehicle for small draws.** Dolphin uses its ubershader against compile stalls, but its property (one pipeline per fixed-function state, the TEV setup as data) attacks our per-draw cost directly. Step 1: the HUD packet's draws on `uber_key` pipelines (the HUD layer variant) instead of specialized ones. Step 2: the fragment stage reads the draw's `DrawState` from the pre-pass's `xformDrawStates` array by a per-vertex draw index (the texture table already carries one per-vertex index, `tev.slang` TEX_TABLE), so draws that differ in TEV setup, uniform block and textures but not in blend/depth/viewport/scissor join one run. Gate by draw area (`gx::draw_bounds_ndc`): only draws whose fragments are few go uber; big draws keep their folded module. | `UberShaderPixel.cpp:GenPixelShader`, `VertexManagerBase.cpp:UpdatePipelineObject` | HUD: up to the measured 1.3 ms (one pipeline for the packet). Main scene: the pipeline (112-116) and uniform (32-33) run ends among small draws; at 20-40 us each the ceiling is ~3-6 ms, realistically 1-3 ms. Costs ALU on the merged draws' fragments (the uber module is far longer than a folded one): measure with an area threshold sweep. | Same TEV source (`-DUBER`), so the same arithmetic; the uber needs an `EARLY_Z` form for draws without discard (key word 0 bit 16 survives `uber_key`). Check captures for 1/255 differences. |
| 2.2 | **Fragment-only uniform identity** (Dolphin's separate pixel block). With the pre-pass on, the draw's vertex stage no longer reads transforms, lights or texgen rows from `DrawState`; a run should end only when the fragment-visible part changes (registers, konst, pixel, fog, indirect, ind_cmd, channels where read) rather than when the 1.8 KB block's identity (`uniforms.block()`, `vulkan_renderer.cpp:3033`) changes. | `VKVertexManager.cpp:UploadUniforms` (VS/PS blocks dirty separately) | Up to the 32-33 uniform run ends a frame: at 20-40 us, 0.6-1.3 ms if all join (fewer: many also differ in textures or pipeline). | None, provided the compare covers every field a fragment module reads (desktop MOTION/effects read lights and channels: keep the full block there). |
| 2.3 | Keep viewport and push constants out of the run key where they cannot differ in effect (S17: 0.7 ms of the HUD's 2.2 ms is viewport/push/dynamic state). Dolphin sets them only on change (`StateTracker::SetViewport` memcmp). | `StateTracker::Bind` | Part of the 0.7 ms on the HUD packet. | None if exact equality is kept. |

## 3. EFB handling: copies, clears, render passes

### What Dolphin does

- **Copies** (`VideoCommon/TextureCacheBase.cpp:CopyRenderTargetToTexture`, whose comment block lays out EFB-to-RAM, EFB-to-texture and the
  old hybrid): `ResolveFramebufferCopyData` decides `copy_to_vram` and `copy_to_ram` ("Store EFB copies to texture only" =
  `bSkipEFBCopyToRam`), the size (`bCopyEFBScaled`: at the internal resolution or native), and a linear filter for half-scale copies.
  `SetupCopyEntry` makes a one-level RGBA8 array texture (one layer per eye). `CopyEFBToCacheEntry` ends the EFB pass and draws one triangle
  with the copy shader (`TextureConversionShaderGen`: format conversion, gamma, the GX copy filter coefficients, top/bottom clamps) into the
  copy, its framebuffer bound with a discard load (`SetAndDiscardFramebuffer`). EFB-to-RAM encodes on the GPU and defers the readback
  (`bDeferEFBCopies`, `m_pending_efb_copies`, `FlushEFBCopies`) until the CPU could see it. `TextureCacheBase::GetVRAMCopyFilterCoefficients`
  disables the filter on request.
- **Copy with clear** (`BPStructs.cpp`, PE copy `clear`) -> `BPFunctions.cpp:ClearScreen` -> `VideoBackends/Vulkan/VKGfx.cpp:ClearRegion`: outside
  a render pass (always the case right after a copy) the clear *becomes the next pass*: `VKFramebuffer::SetAndClear` ->
  `StateTracker::BeginClearRenderPass`, load op CLEAR, render area = the clear rectangle; the following draws continue in that pass while their
  viewport stays inside it (`StateTracker::Bind`, `IsViewportWithinRenderArea`), otherwise it ends and a LOAD pass starts. Inside a pass:
  `vkCmdClearAttachments`; else a quad.
- **Render passes**: each framebuffer has a load, a discard and a clear variant (`VKTexture.cpp` / `ObjectCache.cpp:GetRenderPass`); colour
  and depth are always stored, stencil don't-care. Every EFB copy, readback, reinterpret and poke ends the EFB pass. No render-area shrinking,
  no STORE_OP_NONE, no input attachments.
- **Peeks/pokes/bbox**: `FramebufferManager.cpp:PeekEFBColor`/`PeekEFBDepth` read back whole tiles of the EFB at native resolution into a
  cache (`iEFBAccessTileSize`, `RefreshPeekCache`), pokes are batched (`FlushEFBPokes`), "skip EFB access" returns zero without a sync; bounding
  box is an SSBO written with atomics (`VKBoundingBox.cpp`). Not known to matter here: none of our GX notes (G0, G21) mention peek, poke or
  bounding-box use, but that is absence of evidence (the gx-vulkan plan still lists "does the game call GXPoke*/peek" as open).
- **Formats**: EFB RGBA8 for every GX format; depth D32F, **D24_S8 on Qualcomm Vulkan** (`FramebufferManager.cpp:GetEFBDepthFormat`,
  `BUG_BROKEN_D32F_CLEAR`: 32-bit depth clears had no effect on Adreno).
- **Bloom-style chains**: nothing special: each copy is a pass break plus one draw; the cheap setting is native-size copies
  (`bCopyEFBScaled` off), which makes every blur input a native-resolution texture.

### What REUC does

Each packet (the draws between two EFB copies) is one dynamic-rendering pass (`vulkan_renderer.cpp:3592 record_pass`): render area shrunk to the
draws' viewports/scissors, depth not stored when no draw writes it (STORE_OP_NONE, LOAD_OP_NONE when nothing touches it), copies as blits after
the pass, copy mips made lazily (`record_copy_mips`), the bloom chain in a 2-points-a-guest-pixel proxy (704806c), HUD and glows in layers, the
present reading the EFB in place. A copy's clear is its own pass (`vulkan_renderer.cpp:4518 clear_region`: render area = the cleared rectangle,
DONT_CARE + `vkCmdClearAttachments` + STORE); the next pass then LOADs. So the main scene's pass starts with depth LOADed. S17: the render-stage
trace counted **no LRZ-killed pixels** with or without static depth state.

### Changes for REUC (ranked)

| # | change | Dolphin reference | expected effect | picture risk |
|---|---|---|---|---|
| 3.1 | **Fold a copy's clear into the next pass's load op** (CLEAR over the next pass's render area when the cleared rectangle covers it; the separate clear pass otherwise), for colour and depth. Then check LRZ: Adreno builds its low-resolution depth during binning and rejects hidden fragments for the whole pass, but drivers commonly keep it off for a pass whose depth is LOADed without a valid LRZ state, and for the rest of a pass after a depth-writing draw with discard, a change of compare direction, or ALWAYS/NOT_EQUAL with depth writes (this is driver knowledge, not Dolphin's, and unverified for Qualcomm's driver). The `[gpu-packets] scene draws not order-free` counters already list blending/alpha test/no depth test/compare per scene. | `VKGfx::ClearRegion`, `StateTracker::BeginClearRenderPass`, `VKFramebuffer::SetAndClear` | The clear pass itself is cheap (S17: ~0.1 ms). The gain, if any, is LRZ in the main scene: compare the profiler's shaded-fragment count with the occlusion-query count (8.4 M passing at native). The main scene is fragment-bound at native; whatever overdraw LRZ removes is the size (plausibly 1-2 ms if shaded is ~1.5x passing; nothing if the driver keeps LRZ off for another reason). | None: same clears, same order. |
| 3.2 | **D24_UNORM_S8 depth on the headset** as an A/B. Dolphin uses it on Adreno for a (possibly old) clear bug; for us it is GX's own 24-bit depth (S17 ruled out D16 for losing it). | `GetEFBDepthFormat` | Unknown: same size in tile memory; compression and LRZ behaviour may differ. Cheap to try with 3.1. | Low: GX depth is 24-bit integer; equal-depth ties and clears map exactly. |
| 3.3 | Do not adopt Dolphin's copy path: a copy draw instead of our blit moves the same bytes, and Dolphin breaks the pass at every copy as we do. Its one-level copy textures confirm the lazy-mip choice (cdef265): games run without mips on EFB copies in Dolphin. | `SetupCopyEntry` | None. | - |
| 3.4 | Bloom proxy at 1 point a guest pixel for the blur passes (Dolphin's native-size copies) only after the proxy is measured on the headset (S17: not yet). | `ResolveFramebufferCopyData` (`bCopyEFBScaled`) | A fraction of the chain's remaining ~2.5 ms. | Softer bloom edges; the proxy at 2 points already differs at two bright spots. |

## 4. Mobile / Adreno

### Dolphin's Qualcomm entries (`VideoCommon/DriverDetails.cpp`, `DriverDetails.h`) and what they mean for us

| bug | Dolphin's workaround | relevance to REUC |
|---|---|---|
| `BUG_BROKEN_TEV_COLOR_ZERO` (Vulkan, Android, driver >= 0x802bc000) | Specialized TEV shaders miscompile when colour input A or B is ZERO; Dolphin feeds those inputs from `konstLookup[8]`, a uniform kept at zero (`PixelShaderGen.cpp`, around line 1440) | Our folded modules are specialized TEV with ZERO inputs everywhere. Dolphin's TEV is integer, ours float, so the same miscompile may not occur; when the headset captures are compared (tex-merge plan, relaxed precision), look first at stages with A or B = ZERO. If one is wrong, the same trick (a zero from the uniform block) applies. |
| `BUG_BROKEN_D32F_CLEAR` (Vulkan) | D24_S8 EFB depth | Our D32 clears work today (vkCmdClearAttachments and LOAD_OP_CLEAR); see 3.2 for the format A/B. |
| `BUG_PRIMITIVE_RESTART` (GL and Vulkan) | No primitive restart: lists | We draw lists only (`valid_pipeline_key`). Keep it so. |
| `BUG_BROKEN_REVERSED_DEPTH_RANGE` (Vulkan) | No minDepth > maxDepth viewports | We use the GX near/far as given (`vulkan_renderer.cpp:3881`); never introduce a reversed range. |
| `BUG_SLOW_CACHED_READBACK_MEMORY` (Vulkan) | Coherent, not cached, memory for readbacks | Our captures/readbacks (harness, `keep_frame`): use coherent memory if they ever show in a profile. |
| `BUG_SLOW_OPTIMAL_IMAGE_TO_BUFFER_COPY` (Vulkan, Android) | Copy to a linear image first (`VKTexture.cpp:VKStagingTexture::Create`) | Same, for capture readbacks only. |
| `BUG_BROKEN_NEGATED_BOOLEAN` (GL) | `cond == false` instead of `!cond` | GL only; not us. |
| `BUG_BROKEN_MULTITHREADED_SHADER_PRECOMPILATION` (all Android) | One pre-compile thread | We compile the warm-up keys on several threads (`vulkan_renderer.cpp:1259`); if the Quest ever crashes on the loading screen, this is the known cause. |
| `BUG_BROKEN_DISCARD_WITH_EARLY_Z` (Apple GPUs, Metal and MoltenVK) | Framebuffer fetch instead of discard under forced early Z (`ApplyDriverBugs`, `UberShaderPixel.cpp:GenPixelShader`) | KosmicKrisp runs on the same Apple GPUs: relevant to 1.1 on the Mac, and possibly to the tex-merge seams S17 traced to early-depth tie order on KosmicKrisp. |

### Android choices

Dolphin's Android defaults are the desktop ones (`Source/Android/.../BooleanSetting.kt`: fast depth on, scaled EFB copies on, bounding box off,
arbitrary mipmap detection on); Android adds custom Adreno drivers through adrenotools (`VideoBackends/Vulkan/VulkanLoader.cpp`), which the
Quest cannot use, and treats `VK_SUBOPTIMAL_KHR` as non-fatal (`CommandBufferManager.cpp`). There is no Android or tiler-specific rendering
path: no GMEM/LRZ awareness, no shading rates, no multiview (stereo is a geometry shader or two layers), no input attachments. Every
tiler-specific technique REUC has (S17) goes beyond Dolphin; Dolphin's mobile performance comes from 1x internal resolution and the user's
settings, not from structure.

## 5. Textures

### What Dolphin does

- Decoding: CPU decoders with NEON (`VideoCommon/TextureDecoder_ARM64.cpp`, generic in `TextureDecoder_Common.cpp`), or on the GPU by compute
  from the raw GX data in a texel buffer (`TextureCacheBase.cpp:DecodeTextureOnGPU`, `TextureConversionShader.cpp:GetDecodingShader`) to save
  CPU and upload bandwidth.
- Host formats: RGBA8 for every GX format; host BC/BPTC only for custom (replacement) textures. CMPR is decoded, not passed as DXT1: its
  3-colour mode differs from DXT1 (`TextureDecoder_Common.cpp`, the note that colour 3 is transparent black in DXT1 but not in CMPR).
- Invalidation: textures are hashed (XXH) on load, optionally only a sample of texels ("Texture Cache Accuracy",
  `textureCacheSafetyColorSampleSize` in `GetTexture`); TMEM preloading is emulated (`TMEM.cpp`). EFB copies overlapping RAM textures are
  patched in place (`DoPartialTextureUpdates`).
- Sampling (`GetSamplerState`): GX filters and LOD clamps as given; anisotropy only where safe (`IsAnisotropicEnhancementSafe`), and then
  bilinear/trilinear only; for mip chains that are *not* downscaled copies of the base (`ArbitraryMipmapDetector`, used for distance effects)
  the LOD bias is raised by log2(internal resolution scale) so the effect starts at the console's distance.

### What REUC does

Native decode on the builder thread (`renderer/gx_textures.cpp`), invalidation by guest write-watch page epochs (`runtime/gx_write_watch.h`, no
hashing per load: cheaper than Dolphin), RGBA8 / RGB565 on the host, the game's own mips uploaded but almost never sampled (S17: 0.06%
non-base-level fetches), anisotropy only on mip-filtered samplers.

### Changes for REUC

None with a measurable GPU gain. Notes: (a) an arbitrary-mip LOD bias is irrelevant while the game samples base levels; revisit if a distance
effect (fog textures, LOD fades) looks closer than on the console at 3.07x; (b) GPU decoding only if the builder thread ever becomes the limit
(it has half a core free, S17); (c) host block compression stays off: the Quest has no BC, and CMPR to ETC2/ASTC is a lossy re-encode.

## 6. CPU (brief)

### What Dolphin's JitArm64 does

- **FPR layout and precision tracking.** A register is cached as one of `RegType::Register`, `LowerPair`, `Duplicated`, `Single`,
  `LowerPairSingle`, `DuplicatedSingle` (`PowerPC/JitArm64/JitArm64_RegCache.h`). `PowerPC/PPCAnalyst.cpp` (the forward scan in `Analyze`)
  marks for each instruction which FPRs hold single-precision values (`fprIsSingle`: after `lfs`, `frsp`, single ops, paired ops, paired
  loads), which are duplicated, and which can be stored without the careful conversion (`fprIsStoreSafe`). `JitArm64_Paired.cpp:ps_arith`
  then computes in 32-bit lanes when all inputs are singles, and rounds frC to 25 bits for ps_mul/ps_madd when it is not
  (`Force25BitPrecision`, the Gekko multiplier quirk); fused multiply-add accuracy is a separate option (`m_accurate_fmadds`).
- **Quantized loads/stores** (`JitArm64_LoadStorePaired.cpp:psq_lXX`/`psq_stXX`): when a block uses one GQR that is zero at compile time, the
  block checks it once at entry and then emits plain float accesses (`Jit.cpp:DoJit`, `assumeNoPairedQuantize`); otherwise a call through a
  table of quantize routines indexed by type (`JitAsm.cpp:GenerateQuantizedLoads`/`GenerateQuantizedStores`, NEON SCVTF/UCVTF with the scale).
- **FPRF** is not computed by default (`MAIN_FPRF` false); **fastmem** faults are backpatched: the faulting fast access is rewritten into a
  branch to its slow path (`JitArm64_BackPatch.cpp:HandleFastmemFault`), so a site faults once.

### What REUC does

Paired singles as two-lane double vectors; every paired result goes through double -> float -> double (`runtime/ppc_rt.h:452 ps_result`), so
each paired op is the double op plus FCVTN and FCVTL. In the Mac build `fn_802647AC` has 52 FCVTN and 67 FCVTL against 33 `fmla.2d` and 15
`fmul.2d` (otool of `build/game/uc`); its ~800 loads/stores (context and stack) dominate it, as S17 found. psq: a GQR type test per access with the
float cases inline and the quantized ones out of line (`ppc_rt.h:560 psq_load`, `psq_store`); no FPRF; fastmem emulates the access in the
signal handler (`runtime/fastmem.cpp`) and the translator's MMIO-base dataflow keeps the hot MMIO sites explicit (0 faults in the fight).

### What an AOT translator could still adopt

1. **Single-precision dataflow for paired arithmetic** (Dolphin's `fprIsSingle`): where the translator can prove both inputs are singles,
   emit add/sub/mul/div in float lanes. For those four (and sqrt) the result is bit-identical to double-then-round, since 53 >= 2x24+2 makes
   the double rounding innocuous; keep the double path for the FMA forms (a double FMA then rounding to single is not always the single FMA)
   and for frC that may not be single (the 25-bit quirk). Effect: two conversions off the dependency chain of every such op; on paired-heavy
   functions a few percent of instructions and more of the latency. Determinism check as in S17 (`--det-out` over the fightmac replay).
2. Quantized psq with NEON fixed-point conversion only if `psq_load_slow`/`psq_store_slow` ever show in a profile (they do not today).
3. Nothing to take from fastmem: our zero-fault fight and the static MMIO marking are the AOT form of backpatching. FPRF: same as Dolphin
   (off).

## Top 5 recommendations

1. **Small draws on one ubershader pipeline per fixed-function state, with the draw state read by index (2.1).** Start with the HUD
   packet (measured upper bound 1.3 ms of its 2.2 ms per-draw cost), then a draw-area-gated rollout in the main scene, where pipeline and
   uniform run ends (~145 a frame) are the per-draw cost that is left; realistic 1-3 ms at native. Picture: same TEV source; check captures.
2. **Give the tiler its early rejection back (3.1, 3.2, 1.2).** Fold copy clears into the next pass's load op, A/B D24S8, and pre-evaluate
   the alpha test fully so more pipelines take `EARLY_Z`; then read the trace's LRZ counters and compare shaded vs passing fragments in the
   main scene. 1-2 ms plausible if LRZ comes on; zero picture risk.
3. **Honour GX zcomploc and the EFB pixel format (1.1, 1.3).** Both are what the console does; zcomploc puts alpha-tested draws that asked for
   early Z on early depth tests, RGB8 removes destination-alpha work. Count the draws first (a log of BP 0x43 per packet); check against the
   G20 Dolphin oracle frame.
4. **Fragment-only uniform identity for merging (2.2).** Up to ~30 more joins a frame (0.6-1.3 ms at 20-40 us a draw), no picture risk, small
   change in `vulkan_renderer.cpp` around 3033.
5. **Destination alpha with blending by dual-source blending (1.4)**, after identifying the ~4 second draws a frame: worth ~0.1 ms if they
   are small, up to a few ms if any is full screen; exact either way, and it removes the last two-draw path.

CPU, when the GPU no longer dominates: single-precision dataflow for paired add/sub/mul/div (section 6, item 1).

## Done: depth and EFB (2026-11-03, branch gpu-depth-efb; recommendations 2, 3 and 5)

All switches are off by default until the headset A/B (`/tmp/qgs/plan_depth.sh`, not run yet); the logic is in `renderer/depth_efb.h`
(tested by `test_depth_efb`), the counters in `renderer/vulkan_depth_efb.cpp`. Nothing is generated at run time: no switch adds a pipeline
key in the fight, so the key files and folded modules stay as they are.

| switch | what it does |
|---|---|
| `UC_FOLD_CLEAR=1` | An EFB copy's clear is not a pass of its own: it waits (`efb_clear_`) for the next EFB pass, which takes it as its colour and depth load op (CLEAR) when the cleared rectangle holds the pass's render area (the render area takes the rectangle in), or clears it first thing inside the pass otherwise. An empty packet passes it on; anything else that reads or writes the EFB first (bloom proxy, HUD/glow layers, upscale, HUD-less copy, a copy, the present, an EFB capture) records it as before (`record_efb_clear`). `UC_FOLD_CLEAR_LOG=1` names each fallback. |
| `UC_DEPTH_FORMAT=d24` | The headset's (and preview's) EFB depth and every depth target its pipelines draw into: X8_D24_UNORM_PACK32, else D24_UNORM_S8_UINT (enables separateDepthStencilLayouts for the depth-only barriers and views), else D32 (`[vulkan] EFB depth format ...` says which). `x8d24`, `d24s8`, `d32` force one. The clear depth `d/0xFFFFFF` is exact in 24-bit unorm. |
| `UC_ALPHA_PRECHECK=1` | Key word 0 bit 16 from the full pre-evaluation (`gx::alpha_test_result`: NEVER/ALWAYS plus GEQUAL 0, LEQUAL 255 pass, LESS 0, GREATER 255 fail, combined by AND/OR/XOR/XNOR), not only ALWAYS-and-ALWAYS. Exact: the shader compares the alpha rounded to 0..255. |
| `UC_DEPTH_STATS=1` | Per 2 s, per presented frame: alpha-tested draws and what the pre-check decides, zcomploc (BP 0x43 bit 6), EFB pixel formats, destination-alpha draws (fused / second draw, by callsite and size), EFB clears (own pass / load op / in a pass), the depth format. |

BP 0x43 (PE control, GXData+0x22C, `GXSetPixelFmt`/`GXSetZCompLoc` write it there: the translated GXSetZCompLoc stores bit 6 at r2-8968 -> +556)
now travels with each draw in `DrawUniform pixel[1]` bits 8-14 (bit 15 = known); no shader reads that word.

### Counted first (Mac VR preview, KosmicKrisp, fightvr replay, fight window, per presented frame)

| | per frame |
|---|---|
| draws (issued + merged) | 1260-1320 |
| alpha-tested (discard kept) | 317-355; the full pre-check decides **0** of them (all are real tests) |
| zcomploc early (ztop) | 975-1032; alpha-tested among them 60-96, all undetermined, **none writes depth**: 803B7FE4 (42, depth test off), 800BADD8 and 800BAF88 (8 each, LEQUAL, no write, <0.001 screens), 8006CED4 / 8006D174 (1 each, no test) |
| EFB pixel format | RGBA6_Z24 for **every** draw (no RGB8, no RGB565) |
| destination alpha | 740-777: fused 638-657, second draws **5-7**, all callsite 80366E14, blend SRCALPHA/INVSRCALPHA, 0.010-0.015 scene screens each (bounding box) |
| EFB clears | 1 (the present's clear after the XFB copy); the next packet is empty, the one after it is the next frame's scene pass |

So in this fight:
- **zcomploc (rec 3 / 1.1)**: the only case where honouring it changes anything is an alpha-tested early-Z draw that writes depth (depth then
  written where the alpha test fails, as on the console). There is none, here or in the G21 train capture (TD-049). The alpha-tested ztop
  draws without depth writes give the same picture with early or late depth tests, and the 16 a frame that test depth cover under 0.001
  screens. Not implemented as a pipeline variant (it would need a third TEV module family, folded per key, and Apple GPUs are known to
  break discard under forced early tests, S18 section 4); the counter stays for other scenes.
- **EFB pixel format (1.3)**: RGBA6 everywhere, so there is no destination-alpha work to skip. Not implemented beyond the counter.
  (RGBA6's 6-bit quantization and dither are a picture change, not a cost: separate question.)
- **Dual-source destination alpha (rec 5 / 1.4)**: 5-7 small draws a frame from one callsite (~0.07 scene screens together): at S17's 20-40 us
  per tiny draw at most ~0.2 ms on the headset. Doing it needs another module family (output 1 at location 0 index 1; the stereo modules
  also write the transmit target at location 1, which dual-source blending forbids), so not worth it now.
- **Alpha pre-check (1.2)**: implemented (exact, cheap); no draw of the fight changes, so no new key and no gain there.

### Folded clear and depth format: Mac numbers (same replay)

| | off | `UC_FOLD_CLEAR=1` |
|---|---|---|
| EFB clears a frame: own pass / load op / in a pass | 1 / 0 / 0 | **0 / 1 / 0** |
| render passes a frame | 15 + 1 clear pass | 15 |
| attachment pixels moved / whole targets, draw passes only (`[gpu-area]`; the clear pass's own stores are not in it) | 43% | 41-42% |
| draws issued a frame | 443 | 443 |
| GPU ms per present (Apple M4 Pro; run-to-run 5.6-6.9) | 5.6, 6.9 | 6.8, 5.9 (noise) |
| "present draws 0" packet (held the clear pass) | 0.96-1.06 ms | 0.80-0.92 ms |

The scene pass now starts with colour and depth CLEARed instead of LOADed: that is the state in which Adreno can keep LRZ valid for the
pass (driver behaviour, to be read in the render-stage trace). `UC_DEPTH_FORMAT=d24` takes D32 on the Mac: KosmicKrisp lists no 24-bit
depth format (Apple GPUs have none), so the D24 A/B is headset-only.

Pictures (EFB captures at retraces 8700/9060/9300, 5120x1440): base vs two more base runs <= 1/255; base vs `UC_FOLD_CLEAR=1` in three runs:
two <= 1/255, one 435 px over 1% at 9060 (peak 9/255); a run with every switch off showed the same kind of outlier at 9060 (382 px, peak
6/255), so that is KosmicKrisp's run-to-run variation, not the switch. All three switches at once: <= 1/255. The validation layer reports
nothing new with all three on (only the existing blend-attachment, DrawParameters and sampler-at-shutdown messages).

### Left for the headset (`/tmp/qgs/plan_depth.sh`)

- GPU ms of the main scene and the frame, off / fold / d24 / both, native, alternating twice; `[vulkan] EFB depth format` (which 24-bit
  format the Adreno takes); `[depth-stats] EFB clears` (1 as load op).
- EFB captures off vs fold and off vs d24.
- Render-stage traces off vs fold: whether LRZ now rejects anything in the main scene (S17: no LRZ-killed pixels so far).
- Then the defaults: fold and d24 on if they help; the pre-check can go on regardless (exact, no key changes in the fight; elsewhere it
  can add keys, which `pipeline_keys.quest` would then need).

## 7. Built: recommendations 4 and 1 (2026-11-03, branch gpu-ubershader)

Both opt-in, both ahead of time (prebuilt SPIR-V, no code generation at run time), both only on the headset path (stereo, vertex pre-pass).
Code: `renderer/uber_merge.h` (keys, the fragment-visible uniform compare, the size test; `test_uber_merge`), `vulkan_renderer.cpp` (the
run join in `record_pass`, the HUD layer's runs in `record_layer`, `uber_merge_pipeline`, set 2), `smoke.slang`/`tev.slang` (`UBER_MERGE`).

**Rec 4, fragment-only uniform identity (`UC_MERGE_FRAG_UNIFORM=1`).** With the pre-pass the draws' vertex stage reads no uniform block (the
compute pass reads each draw's own), so two draws join when their blocks are equal where a fragment reads them: TEV registers, konst, `pixel`
(alpha test, destination alpha), fog, indirect matrices, IND_CMD (`gx::fragment_uniforms_equal`). The run's fragments read its first draw's block.

**Rec 1, the small-draw ubershader (`UC_UBER_SMALL=1`, `=scene` or `=hud` for one half; `UC_UBER_PX=<guest pixels>`, default 1024; turns
the texture table on).**
- One pipeline per fixed-function state: key bit 21 (`kUberMergeKeyBit`) on `uber_key` (blend, cull, alpha-test discard bit, HUD layer bit,
  depth word, topology; the bank bit dropped), queued on the loading screen for every known key's state (the fight: `waiting 0`).
- Per-draw state by index: the pre-pass writes a vertex map after the attributes (two words a position: the vertex's first attribute word;
  its uniform block index | coordinate mask << 24; `XformJob.mergeMap`). The vertex stage (`smoke.stereo.xform.uber.vert.spv`) reads its
  attributes and draw index from it instead of push constants and passes the index flat; the fragment stage
  (`smoke.stereo.uber.merge[.early].frag.spv`) reads that draw's block and TEV words from the packet's uniform buffer bound as a storage
  array (set 2, `merge_layout_`), its textures from the texture table, its destination-alpha flag from its own key words. So one run holds
  draws of different TEV setups, uniform blocks, textures and attribute strides; it still needs the same eye block, viewport, scissor,
  push constants (shift, rate) and consecutive indices.
- Which draws: direct TEV stages only (no indirect path in the module), no second destination-alpha draw, not the particle layer, and a
  screen box (`draw_bounds_ndc`'s vertices, a cached mesh's object box) of at most `UC_UBER_PX` guest pixels; a vertex behind the camera or
  more than 256 vertices without a box counts as big.
- Only where it joins: a draw first tries the ordinary join; only when that fails and both it and every draw of the pending run may go on
  the same ubershader pipeline does the run move onto it. Single draws and runs that merge anyway keep their folded modules (moving every small
  draw onto the ubershader broke existing runs: 243 -> 251 draws a frame in a first try).
- HUD layer: its draws were issued one by one (no merging at all); small consecutive HUD draws with the same state, eye block, viewport and
  scissor are now one ubershader draw; a one-draw run keeps its own pipeline.
- The module: the stages as a loop that reads each stage's words where it runs (`evaluateTev` under `UBER_MERGE`), alpha inputs as an indexed
  array. Unrolled with the words preloaded (the existing ubershader's form, whose words are constants) it was 18.5k non-nop instructions and
  48 vec4 registers once the words came from a storage buffer.

**Draws a frame** (Mac VR preview, KosmicKrisp, fightvr replay, retraces 8450-9352, `UC_VR_HIDE_CROSSHAIR=1` so the HUD packet takes the HUD
layer as on the headset; per presented frame, `UC_GPU_PACKETS=1 UC_MERGE_WHY=1`; counter windows vary the per-frame numbers by ~3%):

| switches | draws issued | run ends: pipeline / uniform / not consecutive | notes |
|---|---|---|---|
| none | 453 | 104 / 30 (+ textures 225) / 23 | |
| `UC_TEX_MERGE=1` (the baseline) | 276-288 | 101-104 / 75-79 / 31-32 | HUD layer: ~56 draws, one each |
| + `UC_MERGE_FRAG_UNIFORM=1` | 258-260 (**-9%**) | 101-102 / 52 / 34 | ~20 uniform boundaries a frame removed |
| `UC_UBER_SMALL=hud` | 234 | 105 / 78 / 32 | HUD layer: 58 draws in 4.3 runs |
| `UC_UBER_SMALL=1`, `UC_UBER_PX=1024` (default) | 170-174 (**-39%**) | 85 / 24-29 / 35 | 193 draws in 17 ubershader runs; 33k guest px a frame on it |
| `UC_UBER_SMALL=1`, `UC_UBER_PX=4096` | 150-154 (**-46%**) | 77 / 18-23 / 34-36 | 240-260 draws in 19-23 runs; 72-81k guest px |
| both switches, 4096 | 146 | 77 / 13 / 37 | the uniform test adds little once the ubershader joins |

Threshold sweep (`UC_UBER_SMALL=1`): 256 px 199 draws (9k guest px on the ubershader), 1024 170 (33k), 4096 150 (72k), 16384 138 (187k),
65536 132, unlimited 127. With the crosshair shown (the HUD packet in the EFB): table 243, + fragment uniforms 214, + ubershader (4096) 154.
The MERGE_WHY classes now name the test of the join that failed: two table runs no longer count a texture difference (it does not end them),
so S17's "textures 53-55" of the table run appear here under "uniform block".

**Pictures** (EFB captures at 8700/9060/9300, 5120x1440, against the table alone; `/tmp/uber_tools/cmp0.sh`):
- table vs table + fragment uniforms: late Z (`UC_NO_EARLY_Z=1` both) identical, 0 px; with early-Z modules 41-127 px, peak 2/255.
- table vs ubershader (4096 and 1024): late Z identical, 0 px; with early-Z modules 4-65 px, peak 1/255.
- Both remaining differences are KosmicKrisp's early-depth tie order (S17's tex-merge finding): equal-depth fragments of what is now one draw
  resolve in another order; the same pair on late Z is bit-identical. For scale, two replays of the same build already differ in 27-64 px
  at 1/255. A first version that also took draws with indirect stages left one 28 px spot at 4/255 on late Z (float rounding of the
  ubershader's indirect path against the specialized one); with direct stages only it is gone.

**Ubershader cost** (ir3 for Adreno 740, `/tmp/qgs/ir3c`, static counts; the Qualcomm compiler differs):

| module | non-nop instructions | registers (vec4) | waves |
|---|---|---|---|
| `smoke.stereo.uber.merge.frag.spv` (alpha test) / `.early` | 1106 / 1048 | 32 | 6 |
| folded TEV modules (S17, after ed2a42f) | 124 on average | 6 | 16 |
| `smoke.stereo.xform.uber.vert.spv` / the table's vertex stage | 145 / 134 | 12 / 12 | 16 |

The loop body is ~960 instructions of which a plain stage executes roughly 450-500 (the selector selects, konst, swap, the float combiner;
the compare modes are branches): about 4x a folded one-stage module, more per extra stage. Rough break-even at native 3.07x: a guest pixel
is ~19 fragments for both eyes, so 33k guest px a frame (the default) is ~0.6 M fragments x ~400 extra instructions = ~0.25 G
instructions, ~0.5-1 ms at the fight's measured ALU rate, against ~105 fewer draws x 20-40 us = 2-4 ms. At 4096 px the extra ALU is about
twice that for 20 more joins: the default is the conservative end until the headset's sweep says otherwise. Screen boxes overestimate the
fragments (sprites and HUD quads are mostly transparent), so the real ALU cost is lower.

**Checks.** Mac build and 204/204 tests (`test_netplay`/`test_netplay_stream` fail only when ctest runs them in parallel with each other
-- port use -- and pass alone, as on the base); Quest APK builds (59 MB). Windows: no compiler here; the C++ uses nothing beyond what the file
already did, the new shader variants are stereo-only.

**What needs the headset** (`/tmp/qgs/plan_uber.sh`, not run): captures (table vs + fragment uniforms vs + ubershader; a late-Z pair if they
differ), GPU ms of the main scene and HUD packet with the five switch combinations at native and 1.0x in alternating order, the threshold
sweep (256-65536) and the HUD packet with the ubershader unlimited against S17's 1.3 ms bound. Both stay off until then.

## 8. Why the table and the ubershader were slower on the headset, and the fix (2026-11-04, branch uber-fast)

Starting point (headset, quest3-vr 44f55c6, fightvr replay, 1.0x, foveation off, GPU ms per presented frame at 545 MHz): defaults 14.3-15.1,
`UC_TEX_MERGE=1` 16.3, `UC_TEX_MERGE=1 UC_UBER_SMALL=1` 16.3 (native: 33.1 vs 43.1 with the ubershader and fragment uniforms). Fewer draws
(-40%) did not pay.

**What was slow: the texture table's per-fragment cost on big draws.** Per packet (the same runs):

| packet (1.0x, foveation off) | defaults | `UC_TEX_MERGE=1` | + `UC_UBER_SMALL=1` |
|---|---|---|---|
| main scene (`copy 640x360 draws 200+`) | 5.39 | **4.51** | 4.57 |
| HUD packet (`no copy draws 50-199`: at 1.0x the glow layer is off, so its two lens glows and the fade are full-screen EFB draws) | 1.43 | **3.01** | 3.03 |
| particles (`copy 320x180 draws 200+`: ~295 heat-haze / smoke sprites, ~5 screens of overdraw) | 1.37 | **2.11** | 1.77 |
| frame | 14.3 | 16.3 | 16.3 |

The table did what it was built for in the main scene (-0.9 ms: ~40% fewer draws, few fragments) and cost 2x per fragment in the
fill-bound packets. Every draw that could went onto its TEX_TABLE module, also single draws and runs whose textures are all equal, so
the full-screen glows and the sprites sampled through `tableImages[NonUniformResourceIndex(base + map)]`. The table without any merging
(`UC_TEX_MERGE_EAGER=1 UC_NO_MERGE=1`, below) shows it: the HUD packet is 3.2 ms with no merge at all, the same as with merging. It is not
the non-uniform qualifier: an APK whose table modules index without `NonUniformResourceIndex` (measurement only: wrong where a wave holds
two draws) measures the same, 14.2 eager and 11.0 lazy + ubershader, as the real one (14.2 / 11.0, same session). What costs is sampling
through a run-time index into a descriptor array at all instead of a fixed binding (most likely: the driver sets up a fixed binding's
texture state once a draw, before the fragments run, while an indexed descriptor is fetched by the shader; not traced). Static compiles do not show it (ir3 for Adreno 740: a native
`sam.s2en.nonuniform`, +4 instructions a module, same registers). The ubershader was not the problem: on top of the table it was neutral
to slightly positive (particles 2.11 -> 1.77).

**Fix 1: the table only where a run joins across textures (lazy, the default with the table on).** A draw keeps its folded module and own
set and only remembers its table pipeline; a run moves onto the table when a draw with the same key and uniform block but other textures
joins it (`record_pass`: `table_moves`, `table_takes`). Single draws and runs of equal textures never read the table. Draws a frame stay the
same (Mac: 222 lazy vs 232 eager, the windows vary ~3%), table runs drop from ~228 to ~47 a frame. `UC_TEX_MERGE_EAGER=1` restores every
draw on the table; `UC_TEX_MERGE_PX=<guest px>` lets only draws up to that screen box join across textures; `UC_TEX_MERGE_MAX=<n>` a
smaller table (diagnostics).

**Fix 2: a cheaper ubershader.** (ir3, Adreno 740, non-nop instructions / vec4 registers / waves)
- Colour and alpha inputs by select trees on the selector bits (`uberColorSource`, `uberAlphaSource`: bit 3 the group, bits 2..1 the
  vector, bit 0 the alpha broadcast; 12..15 fall out of (1, 1, 1, 0.5) and (konst, 0)) instead of switches, which both compilers lower to a
  compare and a select per case (~5 instructions a case, 60 cases a stage): general module 1106 -> 918.
- Families of draws with one and with two stages (`smoke.stereo.uber1.*`, `uber2.*`, `UBER_STAGES`): stages unrolled; the vertex stage
  reads each stage's coordinate straight from the pre-pass's attributes (the mask's ids in order) and passes the draw's TEV words as flat
  inputs, so the fragment no longer picks one of eight coordinates per stage or loads its words from the storage buffer: one stage 799 / 13
  / 14, two stages 1447 static (a 1-stage draw runs about half) / 24 / 8, against the general loop's 918 / 32 / 6. Runs still join on the
  general pipeline; a run is drawn with the smallest family that holds all its draws (Mac fight: ~27% of the ubershader's draws have one
  stage, ~53% two; ~47% of its runs take a family). `UC_UBER_FAMILY=0|1|2` (none / only one).
- An array of all 16 colour inputs indexed by the selector was tried: 839 instructions but 48 registers (4 waves); dropped.
All exact: Mac VR preview, late Z (`UC_NO_EARLY_Z=1`), EFB captures at 8700/9060/9300 identical (0 px) table-off vs lazy table, and lazy
table vs lazy table + ubershader (general, families).

### Headset A/B (uber-fast 3bcc9c6, fightvr replay, 1.0x, foveation High, GPU ms per presented frame, 545 MHz, two passes in alternating order)

| configuration | frame | main scene | HUD packet | particles | present | draws issued |
|---|---|---|---|---|---|---|
| defaults (table off) | 12.7 / 12.6 | 5.37 / 5.39 | 1.29 / 1.31 | 1.57 / 1.54 | 2.14 / 2.19 | 447 |
| `UC_TEX_MERGE=1 UC_TEX_MERGE_EAGER=1` (the old table) | 14.3 / 14.2 | 4.42 / 4.35 | 2.96 / 2.84 | 2.07 / 2.03 | 2.43 / 2.42 | 279 |
| `... EAGER=1 UC_NO_MERGE=1` (the table's cost alone) | 19.3 / 19.5 | 8.84 / 8.93 | 3.22 / 3.26 | 2.19 / 2.22 | 2.38 / 2.39 | 964 |
| `UC_TEX_MERGE=1` (lazy table) | 11.6 / 11.5 | 4.56 / 4.51 | 1.48 / 1.48 | 1.31 / 1.28 | 2.46 / 2.43 | 273-280 |
| **`UC_TEX_MERGE=1 UC_UBER_SMALL=1`** (lazy + ubershader, families) | **11.0 / 10.7** | 4.38 / 4.22 | 1.33 / 1.21 | 1.03 / 0.96 | 2.44 / 2.44 | 166-169 |

(The present and the small bloom-chain packets trade ~0.3 ms between them across configurations: work that overlaps a packet boundary is
counted on either side; the frame total is the measure.) `UC_NO_MERGE=1` also bounds the per-draw cost: 964 draws instead of 447 cost
the main scene +3.5 ms with the table's per-fragment cost included, at most ~7 us a draw.

More A/Bs on the same APK and settings (each row two runs, alternating with its neighbours):

| configuration | frame | main | HUD | particles | draws |
|---|---|---|---|---|---|
| lazy + ubershader (families on), the runs of this batch | 10.7 / 10.7 | 4.21 / 4.21 | 1.28 / 1.25 | 0.94 / 0.96 | 169 |
| lazy + ubershader, `UC_UBER_FAMILY=0` (the general loop only) | 11.0 / 10.7 | 4.37 / 4.21 | 1.34 / 1.28 | 1.02 / 0.97 | 162-174 |
| lazy table, `UC_TEX_MERGE_PX=16384` (big draws never join across textures) | 12.0 / 12.0 | 4.84 / 4.80 | 1.73 / 1.70 | 1.19 / 1.21 | 326 |
| lazy + ubershader, table modules without `NonUniformResourceIndex` (diagnostic APK) | 11.0 | 4.40 | 1.33 | 1.00 | 169 |
| eager table, same diagnostic APK | 14.2 | 4.39 | 2.87 | 2.03 | 274 |

- The families do not show at 1.0x (the ubershader's ~30k guest pixels a frame are ~0.3 M fragments here); they stay on: exact, fewer
  registers, measured at native below.
- A size gate on the table's joins is worse (+1.3 ms): the joins of big draws pay for their per-fragment cost. No gate.
- The non-uniform qualifier costs nothing measurable (above).

### Defaults (43e78fb): the table (lazy) and the small-draw ubershader on, on the headset

`UC_TEX_MERGE=0` turns both off, `UC_UBER_SMALL=0` the ubershader alone; the Mac's VR preview keeps both opt-in. Same APK, foveation High,
alternating:

| scale | both off (`UC_TEX_MERGE=0`) | defaults (lazy table + ubershader) | draws issued |
|---|---|---|---|
| native (3.07x) | 26.3 / 26.3 (main 9.65, HUD 2.86, particles 3.40) | **24.4 / 24.2** (main 8.16 / 8.01, HUD 2.81 / 2.77, particles 3.16 / 3.05) | 430-446 -> 165-168 |
| 1.0x | 12.6 (main 5.30, particles 1.56) | **11.3** (main 4.21, particles 1.01) | 435 -> 170 |

So at native -2.0 ms (-8%), at 1.0x -1.3 to -2.0 ms (-10 to -16%), against +2 ms (+14%) for the old table + ubershader. Pictures (EFB
captures at 8700/9060/9300, native, off vs defaults): the same by eye; 13-29k of 9.1 M pixels differ by more than 1% (1-2.3k by more than
4%), along edges, which the head pose of two runs already moves.

What is left of the per-fragment table cost is in the runs that do join across textures (~47 a frame): the gate test shows those joins pay
anyway. The remaining HUD packet at 1.0x (1.2-1.5 ms) is its full-screen glows and fade in the EFB (the glow layer starts at 2.5 points a
guest pixel); the particles' heat haze is fill (S17).

Native (foveation High), the ubershader's knobs on top of the defaults:

| configuration | frame | HUD packet | draws |
|---|---|---|---|
| defaults (`UC_UBER_PX` 1024, families on) | 24.2 / 24.2 (and 24.4 / 24.2 above) | 2.76-2.81 | 168 |
| `UC_UBER_FAMILY=0` | 24.1 / 24.3 | 2.79 | 168 |
| `UC_UBER_PX=4096` | 24.8 / 24.7 | 3.14 / 3.14 | 148 |
| `UC_UBER_PX=4096 UC_UBER_FAMILY=0` | 24.7 | 3.11 | 151 |

The families are neutral at native too, even with four times the ubershader's fragments: the ubershader's ALU is not what limits these
packets (its draws are small; ~0.3 M fragments a frame at 1.0x). They stay (exact, a third to half the registers) for scenes with more
small multi-stage draws. A larger size limit joins 20 more draws and costs +0.6 ms (the HUD layer's bigger quads on the ubershader): the
default stays 1024.

`UC_MERGE_FRAG_UNIFORM=1` on top of the defaults (1.0x, foveation High, alternating fu / def / fu / def): 10.7 / 10.8 against 11.1 / 11.0
(main 4.25 / 4.30 vs 4.47 / 4.42, 156 vs 169 draws): -0.3 ms, the same sign in both pairs but under the run-to-run noise; no picture risk
on late Z (section 7).

**Recommended defaults (the first two now in the code, headset only):** lazy texture table + small-draw ubershader at 1024 guest pixels
with its families; no table size gate; `UC_MERGE_FRAG_UNIFORM=1` as the next candidate (-0.3 ms) once a native pair confirms it.

## 9. Where the headset stands (2026-11-04, quest3-vr a1cc7d4)

What is adopted from Dolphin and on by default on the headset: the copy's clear folded into the next EFB pass (`VKGfx::ClearRegion`),
24-bit EFB depth (Dolphin's Adreno choice; captures show no z-fighting), the texture table with the ubershader for small draws (Dolphin
batches by state; here the table only where a run joins different textures, section 8), EFB copies kept on the GPU (Dolphin's "store EFB
copies to texture only"; the game reads nothing back in the fight: no EFB peeks, no copies to RAM). Ruled out from the fight's own GX state:
the alpha-test pre-check, zcomploc, the RGB8 EFB format, dual-source blending (section "Done: depth and EFB").

Headset defaults besides those: foveation high (vr.cfg version 6), render scale 1.0 into 1548x1656 eye images (version 7), the game's view
widened 1.8 x 2.7 for culling (the top and bottom of the eyes no longer pop).

GPU ms per presented frame, fightvr replay, 545 MHz (the runtime keeps that clock; UC_GPU_LEVEL=boost changes nothing):

| | start of the session | now |
|---|---|---|
| native scene, native eyes, foveation off | 41.3 | 31.2 (with foveation high 24.2-24.4) |
| render scale 1.0, foveation high | 12.6 (native eyes) | 10.7 (9.2 with 1656-row eyes before the table) |

Live train fight (harness frame-rate summary): render scale 1.0 58.8 fps mean (p50 60, p5 40), 1.25 56.1, 1.5 54. Above 1.0 the GPU is
75-80% busy yet frames miss their 120 Hz slot and land on 40 fps: pacing, not GPU load. Next (`/tmp/qgs/plan_pace.sh`): UC_XR_TIMING at 1.25,
display refresh 90 and 72 against 120, and the fragment-uniform merge key (rec 4) at native.
