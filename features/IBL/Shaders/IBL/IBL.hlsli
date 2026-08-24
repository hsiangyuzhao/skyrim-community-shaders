#ifndef __IBL_HLSLI__
#define __IBL_HLSLI__

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"

namespace ImageBasedLighting
{
#if defined(IBL_DEFERRED)
	Texture2D<sh2> DiffuseIBLTexture : register(t14);
	Texture2D<sh2> DiffuseSkyIBLTexture : register(t15);
#else
	Texture2D<sh2> DiffuseIBLTexture : register(t76);
	Texture2D<sh2> DiffuseSkyIBLTexture : register(t77);
	TextureCube<float4> StaticDiffuseIBLTexture : register(t78);
	TextureCube<float4> StaticSpecularIBLTexture : register(t79);
#endif
	float3 GetDiffuseIBL(float3 rayDir)
	{
		sh2 shR = DiffuseIBLTexture.Load(int3(0, 0, 0));
		sh2 shG = DiffuseIBLTexture.Load(int3(1, 0, 0));
		sh2 shB = DiffuseIBLTexture.Load(int3(2, 0, 0));
		float colorR = SphericalHarmonics::SHHallucinateZH3Irradiance(shR, rayDir);
		float colorG = SphericalHarmonics::SHHallucinateZH3Irradiance(shG, rayDir);
		float colorB = SphericalHarmonics::SHHallucinateZH3Irradiance(shB, rayDir);
		return float3(colorR, colorG, colorB) / Math::PI;
	}

	float3 GetSkyDiffuseIBL(float3 rayDir)
	{
		sh2 shR = DiffuseSkyIBLTexture.Load(int3(0, 0, 0));
		sh2 shG = DiffuseSkyIBLTexture.Load(int3(1, 0, 0));
		sh2 shB = DiffuseSkyIBLTexture.Load(int3(2, 0, 0));
		float colorR = SphericalHarmonics::SHHallucinateZH3Irradiance(shR, rayDir);
		float colorG = SphericalHarmonics::SHHallucinateZH3Irradiance(shG, rayDir);
		float colorB = SphericalHarmonics::SHHallucinateZH3Irradiance(shB, rayDir);
		return float3(colorR, colorG, colorB) / Math::PI;
	}

