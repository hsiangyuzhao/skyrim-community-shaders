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
//  - density: 1 at and below the camera's height, exp(-h / ScaleHeight) above it (h = height over the camera);
//  - light toward the sun through the layer has the same form, so sunlight never gets darker than the farthest
//    mountain shows through the fog (1 - Cap).
// Only SharedData is needed: Lighting, Effect, Water, RunGrass, DistantTree and the volumetric fog passes include it.

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

	/// Relative density at a height over the camera.
	float Density(float a_height)
	{
		return exp(-max(a_height, 0.0f) / ScaleHeight());
	}

	/// Integral of the relative density over the heights [a_low, a_high] (a_low <= a_high).
	float DensityIntegral(float a_low, float a_high)
	{
		const float H = ScaleHeight();
		const float below = min(a_high, 0.0f) - min(a_low, 0.0f);
		const float above = H * (exp(-max(a_low, 0.0f) / H) - exp(-max(a_high, 0.0f) / H));
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
	/// weather's fog near distance.
	float Transmittance(float3 a_direction, float a_distance)
	{
		const float nearDistance = max(SharedData::weatherFogData.NearDistance, 0.0f);
		const float tau = SharedData::weatherFogData.Sigma * RayDensityIntegral(a_direction, nearDistance, max(a_distance, nearDistance));
		return 1.0f - saturate(SharedData::weatherFogData.Cap) * (1.0f - exp(-tau));
	}

	/// Opacity of the layer over the part of a camera ray (camera-relative a_positionWS) beyond a_startDistance (what lies
	/// in front of it is the volumetric fog's). a_fullOpacity = the layer's opacity over the whole ray.
	float Opacity(float3 a_positionWS, float a_startDistance, out float a_fullOpacity)
	{
		const float distance = length(a_positionWS);
		const float3 direction = a_positionWS / max(distance, 1e-4f);
		const float endTransmittance = Transmittance(direction, distance);
		a_fullOpacity = saturate(1.0f - endTransmittance);
		float startTransmittance = 1.0f;
		[branch] if (a_startDistance > 0.0f)
			startTransmittance = Transmittance(direction, min(a_startDistance, distance));
		return saturate(1.0f - endTransmittance / max(startTransmittance, 1e-4f));
	}

	/// Transmittance of the layer from a point a_height over the camera toward the sun (to the top of the layer).
	float SunTransmittance(float a_height)
	{
		// Sun near the horizon: the path is held at ~3 degrees; it is long enough there to reach the cap anyway.
		const float mu = max(SharedData::DirLightDirection.z, 0.05f);
		const float H = ScaleHeight();
		const float depth = (max(-a_height, 0.0f) + H * exp(-max(a_height, 0.0f) / H)) / mu;
		return 1.0f - saturate(SharedData::weatherFogData.Cap) * (1.0f - exp(-SharedData::weatherFogData.Sigma * depth));
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
}

#endif
