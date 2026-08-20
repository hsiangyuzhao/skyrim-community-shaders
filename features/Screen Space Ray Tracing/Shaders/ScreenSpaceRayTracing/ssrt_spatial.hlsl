#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> HistoryTexture : register(t0);
// (spec A1) Moments texture written by ssrt_temporal.hlsl: .xy = luminance moments,
// .z = accumulated frame count. t1 used to receive the motion-vector target, which this
// shader never declared or read, so the slot was free.
Texture2D<float4> MomentsTexture : register(t1);
Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);

RWTexture2D<float4> FilteredOutput : register(u0);

// Mirrors ScreenSpaceRayTracing::DenoiserCB, which is the source of truth for the
// layout. ssrt_temporal.hlsl and ssrt_variance.hlsl declare only the leading four
// scalars they use; a shader may declare a prefix of a larger constant buffer.
cbuffer DenoiserCB : register(b2)
{
    float invMaxAccumulatedFrames;
    uint atrousIterations;
    float colorPhi;
    float normalPhi;
    uint adaptiveFiltering;
    float adaptiveHistoryThreshold;
    float adaptiveVarianceEps;
    // (spec S1) Read by ssrt_temporal.hlsl only; declared here so the two views of the
    // buffer keep matching offsets.
    float fireflyClampSigma;
};

// (spec A5) The 3x3 Gaussian pre-blur of the variance channel, and the switch to A/B it.
//
// Worth stating plainly, because it is easy to mistake for a leftover of the era when
// variance guidance was broken (audit #11): this pre-blur is prescribed by SVGF itself.
// Schied et al. 2017 smooth the variance estimate with a small Gaussian before it drives
// the luminance edge-stopping function, because a variance estimated from a handful of
// samples is itself noisy, and feeding that straight into phiLuminance makes the filter
// strength fluctuate pixel to pixel -- which shows up as blotches, not as noise.
//
// What #11 changed is only that the input is now meaningful. It costs 9 loads and ~34
// instruction slots per pixel, so it is a real fraction of the pass, hence the switch;
// but the default stays on and the report recommends keeping it. Set
// SSRT_SVGF_GAUSSIAN=0 to measure the alternative.
#ifndef SSRT_SVGF_GAUSSIAN
#   define SSRT_SVGF_GAUSSIAN 1
#endif

#if SSRT_SVGF_GAUSSIAN
float GaussianBlur(uint2 id)
{
    float sum = 0.f;
    float kernelSum = 0.f;
    const float kernel[2][2] =
    {
        { 1.0 / 4.0, 1.0 / 8.0 },
        { 1.0 / 8.0, 1.0 / 16.0 }
    };
    
    const int radius = 1;
    
    for (int y = -radius; y <= radius; y++)
    {
        for (int x = -radius; x <= radius; x++)
        {
            const int2 p = id + int2(x, y);
            const bool inside = (p.x >= 0 && p.y >= 0) && (p.x < SharedData::BufferDim.x * FrameBuffer::DynamicResolutionParams1.x && p.y < SharedData::BufferDim.y * FrameBuffer::DynamicResolutionParams1.y);

            if (inside)
            {
                const float k = kernel[abs(x)][abs(y)];
                kernelSum += k;
                sum += SSRColorTexture[p].w * k;
            }
        }
    }

    return sum / kernelSum;
}
#endif

static const float kernelWeights[3] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };

#define VAR_EPSILON 0.00001f

// (spec A1) Group-wide convergence vote.
//
// This pipeline compiles as cs_5_0 through fxc, which has no wave intrinsics, so the
// "is every lane of this tile converged?" question cannot be answered with
// WaveActiveAllTrue. It is answered with a groupshared counter instead: every one of the
// 64 lanes of the 8x8 group contributes 1 when it considers itself converged, and only a
// count of exactly 64 authorises the whole group to take the early path.
//
// Whole-group granularity is deliberate, not a fallback. A partial early-out inside a
// group would save nothing: the surviving lanes still execute the 25-tap kernel and the
// converged ones just idle through it, so the group's cost is unchanged. Only skipping a
// group in its entirety removes work.
//
// Both barriers below are executed unconditionally by every lane. That is what forces
// the restructuring of the two early-outs further down (they now happen *after* the
// reduction): a `return` before a GroupMemoryBarrierWithGroupSync() would make the
// barrier non-uniform across the group, which is undefined behaviour.
groupshared uint g_ssrtConvergedLanes;

