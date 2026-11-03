#include "check.h"
#include "vr_cull.h"
#include <cmath>
using namespace uc;
// The headset's per-draw culling (renderer/vr_cull.h).
static const float kIdentity[16]={1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static void points_inside_are_kept() {
    const std::array<float,4> points[]={{-0.5f,0,0.5f,1},{0.5f,0.2f,0.5f,1}};
    CHECK(!clip_outside(kIdentity,points,2));
}
static void points_beyond_one_side_are_culled() {
    const std::array<float,4> right[]={{1.5f,0,0.5f,1},{3,-2,0.5f,1}};
    CHECK(clip_outside(kIdentity,right,2));
    const std::array<float,4> top[]={{0,2,0.5f,1},{-5,1.2f,0.5f,1}};
    CHECK(clip_outside(kIdentity,top,2));
}
static void a_hull_across_the_view_is_kept() {   // left and right of the view, but no single plane has all of them beyond it
    const std::array<float,4> across[]={{-3,0,0.5f,1},{3,0,0.5f,1}};
    CHECK(!clip_outside(kIdentity,across,2));
}
static void points_on_the_edge_are_kept() {   // the margin: a point exactly on a plane is not beyond it
    const std::array<float,4> edge[]={{1,0,0.5f,1},{1.005f,0,0.5f,1}};
    CHECK(!clip_outside(kIdentity,edge,2));
}
static void behind_the_eye_counts_as_beyond() {   // w < 0: x < -w and x > w cannot both fail, the hull lies behind the eye
    const std::array<float,4> behind[]={{0,0,0.5f,-1},{0.2f,0.1f,0.5f,-2}};
    CHECK(clip_outside(kIdentity,behind,2));
}
static void no_points_are_never_culled() { CHECK(!clip_outside(kIdentity,nullptr,0)); }
static void the_eye_transform_is_applied() {   // an eye shifted right by one NDC: a point at x=0.5 lands at 1.5, beyond its right side
    const float shifted[16]={1,0,0,1, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const std::array<float,4> point[]={{0.5f,0,0.5f,1}};
    CHECK(clip_outside(shifted,point,1));
    CHECK(!clip_outside(kIdentity,point,1));
}
static void extent_of_the_same_view_is_one() {
    const auto e=eye_extent(kIdentity);
    CHECK(std::abs(e[0]-1)<1e-5f && std::abs(e[1]-1)<1e-5f);
}
static void extent_of_a_wider_eye() {   // the eye's clip space halves x: the eye sees twice as wide as the game's view
    const float wide[16]={0.5f,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    const auto e=eye_extent(wide);
    CHECK(std::abs(e[0]-2)<1e-5f && std::abs(e[1]-1)<1e-5f);
}
int main() {
    points_inside_are_kept(); points_beyond_one_side_are_culled(); a_hull_across_the_view_is_kept(); points_on_the_edge_are_kept();
    behind_the_eye_counts_as_beyond(); no_points_are_never_culled(); the_eye_transform_is_applied(); extent_of_the_same_view_is_one(); extent_of_a_wider_eye();
    return 0;
}
