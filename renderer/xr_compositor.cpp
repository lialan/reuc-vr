// HOTD4 VR's OpenXR host for this renderer: the session, swapchain, frame loop, foveation and refresh code of lindberghrecomp's
// renderer/vulkan/openxr.cpp (ef5cd20), with lindbergh's Device/Target replaced by the renderer's handles and the screen pass below.
#include "xr_compositor.h"
#include "../runtime/host_thread.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#ifdef __ANDROID__
#include <unistd.h>
#endif

namespace uc {
namespace {
template<class T> T function(XrInstance instance, const char* name) {
    PFN_xrVoidFunction result = nullptr;
    xr_check(xrGetInstanceProcAddr(instance, name, &result), name);
    return reinterpret_cast<T>(result);
}
void vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("XR: ") + what + " failed: VkResult " + std::to_string(result));
}
// The screen pass's push constants: per eye the four columns of the Vulkan clip transform of the quad -1..1.
// hud: the HUD layer's homography rows per eye (xr_screen.slang), hud[0][2][3] = 1 when there is one; the glow layer's affine map in
// hud[0][0][3], hud[0][1][3], hud[1][0][3], hud[1][1][3], hud[1][2][3] = 1 when there is one.
struct EyeConstants { float columns[2][16]; float rect[4]; float hud[2][3][4]; };   // rect: the picture's part the eyes show (u, v, width, height in texture coordinates)
static_assert(sizeof(EyeConstants) == 240);
// UC_XR_TIMING=1: where the render thread waits on the runtime, every 300 frames (LINDBERGH_XR_TIMING in HOTD4).
void xr_time(int what, std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    static const bool on = [] { const auto* v = std::getenv("UC_XR_TIMING"); return v && *v == '1'; }();
    if (!on) return;
    static double sum[4]; static unsigned n;
    sum[what] += std::chrono::duration<double, std::milli>(b - a).count();
    if (what == 3 && ++n == 300) {
        std::fprintf(stderr, "[xr-timing] ms per frame: xrWaitFrame %.2f, xrBeginFrame %.2f, xrWaitSwapchainImage %.2f, xrEndFrame %.2f\n",
                     sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
        sum[0] = sum[1] = sum[2] = sum[3] = 0; n = 0;
    }
}
}  // namespace

void xr_check(XrResult result, const char* what) {
    if (XR_FAILED(result)) throw std::runtime_error(std::string(what) + " failed (OpenXR " + std::to_string(result) + ")");
}

namespace {
xr::ControllerSettings controller_settings(const LgXrSettings& s) {
    // HOTD4's controller_settings(): its profile's gun (TP9), aim on the 15 m plane, the panel's choices.
    xr::ControllerSettings result{XrCompositor::kAimWidth, XrCompositor::kAimHeight, bool(s.dual), bool(s.auto_coins), bool(s.left_handed), false, s.aim};
    result.show_gun = s.show_gun; result.laser = s.laser; result.sight = s.sight; result.gun_model = XO_GUN_TP9;
    return result;
}
}  // namespace