// (spec A4) Groupshared depth/normal tile for the a-trous taps.
//
// The 25 taps of an 8x8 group overlap heavily, so their depth and normal fetches want to
// be shared. The footprint is *stride dependent*, though: a tap sits at
// DTid + k * stride with k in [-2, 2] and stride = atrousIterations + 1, so the group
// needs a (8 + 4 * stride) square, not the fixed 8 + 2 * 2 an unstrided 5x5 kernel would
// need. Reuse falls off accordingly -- 1600 tap fetches over 144 texels at stride 1,
// over 256 at stride 2, over 784 at stride 5.
//
// The tile is therefore sized for stride 2, which covers both iterations of the new
// AtrousIterations default (strides 1 and 2) at 4 KB of LDS and 11x / 6x fewer global
// loads. Beyond that the reuse no longer pays for the LDS pressure and the pass falls
// back to the original per-tap loads.
#define SSRT_SPATIAL_LDS_MAX_STRIDE 2
#define SSRT_SPATIAL_TILE (8 + 4 * SSRT_SPATIAL_LDS_MAX_STRIDE)  // 16

// View-space normal in .xyz, raw depth in .w. Only the tap loop reads it; the centre
// pixel keeps its direct fetch because it also needs roughness, which is not stored.
groupshared float4 g_ssrtSpatialTile[SSRT_SPATIAL_TILE * SSRT_SPATIAL_TILE];

// Returns float4(view-space normal, raw depth) for one tap, from LDS when the tile covers
// this stride and straight from the G-buffer otherwise. Both paths yield the same values
// for the same texel, so the filter output does not depend on which one is taken.
float4 SSRTSpatialFetchGuide(int2 samplePos, int2 tileCoord, bool useLDS)
{
    // Single exit: fxc's flow analysis reports X4000 on the return slot of a function
    // whose early return sits inside a branch.
    float4 guide = 0.0f;
    if (useLDS) {
        guide = g_ssrtSpatialTile[tileCoord.y * SSRT_SPATIAL_TILE + tileCoord.x];
    } else {
        float3 sampleNormalVS;
        float sampleRoughness;
        GetNormalRoughness(uint2(samplePos), sampleNormalVS, sampleRoughness);
        guide = float4(sampleNormalVS, DepthTexture[samplePos]);
    }
    return guide;
}

