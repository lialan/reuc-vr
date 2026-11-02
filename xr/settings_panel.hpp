// HOTD4 VR's SettingsPanel (lindberghrecomp ef5cd20 renderer/vulkan/settings_panel.hpp), unchanged but for the namespace and include paths.
#pragma once
#include <string>
#include <vector>
#include "controller_input.hpp"
#include "xr_menu.h"
#include "xr_panel.h"
namespace uc::xr {
struct PanelDraw {bool visible=false;Matrix model=identity();};
// Render-worker UI state. Policy/painter are shared with GL; pictures allocate
// once and upload only on change, before any game render pass starts.
class SettingsPanel {
public:
    SettingsPanel(int width,int height,const char *title,bool fake=false);
    unsigned update(LgXrSettings &settings,StereoScene &scene,ControllerInput &input,
                    ControllerSample sample,int64_t time,bool focused,const std::vector<float> &rates,int64_t readout_time=0);
    // The panel pictures that changed since the last call (XM_W x XM_H settings, XM_STATS_W x XM_STATS_H stats), RGBA8; null: unchanged.
    const unsigned char *take_panel() {if(!panel_dirty_)return nullptr;panel_dirty_=false;return panel_pixels_.data();}
    const unsigned char *take_label() {if(!label_dirty_)return nullptr;label_dirty_=false;return label_pixels_.data();}
    const std::array<PanelDraw,2> &draws() const {return draws_;}
    const std::array<XoFrame,2> &guns() const {return guns_;}
    const std::array<ControllerHaptic,2> &haptics() const {return haptics_;}
    bool open() const {return panel_.open;}
    void set_eye_size(int width,int height) {eye_w_=width;eye_h_=height;}   // the swapchain's size, for the stats (width_/height_ are the settings)
private:
    XpPanel panel_{};int width_,height_,eye_w_=0,eye_h_=0,previous_menu_=0,previous_upper_[2]{},previous_trigger_[2]{};
    // "Show stats": counted here, sampled and repainted every half second
    int64_t held_=0,stats_since_=0;unsigned stats_frames_=0,stats_samples_=0;bool stats_shown_=false;
    std::string stats_text_;
    bool fake_,panel_dirty_=false,label_dirty_=false;
    std::string title_;std::vector<unsigned char> panel_pixels_,label_pixels_;
    std::array<PanelDraw,2> draws_{};std::array<XoFrame,2> guns_{};
    std::array<ControllerHaptic,2> haptics_{};
};
}
