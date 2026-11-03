#include "check.h"
#include "depth_efb.h"
#include "pipeline_keys.h"
// The GX pixel-engine state the renderer's depth/EFB switches read (renderer/depth_efb.h).
namespace {
uint32_t word(uint32_t first,uint32_t second,uint32_t combine,uint32_t ref0,uint32_t ref1) { return ref0|(ref1<<8)|(first<<16)|(second<<19)|(combine<<22); }
// The shader's test (smoke.slang shadeFragment): the alpha rounded to 0..255 against both references.
bool compare(uint32_t value,uint32_t reference,uint32_t op) {
    switch (op) { case 0: return false; case 1: return value<reference; case 2: return value==reference; case 3: return value<=reference;
                  case 4: return value>reference; case 5: return value!=reference; case 6: return value>=reference; default: return true; }
}
bool passes(uint32_t alpha,uint32_t w) {
    const bool a=compare(alpha,w&255,(w>>16)&7), b=compare(alpha,(w>>8)&255,(w>>19)&7);
    switch ((w>>22)&3) { case 0: return a&&b; case 1: return a||b; case 2: return a!=b; default: return a==b; }
}
void test_alpha_test_result() {
    // Exhaustive over ops and combiners with references at the ends and inside the range: a decided result must hold for every alpha.
    const uint32_t references[]={0,1,128,254,255};
    uint64_t decided=0;
    for (uint32_t first=0;first<8;++first) for (uint32_t second=0;second<8;++second) for (uint32_t combine=0;combine<4;++combine)
        for (uint32_t r0:references) for (uint32_t r1:references) {
            const uint32_t w=word(first,second,combine,r0,r1);
            const auto result=gx::alpha_test_result(w);
            bool any_pass=false, any_fail=false;
            for (uint32_t alpha=0;alpha<256;++alpha) (passes(alpha,w) ? any_pass : any_fail)=true;
            if (result==gx::AlphaTest::pass) CHECK(!any_fail);
            if (result==gx::AlphaTest::fail) CHECK(!any_pass);
            decided+=result!=gx::AlphaTest::undetermined;
            // without the pre-check the old rule (pipeline_keys.h): only ALWAYS/ALWAYS, not XOR, compiles the discard out
            CHECK(gx::alpha_test_needs_discard(w,false)==gx::alpha_test_can_discard(w));
            if (!gx::alpha_test_needs_discard(w,false)) CHECK(!gx::alpha_test_needs_discard(w,true));   // the pre-check only adds
        }
    CHECK(decided>0);
    CHECK(gx::alpha_test_result(word(6,7,0,0,0))==gx::AlphaTest::pass);      // GEQUAL 0 AND ALWAYS
    CHECK(gx::alpha_test_result(word(3,0,1,255,0))==gx::AlphaTest::pass);    // LEQUAL 255 OR NEVER
    CHECK(gx::alpha_test_result(word(4,7,0,255,0))==gx::AlphaTest::fail);    // GREATER 255 AND ALWAYS
    CHECK(gx::alpha_test_result(word(1,1,1,0,0))==gx::AlphaTest::fail);      // LESS 0 OR LESS 0
    CHECK(gx::alpha_test_result(word(7,7,2,0,0))==gx::AlphaTest::fail);      // ALWAYS XOR ALWAYS
    CHECK(gx::alpha_test_result(word(7,0,3,0,0))==gx::AlphaTest::fail);      // ALWAYS XNOR NEVER
    CHECK(gx::alpha_test_result(word(4,7,0,0,0))==gx::AlphaTest::undetermined);   // GREATER 0: the game's cut-outs
    CHECK(gx::alpha_test_result(word(2,7,1,0,0))==gx::AlphaTest::pass);      // EQUAL 0 OR ALWAYS
    CHECK(gx::alpha_test_result(word(2,5,0,9,9))==gx::AlphaTest::undetermined);
}
void test_pe_control() {
    CHECK(!gx::pe_control_known(31) && !gx::pe_z_top(31) && !gx::pe_no_alpha(31));   // no PE control: nothing known, nothing changed
    const uint32_t rgba6_top=31|gx::pe_control_bits(0x43000041u);   // RGBA6_Z24, z compare before texturing (the fight's common value)
    CHECK(gx::pe_control_known(rgba6_top) && gx::pe_pixel_format(rgba6_top)==gx::kRgba6Z24 && gx::pe_z_top(rgba6_top) && !gx::pe_no_alpha(rgba6_top));
    CHECK((rgba6_top&31)==31);   // the depth word's own bits stay
    const uint32_t rgb8_late=gx::pe_control_bits(0x43000000u);
    CHECK(gx::pe_pixel_format(rgb8_late)==gx::kRgb8Z24 && !gx::pe_z_top(rgb8_late) && gx::pe_no_alpha(rgb8_late));
    CHECK(gx::pe_no_alpha(gx::pe_control_bits(gx::kRgb565Z16)) && !gx::pe_no_alpha(gx::pe_control_bits(gx::kZ24)));
}
}
int main() {
    test_alpha_test_result();
    test_pe_control();
    std::puts("test_depth_efb: ok");
}
