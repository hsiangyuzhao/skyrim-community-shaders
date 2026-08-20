#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> HistoryTexture : register(t0);
Texture2D<float4> MomentsTexture : register(t1);
Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);

RWTexture2D<float4> VarianceOutput : register(u0);

cbuffer DenoiserCB : register(b2)
{
    float invMaxAccumulatedFrames;
    uint atrousIterations;
    float colorPhi;
    float normalPhi;
};

// (spec A3) The 7x7 refinement below runs only on pixels with almost no temporal history
// -- disocclusions, freshly revealed geometry, the first frames after a camera cut. That
// is a small and *clustered* fraction of the screen, which is what makes a group-level
// decision worth taking: an 8x8 tile is either entirely in steady state or largely inside
// such a cluster.
//
// The per-pixel `history <= 2` test is kept exactly as it was, so the output is
// unchanged. What the group vote buys is the right to hoist the neighbourhood's depth and
// normal fetches into a groupshared tile: the tile is only worth filling when at least
// one lane will read it, and only a group-wide answer can decide that. In a refining
// group each lane then replaces 48 depth loads and 48 normal loads with 48 LDS reads,
// while the tile itself costs 196 loads spread over 64 lanes.
//
// cs_5_0/fxc has no wave intrinsics, so the vote is a groupshared counter (see the same
// construction in ssrt_spatial.hlsl). Both vote barriers are unconditional, which is why
// the bounds and far-plane early-outs moved below them.
#define SSRT_VARIANCE_RADIUS 3
#define SSRT_VARIANCE_TILE (8 + 2 * SSRT_VARIANCE_RADIUS)  // 14

