// The headset output on Android (Quest): the OpenXR compositor from HOTD4 VR's settings (vr.cfg in the app folder, xr/xr_settings.c; the
// same keys, defaults and environment overrides as HOTD4), created with the activity SDL runs in. UC_XR=0 starts without it (diagnostics).
#include "xr_android.h"
#ifdef UC_OPENXR
#include "xr_compositor.h"
#include "xr_settings.h"
#include "cabinet_buttons.h"
#include "native_input.h"
#include "gamepad_math.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <SDL3/SDL.h>
#include <jni.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <functional>

namespace uc {
namespace {
std::atomic<bool> restart_requested{false};
std::function<void()> restart_activity;
}
bool complete_android_xr_restart() {
    if (!restart_requested.load() || !restart_activity) return false;
    std::fprintf(stderr,"[android] reset: native shutdown complete; relaunching\n");
    restart_activity();
    return true;
}
std::unique_ptr<XrCompositor> make_android_xr() {
    if (const char* v = std::getenv("UC_XR"); v && *v == '0') return nullptr;
    LgXrSettings settings = LG_XR_SETTINGS_DEFAULT;
    // The game's size of a metre, as HOTD4's android_main sets it for its games (Let's Go Jungle: decimetres): Umbrella Chronicles' world is in
    // decimetres too (the camera's eye is 15 units above the floor, scripts/uctest camera_api), so 10 units per metre.
    setenv("LINDBERGH_XR_SCALE", "10", 0);
    xr_settings_load(&settings);
    // A vr.cfg the panel saved before this default holds HOTD4's 1 (metres): that meant "the default", which is now 10 (as xr_settings_load
    // treats the old 2064x2280 eye size).
    if (settings.scale == 1.0f) settings.scale = 10.0f;
    XrConfig config;
    config.width = uint32_t(settings.width); config.height = uint32_t(settings.resolution);   // HOTD4: resolution is the eye height
    // With an explicit render scale the game's picture has 720 x scale rows: eye images taller than that only upscale it, and that pass costs
    // GPU time in every frame (r020 at 2.25: 2064x2208 eyes 3.0 ms, 1603x1680 1.9 ms; slowest 5% 40 -> 60 fps). The eyes take the picture's
    // size then (the headset's compositor scales to the display anyway); Native keeps the chosen eye size, which the scene follows.
    if (settings.render_scale > 0) {
        const int rows = int(720 * settings.render_scale + .5f);
        if (rows < settings.resolution) {
            if (settings.width > 0) config.width = uint32_t(float(settings.width) * rows / settings.resolution + .5f);
            config.height = uint32_t(rows);
            std::fprintf(stderr, "[xr] eye images %ux%u: the picture's size at render scale %.3g (chosen %d rows)\n", config.width, config.height,
                         settings.render_scale, settings.resolution);
        }
    }
    config.refresh = float(settings.refresh);
    config.foveation_level = settings.foveation;
    // UC_CPU_BOOST=1: the runtime's boost CPU level (2.36 GHz on the Quest 3's fast cores) instead of sustained high (2.21 GHz); the
    // runtime may throttle it when the headset heats up. For measurements.
    if (const char* boost = std::getenv("UC_CPU_BOOST")) config.cpu_boost = *boost == '1';
    config.stereo.scale = settings.scale; config.stereo.screen = settings.screen; config.stereo.hud_pitch = float(settings.hud_pitch);
    config.stereo.hud_fit = true;   // gameplay HUD at the display's edges, following the head (StereoScene); menus keep HOTD4's screen
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    JavaVM* vm = nullptr;
    if (!env || env->GetJavaVM(&vm) != JNI_OK) throw std::runtime_error("OpenXR: no Java VM");
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!activity) throw std::runtime_error("OpenXR: no activity");
    jobject held = env->NewGlobalRef(activity);   // OpenXR keeps it for the process: never released
    env->DeleteLocalRef(activity);
    config.settings = settings;
    restart_activity = [vm, held] {
        JNIEnv* thread_env = nullptr;
        if (vm->AttachCurrentThread(&thread_env, nullptr) != JNI_OK || !thread_env) return;
        jclass cls = thread_env->GetObjectClass(held);
        if (jmethodID restart = thread_env->GetMethodID(cls, "restartGame", "()V")) thread_env->CallVoidMethod(held, restart);
        if (thread_env->ExceptionCheck()) { thread_env->ExceptionDescribe(); thread_env->ExceptionClear(); }
        thread_env->DeleteLocalRef(cls);
    };
    config.restart = [] {
        if (restart_requested.exchange(true)) return;
        // Exit through the main loop so guest/file workers, audio, Vulkan and
        // OpenXR finish before Android starts a fresh process.
        SDL_Event quit{}; quit.type=SDL_EVENT_QUIT;
        if (!SDL_PushEvent(&quit)) restart_requested=false;
        else std::fprintf(stderr,"[android] reset: orderly shutdown requested\n");
    };
    config.game_motors = [] { return input_rumble_local(); };   // the game's rumble (runtime/native_input.cpp) on the gun hand
    std::fprintf(stderr, "[xr] vr.cfg: eye %dx%d, %d Hz, foveation %d, screen %.2f m, scale %.2f, aim %.1f m\n", settings.width, settings.resolution,
                 settings.refresh, settings.foveation, settings.screen, settings.scale, settings.aim);
    return std::make_unique<XrCompositor>(config, XrPlatform{vm, held});
}

