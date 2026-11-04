#include "check.h"
#include "ppc_fixture.h"
#include "xr_shot.h"
#include "native_input.h"
#include "controller_input.hpp"
#include <cmath>
#include <limits>

using V=std::array<float,3>;
constexpr uint32_t object=0x80010000,scene=0x80100000,camera=0x8044B4E0;
static void put(ppc::Ctx& c,uint32_t p,const V& v) {for(unsigned i=0;i<3;++i)ppc::wrf32(c,p+4*i,v[i]);}
static V get(ppc::Ctx& c,uint32_t p) {return {ppc::rdf32(c,p),ppc::rdf32(c,p+4),ppc::rdf32(c,p+8)};}
static void near(V a,V b,float eps=2e-5f) {for(unsigned i=0;i<3;++i)CHECK(std::abs(a[i]-b[i])<eps);}
static float dot(V a,V b) {float n=0;for(unsigned i=0;i<3;++i)n+=a[i]*b[i];return n;}
static void setup(ppc::Ctx& c,unsigned player=0) {
    c.r[13]=0x80500000;ppc::wr32(c,c.r[13]-27304,scene);
    // Camera at world (3,4,5), turned 90 degrees around Y:
    // view = (world.z-5, world.y-4, -world.x+3).
    put(c,camera,{0,0,1});ppc::wrf32(c,camera+12,-5);
    put(c,camera+16,{0,1,0});ppc::wrf32(c,camera+28,-4);
    put(c,camera+32,{-1,0,0});ppc::wrf32(c,camera+44,3);
    put(c,object+80,{3,4,5});put(c,object+92,{1,0,0});
    put(c,scene+player*7364+20180,{103,4,5});
    put(c,object+104,{.1f,.2f,.3f});
    ppc::wr32(c,object+136,0x1234);ppc::wr32(c,object+140,9);
}
static void collision_and_trail_start_at_the_laser() {
    Fix f;setup(f.c);
    const uc::TrackedRay ray{{2,-1,-4},{0,.6f,-.8f}};
    CHECK(uc::align_xr_projectile(f.c,object,0,ray));
    const V origin{7,3,7},direction{.8f,.6f,0};
    near(get(f.c,object+80),origin);near(get(f.c,object+92),direction);
    near(get(f.c,object+104),{});near(get(f.c,object+116),origin);
    for(unsigned i=0;i<4;++i)near(get(f.c,object+156+i*12),origin);
    CHECK(ppc::rd32(f.c,object+136)==0x1234 && ppc::rd32(f.c,object+140)==9);
    // Close and far collision points project onto the same view-space laser.
    for(float distance:{.2f,1.f,50.f}) {
        V p=origin;for(unsigned i=0;i<3;++i)p[i]+=distance*direction[i];
        const V view{p[2]-5,p[1]-4,-p[0]+3};
        V expected=ray.origin;for(unsigned i=0;i<3;++i)expected[i]+=distance*ray.direction[i];
        near(view,expected);
    }
}
static void spread_and_invalid_input() {
    Fix f;setup(f.c,1);put(f.c,object+92,{.8f,.6f,0});
    CHECK(uc::align_xr_projectile(f.c,object,1,{{0,0,0},{0,0,1}}));
    const auto pellet=get(f.c,object+92);
    CHECK(std::abs(dot(pellet,{-1,0,0})-.8f)<2e-5f && std::abs(dot(pellet,pellet)-1)<2e-5f);
    setup(f.c);ppc::wrf32(f.c,camera,2);
    CHECK(!uc::align_xr_projectile(f.c,object,0,{{0,0,0},{0,0,-1}}));
    near(get(f.c,object+80),{3,4,5});
    setup(f.c);
    CHECK(!uc::align_xr_projectile(f.c,object,0,{{0,0,0},{0,0,0}}));
    CHECK(!uc::align_xr_projectile(f.c,0xCC000000,0,{{0,0,0},{0,0,-1}}));
    CHECK(!uc::align_xr_projectile(f.c,object,2,{{0,0,0},{0,0,-1}}));
}
static void overlay_and_projectile_use_identical_rays() {
    uc::xr::StereoSettings settings;settings.scale=10;
    uc::xr::StereoScene scene(settings);
    std::array<uc::xr::StereoPose,2> eyes;scene.publish(eyes);
    uc::xr::ControllerSettings controls;controls.laser=1;
    uc::xr::ControllerInput input(controls);
    for(float depth:{.6f,2.f,15.f}) {
        uc::xr::ControllerSample sample;sample.valid={1,1};sample.aim_depth={depth,depth};
        sample.pose[0].position={-.2f,-.1f,-.15f};sample.pose[1].position={.2f,-.1f,-.15f};
        input.update(scene,sample,1000000000,true);
        uc::xr::ControllerSnapshot snapshot;CHECK(input.consume(snapshot));CHECK(snapshot.valid[0]);
        const auto& overlay=input.overlay_frames()[0];const auto& ray=snapshot.shot[0];
        float length=0;V delta{};
        for(unsigned i=0;i<3;++i){CHECK(std::abs(ray.origin[i]-overlay.muzzle[i]*10)<1e-6f);delta[i]=overlay.end[i]-overlay.muzzle[i];length+=delta[i]*delta[i];}
        for(float& v:delta)v/=std::sqrt(length);
        near(ray.direction,delta);
    }
}
static void hook_uses_the_consumed_controller_ray_only_for_player_shots() {
    Fix f;auto& c=f.c;uc::NativeInput input;input.focus(true);uc::input_hle_init(&input);
    input.host_xr_pointer(0,0,uc::TrackedRay{{2,-1,-4},{0,0,-1}});input.consume();
    setup(c);c.lr=0x8006B97C;c.r[3]=object;c.r[7]=0;
    {uc::XrShotScope hook(c);}
    near(get(c,object+80),{7,3,7});near(get(c,object+92),{1,0,0});
    setup(c);c.lr=0x80000000;c.r[3]=object;c.r[7]=0;
    {uc::XrShotScope hook(c);}
    near(get(c,object+80),{3,4,5});
    uc::input_hle_init(nullptr);c.lr=0x8006B97C;
    {uc::XrShotScope hook(c);}
    near(get(c,object+80),{3,4,5});
}
int main() {
    hook_uses_the_consumed_controller_ray_only_for_player_shots();
    collision_and_trail_start_at_the_laser();spread_and_invalid_input();overlay_and_projectile_use_identical_rays();
}
