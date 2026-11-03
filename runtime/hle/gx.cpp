#include <cstdlib>
#include "gx.h"
#include "gx_scene.h"
#include "gx_queue.h"
#include "gx_fifo.h"
#include "gx_watch_stamp.h"
#include "host_thread.h"
#include "guest_call.h"
#include "hle/os.h"
#include "gx_audit.h"
#include "vi.h"
#include "late_aim.h"
#include "native_input.h"
#include <atomic>
#include <bit>
#include <cstring>
#include <cstdio>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <set>

#include <variant>
#include <algorithm>
#include <string>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <list>
#include <unordered_map>
#include "clock.h"
#include "public_release.h"
#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#endif

// The GX HLE in two halves (gx_queue.h). The guest thread (the front) runs the HLE calls: what the guest can observe (SDK block writes, guest
// calls, the bytes of a display list being recorded, the draw-done flag and callback), every check that throws, and the immediate-mode data it
// must copy at call time (matrices, lights and colours loaded from guest pointers, the SDK block a draw sees). For everything else it appends a
// command. The GX builder (the back: its own thread, or the guest thread itself when the builder is off) runs the commands in order and keeps
// the renderer's GX state: transforms, materials, bound textures, the recorded display lists' native draws, the mesh and texture caches, the
// draw queue and the packets it seals.
//
// The builder reads vertex arrays, indexed matrices, textures and display lists from guest memory when it runs the command, after the guest has
// moved on: what the hardware's GPU does too (it reads them from memory when it processes the FIFO). The game cannot reuse that memory before
// the GPU passed it, and the guest thread only learns that through the draw-done, which the builder's packets produce: live play delivers it
// when the render thread submits the packet the builder sealed, a replay when the GPU completes it, and network play waits for the builder
// (request_finish). Everything the guest reads back (the channel words a display list call writes, packet serials) the front computes itself.
namespace {
uc::Sched* scheduler;
gx::Packets* packets;
gx::Memory memory;
gx::CommandQueue& queue=*new gx::CommandQueue;   // never destroyed: the builder thread may still run at exit (gx_stop_builder ends it first)
bool builder_wanted=false;

// ---------------- The SDK block a draw sees (front -> back) ----------------
// The SDK block span both snapshots read (the highest word is the viewport at +0x55C), copied by the front when a draw is issued and unchanged
// while the guest does not change it: the builder derives state and material from it instead of from guest memory.
constexpr size_t kSdkSpan=0x560;
struct SdkBlock {
    uint32_t address=0;
    std::array<std::byte,kSdkSpan> bytes;
    uint32_t word(uint32_t offset) const { uint32_t raw; std::memcpy(&raw,bytes.data()+offset,4); return uc_bswap32(raw); }
};
using SdkRef=std::shared_ptr<const SdkBlock>;

// ================= The builder's state (the back) =================
gx::State state;
gx::Transforms transforms;
// Blocks shared by consecutive draws; each path keeps its own so an interleaved path does not evict the other's.
gx::Shared<gx::Transforms> live_transforms,import_transforms,mesh_transforms_block;
gx::Shared<gx::State> live_state,import_state,mesh_state;
gx::BlendState blend_state,live_blend;
gx::DrawUniform color_uniforms;
gx::Shared<gx::DrawUniform> live_uniforms,import_uniforms,mesh_uniforms_block;
gx::TevMaterial live_material;
bool profile_mesh;
bool replaying=false;
uint32_t prompt_color_logs=0;
uint64_t prompt_batch=0;
std::vector<gx::PromptDraw> carried_prompts;
std::atomic<uint64_t> prompt_annotations{},prompt_present{},prompt_fallback{};
uint32_t mesh_profile_count;
// --profile: the first mesh the profile reports dumps its textures. The dump named its files after uc::now_ticks(), a deterministic clock read
// on the guest thread that demos recorded with --profile contain: the front makes those reads (one per dump) once the builder did the dumps.
std::atomic<uint32_t> mesh_dumps{0};
uint32_t mesh_dumps_seen=0;
bool mesh_dumps_settled=false;
uint32_t copy_profile_count;
uint32_t channel_profile_count;
uint32_t texgen_profile_count;
using TextureMatrix=std::array<std::array<float,4>,3>;
using TextureMatrices=std::array<std::optional<TextureMatrix>,126>;
using TextureSelectors=std::array<uint32_t,8>;
TextureMatrices texture_matrices;
using MatrixIndices=std::array<std::optional<uint32_t>,2>;
MatrixIndices matrix_indices;
std::array<uint32_t,16> indirect_stages{};
// Versions of the host-side inputs of snapshot() / material_snapshot() (the rest of their input is the SDK block, compared by identity or bytes).
// Every write to `state`, the recorded input state, `color_uniforms`, `texture_matrices` or `indirect_stages` bumps one (builder only).
uint64_t state_epoch=1,material_epoch=1;
uint64_t cache_stat[8]={}; uint64_t cache_diff[2][0x560/4]={};   // --profile: snapshot hits/misses, material hits/misses per second
inline void dirty_state() { ++state_epoch; }
inline void dirty_material() { ++material_epoch; }
struct TextureSource { uint32_t address; gx::TextureDesc desc; std::optional<uint32_t> palette; uint8_t wrap=0; };
struct PaletteSource { uint32_t address, entries; gx::PaletteFormat format; };
struct PaletteData { gx::PaletteFormat format; std::vector<std::byte> bytes; };
using Palettes=std::map<uint32_t,std::shared_ptr<const PaletteData>>;
// shortcut: SDK region identity; unify aliased regions if observed (TD-038).
Palettes palettes;
using TextureBindings=std::array<std::optional<TextureSource>,8>;
TextureBindings bound_textures;
std::array<uint8_t,8> wraps(const TextureBindings& bindings) {
    std::array<uint8_t,8> result{};
    for(size_t i=0;i<8;++i) if(bindings[i]) result[i]=bindings[i]->wrap;
    return result;
}
std::array<uint8_t,8> live_texture_wrap{};
std::shared_ptr<const gx::TexturePixels> live_texture;
std::array<std::shared_ptr<const gx::TexturePixels>,7> live_texture_maps;
struct CopyState { std::array<uint8_t,4> color{0,0,0,255}; uint32_t depth=0xFFFFFF; };
CopyState copy_state;
using CopiedTextures=std::map<uint32_t,std::shared_ptr<const gx::TexturePixels>>;
CopiedTextures copied_textures;
struct CopiedTexture { gx::TextureCopy request; bool clear; CopyState clear_state; };
using Imported=std::deque<std::variant<gx::DrawPacket,CopiedTexture>>;
std::unique_ptr<gx::Vertices> capture;
struct MeshGeometry { std::shared_ptr<gx::MeshBuffers> buffers; };   // mutable only while the cache entry is built
struct MeshPosition { uint16_t index; uint32_t row; };
struct MeshNormal { uint16_t index; uint32_t row; };
struct MeshTexture { uint16_t index; uint32_t id; uint32_t words; };
struct CachedMesh {
    struct Source { uint32_t address; std::vector<std::byte> bytes; };
    gx::State layout;
    std::vector<std::byte> bytes;
    std::vector<std::variant<MeshGeometry,MeshPosition,MeshNormal,MeshTexture>> operations;
    std::vector<Source> sources;
    size_t size=0;
    mutable uint32_t stamp=0,skips=0;   // write-barrier stamp of the last byte comparison (gx_write_watch.h); hits since then
};
struct MeshCacheEntry { uintptr_t key; std::shared_ptr<const CachedMesh> mesh; };
std::list<MeshCacheEntry> mesh_cache;
std::unordered_map<uintptr_t,std::list<MeshCacheEntry>::iterator> mesh_index;
size_t mesh_cache_bytes=0;
constexpr size_t mesh_cache_budget=256u<<20;
std::atomic<uint64_t> geometry_hits{0},geometry_misses{0};
// One writer (the builder); atomic load/store lets host readers sample without a locked RMW.
inline void add_guest_stat(std::atomic<uint64_t>& counter,uint64_t amount) {
    counter.store(counter.load(std::memory_order_relaxed)+amount,std::memory_order_relaxed);
}
gx::DrawQueue draws;
// A display-list draw's decoded vertices, kept for the next call: the game records its lists again every frame with the same bytes, and a list's
// draw decodes the same while its payload, its vertex format and the vertex arrays it indexes stay the same. A recording that replaces one at the
// same address takes over the decodes of its draws whose payload and format are unchanged (GXEndDisplayList); a call reuses one while the
// arrays its indices read are bound at the same addresses and no guest store touched them since the decode (and anyway decodes every 256th call).
struct DecodedDraw {
    std::array<gx::Array,12> arrays;   // the vertex arrays bound when it was decoded (only the indexed attributes' are compared)
    std::array<std::optional<gx::Vertices::SourceRange>,12> sources;
    uint32_t stamp=0, uses=0;
    std::vector<gx::Vertex> vertices;
    std::vector<uint32_t> indices;
};
std::atomic<uint64_t> list_decodes{0},list_reuses{0};
struct RecordedDraw {
    gx::State state;
    uint32_t callsite;
    uint8_t primitive, vat;
    uint16_t count;
    size_t expected;
    size_t offset=0;   // the payload: `expected` bytes of the list's recorded bytes from here (Resource::list_bytes)
    gx::TevMaterial material;
    gx::DrawUniform uniforms;
    TextureSelectors texture_selectors;
    std::optional<uc::PromptDescription> prompt;
    bool compact=false;
    mutable std::shared_ptr<DecodedDraw> decoded;   // the builder's cache (see DecodedDraw)
};
uint64_t list_stat[2]={};   // --profile: display-list draws imported and their vertices (the game records its lists again every frame: each is called about once)
struct Resource {
    std::shared_ptr<const std::vector<std::byte>> list_bytes;   // the bytes the guest recorded (the draws' payloads), shared with the front's copy
    uint32_t length=0;   // the list's length as GXEndDisplayList returned it
    struct ArrayBinding { size_t attribute; gx::Array array; };
    struct Child { uint32_t address, length; };
    struct Projection { gx::Projection matrix; };
    struct Position { uint32_t row; gx::PositionMatrix matrix; };
    struct IndexedPosition { uint16_t index; uint32_t row; };
    struct IndexedNormal { uint16_t index; uint32_t row; };
    struct BpRegister { uint32_t reg, value; };
    struct Current { uint32_t row; };
    struct MatrixIndex { uint32_t reg,value; };
    struct CopyClear { CopyState state; };
    struct TextureCopy { std::array<uint32_t,4> source; uint32_t address,width,height; bool clear; uint32_t format=6; };
    struct TextureBinding { uint32_t map; TextureSource source; };
    struct PaletteLoad { uint32_t region; PaletteSource source; };
    struct Blend { gx::BlendState state; };
    struct TevColor { uint32_t id; bool konst; std::array<float,4> value; };
    struct Light { uint32_t id; gx::LightUniform value; };
    struct Normal { uint32_t row; gx::NormalMatrix value; };
    struct NumChannels { uint32_t count; };
    struct ChannelControl { uint32_t index,control; };
    struct TextureTransform { uint32_t id; TextureMatrix matrix; uint32_t rows; };
    // A RecordedDraw is ~5 KB: held by pointer so growing the vector moves 8 bytes, not the draw.
    using Item=std::variant<std::unique_ptr<RecordedDraw>,ArrayBinding,Child,Projection,Position,IndexedPosition,IndexedNormal,BpRegister,Current,MatrixIndex,CopyClear,TextureCopy,TextureBinding,PaletteLoad,Blend,TevColor,Light,Normal,NumChannels,ChannelControl,TextureTransform>;
    std::vector<Item> geometry;
};
RecordedDraw* recorded_draw(Resource::Item& item) { auto* held=std::get_if<std::unique_ptr<RecordedDraw>>(&item); return held ? held->get() : nullptr; }
const RecordedDraw* recorded_draw(const Resource::Item& item) { auto* held=std::get_if<std::unique_ptr<RecordedDraw>>(&item); return held ? held->get() : nullptr; }
std::map<uint32_t,std::shared_ptr<const Resource>> resources; // Immutable recordings shared with rollback checkpoints.
// The game records its display lists again every frame, and a replaced recording (its draws hold ~5 KB of state each) is freed on a helper
// thread: pure host work the builder would otherwise do. The helper empties the pile every few milliseconds (no wake-up per list).
class Graveyard {
public:
    static Graveyard& instance() { static Graveyard* pile=new Graveyard; return *pile; }   // never destroyed: the helper may run at exit
    void bury(std::shared_ptr<const Resource> resource) { std::lock_guard lock(mutex_); dead_.push_back(std::move(resource)); }
private:
    Graveyard() {
        std::thread([this] {
            std::vector<std::shared_ptr<const Resource>> batch;
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
                { std::lock_guard lock(mutex_); batch.swap(dead_); }
                batch.clear();
            }
        }).detach();
    }
    std::mutex mutex_;
    std::vector<std::shared_ptr<const Resource>> dead_;
};
// The builder's side of a display list being recorded: its native items and the host state GXEndDisplayList restores.
struct BackList {
    bool restore=false;
    gx::DrawUniform color_shadow;
    TextureMatrices texture_shadow;
    MatrixIndices indices_shadow;
    Resource native;
    gx::State input_state;
};
std::unique_ptr<BackList> brec;
std::atomic<uint64_t> draw_count{0}, vertex_count{0}, submitted{0}, completed{0}, outside_bytes{0};
std::atomic<uint64_t> copies{0};
std::atomic<uint64_t> texture_decodes{0},texture_evictions{0},texture_resident{0};
uint64_t back_serial=0;   // the builder's view of the last sealed serial (checked against the front's prediction)
struct PromptMark { uint64_t token; uc::GxPromptCheckpoint at; };
std::array<PromptMark,64> prompt_marks{};   // gx_prompt_checkpoint() results as the builder saw them, by token

// ================= The front's state (guest thread) =================
uint32_t known_sdk=0;   // SDK block address last seen by a snapshot; raw display list calls write the texgen words there
std::array<uint32_t,4> texture_copy_source{};
uint32_t texture_copy_width=0,texture_copy_height=0;
uint32_t texture_copy_format=6;
std::mutex det_finish_m;
std::map<uint64_t,uint64_t> det_finishes; // serial -> scheduler generation
std::set<uint64_t> det_early;
std::map<uint64_t, uint32_t> finishes; // serial -> submitting thread's SDA base; guest baton only
uint64_t front_serial=0;   // the serial of the last packet the builder was told to seal (Packets numbers them in that order)
uint32_t front_stamp=0;    // write-watch stamp given to the commands (gx_watch_stamp.h)
uint64_t prompt_tokens=0;
SdkRef sdk_last;           // the last SDK block captured
// What a recorded display list does that the guest can observe when it is called: the channel words GXCallDisplayList writes back, the
// packets its texture copies seal, the lists it calls.
struct FrontEffect { enum Kind : uint8_t { child, num_channels, channel_control, texture_copy } kind; uint32_t a=0, b=0; };
struct FrontResource {
    std::shared_ptr<const std::vector<std::byte>> bytes;   // the recorded bytes (the first `length` are the list's)
    uint32_t length=0;
    mutable uint32_t stamp=0,skips=0;   // write-barrier stamp of the last byte comparison (gx_write_watch.h); uses since then
    std::vector<FrontEffect> effects;
};
std::map<uint32_t,std::shared_ptr<const FrontResource>> front_resources;
struct FrontList {
    uint32_t address, capacity, sdk, owner;
    bool restore, overflow = false;
    std::array<std::byte,0x600> shadow{};
    std::array<uint32_t,4> texture_copy_source_shadow;
    uint32_t texture_copy_width_shadow=0,texture_copy_height_shadow=0;
    uint32_t texture_copy_format_shadow=6;
    std::unique_ptr<std::byte[]> bytes;   // capacity bytes and 8 of slack; written through uc::gx_fifo_window while recording
    size_t used=0;                        // valid while overflowed (the window is closed then); otherwise the window's position
    bool prim_open=false;                 // an open recorded primitive: its payload ends at prim_end
    std::byte* prim_end=nullptr;
    uint32_t prim_callsite=0;
    size_t prim_expected=0;
    std::vector<FrontEffect> effects;
};
std::unique_ptr<FrontList> front_list;
// An open immediate-mode primitive: the FIFO bytes collect here (through the window) until the last one, then go to the builder.
struct FrontPrimitive {
    bool active=false;
    uint32_t producer=0, callsite=0;
    size_t expected=0;
    std::vector<std::byte> bytes;
};
FrontPrimitive front_prim;
gx::State front_derived;   // derive_state of the last block (only descriptors and formats are read: encoded sizes)
SdkRef front_derived_block;

// --profile only: wall time inside the two native draw entry points, printed once per second as "[gxtime]" (per-call figures compare builds
// far more precisely than a whole-fight FPS, which varies several percent between runs). With the builder these are the guest thread's share.
struct CallTimer {
    static inline uint64_t ns[2]={}, calls[2]={}, last=0;
    int id; bool on; std::chrono::steady_clock::time_point start;
    explicit CallTimer(int i):id(i),on(profile_mesh) { if(on) start=std::chrono::steady_clock::now(); }
    ~CallTimer() {
        if(!on) return;
        const auto now=std::chrono::steady_clock::now();
        ns[id]+=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(now-start).count()); ++calls[id];
        const uint64_t t=uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
        if(t-last>=1000) {
            if(last) std::fprintf(stderr,"[gxcache] state %llu/%llu material %llu/%llu\n",(unsigned long long)cache_stat[0],(unsigned long long)cache_stat[1],(unsigned long long)cache_stat[2],(unsigned long long)cache_stat[3]);
            if(last) { std::fprintf(stderr,"[gxmiss] material: epoch %llu bytes %llu | state: epoch %llu bytes %llu | first differing words (state):",(unsigned long long)cache_stat[4],(unsigned long long)cache_stat[5],(unsigned long long)cache_stat[6],(unsigned long long)cache_stat[7]);
                for(int w=0;w<2;++w) { for(int k=0;k<4;++k) { size_t best=0; for(size_t o=0;o<0x560/4;++o) if(cache_diff[w][o]>cache_diff[w][best]) best=o; if(cache_diff[w][best]) { std::fprintf(stderr," +%zX:%llu",best*4,(unsigned long long)cache_diff[w][best]); cache_diff[w][best]=0; } } std::fprintf(stderr,w?" (material)":" |"); }
                std::fprintf(stderr,"\n"); std::memset(cache_diff,0,sizeof cache_diff); }
            for(auto& v:cache_stat) v=0;
            if(last) std::fprintf(stderr,"[gxtime] list %.1f ms %llu calls %.2f us/call mesh %.1f ms %llu calls %.3f us/call; list draws %llu, %llu vertices, decoded %llu reused %llu; builder backlog %llu KiB\n",ns[0]/1e6,(unsigned long long)calls[0],calls[0]?ns[0]/1e3/calls[0]:0.0,ns[1]/1e6,(unsigned long long)calls[1],calls[1]?ns[1]/1e3/calls[1]:0.0,
                (unsigned long long)list_stat[0],(unsigned long long)list_stat[1],(unsigned long long)list_decodes.exchange(0),(unsigned long long)list_reuses.exchange(0),(unsigned long long)(queue.backlog()>>10));
            for(auto& v:list_stat) v=0;
            last=t; ns[0]=ns[1]=calls[0]=calls[1]=0;
        }
    }
};
uint32_t read32(uint32_t address) {
    // MEM1 through its cached/uncached mirrors (aliases 0, 8, 12) is nearly every SDK-state read: skip the generic bounds machinery.
    const uint32_t physical = address & 0x3FFFFFFF;
    if (((0x1101u >> (address >> 28)) & 1) && size_t(physical) + 4 <= memory.mem1.size()) {
        uint32_t raw; std::memcpy(&raw, memory.mem1.data() + physical, 4);
        return uc_bswap32(raw);
    }
    auto bytes = memory.read(address, 4);
    uint32_t value = 0;
    for (auto byte : bytes) value = value << 8 | std::to_integer<uint8_t>(byte);
    return value;
}

