#include "PhysicalSky.h"

#include <cfloat>
#include <imgui_stdlib.h>

#include "CloudShadows.h"
#include "Deferred.h"
#include "LinearLighting.h"
#include "SkySync.h"
#include "TerrainShadows.h"

#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"

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
	proceduralSun,
	sunDiskRad,
	sunAlignToVanilla,
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
	// skyTrueSunHeight (and moonsFadeInDaylight in Load/SaveSettings): now fixed (see
	// UpdateEffective()); old keys are ignored on load. Code paths kept for a possible rollback
	// (specs/physsky-rollback-candidates.md).
	moonGlowStrength,
	moonRadianceCap,
	nightBaseLight,
	twilightLength,
	skyModel,
	// (44h) removed fixSkyAlpha/fixTrLutEdge/fixApShadowDepth/fixReflectionSky/fixMultiScatter:
	// now fixed on; old keys are ignored on load. Code paths kept for a possible rollback
	// (specs/physsky-rollback-candidates.md).
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

	/// (41b, F5) Shown where options are overridden by "Sky Model: Legacy (36f)".
	void LegacyNote(bool a_legacy)
	{
		if (a_legacy)
			ImGui::TextColored({ 1.f, 0.8f, 0.3f, 1.f }, "Sky Model is Legacy (36f): options added after 36f are not used right now.");
	}

	/// (46z) A settings value read without throwing: missing key, wrong type or a non-finite number all
	/// give the default.
	template <class T>
	T ReadSetting(const json& a_json, const char* a_key, T a_default)
	{
		if (!a_json.is_object())
			return a_default;
		const auto it = a_json.find(a_key);
		if (it == a_json.end())
			return a_default;
		try {
			if constexpr (std::is_same_v<T, bool>) {
				return it->is_boolean() ? it->get<bool>() : a_default;
			} else {
				if (!it->is_number())
					return a_default;
				const T value = it->get<T>();
				if constexpr (std::is_floating_point_v<T>) {
					if (!std::isfinite(value))
						return a_default;
				}
				return value;
			}
		} catch (...) {
			return a_default;
		}
	}

	/// (46z, C2/C3) Earth values (Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere
	/// Rendering Technique", and Bruneton 2017), in the Atmosphere tab's units (Mm^-1, km).
	constexpr float kEarthAerosolScatter = 3.996f;
	constexpr float kEarthAerosolAbsorption = 0.444f;  // extinction 4.44 - scattering 3.996
	constexpr float kEarthOzoneAbsorption[3] = { 0.650f, 1.881f, 0.085f };
	// Common.hlsli's ozone density is a tent: 1 at ozoneAltitude, 0 at +-ozoneThickness/2. Earth: 10-40 km.
	constexpr float kEarthOzoneAltitude = 25.f;
	constexpr float kEarthOzoneThickness = 30.f;
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
		// (44h) moonsFadeInDaylight no longer loaded: fixed on in UpdateEffective().
		settings.moonDaylightFade = o_json.value("moonDaylightFade", defaults.moonDaylightFade);
		settings.moonCloudLight = o_json.value("moonCloudLight", defaults.moonCloudLight);
		settings.moonCloudStrength = o_json.value("moonCloudStrength", defaults.moonCloudStrength);
		settings.moonCloudHalo = o_json.value("moonCloudHalo", defaults.moonCloudHalo);
	}
	{
		// (46z) Physical Sky fixes.
		const Settings defaults{};
		settings.sunHalo = ReadSetting(o_json, "sunHalo", defaults.sunHalo);
		settings.skyDither = ReadSetting(o_json, "skyDither", defaults.skyDither);
		settings.accurateLutIntegration = ReadSetting(o_json, "accurateLutIntegration", defaults.accurateLutIntegration);
		settings.vanillaMixFollowsSun = ReadSetting(o_json, "vanillaMixFollowsSun", defaults.vanillaMixFollowsSun);
		settings.aerosolPreset = ReadSetting(o_json, "aerosolPreset", defaults.aerosolPreset);
		settings.ozonePreset = ReadSetting(o_json, "ozonePreset", defaults.ozonePreset);
		settings.starsFadeWithSky = ReadSetting(o_json, "starsFadeWithSky", defaults.starsFadeWithSky);
		settings.starsFadeStrength = ReadSetting(o_json, "starsFadeStrength", defaults.starsFadeStrength);
		settings.highResAerialPerspective = ReadSetting(o_json, "highResAerialPerspective", defaults.highResAerialPerspective);
	}
	settings.sunHalo = std::clamp(settings.sunHalo, 0, kSunHaloCount - 1);
	settings.skyDither = std::clamp(settings.skyDither, 0, kSkyDitherCount - 1);
	settings.aerosolPreset = std::clamp(settings.aerosolPreset, 0, kAtmospherePresetCount - 1);
	settings.ozonePreset = std::clamp(settings.ozonePreset, 0, kAtmospherePresetCount - 1);
	settings.starsFadeStrength = std::clamp(settings.starsFadeStrength, 10.f, 10000.f);
	settings.skyModel = std::clamp(settings.skyModel, 0, kSkyModelCount - 1);
	settings.nightBaseLight = std::clamp(settings.nightBaseLight, 0.f, 1.f);
	settings.twilightLength = std::clamp(settings.twilightLength, 1.f, 3.f);
	settings.cloudLighting = std::clamp(settings.cloudLighting, 0, kCloudLightingCount - 1);
	settings.cloudSkyAmbient = std::clamp(settings.cloudSkyAmbient, 0.f, 4.f);
	settings.cloudLightHeightKm = std::clamp(settings.cloudLightHeightKm, 0.5f, 12.f);
	settings.moonDaylightFade = std::clamp(settings.moonDaylightFade, 0.f, 100.f);
	settings.moonCloudStrength = std::clamp(settings.moonCloudStrength, 0.f, 3.f);
	settings.moonCloudHalo = std::clamp(settings.moonCloudHalo, 0.f, 4.f);

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

	if (UseLegacyPaths()) {
		// 37a: TES worldspace only, hard-coded list, no exclusions.
		auto* tes = RE::TES::GetSingleton();
		auto* worldspace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		if (!worldspace)
			return WorldspaceStatus::Unknown;
		const auto& legacy = LegacyWorldspaceWhitelist();
		if (auto it = legacy.find(worldspace->GetFormEditorID()); it != legacy.end()) {
			a_zBottom = it->second.zBottom;
			return WorldspaceStatus::Whitelist;
		}
		return WorldspaceStatus::NotListed;
	}

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
	// (44h) moonsFadeInDaylight no longer saved: fixed on in UpdateEffective().
	o_json["moonDaylightFade"] = settings.moonDaylightFade;
	o_json["moonCloudLight"] = settings.moonCloudLight;
	o_json["moonCloudStrength"] = settings.moonCloudStrength;
	o_json["moonCloudHalo"] = settings.moonCloudHalo;
	// (46z) See LoadSettings.
	o_json["sunHalo"] = settings.sunHalo;
	o_json["skyDither"] = settings.skyDither;
	o_json["accurateLutIntegration"] = settings.accurateLutIntegration;
	o_json["vanillaMixFollowsSun"] = settings.vanillaMixFollowsSun;
	o_json["aerosolPreset"] = settings.aerosolPreset;
	o_json["ozonePreset"] = settings.ozonePreset;
	o_json["starsFadeWithSky"] = settings.starsFadeWithSky;
	o_json["starsFadeStrength"] = settings.starsFadeStrength;
	o_json["highResAerialPerspective"] = settings.highResAerialPerspective;
}

