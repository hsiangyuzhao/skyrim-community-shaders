#include "Utils/PerfOptions.h"

#include <imgui.h>

#include "Utils/UI.h"

namespace PerfOptions
{
	namespace
	{
		struct Item
		{
			bool Settings::*field;
			const char* key;    ///< JSON key
			const char* label;  ///< menu text
			bool aggressive;    ///< A (only in the Aggressive preset) or C
			const char* tooltip;
		};

		// Menu order = the package's item order.
		constexpr Item kItems[] = {
			{ &Settings::DryCharacterSkip, "DryCharacterSkip", "Dry characters skip the wetness math", false,
				"Character Wetness (Physical): a character with no sweat, rain or swim water on it skips the wet-skin, wet-hair and\n"
				"wet-gear shading. Every value it would add is 0 then. No visible change." },
			{ &Settings::WetCubemapSkip, "WetCubemapSkip", "Skip the wet reflection where nothing is wet", false,
				"The extra reflection read for wet surfaces is not made where the surface's wetness is 0 (always the case for hair).\n"
				"No visible change." },
			{ &Settings::HairLightSkip, "HairLightSkip", "Hair: skip lighting work for lights that add nothing", false,
				"Hair self-shadow and the hair highlight are not computed for a light whose colour is black or that is fully\n"
				"shadowed; the hair's tangent texture is read once instead of per light. No visible change." },
			{ &Settings::SssTransmitSkip, "SssTransmitSkip", "Skin: skip light-through-skin where it is off", false,
				"Advanced Skin's transmittance (light shining through ears and fingers) is not computed when its strength is 0.\n"
				"No visible change." },
			{ &Settings::SsplsDistanceGate, "SsplsDistanceGate", "Lamp contact shadows: skip the pre-pass beyond their distance", false,
				"Screen Space Point Light Shadows: the per-pixel light ranking is skipped beyond SSPLS's Max Distance, where no\n"
				"contact shadow is drawn anyway. No visible change." },
			{ &Settings::RtDropTinyParts, "RtDropTinyParts", "Ray tracing: leave out tiny parts that cast nothing", false,
				"Ray Tracing: eyes, teeth, mouth, eyebrows and similar tiny parts inside the head are not skinned or built for the\n"
				"ray tracer when they could not cast a visible shadow. Only while Ray Tracing is on." },
			{ &Settings::RtBonesTolerant, "RtBonesTolerant", "Ray tracing: ignore bone noise when checking for movement", false,
				"Ray Tracing: a character whose bones moved by less than a thousandth of a unit counts as unchanged, so its last\n"
				"skinning is reused. The update rate is unchanged. Only while Ray Tracing is on." },
			{ &Settings::RtaoNormalFold, "RtaoNormalFold", "RTAO: skip the normal prep where it is not used", false,
				"RTAO Smooth Normals: the normal-prep pass is skipped for pixels the AO does not trace (sky, beyond the RTAO\n"
				"distance). Same result. Only while RTAO is on." },
			{ &Settings::WaterPrepGate, "WaterPrepGate", "Ray tracing: water prep only when a lamp can reach water", false,
				"Point Light Shadows on Water: the water prep pass is skipped on frames where no traced point light reaches any\n"
				"water drawn. Same result. Only while RT point lights are on." },
			{ &Settings::HairSsplsSkip, "HairSsplsSkip", "Hair: no lamp contact shadows in the hair's blended pass", true,
				"Screen Space Point Light Shadows are not marched again for hair in the blended (transparent) pass. Hair keeps\n"
				"its lamp shadows from the shadow maps / ray tracing; only the tiny contact shadows inside the hair go.\n"
				"Visual risk: very small." },
			{ &Settings::SmallCasterCull, "SmallCasterCull", "Sun shadows: small far objects cast none", true,
				"In the far sun shadow cascade, objects whose size is small for their distance (radius under 3% of the distance)\n"
				"are not drawn into the shadow map. Characters always cast. Visual risk: shadows of small things far away\n"
				"(barrels, crates, small rocks) disappear." },
			{ &Settings::ReflSkipLodTerrain, "ReflSkipLodTerrain", "Water reflection: leave out LOD objects and terrain", true,
				"The game's small reflection cubemap for distant water draws only the sky (distant trees are already left out):\n"
				"distant mountains and buildings drop out of far water reflections. Near water keeps its screen-space\n"
				"reflections. Visual risk: visible on large lakes and the sea at a distance." },
			{ &Settings::VrsEffects, "VrsEffects", "Variable Rate Shading also for soft effects", true,
				"With Variable Rate Shading on, soft effects (smoke, mist, steam, glows) are shaded once per 2x2 pixels.\n"
				"Hair and cut-out effects are never coarse-shaded. Visual risk: soft effects get slightly softer." },
		};

