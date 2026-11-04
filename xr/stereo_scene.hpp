#pragma once
// HOTD4 VR's renderer-side camera policy (lindbergh::vk::StereoScene, lindberghrecomp ef5cd20), unchanged but for the namespace and the
// matrix helpers it took from guest_state.hpp. Matrices are column-major guest-GL clip space, as HOTD4's.
#include "stereo_projection.h"
#include <array>
#include <cstddef>
#include <vector>

namespace uc::xr {
using Matrix = std::array<float, 16>;
struct Matrices {
    float view_projection[2][16]{}; // guest GL clip -> eye GL clip (HUD/copies)
    float from_view[2][16]{};       // view -> eye GL clip (perspective world only)
};
inline Matrix identity() { Matrix m{}; m[0] = m[5] = m[10] = m[15] = 1; return m; }
inline Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int c = 0; c < 4; ++c) for (int row = 0; row < 4; ++row) {
        float v = 0; for (int k = 0; k < 4; ++k) v += a[k * 4 + row] * b[c * 4 + k];
        r[c * 4 + row] = v;
    }
    return r;
}

struct StereoPose {
    std::array<float,4> orientation{0,0,0,1};
    std::array<float,3> position{};
    std::array<float,4> tangents{1,1,1,1};
};
struct StereoRay {std::array<float,3> origin{},direction{};int x=-100000,y=-100000;float reach=0;};
struct StereoSettings {
    float scale=1,screen=3,hud_lens=0,hud_pitch=0,zoom_lens=8;
    bool head_locked=false;
    // Umbrella Chronicles (not HOTD4): in gameplay the HUD follows the head and spans the display, its sides at the edges of what both eyes
    // see (hud_margin of the inner half-angle), in the game picture's shape. Menus keep HOTD4's screen, which the gun aims at.
    bool hud_fit=false; float hud_margin=.9f;
};
// A renderer-side draw class beside HOTD4's four: a 2D draw that marks a place in the world (the aim reticle), kept on HOTD4's world-locked
// screen of the game's lens when the rest of the HUD follows the head.
enum { LG_STEREO_HUD_AIM=4 };
// Renderer-thread camera policy. Pose publication invalidates a bounded, reusable transform cache; identical draws never invert their
// camera again. No OpenXR/SDL/GL dependency: the host supplies anchored poses once per frame.
class StereoScene {
public:
    explicit StereoScene(StereoSettings settings);
    void publish(const std::array<StereoPose,2> &eyes);
    void invalidate();
    void configure(StereoSettings settings);
    const std::array<float,4> &head_orientation() const {return eyes_[0].orientation;}
    const StereoSettings &settings() const {return settings_;}
    bool head_locked() const {return settings_.head_locked;}
    bool aim(const StereoPose &controller,int width,int height,bool on_hud,float aim_distance,StereoRay &ray) const;
    const Matrices &overlay_matrices() const {return overlay_;}
    const std::array<std::array<float,3>,2> &overlay_eyes() const {return eye_positions_;}
    bool tracking_valid() const { return valid_; }
    bool world_camera(const Matrix &projection) const;
    bool zoomed(const Matrix &projection) const;
    const Matrices &transforms(const Matrix &projection,int draw_class);
    void observe(const Matrix &projection,int draw_class);
    void finish_frame();
    size_t transform_builds() const { return builds_; }
    const std::array<float,2> &screen_lens() const { return lens_; }
    const std::array<std::array<float,4>,2> &tangents() const { return tangents_; }   // each eye's half-angle tangents (left, right, down, up); zero before the first publish
    void set_screen_lens(std::array<float,2> lens) { lens_=lens; }   // the game's own lens before any frame was observed
    void set_aim_depth(float metres) { if (metres!=aim_depth_) { aim_depth_=metres; cache_.clear(); last_=0; } }
    // Whether the head-following HUD applies now (gameplay; settings().hud_fit): set once a frame before its draws.
    void set_hud_fit(bool on) { if(on!=fit_on_) { fit_on_=on; cache_.clear(); last_=0; } }
private:
    StereoSettings settings_;
    std::array<StereoPose,2> eyes_;
    Matrices overlay_{};std::array<std::array<float,3>,2> eye_positions_{};
    bool valid_=false;
    std::array<Matrix,2> view_{},hud_view_{},head_view_{};   // head_view_: head-relative eye views (the fitted HUD's)
    bool fit_on_=false;
    std::array<std::array<float,4>,2> tangents_{};
    std::array<float,2> lens_{1.732f,2.309f};
    struct Entry { Matrix projection;int kind;Matrices matrices; };
    std::vector<Entry> cache_;
    size_t last_=0,builds_=0;
    struct Lens {Matrix projection{};unsigned draws=0;};
    std::array<Lens,4> tally_{};
    float longest_=0;
    float aim_depth_=0;
    unsigned zoom_frames_=0;
    Matrices identity_{};
};
} // namespace uc::xr
