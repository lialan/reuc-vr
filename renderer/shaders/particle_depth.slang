// The headset's particle layer (VulkanRenderer, UC_PARTICLE_LAYER=1): the scene's depth at half its size a side, both eyes (multiview),
// one value per 2x2 block: the nearest (specialization constant 0 = 0: a sprite behind any of the four is hidden there, so smoke does not
// bleed over the edge of what stands in front of it) or the farthest (1, UC_PARTICLE_DEPTH=far).
[[vk::binding(0, 0)]] Texture2DArray<float> sceneDepth;
[[vk::constant_id(0)]] const uint farthest = 0;

[shader("vertex")]
float4 vertexMain(uint id : SV_VertexID) : SV_Position {   // one triangle over the whole target
    const float2 xy = float2((id & 1) != 0 ? 3.0 : -1.0, (id & 2) != 0 ? 3.0 : -1.0);
    return float4(xy, 0.0, 1.0);
}

[shader("fragment")]
float fragmentMain(float4 position : SV_Position, uint view : SV_ViewID) : SV_Depth {
    uint width, height, layers; sceneDepth.GetDimensions(width, height, layers);
    const int2 at = int2(position.xy) * 2, last = int2(width, height) - 1;
    const float a = sceneDepth.Load(int4(min(at, last), view, 0)), b = sceneDepth.Load(int4(min(at + int2(1, 0), last), view, 0));
    const float c = sceneDepth.Load(int4(min(at + int2(0, 1), last), view, 0)), d = sceneDepth.Load(int4(min(at + int2(1, 1), last), view, 0));
    return farthest != 0 ? max(max(a, b), max(c, d)) : min(min(a, b), min(c, d));
}
