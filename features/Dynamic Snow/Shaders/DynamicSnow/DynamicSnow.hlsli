#ifndef __DYNAMIC_SNOW_DEPENDENCY_HLSL__
#define __DYNAMIC_SNOW_DEPENDENCY_HLSL__

// (batch 39, items 5-6) Dynamic snow: accumulated snow on upward-facing surfaces, and the
// footprint / trail map around the player. Both are applied in Lighting.hlsl, after the
// material's own snow (landscape snow textures, directional snow projection, snow-flagged
// materials) is known and before anything reads the albedo, normal or roughness, so the
// G-buffer -- and with it SSRT, NRD and the deferred composite -- sees one consistent material.

#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "DynamicSnow/TrailTime.hlsli"

namespace DynamicSnow
{
#if defined(PSHADER)
	// R32_UINT, toroidally addressed: hi 16 bits = print code, lo 16 bits = the foot's height
	// modulo kTrailZWrap (UNORM). Written by SnowTrailsCS.hlsl. (batch 39c) Code 0 = untouched,
	// 1..32767 = a pushed-up rim (height = code / 32767), 32768..65535 = a dent (depth =
	// (code - 32768) / 32767): InterlockedMax keeps the deepest dent, and any dent beats a rim.
	Texture2D<uint> SnowTrailMap : register(t101);
#endif

	// Mirrors DynamicSnow::Flag in src/Features/DynamicSnow.h.
	static const uint FlagAccumulation = 1 << 0;      // accumulated snow is drawn this frame
	static const uint FlagTrails = 1 << 1;            // trail map is valid this frame
	static const uint FlagTrailsOnSnow = 1 << 2;      // prints on landscape snow and snow materials
	static const uint FlagTrailsOnAccumulated = 1 << 3;  // prints on accumulated snow
	static const uint FlagMudTrails = 1 << 4;         // prints on non-snow terrain
	// (batch 39b) 1 << 5, 6, 7 are free since 44f: bicubic prints and snowy-ground recognition are
	// permanent, the colour guess was removed.
	static const uint FlagSnowOnCharacters = 1 << 8;    // accumulated snow on actors and what they wear
	// (batch 39c)
	static const uint FlagCoverageSlope = 1 << 9;  // coverage from the geometry slope on a soft ramp (39b: normal threshold)
	static const uint FlagTrees = 1 << 10;         // snow on animated trees and foliage (Lighting, TREE_ANIM)
	static const uint FlagGrass = 1 << 11;         // snow on grass (RunGrass)
	static const uint FlagLodTrees = 1 << 12;      // snow on distant billboard trees (DistantTree)
	// (40e) 1 << 13 is free since 44f: rigid gear on actors always takes its slope from world up.
	// (40e) Permutation::ExtraShaderDescriptor bit, mirrors State::ExtraShaderDescriptors::IsActorRigid
	// (set with IsActorGeometry when the geometry is not skinned). Kept here rather than in
	// Permutation.hlsli, where a new constant reshuffles fxc's output for unrelated permutations.
	static const uint ExtraIsActorRigid = 1 << 14;
	// (44e) Flags bit: the trail map holds time-stamped texels (TrailTime.hlsli); the current tick is in pad0.
	static const uint FlagTrailTimeStamps = 1 << 14;
	uint TrailNowTick() { return uint(SharedData::dynamicSnowSettings.pad0); }
	// (44h) Accumulation only where the region is a cold one: the amount is scaled by the cold-region
	// mask (SnowColdMask, SharedData::dynamicSnowMapData).
	static const uint FlagColdMap = 1 << 15;
	// (44h) Distant LOD (LOD terrain, LOD objects, billboard trees) takes no accumulation, and the
	// full-detail geometry's amount thins out to nothing towards the edge of the loaded cells.
	static const uint FlagNoLodSnow = 1 << 16;
	// (44h) Debug: tint surfaces by the local amount scale (red = no snow here, blue = full).
	static const uint FlagShowColdMap = 1 << 17;
	// (46a) Characters take their own amount from Advanced Skin's b7 (Skin::snowPerGeometry).
	static const uint FlagCharacterSnowState = 1 << 18;

#if defined(PSHADER)
	// (44h) R8_UNORM over the worldspace, 1 = cold region. Bound by DynamicSnow::Prepass.
	Texture2D<float> SnowColdMask : register(t102);
#endif

