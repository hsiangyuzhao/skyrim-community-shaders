#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> HistoryTexture : register(t0);
Texture2D<float4> MotionVectorTexture : register(t1);
Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);
Texture2D<float4> HistoryMomentsTexture : register(t5); // moments in RG, frame count in B
Texture2D<float4> HistoryNormalsTexture : register(t6);

RWTexture2D<float4> FilteredOutput : register(u0);
RWTexture2D<float4> MomentsOutput : register(u1);

// Mirrors ScreenSpaceRayTracing::DenoiserCB. Two float4 rows are declared here, one
// more than before spec S1: fireflyClampSigma sits in the slot the A-layer left as
// padding. A shader may declare a prefix of a larger constant buffer, so the third row
// that ssrt_spatial.hlsl reads is simply left out.
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
    float fireflyClampSigma;
};

// (spec S1) Firefly clamp on the radiance entering the temporal accumulation.
//
// A firefly is a single pixel that drew one unlucky sample -- a near-specular bounce onto
// a torch flame, a grazing hit on a water highlight -- and came back one to three orders
// of magnitude brighter than every pixel around it. Temporal accumulation cannot remove
// it (it re-enters as a fresh outlier and then persists for MaxAccumulatedFrames), and the
// a-trous kernel does not remove it either: it *spreads* it, because the luminance
// edge-stopping term keys off sqrt(variance), which the outlier itself inflates. The
// result is the classic slowly-fading bright blob. Cutting the outlier at the point it
// enters the filter chain is the cheapest place to deal with it, and it is what makes the
// A2 default (AtrousIterations 3 -> 2) safe: fewer spatial iterations only hurt if there
// are spikes left for them to smear.
//
// Scheme: soft clamp against the mean and standard deviation of the 8 neighbours'
// luminance, with the centre pixel deliberately *excluded* from the statistics -- a
// firefly is uncorrelated between adjacent pixels, so letting it into its own reference
// distribution is what defeats the naive 3x3 version of this filter. (Concretely: with the
// centre included, a lone spike X surrounded by near-zero neighbours gives mean = X/9 and
// sigma = X*sqrt(8)/9 = 0.314X, so the limit mean + K*sigma only falls below X for
// K < 2.83 -- the filter can barely reach its own target.)
//
// Only luminance is clamped; the rgb triple is scaled by limit/lum, so hue and saturation
// survive and the pixel keeps looking like the thing it reflected, just dimmer.
//
// K (FireflyClampSigma) default 3.0. Two bounds pin it down:
//   * Lower: for n = 8 samples the largest possible deviation from the mean is
//     sigma*sqrt(n-1) = 2.646*sigma. Any K at or above that makes the limit unreachable by
//     a member of the reference set, i.e. the clamp can only ever fire on the centre --
//     which is precisely the intent. Below 2.646 the clamp starts biting values that a
//     neighbour also produced, i.e. real signal, which is why the maxNeighbour floor below
//     exists as a guard for users who lower K.
//   * Upper: sigma is itself an 8-sample estimate with ~26% relative error
//     (1/sqrt(2(n-1))), so an honest K must sit far enough above the noise floor that a
//     mis-estimated sigma does not let a 100x spike through. K = 3 puts the limit at
//     roughly 4x the neighbourhood mean for 1-spp GI noise (where sigma ~ mean), so a
//     genuine bright feature loses nothing while a two-orders-of-magnitude spike is cut
//     by ~25x.
// K = 0 disables the mechanism entirely (FireflyClamp off), and the prefetch below with it.
// (guard G5) Ceiling on the luminance entering the moment pair, chosen against the storage
// format rather than against the signal -- see the derivation at its use site.
#define SSRT_MOMENT_LUMINANCE_MAX 250.0f

#define SSRT_FIREFLY_RADIUS 1
#define SSRT_FIREFLY_TILE (8 + 2 * SSRT_FIREFLY_RADIUS)  // 10

