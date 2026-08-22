#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> HistoryTexture : register(t0);
// (spec A1) Moments texture written by ssrt_temporal.hlsl: .xy = luminance moments,
// .z = accumulated frame count. t1 used to receive the motion-vector target, which this
// shader never declared or read, so the slot was free.
Texture2D<float4> MomentsTexture : register(t1);
Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);
#if !defined(SSRT_SPECULAR)
// (batch 1, item 2) The per-pixel diffuse hit distance. What it stores is
// t / (t + SSRT_HITT_REF_TEXELS), where t is the correlation length of the light in render
// texels at this pixel's depth; see that constant in ssrt_common.hlsli for the derivation and
// the use site below for how the confidence is recovered from it.
//
// Diffuse only. The specular chain has its own hit distance on a different surface with
// different semantics -- texHitDistance is R32_FLOAT in game units with a 65536 miss sentinel,
// and Upscaling.cpp hands it to DLSS-RR as the specular guide -- and its kernel is already
// sized by roughness, which is the specular equivalent of this mechanism. Wiring it in belongs
// with the specular pipeline split, not here.
Texture2D<float> HitDistanceTexture : register(t5);
#endif

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
    // (defect D1 / diagnostic D3 / diagnostic H) Read by ssrt_temporal.hlsl only. Declared here
    // so the two views of the buffer keep matching offsets -- this shader used to close row 2
    // with a `float3 denoiserPad1`, which it can no longer do now that it needs a field on
    // row 3.
    float historyClampSigma;
    uint disableHistoryDepthTest;
    uint disableHistoryNormalTest;
    // --- row 3 ---
    uint forceAcceptHistory;
    uint rotatedNormalGate;
    uint historyDebugView;
    // (batch 1, item 2) Strength of the hit-distance kernel narrowing; 0 makes the mechanism
    // exactly inert. Took the last pad slot of row 3, so DenoiserCB did not grow.
    float hitRadiusStrength;
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
// One incidental gain: CalculateWeight's phiDepth was set to atrousStride, i.e. to the
// distance of an axis-aligned |k| = 1 tap. Under the 5x5 kernel the |k| = 2 taps were
// therefore judged with a phiD half their true distance -- twice as strict as intended, an
// inconsistency audit #12 did not reach. A 3-tap kernel has no |k| = 2 taps, so phiDepth was
// nearly right for every tap it took (the diagonals were still judged at 1/sqrt(2) of their
// distance). (defect D6) Bringing the 5-tap kernel back makes the error load bearing again,
// so phiDepth is now the tap's actual texel distance -- see its use site.
//
// (defect D6) The diffuse permutation is now compiled with SSRT_SVGF_KERNEL_5X5=1, i.e. the
// 3x3 kernel above is no longer what the diffuse chain runs. The S2 measurement is not wrong;
// its premise was.
//
// S2 traded reach for taps on the assumption that a 0.71x narrower filter was an acceptable
// price. At 2 spp it is not. Reference SVGF (Schied et al. 2017) runs five iterations of the
// 5-tap B3 spline for a second-moment sigma of ~18 px, because that is what a one-to-two
// sample-per-pixel radiance signal needs; S2 left this chain at sigma 1.58 px, an order of
// magnitude short, and the acceptance test that cleared it ran on a build whose variance
// channel was still broken (BUG-1) -- so the kernel that "lost nothing" was being compared
// against a filter that was already not filtering. It has never been evaluated on a working
// chain.
//
// Restoring the 5-tap kernel at the unchanged AtrousIterations default of 2 gives sigma
// 2.24 px for 50 taps per pixel, i.e. 1.4x the reach of the 3x3 chain and two thirds of the
// 75 taps this pass cost before S2. AtrousIterations 3 reaches sigma 3.74 px if that is still
// not enough. The specular permutation keeps the 3x3 kernel: its edge-stops are scaled by
// roughness precisely so that a near-delta lobe is *not* blurred, so extra reach there is
// either annihilated by the weights (spec S3) or actively wrong.
//
// Set SSRT_SVGF_KERNEL_5X5=0 to get the 3x3 chain back.
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
// (defect D6) With the 5-tap kernel back as the diffuse default, its LDS cap moved from A4's
// stride 2 to stride 3, so the whole of the AtrousIterations 3 chain stayed on LDS instead of
// falling back to per-tap global loads on its last iteration. The tile grew to
// 8 + 2 * 2 * 3 = 20 texels a side, i.e. 400 float4 = 6400 bytes.
//
// (perf 2) THAT COVERAGE WAS BOUGHT WITH OCCUPANCY, WHICH IS THE WRONG CURRENCY HERE
//
// "Far inside the 32 KB a cs_5_0 group may hold" was the wrong budget to check it against. The
// binding limit is not what one group may hold, it is how many groups an SM can host at once,
// and a groupshared allocation is charged for the whole dispatch whether or not this particular
// iteration's stride uses it. The tile is sized for the *worst* stride the shader can be
// dispatched with, because `atrousIterations` is a constant-buffer field and a groupshared array
// is not; so at the default AtrousIterations 2 -- strides 1 and 2, which need 12 x 12 and
// 16 x 16 -- every group was still reserving the 20 x 20 that only stride 3 asks for.
//
// An 8x8 group is two warps, and the resident-warp cap and the shared-memory carveout bound the
// group count together. Taking a 64 KB carveout (and a 100 KB one, since the driver picks):
//
//   tile side   bytes   groups/SM @64K   warps   groups/SM @100K   warps
//      20        6404         10          20/48        15          30/48
//      16        4100         15          30/48        24 (cap)    48/48
//
// i.e. cutting the cap to stride 2 takes the diffuse a-trous pass from 42% to 62% of peak
// occupancy on a 64 KB carveout, and from 62% to 100% on a 100 KB one -- on a pass that runs
// twice per frame and whose 25 taps per lane are exactly the kind of work that needs resident
// warps to hide.
//
// What it costs: AtrousIterations 3 and above now take the per-tap global path on their stride-3
// iteration, 50 loads per lane instead of 12.5 tile texels, at full occupancy. That is a genuine
// trade rather than a strict win, and it is taken in the direction of the default (2), where
// there is no trade at all -- both iterations keep the tile *and* gain the occupancy. The
// specular permutation moves 14 -> 12 a side by the same arithmetic and its stride-3 fallback is
// 18 loads per lane rather than 50, since its kernel has 9 taps.
//
// The output does not move either way: SSRTSpatialFetchGuide's two paths return the same
// float4 for the same texel, and the tap loop's own `inside` test means a tap outside the render
// sub-rect -- the only place the tile's fill convention and the G-buffer disagree -- is never
// fetched through either path.
//
// The maximum tile index a tap can produce is 7 + 2 * 2 + 2 * 2 = 15 at the 5x5 kernel's stride
// 2, which is the last row, so the bound stays exact rather than generous.
#if SSRT_SVGF_KERNEL_5X5
#   define SSRT_SPATIAL_LDS_MAX_STRIDE 2
#else
#   define SSRT_SPATIAL_LDS_MAX_STRIDE 2
#endif
// 16 with the 5x5 kernel, 12 with the 3x3 one.
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

        // ---- (batch 1, item 2) the kernel radius as a function of hit distance ----
        //
        // WHY THE RADIUS SHOULD DEPEND ON IT AT ALL
        //
        // The kernel is asking one question per tap: may pixel P's indirect radiance be averaged
        // with pixel Q's? The honest answer depends on the *distance the light came from*.
        // Diffuse irradiance from a source at world distance L varies over a length scale of
        // order L, so two pixels separated by less than L worth of world space see almost the
        // same integrand and averaging them is free, while two pixels separated by much more
        // than L see different ones and averaging them is a blur. That threshold, expressed in
        // render texels at this pixel's depth, is what HitDistanceTexture carries -- see
        // SSRT_HITT_REF_TEXELS for the derivation and for why the surface stores a reciprocal of
        // it rather than the number itself.
        //
        // Concretely, at 1920 px and a 100 degree horizontal FOV with the camera 1000 units out
        // (one texel = 1.24 game units), and at the default AtrousIterations 2 and
        // HitRadiusStrength 4, the mechanism produces:
        //     ray length   correlation   beta (stride 1 / 2)   chain sigma
        //         5 u         4 texels        3.20 / 3.56          1.382 px
        //        20 u        16 texels        1.99 / 2.66          1.536 px
        //       100 u        81 texels        0.66 / 1.13          1.873 px
        //       500 u       403 texels        0.16 / 0.30          2.127 px
        //       miss          (n/a)           0.00 / 0.00          2.236 px  (unmodified)
        // i.e. a contact bounce keeps roughly the reach the 3x3 chain defect D6 replaced had
        // (1.58 px), a mid-range bounce keeps most of the 5-tap chain's, and a ray that missed and
        // came back with cubemap radiance keeps all of it -- which is where most of the residual
        // noise lives, and where the widest kernel is not merely safe but wanted. One number, two
        // opposite prescriptions, and before this the filter applied the same width to both.
        //
        // The same table is depth-adaptive in the right direction, which is the property that
        // makes it a screen-space criterion rather than a world-space guess: with the camera 100
        // units out a 5-unit bounce is 40 texels of correlation and gets 1.72 px, and at 4000
        // units the same bounce is one texel and gets 1.33 px. Every figure here is single
        // precision out of the CPU harness in the branch's scratchpad, including the R8_UNORM
        // round trip, not derived by hand.
        //
        // HOW IT IS APPLIED: A WINDOW, NOT A DIFFERENT KERNEL
        //
        // The tap *positions* cannot vary per pixel -- the loop bounds are compile-time
        // constants, the spec A4 groupshared tile is sized for a group-uniform stride, and the
        // a-trous chain's hole-free coverage property depends on the stride schedule. What can
        // vary per pixel is the tap *weights*, so the mechanism is a radial window multiplied
        // into them:
        //     g(k) = exp(-beta * |k|^2 / R^2),    beta = HitRadiusStrength * (1 - f),
        // with R = SSRT_SPATIAL_KERNEL_RADIUS and f in [0, 1] the confidence that this
        // iteration's reach is inside the light's correlation length,
        //     f = t / (t + SSRT_HITT_KERNEL_REACH * R * stride),
        // recovered from the stored reciprocal below. Three properties make this the right shape:
        //
        //   * It is separable. exp(-beta*(kx^2+ky^2)/R^2) = g(kx) * g(ky), so it folds into the
        //     per-axis kernel weight table and the tap loop below is character-for-character
        //     what it was. Three exp() calls per pixel, not one per tap.
        //   * f = 1 gives beta = 0 gives g == 1 *exactly* -- exp(0) is 1.0 in IEEE-754, not
        //     nearly 1.0 -- so a distant or missed hit reproduces the unmodified kernel bit for
        //     bit. HitRadiusStrength 0 does the same for every pixel. The mechanism can only
        //     ever narrow the filter, never widen it, and only where the ray was short.
        //   * It cannot collapse to a copy. beta is bounded by HitRadiusStrength, so the inner
        //     ring keeps exp(-S/R^2) of its weight however close the hit was: at the default
        //     S = 4 and R = 2 that is exp(-1) = 0.37, and at the slider's maximum S = 8 it is
        //     still exp(-2) = 0.135. A contact pixel gets a narrower filter, never no filter --
        //     which matters, because contact pixels are 2-spp noisy like every other pixel.
        //
        // WHAT IT COSTS IN REACH, WITH NUMBERS
        //
        // Effective second moment per axis, m2(beta) = sum(k^2 h_k g_k) / sum(h_k g_k), for the
        // 5-tap B3 spline in this file's unit-centre normalisation h = {1, 2/3, 1/6}:
        //     beta = 0  (f = 1):        sum h = 2.6667,  sum k^2 h = 2.6667,  m2 = 1.0000
        //     beta = 1:                        2.1610,             1.5289,   m2 = 0.7075
        //     beta = 2  (f = 0.5, S=4):        1.8538,             0.9892,   m2 = 0.5336
        //     beta = 4  (f = 0,   S=4):        1.4966,             0.5149,   m2 = 0.3441
        //     beta = 8  (f = 0,   S=8):        1.1806,             0.1809,   m2 = 0.1532
        // Per-iteration variances add along the chain and scale with stride^2, so at the default
        // AtrousIterations 2 (strides 1 and 2, spec S2's table) the chain's second-moment sigma
        // goes 2.236 / 1.881 / 1.633 / 1.312 / 0.875 px across those five cases. For reference the
        // 3x3 chain defect D6 replaced was 1.58 px. Computed in single precision by the CPU
        // harness in the branch's scratchpad, not by hand.
        //
        // Energy: g_k is a positive multiplier on a weight that is renormalised by weightSum
        // below, so the output stays a convex combination of its taps. The local mean is
        // preserved exactly, and varianceSum / weightSum^2 remains the variance of a weighted
        // mean. Nothing about the energy or variance bookkeeping changes.
        //
        // The specular permutation does not take part at all -- not "takes part with beta 0".
        // Handing it a runtime-zero beta made fxc materialise the weight table as an indexable
        // temp and cost that permutation four instruction slots for a mechanism it does not use,
        // so the table and its indexing are compiled out and the tap expression aliases the
        // static kernel directly. Its generated code is then unchanged.
