#pragma once
#include <stdint.h>
#include "xr_settings.h"
#include "xr_menu.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { XP_SIGHT,XP_LASER,XP_CROSSHAIR,XP_GUN,XP_HAND,XP_DUAL,XP_STATS,XP_SCREEN,XP_HUD,XP_RATE,XP_DISPLAY,XP_CONTROLS,XP_RECENTRE,XP_RESET,XP_CLOSE,XP_COUNT };
/* The Display page (XP_DISPLAY opens it): the picture's size and what the GPU spends on it, all but the eye size applied live. */
enum { XD_PRESET,XD_RES,XD_SCALE,XD_FOVEATION,XD_FXAA,XD_SHARPEN,XD_POST,XD_MERGE,XD_GLOW,XD_SPACE_WARP,XD_BACK,XD_COUNT };
/* The Controls page (XP_CONTROLS opens it): what each Touch input does in the game, read only, and the Vibration switch. */
enum { XC_FIRE,XC_RELOAD,XC_ACTION,XC_KNIFE,XC_GRENADE,XC_WEAPON,XC_SHAKE,XC_PAUSE,XC_MENU,XC_HAND,XC_VIBRATION,XC_BACK,XC_COUNT };
#define XP_MAX2(a,b) ((int)(a)>(int)(b)?(int)(a):(int)(b))
#define XP_MAX_ROWS XP_MAX2(XP_COUNT,XP_MAX2(XD_COUNT,XC_COUNT))
/* Umbrella Chronicles: HOTD4's arcade rows (Coins, Insert a coin, Arcade Test, Arcade Service) are left out - a Wii game has no coin
 * slot or cabinet buttons. Every other row is HOTD4's, in its order, but HOTD4's "Resolution per eye" and "Foveation" moved to the
 * Display page with "Render scale" (the game's own picture per eye; HOTD4 draws into the eye image directly and has none), "Edge
 * smoothing", "Post-effect shading" and "Draw merging". Foveation here foveates the game's own picture (renderer/fovea_map.h). */
enum { XP_SAVE=1,XP_CROSSHAIR_CHANGED=2,XP_SCREEN_CHANGED=4,XP_HUD_CHANGED=8,XP_RATE_CHANGED=16,XP_RECENTER=32,XP_RESTART=64,XP_HAND_CHANGED=128,XP_FOVEATION_CHANGED=256,XP_SCALE_CHANGED=512,XP_SPACE_WARP_CHANGED=1024 };
/* The Display page's presets: each sets the display rate, space warp, render scale and foveation together (measured on the Quest 3, live
 * train fight: docs/spikes/S23-quest-spacewarp.md). render_scale 0 is native. */
typedef struct { const char *name; int refresh,space_warp,foveation; float render_scale; } XpPreset;
#define XP_PRESET_COUNT 3
extern const XpPreset xp_presets[XP_PRESET_COUNT];
int xp_preset_of(const LgXrSettings *settings);   /* the preset the settings match, -1 none (Custom) */
typedef struct {
    int open,hover,dirty;float c[3],right[3],up[3],n[3],hw,hh;
    int test_frames,service_frames,coin,reset_armed;
    int page;   /* 0 the main rows, 1 the Display page, 2 the Controls page */
} XpPanel;
int xp_count(const XpPanel *panel);   /* rows on the shown page */
void xp_items(const XpPanel *panel,const LgXrSettings *settings,int hand,int hud_locked,int running_resolution,int running_width,XmItem *items);
unsigned xp_choose(XpPanel *panel,LgXrSettings *settings,int *hand,int item,const float *rates,int nrates);
void xp_place(XpPanel *panel,const float head[3],const float orientation[4]);
int xp_hit(const XpPanel *panel,const float origin[3],const float direction[3],float *u,float *v,float hit[3]);
void xp_matrix(const XpPanel *panel,float model[16]);
int xp_menu_button(XpPanel *panel,int pressed,int previous);
int xp_hold_button(XpPanel *panel,int pressed,int previous,int dual,int64_t time,int64_t *held_since);
#ifdef __cplusplus
}
#endif
