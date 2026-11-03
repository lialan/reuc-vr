#include "check.h"
#include "fovea_map.h"
#include <cmath>
using namespace uc;
// The headset's foveation map (renderer/fovea_map.h).
namespace {
const float kOuter = std::tan(54.f * .017453292f), kInner = std::tan(40.f * .017453292f), kDown = std::tan(55.f * .017453292f), kUp = std::tan(44.f * .017453292f);
const std::array<std::array<float, 4>, 2> kEyes{std::array<float, 4>{kOuter, kInner, kDown, kUp}, std::array<float, 4>{kInner, kOuter, kDown, kUp}};
constexpr uint32_t kWidth = 2064, kHeight = 2944, kShown = 2208, kTexel = 8;
uint8_t at(const std::vector<uint8_t>& map, uint32_t layer, float x, float y) {
    const uint32_t width = (kWidth + kTexel - 1) / kTexel, height = (kHeight + kTexel - 1) / kTexel;
    return map[(size_t(layer) * height + uint32_t(y / kTexel)) * width + uint32_t(x / kTexel)];
}
// where each eye looks: its forward direction on its picture
float centre_x(const std::array<float, 4>& t) { return kWidth * t[0] / (t[0] + t[1]); }
float centre_y(const std::array<float, 4>& t) { return kShown * t[3] / (t[2] + t[3]); }
void texels_encode_the_rate() {
    CHECK(fovea_texel(1, 1) == 0); CHECK(fovea_texel(2, 2) == 5); CHECK(fovea_texel(4, 4) == 10); CHECK(fovea_texel(2, 1) == 4);
}
void each_eye_is_full_rate_at_its_own_centre() {
    for (int level = 1; level <= 3; ++level) {
        const auto map = fovea_map(level, kWidth, kHeight, kTexel, kTexel, {0, 0, float(kWidth), float(kShown)}, kEyes, 2);
        for (uint32_t eye = 0; eye < 2; ++eye) {
            CHECK(at(map, eye, centre_x(kEyes[eye]), centre_y(kEyes[eye])) == fovea_texel(1, 1));
            CHECK(at(map, eye, 4, 4) == fovea_texel(4, 4));                                 // the far corner
            CHECK(at(map, eye, kWidth / 2.f, kShown + 100.f) == fovea_texel(4, 4));          // below the shown rows: nobody sees it
        }
    }
}
void the_eyes_centres_are_not_the_pictures_centre() {
    // the left eye looks right of its picture's middle, the right eye left of it: a point right of the left eye's centre lies further
    // from the right eye's, so the right eye's layer is as coarse there or coarser
    const auto map = fovea_map(3, kWidth, kHeight, kTexel, kTexel, {0, 0, float(kWidth), float(kShown)}, kEyes, 2);
    CHECK(centre_x(kEyes[0]) > kWidth / 2.f && centre_x(kEyes[1]) < kWidth / 2.f);
    const float x = centre_x(kEyes[0]) + 0.3f * kWidth;   // well right of the left eye's centre: further from the right eye's
    CHECK(at(map, 1, x, centre_y(kEyes[1])) >= at(map, 0, x, centre_y(kEyes[0])));
}
void higher_levels_are_coarser() {
    size_t full[4]{};
    for (int level = 1; level <= 3; ++level)
        for (uint8_t t : fovea_map(level, kWidth, kHeight, kTexel, kTexel, {0, 0, float(kWidth), float(kShown)}, kEyes, 2)) full[level] += t == 0;
    CHECK(full[1] > full[2] && full[2] > full[3] && full[3] > 0);
}
void one_layer_takes_the_finer_eye() {
    const auto both = fovea_map(2, kWidth, kHeight, kTexel, kTexel, {0, 0, float(kWidth), float(kShown)}, kEyes, 1);
    const auto per_eye = fovea_map(2, kWidth, kHeight, kTexel, kTexel, {0, 0, float(kWidth), float(kShown)}, kEyes, 2);
    for (float x : {centre_x(kEyes[0]), centre_x(kEyes[1])}) {
        CHECK(at(both, 0, x, centre_y(kEyes[0])) == fovea_texel(1, 1));
        CHECK(at(both, 0, x, centre_y(kEyes[0])) <= at(per_eye, 0, x, centre_y(kEyes[0])));
    }
}
void the_largest_rate_caps_it() {
    for (uint8_t t : fovea_map(3, kWidth, kHeight, kTexel, kTexel, {0, 0, float(kWidth), float(kShown)}, kEyes, 2, 2)) CHECK(t == fovea_texel(1, 1) || t == fovea_texel(2, 2));
}
}  // namespace
int main() {
    texels_encode_the_rate(); each_eye_is_full_rate_at_its_own_centre(); the_eyes_centres_are_not_the_pictures_centre(); higher_levels_are_coarser();
    one_layer_takes_the_finer_eye(); the_largest_rate_caps_it();
    return 0;
}
