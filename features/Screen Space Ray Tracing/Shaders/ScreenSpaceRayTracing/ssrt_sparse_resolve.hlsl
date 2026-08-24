#include "ScreenSpaceRayTracing/ssrt_common.hlsli"

// (batch 12) SPARSE RESOLVE -- the one pass that turns a sparsely traced frame back into the
// three full-resolution surfaces the rest of the chain already reads.
//
// WHY THIS EXISTS AT ALL, i.e. why the denoiser is not run at the sparse resolution.
//
// The reference design for half-resolution diffuse in this fork's history ran the whole SVGF
// chain on a half-sized texture set and upsampled at the end. That predates REBLUR. Under the
// denoiser that actually ships now, running the chain sparsely is not a smaller change than
// this one, it is a much larger one and a worse one:
//
//   * REBLUR's history, its guides and its own tile classification are all sized by
//     nrd::Instance::Init and nrd::CommonSettings. Halving the signal means halving the guide
//     set too -- viewZ, normal/roughness and motion vectors are produced once per frame by the
//     NRD feature and shared with every consumer -- so a sparse REBLUR would need a second,
//     half-sized guide set and a second instance, i.e. a duplicate of the whole denoising path.
//   * For checkerboard specifically, NRD *does* have native support -- and it costs more than
//     it saves on the denoiser side. Source/Reblur.cpp:117 makes hit-distance reconstruction
//     unavailable whenever checkerboardMode != OFF, and line 119 makes the PrePass
//     unskippable, which for this fork's diffuse instance (NRD.cpp pins
//     diffusePrepassBlurRadius to 0, so the PrePass is skipped today) means adding a
//     full-resolution pass that currently does not run.
//
// Resolving here instead keeps every one of those properties: the denoiser sees a fully
// populated full-resolution input, checkerboardMode stays OFF, the PrePass stays skipped, hit
// distance reconstruction stays available, and one shader serves REBLUR, SVGF and no-denoiser
// alike. The pass is also channel-agnostic -- its weights come from the depth and normal
// guides, never from the payload -- which is what lets it carry REBLUR's packed
// (YCoCg, normHitDist) layout and the chain's own linear (rgb, confidence) layout through the
// same code.
//
// THE THREE SURFACES, AND WHY THEY ARE RESOLVED TOGETHER WITH ONE SET OF WEIGHTS.
//
// The ray march writes three things per lane: radiance (packed or linear), the raw hit
// confidence that ambient reinjection reads after the denoiser, and the reciprocally encoded
// hit distance that sizes the a-trous kernel. Under sparse sampling all three are written on the
// compact grid, and all three have consumers that are full resolution and unchanged. Blending
// them with one shared weight set is not a convenience: the confidence and the kernel radius
// describe the *same* rays as the radiance, so a pixel whose radiance came predominantly from
// one tap must take that tap's confidence too, or the composite would remove an amount of
// vanilla ambient the radiance it was handed does not account for.
//
// Averaging the *encoded* hit distance rather than a decoded one is the same argument
// ssrt_raymarch.hlsl makes for its per-SPP average: the consumer is linear in the encoded value,
// so E[f(L)] is the mean kernel width the pixel's rays call for and f(E[L]) is not.
//
// WHY THIS ALSO SETTLES THE CONFIDENCE FILTER QUESTION (batch 6's chain).
//
// Because confidence is resolved to full resolution *here*, before anything else runs, the
// quarter-resolution confidence filter is completely untouched: it still downsamples a
// full-resolution R8 surface by 2x2, still blurs at quarter resolution, still upsamples. The
// alternative -- leaving confidence sparse and filtering on an eighth-resolution grid -- would
// have widened an already wide kernel onto a grid whose depth and normal guides can no longer
// separate the geometry it reaches across, and would have needed a second permutation of all
// three of its passes. The cost of resolving here instead is two R8 taps and two R8 stores
// inside a pass that has to run anyway.

Texture2D<float4> SparseColorTexture : register(t0);       // compact: radiance (packed or linear)
Texture2D<float> SparseConfidenceTexture : register(t1);   // compact: raw hit confidence
// NormalRoughnessTexture (t2) comes from ssrt_common.hlsli, and is full resolution.
Texture2D<float> SparseHitDistTexture : register(t3);      // compact: encoded hit distance
Texture2D<float> DepthTexture : register(t4);              // full resolution
#if defined(SSRT_SPARSE_HALFRES)
// Mip 1 of the Hi-Z pyramid, i.e. the 2x2 *minimum*. That makes it the exact depth of the
// subpixel each compact texel's ray was traced from -- the argmin the ray march resolved -- so
// the depth half of the guide costs nothing and approximates nothing.
Texture2D<float> HalfDepthTexture : register(t5);
#endif

