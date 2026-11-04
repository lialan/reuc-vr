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
/* Version 7 on the Quest: the scene at render scale 1.0 (720 rows an eye) into 1548x1656 eye images (0.75 of the panel; the runtime
 * scales them to the panel). Live train fight, foveation high: render scale 1.0 59 fps mean (p50 60), 1.25 56, 1.5 54 (the GPU's work
 * plus the compositor's 120 Hz timewarp leave no slack above 1.0 yet). fightvr replay GPU per frame: 1.0 9.2 ms, 1.25 10.1, 1.5 11.4,
 * native / native eyes 24.5. The eye images' size also sets the HUD and glow layers and the screen pass (~2.8 ms of the native size). */
#if defined(__ANDROID__)
#define LG_XR_DEFAULT_WIDTH  1548
#define LG_XR_DEFAULT_HEIGHT 1656
#define LG_XR_DEFAULT_RENDER_SCALE 1.5f   /* version 9: live fight at 90 Hz holds 60 fps (59.5 mean, 5th percentile 60); replay GPU 9.4 ms */
#else
#define LG_XR_DEFAULT_WIDTH  LG_XR_NATIVE_WIDTH
#define LG_XR_DEFAULT_HEIGHT LG_XR_NATIVE_HEIGHT
#define LG_XR_DEFAULT_RENDER_SCALE 0.0f
#endif
#define LG_XR_OLD_QUEST_HEIGHT 1440   /* the version 2-3 default eye height on the Quest: moves to native */
/* ... dual off, stats panel on (vr.cfg "stats off" hides it), medium foveation */
/* foveation off (version 3): the game draws its own picture and only the screen pass renders into the eye image, so foveating it saved
 * nothing and coarsened the picture's edges */
/* game crosshair hidden (version 5): the laser is the aim; the game's reticle sat a little off it in the headset */
/* high foveation (version 6): the game's own scene passes are foveated (renderer/fovea_map.h), most of the GPU time; 60 fps first */
/* render scale 1.0 and 1548x1656 eye images on the Quest (version 7): 60 fps (see LG_XR_DEFAULT_WIDTH) */
/* display refresh 90 Hz (version 8): at 120 Hz the compositor's work per refresh left frames missing their slot (40 fps); at 90 the live
 * fight holds 60 */
#define LG_XR_DEFAULT_REFRESH 90
/* game crosshair shown again (version 10): with it hidden the owner could not aim properly by the laser alone */
#define LG_XR_SETTINGS_DEFAULT {1.0f,3.0f,15.0f,LG_XR_DEFAULT_HEIGHT,LG_XR_DEFAULT_WIDTH,LG_XR_DEFAULT_REFRESH,0,LASER_ON,1,0,1,1,0,0,1,3,10,LG_XR_DEFAULT_RENDER_SCALE}
void xr_settings_load(LgXrSettings *settings);
void xr_settings_set(LgXrSettings *settings,const char *key,const char *value);
void xr_settings_save(const LgXrSettings *settings);
#ifdef __cplusplus
}
#endif
