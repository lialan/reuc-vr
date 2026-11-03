// The headset's HUD layer (hud_layer.h) composited into the EFB, both eyes (multiview), for a frame whose EFB is read or drawn into after
// its HUD (a capture, the desktop preview, a later packet): EFB = EFB * T + L in two draws, specialization constant 0 choosing which.
//   0: multiply, blend (DST_COLOR, SRC_COLOR): EFB * 2v, v = T_stored * 255/256, so EFB * T_stored * 255/128 = EFB * T
//   1: add, blend (ONE, ONE): EFB + L
struct Composite { float4 rows[2][3]; float2 efbSize; float2 unused; };   // rows: per eye, EFB texture coordinate -> layer texture coordinate
[[vk::push_constant]] ConstantBuffer<Composite> composite;
[[vk::binding(0, 0)]] Texture2DArray hudLight;
[[vk::binding(1, 0)]] Texture2DArray hudTransmit;
[[vk::binding(2, 0)]] SamplerState linear;
[[vk::constant_id(0)]] const uint addLight = 0;

[shader("vertex")]
float4 vertexMain(uint id : SV_VertexID) : SV_Position {   // one triangle over the whole target
    const float2 xy = float2((id & 1) ? 3.0 : -1.0, (id & 2) ? 3.0 : -1.0);
    return float4(xy, 0.0, 1.0);
}

[shader("fragment")]
float4 fragmentMain(float4 position : SV_Position, uint view : SV_ViewID) : SV_Target0 {
    const float3 p = float3(position.xy / composite.efbSize, 1.0);
    const float3 q = float3(dot(composite.rows[view][0].xyz, p), dot(composite.rows[view][1].xyz, p), dot(composite.rows[view][2].xyz, p));
    const float2 h = q.xy / q.z;
    const bool inside = q.z > 0.0 && all(h >= 0.0) && all(h <= 1.0);
    if (addLight != 0) return inside ? float4(hudLight.SampleLevel(linear, float3(h, view), 0).rgb, 0.0) : float4(0.0);
    return inside ? float4(hudTransmit.SampleLevel(linear, float3(h, view), 0).rgb * (255.0 / 256.0), 0.5) : float4(0.5);
}
