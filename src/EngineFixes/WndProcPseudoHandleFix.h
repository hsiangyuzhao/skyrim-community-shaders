#pragma once

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
namespace WndProcPseudoHandleFix
{
	void Install();
}
