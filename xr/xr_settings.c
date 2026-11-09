#include "xr_settings.h"
#include "xr_overlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static const char *const LASER_NAME[]={"laser","dot","off"};
static const char *const SIGHT_NAME[]={"none","reddot","holo"};
static const char *const FOVEATION_NAME[]={"off","low","medium","high"};
void xr_settings_save(const LgXrSettings *c)
{
    FILE *f = fopen("vr.cfg", "w");
    if (!f) return;
    fprintf(f, "scale %g\nscreen %g\naim %g\nresolution %d\nwidth %d\nrefresh %d\nhand %s\nlaser %s\ncoins %s\n"
               "sight %s\ncrosshair %s\ngun %s\nhud_pitch %d\ndual %s\nstats %s\nfoveation %s\nversion %d\n",
            c->scale, c->screen, c->aim, c->resolution, c->width, c->refresh, c->left_handed ? "left" : "right",
            LASER_NAME[c->laser], c->auto_coins ? "auto" : "manual",
            SIGHT_NAME[c->sight], c->crosshair ? "on" : "off", c->show_gun ? "on" : "off", c->hud_pitch, c->dual ? "on" : "off", c->show_stats ? "on" : "off",
            FOVEATION_NAME[c->foveation < 0 || c->foveation > 3 ? 3 : c->foveation], c->version);
    if (c->render_scale > 0) fprintf(f, "render_scale %g\n", c->render_scale);
    else fprintf(f, "render_scale native\n");
    fprintf(f, "fxaa %s\npost_shading %s\nmerge %s\nglow %s\n", c->fxaa ? "on" : "off", c->post_shading ? "coarse" : "full", c->merge ? "on" : "off",
            c->glow ? "on" : "off");
    fclose(f);
}

void xr_settings_load(LgXrSettings *c)
{
    /* the game's own size of a metre, until vr.cfg says otherwise: set by
     * its android_main (Let's Go Jungle's world is in about decimetres) */
    { const char *v = getenv("LINDBERGH_XR_SCALE"); if (v && atof(v) > 0) c->scale = (float)atof(v); }
    FILE *f = fopen("vr.cfg", "r");
    if (!f) f = fopen("../vr.cfg", "r");    /* beside the game: where adb can put one */
    char key[32], val[32];
    if (f) c->version = 0;   /* a saved file says which defaults it was written with ("version"; none before 2) */
    while (f && fscanf(f, "%31s %31s", key, val) == 2) xr_settings_set(c,key,val);
    if (f) fclose(f);
    /* written before version 2, the eye size the panel saved was the then default, the headset's native size: now the default is smaller */
    if (c->version < 2 && c->width == LG_XR_NATIVE_WIDTH && c->resolution == LG_XR_NATIVE_HEIGHT) {
        c->resolution = LG_XR_DEFAULT_HEIGHT; c->width = LG_XR_DEFAULT_WIDTH;
    }
    /* written before version 3, high foveation was the default: now off (the eye image holds only the screen pass) */
    if (c->version < 3 && c->foveation == 3) c->foveation = 0;
    /* written before version 4, the Quest's default eye height was 1440: now native */
    if (c->version < 4 && c->resolution == LG_XR_OLD_QUEST_HEIGHT && c->width == 0) { c->resolution = LG_XR_NATIVE_HEIGHT; c->width = LG_XR_NATIVE_WIDTH; }
    /* written before version 5, the game's crosshair was shown: now hidden (the laser aims; the reticle sat a little off it) */
    if (c->version < 5) c->crosshair = 0;
    /* written before version 6, foveation was off by default: now high (it foveates the game's scene passes, most of the GPU time; 60 fps first) */
    if (c->version < 6) c->foveation = 3;
    /* written before version 7, the defaults were native eye images and the native scene: now the 60 fps defaults (render scale 1.0, 0.75 eye images) */
    if (c->version < 7) {
        if (c->render_scale == 0.0f) c->render_scale = LG_XR_DEFAULT_RENDER_SCALE;
        if (c->resolution == LG_XR_NATIVE_HEIGHT && (c->width == LG_XR_NATIVE_WIDTH || c->width == 0)) { c->resolution = LG_XR_DEFAULT_HEIGHT; c->width = LG_XR_DEFAULT_WIDTH; }
    }
    /* written before version 8, the display ran at 120 Hz by default: now 90 (live train fight at render scale 1.0: 120 Hz 58.6 fps mean, 5th
     * percentile 40; 90 Hz 59.6, 5th percentile 60, GPU ~60% busy: the compositor's per-refresh work at 120 Hz left the game no slack) */
    if (c->version < 8 && c->refresh == 120) c->refresh = LG_XR_DEFAULT_REFRESH;
    /* written before version 9, render scale 1.0 was the default: now 1.5 (GPU room after the merge key and 90 Hz; live fight still 60) */
    if (c->version < 9 && c->render_scale == 1.0f) c->render_scale = LG_XR_DEFAULT_RENDER_SCALE;
    /* written before version 10, the game crosshair was hidden by default: shown again (aiming by the laser alone was not enough) */
    if (c->version < 10) c->crosshair = 1;
#if defined(__ANDROID__)
    /* Move the old performance defaults once; retain other explicit choices.
     * This also applies before the first panel save, through the new defaults. */
    if (c->version < 11) {
        if (c->width == 1548 && c->resolution == 1656) {
            c->width = LG_XR_DEFAULT_WIDTH; c->resolution = LG_XR_DEFAULT_HEIGHT;
        }
        if (c->render_scale == 1.5f) c->render_scale = LG_XR_DEFAULT_RENDER_SCALE;
        if (c->foveation == 3) c->foveation = LG_XR_DEFAULT_FOVEATION;
    }
    /* Upgrade the old off default once; a subsequent explicit Off is retained. */
    if (c->version < 12 && c->foveation == 0) c->foveation = LG_XR_DEFAULT_FOVEATION;
    /* Version 13 (GPU time): the medium default moves to high once, FXAA goes off once; later choices on the Display page are kept. */
    if (c->version < 13) {
        if (c->foveation == 2) c->foveation = LG_XR_DEFAULT_FOVEATION;
        c->fxaa = LG_XR_DEFAULT_FXAA;
    }
    /* Version 14 (GPU time): the gun model goes off once; Gun model on the main page brings it back and that choice is kept. */
    if (c->version < 14) c->show_gun = LG_XR_DEFAULT_SHOW_GUN;
#endif
    c->version = LG_XR_SETTINGS_VERSION;
    /* LINDBERGH_XR_SET="key=value,...": for this run only, not saved (testing) */
    const char *e = getenv("LINDBERGH_XR_SET");
    while (e && *e) {
        if (sscanf(e, "%31[^=]=%31[^,]", key, val) == 2) xr_settings_set(c,key,val);
        e = strchr(e, ',');
        if (e) e++;
    }
    if (c->scale <= 0) c->scale = 1;
    if (c->screen <= 0.2f) c->screen = 3;
    if (c->aim < 1.0f || c->aim > 200.0f) c->aim = 15.0f;
    if (c->resolution < 512 || c->resolution > 4096 || c->width < 0 || c->width > 4096) {
        c->resolution = LG_XR_DEFAULT_HEIGHT; c->width = LG_XR_DEFAULT_WIDTH;
    }
    /* 2064x2280 was every build's default and is in every vr.cfg the panel
     * ever saved: it meant "the default", which is now the panel's own */
    if (c->width == 2064 && c->resolution == 2280) {
        c->resolution = LG_XR_DEFAULT_HEIGHT; c->width = LG_XR_DEFAULT_WIDTH;
    }
}

