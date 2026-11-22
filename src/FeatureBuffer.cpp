#include "FeatureBuffer.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
#include "Features/DynamicSnow.h"
#include "Features/ExponentialHeightFog.h"
#include "Features/ExtendedMaterials.h"
#include "Features/ExtendedTranslucency.h"
#include "Features/GrassLighting.h"
#include "Features/HairSpecular.h"
#include "Features/IBL.h"
#include "Features/LODBlending.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/Skin.h"
#include "Features/ScreenSpacePointLightShadows.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/Skylighting.h"
#include "Features/TerrainShadows.h"
#include "Features/TerrainVariation.h"
#include "Features/VanillaFresnel.h"
#include "Features/VolumetricLighting.h"
#include "Features/VolumetricShadows.h"
#include "Features/WetnessEffects.h"

#include "TruePBR.h"

template <class... Ts>
std::pair<unsigned char*, size_t> _GetFeatureBufferData(Ts... feat_datas)
{
	size_t totalSize = (... + sizeof(Ts));
	auto data = new unsigned char[totalSize];
	size_t offset = 0;

	([&] {
		*((decltype(feat_datas)*)(data + offset)) = feat_datas;
		offset += sizeof(decltype(feat_datas));
	}(),
		...);

	return std::make_pair(data, totalSize);
}

std::pair<unsigned char*, size_t> GetFeatureBufferData(bool a_inWorld)
{
	return _GetFeatureBufferData(
		globals::features::grassLighting.settings,
		globals::features::extendedMaterials.settings,
		globals::features::dynamicCubemaps.settings,
		globals::features::terrainShadows.GetCommonBufferData(),
		globals::features::lightLimitFix.GetCommonBufferData(),
		globals::features::wetnessEffects.GetCommonBufferData(),
		globals::features::skylighting.GetCommonBufferData(a_inWorld),
		globals::features::cloudShadows.settings,
		globals::features::lodBlending.settings,
		globals::features::hairSpecular.settings,
		globals::features::terrainVariation.settings,
		globals::features::ibl.settings,
		globals::features::extendedTranslucency.GetCommonBufferData(),
		globals::features::linearLighting.GetCommonBufferData(),
		globals::features::postProcessing.settings,
		globals::features::skin.GetCommonBufferData(),
		globals::features::screenSpacePointLightShadows.GetCommonBufferData(),
		globals::features::vanillaFresnel.GetCommonBufferData(),  // (48c) Env Mask: Apply Once rides in EyeDirectSpecularMode bit 8
		globals::features::physicalSky.cbData,
		globals::features::screenSpaceRayTracing.GetCommonBufferData(),
		globals::features::exponentialHeightFog.GetCommonBufferData(),
		globals::features::volumetricLighting.GetCommonBufferData(),
		globals::features::physicalSky.extCbData,
		globals::features::exponentialHeightFog.GetVolumetricPSData(),
		globals::features::volumetricShadows.GetCommonBufferData(),
		globals::features::dynamicSnow.GetCommonBufferData(),
		globals::features::physicalSky.moonCbData,
		globals::features::physicalSky.nightCbData,
		globals::features::physicalSky.cloudCbData,
		globals::features::dynamicSnow.GetMapBufferData(),
		globals::features::exponentialHeightFog.GetWeatherFogData(),  // (47d)
		globals::features::physicalSky.horizonFixCbData);  // (47f)
}

namespace
{
	// (batch 15) FeatureData in package/Shaders/Common/SharedData.hlsli is nothing but the
	// concatenation above, in that order, so every struct's offset is the sum of the sizes of
	// the ones before it. Grow any one of them and everything after it moves -- silently, with
	// a clean build, and only visible in-game as a feature reading another feature's numbers.
	//
	// Mirroring the concatenation as a real struct lets offsetof() pin the C++ side against the
	// offsets fxc reports for the HLSL side. The numbers below were read out of fxc's reflection
	// listing for Lighting.hlsl (fxc -Fc, `cbuffer SharedData::FeatureData`), not calculated.
	// Batch 15 grew VanillaFresnel::Settings from 64 to 80 bytes, which is why every entry after
	// it sits 16 bytes later than it did at ea8459bd0.
	template <class T>
	using SettingsOf = std::decay_t<T>;

