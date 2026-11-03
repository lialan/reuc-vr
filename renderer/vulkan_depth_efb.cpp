// UC_DEPTH_STATS=1: what the depth/EFB switches of renderer/depth_efb.h would change, counted from the draws' GX state
// (docs/spikes/S18-dolphin-reference.md, "Done: depth and EFB"). Per 2 s, averaged per presented frame.
#include "vulkan_renderer.h"
#include "depth_efb.h"
#include "late_aim.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <map>

namespace uc {
namespace {
// The draw's clip-space box over its viewport, in 640x360 scene screens (0 when unknown).
double draw_screens(const gx::DrawPacket& draw) {
    const auto box=gx::draw_bounds_ndc(draw);
    if (!box) return 0;
    const auto v=draw.state->viewport.value_or(std::array<float,6>{0,0,640,480,0,1});
    return double(std::clamp((*box)[2],-1.f,1.f)-std::clamp((*box)[0],-1.f,1.f))*.5*std::abs(v[2])*double(std::clamp((*box)[3],-1.f,1.f)-std::clamp((*box)[1],-1.f,1.f))*.5*std::abs(v[3])/(640.0*360.0);
}
}
void VulkanRenderer::log_depth_stats(const gx::FramePacket& packet,size_t scene_draws,const std::function<uint32_t(size_t)>& index_count) {
    struct Second { uint64_t count=0; double screens=0; uint32_t blend=0; };
    struct Stats {
        uint64_t presents=0, draws=0, scene=0, tested=0, precheck_pass=0, precheck_fail=0, ztop=0, ztop_tested=0, ztop_tested_precheck=0, ztop_tested_write=0, late_untested=0;
        uint64_t formats[9]{};   // EFB pixel formats 0..7, unknown
        uint64_t destination=0, fused=0, second=0, no_alpha_writes=0, no_alpha_dst_factor=0, no_alpha_destination=0;
        std::map<uint32_t,Second> seconds;   // callsite -> second destination-alpha draws
        std::map<uint32_t,Second> ztops;     // callsite -> alpha-tested draws asking for early depth (blend: the depth word)
    };
    static Stats stats; static auto since=std::chrono::steady_clock::now();
    stats.presents+=packet.present;
    for (size_t d=0;d<packet.draws.size();++d) {
        if (!index_count(d)) continue;
        const auto& draw=packet.draws[d]; const auto& u=*draw.uniforms;
        ++stats.draws; stats.scene+=d<scene_draws;
        const bool tested=gx::alpha_test_needs_discard(u.pixel[0],false);
        const auto result=gx::alpha_test_result(u.pixel[0]);
        stats.tested+=tested; stats.precheck_pass+=tested && result==gx::AlphaTest::pass; stats.precheck_fail+=tested && result==gx::AlphaTest::fail;
        const uint32_t depth=u.pixel[1];
        ++stats.formats[gx::pe_control_known(depth) ? gx::pe_pixel_format(depth) : 8];
        if (gx::pe_z_top(depth)) {
            ++stats.ztop;
            if (tested) {
                ++stats.ztop_tested; stats.ztop_tested_precheck+=result!=gx::AlphaTest::pass; stats.ztop_tested_write+=(depth&17)==17;
                auto& entry=stats.ztops[draw.callsite]; ++entry.count; entry.blend=depth&31; entry.screens+=draw_screens(draw);
            }
        } else stats.late_untested+=gx::pe_control_known(depth) && !tested;
        const bool destination=(u.pixel[2]&256) && draw.blend.alpha_write;
        if (destination) {
            ++stats.destination;
            const bool never=(depth&1) && ((depth>>1)&7)==0;
            if (fused_destination_alpha(draw)) ++stats.fused;
            else if (!never) {
                ++stats.second;
                auto& entry=stats.seconds[draw.callsite]; ++entry.count; entry.blend=uint32_t(draw.blend.mode)|uint32_t(draw.blend.source)<<4|uint32_t(draw.blend.destination)<<8;
                entry.screens+=draw_screens(draw);
            }
        }
        if (gx::pe_no_alpha(depth)) {
            stats.no_alpha_writes+=draw.blend.alpha_write;
            stats.no_alpha_destination+=destination;
            stats.no_alpha_dst_factor+=draw.blend.mode==1 && (draw.blend.source>=6 || draw.blend.destination>=6);
        }
    }
    if (std::chrono::steady_clock::now()-since<std::chrono::seconds(2)) return;
    const double p=double(std::max<uint64_t>(stats.presents,1));
    std::fprintf(stderr,"[depth-stats] 2 s, %llu presents, a frame: %.1f draws (%.1f scene); alpha-tested (discard kept) %.1f, of them the pre-check passes %.1f, fails %.1f; "
                 "ztop %.1f (alpha-tested %.1f: undetermined %.1f, depth-writing %.1f); late Z without alpha test %.1f; formats RGB8 %.1f RGBA6 %.1f RGB565 %.1f Z24 %.1f other %.1f unknown %.1f; "
                 "destination alpha %.1f (fused %.1f, second draw %.1f); no-alpha EFB: alpha writes %.1f, destination alpha %.1f, DSTALPHA factors %.1f\n",
                 (unsigned long long)stats.presents,stats.draws/p,stats.scene/p,stats.tested/p,stats.precheck_pass/p,stats.precheck_fail/p,stats.ztop/p,stats.ztop_tested/p,stats.ztop_tested_precheck/p,stats.ztop_tested_write/p,stats.late_untested/p,
                 stats.formats[0]/p,stats.formats[1]/p,stats.formats[2]/p,stats.formats[3]/p,(stats.formats[4]+stats.formats[5]+stats.formats[6]+stats.formats[7])/p,stats.formats[8]/p,
                 stats.destination/p,stats.fused/p,stats.second/p,stats.no_alpha_writes/p,stats.no_alpha_destination/p,stats.no_alpha_dst_factor/p);
    std::fprintf(stderr,"[depth-stats]   EFB clears a frame: %.2f passes of their own, %.2f as a pass's load op, %.2f inside a pass; depth format %d\n",clear_passes_/p,clears_as_load_/p,clears_in_pass_/p,int(depth_format_));
    clear_passes_=clears_as_load_=clears_in_pass_=0;
    for (const auto& [callsite,entry]:stats.seconds)
        std::fprintf(stderr,"[depth-stats]   second destination-alpha draw: callsite %08X %.2f a frame, blend %03X, %.3f scene screens each\n",callsite,entry.count/p,entry.blend,entry.screens/double(entry.count));
    for (const auto& [callsite,entry]:stats.ztops)
        std::fprintf(stderr,"[depth-stats]   alpha-tested early-depth draw: callsite %08X %.2f a frame, depth word %02X, %.3f scene screens each\n",callsite,entry.count/p,entry.blend,entry.screens/double(entry.count));
    stats=Stats{}; since=std::chrono::steady_clock::now();
}
}  // namespace uc
