// Builds without the vendor upscalers (UC_VENDOR_UPSCALERS off: macOS, Android): DLSS, FSR and NGX report that they are not
// available on this platform instead of loading the Windows-only Streamline, NGX and FidelityFX runtimes.
// Streamline::load and the frame generation constructors fail, so no object of these classes ever exists: their other members are defined
// only so the renderer links, and are never reached.
#include "dlss_frame_generation.h"
#include "fsr_frame_generation.h"
#include "ngx_dlss5.h"
#include "streamline.h"
#include "upscaler.h"
#include <stdexcept>

namespace uc {
struct StreamlineApi {};
namespace {
constexpr const char* kUnavailable = "not available on this platform";
[[noreturn]] void unavailable(const char* what) { throw std::runtime_error(std::string(what) + " is " + kUnavailable); }
}

std::unique_ptr<Streamline> Streamline::load(const std::filesystem::path&, bool, std::string& reason, bool, RendererBackend) {
    reason = std::string("NVIDIA Streamline (DLSS) is ") + kUnavailable;
    return nullptr;
}
Streamline::~Streamline() = default;
bool Streamline::set_native_device(void*, std::string& reason) { reason = kUnavailable; return false; }
bool Streamline::native_dlss_supported(const std::array<uint8_t, 8>&, std::string& reason) const { reason = kUnavailable; return false; }
bool Streamline::native_ray_reconstruction_supported(const std::array<uint8_t, 8>&, std::string& reason) const { reason = kUnavailable; return false; }
bool Streamline::native_dlss_g_supported(const std::array<uint8_t, 8>&, std::string& reason) const { reason = kUnavailable; return false; }
bool Streamline::upgrade_native_interface(void**, std::string& reason) const { reason = kUnavailable; return false; }
VkResult Streamline::create_instance(const VkInstanceCreateInfo&, VkInstance&) const { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult Streamline::create_device(VkInstance, VkPhysicalDevice, const VkDeviceCreateInfo&, VkDevice&) const { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult Streamline::enumerate_physical_devices(VkInstance, uint32_t*, VkPhysicalDevice*) const { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult Streamline::create_surface(VkInstance, void*, VkSurfaceKHR&) const { return VK_ERROR_INITIALIZATION_FAILED; }
void Streamline::destroy_surface(VkInstance, VkSurfaceKHR) const {}
bool Streamline::dlss_supported(VkPhysicalDevice, std::string& reason) const { reason = kUnavailable; return false; }
bool Streamline::ray_reconstruction_supported(VkPhysicalDevice, std::string& reason) const { reason = kUnavailable; return false; }
bool Streamline::dlss_g_supported(VkPhysicalDevice, std::string& reason) const { reason = kUnavailable; return false; }
PresentFunctions Streamline::present_functions(VkDevice) const { return {}; }
void Streamline::begin_frame(uint32_t) {}
void Streamline::marker(Marker) const {}
void Streamline::enable_reflex() const {}
void Streamline::shutdown() {}

std::unique_ptr<Upscaler> make_dlss_upscaler(const Streamline&, UpscaleMode) { unavailable("DLSS"); }
std::unique_ptr<Upscaler> make_fsr_upscaler(VkDevice, VkPhysicalDevice, const std::filesystem::path&) { unavailable("AMD FSR"); }
std::unique_ptr<NgxBackend> make_ngx_backend() { return nullptr; }
std::vector<std::string> ngx_dlss5_instance_extensions(const std::filesystem::path&) { return {}; }
std::vector<std::string> ngx_dlss5_device_extensions(VkInstance, VkPhysicalDevice, const std::filesystem::path&) { return {}; }

DlssFrameGeneration::DlssFrameGeneration(const Streamline& streamline, uint32_t) : streamline_(streamline) { unavailable("DLSS frame generation"); }
DlssFrameGeneration::~DlssFrameGeneration() = default;
bool DlssFrameGeneration::set_active(bool, bool) { return false; }
void DlssFrameGeneration::tag_hudless(VkCommandBuffer, const UpscaleImage&) {}
void DlssFrameGeneration::tag_hudless_native(void*, const NativeUpscaleImage&) {}
void DlssFrameGeneration::clear_native_tags() {}
void DlssFrameGeneration::after_present() {}

struct FsrFrameGeneration::Impl {};
FsrFrameGeneration::FsrFrameGeneration(VkDevice, VkPhysicalDevice, const Queues&, const std::filesystem::path&) { unavailable("AMD FSR frame generation"); }
FsrFrameGeneration::~FsrFrameGeneration() = default;
PresentFunctions FsrFrameGeneration::present_functions() { return {}; }
void FsrFrameGeneration::create_context(VkExtent2D, VkExtent2D, VkFormat, float) {}
void FsrFrameGeneration::release_context() {}
bool FsrFrameGeneration::has_context() const { return false; }
void FsrFrameGeneration::begin_frame(bool, const UpscaleImage&, uint64_t) {}
bool FsrFrameGeneration::active() const { return false; }
void FsrFrameGeneration::prepare(const UpscaleInputs&, uint64_t) {}
void FsrFrameGeneration::after_present() {}
}  // namespace uc
