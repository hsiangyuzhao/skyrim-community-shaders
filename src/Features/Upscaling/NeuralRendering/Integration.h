#pragma once

namespace NeuralRendering
{
	/**
	 * @brief Runs DLSS Neural Rendering on the final tonemapped scene, immediately before UI.
	 *
	 * Called from Upscaling::MenuManagerDrawInterfaceStartHook before the engine's own interface
	 * pass runs, which is the one moment in the frame where kFRAMEBUFFER holds the finished LDR
	 * image and nothing has drawn over it yet.
	 *
	 * @return true when the neural pass ran this frame. Every reason it might not -- the feature
	 *         being off, a precondition unmet, the runtime absent -- returns false without
	 *         touching any render target, so a false return is never a partially applied frame.
	 */
	bool ApplyLdr();

	/** Releases all runtime, D3D12 and shared-resource state. Safe to call when nothing exists. */
	void Reset();

	/**
	 * @brief Frames in which ApplyLdr was reached, and of those, frames the pass actually ran.
	 *
	 * The pair separates the two ways this feature can do nothing. Attempts staying at zero
	 * means the call site is not being reached at all, which is a different problem -- and a
	 * different fix -- from attempts climbing while applications stay at zero, which means a
	 * gate inside is rejecting every frame and the log says which.
	 */
	struct Counters
	{
		unsigned long long attempts = 0;
		unsigned long long applications = 0;
	};

	Counters GetCounters();
}
