#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

// ---------------------------------------------------------------------------------------------
// (batch 1, item 1) PRE-BLUR: the spatial filter that runs *before* the temporal accumulation.
//
// WHY THE CHAIN NEEDS ONE AT ALL
//
// Every shipped real-time GI denoiser in the reference set puts its most important spatial
// filter in front of the temporal pass, not behind it. REBLUR's pass order is
// pre-blur -> temporal -> History Fix -> blur -> post-blur -> Temporal Stabilization; RELAX's is
// PrePass -> temporal accumulation -> History Fix -> History Clamping -> Anti-Firefly -> A-Trous.
// In both, the pre-pass is mandatory rather than optional, and the reason is a property of the
// temporal accumulator rather than a quality preference:
//
//   The accumulation is an EMA. Whatever variance the input carries is written into a buffer
//   that the *next* frame reads back and re-blends. A 2-spp radiance sample has a per-frame
//   relative sigma around 0.7, so the history chain is being fed, sixty times a second, a
//   signal whose noise is of the same order as its mean -- and the accumulation's own job is
//   to average that down over MaxAccumulatedFrames. Every mechanism that has to *judge* the
//   history then has to do so through that noise: the D1 neighbourhood colour clamp compares
//   the history against a nine-tap mean of it, the moment pair that steers the whole a-trous
//   chain is estimated from it, and the accumFrames ramp decides how much of it to trust.
//   Halving the input sigma before any of that happens makes all three of them better
//   conditioned at once, and it is the only place in the chain where one filter buys three
//   improvements.
//
// This pass is therefore *not* "one more a-trous iteration moved earlier". Its output has a
// different consumer (the accumulator and the moments, not the frame) and so it has different
// design constraints, which is why it is its own shader rather than a re-ordering of
// ssrt_spatial.hlsl:
//
//   * It cannot be variance guided. There is no variance yet -- computing it is what the
//     temporal and variance passes downstream exist for -- so the luminance edge-stop that
//     ssrt_spatial.hlsl's kernel is built around has nothing to key off. Worse, a luminance
//     edge-stop evaluated on a raw 2-spp signal actively preserves the outliers this pass is
//     supposed to remove: a firefly *is* a large luminance difference, so the weight function
//     would protect it. Only geometry (depth, normal) may steer a pre-blur, which is exactly
//     what REBLUR and RELAX use in their pre-passes.
//   * It must be small. The industry pre-blur is a sharpening operation in effect, not a
//     smoothing one -- it removes the extreme tail of the sample distribution so the history
//     chain is not poisoned, and leaves the actual denoising to the accumulation plus the
//     a-trous chain that can see the variance. A wide pre-blur would destroy exactly the
//     contact-scale detail that batch 1 item 2 exists to protect. The kernel below is a
//     3x3 at stride 1, i.e. a second-moment sigma of 0.71 texels against the 2.24 the
//     diffuse a-trous chain runs (see the derivation at the kernel).
//   * It has to own the anti-firefly step, and that is the one non-obvious consequence.
//     See the next block.
//
// WHY THE FIREFLY CLAMP MOVED IN HERE
//
// The firefly clamp (spec S1) lives in ssrt_temporal.hlsl, in front of the accumulation. Put
// an unconditional spatial filter in front of *that* and the clamp stops working, for a reason
// that is arithmetic rather than subtle:
//
//   A lone spike of luminance X with dark neighbours, run through the 3x3 kernel below
//   (centre 1, edge 1/2, corner 1/4, sum 4), comes out as 0.25X at its own pixel, 0.125X at
//   each edge neighbour and 0.0625X at each corner. The clamp downstream then measures the
//   centre 0.25X against a neighbourhood whose mean is now 0.094X and whose sigma is 0.031X,
//   so at K = 3 the limit is 0.187X and the centre is cut to that. Total energy left in the
//   patch: 0.187 + 4(0.125) + 4(0.0625) = 0.94X. The firefly has been attenuated by 6%
//   instead of the ~25x the clamp achieves on the raw signal, and what is left is a nine-texel
//   blob whose *local variance is low* -- so the variance-guided a-trous chain downstream will
//   happily keep spreading it. That is the "slowly fading bright blob" spec S1 was written to
//   prevent, reproduced spatially.
//
// The clamp cannot be fixed by also capping the eight neighbour taps, because S1's statistic
// provably cannot flag a member of its own reference set: the limit is
// max(mean_8 + K * sigma_8, max_8), the largest possible deviation of one of eight samples
// from their own mean is sigma * sqrt(7) = 2.646 * sigma, and the max_8 floor covers the rest.
// At any K the eight neighbours pass their own test by construction. The only correct fix is
// to clamp each tap against *its own* eight neighbours, which needs a two-texel halo -- and
// that is what the tile below carries.
//
// So the pass is "anti-firefly + pre-blur", which is the REBLUR PrePass shape. The formula,
// the K, and the centre-excluded-from-its-own-statistics rule are S1's, unchanged, and the
// C++ side hands this pass the real FireflyClampSigma and hands the temporal pass 0 whenever
// this pass runs -- so the mechanism is applied exactly once and the setting keeps its exact
// meaning. With the pre-blur off, ssrt_temporal.hlsl gets the real value back and the chain is
// bit-identical to what it was before this pass existed.
//
// WHAT THIS PASS DELIBERATELY DOES NOT TOUCH
//
//   * .w of the radiance surface. On the ray march's output that channel is the hit
//     confidence, and its own consumer (ssrt_diffuse_composite.hlsl, through
//     texSSRTDiffuseConfidence) reads a separate surface that this chain never writes. The
//     temporal pass reads only .rgb from here and overwrites .w with the variance, so the
//     value is carried through from the centre texel verbatim rather than filtered: filtering
//     it would change nothing any consumer reads, and copying it keeps the surface a faithful
//     stand-in for its input.
//   * The D1 history clamp's reference neighbourhood. ssrt_temporal.hlsl builds that box from
//     the *raw* ray-march surface, bound separately, precisely so that this pass does not
//     perturb D1's calibration -- see the note at RawColorTexture in that file.
// ---------------------------------------------------------------------------------------------

Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);

