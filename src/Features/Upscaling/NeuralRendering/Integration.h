#pragma once

#include <cstdint>

namespace NeuralRendering
{
	// A snapshot of the last real frame considered by the neural pass. Counters survive
	// A/B setting changes and are cleared only by Reset(), so a single session can be
	// inspected without interpreting a frame-rate stream of log messages.
	struct Diagnostics
	{
		bool requested = false;
		bool frameGenerationRequested = false;
		bool appliedThisFrame = false;
		bool historyResetThisFrame = false;
		bool sizeContractSatisfied = false;
		bool failureLatched = false;
		std::uint64_t attempts = 0;
		std::uint64_t successes = 0;
		std::uint64_t failures = 0;
		std::uint64_t historyResets = 0;
		std::uint32_t sceneWidth = 0;
		std::uint32_t sceneHeight = 0;
		std::uint32_t depthWidth = 0;
		std::uint32_t depthHeight = 0;
		std::uint32_t motionWidth = 0;
		std::uint32_t motionHeight = 0;
		std::uint32_t sceneFormat = 0;
		std::uint32_t depthFormat = 0;
		std::uint32_t motionFormat = 0;
		std::uint32_t ngxResult = 0;
		std::uintptr_t sceneResource = 0;
		std::uintptr_t depthResource = 0;
		std::uintptr_t motionResource = 0;
		const char* status = "not-called";
	};

	[[nodiscard]] Diagnostics GetDiagnostics();

	/**
	 * @brief Runs DLSS Neural Rendering on the final tonemapped scene, immediately before UI.
	 *
	 * Called from the upscaling post-processing hook before sharpening and UI. The scene comes
	 * from the currently bound render target, not an assumed engine render-target slot.
	 *
	 * @return true when the neural pass ran this frame. Every reason it might not -- the feature
	 *         being off, a precondition unmet, the runtime absent -- returns false without
	 *         touching any render target, so a false return is never a partially applied frame.
	 */
	bool ApplyLdr();

	/** Releases all runtime, D3D12 and shared-resource state. Safe to call when nothing exists. */
	void Reset();
}
