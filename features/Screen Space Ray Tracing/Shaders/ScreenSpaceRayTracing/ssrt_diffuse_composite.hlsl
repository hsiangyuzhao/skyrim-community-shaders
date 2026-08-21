// ssrt_common.hlsli supersedes the Common/Color.hlsli + Common/SharedData.hlsli pair this
// file used to include (it includes both) and brings filterNaN / filterInf and
// SSRT_MAX_RADIANCE for the G9 guard below. Its own resource declarations
// (NormalRoughnessTexture at t2, LinearSampler at s0) are not referenced here and fxc
// strips them, so the compiled binding table is unchanged.
#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> SSRTDiffuseTexture : register(t0);
Texture2D<float4> AlbedoTexture : register(t1);
// (ambient reinjection) Raw per-pixel hit confidence straight from the ray march, and the
// depth buffer the smoothing below uses as its edge stop.
Texture2D<float> SSRTConfidenceTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);

RWTexture2D<float4> ColorTextureRW : register(u0);
// (ambient reinjection) Spatially smoothed confidence, consumed by DeferredCompositeCS
// (which Deferred::DeferredPasses dispatches after DrawSSRTDiffuse, so it always reads this
// frame's values).
RWTexture2D<float> SSRTConfidenceSmoothRW : register(u1);

// (ambient reinjection) Why the confidence has to be smoothed before anything lerps with it.
//
// Confidence is a *coverage* quantity: the cosine-weighted fraction of the hemisphere in
// which the rays found usable screen-space geometry. The true quantity is spatially smooth --
// it is a property of the surrounding geometry, not of the sampling -- but the estimate is
// built from DIFFUSE_SPP directions per pixel, 2 by default, so a single pixel can only
// report a value from a set of about {0, 0.5, 1} plus the soft validation weights. Feeding
// that straight into `lerp(vanillaAmbient, ssrtGI, conf)` would put a fresh binary hit/miss
// pattern on the ambient term every frame, i.e. it would re-create the very instability the
// reinjection exists to remove.
//
// A plain spatial mean over N texels is the right estimator here (unbiased, and it converges
// as 1/sqrt(N) on a quantity that really is locally constant). At radius 3 the 49-tap window
// takes the per-pixel standard deviation from ~0.5 to ~0.07, and because the two things being
// mixed are diffuse irradiances of comparable magnitude the residual visible error is
// 0.07 * |ssrtGI - ambient|, not 0.07 of the whole term.
//
// Deliberately *not* temporal. A reprojected EMA would smooth further, but it would also
// import the entire class of defect the SVGF temporal pass is being audited for -- nearest
// neighbour reprojection, no disocclusion test -- into a signal whose whole job is to be
// stable, and it would lag the ambient/GI boundary behind moving geometry. Coverage changes
// discontinuously at a silhouette; a spatial estimate follows it in one frame.
#define SSRT_CONF_BLUR_RADIUS 3
#define SSRT_CONF_TILE (8 + 2 * SSRT_CONF_BLUR_RADIUS)  // 14

// Relative linear-depth difference at which a tap stops counting as the same surface.
//
// Same derivation as SSRT_DEPTH_WEIGHT_SCALE in ssrt_common.hlsli: the relative depth change
// per texel is pixelAngularSize * |slope| and is distance independent, ~1e-3 for a face-on
// surface and ~1e-2 for an extremely grazing one, so 3 texels of tap distance stays under
// ~3e-2 on the same surface while a real depth step is 0.3 or more. 0.1 sits an order of
// magnitude clear of both sides.
//
// A hard test rather than a falloff: 49 exp() calls would dominate the cost of this pass, and
// the estimator stays unbiased over whichever taps survive -- a truncated window at a
// silhouette just means slightly more residual noise in a 3-texel band, which is strictly
// better than bleeding one surface's coverage onto another and painting a halo of wrong
// ambient around every character.
#define SSRT_CONF_DEPTH_TOLERANCE 0.1f