RWTexture2D<float4> PreBlurOutput : register(u0);

// Mirrors ScreenSpaceRayTracing::DenoiserCB. Two rows are declared: normalPhi steers the
// normal edge-stop and fireflyClampSigma the outlier rejection. A shader may declare a prefix
// of a larger constant buffer, which is what ssrt_variance.hlsl (one row) and
// ssrt_spatial.hlsl (two) already do.
cbuffer DenoiserCB : register(b2)
{
    float invMaxAccumulatedFrames;
    uint atrousIterations;
    float colorPhi;
    float normalPhi;
    // --- row 1 ---
    uint adaptiveFiltering;
    float adaptiveHistoryThreshold;
    float adaptiveVarianceEps;
    // (spec S1, relocated) Firefly clamp width in neighbourhood standard deviations. 0 leaves
    // the outlier stage and its halo statistics out entirely, which is the state the temporal
    // pass is left in whenever this pass runs.
    float fireflyClampSigma;
};

// The blur's own radius. One texel, stride 1 -- see the "must be small" argument above.
#define SSRT_PREBLUR_RADIUS 1

// Two texels of halo, not one. The blur reads a 10x10 neighbourhood for an 8x8 group, and the
// outlier test needs the 3x3 of every one of those 100 texels, so the tile has to reach
// 8 + 2 * 2 = 12 texels a side. That extra ring is the entire cost of doing the anti-firefly
// step correctly rather than approximately.
#define SSRT_PREBLUR_TILE (8 + 2 * (SSRT_PREBLUR_RADIUS + 1))  // 12

// .rgb = radiance, overwritten in place by the outlier stage; .w = the *raw* Rec.709 luminance
// the outlier statistics are defined over.
//
// .w is deliberately kept as its own channel and deliberately left stale after the clamp. It is
// stored because the statistics need it 9 times per texel and recomputing a dot product per read
// would be 9x the ALU; it is left stale because nothing downstream of the clamp reads it -- the
// blur consumes .rgb only -- and rewriting it would be a second LDS store per texel for a value
// with no consumer.
groupshared float4 g_ssrtPreBlurColor[SSRT_PREBLUR_TILE * SSRT_PREBLUR_TILE];

