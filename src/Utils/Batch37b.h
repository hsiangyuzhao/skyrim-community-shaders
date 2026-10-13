#pragma once

/**
 * @brief Screen Space GI "AO does not darken direct light" (from batch 37b).
 *
 * A real option, default off, drawn and saved by Screen Space GI ("AoSparesDirect"); read by
 * Deferred for the composite. The 37b master switch and its frozen items (post-processing
 * direct output, SSGI skipping the IL-only passes while IL is off) are permanent now (44f).
 */
namespace Batch37b
{
	struct Settings
	{
		/// SSGI AO no longer darkens direct light (default off = the original look).
		bool ssgiAoSparesDirect = false;
	};

	inline Settings settings{};

	/// @brief SSGI AO spares direct light this frame.
	inline bool SsgiAoSparesDirectActive() { return settings.ssgiAoSparesDirect; }
}
