#include "Utils/Batch37b.h"

#include <format>
#include <imgui.h>
#include <string>

#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/VolumetricLighting.h"
#include "Globals.h"
#include "Menu.h"
#include "Util.h"
#include "Utils/UI.h"

namespace Batch37b
{
	namespace
	{
		void Tooltip(const char* a_text)
		{
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(a_text);
		}

		// ---- C items: labels shared by the in-feature checkboxes ------------------------

		constexpr const char* kPostProcessLabel = "Write the last effect straight into the game buffer (37b)";
		constexpr const char* kPostProcessTip =
			"Batch 37b. The last post-processing effect stores its result directly into the game's image instead of its own "
			"texture, so one full-screen copy per frame goes away. Same shader, same values: the picture is bit-identical.\n"
			"Only takes effect while the master switch in Advanced > Batch 37b is on.";

		constexpr const char* kSkipIlLabel = "Skip IL-only passes while IL is off (37b)";
		constexpr const char* kSkipIlTip =
			"Batch 37b. With Indirect Lighting off, skips the radiance copy, the radiance prefilter and the IL blur: "
			"nothing reads their output. AO and contact AO stay bit-identical.\n"
			"Only takes effect while the master switch in Advanced > Batch 37b is on.";

		constexpr const char* kAoDirectLabel = "SSGI AO does not darken direct light (37b)";
		constexpr const char* kAoDirectTip =
			"Batch 37b, off by default (= the look so far). Off: SSGI's AO dims the ambient light fully and everything else "
			"(sunlight, lamps) by its square root. On: SSGI's AO dims only the ambient light; sunlit corners stay as bright as "
			"the sun makes them. With SSRT diffuse on, its traced bounce light is no longer dimmed either (it has its own "
			"occlusion).\n"
			"Switching it rebuilds one shader: a brief hitch. Only takes effect while the master switch in Advanced > Batch 37b is on.";

		void MasterNote()
		{
			if (!IsOn()) {
				const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
				ImGui::SameLine();
				ImGui::TextColored(palette.Warning, "(off: Batch 37b master switch)");
			}
		}

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
			rows.push_back({ group, "  Never brighten (gamma on density only)", vl.loaded, s.DensityOnlyGamma && s.DensityGammaNeverBrighten,
				s.DensityGammaNeverBrighten ? "On" : "Off", "", where + std::string(" > Gamma on Density Only") });
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
			rows.push_back({ group, "Fix: transmittance LUT edge (9fbd052ad)", ps.loaded, s.fixTrLutEdge, onOff(s.fixTrLutEdge), "", fixWhere });
			rows.push_back({ group, "Fix: haze shadow depth under DLSS (224312a11)", ps.loaded, s.fixApShadowDepth, onOff(s.fixApShadowDepth), "", fixWhere });
			rows.push_back({ group, "Fix: dark patches in reflected sky (23156dc5f)", ps.loaded, s.fixReflectionSky, onOff(s.fixReflectionSky), "", fixWhere });
			rows.push_back({ group, "Fix: multiple scattering, full sky (c14664115)", ps.loaded, s.fixMultiScatter, onOff(s.fixMultiScatter), "", fixWhere });
			rows.push_back({ group, "Fix: Cloud Shadow Remap saved (1aaf5168d)", ps.loaded, true, "(no own switch)", "", "-" });
		}

		/// "Now" text for a governed C item that is switched on: "On", or "Idle: <reason>" when
		/// there is nothing to do this frame. Only consulted while the master switch is on.
		std::string IdleOr(bool a_running, const std::string& a_reason)
		{
			return a_running ? std::string("On") : std::format("Idle: {}", a_reason);
		}

