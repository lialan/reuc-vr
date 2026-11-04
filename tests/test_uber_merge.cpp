#include "check.h"
#include "uber_merge.h"
#include <cstdio>

static uc::PipelineKey sample_key() {
    uc::PipelineKey key{};
    key[0]=1u|(4u<<2)|(5u<<5)|(1u<<12)|(1u<<13)|(1u<<14)|(1u<<16)|uc::kBankKeyBit;   // alpha-over, back culling, alpha test, bank
    key[1]=2; key[2]=0x1234; key[3]=0xABC; key[5]=0x48|uc::kTevDirectStage; key[9]=uc::kTevDirectStage; key[67]=1;
    key[68]=17|(3<<1); key[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    return key;
}

static void test_merge_key() {   // one pipeline per fixed-function state; the TEV words and the destination-alpha flag are read per draw
    const auto key=sample_key();
    const auto merged=uc::uber_merge_key(key,true);
    CHECK(merged[1]==uc::kUberFewStages);   // two stages: the family of draws with few stages
    for (size_t i=2;i<68;++i) CHECK(merged[i]==0);
    CHECK((merged[0]&uc::kUberMergeKeyBit)!=0 && (merged[0]&uc::kTexTableKeyBit)!=0);
    CHECK((merged[0]&uc::kBankKeyBit)==0);
    CHECK((merged[0]&(1u<<16))!=0 && ((merged[0]>>14)&3)==1);   // the alpha-test bit (late vs early Z module) and the cull bits stay
    CHECK(merged[68]==key[68] && merged[69]==key[69]);
    CHECK(!uc::valid_pipeline_key(merged,true));   // never recorded in a key file
    auto hud=key; hud[0]|=uc::kHudLayerKeyBit;
    CHECK((uc::uber_merge_key(hud,true)[0]&uc::kHudLayerKeyBit)!=0);   // the HUD layer's own blend attachments
    auto other=key; other[2]=0x4321; other[3]=0x777; other[1]=1; other[67]=0;
    CHECK(uc::uber_merge_key(other,true)==merged);
    auto many=key; many[1]=5;   // more stages: the general module (word 1 = 0), which every draw has (runs join on it)
    CHECK(uc::uber_merge_key(many,true)[1]==0 && uc::uber_merge_key(many,true)!=merged);
    CHECK(uc::uber_merge_key(key)[1]==0 && uc::uber_merge_key(key)==uc::uber_merge_key(many));
}

static void test_same_state() {   // draws of one ubershader run: same blend, cull, alpha-test bit, depth and topology
    const auto key=sample_key();
    auto tev=key; tev[3]^=0xFF; tev[1]=1;
    CHECK(uc::same_uber_state(key,tev));
    auto bank=key; bank[0]&=~uc::kBankKeyBit;
    CHECK(uc::same_uber_state(key,bank));
    auto cull=key; cull[0]^=3u<<14;
    CHECK(!uc::same_uber_state(key,cull));
    auto depth=key; depth[68]=1|(3<<1);
    CHECK(!uc::same_uber_state(key,depth));
    auto test=key; test[0]&=~(1u<<16);
    CHECK(!uc::same_uber_state(key,test));
    auto blend=key; blend[0]^=1u<<2;
    CHECK(!uc::same_uber_state(key,blend));
}

static void test_direct_stages() {   // the ubershader has no indirect path: only draws whose stages are all direct
    auto key=sample_key();
    CHECK(uc::uber_merge_stages_ok(key));
    key[9]=0x48;   // stage 1 reads an indirect coordinate
    CHECK(!uc::uber_merge_stages_ok(key));
    key[1]=1;      // only stage 0 counts
    CHECK(uc::uber_merge_stages_ok(key));
    key[1]=0;
    CHECK(uc::uber_merge_stages_ok(key));
}

static void test_fragment_uniforms() {   // what the headset's fragment stages read decides; transforms, lights, texgen rows and channels do not
    gx::DrawUniform a, b;
    CHECK(gx::fragment_uniforms_equal(a,b));
    b.tex_rows[4][1]=3; b.post_rows[2][0]=7; b.tex_gens[1]=99; b.tex_posts[3]=256; b.lights[2].color={1,2,3,4}; b.materials[0][0]=.5f; b.ambient[1][2]=.25f; b.channels[0]=3;
    CHECK(gx::fragment_uniforms_equal(a,b));
    const auto differs=[&](auto change) { gx::DrawUniform c=a; change(c); return !gx::fragment_uniforms_equal(a,c); };
    CHECK(differs([](gx::DrawUniform& u) { u.registers[1][2]=.5f; }));
    CHECK(differs([](gx::DrawUniform& u) { u.konst[3][3]=0; }));
    CHECK(differs([](gx::DrawUniform& u) { u.pixel[0]=0x7F0010; }));   // alpha test
    CHECK(differs([](gx::DrawUniform& u) { u.pixel[2]=256|128; }));    // destination alpha
    CHECK(differs([](gx::DrawUniform& u) { u.fog[3]=1u<<21; }));
    CHECK(differs([](gx::DrawUniform& u) { u.indirect[9]=5; }));
    CHECK(differs([](gx::DrawUniform& u) { u.ind_cmd[0]=1; }));
}

static void test_box_pixels() {   // the size test: the screen box clamped to the viewport, in guest pixels
    CHECK(gx::box_guest_pixels({-1,-1,1,1},640,480)==640.f*480.f);
    CHECK(gx::box_guest_pixels({-3,-2,5,4},640,480)==640.f*480.f);
    CHECK(gx::box_guest_pixels({0,0,.1f,.1f},640,480)>0 && gx::box_guest_pixels({0,0,.1f,.1f},640,480)<uc::kUberSmallPixels);
    CHECK(gx::box_guest_pixels({.5f,.5f,.4f,.6f},640,480)==0);   // empty
    CHECK(gx::box_guest_pixels({1.5f,-1,2,1},640,480)==0);        // off the viewport
}

int main() {
    test_merge_key();
    test_same_state();
    test_direct_stages();
    test_fragment_uniforms();
    test_box_pixels();
    std::puts("test_uber_merge: ok");
    return 0;
}