RWTexture2D<float4> ResolvedColorOutput : register(u0);
RWTexture2D<float> ResolvedConfidenceOutput : register(u1);
RWTexture2D<float> ResolvedHitDistOutput : register(u2);

// Tolerated relative linear-depth change between a full-resolution pixel and one tap, expressed
// as a multiple of the per-texel figure SSRT_DEPTH_WEIGHT_SCALE derives in ssrt_common.hlsli.
// The budget scales with tap distance exactly as the a-trous kernel's phiDepth does: a
// half-resolution tap sits up to one compact texel -- two full-resolution texels -- away, while
// every checkerboard tap is exactly one full-resolution texel away.
#if defined(SSRT_SPARSE_HALFRES)
#   define SSRT_SPARSE_DEPTH_SCALE (2.0f * SSRT_DEPTH_WEIGHT_SCALE)
#else
#   define SSRT_SPARSE_DEPTH_SCALE (1.0f * SSRT_DEPTH_WEIGHT_SCALE)
#endif

// Normal exponent. Deliberately *not* the a-trous kernel's NormalPhi (default 512):
// pow(dot, 512) puts a tap 8 degrees off the centre normal at 1e-3, so on any curved surface
// every tap would collapse and the resolve would degenerate to nearest-neighbour -- visibly
// blocky. 32 keeps a smooth surface intact (dot 0.99 -> 0.72) while still cutting a 25 degree
// crease down to 0.05.
#define SSRT_SPARSE_NORMAL_PHI 32.0f

// What a pixel with no usable sample publishes. Radiance 0 and confidence 0 say "the rays
// resolved nothing here", which under ambient reinjection makes DeferredCompositeCS keep the
// whole vanilla ambient -- the honest answer. Hit distance 1.0 is the ray march's own "as
// distant as the encoding can say" sentinel, which ssrt_spatial.hlsl maps to the unmodified
// kernel.
#define SSRT_SPARSE_NO_SAMPLE_HITDIST 1.0f