void xr_settings_set(LgXrSettings *c,const char *key,const char *val)
{
    {
        if (!strcmp(key, "scale")) c->scale = (float)atof(val);
        else if (!strcmp(key, "screen")) c->screen = (float)atof(val);
        else if (!strcmp(key, "hud_pitch")) c->hud_pitch = atoi(val);
        else if (!strcmp(key, "aim")) c->aim = (float)atof(val);
        else if (!strcmp(key, "resolution")) c->resolution = atoi(val);
        else if (!strcmp(key, "width")) c->width = atoi(val);
        else if (!strcmp(key, "refresh")) c->refresh = atoi(val);
        else if (!strcmp(key, "hand")) c->left_handed = !strcmp(val, "left");
        else if (!strcmp(key, "laser")) c->laser = !strcmp(val, "dot") ? LASER_DOT : !strcmp(val, "off") ? LASER_OFF : LASER_ON;
        else if (!strcmp(key, "coins")) c->auto_coins = strcmp(val, "manual") != 0;
        else if (!strcmp(key, "sight")) c->sight = !strcmp(val, "reddot") ? XO_SIGHT_RED_DOT : !strcmp(val, "holo") ? XO_SIGHT_HOLO : XO_SIGHT_NONE;
        else if (!strcmp(key, "crosshair")) c->crosshair = strcmp(val, "off") != 0;
        else if (!strcmp(key, "gun")) c->show_gun = strcmp(val, "off") != 0;
        else if (!strcmp(key, "dual")) c->dual = !strcmp(val, "on");
        else if (!strcmp(key, "stats") || !strcmp(key, "fps")) c->show_stats = !strcmp(val, "on");   /* "fps": before it was the stats panel */
        else if (!strcmp(key, "version")) c->version = atoi(val);
        else if (!strcmp(key, "render_scale")) {   /* "native" (0) or a 720p multiplier 0.5..6 */
            const float scale = !strcmp(val, "native") ? 0.0f : (float)atof(val);
            c->render_scale = scale >= 0.5f && scale <= 6.0f ? scale : 0.0f;
        }
        else if (!strcmp(key, "fxaa")) c->fxaa = strcmp(val, "off") != 0;
        else if (!strcmp(key, "post_shading")) c->post_shading = !strcmp(val, "coarse");
        else if (!strcmp(key, "merge")) c->merge = strcmp(val, "off") != 0;
        else if (!strcmp(key, "glow")) c->glow = strcmp(val, "off") != 0;
        else if (!strcmp(key, "foveation")) {
            for (int i = 0; i < 4; i++) if (!strcmp(val, FOVEATION_NAME[i])) c->foveation = i;
        }
    }
}
