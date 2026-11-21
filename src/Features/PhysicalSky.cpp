#include "PhysicalSky.h"

#include <cfloat>
#include <DirectXPackedVector.h>
#include <imgui_stdlib.h>

#include "CloudShadows.h"
#include "ExponentialHeightFog.h"
#include "Deferred.h"
#include "LinearLighting.h"
#include "SkySync.h"
#include "TerrainShadows.h"
#include "PostProcessing/HistogramAutoExposure.h"  // (50b) Sky Light Wobble freezes the eye

#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"
#include "TruePBR.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::WorldspaceInfo,
	zBottom)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PhysicalSky::Settings,
	enabled,
	worldspaceWhitelist,
	worldspaceRemovedDefaults,
	enableAllExteriorWorldspaces,
	overrideDirLight,
	tonemapper,
	vanillaMix,
	trMix,
	apLumMix,
	apTrMix,
	cloudShadowRemapRange,
	sunlightColor,
	masserColor,
	secundaColor,
	// (47m) removed sunDiskRad (Legacy (36f) only); the old key is ignored on load.
	sunReplaceVanilla,
	sunSoftEdge,
	sunPhysicalRadiance,
	sunRadianceCap,
	sunHideVanillaGlare,
	sunDiskRadiusDeg,
	sunGlowIntensity,
	sunGlowWidthDeg,
	sunLook,
	// (44h) removed hideNewMoonDisc/moonGlowFollowsMoon/moonPhysicalRadiance/moonGlowFade/
	// skyTrueSunHeight (and moonsFadeInDaylight in Load/SaveSettings): fixed on (47m: in the code);
	// old keys are ignored on load.
	moonGlowStrength,
	moonRadianceCap,
	nightBaseLight,
	twilightLength,
	// (47m) removed skyModel ("Sky Model: Legacy (36f)"); the old key is ignored on load.
	// (44h) removed fixSkyAlpha/fixTrLutEdge/fixApShadowDepth/fixReflectionSky/fixMultiScatter:
	// fixed on (47m: in the code); old keys are ignored on load.
	adaptationStart,
	adaptationEnd,
	dayExposure,
	nightExposure,
	groundAlbedo,
	planetRadius,
	atmosphereRadius,
	rayleighFalloff,
	rayleighScatter,
	aerosolFalloff,
	aerosolPhaseG,
	aerosolScatter,
	aerosolAbsorption,
	ozoneAltitude,
	ozoneThickness,
	ozoneAbsorption,
	cloudRelightMix,
	cloudOriginalMix,
	silverLiningMix,
	silverLiningSpread)

