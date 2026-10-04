#include "Batch37b.h"

#include <format>
#include <imgui.h>

#include "Globals.h"
#include "Menu.h"
#include "Util.h"

namespace Batch37b
{
	namespace
	{
		// ---- Row providers -------------------------------------------------------------
		// One per 37b group. To add a group: write a provider and list it in kRowProviders.

		// Extension point: groups registered here, in table order. C./D. go after B.
		constexpr RowProvider kRowProviders[] = {
			nullptr,  // placeholder: providers are added by each item's commit
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