XrCompositor::XrCompositor(XrConfig config, XrPlatform platform)
    : config_(config), scene_(config.stereo), controls_(controller_settings(config.settings)),
      panel_(config.settings.width, config.settings.resolution, "Umbrella Chronicles VR") {   // the running eye size ("(on restart)" in the panel)
    try {
#ifdef __ANDROID__
        if (!platform.java_vm || !platform.activity) throw std::invalid_argument("Android OpenXR requires JavaVM and Activity");
        const auto initialize = function<PFN_xrInitializeLoaderKHR>(XR_NULL_HANDLE, "xrInitializeLoaderKHR");
        XrLoaderInitInfoAndroidKHR loader{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        loader.applicationVM = platform.java_vm;
        loader.applicationContext = platform.activity;
        xr_check(initialize(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader)), "initialize Android OpenXR loader");
#else
        (void)platform;
#endif
        uint32_t n = 0;
        xr_check(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr), "enumerate runtime extensions (install/select an OpenXR runtime)");
        std::vector<XrExtensionProperties> available(n, {XR_TYPE_EXTENSION_PROPERTIES});
        xr_check(xrEnumerateInstanceExtensionProperties(nullptr, n, &n, available.data()), "runtime extensions");
        auto has = [&](const char* name) { return std::any_of(available.begin(), available.end(), [&](const auto& v) { return std::strcmp(v.extensionName, name) == 0; }); };
        if (!has(XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME)) throw std::runtime_error("OpenXR runtime does not provide XR_KHR_vulkan_enable2");
        std::vector<const char*> extensions{XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
#ifdef __ANDROID__
        if (!has(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) throw std::runtime_error("OpenXR runtime does not provide XR_KHR_android_create_instance");
        extensions.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
        if (config_.require_foveation) {
            for (const auto name : {XR_FB_FOVEATION_EXTENSION_NAME, XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME, XR_FB_FOVEATION_VULKAN_EXTENSION_NAME,
                                    XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME}) {
                if (!has(name)) throw std::runtime_error(std::string("Required OpenXR foveation extension unavailable: ") + name);
                extensions.push_back(name);
            }
        }
        performance_control_ = has(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
        if (performance_control_) extensions.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
#ifdef __ANDROID__
        thread_control_ = has(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
        if (thread_control_) extensions.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
#endif
        refresh_control_ = has(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        if (refresh_control_) extensions.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
        XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
#ifdef __ANDROID__
        XrInstanceCreateInfoAndroidKHR android{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
        android.applicationVM = platform.java_vm;
        android.applicationActivity = platform.activity;
        create.next = &android;
#endif
        std::strcpy(create.applicationInfo.applicationName, "Umbrella Chronicles VR");
        // HOTD4's tested API level: on the Quest runtime, requesting 1.0.0 left chained density-map outputs empty.
        create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
        create.enabledExtensionCount = uint32_t(extensions.size()); create.enabledExtensionNames = extensions.data();
        xr_check(xrCreateInstance(&create, &instance_), "create OpenXR instance");
        XrSystemGetInfo system{XR_TYPE_SYSTEM_GET_INFO}; system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
        xr_check(xrGetSystem(instance_, &system, &system_), "OpenXR HMD system");
        create_instance_ = function<PFN_xrCreateVulkanInstanceKHR>(instance_, "xrCreateVulkanInstanceKHR");
        create_device_ = function<PFN_xrCreateVulkanDeviceKHR>(instance_, "xrCreateVulkanDeviceKHR");
        get_device_ = function<PFN_xrGetVulkanGraphicsDevice2KHR>(instance_, "xrGetVulkanGraphicsDevice2KHR");
        auto requirements_fn = function<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance_, "xrGetVulkanGraphicsRequirements2KHR");
        XrGraphicsRequirementsVulkanKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
        xr_check(requirements_fn(instance_, system_, &requirements), "OpenXR Vulkan requirements");
        const auto version = XR_MAKE_VERSION(1, 3, 0);   // this renderer needs Vulkan 1.3 (dynamic rendering, synchronization2)
        if (version < requirements.minApiVersionSupported)
            throw std::runtime_error("OpenXR runtime does not support the renderer's Vulkan 1.3 application version");
        XrInstanceProperties properties{XR_TYPE_INSTANCE_PROPERTIES};
        xr_check(xrGetInstanceProperties(instance_, &properties), "OpenXR runtime properties");
        std::fprintf(stderr, "[vulkan-xr] runtime %s; Vulkan enable2; required foveation %s\n", properties.runtimeName, config_.require_foveation ? "yes" : "no");
        // The game's own picture is 16:9 (its widescreen mode) until a frame shows its lens: 60 degrees vertically.
        scene_.set_screen_lens({1.7320508f / (16.f / 9), 1.7320508f});
    } catch (...) { release(); throw; }
}

VkResult XrCompositor::create_instance(const VkInstanceCreateInfo& info, VkInstance& instance) {
    XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    create.systemId = system_; create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr; create.vulkanCreateInfo = &info;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    xr_check(create_instance_(instance_, &create, &instance, &result), "OpenXR Vulkan instance factory");
    return result;
}
VkPhysicalDevice XrCompositor::physical_device(VkInstance instance) {
    XrVulkanGraphicsDeviceGetInfoKHR info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    info.systemId = system_; info.vulkanInstance = instance;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    xr_check(get_device_(instance_, &info, &physical), "OpenXR selected Vulkan GPU");
    return physical;
}
VkResult XrCompositor::create_device(VkPhysicalDevice physical, const VkDeviceCreateInfo& info, VkDevice& device) {
    XrVulkanDeviceCreateInfoKHR create{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    create.systemId = system_; create.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    create.vulkanPhysicalDevice = physical; create.vulkanCreateInfo = &info;
    VkResult result = VK_ERROR_INITIALIZATION_FAILED;
    xr_check(create_device_(instance_, &create, &device, &result), "OpenXR Vulkan device factory");
    return result;
}

void XrCompositor::attach(VkInstance instance, VkPhysicalDevice physical, VkDevice device, uint32_t family, const std::filesystem::path& shaders) {
    if (session_) throw std::logic_error("OpenXR session already attached");
    device_ = device;
    try {
        XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
        binding.instance = instance; binding.physicalDevice = physical; binding.device = device;
        binding.queueFamilyIndex = family; binding.queueIndex = 0;
        XrSessionCreateInfo create{XR_TYPE_SESSION_CREATE_INFO}; create.next = &binding; create.systemId = system_;
        xr_check(xrCreateSession(instance_, &create, &session_), "Vulkan OpenXR session");
        if (performance_control_ && config_.sustained_cpu) {
            auto set = function<PFN_xrPerfSettingsSetPerformanceLevelEXT>(instance_, "xrPerfSettingsSetPerformanceLevelEXT");
            const auto level = config_.cpu_boost ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT;
            const auto result = set(session_, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, level);
            std::fprintf(stderr, "[vulkan-xr] CPU %s performance request: %d\n", config_.cpu_boost ? "boost" : "sustained high", int(result));
        }
        if (performance_control_) {
            // The GPU otherwise stays at the runtime's default clock level; a full-resolution stereo frame needs the sustained-high level.
            // UC_GPU_LEVEL=boost asks for the boost level instead (the fight replays ran at 545 MHz under sustained high); 0 = no request.
            const char* v = std::getenv("UC_GPU_LEVEL");
            if (!v || *v != '0') {
                const bool boost = v && std::string(v) == "boost";
                auto set = function<PFN_xrPerfSettingsSetPerformanceLevelEXT>(instance_, "xrPerfSettingsSetPerformanceLevelEXT");
                const auto result = set(session_, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, boost ? XR_PERF_SETTINGS_LEVEL_BOOST_EXT : XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
                std::fprintf(stderr, "[vulkan-xr] GPU %s performance request: %d\n", boost ? "boost" : "sustained high", int(result));
            }
        }
#ifdef __ANDROID__
        if (thread_control_) {
            set_thread_ = function<PFN_xrSetAndroidApplicationThreadKHR>(instance_, "xrSetAndroidApplicationThreadKHR");
            const auto result = set_thread_(session_, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, uint32_t(gettid()));
            std::fprintf(stderr, "[vulkan-xr] render thread %d registered: %d\n", gettid(), int(result));
        }
#endif
        if (refresh_control_) {
            request_refresh_ = function<PFN_xrRequestDisplayRefreshRateFB>(instance_, "xrRequestDisplayRefreshRateFB");
            get_refresh_ = function<PFN_xrGetDisplayRefreshRateFB>(instance_, "xrGetDisplayRefreshRateFB");
            requested_refresh_ = config_.refresh;
            const auto enumerate = function<PFN_xrEnumerateDisplayRefreshRatesFB>(instance_, "xrEnumerateDisplayRefreshRatesFB");
            uint32_t count = 0; xr_check(enumerate(session_, 0, &count, nullptr), "enumerate refresh rate count");
            refresh_rates_.resize(count); if (count) xr_check(enumerate(session_, count, &count, refresh_rates_.data()), "enumerate refresh rates");
            std::sort(refresh_rates_.begin(), refresh_rates_.end());
        }
        XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL; space.poseInReferenceSpace.orientation.w = 1;
        xr_check(xrCreateReferenceSpace(session_, &space, &space_), "OpenXR local space");
        if (config_.anchor_head) xr_check(xrCreateReferenceSpace(session_, &space, &base_space_), "OpenXR base local space");
        anchor_pending_ = config_.anchor_head;
        if (config_.enable_input) xr_check(lg_xr_actions_create(&actions_, instance_, session_), "shared controller bindings");
        uint32_t count = 0;
        xr_check(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, nullptr), "OpenXR view count");
        if (count != 2) throw std::runtime_error("OpenXR backend requires two stereo views");
        std::array<XrViewConfigurationView, 2> views{{{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}}};
        xr_check(xrEnumerateViewConfigurationViews(instance_, system_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, views.data()), "OpenXR eye sizes");
        width_ = std::max(views[0].recommendedImageRectWidth, views[1].recommendedImageRectWidth);
        height_ = std::max(views[0].recommendedImageRectHeight, views[1].recommendedImageRectHeight);
        if (config_.height) {
            width_ = config_.width ? config_.width : uint32_t(uint64_t(width_) * config_.height / height_);
            height_ = config_.height;
        } else if (config_.width) width_ = config_.width;
        for (const auto& view : views)
            if (!width_ || !height_ || width_ > view.maxImageRectWidth || height_ > view.maxImageRectHeight)
                throw std::runtime_error("Configured OpenXR resolution exceeds runtime eye limits");
        xr_check(xrEnumerateSwapchainFormats(session_, 0, &count, nullptr), "OpenXR formats");
        std::vector<int64_t> formats(count);
        xr_check(xrEnumerateSwapchainFormats(session_, count, &count, formats.data()), "OpenXR formats");
        VkFormat format = VK_FORMAT_UNDEFINED;
        for (auto preferred : {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
            if (std::find(formats.begin(), formats.end(), int64_t(preferred)) != formats.end()) { format = preferred; break; }
        if (format == VK_FORMAT_UNDEFINED) throw std::runtime_error("No supported OpenXR Vulkan color format");
        XrSwapchainCreateInfoFoveationFB fov{XR_TYPE_SWAPCHAIN_CREATE_INFO_FOVEATION_FB};
        fov.flags = XR_SWAPCHAIN_CREATE_FOVEATION_FRAGMENT_DENSITY_MAP_BIT_FB;
        XrSwapchainCreateInfo swap{XR_TYPE_SWAPCHAIN_CREATE_INFO}; swap.next = config_.require_foveation ? &fov : nullptr;
        // The game writes sRGB-encoded colour (the Wii's video output is display-referred), as HOTD4's guest does: store those bytes
        // unchanged through a UNORM view of the sRGB swapchain, which the compositor then decodes once.
        render_format_ = format == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : format == VK_FORMAT_B8G8R8A8_SRGB ? VK_FORMAT_B8G8R8A8_UNORM : format;
        swap.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | (render_format_ != format ? XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT : 0);
        if (std::getenv("UC_CAPTURE_XR")) swap.usageFlags |= XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
        swap.format = format; swap.sampleCount = 1; swap.width = width_; swap.height = height_; swap.faceCount = 1; swap.arraySize = 2; swap.mipCount = 1;
        xr_check(xrCreateSwapchain(session_, &swap, &swapchain_), "OpenXR stereo array swapchain");
        if (config_.require_foveation) {
            create_profile_ = function<PFN_xrCreateFoveationProfileFB>(instance_, "xrCreateFoveationProfileFB");
            destroy_profile_ = function<PFN_xrDestroyFoveationProfileFB>(instance_, "xrDestroyFoveationProfileFB");
            update_swapchain_ = function<PFN_xrUpdateSwapchainFB>(instance_, "xrUpdateSwapchainFB");
            set_foveation(config_.foveation_level);
        }
        xr_check(xrEnumerateSwapchainImages(swapchain_, 0, &count, nullptr), "OpenXR swapchain image count");
        std::vector<XrSwapchainImageVulkanKHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
        xr_check(xrEnumerateSwapchainImages(swapchain_, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())), "OpenXR Vulkan images");
        for (const auto& image : images) {
            images_.push_back(image.image);
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = image.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY; view.format = render_format_; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
            VkImageView created = VK_NULL_HANDLE;
            vk(vkCreateImageView(device_, &view, nullptr, &created), "eye array view");
            views_array_.push_back(created);
        }
        create_screen_pass(shaders);
        overlay_.set_fovea_support(fovea_support_);
        overlay_.initialize(physical, device_, render_format_, width_, height_, shaders);
        panel_.set_eye_size(int(width_), int(height_));
        overlay_depth_image_ = overlay_.depth_image();
        std::fprintf(stderr, "[vulkan-xr] stereo swapchain %ux%u x2, %u images, format %d rendered as %d\n", width_, height_, count, int(format), int(render_format_));
    } catch (...) { detach(); throw; }
}

void XrCompositor::create_screen_pass(const std::filesystem::path& shaders) {
    const auto load = [&](const char* name, VkShaderModule& module) {
        std::ifstream file(shaders / name, std::ios::binary);
        std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
        if (code.empty() || code.size() % 4) throw std::runtime_error(std::string("XR: shader ") + name + " missing or invalid");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = code.size(); info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        vk(vkCreateShaderModule(device_, &info, nullptr, &module), "shader module");
    };
    load("xr_screen.vert.spv", vertex_); load("xr_screen.frag.spv", fragment_);
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR; sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "sampler");
    const VkDescriptorSetLayoutBinding bindings[] = {{0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
                                                     {1, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_},
                                                     {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},    // the HUD layer's L
                                                     {3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},    // and T
                                                     {4, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},    // the glow layer's L
                                                     {5, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};   // and T
    VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount = 6; set.pBindings = bindings;
    vk(vkCreateDescriptorSetLayout(device_, &set, nullptr, &set_layout_), "descriptor layout");
    const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(EyeConstants)};
    VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pSetLayouts = &set_layout_; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
    vk(vkCreatePipelineLayout(device_, &layout, nullptr, &layout_), "pipeline layout");
    // xr_screen.slang constant 0: edge smoothing as the picture is magnified onto the eye (UC_XR_FXAA=0 turns it off, for comparison)
    const uint32_t fxaa = [] { const char* v = std::getenv("UC_XR_FXAA"); return v && *v == '0' ? 0u : 1u; }();
    const VkSpecializationMapEntry fxaa_entry{0, 0, sizeof(uint32_t)};
    const VkSpecializationInfo fxaa_info{1, &fxaa_entry, sizeof fxaa, &fxaa};
    std::fprintf(stderr, "[vulkan-xr] screen pass edge smoothing %s\n", fxaa ? "on" : "off");
    const VkPipelineShaderStageCreateInfo stages[] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertex_, "vertexMain", nullptr},
                                                      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragment_, "fragmentMain", &fxaa_info}};
    VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    // The eye pass has a depth attachment for the controller models. Its screen
    // quad must explicitly disable depth/stencil (VUID-renderPass-09033).
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
    const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.viewMask = 0b11; rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &render_format_;
    rendering.depthAttachmentFormat = XrOverlay::kDepthFormat;   // the eye pass also draws the overlay (depth-tested guns)
    VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.pNext = &rendering;
    pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &vertices; pipeline.pInputAssemblyState = &assembly; pipeline.pViewportState = &viewport;
    pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth; pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic; pipeline.layout = layout_;
    // The renderer's foveation map, when bound, sets the screen pass's rate (REPLACE); the overlay's pipelines ignore it (no rate state: KEEP).
    VkPipelineFragmentShadingRateStateCreateInfoKHR rate{VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR};
    rate.fragmentSize = {1, 1}; rate.combinerOps[0] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR; rate.combinerOps[1] = VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR;
    if (fovea_support_) { rate.pNext = rendering.pNext; rendering.pNext = &rate; pipeline.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR; }
    vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipeline_), "screen pipeline");
    const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 160}, {VK_DESCRIPTOR_TYPE_SAMPLER, 32}};
    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 32; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
    vk(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "descriptor pool");
}

