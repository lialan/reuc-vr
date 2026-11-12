// The headset settings panel's Display page (xr/xr_panel.c) and the vr.cfg keys it edits (xr/xr_settings.c): rows, toggles, paging,
// save and load. The vr.cfg migrations of the Quest build (__ANDROID__) are not compiled here.
#include "xr_panel.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

int main() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("test_xr_panel_" + std::to_string(std::rand()));
    fs::create_directories(dir);
    fs::current_path(dir);   // xr_settings_save / _load use ./vr.cfg

    LgXrSettings cfg = LG_XR_SETTINGS_DEFAULT;
    CHECK(cfg.version == LG_XR_SETTINGS_VERSION);
    CHECK(cfg.fxaa == LG_XR_DEFAULT_FXAA && cfg.post_shading == 0 && cfg.merge == 1 && cfg.glow == 1 && cfg.sharpen == 0);

    XpPanel panel{}; panel.open = 1; int hand = 1;
    XmItem items[XP_MAX_ROWS];
    CHECK(xp_count(&panel) == XP_COUNT);
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XP_DISPLAY].label, "Display") == 0);

    // Display opens the page without saving; its rows toggle and save
    CHECK(xp_choose(&panel, &cfg, &hand, XP_DISPLAY, nullptr, 0) == 0);
    CHECK(panel.page == 1 && xp_count(&panel) == XD_COUNT);
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_FOVEATION].label, "Foveation") == 0 && std::strcmp(items[XD_BACK].label, "Back") == 0);
    const int fxaa = cfg.fxaa;
    CHECK(xp_choose(&panel, &cfg, &hand, XD_FXAA, nullptr, 0) == XP_SAVE && cfg.fxaa == !fxaa);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_POST, nullptr, 0) == XP_SAVE && cfg.post_shading == 1);
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_POST].value, "Coarse (faster)") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_MERGE, nullptr, 0) == XP_SAVE && cfg.merge == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_GLOW, nullptr, 0) == XP_SAVE && cfg.glow == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SHARPEN, nullptr, 0) == XP_SAVE && cfg.sharpen == 1);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SHARPEN, nullptr, 0) == XP_SAVE && cfg.sharpen == 2);
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_GLOW].label, "Glow") == 0 && std::strcmp(items[XD_GLOW].value, "Off (faster)") == 0);
    CHECK(cfg.space_warp == 0 && std::strcmp(items[XD_SPACE_WARP].label, "Space warp") == 0 && std::strcmp(items[XD_SPACE_WARP].value, "Off") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SPACE_WARP, nullptr, 0) == (XP_SAVE | XP_SPACE_WARP_CHANGED) && cfg.space_warp == 1);
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_SPACE_WARP].value, "On (half rate)") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SPACE_WARP, nullptr, 0) == (XP_SAVE | XP_SPACE_WARP_CHANGED) && cfg.space_warp == 2);   // head only (test)
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_SPACE_WARP].value, "Head only (test)") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SPACE_WARP, nullptr, 0) == (XP_SAVE | XP_SPACE_WARP_CHANGED) && cfg.space_warp == 3);   // no head in the vectors (test)
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_SPACE_WARP].value, "No head in vectors (test)") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SPACE_WARP, nullptr, 0) == (XP_SAVE | XP_SPACE_WARP_CHANGED) && cfg.space_warp == 4);   // flat depth (test)
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_SPACE_WARP].value, "Flat depth (test)") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SPACE_WARP, nullptr, 0) == (XP_SAVE | XP_SPACE_WARP_CHANGED) && cfg.space_warp == 5);   // flat depth, no head (test)
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XD_SPACE_WARP].value, "Flat depth, no head (test)") == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SPACE_WARP, nullptr, 0) == (XP_SAVE | XP_SPACE_WARP_CHANGED) && cfg.space_warp == 0);
    const int level = cfg.foveation;
    CHECK(xp_choose(&panel, &cfg, &hand, XD_FOVEATION, nullptr, 0) == (XP_SAVE | XP_FOVEATION_CHANGED) && cfg.foveation == (level + 1) % 4);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_SCALE, nullptr, 0) & XP_SCALE_CHANGED);
    CHECK(xp_choose(&panel, &cfg, &hand, XD_COUNT, nullptr, 0) == 0);   // past the page's rows

    // Back returns to the main rows; closing and reopening starts on them too
    CHECK(xp_choose(&panel, &cfg, &hand, XD_BACK, nullptr, 0) == 0 && panel.page == 0);
    xp_choose(&panel, &cfg, &hand, XP_DISPLAY, nullptr, 0);
    CHECK(xp_menu_button(&panel, 1, 0) && !panel.open);
    CHECK(xp_menu_button(&panel, 1, 0) && panel.open && panel.page == 0);

    // an explicit render scale caps the eyes at the picture's rows (xr_android.cpp): the Display page shows the size in use
    {
        XpPanel shown{}; shown.open = 1; shown.page = 1; LgXrSettings capped = cfg;
        capped.render_scale = 2.25f; capped.resolution = 2208; capped.width = 2064;
        xp_items(&shown, &capped, hand, 1, 1620, 1514, items);
        CHECK(std::strcmp(items[XD_RES].value, "1620 (picture size)") == 0);
        xp_items(&shown, &capped, hand, 1, 2208, 2064, items);
        CHECK(std::strcmp(items[XD_RES].value, "2064 x 2208") == 0);
    }

    // Controls opens a read-only page: its rows change nothing, Back returns
    const LgXrSettings before = cfg;
    CHECK(xp_choose(&panel, &cfg, &hand, XP_CONTROLS, nullptr, 0) == 0);
    CHECK(panel.page == 2 && xp_count(&panel) == XC_COUNT);
    xp_items(&panel, &cfg, hand, 1, 2208, 2064, items);
    CHECK(std::strcmp(items[XC_KNIFE].label, "Knife") == 0 && std::strcmp(items[XC_KNIFE].value, "Hold A, swing the gun") == 0);
    CHECK(std::strcmp(items[XC_BACK].label, "Back") == 0 && items[XC_BACK].value[0] == 0);
    for (int i = 0; i < XC_VIBRATION; i++) CHECK(items[i].value[0] != 0 && xp_choose(&panel, &cfg, &hand, i, nullptr, 0) == 0 && panel.page == 2);
    {   // the Vibration switch: on by default, toggled and saved, the page stays
        CHECK(cfg.vibration == 1 && std::strcmp(items[XC_VIBRATION].label, "Vibration") == 0 && std::strcmp(items[XC_VIBRATION].value, "On") == 0);
        CHECK(xp_choose(&panel, &cfg, &hand, XC_VIBRATION, nullptr, 0) == XP_SAVE && cfg.vibration == 0 && panel.page == 2);
        xp_items(&panel, &cfg, hand, 0, 0, 0, items); CHECK(std::strcmp(items[XC_VIBRATION].value, "Off") == 0);
        CHECK(xp_choose(&panel, &cfg, &hand, XC_VIBRATION, nullptr, 0) == XP_SAVE && cfg.vibration == 1);
    }
    CHECK(std::memcmp(&before, &cfg, sizeof cfg) == 0);
    CHECK(xp_choose(&panel, &cfg, &hand, XC_BACK, nullptr, 0) == 0 && panel.page == 0);

    // the new keys survive vr.cfg
    cfg.space_warp = 1;
    xr_settings_save(&cfg);
    LgXrSettings loaded = LG_XR_SETTINGS_DEFAULT;
    xr_settings_load(&loaded);
    CHECK(loaded.fxaa == cfg.fxaa && loaded.post_shading == 1 && loaded.merge == 0 && loaded.glow == 0 && loaded.sharpen == 2 && loaded.foveation == cfg.foveation);
    CHECK(loaded.space_warp == 1);
    { LgXrSettings warp = LG_XR_SETTINGS_DEFAULT; xr_settings_set(&warp, "space_warp", "on"); CHECK(warp.space_warp == 1); xr_settings_set(&warp, "space_warp", "head"); CHECK(warp.space_warp == 2); xr_settings_set(&warp, "space_warp", "nohead"); CHECK(warp.space_warp == 3); xr_settings_set(&warp, "space_warp", "flat"); CHECK(warp.space_warp == 4); xr_settings_set(&warp, "space_warp", "flatnohead"); CHECK(warp.space_warp == 5); xr_settings_set(&warp, "space_warp", "off"); CHECK(warp.space_warp == 0); }
    LgXrSettings parsed = LG_XR_SETTINGS_DEFAULT;
    xr_settings_set(&parsed, "fxaa", "off"); xr_settings_set(&parsed, "post_shading", "coarse"); xr_settings_set(&parsed, "merge", "off"); xr_settings_set(&parsed, "glow", "off"); xr_settings_set(&parsed, "sharpen", "high");
    CHECK(parsed.fxaa == 0 && parsed.post_shading == 1 && parsed.merge == 0 && parsed.glow == 0 && parsed.sharpen == 3);
    xr_settings_set(&parsed, "fxaa", "on"); xr_settings_set(&parsed, "post_shading", "full"); xr_settings_set(&parsed, "merge", "on");
    CHECK(parsed.fxaa == 1 && parsed.post_shading == 0 && parsed.merge == 1);

    fs::current_path(fs::temp_directory_path());
    fs::remove_all(dir);
    if (failures) return 1;
    std::puts("test_xr_panel: ok");
    return 0;
}
