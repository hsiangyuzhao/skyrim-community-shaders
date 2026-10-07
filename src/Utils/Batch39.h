#pragma once

#include <string>
#include <vector>

/**
 * @brief Batch 39 master switch (Advanced -> "Batch 39").
 *
 * Off = every governed batch 39 item takes its batch 38b path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no
 * restart, no cache clear. Independent of the 36f, 36g, 37b and 38 switches.
 *
 * The table on the tab is built from one row provider per group, listed in Batch39.cpp
 * (kRowProviders); each group keeps its provider (and, where it has one, an extra section
 * drawn under the table) in its own file:
 * - Items 1-4 (water reflection cubemap, depth prepass, temporal LOD dither, texture
 *   clarity): RowsEngine / DrawEngineSection (Batch39Engine.cpp). Their settings live in
 *   Batch39Engine (saved under Advanced."Batch 39 Engine"), because they belong to the
 *   engine rather than to one feature.
 * - Items 5-6 (dynamic snow accumulation, snow/mud footprints): RowsSnowAccumulation /
 *   RowsSnowTrails (Batch39Snow.cpp). Their switches live in the Dynamic Snow feature (saved
 *   with that feature).
 */
namespace Batch39
{
	struct Settings
	{
		bool master = true;
	};

	inline Settings settings{};

	/// @brief Whether batch 39 changes may run this frame.
	inline bool IsOn() { return settings.master; }

	/// @brief Settings live under Advanced."Batch 39". This object holds only the master switch.
	inline void Load(const json& a_json)
	{
		if (!a_json.is_object())
			return;
		if (a_json.contains("Master") && a_json["Master"].is_boolean())
			settings.master = a_json["Master"].get<bool>();
	}

	inline json Save()
	{
		json o;
		o["Master"] = settings.master;
		return o;
	}

	/// @brief One line of the status table on the Batch 39 tab (same shape as Batch38::Row).
	struct Row
	{
		std::string group;       ///< "1. Water reflection cubemap", ...
		std::string name;        ///< what the item is
		bool installed;          ///< the owning feature / hook is present
		bool own;                ///< the item's own switch (true for items with no switch of their own)
		std::string ownText;     ///< the item's own setting, as text
		std::string nowText;     ///< what runs this frame; empty = derive On/Off from own && master
		std::string where;       ///< menu path
		bool governed = true;    ///< false = a fix or diagnostic the master switch does not turn off
		bool* toggle = nullptr;  ///< non-null: "Own setting" is drawn as a checkbox on this bool
	};

	/// @brief Appends the rows of one group. Register new groups in Batch39.cpp's kRowProviders.
	using RowProvider = void (*)(std::vector<Row>&);
	/// @brief Draws a group's extra controls / diagnostics under the table. Register in kSections.
	using SectionDrawer = void (*)();

	/// @brief Contents of the Advanced -> "Batch 39" tab: master switch, item table, sections.
	void DrawTab();

	/// @brief "(off: Batch 39 master switch)" after an item's own checkbox, when the master is off.
	void MasterNote();

	// ---- Items 1-4 (Batch39Engine.cpp) -----------------------------------------------------
	void RowsEngine(std::vector<Row>& a_rows);
	void DrawEngineSection();

	// ---- Items 5-6 row providers (Batch39Snow.cpp) ----------------------------------------
	/// @brief 5. Dynamic snow accumulation.
	void RowsSnowAccumulation(std::vector<Row>& a_rows);
	/// @brief 6. Snow / mud footprints and trails.
	void RowsSnowTrails(std::vector<Row>& a_rows);
}
