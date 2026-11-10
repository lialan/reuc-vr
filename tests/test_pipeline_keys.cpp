#include "check.h"
#include "pipeline_keys.h"
#include "texture_coordinates.h"
#include <fstream>

static void test_without_logic_op() {   // a GPU without logicOp (MoltenVK) draws the GX logic ops with blend equivalents
    uc::PipelineKey k{}; k[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    const uint32_t writes=(1u<<12)|(1u<<13);
    k[0]=2u|(1u<<2)|(0u<<5)|(0u<<8)|writes;   // CLEAR
    CHECK(uc::without_logic_op(k)[0]==(1u|(0u<<2)|(0u<<5)|(3u<<8)|writes));
    k[0]=2u|(1u<<2)|(0u<<5)|(5u<<8)|(1u<<12);   // NOOP, colour writes only
    CHECK(uc::without_logic_op(k)[0]==(1u|(0u<<2)|(1u<<5)|(3u<<8)|(1u<<12)));
    k[0]=2u|(1u<<2)|(0u<<5)|(6u<<8)|writes;   // XOR: no equivalent, a plain write
    CHECK(uc::without_logic_op(k)[0]==(0u|(1u<<2)|(0u<<5)|(3u<<8)|writes));
    CHECK(uc::valid_pipeline_key(uc::without_logic_op(k),false));
    k[0]=1u|(4u<<2)|(5u<<5)|(3u<<8)|writes;   // an ordinary blend is left alone
    CHECK(uc::without_logic_op(k)==k);
}
static void test_alpha_test_bit() {   // pipelines whose draws cannot fail the GX alpha test compile their discard out (early depth rejection)
    const auto word=[](uint32_t first,uint32_t second,uint32_t combine) { return (first<<16)|(second<<19)|(combine<<22)|0x8040u; };
    CHECK(!gx::alpha_test_can_discard(word(7,7,0)));   // ALWAYS and ALWAYS: every fragment passes
    CHECK(!gx::alpha_test_can_discard(word(7,7,1)));
    CHECK(!gx::alpha_test_can_discard(word(7,7,3)));
    CHECK(gx::alpha_test_can_discard(word(7,7,2)));    // XOR of two passes rejects everything
    CHECK(gx::alpha_test_can_discard(word(6,7,0)));    // GEQUAL: a real test
    CHECK(gx::alpha_test_can_discard(word(7,4,1)));
    CHECK(gx::alpha_test_can_discard(word(0,0,0)));    // NEVER
    uc::PipelineKey k{}; k[0]=2u|(0u<<8)|(1u<<12)|(1u<<16); k[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    CHECK((uc::without_logic_op(k)[0]>>16)==1u);       // the bit survives the logic-op conversion
    CHECK(uc::valid_pipeline_key(uc::without_logic_op(k),false));
    k[0]|=1u<<17; CHECK(uc::valid_pipeline_key(k,true));    // bit 17: per-vertex texture matrices (the headset's bank vertex stage)
    CHECK((uc::without_logic_op(k)[0]>>17&1)==1u);
    k[0]|=uc::kHudLayerKeyBit; CHECK(uc::valid_pipeline_key(k,true));   // bit 18: the headset's HUD layer variant
    k[0]|=uc::kParticleLayerKeyBit; CHECK(uc::valid_pipeline_key(k,true));   // bit 19: the particle layer variant
    k[0]|=1u<<20; CHECK(!uc::valid_pipeline_key(k,true));   // nothing above bit 19
}
// The headset's direct-stage marks (order word bit 16): a key recorded before them reads as all-direct, a marked one stays as it is.
void test_headset_direct_key() {
    uc::PipelineKey k{}; k[1]=3; k[5]=0x48; k[9]=0x50; k[13]=0x41; k[17]=0x7;   // three stages (the fourth order word is past the count)
    const auto all=uc::headset_direct_key(k);
    for (int i=0;i<3;++i) CHECK((all[5+4*i]&uc::kTevDirectStage)!=0 && (all[5+4*i]&0xFFFF)==(k[5+4*i]&0xFFFF));
    CHECK(all[17]==k[17]);
    uc::PipelineKey mixed=k; mixed[9]|=uc::kTevDirectStage;   // stage 1 direct, stages 0 and 2 read an indirect coordinate
    CHECK(uc::headset_direct_key(mixed)==mixed);
    uc::PipelineKey none{}; CHECK(uc::headset_direct_key(none)==none);   // no stages: nothing to mark
}
static void test_indirect_keys() {
    uc::PipelineKey base{}; base[1]=2; base[5]=0x148; base[6]=0x2AB; base[9]=0x50; base[10]=0x155;
    base[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    // A shader must distinguish the complete command and the selected IREF pair,
    // but not unused pairs. Its ordinary TEV selectors must survive unchanged.
    for (uint32_t stage=0;stage<2;++stage) for (uint32_t indirect=0;indirect<4;++indirect) {
        const uint32_t command=(0x1ABCDEu&~3u)|indirect;
        const uint32_t references=0x654321;
        auto key=base; uc::specialize_indirect_stage(key,stage,command,references);
        CHECK(uc::valid_pipeline_key(key,true));
        CHECK(uc::headset_direct_key(key)==key); // no direct stage: must not be mistaken for a legacy key
        CHECK((key[5+4*stage]&0xFFFFu)==base[5+4*stage]);
        CHECK((key[6+4*stage]&1023u)==base[6+4*stage]);
        CHECK((key[6+4*stage]>>10)==command);
        CHECK(((key[5+4*stage]>>18)&63u)==((references>>(6*indirect))&63u));
        CHECK(uc::dynamic_indirect_key(key)==base);
        auto unused=base; uc::specialize_indirect_stage(unused,stage,command,references^(63u<<(6*((indirect+1)%4))));
        CHECK(unused==key);
        auto changed=base; uc::specialize_indirect_stage(changed,stage,command,references^(1u<<(6*indirect)));
        CHECK(changed!=key);
        for (uint32_t bit=2;bit<21;++bit) {
            changed=base; uc::specialize_indirect_stage(changed,stage,command^(1u<<bit),references);
            CHECK(changed!=key);
        }
    }
    auto direct=base; uc::specialize_indirect_stage(direct,0,0,0xFFFFFF); CHECK(direct==base);
    // Preserve the dynamic path's behavior for reserved IND_CMD bits instead of truncating them.
    uc::specialize_indirect_stage(direct,0,1u<<21,0xFFFFFF); CHECK(direct==base);
    auto untextured=base; untextured[5]&=~64u; direct=untextured;
    uc::specialize_indirect_stage(untextured,0,0x1FFFFF,0xFFFFFF); CHECK(untextured==direct);
}
static void test_xform_coordinates() {
    uc::PipelineKey k{};
    CHECK(uc::xform_coordinate_mask(k)==1); // no stages: evaluateTev reads texture 0 / coordinate 0
    k[1]=2; k[5]=64|(2u<<3)|uc::kTevDirectStage;
    k[9]=64|(5u<<3)|uc::kTevDirectStage;
    CHECK(uc::xform_coordinate_mask(k)==((1u<<2)|(1u<<5)));
    k[9]&=~uc::kTevDirectStage;
    CHECK(uc::xform_coordinate_mask(k)==255); // unknown indirect reference remains dynamic
    uc::specialize_indirect_stage(k,1,1,7u<<9); // indirect stage 1 reads coordinate 7
    CHECK(uc::xform_coordinate_mask(k)==((1u<<2)|(1u<<5)|(1u<<7)));
    k[5]&=~64u;
    CHECK(uc::xform_coordinate_mask(k)==((1u<<5)|(1u<<7))); // untextured stage does not read coordinates
    k[9]&=~64u;
    CHECK(uc::xform_coordinate_mask(k)==0);
}
static void test_affine_coordinates() {
    gx::DrawUniform u;
    CHECK(gx::unit_texture_q(u,255)); // regular ST and identity post matrices
    u.tex_gens[2]|=2;
    CHECK(!gx::unit_texture_q(u,1u<<2)); // STQ can carry perspective Q
    CHECK(gx::unit_texture_q(u,1u<<0)); // an unused projected coordinate does not split the shader
    u.post_rows[8]={0,0,0,1};
    CHECK(gx::unit_texture_q(u,1u<<2)); // constant post Q replaces the projected Q
    u.tex_posts[2]=256;
    CHECK(!gx::unit_texture_q(u,1u<<2)); // retain normalization's general path
    u.tex_posts[2]=0; u.tex_gens[2]&=~2u;
    u.post_rows[8]={0.1f,0,1,0}; CHECK(!gx::unit_texture_q(u,1u<<2));
    u.post_rows[8]={0,0,2,0}; CHECK(!gx::unit_texture_q(u,1u<<2));
    u.post_rows[8]={0,0,0,0}; CHECK(!gx::unit_texture_q(u,1u<<2));
    u.post_rows[8]={0,0,1,0}; u.tex_gens[2]|=1u<<4;
    CHECK(!gx::unit_texture_q(u,1u<<2)); // unsupported/emboss texgen types stay general
    uc::PipelineKey k{};
    auto affine=uc::affine_coordinates_key(k);
    CHECK(affine[5]==uc::kTevAffineCoordinates && uc::general_coordinates_key(affine)==k);
    CHECK(uc::affine_coordinates_key(affine)==affine);
    k[1]=2; k[5]=64|uc::kTevDirectStage; k[9]=64|uc::kTevIndirectStage|(2u<<21);
    affine=uc::affine_coordinates_key(k);
    CHECK(uc::general_coordinates_key(affine)==k);
    CHECK(uc::xform_coordinate_mask(affine)==((1u<<0)|(1u<<2)));
    k[5]&=~64u; k[9]&=~64u;
    CHECK(uc::affine_coordinates_key(k)==k); // no texture reads: no new variant
}
int main() {
    test_affine_coordinates();
    test_xform_coordinates();
    test_without_logic_op();
    test_alpha_test_bit();
    test_headset_direct_key();
    test_indirect_keys();
    const auto path=std::filesystem::temp_directory_path()/"uc_test_pipeline_keys.bin";
    std::filesystem::remove(path);
    CHECK(uc::load_pipeline_keys(path).empty());
    uc::PipelineKey a{}, b{};
    a[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; b=a; b[1]=4; b[3]=0xABCD;
    CHECK(uc::append_pipeline_key(path,a));
    CHECK(uc::append_pipeline_key(path,b));
    auto keys=uc::load_pipeline_keys(path);
    CHECK(keys.size()==2 && keys[0]==a && keys[1]==b);
    { std::ofstream(path,std::ios::binary|std::ios::app).write("torn",4); }   // torn trailing record
    CHECK(uc::load_pipeline_keys(path).size()==2);
    { std::ofstream(path,std::ios::binary|std::ios::trunc).write("garbage garbage garbage",23); }   // wrong header
    CHECK(uc::load_pipeline_keys(path).empty());
    CHECK(uc::append_pipeline_key(path,a));
    CHECK(uc::load_pipeline_keys(path).size()==1);
    std::filesystem::remove(path);

    // The ubershader pipeline depends on the fixed-function words only.
    uc::PipelineKey c=a; c[0]=0x1234; c[68]=9; c[1]=3; c[2]=7; c[66]=5; c[67]=1;
    uc::PipelineKey d=c; d[1]=9; d[40]=77; d[67]=0;
    CHECK(uc::uber_key(c)==uc::uber_key(d));
    CHECK(uc::uber_key(c)[0]==0x1234 && uc::uber_key(c)[68]==9 && uc::uber_key(c)[69]==VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST && uc::uber_key(c)[1]==0 && uc::uber_key(c)[67]==0);
    CHECK(uc::uber_key(c)!=uc::uber_key([&]{ auto e=c; e[0]^=1; return e; }()));

    // Cull and the depth state are dynamic: keys differing only there share a pipeline; whether the draw writes depth stays in it.
    uc::PipelineKey s=a; s[0]=1|(1u<<14); s[68]=17|(3<<1);
    uc::PipelineKey t=s; t[0]=1|(2u<<14); t[68]=17|(6<<1);
    CHECK(uc::static_key(s)==uc::static_key(t) && uc::static_key(s)[68]==17 && (uc::static_key(s)[0]>>14)==0 && (uc::static_key(s)[0]&0x3FFF)==1);
    t[68]=1|(6<<1); CHECK(uc::static_key(t)[68]==0 && uc::static_key(s)!=uc::static_key(t));
    CHECK(uc::cull_mode(s)==VK_CULL_MODE_BACK_BIT && uc::cull_mode(a)==VK_CULL_MODE_NONE && uc::static_key(uc::static_key(s))==uc::static_key(s));

    // The ubershader closure combines every blend/cull word, depth word and topology seen, so a new TEV key never needs a synchronous compile.
    uc::PipelineKey f=a, g=a, h=a; f[0]=1; f[68]=1; g[0]=2; g[68]=17; h[0]=1; h[68]=1; h[69]=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; h[1]=5;
    const auto closure=uc::uber_key_closure({f,g,h});
    CHECK(closure.size()==2*2*2);
    CHECK(std::find(closure.begin(),closure.end(),uc::uber_key(h))!=closure.end());
    auto cross=a; cross[0]=2; cross[68]=1; CHECK(std::find(closure.begin(),closure.end(),cross)!=closure.end());   // blend word of g with the depth word of f
    CHECK(uc::uber_key_closure({}).empty());

    // Motion vectors are written only by draws that write depth (compare + update), so blended effects keep the vectors underneath.
    uc::PipelineKey z=a; z[68]=17|(3<<1);
    CHECK(uc::motion_write_mask(z)==(VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT));
    z[68]=16; CHECK(uc::motion_write_mask(z)==0);   // update without compare: Vulkan writes no depth either
    z[68]=1; CHECK(uc::motion_write_mask(z)==0);
    z[68]=5; CHECK(uc::motion_write_mask(z)==0);    // destination-alpha second pass
    // The G-buffer follows the same rule: only draws that write depth define a surface.
    z[68]=17|(3<<1); CHECK(uc::gbuffer_write_mask(z)==(VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT));
    z[68]=16; CHECK(uc::gbuffer_write_mask(z)==0);
    z[68]=1; CHECK(uc::gbuffer_write_mask(z)==0);
    CHECK(uc::valid_pipeline_key(a,false));
    auto bad=a; bad[69]=0; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[1]=17; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[68]=32; CHECK(!uc::valid_pipeline_key(bad,false));
    bad=a; bad[0]=2; CHECK(!uc::valid_pipeline_key(bad,false) && uc::valid_pipeline_key(bad,true));
}
