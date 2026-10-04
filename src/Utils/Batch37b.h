#pragma once

/**
 * @brief Batch 37b master switch (Advanced -> "Batch 37b").
 *
 * Off = every governed batch 37b item takes its batch 37a path, whatever its own switch says.
 * Each call site ANDs its own setting with IsOn(), so the switch is read every frame: no
 * restart and no cache clear (C-4 recompiles the deferred composite by itself on the next
 * frame, which is a one-off hitch of a fraction of a second).
 *
 * Governed (C group, settings audit):
 * - C-1 Post-processing: last effect writes straight into the game's buffer, one copy instead of
 *   two (settings.postProcessDirectOutput).
 * - C-2 Screen Space GI: with Indirect Lighting off, skip the passes that only feed IL
 *   (radiance copy + prefilter, IL blur) (settings.ssgiSkipIlPasses).
 * - C-4 Screen Space GI: AO does not darken direct light (settings.ssgiAoSparesDirect,
 *   default off).
 *
 * Not governed (fixes): C-3 REBLUR slider range, D occlusion dry-run readback.
 */
namespace Batch37b
{
	struct Settings
	{
		bool master = true;
		/// C-1: the final post-processing effect writes into the game's buffer itself, so only the
		/// copy into the second buffer remains. Bit-identical output.
		bool postProcessDirectOutput = true;
		/// C-2: Screen Space GI with Indirect Lighting off skips the radiance copy/prefilter and the
		/// IL blur. AO and contact AO bit-identical.
		bool ssgiSkipIlPasses = true;
		/// C-4: Screen Space GI AO no longer darkens direct light (default off = 37a look).
		bool ssgiAoSparesDirect = false;
	};

	inline Settings settings{};

	/// @brief Whether batch 37b changes may run this frame.
	inline bool IsOn() { return settings.master; }

	/// @brief C-1 in effect this frame.
	inline bool PostProcessDirectOutputActive() { return IsOn() && settings.postProcessDirectOutput; }
	/// @brief C-2 in effect this frame.
	inline bool SsgiSkipIlPassesActive() { return IsOn() && settings.ssgiSkipIlPasses; }
	/// @brief C-4 in effect this frame.
	inline bool SsgiAoSparesDirectActive() { return IsOn() && settings.ssgiAoSparesDirect; }

	inline void Load(const json& a_json)
	{
		if (!a_json.is_object())
			return;
		const auto flag = [&a_json](const char* a_key, bool& o_value) {
			if (a_json.contains(a_key) && a_json[a_key].is_boolean())
				o_value = a_json[a_key].get<bool>();
		};
		flag("Master", settings.master);
		flag("PostProcessDirectOutput", settings.postProcessDirectOutput);
		flag("SsgiSkipIlPasses", settings.ssgiSkipIlPasses);
		flag("SsgiAoSparesDirect", settings.ssgiAoSparesDirect);
	}

	inline json Save()
	{
		json o;
		o["Master"] = settings.master;
		o["PostProcessDirectOutput"] = settings.postProcessDirectOutput;
		o["SsgiSkipIlPasses"] = settings.ssgiSkipIlPasses;
		o["SsgiAoSparesDirect"] = settings.ssgiAoSparesDirect;
		return o;
	}

	/// @brief Contents of the Advanced -> "Batch 37b" tab: master switch and the item table.
	void DrawTab();

	/// @brief The three item checkboxes, drawn where each item lives in its feature's menu.
	void DrawPostProcessCheckbox();
	void DrawSsgiCheckboxes();
}
