#ifndef __ENV_AMBIENT_HLSLI__
#define __ENV_AMBIENT_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Game.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

// Environment Ambient ("L1")
//
// Directional ambient light from the prefiltered dynamic cubemaps, modulated by Skylighting sky
// visibility. This is a per-pixel port of the Screen Space Ray Tracing dynamic-cubemap diffuse
// fallback (features/Screen Space Ray Tracing/Shaders/ScreenSpaceRayTracing/
// ssrt_raymarch.hlsl:587-645), which runs once per ray (DiffuseSPP=2 by default).
//
// PARITY IS THE GOAL, INCLUDING THE FALLBACK'S QUIRKS. Every default here reproduces a specific
// line of that block:
//   * EnvMip 2.0                    ssrt_raymarch.hlsl:593  `const uint sampleMip = 2`
//   * cosine-hemisphere directions  ssrt_raymarch.hlsl:378  CosineSampleHemisphereConcentric
//   * no Color::Ambient() wrap      ssrt_raymarch.hlsl:595  the fallback omits it
//   * Normalization 0.0             ScreenSpaceRayTracing.h:59 CubemapNormalization = 0.0f
//   * MultiBounceAO on gamma albedo ssrt_raymarch.hlsl:640  `MultiBounceAO(albedo, ao)`
//   * linear-space composite        ssrt_diffuse_composite.hlsl:20
// Anything "more correct" than the fallback sits behind a non-default option or is gone.
//
// This header is included by package/Shaders/DeferredCompositeCS.hlsl after its resource
// declarations and deliberately reads those globals directly:
//   EnvTexture (t6)              no-sky prefiltered cubemap    [DYNAMIC_CUBEMAPS]
//   EnvReflectionsTexture (t7)   with-sky prefiltered cubemap  [DYNAMIC_CUBEMAPS]
//   LinearSampler (s0)                                         [DYNAMIC_CUBEMAPS]
//   DepthTexture (t4)            full-resolution depth, for the contact occlusion below
// It declares no resources of its own and needs no new register. Skylighting is sampled by the
// caller, which needs the same probe fetch for its own ambient estimate.
//
// Colour space: the cubemaps hold "irradiance-gamma" values (SpecularIrradianceCS.hlsl writes
// Color::IrradianceToGamma), which is the space the composite's ambient term lives in, so the
// result can be blended against the vanilla ambient directly. Because IrradianceToLinear is a pure
// power function, IrradianceToLinear(env * albedo) == IrradianceToLinear(env) *
// IrradianceToLinear(albedo), i.e. multiplying albedo in gamma space here is bit-equivalent to the
// fallback multiplying it in linear space in ssrt_diffuse_composite.hlsl:20.

namespace EnvironmentAmbient
{
	static const uint MaxSamples = 8;

	// Branchless orthonormal basis around a unit vector.
	// [Duff et al. 2017, "Building an Orthonormal Basis, Revisited"]
	void GetOrthonormalBasis(float3 a_n, out float3 o_tangent, out float3 o_bitangent)
	{
		float s = a_n.z >= 0.0 ? 1.0 : -1.0;
		float a = -1.0 / (s + a_n.z);
		float b = a_n.x * a_n.y * a;
		o_tangent = float3(1.0 + s * a_n.x * a_n.x * a, s * b, -s * a_n.x);
		o_bitangent = float3(b, s + a_n.y * a_n.y * a, -a_n.y);
	}

	// Verbatim port of ssrt_common.hlsli:89-105 so the ray distribution is the fallback's.
	float3 ConcentricDiskSamplingHelper(float2 a_e)
	{
		float2 p = 2.0 * a_e - 0.99999994;
		float2 a = abs(p);
		float lo = min(a.x, a.y);
		float hi = max(a.x, a.y);
		const float epsilon = 5.42101086243e-20;  // 2^-64, avoids 0/0
		float phi = (Math::PI / 4.0) * (lo / (hi + epsilon) + 2.0 * float(a.y >= a.x));
		const uint signMask = 0x80000000;
		float2 disk = asfloat((asuint(float2(cos(phi), sin(phi))) & ~signMask) | (asuint(p) & signMask));
		return float3(disk, hi);
	}

	// Verbatim port of ssrt_common.hlsli:123-129 (the PDF is not needed: the cubemap tap is already
	// the cosine-weighted estimate, so the cosine-weighted mean is a plain average).
	float3 CosineSampleHemisphereConcentric(float2 a_e)
	{
		float3 result = ConcentricDiskSamplingHelper(a_e);
		float sinTheta = result.z;
		float cosTheta = sqrt(max(0.0, 1.0 - sinTheta * sinTheta));
		return float3(result.xy * sinTheta, cosTheta);
	}

