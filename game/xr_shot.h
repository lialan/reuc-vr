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
struct NoXrShotScope {};
template<uint32_t entry> auto xr_shot_scope(ppc::Ctx& c) {
    if constexpr(entry==0x80069210u) return XrShotScope(c);
    else return NoXrShotScope{};
}
}
