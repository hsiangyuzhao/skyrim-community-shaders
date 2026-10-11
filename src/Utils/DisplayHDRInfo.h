#pragma once

#include <nlohmann/json.hpp>

/**
 * @brief (42c) Native HDR, phase 0: read-only diagnostics of the display and our swap chains.
 *
 * Nothing here changes a format, a colour space or anything that is presented. It reads:
 *  - IDXGIOutput6::GetDesc1 of the monitor the game window is on (colour space, min / max /
 *    max full-frame luminance, bits per colour);
 *  - IDXGISwapChain3::CheckColorSpaceSupport for HDR10 (G2084/P2020), scRGB (G10/P709) and sRGB
 *    (G22/P709) on every swap chain this plugin owns, the Streamline DLSS-G proxy included;
 *  - whether Windows HDR is on (the output's colour space, and the Windows display-config
 *    "advanced colour" state with the SDR white level).
 *
 * The values are logged at startup and whenever the display changes, shown in Upscaling's
 * "Display / HDR info" section, and written to the F12 JSON as meta.display_hdr.
 */
namespace DisplayHDRInfo
{
	/// Once per presented frame (render thread). The first call queries and logs; later calls
	/// re-query at most every 2 s, and only when DXGI reports a display change, the window moved
	/// to another monitor, or RequestRefresh() was called.
	void Tick();

	/// Re-query on the next Tick().
	void RequestRefresh();

	/// Read-only ImGui section (with a Refresh button).
	void DrawSettings();

	/// Snapshot for the F12 JSON meta block.
	nlohmann::json GetMetaJson();
}
