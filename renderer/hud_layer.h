#pragma once
// The headset's HUD layer (VulkanRenderer, the Quest): the game's 2D HUD drawn into a small two-layer target (one layer per eye) at about
// the eyes' pixel density over the HUD's own viewport, instead of into both eyes of the EFB at the eye resolution with the EFB's tiles, then
// laid over each eye by the screen pass (xr_screen.slang) or, for a frame whose EFB is read afterwards (a capture, the desktop preview), by a
// composite pass into the EFB.
//
// Why it is the same picture: every HUD-class draw (lg_stereo_classify_draw) is reprojected into an eye by HOTD4's screen plane
// (stereo_projection.c E*H*S, independent of the draw's own 2D projection; the draw's clip depth moves its plane by up to 2% of its distance,
// which the eyes see as a little parallax), and a 2D draw has w = 1, so an eye sees the plane of the first HUD draw as a projective map (a
// homography H) of the game's clip plane. Each eye's layer holds the HUD as that eye sees it, in the coordinates of that plane: a draw is
// drawn with H^-1 * (its own eye transform), the identity for draws at the first one's depth. The layer keeps two images: L, what the HUD draws add, and
// T, the factor the scene behind them keeps (per channel), so that eye = scene * T + L. A draw whose blend is affine in the destination
// with factors that do not read it (ZERO, ONE, SRC_ALPHA, ONE_MINUS_SRC_ALPHA as source, those and SRC_COLOR / ONE_MINUS_SRC_COLOR as
// destination factor) or that multiplies the destination by its colour (source factor DST_COLOR) keeps this form: L' = S*src + D*L as
// the draw's own blend on L, T' = D*T (or DST_COLOR*T + D*T) as a second attachment the same fragment colour is written to.
// T is stored at half scale (128/255 = 1) so that a DST_COLOR draw brightening the destination (factor up to 2) is not clamped.
// What it cannot reproduce: the EFB clamps after every draw, the layer only at the composite, so a draw that brightens (an add, a multiply by
// more than one) where the scene behind is already bright, followed on the same pixel by a draw that blends over it, differs there (the
// game's HUD draws its one additive and one multiplying quad last in its gauge); and 8-bit L and T round apart (a few steps of 1/255).
#include <array>
#include <cmath>
#include <cstdint>

namespace uc {

// GX blend factor indices (gx::BlendState::source / destination; vulkan_renderer.cpp make_pipeline maps them to Vulkan factors).
enum : uint8_t { kGxSrcZero = 0, kGxSrcOne = 1, kGxSrcDstColor = 2, kGxSrcInvDstColor = 3, kGxSrcSrcAlpha = 4, kGxSrcInvSrcAlpha = 5, kGxSrcDstAlpha = 6, kGxSrcInvDstAlpha = 7 };
enum : uint8_t { kGxDstZero = 0, kGxDstOne = 1, kGxDstSrcColor = 2, kGxDstInvSrcColor = 3, kGxDstSrcAlpha = 4, kGxDstInvSrcAlpha = 5, kGxDstDstAlpha = 6, kGxDstInvDstAlpha = 7 };

// Whether a draw's blend (GX mode: 0 none, 1 blend, 2 logic, 3 subtract) keeps eye = scene * T + L exact.
inline bool hud_blend_separable(uint8_t mode, uint8_t source, uint8_t destination) {
    if (mode == 0) return true;   // a plain write: L = src, T = 0
    if (mode != 1) return false;  // logic ops and reverse subtract
    const bool source_ok = source == kGxSrcZero || source == kGxSrcOne || source == kGxSrcSrcAlpha || source == kGxSrcInvSrcAlpha || source == kGxSrcDstColor;
    const bool destination_ok = destination <= kGxDstInvSrcAlpha;   // not DST_ALPHA / ONE_MINUS_DST_ALPHA
    return source_ok && destination_ok;
}

// The clear value of T (stored half scale) and the factor that reads it back.
constexpr float kHudTransmitClear = 128.f / 255.f;
constexpr float kHudTransmitScale = 255.f / 128.f;

using Mat3 = std::array<std::array<double, 3>, 3>;
inline Mat3 mul3(const Mat3& a, const Mat3& b) {
    Mat3 r{};
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) for (int k = 0; k < 3; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}
inline bool invert3(const Mat3& m, Mat3& out) {
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1], c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2], c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    if (!std::isfinite(det) || std::fabs(det) < 1e-12) return false;
    const double k = 1 / det;
    out = {{{c00 * k, (m[0][2] * m[2][1] - m[0][1] * m[2][2]) * k, (m[0][1] * m[1][2] - m[0][2] * m[1][1]) * k},
            {c01 * k, (m[0][0] * m[2][2] - m[0][2] * m[2][0]) * k, (m[0][2] * m[1][0] - m[0][0] * m[1][2]) * k},
            {c02 * k, (m[0][1] * m[2][0] - m[0][0] * m[2][1]) * k, (m[0][0] * m[1][1] - m[0][1] * m[1][0]) * k}}};
    return true;
}

