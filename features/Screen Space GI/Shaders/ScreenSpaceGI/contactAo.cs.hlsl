///////////////////////////////////////////////////////////////////////////////////////////////////
// Contact ambient occlusion.
//
// A centimetre-scale, full-resolution depth-buffer AO with its own small temporal accumulator,
// folded into Screen Space GI's AO channel so that every existing consumer of that channel picks it
// up with no change of its own.
//
// WHY IT LIVES HERE. The same estimator used to be evaluated in three places -- Environment
// Ambient's own term in DeferredCompositeCS, the SSRT ambient-reinjection path in the same file, and
// the SSRT diffuse fallback inside ssrt_raymarch.hlsl -- each of them a raw ten-tap estimate with no
// filtering of any kind. Three copies of one signal, three chances to drift, and no control over its
// precision. There is now one evaluation per frame per pixel, one accumulator, and one output: the
// SSGI AO texture. DeferredCompositeCS's multiBounceAO, ssrt_raymarch's `ao *= 1 - SsgiAo` and
// Environment Ambient's envAo all read it exactly as they did before.
//
// WHY IT IS FULL RESOLUTION EVEN WHEN SSGI IS NOT. The whole point of the term is the scale SSGI
// cannot reach: 15 cm is a fraction of a pixel's worth of screen space at half resolution, so
// running it at SSGI's internal resolution would destroy the only thing it contributes. It therefore
// dispatches at the render extent whatever ResolutionMode says, and deliberately does *not* use the
// RES_MIP / READ_DEPTH / OUT_FRAME_DIM macros from common.hlsli (which follow ResolutionMode). The
// resolution defines are still passed to this file, and are used for exactly one thing: selecting
// where the composite into the AO channel happens (see CONTACT_COMPOSE below).
//
// WHY THE RAW DEPTH BUFFER AND NOT texWorkingDepth. texWorkingDepth mip 0 *is* full resolution and
// would have saved a binding, but it is R16_FLOAT view-space depth, whose relative precision is
// ~2^-11. At 1000 game units of depth that is half a game unit of quantisation against a search
// radius of ~10.5 units, which is enough to manufacture false occlusion on flat surfaces seen at
// distance -- exactly the artefact ContactBias exists to absorb, at an amplitude it cannot. The
// hardware depth buffer's 1/z distribution is two orders of magnitude finer there, and linearising
// it costs one mad and one divide per tap. This is also the same input the three kernels this
// replaces read, so the signal is directly comparable to theirs.
///////////////////////////////////////////////////////////////////////////////////////////////////

#include "Common/Game.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/VR.hlsli"
#include "ScreenSpaceGI/common.hlsli"

// Full resolution is the only mode without an upsample pass, so it is the only one where this pass
// has to do the composite into the AO channel itself. In half and quarter res the upsample pass is
// already reading and rewriting the AO channel at the render extent, and folds this term in there
// for free (see CONTACT_AO in upsample.cs.hlsl).
#if !defined(HALF_RES) && !defined(QUARTER_RES)
#	define CONTACT_COMPOSE
#endif

Texture2D<float> srcNDCDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);
Texture2D<float4> srcMotionVec : register(t2);
Texture2D<unorm float> srcPrevContact : register(t3);
#ifdef CONTACT_COMPOSE
Texture2D<unorm float> srcAo : register(t4);
#endif

RWTexture2D<unorm float> outContact : register(u0);
#ifdef CONTACT_COMPOSE
RWTexture2D<unorm float> outAo : register(u1);
#endif

///////////////////////////////////////////////////////////////////////////////
// Kernel constants. Carried over unchanged from the estimator this replaces, so a saved
// Contact Strength keeps its meaning: ten taps on a golden-angle spiral, 2-64 px radius clamp,
// cosine bias 0.1, and the 4/N normalisation that puts strength 1 on full absorption at tight
// contact and ~0.4 at a right-angle corner.

static const uint ContactSamples = 10;
static const float MinContactPixels = 2.0;
static const float MaxContactPixels = 64.0;
static const float ContactBias = 0.1;

