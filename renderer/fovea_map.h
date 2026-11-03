#pragma once
// The headset's fixed foveation (VK_KHR_fragment_shading_rate, attachment rate): the shading rate of each texel of a target that an eye
// sees through a rectangle of it, coarser the further a texel lies from that eye's centre of projection. Each eye's centre is where its
// forward direction meets its picture, which is not the picture's centre: the Quest's eye frusta are asymmetric (wider towards the eye's
// own side and downwards), so the left eye's centre lies right of its picture's middle and the right eye's left of it.
//
// Levels (vr.cfg "foveation", the settings panel's Foveation row): the angle from the eye's forward direction up to which every pixel is
// shaded (1x1), then 2x2 up to the second angle, 4x4 beyond it and outside the shown rectangle. In tangent space (the eye image) the
// periphery takes most of the pixels: with the Quest 3's fields of view 1x1 covers 29% / 20% / 13% of an eye's pixels (low / medium /
// high) and the fragment work of a full-screen pass falls to about 41% / 30% / 22%.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace uc {
struct FoveaLevel { float full_degrees, half_degrees; };   // 1x1 inside full_degrees, 2x2 inside half_degrees, else 4x4
inline FoveaLevel fovea_level(int level) {
    switch (level) {
    case 1: return {35, 50};
    case 2: return {30, 42};
    default: return {25, 36};
    }
}
// VK_KHR_fragment_shading_rate's attachment texel: (log2 width << 2) | log2 height.
constexpr uint8_t fovea_texel(uint32_t width, uint32_t height) {
    return uint8_t(((width >= 4 ? 2 : width >= 2 ? 1 : 0) << 2) | (height >= 4 ? 2 : height >= 2 ? 1 : 0));
}
// The rate (side of the square fragment) at a point of an eye's tangent-space picture.
inline uint32_t fovea_side(const FoveaLevel& level, float tangent_x, float tangent_y) {
    const float degrees = std::atan(std::sqrt(tangent_x * tangent_x + tangent_y * tangent_y)) * 57.29578f;
    return degrees < level.full_degrees ? 1u : degrees < level.half_degrees ? 2u : 4u;
}
// The map of a target `extent` pixels big with `texel`-sized attachment texels. `shown` is the part each eye sees, in target pixels
// (left, top, right, bottom); `tangents` each eye's tangents of its half-angles (left, right, down, up, all positive), as XrCompositor
// publishes them; the picture's top row is the eye's top. `layers` 2: one layer per eye (multiview selects it by the view index); 1: both
// eyes in one layer, each texel at the finer of the two eyes' rates. `largest` caps the side (the device's largest square rate).
inline std::vector<uint8_t> fovea_map(int level, uint32_t extent_width, uint32_t extent_height, uint32_t texel_width, uint32_t texel_height,
                                      const std::array<float, 4>& shown, const std::array<std::array<float, 4>, 2>& tangents, uint32_t layers, uint32_t largest = 4) {
    const uint32_t width = (extent_width + texel_width - 1) / texel_width, height = (extent_height + texel_height - 1) / texel_height;
    std::vector<uint8_t> texels(size_t(width) * height * layers);
    const FoveaLevel rings = fovea_level(level);
    const float shown_width = std::max(1.f, shown[2] - shown[0]), shown_height = std::max(1.f, shown[3] - shown[1]);
    for (uint32_t layer = 0; layer < layers; ++layer)
        for (uint32_t y = 0; y < height; ++y)
            for (uint32_t x = 0; x < width; ++x) {
                // the texel's centre, clamped to the shown rectangle: a texel across its edge is shaded as its inside part needs
                const float px = std::clamp((x + .5f) * texel_width, shown[0], shown[2]), py = std::clamp((y + .5f) * texel_height, shown[1], shown[3]);
                const bool outside = (x + 1.f) * texel_width <= shown[0] || x * float(texel_width) >= shown[2] || (y + 1.f) * texel_height <= shown[1] || y * float(texel_height) >= shown[3];
                uint32_t side = largest;
                if (!outside)
                    for (uint32_t eye = layers > 1 ? layer : 0; eye < (layers > 1 ? layer + 1 : 2u); ++eye) {
                        const auto& t = tangents[eye];
                        const float tx = -t[0] + (px - shown[0]) / shown_width * (t[0] + t[1]), ty = t[3] - (py - shown[1]) / shown_height * (t[2] + t[3]);
                        side = std::min(side, fovea_side(rings, tx, ty));
                    }
                side = std::min(side, largest);
                texels[(size_t(layer) * height + y) * width + x] = fovea_texel(side, side);
            }
    return texels;
}
}  // namespace uc
