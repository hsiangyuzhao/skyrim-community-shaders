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
    // --- row 2 ---
    // (spec S3) Roughness at or below which a specular pixel counts as mirror-like;
    // 0 disables the mechanism.
    float specularRoughnessCutoff;
    float3 denoiserPad1;
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
//
// (spec S2) Re-checked against the 3x3 a-trous kernel. The prescription itself is
// unaffected: Schied's pre-blur is a fixed 3x3 on the variance channel at every a-trous
// level, independent of the level's kernel or stride, so nothing about it needs to change.
// What changes is its share of the pass. The pre-blur's 9 loads used to sit next to 25 tap
// loads (~26% of the texture traffic); next to 9 they are ~50%, and the fxc delta below is
// unchanged in absolute terms while the pass around it got cheaper. So the switch matters
// more than it did, and the recommendation to leave it on is unchanged: what it buys is the
// removal of an artefact class (variance-driven blotching), not smoothing.
//
// Also worth noting for a future round: at stride 1 the pre-blur's +-1 footprint coincides
// exactly with the 3x3 kernel's taps, so iteration 0 loads the same nine texels twice. It
// cannot simply be folded in, because phiLuminance has to be known before the tap loop
// starts -- it would take a second LDS tile for the .w channel.
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

// (spec S2) 3x3 dilated a-trous, with the original 5x5 kept behind a switch.
//
// The pass used to run the 5-tap-per-axis B3 spline [1,4,6,4,1]/16 -- 25 taps per pixel
// per iteration -- at a stride that grows one texel per iteration (atrousStride =
// atrousIterations + 1, where the constant carries the *loop index*, so the schedule is
// 1, 2, 3, ...). The textbook a-trous formulation for a wavelet chain is the 3-tap
// binomial [1,2,1]/4 instead: 9 taps per iteration, and the dilation is what recovers the
// reach.
//
// The two things worth checking before making that swap are coverage and bandwidth.
//
// Coverage. A 3-tap kernel at stride s reaches +-s, so consecutive iterations at strides
// 1 and 2 have supports {-1,0,1} and {-2,0,2}, whose convolution is {-3..3} -- every
// integer offset, no holes. That is the property that makes a 3-tap chain legitimate and
// it survives this codebase's linear stride schedule (1, 2, 3, ...) as well as it does the
// classic 2^i one. The *hard* radius after N iterations is sum(i = 1..N) i = N(N+1)/2
// against the 5-tap kernel's N(N+1), i.e. exactly half. So 3 iterations of 3x3 have the
// same 6-texel support radius as 2 iterations of 5x5, for 27 taps instead of 50.
//
// Bandwidth. Per axis the discrete second moment is sum(k^2 * h_k): 1.0 for [1,4,6,4,1]/16
// and 0.5 for [1,2,1]/4, both at stride 1, scaling with s^2. Summing over the chain (the
// iterations are independent, so variances add):
//   5x5, 2 iters: 1*(1+4)     = 5.0   -> sigma 2.24 px   (50 taps/px)
//   5x5, 3 iters: 1*(1+4+9)   = 14.0  -> sigma 3.74 px   (75 taps/px)
//   3x3, 2 iters: 0.5*(1+4)   = 2.5   -> sigma 1.58 px   (18 taps/px)
//   3x3, 3 iters: 0.5*(1+4+9) = 7.0   -> sigma 2.65 px   (27 taps/px)
// So the 3x3 kernel at the current AtrousIterations default of 2 is a 0.71x narrower
// filter for 36% of the taps, and at 3 iterations it is a 1.18x *wider* one for 54% of the
// taps. The report recommends 3 for parity; the default is left at 2 per the user
// directive that existing defaults do not move.
//
// One incidental gain: CalculateWeight's phiDepth is set to atrousStride, i.e. to the
// distance of a |k| = 1 tap. Under the 5x5 kernel the |k| = 2 taps were therefore judged
// with a phiD half their true distance -- twice as strict as intended, an inconsistency
// audit #12 did not reach. A 3-tap kernel has no |k| = 2 taps, so phiDepth is exactly
// right for every tap it takes.
//
// Set SSRT_SVGF_KERNEL_5X5=1 to restore the 25-tap kernel bit-for-bit, including its LDS
// tile geometry.
#ifndef SSRT_SVGF_KERNEL_5X5
#   define SSRT_SVGF_KERNEL_5X5 0
#endif

