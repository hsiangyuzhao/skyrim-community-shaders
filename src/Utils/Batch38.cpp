#include "Utils/Batch38.h"

#include <imgui.h>
#include <string>

#include "Globals.h"
#include "Menu.h"
#include "Util.h"
#include "Utils/UI.h"

namespace Batch38
{
	namespace
	{
		// Extension point: groups registered here, in table order.
		constexpr RowProvider kRowProviders[] = {
			// Part B: items 3-5
			RowsSkinSss,
			RowsLocalExposure,
		};
	}

	void MasterNote()
	{
		if (!IsOn()) {
			const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
			ImGui::SameLine();
			ImGui::TextColored(palette.Warning, "(off: Batch 38 master switch)");
		}
	}

	void DrawTab()
	{
		auto& master = settings.master;

		ImGui::Checkbox("Batch 38 changes (all)", &master);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"Off = every item below runs exactly as in batch 37c, whatever its own switch says\n"
				"(except the fixes marked \"always on\").\n"
				"Takes effect on the next frame: no restart, no cache clear.");

		ImGui::Spacing();
		ImGui::TextWrapped(
			"Each item keeps its own switch in its feature's menu (and can be toggled here too). \"Now\" is what runs "
			"this frame: the item's own setting, unless the master switch above is off.");
		ImGui::Spacing();

		std::vector<Row> rows;
		for (auto provider : kRowProviders)
			if (provider)
				provider(rows);

		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;

		if (ImGui::BeginTable("##Batch38Items", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
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
		ImGui::TextDisabled("Idle = switched on, but nothing to do this frame.");
	}
}
