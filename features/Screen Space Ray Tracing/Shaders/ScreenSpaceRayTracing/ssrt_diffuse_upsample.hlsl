#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

// (spec C1) Depth- and normal-aware upsample of the half-resolution diffuse chain to the
// full-resolution texSSRTDiffuseColor the composite consumes. Nothing downstream changes.
//
// Modelled on SSGI's upsample.cs.hlsl, but not a copy of it. SSGI takes a binary decision
// per pixel -- a relative depth-spread test picks either an inverse-depth-weighted blend
// of the 2x2 or a plain bilinear tap -- which is right for an AO term. Indirect diffuse
// carries colour across normal discontinuities, where a depth-only guide cannot tell a
// wall from the floor it meets at a corner: both are continuous in depth, and blending
// across them drags bounce light onto the wrong surface. So this version:
//
//   * always blends, with bilinear weights multiplied by geometric ones, which degenerates
//     to exact bilinear on flat surfaces (all guide weights ~1) instead of switching
//     between two different reconstructions and showing the seam;
//   * adds a normal term, so corners and silhouettes stop the blend;
//   * falls back to the single most geometrically similar tap when every weight collapses,
//     rather than to a bilinear average that would be wrong precisely where it matters --
//     a thin full-resolution feature with no matching half-resolution sample.

Texture2D<float4> SrcDiffuseTexture : register(t0);  // half resolution
Texture2D<float> SrcDepthTexture : register(t1);     // half resolution: Hi-Z pyramid mip 1
// NormalRoughnessTexture (t2) comes from ssrt_common.hlsli.
Texture2D<float> DepthTexture : register(t4);        // full resolution

RWTexture2D<float4> UpsampledOutput : register(u0);

// Tolerated relative linear-depth change between a full-resolution pixel and a
// half-resolution tap. SSRT_DEPTH_WEIGHT_SCALE is the per-texel figure derived in
// ssrt_common.hlsli; a tap sits up to one half-resolution texel -- two full-resolution
// texels -- away, so the budget scales with that distance exactly as the a-trous kernel's
// phiDepth does.
#define SSRT_UPSAMPLE_DEPTH_SCALE (2.0f * SSRT_DEPTH_WEIGHT_SCALE)

// Normal exponent for the upsample. Deliberately *not* the NormalPhi the a-trous kernel
// uses (default 512): pow(dot, 512) puts a tap 8 degrees off the centre normal at 1e-3,
// so on any curved surface every tap would collapse and the upsample would degenerate to
// nearest-neighbour -- visibly blocky. 32 keeps a smooth surface intact (dot 0.99 -> 0.72)
// while still cutting a 25-degree crease down to 0.05.
#define SSRT_UPSAMPLE_NORMAL_PHI 32.0f

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 renderExtent = SSRT_GetRenderExtent();
    if (any(DTid.xy >= renderExtent)) {
        // Write rather than return: the composite's dispatch rounds up to whole 8x8 groups
        // as well, so it can read a few texels past the render sub-rect. Zero keeps them
        // deterministic instead of leaving whatever the previous frame left behind.
        UpsampledOutput[DTid.xy] = 0.0;
        return;
    }

    const float depthFull = DepthTexture[DTid.xy];
    if (SSRT_IS_FAR_PLANE(depthFull)) {
        UpsampledOutput[DTid.xy] = 0.0;
        return;
    }

    float3 normalFull;
    float roughnessFull;
    GetNormalRoughness(DTid.xy, normalFull, roughnessFull);

    const uint2 halfExtent = max(uint2(1, 1), renderExtent >> 1);
    const float linearFull = SharedData::GetScreenDepth(depthFull);

    // Continuous half-resolution coordinate of this pixel's centre, offset by half a
    // half-res texel so `base` is the top-left of the 2x2 that brackets it.
    const float2 srcPos = (float2(DTid.xy) + 0.5f) * 0.5f - 0.5f;
    const int2 base = int2(floor(srcPos));
    const float2 frac2 = srcPos - float2(base);

    float4 weightedSum = 0.0f;
    float weightSum = 0.0f;
    float4 bestColor = 0.0f;
    float bestGeomWeight = -1.0f;

    [unroll] for (int dy = 0; dy < 2; dy++)
    {
        [unroll] for (int dx = 0; dx < 2; dx++)
        {
            const int2 tap = clamp(base + int2(dx, dy), int2(0, 0), int2(halfExtent) - 1);
            const float bilinear = (dx == 0 ? 1.0f - frac2.x : frac2.x) * (dy == 0 ? 1.0f - frac2.y : frac2.y);

            const float depthTap = SrcDepthTexture[tap];
            // A tap on the far plane never held a traced sample (sky, or a mip 1 texel the
            // pyramid clear left at 1.0). Excluded outright rather than down-weighted.
            const float valid = SSRT_IS_FAR_PLANE(depthTap) ? 0.0f : 1.0f;

            const float linearTap = SharedData::GetScreenDepth(depthTap);
            const float relative = abs(linearTap - linearFull) / max(linearFull, 1e-5f);
            const float weightDepth = exp(-relative / SSRT_UPSAMPLE_DEPTH_SCALE);

            float3 normalTap;
            float roughnessTap;
            GetNormalRoughness(uint2(tap) * 2, normalTap, roughnessTap);
            const float weightNormal = pow(max(0.0f, dot(normalFull, normalTap)), SSRT_UPSAMPLE_NORMAL_PHI);

            const float geomWeight = weightDepth * weightNormal * valid;
            const float4 color = SrcDiffuseTexture[tap];

            weightedSum += color * (bilinear * geomWeight);
            weightSum += bilinear * geomWeight;

            if (geomWeight > bestGeomWeight) {
                bestGeomWeight = geomWeight;
                bestColor = color;
            }
        }
    }

    // The threshold is on the *bilinear-weighted* sum, so a pixel sitting almost exactly on
    // a tap keeps that tap even if the other three are rejected. Only when no tap survives
    // at all does the nearest-in-guide-space fallback take over; if even that is invalid
    // (an all-sky 2x2) the pixel gets 0, which is what the composite expects for "no
    // indirect light here".
    float4 result = bestGeomWeight > 0.0f ? bestColor : 0.0f;
    if (weightSum > 1e-5f)
        result = weightedSum / weightSum;

    UpsampledOutput[DTid.xy] = result;
}
