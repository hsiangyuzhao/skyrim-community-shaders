#ifndef __DYNAMIC_SNOW_DEPENDENCY_HLSL__
#define __DYNAMIC_SNOW_DEPENDENCY_HLSL__

// (batch 39, items 5-6) Dynamic snow: accumulated snow on upward-facing surfaces, and the
// footprint / trail map around the player. Both are applied in Lighting.hlsl, after the
// material's own snow (landscape snow textures, directional snow projection, snow-flagged
// materials) is known and before anything reads the albedo, normal or roughness, so the
// G-buffer -- and with it SSRT, NRD and the deferred composite -- sees one consistent material.

#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"

namespace DynamicSnow
{
#if defined(PSHADER)
	// R32_UINT, toroidally addressed: hi 16 bits = print strength (UNORM), lo 16 bits = the
	// foot's height modulo kTrailZWrap (UNORM). Written by SnowTrailsCS.hlsl.
	Texture2D<uint> SnowTrailMap : register(t101);
#endif

	// Mirrors DynamicSnow::Flag in src/Features/DynamicSnow.h.
	static const uint FlagAccumulation = 1 << 0;      // accumulated snow is drawn this frame
	static const uint FlagTrails = 1 << 1;            // trail map is valid this frame
	static const uint FlagTrailsOnSnow = 1 << 2;      // prints on landscape snow and snow materials
	static const uint FlagTrailsOnAccumulated = 1 << 3;  // prints on accumulated snow
	static const uint FlagMudTrails = 1 << 4;         // prints on non-snow terrain
	// (batch 39b)
	static const uint FlagSmoothTrails = 1 << 5;        // bicubic trail reconstruction (39a: bilinear)
	static const uint FlagLandSnowDetect = 1 << 6;      // authored snow recognised from the land texture / projected snow
	static const uint FlagAlbedoSnowGuess = 1 << 7;     // bright, grey-white ground counts as snow too
	static const uint FlagSnowOnCharacters = 1 << 8;    // accumulated snow on actors and what they wear

	// Height is stored modulo this many units. Two surfaces exactly a multiple of it apart
	// (14.6 m) would share prints; anything else is told apart by TrailZTolerance.
	static const float kTrailZWrap = 1024.0;

	float Hash(uint2 p)
	{
		return float(Random::iqint3(p)) * (1.0 / 4294967295.0);
	}

	// Smooth 2D value noise, [0,1]. World-space, so terrain and the objects standing on it
	// (Terrain Blending) get the same pattern and blend without a seam.
	float ValueNoise(float2 p)
	{
		float2 i = floor(p);
		float2 f = p - i;
		uint2 c = asuint(int2(i));
		float a = Hash(c);
		float b = Hash(c + uint2(1, 0));
		float d = Hash(c + uint2(0, 1));
		float e = Hash(c + uint2(1, 1));
		float2 u = f * f * (3.0 - 2.0 * f);
		return lerp(lerp(a, b, u.x), lerp(d, e, u.x), u.y);
	}

	/**
	 * Accumulated snow coverage [0,1] at a surface point.
	 *
	 * @param positionWS absolute world position (WorldPosition + CameraPosAdjust)
	 * @param normalWS shading normal (normal map applied)
	 * @param geometryNormalWS interpolated vertex normal
	 * @param skyVisibility [0,1] how open the sky is straight above (0 = under a roof)
	 * @param viewDistance distance to the camera, fades the fine noise octave out
	 */
	float GetCoverage(float3 positionWS, float3 normalWS, float3 geometryNormalWS, float skyVisibility, float viewDistance)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;

		// Half geometry, half normal map: crevices in the normal map that face up hold snow,
		// but a steep wall with a bumpy normal map does not.
		float up = lerp(geometryNormalWS.z, normalWS.z, 0.5);
		float slope = saturate((up - s.NormalThreshold) / max(1.0 - s.NormalThreshold, 1e-3));
		[branch] if (slope <= 0.0) return 0.0;  // walls and overhangs: no noise, no sky lookup

		float2 p = positionWS.xy;
		float fine = lerp(0.5, ValueNoise(p * (1.0 / 37.0)), saturate(1.0 - viewDistance / 6144.0));
		float noise = ValueNoise(p * (1.0 / 300.0)) * 0.65 + fine * 0.35;

