#ifndef __COMMON_HORIZON_FIX_HLSLI__
#define __COMMON_HORIZON_FIX_HLSLI__

// (47f) Physical Sky > Sky Fixes > "Horizon Fix". PhysicalSky::Reset() fills SharedData::horizonFixData every frame; all 0
// while the fix is off (or the physical sky is not drawn here), and every function below is then the identity / unused.
// The planet is the physical sky's: a sphere of radius physSkyData.rPlanet whose top is the worldspace's ground sphere
// (zBottom, 500 units below its water), the same sphere LutGen.cs.hlsl's "distant sea" reflects in.

#include "Common/SharedData.hlsli"

namespace HorizonFix
{
	bool Active()
	{
		return (SharedData::horizonFixData.Flags & 1u) != 0;
	}

	/// The camera's height over the ground sphere (game units).
	float Altitude()
	{
		return max(SharedData::horizonFixData.Altitude, 0.0f);
	}

	/// z of a unit direction along the planet's horizon (-sin(dip)); 0 when inactive.
	float HorizonZ()
	{
		const float R = max(SharedData::physSkyData.rPlanet, 1.0f);
		const float h = Altitude();
		// sin(dip) = sqrt(h (2R + h)) / (R + h), without forming (R + h)^2 - R^2 in fp32.
		return -sqrt(h * (2.0f * R + h)) / (R + h);
	}

	/// How far a sky pixel's fog runs along a unit direction: below the planet horizon to the curved sea (the ground
	/// sphere), above it at least the distance to the horizon (and never less than a_farDistance, the far plane's).
	float SkyDistance(float3 a_direction, float a_farDistance)
	{
		const float R = max(SharedData::physSkyData.rPlanet, 1.0f);
		const float h = Altitude();
		const float b = (R + h) * a_direction.z;
		const float c = h * (2.0f * R + h);
		const float disc = b * b - c;
		float distance = max(a_farDistance, sqrt(c));
		[flatten] if (a_direction.z < 0.0f && disc >= 0.0f)
			distance = max(-b - sqrt(disc), 0.0f);
		return distance;
	}

	/// A unit direction no lower than the planet horizon (for the fog colour's cubemap read: below it the cubemap holds
	/// dark ground, which showed as blotches in the fog). Unchanged while inactive.
	float3 ClampToHorizon(float3 a_direction)
	{
		[branch] if (Active())
		{
			const float z = HorizonZ();
			[branch] if (a_direction.z < z)
			{
				const float horizontal = length(a_direction.xy);
				const float2 heading = horizontal > 1e-6f ? a_direction.xy / horizontal : float2(1.0f, 0.0f);
				a_direction = float3(heading * sqrt(saturate(1.0f - z * z)), z);
			}
		}
		return a_direction;
	}

	/// The weather fog layer (WeatherFog.hlsli) keeps thickening below the camera down to sea level: the camera's height
	/// over sea level (game units). 0 while inactive (the 47e layer: even below the camera).
	float AnchorDepth()
	{
		return max(SharedData::horizonFixData.AnchorDepth, 0.0f);
	}

	/// Distance to the game's cloud deck (a_deckHeight above the camera, on a sphere around the planet's centre, the
	/// camera at its real altitude) along a unit direction: rays down to the real horizon reach it far away.
	float CloudDistance(float3 a_direction, float a_deckHeight)
	{
		const float r = max(SharedData::physSkyData.rPlanet, 1.0f) + Altitude();
		const float rc = r + max(a_deckHeight, 1.0f);
		const float b = r * a_direction.z;
		const float c = (r - rc) * (r + rc);  // < 0: inside the deck's sphere
		return -b + sqrt(max(b * b - c, 0.0f));
	}
}

#endif
