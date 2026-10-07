#include "Utils/Batch39.h"

#include <imgui.h>
#include <string>

#include "Globals.h"
#include "Menu.h"
#include "Util.h"
#include "Utils/UI.h"

namespace Batch39
{
	namespace
	{
		// Extension point: one provider per batch 39 group, in table order. Each provider lives
		// in its own file next to the group's code.
		constexpr RowProvider kRowProviders[] = {
			// Items 1-4: reflection cubemap, depth prepass, LOD dither, texture clarity (Batch39Engine.cpp)
			RowsEngine,
			// Items 5-6: dynamic snow accumulation, snow/mud footprints (Batch39Snow.cpp)
			RowsSnowAccumulation,
			RowsSnowTrails,
		};

		// Extension point: extra controls / diagnostics drawn under the table, in order.
		constexpr SectionDrawer kSections[] = {
			// Items 1-4 (Batch39Engine.cpp)
			DrawEngineSection,
		};
	}

	void MasterNote()
	{
		if (!IsOn()) {
			const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
			ImGui::SameLine();
			ImGui::TextColored(palette.Warning, "(off: Batch 39 master switch)");
		}
	}

	void DrawTab()
	{
		auto& master = settings.master;

		ImGui::Checkbox("Batch 39 changes (all)", &master);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Off = every item below runs exactly as in batch 38b, whatever its own switch says\n"
				"(the diagnostics keep running).\n"
				"Takes effect on the next frame: no restart, no cache clear.\n"
				"Independent of the 36f, 37b and 38 switches.");

		ImGui::Spacing();
		ImGui::TextWrapped(
			"\"Own setting\" can be toggled here. \"Now\" is what runs this frame: the item's own setting, unless the master "
			"switch above is off or something else blocks it (the reason is shown).");
		ImGui::Spacing();

		std::vector<Row> rows;
		for (auto provider : kRowProviders)
			if (provider)
				provider(rows);

		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;

		if (ImGui::BeginTable("##Batch39Items", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
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

				// Group + name: two groups may use the same item name.
				ImGui::PushID((row.group + row.name).c_str());
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
		ImGui::TextDisabled("Idle = switched on, but nothing to do this frame (for example the feature itself is off; the reason is shown where known).");

		for (auto section : kSections) {
			if (section) {
				ImGui::Spacing();
				section();
			}
		}
	}
}
