#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

Texture2D<float4> HistoryTexture : register(t0);
Texture2D<float4> MotionVectorTexture : register(t1);
Texture2D<float4> SSRColorTexture : register(t3);
Texture2D<float> DepthTexture : register(t4);
Texture2D<float4> HistoryMomentsTexture : register(t5); // moments in RG, frame count in B
Texture2D<float4> HistoryNormalsTexture : register(t6);
// (defect D3) The previous frame's raw depth buffer -- a snapshot of mip 0 of the Hi-Z
// pyramid, i.e. of exactly the texture bound at t4 one frame earlier, taken once per frame
// by ScreenSpaceRayTracing::CopyHistoryGeometry alongside the normal snapshot at t6.
Texture2D<float> HistoryDepthTexture : register(t7);

RWTexture2D<float4> FilteredOutput : register(u0);
RWTexture2D<float4> MomentsOutput : register(u1);
// (diagnostic H) Per-pixel picture of what the history acceptance test decided this frame.
// Always bound -- one permutation serves both chains -- but only written when
// historyDebugView is non-zero, which the C++ side sets on the diffuse dispatch alone. See
// the encoding at SSRT_DebugAcceptColour / SSRT_DebugRejectColour.
RWTexture2D<float4> DebugHistoryOutput : register(u2);

// Mirrors ScreenSpaceRayTracing::DenoiserCB. All four float4 rows are declared here now:
// fireflyClampSigma sits in the slot the A-layer left as padding (spec S1), and defect D1's
// historyClampSigma took the first of the three pad slots row 2 still had spare after spec
// S3 claimed its .x. specularRoughnessCutoff itself is read only by the SSRT_SPECULAR
// permutation of ssrt_spatial.hlsl and is declared here purely to keep the offsets aligned.
//
// (diagnostic H) The four diagnostic flags below are all group-uniform -- they are constants
// for the whole dispatch -- so every branch they guard is a scalar branch that costs one
// comparison per group, not per lane. Row 2 had exactly one pad slot spare, which
// disableHistoryNormalTest took; the other three opened row 3. Both sides carry a sizeof
// assertion so the pair cannot drift.
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
    // --- row 2 ---
    float specularRoughnessCutoff;
    float historyClampSigma;
    // (diagnostic D3) Non-zero bypasses the defect D3 plane-distance disocclusion test in
    // IsValidHistory entirely, restoring the pre-D3 predicate (bounds + normal agreement +
    // the G4 finiteness rejection, which is *not* part of the bypass). Group-uniform, so the
    // branch it guards costs nothing. Exists so the plane test can be isolated in-game
    // against the D1 history clamp -- which HistoryClampSigma 0 already switches off -- with
    // one variable moving at a time.
    uint disableHistoryDepthTest;
    // (diagnostic H) Non-zero bypasses the 30 degree normal agreement test and nothing else,
    // i.e. the exact counterpart of disableHistoryDepthTest for the other hard gate. The two
    // together are what turn the debug view's colour reading into a confirmation: a screen
    // that comes back green should start accumulating the moment this is set, and a screen
    // that comes back red should start accumulating the moment the other one is.
    uint disableHistoryNormalTest;
    // --- row 3 ---
    // (diagnostic H) Non-zero reduces the acceptance predicate to its *upper bound*: the
    // screen-bounds tests, the guard G4 finiteness rejection and the accumFrames > 0
    // requirement, and nothing else. Both geometric gates are skipped, so whatever history
    // the reprojection lands on is taken.
    //
    // It exists because the two per-gate bypasses above can only prove a gate *is* the
    // blocker; they cannot prove that the gates are the only blockers. If the accumulation
    // still refuses to build with this set, the fault is not in the acceptance test at all --
    // it is in the bounds arithmetic, in the history contents, or in the alpha path -- and
    // that is a different repair. Strictly a diagnostic: with it set the accumulation reads
    // history straight across silhouettes and depth layers, which is maximal ghosting.
    uint forceAcceptHistory;
    // (diagnostic H, defect D3 follow-up) Selects which of the two forms of the 30 degree
    // normal comparison runs.
    //
    // 0 (the default) compares the *current* view-space normal against the stored
    // previous-frame normal, which is what this pass did for its whole shipped life. It
    // carries a known bias -- the whole inter-frame camera rotation -- but it is a bias whose
    // behaviour is measured: it over-rejects during fast turns and passes everything else.
    //
    // 1 compares the current normal *rotated into the previous frame's view space*, which is
    // the algebraically correct form and what defect D3's repair introduced.
    //
    // (defect P3) The rotation no longer composes or inverts anything: it reads the previous
    // view-space components straight out of one forward multiply by
    // CameraPreviousViewProjUnjittered, undoing only the projection's own first two rows. The
    // remaining assumption is that the two frames share a field of view, which the unit-length
    // self-check at the construction site tests directly; on failure the gate falls back to the
    // un-rotated normal and the diagnostic view paints the pixel magenta. So this can no longer
    // reject the screen silently -- but the default stays 0, because the un-rotated form is the
    // one with years of measured behaviour behind it and switching defaults is a separate change.
    uint rotatedNormalGate;
    // (diagnostic H) Non-zero makes this pass write DebugHistoryOutput. Zero leaves the
    // texture untouched, which is what the specular dispatch always passes -- the two chains
    // share one shader and one debug surface, and only the diffuse picture is wanted.
    uint historyDebugView;
    float denoiserPad3;
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

#define SSRT_NEIGHBOUR_RADIUS 1
#define SSRT_NEIGHBOUR_TILE (8 + 2 * SSRT_NEIGHBOUR_RADIUS)  // 10

// (spec S1, extended for defect D1) The 3x3 neighbourhoods of an 8x8 group overlap almost
// completely -- 64 lanes want 512 neighbour taps out of 100 distinct texels -- so the
// neighbourhood is prefetched into LDS once instead of being loaded up to 9 times each.
// 1600 bytes of LDS turns 8 extra global loads per lane into 100/64 = 1.6.
//
// Two mechanisms now share the tile, which is why it carries four floats per texel rather
// than S1's single luminance:
//   * .xyz = YCoCg of this frame's radiance, the space defect D1's history clamp builds its
//     bounding box in (see SSRTClampHistory).
//   * .w   = Rec.709 luminance, which is what the firefly clamp's statistics are defined
//     over. It is kept as its own channel deliberately: YCoCg's Y is (r + 2g + b) / 4, a
//     *different* weighting, and re-deriving luminance from YCoCg would silently change the
//     P0-hardened firefly behaviour for the sake of four bytes per texel.
//
// The vote mirrors ssrt_variance.hlsl (spec A3): a fully-sky or out-of-bounds group has
// nobody to clamp, so it must not pay for the fill. cs_5_0/fxc has no wave intrinsics,
// hence a groupshared counter rather than WaveActiveAnyTrue. All three barriers are
// executed by every lane unconditionally, which is why the bounds and far-plane
// early-outs had to move below them.
groupshared uint g_ssrtNeighbourLanes;
groupshared float4 g_ssrtNeighbourTile[SSRT_NEIGHBOUR_TILE * SSRT_NEIGHBOUR_TILE];