	struct FeatureDataLayoutMirror
	{
		SettingsOf<decltype(globals::features::grassLighting.settings)> grassLighting;
		SettingsOf<decltype(globals::features::extendedMaterials.settings)> extendedMaterials;
		SettingsOf<decltype(globals::features::dynamicCubemaps.settings)> dynamicCubemaps;
		SettingsOf<decltype(globals::features::terrainShadows.GetCommonBufferData())> terrainShadows;
		SettingsOf<decltype(globals::features::lightLimitFix.GetCommonBufferData())> lightLimitFix;
		SettingsOf<decltype(globals::features::wetnessEffects.GetCommonBufferData())> wetnessEffects;
		SettingsOf<decltype(globals::features::skylighting.GetCommonBufferData(true))> skylighting;
		SettingsOf<decltype(globals::features::cloudShadows.settings)> cloudShadows;
		SettingsOf<decltype(globals::features::lodBlending.settings)> lodBlending;
		SettingsOf<decltype(globals::features::hairSpecular.settings)> hairSpecular;
		SettingsOf<decltype(globals::features::terrainVariation.settings)> terrainVariation;
		SettingsOf<decltype(globals::features::ibl.settings)> ibl;
		SettingsOf<decltype(globals::features::extendedTranslucency.GetCommonBufferData())> extendedTranslucency;
		SettingsOf<decltype(globals::features::linearLighting.GetCommonBufferData())> linearLighting;
		SettingsOf<decltype(globals::features::postProcessing.settings)> postProcessing;
		SettingsOf<decltype(globals::features::skin.GetCommonBufferData())> skin;
		SettingsOf<decltype(globals::features::screenSpacePointLightShadows.GetCommonBufferData())> screenSpacePointLightShadows;
		SettingsOf<decltype(globals::features::vanillaFresnel.settings)> vanillaFresnel;
		SettingsOf<decltype(globals::features::physicalSky.cbData)> physicalSky;
		SettingsOf<decltype(globals::features::screenSpaceRayTracing.GetCommonBufferData())> screenSpaceRayTracing;
		SettingsOf<decltype(globals::features::exponentialHeightFog.GetCommonBufferData())> exponentialHeightFog;
		SettingsOf<decltype(globals::features::volumetricLighting.GetCommonBufferData())> volumetricLighting;
		SettingsOf<decltype(globals::features::physicalSky.extCbData)> physicalSkyExt;
		SettingsOf<decltype(globals::features::exponentialHeightFog.GetVolumetricPSData())> volumetricFog;
		SettingsOf<decltype(globals::features::volumetricShadows.GetCommonBufferData())> volumetricShadows;
		SettingsOf<decltype(globals::features::dynamicSnow.GetCommonBufferData())> dynamicSnow;
		SettingsOf<decltype(globals::features::physicalSky.moonCbData)> physicalSkyMoon;
		SettingsOf<decltype(globals::features::physicalSky.nightCbData)> physicalSkyNight;
		SettingsOf<decltype(globals::features::physicalSky.cloudCbData)> physicalSkyCloud;
		SettingsOf<decltype(globals::features::dynamicSnow.GetMapBufferData())> dynamicSnowMap;
		SettingsOf<decltype(globals::features::exponentialHeightFog.GetWeatherFogData())> weatherFog;
		SettingsOf<decltype(globals::features::physicalSky.horizonFixCbData)> horizonFix;  // (47f)
	};

	static_assert(offsetof(FeatureDataLayoutMirror, vanillaFresnel) == 1056,
		"SharedData::vanillaFresnelSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, physicalSky) == 1136,
		"SharedData::physSkyData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, screenSpaceRayTracing) == 1392,
		"SharedData::ssrtSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, exponentialHeightFog) == 1424,
		"SharedData::exponentialHeightFogSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, volumetricLighting) == 1488,
		"(batch 37b) SharedData::volumetricLightingSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, physicalSkyExt) == 1504,
		"(batch 37b) SharedData::physSkyExtData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, volumetricFog) == 1520,
		"(batch 38) SharedData::volumetricFogSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, volumetricShadows) == 1584,
		"(batch 38) SharedData::volumetricShadowsSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, dynamicSnow) == 1600,
		"(batch 39) SharedData::dynamicSnowSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, physicalSkyMoon) == 1712,
		"(40d) SharedData::physSkyMoonData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, physicalSkyNight) == 1728,
		"(41b) SharedData::physSkyNightData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, physicalSkyCloud) == 1792,
		"(42b) SharedData::physSkyCloudData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, dynamicSnowMap) == 1856,
		"(44h) SharedData::dynamicSnowMapData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, weatherFog) == 1904,
		"(47d) SharedData::weatherFogData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, horizonFix) == 1920,
		"(47f) SharedData::horizonFixData moved; update the HLSL mirror and this offset together.");
	static_assert(sizeof(FeatureDataLayoutMirror) == 1936,  // (47f) + HorizonFixData 16; (47d) + WeatherFogData 16; (44h) + DynamicSnowMapData 48; (44h) + LODBlending 16 (32 -> 48, every offset above +16); (42c) + WetnessEffects 32; (42b) + PhysSkyCloudData 64; (41b) + PhysSkyNightData 64; (40d) + PhysSkyMoonData 16; (batch 39c) DynamicSnow 80 -> 112
		"FeatureData's total size changed; check every offset above against fxc's reflection listing.");

