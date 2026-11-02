// Native (DLL) mods off Windows: the mods are Windows DLLs, so loading one reports that it cannot run here. The hash of the DLL still works,
// because the Mods tab shows it and stores the confirmation in the registry the same way on every host.
#include "mod_paths.h"
#include "runtime_native.h"
#include "sha256.h"
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace uc::mods {
std::string sha256_file_hex(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    Sha256 hash;
    std::vector<char> buffer(1 << 16);
    while (in) {
        in.read(buffer.data(), std::streamsize(buffer.size()));
        if (in.gcount() > 0) hash.update(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buffer.data()), size_t(in.gcount())));
    }
    if (in.bad()) return {};
    return to_hex(hash.finish());
}

namespace {
class UnavailableNativeRuntime : public ModRuntime {
public:
    explicit UnavailableNativeRuntime(NativePolicy policy) : policy_(std::move(policy)) {}
    std::string name() const override { return "native DLL"; }
    CallResult load(const fs::path& mod_root, const std::string& entry) override {
        if (!policy_.allow_native) return fail("native (DLL) mods are switched off: turn on \"Allow native mods\" in the Mods tab");
        const auto path = confine(mod_root, entry);
        if (!path || !fs::is_regular_file(*path)) return fail("cannot find " + entry);
        return fail("native (DLL) mods run only in the Windows version of the port");
    }
    CallResult fire(std::string_view, const Value&, Budget) override { return {}; }
    CallResult draw_ui(Budget) override { return {}; }

private:
    NativePolicy policy_;
    static CallResult fail(std::string text) { return CallResult{false, std::move(text), false}; }
};
}  // namespace

std::unique_ptr<ModRuntime> make_native_runtime(ModContext&, NativePolicy policy) { return std::make_unique<UnavailableNativeRuntime>(std::move(policy)); }
}  // namespace uc::mods
