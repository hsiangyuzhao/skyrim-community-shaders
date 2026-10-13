#include "Utils/FrameTrace.h"

#include "Features/PerformanceOverlay.h"
#include "Features/Upscaling.h"
#include "Menu.h"
#include "State.h"
#include "BuildLabel.h"

#include <thread>

namespace Util::FrameTrace
{
	namespace
	{
		constexpr int kCalls = static_cast<int>(DriverCall::Count);

		int64_t g_frequency = 0;
		double g_toMs = 0.0;

		// Latched once per frame (OnPresentEnd): every timed call then costs one branch when off.
		bool g_active = false;

		struct FrameAcc
		{
			double driver[kCalls]{};
			double driverTotal = 0.0;
			double ownDrawHook = 0.0;
			double ownSetupGeometry = 0.0;
			double permutationUpload = 0.0;
			double reflexSleep = 0.0;
			uint32_t draws = 0;
			uint32_t lightingDraws = 0;
			uint32_t utilityDraws = 0;
			uint32_t firstSeen = 0;
			uint32_t fgPresented = 0;
			float grassApplyMs = -1.0f;
		};
		FrameAcc g_frame;
		int64_t g_simStart = 0;
		std::atomic<uint32_t> g_shaderAsync{ 0 }, g_shaderSync{ 0 }, g_cellAttach{ 0 }, g_cellDetach{ 0 };
		std::unordered_set<const void*> g_seenGeometry;
		bool g_cellSinkRegistered = false;
		float g_lastYaw = 0.0f, g_lastPitch = 0.0f;
		RE::NiPoint3 g_lastPlayerPos{};
		bool g_motionValid = false;

		// Overlay series: one entry per real frame while active.
		struct HistEntry
		{
			int64_t t = 0;
			float cpuMs = 0.0f;
			float gpuMs = -1.0f;
			uint64_t gpuIndex = 0;
		};
		constexpr size_t kHist = 4096;
		std::array<HistEntry, kHist> g_hist{};
		size_t g_histHead = 0;  // next write
		size_t g_histCount = 0;

		struct CellSink : RE::BSTEventSink<RE::TESCellAttachDetachEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				if (a_event)
					(a_event->attached ? g_cellAttach : g_cellDetach).fetch_add(1, std::memory_order_relaxed);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		CellSink g_cellSink;
		int64_t g_lastPresentEnd = 0;

		Smoothed g_smoothed;

		// ---- recorder ----
		struct Row
		{
			double tMs = 0.0;
			float frameMs = 0.0f;
			float presentWaitMs = 0.0f;
			float reflexSleepMs = 0.0f;
			float driver[kCalls]{};
			float ownDrawHookMs = 0.0f;
			float ownSetupGeometryMs = 0.0f;
			float permutationUploadMs = 0.0f;
			uint32_t draws = 0;
			uint32_t lightingDraws = 0;
			uint32_t utilityDraws = 0;
			uint32_t firstSeen = 0;
			uint32_t shaderAsync = 0;
			uint32_t shaderSync = 0;
			uint32_t cellAttach = 0;
			uint32_t cellDetach = 0;
			float cpuFrameMs = -1.0f;
			float yawRate = 0.0f;
			float pitchRate = 0.0f;
			float playerSpeed = 0.0f;
			uint32_t fgPresented = 0;
			float grassApplyMs = -1.0f;
			uint64_t gpuFrameIndex = 0;
			float gpuWorkMs = -1.0f;
			float gpuElapsedMs = -1.0f;
			uint32_t reflexFrame = UINT32_MAX;
		};

		struct ReflexTimes
		{
			uint64_t simStart, simEnd, submitStart, submitEnd, presentStart, presentEnd, driverStart, driverEnd, queueStart, queueEnd, gpuStart, gpuEnd;
		};

		bool g_recording = false;
		int64_t g_recordStart = 0;
		int64_t g_recordEnd = 0;
		std::vector<Row> g_rows;
		Row g_pendingRow{};
		bool g_hasPendingRow = false;
		std::unordered_map<uint64_t, ReflexTimes> g_reflex;
		uint32_t g_reflexPoll = 0;
		std::mutex g_fileMutex;
		std::string g_lastFile;