float3 SSRTClampFirefly(float3 radiance, uint2 gtid, float sigmas)
{
    const int2 c = int2(gtid) + SSRT_NEIGHBOUR_RADIUS;

    float sum = 0.0f;
    float sumSq = 0.0f;
    float maxNeighbour = 0.0f;

    [unroll] for (int y = -SSRT_NEIGHBOUR_RADIUS; y <= SSRT_NEIGHBOUR_RADIUS; y++)
    {
        [unroll] for (int x = -SSRT_NEIGHBOUR_RADIUS; x <= SSRT_NEIGHBOUR_RADIUS; x++)
        {
            if (x == 0 && y == 0)
                continue;  // the centre is not part of its own reference distribution

            const float l = g_ssrtNeighbourTile[(c.y + y) * SSRT_NEIGHBOUR_TILE + (c.x + x)].w;
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

// (defect D1) Neighbourhood colour clamp on the reprojected history -- the missing
// mechanism that lets a moving object drag stale radiance behind it for
// MaxAccumulatedFrames frames.
//
// Why the accumulation needs one at all. The temporal pass validates *where* it reads the
// history (motion vector, screen bounds, normal agreement) but never checks *what* it read.
// Every one of those tests passes for a texel that is geometrically plausible and
// radiometrically wrong: a character's arm sweeping across a lit wall reprojects onto wall
// texels whose normals match to well within the 30 degree gate, so the arm's indirect
// lighting is blended in at alpha = 1/17 and takes ~16 frames to decay. That is the
// accumulator the T3 experiment identified (streak length growing with
// sqrt(MaxAccumulatedFrames)), and no amount of extra spatial filtering removes it, because
// the error is in the history, not in the sample.
//
// The clamp is the standard answer -- bound the history by what *this* frame's neighbourhood
// says the radiance can be -- but the textbook formulation cannot be lifted straight in,
// because it was designed for a converged shaded image and this is a 2-spp Monte-Carlo one.
// The three design decisions, in order of how much they matter:
//
// 1. mean +- K * sigma, not min/max. The classic TAA box (clamp against the 3x3 min and
//    max) is *both* too loose and too fragile here. Too loose: at 2 spp the per-sample
//    relative standard deviation is ~0.7, so the min/max of nine heavy-tailed samples spans
//    roughly the mean +- 1.5..2 sigma, i.e. +- 1.1..1.4x the mean -- wide enough that most
//    ghosts sit comfortably inside it and are not clamped at all. Too fragile: SSRT hit
//    rates are low (audit symptom 3), so a 3x3 patch in which all 18 rays missed is common,
//    and there the min/max box collapses to the single point 0 and *replaces* the
//    accumulated dim GI with black. A mean/sigma box has a tunable width and, with the
//    floor below, cannot collapse.
//
// 2. The width is floored by the *temporally accumulated* sample sigma, not only by the
//    spatial one. sigmaSpatial is estimated from nine samples, so it has ~25% relative error
//    and, worse, is exactly 0 in the all-miss patch above. prevMoments carries the EMA of
//    the per-frame luminance moments, i.e. an estimate of the same sigma built from up to
//    MaxAccumulatedFrames frames, which is both far less noisy and non-zero wherever the
//    signal is genuinely bursty. Taking max(sigmaSpatial, sigmaTemporal) means:
//      * small-sample luck can never collapse the box (the failure mode of (1));
//      * genuine spatial detail -- a texture edge, a geometric crease -- still widens it,
//        because there sigmaSpatial exceeds the temporal noise, and widening is the
//        fail-safe direction (do not clamp across an edge).
//    It is also what *decouples K from MaxAccumulatedFrames*: sigmaTemporal is the per-frame
//    sample sigma, not the residual noise left in the accumulated output, so the box does
//    not shrink as the accumulation window lengthens. A K tuned at 16 frames stays valid
//    at 64.
//
// 3. YCoCg, not RGB. The sample cloud of a tinted GI signal is elongated along the
//    achromatic axis, so an axis-aligned box in RGB is a loose fit to it and rejects less
//    for the same K. It is also the difference between clamping that changes brightness
//    (Y) or saturation (Co/Cg) and clamping that changes *hue*, which is what a per-channel
//    RGB clamp does when it bites one channel and not the others.
//
// Why K = 1 is safe on a healthy static image. Write sigma for the per-frame sample sigma.
// The box half-width is K * sigma. The history's own residual noise for an EMA with
// alpha = 1/(N+1) is sigma * sqrt(alpha / (2 - alpha)) = 0.174 * sigma at N = 16, and the
// box centre m1 is a nine-sample mean whose standard error is sigma / 3. So the distance
// between the history and the box centre has standard deviation
// sqrt(0.174^2 + 0.333^2) = 0.376 * sigma, and at K = 1 the clamp only engages beyond
// 2.66 of those -- under 1% of frames, and when it does engage it moves the value only as
// far as the box edge. The mechanism is a no-op on converged static content by construction,
// not by luck. (defect D8) The word doing the work in that sentence is *converged*: the 0.376
// is spread(alpha) evaluated at the floor blend weight, and at a larger alpha the same K
// engages far more often. The caller therefore hands in a K already scaled to hold the
// engagement rate constant across the accumulation ramp -- see the derivation at the call
// site. Lowering K to 0.5 puts the same event at 1.33 sigma (~18% of frames), which
// starts feeding the neighbourhood's own noise back into the history -- the failure the
// naive formulations are notorious for -- which is why the default does not go there.
//
// What it does to a ghost. The box tracks the current frame, so it re-applies every frame
// with no lag: an error larger than K * sigma ~= 0.7x the local mean is cut to that bound
// on its first frame instead of decaying over 16, which is the whole of the visible,
// high-contrast streaking. An error smaller than that survives the full accumulation -- so
// the honest claim is that trails get much shorter, not that they disappear, and the
// residual is bounded at ~0.7x the SSRT term's own magnitude before albedo modulation.
//
// The centre tap is the *firefly-clamped* radiance while the eight neighbours are raw. That
// asymmetry is deliberate: it removes the one outlier we have already identified and paid
// for, at zero cost, and a firefly left in a *neighbour* only inflates sigma and widens the
// box, i.e. it makes the clamp temporarily inert rather than wrong.
float3 SSRTClampHistory(float3 history, float3 centre, uint2 gtid, float sigmas, float sigmaTemporal)
{
    const int2 c = int2(gtid) + SSRT_NEIGHBOUR_RADIUS;

    float3 m1 = Color::RGBToYCoCg(centre);
    float3 m2 = m1 * m1;

    [unroll] for (int y = -SSRT_NEIGHBOUR_RADIUS; y <= SSRT_NEIGHBOUR_RADIUS; y++)
    {
        [unroll] for (int x = -SSRT_NEIGHBOUR_RADIUS; x <= SSRT_NEIGHBOUR_RADIUS; x++)
        {
            if (x == 0 && y == 0)
                continue;  // supplied by the caller, post firefly clamp

            const float3 n = g_ssrtNeighbourTile[(c.y + y) * SSRT_NEIGHBOUR_TILE + (c.x + x)].xyz;
            m1 += n;
            m2 += n * n;
        }
    }

    // Population mean and standard deviation over all nine taps. Unlike the firefly clamp
    // the centre *is* part of the reference distribution here: this statistic is an estimate
    // of the local radiance, not an outlier test, and the centre is the single most relevant
    // sample of it.
    const float invCount = 1.0f / 9.0f;
    m1 *= invCount;
    m2 *= invCount;
    const float3 sigmaSpatial = sqrt(max(m2 - m1 * m1, 0.0f));

    // sigmaTemporal is a *luminance* sigma applied as an absolute floor to all three
    // channels. Two approximations, both benign because this is a floor on a box width and
    // not a tight bound:
    //   * It is a Rec.709 luminance sigma being compared against a YCoCg-Y one, and
    //     Y = (r + 2g + b) / 4 differs from the Rec.709 weighting. The two agree exactly on
    //     neutral grey and diverge to Y/L = 0.70 on saturated green and 1.18 on saturated
    //     red, i.e. by at most ~30% on fully saturated radiance. Making it exact would mean
    //     storing YCoCg-Y moments, which would change the quantity the a-trous luminance
    //     edge-stop is calibrated against (BUG-1) for no measurable gain here.
    //   * Chroma noise is generally the smaller of the two, so reusing the luma figure errs
    //     wide on Co/Cg -- the fail-safe direction.
    const float3 halfWidth = sigmas * max(sigmaSpatial, sigmaTemporal);
    const float3 lo = m1 - halfWidth;
    const float3 hi = m1 + halfWidth;
    const float3 h = Color::RGBToYCoCg(history);

    // (guard G4 discipline) The bounds are derived from SSRColorTexture, which the upstream
    // guards sanitise but this pass must not *assume* is clean, and a non-finite bound makes
    // clamp()'s min/max pair implementation-defined. Bit-test finiteness -- not isfinite(),
    // for the reason spelled out at isFiniteSafe -- and hand the history back untouched if
    // the box is not usable arithmetic; G4 above has already rejected a non-finite history
    // itself, so "untouched" here is always a finite value.
    if (!(isFiniteSafe(lo) && isFiniteSafe(hi) && isFiniteSafe(h)))
        return history;

    // Clamping Co/Cg independently of Y can put the triple outside the RGB cone, so the
    // round trip needs a floor: a negative radiance channel would be lerped into the output
    // and darken the pixel below anything the ray march could have produced.
    return max(Color::YCoCgToRGB(clamp(h, lo, hi)), 0.0f);
}

// (defect D3, replaced) History acceptance by *plane distance* -- the criterion NRD/ReBLUR
// use -- in place of the linear-depth comparison this pass shipped with.
//
// What the depth form was actually testing. It built the depth the current pixel's surface
// point *should* have had last frame from the camera matrices alone -- i.e. treating the
// surface as static -- and compared that against the depth a history texel really held, at a
// texel the *engine motion vector* pointed at. The two sides therefore do not describe the
// same thing on anything that moves or is oblique:
//   * the expected side is a static reprojection while the sampled side follows object
//     motion, so every animated surface -- swaying grass and trees, cloth, NPCs, i.e. most of
//     a Skyrim exterior -- is asked for the depth it would have had if it had not moved;
//   * the expected depth is the *centre's* while the taps sit up to 3.5 texels away, so a
//     same-surface tap legitimately differs by (per-texel relative depth gradient) x (tap
//     offset), and that gradient is pixelAngularSize * tan(incidence) -- unbounded. A ground
//     plane seen from eye height, a road, a wall or fence edge-on all exceed any fixed budget,
//     which is why the previous repair had to clamp its *measured* slope at 0.5 and still
//     rejected everything there.
// With both terms live the 16 candidates fail together every frame, the pixel takes the
// accumFrames = 1 restart, and the accumulation degenerates to alpha = 1: no denoising and,
// because nothing accumulates, no ghosting either. That is exactly what the in-game A/B
// against this test's own bypass switch measured -- texTemporal starts converging the moment
// the test is switched off -- and it is why every other repair in this series
// (MaxAccumulatedFrames, the D1 history clamp, the moment-format fix) read as zero-gain: all
// of them live inside `if (valid)`.
//
// The replacement asks one question instead. Take the depth the history texel actually held,
// rebuild the position it stood for using the *previous* frame's camera, and test:
//
//     |dot(N_now, P_then - C_now)| <= tolerance
//
// "is the surface point that occupied that texel last frame lying in the plane I am shading
// now?" Three properties follow, in order of how much they matter here:
//   * A same-surface tap contributes *zero*, however oblique the surface is and however far
//     the tap sits from the reprojected position, because both points are in the plane. The
//     entire slope-times-offset budget the depth form needed disappears -- and with it the
//     four extra depth loads per lane its measurement cost.
//   * Motion *within* the surface's own plane is free: grass and foliage sliding, a limb
//     sweeping across a locally flat patch, cloth rippling laterally, and every
//     camera-only reprojection residual. This is the bulk of the animation in an exterior and
//     it is what the depth form was rejecting wholesale.
//   * Motion or geometry that changes the front-to-back relationship *does* leave the plane
//     and is rejected: an occluder in front of a wall, the two treads of a step, an NPC
//     walking towards the camera. That is the only thing this test should reject, and it is
//     precisely the ghost source the depth test was added for.
//
// ---------------------------------------------------------------------------------------------
// (P2.4 follow-up) HOW THE PLANE IS BUILT, AND WHY IT NO LONGER COMPOSES TWO MATRICES
//
// The first implementation of this test reached the previous frame's view space by composing the
// published forward previous view-projection with the *current* projection inverse
// (projInv * CameraPreviousViewProjUnjittered), reconstructed every tap there, and compared
// against a plane expressed in that space. The algebra is correct on paper and it was measured
// dead in game: the diagnostic view came back uniformly blue on every pixel, and forcing history
// acceptance came back uniformly white -- which pins the failure on the plane *construction*
// (every other gate passed and the whole denoising chain downstream was healthy). A composition
// of two engine matrices depends on assumptions about their contents -- storage transpose,
// multiplication order, and the two frames sharing a projection -- that no amount of offline
// derivation can settle, so the composition is gone rather than debugged.
//
// The replacement uses only operations that some *other* shader in this build already depends on
// in game, and only in the forward direction:
//   * uv + raw depth -> camera-relative world, via CameraViewProjInverse. Exactly what
//     DeferredCompositeCS builds `positionWS` with for every pixel of every frame, and what
//     ssrt_raymarch addresses every SHARC hit with (`ScreenSpaceToWorldSpace`).
//   * world rebase, + CameraPosAdjust - CameraPreviousPosAdjust. The engine moves its world
//     origin as the player walks and the previous view-projection is defined against the
//     previous origin. Exactly the correction ScreenSpaceGI's radianceDisocc applies (its own
//     disocclusion test works in game) and that DynamicCubemaps' UpdateCubemapCS applies.
//   * world -> previous clip, via mul(CameraPreviousViewProjUnjittered, float4(world, 1)).
//     The forward direction only. This is MotionBlur::GetSSMotionVector's second line, which
//     DeferredCompositeCS uses to publish the sky's motion vectors -- consumed by TAA/DLSS every
//     frame, so a wrong result there would be visible as smeared sky rather than as silence.
//   * the current normal -> camera-relative world, via mul(CameraViewInverse, float4(n, 0)).
//     DeferredCompositeCS's `normalWS`, i.e. the direction every cubemap and skylighting lookup
//     in the build is already addressed by.
//   * SharedData::GetScreenDepth, to linearise depth. Used by this feature's own bilateral
//     weights (ssrt_common.hlsli) and by ScreenSpaceGI.
// No inverse of any *previous*-frame matrix appears, and no two matrices are multiplied
// together.
//
// ---------------------------------------------------------------------------------------------
// (defect P3) THE ARITHMETIC ERROR THAT WAS STILL IN HERE, AND THE SELF-CHECK THAT ENDS THE CLASS
//
// The P2.4-follow-up form above reached camera-relative world in two steps -- NDC -> current view
// space through CameraProjUnjitteredInverse, then view -> world through CameraViewInverse -- and
// was measured dead in game a second time: texDebugHistory came back *uniformly pale yellow*,
// i.e. SSRT_PLANE_FAIL_PREV_RANGE on every non-sky pixel, with a stationary camera.
//
// That reading is decisive on its own. With the camera at rest CameraPosAdjust equals
// CameraPreviousPosAdjust and the previous view-projection equals the current one, so the chain
// collapses to P * V * V^-1 * P^-1 applied to (ndc, rawDepth, 1) -- the identity. The recovered
// previous NDC depth is then *rawDepth itself*, which is in (0, 1) for every rasterised pixel by
// construction. A whole screen failing the (0, 1) range gate at rest cannot be a tuning problem,
// a resolution convention, a y flip or a VR sub-rect question: every one of those cancels in a
// round trip. It can only be that one of the four matrices is not the transform its name claims,
// in the multiplication convention this file uses.
//
// Three of the four are load-bearing elsewhere in the build and so cannot be:
// CameraViewProjUnjittered and CameraPreviousViewProjUnjittered publish the sky motion vectors
// TAA/DLSS consume; CameraViewInverse orients every cubemap fetch; CameraViewProjInverse
// reconstructs DeferredCompositeCS's world position. CameraProjUnjitteredInverse is the odd one
// out: `grep` finds exactly two readers in the whole tree, and *both* are this test. The third
// apparent reader -- the chain at the top of ssrt_common.hlsli's ReprojectHit, which the P2.4
// comment cited as vouching for it -- computes `prevScreen` from it and then unconditionally
// overwrites `prevScreen` with the motion-vector result four lines later. It has been dead code
// since the commit that introduced it (`ray reuse`, before the feature was even renamed), so no
// frame this build has ever drawn depended on that field's contents.
//
// Two corruptions of that field are consistent with the picture, and both produce a *negative*
// reconstructed view-space z for every pixel at every depth, which is precisely "the shaded point
// is behind the previous camera" and hence prevNDC.z > 1 everywhere:
//   * the field holding the transpose of the inverse -- what you get by inverting the matrix as
//     the engine stores it for mul(M, v) and writing the result back without transposing;
//   * the field holding the projection itself, un-inverted.
// With P = perspective(near, far), a = far/(far-near), b = -near*far/(far-near):
// mul(P^-T, (x, y, d, 1)) / w gives view z = (1/b)/(d - a/b) < 0 for every d >= 0, and
// mul(P, (x, y, d, 1)) / w gives view z = a + b/d < 0 for every d <= 1. The CPU closed-loop
// harness in the branch's scratchpad reproduces both at 100% of samples and reproduces 0% with
// the true inverse, which is what pins the mechanism to that field rather than to the algebra.
//
// The repair is therefore not an algebra change: it drops the two-step reconstruction for the
// one-step CameraViewProjInverse form DeferredCompositeCS ships, and CameraProjUnjitteredInverse
// no longer appears in this file at all. CameraViewProjInverse is the *jittered* view-projection's
// inverse while the forward leg is unjittered, and that mismatch is deliberate and harmless: TAA
// jitter is a translation of clip xy by j * w, so it touches only rows 0 and 1 of the projection
// and leaves the reconstructed view z bit-identical. The residual is a sub-texel xy offset, an
// order of magnitude inside the texel of registration slop SSRT_HISTORY_PLANE_MV_TEXELS already
// budgets, and it is the same mixed pairing DeferredCompositeCS's sky motion vectors use.
//
// And because "correct on paper, dead in game" has now happened twice on this test, the
// reconstruction no longer gets to be trusted on paper. Before the plane is built the shaded
// point is pushed *back* through CameraViewProjUnjittered and the linear depth that comes out is
// compared with the linear depth the depth buffer gave: a per-pixel, per-frame closed loop over
// the exact matrices the GPU holds, with no assumption about storage convention, FOV, dynamic
// resolution, stereo layout or where the world origin sits. A reconstruction that fails it
// reports SSRT_PLANE_FAIL_PROJECT and the pixel abstains from the plane test, so a third
// mislabelled matrix could only ever cost the test its effect -- never turn it into a screen-wide
// rejection -- and the diagnostic view would say so in amber on the first frame.
//
// The trick that makes that sufficient is to stop reconstructing taps at all and to describe the
// plane in the space the taps already live in: previous-frame pixel index plus previous-frame
// depth. A perspective projection sends planes to planes, so the current pixel's tangent plane
// has an exact description there. Concretely, with px, py the previous-frame pixel index and
// z the previous-frame *linear* view depth, screen position is proportional to x/z and y/z, so
// writing a view-space point as (z*u(px), z*v(py), z) turns the plane equation dot(N, X) = k
// into
//     N.x * u(px) + N.y * v(py) + N.z = k / z,
// whose left side is affine in (px, py). So 1/z restricted to the plane is an affine function of
// the pixel index:
//     invZ_plane(px, py) = A * px + B * py + C.
//
// (defect P3) A and B are then written down rather than fitted. u and v are affine in the pixel
// index with gradients the projection hands over directly -- du/dpx = sx / P00, dv/dpy = sy / P11,
// where sx and sy are the NDC extents of one texel -- so
//     A = N.x * sx / (P00 * k),   B = N.y * sy / (P11 * k),
// and the row is anchored at the shaded point's own previous image, where invZ is 1 / prevZ
// exactly, which removes C. N here is the current normal in *previous view space* and k is the
// plane's offset there; the shader already builds both, for the normal gate and from the same
// forward multiply, so the row costs three multiplies on top of what was already computed.
//
// The three-probe fit this replaces was correct algebra and measurably inaccurate: an arbitrary
// in-plane basis projects to two nearly parallel screen edges on an oblique surface, the 2x2 solve
// is then decided by differences of order a tenth of a texel between pixel coordinates of order
// 1e3, and fp32 has only three digits left there. Up to 4% of relative depth error on a
// same-surface tap 1.5 texels away, against a tolerance of about 1% -- i.e. the fit alone could
// reject matching history on exactly the grazing ground planes this test exists to keep. The
// closed form has no basis, no solve, no conditioning floor, and two fewer matrix multiplies.
//
// A tap then costs a dot2, an add, a reciprocal and one GetScreenDepth: predicted linear depth
// of the plane at the tap's own pixel, against the tap's own linear depth. That is the same
// quantity the folded-row form measured -- distance from the plane along the view ray -- so
// SSRT_HISTORY_PLANE_TILT and the tolerance below keep their meaning and their tuning.
//
// Failure is now attributable instead of silent. Each of the four ways the construction can give
// up sets its own code on the returned struct, the diagnostic view paints each in its own shade
// of yellow, and a pixel whose plane could not be built *bypasses* the plane test rather than
// failing it -- see the bypass at IsValidHistory. There is no longer any path by which a broken
// plane rejects the whole screen without saying so.
//
// The pixel-index-to-NDC map is the same one this pass has always used -- ndc = s * pixel + o
// with s = float2(2, -2) / prevRenderSize and o = 0.5 * s + float2(-1, 1) for texel centres at
// index + 0.5 -- applied in the forward direction here, i.e. pixel = (ndc - o) / s.
//
// (defect P3) It now goes through the eye's own half of the buffer under VR rather than treating
// the whole buffer as one frustum, because the projection matrices it is paired with are per-eye
// and the previous approximation made the two disagree by half the screen on the right eye. The
// conversion is spelled out inline rather than calling Stereo::ConvertToStereoUV, for one
// non-negotiable reason: that helper saturates uv.x, and a clamp is not affine. sx above is the
// gradient of that map, so a clamped map would publish a gradient the map does not have. Both
// directions are the identity outside VR, so the flat build's arithmetic is unchanged to the bit.
// ---------------------------------------------------------------------------------------------

// (defect D3, replaced) The plane-distance budget, as a multiple of the view-space size of one
// texel at this pixel's depth.
//
// A correct match is not exactly in the plane, for three reasons, and only the first is
// significant:
//   1. N is the G-buffer *shading* normal, not the geometric one. Normal mapping tilts it by
//      up to ~30 degrees on the surfaces Skyrim actually ships, and a tilt of theta turns a
//      purely tangential offset s into a spurious plane distance s * sin(theta) <= 0.5 * s.
//   2. The tangential offset s itself: this tap's distance from the reprojected sub-texel
//      position (1.5 / 2.5 / 3.5 texels by call site) plus about a texel of motion-vector
//      registration slop. One texel of *screen* offset is one texel of perpendicular
//      view-space extent divided by NoV, which is where the grazing term below comes from.
//   3. Depth-buffer quantisation. Non-inverted R32_FLOAT depth against a ~15 unit near plane
//      keeps the relative linear-depth error below 1e-5 out to 1e4 units, three orders of
//      magnitude under (1); it is in the noise and is not budgeted separately.
// So the modelled worst case is 0.5 * (tap offset + 1) texels of perpendicular extent, and
// this constant is that 0.5 with a 4x safety factor on top.
//
// Why a safety factor rather than a tight bound: the two failure directions are not
// symmetric. Too tight and the accumulation dies outright -- the failure this change exists to
// remove, and one that nothing downstream can compensate for. Too loose and a ghost survives
// an extra frame or two before the D1 neighbourhood colour clamp bounds it -- and that clamp
// has never run in practice, because it sits inside `if (valid)`. Accepting history is what
// switches D1 on, so the loose direction now has a second line of defence that the tight
// direction does not.
#define SSRT_HISTORY_PLANE_TILT 2.0f

// (defect D3, replaced) Motion-vector registration slop, in texels, added to every call site's
// tap offset. The motion vector is quantised and describes a texel centre rather than this
// pixel's exact sub-texel surface point, so even the nearest tap of a perfect reprojection
// sits about a texel from where the surface point really went.
#define SSRT_HISTORY_PLANE_MV_TEXELS 1.0f

// (defect D3, replaced) Floor on NoV in the grazing widening, i.e. a 10x cap on it.
//
// The widening is not a fudge factor: the view-space extent of one texel measured *along the
// surface* is its perpendicular extent divided by NoV, so term (2) above genuinely grows as
// 1/NoV, and NRD applies the identical division. The floor exists because 1/NoV is unbounded
// at a silhouette, which is also where the shading normal is least trustworthy. At 0.1 the
// widest tolerance is 10 texels of perpendicular extent -- 3.6 game units at 1000 units of
// depth and a 70 degree FOV over 1920 texels -- still well under the depth step of any
// occluder that could ghost.
#define SSRT_HISTORY_PLANE_MIN_NOV 0.1f

// (defect P3) SSRT_HISTORY_PLANE_PROBE_TEXELS and SSRT_HISTORY_PLANE_MIN_SIN used to live here.
// They were the step length and the conditioning floor for the three-probe fit of the plane row,
// and both are gone with it: the row now has a closed form (see the derivation at
// SSRTBuildHistoryPlane) so there is no basis to choose and no 2x2 solve to condition. The
// conditioning was not a hypothetical -- a nearly-parallel projected basis on an oblique surface
// was measured costing up to 4% of relative depth error on same-surface taps.

// (P2.4 follow-up) Self-check band for the inter-frame rotation that the row and the normal gate
// are both built on.
//
// A rotation preserves length, so a unit normal must come back out with unit length. The old
// code only tested the result against 1e-12, which passes for *any* garbage the composition
// might produce; that is exactly the "nonsense normal rejects every candidate on screen" failure
// the comment below warns about, left undetected. 0.25 is loose enough that no plausible
// projection-scale residual trips it and tight enough that a wrong multiplication order or a
// transposed matrix cannot slip through.
#define SSRT_HISTORY_ROTATION_TOLERANCE 0.25f

// (defect P3) Relative band for the reconstruction's closed-loop self-check: the shaded point is
// pushed back through CameraViewProjUnjittered and the linear depth that returns must agree with
// the linear depth the depth buffer gave to within this fraction of it.
//
// The exact residual is the TAA jitter's, and it is zero: jitter translates clip xy by j * w and
// so cannot move clip z or w at all, which makes the round trip exact in depth even though the
// outbound leg is the jittered inverse and the return leg is the unjittered forward. What the
// band actually absorbs is fp32 cancellation in two 4x4 products and two divides at Skyrim's
// far-to-near ratio of ~1e4, where a single-precision mantissa leaves about 1e-4 relative. 1%
// is two orders of magnitude above that and three below the smallest failure worth catching --
// the corruptions this check exists for invert the *sign* of the reconstructed depth, so they
// miss by more than 100%.
#define SSRT_HISTORY_PLANE_ROUNDTRIP_TOLERANCE 0.01f

// (P2.4 follow-up) Why the plane could not be built. Zero means it was. Painted by the
// diagnostic view as four distinguishable yellows; see SSRT_DebugPlaneFailColour.
#define SSRT_PLANE_OK 0u
// The shaded point does not land inside the previous frame's depth range, so no history texel
// anywhere stands for anything this plane could be compared with.
#define SSRT_PLANE_FAIL_PREV_RANGE 1u
// A probe's previous-clip position is non-finite, or sits on/behind the previous camera plane.
// (defect P3) Also raised when the shaded point's reconstruction fails its closed-loop
// self-check, i.e. when pushing it back through CameraViewProjUnjittered does not return the
// depth the depth buffer gave. Deliberately the same code and the same amber: both mean "this
// pixel has no position the previous frame can be interrogated with", and the diagnostic colour
// table is not being extended for a case that resolves to the identical abstention.
#define SSRT_PLANE_FAIL_PROJECT 2u
// (defect P3) The plane passes through the previous camera itself, so the reciprocal depth it
// implies is unbounded and no row describes it. Same meaning as the collinear-probes case this
// replaces -- "there is no unique answer" -- and the same dark amber, but now it is a single
// scale-free test on one number instead of a conditioning check on a 2x2 solve.
#define SSRT_PLANE_FAIL_DEGENERATE 3u
// The tolerance came out non-finite or non-positive, which would reject every tap forever.
#define SSRT_PLANE_FAIL_TOLERANCE 4u

struct SSRTHistoryPlane
{
    // (P2.4 follow-up, defect P3)
    //     invZ_plane(px, py) = dot(depthRow.xy, float2(px, py) - centrePixel) + depthRow.z,
    // the reciprocal of the previous-frame linear view depth at which this pixel's tangent plane
    // crosses previous-frame pixel (px, py). See the derivation above for why 1/z rather than z
    // is the affine one. .w is unused; the field is a float4 because a float3 costs the same
    // register and reads worse next to the two-component dot.
    float4 depthRow;
    // (defect P3) The row's origin: this pixel's own image in the previous frame, where .z above
    // is the exact reciprocal depth. Anchoring here rather than at pixel (0, 0) is not cosmetic.
    // The unanchored constant term is invZ0 - A*px0 - B*py0, and on an oblique surface those two
    // products are tens of times larger than invZ0 itself, so evaluating A*px + B*py + C rebuilds
    // a small number out of the difference of large ones. Measured on the CPU harness at 2e-4 of
    // relative depth error for a tap 1.5 texels away, with A and B algebraically exact -- five
    // times the fp32 depth quantum, for nothing. Anchored, every tap's correction is a small
    // offset from a number that is already right.
    float2 centrePixel;
    // The current normal expressed in the *previous* frame's view space. The plane row is built
    // from it (see below), and rotatedNormalGate compares against it.
    // Read by the caller, which picks between it and the un-rotated normal; see
    // rotatedNormalGate and SSRT_SelectNormalGate. That selection deliberately lives outside
    // this struct -- adding a further field for it makes fxc lose track of the struct's
    // initialisation and warn X4000 on the early returns below, even though every field is
    // assigned before any of them.
    float3 normalPrev;
    // Absolute plane-distance budget per texel of tangential slop; each call site scales it by
    // (its own worst tap offset + SSRT_HISTORY_PLANE_MV_TEXELS).
    float tolerancePerTexel;
    bool usable;
    bool normalUsable;
    // (P2.4 follow-up) SSRT_PLANE_OK, or which of the four constructions above gave up. Carried
    // for the diagnostic view only: nothing in the acceptance path reads it.
    uint failCode;
};

SSRTHistoryPlane SSRTBuildHistoryPlane(float2 uv, float rawDepth, float3 normalVS, float2 prevRenderSize, uint eyeIndex)
{
    SSRTHistoryPlane plane;
    plane.depthRow = 0.0f;
    plane.centrePixel = 0.0f;
    // Seeded with the un-rotated normal, so a rotation that cannot be built leaves the gate
    // selection on the shipped pre-D3 value rather than on an undefined one.
    plane.normalPrev = normalVS;
    plane.tolerancePerTexel = 0.0f;
    plane.usable = false;
    plane.normalUsable = false;
    plane.failCode = SSRT_PLANE_FAIL_PROJECT;

    const float4x4 projUnj = FrameBuffer::CameraProjUnjittered[eyeIndex];
    const float linearCenter = SharedData::GetScreenDepth(rawDepth);

    // --- the current surface point, in camera-relative world space ---
    // (defect P3) One matrix, the one DeferredCompositeCS reconstructs positionWS with. The eye's
    // own uv first, so the NDC matches the per-eye projection under VR; the identity outside it.
    const float2 eyeUV = Stereo::ConvertFromStereoUV(uv, eyeIndex);
    const float2 thisNDC = (eyeUV - 0.5f) * float2(2.0f, -2.0f);
    float4 posRW4 = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], float4(thisNDC, rawDepth, 1.0f));
    if (!isFiniteSafe(posRW4) || abs(posRW4.w) < 1e-9f)
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float3 posRW = posRW4.xyz / posRW4.w;
    const float distSq = dot(posRW, posRW);
    if (!isFiniteSafe(posRW) || !(distSq > 1e-12f))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }

    // --- the closed loop: does that point reproduce the depth it was built from? ---
    // (defect P3) The whole reason this test has been dead twice. See the block comment: the
    // reconstruction is pushed back through the *forward* unjittered view-projection -- a field
    // whose correctness TAA and DLSS depend on every frame -- and the linear depth that returns
    // is compared with the linear depth the depth buffer gave. Any mislabelled matrix, any
    // storage-convention surprise and any resolution or stereo convention that breaks the pairing
    // shows up here, on the first frame, in amber, instead of silently rejecting the screen.
    const float4 checkClip = mul(FrameBuffer::CameraViewProjUnjittered[eyeIndex], float4(posRW, 1.0f));
    if (!isFiniteSafe(checkClip) || abs(checkClip.w) < 1e-9f)
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float checkLinear = SharedData::GetScreenDepth(checkClip.z / checkClip.w);
    if (!isFiniteSafe(checkLinear) ||
        abs(checkLinear - linearCenter) > SSRT_HISTORY_PLANE_ROUNDTRIP_TOLERANCE * abs(linearCenter))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }

    // --- the current normal, in camera-relative world space ---
    // CameraViewInverse is rigid, so its linear part transforms a direction directly. This is
    // DeferredCompositeCS's normalWS, i.e. the direction every cubemap fetch in the build already
    // depends on. It is the one step between the reconstructed point and the previous view space
    // the plane row and the normal gate are both expressed in.
    const float3 normalRWRaw = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0.0f)).xyz;
    const float normalRWLenSq = dot(normalRWRaw, normalRWRaw);
    if (!isFiniteSafe(normalRWLenSq) || !(normalRWLenSq > 1e-12f))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float3 normalRW = normalRWRaw * rsqrt(normalRWLenSq);

    // --- the current normal, in previous view space ---
    // Two things need it: the plane row below, in closed form, and the 30 degree normal agreement
    // when rotatedNormalGate is on.
    //
    // (diagnostic H) For the gate it repairs the other half of the acceptance test. The agreement
    // compares against HistoryNormalsTexture, which holds the previous frame's view-space normals,
    // so feeding it an un-rotated current normal biases the dot product by the whole inter-frame
    // camera rotation -- over-rejecting during exactly the fast turns where a rebuilt accumulation
    // is most expensive.
    //
    // (defect P3) No matrix composition and no inverse any more, which is what killed the previous
    // two attempts. A *direction* has w = 0, so pushing it through the previous view-projection
    // gives clip = P * (V_prev * n) with the projection's translation column contributing nothing:
    // clip.w is the previous view-space z outright, and clip.xy back out the other two components
    // through the projection's own first two rows,
    //     clip.x = P00 * vx + P02 * vz,   clip.y = P11 * vy + P12 * vz.
    // Those four entries are read straight off CameraProjUnjittered, the field FidelityFX is
    // handed as cameraViewToClip, so the extraction is exact for any perspective projection --
    // off-centre VR frusta included -- and needs only that the two frames share a field of view.
    //
    // The self-check is kept and is the one property a rotation cannot fake: it preserves length.
    // A unit normal in must come back out with unit length, within
    // SSRT_HISTORY_ROTATION_TOLERANCE, which a changed FOV or any convention surprise breaks. It is
    // load-bearing for the plane now rather than for the gate alone, so failing it abstains from the
    // plane test (amber) rather than only falling back on the gate -- either way, no rejection.
    if (!(abs(projUnj[0][0]) > 1e-9f) || !(abs(projUnj[1][1]) > 1e-9f))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float rcpP00 = 1.0f / projUnj[0][0];
    const float rcpP11 = 1.0f / projUnj[1][1];
    const float4 normalPrevClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(normalRW, 0.0f));
    if (!isFiniteSafe(normalPrevClip))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float3 normalPrevRaw = float3((normalPrevClip.x - projUnj[0][2] * normalPrevClip.w) * rcpP00,
                                        (normalPrevClip.y - projUnj[1][2] * normalPrevClip.w) * rcpP11,
                                        normalPrevClip.w);
    const float normalPrevLenSq = dot(normalPrevRaw, normalPrevRaw);
    const float rotLo = (1.0f - SSRT_HISTORY_ROTATION_TOLERANCE) * (1.0f - SSRT_HISTORY_ROTATION_TOLERANCE);
    const float rotHi = (1.0f + SSRT_HISTORY_ROTATION_TOLERANCE) * (1.0f + SSRT_HISTORY_ROTATION_TOLERANCE);
    if (!isFiniteSafe(normalPrevLenSq) || !(normalPrevLenSq > rotLo) || !(normalPrevLenSq < rotHi))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float3 normalPrevVS = normalPrevRaw * rsqrt(normalPrevLenSq);
    plane.normalPrev = normalPrevVS;
    plane.normalUsable = true;

    // --- the tolerance ---
    // View-space extent of one texel per unit of depth, i.e. 2 / (P00 * renderWidth): the same
    // quantity ScreenSpaceGI builds as NDCToViewMul / OUT_FRAME_DIM. Taking it from the
    // projection matrix and the render extent is what keeps the criterion dynamic-resolution
    // proof -- at 0.667 render scale the texel is 1.5x wider and the tolerance follows -- with
    // no assumed FOV.
