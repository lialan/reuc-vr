// HOTD4 VR's overlay drawing (lindberghrecomp ef5cd20 renderer/vulkan/overlay.cpp) on plain Vulkan: the draw() body below follows
// Overlay::draw call for call (panels, then per gun: model, sight glass and reticle, beam, dot, flash), with its raster states.
#include "xr_overlay.h"
#include "../xr/xr_overlay_geometry.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>

namespace uc {
namespace {
void vk(VkResult result, const char* what) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string("XR overlay: ") + what + " failed: VkResult " + std::to_string(result));
}
struct Parameters { float model[16], color[4], shape[4]; };   // std140 Overlay block of the gun and beam shaders
struct BeamVertex { float left[3], right[3], uv[2]; };
struct Push { float view_projection[2][16]; };
static_assert(sizeof(Parameters) == 96 && sizeof(BeamVertex) == 32 && sizeof(Push) == 128);
constexpr VkDeviceSize kStreamBytes = 1 << 20, kUniformBytes = 256 << 10;   // rings: dozens of frames before a slot is reused
xr::Matrix to_matrix(const float* m) { xr::Matrix r; std::copy(m, m + 16, r.begin()); return r; }
}  // namespace

uint32_t XrOverlay::memory_type(uint32_t mask, VkMemoryPropertyFlags flags, bool required) const {
    for (uint32_t i = 0; i < memory_.memoryTypeCount; ++i)
        if ((mask & (1u << i)) && (memory_.memoryTypes[i].propertyFlags & flags) == flags) return i;
    if (required) throw std::runtime_error("XR overlay: no suitable memory type");
    return UINT32_MAX;
}
XrOverlay::Buffer XrOverlay::make_buffer(VkDeviceSize size, VkBufferUsageFlags usage) {
    Buffer b; b.size = size;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; info.size = size; info.usage = usage;
    vk(vkCreateBuffer(device_, &info, nullptr, &b.buffer), "buffer");
    VkMemoryRequirements requirements; vkGetBufferMemoryRequirements(device_, b.buffer, &requirements);
    VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vk(vkAllocateMemory(device_, &allocate, nullptr, &b.memory), "buffer memory");
    vk(vkBindBufferMemory(device_, b.buffer, b.memory, 0), "bind buffer");
    vk(vkMapMemory(device_, b.memory, 0, VK_WHOLE_SIZE, 0, &b.mapped), "map buffer");
    return b;
}
void XrOverlay::destroy(Buffer& b) {
    if (b.buffer) vkDestroyBuffer(device_, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(device_, b.memory, nullptr);
    b = {};
}

void XrOverlay::initialize(VkPhysicalDevice physical, VkDevice device, VkFormat color_format, uint32_t width, uint32_t height, const std::filesystem::path& shaders) {
    device_ = device; width_ = width; height_ = height;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory_);
    VkPhysicalDeviceProperties properties; vkGetPhysicalDeviceProperties(physical, &properties);
    uniform_align_ = std::max<VkDeviceSize>(properties.limits.minUniformBufferOffsetAlignment, 16);
    // Depth for both eyes, only ever inside the eye pass: transient, lazily allocated where the GPU offers it (tile memory on Quest).
    VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; image.imageType = VK_IMAGE_TYPE_2D; image.format = kDepthFormat;
    image.extent = {width, height, 1}; image.mipLevels = 1; image.arrayLayers = 2; image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL; image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
    vk(vkCreateImage(device_, &image, nullptr, &depth_), "depth image");
    {
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_, depth_, &requirements);
        uint32_t type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, false);
        if (type == UINT32_MAX) type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size; allocate.memoryTypeIndex = type;
        vk(vkAllocateMemory(device_, &allocate, nullptr, &depth_memory_), "depth memory");
        vk(vkBindImageMemory(device_, depth_, depth_memory_, 0), "bind depth");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = depth_; view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = kDepthFormat; view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 2};
        vk(vkCreateImageView(device_, &view, nullptr, &depth_view_), "depth view");
    }
    // The gun meshes: three models x three sights, built once (HOTD4's shared geometry).
    {
        const unsigned capacity = xo_gun_capacity();
        std::vector<float> vertices(size_t(capacity) * 9);
        std::vector<float> all;
        for (int model = 0; model < 3; ++model) for (int sight = 0; sight < 3; ++sight) {
            auto& mesh = meshes_[model * 3 + sight];
            const int count = xo_gun_build(model, sight, vertices.data(), capacity, &mesh.rail);
            if (count <= 0) throw std::runtime_error("XR overlay: cannot build a gun mesh");
            mesh.offset = all.size() * sizeof(float); mesh.vertices = uint32_t(count);
            all.insert(all.end(), vertices.begin(), vertices.begin() + size_t(count) * 9);
        }
        meshes_buffer_ = make_buffer(all.size() * sizeof(float), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        std::memcpy(meshes_buffer_.mapped, all.data(), all.size() * sizeof(float));
        const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
        quad_ = make_buffer(sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        std::memcpy(quad_.mapped, quad, sizeof quad);
        stream_ = make_buffer(kStreamBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        uniforms_ = make_buffer(kUniformBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    }
    // The panels' pictures: the settings panel and the stats label.
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t w = i ? XM_STATS_W : XM_W, h = i ? XM_STATS_H : XM_H;
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; info.imageType = VK_IMAGE_TYPE_2D; info.format = VK_FORMAT_R8G8B8A8_UNORM;
        info.extent = {w, h, 1}; info.mipLevels = info.arrayLayers = 1; info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        vk(vkCreateImage(device_, &info, nullptr, &panel_images_[i]), "panel image");
        VkMemoryRequirements requirements; vkGetImageMemoryRequirements(device_, panel_images_[i], &requirements);
        VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; allocate.allocationSize = requirements.size;
        allocate.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vk(vkAllocateMemory(device_, &allocate, nullptr, &panel_memory_[i]), "panel memory");
        vk(vkBindImageMemory(device_, panel_images_[i], panel_memory_[i], 0), "bind panel");
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; view.image = panel_images_[i]; view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = VK_FORMAT_R8G8B8A8_UNORM; view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk(vkCreateImageView(device_, &view, nullptr, &panel_views_[i]), "panel view");
        panel_staging_[i] = make_buffer(VkDeviceSize(w) * h * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    }
    VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vk(vkCreateSampler(device_, &sampler, nullptr, &sampler_), "sampler");
    // Descriptor sets: the per-draw Overlay block (dynamic offset into the uniform ring) and each panel's picture.
    {
        const VkDescriptorSetLayoutBinding uniform{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; info.bindingCount = 1; info.pBindings = &uniform;
        vk(vkCreateDescriptorSetLayout(device_, &info, nullptr, &uniform_layout_), "uniform layout");
        const VkDescriptorSetLayoutBinding picture{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler_};
        info.pBindings = &picture;
        vk(vkCreateDescriptorSetLayout(device_, &info, nullptr, &panel_layout_), "panel layout");
        const VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; layout.setLayoutCount = 1; layout.pushConstantRangeCount = 1; layout.pPushConstantRanges = &push;
        layout.pSetLayouts = &uniform_layout_; vk(vkCreatePipelineLayout(device_, &layout, nullptr, &uniform_pipeline_layout_), "pipeline layout");
        layout.pSetLayouts = &panel_layout_; vk(vkCreatePipelineLayout(device_, &layout, nullptr, &panel_pipeline_layout_), "panel pipeline layout");
        const VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2}};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; pool.maxSets = 3; pool.poolSizeCount = 2; pool.pPoolSizes = sizes;
        vk(vkCreateDescriptorPool(device_, &pool, nullptr, &pool_), "descriptor pool");
        VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; allocate.descriptorPool = pool_; allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &uniform_layout_; vk(vkAllocateDescriptorSets(device_, &allocate, &uniform_set_), "uniform set");
        const VkDescriptorBufferInfo buffer{uniforms_.buffer, 0, sizeof(Parameters)};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; write.dstSet = uniform_set_; write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC; write.pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
        for (unsigned i = 0; i < 2; ++i) {
            allocate.pSetLayouts = &panel_layout_; vk(vkAllocateDescriptorSets(device_, &allocate, &panel_sets_[i]), "panel set");
            const VkDescriptorImageInfo picture_info{sampler_, panel_views_[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet panel_write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; panel_write.dstSet = panel_sets_[i]; panel_write.descriptorCount = 1;
            panel_write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; panel_write.pImageInfo = &picture_info;
            vkUpdateDescriptorSets(device_, 1, &panel_write, 0, nullptr);
        }
    }
    // The pipelines: HOTD4's raster states (overlay.cpp gun_state, beam_state, glass_state, reticle_state; the panel's beam_state strip).
    const char* names[6] = {"xr_overlay_gun.vert.spv", "xr_overlay_gun.frag.spv", "xr_overlay_beam.vert.spv", "xr_overlay_beam.frag.spv",
                            "xr_overlay_panel.vert.spv", "xr_overlay_panel.frag.spv"};
    for (unsigned i = 0; i < 6; ++i) {
        std::ifstream file(shaders / names[i], std::ios::binary);
        std::vector<char> code((std::istreambuf_iterator<char>(file)), {});
        if (code.empty() || code.size() % 4) throw std::runtime_error(std::string("XR overlay: shader ") + names[i] + " missing or invalid");
        VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; info.codeSize = code.size(); info.pCode = reinterpret_cast<const uint32_t*>(code.data());
        vk(vkCreateShaderModule(device_, &info, nullptr, &modules_[i]), "shader module");
    }
    for (int state = 0; state < kStates; ++state) {
        const unsigned program = state == kGun ? 0 : state == kPanel ? 4 : 2;
        const VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, modules_[program], "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, modules_[program + 1], "main", nullptr}};
        VkVertexInputBindingDescription binding{0, state == kGun ? 36u : state == kPanel ? 8u : uint32_t(sizeof(BeamVertex)), VK_VERTEX_INPUT_RATE_VERTEX};
        std::vector<VkVertexInputAttributeDescription> attributes;
        if (state == kGun) attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32B32_SFLOAT, 24}};
        else if (state == kPanel) attributes = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}};
        else attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12}, {2, 0, VK_FORMAT_R32G32_SFLOAT, 24}};
        VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        input.vertexBindingDescriptionCount = 1; input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = uint32_t(attributes.size()); input.pVertexAttributeDescriptions = attributes.data();
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = state == kPanel ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; raster.cullMode = VK_CULL_MODE_NONE; raster.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        if (state == kGun) { depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_LESS; }
        if (state == kGlass) { depth.depthTestEnable = depth.depthWriteEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_ALWAYS; }
        if (state == kReticle) { depth.depthTestEnable = VK_TRUE; depth.depthCompareOp = VK_COMPARE_OP_EQUAL; }
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        if (state != kGun) {
            attachment.blendEnable = VK_TRUE;
            attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA; attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA; attachment.colorBlendOp = VK_BLEND_OP_ADD;
            attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE; attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &attachment;
        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dynamic.dynamicStateCount = 2; dynamic.pDynamicStates = dynamic_states;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.viewMask = 0b11;
        rendering.colorAttachmentCount = 1; rendering.pColorAttachmentFormats = &color_format; rendering.depthAttachmentFormat = kDepthFormat;
        VkGraphicsPipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; pipeline.pNext = &rendering;
        pipeline.stageCount = 2; pipeline.pStages = stages; pipeline.pVertexInputState = &input; pipeline.pInputAssemblyState = &assembly;
        pipeline.pViewportState = &viewport; pipeline.pRasterizationState = &raster; pipeline.pMultisampleState = &multisample; pipeline.pDepthStencilState = &depth;
        pipeline.pColorBlendState = &blend; pipeline.pDynamicState = &dynamic; pipeline.layout = state == kPanel ? panel_pipeline_layout_ : uniform_pipeline_layout_;
        vk(vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pipeline, nullptr, &pipelines_[state]), "overlay pipeline");
    }
}

