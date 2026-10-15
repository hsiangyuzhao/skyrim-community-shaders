#include "Utils/EngineTiming.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <thread>

#include <psapi.h>

#include <DirectXTex.h>  // (46p) BitsPerPixel / IsCompressed
#include <intrin.h>      // (46p) _ReturnAddress
#include <tuple>

#include "Features/PerformanceOverlay.h"
#include "Features/Upscaling.h"
#include "Utils/FileSystem.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuPhaseTimeline.h"
#include "Utils/WorldTiming.h"

namespace Util::EngineTiming
{
	namespace
	{
		// Every direct (E8) call in Main::Update, SkyrimSE.exe 1.5.97 (ID 35565, 0x1405B2FF0, 0x764
		// bytes): offset of the call instruction, and the Address Library ID of the function it calls.
		// Read off the binary with dumpbin for 44f; the IDs come from the Address Library database.
		struct Site
		{
			uint32_t offset;
			uint32_t calleeId;
		};
		constexpr Site kSites[] = {
			{ 0x01E, 68163 }, { 0x053, 39366 }, { 0x058, 35605 }, { 0x064, 15159 }, { 0x073, 76071 }, { 0x07F, 34862 },
			{ 0x090, 76072 }, { 0x095, 101821 }, { 0x09D, 101831 }, { 0x0A2, 68506 }, { 0x0A9, 38080 }, { 0x0FE, 79937 },
			{ 0x120, 79937 }, { 0x142, 79937 }, { 0x164, 79937 }, { 0x186, 79937 }, { 0x1A4, 79937 }, { 0x1C9, 35208 },
			{ 0x1F6, 79937 }, { 0x1FF, 35208 }, { 0x206, 35207 }, { 0x24D, 35402 }, { 0x25F, 35318 }, { 0x281, 34555 },
			{ 0x286, 34553 }, { 0x28E, 35578 }, { 0x2BB, 26497 }, { 0x2CC, 68167 }, { 0x2D8, 42911 }, { 0x2E4, 42778 },
			{ 0x2F0, 33909 }, { 0x30A, 79937 }, { 0x31A, 35581 }, { 0x344, 18976 }, { 0x366, 35574 }, { 0x37D, 31719 },
			{ 0x38C, 53115 }, { 0x39B, 53116 }, { 0x3A0, 16188 }, { 0x3AC, 56482 }, { 0x3C0, 67315 }, { 0x3C8, 35566 },
			{ 0x3D6, 45570 }, { 0x3DE, 35572 }, { 0x3EF, 16173 }, { 0x403, 40377 }, { 0x418, 35501 }, { 0x41D, 69181 },
			{ 0x43E, 52050 }, { 0x446, 35569 }, { 0x44E, 35568 }, { 0x469, 39637 }, { 0x47B, 13148 }, { 0x485, 68165 },
			{ 0x48F, 68165 }, { 0x499, 68165 }, { 0x4A5, 15033 }, { 0x4AF, 68165 }, { 0x4D0, 100403 }, { 0x4EF, 100409 },
			{ 0x4F9, 34551 }, { 0x509, 68167 }, { 0x53C, 40376 }, { 0x548, 42914 }, { 0x554, 13152 }, { 0x55C, 35571 },
			{ 0x561, 19437 }, { 0x56D, 90944 }, { 0x57B, 51751 }, { 0x580, 101995 }, { 0x592, 51750 }, { 0x599, 51260 },
			{ 0x5B3, 35589 }, { 0x5BA, 34551 }, { 0x5CA, 79945 }, { 0x5D2, 35556 }, { 0x5E3, 35574 }, { 0x5E8, 34554 },
			{ 0x604, 66862 }, { 0x612, 66841 }, { 0x61A, 66889 }, { 0x62B, 79946 }, { 0x630, 52063 }, { 0x638, 52065 },
			{ 0x644, 39572 }, { 0x66D, 87936 }, { 0x672, 34303 }, { 0x677, 32314 }, { 0x681, 35570 }, { 0x690, 53118 },
			{ 0x69A, 38082 }, { 0x6A0, 21434 }, { 0x6AC, 35914 }, { 0x6B8, 35916 }, { 0x6C4, 35912 }, { 0x6CF, 38083 },
			{ 0x6E9, 79937 }, { 0x707, 26407 }, { 0x723, 66862 }, { 0x731, 66841 }, { 0x739, 66890 }, { 0x73E, 21438 },
			{ 0x743, 21400 }, { 0x748, 76765 }, { 0x754, 100571 },
		};
		constexpr int kSiteCount = static_cast<int>(std::size(kSites));
		constexpr uint32_t kMainUpdateId = 35565;
		constexpr uint32_t kRenderOffset = 0x5D2;  // call 35556: the frame render
		constexpr uint32_t kInputOffset = 0x3C0;   // call 67315: BSInputDeviceManager::PollInputDevices
		constexpr int kSegCount = static_cast<int>(Seg::Count);

		const char* g_probeStatus = "not installed yet";
		std::string g_rejectReason;
		int g_renderIndex = -1;
		int g_inputIndex = -1;
		bool g_probes = false;

		DWORD g_mainThread = 0;
		double g_toMs = 0.0;

		// Attribution state: the segment (and game-update call) the main thread is in right now.
		int g_seg = static_cast<int>(Seg::Outer);
		int g_call = -1;
		int64_t g_last = 0;
		std::array<int64_t, kSegCount> g_acc{};
		std::array<int64_t, kSiteCount> g_callAcc{};

		struct StackEntry
		{
			int scope;  ///< GpuScope as int, or -1 for a ScopedSeg
			int seg;    ///< segment to return to
			int call;   ///< game-update call to return to
		};
		constexpr int kMaxDepth = 32;
		std::array<StackEntry, kMaxDepth> g_stack{};
		int g_depth = 0;
		int g_overflow = 0;

		// (44f, A) Reflex markers.
		bool g_frameToken = false;
		bool g_renderMarked = false;

		// Device Create* calls.
		int64_t g_createTicks = 0;
		uint32_t g_creates = 0;
		std::atomic<int64_t> g_createOtherTicks{ 0 };
		std::atomic<uint32_t> g_createsOther{ 0 };

		bool OnMainThread() { return g_mainThread != 0 && GetCurrentThreadId() == g_mainThread; }