// The layer's place in each eye. rows[eye]: G, the homography from an EFB texture coordinate (u, v, 1) of that eye's layer to the HUD
// layer's texture coordinate (divide by the third component); the fourth float of each row is padding.
struct HudLayerMap {
    std::array<std::array<std::array<float, 4>, 3>, 2> rows{};
    std::array<Mat3, 2> forward{};   // layer coordinate -> EFB coordinate (G inverse)
    std::array<Mat3, 2> plane{};     // H^-1 per eye: eye clip (x, y, w) -> the reference plane's NDC (x, y, w), for hud_layer_block
    std::array<float, 4> viewport{0, 0, 640, 480};   // the HUD draws' GX viewport (guest pixels): what the layer holds
};

// A GX viewport's NDC -> texture coordinate of a 640x480 target (render_scale.h efb_viewport: the origin -1/12 guest pixel) and of the layer
// (the viewport's own area, the same -1/12 pixel origin).
inline Mat3 hud_efb_from_ndc(const std::array<float, 4>& v) {
    return {{{v[2] / 1280.0, 0, (v[0] - 1 / 12.0 + v[2] / 2.0) / 640}, {0, v[3] / 960.0, (v[1] - 1 / 12.0 + v[3] / 2.0) / 480}, {0, 0, 1}}};
}
inline Mat3 hud_layer_from_ndc(const std::array<float, 4>& v) {
    return {{{.5, 0, .5 - 1 / (12.0 * v[2])}, {0, .5, .5 - 1 / (12.0 * v[3])}, {0, 0, 1}}};
}

// block: the two eyes' Vulkan-clip transforms of a HUD draw (VulkanRenderer's eye block: eye * 16 + row * 4 + column), z: the Vulkan clip
// depth of the HUD's vertices (a 2D draw's w is 1), viewport: the draws' GX viewport (x, y, width, height in guest pixels; the EFB maps the
// game's 640x480 over its extent, the layer the viewport over its own).
inline bool hud_layer_map(const float* block, float z, const std::array<float, 4>& viewport, HudLayerMap& out) {
    if (!(viewport[2] > 0 && viewport[3] > 0)) return false;
    const Mat3 efb = hud_efb_from_ndc(viewport), layer = hud_layer_from_ndc(viewport);
    Mat3 efb_inverse, layer_inverse;
    if (!invert3(efb, efb_inverse) || !invert3(layer, layer_inverse)) return false;
    for (int eye = 0; eye < 2; ++eye) {
        const float* m = block + eye * 16;
        Mat3 h{};
        const int rows[3] = {0, 1, 3};
        for (int i = 0; i < 3; ++i) {
            const float* r = m + rows[i] * 4;
            h[i] = {double(r[0]), double(r[1]), double(r[2]) * z + r[3]};
        }
        Mat3 h_inverse;
        if (!invert3(h, h_inverse)) return false;
        out.forward[eye] = mul3(mul3(efb, h), layer_inverse);
        out.plane[eye] = h_inverse;
        const Mat3 g = mul3(mul3(layer, h_inverse), efb_inverse);
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) out.rows[eye][i][j] = float(g[i][j]);
    }
    out.viewport = viewport;
    return true;
}