#if SSRT_SVGF_KERNEL_5X5
#   define SSRT_SPATIAL_KERNEL_RADIUS 2
// B3 spline [1,4,6,4,1]/16, normalised to a unit centre tap.
static const float kernelWeights[SSRT_SPATIAL_KERNEL_RADIUS + 1] = { 1.0, 2.0 / 3.0, 1.0 / 6.0 };
#else
#   define SSRT_SPATIAL_KERNEL_RADIUS 1
// Binomial [1,2,1]/4, normalised to a unit centre tap. The separable 3x3 it forms is
// 1 : 1/2 : 1/4 for centre : edge : corner.
static const float kernelWeights[SSRT_SPATIAL_KERNEL_RADIUS + 1] = { 1.0, 1.0 / 2.0 };
#endif

#define VAR_EPSILON 0.00001f

// (BUG-2) Reference-luminance floor for A1's *relative* convergence test below.
//
// A pure relative test asks for sigma^2 < eps * L^2, which demands sigma^2 -> 0 as the
// pixel goes black and would therefore make near-black tiles never skip -- the exact
// inverse of the darkness gate it replaces. Floor the reference luminance instead: below
// 1% of a mid-grey linear radiance the SSRT term contributes less than 1% of a surface's
// indirect light, and its noise is invisible at any relative level. At the default
// AdaptiveVarianceEps this puts an effective absolute floor of 1.3e-2 * 1e-4 = 1.3e-6 on
// the threshold (sigma 1.1e-3) -- 75x *below* the old absolute 1e-4, so it acts as a floor
// and not as a gate.
#define SSRT_ADAPTIVE_LUM_FLOOR 0.01f

// (spec A1) Group-wide convergence vote.
//
// This pipeline compiles as cs_5_0 through fxc, which has no wave intrinsics, so the
// "is every lane of this tile converged?" question cannot be answered with
// WaveActiveAllTrue. It is answered with a groupshared counter instead: every one of the
// 64 lanes of the 8x8 group contributes 1 when it considers itself converged, and only a
// count of exactly 64 authorises the whole group to take the early path.
//
// Whole-group granularity is deliberate, not a fallback. A partial early-out inside a
// group would save nothing: the surviving lanes still execute the kernel and the converged
// ones just idle through it, so the group's cost is unchanged. Only skipping a group in
// its entirety removes work.
//
// (spec S2) The semantics are unchanged by the move to a 3x3 kernel -- the vote is about
// the *input*, not about the kernel, and the early path still copies colour and variance
// verbatim. Two consequences are worth recording, neither of which needs code:
//   * The prize shrinks. A skipped group now avoids 9 taps per lane instead of 25, so the
//     vote's own cost (two barriers and an atomic) is amortised over less saved work. It
//     still pays -- the reduction is a handful of instructions against nine guided taps
//     plus their weight evaluation -- but the A-layer's measured win scales down with it.
//   * Slightly fewer tiles qualify at iterations >= 1. The carried variance shrinks by
//     roughly sum(w^2)/sum(w)^2 per iteration, which is larger for 9 taps than for 25, so
//     .w falls towards adaptiveVarianceEps more slowly down the chain.
//
// Both barriers below are executed unconditionally by every lane. That is what forces
// the restructuring of the two early-outs further down (they now happen *after* the
// reduction): a `return` before a GroupMemoryBarrierWithGroupSync() would make the
// barrier non-uniform across the group, which is undefined behaviour.
groupshared uint g_ssrtConvergedLanes;

