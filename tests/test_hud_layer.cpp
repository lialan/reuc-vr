#include "check.h"
#include "hud_layer.h"
#include <algorithm>
#include <cmath>
#include <random>
using namespace uc;
// The headset's HUD layer (renderer/hud_layer.h): where the layer lies in each eye, and that scene * T + L is what blending the HUD's draws
// over the scene gives.
namespace {
// An eye transform as the renderer's eye block (row-major, Vulkan clip to Vulkan clip) with perspective in it: the HUD plane seen at an angle.
void eye_blocks(float* block) {
    // the depth column moves x with the draw's depth in opposite directions per eye: parallax
    const float rows[2][16] = {{0.62f, 0.03f, 0.05f, 0.11f, -0.02f, 0.58f, 0.f, -0.07f, 0.f, 0.f, 0.5f, 0.5f, 0.04f, -0.03f, 0.01f, 1.f},
                               {0.61f, -0.02f, -0.05f, -0.13f, 0.01f, 0.59f, 0.f, -0.06f, 0.f, 0.f, 0.5f, 0.5f, -0.05f, -0.02f, 0.01f, 1.f}};
    std::copy(&rows[0][0], &rows[0][0] + 32, block);
}
void the_map_finds_each_game_point_in_each_eye() {
    float block[32]; eye_blocks(block);
    const float z = 0.37f;
    const std::array<float, 4> viewport{0, 0, 640, 360};   // the game's widescreen viewport
    HudLayerMap map; CHECK(hud_layer_map(block, z, viewport, map));
    std::mt19937 random(7); std::uniform_real_distribution<float> unit(-1, 1);
    for (int eye = 0; eye < 2; ++eye) for (int i = 0; i < 200; ++i) {
        const float c[4] = {unit(random), unit(random), z, 1};
        const float* m = block + eye * 16;
        float e[4]; for (int r = 0; r < 4; ++r) e[r] = m[r * 4] * c[0] + m[r * 4 + 1] * c[1] + m[r * 4 + 2] * c[2] + m[r * 4 + 3] * c[3];
        // the EFB texture coordinate the eye shows it at (render_scale.h efb_viewport of the viewport over 640x480)
        const float u = (viewport[0] - 1.f / 12 + (e[0] / e[3] * .5f + .5f) * viewport[2]) / 640, v = (viewport[1] - 1.f / 12 + (e[1] / e[3] * .5f + .5f) * viewport[3]) / 480;
        const auto& g = map.rows[eye];
        const float q[3] = {g[0][0] * u + g[0][1] * v + g[0][2], g[1][0] * u + g[1][1] * v + g[1][2], g[2][0] * u + g[2][1] * v + g[2][2]};
        CHECK(q[2] > 0);
        CHECK(std::fabs(q[0] / q[2] - (-1.f / 12 / viewport[2] + c[0] * .5f + .5f)) < 1e-5f);   // the layer covers the viewport
        CHECK(std::fabs(q[1] / q[2] - (-1.f / 12 / viewport[3] + c[1] * .5f + .5f)) < 1e-5f);
    }
}
void a_draw_at_the_reference_depth_is_drawn_as_it_is() {
    float block[32]; eye_blocks(block);
    const float z = 0.37f;
    HudLayerMap map; CHECK(hud_layer_map(block, z, {0, 0, 640, 360}, map));
    float into[32]; hud_layer_block(map, block, into);
    for (int eye = 0; eye < 2; ++eye) for (float x : {-.8f, .1f, .9f}) for (float y : {-.7f, .4f}) {
        const float c[4] = {x, y, z, 1};
        const float* m = into + eye * 16;
        float o[4]; for (int r = 0; r < 4; ++r) o[r] = m[r * 4] * c[0] + m[r * 4 + 1] * c[1] + m[r * 4 + 2] * c[2] + m[r * 4 + 3] * c[3];
        CHECK(std::fabs(o[0] / o[3] - x) < 1e-5f && std::fabs(o[1] / o[3] - y) < 1e-5f && std::fabs(o[2] / o[3] - .5f) < 1e-6f);
    }
    // at another depth the draw lands where that eye sees it on the reference plane: through G back to the EFB coordinate the eye shows
    const float z2 = 0.9f;
    for (int eye = 0; eye < 2; ++eye) {
        const float c[4] = {.3f, -.2f, z2, 1};
        const float* m = block + eye * 16;
        float e[4]; for (int r = 0; r < 4; ++r) e[r] = m[r * 4] * c[0] + m[r * 4 + 1] * c[1] + m[r * 4 + 2] * c[2] + m[r * 4 + 3] * c[3];
        const float* l = into + eye * 16;
        float o[4]; for (int r = 0; r < 4; ++r) o[r] = l[r * 4] * c[0] + l[r * 4 + 1] * c[1] + l[r * 4 + 2] * c[2] + l[r * 4 + 3] * c[3];
        // o is reference-plane NDC; through H it must give the eye's NDC
        const float* b = block + eye * 16;
        const float X = b[0] * o[0] / o[3] + b[1] * o[1] / o[3] + b[2] * z + b[3], Y = b[4] * o[0] / o[3] + b[5] * o[1] / o[3] + b[6] * z + b[7], W = b[12] * o[0] / o[3] + b[13] * o[1] / o[3] + b[14] * z + b[15];
        CHECK(std::fabs(X / W - e[0] / e[3]) < 1e-5f && std::fabs(Y / W - e[1] / e[3]) < 1e-5f);
    }
}
void a_singular_transform_is_refused() {
    float block[32]{}; HudLayerMap map;
    CHECK(!hud_layer_map(block, 0.f, {0, 0, 640, 480}, map));
}
void the_scale_follows_the_eye_pixels() {
    float block[32]{};   // identity: the HUD covers the whole EFB, so an EFB of 1920x1440 is 3 eye pixels a game pixel
    for (int eye = 0; eye < 2; ++eye) for (int r = 0; r < 4; ++r) block[eye * 16 + r * 5] = 1;
    HudLayerMap map; CHECK(hud_layer_map(block, 0.f, {0, 0, 640, 480}, map));
    CHECK(std::fabs(hud_layer_scale(map, 1920, 1440, 4) - 3) < 1e-3f);
    CHECK(std::fabs(hud_layer_scale(map, 2064, 2944, 4) - 4) < 1e-3f);    // 6.1 eye pixels a game pixel vertically: capped
    CHECK(std::fabs(hud_layer_scale(map, 640, 480, 4) - 1) < 1e-3f);
    CHECK(std::fabs(hud_layer_scale(map, 700, 500, 4) - 1.25f) < 1e-3f);  // rounded up to a quarter
}
void only_destination_independent_blends_separate() {
    CHECK(hud_blend_separable(0, 1, 0));                                    // a plain write
    CHECK(hud_blend_separable(1, kGxSrcSrcAlpha, kGxDstInvSrcAlpha));       // alpha over
    CHECK(hud_blend_separable(1, kGxSrcSrcAlpha, kGxDstOne));               // additive
    CHECK(hud_blend_separable(1, kGxSrcDstColor, kGxDstInvSrcAlpha));       // a multiply (the HUD's gauge)
    CHECK(hud_blend_separable(1, kGxSrcZero, kGxDstSrcColor));
    CHECK(!hud_blend_separable(1, kGxSrcInvDstColor, kGxDstOne));
    CHECK(!hud_blend_separable(1, kGxSrcSrcAlpha, kGxDstDstAlpha));
    CHECK(!hud_blend_separable(1, kGxSrcDstAlpha, kGxDstOne));
    CHECK(!hud_blend_separable(2, 1, 0));                                   // logic ops
    CHECK(!hud_blend_separable(3, 1, 1));                                   // subtract
}
// One colour channel through a sequence of blended draws: straight into the EFB (8-bit, clamped), and into L and T (8-bit, T at half scale)
// composited afterwards, as the renderer's pipelines blend them.
float factor(uint8_t f, bool source, float src, float alpha, float dst) {
    if (source) switch (f) { case kGxSrcZero: return 0; case kGxSrcOne: return 1; case kGxSrcDstColor: return dst; case kGxSrcSrcAlpha: return alpha; case kGxSrcInvSrcAlpha: return 1 - alpha; }
    else switch (f) { case kGxDstZero: return 0; case kGxDstOne: return 1; case kGxDstSrcColor: return src; case kGxDstInvSrcColor: return 1 - src; case kGxDstSrcAlpha: return alpha; case kGxDstInvSrcAlpha: return 1 - alpha; }
    return 0;
}
float unorm(float x) { return std::round(std::clamp(x, 0.f, 1.f) * 255) / 255; }
void blending_over_the_layer_matches_blending_into_the_efb() {
    struct Draw { uint8_t source, destination; };
    const Draw draws[] = {{kGxSrcSrcAlpha, kGxDstInvSrcAlpha}, {kGxSrcSrcAlpha, kGxDstInvSrcAlpha}, {kGxSrcDstColor, kGxDstInvSrcAlpha}, {kGxSrcSrcAlpha, kGxDstOne},
                          {kGxSrcOne, kGxDstZero}, {kGxSrcZero, kGxDstSrcColor}, {kGxSrcSrcAlpha, kGxDstInvSrcAlpha}};
    // Draws that cannot brighten (over, copy, modulate) in any order, and the brightening ones (a multiply by more than one, an add) only last:
    // a brightening draw under a later one differs where the EFB would have saturated in between (hud_layer.h).
    std::mt19937 random(11); std::uniform_real_distribution<float> unit(0, 1); std::uniform_int_distribution<int> pick(0, 6), count(1, 5);
    const auto brightens = [&](const Draw& d) { return d.source == kGxSrcDstColor || d.destination == kGxDstOne; };
    float worst = 0;
    for (int trial = 0; trial < 20000; ++trial) {
        const float scene = unorm(unit(random));
        float efb = scene, light = 0, transmit = kHudTransmitClear;
        for (int n = count(random); n > 0; --n) {
            const Draw* pick_draw = &draws[pick(random)];
            while (n > 1 && brightens(*pick_draw)) pick_draw = &draws[pick(random)];
            const Draw& d = *pick_draw;
            const float src = unorm(unit(random)), alpha = unorm(unit(random));
            efb = unorm(factor(d.source, true, src, alpha, efb) * src + factor(d.destination, false, src, alpha, efb) * efb);
            light = unorm(factor(d.source, true, src, alpha, light) * src + factor(d.destination, false, src, alpha, light) * light);
            transmit = unorm((d.source == kGxSrcDstColor ? transmit : 0.f) * src + factor(d.destination, false, src, alpha, transmit) * transmit);
        }
        const float composite = std::clamp(scene * transmit * kHudTransmitScale + light, 0.f, 1.f);
        worst = std::max(worst, std::fabs(composite - efb));
    }
    std::fprintf(stderr, "HUD layer: largest difference to the EFB's blending %.1f / 255\n", worst * 255);
    CHECK(worst <= 4.f / 255);   // 8-bit rounding of L and T where the EFB rounds the sum; a few steps at most
}
}  // namespace
int main() {
    the_map_finds_each_game_point_in_each_eye(); a_draw_at_the_reference_depth_is_drawn_as_it_is(); a_singular_transform_is_refused(); the_scale_follows_the_eye_pixels();
    only_destination_independent_blends_separate(); blending_over_the_layer_matches_blending_into_the_efb();
    return 0;
}
