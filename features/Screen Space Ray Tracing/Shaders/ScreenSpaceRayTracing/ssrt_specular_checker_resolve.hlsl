#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

#if !defined(SSRT_SPECULAR) || !defined(SSRT_SPARSE_CHECKERBOARD)
#error "The specular checkerboard resolve requires both specular and checkerboard defines."
#endif

// One compact texel represents one real full-resolution pixel. The ray march writes
// either linear RGBA16F or REBLUR's packed RGBA16F into t0; t1 is always the raw
// R32_FLOAT world-space hit distance consumed by DLSS-RR. Both must be restored
// before any denoiser, the deferred composite, or the upscaler runs.
Texture2D<float4> SparseColorTexture : register(t0);
Texture2D<float> SparseHitDistanceTexture : register(t1);
// NormalRoughnessTexture is t2 from ssrt_common.hlsli.
Texture2D<float> DepthTexture : register(t3);

RWTexture2D<float4> ResolvedColorOutput : register(u0);
RWTexture2D<float> ResolvedHitDistanceOutput : register(u1);

// C++ ScreenSpaceRayTracing::SSRTCB has these at byte offsets 64 and 68;
// its offsetof assertions protect this b1 c4.x/y mirror. DrawSSRTSpecular
// leaves b1 bound between the ray march and this resolve.
cbuffer SpecularResolveCB : register(b1)
{
    uint NRDFrontEndPack : packoffset(c4.x);
    float SpecularMaxRoughness : packoffset(c4.y);
};

static const float SSRT_SPECULAR_NO_HIT = 65536.0f;
static const float SSRT_SPECULAR_DEPTH_SCALE = SSRT_DEPTH_WEIGHT_SCALE;
static const float SSRT_SPECULAR_NORMAL_PHI = 32.0f;

void StoreResolved(uint2 px, float4 color, float hitDistance)
{
    ResolvedColorOutput[px] = color;
    ResolvedHitDistanceOutput[px] = hitDistance;
}

float4 NoSampleColor()
{
    // The ray march packs a miss to normalized hit-distance 1 on REBLUR's
    // front end. Linear/SVGF/Off instead write zero alpha with zero radiance.
    return float4(0.0f, 0.0f, 0.0f, NRDFrontEndPack != 0u ? 1.0f : 0.0f);
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
    const uint2 px = DTid.xy;
    const uint2 renderExtent = SSRT_GetRenderExtent();
    if (any(px >= renderExtent)) {
        StoreResolved(px, NoSampleColor(), SSRT_SPECULAR_NO_HIT);
        return;
    }

    const float depth = DepthTexture[px];
    if (SSRT_IS_FAR_PLANE(depth)) {
        StoreResolved(px, NoSampleColor(), SSRT_SPECULAR_NO_HIT);
        return;
    }

    float3 normal;
    float roughness;
    GetNormalRoughness(px, normal, roughness);
    roughness = clamp(roughness, 0.02f, 1.0f);  // same clamp as the specular ray march
    if (roughness > SpecularMaxRoughness) {
        // No ray would have run at this destination. Do not fill a rough
        // cubemap-only surface with a sharp neighbour's reflection.
        StoreResolved(px, NoSampleColor(), SSRT_SPECULAR_NO_HIT);
        return;
    }

    const uint2 sparseExtent = SSRT_GetSparseExtent(renderExtent);
    const uint compactX = min(px.x >> 1, sparseExtent.x - 1u);
    const uint2 own = uint2(compactX, px.y);
    if (SSRT_SparseCheckerIsTraced(px)) {
        // Exact copy for every traced pixel, including the unpaired last column
        // of an odd render width. No filtering can soften a traced reflection.
        StoreResolved(px, SparseColorTexture[own], SparseHitDistanceTexture[own]);
        return;
    }

    const float linearDepth = SharedData::GetScreenDepth(depth);

    float4 weightedColor = 0.0f;
    float weightSum = 0.0f;
    float4 bestColor = NoSampleColor();
    float bestHitDistance = SSRT_SPECULAR_NO_HIT;
    float bestWeight = 0.0f;

    // The two vertical taps traced the skipped pixel's own column. The centre
    // tap is its horizontal partner and supplies a bounded fallback at the top
    // and bottom rows or across a depth/normal discontinuity. Each tap's guide
    // comes from the precise full-resolution pixel the compact lane traced.
    [unroll] for (int k = -1; k <= 1; ++k) {
        const int row = int(px.y) + k;
        if (row < 0 || row >= int(renderExtent.y))
            continue;

        const uint2 tap = uint2(compactX, (uint)row);
        const uint2 guidePx = uint2(SSRT_SparseCheckerColumn(tap), (uint)row);
        const float guideDepth = DepthTexture[guidePx];
        if (SSRT_IS_FAR_PLANE(guideDepth))
            continue;

        const float relativeDepth = abs(SharedData::GetScreenDepth(guideDepth) - linearDepth) /
                                    max(linearDepth, 1e-5f);
        float3 guideNormal;
        float guideRoughness;
        GetNormalRoughness(guidePx, guideNormal, guideRoughness);
        guideRoughness = clamp(guideRoughness, 0.02f, 1.0f);
        const float roughnessGap = abs(guideRoughness - roughness);
        if (guideRoughness > SpecularMaxRoughness || roughnessGap > 0.12f)
            continue;  // an untraced rough pixel or a different reflection lobe

        const float weight = exp(-relativeDepth / SSRT_SPECULAR_DEPTH_SCALE) *
                             pow(max(0.0f, dot(normal, guideNormal)), SSRT_SPECULAR_NORMAL_PHI) *
                             exp(-roughnessGap / 0.08f);
        const float4 color = SparseColorTexture[tap];
        const float hitDistance = SparseHitDistanceTexture[tap];
        weightedColor += color * weight;
        weightSum += weight;
        if (weight > bestWeight) {
            bestWeight = weight;
            bestColor = color;
            bestHitDistance = hitDistance;
        }
    }

    float4 resultColor = bestColor;
    float resultHitDistance = bestHitDistance;
    if (weightSum > 1e-5f) {
        resultColor = weightedColor / weightSum;
    }
    // The R32 guide is the dominant tap's *actual* world-space distance, or
    // the exact 65536 miss sentinel. Interpolating them would invent a false
    // distance when a hit and a miss border one another; nor may the guide be
    // saturated like diffuse's reciprocally encoded R8 surface.
    // The color alpha is REBLUR's packed normHitDist when packing is active;
    // it remains in that representation through this channel-agnostic blend.
    StoreResolved(px, resultColor, resultHitDistance);
}
