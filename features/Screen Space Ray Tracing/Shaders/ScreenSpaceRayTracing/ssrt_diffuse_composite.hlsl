// ssrt_common.hlsli supersedes the Common/Color.hlsli + Common/SharedData.hlsli pair this
// file used to include (it includes both) and brings filterNaN / filterInf and
// SSRT_MAX_RADIANCE for the G9 guard below. Its own resource declarations
// (NormalRoughnessTexture at t2, LinearSampler at s0) are not referenced here and fxc
// strips them, so the compiled binding table is unchanged.
#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> SSRTDiffuseTexture : register(t0);
Texture2D<float4> AlbedoTexture : register(t1);

RWTexture2D<float4> ColorTextureRW : register(u0);

// (batch 6) SSRT_CONF_EXTERNAL_FILTER strips this pass back to the colour composite alone.
// The confidence smoothing has moved out into its own three-pass chain -- quarter-resolution
// downsample, wide separable joint bilateral blur, joint bilateral upsample -- which publishes
// the same surface with the same semantics from about 16x the ray samples for less GPU time.
// See ssrt_conf_filter.hlsli.
//
// A permutation rather than a constant-buffer branch, for two reasons: the stripped version
// pays nothing at all (no LDS, no tile prefetch, no barrier, no 49 taps, three fewer SRVs and
// two fewer UAVs), and the unstripped version stays byte-identical to the shader that shipped
// before this change, which is what makes the user-facing toggle a real A/B rather than an
// approximation of one.
#ifndef SSRT_CONF_EXTERNAL_FILTER
// (ambient reinjection) Raw per-pixel hit confidence straight from the ray march, and the
// depth buffer the smoothing below uses as its edge stop.
Texture2D<float> SSRTConfidenceTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);
// (reinjection noise) The confidence accumulator's read ends. Bound only when
// TemporalAmbientConfidence is set; an unbound SRV reads zero, which the validity test below
// rejects as "no history", so a stale binding cannot leak into the result.
Texture2D<float4> ConfidenceHistoryTexture : register(t5);
Texture2D<float4> MotionVectorTexture : register(t6);

// (ambient reinjection) Smoothed confidence, consumed by DeferredCompositeCS (which
// Deferred::DeferredPasses dispatches after DrawSSRTDiffuse, so it always reads this frame's
// values). Spatial mean alone, or the temporal accumulation of it -- see below.
RWTexture2D<float> SSRTConfidenceSmoothRW : register(u1);
// (reinjection noise) This frame's accumulator state, which the C++ side swaps into
// ConfidenceHistoryTexture for the next frame.
RWTexture2D<float4> ConfidenceHistoryRW : register(u2);
#endif

#ifndef SSRT_CONF_EXTERNAL_FILTER
// (reinjection noise) Mirrors ScreenSpaceRayTracing::SSRTCB. Only the last two members are read
// here; the eight before them are declared because a constant buffer cannot be entered at an
// offset. ssrt_raymarch.hlsl declares the same buffer up to UseBlueNoise and is the other
// consumer, so the two declarations must stay in lockstep with the C++ struct.
cbuffer SSRTCB : register(b1)
{
    uint MaxSteps;
    uint MaxMips;
    uint UseDynamicCubemapsAsFallback;
    float Thickness;
    float NormalBias;
    float BRDFBias;
    float OcclusionStrength;
    float CubemapNormalization;
    // --- row 2 ---
    uint FreezeNoisePhase;
    uint UseBlueNoise;
    // Non-zero switches on the temporal accumulation of the confidence signal.
    uint TemporalAmbientConfidence;
    // 1 / (AmbientConfidenceMaxFrames + 1): the floor on the accumulator's blend weight.
    float AmbientConfidenceInvMaxFrames;
}
#endif

#ifndef SSRT_CONF_EXTERNAL_FILTER
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
// (reinjection noise) ...and why the spatial mean is not enough on its own.
//
// The paragraph that used to stand here refused a temporal pass, on the grounds that it would
// import nearest-neighbour reprojection with no disocclusion test into a signal whose job is to
// be stable. Both halves of that have since changed.
//
// The premise is stale: the temporal pass it was pointing at has gained a bilinear gather with
// per-tap validation, a plane-distance disocclusion test and a 30 degree normal gate, so
// "reprojection means ghosting" is no longer the state of this feature.
//
// And the conclusion was measurably wrong, because of *where* the consumer sits.
// DeferredCompositeCS reads this surface as `ambientKeep = 1 - conf * strength` and multiplies
// the reconstructed ambient by it -- and the composite runs *after* SVGF/REBLUR. So the ~0.07
// residual above is a multiplicative noise source downstream of the entire denoiser: no
// denoiser can reach it, whatever it is set to. That is the whole of the reported "reinjection
// looks noisier than leaving it off, and neither denoiser helps": switching reinjection on adds
// an undenoised noise term, and it is multiplicative on the ambient rather than additive on the
// radiance, which is why it also looks different in kind.
//
// A temporal mean is therefore not a refinement here, it is the only remaining place to remove
// that noise. It also fixes a frequency mismatch the spatial-only chain always had: a per-frame
// noisy weight modulating an already-accumulated radiance. Over the same window both are the
// same estimator, which is strictly more correct than the pairing it replaces.
//
// The objection about lagging the ambient/GI boundary behind moving geometry is answered by the
// disocclusion test rather than by declining to accumulate. This accumulator carries its own
// reference depth in the history (.y), so it can reject a stale sample *without* depending on
// texHistoryDepth or texHistoryNormals -- which matters because those are SVGF-only surfaces
// that are neither allocated nor updated under REBLUR, the default, while this pass runs on
// every denoiser path. A rejected pixel resets to the spatial mean, i.e. exactly the previous
// behaviour, so a silhouette still resolves in one frame in the band that actually moved.
#define SSRT_CONF_BLUR_RADIUS 3
#define SSRT_CONF_TILE (8 + 2 * SSRT_CONF_BLUR_RADIUS)  // 14