		// Flat ground and noise peaks fill first, slopes last. Amount 0 covers nothing (the
		// threshold sits above the largest possible h), amount 1 covers every surface above
		// the normal threshold.
		float h = slope * 0.7 + noise * 0.3;
		float t = 1.1 - s.Amount * 1.2;
		float coverage = smoothstep(t - 0.1, t + 0.1, h) * saturate(slope * 3.0);

		return coverage * s.MaxCoverage * skyVisibility;
	}

	// (batch 39b) Last-resort snow test from the colour alone (FlagAlbedoSnowGuess): bright and
	// close to grey-white. Only used on terrain and on the projected (directional) layer of
	// rocks and mountains, where white means snow far more often than not.
	float AlbedoSnowGuess(float3 color)
	{
		float mx = max(color.r, max(color.g, color.b));
		float mn = min(color.r, min(color.g, color.b));
		float lum = dot(color, float3(0.2126, 0.7152, 0.0722));
		float sat = (mx - mn) / max(mx, 1e-3);
		return smoothstep(0.35, 0.55, lum) * (1.0 - smoothstep(0.12, 0.3, sat));
	}

	struct TrailSample
	{
		float pressed;   // [0,1] print strength
		float2 gradient; // d(pressed)/d(world xy), per game unit
		bool any;        // any of the four texels carries a print
	};

	uint LoadTrailTexel(int2 windowTexel)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		uint2 p = uint2(windowTexel + s.TrailWrap) & (s.TrailMapSize - 1);
#if defined(PSHADER)
		return SnowTrailMap.Load(int3(p, 0));
#else
		return 0;
