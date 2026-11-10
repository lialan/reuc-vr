#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#include "vulkan_android_entry.h"
#ifdef UC_OPENXR
#include "xr_compositor.h"
#endif
#include "gl_clip.h"
#include "late_aim.h"
#include "texture_replacement.h"
#include "post_effects.h"
#include "texture_coordinates.h"
#include "vulkan_renderer.h"
#include "render_scale.h"
#include "rebuild_gate.h"
#include "buffer_memory.h"
#include "upscale_math.h"
#include "upscaler_probe.h"
#ifdef _WIN32
#include <vulkan/vulkan_win32.h>
#endif
#include <tuple>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include "vr_cull.h"
#include "depth_efb.h"
#include "fovea_map.h"
#include <chrono>
#include <cstring>
#include <cmath>
#include <fstream>
#include <limits>
#include <unordered_map>
#include <unordered_set>

namespace uc {
namespace {
// Per-draw uniform block: the GX draw state followed by the pipeline key words the ubershader reads (key[1..67], padded to 17 vec4).
using CopyDimensions=std::array<std::array<uint32_t,2>,8>;
constexpr size_t kTevWords=68, kCopyDimensionsOffset=sizeof(gx::DrawUniform)+kTevWords*sizeof(uint32_t);
constexpr size_t kDrawUniformBytes=kCopyDimensionsOffset+sizeof(CopyDimensions);
constexpr size_t kStereoBlockBytes=128;   // smoke.slang StereoBlock: two eyes x four rows
const std::shared_ptr<const gx::TexturePixels>& draw_texture(const gx::DrawPacket& draw,size_t map) { return map ? draw.texture_maps[map-1] : draw.texture; }
// The fragment shading rate of a draw whose textures are all magnified onto it, so that coarse shading only drops texels that are not
// there. Two kinds, outside the scene:
// - the game's bloom chain and lens glows: a 2D quad drawn the same in both eyes (stereo class pass or fill), its EFB copies of at most
//   320x180 (the half- and quarter-size blur chain) at no more than one texel per guest pixel, other textures (the lens masks) at two
//   guest pixels a texel or more;
// - translucent world sprites (smoke, heat haze, glows: blended, no depth write, no EFB copy) whose textures cover two guest pixels a
//   texel or more (by the draw's box in the game's camera). UC_NO_VRS_WORLD=1 leaves them at 1x1.
// Such a draw is shaded on a grid of 1.5 points per guest pixel or finer on each axis (on the Quest's 3.2 x 6.1 EFB scale: 2x4), not at
// the EFB's full scale; anything else at 1x1.
VkExtent2D bloom_shading_rate(const gx::DrawPacket& draw, VkExtent2D target, bool flat_eligible, bool world_eligible, bool depth_write) {
    const VkExtent2D full{1,1};
    static const bool no_world=std::getenv("UC_NO_VRS_WORLD")!=nullptr;
    const bool perspective=draw.transforms->projection[3][3]==0;
    if (draw.reticle || draw.late_aim || !(perspective ? !no_world && world_eligible && draw.blend.mode==1 && !depth_write : flat_eligible)) return full;
    const auto box=gx::draw_bounds_ndc(draw);
    if (!box) return full;
    const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
    const float width=(std::clamp((*box)[2],-1.f,1.f)-std::clamp((*box)[0],-1.f,1.f))*.5f*std::abs(v[2]), height=(std::clamp((*box)[3],-1.f,1.f)-std::clamp((*box)[1],-1.f,1.f))*.5f*std::abs(v[3]);
    bool any=false;
    for (size_t map=0;map<8;++map) {
        const auto& texture=draw_texture(draw,map);
        if (!texture || texture->levels.empty()) continue;
        const float w=float(texture->levels[0].width), h=float(texture->levels[0].height);
        if (texture->gpu_only ? (perspective || w>320 || w>width*1.05f || h>height*1.05f) : (w*2>width || h*2>height)) return full;
        any|=texture->gpu_only || perspective;
    }
    if (!any) return full;
    const auto axis=[](double scale) { return scale/4>=1.5 ? 4u : scale/2>=1.5 ? 2u : 1u; };
    return {axis(target.width/640.0),axis(target.height/480.0)};
}
// The Display page's "Post-effect shading: Coarse": a 2D draw outside the scene, drawn the same in both eyes, whose textures are all full-size
// EFB copies (wider than the bloom chain's 320: the composite, the colour pass over the scene copy) or that has none (fades), shades 2x2
// where an EFB pixel is at least 3 eye pixels' worth of guest detail on that axis (scale >= 3 a guest pixel, as at native); else 1x1. This
// trades the scene copy's finest detail for fragment work: these passes redraw the whole picture. Text and the HUD (ordinary textures),
// the reticle and late-aimed quads keep 1x1.
VkExtent2D post_shading_rate(const gx::DrawPacket& draw, VkExtent2D target) {
    const VkExtent2D full{1,1};
    if (draw.reticle || draw.late_aim || draw.layer || draw.transforms->projection[3][3]==0) return full;
    for (size_t map=0;map<8;++map) {
        const auto& texture=draw_texture(draw,map);
        if (!texture || texture->levels.empty()) continue;
        if (!texture->gpu_only || texture->levels[0].width<=320) return full;
    }
    const auto axis=[](double scale) { return scale>=3 ? 2u : 1u; };
    return {axis(target.width/640.0),axis(target.height/480.0)};
}
VkExtent2D coarser(VkExtent2D a, VkExtent2D b) { return {std::max(a.width,b.width),std::max(a.height,b.height)}; }
void vk_check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + " failed: VkResult " + std::to_string(result));
}
void barrier(VkCommandBuffer command, VkImage image, VkImageLayout before, VkImageLayout after,
             VkPipelineStageFlags2 source, VkAccessFlags2 source_access, VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access,VkImageAspectFlags aspect=VK_IMAGE_ASPECT_COLOR_BIT,uint32_t levels=1,uint32_t base_mip=0) {
    VkImageMemoryBarrier2 image_barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    image_barrier.srcStageMask = source; image_barrier.srcAccessMask = source_access;
    image_barrier.dstStageMask = destination; image_barrier.dstAccessMask = destination_access;
    image_barrier.oldLayout = before; image_barrier.newLayout = after;
    image_barrier.srcQueueFamilyIndex = image_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    image_barrier.image = image; image_barrier.subresourceRange = {aspect, base_mip, levels, 0, VK_REMAINING_ARRAY_LAYERS};
    VkDependencyInfo info{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; info.imageMemoryBarrierCount = 1; info.pImageMemoryBarriers = &image_barrier;
    vkCmdPipelineBarrier2(command, &info);
}
// The headset's eye block of a draw: both eyes' transforms (StereoScene, GL clip) as Vulkan clip to Vulkan clip, C * M * inverse(C),
// C = (x, -y, (z+w)/2, w); rows for the shader (smoke.slang StereoBlock).
void stereo_eye_block(const uc::xr::Matrices& matrices, float* block) {
    for(int eye=0;eye<2;++eye) {
        const float* m=matrices.view_projection[eye];
        const auto gl=[&](int r,int c){return double(m[c*4+r]);};
        double g[4][4];   // M * inverse(C): inverse(C) maps Vulkan clip back to GL (y'=-y, z'=2z-w)
        for(int r=0;r<4;++r) { g[r][0]=gl(r,0); g[r][1]=-gl(r,1); g[r][2]=2*gl(r,2); g[r][3]=gl(r,3)-gl(r,2); }
        for(int c=0;c<4;++c) { block[eye*16+0*4+c]=float(g[0][c]); block[eye*16+1*4+c]=float(-g[1][c]); block[eye*16+2*4+c]=float(.5*(g[2][c]+g[3][c])); block[eye*16+3*4+c]=float(g[3][c]); }
    }
}
VkShaderModule shader(VkDevice device, const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    auto size = file.tellg();
    if (!file || size <= 0 || size % 4 != 0) throw std::runtime_error("invalid or missing SPIR-V: " + path.string());
    std::vector<uint32_t> words(size_t(size) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(words.data()), size);
    if (!file || words[0] != 0x07230203) throw std::runtime_error("invalid SPIR-V header: " + path.string());
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = size_t(size); info.pCode = words.data();
    VkShaderModule module; vk_check(vkCreateShaderModule(device, &info, nullptr, &module), "vkCreateShaderModule"); return module;
}
}
void VulkanRenderer::owner() const {
    if (std::this_thread::get_id() != owner_) throw std::logic_error("Vulkan operation requires renderer owner thread");
}
VKAPI_ATTR VkBool32 VKAPI_CALL VulkanRenderer::debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* user) {
    auto& renderer = *static_cast<VulkanRenderer*>(user);
    // Motion-variant pipelines also run in passes without a vector attachment; VK_EXT_dynamic_rendering_unused_attachments makes that legal, the layer still reports it.
    // FidelityFX 1.1.4 declares its luma history storage image as Rgba8 in the shader and views an R16G16 image: a format the SDK mixes on purpose (same texel size).
    const bool known = std::strstr(message->pMessage, "this write is unused") != nullptr || std::strstr(message->pMessage, "rw_luma_history") != nullptr;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) && !known) renderer.errors_.fetch_add(1);
    std::fprintf(stderr, "Vulkan validation: %s\n", message->pMessage);
    return VK_FALSE;
}
VulkanRenderer::~VulkanRenderer() {
    try { shutdown(); } catch (const std::exception& error) { std::fprintf(stderr, "Vulkan shutdown failed: %s\n", error.what()); }
}
// The stereo preview's stand-in for xrLocateViews: a head at the anchor (HOTD4 anchors the camera on the head), eyes 63 mm apart, each with a
// Quest 3-like asymmetric field of view (wider towards its own side), in the compositor's tangent order (left, right, down, up). UC_VR_YAW and
// UC_VR_PITCH (degrees, left and up positive) turn the head, to look past the game's own frustum.
void VulkanRenderer::preview_publish() {
    static const float yaw = std::getenv("UC_VR_YAW") ? float(std::atof(std::getenv("UC_VR_YAW"))) * .017453292f : 0.f;
    static const float pitch = std::getenv("UC_VR_PITCH") ? float(std::atof(std::getenv("UC_VR_PITCH"))) * .017453292f : 0.f;
    const float cy = std::cos(yaw / 2), sy = std::sin(yaw / 2), cp = std::cos(pitch / 2), sp = std::sin(pitch / 2);
    const std::array<float, 4> head{cy * sp, sy * cp, -sy * sp, cy * cp};   // yaw about Y, then pitch about the turned X (x, y, z, w)
    constexpr float kHalfIpd = .0315f;
    const float outer = std::tan(52.f * .017453292f), inner = std::tan(41.f * .017453292f), down = std::tan(50.f * .017453292f), up = std::tan(42.f * .017453292f);
    std::array<xr::StereoPose, 2> eyes;
    for (int eye = 0; eye < 2; ++eye) {
        const float offset[3] = {eye ? kHalfIpd : -kHalfIpd, 0, 0};
        float turned[3];
        lg_stereo_rotate(head.data(), offset, turned);
        eyes[eye].orientation = head;
        eyes[eye].position = {turned[0], turned[1], turned[2]};
        eyes[eye].tangents = eye ? std::array<float, 4>{inner, outer, down, up} : std::array<float, 4>{outer, inner, down, up};
    }
    preview_->publish(eyes);
}

void VulkanRenderer::initialize(SDL_Window* window, bool validation, const std::filesystem::path& shaders, double render_scale, double render_aspect, bool exclusive_fullscreen) {
    if (instance_) throw std::logic_error("renderer already initialized");
    owner_ = std::this_thread::get_id(); window_ = window;
    render_aspect_=render_aspect;
    set_render_scale(render_scale);
    if (!window) throw std::invalid_argument("Vulkan requires an SDL window");
    uint32_t version = 0; vk_check(vkEnumerateInstanceVersion(&version), "vkEnumerateInstanceVersion");
    if (version < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 loader required");
    if (const char* v = std::getenv("UC_VR_PREVIEW"); !xr_ && v && *v == '1') {
        // The headset's stereo on the desktop: the headset's defaults (10 game units per metre as xr_android.cpp sets for this game, the
        // screen plane 3 m ahead; UC_VR_SCALE overrides the scale) and the screen lens the compositor starts from, before a frame shows the game's own.
        xr::StereoSettings settings;
        settings.scale = 10;
        settings.hud_fit = true;   // as the headset (xr_android.cpp)
        if (const char* s = std::getenv("UC_VR_SCALE")) settings.scale = float(std::atof(s));
        if (!(settings.scale > 0)) throw std::invalid_argument("UC_VR_SCALE must be positive");
        preview_ = std::make_unique<xr::StereoScene>(settings);
        preview_->set_screen_lens({1.7320508f / (16.f / 9), 1.7320508f});
        stereo_ = preview_.get();
        std::fprintf(stderr, "[vr-preview] stereo preview: scale %.3f game units per metre, eyes side by side\n", settings.scale);
        // The picture takes the simulated eye's shape (preview_publish: 52+41 degrees wide, 50+42 high), as the headset's takes its eye image's.
        render_aspect_ = double(std::tan(52.f * .017453292f) + std::tan(41.f * .017453292f)) / (std::tan(50.f * .017453292f) + std::tan(42.f * .017453292f));
        set_render_scale(configured_scale_);
    }
    if (preview_) std::fprintf(stderr, "[vr] eye picture aspect %.3f\n", render_aspect_);
    layers_ = (xr_ || preview_) ? 2 : 1;   // the headset (or its preview): both eyes in every pass (multiview)
    std::vector<const char*> extensions;
    if (!xr_) {   // the headset needs no window surface: OpenXR adds what it needs to the instance
        Uint32 count = 0; const char* const* required = SDL_Vulkan_GetInstanceExtensions(&count);
        if (!required) throw std::runtime_error(SDL_GetError());
        extensions.assign(required, required + count);
    }
    uint32_t available_count = 0; vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &available_count, nullptr), "instance extension count");
    std::vector<VkExtensionProperties> available(available_count);
    vk_check(vkEnumerateInstanceExtensionProperties(nullptr, &available_count, available.data()), "instance extensions");
    auto has = [&](const char* name) { return std::any_of(available.begin(), available.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; }); };
    const bool capabilities2=has(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    bool maintenance_khr = capabilities2 && has("VK_KHR_surface_maintenance1"), maintenance_ext = capabilities2 && has("VK_EXT_surface_maintenance1");
    if (capabilities2) extensions.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
    if (maintenance_khr) extensions.push_back("VK_KHR_surface_maintenance1");
    if (maintenance_ext) extensions.push_back("VK_EXT_surface_maintenance1");
    colorspace_ext_ = has(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);   // HDR10 surface formats; without it HDR stays off
    if (colorspace_ext_) extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    std::vector<std::string> dlss5_instance_names;   // DLSS 5: what NGX asks of the instance (nothing without nvngx_dlssnr.dll next to the exe)
    if (dlss5_allowed_) for (const auto& name : dlss5_instance_extensions(shaders.parent_path().parent_path()))
        if (has(name.c_str()) && std::none_of(extensions.begin(), extensions.end(), [&](const char* have) { return name == have; })) dlss5_instance_names.push_back(name);
    for (const auto& name : dlss5_instance_names) extensions.push_back(name.c_str());
    const char* layer = "VK_LAYER_KHRONOS_validation";
    if (validation) {
        uint32_t n = 0; vk_check(vkEnumerateInstanceLayerProperties(&n, nullptr), "layer count");
        std::vector<VkLayerProperties> layers(n); vk_check(vkEnumerateInstanceLayerProperties(&n, layers.data()), "layers");
        if (!std::any_of(layers.begin(), layers.end(), [&](const auto& item) { return std::strcmp(item.layerName, layer) == 0; }))
            throw std::runtime_error("VK_LAYER_KHRONOS_validation unavailable");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO}; application.pApplicationName = "Umbrella Chronicles"; application.apiVersion = VK_API_VERSION_1_3;
    VkDebugUtilsMessengerCreateInfoEXT debug_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    debug_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    debug_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_info.pfnUserCallback = debug; debug_info.pUserData = this;
#if defined(__APPLE__)
    // Vulkan on Metal (KosmicKrisp, MoltenVK) is a portability driver: the loader lists it only to an instance that asks for those.
    bool portability = false;
    {
        uint32_t n = 0; vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> available(n); vkEnumerateInstanceExtensionProperties(nullptr, &n, available.data());
        portability = std::any_of(available.begin(), available.end(), [](const auto& e) { return std::strcmp(e.extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME) == 0; });
        if (portability) extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
#endif
    VkInstanceCreateInfo instance_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; instance_info.pApplicationInfo = &application;
#if defined(__APPLE__)
    if (portability) instance_info.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
#endif
    instance_info.enabledExtensionCount = uint32_t(extensions.size()); instance_info.ppEnabledExtensionNames = extensions.data();
    if (validation) { instance_info.enabledLayerCount = 1; instance_info.ppEnabledLayerNames = &layer; instance_info.pNext = &debug_info; }
    {   // Optional backends must be ready before the instance, so live picks never need a new device.
        std::string reason;
        const auto directory=shaders.parent_path().parent_path();
        rr_loaded_=std::filesystem::exists(directory/"sl.dlss_d.dll");
        streamline_ = Streamline::load(directory, true, reason, rr_loaded_);
        if (!streamline_ && rr_loaded_) {   // a Ray Reconstruction plugin that fails slInit must not take DLSS down
            rr_loaded_=false; std::string retry;
            streamline_ = Streamline::load(directory, true, retry);
            if (streamline_) std::fprintf(stderr, "Ray Reconstruction unavailable: its plugin failed to initialise (%s)\n", reason.c_str());
        }
        if (!streamline_) {
            std::fprintf(stderr, "DLSS unavailable: %s\n", reason.c_str());
            if (upscaler_kind_==UpscalerKind::dlss) { upscaler_kind_=UpscalerKind::off; frame_generation_=false; }
        }
        else if (!validation && has(VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            // Streamline's Vulkan backend creates a debug messenger through the loader trampoline, which crashes when the extension is off.
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            instance_info.enabledExtensionCount = uint32_t(extensions.size()); instance_info.ppEnabledExtensionNames = extensions.data();
        }
    }
#ifdef UC_OPENXR
    if (xr_) vk_check(xr_->create_instance(instance_info, instance_), "OpenXR vkCreateInstance");
    else
#endif
    vk_check(streamline_ ? streamline_->create_instance(instance_info, instance_) : vkCreateInstance(&instance_info, nullptr, &instance_), "vkCreateInstance");
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        if (!create) throw std::runtime_error("debug-utils creation function unavailable");
        vk_check(create(instance_, &debug_info, nullptr, &messenger_), "debug messenger");
    }
    if (streamline_) {   // the surface is one of the calls Streamline intercepts
        void* hwnd = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (!hwnd) throw std::runtime_error("window has no native handle");
        vk_check(streamline_->create_surface(instance_, hwnd, surface_), "Streamline surface");
    } else if (!xr_ && !SDL_Vulkan_CreateSurface(window, instance_, nullptr, &surface_)) throw std::runtime_error(SDL_GetError());
    const auto enumerate = [&](uint32_t* count, VkPhysicalDevice* devices) { return streamline_ ? streamline_->enumerate_physical_devices(instance_, count, devices) : vkEnumeratePhysicalDevices(instance_, count, devices); };
    uint32_t n = 0; vk_check(enumerate(&n, nullptr), "physical device count");
    std::vector<VkPhysicalDevice> physical(n); vk_check(enumerate(&n, physical.data()), "physical devices");
    const char* chosen_maintenance = nullptr; int score = -1;
#ifdef UC_OPENXR
    if (xr_) {   // the GPU the runtime drives the headset with, its first graphics queue family
        physical_ = xr_->physical_device(instance_);
        uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count); vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
        for (uint32_t family = 0; family < family_count; ++family) if (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) { family_ = family; break; }
        physical.clear();
    }
#endif
    for (auto candidate : physical) {
        VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(candidate, &properties);
        if (properties.apiVersion < VK_API_VERSION_1_3) continue;
        uint32_t extension_count = 0; vk_check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, nullptr), "device extension count");
        std::vector<VkExtensionProperties> device_extensions(extension_count);
        vk_check(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &extension_count, device_extensions.data()), "device extensions");
        auto device_has = [&](const char* name) { return std::any_of(device_extensions.begin(), device_extensions.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; }); };
        const char* maintenance = maintenance_khr && device_has("VK_KHR_swapchain_maintenance1") ? "VK_KHR_swapchain_maintenance1" : maintenance_ext && device_has("VK_EXT_swapchain_maintenance1") ? "VK_EXT_swapchain_maintenance1" : nullptr;
        if (!device_has(VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR retirement{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR};
        VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.pNext = maintenance ? &retirement : nullptr;
        VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; features.pNext = &features13;
        vkGetPhysicalDeviceFeatures2(candidate, &features);
        if (!features13.dynamicRendering || !features13.synchronization2) continue;
        if (!retirement.swapchainMaintenance1) maintenance=nullptr;
        uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count); vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
        for (uint32_t family = 0; family < family_count; ++family) {
            VkBool32 present = VK_FALSE; vk_check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, family, surface_, &present), "surface queue support");
            int candidate_score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 1 : 0;
            if (present && (families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) && candidate_score > score) {
                physical_ = candidate; family_ = family; chosen_maintenance = maintenance; score = candidate_score;
            }
        }
    }
    if (!physical_) throw std::runtime_error("GPU requires Vulkan 1.3 graphics/present, dynamic rendering and synchronization2");
    present_fences_=chosen_maintenance!=nullptr;
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical_, &properties);
    max_image_dimension_=properties.limits.maxImageDimension2D;
    std::fprintf(stderr, "Vulkan device: %s; presentation retirement: %s\n", properties.deviceName, chosen_maintenance ? chosen_maintenance : "legacy (successor acquire, device-idle shutdown)");
    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 details{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};details.pNext=&driver;
    vkGetPhysicalDeviceProperties2(physical_,&details);
    std::fprintf(stderr,"[gpu] vendor=%04x device=%04x Vulkan=%u.%u.%u driver_version=%u driver=%s info=%s\n",properties.vendorID,properties.deviceID,VK_VERSION_MAJOR(properties.apiVersion),VK_VERSION_MINOR(properties.apiVersion),VK_VERSION_PATCH(properties.apiVersion),properties.driverVersion,driver.driverName,driver.driverInfo);
    if (streamline_) {
        std::string reason;
        dlss_available_=streamline_->dlss_supported(physical_, reason);
        if (!dlss_available_) {
            std::fprintf(stderr, "DLSS unavailable: %s\n", reason.c_str());
            streamline_->shutdown(); rr_available_=false;
            if (upscaler_kind_==UpscalerKind::dlss) { upscaler_kind_=UpscalerKind::off; frame_generation_=false; }
        } else {
            dlss_g_available_=streamline_->dlss_g_supported(physical_,reason);
            if (!dlss_g_available_ && upscaler_kind_==UpscalerKind::dlss) frame_generation_=false;
            if (rr_loaded_) { std::string rr_reason; rr_available_=streamline_->ray_reconstruction_supported(physical_,rr_reason); if (!rr_available_) std::fprintf(stderr,"Ray Reconstruction unavailable: %s\n",rr_reason.c_str()); }
        }
    }
    bool through_streamline = streamline_ && streamline_->active();
    bool private_data = false;   // Streamline keeps swapchain data in a VkPrivateDataSlot
    if (through_streamline) {
        VkPhysicalDeviceVulkan13Features supported13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext = &supported13;
        vkGetPhysicalDeviceFeatures2(physical_, &query);
        private_data = supported13.privateData;
        if (!private_data) {
            std::fprintf(stderr,"DLSS unavailable: Streamline presentation requires Vulkan privateData\n");
            streamline_->shutdown(); through_streamline=false; dlss_available_=dlss_g_available_=rr_available_=false;
            if(upscaler_kind_==UpscalerKind::dlss) { upscaler_kind_=UpscalerKind::off; frame_generation_=false; }
        }
    }
    const float priorities[3] = {1, 1, 1};
    uint32_t queue_count = 1;
    VkPhysicalDeviceVulkan12Features supported12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 query12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query12.pNext=&supported12;
    vkGetPhysicalDeviceFeatures2(physical_,&query12);
    {   // Optional interpolation queues are provisioned once, even when generation starts disabled.
        uint32_t family_count = 0; vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count); vkGetPhysicalDeviceQueueFamilyProperties(physical_, &family_count, families.data());
        if (families[family_].queueCount>=3 && supported12.timelineSemaphore) queue_count=3;
        else if (upscaler_kind_==UpscalerKind::fsr) frame_generation_=false;
    }
    VkDeviceQueueCreateInfo queue_info{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; queue_info.queueFamilyIndex = family_; queue_info.queueCount = queue_count; queue_info.pQueuePriorities = priorities;
    VkPhysicalDeviceSwapchainMaintenance1FeaturesKHR retirement{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_KHR}; retirement.swapchainMaintenance1 = VK_TRUE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; features13.pNext = present_fences_ ? &retirement : nullptr; features13.dynamicRendering = VK_TRUE; features13.synchronization2 = VK_TRUE;
    features13.privateData = private_data;
    VkPhysicalDeviceVulkan12Features features12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    features12.timelineSemaphore = queue_count == 3;   // the interpolation swapchain's queue sync
    features12.shaderFloat16=supported12.shaderFloat16;
    bool features12_chained = features12.timelineSemaphore || features12.shaderFloat16;
    if (features12_chained) { features12.pNext = features13.pNext; features13.pNext = &features12; }
    std::vector<const char*> device_extensions;
    if (!xr_) device_extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);   // the headset's swapchain is the runtime's
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    if (layers_ > 1) { features11.multiview = VK_TRUE; features11.pNext = features13.pNext; features13.pNext = &features11; }   // both eyes in one pass
    xform_ = layers_ > 1 && supported12.bufferDeviceAddress && !std::getenv("UC_NO_XFORM");
    // UC_DEPTH_FORMAT (depth_efb.h), the headset and its preview only: GX depth is 24-bit, so a 24-bit format holds it exactly. X8_D24 first
    // (depth only); D24S8 needs separate depth/stencil layouts for the depth-only barriers and views used here. Unsupported: D32 as before.
    // Default in VR: 24-bit (GX's own precision, Dolphin's Adreno choice; headset native 33.0 -> 31.2 ms with the clear fold, no z-fighting in
    // captures). UC_DEPTH_FORMAT=d32 keeps 32-bit float depth.
    const char* depth_env=std::getenv("UC_DEPTH_FORMAT");
    if (const char* wanted=depth_env && *depth_env ? depth_env : "d24"; layers_>1 && std::string(wanted)!="d32") {
        const auto usable=[&](VkFormat format) {
            VkFormatProperties properties{}; vkGetPhysicalDeviceFormatProperties(physical_,format,&properties);
            const VkFormatFeatureFlags needed=VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            return (properties.optimalTilingFeatures&needed)==needed;
        };
        const std::string name(wanted);
        if (name!="d24s8" && usable(VK_FORMAT_X8_D24_UNORM_PACK32)) depth_format_=VK_FORMAT_X8_D24_UNORM_PACK32;
        else if (name!="x8d24" && supported12.separateDepthStencilLayouts && usable(VK_FORMAT_D24_UNORM_S8_UINT)) {
            depth_format_=VK_FORMAT_D24_UNORM_S8_UINT; features12.separateDepthStencilLayouts=VK_TRUE;
            if (!features12_chained) { features12.pNext = features13.pNext; features13.pNext = &features12; features12_chained = true; }
        }
        std::fprintf(stderr,"[vulkan] EFB depth format %s (UC_DEPTH_FORMAT=%s)\n",depth_format_==VK_FORMAT_X8_D24_UNORM_PACK32 ? "X8_D24_UNORM_PACK32" :
                     depth_format_==VK_FORMAT_D24_UNORM_S8_UINT ? "D24_UNORM_S8_UINT" : "D32_SFLOAT (no usable 24-bit depth format)",wanted);
    }
    // Fragment-only uniform identity (uber_merge.h): with the pre-pass the draws' vertex stage reads no uniform block, so a run ends only where
    // the part a fragment reads differs.
    // On by default on the headset (Dolphin's split of vertex and pixel constants, S18 rec 4): fightvr replay at native, foveation high,
    // two alternating pairs 14.2 / 13.8 -> 13.6 / 13.6 ms GPU a frame; Mac preview identical with late Z. UC_MERGE_FRAG_UNIFORM=0 / 1 decides.
#ifdef __ANDROID__
    constexpr bool kFragUniformDefault = true;
#else
    constexpr bool kFragUniformDefault = false;
#endif
    const char* frag_uniform_env = std::getenv("UC_MERGE_FRAG_UNIFORM");
    frag_uniform_forced_ = frag_uniform_env && *frag_uniform_env;   // else the headset's Display page ("Draw merging", vr.cfg merge) decides live
    frag_uniform_merge_ = xform_ && (frag_uniform_forced_ ? std::strcmp(frag_uniform_env, "0") != 0 : kFragUniformDefault);
    if (frag_uniform_merge_) std::fprintf(stderr, "[vr] fragment-only uniform identity for merging on%s\n", frag_uniform_forced_ ? " (UC_MERGE_FRAG_UNIFORM)" : "");
    static_depth_ = layers_ > 1 && std::getenv("UC_STATIC_DEPTH");   // diagnostics: does dynamic depth state cost the tiler its LRZ?   // the headset's vertex pre-pass reads the vertices by address
    if (xform_) {
        features12.bufferDeviceAddress = VK_TRUE;
        if (!features12_chained) { features12.pNext = features13.pNext; features13.pNext = &features12; features12_chained = true; }
    }
    if (layers_ > 1) std::fprintf(stderr, "[vr] vertex pre-pass %s\n", xform_ ? "on" : supported12.bufferDeviceAddress ? "off (UC_NO_XFORM)" : "unavailable (no bufferDeviceAddress)");
    if (layers_ > 1) {   // the texture table (UC_TEX_MERGE=1): descriptor indexing. Features and limits are logged on every headset start.
        VkPhysicalDeviceVulkan12Properties properties12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext = &properties12;
        vkGetPhysicalDeviceProperties2(physical_, &properties);
        const auto& l = properties.properties.limits; const auto& p = properties12;
        std::fprintf(stderr, "[vulkan] descriptor indexing: runtime arrays %d, partially bound %d, variable count %d, non-uniform sampled images %d (native %d), "
                     "sampled images update after bind %d; per stage: samplers %u, sampled images %u, resources %u; per set: samplers %u, sampled images %u; "
                     "update after bind: per stage samplers %u, sampled images %u, resources %u, per set samplers %u, sampled images %u, all pools %u\n",
                     int(supported12.runtimeDescriptorArray), int(supported12.descriptorBindingPartiallyBound), int(supported12.descriptorBindingVariableDescriptorCount),
                     int(supported12.shaderSampledImageArrayNonUniformIndexing), int(p.shaderSampledImageArrayNonUniformIndexingNative), int(supported12.descriptorBindingSampledImageUpdateAfterBind),
                     l.maxPerStageDescriptorSamplers, l.maxPerStageDescriptorSampledImages, l.maxPerStageResources, l.maxDescriptorSetSamplers, l.maxDescriptorSetSampledImages,
                     p.maxPerStageDescriptorUpdateAfterBindSamplers, p.maxPerStageDescriptorUpdateAfterBindSampledImages, p.maxPerStageUpdateAfterBindResources,
                     p.maxDescriptorSetUpdateAfterBindSamplers, p.maxDescriptorSetUpdateAfterBindSampledImages, p.maxUpdateAfterBindDescriptorsInAllPools);
        // A combined image sampler counts as a sampler and a sampled image; set 0 already holds eight of each, the fragment stage a few more
        // resources (uniform block, attachments). At most 8192 entries (a packet of ~1000 draws with eight maps each).
        static constexpr int64_t kTableMax = 8192;
        const auto room = [](std::initializer_list<int64_t> limits) { return uint32_t(std::clamp<int64_t>(std::min(limits), 0, kTableMax)); };
        const uint32_t plain = room({int64_t(l.maxPerStageDescriptorSamplers) - 8, int64_t(l.maxPerStageDescriptorSampledImages) - 8, int64_t(l.maxDescriptorSetSamplers) - 8,
                                     int64_t(l.maxDescriptorSetSampledImages) - 8, int64_t(l.maxPerStageResources) - 32});
        const uint32_t after_bind = supported12.descriptorBindingSampledImageUpdateAfterBind ?
            room({int64_t(p.maxPerStageDescriptorUpdateAfterBindSamplers) - 8, int64_t(p.maxPerStageDescriptorUpdateAfterBindSampledImages) - 8, int64_t(p.maxDescriptorSetUpdateAfterBindSamplers) - 8,
                  int64_t(p.maxDescriptorSetUpdateAfterBindSampledImages) - 8, int64_t(p.maxPerStageUpdateAfterBindResources) - 32, int64_t(p.maxUpdateAfterBindDescriptorsInAllPools) / int64_t(kFrameSlots) - 8}) : 0;
        // The update-after-bind limits only when the plain ones are small (or UC_TEX_MERGE_UAB=1): the table is written before the packet is
        // recorded, so the flag buys nothing else.
        table_update_after_bind_ = after_bind > plain && (plain < 4096 || std::getenv("UC_TEX_MERGE_UAB"));
        // At most 1024 entries (a fight packet holds ~21): the Quest's driver charges a pool for the layout's whole array, whatever the variable
        // count of the set, so every slot's pool is sized to the limit (VK_ERROR_OUT_OF_POOL_MEMORY otherwise).
        table_limit_ = std::min<uint32_t>(1024, table_update_after_bind_ ? after_bind : plain);
        // Diagnostics: UC_TEX_MERGE_MAX=<entries> a smaller table (256 at least), UC_TEX_MERGE_EAGER=1 every draw on the table (not only runs that
        // join across textures), UC_TEX_MERGE_PX=<guest pixels> only draws whose screen box is at most that join across textures.
        if (const char* max = std::getenv("UC_TEX_MERGE_MAX")) table_limit_ = std::min<uint32_t>(table_limit_, uint32_t(std::max(256, std::atoi(max))));
        table_eager_ = std::getenv("UC_TEX_MERGE_EAGER") && std::strcmp(std::getenv("UC_TEX_MERGE_EAGER"), "0") != 0;
        table_max_pixels_ = std::getenv("UC_TEX_MERGE_PX") ? std::max(0.f, float(std::atof(std::getenv("UC_TEX_MERGE_PX")))) : 0.f;
        const bool indexing = supported12.runtimeDescriptorArray && supported12.descriptorBindingPartiallyBound && supported12.descriptorBindingVariableDescriptorCount &&
                              supported12.shaderSampledImageArrayNonUniformIndexing;
        // The small-draw ubershader (UC_UBER_SMALL=1|scene|hud, uber_merge.h) reads its draws' textures from the table: it turns the table on.
        // On the headset both are on by default (S18 section 8: fightvr replay at 1.0x, foveation High, 12.7 -> 10.7-11.0 ms GPU a frame);
        // UC_TEX_MERGE=0 turns both off, UC_UBER_SMALL=0 the ubershader alone. Elsewhere (the Mac's VR preview) both are opt-in.
#ifdef __ANDROID__
        constexpr const char* kTableDefault = "1";
#else
        constexpr const char* kTableDefault = "0";
#endif
        const char* tex_env = std::getenv("UC_TEX_MERGE");
        const char* uber_small = std::getenv("UC_UBER_SMALL");
        // Default: the EFB passes only. On the HUD layer the merged draws showed a panel of garbage texels in the middle of both eyes on the
        // headset (screen recordings; HUD layer off or table off: clean), so UC_UBER_SMALL=1 / hud there stays opt-in until that is fixed.
        if (!uber_small) uber_small = tex_env && std::strcmp(tex_env, "0") == 0 ? "0" : std::strcmp(kTableDefault, "0") == 0 ? "0" : "scene";
        const bool uber_wanted = std::strcmp(uber_small, "0") != 0;
        const bool wanted = std::strcmp(tex_env ? tex_env : kTableDefault, "0") != 0 || uber_wanted;
        tex_merge_ = wanted && xform_ && indexing && table_limit_ >= 256;
        if (uber_wanted && tex_merge_) {
            uber_small_scene_ = std::strcmp(uber_small, "hud") != 0;
            uber_small_hud_ = std::strcmp(uber_small, "scene") != 0;
            uber_max_pixels_ = std::getenv("UC_UBER_PX") ? std::max(0.f, float(std::atof(std::getenv("UC_UBER_PX")))) : kUberSmallPixels;
        }
        if (uber_wanted)
            std::fprintf(stderr, "[vr] small-draw ubershader %s (draws up to %.0f guest pixels, %s)\n", tex_merge_ ? "on" : "unavailable (no texture table)", uber_max_pixels_,
                         uber_small_scene_ && uber_small_hud_ ? "EFB passes and HUD layer" : uber_small_scene_ ? "EFB passes" : "HUD layer");
        std::fprintf(stderr, "[vr] texture table %s (%u entries a packet%s%s)\n", tex_merge_ ? "on" : !wanted ? "off (UC_TEX_MERGE=1 turns it on)" : !xform_ ? "unavailable (no vertex pre-pass)" :
                     !indexing ? "unavailable (descriptor indexing features missing)" : "unavailable (descriptor limits too small)", table_limit_, table_update_after_bind_ ? ", update after bind" : "",
                     table_eager_ ? ", every draw" : ", runs that join across textures");
        if (tex_merge_) {
            features12.runtimeDescriptorArray = features12.descriptorBindingPartiallyBound = features12.descriptorBindingVariableDescriptorCount = VK_TRUE;
            features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            features12.descriptorBindingSampledImageUpdateAfterBind = table_update_after_bind_;
            if (!features12_chained) { features12.pNext = features13.pNext; features13.pNext = &features12; features12_chained = true; }
        }
    }
    if(chosen_maintenance)device_extensions.push_back(chosen_maintenance);
    VkPhysicalDeviceFragmentShadingRateFeaturesKHR shading_rate{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_FEATURES_KHR}; bool shading_rate_wanted = false;
    {   // FidelityFX resolves these entry points by their extension names, including after a live switch.
        uint32_t n = 0; vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> supported(n); vkEnumerateDeviceExtensionProperties(physical_, nullptr, &n, supported.data());
#ifdef _WIN32
        disallow_exclusive_=!exclusive_fullscreen && capabilities2 && std::any_of(supported.begin(),supported.end(),[](const auto& extension){return std::strcmp(extension.extensionName,VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME)==0;});
        if(disallow_exclusive_)device_extensions.push_back(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
#else
        (void)exclusive_fullscreen; disallow_exclusive_=false;   // exclusive fullscreen exists on Win32 surfaces only
#endif
        // A driver that implements only a subset of Vulkan (MoltenVK) lists VK_KHR_portability_subset, and the device must then enable it.
        if (std::any_of(supported.begin(), supported.end(), [](const auto& extension) { return std::strcmp(extension.extensionName, "VK_KHR_portability_subset") == 0; }))
            device_extensions.push_back("VK_KHR_portability_subset");
        for (const char* name : {VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME, VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME})
            if (std::any_of(supported.begin(), supported.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; })) device_extensions.push_back(name);
        // LOAD_OP_NONE: a pass whose draws never touch depth leaves the depth attachment alone (neither loaded nor stored); either name.
        for (const char* name : {"VK_KHR_load_store_op_none", "VK_EXT_load_store_op_none"})
            if (!load_op_none_ && std::any_of(supported.begin(), supported.end(), [&](const auto& extension) { return std::strcmp(extension.extensionName, name) == 0; })) { device_extensions.push_back(name); load_op_none_ = true; }
        // Coarse shading for the game's bloom chain (blurred copies of a half-size scene, magnified): the pipeline rate is enough. On by default on
        // the headset (UC_NO_VRS=1: off), on the desktop with UC_VRS=1.
        if (((layers_ > 1 && !std::getenv("UC_NO_VRS")) || std::getenv("UC_VRS")) && std::any_of(supported.begin(), supported.end(), [](const auto& extension) { return std::strcmp(extension.extensionName, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) == 0; })) {
            VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext = &shading_rate;
            vkGetPhysicalDeviceFeatures2(physical_, &query);
            if (shading_rate.pipelineFragmentShadingRate) {
                device_extensions.push_back(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME);
                // The headset's foveation: the attachment rate as well, when the device has it (multiview selects each eye's layer).
                VkPhysicalDeviceFragmentShadingRatePropertiesKHR limits{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_PROPERTIES_KHR};
                VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2}; properties.pNext = &limits;
                vkGetPhysicalDeviceProperties2(physical_, &properties);
                std::string rates;
                if (const auto list = reinterpret_cast<PFN_vkGetPhysicalDeviceFragmentShadingRatesKHR>(vkGetInstanceProcAddr(instance_, "vkGetPhysicalDeviceFragmentShadingRatesKHR"))) {
                    uint32_t count = 0; list(physical_, &count, nullptr);
                    std::vector<VkPhysicalDeviceFragmentShadingRateKHR> found(count, {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADING_RATE_KHR});
                    list(physical_, &count, found.data());
                    fovea_largest_ = 1;
                    for (const auto& rate : found) {
                        rates += " " + std::to_string(rate.fragmentSize.width) + "x" + std::to_string(rate.fragmentSize.height);
                        if ((rate.sampleCounts & VK_SAMPLE_COUNT_1_BIT) && rate.fragmentSize.width == rate.fragmentSize.height && rate.fragmentSize.width <= 4)
                            fovea_largest_ = std::max(fovea_largest_, rate.fragmentSize.width);
                    }
                }
                std::fprintf(stderr, "[vulkan] shading rates: pipeline %d primitive %d attachment %d, texel %ux%u..%ux%u (aspect %u), layered %d, non-trivial combiners %d, with depth writes %d; rates%s\n",
                             int(shading_rate.pipelineFragmentShadingRate), int(shading_rate.primitiveFragmentShadingRate), int(shading_rate.attachmentFragmentShadingRate),
                             limits.minFragmentShadingRateAttachmentTexelSize.width, limits.minFragmentShadingRateAttachmentTexelSize.height,
                             limits.maxFragmentShadingRateAttachmentTexelSize.width, limits.maxFragmentShadingRateAttachmentTexelSize.height,
                             limits.maxFragmentShadingRateAttachmentTexelSizeAspectRatio, int(limits.layeredShadingRateAttachments),
                             int(limits.fragmentShadingRateNonTrivialCombinerOps), int(limits.fragmentShadingRateWithShaderDepthStencilWrites), rates.c_str());
                fovea_supported_ = layers_ > 1 && shading_rate.attachmentFragmentShadingRate && !std::getenv("UC_NO_FOVEATION") && fovea_largest_ > 1;
                if (fovea_supported_) {   // 16x16 texels when the device allows, else its nearest size
                    const auto pick = [](uint32_t want, uint32_t low, uint32_t high) { return std::clamp(want, std::max(1u, low), std::max(std::max(1u, low), high)); };
                    fovea_texel_ = {pick(16, limits.minFragmentShadingRateAttachmentTexelSize.width, limits.maxFragmentShadingRateAttachmentTexelSize.width),
                                    pick(16, limits.minFragmentShadingRateAttachmentTexelSize.height, limits.maxFragmentShadingRateAttachmentTexelSize.height)};
                    fovea_layered_ = limits.layeredShadingRateAttachments;
                    fovea_max_op_ = limits.fragmentShadingRateNonTrivialCombinerOps;
                }
                shading_rate.primitiveFragmentShadingRate = VK_FALSE;
                shading_rate.attachmentFragmentShadingRate = fovea_supported_;
                shading_rate.pNext = features13.pNext; features13.pNext = &shading_rate; shading_rate_wanted = true;
            }
        }
        if (through_streamline && std::any_of(supported.begin(),supported.end(),[](const auto& extension) { return std::strcmp(extension.extensionName,VK_KHR_PRESENT_ID_EXTENSION_NAME)==0; }))
            device_extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);   // dependency of the low-latency extension Streamline enables
    }
    std::fprintf(stderr,"Vulkan fullscreen policy: %s\n",disallow_exclusive_ ? "exclusive disallowed" : exclusive_fullscreen ? "driver default (exclusive requested)" : "driver default (extension unavailable)");
    // Upscaling: pipelines with the motion attachment also draw passes that bind none (post chain, HUD), which needs unused attachments.
    VkPhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT unused{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_FEATURES_EXT};
    {
        VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; query.pNext = &unused;
        vkGetPhysicalDeviceFeatures2(physical_, &query);
        VkPhysicalDeviceFeatures core_features{}; vkGetPhysicalDeviceFeatures(physical_, &core_features);
        // The headset has no effects path (HOTD4 has none).
        if (layers_ == 1 && unused.dynamicRenderingUnusedAttachments && core_features.independentBlend) { device_extensions.push_back(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME); unused.pNext = features13.pNext; features13.pNext = &unused; motion_outputs_ = true; }
        else { std::fprintf(stderr, "Upscaling unavailable: VK_EXT_dynamic_rendering_unused_attachments or independentBlend missing\n"); upscaler_kind_ = UpscalerKind::off; frame_generation_ = false; }
    }
    // Ray tracing (ray query in compute/fragment shaders): all of it or none, so a GPU without it still creates the device.
    VkPhysicalDeviceRayQueryFeaturesKHR ray_query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR}; ray_query.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR acceleration{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR}; acceleration.accelerationStructure = VK_TRUE;
    ray_tracing_ = motion_outputs_ && !ray_tracing_gap(physical_);
    if (ray_tracing_) {
        for (const char* name : {VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME}) device_extensions.push_back(name);
        if (!features12_chained) { features12.pNext = features13.pNext; features13.pNext = &features12; features12_chained = true; }
        features12.bufferDeviceAddress = features12.descriptorIndexing = features12.runtimeDescriptorArray = features12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
        acceleration.pNext = &ray_query; ray_query.pNext = features13.pNext; features13.pNext = &acceleration;
    }
    // DLSS 5 Neural Rendering: the extensions NGX asks of the device (the same on every NVIDIA RTX generation), only when the effects path exists and nvngx_dlssnr.dll is there.
    std::vector<std::string> dlss5_device_names;
    if (dlss5_allowed_) {
        const auto needs = dlss5_device_needs(instance_, physical_, shaders.parent_path().parent_path());
        dlss5_gap_ = motion_outputs_ || !needs.gap.empty() ? needs.gap : "the effects path is unavailable on this GPU";
        if (dlss5_gap_.empty()) {
            dlss5_device_ = true;
            for (const auto& name : needs.extensions)
                if (std::none_of(device_extensions.begin(), device_extensions.end(), [&](const char* have) { return name == have; })) dlss5_device_names.push_back(name);
            for (const auto& name : dlss5_device_names) device_extensions.push_back(name.c_str());
            if (!features12_chained) { features12.pNext = features13.pNext; features13.pNext = &features12; features12_chained = true; }
            features12.bufferDeviceAddress = VK_TRUE;
        }
    } else dlss5_gap_ = "DLSS 5 is off";
    VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; device_info.pNext = &features13;
    VkPhysicalDeviceFeatures core{}; vkGetPhysicalDeviceFeatures(physical_,&core);
    logic_ops_=core.logicOp;
    VkPhysicalDeviceFeatures enabled{}; enabled.logicOp=logic_ops_; enabled.independentBlend=motion_outputs_; enabled.shaderInt16=core.shaderInt16; device_info.pEnabledFeatures=&enabled;
    enabled.occlusionQueryPrecise=core.occlusionQueryPrecise;   // UC_FRAG_DUMP: exact fragment counts per draw
    enabled.samplerAnisotropy=core.samplerAnisotropy; anisotropy_supported_=core.samplerAnisotropy; enabled.textureCompressionBC=core.textureCompressionBC;
    // The headset: each eye's frustum keeps the game's near and far planes (its fog reads that depth), but a turned head puts the far plane
    // through the game's distant backdrop at an angle: what fell beyond it was clipped, and the view through the train's windows came out
    // spotty, changing with every head movement. Depth clamping keeps it drawn at the far depth instead (the game tests LEQUAL).
    depth_clamp_=layers_>1 && core.depthClamp && !std::getenv("UC_VR_NO_DEPTH_CLAMP"); enabled.depthClamp=depth_clamp_;
    if (layers_>1) std::fprintf(stderr,"[vr] depth clamp %s\n",depth_clamp_ ? "on" : core.depthClamp ? "off (UC_VR_NO_DEPTH_CLAMP)" : "unavailable");
    enabled.shaderStorageImageExtendedFormats=core.shaderStorageImageExtendedFormats;   // r8/rg8/rg16f storage images of GTAO, SMAA and motion blur
    { VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical_,&properties); anisotropy_limit_=properties.limits.maxSamplerAnisotropy; }
    device_info.queueCreateInfoCount = 1; device_info.pQueueCreateInfos = &queue_info;   // queue_info.queueCount: 3 with the interpolation swapchain
    device_info.enabledExtensionCount = uint32_t(device_extensions.size()); device_info.ppEnabledExtensionNames = device_extensions.data();
#ifdef UC_OPENXR
    if (xr_) vk_check(xr_->create_device(physical_, device_info, device), "OpenXR vkCreateDevice");
    else
#endif
    vk_check(through_streamline ? streamline_->create_device(instance_, physical_, device_info, device) : vkCreateDevice(physical_, &device_info, nullptr, &device), "vkCreateDevice");
    vk_android_load_device(device);   // Android: the 1.2/1.3 functions called directly come from this device (vulkan_android_entry.cpp)
    if (shading_rate_wanted) set_shading_rate_ = reinterpret_cast<PFN_vkCmdSetFragmentShadingRateKHR>(vkGetDeviceProcAddr(device, "vkCmdSetFragmentShadingRateKHR"));
    std::fprintf(stderr, "[vulkan] fragment shading rate %s; foveation %s (texel %ux%u, %s, largest %ux%u)\n", set_shading_rate_ ? "on (bloom chain)" : "off",
                 fovea_supported_ ? "available" : "unavailable", fovea_texel_.width, fovea_texel_.height, fovea_layered_ ? "a layer per eye" : "one layer for both eyes", fovea_largest_, fovea_largest_);
    if (through_streamline) {
        base_present_=streamline_->present_functions(device);
        present_.present=base_present_.present;   // SR/RR also need Streamline's per-present tag recycling and garbage collection.
    }
    if (upscaler_kind_==UpscalerKind::dlss && frame_generation_ && dlss_g_available_) {
        dlss_presentation_=true; present_=base_present_;
    }
    vkGetDeviceQueue(device, family_, 0, &queue_); vkGetPhysicalDeviceMemoryProperties(physical_, &memory_);
    dlss5_context_={instance_, physical_, device, vkGetInstanceProcAddr, vkGetDeviceProcAddr, memory_, shaders, shaders.parent_path().parent_path(),
        [this] { vk_check(present_.wait_idle(device), "DLSS 5 idle"); }, {}};
    if (dlss5_device_) dlss5_pass_.configure(dlss5_context_);
    if (queue_count == 3 && motion_outputs_) {
        vkGetDeviceQueue(device, family_, 1, &fg_present_queue_); vkGetDeviceQueue(device, family_, 2, &fg_acquire_queue_);
        try {
            fsr_g_ = std::make_unique<FsrFrameGeneration>(device, physical_, FsrFrameGeneration::Queues{queue_, fg_present_queue_, fg_acquire_queue_, family_}, shaders.parent_path().parent_path());
            if (upscaler_kind_==UpscalerKind::fsr && frame_generation_) { fsr_presentation_=true; present_=fsr_g_->present_functions(); }
        } catch (const std::exception& error) { std::fprintf(stderr, "FSR Frame Generation unavailable: %s\n", error.what()); if(upscaler_kind_==UpscalerKind::fsr)frame_generation_=false; }
    }
    VkPhysicalDeviceProperties limits{}; vkGetPhysicalDeviceProperties(physical_,&limits);
    const auto alignment=size_t(limits.limits.minUniformBufferOffsetAlignment);
    uniform_stride_=(kDrawUniformBytes+alignment-1)/alignment*alignment;
    stereo_stride_=(kStereoBlockBytes+alignment-1)/alignment*alignment;   // the headset's eye blocks, one uniform-buffer binding each
    static_assert(kDrawUniformBytes==136*16, "tev.slang stereo DrawState");
    if (xform_ && uniform_stride_!=kDrawUniformBytes) { xform_=false; std::fprintf(stderr,"[vr] vertex pre-pass off: uniform stride %zu\n",uniform_stride_); }   // xformMain reads the uniform blocks as an array
    VkCommandPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pool_info.queueFamilyIndex = family_;
    vk_check(vkCreateCommandPool(device, &pool_info, nullptr, &pool_), "command pool");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool = pool_; allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount = uint32_t(slots_.size());
    VkCommandBuffer commands[kFrameSlots]; vk_check(vkAllocateCommandBuffers(device, &allocation, commands), "command buffers");
    VkQueryPoolCreateInfo timing{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; timing.queryType = VK_QUERY_TYPE_TIMESTAMP; timing.queryCount = uint32_t(slots_.size() * 2);
    vk_check(vkCreateQueryPool(device, &timing, nullptr, &timing_pool_), "timing query pool");
    timestamp_period_ = limits.limits.timestampPeriod;
    pass_timer_.initialize(device, timestamp_period_, uint32_t(slots_.size()));
    for (size_t i = 0; i < slots_.size(); ++i) {
        slots_[i].command = commands[i];
        VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk_check(vkCreateFence(device, &fence, nullptr, &slots_[i].fence), "frame fence");
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vk_check(vkCreateSemaphore(device, &semaphore, nullptr, &slots_[i].acquired), "acquire semaphore");
    }
    shaders_=shaders; cache_dir_=shaders_.parent_path();
    const std::string variant=layers_>1 ? "smoke.stereo." : motion_outputs_ ? "smoke.motion." : "smoke.";
    if (motion_outputs_) { shadow_vertex_module_=shader(device, shaders_ / "smoke.shadow.vert.spv"); shadow_map_.initialize(device,memory_); }
    vertex_module_=shader(device, shaders_ / (variant+"vert.spv")); fragment_module_=shader(device, shaders_ / (variant+"frag.spv")); if (layers_>1 && !std::getenv("UC_NO_NOBANK_VS")) nobank_vertex_module_=shader(device, shaders_ / "smoke.stereo.nobank.vert.spv"); early_module_=shader(device, shaders_ / (variant+"early.frag.spv")); uber_module_=shader(device, shaders_ / (variant+"uber.spv"));
    if (layers_>1) {   // diagnostics (the headset's GPU cost split): UC_PROBE_VS=1 no lighting or texgen in the vertex stage, UC_PROBE_FS=1 the vertex colour as the fragment, UC_PROBE_NOINV=1 no Invariant position
        const char* probe_vs=std::getenv("UC_PROBE_VS") ? "smoke.stereo.probe.vert.spv" : std::getenv("UC_PROBE_NOINV") ? "smoke.stereo.nobank.noinv.vert.spv" : nullptr;
        if (probe_vs && nobank_vertex_module_) { vkDestroyShaderModule(device,nobank_vertex_module_,nullptr); nobank_vertex_module_=shader(device, shaders_ / probe_vs); std::fprintf(stderr,"[probe] vertex stage %s\n",probe_vs); }
        if (std::getenv("UC_PROBE_FS")) {
            probe_fragment_=true;
            vkDestroyShaderModule(device,fragment_module_,nullptr); vkDestroyShaderModule(device,early_module_,nullptr);
            fragment_module_=shader(device, shaders_ / "smoke.stereo.probe.frag.spv"); early_module_=shader(device, shaders_ / "smoke.stereo.probe.frag.spv");
            std::fprintf(stderr,"[probe] fragment stage: vertex colour\n");
        }
    }
    if (xform_) {   // the vertex pre-pass: its compute pipeline (own set: jobs, chunks, the draws' uniforms, palettes, banks, outputs)
        xform_vertex_module_=shader(device, shaders_ / "smoke.stereo.xform.vert.spv"); xform_compute_module_=shader(device, shaders_ / "smoke.stereo.xform.comp.spv");
        VkDescriptorSetLayoutBinding bindings[7]{};
        const uint32_t numbers[7]={2,3,4,6,7,8,9};
        for (uint32_t i=0;i<7;++i) bindings[i]={numbers[i],VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set_info.bindingCount=7; set_info.pBindings=bindings;
        vk_check(vkCreateDescriptorSetLayout(device,&set_info,nullptr,&xform_set_layout_),"pre-pass descriptor layout");
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_info.setLayoutCount=1; layout_info.pSetLayouts=&xform_set_layout_;
        vk_check(vkCreatePipelineLayout(device,&layout_info,nullptr,&xform_layout_),"pre-pass pipeline layout");
        VkComputePipelineCreateInfo create{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        create.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,xform_compute_module_,"xformMain",nullptr};
        create.layout=xform_layout_;
        vk_check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&create,nullptr,&xform_pipeline_),"pre-pass pipeline");
    }
    if (tex_merge_ && probe_fragment_) { tex_merge_=false; std::fprintf(stderr,"[vr] texture table off: UC_PROBE_FS\n"); }
    if (tex_merge_) {   // the texture table's modules (TEX_TABLE): the pre-pass's draw vertex stage with the table entry, the fragment stage reading the table
        try {
            table_vertex_module_=shader(device, shaders_ / "smoke.stereo.xform.tex.vert.spv");
            table_fragment_module_=shader(device, shaders_ / "smoke.stereo.tex.frag.spv"); table_early_module_=shader(device, shaders_ / "smoke.stereo.tex.early.frag.spv");
        } catch (const std::exception& error) { tex_merge_=false; std::fprintf(stderr,"[vr] texture table off: %s\n",error.what()); }
    }
    if (!tex_merge_) uber_small_scene_=uber_small_hud_=false;
    if (uber_small_scene_ || uber_small_hud_) {   // the small-draw ubershader's modules (UBER_MERGE): the vertex stage reading the pre-pass's vertex map, the ubershader fragment stages
        try {
            merge_vertex_module_=shader(device, shaders_ / "smoke.stereo.xform.uber.vert.spv");
            merge_fragment_module_=shader(device, shaders_ / "smoke.stereo.uber.merge.frag.spv"); merge_early_module_=shader(device, shaders_ / "smoke.stereo.uber.merge.early.frag.spv");
        } catch (const std::exception& error) { uber_small_scene_=uber_small_hud_=false; std::fprintf(stderr,"[vr] small-draw ubershader off: %s\n",error.what()); }
        // its families of draws with one and two stages (uber_merge.h uber_family); UC_UBER_FAMILY=0: every draw on the general module,
        // =1 / =2: only that family
        uber_families_=uber_small_scene_ || uber_small_hud_ ? 3u : 0u;
        if (const char* family=std::getenv("UC_UBER_FAMILY")) uber_families_&=std::strcmp(family,"1")==0 ? 1u : std::strcmp(family,"2")==0 ? 2u : std::strcmp(family,"0")==0 ? 0u : 3u;
        for (uint32_t family=1;family<=kUberFewStages;++family) if (uber_families_&(1u<<(family-1))) try {
            const std::string n=std::to_string(family);
            few_vertex_module_[family-1]=shader(device, shaders_ / ("smoke.stereo.xform.uber"+n+".vert.spv"));
            few_fragment_module_[family-1]=shader(device, shaders_ / ("smoke.stereo.uber"+n+".merge.frag.spv")); few_early_module_[family-1]=shader(device, shaders_ / ("smoke.stereo.uber"+n+".merge.early.frag.spv"));
        } catch (const std::exception& error) { uber_families_&=~(1u<<(family-1)); std::fprintf(stderr,"[vr] small-draw ubershader family %u off: %s\n",family,error.what()); }
        if (uber_small_scene_ || uber_small_hud_) std::fprintf(stderr,"[vr] small-draw ubershader families: 1 stage %s, 2 stages %s\n",uber_families_&1 ? "on" : "off",uber_families_&2 ? "on" : "off");
    }
    load_pipeline_cache();
    if (motion_outputs_ && sharpening_) rcas_.initialize(device, memory_, shaders_);
    gx::DrawPacket initial; initial.primitive=0x90; pipeline_for(pipeline_key(initial));
    start_workers();
#ifdef UC_OPENXR
    if (xr_) {   // the headset: the session and its swapchain; the game's picture keeps its own size (the screen plane scales it)
        xr_->set_fovea_support(fovea_supported_);   // the screen pass may shade by the eye image's foveation map
        xr_->attach(instance_, physical_, device, family_, shaders_);
        stereo_ = &xr_->scene();
        // Each eye's picture has the shape of its eye image, so the screen pass maps it 1:1 onto the eye's view (a 16:9 picture stretched
        // over a nearly square eye gave the vertical view 540 rows at 0.75x). The game keeps its 16:9 view (aim, hit tests, menus): the
        // per-eye reprojection maps its clip space to each eye's frustum whatever the target's shape.
        if (xr_->eye_width() && xr_->eye_height()) {
            render_aspect_ = double(xr_->eye_width()) / xr_->eye_height();
            // Native: the scene (720 rows at 1x) gets exactly the eye image's rows, so the eye sees the game at the display's resolution
            // with nothing resampled (2064x2208 on the Quest 3: 3.07x). An explicit render_scale overrides it.
            set_render_scale(native_eye_scale_ ? std::clamp(xr_->eye_height() / 720.0, 0.5, 6.0) : configured_scale_);
        }
        std::fprintf(stderr, "[vr] eye picture aspect %.3f, scale %.3f\n", render_aspect_, configured_scale_);
        const auto picture=manual_render_extent(configured_scale_,render_aspect_);
        resize(picture.width, picture.height);
    } else
#endif
    {
    int width, height; if (!SDL_GetWindowSizeInPixels(window, &width, &height)) throw std::runtime_error(SDL_GetError());
    resize(uint32_t(std::max(0, width)), uint32_t(std::max(0, height)));
    }
    if (requested_.width && requested_.height) rebuild();
}
// Push constants of every draw: clip matrix plus where to find this draw's matrices (see smoke.slang).
constexpr uint32_t kNoBank=UINT32_MAX;
// bank: texture bank index, kNoBank = the texgen's own matrix. motion (kNoMotion = none), jitter (target pixels) and viewport (target pixels)
// are read by the upscaling variant only.
constexpr uint32_t kNoMotion=UINT32_MAX;

constexpr VkFormat kMotionFormat=VK_FORMAT_R16G16B16A16_SFLOAT;
constexpr VkFormat kGBufferFormat=VK_FORMAT_R16G16B16A16_UNORM;   // gbuffer.h
constexpr float kShadowDistance=30.f;   // how far from the camera the shadow map reaches, in view units (metres)
struct DrawPush { gx::Projection projection; uint32_t palette, current_row, indexed, alpha_pass, bank, motion=kNoMotion; float jitter[2]{}; float viewport[4]{}; uint32_t effects=0, record=kRtNoRecord; float shift[2]{}; };   // effects: kEffect* bits (graphics_effects.h); record: rt_material.h
static_assert(sizeof(DrawPush)==128 && offsetof(DrawPush,jitter)==88 && offsetof(DrawPush,viewport)==96 && offsetof(DrawPush,effects)==112 && offsetof(DrawPush,record)==116 && offsetof(DrawPush,shift)==120);
constexpr VkFormat kMaterialFormat=VK_FORMAT_R32_UINT, kAlbedoFormat=VK_FORMAT_R8G8B8A8_UNORM;   // the RT pair (rt_material.h)
struct MotionBlock { gx::Projection prev_rows; uint32_t prev_palette, kind, unused[2]; };   // smoke.slang MotionBlock (std430)
static_assert(sizeof(MotionBlock)==80);
uint32_t VulkanRenderer::memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & properties) == properties) return i;
    throw std::runtime_error("required Vulkan memory type unavailable");
}
VulkanRenderer::Buffer VulkanRenderer::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool prefer_device_local) {
    Buffer buffer; buffer.size = size;
    VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; create.size = size; create.usage = usage;
    vk_check(vkCreateBuffer(device, &create, nullptr, &buffer.buffer), "vkCreateBuffer");
    try {
        VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device, buffer.buffer, &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        VkMemoryAllocateFlagsInfo address{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO}; address.flags=VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
        if (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) allocate.pNext=&address;   // rt_input_usage(): the ray tracing builder reads it by address
        // A small BAR heap can fill up even when ordinary host memory is still available.
        vk_check(allocate_host_buffer(memory_, requirements.memoryTypeBits, prefer_device_local, [&](uint32_t type) {
            allocate.memoryTypeIndex=type;
            return vkAllocateMemory(device, &allocate, nullptr, &buffer.memory);
        }), "buffer memory");
        vk_check(vkBindBufferMemory(device, buffer.buffer, buffer.memory, 0), "bind buffer memory");
        vk_check(vkMapMemory(device, buffer.memory, 0, size, 0, &buffer.mapped), "map buffer memory");
    } catch (...) { destroy_buffer(buffer); throw; }
    return buffer;
}
void VulkanRenderer::destroy_buffer(Buffer& buffer) {
    if (buffer.mapped) vkUnmapMemory(device, buffer.memory);
    if (buffer.buffer) vkDestroyBuffer(device, buffer.buffer, nullptr);
    if (buffer.memory) vkFreeMemory(device, buffer.memory, nullptr);
    buffer = {};
}
static size_t validate_texture_chain(const gx::TexturePixels* pixels) {
    if(pixels) pixels->wait();   // the background decode of a freshly created texture
    const gx::TextureLevel white{1,1,{255,255,255,255}};
    const bool gpu_only=pixels && pixels->gpu_only;
    if(pixels && (unsigned(pixels->channels)>unsigned(gx::TextureChannels::rgb565) ||
        (pixels->channels!=gx::TextureChannels::rgba && !gpu_only)))throw std::invalid_argument("invalid owned texture channels");
    if(pixels && (pixels->levels.empty() || pixels->levels.size()>15)) throw std::invalid_argument("invalid owned mip chain count");
    const auto& level=pixels ? pixels->levels[0] : white;
    const bool bc7=pixels && pixels->bc7;
    const auto level_bytes=[bc7](const gx::TextureLevel& mip) { return bc7 ? gx::bc7_level_size(mip.width,mip.height) : size_t(mip.width)*mip.height*4; };
    // The device image limit is checked in make_texture; GX textures stay <=1024, replacements may be larger.
    if (!level.width || !level.height || level.width>16384 || level.height>16384 || (gpu_only ? !level.rgba.empty() : level.rgba.size()!=level_bytes(level)))
        throw std::invalid_argument("invalid owned RGBA texture");
    const auto mip_levels=pixels ? uint32_t(pixels->levels.size()) : 1;
    size_t upload_size=0; uint32_t width=level.width,height=level.height;
    for(uint32_t i=0;i<mip_levels;++i) {
        const auto& mip=pixels ? pixels->levels[i] : white;
        if(mip.width!=width || mip.height!=height || (gpu_only ? !mip.rgba.empty() : mip.rgba.size()!=level_bytes(mip)) || (i &&pixels->levels[i-1].width==1 && pixels->levels[i-1].height==1)) throw std::invalid_argument("invalid owned mip chain dimensions");
        upload_size+=mip.rgba.size(); width=std::max(1u,width/2); height=std::max(1u,height/2);
    }
    return upload_size;
}
VulkanRenderer::Texture VulkanRenderer::make_texture(const gx::TexturePixels* pixels, VkExtent2D physical_extent) {
    const auto upload_size=validate_texture_chain(pixels);
    const gx::TextureLevel white{1,1,{255,255,255,255}};
    const bool gpu_only=pixels && pixels->gpu_only;
    const auto& level=pixels ? pixels->levels[0] : white;
    Texture texture; texture.extent=physical_extent.width ? physical_extent : VkExtent2D{level.width,level.height};
    texture.channels=pixels ? pixels->channels : gx::TextureChannels::rgba;
    texture.bc7=pixels && pixels->bc7;
    // shortcut: RGB565 uses Vulkan UNORM rounding; verify exact GX copy rounding before pixel-parity acceptance (TD-045).
    // shortcut: textureCompressionBC is enabled when the GPU has it and assumed for replacements; every desktop GPU does.
    const VkFormat format=texture.bc7 ? VK_FORMAT_BC7_UNORM_BLOCK : texture.channels==gx::TextureChannels::rgb565 ? VK_FORMAT_R5G6B5_UNORM_PACK16 : VK_FORMAT_R8G8B8A8_UNORM;
    if(texture.channels==gx::TextureChannels::rgb565) {
        VkFormatProperties properties; vkGetPhysicalDeviceFormatProperties(physical_,format,&properties);
        const VkFormatFeatureFlags required=VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_BLIT_SRC_BIT|VK_FORMAT_FEATURE_BLIT_DST_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if((properties.optimalTilingFeatures&required)!=required)throw std::runtime_error("GPU lacks native RGB565 copy/filter support");
    }
    if(texture.extent.width>max_image_dimension_ || texture.extent.height>max_image_dimension_) throw std::invalid_argument("texture extent exceeds device image limits");
    texture.mip_levels=pixels ? uint32_t(pixels->levels.size()) : 1;
    texture.mip_levels=std::min(texture.mip_levels,uint32_t(std::bit_width(std::max(texture.extent.width,texture.extent.height))));
    const uint32_t source_levels=texture.mip_levels;
    // The headset (stereo): most of the game's textures have one level and are minified a lot in a headset's view (a 687x720 eye picture
    // magnified over about 100 degrees): without mips they sparkled with every head movement. Such a texture gets the full chain, levels
    // 1.. made on the GPU when it is uploaded, and a sampler with linear mip filtering. UC_NO_AUTO_MIPS=1: one level, as on the desktop.
    static const bool no_auto_mips=std::getenv("UC_NO_AUTO_MIPS")!=nullptr;
    // Only fully opaque textures: a cut-out texture's transparent texels are black, and averaging them into the levels below pulled black
    // into the cut-out's edges and shrank what its alpha test keeps, so outlines blacked out and flickered as the level changed with distance.
    const auto opaque=[&] {
        const auto& rgba=pixels->levels[0].rgba;
        for(size_t i=3;i<rgba.size();i+=4) if(rgba[i]!=255) return false;
        return true;
    };
    if(layers_>1 && !no_auto_mips && !gpu_only && pixels && pixels->levels.size()==1 && !texture.bc7 && texture.channels==gx::TextureChannels::rgba && opaque()) {
        texture.mip_levels=uint32_t(std::bit_width(std::max(texture.extent.width,texture.extent.height)));
        texture.auto_mips=texture.mip_levels>1;
    }
    try {
        if(!gpu_only) texture.upload=make_buffer(upload_size,VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        size_t at=0;
        if(!gpu_only) for(uint32_t i=0;i<source_levels;++i) { const auto& mip=pixels ? pixels->levels[i] : white; std::memcpy(static_cast<std::byte*>(texture.upload.mapped)+at,mip.rgba.data(),mip.rgba.size()); at+=mip.rgba.size(); }
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; create.imageType=VK_IMAGE_TYPE_2D; create.format=format;
        create.extent={texture.extent.width,texture.extent.height,1}; create.mipLevels=texture.mip_levels; create.arrayLayers=gpu_only ? layers_ : 1; create.samples=VK_SAMPLE_COUNT_1_BIT;
        create.tiling=VK_IMAGE_TILING_OPTIMAL; create.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|(gpu_only || texture.auto_mips ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
        vk_check(vkCreateImage(device,&create,nullptr,&texture.image),"texture image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,texture.image,&requirements);
        texture.allocation_size=requirements.size;
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size;
        allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device,&allocate,nullptr,&texture.memory),"texture memory");
        vk_check(vkBindImageMemory(device,texture.image,texture.memory,0),"bind texture memory");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=texture.image; view.viewType=layers_>1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        view.format=format; view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,texture.mip_levels,0,create.arrayLayers};
        if(texture.channels==gx::TextureChannels::alpha_intensity)
            view.components={VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_A,VK_COMPONENT_SWIZZLE_A};
        vk_check(vkCreateImageView(device,&view,nullptr,&texture.view),"texture view");
    } catch (...) {
        if (texture.view) vkDestroyImageView(device,texture.view,nullptr);
        if (texture.image) vkDestroyImage(device,texture.image,nullptr);
        if (texture.memory) vkFreeMemory(device,texture.memory,nullptr);
        destroy_buffer(texture.upload); throw;
    }
    ++texture_creations_;
    return texture;
}
VulkanRenderer::Texture VulkanRenderer::make_copy_texture(const std::shared_ptr<const gx::TexturePixels>& source) {
    if(!source || !source->gpu_only) throw std::invalid_argument("invalid owned GPU copy texture");
    validate_texture_chain(source.get());
    const auto& level=source->levels[0];
    const VkExtent2D physical{std::max(1u,uint32_t((uint64_t(level.width)*efb_extent_.width+320)/640)),std::max(1u,uint32_t((uint64_t(level.height)*efb_extent_.height+240)/480))};
    const auto mip_levels=std::min(uint32_t(source->levels.size()),uint32_t(std::bit_width(std::max(physical.width,physical.height))));
    auto found=std::find_if(recycled_copies_.begin(),recycled_copies_.end(),[&](const auto& texture) {
        return texture.extent.width==physical.width && texture.extent.height==physical.height && texture.mip_levels==mip_levels && texture.channels==source->channels;
    });
    Texture result;
    if(found==recycled_copies_.end()) result=make_texture(source.get(),physical);
    else {
        result=std::move(*found); recycled_copy_bytes_-=result.allocation_size;
        if(found!=recycled_copies_.end()-1) *found=std::move(recycled_copies_.back());
        recycled_copies_.pop_back();
    }
    result.source=source; result.initialized=false;
    return result;
}
void VulkanRenderer::record_texture_copy(VkCommandBuffer command, VkImage source, VkExtent2D extent, const std::array<uint32_t,4>& region, Texture& target, VkImageLayout before) {
    const auto [x,y,width,height]=region;
    const bool initialized=before!=VK_IMAGE_LAYOUT_UNDEFINED;
    barrier(command,target.image,before,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,initialized ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_NONE,
        initialized ? VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT : 0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,target.mip_levels);
    VkImageBlit copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers_};
    copy.srcOffsets[0]={int32_t(uint64_t(x)*extent.width/640),int32_t(uint64_t(y)*extent.height/480),0};
    copy.srcOffsets[1]={std::max(copy.srcOffsets[0].x+1,int32_t(uint64_t(x+width)*extent.width/640)),std::max(copy.srcOffsets[0].y+1,int32_t(uint64_t(y+height)*extent.height/480)),1};
    copy.dstOffsets[1]={int32_t(target.extent.width),int32_t(target.extent.height),1};
    vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_LINEAR);
    // Level 0 only. Every EFB copy has a GX mip chain down to 1x1 (gx.cpp make_copy_texture), but the game samples its copies without a
    // mip filter (the train fight: none of ~120 copy samples per frame): levels 1.. are made when a draw first samples the copy with one
    // (record_copy_mips). Until then they are undefined, and never read (such a sampler's maxLod is 0).
    barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,target.mip_levels);
    target.mips_pending=target.mip_levels>1;
    static const bool eager_mips=std::getenv("UC_EAGER_COPY_MIPS")!=nullptr;   // diagnostics: every copy's chain at once, as before
    if (eager_mips) record_copy_mips(command,target);
}
// UC_FOLD_CLEAR: the waiting EFB clear as a pass of its own over the cleared rectangle (what render's clear_region does for the EFB), for
// whatever reads or writes the EFB before a pass of it could take the clear as its load op.
void VulkanRenderer::record_efb_clear(VkCommandBuffer command,const char* why) {
    if (!efb_clear_.pending) return;
    static const bool log=gx::depth_switch("UC_FOLD_CLEAR_LOG");   // diagnostics: what took a waiting clear as a pass of its own
    if (log) std::fprintf(stderr,"[fold-clear] frame %llu: a pass of its own (%s)\n",(unsigned long long)frame_index_,why);
    efb_clear_.pending=false; ++clear_passes_;
    const auto depth_stages=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier(command,depth_,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView=efb_view_; color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE; color.clearValue.color=efb_clear_.color;
    depth.imageView=depth_view_; depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil={efb_clear_.depth,0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea=efb_clear_.rect; rendering.layerCount=1; rendering.viewMask=layers_>1 ? 0b11u : 0u;
    rendering.colorAttachmentCount=1; rendering.pColorAttachments=&color; rendering.pDepthAttachment=&depth;
    vkCmdBeginRendering(command,&rendering);   // the load ops clear the render area, which is the cleared rectangle
    vkCmdEndRendering(command);
    barrier(command,efb_,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
}
void VulkanRenderer::record_copy_mips(VkCommandBuffer command, Texture& target) {
    if (!target.mips_pending) return;
    target.mips_pending=false;
    const auto fragment=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, transfer=VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier(command,target.image,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,fragment|transfer,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT,transfer,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,0);
    barrier(command,target.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,fragment,0,transfer,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,target.mip_levels-1,1);
    for(uint32_t i=1;i<target.mip_levels;++i) {
        if (i>1) barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,transfer,VK_ACCESS_2_TRANSFER_WRITE_BIT,transfer,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,i-1);
        VkImageBlit mip{}; mip.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i-1,0,layers_}; mip.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,layers_};
        mip.srcOffsets[1]={int32_t(std::max(1u,target.extent.width>>(i-1))),int32_t(std::max(1u,target.extent.height>>(i-1))),1};
        mip.dstOffsets[1]={int32_t(std::max(1u,target.extent.width>>i)),int32_t(std::max(1u,target.extent.height>>i)),1};
        vkCmdBlitImage(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&mip,VK_FILTER_LINEAR);
    }
    barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,transfer,VK_ACCESS_2_TRANSFER_READ_BIT,fragment,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,target.mip_levels-1,0);
    barrier(command,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,transfer,VK_ACCESS_2_TRANSFER_WRITE_BIT,fragment,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,target.mip_levels-1);
}
void VulkanRenderer::recycle_copy_texture(Texture& texture) {
    const double factor=std::max(efb_extent_.width/640.0,efb_extent_.height/480.0);
    const VkDeviceSize budget=VkDeviceSize(std::min(double(VkDeviceSize(512)<<20),double(VkDeviceSize(64)<<20)*factor*factor));
    if(recycled_copies_.size()==32 || recycled_copy_bytes_>=budget || texture.allocation_size>budget-recycled_copy_bytes_) { destroy_texture(texture); return; }
    texture.source.reset(); texture.initialized=false;
    try { recycled_copies_.push_back(std::move(texture)); }
    catch(...) { destroy_texture(texture); throw; }
    recycled_copy_bytes_+=recycled_copies_.back().allocation_size;
    texture={};
}
void VulkanRenderer::destroy_texture(Texture& texture) {
    if (texture.view) vkDestroyImageView(device,texture.view,nullptr);
    if (texture.image) vkDestroyImage(device,texture.image,nullptr);
    if (texture.memory) vkFreeMemory(device,texture.memory,nullptr);
    destroy_buffer(texture.upload);
    texture={};
}
void VulkanRenderer::retire_textures(Slot& slot) {
    if (slot.texture_pool) vkDestroyDescriptorPool(device,slot.texture_pool,nullptr);
    slot.texture_pool=VK_NULL_HANDLE; slot.descriptor_capacity=0; slot.texture_sets.clear();
    if (slot.table_pool) vkDestroyDescriptorPool(device,slot.table_pool,nullptr);
    slot.table_pool=VK_NULL_HANDLE; slot.table_set=VK_NULL_HANDLE; slot.table_capacity=0;
    if (slot.merge_pool) vkDestroyDescriptorPool(device,slot.merge_pool,nullptr);
    slot.merge_pool=VK_NULL_HANDLE; slot.merge_set=VK_NULL_HANDLE;
    if (slot.xform_pool) vkDestroyDescriptorPool(device,slot.xform_pool,nullptr);
    slot.xform_pool=VK_NULL_HANDLE; slot.xform_set=VK_NULL_HANDLE;
    slot.textures.clear();   // the last slot to drop a texture destroys it
    slot.copy_sources.clear();
}
// GX destination alpha (pixel[2] bit 8): the EFB's alpha gets a constant instead of the computed alpha. The renderer draws such a draw
// twice: colour with the computed alpha (blending may read it), then the constant alpha alone. Without blending the computed alpha is
// read by nothing but the alpha test (which runs before the replacement in the shader), so the headset draws it once: colour and
// the constant alpha together (key word 67 = destination alpha pass). Half the draws and program switches of the main scene, whose
// draws almost all have destination alpha; the same picture (alpha is also written where a LESS-tested second pass failed before).
bool VulkanRenderer::fused_destination_alpha(const gx::DrawPacket& draw) const {
    if (layers_<2 || !(draw.uniforms->pixel[2]&256) || !draw.blend.alpha_write) return false;
    const auto depth=draw.uniforms->pixel[1]&31;
    if (depth&1 && ((depth>>1)&7)==0) return false;   // depth test NEVER: nothing is drawn
    return draw.blend.mode==0 || (draw.blend.mode==2 && draw.blend.logic==3);   // no blending (logic COPY is a plain write)
}
static bool fuse_off() { static const bool off=std::getenv("UC_NO_FUSED_ALPHA")!=nullptr; return off; }
static bool affine_coordinates_enabled() {
    static const bool on=[] {
        if (const char* value=std::getenv("UC_AFFINE_TEXCOORDS")) return *value && *value!='0';
#ifdef __ANDROID__
        return true;
#else
        return false;
#endif
    }();
    return on;
}
PipelineKey VulkanRenderer::pipeline_key(const gx::DrawPacket& draw,bool alpha_pass) const {
    auto blend=draw.blend;
    const bool fused=!alpha_pass && !fuse_off() && fused_destination_alpha(draw);
    if(alpha_pass) blend={0,1,0,3,false,true};
    else if((draw.uniforms->pixel[2]&256) && !fused) blend.alpha_write=false;
    if (blend.mode>3 || blend.source>7 || blend.destination>7 || blend.logic>15) throw std::invalid_argument("invalid native blend state");
    if (blend.mode==2 && !logic_ops_) {
        // No logicOp on this GPU (MoltenVK: Metal has none). The ops with a blend equivalent are drawn exactly: CLEAR (the only one RBUE08
        // uses, see project/pipeline_keys.seed) = ZERO*src + ZERO*dst, NOOP = ZERO*src + ONE*dst, COPY = no blending. Any other op is drawn
        // as COPY and named once, so an artifact can be traced to it.
        if (blend.logic==0) { blend.mode=1; blend.source=0; blend.destination=0; }
        else if (blend.logic==5) { blend.mode=1; blend.source=0; blend.destination=1; }
        else {
            static std::array<std::atomic<bool>,16> reported{};
            if (blend.logic!=3 && !reported[blend.logic].exchange(true))
                std::fprintf(stderr,"[vulkan] GX logic op %u drawn as a plain write: the GPU has no logicOp\n",unsigned(blend.logic));
            blend.mode=0;
        }
    }
    // Fields the pipeline ignores in this mode must not split the key space (factors only matter when blending, the logic op only in logic mode).
    if (blend.mode!=1) { blend.source=1; blend.destination=0; }
    if (blend.mode!=2) blend.logic=3;
    PipelineKey key{};
    switch(draw.primitive) {
    case 0x80: case 0x90: case 0x98: case 0xA0: key[69]=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 0xA8: case 0xB0: key[69]=VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    default:
        std::fprintf(stderr,"[unsupported-primitive] type %02X callsite %08X vertices %zu indices %zu\n",draw.primitive,draw.callsite,draw.vertex_view().size(),draw.index_view().size());
        throw std::invalid_argument("unsupported native draw topology");
    }
    if(draw.state->cull>3) throw std::invalid_argument("invalid native cull mode");
    key[0]=blend.mode|(blend.source<<2)|(blend.destination<<5)|(blend.logic<<8)|(uint32_t(blend.color_write)<<12)|(uint32_t(blend.alpha_write)<<13);
    key[0]|=uint32_t(draw.state->cull)<<14;
    static const bool alpha_precheck=gx::depth_switch("UC_ALPHA_PRECHECK");   // depth_efb.h: the alpha test evaluated from its compares and references
    key[0]|=uint32_t(gx::alpha_test_needs_discard(draw.uniforms->pixel[0],alpha_precheck))<<16;   // smoke.slang alphaTestActive (specialization constant 67)
    key[0]|=uint32_t(gx::has_texture_matrix_index(*draw.state))<<17;   // as the bank binding (record), not draw.texture_bank (also set unused)   // per-vertex texture matrices: the headset's vertex stage with the bank code (else NO_BANK)
    const auto& material=draw.material;
    if (material.stages>16) throw std::invalid_argument("native TEV stage count exceeds16");
    key[1]=material.stages;
    key[67]=alpha_pass || fused;
    key[68]=draw.uniforms->pixel[1]&31;
    if(alpha_pass) {
        if((key[68]&17)==17) key[68]=5; // compare equal to the first pass's depth
        else key[68]&=~16u;
    }
    for(size_t i=0;i<4;++i) key[2]|=uint32_t(material.swaps[i])<<(i*8);
    // Known indirect modes fold at build time; untrained keys retain a uniform-driven fallback.
    // Quest defaults on. Other backends keep their existing keys unless explicitly enabled.
    static const bool specialize_indirect=[] {
        if (const char* value=std::getenv("UC_SPECIALIZE_INDIRECT")) return *value && *value!='0';
#ifdef __ANDROID__
        return true;
#else
        return false;
#endif
    }();
    static const bool original_refraction=std::getenv("UC_VR_SCREEN_REFRACTION")!=nullptr;
    const bool adapted=layers_>1 && specialize_indirect && !original_refraction && gx::vr_screen_refraction(draw);
    for(size_t i=0;i<material.stages;++i) {
        const auto& stage=material.stage[i];
        key[3+i*4]=stage.color; key[4+i*4]=stage.alpha; key[5+i*4]=stage.order; key[6+i*4]=stage.konst;
        const uint32_t command=gx::effective_indirect_command(*draw.uniforms,uint32_t(i),adapted);
        if(layers_>1 && command==0) key[5+i*4]|=kTevDirectStage;   // the headset: this stage's indirect path compiled out (pipeline_keys.h)
        if(layers_>1 && specialize_indirect)
            specialize_indirect_stage(key,uint32_t(i),command,draw.uniforms->indirect[9]);
    }
    if (layers_>1 && affine_coordinates_enabled() && gx::unit_texture_q(*draw.uniforms,xform_coordinate_mask(key)))
        key=affine_coordinates_key(key);
    return key;
}
VulkanRenderer::PipelineChoice VulkanRenderer::pipeline_for(const PipelineKey& requested) {
    if (last_pipeline_ && requested==last_key_) return {last_pipeline_,false};
    const auto key=static_depth_ ? requested : static_key(requested);   // static_depth_: cull and depth state baked into the pipeline
    const auto fixed=uber_key(key);
    PipelineChoice choice;
    bool fresh=false;
    {
        std::lock_guard lock(pipelines_mutex_);
        if (!uber_only_) {
            if (auto found=pipelines_.find(key); found!=pipelines_.end()) { last_key_=requested; last_pipeline_=found->second; return {found->second,false}; }
            fresh=known_keys_.insert(key).second;
        }
        if (auto found=uber_pipelines_.find(fixed); found!=uber_pipelines_.end()) {
            choice={found->second,true};
            if (!uber_only_) queue_pipeline(key,false);
        }
    }
    if (fresh && !append_pipeline_key(cache_dir_/"pipeline_keys.bin",key)) std::fprintf(stderr,"[pipeline] cannot record key in %s\n",cache_dir_.string().c_str());
    if (choice.pipeline) {
        if (fresh) std::fprintf(stderr,"[pipeline] new key: ubershader until its pipeline is compiled\n");
        return choice;
    }
    // Neither pipeline exists yet: compile synchronously (the ubershader in uber-only mode, else the specialized one) and queue the ubershader for later misses.
    const bool uber=uber_only_;
    const PipelineKey& wanted=uber ? fixed : key;
    const auto start=std::chrono::steady_clock::now();
    auto pipeline=make_pipeline(wanted,uber);
    const double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
    std::lock_guard lock(pipelines_mutex_);
    std::pair<std::map<PipelineKey,VkPipeline>::iterator,bool> placed;
    try { placed=(uber ? uber_pipelines_ : pipelines_).emplace(wanted,pipeline); } catch (...) { vkDestroyPipeline(device,pipeline,nullptr); throw; }
    if (!placed.second) { vkDestroyPipeline(device,pipeline,nullptr); return {placed.first->second,uber}; }   // a compile thread finished it first
    if (!uber) queue_pipeline(fixed,true);
    if (fresh || ms>=2) std::fprintf(stderr,"[pipeline] %s %s compiled on the render thread: %.1f ms\n",fresh ? "new" : "not yet ready",uber ? "ubershader" : "key",ms);
    return {pipeline,uber};
}
// The texture-table variant of a draw's pipeline (key bit kTexTableKeyBit) when it is compiled; otherwise it is queued for the compile threads
// and the draw keeps its own pipeline and set meanwhile (no ubershader for the table: the draw is just not merged across textures).
VkPipeline VulkanRenderer::table_pipeline(const PipelineKey& requested) {
    if (last_table_pipeline_ && requested==last_table_key_) return last_table_pipeline_;
    auto key=static_depth_ ? requested : static_key(requested);
    key[0]|=kTexTableKeyBit;
    std::lock_guard lock(pipelines_mutex_);
    if (const auto found=pipelines_.find(key); found!=pipelines_.end()) { last_table_key_=requested; last_table_pipeline_=found->second; return found->second; }
    queue_pipeline(key,false);
    return VK_NULL_HANDLE;
}
// The small-draw ubershader pipeline of a draw's fixed-function state (uber_merge.h) when it is compiled; otherwise it is queued and the draw
// keeps its own pipeline meanwhile.
// family 1..kUberFewStages: the pipeline of that family of the ubershader (uber_merge.h; null when the draw has more stages or the family is
// off). Runs join on the general pipeline (family 0); a run whose draws all fit a family is drawn with the smallest such family's.
VkPipeline VulkanRenderer::uber_merge_pipeline(const PipelineKey& requested,uint32_t family) {
    if (family && (family>kUberFewStages || !(uber_families_&(1u<<(family-1))) || requested[1]>family)) return VK_NULL_HANDLE;
    if (last_merge_pipeline_[family] && requested==last_merge_key_[family]) return last_merge_pipeline_[family];
    const auto key=uber_merge_key(static_depth_ ? requested : static_key(requested),family);
    std::lock_guard lock(pipelines_mutex_);
    if (const auto found=pipelines_.find(key); found!=pipelines_.end()) { last_merge_key_[family]=requested; last_merge_pipeline_[family]=found->second; return found->second; }
    queue_pipeline(key,false);
    return VK_NULL_HANDLE;
}
uint32_t VulkanRenderer::effective_anisotropy() const { return clamp_anisotropy(effects_.anisotropy,anisotropy_supported_,anisotropy_limit_); }
// One sampler per GX sampler mode (wrap s/t, mipmap mode, mag/min filter); anisotropic filtering applies to the ones that filter linearly across mips.
void VulkanRenderer::create_samplers() {
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter=sampler.minFilter=VK_FILTER_NEAREST;
    sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST; sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    const VkSamplerAddressMode modes[]={VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    const auto anisotropy=effective_anisotropy();
    for(uint32_t mode=0;mode<256;++mode) {
        const auto s=mode&3,t=(mode>>2)&3,mip=(mode>>5)&3;
        if(s>2 || t>2 || mip>2) continue;
        sampler.addressModeU=modes[s]; sampler.addressModeV=modes[t];
        sampler.magFilter=(mode&16) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.minFilter=(mode&128) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sampler.mipmapMode=mip==2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.maxLod=mip ? VK_LOD_CLAMP_NONE : 0.f;
        sampler.anisotropyEnable=anisotropy>1 && (mode&128); sampler.maxAnisotropy=sampler.anisotropyEnable ? float(anisotropy) : 1.f;   // linear minification, with or without a mip filter: most of the game's textures have one level and sampler mode 0x9x
        vk_check(vkCreateSampler(device,&sampler,nullptr,&texture_samplers_[mode]),"texture sampler");
        if(layers_>1 && (mode&128)) {   // generated mips (make_texture auto_mips): linear minification also filters between levels
            auto mips=sampler; mips.mipmapMode=VK_SAMPLER_MIPMAP_MODE_LINEAR; mips.maxLod=VK_LOD_CLAMP_NONE;
            vk_check(vkCreateSampler(device,&mips,nullptr,&auto_samplers_[mode]),"generated-mip texture sampler");
        }
        if(motion_outputs_) {   // scene draws sample as if at output resolution (DLSS/FSR guides: log2(internal/output) - 1)
            auto biased=sampler; biased.mipLodBias=(upscaler_kind_==UpscalerKind::smaa || upscaler_kind_==UpscalerKind::off) ? 0.f : float(-std::log2(upscale_ratio(upscale_mode_))-1.0);   // SMAA and the pass-through: no upscaling, no jitter, no bias
            vk_check(vkCreateSampler(device,&biased,nullptr,&scene_samplers_[mode]),"scene texture sampler");
        }
    }
}
void VulkanRenderer::destroy_samplers() {
    for(auto& sampler:texture_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
    for(auto& sampler:scene_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
    for(auto& sampler:auto_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
}
void VulkanRenderer::set_effects(const GraphicsEffects& effects) {
    owner();
    const bool anisotropy=effects.anisotropy!=effects_.anisotropy, path_before=scene_path_wanted(), rt_before=rt_material_active();
    chosen_effects_=effects;
    effects_=effective_effects(chosen_effects_,ray_tracing_supported());
    apply_scene_path(anisotropy,path_before);
    if (layout_ && rt_material_active()!=rt_before) {   // with an upscaler the scene targets stay: the RT pair comes and goes on its own
        vk_check(present_.wait_idle(device),"RT material targets");
        sync_rt_material_targets();
    }
}
void VulkanRenderer::set_dlss5(const Dlss5Settings& settings) {
    owner();
    const bool path_before=scene_path_wanted();
    dlss5_settings_=settings; dlss5_pass_.settings_changed();
    apply_scene_path(false,path_before);
}
void VulkanRenderer::use_dlss5_backend(std::function<std::unique_ptr<NgxBackend>()> backend, const std::filesystem::path& directory) {
    owner();
    if(!layout_) throw std::logic_error("use_dlss5_backend needs an initialized renderer");
    const bool path_before=scene_path_wanted();
    vk_check(present_.wait_idle(device),"DLSS 5 backend");
    dlss5_pass_.shutdown();
    dlss5_context_.backend=std::move(backend); dlss5_context_.directory=directory;
    dlss5_pass_.configure(dlss5_context_);
    dlss5_device_=motion_outputs_; dlss5_gap_.clear();
    apply_scene_path(false,path_before);
}
// One DLSS 5 run on `picture` at the stage the setting names; false when it does not run now (another stage, off, a black frame, or the pass refused).
bool VulkanRenderer::record_dlss5(VkCommandBuffer command, Dlss5Hook hook, const UpscaleImage& picture, VkOffset2D origin, VkExtent2D size, std::array<float,2> jitter, bool black_output) {
    if(!dlss5_requested() || dlss5_settings_.hook!=hook || !dlss5_scene_.ready || size.width<8 || size.height<8) return false;
    if(dlss5_settings_.block_hudless && black_output) return false;
    const auto timer_slot=uint32_t(next_slot_);
    pass_timer_.begin(command,timer_slot,GpuPass::dlss5);
    Dlss5Frame frame;
    frame.command=command; frame.picture=picture; frame.origin=origin; frame.size=size;
    frame.depth=dlss5_scene_.depth; frame.motion=dlss5_scene_.motion; frame.scene_size=dlss5_scene_.size;
    frame.jitter=jitter; frame.reset=dlss5_scene_.reset; frame.targets_generation=targets_generation_; frame.settings=dlss5_settings_;
    const bool ran=dlss5_pass_.record(frame);
    pass_timer_.end(command,timer_slot,GpuPass::dlss5);
    return ran;
}
void VulkanRenderer::force_scene_targets(bool on) {
    owner();
    const bool path_before=scene_path_wanted();
    force_scene_targets_=on;
    apply_scene_path(false,path_before);
}
// The samplers and the scene targets follow a change of the anisotropy level or of whether the scene-target path is needed (both only once the device exists).
void VulkanRenderer::apply_scene_path(bool anisotropy_changed, bool path_before) {
    const bool path_changed=scene_path_wanted()!=path_before;
    if(!layout_ || (!anisotropy_changed && !path_changed)) return;
    vk_check(present_.wait_idle(device),"effects change");   // through the presentation backend: frame generation drains its presents first
    destroy_samplers(); create_samplers();   // the scene samplers' mip bias depends on the path too
    if(path_changed && upscaler_kind_==UpscalerKind::off) {   // with an upscaler the scene targets exist anyway
        scene_phase_=false; reset_next_=true; scene_restore_copy_.reset();
        resized_=true; rebuild();
    }
}
// The vertex layout of every mesh draw (gx::GpuMeshVertex); the colour pipelines and the shadow pipeline share it.
static void mesh_vertex_input(VkVertexInputBindingDescription& binding, std::array<VkVertexInputAttributeDescription,14>& attributes) {
    binding={0, uint32_t(sizeof(gx::GpuMeshVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
    attributes={{
        {0,0,VK_FORMAT_R32G32B32_SFLOAT,uint32_t(offsetof(gx::GpuMeshVertex,position))},
        {1,0,VK_FORMAT_R8G8B8A8_UNORM,uint32_t(offsetof(gx::GpuMeshVertex,color))},
        {2,0,VK_FORMAT_R32_UINT,uint32_t(offsetof(gx::GpuMeshVertex,row))},
        {3,0,VK_FORMAT_R32G32_UINT,uint32_t(offsetof(gx::GpuMeshVertex,texture_matrix))},
        {14,0,VK_FORMAT_R8G8B8A8_UNORM,uint32_t(offsetof(gx::GpuMeshVertex,color1))},
        {15,0,VK_FORMAT_R32G32B32_SFLOAT,uint32_t(offsetof(gx::GpuMeshVertex,object_normal))}}};
    for(uint32_t i=0;i<8;++i) attributes[6+i]={5+i,0,VK_FORMAT_R32G32_SFLOAT,uint32_t(offsetof(gx::GpuMeshVertex,texcoords)+i*8)};
}
// The shadow map's pipeline: the shadow vertex stage (no lighting) into a depth-only target, no fragment stage, no culling, a small depth bias.
VkPipeline VulkanRenderer::make_shadow_pipeline() {
    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage=VK_SHADER_STAGE_VERTEX_BIT; stage.module=shadow_vertex_module_; stage.pName="vertexMain";
    VkVertexInputBindingDescription binding; std::array<VkVertexInputAttributeDescription,14> attributes;
    mesh_vertex_input(binding,attributes);
    const std::array<VkVertexInputAttributeDescription,2> used{attributes[0],attributes[2]};   // the shadow vertex stage reads the position (0) and the matrix row (2) only
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; input.vertexBindingDescriptionCount=1; input.pVertexBindingDescriptions=&binding; input.vertexAttributeDescriptionCount=uint32_t(used.size()); input.pVertexAttributeDescriptions=used.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=viewport.scissorCount=1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.frontFace=VK_FRONT_FACE_CLOCKWISE; raster.lineWidth=1;
    raster.depthBiasEnable=VK_TRUE; raster.depthBiasConstantFactor=2.f; raster.depthBiasSlopeFactor=2.f;
    VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depth.depthTestEnable=VK_TRUE; depth.depthWriteEnable=VK_TRUE; depth.depthCompareOp=VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamic_states;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.depthAttachmentFormat=VK_FORMAT_D32_SFLOAT;
    VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; create.pNext=&rendering; create.stageCount=1; create.pStages=&stage; create.pVertexInputState=&input;
    create.pInputAssemblyState=&assembly; create.pViewportState=&viewport; create.pRasterizationState=&raster; create.pMultisampleState=&samples; create.pDepthStencilState=&depth;
    create.pColorBlendState=&blending; create.pDynamicState=&dynamic; create.layout=layout_;
    VkPipeline pipeline=VK_NULL_HANDLE;
    vk_check(vkCreateGraphicsPipelines(device,pipeline_cache_,1,&create,nullptr,&pipeline),"shadow pipeline");
    return pipeline;
}
// The build-time specialized TEV fragment module for these constants and variant (1: early-Z; 2, 3: the same with the texture table), or null;
// loaded once, kept until shutdown.
// Names: FNV-1a 64 of the 68 constants (little-endian words) and the variant byte, as tools/fold_tev_shaders.py writes them.
VkShaderModule VulkanRenderer::folded_module(const std::array<uint32_t,68>& values,bool early,bool table) {
    static const bool off=std::getenv("UC_NO_FOLDED_SHADERS")!=nullptr;   // diagnostics: the driver specializes every pipeline
    if (off) return VK_NULL_HANDLE;
    uint64_t hash=0xCBF29CE484222325ull;
    const auto mix=[&](uint8_t byte) { hash^=byte; hash*=0x100000001B3ull; };
    for (const uint32_t value:values) for (int shift=0;shift<32;shift+=8) mix(uint8_t(value>>shift));
    mix(uint8_t((early ? 1 : 0)|(table ? 2 : 0)));   // 2, 3: the texture-table modules (TEX_TABLE)
    std::lock_guard guard(folded_mutex_);
    if (const auto found=folded_modules_.find(hash); found!=folded_modules_.end()) return found->second;
    char file[32]; std::snprintf(file,sizeof file,"%016llx.spv",(unsigned long long)hash);
    VkShaderModule module=VK_NULL_HANDLE;
    // UC_RELAXED_TEV=1: the modules with their colour arithmetic RelaxedPrecision (tools/spirv_relaxed_precision.py, folded_rp)
    static const char* const folded_dir=std::getenv("UC_RELAXED_TEV") ? "folded_rp" : "folded";
    std::ifstream in(shaders_/folded_dir/file,std::ios::binary);
    std::vector<char> code((std::istreambuf_iterator<char>(in)),{});
    if (!code.empty() && code.size()%4==0) {
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize=code.size(); info.pCode=reinterpret_cast<const uint32_t*>(code.data());
        if (vkCreateShaderModule(device,&info,nullptr,&module)!=VK_SUCCESS) module=VK_NULL_HANDLE;
    }
    folded_modules_.emplace(hash,module);   // a miss is remembered too
    static std::atomic<uint32_t> hits{0},misses{0};
    ++(module ? hits : misses);
    if (((hits+misses)&63)==1) std::fprintf(stderr,"[pipeline] folded TEV modules: %u used, %u keys without one\n",hits.load(),misses.load());
    return module;
}
VkPipeline VulkanRenderer::make_pipeline(const PipelineKey& key,bool uber) {
    gx::BlendState mode;
    mode.mode=key[0]&3; mode.source=(key[0]>>2)&7; mode.destination=(key[0]>>5)&7; mode.logic=(key[0]>>8)&15;
    mode.color_write=key[0]>>12&1; mode.alpha_write=key[0]>>13&1;
    // A pipeline whose alpha test cannot fail (key bit 16 clear) gets the early-Z variant: no discard, depth tested before shading.
    static const bool late_z=std::getenv("UC_NO_EARLY_Z")!=nullptr;   // diagnostics: every pipeline on the module with the alpha test
    const VkShaderModule vertex=((key[0]>>17)&1) || !nobank_vertex_module_ ? vertex_module_ : nobank_vertex_module_, fragment=uber ? uber_module_ : (((key[0]>>16)&1) || late_z || !early_module_) ? fragment_module_ : early_module_;
    VkPipeline pipeline=VK_NULL_HANDLE;
    {
        if (!layout_) {
        VkPushConstantRange projection{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof(DrawPush))};
        VkDescriptorSetLayoutBinding texture_bindings[]={{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,8,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_SAMPLER,8,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{2,layers_>1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{3,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{4,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{5,layers_>1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{6,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr},{7,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT,nullptr}};   // 5: the headset's eye block (uniform) or motion blocks; 6, 7: the pre-pass's positions and attributes (xform_)
        VkDescriptorSetLayoutCreateInfo texture_layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; texture_layout.bindingCount=xform_ ? 8 : 6; texture_layout.pBindings=texture_bindings;
        vk_check(vkCreateDescriptorSetLayout(device,&texture_layout,nullptr,&texture_layout_),"texture descriptor layout");
        create_samplers();
        VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout_info.pushConstantRangeCount=1; layout_info.pPushConstantRanges=&projection;
        layout_info.setLayoutCount=1; layout_info.pSetLayouts=&texture_layout_;
        vk_check(vkCreatePipelineLayout(device, &layout_info, nullptr, &layout_), "pipeline layout");
        if (tex_merge_) {   // the texture table: set 0 as above (its maps unused), set 1 one array of combined image samplers, as many as the packet has
            const VkDescriptorSetLayoutBinding table{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,table_limit_,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
            const VkDescriptorBindingFlags flags=VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT|(table_update_after_bind_ ? VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT : 0u);
            VkDescriptorSetLayoutBindingFlagsCreateInfo binding_flags{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO}; binding_flags.bindingCount=1; binding_flags.pBindingFlags=&flags;
            VkDescriptorSetLayoutCreateInfo table_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; table_info.pNext=&binding_flags; table_info.bindingCount=1; table_info.pBindings=&table;
            if (table_update_after_bind_) table_info.flags=VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
            vk_check(vkCreateDescriptorSetLayout(device,&table_info,nullptr,&table_set_layout_),"texture table layout");
            const VkDescriptorSetLayout sets[2]={texture_layout_,table_set_layout_};
            layout_info.setLayoutCount=2; layout_info.pSetLayouts=sets;
            vk_check(vkCreatePipelineLayout(device,&layout_info,nullptr,&table_layout_),"texture table pipeline layout");
            if (uber_small_scene_ || uber_small_hud_) {   // the small-draw ubershader: sets 0 and 1 as the table's, set 2 the packet's uniform blocks as a storage array
                const VkDescriptorSetLayoutBinding blocks{0,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
                VkDescriptorSetLayoutCreateInfo merge_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; merge_info.bindingCount=1; merge_info.pBindings=&blocks;
                vk_check(vkCreateDescriptorSetLayout(device,&merge_info,nullptr,&merge_set_layout_),"small-draw ubershader layout");
                const VkDescriptorSetLayout merge_sets[3]={texture_layout_,table_set_layout_,merge_set_layout_};
                layout_info.setLayoutCount=3; layout_info.pSetLayouts=merge_sets;
                vk_check(vkCreatePipelineLayout(device,&layout_info,nullptr,&merge_layout_),"small-draw ubershader pipeline layout");
            }
        }
        }
        const bool table=(key[0]&kTexTableKeyBit)!=0;   // the texture-table variant (tex_merge_): its own modules and layout
        if (table && (uber || !tex_merge_)) throw std::invalid_argument("texture-table pipeline without the table");
        const bool merge=(key[0]&kUberMergeKeyBit)!=0;   // the small-draw ubershader (uber_merge.h): its own modules and layout, the TEV words read per draw
        if (merge && (!table || !merge_layout_)) throw std::invalid_argument("small-draw ubershader pipeline without its layout");
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT; stages[0].module = vertex; stages[0].pName = "vertexMain";
        const bool early_variant=!uber && fragment==early_module_;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = table ? (early_variant ? table_early_module_ : table_fragment_module_) : fragment; stages[1].pName = "fragmentMain";
        // Constants 0..66: the TEV words (key words 1..67); 67: whether the draws can fail the alpha test (key word 0 bit 16). The ubershader
        // reads the TEV words from the draw, so it gets only constant 67.
        std::array<uint32_t,68> spec_values{};
        std::copy(key.begin()+1,key.begin()+68,spec_values.begin());
        spec_values[67]=(key[0]>>16)&1;
        std::array<VkSpecializationMapEntry,68> spec_entries{};
        for(uint32_t i=0;i<spec_entries.size();++i) spec_entries[i]={i,i*uint32_t(sizeof(uint32_t)),sizeof(uint32_t)};
        const VkSpecializationMapEntry alpha_entry{67,0,sizeof(uint32_t)};
        VkSpecializationInfo specialization{uint32_t(spec_entries.size()),spec_entries.data(),spec_values.size()*sizeof(uint32_t),spec_values.data()};
        VkSpecializationInfo uber_specialization{1,&alpha_entry,sizeof(uint32_t),&spec_values[67]};
        stages[1].pSpecializationInfo=uber ? &uber_specialization : &specialization;
        // The headset: a module specialized at build time for exactly these constants (tools/fold_tev_shaders.py), when there is one. The
        // driver's own specialization left most of the 600 KB module in (a one-stage quad ran 160-200 instructions a fragment).
        if (!uber && !merge && layers_>1 && !probe_fragment_) {
            VkShaderModule folded=folded_module(spec_values,early_variant,table);
            const auto general=general_coordinates_key(key);
            if (!folded && general!=key) {   // an untrained affine variant keeps the general coordinate divide
                auto values=spec_values; std::copy(general.begin()+1,general.begin()+68,values.begin());
                folded=folded_module(values,early_variant,table);
            }
            if (!folded) {   // Untrained indirect modes keep the existing uniform-driven folded shader until their key is built.
                const auto dynamic=dynamic_indirect_key(general);
                auto values=spec_values;
                std::copy(dynamic.begin()+1,dynamic.begin()+68,values.begin());
                if (values!=spec_values) folded=folded_module(values,early_variant,table);
            }
            if (!folded) {   // a key not listed for the build (pipeline_keys.quest): the module of the same key with every stage's indirect path in
                auto all_indirect=spec_values;
                const auto dynamic=dynamic_indirect_key(general);
                std::copy(dynamic.begin()+1,dynamic.begin()+68,all_indirect.begin());
                for (uint32_t stage=0;stage<std::min<uint32_t>(key[1],16);++stage) all_indirect[4+4*stage]&=~kTevDirectStage;
                if (all_indirect!=spec_values) folded=folded_module(all_indirect,early_variant,table);
            }
            if (folded) { stages[1].module=folded; stages[1].pSpecializationInfo=nullptr; }
        }
        static_assert(sizeof(gx::GpuMeshVertex)==108 && sizeof(gx::Projection)==64);
        VkVertexInputBindingDescription binding; std::array<VkVertexInputAttributeDescription,14> attributes;
        mesh_vertex_input(binding,attributes);
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO}; input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding; input.vertexAttributeDescriptionCount = uint32_t(attributes.size()); input.pVertexAttributeDescriptions = attributes.data();
        if (xform_) { input.vertexBindingDescriptionCount = 0; input.vertexAttributeDescriptionCount = 0; stages[0].module = table ? table_vertex_module_ : xform_vertex_module_; }   // the pre-pass's draws read their vertices from its outputs
        static const bool dynamic_coords=std::getenv("UC_DYNAMIC_XFORM_COORDS")!=nullptr;
        const uint32_t coordinate_mask=dynamic_coords || uber ? 255u : xform_coordinate_mask(key);
        const VkSpecializationMapEntry coordinate_entry{68,0,sizeof(uint32_t)};
        const VkSpecializationInfo coordinate_spec{1,&coordinate_entry,sizeof coordinate_mask,&coordinate_mask};
        if (xform_ && !merge) stages[0].pSpecializationInfo=&coordinate_spec;
        if (merge) {   // the ubershader with and without the alpha test (key word 0 bit 16), constant 67 only
            const uint32_t family=key[1];   // a family of draws with few stages (uber_merge.h), 0: the general module
            if (family>kUberFewStages || (family && !few_vertex_module_[family-1])) throw std::invalid_argument("small-draw ubershader family without its modules");
            stages[0].module=family ? few_vertex_module_[family-1] : merge_vertex_module_;
            stages[1].module=(((key[0]>>16)&1) || late_z) ? (family ? few_fragment_module_[family-1] : merge_fragment_module_) : (family ? few_early_module_[family-1] : merge_early_module_);
            stages[1].pSpecializationInfo=&uber_specialization;
        }
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology = VkPrimitiveTopology(key[69]);
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.polygonMode = VK_POLYGON_MODE_FILL; raster.cullMode = cull_mode(key); raster.frontFace = VK_FRONT_FACE_CLOCKWISE; raster.lineWidth = 1; raster.depthClampEnable = depth_clamp_;
        VkPipelineMultisampleStateCreateInfo samples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; samples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask=(mode.color_write ? VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT : 0)|(mode.alpha_write ? VK_COLOR_COMPONENT_A_BIT : 0);
        const VkBlendFactor sources[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_DST_COLOR,VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        const VkBlendFactor destinations[]={VK_BLEND_FACTOR_ZERO,VK_BLEND_FACTOR_ONE,VK_BLEND_FACTOR_SRC_COLOR,VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,VK_BLEND_FACTOR_SRC_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,VK_BLEND_FACTOR_DST_ALPHA,VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA};
        blend.blendEnable=mode.mode==1 || mode.mode==3;
        blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=mode.mode==3 ? VK_BLEND_FACTOR_ONE : sources[mode.source];
        blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=mode.mode==3 ? VK_BLEND_FACTOR_ONE : destinations[mode.destination];
        blend.colorBlendOp=blend.alphaBlendOp=mode.mode==3 ? VK_BLEND_OP_REVERSE_SUBTRACT : VK_BLEND_OP_ADD;
        VkPipelineColorBlendAttachmentState blends[5]{blend,{},{},{},{}};
        blends[1].colorWriteMask=motion_write_mask(key);   // motion vectors: replaced, never blended
        blends[2].colorWriteMask=gbuffer_write_mask(key);  // G-buffer: replaced, never blended
        const bool rt_writes=rt_material_write(key);
        blends[3].colorWriteMask=rt_writes ? VK_COLOR_COMPONENT_R_BIT : 0;   // material record index
        blends[4].colorWriteMask=rt_writes ? gbuffer_write_mask(key) : 0;    // albedo
        if ((key[0]&kParticleLayerKeyBit) && mode.mode==1) {   // the particle layer: colour as the draw blends it, alpha the scene's remaining share (T' = D * T)
            blends[0].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            blends[0].srcAlphaBlendFactor=VK_BLEND_FACTOR_ZERO; blends[0].dstAlphaBlendFactor=destinations[mode.destination];
        }
        const bool hud_layer=(key[0]&kHudLayerKeyBit)!=0;   // the HUD layer (hud_layer.h): L takes the draw's blend, T the factor the scene behind keeps
        if (hud_layer) {
            blends[0].colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT;
            blends[1]=VkPipelineColorBlendAttachmentState{};
            blends[1].blendEnable=VK_TRUE; blends[1].colorWriteMask=blends[0].colorWriteMask;
            blends[1].srcColorBlendFactor=blends[1].srcAlphaBlendFactor=mode.mode==1 && mode.source==kGxSrcDstColor ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_ZERO;
            blends[1].dstColorBlendFactor=blends[1].dstAlphaBlendFactor=mode.mode==1 ? destinations[mode.destination] : VK_BLEND_FACTOR_ZERO;
            blends[1].colorBlendOp=blends[1].alphaBlendOp=VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blending.attachmentCount = hud_layer ? 2 : scene_color_attachments(); blending.pAttachments = blends;
        blending.logicOpEnable=mode.mode==2; blending.logicOp=VkLogicOp(mode.logic);
        VkDynamicState dynamic_states[7]; uint32_t dynamic_count=0;
        dynamic_states[dynamic_count++]=VK_DYNAMIC_STATE_VIEWPORT; dynamic_states[dynamic_count++]=VK_DYNAMIC_STATE_SCISSOR;
        if (!static_depth_) for (const VkDynamicState d:{VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP}) dynamic_states[dynamic_count++]=d;   // set_draw_state
        if (set_shading_rate_) dynamic_states[dynamic_count++]=VK_DYNAMIC_STATE_FRAGMENT_SHADING_RATE_KHR;   // the rate: record_pass sets it per draw
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = dynamic_count; dynamic.pDynamicStates = dynamic_states;
        const VkFormat formats[5] = {VK_FORMAT_R8G8B8A8_UNORM, kMotionFormat, kGBufferFormat, kMaterialFormat, kAlbedoFormat};
        const VkFormat hud_formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = scene_color_attachments(); rendering.pColorAttachmentFormats = formats; rendering.viewMask = layers_>1 ? 0b11u : 0u;
        rendering.depthAttachmentFormat=depth_format_;
        if (hud_layer) { rendering.colorAttachmentCount=2; rendering.pColorAttachmentFormats=hud_formats; rendering.depthAttachmentFormat=VK_FORMAT_UNDEFINED; }
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        depth.depthTestEnable=bool(key[68]&1) && !hud_layer; depth.depthWriteEnable=bool(key[68]&16) && !hud_layer;
        depth.depthCompareOp=VkCompareOp((key[68]>>1)&7);
        VkGraphicsPipelineCreateInfo create{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; create.pNext = &rendering; create.stageCount = 2; create.pStages = stages; create.pVertexInputState = &input;
        create.pInputAssemblyState = &assembly; create.pViewportState = &viewport; create.pRasterizationState = &raster; create.pMultisampleState = &samples;
        create.pColorBlendState = &blending; create.pDynamicState = &dynamic; create.layout = merge ? merge_layout_ : table ? table_layout_ : layout_;
        create.pDepthStencilState=&depth;
        if (fovea_supported_) create.flags|=VK_PIPELINE_CREATE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR;   // EFB passes may bind the foveation map
        vk_check(vkCreateGraphicsPipelines(device, pipeline_cache_, 1, &create, nullptr, &pipeline), "graphics pipeline");
    }
    return pipeline;
}
// The driver cache makes recompiling a known pipeline cheap on later runs; the key files let a loading screen compile everything before the game starts.
void VulkanRenderer::load_pipeline_cache() {
    std::vector<char> data;
    if (std::ifstream file(cache_dir_/"pipeline_cache.bin",std::ios::binary|std::ios::ate); file) {
        data.resize(size_t(file.tellg())); file.seekg(0); file.read(data.data(),std::streamsize(data.size()));
        if (!file) data.clear();
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO}; info.initialDataSize=data.size(); info.pInitialData=data.data();
    vk_check(vkCreatePipelineCache(device,&info,nullptr,&pipeline_cache_),"pipeline cache");   // the driver ignores data from another driver or version
    pipeline_cache_loaded_=data.size();
    std::fprintf(stderr,"[pipeline] disk cache bytes=%zu\n",data.size());
    for (const char* name:{"pipeline_keys.seed","pipeline_keys.quest","pipeline_keys.bin"})   // .quest: the headset's own keys (stereo builds ship it)
        for (const auto& recorded:load_pipeline_keys(cache_dir_/name))
            if (auto key=logic_ops_ ? static_key(recorded) : without_logic_op(static_key(recorded)); valid_pipeline_key(key,logic_ops_)) {
                if (layers_>1) key=headset_direct_key(key);   // a key recorded before the headset marked its direct stages
                key=general_coordinates_key(key);
                if (known_keys_.insert(key).second) warm_keys_.push_back(key);
                if (layers_>1 && affine_coordinates_enabled()) {
                    const auto affine=affine_coordinates_key(key);
                    if (known_keys_.insert(affine).second) warm_keys_.push_back(affine);
                }
            }
    std::fprintf(stderr,"[pipeline] known pipeline keys=%zu\n",warm_keys_.size());
}
void VulkanRenderer::save_pipeline_cache() {
    static std::mutex saving;   // compile threads and shutdown may save concurrently
    std::lock_guard guard(saving);
    size_t size=0;
    if (vkGetPipelineCacheData(device,pipeline_cache_,&size,nullptr)!=VK_SUCCESS || size==0 || size==pipeline_cache_loaded_) return;
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(device,pipeline_cache_,&size,data.data())!=VK_SUCCESS) return;
    const auto path=cache_dir_/"pipeline_cache.bin", temporary=cache_dir_/"pipeline_cache.tmp";
    if (std::ofstream file(temporary,std::ios::binary|std::ios::trunc); !file.write(data.data(),std::streamsize(size))) { std::fprintf(stderr,"[pipeline] cannot write %s\n",temporary.string().c_str()); return; }
    std::error_code error; std::filesystem::rename(temporary,path,error);   // atomic replace: a crash never leaves a torn cache
    if (error) std::fprintf(stderr,"[pipeline] cannot replace %s: %s\n",path.string().c_str(),error.message().c_str());
    else pipeline_cache_loaded_=size;
}
// Pipelines missing at draw time are compiled by these threads, off the render thread (the ubershader draws meanwhile).
void VulkanRenderer::queue_pipeline(const PipelineKey& key,bool uber) {
    if ((uber ? uber_queued_ : queued_).insert(key).second) { compile_queue_.push_back({key,uber}); compile_cv_.notify_one(); }
}
void VulkanRenderer::start_workers() {
    const unsigned all=std::max(2u,std::thread::hardware_concurrency());
    game_workers_=compile_limit_=std::clamp(all/2,2u,8u);
    for (unsigned t=0;t<all;++t) compilers_.emplace_back([this] { compile_worker(); });   // the extra threads only work during warm-up
}
void VulkanRenderer::compile_worker() {
    SDL_SetCurrentThreadPriority(SDL_THREAD_PRIORITY_LOW);   // in game these never compete with the guest and render threads; on the loading screen nothing else runs
    std::unique_lock lock(pipelines_mutex_);
    for (;;) {
        compile_cv_.wait(lock,[this] { return compile_stop_ || (!compile_queue_.empty() && compiling_<compile_limit_); });
        if (compile_stop_) return;
        const CompileJob job=compile_queue_.front(); compile_queue_.pop_front();
        auto& target=job.uber ? uber_pipelines_ : pipelines_;
        if (target.count(job.key)) { ++jobs_done_; continue; }
        ++compiling_;
        lock.unlock();
        VkPipeline pipeline=VK_NULL_HANDLE;
        const auto start=std::chrono::steady_clock::now();
        try { pipeline=make_pipeline(job.key,job.uber); }
        catch (const std::exception& error) { std::fprintf(stderr,"[pipeline] background compile failed: %s\n",error.what()); }
        lock.lock();
        --compiling_; ++jobs_done_;
        if (pipeline) {
            ++compiled_; (job.uber ? compiled_uber_ms_ : compiled_ms_)+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count(); compiled_ubers_+=job.uber;
            if (!target.emplace(job.key,pipeline).second) vkDestroyPipeline(device,pipeline,nullptr);
        }
        if (compile_queue_.empty() && !compiling_ && compiled_) {
            const size_t done=std::exchange(compiled_,0), ubers=std::exchange(compiled_ubers_,0);
            const double ms=std::exchange(compiled_ms_,0.0), uber_ms=std::exchange(compiled_uber_ms_,0.0);
            lock.unlock();
            save_pipeline_cache();
            std::fprintf(stderr,"[pipeline] background compile idle: %zu pipelines ready (%zu ubershaders: %.0f ms, others: %.0f ms)\n",done,ubers,uber_ms,ms);
            lock.lock();
        }
    }
}
void VulkanRenderer::warm_pipelines(const std::function<void(size_t,size_t)>& progress) {
    owner();
    const auto begin=std::chrono::steady_clock::now();
    std::unique_lock lock(pipelines_mutex_);
    compile_limit_=compilers_.size(); compile_cv_.notify_all();   // nothing else runs on the loading screen: every thread compiles
    for (const auto& key:uber_key_closure(warm_keys_)) queue_pipeline(key,true);   // the ubershader variants first: they cover any key still missing
    for (const auto& key:warm_keys_) queue_pipeline(key,false);
    if (tex_merge_)   // the texture-table variants of the EFB keys too (the HUD and particle layers draw without the table)
        for (auto key:warm_keys_) if (!(key[0]&(kHudLayerKeyBit|kParticleLayerKeyBit))) { key[0]|=kTexTableKeyBit; queue_pipeline(key,false); }
    if (uber_small_scene_ || uber_small_hud_)   // the small-draw ubershader: one pipeline per fixed-function state seen (EFB and HUD layer keys)
        for (auto key:warm_keys_) if (!(key[0]&kParticleLayerKeyBit)) {   // the general pipeline of each state and its families of few stages
            queue_pipeline(uber_merge_key(key),false);
            key[1]=1;
            for (uint32_t family=1;family<=kUberFewStages;++family) if (uber_families_&(1u<<(family-1))) queue_pipeline(uber_merge_key(key,family),false);
        }
    const size_t base=jobs_done_, total=compile_queue_.size()+compiling_;
    while (!compile_queue_.empty() || compiling_) {
        const size_t done=jobs_done_-base;
        lock.unlock();
        progress(done,total);
        lock.lock();
        compile_cv_.wait_for(lock,std::chrono::milliseconds(50),[this] { return compile_queue_.empty() && !compiling_; });
    }
    compile_limit_=game_workers_;
    lock.unlock();
    progress(total,total);
    std::fprintf(stderr,"[pipeline] warm-up: %zu pipelines in %.0f ms\n",total,std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
}
void VulkanRenderer::stop_workers() {
    { std::lock_guard lock(pipelines_mutex_); compile_stop_=true; }
    compile_cv_.notify_all();
    for (auto& thread:compilers_) thread.join();
    compilers_.clear();
}
// Persistent vertex/index buffers of a cached mesh. An entry whose owner expired is stale (the guest freed the mesh, its address may be reused):
// no submission can still hold it, because every slot keeps a strong reference until its fence completed.
VulkanRenderer::GpuMesh& VulkanRenderer::gpu_mesh(const std::shared_ptr<const gx::MeshBuffers>& source) {
    auto found=meshes_.find(source.get());
    if (found!=meshes_.end()) {
        if (!found->second.owner.expired()) return found->second;
        release_mesh(found->second);
        meshes_.erase(found);
    }
    const auto& vertices=source->vertices; const auto& indices=source->indices;
    for (uint32_t index : indices) if (index >= vertices.size()) throw std::invalid_argument("draw index exceeds owned vertices");
    if (vertices.empty() || indices.empty() || vertices.size() > size_t(INT32_MAX) || indices.size() > size_t(UINT32_MAX)) throw std::invalid_argument("invalid cached mesh");
    const auto created_at=std::chrono::steady_clock::now();
    const VkDeviceSize vertex_bytes=VkDeviceSize(vertices.size())*sizeof(gx::GpuMeshVertex), index_bytes=VkDeviceSize(indices.size())*sizeof(uint32_t);
    const VkDeviceSize total=(vertex_bytes+15)/16*16+(index_bytes+15)/16*16;
    if (mesh_slabs_.empty() || mesh_slabs_.back()->buffer.size-mesh_slabs_.back()->used<total) {
        auto slab=std::make_unique<MeshSlab>();
        slab->buffer=make_buffer(std::max<VkDeviceSize>(VkDeviceSize(32)<<20,total),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|VK_BUFFER_USAGE_INDEX_BUFFER_BIT|vertex_usage(),true);
        mesh_slabs_.push_back(std::move(slab));
    }
    MeshSlab& slab=*mesh_slabs_.back();
    GpuMesh mesh;
    mesh.slab=&slab; mesh.vertex_offset=slab.used; mesh.index_offset=slab.used+(vertex_bytes+15)/16*16;
    auto* target=reinterpret_cast<gx::GpuMeshVertex*>(static_cast<std::byte*>(slab.buffer.mapped)+mesh.vertex_offset);
    for (size_t i=0;i<vertices.size();++i) target[i]=gx::mesh_vertex(vertices[i]);
    std::memcpy(static_cast<std::byte*>(slab.buffer.mapped)+mesh.index_offset,indices.data(),index_bytes);
    slab.used+=total; ++slab.live;
    mesh.index_count=uint32_t(indices.size()); mesh.rows=gx::used_rows(vertices); mesh.owner=source; mesh.id=next_mesh_id_++;
    mesh.box={1e30f,1e30f,1e30f,-1e30f,-1e30f,-1e30f};
    for (const auto& vertex:vertices) for (int i=0;i<3;++i) { mesh.box[i]=std::min(mesh.box[i],vertex.position[i]); mesh.box[3+i]=std::max(mesh.box[3+i],vertex.position[i]); }
    if (const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-created_at).count(); ms>2) std::fprintf(stderr,"[gpu-mesh] %zu vertices %zu indices uploaded in %.2f ms (%zu meshes resident)\n",vertices.size(),indices.size(),ms,meshes_.size()+1);
    return meshes_.emplace(source.get(),std::move(mesh)).first->second;
}
// A slab other than the one being filled is freed with its last mesh (every mesh in it expired, so no submission can still read it).
void VulkanRenderer::release_mesh(GpuMesh& mesh) {
    if (rt_.initialized()) rt_.invalidate_mesh(mesh.id);   // its cached BLAS goes with it (the id is never reused, but the memory should)
    MeshSlab* slab=mesh.slab; mesh.slab=nullptr;
    if (!slab || --slab->live) return;
    if (mesh_slabs_.back().get()==slab) { slab->used=0; return; }   // empty current slab: reuse it from the start
    destroy_buffer(slab->buffer);
    std::erase_if(mesh_slabs_,[slab](const auto& held){ return held.get()==slab; });
}
void VulkanRenderer::sweep_meshes(bool all) {
    for (auto it=meshes_.begin(); it!=meshes_.end();) {
        if (all || it->second.owner.expired()) { release_mesh(it->second); it=meshes_.erase(it); }
        else ++it;
    }
    if (all) { for (auto& slab : mesh_slabs_) destroy_buffer(slab->buffer); mesh_slabs_.clear(); }
}
void VulkanRenderer::collect() {
    // Oldest submission first (the ring is filled in order from next_slot_), stopping at the first unfinished fence: one queue completes its
    // submissions in order, and a fence status is a ~10 us kernel call on KosmicKrisp (polling every slot cost ~4 ms of render thread per frame, S16).
    for (size_t k = 0; k < slots_.size(); ++k) {
        auto& slot = slots_[(next_slot_ + k) % slots_.size()];
        if (!slot.submitted || slot.reported) continue;
        VkResult status = vkGetFenceStatus(device, slot.fence);
        if (status == VK_NOT_READY) break;
        vk_check(status, "frame fence status");
        completed_.push_back(slot.serial); slot.reported = true;
        uint64_t stamps[2];
        if (slot.timed && vkGetQueryPoolResults(device, timing_pool_, uint32_t((&slot - slots_.data()) * 2), 2, sizeof stamps, stamps, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && stamps[1] >= stamps[0])
        { const uint64_t ns = uint64_t(double(stamps[1] - stamps[0]) * timestamp_period_); gpu_busy_ns_ += ns; if (ns > gpu_window_max_ns_) gpu_window_max_ns_ = ns;
          static const bool by_kind = std::getenv("UC_GPU_PACKETS") != nullptr;   // diagnostics: GPU time per second by packet kind
          if (by_kind) {
              static std::map<std::string, std::pair<uint64_t, uint64_t>> kinds; static auto since = std::chrono::steady_clock::now();
              auto& k = kinds[slot.timing_kind]; ++k.first; k.second += ns;
              if (std::chrono::steady_clock::now() - since > std::chrono::seconds(2)) {
                  std::vector<std::pair<std::string, std::pair<uint64_t, uint64_t>>> sorted(kinds.begin(), kinds.end());
                  std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second.second > b.second.second; });
                  uint64_t total = 0; for (const auto& [name, v] : sorted) total += v.second;
                  std::fprintf(stderr, "[gpu-packets] 2 s: %.1f ms GPU\n", total / 1e6);
                  for (size_t i = 0; i < sorted.size() && i < 14; ++i)
                      std::fprintf(stderr, "[gpu-packets] %8.2f ms %5llu x  avg %.3f ms  %s\n", sorted[i].second.second / 1e6, (unsigned long long)sorted[i].second.first,
                                   sorted[i].second.second / 1e6 / double(sorted[i].second.first), sorted[i].first.c_str());
                  kinds.clear(); since = std::chrono::steady_clock::now();
              }
          } }
        if (slot.timed) pass_timer_.collect(uint32_t(&slot - slots_.data()));
        slot.timed = false;
    }
}
std::vector<uint64_t> VulkanRenderer::poll_completions() {
    owner(); collect();
    std::sort(completed_.begin(), completed_.end());
    auto completed = std::move(completed_); completed_.clear(); return completed;
}
RenderPixels VulkanRenderer::capture(uint64_t serial, bool efb) {
    owner();
    if(efb && serial!=last_serial_) throw std::invalid_argument("EFB capture requires latest submitted serial");
    for (auto& slot : slots_) {
        if (slot.submitted && slot.serial == serial && (efb || slot.copied)) {
            vk_check(vkGetFenceStatus(device, slot.fence), "capture requires completed frame");
            collect();
            const auto eye_size=efb ? efb_extent_ : extent_;
            const uint32_t eyes=layers_;   // stereo: both eyes side by side, the left eye's layer first
            const VkExtent2D size{eye_size.width*eyes,eye_size.height};
            const VkDeviceSize bytes=VkDeviceSize(size.width)*size.height*4;
            if(slot.pixels.size!=bytes) { destroy_buffer(slot.pixels); slot.pixels=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT); }
            vk_check(vkResetCommandBuffer(slot.command, 0), "reset capture command");
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vk_check(vkBeginCommandBuffer(slot.command, &begin), "begin capture");
            VkBufferImageCopy copies[2]{};
            for (uint32_t eye=0; eye<eyes; ++eye) {
                auto& copy=copies[eye]; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, eye, 1}; copy.imageExtent = {eye_size.width, eye_size.height, 1};
                copy.bufferOffset=VkDeviceSize(eye_size.width)*eye*4; copy.bufferRowLength=size.width;
            }
            if (efb) record_efb_clear(slot.command,"capture");   // UC_FOLD_CLEAR: the EFB as the packet left it, its clear included
            vkCmdCopyImageToBuffer(slot.command, efb ? efb_ : slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.pixels.buffer, eyes, copies);
            VkBufferMemoryBarrier2 host_barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2}; host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT; host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT; host_barrier.srcQueueFamilyIndex = host_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            host_barrier.buffer = slot.pixels.buffer; host_barrier.size = VK_WHOLE_SIZE;
            VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; host_dependency.bufferMemoryBarrierCount = 1; host_dependency.pBufferMemoryBarriers = &host_barrier; vkCmdPipelineBarrier2(slot.command, &host_dependency);
            vk_check(vkEndCommandBuffer(slot.command), "end capture");
            vk_check(vkResetFences(device, 1, &slot.fence), "reset capture fence");
            VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = slot.command;
            VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount = 1; submit.pCommandBufferInfos = &command;
            vk_check(vkQueueSubmit2(queue_, 1, &submit, slot.fence), "submit capture");
            vk_check(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 1000000000ull), "capture fence");
            const auto* data = static_cast<const uint8_t*>(slot.pixels.mapped);
            return {serial, size.width, size.height, {data, data + size_t(bytes)}};
        }
    }
    throw std::runtime_error("capture serial no longer retained: " + std::to_string(serial));
}
bool VulkanRenderer::wait_for_pending(uint64_t timeout_ns) {
    owner();
    const Slot* oldest=nullptr;
    for(const auto& slot:slots_) if(slot.submitted && !slot.reported && (!oldest || slot.serial<oldest->serial)) oldest=&slot;
    if(!oldest) return false;
    const auto result=vkWaitForFences(device,1,&oldest->fence,VK_TRUE,timeout_ns);
    if(result!=VK_TIMEOUT) vk_check(result,"wait pending native frame");
    return true;
}
void VulkanRenderer::resize(uint32_t width, uint32_t height) {
    owner(); requested_ = {width, height}; resized_ = true;
}
void VulkanRenderer::set_render_scale(double scale) {
    owner();
    manual_render_extent(scale,render_aspect_);
    if(configured_scale_!=scale) { configured_scale_=scale; resized_=true; }
}
void VulkanRenderer::configure_upscaling(UpscalerKind kind, UpscaleMode mode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier) {
    if (instance_) throw std::logic_error("upscaling is configured before initialize()");
    upscaler_kind_=kind; upscale_mode_=mode; sharpening_=sharpening; sharpness_=sharpness;
    frame_generation_=frame_generation && (kind==UpscalerKind::dlss || kind==UpscalerKind::fsr); frame_generation_multiplier_=multiplier;
}
bool VulkanRenderer::set_upscaling(UpscalerKind kind, UpscaleMode mode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier) {
    owner();
    if (!motion_outputs_ || (kind==UpscalerKind::dlss && !dlss_available_)) kind=UpscalerKind::off;
    frame_generation=frame_generation && ((kind==UpscalerKind::dlss && dlss_g_available_) || (kind==UpscalerKind::fsr && bool(fsr_g_)));
    sharpening_=sharpening; sharpness_=sharpness;
    if (motion_outputs_ && sharpening_) rcas_.initialize(device,memory_,shaders_);
    if (kind==upscaler_kind_ && mode==upscale_mode_ && frame_generation==frame_generation_ && multiplier==frame_generation_multiplier_) return true;
    vk_check(present_.wait_idle(device), "upscale mode switch");
    if (dlss_g_) dlss_g_->set_active(false,true);
    if (fsr_g_ && fsr_g_->has_context()) fsr_g_->begin_frame(false,{},frame_index_);
    const bool reconstruction_changed=kind!=upscaler_kind_ || mode!=upscale_mode_;
    const bool fsr_presentation=kind==UpscalerKind::fsr && (frame_generation || fsr_presentation_);
    const bool dlss_presentation=kind==UpscalerKind::dlss && (frame_generation || dlss_presentation_);
    const bool backend_changed=fsr_presentation!=fsr_presentation_ || dlss_presentation!=dlss_presentation_;
    if (backend_changed) {
        if (fsr_g_) fsr_g_->release_context();
        retire_images(true);   // every old chain is destroyed through its owning backend before routing changes
        fsr_presentation_=fsr_presentation;
        dlss_presentation_=dlss_presentation;
        present_=fsr_presentation_ ? fsr_g_->present_functions() : dlss_presentation_ ? base_present_ : PresentFunctions{};
        if (!fsr_presentation_ && streamline_ && streamline_->active()) present_.present=base_present_.present;
    }
    if (reconstruction_changed) upscaler_.reset();
    if (multiplier!=frame_generation_multiplier_) { dlss_g_.reset(); fg_waited_value_=0; }
    upscaler_kind_=kind; upscale_mode_=mode; frame_generation_=frame_generation; frame_generation_multiplier_=multiplier;
    scene_phase_=false; reset_next_=true; scene_restore_copy_.reset();
    if (reconstruction_changed && layout_) { destroy_samplers(); create_samplers(); }   // the scene samplers' mip bias follows the reconstruction (the device is idle here)
    if (reconstruction_changed || backend_changed) { resized_=true; rebuild(); }
    else configure_generation();
    return true;
}
std::vector<const gx::TexturePixels*> VulkanRenderer::copy_keys() const {
    std::vector<const gx::TexturePixels*> keys;
    for (const auto& [key,texture] : copy_textures_) keys.push_back(key);
    for(const auto& [key,alias]:copy_aliases_)if(!alias.requested.expired())keys.push_back(key);
    keys.insert(keys.end(), lost_copies_.begin(), lost_copies_.end());
    return keys;
}
void VulkanRenderer::lost_copies(std::vector<const gx::TexturePixels*> keys) {
    lost_copies_.insert(keys.begin(), keys.end());
    if (!blank_) { auto blank=std::make_shared<gx::TexturePixels>(); blank->levels.push_back({1,1,{0,0,0,0}}); blank_=std::move(blank); }
}
void VulkanRenderer::set_menu(std::function<void()> build, std::function<void()> setup) {
    owner();
    menu_build_=std::move(build); menu_setup_=std::move(setup);
}
void VulkanRenderer::set_bloom(const BloomSettings& settings) {
    owner();
    const bool path_before=scene_path_wanted();
    bloom_=settings;
    apply_scene_path(false,path_before);   // the glow runs on the scene targets, which a pass-through upscaler keeps only while an effect needs them
}
BloomLayer VulkanRenderer::bloom_layer(VkOffset2D origin, VkExtent2D size) const {
    BloomLayer layer;
    if (!hdr_active_ || !bloom_pass_.glow_valid(0) || !bloom_region_.width || !bloom_region_.height) return layer;
    layer.view=bloom_pass_.glow_view(0);
    layer.uv_origin[0]=float(origin.x)/bloom_region_.width; layer.uv_origin[1]=float(origin.y)/bloom_region_.height;
    layer.uv_size[0]=float(size.width)/bloom_region_.width; layer.uv_size[1]=float(size.height)/bloom_region_.height;
    return layer;
}
void VulkanRenderer::set_crosshair_scale(uint32_t percent) {
    owner();
    crosshair_scale_=std::min(percent,100u)/100.f;
}
void VulkanRenderer::set_hdr(const HdrSettings& settings) {
    if (instance_) owner();
    if (!valid_hdr_settings(settings)) throw std::invalid_argument("invalid HDR settings");
    if (settings.enabled!=hdr_settings_.enabled) resized_=true;   // a new swapchain format; the brightness sliders apply from the next frame
    hdr_settings_=settings;
}
void VulkanRenderer::set_prompt_style(PromptStyle style) {
    owner();
    if(style!=PromptStyle::monochrome && style!=PromptStyle::sculpted)throw std::invalid_argument("invalid native prompt style");
    prompt_style_=style;
}
// Resize path: waits for the slots' GPU work only (no device idle, no presentation fences) and keeps buffers, textures and the EFB.
// The old swapchain becomes the new one's oldSwapchain and is freed by reap_retired() when its presentations are done.
void VulkanRenderer::retire_for_resize() {
    if (!device || !swapchain_) return;
    for (auto& slot : slots_)
        if (slot.submitted) vk_check(vkWaitForFences(device, 1, &slot.fence, VK_TRUE, 1000000000ull), "resize frame fence");
    collect();
    old_swapchain_ = swapchain_;
    retired_.push_back({swapchain_, std::move(images_)});
    images_.clear(); swapchain_ = VK_NULL_HANDLE;
    hdr_pass_.forget();   // its sets name the slot images destroyed below; every slot's fence was waited for above
    for (auto& slot : slots_) {   // the output-sized images are the only per-slot resources that depend on the window
        destroy_slot_images(slot);
        slot.initialized = slot.copied = false;
    }
    destroy_held_frame();
}
void VulkanRenderer::destroy_slot_images(Slot& slot) {
    for (auto [view,image,memory] : {std::tuple{&slot.view,&slot.image,&slot.memory},std::tuple{&slot.ui_view,&slot.ui_image,&slot.ui_memory}}) {
        if (*view) vkDestroyImageView(device, *view, nullptr);
        if (*image) vkDestroyImage(device, *image, nullptr);
        if (*memory) vkFreeMemory(device, *memory, nullptr);
        *view = VK_NULL_HANDLE; *image = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE;
    }
}
void VulkanRenderer::reap_retired(bool wait) {
    if(!present_fences_ && !wait)return;   // legacy chains wait for a successor image's reacquisition
    for (auto it = retired_.begin(); it != retired_.end();) {
        bool done = wait || it->chain != old_swapchain_;   // a chain still waiting for its successor stays
        for (const auto& image : it->images) {
            if (!done || !image.pending || !present_fences_) continue;
            const VkResult status = wait ? vkWaitForFences(device, 1, &image.fence, VK_TRUE, 1000000000ull) : vkGetFenceStatus(device, image.fence);
            if (status == VK_NOT_READY || status == VK_TIMEOUT) done = false;
            else if (status != VK_ERROR_DEVICE_LOST) vk_check(status, "retired presentation fence");
        }
        if (!done) { ++it; continue; }
        for (auto& image : it->images) {
            if (image.view) vkDestroyImageView(device, image.view, nullptr);
            if (image.ready) vkDestroySemaphore(device, image.ready, nullptr);
            if (image.fence) vkDestroyFence(device, image.fence, nullptr);
        }
        present_.destroy(device, it->chain, nullptr);
        it = retired_.erase(it);
    }
}
void VulkanRenderer::retire_images(bool keep_buffers) {
    if (!device) return;
    // shortcut: legacy Vulkan has no presentation fence at shutdown; use device idle until maintenance1 is available.
    VkResult status = present_.wait_idle(device);
    if (status != VK_SUCCESS && status != VK_ERROR_DEVICE_LOST) vk_check(status, "retire device work");
    old_swapchain_ = VK_NULL_HANDLE;
    if (status == VK_SUCCESS) {
        collect();
        reap_retired(true);
        for (const auto& image : images_)
            if (present_fences_ && image.pending) vk_check(vkWaitForFences(device, 1, &image.fence, VK_TRUE, 1000000000ull), "presentation retirement fence");
    }
    for (auto& image : images_) {
        if (image.view) vkDestroyImageView(device, image.view, nullptr);
        if (image.ready) vkDestroySemaphore(device, image.ready, nullptr);
        if (image.fence) vkDestroyFence(device, image.fence, nullptr);
    }
    images_.clear();
    if (swapchain_) present_.destroy(device, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
    hdr_pass_.forget();   // the device is idle: the sets of the slot images below can go
    for (auto& slot : slots_) {
        if (!keep_buffers) {
            retire_textures(slot);
            destroy_buffer(slot.vertices); destroy_buffer(slot.indices); destroy_buffer(slot.pixels); destroy_buffer(slot.uniforms); destroy_buffer(slot.palettes); destroy_buffer(slot.banks); destroy_buffer(slot.motions); slot.mesh_refs.clear();
            for (Buffer* buffer:{&slot.xform_jobs,&slot.xform_chunks,&slot.xform_positions,&slot.xform_attributes,&slot.xform_indices}) destroy_buffer(*buffer);
        }
        destroy_slot_images(slot);
        slot.submitted = slot.reported = slot.initialized = slot.copied = false; slot.serial = 0;
    }
    destroy_held_frame();
}
UpscaleImage VulkanRenderer::hudless_image() const {   // this frame's HUD-less copy (alternating: the interpolation of the previous frame may still read the other one)
    const auto slot=frame_index_&1;
    return {hudless_[slot],hudless_view_[slot],hudless_format_,hudless_extent_,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT};
}
void VulkanRenderer::record_upscale(VkCommandBuffer command, bool black_output) {
    tracker_.end_frame();   // every scene draw of the frame is recorded: camera cut and reprojection of this frame
    rt_key_direction_=tracker_.camera_cut() ? kRtOverheadDirection : rt_key_direction_after(rt_key_direction_,tracker_.view_to_prev_view());   // the key light stands still in the world
    if (effects_need_lights()) dominant_light_=gx::select_dominant(scene_lights_.lights(),dominant_light_);
    const auto now=std::chrono::steady_clock::now();
    UpscaleInputs in;
    in.command=command; in.targets_generation=targets_generation_;
    // Usage as make_attachment creates them with motion outputs on.
    const VkImageUsageFlags color_usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
    const VkImageUsageFlags depth_usage=VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
    in.color={scene_color_,scene_color_view_,VK_FORMAT_R8G8B8A8_UNORM,scene_extent_,color_usage};
    in.depth={scene_depth_,scene_depth_view_,depth_format_,scene_extent_,depth_usage};
    in.motion={scene_motion_,scene_motion_view_,kMotionFormat,scene_extent_,color_usage};
    in.output={efb_,efb_view_,VK_FORMAT_R8G8B8A8_UNORM,efb_extent_,color_usage};
    // The scene viewport (640x360 of the 640x480 logical EFB, top-left) in pixels of each target.
    const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
    const auto region=[&](VkExtent2D extent) {
        return VkExtent2D{std::clamp<uint32_t>(uint32_t(std::lround((viewport[0]+viewport[2])*extent.width/640)),1,extent.width),
                          std::clamp<uint32_t>(uint32_t(std::lround((viewport[1]+viewport[3])*extent.height/480)),1,extent.height)};
    };
    in.render_size=region(scene_extent_); in.output_size=region(efb_extent_);
    in.jitter=jitter_;
    in.reset=reset_next_ || tracker_.camera_cut() || black_output || (scene_vectors_wanted() && !motion_cleared_);   // the last term: a reader switched on mid-frame finds no vectors written this frame
    in.sharpening=sharpening_; in.sharpness=float(sharpness_);
    in.frame_ms=last_upscale_==std::chrono::steady_clock::time_point{} ? 16.7f : std::clamp(std::chrono::duration<float,std::milli>(now-last_upscale_).count(),0.1f,100.f);
    in.frame=upscale_frame_;
    in.projection=scene_projection_; in.clip_to_prev_clip=tracker_.clip_to_prev_clip();
    dlss5_scene_={in.depth,in.motion,in.render_size,in.reset,true};   // the present stage reads the same scene targets (valid until then)
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,scene_color_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
    barrier(command,scene_depth_,scene_depth_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);
    barrier(command,scene_motion_,motion_initialized_ ? scene_motion_layout_ : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);   // never bound (bloom alone): still UNDEFINED
    scene_depth_layout_=scene_motion_layout_=VK_IMAGE_LAYOUT_GENERAL;   // and they stay there until the next frame's first scene pass
    if (gbuffer_cleared_) { barrier(command,scene_gbuffer_,scene_gbuffer_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any); scene_gbuffer_layout_=VK_IMAGE_LAYOUT_GENERAL; }   // written this frame: readable by the effects
    if (rt_material_cleared_) {   // likewise the RT pair
        for (VkImage image:{scene_material_,scene_albedo_}) barrier(command,image,scene_rt_layout_,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
        scene_rt_layout_=VK_IMAGE_LAYOUT_GENERAL;
    }
    barrier(command,efb_,efb_initialized_ ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
    const auto timer_slot=uint32_t(next_slot_);
    if (shadow_cleared_) barrier(command,shadow_map_.image(),VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);   // drawn this frame: readable by the composite
    const std::array<float,6> scene_viewport{viewport[0]*scene_extent_.width/640,viewport[1]*scene_extent_.height/480,viewport[2]*scene_extent_.width/640,viewport[3]*scene_extent_.height/480,viewport[4],viewport[5]};
    bool rt_shadow_ran=false, rt_reflect_go=false, rt_light_go=false, rt_path_go=false;
    // DLSS Ray Reconstruction (plan p7): when an RT effect is active on a DLSS frame RR upscales the picture instead of Super Resolution. The effects keep their own denoisers:
    // RR fed the raw one-ray signals streaked textures and smeared characters in dark scenes (owner video 2026-10-20). It needs the guides, hence the same preconditions as the RT effects
    static const bool rr_disabled=std::getenv("UC_NO_RR")!=nullptr;   // debug: A/B against the own denoiser with DLSS Super Resolution
    const bool rr_frame=!rr_disabled && effects_.ray_reconstruction && rr_use(upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active(),rr_available_,rt_scene_active()) &&
        rt_frame_ && (rt_as_open_ || rt_.stats().instances) &&   // the frame has the structures the RT effects trace (the same entry condition as their block below)
        gbuffer_cleared_ && rt_material_cleared_ && frame_projection_ && rt_record_count_ && scene_material_ && scene_albedo_;
    if (rr_frame!=rr_frame_) in.reset=true;   // Super Resolution and Ray Reconstruction keep separate histories: the first frame of either after the other starts a new one
    if (rt_frame_ && (rt_as_open_ || rt_.stats().instances)) {   // the TLAS over this frame's BLASes (once: nothing after the boundary builds), then the ray-traced shadows
        if (!rt_as_open_) pass_timer_.begin(command,timer_slot,GpuPass::rt_as);
        rt_as_open_=false; rt_frame_=false;
        const bool tlas_built=rt_.build_tlas(command);
        pass_timer_.end(command,timer_slot,GpuPass::rt_as);
        const bool rt_ready=tlas_built && gbuffer_cleared_ && rt_material_cleared_ && frame_projection_ && rt_record_count_ && scene_material_;
        if (rt_ready && rt_trace_active()) {   // ray-traced shadows: trace, denoise, composite into the scene colour
            const auto idle=[this] { vk_check(present_.wait_idle(device),"rt shadows idle"); };
            rt_shadow_.initialize(device,memory_,shaders_,uint32_t(kFrameSlots),idle);
            rt_shadow_denoiser_.initialize(device,memory_,shaders_,idle);
            pass_timer_.begin(command,timer_slot,GpuPass::rt_shadows);
            RtShadowInputs shadow;
            shadow.command=command; shadow.tlas=rt_.tlas(); shadow.depth=scene_depth_view_; shadow.gbuffer=scene_gbuffer_view_; shadow.material=scene_material_view_; shadow.color=scene_color_view_;
            shadow.records=rt_records_[rt_record_slot_].buffer; shadow.lights=rt_lights_[rt_record_slot_].buffer;
            shadow.region=in.render_size; shadow.viewport=scene_viewport; shadow.view=view_params(*frame_projection_);
            shadow.sun=effects_.rt_shadows; shadow.local=effects_.rt_local_shadows; shadow.ao=effects_.rt_ambient_occlusion; shadow.slot=timer_slot; shadow.level_light=effects_.rt_local_shadows!=EffectLevel::off && !rt_trace_lamps_ && !effects_.path_tracing;   /* path tracing has its own key light: the local shadows are the rig lights of the characters only */ shadow.trace_lamps=rt_trace_lamps_; shadow.key_direction=rt_key_direction_;   // the test mode traces the lamps instead of the key light
            shadow.frame=uint32_t(upscale_frame_%8)+1;   // the noise pattern cycles on every path: the own denoiser's history (and a temporal upscaler's) averages it
            const VkImageView noisy=rt_shadow_.trace(shadow);
            {
                rt_shadow_denoiser_.resize(rt_shadow_.signal_extent());
                const std::array<EffectLevel,3> signal_levels{effects_.rt_shadows,effects_.rt_local_shadows,effects_.rt_ambient_occlusion};
                if (in.reset || !rt_shadow_ran_last_ || signal_levels!=rt_signal_levels_) rt_shadow_denoiser_.reset();   // a cut, a black frame, a frame without the effect or another mix of effects in the signal: the history is stale
                rt_signal_levels_=signal_levels;
                DenoiseInputs denoise;
                denoise.depth=scene_depth_view_; denoise.motion=scene_motion_view_; denoise.gbuffer=scene_gbuffer_view_; denoise.region=in.render_size;
                denoise.view=view_params(*frame_projection_); denoise.depth_range={scene_viewport[4],scene_viewport[5]}; denoise.targets_generation=targets_generation_;
                const bool high=int(effects_.rt_shadows)>=int(EffectLevel::high) || int(effects_.rt_local_shadows)>=int(EffectLevel::high) || int(effects_.rt_ambient_occlusion)>=int(EffectLevel::high);
                rt_shadow_.composite(shadow,rt_shadow_denoiser_.denoise(command,RtSignal::visibility,noisy,denoise,high ? 3 : 2));
            }
            pass_timer_.end(command,timer_slot,GpuPass::rt_shadows);
            rt_shadow_traced_=true; rt_shadow_ran=true;
        }
        rt_reflect_go=rt_ready && rt_reflect_active(); rt_light_go=rt_ready && rt_light_active(); rt_path_go=rt_ready && rt_path_active() && rt_record_count_ && rt_.hit_table() && scene_albedo_;   // they run after the raster occlusion below: they read the colour all the shading finished
    }
    rt_shadow_ran_last_=rt_shadow_ran;
    if ((effects_.ambient_occlusion!=EffectLevel::off || effects_.contact_shadows || (effects_.shadows!=EffectLevel::off && shadow_cleared_)) && gbuffer_cleared_ && frame_projection_) {   // the occlusion darkens the scene colour before it is reconstructed
        occlusion_.initialize(device,memory_,shaders_,[this] { vk_check(present_.wait_idle(device),"occlusion idle"); });
        pass_timer_.begin(command,timer_slot,GpuPass::occlusion);
        OcclusionInputs occlusion;
        occlusion.command=command; occlusion.targets_generation=targets_generation_; occlusion.color=in.color; occlusion.depth=in.depth;
        occlusion.gbuffer={scene_gbuffer_,scene_gbuffer_view_,kGBufferFormat,scene_extent_,color_usage};
        occlusion.region=in.render_size;
        occlusion.viewport=scene_viewport;
        occlusion.view=view_params(*frame_projection_);
        occlusion.ao=effects_.ambient_occlusion; occlusion.contact=effects_.contact_shadows; occlusion.light=dominant_light_; occlusion.slot=timer_slot;
        if (shadow_cleared_ && shadow_fit_ && dominant_light_) {
            const auto& p=dominant_light_->position; const float length=std::sqrt(p[0]*p[0]+p[1]*p[1]+p[2]*p[2]);
            occlusion.shadow=ShadowInputs{shadow_map_.view(),shadow_map_.generation(),*shadow_fit_,shadow_preset(effects_.shadows),{p[0]/length,p[1]/length,p[2]/length}};
        }
        occlusion.noise_frame=(upscaler_kind_==UpscalerKind::dlss || upscaler_kind_==UpscalerKind::fsr) ? uint32_t(upscale_frame_%8)+1 : 0;   // the upscaler's history averages the pattern; otherwise it stays fixed
        occlusion_.record(occlusion);
        pass_timer_.end(command,timer_slot,GpuPass::occlusion);
    }
    // The reflections and the lighting (one diffuse bounce): the same pass in two modes, one instance and one denoiser each, after every other shading of the colour (RT shadows
    // and AO, GTAO, the shadow map, contact shadows); the lighting first, so that the reflections show the bounced light too.
    const auto radiance_pass=[&](RtReflectionPass& pass, RtDenoiser& denoiser, RtRadianceMode mode, EffectLevel level, uint32_t history, bool& ran_last, GpuPass timed) {
        const auto idle=[this] { vk_check(present_.wait_idle(device),"rt radiance idle"); };
        pass.initialize(device,memory_,shaders_,uint32_t(kFrameSlots),idle);
        denoiser.initialize(device,memory_,shaders_,idle);
        denoiser.set_max_history(history);
        pass_timer_.begin(command,timer_slot,timed);
        RtReflectInputs inputs;
        inputs.mode=mode; inputs.command=command; inputs.tlas=rt_.tlas(); inputs.depth=scene_depth_view_; inputs.gbuffer=scene_gbuffer_view_; inputs.material=scene_material_view_; inputs.color=scene_color_view_; inputs.albedo=scene_albedo_view_;
        inputs.records=rt_records_[rt_record_slot_].buffer; inputs.region=in.render_size; inputs.viewport=scene_viewport; inputs.view=view_params(*frame_projection_);
        inputs.level=level; inputs.slot=timer_slot; inputs.frame=uint32_t(upscale_frame_%64)+1;   // more directions than the shadows' 8: one ray per pixel would otherwise average eight fixed samples
        const VkImageView noisy=pass.trace(inputs);
        {
            denoiser.resize(pass.signal_extent());
            if (in.reset || !ran_last) denoiser.reset();
            DenoiseInputs denoise;
            denoise.depth=scene_depth_view_; denoise.motion=scene_motion_view_; denoise.gbuffer=scene_gbuffer_view_; denoise.region=in.render_size;
            denoise.view=view_params(*frame_projection_); denoise.depth_range={scene_viewport[4],scene_viewport[5]}; denoise.targets_generation=targets_generation_;
            pass.composite(inputs,denoiser.denoise(command,RtSignal::radiance,noisy,denoise,int(level)>=int(EffectLevel::high) ? 3 : 2));
        }
        pass_timer_.end(command,timer_slot,timed);
    };
    if (rt_path_go) {   // path tracing: the lit surfaces' lighting from emitters over several bounces, denoised like the radiance effects (docs/superpowers/plans/2026-10-20-path-tracing-spike.md)
        RtPathSettings path_settings=rt_path_override_ ? *rt_path_override_ : rt_path_preset(effects_.rt_quality);
        static const bool env_tuning=[] { for (const char* name:{"UC_PT_DIVISOR","UC_PT_PATHS","UC_PT_BOUNCES","UC_PT_REACH","UC_PT_EMISSION","UC_PT_AMBIENT","UC_PT_MAX_RADIANCE","UC_PT_AO_RAYS","UC_PT_AO_RADIUS","UC_PT_AO_STRENGTH","UC_PT_KEY","UC_PT_KEY_REACH","UC_PT_MIX","UC_PT_THRESHOLD"}) if (std::getenv(name)) return true; return false; }();
        if (env_tuning) {   // debug: UC_PT_<NAME> overrides one setting of the quality preset while tuning
            const auto env=[](const char* name, auto& value) { if (const char* text=std::getenv(name)) value=static_cast<std::remove_reference_t<decltype(value)>>(std::strtod(text,nullptr)); };
            auto& s=path_settings;
            env("UC_PT_DIVISOR",s.divisor); env("UC_PT_PATHS",s.paths); env("UC_PT_BOUNCES",s.bounces); env("UC_PT_REACH",s.reach); env("UC_PT_EMISSION",s.emission); env("UC_PT_AMBIENT",s.ambient);
            env("UC_PT_MAX_RADIANCE",s.max_radiance); env("UC_PT_AO_RAYS",s.ao_rays); env("UC_PT_AO_RADIUS",s.ao_radius); env("UC_PT_AO_STRENGTH",s.ao_strength); env("UC_PT_KEY",s.key); env("UC_PT_KEY_REACH",s.key_reach); env("UC_PT_MIX",s.mix); env("UC_PT_THRESHOLD",s.threshold);
        }
        const auto idle=[this] { vk_check(present_.wait_idle(device),"rt path idle"); };
        rt_path_.initialize(device,memory_,shaders_,uint32_t(kFrameSlots),idle);
        rt_path_denoiser_.initialize(device,memory_,shaders_,idle);
        rt_path_denoiser_.set_max_history(kRtLightHistory);
        pass_timer_.begin(command,timer_slot,GpuPass::rt_path);
        RtPathInputs path;
        path.command=command; path.tlas=rt_.tlas(); path.depth=scene_depth_view_; path.gbuffer=scene_gbuffer_view_; path.material=scene_material_view_; path.color=scene_color_view_; path.albedo=scene_albedo_view_;
        path.records=rt_records_[rt_record_slot_].buffer; path.hit_table=rt_.hit_table(); path.hit_bases=rt_.hit_bases(); path.record_count=rt_record_count_;
        path.region=in.render_size; path.viewport=scene_viewport; path.view=view_params(*frame_projection_); path.overhead=rt_key_direction_; path.settings=path_settings;
        path.frame=uint32_t(upscale_frame_%64)+1; path.slot=timer_slot;
        const VkImageView noisy=rt_path_.trace(path);
        rt_path_denoiser_.resize(rt_path_.signal_extent());
        if (in.reset || !rt_path_ran_last_ || path_settings!=rt_path_last_settings_) rt_path_denoiser_.reset();   // a cut, a frame without it, or another quality: the history is stale
        rt_path_last_settings_=path_settings;
        DenoiseInputs denoise;
        denoise.depth=scene_depth_view_; denoise.motion=scene_motion_view_; denoise.gbuffer=scene_gbuffer_view_; denoise.region=in.render_size;
        denoise.view=view_params(*frame_projection_); denoise.depth_range={scene_viewport[4],scene_viewport[5]}; denoise.targets_generation=targets_generation_;
        rt_path_.composite(path,rt_path_denoiser_.denoise(command,RtSignal::radiance,noisy,denoise,3));
        pass_timer_.end(command,timer_slot,GpuPass::rt_path);
    }
    rt_path_ran_last_=rt_path_go;
    if (rt_light_go) radiance_pass(rt_lighting_,rt_lighting_denoiser_,RtRadianceMode::lighting,effects_.rt_lighting,kRtLightHistory,rt_light_ran_last_,GpuPass::rt_lighting);
    if (rt_reflect_go) radiance_pass(rt_reflection_,rt_reflection_denoiser_,RtRadianceMode::reflections,effects_.rt_reflections,kRtReflectHistory,rt_reflect_ran_last_,GpuPass::rt_reflections);
    rt_light_ran_last_=rt_light_go; rt_reflect_ran_last_=rt_reflect_go;
    if (rr_frame) {   // the guides Ray Reconstruction takes besides the colour, depth, motion and the albedo target
        rr_inputs_.initialize(device,memory_,shaders_,uint32_t(kFrameSlots),[this] { vk_check(present_.wait_idle(device),"rr inputs idle"); });
        RrInputs guides;
        guides.command=command; guides.depth=scene_depth_view_; guides.gbuffer=scene_gbuffer_view_; guides.material=scene_material_view_;
        guides.records=rt_records_[rt_record_slot_].buffer; guides.region=in.render_size; guides.extent=scene_extent_; guides.viewport=scene_viewport; guides.view=view_params(*frame_projection_); guides.slot=timer_slot;
        rr_inputs_.record(guides);
        in.ray_reconstruction=true;
        in.diffuse_albedo={scene_albedo_,scene_albedo_view_,kAlbedoFormat,scene_extent_,color_usage};
        in.specular_albedo=rr_inputs_.specular_albedo(); in.normal_roughness=rr_inputs_.normal_roughness();
    }
    rr_frame_=rr_frame;
    dlss5_scene_.reset=in.reset;
    record_dlss5(command,Dlss5Hook::render,in.color,{0,0},in.render_size,jitter_,black_output);   // DLSS 5 before the upscaler: on the jittered scene colour
    const auto timed_pass=upscaler_kind_==UpscalerKind::smaa ? GpuPass::smaa : GpuPass::upscale;
    pass_timer_.begin(command,timer_slot,timed_pass);
    upscaler_->evaluate(in);
    if (fsr_generation_enabled()) fsr_g_->prepare(in,frame_index_);
    if (sharpening_ && upscaler_kind_!=UpscalerKind::off && !upscaler_->sharpens()) rcas_.record(command,efb_,efb_view_,in.output_size,rcas_constant(sharpness_));   // the pass-through (off) never sharpens
    pass_timer_.end(command,timer_slot,timed_pass);
    if (effects_.motion_blur!=EffectLevel::off && !in.reset) {   // a cut, a black frame or the first frame after a rebuild has no valid vectors
        motion_blur_.initialize(device,memory_,shaders_,[this] { vk_check(present_.wait_idle(device),"motion blur idle"); });
        pass_timer_.begin(command,timer_slot,GpuPass::motion_blur);
        MotionBlurInputs blur;
        blur.command=command; blur.targets_generation=targets_generation_; blur.color=in.output; blur.motion=in.motion;
        blur.output_size=in.output_size; blur.render_size=in.render_size; blur.level=effects_.motion_blur;
        motion_blur_.record(blur);
        pass_timer_.end(command,timer_slot,GpuPass::motion_blur);
    }
    record_dlss5(command,Dlss5Hook::upscaled,in.output,{0,0},in.output_size,{0,0},black_output);   // DLSS 5 after the upscaler, before the HUD
    if (bloom_active() && !black_output) {   // the glow: added to the picture in SDR, kept in float for the HDR encode; both before the HUD
        bloom_pass_.initialize(device,memory_,shaders_,1,[this] { vk_check(present_.wait_idle(device),"bloom idle"); });
        pass_timer_.begin(command,timer_slot,GpuPass::bloom);
        BloomInputs bloom;
        bloom.command=command; bloom.color=in.output; bloom.region=in.output_size; bloom.settings=bloom_; bloom.hdr=hdr_active_;
        bloom.hdr_headroom=float(hdr_settings_.peak_nits)/float(hdr_settings_.paper_white_nits);
        bloom.slot=0; bloom.targets_generation=targets_generation_;
        bloom_pass_.record(bloom);
        bloom_region_=in.output_size;
        if (bloom_pass_.generation()!=bloom_generation_) { hdr_pass_.forget(); bloom_generation_=bloom_pass_.generation(); }   // the pass idled the device when it recreated the glow: sets naming the old views are stale
        pass_timer_.end(command,timer_slot,GpuPass::bloom);
    } else bloom_pass_.invalidate(0);
    barrier(command,efb_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    barrier(command,scene_color_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    if (scene_restore_copy_) {   // restore reconstructed pixels after bloom, rather than the raw jittered scene saved before this boundary
        slots_[next_slot_].copy_sources.push_back(scene_restore_copy_->target);   // the refresh submission owns this GPU use too
        auto& texture=copy_textures_.at(scene_restore_copy_->target.get());
        record_texture_copy(command,efb_,efb_extent_,scene_restore_copy_->source,texture,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        scene_restore_copy_.reset();
    }
    // Draws after the boundary (particles in front of the HUD, 3D markers) test against the scene depth.
    barrier(command,depth_,efb_initialized_ ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageBlit blit{}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_DEPTH_BIT,0,0,1};
    blit.srcOffsets[1]={int32_t(scene_extent_.width),int32_t(scene_extent_.height),1}; blit.dstOffsets[1]={int32_t(efb_extent_.width),int32_t(efb_extent_.height),1};
    vkCmdBlitImage(command,scene_depth_,VK_IMAGE_LAYOUT_GENERAL,depth_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
    barrier(command,depth_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,all,any,all,any,VK_IMAGE_ASPECT_DEPTH_BIT);
    efb_initialized_=true;
    reset_next_=false; scene_phase_=false; ++upscale_frame_; last_upscale_=now;
}
void VulkanRenderer::run_commands(const std::function<void(VkCommandBuffer)>& record) {
    owner();
    vk_check(present_.wait_idle(device),"run commands idle");
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"run commands buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin run commands");
    try { record(command); } catch (...) { vkEndCommandBuffer(command); vkFreeCommandBuffers(device,pool_,1,&command); throw; }
    vk_check(vkEndCommandBuffer(command),"end run commands");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit run commands");
    vk_check(vkQueueWaitIdle(queue_),"run commands wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
}
VulkanRenderer::MotionPixels VulkanRenderer::capture_motion() {
    owner();
    if (!scene_motion_ || !motion_initialized_) throw std::logic_error("no motion target to capture");
    vk_check(present_.wait_idle(device),"motion capture idle");
    const VkDeviceSize bytes=VkDeviceSize(scene_extent_.width)*scene_extent_.height*8;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"motion capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin motion capture");
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,scene_motion_,scene_motion_layout_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
    vkCmdCopyImageToBuffer(command,scene_motion_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
    barrier(command,scene_motion_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scene_motion_layout_,all,any,all,any);
    vk_check(vkEndCommandBuffer(command),"end motion capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit motion capture");
    vk_check(vkQueueWaitIdle(queue_),"motion capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    MotionPixels pixels{scene_extent_.width,scene_extent_.height,{}};
    pixels.rg.reserve(size_t(scene_extent_.width)*scene_extent_.height*2);
    const auto* halves=static_cast<const uint16_t*>(buffer.mapped);
    const auto half=[](uint16_t h) {   // IEEE half to float
        const uint32_t sign=uint32_t(h>>15)<<31, exponent=(h>>10)&31, mantissa=h&1023;
        if (!exponent) return std::bit_cast<float>(sign)+(sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24);
        if (exponent==31) return std::bit_cast<float>(sign|0x7F800000u|(mantissa<<13));
        return std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
    };
    for (size_t i=0;i<size_t(scene_extent_.width)*scene_extent_.height;++i) { pixels.rg.push_back(half(halves[i*4])); pixels.rg.push_back(half(halves[i*4+1])); }
    destroy_buffer(buffer);
    return pixels;
}
static float half_to_float(uint16_t h) {   // IEEE half to float
    const uint32_t sign=uint32_t(h>>15)<<31, exponent=(h>>10)&31, mantissa=h&1023;
    return !exponent ? std::bit_cast<float>(sign)+(sign ? -1.f : 1.f)*std::ldexp(float(mantissa),-24) : exponent==31 ? std::bit_cast<float>(sign|0x7F800000u|(mantissa<<13)) : std::bit_cast<float>(sign|((exponent+112)<<23)|(mantissa<<13));
}
VulkanRenderer::RtShadowPixels VulkanRenderer::capture_rt_shadow_signal() {
    owner();
    const VkExtent2D extent=rt_shadow_.signal_extent();
    if (!rt_shadow_traced_ || !rt_shadow_.signal_image()) throw std::logic_error("no ray-traced shadow signal to capture");
    vk_check(present_.wait_idle(device),"rt shadow capture idle");
    const VkDeviceSize bytes=VkDeviceSize(extent.width)*extent.height*2;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    run_commands([&](VkCommandBuffer command) {
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={extent.width,extent.height,1};
        vkCmdCopyImageToBuffer(command,rt_shadow_.signal_image(),VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);
    });
    RtShadowPixels pixels{extent.width,extent.height,{}};
    pixels.factor.reserve(size_t(extent.width)*extent.height);
    const auto* halves=static_cast<const uint16_t*>(buffer.mapped);
    for (size_t i=0;i<size_t(extent.width)*extent.height;++i) pixels.factor.push_back(half_to_float(halves[i]));
    destroy_buffer(buffer);
    return pixels;
}
VulkanRenderer::RtShadowPixels VulkanRenderer::capture_rt_shadow_filtered() {
    owner();
    const VkExtent2D extent=rt_shadow_.signal_extent();
    if (!rt_shadow_traced_ || !rt_shadow_denoiser_.output_image()) throw std::logic_error("no denoised ray-traced shadow signal to capture");
    vk_check(present_.wait_idle(device),"rt shadow capture idle");
    const VkDeviceSize bytes=VkDeviceSize(extent.width)*extent.height*8;   // RGBA16F: the factor is the red channel
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    run_commands([&](VkCommandBuffer command) {
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={extent.width,extent.height,1};
        vkCmdCopyImageToBuffer(command,rt_shadow_denoiser_.output_image(),VK_IMAGE_LAYOUT_GENERAL,buffer.buffer,1,&copy);
    });
    RtShadowPixels pixels{extent.width,extent.height,{}};
    pixels.factor.reserve(size_t(extent.width)*extent.height);
    const auto* halves=static_cast<const uint16_t*>(buffer.mapped);
    for (size_t i=0;i<size_t(extent.width)*extent.height;++i) pixels.factor.push_back(half_to_float(halves[i*4]));
    destroy_buffer(buffer);
    return pixels;
}
VulkanRenderer::GBufferPixels VulkanRenderer::capture_gbuffer() {
    owner();
    if (!scene_gbuffer_ || !gbuffer_initialized_) throw std::logic_error("no G-buffer to capture");
    vk_check(present_.wait_idle(device),"G-buffer capture idle");
    const VkDeviceSize bytes=VkDeviceSize(scene_extent_.width)*scene_extent_.height*8;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"G-buffer capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin G-buffer capture");
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,scene_gbuffer_,scene_gbuffer_layout_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
    vkCmdCopyImageToBuffer(command,scene_gbuffer_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
    barrier(command,scene_gbuffer_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scene_gbuffer_layout_,all,any,all,any);
    vk_check(vkEndCommandBuffer(command),"end G-buffer capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit G-buffer capture");
    vk_check(vkQueueWaitIdle(queue_),"G-buffer capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    const auto* data=static_cast<const uint16_t*>(buffer.mapped);
    GBufferPixels pixels{scene_extent_.width,scene_extent_.height,{data,data+size_t(bytes/2)}};
    destroy_buffer(buffer);
    return pixels;
}
VulkanRenderer::RtMaterialPixels VulkanRenderer::capture_rt_material() {
    owner();
    if (!scene_material_ || !rt_material_initialized_) throw std::logic_error("no RT material targets to capture");
    vk_check(present_.wait_idle(device),"RT material capture idle");
    const VkDeviceSize texels=VkDeviceSize(scene_extent_.width)*scene_extent_.height;
    Buffer buffer=make_buffer(texels*8,VK_BUFFER_USAGE_TRANSFER_DST_BIT);   // records, then albedo (4 bytes each)
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"RT material capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin RT material capture");
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    VkDeviceSize offset=0;
    for (VkImage image:{scene_material_,scene_albedo_}) {
        barrier(command,image,scene_rt_layout_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
        VkBufferImageCopy copy{}; copy.bufferOffset=offset; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
        vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);
        barrier(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,scene_rt_layout_,all,any,all,any);
        offset+=texels*4;
    }
    vk_check(vkEndCommandBuffer(command),"end RT material capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit RT material capture");
    vk_check(vkQueueWaitIdle(queue_),"RT material capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    const auto* records=static_cast<const uint32_t*>(buffer.mapped);
    const auto* albedo=static_cast<const uint8_t*>(buffer.mapped)+texels*4;
    RtMaterialPixels pixels{scene_extent_.width,scene_extent_.height,{records,records+texels},{albedo,albedo+texels*4}};
    destroy_buffer(buffer);
    return pixels;
}
std::vector<RtMaterialRecord> VulkanRenderer::rt_material_records() const {
    owner();
    const auto& buffer=rt_records_[rt_record_slot_];
    if (!buffer.mapped) return {};
    const auto* records=static_cast<const RtMaterialRecord*>(buffer.mapped);
    return {records,records+rt_record_count_};
}
RenderPixels VulkanRenderer::capture_scene() {
    owner();
    if (!scene_color_ || !scene_initialized_) throw std::logic_error("no scene target to capture");
    vk_check(present_.wait_idle(device),"scene capture idle");
    const VkDeviceSize bytes=VkDeviceSize(scene_extent_.width)*scene_extent_.height*4;
    Buffer buffer=make_buffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; allocation.commandPool=pool_; allocation.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; allocation.commandBufferCount=1;
    VkCommandBuffer command=VK_NULL_HANDLE; vk_check(vkAllocateCommandBuffers(device,&allocation,&command),"scene capture command");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(command,&begin),"begin scene capture");
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={scene_extent_.width,scene_extent_.height,1};
    vkCmdCopyImageToBuffer(command,scene_color_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,buffer.buffer,1,&copy);   // between passes the color is TRANSFER_SRC
    vk_check(vkEndCommandBuffer(command),"end scene capture");
    VkCommandBufferSubmitInfo command_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command_info.commandBuffer=command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.commandBufferInfoCount=1; submit.pCommandBufferInfos=&command_info;
    vk_check(vkQueueSubmit2(queue_,1,&submit,VK_NULL_HANDLE),"submit scene capture");
    vk_check(vkQueueWaitIdle(queue_),"scene capture wait");
    vkFreeCommandBuffers(device,pool_,1,&command);
    const auto* data=static_cast<const uint8_t*>(buffer.mapped);
    RenderPixels pixels{last_serial_,scene_extent_.width,scene_extent_.height,{data,data+size_t(bytes)}};
    destroy_buffer(buffer);
    return pixels;
}
void VulkanRenderer::destroy_scene_targets() {
    ++targets_generation_;   // views of the images made next may reuse these handles: effect passes must rebind
    scene_restore_copy_.reset();
    for (auto [view,image,memory]:{std::tuple{&scene_color_view_,&scene_color_,&scene_color_memory_},{&scene_depth_view_,&scene_depth_,&scene_depth_memory_},{&scene_motion_view_,&scene_motion_,&scene_motion_memory_},{&scene_gbuffer_view_,&scene_gbuffer_,&scene_gbuffer_memory_}}) {
        if (*view) vkDestroyImageView(device,*view,nullptr);
        if (*image) vkDestroyImage(device,*image,nullptr);
        if (*memory) vkFreeMemory(device,*memory,nullptr);
        *view=VK_NULL_HANDLE; *image=VK_NULL_HANDLE; *memory=VK_NULL_HANDLE;
    }
    scene_extent_={}; scene_initialized_=motion_initialized_=gbuffer_initialized_=gbuffer_cleared_=false;
    scene_depth_layout_=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; scene_motion_layout_=scene_gbuffer_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    destroy_rt_material_targets();
}
void VulkanRenderer::destroy_rt_material_targets() {
    for (auto [view,image,memory]:{std::tuple{&scene_material_view_,&scene_material_,&scene_material_memory_},{&scene_albedo_view_,&scene_albedo_,&scene_albedo_memory_}}) {
        if (*view) vkDestroyImageView(device,*view,nullptr);
        if (*image) vkDestroyImage(device,*image,nullptr);
        if (*memory) vkFreeMemory(device,*memory,nullptr);
        *view=VK_NULL_HANDLE; *image=VK_NULL_HANDLE; *memory=VK_NULL_HANDLE;
    }
    rt_material_initialized_=rt_material_cleared_=false; scene_rt_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
// The RT pair exists exactly while an RT effect is active and the scene targets exist (same extent).
void VulkanRenderer::sync_rt_material_targets() {
    const bool wanted=rt_material_active() && scene_color_;
    if (!wanted) { destroy_rt_material_targets(); return; }
    if (scene_material_) return;
    make_attachment(scene_material_,scene_material_memory_,scene_material_view_,scene_extent_,false,kMaterialFormat);
    make_attachment(scene_albedo_,scene_albedo_memory_,scene_albedo_view_,scene_extent_,false,kAlbedoFormat);
}
void VulkanRenderer::make_attachment(VkImage& target,VkDeviceMemory& memory,VkImageView& target_view,VkExtent2D extent,bool depth,VkFormat format,uint32_t layers) {
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = format!=VK_FORMAT_UNDEFINED ? format : depth ? depth_format_ : VK_FORMAT_R8G8B8A8_UNORM;
    image.extent = {extent.width, extent.height, 1}; image.mipLevels = 1; image.arrayLayers = layers; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;   // sampled: the HDR encode reads the EFB and the slot images
    if (motion_outputs_) image.usage |= VK_IMAGE_USAGE_SAMPLED_BIT | (depth ? 0 : VK_IMAGE_USAGE_STORAGE_BIT);   // upscaler inputs and outputs
    if (depth && layers_>1 && particle_layer_on()) image.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;   // the particle layer's depth downsample reads it
    vk_check(vkCreateImage(device, &image, nullptr, &target), "native attachment image");
    VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device, target, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk_check(vkAllocateMemory(device, &allocate, nullptr, &memory), "attachment memory"); vk_check(vkBindImageMemory(device, target, memory, 0), "bind attachment");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = target; view.viewType = layers>1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; view.format = image.format; view.subresourceRange = {VkImageAspectFlags(depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, 1, 0, layers};
    vk_check(vkCreateImageView(device, &view, nullptr, &target_view), "attachment view");
}
void VulkanRenderer::destroy_fovea_map(FoveaMap& map) {
    if (map.view) vkDestroyImageView(device,map.view,nullptr);
    if (map.image) vkDestroyImage(device,map.image,nullptr);
    if (map.memory) vkFreeMemory(device,map.memory,nullptr);
    map=FoveaMap{};
}
void VulkanRenderer::destroy_fovea() { destroy_fovea_map(fovea_efb_); destroy_fovea_map(fovea_eye_); fovea_level_=0; }
// The headset's "Game crosshair: Hidden" (the laser aims): no reticle quad or wake. UC_VR_HIDE_CROSSHAIR=1: the same in the desktop preview.
bool VulkanRenderer::crosshair_hidden() const {
    static const bool preview_hidden=std::getenv("UC_VR_HIDE_CROSSHAIR")!=nullptr;
#ifdef UC_OPENXR
    if (xr_) return !xr_->game_crosshair();
#endif
    return preview_hidden && preview_;
}
// The headset's HUD layer (hud_layer.h). Only over a world (the scan below): menus keep their 2D screens in the EFB.
bool VulkanRenderer::hud_layer_wanted() const {
    static const bool off=std::getenv("UC_NO_HUD_LAYER")!=nullptr;
    return !off && layers_>1 && stereo_ && xform_;
}
void VulkanRenderer::ensure_hud_layer(HudLayer& layer, VkExtent2D extent) {
    if (layer.light && layer.extent.width==extent.width && layer.extent.height==extent.height) return;
    if (layer.light) {   // a new size (render scale): no frame in flight may still read the old images
        vk_check(present_.wait_idle(device),"HUD layer resize");
        for (VkImageView* view:{&layer.light_view,&layer.transmit_view}) { if (*view) vkDestroyImageView(device,*view,nullptr); *view=VK_NULL_HANDLE; }
        for (VkImage* image:{&layer.light,&layer.transmit}) { if (*image) vkDestroyImage(device,*image,nullptr); *image=VK_NULL_HANDLE; }
        for (VkDeviceMemory* memory:{&layer.light_memory,&layer.transmit_memory}) { if (*memory) vkFreeMemory(device,*memory,nullptr); *memory=VK_NULL_HANDLE; }
    }
    for (int i=0;i<2;++i) {   // a layer per eye
        VkImage& image=i ? layer.transmit : layer.light; VkDeviceMemory& memory=i ? layer.transmit_memory : layer.light_memory; VkImageView& view=i ? layer.transmit_view : layer.light_view;
        VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; create.imageType=VK_IMAGE_TYPE_2D; create.format=VK_FORMAT_R8G8B8A8_UNORM;
        create.extent={extent.width,extent.height,1}; create.mipLevels=1; create.arrayLayers=2; create.samples=VK_SAMPLE_COUNT_1_BIT; create.tiling=VK_IMAGE_TILING_OPTIMAL;
        create.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        vk_check(vkCreateImage(device,&create,nullptr,&image),"HUD layer image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,image,&requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device,&allocate,nullptr,&memory),"HUD layer memory"); vk_check(vkBindImageMemory(device,image,memory,0),"bind HUD layer");
        VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; info.image=image; info.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; info.format=create.format; info.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
        vk_check(vkCreateImageView(device,&info,nullptr,&view),"HUD layer view");
    }
    layer.extent=extent; layer.used=false;
    std::fprintf(stderr,"[vr] %s layer %ux%u a layer (EFB %ux%u)\n",&layer==&glow_ ? "glow" : "HUD",extent.width,extent.height,efb_extent_.width,efb_extent_.height);
    if (!hud_.layout) {   // the composite into the EFB: two pipelines over one triangle
        VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter=sampler.minFilter=VK_FILTER_LINEAR; sampler.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sampler.addressModeU=sampler.addressModeV=sampler.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vk_check(vkCreateSampler(device,&sampler,nullptr,&hud_.sampler),"HUD layer sampler");
        const VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},{1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
                                                       {2,VK_DESCRIPTOR_TYPE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,&hud_.sampler}};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=3; set.pBindings=bindings;
        vk_check(vkCreateDescriptorSetLayout(device,&set,nullptr,&hud_.set_layout),"HUD layer set layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT,0,112};   // hud_layer.slang Composite
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&hud_.set_layout; layout.pushConstantRangeCount=1; layout.pPushConstantRanges=&push;
        vk_check(vkCreatePipelineLayout(device,&layout,nullptr,&hud_.layout),"HUD layer pipeline layout");
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,6},{VK_DESCRIPTOR_TYPE_SAMPLER,3}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=3; pool.poolSizeCount=2; pool.pPoolSizes=sizes;   // the HUD, glow and particle layers
        vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&hud_.pool),"HUD layer pool");
        for (HudLayer* each:{&hud_,&glow_}) {   // a set per layer, both from hud_'s pool
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=hud_.pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&hud_.set_layout;
            vk_check(vkAllocateDescriptorSets(device,&allocate,&each->set),"HUD layer set");
        }
        hud_.vertex=shader(device,shaders_/"hud_layer.vert.spv"); hud_.fragment=shader(device,shaders_/"hud_layer.frag.spv");
        for (uint32_t add=0;add<2;++add) {
            const VkSpecializationMapEntry entry{0,0,sizeof(uint32_t)}; const VkSpecializationInfo specialization{1,&entry,sizeof add,&add};
            const VkPipelineShaderStageCreateInfo stages[]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,hud_.vertex,"vertexMain",nullptr},
                                                            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,hud_.fragment,"fragmentMain",&specialization}};
            VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=viewport.scissorCount=1;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState attachment{}; attachment.blendEnable=VK_TRUE; attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT;
            attachment.srcColorBlendFactor=attachment.srcAlphaBlendFactor=add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_DST_COLOR;
            attachment.dstColorBlendFactor=attachment.dstAlphaBlendFactor=add ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_COLOR;
            attachment.colorBlendOp=attachment.alphaBlendOp=VK_BLEND_OP_ADD;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&attachment;
            const VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamic_states;
            const VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.viewMask=0b11; rendering.colorAttachmentCount=1; rendering.pColorAttachmentFormats=&format;
            VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.pNext=&rendering; pipeline.stageCount=2; pipeline.pStages=stages;
            pipeline.pVertexInputState=&vertices; pipeline.pInputAssemblyState=&assembly; pipeline.pViewportState=&viewport; pipeline.pRasterizationState=&raster;
            pipeline.pMultisampleState=&multisample; pipeline.pColorBlendState=&blend; pipeline.pDynamicState=&dynamic; pipeline.layout=hud_.layout;
            vk_check(vkCreateGraphicsPipelines(device,pipeline_cache_,1,&pipeline,nullptr,&hud_.flush[add]),"HUD layer composite pipeline");
        }
    }
    const VkDescriptorImageInfo images[2]{{VK_NULL_HANDLE,layer.light_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,layer.transmit_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i=0;i<2;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=layer.set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo=&images[i]; }
    vkUpdateDescriptorSets(device,2,writes,0,nullptr);
}
void VulkanRenderer::destroy_hud_layer() {
    for (HudLayer* layer:{&hud_,&glow_}) {
        for (VkImageView view:{layer->light_view,layer->transmit_view}) if (view) vkDestroyImageView(device,view,nullptr);
        for (VkImage image:{layer->light,layer->transmit}) if (image) vkDestroyImage(device,image,nullptr);
        for (VkDeviceMemory memory:{layer->light_memory,layer->transmit_memory}) if (memory) vkFreeMemory(device,memory,nullptr);
    }
    for (VkPipeline pipeline:hud_.flush) if (pipeline) vkDestroyPipeline(device,pipeline,nullptr);
    if (hud_.pool) vkDestroyDescriptorPool(device,hud_.pool,nullptr);
    if (hud_.layout) vkDestroyPipelineLayout(device,hud_.layout,nullptr);
    if (hud_.set_layout) vkDestroyDescriptorSetLayout(device,hud_.set_layout,nullptr);
    if (hud_.sampler) vkDestroySampler(device,hud_.sampler,nullptr);
    for (VkShaderModule module:{hud_.vertex,hud_.fragment}) if (module) vkDestroyShaderModule(device,module,nullptr);
    hud_=HudLayer{}; glow_=HudLayer{};
}
void VulkanRenderer::flush_hud_layer(VkCommandBuffer command) { flush_layer(command,glow_); flush_layer(command,hud_); }
// A layer into the EFB (both eyes): EFB * T, then + L. The EFB is between passes (TRANSFER_SRC_OPTIMAL) and is left so.
void VulkanRenderer::flush_layer(VkCommandBuffer command, HudLayer& layer) {
    if (!layer.used) return;
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    for (VkImage image:{layer.light,layer.transmit}) barrier(command,image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView=efb_view_; color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},efb_extent_}; rendering.layerCount=1; rendering.viewMask=0b11; rendering.colorAttachmentCount=1; rendering.pColorAttachments=&color;
    vkCmdBeginRendering(command,&rendering);
    const VkViewport viewport{0,0,float(efb_extent_.width),float(efb_extent_.height),0,1}; const VkRect2D scissor{{0,0},efb_extent_};
    vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
    struct { float rows[2][3][4]; float size[2]; float unused[2]; } push{};
    static_assert(sizeof push==112);
    for (int eye=0;eye<2;++eye) for (int row=0;row<3;++row) for (int c=0;c<4;++c) push.rows[eye][row][c]=layer.map.rows[eye][row][c];
    push.size[0]=float(efb_extent_.width); push.size[1]=float(efb_extent_.height);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,hud_.layout,0,1,&layer.set,0,nullptr);
    vkCmdPushConstants(command,hud_.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
    for (VkPipeline pipeline:hud_.flush) { vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); vkCmdDraw(command,3,1,0,0); }
    vkCmdEndRendering(command);
    barrier(command,efb_,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,all,any);
    layer.used=false; ++hud_.flushes;
}
// The bloom proxy (the headset; UC_NO_BLOOM_PROXY=1: off): a stand-in for the EFB at 2 points a guest pixel (1280x960, a layer per eye) holding the
// top-left corner (kBloomCorner) while the game's bloom chain draws there.
void VulkanRenderer::ensure_bloom_proxy() {
    const VkExtent2D extent{640*2,480*2};
    if (bloom_proxy_.color) return;
    make_attachment(bloom_proxy_.color,bloom_proxy_.color_memory,bloom_proxy_.color_view,extent,false,VK_FORMAT_UNDEFINED,layers_);
    make_attachment(bloom_proxy_.depth,bloom_proxy_.depth_memory,bloom_proxy_.depth_view,extent,true,VK_FORMAT_UNDEFINED,layers_);
    bloom_proxy_.extent=extent; bloom_proxy_.initialized=false; bloom_proxy_.valid=false;
    std::fprintf(stderr,"[vr] bloom proxy %ux%u a layer\n",extent.width,extent.height);
}
void VulkanRenderer::destroy_bloom_proxy() {
    for (VkImageView view:{bloom_proxy_.color_view,bloom_proxy_.depth_view}) if (view) vkDestroyImageView(device,view,nullptr);
    for (VkImage image:{bloom_proxy_.color,bloom_proxy_.depth}) if (image) vkDestroyImage(device,image,nullptr);
    for (VkDeviceMemory memory:{bloom_proxy_.color_memory,bloom_proxy_.depth_memory}) if (memory) vkFreeMemory(device,memory,nullptr);
    bloom_proxy_=BloomProxy{};
}
// The corner between the EFB and the proxy: into the proxy (to_efb false: the chain starts) or back into the EFB (true). Both images are
// between passes (TRANSFER_SRC_OPTIMAL) before and after.
void VulkanRenderer::bloom_proxy_corner(VkCommandBuffer command, bool to_efb) {
    const VkImage source=to_efb ? bloom_proxy_.color : efb_, target=to_efb ? efb_ : bloom_proxy_.color;
    const VkExtent2D source_extent=to_efb ? bloom_proxy_.extent : efb_extent_, target_extent=to_efb ? efb_extent_ : bloom_proxy_.extent;
    const bool target_ready=to_efb ? efb_initialized_ : bloom_proxy_.initialized;
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(command,target,target_ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit blit{}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers_};
    // into the proxy: the whole corner; back into the EFB: exactly the pixels whose centres the chain's draws covered (the rest of the EFB
    // keeps what it had, as if the chain had drawn there at full resolution)
    std::array<float,4> area{0,0,float(kBloomCorner[0]),float(kBloomCorner[1])};
    if (to_efb) area={std::max(0.f,bloom_proxy_.covered[0]),std::max(0.f,bloom_proxy_.covered[1]),std::min(float(kBloomCorner[0]),bloom_proxy_.covered[2]),std::min(float(kBloomCorner[1]),bloom_proxy_.covered[3])};
    const auto edge=[](float guest,uint32_t extent,uint32_t axis) { return int32_t(std::max(0.f,std::ceil(guest*extent/(axis ? 480.f : 640.f)-.5f))); };   // first pixel whose centre lies at or past it
    blit.srcOffsets[0]={edge(area[0],source_extent.width,0),edge(area[1],source_extent.height,1),0};
    blit.srcOffsets[1]={std::max(blit.srcOffsets[0].x+1,edge(area[2],source_extent.width,0)),std::max(blit.srcOffsets[0].y+1,edge(area[3],source_extent.height,1)),1};
    blit.dstOffsets[0]={edge(area[0],target_extent.width,0),edge(area[1],target_extent.height,1),0};
    blit.dstOffsets[1]={std::max(blit.dstOffsets[0].x+1,edge(area[2],target_extent.width,0)),std::max(blit.dstOffsets[0].y+1,edge(area[3],target_extent.height,1)),1};
    if (to_efb && !(bloom_proxy_.covered[2]>bloom_proxy_.covered[0] && bloom_proxy_.covered[3]>bloom_proxy_.covered[1])) blit.dstOffsets[1]=blit.dstOffsets[0];   // nothing drawn: nothing to put back
    if (blit.dstOffsets[1].x>blit.dstOffsets[0].x) vkCmdBlitImage(command,source,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_LINEAR);
    barrier(command,target,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,all,any);
    if (!to_efb) bloom_proxy_.initialized=true;
}
void VulkanRenderer::put_back_bloom_proxy(VkCommandBuffer command) {
    bloom_proxy_corner(command,true);
    bloom_proxy_.valid=false; ++bloom_proxy_.put_backs;
    static const bool stats=std::getenv("UC_GPU_PACKETS")!=nullptr;
    static auto since=std::chrono::steady_clock::now();
    if (stats && std::chrono::steady_clock::now()-since>std::chrono::seconds(2)) {
        std::fprintf(stderr,"[bloom-proxy] 2 s: %llu packets drawn in the proxy, the corner put back %llu times\n",(unsigned long long)bloom_proxy_.packets,(unsigned long long)bloom_proxy_.put_backs);
        bloom_proxy_.packets=bloom_proxy_.put_backs=0; since=std::chrono::steady_clock::now();
    }
}
void VulkanRenderer::ensure_particle_layer() {
    const VkExtent2D extent{(efb_extent_.width+1)/2,(efb_extent_.height+1)/2};
    if (particle_.color && particle_.extent.width==extent.width && particle_.extent.height==extent.height) return;
    if (particle_.color) {
        vk_check(present_.wait_idle(device),"particle layer resize");
        for (VkImageView view:{particle_.color_view,particle_.depth_view}) if (view) vkDestroyImageView(device,view,nullptr);
        for (VkImage image:{particle_.color,particle_.depth}) if (image) vkDestroyImage(device,image,nullptr);
        for (VkDeviceMemory memory:{particle_.color_memory,particle_.depth_memory}) if (memory) vkFreeMemory(device,memory,nullptr);
    }
    make_attachment(particle_.color,particle_.color_memory,particle_.color_view,extent,false,VK_FORMAT_UNDEFINED,layers_);
    make_attachment(particle_.depth,particle_.depth_memory,particle_.depth_view,extent,true,VK_FORMAT_UNDEFINED,layers_);
    particle_.extent=extent; particle_.initialized=false;
    if (!hud_.layout) ensure_hud_layer(hud_,{16,16});   // the composite's layout, sampler and shaders live with the HUD layer's
    if (!particle_.set) {
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=hud_.pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&hud_.set_layout;
        vk_check(vkAllocateDescriptorSets(device,&allocate,&particle_.set),"particle layer set");
        const uint32_t mode=2; const VkSpecializationMapEntry entry{0,0,sizeof(uint32_t)}; const VkSpecializationInfo specialization{1,&entry,sizeof mode,&mode};
        const VkPipelineShaderStageCreateInfo stages[]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,hud_.vertex,"vertexMain",nullptr},
                                                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,hud_.fragment,"fragmentMain",&specialization}};
        VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{}; attachment.blendEnable=VK_TRUE; attachment.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT;
        attachment.srcColorBlendFactor=attachment.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE; attachment.dstColorBlendFactor=attachment.dstAlphaBlendFactor=VK_BLEND_FACTOR_SRC_ALPHA;   // EFB * T + L
        attachment.colorBlendOp=attachment.alphaBlendOp=VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount=1; blend.pAttachments=&attachment;
        const VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamic_states;
        const VkFormat format=VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.viewMask=0b11; rendering.colorAttachmentCount=1; rendering.pColorAttachmentFormats=&format;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.pNext=&rendering; pipeline.stageCount=2; pipeline.pStages=stages;
        pipeline.pVertexInputState=&vertices; pipeline.pInputAssemblyState=&assembly; pipeline.pViewportState=&viewport; pipeline.pRasterizationState=&raster;
        pipeline.pMultisampleState=&multisample; pipeline.pColorBlendState=&blend; pipeline.pDynamicState=&dynamic; pipeline.layout=hud_.layout;
        vk_check(vkCreateGraphicsPipelines(device,pipeline_cache_,1,&pipeline,nullptr,&particle_.composite),"particle layer composite pipeline");
    }
    if (!particle_.depth_pipeline) {   // the depth downsample: a set with the scene depth, a depth-only multiview pipeline
        const VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo set{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; set.bindingCount=1; set.pBindings=&binding;
        vk_check(vkCreateDescriptorSetLayout(device,&set,nullptr,&particle_.depth_set_layout),"particle depth set layout");
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount=1; layout.pSetLayouts=&particle_.depth_set_layout;
        vk_check(vkCreatePipelineLayout(device,&layout,nullptr,&particle_.depth_layout),"particle depth layout");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&size;
        vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&particle_.depth_pool),"particle depth pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=particle_.depth_pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&particle_.depth_set_layout;
        vk_check(vkAllocateDescriptorSets(device,&allocate,&particle_.depth_set),"particle depth set");
        particle_.depth_vertex=shader(device,shaders_/"particle_depth.vert.spv"); particle_.depth_fragment=shader(device,shaders_/"particle_depth.frag.spv");
        static const uint32_t farthest=[] { const char* v=std::getenv("UC_PARTICLE_DEPTH"); return v && std::string_view(v)=="far" ? 1u : 0u; }();
        const VkSpecializationMapEntry entry{0,0,sizeof(uint32_t)}; const VkSpecializationInfo specialization{1,&entry,sizeof farthest,&farthest};
        const VkPipelineShaderStageCreateInfo stages[]={{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,particle_.depth_vertex,"vertexMain",nullptr},
                                                        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,particle_.depth_fragment,"fragmentMain",&specialization}};
        VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; depth.depthTestEnable=VK_TRUE; depth.depthWriteEnable=VK_TRUE; depth.depthCompareOp=VK_COMPARE_OP_ALWAYS;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        const VkDynamicState dynamic_states[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.viewMask=0b11; rendering.depthAttachmentFormat=depth_format_;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.pNext=&rendering; pipeline.stageCount=2; pipeline.pStages=stages;
        pipeline.pVertexInputState=&vertices; pipeline.pInputAssemblyState=&assembly; pipeline.pViewportState=&viewport; pipeline.pRasterizationState=&raster;
        pipeline.pMultisampleState=&multisample; pipeline.pDepthStencilState=&depth; pipeline.pColorBlendState=&blend; pipeline.pDynamicState=&dynamic; pipeline.layout=particle_.depth_layout;
        vk_check(vkCreateGraphicsPipelines(device,pipeline_cache_,1,&pipeline,nullptr,&particle_.depth_pipeline),"particle depth pipeline");
    }
    const VkDescriptorImageInfo images[2]{{VK_NULL_HANDLE,particle_.color_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},{VK_NULL_HANDLE,particle_.color_view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i=0;i<2;++i) { writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[i].dstSet=particle_.set; writes[i].dstBinding=i; writes[i].descriptorCount=1; writes[i].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[i].pImageInfo=&images[i]; }
    vkUpdateDescriptorSets(device,2,writes,0,nullptr);
    std::fprintf(stderr,"[vr] particle layer %ux%u a layer\n",extent.width,extent.height);
    particle_.scene_depth_view=VK_NULL_HANDLE;   // the set is pointed at the EFB depth on first use (ensure below)
}
void VulkanRenderer::destroy_particle_layer() {
    for (VkImageView view:{particle_.color_view,particle_.depth_view}) if (view) vkDestroyImageView(device,view,nullptr);
    for (VkImage image:{particle_.color,particle_.depth}) if (image) vkDestroyImage(device,image,nullptr);
    for (VkDeviceMemory memory:{particle_.color_memory,particle_.depth_memory}) if (memory) vkFreeMemory(device,memory,nullptr);
    if (particle_.composite) vkDestroyPipeline(device,particle_.composite,nullptr);
    if (particle_.depth_pipeline) vkDestroyPipeline(device,particle_.depth_pipeline,nullptr);
    if (particle_.depth_layout) vkDestroyPipelineLayout(device,particle_.depth_layout,nullptr);
    if (particle_.depth_set_layout) vkDestroyDescriptorSetLayout(device,particle_.depth_set_layout,nullptr);
    if (particle_.depth_pool) vkDestroyDescriptorPool(device,particle_.depth_pool,nullptr);
    for (VkShaderModule module:{particle_.depth_vertex,particle_.depth_fragment}) if (module) vkDestroyShaderModule(device,module,nullptr);
    particle_=ParticleLayer{};
}
// The particle layer into the EFB (both eyes): EFB * T + L in one draw. The EFB and the layer are between passes (TRANSFER_SRC_OPTIMAL).
void VulkanRenderer::composite_particle_layer(VkCommandBuffer command) {
    const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier(command,particle_.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView=efb_view_; color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},efb_extent_}; rendering.layerCount=1; rendering.viewMask=0b11; rendering.colorAttachmentCount=1; rendering.pColorAttachments=&color;
    vkCmdBeginRendering(command,&rendering);
    const VkViewport viewport{0,0,float(efb_extent_.width),float(efb_extent_.height),0,1}; const VkRect2D scissor{{0,0},efb_extent_};
    vkCmdSetViewport(command,0,1,&viewport); vkCmdSetScissor(command,0,1,&scissor);
    struct { float rows[2][3][4]; float size[2]; float unused[2]; } push{};
    const float sx=float(efb_extent_.width)/(2*particle_.extent.width), sy=float(efb_extent_.height)/(2*particle_.extent.height);   // the layer covers 2x its size of the EFB
    for (int eye=0;eye<2;++eye) { push.rows[eye][0][0]=sx; push.rows[eye][1][1]=sy; push.rows[eye][2][2]=1; }
    push.size[0]=float(efb_extent_.width); push.size[1]=float(efb_extent_.height);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,hud_.layout,0,1,&particle_.set,0,nullptr);
    vkCmdPushConstants(command,hud_.layout,VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof push,&push);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,particle_.composite); vkCmdDraw(command,3,1,0,0);
    vkCmdEndRendering(command);
    barrier(command,efb_,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,all,any);
    barrier(command,particle_.color,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,all,any);
}
// Fragment-only uniform identity for merging: UC_MERGE_FRAG_UNIFORM when set, else on the headset the Display page's "Draw merging".
bool VulkanRenderer::frag_uniform_merge() const {
#ifdef UC_OPENXR
    if (xr_ && xform_ && !frag_uniform_forced_) return xr_->merge_frag_uniform();
#endif
    return frag_uniform_merge_;
}
// Coarse post-effect shading (post_shading_rate): UC_POST_SHADING=full|coarse or the headset's Display page; off elsewhere.
bool VulkanRenderer::post_shading_coarse() const {
#ifdef UC_OPENXR
    if (xr_) return xr_->post_shading_coarse();
#endif
    static const char* forced=std::getenv("UC_POST_SHADING");
    return forced && std::strcmp(forced,"coarse")==0;
}
// The foveation level asked for: UC_VR_FOVEATION=0..3 (tests, the preview), else the headset's vr.cfg / settings panel.
int VulkanRenderer::wanted_fovea_level() const {
    static const int forced=std::getenv("UC_VR_FOVEATION") ? std::atoi(std::getenv("UC_VR_FOVEATION")) : -1;
    if (forced>=0) return std::min(forced,3);
#ifdef UC_OPENXR
    if (xr_) return std::clamp(xr_->foveation_level(),0,3);
#endif
    return 0;
}
// One map for a target of `extent` pixels whose rectangle `shown` (target pixels) each eye sees: made again only when the level, the
// extent, the rectangle or the eyes' fields of view change (the device idles for the upload, so no frame in flight reads it meanwhile).
bool VulkanRenderer::update_fovea_map(FoveaMap& map, VkExtent2D extent, const std::array<float,4>& shown, const std::array<std::array<float,4>,2>& tangents, int level, uint32_t largest) {
    std::vector<float> key{float(level),float(largest),shown[0],shown[1],shown[2],shown[3]};
    for (const auto& eye:tangents) for (float t:eye) key.push_back(std::round(t*1000)/1000);   // the runtime's angles jitter in the last digits
    if (map.view && map.extent.width==extent.width && map.extent.height==extent.height && key==map.key) return false;
    const uint32_t layers=fovea_layered_ ? layers_ : 1;
    const uint32_t width=(extent.width+fovea_texel_.width-1)/fovea_texel_.width, height=(extent.height+fovea_texel_.height-1)/fovea_texel_.height;
    vk_check(present_.wait_idle(device),"foveation map idle");
    if (!map.view || map.extent.width!=extent.width || map.extent.height!=extent.height) {
        destroy_fovea_map(map);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R8_UINT;
        image.extent={width,height,1}; image.mipLevels=1; image.arrayLayers=layers; image.samples=VK_SAMPLE_COUNT_1_BIT; image.tiling=VK_IMAGE_TILING_OPTIMAL;
        image.usage=VK_IMAGE_USAGE_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vk_check(vkCreateImage(device,&image,nullptr,&map.image),"foveation map image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device,map.image,&requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize=requirements.size; allocate.memoryTypeIndex=memory_type(requirements.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk_check(vkAllocateMemory(device,&allocate,nullptr,&map.memory),"foveation map memory"); vk_check(vkBindImageMemory(device,map.image,map.memory,0),"bind foveation map");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image=map.image; view.viewType=layers>1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D; view.format=image.format;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
        vk_check(vkCreateImageView(device,&view,nullptr,&map.view),"foveation map view");
        map.extent=extent;
    }
    const auto texels=fovea_map(level,extent.width,extent.height,fovea_texel_.width,fovea_texel_.height,shown,tangents,layers,largest);
    Buffer staging=make_buffer(texels.size(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped,texels.data(),texels.size());
    const bool first=map.key.empty();
    run_commands([&](VkCommandBuffer command) {
        barrier(command,map.image,first ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers}; copy.imageExtent={width,height,1};
        vkCmdCopyBufferToImage(command,staging.buffer,map.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier(command,map.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADING_RATE_ATTACHMENT_BIT_KHR,VK_ACCESS_2_FRAGMENT_SHADING_RATE_ATTACHMENT_READ_BIT_KHR);
    });
    destroy_buffer(staging);
    // the shares of the shown rectangle's texels (what the eyes see) at each rate
    size_t counts[3]{}, inside=0;
    for (uint32_t layer=0;layer<layers;++layer) for (uint32_t y=0;y<height;++y) for (uint32_t x=0;x<width;++x) {
        const float cx=(x+.5f)*fovea_texel_.width, cy=(y+.5f)*fovea_texel_.height;
        if (cx<shown[0] || cx>=shown[2] || cy<shown[1] || cy>=shown[3]) continue;
        const uint8_t t=texels[(size_t(layer)*height+y)*width+x]; ++inside;
        ++counts[t==0 ? 0 : t==fovea_texel(2,2) ? 1 : 2];
    }
    inside=std::max<size_t>(inside,1);
    std::fprintf(stderr,"[vr] foveation level %d (coarsest %ux%u): map %ux%u x%u (texel %ux%u) for a %ux%u target, shown %.0f,%.0f..%.0f,%.0f: 1x1 %.0f%%, 2x2 %.0f%%, coarser %.0f%%; eye tangents L %.3f %.3f %.3f %.3f R %.3f %.3f %.3f %.3f\n",
                 level,largest,largest,width,height,layers,fovea_texel_.width,fovea_texel_.height,extent.width,extent.height,shown[0],shown[1],shown[2],shown[3],
                 100.0*counts[0]/inside,100.0*counts[1]/inside,100.0*counts[2]/inside,
                 tangents[0][0],tangents[0][1],tangents[0][2],tangents[0][3],tangents[1][0],tangents[1][1],tangents[1][2],tangents[1][3]);
    map.key=std::move(key);
    return true;
}
// Before a packet's passes are recorded: the maps for the current level, the EFB's size and shown region, the eye image and the eyes'
// fields of view.
void VulkanRenderer::update_fovea() {
    if (!fovea_supported_ || !efb_) return;
    const int level=wanted_fovea_level();
    if (!level) { if (fovea_level_) { std::fprintf(stderr,"[vr] foveation off\n"); fovea_level_=0; } return; }
    std::array<std::array<float,4>,2> tangents{};
    if (stereo_) tangents=stereo_->tangents();
    if (!(tangents[0][0]>0) || !(tangents[1][0]>0)) {   // before the first frame's views: the Quest 3's (preview_publish)
        const float outer=std::tan(52.f*.017453292f), inner=std::tan(41.f*.017453292f), down=std::tan(50.f*.017453292f), up=std::tan(42.f*.017453292f);
        tangents={std::array<float,4>{outer,inner,down,up},std::array<float,4>{inner,outer,down,up}};
    }
    const float sx=float(efb_extent_.width)/640, sy=float(efb_extent_.height)/480;
    const std::array<float,4> shown{fovea_shown_[0]*sx,fovea_shown_[1]*sy,(fovea_shown_[0]+fovea_shown_[2])*sx,(fovea_shown_[1]+fovea_shown_[3])*sy};
    // Below native scale an EFB pixel covers several eye pixels: the EFB's coarsest rate is capped so that a shading point spans no more
    // eye pixels than 4x4 does at native (1.5x: 2x2 at most; 1x: none, only the eye image's screen pass is foveated). UC_VR_FOVEA_CAP=0: no cap.
    uint32_t largest=fovea_largest_;
#ifdef UC_OPENXR
    static const bool capped=!(std::getenv("UC_VR_FOVEA_CAP") && *std::getenv("UC_VR_FOVEA_CAP")=='0');
    if (xr_ && capped && xr_->eye_width() && shown[2]>shown[0]) {
        const float eye_pixels=float(xr_->eye_width())/(shown[2]-shown[0]);   // eye pixels across one EFB pixel
        while (largest>1 && largest*eye_pixels>5.f) largest/=2;
    }
#endif
    if (largest>1) update_fovea_map(fovea_efb_,efb_extent_,shown,tangents,level,largest);
    else if (fovea_efb_.view) { vk_check(present_.wait_idle(device),"foveation map idle"); destroy_fovea_map(fovea_efb_); }
#ifdef UC_OPENXR
    static const bool no_eye=std::getenv("UC_NO_EYE_FOVEATION")!=nullptr;
    if (xr_ && !no_eye && xr_->eye_width() && xr_->eye_height())
        update_fovea_map(fovea_eye_,{xr_->eye_width(),xr_->eye_height()},{0,0,float(xr_->eye_width()),float(xr_->eye_height())},tangents,level,fovea_largest_);
#endif
    fovea_level_=level;
}
void VulkanRenderer::rebuild() {
    if (fsr_presentation_) fsr_g_->release_context();
    if (swapchain_ && !fsr_presentation_ && !streamline_swapchain()) retire_for_resize(); else retire_images(true);
    if (xr_) {   // the headset: no window swapchain; the picture at the eye's shape, exactly the visible scene (the EFB copy into it is 1:1)
        const auto visible=manual_render_extent(device_render_scale(configured_scale_,max_image_dimension_,render_aspect_),render_aspect_);
        extent_ = {visible.width, visible.height};
    }
    else {
    VkSurfaceCapabilitiesKHR capabilities; vk_check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &capabilities), "surface capabilities");
    VkImageUsageFlags swapchain_usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if ((capabilities.supportedUsageFlags & swapchain_usage) != swapchain_usage) throw std::runtime_error("swapchain lacks transfer/color attachment support");
    extent_ = capabilities.currentExtent;
    if (extent_.width == UINT32_MAX) extent_ = {std::clamp(requested_.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width), std::clamp(requested_.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height)};
    if (!extent_.width || !extent_.height) { resized_ = true; return; }
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical_, &properties);
    if (extent_.width > properties.limits.maxImageDimension2D || extent_.height > properties.limits.maxImageDimension2D) throw std::runtime_error("render extent exceeds device image limits");
    uint32_t count = 0; vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, nullptr), "surface format count");
    std::vector<VkSurfaceFormatKHR> formats(count); vk_check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &count, formats.data()), "surface formats");
    VkSurfaceFormatKHR chosen{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    // HDR10 when asked for and possible; SDL reads Windows' "Use HDR" of the window's display (DXGI), the surface may list HDR10 either way.
    const bool windows_hdr = SDL_GetBooleanProperty(SDL_GetWindowProperties(window_), SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
    const bool hdr10_format = colorspace_ext_ && std::any_of(formats.begin(), formats.end(), [](const auto& format) { return format.format == HdrPass::kFormat && format.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT; });
    hdr_active_ = hdr_settings_.enabled && windows_hdr && hdr10_format;
    if (hdr_active_) chosen = {HdrPass::kFormat, VK_COLOR_SPACE_HDR10_ST2084_EXT};
    else for (const auto& format : formats) {
        if (format.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (format.format == VK_FORMAT_UNDEFINED) { chosen = {VK_FORMAT_B8G8R8A8_UNORM, format.colorSpace}; break; }
        if (format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) { chosen = format; break; }
        if (format.format == VK_FORMAT_B8G8R8A8_SRGB || format.format == VK_FORMAT_R8G8B8A8_SRGB) chosen = format;
    }
    if (chosen.format == VK_FORMAT_UNDEFINED) throw std::runtime_error("8-bit SDR swapchain format unavailable");
    const char* hdr_status = hdr_active_ ? "on (HDR10 PQ)" : !hdr_settings_.enabled ? "off" : !windows_hdr ? "off (HDR is off in Windows for this display), SDR" : "off (no HDR10 swapchain format), SDR";
    if (!hdr_status_ || std::strcmp(hdr_status, hdr_status_) != 0) { std::fprintf(stderr, "HDR output: %s\n", hdr_status); hdr_status_ = hdr_status; }
    VkFormatProperties source_properties, target_properties;
    vkGetPhysicalDeviceFormatProperties(physical_, VK_FORMAT_R8G8B8A8_UNORM, &source_properties); vkGetPhysicalDeviceFormatProperties(physical_, chosen.format, &target_properties);
    VkFormatFeatureFlags source_features = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT | VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    VkFormatFeatureFlags target_features = hdr_active_ ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;   // HDR: drawn by the encode pass
    if ((source_properties.optimalTilingFeatures & source_features) != source_features || (target_properties.optimalTilingFeatures & target_features) != target_features)
        throw std::runtime_error("RGBA8 attachment/readback or swapchain blit format unsupported");
    uint32_t images = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount) images = std::min(images, capabilities.maxImageCount);
    VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR}; create.surface = surface_; create.minImageCount = images;
    // Borderless must not silently become driver-managed exclusive fullscreen.
#ifdef _WIN32
    VkSurfaceFullScreenExclusiveInfoEXT fullscreen{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    fullscreen.fullScreenExclusive=VK_FULL_SCREEN_EXCLUSIVE_DISALLOWED_EXT;
    if(disallow_exclusive_)create.pNext=&fullscreen;
#endif
    create.imageFormat = chosen.format; create.imageColorSpace = chosen.colorSpace; create.imageExtent = extent_; create.imageArrayLayers = 1;
    create.imageUsage = swapchain_usage; create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; create.preTransform = capabilities.currentTransform;
    create.compositeAlpha = VkCompositeAlphaFlagBitsKHR(capabilities.supportedCompositeAlpha & (0u - capabilities.supportedCompositeAlpha));
    create.presentMode = VK_PRESENT_MODE_FIFO_KHR; create.clipped = VK_TRUE;
    {   // mailbox by default: with fifo the compositor held a swapchain image for up to two extra refreshes now and then, and the guest stalled in GXWaitDrawDone for 9-12 ms (the micro-freezes). UC_PRESENT_MODE=fifo|mailbox|immediate overrides; fifo is also the fallback
        const char* env = std::getenv("UC_PRESENT_MODE");
        const std::string_view wanted = env ? env : "mailbox";
        const VkPresentModeKHR mode = wanted == "immediate" ? VK_PRESENT_MODE_IMMEDIATE_KHR : wanted == "mailbox" ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR;
        uint32_t mode_count = 0; vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
        std::vector<VkPresentModeKHR> modes(mode_count); vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
        if (std::find(modes.begin(), modes.end(), mode) != modes.end()) create.presentMode = mode;
    }
    create.oldSwapchain = old_swapchain_;
    swapchain_format_ = chosen.format;
    vk_check(present_.create(device, &create, nullptr, &swapchain_), "swapchain");
    std::fprintf(stderr,"Swapchain: %ux%u format=%d present=%s exclusive=%s\n",extent_.width,extent_.height,int(chosen.format),create.presentMode==VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : create.presentMode==VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo",disallow_exclusive_ ? "disallowed" : "driver default");
    old_swapchain_ = VK_NULL_HANDLE;   // retired by the call above; reap_retired() frees it when its presentations are done
    vk_check(present_.images(device, swapchain_, &count, nullptr), "swapchain image count");
    std::vector<VkImage> swapchain_images(count); vk_check(present_.images(device, swapchain_, &count, swapchain_images.data()), "swapchain images");
    images_.resize(count);
    for (size_t i = 0; i < images_.size(); ++i) {
        images_[i].image = swapchain_images[i];
        VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; vk_check(vkCreateSemaphore(device, &semaphore, nullptr, &images_[i].ready), "per-image present semaphore");
        if(present_fences_) { VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; vk_check(vkCreateFence(device, &fence, nullptr, &images_[i].fence), "per-image present fence"); }
        if (hdr_active_) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = swapchain_images[i]; view.viewType = VK_IMAGE_VIEW_TYPE_2D; view.format = chosen.format; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vk_check(vkCreateImageView(device, &view, nullptr, &images_[i].view), "swapchain image view");
        }
    }
    if (hdr_active_) hdr_pass_.initialize(device, shaders_);
    }
    const auto scale=device_render_scale(configured_scale_,max_image_dimension_,render_aspect_);
    const auto visible=manual_render_extent(scale,render_aspect_);
    const VkExtent2D internal{visible.width,uint32_t(std::llround(960*scale))};
    if(internal.width>max_image_dimension_ || internal.height>max_image_dimension_) throw std::runtime_error("internal render extent exceeds device image limits");
    if(efb_extent_.width!=internal.width || efb_extent_.height!=internal.height) {
        if(depth_view_) vkDestroyImageView(device,depth_view_,nullptr);
        if(depth_) vkDestroyImage(device,depth_,nullptr);
        if(depth_memory_) vkFreeMemory(device,depth_memory_,nullptr);
        depth_view_=VK_NULL_HANDLE; depth_=VK_NULL_HANDLE; depth_memory_=VK_NULL_HANDLE;
        if(efb_view_) vkDestroyImageView(device,efb_view_,nullptr);
        if(efb_) vkDestroyImage(device,efb_,nullptr);
        if(efb_memory_) vkFreeMemory(device,efb_memory_,nullptr);
        efb_view_=VK_NULL_HANDLE; efb_=VK_NULL_HANDLE; efb_memory_=VK_NULL_HANDLE; efb_initialized_=false; ++targets_generation_; efb_clear_.pending=false;
        sweep_meshes(true);
        for(auto& texture:recycled_copies_) destroy_texture(texture);
        recycled_copies_.clear(); recycled_copy_bytes_=0;
        efb_extent_=internal;
        std::fprintf(stderr,"Native render: %ux%u EFB,720p multiplier %.4gx (requested %.4gx), scene %ux%u, output %ux%u\n",internal.width,internal.height,scale,configured_scale_,visible.width,visible.height,extent_.width,extent_.height);
    }
    if (!efb_) make_attachment(efb_,efb_memory_,efb_view_,efb_extent_,false,VK_FORMAT_UNDEFINED,layers_);
    if (!depth_) make_attachment(depth_,depth_memory_,depth_view_,efb_extent_,true,VK_FORMAT_UNDEFINED,layers_);
    if (upscaler_ && upscaler_kind_==UpscalerKind::off && !scene_path_wanted()) { upscaler_.reset(); reset_next_=true; }   // the last effect that needed the scene targets went away
    if (motion_outputs_ && (upscaler_kind_!=UpscalerKind::off || scene_path_wanted()) && !upscaler_) {
        if (upscaler_kind_==UpscalerKind::off || upscaler_kind_==UpscalerKind::blit) upscaler_=make_blit_upscaler();   // off: the effects' pass-through at ratio 1
        else if (upscaler_kind_==UpscalerKind::smaa) upscaler_=make_smaa_upscaler(device,memory_,shaders_,[this] { vk_check(present_.wait_idle(device),"SMAA idle"); });
        else if (upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active()) upscaler_=make_dlss_upscaler(*streamline_,upscale_mode_);
        else if (upscaler_kind_==UpscalerKind::fsr) {
            try { upscaler_=make_fsr_upscaler(device,physical_,shaders_.parent_path().parent_path()); }
            catch (const std::exception& error) { std::fprintf(stderr,"FSR unavailable: %s\n",error.what()); }
        }
        else std::fprintf(stderr, "Upscaling: %s is not available in this build\n", upscaler_kind_==UpscalerKind::dlss ? "DLSS" : "FSR");
    }
    if (!upscaler_ && scene_color_) destroy_scene_targets();   // no reconstruction any more: the internal targets go too
    if (upscaler_) {
        const auto internal=internal_extent({efb_extent_.width,efb_extent_.height},effective_upscale_mode(),upscaler_->minimum_internal({efb_extent_.width,efb_extent_.height}));
        if (scene_extent_.width!=internal.width || scene_extent_.height!=internal.height) {
            destroy_scene_targets();
            scene_extent_={internal.width,internal.height};
            make_attachment(scene_color_,scene_color_memory_,scene_color_view_,scene_extent_);
            make_attachment(scene_depth_,scene_depth_memory_,scene_depth_view_,scene_extent_,true);
            make_attachment(scene_motion_,scene_motion_memory_,scene_motion_view_,scene_extent_,false,kMotionFormat);
            make_attachment(scene_gbuffer_,scene_gbuffer_memory_,scene_gbuffer_view_,scene_extent_,false,kGBufferFormat);
            reset_next_=true;
            std::fprintf(stderr,"Upscaling: scene %ux%u -> %ux%u EFB (%s)\n",scene_extent_.width,scene_extent_.height,efb_extent_.width,efb_extent_.height,
                upscaler_kind_==UpscalerKind::dlss ? "DLSS" : upscaler_kind_==UpscalerKind::fsr ? "FSR" : upscaler_kind_==UpscalerKind::smaa ? "SMAA" : "blit");
        }
    }
    sync_rt_material_targets();
    configure_generation();
    for (auto& slot : slots_) {
        make_attachment(slot.image,slot.memory,slot.view,extent_,false,VK_FORMAT_UNDEFINED,layers_);
        if (hdr_active_) make_attachment(slot.ui_image,slot.ui_memory,slot.ui_view,extent_);
    }
    resized_ = false;
}
void VulkanRenderer::configure_generation() {
    if (upscaler_ && frame_generation_ && upscaler_kind_==UpscalerKind::dlss && dlss_g_available_ && !dlss_g_)
        dlss_g_=std::make_unique<DlssFrameGeneration>(*streamline_,frame_generation_multiplier_);
    if (frame_generation_active() || (frame_generation_ && fsr_presentation_)) {
        const VkFormat hudless_format=hdr_active_ ? HdrPass::kFormat : VK_FORMAT_R8G8B8A8_UNORM;   // as the back buffer
        if (hudless_extent_.width!=extent_.width || hudless_extent_.height!=extent_.height || hudless_format_!=hudless_format) {
            for (int i=0;i<2;++i) {
                if (hudless_view_[i]) vkDestroyImageView(device,hudless_view_[i],nullptr);
                if (hudless_[i]) vkDestroyImage(device,hudless_[i],nullptr);
                if (hudless_memory_[i]) vkFreeMemory(device,hudless_memory_[i],nullptr);
                make_attachment(hudless_[i],hudless_memory_[i],hudless_view_[i],extent_,false,hudless_format);
            }
            hudless_extent_=extent_; hudless_format_=hudless_format;
        }
        if (upscaler_ && fsr_presentation_ && !fsr_g_->has_context()) {
            try { fsr_g_->create_context(extent_,scene_extent_,swapchain_format_,hdr_active_ ? float(hdr_settings_.peak_nits) : 0.f); }   // shortcut: a later peak change reaches FSR's motion estimate only with the next context
            catch (const std::exception& error) { std::fprintf(stderr,"FSR Frame Generation unavailable: %s\n",error.what()); }
        }
    }
}
bool VulkanRenderer::copy_ready(const std::shared_ptr<const gx::TexturePixels>& texture,bool previous_frame) {
    owner();
    if(!texture || !texture->gpu_only || texture->levels.empty())throw std::invalid_argument("invalid scene texture copy");
    detached_copies_=true;
    const auto found=copy_textures_.find(texture.get());
    if((found!=copy_textures_.end() && found->second.initialized) || lost_copies_.contains(texture.get()))return true;
    if(const auto alias=copy_aliases_.find(texture.get());alias!=copy_aliases_.end()) {
        if(!alias->second.requested.expired())return true;
        copy_aliases_.erase(alias);
    }
    if(previous_frame && texture->guest_address) {
        const auto latest=latest_copies_.find(texture->guest_address);
        const auto rendered=latest==latest_copies_.end() ? nullptr : latest->second.lock();
        if(rendered && rendered->channels==texture->channels &&
           rendered->levels.front().width==texture->levels.front().width &&
           rendered->levels.front().height==texture->levels.front().height) {
            // Temporal GPU effects advance at presentation rate; guest simulation never reads these pixels.
            copy_aliases_.emplace(texture.get(),CopyAlias{texture,rendered});
            return true;
        }
        if(!blank_) { auto blank=std::make_shared<gx::TexturePixels>(); blank->levels.push_back({1,1,{0,0,0,0}}); blank_=std::move(blank); }
        copy_aliases_.emplace(texture.get(),CopyAlias{texture,blank_});
        return true;
    }
    return false;
}
bool VulkanRenderer::render(const gx::FramePacket& packet, bool display,uint64_t submission_serial) {
    owner();
    if (!device) throw std::logic_error("renderer not initialized");
    if (!requested_.width || !requested_.height) return false;
    const auto serial=submission_serial ? submission_serial : packet.serial;
    if (!serial || serial <= last_serial_) throw std::invalid_argument("frame serial must increase");
    if(packet.clear_depth>0xFFFFFF) throw std::invalid_argument("invalid native EFB clear depth");
    for(const auto& prompt:packet.prompts) {
        for(float value:prompt.efb_corners)if(!std::isfinite(value))throw std::invalid_argument("nonfinite native prompt geometry");
        for(float value:prompt.alpha)if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("invalid native prompt alpha");
        for(const auto& tint:prompt.tint)for(float value:tint)if(!std::isfinite(value) || value<0 || value>1)throw std::invalid_argument("invalid native prompt tint");
        for(float value:prompt.clip)if(!std::isfinite(value))throw std::invalid_argument("nonfinite native prompt clip");
        if(prompt.clip[2]<prompt.clip[0] || prompt.clip[3]<prompt.clip[1])throw std::invalid_argument("invalid native prompt clip");
    }
    const auto& display_source=packet.display_source;
    if(packet.present && (!display_source[2] || !display_source[3] || display_source[0]>=640 || display_source[2]>640-display_source[0] || display_source[1]>=480 || display_source[3]>480-display_source[1])) throw std::invalid_argument("invalid native display source rectangle");
#ifdef UC_OPENXR
    if (xr_ && bloom_.enabled!=xr_->glow_on()) {   // the settings panel's Glow, applied at the next frame (the headset's menu owns the bloom switch)
        BloomSettings bloom=bloom_; bloom.enabled=xr_->glow_on(); set_bloom(bloom);
        std::fprintf(stderr,"[vr] glow %s from the settings panel\n",bloom.enabled ? "on" : "off");
    }
    if (xr_ && xr_->take_render_scale_change()) {   // the settings panel's Render scale: the EFB is remade at the next rebuild (below), no restart
        const float chosen=xr_->render_scale();
        native_eye_scale_=!(chosen>0);
        set_render_scale(native_eye_scale_ ? std::clamp(xr_->eye_height()/720.0,0.5,6.0) : double(chosen));
        std::fprintf(stderr,"[vr] render scale %.3f (%s) from the settings panel\n",configured_scale_,native_eye_scale_ ? "native" : "chosen");
    }
#endif
    if (layers_>1) {   // diagnostics: UC_VR_SCALE_SWITCH=<frame>:<scale>[,...] changes the scale live at those presented frames, as the panel does (0: native)
        static std::vector<std::pair<uint64_t,double>> switches=[] {
            std::vector<std::pair<uint64_t,double>> list;
            if (const char* text=std::getenv("UC_VR_SCALE_SWITCH")) for (const char* at=text;*at;) {
                char* end=nullptr; const auto frame=std::strtoull(at,&end,10); if (end==at || *end!=':') break;
                const double scale=std::strtod(end+1,&end); list.emplace_back(frame,scale); at=*end==',' ? end+1 : end;
            }
            std::reverse(list.begin(),list.end());
            return list;
        }();
        if (!switches.empty() && frame_index_>=switches.back().first) {
            const double scale=switches.back().second; switches.pop_back();
            native_eye_scale_=!(scale>0);
#ifdef UC_OPENXR
            const double native=xr_ ? std::clamp(xr_->eye_height()/720.0,0.5,6.0) : 3.0;
#else
            const double native=3.0;
#endif
            set_render_scale(native_eye_scale_ ? native : scale);
            std::fprintf(stderr,"[vr] render scale %.3f at frame %u (UC_VR_SCALE_SWITCH)\n",configured_scale_,frame_index_);
        }
    }
    const bool has_output = swapchain_ != VK_NULL_HANDLE || (xr_ && efb_);   // the headset has no window swapchain: its output is the XR swapchain
    if (const auto now = std::chrono::steady_clock::now(); uc::rebuild_due(resized_, has_output, now, last_rebuild_)) {
        last_rebuild_ = now; rebuild();        if (resized_) return false;
    }
    if (!swapchain_ && !(xr_ && efb_)) return false;   // while a resize storm postpones the rebuild the old swapchain keeps presenting
    if (fovea_supported_) {   // the headset's foveation map: the region the eyes see is the last presented one
        if (packet.present) fovea_shown_={float(display_source[0]),float(display_source[1]),float(display_source[2]),float(display_source[3])};
        update_fovea();
    }
    const bool overlay_frame=packet.overlay_only;
    const bool layer_on=reticle_layer_active();   // the reticle quads leave the game's frame and are drawn over a held copy of it (reticle_layer_ready)
    if (overlay_frame && !reticle_layer_ready()) return false;
    if (layer_on && !overlay_frame) ensure_held_frame();
    const bool hold=layer_on && !overlay_frame && packet.present && display && !packet.black_output && held_.image;   // this frame is kept, not presented: the overlay frame after it is
    reap_retired(false);
    Slot& slot = slots_[next_slot_];
    VkResult fence_status = vkGetFenceStatus(device, slot.fence);
    if (fence_status == VK_NOT_READY) return false;
    vk_check(fence_status, "frame slot fence");
    const size_t reticle_before=frame_reticle_.size();
    if (layer_on && !overlay_frame) for (const auto& draw:packet.draws) if (draw.layer) frame_reticle_.push_back(draw);   // after every early return: a packet that is retried must not be counted twice
    collect();   // after the check: a fence that finishes between an earlier collect() and the check would be reused unreported and its packet would never complete
    slot.copy_sources.clear();
    slot.mesh_refs.clear();   // the submission that used these meshes completed (fence checked above)
    if ((++renders_ & 255)==0) sweep_meshes(false);
    std::erase_if(copy_aliases_,[](const auto& entry){return entry.second.requested.expired();});
    for(auto it=copy_textures_.begin();it!=copy_textures_.end();) {
        if(it->second.source.use_count()==1) {
            auto retired=std::move(it->second); it=copy_textures_.erase(it);
            recycle_copy_texture(retired);
        }
        else ++it;
    }
    std::erase_if(latest_copies_,[](const auto& entry){return entry.second.expired();});
    Texture* copy_target=nullptr;
    if(packet.texture_copy) {
        const auto& request=*packet.texture_copy;
        const auto [x,y,width,height]=request.source;
        if(!width || !height || x>=640 || width>640-x || y>=480 || height>480-y || !request.target || !request.target->gpu_only) throw std::invalid_argument("invalid native EFB texture copy");
        auto found=copy_textures_.find(request.target.get());
        if(found==copy_textures_.end()) {
            auto texture=make_copy_texture(request.target);
            try { found=copy_textures_.emplace(request.target.get(),texture).first; } catch(...) { destroy_texture(texture); throw; }
        }
        if(found->second.initialized) throw std::invalid_argument("native EFB copy version cannot be overwritten");
        copy_target=&found->second; slot.copy_sources.push_back(request.target);
    }
    // Upscaling: a frame whose predecessor had a 3D scene draws into the internal scene targets until the scene boundary (the game's
    // copy of the finished scene, gx::SceneBoundary), is upscaled into efb_ there, and draws the rest (post chain, HUD) into efb_.
    const struct { gx::SceneBoundary boundary; bool frame_start, scene_phase, scene_mode, motion_cleared, gbuffer_cleared, reset_next; std::array<float,2> jitter; uint64_t whole_frame_upscales; size_t tracker; std::optional<gx::Projection> frame_projection; std::optional<ShadowFit> shadow_fit; bool shadow_cleared, rt_material_cleared; uint32_t rt_record_slot, rt_record_count; }
        upscale_before{boundary_,frame_start_,scene_phase_,scene_mode_,motion_cleared_,gbuffer_cleared_,reset_next_,jitter_,whole_frame_upscales_,tracker_.checkpoint(),frame_projection_,shadow_fit_,shadow_cleared_,rt_material_cleared_,rt_record_slot_,rt_record_count_};
    gx::PacketSummary summary;
    size_t scene_draws=0;          // draws [0, scene_draws) of this packet go to the scene targets
    bool upscale_here=false;       // the upscale runs after those draws, before the rest and before the packet's copy/present
    if (upscaler_ && !overlay_frame) {
        if (frame_start_) {
            const bool scene=boundary_.frame_had_scene();
            if (scene!=scene_mode_) reset_next_=true;
            scene_mode_=scene_phase_=scene; frame_start_=false; motion_cleared_=gbuffer_cleared_=false; hudless_taken_=false;
            scene_lights_.clear(); frame_projection_.reset(); shadow_fit_.reset(); shadow_cleared_=false;
            rt_material_cleared_=false; rt_record_slot_=frame_index_%kRtFrames; rt_record_count_=0;   // the slot's previous frame has completed (see rt_ below)
            rt_frame_=rt_scene_active();
            if (rt_frame_) {   // frame index modulo the in-flight slots: a frame has at least one submission, so the slot's previous frame has completed
                rt_.initialize(device,physical_,memory_,shaders_,uint32_t(kRtFrames));
                rt_.begin_frame(frame_index_);
            }
            if (upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active()) {
                streamline_->begin_frame(frame_index_);
                streamline_->marker(Streamline::Marker::simulation_start);
                streamline_->marker(Streamline::Marker::simulation_end);
                streamline_->marker(Streamline::Marker::render_submit_start);
            }
            if (fsr_presentation_ && fsr_g_->has_context()) fsr_g_->begin_frame(fsr_generation_enabled() && scene && !packet.black_output,hudless_image(),frame_index_);
            if (dlss_g_) dlss_g_->set_active(dlss_generation_enabled() && scene && !packet.black_output,reset_next_);
            if (scene_phase_) {
                tracker_.begin_frame();
                jitter_=uses_jitter(upscaler_kind_) ? jitter_offset(upscale_frame_,jitter_phase_count(scene_extent_.width,efb_extent_.width)) : std::array<float,2>{};
            }
        }
        summary=gx::summarize(packet);
        const auto cut=boundary_.on_packet(summary);
        if (scene_phase_) {
            scene_draws=cut.value_or(packet.draws.size());
            upscale_here=cut.has_value() || packet.present;
            if (!cut && packet.present) ++whole_frame_upscales_;   // no scene copy: the HUD goes through the upscaler too
        }
    }
    const bool vectors=scene_vectors_wanted();   // false: the scene path runs for the bloom alone, nothing reads the motion vectors
    std::vector<MotionBlock> motion_blocks;
    std::vector<uint32_t> draw_motion(scene_draws,kNoMotion);
    // RT material records (rt_material.h) of this packet's depth-writing scene draws, appended to the frame's at rt_record_count_.
    const bool rt_records=rt_material_active() && scene_material_;
    const uint32_t first_record=rt_record_count_;
    std::vector<uint32_t> draw_record(rt_records ? scene_draws : 0,kRtNoRecord);   // empty with RT off: read only when rt_records
    std::vector<RtMaterialRecord> records;
    std::vector<const gx::DrawUniform*> record_uniforms;
    std::unordered_set<const gx::TexturePixels*> used_textures;   // only non-null textures are ever looked up; neighbouring draws mostly repeat theirs
    used_textures.reserve(64);
    {
        const gx::TexturePixels* last=nullptr;
        for(const auto& draw:packet.draws) for(size_t map=0;map<8;++map) if(const auto* used=draw_texture(draw,map).get(); used && used!=last) { used_textures.insert(used); last=used; }
    }
    // This slot's fence is retired. A texture's GPU image is shared by all slots; the renderer holds one reference to its pixels, so more
    // belong to the CPU cache or queued packets, which retain it across passes.
    for (auto it=slot.textures.begin();it!=slot.textures.end();) {
        Texture& texture=**it;
        if (texture.initialized && texture.uploader==&slot) { destroy_buffer(texture.upload); texture.uploader=nullptr; }
        const bool owned=texture.source && texture.source.use_count()>1;
        const bool used=!texture.source || used_textures.contains(texture.source.get());
        if (!used && !owned) it=slot.textures.erase(it); else ++it;
    }
    // Per draw: where its vertices live (a persistent mesh buffer, or this frame's dynamic buffer for non-mesh draws) and which palette it reads.
    struct DrawGpu { PipelineKey key{}; PipelineChoice choice{}; const Buffer* vertices=nullptr; const Buffer* indices=nullptr; VkDeviceSize vertex_offset=0, index_offset=0; uint32_t index_count=0, first_index=0; int32_t first_vertex=0; uint32_t palette=0, current=0, indexed=0, bank=kNoBank; uint64_t mesh_id=0; const std::array<float,6>* box=nullptr; };   // box: a cached mesh's object-space bounds   // mesh_id: GpuMesh::id of a cached mesh draw (0 for an inline draw)
    std::vector<DrawGpu> gpu_draws; gpu_draws.reserve(packet.draws.size());
    std::unordered_map<const gx::TextureBank*,uint32_t> bank_of;   // one bank per distinct texture matrix set
    std::vector<const gx::TextureBank*> banks;
    std::unordered_map<const gx::Transforms*,uint32_t> palette_of;   // one palette per distinct transform block
    std::vector<const gx::Transforms*> palette_blocks;
    size_t vertex_count = 0, index_count = 0;   // dynamic (non-mesh) draws only
    for (const auto& draw : packet.draws) {
        if(draw.state->viewport) {
            const auto& v=*draw.state->viewport;
            for(auto value:v) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite native draw viewport");
            if(std::abs(v[0])>2048 || std::abs(v[1])>2048 || v[2]<=0 || v[3]<=0 || v[2]>1024 || v[3]>1024 || v[4]<0 || v[4]>1 || v[5]<0 || v[5]>1) throw std::invalid_argument("invalid native draw viewport");
        }
        if(draw.state->scissor && ((*draw.state->scissor)[2]<0 || (*draw.state->scissor)[3]<0)) throw std::invalid_argument("negative native draw scissor extent");
        // shortcut: indirect texturing/depth/material parity remains TD-036.
        const size_t group=(draw.primitive==0xA8 || draw.primitive==0xB0) ? 2 : 3;
        const bool indexed=draw.state->descriptors[0]!=gx::Mode::none;
        DrawGpu info; info.indexed=indexed; info.current=draw.transforms->current;
        info.key=pipeline_key(draw); info.choice=pipeline_for(info.key);
        if (draw.mesh) {
            GpuMesh& mesh=gpu_mesh(draw.mesh);   // uploaded and validated on first use
            if (mesh.index_count % group != 0) throw std::invalid_argument("index count does not match native draw topology");
            gx::validate_draw_rows(*draw.transforms,indexed,mesh.rows,draw.callsite);
            if (slot.mesh_refs.empty() || slot.mesh_refs.back()!=draw.mesh) slot.mesh_refs.push_back(draw.mesh);
            info.vertices=info.indices=&mesh.slab->buffer; info.vertex_offset=mesh.vertex_offset; info.index_offset=mesh.index_offset; info.index_count=mesh.index_count; info.mesh_id=mesh.id; info.box=&mesh.box;
        } else {
            const auto vertices = draw.vertex_view(); const auto indices = draw.index_view();
            if (indices.size() % group != 0) throw std::invalid_argument("index count does not match native draw topology");
            for (uint32_t index : indices) if (index >= vertices.size()) throw std::invalid_argument("draw index exceeds owned vertices");
            if (vertices.size() > size_t(INT32_MAX) - vertex_count || indices.size() > size_t(UINT32_MAX) - index_count) throw std::invalid_argument("frame exceeds indexed draw limits");
            gx::validate_draw_rows(*draw.transforms,indexed,gx::used_rows(vertices),draw.callsite);
            info.index_count=uint32_t(indices.size()); info.first_index=uint32_t(index_count); info.first_vertex=int32_t(vertex_count);
            vertex_count += vertices.size(); index_count += indices.size();
        }
        const auto [palette,inserted]=palette_of.try_emplace(draw.transforms.block().get(),uint32_t(palette_blocks.size()));
        if (inserted) palette_blocks.push_back(palette->first);
        info.palette=palette->second;
        static const bool no_banks=std::getenv("UC_NO_TMI")!=nullptr;   // debug: ignore per-vertex texture matrix ids (A/B against the old behaviour)
        if (!no_banks && gx::has_texture_matrix_index(*draw.state)) {
            if (!draw.texture_bank) throw std::invalid_argument("native draw has per-vertex texture matrix indices but no texture bank");
            const auto [bank,added]=bank_of.try_emplace(draw.texture_bank.get(),uint32_t(banks.size()));
            if (added) banks.push_back(bank->first);
            info.bank=bank->second;
        }
        if (const size_t d=gpu_draws.size(); d<scene_draws) {   // scene draw: last frame's matrices of the same draw for its motion vectors
            const auto key=vectors ? tracker_.key(draw) : gx::DrawKey{};
            if(vectors && motion_write_mask(info.key)) {
                MotionBlock block{};
                if (const auto* previous=tracker_.previous(key)) {
                    block.kind=1; block.prev_rows=previous->transforms->projection;
                    const auto [palette,inserted]=palette_of.try_emplace(previous->transforms.get(),uint32_t(palette_blocks.size()));
                    if (inserted) palette_blocks.push_back(palette->first);
                    block.prev_palette=palette->second;
                } else { block.kind=2; block.prev_rows=tracker_.clip_to_prev_clip(); }
                draw_motion[d]=uint32_t(motion_blocks.size()); motion_blocks.push_back(block);
            }
            if (rt_records && rt_material_write(info.key)) {
                const auto& uniforms=*draw.uniforms;
                const auto lighting=gx::channel_lighting(uniforms.channels[0]);
                const uint32_t index=first_record+uint32_t(records.size());
                const auto second=gx::channel_lighting(uniforms.channels[1]);
                const float reflectivity=rt_reflectivity({uniforms.materials[1][0],uniforms.materials[1][1],uniforms.materials[1][2]},second.lit && second.attenuation==1);   // the game's gloss: the specular material colour
                records.push_back({uniforms.materials[0],uniforms.ambient[0],lighting.mask,index*8,rt_reflect_pack(reflectivity,rt_record_flags(lighting,light_model_valid(draw.material,lighting,second))),0.f});
                record_uniforms.push_back(&uniforms);
                draw_record[d]=index;
            }
            if (vectors) tracker_.record(key,draw.transforms.block(),summary.draws[d].z_write);
            if (summary.draws[d].perspective) scene_projection_=draw.transforms->projection;
            if (summary.draws[d].perspective && summary.draws[d].z_write && !frame_projection_) {   // the world's projection: the first depth-writing perspective draw (the weapon comes later with its own)
                frame_projection_=draw.transforms->projection;
#ifdef UC_OPENXR
                if (xr_) xr_->observe(draw.transforms->projection);   // HOTD4's lens tally: the screen plane matches the game's field of view
#endif
                if (preview_) { preview_->observe(gl_from_vulkan(draw.transforms->projection), LG_STEREO_WORLD); preview_lens_seen_ = true; }
                // The map uses last frame's dominant light (stable by its hysteresis), so the casters can be drawn as the scene packets arrive.
                if (effects_.shadows!=EffectLevel::off && dominant_light_ && dominant_light_->kind==gx::LightKind::directional)
                    shadow_fit_=fit_shadow(*frame_projection_,*dominant_light_,kShadowDistance,shadow_preset(effects_.shadows).resolution);
            }
        }
        gpu_draws.push_back(info);
    }
    static const bool depth_stats=gx::depth_switch("UC_DEPTH_STATS");
    if (depth_stats) log_depth_stats(packet,scene_draws,[&](size_t d) { return gpu_draws[d].index_count; });
    if (vertex_count && index_count) {
        if (slot.vertices.size < VkDeviceSize(vertex_count) * sizeof(gx::GpuMeshVertex)) {
            destroy_buffer(slot.vertices); slot.vertices = make_buffer(VkDeviceSize(vertex_count) * sizeof(gx::GpuMeshVertex), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT|vertex_usage());
        }
        if (slot.indices.size < VkDeviceSize(index_count) * sizeof(uint32_t)) {
            destroy_buffer(slot.indices); slot.indices = make_buffer(VkDeviceSize(index_count) * sizeof(uint32_t), VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        }
        for (size_t d=0; d<packet.draws.size(); ++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            if (info.vertices) continue;   // persistent mesh buffer
            const auto vertices = draw.vertex_view(); const auto indices = draw.index_view();
            auto* target=static_cast<gx::GpuMeshVertex*>(slot.vertices.mapped)+info.first_vertex;
            for (size_t i=0;i<vertices.size();++i) target[i]=gx::mesh_vertex(vertices[i]);
            if (draw.wake && aim_history_ && wake_seconds_.load()>0) gx::retarget_wake(draw,target,vertices.size(),wake_seconds_.load(),aim_history_);
            if (!indices.empty()) std::memcpy(static_cast<uint32_t*>(slot.indices.mapped) + info.first_index, indices.data(), indices.size() * sizeof(uint32_t));
        }
    }
    if (!palette_blocks.empty()) {
        const VkDeviceSize palette_bytes=VkDeviceSize(palette_blocks.size())*sizeof(gx::PaletteBlock);
        if (slot.palettes.size < palette_bytes) { destroy_buffer(slot.palettes); slot.palettes = make_buffer(palette_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|rt_input_usage()); }
        auto* palettes=static_cast<gx::PaletteBlock*>(slot.palettes.mapped);
        for (size_t i=0;i<palette_blocks.size();++i) gx::fill_palette(palettes[i], *palette_blocks[i]);
    }
    {   // the descriptor always has a bank bound; a frame without per-vertex texture matrices binds one unused entry
        const VkDeviceSize bank_bytes=VkDeviceSize(std::max<size_t>(banks.size(),1))*sizeof(gx::TextureBank);
        if (slot.banks.size < bank_bytes) { destroy_buffer(slot.banks); slot.banks = make_buffer(bank_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
        for (size_t i=0;i<banks.size();++i) std::memcpy(static_cast<gx::TextureBank*>(slot.banks.mapped)+i,banks[i],sizeof(gx::TextureBank));
    }
    {   // bound even when no draw reads it
        const VkDeviceSize motion_bytes=layers_>1 ? VkDeviceSize(std::max<size_t>(packet.draws.size(),1))*stereo_stride_ : VkDeviceSize(std::max<size_t>(motion_blocks.size(),1))*sizeof(MotionBlock);
        if (slot.motions.size < motion_bytes) { destroy_buffer(slot.motions); slot.motions = make_buffer(motion_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT); }
        if (!motion_blocks.empty()) std::memcpy(slot.motions.mapped, motion_blocks.data(), motion_blocks.size()*sizeof(MotionBlock));
    }
    if (!records.empty()) {   // the frame's record and light arrays: grow-only per frame-in-flight slot
        constexpr VkDeviceSize kLightsBytes=8*sizeof(gx::LightUniform);
        const auto grow=[&](Buffer& buffer, VkDeviceSize used, VkDeviceSize needed) {
            if (buffer.size>=needed) return;
            Buffer bigger=make_buffer(std::max(needed,buffer.size*2),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
            if (used) std::memcpy(bigger.mapped,buffer.mapped,size_t(used));   // the frame's earlier packets (no GPU work reads these buffers before the boundary)
            destroy_buffer(buffer); buffer=bigger;
        };
        Buffer& record_buffer=rt_records_[rt_record_slot_]; Buffer& light_buffer=rt_lights_[rt_record_slot_];
        const VkDeviceSize total=first_record+records.size();
        grow(record_buffer,first_record*sizeof(RtMaterialRecord),total*sizeof(RtMaterialRecord));
        grow(light_buffer,first_record*kLightsBytes,total*kLightsBytes);
        std::memcpy(static_cast<RtMaterialRecord*>(record_buffer.mapped)+first_record,records.data(),records.size()*sizeof(RtMaterialRecord));
        auto* lights=static_cast<std::byte*>(light_buffer.mapped)+first_record*kLightsBytes;
        for (size_t i=0;i<record_uniforms.size();++i) std::memcpy(lights+i*kLightsBytes,record_uniforms[i]->lights.data(),kLightsBytes);
        rt_record_count_=uint32_t(total);
    }
    // The texture coordinates draw i's stages read (sampling stages and indirect maps): the vertex stage generates only those.
    // Key words 1..: stage count, swap tables, then per stage colour/alpha env, order, konst; order bit 6 samples a texture
    // at coordinate (order >> 3) & 7; an indirect stage also reads its indirect map's coordinate (IREF, indirect[9]).
    const auto coord_mask=[&](size_t i) {
        const auto& key=gpu_draws[i].key; const auto& u=*packet.draws[i].uniforms;
        uint32_t coords=key[1]==0 ? 1u : 0u;
        for(uint32_t stage=0;stage<std::min<uint32_t>(key[1],16);++stage) {
            const uint32_t order=key[5+4*stage];
            if(order&64) coords|=1u<<((order>>3)&7);
            if(u.ind_cmd[stage]) coords|=1u<<((u.indirect[9]>>((u.ind_cmd[stage]&3)*6+3))&7);
        }
        return coords;
    };
    // The vertex pre-pass (xform_): where each draw's shaded vertices go. Positions: clip and view float4s a vertex; attributes: 2 + 3 per used
    // coordinate words a vertex. A draw that draws nothing gets no job.
    // The draws' indices are rebased onto the positions, in draw order, into one index buffer (first: the draw's first index there): a
    // run of draws that share their state is then one draw.
    struct XformDraw { uint32_t positions=0, attributes=0, word=0, count=0, first=0; };   // word: stride | mask << 8 (the draws' currentRow)
    std::vector<XformDraw> xform_draws;
    std::vector<uint32_t> draw_uniform(packet.draws.size(),0);   // each draw's uniform block index (offset / uniform_stride_)
    // The indirect TEV math must stay in guest texels at every render scale.
    // GetDimensions sees the enlarged EFB image; Dolphin likewise keeps native
    // texture dimensions separately (TextureCacheBase::BindTextures).
    std::vector<CopyDimensions> copy_dimensions(packet.draws.size());
    std::vector<bool> screen_refraction(packet.draws.size(),false);
    static const bool original_refraction=std::getenv("UC_VR_SCREEN_REFRACTION")!=nullptr;   // diagnostic: original flat-screen displacement
    if (layers_>1 && !original_refraction) for (size_t d=0;d<packet.draws.size();++d)
        screen_refraction[d]=gx::vr_screen_refraction(packet.draws[d]);
    static const bool physical_indirect=std::getenv("UC_INDIRECT_PHYSICAL_SIZE")!=nullptr;   // diagnostic old behaviour
    if (layers_>1 && !physical_indirect) for (size_t d=0;d<packet.draws.size();++d)
        for (size_t map=0;map<8;++map) if (const auto& texture=draw_texture(packet.draws[d],map); texture && texture->gpu_only && !texture->levels.empty())
            copy_dimensions[d][map]={texture->levels[0].width,texture->levels[0].height};
    // Supply imageSize's remaining maps from the actual bound images below.
    // Only indirect stages read sizes: ordinary direct textures must not split
    // fragment-uniform merging just because their dimensions differ.
    std::vector<uint32_t> size_maps(packet.draws.size());
    if (layers_>1) for (size_t d=0;d<packet.draws.size();++d)
        size_maps[d]=gx::indirect_size_maps(packet.draws[d],screen_refraction[d]);
    std::vector<std::array<uint64_t,17>> set_keys(layers_>1 ? packet.draws.size() : 0);   // per draw: image views, samplers, uniform offset
    // The headset: one descriptor set per distinct texture/sampler combination (the uniform block and the eye block are dynamic offsets)
    std::map<std::array<uint64_t,16>,uint32_t> set_of;
    std::vector<uint32_t> draw_set(layers_>1 ? packet.draws.size() : 0,0), draw_uniform_offset(layers_>1 ? packet.draws.size() : 0,0);
    // The texture table (tex_merge_): each distinct set's maps 0..highest used, in set order, as combined image samplers; set_base: a set's first
    // entry. table_ok: the packet's table fits (else every draw keeps its own set, as without the table).
    std::vector<VkDescriptorImageInfo> table_entries; std::vector<uint32_t> set_base; bool table_ok=false;
    // The small-draw ubershader (uber_merge.h): the pre-pass writes a vertex map after the attributes, two words a position (the vertex's
    // first attribute word; its draw's uniform block index | coordinate mask << 24). merge_map: where it starts, ~0u: no map.
    const bool uber_small=uber_small_scene_ || uber_small_hud_;
    uint32_t merge_map=~0u;
    if (xform_ && !packet.draws.empty()) {
        xform_draws.resize(packet.draws.size());
        uint32_t positions=0, attributes=0, indices=0;
        for (size_t i=0;i<packet.draws.size();++i) {
            if (!gpu_draws[i].index_count) continue;
            const auto& draw=packet.draws[i];
            const uint32_t n=uint32_t(draw.vertex_view().size());
            const uint32_t mask=coord_mask(i), stride=2+3*uint32_t(std::popcount(mask))+(tex_merge_ ? 1u : 0u);   // the texture table: one more word, the draw's first entry
            xform_draws[i]={positions,attributes,stride|(mask<<8),n,indices};
            positions+=n; attributes+=n*stride; indices+=gpu_draws[i].index_count;
        }
        const auto grow=[&](Buffer& buffer,VkDeviceSize bytes) {
            bytes=std::max<VkDeviceSize>(bytes,16);
            if (buffer.size>=bytes) return;
            destroy_buffer(buffer); buffer=make_buffer(bytes+bytes/2,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
        };
        if (uber_small) merge_map=attributes;
        grow(slot.xform_positions,VkDeviceSize(positions)*32); grow(slot.xform_attributes,(VkDeviceSize(attributes)+(uber_small ? 2*VkDeviceSize(positions) : 0))*4);
        if (slot.xform_indices.size<VkDeviceSize(indices)*4) { destroy_buffer(slot.xform_indices); slot.xform_indices=make_buffer(std::max<VkDeviceSize>(indices,16)*6,VK_BUFFER_USAGE_INDEX_BUFFER_BIT,true); }
        auto* out=static_cast<uint32_t*>(slot.xform_indices.mapped);
        for (size_t i=0;i<packet.draws.size();++i) {
            if (!gpu_draws[i].index_count) continue;
            const uint32_t base=xform_draws[i].positions; uint32_t* at=out+xform_draws[i].first;
            for (const uint32_t index:packet.draws[i].index_view()) *at++=base+index;
        }
    }
    std::vector<Texture*> copy_mips;   // EFB copies this packet samples with a mip filter whose levels 1.. are still to be made
    if (!packet.draws.empty()) {
        if (packet.draws.size()>UINT32_MAX) throw std::invalid_argument("too many native texture descriptors");
        if(packet.draws.size()>UINT32_MAX/8) throw std::invalid_argument("native texture descriptor count overflows");
        const uint32_t count=uint32_t(packet.draws.size());
        if(slot.uniforms.size<VkDeviceSize(count)*uniform_stride_) { destroy_buffer(slot.uniforms); slot.uniforms=make_buffer(VkDeviceSize(count)*uniform_stride_,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|(xform_ ? VK_BUFFER_USAGE_STORAGE_BUFFER_BIT : 0u)); }
        if(count>slot.descriptor_capacity) {
            if(slot.texture_pool) vkDestroyDescriptorPool(device,slot.texture_pool,nullptr);
            slot.texture_pool=VK_NULL_HANDLE; slot.descriptor_capacity=0; slot.texture_sets.clear();
            VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,count*8},{VK_DESCRIPTOR_TYPE_SAMPLER,count*8},{layers_>1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,layers_>1 ? count*2 : count},{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,layers_>1 ? count*(xform_ ? 4 : 2) : count*3}};
            VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=count; pool.poolSizeCount=4; pool.pPoolSizes=sizes;
            vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&slot.texture_pool),"texture descriptor pool");
            std::vector<VkDescriptorSetLayout> layouts(count,texture_layout_);
            slot.texture_sets.resize(count);
            VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=slot.texture_pool; allocate.descriptorSetCount=count; allocate.pSetLayouts=layouts.data();
            vk_check(vkAllocateDescriptorSets(device,&allocate,slot.texture_sets.data()),"texture descriptor sets");
            slot.descriptor_capacity=count; ++descriptor_pool_creations_;
        }
        // The slot fence has retired; rewrite its retained sets before their next submission.
        slot.textures.reserve(count);
        std::unordered_map<const gx::TexturePixels*,size_t> texture_indices;
        for(size_t i=0;i<slot.textures.size();++i) texture_indices.emplace(slot.textures[i]->source.get(),i);
        VkDeviceSize uniform_offset=0;
        for (size_t i=0;i<count;++i) {
            std::array<VkDescriptorImageInfo,8> image{};
            std::array<bool,8> generated_mips{};   // the map's texture has GPU-made mips (auto_samplers_)
            for(size_t map=0;map<8;++map) {
                const auto set_size=[&](VkExtent2D extent) {
                    if ((size_maps[i]&(1u<<map)) && !copy_dimensions[i][map][0])
                        copy_dimensions[i][map]={extent.width,extent.height};
                };
                const auto* chosen=&draw_texture(packet.draws[i],map);
                if(*chosen)if(const auto alias=copy_aliases_.find(chosen->get());alias!=copy_aliases_.end() && !alias->second.requested.expired())chosen=&alias->second.rendered;
                if(*chosen && (*chosen)->gpu_only) {
                    auto copied=copy_textures_.find(chosen->get());
                    if(copied!=copy_textures_.end() && copied->second.initialized) {
                        static const bool diag=std::getenv("UC_DIAG_COPY")!=nullptr;   // diagnostics: 3D draws sampling an EFB copy, once per callsite
                        if(diag) {
                            static std::set<uint64_t> seen;
                            const auto& d=packet.draws[i]; const auto& u=*d.uniforms;
                            if(seen.insert((uint64_t(d.callsite)<<4)|map|(uint64_t(d.transforms->projection[3][3]==0)<<36)).second && seen.size()<64) {
                                std::string gens; char b[160];
                                for(int g=0;g<8;++g) { std::snprintf(b,sizeof b," g%d src%u proj%u",g,(u.tex_gens[g]>>7)&31,(u.tex_gens[g]>>1)&1); gens+=b; }
                                bool indirect=false; for(auto c:u.ind_cmd) indirect|=c!=0;
                                std::fprintf(stderr,"[diag-copy] callsite %08X %s map %zu copy %ux%u ind %d blend %u z test %u write %u cmp %u |%s\n",d.callsite,d.transforms->projection[3][3]==0 ? "persp" : "ortho",map,
                                             (*chosen)->levels[0].width,(*chosen)->levels[0].height,int(indirect),unsigned(d.blend.mode),gpu_draws[i].key[68]&1,(gpu_draws[i].key[68]>>4)&1,(gpu_draws[i].key[68]>>1)&7,gens.c_str());
                                for(int g=0;g<2;++g) for(int r=0;r<3;++r) { const auto& row=u.tex_rows[g*3+r]; std::fprintf(stderr,"[diag-copy]   g%d row%d %.4f %.4f %.4f %.4f\n",g,r,row[0],row[1],row[2],row[3]); }
                                for(int r=0;r<4;++r) { const auto& row=d.transforms->projection[r]; std::fprintf(stderr,"[diag-copy]   P row%d %.4f %.4f %.4f %.4f\n",r,row[0],row[1],row[2],row[3]); }
                            }
                        }
                        slot.copy_sources.push_back(*chosen);
                        set_size(copied->second.extent);
                        image[map]={VK_NULL_HANDLE,copied->second.view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        if(copied->second.mips_pending && ((packet.draws[i].texture_wrap[map]>>5)&3) && std::find(copy_mips.begin(),copy_mips.end(),&copied->second)==copy_mips.end())
                            copy_mips.push_back(&copied->second);   // a mip filter (maxLod above 0): the levels are made before this packet's passes
                        continue;
                    }
                    if(!lost_copies_.contains(chosen->get())) throw std::invalid_argument("native EFB texture used before its copy");
                    chosen=&blank_;
                }
                const auto& source=*chosen;
                auto found=texture_indices.find(source.get());
                if (found==texture_indices.end()) {
                    std::shared_ptr<Texture> shared;
                    if(const auto pooled=texture_pool_.find(source.get()); pooled!=texture_pool_.end()) shared=pooled->second.lock();
                    if(!shared) {
                        auto texture=make_texture(source.get()); texture.source=source;
                        shared=std::shared_ptr<Texture>(new Texture(std::move(texture)),[this](Texture* dead) { texture_pool_.erase(dead->source.get()); destroy_texture(*dead); delete dead; });
                        texture_pool_[source.get()]=shared;
                    }
                    slot.textures.push_back(std::move(shared));
                    found=texture_indices.emplace(source.get(),slot.textures.size()-1).first;
                }
                image[map]={VK_NULL_HANDLE,slot.textures[found->second]->view,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                set_size(slot.textures[found->second]->extent);
                generated_mips[map]=slot.textures[found->second]->auto_mips;
            }
            if(i==0 || packet.draws[i].uniforms.block()!=packet.draws[i-1].uniforms.block() ||
               copy_dimensions[i]!=copy_dimensions[i-1] ||
               screen_refraction[i]!=screen_refraction[i-1] ||
               !std::equal(gpu_draws[i].key.begin()+1,gpu_draws[i].key.begin()+68,gpu_draws[i-1].key.begin()+1)) {
                uniform_offset=i*uniform_stride_;
                auto* uniform_at=static_cast<std::byte*>(slot.uniforms.mapped)+uniform_offset;
                std::memcpy(uniform_at,&*packet.draws[i].uniforms,sizeof(gx::DrawUniform));
                if (screen_refraction[i]) {
                    const uint32_t direct=gx::effective_indirect_command(*packet.draws[i].uniforms,0,true);
                    std::memcpy(uniform_at+offsetof(gx::DrawUniform,ind_cmd),&direct,sizeof direct);
                }
                std::array<uint32_t,kTevWords> tev{}; std::copy(gpu_draws[i].key.begin()+1,gpu_draws[i].key.begin()+68,tev.begin());
                std::memcpy(uniform_at+sizeof(gx::DrawUniform),tev.data(),sizeof tev);
                std::memcpy(uniform_at+kCopyDimensionsOffset,copy_dimensions[i].data(),sizeof(CopyDimensions));
                // The texture coordinates the stages read (fog[5], a spare word; bit 8: valid): the vertex shader generates only those.
                const uint32_t coord_word=coord_mask(i)|0x100u;
                std::memcpy(uniform_at+offsetof(gx::DrawUniform,fog)+5*sizeof(uint32_t),&coord_word,sizeof coord_word);
            }
            VkDescriptorBufferInfo uniform{slot.uniforms.buffer,uniform_offset,kDrawUniformBytes};
            std::array<VkDescriptorImageInfo,8> samplers{};
            for(size_t map=0;map<8;++map) {
                const auto wrap=packet.draws[i].texture_wrap[map];
                if(!texture_samplers_[wrap]) throw std::invalid_argument("invalid native texture sampler mode");
                const VkSampler plain=generated_mips[map] && auto_samplers_[wrap] ? auto_samplers_[wrap] : texture_samplers_[wrap];
                samplers[map]={i<scene_draws ? scene_samplers_[wrap] : plain,VK_NULL_HANDLE,VK_IMAGE_LAYOUT_UNDEFINED};
            }
            VkDescriptorBufferInfo palette{slot.palettes.buffer,0,VK_WHOLE_SIZE};
            VkDescriptorBufferInfo bank{slot.banks.buffer,0,VK_WHOLE_SIZE};
            VkDescriptorBufferInfo motion{slot.motions.buffer,0,VK_WHOLE_SIZE};
            if (layers_>1) motion={slot.motions.buffer,VkDeviceSize(i)*stereo_stride_,kStereoBlockBytes};   // this draw's eye block
            draw_uniform[i]=uint32_t(uniform_offset/uniform_stride_);
            if (layers_>1) {   // what the draw's set holds besides its uniform block and eye block (dynamic offsets): draws with the same share one set
                auto& key=set_keys[i];
                // only the maps the draw's TEV stages sample (and their indirect maps) count: the others are never read
                const auto& pk=gpu_draws[i].key; const auto& u=*packet.draws[i].uniforms;
                uint32_t maps=pk[1]==0 ? 1u : 0u;
                for (uint32_t stage=0;stage<std::min<uint32_t>(pk[1],16);++stage) {
                    const uint32_t order=pk[5+4*stage];
                    if (order&64) maps|=1u<<(order&7);
                    if (u.ind_cmd[stage]) maps|=1u<<((u.indirect[9]>>((u.ind_cmd[stage]&3)*6))&7);
                }
                for (size_t map=0;map<8;++map) { const bool used=(maps>>map)&1; key[map]=used ? uint64_t(image[map].imageView) : 0; key[8+map]=used ? uint64_t(samplers[map].sampler) : 0; }
                key[16]=uniform_offset;
                draw_uniform_offset[i]=uint32_t(uniform_offset);
                std::array<uint64_t,16> textures; std::copy(key.begin(),key.begin()+16,textures.begin());
                const auto [found,added]=set_of.try_emplace(textures,uint32_t(set_of.size()));
                draw_set[i]=found->second;
                if (!added) continue;   // that set is written
                if (tex_merge_) {   // the new set's maps in the table: 0 .. the highest one its draws read (the shaders read no other)
                    set_base.push_back(uint32_t(table_entries.size()));
                    for (uint32_t map=0;map<uint32_t(std::bit_width(maps));++map) table_entries.push_back({samplers[map].sampler,image[map].imageView,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL});
                }
                uniform={slot.uniforms.buffer,0,kDrawUniformBytes};
                motion={slot.motions.buffer,0,kStereoBlockBytes};
            }
            const VkDescriptorBufferInfo xform_positions{slot.xform_positions.buffer,0,VK_WHOLE_SIZE}, xform_attributes{slot.xform_attributes.buffer,0,VK_WHOLE_SIZE};
            VkWriteDescriptorSet writes[8]{};
            const uint32_t binding_count=xform_ ? 8 : 6;
            const VkDescriptorSet target_set=slot.texture_sets[layers_>1 ? draw_set[i] : i];
            for (uint32_t binding=0;binding<binding_count;++binding) { writes[binding].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; writes[binding].dstSet=target_set; writes[binding].dstBinding=binding; writes[binding].descriptorCount=1; }
            writes[0].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; writes[0].descriptorCount=8; writes[0].pImageInfo=image.data();
            writes[1].descriptorType=VK_DESCRIPTOR_TYPE_SAMPLER; writes[1].descriptorCount=8; writes[1].pImageInfo=samplers.data();
            writes[2].descriptorType=layers_>1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[2].pBufferInfo=&uniform;
            writes[3].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[3].pBufferInfo=&palette;
            writes[4].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[4].pBufferInfo=&bank;
            writes[5].descriptorType=layers_>1 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[5].pBufferInfo=&motion;
            writes[6].descriptorType=writes[7].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[6].pBufferInfo=&xform_positions; writes[7].pBufferInfo=&xform_attributes;
            vkUpdateDescriptorSets(device,binding_count,writes,0,nullptr);
        }
        if (tex_merge_ && !table_entries.empty()) {   // the packet's table: one set per slot, its pool grown to the largest packet so far
            table_ok=table_entries.size()<=table_limit_;
            if (!table_ok) ++table_overflows_;
            if (table_ok && table_entries.size()>slot.table_capacity) {
                if (slot.table_pool) vkDestroyDescriptorPool(device,slot.table_pool,nullptr);
                slot.table_pool=VK_NULL_HANDLE; slot.table_set=VK_NULL_HANDLE; slot.table_capacity=0;
                const uint32_t capacity=std::min<uint32_t>(table_limit_,std::max<uint32_t>(256,uint32_t(table_entries.size()+table_entries.size()/2)));
                // The layout's whole array (see table_limit_), twice over and also as separate images and samplers: the Quest's driver refused a
                // pool of exactly the variable count (VK_ERROR_OUT_OF_POOL_MEMORY); a combined image sampler may count against both budgets there.
                const VkDescriptorPoolSize sizes[3]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2*table_limit_},{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,table_limit_},{VK_DESCRIPTOR_TYPE_SAMPLER,table_limit_}};
                VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=3; pool.pPoolSizes=sizes;
                if (table_update_after_bind_) pool.flags=VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
                vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&slot.table_pool),"texture table pool");
                VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=slot.table_pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&table_set_layout_;
                vk_check(vkAllocateDescriptorSets(device,&allocate,&slot.table_set),"texture table set");
                slot.table_capacity=capacity;
            }
            if (table_ok) {
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=slot.table_set; write.dstBinding=0; write.descriptorCount=uint32_t(table_entries.size());
                write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; write.pImageInfo=table_entries.data();
                vkUpdateDescriptorSets(device,1,&write,0,nullptr);
                ++table_packets_; table_entries_+=table_entries.size();
            }
            if (table_ok && uber_small && merge_layout_) {   // the small-draw ubershader's set 2: every uniform block of the packet, indexed per draw
                if (!slot.merge_pool) {
                    const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,1};
                    VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&size;
                    vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&slot.merge_pool),"small-draw ubershader pool");
                    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=slot.merge_pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&merge_set_layout_;
                    vk_check(vkAllocateDescriptorSets(device,&allocate,&slot.merge_set),"small-draw ubershader set");
                }
                const VkDescriptorBufferInfo blocks{slot.uniforms.buffer,0,VK_WHOLE_SIZE};
                const VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,slot.merge_set,0,0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&blocks,nullptr};
                vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            }
        }
    }
    uint32_t image_index = 0;
    bool suboptimal=false;
    PresentImage* present_image=nullptr;
#ifdef UC_OPENXR
    if (xr_) {   // the headset's frame opens with the game frame's first packet (HOTD4): every pass of it uses this frame's eye poses
        if (display && !xr_->frame_open()) { xr_frame_ = xr_->begin_frame(); world_drawn_ = false; xr_->scene().set_hud_fit(!menu_context_.load(std::memory_order_relaxed)); }
    } else
#endif
    {
        if (preview_ && display && !preview_frame_open_) {   // as the headset's frame opens
            preview_publish(); preview_frame_open_ = true; world_drawn_ = false;
            preview_->set_hud_fit(!menu_context_.load(std::memory_order_relaxed));
        }
    }
    if (!xr_ && packet.present && display && !hold) {
        const bool legacy_retirement=!present_fences_ && !retired_.empty();
        if(legacy_retirement && !retirement_acquired_) {
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vk_check(vkCreateFence(device,&fence,nullptr,&retirement_acquired_),"legacy acquire fence");
        }
        const auto acquire_start = std::chrono::steady_clock::now();
        VkResult acquired = present_.acquire(device, swapchain_, 1000000ull, slot.acquired, legacy_retirement ? retirement_acquired_ : VK_NULL_HANDLE, &image_index);
        timings_.acquire_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - acquire_start).count();
        if (acquired == VK_TIMEOUT || acquired == VK_NOT_READY) {   // retried later: the packet must look unseen to the per-frame upscaling state
            boundary_=upscale_before.boundary; frame_start_=upscale_before.frame_start; scene_phase_=upscale_before.scene_phase; scene_mode_=upscale_before.scene_mode;
            motion_cleared_=upscale_before.motion_cleared; gbuffer_cleared_=upscale_before.gbuffer_cleared; reset_next_=upscale_before.reset_next; jitter_=upscale_before.jitter; whole_frame_upscales_=upscale_before.whole_frame_upscales; frame_projection_=upscale_before.frame_projection; shadow_fit_=upscale_before.shadow_fit; shadow_cleared_=upscale_before.shadow_cleared;
            rt_material_cleared_=upscale_before.rt_material_cleared; rt_record_slot_=upscale_before.rt_record_slot; rt_record_count_=upscale_before.rt_record_count;
            tracker_.rollback(upscale_before.tracker);
            frame_reticle_.resize(reticle_before);
            return false;
        }
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) resized_ = true;   // stale swapchain while the window is resized: the frame is rendered but not shown, the guest must not wait for the rebuild
        else {
            if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) vk_check(acquired, "acquire image");
            suboptimal = acquired == VK_SUBOPTIMAL_KHR;
            present_image = &images_.at(image_index);
            if(legacy_retirement) {
                vk_check(vkWaitForFences(device,1,&retirement_acquired_,VK_TRUE,1000000000ull),"legacy successor acquire");
                vk_check(vkResetFences(device,1,&retirement_acquired_),"reset legacy acquire fence");
                if(present_image->pending)reap_retired(true);   // this successor image was presented, and the presentation engine returned it
            }
            if (present_fences_ && present_image->pending) {
                vk_check(vkWaitForFences(device, 1, &present_image->fence, VK_TRUE, 1000000000ull), "reuse presentation fence");
                present_image->pending = false;
            }
            if(present_fences_)vk_check(vkResetFences(device, 1, &present_image->fence), "reset present fence");
        }
    }
    vk_check(vkResetCommandBuffer(slot.command, 0), "reset command buffer");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk_check(vkBeginCommandBuffer(slot.command, &begin), "begin frame");
    const uint32_t first_query = uint32_t((&slot - slots_.data()) * 2);
    vkCmdResetQueryPool(slot.command, timing_pool_, first_query, 2);
    vkCmdWriteTimestamp2(slot.command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timing_pool_, first_query);
    pass_timer_.reset(slot.command, first_query / 2);
    slot.timed = true;
    {   // UC_FRAG_DUMP=N: from the Nth scene packet (200+ draws) to that frame's present, every draw counted with an exact occlusion
        // query (fragments passing the depth test: shaded), printed per packet after a wait for the GPU. Diagnostics only.
        static const long dump_at = std::getenv("UC_FRAG_DUMP") ? std::atol(std::getenv("UC_FRAG_DUMP")) : -1;
        static long big = 0; static bool dumping = false;
        if (dump_at > 0 && !dumping && packet.draws.size() >= 200 && ++big == dump_at) dumping = true;
        frag_dump_ = dumping;
        if (dumping && packet.present) dumping = false;   // this packet is the last
        if (frag_dump_) {
            if (!frag_pool_) { VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; info.queryType = VK_QUERY_TYPE_OCCLUSION; info.queryCount = kFragQueries; vk_check(vkCreateQueryPool(device, &info, nullptr, &frag_pool_), "fragment query pool"); }
            vkCmdResetQueryPool(slot.command, frag_pool_, 0, kFragQueries);
            frag_queries_.clear();
        }
    }
    static const bool by_kind = std::getenv("UC_GPU_PACKETS") != nullptr;
    if (by_kind) {
        const size_t n = packet.draws.size();
        const char* bucket = n == 0 ? "0" : n < 10 ? "1-9" : n < 50 ? "10-49" : n < 200 ? "50-199" : "200+";
        if (n >= 200 && !(packet.texture_copy && packet.texture_copy->source[2] == 640)) {   // the other big packets: what they draw
            static uint64_t packets = 0, drawn = 0, indices = 0, perspective = 0; static double area = 0; static auto since = std::chrono::steady_clock::now();
            for (size_t d = 0; d < gpu_draws.size(); ++d) {
                if (!gpu_draws[d].index_count) continue;
                ++drawn; indices += gpu_draws[d].index_count;
                if (packet.draws[d].transforms->projection[3][3] == 0) ++perspective;
                else if (const auto b = gx::draw_bounds_ndc(packet.draws[d])) area += std::max(0.f, std::clamp((*b)[2], -1.f, 1.f) - std::clamp((*b)[0], -1.f, 1.f)) * std::max(0.f, std::clamp((*b)[3], -1.f, 1.f) - std::clamp((*b)[1], -1.f, 1.f)) / 4;
            }
            ++packets;
            if (std::chrono::steady_clock::now() - since > std::chrono::seconds(2)) {
                const double p = double(packets);
                std::fprintf(stderr, "[gpu-packets] other big packets (%s): %.0f draws, %.0f perspective, %.0f indices, ortho draws cover %.1f viewports (%llu packets)\n",
                             packet.texture_copy ? "copy" : "no copy", drawn / p, perspective / p, indices / p, area / p, (unsigned long long)packets);
                for (size_t d = 0; d < packet.draws.size() && d < 400; d += 97) {   // a few of its draws: GX viewport, scissor, lens, NDC box
                    const auto& draw = packet.draws[d]; const auto v = draw.state->viewport.value_or(std::array<float,6>{});
                    const auto sc = draw.state->scissor.value_or(std::array<int32_t,4>{}); const auto b = gx::draw_bounds_ndc(draw).value_or(std::array<float,4>{});
                    std::fprintf(stderr, "[gpu-packets]   draw %zu callsite %08X viewport %.0f,%.0f %.0fx%.0f scissor %d,%d %dx%d p00 %.3f p11 %.3f box %.2f,%.2f..%.2f,%.2f blend %u copy %u,%u %ux%u\n", d, draw.callsite, v[0], v[1], v[2], v[3],
                                 int(sc[0]), int(sc[1]), int(sc[2]), int(sc[3]), draw.transforms->projection[0][0], draw.transforms->projection[1][1], b[0], b[1], b[2], b[3], unsigned(draw.blend.mode),
                                 packet.texture_copy ? packet.texture_copy->source[0] : 0, packet.texture_copy ? packet.texture_copy->source[1] : 0, packet.texture_copy ? packet.texture_copy->source[2] : 0, packet.texture_copy ? packet.texture_copy->source[3] : 0);
                }
                packets = drawn = indices = perspective = 0; area = 0; since = std::chrono::steady_clock::now();
            }
        }
        if (n >= 200 && packet.texture_copy && packet.texture_copy->source[2] == 640) {   // the main scene packets: their vertex load and state changes, per second
            static uint64_t indices = 0, packets = 0, drawn = 0, vertices = 0, switches = 0, distinct = 0, meshes = 0; static auto since = std::chrono::steady_clock::now();
            static uint64_t uniform_changes = 0, texture_changes = 0, projection_changes = 0, runs = 0;
            std::set<VkPipeline> pipelines; VkPipeline last = VK_NULL_HANDLE; size_t last_drawn = SIZE_MAX;
            for (size_t d = 0; d < gpu_draws.size(); ++d) {
                const auto& info = gpu_draws[d];
                if (!info.index_count) continue;
                indices += info.index_count; ++drawn;
                vertices += packet.draws[d].mesh ? packet.draws[d].mesh->vertices.size() : packet.draws[d].vertex_view().size();
                meshes += packet.draws[d].mesh != nullptr;
                pipelines.insert(info.choice.pipeline); switches += info.choice.pipeline != last; last = info.choice.pipeline;
                if (last_drawn != SIZE_MAX) {
                    const auto& a = packet.draws[d]; const auto& b = packet.draws[last_drawn];
                    uniform_changes += a.uniforms.block() != b.uniforms.block();
                    bool same = a.texture_wrap == b.texture_wrap;
                    for (size_t map = 0; map < 8 && same; ++map) same = draw_texture(a, map) == draw_texture(b, map);
                    texture_changes += !same;
                    projection_changes += a.transforms->projection != b.transforms->projection;
                    // could the two draws be one (the pre-pass's outputs are consecutive): same pipeline, uniform block, textures, viewport and scissor
                    const bool viewports = a.state->viewport == b.state->viewport && a.state->scissor == b.state->scissor;
                    runs += !(same && info.choice.pipeline == gpu_draws[last_drawn].choice.pipeline && a.uniforms.block() == b.uniforms.block() && viewports && a.transforms->projection == b.transforms->projection && !a.reticle && !a.late_aim && !a.layer);
                }
                last_drawn = d;
            }
            distinct += pipelines.size();
            {   // how many pipeline changes would remain if the runs of order-free draws (opaque: no blending, no alpha test, depth tested
                // LESS/LEQUAL and written) were grouped by pipeline: per run, its distinct pipelines
                static uint64_t opaque = 0;
                std::set<VkPipeline> segment; VkPipeline previous = VK_NULL_HANDLE; uint64_t changes = 0;
                for (size_t d = 0; d < gpu_draws.size(); ++d) {
                    const auto& info = gpu_draws[d];
                    if (!info.index_count) continue;
                    const auto& k = info.key; const uint32_t compare = (k[68] >> 1) & 7;
                    const bool free = (k[0] & 3) == 0 && !((k[0] >> 16) & 1) && (k[68] & 1) && ((k[68] >> 4) & 1) && (compare == VK_COMPARE_OP_LESS || compare == VK_COMPARE_OP_LESS_OR_EQUAL) && !(packet.draws[d].uniforms->pixel[2] & 256);
                    static uint64_t why[6]{}; static auto why_since = std::chrono::steady_clock::now();
                    why[0] += (k[0] & 3) != 0; why[1] += (k[0] >> 16) & 1; why[2] += !(k[68] & 1); why[3] += !((k[68] >> 4) & 1); why[4] += !(compare == VK_COMPARE_OP_LESS || compare == VK_COMPARE_OP_LESS_OR_EQUAL); why[5] += (packet.draws[d].uniforms->pixel[2] & 256) != 0;
                    if (std::chrono::steady_clock::now() - why_since > std::chrono::seconds(2)) {
                        std::fprintf(stderr, "[gpu-packets] scene draws not order-free: blending %llu, alpha test %llu, no depth test %llu, no depth write %llu, compare %llu, destination alpha %llu\n",
                                     (unsigned long long)why[0], (unsigned long long)why[1], (unsigned long long)why[2], (unsigned long long)why[3], (unsigned long long)why[4], (unsigned long long)why[5]);
                        std::memset(why, 0, sizeof why); why_since = std::chrono::steady_clock::now();
                    }
                    if (free) { opaque++; if (!segment.count(info.choice.pipeline)) { segment.insert(info.choice.pipeline); ++changes; } previous = VK_NULL_HANDLE; continue; }
                    segment.clear();
                    changes += info.choice.pipeline != previous; previous = info.choice.pipeline;
                }
                grouped_changes_ += changes; opaque_draws_ += opaque; opaque = 0;
            }
            ++packets;
            if (std::chrono::steady_clock::now() - since > std::chrono::seconds(2)) {
                const double p = double(packets);
                std::fprintf(stderr, "[gpu-packets] scene packets: %.0f order-free (opaque) draws; grouped by pipeline in their runs %.0f pipeline changes would remain\n", opaque_draws_ / p, grouped_changes_ / p);
                opaque_draws_ = grouped_changes_ = 0;
                std::fprintf(stderr, "[gpu-packets] scene packets: %.0f indices each (%llu packets), %.0f draws (%.0f meshes), %.0f vertices, %.0f pipelines, %.0f pipeline changes, %.0f uniform changes, %.0f texture changes, %.0f projection changes, %.0f runs of mergeable draws\n", double(indices) / p, (unsigned long long)packets,
                             drawn / p, meshes / p, vertices / p, distinct / p, switches / p, uniform_changes / p, texture_changes / p, projection_changes / p, (runs + packets) / p);
                indices = packets = drawn = vertices = switches = distinct = meshes = uniform_changes = texture_changes = projection_changes = runs = 0; since = std::chrono::steady_clock::now();
            }
        }
        char kind[128];
        if (packet.texture_copy) std::snprintf(kind, sizeof kind, "copy %ux%u draws %s%s", packet.texture_copy->source[2], packet.texture_copy->source[3], bucket, packet.present ? " +present" : "");
        else std::snprintf(kind, sizeof kind, "%s draws %s", packet.present ? "present" : "no copy", bucket);
        if (packet.texture_copy && copy_target && n < 10) {   // the post chain's packets apart: exact draws, the first draw's callsite, the copy's texture size
            const size_t used = std::strlen(kind);
            std::snprintf(kind + used, sizeof kind - used, " [%zu %08X ->%ux%u]", n, n ? packet.draws[0].callsite : 0u, copy_target->extent.width, copy_target->extent.height);
        }
        slot.timing_kind = kind;
        // An estimate of the fragments this packet shades: each draw's clip-space box (clamped to its viewport) times the viewport's pixels,
        // times the layers (both eyes). Exact for full-screen quads, an upper bound for meshes; per kind, per 2 s.
        static std::map<std::string, double> megapixels; static auto since = std::chrono::steady_clock::now();
        double area = 0;
        for (const auto& draw : packet.draws) {
            const auto box = gx::draw_bounds_ndc(draw);
            if (!box) continue;
            const auto v = draw.state->viewport.value_or(std::array<float,6>{0, 0, 640, 480, 0, 1});
            const double w = std::clamp((*box)[2], -1.f, 1.f) - std::clamp((*box)[0], -1.f, 1.f), h = std::clamp((*box)[3], -1.f, 1.f) - std::clamp((*box)[1], -1.f, 1.f);
            area += std::max(0.0, w) * std::max(0.0, h) / 4 * double(v[2]) * double(v[3]) * (double(efb_extent_.width) / 640) * (double(efb_extent_.height) / 480) * layers_;
        }
        megapixels[kind] += area / 1e6;
        // UC_GPU_DRAWS_DUMP=N: the Nth big (scene) packet's largest draws by that estimate, with what decides their cost on a tiler
        static const long dump_at = std::getenv("UC_GPU_DRAWS_DUMP") ? std::atol(std::getenv("UC_GPU_DRAWS_DUMP")) : -1;
        static long big_packets = 0;
        if (packet.draws.size() >= 200 && ++big_packets == dump_at) {
            std::vector<std::pair<double, size_t>> areas;
            for (size_t d = 0; d < packet.draws.size(); ++d) {
                const auto& draw = packet.draws[d];
                const auto box = gx::draw_bounds_ndc(draw);
                if (!box) continue;
                const auto v = draw.state->viewport.value_or(std::array<float,6>{0, 0, 640, 480, 0, 1});
                const double w = std::clamp((*box)[2], -1.f, 1.f) - std::clamp((*box)[0], -1.f, 1.f), h = std::clamp((*box)[3], -1.f, 1.f) - std::clamp((*box)[1], -1.f, 1.f);
                areas.push_back({std::max(0.0, w) * std::max(0.0, h) / 4 * double(v[2]) * double(v[3]) / (640.0 * 360), d});   // in screens (640x360)
            }
            std::sort(areas.rbegin(), areas.rend());
            double total = 0; for (const auto& a : areas) total += a.first;
            std::fprintf(stderr, "[gpu-draws] scene packet %ld: %zu draws, %.1f screens of box area\n", big_packets, packet.draws.size(), total);
            for (size_t i = 0; i < areas.size() && i < 40; ++i) {
                const auto& draw = packet.draws[areas[i].second];
                const uint32_t alpha = draw.uniforms->pixel[0];
                const bool alpha_test = !(((alpha >> 16) & 7) == 7 && ((alpha >> 19) & 7) == 7);
                const gx::DrawSummary s = areas[i].second < summary.draws.size() ? summary.draws[areas[i].second] : gx::DrawSummary{};
                std::fprintf(stderr, "[gpu-draws] %5.2f screens  callsite %08X  persp %d  blend %u  alpha-test %d  z test %d write %d  verts %zu  tev stages %u\n",
                             areas[i].first, draw.callsite, int(s.perspective), unsigned(draw.blend.mode), int(alpha_test), int(s.z_test), int(s.z_write),
                             draw.mesh ? draw.mesh->indices.size() : draw.indices.size(), unsigned(draw.uniforms->pixel[3] & 15));
            }
        }
        if (std::chrono::steady_clock::now() - since > std::chrono::seconds(2)) {
            std::vector<std::pair<std::string, double>> sorted(megapixels.begin(), megapixels.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            for (size_t i = 0; i < sorted.size() && i < 8; ++i) std::fprintf(stderr, "[gpu-packets] est. %8.1f Mpx / 2 s  %s\n", sorted[i].second, sorted[i].first.c_str());
            megapixels.clear(); since = std::chrono::steady_clock::now();
        }
    }
    for (Texture* copy : copy_mips) record_copy_mips(slot.command, *copy);
    for (const auto& pooled : slot.textures) {
        Texture& texture=*pooled;
        if (texture.initialized) continue;
        texture.uploader=&slot;
        barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_IMAGE_ASPECT_COLOR_BIT,texture.mip_levels);
        const uint32_t uploaded=texture.auto_mips ? 1 : texture.mip_levels;   // generated mips: only level 0 comes from the CPU
        std::vector<VkBufferImageCopy> copies(uploaded); VkDeviceSize at=0;
        for(uint32_t i=0;i<uploaded;++i) {
            const auto width=std::max(1u,texture.extent.width>>i),height=std::max(1u,texture.extent.height>>i);
            copies[i].bufferOffset=at; copies[i].imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1}; copies[i].imageExtent={width,height,1}; at+=texture.bc7 ? gx::bc7_level_size(width,height) : VkDeviceSize(width)*height*4;
        }
        vkCmdCopyBufferToImage(slot.command,texture.upload.buffer,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,uint32_t(copies.size()),copies.data());
        if(texture.auto_mips) {   // each level a linear blit of the one above, as EFB copies make theirs (record_texture_copy)
            for(uint32_t i=1;i<texture.mip_levels;++i) {
                barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,i-1);
                VkImageBlit mip{}; mip.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i-1,0,1}; mip.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,i,0,1};
                mip.srcOffsets[1]={int32_t(std::max(1u,texture.extent.width>>(i-1))),int32_t(std::max(1u,texture.extent.height>>(i-1))),1};
                mip.dstOffsets[1]={int32_t(std::max(1u,texture.extent.width>>i)),int32_t(std::max(1u,texture.extent.height>>i)),1};
                vkCmdBlitImage(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&mip,VK_FILTER_LINEAR);
            }
            barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,1,texture.mip_levels-1);
            barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT|VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,texture.mip_levels);
        } else
        barrier(slot.command,texture.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_COLOR_BIT,texture.mip_levels);
    }
    // A render target of the frame: efb_ (output resolution) or the internal scene targets. Between passes color is TRANSFER_SRC, depth
    // DEPTH_ATTACHMENT and motion COLOR_ATTACHMENT; *initialized false means never written (cleared on first use).
    if (!xform_draws.empty()) {   // the vertex pre-pass: every vertex of the packet's draws shaded once, before the passes that draw them
        // table: the draw's first texture-table entry, written as the last attribute word of every vertex (~0u: none)
        struct XformJob { uint64_t vertices; uint32_t count, draw; gx::Projection rows; uint32_t palette, current, indexed, bank, positions, attributes, mask, stride, table, merge_map, pad[2]; };   // smoke.slang XformJob
        static_assert(sizeof(XformJob)==128 && offsetof(XformJob,rows)==16 && offsetof(XformJob,palette)==80 && offsetof(XformJob,table)==112 && offsetof(XformJob,merge_map)==116);
        std::vector<XformJob> jobs; std::vector<std::array<uint32_t,2>> chunks;
        const auto address=[&](const Buffer& buffer) { VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer=buffer.buffer; return uint64_t(vkGetBufferDeviceAddress(device,&info)); };
        uint64_t inline_base=0;
        for (size_t d=0;d<packet.draws.size();++d) {
            const auto& x=xform_draws[d]; const auto& info=gpu_draws[d];
            if (!x.count) continue;
            uint64_t vertices;
            if (info.vertices) vertices=address(*info.vertices)+info.vertex_offset;
            else { if (!inline_base) inline_base=address(slot.vertices); vertices=inline_base+uint64_t(info.first_vertex)*sizeof(gx::GpuMeshVertex); }
            for (uint32_t first=0;first<x.count;first+=64) chunks.push_back({uint32_t(jobs.size()),first});
            jobs.push_back({vertices,x.count,draw_uniform[d],packet.draws[d].transforms->projection,info.palette,info.current,info.indexed,info.bank,x.positions,x.attributes,x.word>>8,x.word&255,
                            tex_merge_ ? (table_ok ? set_base[draw_set[d]] : 0u) : ~0u,table_ok ? merge_map : ~0u,{}});
        }
        if (!jobs.empty()) {
            const auto fill=[&](Buffer& buffer,const void* data,VkDeviceSize bytes) {
                if (buffer.size<bytes) { destroy_buffer(buffer); buffer=make_buffer(bytes+bytes/2,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT); }
                std::memcpy(buffer.mapped,data,size_t(bytes));
            };
            fill(slot.xform_jobs,jobs.data(),jobs.size()*sizeof(XformJob)); fill(slot.xform_chunks,chunks.data(),chunks.size()*sizeof(chunks[0]));
            if (!slot.xform_pool) {
                const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,7};
                VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets=1; pool.poolSizeCount=1; pool.pPoolSizes=&size;
                vk_check(vkCreateDescriptorPool(device,&pool,nullptr,&slot.xform_pool),"pre-pass descriptor pool");
                VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool=slot.xform_pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&xform_set_layout_;
                vk_check(vkAllocateDescriptorSets(device,&allocate,&slot.xform_set),"pre-pass descriptor set");
            }
            const Buffer* sources[7]={&slot.uniforms,&slot.palettes,&slot.banks,&slot.xform_positions,&slot.xform_attributes,&slot.xform_jobs,&slot.xform_chunks};
            const uint32_t numbers[7]={2,3,4,6,7,8,9};
            VkDescriptorBufferInfo infos[7]; VkWriteDescriptorSet writes[7]{};
            for (int i=0;i<7;++i) {
                infos[i]={sources[i]->buffer,0,VK_WHOLE_SIZE};
                writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,slot.xform_set,numbers[i],0,1,VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,nullptr,&infos[i],nullptr};
            }
            vkUpdateDescriptorSets(device,7,writes,0,nullptr);
            vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_COMPUTE,xform_pipeline_);
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_COMPUTE,xform_layout_,0,1,&slot.xform_set,0,nullptr);
            vkCmdDispatch(slot.command,uint32_t(chunks.size()),1,1);
            VkMemoryBarrier2 written{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            written.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; written.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            written.dstStageMask=VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT; written.dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.memoryBarrierCount=1; dependency.pMemoryBarriers=&written;
            vkCmdPipelineBarrier2(slot.command,&dependency);
        }
    }
    struct PassTarget { VkImage color, depth; VkImageView color_view, depth_view, motion_view, gbuffer_view; VkExtent2D extent; bool* initialized; bool clear_depth=false; };   // clear_depth: a scratch depth buffer, cleared by every pass
    const PassTarget efb_target{efb_,depth_,efb_view_,depth_view_,VK_NULL_HANDLE,VK_NULL_HANDLE,efb_extent_,&efb_initialized_};
    const PassTarget scene_target{scene_color_,scene_depth_,scene_color_view_,scene_depth_view_,scene_motion_view_,scene_gbuffer_view_,scene_extent_,&scene_initialized_};
    const auto depth_stages=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    // Debug: UC_SKIP_CALLSITES=<hex>[,<hex>...] leaves the draws of those guest callsites out (finding what a callsite draws).
    static const std::vector<uint32_t> skipped_callsites=[] {
        std::vector<uint32_t> sites;
        if(const char* text=std::getenv("UC_SKIP_CALLSITES")) for(const char* at=text;*at;) {
            char* end=nullptr; const auto value=std::strtoul(at,&end,16);
            if(end==at) break;
            sites.push_back(uint32_t(value)); at=*end==',' ? end+1 : end;
        }
        return sites;
    }();
    // UC_POST_DUMP=N (diagnostics): every Nth presented frame (up to 12 of them), each packet's passes, draws and copy.
    static const long post_dump_every = std::getenv("UC_POST_DUMP") ? std::atol(std::getenv("UC_POST_DUMP")) : 0;
    static const uint64_t post_dump_serial = std::getenv("UC_CAPTURE_PASS_SERIAL") ? std::strtoull(std::getenv("UC_CAPTURE_PASS_SERIAL"),nullptr,10) : 0;
    static long post_dumps = 0;
    const bool post_dump = (post_dump_serial && packet.serial==post_dump_serial) ||
        (post_dump_every > 0 && frame_index_ > 0 && frame_index_ % uint64_t(post_dump_every) == 0 && post_dumps < 12);
    if (post_dump) {
        std::fprintf(stderr, "[post-dump] frame %llu packet draws %zu scene_draws %zu present %d copy %s clear_after %d scene_phase %d upscale_here %d\n", (unsigned long long)frame_index_,
                     packet.draws.size(), scene_draws, int(packet.present), packet.texture_copy ? (std::to_string(packet.texture_copy->source[0]) + "," + std::to_string(packet.texture_copy->source[1]) + " " +
                     std::to_string(packet.texture_copy->source[2]) + "x" + std::to_string(packet.texture_copy->source[3]) + " -> " + std::to_string(copy_target ? copy_target->extent.width : 0) + "x" +
                     std::to_string(copy_target ? copy_target->extent.height : 0) + " mips " + std::to_string(copy_target ? copy_target->mip_levels : 0)).c_str() : "-",
                     int(packet.clear_after_copy), int(scene_phase_), int(upscale_here));
        if (packet.present) ++post_dumps;
    }
    // The stereo facts of a draw (HOTD4's classification input, lg_stereo_classify_draw).
    const auto stereo_facts=[&](size_t d) {
        const auto& draw=packet.draws[d];
        bool texture_view=false;
        for(size_t map=0;map<8;++map) if(const auto texture=draw_texture(draw,map); texture && texture->gpu_only) texture_view=true;
        LgStereoDrawFacts facts{};
        facts.perspective=draw.transforms->projection[3][3]==0; facts.texture_view=texture_view;   // as ultrawide.h tells perspective from ortho
        // HOTD4 (vulkan_guest.cpp): a 2D draw covers / fills the screen by its geometry's clip-space box (+-0.95 / +-0.98 of its
        // viewport), not by its viewport, which here is the whole scene for every draw, HUD text included.
        facts.covers_viewport=1;
        if (!facts.perspective) if (const auto box=gx::draw_bounds_ndc(draw)) {
            const auto& b=*box;
            facts.covers_viewport=b[0]<=-.95f && b[1]<=-.95f && b[2]>=.95f && b[3]>=.95f;
            facts.fills_screen=b[0]<=-.98f && b[1]<=-.98f && b[2]>=.98f && b[3]>=.98f;
            // This game draws its full-screen effects (the damage flash and blood) as four corner quads, none of which covers the screen:
            // a big 2D draw (a fifth of the screen or more) that reaches a horizontal and a vertical edge is part of such an overlay and
            // fills its part of each eye. A strip from edge to edge on one axis only (a sliding banner) stays on the HUD's screen.
            const float area=(std::clamp(b[2],-1.f,1.f)-std::clamp(b[0],-1.f,1.f))*(std::clamp(b[3],-1.f,1.f)-std::clamp(b[1],-1.f,1.f))/4;
            if (area>=.2f && (b[0]<=-.98f || b[2]>=.98f) && (b[1]<=-.98f || b[3]>=.98f)) facts.fills_screen=1;
        }
        // HOTD4's "offscreen" is a target that is not eye-sized. Here every pass draws into the EFB, which holds both eyes, and its
        // copies are per eye: the scene's packets (copied whole, 640x360, or at half size, 320x180) are eye renders. Only a small
        // perspective copy (a render-to-texture picture) keeps the game's camera; 2D passes over copies (the game's own post chain)
        // keep identity in each eye.
        const bool small_copy=copy_target && packet.texture_copy->source[2]<300;
        facts.offscreen=copy_target && !packet.present && (!facts.perspective || small_copy);
        facts.multiview=1;
        return facts;
    };
    // The headset's HUD and glow layers (hud_layer.h): over a world, this packet's full-screen glows (2D draws each eye sees in its own clip
    // space, without detail finer than a guest pixel) go to the glow layer and its HUD draws to the HUD layer instead of the EFB, when the
    // packet's draws from the first of them on are glows, then HUD draws (with the game's crosshair shown, its quads after them keep the
    // packet in the EFB: the layers are composited last). Draws before stay in the EFB. A later
    // packet that draws or copies composites the layers into the EFB first (flush_hud_layer); the present lays them over the eyes.
    // hud_draw: 1 = the HUD layer, 2 = the glow layer.
    std::vector<uint8_t> hud_draw;
    if (hud_layer_wanted() && !overlay_frame && !copy_target && !(scene_phase_ && !upscale_here) && packet.draws.size()>scene_draws) {
        hud_draw.assign(packet.draws.size(),0);
        bool world_drawn=world_drawn_, any=false, mixed=false;
        std::optional<size_t> first_hud, first_glow; std::array<float,4> hud_viewport{}, glow_viewport{};
        static const bool no_glow=std::getenv("UC_NO_GLOW_LAYER")!=nullptr;
        const bool glow_wanted=!no_glow && std::min(efb_extent_.width/640.f,efb_extent_.height/480.f)>=2.5f;   // the layer's 2 points a guest pixel save only above that
        for (size_t d=scene_draws; d<packet.draws.size() && !mixed; ++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            if (!info.index_count || std::find(skipped_callsites.begin(),skipped_callsites.end(),draw.callsite)!=skipped_callsites.end() ||
                (gx::bloom_composite(draw) && (motion_outputs_ || !bloom_active())) || (layer_on && draw.layer) || (draw.reticle && crosshair_scale_<=0)) continue;   // drawn nowhere
            if (draw.layer && crosshair_hidden()) continue;   // the headset's hidden game crosshair (the laser aims)
            const auto facts=stereo_facts(d);
            int kind=lg_stereo_classify_draw(&facts);
            if (kind==LG_STEREO_FILL && !world_drawn) kind=LG_STEREO_HUD;
            if (kind==LG_STEREO_WORLD && facts.perspective) world_drawn=true;
            const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
            const uint32_t depth=info.key[68];
            const bool aim=draw.reticle || draw.late_aim || draw.wake || draw.layer;
            const bool layer=kind==LG_STEREO_HUD && !aim && !facts.perspective && !facts.texture_view && draw.blend.color_write &&
                hud_blend_separable(draw.blend.mode,draw.blend.source,draw.blend.destination) && !(depth&16) && (!(depth&1) || ((depth>>1)&7)==7) &&
                v[2]>0 && v[3]>0 && (!first_hud || (v[0]==hud_viewport[0] && v[1]==hud_viewport[1] && v[2]==hud_viewport[2] && v[3]==hud_viewport[3]));   // one viewport: the layer's
            const bool glow=glow_wanted && !first_hud && !aim && ((kind==LG_STEREO_WORLD && !facts.perspective) || kind==LG_STEREO_FILL) && draw.blend.color_write &&
                hud_blend_separable(draw.blend.mode,draw.blend.source,draw.blend.destination) && !(depth&16) && (!(depth&1) || ((depth>>1)&7)==7) &&
                v[2]>0 && v[3]>0 && (!first_glow || (v[0]==glow_viewport[0] && v[1]==glow_viewport[1] && v[2]==glow_viewport[2] && v[3]==glow_viewport[3])) &&
                bloom_shading_rate(draw,{2560,1920},true,false,false).width>=2;   // only magnified textures (bloom_shading_rate's test at 4 EFB pixels a guest pixel)
            if (layer && world_drawn) { hud_draw[d]=1; any=true; if (!first_hud) { first_hud=d; hud_viewport={v[0],v[1],v[2],v[3]}; } }
            else if (glow && world_drawn) { hud_draw[d]=2; any=true; if (!first_glow) { first_glow=d; glow_viewport={v[0],v[1],v[2],v[3]}; } }
            else if (first_hud || first_glow) mixed=true;   // drawn into the EFB after a layer draw: under the layers' composite, not over (an aim reticle under a glow)
        }
        if (post_dump) std::fprintf(stderr,"[post-dump] layer scan: %d HUD draws from %d, %d glow draws from %d, mixed %d, world drawn %d\n",int(std::count(hud_draw.begin(),hud_draw.end(),1)),first_hud ? int(*first_hud) : -1,
                                    int(std::count(hud_draw.begin(),hud_draw.end(),2)),first_glow ? int(*first_glow) : -1,int(mixed),int(world_drawn_));
        if (!any || mixed) hud_draw.clear();
        else if (first_glow) {   // the glow layer: the viewport at UC_GLOW_SCALE (default 2) points a guest pixel
            static const float glow_scale=std::getenv("UC_GLOW_SCALE") ? std::clamp(float(std::atof(std::getenv("UC_GLOW_SCALE"))),1.f,4.f) : 2.f;
            ensure_hud_layer(glow_,{uint32_t(std::lround(glow_viewport[2]*glow_scale)),uint32_t(std::lround(glow_viewport[3]*glow_scale))});
            glow_.map=hud_overlay_map(glow_viewport);
        }
        if (!hud_draw.empty() && first_hud) {   // the layer's place in each eye: the HUD class's eye transform (the same for every HUD draw) at the HUD's depth
            const auto& draw=packet.draws[*first_hud]; const auto& info=gpu_draws[*first_hud];
            const auto& matrices=stereo_->transforms(gl_from_vulkan(draw.transforms->projection),LG_STEREO_HUD);
            float block[32];
            stereo_eye_block(matrices,block);
            float z=.5f;   // the clip depth of the draw's first vertex (a 2D draw: w = 1)
            if (const auto vertices=draw.vertex_view(); !vertices.empty()) {
                const auto& t=*draw.transforms; const auto& vertex=vertices[0];
                const uint32_t row=info.indexed ? vertex.position_matrix : info.current;
                if (row<=61) {
                    float view[3]; for (int i=0;i<3;++i) { const auto& m=t.position_rows[row+i]; view[i]=m[0]*vertex.position[0]+m[1]*vertex.position[1]+m[2]*vertex.position[2]+m[3]; }
                    const auto& m=t.projection[2]; const auto& w=t.projection[3];
                    const float cw=w[0]*view[0]+w[1]*view[1]+w[2]*view[2]+w[3];
                    if (cw!=0) z=(m[0]*view[0]+m[1]*view[1]+m[2]*view[2]+m[3])/cw;
                }
            }
            HudLayerMap map;
            if (!hud_layer_map(block,z,hud_viewport,map)) hud_draw.clear();
            else {
                const float k=hud_layer_scale(map,efb_extent_.width,efb_extent_.height,4.f);
                ensure_hud_layer(hud_,{uint32_t(std::lround(hud_viewport[2]*k)),uint32_t(std::lround(hud_viewport[3]*k))});
                hud_.map=map;
            }
        }
    }
    // The particle layer (UC_PARTICLE_LAYER=1, the headset): the longest run of this packet's translucent world sprites (perspective, no depth
    // write, a blend whose destination factor is a scalar: additive and alpha-over smoke, heat haze, glows) is drawn at half the EFB's size
    // a side, tested against a nearest-sampled half-size copy of the scene's depth, into L (colour) and T (alpha: what the scene behind
    // keeps), then composited into the EFB once (EFB * T + L) before the rest of the packet and its copy. ~5 scene screens of overdraw at
    // native become a quarter of that plus one full-screen composite.
    bool particle_pass=false;
    const auto particle_pipeline=[&](const PipelineKey& key) { auto k=key; k[0]|=kParticleLayerKeyBit; return pipeline_for(k).pipeline; };
    size_t particle_first=0, particle_last=0;
    std::vector<uint8_t> particle_draw;   // 1: drawn in the particle layer (inside [particle_first, particle_last)), the rest of the run in the EFB
    {
        if (particle_layer_on() && layers_>1 && stereo_ && !overlay_frame && !(scene_phase_ && !upscale_here) && efb_initialized_ && std::min(efb_extent_.width/640.f,efb_extent_.height/480.f)>=2.5f) {
            // A run of additive world sprites (perspective, no depth write, blend (SRC_ALPHA or ONE, ONE)): additions commute, so the run's
            // sprites whose textures are magnified (a guest pixel a texel or more) can go to the layer and the others stay in the EFB at full
            // size (a spark's sharp rays), in any order. Any other draw ends the run.
            particle_draw.assign(packet.draws.size(),0);
            size_t run_first=0, best_first=0, best_last=0, best_layer=0, layer=0, count=0;
            double area=0, best_area=0;   // the run's layer sprites' screen area (guest pixels, clipped boxes): the layer pays off from about a screen
            for (size_t d=scene_draws; d<packet.draws.size(); ++d) {
                const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
                if (!info.index_count) continue;
                const uint32_t depth=info.key[68]; const auto& b=draw.blend;
                const bool sprite=draw.transforms->projection[3][3]==0 && !draw.reticle && !draw.late_aim && !draw.wake && !draw.layer &&
                    b.mode==1 && b.color_write && !b.alpha_write && !(depth&16) && (b.source==kGxSrcSrcAlpha || b.source==kGxSrcOne) && b.destination==kGxDstOne &&
                    [&] { const auto facts=stereo_facts(d); return lg_stereo_classify_draw(&facts)==LG_STEREO_WORLD && !facts.offscreen; }();
                if (!sprite) { count=layer=0; area=0; continue; }
                if (!count) run_first=d;
                ++count;
                const bool magnified=[&] {   // every texture its stages sample has a texel over a guest pixel or more (by the draw's box), none an EFB copy
                    const auto box=gx::draw_bounds_ndc(draw);
                    if (!box) return false;
                    const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
                    const float width=(std::clamp((*box)[2],-1.f,1.f)-std::clamp((*box)[0],-1.f,1.f))*.5f*std::abs(v[2]), height=(std::clamp((*box)[3],-1.f,1.f)-std::clamp((*box)[1],-1.f,1.f))*.5f*std::abs(v[3]);
                    const auto& key=info.key; const auto& u=*draw.uniforms;
                    uint32_t maps=key[1]==0 ? 1u : 0u;
                    for (uint32_t stage=0;stage<std::min<uint32_t>(key[1],16);++stage) {
                        if (key[5+4*stage]&64) maps|=1u<<(key[5+4*stage]&7);
                        if (u.ind_cmd[stage]) maps|=1u<<((u.indirect[9]>>((u.ind_cmd[stage]&3)*6))&7);
                    }
                    for (size_t map=0;map<8;++map) if ((maps>>map)&1) {
                        const auto& texture=draw_texture(draw,map);
                        if (!texture || texture->levels.empty() || texture->gpu_only) return false;
                        if (texture->levels[0].width>width || texture->levels[0].height>height) return false;   // a texel over a guest pixel or more: 1.6 layer pixels or more at native
                    }
                    return true;
                }();
                if (magnified) {
                    particle_draw[d]=1; ++layer;
                    if (const auto box=gx::draw_bounds_ndc(draw)) {
                        const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
                        area+=double(std::clamp((*box)[2],-1.f,1.f)-std::clamp((*box)[0],-1.f,1.f))*.5*std::abs(v[2])*double(std::clamp((*box)[3],-1.f,1.f)-std::clamp((*box)[1],-1.f,1.f))*.5*std::abs(v[3]);
                    }
                }
                if (area>best_area) { best_area=area; best_layer=layer; best_first=run_first; best_last=d+1; }
            }
            if (post_dump) {
                size_t sprites_seen=0, magnified_seen=0;
                for (size_t d=scene_draws; d<packet.draws.size(); ++d) { magnified_seen+=particle_draw[d]; }
                for (size_t d=scene_draws; d<packet.draws.size(); ++d) { const auto& b=packet.draws[d].blend; sprites_seen+=packet.draws[d].transforms->projection[3][3]==0 && b.mode==1 && b.destination==kGxDstOne; }
                std::fprintf(stderr,"[post-dump] particle scan: %zu draws, %zu additive perspective, %zu magnified, best run %zu..%zu with %zu (%.2f screens)\n",packet.draws.size(),sprites_seen,magnified_seen,best_first,best_last,best_layer,best_area/(640*360));
            }
            static const double min_screens=std::getenv("UC_PARTICLE_MIN_SCREENS") ? std::atof(std::getenv("UC_PARTICLE_MIN_SCREENS")) : 1.0;
            if (best_area>=min_screens*640*360) {
                particle_first=best_first; particle_last=best_last;
                for (size_t d=0; d<packet.draws.size(); ++d) if (d<particle_first || d>=particle_last) particle_draw[d]=0;
            } else particle_draw.clear();
        }
    }
    // The small-draw ubershader's test (uber_merge.h): the guest pixels a draw's screen box covers, from its vertices (a cached mesh drawn
    // with one matrix: its object-space box) through its own transforms; infinite when a vertex lies behind the camera or the draw has too
    // many vertices to bound cheaply. Computed when first asked.
    std::vector<float> draw_pixels_cache;
    const auto draw_pixels=[&](size_t d) {
        if (draw_pixels_cache.empty()) draw_pixels_cache.assign(packet.draws.size(),-1.f);
        float& cached=draw_pixels_cache[d];
        if (cached>=0) return cached;
        constexpr size_t kMaxVertices=256;
        const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d]; const auto& t=*draw.transforms;
        float x0=1e9f, y0=1e9f, x1=-1e9f, y1=-1e9f; bool unbounded=false, any=false;
        const auto add=[&](const std::array<float,3>& p,uint32_t row) {
            if (row>61) return;
            float view[3]; for (int i=0;i<3;++i) { const auto& m=t.position_rows[row+i]; view[i]=m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3]; }
            float clip[4]; for (int i=0;i<4;++i) { const auto& m=t.projection[i]; clip[i]=m[0]*view[0]+m[1]*view[1]+m[2]*view[2]+m[3]; }
            if (!(clip[3]>1e-6f)) { unbounded=true; return; }
            const float x=clip[0]/clip[3], y=clip[1]/clip[3];
            x0=std::min(x0,x); x1=std::max(x1,x); y0=std::min(y0,y); y1=std::max(y1,y); any=true;
        };
        const auto vertices=draw.vertex_view();
        if (info.box && !info.indexed) { const auto& b=*info.box; for (int corner=0;corner<8;++corner) add({b[corner&1 ? 3 : 0],b[corner&2 ? 4 : 1],b[corner&4 ? 5 : 2]},info.current); }
        else if (vertices.size()<=kMaxVertices) { for (const auto& vertex:vertices) add(vertex.position,info.indexed ? vertex.position_matrix : info.current); }
        else unbounded=true;
        const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
        cached=unbounded ? INFINITY : any ? gx::box_guest_pixels({x0,y0,x1,y1},v[2],v[3]) : 0.f;
        return cached;
    };
    // The actual raster viewport, shared by attachment bounds and draw recording.
    // Late aim shifts clip coordinates, not this viewport. Reticle scaling does
    // change it, so apply the same centre/scale calculation in both places.
    const auto reticle_viewport=[&](const gx::DrawPacket& draw,EfbViewport area) {
        if (draw.reticle && crosshair_scale_>0 && crosshair_scale_!=1)
            if (const auto center=gx::draw_center_ndc(draw))
                gx::scale_viewport_about(*center,crosshair_scale_,area.x,area.y,area.width,area.height);
        return area;
    };
    auto record_pass=[&](const PassTarget& target, size_t first, size_t last, bool scene) {
    const bool initialized=*target.initialized;
    const bool efb_clear=efb_clear_.pending && target.color==efb_ && !overlay_frame;   // UC_FOLD_CLEAR: the copy's clear this pass applies first
    barrier(slot.command, target.color, initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE, initialized ? VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT : 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; color.imageView = target.color_view; color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = initialized ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    for (size_t i=0;i<4;++i) color.clearValue.color.float32[i]=packet.clear_color[i]/255.f;
    VkRenderingAttachmentInfo colors[5]{color,{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};   // [1]: motion, [2]: G-buffer, [3], [4]: RT material and albedo; none bound outside the scene pass
    const bool bind_motion=scene && vectors && (!motion_cleared_ || std::any_of(draw_motion.begin()+first,draw_motion.begin()+last,[](uint32_t index) { return index!=kNoMotion; }));
    if (bind_motion) {   // clear once, then bind only for passes whose depth-writing draws produce vectors
        barrier(slot.command,scene_motion_,motion_initialized_ && motion_cleared_ ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        colors[1].imageView=target.motion_view; colors[1].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[1].loadOp=motion_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; colors[1].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        motion_cleared_=motion_initialized_=true;
        scene_motion_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (scene && effects_need_gbuffer()) {   // cleared by the first scene pass of a frame (no surface), then loaded; bound only while an effect reads it
        barrier(slot.command,scene_gbuffer_,gbuffer_initialized_ && gbuffer_cleared_ ? scene_gbuffer_layout_ : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        colors[2].imageView=target.gbuffer_view; colors[2].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[2].loadOp=gbuffer_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; colors[2].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        gbuffer_cleared_=gbuffer_initialized_=true;
        scene_gbuffer_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (scene && rt_records) {   // the RT pair the same way: cleared once per frame (no record, no albedo), then loaded
        for (VkImage image:{scene_material_,scene_albedo_})
            barrier(slot.command,image,rt_material_initialized_ && rt_material_cleared_ ? scene_rt_layout_ : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        for (auto [attachment,view]:{std::pair{&colors[3],scene_material_view_},std::pair{&colors[4],scene_albedo_view_}}) {
            attachment->imageView=view; attachment->imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment->loadOp=rt_material_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; attachment->storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        }
        colors[3].clearValue.color.uint32[0]=kRtNoRecord;
        rt_material_cleared_=rt_material_initialized_=true;
        scene_rt_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    // The render area: only the pixels this pass's draws can reach (the union of their viewport and scissor rectangles; clipping keeps
    // every fragment inside its viewport). Load and store only cover the render area, so on a tiler (Quest) a post pass drawing one
    // 160x90 quad no longer loads and stores the whole EFB, colour and depth, for both eyes. Outside it the attachments keep their
    // contents. A first use (cleared) and the held overlay frame (other viewport mapping) keep the whole target.
    static const bool full_render_area = std::getenv("UC_FULL_RENDER_AREA") != nullptr;   // diagnostics: the whole target, as before
    VkRect2D render_area{{0, 0}, target.extent};
    if (initialized && !overlay_frame && !full_render_area) {
        int64_t x0 = target.extent.width, y0 = target.extent.height, x1 = 0, y1 = 0;        const auto add = [&](int64_t ax, int64_t ay, int64_t bx, int64_t by) {
            ax = std::clamp<int64_t>(ax, 0, target.extent.width); bx = std::clamp<int64_t>(bx, 0, target.extent.width);
            ay = std::clamp<int64_t>(ay, 0, target.extent.height); by = std::clamp<int64_t>(by, 0, target.extent.height);
            if (bx <= ax || by <= ay) return;
            x0 = std::min(x0, ax); y0 = std::min(y0, ay); x1 = std::max(x1, bx); y1 = std::max(y1, by);
        };
        if (efb_clear) add(efb_clear_.rect.offset.x, efb_clear_.rect.offset.y, int64_t(efb_clear_.rect.offset.x) + efb_clear_.rect.extent.width, int64_t(efb_clear_.rect.offset.y) + efb_clear_.rect.extent.height);
        if (scene && scene_clear_.pending) {
            const auto& r = scene_clear_.region;
            add(int64_t(r[0]) * target.extent.width / 640, int64_t(r[1]) * target.extent.height / 480,
                (int64_t(r[0]) + r[2]) * target.extent.width / 640 + 1, (int64_t(r[1]) + r[3]) * target.extent.height / 480 + 1);
        }
        bool bounded_reticle=false;
        static const bool full_reticle_area=std::getenv("UC_FULL_RETICLE_AREA")!=nullptr;
        for (size_t d = first; d < last; ++d) {
            if (x0 == 0 && y0 == 0 && x1 == int64_t(target.extent.width) && y1 == int64_t(target.extent.height)) break;   // already everything
            const auto& draw = packet.draws[d];
            if (!gpu_draws[d].index_count || (!hud_draw.empty() && hud_draw[d]) || (!particle_draw.empty() && bool(particle_draw[d])!=particle_pass)) continue;
            if (draw.reticle && crosshair_scale_<=0) continue;
            if (draw.reticle || draw.late_aim) {
                // Triangle clipping bounds fragments by the actual viewport,
                // even after late aim. Keep the prior bound for line expansion.
                if (full_reticle_area || gpu_draws[d].key[69]!=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST) {
                    add(0,0,target.extent.width,target.extent.height); continue;
                }
                bounded_reticle=true;
            }
            const auto v = draw.state->viewport.value_or(std::array<float,6>{0, 0, 640, 480, 0, 1});
            const auto a = reticle_viewport(draw,uc::efb_viewport(v,target.extent.width,target.extent.height));
            int64_t ax = int64_t(std::floor(std::min(a.x, a.x + a.width))), ay = int64_t(std::floor(std::min(a.y, a.y + a.height)));
            int64_t bx = int64_t(std::ceil(std::max(a.x, a.x + a.width))) + 1, by = int64_t(std::ceil(std::max(a.y, a.y + a.height))) + 1;
            if (draw.state->scissor && !draw.late_aim) {   // late aim may reset the scissor; retain its whole viewport
                const auto& s = *draw.state->scissor;
                ax = std::max(ax, int64_t(s[0]) * target.extent.width / 640); ay = std::max(ay, int64_t(s[1]) * target.extent.height / 480);
                bx = std::min(bx, (int64_t(s[0]) + s[2]) * target.extent.width / 640); by = std::min(by, (int64_t(s[1]) + s[3]) * target.extent.height / 480);
            }
            add(ax, ay, bx, by);
        }
        if (x1 <= x0 || y1 <= y0) { x0 = y0 = 0; x1 = y1 = 1; }   // nothing drawn: one pixel
        render_area = {{int32_t(x0), int32_t(y0)}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
        static const bool area_log = std::getenv("UC_GPU_PACKETS") != nullptr;   // the render area of the big passes, once per 2 s per kind
        if (area_log && (last - first >= 200 || bounded_reticle)) {
            static std::map<std::string, std::chrono::steady_clock::time_point> logged;
            auto& at = logged[slot.timing_kind];
            if (std::chrono::steady_clock::now() - at > std::chrono::seconds(2)) {
                at = std::chrono::steady_clock::now();
                std::fprintf(stderr, "[gpu-area] %s: render area %d,%d %ux%u of %ux%u\n", slot.timing_kind.c_str(), render_area.offset.x, render_area.offset.y, render_area.extent.width, render_area.extent.height, target.extent.width, target.extent.height);
            }
        }
    }
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea = render_area; rendering.layerCount = 1; rendering.viewMask = layers_>1 ? 0b11u : 0u; rendering.colorAttachmentCount = scene_color_attachments(); rendering.pColorAttachments = colors;
    // The headset's foveation map on every EFB pass while a level is on. A packet that draws a picture for a small copy (the bloom chain's
    // 320x180 / 160x90 corners, a render-to-texture) is not what the eyes see where it is drawn: its draws keep their own rate.
    VkRenderingFragmentShadingRateAttachmentInfoKHR fovea_attachment{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_SHADING_RATE_ATTACHMENT_INFO_KHR};
    const bool foveated=fovea_level_ && fovea_efb_.view && target.color==efb_;
    const bool fovea_packet=foveated && !(copy_target && packet.texture_copy->source[2]<300);
    if (foveated) {
        fovea_attachment.imageView=fovea_efb_.view; fovea_attachment.imageLayout=VK_IMAGE_LAYOUT_FRAGMENT_SHADING_RATE_ATTACHMENT_OPTIMAL_KHR;
        fovea_attachment.shadingRateAttachmentTexelSize=fovea_texel_; rendering.pNext=&fovea_attachment;
    }
    const VkImageLayout depth_before=!initialized ? VK_IMAGE_LAYOUT_UNDEFINED : scene ? scene_depth_layout_ : VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    // Attachment reuse depends on earlier depth tests, not unrelated vertex,
    // compute or colour-copy work. Transfers/sampling explicitly transition back
    // to this layout before reuse; scene upscalers may leave GENERAL and retain
    // the broad dependency. Dolphin's PrepareForRenderPass uses the same split.
    static const bool broad_depth=std::getenv("UC_BROAD_DEPTH_BARRIER")!=nullptr;
    const bool depth_attachment=depth_before==VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL && !broad_depth;
    barrier(slot.command,target.depth,depth_before,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            !initialized ? VK_PIPELINE_STAGE_2_NONE : depth_attachment ? depth_stages : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            !initialized ? 0 : depth_attachment ? VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT : VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,
            depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    if (scene) scene_depth_layout_=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth.imageView=target.depth_view; depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth.loadOp=initialized && !target.clear_depth ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil={packet.clear_depth/float(0xFFFFFF),0};
    // A pass whose draws neither test nor write depth (the game's full-screen post quads, 2D) and that clears none keeps depth as it is
    // without moving it through the tile memory: not stored, and with LOAD_OP_NONE not loaded either.
    if (initialized && !target.clear_depth && !full_render_area && !(scene && scene_clear_.pending) && !efb_clear &&
        std::none_of(gpu_draws.begin()+first,gpu_draws.begin()+last,[](const auto& info) { return info.index_count && ((info.key[68]&1) || ((info.key[68]>>4)&1)); })) {
        depth.storeOp=VK_ATTACHMENT_STORE_OP_NONE;
        if (load_op_none_) depth.loadOp=VK_ATTACHMENT_LOAD_OP_NONE_EXT;
    }
    // A pass whose draws test depth but none writes it (the translucent effects and particles after the scene) loads depth and leaves it
    // as it was: STORE_OP_NONE (Vulkan 1.3) writes nothing back, 2 x 2064x2944 x 4 bytes saved a pass at native. UC_DEPTH_STORE_ALWAYS=1: stored.
    static const bool store_always=std::getenv("UC_DEPTH_STORE_ALWAYS")!=nullptr;
    if (!store_always && depth.storeOp==VK_ATTACHMENT_STORE_OP_STORE && initialized && !target.clear_depth && !(scene && scene_clear_.pending) && !efb_clear &&
        std::none_of(gpu_draws.begin()+first,gpu_draws.begin()+last,[](const auto& info) { return info.index_count && ((info.key[68]>>4)&1); }))
        depth.storeOp=VK_ATTACHMENT_STORE_OP_NONE;
    if (!store_always && target.clear_depth) depth.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;   // a scratch depth buffer (the bloom proxy's, the held frame's): cleared by every pass, never read after it
    rendering.pDepthAttachment=&depth;
    bool clear_inside=false;   // UC_FOLD_CLEAR: the rectangle does not hold the render area (or the target is new): cleared after the begin
    if (efb_clear) {
        const auto& r=efb_clear_.rect;
        const bool holds=initialized && render_area.offset.x>=r.offset.x && render_area.offset.y>=r.offset.y &&
            int64_t(render_area.offset.x)+render_area.extent.width<=int64_t(r.offset.x)+r.extent.width && int64_t(render_area.offset.y)+render_area.extent.height<=int64_t(r.offset.y)+r.extent.height;
        if (holds) {
            color.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; color.clearValue.color=efb_clear_.color; colors[0]=color;
            depth.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; depth.clearValue.depthStencil={efb_clear_.depth,0};
            ++clears_as_load_;
        } else { clear_inside=true; ++clears_in_pass_; }
        efb_clear_.pending=false;
    }
    {   // UC_GPU_PACKETS: the attachment pixels moved through tile memory (loaded + stored, colour and depth, all eyes) against the whole target
        static const bool area_stats = std::getenv("UC_GPU_PACKETS") != nullptr;
        if (area_stats) {
            static double moved = 0, full = 0; static uint64_t passes = 0; static auto since = std::chrono::steady_clock::now();
            const double pixels = double(render_area.extent.width) * render_area.extent.height * layers_, whole = double(target.extent.width) * target.extent.height * layers_;
            const int color_moves = (color.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) + 1, depth_moves = (depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) + (depth.storeOp == VK_ATTACHMENT_STORE_OP_STORE);
            moved += pixels * (color_moves + depth_moves); full += whole * (color_moves + 2); ++passes;
            if (std::chrono::steady_clock::now() - since > std::chrono::seconds(2)) {
                std::fprintf(stderr, "[gpu-area] %llu passes: %.1f M attachment pixels moved (whole targets, colour+depth loaded and stored: %.1f M), %.0f%%\n",
                             (unsigned long long)passes, moved / 1e6, full / 1e6, 100 * moved / std::max(1.0, full));
                moved = full = 0; passes = 0; since = std::chrono::steady_clock::now();
            }
        }
    }
    if (post_dump) {   // UC_POST_DUMP: the pass, then each draw (viewport, scissor, clip-space box, what it samples)
        std::fprintf(stderr, "[post-dump] frame %llu pass %s %ux%u area %d,%d %ux%u colour load %d depth load %d store %d draws %zu-%zu\n", (unsigned long long)frame_index_,
                     scene ? "scene" : target.color == efb_ ? "efb" : "other", target.extent.width, target.extent.height, render_area.offset.x, render_area.offset.y,
                     render_area.extent.width, render_area.extent.height, int(color.loadOp), int(depth.loadOp), int(depth.storeOp), first, last);
        for (size_t d = first; d < last && d < first + 24; ++d) {
            const auto& draw = packet.draws[d];
            const auto v = draw.state->viewport.value_or(std::array<float,6>{0, 0, 640, 480, 0, 1});
            const auto box = gx::draw_bounds_ndc(draw);
            char textures[200] = ""; size_t used = 0;
            for (size_t map = 0; map < 8; ++map) if (const auto& t = draw_texture(draw, map); t && !t->levels.empty() && used < sizeof textures - 40)
                used += std::snprintf(textures + used, sizeof textures - used, " t%zu=%ux%u%s%s/%zu w%02x", map, t->levels[0].width, t->levels[0].height, t->gpu_only ? "c" : "", t->guest_address ? "a" : "", t->levels.size(), unsigned(draw.texture_wrap[map]));
            {   // the folded module this draw's pipeline looks for (tools/fold_tev_shaders.py name) and whether it exists
                const auto& key = gpu_draws[d].key; uint64_t hash = 0xCBF29CE484222325ull;
                const auto mix = [&](uint8_t byte) { hash ^= byte; hash *= 0x100000001B3ull; };
                for (size_t i = 1; i < 68; ++i) for (int shift = 0; shift < 32; shift += 8) mix(uint8_t(key[i] >> shift));
                for (int shift = 0; shift < 32; shift += 8) mix(uint8_t(((key[0] >> 16) & 1) >> shift));
                mix(((key[0] >> 16) & 1) ? 0 : 1);
                std::lock_guard guard(folded_mutex_);
                const auto found = folded_modules_.find(hash);
                std::fprintf(stderr, "[post-dump]   draw %zu pipeline %s fold %016llx %s key0 %08X tev %08X %08X %08X %08X %08X %08X gen0 %08X fog3 %08X alpha %08X\n", d, gpu_draws[d].choice.uber ? "uber" : "specialized", (unsigned long long)hash,
                             found == folded_modules_.end() ? "unknown" : found->second ? "folded" : "MISSING", key[0], key[1], key[2], key[3], key[4], key[5], key[6],
                             draw.uniforms->tex_gens[0], draw.uniforms->fog[3], draw.uniforms->pixel[0]);
                if (draw.vertex_view().size() <= 6) for (const auto& vertex : draw.vertex_view())
                    std::fprintf(stderr, "[post-dump]     v %.3f %.3f %.3f uv %.4f %.4f rgba %u %u %u %u row %u\n", vertex.position[0], vertex.position[1], vertex.position[2], vertex.texcoord[0][0], vertex.texcoord[0][1],
                                 vertex.color[0][0], vertex.color[0][1], vertex.color[0][2], vertex.color[0][3], vertex.position_matrix);
                const auto& rows = draw.uniforms->tex_rows;
                std::fprintf(stderr, "[post-dump]     texrow0 %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n", rows[0][0], rows[0][1], rows[0][2], rows[0][3], rows[1][0], rows[1][1], rows[1][2], rows[1][3]);
            }
            std::fprintf(stderr, "[post-dump]   draw %zu idx %u callsite %08X vp %.0f,%.0f %.0fx%.0f sc %s box %s blend %u cw %d aw %d z %u tev %u persp %d%s\n", d, gpu_draws[d].index_count, draw.callsite,
                         v[0], v[1], v[2], v[3], draw.state->scissor ? (std::to_string((*draw.state->scissor)[0]) + "," + std::to_string((*draw.state->scissor)[1]) + " " + std::to_string((*draw.state->scissor)[2]) + "x" + std::to_string((*draw.state->scissor)[3])).c_str() : "-",
                         box ? (std::to_string((*box)[0]).substr(0, 5) + "," + std::to_string((*box)[1]).substr(0, 5) + ".." + std::to_string((*box)[2]).substr(0, 5) + "," + std::to_string((*box)[3]).substr(0, 5)).c_str() : "-",
                         unsigned(draw.blend.mode), int(draw.blend.color_write), int(draw.blend.alpha_write), unsigned(gpu_draws[d].key[68] & 0x1f), unsigned(draw.uniforms->pixel[3] & 15), int(draw.transforms->projection[3][3] == 0), textures);
        }
    }
    vkCmdBeginRendering(slot.command, &rendering);
    if (clear_inside) {
        VkClearAttachment clears[2]{}; clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[0].clearValue.color=efb_clear_.color;
        clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT; clears[1].clearValue.depthStencil={efb_clear_.depth,0};
        const VkClearRect rectangle{efb_clear_.rect,0,1};
        vkCmdClearAttachments(slot.command,2,clears,1,&rectangle);
    }
    if (scene && scene_clear_.pending) {   // the last present's clear, which GX applied to the EFB this scene renders into
        const auto& r=scene_clear_.region;
        const uint32_t x=uint64_t(r[0])*target.extent.width/640, y=uint64_t(r[1])*target.extent.height/480;
        const uint32_t right=std::max(x+1,uint32_t(uint64_t(r[0]+r[2])*target.extent.width/640)), bottom=std::max(y+1,uint32_t(uint64_t(r[1]+r[3])*target.extent.height/480));
        VkClearRect rectangle{{{int32_t(x),int32_t(y)},{right-x,bottom-y}},0,1};
        VkClearAttachment clears[2]{}; clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
        for (size_t i=0;i<4;++i) clears[0].clearValue.color.float32[i]=scene_clear_.color[i]/255.f;
        clears[1].clearValue.depthStencil={scene_clear_.depth/float(0xFFFFFF),0};
        vkCmdClearAttachments(slot.command,2,clears,1,&rectangle);
        scene_clear_.pending=false;
    }
    VkViewport viewport{0, 0, float(target.extent.width), float(target.extent.height), 0, 1}; VkRect2D scissor{{0, 0}, target.extent};
    vkCmdSetViewport(slot.command, 0, 1, &viewport); vkCmdSetScissor(slot.command, 0, 1, &scissor);
    if (!gpu_draws.empty()) {
        const Buffer* bound_vertices=nullptr; const Buffer* bound_indices=nullptr; VkDeviceSize bound_vertex_offset=0, bound_index_offset=0;
        uint32_t bound_state=~0u;   // cull and depth state last set on this command buffer (every pipeline bound below leaves them dynamic)
        // The pipeline, viewport and scissor last set in this pass: a draw that changes none of them records no command for them (the
        // scene packet's ~840 draws use ~18 pipelines). UC_NO_STATE_DEDUP=1 sets every one per draw, as before.
        static const bool no_dedup=std::getenv("UC_NO_STATE_DEDUP")!=nullptr;
        VkPipeline bound_pipeline=VK_NULL_HANDLE; VkViewport bound_viewport=viewport; VkRect2D bound_scissor=scissor;
        const auto bind_pipeline=[&](VkPipeline pipeline) {
            if (pipeline==bound_pipeline && !no_dedup) return;
            vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); bound_pipeline=pipeline; ++pipeline_binds_;
        };
        VkExtent2D bound_rate{1,1};   // the fragment shading rate: 1x1 at every pass's start
        if (set_shading_rate_) { const VkFragmentShadingRateCombinerOpKHR keep[2]{VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR}; set_shading_rate_(slot.command,&bound_rate,keep); }
        const auto set_draw_state=[&](const PipelineKey& key) {
            if (static_depth_) return;   // in the pipeline
            const uint32_t state=((key[0]>>14)&3)|((key[68]&31)<<2);
            if (state==bound_state) return;
            bound_state=state;
            vkCmdSetCullMode(slot.command,cull_mode(key));
            vkCmdSetDepthTestEnable(slot.command,key[68]&1);
            vkCmdSetDepthWriteEnable(slot.command,(key[68]>>4)&1);
            vkCmdSetDepthCompareOp(slot.command,VkCompareOp((key[68]>>1)&7));
        };
        bool probe_bound=false;
        // The headset's descriptor binding: the draw's shared set with its uniform block and eye block as dynamic offsets; nothing is
        // recorded when the set and both offsets are the ones bound. An eye block equal to the bound one keeps the bound offset.
        // A texture-table run (table) binds set 0 of the packet's first texture combination (its maps are not read, its other bindings are the
        // same in every set) and the table as set 1, through the table's layout (compatible with layout_ for set 0).
        // A small-draw ubershader run (uber) binds the same two sets and the packet's uniform blocks as set 2, through merge_layout_.
        VkDescriptorSet bound_set=VK_NULL_HANDLE; uint32_t bound_offsets[2]{~0u,~0u}; bool bound_table=false, bound_uber=false;
        const auto bind_draw_set=[&](size_t d,bool compare_eye,bool table=false,bool uber=false) {
            uint32_t offsets[2]={draw_uniform_offset[d],uint32_t(d*stereo_stride_)};
            if (compare_eye && bound_offsets[1]!=~0u && std::memcmp(static_cast<std::byte*>(slot.motions.mapped)+offsets[1],static_cast<std::byte*>(slot.motions.mapped)+bound_offsets[1],kStereoBlockBytes)==0)
                offsets[1]=bound_offsets[1];
            const VkDescriptorSet set=slot.texture_sets[table ? 0 : draw_set[d]];
            if (set==bound_set && offsets[0]==bound_offsets[0] && offsets[1]==bound_offsets[1] && (!table || bound_table) && (!uber || bound_uber) && !no_dedup) return;
            if (uber) { const VkDescriptorSet sets[3]={set,slot.table_set,slot.merge_set}; vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,merge_layout_,0,3,sets,2,offsets); }
            else if (table) { const VkDescriptorSet sets[2]={set,slot.table_set}; vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,table_layout_,0,2,sets,2,offsets); }
            else vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&set,2,offsets);
            ++set_binds_;
            bound_set=set; bound_offsets[0]=offsets[0]; bound_offsets[1]=offsets[1]; bound_table=table || uber; bound_uber=uber;
        };
        // The pre-pass (xform_): consecutive draws that differ only in their vertices (same pipeline, textures, samplers, uniform block, eye
        // block, push constants, viewport, scissor; consecutive in the rebased index buffer) are one draw. UC_NO_MERGE=1: a draw each.
        static const bool no_merge=std::getenv("UC_NO_MERGE")!=nullptr;
        const bool frag_uniform=frag_uniform_merge();   // the Display page's "Draw merging", once a pass
        // A draw's own shading rate: the bloom chain's and world sprites' (bloom_shading_rate), and with the Display page's coarse post-effect
        // shading the full-screen 2D passes over the scene copy (post_shading_rate).
        const bool post_coarse=post_shading_coarse();
        const auto draw_rate=[&](const gx::DrawPacket& draw,int stereo_kind,bool depth_write) {
            const bool flat=!scene && (stereo_kind<0 || stereo_kind==LG_STEREO_PASS || stereo_kind==LG_STEREO_FILL);
            const VkExtent2D rate=bloom_shading_rate(draw,target.extent,flat,!scene && (stereo_kind<0 || stereo_kind==LG_STEREO_WORLD),depth_write);
            return post_coarse && flat ? coarser(rate,post_shading_rate(draw,target.extent)) : rate;
        };
        // uber: a small-draw ubershader run (uber_merge.h), draws: how many draws it holds; merge: the ubershader pipeline every draw of the run
        // may take (null: not all may); pixels: the guest pixels its draws' screen boxes cover (the ubershader's fragments, an upper bound).
        struct Run { size_t d; VkPipeline pipeline; size_t set; DrawPush push; VkViewport viewport; VkRect2D scissor; uint32_t first, count, positions_end, attributes_end; VkExtent2D rate; bool fovea; bool table=false;
                     bool uber=false; uint32_t draws=1; VkPipeline merge=VK_NULL_HANDLE; float pixels=0; VkPipeline table_pipeline=VK_NULL_HANDLE; VkPipeline few[2]{}; };   // rate: the fragment shading rate, fovea: whether the foveation map applies (a run joins only draws of one rate and kind); table: textures from the packet's texture table
        bool bound_fovea=false;
        // The rate of a draw: its own (pipeline rate), and with the foveation map the coarser of both (MAX), or the map's where the draw's own
        // is 1x1 (REPLACE) on a device without the non-trivial combiners.
        const auto apply_rate=[&](VkExtent2D rate,bool fovea) {
            if (!set_shading_rate_ || (rate.width==bound_rate.width && rate.height==bound_rate.height && fovea==bound_fovea)) return;
            VkFragmentShadingRateCombinerOpKHR ops[2]{VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR};
            if (fovea) ops[1]=fovea_max_op_ ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_MAX_KHR : rate.width*rate.height==1 ? VK_FRAGMENT_SHADING_RATE_COMBINER_OP_REPLACE_KHR : VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR;
            set_shading_rate_(slot.command,&rate,ops); bound_rate=rate; bound_fovea=fovea;
        };
        std::optional<Run> pending;
        size_t run_last=0;   // the pending run's last draw
        const auto issue_run=[&](const Run& run) {
            const VkPipeline family=run.few[0] ? run.few[0] : run.few[1];   // the smallest family of the ubershader that holds every draw of the run
            if (run.uber) { ++uber_runs_; uber_draws_+=run.draws; uber_pixels_+=run.pixels; uber_few_runs_+=family!=VK_NULL_HANDLE; }
            else if (run.merge) ++uber_singles_;   // small draws that stayed on their folded module (nothing to join through the ubershader)
            const auto& draw=packet.draws[run.d]; const auto& info=gpu_draws[run.d];
            if (no_dedup || std::memcmp(&run.viewport,&bound_viewport,sizeof run.viewport)) { vkCmdSetViewport(slot.command,0,1,&run.viewport); bound_viewport=run.viewport; }
            if (no_dedup || std::memcmp(&run.scissor,&bound_scissor,sizeof run.scissor)) { vkCmdSetScissor(slot.command,0,1,&run.scissor); bound_scissor=run.scissor; }
            if (bound_indices!=&slot.xform_indices) { vkCmdBindIndexBuffer(slot.command,slot.xform_indices.buffer,0,VK_INDEX_TYPE_UINT32); bound_indices=&slot.xform_indices; bound_index_offset=0; }
            bind_pipeline(run.uber && family ? family : run.pipeline);
            set_draw_state(info.key);
            if (layers_>1) bind_draw_set(run.set,true,run.table,run.uber);
            else vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[run.set],0,nullptr);
            table_runs_+=run.table;
            DrawPush push=run.push;
            apply_rate(run.rate,run.fovea);
            {   // UC_GPU_PACKETS: how many issued runs shade coarsely, flat (bloom chain, glows) and world (sprites), and how many take the foveation map
                static const bool rate_stats=std::getenv("UC_GPU_PACKETS")!=nullptr;
                if (rate_stats) {
                    static uint64_t flat=0, world=0, foveated_runs=0, all=0; static auto since=std::chrono::steady_clock::now();
                    ++all; if (run.rate.width*run.rate.height>1) ++(draw.transforms->projection[3][3]==0 ? world : flat);
                    foveated_runs+=run.fovea;
                    if (std::chrono::steady_clock::now()-since>std::chrono::seconds(2)) {
                        std::fprintf(stderr,"[vrs] runs shaded coarsely: %llu flat, %llu world, of %llu; %llu foveated (level %d)\n",(unsigned long long)flat,(unsigned long long)world,(unsigned long long)all,(unsigned long long)foveated_runs,fovea_level_);
                        flat=world=foveated_runs=all=0; since=std::chrono::steady_clock::now();
                    }
                }
            }
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push);
            const bool counted=frag_dump_ && (frag_queries_.size()+1)*layers_<=kFragQueries;
            if (counted) vkCmdBeginQuery(slot.command,frag_pool_,uint32_t(frag_queries_.size()*layers_),VK_QUERY_CONTROL_PRECISE_BIT);
            vkCmdDrawIndexed(slot.command,run.count,1,run.first,0,0);
            ++issued_draws_;
            const auto depth=draw.uniforms->pixel[1]&31;
            if((draw.uniforms->pixel[2]&256) && draw.blend.alpha_write && !(depth&1 && ((depth>>1)&7)==0) && (fuse_off() || !fused_destination_alpha(draw))) {   // as below: destination alpha after RGB blending
                const auto alpha_key=pipeline_key(draw,true);
                const auto alpha=pipeline_for(alpha_key);
                bind_pipeline(alpha.pipeline);
                set_draw_state(alpha_key);
                if (alpha.uber) { push.alpha_pass=1; vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push); }
                vkCmdDrawIndexed(slot.command,run.count,1,run.first,0,0);
                ++alpha_passes_;
            }
            if (counted) { vkCmdEndQuery(slot.command,frag_pool_,uint32_t(frag_queries_.size()*layers_)); frag_queries_.push_back(uint32_t(run.d)); }
        };
        for (size_t d=first; d<last; ++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            const size_t set_index=d;
            if (!info.index_count || (!hud_draw.empty() && hud_draw[d])) continue;   // drawn into the HUD layer (record_hud_layer)
            if (!particle_draw.empty() && bool(particle_draw[d])!=particle_pass) continue;   // the particle layer's draws in its pass only
            if (layer_on && !overlay_frame && draw.layer) continue;   // drawn later, over the held frame
            if (overlay_frame && !draw.layer) continue;
            if (gx::bloom_composite(draw) && (motion_outputs_ || !bloom_active())) continue;   // the game's own bloom is replaced by bloom_pass_ (off means none); a device without the scene-target path cannot run it and keeps the game's
            if (scene && effects_need_lights()) scene_lights_.add(*draw.uniforms);
            if (std::find(skipped_callsites.begin(),skipped_callsites.end(),draw.callsite)!=skipped_callsites.end()) continue;
            const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
            auto area=uc::efb_viewport(v,target.extent.width,target.extent.height);
            if (overlay_frame) {   // EFB coordinates onto the output: the part of the EFB the held frame shows, stretched over the whole picture
                const float sx=target.extent.width/float(display_source[2]),sy=target.extent.height/float(display_source[3]);
                area={(v[0]-1.f/12-display_source[0])*sx,(v[1]-1.f/12-display_source[1])*sy,v[2]*sx,v[3]*sy};
            }
            const auto raster_area=reticle_viewport(draw,area);
            VkViewport draw_viewport{raster_area.x,raster_area.y,raster_area.width,raster_area.height,v[4],v[5]};
            auto draw_scissor=scissor;
            if(draw.state->scissor && !overlay_frame) {
                const auto& s=*draw.state->scissor;
                const auto x=std::clamp<int64_t>(int64_t(s[0])*target.extent.width/640,0,target.extent.width);
                const auto y=std::clamp<int64_t>(int64_t(s[1])*target.extent.height/480,0,target.extent.height);
                const auto right=std::clamp<int64_t>((int64_t(s[0])+s[2])*target.extent.width/640,x,target.extent.width);
                const auto bottom=std::clamp<int64_t>((int64_t(s[1])+s[3])*target.extent.height/480,y,target.extent.height);
                draw_scissor={{int32_t(x),int32_t(y)},{uint32_t(right-x),uint32_t(bottom-y)}};
            }
            if (draw.layer && crosshair_hidden()) continue;   // the headset's "Game crosshair: Hidden": no reticle quad or wake (the laser aims)
            if (draw.reticle && crosshair_scale_<=0) continue;   // 0%: no crosshair (a Vulkan viewport needs a positive size)
            float late_shift[2]{};   // clip-space translation of a late-aimed reticle quad, in NDC of the draw's viewport
#ifdef UC_OPENXR
            if (xr_ && (draw.reticle || draw.late_aim || draw.wake)) xr_->finish_input();
#endif
            if (draw.late_aim && aim_source_ && !(draw.wake && aim_history_ && wake_seconds_.load()>0)) {
                auto latest=aim_source_();
#ifdef UC_OPENXR
                if (xr_) if (const auto aimed=xr_->aim_pointer()) latest=*aimed;
#endif
                const auto& from=draw.aim_exact ? draw.aim_ref : packet.aim_consumed;
                const float dx=(latest[0]-from[0])*gx::aim_to_ndc_x()*0.5f*area.width, dy=(latest[1]-from[1])*gx::aim_to_ndc_y()*0.5f*area.height;
                static const bool log_aim=std::getenv("UC_AIM_LOG")!=nullptr;   // diagnostics: one line per shifted reticle draw (consumed, latest, shift in aim units)
                if (log_aim) std::fprintf(stderr,"[aim] t=%.4f consumed %.4f %.4f latest %.4f %.4f callsite %08X\n",std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(),from[0],from[1],latest[0],latest[1],draw.callsite);
                if (dx!=0 || dy!=0) { late_shift[0]=dx*2/draw_viewport.width; late_shift[1]=dy*2/draw_viewport.height; draw_scissor=scissor; ++late_aim_draws_; }
            }
            if (!xform_) {   // the pre-pass: the state goes with the run's draw (issue_run below)
            if (no_dedup || std::memcmp(&draw_viewport,&bound_viewport,sizeof draw_viewport)) { vkCmdSetViewport(slot.command,0,1,&draw_viewport); bound_viewport=draw_viewport; }
            if (no_dedup || std::memcmp(&draw_scissor,&bound_scissor,sizeof draw_scissor)) { vkCmdSetScissor(slot.command,0,1,&draw_scissor); bound_scissor=draw_scissor; }
            const Buffer& vertex_buffer=info.vertices ? *info.vertices : slot.vertices;
            const Buffer& index_buffer=info.indices ? *info.indices : slot.indices;
            if (!xform_ && (&vertex_buffer!=bound_vertices || info.vertex_offset!=bound_vertex_offset)) { VkDeviceSize offset=info.vertex_offset; vkCmdBindVertexBuffers(slot.command,0,1,&vertex_buffer.buffer,&offset); bound_vertices=&vertex_buffer; bound_vertex_offset=info.vertex_offset; }   // the pre-pass's pipelines have no vertex input
            if (&index_buffer!=bound_indices || info.index_offset!=bound_index_offset) { vkCmdBindIndexBuffer(slot.command,index_buffer.buffer,info.index_offset,VK_INDEX_TYPE_UINT32); bound_indices=&index_buffer; bound_index_offset=info.index_offset; }
            bind_pipeline(particle_pass ? particle_pipeline(info.key) : info.choice.pipeline);
            set_draw_state(info.key);
            static const bool probe_sets=std::getenv("UC_PROBE_SETS")!=nullptr;   // diagnostics: one descriptor set for the whole pass (wrong textures and uniforms)
            if (layers_>1) { if (!probe_sets || !probe_bound) bind_draw_set(set_index,false); }
            else if (!probe_sets || !probe_bound)
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[set_index],0,nullptr);
            probe_bound=true;
            }
            DrawPush push{draw.transforms->projection,info.palette,info.current,info.indexed,info.key[67],info.bank};   // alpha_pass: the ubershader's destination alpha (a fused draw's too)
            if (xform_) { const auto& x=xform_draws[d]; push.palette=x.attributes; push.current_row=x.word; push.indexed=x.positions; }   // XFORM_DRAW: where the pre-pass put this draw's vertices
            push.viewport[0]=draw_viewport.x; push.viewport[1]=draw_viewport.y; push.viewport[2]=draw_viewport.width; push.viewport[3]=draw_viewport.height;
            push.shift[0]=late_shift[0]; push.shift[1]=late_shift[1];
            if (effects_.per_pixel_lighting && motion_outputs_) push.effects|=kEffectPerPixelLighting;   // only lit draws change: nothing else has a lighting channel on
            if (scene) {
                push.motion=draw_motion[d];
                if (summary.draws[d].perspective) { push.jitter[0]=jitter_[0]; push.jitter[1]=jitter_[1]; }   // post-style quads stay unjittered
                if (rt_records && draw_record[d]!=kRtNoRecord) {
                    push.record=draw_record[d];
                    push.effects|=kEffectRtMaterial|(records[draw_record[d]-first_record].flags&kRtModelValid ? kEffectRtModelValid : 0u);
                }
            }
            bool culled=false;   // the headset: neither eye sees this draw
            int stereo_kind=-1;   // the draw's stereo class (headset), for the shading rate below
            bool draw_offscreen=false;   // the class's offscreen fact: a perspective picture for a small copy or a 2D pass over copies
            if (layers_>1 && stereo_) {   // HOTD4's per-draw reprojection: both eyes' transforms of this draw into binding 5
                LgStereoDrawFacts facts=stereo_facts(d);
                const bool texture_view=facts.texture_view;
                const auto& source=packet.display_source;   // diagnostics only
                int kind=lg_stereo_classify_draw(&facts);
                if (kind==LG_STEREO_FILL && !world_drawn_) kind=LG_STEREO_HUD;   // HOTD4: full-screen 2D with no world yet is a 2D screen (menus); over a world, a fade
                if (kind==LG_STEREO_WORLD && facts.perspective) world_drawn_=true;
                static const bool class_stats=std::getenv("UC_VR_CLASS_STATS")!=nullptr;
                if (class_stats) {   // diagnostics: per second, how many draws went to each stereo class (and the 2D draws by packet kind)
                    static uint64_t counts[4][2][2]{}; static auto since=std::chrono::steady_clock::now();
                    ++counts[kind&3][facts.perspective][texture_view];
                    static std::map<std::string,uint64_t> huds;
                    {
                        static const char* kNames[]={"world","hud","pass","fill"};
                        std::array<uint32_t,4> copy_source{};
                        if (packet.texture_copy) copy_source=packet.texture_copy->source;
                        char key[260]; std::snprintf(key,sizeof key,"%s persp=%d present=%d copy=%u,%u %ux%u tex=%d covers=%d viewport %.0f,%.0f %.0fx%.0f source %d,%d %dx%d target %ux%u",kNames[kind&3],int(facts.perspective),int(packet.present),
                            copy_source[0],copy_source[1],copy_source[2],copy_source[3],int(texture_view),int(facts.covers_viewport),
                            draw_viewport.x,draw_viewport.y,draw_viewport.width,draw_viewport.height,int(source[0]),int(source[1]),int(source[2]),int(source[3]),target.extent.width,target.extent.height);
                        ++huds[key];
                        if (kind==LG_STEREO_HUD && !facts.perspective) if (const auto b=gx::draw_bounds_ndc(draw)) {   // the big 2D draws: where their boxes lie
                            const float area=(std::clamp((*b)[2],-1.f,1.f)-std::clamp((*b)[0],-1.f,1.f))*(std::clamp((*b)[3],-1.f,1.f)-std::clamp((*b)[1],-1.f,1.f))/4;
                            if (area>0.25f) {
                                char big[200]; std::snprintf(big,sizeof big,"BIG hud box %.2f,%.2f..%.2f,%.2f tex=%d blend %u callsite %08X world_drawn %d fills %d present %d copy %d draws %zu",(*b)[0],(*b)[1],(*b)[2],(*b)[3],int(texture_view),unsigned(draw.blend.mode),draw.callsite,int(world_drawn_),int(facts.fills_screen),int(packet.present),int(bool(copy_target)),packet.draws.size());
                                ++huds[big];
                            }
                        }
                    }
                    if (std::chrono::steady_clock::now()-since>std::chrono::seconds(1)) {
                        std::fprintf(stderr,"[vr-class-sec] world p=%llu o=%llu otex=%llu | hud p=%llu o=%llu otex=%llu | pass %llu | fill %llu\n",
                            (unsigned long long)(counts[0][1][0]+counts[0][1][1]),(unsigned long long)counts[0][0][0],(unsigned long long)counts[0][0][1],
                            (unsigned long long)(counts[1][1][0]+counts[1][1][1]),(unsigned long long)counts[1][0][0],(unsigned long long)counts[1][0][1],
                            (unsigned long long)(counts[2][0][0]+counts[2][0][1]+counts[2][1][0]+counts[2][1][1]),(unsigned long long)(counts[3][0][0]+counts[3][0][1]+counts[3][1][0]+counts[3][1][1]));
                        for (const auto& [k,n]:huds) std::fprintf(stderr,"[vr-class-hud] %6llu %s\n",(unsigned long long)n,k.c_str());
                        std::memset(counts,0,sizeof counts); huds.clear(); since=std::chrono::steady_clock::now();
                    }
                }
                static const long dump_frame=std::getenv("UC_VR_CLASSIFY_DUMP") ? std::atol(std::getenv("UC_VR_CLASSIFY_DUMP")) : -1;
                if (dump_frame>0 && preview_world_frames_==uint64_t(dump_frame)) {   // diagnostics (preview): the Nth frame after N-1 that drew the world, draw by draw
                    static const char* kNames[]={"world","hud","pass","fill"};
                    const auto& p=draw.transforms->projection;
                    std::fprintf(stderr,"[vr-class] frame %llu packet present=%d copy=%d draw %zu: %s persp=%d texview=%d covers=%d world_drawn=%d offscreen=%d viewport %.0f,%.0f %.0fx%.0f target %ux%u source %d,%d %dx%d p00 %.3f p11 %.3f p03 %.3f p13 %.3f\n",
                                 (unsigned long long)frame_index_,int(packet.present),int(bool(copy_target)),d,kind>=0&&kind<4?kNames[kind]:"?",int(facts.perspective),int(texture_view),int(facts.covers_viewport),int(world_drawn_),int(facts.offscreen),
                                 draw_viewport.x,draw_viewport.y,draw_viewport.width,draw_viewport.height,target.extent.width,target.extent.height,int(source[0]),int(source[1]),int(source[2]),int(source[3]),p[0][0],p[1][1],p[0][3],p[1][3]);
                }
                // The aim reticle (and its wake) marks a place in the world: it stays on HOTD4's world-locked screen when the HUD follows the head.
                if (kind==LG_STEREO_HUD && (draw.reticle || draw.late_aim || draw.wake)) kind=xr::LG_STEREO_HUD_AIM;
                stereo_kind=kind; draw_offscreen=facts.offscreen;
#ifdef UC_OPENXR
                if (xr_ && kind==LG_STEREO_WORLD && facts.perspective && !facts.offscreen &&
                    !draw.reticle && !draw.late_aim && !draw.wake && !draw.layer && (info.key[68]&16))
                    xr_->aim_draw(draw,info.box);
#endif
                if (post_dump) {   // UC_POST_DUMP: every draw's stereo class and blend (the HUD layer's question: which draws are separable)
                    const auto b=gx::draw_bounds_ndc(draw);
                    std::fprintf(stderr,"[post-class] draw %zu kind %d persp %d blend %u src %u dst %u logic %u cw %d aw %d depth %02X box %.2f,%.2f..%.2f,%.2f callsite %08X reticle %d layer %d idx %u\n",d,kind,int(facts.perspective),
                                 unsigned(draw.blend.mode),unsigned(draw.blend.source),unsigned(draw.blend.destination),unsigned(draw.blend.logic),int(draw.blend.color_write),int(draw.blend.alpha_write),unsigned(info.key[68]&31),
                                 b?(*b)[0]:9.f,b?(*b)[1]:9.f,b?(*b)[2]:9.f,b?(*b)[3]:9.f,draw.callsite,int(draw.reticle),int(draw.layer),info.index_count);
                }
                const auto& matrices=stereo_->transforms(gl_from_vulkan(draw.transforms->projection),kind);
                float* block=reinterpret_cast<float*>(static_cast<std::byte*>(slot.motions.mapped)+size_t(d)*stereo_stride_);
                stereo_eye_block(matrices,block);
                static const bool probe_mono=std::getenv("UC_PROBE_MONO")!=nullptr;   // diagnostics: both eyes see the game's own clip space (no reprojection)
                if (probe_mono) for(int eye=0;eye<2;++eye) for(int r=0;r<4;++r) for(int c=0;c<4;++c) block[eye*16+r*4+c]=r==c ? 1.f : 0.f;
                push.motion=uint32_t(d);
                // Culling (vr_cull.h): a world draw that neither eye sees is not drawn. Only perspective world draws of the visible scene
                // (not offscreen copies the game reads, not HUD, reticle or late-aimed quads); a cached mesh drawn with one matrix is
                // tested by its object-space box, any other draw by its vertices. UC_VR_NO_CULL=1 draws everything.
                static const bool no_cull=std::getenv("UC_VR_NO_CULL")!=nullptr;
                static uint64_t cull_tested=0, cull_draws=0, cull_indices=0, all_indices=0; static auto cull_since=std::chrono::steady_clock::now();
                static float reach[2]={0,0};   // the eyes' largest extent in the game's clip space (vr_cull.h eye_extent) over the period
                all_indices+=info.index_count;
                static const bool cull_stats=std::getenv("UC_GPU_PACKETS")!=nullptr;   // the [vr-cull] line (the eyes' reach is diagnostics only: a per-draw cost)
                if (cull_stats && kind==LG_STEREO_WORLD && facts.perspective && !facts.offscreen)
                    for (int eye=0;eye<2;++eye) { const auto e=uc::eye_extent(block+16*eye); reach[0]=std::max(reach[0],e[0]); reach[1]=std::max(reach[1],e[1]); }
                if (!no_cull && !probe_mono && kind==LG_STEREO_WORLD && facts.perspective && !facts.offscreen && !draw.reticle && !draw.late_aim && !draw.wake && !draw.layer) {
                    ++cull_tested;
                    const auto& t=*draw.transforms;
                    const auto to_clip=[&](const std::array<float,3>& p,uint32_t row) {
                        float view[3]; for (int i=0;i<3;++i) { const auto& m=t.position_rows[row+i]; view[i]=m[0]*p[0]+m[1]*p[1]+m[2]*p[2]+m[3]; }
                        std::array<float,4> clip; for (int i=0;i<4;++i) { const auto& m=t.projection[i]; clip[i]=m[0]*view[0]+m[1]*view[1]+m[2]*view[2]+m[3]; }
                        return clip;
                    };
                    std::vector<std::array<float,4>>& points=cull_points_; points.clear();
                    if (info.box && !info.indexed) {
                        const auto& b=*info.box;
                        for (int corner=0;corner<8;++corner) points.push_back(to_clip({b[corner&1 ? 3 : 0],b[corner&2 ? 4 : 1],b[corner&4 ? 5 : 2]},info.current));
                    } else {
                        for (const auto& vertex:draw.vertex_view()) { const uint32_t row=info.indexed ? vertex.position_matrix : info.current; if (row<=61) points.push_back(to_clip(vertex.position,row)); }
                    }
                    if (uc::clip_outside(block,points.data(),points.size()) && uc::clip_outside(block+16,points.data(),points.size())) {
                        ++cull_draws; cull_indices+=info.index_count;
                        culled=true;
                    }
                }
                if (cull_stats && std::chrono::steady_clock::now()-cull_since>std::chrono::seconds(2)) {
                    std::fprintf(stderr,"[vr-cull] 2 s: %llu world draws tested, %llu culled (%llu of %llu indices drawn); the eyes reach %.3f x %.3f of the game's view (widened %.2fx); %llu draws issued, %llu more merged into them, %llu descriptor set binds, %llu pipeline binds, %llu second destination-alpha draws\n",(unsigned long long)cull_tested,(unsigned long long)cull_draws,(unsigned long long)cull_indices,(unsigned long long)all_indices,reach[0],reach[1],gx::vr_fov_scale().load(),(unsigned long long)issued_draws_,(unsigned long long)merged_draws_,(unsigned long long)set_binds_,(unsigned long long)pipeline_binds_,(unsigned long long)alpha_passes_);
                    if (tex_merge_)   // the texture table over the same 2 s: packets with a table (and too large for it), its entries, the runs drawn from it, the draws it joined, draws whose table pipeline was still compiling
                        std::fprintf(stderr,"[tex-merge] packets %llu, overflows %llu, entries %llu, runs %llu, joins %llu, waiting %llu\n",(unsigned long long)table_packets_,(unsigned long long)table_overflows_,
                                     (unsigned long long)table_entries_,(unsigned long long)table_runs_,(unsigned long long)table_joins_,(unsigned long long)table_pending_);
                    table_packets_=table_overflows_=table_entries_=table_runs_=table_joins_=table_pending_=0;
                    if (uber_small_scene_ || uber_small_hud_ || frag_uniform_merge())   // the small-draw ubershader and the fragment-only uniform test over the same 2 s
                        std::fprintf(stderr,"[uber-merge] EFB: ubershader runs %llu holding %llu draws, joins %llu, runs of small draws left on their own pipeline %llu, big draws %llu, waiting %llu; HUD layer: %llu draws in %llu runs, %llu ubershader runs holding %llu draws; fragment-uniform joins %llu; ubershader guest pixels %.0f; EFB ubershader draws of 0/1/2/3+ stages %llu/%llu/%llu/%llu, runs on the few-stage family %llu\n",
                                     (unsigned long long)uber_runs_,(unsigned long long)uber_draws_,(unsigned long long)uber_joins_,(unsigned long long)uber_singles_,(unsigned long long)uber_big_,(unsigned long long)uber_pending_,
                                     (unsigned long long)hud_layer_draws_,(unsigned long long)hud_layer_runs_,(unsigned long long)hud_uber_runs_,(unsigned long long)hud_uber_draws_,(unsigned long long)frag_uniform_joins_,uber_pixels_,
                                     (unsigned long long)uber_stage_draws_[0],(unsigned long long)uber_stage_draws_[1],(unsigned long long)uber_stage_draws_[2],(unsigned long long)uber_stage_draws_[3],(unsigned long long)uber_few_runs_); uber_few_runs_=0;
                    std::fill(std::begin(uber_stage_draws_),std::end(uber_stage_draws_),0);
                    uber_runs_=uber_draws_=uber_joins_=uber_singles_=uber_big_=uber_pending_=frag_uniform_joins_=hud_layer_draws_=hud_layer_runs_=hud_uber_runs_=hud_uber_draws_=0; uber_pixels_=0;
                    cull_tested=cull_draws=cull_indices=all_indices=0; issued_draws_=merged_draws_=set_binds_=pipeline_binds_=alpha_passes_=0; reach[0]=reach[1]=0; cull_since=std::chrono::steady_clock::now();
                }
            }
            // Preserve view position through the pre-pass: reconstructing it
            // from rounded guest clip depth creates flashing holes under head
            // translation. UC_VR_CLIP_REPROJECT restores that path for diagnosis.
            static const bool direct_view=std::getenv("UC_VR_CLIP_REPROJECT")==nullptr;
            if (direct_view && stereo_ && stereo_->tracking_valid() && stereo_kind==LG_STEREO_WORLD &&
                !draw_offscreen && draw.transforms->projection[3][3]==0 && push.shift[0]==0 && push.shift[1]==0) {
                const auto& matrices=stereo_->transforms(gl_from_vulkan(draw.transforms->projection),LG_STEREO_WORLD);
                float* block=reinterpret_cast<float*>(static_cast<std::byte*>(slot.motions.mapped)+size_t(d)*stereo_stride_);
                stereo_view_block(matrices,block);
                push.effects|=kEffectStereoView;
            }
            if (culled) continue;
            // The foveation map: the eyes' scene (world draws that are not a picture for a copy), full-screen fades over it and the 2D passes
            // over full-size copies (the composite, the colour pass) shade by it; the HUD and the aim keep their rate (text stays sharp).
            const bool fovea_draw=fovea_packet && !draw.reticle && !draw.late_aim && !draw.wake && !draw.layer &&
                ((stereo_kind==LG_STEREO_WORLD && !draw_offscreen) || stereo_kind==LG_STEREO_FILL || stereo_kind==LG_STEREO_PASS);
            if (xform_) {   // join the pending run when nothing but the vertex range differs, else draw the pending run and start a new one
                push.motion=0;   // the stereo shaders read the eye block from the set, not this index
                const auto& x=xform_draws[d];
                Run run{d,particle_pass ? particle_pipeline(info.key) : info.choice.pipeline,set_index,push,draw_viewport,draw_scissor,x.first,info.index_count,x.positions+x.count,x.attributes+x.count*(x.word&255),
                        set_shading_rate_ ? draw_rate(draw,stereo_kind,(info.key[68]>>4)&1) : VkExtent2D{1,1},fovea_draw};
                // The texture table: the draw's pipeline variant that reads its textures from the packet's table, once compiled (meanwhile the
                // draw keeps its own). Not for the particle layer's pipelines, nor a draw whose second destination-alpha draw reads its own set.
                // UC_TEX_MERGE_SKIP=<hex>[,<hex>...] (diagnostics): those callsites keep their own sets
                static const std::vector<uint32_t> table_skipped=[] {
                    std::vector<uint32_t> sites;
                    if (const char* text=std::getenv("UC_TEX_MERGE_SKIP")) for (const char* at=text;*at;) { char* end=nullptr; const auto value=std::strtoul(at,&end,16); if (end==at) break; sites.push_back(uint32_t(value)); at=*end==',' ? end+1 : end; }
                    return sites;
                }();
                if (table_ok && !particle_pass && std::find(table_skipped.begin(),table_skipped.end(),draw.callsite)==table_skipped.end()) {
                    const auto depth=draw.uniforms->pixel[1]&31;
                    const bool second_alpha=(draw.uniforms->pixel[2]&256) && draw.blend.alpha_write && !(depth&1 && ((depth>>1)&7)==0) && (fuse_off() || !fused_destination_alpha(draw));
                    // Lazily (the default): the draw keeps its folded module and own set and only remembers its table variant (run.table_pipeline);
                    // the run moves onto the table when a draw with other textures joins it. A single draw or a run of equal textures reading the
                    // table cost the headset per fragment (non-uniform descriptor indexing: the full-screen glows, fades and sprites ~2x, S18
                    // section 8). UC_TEX_MERGE_EAGER=1: every draw that can on the table, as before.
                    if (!second_alpha) {
                        if (const VkPipeline table=table_pipeline(info.key)) {
                            if (table_eager_) { run.pipeline=table; run.table=true; }
                            else if (!table_max_pixels_ || draw_pixels(d)<=table_max_pixels_) run.table_pipeline=table;
                        } else ++table_pending_;
                    }
                    // The small-draw ubershader (uber_merge.h): a small draw may move onto the ubershader of its fixed-function state, but only to
                    // join a run it cannot join otherwise (below): a run of one draw, or of draws that join anyway, keeps its folded module.
                    if (!second_alpha && uber_small_scene_ && !frag_dump_ && uber_merge_stages_ok(info.key)) {
                        if (draw_pixels(d)>uber_max_pixels_) ++uber_big_;
                        else if (!(run.merge=uber_merge_pipeline(info.key))) ++uber_pending_;
                        else { run.pixels=draw_pixels(d); run.few[0]=uber_merge_pipeline(info.key,1); run.few[1]=uber_merge_pipeline(info.key,2); }
                    }
                }
                const auto same_push=[](DrawPush a,DrawPush b) { a.palette=b.palette=0; a.indexed=b.indexed=0; return std::memcmp(&a,&b,sizeof a)==0; };
                // the small-draw ubershader's runs: what the vertex map and each draw's block replace (attributes, stride and mask, first position, the
                // projection the draw vertex stage does not read, the destination-alpha flag, the bank) need not be equal
                const auto same_uber_push=[](DrawPush a,DrawPush b) { a.projection=b.projection={}; a.palette=b.palette=a.current_row=b.current_row=a.indexed=b.indexed=a.alpha_pass=b.alpha_pass=a.bank=b.bank=0; return std::memcmp(&a,&b,sizeof a)==0; };
                // the uniform blocks: the same block, or with UC_MERGE_FRAG_UNIFORM blocks equal where a fragment reads them (uber_merge.h)
                const auto same_uniform=[&](size_t a,size_t b) { return set_keys[a][16]==set_keys[b][16] || (frag_uniform && screen_refraction[a]==screen_refraction[b] && copy_dimensions[a]==copy_dimensions[b] && gx::fragment_uniforms_equal(*packet.draws[a].uniforms,*packet.draws[b].uniforms)); };
                const bool same_textures=pending && std::equal(set_keys[pending->set].begin(),set_keys[pending->set].begin()+16,set_keys[run.set].begin());
                // the sets: equal (textures, samplers, uniform block), or for two table runs the uniform block alone (the textures are in the table)
                // Lazily (run.table_pipeline): a run on its folded modules moves onto the table (table_moves) when the next draw has the same key and
                // uniform block and other textures; a run already on the table takes a draw whose table variant it uses.
                const bool table_moves=pending && !pending->table && !pending->uber && !run.table && run.table_pipeline && pending->table_pipeline==run.table_pipeline &&
                                       !same_textures && gpu_draws[pending->d].key==info.key && same_uniform(pending->set,run.set);
                const bool table_takes=pending && pending->table && !pending->uber && !run.table && run.table_pipeline && pending->pipeline==run.table_pipeline;
                const bool same_set=pending && (run.table || table_takes ? pending->table && same_uniform(pending->set,run.set) :
                                                table_moves || (!pending->table && same_textures && same_uniform(pending->set,run.set)));
                const bool same_eye=pending && std::memcmp(static_cast<std::byte*>(slot.motions.mapped)+pending->set*stereo_stride_,static_cast<std::byte*>(slot.motions.mapped)+run.set*stereo_stride_,kStereoBlockBytes)==0;
                const bool same_pipeline=pending && !pending->uber && (pending->pipeline==run.pipeline || table_moves || table_takes) && gpu_draws[pending->d].key==info.key;
                const bool same_view=pending && std::memcmp(&pending->viewport,&run.viewport,sizeof run.viewport)==0 && std::memcmp(&pending->scissor,&run.scissor,sizeof run.scissor)==0;
                const bool same_rate=pending && pending->first+pending->count==run.first && pending->rate.width==run.rate.width && pending->rate.height==run.rate.height && pending->fovea==run.fovea;
                const bool plain_joins=pending && !no_merge && !frag_dump_ && same_pipeline && same_set && same_eye && same_push(pending->push,run.push) && same_view && same_rate &&
                    pending->positions_end==x.positions && pending->attributes_end==x.attributes;
                // the ubershader's join: a small draw onto a run of small draws of the same fixed-function state, eye block, viewport, scissor and
                // rate, whatever their TEV setups, uniform blocks, textures and strides (the vertex map and set 2 carry them); the run moves onto
                // the ubershader
                const bool uber_state=pending && run.merge && pending->merge==run.merge && same_uber_state(gpu_draws[pending->d].key,info.key);
                const bool uber_joins=!plain_joins && pending && !no_merge && !frag_dump_ && uber_state && same_eye && same_uber_push(pending->push,run.push) && same_view && same_rate;
                const bool joins=plain_joins || uber_joins;
                static const bool why=std::getenv("UC_MERGE_WHY")!=nullptr;   // diagnostics: what ends the runs, per 2 s
                if (why && pending && !joins) {
                    static uint64_t reasons[8]{}; static auto since=std::chrono::steady_clock::now();
                    // the first test of the join that failed: textures only where the textures alone keep two runs apart (outside the table), the
                    // uniform block with UC_MERGE_FRAG_UNIFORM as that test; for two draws the ubershader could join, its tests
                    const bool textures=!((pending->table || pending->table_pipeline) && (run.table || run.table_pipeline)) && !same_textures, uniform=!same_uniform(pending->set,run.set);
                    if (pending->merge && run.merge) ++reasons[!uber_state ? 0 : !same_eye ? 3 : !same_uber_push(pending->push,run.push) ? 4 : !same_view ? 5 : 6];
                    else ++reasons[!same_pipeline ? 0 : textures ? 1 : uniform ? 2 : !same_eye ? 3 : !same_push(pending->push,run.push) ? 4 : !same_view ? 5 : 6];
                    // what bindless textures alone would join: everything but the textures equal (pipeline, uniform block, eye block, push,
                    // viewport, scissor, consecutive, rate)
                    if (same_pipeline && textures && !uniform && same_eye && same_push(pending->push,run.push) && same_view && same_rate) ++reasons[7];
                    if (std::chrono::steady_clock::now()-since>std::chrono::seconds(2)) {
                        std::fprintf(stderr,"[merge-why] runs ended by: pipeline %llu, textures %llu, uniform block %llu, eye block %llu, push %llu, viewport %llu, not consecutive %llu; textures only %llu\n",
                                     (unsigned long long)reasons[0],(unsigned long long)reasons[1],(unsigned long long)reasons[2],(unsigned long long)reasons[3],(unsigned long long)reasons[4],(unsigned long long)reasons[5],(unsigned long long)reasons[6],(unsigned long long)reasons[7]);
                        std::memset(reasons,0,sizeof reasons); since=std::chrono::steady_clock::now();
                    }
                }
                if (joins) {
                    if (uber_joins) {
                        if (!pending->uber) { pending->pipeline=pending->merge; pending->table=pending->uber=true; pending->push.palette=merge_map; ++uber_stage_draws_[std::min<uint32_t>(gpu_draws[pending->d].key[1],3)]; }   // the run moves onto the ubershader
                        ++uber_joins_; ++uber_stage_draws_[std::min<uint32_t>(info.key[1],3)];
                    } else {
                        if (table_moves) { pending->pipeline=pending->table_pipeline; pending->table=true; }   // the run moves onto the texture table
                        if (pending->merge!=run.merge) pending->merge=VK_NULL_HANDLE;   // a draw that may not go on the ubershader: the run stays as it is
                        frag_uniform_joins_+=set_keys[run_last][16]!=set_keys[run.set][16];   // a block boundary only the fragment-only test removed (against the run's last draw)
                    }
                    run_last=d;
                    for (size_t f=0;f<2;++f) if (pending->few[f]!=run.few[f]) pending->few[f]=VK_NULL_HANDLE;   // a draw with more stages: a larger family or the general ubershader
                    ++pending->draws; pending->pixels+=run.pixels;
                    pending->count+=run.count; pending->positions_end=run.positions_end; pending->attributes_end=run.attributes_end; ++merged_draws_;
                    if (plain_joins && pending->table && !same_textures) {   // joined only through the table
                        ++table_joins_;
                        static const bool log_joins=std::getenv("UC_TEX_MERGE_LOG")!=nullptr;   // diagnostics: each pair of callsites the table joins, once, with their maps
                        static std::set<std::pair<uint32_t,uint32_t>> logged;
                        if (log_joins && logged.insert({packet.draws[pending->d].callsite,draw.callsite}).second) {
                            std::string maps; char text[160];
                            for (size_t map=0;map<8;++map) if (set_keys[run.set][map]) {
                                const auto& a=draw_texture(packet.draws[pending->d],map); const auto& b=draw_texture(draw,map);
                                std::snprintf(text,sizeof text," map%zu %ux%u/%ux%u wrap %02X/%02X",map,a ? a->levels[0].width : 0,a ? a->levels[0].height : 0,b ? b->levels[0].width : 0,b ? b->levels[0].height : 0,
                                              unsigned(packet.draws[pending->d].texture_wrap[map]),unsigned(draw.texture_wrap[map]));
                                maps+=text;
                            }
                            std::fprintf(stderr,"[tex-merge-join] %08X + %08X key0 %08X stages %u depth %02X persp %d%s\n",packet.draws[pending->d].callsite,draw.callsite,info.key[0],info.key[1],info.key[68],int(draw.transforms->projection[3][3]==0),maps.c_str());
                        }
                    }
                }
                else { if (pending) issue_run(*pending); pending=run; run_last=d; }
                continue;
            }
            if (set_shading_rate_) {
                const VkExtent2D rate=draw_rate(draw,stereo_kind,(info.key[68]>>4)&1);
                apply_rate(rate,fovea_draw);
                static const bool rate_stats=std::getenv("UC_GPU_PACKETS")!=nullptr;
                if (rate_stats) {
                    static uint64_t coarse=0, all=0; static VkExtent2D coarse_rate{1,1}; static auto since=std::chrono::steady_clock::now();
                    ++all; if (rate.width*rate.height>1) { ++coarse; coarse_rate=rate; }
                    if (std::chrono::steady_clock::now()-since>std::chrono::seconds(2)) {
                        std::fprintf(stderr,"[vrs] %llu of %llu draws shaded coarsely (%ux%u)\n",(unsigned long long)coarse,(unsigned long long)all,coarse_rate.width,coarse_rate.height);
                        coarse=all=0; since=std::chrono::steady_clock::now();
                    }
                }
            }
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push);
            const bool counted=frag_dump_ && (frag_queries_.size()+1)*layers_<=kFragQueries;
            if (counted) vkCmdBeginQuery(slot.command,frag_pool_,uint32_t(frag_queries_.size()*layers_),VK_QUERY_CONTROL_PRECISE_BIT);
            vkCmdDrawIndexed(slot.command, info.index_count, 1, info.first_index, xform_ ? 0 : info.first_vertex, 0);
            const auto depth=draw.uniforms->pixel[1]&31;
            if((draw.uniforms->pixel[2]&256) && draw.blend.alpha_write && !(depth&1 && ((depth>>1)&7)==0) && (fuse_off() || !fused_destination_alpha(draw))) {
                // GX destination alpha replaces stored alpha after RGB blending.
                const auto alpha_key=pipeline_key(draw,true);
                const auto alpha=pipeline_for(alpha_key);
                bind_pipeline(alpha.pipeline);
                set_draw_state(alpha_key);
                if (alpha.uber) { push.alpha_pass=1; vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push); }
                vkCmdDrawIndexed(slot.command,info.index_count,1,info.first_index,xform_ ? 0 : info.first_vertex,0);
            }
            if (counted) { vkCmdEndQuery(slot.command,frag_pool_,uint32_t(frag_queries_.size()*layers_)); frag_queries_.push_back(uint32_t(d)); }
        }
        if (pending) issue_run(*pending);
    }
    vkCmdEndRendering(slot.command);
    barrier(slot.command, target.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    *target.initialized=true;
    };
    // A layer's pass (hud_layer.h): this packet's draws of that layer (which: 1 HUD, 2 glow) into its L and T, a slice per eye, at the
    // layer's size; the first pass of a frame clears them (L = 0, T = 1). A HUD draw's eye transforms are carried into the layer's plane
    // (hud_layer_block), a glow's are its own; their pipelines are the layer variant of their key (the two attachments, no depth).
    auto record_layer=[&](HudLayer& layer, uint8_t which) {
        static const bool no_merge_hud=std::getenv("UC_NO_MERGE")!=nullptr;
        if (std::find(hud_draw.begin(),hud_draw.end(),which)==hud_draw.end()) return;
        const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        for (VkImage image:{layer.light,layer.transmit})
            barrier(slot.command,image,layer.used ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,all,any,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo colors[2]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}};
        for (int i=0;i<2;++i) {
            colors[i].imageView=i ? layer.transmit_view : layer.light_view; colors[i].imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[i].loadOp=layer.used ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; colors[i].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            for (int c=0;c<4;++c) colors[i].clearValue.color.float32[c]=i ? kHudTransmitClear : 0.f;
        }
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},layer.extent}; rendering.layerCount=1; rendering.viewMask=0b11;
        rendering.colorAttachmentCount=2; rendering.pColorAttachments=colors;
        vkCmdBeginRendering(slot.command,&rendering);
        if (set_shading_rate_) { const VkExtent2D one{1,1}; const VkFragmentShadingRateCombinerOpKHR keep[2]{VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR,VK_FRAGMENT_SHADING_RATE_COMBINER_OP_KEEP_KHR}; set_shading_rate_(slot.command,&one,keep); }
        if (!static_depth_) { vkCmdSetDepthTestEnable(slot.command,VK_FALSE); vkCmdSetDepthWriteEnable(slot.command,VK_FALSE); vkCmdSetDepthCompareOp(slot.command,VK_COMPARE_OP_ALWAYS); }
        vkCmdBindIndexBuffer(slot.command,slot.xform_indices.buffer,0,VK_INDEX_TYPE_UINT32);
        VkPipeline bound=VK_NULL_HANDLE; int bound_cull=-1;
        // The small-draw ubershader (UC_UBER_SMALL=1 or hud, uber_merge.h): consecutive small HUD draws of one fixed-function state, eye block,
        // viewport and scissor are one draw on the ubershader (their TEV setups, blocks and textures read per draw); a run left with one draw
        // is drawn as before.
        struct LayerRun { size_t d; VkPipeline pipeline; bool uber; uint32_t first, count, draws; VkViewport viewport; VkRect2D scissor; DrawPush push; float pixels=0; VkPipeline few[2]{}; };
        std::optional<LayerRun> pending;
        const auto issue=[&](const LayerRun& run) {
            const auto& draw=packet.draws[run.d]; const auto& info=gpu_draws[run.d]; const auto& x=xform_draws[run.d];
            VkPipeline pipeline=run.pipeline; DrawPush push=run.push; bool uber=run.uber;
            if (uber && run.draws==1) { auto key=info.key; key[0]|=kHudLayerKeyBit; pipeline=pipeline_for(key).pipeline; push.palette=x.attributes; uber=false; }
            else if (uber && (run.few[0] || run.few[1])) pipeline=run.few[0] ? run.few[0] : run.few[1];   // the smallest family that holds the run's draws
            if (pipeline!=bound) { vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline); bound=pipeline; ++pipeline_binds_; }
            if (!static_depth_ && int(cull_mode(info.key))!=bound_cull) { vkCmdSetCullMode(slot.command,cull_mode(info.key)); bound_cull=int(cull_mode(info.key)); }
            const uint32_t offsets[2]={draw_uniform_offset[run.d],uint32_t(run.d*stereo_stride_)};
            if (uber) { const VkDescriptorSet sets[3]={slot.texture_sets[draw_set[run.d]],slot.table_set,slot.merge_set}; vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,merge_layout_,0,3,sets,2,offsets); }
            else vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[draw_set[run.d]],2,offsets);
            vkCmdSetViewport(slot.command,0,1,&run.viewport); vkCmdSetScissor(slot.command,0,1,&run.scissor);
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push);
            vkCmdDrawIndexed(slot.command,run.count,1,run.first,0,0);
            ++issued_draws_; hud_.draws+=run.draws; ++hud_layer_runs_; hud_layer_draws_+=run.draws;
            if (uber) { ++hud_uber_runs_; hud_uber_draws_+=run.draws; uber_pixels_+=run.pixels; }
            (void)draw;
        };
        for (size_t d=scene_draws; d<packet.draws.size(); ++d) {
            if (hud_draw[d]!=which) continue;
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d]; const auto& x=xform_draws[d];
            auto key=info.key; key[0]|=kHudLayerKeyBit;
            {   // the draw's own eye transforms (a HUD draw's carried into the layer's plane, hud_layer_block)
                float* target=reinterpret_cast<float*>(static_cast<std::byte*>(slot.motions.mapped)+d*stereo_stride_);
                if (which==1) {
                    float block[32];
                    stereo_eye_block(stereo_->transforms(gl_from_vulkan(draw.transforms->projection),LG_STEREO_HUD),block);
                    hud_layer_block(layer.map,block,target);
                } else {
                    const auto facts=stereo_facts(d);
                    stereo_eye_block(stereo_->transforms(gl_from_vulkan(draw.transforms->projection),lg_stereo_classify_draw(&facts)),target);
                }
            }
            // the layer holds the HUD's viewport (layer.map.viewport) at its own size, its origin -1/12 guest pixel as efb_viewport's
            const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
            const auto& hv=layer.map.viewport;
            const float kx=layer.extent.width/hv[2], ky=layer.extent.height/hv[3];
            const VkViewport viewport{(v[0]-1.f/12-hv[0])*kx,(v[1]-1.f/12-hv[1])*ky,v[2]*kx,v[3]*ky,v[4],v[5]};
            VkRect2D scissor{{0,0},layer.extent};
            if (draw.state->scissor) {   // in guest pixels, as the EFB's (record_pass)
                const auto& sc=*draw.state->scissor;
                const auto sx=std::clamp<int64_t>(std::llround((int64_t(sc[0])-hv[0])*kx),0,layer.extent.width), sy=std::clamp<int64_t>(std::llround((int64_t(sc[1])-hv[1])*ky),0,layer.extent.height);
                const auto right=std::clamp<int64_t>(std::llround((int64_t(sc[0])+sc[2]-hv[0])*kx),sx,layer.extent.width), bottom=std::clamp<int64_t>(std::llround((int64_t(sc[1])+sc[3]-hv[1])*ky),sy,layer.extent.height);
                scissor={{int32_t(sx),int32_t(sy)},{uint32_t(right-sx),uint32_t(bottom-sy)}};
            }
            DrawPush push{draw.transforms->projection,x.attributes,x.word,x.positions,info.key[67],info.bank};
            push.motion=0; push.viewport[0]=viewport.x; push.viewport[1]=viewport.y; push.viewport[2]=viewport.width; push.viewport[3]=viewport.height;
            LayerRun run{d,VK_NULL_HANDLE,false,x.first,info.index_count,1,viewport,scissor,push};
            if (uber_small_hud_ && table_ok && !frag_dump_ && uber_merge_stages_ok(info.key) && draw_pixels(d)<=uber_max_pixels_)
                if (const VkPipeline merge=uber_merge_pipeline(key)) { run.pipeline=merge; run.uber=true; run.push.palette=merge_map; run.pixels=draw_pixels(d); run.few[0]=uber_merge_pipeline(key,1); run.few[1]=uber_merge_pipeline(key,2); }
            if (!run.uber) run.pipeline=pipeline_for(key).pipeline;
            const auto plain=[](DrawPush a) { a.projection={}; a.palette=a.current_row=a.indexed=a.alpha_pass=a.bank=0; return a; };
            const bool joins=pending && run.uber && pending->uber && !no_merge_hud && pending->pipeline==run.pipeline && same_uber_state(gpu_draws[pending->d].key,info.key) &&
                std::memcmp(static_cast<std::byte*>(slot.motions.mapped)+pending->d*stereo_stride_,static_cast<std::byte*>(slot.motions.mapped)+d*stereo_stride_,kStereoBlockBytes)==0 &&
                std::memcmp(&pending->viewport,&run.viewport,sizeof viewport)==0 && std::memcmp(&pending->scissor,&run.scissor,sizeof scissor)==0 &&
                [&] { const DrawPush a=plain(pending->push), b=plain(run.push); return std::memcmp(&a,&b,sizeof a)==0; }() && pending->first+pending->count==run.first;
            if (joins) { pending->count+=run.count; ++pending->draws; pending->pixels+=run.pixels; ++merged_draws_; for (size_t f=0;f<2;++f) if (pending->few[f]!=run.few[f]) pending->few[f]=VK_NULL_HANDLE; }
            else { if (pending) issue(*pending); pending=run; }
        }
        if (pending) issue(*pending);
        vkCmdEndRendering(slot.command);
        if (!layer.used && which==1) ++hud_.frames;
        layer.used=true;
        static const bool stats=std::getenv("UC_GPU_PACKETS")!=nullptr;
        static auto since=std::chrono::steady_clock::now();
        if (stats && which==1 && std::chrono::steady_clock::now()-since>std::chrono::seconds(2)) {
            std::fprintf(stderr,"[hud-layer] 2 s: %llu frames, %llu draws into the layers (HUD %ux%u, glow %ux%u), %llu composited into the EFB\n",(unsigned long long)hud_.frames,(unsigned long long)hud_.draws,
                         hud_.extent.width,hud_.extent.height,glow_.extent.width,glow_.extent.height,(unsigned long long)hud_.flushes);
            hud_.frames=hud_.draws=hud_.flushes=0; since=std::chrono::steady_clock::now();
        }
    };
    // The shadow map: this packet's casters (mesh draws that write depth, in the scene, without an alpha cut-out) drawn from the light, in the packet's own command buffer.
    auto record_shadow=[&](size_t first, size_t last) {
        if (!shadow_fit_ || effects_.shadows==EffectLevel::off || !shadow_vertex_module_) return;
        std::vector<size_t> casters;
        for (size_t d=first;d<last;++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            // the ray tracing instance rule; alpha cut-outs (foliage, fences) are not opaque and cast no shadow here (a limit)
            if (rt_instance_opaque(draw.mesh && info.vertices,info.index_count!=0,summary.draws[d].perspective,summary.draws[d].z_write,draw.uniforms->pixel[0],VkPrimitiveTopology(info.key[69]))==true) casters.push_back(d);
        }
        if (casters.empty()) return;
        const uint32_t resolution=shadow_preset(effects_.shadows).resolution;
        if (shadow_map_.resolution()!=resolution) {   // a preset change: the map is recreated and this frame's earlier casters are lost
            vk_check(present_.wait_idle(device),"shadow map resize");
            shadow_map_.ensure(resolution); shadow_cleared_=false;
        }
        const auto timer_slot=uint32_t(next_slot_);
        pass_timer_.begin(slot.command,timer_slot,GpuPass::shadow);
        barrier(slot.command,shadow_map_.image(),shadow_cleared_ ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; depth.imageView=shadow_map_.view(); depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        depth.loadOp=shadow_cleared_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE; depth.clearValue.depthStencil={1.f,0};
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},{resolution,resolution}}; rendering.layerCount=1; rendering.pDepthAttachment=&depth;
        vkCmdBeginRendering(slot.command,&rendering);
        if (!shadow_pipeline_) shadow_pipeline_=make_shadow_pipeline();
        vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,shadow_pipeline_);
        const VkViewport viewport{0,0,float(resolution),float(resolution),0,1}; const VkRect2D scissor{{0,0},{resolution,resolution}};
        vkCmdSetViewport(slot.command,0,1,&viewport); vkCmdSetScissor(slot.command,0,1,&scissor);
        for (const size_t d:casters) {
            const auto& info=gpu_draws[d];
            VkDeviceSize vertex_offset=info.vertex_offset;
            vkCmdBindVertexBuffers(slot.command,0,1,&info.vertices->buffer,&vertex_offset);
            vkCmdBindIndexBuffer(slot.command,info.indices->buffer,info.index_offset,VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,layout_,0,1,&slot.texture_sets[d],0,nullptr);
            DrawPush push{shadow_fit_->view_to_light,info.palette,info.current,info.indexed,0,info.bank};   // the light's matrix takes the place of the projection; motion stays "none"
            push.viewport[2]=push.viewport[3]=float(resolution);   // the vertex stage divides its jitter by the viewport (zero jitter here)
            vkCmdPushConstants(slot.command,layout_,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,uint32_t(sizeof push),&push);
            vkCmdDrawIndexed(slot.command,info.index_count,1,info.first_index,info.first_vertex,0);
        }
        vkCmdEndRendering(slot.command);
        shadow_cleared_=true;
        pass_timer_.end(slot.command,timer_slot,GpuPass::shadow);
    };
    // Ray tracing: this packet's instances (the caster rule, cut-outs included as non-opaque) into one BLAS, in the packet's own command buffer.
    auto record_rt=[&](size_t first, size_t last) {
        if (!rt_frame_) return;
        const auto address=[&](const Buffer& buffer) { VkBufferDeviceAddressInfo info{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO}; info.buffer=buffer.buffer; return vkGetBufferDeviceAddress(device,&info); };
        std::vector<RtGeometry> geometries;
        for (size_t d=first;d<last;++d) {
            const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
            const auto opaque=rt_instance_opaque(draw.mesh && info.vertices,info.index_count!=0,summary.draws[d].perspective,summary.draws[d].z_write,draw.uniforms->pixel[0],VkPrimitiveTopology(info.key[69]));
            if (!opaque) continue;
            RtGeometry geometry;
            geometry.vertices=address(*info.vertices)+info.vertex_offset; geometry.indices=address(*info.indices)+info.index_offset;
            geometry.vertex_count=uint32_t(draw.mesh->vertices.size()); geometry.index_count=info.index_count; geometry.first_index=info.first_index; geometry.first_vertex=info.first_vertex;
            geometry.palette=info.palette; geometry.current=info.current; geometry.indexed=info.indexed; geometry.opaque=*opaque; geometry.palettes=address(slot.palettes);
            if (rt_records && d<draw_record.size()) geometry.record=draw_record[d];
            geometry.skinned=info.indexed!=0;   // characters and enemies: a rig light's rays see only these (the instance mask), a lamp's rays see the whole scene
            if (!geometry.skinned) {   // a static draw: its mesh's BLAS is cached in object space, the instance carries the draw's object-to-view rows (current row and the two after it)
                geometry.mesh_id=info.mesh_id;
                for (uint32_t row=0;row<3;++row) geometry.transform[row]=draw.transforms->position_rows[info.current+row];
            }
            geometries.push_back(geometry);
        }
        if (geometries.empty()) return;
        const auto timer_slot=uint32_t(next_slot_);
        pass_timer_.begin(slot.command,timer_slot,GpuPass::rt_as);
        rt_.record_packet(slot.command,geometries);
        if (upscale_here) rt_as_open_=true;   // the boundary's TLAS build follows in this submission: one timed span (a query is written once per submission)
        else pass_timer_.end(slot.command,timer_slot,GpuPass::rt_as);
    };
    const bool scene_only=scene_phase_ && !upscale_here && !overlay_frame;   // the whole packet belongs to the scene: copy and clear act on the scene targets
    const bool writes_scene=scene_phase_ && !overlay_frame && (scene_draws || !scene_initialized_);   // overwrites the depth and vectors frame generation may still read
    // The headset's bloom proxy (UC_NO_BLOOM_PROXY=1: off): the game's bloom chain draws 2D quads into the EFB's top-left corner and copies that
    // corner (319x179, 320x180, 160x90), pass after pass, then restores the corner (an opaque quad) before anything else reads it. Such a
    // packet (its copy and every draw inside the corner, 2D, no depth) is drawn into a proxy of the EFB at 2 points a guest pixel instead of
    // the EFB's 3.2 x 6.1 (native), and its copy is made from the proxy. The corner's content stays in the proxy until the next other
    // packet that draws, copies or presents, which first puts it back into the EFB (an upscaling blit: the corner as the chain left it,
    // at the proxy's resolution).
    bool bloom_chain=false;
    std::array<float,4> chain_box{1e9f,1e9f,-1e9f,-1e9f};
    {
        static const bool on=std::getenv("UC_NO_BLOOM_PROXY")==nullptr;
        if (on && layers_>1 && copy_target && !packet.clear_after_copy && !packet.present && !overlay_frame && !scene_phase_ && !scene_draws && efb_initialized_ &&
            std::min(efb_extent_.width/640.f,efb_extent_.height/480.f)>=2.5f) {
            const auto& src=packet.texture_copy->source;
            bloom_chain=src[0]+src[2]<=kBloomCorner[0] && src[1]+src[3]<=kBloomCorner[1] && (!packet.draws.empty() || bloom_proxy_.valid);
            for (size_t d=0; d<packet.draws.size() && bloom_chain; ++d) {
                const auto& draw=packet.draws[d]; const auto& info=gpu_draws[d];
                if (!info.index_count) continue;
                const uint32_t depth=info.key[68];
                const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
                const auto box=gx::draw_bounds_ndc(draw);
                const bool inside=!(draw.transforms->projection[3][3]==0) && !draw.reticle && !draw.late_aim && !draw.wake && !draw.layer && !(depth&16) && (!(depth&1) || ((depth>>1)&7)==7) && box &&
                    v[0]+(std::max((*box)[0],-1.f)*.5f+.5f)*v[2]>=-.5f && v[1]+(std::max((*box)[1],-1.f)*.5f+.5f)*v[3]>=-.5f &&
                    v[0]+(std::min((*box)[2],1.f)*.5f+.5f)*v[2]<=kBloomCorner[0]+.5f && v[1]+(std::min((*box)[3],1.f)*.5f+.5f)*v[3]<=kBloomCorner[1]+.5f;
                bloom_chain=inside;
                if (inside) {   // what this draw covers, in guest pixels with the viewport's -1/12 origin (efb_viewport): the part put back
                    chain_box[0]=std::min(chain_box[0],v[0]-1.f/12+(std::max((*box)[0],-1.f)*.5f+.5f)*v[2]); chain_box[1]=std::min(chain_box[1],v[1]-1.f/12+(std::max((*box)[1],-1.f)*.5f+.5f)*v[3]);
                    chain_box[2]=std::max(chain_box[2],v[0]-1.f/12+(std::min((*box)[2],1.f)*.5f+.5f)*v[2]); chain_box[3]=std::max(chain_box[3],v[1]-1.f/12+(std::min((*box)[3],1.f)*.5f+.5f)*v[3]);
                }
            }
        }
        // UC_FOLD_CLEAR: a waiting clear goes first where something else than this packet's EFB passes reads or writes the EFB before them
        if (efb_clear_.pending && (bloom_chain || bloom_proxy_.valid || hud_.used || glow_.used || upscale_here || writes_scene || dlss_generation_enabled() || fsr_generation_enabled() || overlay_frame))
            record_efb_clear(slot.command,bloom_chain ? "bloom chain" : bloom_proxy_.valid ? "bloom proxy" : hud_.used || glow_.used ? "layers" : "scene/upscale/overlay");
        if (!bloom_chain && bloom_proxy_.valid && (!packet.draws.empty() || copy_target || packet.present)) put_back_bloom_proxy(slot.command);
        if (bloom_chain) {
            ensure_bloom_proxy();
            if (!bloom_proxy_.valid) bloom_proxy_corner(slot.command,false);   // the corner as the EFB has it: blended draws build on it
        }
    }
    if ((hud_.used || glow_.used) && !overlay_frame && (!packet.draws.empty() || copy_target)) flush_hud_layer(slot.command);   // the HUD layer goes under what this packet draws or into its copy
    if (writes_scene) { record_pass(scene_target,0,scene_draws,true); record_shadow(0,scene_draws); record_rt(0,scene_draws); }
    if (upscale_here) {
        if (scene_draws || packet.present) scene_restore_copy_.reset();   // another draw changed the scene since the saved copy
        record_upscale(slot.command,packet.black_output);
    }
    // DLSS-G's HUD-less colour: the frame after its post chain, before the HUD packet (the first packet after the scene boundary that draws
    // without copying), at back-buffer size; at the present when no such packet came (it then includes the HUD).
    const bool hud_packet=!packet.texture_copy && !packet.draws.empty();
    auto take_hudless=[&] {
        const auto viewport=boundary_.scene_viewport().value_or(std::array<float,6>{0,0,640,480,0,1});
        const auto hudless=hudless_image();
        VkImageBlit blit{}; blit.srcSubresource=blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
        blit.srcOffsets[0]={int32_t(viewport[0]*efb_extent_.width/640),int32_t(viewport[1]*efb_extent_.height/480),0};
        blit.srcOffsets[1]={int32_t((viewport[0]+viewport[2])*efb_extent_.width/640),int32_t((viewport[1]+viewport[3])*efb_extent_.height/480),1};
        blit.dstOffsets[1]={int32_t(hudless_extent_.width),int32_t(hudless_extent_.height),1};
        if (hudless_format_==HdrPass::kFormat) {   // HDR: encoded like the back buffer it is compared with
            const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
            barrier(slot.command,hudless.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,all,any,all,any);
            hdr_pass_.record(slot.command,efb_view_,{blit.srcOffsets[0].x,blit.srcOffsets[0].y},
                             {uint32_t(std::max(1,blit.srcOffsets[1].x-blit.srcOffsets[0].x)),uint32_t(std::max(1,blit.srcOffsets[1].y-blit.srcOffsets[0].y))},
                             VK_NULL_HANDLE,hudless.view,hudless_extent_,hdr_settings_,
                             bloom_layer({blit.srcOffsets[0].x,blit.srcOffsets[0].y},{uint32_t(std::max(1,blit.srcOffsets[1].x-blit.srcOffsets[0].x)),uint32_t(std::max(1,blit.srcOffsets[1].y-blit.srcOffsets[0].y))}));
            barrier(slot.command,efb_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
            barrier(slot.command,hudless.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,VK_ACCESS_2_MEMORY_READ_BIT);
        } else {
            barrier(slot.command,hudless.image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdBlitImage(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,hudless.image,VK_IMAGE_LAYOUT_GENERAL,1,&blit,VK_FILTER_NEAREST);   // as the present copies
            barrier(slot.command,hudless.image,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
        }
        if (dlss_generation_enabled()) dlss_g_->tag_hudless(slot.command,hudless);
        hudless_taken_=true;
    };
    const bool hudless_wanted=(dlss_generation_enabled() || (fsr_generation_enabled() && fsr_g_->active())) && hudless_[0] && scene_mode_ && !scene_phase_ && !hudless_taken_ && !overlay_frame;
    if (hudless_wanted && efb_initialized_ && hud_packet) take_hudless();
    if (bloom_chain) {
        if (!packet.draws.empty()) {
            const PassTarget proxy_target{bloom_proxy_.color,bloom_proxy_.depth,bloom_proxy_.color_view,bloom_proxy_.depth_view,VK_NULL_HANDLE,VK_NULL_HANDLE,bloom_proxy_.extent,&bloom_proxy_.initialized,true};
            record_pass(proxy_target,0,packet.draws.size(),false);
        }
        if (!bloom_proxy_.valid) bloom_proxy_.covered={1e9f,1e9f,-1e9f,-1e9f};
        for (int i=0;i<2;++i) { bloom_proxy_.covered[i]=std::min(bloom_proxy_.covered[i],chain_box[i]); bloom_proxy_.covered[i+2]=std::max(bloom_proxy_.covered[i+2],chain_box[i+2]); }
        bloom_proxy_.valid=true; ++bloom_proxy_.packets;
    }
    else if (particle_last>particle_first) {   // the particle layer: the draws before, the run in the layer and composited, the draws after
        record_pass(efb_target,scene_draws,particle_last,false);   // the draws before the run and the run's full-size sprites (additions commute)
        ensure_particle_layer();
        if (particle_.scene_depth_view!=depth_view_) {   // the EFB (and its depth view) is remade with the render scale
            const VkDescriptorImageInfo image{VK_NULL_HANDLE,depth_view_,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet=particle_.depth_set; write.dstBinding=0; write.descriptorCount=1; write.descriptorType=VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE; write.pImageInfo=&image;
            vkUpdateDescriptorSets(device,1,&write,0,nullptr);
            particle_.scene_depth_view=depth_view_;
        }
        const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        // the scene's depth at the layer's size (particle_depth.slang: the nearest of each 2x2 block), and L = 0, T = 1
        barrier(slot.command,depth_,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(slot.command,particle_.depth,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,all,any,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        {
            VkRenderingAttachmentInfo target{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; target.imageView=particle_.depth_view; target.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
            target.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; target.storeOp=VK_ATTACHMENT_STORE_OP_STORE;   // every texel written
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO}; rendering.renderArea={{0,0},particle_.extent}; rendering.layerCount=1; rendering.viewMask=0b11; rendering.pDepthAttachment=&target;
            vkCmdBeginRendering(slot.command,&rendering);
            const VkViewport viewport{0,0,float(particle_.extent.width),float(particle_.extent.height),0,1}; const VkRect2D scissor{{0,0},particle_.extent};
            vkCmdSetViewport(slot.command,0,1,&viewport); vkCmdSetScissor(slot.command,0,1,&scissor);
            vkCmdBindPipeline(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,particle_.depth_pipeline);
            vkCmdBindDescriptorSets(slot.command,VK_PIPELINE_BIND_POINT_GRAPHICS,particle_.depth_layout,0,1,&particle_.depth_set,0,nullptr);
            vkCmdDraw(slot.command,3,1,0,0);
            vkCmdEndRendering(slot.command);
        }
        barrier(slot.command,depth_,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
        barrier(slot.command,particle_.color,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue clear{{0,0,0,1}}; const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,VK_REMAINING_ARRAY_LAYERS};
        vkCmdClearColorImage(slot.command,particle_.color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&clear,1,&range);
        barrier(slot.command,particle_.color,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,all,any);
        particle_.initialized=true;
        const PassTarget particle_target{particle_.color,particle_.depth,particle_.color_view,particle_.depth_view,VK_NULL_HANDLE,VK_NULL_HANDLE,particle_.extent,&particle_.initialized};
        particle_pass=true; record_pass(particle_target,particle_first,particle_last,false); particle_pass=false;
        composite_particle_layer(slot.command);
        ++particle_.packets; particle_.draws+=size_t(std::count(particle_draw.begin(),particle_draw.end(),1));
        static const bool stats=std::getenv("UC_GPU_PACKETS")!=nullptr;
        static auto since=std::chrono::steady_clock::now();
        if (stats && std::chrono::steady_clock::now()-since>std::chrono::seconds(2)) {
            std::fprintf(stderr,"[particle-layer] 2 s: %llu packets, %llu draws at %ux%u a layer\n",(unsigned long long)particle_.packets,(unsigned long long)particle_.draws,particle_.extent.width,particle_.extent.height);
            particle_.packets=particle_.draws=0; since=std::chrono::steady_clock::now();
        }
        particle_draw.clear();
        if (particle_last<packet.draws.size()) record_pass(efb_target,particle_last,packet.draws.size(),false);
    }
    else if (!scene_only && !overlay_frame && (!efb_initialized_ || scene_draws<packet.draws.size())) { record_pass(efb_target,scene_draws,packet.draws.size(),false); record_layer(glow_,2); record_layer(hud_,1); }
    if (hudless_wanted && !hudless_taken_ && packet.present) take_hudless();
    // UC_FOLD_CLEAR: no EFB pass of this packet took a waiting clear. An empty packet (no draws, copy, present or clear: one follows every
    // present in the fight) leaves the EFB alone, so the clear waits on; anything else may read or write the EFB from here on.
    if (!packet.draws.empty() || copy_target || packet.present || packet.clear_only || packet.clear_after_copy || overlay_frame) record_efb_clear(slot.command,packet.draws.empty() ? "copy or present" : "draws, no EFB pass");
    const PassTarget& active=scene_only ? scene_target : efb_target;
    if(copy_target) {
        if (bloom_chain) record_texture_copy(slot.command,bloom_proxy_.color,bloom_proxy_.extent,packet.texture_copy->source,*copy_target,VK_IMAGE_LAYOUT_UNDEFINED);
        else record_texture_copy(slot.command,active.color,active.extent,packet.texture_copy->source,*copy_target,VK_IMAGE_LAYOUT_UNDEFINED);
    }
    if (scene_only) {
        if (copy_target && !packet.clear_after_copy) scene_restore_copy_=packet.texture_copy;
        else if (!packet.draws.empty() || (copy_target && packet.clear_after_copy)) scene_restore_copy_.reset();
    }
    bool ui_layer=false;   // HDR: the overlays went to the slot's UI layer, the encode composites them
    // The headset reads the presented EFB region in place: the copy into the slot image (2 x 2064x2208 at the native scale, 1.35 ms on the
    // Quest) is made only for a frame that may be captured (keep_frame), held for the reticle layer or blanked. UC_XR_VIA_SLOT=1: always copy.
    static const bool xr_via_slot=std::getenv("UC_XR_VIA_SLOT")!=nullptr;
    const bool capture_xr=xr_ && keep_frame_ && std::getenv("UC_CAPTURE_XR");
#ifdef UC_OPENXR
    const bool xr_direct=xr_ && xr_frame_ && packet.present && display && !overlay_frame && !packet.black_output && !hold && (!keep_frame_ || capture_xr) && !xr_via_slot;
#else
    const bool xr_direct=false; (void)xr_via_slot;
#endif
    if ((hud_.used || glow_.used) && packet.present) {   // the layers: laid over the eyes by the screen pass (xr_direct), else into the EFB the present reads
        if (!display || overlay_frame || packet.black_output) hud_.used=glow_.used=false;
        else if (!xr_direct) flush_hud_layer(slot.command);
    }
    if (packet.present && display && !overlay_frame) {
        const bool prompts=layers_==1 && !packet.prompts.empty() && !packet.black_output, menu=layers_==1 && bool(menu_build_);   // the headset has HOTD4's panel instead
        ui_layer=hdr_active_ && (prompts || menu);
        if(prompts)prompt_overlay_.initialize(instance_,physical_,device,queue_,family_,uint32_t(slots_.size()));
        if(menu)menu_overlay_.initialize(window_,instance_,physical_,device,queue_,family_,uint32_t(slots_.size()),menu_setup_);
        if(!packet.black_output && dlss5_scene_.ready && dlss5_requested() && dlss5_settings_.hook==Dlss5Hook::present) {   // DLSS 5 on the finished frame, HUD included
            const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            const VkOffset2D origin{int32_t(uint64_t(display_source[0])*efb_extent_.width/640),int32_t(uint64_t(display_source[1])*efb_extent_.height/480)};
            const VkExtent2D size{uint32_t(std::max<int64_t>(1,int64_t(uint64_t(display_source[0]+display_source[2])*efb_extent_.width/640)-origin.x)),uint32_t(std::max<int64_t>(1,int64_t(uint64_t(display_source[1]+display_source[3])*efb_extent_.height/480)-origin.y))};
            const VkImageUsageFlags usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT;
            barrier(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,all,any,all,any);
            record_dlss5(slot.command,Dlss5Hook::present,{efb_,efb_view_,VK_FORMAT_R8G8B8A8_UNORM,efb_extent_,usage},origin,size,{0,0},packet.black_output);
            barrier(slot.command,efb_,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,all,any);
        }
        // Direct XR presentation never reads this slot. Do not transition an
        // unused image twice or mark an untouched image as initialized. Captures
        // still prepare it for XrCompositor::copy_capture below.
        if (!xr_direct || capture_xr) {
        barrier(slot.command,slot.image,slot.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                slot.initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,slot.initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit copy{}; copy.srcSubresource=copy.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers_};
        copy.srcOffsets[0]={int32_t(uint64_t(display_source[0])*efb_extent_.width/640),int32_t(uint64_t(display_source[1])*efb_extent_.height/480),0};
        copy.srcOffsets[1]={std::max(copy.srcOffsets[0].x+1,int32_t(uint64_t(display_source[0]+display_source[2])*efb_extent_.width/640)),std::max(copy.srcOffsets[0].y+1,int32_t(uint64_t(display_source[1]+display_source[3])*efb_extent_.height/480)),1};
        copy.dstOffsets[1]={int32_t(extent_.width),int32_t(extent_.height),1};
        if(packet.black_output) {
            const VkClearColorValue black{}; const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&black,1,&range);
        } else if (!xr_direct) vkCmdBlitImage(slot.command,efb_,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy,VK_FILTER_NEAREST);
        const auto record_overlays=[&](VkImageView view,VkAttachmentLoadOp load) {
            VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};color.imageView=view;color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;color.loadOp=load;color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo ui{VK_STRUCTURE_TYPE_RENDERING_INFO};ui.renderArea.extent=extent_;ui.layerCount=1;ui.colorAttachmentCount=1;ui.pColorAttachments=&color;
            vkCmdBeginRendering(slot.command,&ui);
            if(prompts)prompt_overlay_.record(slot.command,packet.prompts,extent_,display_source,prompt_style_);
            if(menu)menu_overlay_.record(slot.command,extent_,menu_build_);
            vkCmdEndRendering(slot.command);
        };
        if((prompts || menu) && !ui_layer) {
            barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            record_overlays(slot.view,VK_ATTACHMENT_LOAD_OP_LOAD);
            barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        } else
        barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        if(ui_layer) {   // a transparent layer of its own: the ImGui blend over zero leaves premultiplied colour
            barrier(slot.command,slot.ui_image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,0,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            record_overlays(slot.ui_view,VK_ATTACHMENT_LOAD_OP_CLEAR);
            barrier(slot.command,slot.ui_image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        }
    }
#ifdef UC_OPENXR
    if (xr_direct) {   // before the clear after the present empties the region: the displayed rectangle of the EFB, in texture coordinates
        const float u0=float(uint64_t(display_source[0])*efb_extent_.width/640), v0=float(uint64_t(display_source[1])*efb_extent_.height/480);
        const float u1=float(std::max<uint64_t>(uint64_t(u0)+1,uint64_t(display_source[0]+display_source[2])*efb_extent_.width/640));
        const float v1=float(std::max<uint64_t>(uint64_t(v0)+1,uint64_t(display_source[1]+display_source[3])*efb_extent_.height/480));
        const float w=float(efb_extent_.width), h=float(efb_extent_.height);
        XrHudLayer hud;
        for (HudLayer* layer:{&hud_,&glow_}) if (layer->used)
            for (VkImage image:{layer->light,layer->transmit}) barrier(slot.command,image,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        if (hud_.used) { hud.light=hud_.light_view; hud.transmit=hud_.transmit_view; hud.rows=hud_.map.rows; }
        if (glow_.used) {   // the glow layer's map is affine and the same in both eyes
            hud.glow_light=glow_.light_view; hud.glow_transmit=glow_.transmit_view;
            hud.glow={glow_.map.rows[0][0][0],glow_.map.rows[0][0][2],glow_.map.rows[0][1][1],glow_.map.rows[0][1][2]};
        }
        xr_->record(slot.command, efb_, efb_view_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, {u0/w, v0/h, (u1-u0)/w, (v1-v0)/h}, fovea_level_ ? fovea_eye_.view : VK_NULL_HANDLE, fovea_texel_, (hud_.used || glow_.used) ? &hud : nullptr);
        hud_.used=glow_.used=false;
    }
#endif
    if (hold) {   // keep the finished picture (without the reticle) and the reticle draws that belong on it: the overlay frame after this one presents them
        const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier(slot.command,held_.image,held_.image_ready ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageBlit whole{}; whole.srcSubresource=whole.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; whole.srcOffsets[1]=whole.dstOffsets[1]={int32_t(extent_.width),int32_t(extent_.height),1};
        vkCmdBlitImage(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,held_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&whole,VK_FILTER_NEAREST);
        barrier(slot.command,held_.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        held_.image_ready=true; held_.valid=true; held_.presented=false; held_.display_source=display_source;
        held_.draws=std::move(frame_reticle_); frame_reticle_.clear();
        if (std::getenv("UC_RETICLE_DUMP")) { static std::map<size_t,uint64_t> counts; ++counts[held_.draws.size()]; static uint64_t holds=0; if ((++holds%200)==0) { std::fprintf(stderr,"[reticle-layer] reticle draws per held frame:"); for (const auto& [n,c]:counts) std::fprintf(stderr," %zu x%llu",n,(unsigned long long)c); std::fprintf(stderr,"\n"); } }
    } else if (overlay_frame) {   // the held picture again, the reticle drawn over it where the mouse is now
        const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
        barrier(slot.command,slot.image,slot.initialized ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                slot.initialized ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_NONE,slot.initialized ? VK_ACCESS_2_TRANSFER_READ_BIT : 0,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT);
        barrier(slot.command,held_.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,all,any,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);   // after the copy that made it
        VkImageBlit whole{}; whole.srcSubresource=whole.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; whole.srcOffsets[1]=whole.dstOffsets[1]={int32_t(extent_.width),int32_t(extent_.height),1};
        vkCmdBlitImage(slot.command,held_.image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&whole,VK_FILTER_NEAREST);
        barrier(slot.command,slot.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
        bool overlay_initialized=true;
        const PassTarget overlay_target{slot.image,held_.depth,slot.view,held_.depth_view,VK_NULL_HANDLE,VK_NULL_HANDLE,extent_,&overlay_initialized,true};
        if (!packet.draws.empty()) record_pass(overlay_target,0,packet.draws.size(),false);
    }
    static const bool full_render_area_clears = std::getenv("UC_FULL_RENDER_AREA") != nullptr;
    auto clear_region=[&](const PassTarget& active, bool with_motion) {   // after a copy: GX clears the copied EFB region when asked
            barrier(slot.command,active.depth,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,depth_stages,VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
            barrier(slot.command,active.color,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT|VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            VkRenderingAttachmentInfo colors[5]{{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO},{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}},depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            const bool clear_motion=with_motion && active.motion_view && motion_cleared_;   // the scene's vectors in the cleared region go with it
            const bool clear_gbuffer=with_motion && active.gbuffer_view && gbuffer_cleared_ && scene_gbuffer_layout_==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // and so do its surfaces
            const bool clear_rt=with_motion && active.gbuffer_view && scene_material_ && rt_material_cleared_ && scene_rt_layout_==VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // and their records
            for (auto [attachment,view]:{std::pair{&colors[0],active.color_view},std::pair{&colors[1],clear_motion ? active.motion_view : VK_NULL_HANDLE},std::pair{&colors[2],clear_gbuffer ? active.gbuffer_view : VK_NULL_HANDLE},
                                         std::pair{&colors[3],clear_rt ? scene_material_view_ : VK_NULL_HANDLE},std::pair{&colors[4],clear_rt ? scene_albedo_view_ : VK_NULL_HANDLE}}) {
                attachment->imageView=view; attachment->imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; attachment->loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; attachment->storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            }
            depth.imageView=active.depth_view; depth.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL; depth.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD; depth.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo clear_render{VK_STRUCTURE_TYPE_RENDERING_INFO}; clear_render.renderArea.extent=active.extent; clear_render.layerCount=1; clear_render.viewMask=layers_>1 ? 0b11u : 0u; clear_render.colorAttachmentCount=clear_rt ? 5 : clear_gbuffer ? 3 : clear_motion ? 2 : 1; clear_render.pColorAttachments=colors; clear_render.pDepthAttachment=&depth;
            const auto& source=copy_target ? packet.texture_copy->source : packet.display_source;
            const uint32_t x=uint64_t(source[0])*active.extent.width/640,y=uint64_t(source[1])*active.extent.height/480;
            const uint32_t right=std::max(x+1,uint32_t(uint64_t(source[0]+source[2])*active.extent.width/640)),bottom=std::max(y+1,uint32_t(uint64_t(source[1]+source[3])*active.extent.height/480));
            VkClearRect rectangle{{{int32_t(x),int32_t(y)},{right-x,bottom-y}},0,1};
            // Only the cleared rectangle: every attachment of this pass is cleared over exactly its render area, so nothing is loaded
            // (on a tiler the rest of the EFB is neither read nor written back).
            if (!full_render_area_clears) {
                clear_render.renderArea=rectangle.rect;
                for (auto& attachment:colors) attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
                depth.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            }
            VkClearAttachment clears[6]{}; clears[0].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[1].aspectMask=VK_IMAGE_ASPECT_DEPTH_BIT;
            uint32_t clear_count=2;
            if (clear_motion) { clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count++].colorAttachment=1; }
            if (clear_gbuffer) { clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count++].colorAttachment=2; }   // cleared to zero: no surface
            if (clear_rt) {   // no record, no albedo
                clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count].clearValue.color.uint32[0]=kRtNoRecord; clears[clear_count++].colorAttachment=3;
                clears[clear_count].aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; clears[clear_count++].colorAttachment=4;
            }
            for(size_t i=0;i<4;++i) clears[0].clearValue.color.float32[i]=packet.clear_color[i]/255.f;
            clears[1].clearValue.depthStencil={packet.clear_depth/float(0xFFFFFF),0};
            vkCmdBeginRendering(slot.command,&clear_render);
            vkCmdClearAttachments(slot.command,clear_count,clears,1,&rectangle);
            vkCmdEndRendering(slot.command);
            barrier(slot.command,active.color,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_READ_BIT);
    };
    // The copy's clear folded into the next EFB pass's load op (Dolphin's ClearRegion): on by default in VR (headset A/B at render scale 1,
    // foveation high: 12.6 -> 12.0 ms with the 24-bit depth); UC_FOLD_CLEAR=0 / 1 decides otherwise.
    static const char* fold_env=std::getenv("UC_FOLD_CLEAR");
    const bool fold_clear=fold_env && *fold_env ? *fold_env!='0' : layers_>1;
    if (packet.clear_after_copy && (packet.present || copy_target || packet.clear_only)) {
        if (fold_clear && !scene_only && &active==&efb_target && !full_render_area_clears && efb_initialized_) {   // the next EFB pass applies it (efb_clear_)
            const auto& source=copy_target ? packet.texture_copy->source : packet.display_source;
            const uint32_t x=uint64_t(source[0])*efb_extent_.width/640,y=uint64_t(source[1])*efb_extent_.height/480;
            const uint32_t right=std::max(x+1,uint32_t(uint64_t(source[0]+source[2])*efb_extent_.width/640)),bottom=std::max(y+1,uint32_t(uint64_t(source[1]+source[3])*efb_extent_.height/480));
            efb_clear_.pending=true; efb_clear_.rect={{int32_t(x),int32_t(y)},{right-x,bottom-y}};
            for(size_t i=0;i<4;++i) efb_clear_.color.float32[i]=packet.clear_color[i]/255.f;
            efb_clear_.depth=packet.clear_depth/float(0xFFFFFF);
        } else { clear_region(active,true); ++clear_passes_; }
        // The scene targets keep this frame until it is presented (frame generation reads depth and vectors there); the next frame's first
        // scene pass applies this clear to them.
        if (packet.present && upscaler_ && !scene_only) scene_clear_={true,packet.display_source,packet.clear_color,packet.clear_depth};
    }
#ifdef UC_OPENXR
    if (xr_ && xr_frame_ && packet.present && display && !xr_direct) xr_->record(slot.command, slot.image, slot.view, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, {0, 0, 1, 1}, fovea_level_ ? fovea_eye_.view : VK_NULL_HANDLE, fovea_texel_);
    if (capture_xr && xr_frame_ && packet.present && display) xr_->copy_capture(slot.command,slot.image,extent_);
#endif
    if(present_image) {
        const auto present_layout=fsr_presentation_ ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : streamline_swapchain() ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (hdr_active_) {   // the slot image stays SDR (captures, readback); the swapchain gets its HDR10 encoding
            const auto all=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT; const auto any=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
            barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            barrier(slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, all, any, all, any);
            const VkOffset2D glow_from{int32_t(uint64_t(display_source[0])*efb_extent_.width/640),int32_t(uint64_t(display_source[1])*efb_extent_.height/480)};   // the EFB rectangle the slot image shows (the blit above)
            const VkOffset2D glow_to{std::max(glow_from.x+1,int32_t(uint64_t(display_source[0]+display_source[2])*efb_extent_.width/640)),std::max(glow_from.y+1,int32_t(uint64_t(display_source[1]+display_source[3])*efb_extent_.height/480))};
            hdr_pass_.record(slot.command, slot.view, {0, 0}, extent_, ui_layer ? slot.ui_view : VK_NULL_HANDLE, present_image->view, extent_, hdr_settings_,
                             packet.black_output ? BloomLayer{} : bloom_layer(glow_from,{uint32_t(glow_to.x-glow_from.x),uint32_t(glow_to.y-glow_from.y)}));   // a blanked frame is black: the scene boundary came in an earlier packet and could not know
            bloom_pass_.invalidate(0);   // this frame's glow is spent: a later frame without a scene must not find it
            barrier(slot.command, slot.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, all, any, all, any);
            barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, present_layout,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        } else {
            barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkImageBlit blit{}; blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = blit.dstOffsets[1] = {int32_t(extent_.width), int32_t(extent_.height), 1};
            if (preview_) {   // the stereo preview: the left eye's layer on the window's left half, the right eye's on the right half
                VkImageBlit eyes[2]{blit, blit};
                for (uint32_t eye = 0; eye < 2; ++eye) {
                    eyes[eye].srcSubresource.baseArrayLayer = eye;
                    eyes[eye].dstOffsets[0] = {int32_t(extent_.width * eye / 2), 0, 0};
                    eyes[eye].dstOffsets[1] = {int32_t(extent_.width * (eye + 1) / 2), int32_t(extent_.height), 1};
                }
                vkCmdBlitImage(slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 2, eyes, VK_FILTER_LINEAR);
            } else
            vkCmdBlitImage(slot.command, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
            barrier(slot.command, present_image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,present_layout,
                VK_PIPELINE_STAGE_2_TRANSFER_BIT,VK_ACCESS_2_TRANSFER_WRITE_BIT,VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,VK_ACCESS_2_MEMORY_READ_BIT);
        }
    }
    vkCmdWriteTimestamp2(slot.command, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, timing_pool_, first_query + 1);
    vk_check(vkEndCommandBuffer(slot.command), "end frame"); vk_check(vkResetFences(device, 1, &slot.fence), "reset frame fence");
    VkSemaphoreSubmitInfo waits[2]{{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}}; uint32_t wait_count = 0;
    if (present_image) { waits[wait_count].semaphore = slot.acquired; waits[wait_count].stageMask = hdr_active_ ? VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT : VK_PIPELINE_STAGE_2_TRANSFER_BIT; ++wait_count; }   // the first write of the swapchain image
    if (writes_scene && dlss_g_ && dlss_g_->inputs_fence() && dlss_g_->inputs_fence_value() > fg_waited_value_) {   // DLSS-G guide §15.1
        waits[wait_count].semaphore = dlss_g_->inputs_fence(); waits[wait_count].value = dlss_g_->inputs_fence_value(); waits[wait_count].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        fg_waited_value_ = waits[wait_count].value; ++wait_count;
    }
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO}; signal.semaphore = present_image ? present_image->ready : VK_NULL_HANDLE; signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO}; command.commandBuffer = slot.command;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2}; submit.waitSemaphoreInfoCount = wait_count; submit.signalSemaphoreInfoCount = present_image ? 1 : 0; submit.commandBufferInfoCount = 1;
    submit.pWaitSemaphoreInfos = waits; submit.pSignalSemaphoreInfos = &signal; submit.pCommandBufferInfos = &command;
    if (packet.present && !overlay_frame && upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active()) streamline_->marker(Streamline::Marker::render_submit_end);
    const auto submit_start = std::chrono::steady_clock::now();
    vk_check(vkQueueSubmit2(queue_, 1, &submit, slot.fence), "submit frame");
    if (frag_dump_ && !frag_queries_.empty()) {
        vk_check(vkQueueWaitIdle(queue_), "fragment dump wait");
        std::vector<uint64_t> counts(frag_queries_.size()*layers_);
        vkGetQueryPoolResults(device, frag_pool_, 0, uint32_t(counts.size()), counts.size()*sizeof(uint64_t), counts.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT);
        std::vector<std::pair<uint64_t,size_t>> per;   // fragments (all eyes), index into frag_queries_
        uint64_t total=0;
        for (size_t i=0;i<frag_queries_.size();++i) { uint64_t n=0; for (uint32_t v=0;v<layers_;++v) n+=counts[i*layers_+v]; per.push_back({n,i}); total+=n; }
        std::sort(per.rbegin(),per.rend());
        uint64_t bank_indices=0, plain_indices=0;
        for (const uint32_t d:frag_queries_) (gpu_draws[d].bank!=kNoBank ? bank_indices : plain_indices)+=gpu_draws[d].index_count;
        std::fprintf(stderr,"[frag-dump] indices: %llu with a texture bank, %llu without\n",(unsigned long long)bank_indices,(unsigned long long)plain_indices);
        const double screen=double(efb_extent_.width)*efb_extent_.height*0.75*layers_;   // the 640x360 scene region of every eye
        std::fprintf(stderr,"[frag-dump] packet %zu draws, %.2f M fragments (%.1f scene screens)%s%s\n",packet.draws.size(),total/1e6,total/screen,
                     packet.texture_copy ? " copy" : "",packet.present ? " present" : "");
        static const bool cost=std::getenv("UC_FRAG_COST")!=nullptr;   // the cost model: every counted draw with its folded module's name
        if (cost) for (const auto& [n,i]:per) {
            const size_t d=frag_queries_[i]; const auto& draw=packet.draws[d]; const auto& key=gpu_draws[d].key;
            uint64_t hash=0xCBF29CE484222325ull;   // tools/fold_tev_shaders.py name: key words 1..67, the alpha-test bit, the module
            const auto mix=[&](uint8_t byte) { hash^=byte; hash*=0x100000001B3ull; };
            for (size_t w=1;w<68;++w) for (int shift=0;shift<32;shift+=8) mix(uint8_t(key[w]>>shift));
            for (int shift=0;shift<32;shift+=8) mix(uint8_t(((key[0]>>16)&1)>>shift));
            mix(((key[0]>>16)&1) ? 0 : 1);
            int textures=0; for (size_t map=0;map<8;++map) if (draw_texture(draw,map)) ++textures;
            std::fprintf(stderr,"[frag-cost] packet %zu%s%s fragments %llu module %016llx stages %u textures %d indices %u callsite %08X persp %d blend %u\n",packet.draws.size(),
                         packet.texture_copy ? " copy" : "",packet.present ? " present" : "",(unsigned long long)n,(unsigned long long)hash,key[1],textures,gpu_draws[d].index_count,draw.callsite,
                         int(draw.transforms->projection[3][3]==0),unsigned(draw.blend.mode));
        }
        for (size_t k=0;k<per.size() && k<25;++k) {
            const size_t d=frag_queries_[per[k].second]; const auto& draw=packet.draws[d]; const auto& key=gpu_draws[d].key;
            const uint32_t alpha=draw.uniforms->pixel[0];
            std::fprintf(stderr,"[frag-dump] %6.2f M %5.2f scr  draw %4zu callsite %08X blend %u  alpha-test %d  z test %u write %u cmp %u  tev %u  idx %u  reticle %d  bank %d\n",
                         per[k].first/1e6,per[k].first/screen,d,draw.callsite,unsigned(draw.blend.mode),int(!(((alpha>>16)&7)==7 && ((alpha>>19)&7)==7)),
                         key[68]&1,(key[68]>>4)&1,(key[68]>>1)&7,unsigned(draw.uniforms->pixel[3]&15),gpu_draws[d].index_count,int(draw.reticle),int(gpu_draws[d].bank!=kNoBank));
        }
    }
    timings_.submit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_start).count();
    if(copy_target) {
        copy_target->initialized=true;
        if(detached_copies_ && packet.texture_copy->target->guest_address)latest_copies_[packet.texture_copy->target->guest_address]=packet.texture_copy->target;
    }
    for (auto& texture : slot.textures) texture->initialized=true;
    if (packet.present && display) keep_frame_=false;
    if (packet.present && !overlay_frame) { frame_start_=true; ++frame_index_; scene_restore_copy_.reset(); dlss5_scene_.ready=false; frame_reticle_.clear(); }
    slot.serial = serial; slot.submitted = true; slot.copied=packet.present && display && (!xr_direct || capture_xr); slot.initialized |= slot.copied; slot.reported = false; last_serial_ = serial; next_slot_ = (next_slot_ + 1) % slots_.size();   // efb_initialized_/scene_initialized_ are set by the passes that wrote them
#ifdef UC_OPENXR
    if (xr_ && packet.present && display && xr_->frame_open()) { xr_->end_frame(xr_frame_); if (xr_frame_) ++presentations_; xr_frame_ = false; }
#endif
    if (preview_ && packet.present && display && preview_frame_open_) {   // as the headset's frame ends: the frame's most-used lens sizes the screen
        if (preview_lens_seen_) preview_->finish_frame();
        if (world_drawn_) ++preview_world_frames_;
        preview_lens_seen_ = false; preview_frame_open_ = false;
    }
    if (!present_image) return true;
    VkSwapchainPresentFenceInfoKHR present_fence{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_KHR}; present_fence.swapchainCount = 1; present_fence.pFences = &present_image->fence;
    // With DLSS-G Streamline presents (and paces) the frames itself and never signals a present fence: its swapchain images are not tracked here.
    const bool tracked = !streamline_swapchain() && !fsr_presentation_;
    const bool fenced = present_fences_ && tracked;
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR}; present.pNext = fenced ? &present_fence : nullptr; present.waitSemaphoreCount = 1; present.pWaitSemaphores = &present_image->ready;
    present.swapchainCount = 1; present.pSwapchains = &swapchain_; present.pImageIndices = &image_index;
    const auto present_start = std::chrono::steady_clock::now();
    const bool marked = upscaler_kind_==UpscalerKind::dlss && streamline_ && streamline_->active() && (!overlay_frame || !held_.presented);   // a held frame is marked once, however often it is shown
    if (overlay_frame) held_.presented=true;
    if (marked) streamline_->marker(Streamline::Marker::present_start);
    VkResult result = present_.present(queue_, &present);   // with DLSS-G Streamline presents the generated frames here too
    if (marked) streamline_->marker(Streamline::Marker::present_end);
    if (dlss_generation_enabled()) dlss_g_->after_present();
    if (fsr_presentation_) fsr_g_->after_present();
    timings_.present_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - present_start).count();
    if (result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) { present_image->pending = tracked; ++presentations_; }
    else if (result != VK_ERROR_OUT_OF_DATE_KHR) vk_check(result, "present frame");
    if (suboptimal || result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR) resized_ = true;
    return true;
}
void VulkanRenderer::ensure_held_frame() {
    if (held_.image) return;   // a window resize destroys it (destroy_held_frame) with the other output-sized images
    make_attachment(held_.image,held_.memory,held_.view,extent_);
    make_attachment(held_.depth,held_.depth_memory,held_.depth_view,extent_,true);
    held_.extent=extent_; held_.valid=held_.image_ready=held_.presented=false;
    run_commands([&](VkCommandBuffer command) {   // the scratch depth rests in DEPTH_ATTACHMENT like the EFB's: the reticle pass clears it before every use
        barrier(command,held_.depth,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,VK_PIPELINE_STAGE_2_NONE,0,VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,VK_IMAGE_ASPECT_DEPTH_BIT);
    });
}
void VulkanRenderer::destroy_held_frame() {   // the device is idle or every slot fence was waited for
    for (auto [view,image,memory] : {std::tuple{&held_.view,&held_.image,&held_.memory},std::tuple{&held_.depth_view,&held_.depth,&held_.depth_memory}}) {
        if (*view) vkDestroyImageView(device, *view, nullptr);
        if (*image) vkDestroyImage(device, *image, nullptr);
        if (*memory) vkFreeMemory(device, *memory, nullptr);
        *view = VK_NULL_HANDLE; *image = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE;
    }
    held_.extent={}; held_.valid=held_.image_ready=held_.presented=false; held_.draws.clear(); frame_reticle_.clear();
}
bool VulkanRenderer::render_overlay(uint64_t serial) {
    if (!reticle_layer_ready()) return false;
    // The overlay frames share the slot ring with the game's packets and come far more often: they leave the next two slots to the packets (a slow GPU
    // would otherwise keep every slot busy with reticle frames and the simulation would wait for one).
    for (size_t k=0;k<2 && k<slots_.size();++k)
        if (vkGetFenceStatus(device,slots_[(next_slot_+k)%slots_.size()].fence)==VK_NOT_READY) return false;
    gx::FramePacket packet;
    packet.serial=serial; packet.present=true; packet.clear_after_copy=false; packet.display_source=held_.display_source; packet.overlay_only=true;
    for (const auto& draw:held_.draws) packet.draws.push_back(draw);
    return render(packet,true,serial);
}
void VulkanRenderer::shutdown() {
    if (!instance_ && !device) return;
    owner();
    if (device) {
        retire_images();
        sweep_meshes(true);
        prompt_overlay_.shutdown();
        menu_overlay_.shutdown();
        for(auto& [source,texture]:copy_textures_) destroy_texture(texture);
        copy_textures_.clear();
        copy_aliases_.clear(); latest_copies_.clear();
        for(auto& texture:recycled_copies_) destroy_texture(texture);
        recycled_copies_.clear(); recycled_copy_bytes_=0;
        if(depth_view_) vkDestroyImageView(device,depth_view_,nullptr);
        if(depth_) vkDestroyImage(device,depth_,nullptr);
        if(depth_memory_) vkFreeMemory(device,depth_memory_,nullptr);
        depth_view_=VK_NULL_HANDLE; depth_=VK_NULL_HANDLE; depth_memory_=VK_NULL_HANDLE;
        if (efb_view_) vkDestroyImageView(device,efb_view_,nullptr);
        if (efb_) vkDestroyImage(device,efb_,nullptr);
        if (efb_memory_) vkFreeMemory(device,efb_memory_,nullptr);
        efb_view_=VK_NULL_HANDLE; efb_=VK_NULL_HANDLE; efb_memory_=VK_NULL_HANDLE; efb_initialized_=false; efb_extent_={}; efb_clear_.pending=false;
        destroy_fovea();
        destroy_hud_layer();
        destroy_bloom_proxy();
        destroy_particle_layer();
        dlss_g_.reset();
        fsr_g_.reset();   // its swapchain context was emptied by retire_images; the contexts go before the device
        for (int i=0;i<2;++i) {
            if (hudless_view_[i]) vkDestroyImageView(device,hudless_view_[i],nullptr);
            if (hudless_[i]) vkDestroyImage(device,hudless_[i],nullptr);
            if (hudless_memory_[i]) vkFreeMemory(device,hudless_memory_[i],nullptr);
            hudless_view_[i]=VK_NULL_HANDLE; hudless_[i]=VK_NULL_HANDLE; hudless_memory_[i]=VK_NULL_HANDLE;
        }
        hudless_extent_={};
        upscaler_.reset();
        dlss5_pass_.shutdown(); rcas_.shutdown(); hdr_pass_.shutdown(); bloom_pass_.shutdown(); occlusion_.shutdown(); motion_blur_.shutdown(); rt_shadow_.shutdown(); rt_shadow_denoiser_.shutdown(); rt_reflection_.shutdown(); rt_reflection_denoiser_.shutdown(); rt_lighting_.shutdown(); rt_lighting_denoiser_.shutdown(); rt_path_.shutdown(); rt_path_denoiser_.shutdown(); rr_inputs_.shutdown(); rt_.shutdown();
        destroy_scene_targets();
        for (auto& buffer:rt_records_) destroy_buffer(buffer);
        for (auto& buffer:rt_lights_) destroy_buffer(buffer);
        rt_record_count_=0;
        for(auto& sampler:scene_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
        stop_workers();
        for (auto [key,pipeline]:pipelines_) vkDestroyPipeline(device,pipeline,nullptr);
        for (auto [key,pipeline]:uber_pipelines_) vkDestroyPipeline(device,pipeline,nullptr);
        pipelines_.clear(); uber_pipelines_.clear(); last_pipeline_=VK_NULL_HANDLE; last_table_pipeline_=VK_NULL_HANDLE; last_merge_pipeline_[0]=last_merge_pipeline_[1]=last_merge_pipeline_[2]=VK_NULL_HANDLE;
        if (pipeline_cache_) { save_pipeline_cache(); vkDestroyPipelineCache(device,pipeline_cache_,nullptr); pipeline_cache_=VK_NULL_HANDLE; }
        if (shadow_pipeline_) vkDestroyPipeline(device,shadow_pipeline_,nullptr);
        shadow_pipeline_=VK_NULL_HANDLE; shadow_map_.shutdown();
        for (auto module:{vertex_module_,nobank_vertex_module_,fragment_module_,early_module_,uber_module_,shadow_vertex_module_,xform_vertex_module_,xform_compute_module_}) if (module) vkDestroyShaderModule(device,module,nullptr);
        xform_vertex_module_=xform_compute_module_=VK_NULL_HANDLE;
        for (auto module:{table_vertex_module_,table_fragment_module_,table_early_module_}) if (module) vkDestroyShaderModule(device,module,nullptr);
        table_vertex_module_=table_fragment_module_=table_early_module_=VK_NULL_HANDLE;
        for (auto module:{merge_vertex_module_,merge_fragment_module_,merge_early_module_,few_vertex_module_[0],few_fragment_module_[0],few_early_module_[0],few_vertex_module_[1],few_fragment_module_[1],few_early_module_[1]}) if (module) vkDestroyShaderModule(device,module,nullptr);
        merge_vertex_module_=merge_fragment_module_=merge_early_module_=VK_NULL_HANDLE;
        for (auto* modules:{few_vertex_module_,few_fragment_module_,few_early_module_}) modules[0]=modules[1]=VK_NULL_HANDLE;
        if (merge_layout_) vkDestroyPipelineLayout(device,merge_layout_,nullptr);
        if (merge_set_layout_) vkDestroyDescriptorSetLayout(device,merge_set_layout_,nullptr);
        merge_layout_=VK_NULL_HANDLE; merge_set_layout_=VK_NULL_HANDLE;
        if (table_layout_) vkDestroyPipelineLayout(device,table_layout_,nullptr);
        if (table_set_layout_) vkDestroyDescriptorSetLayout(device,table_set_layout_,nullptr);
        table_layout_=VK_NULL_HANDLE; table_set_layout_=VK_NULL_HANDLE;
        if (xform_pipeline_) vkDestroyPipeline(device,xform_pipeline_,nullptr);
        if (xform_layout_) vkDestroyPipelineLayout(device,xform_layout_,nullptr);
        if (xform_set_layout_) vkDestroyDescriptorSetLayout(device,xform_set_layout_,nullptr);
        xform_pipeline_=VK_NULL_HANDLE; xform_layout_=VK_NULL_HANDLE; xform_set_layout_=VK_NULL_HANDLE;
        for (auto& [hash,module]:folded_modules_) if (module) vkDestroyShaderModule(device,module,nullptr);
        folded_modules_.clear();
        shadow_vertex_module_=VK_NULL_HANDLE;
        vertex_module_=nobank_vertex_module_=fragment_module_=early_module_=uber_module_=VK_NULL_HANDLE;
        if (layout_) vkDestroyPipelineLayout(device, layout_, nullptr);
        if (texture_layout_) vkDestroyDescriptorSetLayout(device,texture_layout_,nullptr);
        for(auto& sampler:texture_samplers_) { if(sampler) vkDestroySampler(device,sampler,nullptr); sampler=VK_NULL_HANDLE; }
        texture_layout_=VK_NULL_HANDLE;
        for (auto& slot : slots_) {
            if (slot.fence) vkDestroyFence(device, slot.fence, nullptr);
            if (slot.acquired) vkDestroySemaphore(device, slot.acquired, nullptr);
            slot = {};
        }
        pass_timer_.shutdown();
        if(retirement_acquired_)vkDestroyFence(device,retirement_acquired_,nullptr);
        retirement_acquired_=VK_NULL_HANDLE;
        if (timing_pool_) vkDestroyQueryPool(device, timing_pool_, nullptr);
        if (pool_) vkDestroyCommandPool(device, pool_, nullptr);
        if (streamline_ && streamline_->active()) {   // slShutdown before the device and instance go; the surface it created first
            if (surface_) { streamline_->destroy_surface(instance_, surface_); surface_ = VK_NULL_HANDLE; }
            streamline_->shutdown();
        }
#ifdef UC_OPENXR
        if (xr_) xr_->detach();   // the session and its swapchain views belong to this device
#endif
        vkDestroyDevice(device, nullptr); device = VK_NULL_HANDLE;
        layout_ = VK_NULL_HANDLE; pool_ = VK_NULL_HANDLE;
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    surface_ = VK_NULL_HANDLE;
    if (messenger_) {
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (destroy) destroy(instance_, messenger_, nullptr);
    }
    messenger_ = VK_NULL_HANDLE;
    vkDestroyInstance(instance_, nullptr); instance_ = VK_NULL_HANDLE;
}
} // namespace uc
