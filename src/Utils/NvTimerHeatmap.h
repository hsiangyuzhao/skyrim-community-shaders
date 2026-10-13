#pragma once

#include <cstdint>
#include <d3d11.h>

/**
 * @brief (45c) Diagnostic per-tile cost heatmap of the SSRT diffuse ray march, measured with the
 *        NVIDIA global timer (Advanced > NVIDIA > Debug > Timer heatmap). Off by default.
 *
 * The heatmap permutation of ssrt_raymarch.hlsl adds every thread's main() duration (ns) into one
 * R32_UINT cell per 8x8 thread group. This file owns that surface, reads it back a few frames later
 * without ever waiting on the GPU, turns it into a small colour image on the CPU and shows it in an
 * ImGui window. Nothing here runs while the switch is off.
 */
namespace Util::NvTimerHeatmap
{
	/**
	 * @brief Render thread, right before the diffuse ray march dispatch. Ensures and clears the tile
	 *        surface and returns its UAV (bind at u7), or nullptr when the heatmap cannot run.
	 * @param a_threadsPerTile threads per thread group (8 * 8 * samples per pixel), for the per-thread average.
	 */
	ID3D11UnorderedAccessView* Begin(ID3D11DeviceContext* a_context, uint32_t a_tilesX, uint32_t a_tilesY, uint32_t a_threadsPerTile);

	/// Right after the dispatch (UAV already unbound): queues the non-blocking readback.
	void End(ID3D11DeviceContext* a_context);

	/// The overlay has something to show (keeps the ImGui frame alive while the menu is closed).
	bool WantsOverlayFrame();

	/// Inside the ImGui frame (render thread): folds in a finished readback and draws the window.
	void DrawOverlay();
}