groupshared float g_ssrtConfTile[SSRT_CONF_TILE * SSRT_CONF_TILE];
groupshared float g_ssrtConfDepthTile[SSRT_CONF_TILE * SSRT_CONF_TILE];

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID, uint3 groupID : SV_GroupID)
{
    // (audit #7) The render extent, matching every other pass in this feature: the dispatch,
    // the ray march's traversal grid and the pyramid's valid area all agree on it, and it is
    // what the tile fill has to clamp against.
    const int2 screen_size = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);

    // ---- (ambient reinjection) confidence tile prefetch. The 7x7 neighbourhoods of an 8x8
    // ---- group overlap almost completely -- 64 lanes want 3136 taps out of 196 distinct
    // ---- texels -- so they are read once each into LDS. No lane may leave before the barrier
    // ---- below, which is why the bounds early-out had to move underneath it (same shape as
    // ---- the firefly prefetch in ssrt_temporal.hlsl).
    {
        const int2 tileOrigin = int2(groupID.xy) * 8 - SSRT_CONF_BLUR_RADIUS;
        for (uint ty = groupThreadID.y; ty < SSRT_CONF_TILE; ty += 8) {
            for (uint tx = groupThreadID.x; tx < SSRT_CONF_TILE; tx += 8) {
                // Clamp to edge, as the firefly prefetch does: duplicating a border texel is
                // free here because the statistic is a local mean of a slowly varying field,
                // not an energy-preserving integral.
                const int2 p = clamp(tileOrigin + int2(tx, ty), int2(0, 0), screen_size - 1);
                const uint slot = ty * SSRT_CONF_TILE + tx;
                g_ssrtConfTile[slot] = SSRTConfidenceTexture[p];
                g_ssrtConfDepthTile[slot] = SharedData::GetScreenDepth(DepthTexture[p]);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- prefetch complete; early returns are safe from here on ----

    // (audit P9) SharedData::BufferDim.xy is the same full-resolution extent
    // ColorTextureRW.GetDimensions() returned (this UAV is the kMAIN render target), so
    // this is output-identical while dropping a per-thread resource query.
    if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
        return;

    {
        const int2 c = int2(groupThreadID.xy) + SSRT_CONF_BLUR_RADIUS;
        const uint centreSlot = c.y * SSRT_CONF_TILE + c.x;
        const float centreConf = g_ssrtConfTile[centreSlot];
        const float centreDepth = g_ssrtConfDepthTile[centreSlot];

        float sum = 0.0f;
        float weightSum = 0.0f;
        [unroll] for (int y = -SSRT_CONF_BLUR_RADIUS; y <= SSRT_CONF_BLUR_RADIUS; y++)
        {
            [unroll] for (int x = -SSRT_CONF_BLUR_RADIUS; x <= SSRT_CONF_BLUR_RADIUS; x++)
            {
                const uint slot = (c.y + y) * SSRT_CONF_TILE + (c.x + x);
                const float relative = abs(g_ssrtConfDepthTile[slot] - centreDepth) / max(centreDepth, 1e-5f);
                const float weight = relative < SSRT_CONF_DEPTH_TOLERANCE ? 1.0f : 0.0f;
                sum += g_ssrtConfTile[slot] * weight;
                weightSum += weight;
            }
        }

        // weightSum >= 1 always (the centre tap passes its own test with relative == 0), so
        // the fallback is unreachable arithmetic insurance rather than a real branch.
        SSRTConfidenceSmoothRW[dispatchID.xy] = weightSum > 0.0f ? saturate(sum / weightSum) : centreConf;
    }

    float4 ssrtDiffuse = SSRTDiffuseTexture[dispatchID.xy];
    // (guard G9) The last gate in the chain, and the one that decides whether an SSRT
    // failure is a local artefact or a global one. ColorTextureRW is kMAIN: whatever is
    // written here is what the upscaler, the bloom chain and the tonemapper consume, and
    // every one of those spreads a NaN far beyond the pixel that produced it -- a single
    // non-finite texel entering a downsample pyramid takes the whole mip with it, which is
    // how a handful of dead pixels becomes a black screen.
    //
    // The guard is deliberately duplicated with G2 at the ray march's own output rather
    // than being trusted from there. Between the two sit the temporal, variance and
    // a-trous passes plus a CopyResource, i.e. the entire denoiser, and this pass is also
    // reached with EnableSVGF off, where the ceiling costs one min() on a value that is
    // already bounded.
    //
    // No-op on healthy data for the same reason as G2: filterNaN is the identity on ordered
    // values, filterInf on finite ones, and min(x, SSRT_MAX_RADIANCE) on the whole
    // legitimate radiance range. .w is unused by the composite, so only the colour channels
    // are handled.
    ssrtDiffuse.rgb = min(filterInf(filterNaN(ssrtDiffuse.rgb)), SSRT_MAX_RADIANCE);
    float4 albedo = AlbedoTexture[dispatchID.xy];
    float4 originalColor = ColorTextureRW[dispatchID.xy];

    float3 color = Color::IrradianceToGamma(ssrtDiffuse.xyz * Color::IrradianceToLinear(albedo.xyz) + Color::IrradianceToLinear(originalColor.xyz));
    ColorTextureRW[dispatchID.xy] = float4(color, originalColor.w);
}
