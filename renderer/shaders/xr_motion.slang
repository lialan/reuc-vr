// The headset's space warp vectors and depth (XR_FB_space_warp, renderer/xr_compositor.h): the depth-writing world draws again, at the
// runtime's motion vector size, both eyes by multiview. Positions come from the vertex pre-pass (smoke.slang xformMain): this frame's game
// view position, and with kind 1 last frame's (the draw's previous matrices, gx::MotionTracker). Both are projected with this frame's eye,
// so the vectors hold the scene's own motion (the game's camera and its actors) and not the head's, which the runtime adds itself.
// Output: current minus previous NDC (xyz) in RGB, as XR_FB_space_warp defines it; the depth is the eye's with the frame's one near/far.
// Vulkan NDC (y down, z 0..1), as XR_EXT_frame_synthesis states for its vectors: in GL and Vulkan alike NDC y = -1 is the image's row 0,
// so the runtime, which reads this image in the eye image's memory layout, sees the same direction either way.
struct XformPosition { float4 clip; float4 view; };
[[vk::binding(0, 0)]] StructuredBuffer<XformPosition> positions;
[[vk::binding(1, 0)]] StructuredBuffer<float4> previous;   // last frame's view position per pre-pass position (kind 1 draws only)
// eyes: per eye the rows of game view -> Vulkan clip (stereo_view_block with the frame's near/far). camera: rows of this frame's view ->
// last frame's view for a still point (MotionTracker::view_to_prev_view), for draws without a previous match (kind 2). flipY: 1 negates
// the vectors' y (UC_SPACE_WARP_FLIP_Y, a convention check on the headset).
struct Push { float4 eyes[2][4]; float4 camera[3]; uint kind; uint flipY; uint2 unused; };
[[vk::push_constant]] ConstantBuffer<Push> push;
struct Output {
    float4 position : SV_Position;
    [[vk::location(0)]] float4 current : TEXCOORD0;
    [[vk::location(1)]] float4 previous : TEXCOORD1;
};
float4 eyeClip(uint view, float4 v) {
    return float4(dot(push.eyes[view][0], v), dot(push.eyes[view][1], v), dot(push.eyes[view][2], v), dot(push.eyes[view][3], v));
}
Output vertexMain(uint vertex : SV_VulkanVertexID, uint view : SV_ViewID) {   // the pre-pass's rebased index (one index buffer a packet)
    const float4 v = positions[vertex].view;
    float4 p = v;
    if (push.kind == 1) p = previous[vertex];
    else if (push.kind == 2) p = float4(dot(push.camera[0], v), dot(push.camera[1], v), dot(push.camera[2], v), 1.0);
    Output output;
    output.current = eyeClip(view, v);
    output.previous = eyeClip(view, p);
    output.position = output.current;
    return output;
}
float4 fragmentMain(Output input) : SV_Target {
    if (input.previous.w <= 1e-5) return float4(0.0);   // last frame's point behind the eye: no usable vector
    float3 vector = input.current.xyz / input.current.w - input.previous.xyz / input.previous.w;
    if (push.flipY != 0) vector.y = -vector.y;
    return float4(vector, 0.0);
}