namespace
{
	void InfoBox(const char* str)
	{
		if (ImGui::BeginTable("Info", 1, ImGuiTableFlags_BordersOuter | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::TextWrapped(str);
			ImGui::EndTable();
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////

void PhysicalSky::DataLoaded()
{
	if (!globals::features::skySync.loaded) {
		failedLoadedMessage = "Sky Sync is required for Physical Sky to function.";
		loaded = false;
	}
}

void PhysicalSky::RestoreDefaultSettings()
{
	settings = {};
	UpdateEffective();
}

void PhysicalSky::LoadSettings(json& o_json)
{
	settings = o_json;
	// (42b) Outside the NLOHMANN macro, which takes at most 64 fields.
	if (o_json.is_object()) {
		const Settings defaults{};
		settings.cloudLighting = o_json.value("cloudLighting", defaults.cloudLighting);
		settings.cloudSkyAmbient = o_json.value("cloudSkyAmbient", defaults.cloudSkyAmbient);
		settings.cloudLightHeightKm = o_json.value("cloudLightHeightKm", defaults.cloudLightHeightKm);
		// (44h) moonsFadeInDaylight no longer loaded: fixed on (47m: in the code).
		settings.moonDaylightFade = o_json.value("moonDaylightFade", defaults.moonDaylightFade);
		settings.moonCloudLight = o_json.value("moonCloudLight", defaults.moonCloudLight);
		settings.moonCloudStrength = o_json.value("moonCloudStrength", defaults.moonCloudStrength);
		settings.moonCloudHalo = o_json.value("moonCloudHalo", defaults.moonCloudHalo);
		// (48o) skyFixes (47a), skyPrecision (47b) and moonPlanetshine (48b) are always on; their keys are ignored.
		// (47m) horizonFix (47f) removed; the old key is ignored.
		settings.aerosolPreset = o_json.value("aerosolPreset", defaults.aerosolPreset);  // (47b)
		settings.moonFadesStars = o_json.value("moonFadesStars", defaults.moonFadesStars);  // (48b)
		settings.waterSunTransmittance = o_json.value("waterSunTransmittance", defaults.waterSunTransmittance);  // (49h)
		settings.rainNoColour = o_json.value("rainNoColour", defaults.rainNoColour);  // (49j)
		// (49h stage 1) Physical Lighting: missing keys keep their defaults.
		if (const auto it = o_json.find("physicalLighting"); it != o_json.end() && it->is_object()) {
			auto& pl = settings.physicalLighting;
			const Settings::PhysicalLighting d{};
			const auto& j = *it;
			pl.enabled = j.value("enabled", d.enabled);
			pl.iblNoFeedback = j.value("iblNoFeedback", d.iblNoFeedback);
			pl.iblEnvOcclusion = j.value("iblEnvOcclusion", d.iblEnvOcclusion);
			pl.farShadows = j.value("farShadows", d.farShadows);
			pl.farSkylight = j.value("farSkylight", d.farSkylight);
			pl.lodScale = j.value("lodScale", d.lodScale);
			pl.farFog = j.value("farFog", d.farFog);
			pl.forwardCubemap = j.value("forwardCubemap", d.forwardCubemap);
			pl.smallFixes = j.value("smallFixes", d.smallFixes);
		}
		// (50b) Stage 2: missing keys keep their defaults (on).
		if (const auto it = o_json.find("lightingStage2"); it != o_json.end() && it->is_object()) {
			auto& st = settings.lightingStage2;
			const Settings::LightingStage2 d{};
			const auto& j = *it;
			st.enabled = j.value("enabled", d.enabled);
			st.skyAnchor = j.value("skyAnchor", d.skyAnchor);
			st.cloudsLikeGround = j.value("cloudsLikeGround", d.cloudsLikeGround);
			st.weatherHidesMoon = j.value("weatherHidesMoon", d.weatherHidesMoon);
			st.starExtinction = j.value("starExtinction", d.starExtinction);
		}
		// (51a) Physical Outdoor Light: missing keys keep their defaults.
		if (const auto it = o_json.find("physicalOutdoorLight"); it != o_json.end() && it->is_object()) {
			auto& ol = settings.outdoorLight;
			const Settings::OutdoorLight d{};
			const auto& j = *it;
			ol.enabled = j.value("enabled", d.enabled);
			ol.sunFamily = j.value("sunFamily", d.sunFamily);
			ol.weatherGloominess = j.value("weatherGloominess", d.weatherGloominess);
			ol.moonFamily = j.value("moonFamily", d.moonFamily);
			ol.nightGlow = j.value("nightGlow", d.nightGlow);
			ol.nightSkyGlow = j.value("nightSkyGlow", d.nightSkyGlow);
			ol.ambient = j.value("ambient", d.ambient);
			ol.ambientTint = j.value("ambientTint", d.ambientTint);
			ol.clouds = j.value("clouds", d.clouds);
			ol.smoothSun = j.value("smoothSun", d.smoothSun);
			ol.realOzone = j.value("realOzone", d.realOzone);
		}
		settings.moonStarFadeStrength = o_json.value("moonStarFadeStrength", defaults.moonStarFadeStrength);
		settings.moonPlanetshineStrength = o_json.value("moonPlanetshineStrength", defaults.moonPlanetshineStrength);
	}
	settings.aerosolPreset = std::clamp(settings.aerosolPreset, 0, kAerosolPresetCount - 1);
	settings.nightBaseLight = std::clamp(settings.nightBaseLight, 0.f, 1.f);
	settings.twilightLength = std::clamp(settings.twilightLength, 1.f, 3.f);
	settings.cloudLighting = std::clamp(settings.cloudLighting, 0, kCloudLightingCount - 1);
	settings.cloudSkyAmbient = std::clamp(settings.cloudSkyAmbient, 0.f, 4.f);
	settings.cloudLightHeightKm = std::clamp(settings.cloudLightHeightKm, 0.5f, 12.f);
	settings.moonDaylightFade = std::clamp(settings.moonDaylightFade, 0.f, 100.f);
	settings.moonCloudStrength = std::clamp(settings.moonCloudStrength, 0.f, 3.f);
	settings.moonCloudHalo = std::clamp(settings.moonCloudHalo, 0.f, 4.f);
	settings.moonStarFadeStrength = std::isfinite(settings.moonStarFadeStrength) ? std::clamp(settings.moonStarFadeStrength, 0.f, 3.f) : 1.f;  // (48b)
	settings.moonPlanetshineStrength = std::isfinite(settings.moonPlanetshineStrength) ? std::clamp(settings.moonPlanetshineStrength, 0.f, 0.5f) : 0.1f;
	{
		auto& ol = settings.outdoorLight;  // (51a)
		ol.weatherGloominess = std::isfinite(ol.weatherGloominess) ? std::clamp(ol.weatherGloominess, 0.f, 1.f) : 1.f;
		ol.nightSkyGlow = std::isfinite(ol.nightSkyGlow) ? std::clamp(ol.nightSkyGlow, 0.f, 100.f) : 1.f;
		ol.ambientTint = std::isfinite(ol.ambientTint) ? std::clamp(ol.ambientTint, 0.f, 1.f) : 1.f;
	}

	// (batch 37b) A saved whitelist replaces the default one wholesale. Merge the defaults back
	// in, except those the user removed, so defaults added later still reach older configs.
	for (const auto& [name, info] : DefaultWorldspaceWhitelist()) {
		if (!settings.worldspaceWhitelist.contains(name) &&
			std::ranges::find(settings.worldspaceRemovedDefaults, name) == settings.worldspaceRemovedDefaults.end())
			settings.worldspaceWhitelist.emplace(name, info);
	}
	UpdateEffective();
}

const std::map<std::string, PhysicalSky::WorldspaceInfo>& PhysicalSky::LegacyWorldspaceWhitelist()
{
	static const std::map<std::string, WorldspaceInfo> legacy = {
		{ "Tamriel", { -14500.f } },
		{ "WindhelmWorld", { -14500.f } },
		{ "RiftenWorld", { -14500.f } },
		{ "MarkarthWorld", { -14500.f } },
		{ "WhiterunWorld", { -14500.f } },
		{ "SolitudeWorld", { -14500.f } },
		{ "WhiterunDragonsreachWorld", { -14500.f } },
		{ "DLC01FalmerValley", { 3000.f } },
		{ "DLC2SolstheimWorld", { 256.f } }
	};
	return legacy;
}

std::map<std::string, PhysicalSky::WorldspaceInfo> PhysicalSky::DefaultWorldspaceWhitelist()
{
	auto list = LegacyWorldspaceWhitelist();
	// Dawnguard.esm WRLD records (EDID, DNAM water height, PNAM parent-use flags):
	// - DLC1HunterHQWorld: Fort Dawnguard. Child of Tamriel sharing its map/terrain heights
	//   (own water -5000 is a placeholder), so Tamriel's -14500.
	// - DLC1VampireCastleCourtyard: uses Tamriel's water (-14000) -> -14500.
	// - DLC1AncestorsGladeWorld: separate grotto, own water -200 -> -700.
	list.emplace("DLC1HunterHQWorld", WorldspaceInfo{ -14500.f });
	list.emplace("DLC1VampireCastleCourtyard", WorldspaceInfo{ -14500.f });
	list.emplace("DLC1AncestorsGladeWorld", WorldspaceInfo{ -700.f });
	return list;
}

bool PhysicalSky::IsExcludedWorldspace(const RE::TESWorldSpace* a_worldspace)
{
	if (!a_worldspace)
		return true;

	// Other realms with a sky that is not Nirn's: Soul Cairn and the Boneyard (Dawnguard),
	// Apocrypha (Dragonborn), Sovngarde (Skyrim).
	static constexpr std::array<std::string_view, 4> otherRealms = {
		"DLC01SoulCairn", "DLC01Boneyard", "DLC2ApocryphaWorld", "Sovngarde"
	};
	const std::string_view name = a_worldspace->GetFormEditorID();
	for (const auto& realm : otherRealms)
		if (_strnicmp(name.data(), realm.data(), realm.size()) == 0 && name.size() == realm.size())
			return true;

	// Underground worldspaces (Blackreach, Darkfall Passage, Forebears' Holdout, caves...).
	return a_worldspace->flags.any(RE::TESWorldSpace::Flag::kNoSky);
}

float PhysicalSky::FallbackZBottom(const RE::TESWorldSpace* a_worldspace)
{
	constexpr float kTamrielZBottom = -14500.f;
	constexpr float kBelowSeaLevel = 500.f;
	constexpr float kSane = 100000.f;  // "no water" worldspaces store 9999999 or -500000

	const RE::TESWorldSpace* world = a_worldspace;
	while (world && world->parentWorld &&
		   world->parentUseFlags.any(RE::TESWorldSpace::ParentUseFlag::kUseLandData, RE::TESWorldSpace::ParentUseFlag::kUseWaterData))
		world = world->parentWorld;
	if (!world)
		return kTamrielZBottom;

	if (std::isfinite(world->defaultWaterHeight) && std::abs(world->defaultWaterHeight) < kSane)
		return world->defaultWaterHeight - kBelowSeaLevel;
	if (std::isfinite(world->defaultLandHeight) && std::abs(world->defaultLandHeight) < kSane)
		return world->defaultLandHeight - kBelowSeaLevel;
	return kTamrielZBottom;
}

RE::TESWorldSpace* PhysicalSky::GetCurrentWorldspace()
{
	auto* tes = RE::TES::GetSingleton();
	auto* worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
	if (!worldspace) {
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* cell = player ? player->GetParentCell() : nullptr;
		if (cell && !cell->IsInteriorCell())
			worldspace = cell->GetRuntimeData().worldSpace;
	}
	return worldspace;
}

PhysicalSky::WorldspaceStatus PhysicalSky::GetWorldspaceStatus(float& a_zBottom) const
{
	a_zBottom = WorldspaceInfo{}.zBottom;

	if (auto player = RE::PlayerCharacter::GetSingleton(); player)
		if (auto cell = player->GetParentCell(); cell && cell->IsInteriorCell())
			return WorldspaceStatus::Interior;

	auto* worldspace = GetCurrentWorldspace();
	if (!worldspace)
		return WorldspaceStatus::Unknown;
	if (IsExcludedWorldspace(worldspace))
		return WorldspaceStatus::Excluded;
	if (auto it = settings.worldspaceWhitelist.find(worldspace->GetFormEditorID()); it != settings.worldspaceWhitelist.end()) {
		a_zBottom = it->second.zBottom;
		return WorldspaceStatus::Whitelist;
	}
	if (settings.enableAllExteriorWorldspaces) {
		a_zBottom = FallbackZBottom(worldspace);
		return WorldspaceStatus::AllExteriors;
	}
	return WorldspaceStatus::NotListed;
}

void PhysicalSky::SaveSettings(json& o_json)
{
	o_json = settings;
	// (42b) See LoadSettings.
	o_json["cloudLighting"] = settings.cloudLighting;
	o_json["cloudSkyAmbient"] = settings.cloudSkyAmbient;
	o_json["cloudLightHeightKm"] = settings.cloudLightHeightKm;
	// (44h) moonsFadeInDaylight no longer saved: fixed on (47m: in the code).
	o_json["moonDaylightFade"] = settings.moonDaylightFade;
	o_json["moonCloudLight"] = settings.moonCloudLight;
	o_json["moonCloudStrength"] = settings.moonCloudStrength;
	o_json["moonCloudHalo"] = settings.moonCloudHalo;
	o_json["aerosolPreset"] = settings.aerosolPreset;  // (47b)
	o_json["moonFadesStars"] = settings.moonFadesStars;  // (48b)
	o_json["waterSunTransmittance"] = settings.waterSunTransmittance;  // (49h)
	o_json["rainNoColour"] = settings.rainNoColour;  // (49j)
	{
		const auto& pl = settings.physicalLighting;  // (49h stage 1)
		o_json["physicalLighting"] = { { "enabled", pl.enabled }, { "iblNoFeedback", pl.iblNoFeedback }, { "iblEnvOcclusion", pl.iblEnvOcclusion },
			{ "farShadows", pl.farShadows }, { "farSkylight", pl.farSkylight }, { "lodScale", pl.lodScale }, { "farFog", pl.farFog },
			{ "forwardCubemap", pl.forwardCubemap }, { "smallFixes", pl.smallFixes } };
	}
	{
		const auto& st = settings.lightingStage2;  // (50b)
		o_json["lightingStage2"] = { { "enabled", st.enabled }, { "skyAnchor", st.skyAnchor }, { "cloudsLikeGround", st.cloudsLikeGround },
			{ "weatherHidesMoon", st.weatherHidesMoon }, { "starExtinction", st.starExtinction } };
	}
	{
		const auto& ol = settings.outdoorLight;  // (51a)
		o_json["physicalOutdoorLight"] = { { "enabled", ol.enabled }, { "sunFamily", ol.sunFamily }, { "weatherGloominess", ol.weatherGloominess },
			{ "moonFamily", ol.moonFamily }, { "nightGlow", ol.nightGlow }, { "nightSkyGlow", ol.nightSkyGlow }, { "ambient", ol.ambient },
			{ "ambientTint", ol.ambientTint }, { "clouds", ol.clouds }, { "smoothSun", ol.smoothSun }, { "realOzone", ol.realOzone } };
	}
	o_json["moonStarFadeStrength"] = settings.moonStarFadeStrength;
	o_json["moonPlanetshineStrength"] = settings.moonPlanetshineStrength;
}

void PhysicalSky::GetUploadedAerosol(float3& a_scatter, float3& a_absorption, float& a_phaseG) const
{
	a_scatter = settings.aerosolScatter;
	a_absorption = settings.aerosolAbsorption;
	a_phaseG = settings.aerosolPhaseG;
	if (effective.aerosolPreset != kAerosolContinental)
		return;
	// (47b) Upstream 846c97ede, AerosolData.inl: OPAC 4.0 continental clean at 50% relative humidity, reference
	// loading (1x). Scatter / absorption in Mm^-1 for R/G/B; the phase g is the Cornette-Shanks g whose mean cosine
	// equals the OPAC asymmetry (scatter moment / green scatter), found by bisection as upstream's PhaseG().
	static const float continentalG = [] {
		constexpr float meanCosine = 11.6354217f / 17.0945f;
		float lower = -0.999f, upper = 0.999f;
		for (int i = 0; i < 24; ++i) {
			const float g = (lower + upper) * 0.5f;
			const float g2 = g * g;
			if (3.f * g * (4.f + g2) / (5.f * (2.f + g2)) < meanCosine)
				lower = g;
			else
				upper = g;
		}
		return (lower + upper) * 0.5f;
	}();
	a_scatter = { 12.61057f, 17.0945f, 22.87603f };
	a_absorption = { 0.663498f, 0.7328f, 0.802034f };
	a_phaseG = continentalG;
}

void PhysicalSky::DrawSettings()
{
	// (48o) "Sky Fixes" (47a) and "Sky Precision" (47b) are always on (no switches).

	if (ImGui::BeginTabBar("##PHYSSKY")) {
		if (ImGui::BeginTabItem("General")) {
			SettingsGeneral();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Celestials")) {
			SettingsCelestials();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Atmosphere")) {
			SettingsAtmosphere();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Clouds")) {
			SettingsClouds();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Scene Lighting")) {  // (49h stage 1)
			SettingsSceneLighting();
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("Debug")) {
			SettingsDebug();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
}

void PhysicalSky::SettingsGeneral()
{
	if (ImGui::BeginTable("Info", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
		ImGui::TableNextColumn();
		ImGui::Text("Shader Status: ");
		ImGui::TableNextColumn();
		if (ShadersOK())
			ImGui::TextColored({ 0, 1, 0, 1 }, "OK");
		else
			ImGui::TextColored({ 1, 0, 0, 1 }, "ERROR");

		ImGui::TableNextColumn();
		ImGui::Text("Worldspace: ");
		ImGui::TableNextColumn();
		{
			float zBottom = 0.f;
			const auto status = GetWorldspaceStatus(zBottom);
			auto* worldspace = GetCurrentWorldspace();
			const char* name = worldspace ? worldspace->GetFormEditorID() : "";
			switch (status) {
			case WorldspaceStatus::Interior:
				ImGui::Text("Interior (Disabled)");
				break;
			case WorldspaceStatus::Whitelist:
				ImGui::Text("%s (Enabled, list, ground %.0f)", name, zBottom);
				break;
			case WorldspaceStatus::AllExteriors:
				ImGui::Text("%s (Enabled, all exteriors, ground %.0f)", name, zBottom);
				break;
			case WorldspaceStatus::Excluded:
				ImGui::Text("%s (Disabled, other realm / no sky)", name);
				break;
			case WorldspaceStatus::NotListed:
				ImGui::Text("%s (Disabled, not listed)", name);
				break;
			default:
				ImGui::Text("Unknown");
				break;
			}
		}

		ImGui::EndTable();
	}

	ImGui::Checkbox("Enabled", &settings.enabled);

	// (47m) "Sky Model" (Current / Legacy (36f)) removed: Current is the only model.

	SettingsWorldspaces();

	ImGui::SeparatorText("Post Processing");
	{
		// (47a) Day / Night Exposure and the adaptation angles are taken over by Sky Fixes (treated as 1 / off; the saved values are
		// kept). (48o) Sky Fixes is always on: their collapsed "Exposure" node is gone.
		// As before: kept positive whenever this tab is drawn, open or not.
		settings.dayExposure = std::max(1e-10f, settings.dayExposure);
		settings.nightExposure = std::max(1e-10f, settings.nightExposure);

		if (ImGui::BeginTable("tonemap", 4, ImGuiTableFlags_SizingStretchSame, { -1, 0 })) {
			ImGui::TableNextColumn();
			ImGui::Text("Tonemapper");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How bright sky values are squeezed into display range. Ignored (always Linear) while Linear Lighting is on.");
			ImGui::TableNextColumn();
			ImGui::RadioButton("Linear", &settings.tonemapper, 0);
			ImGui::TableNextColumn();
			ImGui::RadioButton("Gamma", &settings.tonemapper, 1);
			ImGui::TableNextColumn();
			ImGui::RadioButton("Reinherd", &settings.tonemapper, 2);
			ImGui::EndTable();
		}
		ImGui::SliderFloat("Vanilla Mix", &settings.vanillaMix, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Blend in vanilla sky color.");
	}
}

void PhysicalSky::SettingsWorldspaces()
{
	// (batch 37b) Ported from upstream ba4b640f2 (editable list) + f3fb48d12 (all exteriors).
	ImGui::SeparatorText("Worldspaces");

	ImGui::Checkbox("All Exterior Worldspaces", &settings.enableAllExteriorWorldspaces);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Also use Physical Sky in exterior worldspaces that are not in the list below.\n"
			"Their ground height is worked out from the worldspace's water level.\n"
			"Other realms (Soul Cairn, Boneyard, Apocrypha, Sovngarde) and worldspaces\n"
			"without a sky (Blackreach...) always stay vanilla.");

	if (!ImGui::TreeNode("Worldspace List"))
		return;

	auto* currentWorldspace = GetCurrentWorldspace();
	const std::string currentName = currentWorldspace ? currentWorldspace->GetFormEditorID() : "";
	const auto defaults = DefaultWorldspaceWhitelist();

	const auto removeEntry = [&](const std::string& name) {
		settings.worldspaceWhitelist.erase(name);
		if (defaults.contains(name) && std::ranges::find(settings.worldspaceRemovedDefaults, name) == settings.worldspaceRemovedDefaults.end())
			settings.worldspaceRemovedDefaults.push_back(name);
	};
	const auto addEntry = [&](std::string name, float zBottom) {
		const auto first = name.find_first_not_of(" \t");
		const auto last = name.find_last_not_of(" \t");
		if (first == std::string::npos)
			return;
		name = name.substr(first, last - first + 1);
		settings.worldspaceWhitelist[name].zBottom = zBottom;
		std::erase(settings.worldspaceRemovedDefaults, name);
	};

	static std::string newName;
	static float newZBottom = -14500.f;
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
	ImGui::InputTextWithHint("##NewWorldspace", "Editor ID, e.g. Tamriel", &newName);
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.4f);
	ImGui::InputFloat("Ground##NewWorldspace", &newZBottom, 10.f, 100.f, "%.0f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Planet ground height (Z) for this worldspace: about 500 below its sea level. Tamriel: -14500.");
	if (ImGui::Button("Add / Update"))
		addEntry(newName, newZBottom);

	if (!currentName.empty()) {
		ImGui::SameLine();
		const bool listed = settings.worldspaceWhitelist.contains(currentName);
		if (ImGui::Button(listed ? "Remove Current Worldspace" : "Add Current Worldspace")) {
			if (listed)
				removeEntry(currentName);
			else
				addEntry(currentName, FallbackZBottom(currentWorldspace));
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", currentName.c_str());
	}
	ImGui::SameLine();
	if (ImGui::Button("Restore Defaults")) {
		for (const auto& [name, info] : defaults)
			settings.worldspaceWhitelist.insert_or_assign(name, info);
		settings.worldspaceRemovedDefaults.clear();
	}

	if (ImGui::BeginTable("WorldspaceWhitelist", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp, { -1, 0 })) {
		ImGui::TableSetupColumn("Editor ID");
		ImGui::TableSetupColumn("Ground", ImGuiTableColumnFlags_WidthFixed, 160.f);
		ImGui::TableSetupColumn("##Action", ImGuiTableColumnFlags_WidthFixed, 90.f);
		ImGui::TableHeadersRow();

		std::string toRemove;
		for (auto& [name, info] : settings.worldspaceWhitelist) {
			ImGui::PushID(name.c_str());
			ImGui::TableNextRow();
			ImGui::TableSetColumnIndex(0);
			ImGui::TextUnformatted(name.c_str());
			ImGui::TableSetColumnIndex(1);
			ImGui::SetNextItemWidth(-FLT_MIN);
			ImGui::InputFloat("##Ground", &info.zBottom, 10.f, 100.f, "%.0f");
			ImGui::TableSetColumnIndex(2);
			if (ImGui::Button("Remove", { -1, 0 }))
				toRemove = name;
			ImGui::PopID();
		}
		if (!toRemove.empty())
			removeEntry(toRemove);
		ImGui::EndTable();
	}
	ImGui::TreePop();
}

void PhysicalSky::SettingsCelestials()
{
	constexpr auto lightColorHint = "This sets the light color BEFORE it goes through the atmosphere i.e. extraterrestrial radiance.";

	InfoBox("The sun and moons, and their lights.");

	ImGui::Checkbox("Override Directional Light", &settings.overrideDirLight);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Overrides the color of directional light. Linear tonemapper and 1.0 transmittance mix are recommended.");
	ImGui::SliderFloat("Transmittance Mix", &settings.trMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Apply additional atmospheric tranmisttance on the directional light.\n"
			"Introduces natural yellowening at sunset with white sunlight.");
	// (49h stage 1) "Sun Highlight on Water Follows the Atmosphere" moved to Scene Lighting > Related (own switches).
	ImGui::TextDisabled("Sun highlight on water: Scene Lighting tab > Related (own switches)");

	ImGui::SeparatorText("Sun");
	{
		ImGui::PushID("Sun");
		ImGui::ColorEdit3("Light Color", &settings.sunlightColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		// (49h) Sun Look, Procedural Sun, Sun Disk Angular Radius and the procedural-sun group are retired (the game's sun is drawn).
		ImGui::PopID();
	}

	ImGui::SeparatorText("Night and Twilight");
	ImGui::SliderFloat("Night Sky Base Light", &settings.nightBaseLight, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"A faint glow of the night sky itself, in the current weather's own night sky colours (zenith,\n"
			"horizon, below), with a little deep-blue airglow where a weather's night sky is pure black.\n"
			"Without it the sky is black whenever no moon is up and the sun is far below the horizon\n"
			"(before dawn, after dusk). It also tints distant haze, reflections and the sky's ambient light.\n"
			"Fades out as the sun climbs from 6 degrees below the horizon to 4 above.\n"
			"0 = off (exactly as before). Default 0.2.");
	// (44h) removed "Sky Uses True Sun Height": fixed on (47m: in the code).
	// (47a) Twilight Length is taken over by Sky Fixes (treated as 1). (48o) Sky Fixes is always on: its "Twilight" node is gone.

	ImGui::SeparatorText("Masser");
	{
		ImGui::PushID("Masser");
		ImGui::ColorEdit3("Light Color", &settings.masserColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		ImGui::PopID();
	}

	ImGui::SeparatorText("Secunda");
	{
		ImGui::PushID("Secunda");
		ImGui::ColorEdit3("Light Color", &settings.secundaColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		ImGui::PopID();
	}

	ImGui::SeparatorText("Moon Glow and Discs");
	ImGui::SliderFloat("Moon Sky Glow Strength", &settings.moonGlowStrength, 0.f, 1.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How strongly each moon lights up the sky around it (the glow and the night sky's brightness).\n"
			"Does not change the moonlight on the ground, characters or shadows (Light Color above).\n"
			"1 = the moon light colour in full: with bright moonlight the glow is as bright as the discs,\n"
			"which then look washed out. Default 0.2: the discs stand clearly out of their glow.");
	// (44h) removed "Moon Glow Follows Phase and Visibility" (fixed on), "Moon Glow Fades" (fixed
	// At Horizon), "Moon Physical Brightness" and "Moons Fade in Daylight" (fixed on): fixed in the
	// code (47m). Their sliders below are now always shown.
	ImGui::SliderFloat("Moon Brightness Cap", &settings.moonRadianceCap, 1.f, 50.f, "%.1f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"The moon discs get brighter by their light colour divided by their size in the sky, up to this\n"
			"multiple of the game's disc brightness. Now: Masser x%.1f, Secunda x%.1f.",
			moonCbData.masserDiskScale, moonCbData.secundaDiskScale);
	ImGui::SliderFloat("Daylight Fade Strength", &settings.moonDaylightFade, 0.f, 100.f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"A moon in the daytime sky is pale, because the bright sky lies in front of it.\n"
			"Higher = paler moons by day. Default 20: at noon a moon keeps roughly 40%% of its contrast,\n"
			"in the early morning about 60%%, in twilight almost all of it. No change at night.");

	// (48b) Moonlight on the stars and the dark side of the discs.
	ImGui::Checkbox("Moonlight Fades Stars", &settings.moonFadesStars);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"A bright moon lights up the whole night sky, so the Milky Way and faint stars get hard to see.\n"
			"On: stars fade by the moonlight itself (how full and how high each moon is, both moons added,\n"
			"less when clouds cover the moon), not by the glow set above. The Milky Way and faint stars go\n"
			"first, bright stars stay. No moon up, or off: exactly as before.\n"
			"Needs Cloud Shadows for the cloud part (without it clouds do not count).");
	if (settings.moonFadesStars) {
		ImGui::SliderFloat("Star Fade Strength", &settings.moonStarFadeStrength, 0.f, 3.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How strongly moonlight hides the stars. Default 1: a full Masser high on a clear night leaves\n"
				"the Milky Way at about 40%% of its brightness; a half moon leaves about 60%%, a thin crescent 75%%.\n"
				"Now: Masser %.4f, Secunda %.4f (0 = no effect; before clouds).",
				moonStarFadeAmount[0], moonStarFadeAmount[1]);
	}
	// (48o) "Moon Planetshine" (48b) is always on; its strength stays.
	ImGui::SliderFloat("Planetshine Strength", &settings.moonPlanetshineStrength, 0.f, 0.5f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Earthshine: the dark side of each moon, lit by the Earth, strongest at a thin crescent, none at full moon.\n"
			"(49j) The dark side is a solid disc (it hides the stars behind it) with a sharp edge and the moon's own markings; its\n"
			"brightness is 1/1000 of the lit side x the Earth's phase at the default 0.10 (the real ratio); 0.20 = twice that.\n"
			"Now (relative to the real ratio): Masser %.2f, Secunda %.2f.",
			moonPlanetshineAmount[0], moonPlanetshineAmount[1]);

	// (44h) removed the "New Moon" section ("Hide New Moon Disc"): fixed on (47m: in the code).
}

void PhysicalSky::SettingsAtmosphere()
{
	InfoBox("The composition and physical properties of the atmosphere.");

	ImGui::SliderFloat("AP Luminance Mix", &settings.apLumMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Add light scattered by air (Aerial Perspective) to the scene.");
	ImGui::SliderFloat("AP Transmittance Mix", &settings.apTrMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Remove light absorbed by air (Aerial Perspective) from the scene.");

	ImGui::SliderFloat2("Cloud Shadow Remap", &settings.cloudShadowRemapRange.x, 0.f, 1.f, "%.2f");

	ImGui::SeparatorText("Air Molecules (Rayleigh)");
	{
		ImGui::PushID("Rayleigh");
		ImGui::TextWrapped(
			"Particles much smaller than the wavelength of light. They have almost complete symmetry in forward and backward scattering. "
			"On earth, they are what makes the sky blue and, at sunset, red. Usually needs no extra change.");

		ImGui::ColorEdit3("Scatter", &settings.rayleighScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::SliderFloat("Falloff", &settings.rayleighFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Aerosol (Mie)");
	{
		ImGui::PushID("Mie");
		ImGui::TextWrapped(
			"Solid and liquid particles greater than 1/10 of the light wavelength but not too much, like dust. Strongly anisotropic (Mie Scattering). "
			"They contributes to the aureole around bright celestial bodies.");

		// (47b)
		static constexpr const char* aerosolNames[kAerosolPresetCount] = { "Current", "Continental" };
		ImGui::Combo("Aerosol", &settings.aerosolPreset, aerosolNames, kAerosolPresetCount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Current: the values below.\n"
				"Continental: measured clean continental air (OPAC, 50%% humidity; upstream's preset): less and bluer\n"
				"haze, a smaller and softer glow around the sun, much less absorption. Replaces the values below\n"
				"while picked (they are kept, not changed).");
		const bool aerosolPresetOn = settings.aerosolPreset != kAerosolCurrent;
		ImGui::BeginDisabled(aerosolPresetOn);
		ImGui::SliderFloat("Anisotropy", &settings.aerosolPhaseG, -1, 1);
		ImGui::ColorEdit3("Scatter", &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3("Absorption", &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Usually 1/9 of scatter coefficient. Dust/pollution is lower, fog is higher.");
		ImGui::EndDisabled();
		ImGui::SliderFloat("Falloff", &settings.aerosolFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Ozone");
	{
		ImGui::PushID("Ozone");
		ImGui::TextWrapped(
			"The ozone layer high up in the sky that mainly absorbs light of certain wavelength. "
			"It keeps the zenith sky blue, especially at sunrise or sunset.");

		ImGui::ColorEdit3("Absorption", &settings.ozoneAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::DragFloat("Mean Altitude", &settings.ozoneAltitude, .1f, 0.f, 100.f, "%.3f km");
		ImGui::DragFloat("Layer Thickness", &settings.ozoneThickness, .1f, 0.f, 50.f, "%.3f km");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Planetary Parameters");
	{
		ImGui::InputFloat("Planet Radius", &settings.planetRadius, 1.f, 100000.f, "%.1f km");
		ImGui::InputFloat("Atmosphere Radius", &settings.atmosphereRadius, 1.f, 100000.f, "%.1f km");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Planet radius is the distance from the planet center to sea level.\n"
				"Atmosphere radius is the distance from the planet center to the top of atmosphere.\n"
				"On Earth, they are about 6360 km and 6420 km respectively.");
	}

	// (44h) removed the "Fixes" section (Opaque Sky, Transmittance Edge Fix, Atmosphere Shadow
	// Depth Fix, Reflected Sky Fix, Multiple Scattering Fix): fixed on (47m: in the code).
}

void PhysicalSky::SettingsClouds()
{
	InfoBox("Clouds.");

	static constexpr const char* cloudLightingNames[kCloudLightingCount] = { "Single light (old)", "Sun + Moons + Sky" };
	ImGui::Combo("Cloud Lighting", &settings.cloudLighting, cloudLightingNames, kCloudLightingCount);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"What lights the clouds.\n"
			"Sun + Moons + Sky (default): the sun lights them whatever lights the ground, coloured by the air\n"
			"between the sun and the cloud, so high clouds stay pink and orange for a while after sunset and\n"
			"turn first at dawn; they go dark only when the planet's own shadow reaches them. Both moons add\n"
			"their light (by phase, gone below the horizon or in weathers that hide them), and the sky adds\n"
			"its own glow, so clouds are never pitch black against a lit sky.\n"
			"Single light (old): only the one sun-or-moon light that casts shadows. Clouds went black at dawn\n"
			"and dusk while it switched over, and lost the sun the moment it set.");
	if (settings.cloudLighting == kCloudLightingAll) {
		ImGui::SliderFloat("Sky Light on Clouds", &settings.cloudSkyAmbient, 0.f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How much the sky's own light (blue by day, the night base light at night) brightens the clouds,\n"
				"mostly their shaded sides. 0 = none. Default 1.");
		ImGui::SliderFloat("Cloud Height for Sunlight", &settings.cloudLightHeightKm, 0.5f, 12.f, "%.1f km", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How high the clouds count as for catching the sun (the drawn clouds do not move). Higher = they\n"
				"stay lit longer after sunset and light up earlier before sunrise, redder. Default 2 km: lit until\n"
				"the sun is about 1.5 degrees below the horizon (more with a longer Twilight Length).");
		ImGui::Checkbox("Moonlight on Clouds", &settings.moonCloudLight);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Lets the moons light up the night clouds (the weather's dark night cloud colour no longer\nswallows it). Off = clouds look exactly as before.");
		if (settings.moonCloudLight) {
			ImGui::SliderFloat("Moonlight Strength", &settings.moonCloudStrength, 0.f, 3.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How bright the moonlight on clouds is. Default 0.5.");
			if (Stage2On(settings.lightingStage2.cloudsLikeGround))  // (50b)
				ImGui::TextDisabled("Not used now: Scene Lighting > Clouds Lit Like the Ground gives the clouds the ground's moonlight.");
			ImGui::SliderFloat("Moon Halo", &settings.moonCloudHalo, 0.f, 4.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra glow on thin clouds right around the moon. 0 = none. Default 1.");
		}
	}

	ImGui::SliderFloat("Vanilla Mix", &settings.cloudOriginalMix, 0.f, 2.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How much of the clouds' original colour is kept.");
	ImGui::SliderFloat("Relight Mix", &settings.cloudRelightMix, 0.f, 2.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How much light from the physical sun/moons is added to the clouds.");
	ImGui::SliderFloat("Silver Lining Accent", &settings.silverLiningMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightens clouds when looking toward the sun (bright cloud edges).");
	ImGui::SliderFloat("Silver Lining Spread", &settings.silverLiningSpread, -0.99f, 0.99f, "%.2f");
}

void PhysicalSky::SettingsDebug()
{
	InfoBox("Beep Boop.");

	if (ImGui::Button("Recompile Shaders"))
		ClearShaderCache();

	// (50b) Stage 2 self-test (not saved).
	ImGui::SeparatorText("Self-Test");
	ImGui::BeginDisabled(!Stage2On(true));
	if (ImGui::Checkbox("Sky Light Wobble", &skyLightWobble))
		skyLightWobbleStart = static_cast<double>(GetTickCount64()) * 1e-3;
	ImGui::EndDisabled();
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Not saved. The sky family's factor k swings up and down every 4 seconds (x2 .. x1/2) while the eye adaptation holds still.");
		ImGui::Text("  Must pulse together: sunlit / moonlit ground, the sky, clouds, the moon discs, god rays, the sun's glint on water.");
		ImGui::Text("  Must stay still: torches, windows, rain, the fog colour, the water body, a moonless starry sky, the sun disc (the game's own picture).");
		ImGui::Text("  With a moon up: faint stars and the Milky Way's edge come and go with the sky behind them; bright stars stay.");
		ImGui::Text("Anything sun- or moon-coloured that stays still is a path the factor misses. Needs Scene Lighting > Sky Family on One Scale.");
	}
	if (skyLightWobble && Stage2On(true))
		ImGui::TextDisabled("Wobbling: x%.2f now (k x%.3f), eye adaptation frozen", skyAnchorApplied / std::max(Stage2On(settings.lightingStage2.skyAnchor) ? skyAnchorK : 1.f, 1e-6f),
			Stage2On(settings.lightingStage2.skyAnchor) ? skyAnchorK : 1.f);

	ImGui::SeparatorText("Values");
	{
		ImGui::InputFloat3("Sun Direction", &cbData.sunDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3("Masser Direction", &cbData.masserDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3("Secunda Direction", &cbData.secundaDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		// (47a) Sky Fixes state, read only.
		if (SkyFixesActive()) {
			const auto& d = skyFixesDebug;
			ImGui::Text("Sky Fixes: sun %.1f deg, sky x%.2f (target x%.2f)%s", DirectX::XMConvertToDegrees(std::asin(std::clamp(nightCbData.lutSunDir.z, -1.f, 1.f))),
				nightCbData.skyScale, d.target, d.valid ? "" : " - no usable weather ambient, x1");
			ImGui::Text("  ground progress %.2f, sun scatter %.2e, moon glow %.2e, night base %.2e, night ref %.2e, day ref %.2e", d.g, d.zScatter, d.zMoon, d.zBase, d.zNight, d.zDay);
			ImGui::Text("  aerial perspective LUT %ux%u, timing warp %s", apLutW, apLutH, globals::features::skySync.sunWarp.valid ? "on" : "off (46y timing)");
		}
		// (47b)
		{
			float3 scatter, absorption;
			float g = 0.f;
			GetUploadedAerosol(scatter, absorption, g);
			ImGui::Text("Sky Precision %s; aerosol uploaded: scatter %.2f/%.2f/%.2f, absorption %.2f/%.2f/%.2f, g %.3f", (extCbData.flags & kExtSkyPrecision) ? "on" : "off",
				scatter.x, scatter.y, scatter.z, absorption.x, absorption.y, absorption.z, g);
		}
	}

	ImGui::SeparatorText("Textures");
	{
		static float debugScale = 0.2f;
		ImGui::SliderFloat("View Scale", &debugScale, 0.1f, 1.f);

		BUFFER_VIEWER_NODE_BULLET(texTrLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texMsLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texSvLut, 1.f);
		BUFFER_VIEWER_NODE_BULLET(texApShadow, debugScale);
	}
}

void PhysicalSky::SetupResources()
{
	auto device = globals::d3d::device;

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampTr.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampSv.put()));

		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, sampNoise.put()));
	}

	logger::debug("Creating textures...");
	{
		D3D11_TEXTURE2D_DESC tex2dDesc{
			.Width = kTrLutW,
			.Height = kTrLutH,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { .Count = 1, .Quality = 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = tex2dDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texTrLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texTrLut->CreateSRV(srvDesc);
		texTrLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kMsLutW;
		tex2dDesc.Height = kMsLutH;

		texMsLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texMsLut->CreateSRV(srvDesc);
		texMsLut->CreateUAV(uavDesc);

		tex2dDesc.Width = kSvLutW;
		tex2dDesc.Height = kSvLutH;

		texSvLut = eastl::make_unique<Texture2D>(tex2dDesc);
		texSvLut->CreateSRV(srvDesc);
		texSvLut->CreateUAV(uavDesc);

		// (47a) 32x32 directions as before; UpdateApLutSize() switches to 64x64 while Sky Fixes is on.
		if (!CreateApLut(kApLutW, kApLutH))
			DX::ThrowIfFailed(E_FAIL);  // as before: no aerial perspective LUT is a setup failure
	}
	{
		D3D11_TEXTURE2D_DESC texDesc;
		auto mainTex = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MipLevels = 1;
		texDesc.Width /= 2;
		texDesc.Height /= 2;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texApShadow = eastl::make_unique<Texture2D>(texDesc);
		texApShadow->CreateSRV(srvDesc);
		texApShadow->CreateUAV(uavDesc);
	}

	CompileShaders();
}

bool PhysicalSky::CreateApLut(uint16_t a_w, uint16_t a_h)
{
	D3D11_TEXTURE3D_DESC tex3dDesc{
		.Width = a_w,
		.Height = a_h,
		.Depth = kApLutD,
		.MipLevels = 1,
		.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
		.CPUAccessFlags = 0,
		.MiscFlags = 0
	};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
		.Format = tex3dDesc.Format,
		.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D,
		.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 }
	};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
		.Format = tex3dDesc.Format,
		.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kApLutD }
	};

	try {
		auto texture = eastl::make_unique<Texture3D>(tex3dDesc);
		texture->CreateSRV(srvDesc);
		texture->CreateUAV(uavDesc);
		// The old texture may still be bound from the last frame (t63 and the deferred passes): D3D11 keeps
		// its own reference to bound views, so dropping ours here is safe. Every user binds texApLut afresh
		// each frame after GenerateLuts().
		texApLut = std::move(texture);
		apLutW = a_w;
		apLutH = a_h;
		return true;
	} catch (const std::exception& e) {
		logger::warn("[Physical Sky] Could not create the {}x{}x{} aerial perspective LUT: {}", a_w, a_h, kApLutD, e.what());
		return false;
	}
}

void PhysicalSky::UpdateApLutSize()
{
	// (47a) Sky Fixes: 64x64 directions (each texel ~6 degrees instead of ~11); depth unchanged.
	const bool fine = SkyFixesActive();
	const uint16_t w = fine ? kApLutFixW : kApLutW;
	const uint16_t h = fine ? kApLutFixH : kApLutH;
	if (texApLut && w == apLutW && h == apLutH)
		return;
	static bool failedFine = false;  // do not retry a failing size every frame
	if (fine && failedFine && texApLut)
		return;
	if (!CreateApLut(w, h) && fine)
		failedFine = true;
}

void PhysicalSky::ClearShaderCache()
{
	CompileShaders();
}

void PhysicalSky::CompileShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* csPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines = {};
		std::string_view entry = "main";
	};

	std::vector<ShaderCompileInfo> shaderInfos = {
		{ &csTrLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "0" } } },
		{ &csMsLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "1" } } },
		{ &csSvLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "2" } } },
		{ &csApLutGen, "LutGen.cs.hlsl", { { "LUTGEN", "3" } } },
		{ &csShadowAccum, "ShadowAccum.cs.hlsl", {} }
	};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PhysicalSky") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.data())))
			info.csPtr->attach(rawPtr);
	}
}

bool PhysicalSky::ShadersOK()
{
	return csTrLutGen && csMsLutGen && csSvLutGen && csApLutGen && csShadowAccum;
}

void PhysicalSky::UpdateExtCbData()
{
	extCbData = {};
	if (!loaded)
		return;

	const auto& s = effective;
	uint flags = 0;
	if (s.sunReplaceVanilla)
		flags |= kExtSunReplace;
	if (s.sunSoftEdge)
		flags |= kExtSunSoftEdge;
	if (s.sunPhysicalRadiance)
		flags |= kExtSunPhysicalRadiance;
	if (s.sunHideVanillaGlare)
		flags |= kExtHideSunGlare;
	// fixed (47m): the five 37b fixes and Moon Physical Brightness (pinned on since 44h).
	flags |= kExtSkyAlphaOpaque | kExtTrLutEdgeFix | kExtApShadowDepthFix | kExtReflectionSkyFix | kExtMultiScatterFix;
	flags |= kExtMoonPhysicalRadiance;
	if (s.skyPrecision)
		flags |= kExtSkyPrecision;  // (47b)
	extCbData.flags = flags;
	extCbData.sunRadianceCap = std::clamp(s.sunRadianceCap, 1.f, 62250.f);
	extCbData.sunGlowIntensity = 0.f;  // (49h) the procedural sun is retired
	extCbData.sunGlowWidth = DirectX::XMConvertToRadians(std::clamp(s.sunGlowWidthDeg, 0.1f, 5.f));
}

void PhysicalSky::UpdateMoonData(float a_exposure, float a_sunElevation, float3& a_masserGlow, float3& a_secundaGlow)
{
	// (40d) The moon colours stay the user's (they light the scene through Sky Sync); only their
	// glow in the scattering LUTs is scaled here.
	const auto& s = effective;
	const float strength = std::clamp(s.moonGlowStrength, 0.f, 1.f);
	a_masserGlow = settings.masserColor * (a_exposure * strength);
	a_secundaGlow = settings.secundaColor * (a_exposure * strength);
	moonCbData = {};
	moonStarFadeAmount[0] = moonStarFadeAmount[1] = 0.f;  // (48b)
	moonPlanetshineAmount[0] = moonPlanetshineAmount[1] = 0.f;

	auto* sky = globals::game::sky;
	if (!sky)
		return;

	// fixed (47m, pinned since 44h): "Moon Glow Follows Phase and Visibility" on, "Moon Glow Fades" At Horizon.
	{
		// A new moon (or a moon the game has hidden or faded out) no longer lights the sky.
		// (41b, F2) The vanilla disc is faded out 16-28 degrees above the horizon (orbit angle
		// 145-160), long before the moon sets; the glow follows the moon down to it.
		const auto& skySync = globals::features::skySync;
		const float masserVisibility = MoonHorizonVisibility(sky->masser, sky, skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)].z);
		const float secundaVisibility = MoonHorizonVisibility(sky->secunda, sky, skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)].z);
		a_masserGlow = a_masserGlow * (SkySync::PhaseFactorFromTexture(sky->masser) * masserVisibility);
		a_secundaGlow = a_secundaGlow * (SkySync::PhaseFactorFromTexture(sky->secunda) * secundaVisibility);
	}

	// fixed (47m, pinned since 44h): "Moon Physical Brightness" on.
	{
		// Like the sun's Physical Brightness: the moonlight colour is an irradiance, the disc shows
		// radiance = irradiance / disc solid angle. The vanilla disc (phase picture, about 1 at
		// its brightest) is multiplied by that, never darkened, capped at moonRadianceCap.
		const float cap = std::clamp(s.moonRadianceCap, 1.f, 1000.f);
		auto discScale = [&](const RE::Moon* a_moon, const float3& a_color) {
			if (!a_moon || !a_moon->moonMesh || !sky->root)
				return 1.f;
			const float dist = (a_moon->moonMesh->world.translate - sky->root->world.translate).Length();
			// Bounding sphere of the square moon quad -> radius of the inscribed disc.
			const float radius = a_moon->moonMesh->worldBound.radius * 0.70710678f;
			if (!(radius > 0.f) || dist <= radius)
				return 1.f;
			const float sinR = radius / dist;
			const float solidAngle = 2.f * DirectX::XM_PI * (1.f - std::sqrt(1.f - sinR * sinR));
			const float irradiance = (0.2126f * a_color.x + 0.7152f * a_color.y + 0.0722f * a_color.z) * a_exposure;
			// (50b) x k_sky (with its cap: the disc follows the family; x 1.0f off).
			return std::clamp(irradiance / std::max(solidAngle, 1e-6f), 1.f, cap) * skyAnchorApplied;
		};
		moonCbData.masserDiskScale = discScale(sky->masser, settings.masserColor);
		moonCbData.secundaDiskScale = discScale(sky->secunda, settings.secundaColor);
	}

	UpdateMoonSky(a_exposure, a_sunElevation);
}

void PhysicalSky::UpdateMoonSky(float a_exposure, float a_sunElevation)
{
	// (48b) "Moonlight Fades Stars" and "Moon Planetshine". Off: no flag and zero values, Sky.hlsl runs the 48a code.
	moonStarFadeAmount[0] = moonStarFadeAmount[1] = 0.f;
	moonPlanetshineAmount[0] = moonPlanetshineAmount[1] = 0.f;
	moonCbData.starFadeHalves = 0;
	moonCbData.planetshineHalves = 0;
	const auto& s = effective;
	auto* sky = globals::game::sky;
	if (!sky || (!s.moonFadesStars && !s.moonPlanetshine))
		return;

	// Gone as the sun comes up, as the moonlight on the ground and clouds (sun -2 -> +6 degrees).
	const float dayFade = SkySync::MoonDayFade(DirectX::XMConvertToDegrees(a_sunElevation));
	const auto& skySync = globals::features::skySync;
	const RE::Moon* moons[2] = { sky->masser, sky->secunda };
	const float3* colors[2] = { &settings.masserColor, &settings.secundaColor };
	const float sinElevation[2] = { skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)].z,
		skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)].z };
	auto toHalves = [](const float a_low, const float a_high) {
		return static_cast<uint32_t>(DirectX::PackedVector::XMConvertFloatToHalf(a_low)) |
		       (static_cast<uint32_t>(DirectX::PackedVector::XMConvertFloatToHalf(a_high)) << 16);
	};

	if (s.moonFadesStars) {
		// The moonlit sky's brightness relative to the clear moonless night, as one drowning amount D per moon (the
		// shader adds them, each x (1 - cloud cover around that moon), and a star texel of brightness c0 keeps
		// saturate(1 - D / c0) of its alpha, as with Sky Fixes' own fade):
		//   D = strength x kMoonStarFadeScale x phase x horizon visibility (weather, -2..+3 degrees) x night x height
		//       weight x luminance of the full moonlight (light colour x exposure; Secunda x 0.67 as on the ground).
		// Height weight: the moon's light reaching the air through the atmosphere, relative to the zenith (Krisciunas &
		// Schaefer 1991 airmass, V-band extinction 0.172): 1 high up, 0.85 at 30 degrees, 0.5 at 10, 0.1 at 3.
		const float strength = std::clamp(s.moonStarFadeStrength, 0.f, 3.f);
		for (int i = 0; i < 2; ++i) {
			if (!moons[i] || !(strength > 0.f) || !(dayFade > 0.f))
				continue;
			const float cosZenith = std::max(sinElevation[i], 0.f);
			const float airmass = 1.f / (cosZenith + 0.025f * std::exp(-11.f * cosZenith));
			const float height = std::pow(10.f, -0.4f * kMoonSkyExtinction * (airmass - 1.f));
			const float3 light = *colors[i] * a_exposure;
			const float luminance = std::max(0.2126f * light.x + 0.7152f * light.y + 0.0722f * light.z, 0.f);
			const float amount = strength * kMoonStarFadeScale * SkySync::PhaseFactorFromTexture(moons[i]) *
			                     MoonHorizonVisibility(moons[i], sky, sinElevation[i]) * dayFade * height * luminance *
			                     (i == 1 ? SkySync::SecundaIntensityFactor : 1.f) * skyAnchorApplied;  // (50b) the moonlight in the scene's scale (x 1.0f off)
			// D >= 1 drowns every texel (c0 <= 1); the half keeps it exactly.
			moonStarFadeAmount[i] = std::isfinite(amount) ? std::clamp(amount, 0.f, 1.f) : 0.f;
		}
		moonCbData.starFadeHalves = toHalves(moonStarFadeAmount[0], moonStarFadeAmount[1]);
		nightCbData.flags |= kNightMoonFadesStars;
	}

	if (s.moonPlanetshine) {
		// (49j, design 9-3) Earthshine. The value is the dark side's brightness relative to kPlanetshineRatio x the lit disc: strength / 0.1
		// (the default 0.1 = the real ratio) x the Earth's phase seen from the moon (1 - the moon's lit fraction, from the phase picture) x night.
		// Sky.hlsl draws the dark side opaque (a real moon hides the stars behind it) with the sky behind plus this grey, sharp-edged and with
		// the picture's brightness structure. 48b: an opacity of strength x (1 - phase), a grey of 0.25 x the lit disc (10-20x too bright).
		const float strength = std::clamp(s.moonPlanetshineStrength, 0.f, 0.5f);
		for (int i = 0; i < 2; ++i) {
			if (!moons[i] || !(strength > 0.f) || !(dayFade > 0.f))
				continue;
			const float phase = SkySync::PhaseFactorFromTexture(moons[i]);
			if (phase <= SkySync::NewMoonIntensityFactor)
				continue;
			const float amount = (strength / 0.1f) * (1.f - phase) * dayFade;
			moonPlanetshineAmount[i] = std::isfinite(amount) ? std::clamp(amount, 0.f, 8.f) : 0.f;
		}
		moonCbData.planetshineHalves = toHalves(moonPlanetshineAmount[0], moonPlanetshineAmount[1]);
		nightCbData.flags |= kNightPlanetshine;
	}
}

std::string PhysicalSky::DescribeForTrace() const
{
	// (48b) Physical Sky's moon / star options, as in effect when the trace starts.
	if (!loaded)
		return "Physical Sky not loaded";
	const auto& s = effective;
	return std::format(
		"Physical Sky {} (sky fixes {}, moon sky glow {:.2f}); moonlight fades stars {} (strength {:.2f}, now Masser {:.4f} / Secunda {:.4f} before clouds); "
		"moon planetshine {} (strength {:.2f}, now Masser {:.3f} / Secunda {:.3f})",
		s.enabled && cbData.enabled ? "on" : (s.enabled ? "on (not drawn here)" : "off"), SkyFixesActive() ? "on" : "off", s.moonGlowStrength,
		s.moonFadesStars ? "on" : "off", s.moonStarFadeStrength, moonStarFadeAmount[0], moonStarFadeAmount[1],
		s.moonPlanetshine ? "on" : "off", s.moonPlanetshineStrength, moonPlanetshineAmount[0], moonPlanetshineAmount[1]) +
	       std::format("; stage 2 {} (one scale {}, k {:.3f}, clouds like ground {}, weather hides moon {}, star extinction {}{})",  // (50b)
			   settings.lightingStage2.enabled ? "on" : "off", Stage2On(settings.lightingStage2.skyAnchor) ? "on" : "off",
			   Stage2On(settings.lightingStage2.skyAnchor) ? skyAnchorK : 1.f, Stage2On(settings.lightingStage2.cloudsLikeGround) ? "on" : "off",
			   Stage2On(settings.lightingStage2.weatherHidesMoon) ? "on" : "off", Stage2On(settings.lightingStage2.starExtinction) ? "on" : "off",
			   skyLightWobble ? ", sky light wobble on" : "") +
	       "; " + DescribeOutdoorLight();  // (51a)
}

float PhysicalSky::MoonHorizonVisibility(const RE::Moon* a_moon, const RE::Sky* a_sky, float a_sinElevation)
{
	if (!a_moon || !a_moon->moonMesh || !a_sky)
		return 0.f;

	// The weather still hides the moon. SkyrimSE.exe 1.5.97 Moon::Update (0x1403AE337) hides the
	// whole moon while the blended Moon Glare colour (Sky+0x168, skyColor[kMoonGlare]) equals a
	// fixed colour at 0x1430135F0 (presumably black); other runtimes get only the black test below.
	// The disc is drawn in the Moon Glare colour, so the glow also ramps in over its first 10% (no
	// pop when a weather change brings the moon back).
	const RE::NiColor& glare = a_sky->skyColor[RE::TESWeather::ColorTypes::kMoonGlare];
	static const RE::NiColor* hideColor = (!REL::Module::IsVR() && REL::Module::get().version() == SKSE::RUNTIME_SSE_1_5_97) ?
	                                          reinterpret_cast<const RE::NiColor*>(REL::Offset(0x30135F0).address()) :
	                                          nullptr;
	if (hideColor && glare.red == hideColor->red && glare.green == hideColor->green && glare.blue == hideColor->blue)
		return 0.f;
	const float glarePeak = std::max({ glare.red, glare.green, glare.blue });
	const float weather = std::clamp(glarePeak / 0.1f, 0.f, 1.f);
	if (!(weather > 0.f))
		return 0.f;

	// The moon's true elevation (its orbit direction, without Sky Sync's altitude dip).
	const float elevationDeg = DirectX::XMConvertToDegrees(std::asin(std::clamp(a_sinElevation, -1.f, 1.f)));
	return weather * SkySync::SmoothStep(-2.f, 3.f, elevationDeg);
}

void PhysicalSky::UpdateNightBaseLight(float a_sunElevation)
{
	nightCbData.flags = 0;
	nightCbData.baseUpper = {};
	nightCbData.baseHorizon = {};
	nightCbData.baseLower = {};
	nightBaseUnfadedUpper = {};
	nightBaseUnfadedHorizon = {};
	nightBaseFade = 0.f;

	const float strength = std::clamp(effective.nightBaseLight, 0.f, 1.f);
	if (!(strength > 0.f))
		return;  // 0: no flag, the LUTs run exactly the 41a code
	// Gone once the sun's own twilight has taken over: full at -6 degrees, 0 at +4.
	const float fade = 1.f - SkySync::SmoothStep(-6.f, 4.f, DirectX::XMConvertToDegrees(a_sunElevation));
	// (47a) Sky Fixes measures the night base at full strength even after it has faded out.
	const bool keepUnfaded = SkyFixesActive();
	if (!(fade > 0.f) && !keepUnfaded)
		return;

	// The weather's night keyframes (sRGB 0-255), blended like the game blends weathers.
	using ColorTypes = RE::TESWeather::ColorTypes;
	auto nightColor = [](const RE::TESWeather* a_weather, int a_type) {
		const auto& c = a_weather->colorData[a_type][RE::TESWeather::ColorTime::kNight];
		return float3(c.red, c.green, c.blue) * (1.f / 255.f);
	};
	float3 upper = {}, horizon = {}, lower = {};
	if (auto* sky = globals::game::sky; sky && sky->currentWeather) {
		upper = nightColor(sky->currentWeather, ColorTypes::kSkyUpper);
		horizon = nightColor(sky->currentWeather, ColorTypes::kHorizon);
		lower = nightColor(sky->currentWeather, ColorTypes::kSkyLower);
		const float pct = std::clamp(sky->currentWeatherPct, 0.f, 1.f);
		if (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.f) {
			upper = float3::Lerp(nightColor(sky->lastWeather, ColorTypes::kSkyUpper), upper, pct);
			horizon = float3::Lerp(nightColor(sky->lastWeather, ColorTypes::kHorizon), horizon, pct);
			lower = float3::Lerp(nightColor(sky->lastWeather, ColorTypes::kSkyLower), lower, pct);
		}
	}

	// Airglow floor: many NAT night skies have SkyUpper = 0 (pure black zenith). A real moonless
	// sky never is; sRGB (8, 12, 20) is a deep navy, before the strength below.
	const float3 airglow = float3(8.f, 12.f, 20.f) * (1.f / 255.f);
	auto toLinear = [&](float3 c) {
		c = float3(std::max(c.x, airglow.x), std::max(c.y, airglow.y), std::max(c.z, airglow.z));
		// The same linearisation as the vanilla sky colours (Color::Sky in Color.hlsli).
		const auto& ll = globals::features::linearLighting.settings;
		if (ll.enableLinearLighting) {
			const float g = ll.skyGamma;
			c = float3(std::pow(c.x, g), std::pow(c.y, g), std::pow(c.z, g));
		}
		return c * (strength * fade);
	};
	if (keepUnfaded) {
		// (47a) The same colours at fade 1. Only Sky Fixes reads these.
		auto toLinearUnfaded = [&](float3 c) {
			c = float3(std::max(c.x, airglow.x), std::max(c.y, airglow.y), std::max(c.z, airglow.z));
			const auto& ll = globals::features::linearLighting.settings;
			if (ll.enableLinearLighting) {
				const float g = ll.skyGamma;
				c = float3(std::pow(c.x, g), std::pow(c.y, g), std::pow(c.z, g));
			}
			return c * strength;
		};
		nightBaseUnfadedUpper = toLinearUnfaded(upper);
		nightBaseUnfadedHorizon = toLinearUnfaded(horizon);
		nightBaseFade = std::max(fade, 0.f);
		if (!(fade > 0.f))
			return;  // faded out: no flag, as without Sky Fixes
	}
	nightCbData.baseUpper = toLinear(upper);
	nightCbData.baseHorizon = toLinear(horizon);
	nightCbData.baseLower = toLinear(lower);
	nightCbData.flags = kNightBaseLight;
}

bool PhysicalSky::GetAmbientProgress(float& a_g)
{
	a_g = 0.f;
	const auto* sky = globals::game::sky;
	if (!sky || !sky->currentWeather)
		return false;

	// The same linearisation the ambient gets in the shaders (Linear Lighting's ambient gamma).
	const auto& ll = globals::features::linearLighting.settings;
	const float gamma = ll.enableLinearLighting && std::isfinite(ll.ambientGamma) ? std::clamp(ll.ambientGamma, 0.1f, 5.f) : 1.f;
	auto luminance = [&](float r, float g, float b) {
		auto lin = [&](float v) { return std::pow(std::isfinite(v) ? std::max(v, 0.f) : 0.f, gamma); };
		return 0.2126f * lin(r) + 0.7152f * lin(g) + 0.0722f * lin(b);
	};

	// Now: the engine's own blend (time of day x weather transition) of the six directional ambient colours.
	float now = 0.f;
	for (int axis = 0; axis < 3; ++axis)
		for (int side = 0; side < 2; ++side) {
			const auto& c = sky->directionalAmbientColors[axis][side];
			now += luminance(c.red, c.green, c.blue);
		}
	now /= 6.f;

	// The night and day keys of the same weathers, blended with the same weather transition.
	const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.f, 1.f) : 1.f;
	const RE::TESWeather* last = (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.f) ? sky->lastWeather : nullptr;
	auto keyLuminance = [&](RE::TESWeather::ColorTime a_time) {
		auto channel = [](const RE::Color& c, int i) { return (i == 0 ? c.red : i == 1 ? c.green : c.blue) * (1.f / 255.f); };
		float sum = 0.f;
		for (int axis = 0; axis < 3; ++axis)
			for (int side = 0; side < 2; ++side) {
				auto colorOf = [&](const RE::TESWeather* w) -> const RE::Color& {
					const auto& d = w->directionalAmbientLightingColors[a_time].directional;
					const auto& mm = axis == 0 ? d.x : axis == 1 ? d.y : d.z;
					return side == 0 ? mm.max : mm.min;
				};
				const RE::Color& cur = colorOf(sky->currentWeather);
				float rgb[3];
				for (int i = 0; i < 3; ++i)
					rgb[i] = last ? std::lerp(channel(colorOf(last), i), channel(cur, i), pct) : channel(cur, i);
				sum += luminance(rgb[0], rgb[1], rgb[2]);
			}
		return sum / 6.f;
	};
	const float night = keyLuminance(RE::TESWeather::ColorTime::kNight);
	const float day = keyLuminance(RE::TESWeather::ColorTime::kDay);
	// Storms and odd weathers whose day ambient is not clearly above their night: no matching.
	if (!(day - night > 0.1f * std::max(day, night)) || !std::isfinite(now))
		return false;
	a_g = std::clamp((now - night) / (day - night), 0.f, 1.f);
	return true;
}

void PhysicalSky::UpdateSkyFixes(const bool a_fixes, const RE::NiPoint3& a_lutSunDir)
{
	nightCbData.skyScale = 1.f;
	nightCbData.starFadeStrength = 0.f;
	nightCbData.nightBaseUnfade = 0.f;
	skyFixesDebug = {};
	if (!a_fixes) {
		skyScaleSmoothed = -1.f;
		return;
	}

	nightCbData.flags |= kNightMidpoint | kNightGroundFix | kNightSkyFixes | kNightDistantSea | kNightWaterAp;  // (47b) + distant sea, water haze

	// Stars: their fade compares the sky with the night base light at full strength (needs the base light).
	if (std::clamp(effective.nightBaseLight, 0.f, 1.f) > 0.f) {
		nightCbData.starFadeStrength = kSkyFixesStarFade;
		nightCbData.nightBaseUnfade = nightBaseFade > 1e-6f ? 1.f / nightBaseFade : 0.f;
	}

	// ---- Sky brightness follows the ground ----
	// T = Zn + g (Zd - Zn): the zenith brightness that keeps the day's sky-to-ground ratio, where g is how far
	// the weather's ground ambient has gone from its night to its day key, Zn the night base light's zenith
	// and Zd the zenith with the sun at +12 degrees (where the day colours are full). The factor scales only
	// the sun's in-scatter Zs (47a2: not the moons' Zm), so it is the one that brings the zenith to T with the
	// night base Zb and the moon glow held: f = (T - Zb - Zm) / Zs, never below 1, at most kSkyFixesScaleCap (32: larger caps light a horizon band at
	// deep twilight that is brighter than later in the dawn; reaching T there would need x10000).
	float target = 1.f;
	auto& dbg = skyFixesDebug;
	float g = 0.f;
	if (GetAmbientProgress(g)) {
		if (UpdateCpuSkyModel()) {
			auto unit = [](float x, float y, float z) {
				const double l = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
				return l > 1e-9 ? PhysicalSkyModel::Vec3{ x / l, y / l, z / l } : PhysicalSkyModel::Vec3{ 0.0, 0.0, 1.0 };
			};
			auto vec = [](const float3& c) { return PhysicalSkyModel::Vec3{ c.x, c.y, c.z }; };
			const double cameraRadius = static_cast<double>(cbData.zCameraPlanet) * Util::Units::GAME_UNIT_TO_KM;
			const PhysicalSkyModel::Light lights[3] = {
				{ unit(a_lutSunDir.x, a_lutSunDir.y, a_lutSunDir.z), vec(cbData.sunlightColor) },
				{ unit(cbData.masserDir.x, cbData.masserDir.y, cbData.masserDir.z), vec(cbData.masserColor) },
				{ unit(cbData.secundaDir.x, cbData.secundaDir.y, cbData.secundaDir.z), vec(cbData.secundaColor) },
			};
			// (47a2) The sun and the moons apart: only the sun's share is scaled.
			const double zScatter = skyModelCpu.RingLuminance(cameraRadius, kSkyFixesRingElevationDeg, lights, 1);
			const double zMoon = skyModelCpu.RingLuminance(cameraRadius, kSkyFixesRingElevationDeg, lights + 1, 2);

			// The day reference: the same sun at +12 degrees (same azimuth), no moons, no night base.
			const double horizontal = std::sqrt(double(a_lutSunDir.x) * a_lutSunDir.x + double(a_lutSunDir.y) * a_lutSunDir.y);
			const double dayEl = DirectX::XMConvertToRadians(kSkyFixesDayElevationDeg);
			PhysicalSkyModel::Light day = lights[0];
			day.dir = horizontal > 1e-6 ? PhysicalSkyModel::Vec3{ a_lutSunDir.x / horizontal * std::cos(dayEl), a_lutSunDir.y / horizontal * std::cos(dayEl), std::sin(dayEl) } :
			                              PhysicalSkyModel::Vec3{ std::cos(dayEl), 0.0, std::sin(dayEl) };
			const double zDay = skyModelCpu.RingLuminance(cameraRadius, kSkyFixesRingElevationDeg, &day, 1);

			// The night base on the ring (LutGen's NightSkyBase gradient), as drawn now and at full strength.
			const float ringMix = SkySync::SmoothStep(0.f, 0.5f, std::sin(DirectX::XMConvertToRadians(kSkyFixesRingElevationDeg)));
			auto ringLuminance = [&](const float3& a_upper, const float3& a_horizon) {
				const float3 c = a_horizon + (a_upper - a_horizon) * ringMix;
				return 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
			};
			const bool baseOn = (nightCbData.flags & kNightBaseLight) != 0;
			const double zBase = baseOn ? ringLuminance(nightCbData.baseUpper, nightCbData.baseHorizon) : 0.0;
			const double zNight = ringLuminance(nightBaseUnfadedUpper, nightBaseUnfadedHorizon);

			const double t = zNight + g * (zDay - zNight);
			const double need = t - zBase - zMoon;
			if (std::isfinite(need) && std::isfinite(zScatter)) {
				if (!(need > 0.0))
					target = 1.f;
				else if (!(zScatter * kSkyFixesScaleCap > need))
					target = kSkyFixesScaleCap;
				else
					target = static_cast<float>(std::clamp(need / zScatter, 1.0, static_cast<double>(kSkyFixesScaleCap)));
				dbg.valid = true;
			}
			dbg.g = g;
			dbg.zScatter = static_cast<float>(zScatter);
			dbg.zMoon = static_cast<float>(zMoon);
			dbg.zBase = static_cast<float>(zBase);
			dbg.zNight = static_cast<float>(zNight);
			dbg.zDay = static_cast<float>(zDay);
		}
	}
	if (!std::isfinite(target))
		target = 1.f;
	dbg.target = target;

	// About 1.5 s of smoothing against weather changes and lightning; a jump on time skips, loads, a stopped
	// clock, a new climate or sun path, and whenever the sky (re)appears.
	const auto calendar = RE::Calendar::GetSingleton();
	const float hours = calendar ? calendar->GetHoursPassed() : 0.f;
	const float timeScale = calendar ? calendar->GetTimescale() : 0.f;
	const uint32_t warp = globals::features::skySync.GetSunWarpGeneration();
	const bool snap = skyScaleSmoothed < 0.f || !(timeScale > 0.f) || !(std::abs(hours - skyScaleLastHours) < 0.01f) || warp != skyScaleLastWarp;
	skyScaleLastHours = hours;
	skyScaleLastWarp = warp;
	if (snap) {
		skyScaleSmoothed = target;
	} else {
		const float dt = globals::game::deltaTime ? std::clamp(*globals::game::deltaTime, 0.f, 1.f) : 0.f;
		skyScaleSmoothed += (target - skyScaleSmoothed) * (1.f - std::exp(-dt / kSkyFixesSmoothingSeconds));
	}
	if (!std::isfinite(skyScaleSmoothed))
		skyScaleSmoothed = 1.f;
	nightCbData.skyScale = std::clamp(skyScaleSmoothed, 1.f, kSkyFixesScaleCap);
}

void PhysicalSky::UpdateCloudData(float a_exposure, float a_sunElevation)
{
	cloudCbData = {};
	const auto& s = effective;

	// fixed (47m, pinned since 44h): "Moons Fade in Daylight" on.
	cloudCbData.flags |= kCloudMoonDaylightFade;
	cloudCbData.moonDaylightFade = std::clamp(s.moonDaylightFade, 0.f, 100.f);

	if (s.cloudLighting != kCloudLightingAll)
		return;  // no flag: Sky.hlsl runs the 42a RelightCloud exactly
	cloudCbData.flags |= kCloudMultiLight;

	// The same scale the directional light reaches the shaders with (State::UpdateSharedData):
	// colour x exposure / PBR compensation (as overrideDirLight writes it) x the image space's
	// sunlight scale. At noon the sun on the clouds is then exactly what the old single light gave.
	const float pbrCompensation = globals::features::linearLighting.settings.enableLinearLighting ? 1.f : RE::NI_PI;
	float sunlightScale = 1.f;
	if (auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton())
		sunlightScale = !globals::game::isVR ? imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale : imageSpaceManager->GetVRRuntimeData().data.baseData.hdr.sunlightScale;
	if (!std::isfinite(sunlightScale) || sunlightScale < 0.f)
		sunlightScale = 1.f;
	const float scale = a_exposure * sunlightScale / pbrCompensation;

	// The sun: no horizon factor here. Sky.hlsl applies the transmittance and the planet's shadow
	// at the cloud, which is what keeps high clouds lit after sunset.
	cloudCbData.sunColor = settings.sunlightColor * scale;

	// The moons: phase, the true elevation with the weather (the 41b glow visibility) and, as for the
	// ground light in Sky Sync's Combined model, gone as the sun comes up (the moonlight colours are
	// far above a real moon's, so by day they would brighten the clouds).
	if (auto* sky = globals::game::sky) {
		const auto& skySync = globals::features::skySync;
		const float dayFade = SkySync::MoonDayFade(DirectX::XMConvertToDegrees(a_sunElevation));
		const float masser = SkySync::PhaseFactorFromTexture(sky->masser) *
		                     MoonHorizonVisibility(sky->masser, sky, skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)].z) * dayFade;
		const float secunda = SkySync::PhaseFactorFromTexture(sky->secunda) * SkySync::SecundaIntensityFactor *
		                      MoonHorizonVisibility(sky->secunda, sky, skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)].z) * dayFade;
		cloudCbData.masserColor = settings.masserColor * (scale * masser);
		cloudCbData.secundaColor = settings.secundaColor * (scale * secunda);
	}

	// (44h) "Moonlight on Clouds": Sky.hlsl takes the moons out of RelightCloudMulti and adds them
	// after the weather's cloud colour (CloudMoonLight).
	if (s.moonCloudLight) {
		cloudCbData.flags |= kCloudMoonDirect;
		cloudCbData.moonCloudStrength = std::clamp(s.moonCloudStrength, 0.f, 3.f);
		cloudCbData.moonCloudHalo = std::clamp(s.moonCloudHalo, 0.f, 4.f);
		cloudCbData.moonCloudSaturation = 0.35f;
	}

	cloudCbData.skyAmbient = std::clamp(s.cloudSkyAmbient, 0.f, 4.f);
	// (50b) Stage 2 "Clouds Lit Like the Ground" (needs Moonlight on Clouds): Sky.hlsl takes the moons' light as the ground gets it (the
	// moonlight colour x VDM-equivalent, its own colour, the cloud's alpha once) and adds the ground's ambient (the weather DALC, as Lighting
	// converts it) x the cloud texture x the weather cloud colour's hue after the (near-black at night) weather cloud colour. The sky-view LUT
	// term of RelightCloudMulti is switched off by its strength 0; the user's Cloud Sky Ambient goes to Sky.hlsl in the moonCloudSaturation
	// field (that field's greying is not used on this path).
	if (s.moonCloudLight && Stage2On(settings.lightingStage2.cloudsLikeGround)) {
		cloudCbData.flags |= kCloudLikeGround;
		cloudCbData.moonCloudSaturation = cloudCbData.skyAmbient;
		cloudCbData.skyAmbient = 0.f;
	}
	// Cloud Shadows' layer is 0.5 km above the camera's ground; only the extra goes in.
	cloudCbData.heightOffset = (std::clamp(s.cloudLightHeightKm, 0.5f, 12.f) - 0.5f) / Util::Units::GAME_UNIT_TO_KM;
}

