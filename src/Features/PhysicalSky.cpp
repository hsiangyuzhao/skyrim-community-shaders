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
	hideNewMoonDisc,
	fixMultiScatter,
	fixEffectBlend,
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
}

void PhysicalSky::LoadSettings(json& o_json)
{
	settings = o_json;

	// (batch 37b) A saved whitelist replaces the default one wholesale. Merge the defaults back
	// in, except those the user removed, so defaults added later still reach older configs.
	for (const auto& [name, info] : DefaultWorldspaceWhitelist()) {
		if (!settings.worldspaceWhitelist.contains(name) &&
			std::ranges::find(settings.worldspaceRemovedDefaults, name) == settings.worldspaceRemovedDefaults.end())
			settings.worldspaceWhitelist.emplace(name, info);
	}
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

	ImGui::SeparatorText("Sun");
	{
		ImGui::PushID("Sun");
		ImGui::ColorEdit3("Light Color", &settings.sunlightColor.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(lightColorHint);
		{
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
		}
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
		ImGui::PopID();
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

	ImGui::SeparatorText("New Moon");
	ImGui::Checkbox("Hide New Moon Disc", &settings.hideNewMoonDisc);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On a new moon the game draws Masser and Secunda as solid black discs over the stars\n"
			"(its new-moon pictures are plain black). Both moons share one phase calendar, so both\n"
			"turn black on the same nights. On: a new moon is not drawn at all, like the real sky.\n"
			"Off: the game's black discs.");
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

		ImGui::SliderFloat("Anisotropy", &settings.aerosolPhaseG, -1, 1);
		ImGui::ColorEdit3("Scatter", &settings.aerosolScatter.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		ImGui::ColorEdit3("Absorption", &settings.aerosolAbsorption.x, ImGuiColorEditFlags_DisplayHSV | ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Usually 1/9 of scatter coefficient. Dust/pollution is lower, fog is higher.");
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

	SettingsFixes();
}

void PhysicalSky::SettingsFixes()
{
	ImGui::SeparatorText("Multiple Scattering");
	ImGui::Checkbox("Full-Sky Multiple Scattering (changes sky colour)", &settings.fixMultiScatter);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"The sky's multiple-scattering table only looked at half the sky. On: the full sky, as upstream.\n"
			"Changes overall sky brightness and colour (usually the side away from the sun gets a little\n"
			"brighter, the sun side a little darker). Off by default: compare and decide.");

	ImGui::SeparatorText("Fixes");
	ImGui::Checkbox("Upstream fix: distance haze on effects", &settings.fixEffectBlend);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Distant haze treated every effect the same: glowing effects (fire, magic) got extra haze light\n"
			"on every layer and darkening decals (blood) were brightened. On: glow effects only fade, decals are\n"
			"left alone. Off = old behaviour.");
}

void PhysicalSky::SettingsClouds()
{
	InfoBox("Clouds.");

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

		D3D11_TEXTURE3D_DESC tex3dDesc{
			.Width = kApLutW,
			.Height = kApLutH,
			.Depth = kApLutD,
			.MipLevels = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D = { .MostDetailedMip = 0, .MipLevels = 1 };
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D,
		uavDesc.Texture3D = { .MipSlice = 0, .FirstWSlice = 0, .WSize = kApLutD };

		texApLut = eastl::make_unique<Texture3D>(tex3dDesc);
		texApLut->CreateSRV(srvDesc);
		texApLut->CreateUAV(uavDesc);
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

	uint flags = 0;
	if (settings.sunReplaceVanilla)
		flags |= kExtSunReplace;
	if (settings.sunSoftEdge)
		flags |= kExtSunSoftEdge;
	if (settings.sunPhysicalRadiance)
		flags |= kExtSunPhysicalRadiance;
	if (settings.sunHideVanillaGlare)
		flags |= kExtHideSunGlare;
	// Upstream correctness fixes, always on.
	flags |= kExtSkyAlphaOpaque | kExtTrLutEdgeFix | kExtApShadowDepthFix | kExtReflectionSkyFix;
	if (settings.fixMultiScatter)
		flags |= kExtMultiScatterFix;
	if (settings.fixEffectBlend)
		flags |= kExtEffectBlendFix;
	extCbData.flags = flags;
	extCbData.sunRadianceCap = std::clamp(settings.sunRadianceCap, 1.f, 62250.f);
	extCbData.sunGlowIntensity = settings.proceduralSun ? std::clamp(settings.sunGlowIntensity, 0.f, 30.f) : 0.f;
	extCbData.sunGlowWidth = DirectX::XMConvertToRadians(std::clamp(settings.sunGlowWidthDeg, 0.1f, 5.f));
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
	UpdateExtCbData();

	auto& skySync = globals::features::skySync;
	skySync.lightColors = std::nullopt;

	auto& linearLighting = globals::features::linearLighting;

	bool allGood = settings.enabled && ShadersOK() && skySync.loaded && skySync.settings.Enabled;

	// check worldspace
	bool inMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));

	// Whitelist (saved, editable, DLC exteriors added) / all exteriors / exclusions.
	WorldspaceInfo worldspaceInfo = {};
	const auto worldspaceStatus = GetWorldspaceStatus(worldspaceInfo.zBottom);
	const bool worldspaceEnabled = worldspaceStatus == WorldspaceStatus::Whitelist || worldspaceStatus == WorldspaceStatus::AllExteriors;
	allGood &= worldspaceEnabled && !inMainLoadingMenu;

	if (!allGood) {
		if (skySync.loaded && skySync.settings.Enabled)
			skySync.lightColors = std::nullopt;
		cbData.enabled = allGood;
		linearLighting.isDirLightLinear = false;
		return;
	}

	// resolution
	float2 res = globals::state->screenSize;
	float2 dynres = Util::ConvertToDynamic(res);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto sunDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Sun)];
	// (batch 37b) Align with the vanilla sun quad. Sky Sync places it (and points the sun light)
	// along the apparent direction -- dipped by atan(altitude / 325000), 1.5-4 degrees -- in the
	// sky root's local frame, which Sky Sync rotates by the cell's north rotation. The raw
	// direction used before sat that far off the quad: a second, clipped or missing disk.
	if (settings.sunAlignToVanilla) {
		RE::NiPoint3 apparent = skySync.directions[static_cast<int>(SkySync::Caster::Sun)];
		if (auto* sky = globals::game::sky; sky && sky->root)
			apparent = sky->root->world.rotate * apparent;
		if (apparent.Unitize() > FLT_EPSILON)
			sunDir = apparent;
	}
	auto masserDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Masser)];
	auto secundaDir = skySync.rawDirections[static_cast<int>(SkySync::Caster::Secunda)];

	float sunAngle = DirectX::XMConvertToRadians(90.f) - acos(sunDir.z);
	float adaptAmount = (sunAngle - settings.adaptationStart) / (settings.adaptationEnd - settings.adaptationStart);
	adaptAmount = std::min(1.f, std::max(0.f, adaptAmount));
	float exposure = settings.dayExposure * exp(log(settings.nightExposure / settings.dayExposure) * adaptAmount);

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
		.masserColor = settings.masserColor * exposure,
		.apTrMix = settings.apTrMix,
		.secundaDir = { secundaDir.x, secundaDir.y, secundaDir.z },
		.sunDiskCos = cos(DirectX::XMConvertToRadians(std::clamp(settings.sunDiskRadiusDeg, 0.05f, 10.f))) * (settings.proceduralSun ? 1.f : 0.f),
		.secundaColor = settings.secundaColor * exposure,
		.enabled = allGood,
		.tonemapper = linearLighting.settings.enableLinearLighting ? 0 : settings.tonemapper,
		.vanillaMix = settings.vanillaMix,
		.zBottom = worldspaceInfo.zBottom,
		.rPlanet = settings.planetRadius / Util::Units::GAME_UNIT_TO_KM,
		.rAtmosphere = settings.atmosphereRadius / Util::Units::GAME_UNIT_TO_KM,
		.groundAlbedo = settings.groundAlbedo,
		.cloudShadowRemapRange = settings.cloudShadowRemapRange,
		.aerosolFalloff = settings.aerosolFalloff * Util::Units::GAME_UNIT_TO_KM,
		.aerosolPhaseG = settings.aerosolPhaseG,
		.aerosolScatter = settings.aerosolScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.aerosolAbsorption = settings.aerosolAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.rayleighFalloff = settings.rayleighFalloff * Util::Units::GAME_UNIT_TO_KM,
		.rayleighScatter = settings.rayleighScatter * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
		.ozoneAltitude = settings.ozoneAltitude / Util::Units::GAME_UNIT_TO_KM,
		.ozoneThickness = settings.ozoneThickness / Util::Units::GAME_UNIT_TO_KM,
		.ozoneAbsorption = settings.ozoneAbsorption * 1e-3 * Util::Units::GAME_UNIT_TO_KM,
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
		skySync.lightColors = { LightConvFn(cbData.sunlightColor), LightConvFn(cbData.masserColor), LightConvFn(cbData.secundaColor) };
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
		context->Dispatch((kApLutW + 7) >> 3, (kApLutH + 7) >> 3, 1);

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
	                static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon));

	if (!a_pass || !a_pass->shaderProperty)
		return;
	const auto* skyProperty = static_cast<const RE::BSSkyShaderProperty*>(a_pass->shaderProperty);
	if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN)
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSun);
	else if (skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_SUN_GLARE)
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsSunGlare);
	else if ((skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON ||
				 skyProperty->uiSkyObjectType == RE::BSSkyShaderProperty::SkyObject::SO_MOON_SHADOW) &&
			 globals::features::physicalSky.settings.hideNewMoonDisc && IsNewMoonDraw(a_pass))
		descriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon);
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
	                static_cast<uint32_t>(State::ExtraShaderDescriptors::IsNewMoon));
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