	// Verbatim port of ssrt_common.hlsli:168-173.
	float2 Hammersley16(uint a_index, uint a_numSamples, uint2 a_random)
	{
		float e1 = frac((float)a_index / (float)a_numSamples + float(a_random.x) * (1.0 / 65536.0));
		float e2 = float((reversebits(a_index) >> 16) ^ a_random.y) * (1.0 / 65536.0);
		return float2(e1, e2);
	}

	// One prefiltered cubemap tap. The sky increment is kept separate so only it can be attenuated
	// by sky visibility, and the max() is taken per direction so a direction where the sky map is
	// darker than the no-sky map cannot borrow brightness from its neighbours
	// (ssrt_raymarch.hlsl:617-619).
	void AccumulateDirection(float3 a_dir, float a_mip, inout float3 io_envNoSky, inout float3 io_skyOnly)
	{
		float3 noSky = EnvTexture.SampleLevel(LinearSampler, a_dir, a_mip);
		io_envNoSky += noSky;
#if !defined(INTERIOR)
		io_skyOnly += max(EnvReflectionsTexture.SampleLevel(LinearSampler, a_dir, a_mip) - noSky, 0.0);
#endif
	}

	////////////////////////////////////////////////////////////////////////////////////////////////
	// Contact occlusion
	//
	// The last piece of the fallback with no counterpart here was its near-field self-occlusion:
	// full-resolution, per-ray, and effective at centimetre scale, which is what darkens hair
	// against a face, cloth where it meets skin, and a window frame behind a character. Skylighting
	// works on a metre-scale probe grid and Screen Space GI is half-resolution with a large radius;
	// neither can reach that scale. This is a deliberately tiny depth-buffer AO that can.
	//
	// The estimator is the Alchemy/HBAO cosine form: each tap contributes
	// saturate(dot(N, dir) - bias) * falloff(distance). The cosine factor is the grazing-surface
	// guard - a neighbour lying in this pixel's own tangent plane has dot ~ 0 and contributes
	// nothing, so flat surfaces seen at a glancing angle do not self-darken - and the quadratic
	// range falloff keeps the term strictly local, so it cannot double-count what Skylighting or
	// SSGI already applied at their own scales.
	////////////////////////////////////////////////////////////////////////////////////////////////

	static const uint ContactSamples = 6;    // <= 8 depth Loads, unrolled
	static const float MinContactPixels = 2.0;
	static const float MaxContactPixels = 32.0;
	static const float ContactBias = 0.1;

	// Same reconstruction the caller uses for its own pixel (DeferredCompositeCS.hlsl:115-131), so
	// centre and neighbours land in one consistent camera-relative world space. Only differences of
	// these positions are used, so the camera-relative origin cancels.
	float3 ReconstructPositionWS(int2 a_coord, float a_depth, uint a_eyeIndex)
	{
		float2 uv = (float2(a_coord) + 0.5) * SharedData::BufferDim.zw;
		uv *= FrameBuffer::DynamicResolutionParams2.xy;
		uv = Stereo::ConvertFromStereoUV(uv, a_eyeIndex);

		float4 positionCS = float4(2.0 * float2(uv.x, -uv.y + 1.0) - 1.0, a_depth, 1.0);
		float4 positionWS = mul(FrameBuffer::CameraViewProjInverse[a_eyeIndex], positionCS);
		return positionWS.xyz / positionWS.w;
	}