#if defined(VR)
    const float widthPerEye = prevRenderSize.x * 0.5f;  // each eye owns half the buffer
#else
    const float widthPerEye = prevRenderSize.x;
#endif
    const float texelPerDepth = 2.0f / max(abs(projUnj[0][0]) * widthPerEye, 1e-6f);
    // (defect P3) In camera-relative world space, where the camera sits at the origin -- the
    // engine's own reading of CameraPosAdjust, which Util::GetEyePosition returns verbatim as the
    // eye position and which every SHARC grid lookup in ssrt_raymarch is anchored on. So the view
    // ray at this pixel is just posRW, and NoV needs no matrix of its own. abs() so the sign
    // convention of the axis cannot matter; distSq > 0 was established above, so the normalize is
    // safe. NoV only widens a tolerance and is floored by SSRT_HISTORY_PLANE_MIN_NOV, so this is
    // the one quantity here whose error budget is a tuning question rather than a correctness one.
    const float NoV = abs(dot(normalRW, posRW * rsqrt(distSq)));
    const float tolerance = linearCenter * texelPerDepth * SSRT_HISTORY_PLANE_TILT /
                            max(NoV, SSRT_HISTORY_PLANE_MIN_NOV);

    // Bit-test rather than isfinite() for the usual reason (see isFiniteSafe). A zero or
    // negative tolerance would reject every tap forever, which is the failure being repaired,
    // so it is reported as its own construction failure and the pixel bypasses the plane test.
    if (!isFiniteSafe(tolerance) || tolerance <= 0.0f)
    {
        plane.failCode = SSRT_PLANE_FAIL_TOLERANCE;
        return plane;
    }
    plane.tolerancePerTexel = tolerance;

    // --- this pixel's image in the previous frame ---
    // (defect P3) Rebased onto the previous frame's world origin, which is what
    // CameraPreviousViewProjUnjittered is defined against, then forward through it. No matrix is
    // composed and none is inverted; see the block comment for what vouches for each step.
    const float3 prevWorld = posRW +
                             FrameBuffer::CameraPosAdjust[eyeIndex].xyz -
                             FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xyz;
    const float4 prevClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(prevWorld, 1.0f));
    // A point on the previous camera plane has w = 0 and no image at all. The bit test rather than
    // isfinite() for the usual reason (see isFiniteSafe): fxc may assume its inputs finite without
    // /Gis, so the guard has to look at the bits.
    if (!isFiniteSafe(prevClip) || abs(prevClip.w) < 1e-9f)
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }
    const float3 prevNDC = prevClip.xyz / prevClip.w;
    if (!isFiniteSafe(prevNDC))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }

    // Retained from the folded-row form, unchanged in meaning and in bounds: a shaded point whose
    // image falls outside the previous frame's depth range -- behind that near plane, or past its
    // far plane -- had no history at all, so there is nothing for any tap to match. The *row* is
    // still valid well outside the frustum, which is what lets every tap be judged; only this one
    // point has to have been visible.
    if (prevNDC.z <= 0.0f || prevNDC.z >= 1.0f)
    {
        plane.failCode = SSRT_PLANE_FAIL_PREV_RANGE;
        return plane;
    }

    // Linear view depth, from the same helper the tap side linearises with, so both ends of the
    // comparison are the same quantity by construction.
    const float prevZ = SharedData::GetScreenDepth(prevNDC.z);
    if (!isFiniteSafe(prevZ) || !(prevZ > 1e-6f))
    {
        plane.failCode = SSRT_PLANE_FAIL_PROJECT;
        return plane;
    }

    // NDC -> this eye's own uv, then -> whole-buffer uv, then -> whole-buffer pixel index. The
    // inverse of the pixel-index-to-NDC map this pass has always used, now with the VR half-buffer
    // step made explicit; see the block comment for why the stereo step is open-coded rather than
    // calling the saturating helper.
    float2 prevUV = prevNDC.xy * float2(0.5f, -0.5f) + 0.5f;
