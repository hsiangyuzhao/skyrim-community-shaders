#pragma once

/**
 * @brief Denoiser savings (former batch 36f master switch).
 *
 * The master switch was removed from the menu: the savings are always on, which is how every
 * saved configuration ran them (the switch defaulted to on and was never turned off). The old
 * key Advanced."Batch 36f".Master is ignored. Call sites keep ANDing their own setting with
 * IsOn(), so each item still follows its own switch exactly as before.
 *
 * Covers: Screen Space Ray Tracing Skip Reflection Pre-pass, Distance Limit, Fold Unpack Into
 * Composite; NRD motion-vector copy limited to the render rectangle.
 */
namespace Batch36f
{
	/// @brief Always true (the master switch is frozen on).
	constexpr bool IsOn() { return true; }
}
