#ifndef __EXPONENTIAL_HEIGHT_FOG_WEATHER_FOG_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_WEATHER_FOG_HLSLI__

// (47d) "EHF Follows Weather" = "CS Fog Takes Over": the weather's own fog becomes a layer of the height fog instead of
// the game's flat fog. ExponentialHeightFog::GetWeatherFogData() fills SharedData::weatherFogData every frame from the
// fog the engine has blended for this moment (Sky::fogNear / fogFar / fogPower / fogClamp), the 47b weather weight w
// and Linear Lighting's fog transparency gamma; VolumetricFogSettings.pad0.y carries kFlagTakeover only while the layer
// applies (outdoors, full sky, w > 0). Generic: no weather or mod is read by name.
//  - opacity along a camera ray: Cap (1 - exp(-tau)), with Cap = w x the weather's on-screen max opacity and
//    tau = Sigma x the integral of the density from the weather's fog near distance on. Sigma is fitted so that the
//    on-screen half-cover distance equals the game's;
//  - density: 1 at and below the layer's base, exp(-h / ScaleHeight) above it. The base is the camera's height (47d), or
//    (47g, Horizon Fix) a world height the camera can rise above (HorizonFix::AnchorOffset());
//  - light toward the sun through the layer has the same form, so sunlight never gets darker than the farthest
//    mountain shows through the fog (1 - Cap).
// Only SharedData is needed: Lighting, Effect, Water, RunGrass, DistantTree and the volumetric fog passes include it.

#include "Common/HorizonFix.hlsli"  // (47f)
#include "Common/SharedData.hlsli"

namespace WeatherFog
{
	static const uint kFlagTakeover = 2u;  // VolumetricFogSettings.pad0.y bit (bit 0 = 47b's opacity fix)

	/// The layer applies this frame (exterior, "CS Fog Takes Over", weather fog weight > 0).
	bool Active()
	{
		return (asuint(SharedData::volumetricFogSettings.pad0.y) & kFlagTakeover) != 0;
	}

	float ScaleHeight()
	{
		return max(SharedData::weatherFogData.ScaleHeight, 1.0f);
	}

	/// (47g) How far the camera is above the layer's base (Horizon Fix: the base is anchored to world height); 0 = the base
	/// is the camera's height, the 47e layer exactly.
	float Offset()
	{
		return HorizonFix::AnchorOffset();
	}

	/// Relative density at a height over the camera (1 at and below the base).
	float Density(float a_height)
	{
		return exp(-max(a_height + Offset(), 0.0f) / ScaleHeight());
	}

	/// Integral of the relative density over the heights [a_low, a_high] (a_low <= a_high, over the camera).
	float DensityIntegral(float a_low, float a_high)
	{
		const float H = ScaleHeight();
		const float o = Offset();  // (47g) 0 = the 47e form exactly
		const float below = min(a_high, -o) - min(a_low, -o);
		const float above = H * (exp(-max(a_low + o, 0.0f) / H) - exp(-max(a_high + o, 0.0f) / H));
		return below + above;
	}

	/// Integral of the relative density along a camera ray (unit direction) from distance a_start to a_end.
	float RayDensityIntegral(float3 a_direction, float a_start, float a_end)
	{
		const float len = max(a_end - a_start, 0.0f);
		const float h0 = a_direction.z * a_start;
		const float h1 = a_direction.z * a_end;
		const float dh = abs(h1 - h0);
		// (Nearly) level ray: the midpoint density, exact to (dh / H)^2.
		const float average = dh > 0.01f * ScaleHeight() ? DensityIntegral(min(h0, h1), max(h0, h1)) / max(dh, 1e-6f) : Density(0.5f * (h0 + h1));
		return len * average;
	}

	/// Transmittance of the layer along a camera ray up to distance a_distance: 1 - Cap (1 - exp(-tau)); 1 within the
	/// weather's fog near distance. (47g) a_curvedSky (Horizon Fix, sky pixels): along the ray's curved path out of the air,
	/// or to the sea and its reflection (HorizonFix::SkyLayerIntegral); a_distance is then not used.
	float Transmittance(float3 a_direction, float a_distance, bool a_curvedSky = false)
	{
		const float nearDistance = max(SharedData::weatherFogData.NearDistance, 0.0f);
		float tau;
		[branch] if (a_curvedSky)
		{
			const HorizonFix::SkyRay ray = HorizonFix::MakeSkyRay(a_direction.z);
			const float base = HorizonFix::Altitude() - Offset();
			tau = SharedData::weatherFogData.Sigma * max(HorizonFix::SkyLayerIntegral(ray, base, ScaleHeight()) - RayDensityIntegral(a_direction, 0.0f, nearDistance), 0.0f);
		}
		else
			tau = SharedData::weatherFogData.Sigma * RayDensityIntegral(a_direction, nearDistance, max(a_distance, nearDistance));
		return 1.0f - saturate(SharedData::weatherFogData.Cap) * (1.0f - exp(-tau));
	}

	/// Opacity of the layer over the part of a camera ray (camera-relative a_positionWS) beyond a_startDistance (what lies
	/// in front of it is the volumetric fog's). a_fullOpacity = the layer's opacity over the whole ray.
	float Opacity(float3 a_positionWS, float a_startDistance, out float a_fullOpacity, bool a_curvedSky = false)
	{
		const float distance = length(a_positionWS);
		const float3 direction = a_positionWS / max(distance, 1e-4f);
		const float endTransmittance = Transmittance(direction, distance, a_curvedSky);
		a_fullOpacity = saturate(1.0f - endTransmittance);
		float startTransmittance = 1.0f;
		[branch] if (a_startDistance > 0.0f)
			startTransmittance = Transmittance(direction, min(a_startDistance, distance));
		return saturate(1.0f - endTransmittance / max(startTransmittance, 1e-4f));
	}