void PhysicalSky::UpdateEffective()
{
	effective = settings;
	// (47m) The 44h pins (five atmosphere fixes, six celestial options) are no longer fields: the code always
	// takes their "on" side. "Sky Model: Legacy (36f)" is gone.
	// (47a) Sky Fixes takes over Twilight Length (and Day/Night Exposure with the adaptation angles,
	// see Reset()): treated as 1 / off. The saved values are kept.
	if (SkyFixesActive())
		effective.twilightLength = 1.f;
}

void PhysicalSky::Reset()
{
	UpdateEffective();
	const auto& s = effective;
	UpdateExtCbData();

	auto& skySync = globals::features::skySync;
	skySync.lightColors = std::nullopt;

	auto& linearLighting = globals::features::linearLighting;

	bool allGood = settings.enabled && ShadersOK() && skySync.loaded && skySync.settings.Enabled;

	// check worldspace
	bool inMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));

	// (batch 37b) Whitelist (saved, editable, DLC exteriors added) / all exteriors / exclusions.
	WorldspaceInfo worldspaceInfo = {};
	const auto worldspaceStatus = GetWorldspaceStatus(worldspaceInfo.zBottom);
	const bool worldspaceEnabled = worldspaceStatus == WorldspaceStatus::Whitelist || worldspaceStatus == WorldspaceStatus::AllExteriors;
	allGood &= worldspaceEnabled && !inMainLoadingMenu;

	if (!allGood) {
		if (skySync.loaded && skySync.settings.Enabled)
			skySync.lightColors = std::nullopt;
		skyScaleSmoothed = -1.f;  // (47a) snap when the sky comes back
		skyAnchorSnap = true;  // (50b) likewise
		skyAnchorApplied = 1.f;
		HistogramAutoExposure::freezeAdaptation = false;
		cbData.enabled = allGood;
		horizonFixCbData = {};  // (47f)
		horizonFixCbData.flags = PhysLightingFlags();  // (49h stage 1) the rules that do not need the physical sky still apply (interiors)
		cloudCbData = {};
		linearLighting.isDirLightLinear = false;
		return;
	}

	// resolution
	float2 res = globals::state->screenSize;
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto sunDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	// (41b, F4a) The scattering LUTs' sun. Without sunAlignToVanilla it is sunDir, as before.
	RE::NiPoint3 lutSunDir = sunDir;
	// (batch 37b) Align with the vanilla sun quad. Sky Sync places it (and points the sun light)
	// along the apparent direction -- dipped by atan(altitude / 325000), 1.5-4 degrees -- in the
	// sky root's local frame, which Sky Sync rotates by the cell's north rotation. The raw
	// direction used before sat that far off the quad: a second, clipped or missing disk.
	if constexpr (Settings::sunAlignToVanilla) {  // (49h) fixed on
		RE::NiPoint3 apparent = skySync.directions[static_cast<int>(SkySync::Caster::Sun)];
		RE::NiPoint3 trueDir = sunDir;
		if (auto* sky = globals::game::sky; sky && sky->root) {
			apparent = sky->root->world.rotate * apparent;
			trueDir = sky->root->world.rotate * trueDir;
		}
		if (apparent.Unitize() > FLT_EPSILON)
			sunDir = apparent;
		lutSunDir = sunDir;
		// (41b, F4a) The dip is a drawing correction for the disc (the game's sky dome is centred on
		// the camera, so a higher camera sees the sun lower). Fed to the LUTs it made the whole sky
		// think the sun was 1.5-6 degrees lower: dawn 7-25 minutes late, dusk as early. The LUTs
		// now get the true height in the same (world) frame; the disc and its glow keep the dip.
		// fixed (47m, pinned since 44h): "Sky Uses True Sun Height" on.
		if (trueDir.Unitize() > FLT_EPSILON)
			lutSunDir = trueDir;
	}
	// (41b, F4b) Longer twilight, LUTs only: a sun below the horizon is taken as elevation / L.
	if (const float twilight = std::clamp(s.twilightLength, 1.f, 3.f); twilight > 1.f && lutSunDir.z < 0.f) {
		const float horizontal = std::sqrt(lutSunDir.x * lutSunDir.x + lutSunDir.y * lutSunDir.y);
		if (horizontal > 1e-6f) {
			const float elevation = std::asin(std::clamp(lutSunDir.z, -1.f, 1.f)) / twilight;
			const float scale = std::cos(elevation) / horizontal;
			lutSunDir = { lutSunDir.x * scale, lutSunDir.y * scale, std::sin(elevation) };
		}
	}
	nightCbData.lutSunDir = { lutSunDir.x, lutSunDir.y, lutSunDir.z };
	// (41b, F1) Faded by the sun height the sky itself uses.
	UpdateNightBaseLight(std::asin(std::clamp(lutSunDir.z, -1.f, 1.f)));
	auto masserDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)];
	auto secundaDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)];
	// (40d) With the discs left where the game puts them, the glow follows each disc's on-screen
	// direction (world frame, like the aligned sun above; upstream b2d671ba8's idea). The raw
	// direction is in the sky root's local frame, so it missed the disc by the north rotation and
	// the altitude dip. fixed (47m, pinned since 44h): Sky Sync's "Keep Moons Where the Game Puts Them" on.
	{
		if (auto* sky = globals::game::sky) {
			RE::NiPoint3 onScreen;
			if (SkySync::OnScreenDirection(sky->masser, sky, onScreen))
				masserDir = onScreen;
			if (SkySync::OnScreenDirection(sky->secunda, sky, onScreen))
				secundaDir = onScreen;
		}
	}

	float sunAngle = DirectX::XMConvertToRadians(90.f) - acos(sunDir.z);
	float adaptAmount = (sunAngle - settings.adaptationStart) / (settings.adaptationEnd - settings.adaptationStart);
	adaptAmount = std::min(1.f, std::max(0.f, adaptAmount));
	float exposure = settings.dayExposure * exp(log(settings.nightExposure / settings.dayExposure) * adaptAmount);
	// (47a) Sky Fixes: no Day/Night Exposure and no adaptation (the brightness of dawn and dusk comes
	// from the sky brightness factor instead).
	const bool skyFixes = SkyFixesActive();
	if (skyFixes)
		exposure = 1.f;

	// (50b) Stage 2: the factor this frame's sky-family uploads carry: k_sky (smoothed, from the frames before) x the Sky Light Wobble; exactly 1
	// with the master or the anchor off (then every upload below is the 50a value: x 1.0f is exact).
	const bool stage2 = Stage2On(true);
	const bool anchor = Stage2On(settings.lightingStage2.skyAnchor);
	{
		float applied = anchor ? skyAnchorK : 1.f;
		if (stage2 && skyLightWobble) {
			const double t = static_cast<double>(GetTickCount64()) * 1e-3 - skyLightWobbleStart;
			applied *= static_cast<float>(std::exp2(std::sin(2.0 * 3.14159265358979 * t / 4.0)));
		}
		skyAnchorApplied = applied;
		HistogramAutoExposure::freezeAdaptation = stage2 && skyLightWobble;  // the eye holds still while the sky family pulses
	}
	if (!anchor)
		skyAnchorSnap = true;  // snap when it comes back

	float3 masserGlow, secundaGlow;
	// (48b) The sun's true elevation, as for the clouds just below.
	UpdateMoonData(exposure, std::asin(std::clamp(skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)].z, -1.f, 1.f)), masserGlow, secundaGlow);
	UpdateCloudData(exposure, std::asin(std::clamp(skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)].z, -1.f, 1.f)));
	if (skyAnchorApplied != 1.f) {
		// (50b) The clouds' sun and moons (DirLightColor units) with the family.
		cloudCbData.sunColor = cloudCbData.sunColor * skyAnchorApplied;
		cloudCbData.masserColor = cloudCbData.masserColor * skyAnchorApplied;
		cloudCbData.secundaColor = cloudCbData.secundaColor * skyAnchorApplied;
		extCbData.sunRadianceCap = extCbData.sunRadianceCap * skyAnchorApplied;  // (no shader reads it since 49h; kept consistent)
	}

	// (47a) Sky Fixes: the ozone layer moved to 10-40 km, keeping the user's absorption colour and the
	// column (peak x thickness / 2). Only the upload changes; the saved settings keep their values.
	float3 ozoneAbsorption = settings.ozoneAbsorption;
	float ozoneAltitude = settings.ozoneAltitude;
	float ozoneThickness = settings.ozoneThickness;
	if (skyFixes) {
		const float thickness = std::isfinite(settings.ozoneThickness) ? std::max(settings.ozoneThickness, 0.f) : 0.f;
		ozoneAbsorption = settings.ozoneAbsorption * (thickness / kOzoneFixThickness);
		ozoneAltitude = kOzoneFixAltitude;
		ozoneThickness = kOzoneFixThickness;
	}

	// (47b) "Aerosol": a preset replaces the aerosol optics in the upload only; the saved values are kept.
	float3 aerosolScatter, aerosolAbsorption;
	float aerosolPhaseG = settings.aerosolPhaseG;
	GetUploadedAerosol(aerosolScatter, aerosolAbsorption, aerosolPhaseG);

	cbData = {
		.texDim = res,
		.rcpTexDim = float2(1.0f) / res,
		.frameDim = dynres,
		.rcpFrameDim = float2(1.0f) / dynres,
		.sunDir = { sunDir.x, sunDir.y, sunDir.z },
		.sunlightColor = settings.sunlightColor * exposure * skyAnchorApplied,  // (50b) x k_sky (x 1.0f off)
		.trMix = settings.trMix,
		.masserDir = { masserDir.x, masserDir.y, masserDir.z },
		.apLumMix = settings.apLumMix,
		.masserColor = masserGlow * skyAnchorApplied,
		.apTrMix = settings.apTrMix,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.sunDiskCos = 0.f,  // (49h) the procedural sun is retired (field kept, SharedData unchanged)
		.secundaColor = secundaGlow * skyAnchorApplied,
		.enabled = allGood,
		.tonemapper = linearLighting.settings.enableLinearLighting ? 0 : settings.tonemapper,
		.vanillaMix = settings.vanillaMix,
		.zBottom = worldspaceInfo.zBottom,
		.rPlanet = settings.planetRadius / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = settings.atmosphereRadius / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = settings.groundAlbedo,
		.cloudShadowRemapRange = settings.cloudShadowRemapRange,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		.aerosolPhaseG = aerosolPhaseG,
		.aerosolScatter = aerosolScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.aerosolAbsorption = aerosolAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.rayleighFalloff = settings.rayleighFalloff * Util::Units::GAME_UNIT_TO_KM,
		.rayleighScatter = settings.rayleighScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.ozoneAltitude = ozoneAltitude / Util::Units::GAME_UNIT_TO_KM,
		.ozoneThickness = ozoneThickness / Util::Units::GAME_UNIT_TO_KM,
		.ozoneAbsorption = ozoneAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.cloudRelightMix = settings.cloudRelightMix,
		.cloudOriginalMix = settings.cloudOriginalMix,
		.silverLiningMix = settings.silverLiningMix,
		.silverLiningSpread = settings.silverLiningSpread,
	};

	if (settings.overrideDirLight) {
		linearLighting.isDirLightLinear = true;
		const float pbrCompensationMult = linearLighting.settings.enableLinearLighting ? 1.0f : RE::NI_PI;  // Colors should match PBR values when not using linear lighting
		auto LightConvFn = [pbrCompensationMult](float3 color) {
			color /= pbrCompensationMult;
			return RE::NiColor(color.x, color.y, color.z);
		};
		// (40d) The moonlight keeps the full colour: the glow strength only scales the sky LUTs.
		// (50b) The moons' light x k_sky too (the sun's through cbData.sunlightColor).
		skySync.lightColors = { LightConvFn(cbData.sunlightColor), LightConvFn(settings.masserColor * exposure * skyAnchorApplied),
			LightConvFn(settings.secundaColor * exposure * skyAnchorApplied) };
	} else {
		linearLighting.isDirLightLinear = false;
	}

	RE::NiPoint3 posCam = { 0, 0, 0 };
	if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		posCam = cam->cameraRoot->world.translate;
		cbData.zCameraPlanet = posCam.z - cbData.zBottom + cbData.rPlanet;
	}

	// (47a) Sky brightness factor, LUT flags and star values (all neutral when off).
	UpdateSkyFixes(skyFixes, lutSunDir);
	if (anchor) {
		// (50b) k_sky replaces Sky Fixes' zenith-only factor (the sun's in-scatter now carries k through cbData.sunlightColor), and is
		// updated for the next frames from this frame's values before k.
		nightCbData.skyScale = 1.f;
		UpdateSkyAnchor(lutSunDir, settings.sunlightColor * exposure, masserGlow, secundaGlow, exposure);
	}
	if (Stage2On(settings.lightingStage2.starExtinction))
		nightCbData.flags |= kNightStarExtinction;  // (50b)
	if (settings.waterSunTransmittance)
		nightCbData.flags |= kNightWaterSunTr;  // (49h)

	// fixed (47m): Horizon Fix removed; anchorOffset, altitude and flags stay 0 (as with Horizon Fix off).
	horizonFixCbData = {};
	horizonFixCbData.flags = PhysLightingFlags();  // (49h stage 1) Physical Lighting rule bits (flags was unused since 47m)
	auto& heightFog = globals::features::exponentialHeightFog;
	// (47g) EHF "Physical Fog Brightness": the clear sky's light on the fog (0 = off; independent of the Horizon Fix).
	if (heightFog.loaded && heightFog.vanillaFog.PhysicalFogBrightness)
		horizonFixCbData.fogSkyLight = ComputeFogSkyLight(lutSunDir);
	else
		fogSkyLightFrame = UINT32_MAX;
}