// Spatiotemporal Variance-Guided Filter
[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint2 screen_size = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
    const bool inBounds = DTid.x < screen_size.x && DTid.y < screen_size.y;

    float2 uv = float2(DTid.xy + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;

    // (audit P1) Sky / far-plane early-out. The existing `depthCenter > 0` gate below
    // does *not* cover the far plane (sky depth is 1.0, which is > 0), so today every
    // sky pixel pays the full 25-tap a-trous kernel. Write 0 rather than just
    // returning: this shader ping-pongs between two textures, so skipping the write
    // would leave the previous iteration's (or previous frame's) content behind.
    float depthCenter = inBounds ? DepthTexture[DTid.xy] : 1.0f;
    const bool isFarPlane = SSRT_IS_FAR_PLANE(depthCenter);

    float4 ssrColor = inBounds ? SSRColorTexture[DTid.xy] : 0.0f;

    // ---- (spec A1) convergence reduction; no lane may leave before it completes ----
    if (all(GTid.xy == 0))
        g_ssrtConvergedLanes = 0;
    GroupMemoryBarrierWithGroupSync();

    // An out-of-bounds or sky lane has nothing left to filter, so it must not veto the
    // early path for the rest of the tile: it counts as converged. Its own output is
    // still produced by the dedicated branches below.
    bool laneConverged = !inBounds || isFarPlane;
    if (!laneConverged) {
        // .z is the accumulated frame count maintained by ssrt_temporal.hlsl. Once it
        // reaches MaxAccumulatedFrames the temporal blend weight has bottomed out at
        // invMaxAccumulatedFrames, i.e. the pixel is in steady state -- which is exactly
        // why the default threshold equals the default MaxAccumulatedFrames.
        const float accumFrames = MomentsTexture[DTid.xy].z;
        // .w is the per-pixel variance the ping-pong carries (audit #11 made it real);
        // it shrinks with every a-trous iteration, so a tile that is not quiet enough to
        // skip iteration 0 may still skip iteration 1 or 2.
        laneConverged = accumFrames >= adaptiveHistoryThreshold && ssrColor.w < adaptiveVarianceEps;
    }
    if (laneConverged)
        InterlockedAdd(g_ssrtConvergedLanes, 1u);
    GroupMemoryBarrierWithGroupSync();

    const bool groupConverged = g_ssrtConvergedLanes == 64u;
    const bool skipFilter = adaptiveFiltering != 0 && groupConverged;

    // ---- (spec A4) tap guide prefetch ----
    const uint atrousStride = atrousIterations + 1;
    const bool useLDS = atrousStride <= SSRT_SPATIAL_LDS_MAX_STRIDE;
    // Both conditions are group uniform -- skipFilter comes from the vote above and
    // useLDS from a constant buffer -- so no lane fills the tile for nothing. They guard
    // LDS *writes* only; the barrier that publishes them stays unconditional.
    if (!skipFilter && useLDS) {
        const uint tileDim = 8 + 4 * atrousStride;
        const int2 tileOrigin = int2(Gid.xy) * 8 - int(2 * atrousStride);
        for (uint ty = GTid.y; ty < tileDim; ty += 8) {
            for (uint tx = GTid.x; tx < tileDim; tx += 8) {
                const int2 p = tileOrigin + int2(tx, ty);
                const bool valid = all(p >= 0) && all(p < int2(screen_size));

                // Depth 0 for a texel outside the render sub-rect: the tap loop's
                // existing `sampleDepth > 0` guard then rejects it exactly as its
                // `inside` test does. The zero normal is a second line of defence --
                // it drives weightNormal = pow(max(0, dot(n, 0)), phiNormal) to 0.
                float3 tileNormal = 0.0f;
                float tileDepth = 0.0f;
                if (valid) {
                    float tileRoughness;
                    GetNormalRoughness(uint2(p), tileNormal, tileRoughness);
                    tileDepth = DepthTexture[p];
                }
                g_ssrtSpatialTile[ty * SSRT_SPATIAL_TILE + tx] = float4(tileNormal, tileDepth);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- reduction and prefetch complete; early returns are safe from here on ----

    if (!inBounds)
        return;

    if (isFarPlane) {
        FilteredOutput[DTid.xy] = 0.0;
        return;
    }

    if (skipFilter) {
        // Copy input to output verbatim, .w included. The a-trous chain ping-pongs
        // between two textures and the next iteration reads both the colour and the
        // variance back, so the early path must *copy* rather than skip the write --
        // otherwise the following iteration would consume the content of two iterations
        // ago (or of the previous frame).
        FilteredOutput[DTid.xy] = ssrColor;
        return;
    }

    float3 blendedColor = 0;

    float3 normalVS;
    float roughness;
    GetNormalRoughness(DTid.xy, normalVS, roughness);
    roughness = clamp(roughness, 0.001f, 1.0f);

    float luminanceCenter = Color::RGBToLuminance(ssrColor.rgb);
#if SSRT_SVGF_GAUSSIAN
    float variance = GaussianBlur(DTid.xy);
#else
    // (spec A5) The carried variance, unsmoothed. Post-#11 this channel is already the
    // squared-weight filtered variance the SVGF paper prescribes.
    float variance = ssrColor.w;
#endif

    // (audit #11) Variance travels in .w through the ping-pong, and it is what drives
    // phiLuminance above. The output used to hard-code .w = 1.0, so from the second
    // a-trous iteration on GaussianBlur() read back a constant 1 and the filter stopped
    // being variance-guided. Filter the variance alongside the colour with the squared
    // weights, as SVGF prescribes, and carry the result.
    float filteredVariance = ssrColor.w;

    if (depthCenter > 0)
    {
        float phiLuminance = max(colorPhi * sqrt(abs(variance) + VAR_EPSILON), VAR_EPSILON);
        float phiNormal = normalPhi;
#if defined(SSRT_SPECULAR)
        // Trying to reduce blurriness on glossy surfaces
        phiLuminance *= roughness;
        phiNormal /= roughness;
#endif
        float phiDepth = atrousStride;
        float weightSum = 0.f;
        float varianceSum = 0.f;

        for (int ky = -2; ky <= 2; ky++)
        {
            for (int kx = -2; kx <= 2; kx++)
            {
                // A-Trous sampling
                int2 samplePos = int2(DTid.xy) + int2(kx, ky) * int(atrousStride);
                bool inside = (samplePos.x >= 0 && samplePos.y >= 0) && (samplePos.x < screen_size.x && samplePos.y < screen_size.y);
                if (inside)
                {
                    float4 sampleSSRColor = SSRColorTexture[samplePos];
                    // (spec A4) Tile coordinate of this tap; the tile origin sits
                    // 2 * stride texels before the group, so the offset cancels out.
                    const int2 tileCoord = int2(GTid.xy) + int2(kx, ky) * int(atrousStride) + int(2 * atrousStride);
                    const float4 guide = SSRTSpatialFetchGuide(samplePos, tileCoord, useLDS);
                    float sampleDepth = guide.w;
                    if (sampleDepth > 0)
                    {
                        float3 sampleNormalVS = guide.xyz;

                        float luminanceP = Color::RGBToLuminance(sampleSSRColor.rgb);
                        float weight = CalculateWeight(depthCenter, sampleDepth, phiDepth, normalVS, sampleNormalVS, phiNormal, luminanceCenter, luminanceP, phiLuminance) * kernelWeights[abs(kx)] * kernelWeights[abs(ky)];

                        blendedColor += sampleSSRColor.rgb * weight;
                        // Variance of a weighted mean scales with the squared weights.
                        varianceSum += sampleSSRColor.w * weight * weight;
                        weightSum += weight;
                    }
                }
            }
        }
        if (weightSum > 0.f)
        {
            blendedColor /= weightSum;
            filteredVariance = varianceSum / (weightSum * weightSum);
        }
        else
        {
            blendedColor = ssrColor.rgb;
            filteredVariance = ssrColor.w;
        }
    }

    FilteredOutput[DTid.xy] = float4(blendedColor, filteredVariance);
}