void feed_xr_input(XrCompositor& xr, NativeInput& input) {
    xr::ControllerSnapshot snapshot;
    if (!xr.consume(snapshot)) return;
    // The aim: the scene hit projected into the game's camera (-1..1, y down). In gameplay a ray past the picture still aims: the pointer
    // stays at the picture's edge and the shot follows the controller's ray (xr_shot.cpp). In menus, off the screen the pointer stays put.
    const auto& ray = snapshot.ray[0];
    if (snapshot.valid[0] && ray.x >= 0 && ray.y >= 0 && ray.x < XrCompositor::kAimWidth && ray.y < XrCompositor::kAimHeight)
        input.host_xr_pointer(2.f * (ray.x + .5f) / XrCompositor::kAimWidth - 1, 2.f * (ray.y + .5f) / XrCompositor::kAimHeight - 1,
                             TrackedRay{snapshot.shot[0].origin,snapshot.shot[0].direction});
    // Swings: the knife (hold A and swing) and the shake prompts read the Wii Remote's pushes as mouse counts (host_wii_motion). The gun's turn
    // since the last sample, as yaw right / pitch down in kGyroCountsPerRadian (a gyro pad's scale): a flick of ~2 rad/s reaches kKnifeSpeed,
    // careful aiming (under 1 rad/s) does not. The first sample after the ray was lost only sets the start.
    static std::array<float,3> last_direction{};
    static bool have_direction = false;
    if (snapshot.valid[0]) {
        const auto& d = ray.direction;
        if (have_direction) {
            const auto& p = last_direction;
            float yaw = std::atan2(d[0], -d[2]) - std::atan2(p[0], -p[2]);
            if (yaw > 3.14159265f) yaw -= 6.2831853f; else if (yaw < -3.14159265f) yaw += 6.2831853f;
            const float pitch = std::asin(std::clamp(d[1], -1.f, 1.f)) - std::asin(std::clamp(p[1], -1.f, 1.f));
            const float dx = yaw * kGyroCountsPerRadian, dy = -pitch * kGyroCountsPerRadian;
            if (dx != 0 || dy != 0) input.host_wii_motion(dx, dy, 0);
        }
        last_direction = d; have_direction = true;
    } else {
        have_direction = false;
    }
    // HOTD4's cabinet buttons as the remote's, by role (the original game's Wii layout, input_bindings.cpp default_wii_bindings):
    // trigger = B (fire), reload = 1, action (gun hand A/X) = A, fire mode (gun hand B/Y) = + (next weapon), start (stick click) = Home (pause).
    struct Map { unsigned cabinet; uint16_t wii; };
    static constexpr Map kMap[] = {{CAB_P1_TRIGGER, 7}, {CAB_P1_RELOAD, 6}, {CAB_P1_ACTION, 8}, {CAB_P1_FIREMODE, 4}, {CAB_P1_START, 10}};
    static unsigned previous = 0;
    // HOTD4's trigger also selects in its menus. The Wii game fires with B but confirms with A, and B is "back" in its menus: a trigger pull
    // on a menu (the save prompts) answered "no". So in a menu the trigger is A; the button a pull pressed is the one its release lets go.
    static uint16_t trigger_button = 7;
    for (const auto& m : kMap) {
        const bool down = (snapshot.held & m.cabinet) != 0, was = (previous & m.cabinet) != 0;
        uint16_t wii = m.wii;
        if (m.cabinet == CAB_P1_TRIGGER) {
            if (down && !was) trigger_button = input.current_context() == InputContext::menu ? 8 : 7;
            wii = trigger_button;
        }
        if (down != was) input.host_button(PhysicalInput{PhysicalKind::wii_button, wii}, down);
        // Diagnostics for "the trigger did not shoot": every press of fire (aimed: on the game's picture), reload (the trigger off the
        // picture, or grip) and A, with the ray's place on the kAimWidth x kAimHeight picture. [kpad] in native_input.cpp shows what the game read.
        if (down && !was && (m.cabinet & (CAB_P1_TRIGGER | CAB_P1_RELOAD | CAB_P1_ACTION)))
            std::fprintf(stderr, "[xr-input] %s (Wii %s) ray %s %d,%d held %04X\n",
                         m.cabinet == CAB_P1_TRIGGER ? "trigger" : m.cabinet == CAB_P1_RELOAD ? "reload (trigger off the picture, or grip)" : "action",
                         wii == 8 ? "A" : wii == 7 ? "B" : wii == 6 ? "1" : "?", snapshot.valid[0] ? "valid" : "invalid", ray.x, ray.y, snapshot.held);
    }
    previous = snapshot.held;
}
}  // namespace uc
#endif
