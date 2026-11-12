// The headset's space warp vectors and depth (XR_FB_space_warp, renderer/xr_compositor.h): the depth-writing world draws again, at the
// runtime's motion vector size, both eyes by multiview. Positions come from the vertex pre-pass (smoke.slang xformMain): this frame's game
// view position, and with kind 1 last frame's (the draw's previous matrices, gx::MotionTracker). The current position goes through this
// frame's eye, the previous one through last frame's eye (Meta's XrSpaceWarp sample: PrevViewProjectionMatrix * PrevLocalToWorld), so the
// vectors hold the whole screen motion: the head's, the game camera's and the actors'.
// Output: current minus previous NDC (xyz) in RGB, as XR_FB_space_warp defines it; the depth is the eye's with fixed near/far planes.
// Vulkan NDC (y down, z 0..1), as XR_EXT_frame_synthesis states for its vectors: in GL and Vulkan alike NDC y = -1 is the image's row 0,
// so the runtime, which reads this image in the eye image's memory layout, sees the same direction either way.
struct XformPosition { float4 clip; float4 view; };
[[vk::binding(0, 0)]] StructuredBuffer<XformPosition> positions;
[[vk::binding(1, 0)]] StructuredBuffer<float4> previous;   // last frame's view position per pre-pass position (kind 1 draws only)
// Per eye, rows of game view -> Vulkan clip: eyes this frame's; previousEyes last frame's (this frame's when the head is left out, a
// test); previousCamera = previousEyes * the game camera's motion (MotionTracker::view_to_prev_view), for draws without a match (kind 2).
struct WarpEyes { float4 eyes[2][4]; float4 previousEyes[2][4]; float4 previousCamera[2][4]; };
[[vk::binding(2, 0)]] StructuredBuffer<WarpEyes> frame;
// flipY: 1 negates the vectors' y (UC_SPACE_WARP_FLIP_Y, a convention check on the headset).
struct Push { uint kind; uint flipY; uint2 unused; };
[[vk::push_constant]] ConstantBuffer<Push> push;
struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 current : TEXCOORD0;
    [[vk::location(1)]] float4 previous : TEXCOORD1;
};
float4 rows(float4 m[4], float4 v) { return float4(dot(m[0], v), dot(m[1], v), dot(m[2], v), dot(m[3], v)); }
Output vertexMain(uint vertex : SV_VulkanVertexID, uint view : SV_ViewID) {   // the pre-pass's rebased index (one index buffer a packet)
    const float4 v = positions[vertex].view;
    Output output;
    output.current = rows(frame[0].eyes[view], v);
    if (push.kind == 1) output.previous = rows(frame[0].previousEyes[view], previous[vertex]);
    else output.previous = rows(frame[0].previousCamera[view], v);
    output.position = output.current;
    return output;
}
float4 fragmentMain(Output input) : SV_Target {
    if (input.previous.w <= 1e-5) return float4(0.0);   // last frame's point behind the eye: no usable vector
    float3 vector = input.current.xyz / input.current.w - input.previous.xyz / input.previous.w;
    if (push.flipY != 0) vector.y = -vector.y;
    return float4(vector, 0.0);
}
