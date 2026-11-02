// The game's finished picture on the headset's screen plane (xr_compositor.cpp), both eyes in one multiview pass. The quad is the game's clip
// space -1..1; each eye's transform is HOTD4's HUD mapping (StereoScene::transforms, GL clip convention, column-major), converted to
// Vulkan clip space afterwards as HOTD4's shader wrapper does: Y flipped, depth -w..w to 0..w.
struct Eyes { float4 columns[2][4]; };
[[vk::push_constant]] ConstantBuffer<Eyes> eyes;
[[vk::binding(0, 0)]] Texture2D picture;
[[vk::binding(1, 0)]] SamplerState linear;

struct VOut { float4 position : SV_Position; float2 uv : TEXCOORD0; };

[shader("vertex")]
VOut vertexMain(uint id : SV_VertexID, uint view : SV_ViewID) {
    // two triangles: (0,1,2) (2,1,3) over the corners (-1,-1) (1,-1) (-1,1) (1,1)
    const uint corner = id < 3 ? id : (id == 3 ? 2 : (id == 4 ? 1 : 3));
    const float2 xy = float2((corner & 1) ? 1.0 : -1.0, (corner & 2) ? 1.0 : -1.0);
    const float4 clip = eyes.columns[view][0] * xy.x + eyes.columns[view][1] * xy.y + eyes.columns[view][3];
    VOut o;
    o.position = float4(clip.x, -clip.y, 0.5 * (clip.z + clip.w), clip.w);
    o.uv = float2(0.5 + 0.5 * xy.x, 0.5 - 0.5 * xy.y);   // clip +y is the picture's top row
    return o;
}

[shader("fragment")]
float4 fragmentMain(VOut i) : SV_Target0 {
    return float4(picture.Sample(linear, i.uv).rgb, 1);
}