		// ---- (46o) M1: main-thread waits per segment (wall - thread CPU time) ----
		bool g_waitPacer = false;                        // the pacer counts hidden GPU waits (F1a)
		std::atomic<double> g_cyclesPerMs{ 0.0 };        // thread cycles per ms (calibrated on a background thread)
		std::atomic<double> g_cycleCostNs{ -1.0 };       // one QueryThreadCycleTime call
		bool g_calibrationStarted = false;
		uint64_t g_cycLast = 0;                          // thread cycles at the last sample
		int64_t g_waitQpcLast = 0;                       // QPC at the last sample (0 = no open interval)
		std::array<uint64_t, kSegCount> g_cycAcc{};      // thread cycles per segment this frame
		std::array<int64_t, kSegCount> g_waitWallAcc{};  // QPC ticks per segment this frame (same intervals as g_cycAcc)
		bool g_waitTouched = false;                      // something was sampled this frame

		bool WaitTrackingOn() { return g_probes && g_cyclesPerMs.load(std::memory_order_relaxed) > 0.0 && (g_waitPacer || FrameTrace::IsRecording()); }

		uint64_t ThreadCycles()
		{
			ULONG64 cycles = 0;
			QueryThreadCycleTime(GetCurrentThread(), &cycles);
			return cycles;
		}

		/// Bills the main thread's wall and CPU time since the last sample to a_seg.
		void SampleWait(int a_seg)
		{
			const uint64_t cyc = ThreadCycles();
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			if (g_waitQpcLast && cyc >= g_cycLast && t.QuadPart >= g_waitQpcLast) {
				g_cycAcc[a_seg] += cyc - g_cycLast;
				g_waitWallAcc[a_seg] += t.QuadPart - g_waitQpcLast;
				g_waitTouched = true;
			}
			g_cycLast = cyc;
			g_waitQpcLast = t.QuadPart;
		}

		/// Once, on a background thread: thread cycles per ms (spins 3 x 25 ms, the largest ratio: preemption only lowers it),
		/// and the cost of one QueryThreadCycleTime call.
		void CalibrateCycles()
		{
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			const double qpcPerMs = static_cast<double>(f.QuadPart) / 1000.0;
			double best = 0.0;
			for (int i = 0; i < 3; ++i) {
				LARGE_INTEGER t0, t1;
				QueryPerformanceCounter(&t0);
				const uint64_t c0 = ThreadCycles();
				do {
					QueryPerformanceCounter(&t1);
				} while (static_cast<double>(t1.QuadPart - t0.QuadPart) < 25.0 * qpcPerMs);
				const uint64_t c1 = ThreadCycles();
				QueryPerformanceCounter(&t1);
				const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) / qpcPerMs;
				if (ms > 0.0 && c1 > c0)
					best = std::max(best, static_cast<double>(c1 - c0) / ms);
			}
			constexpr int kCalls = 20000;
			LARGE_INTEGER a, b;
			QueryPerformanceCounter(&a);
			uint64_t sink = 0;
			for (int i = 0; i < kCalls; ++i)
				sink += ThreadCycles();
			QueryPerformanceCounter(&b);
			const double costNs = static_cast<double>(b.QuadPart - a.QuadPart) / qpcPerMs * 1e6 / kCalls;
			// The TSC rate, for the log: QueryThreadCycleTime counts TSC ticks on current CPUs.
			const uint64_t r0 = __rdtsc();
			LARGE_INTEGER q0, q1;
			QueryPerformanceCounter(&q0);
			do {
				QueryPerformanceCounter(&q1);
			} while (static_cast<double>(q1.QuadPart - q0.QuadPart) < 10.0 * qpcPerMs);
			const double tscPerMs = static_cast<double>(__rdtsc() - r0) / (static_cast<double>(q1.QuadPart - q0.QuadPart) / qpcPerMs);
			g_cycleCostNs.store(costNs, std::memory_order_relaxed);
			if (best > 1000.0)
				g_cyclesPerMs.store(best, std::memory_order_relaxed);
			logger::info("[EngineTiming] (46o) Thread CPU time: {:.0f} thread cycles per ms (TSC {:.0f} per ms), one QueryThreadCycleTime {:.0f} ns{}{}",
				best, tscPerMs, costNs, best > 1000.0 ? "" : "; calibration FAILED, segment waits off", sink == 0 ? " (no cycles)" : "");
		}

		// ---- (46o) M2: stall sampler ----
		constexpr int kStackWords = 64;
		constexpr int kMaxModules = 512;
		constexpr int kSamplesPerFrame = 3;
		constexpr double kStallFirstMs = 8.0;
		constexpr double kStallNextMs = 4.0;
		std::atomic<bool> g_samplerOn{ false };
		std::atomic<bool> g_samplerThread{ false };
		std::atomic<int64_t> g_segSince{ 0 };   // QPC of the last segment / call change (main thread, while the sampler is on)
		std::atomic<int> g_segNow{ 0 };         // the segment then
		std::atomic<uint64_t> g_frameNow{ 0 };  // the frame then
		HANDLE g_mainHandle = nullptr;
		uintptr_t g_mainStackBase = 0;
		struct ModuleRange
		{
			uintptr_t base, end;
			char name[48];
		};
		ModuleRange g_modules[kMaxModules]{};
		std::atomic<int> g_moduleCount{ 0 };
		std::mutex g_sampleMutex;  // never taken while the main thread is suspended
		std::string g_samples;     // taken by the main thread every frame