#if defined(VR)
    prevUV.x = (prevUV.x + (float)eyeIndex) * 0.5f;
#endif
    const float2 prevPixel = prevUV * prevRenderSize - 0.5f;

    // --- the row, in closed form ---
    // (defect P3) This replaces the three-probe fit, which was measured on the CPU harness giving
    // up to 4% of relative depth error on a grazing surface for a tap 1.5 texels away -- not from
    // floating point in the solve (a double-precision solve on the same three probes agrees to six
    // digits) but from the probes themselves: an arbitrary in-plane basis projects to two nearly
    // parallel screen edges on an oblique surface, the 2x2 solve is then determined by
    // sub-tenth-of-a-texel differences between pixel coordinates of order 1e3, and fp32 leaves
    // those three digits. Choosing a better-conditioned basis is possible but there is no need,
    // because the row has a closed form.
    //
    // With Nv the normal in previous view space and k = dot(Nv, V) the plane's offset there, the
    // derivation at the top of this file reads
    //     1 / Vz = (Nv.x * u + Nv.y * v + Nv.z) / k,     u = Vx/Vz, v = Vy/Vz,
    // and the projection gives u, v affinely from the pixel index:
    //     u = (ndc.x - P02) / P00,   ndc.x = sx * px + ...
    //     v = (ndc.y - P12) / P11,   ndc.y = sy * py + ...
    // so the two gradients are exact one-liners. sx is the NDC width of a texel -- 2 over the
    // eye's own render width, which is why widthPerEye is the right denominator under VR too --
    // and sy is negative because the pixel index runs down while NDC y runs up. The constant term
    // is not needed at all: the row is anchored at prevPixel, where the reciprocal depth is
    // 1 / prevZ exactly (see SSRTHistoryPlane::centrePixel for why anchoring matters).
    //
    // Everything on the right comes from quantities this function has already computed and
    // already self-checked. There is no third matrix, no solve, and no conditioning question.
    const float k = dot(normalPrevVS, float3(prevZ * (prevNDC.x - projUnj[0][2]) * rcpP00,
                                             prevZ * (prevNDC.y - projUnj[1][2]) * rcpP11,
                                             prevZ));
    // The plane passing through the previous camera makes 1 / Vz unbounded on it, so no row
    // describes it. Scale free: k is a length and prevZ is the length it is compared against.
    if (!isFiniteSafe(k) || !(abs(k) > 1e-4f * prevZ))
    {
        plane.failCode = SSRT_PLANE_FAIL_DEGENERATE;
        return plane;
    }
    const float rcpK = 1.0f / k;
    const float sx = 2.0f / widthPerEye;
    const float sy = -2.0f / prevRenderSize.y;
    const float4 depthRow = float4(normalPrevVS.x * sx * rcpP00 * rcpK,
                                   normalPrevVS.y * sy * rcpP11 * rcpK,
                                   1.0f / prevZ,
                                   0.0f);
    if (!isFiniteSafe(depthRow))
    {
        plane.failCode = SSRT_PLANE_FAIL_DEGENERATE;
        return plane;
    }

    plane.depthRow = depthRow;
    plane.centrePixel = prevPixel;
    plane.usable = true;
    plane.failCode = SSRT_PLANE_OK;
    return plane;
}

