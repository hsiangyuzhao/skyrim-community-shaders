#include "Batch37b.h"

#include <format>
#include <imgui.h>

#include "Features/PhysicalSky.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "Menu.h"
#include "Util.h"

namespace Batch37b
{
	namespace
	{
		// ---- Row providers -------------------------------------------------------------
		// One per 37b group. To add a group: write a provider and list it in kRowProviders.

		void RowsVolumetricLighting(std::vector<Row>& rows)
		{
			const auto& vl = globals::features::volumetricLighting;
			const auto& s = vl.settings;
			const char* group = "A. Volumetric Lighting";
			const char* where = "Lighting > Volumetric Lighting > Batch 37b";

			rows.push_back({ group, "Cloud & terrain occlusion", vl.loaded, s.WorldShadowPower > 0.0f,
				s.WorldShadowPower > 0.0f ? std::format("{:.2f}", s.WorldShadowPower) : std::string("0 (off)"),
				std::format("{:.2f}", vl.WorldShadowPowerActive()), where });
			rows.push_back({ group, "Gamma on density only", vl.loaded, s.DensityOnlyGamma,
				s.DensityOnlyGamma ? std::format("On, ref {:.2f}", s.DensityGammaReference) : std::string("Off"),
				"", where });
			rows.push_back({ group, "Night strength", vl.loaded, s.NightIntensity != 1.0f,
				std::format("{:.2f}", s.NightIntensity), std::format("{:.2f}", vl.NightIntensityActive()), where });
			rows.push_back({ group, "Linearize shaft colour (no Physical Sky override)", vl.loaded, s.LinearizeColor,
				s.LinearizeColor ? "On" : "Off", "", where });
		}

		void RowsPhysicalSky(std::vector<Row>& rows)
		{
			auto& ps = globals::features::physicalSky;
			const auto& s = ps.settings;
			const char* group = "B. Physical Sky";

			float zBottom = 0.f;
			const auto status = ps.loaded ? ps.GetWorldspaceStatus(zBottom) : PhysicalSky::WorldspaceStatus::Unknown;
			const char* statusText = "-";
			switch (status) {
			case PhysicalSky::WorldspaceStatus::Whitelist:
				statusText = "here: on (list)";
				break;
			case PhysicalSky::WorldspaceStatus::AllExteriors:
				statusText = "here: on (all exteriors)";
				break;
			case PhysicalSky::WorldspaceStatus::Excluded:
				statusText = "here: off (excluded)";
				break;
			case PhysicalSky::WorldspaceStatus::NotListed:
				statusText = "here: off (not listed)";
				break;
			case PhysicalSky::WorldspaceStatus::Interior:
				statusText = "here: off (interior)";
				break;
			default:
				break;
			}

			rows.push_back({ group, "Worldspace list (+DLC, saved, editable)", ps.loaded, true,
				std::format("{} entries", s.worldspaceWhitelist.size()), statusText,
				"Sky > Physical Sky > General > Worldspaces" });
			rows.push_back({ group, "All exterior worldspaces", ps.loaded, s.enableAllExteriorWorldspaces,
				s.enableAllExteriorWorldspaces ? "On" : "Off", "", "Sky > Physical Sky > General > Worldspaces" });

			const char* sunWhere = "Sky > Physical Sky > Celestials > Sun (needs Procedural Sun on)";
			const auto onOff = [](bool b) { return std::string(b ? "On" : "Off"); };
			rows.push_back({ group, "Sun: align with vanilla sun", ps.loaded, s.sunAlignToVanilla, onOff(s.sunAlignToVanilla), "",
				"Sky > Physical Sky > Celestials > Sun" });
			rows.push_back({ group, "Sun: replace vanilla sun", ps.loaded, s.sunReplaceVanilla, onOff(s.sunReplaceVanilla), "", sunWhere });
			rows.push_back({ group, "Sun: soft edge", ps.loaded, s.sunSoftEdge, onOff(s.sunSoftEdge), "", sunWhere });
			rows.push_back({ group, "Sun: physical brightness (cap)", ps.loaded, s.sunPhysicalRadiance,
				s.sunPhysicalRadiance ? std::format("On, cap {:.0f}", s.sunRadianceCap) : std::string("Off"), "", sunWhere });
			rows.push_back({ group, "Sun: hide vanilla glare", ps.loaded, s.sunHideVanillaGlare, onOff(s.sunHideVanillaGlare), "", sunWhere });
			rows.push_back({ group, "Sun: disk radius", ps.loaded, true, std::format("{:.2f} deg", s.sunDiskRadiusDeg),
				Batch37b::IsOn() ? std::format("{:.2f} deg", s.sunDiskRadiusDeg) : std::string(),
				"Sky > Physical Sky > Celestials > Sun" });

			const char* fixWhere = "Sky > Physical Sky > Atmosphere > Fixes (Batch 37b)";
			rows.push_back({ group, "Fix: opaque sky (5846ad833)", ps.loaded, s.fixSkyAlpha, onOff(s.fixSkyAlpha), "", fixWhere });
			rows.push_back({ group, "Fix: Cloud Shadow Remap saved (1aaf5168d)", ps.loaded, true, "(no own switch)", "", "-" });
		}

		// Extension point: groups registered here, in table order. C./D. go after B.
		constexpr RowProvider kRowProviders[] = {
			RowsVolumetricLighting,
			RowsPhysicalSky,
		};
	}

	void DrawTab()
	{
		auto& master = settings.master;

		ImGui::Checkbox("Batch 37b changes (all)", &master);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Off = every item below runs exactly as in batch 37a, whatever its own switch says.\n"
				"Takes effect on the next frame: no restart, no cache clear.");
		}

		ImGui::Spacing();
		ImGui::TextWrapped(
			"Each item keeps its own switch in its feature's menu. \"Now\" is what runs this frame: "
			"the item's own setting, unless the master switch above is off.");
		ImGui::Spacing();

		std::vector<Row> rows;
		for (auto provider : kRowProviders)
			if (provider)
				provider(rows);

		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;

		if (ImGui::BeginTable("##Batch37bItems", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("Item");
			ImGui::TableSetupColumn("Own setting");
			ImGui::TableSetupColumn("Now");
			ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			std::string lastGroup;
			for (const auto& row : rows) {
				if (row.group != lastGroup) {
					lastGroup = row.group;
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
					ImGui::TextColored(palette.InfoColor, "%s", row.group.c_str());
				}

				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.name.c_str());
				ImGui::TableNextColumn();
				if (!row.installed)
					ImGui::TextDisabled("not installed");
				else
					ImGui::TextUnformatted(row.ownText.c_str());
				ImGui::TableNextColumn();
				if (!row.installed) {
					ImGui::TextDisabled("-");
				} else if (!master) {
					ImGui::TextColored(row.own ? palette.Warning : palette.Disable, "%s", row.own ? "Off (master)" : "Off");
				} else if (!row.nowText.empty()) {
					ImGui::TextColored(row.own ? palette.SuccessColor : palette.Disable, "%s", row.nowText.c_str());
				} else {
					ImGui::TextColored(row.own ? palette.SuccessColor : palette.Disable, "%s", row.own ? "On" : "Off");
				}
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", row.where.c_str());
			}
			ImGui::EndTable();
		}
	}
}
