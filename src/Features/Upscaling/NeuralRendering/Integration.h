#pragma once

#include <cstdint>

namespace NeuralRendering
{
	/**
	 * @brief (batch 38a) Per-frame entry point ahead of upscaling.
	 *
	 * Called every frame from Upscaling's post-processing hook, right before DLSS runs, whether or
	 * not the feature is on: this is where it decides what the frame does, releases its GPU memory
	 * when switched off or blocked, rebuilds when placement, extent or model resolution change, and
	 * logs one state line whenever any of that changes. With "Run before upscaling" on it also runs
	 * the pass here, on the HDR scene at render resolution, so DLSS upscales the network's output.
	 *
	 * Does nothing while Advanced > Batch 38 is off (37c had no such hook).
	 */
	void BeforeUpscaling();

	/**
	 * @brief Runs DLSS Neural Rendering on the final tonemapped scene, immediately before UI.
	 *
	 * Called from Upscaling's post-processing hook after the engine's post chain, the one moment in
	 * the frame where the bound target holds the finished LDR image and nothing has drawn over it.
	 * With Batch 38 on it runs only when BeforeUpscaling armed the after-upscaling pass this frame.
	 *
	 * @return true when the neural pass ran this frame. Every reason it might not -- the feature
	 *         being off, a precondition unmet, the runtime absent -- returns false without
	 *         touching any render target, so a false return is never a partially applied frame.
	 */
	bool ApplyLdr();

	/** Releases all runtime, D3D12 and shared-resource state. Safe to call when nothing exists. */
	void Reset();

	/// @brief (batch 38a) What the pass did, for the menu, the Batch 38 table and the F12 JSON.
	struct FrameStatus
	{
		bool batch38 = true;             ///< Batch 38 master switch, as read this frame
		bool enabled = false;            ///< the feature's own switch
		bool running = false;            ///< the pass ran on the most recent frame it was asked to
		bool beforeUpscaling = false;    ///< placement in effect
		bool frameGeneration = false;    ///< frame generation requested this frame
		const char* blockedReason = "";  ///< empty while allowed to run
		std::uint32_t width = 0;         ///< extent the network is given (colour = guides)
		std::uint32_t height = 0;
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		std::uint32_t outputWidth = 0;
		std::uint32_t outputHeight = 0;
		std::uint32_t modelPercent = 100;  ///< model resolution in effect
		std::uint64_t vramMB = 0;          ///< this process's local video memory at the last state change
	};

	/// @brief (batch 38a) Status as of the most recent frame.
	const FrameStatus& GetFrameStatus();

	/// @brief (batch 38a) Status plus timings, for the performance overlay's saved frame.
	json StatusJson();
}
