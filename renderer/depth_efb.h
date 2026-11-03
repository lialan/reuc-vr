#pragma once
// GX pixel-engine state that decides how the host tests depth and keeps alpha (docs/spikes/S18-dolphin-reference.md, recommendations 2 and 3):
// the alpha test evaluated ahead of time from its compare ops and references, BP 0x43 (PE control: EFB pixel format and z compare location),
// and the switches that put them to use. Pure functions, so tests can check them without a GPU.
#include <cstdint>
#include <cstdlib>

namespace gx {

// What a draw's alpha test does to every fragment, known from the compare word alone (DrawUniform pixel[0]: references in bits 0-7 and 8-15,
// compare ops in 16-18 and 19-21, the combining op in 22-23). The shader compares the TEV alpha rounded to 0..255 (smoke.slang), so a
// reference at an end of that range decides some compares: GEQUAL 0 and LEQUAL 255 always pass, LESS 0 and GREATER 255 never do.
enum class AlphaTest : uint8_t { pass, fail, undetermined };
constexpr AlphaTest alpha_compare_result(uint32_t op,uint32_t reference) {
    switch (op&7) {
    case 0: return AlphaTest::fail;                                              // NEVER
    case 1: return reference==0 ? AlphaTest::fail : AlphaTest::undetermined;     // LESS
    case 3: return reference>=255 ? AlphaTest::pass : AlphaTest::undetermined;   // LEQUAL
    case 4: return reference>=255 ? AlphaTest::fail : AlphaTest::undetermined;   // GREATER
    case 6: return reference==0 ? AlphaTest::pass : AlphaTest::undetermined;     // GEQUAL
    case 7: return AlphaTest::pass;                                              // ALWAYS
    default: return AlphaTest::undetermined;                                     // EQUAL, NEQUAL
    }
}
constexpr AlphaTest alpha_test_result(uint32_t word) {
    const AlphaTest a=alpha_compare_result((word>>16)&7,word&255), b=alpha_compare_result((word>>19)&7,(word>>8)&255);
    const auto known=[](AlphaTest t) { return t!=AlphaTest::undetermined; };
    switch ((word>>22)&3) {
    case 0:   // AND
        if (a==AlphaTest::fail || b==AlphaTest::fail) return AlphaTest::fail;
        return a==AlphaTest::pass && b==AlphaTest::pass ? AlphaTest::pass : AlphaTest::undetermined;
    case 1:   // OR
        if (a==AlphaTest::pass || b==AlphaTest::pass) return AlphaTest::pass;
        return a==AlphaTest::fail && b==AlphaTest::fail ? AlphaTest::fail : AlphaTest::undetermined;
    case 2:   // XOR
        if (!known(a) || !known(b)) return AlphaTest::undetermined;
        return a!=b ? AlphaTest::pass : AlphaTest::fail;
    default:  // XNOR
        if (!known(a) || !known(b)) return AlphaTest::undetermined;
        return a==b ? AlphaTest::pass : AlphaTest::fail;
    }
}

// BP 0x43 PE control, as GXSetPixelFmt / GXSetZCompLoc leave it in the SDK's shadow (GXData + 0x22C): bits 0-2 the EFB pixel format, 3-5 the
// depth format, 6 the z compare location (1: depth tested and written before texturing). gx.cpp material_snapshot carries its low 7 bits in
// DrawUniform pixel[1] bits 8-14 with bit 15 set; a draw without bit 15 (tests, synthetic draws) has no known PE control.
constexpr uint32_t kPeControlShift=8, kPeControlKnown=1u<<15;
constexpr uint32_t pe_control_bits(uint32_t pe_control) { return ((pe_control&0x7F)<<kPeControlShift)|kPeControlKnown; }
constexpr bool pe_control_known(uint32_t depth_word) { return (depth_word&kPeControlKnown)!=0; }
enum PixelFormat : uint32_t { kRgb8Z24=0, kRgba6Z24=1, kRgb565Z16=2, kZ24=3 };
constexpr uint32_t pe_pixel_format(uint32_t depth_word) { return (depth_word>>kPeControlShift)&7; }
constexpr bool pe_z_top(uint32_t depth_word) { return pe_control_known(depth_word) && ((depth_word>>(kPeControlShift+6))&1); }
// Whether the EFB keeps no alpha for this draw (RGB8_Z24 or RGB565_Z16): alpha writes, destination alpha and DSTALPHA factors mean nothing.
constexpr bool pe_no_alpha(uint32_t depth_word) {
    return pe_control_known(depth_word) && (pe_pixel_format(depth_word)==kRgb8Z24 || pe_pixel_format(depth_word)==kRgb565Z16);
}

// The switches (each read once), all off by default until the headset A/B (docs/spikes/S18-dolphin-reference.md, "Done: depth and EFB"):
//   UC_ALPHA_PRECHECK=1  the full pre-evaluation above decides "cannot discard" (key word 0 bit 16), not only ALWAYS-and-ALWAYS
//   UC_FOLD_CLEAR=1      an EFB copy's clear becomes the next EFB pass's load op (or a clear inside it) instead of a pass of its own
//   UC_DEPTH_FORMAT=d24|x8d24|d24s8|d32  the headset's EFB depth format (default d32; d24: X8_D24_UNORM_PACK32, else D24_UNORM_S8_UINT;
//                        D32 where neither is a usable attachment, e.g. KosmicKrisp)
//   UC_DEPTH_STATS=1     per 2 s: what zcomploc, the EFB pixel format, the pre-check and destination alpha amount to in the draws
// zcomploc and the pixel format are read (pe_z_top, pe_no_alpha) but change no draw: in the fight no alpha-tested early-depth draw writes
// depth and every draw has RGBA6_Z24 (S18).
inline bool depth_switch(const char* name) { const char* value=std::getenv(name); return value && *value && *value!='0'; }

// Whether the pipeline must keep the alpha test's discard (key word 0 bit 16). Without UC_ALPHA_PRECHECK only both compares ALWAYS (and
// not combined by XOR) count as "cannot discard", as before.
inline bool alpha_test_needs_discard(uint32_t word,bool precheck) {
    if (precheck) return alpha_test_result(word)!=AlphaTest::pass;
    const uint32_t first=(word>>16)&7, second=(word>>19)&7, combine=(word>>22)&3;
    return !(first==7 && second==7 && combine!=2);
}

}  // namespace gx