// (diagnostic H) The reason codes IsValidHistory returns in place of a bool. They exist so
// the debug view can say *which* gate rejected a pixel rather than only that something did,
// which is the difference between one trip into the game and one trip per hypothesis.
//
// The refactor is deliberately result-preserving: every `return false` in the predicate became
// a `return <that branch's code>` with no reordering and no change of condition, every code is
// non-zero, and every call site tests `== SSRT_HISTORY_OK` where it used to test the bool. So
// the accept/reject decision for any tap is bit-identical to the bool form, branch by branch,
// and the codes are pure extra information carried out alongside it.
#define SSRT_HISTORY_OK 0u
// Screen bounds, an unusable plane, a non-finite tap depth, a non-finite history sample, or a
// zero accumulated frame count -- i.e. "there is nothing here to read", as opposed to "what is
// here disagrees with me". The last three are raised by the call sites rather than by the
// predicate, since that is where the history load happens.
#define SSRT_HISTORY_REJ_DATA 1u
// The plane-distance disocclusion test.
#define SSRT_HISTORY_REJ_PLANE 2u
// The 30 degree normal agreement test.
#define SSRT_HISTORY_REJ_NORMAL 3u

// (diagnostic H, defect D3 follow-up) Which normal the 30 degree agreement test compares.
//
// plane.normalPrev already falls back to the un-rotated normal when the rotation could not be
// built, so the un-rotated argument is only needed for the deliberate rollback case and the
// select cannot produce anything undefined either way. Computed once per lane in main() and
// handed down, rather than stored on SSRTHistoryPlane, for the reason recorded at that
// struct's normalPrev field.
float3 SSRT_SelectNormalGate(SSRTHistoryPlane plane, float3 normalVS)
{
    return (rotatedNormalGate != 0) ? plane.normalPrev : normalVS;
}