// (perf 5) THE GUIDE TILE IS 10x10, NOT 12x12, AND THAT ASYMMETRY IS THE POINT
//
// The two tiles have different readers, so they have different footprints, and sizing the guide
// to match the colour tile was reserving a ring nothing reads.
//
//   * The *colour* tile genuinely needs the full 12 x 12. The outlier stage tests each of the 100
//     interior texels against its own 3 x 3, so it reaches one texel beyond the 10 x 10 the blur
//     will read -- that outer ring is the entire cost of doing the anti-firefly step correctly
//     rather than approximately, and it stays.
//   * The *guide* tile is read by the blur and by nothing else: the centre lane's own entry, and
//     the nine taps at centreTile +- 1 with centreTile in [2, 9]. That is tile indices 1..10 on
//     each axis. The outer ring was filled every frame and never read.
//
// LDS goes 4608 -> 3904 bytes a group, which at a 64 KB shared carveout is 14 -> 16 groups per SM
// (28 -> 32 of 48 warps) and at 100 KB is 21 -> 24, the resident-group cap. The indices shift by
// one against the colour tile's, which is why the mapping gets its own macro rather than an
// open-coded `- 1` at each of the three sites.
//
// Same construction and the same "depth 0 means outside the render sub-rect, and the tap loop's
// `> 0` test rejects it" convention as ssrt_spatial.hlsl's spec A4 tile and ssrt_variance.hlsl's
// spec A3 one; the values stored and read are unchanged texel for texel.
#define SSRT_PREBLUR_GUIDE_DIM (8 + 2 * SSRT_PREBLUR_RADIUS)  // 10
// Maps a colour-tile coordinate (0..11) to a guide-tile index. Only defined for the 1..10
// interior, which is the whole of what the blur touches.
#define SSRT_PREBLUR_GUIDE_INDEX(c) (((c).y - 1u) * SSRT_PREBLUR_GUIDE_DIM + ((c).x - 1u))
// .xyz = view-space normal, .w = raw depth.
groupshared float4 g_ssrtPreBlurGuide[SSRT_PREBLUR_GUIDE_DIM * SSRT_PREBLUR_GUIDE_DIM];

// Binomial [1, 2, 1] / 4, normalised to a unit centre tap -- the same kernel, in the same
// normalisation, that ssrt_spatial.hlsl runs when SSRT_SVGF_KERNEL_5X5 is off.
//
// Second moment per axis, sum(k^2 * h_k) / sum(h_k) with h = {1, 1/2}: 1 / 2 = 0.5, i.e. a
// sigma of 0.71 texels. Against the diffuse a-trous chain's 2.24 texels (two iterations of the
// 5-tap B3 spline at strides 1 and 2, spec S2's table) this pass contributes 10% of the total
// filtering variance -- which is the point. It removes the tail of the sample distribution
// before the accumulator sees it and leaves the actual smoothing to the passes that can measure
// what they are smoothing.
static const float kPreBlurWeights[SSRT_PREBLUR_RADIUS + 1] = { 1.0, 1.0 / 2.0 };