		double Ms(int64_t a_ticks) { return static_cast<double>(a_ticks) * g_toMs; }

		bool WantedThisFrame()
		{
			auto& overlay = globals::features::performanceOverlay;
			if (!overlay.loaded || !overlay.settings.DriverWaitTiming)
				return false;
			if (g_recording)
				return true;
			return globals::menu && globals::menu->overlayVisible && overlay.IsOverlayVisible();
		}

		void PollReflex()
		{
			auto& streamline = globals::features::upscaling.streamline;
			if (!streamline.reflexFunctionsReady)
				return;
			sl::ReflexState state{};
			if (!streamline.GetReflexState(state) || !state.latencyReportAvailable)
				return;
			for (const auto& r : state.frameReport) {
				if (r.simStartTime == 0)
					continue;
				g_reflex[r.frameID] = { r.simStartTime, r.simEndTime, r.renderSubmitStartTime, r.renderSubmitEndTime, r.presentStartTime, r.presentEndTime,
					r.driverStartTime, r.driverEndTime, r.osRenderQueueStartTime, r.osRenderQueueEndTime, r.gpuRenderStartTime, r.gpuRenderEndTime };
			}
		}

		void WriteCsv(std::vector<Row> a_rows, std::unordered_map<uint64_t, ReflexTimes> a_reflex)
		{
			try {
				auto dir = logger::log_directory();
				if (!dir)
					return;
				std::filesystem::path path = *dir / "CommunityShaders" / "Perf";
				std::filesystem::create_directories(path);
				const auto now = std::chrono::system_clock::now();
				const std::time_t t = std::chrono::system_clock::to_time_t(now);
				std::tm local{};
				localtime_s(&local, &t);
				char stamp[32];
				std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
				const std::string name = std::format("trace-{}.csv", stamp);
				path /= name;

				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out)
					return;
				out << "# Community Shaders frame trace (" << BuildLabel::kBuild << "). One row per real (rendered) frame. Times in ms unless noted.\n";
				out << "# cpu_busy = frame - present_wait - reflex_sleep (includes driver_*). gpu_* come from GPU timestamps (-1 = no sample).\n";
				out << "# reflex_* in us relative to reflex_sim_start (-1 = no report). fg_presented = frames DLSS-G showed for this frame (0 = not generating).\n";
				out << "# cpu_frame = simulation start (after Reflex sleep) to Present start: all CPU submission work, driver blocking included.\n";
				out << "frame,t_ms,frame_ms,cpu_frame_ms,cpu_busy_ms,present_wait_ms,reflex_sleep_ms,"
					   "driver_draw_ms,driver_map_ms,driver_upload_ms,driver_dispatch_ms,driver_flush_ms,driver_total_ms,"
					   "own_draw_hook_ms,own_setup_geometry_ms,perm_cb_upload_ms,draws,lighting_draws,utility_draws,own_per_draw_us,"
					   "first_seen_geometry,shader_miss_async,shader_miss_sync,cell_attach,cell_detach,cam_yaw_dps,cam_pitch_dps,player_speed,"
					   "gpu_work_ms,gpu_elapsed_ms,fg_presented,grass_apply_ms,"
					   "reflex_frame,reflex_sim_end,reflex_submit_start,reflex_submit_end,reflex_present_start,reflex_present_end,"
					   "reflex_driver_start,reflex_driver_end,reflex_queue_start,reflex_queue_end,reflex_gpu_start,reflex_gpu_end,reflex_latency_ms\n";
				uint32_t index = 0;
				for (const auto& r : a_rows) {
					float driverTotal = 0.0f;
					for (float d : r.driver)
						driverTotal += d;
					const float busy = std::max(0.0f, r.frameMs - r.presentWaitMs - r.reflexSleepMs);
					const float perDraw = r.draws ? (r.ownDrawHookMs + r.ownSetupGeometryMs) * 1000.0f / static_cast<float>(r.draws) : 0.0f;
					out << std::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{:.3f},{},{},{},{},{},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f},{},{:.3f},",
						index++, r.tMs, r.frameMs, r.cpuFrameMs, busy, r.presentWaitMs, r.reflexSleepMs,
						r.driver[0], r.driver[1], r.driver[2], r.driver[3], r.driver[4], driverTotal,
						r.ownDrawHookMs, r.ownSetupGeometryMs, r.permutationUploadMs, r.draws, r.lightingDraws, r.utilityDraws, perDraw,
						r.firstSeen, r.shaderAsync, r.shaderSync, r.cellAttach, r.cellDetach, r.yawRate, r.pitchRate, r.playerSpeed,
						r.gpuWorkMs, r.gpuElapsedMs, r.fgPresented, r.grassApplyMs);
					auto it = r.reflexFrame != UINT32_MAX ? a_reflex.find(r.reflexFrame) : a_reflex.end();
					if (it == a_reflex.end()) {
						out << (r.reflexFrame == UINT32_MAX ? std::string("-1") : std::to_string(r.reflexFrame)) << ",-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1\n";
						continue;
					}
					const auto& x = it->second;
					auto rel = [&](uint64_t v) { return v >= x.simStart && v != 0 ? static_cast<int64_t>(v - x.simStart) : -1; };
					out << std::format("{},{},{},{},{},{},{},{},{},{},{},{},{:.3f}\n", r.reflexFrame, rel(x.simEnd), rel(x.submitStart), rel(x.submitEnd), rel(x.presentStart),
						rel(x.presentEnd), rel(x.driverStart), rel(x.driverEnd), rel(x.queueStart), rel(x.queueEnd), rel(x.gpuStart), rel(x.gpuEnd),
						x.gpuEnd > x.simStart ? static_cast<double>(x.gpuEnd - x.simStart) / 1000.0 : -1.0);
				}
				out.close();
				{
					std::scoped_lock lk(g_fileMutex);
					g_lastFile = name;
				}
				logger::info("[FrameTrace] Wrote {} frames to {}", a_rows.size(), path.string());
			} catch (const std::exception& e) {
				logger::error("[FrameTrace] Writing the trace failed: {}", e.what());
			}
		}

		// ---- D3D11 immediate-context hooks ----
		template <DriverCall C>
		struct Timed
		{
			ID3D11DeviceContext* ctx;
			int64_t start = 0;
			explicit Timed(ID3D11DeviceContext* a_ctx) :
				ctx(a_ctx)
			{
				if (g_active && a_ctx == globals::d3d::context)
					start = Now();
			}
			~Timed()
			{
				if (start) {
					const double ms = Ms(Now() - start);
					g_frame.driver[static_cast<int>(C)] += ms;
					g_frame.driverTotal += ms;
				}
			}
		};

		struct DrawIndexed
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, INT c)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Draw
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Map
		{
			static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Resource* r, UINT s, D3D11_MAP m, UINT f, D3D11_MAPPED_SUBRESOURCE* o)
			{
				Timed<DriverCall::Map> t(This);
				return func(This, r, s, m, f, o);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Unmap
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* r, UINT s)
			{
				Timed<DriverCall::Map> t(This);
				func(This, r, s);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawIndexedInstanced
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, UINT c, INT d, UINT e)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b, c, d, e);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawInstanced
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, UINT c, UINT d)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b, c, d);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawIndexedInstancedIndirect
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Buffer* a, UINT b)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawInstancedIndirect
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Buffer* a, UINT b)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Dispatch
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, UINT c)
			{
				Timed<DriverCall::Dispatch> t(This);
				func(This, a, b, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DispatchIndirect
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Buffer* a, UINT b)
			{
				Timed<DriverCall::Dispatch> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct CopySubresourceRegion
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* b)
			{
				Timed<DriverCall::Upload> t(This);
				func(This, d, ds, x, y, z, s, ss, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct CopyResource
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, ID3D11Resource* s)
			{
				Timed<DriverCall::Upload> t(This);
				func(This, d, s);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct UpdateSubresource
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, UINT ds, const D3D11_BOX* b, const void* p, UINT rp, UINT dp)
			{
				Timed<DriverCall::Upload> t(This);
				func(This, d, ds, b, p, rp, dp);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Flush
		{
			static void thunk(ID3D11DeviceContext* This)
			{
				Timed<DriverCall::Flush> t(This);
				func(This);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	int64_t Now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	void InstallContextHooks(ID3D11DeviceContext* a_context)
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_frequency = f.QuadPart;
		g_toMs = g_frequency ? 1000.0 / static_cast<double>(g_frequency) : 0.0;

		stl::detour_vfunc<12, DrawIndexed>(a_context);
		stl::detour_vfunc<13, Draw>(a_context);
		stl::detour_vfunc<14, Map>(a_context);
		stl::detour_vfunc<15, Unmap>(a_context);
		stl::detour_vfunc<20, DrawIndexedInstanced>(a_context);
		stl::detour_vfunc<21, DrawInstanced>(a_context);
		stl::detour_vfunc<39, DrawIndexedInstancedIndirect>(a_context);
		stl::detour_vfunc<40, DrawInstancedIndirect>(a_context);
		stl::detour_vfunc<41, Dispatch>(a_context);
		stl::detour_vfunc<42, DispatchIndirect>(a_context);
		stl::detour_vfunc<46, CopySubresourceRegion>(a_context);
		stl::detour_vfunc<47, CopyResource>(a_context);
		stl::detour_vfunc<48, UpdateSubresource>(a_context);
		stl::detour_vfunc<111, Flush>(a_context);
		logger::info("[FrameTrace] D3D11 call timing hooks installed");
	}

	bool Active() { return g_active; }
	bool IsRecording() { return g_recording; }

	double DriverMsSoFar() { return g_frame.driverTotal; }

	void AddOwnDrawHook(int64_t a_start, double a_driverAtStart, int a_type)
	{
		if (!g_active)
			return;
		const double ms = Ms(Now() - a_start) - (g_frame.driverTotal - a_driverAtStart);
		g_frame.ownDrawHook += std::max(ms, 0.0);
		++g_frame.draws;
		if (a_type == static_cast<int>(RE::BSShader::Type::Lighting))
			++g_frame.lightingDraws;
		else if (a_type == static_cast<int>(RE::BSShader::Type::Utility))
			++g_frame.utilityDraws;
	}

	void OnLightingGeometry(const void* a_geometry)
	{
		if (!g_active)
			return;
		if (g_seenGeometry.size() > 2000000)
			g_seenGeometry.clear();
		if (g_seenGeometry.insert(a_geometry).second)
			++g_frame.firstSeen;
	}

	void OnShaderMiss(bool a_async)
	{
		(a_async ? g_shaderAsync : g_shaderSync).fetch_add(1, std::memory_order_relaxed);
	}

	void OnSimStart()
	{
		g_simStart = Now();
	}

	SeriesStats GetSeriesStats(float a_seconds)
	{
		SeriesStats s;
		if (!g_histCount || !g_frequency)
			return s;
		const int64_t cutoff = Now() - static_cast<int64_t>(static_cast<double>(a_seconds) * static_cast<double>(g_frequency));
		std::vector<float> cpu, gpu;
		cpu.reserve(g_histCount);
		gpu.reserve(g_histCount);
		int cpuLimited = 0, both = 0;
		for (size_t i = 0; i < g_histCount; ++i) {
			const auto& e = g_hist[(g_histHead + kHist - 1 - i) % kHist];
			if (e.t < cutoff)
				break;
			if (e.cpuMs > 0.0f)
				cpu.push_back(e.cpuMs);
			if (e.gpuMs > 0.0f)
				gpu.push_back(e.gpuMs);
			if (e.cpuMs > 0.0f && e.gpuMs > 0.0f) {
				++both;
				if (e.cpuMs > e.gpuMs)
					++cpuLimited;
			}
		}
		auto stats = [](std::vector<float>& v, float& avg, float& low1, float& mx) {
			if (v.empty())
				return;
			double sum = 0.0;
			for (float x : v)
				sum += x;
			avg = static_cast<float>(sum / static_cast<double>(v.size()));
			const size_t k = std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.99));
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
			low1 = v[k];
			mx = *std::max_element(v.begin(), v.end());
		};
		s.frames = static_cast<int>(cpu.size());
		s.gpuFrames = static_cast<int>(gpu.size());
		stats(cpu, s.cpuAvg, s.cpuLow1, s.cpuMax);
		stats(gpu, s.gpuAvg, s.gpuLow1, s.gpuMax);
		s.cpuLimitedPct = both ? 100.0f * static_cast<float>(cpuLimited) / static_cast<float>(both) : 0.0f;
		return s;
	}

	void GetSeries(std::vector<float>& o_cpu, std::vector<float>& o_gpu, int a_count)
	{
		o_cpu.clear();
		o_gpu.clear();
		const size_t n = std::min(g_histCount, static_cast<size_t>(std::max(a_count, 0)));
		for (size_t i = 0; i < n; ++i) {
			const auto& e = g_hist[(g_histHead + kHist - n + i) % kHist];
			o_cpu.push_back(e.cpuMs);
			o_gpu.push_back(e.gpuMs);
		}
	}

	void AddOwnSetupGeometry(int64_t a_start, double a_driverAtStart)
	{
		if (!g_active)
			return;
		const double ms = Ms(Now() - a_start) - (g_frame.driverTotal - a_driverAtStart);
		g_frame.ownSetupGeometry += std::max(ms, 0.0);
	}

	void AddPermutationUpload(int64_t a_start)
	{
		if (g_active)
			g_frame.permutationUpload += Ms(Now() - a_start);
	}

	void OnReflexSleep(int64_t a_start)
	{
		if (g_active)
			g_frame.reflexSleep += Ms(Now() - a_start);
	}

	void OnFramesPresented(uint32_t a_count) { g_frame.fgPresented = a_count; }
	void OnGrassApply(float a_ms) { g_frame.grassApplyMs = a_ms; }

	void OnGpuSample(uint64_t a_gpuFrameIndex, float a_workMs, float a_elapsedMs)
	{
		for (size_t i = 0; i < std::min<size_t>(g_histCount, 16); ++i) {
			auto& e = g_hist[(g_histHead + kHist - 1 - i) % kHist];
			if (e.gpuIndex == a_gpuFrameIndex) {
				e.gpuMs = a_workMs;
				break;
			}
		}
		if (!g_recording)
			return;
		for (auto it = g_rows.rbegin(); it != g_rows.rend() && it - g_rows.rbegin() < 16; ++it) {
			if (it->gpuFrameIndex == a_gpuFrameIndex) {
				it->gpuWorkMs = a_workMs;
				it->gpuElapsedMs = a_elapsedMs;
				return;
			}
		}
	}

	void OnPresentEnd(int64_t a_presentStart, uint64_t a_gpuFrameIndex)
	{
		const int64_t now = Now();
		const bool wasActive = g_active;

		if (!g_cellSinkRegistered) {
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESCellAttachDetachEvent>(&g_cellSink);
				g_cellSinkRegistered = true;
			}
		}

		if (wasActive && g_lastPresentEnd) {
			const float frameMs = static_cast<float>(Ms(now - g_lastPresentEnd));
			const float presentWait = static_cast<float>(Ms(now - a_presentStart));
			// CPU frame: simulation start (after the Reflex sleep) to Present start. Without a
			// simulation marker this frame, from the previous Present's end minus the sleep.
			float cpuFrameMs = (g_simStart > g_lastPresentEnd && g_simStart < a_presentStart) ?
			                       static_cast<float>(Ms(a_presentStart - g_simStart)) :
			                       std::max(0.0f, static_cast<float>(Ms(a_presentStart - g_lastPresentEnd) - g_frame.reflexSleep));

			// Camera turn rate and player speed, per real frame.
			float yawRate = 0.0f, pitchRate = 0.0f, speed = 0.0f;
			if (auto* cam = RE::Main::WorldRootCamera()) {
				const auto& r = cam->world.rotate;
				const float fx = r.entry[0][0], fy = r.entry[1][0], fz = r.entry[2][0];
				const float yaw = std::atan2(fy, fx) * 57.29578f;
				const float pitch = std::asin(std::clamp(fz, -1.0f, 1.0f)) * 57.29578f;
				if (g_motionValid && frameMs > 0.0f) {
					float dy = yaw - g_lastYaw;
					dy -= 360.0f * std::round(dy / 360.0f);
					yawRate = std::abs(dy) * 1000.0f / frameMs;
					pitchRate = std::abs(pitch - g_lastPitch) * 1000.0f / frameMs;
				}
				g_lastYaw = yaw;
				g_lastPitch = pitch;
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const auto pos = player->GetPosition();
				if (g_motionValid && frameMs > 0.0f)
					speed = pos.GetDistance(g_lastPlayerPos) * 1000.0f / frameMs;
				g_lastPlayerPos = pos;
			}
			g_motionValid = true;

			{
				auto& e = g_hist[g_histHead];
				e = {};
				e.t = now;
				e.cpuMs = cpuFrameMs;
				e.gpuIndex = a_gpuFrameIndex;
				g_histHead = (g_histHead + 1) % kHist;
				g_histCount = std::min(g_histCount + 1, kHist);
			}

			const uint32_t shaderAsync = g_shaderAsync.exchange(0, std::memory_order_relaxed);
			const uint32_t shaderSync = g_shaderSync.exchange(0, std::memory_order_relaxed);
			const uint32_t cellAttach = g_cellAttach.exchange(0, std::memory_order_relaxed);
			const uint32_t cellDetach = g_cellDetach.exchange(0, std::memory_order_relaxed);

			if (g_recording) {
				Row r;
				r.cpuFrameMs = cpuFrameMs;
				r.utilityDraws = g_frame.utilityDraws;
				r.firstSeen = g_frame.firstSeen;
				r.shaderAsync = shaderAsync;
				r.shaderSync = shaderSync;
				r.cellAttach = cellAttach;
				r.cellDetach = cellDetach;
				r.yawRate = yawRate;
				r.pitchRate = pitchRate;
				r.playerSpeed = speed;
				g_pendingRow = r;
				g_hasPendingRow = true;
			}

			if (frameMs > 0.0f && frameMs < 1000.0f) {
				auto ema = [](float& s, float v) { s = s * 0.95f + v * 0.05f; };
				auto& s = g_smoothed;
				if (!s.valid) {
					s = {};
					s.valid = true;
					s.frameMs = frameMs;
				}
				ema(s.frameMs, frameMs);
				ema(s.presentWaitMs, presentWait);
				ema(s.reflexSleepMs, static_cast<float>(g_frame.reflexSleep));
				for (int i = 0; i < kCalls; ++i)
					ema(s.driverMs[i], static_cast<float>(g_frame.driver[i]));
				ema(s.driverTotalMs, static_cast<float>(g_frame.driverTotal));
				ema(s.ownDrawHookMs, static_cast<float>(g_frame.ownDrawHook));
				ema(s.ownSetupGeometryMs, static_cast<float>(g_frame.ownSetupGeometry));
				ema(s.permutationUploadMs, static_cast<float>(g_frame.permutationUpload));
				ema(s.draws, static_cast<float>(g_frame.draws));
				ema(s.lightingDraws, static_cast<float>(g_frame.lightingDraws));
				s.ownPerDrawUs = s.draws > 0.5f ? (s.ownDrawHookMs + s.ownSetupGeometryMs) * 1000.0f / s.draws : 0.0f;
			}

			if (g_recording) {
				Row r = g_hasPendingRow ? g_pendingRow : Row{};
				g_hasPendingRow = false;
				r.tMs = Ms(now - g_recordStart);
				r.frameMs = frameMs;
				r.presentWaitMs = presentWait;
				r.reflexSleepMs = static_cast<float>(g_frame.reflexSleep);
				for (int i = 0; i < kCalls; ++i)
					r.driver[i] = static_cast<float>(g_frame.driver[i]);
				r.ownDrawHookMs = static_cast<float>(g_frame.ownDrawHook);
				r.ownSetupGeometryMs = static_cast<float>(g_frame.ownSetupGeometry);
				r.permutationUploadMs = static_cast<float>(g_frame.permutationUpload);
				r.draws = g_frame.draws;
				r.lightingDraws = g_frame.lightingDraws;
				r.fgPresented = g_frame.fgPresented;
				r.grassApplyMs = g_frame.grassApplyMs;
				r.gpuFrameIndex = a_gpuFrameIndex;
				r.reflexFrame = globals::features::upscaling.streamline.frameTokenIndex;
				g_rows.push_back(r);
				if ((++g_reflexPoll & 15) == 0)
					PollReflex();
				if (now >= g_recordEnd)
					StopRecording();
			}
		}

		g_frame = {};
		g_lastPresentEnd = now;
		g_active = WantedThisFrame() && g_frequency != 0;
		if (!g_active)
			g_lastPresentEnd = 0;
	}

	void StartRecording(float a_seconds)
	{
		if (g_recording || g_frequency == 0)
			return;
		g_rows.clear();
		g_rows.reserve(static_cast<size_t>(std::clamp(a_seconds, 1.0f, 600.0f) * 250.0f));
		g_reflex.clear();
		g_recordStart = Now();
		g_recordEnd = g_recordStart + static_cast<int64_t>(static_cast<double>(std::clamp(a_seconds, 1.0f, 600.0f)) * static_cast<double>(g_frequency));
		g_recording = true;
		logger::info("[FrameTrace] Recording {:.0f} s", a_seconds);
	}

	void StopRecording()
	{
		if (!g_recording)
			return;
		g_recording = false;
		PollReflex();
		std::thread(WriteCsv, std::move(g_rows), std::move(g_reflex)).detach();
		g_rows = {};
		g_reflex = {};
	}

	float RecordingSecondsLeft()
	{
		if (!g_recording)
			return 0.0f;
		return static_cast<float>(std::max<double>(0.0, Ms(g_recordEnd - Now()) / 1000.0));
	}

	std::string LastFile()
	{
		std::scoped_lock lk(g_fileMutex);
		return g_lastFile;
	}

	Smoothed GetSmoothed() { return g_smoothed; }

	nlohmann::json ToJson()
	{
		const auto s = g_smoothed;
		nlohmann::json j;
		j["valid"] = s.valid;
		j["note"] = "Smoothed (0.95 EMA). driver_* = CPU time blocked inside D3D11 calls on the immediate context; own_* = our per-draw hook work without the D3D11 time inside it.";
		j["frame_ms"] = s.frameMs;
		j["present_wait_ms"] = s.presentWaitMs;
		j["reflex_sleep_ms"] = s.reflexSleepMs;
		j["driver_draw_ms"] = s.driverMs[0];
		j["driver_map_ms"] = s.driverMs[1];
		j["driver_upload_ms"] = s.driverMs[2];
		j["driver_dispatch_ms"] = s.driverMs[3];
		j["driver_flush_ms"] = s.driverMs[4];
		j["driver_total_ms"] = s.driverTotalMs;
		j["own_draw_hook_ms"] = s.ownDrawHookMs;
		j["own_setup_geometry_ms"] = s.ownSetupGeometryMs;
		j["perm_cb_upload_ms"] = s.permutationUploadMs;
		j["draws"] = s.draws;
		j["lighting_draws"] = s.lightingDraws;
		j["own_per_draw_us"] = s.ownPerDrawUs;
		return j;
	}
}