		/// Main thread, while the sampler is off (the sampler thread reads the table only while it is on).
		void RefreshModules()
		{
			static HMODULE handles[kMaxModules];
			DWORD needed = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), handles, sizeof(handles), &needed))
				return;
			const int n = std::min(static_cast<int>(needed / sizeof(HMODULE)), kMaxModules);
			int count = 0;
			for (int i = 0; i < n; ++i) {
				MODULEINFO info{};
				if (!K32GetModuleInformation(GetCurrentProcess(), handles[i], &info, sizeof(info)))
					continue;
				auto& m = g_modules[count++];
				m.base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
				m.end = m.base + info.SizeOfImage;
				char path[MAX_PATH]{};
				K32GetModuleBaseNameA(GetCurrentProcess(), handles[i], path, MAX_PATH);
				strncpy_s(m.name, path, _TRUNCATE);
				for (char& c : m.name)
					if (c == ',' || c == '|' || c == '>' || c == ';' || c == '@' || c == ':')
						c = '_';
			}
			g_moduleCount.store(count, std::memory_order_release);
		}

		const ModuleRange* FindModule(uintptr_t a_address)
		{
			const int n = g_moduleCount.load(std::memory_order_acquire);
			for (int i = 0; i < n; ++i)
				if (a_address >= g_modules[i].base && a_address < g_modules[i].end)
					return &g_modules[i];
			return nullptr;
		}

		/// A stack word that is a return address: inside a module, right after a call (E8 rel32, FF 15 [rip], FF /2 reg or [reg]).
		/// Read with ReadProcessMemory: a bad guess fails instead of faulting.
		bool LooksLikeReturn(uintptr_t a_address)
		{
			if (a_address < 8 || !FindModule(a_address))
				return false;
			uint8_t b[8]{};
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a_address - 8), b, sizeof(b), &got) || got != sizeof(b))
				return false;
			if (b[3] == 0xE8)  // call rel32
				return true;
			if (b[2] == 0xFF && (b[3] & 0x38) == 0x10 && ((b[3] & 0xC7) == 0x05 || (b[3] & 0xC0) == 0x80))  // call [rip+d32] / [reg+d32]
				return true;
			if (b[5] == 0xFF && (b[6] & 0x38) == 0x10 && (b[6] & 0xC0) == 0x40)  // call [reg+d8]
				return true;
			if (b[6] == 0xFF && (b[7] & 0x38) == 0x10 && ((b[7] & 0xC0) == 0xC0 || (b[7] & 0xC0) == 0x00))  // call reg / [reg]
				return true;
			return false;
		}

		std::string DescribeCode(uintptr_t a_address)
		{
			if (const auto* m = FindModule(a_address))
				return std::format("{}+{:X}", m->name, a_address - m->base);
			return std::format("{:X}", a_address);
		}

		void SamplerThread()
		{
			// Highest priority: the time between SuspendThread and ResumeThread must not include a preemption of this thread.
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
			HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			const double qpcPerMs = static_cast<double>(f.QuadPart) / 1000.0;
			alignas(16) static CONTEXT ctx;
			static uint64_t stack[kStackWords];
			uint64_t frame = UINT64_MAX;
			int samplesThisFrame = 0;
			int64_t sampledSince = 0;
			int samplesThisInterval = 0;
			for (;;) {
				if (!g_samplerOn.load(std::memory_order_acquire) || !g_mainHandle) {
					Sleep(50);
					continue;
				}
				LARGE_INTEGER due;
				due.QuadPart = -10000;  // 1 ms
				if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
					WaitForSingleObject(timer, 5);
				else
					Sleep(1);
				const int64_t since = g_segSince.load(std::memory_order_acquire);
				const uint64_t frameNow = g_frameNow.load(std::memory_order_relaxed);
				if (!since || !g_samplerOn.load(std::memory_order_acquire))
					continue;
				if (frameNow != frame) {
					frame = frameNow;
					samplesThisFrame = 0;
				}
				if (since != sampledSince) {
					sampledSince = since;
					samplesThisInterval = 0;
				}
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				const double inSeg = static_cast<double>(now.QuadPart - since) / qpcPerMs;
				if (samplesThisFrame >= kSamplesPerFrame || inSeg < kStallFirstMs + kStallNextMs * samplesThisInterval || inSeg > 2000.0)
					continue;
				const int seg = g_segNow.load(std::memory_order_relaxed);

				// ---- suspended window: syscalls only (no allocation, no lock, no logging); ResumeThread on every path ----
				ctx.ContextFlags = CONTEXT_CONTROL;
				SIZE_T got = 0;
				bool ok = false;
				if (SuspendThread(g_mainHandle) != static_cast<DWORD>(-1)) {
					ok = GetThreadContext(g_mainHandle, &ctx) != FALSE;  // also waits until the thread is really suspended
					if (ok && ctx.Rsp && g_mainStackBase > ctx.Rsp) {
						const SIZE_T bytes = std::min<SIZE_T>(sizeof(stack), static_cast<SIZE_T>(g_mainStackBase - ctx.Rsp)) & ~static_cast<SIZE_T>(7);
						if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(ctx.Rsp), stack, bytes, &got))
							got = 0;
					}
					ResumeThread(g_mainHandle);
				}
				// ---- resumed ----
				if (!ok)
					continue;
				++samplesThisFrame;
				++samplesThisInterval;
				std::string text = std::format("{}@{:.0f}:{}", SegName(static_cast<Seg>(seg)), inSeg, DescribeCode(static_cast<uintptr_t>(ctx.Rip)));
				int found = 0;
				for (SIZE_T i = 0; i < got / 8 && found < 6; ++i) {
					if (LooksLikeReturn(static_cast<uintptr_t>(stack[i]))) {
						text += ">" + DescribeCode(static_cast<uintptr_t>(stack[i]));
						++found;
					}
				}
				std::scoped_lock lk(g_sampleMutex);
				if (g_samples.size() < 4096)
					g_samples += (g_samples.empty() ? "" : "|") + text;
			}
		}

		// ---- (46n) cs_passes split per pass ----
		constexpr int kOurPasses = static_cast<int>(Seg::OurPasses);
		constexpr int kPassBlocks = static_cast<int>(PassBlock::Count);
		constexpr int kMaxPassKeys = 192;
		constexpr int kMaxPassDepth = 32;
		constexpr int kMaxGpuBuckets = 128;
		constexpr int kMaxScopes = 64;

		struct PassKey
		{
			std::string name;            // assigned once, never changed: name.data() stays valid
			const char* lastPtr = nullptr;  // fast path: the caller's key pointer seen last
			size_t lastLen = 0;
			int gpuBucket = -1;
			bool untimed = false;
			// this frame
			int64_t ticks = 0;
			uint32_t calls = 0;
			double blockMs[kPassBlocks]{};
			uint32_t blockCalls[kPassBlocks]{};
			bool touched = false;
		};
		std::array<PassKey, kMaxPassKeys> g_passKeys{};
		int g_passKeyCount = 0;
		std::array<int, kMaxPassKeys> g_passTouched{};
		int g_passTouchedCount = 0;
		std::array<int, kMaxGpuBuckets> g_gpuPassKey = [] {
			std::array<int, kMaxGpuBuckets> a{};
			a.fill(-1);
			return a;
		}();
		std::array<int, kMaxScopes + 1> g_untimedKey = [] {
			std::array<int, kMaxScopes + 1> a{};
			a.fill(-1);
			return a;
		}();

		struct PassEntry
		{
			int key;
			int engineDepth;  // g_depth when opened: a pop of the engine scope below it closes it
		};
		std::array<PassEntry, kMaxPassDepth> g_passStack{};
		int g_passDepth = 0;
		uint64_t g_frameId = 0;

		int AddPassKey(std::string a_name, int a_gpuBucket, bool a_untimed)
		{
			if (g_passKeyCount >= kMaxPassKeys)
				return -1;
			auto& k = g_passKeys[g_passKeyCount];
			k.name = std::move(a_name);
			k.gpuBucket = a_gpuBucket;
			k.untimed = a_untimed;
			return g_passKeyCount++;
		}

		int InternPassKey(std::string_view a_key)
		{
			for (int i = 0; i < g_passKeyCount; ++i) {
				auto& k = g_passKeys[i];
				if (k.lastPtr == a_key.data() && k.lastLen == a_key.size() && k.gpuBucket < 0 && !k.untimed)
					return i;
			}
			for (int i = 0; i < g_passKeyCount; ++i) {
				auto& k = g_passKeys[i];
				if (k.gpuBucket < 0 && !k.untimed && k.name == a_key) {
					k.lastPtr = a_key.data();
					k.lastLen = a_key.size();
					return i;
				}
			}
			const int i = AddPassKey(std::string(a_key), -1, false);
			if (i >= 0) {
				g_passKeys[i].lastPtr = a_key.data();
				g_passKeys[i].lastLen = a_key.size();
			}
			return i;
		}

		const char* ScopeName(int a_scope)
		{
			switch (static_cast<GpuScope>(a_scope)) {
			case GpuScope::CsPasses:
				return "untimed.cs_passes";
			case GpuScope::CsOther:
				return "untimed.cs_other";
			case GpuScope::CsUpscaling:
				return "untimed.cs_upscaling";
			default:
				return nullptr;
			}
		}

		/// The pass billed right now: the innermost open pass, else "untimed.<innermost engine scope>".
		int CurrentPassKey();

		void MarkTouched(int a_key)
		{
			auto& k = g_passKeys[a_key];
			if (!k.touched) {
				k.touched = true;
				g_passTouched[g_passTouchedCount++] = a_key;
			}
		}

		void SwitchTo(int a_seg, int a_call)
		{
			// (46l) The world-render split runs exactly while the main thread is in the world segment.
			constexpr int kWorld = static_cast<int>(Seg::World);
			if ((g_seg == kWorld) != (a_seg == kWorld))
				WorldTiming::OnWorldSegment(a_seg == kWorld);
			// (46o) M1: the thread's CPU time at segment changes only (not at the call switches inside logic).
			if (a_seg != g_seg) {
				if (WaitTrackingOn())
					SampleWait(g_seg);
				else
					g_waitQpcLast = 0;
				// (46p) A GPU timestamp at the change (recording only).
				if (FrameTrace::IsRecording())
					FrameTrace::OnEngineSegment(a_seg);
			}
			// (46o) M2: where the main thread is now, for the stall sampler.
			if ((a_seg != g_seg || a_call != g_call) && g_samplerOn.load(std::memory_order_relaxed)) {
				LARGE_INTEGER t;
				QueryPerformanceCounter(&t);
				g_segNow.store(a_seg, std::memory_order_relaxed);
				g_segSince.store(t.QuadPart, std::memory_order_release);
			}
			if (FrameTrace::Active()) {
				const int64_t now = FrameTrace::Now();
				if (g_last) {
					const int64_t d = now - g_last;
					g_acc[g_seg] += d;
					if (g_call >= 0)
						g_callAcc[g_call] += d;
					// (46n) cs_passes time goes to the pass open now.
					if (g_seg == kOurPasses) {
						const int key = CurrentPassKey();
						if (key >= 0) {
							g_passKeys[key].ticks += d;
							MarkTouched(key);
						}
					}
				}
				g_last = now;
			} else {
				g_last = 0;
			}
			g_seg = a_seg;
			g_call = a_call;
		}

		int CurrentPassKey()
		{
			if (g_passDepth > 0)
				return g_passStack[g_passDepth - 1].key;
			int scope = -1;
			for (int i = g_depth - 1; i >= 0; --i) {
				if (g_stack[i].scope >= 0) {
					scope = g_stack[i].scope;
					break;
				}
			}
			const int slot = scope >= 0 && scope < kMaxScopes ? scope : kMaxScopes;
			if (g_untimedKey[slot] < 0) {
				const char* name = scope >= 0 ? ScopeName(scope) : nullptr;
				g_untimedKey[slot] = AddPassKey(name ? std::string(name) : std::format("untimed.scope{}", scope), -1, true);
			}
			return g_untimedKey[slot];
		}

		void PushPass(int a_key)
		{
			if (a_key < 0 || g_passDepth >= kMaxPassDepth)
				return;
			SwitchTo(g_seg, g_call);  // bill the time so far to the pass that was open
			g_passStack[g_passDepth++] = { a_key, g_depth };
			++g_passKeys[a_key].calls;
			MarkTouched(a_key);
		}

		/// Closes the innermost open pass a_match accepts (and anything opened inside it).
		template <class Match>
		void PopPass(Match a_match)
		{
			for (int i = g_passDepth - 1; i >= 0; --i) {
				if (a_match(g_passKeys[g_passStack[i].key])) {
					SwitchTo(g_seg, g_call);
					g_passDepth = i;
					return;
				}
			}
		}

		void Push(int a_scope, Seg a_seg)
		{
			if (g_depth >= kMaxDepth) {
				++g_overflow;
				return;
			}
			g_stack[g_depth++] = { a_scope, g_seg, g_call };
			SwitchTo(static_cast<int>(a_seg), -1);
		}

		void Pop(int a_scope)
		{
			if (g_overflow > 0) {
				--g_overflow;
				return;
			}
			for (int i = g_depth - 1; i >= 0; --i) {
				if (g_stack[i].scope == a_scope) {
					const StackEntry e = g_stack[i];
					SwitchTo(e.seg, e.call);  // (46n) bills the interval before the passes inside are closed
					g_depth = i;
					// (46n) Passes opened inside the popped scope cannot outlive it.
					while (g_passDepth > 0 && g_passStack[g_passDepth - 1].engineDepth > g_depth)
						--g_passDepth;
					return;
				}
			}
		}

		Seg SegOfScope(GpuScope a_scope)
		{
			switch (a_scope) {
			case GpuScope::ShadowMaps:
			case GpuScope::ShadowSun:
			case GpuScope::ShadowSlice:
			case GpuScope::ShadowLocal:
			case GpuScope::ShadowMask:
				return Seg::Shadows;
			case GpuScope::WaterPrep:
			case GpuScope::Reflections:
				return Seg::Reflections;
			case GpuScope::DepthPrepass:
			case GpuScope::World:
			case GpuScope::Opaque:
			case GpuScope::FirstPerson:
				return Seg::World;
			case GpuScope::Imagespace:
				return Seg::ImageSpace;
			case GpuScope::UI:
				return Seg::UI;
			case GpuScope::CsOverlay:
				return Seg::Overlay;
			default:
				return Seg::OurPasses;
			}
		}

		void RenderStartMarkers()
		{
			if (!g_frameToken)
				return;
			auto& upscaling = globals::features::upscaling;
			if (!upscaling.WantsReflexMarkersAtRenderStart() || !upscaling.streamline.pclFunctionsReady)
				return;
			upscaling.streamline.SetPCLMarker(sl::PCLMarker::eSimulationEnd);
			upscaling.streamline.SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);
			g_renderMarked = true;
		}

		/// Called by every probe stub (any thread; only the main thread is counted).
		void OnProbe(int a_index)
		{
			if (!OnMainThread() || a_index < 0 || a_index >= kSiteCount)
				return;
			if (a_index == g_renderIndex) {
				RenderStartMarkers();
				SwitchTo(static_cast<int>(Seg::RenderOther), -1);
				return;
			}
			SwitchTo(static_cast<int>(kSites[a_index].offset < kRenderOffset ? Seg::Logic : Seg::Tail), a_index);
		}

		void EmitStub(Xbyak::CodeGenerator& a_gen, int a_index, uintptr_t a_target)
		{
			EmitProbeStub(a_gen, a_index, a_target, &OnProbe);
		}

		// ---- device Create* timing ----
		struct CreateTimer
		{
			int64_t start = 0;
			bool main = false;
			int index = 0;
			explicit CreateTimer(int a_index) :
				index(a_index)
			{
				if (FrameTrace::Active()) {
					main = OnMainThread();
					start = FrameTrace::Now();
				}
			}
			~CreateTimer()
			{
				if (!start)
					return;
				const int64_t d = FrameTrace::Now() - start;
				if (main) {
					g_createTicks += d;
					++g_creates;
					AddPassBlock(PassBlock::Create, static_cast<double>(d) * g_toMs);  // (46n)
					if (WorldTiming::On())  // (46l) made inside the world render
						WorldTiming::AddCreate(index, static_cast<double>(d) * g_toMs);
				} else {
					g_createOtherTicks.fetch_add(d, std::memory_order_relaxed);
					g_createsOther.fetch_add(1, std::memory_order_relaxed);
				}
			}
		};

		template <std::size_t Index, class Sig>
		struct DeviceCreate;

		// ---- (46p) video memory of D3D11 textures / buffers (FrameTrace::OnD3D11Created) ----
		constexpr UINT kSharedMisc = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

		/// Bytes of one 2D surface of a_format (block-compressed formats by 4x4 blocks).
		uint64_t SurfaceBytes(DXGI_FORMAT a_format, uint64_t a_width, uint64_t a_height)
		{
			const uint64_t bits = DirectX::BitsPerPixel(a_format);
			if (DirectX::IsCompressed(a_format))
				return ((a_width + 3) / 4) * ((a_height + 3) / 4) * bits * 2;  // 4 bpp = 8 bytes a block, 8 bpp = 16
			return (a_width * a_height * (bits ? bits : 32) + 7) / 8;
		}

		uint64_t MipChainBytes(DXGI_FORMAT a_format, uint64_t a_width, uint64_t a_height, uint64_t a_depth, UINT a_mips)
		{
			uint64_t total = 0;
			const UINT mips = a_mips ? a_mips : 1;  // 0 = a full chain asked for: at least the top level counted
			for (UINT m = 0; m < mips && m < 16; ++m)
				total += SurfaceBytes(a_format, std::max<uint64_t>(a_width >> m, 1), std::max<uint64_t>(a_height >> m, 1)) * std::max<uint64_t>(a_depth >> m, 1);
			return total;
		}

		void NoteCreated(const D3D11_BUFFER_DESC* a_desc, ID3D11Buffer** a_out, const void* a_caller)
		{
			if (a_desc && a_out && *a_out)
				FrameTrace::OnD3D11Created(*a_out, a_desc->ByteWidth, (a_desc->MiscFlags & kSharedMisc) != 0, a_desc->Usage == D3D11_USAGE_STAGING, a_caller);
		}
		void NoteCreated(const D3D11_TEXTURE1D_DESC* a_desc, ID3D11Texture1D** a_out, const void* a_caller)
		{
			if (a_desc && a_out && *a_out)
				FrameTrace::OnD3D11Created(*a_out, MipChainBytes(a_desc->Format, a_desc->Width, 1, 1, a_desc->MipLevels) * std::max<UINT>(a_desc->ArraySize, 1),
					(a_desc->MiscFlags & kSharedMisc) != 0, a_desc->Usage == D3D11_USAGE_STAGING, a_caller);
		}
		void NoteCreated(const D3D11_TEXTURE2D_DESC* a_desc, ID3D11Texture2D** a_out, const void* a_caller)
		{
			if (a_desc && a_out && *a_out)
				FrameTrace::OnD3D11Created(*a_out,
					MipChainBytes(a_desc->Format, a_desc->Width, a_desc->Height, 1, a_desc->MipLevels) * std::max<UINT>(a_desc->ArraySize, 1) *
						std::max<UINT>(a_desc->SampleDesc.Count, 1),
					(a_desc->MiscFlags & kSharedMisc) != 0, a_desc->Usage == D3D11_USAGE_STAGING, a_caller);
		}
		void NoteCreated(const D3D11_TEXTURE3D_DESC* a_desc, ID3D11Texture3D** a_out, const void* a_caller)
		{
			if (a_desc && a_out && *a_out)
				FrameTrace::OnD3D11Created(*a_out, MipChainBytes(a_desc->Format, a_desc->Width, a_desc->Height, a_desc->Depth, a_desc->MipLevels),
					(a_desc->MiscFlags & kSharedMisc) != 0, a_desc->Usage == D3D11_USAGE_STAGING, a_caller);
		}

		template <std::size_t Index, class... Args>
		struct DeviceCreate<Index, HRESULT(Args...)>
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* This, Args... a_args)
			{
				HRESULT hr;
				{
					CreateTimer timer(static_cast<int>(Index));
					hr = func(This, a_args...);
				}
				// (46p) Textures and buffers: counted per owner (the caller's module) while they live.
				if constexpr (Index >= 3 && Index <= 6) {
					if (SUCCEEDED(hr)) {
						auto args = std::forward_as_tuple(a_args...);
						NoteCreated(std::get<0>(args), std::get<2>(args), _ReturnAddress());
					}
				}
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <std::size_t Index, class Sig>
		void HookCreate(ID3D11Device* a_device)
		{
			stl::detour_vfunc<Index, DeviceCreate<Index, Sig>>(a_device);
		}
	}

	const char* SegName(Seg a_seg)
	{
		static constexpr const char* kNames[] = { "outer", "reflex_sleep", "logic", "render_other", "shadows", "reflections", "world",
			"imagespace", "ui", "cs_passes", "overlay", "present", "tail" };
		static_assert(std::size(kNames) == static_cast<size_t>(Seg::Count));
		const int i = static_cast<int>(a_seg);
		return i >= 0 && i < kSegCount ? kNames[i] : "?";
	}

	bool ProbesInstalled() { return g_probes; }
	const char* ProbeStatus() { return g_probeStatus; }

	// Probe stub: notes the call, then jumps to the original target as if called directly.
	// Saves the flags and every volatile register (rax, rcx, rdx, r8-r11, xmm0-xmm5), so the
	// callee sees exactly the registers and the stack the engine set up; the C++ callback
	// preserves the non-volatile ones itself. On entry rsp is 8 mod 16 (the engine's call
	// pushed the return address); 8 pushes + 0x88 keep the callback's call 16-byte aligned.
	void EmitProbeStub(Xbyak::CodeGenerator& a_gen, int a_index, uintptr_t a_target, void (*a_callback)(int))
	{
		using namespace Xbyak::util;
		Xbyak::Label callback, target;
		a_gen.pushfq();
		a_gen.push(rax);
		a_gen.push(rcx);
		a_gen.push(rdx);
		a_gen.push(r8);
		a_gen.push(r9);
		a_gen.push(r10);
		a_gen.push(r11);
		a_gen.sub(rsp, 0x88);
		a_gen.movdqu(ptr[rsp + 0x20], xmm0);
		a_gen.movdqu(ptr[rsp + 0x30], xmm1);
		a_gen.movdqu(ptr[rsp + 0x40], xmm2);
		a_gen.movdqu(ptr[rsp + 0x50], xmm3);
		a_gen.movdqu(ptr[rsp + 0x60], xmm4);
		a_gen.movdqu(ptr[rsp + 0x70], xmm5);
		a_gen.mov(ecx, a_index);
		a_gen.call(ptr[rip + callback]);
		a_gen.movdqu(xmm0, ptr[rsp + 0x20]);
		a_gen.movdqu(xmm1, ptr[rsp + 0x30]);
		a_gen.movdqu(xmm2, ptr[rsp + 0x40]);
		a_gen.movdqu(xmm3, ptr[rsp + 0x50]);
		a_gen.movdqu(xmm4, ptr[rsp + 0x60]);
		a_gen.movdqu(xmm5, ptr[rsp + 0x70]);
		a_gen.add(rsp, 0x88);
		a_gen.pop(r11);
		a_gen.pop(r10);
		a_gen.pop(r9);
		a_gen.pop(r8);
		a_gen.pop(rdx);
		a_gen.pop(rcx);
		a_gen.pop(rax);
		a_gen.popfq();
		a_gen.jmp(ptr[rip + target]);
		a_gen.L(callback);
		a_gen.dq(reinterpret_cast<uint64_t>(a_callback));
		a_gen.L(target);
		a_gen.dq(static_cast<uint64_t>(a_target));
	}

	const char* ProbeBlocker()
	{
		if (!REL::Module::IsSE())
			return "SE 1.5.97 only";
		std::error_code ec;
		const auto killFile = Util::PathHelpers::GetSettingsUserPath().parent_path() / kKillSwitchFile;
		if (std::filesystem::exists(killFile, ec))
			return "EngineTimingOff.txt exists";
		if (!globals::features::performanceOverlay.settings.EngineTimingProbes)
			return "Performance Overlay > Engine Timing Probes is off";
		return nullptr;
	}

	void InstallUpdateProbes()
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_toMs = f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;

		if (!REL::Module::IsSE()) {
			g_probeStatus = "SE 1.5.97 only";
			logger::info("[EngineTiming] Main::Update probes SKIPPED: SE 1.5.97 only; engine split and Reflex render-start markers off");
			return;
		}
		// (44f) Escape hatches, read once here: the menu setting (needs a restart) and a file that
		// works without the menu (e.g. when the game crashes before it can be opened).
		std::error_code ec;
		const auto killFile = Util::PathHelpers::GetSettingsUserPath().parent_path() / kKillSwitchFile;
		if (std::filesystem::exists(killFile, ec)) {
			g_probeStatus = "off (EngineTimingOff.txt)";
			logger::info("[EngineTiming] Main::Update probes SKIPPED: {} exists; engine split and Reflex render-start markers off", killFile.string());
			return;
		}
		if (!globals::features::performanceOverlay.settings.EngineTimingProbes) {
			g_probeStatus = "off (setting)";
			logger::info("[EngineTiming] Main::Update probes SKIPPED: Performance Overlay > Engine Timing Probes is off; engine split and Reflex render-start markers off");
			return;
		}
		const uintptr_t base = REL::ID(kMainUpdateId).address();

		// Every site must still be a direct call. Most must still call the function the table
		// says (another plugin may have redirected a few, which chains fine); if many do not,
		// this is not the Main::Update the table was read from and nothing is installed.
		std::array<uintptr_t, kSiteCount> targets{};
		int mismatched = 0;
		for (int i = 0; i < kSiteCount; ++i) {
			const auto* p = reinterpret_cast<const uint8_t*>(base + kSites[i].offset);
			if (p[0] != 0xE8) {
				g_probeStatus = "rejected (byte check)";
				logger::warn("[EngineTiming] Main::Update probes REJECTED: +0x{:X} is not a call (0x{:02X}); nothing installed", kSites[i].offset, p[0]);
				return;
			}
			int32_t rel = 0;
			std::memcpy(&rel, p + 1, sizeof(rel));
			targets[i] = base + kSites[i].offset + 5 + static_cast<intptr_t>(rel);
			if (targets[i] != REL::ID(kSites[i].calleeId).address())
				++mismatched;
			if (kSites[i].offset == kRenderOffset)
				g_renderIndex = i;
			if (kSites[i].offset == kInputOffset)
				g_inputIndex = i;
		}
		if (mismatched > 8) {
			g_probeStatus = "rejected (byte check)";
			logger::warn("[EngineTiming] Main::Update probes REJECTED: {} of {} calls do not match SE 1.5.97; nothing installed", mismatched, kSiteCount);
			return;
		}

		// One code buffer for every stub, in its own pages, read/execute once written. (Xbyak's
		// ready() only changes protection for AutoGrow buffers, so the pages are flipped here.)
		const size_t codeSize = static_cast<size_t>(kSiteCount) * 256;
		void* code = VirtualAlloc(nullptr, codeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		if (!code) {
			g_probeStatus = "failed (no code memory)";
			logger::warn("[EngineTiming] Main::Update probes FAILED: VirtualAlloc error {}; nothing installed", GetLastError());
			return;
		}
		auto* gen = new Xbyak::CodeGenerator(codeSize, code);
		std::array<const uint8_t*, kSiteCount> stubs{};
		for (int i = 0; i < kSiteCount; ++i) {
			gen->align(16);
			stubs[i] = gen->getCurr();
			EmitStub(*gen, i, targets[i]);
		}
		gen->ready();
		DWORD oldProtect = 0;
		if (!VirtualProtect(code, codeSize, PAGE_EXECUTE_READ, &oldProtect)) {
			g_probeStatus = "failed (protect)";
			logger::warn("[EngineTiming] Main::Update probes FAILED: VirtualProtect error {}; nothing installed", GetLastError());
			return;
		}
		FlushInstructionCache(GetCurrentProcess(), code, codeSize);

		SKSE::AllocTrampoline(static_cast<size_t>(kSiteCount) * 14);
		auto& trampoline = SKSE::GetTrampoline();
		for (int i = 0; i < kSiteCount; ++i) {
			const uintptr_t previous = trampoline.write_call<5>(base + kSites[i].offset, reinterpret_cast<uintptr_t>(stubs[i]));
			if (previous != targets[i])
				logger::warn("[EngineTiming] Main::Update+0x{:X} changed while installing", kSites[i].offset);
		}
		g_probes = true;
		g_probeStatus = "installed";
		if (!g_calibrationStarted) {  // (46o) M1
			g_calibrationStarted = true;
			std::thread(CalibrateCycles).detach();
		}
		logger::info("[EngineTiming] Main::Update probes INSTALLED: {} call sites ({} already redirected by other plugins). To turn them off: Performance Overlay > Engine Timing Probes, or create {} next to SettingsUser.json", kSiteCount, mismatched, kKillSwitchFile);
	}

	void InstallDeviceHooks(ID3D11Device* a_device)
	{
		if (!a_device)
			return;
		HookCreate<3, HRESULT(const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**)>(a_device);
		HookCreate<4, HRESULT(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**)>(a_device);
		HookCreate<5, HRESULT(const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**)>(a_device);
		HookCreate<6, HRESULT(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**)>(a_device);
		HookCreate<7, HRESULT(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**)>(a_device);
		HookCreate<8, HRESULT(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**)>(a_device);
		HookCreate<9, HRESULT(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**)>(a_device);
		HookCreate<10, HRESULT(ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**)>(a_device);
		HookCreate<11, HRESULT(const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**)>(a_device);
		// 12 (vertex) and 15 (pixel shaders) are hooked by Hooks.cpp in shader-dump mode only;
		// those two are timed through ShaderCache's own counters instead.
		HookCreate<13, HRESULT(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**)>(a_device);
		HookCreate<18, HRESULT(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**)>(a_device);
		HookCreate<20, HRESULT(const D3D11_BLEND_DESC*, ID3D11BlendState**)>(a_device);
		HookCreate<21, HRESULT(const D3D11_DEPTH_STENCIL_DESC*, ID3D11DepthStencilState**)>(a_device);
		HookCreate<22, HRESULT(const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**)>(a_device);
		HookCreate<24, HRESULT(const D3D11_QUERY_DESC*, ID3D11Query**)>(a_device);
		logger::info("[EngineTiming] Device Create* timers installed");
	}

	void OnUpdateBegin(bool a_frameToken)
	{
		g_mainThread = GetCurrentThreadId();
		WorldTiming::OnFrameStart();
		g_frameToken = a_frameToken;
		g_renderMarked = false;
		g_depth = 0;
		g_overflow = 0;
		g_passDepth = 0;
		SwitchTo(static_cast<int>(Seg::Logic), -1);
	}

	void OnUpdateEnd()
	{
		if (!OnMainThread())
			return;
		g_depth = 0;
		g_overflow = 0;
		g_passDepth = 0;
		SwitchTo(static_cast<int>(Seg::Outer), -1);
	}

	// ---- (46n) cs_passes split per pass ----

	void PassBegin(std::string_view a_key)
	{
		if (!g_probes || !FrameTrace::Active() || !OnMainThread())
			return;
		PushPass(InternPassKey(a_key));
	}

	void PassEnd(std::string_view a_key)
	{
		if (g_passDepth == 0 || !OnMainThread())
			return;
		PopPass([a_key](const PassKey& a_k) { return a_k.gpuBucket < 0 && !a_k.untimed && a_k.name == a_key; });
	}

	void PassBeginGpu(int a_bucket, std::string_view a_name)
	{
		if (!g_probes || !FrameTrace::Active() || a_bucket < 0 || a_bucket >= kMaxGpuBuckets || !OnMainThread())
			return;
		int& key = g_gpuPassKey[a_bucket];
		if (key < 0)
			key = AddPassKey(std::format("g.{}", a_name), a_bucket, false);
		PushPass(key);
	}

	void PassEndGpu(int a_bucket)
	{
		if (g_passDepth == 0 || !OnMainThread())
			return;
		PopPass([a_bucket](const PassKey& a_k) { return a_k.gpuBucket == a_bucket; });
	}

	void AddPassBlock(PassBlock a_kind, double a_ms)
	{
		if (!g_probes || !OnMainThread() || g_seg != kOurPasses)
			return;
		const int key = CurrentPassKey();
		if (key < 0)
			return;
		auto& k = g_passKeys[key];
		k.blockMs[static_cast<int>(a_kind)] += a_ms;
		++k.blockCalls[static_cast<int>(a_kind)];
		MarkTouched(key);
	}

	uint64_t FrameId() { return g_frameId; }

	std::string_view GpuPassName(int a_bucket)
	{
		if (a_bucket < 0 || a_bucket >= kMaxGpuBuckets || g_gpuPassKey[a_bucket] < 0)
			return {};
		return g_passKeys[g_gpuPassKey[a_bucket]].name;
	}

	CompileTimer::CompileTimer()
	{
		if (g_probes && FrameTrace::Active() && OnMainThread())
			start = FrameTrace::Now();
	}

	CompileTimer::~CompileTimer()
	{
		if (start)
			AddPassBlock(PassBlock::Compile, static_cast<double>(FrameTrace::Now() - start) * g_toMs);
	}

	ScopedSeg::ScopedSeg(Seg a_seg)
	{
		if (!OnMainThread())
			return;
		Push(-1, a_seg);
		pushed = true;
	}

	ScopedSeg::~ScopedSeg()
	{
		if (pushed)
			Pop(-1);
	}

	void OnPhasePush(int a_gpuScope)
	{
		if (!g_probes || !OnMainThread())
			return;
		Push(a_gpuScope, SegOfScope(static_cast<GpuScope>(a_gpuScope)));
		WorldTiming::OnScopePush(a_gpuScope);  // (46l) names the world-render place
	}

	void OnPhasePop(int a_gpuScope)
	{
		if (!g_probes || !OnMainThread())
			return;
		Pop(a_gpuScope);
	}

	bool RenderStartMarked() { return g_renderMarked; }

	Frame TakeFrame(bool a_detail, bool a_passWindow)
	{
		Frame f;
		SwitchTo(g_seg, g_call);  // close the open interval at "now"
		f.id = g_frameId++;
		// (46o) M1: wall - thread CPU per segment.
		if (WaitTrackingOn() && OnMainThread())
			SampleWait(g_seg);
		if (g_waitTouched) {
			const double cpm = g_cyclesPerMs.load(std::memory_order_relaxed);
			double cpu = 0.0;
			for (int i = 0; i < kSegCount; ++i) {
				const double c = static_cast<double>(g_cycAcc[i]) / cpm;
				cpu += c;
				f.waitMs[i] = static_cast<float>(std::max(static_cast<double>(g_waitWallAcc[i]) * g_toMs - c, 0.0));
			}
			f.mainCpuMs = static_cast<float>(cpu);
		}
		g_cycAcc.fill(0);
		g_waitWallAcc.fill(0);
		g_waitTouched = false;
		g_frameNow.store(g_frameId, std::memory_order_relaxed);  // (46o) M2

		if (g_probes) {
			f.valid = true;
			for (int i = 0; i < kSegCount; ++i)
				f.ms[i] = static_cast<float>(static_cast<double>(g_acc[i]) * g_toMs);
			// (46n) cs_passes split: the sum and the untimed part always, the passes when asked for.
			{
				double sum = 0.0, untimed = 0.0;
				const bool list = a_detail && (a_passWindow || f.ms[kOurPasses] > kPassSpikeMs);
				for (int t = 0; t < g_passTouchedCount; ++t) {
					const auto& k = g_passKeys[g_passTouched[t]];
					const double ms = static_cast<double>(k.ticks) * g_toMs;
					sum += ms;
					if (k.untimed)
						untimed += ms;
					if (list) {
						PassItem item;
						item.name = k.name;
						item.gpuBucket = k.gpuBucket;
						item.cpuMs = static_cast<float>(ms);
						item.calls = k.calls;
						item.untimed = k.untimed;
						for (int b = 0; b < kPassBlocks; ++b) {
							item.blockMs[b] = static_cast<float>(k.blockMs[b]);
							item.blockCalls[b] = k.blockCalls[b];
						}
						f.passes.push_back(item);
					}
				}
				f.passSumMs = static_cast<float>(sum);
				f.passUntimedMs = static_cast<float>(untimed);
				std::sort(f.passes.begin(), f.passes.end(), [](const PassItem& a_l, const PassItem& a_r) { return a_l.cpuMs > a_r.cpuMs; });
			}
			if (g_inputIndex >= 0)
				f.inputMs = static_cast<float>(static_cast<double>(g_callAcc[g_inputIndex]) * g_toMs);
			int top = -1;
			for (int i = 0; i < kSiteCount; ++i) {
				if (i == g_renderIndex)
					continue;
				if (top < 0 || g_callAcc[i] > g_callAcc[top])
					top = i;
				if (a_detail) {
					const double ms = static_cast<double>(g_callAcc[i]) * g_toMs;
					if (ms > 1.0)
						f.callsOver1ms += std::format("{}@{:X}:{:.2f};", kSites[i].calleeId, kSites[i].offset, ms);
				}
			}
			if (top >= 0 && g_callAcc[top] > 0) {
				f.topCallId = kSites[top].calleeId;
				f.topCallOffset = kSites[top].offset;
				f.topCallMs = static_cast<float>(static_cast<double>(g_callAcc[top]) * g_toMs);
			}
		}
		if (auto* timer = RE::BSTimer::GetSingleton()) {
			f.engineDtMs = timer->delta * 1000.0f;
			f.engineRealDtMs = timer->realTimeDelta * 1000.0f;
		}
		f.createMs = static_cast<float>(static_cast<double>(g_createTicks) * g_toMs);
		f.creates = g_creates;
		f.createOtherMs = static_cast<float>(static_cast<double>(g_createOtherTicks.exchange(0, std::memory_order_relaxed)) * g_toMs);
		f.createsOther = g_createsOther.exchange(0, std::memory_order_relaxed);

		g_acc.fill(0);
		g_callAcc.fill(0);
		g_createTicks = 0;
		g_creates = 0;
		for (int t = 0; t < g_passTouchedCount; ++t) {  // (46n)
			auto& k = g_passKeys[g_passTouched[t]];
			k.ticks = 0;
			k.calls = 0;
			std::fill(std::begin(k.blockMs), std::end(k.blockMs), 0.0);
			std::fill(std::begin(k.blockCalls), std::end(k.blockCalls), 0u);
			k.touched = false;
		}
		g_passTouchedCount = 0;
		return f;
	}

	// ---- (46o) M1 ----

	void SetPacerWaitTracking(bool a_on) { g_waitPacer = a_on; }

	double RenderWaitMsSoFar()
	{
		if (!WaitTrackingOn() || !OnMainThread())
			return -1.0;
		SampleWait(g_seg);
		const double cpm = g_cyclesPerMs.load(std::memory_order_relaxed);
		double wall = 0.0, cpu = 0.0;
		for (int i = static_cast<int>(Seg::RenderOther); i <= static_cast<int>(Seg::Present); ++i) {
			wall += static_cast<double>(g_waitWallAcc[i]) * g_toMs;
			cpu += static_cast<double>(g_cycAcc[i]) / cpm;
		}
		return std::max(wall - cpu, 0.0);
	}

	double CyclesPerMs() { return g_cyclesPerMs.load(std::memory_order_relaxed); }
	double CycleReadCostNs() { return g_cycleCostNs.load(std::memory_order_relaxed); }

	// ---- (46o) M2 ----

	void SetStallSampler(bool a_on)
	{
		a_on = a_on && g_probes && OnMainThread();
		if (a_on == g_samplerOn.load(std::memory_order_relaxed))
			return;
		if (a_on) {
			if (!g_mainHandle) {
				g_mainHandle = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, g_mainThread);
				if (!g_mainHandle) {
					logger::warn("[EngineTiming] (46o) Stall sampler: OpenThread failed ({}); off", GetLastError());
					return;
				}
				// The main thread's stack base (this is the main thread).
				g_mainStackBase = reinterpret_cast<uintptr_t>(reinterpret_cast<const NT_TIB*>(NtCurrentTeb())->StackBase);
			}
			RefreshModules();
			g_segSince.store(0, std::memory_order_relaxed);
			if (!g_samplerThread.exchange(true)) {
				std::thread(SamplerThread).detach();
				logger::info("[EngineTiming] (46o) Stall sampler started ({} modules)", g_moduleCount.load());
			}
		}
		g_samplerOn.store(a_on, std::memory_order_release);
	}

	std::string TakeStallSamples()
	{
		std::scoped_lock lk(g_sampleMutex);
		std::string out;
		out.swap(g_samples);
		return out;
	}
}
