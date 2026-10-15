#include "Utils/LatencyPacer.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <intrin.h>
#include <mutex>
#include <thread>

#include <d3d12.h>

#include "Utils/EngineTiming.h"

namespace Util::LatencyPacer
{
	namespace
	{
		constexpr size_t kRing = 32;
		constexpr double kMaxSleepMs = 50.0;     // never pace a frame by more than this
		constexpr DWORD kCapTimeoutMs = 50;      // the hard bound gives up after this
		constexpr size_t kMinSamples = 8;        // estimates need this many samples first

		int64_t g_qpf = 0;
		double ToMs(int64_t a_ticks) { return g_qpf ? static_cast<double>(a_ticks) * 1000.0 / static_cast<double>(g_qpf) : 0.0; }
		int64_t Now()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		// ---- shared with the waiter thread ----
		std::mutex g_mutex;
		std::condition_variable g_cv;
		std::deque<uint64_t> g_pending;  // per-frame end values the GPU has not finished yet
		winrt::com_ptr<ID3D12Fence> g_fence;
		int64_t g_lastDoneTime = 0;
		std::array<float, kRing> g_intervals{};  // ms between consecutive single-frame completions
		size_t g_intervalCount = 0, g_intervalHead = 0;
		bool g_threadStarted = false;

		// ---- main thread only ----
		std::array<float, kRing> g_leads{};  // ms, frame start to D3D11 submit
		size_t g_leadCount = 0, g_leadHead = 0;
		int64_t g_simStart = 0;
		FrameInfo g_last{};
		HANDLE g_timer = nullptr;
		HANDLE g_capEvent = nullptr;
		int g_capTimeouts = 0;       // consecutive hard-bound timeouts
		bool g_capDisabled = false;  // switched off for the session after repeated timeouts

		// (46n) Waits inside D3D11 (Map / Flush / GetData of 0.5 ms or more) on the main thread. 46m traces after a fast travel
		// showed the pacer locked out: one Map(WRITE_DISCARD) waited ~10 ms for the GPU every frame, that wait counted as CPU work
		// in the lead (frame start to submit: 28 ms instead of 10-18), so the predictive sleep never fired and the wait stayed in
		// the middle of the frame. The lead no longer includes these waits, and with stall feedback the measured wait is learned
		// as extra sleep at the frame start (where the wait costs no input lag) until the in-frame wait is gone.
		constexpr double kStallCallMs = 0.5;     // shorter calls are work, not a wait for the GPU
		constexpr double kStallThresholdMs = 1.0;  // a frame with less in-frame waiting lets the learned sleep decay
		constexpr float kStallGain = 0.5f;       // share of a frame's wait added to the learned sleep
		constexpr float kStallDecayMs = 0.2f;    // learned sleep given back per frame without a wait
		constexpr float kMaxCorrectionMs = 20.0f;
		DWORD g_mainThread = 0;
		double g_stallMs = 0.0;      // this frame (since OnSimulationStart)
		float g_lastStallMs = 0.0f;  // the last frame's, taken at its submit
		bool g_stallFresh = false;   // g_lastStallMs not folded into the correction yet
		float g_correctionMs = 0.0f;
		uint64_t g_tscStart = 0;
		int64_t g_tscStartQpc = 0;
		double g_tscPerMs = 0.0;
		// (46n) Guard: a 2 s window; a warning when the pacer does not pace while the GPU is behind and the CPU waits inside D3D11.
		int64_t g_windowStart = 0;
		uint32_t g_windowFrames = 0, g_windowBehind = 0;
		double g_windowSleepMs = 0.0, g_windowStallMs = 0.0, g_windowPredMs = 0.0, g_windowLeadMs = 0.0;
		int64_t g_lastWarn = 0;

		// (46o) F1a "Count Hidden GPU Waits": 46n saw only Map / Flush / GetData. When the pacer stopped sleeping, the CPU's wait
		// for the GPU moved into untimed D3D11 state calls in the middle of the frame (the world and shadow passes), was counted as
		// CPU work in the lead, so it slept even less: locked (zig-zag frames, 172633 frames 740-886). The main thread's wait
		// (wall - thread CPU time, EngineTiming M1) in the render segments up to the submit is a wait too, when the frame started
		// with the GPU behind. The lead leaves out the larger of it and the 46n stall (not the sum: a blocking Map is in both).
		// The learned sleep is fed the median of the last 3 frames' waits, so one GPU spike (its wait lands in the next frame's
		// world pass) teaches nothing.
		constexpr double kHiddenMinMs = 0.5;  // less is noise (preemption, page faults)
		bool g_hiddenOn = false;
		bool g_frameBehind = false;         // this frame started with the GPU behind (in_flight >= 1)
		float g_lastHiddenMs = -1.0f;       // the last frame's render-segment wait (-1 = not measured)
		float g_lastWaitMs = 0.0f;          // the last frame's wait left out of its lead
		std::array<float, 3> g_waitHist{};  // the last three frames' waits
		size_t g_waitHistHead = 0;
		// The hidden wait the frame has anyway (job joins, preemption, our RT lanes): p25 of the hidden waits of frames that slept
		// 1 ms or more at their start (no GPU lock then). Only what is above it counts, so a constant non-GPU wait never grows the
		// learned sleep.
		std::array<float, kRing> g_hiddenBase{};
		size_t g_hiddenBaseCount = 0, g_hiddenBaseHead = 0;
		float g_hiddenBaseMs = 0.0f;
		constexpr float kBaseSleepMs = 1.0f;