	/**
	 * @brief Near-field ambient occlusion from the full-resolution depth buffer.
	 *
	 * @param a_pixCoord   Dispatch pixel coordinate, also the depth texel and the jitter seed.
	 * @param a_positionWS This pixel's camera-relative world position.
	 * @param a_normalWS   Geometric world-space normal.
	 * @param a_depth      This pixel's raw depth, used to measure the local pixel-to-world scale.
	 * @param a_eyeIndex   VR eye.
	 * @return Visibility in [0, 1]; 1 is unoccluded.
	 */
	float EvaluateContactOcclusion(uint2 a_pixCoord, float3 a_positionWS, float3 a_normalWS, float a_depth, uint a_eyeIndex)
	{
		const float radius = max(SharedData::envAmbientSettings.ContactRadius, 0.1) / GAME_UNIT_TO_CM;

		// World units per pixel at this depth, measured by reconstructing the next texel across at
		// the *same* depth. This needs no knowledge of the projection convention and is
		// automatically right under dynamic resolution and in VR.
		float3 positionRight = ReconstructPositionWS(int2(a_pixCoord) + int2(1, 0), a_depth, a_eyeIndex);
		float unitsPerPixel = length(positionRight - a_positionWS);

		// Clamped at both ends: the floor keeps a distant pixel's taps on distinct texels, and the
		// ceiling stops a near-field pixel from turning this into a full-screen AO pass.
		float pixelRadius = clamp(radius / max(unitsPerPixel, 1e-4), MinContactPixels, MaxContactPixels);

		int2 lo = int2(0, 0);
		int2 hi = int2(SharedData::BufferDim.xy) - 1;
#if defined(VR)
		// Keep every tap inside this eye's half of the side-by-side buffer. BufferDim is float, so
		// halve it before the cast rather than emitting an integer divide.
		int eyeWidth = (int)(SharedData::BufferDim.x * 0.5);
		lo.x = (int)a_eyeIndex * eyeWidth;
		hi.x = lo.x + eyeWidth - 1;
#endif

		// Per-pixel, per-frame spiral rotation. InterleavedGradientNoise cycles the frame term with
		// period 16, so the sequence is finite and TAA converges instead of chasing it.
		float phase = Random::InterleavedGradientNoise(float2(a_pixCoord), SharedData::FrameCount) * Math::TAU;
		float radiusSq = radius * radius;

		float occlusion = 0.0;
		[unroll] for (uint i = 0; i < ContactSamples; ++i) {
			// Golden-angle spiral: near-uniform disk coverage from very few taps. The sqrt spaces
			// the radii by equal area instead of piling taps up at the centre.
			float t = (float(i) + 0.5) / float(ContactSamples);
			float angle = phase + float(i) * 2.39996323;

			float2 dir;
			sincos(angle, dir.y, dir.x);
			int2 coord = clamp(int2(a_pixCoord) + int2(round(dir * (sqrt(t) * pixelRadius))), lo, hi);

			float sampleDepth = DepthTexture[coord];

			float3 v = ReconstructPositionWS(coord, sampleDepth, a_eyeIndex) - a_positionWS;
			float distSq = dot(v, v);

			// Grazing-surface guard, and the bias also absorbs depth quantisation on flat surfaces.
			// A tap that rounds onto this very pixel gives v = 0 and cosine 0, so it drops out.
			float cosine = dot(a_normalWS, v * rsqrt(max(distSq, 1e-8)));

			// Strictly local: zero at and beyond the radius.
			float falloff = saturate(1.0 - distSq / radiusSq);

			// Sky and far-plane neighbours occlude nothing.
			float valid = sampleDepth < 1.0 ? 1.0 : 0.0;

			occlusion += saturate(cosine - ContactBias) * falloff * valid;
		}

		// 2/N normalises a roughly half-occluded neighbourhood towards full occlusion at strength 1,
		// which puts a 90-degree corner near 0.2 and tight contact such as hair on skin near 0.5.
		occlusion *= SharedData::envAmbientSettings.ContactStrength * (2.0 / float(ContactSamples));

		return saturate(1.0 - occlusion);
	}

	// Sky visibility is sampled once per pixel by the caller (DeferredCompositeCS.hlsl), which needs
	// the same probe fetch for the IBL half of its ambient estimate. The fallback re-evaluates it
	// per ray, but its cosine-lobe integrand always uses the folded *surface* normal
	// (ssrt_raymarch.hlsl:605-606); the ray direction reaches Skylighting::sample only as the
	// receiver normal bias that offsets the probe fetch by one cell (Skylighting.hlsli:65), so per
	// ray it buys a little extra spatial dither and no directional gradient. Once per pixel is the
	// faithful reduction, and sharing the fetch with the caller costs nothing.

