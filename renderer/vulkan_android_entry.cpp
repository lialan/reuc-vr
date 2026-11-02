// Android's libvulkan.so exports only the Vulkan 1.0/1.1 entry points; the renderer calls a few 1.2/1.3 device functions directly.
// These definitions stand in for the missing exports and forward to the device's own functions, resolved once the renderer has its
// device (vk_android_load_device). Every call happens after that, on that device (one renderer device per process).
#include "vulkan_android_entry.h"
#include <cstdio>
#include <cstdlib>

namespace {
#define UC_VK_DEVICE_FUNCTIONS(X) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier2) X(vkCmdSetCullMode) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdWriteTimestamp2) X(vkGetBufferDeviceAddress) X(vkQueueSubmit2)
#define UC_VK_POINTER(name) PFN_##name p_##name = nullptr;
UC_VK_DEVICE_FUNCTIONS(UC_VK_POINTER)
#undef UC_VK_POINTER
[[noreturn]] void missing(const char* name) {
    std::fprintf(stderr, "Vulkan: %s called before the device was loaded or the device lacks it\n", name);
    std::abort();
}
}  // namespace

namespace uc {
void vk_android_load_device(VkDevice device) {
#define UC_VK_LOAD(name) p_##name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    UC_VK_DEVICE_FUNCTIONS(UC_VK_LOAD)
#undef UC_VK_LOAD
}
}  // namespace uc

#define UC_VK_CALL(name, ...) do { if (!p_##name) missing(#name); return p_##name(__VA_ARGS__); } while (0)
extern "C" {
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer b, const VkRenderingInfo* i) { UC_VK_CALL(vkCmdBeginRendering, b, i); }
VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer b) { UC_VK_CALL(vkCmdEndRendering, b); }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer b, const VkDependencyInfo* d) { UC_VK_CALL(vkCmdPipelineBarrier2, b, d); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer b, VkCullModeFlags m) { UC_VK_CALL(vkCmdSetCullMode, b, m); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer b, VkCompareOp o) { UC_VK_CALL(vkCmdSetDepthCompareOp, b, o); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer b, VkBool32 e) { UC_VK_CALL(vkCmdSetDepthTestEnable, b, e); }
VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer b, VkBool32 e) { UC_VK_CALL(vkCmdSetDepthWriteEnable, b, e); }
VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer b, VkPipelineStageFlags2 s, VkQueryPool p, uint32_t q) { UC_VK_CALL(vkCmdWriteTimestamp2, b, s, p, q); }
VKAPI_ATTR VkDeviceAddress VKAPI_CALL vkGetBufferDeviceAddress(VkDevice d, const VkBufferDeviceAddressInfo* i) { UC_VK_CALL(vkGetBufferDeviceAddress, d, i); }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) { UC_VK_CALL(vkQueueSubmit2, q, n, s, f); }
}
