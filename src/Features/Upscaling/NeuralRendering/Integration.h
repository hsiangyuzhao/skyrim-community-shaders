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
}