// ---------------- Front: issuing commands ----------------
[[noreturn]] void builder_failed() {
    try { queue.rethrow(); }
    catch (const gx::Error& error) { if (error.code==gx::ErrorCode::canceled) throw uc::ThreadExit{}; throw; }
}
template<class F> void back(F&& f) {
    try { queue.emit(std::forward<F>(f),front_stamp); }
    catch (const gx::Error& error) { if (error.code==gx::ErrorCode::canceled) throw uc::ThreadExit{}; throw; }
}
void reset_watch();
// A command whose cache checks compare guest memory (textures, meshes): it records a fresh stamp.
template<class F> void back_checked(F&& f) {
    if (queue.threaded()) {
        if (gx::watch_exhausted()) reset_watch();
        front_stamp=gx::watch_stamp();
    }
    back(std::forward<F>(f));
}
void drain() {
    try { queue.drain(); }
    catch (const gx::Error& error) { if (error.code==gx::ErrorCode::canceled) throw uc::ThreadExit{}; throw; }
}
uint32_t producer() { return scheduler->in_irq() ? UINT32_MAX : uint32_t(scheduler->current()->id); }
// The SDK block as a draw issued now sees it (the read also checks the whole block's bounds, as the snapshots did).
SdkRef capture_sdk(uint32_t sdk) {
    const auto live=memory.read(sdk,0x600);
    if(sdk_last && sdk_last->address==sdk && std::memcmp(live.data(),sdk_last->bytes.data(),kSdkSpan)==0) return sdk_last;
    auto block=std::allocate_shared<SdkBlock>(gx::PoolAllocator<SdkBlock>{});
    block->address=sdk; std::memcpy(block->bytes.data(),live.data(),kSdkSpan);
    sdk_last=std::move(block);
    return sdk_last;
}
// Live and recorded primitives open a window over their bytes: the inline FIFO path fills it (gx_fifo.h).
void list_window() {
    auto& list=*front_list;
    if(list.overflow) { uc::gx_fifo_window={}; return; }
    std::byte* end=list.bytes.get()+list.capacity;
    if(list.prim_open && list.prim_end<end) end=list.prim_end;
    uc::gx_fifo_window.end=end;
}
size_t list_used() { return front_list->overflow ? front_list->used : size_t(uc::gx_fifo_window.at-front_list->bytes.get()); }
bool recording_primitive() { return front_list && front_list->prim_open; }
bool live_primitive() { return front_prim.active; }
// What capture->finish() threw: a primitive whose vertices did not all arrive.
void finish_primitive() {
    if(front_prim.active) throw gx::Error(gx::ErrorCode::truncated,front_prim.callsite,front_prim.expected,size_t(uc::gx_fifo_window.at-front_prim.bytes.data()));
}

