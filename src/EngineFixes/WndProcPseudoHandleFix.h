#pragma once

#include <nlohmann/json.hpp>

// Compatibility guard for third-party plugins that call the return value of
// Get/SetWindowLongPtr(GWLP_WNDPROC) directly instead of going through
// CallWindowProc.
//
// When the window procedure's character set (ANSI/Unicode) differs from the
// API flavour the caller uses, user32 does not return the real previous
// procedure but an A/W conversion pseudo-handle (0xFFFF0xxx). Such a value is
// only valid as the first argument of CallWindowProcA/W; jumping to it crashes.
// This happens in practice when a Unicode subclass (e.g. a frame-generation
// overlay calling SetWindowLongPtrW) lands on the ANSI game window and a later
// plugin subclasses with SetWindowLongPtrA and calls the "previous" proc
// directly.
//
// The guard detours the four user32 exports and, for GWLP_WNDPROC only,
// replaces a pseudo-handle result with a real, callable forwarder that does
// CallWindowProcA/W(pseudoHandle, ...). Real pointers are never touched.
//
// (44d) Window / overlay compatibility, in the same place because it watches the same window:
//  - every window-procedure subclass is logged once per module (system modules too), so the log
//    shows who sits on the game window and in which order;
//  - DirectInput's low-level keyboard hook (Skyrim's keyboard is exclusive): wrapped so the modifier
//    keys and overlay hotkeys also reach Windows (the NVIDIA overlay's Alt+Z), see Settings;
//  - RegisterRawInputDevices: RIDEV_NOHOTKEYS on a keyboard registration is removed and logged;
//  - SetWindowsHookEx / SetForegroundWindow / RegisterHotKey callers are logged once per module;
//  - which module the real swap chain's Present / Present1 lead into (overlay hooks), logged on change.
namespace WndProcPseudoHandleFix
{
	void Install();

	struct Settings
	{
		/// (44d) Skyrim takes the keyboard through DirectInput in exclusive mode; Windows' DirectInput
		/// then blocks every key from the system (its low-level keyboard hook returns 1 for all but
		/// Alt+Tab and Alt key-up), so Windows never sees Alt held and no Alt+key hotkey of another
		/// program fires -- the NVIDIA overlay's Alt+Z among them. The game still gets every key.
		/// 0 = as vanilla; 1 = let the modifier keys and the NVIDIA overlay's default hotkeys through
		/// (Alt + Z, R, F1, F2, F3, F9, F10, F12); 2 = every Alt combination except Alt+F4, Alt+Space,
		/// Alt+Enter and Alt+Esc. Live. Also removes RIDEV_NOHOTKEYS from raw-input keyboard
		/// registrations (none seen so far; logged if one appears).
		uint32_t overlayKeyMode = 1;
		/// Log window hooks, subclasses, focus grabs and Present hooks (once per module).
		bool logWindowHooks = true;
	};

	Settings& GetSettings();
	/// (44d) True while keys are let through to Windows: the game's window procedure then drops
	/// WM_SYSCOMMAND / SC_KEYMENU, so a tapped Alt cannot open the (invisible) window menu.
	bool BlockKeyMenu();
	void Load(const nlohmann::json& a_json);
	nlohmann::json Save();
	/// Advanced > Overlays & Window.
	void DrawSettings();

	/// (44d) Logs, on change, which module a swap chain's Present / Present1 lead into. Cheap; the
	/// caller rate-limits.
	void ProbePresentHooks(IUnknown* a_swapChain, const char* a_label);
}
