#pragma once

std::pair<unsigned char*, size_t> GetFeatureBufferData(bool a_early);

/// (52a) Byte offsets inside FeatureData of the fields Physical Lighting writes (PhysicalLighting::PatchFeatureData): the light values it
/// scales and the unread pad slots that carry its shader scalars. Pinned against the layout mirror in FeatureBuffer.cpp.
struct FeatureDataUnitOffsets
{
	size_t total;
	// light values (float3 unless noted)
	size_t sunlightColor, masserColor, secundaColor;     // physSkyData
	size_t nightBaseUpper, nightBaseHorizon, nightBaseLower;  // physSkyNightData
	size_t masserDiskScale, secundaDiskScale;            // physSkyMoonData (float)
	size_t cloudSunColor, cloudMasserColor, cloudSecundaColor;  // physSkyCloudData
	size_t fogSkyLight;                                  // horizonFixData (float)
	size_t pointLightMult, emitColorMult, glowmapMult;   // linearLightingSettings (float)
	size_t physSkyEnabled;                               // physSkyData.enabled (uint, read only)
	// shader scalars in unread pad slots (float bits)
	size_t padCharacterLight;  // physSkyData._pad5                  (Lighting)
	size_t padSnowRim;         // iblSettings.pad                     (Lighting)
	size_t padFog;             // postProcessingSettings.pad0.x       (Lighting, ISSAOComposite, Water)
	size_t padFlareOut;        // postProcessingSettings.pad0.y       (ISIBLensFlare)
	size_t padSkyDome;         // cloudShadowsSettings.pad0.x         (Sky)
	size_t padSkyStars;        // cloudShadowsSettings.pad0.y         (Sky)
	size_t padSkySunDisc;      // cloudShadowsSettings.pad0.z         (Sky)
	size_t padSkyMoonBase;     // lodBlendingSettings.pad0.x          (Sky)
	size_t padSkyCloud;        // lodBlendingSettings.pad0.y          (Sky)
	size_t padWaterGlint;      // lodBlendingSettings.pad0.z          (Water: 1 = the sun glint reads SharedData::DirLightColor)
	size_t padWaterBody;       // terrainVariationSettings.pad0.x     (Water)
	size_t padWaterFallback;   // terrainVariationSettings.pad0.y     (Water)
	size_t padPrecipitation;   // volumetricLightingSettings.pad0.x   (Particle)
	size_t padSparkle;         // volumetricLightingSettings.pad0.y   (ISSAOComposite)
	size_t padFrameS;          // volumetricShadowsSettings.pad0.x    (ISSnowSSS, ISIBLensFlare)
	size_t padFrameInvS;       // volumetricShadowsSettings.pad0.y    (ISSnowSSS, ISIBLensFlare)
};
const FeatureDataUnitOffsets& GetFeatureDataUnitOffsets();