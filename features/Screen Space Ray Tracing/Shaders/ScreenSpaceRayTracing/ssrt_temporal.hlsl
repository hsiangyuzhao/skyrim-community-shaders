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

// Mirrors ScreenSpaceRayTracing::DenoiserCB. All three float4 rows are declared here now:
// fireflyClampSigma sits in the slot the A-layer left as padding (spec S1), and defect D1's
// historyClampSigma took the first of the three pad slots row 2 still had spare after spec
// S3 claimed its .x. specularRoughnessCutoff itself is read only by the SSRT_SPECULAR
// permutation of ssrt_spatial.hlsl and is declared here purely to keep the offsets aligned.
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
    // (diagnostic D3) Non-zero bypasses the defect D3 depth-disocclusion test in
    // IsValidHistory entirely, restoring the pre-D3 predicate (bounds + normal agreement +
    // the G4 finiteness rejection, which is *not* part of the bypass). Group-uniform, so the
    // branch it guards costs nothing. Exists so the depth test can be isolated in-game
    // against the D1 history clamp -- which HistoryClampSigma 0 already switches off -- with
    // one variable moving at a time.
    uint disableHistoryDepthTest;
    float denoiserPad2;
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
// It is also cheaper per tap than the form it replaces. Write v = (ndc.x, ndc.y, rawDepth, 1)
// for the tap and Pinv for the projection inverse; the tap's previous-view-space position is
// P = (Pinv * v).xyz / (Pinv * v).w, so
//     dot(N, P) - dot(N, C) = dot(A - dot(N, C) * W, v) / dot(W, v),
// with A = N.x * Pinv[0] + N.y * Pinv[1] + N.z * Pinv[2] and W = Pinv[3] -- both foldable once
// per lane. Multiplying the comparison through by |dot(W, v)| removes the division as well, so
// a tap costs two dot4s and a compare, against the two linearising divides the depth form
// needed. It also cannot manufacture a non-finite value from finite inputs, which is what
// lets the degenerate case (a tap on the previous camera plane, w -> 0) reject by arithmetic
// rather than by a guard: the right-hand side goes to zero with it.
//
// The NDC pair is folded into those two rows as well, so a tap never builds one. A tap's NDC
// is affine in its integer index -- ndc = s * pixel + o with s = float2(2, -2) / prevRenderSize
// and o = 0.5 * s + float2(-1, 1) for texel centres at index + 0.5 -- so for any row R,
//     dot(R, v) = dot(float4(R.x * s.x, R.y * s.y, R.z, R.x * o.x + R.y * o.y + R.w),
//                     float4(pixel.x, pixel.y, rawDepth, 1)).
// Both stored rows are already in that folded form, which is why they are documented against
// the *pixel index* rather than against NDC.
//
// One approximation is worth naming. Only the *forward* previous view-projection is published
// (CameraPreviousViewProjUnjittered), so the previous frame's view space is reached by
// composing it with the *current* projection inverse. That composition is exactly the previous
// view transform whenever the two frames share a projection matrix, which is every frame
// except those where the FOV is animating (a bow zoom, a killcam). During such a frame both
// sides of the subtraction are warped by the same map, so a small difference vector survives
// up to a mild local scaling -- the tolerance absorbs it, and the alternative would be
// inverting a 4x4 per lane.

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

struct SSRTHistoryPlane
{
    // dot(offsetRow, q) = (signed plane distance of the tap) * dot(wRow, q), for
    // q = float4(tap pixel index xy, tap raw depth, 1). See the derivation above, including
    // why the index rather than NDC.
    float4 offsetRow;
    // The w row of the unprojection, in the same folded form: the homogeneous divisor the
    // comparison is multiplied through by instead of dividing by.
    float4 wRow;
    // The current normal expressed in the *previous* frame's view space, which is the space
    // HistoryNormalsTexture is stored in and therefore the space the 30 degree agreement test
    // has to be taken in. Falls back to the un-rotated current normal if the rotation cannot
    // be built at all.
    float3 normalPrev;
    // Absolute plane-distance budget per texel of tangential slop; each call site scales it by
    // (its own worst tap offset + SSRT_HISTORY_PLANE_MV_TEXELS).
    float tolerancePerTexel;
    bool usable;
    bool normalUsable;
};

