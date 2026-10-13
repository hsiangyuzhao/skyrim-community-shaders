#pragma once

/**
 * @brief Former batch 38 master switch, frozen on.
 *
 * Only DLSS 5 Neural Rendering still reads it (its 37c fallback path); that goes with the
 * Neural Rendering cleanup, which is on hold while Neural Rendering testing is paused. Every
 * other batch 38 item lost the switch in 44f.
 */
namespace Batch38
{
	/// @brief Always true (the master switch is frozen on).
	constexpr bool IsOn() { return true; }
}
