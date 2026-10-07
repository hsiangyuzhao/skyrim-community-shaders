#pragma once

#include <string>
#include <vector>

/**
 * @brief Batch 38 master switch (Advanced -> "Batch 38").
 *
 * Off = every governed batch 38 item takes its batch 37c path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no restart.
 * Independent of the 36f, 36g and 37b switches.
 *
 * Governed so far:
 * - E. DLSS 5 Neural Rendering (Upscaling > Neural Rendering): coexisting with frame generation,
 *   running before upscaling, model resolution, jitter-aware motion before upscaling, the
 *   guide/motion-vector fixes, the larger D3D12 command ring and the release-on-off of its GPU
 *   memory. (38c) Our own model resolution, padding to the network grid, tone-matched input and
 *   its precision, tone preservation, tuning at creation. Off = 37c's pass exactly (blocked under
 *   frame generation, after upscaling).
 *
 * Not governed: the diagnostics (the overlay's Neural Rendering row, the model-only timer, the
 * one-line state log).
 *
 * Table rows come from one provider per group (kRowProviders in Batch38.cpp). Each group keeps
 * its provider in its own file, so adding a group touches this list and nothing else.
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
		std::string group;       ///< "E. DLSS 5 Neural Rendering", ...
		std::string name;        ///< what the item is
		bool installed;          ///< the owning feature is loaded
		bool own;                ///< the item's own switch (true for items with no switch of their own)
		std::string ownText;     ///< the item's own setting, as text
		std::string nowText;     ///< what runs this frame; empty = derive On/Off from own && master
		std::string where;       ///< menu path
		bool governed = true;    ///< false = a diagnostic the master switch does not turn off
		bool* toggle = nullptr;  ///< non-null: "Own setting" is drawn as a checkbox on this bool
	};

	/// @brief Appends the rows of one group.
	using RowProvider = void (*)(std::vector<Row>&);

	/// @brief E. DLSS 5 Neural Rendering rows (Batch38NeuralRendering.cpp).
	void RowsNeuralRendering(std::vector<Row>& rows);

	/// @brief Contents of the Advanced -> "Batch 38" tab: master switch and the item table.
	void DrawTab();
}