// The glow layer (the same images, a second instance): full-screen 2D draws that each eye sees in its own clip space (the game's lens
// glows and fades over the scene: HOTD4's identity / fill classes) and that carry no detail finer than about a guest pixel (their textures
// are magnified: VulkanRenderer's bloom_shading_rate test), drawn at 1.5 points per guest pixel instead of the EFB's 3-6. The map is the
// viewport's place in the EFB, the same affine one in both eyes: layer = (EFB * 640x480 - origin) / size.
inline HudLayerMap hud_overlay_map(const std::array<float, 4>& viewport) {
    HudLayerMap map;
    const Mat3 g{{{640.0 / viewport[2], 0, -viewport[0] / viewport[2]}, {0, 480.0 / viewport[3], -viewport[1] / viewport[3]}, {0, 0, 1}}};
    for (int eye = 0; eye < 2; ++eye) {
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) map.rows[eye][i][j] = float(g[i][j]);
        invert3(g, map.forward[eye]);
        map.plane[eye] = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
    }
    map.viewport = viewport;
    return map;
}

// A HUD draw's transform into the layer (each eye's slice): its eye block (VulkanRenderer's, eye * 16 + row * 4 + column) followed by H^-1 of
// that eye; depth z/w = 1/2 (the layer has no depth test). The draw at the reference depth gets the identity in x, y and w.
inline void hud_layer_block(const HudLayerMap& map, const float* block, float* out) {
    for (int eye = 0; eye < 2; ++eye) {
        const float* m = block + eye * 16;
        const auto& p = map.plane[eye];
        for (int c = 0; c < 4; ++c) {
            double r[3];
            for (int i = 0; i < 3; ++i) r[i] = p[i][0] * m[0 * 4 + c] + p[i][1] * m[1 * 4 + c] + p[i][2] * m[3 * 4 + c];
            out[eye * 16 + 0 * 4 + c] = float(r[0]); out[eye * 16 + 1 * 4 + c] = float(r[1]);
            out[eye * 16 + 2 * 4 + c] = float(.5 * r[2]); out[eye * 16 + 3 * 4 + c] = float(r[2]);
        }
    }
}

// EFB pixels per guest pixel of the HUD at the layer's centre, the larger axis of either eye: the layer is made at the viewport's size
// times the smallest k (rounded up to a quarter, at least 1, at most `largest`) that keeps a layer texel no larger than an eye pixel there.
inline float hud_layer_scale(const HudLayerMap& map, uint32_t efb_width, uint32_t efb_height, float largest) {
    float k = 1;
    for (const auto& f : map.forward) {
        const auto at = [&](double u, double v) {
            const double x = f[0][0] * u + f[0][1] * v + f[0][2], y = f[1][0] * u + f[1][1] * v + f[1][2], w = f[2][0] * u + f[2][1] * v + f[2][2];
            return std::array<double, 2>{x / w, y / w};
        };
        const double step = 1e-3;
        const auto c = at(.5, .5), du = at(.5 + step, .5), dv = at(.5, .5 + step);
        const double sx = std::hypot((du[0] - c[0]) * efb_width, (du[1] - c[1]) * efb_height) / (step * map.viewport[2]);
        const double sy = std::hypot((dv[0] - c[0]) * efb_width, (dv[1] - c[1]) * efb_height) / (step * map.viewport[3]);
        if (std::isfinite(sx)) k = std::max(k, float(sx));
        if (std::isfinite(sy)) k = std::max(k, float(sy));
    }
    return std::min(largest, std::ceil(k * 4) / 4);
}

}  // namespace uc
