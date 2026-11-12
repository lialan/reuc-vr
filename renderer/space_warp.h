#pragma once
// The headset's space warp (XR_FB_space_warp, xr_compositor.h): the depth image's near and far planes. The motion pass projects every
// world draw with the frame's world lens (its first depth-writing world draw), whose planes are recovered as xr/stereo_projection.c
// lg_stereo_eye_projection does, and submits them to the runtime in metres.
#include <cmath>
#include <optional>

namespace uc {
struct WarpDepthRange { float near_plane, far_plane; };
// The planes the motion pass uses, in metres, whatever the game's lenses: a frame's first world draw can be a distant backdrop with its own
// lens (near 40 m was seen), which would leave everything closer without depth or vectors. D24 resolves ~1 cm at 5 m with these.
constexpr WarpDepthRange kWarpPlanesMetres{.1f, 1000.f};
// p: a GL-convention perspective projection, column-major (gl_from_vulkan). In the projection's units; the near plane at least min_near
// (the game's world lens has its near plane at 1 mm: a 24-bit depth image then resolves ~1.5 m at 5 m, useless to the runtime); an
// infinite (or unusable) far plane becomes 10^4 times the near one, which the depth image cannot tell from infinity. None: not a
// perspective projection.
inline std::optional<WarpDepthRange> warp_depth_range(const float* p, float min_near = 0) {
    if (p[11] != -1.f || p[15] != 0.f) return std::nullopt;
    float n = p[14] / (p[10] - 1);
    const float f = p[14] / (p[10] + 1);
    if (!std::isfinite(n) || !(n > 0)) return std::nullopt;
    n = std::fmax(n, min_near);
    return WarpDepthRange{n, std::isfinite(f) && f > n ? f : n * 1e4f};
}
// p with its near and far planes replaced by range (the depth terms p[10], p[14]; lg_stereo_eye_projection reads only these and the
// perspective form): the lens the motion pass projects with.
inline void warp_projection(const float* p, WarpDepthRange range, float* out) {
    for (int i = 0; i < 16; ++i) out[i] = p[i];
    const float n = range.near_plane, f = range.far_plane;
    out[10] = -(f + n) / (f - n); out[14] = -2 * f * n / (f - n);
}
}  // namespace uc
