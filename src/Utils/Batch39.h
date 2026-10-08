#pragma once

/**
 * @brief Former batch 39 master switch, frozen on.
 *
 * The master switch was removed from the menu; every item now follows only its own setting,
 * which is exactly what ran with the master on (its default, never turned off). The old key
 * Advanced."Batch 39".Master is ignored.
 *
 * Covers: the engine-level changes in Batch39Engine (water reflection cubemap, depth prepass,
 * temporal LOD dither, texture clarity) and Dynamic Snow.
 */
namespace Batch39
{
	/// @brief Always true (the master switch is frozen on).
	constexpr bool IsOn() { return true; }
}
