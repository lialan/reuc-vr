#pragma once
// The headset output on Android (xr_android.cpp); nothing elsewhere.
#include <memory>

namespace uc {
class XrCompositor;
#ifdef UC_OPENXR
std::unique_ptr<XrCompositor> make_android_xr();
#endif
}  // namespace uc