	/// Optical depth of the layer above a point a_height over the camera, straight up (Sigma x the density integral).
	float VerticalDepth(float a_height)
	{
		const float H = ScaleHeight();
		const float z = a_height + Offset();  // over the base
		return SharedData::weatherFogData.Sigma * (max(-z, 0.0f) + H * exp(-max(z, 0.0f) / H));
	}

	/// Transmittance of the layer from a point a_height over the camera toward the sun (to the top of the layer).
	float SunTransmittance(float a_height)
	{
		// Sun near the horizon: the path is held at ~3 degrees; it is long enough there to reach the cap anyway.
		const float mu = max(SharedData::DirLightDirection.z, 0.05f);
		return 1.0f - saturate(SharedData::weatherFogData.Cap) * (1.0f - exp(-VerticalDepth(a_height) / mu));
	}

	/// Direct sunlight on a surface a_height over the camera: dimmed by the layer between it and the sun.
	/// Exactly 1 while the layer does not apply (other modes, interiors, clear weather).
	float SunlightAttenuation(float a_height)
	{
		float attenuation = 1.0f;
		[branch] if (Active())
			attenuation = SunTransmittance(a_height);
		return attenuation;
	}

	/// Extinction of the layer at a camera-relative point for the volumetric fog, shaped so that a camera ray through
	/// the volume ends with exactly Transmittance(): the cap holds (d/ds of -ln(1 - Cap + Cap e^-tau)).
	float VolumetricExtinction(float3 a_positionWS)
	{
		float extinction = 0.0f;
		const float distance = length(a_positionWS);
		const float nearDistance = max(SharedData::weatherFogData.NearDistance, 0.0f);
		[branch] if (Active() && distance > nearDistance)
		{
			const float3 direction = a_positionWS / max(distance, 1e-4f);
			const float cap = saturate(SharedData::weatherFogData.Cap);
			const float sigma = SharedData::weatherFogData.Sigma;
			const float remaining = exp(-sigma * RayDensityIntegral(direction, nearDistance, distance));
			const float transmittance = 1.0f - cap + cap * remaining;
			extinction = max(cap * sigma * Density(a_positionWS.z) * remaining / max(transmittance, 1e-4f), 0.0f);
		}
		return extinction;
	}

	// ---- (47g) "Physical Fog Brightness" (Exponential Height Fog) ---------------------------------------------------
	//
	// The fog's brightness from what lights it, with the weather's fog colour giving only the hue: a deep fog of droplets
	// (single-scattering albedo about 1) shows the mean of the light coming down to it and the light coming back up from
	// below, (1 + ground albedo) / 2 x the downwelling light, in the units a white surface facing up would show (the game's
	// sun x cos, the sky as ambient). The downwelling light: the sun, and the physical sky's own light (unfogged, from
	// Physical Sky's model on the CPU: HorizonFixData.FogSkyLight, 0 = off / no physical sky), each through the weather layer
	// above the point with its scattered light kept (two-stream, conservative scattering, g = 0.85): 1 / (1 + 0.75 (1 - g)
	// tau / mu). Off (FogSkyLight 0): the weather's colour as before.

	float PhysicalSkyLight()
	{
		return SharedData::horizonFixData.FogSkyLight;
	}

	/// The directional light as surfaces receive it (linear; Linear Lighting's conversion, as the volumetric fog's
	/// GetDirectionalLightColor, without the TRUE_PBR compensation so every permutation agrees).
	float3 SunLightLinear()
	{
		const bool convert = SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear;
		const float mult = convert ? max(SharedData::linearLightingSettings.dirLightMult, 1e-5f) : 1.0f;
		float3 color = SharedData::DirLightColor.xyz / mult;
		[flatten] if (convert)
			color = pow(abs(color), SharedData::linearLightingSettings.lightGamma) * SharedData::linearLightingSettings.lightMult * SharedData::linearLightingSettings.directionalLightMult;
		return color * mult;
	}

	/// The fog's luminance at a point a_height over the camera.
	float PhysicalLuminance(float a_height)
	{
		const float k = 0.75f * (1.0f - 0.85f);
		const float tau = Active() ? VerticalDepth(a_height) : 0.0f;
		const float mu = saturate(SharedData::DirLightDirection.z);
		const float sun = dot(SunLightLinear(), float3(0.2126f, 0.7152f, 0.0722f)) * mu / (1.0f + k * tau / max(mu, 0.1f));
		const float sky = PhysicalSkyLight() / (1.0f + 2.0f * k * tau);  // diffuse light: mean cosine 0.5
		const float groundAlbedo = dot(SharedData::physSkyData.groundAlbedo, float3(0.2126f, 0.7152f, 0.0722f));
		return (sun + sky) * 0.5f * (1.0f + saturate(groundAlbedo));
	}

	/// The fog colour: the weather's hue (a_weatherColor with its luminance normalised) at the physical brightness; the
	/// colour unchanged while Physical Fog Brightness is off.
	float3 PhysicalFogColor(float3 a_weatherColor, float a_height)
	{
		float3 color = a_weatherColor;
		[branch] if (PhysicalSkyLight() > 0.0f)
		{
			const float y = dot(max(a_weatherColor, 0.0f), float3(0.2126f, 0.7152f, 0.0722f));
			const float3 hue = y > 1e-4f ? max(a_weatherColor, 0.0f) / y : float3(1.0f, 1.0f, 1.0f);
			color = hue * PhysicalLuminance(a_height);
		}
		return color;
	}
}

#endif