// ================= Builder: snapshots =================
TextureSelectors texture_selectors(const SdkBlock& block) {
    TextureSelectors result{};
    const auto low=matrix_indices[0].value_or(block.word(0x80)),high=matrix_indices[1].value_or(block.word(0x84));
    for(uint32_t i=0;i<8;++i) result[i]=((i<4 ? low : high)>>(i<4 ? 6+i*6 : (i-4)*6))&63;
    return result;
}
// NW4R writes XF selectors without changing the SDK cache; a later SDK flush can replace them. `push`: record it into the list being recorded.
void set_matrix_index(uint32_t reg,uint32_t value,bool push) {
    matrix_indices[reg]=value; dirty_material();
    if(brec) { if(push) brec->native.geometry.push_back(Resource::MatrixIndex{reg,value}); }
    else if(reg==0) transforms.current=value&63;
}
// Texture matrix ids follow GX: 0..61 (GX_IDENTITY 60), post matrices at 64+row (XF 0x500+row*4).
void resolve_texture_matrices(gx::DrawUniform& uniforms,const TextureSelectors& selectors,const TextureMatrices& matrices) {
    constexpr TextureMatrix identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    for(size_t i=0;i<8;++i) {
        const auto& rows=selectors[i]!=60 && matrices[selectors[i]] ? *matrices[selectors[i]] : identity;
        const auto post=64+(uniforms.tex_posts[i]&63);
        const auto& post_rows=post<matrices.size() && matrices[post] ? *matrices[post] : identity;
        for(size_t row=0;row<3;++row) { uniforms.tex_rows[i*3+row]=rows[row]; uniforms.post_rows[i*3+row]=post_rows[row]; }
    }
}
// The bank a draw with per-vertex texture matrix indices reads (null for every other draw). Draws whose matrices did not change share one.
std::shared_ptr<const gx::TextureBank> texture_bank(const gx::State& state,const TextureMatrices& matrices) {
    if(!gx::has_texture_matrix_index(state)) return nullptr;
    thread_local std::shared_ptr<const gx::TextureBank> last;
    gx::TextureBank bank;
    for(size_t id=0;id<matrices.size();++id) if(id!=60 && matrices[id]) std::copy(matrices[id]->begin(),matrices[id]->end(),bank.rows.begin()+id*3);
    if(!last || !(*last==bank)) last=std::make_shared<const gx::TextureBank>(bank);
    return last;
}
struct MaterialSnapshot { gx::TevMaterial material; gx::DrawUniform uniforms; };
// A snapshot is a pure function of the SDK block and the host-side state versioned above, so the last result is reused while neither changed:
// consecutive meshes of one model draw with identical GX state (the front hands the same block on while the guest leaves it alone).
struct MaterialCache { bool valid=false; uint64_t epoch=0; SdkRef block; MaterialSnapshot result; };
MaterialCache material_cache;
// UC_GX_CACHE_VERIFY=1 recomputes every cached snapshot and reports results that differ (debugging aid for a missed version bump).
const bool cache_verify=std::getenv("UC_GX_CACHE_VERIFY")!=nullptr;
uint32_t cache_mismatches=0;
// The front hands the same block on while the guest leaves the bytes alone (capture_sdk), so identity is byte equality.
bool same_block(const SdkRef& a,const SdkRef& b) { return a==b; }
MaterialSnapshot material_snapshot(const SdkRef& sdk_block,uint32_t lr,bool use_cache=true) {
    const SdkBlock& block=*sdk_block;
    if(use_cache && material_cache.valid && material_cache.epoch==material_epoch && same_block(material_cache.block,sdk_block)) {
        if(cache_verify) {
            const auto cached=material_cache.result; const auto fresh=material_snapshot(sdk_block,lr,false);
            if((std::memcmp(&cached.material,&fresh.material,sizeof cached.material) || std::memcmp(&cached.uniforms,&fresh.uniforms,sizeof cached.uniforms)) && cache_mismatches++<8)
                std::fprintf(stderr,"[gx-cache] material snapshot differs at callsite %08X (epoch %llu)\n",lr,(unsigned long long)material_epoch);
        }
        ++cache_stat[2]; return material_cache.result;
    }
    if(profile_mesh && material_cache.valid && material_cache.block && material_cache.block->address==block.address) {
        if(material_cache.epoch!=material_epoch) ++cache_stat[4];
        else for(size_t o=0;o<kSdkSpan;o+=4) if(std::memcmp(block.bytes.data()+o,material_cache.block->bytes.data()+o,4)) { ++cache_diff[1][o/4]; ++cache_stat[5]; break; }
    }
    ++cache_stat[3]; material_cache.valid=false;
    const auto selectors=texture_selectors(block);
    MaterialSnapshot result{gx::TevMaterial{},color_uniforms};
    auto& material=result.material; auto& uniforms=result.uniforms;
    auto word=[&](uint32_t offset) { return block.word(offset); };
    material.stages=((word(0x254)>>10)&15)+1;
    uniforms.pixel[1]=word(0x228)&31;
    uniforms.pixel[2]=word(0x224)&511;
    uniforms.pixel[3]=(word(0x254)>>4)&3;
    for(uint32_t i=0;i<2;++i) for(uint32_t lane=0;lane<4;++lane) {
        uniforms.materials[i][lane]=((word(0xB0+i*4)>>(24-lane*8))&255)/255.f;
        uniforms.ambient[i][lane]=((word(0xA8+i*4)>>(24-lane*8))&255)/255.f;
    }
    for(uint32_t i=0;i<4;++i) uniforms.channels[i]=word(0xB8+i*4);
    for(size_t i=0;i<4;++i) material.swaps[i]=uint8_t((word(0x200+uint32_t(i)*8)&15)|((word(0x204+uint32_t(i)*8)&15)<<4));
    for(uint32_t i=0;i<material.stages;++i) {
        auto& stage=material.stage[i];
        stage.color=word(0x180+i*4)&0xFFFFFF; stage.alpha=word(0x1C0+i*4)&0xFFFFFF;
        stage.order=(word(0x150+i/2*4)>>((i%2)*12))&0xFFF;
        stage.konst=(word(0x200+i/2*4)>>(4+(i%2)*10))&1023;
        stage.indirect=indirect_stages[i];
        uniforms.ind_cmd[i]=stage.indirect;
    }
    for(uint32_t i=0;i<8;++i) {
        const auto generator=word(0xC8+i*4),source=(generator>>7)&31;
        uniforms.tex_gens[i]=generator; uniforms.tex_posts[i]=word(0xE8+i*4); // dual-texture post is always on (XF 0x1012 set once by __GXInitRevisionBits)
        static int unsupported_logs=0;
        if(((generator>>4)&7 || source==2 || source==3 || source==4 || source>12) && unsupported_logs<8 && ++unsupported_logs)
            std::fprintf(stderr,"[native-texgen] unsupported generator %08X slot %u callsite %08X\n",generator,i,lr);
        const auto id=selectors[i];
        if(profile_mesh && texgen_profile_count<16 && (source<5 || (generator&2))) {
            const bool used=std::any_of(material.stage.begin(),material.stage.begin()+material.stages,[&](const auto& stage){return (stage.order&64) && ((stage.order>>3)&7)==i;});
            if(used) { ++texgen_profile_count; std::fprintf(stderr,"[native-texgen] callsite %08X slot %u generator %08X matrix %u post %08X\n",lr,i,generator,id,word(0xE8+i*4)); }
        }
    }
    resolve_texture_matrices(uniforms,selectors,texture_matrices);
    material_cache.block=sdk_block; material_cache.epoch=material_epoch; material_cache.result=result; material_cache.valid=true;
    return result;
}
uint32_t texture_mask(const gx::TevMaterial& material,const gx::DrawUniform& uniforms) {
    uint32_t mask=1;
    for(uint32_t i=0;i<material.stages;++i) {
        const auto& stage=material.stage[i];
        if(!(stage.order&64)) continue;
        mask|=1u<<(stage.order&7);
        if(uniforms.ind_cmd[i]) mask|=1u<<((uniforms.indirect[9]>>((uniforms.ind_cmd[i]&3)*6))&7);
    }
    return mask;
}
// Position and texture loads share XF rows; selectors may start inside another loaded matrix.
void matrix_rows(TextureMatrices& matrices,gx::Transforms* positions,uint32_t id,const TextureMatrix& matrix,uint32_t rows) {
    constexpr TextureMatrix identity{{{1,0,0,0},{0,1,0,0},{0,0,1,0}}};
    const uint32_t first=id>=64 ? 64 : 0,last=id>=64 ? 125 : 63;
    for(uint32_t base=std::max(first,id>1 ? id-2 : 0u);base<=std::min(last,id+rows-1);++base) {
        auto& target=matrices[base];
        if(!target) target=identity;
        for(uint32_t row=std::max(base,id);row<std::min(base+3,id+rows);++row) (*target)[row-base]=matrix[row-id];
    }
    if(positions && id<64) for(uint32_t row=id;row<std::min(64u,id+rows);++row) {
        positions->position_rows[row]=matrix[row-id]; positions->loaded_rows.set(row);
    }
}
gx::PositionMatrix position_matrix(uint32_t address) {   // front (immediate loads) and builder (indexed loads)
    memory.read(address,48);
    gx::PositionMatrix result{};
    for (size_t r=0;r<3;++r) for (size_t col=0;col<4;++col) result[r][col]=std::bit_cast<float>(read32(address+uint32_t(r*16+col*4)));
    static std::atomic<unsigned> reported{0};
    if (reported.load(std::memory_order_relaxed)<8 && std::any_of(result.begin(),result.end(),[](const auto& row){ return !std::isfinite(row[0]) || !std::isfinite(row[1]) || !std::isfinite(row[2]) || !std::isfinite(row[3]); })) {
        ++reported; std::fprintf(stderr,"[gx] guest loaded a non-finite position matrix from %08X (the hardware drops the vertices it transforms)\n",address);
    }
    return result;
}
void indexed_position(gx::Transforms& target,TextureMatrices& matrices,const gx::State& bindings,uint16_t index,uint32_t row) {
    const auto& array=bindings.arrays[12];
    const uint64_t address=uint64_t(array.address)+uint64_t(index)*array.stride;
    if (!array.address || !array.stride || address>UINT32_MAX) throw gx::Error(gx::ErrorCode::array_bounds,array.address,48,array.stride);
    const auto matrix=position_matrix(uint32_t(address));
    gx::set_position_matrix(target,row,matrix);
    matrix_rows(matrices,nullptr,row,matrix,3);
}
// GX_NRM_MTX_ARRAY entry: nine consecutive floats, as the hardware's indexed XF load reads them.
void indexed_normal(gx::Transforms& target,const gx::State& bindings,uint16_t index,uint32_t row) {
    const auto& array=bindings.arrays[13];
    const uint64_t source=uint64_t(array.address)+uint64_t(index)*array.stride;
    if(!array.address || !array.stride || source>UINT32_MAX) throw gx::Error(gx::ErrorCode::array_bounds,array.address,36,array.stride);
    memory.read(uint32_t(source),36);
    gx::NormalMatrix matrix{};
    for(uint32_t i=0;i<9;++i) matrix[i/3][i%3]=std::bit_cast<float>(read32(uint32_t(source)+i*4));
    gx::set_normal_matrix(target,row,matrix);
}
// GX_TEXMTX_ARRAY entry (arrays[14]): a 3x4 or 2x4 matrix of consecutive floats, as the hardware's indexed XF load reads it.
void indexed_texture(TextureMatrices& target,gx::Transforms& positions,const gx::State& bindings,uint16_t index,uint32_t id,uint32_t words) {
    const auto& array=bindings.arrays[14];
    const uint64_t source=uint64_t(array.address)+uint64_t(index)*array.stride;
    if(!array.address || !array.stride || source>UINT32_MAX) throw gx::Error(gx::ErrorCode::array_bounds,array.address,words*4,array.stride);
    memory.read(uint32_t(source),words*4);
    TextureMatrix matrix{{{},{},{0,0,1,0}}};
    for(uint32_t i=0;i<words;++i) matrix[i/4][i%4]=std::bit_cast<float>(read32(uint32_t(source)+i*4));
    matrix_rows(target,&positions,id,matrix,words/4);
}
bool apply_bp(gx::DrawUniform& target,uint32_t reg,uint32_t data) {
    if(reg>=0xEE && reg<=0xF2) target.fog[reg-0xEE]=data;
    else if(reg>=0x06 && reg<=0x0E) target.indirect[reg-0x06]=data;
    else if(reg==0x27) target.indirect[9]=data;
    else if(reg==0x25 || reg==0x26) target.indirect[10+reg-0x25]=data;
    else if(reg>=0x10 && reg<=0x1F) target.ind_cmd[reg-0x10]=data;
    else return false;
    return true;
}
// BP state set by a replaced SDK call: emit the word the SDK writes, then apply it natively (into a recording too, as the SDK does).
void native_bp(uint32_t word) {
    uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word);
    const uint32_t reg=word>>24,data=word&0xFFFFFF;
    if((reg>=0xEE && reg<=0xF2) || (reg>=0x06 && reg<=0x0E) || reg==0x27 || reg==0x25 || reg==0x26 || (reg>=0x10 && reg<=0x1F)) {   // the registers apply_bp keeps
        const bool push=front_list && !front_list->overflow;
        back([reg,data,push] {
            apply_bp(color_uniforms,reg,data);
            dirty_material();
            if(reg>=0x10 && reg<=0x1F) indirect_stages[reg-0x10]=data;
            if(brec && push) brec->native.geometry.push_back(Resource::BpRegister{reg,data});
        });
    }
}
// The part of a snapshot that the SDK block decides: culling, viewport, scissor, vertex descriptors and attribute formats. The vertex arrays (and the
// rest of the host state) are not part of it; they are applied fresh on every call.
void derive_state(const SdkBlock& block, gx::State& result) {
    auto word=[&](uint32_t offset) { return block.word(offset); };
    uint32_t low = word(0x14), high = word(0x18);
    result.cull=uint8_t((word(0x254)>>14)&3);
    std::array<float,6> viewport{};
    for(uint32_t i=0;i<6;++i) viewport[i]=std::bit_cast<float>(word(0x544+i*4));
    if(viewport[2]!=0 || viewport[3]!=0) {
        for(auto value:viewport) if(!std::isfinite(value)) throw std::invalid_argument("nonfinite native viewport");
        if(viewport[2]<=0 || viewport[3]<=0 || viewport[4]<0 || viewport[4]>1 || viewport[5]<0 || viewport[5]>1) throw std::invalid_argument("invalid native viewport extent/depth");
        result.viewport=viewport;
    }
    const auto top=word(0x148),bottom=word(0x14C);
    if((top>>24)==0x20 && (bottom>>24)==0x21) {
        const auto left=int32_t((top>>12)&2047)-342,upper=int32_t(top&2047)-342;
        const auto right=int32_t((bottom>>12)&2047)-342,lower=int32_t(bottom&2047)-342;
        if(right<left || lower<upper) throw std::invalid_argument("invalid native scissor bounds");
        result.scissor=std::array<int32_t,4>{left,upper,right-left+1,lower-upper+1};
    }
    for (size_t i = 0; i < 9; ++i) result.descriptors[i] = low & (1u << i) ? gx::Mode::direct : gx::Mode::none;
    for (size_t i = 0; i < 4; ++i) result.descriptors[9 + i] = gx::Mode((low >> (9 + i * 2)) & 3);
    for (size_t i = 0; i < 8; ++i) result.descriptors[13 + i] = gx::Mode((high >> (i * 2)) & 3);
    for (size_t vat = 0; vat < 8; ++vat) {
        uint32_t a = word(0x1C + uint32_t(vat * 4));
        uint32_t b = word(0x3C + uint32_t(vat * 4));
        uint32_t c = word(0x5C + uint32_t(vat * 4));
        auto& formats = result.formats[vat];
        formats[9] = {gx::Type((a >> 1) & 7), uint8_t(2 + (a & 1)), uint8_t((a >> 4) & 31)};
        formats[10] = {gx::Type((a >> 10) & 7), uint8_t((a & 512) ? 9 : 3), 0, gx::Color::rgba8, bool(a >> 31)};
        formats[11].color = gx::Color((a >> 14) & 7); formats[12].color = gx::Color((a >> 18) & 7);
        auto texture = [&](size_t attr, uint32_t word, unsigned bit, uint32_t fraction) {
            formats[attr] = {gx::Type((word >> (bit + 1)) & 7), uint8_t(1 + ((word >> bit) & 1)), uint8_t(fraction & 31)};
        };
        texture(13, a, 21, a >> 25); texture(14, b, 0, b >> 4); texture(15, b, 9, b >> 13);
        texture(16, b, 18, b >> 22); texture(17, b, 27, c); texture(18, c, 5, c >> 9);
        texture(19, c, 14, c >> 18); texture(20, c, 23, c >> 27);
    }
}
// Exactly the fields derive_state sets, written over the host state (the entries it leaves alone keep the host values).
void apply_derived(gx::State& result, const gx::State& d) {
    result.cull = d.cull;
    if (d.viewport) result.viewport = d.viewport;
    if (d.scissor) result.scissor = d.scissor;
    result.descriptors = d.descriptors;
    for (size_t vat = 0; vat < 8; ++vat) {
        auto& to = result.formats[vat]; const auto& from = d.formats[vat];
        to[9] = from[9]; to[10] = from[10]; to[11].color = from[11].color; to[12].color = from[12].color;
        for (size_t attr = 13; attr < 21; ++attr) to[attr] = from[attr];
    }
}
// derive_state's inputs in the block: only these invalidate a derived state (dirty flags, textures and matrices share the SDK block).
bool same_derive_inputs(const SdkBlock& a,const SdkBlock& b) {
    return a.address==b.address && std::memcmp(a.bytes.data()+0x14,b.bytes.data()+0x14,0x68)==0 && std::memcmp(a.bytes.data()+0x148,b.bytes.data()+0x148,8)==0
        && std::memcmp(a.bytes.data()+0x254,b.bytes.data()+0x254,4)==0 && std::memcmp(a.bytes.data()+0x544,b.bytes.data()+0x544,24)==0;
}
struct StateCache { bool valid=false; SdkRef block; gx::State derived; };
StateCache state_cache;
gx::State snapshot(const SdkRef& sdk_block,uint32_t lr) {
    if (state_cache.valid && (state_cache.block==sdk_block || same_derive_inputs(*state_cache.block,*sdk_block))) ++cache_stat[0];
    else {
        ++cache_stat[1]; state_cache.valid = false;
        gx::State fresh;
        derive_state(*sdk_block, fresh);
        state_cache.derived = fresh; state_cache.block = sdk_block; state_cache.valid = true;
    }
    gx::State result = brec ? brec->input_state : state;
    apply_derived(result, state_cache.derived);
    if (cache_verify) {
        gx::State reference = brec ? brec->input_state : state;
        gx::State check; derive_state(*sdk_block, check); apply_derived(reference, check);
        if (!(reference.cull == result.cull && reference.descriptors == result.descriptors && reference.formats == result.formats && reference.viewport == result.viewport && reference.scissor == result.scissor && reference.arrays == result.arrays) && cache_mismatches++ < 8)
            std::fprintf(stderr, "[gx-cache] state snapshot differs at callsite %08X\n", lr);
    }
    return result;
}
// Front: the descriptors and formats a draw issued now has (what its encoded size depends on), and derive_state's errors at the same call.
// The host part of a snapshot (vertex arrays, the formats derive_state leaves alone) never changes them: those formats keep their defaults.
const gx::State& front_state(const SdkRef& block) {
    if (!front_derived_block || (front_derived_block!=block && !same_derive_inputs(*front_derived_block,*block))) {
        gx::State fresh;
        derive_state(*block, fresh);
        gx::State result; apply_derived(result, fresh);
        front_derived = result; front_derived_block = block;
    } else front_derived_block = block;
    return front_derived;
}
// Front: a display list's recorded bytes, compared against guest memory only when a store touched the pages since the last comparison, and
// anyway every 256th use.
const FrontResource& checked_resource(uint32_t address,uint32_t length) {
    const auto bytes=memory.read(address,length);
    auto found=front_resources.find(address & 0x3FFFFFFF);
    const bool known=found!=front_resources.end() && found->second->length==length;
    const bool clean=known && found->second->skips<256 && !gx::written_since(address,length,found->second->stamp);
    if (known && !clean && gx::same_bytes(bytes,std::span<const std::byte>(found->second->bytes->data(),length))) { found->second->stamp=gx::watch_stamp(); found->second->skips=0; }
    else if (clean) ++found->second->skips;
    else {
        std::fprintf(stderr,"[native-resource-missing] address %08X length %u prefix",address,length);
        for (auto byte:bytes.first(std::min(bytes.size(),size_t(256)))) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n");
        throw gx::Error(gx::ErrorCode::layout,address,length,0); // TD-014: prebuilt/modified lists require native import
    }
    return *found->second;
}
// Builder: the recording the front checked for this call.
const Resource& back_resource(uint32_t address,uint32_t length) {
    const auto found=resources.find(address & 0x3FFFFFFF);
    if (found==resources.end() || found->second->length!=length) throw gx::Error(gx::ErrorCode::layout,address,length,1);   // the front checked it: the two sides disagree
    return *found->second;
}
void check_palette(const PaletteSource& source) {
    if (!source.entries || source.entries>16384 || uint32_t(source.format)>2)
        throw gx::TextureError(gx::TextureErrorCode::descriptor,"invalid native palette descriptor");
}
std::shared_ptr<const PaletteData> load_palette(const PaletteSource& source) {
    check_palette(source);
    auto bytes=memory.read(source.address,source.entries*2);
    return std::make_shared<const PaletteData>(PaletteData{source.format,{bytes.begin(),bytes.end()}});
}
gx::TextureCache texture_cache;
std::shared_ptr<const gx::TexturePixels> make_copy_texture(uint32_t address,uint32_t width,uint32_t height,uint32_t format) {
    memory.read(address,gx::encoded_texture_size({width,height,format==0x27 ? gx::TextureFormat::i8 : gx::TextureFormat(format),1}));
    auto target=std::make_shared<gx::TexturePixels>(); target->gpu_only=true; target->guest_address=address&0x3FFFFFFF;
    target->channels=format==0x27 ? gx::TextureChannels::alpha_intensity : format==4 ? gx::TextureChannels::rgb565 : gx::TextureChannels::rgba;
    for(;;) {
        target->levels.push_back({width,height,{}});
        if(width==1 && height==1) break;
        width=std::max(1u,width/2); height=std::max(1u,height/2);
    }
    return target;
}
std::shared_ptr<const gx::TexturePixels> snapshot_texture(const std::optional<TextureSource>& source,const Palettes& loaded,const CopiedTextures& versions=copied_textures) {
    if (!source) return {};
    if(auto found=versions.find(source->address&0x3FFFFFFF);found!=versions.end() && found->second) {
        const auto& level=found->second->levels[0];
        const auto expected=found->second->channels==gx::TextureChannels::alpha_intensity ? gx::TextureFormat::i8 : found->second->channels==gx::TextureChannels::rgb565 ? gx::TextureFormat::rgb565 : gx::TextureFormat::rgba8;
        if(source->desc.format!=expected || source->palette || source->desc.width>level.width || source->desc.height>level.height) throw std::invalid_argument("unsupported native EFB texture view");
        // shortcut: cropped GX views share the GPU copy; exact sampler/view coordinates remain TD-045.
        return found->second;
    }
    const auto bytes=memory.read(source->address,gx::encoded_texture_size(source->desc));
    gx::PaletteView palette;
    if (source->palette) {
        auto found=loaded.find(*source->palette);
        if (found==loaded.end()) throw gx::TextureError(gx::TextureErrorCode::descriptor,"GX texture uses an unloaded palette");
        palette={found->second->format,found->second->bytes};
    }
    auto result=texture_cache.snapshot(source->address,source->desc,bytes,palette);
    texture_decodes=texture_cache.decodes(); texture_evictions=texture_cache.evictions(); texture_resident=texture_cache.resident_bytes();
    return result;
}
// The front's view of the state the reticle and prompt handling of a sealed packet reads (the pointer the game consumed when it drew it).
struct SealInputs {
    std::array<float,2> aim_consumed{};
    std::array<std::optional<std::array<float,2>>,2> consumed{};
    unsigned channel=0;
    bool exact=false;
    gx::ReticleStyle style;
};
SealInputs seal_inputs() {
    SealInputs inputs;
    inputs.aim_consumed=uc::input_consumed_aim();
    inputs.consumed={uc::input_consumed_aim_of(0),uc::input_consumed_aim_of(1)};
    inputs.channel=uc::input_aim_channel();
    inputs.exact=uc::input_aim_exact() || !uc::input_demo_playing();
    inputs.style=gx::reticle_style();
    return inputs;
}
bool decoded_still_valid(const RecordedDraw& draw,const gx::State& layout) {
    const auto* cached=draw.decoded.get();
    if(!cached || cached->uses>=256) return false;
    for(size_t attr=9;attr<21;++attr) {
        const auto mode=layout.descriptors[attr];
        if((mode==gx::Mode::index8 || mode==gx::Mode::index16) && !(cached->arrays[attr-9]==layout.arrays[attr-9])) return false;
    }
    for(const auto& range:cached->sources) if(range && gx::written_since(range->address,range->size,cached->stamp)) return false;
    return true;
}
// A new recording of the list at the same address takes over the decodes of the earlier recording's draws that decode the same.
void inherit_decodes(Resource& fresh,const Resource& previous) {
    size_t at=0;
    for(auto& item:fresh.geometry) {
        auto* draw=recorded_draw(item);
        if(!draw) continue;
        while(at<previous.geometry.size() && !recorded_draw(previous.geometry[at])) ++at;
        if(at==previous.geometry.size()) return;
        const auto& old=*recorded_draw(previous.geometry[at++]);
        if(!old.decoded || old.primitive!=draw->primitive || old.vat!=draw->vat || old.count!=draw->count || old.expected!=draw->expected) continue;
        if(old.state.descriptors!=draw->state.descriptors || !(old.state.formats[draw->vat]==draw->state.formats[draw->vat])) continue;
        if(std::memcmp(previous.list_bytes->data()+old.offset,fresh.list_bytes->data()+draw->offset,draw->expected)!=0) continue;
        draw->decoded=old.decoded;
    }
}
void import_resource(uint32_t address,uint32_t length,gx::State& bindings,gx::Transforms& transform_state,CopyState& copy,TextureBindings& textures,Palettes& loaded,gx::BlendState& blend,gx::DrawUniform& colors,TextureMatrices& matrices,MatrixIndices& indices,Imported& imported,CopiedTextures& versions,std::vector<uint32_t>& path) {
    const uint32_t physical=address & 0x3FFFFFFF;
    // shortcut: nesting capped at64; replace traversal if a verified deeper native asset appears (TD-032).
    if (path.size()==64 || std::find(path.begin(),path.end(),physical)!=path.end())
        throw gx::Error(gx::ErrorCode::sequence,address,64,path.size());
    const auto& resource=back_resource(address,length);
    path.push_back(physical);
    for (const auto& item : resource.geometry) {
        if (const auto* source=recorded_draw(item)) {
            // shortcut: decode attributes per call; cache native mesh revisions after profiling (TD-032).
            auto layout=source->state;
            layout.arrays=bindings.arrays;
            const auto& shared_layout=gx::share(import_state,layout);
            ++list_stat[0]; list_stat[1]+=source->count;
            gx::DrawPacket draw;
            if(!replaying && decoded_still_valid(*source,layout)) {
                auto& cached=*source->decoded; ++cached.uses; add_guest_stat(list_reuses,1);
                draw.callsite=source->callsite; draw.primitive=source->primitive; draw.vat=source->vat; draw.state=shared_layout;
                draw.vertices=cached.vertices; draw.indices=cached.indices;
                if(cache_verify) {   // UC_GX_CACHE_VERIFY: the reused decode must equal a fresh one
                    gx::Vertices fresh(memory);
                    fresh.begin(shared_layout,source->primitive,source->vat,source->count,0,source->callsite);
                    fresh.write_attribute(0,std::span<const std::byte>(resource.list_bytes->data()+source->offset,source->expected));
                    const auto check=fresh.take_draw();
                    bool same=check.indices==draw.indices && check.vertices.size()==draw.vertices.size();
                    for(size_t i=0;same && i<check.vertices.size();++i) {
                        const auto& a=check.vertices[i]; const auto& b=draw.vertices[i];
                        same=!std::memcmp(&a.position,&b.position,sizeof a.position) && !std::memcmp(&a.normal,&b.normal,sizeof a.normal) && a.color==b.color
                            && !std::memcmp(&a.texcoord,&b.texcoord,sizeof a.texcoord) && a.position_matrix==b.position_matrix && a.texture_matrix==b.texture_matrix;
                    }
                    if(!same && cache_mismatches++<8) std::fprintf(stderr,"[gx-cache] reused display-list decode differs at callsite %08X list %08X\n",source->callsite,address);
                }
            } else {
                gx::Vertices vertices(memory,!replaying);
                vertices.begin(shared_layout,source->primitive,source->vat,source->count,0,source->callsite,replaying);
                vertices.write_attribute(0,std::span<const std::byte>(resource.list_bytes->data()+source->offset,source->expected));
                draw=vertices.take_draw();
                // Replay retains geometry validation and GX state commands, but publishes no graphics.
                if(replaying) { add_guest_stat(vertex_count,vertices.vertex_count()); add_guest_stat(draw_count,1); continue; }
                auto cached=std::make_shared<DecodedDraw>();
                std::copy(layout.arrays.begin(),layout.arrays.begin()+12,cached->arrays.begin());
                cached->sources=vertices.sources(); cached->stamp=gx::verified_stamp();
                cached->vertices=draw.vertices; cached->indices=draw.indices;
                source->decoded=std::move(cached); add_guest_stat(list_decodes,1);
            }
            draw.transforms=gx::share(import_transforms,transform_state);
            draw.texture=snapshot_texture(textures[0],loaded,versions);
            const auto maps=texture_mask(source->material,source->uniforms);
            for(uint32_t map=1;map<8;++map) if(maps&(1u<<map)) {
                draw.texture_maps[map-1]=snapshot_texture(textures[map],loaded,versions);
            }
            draw.material=source->material;
            auto uniforms=source->uniforms;
            uniforms.registers=colors.registers; uniforms.konst=colors.konst;
            uniforms.lights=colors.lights; uniforms.materials=colors.materials;
            uniforms.ambient=colors.ambient; uniforms.channels=colors.channels;
            uniforms.pixel[3]=colors.pixel[3];
            auto selectors=source->texture_selectors;
            for(uint32_t i=0;i<8;++i) if(indices[i/4])
                selectors[i]=(*indices[i/4]>>(i<4 ? 6+i*6 : (i-4)*6))&63;
            resolve_texture_matrices(uniforms,selectors,matrices);
            draw.uniforms=gx::share(import_uniforms,uniforms);
            draw.texture_bank=texture_bank(*draw.state,matrices);
            draw.texture_wrap=wraps(textures);
            draw.blend=blend;
            if(source->prompt)if(auto prompt=uc::prompt_from_quad(draw,*source->prompt)) {
                if(profile_mesh && source->compact && prompt_color_logs++<12)std::fprintf(stderr,"[prompt-color] action %u pos %.2f %.2f rgba %u %u %u %u material %.2f %.2f %.2f channel %08X\n",unsigned(source->prompt->action),prompt->efb_corners[0],prompt->efb_corners[1],draw.vertices[0].color[0][0],draw.vertices[0].color[0][1],draw.vertices[0].color[0][2],draw.vertices[0].color[0][3],draw.uniforms->materials[0][0],draw.uniforms->materials[0][1],draw.uniforms->materials[0][2],draw.uniforms->channels[0]);
                prompt->compact=source->compact;draw.native_prompt=std::move(*prompt);++prompt_annotations;
            }
            imported.push_back(std::move(draw));
        } else if (const auto* array=std::get_if<Resource::ArrayBinding>(&item)) bindings.arrays[array->attribute]=array->array;
        else if (const auto* projection=std::get_if<Resource::Projection>(&item)) transform_state.projection=projection->matrix;
        else if (const auto* matrix=std::get_if<Resource::Position>(&item)) matrix_rows(matrices,&transform_state,matrix->row,matrix->matrix,3);
        else if (const auto* matrix=std::get_if<Resource::IndexedPosition>(&item)) indexed_position(transform_state,matrices,bindings,matrix->index,matrix->row);
        else if (const auto* matrix=std::get_if<Resource::IndexedNormal>(&item)) indexed_normal(transform_state,bindings,matrix->index,matrix->row);
        else if (const auto* bp=std::get_if<Resource::BpRegister>(&item)) apply_bp(colors,bp->reg,bp->value);
        else if (const auto* current=std::get_if<Resource::Current>(&item)) transform_state.current=current->row;
        else if (const auto* index=std::get_if<Resource::MatrixIndex>(&item)) { indices[index->reg]=index->value; if(index->reg==0) transform_state.current=index->value&63; }
        else if (const auto* clear=std::get_if<Resource::CopyClear>(&item)) copy=clear->state;
        else if (const auto* command=std::get_if<Resource::TextureCopy>(&item)) {
            auto target=make_copy_texture(command->address,command->width,command->height,command->format);
            versions[command->address&0x3FFFFFFF]=target;
            imported.push_back(CopiedTexture{{command->source,std::move(target)},command->clear,copy});
        }
        else if (const auto* binding=std::get_if<Resource::TextureBinding>(&item)) textures[binding->map]=binding->source;
        else if (const auto* palette=std::get_if<Resource::PaletteLoad>(&item)) loaded[palette->region]=load_palette(palette->source);
        else if (const auto* value=std::get_if<Resource::Blend>(&item)) blend=value->state;
        else if (const auto* value=std::get_if<Resource::TevColor>(&item)) {
            if(value->konst) colors.konst[value->id]=value->value;
            else colors.registers[value->id]=value->value;
        }
        else if (const auto* value=std::get_if<Resource::Light>(&item)) colors.lights[value->id]=value->value;
        else if (const auto* value=std::get_if<Resource::Normal>(&item)) gx::set_normal_matrix(transform_state,value->row,value->value);
        else if (const auto* value=std::get_if<Resource::NumChannels>(&item)) colors.pixel[3]=value->count;
        else if (const auto* value=std::get_if<Resource::ChannelControl>(&item)) colors.channels[value->index]=value->control;
        else if (const auto* value=std::get_if<Resource::TextureTransform>(&item)) matrix_rows(matrices,&transform_state,value->id,value->matrix,value->rows);
        else {
            const auto& child=std::get<Resource::Child>(item);
            import_resource(child.address,child.length,bindings,transform_state,copy,textures,loaded,blend,colors,matrices,indices,imported,versions,path);
        }
    }
    path.pop_back();
}
// UC_GX_PACKET_HASH=1 (diagnostics): one "[gx-hash]" line per sealed packet, a hash of what the renderer receives (draw geometry, state,
// transforms, uniforms, material, textures by size, the packet's own fields; =2 adds the decoded texels). A deterministic replay prints the
// same lines with the builder thread and without it (UC_GX_SYNC=1) unless the builder read guest memory the guest had changed meanwhile.
const int packet_hash=[] { const char* value=std::getenv("UC_GX_PACKET_HASH"); return value ? std::atoi(value) : 0; }();
struct Fnv {
    uint64_t h=1469598103934665603ull;
    void bytes(const void* data,size_t size) { auto* p=static_cast<const uint8_t*>(data); for(size_t i=0;i<size;++i) { h^=p[i]; h*=1099511628211ull; } }
    template<class T> void value(const T& v) { static_assert(std::is_trivially_copyable_v<T>); bytes(&v,sizeof v); }
};
void hash_packet(const gx::FramePacket& frame,uint64_t serial) {
    Fnv f;
    f.value(frame.present); f.value(frame.clear_after_copy); f.value(frame.clear_color); f.value(frame.clear_depth); f.value(frame.xfb);
    f.value(frame.display_source); f.value(frame.black_output); f.value(frame.replay); f.value(frame.prompts.size());
    if(frame.texture_copy) { f.value(frame.texture_copy->source); f.value(frame.texture_copy->target->guest_address); }
    auto texture=[&](const std::shared_ptr<const gx::TexturePixels>& pixels) {
        if(!pixels) { f.value(0); return; }
        f.value(pixels->gpu_only); f.value(pixels->guest_address); f.value(pixels->desc_width); f.value(pixels->desc_height);
        if(packet_hash>1 && !pixels->gpu_only) { pixels->wait(); for(const auto& level:pixels->levels) { f.value(level.width); f.value(level.height); f.bytes(level.rgba.data(),level.rgba.size()); } }
    };
    for(const auto& draw:frame.draws) {
        f.value(draw.callsite); f.value(draw.primitive); f.value(draw.vat);
        const auto& s=*draw.state;
        f.value(s.cull); f.value(s.descriptors); f.value(s.formats);
        for(const auto& array:s.arrays) { f.value(array.address); f.value(array.stride); }
        f.value(s.viewport.has_value()); if(s.viewport) f.value(*s.viewport);
        f.value(s.scissor.has_value()); if(s.scissor) f.value(*s.scissor);
        for(const auto& v:draw.vertex_view()) { f.value(v.position); f.value(v.normal); f.value(v.color); f.value(v.texcoord); f.value(v.position_matrix); f.value(v.texture_matrix); }
        const auto indices=draw.index_view(); f.bytes(indices.data(),indices.size_bytes());
        const auto& t=*draw.transforms;
        f.value(t.projection); f.value(t.position_rows); f.value(t.loaded_rows.to_ullong()); f.value(t.current); f.value(t.normal_rows); f.value(t.loaded_normal_rows.to_ullong());
        f.value(*draw.uniforms); f.value(draw.material); f.value(draw.blend); f.value(draw.texture_wrap);
        if(draw.texture_bank) f.value(draw.texture_bank->rows);
        texture(draw.texture); for(const auto& map:draw.texture_maps) texture(map);
        f.value(draw.reticle); f.value(draw.late_aim); f.value(draw.layer); f.value(draw.wake);
    }
    std::fprintf(stderr,"[gx-hash] serial %llu draws %zu hash %016llX\n",(unsigned long long)serial,frame.draws.size(),(unsigned long long)f.h);
}
// Builder: seals the draws so far into a packet (the front predicted its serial).
uint64_t submit_batch(const SealInputs& inputs,bool present,bool clear,uint32_t xfb=0,std::optional<gx::TextureCopy> texture_copy={},std::array<uint32_t,4> display_source={0,0,640,480},bool black=false) {
    if (!packets) return 0;
    gx::FramePacket frame{0,std::move(draws),present,clear,copy_state.color,copy_state.depth,xfb,std::move(texture_copy),display_source};
    frame.black_output=black;
    frame.replay=replaying;
    frame.aim_consumed=inputs.aim_consumed;
    gx::mark_reticle_draws(frame.draws,inputs.consumed,inputs.channel,inputs.exact);
    gx::apply_reticle_style(frame.draws,inputs.style,inputs.channel,inputs.consumed[1].has_value());   // the menu's Reticle tab
    for(auto& draw:frame.draws) {
        if(draw.late_aim) continue;
        if(profile_mesh && gx::reticle_callsite(draw.callsite) && draw.texture && !draw.texture->levels.empty()) {   // another weapon's reticle texture would show up here
            static std::set<uint64_t> reported; const auto& level=draw.texture->levels[0];
            if(reported.insert(uint64_t(draw.callsite)<<32|level.width<<16|level.height).second) std::fprintf(stderr,"[late-aim] reticle callsite %08X draws a %ux%u texture that is not shifted\n",draw.callsite,level.width,level.height);
        }
    }
    if(frame.texture_copy && clear) {
        // The copy erases the EFB: prompts drawn so far leave with it, originals stay in the copied texture.
        carried_prompts.clear();
        for(const auto& draw:frame.draws)if(draw.native_prompt)++prompt_fallback;
    } else {
        // EFB content persists across DrawDone/non-clearing copies until the display copy presents it.
        for(auto& draw:frame.draws)if(draw.native_prompt)carried_prompts.push_back(std::move(*draw.native_prompt));
        std::erase_if(frame.draws,[](const auto& draw){return draw.native_prompt.has_value();});
        if(present) {prompt_present+=carried_prompts.size();frame.prompts=std::move(carried_prompts);carried_prompts.clear();}
    }
    ++prompt_batch;
    if(packet_hash) hash_packet(frame,back_serial+1);
    const uint64_t serial=packets->seal_batch(std::move(frame));
    draws=gx::DrawQueue{};
    if(serial!=++back_serial) {
        static bool reported=false;
        if(!reported) { reported=true; std::fprintf(stderr,"[gx-builder] sealed packet %llu where %llu was expected\n",(unsigned long long)serial,(unsigned long long)back_serial); }
        back_serial=serial;
    }
    return serial;
}
// Front: the serial of the next packet the builder seals.
uint64_t next_seal() { if (!packets) return 0; return ++front_serial; }
void post_finish(uc::Sched* sched, uint64_t serial,uint64_t generation) {
    sched->post_external([sched, serial] {
        auto found = finishes.find(serial);
        if (found == finishes.end()) return;
        uint32_t base = found->second; finishes.erase(found); ++completed;
        ppc::Ctx& irq = sched->irq_ctx();
        memory.read(base - 25328, 1);
        ppc::wr8(irq, base - 25328, 1);
        uint32_t callback = read32(base - 25324);
        if (callback) uc::call_guest(irq, callback, {});
        sched->wake_all(base - 25336);
    },uc::Sched::EventKind::Gx,generation);
}
void request_finish(ppc::Ctx& ctx) {
    if (front_list) throw gx::Error(gx::ErrorCode::sequence,0x802848AC,0,1);
    finish_primitive();
    auto& sched = *scheduler;
    bool interrupts = sched.set_interrupts(false);
    memory.read(ctx.r[13] - 25328, 1);
    ppc::wr8(ctx, ctx.r[13] - 25328, 0);
    if (packets) {
        // Registered before the builder can seal the packet: its completion always finds the request (det_early stays the race of the
        // unthreaded builder, whose seal happens right here).
        const uint64_t serial=next_seal();
        finishes.emplace(serial,ctx.r[13]); ++submitted;
        bool early=false;
        if(!packets->detached() && sched.deterministic()) {
            std::lock_guard lock(det_finish_m);
            if(queue.threaded()) { det_early.erase(det_early.begin(),det_early.lower_bound(serial)); det_finishes.emplace(serial,sched.async_begin()); }
        }
        back([inputs=seal_inputs()] { submit_batch(inputs,false,false); });
        if(packets->detached()) {
            drain();   // network play delivers the draw-done at once: the builder must have read what this frame drew before the guest reuses it
            post_finish(&sched,serial,sched.generation());
        } else if(sched.deterministic() && !queue.threaded()) {
            { std::lock_guard lock(det_finish_m); early=det_early.erase(serial)>0; det_early.erase(det_early.begin(),det_early.lower_bound(serial)); if(!early) det_finishes.emplace(serial,sched.async_begin()); }
            if(early) post_finish(&sched,serial,sched.generation());
        }
    }
    // Headless is runtime-only diagnostics: it submits no work and cannot signal a graphics fence.
    sched.set_interrupts(interrupts);
}
}
namespace {
// ================= Builder: the commands of the draw entry points =================
// GXBegin outside a display list: the snapshots a live primitive draws with (the front sends the bytes when the last one arrived).
void live_begin(const SdkRef& block,uint8_t primitive,uint8_t vat,uint16_t count,uint32_t who,uint32_t callsite,uint32_t lr) {
    capture->begin(gx::share(live_state,snapshot(block,lr)), primitive, vat, count, who, callsite,replaying);
    if(replaying) return;
    live_texture=snapshot_texture(bound_textures[0],palettes);
    const auto [material,uniforms]=material_snapshot(block,lr);
    live_material=material; gx::share(live_uniforms,uniforms);
    live_texture_wrap=wraps(bound_textures);
    live_texture_maps={};
    const auto maps=texture_mask(material,uniforms);
    for(uint32_t map=1;map<8;++map) if(maps&(1u<<map)) {
        live_texture_maps[map-1]=snapshot_texture(bound_textures[map],palettes);
    }
    gx::share(live_transforms,transforms);
    live_blend=blend_state;
}
void live_end(uint32_t who,std::span<const std::byte> bytes) {
    capture->write_attribute(who,bytes);
    auto draw = capture->take_draw(); add_guest_stat(vertex_count,capture->vertex_count()); add_guest_stat(draw_count,1);
    if(replaying)return;
    draw.transforms=live_transforms;
    draw.texture=live_texture;
    draw.material=live_material; draw.uniforms=live_uniforms; draw.texture_bank=texture_bank(*draw.state,texture_matrices);
    draw.texture_maps=live_texture_maps;
    draw.texture_wrap=live_texture_wrap;
    draw.blend=live_blend;
#ifdef UC_TRACE
    static uint32_t movie_draw_logs=0;
    if (draw.callsite==0x801619C4 && draw.texture && movie_draw_logs++<8) {
        draw.texture->wait();
        std::array<uint8_t,4> low{255,255,255,255},high{};
        const auto& rgba=draw.texture->levels[0].rgba;
        for (size_t i=0;i<rgba.size();++i) { low[i%4]=std::min(low[i%4],rgba[i]); high[i%4]=std::max(high[i%4],rgba[i]); }
        std::fprintf(stderr,"[movie-draw] range %u:%u %u:%u %u:%u %u:%u blend %u %u %u mask %u\n",low[0],high[0],low[1],high[1],low[2],high[2],low[3],high[3],draw.blend.mode,draw.blend.source,draw.blend.destination,draw.blend.color_write);
        for (const auto& v:draw.vertices) std::fprintf(stderr,"[movie-vertex] %.2f %.2f %.2f rgba %u %u %u %u uv %.3f %.3f mtx %u\n",v.position[0],v.position[1],v.position[2],v.color[0][0],v.color[0][1],v.color[0][2],v.color[0][3],v.texcoord[0][0],v.texcoord[0][1],v.position_matrix);
    }
#endif
    draws.push_back(std::move(draw));
}
// GXBegin inside a display list: the recorded draw's state (its payload is the list's bytes from `offset`, known at GXEndDisplayList).
void record_draw(const SdkRef& block,uint32_t lr,uint32_t callsite,uint8_t primitive,uint8_t vat,uint16_t count,size_t expected,size_t offset) {
    auto recorded=std::make_unique<RecordedDraw>();   // filled in place: the ~5 KB of state are copied once
    auto& draw=*recorded;
    draw.state=snapshot(block,lr);
    draw.expected=expected;
    const auto snapped=material_snapshot(block,lr);
    draw.callsite=callsite; draw.primitive=primitive; draw.vat=vat; draw.count=count;
    draw.material=snapped.material; draw.uniforms=snapped.uniforms; draw.texture_selectors=texture_selectors(*block);
    draw.offset=offset;
    brec->native.geometry.push_back(std::move(recorded));
}
bool prompt_attach(uint64_t token,uint32_t who,uint32_t list,bool overflow,const uc::PromptDescription& description,bool compact) {
    const auto& mark=prompt_marks[token%prompt_marks.size()];
    if(mark.token!=token) return false;
    const auto& checkpoint=mark.at;
    if(checkpoint.batch!=prompt_batch || checkpoint.producer!=who)return false;
    if(brec) {
        auto& geometry=brec->native.geometry;
        if(overflow || checkpoint.list!=list || geometry.size()<=checkpoint.recorded)return false;
        RecordedDraw* quad=nullptr;
        for(size_t i=checkpoint.recorded;i<geometry.size();++i)if(auto* draw=recorded_draw(geometry[i])) {
            if(quad)return false; // More than one primitive: not a single glyph/picture quad.
            quad=draw;
        }
        if(!quad || quad!=recorded_draw(geometry.back()) || quad->primitive!=0x80 || quad->count!=4)return false;
        quad->prompt=description;quad->compact=compact;return true;
    }
    if(checkpoint.list || draws.size()!=checkpoint.draw+1)return false;
    auto prompt=uc::prompt_from_quad(draws.back(),description);
    if(!prompt)return false;
    prompt->compact=compact;draws.back().native_prompt=std::move(*prompt);++prompt_annotations;
    return true;
}
// Front: the write-watch counter is about to wrap: every cache of guest memory compares its bytes again.
void reset_watch() {
    drain();
    gx::watch_reset();
    for(auto& [address,resource]:front_resources) resource->skips=256;
    back([] { texture_cache.clear(); mesh_index.clear(); mesh_cache.clear(); mesh_cache_bytes=0; });
}
void close_primitive() {
    auto& prim=front_prim;
    prim.active=false; uc::gx_fifo_window={};
    if(prim.expected>gx::CommandQueue::kMaxBytes) {   // more than a chunk: the bytes travel in their own buffer
        back([who=prim.producer,bytes=std::vector<std::byte>(prim.bytes.begin(),prim.bytes.begin()+std::ptrdiff_t(prim.expected))] { live_end(who,bytes); });
        return;
    }
    try { queue.emit_bytes([who=prim.producer](std::span<const std::byte> bytes) { live_end(who,bytes); },prim.bytes.data(),uint32_t(prim.expected),front_stamp); }
    catch (const gx::Error& error) { if (error.code==gx::ErrorCode::canceled) throw uc::ThreadExit{}; throw; }
}
}
namespace uc {
void gx_set_replaying(bool on) { back([on] { replaying=on; }); }
struct GxSnapshot {
    std::shared_ptr<const gx::SceneSnapshot> scene;
    gx::State state;
    gx::Transforms transforms;
    gx::Shared<gx::Transforms> live_transforms,import_transforms,mesh_transforms;
    gx::Shared<gx::State> live_state,import_state,mesh_state;
    gx::BlendState blend,live_blend;
    gx::DrawUniform colors;
    gx::Shared<gx::DrawUniform> live_uniforms,import_uniforms,mesh_uniforms;
    gx::TevMaterial material;
    TextureMatrices matrices;
    MatrixIndices indices;
    uint32_t sdk;
    std::array<uint32_t,16> indirect;
    Palettes palettes;
    TextureBindings textures;
    std::array<uint8_t,8> texture_wrap;
    std::shared_ptr<const gx::TexturePixels> texture;
    std::array<std::shared_ptr<const gx::TexturePixels>,7> texture_maps;
    CopyState copy;
    std::array<uint32_t,4> copy_source;
    uint32_t copy_width,copy_height,copy_format;
    CopiedTextures copied;
    gx::Vertices vertices;
    gx::DrawQueue draws;
    std::map<uint32_t,std::shared_ptr<const Resource>> resources;
    std::map<uint32_t,std::shared_ptr<const FrontResource>> front_resources;
    std::map<uint64_t,uint32_t> finishes;
    uint64_t prompt_batch;
    std::vector<gx::PromptDraw> prompts;
};
std::shared_ptr<const GxSnapshot> gx_checkpoint() {
    if(front_prim.active || front_list)
        throw std::logic_error("GX checkpoint inside a primitive or display-list recording");
    drain();   // the builder's state is read below
    if(!capture || capture->active() || brec)
        throw std::logic_error("GX checkpoint inside a primitive or display-list recording");
    std::lock_guard lock(det_finish_m);
    if(!det_finishes.empty()) throw std::logic_error("GX checkpoint has unfinished draw-done requests");
    return std::make_shared<GxSnapshot>(GxSnapshot{
        packets ? packets->checkpoint() : nullptr,state,transforms,live_transforms,import_transforms,mesh_transforms_block,
        live_state,import_state,mesh_state,blend_state,live_blend,color_uniforms,
        live_uniforms,import_uniforms,mesh_uniforms_block,live_material,texture_matrices,matrix_indices,
        known_sdk,indirect_stages,palettes,bound_textures,live_texture_wrap,live_texture,live_texture_maps,
        copy_state,texture_copy_source,texture_copy_width,texture_copy_height,texture_copy_format,
        copied_textures,*capture,draws,resources,front_resources,finishes,prompt_batch,carried_prompts});
}
void gx_restore(const GxSnapshot& saved) {
    drain();   // the builder is idle while its state is replaced
    std::lock_guard lock(det_finish_m);
    det_finishes.clear(); det_early.clear();
    if(saved.scene)packets->restore(*saved.scene);
    state=saved.state; transforms=saved.transforms;
    live_transforms=saved.live_transforms; import_transforms=saved.import_transforms; mesh_transforms_block=saved.mesh_transforms;
    live_state=saved.live_state; import_state=saved.import_state; mesh_state=saved.mesh_state;
    blend_state=saved.blend; live_blend=saved.live_blend; color_uniforms=saved.colors;
    live_uniforms=saved.live_uniforms; import_uniforms=saved.import_uniforms; mesh_uniforms_block=saved.mesh_uniforms;
    live_material=saved.material; texture_matrices=saved.matrices; matrix_indices=saved.indices;
    known_sdk=saved.sdk; indirect_stages=saved.indirect; palettes=saved.palettes; bound_textures=saved.textures;
    live_texture_wrap=saved.texture_wrap; live_texture=saved.texture; live_texture_maps=saved.texture_maps;
    copy_state=saved.copy; texture_copy_source=saved.copy_source; texture_copy_width=saved.copy_width;
    texture_copy_height=saved.copy_height; texture_copy_format=saved.copy_format; copied_textures=saved.copied;
    capture=std::make_unique<gx::Vertices>(saved.vertices); brec.reset(); front_list.reset(); front_prim.active=false; uc::gx_fifo_window={}; draws=saved.draws;
    resources=saved.resources; front_resources=saved.front_resources; finishes=saved.finishes; prompt_batch=saved.prompt_batch; carried_prompts=saved.prompts;
    // State/material snapshots belong to the abandoned timeline.
    state_cache.valid=material_cache.valid=false; dirty_state(); dirty_material();
    sdk_last.reset(); front_derived_block.reset();
    // Retain immutable decoded meshes; marked epochs force descriptor/source byte validation,
    // including RAM restored by bulk writes that bypass the guest store barrier.
    for(auto& epoch:gx::page_epoch) epoch=gx::write_epoch;
    gx::watch_stamp();
    if(queue.threaded()) front_stamp=gx::watch_stamp();
}
bool gx_fifo_in_primitive() { return live_primitive() || recording_primitive(); }
bool gx_fifo_recording() { return front_list != nullptr; }
// The builder evaluates both in command order; the front's result only says the request was passed on (the caller logs rejections).
GxPromptCheckpoint gx_prompt_checkpoint() {
    const uint64_t token=++prompt_tokens;
    const uint32_t who=producer(),list=front_list ? front_list->address : 0;
    back([token,who,list] {
        auto& mark=prompt_marks[token%prompt_marks.size()];
        mark.token=token; mark.at={prompt_batch,draws.size(),who,list,brec ? brec->native.geometry.size() : 0};
    });
    return {token,0,who,list,0};
}
bool gx_prompt_attach(GxPromptCheckpoint checkpoint,const PromptDescription& description,bool compact) {
    const uint32_t who=producer(),list=front_list ? front_list->address : 0;
    const bool overflow=front_list && front_list->overflow;
    if(!queue.threaded()) return prompt_attach(checkpoint.batch,who,list,overflow,description,compact);
    back([token=checkpoint.batch,who,list,overflow,description,compact] { prompt_attach(token,who,list,overflow,description,compact); });
    return true;
}
void gx_enable_async_textures() { back([] { texture_cache.set_async(true); }); }
void gx_set_texture_budget(size_t bytes) { texture_cache.set_budget(bytes); }
void gx_hle_init(Sched& sched, uint8_t* base, gx::Packets* packet_queue, bool profile) {
    queue.stop();   // a builder from an earlier init ends after its commands ran
    replaying=false;
    prompt_batch=0;carried_prompts.clear();
    prompt_annotations=prompt_present=prompt_fallback=0;
    profile_mesh=profile; mesh_profile_count=0; mesh_dumps=0; mesh_dumps_seen=0; mesh_dumps_settled=false;
    prompt_color_logs=0;
    copy_profile_count=0;
    channel_profile_count=0;
    texgen_profile_count=0;
    scheduler = &sched; packets = packet_queue;
    memory = {{reinterpret_cast<const std::byte*>(base), 24u << 20}, {reinterpret_cast<const std::byte*>(base + 0x10000000), 64u << 20}, {reinterpret_cast<const std::byte*>(base + 0x20000000), 0x4000}};
    dirty_state(); dirty_material();
    state = {}; transforms={}; live_transforms=gx::Transforms{}; import_transforms=gx::Transforms{}; mesh_transforms_block=gx::Transforms{}; live_state=gx::State{}; import_state=gx::State{}; mesh_state=gx::State{}; import_uniforms=gx::DrawUniform{}; mesh_uniforms_block=gx::DrawUniform{}; draws.clear(); finishes.clear(); resources.clear(); brec.reset(); capture = std::make_unique<gx::Vertices>(memory);
    copy_state={}; copies=0;
    texture_copy_source={}; texture_copy_width=texture_copy_height=0; texture_copy_format=6; copied_textures.clear();
    texture_cache.clear(); texture_decodes=0;
    mesh_index.clear(); mesh_cache.clear(); mesh_cache_bytes=0; geometry_hits=0; geometry_misses=0;
    blend_state={}; live_blend={}; color_uniforms={}; live_uniforms=gx::DrawUniform{}; live_material={}; texture_matrices={}; matrix_indices={}; indirect_stages={};
    bound_textures={}; palettes.clear(); live_texture.reset(); live_texture_maps={};
    draw_count = 0; vertex_count = 0; submitted = 0; completed = 0; outside_bytes = 0;
    state_cache={}; material_cache={};
    known_sdk=0; front_resources.clear(); front_list.reset(); front_prim.active=false; uc::gx_fifo_window={};
    sdk_last.reset(); front_derived_block.reset();
    { std::lock_guard lock(det_finish_m); det_finishes.clear(); det_early.clear(); }
    front_serial=back_serial=packets ? packets->next_serial()-1 : 0;
    front_stamp=0; prompt_tokens=0; prompt_marks={};
}
void gx_enable_builder() {
    if(std::getenv("UC_GX_SYNC")) { std::fprintf(stderr,"[gx-builder] off (UC_GX_SYNC): the guest thread builds the draws\n"); return; }
    front_stamp=gx::watch_stamp();
    queue.start([] {
        host_thread_mark_guest();   // the headset registers it with the XR runtime as one of the application's main threads
#if defined(__APPLE__)
        pthread_setname_np("gx-builder");
#elif defined(__linux__)
        pthread_setname_np(pthread_self(),"gx-builder");
#endif
    },[] {
        // The guest thread may be blocked (waiting for a draw-done the failed builder never seals): an event rethrows the error there.
        if(scheduler) scheduler->post_external([] { builder_failed(); },uc::Sched::EventKind::Gx);
    });
}
void gx_stop_builder() { queue.stop(); }
void gx_sync() { if(queue.threaded()) drain(); }
}
namespace {
void store_guest_word(uint32_t address,uint32_t value) {
    const size_t physical=address & 0x3FFFFFFF;
    if(physical+4>memory.mem1.size()) return;
    auto* host=const_cast<std::byte*>(memory.mem1.data())+physical;
    for(int i=0;i<4;++i) host[i]=std::byte(uint8_t(value>>(24-i*8)));
    gx::note_write_range(address,4);
}
// A raw "call display list" command (0x40, address, length) written straight to the FIFO, as the game's texgen/setup appliers do instead of GXCallDisplayList.
// The hardware executes it; here the XF texgen words it loads (0x1040+n control, 0x1050+n dual-texture post) are written into the SDK block, which the native
// texgen reads, so the display list wins until the guest sets that texgen again. Other commands of the list are not interpreted (counted for the log).
void raw_display_list(uint32_t address,uint32_t length) {
    static uint32_t ignored_reports=0;
    if(!known_sdk || length>4096 || (address&3)) return;
    const auto bytes=memory.read(address,length);
    auto word=[&](size_t at) { uint32_t value=0; for(size_t i=0;i<4;++i) value=value<<8|std::to_integer<uint8_t>(bytes[at+i]); return value; };
    for(size_t at=0;at<bytes.size();) {
        const auto op=std::to_integer<uint8_t>(bytes[at++]);
        if(op==0) continue;
        if(op==0x61 && at+4<=bytes.size()) { at+=4; continue; }
        if(op==0x10 && at+4<=bytes.size()) {
            const uint32_t header=word(at),count=(header>>16)+1,reg=header&0xFFFF; at+=4;
            if(at+4*size_t(count)>bytes.size()) return;
            for(uint32_t i=0;i<count;++i,at+=4) {
                const uint32_t r=reg+i;
                if(r>=0x1040 && r<=0x1047) store_guest_word(known_sdk+0xC8+(r-0x1040)*4,word(at));
                else if(r>=0x1050 && r<=0x1057) store_guest_word(known_sdk+0xE8+(r-0x1050)*4,word(at));
                else if(r==0x1018 || r==0x1019) { const uint32_t value=word(at); back([reg=r-0x1018,value] { set_matrix_index(reg,value,false); }); }
            }
            continue;
        }
        if(ignored_reports++<4) std::fprintf(stderr,"[raw-display-list] %08X: unhandled opcode %02X, rest ignored\n",address,op);
        return;
    }
}
}
namespace uc {
void gx_fifo_write(int size, uint32_t value) {
    if (size != 1 && size != 2 && size != 4) throw std::invalid_argument("invalid native GX write width");
    const uint32_t big=uc_bswap32(value<<(32-8*size));   // the size low bytes of value, most significant first
    if (front_list) {
        auto& list=*front_list;
        if (producer() != list.owner) throw gx::Error(gx::ErrorCode::producer,list.address,list.owner,producer());
        if (list.overflow) return; // GXEndDisplayList reports this failed recording with length zero
        std::byte* at=gx_fifo_window.at;
        const size_t used=size_t(at-list.bytes.get());
        if (size_t(size) > list.capacity - used) { list.overflow=true; list.used=used; gx_fifo_window={}; return; }
        if (list.prim_open) {
            const size_t left=size_t(list.prim_end-at);
            if (size_t(size)>left) throw gx::Error(gx::ErrorCode::truncated,list.prim_callsite,left,size);
        }
        std::memcpy(at,&big,size_t(size)); at+=size;
        gx_fifo_window.at=at;
        if (list.prim_open && at==list.prim_end) list.prim_open=false;
        list_window();
        return;
    }
    if (!front_prim.active) {
        static int raw_state=0; static uint32_t raw_address=0;   // a raw display list call is three writes: 0x40, address, length
        if (raw_state==0) { if (size==1 && value==0x40) raw_state=1; }
        else if (raw_state==1) { if (size==4) { raw_address=value; raw_state=2; } else raw_state=0; }
        else {
            if (size==4) {
                try { raw_display_list(raw_address,value); }
                catch(const std::exception& error) { static bool reported=false; if(!reported) { reported=true; std::fprintf(stderr,"[raw-display-list] %08X length %u: %s\n",raw_address,value,error.what()); } }
            }
            raw_state=0;
        }
        add_guest_stat(outside_bytes,uint64_t(size)); return;
    } // existing TD-014; all state bytes need owners before game-picture acceptance
    auto& prim=front_prim;
    if (producer() != prim.producer) throw gx::Error(gx::ErrorCode::producer,prim.callsite,prim.producer,producer());
    std::byte* at=gx_fifo_window.at;
    const size_t left=size_t(prim.bytes.data()+prim.expected-at);
    if (size_t(size)>left) throw gx::Error(gx::ErrorCode::truncated,prim.callsite,left,size);
    std::memcpy(at,&big,size_t(size)); at+=size;
    gx_fifo_window.at=at;
    if (size_t(size)==left) close_primitive();
}
void gx_completed(uint64_t serial) {
    Sched* sched = scheduler;
    if (!sched) return;
    uint64_t generation=sched->generation();
    if (sched->deterministic()) {   // only the draw-done requests the guest announced are delivered (and only while it is idle)
        std::lock_guard lock(det_finish_m);
        const auto found=det_finishes.find(serial);
        if(found==det_finishes.end()) { det_early.insert(serial); return; }
        generation=found->second; det_finishes.erase(found);
    }
    post_finish(sched, serial,generation);
}
std::string gx_state_summary() {   // crash bundle gx.txt: bound arrays (12 = position matrices, 13 = normal matrices), "recorded" while a display list is being built
    try { drain(); } catch (...) {}   // a failed builder has stopped: its state is still readable
    std::string text; char line[96];
    const auto add = [&](const char* kind, const gx::State& s) {
        for (size_t i = 0; i < s.arrays.size(); ++i) if (s.arrays[i].address) { std::snprintf(line, sizeof line, "%s array %zu address %08X stride %u\n", kind, i, s.arrays[i].address, unsigned(s.arrays[i].stride)); text += line; }
    };
    add("live", state);
    if (brec) add("recorded", brec->input_state);
    return text;
}
GxStats gx_stats() { return {draw_count.load(), vertex_count.load(), submitted.load(), completed.load(), outside_bytes.load(),copies.load(),texture_decodes.load(),geometry_hits.load(),geometry_misses.load(),prompt_annotations.load(),prompt_present.load(),prompt_fallback.load(),texture_evictions.load(),texture_resident.load()}; }
}
namespace {
// The checks every GX call between primitives makes (sequence, producer).
void matrix_boundary(uint32_t address) {
    if (live_primitive() || recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,address,0,1);
    if (front_list && producer()!=front_list->owner) throw gx::Error(gx::ErrorCode::producer,address,front_list->owner,producer());
}
bool recording_push() { return front_list && !front_list->overflow; }   // the call is recorded into the open display list
void front_effect(FrontEffect effect) { if (recording_push()) front_list->effects.push_back(effect); }
}
void hle_GXBegin(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80284FD0, c);
    if (front_list) {
        if (recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,0x80284FD0,0,1);
        const uint32_t primitive=c.r[3], vat=c.r[4], count=c.r[5], sdk=read32(c.r[2]-8968);
        const uint32_t callsite=c.lr ? c.lr-4 : 0x80284FD0;
        if (read32(sdk+0x5FC)) uc::call_guest(c,0x80284D58,{});
        if (!read32(sdk)) uc::call_guest(c,0x8028511C,{});
        uc::gx_fifo_store(1,uint8_t(primitive) | uint8_t(vat));
        uc::gx_fifo_store(2,uint16_t(count));
        if (!uint16_t(count)) return;   // the list keeps the header; a primitive without vertices draws nothing
        if (!front_list->overflow) {
            const auto block=capture_sdk(sdk);
            const size_t expected=gx::encoded_draw_size(front_state(block),uint8_t(primitive),uint8_t(vat),uint16_t(count),callsite);
            known_sdk=sdk;
            auto& list=*front_list;
            const size_t offset=list_used();
            list.prim_open=true; list.prim_end=list.bytes.get()+offset+expected; list.prim_callsite=callsite; list.prim_expected=expected;
            if(offset+expected>list.capacity) list.prim_end=list.bytes.get()+list.capacity+1;   // never reached: the recording overflows first
            list_window();
            back([block,lr=c.lr,callsite,primitive=uint8_t(primitive),vat=uint8_t(vat),count=uint16_t(count),expected,offset] { record_draw(block,lr,callsite,primitive,vat,count,expected,offset); });
        }
        return;
    }
    const uint32_t sdk=read32(c.r[2]-8968),dirty=read32(sdk+0x5FC);
    if(dirty&0x04000000) {
        for(uint32_t reg=0;reg<2;++reg) { const uint32_t value=read32(sdk+0x80+reg*4); back([reg,value] { set_matrix_index(reg,value,false); }); }
        ppc::wr32(c,sdk+0x5FC,dirty&~0x04000000u);
    }
    if (!uint16_t(c.r[5])) return;   // a primitive without vertices (an empty quad batch) draws nothing
    const auto block=capture_sdk(sdk);
    const uint32_t callsite=c.lr ? c.lr - 4 : 0x80284FD0;
    const auto& layout=front_state(block);
    if (front_prim.active) throw gx::Error(gx::ErrorCode::sequence,callsite,0,uint16_t(c.r[5]));
    const size_t expected=gx::encoded_draw_size(layout,uint8_t(c.r[3]),uint8_t(c.r[4]),uint16_t(c.r[5]),callsite);
    known_sdk=sdk;
    const uint32_t who=producer();
    back_checked([block,who,callsite,lr=c.lr,primitive=uint8_t(c.r[3]),vat=uint8_t(c.r[4]),count=uint16_t(c.r[5])] { live_begin(block,primitive,vat,count,who,callsite,lr); });
    auto& prim=front_prim;
    prim.active=true; prim.producer=who; prim.callsite=callsite; prim.expected=expected;
    if(prim.bytes.size()<expected+8) prim.bytes.resize(expected+8);
    uc::gx_fifo_window={prim.bytes.data(),prim.bytes.data()+expected};
}
void hle_GXSetArray(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802841DC, c);
    uint32_t attr = c.r[3] == 25 ? 10 : c.r[3];
    if (attr < 9 || attr > 24) throw gx::Error(gx::ErrorCode::layout, 0x802841DC, 24, attr);
    const gx::Array array{c.r[4],uint8_t(c.r[5])};
    if (front_list) {
        if (recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,0x802841DC,0,1);
        uc::gx_fifo_store(1,8); uc::gx_fifo_store(1,0xA0+attr-9); uc::gx_fifo_store(4,c.r[4] & 0x3FFFFFFF);
        uc::gx_fifo_store(1,8); uc::gx_fifo_store(1,0xB0+attr-9); uc::gx_fifo_store(4,c.r[5]);
        back([attr,array] { brec->input_state.arrays[attr-9]=array; dirty_state(); brec->native.geometry.push_back(Resource::ArrayBinding{attr-9,array}); });
        return;
    }
    back([attr,array] { state.arrays[attr - 9] = array; dirty_state(); });
}
void hle_GXSetDrawDone(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802848AC,c);
    uc::PendingHle pending(*scheduler,c);
    request_finish(c);
}
void hle_GXWaitDrawDone(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80284944, c);
    auto& sched = *scheduler;
    uc::PendingHle pending(sched,c,hle_GXWaitDrawDone);
    bool interrupts = sched.set_interrupts(false);
    while (std::to_integer<uint8_t>(memory.read(c.r[13] - 25328, 1)[0]) == 0) sched.block_on(c.r[13] - 25336);
    pending.committed();
    sched.set_interrupts(interrupts);
}
void hle_GXDrawDone(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80284990,c);
    uc::PendingHle pending(*scheduler,c,hle_GXWaitDrawDone);
    request_finish(c);
    hle_GXWaitDrawDone(c);
}
void hle_GXBeginDisplayList(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288528,c);
    if (front_list || front_prim.active) throw gx::Error(gx::ErrorCode::sequence,0x80288528,0,1);
    const uint32_t address = c.r[3], capacity = c.r[4], sdk = read32(c.r[2]-8968);
    if ((address & 31) || capacity < 32 || (capacity & 31)) throw gx::Error(gx::ErrorCode::layout,address,32,capacity);
    memory.read(address,capacity); memory.read(sdk,0x600);
    if (read32(sdk+0x5FC)) uc::call_guest(c,0x80284D58,{}); // SDK dirty flush occurs before recording
    auto list = std::make_unique<FrontList>();
    list->address=address; list->capacity=capacity; list->sdk=sdk; list->owner=producer();
    list->restore=std::to_integer<uint8_t>(memory.read(sdk+0x5F9,1)[0])!=0;
    if (list->restore) {
        auto shadow=memory.read(sdk,0x600);
        std::copy(shadow.begin(),shadow.end(),list->shadow.begin());
        list->texture_copy_source_shadow=texture_copy_source;
        list->texture_copy_width_shadow=texture_copy_width; list->texture_copy_height_shadow=texture_copy_height;
        list->texture_copy_format_shadow=texture_copy_format;
    }
    list->bytes=std::make_unique_for_overwrite<std::byte[]>(size_t(capacity)+8);   // the SDK passes the exact buffer size; 8 bytes of slack for the inline FIFO path
    ppc::wr8(c,sdk+0x5F8,1);
    back([restore=list->restore] {
        auto recording=std::make_unique<BackList>();
        recording->input_state=state; recording->restore=restore;
        if (restore) { recording->color_shadow=color_uniforms; recording->texture_shadow=texture_matrices; recording->indices_shadow=matrix_indices; }
        brec=std::move(recording); dirty_state();
    });
    front_list=std::move(list);
    uc::gx_fifo_window={front_list->bytes.get(),nullptr}; list_window();
}
void hle_GXEndDisplayList(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802885D8,c);
    if (!front_list) throw gx::Error(gx::ErrorCode::sequence,0x802885D8,1,0);
    if (front_list->owner!=producer()) throw gx::Error(gx::ErrorCode::producer,front_list->address,front_list->owner,producer());
    if (!front_list->overflow && front_list->prim_open)
        throw gx::Error(gx::ErrorCode::truncated,front_list->prim_callsite,front_list->prim_expected,front_list->prim_expected-size_t(front_list->prim_end-uc::gx_fifo_window.at));
    uc::call_guest(c,0x80284504,{}); // verified GXFlush: dirty state followed by 32 NOP bytes
    const size_t used=list_used();
    auto list=std::move(front_list); uc::gx_fifo_window={};
    const uint32_t length=list->overflow ? 0 : uint32_t(used) & ~31u;
    if (length) { std::memcpy(c.mem+(list->address & 0x3FFFFFFF),list->bytes.get(),length); gx::note_write_range(list->address,length); }
    std::shared_ptr<const std::vector<std::byte>> bytes;
    if (length) {
        const uint32_t address=list->address & 0x3FFFFFFF;
        // The recordings never overlap (each one replaced those it overlaps): only the one before address and those starting inside the new one can.
        auto it=front_resources.lower_bound(address);
        if (it!=front_resources.begin() && uint64_t(std::prev(it)->first)+std::prev(it)->second->length>address) --it;
        while (it!=front_resources.end() && uint64_t(it->first)<uint64_t(address)+length) {
            if (uint64_t(address)<uint64_t(it->first)+it->second->length) it=front_resources.erase(it);
            else ++it;
        }
        bytes=std::make_shared<const std::vector<std::byte>>(list->bytes.get(),list->bytes.get()+used);
        auto resource=std::make_shared<FrontResource>();
        resource->bytes=bytes; resource->length=length; resource->stamp=gx::watch_stamp(); resource->effects=std::move(list->effects);
        front_resources.emplace(address,std::move(resource));
    }
    uc::gx_audit_recorded_resource(list->address,std::span<const std::byte>(list->bytes.get(),length));
    back([address=list->address,length,bytes] {
        auto recording=std::move(brec); dirty_state();
        if (length) {
            const uint32_t physical=address & 0x3FFFFFFF;
            std::shared_ptr<const Resource> replaced_at;   // the earlier recording at this address: its decodes may be reused
            if(auto same=resources.find(physical);same!=resources.end()) replaced_at=same->second;
            auto it=resources.lower_bound(physical);
            if (it!=resources.begin() && uint64_t(std::prev(it)->first)+std::prev(it)->second->length>physical) --it;
            while (it!=resources.end() && uint64_t(it->first)<uint64_t(physical)+length) {
                if (uint64_t(physical)<uint64_t(it->first)+it->second->length) { Graveyard::instance().bury(std::move(it->second)); it=resources.erase(it); }
                else ++it;
            }
            recording->native.list_bytes=bytes; recording->native.length=length;
            if(const auto previous=replaced_at; previous) inherit_decodes(recording->native,*previous);
            resources.emplace(physical,std::make_shared<const Resource>(std::move(recording->native)));
            if(replaced_at) Graveyard::instance().bury(std::move(replaced_at));
        }
        if (recording->restore) {
            color_uniforms=recording->color_shadow; texture_matrices=std::move(recording->texture_shadow); dirty_material();
            matrix_indices=recording->indices_shadow;
        }
    });
    if (list->restore) {
        const uint32_t kept=read32(list->sdk+8);
        std::memcpy(c.mem+(list->sdk & 0x3FFFFFFF),list->shadow.data(),list->shadow.size()); gx::note_write_range(list->sdk,list->shadow.size());
        ppc::wr32(c,list->sdk+8,kept);
        texture_copy_source=list->texture_copy_source_shadow;
        texture_copy_width=list->texture_copy_width_shadow; texture_copy_height=list->texture_copy_height_shadow;
        texture_copy_format=list->texture_copy_format_shadow;
    }
    ppc::wr8(c,list->sdk+0x5F8,0);
    c.r[3]=length;
}

