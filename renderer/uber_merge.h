#pragma once
// The headset's draw merging beyond the texture table (VulkanRenderer::record_pass, the vertex pre-pass): what two consecutive draws may
// differ in and still be one vkCmdDrawIndexed.
//
// Fragment-only uniform identity (UC_MERGE_FRAG_UNIFORM=1): with the pre-pass, a draw's vertex stage reads nothing from its uniform block
// (transforms, lights and texgen rows were consumed by the compute pass, which reads every draw's own block), so a run need only end where
// the part of the block a fragment reads changes: the TEV registers and konst colours, the alpha test and destination alpha (pixel), fog,
// the indirect matrices and IND_CMD words. The run's fragments read the first draw's block.
//
// The small-draw ubershader (UC_UBER_SMALL=1, needs the texture table): a small draw goes on the ubershader pipeline of its fixed-function
// state (blend, cull, alpha-test discard bit, depth, topology); the fragment stage reads each draw's TEV words and uniform block from the
// packet's uniform blocks as an array, by a flat draw index the vertex stage takes from the pre-pass's vertex map, and its textures from the
// table. Draws that differ in TEV setup, uniform block and textures then join. A draw is small when its screen box covers at most
// UC_UBER_PX guest pixels (default kUberSmallPixels): big draws keep their folded module, whose fragments are far cheaper. A run moves onto
// the ubershader only when a draw joins it that could not join it otherwise, so single draws and runs that merge anyway keep their modules.
#include "gx_materials.h"
#include "pipeline_keys.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace uc {
// key[0] bit 21: the small-draw ubershader's pipeline (UBER_MERGE modules, set 2: the packet's uniform blocks as an array). Derived at draw
// time like kTexTableKeyBit, never recorded (valid_pipeline_key rejects bits 20 and up).
constexpr uint32_t kUberMergeKeyBit=1u<<21;
constexpr uint32_t kBankKeyBit=1u<<17;   // per-vertex texture matrices: only the pre-pass reads them, so not part of a merge pipeline
constexpr float kUberSmallPixels=1024.f;   // guest pixels (640x480 space): a 32x32 quad (S18 section 7: the ALU estimate and the sweep)
// The ubershader's families of draws with one and with two stages (key word 1 of the pipeline key: the family's stage count; 0: the general
// module, a loop over up to 16 stages): the stages unrolled, the coordinates and TEV words chosen per vertex and passed flat. ir3 (Adreno
// 740): 13 / 24 vec4 registers (14 / 8 waves) instead of 32 (6). In the fight ~27% of the ubershader's draws have one stage, ~53% two.
// Runs join on the general pipeline (so draws of any stage count still join); a run is drawn with the smallest family that holds all its
// draws (a draw without stages counts as one).
constexpr uint32_t kUberFewStages=2;   // the largest family
inline uint32_t uber_family(uint32_t stages) { return stages<=1 ? 1 : stages<=kUberFewStages ? stages : 0; }

// The pipeline key of a draw on the small-draw ubershader: the TEV words and the destination-alpha flag go (read per draw), the bank bit goes,
// the texture-table and merge bits come; the blend, cull and alpha-test bits, the HUD layer bit, the depth word and the topology stay; key
// word 1 names the family (family 1..kUberFewStages when the draw has at most that many stages, else 0: the general module).
inline PipelineKey uber_merge_key(PipelineKey key, uint32_t family=0) {
    if (family>kUberFewStages || key[1]>family) family=0;
    key=uber_key(key);
    key[0]=(key[0]&~kBankKeyBit)|kTexTableKeyBit|kUberMergeKeyBit;
    key[1]=family;
    return key;
}
// Whether a draw's TEV setup can go on the small-draw ubershader: every stage direct (the headset's keys mark them, kTevDirectStage). The
// ubershader has no indirect path: with it the stage loop was a fifth longer for draws that are rare among small ones.
inline bool uber_merge_stages_ok(const PipelineKey& key) {
    for (uint32_t stage=0;stage<std::min<uint32_t>(key[1],16);++stage) if (!(key[5+4*stage]&kTevDirectStage)) return false;
    return true;
}
// Whether two draws' full keys (with cull and depth, the dynamic state set per run) can share one ubershader run.
inline bool same_uber_state(const PipelineKey& a, const PipelineKey& b) {
    return ((a[0]^b[0])&~kBankKeyBit)==0 && a[68]==b[68] && a[69]==b[69];
}
}  // namespace uc

namespace gx {
// The part of a draw's uniform block that the headset's fragment stages read (tev.slang, smoke.slang shadeFragment; not the MOTION or
// DirectX variants, which also light per pixel).
inline bool fragment_uniforms_equal(const DrawUniform& a, const DrawUniform& b) {
    return a.registers==b.registers && a.konst==b.konst && a.pixel==b.pixel && a.fog==b.fog && a.indirect==b.indirect && a.ind_cmd==b.ind_cmd;
}
// Guest pixels a draw's screen box covers: its NDC box (x0, y0, x1, y1) clamped to the viewport, times the viewport's size in guest pixels.
inline float box_guest_pixels(const std::array<float,4>& box, float viewport_width, float viewport_height) {
    const float w=std::max(0.f,std::clamp(box[2],-1.f,1.f)-std::clamp(box[0],-1.f,1.f)), h=std::max(0.f,std::clamp(box[3],-1.f,1.f)-std::clamp(box[1],-1.f,1.f));
    return w*h/4*viewport_width*viewport_height;
}
}  // namespace gx