uint IsValidHistory(uint2 pixel, float2 uv, SSRTHistoryPlane plane, float3 normalGate, uint2 prevRenderSize, float tapTexels)
{
    // (audit #16) Every caller passes a pixel in the *history* textures, whose valid
    // sub-rectangle is the previous frame's dynamic-resolution extent -- hence
    // DynamicResolutionParams1.zw (previous width/height ratio) rather than .xy at the call
    // site, which is also where it is now computed: once per lane instead of 13 times.
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1)
        return SSRT_HISTORY_REJ_DATA;

    if (pixel.x >= prevRenderSize.x || pixel.y >= prevRenderSize.y)
        return SSRT_HISTORY_REJ_DATA;

    // (defect D3, replaced) Plane-distance disocclusion; see SSRTBuildHistoryPlane for the
    // derivation and for why the depth comparison this replaces rejected everything.
    //
    // Deliberately inside IsValidHistory rather than beside it: the bilinear quad and both
    // disocclusion searches go through this one predicate, so the test applies uniformly, a
    // pixel that fails it everywhere lands on the existing accumFrames = 1 restart at the
    // bottom of main() instead of on a second parallel rejection mechanism, and neither
    // fallback search can pull history across a depth layer (the widening path guard G4
    // documents).
    // (diagnostic D3) Group-uniform bypass of the plane test and of nothing else: the bounds
    // tests above and the normal agreement plus G4 finiteness rejection below stay in force, so
    // this still reproduces the pre-D3 predicate exactly rather than accepting anything at all.
    // (diagnostic H) forceAcceptHistory drops both geometric gates at once; see its
    // declaration for why that is a separate question from either gate on its own.
    //
    // Both gates below assign to `reason` instead of returning early, and the function has a
    // single exit. That shape is mandatory, not tidiness: with an early `return` inside the
    // *second* conditional block, fxc concludes that this function can never return
    // SSRT_HISTORY_OK at all. It then proves the acceptance branch at every call site dead and
    // deletes both history loads outright -- t0 and t5 vanish from the compiled shader, the
    // accumulation silently reads nothing, and the only outward sign is a handful of X4008
    // warnings on the renormalising divides whose guards have become unreachable. The bounds
    // tests above keep their early returns because they sit before any conditional block and
    // are unaffected; verified against the disassembly, which must declare t0 through t7.
    uint reason = SSRT_HISTORY_OK;

    // (P2.4 follow-up) `plane.usable` joins the two bypass flags in the condition rather than
    // rejecting inside the block, which is the single most important behavioural change here.
    //
    // It used to raise SSRT_HISTORY_REJ_DATA, so a construction that failed for every pixel on
    // screen turned into a rejection of every pixel on screen -- silently, and reported in the
    // same blue as a genuine out-of-bounds tap. That is exactly what was measured. A plane that
    // could not be built is not evidence *against* a tap, it is the absence of evidence, so the
    // honest response is to abstain: the bounds tests above, the 30 degree normal agreement
    // below and guard G4 all stay in force, which is precisely the pre-D3 predicate. The
    // diagnostic view paints these pixels yellow (see SSRT_DebugPlaneFailColour) so an abstention
    // is loud rather than invisible -- and a full yellow screen now means "ghosting, plane test
    // off" instead of "no history anywhere".
    if (disableHistoryDepthTest == 0 && forceAcceptHistory == 0 && plane.usable)
    {
        // The row already carries the pixel-index-to-NDC map, so the tap supplies only its own
        // index and its own depth. A cleared history depth is the far plane (see
        // ClearDenoiserHistory) and so is a previous-frame sky texel; both linearise to a
        // distance enormously far from any plane through a shaded surface, so both reject without
        // needing a case of their own.
        const float tapLinear = SharedData::GetScreenDepth(HistoryDepthTexture[pixel]);
        // The plane's own linear depth at this tap's pixel, from the affine reciprocal form,
        // measured from the row's own origin (see SSRTHistoryPlane::centrePixel).
        const float invZPlane = dot(plane.depthRow.xy, float2(pixel) - plane.centrePixel) + plane.depthRow.z;
        // Data, not plane: the row was bit-tested finite when the plane was built and the tap
        // depth is the only fresh input, so a non-finite value here means the history depth texel
        // itself is corrupt.
        if (!(isFiniteSafe(tapLinear) && isFiniteSafe(invZPlane)))
            reason = SSRT_HISTORY_REJ_DATA;
        // A non-positive reciprocal depth means the plane passes behind the previous camera at
        // this pixel, so no surface lying in it could have been visible there. A plane
        // disagreement, and one that cannot produce a non-finite value on the way to saying so.
        else if (invZPlane <= 0.0f)
            reason = SSRT_HISTORY_REJ_PLANE;
        // The comparison itself, in linear view depth along the view ray -- the same quantity the
        // folded-row form measured, so SSRT_HISTORY_PLANE_TILT keeps its meaning.
        else if (abs(tapLinear - 1.0f / invZPlane) > plane.tolerancePerTexel * tapTexels)
            reason = SSRT_HISTORY_REJ_PLANE;
    }

    // (diagnostic H) The counterpart bypass. Both flags are group-uniform, so with the
    // defaults (both zero) this is a scalar branch that is always taken and the body below is
    // reached with exactly the operands it had before the flags existed.
    //
    // The leading `reason == SSRT_HISTORY_OK` is what preserves the original short-circuit: the
    // predicate only ever reached the normal comparison when the plane test had passed, so a
    // tap the plane already turned down must still not pay for the normal fetch. It also fixes
    // the attribution -- the first gate to object is the one reported, exactly as with the
    // early returns this replaces.
    if (reason == SSRT_HISTORY_OK && disableHistoryNormalTest == 0 && forceAcceptHistory == 0)
    {
        float3 prevNormalVS;
        float roughness;
        GetNormalRoughness(HistoryNormalsTexture, pixel, prevNormalVS, roughness);
        // normalGate is the un-rotated current normal by default and the previous-view-space
        // rotation of it when rotatedNormalGate is set; see SSRT_SelectNormalGate, that flag's
        // declaration, and the rotation in SSRTBuildHistoryPlane.
        float normalDiff = dot(normalGate, prevNormalVS);
        if (normalDiff < 0.866f) // cos 30
            reason = SSRT_HISTORY_REJ_NORMAL;
    }

    return reason;
}

// (diagnostic H) Floor on the accepted-history grey, so that "accumulating, one frame in" is
// visibly different from "sky" on a real monitor rather than only in a pixel probe.
//
// Without it the encoding has a hole exactly where the interesting reading is. A pixel that
// has just started a chain reports accumFrames = 1, and at MaxAccumulatedFrames 16 that is
// 1/17 = 6% grey; at 64 it is 1.5%. Both are indistinguishable from the pure black the sky and
// the far plane write, so the single most important negative result -- "history is being
// accepted but the frame count never climbs" -- would read as a black screen and be mistaken
// for the accumulation never starting at all. Lifting the ramp onto a 12% pedestal keeps it
// monotonic in the frame count and keeps full accumulation at pure white, while making any
// accepted pixel unmistakably not-black.
#define SSRT_DEBUG_ACCEPT_FLOOR 0.12f

// (diagnostic H) Grey level for an accepted pixel: brightness is how deep the accumulation
// has got. saturate() rather than a wrap because the frame counter is not bounded above by
// MaxAccumulatedFrames -- only the blend weight is -- so a long-lived static pixel would
// otherwise cycle back through black.
float3 SSRT_DebugAcceptColour(float accumFrames)
{
    const float depth = saturate(accumFrames * invMaxAccumulatedFrames);
    return (SSRT_DEBUG_ACCEPT_FLOOR + (1.0f - SSRT_DEBUG_ACCEPT_FLOOR) * depth).xxx;
}

// (diagnostic H) Pure primary for a rejected pixel, naming the gate that turned away the most
// candidates: red = plane distance, green = normal agreement, blue = bounds or data.
//
// Why the mode and not the first failure. A pixel evaluates up to sixteen taps across the
// bilinear quad and the two disocclusion searches, and the taps do not have to agree -- near a
// silhouette some fail the plane test while others are simply out of bounds. The single number
// worth reporting is therefore which gate is doing the bulk of the work, which is what makes a
// *uniform* screen colour meaningful: it says one gate is rejecting everything everywhere,
// which is the failure being hunted, as opposed to a speckle of colours along edges, which is
// the healthy picture.
//
// Ties break data > plane > normal. A bounds-or-data rejection is the one nobody predicted, so
// it wins the coin toss and gets looked at; between the two geometric gates the plane test is
// evaluated first and so is the one that can mask the other. An all-zero tally -- possible
// only if every evaluated tap passed the predicate and was then discarded for carrying no
// bilinear weight -- also lands on blue, which is correct: that is a data outcome.
float3 SSRT_DebugRejectColour(uint3 tally)
{
    if (tally.x >= tally.y && tally.x >= tally.z)
        return float3(0.0f, 0.0f, 1.0f);
    if (tally.y >= tally.z)
        return float3(1.0f, 0.0f, 0.0f);
    return float3(0.0f, 1.0f, 0.0f);
}

// (P2.4 follow-up) The colour a pixel whose plane could not be built gets, naming which of the
// four constructions gave up. Split out of blue, which is what made the first measurement
// ambiguous: "the plane was never built" and "the taps were out of bounds" arrived in the same
// colour, so a uniform blue screen had two readings and the more alarming one had to be
// established by a second trip into the game with Force Accept History on.
//
// All four are yellows so the *class* reads at a glance -- any yellow means the plane test did
// not run on that pixel and the history was judged by bounds plus normal agreement alone -- while
// the four shades stay apart on a real monitor:
//   pale yellow  the shaded point has no image inside the previous frame's depth range;
//   amber        a probe has no finite image at all (non-finite, or on the previous camera plane);
//   dark amber   the three probes are collinear in previous screen space, i.e. an edge-on surface;
//   bright lemon the tolerance came out non-finite or non-positive.
// Anything else is a coding error and comes back white-ish, which is not a colour any other
// branch of this view produces.
float3 SSRT_DebugPlaneFailColour(uint failCode)
{
    if (failCode == SSRT_PLANE_FAIL_PREV_RANGE)
        return float3(1.0f, 0.85f, 0.40f);
    if (failCode == SSRT_PLANE_FAIL_PROJECT)
        return float3(1.0f, 0.70f, 0.00f);
    if (failCode == SSRT_PLANE_FAIL_DEGENERATE)
        return float3(0.70f, 0.45f, 0.00f);
    if (failCode == SSRT_PLANE_FAIL_TOLERANCE)
        return float3(1.0f, 1.0f, 0.35f);
    return float3(0.9f, 0.9f, 0.8f);
}

