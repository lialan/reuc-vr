#pragma once
// The headset output (Quest 3 and other OpenXR HMDs): HOTD4 VR's OpenXR host (lindberghrecomp renderer/vulkan/openxr.cpp) for this
// renderer. The Vulkan instance and device come from the runtime (XR_KHR_vulkan_enable2); one stereo swapchain (two array layers) is drawn
// with multiview and shown as one projection layer. What the eyes see is decided by StereoScene (xr/stereo_scene.hpp), as in HOTD4: the
// game's flat picture on a screen plane `screen` metres ahead, sized by the game's lens.
//
// Per presented game frame (VulkanRenderer::render): begin_frame() (xrWaitFrame/xrBeginFrame/xrLocateViews, acquire), record() into that
// frame's command buffer, the queue submit, end_frame(). begin_frame() false: nothing is drawn, end_frame(false) still closes the frame.
#include "../xr/stereo_scene.hpp"
#include "../xr/controller_input.hpp"
#include "../xr/settings_panel.hpp"
#include "../xr/xr_actions.h"
#include "../xr/xr_settings.h"
#include "xr_overlay.h"
#include <functional>
#include "gx_transforms.h"
#include "gl_clip.h"
#include <vulkan/vulkan.h>
#ifdef __ANDROID__
#include <jni.h>
#define XR_USE_PLATFORM_ANDROID
#endif
#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <array>
#include <filesystem>
#include <vector>

namespace uc {
struct XrPlatform { void* java_vm = nullptr; void* activity = nullptr; };   // Android: the JavaVM and the Activity (global ref), kept alive by the caller
struct XrConfig {
    uint32_t width = 0, height = 0;   // per eye; 0: the runtime's recommendation (HOTD4: native 2064x2208 on Quest 3)
    float refresh = 120;              // requested display rate (HOTD4: 120 Hz, the game's 60 fps shown twice)
    int foveation_level = 3;          // 0 off .. 3 high (HOTD4 default: high)
    bool require_foveation = false;   // the swapchain's fragment density map (needs the device's VK_EXT_fragment_density_map)
    bool anchor_head = true;          // yaw-only recentring on the head at start, on focus and on the system recentre
    bool sustained_cpu = true, cpu_boost = false, enable_input = true;
    xr::StereoSettings stereo{};      // scale (game units per metre), screen distance, HUD pitch
    LgXrSettings settings = LG_XR_SETTINGS_DEFAULT;   // vr.cfg: the settings panel edits and saves it (HOTD4's panel)
    std::function<void()> restart;    // the panel's "Reset game" (the activity's restartGame)
};
void xr_check(XrResult result, const char* what);

class XrCompositor {
public:
    XrCompositor(XrConfig config, XrPlatform platform);
    ~XrCompositor();
    XrCompositor(const XrCompositor&) = delete;
    XrCompositor& operator=(const XrCompositor&) = delete;
    // Vulkan through the runtime: the renderer calls these instead of vkCreateInstance, physical device selection and vkCreateDevice.
    VkResult create_instance(const VkInstanceCreateInfo& info, VkInstance& instance);
    VkPhysicalDevice physical_device(VkInstance instance);
    VkResult create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device);
    // After the device exists: the session, the swapchain and the screen pass. shaders: the folder holding xr_screen.*.spv.
    void attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t family, const std::filesystem::path& shaders);
    void detach();
    bool begin_frame();
    // The finished game picture (layout `layout`, left in it again) on the screen plane in both eyes. Call only after begin_frame() true.
    // rect: the part of the picture the eyes show, in texture coordinates (the whole picture, or the EFB's displayed region read in place)
    // fovea: the renderer's foveation map of the eye image (null: none), texel its texel size; the screen pass shades by it, the overlay not.
    void record(VkCommandBuffer command, VkImage picture, VkImageView picture_view, VkImageLayout layout, const std::array<float,4>& rect = {0, 0, 1, 1},
                VkImageView fovea = VK_NULL_HANDLE, VkExtent2D texel = {});
    // Before attach(): the device can shade by a foveation map (the screen and overlay pipelines are made to accept one).
    void set_fovea_support(bool on) { fovea_support_ = on; }
    void end_frame(bool rendered);
    bool frame_open() const { return frame_open_; }
    bool exiting() const { return exiting_; }
    XrSessionState session_state() const { return session_state_; }
    // The game's projection of a perspective draw, every frame (HOTD4's lens tally: the screen plane matches the game's field of view).
    void observe(const gx::Projection& projection);
    // The game thread: the latest controller state (HOTD4's cabinet bits and aim rays), false when nothing new arrived.
    bool consume(xr::ControllerSnapshot& snapshot) { return controls_.consume(snapshot); }
    // The aim's coordinate range: ray x/y are pixels of a kAimWidth x kAimHeight picture of the screen plane.
    static constexpr int kAimWidth = 1280, kAimHeight = 720;
    xr::StereoScene& scene() { return scene_; }
    bool input(LgXrActionSample& sample);
    void rumble(int hand, float amplitude, int milliseconds);
    void recenter() { if (config_.anchor_head) anchor_pending_ = true; }
    void set_foveation(int level);
    int foveation_level() const { return config_.settings.foveation; }   // vr.cfg / the panel: the renderer's foveation map (0 off .. 3 high)
    float render_scale() const { return config_.settings.render_scale; }   // vr.cfg / the panel: the eyes' scene scale, 0 = native (the eye image's rows)
    // The panel changed the render scale since the last call: the renderer applies it at its next frame (no restart).
    bool take_render_scale_change() { const bool changed = render_scale_changed_; render_scale_changed_ = false; return changed; }
    void request_refresh(float hz);
    const std::vector<float>& refresh_rates() const { return refresh_rates_; }
    uint32_t eye_width() const { return width_; }
    uint32_t eye_height() const { return height_; }
    XrTime display_time() const { return frame_.predictedDisplayTime; }
    XrSpace space() const { return space_; }
