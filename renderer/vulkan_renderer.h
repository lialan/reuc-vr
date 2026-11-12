#pragma once
#include "dlss5_pass.h"
#include "graphics_effects.h"
#include "gx_packets.h"
#include "render_pixels.h"
#include "hdr_pass.h"
#include "hud_layer.h"
#include "pipeline_keys.h"
#include "uber_merge.h"
#include "prompt_overlay.h"
#include "menu_overlay.h"
#include "motion_tracker.h"
#include "bloom_pass.h"
#include "motion_blur_pass.h"
#include "occlusion_pass.h"
#include "pass_timer.h"
#include "rcas_pass.h"
#include "rt_denoiser.h"
#include "rt_material.h"
#include "rt_scene.h"
#include "rr_inputs_pass.h"
#include "rr_math.h"
#include "rt_reflection_pass.h"
#include "rt_path_pass.h"
#include "rt_shadow_pass.h"
#include "streamline.h"
#include "dlss_frame_generation.h"
#include "fsr_frame_generation.h"
#include "scene_boundary.h"
#include "scene_lights.h"
#include "shadow_map.h"
#include "shadow_math.h"
#include "../xr/stereo_scene.hpp"
#include "upscale_mode.h"
#include "upscaler.h"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <intrin.h> // Load Clang's intrinsic guards before SDL 3.2.0's compatibility definitions.
#endif
#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace uc {
class VulkanRenderer {
public:
    VulkanRenderer() = default;
    ~VulkanRenderer();
    VulkanRenderer(const VulkanRenderer&) = delete;
    VulkanRenderer& operator=(const VulkanRenderer&) = delete;
    // The headset output (xr_compositor.h): before initialize(); the instance and device then come from the OpenXR runtime and every
    // presented frame goes to the headset instead of the window's swapchain.
    void use_xr(class XrCompositor* xr) { xr_ = xr; }
    // The game's input context each loop (any thread): in a menu the headset's HUD stays on HOTD4's screen, which the gun aims at; in gameplay it
    // follows the head at the display's edges (StereoScene hud_fit).
    void set_menu_context(bool menu) { menu_context_.store(menu, std::memory_order_relaxed); }
    bool xr_active() const { return xr_ != nullptr; }
    void initialize(SDL_Window* window, bool validation, const std::filesystem::path& shaders, double render_scale = 0.5, double render_aspect = 16.0/9, bool exclusive_fullscreen = false);
    // Suppressing display preserves guest-frame rendering, copies, clears and completion.
    bool render(const gx::FramePacket& packet, bool display = true,uint64_t submission_serial=0);
    bool copy_ready(const std::shared_ptr<const gx::TexturePixels>& texture,bool previous_frame=false);
    // Freshest pointer in aim units: reticle draws are re-aimed to it when their commands are recorded (late_aim.h).
    void set_aim_source(std::function<std::array<float,2>()> source) { aim_source_ = std::move(source); }
    // The pointer `seconds` ago, and how many seconds the local wake strip spans: the wake is laid along the mouse path (late_aim.h) while both are set.
    void set_aim_history(std::function<std::array<float,2>(double)> history) { aim_history_ = std::move(history); }
    void set_wake_seconds(double seconds) { wake_seconds_ = seconds; }
    // The headset's eyes read the presented EFB region in place (no copy into the slot image) unless the frame may be captured.
    void keep_frame(bool keep) { keep_frame_ = keep; }
    uint64_t late_aim_draws() const { return late_aim_draws_.load(); }
    struct FrameTimings { double acquire_ms = 0, submit_ms = 0, present_ms = 0; };   // inside the last render(): the calls a layer or the driver can stall
    FrameTimings last_timings() const { return timings_; }
    void resize(uint32_t width, uint32_t height);
    // Compiles every known pipeline (seed and recorded keys) before the game starts; progress(done,total) is called on this thread.
    void warm_pipelines(const std::function<void(size_t,size_t)>& progress);
    void set_uber_only(bool enabled) { owner(); uber_only_=enabled; }   // tests: draw everything with the ubershader
    void set_render_scale(double scale); // Multiplier of720p; output resize preserves it.
    void set_native_eye_scale(bool on) { native_eye_scale_=on; }   // the headset: the scale making the scene exactly the eye image (set before initialize)
    void set_prompt_style(PromptStyle);
    // The renderer's own bloom (bloom_pass.h) replaces the game's, whose composite draw is always skipped. Off or strength 0: no bloom at all. Live; off in a fresh renderer.
    void set_bloom(const BloomSettings&);
    void set_crosshair_scale(uint32_t percent);   // aim reticle size 0..100 % of the game's (late_aim.h); 0 hides it
    // HDR10 output (hdr_math.h): callable before or after initialize(). Active only while Windows reports HDR for the window's display and the
    // surface offers HDR10; otherwise the SDR swapchain stays. hdr_state_changed(): SDL_EVENT_WINDOW_HDR_STATE_CHANGED, re-picks the mode.
    void set_hdr(const HdrSettings& settings);
    void hdr_state_changed() { owner(); resized_=true; }
    bool hdr_active() const { return hdr_active_; }
    // Graphics effects (graphics_effects.h): callable before or after initialize(); a change of the anisotropy level swaps the samplers at once.
    void set_effects(const GraphicsEffects& effects);
    uint32_t effective_anisotropy() const;   // the level the device runs (requested, clamped to its limit)
    // DLSS 5 Neural Rendering (dlss5_settings.h): callable before or after initialize(). The pass needs the device created with the NGX extensions (dlss5_device())
    // and the nvngx_dlssnr.dll NGX accepts; when either fails it stays off and dlss5_status() says why.
    void allow_dlss5(bool allowed) { dlss5_allowed_=allowed; }   // before initialize: false keeps nvngx_dlssnr.dll unloaded (the launcher's DLSS 5 checkbox)
    void set_dlss5(const Dlss5Settings& settings);
    bool dlss5_device() const { return dlss5_device_; }
    void use_dlss5_backend(std::function<std::unique_ptr<NgxBackend>()> backend, const std::filesystem::path& directory);   // tests: run the pass against a stand-in for NGX (a directory holding a nvngx_dlssnr.dll placeholder), whatever this GPU can do
    bool dlss5_active() const { return dlss5_pass_.ran(); }   // the latest frame went through the network
    std::string dlss5_status() const { return !dlss5_gap_.empty() ? dlss5_gap_ : dlss5_pass_.status(); }   // empty while it works or has not been tried
    // In-game menu: build() lays out the ImGui widgets of each presented frame (empty = closed, no ImGui work at all); setup() loads fonts and
    // style once into the menu's ImGui context.
    void set_menu(std::function<void()> build, std::function<void()> setup = {});
    bool menu_event(const SDL_Event& event) { owner(); return menu_overlay_.event(event); }   // true when the menu wants the event
    // Initial device capabilities (Streamline, motion outputs and frame generation).
    void configure_upscaling(UpscalerKind kind, UpscaleMode mode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier);
    // At a guest frame boundary. False requires a new device; sharpening and off/on of the initialized kind preserve it.
    bool set_upscaling(UpscalerKind kind, UpscaleMode mode, bool sharpening, double sharpness, bool frame_generation, uint32_t multiplier);
    // SMAA works on the finished frame at the output size, whatever the quality setting says.
    UpscaleMode effective_upscale_mode() const { return upscaler_kind_==UpscalerKind::smaa || upscaler_kind_==UpscalerKind::off ? UpscaleMode::native : upscale_mode_; }
    bool bloom_active() const { return bloom_.enabled && bloom_.strength>0; }
    bool scene_path_wanted() const { return motion_outputs_ && (force_scene_targets_ || needs_scene_targets(effects_) || dlss5_requested() || bloom_active()); }   // a pass-through upscaler keeps the scene targets without DLSS/FSR/SMAA
    // The scene path of the bloom alone (pass-through, no effect, no DLSS 5) has no reader of the motion vectors: its draws are not tracked and the motion target stays unbound.
    bool scene_vectors_wanted() const { return upscaler_kind_!=UpscalerKind::off || force_scene_targets_ || needs_scene_targets(effects_) || dlss5_requested(); }
    UpscalerKind active_upscaler() const { return motion_outputs_ && upscaler_ ? upscaler_kind_ : UpscalerKind::off; }   // after fallbacks
    bool frame_generation_active() const { return dlss_generation_enabled() || fsr_generation_enabled(); }
    // Renderer replacement: the EFB copies the old renderer held (copy_keys) are drawn as a blank texture by the new one until the game copies them again.
    std::vector<const gx::TexturePixels*> copy_keys() const;
    void lost_copies(std::vector<const gx::TexturePixels*> keys);
    RenderExtent upscale_internal_extent() const { return {scene_extent_.width, scene_extent_.height}; }   // {0,0} without an upscaler
    uint64_t whole_frame_upscales() const { return whole_frame_upscales_; }   // frames without the game's scene copy: upscaled whole at present
    using MotionPixels=uc::MotionPixels;
    MotionPixels capture_motion();   // tests/tools: the motion target of the last scene (waits for the GPU)
    struct GBufferPixels { uint32_t width=0, height=0; std::vector<uint16_t> texels; };   // rgba per texel, as gbuffer.h encodes
    GBufferPixels capture_gbuffer();   // tests/tools: the G-buffer of the last scene (waits for the GPU); throws when no scene wrote one
    void force_gbuffer(bool on) { owner(); force_gbuffer_=on; }   // tests: write the G-buffer although no effect reads it yet
    void force_lights(bool on) { owner(); force_lights_=on; }   // tests: collect the scene's lights although no effect reads them yet
    bool effects_need_lights() const { return motion_outputs_ && (force_lights_ || effects_.contact_shadows || effects_.shadows!=EffectLevel::off || any_rt_active(effects_,true)); }
    const std::optional<gx::CollectedLight>& dominant_light() const { return dominant_light_; }   // the directional or spot light of the last scene, if any
    void force_scene_targets(bool on);   // tests: keep the scene-target path (blit pass-through without an upscaler) although no effect needs it
    bool effects_need_gbuffer() const { return motion_outputs_ && (force_gbuffer_ || effects_.ambient_occlusion!=EffectLevel::off || effects_.contact_shadows || effects_.shadows!=EffectLevel::off || any_rt_active(effects_,true)); }
    // RT effects' albedo and material-record targets (rt_material.h): written by the scene pass while any RT effect is active, absent otherwise.
    bool rt_material_active() const { return motion_outputs_ && any_rt_active(effects_,true); }   // effects_ has the RT levels masked by device support
    bool rt_material_targets() const { return scene_material_!=VK_NULL_HANDLE; }
    struct RtMaterialPixels { uint32_t width=0, height=0; std::vector<uint32_t> records; std::vector<uint8_t> albedo; };   // albedo: rgba8 per texel
    RtMaterialPixels capture_rt_material();   // tests/tools: both targets of the last scene (waits for the GPU); throws when no scene wrote them
    std::vector<RtMaterialRecord> rt_material_records() const;   // tests/tools: the material records of the latest frame with a scene
    // Ray-traced shadows (docs/superpowers/plans/2026-10-19-ray-tracing-p2.md): the sun and the local lights trace rays through the frame's TLAS. effects_ has the RT
    // levels masked by device support. rt_scene_active: the acceleration structures are built this frame; off, nothing ray tracing runs or allocates.
    bool rt_trace_active() const { return motion_outputs_ && ray_tracing_ && (effects_.rt_shadows!=EffectLevel::off || effects_.rt_local_shadows!=EffectLevel::off || effects_.rt_ambient_occlusion!=EffectLevel::off); }   // the visibility pass (shadows and ambient occlusion)
    bool rt_reflect_active() const { return motion_outputs_ && ray_tracing_ && effects_.rt_reflections!=EffectLevel::off; }
    bool rt_light_active() const { return motion_outputs_ && ray_tracing_ && effects_.rt_lighting!=EffectLevel::off; }
    void set_rt_path_settings(std::optional<RtPathSettings> settings) { rt_path_override_=settings; }   // tests: fixed settings instead of the quality preset (nullopt: the preset)
    bool rt_path_active() const { return motion_outputs_ && ray_tracing_ && effects_.path_tracing; }   // path tracing replaces the RT shadows, AO and lighting (effective_effects)
    bool rt_scene_active() const { return rt_trace_active() || rt_reflect_active() || rt_light_active() || rt_path_active(); }
    struct RtShadowPixels { uint32_t width=0, height=0; std::vector<float> factor; };   // one visibility factor per signal pixel
    void set_rt_trace_lamps(bool on) { rt_trace_lamps_=on; }   // tests: trace the game's point lights instead of the synthetic key light (the lights are camera-relative in the game, so off in play)
    RtShadowPixels capture_rt_shadow_filtered();   // tests/tools: the denoiser's output for the same frame (what the composite multiplies by)
    RtShadowPixels capture_rt_shadow_signal();   // tests/tools: the noisy signal of the latest traced frame (waits for the GPU); throws when none was traced
    uint32_t rt_shadow_signal_images() const { return rt_shadow_.signal_images_created(); }
    RtScene::Stats rt_stats() const { return rt_.stats(); }   // the acceleration structures of the latest frame
    RtScene::CacheStats rt_cache_stats() const { return rt_.cache_stats(); }   // the cached BLASes of static meshes
    uint64_t rt_buffer_growths() const { return rt_.buffer_growths(); }
    double take_pass_total_ms(GpuPass pass) { return pass_timer_.take_total_ms(pass); }   // summed GPU time of a pass over the submissions retired since the last call (frame log)
    RenderPixels capture_scene();    // tests/tools: the internal-resolution scene color of the last scene (waits for the GPU)
    std::vector<uint64_t> poll_completions();
    bool wait_for_pending(uint64_t timeout_ns); // True when an unfinished batch was waited on, including timeout.
    RenderPixels capture(uint64_t serial, bool efb = false);
    void shutdown();
    uint32_t validation_errors() const { return errors_.load(); }
    uint64_t presentation_count() const { return presentations_.load(); }
    // Reticle layer (network play): the reticle quads are left out of the game's frame and drawn over a held copy of the finished picture, which is shown
    // again between ticks with the reticle re-aimed to the mouse, so the reticle runs at the display's rate. Off with HDR output, frame generation and
    // Streamline's swapchain (those present through paths that keep their own frame state).
    void set_reticle_layer(bool on) { reticle_layer_ = on; }
    bool reticle_layer_active() const { return reticle_layer_ && !hdr_active_ && !streamline_swapchain() && !fsr_presentation_ && !frame_generation_active(); }
    bool reticle_layer_ready() const { return reticle_layer_active() && held_.valid && held_.extent.width==extent_.width && held_.extent.height==extent_.height; }
    bool held_presented() const { return held_.presented; }   // the held frame has been shown (render_overlay)
    bool held_has_reticle() const { return !held_.draws.empty(); }
    bool render_overlay(uint64_t serial);   // presents the held frame with the reticle re-aimed; false: nothing shown (no held frame, or the slot is busy)
    double take_gpu_window_ms() { return gpu_window_max_ns_.exchange(0) / 1e6; }   // longest single GPU submission retired since the last call (frame log)
    uint64_t pass_runs(GpuPass pass) const { return pass_timer_.runs(pass); }   // how often an effect pass was recorded
    double pass_ms(GpuPass pass) const { return pass_timer_.last_ms(pass); }   // latest GPU time of one effect pass (0 until it ran)
    uint64_t gpu_busy_ns() const { return gpu_busy_ns_.load(); }   // summed GPU execution time of retired frame submissions
    uint64_t texture_creations() const { owner(); return texture_creations_; }
    uint64_t descriptor_pool_creations() const { owner(); return descriptor_pool_creations_; }
    bool ray_reconstruction_supported() const { return rr_available_; }   // the Ray Reconstruction plugin loaded and the GPU supports it
    bool ray_reconstruction_active() const { return rr_frame_; }          // the latest frame went through Ray Reconstruction (tests, diagnostics)
    bool ray_tracing_supported() const { return ray_tracing_; }   // the device was created with ray query, acceleration structures and descriptor indexing, and the effects path
    const VkPhysicalDeviceMemoryProperties& memory_properties() const { return memory_; }
    void run_commands(const std::function<void(VkCommandBuffer)>& record);   // tests/tools: records a one-time command buffer on the render queue, submits it and waits for the GPU
    VkDevice device = VK_NULL_HANDLE;
private:
    FrameTimings timings_;
    std::function<std::array<float,2>()> aim_source_;
    std::function<std::array<float,2>(double)> aim_history_;
    std::atomic<double> wake_seconds_{0};
    std::atomic<uint64_t> late_aim_draws_{0};
    uint64_t merged_draws_=0, issued_draws_=0, set_binds_=0, pipeline_binds_=0, alpha_passes_=0, grouped_changes_=0, opaque_draws_=0;   // the pre-pass's draw merging (UC_GPU_PACKETS prints them)
    struct Buffer { VkBuffer buffer = VK_NULL_HANDLE; VkDeviceMemory memory = VK_NULL_HANDLE; void* mapped = nullptr; VkDeviceSize size = 0; };
    struct Texture {
        VkImage image=VK_NULL_HANDLE;
        VkDeviceMemory memory=VK_NULL_HANDLE;
        VkImageView view=VK_NULL_HANDLE;
        Buffer upload;
        VkExtent2D extent{};
        uint32_t mip_levels=1;
        bool auto_mips=false;   // the headset: levels 1.. generated on the GPU from a one-level GX texture (sampled with auto_samplers_)
        bool mips_pending=false;   // an EFB copy whose levels 1.. are not made yet: record_copy_mips, when a draw first samples it with a mip filter
        gx::TextureChannels channels=gx::TextureChannels::rgba;
        bool bc7=false;   // a texture replacement: BC7 blocks, not RGBA8
        VkDeviceSize allocation_size=0;
        std::shared_ptr<const gx::TexturePixels> source;
        bool initialized=false;
        const void* uploader=nullptr;   // the slot whose command buffer copies `upload`; it frees the staging buffer once its fence retired
    };
    struct Slot {
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImage ui_image = VK_NULL_HANDLE; VkDeviceMemory ui_memory = VK_NULL_HANDLE; VkImageView ui_view = VK_NULL_HANDLE;   // HDR: the menu and prompts, composited at UI brightness
        Buffer vertices, indices, pixels, uniforms, palettes, banks, motions;   // vertices/indices: this frame's non-mesh draws; motions: MotionBlock per scene draw
        Buffer xform_jobs, xform_chunks, xform_positions, xform_attributes, xform_indices;   // the vertex pre-pass (xform_): its input table, outputs and the rebased indices
        VkDescriptorPool xform_pool = VK_NULL_HANDLE; VkDescriptorSet xform_set = VK_NULL_HANDLE;
        Buffer xform_previous;   // the headset's space warp: last frame's view position per pre-pass position (xr_motion.slang)
        Buffer warp_frame;       // and its eye rows (xr_motion.slang WarpEyes)
        VkDescriptorPool warp_pool = VK_NULL_HANDLE; VkDescriptorSet warp_set = VK_NULL_HANDLE;
        std::vector<std::shared_ptr<const gx::MeshBuffers>> mesh_refs;   // keeps the meshes this submission draws alive until its fence completed
        std::vector<std::shared_ptr<Texture>> textures;   // shared through texture_pool_: one GPU image per texture, not one per slot
        std::vector<std::shared_ptr<const gx::TexturePixels>> copy_sources;
        std::vector<VkDescriptorSet> texture_sets;
        VkDescriptorPool texture_pool=VK_NULL_HANDLE;
        uint32_t descriptor_capacity=0;
        VkDescriptorPool table_pool=VK_NULL_HANDLE; VkDescriptorSet table_set=VK_NULL_HANDLE; uint32_t table_capacity=0;   // the texture table (tex_merge_): set 1, one per packet
        VkDescriptorPool merge_pool=VK_NULL_HANDLE; VkDescriptorSet merge_set=VK_NULL_HANDLE;   // the small-draw ubershader (uber_small_*): set 2, the packet's uniform blocks as an array
        uint64_t serial = 0;
        bool submitted = false, reported = false, initialized = false, copied = false, timed = false;
        std::string timing_kind;   // UC_GPU_PACKETS: what this packet was (copy size, present, draws), for the per-kind GPU time
    };
    // A mesh uploaded once and drawn from every frame; freed once the guest dropped it (owner expired) and no submission still holds it.
    // Meshes live in a few large buffers (one vkAllocateMemory per slab, not per mesh): vertices then indices at the given offsets.
    struct MeshSlab { Buffer buffer; VkDeviceSize used=0; uint32_t live=0; };
    struct GpuMesh { MeshSlab* slab=nullptr; VkDeviceSize vertex_offset=0, index_offset=0; uint32_t index_count=0; uint64_t rows=0; std::weak_ptr<const gx::MeshBuffers> owner; uint64_t id=0; std::array<float,6> box{}; };   // box: object-space bounds (min xyz, max xyz), the headset's culling   // id: never reused; the ray tracing cache keys its BLAS by it
    struct PresentImage {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;   // HDR only: the encode pass renders into it
        VkSemaphore ready = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        bool pending = false;
    };
    void destroy_slot_images(Slot& slot);
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void* user);
    void owner() const;
    uint32_t memory_type(uint32_t mask, VkMemoryPropertyFlags properties) const;
    Buffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool prefer_device_local=false);
    GpuMesh& gpu_mesh(const std::shared_ptr<const gx::MeshBuffers>& source);
    void sweep_meshes(bool all);
    void release_mesh(GpuMesh& mesh);
    void destroy_buffer(Buffer& buffer);
    Texture make_texture(const gx::TexturePixels* pixels, VkExtent2D physical_extent = {});
    Texture make_copy_texture(const std::shared_ptr<const gx::TexturePixels>& source);
    void record_texture_copy(VkCommandBuffer command, VkImage source, VkExtent2D extent, const std::array<uint32_t,4>& region, Texture& target, VkImageLayout before);
    void record_copy_mips(VkCommandBuffer command, Texture& target);   // levels 1.. of an EFB copy from level 0 (SHADER_READ before and after)
    void recycle_copy_texture(Texture& texture);
    void destroy_texture(Texture& texture);
    void retire_textures(Slot& slot);
    void rebuild();
    void retire_images(bool keep_buffers=false);
    void retire_for_resize();
    void reap_retired(bool wait);
    // A pipeline for a draw: the specialized one when compiled, else the ubershader variant of the same fixed state (see queue_pipeline).
    struct PipelineChoice { VkPipeline pipeline=VK_NULL_HANDLE; bool uber=false; };
    PipelineKey pipeline_key(const gx::DrawPacket& draw,bool alpha_pass=false) const;
    bool fused_destination_alpha(const gx::DrawPacket& draw) const;   // the headset: colour and destination alpha in one draw
    void log_depth_stats(const gx::FramePacket& packet,size_t scene_draws,const std::function<uint32_t(size_t)>& index_count);   // UC_DEPTH_STATS (vulkan_depth_efb.cpp)
    // UC_FOLD_CLEAR (depth_efb.h): an EFB copy's clear waits for the next pass on the EFB, which takes it as its load op (CLEAR, when the
    // cleared rectangle holds the pass's render area) or clears it first thing; anything else that reads or writes the EFB before that pass
    // records it as a pass of its own (record_efb_clear), as without the switch.
    struct EfbClear { bool pending=false; VkRect2D rect{}; VkClearColorValue color{}; float depth=1; } efb_clear_;
    void record_efb_clear(VkCommandBuffer command,const char* why);
    uint64_t clear_passes_=0, clears_as_load_=0, clears_in_pass_=0;   // [depth-stats]: clears as passes of their own, as a pass's load op, inside a pass
    // UC_DEPTH_FORMAT (depth_efb.h): the format of the EFB's depth and of every depth target its pipelines draw into (the headset and its preview).
    VkFormat depth_format_=VK_FORMAT_D32_SFLOAT;
    PipelineChoice pipeline_for(const PipelineKey& key);
    VkPipeline make_pipeline(const PipelineKey& key,bool uber);
    VkShaderModule folded_module(const std::array<uint32_t,68>& values,bool early,bool table=false);   // tools/fold_tev_shaders.py's module for these constants, or null
    VkPipeline table_pipeline(const PipelineKey& key);   // the texture-table variant of a draw's pipeline when compiled (else queued, null)
    VkPipeline uber_merge_pipeline(const PipelineKey& key,uint32_t family=0);   // the small-draw ubershader pipeline of the draw's fixed-function state when compiled (else queued, null)
    std::mutex folded_mutex_; std::unordered_map<uint64_t,VkShaderModule> folded_modules_;
    void queue_pipeline(const PipelineKey& key,bool uber);   // pipelines_mutex_ held
    void load_pipeline_cache();
    void save_pipeline_cache();
    void start_workers();
    void stop_workers();
    void compile_worker();
    void make_attachment(VkImage& image,VkDeviceMemory& memory,VkImageView& view,VkExtent2D extent,bool depth=false,VkFormat format=VK_FORMAT_UNDEFINED,uint32_t layers=1);
    void collect();
    SDL_Window* window_ = nullptr;
    class XrCompositor* xr_ = nullptr;   // not owned
    // The desktop stereo preview (UC_VR_PREVIEW=1, no headset): the headset's stereo path (multiview, HOTD4's per-draw reprojection) with
    // fixed Quest 3 eyes, both eyes side by side in the window and in captures. stereo_: the scene the draws are reprojected with, the
    // headset's or the preview's.
    std::unique_ptr<xr::StereoScene> preview_;
    xr::StereoScene* stereo_ = nullptr;
    bool preview_frame_open_ = false, preview_lens_seen_ = false;
    uint64_t preview_world_frames_ = 0;   // frames that drew the world (UC_VR_CLASSIFY_DUMP picks one)
    std::atomic<bool> menu_context_{true};
    void preview_publish();
    bool xr_frame_ = false;   // this presented frame draws into the headset (begin_frame() said so)
    uint32_t layers_ = 1;     // 2 with the headset: the EFB, its copies and the presented picture hold one layer per eye (multiview)
    bool world_drawn_ = false;   // this frame drew the world already: full-screen 2D after it fills the eye (HOTD4's FILL), before it is the HUD screen
    PromptOverlay prompt_overlay_;
    MenuOverlay menu_overlay_;
    std::function<void()> menu_build_, menu_setup_;
    PromptStyle prompt_style_=PromptStyle::monochrome;
    BloomSettings bloom_{false,100};
    BloomPass bloom_pass_;   // one glow image for all slots: it is read by the present (or the HUD-less copy) of the frame that wrote it, before the next frame's bloom
    float crosshair_scale_=1;
    GraphicsEffects chosen_effects_;   // as configured
    GraphicsEffects effects_;   // effective_effects(chosen_effects_): what the passes run from
    bool anisotropy_supported_=false; float anisotropy_limit_=1;
    bool depth_clamp_=false;   // the headset: game pipelines clamp depth instead of clipping at the eye frustum's near and far planes
    void create_samplers();
    void apply_scene_path(bool anisotropy_changed, bool path_before);
    void destroy_samplers();
    // DLSS 5: the settings, the pass, whether the device carries the NGX extensions (else dlss5_gap_ says why not) and the frame's scene inputs kept for the present stage.
    bool dlss5_requested() const { return dlss5_device_ && dlss5_settings_.enabled() && !dlss5_pass_.failed(); }
    bool record_dlss5(VkCommandBuffer command, Dlss5Hook hook, const UpscaleImage& picture, VkOffset2D origin, VkExtent2D size, std::array<float,2> jitter, bool black_output);
    bool dlss5_allowed_=true;
    Dlss5Settings dlss5_settings_;
    Dlss5Pass dlss5_pass_;
    Dlss5Context dlss5_context_;
    bool dlss5_device_=false;
    std::string dlss5_gap_;
    struct Dlss5Scene { UpscaleImage depth, motion; VkExtent2D size{}; bool reset=false, ready=false; } dlss5_scene_;   // ready: this frame's scene was reconstructed (the present stage may run)
    UpscalerKind upscaler_kind_=UpscalerKind::off;
    bool dlss_available_=false, dlss_g_available_=false;
    UpscaleMode upscale_mode_=UpscaleMode::quality;
    bool sharpening_=false, frame_generation_=false;
    double sharpness_=0.5;
    uint32_t frame_generation_multiplier_=2;
    bool ray_tracing_=false;
    RrInputsPass rr_inputs_;
    bool rr_frame_=false;   // the latest frame went through Ray Reconstruction (RR_active())
    bool rr_loaded_=false, rr_available_=false;   // the Ray Reconstruction plugin loaded with Streamline / the GPU supports it (docs/superpowers/plans/2026-10-20-ray-tracing-p7-ray-reconstruction.md)
    bool rt_frame_=false, rt_as_open_=false;   // rt_frame_: ray tracing was active at this frame's start; rt_as_open_: the rt_as timer runs on to the TLAS
    RtScene rt_;
    RtShadowPass rt_shadow_;
    RtDenoiser rt_shadow_denoiser_;
    RtReflectionPass rt_reflection_;
    RtDenoiser rt_reflection_denoiser_;
    bool rt_reflect_ran_last_=false;
    RtReflectionPass rt_lighting_;   // the same pass in its lighting mode
    RtDenoiser rt_lighting_denoiser_;
    bool rt_light_ran_last_=false;
    std::optional<RtPathSettings> rt_path_override_;   // tests: fixed settings instead of the defaults (and the UC_PT_* debug variables)
    RtPathPass rt_path_;
    RtDenoiser rt_path_denoiser_;
    bool rt_path_ran_last_=false;
    RtPathSettings rt_path_last_settings_;
    bool rt_trace_lamps_=false;
    std::array<EffectLevel,3> rt_signal_levels_{};   // the levels (shadows, local shadows, ambient occlusion) the denoiser's history was accumulated under
    std::array<float,3> rt_key_direction_=kRtOverheadDirection;   // the synthetic key light's view-space direction, turned with the camera so that it stands still in the world
    bool rt_shadow_traced_=false, rt_shadow_ran_last_=false;   // traced: the signal image holds a frame's trace; ran_last: the previous frame denoised (else the history is stale)
    VkBufferUsageFlags rt_input_usage() const { return ray_tracing_ ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT|VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0; }   // mesh and palette buffers the AS builder reads
    bool motion_outputs_=false;   // upscaling: pipelines carry the motion attachment and the jitter (smoke.motion.*.spv)
    // Upscaling: the 3D scene of a guest frame renders into these internal-resolution targets up to the scene boundary, then the upscaler
    // fills efb_ and the rest of the frame (post chain, HUD) draws into efb_ at output resolution.
    std::unique_ptr<Streamline> streamline_;   // DLSS: loaded before the instance, shut down before the device
    PresentFunctions present_, base_present_;   // current backend and the preloaded Streamline calls
    bool fsr_presentation_=false, dlss_presentation_=false;
    std::unique_ptr<Upscaler> upscaler_;
    RcasPass rcas_;   // sharpening after upscalers that do not sharpen themselves
    // DLSS Frame Generation: the HUD-less copy of the frame at back-buffer size, taken before the HUD packet (or at the present).
    std::unique_ptr<DlssFrameGeneration> dlss_g_;
    // FSR Frame Generation (frame interpolation swapchain); its interpolation may still read the previous frame's HUD-less copy, hence two.
    std::unique_ptr<FsrFrameGeneration> fsr_g_;
    bool dlss_generation_enabled() const { return frame_generation_ && upscaler_kind_==UpscalerKind::dlss && bool(dlss_g_); }
    bool fsr_generation_enabled() const { return frame_generation_ && upscaler_kind_==UpscalerKind::fsr && fsr_presentation_ && fsr_g_ && fsr_g_->has_context(); }
    bool streamline_swapchain() const { return dlss_presentation_; }
    VkQueue fg_present_queue_=VK_NULL_HANDLE, fg_acquire_queue_=VK_NULL_HANDLE;   // extra queues of the game's family for the interpolation swapchain
    VkFormat swapchain_format_=VK_FORMAT_UNDEFINED;
    VkImage hudless_[2]{}; VkDeviceMemory hudless_memory_[2]{}; VkImageView hudless_view_[2]{};
    VkExtent2D hudless_extent_{};
    VkFormat hudless_format_=VK_FORMAT_R8G8B8A8_UNORM;   // the back buffer's: HDR10 while HDR is active (frame generation compares the two)
    HdrSettings hdr_settings_;
    struct HeldFrame {   // the last presented picture without the reticle, and the reticle draws that belong on it
        VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE;
        VkImage depth=VK_NULL_HANDLE; VkDeviceMemory depth_memory=VK_NULL_HANDLE; VkImageView depth_view=VK_NULL_HANDLE;   // scratch depth of the reticle pass (cleared every use)
        VkExtent2D extent{}; bool valid=false, image_ready=false, presented=false;
        std::array<uint32_t,4> display_source{0,0,640,480};
        std::vector<gx::DrawPacket> draws;
    } held_;
    std::vector<gx::DrawPacket> frame_reticle_;   // reticle draws of the frame being recorded (they may come in earlier packets than its present)
    bool reticle_layer_=false;
    void ensure_held_frame();
    void destroy_held_frame();
    bool hdr_active_=false;       // this swapchain is HDR10 (settings on, Windows HDR on, surface offers it)
    VkExtent2D bloom_region_{};   // the part of the EFB the glow covers, in EFB pixels (set when the bloom is recorded)
    uint64_t bloom_generation_=0; // BloomPass::generation() the HDR pass's descriptor sets were last reset for
    BloomLayer bloom_layer(VkOffset2D origin, VkExtent2D size) const;   // the glow for an HDR encode of this EFB rectangle; empty unless this frame's glow is valid
    static constexpr uint32_t kFragQueries=16384; VkQueryPool frag_pool_=VK_NULL_HANDLE; bool frag_dump_=false; std::vector<uint32_t> frag_queries_;   // UC_FRAG_DUMP
    size_t stereo_stride_=128;   // bytes between the headset's eye blocks (minUniformBufferOffsetAlignment)
    bool keep_frame_=false;   // keep_frame(): the next presented frame keeps its slot image (captures read it)
    bool load_op_none_=false;   // VK_KHR/EXT_load_store_op_none enabled: depth a pass does not use is not loaded
    PFN_vkCmdSetFragmentShadingRateKHR set_shading_rate_=nullptr;   // VK_KHR_fragment_shading_rate (pipeline rate) enabled: the bloom chain's draws shade coarsely
    // The headset's fixed foveation (renderer/fovea_map.h): an attachment shading-rate image over the EFB, one layer per eye, coarser
    // towards each eye's periphery, bound to every EFB pass while the level (vr.cfg "foveation", UC_VR_FOVEATION) is above 0. The draws
    // it applies to (world draws of the eyes' scene, full-screen fades, 2D passes over full-size copies) take its rate; the HUD, the
    // reticle, the bloom chain's coarse draws and pictures drawn for a small copy keep their own. UC_NO_FOVEATION=1: no attachment rate.
    bool fovea_supported_=false, fovea_layered_=false, fovea_max_op_=false;
    VkExtent2D fovea_texel_{16,16};
    uint32_t fovea_largest_=4;   // the largest square rate the device has (1, 2 or 4)
    int fovea_level_=0;          // the level the maps hold (0: none bound)
    // A map: the EFB's (every EFB pass) and the eye image's (the compositor's screen pass, UC_NO_EYE_FOVEATION=1: none).
    struct FoveaMap { VkImage image=VK_NULL_HANDLE; VkDeviceMemory memory=VK_NULL_HANDLE; VkImageView view=VK_NULL_HANDLE; VkExtent2D extent{}; std::vector<float> key; };
    FoveaMap fovea_efb_, fovea_eye_;
    std::array<float,4> fovea_shown_{0,0,640,360};   // the last presented display region, in guest pixels
    void update_fovea();
    bool update_fovea_map(FoveaMap& map, VkExtent2D extent, const std::array<float,4>& shown, const std::array<std::array<float,4>,2>& tangents, int level, uint32_t largest);
    void destroy_fovea();
    void destroy_fovea_map(FoveaMap& map);
    // The headset's HUD layer (hud_layer.h): the frame's 2D HUD draws in a two-layer target of the HUD viewport * k (L: what they add,
    // T: what the scene behind keeps), laid over the eyes by the screen pass, or composited into the EFB (flush_hud_layer) before anything
    // else reads or draws into it. UC_NO_HUD_LAYER=1: the HUD is drawn into the EFB as before.
    struct HudLayer {
        VkImage light=VK_NULL_HANDLE, transmit=VK_NULL_HANDLE; VkDeviceMemory light_memory=VK_NULL_HANDLE, transmit_memory=VK_NULL_HANDLE;
        VkImageView light_view=VK_NULL_HANDLE, transmit_view=VK_NULL_HANDLE; VkExtent2D extent{};
        bool used=false;            // this frame drew into it (cleared at its first pass of the frame), not yet laid over the eyes
        HudLayerMap map{};          // where it lies in each eye (valid while used)
        VkPipeline flush[2]{};      // the composite into the EFB: multiply by T, then add L
        VkPipelineLayout layout=VK_NULL_HANDLE; VkDescriptorSetLayout set_layout=VK_NULL_HANDLE; VkDescriptorPool pool=VK_NULL_HANDLE; VkDescriptorSet set=VK_NULL_HANDLE;
        VkSampler sampler=VK_NULL_HANDLE; VkShaderModule vertex=VK_NULL_HANDLE, fragment=VK_NULL_HANDLE;
        uint64_t frames=0, draws=0, flushes=0;   // UC_GPU_PACKETS statistics
    };
    HudLayer hud_, glow_;   // glow_: the glow layer (hud_layer.h hud_overlay_map), under the HUD; the composite pipelines live in hud_
    bool hud_layer_wanted() const;
    bool crosshair_hidden() const;
    void ensure_hud_layer(HudLayer& layer, VkExtent2D extent);
    void destroy_hud_layer();
    void flush_hud_layer(VkCommandBuffer command);   // both layers, the glow first
    void flush_layer(VkCommandBuffer command, HudLayer& layer);
    // The bloom proxy (UC_NO_BLOOM_PROXY=1: off): the EFB's top-left corner at 2 points a guest pixel while the game's bloom chain draws there.
    static constexpr uint32_t kBloomCorner[2]={320,180};
    struct BloomProxy {
        VkImage color=VK_NULL_HANDLE, depth=VK_NULL_HANDLE; VkDeviceMemory color_memory=VK_NULL_HANDLE, depth_memory=VK_NULL_HANDLE;
        VkImageView color_view=VK_NULL_HANDLE, depth_view=VK_NULL_HANDLE; VkExtent2D extent{};
        bool initialized=false, valid=false;   // valid: the corner's content is in the proxy, not in the EFB
        std::array<float,4> covered{};          // what the chain's draws covered since it started (guest pixels): put back into the EFB
        uint64_t packets=0, put_backs=0;
    };
    BloomProxy bloom_proxy_;
    // The particle layer (UC_PARTICLE_LAYER=1): a packet's run of translucent world sprites at half the EFB's size a side.
    struct ParticleLayer {
        VkImage color=VK_NULL_HANDLE, depth=VK_NULL_HANDLE; VkDeviceMemory color_memory=VK_NULL_HANDLE, depth_memory=VK_NULL_HANDLE;
        VkImageView color_view=VK_NULL_HANDLE, depth_view=VK_NULL_HANDLE; VkExtent2D extent{}; bool initialized=false;
        VkDescriptorSet set=VK_NULL_HANDLE; VkPipeline composite=VK_NULL_HANDLE;
        // the depth downsample (particle_depth.slang): its own set (the scene depth), layout and pipeline
        VkDescriptorSetLayout depth_set_layout=VK_NULL_HANDLE; VkPipelineLayout depth_layout=VK_NULL_HANDLE; VkDescriptorPool depth_pool=VK_NULL_HANDLE;
        VkDescriptorSet depth_set=VK_NULL_HANDLE; VkPipeline depth_pipeline=VK_NULL_HANDLE; VkShaderModule depth_vertex=VK_NULL_HANDLE, depth_fragment=VK_NULL_HANDLE;
        VkImageView scene_depth_view=VK_NULL_HANDLE;   // the view the set holds (the EFB depth's, remade with the EFB)
        uint64_t packets=0, draws=0;
    };
    ParticleLayer particle_;
    static bool particle_layer_on() { static const bool on=[] { const char* v=std::getenv("UC_PARTICLE_LAYER"); return v && *v=='1'; }(); return on; }
    void ensure_particle_layer();
    void destroy_particle_layer();
    void composite_particle_layer(VkCommandBuffer command);
    void ensure_bloom_proxy();
    void destroy_bloom_proxy();
    void bloom_proxy_corner(VkCommandBuffer command, bool to_efb);
    void put_back_bloom_proxy(VkCommandBuffer command);
    int wanted_fovea_level() const;
    bool colorspace_ext_=false;   // VK_EXT_swapchain_colorspace enabled: HDR10 surface formats are usable
    const char* hdr_status_=nullptr;   // the last logged mode (a string literal), printed when it changes
    HdrPass hdr_pass_;
    bool hudless_taken_=false;
    uint32_t frame_index_=0;   // guest frames presented: the frame token index (must match at present)
    uint64_t fg_waited_value_=0;   // the frame-generation inputs fence value the scene targets were last released at
    // Layouts of the scene depth and vectors between packets: attachment layouts during the scene, GENERAL after the upscale until the next
    // frame's first scene pass (frame generation reads them at the present in that layout).
    VkImageLayout scene_depth_layout_=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, scene_motion_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, scene_gbuffer_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkImage scene_color_=VK_NULL_HANDLE, scene_depth_=VK_NULL_HANDLE, scene_motion_=VK_NULL_HANDLE, scene_gbuffer_=VK_NULL_HANDLE;
    VkDeviceMemory scene_color_memory_=VK_NULL_HANDLE, scene_depth_memory_=VK_NULL_HANDLE, scene_motion_memory_=VK_NULL_HANDLE, scene_gbuffer_memory_=VK_NULL_HANDLE;
    VkImageView scene_color_view_=VK_NULL_HANDLE, scene_depth_view_=VK_NULL_HANDLE, scene_motion_view_=VK_NULL_HANDLE, scene_gbuffer_view_=VK_NULL_HANDLE;
    VkExtent2D scene_extent_{};
    bool scene_initialized_=false, motion_initialized_=false, gbuffer_initialized_=false;
    bool gbuffer_cleared_=false;   // the G-buffer was cleared (and bound) by a scene pass of this frame
    bool force_gbuffer_=false, force_scene_targets_=false, force_lights_=false;
    OcclusionPass occlusion_;
    MotionBlurPass motion_blur_;
    ShadowMap shadow_map_;
    VkShaderModule shadow_vertex_module_=VK_NULL_HANDLE;
    VkPipeline shadow_pipeline_=VK_NULL_HANDLE;
    VkPipeline make_shadow_pipeline();
    std::optional<ShadowFit> shadow_fit_;                  // this frame's light box (last frame's dominant directional light), set with the world projection
    bool shadow_cleared_=false;                            // the map was cleared (and drawn into) by a scene packet of this frame
    uint64_t targets_generation_=1;                        // bumped whenever the scene targets or the EFB are destroyed
    std::optional<gx::Projection> frame_projection_;       // the projection the scene's world was drawn with this frame (what the screen-space effects reconstruct with)
    gx::SceneLightCollector scene_lights_;                 // the scene draws' lights of this frame, filled while the scene is recorded
    std::optional<gx::CollectedLight> dominant_light_;     // chosen at the scene boundary, kept across frames (hysteresis)
    // The last present's EFB clear, applied to the scene targets by the next frame's first scene pass (GX clears the EFB at the present copy).
    struct SceneClear { bool pending=false; std::array<uint32_t,4> region{}; std::array<uint8_t,4> color{}; uint32_t depth=0xFFFFFF; } scene_clear_;
    gx::SceneBoundary boundary_;
    std::optional<gx::TextureCopy> scene_restore_copy_;   // final scene snapshot restored over the post chain's scratch workspace
    gx::MotionTracker tracker_;
    bool frame_start_=true, scene_phase_=false, scene_mode_=false, motion_cleared_=false, reset_next_=true;
    uint64_t upscale_frame_=0, whole_frame_upscales_=0;
    std::array<float,2> jitter_{};
    gx::Projection scene_projection_{};
    std::chrono::steady_clock::time_point last_upscale_{};
    std::array<VkSampler,256> scene_samplers_{};   // texture_samplers_ with the upscaling mip bias, for scene draws
    void destroy_scene_targets();
    // Ends the scene of this frame: reconstructs scene_color_ into efb_, carries the scene depth into depth_ for the draws after it.
    void record_upscale(VkCommandBuffer command, bool black_output);
    void configure_generation();
    UpscaleImage hudless_image() const;
    std::thread::id owner_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t family_ = 0;
    VkPhysicalDeviceMemoryProperties memory_{};
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    bool present_fences_=false;
    bool disallow_exclusive_=false;
    VkFence retirement_acquired_=VK_NULL_HANDLE;   // legacy resize: proves a successor presentation finished
    VkExtent2D extent_{};
    VkExtent2D requested_{};
    double configured_scale_=0.5, render_aspect_=16.0/9;
    bool native_eye_scale_=false;
    uint32_t max_image_dimension_=0;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::map<PipelineKey,VkPipeline> pipelines_, uber_pipelines_;   // guarded by pipelines_mutex_ with the members below: compile threads insert too
    // The last specialized pipeline pipeline_for found, by its requested key (render thread only): consecutive draws mostly share one, and a
    // specialized entry never changes until shutdown, so the lock and the map search are skipped for them.
    PipelineKey last_key_{}; VkPipeline last_pipeline_=VK_NULL_HANDLE;
    std::mutex pipelines_mutex_;
    struct CompileJob { PipelineKey key; bool uber; };
    std::deque<CompileJob> compile_queue_;
    std::set<PipelineKey> queued_, uber_queued_;
    std::condition_variable compile_cv_;
    size_t compiling_ = 0, compiled_ = 0, compiled_ubers_ = 0, jobs_done_ = 0;
    size_t compile_limit_ = 0, game_workers_ = 0;   // threads allowed to compile at once: all of them during warm-up, game_workers_ in game
    double compiled_ms_ = 0, compiled_uber_ms_ = 0;   // compile time since the last idle report
    bool compile_stop_ = false;
    bool uber_only_ = false;
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    size_t pipeline_cache_loaded_ = 0;
    bool probe_fragment_ = false;   // UC_PROBE_FS (diagnostics): no folded TEV modules
    // The headset's vertex pre-pass (smoke.slang xformMain): each packet's vertices are shaded once by a compute dispatch, the draws read
    // the results (smoke.stereo.xform.vert.spv). Needs bufferDeviceAddress (the jobs point at the mesh slabs); UC_NO_XFORM=1 turns it off.
    bool xform_ = false;
    bool static_depth_ = false;   // UC_STATIC_DEPTH=1 (headset): cull and depth state in the pipelines instead of dynamic
    std::vector<std::array<float,4>> cull_points_;   // the headset's culling: one draw's clip-space points (scratch)
    VkShaderModule xform_vertex_module_ = VK_NULL_HANDLE, xform_compute_module_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout xform_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout xform_layout_ = VK_NULL_HANDLE;
    VkPipeline xform_pipeline_ = VK_NULL_HANDLE;
    // The headset's space warp (XrCompositor::space_warp): the motion pass (xr_motion.slang) draws the frame's depth-writing world draws
    // again into the runtime's motion and depth images, from the pre-pass's positions. warp_track_: this display frame feeds tracker_ and
    // writes previous positions; warp_eyes_: the frame's eye rows (game view -> Vulkan clip) at one near/far, from its first such draw.
    struct WarpDraw { uint32_t first, count, kind; VkViewport viewport; };   // viewport: the draw's in the EFB (pixels)
    // The EFB rectangle the eyes show (pixels u0, v0, u1, v1; the last presented frame's): the eye image is this rectangle stretched, so the
    // motion pass maps a draw's EFB viewport through it. Empty (u1 <= u0): the whole EFB.
    std::array<float,4> warp_display_{};
    VkShaderModule warp_vertex_ = VK_NULL_HANDLE, warp_fragment_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout warp_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout warp_layout_ = VK_NULL_HANDLE;
    VkPipeline warp_pipeline_ = VK_NULL_HANDLE;
    bool warp_track_ = false;
    uint64_t warp_draw_count_ = 0, warp_call_count_ = 0;   // the [space-warp] line: world draws the pass took, draw calls it issued
    std::optional<std::array<float,32>> warp_eyes_, warp_eyes_prev_;   // prev: the last presented frame's (the vectors' previous positions)
    void record_warp_pass(Slot& slot, const std::vector<WarpDraw>& draws);
    // The headset's texture table (UC_TEX_MERGE=1, needs xform_ and descriptor indexing): every packet's distinct texture/sampler sets as one
    // array of combined image samplers (set 1); a draw's first entry travels with its vertices (the pre-pass writes it), so draws that differ
    // only in their textures merge into one run. Pipelines with key bit kTexTableKeyBit use the TEX_TABLE modules and table_layout_.
    bool tex_merge_ = false, table_update_after_bind_ = false;
    uint32_t table_limit_ = 0;   // entries a packet's table may hold (the device's per-stage limits less set 0's eight)
    bool table_eager_ = false;   // UC_TEX_MERGE_EAGER=1: every draw that can reads the table (default: only runs that join draws of other textures)
    float table_max_pixels_ = 0;   // UC_TEX_MERGE_PX: only draws whose screen box covers at most this many guest pixels join across textures (0: any)
    VkDescriptorSetLayout table_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout table_layout_ = VK_NULL_HANDLE;
    VkShaderModule table_vertex_module_ = VK_NULL_HANDLE, table_fragment_module_ = VK_NULL_HANDLE, table_early_module_ = VK_NULL_HANDLE;
    PipelineKey last_table_key_{}; VkPipeline last_table_pipeline_ = VK_NULL_HANDLE;   // table_pipeline's memo (render thread only)
    uint64_t table_runs_ = 0, table_joins_ = 0, table_packets_ = 0, table_overflows_ = 0, table_pending_ = 0, table_entries_ = 0;   // [tex-merge] (UC_GPU_PACKETS)
    // The headset's small-draw ubershader (uber_merge.h; UC_UBER_SMALL=1 for the EFB passes and the HUD layer, =scene or =hud for one of
    // them; needs the texture table, which it turns on): draws whose screen box covers at most uber_max_pixels_ guest pixels (UC_UBER_PX) go on
    // the ubershader pipeline of their fixed-function state (key bit kUberMergeKeyBit, merge_layout_: set 2 the packet's uniform blocks), so
    // runs join draws of different TEV setups, uniform blocks and textures. A draw moves onto it only to join a run it could not join
    // otherwise: a run of one draw, or of draws that join anyway, keeps its folded module.
    // frag_uniform_merge_ (UC_MERGE_FRAG_UNIFORM=1): runs join draws whose uniform blocks differ only where no fragment reads them.
    bool uber_small_scene_ = false, uber_small_hud_ = false, frag_uniform_merge_ = false, frag_uniform_forced_ = false;
    bool frag_uniform_merge() const;   // frag_uniform_merge_, or live from the headset's Display page
    bool post_shading_coarse() const;  // the Display page's "Post-effect shading" (post_shading_rate)
    float uber_max_pixels_ = 0;
    VkDescriptorSetLayout merge_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout merge_layout_ = VK_NULL_HANDLE;
    VkShaderModule merge_vertex_module_ = VK_NULL_HANDLE, merge_fragment_module_ = VK_NULL_HANDLE, merge_early_module_ = VK_NULL_HANDLE;
    uint32_t uber_families_ = 0;   // the ubershader's families of draws with 1 and 2 stages (bit family-1; uber_merge.h; UC_UBER_FAMILY=0 none, =2 the two-stage one)
    VkShaderModule few_vertex_module_[2]{}, few_fragment_module_[2]{}, few_early_module_[2]{};   // per family (index family-1)
    PipelineKey last_merge_key_[3]{}; VkPipeline last_merge_pipeline_[3]{};   // uber_merge_pipeline's memo per family (render thread only)
    uint64_t uber_runs_ = 0, uber_draws_ = 0, uber_joins_ = 0, uber_singles_ = 0, uber_pending_ = 0, uber_big_ = 0, frag_uniform_joins_ = 0;   // [uber-merge] (UC_GPU_PACKETS)
    uint64_t uber_few_runs_ = 0, uber_stage_draws_[4] = {};   // [uber-merge]: draws in EFB ubershader runs by stage count (0, 1, 2, 3 or more)
    uint64_t hud_uber_runs_ = 0, hud_uber_draws_ = 0, hud_layer_runs_ = 0, hud_layer_draws_ = 0;
    double uber_pixels_ = 0;   // guest pixels of the draws drawn on the ubershader (their screen boxes)
    VkBufferUsageFlags vertex_usage() const { return rt_input_usage() | (xform_ ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0); }   // buffers the pre-pass reads by address
    VkShaderModule vertex_module_ = VK_NULL_HANDLE, fragment_module_ = VK_NULL_HANDLE, early_module_ = VK_NULL_HANDLE, uber_module_ = VK_NULL_HANDLE, nobank_vertex_module_ = VK_NULL_HANDLE;   // early_module_: EARLY_Z (no discard)
    std::filesystem::path cache_dir_;
    std::set<PipelineKey> known_keys_;        // keys already in the seed or recorded file
    std::vector<PipelineKey> warm_keys_;
    std::vector<std::thread> compilers_;
    std::map<const gx::TexturePixels*,Texture> copy_textures_;
    struct CopyAlias { std::weak_ptr<const gx::TexturePixels> requested; std::shared_ptr<const gx::TexturePixels> rendered; };
    std::unordered_map<const gx::TexturePixels*,CopyAlias> copy_aliases_;
    std::unordered_map<uint32_t,std::weak_ptr<const gx::TexturePixels>> latest_copies_;
    bool detached_copies_=false;
    std::unordered_set<const gx::TexturePixels*> lost_copies_;   // EFB copies made by a previous renderer (lost_copies)
    std::shared_ptr<const gx::TexturePixels> blank_;             // what a lost copy is drawn as
    std::unordered_map<const gx::MeshBuffers*,GpuMesh> meshes_;
    uint64_t next_mesh_id_=1;
    std::vector<std::unique_ptr<MeshSlab>> mesh_slabs_;   // the last one is the one being filled
    uint64_t renders_=0;
    std::vector<Texture> recycled_copies_;
    VkDeviceSize recycled_copy_bytes_=0;
    size_t uniform_stride_=sizeof(gx::DrawUniform);
    std::filesystem::path shaders_;
    bool logic_ops_=false;
    VkDescriptorSetLayout texture_layout_=VK_NULL_HANDLE;
    std::array<VkSampler,256> texture_samplers_{};
    std::array<VkSampler,256> auto_samplers_{};   // texture_samplers_ with linear mip filtering, for textures with generated mips (linear minification modes)
    VkImage efb_ = VK_NULL_HANDLE;
    VkExtent2D efb_extent_{};
    VkDeviceMemory efb_memory_ = VK_NULL_HANDLE;
    VkImageView efb_view_ = VK_NULL_HANDLE;
    bool efb_initialized_ = false;
    VkImage depth_=VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_=VK_NULL_HANDLE;
    VkImageView depth_view_=VK_NULL_HANDLE;
    static constexpr size_t kFrameSlots = 16;   // GPU submissions in flight (6 stalled the render thread behind the first heavy packet of a ~23-packet frame; 16 ends that stall, 48 gains ~5% more for much more host-visible memory); the guest's queue window is larger (gx::kMaxOutstandingPackets)
    std::unordered_map<const gx::TexturePixels*,std::weak_ptr<Texture>> texture_pool_;   // before slots_: the slots' textures erase themselves from it
    std::array<Slot, kFrameSlots> slots_{};
    // RT albedo/material targets (scene extent, created and destroyed with rt_material_active()) and the records of the frame: per frame-in-flight
    // buffers, grow-only, filled per scene packet from rt_record_count_ on. Read by RT passes at the scene boundary.
    VkImage scene_material_=VK_NULL_HANDLE, scene_albedo_=VK_NULL_HANDLE;
    VkDeviceMemory scene_material_memory_=VK_NULL_HANDLE, scene_albedo_memory_=VK_NULL_HANDLE;
    VkImageView scene_material_view_=VK_NULL_HANDLE, scene_albedo_view_=VK_NULL_HANDLE;
    VkImageLayout scene_rt_layout_=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;   // of both
    bool rt_material_initialized_=false, rt_material_cleared_=false;   // cleared (and bound) by a scene pass of this frame
    // RtMaterialRecord / 8 gx::LightUniform per record. A growth destroys the replaced buffer mid-frame: a consumer binds them only after the frame's
    // last scene packet (the boundary).
    static constexpr size_t kRtFrames = 5;   // frames of per-frame RT state (acceleration structures, records): a frame starts once the frame before the previous one is complete (Packets seals a presenting packet only with fewer than two unfinished), so four would do
    std::array<Buffer,kRtFrames> rt_records_{}, rt_lights_{};
    uint32_t rt_record_slot_=0, rt_record_count_=0;
    void sync_rt_material_targets();   // the GPU is idle or the targets are unused
    void destroy_rt_material_targets();
    uint32_t scene_color_attachments() const { return motion_outputs_ ? (ray_tracing_ ? 5 : 3) : 1; }   // the RT pair only on RT devices (maxColorAttachments may be 4 elsewhere)
    std::vector<PresentImage> images_;
    struct RetiredSwapchain { VkSwapchainKHR chain; std::vector<PresentImage> images; };
    std::vector<RetiredSwapchain> retired_;   // swapchains replaced by a resize, freed once their queued presentations finished
    VkSwapchainKHR old_swapchain_ = VK_NULL_HANDLE;   // the newest retired chain, passed as oldSwapchain to its successor
    std::vector<uint64_t> completed_;
    size_t next_slot_ = 0;
    uint64_t last_serial_ = 0;
    bool resized_ = true;
    std::chrono::steady_clock::time_point last_rebuild_{};
    std::atomic<uint32_t> errors_{0};
    std::atomic<uint64_t> presentations_{0}, gpu_busy_ns_{0}, gpu_window_max_ns_{0};
    PassTimer pass_timer_;
    VkQueryPool timing_pool_ = VK_NULL_HANDLE;   // two timestamps per slot
    double timestamp_period_ = 0;
    uint64_t texture_creations_=0;
    uint64_t descriptor_pool_creations_=0;
};
}
