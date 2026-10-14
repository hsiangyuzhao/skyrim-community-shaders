#include "Utils/FrameTrace.h"

#include "Features/PerformanceOverlay.h"
#include "Features/RayTracing.h"
#include "Features/Upscaling.h"
#include "Menu.h"
#include "State.h"
#include "BuildLabel.h"
#include "Utils/EngineTiming.h"
#include "Utils/GpuTimers.h"
#include "Utils/LatencyPacer.h"

#include <intrin.h>
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
			double pacerSleep = 0.0;  // (45m) frame-start pacer (predictive sleep + hard bound)
			uint32_t draws = 0;
			uint32_t lightingDraws = 0;
			uint32_t utilityDraws = 0;
			uint32_t firstSeen = 0;
			uint32_t fgPresented = 0;
			float grassApplyMs = -1.0f;
			float rtGpuMs = -1.0f;  // (45) Ray Tracing: D3D12 GPU ms (last completed), CPU ms, BLAS builds
			float rtCpuMs = -1.0f;
			uint32_t rtBuilds = 0;
			RayTracingDetail rt{};  // (45j)
			// (45j) Map attribution: calls, slow calls (> 0.5 ms), the slowest one's resource and caller.
			uint32_t mapCalls = 0;
			uint32_t mapSlow = 0;
			double mapWorstMs = 0.0;
			const void* mapWorstCaller = nullptr;
			char mapWorstResource[96]{};
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
		uint32_t g_frameCounter = 0;

		// ---- recorder ----
		struct Row
		{
			double tMs = 0.0;
			float frameMs = 0.0f;
			float presentWaitMs = 0.0f;
			float reflexSleepMs = 0.0f;
			float pacerSleepMs = 0.0f;  // (45m)
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
			float rtGpuMs = -1.0f;
			float rtCpuMs = -1.0f;
			uint32_t rtBuilds = 0;
			RayTracingDetail rt{};  // (45j)
			uint32_t mapCalls = 0, mapSlow = 0;
			float mapWorstMs = 0.0f;
			std::string mapWorstResource, mapWorstCaller;
			std::shared_ptr<const Location> location;  // (45j) shared while it does not change
			std::string cpuTop;  // (45j) our costliest CPU features this frame
			uint64_t gpuFrameIndex = 0;
			float gpuWorkMs = -1.0f;
			float gpuElapsedMs = -1.0f;
			uint32_t reflexFrame = UINT32_MAX;
			// (44f) Engine CPU split, engine time step, device Create* calls.
			EngineTiming::Frame engine;
			bool reflexMarkersAtRenderStart = false;
			// (45m) Reflex / pacer state for this frame.
			LatencyPacer::FrameInfo pacer{};
			bool reflexSleepOk = false;
			int lowLatencyAvailable = -1;
			int reflexModeApplied = -1;
			int reflexLimitApplied = -1;
			bool latencyFix = false;
		};

		struct ReflexTimes
		{
			uint64_t simStart, simEnd, submitStart, submitEnd, presentStart, presentEnd, driverStart, driverEnd, queueStart, queueEnd, gpuStart, gpuEnd;
			uint32_t gpuActiveUs, gpuFrameUs;  // (45m) what the driver counts as GPU work for the frame
		};

		bool g_recording = false;
		// (45j) Taken when a recording starts (the CSV is written on another thread).
		std::string g_headerLocation, g_headerRayTracing, g_headerTag, g_headerLatency;
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
					r.driverStartTime, r.driverEndTime, r.osRenderQueueStartTime, r.osRenderQueueEndTime, r.gpuRenderStartTime, r.gpuRenderEndTime,
					r.gpuActiveRenderTimeUs, r.gpuFrameTimeUs };
			}
		}

		/// (44f, A) Newest complete Reflex report: simulation start to GPU end, for the overlay line.
		void PollLatency()
		{
			auto& streamline = globals::features::upscaling.streamline;
			if (!streamline.reflexFunctionsReady)
				return;
			sl::ReflexState state{};
			if (!streamline.GetReflexState(state) || !state.latencyReportAvailable)
				return;
			const sl::ReflexReport* newest = nullptr;
			for (const auto& r : state.frameReport)
				if (r.simStartTime != 0 && r.gpuRenderEndTime > r.simStartTime && (!newest || r.frameID > newest->frameID))
					newest = &r;
			if (!newest)
				return;
			const float ms = static_cast<float>(static_cast<double>(newest->gpuRenderEndTime - newest->simStartTime) / 1000.0);
			auto& s = g_smoothed.reflexLatencyMs;
			s = s < 0.0f ? ms : s * 0.8f + ms * 0.2f;
		}

		void WriteCsv(std::vector<Row> a_rows, std::unordered_map<uint64_t, ReflexTimes> a_reflex, std::string a_location, std::string a_rayTracing, std::string a_tag,
			std::string a_latency)
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
				const std::string name = a_tag.empty() ? std::format("trace-{}.csv", stamp) : std::format("trace-{}-{}.csv", stamp, a_tag);
				path /= name;

				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out)
					return;
				out << "# Community Shaders frame trace (" << BuildLabel::kBuild << "). One row per real (rendered) frame. Times in ms unless noted.\n";
				out << "# cpu_busy = frame - present_wait - reflex_sleep (includes driver_*). gpu_* come from GPU timestamps (-1 = no sample).\n";
				out << "# reflex_* in us relative to reflex_sim_start (-1 = no report). fg_presented = frames DLSS-G showed for this frame (0 = not generating).\n";
				out << "# cpu_frame = simulation start (after Reflex sleep) to Present start: all CPU submission work, driver blocking included.\n";
				out << "# eng_* (44f): the main thread from one Present end to the next, split into engine segments; they add up to frame_ms (-1 = probes not installed).\n";
				out << "#   eng_logic = the game update (AI, scripts, physics, animation, ...); eng_top_call = Address Library ID@offset in Main::Update of its slowest call,\n";
				out << "#   eng_calls_over_1ms = every such call over 1 ms (id@offset:ms;...). eng_render_other = rendering minus the named phases (culling, scene traversal).\n";
				out << "# eng_dt_ms / eng_real_dt_ms = the engine's time step (BSTimer). dev_create_* = time inside D3D11 device Create* calls (main thread / others).\n";
				out << "# reflex_markers = 1 when Reflex Markers at Render Start was on for that frame.\n";
				out << "# (45m) reflex_gpu_active_ms / reflex_gpu_frame_ms = GPU time the driver counts for the frame (Reflex sees only the D3D12 interop device).\n";
				out << "#   reflex_sleep_ok = slReflexSleep succeeded; reflex_lowlat = lowLatencyAvailable (slReflexGetState, -1 = not queried); reflex_mode / reflex_limit_fps = applied.\n";
				out << "#   latency_fix = 45m Reflex fix on; pacer_* = frame-start pacer: on, sleep (predictive), cap_wait (hard bound: frame before the previous one),\n";
				out << "#   in_flight = frames the GPU had not finished at frame start, pred = predicted ms until the previous frame finishes, lead = CPU ms reserved (p75),\n";
				out << "#   gpu_period = median ms between GPU frame completions. pacer_sleep_ms = the whole pacer wait (both parts); cpu_busy also excludes it.\n";
				out << "# (45m) latency at start: " << a_latency << "\n";
				out << "# (45j) location at start: " << a_location << "\n";
				out << "# (45j) ray tracing at start: " << a_rayTracing << "\n";
				out << "# (45j) rt_* = Ray Tracing CPU split (ms, -1 = not run): scene update (grid diff, cell capture, filter registration, objects of which new\n";
				out << "#   captures), TLAS instances, missed-caster check, characters, D3D12 record+submit (of which BLAS creation, skinning), D3D11 passes,\n";
				out << "#   end of frame, engine references dropped; counts: recaptures, objects captured, misses examined, references waiting, TLAS instances,\n";
				out << "#   cells being captured. map_* = Map calls this frame, those over 0.5 ms, the slowest (ms, resource, caller module+offset).\n";
				out << "#   loc_* / pos_* / weather / game_hour = where the player was on that frame.\n";
				out << "# (45k) rt_denoiser = shadow denoiser (0 off, 1 temporal 45j, 2 penumbra, 3 DLSS RR); rt_phase = 0 play, 1 loading screen, 2 warm-up after it.\n";
				out << "#   rt_x_* = D3D12 record + submit call by call: context choice, D3D11 Signal, D3D11 Flush, D3D12 queue Wait, allocator/list Reset,\n";
				out << "#   TLAS instances, alpha records, recording, ExecuteCommandLists, D3D12 Signal, D3D11 Wait (with rt_exec_build/skin they add up to rt_exec).\n";
				out << "#   rt_denoise_gpu_ms / sss_gpu_ms = last GPU sample of the denoiser passes / Screen Space Shadows (a few frames old).\n";
				out << "# (45l) rt_denoiser also 4 = NRD SIGMA; rt_denoiser_eff = the one in effect (3 whenever Upscaling's Ray Reconstruction runs); rt_quality 0-4 = Very Low..Ultra, 5 Custom.\n";
				out << "#   rt_denoise_gpu_ms = tiles + filter + SIGMA; rt_tile/filter/sigma/combine_gpu_ms per pass; rt_sync_gpu_ms = the game queue standing still at the wait\n";
				out << "#   for the trace; rt_prep_gpu_ms / rt_trace_gpu_ms = D3D12 builds+skinning+TLAS / the trace dispatch (rt_gpu_ms = their sum); rt_skin_draws / _reused =\n";
				out << "#   character shapes traced / of which with last frame's skinning (other actors at reduced quality).\n";
				out << "frame,t_ms,frame_ms,cpu_frame_ms,cpu_busy_ms,present_wait_ms,reflex_sleep_ms,"
					   "driver_draw_ms,driver_map_ms,driver_upload_ms,driver_dispatch_ms,driver_flush_ms,driver_total_ms,"
					   "own_draw_hook_ms,own_setup_geometry_ms,perm_cb_upload_ms,draws,lighting_draws,utility_draws,own_per_draw_us,"
					   "first_seen_geometry,shader_miss_async,shader_miss_sync,cell_attach,cell_detach,cam_yaw_dps,cam_pitch_dps,player_speed,"
					   "gpu_work_ms,gpu_elapsed_ms,fg_presented,grass_apply_ms,rt_gpu_ms,rt_cpu_ms,rt_blas_builds,"
					   "reflex_frame,reflex_sim_end,reflex_submit_start,reflex_submit_end,reflex_present_start,reflex_present_end,"
					   "reflex_driver_start,reflex_driver_end,reflex_queue_start,reflex_queue_end,reflex_gpu_start,reflex_gpu_end,reflex_latency_ms,reflex_gpu_active_ms,reflex_gpu_frame_ms,"
					   "reflex_markers,reflex_sleep_ok,reflex_lowlat,reflex_mode,reflex_limit_fps,latency_fix,pacer_on,pacer_sleep_ms,pacer_cap_wait_ms,pacer_in_flight,"
					   "pacer_pred_ms,pacer_lead_ms,pacer_gpu_period_ms";
				for (int s = 0; s < static_cast<int>(EngineTiming::Seg::Count); ++s)
					out << ",eng_" << EngineTiming::SegName(static_cast<EngineTiming::Seg>(s)) << "_ms";
				out << ",eng_input_ms,eng_top_call,eng_top_call_ms,eng_calls_over_1ms,eng_dt_ms,eng_real_dt_ms,dev_create_ms,dev_creates,dev_create_other_ms,dev_creates_other";
				out << ",rt_scene_ms,rt_grid_ms,rt_gather_ms,rt_register_ms,rt_objects_ms,rt_object_capture_ms,rt_instances_ms,rt_missed_ms,rt_chars_ms,rt_exec_ms,"
				       "rt_exec_build_ms,rt_exec_skin_ms,rt_d3d11_ms,rt_endframe_ms,rt_release_ms,rt_recaptures,rt_objects_new,rt_missed_n,rt_release_pending,"
				       "rt_tlas_instances,rt_cells_gathering,map_calls,map_slow,map_worst_ms,map_worst_res,map_worst_caller,loc_world,loc_cell,loc_interior,"
				       "pos_x,pos_y,pos_z,weather,game_hour,rt_denoiser,rt_phase,rt_x_collect_ms,rt_x_signal11_ms,rt_x_flush11_ms,rt_x_wait12_ms,rt_x_reset_ms,"
				       "rt_x_tlas_ms,rt_x_alpha_ms,rt_x_record_ms,rt_x_execute_ms,rt_x_signal12_ms,rt_x_wait11_ms,rt_denoise_gpu_ms,sss_gpu_ms,"
				       "rt_quality,rt_denoiser_eff,rt_tile_gpu_ms,rt_filter_gpu_ms,rt_sigma_gpu_ms,rt_combine_gpu_ms,rt_sync_gpu_ms,rt_prep_gpu_ms,rt_trace_gpu_ms,"
				       "rt_skin_draws,rt_skin_reused,cpu_top_features\n";
				auto extraColumns = [](const Row& a_row) {
					const auto& t = a_row.rt;
					std::string s = std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{}", t.scene,
						t.gridDiff, t.gather, t.registration, t.objects, t.objectCapture, t.instances, t.missed, t.characters, t.execute, t.executeBuild, t.executeSkin,
						t.d3d11, t.endFrame, t.release, t.recaptures, t.objectsCaptured, t.missedJudged, t.releasePending, t.tlasInstances, t.cellsGathering);
					s += std::format(",{},{},{:.3f},{},{}", a_row.mapCalls, a_row.mapSlow, a_row.mapWorstMs, a_row.mapWorstResource, a_row.mapWorstCaller);
					if (a_row.location) {
						const auto& l = *a_row.location;
						s += std::format(",{},{},{},{},{},{},{},{:.2f}", l.world, l.cell, l.interior ? 1 : 0, l.x, l.y, l.z, l.weather, l.hour);
					} else {
						s += ",,,,,,,,";
					}
					s += std::format(",{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}", t.denoiser, t.phase, t.xCollect, t.xSignal11,
						t.xFlush11, t.xWait12, t.xReset, t.xTlas, t.xAlpha, t.xRecord, t.xExecute, t.xSignal12, t.xWait11, t.denoiseGpuMs, t.sssGpuMs);
					s += std::format(",{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{}", t.quality, t.denoiserEffective, t.tileGpuMs, t.filterGpuMs, t.sigmaGpuMs,
						t.combineGpuMs, t.syncGpuMs, t.prepGpuMs, t.traceGpuMs, t.skinDraws, t.skinReused);
					s += "," + a_row.cpuTop + "\n";
					return s;
				};
				auto engineColumns = [](const Row& a_row) {
					const auto& e = a_row.engine;
					const auto& p = a_row.pacer;
					std::string s = std::format(",{},{},{},{},{},{},{},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f}", a_row.reflexMarkersAtRenderStart ? 1 : 0, a_row.reflexSleepOk ? 1 : 0,
						a_row.lowLatencyAvailable, a_row.reflexModeApplied, a_row.reflexLimitApplied, a_row.latencyFix ? 1 : 0, p.enabled ? 1 : 0, a_row.pacerSleepMs, p.capWaitMs,
						p.inFlight, p.predictedMs, p.leadMs, p.gpuPeriodMs);
					for (int i = 0; i < static_cast<int>(EngineTiming::Seg::Count); ++i)
						s += e.valid ? std::format(",{:.3f}", e.ms[i]) : std::string(",-1");
					s += e.valid ? std::format(",{:.3f},{}@{:X},{:.3f},{}", e.inputMs, e.topCallId, e.topCallOffset, e.topCallMs, e.callsOver1ms) : std::string(",-1,-1,-1,");
					s += std::format(",{:.3f},{:.3f},{:.3f},{},{:.3f},{}", e.engineDtMs, e.engineRealDtMs, e.createMs, e.creates, e.createOtherMs, e.createsOther);
					return s;
				};
				uint32_t index = 0;
				for (const auto& r : a_rows) {
					float driverTotal = 0.0f;
					for (float d : r.driver)
						driverTotal += d;
					const float busy = std::max(0.0f, r.frameMs - r.presentWaitMs - r.reflexSleepMs - r.pacerSleepMs);
					const float perDraw = r.draws ? (r.ownDrawHookMs + r.ownSetupGeometryMs) * 1000.0f / static_cast<float>(r.draws) : 0.0f;
					out << std::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{:.3f},{},{},{},{},{},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f},{},",
						index++, r.tMs, r.frameMs, r.cpuFrameMs, busy, r.presentWaitMs, r.reflexSleepMs,
						r.driver[0], r.driver[1], r.driver[2], r.driver[3], r.driver[4], driverTotal,
						r.ownDrawHookMs, r.ownSetupGeometryMs, r.permutationUploadMs, r.draws, r.lightingDraws, r.utilityDraws, perDraw,
						r.firstSeen, r.shaderAsync, r.shaderSync, r.cellAttach, r.cellDetach, r.yawRate, r.pitchRate, r.playerSpeed,
						r.gpuWorkMs, r.gpuElapsedMs, r.fgPresented, r.grassApplyMs, r.rtGpuMs, r.rtCpuMs, r.rtBuilds);
					auto it = r.reflexFrame != UINT32_MAX ? a_reflex.find(r.reflexFrame) : a_reflex.end();
					if (it == a_reflex.end()) {
						out << (r.reflexFrame == UINT32_MAX ? std::string("-1") : std::to_string(r.reflexFrame)) << ",-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1" << engineColumns(r) << extraColumns(r);
						continue;
					}
					const auto& x = it->second;
					auto rel = [&](uint64_t v) { return v >= x.simStart && v != 0 ? static_cast<int64_t>(v - x.simStart) : -1; };
					out << std::format("{},{},{},{},{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f}", r.reflexFrame, rel(x.simEnd), rel(x.submitStart), rel(x.submitEnd), rel(x.presentStart),
						rel(x.presentEnd), rel(x.driverStart), rel(x.driverEnd), rel(x.queueStart), rel(x.queueEnd), rel(x.gpuStart), rel(x.gpuEnd),
						x.gpuEnd > x.simStart ? static_cast<double>(x.gpuEnd - x.simStart) / 1000.0 : -1.0, x.gpuActiveUs / 1000.0, x.gpuFrameUs / 1000.0)
						<< engineColumns(r) << extraColumns(r);
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
		/// (45j) Remembers the slowest Map of the frame: what was mapped, how, and by whom.
		void NoteMap(ID3D11Resource* a_resource, D3D11_MAP a_type, UINT a_flags, double a_ms, const void* a_caller)
		{
			++g_frame.mapCalls;
			if (a_ms > 0.5)
				++g_frame.mapSlow;
			if (a_ms <= g_frame.mapWorstMs || a_ms < 0.1)
				return;
			g_frame.mapWorstMs = a_ms;
			g_frame.mapWorstCaller = a_caller;
			static constexpr const char* kTypes[] = { "?", "read", "write", "read-write", "write-discard", "write-no-overwrite" };
			const char* type = static_cast<unsigned>(a_type) < 6 ? kTypes[a_type] : "?";
			const char* noWait = (a_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) ? " no-wait" : "";
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			if (a_resource)
				a_resource->GetType(&dimension);
			if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
				D3D11_BUFFER_DESC d{};
				static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&d);
				std::snprintf(g_frame.mapWorstResource, sizeof(g_frame.mapWorstResource), "buffer %u B usage %u bind 0x%X %s%s", d.ByteWidth,
					static_cast<unsigned>(d.Usage), d.BindFlags, type, noWait);
			} else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				D3D11_TEXTURE2D_DESC d{};
				static_cast<ID3D11Texture2D*>(a_resource)->GetDesc(&d);
				std::snprintf(g_frame.mapWorstResource, sizeof(g_frame.mapWorstResource), "tex2d %ux%u fmt %u usage %u misc 0x%X %s%s", d.Width, d.Height,
					static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Usage), d.MiscFlags, type, noWait);
			} else {
				std::snprintf(g_frame.mapWorstResource, sizeof(g_frame.mapWorstResource), "resource dim %d %s", static_cast<int>(dimension), type);
			}
		}

		/// Module + offset of a code address ("SkyrimSE.exe+1A2B3C").
		std::string DescribeAddress(const void* a_address)
		{
			if (!a_address)
				return {};
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address), &module) ||
				!module)
				return std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_address));
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* slash = std::strrchr(path, '\\');
			return std::format("{}+{:X}", slash ? slash + 1 : path, reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module));
		}

		struct Map
		{
			static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Resource* r, UINT s, D3D11_MAP m, UINT f, D3D11_MAPPED_SUBRESOURCE* o)
			{
				if (!g_active || This != globals::d3d::context)
					return func(This, r, s, m, f, o);
				const int64_t start = Now();
				HRESULT hr;
				{
					Timed<DriverCall::Map> t(This);
					hr = func(This, r, s, m, f, o);
				}
				NoteMap(r, m, f, Ms(Now() - start), _ReturnAddress());
				return hr;
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

	void OnPacerSleep(int64_t a_start)
	{
		if (g_active)
			g_frame.pacerSleep += Ms(Now() - a_start);
	}

	void OnReflexSleep(int64_t a_start)
	{
		if (g_active)
			g_frame.reflexSleep += Ms(Now() - a_start);
	}

	void OnFramesPresented(uint32_t a_count) { g_frame.fgPresented = a_count; }
	void OnGrassApply(float a_ms) { g_frame.grassApplyMs = a_ms; }
	void OnRayTracing(float a_gpuMs, float a_cpuMs, uint32_t a_builds)
	{
		g_frame.rtGpuMs = a_gpuMs;
		g_frame.rtCpuMs = a_cpuMs;
		g_frame.rtBuilds = a_builds;
	}

	void OnRayTracingDetail(const RayTracingDetail& a_detail) { g_frame.rt = a_detail; }

	namespace
	{
		std::string FormName(RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			const char* edid = a_form->GetFormEditorID();
			if (edid && *edid)
				return edid;
			const char* name = a_form->GetName();
			if (name && *name)
				return name;
			return std::format("{:08X}", a_form->GetFormID());
		}

		/// CSV-safe: no commas, quotes or line breaks.
		std::string CsvSafe(std::string a_text)
		{
			for (char& c : a_text)
				if (c == ',' || c == '"' || c == '\n' || c == '\r')
					c = ';';
			return a_text;
		}
	}

	Location CurrentLocation()
	{
		Location location;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return location;
		const auto position = player->GetPosition();
		location.x = static_cast<int>(std::lround(position.x));
		location.y = static_cast<int>(std::lround(position.y));
		location.z = static_cast<int>(std::lround(position.z));
		// Names only change with the cell / worldspace / weather: cached by pointer.
		static const void* lastCell = nullptr;
		static const void* lastWorld = nullptr;
		static const void* lastWeather = nullptr;
		static std::string cellName, worldName, weatherName;
		static bool interior = false;
		auto* cell = player->GetParentCell();
		if (cell != lastCell) {
			lastCell = cell;
			cellName = CsvSafe(FormName(cell));
			interior = cell && cell->IsInteriorCell();
		}
		auto* world = player->GetWorldspace();
		if (world != lastWorld) {
			lastWorld = world;
			worldName = CsvSafe(FormName(world));
		}
		auto* sky = RE::Sky::GetSingleton();
		auto* weather = sky ? sky->currentWeather : nullptr;
		if (weather != lastWeather) {
			lastWeather = weather;
			weatherName = CsvSafe(FormName(weather));
		}
		location.cell = cellName;
		location.world = worldName;
		location.weather = weatherName;
		location.interior = interior;
		if (auto* calendar = RE::Calendar::GetSingleton())
			location.hour = calendar->GetHour();
		return location;
	}

	std::string LocationTag(const Location& a_location)
	{
		const std::string& source = a_location.interior || a_location.world.empty() ? a_location.cell : a_location.world;
		std::string tag;
		for (const char c : source) {
			if (std::isalnum(static_cast<unsigned char>(c)))
				tag += c;
			if (tag.size() >= 24)
				break;
		}
		return tag;
	}

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

		// (44f) Close the engine CPU split for this frame (also resets it while inactive).
		const auto engine = EngineTiming::TakeFrame(wasActive && g_recording);

		if (wasActive && g_lastPresentEnd) {
			const float frameMs = static_cast<float>(Ms(now - g_lastPresentEnd));
			const float presentWait = static_cast<float>(Ms(now - a_presentStart));
			// CPU frame: simulation start (after the Reflex sleep) to Present start. Without a
			// simulation marker this frame, from the previous Present's end minus the sleep.
			float cpuFrameMs = (g_simStart > g_lastPresentEnd && g_simStart < a_presentStart) ?
			                       static_cast<float>(Ms(a_presentStart - g_simStart)) :
			                       std::max(0.0f, static_cast<float>(Ms(a_presentStart - g_lastPresentEnd) - g_frame.reflexSleep - g_frame.pacerSleep));

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
				// (44f) Engine CPU split: EMA, and a peak that decays over about two seconds.
				s.engineValid = engine.valid;
				if (engine.valid) {
					static_assert(static_cast<int>(EngineTiming::Seg::Count) == 13);
					for (int i = 0; i < static_cast<int>(EngineTiming::Seg::Count); ++i) {
						ema(s.engineMs[i], engine.ms[i]);
						s.enginePeakMs[i] = std::max(engine.ms[i], s.enginePeakMs[i] * 0.99f);
					}
					ema(s.engineInputMs, engine.inputMs);
					if (engine.topCallMs > 5.0f) {
						s.engineTopCallId = engine.topCallId;
						s.engineTopCallOffset = engine.topCallOffset;
						s.engineTopCallMs = engine.topCallMs;
					}
				}
				if (engine.engineDtMs >= 0.0f)
					ema(s.engineDtMs, engine.engineDtMs);
				ema(s.createMs, engine.createMs);
			}
			if ((g_frameCounter++ & 15) == 0)
				PollLatency();

			if (g_recording) {
				Row r = g_hasPendingRow ? g_pendingRow : Row{};
				g_hasPendingRow = false;
				r.tMs = Ms(now - g_recordStart);
				r.frameMs = frameMs;
				r.presentWaitMs = presentWait;
				r.reflexSleepMs = static_cast<float>(g_frame.reflexSleep);
				r.pacerSleepMs = static_cast<float>(g_frame.pacerSleep);
				for (int i = 0; i < kCalls; ++i)
					r.driver[i] = static_cast<float>(g_frame.driver[i]);
				r.ownDrawHookMs = static_cast<float>(g_frame.ownDrawHook);
				r.ownSetupGeometryMs = static_cast<float>(g_frame.ownSetupGeometry);
				r.permutationUploadMs = static_cast<float>(g_frame.permutationUpload);
				r.draws = g_frame.draws;
				r.lightingDraws = g_frame.lightingDraws;
				r.fgPresented = g_frame.fgPresented;
				r.grassApplyMs = g_frame.grassApplyMs;
				r.rtGpuMs = g_frame.rtGpuMs;
				r.rtCpuMs = g_frame.rtCpuMs;
				r.rtBuilds = g_frame.rtBuilds;
				r.rt = g_frame.rt;
				r.cpuTop = Util::CpuPassTimers::GetSingleton()->TopOfCurrentFrame(3);
				r.mapCalls = g_frame.mapCalls;
				r.mapSlow = g_frame.mapSlow;
				r.mapWorstMs = static_cast<float>(g_frame.mapWorstMs);
				if (g_frame.mapWorstMs > 0.0) {
					r.mapWorstResource = g_frame.mapWorstResource;
					for (char& c : r.mapWorstResource)
						if (c == ',')
							c = ';';
					r.mapWorstCaller = DescribeAddress(g_frame.mapWorstCaller);
				}
				{
					// (45j) Where the player is, shared between rows while nothing changes.
					static std::shared_ptr<const Location> last;
					auto here = CurrentLocation();
					if (!last || last->world != here.world || last->cell != here.cell || last->weather != here.weather || last->x != here.x ||
						last->y != here.y || last->z != here.z || std::abs(last->hour - here.hour) > 0.01f)
						last = std::make_shared<const Location>(std::move(here));
					r.location = last;
				}
				r.gpuFrameIndex = a_gpuFrameIndex;
				r.reflexFrame = globals::features::upscaling.streamline.frameTokenIndex;
				r.engine = engine;
				r.reflexMarkersAtRenderStart = globals::features::upscaling.WantsReflexMarkersAtRenderStart() && EngineTiming::ProbesInstalled();
				{
					auto& up = globals::features::upscaling;
					r.pacer = LatencyPacer::Last();
					r.reflexSleepOk = up.streamline.lastReflexSleepOk;
					r.lowLatencyAvailable = up.streamline.lastLowLatencyAvailable;
					r.reflexModeApplied = up.GetReflexModeApplied() == UINT_MAX ? -1 : static_cast<int>(up.GetReflexModeApplied());
					r.reflexLimitApplied = up.GetReflexFrameLimitApplied() == UINT_MAX ? -1 : static_cast<int>(up.GetReflexFrameLimitApplied());
					r.latencyFix = up.settings.reflexLatencyFix;
				}
				// (45j) A spike frame names its suspects in the log (at most one line a second).
				if (frameMs > 1.5f * g_smoothed.frameMs + 5.0f) {
					static int64_t lastSpikeLog = 0;
					if (Ms(now - lastSpikeLog) > 1000.0) {
						lastSpikeLog = now;
						logger::info("[FrameTrace] Spike {:.1f} ms (avg {:.1f}): our CPU features [{}], RT CPU {:.2f} ms (scene {:.2f}, chars {:.2f}, exec {:.2f}), "
									 "slowest Map {:.2f} ms ({} from {}), engine CS passes {:.2f} ms, device creates on other threads {:.2f} ms",
							frameMs, g_smoothed.frameMs, r.cpuTop, r.rtCpuMs, r.rt.scene, r.rt.characters, r.rt.execute, r.mapWorstMs, r.mapWorstResource,
							r.mapWorstCaller, engine.valid ? engine.ms[static_cast<int>(EngineTiming::Seg::OurPasses)] : -1.0f, engine.createOtherMs);
					}
				}
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
		// (44f) "First seen" geometry counts from the start of every recording.
		g_seenGeometry.clear();
		g_rows.reserve(static_cast<size_t>(std::clamp(a_seconds, 1.0f, 600.0f) * 250.0f));
		g_reflex.clear();
		g_recordStart = Now();
		g_recordEnd = g_recordStart + static_cast<int64_t>(static_cast<double>(std::clamp(a_seconds, 1.0f, 600.0f)) * static_cast<double>(g_frequency));
		g_recording = true;
		{
			const auto location = CurrentLocation();
			g_headerLocation = std::format("world {}, cell {}, {}, position {} {} {}, weather {}, game hour {:.2f}", location.world.empty() ? "-" : location.world,
				location.cell.empty() ? "-" : location.cell, location.interior ? "interior" : "exterior", location.x, location.y, location.z,
				location.weather.empty() ? "-" : location.weather, location.hour);
			g_headerTag = LocationTag(location);
			g_headerRayTracing = std::format("{}; build {}", globals::features::rayTracing.DescribeForProfile(), BuildLabel::kBuild);
			g_headerLatency = globals::features::upscaling.DescribeLatencyForTrace();
		}
		logger::info("[FrameTrace] Recording {:.0f} s", a_seconds);
	}

	void StopRecording()
	{
		if (!g_recording)
			return;
		g_recording = false;
		PollReflex();
		std::thread(WriteCsv, std::move(g_rows), std::move(g_reflex), g_headerLocation, g_headerRayTracing, g_headerTag, g_headerLatency).detach();
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
		if (s.engineValid) {
			nlohmann::json e;
			for (int i = 0; i < static_cast<int>(EngineTiming::Seg::Count); ++i)
				e[EngineTiming::SegName(static_cast<EngineTiming::Seg>(i))] = { { "ms", s.engineMs[i] }, { "peak_ms", s.enginePeakMs[i] } };
			e["input_ms"] = s.engineInputMs;
			e["last_slow_call"] = std::format("{}@{:X}", s.engineTopCallId, s.engineTopCallOffset);
			e["last_slow_call_ms"] = s.engineTopCallMs;
			j["engine_split"] = std::move(e);
		}
		j["engine_dt_ms"] = s.engineDtMs;
		j["device_create_ms"] = s.createMs;
		j["reflex_latency_ms"] = s.reflexLatencyMs;
		j["reflex_markers_at_render_start"] = globals::features::upscaling.WantsReflexMarkersAtRenderStart();
		j["reflex_latency_fix"] = globals::features::upscaling.settings.reflexLatencyFix;
		j["pacer_enabled"] = globals::features::upscaling.IsLatencyPacerEnabled();
		j["pacer_in_flight_now"] = LatencyPacer::Last().inFlight;
		j["pacer_gpu_period_ms"] = LatencyPacer::Last().gpuPeriodMs;
		return j;
	}
}