namespace {
// Front: what a call of the list does that the guest observes (the colour channel words written back, the packets its texture copies seal),
// with the checks the import made (nesting, every child list's bytes).
void walk_effects(uint32_t address,uint32_t length,uint32_t& channels_count,std::array<uint32_t,4>& channels,uint32_t& texture_copies,std::vector<uint32_t>& path) {
    const uint32_t physical=address & 0x3FFFFFFF;
    if (path.size()==64 || std::find(path.begin(),path.end(),physical)!=path.end())
        throw gx::Error(gx::ErrorCode::sequence,address,64,path.size());
    const auto& resource=checked_resource(address,length);
    path.push_back(physical);
    for (const auto& effect : resource.effects) switch (effect.kind) {
        case FrontEffect::child: walk_effects(effect.a,effect.b,channels_count,channels,texture_copies,path); break;
        case FrontEffect::num_channels: channels_count=effect.a; break;
        case FrontEffect::channel_control: channels[effect.a]=effect.b; break;
        case FrontEffect::texture_copy: ++texture_copies; break;
    }
    path.pop_back();
}
void call_list(const SdkRef& block,uint32_t lr,uint32_t address,uint32_t length,const SealInputs& inputs) {
    Imported imported;
    auto bindings=state;
    auto transform_state=transforms;
    auto copy=copy_state;
    auto textures=bound_textures;
    auto loaded=palettes;
    auto blend=blend_state;
    auto colors=material_snapshot(block,lr).uniforms;
    auto matrices=texture_matrices;
    auto indices=matrix_indices;
    auto versions=copied_textures;
    std::vector<uint32_t> path;
    import_resource(address,length,bindings,transform_state,copy,textures,loaded,blend,colors,matrices,indices,imported,versions,path);
    for (auto& item : imported) {
        if(auto* draw=std::get_if<gx::DrawPacket>(&item)) { add_guest_stat(vertex_count,draw->vertices.size()); add_guest_stat(draw_count,1); draws.push_back(std::move(*draw)); }
        else {
            auto& command=std::get<CopiedTexture>(item); copy_state=command.clear_state;
            submit_batch(inputs,false,command.clear,0,std::move(command.request));
        }
    }
    copied_textures=std::move(versions);
    state.arrays=bindings.arrays; dirty_state(); dirty_material();
    transforms=transform_state;
    copy_state=copy;
    bound_textures=textures;
    palettes=std::move(loaded);
    blend_state=blend;
    color_uniforms.registers=colors.registers; color_uniforms.konst=colors.konst;
    color_uniforms.lights=colors.lights; color_uniforms.fog=colors.fog; color_uniforms.indirect=colors.indirect;
    texture_matrices=std::move(matrices);
    matrix_indices=indices;
}
}
void hle_GXCallDisplayList(ppc::Ctx& c) {
    CallTimer timer(0);
    uc::gx_audit_enter(0x80288684,c);
    const uint32_t address=c.r[3],length=c.r[4],sdk=read32(c.r[2]-8968);
    if (live_primitive() || recording_primitive()) throw gx::Error(gx::ErrorCode::sequence,0x80288684,0,1);
    checked_resource(address,length);
    if (read32(sdk+0x5FC)) uc::call_guest(c,0x80284D58,{});
    if (!read32(sdk)) uc::call_guest(c,0x8028511C,{});
    if (front_list) {
        if (producer()!=front_list->owner) throw gx::Error(gx::ErrorCode::producer,address,front_list->owner,producer());
        uc::gx_fifo_store(1,0x40); uc::gx_fifo_store(4,address); uc::gx_fifo_store(4,length);
        if (front_list->overflow) return;
        front_list->effects.push_back({FrontEffect::child,address,length});
        back([address,length] { brec->native.geometry.push_back(Resource::Child{address,length}); });
        return;
    }
    const auto block=capture_sdk(sdk); known_sdk=sdk;
    // The colour channel count and controls the guest sees afterwards: the SDK block's, changed by the list's own settings.
    uint32_t channels_count=(block->word(0x254)>>4)&3;
    std::array<uint32_t,4> channels{};
    for(uint32_t i=0;i<4;++i) channels[i]=block->word(0xB8+i*4);
    uint32_t texture_copies=0;
    std::vector<uint32_t> path;
    walk_effects(address,length,channels_count,channels,texture_copies,path);
    for(uint32_t i=0;i<texture_copies;++i) { next_seal(); ++copies; }
    back_checked([block,lr=c.lr,address,length,inputs=texture_copies ? seal_inputs() : SealInputs{}] { call_list(block,lr,address,length,inputs); });   // the inputs only serve the packets its copies seal
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x70u)|(channels_count<<4));
    for(uint32_t i=0;i<4;++i) ppc::wr32(c,sdk+0xB8+i*4,channels[i]);
}