void SSRT_SparseStore(uint2 px, float4 color, float confidence, float hitDist)
{
	ResolvedColorOutput[px] = color;
	ResolvedConfidenceOutput[px] = confidence;
	ResolvedHitDistOutput[px] = hitDist;
}

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID)
{
	const uint2 renderExtent = SSRT_GetRenderExtent();
	const uint2 px = DTid.xy;

	if (any(px >= renderExtent)) {
		// Write rather than return: the composite's dispatch rounds up to whole 8x8 groups as
		// well, so it can read a few texels past the render sub-rect. Publishing the no-sample
		// triple keeps them deterministic instead of leaving whatever the previous frame left.
		SSRT_SparseStore(px, 0.0f, 0.0f, SSRT_SPARSE_NO_SAMPLE_HITDIST);
		return;
	}

	const float depthFull = DepthTexture[px];
	if (SSRT_IS_FAR_PLANE(depthFull)) {
		// (audit P1's argument, one pass later) Nothing computed for a far-plane pixel can reach
		// the frame: the diffuse composite multiplies by the albedo G-buffer, which is 0 wherever
		// no deferred geometry was rasterised, and REBLUR early-outs past its denoising range. So
		// skip the taps and publish the same triple a far-plane ray-march lane resolves to.
		SSRT_SparseStore(px, 0.0f, 0.0f, SSRT_SPARSE_NO_SAMPLE_HITDIST);
		return;
	}

	const uint2 sparseExtent = SSRT_GetSparseExtent(renderExtent);

	float3 normalFull;
	float roughnessFull;
	GetNormalRoughness(px, normalFull, roughnessFull);
	const float linearFull = SharedData::GetScreenDepth(depthFull);

	float4 weightedColor = 0.0f;
	float weightedConfidence = 0.0f;
	float weightedHitDist = 0.0f;
	float weightSum = 0.0f;

	float4 bestColor = 0.0f;
	float bestConfidence = 0.0f;
	float bestHitDist = SSRT_SPARSE_NO_SAMPLE_HITDIST;
	float bestGeomWeight = -1.0f;

	// One tap: `tap` is the compact texel, `guidePx` the full-resolution pixel its guide is read
	// from, `guideDepth` the depth of the surface the sample was actually traced from, and
	// `spatial` the reconstruction weight (bilinear for half resolution, uniform for
	// checkerboard, where every tap is the same distance away).
#define SSRT_SPARSE_TAP(tap, guidePx, guideDepth, spatial)                                                     \
	{                                                                                                          \
		const float _tapDepth = (guideDepth);                                                                  \
		/* A tap on the far plane never held a traced sample -- sky, or a compact texel outside the  */         \
		/* render sub-rect. Excluded outright rather than down-weighted. */                                     \
		const float _valid = SSRT_IS_FAR_PLANE(_tapDepth) ? 0.0f : 1.0f;                                       \
		const float _linearTap = SharedData::GetScreenDepth(_tapDepth);                                        \
		const float _relative = abs(_linearTap - linearFull) / max(linearFull, 1e-5f);                          \
		const float _weightDepth = exp(-_relative / SSRT_SPARSE_DEPTH_SCALE);                                  \
		float3 _normalTap;                                                                                     \
		float _roughnessTap;                                                                                   \
		GetNormalRoughness((guidePx), _normalTap, _roughnessTap);                                              \
		const float _weightNormal = pow(max(0.0f, dot(normalFull, _normalTap)), SSRT_SPARSE_NORMAL_PHI);       \
		const float _geom = _weightDepth * _weightNormal * _valid;                                             \
		const float4 _color = SparseColorTexture[(tap)];                                                       \
		const float _conf = SparseConfidenceTexture[(tap)];                                                    \
		const float _hitd = SparseHitDistTexture[(tap)];                                                       \
		const float _w = (spatial) * _geom;                                                                    \
		weightedColor += _color * _w;                                                                          \
		weightedConfidence += _conf * _w;                                                                      \
		weightedHitDist += _hitd * _w;                                                                          \
		weightSum += _w;                                                                                       \
		if (_geom > bestGeomWeight) {                                                                          \
			bestGeomWeight = _geom;                                                                             \
			bestColor = _color;                                                                                 \
			bestConfidence = _conf;                                                                             \
			bestHitDist = _hitd;                                                                                \
		}                                                                                                      \
	}

#if defined(SSRT_SPARSE_HALFRES)
	// THE HALF-RESOLUTION RECONSTRUCTION.
	//
	// Modelled on SSGI's upsample but deliberately not a copy of it. SSGI takes a binary decision
	// per pixel -- a relative depth-spread test picks either an inverse-depth-weighted blend of
	// the 2x2 or a plain bilinear tap -- which is right for an AO term. Indirect diffuse carries
	// *colour* across normal discontinuities, where a depth-only guide cannot tell a wall from
	// the floor it meets at a corner: both are continuous in depth, and blending across them
	// drags bounce light onto the wrong surface. So this version always blends, with bilinear
	// weights multiplied by geometric ones -- which degenerates to exact bilinear on flat
	// surfaces (all guide weights ~1) instead of switching between two reconstructions and
	// showing the seam -- adds a normal term so corners and silhouettes stop the blend, and falls
	// back to the single most geometrically similar tap when every weight collapses, which is
	// precisely where a bilinear average would be wrong (a thin full-resolution feature with no
	// matching compact sample).
	//
	// The guide asymmetry is intentional and worth stating plainly. The depth half is exact: mip
	// 1 holds the 2x2 minimum, which *is* the depth of the argmin subpixel the ray march traced
	// from. The normal half is read at the block's top-left subpixel, which is not always that
	// same subpixel. Resolving the argmin per tap would cost four extra depth loads for each of
	// the four taps, and for an edge-stopping guide what matters is that the rule is one rule:
	// the depth term already carries every silhouette, leaving the normal term to catch creases
	// where all four subpixels lie on geometry continuous in depth and their normals are
	// therefore close. Inside a 2x2 that straddles a genuine crease there is no correct answer at
	// half resolution anyway -- the sample is one surface point being asked to serve two surfaces
	// -- and the fallback below is what that case lands on.
	const float2 srcPos = (float2(px) + 0.5f) * 0.5f - 0.5f;
	const int2 base = int2(floor(srcPos));
	const float2 frac2 = srcPos - float2(base);

	[unroll] for (int dy = 0; dy < 2; dy++)
	{
		[unroll] for (int dx = 0; dx < 2; dx++)
		{
			const int2 tap = clamp(base + int2(dx, dy), int2(0, 0), int2(sparseExtent) - 1);
			const float bilinear = (dx == 0 ? 1.0f - frac2.x : frac2.x) * (dy == 0 ? 1.0f - frac2.y : frac2.y);
			SSRT_SPARSE_TAP(tap, uint2(tap) * 2, HalfDepthTexture[tap], bilinear)
		}
	}
#elif defined(SSRT_SPARSE_CHECKERBOARD)
	// THE CHECKERBOARD RECONSTRUCTION.
	//
	// A pixel the phase selected this frame owns a sample of itself, so it is copied through
	// exactly -- bit for bit, no filtering at all. That is the whole quality argument for this
	// mode: half the frame is untouched full-resolution data every frame, and (because the phase
	// alternates) every pixel is that half every other frame.
	//
	// A pixel the phase skipped has three samples one texel away, and every one of their guides
	// is exact because every compact texel corresponds to a real full-resolution pixel:
	//   * the rows above and below traced *this very column* -- the phase flips with y, so
	//     (compact.x, y-1) and (compact.x, y+1) both resolve to column px.x. NRD's own
	//     checkerboard resolve, in REBLUR_PrePass.cs.hlsl, blends x-1 and x+1 instead: two
	//     samples of the neighbouring columns. Ours are two samples of the correct column.
	//   * its own pair partner at (compact.x, y). Kept as a third tap rather than as a fallback so
	//     that a pixel at the top or bottom edge of the render sub-rect, or one whose vertical
	//     neighbours are on other geometry, still has something with the right spatial weight.
	// All three are at Manhattan distance 1, so the spatial weight is uniform and the geometric
	// weights do all the work.
	//
	// Every tap's guide is read at the full-resolution pixel that compact texel was actually
	// traced from, which SSRT_SparseCheckerColumn gives exactly. Deriving the guide from the tap
	// rather than assuming it (px.x for the vertical pair, px.x ^ 1 for the partner) is what makes
	// the odd-render-width edge case below correct instead of merely bounded: no tap's weight is
	// ever computed against a different surface than the sample it weights.
	//
	// The compact extent is floor(renderExtent.x / 2), so the last column of an *odd* render width
	// has no compact texel of its own -- the ray march never dispatched one. Clamping borrows the
	// pair to its left, whose samples are one or two columns away and whose guides come out of
	// SSRT_SparseCheckerColumn correctly, instead of reading a texel nothing ever wrote.
	const uint compactX = min(px.x >> 1, sparseExtent.x - 1u);
	const bool ownsSample = (px.x >> 1) < sparseExtent.x;

	if (ownsSample && SSRT_SparseCheckerIsTraced(px)) {
		const uint2 own = uint2(compactX, px.y);
		SSRT_SparseStore(px, SparseColorTexture[own], SparseConfidenceTexture[own], SparseHitDistTexture[own]);
		return;
	}

	[unroll] for (int k = 0; k < 3; k++)
	{
		const int ty = int(px.y) + (k - 1);
		if (ty < 0 || ty >= int(renderExtent.y))
			continue;
		const uint2 tap = uint2(compactX, (uint)ty);
		const uint2 guidePx = uint2(SSRT_SparseCheckerColumn(tap), (uint)ty);
		SSRT_SPARSE_TAP(tap, guidePx, DepthTexture[guidePx], 1.0f)
	}
#else
#	error "ssrt_sparse_resolve.hlsl needs SSRT_SPARSE_HALFRES or SSRT_SPARSE_CHECKERBOARD; there is nothing to resolve otherwise."
#endif

#undef SSRT_SPARSE_TAP

	// The threshold is on the weighted sum, so a pixel sitting almost exactly on one tap keeps
	// that tap even if the others are rejected. Only when no tap survives at all does the
	// nearest-in-guide-space fallback take over; if even that is invalid (an all-sky
	// neighbourhood) the pixel gets the no-sample triple.
	float4 outColor = bestColor;
	float outConfidence = bestConfidence;
	float outHitDist = bestHitDist;
	if (bestGeomWeight <= 0.0f) {
		outColor = 0.0f;
		outConfidence = 0.0f;
		outHitDist = SSRT_SPARSE_NO_SAMPLE_HITDIST;
	}
	if (weightSum > 1e-5f) {
		const float invWeight = 1.0f / weightSum;
		outColor = weightedColor * invWeight;
		outConfidence = weightedConfidence * invWeight;
		outHitDist = weightedHitDist * invWeight;
	}

	SSRT_SparseStore(px, outColor, saturate(outConfidence), saturate(outHitDist));
}
