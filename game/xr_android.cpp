// The headset output on Android (Quest): the OpenXR compositor from HOTD4 VR's settings (vr.cfg in the app folder, xr/xr_settings.c; the
// same keys, defaults and environment overrides as HOTD4), created with the activity SDL runs in. UC_XR=0 starts without it (diagnostics).
#include "xr_android.h"
#ifdef UC_OPENXR
#include "xr_compositor.h"
#include "xr_settings.h"
#include <SDL3/SDL.h>
#include <jni.h>
#include <cstdio>
#include <cstdlib>

namespace uc {
std::unique_ptr<XrCompositor> make_android_xr() {
    if (const char* v = std::getenv("UC_XR"); v && *v == '0') return nullptr;
    LgXrSettings settings = LG_XR_SETTINGS_DEFAULT;
    xr_settings_load(&settings);
    XrConfig config;
    config.width = uint32_t(settings.width); config.height = uint32_t(settings.resolution);   // HOTD4: resolution is the eye height
    config.refresh = float(settings.refresh);
    config.foveation_level = settings.foveation;
    config.stereo.scale = settings.scale; config.stereo.screen = settings.screen; config.stereo.hud_pitch = float(settings.hud_pitch);
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    JavaVM* vm = nullptr;
    if (!env || env->GetJavaVM(&vm) != JNI_OK) throw std::runtime_error("OpenXR: no Java VM");
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) throw std::runtime_error("OpenXR: no activity");
    jobject held = env->NewGlobalRef(activity);   // OpenXR keeps it for the process: never released
    env->DeleteLocalRef(activity);
    std::fprintf(stderr, "[xr] vr.cfg: eye %dx%d, %d Hz, foveation %d, screen %.2f m, scale %.2f, aim %.1f m\n", settings.width, settings.resolution,
                 settings.refresh, settings.foveation, settings.screen, settings.scale, settings.aim);
    return std::make_unique<XrCompositor>(config, XrPlatform{vm, held});
}
}  // namespace uc
#endif
