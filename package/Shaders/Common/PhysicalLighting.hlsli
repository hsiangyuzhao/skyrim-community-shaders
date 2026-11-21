#ifndef __PHYSICAL_LIGHTING_HLSLI__
#define __PHYSICAL_LIGHTING_HLSLI__

// (49h stage 1; 50a redo base) "Lighting Fixes" (F11 > Sky > Physical Sky > Scene Lighting): the rule bits PhysicalSky writes into
// SharedData::horizonFixData.Flags (unused since 47m; the SharedData layout is unchanged). Mirrors PhysicalSky::PhysLightFlags.
// 0 with the master off, without Physical Sky loaded, or before the first frame: every rule then takes its 49h-groups-1-7 path.
// Bits 0-2 (49h S1/S2) are retired and never set; bit 15 = 49j Rain Without Colour (independent of the master).

#include "Common/Color.hlsli"
#include "Common/SharedData.hlsli"

namespace PhysLight
{
	static const uint kIblNoFeedback = 1u << 3;   // S3a the screen probe is captured without the ambient in the frame
	static const uint kEnvOcclusion = 1u << 4;    // S3b the env probe's upper hemisphere x skylighting
	static const uint kFarSkylight = 1u << 5;     // S4b fallback visibility beyond the skylighting probe volume
	static const uint kLodScale = 1u << 6;        // S5 LOD land in the near land's light scale, LOD gamma on both techniques
	static const uint kFarFog = 1u << 7;          // S6 the far height fog continues the volumetric fog
	static const uint kForwardCubemap = 1u << 8;  // S7 forward cubemap reflections at the cubemap's own brightness
	static const uint kSmallFixes = 1u << 9;      // S8 small consistency fixes
	static const uint kLandPbr = 1u << 10;        // information: the loaded land uses True PBR texture sets

#if defined(PSHADER) || defined(CSHADER) || defined(COMPUTESHADER)  // as SharedData.hlsli: FeatureData exists in pixel and compute shaders
	bool On(uint a_bit)
	{
		return (SharedData::horizonFixData.Flags & a_bit) != 0u;
	}
#else
	bool On(uint a_bit) { return false; }  // vertex shaders: no FeatureData, every rule off (none of them acts there)
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