void XrCompositor::poll() {
    XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
    XrResult result;
    while ((result = xrPollEvent(instance_, &event)) == XR_SUCCESS) {
        if (event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& change = *reinterpret_cast<XrEventDataSessionStateChanged*>(&event);
            if (change.session == session_) {
                session_state_ = change.state;
                if (config_.anchor_head && change.state == XR_SESSION_STATE_FOCUSED) anchor_pending_ = true;
                std::fprintf(stderr, "[vulkan-xr] session state %d\n", int(change.state));
                if (change.state == XR_SESSION_STATE_READY && !running_) {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO}; begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    xr_check(xrBeginSession(session_, &begin), "begin OpenXR session"); running_ = true;
                    if (request_refresh_ && requested_refresh_ > 0) {
                        const auto requested = request_refresh_(session_, requested_refresh_);
                        float observed = 0;
                        const auto got = get_refresh_(session_, &observed);
                        std::fprintf(stderr, "[vulkan-xr] requested %.2f Hz: %d; reported %.2f Hz: %d\n", requested_refresh_, int(requested), observed, int(got));
                    }
                } else if (change.state == XR_SESSION_STATE_STOPPING) {
                    xr_check(xrEndSession(session_), "end OpenXR session"); running_ = false;
                } else if (change.state == XR_SESSION_STATE_EXITING || change.state == XR_SESSION_STATE_LOSS_PENDING) exiting_ = true;
            }
        } else if (event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            if (config_.anchor_head) anchor_pending_ = true;
        } else if (event.type == XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) {
            const auto& rate = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
            std::fprintf(stderr, "[vulkan-xr] runtime refresh changed %.2f -> %.2f Hz\n", rate.fromDisplayRefreshRate, rate.toDisplayRefreshRate);
        } else if (event.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) exiting_ = true;
        event = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (result != XR_EVENT_UNAVAILABLE) xr_check(result, "poll OpenXR events");
}

