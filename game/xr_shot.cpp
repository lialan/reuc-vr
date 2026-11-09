#include "xr_shot.h"
#include "native_input.h"
#include "gx_vertices.h"
#include <algorithm>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
namespace uc {
namespace {
using V=std::array<float,3>;
V sub(V a,const V& b) { for(unsigned i=0;i<3;++i)a[i]-=b[i];return a; }
V cross(const V& a,const V& b) { return {a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; }
float dot(const V& a,const V& b) {return a[0]*b[0]+a[1]*b[1]+a[2]*b[2];}
bool normalize(V& v) { const float n=std::sqrt(dot(v,v));if(!std::isfinite(n)||n<1e-6f)return false;for(float& x:v)x/=n;return true; }
V read(ppc::Ctx& c,uint32_t p) {return {ppc::rdf32(c,p),ppc::rdf32(c,p+4),ppc::rdf32(c,p+8)};}
void write(ppc::Ctx& c,uint32_t p,const V& v) {for(unsigned i=0;i<3;++i)ppc::wrf32(c,p+i*4,v[i]);}
// Rotate a pellet with its cone, preserving the game's spread around the
// original central ray. A zero-spread bullet becomes exactly the laser ray.
V redirect(const V& pellet,const V& from,const V& to) {
    const float cosine=std::clamp(dot(from,to),-1.f,1.f);
    if(cosine<-.9999f) {
        V axis=cross(from,std::abs(from[0])<.8f ? V{1,0,0} : V{0,1,0});normalize(axis);
        V out;for(unsigned i=0;i<3;++i)out[i]=2*dot(axis,pellet)*axis[i]-pellet[i];return out;
    }
    const V v=cross(from,to),a=cross(v,pellet),b=cross(v,a);
    V out;for(unsigned i=0;i<3;++i)out[i]=pellet[i]+a[i]+b[i]/(1+cosine);return out;
}
gx::Memory guest_memory(const ppc::Ctx& c) {return {{reinterpret_cast<const std::byte*>(c.mem),24u<<20},{reinterpret_cast<const std::byte*>(c.mem+0x10000000),64u<<20},{}};}
// The tracked ray (guest camera space) in the world, through the game's view matrix at 8044B4E0; false for a matrix that is not a rotation.
bool world_ray(ppc::Ctx& c,const TrackedRay& ray,V& origin,V& direction) {
    std::array<V,3> rows;V translation;
    for(unsigned i=0;i<3;++i){rows[i]=read(c,0x8044B4E0+i*16);translation[i]=ppc::rdf32(c,0x8044B4E0+i*16+12);}
    for(unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j)
        if(!std::isfinite(dot(rows[i],rows[j])) || std::abs(dot(rows[i],rows[j])-(i==j?1.f:0.f))>.02f)return false;
    origin={};direction={};
    for(unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j) {
        origin[i]+=rows[j][i]*(ray.origin[j]-translation[j]);
        direction[i]+=rows[j][i]*ray.direction[j];
    }
    return true;
}
// Explicit harness-only ray override (UC_XR_TEST_SHOT, guest camera space); ordinary demos have no tracked ray.
const std::optional<TrackedRay>& diagnostic_ray() {
    static const auto ray=[]()->std::optional<TrackedRay> {
        const char* s=std::getenv("UC_XR_TEST_SHOT");if(!s)return {};
        TrackedRay r;auto& o=r.origin;auto& d=r.direction;
        if(std::sscanf(s,"%f,%f,%f,%f,%f,%f",&o[0],&o[1],&o[2],&d[0],&d[1],&d[2])!=6 || !normalize(d))
            throw std::runtime_error("UC_XR_TEST_SHOT requires origin xyz and direction xyz");
        return r;
    }();
    return ray;
}
}
bool align_xr_projectile(ppc::Ctx& c,uint32_t object,unsigned player,const TrackedRay& ray) {
    if(!c.mem || player>1)return false;
    const auto memory=guest_memory(c);
    try {
        memory.read(object,204);memory.read(c.r[13]-27304,4);memory.read(0x8044B4E0,48);
        const uint32_t scene=ppc::rd32(c,c.r[13]-27304),target=scene+player*7364+20180;
        memory.read(target,12);
        V origin,direction;
        if(!world_ray(c,ray,origin,direction))return false;
        V old_origin=read(c,object+80),old_direction=read(c,object+92),central=sub(read(c,target),old_origin);
        if(!normalize(central)||!normalize(direction)||!normalize(old_direction))return false;
        for(float v:origin)if(!std::isfinite(v))return false;
        V aimed=redirect(old_direction,central,direction);if(!normalize(aimed))return false;
        // 80069580 collision sweeps object+80 along +92; 8006B04C draws at
        // +80 plus +104. Remove the flat-screen muzzle offset and reset every
        // trail sample, so the very first visible segment follows that sweep.
        write(c,object+80,origin);write(c,object+92,aimed);write(c,object+104,{});write(c,object+116,origin);
        for(unsigned i=0;i<4;++i)write(c,object+156+i*12,origin);
        if(std::getenv("UC_XR_SHOT_LOG")) {
            const float error=std::sqrt(dot(sub(aimed,direction),sub(aimed,direction)));
            std::fprintf(stderr,"[xr-shot] object %08X player %u origin %.5f,%.5f,%.5f direction %.6f,%.6f,%.6f laser %.6f,%.6f,%.6f spread %.7f old-origin %.3f,%.3f,%.3f\n",object,player,origin[0],origin[1],origin[2],aimed[0],aimed[1],aimed[2],direction[0],direction[1],direction[2],error,old_origin[0],old_origin[1],old_origin[2]);
        }
        return true;
    } catch(const gx::Error&) {return false;}
}
XrShotScope::XrShotScope(ppc::Ctx& c):c_(c),exceptions_(std::uncaught_exceptions()) {
    if(c.lr!=0x8006B97Cu || c.r[7]>1)return;
    object_=c.r[3];player_=c.r[7];ray_=input_xr_ray(player_);
    if(const auto& diagnostic=diagnostic_ray())ray_=diagnostic;
}
XrShotScope::~XrShotScope() noexcept(false) {
    if(ray_ && std::uncaught_exceptions()==exceptions_ && !align_xr_projectile(c_,object_,player_,*ray_) && std::getenv("UC_XR_SHOT_LOG"))
        std::fprintf(stderr,"[xr-shot] rejected invalid projectile/camera %08X\n",object_);
}
}

namespace uc {
XrPickScope::XrPickScope(ppc::Ctx& c):c_(c),exceptions_(std::uncaught_exceptions()) {
    // 801C7800(r4 start, r5 end, r6 categories): the segment query. From 800F2B00 (mask 23) it is the pointer's ray, eye to the unprojected
    // pointer, whose hit point (+20180) and flags (+20204) decide what the player aims at; from 801AB354 (mask 4) it picks the item to take,
    // eye to that hit point. The VR controller aims from the hand: both run along its ray instead, so the item the laser passes through is
    // the one hit (the eye's ray through the 2D pointer passed beside it).
    const bool pointer=c.lr==0x800F2C74u,pickup=c.lr==0x801AB464u;
    if((!pointer && !pickup) || !c.mem)return;
    const auto memory=guest_memory(c);
    try {
        memory.read(c.r[4],12);memory.read(c.r[5],12);memory.read(c.r[13]-27304,4);memory.read(0x8044B4E0,48);
        const uint32_t scene=ppc::rd32(c,c.r[13]-27304);
        const V end=read(c,c.r[5]);
        for(unsigned p=0;p<2;++p) {
            const uint32_t target=scene+p*7364+20180;memory.read(target,24);
            if(pointer ? c.r[5]!=target : read(c,target)!=end)continue;   // the pointer query writes the player's hit point; the pickup reads a copy of it
            auto ray=input_xr_ray(p);if(diagnostic_ray())ray=diagnostic_ray();
            V origin,direction;
            if(!ray || !world_ray(c,*ray,origin,direction) || !normalize(direction))return;
            for(float v:origin)if(!std::isfinite(v))return;
            start_=c.r[4];saved_=read(c,start_);
            write(c,start_,origin);
            if(pointer) {
                const V d=sub(end,saved_);const float length=std::sqrt(dot(d,d));
                if(!std::isfinite(length) || length<=0)return;
                V far;for(unsigned i=0;i<3;++i)far[i]=origin[i]+direction[i]*length;
                write(c,c.r[5],far);write(c,target+12,far);   // +20192: the unclipped far point, read by the hotspots and the reticle
            }
            if(std::getenv("UC_XR_SHOT_LOG"))
                std::fprintf(stderr,"[xr-pick] %s player %u eye %.3f,%.3f,%.3f -> ray %.3f,%.3f,%.3f dir %.4f,%.4f,%.4f\n",pointer ? "pointer" : "pickup",p,
                             saved_[0],saved_[1],saved_[2],origin[0],origin[1],origin[2],direction[0],direction[1],direction[2]);
            return;
        }
    } catch(const gx::Error&) {start_=0;}
}
XrPickScope::~XrPickScope() noexcept(false) {
    if(start_ && std::uncaught_exceptions()==exceptions_)write(c_,start_,saved_);   // the caller's eye point, as it was
}
}