private:
    void poll();
    void release();
    void create_screen_pass(const std::filesystem::path& shaders);
    XrConfig config_;
    LgXrActions actions_{};
    XrInstance instance_ = XR_NULL_HANDLE;
    XrSystemId system_ = XR_NULL_SYSTEM_ID;
    XrSession session_ = XR_NULL_HANDLE;
    XrSpace space_ = XR_NULL_HANDLE, base_space_ = XR_NULL_HANDLE;
    bool anchor_pending_ = true;
    XrSwapchain swapchain_ = XR_NULL_HANDLE;
    XrFoveationProfileFB profile_ = XR_NULL_HANDLE;
    bool running_ = false, exiting_ = false, frame_open_ = false, acquired_ = false, render_scale_changed_ = false, fovea_support_ = false;
    uint32_t width_ = 0, height_ = 0, image_ = 0;
    XrSessionState session_state_ = XR_SESSION_STATE_UNKNOWN;
    XrFrameState frame_{XR_TYPE_FRAME_STATE};
    std::array<XrView, 2> views_{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
    PFN_xrCreateVulkanInstanceKHR create_instance_ = nullptr;
    PFN_xrCreateVulkanDeviceKHR create_device_ = nullptr;
    PFN_xrGetVulkanGraphicsDevice2KHR get_device_ = nullptr;
    bool refresh_control_ = false, performance_control_ = false, thread_control_ = false;
#ifdef __ANDROID__
    PFN_xrSetAndroidApplicationThreadKHR set_thread_ = nullptr;
    size_t guest_tids_seen_ = 0;   // guest host threads already registered with the runtime (begin_frame)
#endif
    std::vector<float> refresh_rates_;
    float requested_refresh_ = 0;
    PFN_xrRequestDisplayRefreshRateFB request_refresh_ = nullptr;
    PFN_xrGetDisplayRefreshRateFB get_refresh_ = nullptr;
    PFN_xrDestroyFoveationProfileFB destroy_profile_ = nullptr;
    PFN_xrCreateFoveationProfileFB create_profile_ = nullptr;
    PFN_xrUpdateSwapchainFB update_swapchain_ = nullptr;
    // Vulkan side
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat render_format_ = VK_FORMAT_UNDEFINED;
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_array_;   // per swapchain image: both eyes, 2D array view in the render format
    VkShaderModule vertex_ = VK_NULL_HANDLE, fragment_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    struct Bound { VkImageView view; VkDescriptorSet set; };
    std::vector<Bound> bound_;
    xr::StereoScene scene_;
    bool lens_seen_ = false;
    xr::ControllerInput controls_;
    xr::SettingsPanel panel_;
    XrOverlay overlay_;
    void controller_frame(bool tracked);
    VkImage overlay_depth_image_ = VK_NULL_HANDLE;
};
}  // namespace uc