#if defined(SSRT_SPECULAR)
// (spec S3) Group-wide "is this whole tile mirror-like?" vote.
//
// The specular path already scales its edge-stopping functions by roughness, and the two
// scalings both diverge as roughness goes to zero:
//     phiLuminance *= roughness;   phiNormal /= roughness;
// A tap's weight is exp(-weightDepth - |dL| / phiLuminance) * dot(n, nP)^phiNormal, so at
// the default ColorPhi 2.0 and NormalPhi 512:
//   * at roughness 0.05, phiNormal = 10240, and a tap needs its normal within 0.81 deg of
//     the centre's to keep even 1/e of its weight;
//   * phiLuminance = 2.0 * 0.05 * sqrt(variance) = 0.1 * sigma, so a tap a single standard
//     deviation from the centre keeps exp(-10) = 4.5e-5 of its weight. (The conclusion is
//     unchanged by the BUG-1 ColorPhi re-tune from 0.5: the roughness factor is what
//     dominates here, and 0.1 * sigma is still a 10x-tighter gate than the diffuse path's
//     2.0 * sigma.)
// Both conditions together mean the kernel already returns very nearly the centre pixel:
// the centre tap has weight 1 by construction, every other tap is annihilated by one term
// or the other, and weightSum ends up ~1 with blendedColor ~ ssrColor.rgb. Running 9 (or
// 25) guided taps to compute a value the shader already holds is pure waste, and on a
// mirror it is the *only* thing those taps could do that is not harmful -- the whole point
// of the roughness scaling is that a near-delta reflection lobe must not be blurred.
//
// So this is not a quality/performance trade in the usual sense: it removes work whose
// output is already, by construction, indistinguishable from the input. What it buys is
// the water surfaces, polished metal, glass and ice of a Skyrim scene skipping the a-trous
// chain outright.
//
// Same construction as A1: no wave intrinsics under cs_5_0, so a groupshared counter, and
// only a unanimous 64 authorises the skip. Out-of-bounds and far-plane lanes count as
// mirror-like so they cannot veto a tile they take no part in -- exactly as they count as
// converged for A1 -- and they still take their own dedicated branches afterwards.
groupshared uint g_ssrtMirrorLanes;
#endif

// (spec A4, re-derived for spec S2) Groupshared depth/normal tile for the a-trous taps.
//
// The taps of an 8x8 group overlap heavily, so their depth and normal fetches want to be
// shared. The footprint is *stride dependent*: a tap sits at DTid + k * stride with
// |k| <= R and stride = atrousIterations + 1, so the group needs a
// (8 + 2 * R * stride) square, not the fixed 8 + 2 * R an unstrided kernel would need.
//
// S2 halves R from 2 to 1, which changes both terms of the trade:
//
//   * The halo shrinks from 2 * stride to stride per side, so the tile goes from
//     8 + 4 * stride to 8 + 2 * stride texels a side -- 100 / 144 / 196 / 256 / 324 entries
//     at strides 1..5 where the 5x5 kernel needed 144 / 256 / 400 / 576 / 784.
//   * The demand halves too: 64 lanes x 9 taps = 576 tap fetches per group instead of
//     1600. Global loads per lane (two per tap or per tile texel) therefore go 18 -> 3.1
//     at stride 1, -> 4.5 at stride 2, -> 6.1 at stride 3, -> 8.0 at stride 4,
//     -> 10.1 at stride 5. Break-even is (8 + 2s)^2 = 576, i.e. stride 8, so LDS is still
//     a win across the whole legal AtrousIterations range of 1..5 -- but with diminishing
//     returns, and the LDS pressure is what caps it.
//
// The cap moves from stride 2 to stride 3: 14 x 14 float4 = 3136 bytes, *less* LDS than
// A4's 16 x 16 = 4096 while covering one stride more. That reaches AtrousIterations 3,
// which is both the current default of 2 and the value the S2 report recommends should
// the shorter 3x3 reach need compensating. Strides 4 and 5 fall back to per-tap loads
// exactly as before.
//
// Under SSRT_SVGF_KERNEL_5X5 the original geometry is restored verbatim (cap 2, 16 x 16),
// so the fallback path is bit-identical.
#if SSRT_SVGF_KERNEL_5X5
#   define SSRT_SPATIAL_LDS_MAX_STRIDE 2
#else
#   define SSRT_SPATIAL_LDS_MAX_STRIDE 3
#endif
// 16 with the 5x5 kernel, 14 with the 3x3 one.
#define SSRT_SPATIAL_TILE (8 + 2 * SSRT_SPATIAL_KERNEL_RADIUS * SSRT_SPATIAL_LDS_MAX_STRIDE)

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
    // sky pixel pays the full a-trous kernel. Write 0 rather than just
    // returning: this shader ping-pongs between two textures, so skipping the write
    // would leave the previous iteration's (or previous frame's) content behind.
    float depthCenter = inBounds ? DepthTexture[DTid.xy] : 1.0f;
    const bool isFarPlane = SSRT_IS_FAR_PLANE(depthCenter);

    float4 ssrColor = inBounds ? SSRColorTexture[DTid.xy] : 0.0f;

    // (BUG-2) Hoisted above the A1 reduction, which needs it as the reference for the
    // relative convergence test. It used to be computed just before the tap loop; the
    // value is identical (same texel, same function), so for a filtering lane this is a
    // pure move and a skipping lane pays one extra dot product. An out-of-bounds lane sees
    // ssrColor 0 and hence luminance 0, which the floor below covers.
    float luminanceCenter = Color::RGBToLuminance(ssrColor.rgb);