		void RowsSettingsAudit(std::vector<Row>& rows)
		{
			const auto& pp = globals::features::postProcessing;
			const auto& ssgi = globals::features::screenSpaceGI;
			const char* group = "C. Settings audit";
			const auto onOff = [](bool b) { return std::string(b ? "On" : "Off"); };

			const std::string c2Idle = !ssgi.settings.Enabled ? "SSGI is off" :
			                           (ssgi.compiledWithGI ? "Indirect Lighting is on, nothing to skip" : "SSGI not running");

			{
				Row r{ group, "C-1 Post-processing: one write-back copy instead of two", pp.loaded, settings.postProcessDirectOutput,
					onOff(settings.postProcessDirectOutput),
					settings.postProcessDirectOutput ? IdleOr(pp.directOutputUsed, pp.directOutputStatus) : std::string(),
					"Post-Processing (top, under Bypass)" };
				r.toggle = &settings.postProcessDirectOutput;
				rows.push_back(std::move(r));
			}
			{
				Row r{ group, "C-2 SSGI: skip IL-only passes while IL is off", ssgi.loaded, settings.ssgiSkipIlPasses,
					onOff(settings.ssgiSkipIlPasses),
					settings.ssgiSkipIlPasses ? IdleOr(ssgi.ilPassesSkipped, c2Idle) : std::string(),
					"Lighting > Screen Space GI (under Toggles)" };
				r.toggle = &settings.ssgiSkipIlPasses;
				rows.push_back(std::move(r));
			}
			{
				Row r{ group, "C-4 SSGI AO does not darken direct light (default off)", ssgi.loaded, settings.ssgiAoSparesDirect,
					onOff(settings.ssgiAoSparesDirect),
					settings.ssgiAoSparesDirect ? IdleOr(ssgi.settings.Enabled, "SSGI is off") : std::string(),
					"Lighting > Screen Space GI (under Toggles)" };
				r.toggle = &settings.ssgiAoSparesDirect;
				rows.push_back(std::move(r));
			}
			{
				Row r{ group, "C-3 REBLUR Max Stabilized Frames slider goes to 63", true, true, "(fix, always on)", "On",
					"Lighting > Screen Space Ray Tracing > Denoiser > REBLUR Diffuse / Specular" };
				r.governed = false;
				rows.push_back(std::move(r));
			}
		}

		void RowsDiagnostics(std::vector<Row>& rows)
		{
			Row r{ "D. Diagnostics", "Occlusion dry run gets past \"Waiting for the first depth readback\"", true, true,
				"(fix, always on)", "On", "Performance overlay > Occlusion (dry run)" };
			r.governed = false;
			rows.push_back(std::move(r));
		}

		// Extension point: groups registered here, in table order.
		constexpr RowProvider kRowProviders[] = {
			RowsVolumetricLighting,
			RowsPhysicalSky,
			RowsSettingsAudit,
			RowsDiagnostics,
		};
	}

	void DrawPostProcessCheckbox()
	{
		ImGui::Checkbox(kPostProcessLabel, &settings.postProcessDirectOutput);
		Tooltip(kPostProcessTip);
		MasterNote();
	}

	void DrawSsgiCheckboxes()
	{
		ImGui::Checkbox(kSkipIlLabel, &settings.ssgiSkipIlPasses);
		Tooltip(kSkipIlTip);
		MasterNote();
		ImGui::Checkbox(kAoDirectLabel, &settings.ssgiAoSparesDirect);
		Tooltip(kAoDirectTip);
		MasterNote();
	}

	void DrawTab()
	{
		auto& master = settings.master;

		ImGui::Checkbox("Batch 37b changes (all)", &master);
		Tooltip(
			"Off = every item below runs exactly as in batch 37a, whatever its own switch says\n"
			"(except the fixes marked \"always on\").\n"
			"Takes effect on the next frame: no restart, no cache clear.");

		ImGui::Spacing();
		ImGui::TextWrapped(
			"Each item keeps its own switch in its feature's menu (C items can also be toggled here). \"Now\" is what runs "
			"this frame: the item's own setting, unless the master switch above is off.");
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

				ImGui::PushID(row.name.c_str());
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.name.c_str());
				ImGui::TableNextColumn();
				if (!row.installed)
					ImGui::TextDisabled("not installed");
				else if (row.toggle)
					ImGui::Checkbox("##own", row.toggle);
				else if (!row.governed)
					ImGui::TextDisabled("%s", row.ownText.c_str());
				else
					ImGui::TextUnformatted(row.ownText.c_str());
				ImGui::TableNextColumn();
				if (!row.installed) {
					ImGui::TextDisabled("-");
				} else if (!master && row.governed) {
					ImGui::TextColored(row.own ? palette.Warning : palette.Disable, "%s", row.own ? "Off (master)" : "Off");
				} else if (!row.nowText.empty()) {
					ImGui::TextColored(row.own ? palette.SuccessColor : palette.Disable, "%s", row.nowText.c_str());
				} else {
					ImGui::TextColored(row.own ? palette.SuccessColor : palette.Disable, "%s", row.own ? "On" : "Off");
				}
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", row.where.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextDisabled(
			"Idle = switched on, but nothing to do this frame. C-1 and C-2 never change the picture; C-4 does, which is why it "
			"is off by default.");
	}
}