groupshared uint g_ssrtRefineLanes;
// View-space normal in .xyz, raw depth in .w. Neighbour roughness is not stored: the
// original loop decoded it and never used it.
groupshared float4 g_ssrtVarianceTile[SSRT_VARIANCE_TILE * SSRT_VARIANCE_TILE];

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint2 screen_size = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
    const bool inBounds = DTid.x < screen_size.x && DTid.y < screen_size.y;

    float2 uv = float2(DTid.xy + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;

    // (audit P1) Sky / far-plane early-out -- the 49-tap refinement below is pure waste
    // on background pixels, whose SSRT result can never reach the frame.
    float depthCenter = inBounds ? DepthTexture[DTid.xy] : 1.0f;
    const bool isFarPlane = SSRT_IS_FAR_PLANE(depthCenter);

    float4 ssrColor = inBounds ? SSRColorTexture[DTid.xy] : 0.0f;
    // SSRT_FLOAT_MAX for a lane that will not refine anyway keeps the load off sky and
    // out-of-bounds lanes, which used to `return` before reaching it.
    float history = (inBounds && !isFarPlane) ? MomentsTexture[DTid.xy].z : SSRT_FLOAT_MAX;

    // ---- (spec A3) refinement vote; no lane may leave before it completes ----
    if (all(GTid.xy == 0))
        g_ssrtRefineLanes = 0;
    GroupMemoryBarrierWithGroupSync();

    const bool laneRefines = inBounds && !isFarPlane && history <= 2;
    if (laneRefines)
        InterlockedAdd(g_ssrtRefineLanes, 1u);
    GroupMemoryBarrierWithGroupSync();

    // Group-uniform in practice, so no lane does the prefetch for nothing. The condition
    // guards LDS *writes* only -- the barrier that publishes them stays unconditional,
    // which keeps it uniform across the group.
    if (g_ssrtRefineLanes != 0u) {
        const int2 tileOrigin = int2(Gid.xy) * 8 - SSRT_VARIANCE_RADIUS;
        for (uint ty = GTid.y; ty < SSRT_VARIANCE_TILE; ty += 8) {
            for (uint tx = GTid.x; tx < SSRT_VARIANCE_TILE; tx += 8) {
                const int2 p = tileOrigin + int2(tx, ty);
                const bool valid = all(p >= 0) && all(p < int2(screen_size));

                // A halo texel outside the dynamic-resolution sub-rect is stored as
                // normal 0 / depth 1.0. The far plane keeps the linearisation inside
                // CalculateWeight well behaved (audit #8's "invalid region == 1.0"
                // convention), and a zero normal drives weightNormal =
                // pow(max(0, dot(n, 0)), phiNormal) to exactly 0, so such a texel can
                // never contribute even if the `inside` guard below were removed.
                float3 tileNormal = 0.0f;
                float tileDepth = 1.0f;
                if (valid) {
                    float tileRoughness;
                    GetNormalRoughness(uint2(p), tileNormal, tileRoughness);
                    tileDepth = DepthTexture[p];
                }
                g_ssrtVarianceTile[ty * SSRT_VARIANCE_TILE + tx] = float4(tileNormal, tileDepth);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- vote and prefetch complete; early returns are safe from here on ----

    if (!inBounds)
        return;

    if (isFarPlane) {
        VarianceOutput[DTid.xy] = 0.0;
        return;
    }

    float3 blendedColor = ssrColor.xyz;
    VarianceOutput[DTid.xy] = ssrColor;

    if (laneRefines) {
        const uint2 centerTile = uint2(GTid.xy) + SSRT_VARIANCE_RADIUS;
        float3 normalVS = g_ssrtVarianceTile[centerTile.y * SSRT_VARIANCE_TILE + centerTile.x].xyz;

        float luminanceCenter = Color::RGBToLuminance(ssrColor.xyz);
        float weightedColor = 1.f;
        float3 colorSum = ssrColor.xyz;
        float2 momentsSum = MomentsTexture[DTid.xy].xy;

        const int radius = SSRT_VARIANCE_RADIUS;
        for (int y = -radius; y <= radius; y++)
        {
            for (int x = -radius; x <= radius; x++)
            {
                if (x == 0 && y == 0)
                    continue;

                const int2 p = int2(DTid.xy) + int2(x, y);
                const bool inside = (p.x >= 0 && p.y >= 0) && (p.x < screen_size.x && p.y < screen_size.y);

                if (inside)
                {
                    float4 neighborSSRColor = SSRColorTexture[p];
                    const uint2 tp = uint2(int2(GTid.xy) + int2(x, y) + radius);
                    const float4 tile = g_ssrtVarianceTile[tp.y * SSRT_VARIANCE_TILE + tp.x];
                    float3 neighborNormalVS = tile.xyz;
                    float depthNeighbor = tile.w;
                    float neighborLuminance = Color::RGBToLuminance(neighborSSRColor.xyz);

                    float weight = CalculateWeight(depthCenter, depthNeighbor, length(float2(x, y)), normalVS, neighborNormalVS, normalPhi, luminanceCenter, neighborLuminance, colorPhi);

                    weightedColor += weight;
                    colorSum += neighborSSRColor.xyz * weight;
                    momentsSum += MomentsTexture[p].xy * weight;
                }
            }
        }

        weightedColor = max(weightedColor, 1e-5f);
        blendedColor = colorSum / weightedColor;
        momentsSum /= weightedColor;

        float variance = momentsSum.y - (momentsSum.x * momentsSum.x);
        // (audit #14) `history` comes from the moments texture and is 0 on the first
        // frame a pixel is seen (and on any frame the temporal pass found no valid
        // history), which made 2 / history evaluate to +inf and write inf into the
        // variance channel. That inf then reaches phiLuminance in the spatial pass and
        // NaNs the filter. Clamp the divisor to 1, i.e. treat "no accumulation yet" as
        // one frame of accumulation -- the maximum variance boost, which is the
        // conservative choice for a pixel we know nothing about.
        variance *= 2.0 / max(history, 1.0);
        VarianceOutput[DTid.xy] = float4(blendedColor, variance);
    }
}