	/**
	 * @brief Evaluates the environment ambient term for one pixel.
	 *
	 * @param a_normalWS       Geometric world-space normal.
	 * @param a_albedo         G-buffer albedo, gamma space, applied last as the fallback does.
	 * @param a_pixCoord       Dispatch pixel coordinate (sample scramble seed).
	 * @param a_skyVisibility  The fallback's `skylightingDiffuse` (ssrt_raymarch.hlsl:602-612),
	 *                         computed by the caller. Attenuates the sky increment only.
	 * @param a_enclosure      Orientation-normalised sky openness, ~1 in the open. Stands in for the
	 *                         fallback's screen-space occlusion factor.
	 * @param o_occlusion      Proxy for the fallback's per-ray screen-space occlusion factor
	 *                         (ssrt_raymarch.hlsl:632). The caller folds it into MultiBounceAO.
	 * @return Environment ambient in the same space and scale as the vanilla ambient term
	 *         (albedo included, occlusion NOT included).
	 */
	float3 Evaluate(float3 a_normalWS, float3 a_albedo, uint2 a_pixCoord, float a_skyVisibility, float a_enclosure, out float o_occlusion)
	{
		const float mip = SharedData::envAmbientSettings.EnvMip;
		const uint numSamples = clamp(SharedData::envAmbientSettings.SampleCount, 1u, MaxSamples);
		const float spread = saturate(SharedData::envAmbientSettings.Spread);

		// The fallback's angular width came from the cosine-hemisphere ray distribution, not from
		// the mip: mip 2 is GGX roughness 2/7 (DynamicCubemaps.cpp:444), far sharper than a cosine
		// lobe. Sampling K cosine-hemisphere directions and averaging is the per-pixel equivalent;
		// a narrow cone around the normal is not, because it keeps almost no sky for vertical
		// normals and almost nothing but the zenith for horizontal ones.
		float3 tangent, bitangent;
		GetOrthonormalBasis(a_normalWS, tangent, bitangent);

		// Per-pixel, per-frame scramble of the Hammersley set, as in
		// SampleRandomVector2DBaked (ssrt_raymarch.hlsl:350-363). TAA resolves the residual noise.
		uint3 seed = uint3(a_pixCoord, Random::pcg3d(uint3(a_pixCoord, SharedData::FrameCount)).x);
		uint2 scramble = Random::pcg3d(seed).xy / 0x10000;

		float3 envNoSky = 0.0;
		float3 skyOnly = 0.0;

		[loop] for (uint i = 0; i < numSamples; ++i)
		{
			float3 local = CosineSampleHemisphereConcentric(Hammersley16(i, numSamples, scramble));
			float3 dir = local.x * tangent + local.y * bitangent + local.z * a_normalWS;

			// spread = 1 is the fallback's distribution; lower values collapse the hemisphere back
			// towards the surface normal for debugging or for a sharper look.
			dir = normalize(lerp(a_normalWS, dir, spread));

			AccumulateDirection(dir, mip, envNoSky, skyOnly);
		}

		float rcpSamples = rcp((float)numSamples);
		envNoSky *= rcpSamples;
		skyOnly *= rcpSamples;

		float3 env = envNoSky;

		[branch] if (SharedData::envAmbientSettings.Normalization > 0.0)
		{
			// ssrt_raymarch.hlsl:595 + 620-621, verbatim: no Color::Ambient() wrap (the fallback
			// omits it), ReflectionNormalisationScale included, mip 15 clamps to the 1x1x6 average.
			float envLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, a_normalWS, 15));
			float directionalAmbientLuminance = Color::RGBToLuminance(max(0.0, mul(SharedData::DirectionalAmbient, float4(a_normalWS, 1.0)))) * Color::ReflectionNormalisationScale;

			env = lerp(env, env * (directionalAmbientLuminance / max(envLuminance, 1e-4)), SharedData::envAmbientSettings.Normalization);
		}

		// Only the sky increment is attenuated by sky visibility (ssrt_raymarch.hlsl:622);
		// attenuating the whole colour would fade the non-sky environment away as well.
		env += skyOnly * a_skyVisibility;

		// The fallback's second, independent darkening factor: `lerp(1, occlusion, OcclusionStrength)`
		// where occlusion = 1 - hitConfidence from SSRT_ValidateHit (ssrt_raymarch.hlsl:299/307,
		// :632). It fires on self-intersecting and back-facing rays, and it multiplies the *whole*
		// env colour, not just the sky part. Nothing screen-space is available per pixel in the
		// composite, so the orientation-normalised sky openness stands in for it - normalised
		// because the factor it replaces is ray-hit driven and therefore ~1 in the open whatever way
		// the surface faces, which the raw hemisphere fraction is not. The sky increment does
		// consequently carry openness twice while the no-sky part carries it once; the fallback
		// carries two genuinely different signals. OcclusionStrength brackets the difference.
		o_occlusion = 1.0;
		[branch] if (SharedData::envAmbientSettings.ApplyAO != 0)
			o_occlusion = lerp(1.0, a_enclosure, saturate(SharedData::envAmbientSettings.OcclusionStrength));

		// Non-parity extras: both are identity at their defaults.
		env = Color::Saturation(env, SharedData::envAmbientSettings.Saturation);
		env *= SharedData::envAmbientSettings.Intensity;

		return env * a_albedo;
	}
}

#endif  // __ENV_AMBIENT_HLSLI__
