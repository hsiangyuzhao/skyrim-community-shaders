#ifndef __ENV_AMBIENT_HLSLI__
#define __ENV_AMBIENT_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

// Environment Ambient ("L1")
//
// Directional ambient light from the prefiltered dynamic cubemaps, modulated by Skylighting sky
// visibility. This is a once-per-pixel restatement of the Screen Space Ray Tracing dynamic-cubemap
// diffuse fallback (features/Screen Space Ray Tracing/Shaders/ScreenSpaceRayTracing/
// ssrt_raymarch.hlsl:588-644), which runs once per ray (DiffuseSPP=2 by default).
//
// This header is included by package/Shaders/DeferredCompositeCS.hlsl after its resource
// declarations and deliberately reads those globals directly:
//   EnvTexture (t6)              no-sky prefiltered cubemap    [DYNAMIC_CUBEMAPS]
//   EnvReflectionsTexture (t7)   with-sky prefiltered cubemap  [DYNAMIC_CUBEMAPS]
//   LinearSampler (s0)                                         [DYNAMIC_CUBEMAPS]
//   SkylightingProbeArray (t8), stbn_vec3_2Dx1D_128x128x64 (t9) [SKYLIGHTING]
// It declares no resources of its own and needs no new register.
//
// Colour space: the cubemaps hold "irradiance-gamma" values (SpecularIrradianceCS.hlsl writes
// Color::IrradianceToGamma), which is exactly the space the composite's ambient term lives in, so
// the result can be blended against ambDalc directly and the caller's single
// IrradianceToLinear/MultiBounceAO/IrradianceToGamma round trip stays correct under Linear
// Lighting and without it.

namespace EnvironmentAmbient
{
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

	// One prefiltered cubemap tap. The sky increment is kept separate so only it can be attenuated
	// by sky visibility, and the max() is taken per direction so a direction where the sky map is
	// darker than the no-sky map cannot borrow brightness from its neighbours.
	void AccumulateDirection(float3 a_dir, float a_mip, inout float3 io_envNoSky, inout float3 io_skyOnly)
	{
		float3 noSky = EnvTexture.SampleLevel(LinearSampler, a_dir, a_mip);
		io_envNoSky += noSky;
#if !defined(INTERIOR)
		io_skyOnly += max(EnvReflectionsTexture.SampleLevel(LinearSampler, a_dir, a_mip) - noSky, 0.0);
#endif
	}

	// Sky visibility along a_normalWS, evaluated once per pixel. The SSRT fallback evaluates this
	// per ray with the ray direction; using the surface normal is the once-per-pixel equivalent.
	float GetSkyVisibility(float3 a_normalWS, float3 a_positionMS, uint2 a_pixCoord)
	{
#if defined(SKYLIGHTING) && !defined(INTERIOR)
		sh2 skylighting = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, a_pixCoord, a_positionMS, a_normalWS);

		// Fold the lower hemisphere up, as the SSRT fallback does (ssrt_raymarch.hlsl:605).
		float3 visibilityNormal = normalize(float3(a_normalWS.xy, max(0, a_normalWS.z)));

