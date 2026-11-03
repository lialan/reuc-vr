#pragma once
// The headset's per-draw culling (vulkan_renderer.cpp): the game culls against its own (widened) frustum, so it still sends draws that
// neither eye sees. A draw whose points, carried into an eye's clip space, all lie beyond one same side plane (x or y) of that eye's
// clip volume cannot put a pixel into that eye: the convex hull of the points lies beyond the plane too. Near and far are not tested
// (the headset's pipelines clamp depth instead of clipping it). Conservative: a point counts as beyond a plane only by a margin.
#include <algorithm>
#include <array>
#include <cstddef>

namespace uc {
// rows: the eye's 4x4 transform from the game's clip space, row-major (the shader's eye block). points: game clip-space points (x, y, z, w).
inline bool clip_outside(const float* rows, const std::array<float,4>* points, size_t count) {
    bool left = true, right = true, bottom = true, top = true;
    for (size_t i = 0; i < count && (left || right || bottom || top); ++i) {
        const auto& p = points[i];
        float c[4];
        for (int r = 0; r < 4; ++r) c[r] = rows[r * 4] * p[0] + rows[r * 4 + 1] * p[1] + rows[r * 4 + 2] * p[2] + rows[r * 4 + 3] * p[3];
        const float w = c[3] + (c[3] < 0 ? -c[3] : c[3]) * 0.01f + 1e-4f;   // the margin
        if (c[0] >= -w) left = false;
        if (c[0] <= w) right = false;
        if (c[1] >= -w) bottom = false;
        if (c[1] <= w) top = false;
    }
    return count && (left || right || bottom || top);
}
// How far an eye's view reaches in the game's clip space: the largest |x/w| and |y/w| of the eye's clip-volume corners at a few depths
// (Vulkan z 0.999, 0.9999, 1: far from the eye, where its offset from the game's camera no longer matters), carried back through the inverse of rows. Below 1 the game's (widened) view
// covers the eye; the game's widening could shrink by that factor. {0, 0} when rows cannot be inverted.
inline std::array<float,2> eye_extent(const float* rows) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? rows[r * 4 + c] : (c - 4 == r ? 1 : 0);
    for (int c = 0; c < 4; ++c) {   // Gauss-Jordan with partial pivoting
        int pivot = c;
        for (int r = c + 1; r < 4; ++r) if ((a[r][c] < 0 ? -a[r][c] : a[r][c]) > (a[pivot][c] < 0 ? -a[pivot][c] : a[pivot][c])) pivot = r;
        if (a[pivot][c] == 0) return {0, 0};
        for (int k = 0; k < 8; ++k) { const double t = a[c][k]; a[c][k] = a[pivot][k]; a[pivot][k] = t; }
        const double d = a[c][c];
        for (int k = 0; k < 8; ++k) a[c][k] /= d;
        for (int r = 0; r < 4; ++r) if (r != c) { const double f = a[r][c]; for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k]; }
    }
    float ex = 0, ey = 0;
    for (double z : {0.999, 0.9999, 1.0}) for (int corner = 0; corner < 4; ++corner) {
        const double p[4] = {corner & 1 ? 1.0 : -1.0, corner & 2 ? 1.0 : -1.0, z, 1.0};
        double g[4];
        for (int r = 0; r < 4; ++r) g[r] = a[r][4] * p[0] + a[r][5] * p[1] + a[r][6] * p[2] + a[r][7] * p[3];
        if (g[3] <= 0) continue;
        ex = std::max(ex, float((g[0] < 0 ? -g[0] : g[0]) / g[3])); ey = std::max(ey, float((g[1] < 0 ? -g[1] : g[1]) / g[3]));
    }
    return {ex, ey};
}
}