// Temporal window, in frames. Deliberately a constant of this pass and *not* SSGI's
// MaxAccumFramesAO:
//
//  * MaxAccumFramesAO defaults to 4 because SSGI's AO has no defence against a moving occluder
//    other than its depth-disocclusion test, and that test cannot see one: when an arm sweeps
//    across a wall the *wall* pixel's depth does not change, so the history passes and the arm's
//    occlusion trails behind it. Shortening the window is the only lever there. The neighbourhood
//    fence below closes exactly that hole, so this signal does not need the same lever and would
//    only lose stability to it.
//  * It is also a user-facing slider that may legitimately be set to 1, which would turn this
//    accumulator off entirely and leave the contact term at raw single-frame noise -- the state
//    this whole pass exists to end.
//
// 8 frames gives an EMA variance factor of alpha / (2 - alpha) = 1/15, i.e. ~3.9x less flicker
// than the raw ten-tap estimate, while the fence bounds the worst-case lag independently of this
// number (see below). Longer windows buy little: sqrt improvement against a linear increase in the
// amount of history the fence has to catch.
static const float ContactAccumFrames = 8.0;

// Slack on the neighbourhood fence, in occlusion units. Two R8_UNORM quanta.
//
// Needed only for the degenerate case: on a perfectly flat, perfectly unoccluded surface every
// neighbour reports the same value, the fence collapses to a point, and clamp() would pin the
// history to the current frame -- no accumulation at all. There is nothing to accumulate there, so
// this costs nothing in quality, but it keeps the mechanism from being *defined* as a no-op on the
// easiest input, and it keeps R8 quantisation of the history from being read as disagreement.
static const float ContactFenceSlack = 2.0 / 255.0;

#ifdef TEMPORAL_DENOISER
// One current-frame contact value per thread, for the neighbourhood fence. 256 bytes.
groupshared float g_contact[8][8];
#endif

/**
 * @brief Near-field self-occlusion for one pixel, from the hardware depth buffer.
 *
 * Alchemy/HBAO cosine form: each tap contributes saturate(dot(N, dir) - bias) * falloff(distance).
 * The cosine factor is the grazing-surface guard -- a neighbour lying in this pixel's own tangent
 * plane has dot ~ 0 and contributes nothing, so a flat surface seen at a glancing angle does not
 * self-darken -- and the quadratic range falloff keeps the term strictly local, so it cannot
 * double-count what Skylighting or SSGI's own large-radius pass already applied at their scales.
 *
 * Everything is in view space. Only differences of positions are used and the G-buffer normal is
 * already view space, so there is no reason to pay a world-space transform: the old world-space
 * spelling existed because its caller (the deferred composite) had a world position to hand, not
 * because the estimator wanted one.
 *
 * @param a_px       Render-extent pixel coordinate; also the depth texel and the jitter seed.
 * @param a_posVS    This pixel's view-space position.
 * @param a_normalVS View-space geometric normal, pointing towards the camera.
 * @param a_viewZ    This pixel's view-space depth.
 * @param a_eyeIndex VR eye.
 * @param a_phase    Spiral rotation, radians.
 * @return Visibility in [0, 1]; 1 is unoccluded.
 */