void XrOverlay::upload(VkCommandBuffer command, const unsigned char* panel, const unsigned char* label) {
    const unsigned char* pictures[2] = {panel, label};
    for (unsigned i = 0; i < 2; ++i) {
        if (!pictures[i]) continue;
        const uint32_t w = i ? XM_STATS_W : XM_W, h = i ? XM_STATS_H : XM_H;
        std::memcpy(panel_staging_[i].mapped, pictures[i], size_t(w) * h * 4);
        auto barrier = [&](VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 src, VkAccessFlags2 src_access, VkPipelineStageFlags2 dst, VkAccessFlags2 dst_access) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2}; b.srcStageMask = src; b.srcAccessMask = src_access; b.dstStageMask = dst; b.dstAccessMask = dst_access;
            b.oldLayout = from; b.newLayout = to; b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = panel_images_[i]; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dependency.imageMemoryBarrierCount = 1; dependency.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(command, &dependency);
        };
        barrier(panel_initialized_[i] ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(command, panel_staging_[i].buffer, panel_images_[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        panel_initialized_[i] = panel_ready_[i] = true;
    }
}

void XrOverlay::draw(VkCommandBuffer command, const xr::Matrices& views, const std::array<std::array<float,3>,2>& eyes, const std::array<XoFrame,2>& frames,
                     const std::array<xr::PanelDraw,2>& panels) {
    bool any = false;
    for (const auto& f : frames) any |= f.gun_valid && (f.show_gun || f.beam || f.dot || f.flash);
    for (unsigned i = 0; i < 2; ++i) any |= panels[i].visible && panel_ready_[i];
    if (!any) return;
    const VkRect2D rect{{0, 0}, {width_, height_}};
    int bound = -1; bool forced_depth = false;
    auto viewport = [&](bool force) {   // HOTD4's forced depth: the whole draw at depth 1
        const VkViewport v{0, 0, float(width_), float(height_), force ? 1.f : 0.f, 1};
        vkCmdSetViewport(command, 0, 1, &v);
    };
    vkCmdSetScissor(command, 0, 1, &rect); viewport(false);
    auto select = [&](int state, bool force) {
        if (bound != state) { vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelines_[state]); bound = state; }
        if (force != forced_depth) { viewport(force); forced_depth = force; }
    };
    auto clear_depth = [&](float value) {
        VkClearAttachment clear{}; clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT; clear.clearValue.depthStencil = {value, 0};
        const VkClearRect area{rect, 0, 1};
        vkCmdClearAttachments(command, 1, &clear, 1, &area);
    };
    auto push = [&](const xr::Matrices& m, VkPipelineLayout layout) { vkCmdPushConstants(command, layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Push), m.view_projection); };
    auto bind = [&](const Parameters& p) {
        if (uniforms_at_ + uniform_align_ > uniforms_.size) uniforms_at_ = 0;
        std::memcpy(static_cast<char*>(uniforms_.mapped) + uniforms_at_, &p, sizeof p);
        const uint32_t offset = uint32_t(uniforms_at_);
        uniforms_at_ += (sizeof p + uniform_align_ - 1) / uniform_align_ * uniform_align_;
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, uniform_pipeline_layout_, 0, 1, &uniform_set_, 1, &offset);
    };
    auto shape = [&](const float q[2][6][5], const std::array<float,4>& color, float ring, float thick, int state, bool force) {
        select(state, force);
        Parameters p{}; std::copy(color.begin(), color.end(), p.color); p.shape[0] = ring; p.shape[1] = thick; bind(p);
        const int uv_eye = q[0][0][3] != 0 ? 0 : 1;
        BeamVertex vertices[6];
        for (int i = 0; i < 6; ++i) { std::memcpy(vertices[i].left, q[0][i], 12); std::memcpy(vertices[i].right, q[1][i], 12); std::memcpy(vertices[i].uv, q[uv_eye][i] + 3, 8); }
        if (stream_at_ + sizeof vertices > stream_.size) stream_at_ = 0;
        std::memcpy(static_cast<char*>(stream_.mapped) + stream_at_, vertices, sizeof vertices);
        const VkDeviceSize offset = stream_at_; stream_at_ += sizeof vertices;
        vkCmdBindVertexBuffers(command, 0, 1, &stream_.buffer, &offset);
        push(views, uniform_pipeline_layout_);
        vkCmdDraw(command, 6, 1, 0, 0);
    };
    auto spot = [&](const float* position, const std::array<float,2>& radii, const std::array<float,4>& color, float ring, float thick, bool reticle) {
        float q[2][6][5]{}; bool visible = false;
        for (int eye = 0; eye < 2; ++eye) visible |= xo_spot_vertices(q[eye], eyes[eye].data(), position, radii[eye]) != 0;
        if (visible) shape(q, color, ring, thick, reticle ? kReticle : kBeam, reticle);
    };
    auto draw_panel = [&](unsigned slot) {
        if (!panels[slot].visible || !panel_ready_[slot]) return;
        select(kPanel, false);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, panel_pipeline_layout_, 0, 1, &panel_sets_[slot], 0, nullptr);
        xr::Matrices mvp;
        for (int eye = 0; eye < 2; ++eye) {
            const auto m = xr::multiply(to_matrix(views.view_projection[eye]), panels[slot].model);
            std::copy(m.begin(), m.end(), mvp.view_projection[eye]);
        }
        const VkDeviceSize offset = 0; vkCmdBindVertexBuffers(command, 0, 1, &quad_.buffer, &offset);
        push(mvp, panel_pipeline_layout_);
        vkCmdDraw(command, 4, 1, 0, 0);
    };
    if (panels[0].visible) { draw_panel(1); draw_panel(0); }   // guns stay over an open menu, with one draw each
    for (const auto& f : frames) {
        if (!f.gun_valid) continue;
        if (f.gun_model < 0 || f.gun_model > 2 || f.sight < 0 || f.sight > 2) throw std::invalid_argument("Invalid overlay model/sight");
        const std::array<float,4> color = f.p2 ? std::array<float,4>{.15f, .45f, 1, 1} : std::array<float,4>{1, .12f, .08f, 1};
        const auto& mesh = meshes_[f.gun_model * 3 + f.sight];
        xr::Matrix model; xo_pose_matrix(f.gun_q, f.gun_p, model.data());
        auto kick = xr::identity(); kick[14] = f.recoil; model = xr::multiply(model, kick);
        if (f.show_gun) {
            select(kGun, false); clear_depth(1);
            Parameters p{}; std::copy(model.begin(), model.end(), p.model); bind(p);
            xr::Matrices mvp;
            for (int eye = 0; eye < 2; ++eye) { const auto m = xr::multiply(to_matrix(views.view_projection[eye]), model); std::copy(m.begin(), m.end(), mvp.view_projection[eye]); }
            vkCmdBindVertexBuffers(command, 0, 1, &meshes_buffer_.buffer, &mesh.offset);
            push(mvp, uniform_pipeline_layout_);
            vkCmdDraw(command, mesh.vertices, 1, 0, 0);
        }
        std::array<float,2> distances;
        for (int eye = 0; eye < 2; ++eye) { float d = 0; for (int i = 0; i < 3; ++i) d += (f.end[i] - eyes[eye][i]) * (f.end[i] - eyes[eye][i]); distances[eye] = std::sqrt(d); }
        auto radii = [&](float scale) { return std::array<float,2>{scale * distances[0], scale * distances[1]}; };
        if (f.show_gun && f.sight != XO_SIGHT_NONE) {
            select(kBeam, true); clear_depth(0);
            float q[2][6][5]; xo_glass_vertices(q[0], model.data(), f.sight, mesh.rail); std::memcpy(q[1], q[0], sizeof q[0]);
            shape(q, {.55f, .75f, .85f, .1f}, 0, 0, kGlass, true);
            if (f.sight == XO_SIGHT_RED_DOT) spot(f.end, radii(.0012f), color, 0, 0, true);
            else { auto ring = color; ring[3] = .95f; spot(f.end, radii(.0105f), ring, .9f, .08f, true); spot(f.end, radii(.0009f), color, 0, 0, true); }
            clear_depth(1);
        }
        if (f.beam) {
            float q[2][6][5]{}; bool visible = false;
            for (int eye = 0; eye < 2; ++eye) visible |= xo_beam_vertices(q[eye], eyes[eye].data(), f.muzzle, f.end, .0015f) != 0;
            if (visible) { auto beam = color; beam[3] = .85f; shape(q, beam, 0, 0, kBeam, false); }
        }
        if (f.dot) spot(f.end, radii(.006f), color, 0, 0, false);
        if (f.flash) spot(f.muzzle, {.045f, .045f}, {1, .75f, .3f, .9f}, 0, 0, false);
    }
    if (!panels[0].visible) draw_panel(1);
    if (forced_depth) viewport(false);
}

void XrOverlay::shutdown() {
    if (!device_) return;
    for (auto p : pipelines_) if (p) vkDestroyPipeline(device_, p, nullptr);
    for (auto m : modules_) if (m) vkDestroyShaderModule(device_, m, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    for (auto l : {uniform_pipeline_layout_, panel_pipeline_layout_}) if (l) vkDestroyPipelineLayout(device_, l, nullptr);
    for (auto l : {uniform_layout_, panel_layout_}) if (l) vkDestroyDescriptorSetLayout(device_, l, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    for (unsigned i = 0; i < 2; ++i) {
        if (panel_views_[i]) vkDestroyImageView(device_, panel_views_[i], nullptr);
        if (panel_images_[i]) vkDestroyImage(device_, panel_images_[i], nullptr);
        if (panel_memory_[i]) vkFreeMemory(device_, panel_memory_[i], nullptr);
        destroy(panel_staging_[i]);
    }
    destroy(meshes_buffer_); destroy(quad_); destroy(stream_); destroy(uniforms_);
    if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
    if (depth_) vkDestroyImage(device_, depth_, nullptr);
    if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
    *this = XrOverlay{};
}
}  // namespace uc