#if defined(SSRT_SPECULAR)
    // (spec S3) The centre pixel's guide, hoisted above the reduction because the mirror
    // vote needs the roughness. Same fetch, same clamp, same values as when it sat below
    // the early-outs -- only earlier -- so the filtered output is unaffected. Guarded on
    // inBounds so an out-of-range lane does not issue a pointless load; the initialiser is
    // what it would have read anyway (DecodeNormal of a zero G-buffer texel is 0, and
    // 1 - 0 = 1 before the clamp), and such a lane returns before using either.
    //
    // The hoist is deliberately confined to this permutation: the diffuse one has no
    // roughness term in its kernel and no vote to feed, so it keeps the original site and
    // stays byte-identical.
    float3 normalVS = 0.0f;
    float roughness = 1.0f;
    if (inBounds)
        GetNormalRoughness(DTid.xy, normalVS, roughness);
    roughness = clamp(roughness, 0.001f, 1.0f);
#endif

    // ---- (spec A1) convergence reduction; no lane may leave before it completes ----
    if (all(GTid.xy == 0)) {
        g_ssrtConvergedLanes = 0;
#if defined(SSRT_SPECULAR)
        g_ssrtMirrorLanes = 0;  // (spec S3)
#endif
    }
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
        // .w is the per-pixel variance the ping-pong carries (audit #11 made it real,
        // BUG-1 made it correctly *scaled*); it shrinks with every a-trous iteration, so a
        // tile that is not quiet enough to skip iteration 0 may still skip iteration 1 or 2.
        //
        // (BUG-2) The test is now RELATIVE: adaptiveVarianceEps is a squared coefficient of
        // variation, compared against the local mean luminance squared, rather than an
        // absolute luminance-squared threshold.
        //
        // The old absolute form was a darkness gate rather than a convergence test.
        // Monte-Carlo radiance noise is multiplicative -- sigma scales with the mean for a
        // given sample count -- so an absolute threshold selects on brightness, not on how
        // noisy a pixel is. Concretely, before BUG-1 the channel held roughly
        // 0.0285 * mean^2 + 0.0294 * sigma^2, and at the 2-spp diffuse default (sigma about
        // 1.4 * mean) that is 0.0861 * mean^2, so `< 1e-4` reduced to `mean < 0.034`: A1
        // fired on dark pixels and only on dark pixels, regardless of their noise. Fixing
        // BUG-1 does not rescue the absolute form -- it makes it stricter still
        // (sigma^2 < 1e-4 with sigma = 1.4 * mean is mean < 0.0071) -- so the criterion had
        // to be re-derived, not just re-defaulted.
        //
        // The relative form costs one multiply and one max on top of a luminance that the
        // shader already needed, so it is the cheap option as well as the correct one. It
        // preserves both halves of A1's contract: a converged *and* quiet tile skips, and a
        // tile whose relative noise is still above the line keeps filtering at every
        // iteration until the chain has brought .w down.
        const float lumRef = max(luminanceCenter, SSRT_ADAPTIVE_LUM_FLOOR);
        laneConverged = accumFrames >= adaptiveHistoryThreshold && ssrColor.w < adaptiveVarianceEps * lumRef * lumRef;
    }
    if (laneConverged)
        InterlockedAdd(g_ssrtConvergedLanes, 1u);

