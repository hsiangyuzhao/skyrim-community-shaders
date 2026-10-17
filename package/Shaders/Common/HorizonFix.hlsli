#ifndef __COMMON_HORIZON_FIX_HLSLI__
#define __COMMON_HORIZON_FIX_HLSLI__

// (47f) Physical Sky > Sky Fixes > "Horizon Fix". PhysicalSky::Reset() fills SharedData::horizonFixData every frame; Flags,
// Altitude and AnchorOffset are 0 while the fix is off (or the physical sky is not drawn here), and every function below is
// then the identity / unused. The planet is the physical sky's: a sphere of radius physSkyData.rPlanet whose top is the
// worldspace's ground sphere (zBottom, 500 units below its water), the same sphere LutGen.cs.hlsl's "distant sea" reflects in.
// (47g) Redone: a sky pixel's fog follows the ray's real, curved path (SkyLayerIntegral): rays between 0 degrees and the
// planet horizon pass above the sea and leave the air, rays below it end on the sea, whose reflection carries the fog of the
// reflected ray; the layers keep their own world-height profiles. No camera-relative thickening any more.

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

	/// (47g) The weather fog layer (WeatherFog.hlsli) is anchored to world height: its base (density 1 at and below it) is the
	/// camera's height while the camera is below the reference height max(EHF Fog Height, sea level) + its scale height, and
	/// that reference above it. This is how far the camera is above the base (game units); 0 while inactive (the 47e layer).
	float AnchorOffset()
	{
		return SharedData::horizonFixData.AnchorOffset;  // >= 0 (PhysicalSky::Reset), no max(): register pressure
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

	// ---- (47g) Curved-ray integrals of a height layer, for sky pixels ------------------------------------------------
	//
	// A layer has density 1 at and below its base altitude aB (over the ground sphere) and exp(-(a - aB) / H) above it. Along
	// the straight line of a view ray, the altitude a and the distance from the line's point closest to the planet centre p
	// obey (R + a)^2 = (R + aMin)^2 + p^2, aMin the altitude of that closest point. With a = aMin + w^2 the integral over the
	// ray becomes J x the integral of g(aMin + w^2) dw, J = 2 (R + a) / sqrt(2R + a + aMin) (constant to 1e-4 over a layer,
	// taken at the camera): a flat part and a Gaussian tail (erfcx). The same w runs down to the closest point and up again,
	// so the result is continuous across 0 degrees, and the ray's lowest altitude is the real one (no flat-earth descent).

	/// erfcx(x) = exp(x^2) erfc(x) for x >= 0 (Boerjesson-Sundberg form: relative error below 0.3 %, smooth, exact at 0 and
	/// at infinity).
	float Erfcx(float x)
	{
		return 0.5641896f / (0.661f * x + 0.339f * sqrt(x * x + 2.755f));
	}

	/// Integral, over the part of the line from altitude a0 to altitude a1 on one side of its lowest point (a_toInfinity: a1 =
	/// infinity), of g along w: a flat part from the altitude differences (no fp32 cancellation of large w) and the tail.
	float LayerIntegralA(float a0, float a1, bool a_toInfinity, float aMin, float aB, float H)
	{
		const float w0 = sqrt(max(a0 - aMin, 0.0f));
		float flat = 0.0f;
		[flatten] if (a0 < aB)
		{
			const float aTop = a_toInfinity ? aB : min(a1, aB);
			flat = max(aTop - a0, 0.0f) / max(sqrt(max(aTop - aMin, 0.0f)) + w0, 1e-6f);  // w(aTop) - w0
		}
		const float aT0 = max(a0, aB);
		float tail = exp((aB - aT0) / H) * Erfcx(sqrt(max(aT0 - aMin, 0.0f) / H));
		[flatten] if (!a_toInfinity)
		{
			const float aT1 = max(a1, aT0);
			tail -= exp((aB - aT1) / H) * Erfcx(sqrt(max(aT1 - aMin, 0.0f) / H));
		}
		return flat + sqrt(H) * 0.8862269f * max(tail, 0.0f);
	}

	struct SkyRay
	{
		float aMin;     // altitude of the line's point closest to the planet centre (< 0: the ray ends on the sea)
		float J;        // dt / dw
		float h;        // the camera's altitude
		uint kind;      // 0 = going up, 1 = down past its lowest point and up again (above the planet horizon), 2 = down to the sea
		float fresnel;  // the sea's reflectance at the hit (Schlick, F0 0.02, as LutGen's distant sea); 1 otherwise
	};

	SkyRay MakeSkyRay(float a_directionZ)
	{
		SkyRay ray;
		const float R = max(SharedData::physSkyData.rPlanet, 1.0f);
		const float h = Altitude();
		const float mu = clamp(a_directionZ, -1.0f, 1.0f);
		const float s = sqrt(saturate(1.0f - mu * mu));
		ray.aMin = h * s - R * mu * mu / (1.0f + s);  // (R + h) sin(zenith) - R, without the fp32 cancellation
		ray.J = 2.0f * (R + h) * rsqrt(max(2.0f * R + h + ray.aMin, 1.0f));
		ray.h = h;
		ray.kind = mu >= 0.0f ? 0u : (ray.aMin >= 0.0f ? 1u : 2u);
		// cos of the incidence at the sea point: sqrt(1 - ((R + aMin) / R)^2)
		const float cosIncidence = sqrt(max(-ray.aMin, 0.0f) * max(2.0f * R + ray.aMin, 0.0f)) / R;
		ray.fresnel = ray.kind == 2u ? 0.02f + 0.98f * pow(1.0f - saturate(cosIncidence), 5.0f) : 1.0f;
		return ray;
	}

	/// The integral of a layer (base altitude a_base over the ground sphere, scale height a_scale; game units) along a sky
	/// ray from the camera: above the planet horizon out of the air (to infinity), below it to the sea, plus the sea's
	/// reflectance x the reflected ray's way out (the sea mirrors the sky through the fog on that side). At the planet horizon
	/// the reflectance is 1 and the reflected ray is the far half of the ray just above it: no seam there.
	float SkyLayerIntegral(SkyRay a_ray, float a_base, float a_scale)
	{
		const float H = clamp(a_scale, 1.0f, 1e7f);
		float I;
		[flatten] if (a_ray.kind == 0u)
			I = LayerIntegralA(a_ray.h, 0.0f, true, a_ray.aMin, a_base, H);
		else if (a_ray.kind == 1u)
			I = LayerIntegralA(a_ray.aMin, a_ray.h, false, a_ray.aMin, a_base, H) + LayerIntegralA(a_ray.aMin, 0.0f, true, a_ray.aMin, a_base, H);
		else
			I = LayerIntegralA(0.0f, a_ray.h, false, a_ray.aMin, a_base, H) + a_ray.fresnel * LayerIntegralA(0.0f, 0.0f, true, a_ray.aMin, a_base, H);
		return a_ray.J * I;
	}
}

#endif
