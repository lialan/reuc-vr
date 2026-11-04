#pragma once
// The headset output on Android (xr_android.cpp); nothing elsewhere.
#include <memory>

namespace uc {
class XrCompositor;
class NativeInput;
#ifdef UC_OPENXR
std::unique_ptr<XrCompositor> make_android_xr();
// The game thread, every main-loop pass: the headset controllers as the game's Wii Remote (HOTD4's host_vr_input for this game).
void feed_xr_input(XrCompositor& xr, NativeInput& input);
// Called only after uc_game_main has destroyed the renderer and OpenXR session.
bool complete_android_xr_restart();
#endif
}  // namespace uc