// (spec S1) The 3x3 neighbourhoods of an 8x8 group overlap almost completely -- 64 lanes
// want 512 neighbour taps out of 100 distinct texels -- so the luminances are prefetched
// into LDS once instead of being loaded up to 9 times each. 400 bytes of LDS turns 8 extra
// global loads per lane into 100/64 = 1.6, and only the luminance is kept: that is all the
// clamp needs, and it is what makes the tile a quarter the size of A4's.
//
// The vote mirrors ssrt_variance.hlsl (spec A3): a fully-sky or out-of-bounds group has
// nobody to clamp, so it must not pay for the fill. cs_5_0/fxc has no wave intrinsics,
// hence a groupshared counter rather than WaveActiveAnyTrue. All three barriers are
// executed by every lane unconditionally, which is why the bounds and far-plane
// early-outs had to move below them.
groupshared uint g_ssrtFireflyLanes;
groupshared float g_ssrtFireflyTile[SSRT_FIREFLY_TILE * SSRT_FIREFLY_TILE];

float3 SSRTClampFirefly(float3 radiance, uint2 gtid, float sigmas)
{
    const int2 c = int2(gtid) + SSRT_FIREFLY_RADIUS;

    float sum = 0.0f;
    float sumSq = 0.0f;
    float maxNeighbour = 0.0f;

    [unroll] for (int y = -SSRT_FIREFLY_RADIUS; y <= SSRT_FIREFLY_RADIUS; y++)
    {
        [unroll] for (int x = -SSRT_FIREFLY_RADIUS; x <= SSRT_FIREFLY_RADIUS; x++)
        {
            if (x == 0 && y == 0)
                continue;  // the centre is not part of its own reference distribution

            const float l = g_ssrtFireflyTile[(c.y + y) * SSRT_FIREFLY_TILE + (c.x + x)];
            sum += l;
            sumSq += l * l;
            maxNeighbour = max(maxNeighbour, l);
        }
    }

    // Population mean and standard deviation over the 8 neighbours.
    const float invCount = 1.0f / 8.0f;
    const float mean = sum * invCount;
    const float sigma = sqrt(max(sumSq * invCount - mean * mean, 0.0f));

    // The floor is what keeps a low K honest: a value that at least one neighbour also
    // reached is, by the definition above, not a firefly, so the clamp never pushes the
    // centre below the brightest neighbour. For K >= sqrt(7) = 2.646 the term is provably
    // inert (see the derivation above), so at the default it costs one max and changes
    // nothing.
    const float limit = max(mean + sigmas * sigma, maxNeighbour);

    const float lum = Color::RGBToLuminance(radiance);
    // lum > limit >= 0 implies lum > 0, so the division is safe.
    //
    // (guard G3) ...but only against a *zero* divisor. The clamp was also, and much more
    // damagingly, an Inf-to-NaN converter: for lum = +Inf the test lum > limit passes,
    // limit / lum evaluates to exactly 0, and radiance * 0 is Inf * 0 = NaN in every
    // channel. So the mechanism whose entire job is to remove outliers was upgrading the
    // one outlier it cannot scale into the one value that poisons the persistent history
    // forever -- and it is on by default. Requiring lum to be finite before taking the
    // scaling path leaves the Inf in place for the output sanitisation (G2 upstream, G9
    // downstream) and the history rejection (G4) to deal with, which are the guards that
    // can actually dispose of it.
    //
    // Healthy data is untouched: isFiniteSafe(lum) is true for every ordered, finite
    // luminance, so the predicate reduces to the original lum > limit and the returned
    // expression is unchanged.
    return (isFiniteSafe(lum) && lum > limit) ? radiance * (limit / lum) : radiance;
}

bool IsValidHistory(uint2 pixel, float2 uv, float3 currNormalVS)
{
    // (audit #16) Every caller passes a pixel in the *history* textures, whose valid
    // sub-rectangle is the previous frame's dynamic-resolution extent -- hence
    // DynamicResolutionParams1.zw (previous width/height ratio) rather than .xy.
    uint2 prev_screen_size = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.zw;
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1)
        return false;

    if (pixel.x >= prev_screen_size.x || pixel.y >= prev_screen_size.y)
        return false;

    float3 prevNormalVS;
    float roughness;
    GetNormalRoughness(HistoryNormalsTexture, pixel, prevNormalVS, roughness);
    float normalDiff = dot(currNormalVS, prevNormalVS);
    if (normalDiff < 0.866f) // cos 30
        return false;

    return true;
}