		bool PresetValue(const Item& a_item, Preset a_preset)
		{
			return a_preset == Preset::Aggressive ? true : !a_item.aggressive;
		}
	}

	void ApplyPreset(Preset a_preset)
	{
		if (a_preset == Preset::Custom)
			return;
		settings.preset = a_preset;
		for (const auto& item : kItems)
			settings.*item.field = PresetValue(item, a_preset);
	}

	void Load(const json& a_json)
	{
		ApplyPreset(Preset::Conservative);  // no saved block: the 47p default
		if (!a_json.is_object())
			return;
		int preset = 0;
		if (a_json.contains("Preset") && a_json["Preset"].is_number_integer())
			preset = std::clamp(a_json["Preset"].get<int>(), 0, 2);
		if (preset != static_cast<int>(Preset::Custom)) {
			ApplyPreset(static_cast<Preset>(preset));
			return;
		}
		settings.preset = Preset::Custom;
		for (const auto& item : kItems)
			if (a_json.contains(item.key) && a_json[item.key].is_boolean())
				settings.*item.field = a_json[item.key].get<bool>();
	}

	json Save()
	{
		json j;
		j["Preset"] = static_cast<int>(settings.preset);
		for (const auto& item : kItems)
			j[item.key] = settings.*item.field;
		return j;
	}

	std::string Describe()
	{
		std::string s = settings.preset == Preset::Aggressive ? "Aggressive" : (settings.preset == Preset::Conservative ? "Conservative" : "Custom");
		s += " (";
		bool first = true;
		for (const auto& item : kItems) {
			if (!first)
				s += " ";
			first = false;
			s += std::format("{}{}", item.key, settings.*item.field ? "+" : "-");
		}
		s += ")";
		return s;
	}

	void DrawMenu()
	{
		ImGui::TextWrapped(
			"Performance package (47p). Conservative turns on only changes that look exactly the same. "
			"Aggressive adds a few that trade a little picture for speed. Every switch works on its own and takes effect at once.");
		ImGui::Spacing();
		ImGui::TextUnformatted("Performance:");
		ImGui::SameLine();
		const bool isConservative = settings.preset == Preset::Conservative;
		const bool isAggressive = settings.preset == Preset::Aggressive;
		if (isConservative)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button("Conservative"))
			ApplyPreset(Preset::Conservative);
		if (isConservative)
			ImGui::PopStyleColor();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Only the switches marked C: no visible change. The default.");
		ImGui::SameLine();
		if (isAggressive)
			ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
		if (ImGui::Button("Aggressive"))
			ApplyPreset(Preset::Aggressive);
		if (isAggressive)
			ImGui::PopStyleColor();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Every switch, C and A: the A ones change the picture a little.");
		ImGui::SameLine();
		ImGui::TextDisabled(settings.preset == Preset::Custom ? "(custom)" : "");

		if (ImGui::TreeNodeEx("Advanced: individual switches")) {
			bool headerA = false;
			for (const auto& item : kItems) {
				if (item.aggressive && !headerA) {
					ImGui::SeparatorText("A: small visible trade (Aggressive only)");
					headerA = true;
				} else if (!item.aggressive && &item == &kItems[0]) {
					ImGui::SeparatorText("C: no visible change (Conservative and Aggressive)");
				}
				bool value = settings.*item.field;
				if (ImGui::Checkbox(std::format("{} [{}]", item.label, item.aggressive ? "A" : "C").c_str(), &value)) {
					settings.*item.field = value;
					settings.preset = Preset::Custom;
					for (const Preset p : { Preset::Conservative, Preset::Aggressive }) {
						bool all = true;
						for (const auto& other : kItems)
							all = all && settings.*other.field == PresetValue(other, p);
						if (all)
							settings.preset = p;
					}
				}
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted(item.tooltip);
			}
			ImGui::TreePop();
		}
	}
}