	// (44h) Bilinear cold-region mask at a world position (manual, from four loads: no sampler slot needed).
	// Outside the mask the edge texels repeat; the mask has a margin, so they hold the "no region" value.
	// (44i) Above the snow line everything is cold: max(region mask, smoothstep(SnowLineStart, SnowLineEnd, z)).
	// The snow line off puts both far above any terrain, so it adds nothing.
	float ColdMask(float3 positionWS)
	{
#if defined(PSHADER)
		const SharedData::DynamicSnowMapData m = SharedData::dynamicSnowMapData;
		const float snowLine = smoothstep(m.SnowLineStart, m.SnowLineEnd, positionWS.z);
		float mask = 1.0;
		[branch] if (snowLine < 1.0)
		{
			float2 t = (positionWS.xy - m.MaskOrigin) * m.MaskInvTexelSize - 0.5;
			float2 tf = floor(t);
			float2 f = t - tf;
			int2 hi = int2(m.MaskSize) - 1;
			int2 i0 = clamp(int2(tf), 0, hi);
			int2 i1 = clamp(int2(tf) + 1, 0, hi);
			float a = SnowColdMask.Load(int3(i0.x, i0.y, 0));
			float b = SnowColdMask.Load(int3(i1.x, i0.y, 0));
			float c = SnowColdMask.Load(int3(i0.x, i1.y, 0));
			float d = SnowColdMask.Load(int3(i1.x, i1.y, 0));
			mask = lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
		}
		return max(mask, snowLine);
#else
		return 1.0;
#endif
	}

	/**
	 * (44h) Local scale [0,1] of the global accumulation amount at a world position (absolute).
	 * @param lod the surface is distant LOD (LOD terrain / objects / billboard trees)
	 * Neither FlagColdMap nor FlagNoLodSnow set: 1, the global amount everywhere (the 44g picture).
	 */
	float AmountScale(float3 positionWS, bool lod)
	{
		const uint flags = SharedData::dynamicSnowSettings.Flags;
		float scale = 1.0;
		[branch] if (flags & FlagNoLodSnow)
		{
			if (lod)
				return 0.0;
			const SharedData::DynamicSnowMapData m = SharedData::dynamicSnowMapData;
			float2 d = abs(positionWS.xy - m.LoadedCenter);
			scale = 1.0 - smoothstep(m.FadeStart, m.FadeEnd, max(d.x, d.y));
		}
		[branch] if ((flags & FlagColdMap) && scale > 0.0)
			scale *= ColdMask(positionWS);
		return scale;
	}

	// (44h) FlagShowColdMap: red where no snow can build, blue where it builds fully.
	float3 ColdMapDebugColor(float scale)
	{
		return lerp(float3(0.8, 0.08, 0.05), float3(0.05, 0.25, 0.9), saturate(scale));
	}

	// (40e) Mirrors CommonBufferData::SnowSmoothing / SteepCover (stored in pad1, see SharedData.hlsli).
	float SnowSmoothing() { return SharedData::dynamicSnowSettings.pad1.x; }
	float SteepCover() { return SharedData::dynamicSnowSettings.pad1.y; }

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
	float GetCoverage(float3 positionWS, float3 normalWS, float3 geometryNormalWS, float skyVisibility, float viewDistance, float amount)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		const bool slopeModel = (s.Flags & FlagCoverageSlope) != 0;

		float slope;
		[branch] if (slopeModel)
		{
			// (batch 39c) The slope comes from the geometry alone, on a soft ramp that starts well
			// below typical roof pitches. 39b gated on half geometry / half normal map against a
			// hard 0.6 (53 degrees) threshold with a 3x ramp: thatch roofs (vertex normals 0.45..0.85
			// on the PBR Unique Northern Vanilla Farmhouses inn, lumpy by design) straddled the
			// threshold, so the snow followed the thatch's lumps in streaks while flatter shingles
			// and boardwalks went fully white.
			slope = smoothstep(s.SlopeStart, s.SlopeFull, geometryNormalWS.z);
			// (40e) 39c gave every surface flatter than SlopeFull (57 degrees) full cover: roofs,
			// rock faces and wall tops all went solid white, far more than 39b (which covered
			// nothing steeper than 53 degrees and only partly up to 43). Roofs and slopes now hold
			// an even SteepCover; the cover rises to full only towards level ground (37 -> 23
			// degrees). The plateau spans nearly the whole thatch range, so the lumps stay even.
			// SteepCover 1 = 39c.
			slope *= lerp(SteepCover(), 1.0, smoothstep(0.80, 0.92, geometryNormalWS.z));
		} else {
			// 39b: half geometry, half normal map, hard threshold.
			float up = lerp(geometryNormalWS.z, normalWS.z, 0.5);
			slope = saturate((up - s.NormalThreshold) / max(1.0 - s.NormalThreshold, 1e-3));
		}
		[branch] if (slope <= 0.0) return 0.0;  // walls and overhangs: no noise, no sky lookup