	// (B7) Saturation that is a *bit-exact* no-op at 1.0, which plain Color::Saturation is not:
	// it expands to lerp(grey, c, s), i.e. fl(grey + fl(c - grey)), and that round trip only
	// returns c exactly when the subtraction itself is exact - which by the usual float
	// subtraction argument needs exponent(c - grey) <= min(exponent(c), exponent(grey)). A
	// strongly tinted ambient breaks that: grey near 1 with a channel near 1e-3 loses the low
	// bits of the channel in the subtraction and does not get them back in the add. That is
	// invisible in absolute terms, but the four knobs below default to 1.0 precisely so a
	// default install is *identical*, not merely indistinguishable, and an unconditional
	// Color::Saturation would have spent that guarantee on a no-op. The select costs three movc.
	float3 TrimSaturation(float3 color, float saturation)
	{
		return saturation == 1.0 ? color : Color::Saturation(color, saturation);
	}

#if defined(SKYLIGHTING) && !defined(INTERIOR)
	float3 GetIBLColor(float3 rayDir, float skylighting)
#else
	float3 GetIBLColor(float3 rayDir)
#endif
	{
		// (B7) EnvIBLScale / SkyIBLScale / EnvIBLSaturation / SkyIBLSaturation, all defaulting to
		// 1.0. They exist because of where this function's output ends up: under SSRT's ambient
		// reinjection what survives in the frame is (1 - confidence) * (DALC + IBL probe), and
		// confidence goes to ~0 in a shadowed pocket facing the camera, so the split below is
		// what literally decides the colour of every gateway arch and eave the player looks into.
		// A single master saturation could not separate "the surroundings bounce too much green"
		// from "the sky is too blue"; these can.
		//
		// Both scales multiply by exactly 1.0f by default, which IEEE-754 guarantees is exact,
		// and TrimSaturation is an exact identity at 1.0, so the interior and skylighting
		// branches below reduce bit-for-bit to their pre-B7 form on a default install.
		float3 color = 0;
		if (SharedData::InInterior)
		{
			// Interiors never see the sky probe, so only the env knobs can apply here.
			color = TrimSaturation(GetDiffuseIBL(rayDir), SharedData::iblSettings.EnvIBLSaturation) * SharedData::iblSettings.EnvIBLScale;
		}
		else
// Must match the signature guard above exactly: with SKYLIGHTING and INTERIOR both defined the
// overload without the `skylighting` parameter is the one declared, so this branch would
// reference an undeclared identifier and fail to compile.
#if defined(SKYLIGHTING) && !defined(INTERIOR)
		{
			// Subtract-and-add, not lerp. The `skylighting` weight that arrives here is not a raw
			// visibility: every caller scales it by up to 1.9 (DeferredCompositeCS.hlsl:257,
			// ssrt_raymarch.hlsl:1109) before Skylighting::mixDiffuse, so any raw visibility past
			// ~0.53 already saturates it to 1. Under a lerp that multiplied the environment probe
			// by zero on essentially every upward-facing outdoor surface: no bounce colour from
			// the surroundings at all, and shadows tinted pure sky blue. Splitting the two probes
			// keeps the geometry bounce at full strength unconditionally and attenuates only the
			// sky part - the same form ssrt_raymarch.hlsl:1115-1120 already applies to this very
			// cubemap pair (DiffuseIBLTexture <- envTexture, whose sky is rejected by
			// UpdateCubemapCS.hlsl:85's `depth != 1.0`; DiffuseSkyIBLTexture <-
			// envReflectionsTexture, which keeps the sky).
			//
			// The subtraction is done on the evaluated RGB rather than on the sh2 coefficients.
			// Coefficient-domain subtraction is exact only for a linear evaluator, and
			// SphericalHarmonics::SHHallucinateZH3Irradiance is not linear: it normalizes the L1
			// band into a zonal axis and squares the L1/L0 ratio (SphericalHarmonics.hlsli:233-239),
			// so eval(shSky - shEnv) != eval(shSky) - eval(shEnv). Subtracting coefficients would
			// therefore also break the skylighting == 1 endpoint, which must still reproduce the
			// sky probe exactly as the old lerp did. RGB is additionally the domain where the
			// max(..., 0) clamp actually means something: a negative SH coefficient is a perfectly
			// legal signal, a negative irradiance is not.
			//
			// (B7) The subtraction stays on the *raw* probes. Trimming the environment term first
			// and subtracting that would break both ends of the knob: skylighting == 1 would no
			// longer reproduce the sky probe (the whole point of the subtract-and-add form), and
			// EnvIBLScale would leak into the sky term with the wrong sign - turning the
			// environment down would silently turn the sky up by the same amount. So: subtract
			// raw, then trim each half independently.
			float3 envRaw = GetDiffuseIBL(rayDir);
			float3 skyRaw = max(GetSkyDiffuseIBL(rayDir) - envRaw, 0);
			float3 envTerm = TrimSaturation(envRaw, SharedData::iblSettings.EnvIBLSaturation) * SharedData::iblSettings.EnvIBLScale;
			float3 skyTerm = TrimSaturation(skyRaw, SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
			color = envTerm + skyTerm * skylighting;
		}
#else
		{
			// (B7) Same split at an implied visibility of 1 rather than the bare sky probe, so the
			// four knobs also reach the paths that have no visibility to hand: the interior
			// composite's specular tap (DeferredCompositeCS.hlsl:503) and, more importantly, every
			// path at all once Skylighting is switched off. Note this is the one
			// branch that is *not* a bit-exact no-op at the defaults, and deliberately so: it used
			// to return the sky probe directly, and envRaw + max(skyRaw - envRaw, 0) equals that
			// only where the sky probe is the brighter of the two per channel. Where the
			// environment probe is brighter the max clamps and the environment bounce wins, which
			// is exactly the behaviour the SKYLIGHTING branch already has at skylighting == 1 -
			// the two overloads now agree instead of disagreeing.
			float3 envRaw = GetDiffuseIBL(rayDir);
			float3 skyRaw = max(GetSkyDiffuseIBL(rayDir) - envRaw, 0);
			float3 envTerm = TrimSaturation(envRaw, SharedData::iblSettings.EnvIBLSaturation) * SharedData::iblSettings.EnvIBLScale;
			float3 skyTerm = TrimSaturation(skyRaw, SharedData::iblSettings.SkyIBLSaturation) * SharedData::iblSettings.SkyIBLScale;
			color = envTerm + skyTerm;
		}
#endif
		return color;
	}

#if defined(LIGHTING)
	float3 GetStaticDiffuseIBL(float3 N, SamplerState samp)
	{
		return StaticDiffuseIBLTexture.SampleLevel(samp, N.xzy, 0).xyz / Math::PI;
	}
#endif

	float3 GetFogIBLColor(float3 fogColor)
	{
		float3 directionalAmbientColor = max(0, mul(SharedData::DirectionalAmbient, float4(float3(0, 0, 0), 1.0))).xyz;
		float3 iblColor = directionalAmbientColor * SharedData::iblSettings.DALCAmount + Color::Saturation(GetSkyDiffuseIBL(float3(0, 0, 0)), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
		if (SharedData::iblSettings.PreserveFogLuminance) {
			const float fogLuminance = Color::RGBToLuminance(fogColor);
			const float iblLuminance = Color::RGBToLuminance(iblColor);
			if (iblLuminance > 0) {
				const float scale = fogLuminance / iblLuminance;
				iblColor *= scale;
			} else {
				iblColor = fogColor;
			}
		}
		return lerp(fogColor, iblColor, SharedData::iblSettings.FogAmount);
	}
}

#endif // __IBL_HLSLI__