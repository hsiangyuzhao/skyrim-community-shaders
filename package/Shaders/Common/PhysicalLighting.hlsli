#ifndef __PHYSICAL_LIGHTING_HLSLI__
#define __PHYSICAL_LIGHTING_HLSLI__

// (49h stage 1) "Physical Lighting" (F11 > Sky > Physical Sky > Scene Lighting): the rule bits PhysicalSky writes into
// SharedData::horizonFixData.Flags (unused since 47m; the SharedData layout is unchanged). Mirrors PhysicalSky::PhysLightFlags.
// 0 with the master off, without Physical Sky loaded, or before the first frame: every rule then takes its 49h-groups-1-7 path.

#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"

namespace PhysLight
{
	static const uint kDirect = 1u << 0;          // S1 clouds / lit effects / moonlight in ground units
	static const uint kMoonGrey = 1u << 1;        // S1 option: partial desaturation of moonlight on clouds kept
	static const uint kCloudAmbient = 1u << 2;    // S2 clouds get the ground's sky ambient
	static const uint kIblNoFeedback = 1u << 3;   // S3a the screen probe is captured without the ambient in the frame
	static const uint kEnvOcclusion = 1u << 4;    // S3b the env probe's upper hemisphere x skylighting
	static const uint kFarSkylight = 1u << 5;     // S4b fallback visibility beyond the skylighting probe volume
	static const uint kLodScale = 1u << 6;        // S5 LOD land in the near land's light scale, LOD gamma on both techniques
	static const uint kFarFog = 1u << 7;          // S6 the far height fog continues the volumetric fog
	static const uint kForwardCubemap = 1u << 8;  // S7 forward cubemap reflections at the cubemap's own brightness
	static const uint kSmallFixes = 1u << 9;      // S8 small consistency fixes
	static const uint kLandPbr = 1u << 10;        // information: the loaded land uses True PBR texture sets
	static const uint kPhysAmbient = 1u << 12;    // (49j) Physical Brightness outdoors: the ambient is the physical sky's
	static const uint kFollow = 1u << 14;         // (49j) Physical Brightness: the game's colours x the game-colour factor (horizonFixData.Altitude)

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)  // as SharedData.hlsli: FeatureData exists in pixel and compute shaders
	bool On(uint a_bit)
	{
		return (SharedData::horizonFixData.Flags & a_bit) != 0u;
	}
#else
	bool On(uint a_bit) { return false; }  // vertex shaders: no FeatureData, every rule off (none of them acts there)
#endif

	/// (49j, design 3.2 rows 20 / 28) Physical Brightness: the game's unlit colours and absolute constants (fog colour, snow rim light, the
	/// character light) were set against the game's ambient: x the game-colour factor (the ambient now / the game's, in P units). 1 while off.
#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	float GameColourFactor()
	{
		return On(kFollow) ? max(SharedData::horizonFixData.Altitude, 0.0) : 1.0;
	}

	/// (49j, design 9-6) Physical Brightness outdoors: the ground's bounce light from below in the physical ambient, at a face with normal
	/// a_n (rho E_h (1 - n.z) / 2 / pi x P, shader units), so it can be kept out of the sky-visibility occlusion. Halves packed by
	/// PhysicalSky::Reset in fields no other shader reads: physSkyData._pad5 = (r, g), physSkyExtData.SunGlowIntensity low half = b.
	float3 GroundBounce(float3 a_n)
	{
		const uint rg = asuint(SharedData::physSkyData._pad5);
		const uint b = asuint(SharedData::physSkyExtData.SunGlowIntensity);
		const float3 bounce = max(float3(f16tof32(rg & 0xFFFF), f16tof32(rg >> 16), f16tof32(b & 0xFFFF)), 0.0) * (0.5 * (1.0 - a_n.z));
		return On(kPhysAmbient) ? bounce : 0.0;  // single exit (no uninitialised-path warning)
	}
#endif

	/// S8: Linear Lighting's directional-light multiplier applies (as Lighting.hlsl decides it: not indoors). Off: without the interior check (49g).
#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	bool DirLightMultApplies()
	{
		return SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear &&
		       !(On(kSmallFixes) && SharedData::InInterior);
	}
#endif

	// S5: the LOD land techniques (LODLANDSCAPE / LODLANDNOISE; the game's log shows both in use: Lighting:Pixel:9000015 and
	// 12000005). Defined here, not in Lighting.hlsl (no new #if blocks there).
#if defined(LODLANDSCAPE) || defined(LODLANDNOISE)
	static const bool kLodLandTechnique = true;
#else
	static const bool kLodLandTechnique = false;
#endif

	/// S5: the direct light's diffuse factor of the non-PBR path: the vanilla factor (Linear Lighting 0.28), but 1 / pi on LOD land while
	/// the near land is True PBR (its direct diffuse is E / pi): no step at the loaded edge.
#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)
	float LodLandDirectMult()
	{
		return (kLodLandTechnique && ENABLE_LL && On(kLodScale) && On(kLandPbr)) ? (1.0 / Math::PI) : Color::VanillaDiffuseMult();
	}
#endif
}

#endif