namespace {
void projection(ppc::Ctx& c,const std::array<float,6>& p,uint32_t type) {
    matrix_boundary(0x8028873C);
    const auto matrix=gx::make_projection(p,type);
    const uint32_t sdk=read32(c.r[2]-8968);
    memory.read(sdk+0x528,28);
    ppc::wr32(c,sdk+0x528,type);
    for (size_t i=0;i<6;++i) ppc::wr32(c,sdk+0x52C+uint32_t(i*4),std::bit_cast<uint32_t>(p[i]));
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|0x08000000);
    const bool push=recording_push();
    back([matrix,push] { if (brec) { if (push) brec->native.geometry.push_back(Resource::Projection{matrix}); } else transforms.projection=matrix; });
}
}
void hle_GXSetProjection(ppc::Ctx& c) {
    uc::gx_audit_enter(0x8028873C,c);
    const uint32_t address=c.r[3],type=c.r[4]; memory.read(address,48);
    std::array<float,6> p{};
    const uint32_t offsets[]={0,type==1 ? 12u : 8u,20,type==1 ? 28u : 24u,40,44};
    for (size_t i=0;i<6;++i) p[i]=std::bit_cast<float>(read32(address+offsets[i]));
    projection(c,p,type);
}
void hle_GXSetProjectionv(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802887A0,c);
    const uint32_t address=c.r[3]; memory.read(address,28);
    std::array<float,6> p{};
    for (size_t i=0;i<6;++i) p[i]=std::bit_cast<float>(read32(address+4+uint32_t(i*4)));
    projection(c,p,std::bit_cast<float>(read32(address))==0 ? 0 : 1);
}
void hle_GXLoadPosMtxImm(ppc::Ctx& c) {
    uc::gx_audit_enter(0x8028882C,c); matrix_boundary(0x8028882C);
    const uint32_t row=c.r[4]; const auto matrix=position_matrix(c.r[3]);
    if (row>61) throw gx::Error(gx::ErrorCode::layout,0x8028882C,61,row);   // gx::set_position_matrix's check
    if (front_list) {
        uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,0x000B0000|row*4);
        for (const auto& values : matrix) for (float value : values) uc::gx_fifo_store(4,std::bit_cast<uint32_t>(value));
        if (recording_push()) back([row,matrix] { brec->native.geometry.push_back(Resource::Position{row,matrix}); });
    } else back([row,matrix] { gx::set_position_matrix(transforms,row,matrix); matrix_rows(texture_matrices,nullptr,row,matrix,3); dirty_material(); });
}
void hle_GXLoadPosMtxIndx(ppc::Ctx& c) {
    uc::gx_audit_enter(0x8028887C,c); matrix_boundary(0x8028887C);
    const uint16_t index=uint16_t(c.r[3]); const uint32_t row=c.r[4];
    if (row>61) throw gx::Error(gx::ErrorCode::layout,0x8028887C,61,row);
    if (front_list) {
        uc::gx_fifo_store(1,0x20); uc::gx_fifo_store(4,uint32_t(index)<<16|0xB000|row*4);
        if (recording_push()) back([index,row] { brec->native.geometry.push_back(Resource::IndexedPosition{index,row}); });
    } else back([index,row] { indexed_position(transforms,texture_matrices,state,index,row); dirty_material(); });
}
void hle_GXLoadNrmMtxIndx3x3(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288900,c); matrix_boundary(0x80288900);
    const uint16_t index=uint16_t(c.r[3]); const uint32_t row=c.r[4];
    if (row>61) throw gx::Error(gx::ErrorCode::layout,0x80288900,61,row);
    if (front_list) {
        uc::gx_fifo_store(1,0x28); uc::gx_fifo_store(4,uint32_t(index)<<16|0x8000|(0x400+row*3));
        if (recording_push()) back([index,row] { brec->native.geometry.push_back(Resource::IndexedNormal{index,row}); });
    } else back([index,row] { indexed_normal(transforms,state,index,row); });
}
void hle_GXSetCurrentMtx(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288934,c); matrix_boundary(0x80288934);
    const uint32_t sdk=read32(c.r[2]-8968),row=c.r[3]&63;
    ppc::wr32(c,sdk+0x80,(read32(sdk+0x80)&~63u)|row);
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|0x04000000);
    const bool push=recording_push();
    back([row,push] { if (brec) { if (push) brec->native.geometry.push_back(Resource::Current{row}); } else transforms.current=row; });
}
void hle_GXSetMatrixIndex(ppc::Ctx& c) {
    matrix_boundary(0x80288BFC);
    const auto reg=int32_t(c.r[3])<5 ? 0u : 1u,sdk=read32(c.r[2]-8968),value=read32(sdk+0x80+reg*4);
    uc::gx_fifo_store(1,8); uc::gx_fifo_store(1,0x30+reg*0x10); uc::gx_fifo_store(4,value);
    uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,0x1018+reg); uc::gx_fifo_store(4,value);
    const bool push=recording_push();
    back([reg,value,push] { set_matrix_index(reg,value,push); });
    ppc::wr16(c,sdk+2,1);
}
void hle_ResResetMatrixIndices(ppc::Ctx&) {
    matrix_boundary(0x803740B8);
    constexpr uint32_t values[]={0x3CF3CF00,0x00F3CF3C};
    for(uint32_t reg=0;reg<2;++reg) { uc::gx_fifo_store(1,8); uc::gx_fifo_store(1,0x30+reg*0x10); uc::gx_fifo_store(4,values[reg]); }
    uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,0x00011018);
    for(uint32_t reg=0;reg<2;++reg) { uc::gx_fifo_store(4,values[reg]); const bool push=recording_push(); back([reg,value=values[reg],push] { set_matrix_index(reg,value,push); }); }
}
void hle_ResTexMatrixIndices(ppc::Ctx& c) {
    matrix_boundary(0x80374114); memory.read(c.r[3],32);
    uint32_t values[2]={};
    for(uint32_t i=0;i<8;++i) values[i/4]|=read32(c.r[3]+i*4)<<(i<4 ? 6+i*6 : (i-4)*6);
    uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,0x00011018);
    for(uint32_t reg=0;reg<2;++reg) { uc::gx_fifo_store(4,values[reg]); const bool push=recording_push(); back([reg,value=values[reg],push] { set_matrix_index(reg,value,push); }); }
}

