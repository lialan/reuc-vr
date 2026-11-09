#include "check.h"
#include "aim_geometry.h"
#include <cmath>
using namespace uc;
static gx::DrawPacket target(float z,float x=0) {
    gx::DrawPacket d; d.primitive=0x90;
    d.vertices.resize(3);
    d.vertices[0].position={x-2,-2,z}; d.vertices[1].position={x+2,-2,z}; d.vertices[2].position={x,2,z};
    d.indices={0,1,2}; return d;
}
static void close_target(float metres) {
    xr::StereoPose gun; gun.position={.2f,-.1f,-.15f};
    // Point from the actual hand origin at the centre of a nearby surface.
    const double dx=-gun.position[0],dy=-gun.position[1],dz=-metres-gun.position[2];
    const double length=std::sqrt(dx*dx+dy*dy+dz*dz),w=std::sqrt((1-dz/length)/2);
    gun.orientation={float(dy/length/(2*w)),float(-dx/length/(2*w)),0,float(w)};
    AimGeometry geometry; geometry.set_ray(1,gun,10);
    geometry.draw(target(-200)); geometry.draw(target(-10*metres)); geometry.draw(target(-300));
    CHECK(std::abs(geometry.depth(1,10)-metres)<1e-5f);
    CHECK(geometry.depth(0,10)==0);
    xr::StereoSettings settings; settings.scale=10;
    xr::StereoScene scene(settings); std::array<xr::StereoPose,2> eyes;
    eyes[0].position={-.032f,.08f,.1f}; eyes[1].position={.032f,.08f,.1f};
    eyes[0].tangents={1.3f,.8f,1.4f,.9f}; eyes[1].tangents={.8f,1.3f,1.4f,.9f};
    scene.publish(eyes);
    xr::StereoRay ray,old;
    CHECK(scene.aim(gun,1280,720,false,geometry.depth(1,10),ray));
    CHECK(std::abs(ray.x-640)<=1 && std::abs(ray.y-360)<=1);
    CHECK(scene.aim(gun,1280,720,false,15,old));
    if (metres<3) CHECK(std::abs(old.x-ray.x)>20); // catches the original fixed-plane miss
    scene.set_aim_depth(metres);
    const auto matrices=scene.transforms(xr::identity(),xr::LG_STEREO_HUD_AIM);
    for (int eye=0;eye<2;++eye) {
        const float center[]={0,0,0,1}; float reticle[2];
        CHECK(lg_stereo_project_xy(matrices.view_projection[eye],center,reticle));
        const float hit[]={0,0,-metres,1}; float actual[2];
        CHECK(lg_stereo_project_xy(scene.overlay_matrices().view_projection[eye],hit,actual));
        CHECK(std::abs(reticle[0]-actual[0])<1e-5f && std::abs(reticle[1]-actual[1])<1e-5f);
    }
    CHECK(ray.on_screen);
    // Far to the right of the picture: in the world it still aims, pinned to the right edge; on the menu screen it does not.
    xr::StereoPose aside=gun; const float a=1.2f; aside.orientation={0,-std::sin(a/2),0,std::cos(a/2)};
    xr::StereoRay off;
    CHECK(scene.aim(aside,1280,720,false,metres,off));
    CHECK(!off.on_screen && off.x==1279 && off.y>=0 && off.y<720);
    CHECK(!scene.aim(aside,1280,720,true,metres,off));
    geometry.reset(); CHECK(!geometry.active() && geometry.depth(1,10)==0);
}
int main() {
    close_target(.5f); close_target(1.f); close_target(2.f); close_target(15.f);
    AimGeometry geometry; xr::StereoPose gun; geometry.set_ray(0,gun,10);
    geometry.draw(target(10)); geometry.draw(target(-10,20)); CHECK(geometry.depth(0,10)==0);
    auto d=target(0); auto& t=d.transforms.edit(); t.current=3;
    t.position_rows[3]={1,0,0,0}; t.position_rows[4]={0,1,0,0}; t.position_rows[5]={0,0,1,-7};
    const std::array<float,6> box{-2,-2,0,2,2,0}; geometry.draw(d,&box); CHECK(std::abs(geometry.depth(0,10)-.7f)<1e-5f);
    geometry.reset(); geometry.set_ray(0,gun,10);
    d.state.edit().descriptors[0]=gx::Mode::direct;
    for (auto& v:d.vertices) v.position_matrix=3;
    t.current=0; geometry.draw(d); CHECK(std::abs(geometry.depth(0,10)-.7f)<1e-5f);
    geometry.reset(); geometry.set_ray(0,gun,10); d.indices={99,0,1}; geometry.draw(d); CHECK(geometry.depth(0,10)==0);
}
