// The game's finished frame in the headset (xr_compositor.cpp), both eyes in one multiview pass. The picture holds one layer per eye, each
// already drawn in its eye's space (the renderer's per-draw reprojection: world per eye, HUD on HOTD4's screen plane); the quad covers the
// eye through the transform pushed for it (GL clip convention, column-major), converted to Vulkan clip space afterwards as HOTD4's shader
// wrapper does: Y flipped, depth -w..w to 0..w.
// hud: the renderer's HUD layer (hud_layer.h) per eye, the rows of the homography from the picture's texture coordinate to the layer's;
// hud[0][2].w = 1 when this frame has one (eye = picture * T + L), 0 otherwise (bindings 2 and 3 then hold any image and are not read).
struct Eyes { float4 columns[2][4]; float4 rect; float4 hud[2][3]; };   // rect: the part of the picture shown (u, v, width, height): the whole slot image, or the EFB's displayed region
[[vk::push_constant]] ConstantBuffer<Eyes> eyes;
[[vk::binding(0, 0)]] Texture2DArray picture;
[[vk::binding(1, 0)]] SamplerState linear;
[[vk::binding(2, 0)]] Texture2DArray hudLight;      // L: what the HUD adds (a layer per eye)
[[vk::binding(3, 0)]] Texture2DArray hudTransmit;   // T: what the picture behind it keeps, stored at half scale (128/255 = 1)
[[vk::binding(4, 0)]] Texture2DArray glowLight;     // the glow layer under the HUD: the same pair, an affine map (hud[*][*].w, hud[1][2].w = 1 when there)
[[vk::binding(5, 0)]] Texture2DArray glowTransmit;

struct VOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };

[shader("vertex")]
VOut vertexMain(uint id : SV_VertexID, uint view : SV_ViewID) {
    // two triangles: (0,1,2) (2,1,3) over the corners (-1,-1) (1,-1) (-1,1) (1,1)
    const uint corner = id < 3 ? id : (id == 3 ? 2 : (id == 4 ? 1 : 3));
    const float2 xy = float2((corner & 1) ? 1.0 : -1.0, (corner & 2) ? 1.0 : -1.0);
    const float4 clip = eyes.columns[view][0] * xy.x + eyes.columns[view][1] * xy.y + eyes.columns[view][3];
    VOut o;
    o.position = float4(clip.x, -clip.y, 0.5 * (clip.z + clip.w), clip.w);
    o.uv = eyes.rect.xy + float2(0.5 + 0.5 * xy.x, 0.5 - 0.5 * xy.y) * eyes.rect.zw;   // clip +y is the picture's top row
    return o;
}

// Edge smoothing (FXAA, Lottes' 2009 form) as the picture is magnified onto the eye: the game draws without anti-aliasing and the eye's
// picture (687x720 at the default scale) spreads over about 100 degrees, so its stair-stepped edges crawled with every movement. Works on
// the picture's own texels and its stored (sRGB-encoded) colours, as FXAA expects. Specialization constant 0 = 0 turns it off (UC_XR_FXAA=0).
[[vk::constant_id(0)]] const uint fxaaOn = 1;
float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
float3 tap(float2 uv, float layer) {   // clamped to the shown rect's texel centres, as a picture of exactly that size clamps to its edge
    uint width, height, layers; picture.GetDimensions(width, height, layers);
    const float2 half = 0.5 / float2(width, height);
    return picture.SampleLevel(linear, float3(clamp(uv, eyes.rect.xy + half, eyes.rect.xy + eyes.rect.zw - half), layer), 0).rgb;
}
float3 fxaa(float2 uv, float layer) {
    uint width, height, layers; picture.GetDimensions(width, height, layers);
    const float2 texel = float2(1.0 / width, 1.0 / height);
    const float3 nw = tap(uv + float2(-0.5, -0.5) * texel, layer), ne = tap(uv + float2(0.5, -0.5) * texel, layer);
    const float3 sw = tap(uv + float2(-0.5, 0.5) * texel, layer), se = tap(uv + float2(0.5, 0.5) * texel, layer);
    const float3 m = tap(uv, layer);
    const float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);
    const float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    const float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
    const float scale = 1.0 / (min(abs(dir.x), abs(dir.y)) + reduce);
    dir = clamp(dir * scale, -8.0, 8.0) * texel;
    const float3 a = 0.5 * (tap(uv + dir * (1.0 / 3.0 - 0.5), layer) + tap(uv + dir * (2.0 / 3.0 - 0.5), layer));
    const float3 b = a * 0.5 + 0.25 * (tap(uv - dir * 0.5, layer) + tap(uv + dir * 0.5, layer));
    const float lb = luma(b);
    return (lb < lmin || lb > lmax) ? a : b;
}

float3 hud(float3 color, float2 uv, uint view) {
    if (eyes.hud[1][2].w != 0.0) {
        const float2 g = float2(eyes.hud[0][0].w * uv.x + eyes.hud[0][1].w, eyes.hud[1][0].w * uv.y + eyes.hud[1][1].w);
        if (all(g >= 0.0) && all(g <= 1.0))
            color = saturate(color * glowTransmit.SampleLevel(linear, float3(g, view), 0).rgb * (255.0 / 128.0) + glowLight.SampleLevel(linear, float3(g, view), 0).rgb);
    }
    if (eyes.hud[0][2].w == 0.0) return color;
    const float3 p = float3(uv, 1.0);
    const float3 q = float3(dot(eyes.hud[view][0].xyz, p), dot(eyes.hud[view][1].xyz, p), dot(eyes.hud[view][2].xyz, p));
    const float2 h = q.xy / q.z;
    if (q.z <= 0.0 || any(h < 0.0) || any(h > 1.0)) return color;
    const float3 light = hudLight.SampleLevel(linear, float3(h, view), 0).rgb, transmit = hudTransmit.SampleLevel(linear, float3(h, view), 0).rgb * (255.0 / 128.0);
    return saturate(color * transmit + light);
}

[shader("fragment")]
float4 fragmentMain(VOut i, uint view : SV_ViewID) : SV_Target0 {
    if (fxaaOn != 0) return float4(hud(fxaa(i.uv, float(view)), i.uv, view), 1);
    return float4(hud(picture.Sample(linear, float3(i.uv, float(view))).rgb, i.uv, view), 1);
}