SSRTHistoryPlane SSRTBuildHistoryPlane(float2 uv, float rawDepth, float3 normalVS, float2 prevRenderSize, uint eyeIndex)
{
    SSRTHistoryPlane plane;
    plane.offsetRow = 0.0f;
    plane.wRow = 0.0f;
    plane.normalPrev = normalVS;
    plane.tolerancePerTexel = 0.0f;
    plane.usable = false;
    plane.normalUsable = false;

    const float4x4 projInv = FrameBuffer::CameraProjUnjitteredInverse[eyeIndex];

    // --- the current surface point, in current view space and then in current world space ---
    const float2 thisScreen = (uv - 0.5f) * float2(2.0f, -2.0f);
    float4 thisView = mul(projInv, float4(thisScreen, rawDepth, 1.0f));
    thisView.xyz = thisView.xyz / thisView.w;
    float4 thisWorld = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(thisView.xyz, 1.0f));
    thisWorld.xyz = thisWorld.xyz / thisWorld.w;

    // --- the current normal, in previous view space ---
    // CameraViewInverse is rigid, so its linear part transforms a direction directly. The trip
    // out through the previous view-projection and back in through the projection inverse
    // leaves exactly the inter-frame camera rotation (the two projections cancel; see the note
    // on the FOV-animating case above), and normalize() absorbs any residual scale. A direction
    // carries w = 0, so no division is involved and there is nothing to guard.
    //
    // This is also what repairs the *other* half of the acceptance test. The 30 degree
    // agreement compares against HistoryNormalsTexture, which holds the previous frame's
    // view-space normals, so feeding it an un-rotated current normal biased the dot product by
    // the whole inter-frame camera rotation -- over-rejecting during exactly the fast turns
    // where a rebuilt accumulation is most expensive.
    const float3 normalWS = mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0.0f)).xyz;
    const float3 normalPrevRaw = mul(projInv, mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(normalWS, 0.0f))).xyz;
    const float normalPrevLenSq = dot(normalPrevRaw, normalPrevRaw);
    if (isFiniteSafe(normalPrevLenSq) && normalPrevLenSq > 1e-12f)
    {
        plane.normalPrev = normalPrevRaw * rsqrt(normalPrevLenSq);
        plane.normalUsable = true;
    }

    // --- the same point in previous world space, then in previous view space ---
    // The game shifts its world origin as the player moves and CameraPreviousViewProjUnjittered
    // is defined against the *previous* adjust point, so the rebasing is mandatory: without it
    // every frame the game re-bases would reject the whole screen. (Same correction
    // MotionBlur::GetSSMotionVector relies on its caller having done, and that
    // ScreenSpaceGI's radianceDisocc does explicitly.)
    const float3 prevWorld = thisWorld.xyz +
                             FrameBuffer::CameraPosAdjust[eyeIndex].xyz -
                             FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xyz;
    const float4 prevClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(prevWorld, 1.0f));
    const float prevNDC = prevClip.z / prevClip.w;

    // Retained from the depth form, and for the same reason: a point outside the previous
    // frame's depth range -- behind that near plane, or past its far plane -- had no history at
    // all, so there is nothing for any tap to match and the plane would be meaningless. The bit
    // test is what makes the range test a guard: an ordered comparison against a NaN is false,
    // but fxc may assume its inputs finite without /Gis (see isFiniteSafe), and a point on the
    // previous camera plane makes prevClip.w vanish and this ratio blow up.
    if (!isFiniteSafe(prevNDC) || prevNDC <= 0.0f || prevNDC >= 1.0f)
        return plane;

    const float4 originH = mul(projInv, prevClip);
    const float3 origin = originH.xyz / originH.w;
    if (!isFiniteSafe(origin) || !plane.normalUsable)
        return plane;

    // --- fold the normal, the plane offset and the NDC mapping into two rows ---
    // dot(N, (projInv * v).xyz) = dot(N.x * projInv[0] + N.y * projInv[1] + N.z * projInv[2], v),
    // and subtracting dot(N, origin) * projInv[3] folds the plane's own offset in as well, so a
    // tap's numerator is one dot4. The pixel-index-to-NDC map is folded in on top of that; see
    // the derivation above for the two coefficient vectors.
    const float4 rowA = plane.normalPrev.x * projInv[0] +
                        plane.normalPrev.y * projInv[1] +
                        plane.normalPrev.z * projInv[2];
    const float4 rowW = projInv[3];
    const float4 rowB = rowA - dot(plane.normalPrev, origin) * rowW;

    const float2 ndcScale = float2(2.0f, -2.0f) / prevRenderSize;
    const float2 ndcBias = 0.5f * ndcScale + float2(-1.0f, 1.0f);
    plane.offsetRow = float4(rowB.x * ndcScale.x, rowB.y * ndcScale.y, rowB.z,
                             rowB.x * ndcBias.x + rowB.y * ndcBias.y + rowB.w);
    plane.wRow = float4(rowW.x * ndcScale.x, rowW.y * ndcScale.y, rowW.z,
                        rowW.x * ndcBias.x + rowW.y * ndcBias.y + rowW.w);

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
    const float texelPerDepth = 2.0f / max(abs(FrameBuffer::CameraProjUnjittered[eyeIndex][0][0]) * widthPerEye, 1e-6f);
    // abs() so the sign convention of the view axis cannot matter. thisView.xyz is non-zero for
    // any rasterised pixel, the near plane being in front of the camera.
    const float NoV = abs(dot(normalVS, normalize(thisView.xyz)));
    const float tolerance = SharedData::GetScreenDepth(rawDepth) * texelPerDepth * SSRT_HISTORY_PLANE_TILT /
                            max(NoV, SSRT_HISTORY_PLANE_MIN_NOV);

    // Bit-test rather than isfinite() for the usual reason (see isFiniteSafe). A zero or
    // negative tolerance would reject every tap forever, which is the failure being repaired,
    // so it is treated as "no usable plane" and the bypass switch remains the way out.
    if (!(isFiniteSafe(plane.offsetRow) && isFiniteSafe(plane.wRow) && isFiniteSafe(tolerance)) || tolerance <= 0.0f)
        return plane;

    plane.tolerancePerTexel = tolerance;
    plane.usable = true;
    return plane;
}