#if defined(SSRT_SPECULAR)
    // (spec S3) The mirror vote rides on the same two barriers as A1's, so it costs one
    // atomic and no extra synchronisation. The *clamped* roughness is compared, so a
    // perfectly smooth G-buffer texel reads as 0.001 rather than 0 -- which is what the
    // kernel itself sees, and therefore the right quantity to threshold.
    const bool laneMirror = !inBounds || isFarPlane || roughness <= specularRoughnessCutoff;
    if (laneMirror)
        InterlockedAdd(g_ssrtMirrorLanes, 1u);
#endif
    GroupMemoryBarrierWithGroupSync();

    const bool groupConverged = g_ssrtConvergedLanes == 64u;
    bool skipFilter = adaptiveFiltering != 0 && groupConverged;
#if defined(SSRT_SPECULAR)
    // (spec S3) Folded into A1's skip so the two mechanisms share the copy path *and* the
    // suppression of the A4 prefetch below -- a skipped group reads nothing at all.
    // specularRoughnessCutoff == 0 leaves the whole thing inert.
    skipFilter = skipFilter || (specularRoughnessCutoff > 0.0f && g_ssrtMirrorLanes == 64u);
#endif

    // ---- (spec A4) tap guide prefetch ----
    const uint atrousStride = atrousIterations + 1;
    const bool useLDS = atrousStride <= SSRT_SPATIAL_LDS_MAX_STRIDE;
    // Both conditions are group uniform -- skipFilter comes from the vote above and
    // useLDS from a constant buffer -- so no lane fills the tile for nothing. They guard
    // LDS *writes* only; the barrier that publishes them stays unconditional.
    if (!skipFilter && useLDS) {
        const uint tileDim = 8 + 2 * SSRT_SPATIAL_KERNEL_RADIUS * atrousStride;
        const int2 tileOrigin = int2(Gid.xy) * 8 - int(SSRT_SPATIAL_KERNEL_RADIUS * atrousStride);
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

#if !defined(SSRT_SPECULAR)
    float3 normalVS;
    float roughness;
    GetNormalRoughness(DTid.xy, normalVS, roughness);
    roughness = clamp(roughness, 0.001f, 1.0f);
#endif
    // (spec S3) The specular permutation fetched normalVS / roughness above the reduction,
    // because its mirror vote needs the roughness before any lane may leave.

    // (BUG-2) luminanceCenter is computed above the A1 reduction, which needs it.
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

        for (int ky = -SSRT_SPATIAL_KERNEL_RADIUS; ky <= SSRT_SPATIAL_KERNEL_RADIUS; ky++)
        {
            for (int kx = -SSRT_SPATIAL_KERNEL_RADIUS; kx <= SSRT_SPATIAL_KERNEL_RADIUS; kx++)
            {
                // A-Trous sampling
                int2 samplePos = int2(DTid.xy) + int2(kx, ky) * int(atrousStride);
                bool inside = (samplePos.x >= 0 && samplePos.y >= 0) && (samplePos.x < screen_size.x && samplePos.y < screen_size.y);
                if (inside)
                {
                    float4 sampleSSRColor = SSRColorTexture[samplePos];
                    // (spec A4) Tile coordinate of this tap; the tile origin sits
                    // radius * stride texels before the group, so the offset cancels out.
                    const int2 tileCoord = int2(GTid.xy) + int2(kx, ky) * int(atrousStride) + int(SSRT_SPATIAL_KERNEL_RADIUS * atrousStride);
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