void PhysicalSky::EarlyPrepass()
{
	if (cbData.enabled) {
		UpdateApLutSize();  // (47a) before anything binds texApLut this frame
		GenerateLuts();
	}
}

void PhysicalSky::ReflectionsPrepass()
{
	if (cbData.enabled) {
		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
	}
}

void PhysicalSky::Prepass()
{
	if (cbData.enabled) {
		AccumShadow();

		std::array srvs = { texTrLut->srv.get(), texSvLut->srv.get(), texApLut->srv.get(), texApShadow->srv.get() };
		globals::d3d::context->PSSetShaderResources(61, (uint)srvs.size(), srvs.data());
	}
}

void PhysicalSky::GenerateLuts()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	constexpr auto debugStr = "Physical Sky: LUT Generation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto samplers = std::array{ sampTr.get(), sampSv.get(), sampNoise.get() };
		std::array<ID3D11ShaderResourceView*, 2> srvs = {};
		ID3D11UnorderedAccessView* uav = nullptr;

		Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::PhysicalSkyLuts);

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());

		// -> transmittance
		uav = texTrLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csTrLutGen.get(), nullptr, 0);
		context->Dispatch((kTrLutW + 7) >> 3, (kTrLutH + 7) >> 3, 1);

		// -> multiscatter
		uav = texMsLut->uav.get();
		srvs.at(0) = texTrLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csMsLutGen.get(), nullptr, 0);
		context->Dispatch((kMsLutW + 7) >> 3, (kMsLutH + 7) >> 3, 1);

		// -> sky-view
		uav = texSvLut->uav.get();
		srvs.at(1) = texMsLut->srv.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(csSvLutGen.get(), nullptr, 0);
		context->Dispatch((kSvLutW + 7) >> 3, (kSvLutH + 7) >> 3, 1);

		// -> aerial perspective
		uav = texApLut->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csApLutGen.get(), nullptr, 0);
		context->Dispatch((apLutW + 7) >> 3, (apLutH + 7) >> 3, 1);  // (47a) 32x32 or 64x64

		Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::PhysicalSkyLuts);

		/* ---- RESTORE ---- */
		samplers.fill(nullptr);
		srvs.fill(nullptr);
		uav = nullptr;

		context->CSSetSamplers(0, (int)samplers.size(), samplers.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void PhysicalSky::AccumShadow()
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	auto deferred = globals::deferred;
	if (!deferred)
		return;
	auto& terrainShadows = globals::features::terrainShadows;
	auto& cloudShadows = globals::features::cloudShadows;

	float2 size = Util::ConvertToDynamic(state->screenSize);
	uint resolution[2] = { (uint)size.x, (uint)size.y };

	constexpr auto debugStr = "Physical Sky: Shadow Accumulation";
	state->BeginPerfEvent(debugStr);
	{
		TracyD3D11Zone(state->tracyCtx, debugStr);

		auto sampler = sampTr.get();
		auto srvs = std::array{
			globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV,
			// (47z) The shared capture: with Ray Tracing's Keep Buildings in the Game's Sun Shadow Map the merged cascades (else the
			// same array as shadowView).
			deferred->HasFreshShadowCapture() ? deferred->capturedShadowMap.get() : deferred->shadowView,
			deferred->perShadow->srv.get(),
			terrainShadows.IsHeightMapReady() ? terrainShadows.texShadowHeight->srv.get() : nullptr,
			cloudShadows.loaded ? cloudShadows.texCubemapCloudOcc->srv.get() : nullptr,
		};
		auto uav = texApShadow->uav.get();

		Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::PhysicalSkyShadowAccum);

		/* ---- DISPATCH ---- */
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(csShadowAccum.get(), nullptr, 0);
		// texApShadow is half resolution (see Draw()/SetupResources: Width/Height /= 2) and
		// ShadowAccum.cs.hlsl addresses it as such -- SV_DispatchThreadID is a half-res texel,
		// scaled back to a full-res UV by `* rcpFrameDim * 2`. Consumers read TexApShadow[px / 2].
		// So the dispatch domain is ceil(renderExtent / 2), not the full render extent: the group
		// count is ceil(ceil(res / 2) / 8) == ceil(res / 16). Dispatching the full extent launched
		// 4x the required threads, and the surplus ran all 30 shadow steps before having its
		// out-of-bounds UAV write discarded.
		context->Dispatch((resolution[0] + 15u) >> 4, (resolution[1] + 15u) >> 4, 1);

		Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::PhysicalSkyShadowAccum);

		/* ---- RESTORE ---- */
		sampler = nullptr;
		srvs.fill(nullptr);
		uav = nullptr;

		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShaderResources(0, (int)srvs.size(), srvs.data());
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();
}

