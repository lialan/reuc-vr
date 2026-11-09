#pragma once
#include "ppc_rt.h"
#include "tracked_ray.h"
#include <optional>
namespace uc {
// RBUE08 projectile initializer. The original executes first (ammo, spread,
// weapon flags and speed), then collision and visual trail share the VR ray.
bool align_xr_projectile(ppc::Ctx&,uint32_t object,unsigned player,const TrackedRay&);
class XrShotScope {
public:
    explicit XrShotScope(ppc::Ctx&);
    ~XrShotScope() noexcept(false);
    XrShotScope(const XrShotScope&)=delete;
private:
    ppc::Ctx& c_;
    uint32_t object_=0;
    unsigned player_=0;
    int exceptions_=0;
    std::optional<TrackedRay> ray_;
};
// The segment query 801C7800 from the pointer's ray (800F2B00) and from the item pickup (801AB354): along the VR controller's ray.
class XrPickScope {
public:
    explicit XrPickScope(ppc::Ctx&);
    ~XrPickScope() noexcept(false);
    XrPickScope(const XrPickScope&)=delete;
private:
    ppc::Ctx& c_;
    int exceptions_=0;
    uint32_t start_=0;
    std::array<float,3> saved_{};
};
struct NoXrShotScope {};
template<uint32_t entry> auto xr_shot_scope(ppc::Ctx& c) {
    if constexpr(entry==0x80069210u) return XrShotScope(c);
    else if constexpr(entry==0x801C7800u) return XrPickScope(c);
    else return NoXrShotScope{};
}
}