		// (46o) F1b "Unlock Backstop": 30 frames in a row of no sleep while the GPU is behind and the frame period matches the GPU
		// period: the reserved lead shrinks 0.5 ms per frame (floor: the lead's p25, at least 2 ms) until the pacer sleeps again.
		// Then it is handed back slowly (0.1 ms per frame) as the clean lead samples come in; if frames get slower than the GPU
		// period at the start by over 3 %, it is handed back fast (1 ms per frame) and needs 30 more locked frames to start again.
		constexpr int kLockFrames = 30;
		constexpr float kLockSleepMs = 0.5f;
		constexpr float kPeriodTolerance = 0.03f;
		constexpr float kBackstopStepMs = 0.5f;
		constexpr float kBackstopReturnMs = 0.1f;
		constexpr float kBackstopFastReturnMs = 1.0f;
		constexpr float kBackstopMaxMs = 15.0f;
		constexpr float kBackstopFloorMs = 2.0f;
		int g_lockRun = 0;
		float g_backstopMs = 0.0f;
		float g_backstopRef = 0.0f;  // the GPU period when it engaged
		int64_t g_prevStart = 0;
		// Frame period (frame start to frame start), the mean of the last 16: zig-zag frames (34 / 24 / 15 ms) average out over whole
		// cycles, so a locked run is seen as "at the GPU period" and does not trip the "slower" test.
		constexpr size_t kPeriodRing = 16;
		std::array<float, kPeriodRing> g_periods{};
		size_t g_periodCount = 0, g_periodHead = 0;
		float g_periodMean = -1.0f;
		bool g_wasPaused = false;

		// (46p) "Never Over-Sleep". 46o traces in Solitude (203926 frames 50-150): the learned sleep sat at 13-20 ms on top of a
		// predicted 7-10 ms, the pacer slept ~20 ms, frames took 41-45 ms with the GPU working 22-29 ms (idle the rest). Every 2-3
		// frames a GPU spike's after-wait (10-15 ms, in the next frame's world pass) pumped it back up; it decays 0.2 ms a frame.
		// The prediction cannot bound it: the GPU period it uses is the completion interval, which an over-sleeping pacer
		// stretches to its own frame period (36-40 ms there). So the sleep waits on the interop fence itself: it ends at the
		// previous frame's actual completion + a small slack (nothing queued for the GPU after that: any later start only
		// idles it). A sleep cut short that way, or frames slower than the GPU period, cut the learned sleep fast; a frame
		// right after a GPU spike does not feed it. Off: 46o exactly.
		constexpr float kSlackShare = 0.1f;      // slack after the completion: this share of the GPU period ...
		constexpr float kSlackMinMs = 0.5f;      // ... within these bounds
		constexpr float kSlackMaxMs = 2.0f;
		constexpr float kCutMinMs = 0.5f;        // a shorter cut is timer noise
		constexpr float kFastDecayMs = 2.0f;     // learned sleep taken off per frame at least, when cut / when frames are slow
		constexpr float kSlowShare = 0.05f;      // frames slower than the GPU period by this share: fast decay
		constexpr float kSpikeShare = 1.5f;      // a GPU frame this many periods long is a spike
		constexpr int kSpikeSkipFrames = 2;      // frames whose wait is not learned after one (that frame and the next)
		HANDLE g_doneEvent = nullptr;
		bool g_guardOn = false;                  // the last PaceFrameStart's switch (for OnD3D11Submitted)
		int g_spikeSkip = 0;                     // frames still to skip
		bool g_skipFeed = false;                 // the last submitted frame's wait is not learned
		// Waiter results (under g_mutex): the longest single-frame completion gap since the main thread last looked (ms per
		// frame when several finished together), the last one, and the recent completions (QPC, index of the frame).
		float g_maxGapMs = -1.0f;
		float g_lastGapMs = -1.0f;
		constexpr size_t kDoneRing = 6;
		std::array<int64_t, kDoneRing> g_doneTimes{};
		std::array<uint64_t, kDoneRing> g_doneIndex{};
		size_t g_doneCount = 0, g_doneHead = 0;
		uint64_t g_completedFrames = 0;  // frames the waiter has seen finish (index of the newest)

