#pragma once

/**
 * @brief Batch 36f master switch (Advanced -> "Batch 36f").
 *
 * Off = every batch 36f denoiser saving takes its batch 36e path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no
 * restart, no cache clear.
 *
 * Governed (36f items only; later batches get their own switch):
 * - Screen Space Ray Tracing: Skip Reflection Pre-pass (Settings::ReblurSkipSpecularPrepass),
 *   Distance Limit (Settings::DistanceLimit), Fold Unpack Into Composite
 *   (Settings::ReblurFoldUnpack)
 * - NRD: motion-vector copy limited to the render rectangle (no switch of its own)
 *
 * Not governed: the Denoiser breakdown panel fixes (display only).
 */
namespace Batch36f
{
	struct Settings
	{
		bool master = true;
	};

	inline Settings settings{};

	/// @brief Whether batch 36f savings may run this frame.
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
}
