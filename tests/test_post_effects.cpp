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
    draw.transforms.edit().projection[3][3]=0; CHECK(!gx::vr_screen_refraction(draw));
    draw.transforms.edit().projection[3][3]=1;
    copy->gpu_only=false; CHECK(!gx::vr_screen_refraction(draw)); copy->gpu_only=true;
    draw.uniforms.edit().ind_cmd[1]=0x100000; CHECK(!gx::vr_screen_refraction(draw)); draw.uniforms.edit().ind_cmd[1]=0;
    draw.uniforms.edit().ind_cmd[0]=0; CHECK(!gx::vr_screen_refraction(draw));
    return 0;
}