		// (46p) "Smooth GPU Phase": 46o RT-off Solitude (204223 frames 400-430) had the predicted completion (t + pacer_pred_ms)
		// stepping by 22.5 +- 5.6 ms (sd) frame to frame while the GPU's own frame times (gpu_elapsed) varied by about 1 ms: the
		// single last completion time is a noisy phase (the wake-up of the waiter thread, the DLSS-G present path behind the
		// signalled value). The median of the last completions' offsets from a steady period gives the phase instead.
		constexpr double kPhaseClampShare = 0.5;  // the median offset is kept within half a period of the raw phase
		constexpr size_t kPhaseMinSamples = 4;

		float Median3(const std::array<float, 3>& a_v)
		{
			return std::max(std::min(a_v[0], a_v[1]), std::min(std::max(a_v[0], a_v[1]), a_v[2]));
		}

		/// (46o) After a pause, a loading screen or the map: the lead samples and everything learned start over.
		void ResetLearning()
		{
			g_leadCount = 0;
			g_leadHead = 0;
			g_correctionMs = 0.0f;
			g_stallFresh = false;
			g_lastStallMs = 0.0f;
			g_lastWaitMs = 0.0f;
			g_waitHist.fill(0.0f);
			g_backstopMs = 0.0f;
			g_lockRun = 0;
			g_periodCount = 0;
			g_periodMean = -1.0f;
			g_hiddenBaseCount = 0;
			g_hiddenBaseMs = 0.0f;
			g_spikeSkip = 0;  // (46p)
			g_skipFeed = false;
		}

		void CalibrateTsc(int64_t a_now)
		{
			if (g_tscPerMs > 0.0)
				return;
			const uint64_t tsc = __rdtsc();
			if (!g_tscStart) {
				g_tscStart = tsc;
				g_tscStartQpc = a_now;
				return;
			}
			const double ms = ToMs(a_now - g_tscStartQpc);
			if (ms >= 1000.0 && tsc > g_tscStart)
				g_tscPerMs = static_cast<double>(tsc - g_tscStart) / ms;
		}

		void UpdateGuard(const FrameInfo& a_info, int64_t a_now)
		{
			if (!a_info.enabled || !a_info.tracking) {
				g_windowStart = 0;
				return;
			}
			if (!g_windowStart) {
				g_windowStart = a_now;
				g_windowFrames = g_windowBehind = 0;
				g_windowSleepMs = g_windowStallMs = g_windowPredMs = g_windowLeadMs = 0.0;
			}
			++g_windowFrames;
			if (a_info.inFlight >= 1)
				++g_windowBehind;
			g_windowSleepMs += a_info.sleepMs + a_info.capWaitMs;
			g_windowStallMs += a_info.stallMs;
			g_windowPredMs += std::max(a_info.predictedMs, 0.0f);
			g_windowLeadMs += std::max(a_info.leadMs, 0.0f);
			if (ToMs(a_now - g_windowStart) < 2000.0)
				return;
			const double n = static_cast<double>(std::max(g_windowFrames, 1u));
			const bool behind = g_windowFrames >= 30 && static_cast<double>(g_windowBehind) >= 0.8 * n;
			if (behind && g_windowSleepMs / n < 0.2 && g_windowStallMs / n > 2.0 && (g_lastWarn == 0 || ToMs(a_now - g_lastWarn) > 30000.0)) {
				g_lastWarn = a_now;
				logger::warn("[LatencyPacer] Not pacing for 2 s while the GPU is behind: slept {:.2f} ms per frame, the CPU waited {:.1f} ms per frame inside "
							 "D3D11 (Map / Flush / GetData) instead; lead {:.1f} ms, predicted {:.1f} ms, learned frame-start sleep {:.1f} ms, {} frames",
					g_windowSleepMs / n, g_windowStallMs / n, g_windowLeadMs / n, g_windowPredMs / n, g_correctionMs, g_windowFrames);
			}
			g_windowStart = a_now;
			g_windowFrames = g_windowBehind = 0;
			g_windowSleepMs = g_windowStallMs = g_windowPredMs = g_windowLeadMs = 0.0;
		}

