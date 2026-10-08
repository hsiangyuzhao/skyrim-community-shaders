#pragma once

/**
 * @brief Former batch 38 master switch, frozen on.
 *
 * The master switch was removed from the menu; every item now follows only its own setting in
 * its feature's menu, which is exactly what ran with the master on (its default, never turned
 * off). The old key Advanced."Batch 38".Master is ignored.
 *
 * Covers: volumetric fog and smoke receiving sun shadow, the skin SSS upgrade, Local Exposure,
 * the Skylighting fixes and DLSS 5 Neural Rendering's coexistence with frame generation.
 */
namespace Batch38
{
	/// @brief Always true (the master switch is frozen on).
	constexpr bool IsOn() { return true; }
}
