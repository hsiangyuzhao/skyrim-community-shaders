#pragma once

/**
 * @brief Batch 36 master switch (Advanced -> Batch 36).
 *
 * Off = every batch 36 optimisation below takes its batch 34 path, whatever its own switch in
 * its feature's menu says. Each call site ANDs its own setting with IsOn(), so the switch is
 * read every frame: no restart, no recompile, no cache clear.
 *
 * Governed:
 * - Screen Space Ray Tracing: Fold Diffuse Unpack Into Composite, Direct Motion Vectors
 *   (Settings::ReblurFoldDiffuseUnpack / ReblurDirectMotionVectors); (batch 36b) REBLUR Mode
 *   (Efficiency, which carries deviation 3), AO Source, Direction-Aware Reinjection (deviation 2),
 *   Bounce Light Skips AO (deviation 4), Reflection Misses Use Scene Cubemap (deviation 5).
 *   Off = Quality mode, Screen Space GI's AO, and the batch 34 composite and fallbacks.
 *   (batch 36c) Steady Confidence and AO (Settings::FilterSignalsFromRays) and the efficiency-mode
 *   pre-blur only act inside efficiency mode / the denoiser AO tiers, so the master switch reaches
 *   them through those; the 36c direction-aware reinjection rework is inside deviation 2.
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
