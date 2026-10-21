#pragma once

#include <cstdint>

struct ID3D12Fence;

/**
 * @brief (45m) GPU-aware frame-start pacing for the D3D11 -> D3D12 interop path.
 *
 * Why it exists: Skyrim renders on the D3D11 device, but Reflex (Streamline) is attached to the
 * D3D12 interop device that only copies, upscales and presents. The driver's Reflex sleep cannot
 * see the D3D11 work, so it takes every frame for CPU-bound and never sleeps; and with no D3D11
 * Present nothing else bounds how far the CPU runs ahead of the GPU. The CPU then waits inside
 * Present (DLSS-G) or keeps queuing (menus, no frame generation) and input lag grows by whole frames.
 *
 * What it does: every Present signals a per-frame "GPU finished this frame" fence value (the
 * D3D12 queue signal that the next frame's D3D11 work already waits on). A waiter thread notes when
 * each one completes. At frame start -- the Reflex sleep point in Main::Update, before input is
 * sampled -- the pacer predicts when the GPU will finish the previous frame and sleeps until the
 * CPU work of the new frame (measured) would end right then. A hard bound then waits for the frame
 * before that, so at most one finished-on-CPU frame is ever queued. Measurement always runs (for
 * the trace); sleeping only when switched on.
 */
namespace Util::LatencyPacer
{
	struct FrameInfo
	{
		bool enabled = false;      ///< pacing was switched on for this frame
		bool tracking = false;     ///< the interop fence is being followed
		float sleepMs = 0.0f;      ///< predictive sleep taken
		float capWaitMs = 0.0f;    ///< hard-bound wait taken (frame before the previous one)
		int inFlight = -1;         ///< frames submitted but not finished by the GPU at frame start
		float predictedMs = -1.0f; ///< predicted time until the previous frame finishes (at frame start)
		float leadMs = -1.0f;      ///< CPU time the pacer reserves (frame start to D3D11 submit, p75)
		float gpuPeriodMs = -1.0f; ///< median interval between GPU frame completions
		int inFlightAfter = -1;    ///< (45n) frames not finished by the GPU when the frame's CPU work starts (after the sleep / bound)
		float stallMs = 0.0f;      ///< (46n) the previous frame's CPU time blocked inside D3D11 waiting for the GPU (calls of 0.5 ms or more)
		float correctionMs = 0.0f; ///< (46n) learned extra sleep that moves such waits to the frame start (stall feedback)
		float hiddenWaitMs = -1.0f; ///< (46o) the previous frame's main-thread wait in the render segments up to the submit (-1 = not measured)
		float waitUsedMs = 0.0f;   ///< (46o) the previous frame's wait left out of its lead sample (46n stall, or the larger hidden wait)
		float backstopMs = 0.0f;   ///< (46o) lead taken off by the unlock backstop this frame
		float hiddenBaseMs = 0.0f; ///< (46o) the hidden wait frames have anyway (p25 while sleeping); only the part above it counts
		bool reset = false;        ///< (46o) the lead samples and learned sleep were cleared at this frame (after a pause / loading / map)
		// (46p) "Never Over-Sleep": planned sleep not taken because the previous frame had finished on the GPU (+ slack), learned
		// sleep taken off by the fast decay this frame, the previous frame's wait not learned (right after a GPU spike).
		float overSleepCutMs = 0.0f;
		float decayMs = 0.0f;
		int spikeSkip = 0;
		float doneIntervalMs = -1.0f; ///< (46p) the last single-frame GPU completion interval the waiter saw (-1 = none yet)
		float phaseShiftMs = 0.0f;    ///< (46p) smoothed minus raw predicted completion (computed always; used with Smooth GPU Phase)
		float learnedCapMs = 0.0f;    ///< (47m) cap on the learned sleep: 1.5 x the largest of the last 60 learned waits, at most 20 ms (0 = none fed yet)
		float learnedCutMs = 0.0f;    ///< (47m) learned sleep not taken: it would have run past the previous frame's predicted completion
	};

	/// (46o) The pacer's switches for one frame. (47m) Always on, no switches any more: (46n) GPU waits moved to the frame
	/// start, (46o) hidden GPU waits counted and the unlock backstop, (46p) never over-sleep (fence-bounded sleep, fast decay,
	/// spike skip).
	struct Options
	{
		bool enabled = false;        ///< sleep (else measure only)
		float marginMs = 1.0f;
		bool paused = false;         ///< (46o) game paused / loading screen / map open: the samples are cleared when it ends
		bool smoothPhase = false;    ///< (46p) "Smooth GPU Phase": the predicted completion from the median phase of the last completions
	};

	/// Main thread, in Main::Update before the Reflex sleep. Sleeps only when a_options.enabled.
	void PaceFrameStart(const Options& a_options);

	/// (46n) Main thread, any D3D11 call on the immediate context that can wait for the GPU (Map, Flush, GetData): its duration
	/// in TSC ticks (__rdtsc) or ms. Only calls of 0.5 ms or more count. Not counted in the CPU time the pacer reserves.
	void AddD3D11WaitTsc(uint64_t a_ticks);
	void AddD3D11WaitMs(double a_ms);

	/// (47h) The last submitted frame's time waiting for the GPU inside its CPU work (the 46n blocking D3D11 calls, or the larger
	/// hidden render-segment wait when the frame started with the GPU behind -- measured whether or not the pacer uses them); 0 from
	/// the next frame's start until its submit. For the overlay's bottleneck reading (FrameTrace::GetSeriesStats, the summary).
	float FrameGpuWaitMs();
	/// (47h) The same for the previous frame (for readers before this frame's submit: the overlay, drawn before Present).
	float PreviousFrameGpuWaitMs();

	/// Main thread, right after PaceFrameStart and the Reflex sleep: the frame's CPU work starts now.
	void OnSimulationStart();

	/// DX12SwapChain::Present, when the frame's D3D11 work has been handed to the GPU.
	void OnD3D11Submitted();

	/// DX12SwapChain::Present, after the per-frame D3D12 queue signal.
	void OnFrameEndSignaled(ID3D12Fence* a_fence, uint64_t a_value);

	/// Info for the frame that is being built now (valid from PaceFrameStart to the next one).
	const FrameInfo& Last();
}
