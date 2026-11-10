#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <set>
#include <vector>

namespace uc {
// A graphics pipeline is fully described by this key (blend/cull word, TEV specialization constants, depth word, topology).
using PipelineKey=std::array<uint32_t,70>;

// Key file: {magic, version, key size} followed by raw keys. Replaying it at startup compiles pipelines before the game first needs them.
inline constexpr uint32_t kPipelineKeysHeader[3]={0x4B504355u,1u,uint32_t(sizeof(PipelineKey))};

// The key a logic-op draw gets on a GPU without logicOp (VulkanRenderer::pipeline_key): CLEAR and NOOP as blends, everything else as a plain
// write. key[0] = mode | source<<2 | destination<<5 | logic<<8 | color_write<<12 | alpha_write<<13.
inline PipelineKey without_logic_op(PipelineKey key) {
    if ((key[0]&3)!=2) return key;
    const uint32_t writes=key[0]&((3u<<12)|(3u<<16)), logic=(key[0]>>8)&15;   // the write masks, the alpha-test bit and the bank bit stay
    if (logic==0) key[0]=1u|(0u<<2)|(0u<<5)|(3u<<8)|writes;        // CLEAR: ZERO*src + ZERO*dst
    else if (logic==5) key[0]=1u|(0u<<2)|(1u<<5)|(3u<<8)|writes;   // NOOP: ZERO*src + ONE*dst
    else key[0]=0u|(1u<<2)|(0u<<5)|(3u<<8)|writes;                 // COPY and the rest: no blending
    return key;
}
// The headset's keys mark every TEV stage that reads no indirect texture coordinate (its IND_CMD is 0) in bit 16 of the stage's order word
// (key[5 + 4 * stage], specialization constant 4 + 4 * stage): tev.slang then compiles the indirect path out of that stage. Desktop keys never
// set it (their recorded seed stays valid). Keys recorded before the bit existed are read as all-direct (headset_direct_key).
constexpr uint32_t kTevDirectStage=1u<<16;
// Indirect-stage specialization uses spare bits in the existing stage words:
// order[17] marks it, order[18..23] selects the indirect map/coordinate,
// konst[10..30] holds IND_CMD. The low order/konst fields keep their GX meaning.
// Matrix coefficients and coordinate scales remain uniforms.
constexpr uint32_t kTevIndirectStage=1u<<17;
constexpr uint32_t kTevIndirectReferenceMask=63u<<18;
inline void specialize_indirect_stage(PipelineKey& key,uint32_t stage,uint32_t command,uint32_t references) {
    if (!command || (command&~0x1FFFFFu) || !(key[5+4*stage]&64)) return;
    key[5+4*stage]=(key[5+4*stage]&~(kTevDirectStage|kTevIndirectReferenceMask))|kTevIndirectStage|
        (((references>>((command&3)*6))&63u)<<18);
    key[6+4*stage]=(key[6+4*stage]&1023u)|((command&0x1FFFFFu)<<10);
}
inline PipelineKey dynamic_indirect_key(PipelineKey key) {
    for(uint32_t i=0;i<std::min<uint32_t>(key[1],16);++i) {
        key[5+4*i]&=~(kTevIndirectStage|kTevIndirectReferenceMask);
        key[6+4*i]&=1023u;
    }
    return key;
}
// Coordinates consumed by a known TEV shader. Dynamic indirect stages can select
// any coordinate from IREF, so keep the general vertex path for those keys.
// This masks outputs, not the packed pre-pass layout: that can contain extra
// coordinates (including raw indirect inputs removed by the VR adaptation).
inline uint32_t xform_coordinate_mask(const PipelineKey& key) {
    uint32_t mask=key[1]==0 ? 1u : 0u;
    for (uint32_t i=0;i<std::min<uint32_t>(key[1],16);++i) {
        const uint32_t order=key[5+4*i];
        if (!(order&64)) continue;
        mask|=1u<<((order>>3)&7);
        if (order&kTevDirectStage) continue;
        if (!(order&kTevIndirectStage)) return 255u;
        mask|=1u<<((order>>21)&7);
    }
    return mask;
}
// Stage-zero order bit 24: every consumed texture coordinate has Q=1.
// This is a whole-shader property, including the no-stage texture-zero path.
constexpr uint32_t kTevAffineCoordinates=1u<<24;
inline PipelineKey general_coordinates_key(PipelineKey key) {
    key[5]&=~kTevAffineCoordinates;
    return key;
}
inline PipelineKey affine_coordinates_key(PipelineKey key) {
    if (xform_coordinate_mask(key)) key[5]|=kTevAffineCoordinates;
    return key;
}
inline PipelineKey headset_direct_key(PipelineKey key) {
    const uint32_t stages=std::min<uint32_t>(key[1],16);
    for(uint32_t i=0;i<stages;++i) if(key[5+4*i]&(kTevDirectStage|kTevIndirectStage)) return key;   // already in the headset's form
    for(uint32_t i=0;i<stages;++i) key[5+4*i]|=kTevDirectStage;
    return key;
}
// key[0] bit 18: the headset's HUD layer variant (hud_layer.h): no multiview, two colour attachments (L, T), no depth.
constexpr uint32_t kHudLayerKeyBit=1u<<18;
// key[0] bit 19: the headset's particle layer variant (VulkanRenderer, UC_PARTICLE_LAYER): the EFB's formats, alpha written as T (ZERO, D).
constexpr uint32_t kParticleLayerKeyBit=1u<<19;
// key[0] bit 20: the headset's texture-table variant (VulkanRenderer, UC_TEX_MERGE): the draw's textures from the packet's table (TEX_TABLE
// modules, set 1). Never recorded in a key file (valid_pipeline_key rejects it): it is derived from the recorded key at draw time.
constexpr uint32_t kTexTableKeyBit=1u<<20;
inline bool valid_pipeline_key(const PipelineKey& key,bool logic_ops) {
    const bool topology=key[69]==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST || key[69]==VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    return topology && key[1]<=16 && key[67]<=1 && key[68]<32 && (key[0]>>20)==0 && ((key[0]&3)!=2 || logic_ops);
}
}  // namespace uc (pipeline keys)
namespace gx {
// Whether a GX alpha compare word (the draw's pixel[0]: references in bits 0-15, compare ops in 16-18 and 19-21, the combining op in 22-23) can
// reject a fragment. Both compares ALWAYS (7) pass every fragment unless combined by XOR (2: true != true fails every one). A pipeline whose draws
// cannot reject compiles its discard out (key word 0 bit 16), which keeps early depth rejection on for it.
inline bool alpha_test_can_discard(uint32_t word) {
    const uint32_t first=(word>>16)&7, second=(word>>19)&7, combine=(word>>22)&3;
    return !(first==7 && second==7 && combine!=2);
}
}  // namespace gx
namespace uc {

// Cull mode and the depth test, write and compare op are dynamic state (Vulkan 1.3), set from the draw's full key; a pipeline only keeps
// whether the draw writes depth (the motion and G-buffer masks follow it). Pipelines, the seed's closure and the recorded file use this form.
inline PipelineKey static_key(PipelineKey key) {
    key[0]&=~(3u<<14);
    key[68]=(key[68]&17)==17 ? 17u : 0u;
    return key;
}

inline VkCullModeFlags cull_mode(const PipelineKey& key) {
    constexpr VkCullModeFlags culls[]={VK_CULL_MODE_NONE,VK_CULL_MODE_BACK_BIT,VK_CULL_MODE_FRONT_BIT,VK_CULL_MODE_FRONT_AND_BACK};
    return culls[(key[0]>>14)&3];
}

// The part of a key the ubershader still needs a separate pipeline for (blend/cull, depth, topology); the TEV words and the
// destination-alpha pass are read from the draw at run time.
inline PipelineKey uber_key(PipelineKey key) {
    std::fill(key.begin()+1,key.begin()+68,0u);
    return key;
}

// Every ubershader key built from the blend/cull words, depth words and topologies that occur in `keys`: a key not recorded yet is mostly a
// new TEV setup over fixed-function state seen before, so these cover it with a ready pipeline.
inline std::vector<PipelineKey> uber_key_closure(const std::vector<PipelineKey>& keys) {
    std::set<uint32_t> blends,depths,topologies;
    for(const auto& key:keys) { blends.insert(key[0]); depths.insert(key[68]); topologies.insert(key[69]); }
    std::vector<PipelineKey> result;
    for(auto blend:blends) for(auto depth:depths) for(auto topology:topologies) { PipelineKey key{}; key[0]=blend; key[68]=depth; key[69]=topology; result.push_back(key); }
    return result;
}

// Write mask of the motion attachment (upscaling): only draws that write depth (compare + update, key word 68) write their vectors.
inline VkColorComponentFlags motion_write_mask(const PipelineKey& key) {
    return (key[68]&17)==17 ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT : 0;
}

// Write mask of the G-buffer attachment: the same draws that write motion vectors, i.e. the ones that define a visible surface.
inline VkColorComponentFlags gbuffer_write_mask(const PipelineKey& key) {
    return (key[68]&17)==17 ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT : 0;
}

// Whether a draw writes the RT material-record and albedo attachments: the G-buffer's draws, except logic-op ones (the op would apply to the record index).
// A key that without_logic_op converted (no logicOp on the device) is a blend key and so writes them; that only matters with ray tracing,
// which no device without logicOp (Metal) exposes today.
inline bool rt_material_write(const PipelineKey& key) {
    return gbuffer_write_mask(key) && (key[0]&3)!=2;
}

inline std::vector<PipelineKey> load_pipeline_keys(const std::filesystem::path& path) {
    std::vector<PipelineKey> keys;
    std::unique_ptr<FILE,int(*)(FILE*)> file(std::fopen(path.string().c_str(),"rb"),std::fclose);
    uint32_t header[3];
    if(!file || std::fread(header,sizeof header,1,file.get())!=1 || !std::equal(header,header+3,kPipelineKeysHeader)) return keys;
    for(PipelineKey key; std::fread(key.data(),sizeof key,1,file.get())==1;) keys.push_back(key);   // a torn trailing record is dropped
    return keys;
}

// Appends one key, creating the file (or replacing an unreadable one) with a fresh header. Returns false when it cannot be written.
inline bool append_pipeline_key(const std::filesystem::path& path,const PipelineKey& key) {
    std::unique_ptr<FILE,int(*)(FILE*)> probe(std::fopen(path.string().c_str(),"rb"),std::fclose);
    uint32_t header[3];
    const bool intact=probe && std::fread(header,sizeof header,1,probe.get())==1 && std::equal(header,header+3,kPipelineKeysHeader);
    probe.reset();
    std::unique_ptr<FILE,int(*)(FILE*)> file(std::fopen(path.string().c_str(),intact ? "ab" : "wb"),std::fclose);
    if(!file) return false;
    if(!intact && std::fwrite(kPipelineKeysHeader,sizeof kPipelineKeysHeader,1,file.get())!=1) return false;
    return std::fwrite(key.data(),sizeof key,1,file.get())==1 && std::fflush(file.get())==0;
}
}
