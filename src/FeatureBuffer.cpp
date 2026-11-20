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
#include "Features/PhysicalBrightness.h"
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
		[] {
			// (49h stage 2) Physical Sky "Physical Ambient": the ambient is the physical sky, so IBL adds only the surroundings' bounce
			// (values only, at upload; the saved settings are untouched). (49i) Env IBL Scale is no longer overridden (49h forced 1): the
			// user's value applies (the IBL page draws Sky IBL Scale / Saturation and DALC Amount disabled while this is in effect).
			auto ibl = globals::features::ibl.settings;
			if (globals::features::physicalSky.PhysicalAmbientActive()) {
				ibl.DALCAmount = 1.0f;
				ibl.SkyIBLScale = 0.0f;
				// (49j, design 7) Physical Brightness: the bounce's strength outdoors is its "Bounce Strength" (default 1; your Env IBL Scale indoors).
				ibl.EnvIBLScale = PhysicalBrightness::GetSingleton()->settings.envBounceStrength;
			}
			return ibl;
		}(),
		globals::features::extendedTranslucency.GetCommonBufferData(),
		[a_inWorld] {
			// (49j, design 3.2 rows 8-9) Physical Brightness: lamps and emissives in calibrated units x P; Linear Lighting's general light
			// multiplier is not applied on top. Not for menus and item previews (design 3.2 row 29).
			auto data = globals::features::linearLighting.GetCommonBufferData();
			if (const auto& pb = *PhysicalBrightness::GetSingleton(); a_inWorld && pb.Active() && data.enableLinearLighting) {
				data.lightMult = 1.0f;
				data.pointLightMult = pb.LampMult();
				data.emitColorMult = pb.EmissiveMult();
				data.glowmapMult = pb.EmissiveMult();
			}
			return data;
		}(),
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
}
