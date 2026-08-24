// (batch 6) Shared constants and geometric predicates for the ambient-reinjection
// confidence filter: a quarter-resolution downsample, a wide separable joint-bilateral
// blur and a joint-bilateral upsample. Three dispatches replacing the depth-aware 7x7
// window that used to be folded into ssrt_diffuse_composite.hlsl.
//
// WHY THE SHAPE CHANGED. Confidence is the cosine-weighted fraction of the hemisphere in
// which the rays found usable screen-space geometry, estimated from DIFFUSE_SPP directions
// per pixel -- two by default. DeferredCompositeCS consumes it as
// `ambientKeep = 1 - conf * strength`, and the composite runs *after* SVGF/REBLUR, so
// whatever noise survives this filter is a multiplicative noise source structurally
// downstream of the entire denoiser. No denoiser setting can reach it. The only lever is
// the estimator itself, and the only free parameter left is how many ray samples it
// averages:
//
//   old:  49 full-resolution taps x 2 rays                       =   98 samples
//   new: 159 quarter-resolution taps x (4 pixels x 2 rays)       = 1274 samples
//
// i.e. 3.6x less noise (noise falls as 1/sqrt(N)) for *less* work, because those 159
// effective taps are spent on a surface with a quarter of the texels: two separable 15-tap
// passes at quarter resolution cost about 30/4 = 7.5 full-resolution taps against the 49 the
// old window took. Measured on fxc /Ges /O3, the whole new chain is 306 instruction slots per
// full-resolution pixel (36 + 105 at full resolution, 143 + 258 + 258 at quarter) against the
// 587 the old composite spent, so it is a little over half the ALU as well.
//
// The 159 is the effective count of the separable Gaussian pair, (sum w)^2 / sum w^2 squared,
// not the raw 225 a 15x15 box would give; see SSRT_CONF_LO_SIGMA for why the taper is worth
// the taps it gives up.
//
// WHY NOT TEMPORALLY. Deliberately, and permanently. Confidence is a geometric quantity:
// the instant anything in the scene moves its correct value changes discontinuously, so any
// accumulator lags it by construction, and because the signal then multiplies the ambient
// term the lag reads as a brightness trail behind every moving object. That was measured and
// is why Settings::TemporalAmbientConfidence defaults off. Nothing in this chain reads a
// previous frame, a history surface or a motion vector; it is same-frame spatial filtering
// end to end, so it has exactly zero lag.
#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

// Half width of the separable blur, in quarter-resolution texels: 15 taps per pass.
// 7 low-resolution texels reach 14 full-resolution ones, which is what buys the sample
// count above; the geometric predicates below are what stop that reach crossing a surface.
#define SSRT_CONF_LO_RADIUS 7

// Spatial falloff of the separable kernel, in quarter-resolution texels.
//
// A box would maximise the raw tap count but its frequency response is poor exactly where
// this filter is judged -- a truncated box (which is what every tap rejection produces)
// leaves a step in the filtered field, and a step in `ambientKeep` is a visible seam in the
// ambient. A Gaussian truncated at 1.75 sigma tapers to 0.22 at the last tap, so a rejected
// edge tap costs a fifth of the weight a box would have staked on it.
//
// Effective sample count of the discrete kernel is (sum w)^2 / sum w^2 = 9.4233^2 / 7.0351 =
// 12.62 taps per pass, hence 12.62^2 = 159 low-resolution taps for the separable pair against
// the 225 a box of the same reach would nominally give. exp() of a literal is constant-folded
// by fxc inside the [unroll] loops, so the weights cost nothing at runtime.
#define SSRT_CONF_LO_SIGMA 4.0f

// Normal agreement required between two taps, as a cosine: 45 degrees.
//
// Deliberately lenient, because the depth predicates below are the primary edge stop and
// this one is only here to catch the case depth cannot see -- two surfaces at the same
// distance facing different ways, i.e. the inside of a corner or a thin fin. The guide is
// the NORMALROUGHNESS G-buffer, which carries normal-map detail as well as geometry, so a
// tight threshold would reject taps across the surface of a single normal-mapped rock or
// leaf and throw away most of the sample count on exactly the foliage the filter exists for.
#define SSRT_CONF_LO_NORMAL_COS 0.707f

// Relative linear-depth agreement inside the 2x2 downsample block.
//
// Same derivation as SSRT_DEPTH_WEIGHT_SCALE in ssrt_common.hlsli: the relative depth change
// per texel is pixelAngularSize * |slope|, distance independent, ~1e-3 face-on and ~1e-2 on
// an extremely grazing surface. The taps here are at most one full-resolution texel from the
// reference, so 0.02 clears the grazing case by 2x and sits more than an order of magnitude
// under a real depth step.
#define SSRT_CONF_DS_DEPTH_TOL 0.02f

