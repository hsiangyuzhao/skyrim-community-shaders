#pragma once

/**
 * @brief Batch 36 master switch (Advanced -> Batch 36).
 *
 * Off = every batch 36 optimisation below takes its batch 34 path, whatever its own switch in
 * its feature's menu says. Each call site ANDs its own setting with IsOn(), so the switch is
 * read every frame: no restart, no recompile, no cache clear.
 *
 * Governed:
 * - Screen Space Ray Tracing: Half-Resolution Diffuse Denoising, Fold Diffuse Unpack Into
 *   Composite, Direct Motion Vectors (Settings::ReblurDiffuseHalfRes / ReblurFoldDiffuseUnpack /
 *   ReblurDirectMotionVectors)
 * - Screen Space GI: Skip IL While SSRT Diffuse Is On (Settings::SkipILUnderSSRTDiffuse)
 * - Variable Rate Shading: can only be forced off here, never on (its own Enable stays the
 *   only way to turn it on).
 *
 * Not governed: the overlay's engine-pass timers and the menu text.
 */
namespace Batch36
{
	struct Settings
	{
		bool master = true;
	};

	inline Settings settings{};

	/// @brief Whether batch 36 optimisations may run this frame.
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