void hle_GXLoadTexObjPreLoaded(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802869A0,c); matrix_boundary(0x802869A0);
    const uint32_t object=c.r[3],region=c.r[4],map=c.r[5],sdk=read32(c.r[2]-8968);
    if (map>7) throw gx::Error(gx::ErrorCode::layout,0x802869A0,7,map);
    memory.read(object,32); memory.read(region,8); memory.read(sdk,0x600);
    const auto flags=std::to_integer<uint8_t>(memory.read(object+31,1)[0]);
    const uint32_t image0=read32(object+8),format=read32(object+20);
    TextureSource source{(read32(object+12)&0xFFFFFF)<<5,{(image0&1023)+1,((image0>>10)&1023)+1,gx::TextureFormat(format),uint8_t(flags&1 ? ((read32(object+4)>>8)&255)/16+1 : 1)}};
    source.wrap=uint8_t(read32(object)&255);
    if((source.wrap&3)>2 || ((source.wrap>>2)&3)>2 || ((source.wrap>>5)&3)>2) throw std::invalid_argument("invalid native texture sampler mode");
    if (format>255 || (format&15)!=((image0>>20)&15)) throw gx::TextureError(gx::TextureErrorCode::descriptor,"inconsistent native GXTexObj format");
    memory.read(source.address,gx::encoded_texture_size(source.desc));
#ifdef UC_TRACE
    static std::vector<uint64_t> plane_logs;
    const uint32_t outer=read32(c.r[1]+20);
    const uint64_t key=uint64_t(outer)<<32 | uint64_t(map)<<28 | uint64_t(format)<<20 | uint64_t(source.desc.width-1)<<10 | (source.desc.height-1);
    if (plane_logs.size()<80 && std::find(plane_logs.begin(),plane_logs.end(),key)==plane_logs.end()) {
        plane_logs.push_back(key);
        std::fprintf(stderr,"[texture-plane] map %u address %08X %ux%u format %u caller %08X outer %08X\n",map,source.address,source.desc.width,source.desc.height,format,c.lr,outer);
    }
#endif
    const uint32_t tables[]={28936,28928,28920,28912,28904,28896};
    uint32_t words[]={read32(object),read32(object+4),image0,read32(region),read32(region+4),read32(object+12)};
    for (size_t i=0;i<6;++i) words[i]=(words[i]&0xFFFFFF)|(uint32_t(std::to_integer<uint8_t>(memory.read(c.r[13]-tables[i]+map,1)[0]))<<24);
    // Keep verified SDK byte identity for native resource version checks; no BP interpretation.
    for (auto word : words) { uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word); }
    ppc::wr32(c,object,words[0]); ppc::wr32(c,object+4,words[1]); ppc::wr32(c,object+8,words[2]); ppc::wr32(c,object+12,words[5]);
    ppc::wr32(c,region,words[3]); ppc::wr32(c,region+4,words[4]);
    if (!(flags&2)) {
        const auto palette_region=uc::call_guest(c,read32(sdk+0x51C),{read32(object+24)});
        memory.read(palette_region,16);
        source.palette=palette_region&0x3FFFFFFF;
        const auto tag=std::to_integer<uint8_t>(memory.read(c.r[13]-28888+map,1)[0]);
        const auto word=(read32(palette_region+4)&0xFFFFFF)|(uint32_t(tag)<<24);
        ppc::wr32(c,palette_region+4,word);
        uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word);
    }
    ppc::wr32(c,sdk+0x564+map*4,words[2]); ppc::wr32(c,sdk+0x584+map*4,words[0]);
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|1); ppc::wr16(c,sdk+2,0);
    const bool push=recording_push();
    back([map,source,push] { if (brec) { if (push) brec->native.geometry.push_back(Resource::TextureBinding{map,source}); } else bound_textures[map]=source; });
}
void hle_GXLoadTlut(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80286B80,c); matrix_boundary(0x80286B80);
    const uint32_t object=c.r[3],name=c.r[4],sdk=read32(c.r[2]-8968);
    memory.read(object,12);
    const uint32_t region=uc::call_guest(c,read32(sdk+0x51C),{name});
    memory.read(region,16);
    const uint32_t format=read32(object),image=read32(object+4),count_word=read32(object+8);
    const PaletteSource source{(image&0xFFFFFF)<<5,count_word>>16,gx::PaletteFormat((format>>10)&3)};
    auto loaded=load_palette(source);   // immediate: the guest may reuse the palette's memory once the TMEM load is in the FIFO
    uc::call_guest(c,0x80287720,{});
    uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,image);
    uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,read32(region));
    uc::call_guest(c,0x80287720,{});
    const uint32_t binding=(format&~1023u)|(read32(region)&1023);
    ppc::wr32(c,region+4,binding); ppc::wr32(c,region+8,image); ppc::wr32(c,region+12,count_word);
    ppc::wr32(c,object,binding);
    const bool push=recording_push();
    back([region=region&0x3FFFFFFF,source,loaded=std::move(loaded),push] {
        if (brec) { if (push) brec->native.geometry.push_back(Resource::PaletteLoad{region,source}); }
        else palettes[region]=loaded;
    });
}
void hle_GXSetCopyClear(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802855C8,c); matrix_boundary(0x802855C8);
    const auto bytes=memory.read(c.r[3],4);
    CopyState next;
    for (size_t i=0;i<4;++i) next.color[i]=std::to_integer<uint8_t>(bytes[i]);
    next.depth=c.r[4]&0xFFFFFF;
    const uint32_t sdk=read32(c.r[2]-8968); memory.read(sdk+2,2);
    if (front_list) {
        const uint32_t words[]={0x4F000000u|next.color[0]|uint32_t(next.color[3])<<8,0x50000000u|next.color[2]|uint32_t(next.color[1])<<8,0x51000000u|next.depth};
        for (uint32_t word : words) { uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word); }
        if (recording_push()) back([next] { brec->native.geometry.push_back(Resource::CopyClear{next}); });
    } else back([next] { copy_state=next; });
    ppc::wr16(c,sdk+2,0);
}
void hle_GXCopyDisp(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285838,c);
    if (front_list) throw gx::Error(gx::ErrorCode::sequence,0x80285838,0,1); // TD-034: recorded copies need a verified native resource boundary
    finish_primitive();
    const uint32_t xfb=c.r[3];
    if (xfb&31) throw gx::Error(gx::ErrorCode::layout,xfb,32,xfb&31);
    memory.read(xfb,32);
    const uint32_t sdk=read32(c.r[2]-8968);
    const bool clear=c.r[4]!=0;
    ppc::wr32(c,sdk+0x23C,(read32(sdk+0x23C)&~0xFF000800u)|0x52004000u|(clear ? 0x800u : 0));
    memory.read(sdk+2,2); ppc::wr16(c,sdk+2,0);
    const auto origin=read32(sdk+0x230),extent=read32(sdk+0x234);
    std::array<uint32_t,4> source{0,0,640,480};
    if ((origin>>24)==0x49 && (extent>>24)==0x4A) source={origin&1023,(origin>>10)&1023,(extent&1023)+1,((extent>>10)&1023)+1};
    if(source[0]>=640 || source[2]>640-source[0] || source[1]>=480 || source[3]>480-source[1]) throw std::invalid_argument("invalid native display source rectangle");
    next_seal();
    back([inputs=seal_inputs(),clear,xfb,source,black=uc::vi_stats().black] { submit_batch(inputs,true,clear,xfb,{},source,black); });
    ++copies;
}
void hle_GXSetTexCopySrc(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285334,c); finish_primitive();
    const auto x=c.r[3],y=c.r[4],width=c.r[5],height=c.r[6],sdk=read32(c.r[2]-8968);
    if(!width || !height || x>=640 || width>640-x || y>=480 || height>480-y) throw std::invalid_argument("invalid native EFB texture source rectangle");
    memory.read(sdk+0x240,8);
    texture_copy_source={x,y,width,height};
    ppc::wr32(c,sdk+0x240,0x49000000u|x|y<<10);
    ppc::wr32(c,sdk+0x244,0x4A000000u|(width-1)|(height-1)<<10);
}
void hle_GXSetTexCopyDst(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285398,c); finish_primitive();
    const auto width=c.r[3],height=c.r[4],format=c.r[5],half=c.r[6],sdk=read32(c.r[2]-8968);
    if(!width || !height || width>1024 || height>1024 || (format!=4 && format!=6 && format!=0x27) || half>1) {
        std::fprintf(stderr,"[efb-destination-invalid] %ux%u format %08X half %u caller %08X\n",width,height,format,half,c.lr);
        throw std::invalid_argument("unsupported native EFB texture destination");
    }
    memory.read(sdk+0x248,9);
    // Original SDK tile stride in32byte units: RGBA8=4x4/64; RGB565=4x4/32; A8=8x4/32.
    const auto stride=format==0x27 ? (width+7)/8 : ((width+3)/4)*(format==6 ? 2 : 1);
    const auto copy_type=format==0x27 ? 0x70u : format<<4;
    ppc::wr32(c,sdk+0x248,0x4D000000u|stride);
    ppc::wr32(c,sdk+0x24C,(read32(sdk+0x24C)&~0x18278u)|0x10000u|half<<9|copy_type);
    ppc::wr8(c,sdk+0x250,0);
    texture_copy_width=width; texture_copy_height=height;
    texture_copy_format=format;
}
void hle_GXCopyTex(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80285974,c); finish_primitive();
    const auto address=c.r[3],sdk=read32(c.r[2]-8968);
    if((address&31) || c.r[4]>1 || !texture_copy_source[2] || !texture_copy_width || !texture_copy_height) throw std::invalid_argument("invalid native EFB texture copy request");
    memory.read(address,gx::encoded_texture_size({texture_copy_width,texture_copy_height,texture_copy_format==0x27 ? gx::TextureFormat::i8 : gx::TextureFormat(texture_copy_format),1}));
    memory.read(sdk,0x254);
    const auto control=(read32(sdk+0x24C)&~0xFF004800u)|0x52000000u|c.r[4]<<11;
    if(profile_mesh) back([clear=c.r[4],source=texture_copy_source,width=texture_copy_width,height=texture_copy_height,blend=read32(sdk+0x220),depth=read32(sdk+0x228)] {
        if(!mesh_profile_count || copy_profile_count>=24) return;
        ++copy_profile_count;
        std::fprintf(stderr,"[scene-copy] clear %u source %u:%u:%u:%u target %u:%u color %u:%u:%u:%u blend %08X depth %08X\n",clear,source[0],source[1],source[2],source[3],width,height,copy_state.color[0],copy_state.color[1],copy_state.color[2],copy_state.color[3],blend,depth);
    });
    if(front_list) {
        if(producer()!=front_list->owner || recording_primitive()) throw std::invalid_argument("invalid recorded EFB texture copy boundary");
        if(c.r[4]) for(auto word:{read32(sdk+0x228)|15,read32(sdk+0x220)&~3u}) { uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word); }
        for(auto word:{read32(sdk+0x240),read32(sdk+0x244),read32(sdk+0x248),0x4B000000u|((address&0x3FFFFFFF)>>5),control}) { uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word); }
        if(c.r[4]) for(auto word:{read32(sdk+0x228),read32(sdk+0x220)}) { uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word); }
        if(recording_push()) {
            front_list->effects.push_back({FrontEffect::texture_copy});
            back([copy=Resource::TextureCopy{texture_copy_source,address,texture_copy_width,texture_copy_height,c.r[4]!=0,texture_copy_format}] { brec->native.geometry.push_back(copy); });
        }
        ppc::wr32(c,sdk+0x24C,control); ppc::wr16(c,sdk+2,0);
        return;
    }
    next_seal();
    back([inputs=seal_inputs(),address,width=texture_copy_width,height=texture_copy_height,format=texture_copy_format,source=texture_copy_source,clear=c.r[4]!=0] {
        auto target=make_copy_texture(address,width,height,format);
        const auto [entry,inserted]=copied_textures.try_emplace(address&0x3FFFFFFF);
        try { submit_batch(inputs,false,clear,0,gx::TextureCopy{source,target}); }
        catch(...) { if(inserted) copied_textures.erase(entry); throw; }
        entry->second=std::move(target);
    });
    ppc::wr32(c,sdk+0x24C,control);
    ppc::wr16(c,sdk+2,0); ++copies;
}
namespace {
void publish_blend(ppc::Ctx& c,uint32_t sdk,uint32_t word) {
    uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,word);
    ppc::wr32(c,sdk+0x220,word); ppc::wr16(c,sdk+2,0);
    gx::BlendState blend{uint8_t(word&0x800 ? 3 : word&2 ? 2 : word&1 ? 1 : 0),uint8_t((word>>8)&7),uint8_t((word>>5)&7),uint8_t((word>>12)&15),bool(word&8),bool(word&16)};
    const bool push=recording_push();
    back([blend,push] { if (brec) { if (push) brec->native.geometry.push_back(Resource::Blend{blend}); } else blend_state=blend; });
}
}
void hle_GXSetBlendMode(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80288274,c); matrix_boundary(0x80288274);
    if (c.r[3]>3 || c.r[4]>7 || c.r[5]>7 || c.r[6]>15) throw gx::Error(gx::ErrorCode::layout,0x80288274,3,c.r[3]);
    const auto sdk=read32(c.r[2]-8968),mode=c.r[3];
    const auto word=(read32(sdk+0x220)&~0xF7E3u)|(mode&1)|(mode==2 ? 2u : 0u)|(mode==3 ? 0x800u : 0u)|(c.r[4]<<8)|(c.r[5]<<5)|(c.r[6]<<12);
    publish_blend(c,sdk,word);
}
void hle_GXSetColorUpdate(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802882C4,c); matrix_boundary(0x802882C4);
    if (c.r[3]>1) throw gx::Error(gx::ErrorCode::layout,0x802882C4,1,c.r[3]);
    const auto sdk=read32(c.r[2]-8968); publish_blend(c,sdk,(read32(sdk+0x220)&~8u)|(c.r[3]<<3));
}
void hle_GXSetAlphaUpdate(ppc::Ctx& c) {
    uc::gx_audit_enter(0x802882F0,c); matrix_boundary(0x802882F0);
    if (c.r[3]>1) throw gx::Error(gx::ErrorCode::layout,0x802882F0,1,c.r[3]);
    const auto sdk=read32(c.r[2]-8968); publish_blend(c,sdk,(read32(sdk+0x220)&~16u)|(c.r[3]<<4));
}
void hle_ResTevApply(ppc::Ctx& c) {
    matrix_boundary(0x80366E78);
    const auto object=read32(c.r[3]),sdk=read32(c.r[2]-8968);
    memory.read(object,32); memory.read(sdk,0x600);
    const auto count=std::to_integer<uint8_t>(memory.read(object+12,1)[0]);
    const auto bytes=memory.read(object+32,gx::tev_material_size(count));
    gx::TevMaterial parsed;
    try { parsed=gx::import_tev_material(count,bytes); }
    catch(const std::invalid_argument&) {
        std::fprintf(stderr,"[native-tev-invalid] object %08X stages %u prefix",object,count);
        for(auto byte:bytes) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n"); throw;
    }
    {
        std::array<uint32_t,16> stages{};
        for(uint32_t i=0;i<count && i<16;++i) stages[i]=parsed.stage[i].indirect;
        back([order=parsed.indirect_order,stages,count] {
            color_uniforms.indirect[9]=order; dirty_material();
            for(uint32_t i=0;i<count && i<16;++i) { indirect_stages[i]=stages[i]; dirty_material(); }
        });
    }
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x3C00u)|((count-1)<<10));
    for(size_t i=0;i<4;++i) {
        for(size_t lane=0;lane<2;++lane) {
            const auto at=sdk+0x200+uint32_t(i*8+lane*4);
            ppc::wr32(c,at,(read32(at)&~15u)|((parsed.swaps[i]>>(lane*4))&15));
        }
    }
    for(uint32_t i=0;i<count;++i) {
        const auto& stage=parsed.stage[i];
        ppc::wr32(c,sdk+0x180+i*4,(0xC0+i*2)<<24|stage.color);
        ppc::wr32(c,sdk+0x1C0+i*4,(0xC1+i*2)<<24|stage.alpha);
        const auto at=sdk+0x150+i/2*4,shift=(i%2)*12;
        ppc::wr32(c,at,(read32(at)&~(0xFFFu<<shift))|(stage.order<<shift));
        const auto selector=sdk+0x200+i/2*4,bits=4+(i%2)*10;
        ppc::wr32(c,selector,(read32(selector)&~(1023u<<bits))|(stage.konst<<bits));
    }
}
void hle_ResColorApply(ppc::Ctx& c) {
    matrix_boundary(0x80367024);
    const auto bytes=memory.read(read32(c.r[3]),128);
    const auto sdk=read32(c.r[2]-8968);
    memory.read(sdk,4);
    gx::ColorMaterial parsed;
    try { parsed=gx::import_color_material(bytes); }
    catch(const std::invalid_argument&) {
        std::fprintf(stderr,"[native-color-invalid] hex");
        for(auto byte:bytes) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n"); throw;
    }
    if(front_list) {
        uc::gx_fifo_store(1,0x40); uc::gx_fifo_store(4,read32(c.r[3])); uc::gx_fifo_store(4,128);
    }
    const bool push=recording_push();
    back([parsed,push] {
        for(uint32_t i=0;i<3;++i) {
            color_uniforms.registers[i+1]=parsed.registers[i]; dirty_material();
            if(brec && push) brec->native.geometry.push_back(Resource::TevColor{i+1,false,parsed.registers[i]});
        }
        for(uint32_t i=0;i<4;++i) {
            color_uniforms.konst[i]=parsed.konst[i]; dirty_material();
            if(brec && push) brec->native.geometry.push_back(Resource::TevColor{i,true,parsed.konst[i]});
        }
    });
    ppc::wr16(c,sdk+2,0);
}
void hle_ResPixelApply(ppc::Ctx& c) {
    matrix_boundary(0x80366FF4);
    const auto object=read32(c.r[3]),sdk=read32(c.r[2]-8968);
    const auto pixel=gx::import_pixel_material(memory.read(object,32));
    memory.read(sdk,0x600);
    back([pixel] { color_uniforms.pixel={pixel.alpha,pixel.depth,pixel.destination_alpha,0}; dirty_material(); });
    ppc::wr32(c,sdk+0x228,0x40000000|pixel.depth);
    ppc::wr32(c,sdk+0x224,0x42000000|pixel.destination_alpha);
    const auto blend=(read32(sdk+0x220)&~0xFFE3u)|(pixel.blend&0xFFE3u);
    publish_blend(c,sdk,blend);
}
void hle_GXSetAlphaCompare(ppc::Ctx& c) {
    matrix_boundary(0x80287B80);
    if(c.r[3]>7 || c.r[4]>255 || c.r[5]>3 || c.r[6]>7 || c.r[7]>255) throw std::invalid_argument("invalid native alpha compare");
    const auto word=c.r[4]|c.r[7]<<8|c.r[3]<<16|c.r[6]<<19|c.r[5]<<22;
    back([word] { color_uniforms.pixel[0]=word; dirty_material(); });
    uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,0xF3000000|word);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