// Relative agreement between a blur tap's 1/z and the value the centre's own plane predicts
// for it. See SSRTConfPlaneGradient: on a planar surface 1/z is an exactly affine function of
// screen position, so a two-tap gradient turns the flat depth test into a slope-corrected one
// and stops the kernel truncating itself on grazing ground -- which is the single largest
// area the reinjection noise was reported on. 0.02 is the same figure as the downsample's,
// and it is now a bound on *curvature plus fp error* rather than on slope, so it does not
// have to grow with tap distance.
#define SSRT_CONF_LO_PLANE_TOL 0.02f

// Relative depth agreement required of the two immediate neighbours before their central
// difference is trusted as the plane gradient. Looser than SSRT_CONF_LO_PLANE_TOL because
// these two taps are two full-resolution texels from the centre and this test has to survive
// the grazing case it exists to serve: 2 x 1e-2 = 0.02 at the worst legitimate slope, so 0.06
// clears it 3x while staying 5x under a real depth step. A failure zeroes the gradient, which
// degrades the kernel to the flat test -- narrower, never wider.
#define SSRT_CONF_LO_GRAD_TOL 0.06f

// Relative depth agreement between a full-resolution pixel and a quarter-resolution
// neighbour in the upsample. Those neighbours sit up to three full-resolution texels away,
// which is exactly the reach of the 7x7 window this chain replaces, so this is that window's
// own tolerance unchanged.
#define SSRT_CONF_US_DEPTH_TOL 0.1f

// Guard for the reciprocals and the relative comparisons. Linear view depth is positive by
// construction; this only keeps a cleared or far-plane texel from producing an infinity.
#define SSRT_CONF_MIN_DEPTH 1e-5f

/// @brief Both geometric predicates against a flat relative depth tolerance.
///
/// Hard tests rather than falloffs, for the reason the 7x7 window recorded: the estimator
/// stays unbiased over whichever taps survive, a truncated window at a silhouette costs only
/// a little residual noise in a narrow band, and bleeding one surface's coverage onto
/// another paints a halo of wrong ambient around every character. Exponential falloffs on
/// 200 taps would also dominate the cost of the whole chain.
bool SSRTConfAccept(float a_centreDepth, float3 a_centreNormal, float a_tapDepth, float3 a_tapNormal, float a_tolerance)
{
	const float relative = abs(a_tapDepth - a_centreDepth) / max(a_centreDepth, SSRT_CONF_MIN_DEPTH);
	return relative < a_tolerance && dot(a_tapNormal, a_centreNormal) >= SSRT_CONF_LO_NORMAL_COS;
}

/// @brief Central difference of 1/z along the blur axis, or zero if it cannot be trusted.
///
/// For a planar surface 1/z is an affine function of screen position -- exactly, not
/// approximately -- and a pixel index is an affine function of screen position, so a linear
/// prediction in 1/z tracks any plane at any slope with no error at all. That is what lets
/// the 15-tap kernel keep its full width on grazing ground, where a flat relative-depth test
/// would have to reject everything past about five taps.
///
/// Gated on both neighbours agreeing in depth and normal: at a silhouette one of them belongs
/// to the other surface, and an unguarded difference would then predict a slope steep enough
/// to accept that surface's taps outright -- the exact bleed the predicates exist to prevent.
/// A rejected gradient is zero, which reduces the prediction to the flat test.
/// The two neighbours are the taps at -1 and +1 *along the blur axis of this frame's image*.
/// Nothing in this file has a temporal dimension; "minus" and "plus" are spatial offsets.
float SSRTConfPlaneGradient(
	float a_centreDepth, float3 a_centreNormal,
	float a_minusDepth, float3 a_minusNormal,
	float a_plusDepth, float3 a_plusNormal)
{
	const bool usable =
		SSRTConfAccept(a_centreDepth, a_centreNormal, a_minusDepth, a_minusNormal, SSRT_CONF_LO_GRAD_TOL) &&
		SSRTConfAccept(a_centreDepth, a_centreNormal, a_plusDepth, a_plusNormal, SSRT_CONF_LO_GRAD_TOL);

	const float gradient = 0.5f * (rcp(max(a_plusDepth, SSRT_CONF_MIN_DEPTH)) - rcp(max(a_minusDepth, SSRT_CONF_MIN_DEPTH)));

	// Finiteness by bit test rather than isfinite(): fxc may assume its inputs finite without
	// /Gis, so the guard has to look at the bits. Both operands are reciprocals of a clamped
	// positive depth, so this is insurance against a non-finite value arriving in the depth
	// buffer rather than against the arithmetic here.
	return (usable && isFiniteSafe(gradient)) ? gradient : 0.0f;
}

/// @brief Normalises a summed normal, falling back to the reference on cancellation.
///
/// The taps folded in are all within SSRT_CONF_LO_NORMAL_COS of the reference, so the sum
/// cannot actually cancel; this exists so that a non-finite G-buffer texel cannot publish a
/// NaN normal into the low-resolution guide, where every later pass would read it and reject
/// every tap around it.
float3 SSRTConfNormaliseGuide(float3 a_sum, float3 a_reference)
{
	const float lengthSquared = dot(a_sum, a_sum);
	return (isFiniteSafe(lengthSquared) && lengthSquared > 1e-8f) ? a_sum * rsqrt(lengthSquared) : a_reference;
}
