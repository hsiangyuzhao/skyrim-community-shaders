#include "FeatureBuffer.h"

#include "Features/CloudShadows.h"
#include "Features/DynamicCubemaps.h"
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
		globals::features::vanillaFresnel.settings,
		globals::features::physicalSky.cbData,
		globals::features::screenSpaceRayTracing.GetCommonBufferData(),
		globals::features::exponentialHeightFog.settings,
		globals::features::volumetricLighting.GetCommonBufferData());
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
		SettingsOf<decltype(globals::features::exponentialHeightFog.settings)> exponentialHeightFog;
		SettingsOf<decltype(globals::features::volumetricLighting.GetCommonBufferData())> volumetricLighting;
	};

	static_assert(offsetof(FeatureDataLayoutMirror, vanillaFresnel) == 1008,
		"SharedData::vanillaFresnelSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, physicalSky) == 1088,
		"SharedData::physSkyData moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, screenSpaceRayTracing) == 1344,
		"SharedData::ssrtSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, exponentialHeightFog) == 1376,
		"SharedData::exponentialHeightFogSettings moved; update the HLSL mirror and this offset together.");
	static_assert(offsetof(FeatureDataLayoutMirror, volumetricLighting) == 1440,
		"(batch 37b) SharedData::volumetricLightingSettings moved; update the HLSL mirror and this offset together.");
	static_assert(sizeof(FeatureDataLayoutMirror) == 1456,
		"FeatureData's total size changed; check every offset above against fxc's reflection listing.");
}