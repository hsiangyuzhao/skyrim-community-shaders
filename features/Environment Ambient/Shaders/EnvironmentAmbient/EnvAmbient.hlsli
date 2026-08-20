#ifndef __ENV_AMBIENT_HLSLI__
#define __ENV_AMBIENT_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
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

		float3 envNoSky = EnvTexture.SampleLevel(LinearSampler, a_normalWS, mip);

		// Only the sky increment is attenuated by sky visibility, as in the SSRT fallback
		// (ssrt_raymarch.hlsl:619-622); attenuating the whole colour would fade the non-sky
		// environment away as well.
		float3 skyOnly = 0.0;
		float visibility = 1.0;
#if !defined(INTERIOR)
		skyOnly = max(EnvReflectionsTexture.SampleLevel(LinearSampler, a_normalWS, mip) - envNoSky, 0.0);
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
