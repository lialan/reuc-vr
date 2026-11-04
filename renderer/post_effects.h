#pragma once
#include "gx_vertices.h"

namespace gx {
// The game's full-screen quad helper (callsite 0x800421B8) draws every step of its post-process chain. The bloom chain blurs a half-resolution copy of
// the scene down and back up, then adds the result to the full frame: the only draw of this helper that blends source ONE with destination ONE.
// Found with the packet dump (UC_PACKET_DUMP / UC_PACKET_STATS); skipping it removes the glow around lamps and nothing else.
inline bool bloom_composite(const DrawPacket& draw) {
    return draw.callsite == 0x800421B8 && draw.blend.mode == 1 && draw.blend.source == 1 && draw.blend.destination == 1;
}

// Camera-space refraction overlays: the first stage displaces a scene copy,
// followed only by direct mask/tint stages. In VR these screen-space offsets
// duplicate/displace stereo scene edges. Keep the overlay's colour and masks,
// but sample the scene without that displacement. World particles and the
// bloom chain are separate draws and retain their original shaders.
inline bool vr_screen_refraction(const DrawPacket& draw) {
    if (draw.transforms->projection[3][3]==0 || !draw.material.stages ||
        draw.blend.mode!=1 || draw.blend.source!=4 || draw.blend.destination!=5 ||
        (draw.uniforms->pixel[1]&16) || !draw.uniforms->ind_cmd[0]) return false;
    const uint32_t order=draw.material.stage[0].order;
    if (!(order&64)) return false;
    const uint32_t map=order&7;
    const auto& texture=map ? draw.texture_maps[map-1] : draw.texture;
    if (!texture || !texture->gpu_only) return false;
    for (uint32_t stage=1;stage<draw.material.stages;++stage)
        if (draw.uniforms->ind_cmd[stage]) return false;
    return true;
}
}