	// (52a) The in-block offsets Physical Lighting relies on, against the HLSL packing of SharedData.hlsli (verified with fxc's
	// reflection listing of the FeatureData cbuffer, see specs/spec-batch52a.md).
	using PhysSkyCb = decltype(FeatureDataLayoutMirror::physicalSky);
	using NightCb = decltype(FeatureDataLayoutMirror::physicalSkyNight);
	using MoonCb = decltype(FeatureDataLayoutMirror::physicalSkyMoon);
	using CloudCb = decltype(FeatureDataLayoutMirror::physicalSkyCloud);
	using HorizonCb = decltype(FeatureDataLayoutMirror::horizonFix);
	using LinearCb = decltype(FeatureDataLayoutMirror::linearLighting);
	using IblCb = decltype(FeatureDataLayoutMirror::ibl);
	using PostCb = decltype(FeatureDataLayoutMirror::postProcessing);
	using CloudShadowsCb = decltype(FeatureDataLayoutMirror::cloudShadows);
	using LodCb = decltype(FeatureDataLayoutMirror::lodBlending);
	using TerrainVariationCb = decltype(FeatureDataLayoutMirror::terrainVariation);
	using VolLightCb = decltype(FeatureDataLayoutMirror::volumetricLighting);
	using VolShadowCb = decltype(FeatureDataLayoutMirror::volumetricShadows);
	static_assert(offsetof(PhysSkyCb, sunlightColor) == 48 && offsetof(PhysSkyCb, masserColor) == 80 && offsetof(PhysSkyCb, secundaColor) == 112 &&
				  offsetof(PhysSkyCb, enabled) == 124 && offsetof(PhysSkyCb, _pad5) == 188,
		"(52a) PhysSkyData packing");
	static_assert(offsetof(NightCb, baseUpper) == 16 && offsetof(NightCb, baseHorizon) == 32 && offsetof(NightCb, baseLower) == 48, "(52a) PhysSkyNightData packing");
	static_assert(offsetof(MoonCb, masserDiskScale) == 0 && offsetof(MoonCb, secundaDiskScale) == 4, "(52a) PhysSkyMoonData packing");
	static_assert(offsetof(CloudCb, sunColor) == 0 && offsetof(CloudCb, masserColor) == 16 && offsetof(CloudCb, secundaColor) == 32, "(52a) PhysSkyCloudData packing");
	static_assert(offsetof(HorizonCb, fogSkyLight) == 12, "(52a) HorizonFixData packing");
	static_assert(offsetof(LinearCb, pointLightMult) == 92 && offsetof(LinearCb, emitColorMult) == 96 && offsetof(LinearCb, glowmapMult) == 100, "(52a) LinearLightingSettings packing");
	static_assert(offsetof(IblCb, pad) == 60 && sizeof(IblCb) == 64, "(52a) IBLSettings.pad");
	static_assert(offsetof(PostCb, pad) == 4 && sizeof(PostCb) == 16, "(52a) PostProcessingSettings.pad0");
	static_assert(offsetof(CloudShadowsCb, pad) == 4 && sizeof(CloudShadowsCb) == 16, "(52a) CloudShadowsSettings.pad0");
	static_assert(offsetof(LodCb, pad0) == 36 && sizeof(LodCb) == 48, "(52a) LODBlendingSettings.pad0");
	static_assert(offsetof(TerrainVariationCb, pad0) == 8 && sizeof(TerrainVariationCb) == 16, "(52a) TerrainVariationSettings.pad0");
	static_assert(offsetof(VolLightCb, pad0) == 8 && sizeof(VolLightCb) == 16, "(52a) VolumetricLightingSettings.pad0");
	static_assert(offsetof(VolShadowCb, pad0) == 8 && offsetof(VolShadowCb, pad1) == 12 && sizeof(VolShadowCb) == 16, "(52a) VolumetricShadowsSettings.pad0");
}

