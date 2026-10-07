#pragma once

#include <string>
#include <vector>

/**
 * @brief Batch 38 master switch (Advanced -> "Batch 38").
 *
 * Off = every governed batch 38 item takes its batch 37c path, whatever its own switch says.
 * Each item ANDs its own setting with IsOn() where it runs, so the switch takes effect on the
 * next frame: no restart, no cache clear. Independent of the 36f / 36g / 37b switches.
 *
 * Items keep their own settings in their features; this object holds only the master switch.
 * The tab's table is built from one row provider per group (see Batch38.cpp, kRowProviders):
 * - A. Volumetric fog + smoke receiving sun shadow: RowsFogAndShadows (Batch38Fog.cpp)
 * - B. (items 3-5: SSS, Local Exposure, Skylighting fixes) are added by their own provider.
 */
namespace Batch38
{
	struct Settings
	{
		bool master = true;
	};

	inline Settings settings{};

	/// @brief Whether batch 38 changes may run this frame.
	inline bool IsOn() { return settings.master; }

	/// @brief Settings live under Advanced."Batch 38".
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

	/// @brief One line of the status table on the Batch 38 tab.
	struct Row
	{
		std::string group;    ///< "A1. Volumetric fog", ...
		std::string name;     ///< what the item is
		bool installed;       ///< the owning feature is loaded
		bool own;             ///< the item's own switch (true for items with no switch of their own)
		std::string ownText;  ///< the item's own setting, as text
		std::string nowText;  ///< what runs this frame; empty = derive On/Off from own && master
		std::string where;    ///< menu path
		bool governed = true;    ///< false = not turned off by the master switch
		bool* toggle = nullptr;  ///< non-null: "Setting" is drawn as a checkbox on this bool
	};

	using RowProvider = void (*)(std::vector<Row>&);

	/// @brief A. Volumetric fog (item 1) and smoke/effects receiving sun shadow (item 2).
	void RowsFogAndShadows(std::vector<Row>& a_rows);

	/// @brief Contents of the Advanced -> "Batch 38" tab: master switch and the item table.
	void DrawTab();
}
