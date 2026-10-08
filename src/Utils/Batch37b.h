#pragma once

/**
 * @brief Former batch 37b master switch and its post-processing / SSGI items.
 *
 * The master switch was removed from the menu and is frozen on (its default, never turned
 * off): every item follows only its own setting. The old key Advanced."Batch 37b".Master is
 * ignored.
 *
 * - Post-processing: the last effect writes straight into the game's buffer (one copy instead
 *   of two). Frozen on; bit-identical output. Old key PostProcessDirectOutput ignored.
 * - Screen Space GI with Indirect Lighting off skips the passes that only feed IL. Frozen on;
 *   AO bit-identical. Old key SsgiSkipIlPasses ignored.
 * - Screen Space GI AO does not darken direct light: a real option, default off, drawn and
 *   saved by Screen Space GI ("AoSparesDirect"). The old key Advanced."Batch 37b".
 *   SsgiAoSparesDirect is migrated by Load() (Screen Space GI's own key wins when present).
 */
namespace Batch37b
{
	struct Settings
	{
		/// SSGI AO no longer darkens direct light (default off = the original look).
		bool ssgiAoSparesDirect = false;
	};

	inline Settings settings{};

	/// @brief Always true (the master switch is frozen on).
	constexpr bool IsOn() { return true; }

	/// @brief Post-processing writes the last effect into the game's buffer (frozen on).
	constexpr bool PostProcessDirectOutputActive() { return true; }
	/// @brief SSGI skips the IL-only passes while IL is off (frozen on).
	constexpr bool SsgiSkipIlPassesActive() { return true; }
	/// @brief SSGI AO spares direct light this frame.
	inline bool SsgiAoSparesDirectActive() { return settings.ssgiAoSparesDirect; }

	/// @brief Migrates the old Advanced."Batch 37b" object (only SsgiAoSparesDirect is kept).
	inline void Load(const json& a_json)
	{
		if (a_json.is_object() && a_json.contains("SsgiAoSparesDirect") && a_json["SsgiAoSparesDirect"].is_boolean())
			settings.ssgiAoSparesDirect = a_json["SsgiAoSparesDirect"].get<bool>();
	}
}
