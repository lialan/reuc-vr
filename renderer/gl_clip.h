#pragma once
// GL-convention column-major clip transform (HOTD4's matrices, xr/stereo_scene.hpp) from this renderer's Vulkan-ready row-major projection
// (gx::make_projection): the Y row's sign back and the depth range 0..w back to -w..w. Shared by the headset (xr_compositor) and the
// desktop stereo preview (UC_VR_PREVIEW), so it needs no OpenXR.
#include "../xr/stereo_scene.hpp"
#include "gx_transforms.h"

namespace uc {
inline xr::Matrix gl_from_vulkan(const gx::Projection& r) {
    xr::Matrix m{};
    for (int c = 0; c < 4; ++c) {
        m[c * 4 + 0] = r[0][c];
        m[c * 4 + 1] = -r[1][c];
        m[c * 4 + 2] = 2 * r[2][c] - r[3][c];
        m[c * 4 + 3] = r[3][c];
    }
    return m;
}
}  // namespace uc