// (diagnostic H) Fold one evaluated tap into the running tally, accepted or not.
//
// Branchless, and called *after* the acceptance `if` rather than from an `else` arm attached to
// it. Both of those are deliberate. An `else` arm changes the shape of the unrolled tap loops
// enough that fxc starts enumerating the all-taps-rejected path, constant-folds weightSum to
// zero on it, and then warns X4008 on the renormalising divides further down -- divides that
// are guarded by `weightSum > 0` and cannot actually see a zero. Selects and a straight-line
// call keep the loops in exactly the shape they had before the diagnostic existed, so the
// generated code for the acceptance logic is untouched and no warning appears.
//
// `accepted` folds in the two rejections the predicate cannot see: a non-finite history sample
// (guard G4) and a zero accumulated frame count, both of which are data outcomes and both of
// which arrive here as reason == OK with accepted == false.
void SSRT_DebugTally(inout uint3 tally, bool accepted, uint reason)
{
    const uint code = accepted ? SSRT_HISTORY_OK :
                                 ((reason == SSRT_HISTORY_OK) ? SSRT_HISTORY_REJ_DATA : reason);
    tally.x += (code == SSRT_HISTORY_REJ_DATA) ? 1u : 0u;
    tally.y += (code == SSRT_HISTORY_REJ_PLANE) ? 1u : 0u;
    tally.z += (code == SSRT_HISTORY_REJ_NORMAL) ? 1u : 0u;
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

    // ---- (spec S1 / defect D1) neighbourhood vote and prefetch; no lane may leave before
    // ---- they complete, because every barrier below must stay group uniform ----
    const bool clampFireflies = fireflyClampSigma > 0.0f;
    // (defect D1) The history clamp reads the same 3x3 tile, so it joins the same vote
    // rather than getting a second one. Either mechanism being on is enough to make the
    // fill worth paying for; both off leaves the tile unwritten exactly as before.
    const bool clampHistory = historyClampSigma > 0.0f;
    const bool laneClamps = (clampFireflies || clampHistory) && inBounds && !isFarPlane;

    if (all(GTid.xy == 0))
        g_ssrtNeighbourLanes = 0;
    GroupMemoryBarrierWithGroupSync();

    if (laneClamps)
        InterlockedAdd(g_ssrtNeighbourLanes, 1u);
    GroupMemoryBarrierWithGroupSync();

    // Guards LDS *writes* only; the barrier that publishes them stays unconditional.
    if (g_ssrtNeighbourLanes != 0u) {
        const int2 tileOrigin = int2(Gid.xy) * 8 - SSRT_NEIGHBOUR_RADIUS;
        for (uint ty = GTid.y; ty < SSRT_NEIGHBOUR_TILE; ty += 8) {
            for (uint tx = GTid.x; tx < SSRT_NEIGHBOUR_TILE; tx += 8) {
                // Clamp-to-edge rather than a validity flag: duplicating the border texel
                // costs nothing here, because the statistics it feeds are an outlier test
                // and a bounding box, not energy-preserving averages. A halo texel outside
                // the dynamic-resolution sub-rect would otherwise have to be excluded from
                // the count, which is 8 extra predicates per lane for a border effect.
                const int2 p = clamp(tileOrigin + int2(tx, ty), int2(0, 0), int2(screen_size) - 1);
                const float3 radiance = SSRColorTexture[p].rgb;
                g_ssrtNeighbourTile[ty * SSRT_NEIGHBOUR_TILE + tx] =
                    float4(Color::RGBToYCoCg(radiance), Color::RGBToLuminance(radiance));
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    // ---- vote and prefetch complete; early returns are safe from here on ----

    if (!inBounds)
        return;

    // (diagnostic H) Group-uniform, so every branch guarding a debug write below is scalar.
    const bool writeDebug = historyDebugView != 0;

    if (isFarPlane) {
        FilteredOutput[DTid.xy] = 0.0;
        MomentsOutput[DTid.xy] = 0.0;
        // (diagnostic H) Sky and far plane are black: there is no history question to ask
        // here, so neither a grey nor a rejection colour would mean anything.
        if (writeDebug)
            DebugHistoryOutput[DTid.xy] = float4(0.0f, 0.0f, 0.0f, 1.0f);
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
    // (guard G5) The pair is stored in MomentsOutput, whose largest finite value is 65504:
    // *anything above it is written back as +Inf, permanently*. (defect D5 moved the target
    // from R11G11B10_FLOAT to R16G16B16A16_FLOAT, which changes the mantissa but not the
    // 5-bit exponent, so the ceiling moves only from 65024 to 65504 and this guard is
    // unaffected -- see the derivation at the allocation site.) The second moment is a
    // square, so the overflow point in luminance is sqrt(65504) = 255.9 -- reachable
    // by a single bright specular sample, no NaN or corruption required. Once .y is Inf the
    // variance is Inf - x^2 = Inf, the a-trous luminance edge-stop divides by sqrt(Inf),
    // every neighbour weight becomes 0 or NaN, and the moments EMA can never recover
    // because lerp(Inf, finite, alpha) stays Inf for every alpha < 1.
    //
    // 250 leaves 4% of headroom below the overflow point (250^2 = 62500 < 65504) and is
    // ~30x above the top of the radiance the firefly clamp lets through, so it can only
    // engage on values that were already outside the representable range of their own
    // storage. Both moments use the clamped luminance so the pair stays a consistent
    // (mu1, mu2) and the variance estimate y - x^2 keeps its sign.
    const float momentLuminance = min(luminance, SSRT_MOMENT_LUMINANCE_MAX);
    float2 curMoment = float2(momentLuminance, momentLuminance * momentLuminance);

    // Reproject UVs using motion vectors
    float2 prevUV = uv;
    ReprojectHit(MotionVectorTexture, float3(uv, depthCenter), eyeIndex, prevUV);

    // (audit #16) The previous frame's dynamic-resolution extent. It is both the grid the
    // history textures are valid on and the grid every tap's NDC is derived from, so it is
    // computed once here and handed down rather than rebuilt inside each validation.
    const float2 prevRenderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.zw;
    const uint2 prevRenderSizeI = uint2(prevRenderSize);

    // (defect D3, replaced) The acceptance plane, built once and handed to every one of the up
    // to 16 tap validations below.
    const SSRTHistoryPlane historyPlane = SSRTBuildHistoryPlane(uv, depthCenter, normalVS, prevRenderSize, eyeIndex);
    // (diagnostic H) The normal the 30 degree gate compares, resolved once for all 16 taps.
    const float3 historyNormalGate = SSRT_SelectNormalGate(historyPlane, normalVS);

    // Tangential slop per call site, in texels: that site's worst tap distance from the
    // reprojected sub-texel position, plus the motion vector's own registration slop.
    //   * the 2x2 bilinear quad reaches sqrt(2) = 1.42 texels;
    //   * the 4-tap cross is centred on floor(prevCoord), so a tap is up to 1 (truncation) + 1
    //     (offset) = 2 texels away, and diagonally nothing -- the offsets are axis aligned --
    //     giving 2.0 plus the half-texel the truncation can add back, i.e. 2.5;
    //   * the 8-tap search adds one more texel of reach on both axes, i.e. 3.5.
    // These are worst cases, so a tap closer than the bound is judged slightly loosely -- the
    // fail-safe direction, for the reason given at SSRT_HISTORY_PLANE_TILT.
    const float planeSlopBilinear = 1.5f + SSRT_HISTORY_PLANE_MV_TEXELS;
    const float planeSlopSearch4 = 2.5f + SSRT_HISTORY_PLANE_MV_TEXELS;
    const float planeSlopSearch8 = 3.5f + SSRT_HISTORY_PLANE_MV_TEXELS;

    float4 prevColor = 0.f;
    float prevAccumFrames = 0.f;
    float2 prevMoments = float2(0.f, 0.f);
    // (audit #16) prevUV is normalised to the *previous* frame's render sub-rect, so it
    // must be scaled by the previous frame's DRS ratio (DynamicResolutionParams1.zw),
    // not the current one. With DLSS/DRS the two differ whenever the ratio moves, which
    // shifted the whole history lookup and silently invalidated reprojection.
    const float2 prevCoord = prevUV * prevRenderSize;
    // The texel the reprojection lands *in*. No longer the primary lookup (see D2 below),
    // but still the origin the two disocclusion searches further down are defined against.
    uint2 prevPixel = uint2(prevCoord);
    bool valid = false;
    // (diagnostic H) Rejected-tap counts, in the order (data, plane, normal). Accumulated
    // across all three resolution paths so the mode is taken over every candidate the pixel
    // actually looked at, and touched only on the rejection path -- a fully accepting bilinear
    // quad never writes it.
    uint3 debugTally = uint3(0u, 0u, 0u);

    // (defect D2) 2x2 bilinear gather with per-tap validity, replacing a point fetch of the
    // single texel containing prevCoord.
    //
    // What the point fetch cost. A motion vector lands at an arbitrary sub-texel position, so
    // rounding it to one texel throws away up to half a texel of registration in each axis
    // and, worse, does it *inconsistently between frames*: as the reprojection drifts across
    // a texel boundary the history source jumps by a whole pixel. Under camera motion that
    // makes the accumulation resample its own output along the motion direction every few
    // frames, which is a directional low-pass filter -- i.e. it manufactures exactly the
    // smear the reprojection was supposed to prevent, and it does so at a rate proportional
    // to screen-space velocity. It also throws away the *sharpening* half of correct
    // reprojection: sub-texel-accurate history is what lets an accumulated image hold detail
    // finer than the per-frame sample can resolve.
    //
    // Why per-tap validity rather than a single bilinear sample through LinearSampler. The
    // hardware filter would be one fetch instead of four, but it cannot be told to skip a
    // tap: a 2x2 quad straddling a silhouette would bleed the background's history into the
    // foreground with a weight the shader cannot see, which is the ghost source this whole
    // series is about. Each tap is therefore validated on its own (bounds, normal agreement,
    // G4 finiteness) and the surviving bilinear weights are renormalised -- the same
    // construction ScreenSpaceGI's radianceDisocc.cs.hlsl uses for its history.
    //
    // Cost: the primary path goes from 3 texture loads to 12. It buys back some of that
    // statistically -- a partially valid quad now resolves here instead of falling through
    // to the 4-tap and 8-tap searches, which are 12 and 24 loads -- but on a
    // fully-reprojectable screen it is a real +9 loads per lane.
    //
    // A tap is additionally required to carry accumFrames > 0, which the point fetch did not
    // demand. That was harmless when a single texel answered the query -- a zero count drives
    // alpha to 1, so the history was ignored anyway -- but it is not harmless in a weighted
    // sum: the far-plane branch writes colour 0 and count 0, so an unguarded sky texel inside
    // the quad would drag a fraction of black into the blend with a non-zero weight. The
    // guard also matches what both fallback searches already require, so all three paths
    // agree on what counts as history.
    {
        // Texel centres sit at integer + 0.5, so the quad's top-left index is
        // floor(coord - 0.5) and the interpolants are the remainder.
        const float2 bilinCoord = prevCoord - 0.5f;
        const float2 bilinBase = floor(bilinCoord);
        const float2 f = bilinCoord - bilinBase;
        const int2 bilinOffset[4] = { int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1) };
        const float bilinWeight[4] = {
            (1.0f - f.x) * (1.0f - f.y),
            f.x * (1.0f - f.y),
            (1.0f - f.x) * f.y,
            f.x * f.y
        };

        float weightSum = 0.f;
        [unroll(4)]
        for (int i = 0; i < 4; i++)
        {
            const uint2 tapPixel = uint2(int2(bilinBase) + bilinOffset[i]);
            // (guard G4) Per-tap rejection, identical in kind to the two searches below: a
            // non-finite tap contributes to neither the sums nor weightSum, so one poisoned
            // texel cannot be filtered into its neighbours.
            float4 tapColor;
            float2 tapMoments;
            float tapAccumFrames;
            // (diagnostic H) The predicate's verdict, kept so the rejection can be attributed.
            // The `&&` chain below is unchanged in structure and still short-circuits, so
            // SSRT_LoadHistory is issued on exactly the taps it was issued on before.
            const uint reason = IsValidHistory(tapPixel, prevUV, historyPlane, historyNormalGate, prevRenderSizeI, planeSlopBilinear);
            // (diagnostic H) The condition is character-for-character the one this loop always
            // had, and it stays an if condition rather than becoming a const bool
            // initialiser. That is not style: as an initialiser fxc stops short-circuiting the
            // chain, so 	apAccumFrames > 0.f is evaluated before SSRT_LoadHistory has written
            // it, the undefined read poisons the whole expression, and the optimiser deletes
            // both history loads outright -- t0 and t5 disappear from the shader and the
            // accumulation silently reads nothing at all.
            bool accepted = false;
            if (reason == SSRT_HISTORY_OK &&
                SSRT_LoadHistory(tapPixel, tapColor, tapMoments, tapAccumFrames) &&
                tapAccumFrames > 0.f)
            {
                accepted = true;
                const float w = bilinWeight[i];
                prevColor += tapColor * w;
                prevMoments += tapMoments * w;
                prevAccumFrames += tapAccumFrames * w;
                weightSum += w;
            }
            SSRT_DebugTally(debugTally, accepted, reason);
        }

        // An exactly-aligned reprojection gives two or three taps a weight of 0, so the
        // threshold is an epsilon rather than 0: a quad whose only survivor carries no weight
        // has told us nothing and must fall through to the searches, not divide by ~0.
        if (weightSum > 1e-5f)
        {
            const float invWeightSum = 1.0f / weightSum;
            prevColor *= invWeightSum;
            prevMoments *= invWeightSum;
            prevAccumFrames *= invWeightSum;
            valid = true;
        }
        else
        {
            // Discard whatever sub-epsilon dust accumulated; the searches below add into
            // these same accumulators and expect them zeroed.
            prevColor = 0.f;
            prevMoments = float2(0.f, 0.f);
            prevAccumFrames = 0.f;
        }
    }

    if (!valid)
    {
        int2 crossOffset[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
        float weightSum = 0.f;
        [unroll(4)]
        for (int i = 0; i < 4; i++)
        {
            int2 neighborPixel = int2(prevPixel) + crossOffset[i];
            // (guard G4) A non-finite tap is excluded from the average and from weightSum,
            // which is what stops a single poisoned texel from being spread into the
            // disoccluded pixels around it every time the camera moves.
            float4 neighborColor;
            float2 neighborMoments;
            float neighborAccumFrames;
            // (diagnostic H) Same attribution as the bilinear quad above.
            const uint reason = IsValidHistory(uint2(neighborPixel), prevUV, historyPlane, historyNormalGate, prevRenderSizeI, planeSlopSearch4);
            // (diagnostic H) Kept as an if condition for the reason recorded at the bilinear
            // quad above: as an initialiser the short-circuit is lost and both history loads
            // are optimised away.
            bool accepted = false;
            if (reason == SSRT_HISTORY_OK &&
                SSRT_LoadHistory(uint2(neighborPixel), neighborColor, neighborMoments, neighborAccumFrames) &&
                neighborAccumFrames > 0.f)
            {
                accepted = true;
                prevColor += neighborColor;
                prevAccumFrames += neighborAccumFrames;
                prevMoments += neighborMoments;
                weightSum += 1.f;
            }
            SSRT_DebugTally(debugTally, accepted, reason);
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
            // (diagnostic H) Same attribution as the two paths above.
            const uint reason = IsValidHistory(uint2(neighborPixel), prevUV, historyPlane, historyNormalGate, prevRenderSizeI, planeSlopSearch8);
            // (diagnostic H) Kept as an if condition for the reason recorded at the bilinear
            // quad above: as an initialiser the short-circuit is lost and both history loads
            // are optimised away.
            bool accepted = false;
            if (reason == SSRT_HISTORY_OK &&
                SSRT_LoadHistory(uint2(neighborPixel), neighborColor, neighborMoments, neighborAccumFrames) &&
                neighborAccumFrames > 0.f)
            {
                accepted = true;
                prevColor += neighborColor;
                prevAccumFrames += neighborAccumFrames;
                prevMoments += neighborMoments;
                weightSum += 1.f;
            }
            SSRT_DebugTally(debugTally, accepted, reason);
        }
        if (weightSum > 0.f)
        {
            prevColor /= weightSum;
            prevAccumFrames /= weightSum;
            prevMoments /= weightSum;
            valid = true;
        }
    }

    // (diagnostic H) The one and only debug write for a shaded pixel, deliberately placed here
    // -- after all three resolution paths have settled `valid` and `prevAccumFrames`, and
    // *before* the accumulation branch below.
    //
    // The position is not cosmetic. Splitting this into a write inside `if (valid)` and a
    // second one after that block's `return` costs four new fxc warnings in this file: with a
    // store sitting past that return, fxc stops treating it as an exit, merges both tails and
    // flattens the whole tap-resolution chain into predicated straight-line code. In that form
    // it enumerates the all-taps-rejected path, sees weightSum constant-folded to zero, and
    // reports X4008 on the three renormalising divides that `weightSum > 0` already makes
    // unreachable -- plus an X4000 on the plane struct, phi-merged across the same paths.
    // Writing once from here leaves both tails exactly as they were, so the acceptance and
    // blending code generates as it did before this diagnostic existed.
    //
    // prevAccumFrames is final at this point, so `prevAccumFrames + 1` is the very value the
    // valid branch is about to publish as MomentsOutput.z: the picture and the stored count
    // cannot disagree.
    if (writeDebug)
    {
        float3 debugColour = valid ? SSRT_DebugAcceptColour(prevAccumFrames + 1.0f)
                                   : SSRT_DebugRejectColour(debugTally);

        // (P2.4 follow-up) Two per-pixel facts about the *construction* outrank the per-tap
        // tally, because they say the test the tally reports on did not actually run.
        //
        // Order matters: the plane failure is checked last so it wins, since it is the one that
        // changes what the acceptance means. Both are suppressed when the corresponding gate is
        // switched off, so a diagnostic pass taken with Force Accept History or Disable History
        // Depth Test on still reads as the plain accept/reject picture it did before -- the four
        // switches keep their exact semantics and their exact pictures.
        //
        // The rotation warning is only reachable with Rotated Normal Gate on: with it off the
        // un-rotated normal is what the gate compares anyway, so a failed self-check has changed
        // nothing and there is nothing to report.
        //
        // (defect P3) With Disable History Depth Test off -- the default -- the magenta is now
        // subsumed by the amber below, because the plane row is built from the same rotation and
        // so a failed self-check also stops the plane being published. That is the honest
        // ordering: the stronger statement is "the plane test did not run", not "one gate lost
        // its preferred normal". The branch is kept rather than deleted because it is still the
        // only report available with the depth test switched off, where the rotation matters and
        // the plane does not.
        if (rotatedNormalGate != 0 && !historyPlane.normalUsable && disableHistoryNormalTest == 0 && forceAcceptHistory == 0)
            debugColour = float3(1.0f, 0.0f, 1.0f);
        if (!historyPlane.usable && disableHistoryDepthTest == 0 && forceAcceptHistory == 0)
            debugColour = SSRT_DebugPlaneFailColour(historyPlane.failCode);

        DebugHistoryOutput[DTid.xy] = float4(debugColour, 1.0f);
    }

    if (valid)
    {
        // (defect D1) Bound the resolved history by this frame's neighbourhood before it is
        // blended in. Placed here, after all three resolution paths have converged on a
        // single prevColor / prevMoments pair, so the direct reprojection and both
        // disocclusion searches are covered by one mechanism -- and the searches need it
        // most, since they average history from texels up to two pixels away by construction.
        //
        // The floor for the box width is the sigma the *accumulated* moment pair implies.
        // prevMoments is the EMA of the per-frame luminance moments, so
        // sqrt(y - x^2) is an estimate of the per-frame sample sigma built from the whole
        // accumulation window rather than from nine spatial taps; see the derivation at
        // SSRTClampHistory for why flooring with it is what keeps the clamp both
        // collapse-proof and independent of MaxAccumulatedFrames. A freshly seeded pixel
        // reports 0 here, which is correct and harmless: its alpha is near 1, so the history
        // it would clamp barely contributes to the output anyway.
        //
        // Only the colour is clamped; prevMoments is passed through to the EMA below
        // untouched. The moments describe the noise in the *samples* being integrated, which
        // a wrong history colour does not change, and SVGF requires the a-trous chain to be
        // steered by exactly that quantity (see BUG-1b). Clamping them as well would make the
        // variance channel describe the filtered result instead, which is the error BUG-1b
        // removed.
        float alpha = max(1.0f / (prevAccumFrames + 1.0f), invMaxAccumulatedFrames);

        if (clampHistory)
        {
            const float sigmaTemporal = sqrt(max(prevMoments.y - prevMoments.x * prevMoments.x, 0.0f));

            // (defect D8) HistoryClampSigma is calibrated against a *converged* history, and
            // applying that width to an unconverged one is a self-lock: it is the one way this
            // mechanism can stop the accumulation ever reaching the state its own derivation
            // assumes.
            //
            // The derivation at SSRTClampHistory fixes K = 1 by asking how far a healthy
            // history sits from the box centre. In units of the per-frame sample sigma that
            // distance has standard deviation
            //     spread(a) = sqrt(1/9 + a/(2 - a)),
            // where 1/9 is the variance of the nine-tap mean that forms the centre and
            // a/(2 - a) is the residual an EMA at blend weight a leaves in the history. The
            // quoted 0.376 is spread(1/17), i.e. the value at the *floor* weight -- a fully
            // accumulated pixel. A pixel three frames in blends at a = 1/4, where the spread
            // is 0.564: the same K then engages at 1.8 standard deviations instead of 2.66,
            // which is a ~7% per-frame hit rate rather than under 1%, and every hit drags the
            // history back onto a nine-sample mean of *this* frame's 2-spp noise. That
            // re-injects the noise the accumulation is trying to average out, so the pixel
            // stays noisy, which keeps the clamp firing. The loop is stable and the pixel never
            // converges.
            //
            // Scaling K by spread(alpha) / spread(floor alpha) holds the false-positive rate
            // constant instead of holding the width constant. alpha can never be below the
            // floor, so the ratio is >= 1 always: the clamp can only ever be *more* permissive
            // than its tuned value, never less, which is what makes this safe by construction
            // rather than by measurement. It reaches exactly 1 -- i.e. exactly the tuned
            // behaviour, with no residual widening -- as soon as the pixel hits
            // MaxAccumulatedFrames, and it self-normalises to whatever that setting is rather
            // than to a hard-coded 16.
            //
            // Magnitudes at MaxAccumulatedFrames 16: 2.80x on the first blended frame, 1.77x at
            // one accumulated frame, 1.25x at four, 1.00x at sixteen. The early widening costs
            // nothing in ghosting terms because alpha is near 1 there -- the history it declines
            // to clamp contributes almost none of the output.
            const float spreadNow = sqrt(1.0f / 9.0f + alpha / (2.0f - alpha));
            const float spreadFloor = sqrt(1.0f / 9.0f + invMaxAccumulatedFrames / (2.0f - invMaxAccumulatedFrames));
            const float sigmasEffective = historyClampSigma * (spreadNow / spreadFloor);

            prevColor.rgb = SSRTClampHistory(prevColor.rgb, ssrColor.rgb, GTid.xy, sigmasEffective, sigmaTemporal);
        }

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
    // (diagnostic H) This is the branch the whole diagnostic exists to explain -- reaching it
    // every frame on every pixel is precisely the "texTemporal never converges" symptom,
    // because it is the alpha = 1 passthrough. The colour naming the gate responsible was
    // already written above, before the branch.
    MomentsOutput[DTid.xy] = float4(curMoment, 1.0f, 0.f);
    FilteredOutput[DTid.xy] = float4(ssrColor.rgb, 0.0f);
}