#if !defined(SSRT_SPECULAR)
#   define SSRT_TAP_KERNEL tapKernel
        float tapKernel[SSRT_SPATIAL_KERNEL_RADIUS + 1];
        {
            // The stored value is u = t / (t + SSRT_HITT_REF_TEXELS) with t the correlation
            // length in texels; see the derivation at that constant for why the encoding is a
            // reciprocal rather than a linear scale. The confidence this iteration wants is
            //     f = t / (t + REACH * hardRadius),
            // and substituting t = REF * u / (1 - u) clears both fractions:
            //     f = REF * u / (REF * u + REACH * hardRadius * (1 - u)).
            // Written that way there is nothing to guard. The denominator is a sum of two
            // non-negative terms and cannot be zero: u = 0 makes the second term
            // REACH * hardRadius > 0, and u = 1 makes the first REF > 0. The two endpoints are
            // exact -- u = 0 gives f = 0 and u = 1 gives f = REF / REF = 1, hence beta = 0 and
            // the unmodified kernel, which is what a missed ray is entitled to.
            //
            // hardRadius is this iteration's own reach, so the same correlation length is judged
            // more strictly by the later, wider iterations. That is the correct direction: it is
            // the wide iterations that reach across a contact feature, and the narrow first one
            // is doing the work that a 2-spp signal genuinely needs.
            const float hitEncoded = HitDistanceTexture[DTid.xy];
            const float hardRadius = max(float(SSRT_SPATIAL_KERNEL_RADIUS * atrousStride), 1.0f);
            const float hitNumer = SSRT_HITT_REF_TEXELS * hitEncoded;
            const float hitFactor = hitNumer / (hitNumer + SSRT_HITT_KERNEL_REACH * hardRadius * (1.0f - hitEncoded));
            const float beta = hitRadiusStrength * (1.0f - hitFactor);
            const float rcpRadiusSq = 1.0f / float(SSRT_SPATIAL_KERNEL_RADIUS * SSRT_SPATIAL_KERNEL_RADIUS);
            [unroll] for (int kr = 0; kr <= SSRT_SPATIAL_KERNEL_RADIUS; kr++)
                tapKernel[kr] = kernelWeights[kr] * exp(-beta * float(kr * kr) * rcpRadiusSq);
        }