float EvaluateContact(uint2 a_px, float3 a_posVS, float3 a_normalVS, float a_viewZ, uint a_eyeIndex, float a_phase)
{
	const float radius = max(ContactRadius, 0.1) / GAME_UNIT_TO_CM;
	const float radiusSq = radius * radius;

	// View units per pixel at this depth, measured by reconstructing the next texel across at the
	// *same* depth. Needs no knowledge of the projection convention and is automatically right
	// under dynamic resolution and in VR.
	const float2 uvRight = (float2(a_px) + float2(1.5, 0.5)) * RcpFrameDim;
	const float3 posRight = ScreenToViewPosition(Stereo::ConvertFromStereoUV(uvRight, a_eyeIndex), a_viewZ, a_eyeIndex);
	const float unitsPerPixel = length(posRight - a_posVS);

	// Clamped at both ends: the floor keeps a distant pixel's taps on distinct texels, and the
	// ceiling stops a near-field pixel from turning this into a full-screen AO pass.
	const float pixelRadius = clamp(radius / max(unitsPerPixel, 1e-4), MinContactPixels, MaxContactPixels);

	// The clamp is the *render* extent, not the texture extent: under dynamic resolution only the
	// sub-rect [0, FrameDim) of the depth buffer holds this frame's values, so a 64-pixel tap
	// radius near its edge has to stop there. The estimator this replaces had to be told which of
	// the two extents its caller meant; here there is only one caller and one right answer.
	int2 lo = int2(0, 0);
	int2 hi = int2(FrameDim) - 1;
#if defined(VR)
	// Keep every tap inside this eye's half of the side-by-side buffer.
	int eyeWidth = (int)(FrameDim.x * 0.5);
	lo.x = (int)a_eyeIndex * eyeWidth;
	hi.x = lo.x + eyeWidth - 1;
#endif

	float occlusion = 0.0;
	[unroll] for (uint i = 0; i < ContactSamples; ++i)
	{
		// Golden-angle spiral: near-uniform disk coverage from very few taps. The sqrt spaces the
		// radii by equal area instead of piling taps up at the centre.
		float t = (float(i) + 0.5) / float(ContactSamples);
		float angle = a_phase + float(i) * 2.39996323;

		float2 dir;
		sincos(angle, dir.y, dir.x);
		int2 coord = clamp(int2(a_px) + int2(round(dir * (sqrt(t) * pixelRadius))), lo, hi);

		float tapNDC = srcNDCDepth[coord];

		float2 tapUv = (float2(coord) + 0.5) * RcpFrameDim;
		float3 tapPos = ScreenToViewPosition(Stereo::ConvertFromStereoUV(tapUv, a_eyeIndex), ScreenToViewDepth(tapNDC), a_eyeIndex);

		float3 v = tapPos - a_posVS;
		float distSq = dot(v, v);

		// Grazing-surface guard; the bias also absorbs depth quantisation on flat surfaces. A tap
		// that rounds onto this very pixel gives v = 0 and cosine 0, so it drops out.
		float cosine = dot(a_normalVS, v * rsqrt(max(distSq, 1e-8)));

		// Strictly local: zero at and beyond the radius.
		float falloff = saturate(1.0 - distSq / radiusSq);

		// Sky and far-plane neighbours occlude nothing. Selected rather than multiplied by a 0/1
		// mask: ScreenToViewDepth at the far plane can produce a value whose reconstruction is not
		// finite, and NaN * 0 is NaN whereas movc simply does not read the rejected operand.
		occlusion += (tapNDC < 1.0) ? saturate(cosine - ContactBias) * falloff : 0.0;
	}

	occlusion *= ContactStrength * (4.0 / float(ContactSamples));

	return saturate(1.0 - occlusion);
}