void PhysicalSky::ModifySky()
{
	auto context = globals::d3d::context;
	context->PSGetSamplers(3, 2, originalPSSamplers);

	auto samplers = std::array{ sampTr.get(), sampSv.get() };
	context->PSSetSamplers(3, static_cast<UINT>(samplers.size()), samplers.data());

	GET_INSTANCE_MEMBER(PSSamplerModifiedBits, globals::game::shadowState);
	PSSamplerModifiedBits |= (1 << 3);
}

void PhysicalSky::RestoreSamplers()
{
	auto context = globals::d3d::context;
	context->PSSetSamplers(3, 2, originalPSSamplers);

	GET_INSTANCE_MEMBER(PSSamplerModifiedBits, globals::game::shadowState);
	PSSamplerModifiedBits &= ~(1 << 3);
}

void PhysicalSky::SetSunDrawFlags(const RE::BSRenderPass* a_pass)
{
	auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
	descriptor &= ~(static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSun) | static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSunGlare) |
	                static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon) | static_cast<uint32_t>(State::ExtraShaderDescriptors::IsMoon) |
	                static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSecunda));

	if (!a_pass || !a_pass->shaderProperty)
		return;
	const auto* skyProperty = static_cast<const RE::BSSkyShaderProperty*>(a_pass->shaderProperty);
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN)
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSun);
	else if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN_GLARE)
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSunGlare);
	else if ((skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON ||
				 skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON_SHADOW) &&
			 IsNewMoonDraw(a_pass))  // fixed (47m, pinned since 44h): "Hide New Moon Disc" on
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon);

	// (40d) The moon disc itself (not its star mask, SO_MOON_SHADOW), for "Moon Physical Brightness"
	// and (42b) "Moons Fade in Daylight" (both fixed on, 47m). Sky.hlsl checks each option's own flag.
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON) {
		if (const auto sky = RE::Sky::GetSingleton(); sky && a_pass->geometry) {
			const auto* geometry = static_cast<const void*>(a_pass->geometry);
			if (sky->masser && geometry == sky->masser->moonMesh.get())
				descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsMoon);
			else if (sky->secunda && geometry == sky->secunda->moonMesh.get())
				descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsMoon) | static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSecunda);
		}
	}
}

bool PhysicalSky::IsNewMoonDraw(const RE::BSRenderPass* a_pass)
{
	// (batch 37c) Which moon this draw belongs to (its disc or its star mask), and whether that
	// moon currently shows its new-moon texture. The phase is read from the texture bound to the
	// moon's own disc -- the same way Sky Sync reads it -- so mods that drive the phases (Moon and
	// Stars) are followed too. Up to 4 draws a frame; the name test is a few dozen bytes.
	const auto sky = RE::Sky::GetSingleton();
	if (!sky || !a_pass->geometry)
		return false;
	for (const RE::Moon* moon : { sky->masser, sky->secunda }) {
		if (!moon || !moon->moonMesh)
			continue;
		const auto* geometry = static_cast<const void*>(a_pass->geometry);
		if (geometry != moon->moonMesh.get() && geometry != moon->shadowMesh.get())
			continue;
		const auto property = skyrim_cast<RE::BSSkyShaderProperty*>(moon->moonMesh->GetGeometryRuntimeData().properties[1].get());
		const auto texture = property ? property->GetBaseTexture() : nullptr;
		const char* name = texture ? texture->name.c_str() : nullptr;
		if (!name)
			return false;
		// "..._new.dds" (vanilla and Moon and Stars naming); case-insensitive.
		std::string_view view(name);
		const auto dot = view.rfind('.');
		const auto stem = view.substr(0, dot);
		return stem.size() >= 4 && _strnicmp(stem.data() + stem.size() - 4, "_new", 4) == 0;
	}
	return false;
}

void PhysicalSky::ClearSunDrawFlags()
{
	globals::state->permutationData.ExtraShaderDescriptor &=
		~(static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSun) | static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSunGlare) |
	                static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon) | static_cast<uint32_t>(State::ExtraShaderDescriptors::IsMoon) |
	                static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSecunda));
}

void PhysicalSky::Hooks::BSSkyShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.ModifySky();
	SetSunDrawFlags(Pass);
	globals::features::physicalSky.BindGlareDepth(Pass);  // (47a)
	func(This, Pass, RenderFlags);
}

void PhysicalSky::Hooks::BSSkyShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.RestoreSamplers();
	globals::features::physicalSky.UnbindGlareDepth();  // (47a)
	ClearSunDrawFlags();
	func(This, Pass, RenderFlags);
}

void PhysicalSky::BindGlareDepth(const RE::BSRenderPass* a_pass)
{
	// (47a) Sky Fixes: the vanilla sun glare fades with how much of the sun's neighbourhood is open sky
	// (upstream 6491de101's 16-tap ridge test; the game's own occlusion query sees gaps between distant
	// ridges). Main view only; a depth texture that is not the bound depth target goes to PS t65 for this
	// one draw, and whatever was there is put back in RestoreGeometry.
	if (glareDepthBound)
		UnbindGlareDepth();
	if (!cbData.enabled || !SkyFixesActive() || !a_pass || !a_pass->shaderProperty)
		return;
	const auto* skyProperty = static_cast<const RE::BSSkyShaderProperty*>(a_pass->shaderProperty);
	if (skyProperty->uiSkyObjectType != RE::BSSkyShaderProperty::SkyObject::SO_SUN_GLARE)
		return;
	if (!(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld)) ||
		(globals::state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections)))
		return;
	auto* renderer = globals::game::renderer;
	auto* context = globals::d3d::context;
	if (!renderer || !context)
		return;

	const auto& depthStencils = renderer->GetDepthStencilData().depthStencils;
	const auto& main = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	const auto& copy = depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	ID3D11ShaderResourceView* srv = main.depthSRV;
	{
		// Never the texture the draw itself depth-tests against (D3D11 would null the view).
		ID3D11DepthStencilView* dsv = nullptr;
		context->OMGetRenderTargets(0, nullptr, &dsv);
		if (dsv) {
			ID3D11Resource* bound = nullptr;
			dsv->GetResource(&bound);
			if (bound && bound == static_cast<ID3D11Resource*>(main.texture))
				srv = copy.texture != main.texture ? copy.depthSRV : nullptr;
			if (bound)
				bound->Release();
			dsv->Release();
		}
	}
	if (!srv)
		return;

	glarePreviousSrv = nullptr;
	context->PSGetShaderResources(kGlareDepthSlot, 1, &glarePreviousSrv);  // AddRef'd; released in Unbind
	context->PSSetShaderResources(kGlareDepthSlot, 1, &srv);
	glareDepthBound = true;
}

void PhysicalSky::UnbindGlareDepth()
{
	if (!glareDepthBound)
		return;
	glareDepthBound = false;
	if (auto* context = globals::d3d::context)
		context->PSSetShaderResources(kGlareDepthSlot, 1, &glarePreviousSrv);
	if (glarePreviousSrv) {
		glarePreviousSrv->Release();
		glarePreviousSrv = nullptr;
	}
}

/// (47a, 47g) The CPU copy of the scattering model with the atmosphere as uploaded (Sky Fixes, Sky Precision, Aerosol).
bool PhysicalSky::UpdateCpuSkyModel()
{
	const auto& s = settings;
	PhysicalSkyModel::Atmosphere atmosphere;
	atmosphere.rPlanet = s.planetRadius;
	atmosphere.rAtmosphere = s.atmosphereRadius;
	atmosphere.rayleighScatter = { s.rayleighScatter.x * 1e-3, s.rayleighScatter.y * 1e-3, s.rayleighScatter.z * 1e-3 };
	atmosphere.rayleighFalloff = s.rayleighFalloff;
	// (47b) As uploaded: the Aerosol preset's values when one is picked.
	float3 aerosolScatter, aerosolAbsorption;
	float aerosolPhaseG = 0.f;
	GetUploadedAerosol(aerosolScatter, aerosolAbsorption, aerosolPhaseG);
	atmosphere.aerosolScatter = { aerosolScatter.x * 1e-3, aerosolScatter.y * 1e-3, aerosolScatter.z * 1e-3 };
	atmosphere.aerosolAbsorption = { aerosolAbsorption.x * 1e-3, aerosolAbsorption.y * 1e-3, aerosolAbsorption.z * 1e-3 };
	atmosphere.aerosolFalloff = s.aerosolFalloff;
	atmosphere.aerosolPhaseG = aerosolPhaseG;
	// As uploaded (Reset): the moved ozone layer.
	const double ozoneScale = std::isfinite(s.ozoneThickness) ? std::max(s.ozoneThickness, 0.f) / kOzoneFixThickness : 0.0;
	atmosphere.ozoneAltitude = kOzoneFixAltitude;
	atmosphere.ozoneThickness = kOzoneFixThickness;
	atmosphere.ozoneAbsorption = { s.ozoneAbsorption.x * 1e-3 * ozoneScale, s.ozoneAbsorption.y * 1e-3 * ozoneScale, s.ozoneAbsorption.z * 1e-3 * ozoneScale };
	atmosphere.groundAlbedo = { s.groundAlbedo.x, s.groundAlbedo.y, s.groundAlbedo.z };
	atmosphere.midpoint = true;
	atmosphere.groundFix = true;
	atmosphere.multiScatterFix = true;  // fixed (47m, pinned since 44h)
	atmosphere.trLutEdgeFix = true;     // fixed (47m, pinned since 44h)
	atmosphere.precision = (extCbData.flags & kExtSkyPrecision) != 0;  // (47b) the transmittance LUT as the GPU builds it
	return skyModelCpu.Update(atmosphere);
}

float PhysicalSky::ComputeFogSkyLight(const RE::NiPoint3& a_lutSunDir)
{
	// Every 4th frame (the sky changes slowly), and on the first one.
	const uint32_t frame = globals::state->frameCount;
	if (fogSkyLightFrame != UINT32_MAX && frame - fogSkyLightFrame < 4u)
		return fogSkyLightCached;
	fogSkyLightFrame = frame;
	fogSkyLightCached = 0.f;
	if (!UpdateCpuSkyModel())
		return 0.f;
	auto unit = [](float x, float y, float z) {
		const double l = std::sqrt(double(x) * x + double(y) * y + double(z) * z);
		return l > 1e-9 ? PhysicalSkyModel::Vec3{ x / l, y / l, z / l } : PhysicalSkyModel::Vec3{ 0.0, 0.0, 1.0 };
	};
	auto vec = [](const float3& c) { return PhysicalSkyModel::Vec3{ c.x, c.y, c.z }; };
	const double cameraRadius = static_cast<double>(cbData.zCameraPlanet) * Util::Units::GAME_UNIT_TO_KM;
	const PhysicalSkyModel::Light lights[3] = {
		{ unit(a_lutSunDir.x, a_lutSunDir.y, a_lutSunDir.z), vec(cbData.sunlightColor) },
		{ unit(cbData.masserDir.x, cbData.masserDir.y, cbData.masserDir.z), vec(cbData.masserColor) },
		{ unit(cbData.secundaDir.x, cbData.secundaDir.y, cbData.secundaDir.z), vec(cbData.secundaColor) },
	};
	// Cosine-weighted mean over the upper hemisphere: bands 0-30, 30-60, 60-90 degrees (weights sin^2 differences
	// 0.25 / 0.5 / 0.25), each at its middle ring. The sun's in-scatter x Sky Fixes' brightness factor, as drawn.
	constexpr double kElevations[3] = { 15.0, 45.0, 75.0 };
	constexpr double kWeights[3] = { 0.25, 0.5, 0.25 };
	const bool baseOn = (nightCbData.flags & kNightBaseLight) != 0;
	double light = 0.0;
	for (int i = 0; i < 3; ++i) {
		const double sun = skyModelCpu.RingLuminance(cameraRadius, kElevations[i], lights, 1);
		const double moons = skyModelCpu.RingLuminance(cameraRadius, kElevations[i], lights + 1, 2);
		double base = 0.0;
		if (baseOn) {
			const float mix = SkySync::SmoothStep(0.f, 0.5f, static_cast<float>(std::sin(DirectX::XMConvertToRadians(static_cast<float>(kElevations[i])))));
			const float3 c = nightCbData.baseHorizon + (nightCbData.baseUpper - nightCbData.baseHorizon) * mix;
			base = 0.2126 * c.x + 0.7152 * c.y + 0.0722 * c.z;
		}
		light += kWeights[i] * (sun * std::clamp(nightCbData.skyScale, 1.f, kSkyFixesScaleCap) + moons + base);
	}
	if (std::isfinite(light) && light > 0.0)
		fogSkyLightCached = static_cast<float>(std::clamp(light, 1e-6, 100.0));
	return fogSkyLightCached;
}

float3 PhysicalSky::ComputeLightTransmittance(const float3& a_dir)
{
	// (49h) As PhysSky::SampleTr: the LUT from the camera toward the light, lerp(1, tr, trMix); 1 while Physical Sky is off.
	if (!loaded || !cbData.enabled || !(cbData.trMix > 1e-8f) || !UpdateCpuSkyModel())
		return { 1.f, 1.f, 1.f };
	const double l = std::sqrt(double(a_dir.x) * a_dir.x + double(a_dir.y) * a_dir.y + double(a_dir.z) * a_dir.z);
	if (!(l > 1e-9))
		return { 1.f, 1.f, 1.f };
	const double cameraRadius = static_cast<double>(cbData.zCameraPlanet) * Util::Units::GAME_UNIT_TO_KM;
	const auto tr = skyModelCpu.Transmittance(cameraRadius, { a_dir.x / l, a_dir.y / l, a_dir.z / l });
	const float mix = std::clamp(cbData.trMix, 0.f, 1.f);
	float3 result;
	float* out[3] = { &result.x, &result.y, &result.z };
	for (int c = 0; c < 3; ++c) {
		const float t = std::isfinite(tr[c]) ? std::clamp(static_cast<float>(tr[c]), 0.f, 1.f) : 1.f;
		*out[c] = 1.f + (t - 1.f) * mix;
	}
	return result;
}

