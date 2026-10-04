#pragma once

#include <string>
#include <vector>

/**
 * @brief Batch 37b master switch (Advanced -> "Batch 37b").
 *
 * Off = every batch 37b item takes its batch 37a path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no
 * restart, no cache clear.
 *
 * Governed (37b items only; later batches get their own switch):
 * - A. Volumetric Lighting: cloud/terrain occlusion, gamma on density only, night strength,
 *   colour linearisation (Lighting > Volumetric Lighting > Batch 37b).
 * - B. Physical Sky: worldspace list / all exteriors, procedural sun v2, upstream correctness
 *   fixes (Sky > Physical Sky).
 * - C./D.: rows are appended by their own providers (see Batch37b.cpp, kRowProviders).
 */
namespace Batch37b
{
	struct Settings
	{
		bool master = true;
	};

	inline Settings settings{};

	/// @brief Whether batch 37b changes may run this frame.
	inline bool IsOn() { return settings.master; }

	inline void Load(const json& a_json)
	{
		if (a_json.is_object() && a_json.contains("Master") && a_json["Master"].is_boolean())
			settings.master = a_json["Master"].get<bool>();
	}

	inline json Save()
	{
		json o;
		o["Master"] = settings.master;
		return o;
	}

	/// @brief One line of the status table on the Batch 37b tab.
	struct Row
	{
		std::string group;    ///< "A. Volumetric Lighting", ...
		std::string name;     ///< what the item is
		bool installed;       ///< the owning feature is loaded
		bool own;             ///< the item's own switch (true for items with no switch of their own)
		std::string ownText;  ///< the item's own setting, as text
		std::string nowText;  ///< what runs this frame; empty = derive On/Off from own && master
		std::string where;    ///< menu path
	};

	/// @brief Appends the rows of one group. Each 37b group registers one provider in
	/// Batch37b.cpp's kRowProviders; nothing else needs to change to add a group.
	using RowProvider = void (*)(std::vector<Row>&);

	/// @brief Draws the Batch 37b tab: master switch + status table.
	void DrawTab();
}
