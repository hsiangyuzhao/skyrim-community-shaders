#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

/**
 * @brief (41a) A/B Compare: two in-memory settings snapshots and a one-key switch between them.
 *
 * "Store current as A/B" snapshots every loaded feature's settings plus the global options that
 * change the picture (Shader Defines, the Engine block, Enable Shaders, Replace Original Shaders,
 * Disable at Boot). "Switch A <-> B" (button or hotkey, default F9) applies the other slot by
 * calling each feature's own LoadSettings - the same call Restore Saved Settings ends in - but only
 * for features whose values actually differ, so nothing is rebuilt or recompiled for nothing.
 *
 * Safety:
 *  - The switch is only queued by the button / hotkey; it runs at the next Present, before the
 *    ImGui frame starts (the rendering of the frame is finished, the next has not begun), and at
 *    most once per kDebounceSeconds. Presses inside that window are dropped.
 *  - Nothing here writes SettingsUser.json. Only the CS Save button does.
 *  - Features that are not loaded, and the UI-only features (Performance Overlay, RenderDoc,
 *    VR, Weather Picker), are neither captured nor applied.
 *  - Settings that only take effect after a restart are applied where possible and marked in
 *    the difference list ("Disable at Boot" is never applied live).
 *  - Locked out while the Performance Overlay's timed A/B test runs (it reloads the settings
 *    files on its own timer).
 *
 * Slots can be kept in SettingsAB.json next to SettingsUser.json so they survive a restart.
 */
namespace ABCompare
{
	enum class Slot : uint8_t
	{
		None,
		A,
		B
	};

	/// Snapshot the live settings into a slot (UI thread / Present). Makes that slot the active one.
	void Store(Slot a_slot);

	/// Ask for a switch to the other slot. Queued; applied at the next ProcessPending().
	void RequestToggle();

	/// Called once per Present, before the ImGui frame starts. Applies a queued switch.
	void ProcessPending();

	/// Restore Saved Settings / Load: the live settings no longer belong to a slot.
	void OnSettingsReloaded();

	/// True while the on-screen tag is showing, so the overlay renders that frame.
	bool WantsOverlayFrame();

	/// Short-lived "A" / "B" tag after a switch. Inside the ImGui frame.
	void DrawIndicator();

	/// Compact controls at the top of the CS menu (always visible on every page).
	void DrawMenuStrip();

	/// One line in the Performance Overlay (F10) header. Draws nothing until a slot is stored.
	void DrawOverlayHeaderLine();

	/// meta.ab_compare for the F12 JSON.
	nlohmann::json GetMetaJson();

	/// Input thread: true when this keyboard button event belongs to the A/B key and must not
	/// reach the game (F9 is Skyrim's Quickload by default).
	bool ShouldBlockKeyFromGame(uint32_t a_dikCode);

	/// Hotkey capture state for the "Change" button (read by Menu::ProcessInputEventQueue).
	inline bool capturingKey = false;
}