void PhysicalSky::DrawSettings()
{
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

	static constexpr const char* skyModelNames[kSkyModelCount] = { "Current", "Legacy (36f)" };
	ImGui::Combo("Sky Model", &settings.skyModel, skyModelNames, kSkyModelCount);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"For comparing only. Legacy (36f): every Physical Sky and Sky Sync option added after 36f is\n"
			"used at its 36f value while this is selected (sun look and alignment, fixes, moon glow at full\n"
			"strength without fading, new-moon disc, Vanilla moon orbit, discs lowered by altitude, no night\n"
			"base light, no twilight changes, the old worldspace list, the 42a cloud lighting and moon discs, the\n"
			"Clock light source model). Your own values are kept and come back\n"
			"with Current. Colours, exposures, atmosphere and clouds are always yours.");
	LegacyNote(IsLegacy());

	SettingsWorldspaces();

	ImGui::SeparatorText("Post Processing");
	{
		ImGui::InputFloat("Day Exposure", &settings.dayExposure);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Brightness of the sky and sun/moon light by day. Higher = brighter.");
		settings.dayExposure = std::max(1e-10f, settings.dayExposure);
		ImGui::InputFloat("Night Exposure", &settings.nightExposure);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Same, but at night. Higher = brighter nights.");
		settings.nightExposure = std::max(1e-10f, settings.nightExposure);
		ImGui::SliderAngle("Adaptation Start", &settings.adaptationStart, -90.f, 0.f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Sun angle below the horizon where the switch from Day to Night Exposure begins.");
		ImGui::SliderAngle("Adaptation End", &settings.adaptationEnd, -90.f, 0.f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Sun angle below the horizon where Night Exposure is fully reached.");

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
		// (46z, C1)
		ImGui::Checkbox("Vanilla Mix Follows Sun Height", &settings.vanillaMixFollowsSun);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Vanilla Mix only at night: in full while the sun is more than 6 degrees below the horizon,\n"
				"fading out until sunrise, none by day (and back in after sunset).\n"
				"By day the physical sky is far darker than the vanilla sky colours, so even a small Vanilla Mix\n"
				"decides the colour of the zenith (grey with many weathers) and tints the dawn horizon purple.\n"
				"Off = Vanilla Mix at all times (as before).");
		if (settings.vanillaMixFollowsSun)
			ImGui::TextDisabled("Vanilla Mix now: %.2f", cbData.vanillaMix);

		// (46z, A2)
		static constexpr const char* skyDitherNames[kSkyDitherCount] = { "Off", "Fine (R11G11B10)", "Vanilla strength" };
		ImGui::Combo("Sky Dither", &settings.skyDither, skyDitherNames, kSkyDitherCount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"A little noise on the physical sky so smooth colour gradients do not show steps (bands).\n"
				"The game's main picture keeps only about 2 significant digits per colour (R11G11B10), and the vanilla\n"
				"sky's own noise is mostly gone under the physical sky.\n"
				"Fine (default): changes every pixel and frame by less than half a step; DLSS smooths it away.\n"
				"Vanilla strength: the vanilla sky's noise pattern in full (coarser, visible grain).\n"
				"Off: as before.");
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
	LegacyNote(IsLegacy());

	ImGui::Checkbox("Override Directional Light", &settings.overrideDirLight);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Overrides the color of directional light. Linear tonemapper and 1.0 transmittance mix are recommended.");
	ImGui::SliderFloat("Transmittance Mix", &settings.trMix, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Apply additional atmospheric tranmisttance on the directional light.\n"
			"Introduces natural yellowening at sunset with white sunlight.");

	ImGui::SeparatorText("Sun");
	{
		ImGui::PushID("Sun");
		ImGui::ColorEdit3("Light Color", &settings.sunlightColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		static constexpr const char* sunLookNames[kSunLookCount] = { "Custom", "Bright (realistic)", "Soft", "Vanilla sun (procedural off)" };
		int look = settings.proceduralSun ? std::clamp(settings.sunLook, 0, kSunLookCount - 1) : static_cast<int>(kSunLookVanilla);
		if (ImGui::Combo("Sun Look", &look, sunLookNames, kSunLookCount))
			ApplySunLook(look);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"One-click looks for the sun:\n"
				"Bright: real-size disk with a strong glow around it, reads as blinding.\n"
				"Soft: slightly larger disk, gentle glow.\n"
				"Vanilla sun: procedural sun off, the game's own sun picture.\n"
				"Moving any sun setting below switches this to Custom.");
		if (ImGui::Checkbox("Procedural Sun", &settings.proceduralSun))
			settings.sunLook = settings.proceduralSun ? kSunLookCustom : kSunLookVanilla;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Draws the sun as part of the physical sky: a real-size disk whose colour and brightness\n"
				"come from the atmosphere (white at noon, orange and dimmer at sunset, gone below the\n"
				"horizon), instead of the game's fixed sun picture. Off: the game's sun picture.");
		if (ImGui::SliderFloat("Sun Disk Angular Radius", &settings.sunDiskRadiusDeg, 0.05f, 2.f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
			settings.sunLook = kSunLookCustom;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Radius of the disk. 0.27 = the real sun (0.53 degrees across).");
		ImGui::SameLine();
		ImGui::TextDisabled("(%.2f deg across)", settings.sunDiskRadiusDeg * 2.f);

		// (46z, A1)
		static constexpr const char* sunHaloNames[kSunHaloCount] = { "Off (disc only)", "Smooth", "Keep (as before)" };
		ImGui::Combo("Vanilla Sun Halo (with Physical Sky)", &settings.sunHalo, sunHaloNames, kSunHaloCount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"The big halo painted into the game's sun picture (and its sun glare picture). Its see-through\n"
				"edge is stored in coarse steps; over the dark physical sky at dawn they show as rings and a\n"
				"hard-edged red disc, even while the sun is still below the horizon.\n"
				"Smooth (default): the steps are blurred out, and the halo fades in only as the physical sun rises\n"
				"(from 1 degree below the horizon to 2 above) and by how much light gets through the air.\n"
				"Off: only the solid sun disc, no halo, no glare.\n"
				"Keep: as before.\n"
				"Applies to the game's sun picture (Sun Look \"Vanilla sun\"; with the procedural sun, to the glare).");

		// (44h) The procedural-sun options are hidden (not just disabled) while Procedural Sun is
		// off; their values are still saved and loaded.
		if (settings.proceduralSun) {
			ImGui::SeparatorText("Procedural Sun");
			ImGui::Checkbox("Align with Vanilla Sun", &settings.sunAlignToVanilla);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Centres the disk (and the sky's glow around the sun) exactly where the game draws its sun.\n"
					"Off: the old direction, which drifts 2-4 degrees off the game's sun as you climb.");
			if (ImGui::Checkbox("Replace Vanilla Sun", &settings.sunReplaceVanilla))
				settings.sunLook = kSunLookCustom;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("The disk replaces the game's sun picture instead of being added next to it (no second sun).");
			if (ImGui::Checkbox("Soft Edge", &settings.sunSoftEdge))
				settings.sunLook = kSunLookCustom;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Fades the rim of the disk so it does not shimmer with DLSS.");
			if (ImGui::Checkbox("Physical Brightness", &settings.sunPhysicalRadiance))
				settings.sunLook = kSunLookCustom;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Makes the disk as bright as a real sun for its size, limited by the cap below.\n"
					"Anything far above a sunlit white wall already shows as pure white on screen; the extra\n"
					"only matters for bloom (COD Bloom). Off: the old dim flat disk.");
			if (settings.sunPhysicalRadiance) {
				if (ImGui::SliderFloat("Brightness Cap", &settings.sunRadianceCap, 10.f, 62250.f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
					settings.sunLook = kSunLookCustom;
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text(
						"Upper limit of the disk's brightness (1 = about a sunlit white wall). Only changes how\n"
						"strongly COD Bloom spreads the sun; lower = less DLSS/frame-gen shimmer. Upstream 62250.");
			}
			if (ImGui::SliderFloat("Sun Glow", &settings.sunGlowIntensity, 0.f, 30.f, "%.1f", ImGuiSliderFlags_AlwaysClamp))
				settings.sunLook = kSunLookCustom;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"A glow in the sky around the disk, coloured by the atmosphere like the disk.\n"
					"This is what makes the sun read as blinding: the game has no bloom on it unless COD Bloom\n"
					"is on. 1 = as bright as a sunlit white wall at the disk's edge. 0 = no glow.");
			if (ImGui::SliderFloat("Sun Glow Width", &settings.sunGlowWidthDeg, 0.1f, 5.f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic))
				settings.sunLook = kSunLookCustom;
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How far the glow reaches from the disk's edge before fading (a faint wider tail follows).");
			ImGui::Checkbox("Hide Vanilla Sun Glare", &settings.sunHideVanillaGlare);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Removes the game's large halo around the sun while the procedural sun is on.");
		}
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
	// (44h) removed "Sky Uses True Sun Height": now fixed on (UpdateEffective()).
	ImGui::SliderFloat("Twilight Length", &settings.twilightLength, 1.f, 3.f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Stretches dawn and dusk in the sky only: with 2, a sun 12 degrees below the horizon lights the sky\n"
			"like one 6 degrees below, so the sky starts to brighten earlier and stays lit longer after sunset.\n"
			"Does not change the sun or moon light on the ground. 1 = real twilight (default).");
	// (46z, D1)
	ImGui::Checkbox("Stars Fade With Sky Brightness", &settings.starsFadeWithSky);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Stars (and the Milky Way painted with them) disappear as the physical sky behind them brightens,\n"
			"first near the bright horizon, as real stars are lost against the dawn sky. Nothing changes in full\n"
			"night (sun more than 18 degrees below the horizon).\n"
			"Off = the weather alone decides the stars' brightness (as before), which keeps them bright while the\n"
			"physical sky already shows dawn.");
	if (settings.starsFadeWithSky) {
		ImGui::SliderFloat("Stars Fade Strength", &settings.starsFadeStrength, 10.f, 10000.f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Higher = stars vanish earlier in the dawn (and later at dusk). Default 1000.");
	}

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
	// At Horizon), "Moon Physical Brightness" and "Moons Fade in Daylight" (fixed on): see
	// UpdateEffective(). Their sliders below are now always shown.
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

	// (44h) removed the "New Moon" section ("Hide New Moon Disc"): now fixed on (UpdateEffective()).
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

	// (46z, E1)
	ImGui::Checkbox("High-Resolution Aerial Perspective", &settings.highResAerialPerspective);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"The haze laid over distant land is looked up in a table of directions. On: 64 x 64 directions\n"
			"instead of 32 x 32 (about 6 degrees apart instead of 11), so the bright haze towards the sun stays a\n"
			"narrow glow instead of being smeared over a wide part of the view. About +0.02 ms. Off = as before.");
	// (46z, C4 + C5)
	ImGui::Checkbox("Accurate LUT Integration", &settings.accurateLutIntegration);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How the sky tables are summed up along each line of sight. On: each step is sampled in its middle\n"
			"instead of at its far end (twilight horizons were up to 2.5x too bright and purple, the sky near a low\n"
			"sun too dark), and the light bounced off the ground is no longer about 3x too strong (it greyed the\n"
			"whole sky a little). Off = as before.");

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

		ImGui::SliderFloat("Anisotropy", &settings.aerosolPhaseG, -1, 1);
		// (46z, C2)
		static constexpr const char* aerosolPresetNames[kAtmospherePresetCount] = { "Current (values below)", "Earth Clear" };
		ImGui::Combo("Aerosol Preset", &settings.aerosolPreset, aerosolPresetNames, kAtmospherePresetCount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How much dust and haze is in the air.\n"
				"Earth Clear: a clear day on Earth (scatter 3.996, absorption 0.444), a tenth of the default values\n"
				"below. Less white haze over distant land, a smaller bright patch around the sun, a bluer zenith and\n"
				"less dark-red horizons at dawn. Your values below are kept and come back with Current.");
		const bool aerosolPreset = settings.aerosolPreset != kAtmospherePresetCurrent;
		ImGui::BeginDisabled(aerosolPreset);
		ImGui::ColorEdit3("Scatter", &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3("Absorption", &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Usually 1/9 of scatter coefficient. Dust/pollution is lower, fog is higher.");
		ImGui::EndDisabled();
		if (aerosolPreset)
			ImGui::TextDisabled("In use: scatter %.3f, absorption %.3f (Earth Clear).", kEarthAerosolScatter, kEarthAerosolAbsorption);
		ImGui::SliderFloat("Falloff", &settings.aerosolFalloff, 0.f, 2.f, "%.2f km^-1");
		ImGui::PopID();
	}

	ImGui::SeparatorText("Ozone");
	{
		ImGui::PushID("Ozone");
		ImGui::TextWrapped(
			"The ozone layer high up in the sky that mainly absorbs light of certain wavelength. "
			"It keeps the zenith sky blue, especially at sunrise or sunset.");

		// (46z, C3)
		static constexpr const char* ozonePresetNames[kAtmospherePresetCount] = { "Current (values below)", "Earth" };
		ImGui::Combo("Ozone Preset", &settings.ozonePreset, ozonePresetNames, kAtmospherePresetCount);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Earth: real ozone, which takes out mostly green and some red (absorption 0.650, 1.881, 0.085)\n"
				"in a layer from 10 to 40 km up. It gives the deep blue-violet zenith of the blue hour. The default\n"
				"values below take out mostly red, higher up (22-58 km). Your values below are kept and come back\n"
				"with Current.");
		const bool ozonePreset = settings.ozonePreset != kAtmospherePresetCurrent;
		ImGui::BeginDisabled(ozonePreset);
		ImGui::ColorEdit3("Absorption", &settings.ozoneAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::DragFloat("Mean Altitude", &settings.ozoneAltitude, .1f, 0.f, 100.f, "%.3f km");
		ImGui::DragFloat("Layer Thickness", &settings.ozoneThickness, .1f, 0.f, 50.f, "%.3f km");
		ImGui::EndDisabled();
		if (ozonePreset)
			ImGui::TextDisabled("In use: absorption %.3f, %.3f, %.3f, altitude %.0f km, thickness %.0f km (Earth).",
				kEarthOzoneAbsorption[0], kEarthOzoneAbsorption[1], kEarthOzoneAbsorption[2], kEarthOzoneAltitude, kEarthOzoneThickness);
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
	// Depth Fix, Reflected Sky Fix, Multiple Scattering Fix): now fixed on; code paths kept for a
	// possible rollback (specs/physsky-rollback-candidates.md).
}

void PhysicalSky::SettingsClouds()
{
	InfoBox("Clouds.");
	LegacyNote(IsLegacy());

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

	ImGui::SeparatorText("Values");
	{
		ImGui::InputFloat3("Sun Direction", &cbData.sunDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3("Masser Direction", &cbData.masserDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
		ImGui::InputFloat3("Secunda Direction", &cbData.secundaDir.x, "%.3f", ImGuiInputTextFlags_ReadOnly);
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

		// (46z, E1) Created at the size the setting asks for; UpdateApLutSize() follows later changes.
		const bool hiRes = loaded && Effective().highResAerialPerspective;
		if (!CreateApLut(hiRes ? kApLutHiResW : kApLutW, hiRes ? kApLutHiResH : kApLutH) && !CreateApLut(kApLutW, kApLutH))
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
		// The old texture may still be bound from the last frame (t63, the deferred composite): D3D11
		// keeps its own reference to bound views, so releasing ours here is safe. Everything binds
		// texApLut afresh each frame after the LUTs are generated.
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
	const bool hiRes = Effective().highResAerialPerspective;
	const uint16_t w = hiRes ? kApLutHiResW : kApLutW;
	const uint16_t h = hiRes ? kApLutHiResH : kApLutH;
	if (texApLut && w == apLutW && h == apLutH)
		return;
	static bool failedOnce = false;  // do not retry a failing size every frame
	if (failedOnce && texApLut)
		return;
	if (!CreateApLut(w, h))
		failedOnce = true;
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
	if (!loaded || UseLegacyPaths())
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
	if (s.fixSkyAlpha)
		flags |= kExtSkyAlphaOpaque;
	if (s.fixTrLutEdge)
		flags |= kExtTrLutEdgeFix;
	if (s.fixApShadowDepth)
		flags |= kExtApShadowDepthFix;
	if (s.fixReflectionSky)
		flags |= kExtReflectionSkyFix;
	if (s.fixMultiScatter)
		flags |= kExtMultiScatterFix;
	if (s.moonPhysicalRadiance)
		flags |= kExtMoonPhysicalRadiance;
	// (46z)
	if (s.sunHalo == kSunHaloSmooth)
		flags |= kExtSunHaloSmooth;
	else if (s.sunHalo == kSunHaloOff)
		flags |= kExtSunHaloOff;
	if (s.skyDither == kSkyDitherFine)
		flags |= kExtSkyDitherFine;
	else if (s.skyDither == kSkyDitherVanilla)
		flags |= kExtSkyDitherVanilla;
	if (s.accurateLutIntegration)
		flags |= kExtAccurateLut;
	if (s.starsFadeWithSky)
		flags |= kExtStarsFade;
	extCbData.flags = flags;
	extCbData.sunRadianceCap = std::clamp(s.sunRadianceCap, 1.f, 62250.f);
	extCbData.sunGlowIntensity = s.proceduralSun ? std::clamp(s.sunGlowIntensity, 0.f, 30.f) : 0.f;
	extCbData.sunGlowWidth = DirectX::XMConvertToRadians(std::clamp(s.sunGlowWidthDeg, 0.1f, 5.f));
}

void PhysicalSky::UpdateMoonData(float a_exposure, float3& a_masserGlow, float3& a_secundaGlow)
{
	// (40d) The moon colours stay the user's (they light the scene through Sky Sync); only their
	// glow in the scattering LUTs is scaled here.
	const auto& s = effective;
	const float strength = std::clamp(s.moonGlowStrength, 0.f, 1.f);
	a_masserGlow = settings.masserColor * (a_exposure * strength);
	a_secundaGlow = settings.secundaColor * (a_exposure * strength);
	moonCbData = {};

	auto* sky = globals::game::sky;
	if (!sky)
		return;

	if (s.moonGlowFollowsMoon) {
		// A new moon (or a moon the game has hidden or faded out) no longer lights the sky.
		float masserVisibility, secundaVisibility;
		if (s.moonGlowFade == kMoonGlowFadeAtHorizon) {
			// (41b, F2) The vanilla disc is faded out 16-28 degrees above the horizon (orbit angle
			// 145-160), long before the moon sets; the glow now follows the moon down to it.
			const auto& skySync = globals::features::skySync;
			masserVisibility = MoonHorizonVisibility(sky->masser, sky, skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)].z);
			secundaVisibility = MoonHorizonVisibility(sky->secunda, sky, skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)].z);
		} else {
			masserVisibility = SkySync::VanillaVisibility(sky->masser);
			secundaVisibility = SkySync::VanillaVisibility(sky->secunda);
		}
		a_masserGlow = a_masserGlow * (SkySync::PhaseFactorFromTexture(sky->masser) * masserVisibility);
		a_secundaGlow = a_secundaGlow * (SkySync::PhaseFactorFromTexture(sky->secunda) * secundaVisibility);
	}

	if (s.moonPhysicalRadiance) {
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
			return std::clamp(irradiance / std::max(solidAngle, 1e-6f), 1.f, cap);
		};
		moonCbData.masserDiskScale = discScale(sky->masser, settings.masserColor);
		moonCbData.secundaDiskScale = discScale(sky->secunda, settings.secundaColor);
	}
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

	const float strength = std::clamp(effective.nightBaseLight, 0.f, 1.f);
	if (!(strength > 0.f))
		return;  // 0: no flag, the LUTs run exactly the 41a code
	// Gone once the sun's own twilight has taken over: full at -6 degrees, 0 at +4.
	const float fade = 1.f - SkySync::SmoothStep(-6.f, 4.f, DirectX::XMConvertToDegrees(a_sunElevation));
	if (!(fade > 0.f))
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
	nightCbData.baseUpper = toLinear(upper);
	nightCbData.baseHorizon = toLinear(horizon);
	nightCbData.baseLower = toLinear(lower);
	nightCbData.flags = kNightBaseLight;
}

void PhysicalSky::UpdateCloudData(float a_exposure, float a_sunElevation)
{
	cloudCbData = {};
	const auto& s = effective;

	if (s.moonsFadeInDaylight) {
		cloudCbData.flags |= kCloudMoonDaylightFade;
		cloudCbData.moonDaylightFade = std::clamp(s.moonDaylightFade, 0.f, 100.f);
	}

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
	// Cloud Shadows' layer is 0.5 km above the camera's ground; only the extra goes in.
	cloudCbData.heightOffset = (std::clamp(s.cloudLightHeightKm, 0.5f, 12.f) - 0.5f) / Util::Units::GAME_UNIT_TO_KM;
}

bool PhysicalSky::UseLegacyPaths() const
{
	return IsLegacy();
}

void PhysicalSky::UpdateEffective()
{
	effective = settings;
	// (44h) The five atmosphere fixes are no longer user settings: pinned on here so nothing that
	// copies or loads `settings` can turn them off. Legacy (36f) below still sets them off (and
	// UseLegacyPaths() skips the ext flags anyway). Code paths kept for a possible rollback
	// (specs/physsky-rollback-candidates.md).
	effective.fixSkyAlpha = true;
	effective.fixTrLutEdge = true;
	effective.fixApShadowDepth = true;
	effective.fixReflectionSky = true;
	effective.fixMultiScatter = true;
	// (44h) Celestials options removed from the menu and the JSON, pinned at the user's values the
	// same way. Legacy (36f) below still sets them to their 36f values. Code paths kept for a
	// possible rollback (specs/physsky-rollback-candidates.md).
	effective.skyTrueSunHeight = true;
	effective.moonGlowFollowsMoon = true;
	effective.moonGlowFade = kMoonGlowFadeAtHorizon;
	effective.moonPhysicalRadiance = true;
	effective.moonsFadeInDaylight = true;
	effective.hideNewMoonDisc = true;
	if (!IsLegacy())
		return;

	// (41b, F5) Legacy (36f): every Physical Sky option added after 36f at its 36f value. The
	// worldspace list and the 37a sun disc size come from UseLegacyPaths(). Kept as the user set
	// them: enabled, colours, exposures, tonemapper, mixes, procedural sun on/off, atmosphere,
	// clouds.
	auto& e = effective;
	// 37b
	e.enableAllExteriorWorldspaces = false;
	e.sunAlignToVanilla = false;
	e.sunReplaceVanilla = false;
	e.sunSoftEdge = false;
	e.sunPhysicalRadiance = false;
	e.sunHideVanillaGlare = false;
	e.fixSkyAlpha = false;
	e.fixTrLutEdge = false;
	e.fixApShadowDepth = false;
	e.fixReflectionSky = false;
	e.fixMultiScatter = false;
	// 37c
	e.sunGlowIntensity = 0.f;
	e.hideNewMoonDisc = false;
	// 40d
	e.moonGlowStrength = 1.f;
	e.moonGlowFollowsMoon = false;
	e.moonPhysicalRadiance = false;
	// 41b
	e.moonGlowFade = kMoonGlowFadeWithDisc;
	e.nightBaseLight = 0.f;
	e.skyTrueSunHeight = false;
	e.twilightLength = 1.f;
	// 42b
	e.cloudLighting = kCloudLightingSingle;
	e.moonsFadeInDaylight = false;
	// 44h
	e.moonCloudLight = false;
	// 46z
	e.sunHalo = kSunHaloKeep;
	e.skyDither = kSkyDitherOff;
	e.accurateLutIntegration = false;
	e.vanillaMixFollowsSun = false;
	e.aerosolPreset = kAtmospherePresetCurrent;
	e.ozonePreset = kAtmospherePresetCurrent;
	e.starsFadeWithSky = false;
	e.highResAerialPerspective = false;
}

void PhysicalSky::ApplySunLook(int a_look)
{
	settings.sunLook = std::clamp(a_look, 0, kSunLookCount - 1);
	switch (settings.sunLook) {
	case kSunLookBright:
		settings.proceduralSun = true;
		settings.sunReplaceVanilla = true;
		settings.sunSoftEdge = true;
		settings.sunPhysicalRadiance = true;
		settings.sunDiskRadiusDeg = 0.27f;
		settings.sunRadianceCap = 4000.f;
		settings.sunGlowIntensity = 6.f;
		settings.sunGlowWidthDeg = 0.8f;
		break;
	case kSunLookSoft:
		settings.proceduralSun = true;
		settings.sunReplaceVanilla = true;
		settings.sunSoftEdge = true;
		settings.sunPhysicalRadiance = true;
		settings.sunDiskRadiusDeg = 0.4f;
		settings.sunRadianceCap = 100.f;
		settings.sunGlowIntensity = 2.f;
		settings.sunGlowWidthDeg = 1.5f;
		break;
	case kSunLookVanilla:
		settings.proceduralSun = false;
		break;
	default:
		break;
	}
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
	// With the 37b master off this is the 37a hard-coded list.
	WorldspaceInfo worldspaceInfo = {};
	const auto worldspaceStatus = GetWorldspaceStatus(worldspaceInfo.zBottom);
	const bool worldspaceEnabled = worldspaceStatus == WorldspaceStatus::Whitelist || worldspaceStatus == WorldspaceStatus::AllExteriors;
	allGood &= worldspaceEnabled && !inMainLoadingMenu;

	if (!allGood) {
		if (skySync.loaded && skySync.settings.Enabled)
			skySync.lightColors = std::nullopt;
		cbData.enabled = allGood;
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
	if (s.sunAlignToVanilla) {
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
		if (s.skyTrueSunHeight && trueDir.Unitize() > FLT_EPSILON)
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
	// the altitude dip.
	if (skySync.Effective().KeepMoonPosition) {
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

	float3 masserGlow, secundaGlow;
	UpdateMoonData(exposure, masserGlow, secundaGlow);
	UpdateCloudData(exposure, std::asin(std::clamp(skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)].z, -1.f, 1.f)));

	// (46z, D1) Stars Fade With Sky Brightness (Sky.hlsl reads it with the ExtFlags bit).
	nightCbData.starsFadeStrength = s.starsFadeWithSky ? std::clamp(s.starsFadeStrength, 10.f, 10000.f) : 0.f;

	// (46z, C1) Vanilla Mix by the sun height the sky itself uses: full below -6 degrees, 0 from 0 up.
	float vanillaMix = std::clamp(settings.vanillaMix, 0.f, 1.f);
	if (s.vanillaMixFollowsSun) {
		const float lutSunElevationDeg = DirectX::XMConvertToDegrees(std::asin(std::clamp(lutSunDir.z, -1.f, 1.f)));
		vanillaMix *= 1.f - SkySync::SmoothStep(-6.f, 0.f, lutSunElevationDeg);
	}

	// (46z, C2/C3) Presets replace the user's values only here, at upload; the settings keep theirs.
	float3 aerosolScatter = settings.aerosolScatter;
	float3 aerosolAbsorption = settings.aerosolAbsorption;
	if (s.aerosolPreset == kAtmospherePresetEarth) {
		aerosolScatter = float3(kEarthAerosolScatter, kEarthAerosolScatter, kEarthAerosolScatter);
		aerosolAbsorption = float3(kEarthAerosolAbsorption, kEarthAerosolAbsorption, kEarthAerosolAbsorption);
	}
	float3 ozoneAbsorption = settings.ozoneAbsorption;
	float ozoneAltitude = settings.ozoneAltitude;
	float ozoneThickness = settings.ozoneThickness;
	if (s.ozonePreset == kAtmospherePresetEarth) {
		ozoneAbsorption = float3(kEarthOzoneAbsorption[0], kEarthOzoneAbsorption[1], kEarthOzoneAbsorption[2]);
		ozoneAltitude = kEarthOzoneAltitude;
		ozoneThickness = kEarthOzoneThickness;
	}

	cbData = {
		.texDim = res,
		.rcpTexDim = float2(1.0f) / res,
		.frameDim = dynres,
		.rcpFrameDim = float2(1.0f) / dynres,
		.sunDir = { sunDir.x, sunDir.y, sunDir.z },
		.sunlightColor = settings.sunlightColor * exposure,
		.trMix = settings.trMix,
		.masserDir = { masserDir.x, masserDir.y, masserDir.z },
		.apLumMix = settings.apLumMix,
		.masserColor = masserGlow,
		.apTrMix = settings.apTrMix,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.sunDiskCos = cos(!UseLegacyPaths() ? DirectX::XMConvertToRadians(std::clamp(s.sunDiskRadiusDeg, 0.05f, 10.f)) : settings.sunDiskRad) * (settings.proceduralSun ? 1.f : 0.f),
		.secundaColor = secundaGlow,
		.enabled = allGood,
		.tonemapper = linearLighting.settings.enableLinearLighting ? 0 : settings.tonemapper,
		.vanillaMix = vanillaMix,
		.zBottom = worldspaceInfo.zBottom,
		.rPlanet = settings.planetRadius / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = settings.atmosphereRadius / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = settings.groundAlbedo,
		.cloudShadowRemapRange = settings.cloudShadowRemapRange,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		.aerosolPhaseG = settings.aerosolPhaseG,
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
		skySync.lightColors = { LightConvFn(cbData.sunlightColor), LightConvFn(settings.masserColor * exposure), LightConvFn(settings.secundaColor * exposure) };
	} else {
		linearLighting.isDirLightLinear = false;
	}

	RE::NiPoint3 posCam = { 0, 0, 0 };
	if (auto cam = RE::PlayerCamera::GetSingleton(); cam && cam->cameraRoot) {
		posCam = cam->cameraRoot->world.translate;
		cbData.zCameraPlanet = posCam.z - cbData.zBottom + cbData.rPlanet;
	}
}

void PhysicalSky::EarlyPrepass()
{
	if (cbData.enabled) {
		UpdateApLutSize();  // (46z, E1)
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
		context->Dispatch((apLutW + 7) >> 3, (apLutH + 7) >> 3, 1);

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
			deferred->shadowView,
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
			 globals::features::physicalSky.Effective().hideNewMoonDisc && IsNewMoonDraw(a_pass))
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon);

	// (40d) The moon disc itself (not its star mask, SO_MOON_SHADOW), for "Moon Physical Brightness"
	// and (42b) "Moons Fade in Daylight". Sky.hlsl checks each option's own flag.
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON &&
		(globals::features::physicalSky.Effective().moonPhysicalRadiance || globals::features::physicalSky.Effective().moonsFadeInDaylight)) {
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
	func(This, Pass, RenderFlags);
}

void PhysicalSky::Hooks::BSSkyShader_RestoreGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::physicalSky.RestoreSamplers();
	ClearSunDrawFlags();
	func(This, Pass, RenderFlags);
}