// (spec S1, relocated) One texel's outlier test, against the eight neighbours of its own tile
// position. Identical in formula, in K, and in the centre-excluded-from-its-own-reference-set
// rule to SSRTClampFirefly in ssrt_temporal.hlsl; see the derivation there for why the centre
// must be excluded, why the maxNeighbour floor exists, and why only luminance is clamped while
// the rgb triple is scaled (hue and saturation survive, the pixel just gets dimmer).
//
// The one difference is *which* texel is being tested. In the temporal pass only the lane's own
// pixel is, because that is the only value entering the accumulation there. Here every texel the
// blur will read has to be tested, or the blur spreads the outliers of the ones that were not.
float3 SSRTPreBlurClampFirefly(uint2 tileCoord, float sigmas)
{
    const uint c = tileCoord.y * SSRT_PREBLUR_TILE + tileCoord.x;
    const float3 radiance = g_ssrtPreBlurColor[c].rgb;

    float sum = 0.0f;
    float sumSq = 0.0f;
    float maxNeighbour = 0.0f;

    [unroll] for (int y = -1; y <= 1; y++)
    {
        [unroll] for (int x = -1; x <= 1; x++)
        {
            if (x == 0 && y == 0)
                continue;  // the texel under test is not part of its own reference distribution

            const float l = g_ssrtPreBlurColor[(tileCoord.y + y) * SSRT_PREBLUR_TILE + (tileCoord.x + x)].w;
            sum += l;
            sumSq += l * l;
            maxNeighbour = max(maxNeighbour, l);
        }
    }

    const float invCount = 1.0f / 8.0f;
    const float mean = sum * invCount;
    const float sigma = sqrt(max(sumSq * invCount - mean * mean, 0.0f));
    const float limit = max(mean + sigmas * sigma, maxNeighbour);

    // (guard G3) The finiteness test on the luminance is load bearing and is not a paranoia
    // check: for lum = +Inf the comparison lum > limit passes, limit / lum evaluates to exactly
    // 0, and radiance * 0 is Inf * 0 = NaN in every channel -- so the mechanism whose job is to
    // remove outliers would be manufacturing the one value the persistent history can never get
    // rid of. Leaving the Inf in place hands it to the guards that can dispose of it (G2
    // upstream sanitises the ray march output, G4 downstream refuses to accept it as history).
    const float lum = g_ssrtPreBlurColor[c].w;
    return (isFiniteSafe(lum) && lum > limit) ? radiance * (limit / lum) : radiance;
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    const uint2 screen_size = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
    const bool inBounds = DTid.x < screen_size.x && DTid.y < screen_size.y;

    const float depthCenter = inBounds ? DepthTexture[DTid.xy] : 1.0f;
    const bool isFarPlane = SSRT_IS_FAR_PLANE(depthCenter);

    // Group uniform, so every branch it guards is a scalar branch costing one comparison per
    // group rather than per lane.
    const bool clampFireflies = fireflyClampSigma > 0.0f;

    const uint tid = GTid.y * 8 + GTid.x;
    const int2 tileOrigin = int2(Gid.xy) * 8 - int(SSRT_PREBLUR_RADIUS + 1);
    const uint tileTexels = SSRT_PREBLUR_TILE * SSRT_PREBLUR_TILE;  // 144

    // ---- stage 1: fill both tiles ----
    // Every lane runs this unconditionally. The bounds and far-plane early-outs are below the
    // barriers for the reason recorded all over this feature: a `return` before a
    // GroupMemoryBarrierWithGroupSync() makes the barrier non-uniform across the group, which
    // is undefined behaviour.
    for (uint t = tid; t < tileTexels; t += 64) {
        const int2 local = int2(int(t % SSRT_PREBLUR_TILE), int(t / SSRT_PREBLUR_TILE));
        const int2 p = tileOrigin + local;
        const bool valid = all(p >= 0) && all(p < int2(screen_size));

        // Clamp-to-edge for the radiance, exactly as ssrt_temporal.hlsl's spec S1 tile does:
        // the statistics it feeds are an outlier test, not an energy-preserving average, so
        // duplicating a border texel costs nothing and saves the eight extra predicates a
        // validity flag would need inside the inner test. A duplicated texel can never reach
        // the blur output either, because the *guide* below marks it invalid.
        const int2 pc = clamp(p, int2(0, 0), int2(screen_size) - 1);
        const float3 radiance = SSRColorTexture[pc].rgb;
        g_ssrtPreBlurColor[t] = float4(radiance, Color::RGBToLuminance(radiance));

        // (perf 5) The guide is only wanted over the 10 x 10 the blur reads, so the outer ring of
        // the colour tile's footprint neither loads nor stores one. The predicate is on `local`,
        // the tile coordinate, so it costs the fill loop two comparisons per iteration and saves
        // the 44 border texels their normal decode and their depth load as well as their slot.
        const bool guideWanted = all(local >= 1) && all(local <= int2(SSRT_PREBLUR_GUIDE_DIM, SSRT_PREBLUR_GUIDE_DIM));
        if (guideWanted) {
            // Depth 0 for a texel outside the render sub-rect. The tap loop's `> 0` test then
            // rejects it, and the zero normal is a second line of defence: it drives
            // weightNormal = pow(max(0, dot(n, 0)), phiNormal) to exactly 0.
            float3 tileNormal = 0.0f;
            float tileDepth = 0.0f;
            if (valid) {
                float tileRoughness;
                GetNormalRoughness(uint2(p), tileNormal, tileRoughness);
                tileDepth = DepthTexture[p];
            }
            g_ssrtPreBlurGuide[SSRT_PREBLUR_GUIDE_INDEX(uint2(local))] = float4(tileNormal, tileDepth);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    // ---- stage 2: the outlier stage, over the 10x10 the blur will read ----
    // In place in g_ssrtPreBlurColor.rgb, which is race free: the value written at a tile index
    // depends on the *radiance* at that index only (owned by this lane for this iteration) and
    // on the *luminance* of its neighbours, which lives in .w and is never written here. So no
    // lane reads a radiance another lane is writing, and a second tile would buy nothing.
    if (clampFireflies) {
        const uint interiorDim = 8 + 2 * SSRT_PREBLUR_RADIUS;  // 10
        const uint interiorTexels = interiorDim * interiorDim;  // 100
        for (uint j = tid; j < interiorTexels; j += 64) {
            const uint2 tileCoord = uint2(j % interiorDim, j / interiorDim) + 1u;
            const float3 cleaned = SSRTPreBlurClampFirefly(tileCoord, fireflyClampSigma);
            g_ssrtPreBlurColor[tileCoord.y * SSRT_PREBLUR_TILE + tileCoord.x].rgb = cleaned;
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- both tiles are final; early returns are safe from here on ----

    if (!inBounds)
        return;

    // Sky and far plane: written rather than skipped, so the surface stays deterministic for
    // every texel the dispatch covers. The temporal pass takes its own far-plane branch and
    // never reads this value, but leaving the previous frame's content behind would make the
    // surface a misleading thing to inspect in the Buffer Viewer.
    if (isFarPlane) {
        PreBlurOutput[DTid.xy] = 0.0f;
        return;
    }

    const uint2 centreTile = uint2(GTid.xy) + (SSRT_PREBLUR_RADIUS + 1);
    const float4 centreColor = g_ssrtPreBlurColor[centreTile.y * SSRT_PREBLUR_TILE + centreTile.x];
    const float3 centreNormalVS = g_ssrtPreBlurGuide[SSRT_PREBLUR_GUIDE_INDEX(centreTile)].xyz;

    float3 blended = 0.0f;
    float weightSum = 0.0f;

    for (int ky = -SSRT_PREBLUR_RADIUS; ky <= SSRT_PREBLUR_RADIUS; ky++)
    {
        for (int kx = -SSRT_PREBLUR_RADIUS; kx <= SSRT_PREBLUR_RADIUS; kx++)
        {
            const uint2 tap = uint2(int2(centreTile) + int2(kx, ky));
            const uint ti = tap.y * SSRT_PREBLUR_TILE + tap.x;
            const float4 guide = g_ssrtPreBlurGuide[SSRT_PREBLUR_GUIDE_INDEX(tap)];
            const float sampleDepth = guide.w;
            if (sampleDepth > 0.0f)
            {
                // The depth and normal edge-stops are the chain's own, unchanged: the same
                // CalculateWeight the a-trous pass and the variance pass call, with the same
                // SSRT_DEPTH_WEIGHT_SCALE calibration and the same phiD convention (the tap's
                // actual distance in texels, defect D6).
                //
                // The luminance term is neutralised by handing it a pair of equal luminances
                // rather than by calling a second helper: |0 - 0| / 1 is exactly 0, so
                // weightLuminance vanishes and what is left is exp(-weightDepth) *
                // weightNormal. Reusing the audited function is deliberate -- it is what makes
                // "the pre-blur's edge-stop is the same edge-stop" a fact about the code rather
                // than a claim about two copies of it -- and fxc folds the dead term away, so
                // it costs nothing. See the block comment for why a *live* luminance term would
                // be actively wrong here.
                const float phiDepth = length(float2(kx, ky));
                const float w = CalculateWeight(depthCenter, sampleDepth, phiDepth,
                                                centreNormalVS, guide.xyz, normalPhi,
                                                0.0f, 0.0f, 1.0f) *
                                kPreBlurWeights[abs(kx)] * kPreBlurWeights[abs(ky)];

                blended += g_ssrtPreBlurColor[ti].rgb * w;
                weightSum += w;
            }
        }
    }

    // The centre tap's weight is 1 * 1 * 1 by construction -- phiDepth 0 makes CalculateWeight
    // take its own `(phiD == 0) ? 0` branch on the depth term, the normal term is
    // pow(dot(n, n), phiNormal) on a unit normal, and both kernel factors are 1 -- so weightSum
    // is normally >= 1 and the division is a convex combination of the nine taps. That is the
    // energy statement this pass needs and the whole of it: the output is a weighted mean with
    // non-negative weights summing to one, so it preserves the local mean exactly, cannot leave
    // the convex hull of its inputs, and therefore cannot exceed the SSRT_MAX_RADIANCE ceiling
    // guard G2 already imposed upstream.
    //
    // The fallback exists for the one case that statement does not cover: a G-buffer normal
    // whose decode is not quite unit length makes pow(|n|^2, phiNormal) underflow at large
    // NormalPhi, and a centre weight of 0 with every neighbour rejected on depth would divide by
    // zero. Copying the input through is the same answer ssrt_spatial.hlsl gives in its own
    // weightSum == 0 branch.
    const float3 result = (weightSum > 0.0f) ? (blended / weightSum) : centreColor.rgb;

    // .w carried through from the centre texel, not filtered; see the block comment.
    PreBlurOutput[DTid.xy] = float4(result, SSRColorTexture[DTid.xy].w);
}