		float2 p = positionWS.xy;
		float fine = lerp(0.5, ValueNoise(p * (1.0 / 37.0)), saturate(1.0 - viewDistance / 6144.0));
		float noise = ValueNoise(p * (1.0 / 300.0)) * 0.65 + fine * 0.35;

		// Flat ground and noise peaks fill first, slopes last. Amount 0 covers nothing (the
		// threshold sits above the largest possible h), amount 1 covers every surface above
		// the normal threshold.
		float h = slope * 0.7 + noise * 0.3;
		float t = 1.1 - amount * 1.2;
		float coverage = smoothstep(t - 0.1, t + 0.1, h);

		[branch] if (slopeModel)
		{
			coverage *= slope;
			// The normal map only shapes partial cover (up-facing detail fills first, the
			// grooves last); full cover stays full, so a bumpy texture never punches holes.
			float detail = normalWS.z - geometryNormalWS.z;
			coverage = saturate(coverage + detail * 2.0 * coverage * (1.0 - coverage));
			// (40e) At any cover, the normal map's grooves and steep facets hold less than its
			// up-facing tops, so the cobbles, planks and stones stay readable under the snow as
			// they did in 39b. SnowSmoothing 1 = 39c (thick snow ignores the texture).
			coverage *= saturate(1.0 + detail * 1.5 * (1.0 - SnowSmoothing()));
		} else {
			coverage *= saturate(slope * 3.0);
		}