// (guard G4) The self-healing half of the NaN guard set, and the only one that repairs
// damage rather than preventing it.
//
// texHistoryDiffuse / texHistoryMomentsDiffuse are read here and rewritten from this pass's
// own output every frame, which makes them a closed feedback loop: once a texel holds a NaN
// or an Inf, `lerp(prevColor, ssrColor, alpha)` reproduces it for every alpha < 1, so the
// pixel is dead for the rest of the process -- across cell changes, save loads and
// resolution changes -- no matter how clean the incoming radiance is. The 4-tap and 8-tap
// disocclusion fallbacks below then *spread* it, averaging a poisoned neighbour into a
// pixel that had nothing wrong with it, which is why the damage grows with camera motion.
//
// Treating a non-finite history sample as *absent* rather than as data breaks the loop.
// Every reader below already has a well-tested path for "no usable history": the direct
// reprojection falls through to the 4-tap search, the taps drop out of the average, and a
// pixel that finds nothing at all seeds a fresh chain from this frame's sample with
// accumFrames = 1 (the disocclusion path, which ssrt_variance.hlsl covers with its 7x7
// spatial estimator). So a poisoned pixel costs exactly one frame of accumulation and then
// rebuilds, instead of persisting forever.
//
// The out parameters are written unconditionally -- the caller must ignore them when the
// return value is false -- so that healthy data takes no extra copy.
//
// Also collapses the two separate HistoryMomentsTexture loads the call sites used to issue
// (one for .z, one for .xy) into one; same texel, same values, one fewer fetch.
bool SSRT_LoadHistory(uint2 pixel, out float4 color, out float2 moments, out float accumFrames)
{
    color = HistoryTexture[pixel];
    const float3 m = HistoryMomentsTexture[pixel].xyz;
    moments = m.xy;
    accumFrames = m.z;
    // Colour *and* moments, because the two textures poison independently: a NaN colour
    // ruins the output directly, while an Inf second moment ruins every a-trous weight the
    // variance channel steers (see G5). .w of the history colour is the variance the
    // previous frame published, so it belongs to the same test.
    return isFiniteSafe(color) && isFiniteSafe(m);
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID, uint3 GTid : SV_GroupThreadID, uint3 Gid : SV_GroupID)
{
    uint2 screen_size = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
    const bool inBounds = DTid.x < screen_size.x && DTid.y < screen_size.y;

    float2 uv = float2(DTid.xy + 0.5) * SharedData::BufferDim.zw * FrameBuffer::DynamicResolutionParams2.xy;
    uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);

    // (audit P1) Sky / far-plane early-out -- skips reprojection plus up to 13 history
    // probes. Both targets are written (rather than left alone) so the history and
    // moments textures stay deterministic on background pixels; a zero frame count
    // also means a pixel that later becomes geometry restarts accumulation cleanly.
    float depthCenter = inBounds ? DepthTexture[DTid.xy] : 1.0f;
    const bool isFarPlane = SSRT_IS_FAR_PLANE(depthCenter);

    // ---- (spec S1) firefly-clamp vote and prefetch; no lane may leave before they
    // ---- complete, because every barrier below must stay group uniform ----
    const bool clampFireflies = fireflyClampSigma > 0.0f;
    const bool laneClamps = clampFireflies && inBounds && !isFarPlane;

    if (all(GTid.xy == 0))
        g_ssrtFireflyLanes = 0;
    GroupMemoryBarrierWithGroupSync();

    if (laneClamps)
        InterlockedAdd(g_ssrtFireflyLanes, 1u);
    GroupMemoryBarrierWithGroupSync();

    // Guards LDS *writes* only; the barrier that publishes them stays unconditional.
    if (g_ssrtFireflyLanes != 0u) {
        const int2 tileOrigin = int2(Gid.xy) * 8 - SSRT_FIREFLY_RADIUS;
        for (uint ty = GTid.y; ty < SSRT_FIREFLY_TILE; ty += 8) {
            for (uint tx = GTid.x; tx < SSRT_FIREFLY_TILE; tx += 8) {
                // Clamp-to-edge rather than a validity flag: duplicating the border texel
                // costs nothing here, because the statistic it feeds is an outlier test
                // and not an energy-preserving average. A halo texel outside the
                // dynamic-resolution sub-rect would otherwise have to be excluded from
                // the count, which is 8 extra predicates per lane for a border effect.
                const int2 p = clamp(tileOrigin + int2(tx, ty), int2(0, 0), int2(screen_size) - 1);
                g_ssrtFireflyTile[ty * SSRT_FIREFLY_TILE + tx] = Color::RGBToLuminance(SSRColorTexture[p].rgb);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- vote and prefetch complete; early returns are safe from here on ----

    if (!inBounds)
        return;

    if (isFarPlane) {
        FilteredOutput[DTid.xy] = 0.0;
        MomentsOutput[DTid.xy] = 0.0;
        return;
    }

    float3 blendedColor = 0;
    float4 ssrColor = SSRColorTexture[DTid.xy];
    // (spec S1) Everything downstream -- the moments, the temporal blend, and the
    // no-history fallback write at the bottom -- consumes the clamped radiance, so the
    // outlier never enters the accumulation in the first place.
    //
    // (BUG-1) The ordering is unchanged and remains the correct one, but it now *matters*
    // where it used to be nearly inert: the clamped sample is what feeds curMoment, so the
    // variance the a-trous chain is steered by is the variance of the post-clamp
    // distribution. That is the honest quantity -- the filter should be told about the
    // signal it is actually being handed -- but it does mean the clamp's aggressiveness and
    // the filter's strength are now coupled, whereas before the fix the moments were too
    // degenerate for the clamp to move them measurably.
    if (clampFireflies)
        ssrColor.rgb = SSRTClampFirefly(ssrColor.rgb, GTid.xy, fireflyClampSigma);

    float3 normalVS;
    float roughness;
    GetNormalRoughness(DTid.xy, normalVS, roughness);

    float luminance = Color::RGBToLuminance(ssrColor.rgb);
    // (BUG-1a) The raw first and second luminance moments of *this frame's* sample, with
    // no scale factor. The `* 0.5` that used to sit here had no derivation behind it and
    // it is not in reference SVGF (Schied et al. 2017 accumulate mu1 = l and mu2 = l*l;
    // Falcor's SVGFReprojection does the same). Halving only mu2 would have been a pure
    // 2x variance underestimate; halving *both* is worse than that, because the estimator
    // y - x^2 is not homogeneous: scaling the pair by c gives
    //     c*E[l^2] - c^2*E[l]^2 = c*sigma^2 + c*(1 - c)*mean^2,
    // so at c = 0.5 the estimate becomes 0.5*sigma^2 + 0.25*mean^2 -- half the real noise
    // plus a term that depends only on how *bright* the pixel is. A converged, perfectly
    // noise-free surface at luminance 0.5 therefore reported variance 0.0625 while a noisy
    // dark one reported almost nothing, which is the exact inverse of what the edge-stop
    // and the A1 convergence vote need. The same pair is re-read by ssrt_variance.hlsl's
    // 7x7 spatial estimator (history <= 2), which averaged the scaled pairs and inherited
    // the identical 0.5*sigma^2 + 0.25*mean^2 error, so removing the factor here fixes
    // that path too -- there is nothing to compensate for on either side.
    //
    // (guard G5) The pair is stored in MomentsOutput, an R11G11B10_FLOAT target: R and G
    // carry a 5-bit exponent and a 6-bit mantissa, so the largest representable value is
    // 65024 and *anything above it is written back as +Inf, permanently*. The second moment
    // is a square, so the overflow point in luminance is sqrt(65024) = 255.0 -- reachable
    // by a single bright specular sample, no NaN or corruption required. Once .y is Inf the
    // variance is Inf - x^2 = Inf, the a-trous luminance edge-stop divides by sqrt(Inf),
    // every neighbour weight becomes 0 or NaN, and the moments EMA can never recover
    // because lerp(Inf, finite, alpha) stays Inf for every alpha < 1.
    //
    // 250 leaves 2% of headroom below the overflow point (250^2 = 62500 < 65024) and is
    // ~30x above the top of the radiance the firefly clamp lets through, so it can only
    // engage on values that were already outside the representable range of their own
    // storage. Both moments use the clamped luminance so the pair stays a consistent
    // (mu1, mu2) and the variance estimate y - x^2 keeps its sign.
    const float momentLuminance = min(luminance, SSRT_MOMENT_LUMINANCE_MAX);
    float2 curMoment = float2(momentLuminance, momentLuminance * momentLuminance);

    // Reproject UVs using motion vectors
    float2 prevUV = uv;
    ReprojectHit(MotionVectorTexture, LinearSampler, float3(uv, depthCenter), eyeIndex, prevUV);

    float4 prevColor = 0.f;
    float prevAccumFrames = 0.f;
    float2 prevMoments = float2(0.f, 0.f);
    // (audit #16) prevUV is normalised to the *previous* frame's render sub-rect, so it
    // must be scaled by the previous frame's DRS ratio (DynamicResolutionParams1.zw),
    // not the current one. With DLSS/DRS the two differ whenever the ratio moves, which
    // shifted the whole history lookup and silently invalidated reprojection.
    uint2 prevPixel = uint2(prevUV * SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.zw);
    bool valid = false;

    // (guard G4) A non-finite history sample is not history; leaving prevColor / prevMoments
    // / prevAccumFrames at their zero initialisation makes this fall through to the 4-tap
    // search exactly as a geometrically invalid reprojection does.
    float4 histColor;
    float2 histMoments;
    float histAccumFrames;
    if (IsValidHistory(prevPixel, prevUV, normalVS) &&
        SSRT_LoadHistory(prevPixel, histColor, histMoments, histAccumFrames))
    {
        prevColor = histColor;
        prevAccumFrames = histAccumFrames;
        prevMoments = histMoments;
        valid = true;
    }

    if (!valid)
    {
        int2 bilinOffset[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float weightSum = 0.f;
        [unroll(4)]
        for (int i = 0; i < 4; i++)
        {
            int2 neighborPixel = int2(prevPixel) + bilinOffset[i];
            // (guard G4) A non-finite tap is excluded from the average and from weightSum,
            // which is what stops a single poisoned texel from being spread into the
            // disoccluded pixels around it every time the camera moves.
            float4 neighborColor;
            float2 neighborMoments;
            float neighborAccumFrames;
            if (IsValidHistory(uint2(neighborPixel), prevUV, normalVS) &&
                SSRT_LoadHistory(uint2(neighborPixel), neighborColor, neighborMoments, neighborAccumFrames) &&
                neighborAccumFrames > 0.f)
            {
                prevColor += neighborColor;
                prevAccumFrames += neighborAccumFrames;
                prevMoments += neighborMoments;
                weightSum += 1.f;
            }
        }

        if (weightSum > 0.f)
        {
            prevColor /= weightSum;
            prevAccumFrames /= weightSum;
            prevMoments /= weightSum;
            valid = true;
        }
    }
    
    if (!valid)
    {
        float weightSum = 0.f;
            
        int2 offsets[8] =
        {
            int2(0, 2),
            int2(0, -2),
            int2(1, 1),
            int2(1, -1),
            int2(-1, 1),
            int2(-1, -1),
            int2(2, 0),
            int2(-2, 0)
        };

        [unroll(8)]
        for (int i = 0; i < 8; i++)
        {
            int2 neighborPixel = int2(prevPixel) + offsets[i];
            // (guard G4) Same per-tap rejection as the 4-tap search above.
            float4 neighborColor;
            float2 neighborMoments;
            float neighborAccumFrames;
            if (IsValidHistory(uint2(neighborPixel), prevUV, normalVS) &&
                SSRT_LoadHistory(uint2(neighborPixel), neighborColor, neighborMoments, neighborAccumFrames) &&
                neighborAccumFrames > 0.f)
            {
                prevColor += neighborColor;
                prevAccumFrames += neighborAccumFrames;
                prevMoments += neighborMoments;
                weightSum += 1.f;
            }
        }
        if (weightSum > 0.f)
        {
            prevColor /= weightSum;
            prevAccumFrames /= weightSum;
            prevMoments /= weightSum;
            valid = true;
        }
    }

    if (valid)
    {
        float alpha = max(1.0f / (prevAccumFrames + 1.0f), invMaxAccumulatedFrames);
        blendedColor = lerp(prevColor.rgb, ssrColor.rgb, alpha);

        // (BUG-1b) Continue the *accumulated* moment chain. `prevMoments` is the EMA that
        // every history path above has just resolved out of HistoryMomentsTexture.xy --
        // directly for a valid reprojection, or as the mean over the surviving neighbours
        // in the two fallbacks -- and until now it was read and then thrown away. What the
        // lerp consumed instead was a pair rebuilt from `prevColor`, the already *filtered*
        // history colour:
        //     prevMoment = (P, P*P)   with P = luminance(prevColor)
        // whose own variance y - x^2 is identically zero by construction. Feeding that in
        // as the running estimate resets the second moment to the square of the first every
        // single frame, so the recursion never accumulates: only the newest sample's
        // contribution survives, and the steady-state estimate collapses to
        //     alpha * (1 - alpha/2)*mean^2 ... i.e. O(alpha) * sigma^2
        // instead of sigma^2. At the default MaxAccumulatedFrames 16 (alpha = 1/17) that is
        // a ~34x underestimate of the real per-frame noise -- enough that the a-trous
        // luminance edge-stop annihilated every non-centre tap and the whole spatial chain
        // degenerated into a copy.
        //
        // Both halves of BUG-1 have to be fixed together. Dropping the 0.5 alone still
        // leaves the reset-every-frame recursion (alpha*sigma^2, 17x low); switching to
        // prevMoments alone still leaves the 0.25*mean^2 brightness bias.
        //
        // Note that `prevMoments` is a moment of the *unfiltered per-frame* luminance, which
        // is exactly the quantity SVGF prescribes: the estimate must describe the noise in
        // the samples being integrated, not the noise left in the integrated result.
        float momentAlpha = max(1.0f / (prevAccumFrames + 1.0f), invMaxAccumulatedFrames);
        float2 moment = lerp(prevMoments, curMoment, momentAlpha);
        float variance = moment.y - (moment.x * moment.x);
        variance = max(variance, 0.f);
        FilteredOutput[DTid.xy] = float4(blendedColor, variance);
        MomentsOutput[DTid.xy] = float4(moment, prevAccumFrames + 1.0f, 0.f);
        return;
    }
    // (BUG-1) No usable history: seed the chain with this frame's raw moments and a count
    // of 1. One sample carries no variance information, so `curMoment.y - curMoment.x^2`
    // is now *exactly* zero -- both operands are the identical `luminance * luminance`
    // expression -- where the old scaled pair made it 0.25 * luminance^2, a value that was
    // never anything but the brightness bias described above. The `abs()` that used to
    // wrap it existed only to hide the sign of that garbage; write the zero plainly.
    //
    // Zero here is correct and not a gap: this pixel writes accumFrames = 1, and
    // ssrt_variance.hlsl (which runs immediately after, reading texMoments and texTemporal)
    // refines every lane with `history <= 2` using a 7x7 luminance-moment neighbourhood and
    // *overwrites* the .w channel with 2 * spatial sigma^2 before the first a-trous
    // iteration ever sees it. So the first two frames of a disocclusion are covered by the
    // spatial estimator and frame 3 onwards by the temporal one -- and with the 0.5 gone
    // that spatial estimator is finally on the same scale as the temporal one, which is
    // what makes the handover at history = 3 continuous instead of a 34x step.
    MomentsOutput[DTid.xy] = float4(curMoment, 1.0f, 0.f);
    FilteredOutput[DTid.xy] = float4(ssrColor.rgb, 0.0f);
}