// (reinjection noise) Relative linear-depth agreement required between the reference depth a
// history texel stored and the depth this pixel's surface *should* have had in the previous
// frame. Both sides are linear view depth from SharedData::GetScreenDepth, so the comparison is
// one quantity by construction.
//
// 0.05, i.e. tighter than the 0.1 the spatial kernel uses, and for a different reason. The
// spatial test compares taps up to 3 texels apart on the current frame; this one compares the
// same surface point against itself one frame earlier, so the only legitimate difference is the
// sub-texel parallax between the exact reprojected position and the integer texel actually
// loaded -- at most ~0.7 texel, which is under 1e-2 of relative depth even on a grazing surface
// (the per-texel relative depth change is pixelAngularSize * |slope| and distance independent,
// the same derivation as SSRT_CONF_DEPTH_TOLERANCE). 0.05 clears that by 5x while still sitting
// an order of magnitude under a real depth step.
//
// Erring permissive is also much cheaper here than it would be for radiance: accepting a wrong
// tap blends one frame of a neighbouring surface's *coverage* at weight alpha <= 1/2 into a
// smooth [0,1] field, where the same mistake on radiance is a ghost.
#define SSRT_CONF_HISTORY_DEPTH_TOLERANCE 0.05f

// (reinjection noise) The history's .y is the linear view depth divided by this. Purely an
// overflow guard: distant LOD terrain reaches depths past fp16's 65504 ceiling, and 1/4096 puts
// the representable range past 2.6e8 game units. Floating-point relative precision is scale
// invariant, so the division costs nothing in accuracy -- it only moves where the format runs
// out of exponent.
#define SSRT_CONF_DEPTH_STORE_SCALE 4096.0f

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
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID, uint3 groupID : SV_GroupID)
{
#ifndef SSRT_CONF_EXTERNAL_FILTER
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
#endif

    // (audit P9) SharedData::BufferDim.xy is the same full-resolution extent
    // ColorTextureRW.GetDimensions() returned (this UAV is the kMAIN render target), so
    // this is output-identical while dropping a per-thread resource query.
    if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
        return;

#ifndef SSRT_CONF_EXTERNAL_FILTER
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
        const float spatialConf = weightSum > 0.0f ? saturate(sum / weightSum) : centreConf;

        // ---- (reinjection noise) temporal accumulation of the spatial mean ----
        // Published value defaults to the spatial mean, so every path that declines to
        // accumulate -- switch off, unbound history, rejected history, out of the
        // dynamic-resolution sub-rect -- reproduces the pre-existing behaviour exactly.
        float publishedConf = spatialConf;

        [branch] if (TemporalAmbientConfidence != 0)
        {
            // Reconstruct this pixel's surface point and ask where it was last frame. Same two
            // steps, same matrices and same order as SSRTBuildHistoryPlane in ssrt_temporal.hlsl:
            // one unprojection through CameraViewProjInverse into camera-relative world space,
            // then a rebase onto the previous frame's world origin (which is what
            // CameraPreviousViewProjUnjittered is defined against) and forward through it. No
            // matrix is composed and none is inverted.
            const float2 uv = (float2(dispatchID.xy) + 0.5f) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
            const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
            const float rawDepth = DepthTexture[dispatchID.xy];

            const float2 eyeUV = Stereo::ConvertFromStereoUV(uv, eyeIndex);
            const float2 thisNDC = (eyeUV - 0.5f) * float2(2.0f, -2.0f);
            const float4 posRW4 = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4(thisNDC, rawDepth, 1.0f));
            const float3 posRW = posRW4.xyz / posRW4.w;
            const float3 prevWorld = posRW +
                                     FrameBuffer::CameraPosAdjust[eyeIndex].xyz -
                                     FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xyz;
            const float4 prevClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(prevWorld, 1.0f));
            const float3 prevNDC = prevClip.xyz / prevClip.w;
            // The depth this surface point *should* have been seen at last frame. Linearised with
            // the same helper the stored side was written through, so both ends of the comparison
            // below are the same quantity.
            const float expectedPrevZ = SharedData::GetScreenDepth(prevNDC.z);

            // Where to look, from the motion vectors rather than from prevNDC.xy -- the same
            // choice ReprojectHit makes, and deliberately the same criterion the radiance chain
            // uses, so the two agree about what counts as the same surface. Point load, not a
            // filtered sample: a bilinear tap on the motion target averages two surfaces wherever
            // its footprint straddles a silhouette and lands between both (defect D3).
            const int2 motionMax = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy) - 1;
            const int2 motionPixel = clamp(int2(dispatchID.xy), int2(0, 0), motionMax);
            const float2 velocity = MotionVectorTexture[motionPixel].xy;
            const float2 thisScreen = (uv - 0.5f) * float2(2.0f, -2.0f);
            const float2 prevUV = (thisScreen + velocity * float2(2.0f, -2.0f)) * float2(0.5f, -0.5f) + 0.5f;

            // .zw, not .xy: the previous frame's dynamic-resolution ratio, which is what the
            // history was written at (audit #16).
            const float2 prevRenderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.zw;
            const int2 prevPixel = int2(prevUV * prevRenderSize);

            const float4 history = ConfidenceHistoryTexture[prevPixel];
            const float storedZ = history.y * SSRT_CONF_DEPTH_STORE_SCALE;
            const float prevFrames = history.z;

            // Single flat predicate, no early return anywhere in this block. Two reasons: a
            // return inside a second conditional block lets fxc prove the accept path dead and
            // strip the history bindings outright (the shape ssrt_temporal.hlsl's IsValidHistory
            // is built around), and the writes at the end have to happen on both outcomes.
            //
            // Finiteness by bit test rather than isfinite(), as everywhere else here: fxc may
            // assume its inputs finite without /Gis, so the guard has to look at the bits. The
            // freshly-allocated history is undefined memory, so this is load bearing on frame one
            // even though the C++ side also clears it.
            bool historyValid =
                isFiniteSafe(posRW4) && abs(posRW4.w) > 1e-9f &&
                isFiniteSafe(prevClip) && abs(prevClip.w) > 1e-9f &&
                isFiniteSafe(prevNDC) &&
                // A point whose previous image fell outside the previous depth range had no
                // history at all, so no texel can match it.
                prevNDC.z > 0.0f && prevNDC.z < 1.0f &&
                isFiniteSafe(expectedPrevZ) && expectedPrevZ > 1e-6f &&
                // Off the previous frame's screen, or off this frame's dynamic-resolution
                // sub-rect (lanes between the sub-rect and BufferDim still publish a spatial mean
                // above, but they have no meaningful reprojection and must not seed history).
                all(prevUV >= 0.0f) && all(prevUV <= 1.0f) &&
                all(prevPixel >= int2(0, 0)) && all(prevPixel < int2(prevRenderSize)) &&
                all(dispatchID.xy < uint2(screen_size)) &&
                isFiniteSafe(history) &&
                // accumFrames 0 is the "cleared / never written" state the C++ side and every
                // rejection produce, so it reads as absent rather than as a zero-confidence
                // sample.
                prevFrames > 0.0f &&
                isFiniteSafe(storedZ) &&
                abs(storedZ - expectedPrevZ) <= SSRT_CONF_HISTORY_DEPTH_TOLERANCE * expectedPrevZ;

            // alpha = 1/(n+1) until the window length is reached, then the constant floor. The
            // counter form is what makes a fresh pixel converge as 1/n from its very first frame
            // instead of crawling in at the steady-state rate, so a reset costs a handful of
            // frames rather than the whole window.
            const float accumFrames = historyValid ? prevFrames : 0.0f;
            const float alpha = max(1.0f / (accumFrames + 1.0f), AmbientConfidenceInvMaxFrames);
            publishedConf = saturate(lerp(historyValid ? history.x : spatialConf, spatialConf, alpha));

            // Capped rather than left to grow: past the window length the alpha floor makes a
            // larger count meaningless, and an unbounded counter in a float channel is the exact
            // shape of the defect D5 latch (and of the R8_UINT wrap in Skylighting's
            // UpdateProbesCS). 1/AmbientConfidenceInvMaxFrames is the window length plus one,
            // which fp16 represents exactly for any value this setting can take.
            const float nextFrames = min(accumFrames + 1.0f, rcp(AmbientConfidenceInvMaxFrames));
            ConfidenceHistoryRW[dispatchID.xy] =
                float4(publishedConf, centreDepth / SSRT_CONF_DEPTH_STORE_SCALE, nextFrames, 0.0f);
        }

        SSRTConfidenceSmoothRW[dispatchID.xy] = publishedConf;
    }
#endif

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