		float Quantile(const std::array<float, kRing>& a_ring, size_t a_count, float a_q)
		{
			std::array<float, kRing> v{};
			const size_t n = std::min(a_count, kRing);
			std::copy_n(a_ring.begin(), n, v.begin());
			const size_t k = std::min(n - 1, static_cast<size_t>(static_cast<float>(n - 1) * a_q + 0.5f));
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.begin() + static_cast<std::ptrdiff_t>(n));
			return v[k];
		}

		void WaiterThread()
		{
			HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!ev) {
				logger::warn("[LatencyPacer] CreateEvent failed; GPU completion tracking off");
				return;
			}
			int64_t prevDone = 0;
			for (;;) {
				uint64_t target = 0;
				winrt::com_ptr<ID3D12Fence> fence;
				{
					std::unique_lock lk(g_mutex);
					g_cv.wait_for(lk, std::chrono::milliseconds(250), [] { return !g_pending.empty(); });
					if (g_pending.empty() || !g_fence)
						continue;
					target = g_pending.front();
					fence = g_fence;
				}
				if (FAILED(fence->SetEventOnCompletion(target, ev)))
					continue;
				if (WaitForSingleObject(ev, 250) != WAIT_OBJECT_0)
					continue;
				const int64_t now = Now();
				const uint64_t completed = fence->GetCompletedValue();
				if (completed == UINT64_MAX) {
					logger::warn("[LatencyPacer] Interop fence reports device removal; GPU completion tracking stopped");
					std::scoped_lock lk(g_mutex);
					g_pending.clear();
					g_fence = nullptr;
					break;
				}
				std::scoped_lock lk(g_mutex);
				size_t popped = 0;
				while (!g_pending.empty() && g_pending.front() <= completed) {
					g_pending.pop_front();
					++popped;
				}
				if (popped == 0)
					continue;
				// Only single-frame steps are timed: when several finish together the wake-up time
				// says nothing about how long each of them took.
				if (popped == 1 && prevDone != 0) {
					const float ms = static_cast<float>(ToMs(now - prevDone));
					if (ms > 0.2f && ms < 250.0f) {
						g_intervals[g_intervalHead] = ms;
						g_intervalHead = (g_intervalHead + 1) % kRing;
						g_intervalCount = std::min(g_intervalCount + 1, kRing);
					}
				}
				// (46p) Spike detection (per frame when several finished together) and the recent completions for the phase.
				if (prevDone != 0) {
					const float gap = static_cast<float>(ToMs(now - prevDone)) / static_cast<float>(popped);
					g_lastGapMs = gap;
					g_maxGapMs = std::max(g_maxGapMs, gap);
				}
				g_completedFrames += popped;
				g_doneTimes[g_doneHead] = now;
				g_doneIndex[g_doneHead] = g_completedFrames;
				g_doneHead = (g_doneHead + 1) % kDoneRing;
				g_doneCount = std::min(g_doneCount + 1, kDoneRing);
				prevDone = now;
				g_lastDoneTime = now;
			}
			CloseHandle(ev);
		}

		void SleepUntil(int64_t a_target)
		{
			const double ms = ToMs(a_target - Now());
			if (ms <= 0.0)
				return;
			if (!g_timer)
				g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (g_timer) {
				LARGE_INTEGER due;
				due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);  // relative, 100 ns units
				if (SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
					WaitForSingleObject(g_timer, static_cast<DWORD>(ms) + 5);
					return;
				}
			}
			::Sleep(static_cast<DWORD>(ms));
		}

		/// (46p) Sleeps until a_target, or until a_value completes on a_fence plus a_slackTicks, whichever is first.
		/// Returns the QPC at which the value was seen complete (0 = it was not before a_target, or no fence wait was possible).
		int64_t SleepUntilOrDone(int64_t a_target, ID3D12Fence* a_fence, uint64_t a_value, int64_t a_slackTicks)
		{
			if (a_fence->GetCompletedValue() >= a_value) {
				const int64_t done = Now();
				SleepUntil(std::min(a_target, done + a_slackTicks));
				return done;
			}
			if (!g_doneEvent)
				g_doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (!g_timer)
				g_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!g_doneEvent || !g_timer || FAILED(a_fence->SetEventOnCompletion(a_value, g_doneEvent))) {
				SleepUntil(a_target);
				return 0;
			}
			for (;;) {
				const double ms = ToMs(a_target - Now());
				if (ms <= 0.0)
					return 0;
				LARGE_INTEGER due;
				due.QuadPart = -static_cast<LONGLONG>(ms * 10000.0);  // relative, 100 ns units
				if (!SetWaitableTimer(g_timer, &due, 0, nullptr, nullptr, FALSE)) {
					SleepUntil(a_target);
					return 0;
				}
				const HANDLE handles[2] = { g_doneEvent, g_timer };
				const DWORD r = WaitForMultipleObjects(2, handles, FALSE, static_cast<DWORD>(ms) + 5);
				if (r != WAIT_OBJECT_0)
					return 0;  // the timer (or the timeout): the planned end
				// The event is shared by every frame's registration: an older value may have set it. Ours is still armed then.
				if (a_fence->GetCompletedValue() < a_value)
					continue;
				const int64_t done = Now();
				CancelWaitableTimer(g_timer);
				SleepUntil(std::min(a_target, done + a_slackTicks));
				return done;
			}
		}

		/// (46p) Predicted completion of frame a_targetIndex from the median phase of the recent completions (a_times / a_index:
		/// oldest first, the last one is the newest), with a_periodTicks per frame. a_raw when there are too few.
		int64_t SmoothedCompletion(const int64_t* a_times, const uint64_t* a_index, size_t a_count, uint64_t a_targetIndex, double a_periodTicks, int64_t a_raw)
		{
			if (a_count < kPhaseMinSamples || a_periodTicks <= 0.0)
				return a_raw;
			const int64_t lastTime = a_times[a_count - 1];
			const uint64_t lastIndex = a_index[a_count - 1];
			std::array<double, kDoneRing> offsets{};
			size_t n = 0;
			for (size_t i = 0; i < a_count; ++i) {
				if (a_index[i] > lastIndex || lastIndex - a_index[i] > 2 * kDoneRing)
					continue;  // not from this run of completions
				offsets[n++] = static_cast<double>(a_times[i] - lastTime) + static_cast<double>(lastIndex - a_index[i]) * a_periodTicks;
			}
			if (n < kPhaseMinSamples)
				return a_raw;
			std::sort(offsets.begin(), offsets.begin() + static_cast<std::ptrdiff_t>(n));
			const double median = (n & 1) ? offsets[n / 2] : 0.5 * (offsets[n / 2 - 1] + offsets[n / 2]);
			const double clamp = kPhaseClampShare * a_periodTicks;
			const double shift = std::clamp(median, -clamp, clamp);
			const double frames = a_targetIndex >= lastIndex ? static_cast<double>(a_targetIndex - lastIndex) : 0.0;
			return lastTime + static_cast<int64_t>(shift + frames * a_periodTicks);
		}
	}

	void PaceFrameStart(const Options& a_options)
	{
		if (!g_qpf) {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			g_qpf = f.QuadPart;
		}
		const bool a_enabled = a_options.enabled;
		const float a_marginMs = a_options.marginMs;
		const bool a_stallFeedback = a_options.stallFeedback;
		const bool guard = a_options.noOverSleep;  // (46p)
		g_mainThread = GetCurrentThreadId();
		FrameInfo info{};
		info.enabled = a_enabled;
		g_hiddenOn = a_options.hiddenWaits;
		g_guardOn = guard;
		EngineTiming::SetPacerWaitTracking(a_options.hiddenWaits && a_enabled);

		const int64_t start = Now();
		CalibrateTsc(start);
		// (46o) After a pause / loading screen / the map, with either 46o switch on (off / off = 46n exactly). (46p) Or Never Over-Sleep
		// (its fast decay reads the frame period).
		if (a_options.hiddenWaits || a_options.backstop || guard) {
			if (a_options.paused) {
				g_wasPaused = true;
			} else if (g_wasPaused) {
				g_wasPaused = false;
				ResetLearning();
				info.reset = true;
			}
			if (g_prevStart) {
				const float p = static_cast<float>(ToMs(start - g_prevStart));
				if (p > 0.0f && p < 250.0f && !a_options.paused) {
					g_periods[g_periodHead] = p;
					g_periodHead = (g_periodHead + 1) % kPeriodRing;
					g_periodCount = std::min(g_periodCount + 1, kPeriodRing);
				} else {
					g_periodCount = 0;
				}
				float sum = 0.0f;
				for (size_t i = 0; i < g_periodCount; ++i)
					sum += g_periods[i];
				g_periodMean = g_periodCount >= 8 ? sum / static_cast<float>(g_periodCount) : -1.0f;
			}
		}
		g_prevStart = start;
		// (46n) Stall feedback: the last frame's wait inside D3D11 grows the learned frame-start sleep, a frame without one shrinks it.
		if (g_stallFresh) {
			g_stallFresh = false;
			// (46o) F1a: the median of the last three frames' waits (46n stall or hidden wait, the larger).
			const float wait = a_options.hiddenWaits ? Median3(g_waitHist) : g_lastStallMs;
			// (46p) Right after a GPU spike the wait is the spike's, not a steady one: not learned (the learned sleep decays).
			const bool skip = guard && g_skipFeed;
			info.spikeSkip = skip ? 1 : 0;
			if (a_stallFeedback && !skip && wait >= kStallThresholdMs)
				g_correctionMs = std::min(g_correctionMs + kStallGain * wait, kMaxCorrectionMs);
			else
				g_correctionMs = std::max(g_correctionMs - kStallDecayMs, 0.0f);
		}
		g_skipFeed = false;
		if (!a_stallFeedback)
			g_correctionMs = 0.0f;
		info.stallMs = g_lastStallMs;
		info.correctionMs = g_correctionMs;
		info.hiddenWaitMs = g_lastHiddenMs;
		info.waitUsedMs = g_lastWaitMs;
		info.hiddenBaseMs = g_hiddenBaseMs;
		int pending = 0;
		uint64_t secondNewest = 0;
		uint64_t newest = 0;  // (46p) the previous frame's value
		int64_t lastDone = 0;
		float period = -1.0f;
		winrt::com_ptr<ID3D12Fence> fence;
		// (46p) The recent completions, oldest first, and the index of the newest pending frame.
		std::array<int64_t, kDoneRing> doneTimes{};
		std::array<uint64_t, kDoneRing> doneIndex{};
		size_t doneCount = 0;
		uint64_t newestIndex = 0;
		{
			std::scoped_lock lk(g_mutex);
			info.tracking = g_fence != nullptr;
			pending = static_cast<int>(g_pending.size());
			if (pending >= 2)
				secondNewest = g_pending[g_pending.size() - 2];
			if (pending >= 1)
				newest = g_pending.back();
			lastDone = g_lastDoneTime;
			if (g_intervalCount >= kMinSamples)
				period = Quantile(g_intervals, g_intervalCount, 0.5f);
			fence = g_fence;
			info.doneIntervalMs = g_lastGapMs;
			doneCount = g_doneCount;
			for (size_t i = 0; i < doneCount; ++i) {
				const size_t slot = (g_doneHead + kDoneRing - doneCount + i) % kDoneRing;
				doneTimes[i] = g_doneTimes[slot];
				doneIndex[i] = g_doneIndex[slot];
			}
			newestIndex = g_completedFrames + static_cast<uint64_t>(pending);
		}
		// (46p) Fast decay: frames slower than the GPU period (16-frame mean) while sleep is learned.
		if (guard && g_correctionMs > 0.0f && period > 0.0f && g_periodMean > period * (1.0f + kSlowShare)) {
			const float cut = std::min(g_correctionMs, std::max(kFastDecayMs, 0.5f * g_correctionMs));
			g_correctionMs -= cut;
			info.decayMs += cut;
			info.correctionMs = g_correctionMs;
		}
		if (!info.tracking) {
			g_frameBehind = false;
			g_last = info;
			UpdateGuard(info, start);
			return;
		}
		info.inFlight = pending;
		info.gpuPeriodMs = period;
		g_frameBehind = pending >= 1;  // (46o) F1a counts this frame's hidden wait only then
		const float lead = g_leadCount >= kMinSamples ? Quantile(g_leads, g_leadCount, 0.75f) : -1.0f;
		info.leadMs = lead;
		// (46o) F1b: the unlock backstop (decided on the last frame's sleep and GPU state).
		float leadUsed = lead;
		if (a_options.backstop && a_enabled && lead >= 0.0f && !a_options.paused) {
			const bool locked = g_last.tracking && g_last.sleepMs < kLockSleepMs && g_last.inFlight >= 1 && period > 0.0f && g_periodMean > 0.0f &&
			                    std::abs(g_periodMean - period) <= kPeriodTolerance * period;
			g_lockRun = locked ? g_lockRun + 1 : 0;
			if (g_lockRun == kLockFrames)
				g_backstopRef = period;
			const bool slower = g_backstopMs > 0.0f && g_backstopRef > 0.0f && g_periodMean > g_backstopRef * (1.0f + kPeriodTolerance);
			if (slower) {
				g_lockRun = 0;
				g_backstopMs = std::max(g_backstopMs - kBackstopFastReturnMs, 0.0f);
			} else if (g_lockRun >= kLockFrames) {
				g_backstopMs = std::min(g_backstopMs + kBackstopStepMs, kBackstopMaxMs);
			} else {
				g_backstopMs = std::max(g_backstopMs - kBackstopReturnMs, 0.0f);
			}
			const float floor = std::max(Quantile(g_leads, g_leadCount, 0.25f), kBackstopFloorMs);
			g_backstopMs = std::min(g_backstopMs, std::max(lead - floor, 0.0f));
			leadUsed = lead - g_backstopMs;
		} else {
			g_backstopMs = 0.0f;
			g_lockRun = 0;
		}
		info.backstopMs = g_backstopMs;

		if (pending > 0 && lastDone != 0 && period > 0.0f) {
			// The previous frame finishes about `pending` GPU periods after the last completion.
			const double periodTicks = static_cast<double>(period) * static_cast<double>(g_qpf) / 1000.0;
			const int64_t raw = lastDone + static_cast<int64_t>(periodTicks * pending);
			// (46p) The same from the median phase of the recent completions (shown always; used with Smooth GPU Phase).
			const int64_t smoothed = doneCount > 0 && doneTimes[doneCount - 1] == lastDone ?
			                             SmoothedCompletion(doneTimes.data(), doneIndex.data(), doneCount, newestIndex, periodTicks, raw) :
			                             raw;
			info.phaseShiftMs = static_cast<float>(ToMs(smoothed - raw));
			const int64_t predicted = a_options.smoothPhase ? smoothed : raw;
			info.predictedMs = static_cast<float>(std::max(0.0, ToMs(predicted - start)));
			if (a_enabled && lead >= 0.0f) {
				const double reserveMs = static_cast<double>(leadUsed) + static_cast<double>(a_marginMs);
				// (46n) Plus the learned wait (only while the GPU is behind: pending > 0 here).
				const double baseMs = std::max(ToMs(predicted - start) - reserveMs, 0.0);
				const double sleepMs = std::min(baseMs + static_cast<double>(g_correctionMs), kMaxSleepMs);
				if (sleepMs > 0.2) {
					const int64_t target = start + static_cast<int64_t>(sleepMs * static_cast<double>(g_qpf) / 1000.0);
					if (guard && fence && newest != 0) {
						// (46p) Never past the previous frame's completion (+ slack): the frame after it only idles the GPU.
						const double slackMs = std::clamp(static_cast<double>(period) * kSlackShare, static_cast<double>(kSlackMinMs), static_cast<double>(kSlackMaxMs));
						const int64_t done = SleepUntilOrDone(target, fence.get(), newest, static_cast<int64_t>(slackMs * static_cast<double>(g_qpf) / 1000.0));
						info.sleepMs = static_cast<float>(ToMs(Now() - start));
						const double cut = sleepMs - static_cast<double>(info.sleepMs);
						if (done != 0 && cut >= kCutMinMs) {
							info.overSleepCutMs = static_cast<float>(cut);
							// The learned part that was of use; the rest goes fast.
							if (g_correctionMs > 0.0f) {
								const float used = static_cast<float>(std::max(static_cast<double>(info.sleepMs) - baseMs, 0.0));
								const float next = std::max(std::min(std::max(used, 0.5f * g_correctionMs), g_correctionMs - kFastDecayMs), 0.0f);
								info.decayMs += g_correctionMs - next;
								g_correctionMs = next;
							}
						}
					} else {
						SleepUntil(target);
						info.sleepMs = static_cast<float>(ToMs(Now() - start));
					}
				}
			}
		} else if (pending == 0) {
			info.predictedMs = 0.0f;
		}

		// Hard bound: the frame before the previous one must be finished before this one starts,
		// so at most one frame of CPU work is ever queued ahead of the GPU. While the GPU keeps up
		// this costs nothing (the previous frame is still queued for it).
		if (a_enabled && !g_capDisabled && secondNewest != 0 && fence && fence->GetCompletedValue() < secondNewest) {
			if (!g_capEvent)
				g_capEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (g_capEvent && SUCCEEDED(fence->SetEventOnCompletion(secondNewest, g_capEvent))) {
				const int64_t t0 = Now();
				const DWORD r = WaitForSingleObject(g_capEvent, kCapTimeoutMs);
				info.capWaitMs = static_cast<float>(ToMs(Now() - t0));
				// A GPU that never gets there (work not flushed, a hang) must not turn into a
				// 50 ms stall on every frame: three timeouts in a row switch the bound off.
				g_capTimeouts = r == WAIT_OBJECT_0 ? 0 : g_capTimeouts + 1;
				if (g_capTimeouts >= 3) {
					g_capDisabled = true;
					logger::warn("[LatencyPacer] Hard frame bound timed out 3 times in a row; switched off for this session");
				}
			}
		}
		// (45n) In flight once the frame really starts. inFlight is taken before the sleep: with the sleep sized
		// to end as the previous frame finishes, "2 before the sleep" is the intended state, not a queue.
		if (fence) {
			const uint64_t completed = fence->GetCompletedValue();
			std::scoped_lock lk(g_mutex);
			int after = 0;
			for (const uint64_t value : g_pending)
				after += value > completed ? 1 : 0;
			info.inFlightAfter = after;
		}
		g_last = info;
		UpdateGuard(info, Now());
	}

	void AddD3D11WaitTsc(uint64_t a_ticks)
	{
		if (g_tscPerMs > 0.0)
			AddD3D11WaitMs(static_cast<double>(a_ticks) / g_tscPerMs);
	}

	void AddD3D11WaitMs(double a_ms)
	{
		if (a_ms < kStallCallMs || g_mainThread == 0 || GetCurrentThreadId() != g_mainThread)
			return;
		g_stallMs += a_ms;
	}

	void OnSimulationStart()
	{
		g_simStart = Now();
		g_stallMs = 0.0;  // (46n)
	}

	void OnD3D11Submitted()
	{
		if (!g_simStart || !g_qpf)
			return;
		// (46n) The waits inside D3D11 are not CPU work the pacer has to keep in hand.
		const float stall = static_cast<float>(g_stallMs);
		// (46o) F1a: the render segments' wait so far (measured whenever it is tracked: the F7 trace shows it with F1a off too).
		const double hidden = EngineTiming::RenderWaitMsSoFar();
		float wait = stall;
		if (hidden >= 0.0 && g_last.sleepMs >= kBaseSleepMs) {
			g_hiddenBase[g_hiddenBaseHead] = static_cast<float>(hidden);
			g_hiddenBaseHead = (g_hiddenBaseHead + 1) % kRing;
			g_hiddenBaseCount = std::min(g_hiddenBaseCount + 1, kRing);
			g_hiddenBaseMs = g_hiddenBaseCount >= kMinSamples ? Quantile(g_hiddenBase, g_hiddenBaseCount, 0.25f) : 0.0f;
		}
		const double extra = hidden - static_cast<double>(g_hiddenBaseMs);
		if (g_hiddenOn && hidden >= 0.0 && extra >= kHiddenMinMs && g_frameBehind)
			wait = std::max(wait, static_cast<float>(extra));
		g_lastHiddenMs = static_cast<float>(hidden);
		g_lastWaitMs = wait;
		// (46p) Never Over-Sleep: a GPU frame of over 1.5 periods finished since the last submit, or the GPU is in one right now
		// (the last completion that long ago with frames still pending): this frame's wait and the next one's are not learned.
		bool skipFeed = false;
		if (g_guardOn) {
			const float period = g_last.gpuPeriodMs;
			float maxGap = -1.0f;
			int64_t lastDone = 0;
			size_t pendingNow = 0;
			{
				std::scoped_lock lk(g_mutex);
				maxGap = g_maxGapMs;
				g_maxGapMs = -1.0f;
				lastDone = g_lastDoneTime;
				pendingNow = g_pending.size();
			}
			if (period > 0.0f) {
				const bool spikeDone = maxGap > kSpikeShare * period;
				const bool spikeNow = pendingNow > 0 && lastDone != 0 && ToMs(Now() - lastDone) > static_cast<double>(kSpikeShare * period);
				if (spikeDone || spikeNow)
					g_spikeSkip = kSpikeSkipFrames;
			}
			if (g_spikeSkip > 0) {
				--g_spikeSkip;
				skipFeed = true;
			}
		}
		g_skipFeed = skipFeed;
		if (!skipFeed) {
			g_waitHist[g_waitHistHead] = wait;
			g_waitHistHead = (g_waitHistHead + 1) % g_waitHist.size();
		}
		g_lastStallMs = stall;
		g_stallFresh = true;
		g_stallMs = 0.0;
		const float ms = static_cast<float>(ToMs(Now() - g_simStart)) - wait;
		g_simStart = 0;
		if (ms <= 0.0f || ms > 250.0f)
			return;
		g_leads[g_leadHead] = ms;
		g_leadHead = (g_leadHead + 1) % kRing;
		g_leadCount = std::min(g_leadCount + 1, kRing);
	}

	void OnFrameEndSignaled(ID3D12Fence* a_fence, uint64_t a_value)
	{
		if (!a_fence)
			return;
		{
			std::scoped_lock lk(g_mutex);
			if (g_fence.get() != a_fence) {
				g_fence.copy_from(a_fence);
				g_pending.clear();
				g_intervalCount = 0;
				g_lastDoneTime = 0;
				g_doneCount = 0;  // (46p)
				g_maxGapMs = g_lastGapMs = -1.0f;
			}
			// Bounded: if the waiter ever stalls, old entries are dropped rather than piling up.
			if (g_pending.size() >= 16)
				g_pending.pop_front();
			g_pending.push_back(a_value);
			if (!g_threadStarted) {
				g_threadStarted = true;
				std::thread(WaiterThread).detach();
				logger::info("[LatencyPacer] Following GPU frame completion on the interop fence");
			}
		}
		g_cv.notify_one();
	}

	const FrameInfo& Last() { return g_last; }
}