bool XrCompositor::begin_frame() {
    if (!session_ || frame_open_) throw std::logic_error("Invalid OpenXR frame begin");
    poll(); if (!running_ || exiting_) return false;
#ifdef __ANDROID__
    static const bool guest_threads = std::getenv("UC_NO_GUEST_XR_THREAD") == nullptr;   // UC_NO_GUEST_XR_THREAD=1: as before (A/B)
    if (set_thread_ && guest_threads) {   // the guest's host threads (the CPU-bound game) are the application's main threads: performance-critical scheduling
        std::vector<int> tids;
        guest_tids_seen_ = uc::host_thread_guest_tids(guest_tids_seen_, tids);
        for (const int tid : tids) {
            const auto result = set_thread_(session_, XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, uint32_t(tid));
            std::fprintf(stderr, "[vulkan-xr] guest thread %d registered: %d\n", tid, int(result));
        }
    }
#endif
    const auto t0 = std::chrono::steady_clock::now();
    XrFrameWaitInfo wait{XR_TYPE_FRAME_WAIT_INFO}; xr_check(xrWaitFrame(session_, &wait, &frame_), "wait OpenXR frame");
    const auto t1 = std::chrono::steady_clock::now();
    XrFrameBeginInfo begin{XR_TYPE_FRAME_BEGIN_INFO}; xr_check(xrBeginFrame(session_, &begin), "begin OpenXR frame"); frame_open_ = true;
    xr_time(0, t0, t1); xr_time(1, t1, std::chrono::steady_clock::now());
    if (!frame_.shouldRender) return false;
    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO}; locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frame_.predictedDisplayTime; locate.space = anchor_pending_ ? base_space_ : space_;
    XrViewState state{XR_TYPE_VIEW_STATE}; uint32_t count = 0;
    xr_check(xrLocateViews(session_, &locate, &state, 2, &count, views_.data()), "locate OpenXR views");
    if (count != 2 || !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
    if (anchor_pending_) {
        if (!(state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) return false;
        const auto& q = views_[0].pose.orientation;
        const float quaternion[] = {q.x, q.y, q.z, q.w}, back[] = {0, 0, -1}; float forward[3];
        lg_stereo_rotate(quaternion, back, forward);
        const float yaw = std::atan2(-forward[0], -forward[2]);
        XrReferenceSpaceCreateInfo anchor{XR_TYPE_REFERENCE_SPACE_CREATE_INFO}; anchor.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        anchor.poseInReferenceSpace.orientation = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};
        anchor.poseInReferenceSpace.position = {(views_[0].pose.position.x + views_[1].pose.position.x) / 2,
                                                (views_[0].pose.position.y + views_[1].pose.position.y) / 2,
                                                (views_[0].pose.position.z + views_[1].pose.position.z) / 2};
        XrSpace replacement;
        xr_check(xrCreateReferenceSpace(session_, &anchor, &replacement), "anchor OpenXR camera on head");
        xrDestroySpace(space_); space_ = replacement; anchor_pending_ = false;
        locate.space = space_;
        xr_check(xrLocateViews(session_, &locate, &state, 2, &count, views_.data()), "locate anchored OpenXR views");
        if (count != 2 || !(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) return false;
        std::fprintf(stderr, "[vulkan-xr] camera anchored on head at %.2f %.2f %.2f m; yaw %.1f degrees\n", anchor.poseInReferenceSpace.position.x,
                     anchor.poseInReferenceSpace.position.y, anchor.poseInReferenceSpace.position.z, yaw * 57.2957795f);
    }
    std::array<xr::StereoPose, 2> eyes;
    // UC_VR_WOBBLE=1 (diagnostics): a slow artificial head sway on top of the tracked pose (yaw +-6 degrees and +-4 cm sideways, about a
    // 3 s period), so runs with the headset lying still move as a worn one does.
    static const bool wobble = [] { const char* v = std::getenv("UC_VR_WOBBLE"); return v && *v == '1'; }();
    // Deterministic EFB comparisons on the headset: keep 64 mm stereo separation,
    // but remove tracking drift and the headset's resting orientation. Diagnostic
    // captures only; the compositor still receives the real tracked poses.
    static const bool fixed_pose = [] { const char* v = std::getenv("UC_VR_TEST_POSE"); return v && *v == '1'; }();
    static const float test_pitch = [] { const char* v = std::getenv("UC_VR_TEST_PITCH"); return v ? float(std::atof(v)) * .017453292f : 0.f; }();
    static const float test_yaw = [] { const char* v = std::getenv("UC_VR_TEST_YAW"); return v ? float(std::atof(v)) * .017453292f : 0.f; }();
    const double phase = wobble ? double(frame_.predictedDisplayTime % 3000000000ll) / 3e9 * 6.283185307 : 0.0;
    const float yaw = wobble ? float(0.105 * std::sin(phase)) : 0.f, sway = wobble ? float(0.04 * std::sin(phase * 1.7)) : 0.f;
    const float qy[4] = {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)};   // x y z w
    for (unsigned i = 0; i < 2; ++i) {
        const auto& v = views_[i];
        eyes[i].orientation = {v.pose.orientation.x, v.pose.orientation.y, v.pose.orientation.z, v.pose.orientation.w};
        eyes[i].position = {v.pose.position.x, v.pose.position.y, v.pose.position.z};
        if (fixed_pose) {
            const float sx = std::sin(test_pitch / 2), cx = std::cos(test_pitch / 2);
            const float sy = std::sin(test_yaw / 2), cy = std::cos(test_yaw / 2);
            eyes[i].orientation = {cy * sx, sy * cx, -sy * sx, cy * cx};
            const float eye_x = i ? .032f : -.032f;
            eyes[i].position = {std::cos(test_yaw) * eye_x, 0, -std::sin(test_yaw) * eye_x};
        }
        if (wobble) {   // yaw about the head's vertical axis (q = qy * q, the position rotated with it), then the sideways sway
            const float x = eyes[i].orientation[0], y = eyes[i].orientation[1], z = eyes[i].orientation[2], w = eyes[i].orientation[3];
            eyes[i].orientation = {qy[3] * x + qy[1] * z, qy[3] * y + qy[1] * w, qy[3] * z - qy[1] * x, qy[3] * w - qy[1] * y};
            const float px = eyes[i].position[0], pz = eyes[i].position[2], c = std::cos(yaw), s = std::sin(yaw);
            eyes[i].position[0] = c * px + s * pz + sway; eyes[i].position[2] = -s * px + c * pz;
        }
        eyes[i].tangents = {std::tan(-v.fov.angleLeft), std::tan(v.fov.angleRight), std::tan(-v.fov.angleDown), std::tan(v.fov.angleUp)};
    }
    // Reproduce an observed faulty view without tracking drift. The file holds
    // orientation xyzw, position xyz and four tangents for each eye (22 floats).
    // Like TEST_POSE, this changes rendering only; XR submission stays tracked.
    static const auto recorded_pose = []() -> std::optional<std::array<xr::StereoPose, 2>> {
        const char* path = std::getenv("UC_VR_TEST_POSE_FILE");
        if (!path || !*path) return std::nullopt;
        std::ifstream file(path);
        std::array<xr::StereoPose, 2> result;
        const auto read = [&](auto& values) {
            for (float& value : values)
                if (!(file >> value) || !std::isfinite(value)) throw std::runtime_error("Invalid UC_VR_TEST_POSE_FILE");
        };
        for (auto& eye : result) {
            read(eye.orientation); read(eye.position); read(eye.tangents);
            float norm = 0;
            for (float q : eye.orientation) norm += q * q;
            if (std::fabs(norm - 1.f) > .001f || std::any_of(eye.tangents.begin(), eye.tangents.end(), [](float t) { return t <= 0; }))
                throw std::runtime_error("Invalid eye in UC_VR_TEST_POSE_FILE");
        }
        std::string extra;
        if (file >> extra) throw std::runtime_error("Extra data in UC_VR_TEST_POSE_FILE");
        return result;
    }();
    if (recorded_pose) eyes = *recorded_pose;
    static const unsigned pose_dump = [] { const char* v = std::getenv("UC_VR_POSE_DUMP"); return v ? unsigned(std::strtoul(v, nullptr, 10)) : 0u; }();
    static uint64_t pose_number = 0;
    if (pose_dump && ++pose_number % pose_dump == 0) {
        std::fprintf(stderr, "[vr-pose] %llu %lld", (unsigned long long)pose_number, (long long)frame_.predictedDisplayTime);
        for (const auto& eye : eyes) {
            for (float value : eye.orientation) std::fprintf(stderr, " %.9g", value);
            for (float value : eye.position) std::fprintf(stderr, " %.9g", value);
            for (float value : eye.tangents) std::fprintf(stderr, " %.9g", value);
        }
        std::fputc('\n', stderr);
    }
    scene_.publish(eyes);
    controller_frame(true);
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xr_check(xrAcquireSwapchainImage(swapchain_, &acquire, &image_), "acquire OpenXR eye array"); acquired_ = true;
    XrSwapchainImageWaitInfo ready{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; ready.timeout = XR_INFINITE_DURATION;
    const auto t2 = std::chrono::steady_clock::now();
    xr_check(xrWaitSwapchainImage(swapchain_, &ready), "wait OpenXR eye array");
    xr_time(2, t2, std::chrono::steady_clock::now());
    return true;
}

// HOTD4's per-frame controller step (lindberghrecomp vulkan_guest.cpp State::frame and panel_input): sample the actions, run the settings
// panel (which runs ControllerInput), apply what the panel changed, send the haptics.
void XrCompositor::controller_frame(bool tracked) {
    LgXrActionSample input{}; xr::ControllerSample sample;
    const bool focused = tracked && this->input(input);
    for (unsigned h = 0; h < 2; ++h) {
        sample.valid[h] = input.valid[h];
        sample.pose[h].position = {input.pose[h].position.x, input.pose[h].position.y, input.pose[h].position.z};
        sample.pose[h].orientation = {input.pose[h].orientation.x, input.pose[h].orientation.y, input.pose[h].orientation.z, input.pose[h].orientation.w};
        sample.buttons.trigger[h] = input.trigger[h] > .55f; sample.buttons.grip[h] = input.grip[h] > .6f;
        sample.buttons.lower[h] = input.lower[h]; sample.buttons.upper[h] = input.upper[h]; sample.buttons.stick[h] = input.stick[h];
    }
    sample.menu = input.menu;
    auto& settings = config_.settings;
    const unsigned effects = panel_.update(settings, scene_, controls_, focused ? sample : xr::ControllerSample{}, frame_.predictedDisplayTime, focused, refresh_rates_);
    if (effects & XP_SAVE) xr_settings_save(&settings);
    if (effects & XP_RATE_CHANGED) request_refresh(float(settings.refresh));
    if (effects & XP_FOVEATION_CHANGED) set_foveation(settings.foveation);   // the runtime's (a swapchain made with foveation); the renderer's map reads foveation_level()
    if (effects & XP_SCALE_CHANGED) render_scale_changed_ = true;
    if (effects & XP_RECENTER) recenter();
    if ((effects & XP_RESTART) && config_.restart) config_.restart();
    const auto& haptics = panel_.haptics();
    for (int h = 0; h < 2; ++h) if (haptics[h].milliseconds) rumble(h, haptics[h].amplitude, haptics[h].milliseconds);
}

void XrCompositor::observe(const gx::Projection& projection) {
    scene_.observe(gl_from_vulkan(projection), LG_STEREO_WORLD);
    lens_seen_ = true;
}

void XrCompositor::record(VkCommandBuffer command, VkImage picture, VkImageView picture_view, VkImageLayout layout, const std::array<float,4>& rect, VkImageView fovea, VkExtent2D texel,
                          const XrHudLayer* hud) {
    if (!acquired_) throw std::logic_error("XR: record without an acquired eye array");
    VkDescriptorSet set = VK_NULL_HANDLE;
    // Without a HUD layer, bindings 2 and 3 hold the picture (any image of the declared kind: the shader does not read them then).
    const VkImageView light = hud && hud->light ? hud->light : picture_view, transmit = hud && hud->transmit ? hud->transmit : picture_view;
    const VkImageView glow_light = hud && hud->glow_light ? hud->glow_light : picture_view, glow_transmit = hud && hud->glow_transmit ? hud->glow_transmit : picture_view;
    for (const auto& bound : bound_)
        if (bound.view == picture_view && bound.light == light && bound.transmit == transmit && bound.glow_light == glow_light && bound.glow_transmit == glow_transmit) set = bound.set;
    if (!set) {
        if (bound_.size() >= 32) { vk(vkResetDescriptorPool(device_, pool_, 0), "descriptor pool reset"); bound_.clear(); }
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool_; allocate.descriptorSetCount = 1; allocate.pSetLayouts = &set_layout_;
        vk(vkAllocateDescriptorSets(device_, &allocate, &set), "descriptor set");
        const VkDescriptorImageInfo images[5]{{VK_NULL_HANDLE, picture_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, light, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              {VK_NULL_HANDLE, transmit, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}, {VK_NULL_HANDLE, glow_light, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                              {VK_NULL_HANDLE, glow_transmit, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        VkWriteDescriptorSet writes[5]{};
        for (uint32_t i = 0; i < 5; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet = set; writes[i].dstBinding = i == 0 ? 0 : i + 1; writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo = &images[i];
        }
        vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
        bound_.push_back({picture_view, light, transmit, glow_light, glow_transmit, set});
    }
    auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = src; b.srcAccessMask = src_access; b.dstStageMask = dst; b.dstAccessMask = dst_access;
        b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = picture; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};   // both eyes' layers: the right eye's was sampled in TRANSFER_SRC
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(command, &dependency);
    };
    barrier(layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    overlay_.upload(command, panel_.take_panel(), panel_.take_label());
    // The eye array arrives from xrWaitSwapchainImage in COLOR_ATTACHMENT_OPTIMAL and is released in it (XR_KHR_vulkan_enable2). Cleared,
    // not loaded: nothing of the previous frame stays (HOTD4: CLEAR instead of LOAD saved 1.8 ms on Quest).
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView = views_array_.at(image_);
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.clearValue.color = {{0, 0, 0, 1}};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth.imageView = overlay_.depth_view();
    depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.clearValue.depthStencil = {1, 0};
    {   // transient depth: its contents never matter between frames
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT; b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = b.srcStageMask; b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = overlay_depth_image_; b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(command, &dependency);
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea.extent = {width_, height_}; rendering.layerCount = 1; rendering.viewMask = 0b11;
    rendering.colorAttachmentCount = 1; rendering.pColorAttachments = &color; rendering.pDepthAttachment = &depth;
    VkRenderingFragmentShadingRateAttachmentInfoKHR rate{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    if (fovea && fovea_support_) {   // the renderer's map of the eye image: the screen pass coarser towards each eye's periphery
        rate.imageView = fovea; rate.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR; rate.shadingRateAttachmentTexelSize = texel;
        rendering.pNext = &rate;
    }
    vkCmdBeginRendering(command, &rendering);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &set, 0, nullptr);
    const VkViewport viewport{0, 0, float(width_), float(height_), 0, 1}; const VkRect2D scissor{{0, 0}, {width_, height_}};
    vkCmdSetViewport(command, 0, 1, &viewport); vkCmdSetScissor(command, 0, 1, &scissor);
    // Each eye's layer covers that eye: the renderer drew it in the eye's space already.
    EyeConstants constants{};
    for (int eye = 0; eye < 2; ++eye) { const auto unit = xr::identity(); std::memcpy(constants.columns[eye], unit.data(), sizeof constants.columns[eye]); }
    std::copy(rect.begin(), rect.end(), constants.rect);
    if (hud && hud->light) {
        for (int eye = 0; eye < 2; ++eye) for (int row = 0; row < 3; ++row) for (int c = 0; c < 3; ++c) constants.hud[eye][row][c] = hud->rows[eye][row][c];
        constants.hud[0][2][3] = 1;
    }
    if (hud && hud->glow_light) {
        constants.hud[0][0][3] = hud->glow[0]; constants.hud[0][1][3] = hud->glow[1]; constants.hud[1][0][3] = hud->glow[2]; constants.hud[1][1][3] = hud->glow[3];
        constants.hud[1][2][3] = 1;
    }
    vkCmdPushConstants(command, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
    vkCmdDraw(command, 6, 1, 0, 0);
    overlay_.draw(command, scene_.overlay_matrices(), scene_.overlay_eyes(), panel_.guns(), panel_.draws());
    vkCmdEndRendering(command);
    barrier(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, layout, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
}

void XrCompositor::copy_capture(VkCommandBuffer command, VkImage destination, VkExtent2D size) {
    if (!acquired_ || size.width!=width_ || size.height!=height_ || render_format_!=VK_FORMAT_R8G8B8A8_UNORM)
        throw std::logic_error("UC_CAPTURE_XR requires native-sized RGBA8 eyes");
    // Copy bytes: blitting from the runtime's sRGB image would decode colours that
    // were deliberately written through its UNORM view.
    const VkImage source=images_.at(image_);
    const auto transition=[&](VkImage image,VkImageLayout from,VkImageLayout to,VkPipelineStageFlags2 src,VkAccessFlags2 read,
                              VkPipelineStageFlags2 dst,VkAccessFlags2 write) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask=src; b.srcAccessMask=read; b.dstStageMask=dst; b.dstAccessMask=write;
        b.oldLayout=from; b.newLayout=to; b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.image=image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount=1; dependency.pImageMemoryBarriers=&b;
        vkCmdPipelineBarrier2(command,&dependency);
    };
    transition(source,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    transition(destination,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
               VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,2}; copy.extent={width_,height_,1};
    vkCmdCopyImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    transition(destination,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    transition(source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
               VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
}

void XrCompositor::end_frame(bool rendered) {
    if (!frame_open_) return;
    if (rendered && !acquired_) throw std::logic_error("OpenXR frame has no acquired eye image");
    if (acquired_) { XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO}; xr_check(xrReleaseSwapchainImage(swapchain_, &release), "release OpenXR eye array"); acquired_ = false; }
    std::array<XrCompositionLayerProjectionView, 2> views{{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}}};
    for (unsigned i = 0; i < 2; ++i) {
        views[i].pose = views_[i].pose; views[i].fov = views_[i].fov;
        views[i].subImage.swapchain = swapchain_; views[i].subImage.imageRect.extent = {int32_t(width_), int32_t(height_)}; views[i].subImage.imageArrayIndex = i;
    }
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION}; projection.space = space_; projection.viewCount = 2; projection.views = views.data();
    const auto layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO}; end.displayTime = frame_.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; end.layerCount = rendered ? 1 : 0; end.layers = &layer;
    const auto t3 = std::chrono::steady_clock::now();
    xr_check(xrEndFrame(session_, &end), "submit OpenXR frame"); frame_open_ = false;
    xr_time(3, t3, std::chrono::steady_clock::now());
    if (lens_seen_) { scene_.finish_frame(); lens_seen_ = false; }   // HOTD4: the frame's most-used lens sizes the screen
}

bool XrCompositor::input(LgXrActionSample& sample) {
    sample = {};
    if (!config_.enable_input || !frame_open_ || session_state_ != XR_SESSION_STATE_FOCUSED) return false;
    const auto result = lg_xr_actions_sample(&actions_, space_, frame_.predictedDisplayTime, &sample);
    if (result == XR_SESSION_NOT_FOCUSED) return false;
    xr_check(result, "shared controller input"); return result == XR_SUCCESS;
}
void XrCompositor::rumble(int hand, float amplitude, int milliseconds) {
    if (config_.enable_input && session_state_ == XR_SESSION_STATE_FOCUSED) lg_xr_actions_rumble(&actions_, hand, amplitude, milliseconds);
}
void XrCompositor::set_foveation(int level) {
    if (!create_profile_ || !update_swapchain_ || !swapchain_) return;
    static const XrFoveationLevelFB levels[] = {XR_FOVEATION_LEVEL_NONE_FB, XR_FOVEATION_LEVEL_LOW_FB, XR_FOVEATION_LEVEL_MEDIUM_FB, XR_FOVEATION_LEVEL_HIGH_FB};
    XrFoveationLevelProfileCreateInfoFB info{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
    info.level = levels[level < 0 || level > 3 ? 2 : level]; info.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
    XrFoveationProfileCreateInfoFB profile{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB}; profile.next = &info;
    XrFoveationProfileFB created = XR_NULL_HANDLE;
    xr_check(create_profile_(session_, &profile, &created), "OpenXR foveation profile");
    XrSwapchainStateFoveationFB state{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB}; state.profile = created;
    xr_check(update_swapchain_(swapchain_, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&state)), "OpenXR foveation swapchain state");
    if (profile_ && destroy_profile_) destroy_profile_(profile_);
    profile_ = created;
    std::fprintf(stderr, "[vulkan-xr] foveation level %d\n", level);
}
void XrCompositor::request_refresh(float hz) {
    requested_refresh_ = hz;
    if (request_refresh_ && running_) {
        const auto result = request_refresh_(session_, hz);
        std::fprintf(stderr, "[vulkan-xr] menu refresh request %.2f Hz: %d\n", hz, int(result));
    }
}
void XrCompositor::detach() {
    if (device_) vkDeviceWaitIdle(device_);   // session shutdown, not frame submission
    if (device_) {
        overlay_.shutdown();
        for (auto view : views_array_) vkDestroyImageView(device_, view, nullptr);
        if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (layout_) vkDestroyPipelineLayout(device_, layout_, nullptr);
        if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        if (vertex_) vkDestroyShaderModule(device_, vertex_, nullptr);
        if (fragment_) vkDestroyShaderModule(device_, fragment_, nullptr);
    }
    views_array_.clear(); images_.clear(); bound_.clear();
    pool_ = VK_NULL_HANDLE; pipeline_ = VK_NULL_HANDLE; layout_ = VK_NULL_HANDLE; set_layout_ = VK_NULL_HANDLE; sampler_ = VK_NULL_HANDLE; vertex_ = fragment_ = VK_NULL_HANDLE;
    if (profile_ && destroy_profile_) destroy_profile_(profile_); profile_ = XR_NULL_HANDLE;
    if (swapchain_) xrDestroySwapchain(swapchain_); swapchain_ = XR_NULL_HANDLE;
    if (space_) xrDestroySpace(space_); space_ = XR_NULL_HANDLE;
    if (base_space_) xrDestroySpace(base_space_); base_space_ = XR_NULL_HANDLE;
    lg_xr_actions_destroy(&actions_);
    if (session_) xrDestroySession(session_); session_ = XR_NULL_HANDLE;
#ifdef __ANDROID__
    set_thread_ = nullptr;
#endif
    refresh_rates_.clear(); requested_refresh_ = 0; session_state_ = XR_SESSION_STATE_UNKNOWN;
    device_ = VK_NULL_HANDLE; running_ = false; frame_open_ = false; acquired_ = false;
}
void XrCompositor::release() { detach(); if (instance_) xrDestroyInstance(instance_); instance_ = XR_NULL_HANDLE; }
XrCompositor::~XrCompositor() { release(); }
}  // namespace uc
