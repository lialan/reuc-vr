#pragma once
// HOTD4 VR's overlay (lindberghrecomp renderer/vulkan/overlay.cpp) for this renderer: the gun models, laser beams, dots, muzzle flashes,
// optical sights and the settings / stats panels, drawn into both eyes after the game with multiview. Same meshes (xr_overlay_geometry.c),
// shaders (shaders/xr_overlay_*), raster states and draw order as HOTD4; plain Vulkan instead of lindbergh's Renderer/Target classes.
#include "../xr/settings_panel.hpp"
#include "../xr/xr_overlay.h"
#include <vulkan/vulkan.h>
#include <array>
#include <filesystem>
#include <vector>

namespace uc {
class XrOverlay {
public:
    static constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT;
    void initialize(VkPhysicalDevice physical, VkDevice device, VkFormat color_format, uint32_t width, uint32_t height, const std::filesystem::path& shaders);
    void shutdown();
    void set_fovea_support(bool on) { fovea_support_ = on; }   // before initialize(): the eye pass may bind a foveation map (the overlay ignores it)
    VkImageView depth_view() const { return depth_view_; }
    VkImage depth_image() const { return depth_; }
    // Before the eye pass, outside any rendering: the panel pictures that changed (null: unchanged).
    void upload(VkCommandBuffer command, const unsigned char* panel, const unsigned char* label);
    // Inside the eye pass (multiview, depth attached): what HOTD4's Overlay::draw draws.
    void draw(VkCommandBuffer command, const xr::Matrices& views, const std::array<std::array<float,3>,2>& eyes, const std::array<XoFrame,2>& frames,
              const std::array<xr::PanelDraw,2>& panels);
private:
    struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0; };
    enum State { kGun, kBeam, kGlass, kReticle, kPanel, kStates };
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage);
    void destroy(Buffer&);
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags flags, bool required = true) const;
    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory_{};
    uint32_t width_ = 0, height_ = 0;
    bool fovea_support_ = false;
    VkImage depth_ = VK_NULL_HANDLE; VkDeviceMemory depth_memory_ = VK_NULL_HANDLE; VkImageView depth_view_ = VK_NULL_HANDLE;
    struct Mesh { VkDeviceSize offset = 0; uint32_t vertices = 0; float rail = 0; };
    std::array<Mesh, 9> meshes_{};
    Buffer meshes_buffer_, quad_, stream_, uniforms_;
    VkDeviceSize stream_at_ = 0, uniforms_at_ = 0, uniform_align_ = 256;
    std::array<VkImage, 2> panel_images_{}; std::array<VkDeviceMemory, 2> panel_memory_{}; std::array<VkImageView, 2> panel_views_{};
    std::array<Buffer, 2> panel_staging_{}; std::array<bool, 2> panel_ready_{}, panel_initialized_{};
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout uniform_layout_ = VK_NULL_HANDLE, panel_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout uniform_pipeline_layout_ = VK_NULL_HANDLE, panel_pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool pool_ = VK_NULL_HANDLE;
    VkDescriptorSet uniform_set_ = VK_NULL_HANDLE; std::array<VkDescriptorSet, 2> panel_sets_{};
    std::array<VkShaderModule, 6> modules_{};
    std::array<VkPipeline, kStates> pipelines_{};
};
}  // namespace uc
