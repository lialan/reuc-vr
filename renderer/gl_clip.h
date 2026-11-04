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
// Direct view -> Vulkan clip. Unlike clip reprojection, this needs C * E,
// with no inverse guest projection and no input clip-space conversion.
inline void stereo_view_block(const xr::Matrices& matrices, float* block) {
    for (int eye=0; eye<2; ++eye) for (int c=0; c<4; ++c) {
        const float* m=matrices.from_view[eye]+4*c;
        block[eye*16+c]=m[0]; block[eye*16+4+c]=-m[1];
        block[eye*16+8+c]=float(.5*(double(m[2])+m[3])); block[eye*16+12+c]=m[3];
    }
}
}  // namespace uc
