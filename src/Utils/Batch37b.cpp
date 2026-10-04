#include "Utils/Batch37b.h"

#include <format>
#include <imgui.h>
#include <string>

#include "Features/PostProcessing.h"
#include "Features/ScreenSpaceGI.h"
#include "Globals.h"
#include "Menu.h"
#include "Utils/UI.h"

namespace Batch37b
{
	namespace
	{
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

		void Tooltip(const char* a_text)
		{
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(a_text);
		}

		void MasterNote()
		{
			if (!IsOn()) {
				const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
				ImGui::SameLine();
				ImGui::TextColored(palette.Warning, "(off: Batch 37b master switch)");
			}
		}
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
			"Off = every item marked \"master\" below runs exactly as in batch 37a, whatever its own switch says.\n"
			"Takes effect on the next frame: no restart, no cache clear.");

		ImGui::Spacing();
		ImGui::TextWrapped(
			"Each item keeps its own switch in its feature's menu (also here). \"Now\" is what runs this frame.");
		ImGui::Spacing();

		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
		const auto& pp = globals::features::postProcessing;
		const auto& ssgi = globals::features::screenSpaceGI;

		enum class Now
		{
			On,
			Off,
			OffMaster,
			Idle,
			NotInstalled
		};
		struct Row
		{
			const char* name;
			bool* own;  // null = fix, no switch
			Now now;
			std::string detail;
			const char* where;
		};

		const auto governed = [&](bool a_installed, bool a_own, bool a_running, std::string a_idle) -> std::pair<Now, std::string> {
			if (!a_installed)
				return { Now::NotInstalled, "" };
			if (!a_own)
				return { Now::Off, "" };
			if (!master)
				return { Now::OffMaster, "" };
			if (!a_running)
				return { Now::Idle, std::move(a_idle) };
			return { Now::On, "" };
		};

		auto [c1Now, c1Detail] = governed(pp.loaded, settings.postProcessDirectOutput, pp.directOutputUsed, pp.directOutputStatus);
		auto [c2Now, c2Detail] = governed(ssgi.loaded, settings.ssgiSkipIlPasses, ssgi.ilPassesSkipped,
			!ssgi.settings.Enabled ? "SSGI is off" : (ssgi.compiledWithGI ? "Indirect Lighting is on: nothing to skip" : "SSGI not running"));
		auto [c4Now, c4Detail] = governed(ssgi.loaded, settings.ssgiAoSparesDirect, ssgi.settings.Enabled, "SSGI is off");

		const Row rows[] = {
			{ "C-1 Post-processing: one write-back copy instead of two", &settings.postProcessDirectOutput, c1Now, c1Detail,
				"Post-Processing (top, under Bypass)" },
			{ "C-2 SSGI: skip IL-only passes while IL is off", &settings.ssgiSkipIlPasses, c2Now, c2Detail,
				"Lighting > Screen Space GI (under Toggles)" },
			{ "C-4 SSGI AO does not darken direct light (default off)", &settings.ssgiAoSparesDirect, c4Now, c4Detail,
				"Lighting > Screen Space GI (under Toggles)" },
			{ "C-3 REBLUR Max Stabilized Frames slider goes to 63", nullptr, Now::On, "fix",
				"Lighting > Screen Space Ray Tracing > Denoiser > REBLUR Diffuse / Specular" },
			{ "D Occlusion dry run gets past \"Waiting for the first depth readback\"", nullptr, Now::On, "fix",
				"Performance overlay > Occlusion (dry run)" },
		};

		if (ImGui::BeginTable("##Batch37bItems", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("Item");
			ImGui::TableSetupColumn("Own switch");
			ImGui::TableSetupColumn("Now");
			ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			for (const auto& row : rows) {
				ImGui::PushID(row.name);
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.name);
				ImGui::TableNextColumn();
				if (row.own)
					ImGui::Checkbox("##own", row.own);
				else
					ImGui::TextDisabled("(fix, always on)");
				ImGui::TableNextColumn();
				switch (row.now) {
				case Now::On:
					ImGui::TextColored(palette.SuccessColor, "On");
					break;
				case Now::Off:
					ImGui::TextColored(palette.Disable, "Off");
					break;
				case Now::OffMaster:
					ImGui::TextColored(palette.Warning, "Off (master)");
					break;
				case Now::Idle:
					ImGui::TextColored(palette.InfoColor, "Idle");
					break;
				case Now::NotInstalled:
					ImGui::TextDisabled("not installed");
					break;
				}
				if (!row.detail.empty() && row.now == Now::Idle)
					Tooltip(row.detail.c_str());
				ImGui::TableNextColumn();
				ImGui::TextWrapped("%s", row.where);
				if (!row.detail.empty() && row.now == Now::Idle)
					ImGui::TextDisabled("%s", row.detail.c_str());
				ImGui::PopID();
			}
			ImGui::EndTable();
		}

		ImGui::Spacing();
		ImGui::TextDisabled(
			"Idle = switched on, but nothing to do this frame (the reason is under \"Where\"). C-1 and C-2 never change the "
			"picture; C-4 does, which is why it is off by default.");
	}
}