#else
#   define SSRT_TAP_KERNEL kernelWeights
#endif

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
                        // (defect D6) phiD is this tap's distance in texels, which is what
                        // CalculateWeight's contract asks for: the expected same-surface depth
                        // delta grows linearly with distance, so dividing by the distance is
                        // what turns weightDepth into a pure measure of surface slope that no
                        // longer changes with the tap offset or the a-trous stride. It used to
                        // be atrousStride for every tap, which is the distance of an
                        // axis-aligned |k| = 1 tap only -- so a |k| = 2 tap was judged twice as
                        // strictly as intended and a diagonal by sqrt(2). The error was mild
                        // while the kernel had no |k| = 2 taps; with the 5-tap kernel back it
                        // costs a grazing-incidence surface most of its outer ring (weight
                        // exp(-2.8) = 0.06 where exp(-1.4) = 0.25 was intended), i.e. exactly
                        // the wide reach the kernel was restored for.
                        const float phiDepth = length(float2(kx, ky)) * atrousStride;
                        // (batch 1, item 2) SSRT_TAP_KERNEL is kernelWeights with the per-pixel
                        // hit-distance window folded in on the diffuse permutation -- and it *is*
                        // kernelWeights, bit for bit, whenever the window is inert -- and the
                        // static kernelWeights itself on the specular one. See the derivation
                        // above.
                        float weight = CalculateWeight(depthCenter, sampleDepth, phiDepth, normalVS, sampleNormalVS, phiNormal, luminanceCenter, luminanceP, phiLuminance) * SSRT_TAP_KERNEL[abs(kx)] * SSRT_TAP_KERNEL[abs(ky)];

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