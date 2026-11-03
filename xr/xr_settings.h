#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { LASER_ON, LASER_DOT, LASER_OFF };
typedef struct {
    float scale,screen,aim;
    int resolution,width,refresh,left_handed,laser,auto_coins;
    int sight,crosshair,show_gun,hud_pitch,dual,show_stats;
    int foveation;              /* 0 off, 1 low, 2 medium, 3 high: Umbrella Chronicles' foveated shading of its own picture (renderer/fovea_map.h) */
    int version;                /* vr.cfg's defaults generation (2: Umbrella Chronicles' Quest eye size) */
    float render_scale;         /* Umbrella Chronicles: the eyes' scene scale (720p multiplier), 0 = native (the eye image's rows); applied live */
} LgXrSettings;
/* The settings panel's render scales after native, largest first */
#define LG_XR_RENDER_SCALES {2.0f,1.5f,1.25f,1.0f}
#define LG_XR_RENDER_SCALE_COUNT 4
/* Each eye's image by default: the headset's own panel, pixel for pixel.
 * OpenXR reports only a runtime-scaled recommendation, so the panel is the
 * build's: Quest 3 (Android) 2064x2208 per eye, Steam Frame (native Linux)
 * 2160x2160 per eye. Other hosts keep the earlier 2064x2280. */
#if defined(__ANDROID__)
#define LG_XR_NATIVE_WIDTH  2064
#define LG_XR_NATIVE_HEIGHT 2208
#elif defined(__linux__)
#define LG_XR_NATIVE_WIDTH  2160
#define LG_XR_NATIVE_HEIGHT 2160
#else
#define LG_XR_NATIVE_WIDTH  2064
#define LG_XR_NATIVE_HEIGHT 2280
#endif
/* Umbrella Chronicles on the Quest (version 4): native eye images, and the game renders each eye at that size (VulkanRenderer: the scene's
 * rows are the eye image's), so nothing is resampled on the way to the display. */
#define LG_XR_DEFAULT_WIDTH  LG_XR_NATIVE_WIDTH
#define LG_XR_DEFAULT_HEIGHT LG_XR_NATIVE_HEIGHT
#define LG_XR_OLD_QUEST_HEIGHT 1440   /* the version 2-3 default eye height on the Quest: moves to native */
/* ... dual off, stats panel on (vr.cfg "stats off" hides it), medium foveation */
/* foveation off (version 3): the game draws its own picture and only the screen pass renders into the eye image, so foveating it saved
 * nothing and coarsened the picture's edges */
/* game crosshair hidden (version 5): the laser is the aim; the game's reticle sat a little off it in the headset */
#define LG_XR_SETTINGS_DEFAULT {1.0f,3.0f,15.0f,LG_XR_DEFAULT_HEIGHT,LG_XR_DEFAULT_WIDTH,120,0,LASER_ON,1,0,0,1,0,0,1,0,5,0.0f}
void xr_settings_load(LgXrSettings *settings);
void xr_settings_set(LgXrSettings *settings,const char *key,const char *value);
void xr_settings_save(const LgXrSettings *settings);
#ifdef __cplusplus
}
#endif