[numthreads(8, 8, 1)] void main(
	uint2 dtid : SV_DispatchThreadID, uint2 gtid : SV_GroupThreadID)
{
	// No early return: the group barrier below sits in flow control no thread may skip. Threads
	// past the render extent evaluate the clamped border pixel instead, so the LDS tile always
	// holds real values, and only their final writes are suppressed.
	const int2 maxPx = int2(FrameDim) - 1;
	const uint2 px = (uint2)clamp(int2(dtid), int2(0, 0), maxPx);

	const float2 uv = (float2(px) + 0.5) * RcpFrameDim;
	const uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	const float2 screenPos = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	const float ndcDepth = srcNDCDepth[px];

	float curr = 1.0;
	// The sky has nothing to self-occlude and every tap there is a far-plane tap, so the kernel
	// would return 1 after paying for ten loads. Same gate the three estimators this replaces used.
	[branch] if (ndcDepth < 1.0)
	{
		const float viewZ = ScreenToViewDepth(ndcDepth);
		const float3 posVS = ScreenToViewPosition(screenPos, viewZ, eyeIndex);
		const float3 normalVS = GBuffer::DecodeNormal(srcNormalRoughness[px].xy);

		// With an accumulator of its own the spiral can rotate with the frame again, which is what
		// makes the ten taps an unbiased estimate of the disk rather than one fixed pattern's
		// opinion of it. InterleavedGradientNoise cycles its frame term with period 16, so the
		// sequence is finite and the EMA converges on the mean instead of chasing it.
		//
		// Without the accumulator there is nothing to average the rotation away, so the phase is
		// frozen instead: a fixed phase leaves a high-frequency, *motionless* grain, which is what
		// the frame-static variant of this kernel did on the SSRT fallback path and is the correct
		// degradation for "user turned the temporal denoiser off".
#ifdef TEMPORAL_DENOISER
		const float phase = Random::InterleavedGradientNoise(float2(px), SharedData::FrameCount) * Math::TAU;
#else
		const float phase = Random::InterleavedGradientNoise(float2(px)) * Math::TAU;
#endif

		curr = EvaluateContact(px, posVS, normalVS, viewZ, eyeIndex, phase);
	}

	float accum = curr;

#ifdef TEMPORAL_DENOISER
	///////////////////////////////////////////////////////////////////////////
	// (contact AO fence) Ghosting is bounded by the current frame's own neighbourhood, not by a
	// geometric disocclusion test.
	//
	// Why that is the right instrument for *this* signal. A disocclusion test asks "is the history
	// I fetched from the same surface?", and for a moving occluder over a static background the
	// honest answer is yes -- the background's depth and normal are unchanged -- which is precisely
	// how SSGI's AO acquires dark trails. This signal does not need to ask that question, because
	// two things are true of it and of nothing else in the chain: it is bounded in [0, 1], and
	// every pixel has a complete, fresh, unbiased estimate of it every single frame. So the history
	// is only ever used to *refine* a value we already have, and the current frame can be asked
	// directly whether the history is plausible.
	//
	// Why the neighbourhood and not a fixed +-window. The fence has to be tight where the signal is
	// smooth and loose where it is not, and the 3x3 spread delivers that for free: neighbouring
	// pixels draw independent spiral phases (InterleavedGradientNoise is a per-pixel dither), so
	// their spatial spread tracks the estimator's temporal spread. On a clean surface all nine taps
	// agree near 1, the window is a point, and a trail is killed the frame it appears. In a
	// transition region the window opens to the estimator's own noise band and the EMA is allowed
	// to do its job. A fixed window cannot be both.
	//
	// Cost and consequences. One 256-byte LDS tile and one barrier; no extra depth taps, because
	// the fence reuses the value each thread already computed. The window is therefore the group's
	// own 8x8 tile: neighbours outside it are replaced by the nearest inside it, which is still a
	// genuine current-frame sample one texel further away. A 1-pixel apron would cost 100
	// evaluations per 64 threads (1.56x the whole pass) to remove an anisotropy at group borders
	// that is a fraction of ContactFenceSlack wide.
	//
	// It also makes history initialisation a non-issue: whatever an uninitialised or stale R8
	// texture contains, clamp() drags it inside this frame's own range before it is used.
	g_contact[gtid.y][gtid.x] = curr;
	GroupMemoryBarrierWithGroupSync();

	float fenceLo = curr;
	float fenceHi = curr;
	[unroll] for (int dy = -1; dy <= 1; ++dy)
	{
		[unroll] for (int dx = -1; dx <= 1; ++dx)
		{
			int2 n = clamp(int2(gtid) + int2(dx, dy), int2(0, 0), int2(7, 7));
			float s = g_contact[n.y][n.x];
			fenceLo = min(fenceLo, s);
			fenceHi = max(fenceHi, s);
		}
	}
	fenceLo = max(fenceLo - ContactFenceSlack, 0.0);
	fenceHi = min(fenceHi + ContactFenceSlack, 1.0);

	// Reprojection, same convention as radianceDisocc.cs.hlsl: motion vectors are in stereo-free
	// screen space, the history texture is TexDim-sized with the render extent at its origin.
	const float2 prevScreenPos = screenPos + srcMotionVec[px].xy;
	const bool histValid = !(any(prevScreenPos < 0) || any(prevScreenPos > 1));
	[branch] if (histValid)
	{
		const float2 prevTexCoord = Stereo::ConvertToStereoUV(prevScreenPos, eyeIndex) * (FrameDim * RcpTexDim);
		// R8_UNORM, so the fetch is a [0, 1] value by construction and no finiteness test on it
		// could ever fire -- the format has no encoding for NaN, Inf, or an out-of-range value.
		// The same is true of everything this pass writes, which is why the whole guard family in
		// common.hlsli (filterNaN / filterInf / isFiniteSafe) is absent here: the only float that
		// is not format-bounded is `curr`, and it leaves EvaluateContact through saturate().
		const float hist = srcPrevContact.SampleLevel(samplerLinearClamp, prevTexCoord, 0);
		accum = lerp(clamp(hist, fenceLo, fenceHi), curr, rcp(ContactAccumFrames));
	}
#endif

	[branch] if (all(dtid < (uint2)FrameDim))
	{
		outContact[dtid] = accum;
#ifdef CONTACT_COMPOSE
		// SSGI's AO channel stores *occlusion*; its consumers read `1 - value` as visibility. So
		// combining the two visibilities is a single multiply in that space.
		outAo[dtid] = saturate(1.0 - (1.0 - srcAo[dtid]) * accum);
#endif
	}
}
