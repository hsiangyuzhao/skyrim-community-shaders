#pragma once

#include <string>
#include <vector>

/**
 * @brief Batch 38 master switch (Advanced -> "Batch 38").
 *
 * Off = every governed batch 38 item takes its batch 37c path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no
 * restart, no cache clear. Independent of the 36f, 36g and 37b switches.
 *
 * Each item keeps its own switch in its feature (saved with that feature); this object holds
 * only the master switch. The table on the tab is built from one row provider per group,
 * listed in Batch38.cpp (kRowProviders). Each group keeps its provider in its own file:
 * - A. Volumetric fog + smoke receiving sun shadow: RowsFogAndShadows (Batch38Fog.cpp)
 * - B. Skin SSS, Local Exposure, Skylighting fixes: RowsSkinSss / RowsLocalExposure /
 *   RowsSkylighting (Batch38PartB.cpp)
 * - E. DLSS 5 Neural Rendering: RowsNeuralRendering (Batch38NeuralRendering.cpp). Governs
 *   coexisting with frame generation, running before upscaling, model resolution, jitter-aware
 *   motion before upscaling, the guide/motion-vector fixes, the larger D3D12 command ring and
 *   the release-on-off of its GPU memory. Off = 37c's pass exactly. Not governed: the
 *   diagnostics (the overlay's Neural Rendering row, the model-only timer, the state log).
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

	/// @brief Settings live under Advanced."Batch 38". Each item keeps its own setting in its
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

	/// @brief One line of the status table on the Batch 38 tab.
	struct Row
	{
		std::string group;       ///< "A1. Volumetric fog", "3. Skin SSS upgrade", "E. DLSS 5 Neural Rendering", ...
		std::string name;        ///< what the item is
		bool installed;          ///< the owning feature is loaded
		bool own;                ///< the item's own switch (true for items with no switch of their own)
		std::string ownText;     ///< the item's own setting, as text
		std::string nowText;     ///< what runs this frame; empty = derive On/Off from own && master
		std::string where;       ///< menu path
		bool governed = true;    ///< false = a fix or diagnostic the master switch does not turn off
		bool* toggle = nullptr;  ///< non-null: "Own setting" is drawn as a checkbox on this bool
	};

	/// @brief Appends the rows of one group. Register new groups in Batch38.cpp's kRowProviders.
	using RowProvider = void (*)(std::vector<Row>&);

	/// @brief Contents of the Advanced -> "Batch 38" tab: master switch and the item table.
	void DrawTab();

	/// @brief "(off: Batch 38 master switch)" after an item's own checkbox, when the master is off.
	void MasterNote();

	// ---- Part B row providers (Batch38PartB.cpp): items 3-5 -------------------------------
	void RowsSkinSss(std::vector<Row>& a_rows);
	void RowsLocalExposure(std::vector<Row>& a_rows);
	void RowsSkylighting(std::vector<Row>& a_rows);

	// ---- E row provider (Batch38NeuralRendering.cpp) --------------------------------------
	/// @brief E. DLSS 5 Neural Rendering rows.
	void RowsNeuralRendering(std::vector<Row>& rows);
}
