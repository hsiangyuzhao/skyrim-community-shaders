#include "Utils/LatencyPacer.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <d3d12.h>

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
	}

	void PaceFrameStart(bool a_enabled, float a_marginMs)
	{
		if (!g_qpf) {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			g_qpf = f.QuadPart;
		}
		FrameInfo info{};
		info.enabled = a_enabled;

		const int64_t start = Now();
		int pending = 0;
		uint64_t secondNewest = 0;
		int64_t lastDone = 0;
		float period = -1.0f;
		winrt::com_ptr<ID3D12Fence> fence;
		{
			std::scoped_lock lk(g_mutex);
			info.tracking = g_fence != nullptr;
			pending = static_cast<int>(g_pending.size());
			if (pending >= 2)
				secondNewest = g_pending[g_pending.size() - 2];
			lastDone = g_lastDoneTime;
			if (g_intervalCount >= kMinSamples)
				period = Quantile(g_intervals, g_intervalCount, 0.5f);
			fence = g_fence;
		}
		if (!info.tracking) {
			g_last = info;
			return;
		}
		info.inFlight = pending;
		info.gpuPeriodMs = period;
		const float lead = g_leadCount >= kMinSamples ? Quantile(g_leads, g_leadCount, 0.75f) : -1.0f;
		info.leadMs = lead;

		if (pending > 0 && lastDone != 0 && period > 0.0f) {
			// The previous frame finishes about `pending` GPU periods after the last completion.
			const int64_t predicted = lastDone + static_cast<int64_t>(static_cast<double>(period) * pending * static_cast<double>(g_qpf) / 1000.0);
			info.predictedMs = static_cast<float>(std::max(0.0, ToMs(predicted - start)));
			if (a_enabled && lead >= 0.0f) {
				const double reserveMs = static_cast<double>(lead) + static_cast<double>(a_marginMs);
				const double sleepMs = std::min(ToMs(predicted - start) - reserveMs, kMaxSleepMs);
				if (sleepMs > 0.2) {
					SleepUntil(start + static_cast<int64_t>(sleepMs * static_cast<double>(g_qpf) / 1000.0));
					info.sleepMs = static_cast<float>(ToMs(Now() - start));
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
	}

	void OnSimulationStart()
	{
		g_simStart = Now();
	}

	void OnD3D11Submitted()
	{
		if (!g_simStart || !g_qpf)
			return;
		const float ms = static_cast<float>(ToMs(Now() - g_simStart));
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