#endif
	}

	// Strength of one texel's print as seen from a surface at height z: zero unless the foot
	// that made it stood within TrailZTolerance of this surface (bridges, roofs, caves above
	// or below a trail do not receive it).
	float DecodeTrailTexel(uint v, float z)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float strength = float(v >> 16) * (1.0 / 65535.0);
		float footZ = float(v & 0xFFFF) * (kTrailZWrap / 65535.0);
		float dz = (frac((z - footZ) / kTrailZWrap + 0.5) - 0.5) * kTrailZWrap;
		return strength * (1.0 - smoothstep(s.TrailZTolerance * 0.5, s.TrailZTolerance, abs(dz)));
	}

	TrailSample SampleTrailBilinear(float2 positionXY, float z)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		TrailSample o;
		o.pressed = 0;
		o.gradient = 0;
		o.any = false;

		float2 t = (positionXY - s.TrailOrigin) / s.TrailTexelSize - 0.5;
		float size = float(s.TrailMapSize);
		// Fade out towards the window edge: the strip that scrolls in is cleared, so prints
		// there would pop.
		float2 edge = min(t + 0.5, size - (t + 0.5));
		float fade = saturate((min(edge.x, edge.y) - 1.0) / max(s.TrailEdgeFade, 1.0));
		[branch] if (fade <= 0.0) return o;

		int2 i = int2(floor(t));
		float2 f = t - floor(t);
		uint r00 = LoadTrailTexel(i);
		uint r10 = LoadTrailTexel(i + int2(1, 0));
		uint r01 = LoadTrailTexel(i + int2(0, 1));
		uint r11 = LoadTrailTexel(i + int2(1, 1));
		o.any = (r00 | r10 | r01 | r11) >= 0x10000;
		[branch] if (!o.any) return o;

		float v00 = DecodeTrailTexel(r00, z);
		float v10 = DecodeTrailTexel(r10, z);
		float v01 = DecodeTrailTexel(r01, z);
		float v11 = DecodeTrailTexel(r11, z);

		o.pressed = lerp(lerp(v00, v10, f.x), lerp(v01, v11, f.x), f.y) * fade;
		o.gradient = float2(lerp(v10 - v00, v11 - v01, f.y), lerp(v01 - v00, v11 - v10, f.x)) * (fade / s.TrailTexelSize);
		return o;
	}

	// (batch 39b) Smooth reconstruction of the trail map. 39a read it bilinearly and took the
	// slope from the four texels' differences, which is constant across each texel: the
	// print's normal jumped at every texel edge and the print looked like a mosaic. Now a
	// uniform cubic B-spline over the 4x4 texels around the point: the strength and its
	// slope are both continuous (C2 / C1), so the normal derived from it varies smoothly and
	// no texel grid shows, at any DLSS jitter offset. The idea of a B-spline-filtered field
	// for the print normal follows community-shaders PR #2659 (PppPlyr1, "Snow
	// Deformation"); that PR samples a filterable float map with 4 bilinear taps, here the
	// map stays R32_UINT (strength + foot height, for the per-surface height test) so the
	// 16 texels are loaded and decoded one by one.
	TrailSample SampleTrailBicubic(float2 positionXY, float z)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		TrailSample o;
		o.pressed = 0;
		o.gradient = 0;
		o.any = false;

		float2 t = (positionXY - s.TrailOrigin) / s.TrailTexelSize - 0.5;
		float size = float(s.TrailMapSize);
		float2 edge = min(t + 0.5, size - (t + 0.5));
		float fade = saturate((min(edge.x, edge.y) - 2.0) / max(s.TrailEdgeFade, 1.0));
		[branch] if (fade <= 0.0) return o;

		float2 tf = floor(t);
		float2 f = t - tf;
		int2 i0 = int2(tf) - 1;

		uint raw[16];
		uint anyBits = 0;
		[unroll] for (int ly = 0; ly < 4; ++ly)
		{
			[unroll] for (int lx = 0; lx < 4; ++lx)
			{
				uint v = LoadTrailTexel(i0 + int2(lx, ly));
				raw[ly * 4 + lx] = v;
				anyBits |= v;
			}
		}
		o.any = anyBits >= 0x10000;
		[branch] if (!o.any) return o;

		// Cubic B-spline weights and their derivatives (per texel).
		float2 f2 = f * f;
		float2 f3 = f2 * f;
		float2 g = 1.0 - f;
		float2 w[4] = { g * g * g / 6.0, (3.0 * f3 - 6.0 * f2 + 4.0) / 6.0, (-3.0 * f3 + 3.0 * f2 + 3.0 * f + 1.0) / 6.0, f3 / 6.0 };
		float2 dw[4] = { -0.5 * g * g, 1.5 * f2 - 2.0 * f, -1.5 * f2 + f + 0.5, 0.5 * f2 };

		float value = 0.0;
		float2 grad = 0.0;
		[unroll] for (int y = 0; y < 4; ++y)
		{
			float row = 0.0;
			float rowD = 0.0;
			[unroll] for (int x = 0; x < 4; ++x)
			{
				float v = DecodeTrailTexel(raw[y * 4 + x], z);
				row += w[x].x * v;
				rowD += dw[x].x * v;
			}
			value += w[y].y * row;
			grad.x += w[y].y * rowD;
			grad.y += dw[y].y * row;
		}

		o.pressed = saturate(value) * fade;
		o.gradient = grad * (fade / s.TrailTexelSize);
		return o;
	}

	/**
	 * Print strength at a surface point, with one step of parallax so the print reads as a
	 * dent rather than a decal: the lookup is shifted along the view ray by the print's depth.
	 *
	 * @param positionWS absolute world position
	 * @param viewDirectionWS surface -> camera, normalised
	 */
	TrailSample SampleTrailAt(float2 positionXY, float z)
	{
		[branch] if (SharedData::dynamicSnowSettings.Flags & FlagSmoothTrails)
			return SampleTrailBicubic(positionXY, z);
		return SampleTrailBilinear(positionXY, z);
	}

	TrailSample SampleTrail(float3 positionWS, float3 viewDirectionWS)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		TrailSample o = SampleTrailAt(positionWS.xy, positionWS.z);
		[branch] if (o.any && o.pressed > 1e-3)
		{
			float h = o.pressed * s.TrailDepth;
			float2 offset = -viewDirectionWS.xy * (h / max(viewDirectionWS.z, 0.3));
			TrailSample p = SampleTrailAt(positionWS.xy + offset, positionWS.z);
			if (p.any)
				o = p;
		}
		return o;
	}

	// Normal of the dented surface: the print flattens the snow's own bumps and its rim slopes
	// follow the print's gradient.
	float3 ApplyTrailNormal(float3 normalWS, float3 geometryNormalWS, TrailSample t, float weight)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float3 n = normalize(lerp(normalWS, geometryNormalWS, saturate(t.pressed * weight) * 0.5));
		n.xy += t.gradient * (s.TrailDepth * weight);
		return normalize(n);
	}
}

#endif  // __DYNAMIC_SNOW_DEPENDENCY_HLSL__
