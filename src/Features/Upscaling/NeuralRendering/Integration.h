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
	 * Runs every frame.
	 */
	void BeforeUpscaling();

	/**
	 * @brief Runs DLSS Neural Rendering on the final tonemapped scene, immediately before UI.
	 *
	 * Called from Upscaling's post-processing hook after the engine's post chain, the one moment in
	 * the frame where the bound target holds the finished LDR image and nothing has drawn over it.
	 * It runs only when BeforeUpscaling armed the after-upscaling pass this frame.
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
		// (batch 38c)
		std::uint32_t workWidth = 0;  ///< the model's content extent (Model Resolution, our own downscale)
		std::uint32_t workHeight = 0;
		std::uint32_t paddedWidth = 0;  ///< the network's extent (content padded to the network grid)
		std::uint32_t paddedHeight = 0;
		bool toneMatched = false;          ///< before upscaling: tone-matched input in effect
		std::uint32_t inputPrecision = 0;  ///< before upscaling: 0 8-bit, 1 10-bit, 2 16-bit float
		float toneStrength = 0.0f;         ///< tone preservation in effect for the placement
		bool tuningAtCreate = false;
	};

	/// @brief (batch 38a) Status as of the most recent frame.
	const FrameStatus& GetFrameStatus();

	/// @brief (batch 38a) Status plus timings, for the performance overlay's saved frame.
	json StatusJson();

	/// @brief (batch 38c) Where the before-upscaling tone curve came from on the last frame
	/// ("game exposure + colour grading", "fallback curve", ...), for the menu and the Batch 38 table.
	const char* ToneSourceText();
}