const FeatureDataUnitOffsets& GetFeatureDataUnitOffsets()
{
	using M = FeatureDataLayoutMirror;
	static const FeatureDataUnitOffsets offsets = {
		.total = sizeof(M),
		.sunlightColor = offsetof(M, physicalSky) + offsetof(PhysSkyCb, sunlightColor),
		.masserColor = offsetof(M, physicalSky) + offsetof(PhysSkyCb, masserColor),
		.secundaColor = offsetof(M, physicalSky) + offsetof(PhysSkyCb, secundaColor),
		.nightBaseUpper = offsetof(M, physicalSkyNight) + offsetof(NightCb, baseUpper),
		.nightBaseHorizon = offsetof(M, physicalSkyNight) + offsetof(NightCb, baseHorizon),
		.nightBaseLower = offsetof(M, physicalSkyNight) + offsetof(NightCb, baseLower),
		.masserDiskScale = offsetof(M, physicalSkyMoon) + offsetof(MoonCb, masserDiskScale),
		.secundaDiskScale = offsetof(M, physicalSkyMoon) + offsetof(MoonCb, secundaDiskScale),
		.cloudSunColor = offsetof(M, physicalSkyCloud) + offsetof(CloudCb, sunColor),
		.cloudMasserColor = offsetof(M, physicalSkyCloud) + offsetof(CloudCb, masserColor),
		.cloudSecundaColor = offsetof(M, physicalSkyCloud) + offsetof(CloudCb, secundaColor),
		.fogSkyLight = offsetof(M, horizonFix) + offsetof(HorizonCb, fogSkyLight),
		.pointLightMult = offsetof(M, linearLighting) + offsetof(LinearCb, pointLightMult),
		.emitColorMult = offsetof(M, linearLighting) + offsetof(LinearCb, emitColorMult),
		.glowmapMult = offsetof(M, linearLighting) + offsetof(LinearCb, glowmapMult),
		.physSkyEnabled = offsetof(M, physicalSky) + offsetof(PhysSkyCb, enabled),
		.padCharacterLight = offsetof(M, physicalSky) + offsetof(PhysSkyCb, _pad5),
		.padSnowRim = offsetof(M, ibl) + offsetof(IblCb, pad),
		.padFog = offsetof(M, postProcessing) + offsetof(PostCb, pad) + 0,
		.padFlareOut = offsetof(M, postProcessing) + offsetof(PostCb, pad) + 4,
		.padSkyDome = offsetof(M, cloudShadows) + offsetof(CloudShadowsCb, pad) + 0,
		.padSkyStars = offsetof(M, cloudShadows) + offsetof(CloudShadowsCb, pad) + 4,
		.padSkySunDisc = offsetof(M, cloudShadows) + offsetof(CloudShadowsCb, pad) + 8,
		.padSkyMoonBase = offsetof(M, lodBlending) + offsetof(LodCb, pad0) + 0,
		.padSkyCloud = offsetof(M, lodBlending) + offsetof(LodCb, pad0) + 4,
		.padWaterGlint = offsetof(M, lodBlending) + offsetof(LodCb, pad0) + 8,
		.padWaterBody = offsetof(M, terrainVariation) + offsetof(TerrainVariationCb, pad0) + 0,
		.padWaterFallback = offsetof(M, terrainVariation) + offsetof(TerrainVariationCb, pad0) + 4,
		.padPrecipitation = offsetof(M, volumetricLighting) + offsetof(VolLightCb, pad0) + 0,
		.padSparkle = offsetof(M, volumetricLighting) + offsetof(VolLightCb, pad0) + 4,
		.padFrameS = offsetof(M, volumetricShadows) + offsetof(VolShadowCb, pad0),
		.padFrameInvS = offsetof(M, volumetricShadows) + offsetof(VolShadowCb, pad1),
	};
	return offsets;
}
