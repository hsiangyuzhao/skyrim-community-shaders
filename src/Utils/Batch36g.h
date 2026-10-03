#pragma once

/**
 * @brief Batch 36g master switch (Advanced -> "Batch 36g").
 *
 * Off = every batch 36g experiment switch takes its default value (= batch 36f), whatever the
 * switch itself says. Read every frame: no restart, no cache clear.
 *
 * Governed (36g items only): Screen Space Ray Tracing's tracing pattern
 * (Settings::B36gPattern), merged denoiser (B36gMergedDenoiser), confidence source
 * (B36gConfidenceSource) and diffuse pre-blur (B36gDiffusePrepass).
 *
 * Deliberately independent of Batch36f: neither master switch changes the other's items. A pattern
 * that needs REBLUR's reflection pre-pass overrides 36f's "Skip Reflection Pre-pass" while it runs,
 * and the Batch 36g tab says so.
 */
namespace Batch36g
{
	struct Settings
	{
		bool master = true;
	};

	inline Settings settings{};

	/// @brief Whether batch 36g experiments may run this frame.
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