void PhysicalSky::UpdateSkyAnchor(const RE::NiPoint3& a_lutSunDir, const float3& a_sun, const float3& a_masserGlow, const float3& a_secundaGlow, float a_exposure)
{
	// (50b) Stage 2: one factor k_sky that puts the physical sky family on the scene's (vanilla / weather) brightness scale.
	//   The brightness a white surface sends back (DALC and Lighting's units: ambient as is, a directional light x the vanilla diffuse
	//   multiplier VDM), the directional light at normal incidence (no elevation term: a low sun lights walls facing it fully, and an N.L term
	//   would have pushed k up exactly when the direct light is strong on walls):
	//     E_v    = the weather's DALC mean (as Lighting linearises it) + its sunlight colour (light gamma, directional multiplier) x VDM;
	//     E_phys = the CPU sky model's cosine-weighted sky radiance (the sun's and the moons' scattering, WITHOUT the night base light, which is
	//              the weather's own layer) + the sun x the transmittance x VDM (none below the horizon, -0.5..+0.5 degree ramp) + each moon's
	//              light x phase x the weather / horizon visibility x the daylight fade x VDM.
	//   k = (E_v / E_phys) / (E_v,ref / E_phys,ref): the reference is the same weather's day key with the sun at kAnchorRefElevationDeg (same
	//   azimuth, no moons), so k = 1 at a clear noon of every weather (the user's noon tuning is kept) and follows the weather's own data at
	//   every other moment (no time keying). Clamp: [1 / kAnchorCap, kAnchorCap] (32, as Sky Fixes' factor). Moonless nights: E_phys -> 0,
	//   k reaches the cap; it then multiplies only light that is ~0 (the night base light and the stars are not multiplied).
	//   Recomputed every 4th frame (the CPU model, as Physical Fog Brightness); smoothed in log2 over 1.5 s, snapping as Sky Fixes.
	const uint32_t frame = globals::state->frameCount;
	if (skyAnchorFrame == UINT32_MAX || frame - skyAnchorFrame >= 4u || skyAnchorSnap) {
		skyAnchorFrame = frame;
		auto& dbg = skyAnchorDebug;
		dbg = {};
		float target = 1.f;
		auto* sky = globals::game::sky;
		if (sky && sky->currentWeather && UpdateCpuSkyModel()) {
			const auto& ll = globals::features::linearLighting.settings;
			const bool linear = ll.enableLinearLighting;
			const float ambientGamma = linear && std::isfinite(ll.ambientGamma) ? std::clamp(ll.ambientGamma, 0.1f, 5.f) : 1.f;
			const float lightGamma = linear && std::isfinite(ll.lightGamma) ? std::clamp(ll.lightGamma, 0.1f, 5.f) : 1.f;
			const float dirMult = linear && std::isfinite(ll.directionalLightMult) ? std::max(ll.directionalLightMult, 0.f) : 1.f;
			const float vdm = linear && std::isfinite(ll.vanillaDiffuseMult) ? std::max(ll.vanillaDiffuseMult, 0.f) : 1.f;
			auto lumPow = [](float r, float g, float b, float a_gamma) {
				auto lin = [&](float v) { return std::pow(std::isfinite(v) ? std::max(v, 0.f) : 0.f, a_gamma); };
				return 0.2126f * lin(r) + 0.7152f * lin(g) + 0.0722f * lin(b);
			};
			auto lum = [](const float3& c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; };
			// The weather blend (current / last, as the game blends the keys).
			const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.f, 1.f) : 1.f;
			const RE::TESWeather* last = (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.f) ? sky->lastWeather : nullptr;
			// E_v now: DALC (the engine's blend) and the sunlight colour (the engine's blend).
			float dalc = 0.f;
			for (int axis = 0; axis < 3; ++axis)
				for (int side = 0; side < 2; ++side) {
					const auto& c = sky->directionalAmbientColors[axis][side];
					dalc += lumPow(c.red, c.green, c.blue, ambientGamma);
				}
			dalc /= 6.f;
			const auto& sl = sky->skyColor[RE::TESWeather::ColorTypes::kSunlight];
			const float sunV = lumPow(sl.red, sl.green, sl.blue, lightGamma) * dirMult;
			const auto& skySync = globals::features::skySync;
			const float masserZ = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)].z;
			const float secundaZ = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)].z;
			const float sunZ = std::clamp(a_lutSunDir.z, -1.f, 1.f);
			const float eV = dalc + sunV * vdm;  // the directional light at normal incidence (no elevation term: see above)
			// E_v at the reference: the day keys of the same weathers.
			auto keyDalc = [&]() {
				auto channel = [](const RE::Color& c, int i) { return (i == 0 ? c.red : i == 1 ? c.green : c.blue) * (1.f / 255.f); };
				float sum = 0.f;
				for (int axis = 0; axis < 3; ++axis)
					for (int side = 0; side < 2; ++side) {
						auto colorOf = [&](const RE::TESWeather* w) -> const RE::Color& {
							const auto& d = w->directionalAmbientLightingColors[RE::TESWeather::ColorTime::kDay].directional;
							const auto& mm = axis == 0 ? d.x : axis == 1 ? d.y : d.z;
							return side == 0 ? mm.max : mm.min;
						};
						float rgb[3];
						for (int i = 0; i < 3; ++i)
							rgb[i] = last ? std::lerp(channel(colorOf(last), i), channel(colorOf(sky->currentWeather), i), pct) : channel(colorOf(sky->currentWeather), i);
						sum += lumPow(rgb[0], rgb[1], rgb[2], ambientGamma);
					}
				return sum / 6.f;
			};
			auto keySun = [&]() {
				auto colorOf = [](const RE::TESWeather* w) { return w->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTime::kDay]; };
				const auto cur = colorOf(sky->currentWeather);
				float rgb[3] = { cur.red / 255.f, cur.green / 255.f, cur.blue / 255.f };
				if (last) {
					const auto old = colorOf(last);
					rgb[0] = std::lerp(old.red / 255.f, rgb[0], pct);
					rgb[1] = std::lerp(old.green / 255.f, rgb[1], pct);
					rgb[2] = std::lerp(old.blue / 255.f, rgb[2], pct);
				}
				return lumPow(rgb[0], rgb[1], rgb[2], lightGamma) * dirMult;
			};
			const double refEl = DirectX::XMConvertToRadians(kAnchorRefElevationDeg);
			const float eVRef = keyDalc() + keySun() * vdm;
			// E_phys now and at the reference (CPU model; the cosine-weighted mean as ComputeFogSkyLight, without the night base).
			auto unit = [](double x, double y, double z) {
				const double l = std::sqrt(x * x + y * y + z * z);
				return l > 1e-9 ? PhysicalSkyModel::Vec3{ x / l, y / l, z / l } : PhysicalSkyModel::Vec3{ 0.0, 0.0, 1.0 };
			};
			auto vec = [](const float3& c) { return PhysicalSkyModel::Vec3{ c.x, c.y, c.z }; };
			const double cameraRadius = static_cast<double>(cbData.zCameraPlanet) * Util::Units::GAME_UNIT_TO_KM;
			const PhysicalSkyModel::Light lights[3] = {
				{ unit(a_lutSunDir.x, a_lutSunDir.y, a_lutSunDir.z), vec(a_sun) },
				{ unit(cbData.masserDir.x, cbData.masserDir.y, cbData.masserDir.z), vec(a_masserGlow) },
				{ unit(cbData.secundaDir.x, cbData.secundaDir.y, cbData.secundaDir.z), vec(a_secundaGlow) },
			};
			const double horizontal = std::sqrt(double(a_lutSunDir.x) * a_lutSunDir.x + double(a_lutSunDir.y) * a_lutSunDir.y);
			PhysicalSkyModel::Light refLight = lights[0];
			refLight.dir = horizontal > 1e-6 ? PhysicalSkyModel::Vec3{ a_lutSunDir.x / horizontal * std::cos(refEl), a_lutSunDir.y / horizontal * std::cos(refEl), std::sin(refEl) } :
			                                   PhysicalSkyModel::Vec3{ std::cos(refEl), 0.0, std::sin(refEl) };
			constexpr double kElevations[3] = { 15.0, 45.0, 75.0 };
			constexpr double kWeights[3] = { 0.25, 0.5, 0.25 };
			double skyNow = 0.0, skyRef = 0.0;
			for (int i = 0; i < 3; ++i) {
				skyNow += kWeights[i] * (skyModelCpu.RingLuminance(cameraRadius, kElevations[i], lights, 1) + skyModelCpu.RingLuminance(cameraRadius, kElevations[i], lights + 1, 2));
				skyRef += kWeights[i] * skyModelCpu.RingLuminance(cameraRadius, kElevations[i], &refLight, 1);
			}
			const float3 trSun = ComputeLightTransmittance(float3(a_lutSunDir.x, a_lutSunDir.y, a_lutSunDir.z));
			const float3 trRef = ComputeLightTransmittance(float3(static_cast<float>(refLight.dir[0]), static_cast<float>(refLight.dir[1]), static_cast<float>(refLight.dir[2])));
			const float sunDirect = lum(a_sun * trSun) * vdm * SkySync::SmoothStep(-0.5f, 0.5f, DirectX::XMConvertToDegrees(std::asin(sunZ)));  // below the horizon: none
			float moonDirect = 0.f;
			const float dayFade = SkySync::MoonDayFade(DirectX::XMConvertToDegrees(std::asin(std::clamp(skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)].z, -1.f, 1.f))));
			const RE::Moon* moons[2] = { sky->masser, sky->secunda };
			const float3* colors[2] = { &settings.masserColor, &settings.secundaColor };
			const float moonZ[2] = { masserZ, secundaZ };
			for (int i = 0; i < 2; ++i) {
				if (!moons[i])
					continue;
				const float strength = SkySync::PhaseFactorFromTexture(moons[i]) * MoonHorizonVisibility(moons[i], sky, moonZ[i]) * dayFade *
				                       (i == 1 ? SkySync::SecundaIntensityFactor : 1.f);
				moonDirect += lum(*colors[i] * a_exposure) * strength * vdm;  // the visibility holds the -2..3 degree rise
			}
			const double ePhys = skyNow + sunDirect + moonDirect;
			const double ePhysRef = skyRef + lum(a_sun * trRef) * vdm;
			dbg.eVanilla = eV;
			dbg.ePhys = static_cast<float>(ePhys);
			dbg.eVanillaRef = eVRef;
			dbg.ePhysRef = static_cast<float>(ePhysRef);
			dbg.dalc = dalc;
			dbg.skyPhys = static_cast<float>(skyNow);
			if (std::isfinite(eV) && std::isfinite(ePhys) && std::isfinite(eVRef) && std::isfinite(ePhysRef) && eVRef > 1e-6f && ePhysRef > 1e-9) {
				const double k = (static_cast<double>(eV) / std::max(ePhys, 1e-9)) * (ePhysRef / static_cast<double>(eVRef));
				target = static_cast<float>(std::clamp(k, 1.0 / kAnchorCap, static_cast<double>(kAnchorCap)));
				dbg.valid = true;
			}
		}
		skyAnchorTarget = std::isfinite(target) ? target : 1.f;
	}
	// Smoothing (log2), snapping on time skips, loads, a stopped clock, a new sun path and when the sky (re)appears.
	const auto calendar = RE::Calendar::GetSingleton();
	const float hours = calendar ? calendar->GetHoursPassed() : 0.f;
	const float timeScale = calendar ? calendar->GetTimescale() : 0.f;
	const uint32_t warp = globals::features::skySync.GetSunWarpGeneration();
	const bool snap = skyAnchorSnap || !(timeScale > 0.f) || !(std::abs(hours - skyAnchorLastHours) < 0.01f) || warp != skyAnchorLastWarp;
	skyAnchorLastHours = hours;
	skyAnchorLastWarp = warp;
	skyAnchorSnap = false;
	if (snap) {
		skyAnchorK = skyAnchorTarget;
	} else {
		const float dt = globals::game::deltaTime ? std::clamp(*globals::game::deltaTime, 0.f, 1.f) : 0.f;
		const float logK = std::log2(std::max(skyAnchorK, 1e-6f));
		const float logT = std::log2(std::max(skyAnchorTarget, 1e-6f));
		skyAnchorK = std::exp2(logK + (logT - logK) * (1.f - std::exp(-dt / kSkyFixesSmoothingSeconds)));
	}
	if (!std::isfinite(skyAnchorK))
		skyAnchorK = 1.f;
	skyAnchorK = std::clamp(skyAnchorK, 1.f / kAnchorCap, kAnchorCap);
}

uint32_t PhysicalSky::PhysLightingFlags() const
{
	// (49h stage 1) Physical Lighting rule bits for SharedData::horizonFixData.Flags (Common/PhysicalLighting.hlsli).
	const auto& pl = settings.physicalLighting;
	// (49j, design 9-4) Independent of the master.
	const uint32_t independent = (loaded && settings.rainNoColour) ? kPLRainNoColour : 0u;
	if (!pl.enabled)
		return independent;
	uint32_t flags = independent;
	flags |= pl.iblNoFeedback ? kPLIblNoFeedback : 0u;
	flags |= pl.iblEnvOcclusion ? kPLEnvOcclusion : 0u;
	flags |= pl.farSkylight ? kPLFarSkylight : 0u;
	flags |= pl.lodScale ? kPLLodScale : 0u;
	flags |= pl.farFog ? kPLFarFog : 0u;
	flags |= pl.forwardCubemap ? kPLForwardCubemap : 0u;
	flags |= pl.smallFixes ? kPLSmallFixes : 0u;
	// True PBR land drawn in the last ~2 s (the near land's light scale is 1 / pi then).
	const uint32_t frame = globals::state->frameCount;
	const uint32_t lastPbr = TruePBR::lastPbrLandFrame.load(std::memory_order_relaxed);
	if (lastPbr != 0u && frame - lastPbr < 240u)
		flags |= kPLLandPbr;
	return flags;
}

void PhysicalSky::SettingsStage2()
{
	// (50b) Physical lighting redo, stage 2: "One Sun, One Moon, One Sky".
	auto& st = settings.lightingStage2;
	ImGui::SeparatorText("One Sun, One Moon, One Sky (stage 2)");
	ImGui::Checkbox("Sky Family on One Scale", &st.enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the switches below. Off: exactly as 50a (nothing below acts).\n"
			"The physical sky family (sunlight and moonlight on the ground, the sky's own glow, the clouds' sun and moon light, the moon\n"
			"discs, the sky's light on the fog) is brought to the game's own brightness by one factor per frame, worked out from the\n"
			"weather's ambient and sunlight colours against the physical sky model. Lamps, fire, windows, glowing things, the weather's\n"
			"ambient, rain, the stars and the night sky's own glow are never changed by it.");
	ImGui::BeginDisabled(!st.enabled);
	ImGui::Indent();
	ImGui::Checkbox("One Brightness for Sun, Moon, Sky and Clouds", &st.skyAnchor);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): one factor k for the whole sky family = how bright the weather wants this moment (its ambient and sunlight\n"
			"colours on a white surface) / how bright the physical sky makes it, normalised so a clear noon of each weather keeps k = 1\n"
			"(your noon stays as it is). Dawn, dusk and moonlit nights: sky, clouds, discs and the ground's sun / moonlight rise and fall\n"
			"together. Replaces Sky Fixes' zenith-only sky factor. k is limited to 1/32 .. 32 and changes smoothly (1.5 s).\n"
			"Off: as 50a (only the sky's sun scattering follows the ground, through Sky Fixes).");
	ImGui::Checkbox("Clouds Lit Like the Ground", &st.cloudsLikeGround);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the moons light the clouds exactly as they light the ground (no separate half-strength, no greying, the\n"
			"cloud's transparency counted once instead of twice), and the clouds get the ground's ambient light, no longer dimmed\n"
			"by the weather's near-black night cloud colour (whose hue is kept). Night clouds are no longer dark and dirty.\n"
			"Daytime clouds can look a little different (their ambient is the ground's now). Needs Clouds > Moonlight on Clouds.\n"
			"Off: as 50a (Moonlight Strength and the sky's glow on clouds as set on the Clouds tab).");
	ImGui::Checkbox("Weather Hides the Moon on the Ground Too", &st.weatherHidesMoon);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): when the weather hides the moon (thick overcast, storms), the moonlight on the ground fades with it, the same\n"
			"way the moon's glow in the sky and its light on the clouds fade. Also the same rise above the horizon (-2 to +3 degrees).\n"
			"Off: as 50a (overcast nights keep their moonlight on the ground).");
	ImGui::Checkbox("Stars Dim Toward the Horizon", &st.starExtinction);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): stars and the Milky Way get dimmer and warmer toward the horizon, where their light crosses more air\n"
			"(atmospheric extinction, from the physical sky's transmittance); straight up nothing changes. With dense haze settings\n"
			"the dimmed band can be wide. Off: as 50a.");
	ImGui::Unindent();
	ImGui::EndDisabled();
	if (Stage2On(st.skyAnchor) && cbData.enabled && !outdoorActive) {
		const auto& d = skyAnchorDebug;
		ImGui::TextDisabled("k now x%.3f (target x%.3f)%s", skyAnchorK, skyAnchorTarget, d.valid ? "" : " - no usable weather data, x1");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Game brightness now %.4f (ambient %.4f), physical %.4f (sky %.4f); at the clear-noon reference: game %.4f, physical %.4f",
				d.eVanilla, d.dalc, d.ePhys, d.skyPhys, d.eVanillaRef, d.ePhysRef);
			ImGui::Text("Self-test: Debug tab > Sky Light Wobble.");
		}
	}
}

void PhysicalSky::SettingsSceneLighting()
{
	SettingsOutdoorLight();  // (51a) first on the tab
	ImGui::Spacing();
	// (50b) Stage 2: replaced by Physical Outdoor Light while that is on (its anchor is not used then).
	if (OutdoorOn(true))
		ImGui::TextDisabled(
			"Stage 2 below: its \"One Brightness\" is not used while Physical Outdoor Light is on; Clouds Lit Like the Ground and Weather\n"
			"Hides the Moon apply only where the matching new item is off; Star Extinction always applies.");
	SettingsStage2();
	ImGui::Spacing();
	// (49h stage 1; 50a redo base) "Lighting Fixes": fixes that do not change the light units (S3a-S8). The 49h S1/S2 unit rules are retired.
	InfoBox("Lighting Fixes: consistency fixes for shadows, reflections, far land and fog. They do not change the light units.");
	auto& pl = settings.physicalLighting;
	ImGui::Checkbox("Lighting Fixes", &pl.enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the fixes below (each can be switched alone). Off: none of the fixes below.\n"
			"Shown in game where it matters most: snow in the open, forest edges, the edge of the loaded area, and fog in the distance.");
	ImGui::BeginDisabled(!pl.enabled);
	ImGui::Indent();
	ImGui::Checkbox("Screen Light Probe Without Feedback", &pl.iblNoFeedback);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"The light probe that Image Based Lighting takes from the screen no longer contains its own light: bright snow and\n"
			"stone stop lifting their own shadows (it could run away on snow). Shadows are a little darker and more even.");
	ImGui::Checkbox("Canopies Block the Surroundings' Light", &pl.iblEnvOcclusion);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Under trees, eaves and overhangs the light from the surroundings coming from above is blocked like the sky's light\n"
			"(the probe fills the sky with ground colour, which leaked under every roof).");
	ImGui::Checkbox("Far Objects Keep Their Shadows", &pl.farShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"With ray-traced sun shadows: houses, rocks and other full objects get sun / moon shadows out to where the rays reach\n"
			"(the edge of the loaded area, ~120-175 m), not only to the game's 86 m.");
	ImGui::Checkbox("Far Skylight Is Not Open Sky", &pl.farSkylight);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Beyond the skylighting area (~70 m) surfaces no longer count as seeing the whole sky: a sideways or downward face sees\n"
			"half or less. Far shaded areas, forests and valleys are less bright, especially at night.");
	ImGui::Checkbox("Far Land in the Near Land's Light", &pl.lodScale);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"With PBR land the far (LOD) land gets the same sunlight / moonlight scale as the near land (was 12%% darker), and the\n"
			"LOD Blending brightness (gamma) reaches both kinds of far land. Removes the step at the loaded edge.");
	ImGui::Checkbox("Far Fog Continues the Near Fog", &pl.farFog);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"With volumetric fog on, the far height fog takes the colour the volumetric fog has at its far end (same sun, sky and\n"
			"weather fog colour) instead of the reflection probe's colour toward the view, so there is no seam at ~850 m and\n"
			"no white veil from the probe. Without volumetric fog: the weather fog colour at its physical brightness.");
	ImGui::Checkbox("Forward Reflections at the Cubemap's Brightness", &pl.forwardCubemap);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Hair, glass and other see-through or forward-drawn shiny things reflect the cubemap at its own brightness, like the\n"
			"rest of the scene with Reflection Brightness Fix (Materials > Dynamic Cubemaps). Were 2-5x too bright.");
	ImGui::Checkbox("Small Lighting Fixes", &pl.smallFixes);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Unlit effects counted the surroundings' light twice and the weather ambient unconverted; distant billboard trees got\n"
			"the weather ambient through roofs and canopies; the deferred ambient used another upward boost than the surfaces;\n"
			"indoors a few passes scaled the sun differently.");
	ImGui::Unindent();
	ImGui::EndDisabled();

	ImGui::SeparatorText("Related (own switches)");
	ImGui::Checkbox("Sun Highlight on Water Follows the Atmosphere", &settings.waterSunTransmittance);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the sun's bright highlight on water is reddened and dimmed by the atmosphere like the sunlight on the\n"
			"ground (Celestials > Transmittance Mix), so at dusk and dawn it is no longer a white glare. Off: as in 49g.");
	ImGui::Checkbox("Rain Without Colour", &settings.rainNoColour);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): rain and snow streaks keep only the brightness of their texture, tinted by the weather's precipitation colour.\n"
			"Some weather mods paint the rain texture with refraction colours (cyan, brown, purple noise), which at night showed as\n"
			"rainbow-coloured streaks. Off: as in 49i.");
	ImGui::TextDisabled("Also: Lighting > Exponential Height Fog > Stable Fog Colour; Materials > Dynamic Cubemaps > Reflection Brightness Fix.");
	if (pl.enabled && pl.lodScale)
		ImGui::TextDisabled("PBR land seen: %s", (PhysLightingFlags() & kPLLandPbr) ? "yes (far land uses the PBR scale)" : "no");
}