		float visibility = SphericalHarmonics::FuncProductIntegral(skylighting, SphericalHarmonics::EvaluateCosineLobe(visibilityNormal)) / Math::PI;
		visibility = saturate(visibility);
		visibility = lerp(1.0, visibility, Skylighting::getFadeOutFactor(a_positionMS));
		visibility *= 1.0 + saturate(a_normalWS.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility);
		return Skylighting::mixDiffuse(SharedData::skylightingSettings, visibility);
#else
		return 1.0;
#endif
	}

	/**
	 * @brief Evaluates the environment ambient term for one pixel.
	 *
	 * @param a_normalWS   Geometric world-space normal.
	 * @param a_positionMS Model-space position (already VR eye-adjusted by the caller).
	 * @param a_albedo     G-buffer albedo, applied with the same convention as the DALC path.
	 * @param a_ambDalc    Ambient term separated out of MAIN: vanilla DALC chroma with the forward
	 *                     ground-truth luminance from Masks.z. Used as the normalization target.
	 * @param a_pixCoord   Dispatch pixel coordinate (Skylighting blue-noise seed).
	 * @return Environment ambient in the same space and scale as a_ambDalc (albedo included).
	 */
	float3 Evaluate(float3 a_normalWS, float3 a_positionMS, float3 a_albedo, float3 a_ambDalc, uint2 a_pixCoord)
	{
		const float mip = SharedData::envAmbientSettings.EnvMip;

		// A single tap of one prefiltered mip cannot express a cosine lobe: the SSRT fallback got
		// its angular width from cosine-hemisphere ray directions plus SVGF filtering. Averaging a
		// rotating triple widens the effective kernel and breaks up cubemap face seams. The taps
		// come from mips small enough to sit in cache (mip 4 is 8x8x6), so the cost is negligible.
		float3 envNoSky = 0.0;
		float3 skyOnly = 0.0;
		float sampleWeight = 1.0;

		AccumulateDirection(a_normalWS, mip, envNoSky, skyOnly);

		[branch] if (SharedData::envAmbientSettings.JitteredSampling != 0)
		{
			float3 tangent, bitangent;
			GetOrthonormalBasis(a_normalWS, tangent, bitangent);

			// Per-pixel, per-frame rotation; TAA resolves the residual noise.
			float phase = Random::InterleavedGradientNoise(float2(a_pixCoord), SharedData::FrameCount) * Math::TAU;

			float sinTheta, cosTheta;
			sincos(SharedData::envAmbientSettings.JitterAngle, sinTheta, cosTheta);

			// Perpendicular to a_normalWS with length sinTheta, so cosTheta * N +- tilt is already
			// unit length and needs no renormalisation.
			float3 tilt = sinTheta * (cos(phase) * tangent + sin(phase) * bitangent);

			AccumulateDirection(cosTheta * a_normalWS + tilt, mip, envNoSky, skyOnly);
			AccumulateDirection(cosTheta * a_normalWS - tilt, mip, envNoSky, skyOnly);

			sampleWeight = 3.0;
		}

		envNoSky /= sampleWeight;
		skyOnly /= sampleWeight;

		// Only the sky increment is attenuated by sky visibility, as in the SSRT fallback
		// (ssrt_raymarch.hlsl:619-622); attenuating the whole colour would fade the non-sky
		// environment away as well.
		float visibility = 1.0;
#if !defined(INTERIOR)
		visibility = GetSkyVisibility(a_normalWS, a_positionMS, a_pixCoord);
#endif

		float3 env = envNoSky;

		[branch] if (SharedData::envAmbientSettings.Normalization > 0.0)
		{
			// mip 15 clamps to the last mip (1x1x6), i.e. the average cubemap colour.
			float envLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, a_normalWS, 15));

			float scale;
			if (SharedData::envAmbientSettings.NormalizationMode == 0) {
				// Mode A: the SSRT fallback target (ssrt_raymarch.hlsl:595). Wrapped in
				// Color::Ambient, which that line omits, so the target sits in the same space as
				// the rest of the ambient pipeline when Linear Lighting is on.
				float targetLuminance = Color::RGBToLuminance(Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(a_normalWS, 1.0))))) * Color::ReflectionNormalisationScale;
				scale = targetLuminance / max(envLuminance, 1e-4);
			} else {
				// Mode B: target the forward ground truth in Masks.z. a_ambDalc already includes
				// albedo, so divide the albedo luminance back out to compare pre-albedo scales.
				// Needs no ReflectionNormalisationScale fudge and is identical with and without
				// Linear Lighting.
				float targetLuminance = Color::RGBToLuminance(a_ambDalc);
				scale = targetLuminance / max(envLuminance * Color::RGBToLuminance(a_albedo), 1e-4);
			}

			env = lerp(env, env * scale, SharedData::envAmbientSettings.Normalization);
		}

		env += skyOnly * visibility;

		// Saturation and intensity act on the light itself, before albedo, matching the Diffuse IBL
		// convention (IBL.hlsli / Lighting.hlsl:3241-3255).
		env = Color::Saturation(env, SharedData::envAmbientSettings.Saturation);
		env *= SharedData::envAmbientSettings.Intensity;

		return env * a_albedo;
	}
}

#endif  // __ENV_AMBIENT_HLSLI__
