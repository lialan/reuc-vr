#include "check.h"
#include "gl_clip.h"
#include <cmath>

using namespace uc;
static void world_projection_keeps_eye_translation(float near_plane) {
    xr::StereoSettings settings; settings.scale=10;
    xr::StereoScene scene(settings);
    std::array<xr::StereoPose,2> eyes;
    eyes[0].position={-.032f,.17f,.19f}; eyes[1].position={.032f,.17f,.19f};
    eyes[0].tangents={1.3f,.8f,1.4f,.9f}; eyes[1].tangents={.8f,1.3f,1.4f,.9f};
    scene.publish(eyes);
    // Infinite-far perspective: for a tiny near plane, projecting z=-10 to
    // float clip space rounds z to w. Recovering view w from z-w then loses
    // the eye translation. Direct projection must retain it.
    xr::Matrix p{}; p[0]=1.2f; p[5]=1.6f; p[10]=-1; p[11]=-1; p[14]=-2*near_plane;
    const auto& matrices=scene.transforms(p,LG_STEREO_WORLD);
    float rows[32]; stereo_view_block(matrices,rows);
    const float point[4]={.5f,.25f,-10,1};
    for (int eye=0;eye<2;++eye) {
        float clip[4]{};
        for (int r=0;r<4;++r) for (int c=0;c<4;++c) clip[r]+=rows[16*eye+4*r+c]*point[c];
        const auto& t=eyes[eye].tangents;
        const double x=.5-double(eyes[eye].position[0])*10;
        const double y=.25-double(eyes[eye].position[1])*10;
        const double z=-10-double(eyes[eye].position[2])*10;
        const double expected_x=2*x/(t[0]+t[1])+(t[1]-t[0])*z/(t[0]+t[1]);
        const double expected_y=-(2*y/(t[2]+t[3])+(t[3]-t[2])*z/(t[2]+t[3]));
        CHECK(std::abs(clip[0]-expected_x)<2e-6);
        CHECK(std::abs(clip[1]-expected_y)<2e-6);
        CHECK(std::abs(clip[2]-(-z-near_plane))<2e-6);
        CHECK(std::abs(clip[3]+z)<2e-6);
    }
    // The pose cache must rebuild the direct transform after a new sample.
    eyes[0].position[2]+=.1f; scene.publish(eyes);
    const auto& moved=scene.transforms(p,LG_STEREO_WORLD);
    CHECK(std::abs(moved.from_view[0][15]-2.9f)<1e-6f);
}
int main() {
    world_projection_keeps_eye_translation(.1f);
    world_projection_keeps_eye_translation(1e-9f);
}