void PhysicalSky::SettingsOutdoorLight()
{
	// (51a) "Physical Outdoor Light": first on the Scene Lighting tab, replacing stage 2's place.
	auto& ol = settings.outdoorLight;
	ImGui::SeparatorText("Physical Outdoor Light (new model, package 1)");
	ImGui::Checkbox("Physical Outdoor Light", &ol.enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): outdoors where the physical sky is drawn, the outdoor light follows the new model (the items below, each with\n"
			"its own switch). The sun, the sky and the moonlight are no longer pulled to the weather's brightness moment by moment (stage 2's\n"
			"\"one brightness\" factor and Sky Fixes' sky factor are not used); the weather decides how gloomy the day is and whether the moon\n"
			"shows. Interiors and other worldspaces are unchanged. Off: exactly as 50d (stage 2's section below applies again).");
	ImGui::BeginDisabled(!ol.enabled);
	ImGui::Indent();
	ImGui::Checkbox("Physical Sun and Sky", &ol.sunFamily);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): sunlight, the sky's glow, the sun on the clouds, the sky's light on the fog and the sun's share of the ambient come\n"
			"from the physical sky with one fixed constant (a clear noon looks as before) times the weather's gloominess. Dawn, dusk and\n"
			"twilight follow the atmosphere alone. The weather's per-time sunlight scale (imagespace) is replaced by its clear-day value.\n"
			"Needs Celestials > Override Directional Light for the light on the ground.");
	ImGui::BeginDisabled(!ol.sunFamily);
	ImGui::SliderFloat("Weather Gloominess", &ol.weatherGloominess, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How much the weather's own day brightness dims the sun family: 1 (default) = as the weather mod wrote it (its daylight, ambient +\n"
			"sunlight x its sunlight scale, against this world's clear weathers; limited to 3%%..100%%), 0 = every weather as bright as a clear day.");
	ImGui::EndDisabled();
	ImGui::Checkbox("Moon by Phase, Height and Weather", &ol.moonFamily);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the moonlight is your moon colour (Celestials) x phase x height x how visible the weather leaves the moon (its night moon\n"
			"glare against this world's clear weathers; overcast and rain dim it, a black glare still hides it). The same rule for the ground, the\n"
			"sky's moon glow, the clouds and the stars. Full moonlight once the sun is 1 degree below the horizon, none by +3 degrees.\n"
			"The weather's night sunlight scale no longer multiplies it.");
	ImGui::Checkbox("Night Sky Glow from CS", &ol.nightGlow);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the night sky's own faint glow is a fixed physical value (the natural moonless sky, about 0.0005 cd/m2, a little\n"
			"brighter toward the horizon), not the weather's night sky colours. The stars are not changed.");
	ImGui::BeginDisabled(!ol.nightGlow);
	ImGui::SliderFloat("Night Sky Glow", &ol.nightSkyGlow, 0.f, 100.f, "x%.2f", ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("x the physical value: 1 (default) = physical, up to x100.");
	ImGui::EndDisabled();
	ImGui::Checkbox("Ambient from the Physical Sky", &ol.ambient);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the game's directional ambient light (DALC) is worked out from the physical sky above (sun, moon, night glow) and the\n"
			"ground's bounce, with one constant so a clear noon matches; no fixed weather ambient floor. Its colour takes the weather's ambient hue\n"
			"(below). Image Based Lighting keeps reading the drawn sky. Other readers of the weather's own ambient still see the weather's.");
	ImGui::BeginDisabled(!ol.ambient);
	ImGui::SliderFloat("Weather Ambient Tint", &ol.ambientTint, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"1 (default): the ambient's colour is the weather's ambient hue for the sun's height (brightness from physics; the saturation is limited\n"
			"to natural daylight / sky colours). 0: the physical sky's own colour.");
	ImGui::EndDisabled();
	ImGui::Checkbox("Clouds Lit by CS Light Only", &ol.clouds);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the clouds are lit only by CS's sun (with the weather's gloominess), the moons and the physical sky's light, at one\n"
			"constant that keeps a clear noon; the weather's cloud colour no longer tints them (night clouds are no longer teal). The cloud\n"
			"textures and their transparency are kept. Needs Clouds > Cloud Lighting \"Sun + Moons + Sky\".");
	ImGui::Checkbox("Smooth Sun Below Horizon", &ol.smoothSun);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): below the horizon the sun keeps going down (Sky Sync's timing curve). Off: as 50d, where it hangs at about -6 degrees\n"
			"for nearly an hour (about 19:15-20:15 and 06:00-06:45 with Skyrim's climate timing).");
	ImGui::Checkbox("Real Ozone", &ol.realOzone);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the ozone layer absorbs as real ozone does (mostly green: 0.650 / 1.881 / 0.085 per Mm, Hillaire 2020 / Bruneton),\n"
			"so a low sun turns orange-red instead of lemon yellow. Off: your Atmosphere > Ozone values as before.");
	ImGui::Unindent();
	ImGui::EndDisabled();
	if (OutdoorOn(true)) {
		if (!outdoorActive) {
			ImGui::TextDisabled("Not in effect here (interior, or a worldspace without the physical sky).");
		} else {
			const auto& f = outdoorFrame;
			ImGui::TextDisabled("Gloominess %.2f (current %.2f, last %.2f) -> sun family x%.3f; sunlight scale now %.2f, clear-day %.2f", f.g, f.gCurrent, f.gLast, f.sunScale, f.sigmaNow, f.sigmaRef);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				static constexpr const char* kSources[] = { "none (neutral)", "this world's region weather lists", "the parent world's region weather lists", "the climate's list" };
				ImGui::Text("Clear reference: %s, %d Pleasant entries; its day light %.4f, ambient %.4f, night moon glare %.4f, day cloud colour %.4f",
					kSources[std::clamp(weatherRef.source, 0, 3)], weatherRef.entries, weatherRef.keys.e, weatherRef.keys.a, weatherRef.keys.glareNight, weatherRef.keys.cloudDay);
				ImGui::Text("Moon visibility now: Masser %.2f, Secunda %.2f (weather night glare %.4f)", globals::game::sky ? MoonVisibility(globals::game::sky->masser, globals::game::sky, globals::features::skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)].z) : 0.f,
					globals::game::sky ? MoonVisibility(globals::game::sky->secunda, globals::game::sky, globals::features::skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)].z) : 0.f, f.glareNight);
				if (outdoorAmbient.valid) {
					float mean = 0.f;
					for (int axis = 0; axis < 3; ++axis)
						for (int side = 0; side < 2; ++side)
							mean += 0.2126f * outdoorAmbient.linear[axis][side].x + 0.7152f * outdoorAmbient.linear[axis][side].y + 0.0722f * outdoorAmbient.linear[axis][side].z;
					ImGui::Text("Ambient: mean %.4f (up %.4f, down %.4f), calibration %.3f; cloud sky ambient calibration %.3f", mean / 6.f,
						0.2126f * outdoorAmbient.linear[2][0].x + 0.7152f * outdoorAmbient.linear[2][0].y + 0.0722f * outdoorAmbient.linear[2][0].z,
						0.2126f * outdoorAmbient.linear[2][1].x + 0.7152f * outdoorAmbient.linear[2][1].y + 0.0722f * outdoorAmbient.linear[2][1].z, outdoorAmbient.calibration, outdoorAmbient.cloudAmbientCal);
				}
				ImGui::Text("Engine DALC layout check: %s", dalcConventionChecked ? (dalcConventionError < 0.01f ? "matches" : "DIFFERS (tell the developer)") : "not checked yet");
				ImGui::Text("Weather keys by sun height: %s", keyElevations.valid ? "from the climate timing" : "fallback ramp");
			}
		}
	}
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// (51a) Physical Outdoor Light (four-layer outdoor light model, package 1)

namespace
{
	struct OutdoorGammas
	{
		float ambient = 1.f, light = 1.f, sky = 1.f, dirMult = 1.f, vdm = 1.f;
	};
	/// Linear Lighting's conversions as the shaders apply them (all 1 without Linear Lighting).
	OutdoorGammas GetOutdoorGammas()
	{
		OutdoorGammas g;
		const auto& ll = globals::features::linearLighting.settings;
		if (!ll.enableLinearLighting)
			return g;
		auto gamma = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.1f, 5.f) : 1.f; };
		g.ambient = gamma(ll.ambientGamma);
		g.light = gamma(ll.lightGamma);
		g.sky = gamma(ll.skyGamma);
		g.dirMult = std::isfinite(ll.directionalLightMult) ? std::max(ll.directionalLightMult, 0.f) : 1.f;
		g.vdm = std::isfinite(ll.vanillaDiffuseMult) ? std::max(ll.vanillaDiffuseMult, 0.f) : 1.f;
		return g;
	}
	float LumBytes(const RE::Color& a_c, float a_gamma)
	{
		auto lin = [a_gamma](std::uint8_t v) { return std::pow(v * (1.f / 255.f), a_gamma); };
		return 0.2126f * lin(a_c.red) + 0.7152f * lin(a_c.green) + 0.0722f * lin(a_c.blue);
	}
	float Lum3(const float3& a_c) { return 0.2126f * a_c.x + 0.7152f * a_c.y + 0.0722f * a_c.z; }
	float ImagespaceSunlightScale(const RE::TESImageSpace* a_imagespace)
	{
		if (!a_imagespace)
			return 1.f;
		const float v = a_imagespace->data.hdr.sunlightScale;
		return std::isfinite(v) && v >= 0.f ? v : 1.f;
	}
	/// The imagespace sunlight scale the engine applies to the directional light this frame (State::UpdateSharedData, the Lighting constant).
	float CurrentSunlightScale()
	{
		auto* imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
		if (!imageSpaceManager)
			return 1.f;
		const float v = !globals::game::isVR ? imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale : imageSpaceManager->GetVRRuntimeData().data.baseData.hdr.sunlightScale;
		return std::isfinite(v) && v >= 0.f ? v : 1.f;
	}
	bool IsPleasant(const RE::TESWeather* a_weather)
	{
		return a_weather && a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kPleasant);
	}
	/// The six directional ambient colours of a weather key in the engine's order ([axis][0] = +axis (max), [axis][1] = -axis (min)), 0-1.
	void DalcKey(const RE::TESWeather* a_weather, int a_time, float3 a_out[3][2])
	{
		const auto& d = a_weather->directionalAmbientLightingColors[a_time].directional;
		const RE::Color* c[3][2] = { { &d.x.max, &d.x.min }, { &d.y.max, &d.y.min }, { &d.z.max, &d.z.min } };
		for (int axis = 0; axis < 3; ++axis)
			for (int side = 0; side < 2; ++side)
				a_out[axis][side] = float3(c[axis][side]->red, c[axis][side]->green, c[axis][side]->blue) * (1.f / 255.f);
	}
	PhysicalSkyModel::Vec3 UnitVec(double x, double y, double z)
	{
		const double l = std::sqrt(x * x + y * y + z * z);
		return l > 1e-9 ? PhysicalSkyModel::Vec3{ x / l, y / l, z / l } : PhysicalSkyModel::Vec3{ 0.0, 0.0, 1.0 };
	}
	/// Irradiance (rgb, in the lights' colour units) on the six axis-facing surfaces from the sky-view radiance of a_lights over the upper
	/// hemisphere (3 bands x 8 azimuths) plus the night glow (radiance by elevation, upper / horizon, LutGen's gradient), with a Lambertian
	/// ground of a_albedo under the sky and a_directHorizontal for the lower hemisphere. [axis][0] = +axis, [axis][1] = -axis.
	struct SkyIrradiance
	{
		float3 e[3][2] = {};
		float3 horizontal = {};  // the sky's alone, on a horizontal surface
	};
	SkyIrradiance IntegrateSky(const PhysicalSkyModel::Model& a_model, double a_cameraRadius, const PhysicalSkyModel::Light* a_lights, int a_lightCount,
		const float3& a_glowUpper, const float3& a_glowHorizon, const float3& a_directHorizontal, const float3& a_albedo)
	{
		SkyIrradiance out;
		constexpr double kPi = 3.14159265358979323846;
		constexpr double kBandLow[3] = { 0.0, 30.0, 60.0 };
		constexpr int kAzimuths = 8;
		double e[3][2][3] = {};
		double h[3] = {};
		for (int band = 0; band < 3; ++band) {
			const double lo = kBandLow[band] * (kPi / 180.0), hi = (kBandLow[band] + 30.0) * (kPi / 180.0);
			const double el = 0.5 * (lo + hi);
			const double solidAngle = (std::sin(hi) - std::sin(lo)) * (2.0 * kPi / kAzimuths);  // the band's solid angle per azimuth sample
			const float glowMix = SkySync::SmoothStep(0.f, 0.5f, static_cast<float>(std::sin(el)));
			const float3 glow = a_glowHorizon + (a_glowUpper - a_glowHorizon) * glowMix;
			for (int j = 0; j < kAzimuths; ++j) {
				const double az = (j + 0.5) * (2.0 * kPi / kAzimuths);
				const PhysicalSkyModel::Vec3 dir = { std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el) };
				PhysicalSkyModel::Vec3 radiance = a_lightCount > 0 ? a_model.SkyRay(a_cameraRadius, dir, a_lights, a_lightCount) : PhysicalSkyModel::Vec3{ 0.0, 0.0, 0.0 };
				radiance[0] += glow.x;
				radiance[1] += glow.y;
				radiance[2] += glow.z;
				for (int axis = 0; axis < 3; ++axis) {
					const double c = dir[axis];
					const int side = c >= 0.0 ? 0 : 1;
					const double w = std::abs(c) * solidAngle;
					for (int ch = 0; ch < 3; ++ch)
						e[axis][side][ch] += radiance[ch] * w;
				}
				for (int ch = 0; ch < 3; ++ch)
					h[ch] += radiance[ch] * dir[2] * solidAngle;
			}
		}
		out.horizontal = float3(static_cast<float>(h[0]), static_cast<float>(h[1]), static_cast<float>(h[2]));
		// The ground: a Lambertian floor under the sky and the direct light, seen by the lower hemisphere (projected solid angle
		// pi (1 - n_z) / 2 for a surface facing n: pi straight down, pi / 2 sideways, 0 up).
		const float3 groundRadiance = a_albedo * (out.horizontal + a_directHorizontal) * static_cast<float>(1.0 / kPi);
		for (int axis = 0; axis < 3; ++axis)
			for (int side = 0; side < 2; ++side) {
				const float nz = axis == 2 ? (side == 0 ? 1.f : -1.f) : 0.f;
				const float3 bounce = groundRadiance * static_cast<float>(kPi * (1.0 - nz) * 0.5);
				out.e[axis][side] = float3(static_cast<float>(e[axis][side][0]), static_cast<float>(e[axis][side][1]), static_cast<float>(e[axis][side][2])) + bounce;
			}
		return out;
	}
	bool AllFinite(const float3& c) { return std::isfinite(c.x) && std::isfinite(c.y) && std::isfinite(c.z); }
	bool SameTransform(const RE::NiTransform& a, const RE::NiTransform& b)
	{
		return std::memcmp(a.rotate.entry, b.rotate.entry, sizeof(a.rotate.entry)) == 0 && a.translate.x == b.translate.x && a.translate.y == b.translate.y &&
		       a.translate.z == b.translate.z;
	}
}

PhysicalSky::WeatherKeys PhysicalSky::ReadWeatherKeys(const RE::TESWeather* a_weather)
{
	WeatherKeys k;
	if (!a_weather)
		return k;
	const auto g = GetOutdoorGammas();
	using ColorTypes = RE::TESWeather::ColorTypes;
	using ColorTime = RE::TESWeather::ColorTime;
	float3 dalc[3][2];
	DalcKey(a_weather, ColorTime::kDay, dalc);
	float a = 0.f;
	float3 mean = {};
	for (int axis = 0; axis < 3; ++axis)
		for (int side = 0; side < 2; ++side) {
			const float3& c = dalc[axis][side];
			a += Lum3(float3(std::pow(c.x, g.ambient), std::pow(c.y, g.ambient), std::pow(c.z, g.ambient)));
			mean += c;
		}
	k.a = a / 6.f;
	mean = mean * (1.f / 6.f);
	k.aCloud = Lum3(float3(std::pow(mean.x, g.ambient), std::pow(mean.y, g.ambient), std::pow(mean.z, g.ambient)));
	k.sigmaDay = ImagespaceSunlightScale(a_weather->imageSpaces[ColorTime::kDay]);
	k.e = k.a + LumBytes(a_weather->colorData[ColorTypes::kSunlight][ColorTime::kDay], g.light) * g.dirMult * g.vdm * k.sigmaDay;
	k.glareNight = LumBytes(a_weather->colorData[ColorTypes::kMoonGlare][ColorTime::kNight], g.sky);
	float weight = 0.f, sum = 0.f;
	for (int i = 0; i < RE::TESWeather::kTotalLayers; ++i) {
		if (a_weather->cloudLayerDisabledBits & (1u << i))
			continue;
		if (a_weather->cloudTextures[i].textureName.empty())
			continue;
		const float alpha = a_weather->cloudAlpha[i][ColorTime::kDay];
		if (!(alpha > 0.01f) || !std::isfinite(alpha))
			continue;
		weight += alpha;
		sum += alpha * LumBytes(a_weather->cloudColorData[i][ColorTime::kDay], g.sky);
	}
	k.cloudDay = weight > 0.f ? sum / weight : 0.f;
	return k;
}

void PhysicalSky::UpdateWeatherReference()
{
	auto* worldspace = GetCurrentWorldspace();
	auto* sky = globals::game::sky;
	const RE::TESClimate* climate = sky ? sky->currentClimate : nullptr;
	const auto g = GetOutdoorGammas();
	const float gammaKey = g.ambient * 1000.f + g.light * 100.f + g.sky * 10.f + g.vdm + g.dirMult * 0.001f;
	auto& ref = weatherRef;
	if (ref.built && ref.worldspace == worldspace && ref.climate == climate && ref.gammaKey == gammaKey)
		return;
	ref = {};
	ref.built = true;
	ref.worldspace = worldspace;
	ref.climate = climate;
	ref.gammaKey = gammaKey;

	WeatherKeys sum;
	sum.sigmaDay = 0.f;
	float total = 0.f;
	int entries = 0;
	auto add = [&](const RE::WeatherType* a_type) {
		if (!a_type || !IsPleasant(a_type->weather) || a_type->chance == 0)
			return;
		const float w = static_cast<float>(a_type->chance);
		const WeatherKeys k = ReadWeatherKeys(a_type->weather);
		sum.e += w * k.e;
		sum.a += w * k.a;
		sum.aCloud += w * k.aCloud;
		sum.sigmaDay += w * k.sigmaDay;
		sum.glareNight += w * k.glareNight;
		sum.cloudDay += w * k.cloudDay;
		total += w;
		++entries;
	};
	// Every region weather list in the worldspace (the lists the game draws the outdoor weather from; summed chances, as validated in
	// reviews/validation-weather-rules-2026-11.md), else the parent worldspace's, else the climate's list.
	auto gather = [&](const RE::TESWorldSpace* a_worldspace) {
		auto* dataHandler = RE::TESDataHandler::GetSingleton();
		if (!dataHandler || !a_worldspace)
			return;
		constexpr auto skipFlags = RE::TESRegion::RecordFlags::kBorderRegion | RE::TESRegion::RecordFlags::kDeleted | RE::TESRegion::RecordFlags::kIgnored;
		for (auto* region : dataHandler->GetFormArray<RE::TESRegion>()) {
			if (!region || region->worldSpace != a_worldspace || !region->dataList || (region->formFlags & skipFlags))
				continue;
			for (auto* data : region->dataList->regionDataList) {
				if (!data || data->GetType() != RE::TESRegionData::Type::kWeather)
					continue;
				for (auto* type : static_cast<RE::TESRegionDataWeather*>(data)->weatherTypes)
					add(type);
			}
		}
	};
	gather(worldspace);
	ref.source = total > 0.f ? 1 : 0;
	if (!(total > 0.f) && worldspace && worldspace->parentWorld) {
		gather(worldspace->parentWorld);
		ref.source = total > 0.f ? 2 : 0;
	}
	if (!(total > 0.f) && climate) {
		for (auto* type : const_cast<RE::TESClimate*>(climate)->weatherList)
			add(type);
		ref.source = total > 0.f ? 3 : 0;
	}
	if (total > 0.f) {
		const float inv = 1.f / total;
		ref.keys = { sum.e * inv, sum.a * inv, sum.aCloud * inv, sum.sigmaDay * inv, sum.glareNight * inv, sum.cloudDay * inv };
		ref.entries = entries;
		ref.chance = total;
	}
	const char* name = worldspace ? worldspace->GetFormEditorID() : nullptr;
	logger::info("[Physical Sky] (51a) weather reference for {}: source {}, {} Pleasant entries (chance {:.0f}), E {:.4f}, ambient {:.4f}, sunlight scale {:.3f}, night moon glare {:.4f}, day cloud {:.4f}",
		name && *name ? name : "-", ref.source, ref.entries, ref.chance, ref.keys.e, ref.keys.a, ref.keys.sigmaDay, ref.keys.glareNight, ref.keys.cloudDay);
}

void PhysicalSky::UpdateOutdoorFrame()
{
	auto& f = outdoorFrame;
	UpdateWeatherReference();
	auto* sky = globals::game::sky;

	// Time jumps (wait, sleep, fast travel, console), a stopped clock, a new sun path, weather or worldspace, or the model just switched on:
	// the per-4th-frame CPU work runs this very frame (the 50b anchor decided its snap after a recompute that had been skipped, so it
	// snapped to a target up to 3 frames old; nothing in this model is smoothed over time).
	const auto calendar = RE::Calendar::GetSingleton();
	const float hours = calendar ? calendar->GetHoursPassed() : 0.f;
	const float timeScale = calendar ? calendar->GetTimescale() : 0.f;
	const uint32_t warp = globals::features::skySync.GetSunWarpGeneration();
	const RE::TESWeather* weather = sky ? sky->currentWeather : nullptr;
	const RE::TESWorldSpace* worldspace = GetCurrentWorldspace();
	f.timeJump = !outdoorWasActive || !(timeScale > 0.f) || !(std::abs(hours - outdoorLastHours) < 0.01f) || warp != outdoorLastWarp ||
	             weather != outdoorLastWeather || worldspace != outdoorLastWorldspace;
	outdoorLastHours = hours;
	outdoorLastWarp = warp;
	outdoorLastWeather = weather;
	outdoorLastWorldspace = worldspace;
	outdoorWasActive = true;

	f.valid = sky && sky->currentWeather;
	f.sigmaNow = CurrentSunlightScale();
	if (!f.valid) {
		f.gCurrent = f.gLast = f.g = f.sunScale = 1.f;
		f.sigmaRef = f.sigmaNow;
		f.glareNight = 0.f;
		return;
	}
	const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.f, 1.f) : 1.f;
	const RE::TESWeather* last = (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.f) ? sky->lastWeather : nullptr;
	const WeatherKeys keysCur = ReadWeatherKeys(sky->currentWeather);
	const WeatherKeys keysLast = last ? ReadWeatherKeys(last) : keysCur;
	const auto& ref = weatherRef;

	// R1 gloominess: the weather's day light (ambient + sunlight x imagespace sunlight scale) / the reference's, [0.03, 1], day key only.
	auto gloom = [&](const WeatherKeys& k) {
		if (ref.source == 0 || !(ref.keys.e > 1e-6f) || !std::isfinite(k.e))
			return 1.f;
		return std::clamp(k.e / ref.keys.e, 0.03f, 1.f);
	};
	f.gCurrent = gloom(keysCur);
	f.gLast = last ? gloom(keysLast) : f.gCurrent;
	f.g = std::lerp(f.gLast, f.gCurrent, pct);
	const float strength = std::clamp(settings.outdoorLight.weatherGloominess, 0.f, 1.f);
	f.sunScale = std::pow(std::max(f.g, 0.03f), strength);
	if (!std::isfinite(f.sunScale))
		f.sunScale = 1.f;
	// The direct light's clear-noon constant: the reference's day sunlight scale (no reference: the weathers' own day keys, never the clock).
	f.sigmaRef = ref.source != 0 ? ref.keys.sigmaDay : std::lerp(keysLast.sigmaDay, keysCur.sigmaDay, pct);
	if (!std::isfinite(f.sigmaRef) || f.sigmaRef < 0.f)
		f.sigmaRef = 1.f;
	// R4 input: the weathers' night moon glare.
	f.glareNight = std::lerp(keysLast.glareNight, keysCur.glareNight, pct);
}

float PhysicalSky::MoonVisibility(const RE::Moon* a_moon, const RE::Sky* a_sky, float a_sinElevation)
{
	const auto& physicalSky = globals::features::physicalSky;
	if (physicalSky.OutdoorActive(physicalSky.settings.outdoorLight.moonFamily))
		return physicalSky.MoonVisibilityR4(a_moon, a_sky, a_sinElevation);
	return MoonHorizonVisibility(a_moon, a_sky, a_sinElevation);
}

float PhysicalSky::MoonVisibilityR4(const RE::Moon* a_moon, const RE::Sky* a_sky, float a_sinElevation) const
{
	if (!a_moon || !a_moon->moonMesh || !a_sky)
		return 0.f;
	// The engine's own hide (as MoonHorizonVisibility): the blended Moon Glare colour equal to the fixed hide colour (1.5.97), or black.
	const RE::NiColor& glare = a_sky->skyColor[RE::TESWeather::ColorTypes::kMoonGlare];
	static const RE::NiColor* hideColor = (!REL::Module::IsVR() && REL::Module::get().version() == SKSE::RUNTIME_SSE_1_5_97) ?
	                                          reinterpret_cast<const RE::NiColor*>(REL::Offset(0x30135F0).address()) :
	                                          nullptr;
	if (hideColor && glare.red == hideColor->red && glare.green == hideColor->green && glare.blue == hideColor->blue)
		return 0.f;
	if (!(std::max({ glare.red, glare.green, glare.blue }) > 0.f))
		return 0.f;
	// R4: the weathers' night moon glare relative to the reference's (vanilla uses it as a switch, NAT as a dimmer: both read right
	// against their own clear weathers; the 41b fixed threshold 0.1 is not used). No reference: visible (the hide above still applies).
	float weather = 1.f;
	if (weatherRef.source != 0 && weatherRef.keys.glareNight > 1e-6f)
		weather = std::clamp(outdoorFrame.glareNight / weatherRef.keys.glareNight, 0.f, 1.f);
	if (!std::isfinite(weather))
		weather = 1.f;
	const float elevationDeg = DirectX::XMConvertToDegrees(std::asin(std::clamp(a_sinElevation, -1.f, 1.f)));
	return weather * SkySync::SmoothStep(-2.f, 3.f, elevationDeg);
}