namespace {
std::span<const std::byte> mesh_resource(ppc::Ctx& c,uint32_t descriptor,uint32_t skip) {
    const auto object=read32(c.r[3]); memory.read(object,48);
    const auto meta=object+descriptor,size=read32(meta+4),offset=read32(meta+8);
    const int64_t address=int64_t(meta)+int32_t(offset)+skip;
    if(!offset || size<skip || address<0 || address>UINT32_MAX) throw std::invalid_argument("invalid native mesh resource span");
    return memory.read(uint32_t(address),size-skip);
}
}
void hle_ResMeshSetup(ppc::Ctx& c) {
    uc::gx_audit_enter(0x80366D3C,c);
    matrix_boundary(0x80366D3C);
    if(front_list) throw std::invalid_argument("recorded native mesh setup requires implementation");
    const auto bytes=mesh_resource(c,24,c.r[5] ? 32 : 0);
    if(bytes.size()<96 || bytes.size()>224 || (bytes.size()&31)) {
        std::fprintf(stderr,"[mesh-setup-variant] object %08X size %zu skip %u hex",read32(c.r[3]),bytes.size(),c.r[5] ? 32u : 0u);
        for(auto byte:bytes.first(std::min<size_t>(bytes.size(),512))) std::fprintf(stderr," %02X",std::to_integer<unsigned>(byte));
        std::fprintf(stderr,"\n");
    }
    const auto parsed=gx::import_mesh_setup(bytes);
    const auto sdk=read32(c.r[2]-8968); memory.read(sdk,0x600);
    if(parsed.descriptors) { ppc::wr32(c,sdk+0x14,parsed.low); ppc::wr32(c,sdk+0x18,parsed.high); }
    ppc::wr32(c,sdk+0x1C,parsed.vat[0]); ppc::wr32(c,sdk+0x3C,parsed.vat[1]); ppc::wr32(c,sdk+0x5C,parsed.vat[2]);
    back([arrays=parsed.arrays,addresses=parsed.addresses,strides=parsed.strides] {
        for(size_t i=0;i<12;++i) if(arrays&(1u<<i)) state.arrays[i]={addresses[i],uint8_t(strides[i])};
        dirty_state();
    });
}
namespace {
uint32_t guest_address(const std::byte* host) {
    if(host>=memory.mem1.data() && host<memory.mem1.data()+memory.mem1.size()) return uint32_t(host-memory.mem1.data());
    return 0x10000000u+uint32_t(host-memory.mem2.data());
}
std::shared_ptr<const CachedMesh> native_mesh(std::span<const std::byte> bytes,const gx::State& layout) {
    const auto key=reinterpret_cast<uintptr_t>(bytes.data());
    auto found=mesh_index.find(key);
    if(found!=mesh_index.end()) {
        const auto& cached=*found->second->mesh;
        // The mesh description and its source arrays are re-read only when a guest store touched their pages since the last comparison,
        // and anyway every 256th hit (net for native writers that miss the barrier).
        const uint32_t guest=guest_address(bytes.data());
        bool clean=cached.skips<256 && cached.bytes.size()==bytes.size() && !gx::written_since(guest,bytes.size(),cached.stamp);
        if(clean) for(const auto& source:cached.sources) if(gx::written_since(source.address,source.bytes.size(),cached.stamp)) { clean=false; break; }
        bool valid=cached.layout.descriptors==layout.descriptors && (clean || gx::same_bytes(bytes,cached.bytes));
        for(size_t attr=0;valid && attr<21;++attr) if(layout.descriptors[attr]!=gx::Mode::none) {
            valid=cached.layout.formats[0][attr]==layout.formats[0][attr];
            if(attr>=9 && (layout.descriptors[attr]==gx::Mode::index8 || layout.descriptors[attr]==gx::Mode::index16)) valid&=cached.layout.arrays[attr-9]==layout.arrays[attr-9];
        }
        if(valid && !clean) for(const auto& source:cached.sources) {
            const auto current=memory.read(source.address,source.bytes.size());
            if(!gx::same_bytes(current,source.bytes)) { valid=false; break; }
        }
        if(valid) {
            if(clean) ++cached.skips; else { cached.stamp=gx::verified_stamp(); cached.skips=0; }
            add_guest_stat(geometry_hits,1); mesh_cache.splice(mesh_cache.begin(),mesh_cache,found->second); return found->second->mesh;
        }
    }
    add_guest_stat(geometry_misses,1);
    auto result=std::make_shared<CachedMesh>(); result->layout=layout;
    result->bytes.assign(bytes.begin(),bytes.end());
    std::array<std::optional<gx::Vertices::SourceRange>,12> sources{};
    gx::Vertices decoder(memory,true);
    for(size_t at=0;at<bytes.size();) {
        const auto opcode=std::to_integer<uint8_t>(bytes[at++]);
        if(!opcode) continue;
        if(opcode==0x30) {
            if(bytes.size()-at<4) throw std::invalid_argument("truncated native mesh matrix binding");
            uint32_t word=0; for(auto byte:bytes.subspan(at,4)) word=word<<8|std::to_integer<uint8_t>(byte); at+=4;
            const auto target=word&0xFFF,format=(word>>12)&15;
            const auto id=target>=0x500 ? 64+(target-0x500)/4 : target/4;
            if((format!=11 && format!=7) || (target&3) || (target<0x500 && id>61) || id>125) throw std::invalid_argument("invalid native mesh texture matrix binding");
            result->operations.push_back(MeshTexture{uint16_t(word>>16),id,format+1});
            continue;
        }
        if(opcode==0x20 || opcode==0x28) {
            if(bytes.size()-at<4) throw std::invalid_argument("truncated native mesh matrix binding");
            uint32_t word=0; for(auto byte:bytes.subspan(at,4)) word=word<<8|std::to_integer<uint8_t>(byte); at+=4;
            const auto target=word&0xFFF;
            if(opcode==0x20) {
                if(((word>>12)&15)!=11 || (target&3)) throw std::invalid_argument("invalid native mesh position matrix binding");
                result->operations.push_back(MeshPosition{uint16_t(word>>16),target/4});
            } else {
                if(((word>>12)&15)!=8 || target<0x400 || target>0x4B7 || (target-0x400)%3) throw std::invalid_argument("invalid native mesh normal matrix binding");
                result->operations.push_back(MeshNormal{uint16_t(word>>16),(target-0x400)/3});
            }
            continue;
        }
        if((opcode&7)!=0 || at+2>bytes.size()) throw std::invalid_argument("unsupported native mesh primitive record");
        const auto count=uint16_t(std::to_integer<uint8_t>(bytes[at])<<8|std::to_integer<uint8_t>(bytes[at+1])); at+=2;
        if(!count) continue;
        if(opcode!=0x80 && opcode!=0x90 && opcode!=0x98 && opcode!=0xA0) throw std::invalid_argument("unsupported native mesh triangle primitive");
        const auto length=gx::encoded_draw_size(layout,opcode,0,count,0x80366E14);
        if(length>bytes.size()-at) throw gx::Error(gx::ErrorCode::truncated,0x80366E14,length,bytes.size()-at);
        decoder.begin(layout,opcode,0,count,0,0x80366E14);
        decoder.write_attribute(0,bytes.subspan(at,length)); at+=length;
        auto draw=decoder.take_draw();
        for(size_t i=0;i<sources.size();++i) if(const auto& incoming=decoder.sources()[i];incoming) {
            auto& range=sources[i];
            const auto begin=range ? std::min(uint64_t(range->address),uint64_t(incoming->address)) : incoming->address;
            const auto end=range ? std::max(uint64_t(range->address)+range->size,uint64_t(incoming->address)+incoming->size) : uint64_t(incoming->address)+incoming->size;
            range=gx::Vertices::SourceRange{uint32_t(begin),size_t(end-begin)};
        }
        auto* previous=result->operations.empty() ? nullptr : std::get_if<MeshGeometry>(&result->operations.back());
        if(previous) {
            auto& merged=*previous->buffers;
            if(draw.vertices.size()>size_t(INT32_MAX)-merged.vertices.size()) throw std::invalid_argument("native mesh batch exceeds vertex index limits");
            const auto offset=uint32_t(merged.vertices.size());
            merged.vertices.insert(merged.vertices.end(),draw.vertices.begin(),draw.vertices.end());
            for(auto index:draw.indices) merged.indices.push_back(offset+index);
        } else result->operations.push_back(MeshGeometry{std::make_shared<gx::MeshBuffers>(gx::MeshBuffers{std::move(draw.vertices),std::move(draw.indices)})});
    }
    for(const auto& range:sources) if(range) {
        const auto current=memory.read(range->address,range->size);
        result->sources.push_back({range->address,{current.begin(),current.end()}});
    }
    result->stamp=gx::verified_stamp();
    result->size=sizeof(CachedMesh)+result->bytes.capacity()+result->operations.capacity()*sizeof(decltype(result->operations)::value_type)+result->sources.capacity()*sizeof(CachedMesh::Source);
    for(const auto& source:result->sources) result->size+=source.bytes.capacity();
    for(const auto& operation:result->operations) if(const auto* geometry=std::get_if<MeshGeometry>(&operation)) result->size+=geometry->buffers->vertices.capacity()*sizeof(gx::Vertex)+geometry->buffers->indices.capacity()*sizeof(uint32_t);
    if(found!=mesh_index.end()) { mesh_cache_bytes-=found->second->mesh->size; mesh_cache.erase(found->second); mesh_index.erase(found); }
    if(result->size<=mesh_cache_budget) {
        while(mesh_cache_bytes>mesh_cache_budget-result->size) { mesh_cache_bytes-=mesh_cache.back().mesh->size; mesh_index.erase(mesh_cache.back().key); mesh_cache.pop_back(); }
        mesh_cache.push_front({key,result});
        try { mesh_index.emplace(key,mesh_cache.begin()); }
        catch(...) { mesh_cache.pop_front(); throw; }
        mesh_cache_bytes+=result->size;
    }
    return result;
}
// The builder's half of hle_ResMeshDraw: the mesh bytes at `address` as the front checked them, the SDK block as the call saw it.
void mesh_draw(const SdkRef& block,uint32_t lr,uint32_t object,uint32_t address,uint32_t size) {
    const auto bytes=memory.read(address,size);
    const auto layout=snapshot(block,lr);
    const auto misses_before=geometry_misses.load();
    const auto cached=native_mesh(bytes,layout);
    if(replaying) {
        // Matrix loads affect later draws; immutable vertex buffers and material packets do not.
        for(const auto& operation:cached->operations) {
            if(const auto* texture=std::get_if<MeshTexture>(&operation)) {
                indexed_texture(texture_matrices,transforms,layout,texture->index,texture->id,texture->words); dirty_material();
            } else if(const auto* position=std::get_if<MeshPosition>(&operation)) {
                indexed_position(transforms,texture_matrices,layout,position->index,position->row); dirty_material();
            } else if(const auto* normal=std::get_if<MeshNormal>(&operation)) {
                indexed_normal(transforms,layout,normal->index,normal->row);
            } else {
                add_guest_stat(vertex_count,std::get<MeshGeometry>(operation).buffers->vertices.size()); add_guest_stat(draw_count,1);
            }
        }
        return;
    }
    auto [material,uniforms]=material_snapshot(block,lr);
    std::array<std::shared_ptr<const gx::TexturePixels>,8> textures{};
    const bool profile_this_mesh=profile_mesh && mesh_profile_count<256 && geometry_misses.load()!=misses_before;
    if(profile_this_mesh) {
        ++mesh_profile_count;
        auto word=[&](uint32_t offset) { return block->word(offset); };
        std::fprintf(stderr,"[mesh-material] object %08X stages %u channels %u descriptors %08X:%08X ambient %08X:%08X material %08X:%08X control %08X:%08X:%08X:%08X\n",object,material.stages,(word(0x254)>>4)&7,word(0x14),word(0x18),word(0xA8),word(0xAC),word(0xB0),word(0xB4),word(0xB8),word(0xBC),word(0xC0),word(0xC4));
        for(uint32_t i=0;i<material.stages;++i) std::fprintf(stderr,"[mesh-stage] %u color %06X alpha %06X order %03X konst %03X indirect %06X\n",i,material.stage[i].color,material.stage[i].alpha,material.stage[i].order,material.stage[i].konst,material.stage[i].indirect);
        for(uint32_t i=0;i<8;++i) {
            const auto& light=uniforms.lights[i];
            std::fprintf(stderr,"[mesh-light] %u rgba %.6g:%.6g:%.6g:%.6g cosine %.6g:%.6g:%.6g distance %.6g:%.6g:%.6g position %.6g:%.6g:%.6g direction %.6g:%.6g:%.6g\n",i,light.color[0],light.color[1],light.color[2],light.color[3],light.cosine[0],light.cosine[1],light.cosine[2],light.distance[0],light.distance[1],light.distance[2],light.position[0],light.position[1],light.position[2],light.direction[0],light.direction[1],light.direction[2]);
        }
        for(uint32_t i=0;i<8;++i) if(bound_textures[i]) {
            const auto& rows=uniforms.tex_rows;
            std::fprintf(stderr,"[mesh-map] %u source %08X extent %u:%u format %u texgen %08X rows %.6g:%.6g:%.6g:%.6g %.6g:%.6g:%.6g:%.6g\n",i,bound_textures[i]->address,bound_textures[i]->desc.width,bound_textures[i]->desc.height,unsigned(bound_textures[i]->desc.format),uniforms.tex_gens[i],rows[i*3][0],rows[i*3][1],rows[i*3][2],rows[i*3][3],rows[i*3+1][0],rows[i*3+1][1],rows[i*3+1][2],rows[i*3+1][3]);
        }
    }
    const auto maps=texture_mask(material,uniforms);
    for(uint32_t map=0;map<8;++map) if(maps&(1u<<map)) {
        if(!textures[map]) textures[map]=snapshot_texture(bound_textures[map],palettes);
    }
    if(profile_this_mesh && mesh_profile_count==1 && !uc::public_release()) for(size_t map=0;map<2;++map) {   // a release client writes no debug files
        if(textures[map]) textures[map]->wait();
        if(!textures[map] || textures[map]->gpu_only || textures[map]->levels.empty()) continue;
        const auto& level=textures[map]->levels[0];
        try {
            const auto directory=std::filesystem::path("debug");   // profiling dump: relative to the working directory
            std::filesystem::create_directories(directory);
            const auto path=directory/("mesh-map-"+std::to_string(map)+"-"+std::to_string(++mesh_dumps)+".ppm");
            std::ofstream file(path,std::ios::binary);
            file.exceptions(std::ios::badbit|std::ios::failbit);
            file<<"P6\n"<<level.width<<' '<<level.height<<"\n255\n";
            std::vector<uint8_t> rgb; rgb.reserve(size_t(level.width)*level.height*3);
            for(size_t i=0;i<level.rgba.size();i+=4) rgb.insert(rgb.end(),level.rgba.begin()+i,level.rgba.begin()+i+3);
            file.write(reinterpret_cast<const char*>(rgb.data()),std::streamsize(rgb.size())); file.close();
            std::fprintf(stderr,"[mesh-texture-dump] %s\n",path.string().c_str());
        } catch(const std::exception& error) { std::fprintf(stderr,"mesh texture dump failed: %s\n",error.what()); }
    }
    const size_t first_draw=draws.size();   // appended in place; removed again if the mesh fails midway
    // Edited in place: a failure below stops the builder, and the guest thread ends, so the half-updated state is never observed.
    const auto selectors=texture_selectors(*block);
    gx::share(mesh_state,layout);
    bool uniforms_dirty=true,transforms_dirty=true;   // changed since the shared block was last compared (that comparison is a memcmp of 2 KB per draw)
    const auto texture_wrap=wraps(bound_textures);
    try {
        for(const auto& operation:cached->operations) {
            if(const auto* texture=std::get_if<MeshTexture>(&operation)) {
                indexed_texture(texture_matrices,transforms,layout,texture->index,texture->id,texture->words); dirty_material();
                resolve_texture_matrices(uniforms,selectors,texture_matrices);
                uniforms_dirty=transforms_dirty=true;
                continue;
            }
            if(const auto* position=std::get_if<MeshPosition>(&operation)) {
                indexed_position(transforms,texture_matrices,layout,position->index,position->row); dirty_material();
                resolve_texture_matrices(uniforms,selectors,texture_matrices);
                uniforms_dirty=transforms_dirty=true;
            } else if(const auto* normal=std::get_if<MeshNormal>(&operation)) {
                indexed_normal(transforms,layout,normal->index,normal->row);
                transforms_dirty=true;
            } else {
                const auto& geometry=std::get<MeshGeometry>(operation);
                if(uniforms_dirty) { gx::share(mesh_uniforms_block,uniforms); uniforms_dirty=false; }
                auto& draw=draws.emplace_back();
                draw.callsite=0x80366E14; draw.primitive=0x90; draw.vat=0; draw.state=mesh_state;
                draw.mesh=geometry.buffers;
                if(transforms_dirty) { gx::share(mesh_transforms_block,transforms); transforms_dirty=false; }
                draw.transforms=mesh_transforms_block; draw.material=material; draw.uniforms=mesh_uniforms_block; draw.texture_bank=texture_bank(*draw.state,texture_matrices); draw.blend=blend_state; draw.texture=textures[0];
                draw.texture_wrap=texture_wrap;
                std::copy(textures.begin()+1,textures.end(),draw.texture_maps.begin());
            }
        }
    } catch(...) { draws.erase(draws.begin()+std::ptrdiff_t(first_draw),draws.end()); throw; }
    if(profile_this_mesh && draws.size()>first_draw) {
        std::array<uint8_t,4> low_color{255,255,255,255},high_color{};
        for(size_t d=first_draw;d<draws.size();++d) for(const auto& vertex:draws[d].vertex_view()) for(size_t channel=0;channel<4;++channel) {
            low_color[channel]=std::min(low_color[channel],vertex.color[0][channel]); high_color[channel]=std::max(high_color[channel],vertex.color[0][channel]);
        }
        std::fprintf(stderr,"[mesh-color-range] object %08X rgba %u:%u %u:%u %u:%u %u:%u mode %u\n",object,low_color[0],high_color[0],low_color[1],high_color[1],low_color[2],high_color[2],low_color[3],high_color[3],unsigned(layout.descriptors[11]));
        for(size_t map=0;map<2;++map) {
            std::array<float,2> low{INFINITY,INFINITY},high{-INFINITY,-INFINITY};
            for(size_t d=first_draw;d<draws.size();++d) for(const auto& vertex:draws[d].vertex_view()) for(size_t axis=0;axis<2;++axis) {
                low[axis]=std::min(low[axis],vertex.texcoord[map][axis]); high[axis]=std::max(high[axis],vertex.texcoord[map][axis]);
            }
            std::fprintf(stderr,"[mesh-uv-range] map %zu UV %.6g:%.6g %.6g:%.6g wrap %u\n",map,low[0],high[0],low[1],high[1],bound_textures[map] ? bound_textures[map]->wrap : 0);
        }
    }
    for(size_t d=first_draw;d<draws.size();++d) { add_guest_stat(vertex_count,draws[d].vertex_view().size()); add_guest_stat(draw_count,1); }
}
}
void hle_ResMeshDraw(ppc::Ctx& c) {
    CallTimer timer(1);
    uc::gx_audit_enter(0x80366E14,c);
    matrix_boundary(0x80366E14);
    if(front_list) throw std::invalid_argument("recorded native mesh draw requires implementation");
    scheduler->safe_point(); // Service audio/VI between complete native mesh calls.
    const auto bytes=mesh_resource(c,36,0);
    const uint32_t sdk=read32(c.r[2]-8968);
    const auto block=capture_sdk(sdk);
    front_state(block);   // derive_state's errors at the call
    known_sdk=sdk;
    back_checked([block,lr=c.lr,object=c.r[3],address=guest_address(bytes.data()),size=uint32_t(bytes.size())] { mesh_draw(block,lr,object,address,size); });
    if(profile_mesh && !mesh_dumps_settled) {   // until the profile's first mesh: whether it dumped decides the clock reads below
        drain();
        for(;mesh_dumps_seen<mesh_dumps.load();++mesh_dumps_seen) uc::now_ticks();
        mesh_dumps_settled=mesh_profile_count>=1;
    }
}
namespace {
void native_tev_color(ppc::Ctx& c,bool signed_color,bool konst) {
    matrix_boundary(konst ? 0x802879CC : signed_color ? 0x80287968 : 0x80287908);
    const auto id=c.r[3],pointer=c.r[4],sdk=read32(c.r[2]-8968);
    if(id>3) throw gx::Error(gx::ErrorCode::layout,pointer,3,id);
    memory.read(pointer,signed_color ? 8 : 4); memory.read(sdk,4);
    std::array<float,4> value{}; std::array<uint32_t,4> channels{};
    for(uint32_t i=0;i<4;++i) {
        const auto v=signed_color ? int32_t(int16_t(ppc::rd16(c,pointer+i*2))) : int32_t(ppc::rd8(c,pointer+i));
        if(signed_color && (v< -1024 || v>1023)) throw gx::Error(gx::ErrorCode::layout,pointer,1023,size_t(v));
        value[i]=v/255.f; channels[i]=uint32_t(v)&0x7FF;
    }
    const auto low=((0xE0+id*2)<<24)|(konst ? 0x800000u : 0u)|channels[0]|channels[3]<<12;
    const auto high=((0xE1+id*2)<<24)|(konst ? 0x800000u : 0u)|channels[2]|channels[1]<<12;
    uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,low);
    for(size_t i=0;i<(konst ? 1 : 3);++i) { uc::gx_fifo_store(1,0x61); uc::gx_fifo_store(4,high); }
    const bool push=recording_push();
    back([id,konst,value,push] {
        if(konst) color_uniforms.konst[id]=value; else color_uniforms.registers[id]=value;
        dirty_material();
        if(brec && push) brec->native.geometry.push_back(Resource::TevColor{id,konst,value});
    });
    ppc::wr16(c,sdk+2,0);
}
}
void hle_GXSetTevColor(ppc::Ctx& c) { native_tev_color(c,false,false); }
void hle_GXSetTevColorS10(ppc::Ctx& c) { native_tev_color(c,true,false); }
void hle_GXSetTevKColor(ppc::Ctx& c) { native_tev_color(c,false,true); }
void hle_GXSetTevDirect(ppc::Ctx& c) {
    matrix_boundary(0x80287650);
    const auto stage=c.r[3],sdk=read32(c.r[2]-8968);
    if(stage>15) throw gx::Error(gx::ErrorCode::layout,0x80287650,15,stage);
    native_bp((0x10+stage)<<24);
    ppc::wr16(c,sdk+2,0);
}
// Fog and indirect setters, call-level replacements of the RBUE08 SDK bodies (bit layouts from their disassembly).
// The FIFO words stay byte-identical for display lists; native state comes from the arguments, never from FIFO decoding.
void hle_GXSetTevIndirect(ppc::Ctx& c) {
    matrix_boundary(0x802872A4);
    const auto stage=c.r[3],utc_lod=ppc::rd8(c,c.r[1]+11),alpha=read32(c.r[1]+12); // 9th/10th arguments on the caller's stack
    if(stage>15) throw gx::Error(gx::ErrorCode::layout,0x802872A4,15,stage);
    native_bp((0x10+stage)<<24|(c.r[4]&3)|(c.r[5]&3)<<2|(c.r[6]&7)<<4|(alpha&3)<<7|(c.r[7]&15)<<9|(c.r[8]&7)<<13|(c.r[9]&7)<<16|(utc_lod&1u)<<19|(c.r[10]&1)<<20);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
void hle_GXSetIndTexMtx(ppc::Ctx& c) {
    matrix_boundary(0x8028730C);
    const auto id=c.r[3];
    const uint32_t index=(id>=1 && id<=3) ? id-1 : (id>=5 && id<=7) ? id-5 : (id>=9 && id<=11) ? id-9 : 0;
    memory.read(c.r[4],24);
    // offset[2][3] row-major; the SDK scales by 1024.0f (SDA2 constant) and truncates like fctiwz.
    auto fixed=[&](uint32_t i) {
        const float scaled=std::bit_cast<float>(read32(c.r[4]+i*4))*1024.0f;
        const float clamped=std::isnan(scaled) ? 0.f : std::clamp(scaled,-2147483648.f,2147483520.f);
        return uint32_t(int32_t(clamped))&0x7FF;
    };
    const uint32_t exponent=uint32_t(int32_t(c.r[5])+17);
    for(uint32_t column=0;column<3;++column)
        native_bp((index*3+6+column)<<24|fixed(column)|fixed(3+column)<<11|((exponent>>(column*2))&3)<<22);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
void hle_GXSetIndTexCoordScale(ppc::Ctx& c) {
    matrix_boundary(0x80287460);
    const auto stage=c.r[3],sdk=read32(c.r[2]-8968);
    if(stage<4) {
        const uint32_t at=sdk+(stage<2 ? 0x178 : 0x17C),shift=(stage&1)*8;
        const auto word=(read32(at)&~(0xFFu<<shift)&0xFFFFFF)|(c.r[4]&15)<<shift|(c.r[5]&15)<<(shift+4)|(stage<2 ? 0x25u : 0x26u)<<24;
        ppc::wr32(c,at,word);
        native_bp(word);
    }
    ppc::wr16(c,sdk+2,0);
}
void hle_GXSetIndTexOrder(ppc::Ctx& c) {
    matrix_boundary(0x80287564);
    const auto stage=c.r[3],sdk=read32(c.r[2]-8968);
    const uint32_t coord=c.r[4]==255 ? 0 : c.r[4],map=c.r[5]==255 ? 0 : c.r[5];
    auto word=read32(sdk+0x170);
    if(stage<4) word=(word&~(0x3Fu<<(stage*6)))|(map&7)<<(stage*6)|(coord&7)<<(stage*6+3);
    ppc::wr32(c,sdk+0x170,word);
    native_bp(word);
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|3);
    ppc::wr16(c,sdk+2,0);
}
namespace {
uint32_t fog_float(float value) { const auto bits=std::bit_cast<uint32_t>(value); return ((bits>>12)&0x7FF)|((bits>>23)&0xFF)<<11|(bits>>31)<<19; }
}
// GXSetFog (80287DC4): single-precision steps and constants as in the SDK body (0, 0.5, 1, 2, 8388638 from SDA2).
void hle_GXSetFog(ppc::Ctx& c) {
    matrix_boundary(0x80287DC4);
    const auto type=c.r[3];
    const float start=float(c.f[1]),end=float(c.f[2]),nearz=float(c.f[3]),farz=float(c.f[4]);
    const auto color=read32(c.r[4]);
    float a=0,cvalue=0; uint32_t b_word=0,e_word=0; // ortho leaves both B words zero
    if(type&8) {
        if(farz!=nearz && end!=start) {
            const float scale=1.0f/(end-start);
            a=scale*(farz-nearz); cvalue=scale*(start-nearz);
        }
    } else {
        float b=0.5f,a_num=0;
        if(farz!=nearz && end!=start) {
            const float depth=farz-nearz,range=end-start;
            a_num=farz*nearz; b=farz/depth; a_num=a_num/(depth*range); cvalue=start/range;
        }
        int exponent=0;
        while(double(b)>1.0) { b*=0.5f; ++exponent; }
        while(b>0.0f && double(b)<0.5) { b*=2.0f; --exponent; }
        a=a_num/float(int32_t(1u<<(exponent+1)));
        const float mantissa=8388638.0f*b;
        b_word=0xEF000000|(uint32_t(std::max(mantissa,0.f))&0xFFFFFF);
        e_word=0xF0000000|(uint32_t(exponent+1)&31);
    }
    native_bp(0xEE000000|fog_float(a));
    native_bp(b_word); native_bp(e_word);
    native_bp(0xF1000000|fog_float(cvalue)|((type>>3)&1)<<20|(type&7)<<21);
    native_bp(0xF2000000|color>>8);
    ppc::wr16(c,read32(c.r[2]-8968)+2,0);
}
void hle_ResChannelApply(ppc::Ctx& c) {
    matrix_boundary(0x80376148);
    if(front_list) throw std::invalid_argument("recorded native channel resource requires implementation");
    const auto data=read32(c.r[3]),sdk=read32(c.r[2]-8968);
    struct { std::array<std::pair<uint32_t,uint32_t>,16> items; size_t size=0;
        void emplace_back(uint32_t address,uint32_t value) { items[size++]={address,value}; }   // at most 13 updates
        auto begin() const { return items.begin(); } auto end() const { return items.begin()+size; } } updates;
    if(data) {
        memory.read(data,40); memory.read(sdk,0x600); memory.read(c.r[8],4);
        const auto count=ppc::rd8(c,0x806A0041);
        if(count>2) throw std::invalid_argument("invalid native material channel count");
        for(uint32_t channel=0;channel<count;++channel) {
            const auto at=data+channel*20,flags=read32(at);
            if(flags&3) updates.emplace_back(sdk+0xB0+channel*4,read32(at+4));
            if(flags&12) {
                auto ambient=read32(at+8);
                if(!channel) {
                    const auto multiplier=read32(c.r[8]); uint32_t value=0;
                    for(uint32_t lane=0;lane<4;++lane) {
                        const auto shift=24-lane*8;
                        value|=((((ambient>>shift)&255)*((multiplier>>shift)&255)+127)/255)<<shift;
                    }
                    ambient=value;
                }
                updates.emplace_back(sdk+0xA8+channel*4,ambient);
            }
            for(uint32_t alpha=0;alpha<2;++alpha) if(flags&(16u<<alpha)) {
                const auto mask=c.r[4+channel*2+alpha];
                if(mask>255) throw std::invalid_argument("invalid native material light mask");
                const auto control=(read32(at+12+alpha*4)&(c.r[9] ? 0xFFFF87C1u : 0xFFFF87C3u))|((mask&15)<<2)|((mask&240)<<7);
                updates.emplace_back(sdk+0xB8+channel*4+alpha*8,control);
            }
        }
        if(count<2) { updates.emplace_back(sdk+0xBC,0); updates.emplace_back(sdk+0xC4,0); }
        updates.emplace_back(sdk+0x254,(read32(sdk+0x254)&~0x70u)|(uint32_t(count)<<4));
    }
    if(profile_mesh && channel_profile_count<32) {
        ++channel_profile_count;
        const auto data=read32(c.r[3]);
        if(data) {
            memory.read(data,40); memory.read(c.r[8],4);
            std::fprintf(stderr,"[native-channel] data %08X masks %u:%u ambient %08X disable %u count %u words",data,c.r[4],c.r[5],read32(c.r[8]),c.r[9],unsigned(ppc::rd8(c,0x806A0041)));
            for(uint32_t i=0;i<10;++i) std::fprintf(stderr," %08X",read32(data+i*4));
            std::fprintf(stderr,"\n");
        }
    }
    if(data) {
        const auto mode=0x806A0040u,flags=read32(mode+8);
        if((flags&4) && (flags&3)!=3) {
            const auto tex=ppc::rd8(c,mode),channels=ppc::rd8(c,mode+1),tev=ppc::rd8(c,mode+2),indirect=ppc::rd8(c,mode+3),cull=read32(mode+4);
            if(tex>8 || channels>2 || !tev || tev>16 || indirect>4 || cull>3) throw std::invalid_argument("invalid native material generation mode");
            memory.read(c.r[2]-5488+cull,1);
            const auto word=uint32_t(tex)|(uint32_t(channels)<<4)|(uint32_t(tev-1)<<10)|(uint32_t(indirect)<<16)|(uint32_t(ppc::rd8(c,c.r[2]-5488+cull))<<14);
            updates.emplace_back(sdk+0x254,(read32(sdk+0x254)&~0x7FC3Fu)|(word&0x7FC3Fu));
            updates.emplace_back(mode+8,flags|3);
        }
    }
    for(const auto& [address,value]:updates) ppc::wr32(c,address,value);
}
void hle_ResGenerationMode(ppc::Ctx& c) {
    matrix_boundary(0x80373D00);
    const auto tex=c.r[3],channels=c.r[4],tev=c.r[5],indirect=c.r[6],cull=c.r[7],sdk=read32(c.r[2]-8968);
    if(tex>8 || channels>2 || !tev || tev>16 || indirect>4 || cull>3) throw std::invalid_argument("invalid native resource generation mode");
    memory.read(sdk,0x600); memory.read(c.r[2]-5488+cull,1);
    const auto native_cull=ppc::rd8(c,c.r[2]-5488+cull);
    if(native_cull>3) throw std::invalid_argument("invalid native cull conversion");
    const auto word=tex|(channels<<4)|((tev-1)<<10)|(indirect<<16)|(uint32_t(native_cull)<<14);
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x7FC3Fu)|(word&0x7FC3Fu));
    if(recording_push()) { front_effect({FrontEffect::num_channels,channels}); back([channels] { brec->native.geometry.push_back(Resource::NumChannels{channels}); }); }
}
void hle_GXSetNumChans(ppc::Ctx& c) {
    matrix_boundary(0x80286190);
    const auto count=c.r[3],sdk=read32(c.r[2]-8968);
    if(count>2) throw std::invalid_argument("invalid native color channel count");
    memory.read(sdk,0x600);
    ppc::wr32(c,sdk+0x254,(read32(sdk+0x254)&~0x70u)|(count<<4));
    ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|0x01000004u);
    if(recording_push()) { front_effect({FrontEffect::num_channels,count}); back([count] { brec->native.geometry.push_back(Resource::NumChannels{count}); }); }
}
void hle_GXSetChanCtrl(ppc::Ctx& c) {
    matrix_boundary(0x802861B4);
    const auto channel=c.r[3],enable=c.r[4],ambient=c.r[5],material=c.r[6],mask=c.r[7],diffuse=c.r[8],attenuation=c.r[9],sdk=read32(c.r[2]-8968);
    if(channel>5 || enable>1 || ambient>1 || material>1 || mask>255 || diffuse>2 || attenuation>2) throw std::invalid_argument("invalid native color channel control");
    memory.read(sdk,0x600);
    const uint32_t attenuation_modes[]={1,3,2};
    const auto control=material|(enable<<1)|((mask&15)<<2)|(ambient<<6)|((attenuation ? diffuse : 0)<<7)|(attenuation_modes[attenuation]<<9)|((mask&240)<<7);
    const auto publish=[&](uint32_t index) {
        ppc::wr32(c,sdk+0xB8+index*4,control);
        ppc::wr32(c,sdk+0x5FC,read32(sdk+0x5FC)|(0x1000u<<index));
        if(recording_push()) { front_effect({FrontEffect::channel_control,index,control}); back([index,control] { brec->native.geometry.push_back(Resource::ChannelControl{index,control}); }); }
    };
    publish(channel&3);
    if(channel>=4) publish((channel&1)+2);
}
void hle_GXLoadLightObjImm(ppc::Ctx& c) {
    matrix_boundary(0x80285F64);
    const auto pointer=c.r[3],mask=c.r[4];
    if(!mask || mask>255) throw std::invalid_argument("invalid native light id");
    memory.read(pointer,64); memory.read(read32(c.r[2]-8968),4);
    const auto id=uint32_t(std::bit_width(mask)-1);
    gx::LightUniform light;
    for(uint32_t lane=0;lane<4;++lane) light.color[lane]=float(ppc::rd8(c,pointer+12+lane));
    std::array<float,4>* fields[]={&light.cosine,&light.distance,&light.position,&light.direction};
    for(uint32_t field=0;field<4;++field) for(uint32_t lane=0;lane<3;++lane) {
        const auto value=ppc::rdf32(c,pointer+16+field*12+lane*4);
        if(!std::isfinite(value)) throw std::invalid_argument("nonfinite native light");
        (*fields[field])[lane]=value;
    }
    if(front_list) {
        uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,0x000F0600|id*16);
        for(uint32_t i=0;i<3;++i) uc::gx_fifo_store(4,0);
        for(uint32_t i=3;i<16;++i) uc::gx_fifo_store(4,read32(pointer+i*4));
    }
    const bool push=recording_push();
    back([id,light,push] { color_uniforms.lights[id]=light; dirty_material(); if(brec && push) brec->native.geometry.push_back(Resource::Light{id,light}); });
    ppc::wr16(c,read32(c.r[2]-8968)+2,1);
}
void hle_GXLoadNrmMtxImm(ppc::Ctx& c) {
    matrix_boundary(0x802888A8);
    const auto pointer=c.r[3],row=c.r[4]; memory.read(pointer,44);
    gx::NormalMatrix normal{};
    for(uint32_t i=0;i<9;++i) normal[i/3][i%3]=ppc::rdf32(c,pointer+(i/3)*16+(i%3)*4);
    if (row>61) throw gx::Error(gx::ErrorCode::layout,0x802888A8,61,row);   // gx::set_normal_matrix's check
    if(front_list) {
        uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,0x00080400|row*3);
        for(const auto& values:normal) for(auto value:values) uc::gx_fifo_store(4,std::bit_cast<uint32_t>(value));
        if(recording_push()) back([row,normal] { brec->native.geometry.push_back(Resource::Normal{row,normal}); });
    } else back([row,normal] { gx::set_normal_matrix(transforms,row,normal); });
}
void hle_GXLoadTexMtxImm(ppc::Ctx& c) {
    matrix_boundary(0x80288954);
    const auto pointer=c.r[3],id=c.r[4],type=c.r[5];
    if(type>1 || id>125) throw gx::Error(gx::ErrorCode::layout,pointer,125,id);
    const auto count=type ? 8u : 12u;
    memory.read(pointer,count*4);
    TextureMatrix matrix{};
    for(size_t i=0;i<count;++i) matrix[i/4][i%4]=std::bit_cast<float>(read32(pointer+uint32_t(i*4)));   // non-finite words are guest data, loaded as the hardware does
    const auto address=id>=64 ? 0x500+(id-64)*4 : id*4;
    uc::gx_fifo_store(1,0x10); uc::gx_fifo_store(4,(count-1)<<16|address);
    for(uint32_t i=0;i<count;++i) uc::gx_fifo_store(4,read32(pointer+i*4));
    const bool push=recording_push();
    back([id,matrix,count,push] {
        matrix_rows(texture_matrices,brec ? nullptr : &transforms,id,matrix,count/4); dirty_material();
        if(brec && push) brec->native.geometry.push_back(Resource::TextureTransform{id,matrix,count/4});
    });
}
