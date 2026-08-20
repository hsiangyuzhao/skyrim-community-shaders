#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

RWTexture2D<float> DepthOutput : register(u0);

Texture2D<float> DepthTexture : register(t4);

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
    // (audit #8) Mip 0 of the Hi-Z pyramid is full-resolution but only the
    // dynamic-resolution sub-rect ever holds real depth; the dispatch is sized to that
    // sub-rect and rounds up to whole 8x8 groups, so it also touches up to 7 pixels
    // beyond it. Those pixels used to receive whatever the (never-cleared) depth target
    // held outside the sub-rect. Anything below the far plane there gets carried upward
    // by the min() in ssrt_depth_downsample.hlsl, and a value near 0 makes
    // `above_surface = surface_z > position.z` false on first test, so any ray landing
    // in such a tile is instantly judged a hit.
    //
    // Writing the far plane instead makes the whole pyramid well defined: min() with
    // 1.0 is a no-op, so every coarser level inherits "1.0 outside the sub-rect" by
    // induction. Pixels beyond the dispatch entirely are covered by the one-off clear
    // in ScreenSpaceRayTracing::Prepass.
    const uint2 renderDim = uint2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
    DepthOutput[DTid.xy] = any(DTid.xy >= renderDim) ? 1.0 : DepthTexture[DTid.xy];
}