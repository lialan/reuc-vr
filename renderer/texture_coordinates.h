#pragma once
#include "gx_materials.h"

namespace gx {
// generateTexcoord's output Q is exactly one for these regular affine texgens.
// The ST mode forces Q=1 before the post matrix, including indexed texture banks.
// A constant final post row also forces Q=1. Normalization/projected post rows
// keep the general per-fragment divide. Only coordinates consumed by TEV matter.
inline bool unit_texture_q(const DrawUniform& u,uint32_t coordinates) {
    for (uint32_t id=0;id<8;++id) if (coordinates&(1u<<id)) {
        if ((u.tex_gens[id]&(7u<<4)) || (u.tex_posts[id]&256u)) return false;
        const auto& q=u.post_rows[id*3+2];
        if (q[0]!=0 || q[1]!=0) return false;
        if (q[2]==0 && q[3]==1) continue;
        if (!(u.tex_gens[id]&2u) && q[2]==1 && q[3]==0) continue;
        return false;
    }
    return true;
}
}
