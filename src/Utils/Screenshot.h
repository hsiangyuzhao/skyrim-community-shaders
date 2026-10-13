#pragma once

#include <string>

/**
 * @brief (44e, D1) Screenshot of exactly what is presented: the scene, the game's UI, the CS menu
 * (F11) and the overlays (F10), taken at the very end of the frame -- after the HDR composite, on
 * the image that is copied to the swap chain, before Present.
 *
 * The frame is copied to a staging texture on the render thread (one CopyResource), read back a
 * few frames later without stalling, and encoded on a worker thread. HDR (PQ / BT.2020) frames
 * are decoded and tone-mapped to SDR for the PNG; optionally the untouched PQ image is saved too,
 * as a 16-bit PNG (values are the PQ code, BT.2020 primaries; not tagged, so viewers show it
 * flat -- it is for analysis).
 *
 * Files: Documents\My Games\Skyrim Special Edition\SKSE\CommunityShaders\Screenshots\cs-<time>.png
 */
namespace Util::Screenshot
{
	/// Queues a capture of the next presented frame. a_stamp: file-name stamp ("" = now).
	void Request(const std::string& a_stamp = {});

	/// Present hooks: the final image of this frame. a_pq = it is HDR10 (PQ, BT.2020).
	/// Called once per frame; does nothing unless a capture is queued.
	void OnFinalImage(ID3D11Texture2D* a_image, bool a_pq);

	/// Once per frame, before OnFinalImage: reads back a finished copy without waiting.
	void Poll();

	/// The D3D11 swap chain path (no D3D12 proxy): capture from its back buffer.
	void OnD3D11Present(IDXGISwapChain* a_swapChain);

	std::string LastFile();

	/// "cs-YYYYMMDD-HHMMSS-mmm" for now; shared by the F12 JSON and its screenshot.
	std::string MakeStamp();
}
