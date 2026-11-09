#pragma once
#include <stdint.h>
#include "xr_settings.h"
#include "xr_menu.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { XP_SIGHT,XP_LASER,XP_CROSSHAIR,XP_GUN,XP_HAND,XP_DUAL,XP_STATS,XP_SCREEN,XP_HUD,XP_RATE,XP_DISPLAY,XP_RECENTRE,XP_RESET,XP_CLOSE,XP_COUNT };
/* The Display page (XP_DISPLAY opens it): the picture's size and what the GPU spends on it, all but the eye size applied live. */
enum { XD_RES,XD_SCALE,XD_FOVEATION,XD_FXAA,XD_POST,XD_MERGE,XD_BACK,XD_COUNT };
#define XP_MAX_ROWS ((int)XP_COUNT>(int)XD_COUNT?(int)XP_COUNT:(int)XD_COUNT)
/* Umbrella Chronicles: HOTD4's arcade rows (Coins, Insert a coin, Arcade Test, Arcade Service) are left out - a Wii game has no coin
 * slot or cabinet buttons. Every other row is HOTD4's, in its order, but HOTD4's "Resolution per eye" and "Foveation" moved to the
 * Display page with "Render scale" (the game's own picture per eye; HOTD4 draws into the eye image directly and has none), "Edge
 * smoothing", "Post-effect shading" and "Draw merging". Foveation here foveates the game's own picture (renderer/fovea_map.h). */
enum { XP_SAVE=1,XP_CROSSHAIR_CHANGED=2,XP_SCREEN_CHANGED=4,XP_HUD_CHANGED=8,XP_RATE_CHANGED=16,XP_RECENTER=32,XP_RESTART=64,XP_HAND_CHANGED=128,XP_FOVEATION_CHANGED=256,XP_SCALE_CHANGED=512 };
typedef struct {
    int open,hover,dirty;float c[3],right[3],up[3],n[3],hw,hh;
    int test_frames,service_frames,coin,reset_armed;
    int page;   /* 0 the main rows, 1 the Display page */
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