bool IsValidHistory(uint2 pixel, float2 uv, SSRTHistoryPlane plane, uint2 prevRenderSize, float tapTexels)
{
    // (audit #16) Every caller passes a pixel in the *history* textures, whose valid
    // sub-rectangle is the previous frame's dynamic-resolution extent -- hence
    // DynamicResolutionParams1.zw (previous width/height ratio) rather than .xy at the call
    // site, which is also where it is now computed: once per lane instead of 13 times.
    if (uv.x < 0 || uv.x > 1 || uv.y < 0 || uv.y > 1)
        return false;

    if (pixel.x >= prevRenderSize.x || pixel.y >= prevRenderSize.y)
        return false;

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
    if (disableHistoryDepthTest == 0)
    {
        if (!plane.usable)
            return false;

        // Both rows already carry the pixel-index-to-NDC map, so the tap only supplies its own
        // index and depth. A cleared history depth is the far plane (see ClearDenoiserHistory)
        // and so is a previous-frame sky texel; both reconstruct to a point on the far plane
        // whose plane distance is enormous, so both reject without needing a case of their own.
        const float4 q = float4(float2(pixel), HistoryDepthTexture[pixel], 1.0f);
        const float distTimesW = dot(plane.offsetRow, q);
        const float homogeneousW = dot(plane.wRow, q);
        if (!(isFiniteSafe(distTimesW) && isFiniteSafe(homogeneousW)))
            return false;

        // Both sides carry the factor |w|, which is what removes the division. A tap on the
        // previous camera plane has w = 0 and no finite position at all; the right-hand side
        // goes to zero with it, so it rejects by arithmetic.
        if (abs(distTimesW) > plane.tolerancePerTexel * tapTexels * abs(homogeneousW))
            return false;
    }

    float3 prevNormalVS;
    float roughness;
    GetNormalRoughness(HistoryNormalsTexture, pixel, prevNormalVS, roughness);
    // Both sides are now in the *previous* frame's view space; see the normal transform in
    // SSRTBuildHistoryPlane for why comparing an un-rotated current normal was biased.
    float normalDiff = dot(plane.normalPrev, prevNormalVS);
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
            if (IsValidHistory(tapPixel, prevUV, historyPlane, prevRenderSizeI, planeSlopBilinear) &&
                SSRT_LoadHistory(tapPixel, tapColor, tapMoments, tapAccumFrames) &&
                tapAccumFrames > 0.f)
            {
                const float w = bilinWeight[i];
                prevColor += tapColor * w;
                prevMoments += tapMoments * w;
                prevAccumFrames += tapAccumFrames * w;
                weightSum += w;
            }
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
            if (IsValidHistory(uint2(neighborPixel), prevUV, historyPlane, prevRenderSizeI, planeSlopSearch4) &&
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
            if (IsValidHistory(uint2(neighborPixel), prevUV, historyPlane, prevRenderSizeI, planeSlopSearch8) &&
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
    MomentsOutput[DTid.xy] = float4(curMoment, 1.0f, 0.f);
    FilteredOutput[DTid.xy] = float4(ssrColor.rgb, 0.0f);
}