		return coverage * s.MaxCoverage * skyVisibility;
	}

	// (40d) Characters, creatures and what they wear or carry (Lighting.hlsl, IsActorGeometry).
	// Everything is in the mesh's own space before skinning (Z up): positionMS is the vertex
	// position, normalMS the interpolated vertex normal (smooth over the mesh). So the cover stays
	// on the same patch of cloth while the limbs move, and follows the mesh's curvature instead of
	// its triangles. Softer than the ground: a gentle slope ramp, a wide noise ramp and a lower
	// ceiling, so a shoulder gets a dusting that fades out rather than a hard white patch.
	float GetActorCoverage(float3 positionMS, float3 normalMS, float viewDistance, float amount)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float slope = smoothstep(0.2, 0.95, normalMS.z);
		[branch] if (slope <= 0.0) return 0.0;

		// Model units (~1.4 cm). Coarse ~40 units (a hand's span), fine ~6 units; the height term
		// keeps stacked layers (hood over shoulders) from sharing one pattern.
		float2 p = positionMS.xy + positionMS.zz * float2(0.37, -0.61);
		float fine = lerp(0.5, ValueNoise(p * (1.0 / 6.0)), saturate(1.0 - viewDistance / 4096.0));
		float noise = ValueNoise(p * (1.0 / 40.0)) * 0.65 + fine * 0.35;

		float h = slope * 0.7 + noise * 0.3;
		float t = 1.1 - amount * 1.2;
		float coverage = smoothstep(t - 0.25, t + 0.25, h) * slope;
		return coverage * s.MaxCoverage * 0.8;
	}

	// (batch 39c) Animated trees and foliage (Lighting.hlsl, TREE_ANIM). The leaves sway, so
	// nothing here may follow the swaying world position closely: the slope comes from the
	// vertex normal (tree mods give leaf cards normals that point out of the canopy, so the top
	// of the crown faces up), the fine breakup is in texture space and moves with the leaf, and
	// the world-space noise is only the 300-unit octave, against which a sway of a few units is
	// invisible. No front/back-face term either: a swaying card flips sides against the camera.
	float GetTreeCoverage(float3 positionWS, float3 geometryNormalWS, float2 uv, float amount)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float slope = smoothstep(-0.1, 0.6, geometryNormalWS.z);
		[branch] if (slope <= 0.0) return 0.0;
		float noise = ValueNoise(positionWS.xy * (1.0 / 300.0)) * 0.6 + ValueNoise(uv * 6.0) * 0.4;
		float h = slope * 0.7 + noise * 0.3;
		float t = 1.1 - amount * 1.2;
		return smoothstep(t - 0.1, t + 0.1, h) * slope * s.TreeCoverage * s.MaxCoverage;
	}

	// (batch 39c) Grass (RunGrass.hlsl). Snow buries a blade from the root up as it builds (the
	// root is white with the ground around it), and dusts the up-facing upper part. tip = the
	// vertex's wind weight (0 at the root, 1 at the tip in Skyrim grass meshes): a per-vertex
	// constant, so the cover does not move with the wind.
	float GetGrassCoverage(float3 positionWS, float tip, float upZ, float amount)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float snowLine = amount * 0.7;
		float buried = 1.0 - smoothstep(snowLine - 0.15, snowLine + 0.05, tip);
		buried *= smoothstep(0.0, 0.15, amount);
		float dust = smoothstep(-0.6, 0.8, upZ) * smoothstep(0.0, 0.6, amount) * 0.6;
		float noise = ValueNoise(positionWS.xy * (1.0 / 300.0));
		return saturate(max(buried, dust) * lerp(0.75, 1.0, noise)) * s.GrassCoverage * s.MaxCoverage;
	}

	// (batch 39c) Distant billboard trees (DistantTree.hlsl): a tint that matches the average
	// of the near trees, whiter towards the top of the billboard. heightFraction is 0 at the
	// billboard's base and 1 at its top; the breakup is in atlas space.
	float GetLodTreeCoverage(float heightFraction, float2 uv, float amount)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float a = smoothstep(0.0, 1.0, amount);
		float noise = ValueNoise(uv * 48.0);
		return a * lerp(0.45, 1.0, heightFraction) * lerp(0.7, 1.0, noise) * s.LodTreeCoverage * s.MaxCoverage;
	}

	// (batch 39b) Snow test from the colour alone: bright and close to grey-white. Only used on the
	// projected (directional) layer of rocks and mountains, where white means snow far more often
	// than not.
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
		float pressed;   // [-1,1] print depth (negative = the pushed-up rim)
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

	// Signed depth of one texel's print (rim negative) as seen from a surface at height z: zero unless the foot
	// that made it stood within TrailZTolerance of this surface (bridges, roofs, caves above
	// or below a trail do not receive it).
	float DecodeTrailTexel(uint v, float z)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		float strength;
		float footZ;
		[branch] if (s.Flags & FlagTrailTimeStamps)
		{
			// (44e) Refill computed here from the stamp's age: no decay pass.
			bool dent;
			const float rem = TrailTime::Remaining(v, TrailNowTick(), dent);
			strength = dent ? rem : -rem * s.TrailRim;
			footZ = TrailTime::FootZ(v);
		}
		else
		{
			uint code = v >> 16;
			strength = code >= 32768 ? float(code - 32768) * (1.0 / 32767.0) : -float(code) * (s.TrailRim / 32767.0);
			footZ = float(v & 0xFFFF) * (kTrailZWrap / 65535.0);
		}
		float dz = (frac((z - footZ) / kTrailZWrap + 0.5) - 0.5) * kTrailZWrap;
		return strength * (1.0 - smoothstep(s.TrailZTolerance * 0.5, s.TrailZTolerance, abs(dz)));
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

		o.pressed = clamp(value, -1.0, 1.0) * fade;
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
		// (44f) Always bicubic (39b); the bilinear reading (39a's mosaic) is gone.
		return SampleTrailBicubic(positionXY, z);
	}

	TrailSample SampleTrail(float3 positionWS, float3 viewDirectionWS)
	{
		const SharedData::DynamicSnowSettings s = SharedData::dynamicSnowSettings;
		TrailSample o = SampleTrailAt(positionWS.xy, positionWS.z);
		[branch] if (o.any && abs(o.pressed) > 1e-3)
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
