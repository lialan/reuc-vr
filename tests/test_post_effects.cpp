#include "check.h"
#include "post_effects.h"
int main() {
    gx::DrawPacket draw;
    draw.callsite = 0x800421B8; draw.blend.mode = 1; draw.blend.source = 1; draw.blend.destination = 1;
    CHECK(gx::bloom_composite(draw));
    draw.blend.destination = 4; CHECK(!gx::bloom_composite(draw));   // the blur taps of the chain blend source alpha
    draw.blend.destination = 1; draw.blend.mode = 0; CHECK(!gx::bloom_composite(draw));
    draw.blend.mode = 1; draw.callsite = 0x80393B0C; CHECK(!gx::bloom_composite(draw));   // other additive draws (sprites, flashes) stay
    // A screen-space scene-copy distortion is adapted for VR; ordinary bloom,
    // mask textures, perspective particles and dependent indirect chains stay.
    draw.transforms.edit().projection[3][3]=1;
    draw.blend.source=4; draw.blend.destination=5;
    draw.material.stages=3; draw.material.stage[0].order=64;
    draw.uniforms.edit().ind_cmd[0]=0x210;
    auto copy=std::make_shared<gx::TexturePixels>(); copy->gpu_only=true;
    draw.texture=copy;
    CHECK(gx::vr_screen_refraction(draw));
    CHECK(gx::effective_indirect_command(*draw.uniforms,0,gx::vr_screen_refraction(draw))==0);
    CHECK(gx::effective_indirect_command(*draw.uniforms,0,false)==0x210); // desktop/original effect
    draw.transforms.edit().projection[3][3]=0; CHECK(!gx::vr_screen_refraction(draw));
    CHECK(gx::effective_indirect_command(*draw.uniforms,0,gx::vr_screen_refraction(draw))==0x210); // world particles
    draw.transforms.edit().projection[3][3]=1;
    copy->gpu_only=false; CHECK(!gx::vr_screen_refraction(draw)); copy->gpu_only=true;
    draw.uniforms.edit().ind_cmd[1]=0x100000; CHECK(!gx::vr_screen_refraction(draw));
    CHECK(gx::effective_indirect_command(*draw.uniforms,1,true)==0x100000); // later stages never overridden
    draw.uniforms.edit().ind_cmd[1]=0;
    draw.uniforms.edit().ind_cmd[0]=0; CHECK(!gx::vr_screen_refraction(draw));
    // Texture sizes follow the effective shader's map reads, not every bound
    // texture. Coordinate bits in IREF are not texture map bits.
    draw.material.stages=2;
    draw.material.stage[0].order=64|2;
    draw.material.stage[1].order=64|7;
    draw.uniforms.edit().indirect[9]=(5u|(6u<<3))<<12;
    draw.uniforms.edit().ind_cmd[0]=2; // IREF entry 2, map 5, coordinate 6
    CHECK(gx::indirect_size_maps(draw,false)==((1u<<2)|(1u<<5)));
    CHECK(gx::indirect_size_maps(draw,true)==0); // adapted first stage is direct
    draw.uniforms.edit().ind_cmd[1]=2;
    CHECK(gx::indirect_size_maps(draw,true)==((1u<<7)|(1u<<5)));
    CHECK(gx::indirect_size_maps(draw,false)==((1u<<2)|(1u<<7)|(1u<<5)));
    draw.material.stage[0].order&=~64u;
    CHECK(gx::indirect_size_maps(draw,false)==((1u<<7)|(1u<<5))); // disabled texture stage
    draw.material.stages=0;
    CHECK(gx::indirect_size_maps(draw,false)==0); // no-stage texture-zero path has no indirect math
    return 0;
}
