#pragma once
#ifdef __cplusplus
extern "C" {
#endif
enum { LASER_ON, LASER_DOT, LASER_OFF };
typedef struct {
    float scale,screen,aim;
    int resolution,width,refresh,left_handed,laser,auto_coins;
    int sight,crosshair,show_gun,hud_pitch,dual,show_stats;
    int foveation;              /* 0 off, 1 low, 2 medium, 3 high (Vulkan host) */
    int version;                /* vr.cfg's defaults generation (2: Umbrella Chronicles' Quest eye size) */
} LgXrSettings;
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
/* Umbrella Chronicles on the Quest: eye images 1440 high (the width follows the headset's proportions, 1346), one of the panel's sizes.
 * The game's picture is 960x540 an eye at the headset's default render scale, so the panel's 2064x2208 only cost the eye pass and the
 * compositor bandwidth (about 1 ms a frame) for no detail; native stays a panel choice. */
#if defined(__ANDROID__)
#define LG_XR_DEFAULT_WIDTH  0
#define LG_XR_DEFAULT_HEIGHT 1440
#else
#define LG_XR_DEFAULT_WIDTH  LG_XR_NATIVE_WIDTH
#define LG_XR_DEFAULT_HEIGHT LG_XR_NATIVE_HEIGHT
#endif
/* ... dual off, stats panel on (vr.cfg "stats off" hides it), medium foveation */
#define LG_XR_SETTINGS_DEFAULT {1.0f,3.0f,15.0f,LG_XR_DEFAULT_HEIGHT,LG_XR_DEFAULT_WIDTH,120,0,LASER_ON,1,0,1,1,0,0,1,3,2}
void xr_settings_load(LgXrSettings *settings);
void xr_settings_set(LgXrSettings *settings,const char *key,const char *value);
void xr_settings_save(const LgXrSettings *settings);
#ifdef __cplusplus
}
#endif