void PhysicalSky::UpdateKeyElevations()
{
	auto& k = keyElevations;
	k.valid = false;
	auto* sky = globals::game::sky;
	if (!sky || !sky->currentClimate)
		return;
	const auto& t = sky->currentClimate->timing;
	double srb = t.sunrise.begin / 6.0, sre = t.sunrise.end / 6.0, ssb = t.sunset.begin / 6.0, sse = t.sunset.end / 6.0;
	if (srb == 0.0 && sre == 0.0 && ssb == 0.0 && sse == 0.0)
		return;
	auto widen = [](double& a_begin, double& a_end) {
		if (a_end - a_begin < 0.5) {
			const double centre = (a_begin + a_end) * 0.5;
			a_begin = centre - 0.25;
			a_end = centre + 0.25;
		}
	};
	widen(srb, sre);
	widen(ssb, sse);
	// The engine's colour keys (as SkySync::SunWarp::Build): night until csb, sunrise peak at midR, day from sre to ssb, sunset peak at midS,
	// night from cse.
	const double ext = SkySync::GetDaytimeColorExtension();
	const double csb = std::max(0.0, srb - ext), cse = std::min(23.99, sse + ext);
	const double midR = csb + (sre - csb) * 0.5, midS = ssb + (cse - ssb) * 0.5;
	auto& skySync = globals::features::skySync;
	k.csb = skySync.SunElevationDegAtHour(sky, static_cast<float>(csb));
	k.midR = skySync.SunElevationDegAtHour(sky, static_cast<float>(midR));
	k.sre = skySync.SunElevationDegAtHour(sky, static_cast<float>(sre));
	k.ssb = skySync.SunElevationDegAtHour(sky, static_cast<float>(ssb));
	k.midS = skySync.SunElevationDegAtHour(sky, static_cast<float>(midS));
	k.cse = skySync.SunElevationDegAtHour(sky, static_cast<float>(cse));
	k.noonHour = static_cast<float>((sre + ssb) * 0.5);
	// Rising through the morning keys and setting through the evening ones; anything else (other realms' odd timings): not valid.
	k.valid = k.csb + 0.5f < k.midR && k.midR + 0.5f < k.sre && k.ssb > k.midS + 0.5f && k.midS > k.cse + 0.5f;
}

bool PhysicalSky::KeyWeightsByElevation(float a_sunElevationDeg, float a_weights[4]) const
{
	using ColorTime = RE::TESWeather::ColorTime;
	a_weights[0] = a_weights[1] = a_weights[2] = a_weights[3] = 0.f;
	const auto& k = keyElevations;
	auto* sky = globals::game::sky;
	const float hour = sky && std::isfinite(sky->currentGameHour) ? std::fmod(std::max(sky->currentGameHour, 0.f), 24.f) : 12.f;
	const bool morning = hour < k.noonHour;  // which side of the day: the sunrise or the sunset key
	const int edge = morning ? ColorTime::kSunrise : ColorTime::kSunset;
	const float el = a_sunElevationDeg;
	if (!k.valid || !std::isfinite(el)) {
		// No usable timing: a plain elevation ramp (night below -12, the edge key at 0, day from +20).
		if (!(el > -12.f))
			a_weights[ColorTime::kNight] = 1.f;
		else if (el < 0.f) {
			const float t = (el + 12.f) / 12.f;
			a_weights[ColorTime::kNight] = 1.f - t;
			a_weights[edge] = t;
		} else if (el < 20.f) {
			const float t = el / 20.f;
			a_weights[edge] = 1.f - t;
			a_weights[ColorTime::kDay] = t;
		} else
			a_weights[ColorTime::kDay] = 1.f;
		return false;
	}
	const float eNight = morning ? k.csb : k.cse;
	const float ePeak = morning ? k.midR : k.midS;
	const float eDay = morning ? k.sre : k.ssb;
	if (el <= eNight)
		a_weights[ColorTime::kNight] = 1.f;
	else if (el < ePeak) {
		const float t = (el - eNight) / (ePeak - eNight);
		a_weights[ColorTime::kNight] = 1.f - t;
		a_weights[edge] = t;
	} else if (el < eDay) {
		const float t = (el - ePeak) / (eDay - ePeak);
		a_weights[edge] = 1.f - t;
		a_weights[ColorTime::kDay] = t;
	} else
		a_weights[ColorTime::kDay] = 1.f;
	return true;
}

bool PhysicalSky::PlanckianBoundedChroma(const float3& a_linear, float3& a_unitLuminanceOut)
{
	a_unitLuminanceOut = float3(1.f, 1.f, 1.f);
	if (!AllFinite(a_linear))
		return false;
	const double r = std::max(a_linear.x, 0.f), g = std::max(a_linear.y, 0.f), b = std::max(a_linear.z, 0.f);
	// Linear sRGB (D65) -> XYZ -> CIE 1960 uv.
	const double X = 0.4124 * r + 0.3576 * g + 0.1805 * b;
	const double Y = 0.2126 * r + 0.7152 * g + 0.0722 * b;
	const double Z = 0.0193 * r + 0.1192 * g + 0.9505 * b;
	const double denom = X + 15.0 * Y + 3.0 * Z;
	if (!(Y > 1e-9) || !(denom > 1e-12))
		return false;
	double u = 4.0 * X / denom, v = 6.0 * Y / denom;
	// The Planckian locus (Krystek 1985), 1700 K .. 1e7 K, as a polyline in equal mired steps.
	static const std::vector<std::array<double, 2>> locus = [] {
		std::vector<std::array<double, 2>> points;
		constexpr int kSteps = 240;
		const double miredHot = 1e6 / 1e7, miredCold = 1e6 / 1700.0;
		for (int i = 0; i <= kSteps; ++i) {
			const double mired = miredCold + (miredHot - miredCold) * i / kSteps;
			const double T = 1e6 / mired;
			const double pu = (0.860117757 + 1.54118254e-4 * T + 1.28641212e-7 * T * T) / (1.0 + 8.42420235e-4 * T + 7.08145163e-7 * T * T);
			const double pv = (0.317398726 + 4.22806245e-5 * T + 4.20481691e-8 * T * T) / (1.0 - 2.89741816e-5 * T + 1.61456053e-7 * T * T);
			points.push_back({ pu, pv });
		}
		return points;
	}();
	double bestD2 = 1e30, bestU = u, bestV = v;
	for (size_t i = 0; i + 1 < locus.size(); ++i) {
		const double ax = locus[i][0], ay = locus[i][1], bx = locus[i + 1][0], by = locus[i + 1][1];
		const double dx = bx - ax, dy = by - ay;
		const double len2 = dx * dx + dy * dy;
		const double t = len2 > 0.0 ? std::clamp(((u - ax) * dx + (v - ay) * dy) / len2, 0.0, 1.0) : 0.0;
		const double px = ax + dx * t, py = ay + dy * t;
		const double d2 = (u - px) * (u - px) + (v - py) * (v - py);
		if (d2 < bestD2) {
			bestD2 = d2;
			bestU = px;
			bestV = py;
		}
	}
	const double d = std::sqrt(bestD2);
	if (d > kHueMaxDuv) {
		// Only the saturation goes: along the line to the nearest locus point, the hue direction kept.
		const double s = kHueMaxDuv / d;
		u = bestU + (u - bestU) * s;
		v = bestV + (v - bestV) * s;
	}
	const double den2 = 2.0 * u - 8.0 * v + 4.0;
	if (!(std::abs(den2) > 1e-12))
		return false;
	const double x = 3.0 * u / den2, y = 2.0 * v / den2;
	if (!(y > 1e-9))
		return false;
	const double X2 = x / y, Y2 = 1.0, Z2 = (1.0 - x - y) / y;
	float3 rgb(static_cast<float>(3.2406 * X2 - 1.5372 * Y2 - 0.4986 * Z2), static_cast<float>(-0.9689 * X2 + 1.8758 * Y2 + 0.0415 * Z2),
		static_cast<float>(0.0557 * X2 - 0.2040 * Y2 + 1.0570 * Z2));
	rgb = float3(std::max(rgb.x, 0.f), std::max(rgb.y, 0.f), std::max(rgb.z, 0.f));
	const float l = Lum3(rgb);
	if (!(l > 1e-6f) || !AllFinite(rgb))
		return false;
	a_unitLuminanceOut = rgb * (1.f / l);
	return true;
}

void PhysicalSky::UpdateOutdoorCalibration()
{
	// The ambient's one global constant: the reference's clear-noon directional ambient (mean of the six, linear) over the physical model's
	// clear noon (the sun at the path's peak, colour as set, gloominess 1, no moons, no glow, the same ground bounce). Likewise the clouds'
	// sky ambient: 50b read the DALC's mean colour (Stage2::Ambient), the new one reads the sky-view LUT at Sky.hlsl's 5 taps.
	auto& amb = outdoorAmbient;
	const uint32_t frame = globals::state->frameCount;
	if (amb.calibrationFrame != UINT32_MAX && frame - amb.calibrationFrame < 64u && !outdoorFrame.timeJump)
		return;
	amb.calibrationFrame = frame;
	amb.calibration = 0.f;
	amb.cloudAmbientCal = 0.f;
	if (!UpdateCpuSkyModel())
		return;
	const float sinTilt = std::abs(std::sin(DirectX::XMConvertToRadians(globals::features::skySync.GetSunAngle())));
	const double peak = std::asin(std::clamp(static_cast<double>(sinTilt), 0.17, 1.0));  // a flat path's peak counted no lower than 10 deg
	const PhysicalSkyModel::Light sun = { { std::cos(peak), 0.0, std::sin(peak) }, { settings.sunlightColor.x, settings.sunlightColor.y, settings.sunlightColor.z } };
	const double cameraRadius = static_cast<double>(cbData.zCameraPlanet) * Util::Units::GAME_UNIT_TO_KM;
	const auto tr = skyModelCpu.Transmittance(cameraRadius, sun.dir);
	const float3 direct = float3(static_cast<float>(sun.color[0] * tr[0]), static_cast<float>(sun.color[1] * tr[1]), static_cast<float>(sun.color[2] * tr[2])) *
	                      static_cast<float>(std::sin(peak));
	const SkyIrradiance noon = IntegrateSky(skyModelCpu, cameraRadius, &sun, 1, {}, {}, direct, settings.groundAlbedo);
	float mean = 0.f;
	for (int axis = 0; axis < 3; ++axis)
		for (int side = 0; side < 2; ++side)
			mean += Lum3(noon.e[axis][side]);
	mean /= 6.f;
	const auto& ref = weatherRef;
	const RE::TESWeather* current = globals::game::sky ? globals::game::sky->currentWeather : nullptr;
	const WeatherKeys fallback = ref.source == 0 ? ReadWeatherKeys(current) : WeatherKeys{};
	const float targetA = ref.source != 0 ? ref.keys.a : fallback.a;
	const float targetCloud = ref.source != 0 ? ref.keys.aCloud : fallback.aCloud;
	if (mean > 1e-9f && std::isfinite(mean) && targetA > 0.f)
		amb.calibration = targetA / mean;
	// Sky.hlsl's cloud sky ambient taps: 0.3 x the zenith + 4 x 0.175 at 15 degrees (the ring mean stands for the four).
	const auto zenith = skyModelCpu.SkyRay(cameraRadius, { 0.0, 0.0, 1.0 }, &sun, 1);
	const double lut = 0.3 * (0.2126 * zenith[0] + 0.7152 * zenith[1] + 0.0722 * zenith[2]) + 0.7 * skyModelCpu.RingLuminance(cameraRadius, 15.0, &sun, 1);
	if (lut > 1e-9 && std::isfinite(lut) && targetCloud > 0.f)
		amb.cloudAmbientCal = static_cast<float>(targetCloud / lut);
}

void PhysicalSky::UpdateOutdoorAmbient(const RE::NiPoint3& a_lutSunDir)
{
	auto& amb = outdoorAmbient;
	const uint32_t frame = globals::state->frameCount;
	if (amb.valid && amb.frame != UINT32_MAX && frame - amb.frame < 4u && !outdoorFrame.timeJump)
		return;
	amb.frame = frame;
	amb.valid = false;
	auto* sky = globals::game::sky;
	if (!sky || !sky->currentWeather || !UpdateCpuSkyModel())
		return;
	UpdateOutdoorCalibration();
	UpdateKeyElevations();
	if (!(amb.calibration > 0.f))
		return;

	// Luminance from physics: the sky as drawn (the sun family's scattering through cbData.sunlightColor, the moons' glows, the night glow) +
	// the ground's bounce of it and of the direct sun and moons, x the one calibration constant.
	const double cameraRadius = static_cast<double>(cbData.zCameraPlanet) * Util::Units::GAME_UNIT_TO_KM;
	const PhysicalSkyModel::Light lights[3] = {
		{ UnitVec(a_lutSunDir.x, a_lutSunDir.y, a_lutSunDir.z), { cbData.sunlightColor.x, cbData.sunlightColor.y, cbData.sunlightColor.z } },
		{ UnitVec(cbData.masserDir.x, cbData.masserDir.y, cbData.masserDir.z), { cbData.masserColor.x, cbData.masserColor.y, cbData.masserColor.z } },
		{ UnitVec(cbData.secundaDir.x, cbData.secundaDir.y, cbData.secundaDir.z), { cbData.secundaColor.x, cbData.secundaColor.y, cbData.secundaColor.z } },
	};
	const bool glowOn = (nightCbData.flags & kNightBaseLight) != 0;
	const float3 glowUpper = glowOn ? nightCbData.baseUpper : float3{};
	const float3 glowHorizon = glowOn ? nightCbData.baseHorizon : float3{};
	// Direct light on the ground (for the bounce): the sun through the air (none below the horizon), each moon as it lights the ground.
	float3 direct = {};
	{
		const auto trSun = skyModelCpu.Transmittance(cameraRadius, lights[0].dir);
		const float up = static_cast<float>(std::max(lights[0].dir[2], 0.0));
		direct += float3(static_cast<float>(lights[0].color[0] * trSun[0]), static_cast<float>(lights[0].color[1] * trSun[1]), static_cast<float>(lights[0].color[2] * trSun[2])) * up;
		const auto& skySync = globals::features::skySync;
		const float dayFade = SkySync::MoonDayFade(DirectX::XMConvertToDegrees(std::asin(std::clamp(skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)].z, -1.f, 1.f))));
		const RE::Moon* moons[2] = { sky->masser, sky->secunda };
		const float3* colors[2] = { &settings.masserColor, &settings.secundaColor };
		const SkySync::Caster casters[2] = { SkySync::Caster::Masser, SkySync::Caster::Secunda };
		for (int i = 0; i < 2; ++i) {
			if (!moons[i])
				continue;
			const float sinEl = skySync.rawDirections[static_cast<int>(casters[i])].z;
			const float strength = SkySync::PhaseFactorFromTexture(moons[i]) * MoonVisibility(moons[i], sky, sinEl) * dayFade * (i == 1 ? SkySync::SecundaIntensityFactor : 1.f);
			const auto trMoon = skyModelCpu.Transmittance(cameraRadius, lights[1 + i].dir);
			const float3 c = *colors[i] * (strength * std::max(sinEl, 0.f));
			direct += float3(c.x * static_cast<float>(trMoon[0]), c.y * static_cast<float>(trMoon[1]), c.z * static_cast<float>(trMoon[2]));
		}
	}
	const SkyIrradiance now = IntegrateSky(skyModelCpu, cameraRadius, lights, 3, glowUpper, glowHorizon, direct, settings.groundAlbedo);

	// Hue (R2): the weather's directional ambient sampled by the sun's elevation (R3), each direction's chromaticity bounded to 0.02 Duv from
	// the Planckian locus; the luminance stays the physical one. Weather Ambient Tint blends the physical colour toward it.
	float weights[4];
	KeyWeightsByElevation(DirectX::XMConvertToDegrees(std::asin(std::clamp(a_lutSunDir.z, -1.f, 1.f))), weights);
	const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.f, 1.f) : 1.f;
	const RE::TESWeather* last = (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.f) ? sky->lastWeather : nullptr;
	float3 weatherGamma[3][2] = {};
	auto addWeather = [&](const RE::TESWeather* a_weather, float a_share) {
		for (int key = 0; key < 4; ++key) {
			if (!(weights[key] > 0.f))
				continue;
			float3 c[3][2];
			DalcKey(a_weather, key, c);
			for (int axis = 0; axis < 3; ++axis)
				for (int side = 0; side < 2; ++side)
					weatherGamma[axis][side] += c[axis][side] * (weights[key] * a_share);
		}
	};
	addWeather(sky->currentWeather, last ? pct : 1.f);
	if (last)
		addWeather(last, 1.f - pct);
	const auto g = GetOutdoorGammas();
	const float tint = std::clamp(settings.outdoorLight.ambientTint, 0.f, 1.f);
	const float invGamma = 1.f / g.ambient;
	float3 gammaSpace[3][2];
	for (int axis = 0; axis < 3; ++axis)
		for (int side = 0; side < 2; ++side) {
			const float3 phys = now.e[axis][side] * amb.calibration;
			amb.physical[axis][side] = phys;
			const float l = Lum3(phys);
			float3 result = {};
			if (l > 0.f && AllFinite(phys)) {
				const float3 physChroma = phys * (1.f / l);
				const float3& wg = weatherGamma[axis][side];
				float3 weatherChroma;
				if (!PlanckianBoundedChroma(float3(std::pow(wg.x, g.ambient), std::pow(wg.y, g.ambient), std::pow(wg.z, g.ambient)), weatherChroma))
					weatherChroma = float3(1.f, 1.f, 1.f);  // black or unusable: neutral
				float3 chroma = physChroma + (weatherChroma - physChroma) * tint;
				chroma = float3(std::max(chroma.x, 0.f), std::max(chroma.y, 0.f), std::max(chroma.z, 0.f));
				const float cl = Lum3(chroma);
				result = cl > 1e-6f ? chroma * (l / cl) : float3(l, l, l);
			}
			amb.linear[axis][side] = result;
			// The engine stores what Lighting raises to the ambient gamma (Color::Ambient): the inverse here.
			gammaSpace[axis][side] = float3(std::pow(std::max(result.x, 0.f), invGamma), std::pow(std::max(result.y, 0.f), invGamma), std::pow(std::max(result.z, 0.f), invGamma));
			if (!AllFinite(gammaSpace[axis][side]))
				return;
		}
	// The engine's transform layout (checked against its own colours at run time, dalcConventionError): translation = the mean of the six,
	// rotate[channel][axis] = (+axis - -axis) / 2, so DALC x (n, 1) gives the mean plus the half difference along each axis.
	RE::NiTransform t{};
	float3 mean = {};
	for (int axis = 0; axis < 3; ++axis)
		mean += gammaSpace[axis][0] + gammaSpace[axis][1];
	mean = mean * (1.f / 6.f);
	t.translate = { mean.x, mean.y, mean.z };
	for (int axis = 0; axis < 3; ++axis) {
		const float3 half = (gammaSpace[axis][0] - gammaSpace[axis][1]) * 0.5f;
		t.rotate.entry[0][axis] = half.x;
		t.rotate.entry[1][axis] = half.y;
		t.rotate.entry[2][axis] = half.z;
	}
	t.scale = 1.f;
	amb.transform = t;
	amb.valid = true;
}

void PhysicalSky::ApplyOutdoorDalc()
{
	auto* sm = globals::game::smState;
	if (!sm)
		return;
	RE::NiTransform& current = sm->directionalAmbientTransform;
	const bool holdsOurs = dalcWritten && SameTransform(current, writtenDalc);
	if (!holdsOurs) {
		// The engine (re)wrote its own: keep it for the CPU readers and for putting back.
		engineDalc = current;
		dalcEngineSaved = true;
		dalcWritten = false;
		if (!dalcConventionChecked && globals::game::sky && outdoorActive) {
			// Self-check of the layout UpdateOutdoorAmbient writes: the engine's translation against the mean of its six colours.
			float3 mean = {};
			for (int axis = 0; axis < 3; ++axis)
				for (int side = 0; side < 2; ++side) {
					const auto& c = globals::game::sky->directionalAmbientColors[axis][side];
					mean += float3(c.red, c.green, c.blue);
				}
			mean = mean * (1.f / 6.f);
			if (Lum3(mean) > 1e-4f) {
				dalcConventionError = std::max({ std::abs(mean.x - current.translate.x), std::abs(mean.y - current.translate.y), std::abs(mean.z - current.translate.z) });
				dalcConventionChecked = true;
				logger::info("[Physical Sky] (51a) engine DALC layout check: |translation - mean of the six colours| = {:.5f}", dalcConventionError);
			}
		}
	}
	const bool want = OutdoorActive(settings.outdoorLight.ambient) && outdoorAmbient.valid;
	if (want) {
		std::memcpy(current.rotate.entry, outdoorAmbient.transform.rotate.entry, sizeof(current.rotate.entry));
		current.translate = outdoorAmbient.transform.translate;
		writtenDalc = current;
		dalcWritten = true;
	} else if (holdsOurs) {
		std::memcpy(current.rotate.entry, engineDalc.rotate.entry, sizeof(current.rotate.entry));
		current.translate = engineDalc.translate;
		dalcWritten = false;
	}
}

RE::NiPoint3 PhysicalSky::EngineDalcTranslate() const
{
	const auto* sm = globals::game::smState;
	if (!sm)
		return {};
	const RE::NiTransform& current = sm->directionalAmbientTransform;
	if (dalcWritten && dalcEngineSaved && SameTransform(current, writtenDalc))
		return engineDalc.translate;
	return current.translate;
}

std::string PhysicalSky::DescribeOutdoorLight() const
{
	const auto& ol = settings.outdoorLight;
	if (!OutdoorOn(true))
		return "physical outdoor light off";
	const auto& f = outdoorFrame;
	return std::format(
		"physical outdoor light on ({}; sun family {} g {:.3f} -> x{:.3f} [strength {:.2f}], sunlight scale now {:.2f} ref {:.2f}; moon family {}; night glow {} x{:.1f}; "
		"ambient {} tint {:.2f} cal {:.4f}; clouds {}; smooth sun {}; real ozone {}; reference source {} ({} entries))",
		outdoorActive ? "active" : "not here", ol.sunFamily ? "on" : "off", f.g, f.sunScale, ol.weatherGloominess, f.sigmaNow, f.sigmaRef,
		ol.moonFamily ? "on" : "off", ol.nightGlow ? "on" : "off", ol.nightSkyGlow, ol.ambient ? "on" : "off", ol.ambientTint, outdoorAmbient.calibration,
		ol.clouds ? "on" : "off", ol.smoothSun ? "on" : "off", ol.realOzone ? "on" : "off", weatherRef.source, weatherRef.entries);
}
