#pragma once
// Vulkan 1.2/1.3 device functions on Android (vulkan_android_entry.cpp): call once with the renderer's device right after creating it.
#include <vulkan/vulkan.h>

namespace uc {
#ifdef __ANDROID__
void vk_android_load_device(VkDevice device);
#else
inline void vk_android_load_device(VkDevice) {}
#endif
}  // namespace uc
