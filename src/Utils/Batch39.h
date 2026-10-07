#pragma once

#include <string>
#include <vector>

/**
 * @brief Batch 39 master switch (Advanced -> "Batch 39").
 *
 * Off = every governed batch 39 item takes its batch 38b path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no
 * restart, no cache clear. Independent of the 36f, 37b and 38 switches.
 *
 * Each item keeps its own switch in its feature (saved with that feature); this object holds
 * only the master switch. The table on the tab is built from one row provider per group,
 * listed in Batch39.cpp (kRowProviders). Each group keeps its provider in its own file:
 * - Items 5-6. Dynamic snow accumulation and snow/mud footprints: RowsSnowAccumulation /
 *   RowsSnowTrails (Batch39Snow.cpp)
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

	/// @brief Settings live under Advanced."Batch 39". Each item keeps its own setting in its
	/// feature; this object holds only the master switch.
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

	/// @brief One line of the status table on the Batch 39 tab.
	struct Row
	{
		std::string group;       ///< "5. Dynamic snow", "6. Snow footprints", ...
		std::string name;        ///< what the item is
		bool installed;          ///< the owning feature is loaded
		bool own;                ///< the item's own switch (true for items with no switch of their own)
		std::string ownText;     ///< the item's own setting, as text
		std::string nowText;     ///< what runs this frame; empty = derive On/Off from own && master
		std::string where;       ///< menu path
		bool governed = true;    ///< false = a fix or diagnostic the master switch does not turn off
		bool* toggle = nullptr;  ///< non-null: "Own setting" is drawn as a checkbox on this bool
	};

	/// @brief Appends the rows of one group. Register new groups in Batch39.cpp's kRowProviders.
	using RowProvider = void (*)(std::vector<Row>&);

	/// @brief Contents of the Advanced -> "Batch 39" tab: master switch and the item table.
	void DrawTab();

	/// @brief "(off: Batch 39 master switch)" after an item's own checkbox, when the master is off.
	void MasterNote();

	// ---- Items 5-6 row providers (Batch39Snow.cpp) ----------------------------------------
	/// @brief 5. Dynamic snow accumulation.
	void RowsSnowAccumulation(std::vector<Row>& a_rows);
	/// @brief 6. Snow / mud footprints and trails.
	void RowsSnowTrails(std::vector<Row>& a_rows);
}
