#pragma once

#include <cstdint>
#include <string>

/**
 * @brief (44e, D2) Per-frame CPU wait split and the frame-time trace recorder.
 *
 * Measurement only: nothing here changes what the game or the GPU does.
 *
 * Per real frame it adds up, on the render thread:
 *  - time blocked inside D3D11 calls on the immediate context (draws, Map / Unmap,
 *    UpdateSubresource and copies, dispatches, Flush), timed with QPC around the call;
 *  - our own per-draw hook work (State::Draw and the Lighting SetupGeometry hook), with the
 *    D3D11 time inside it taken out, and the permutation constant-buffer upload on its own;
 *  - the wait inside Present and inside Reflex sleep.
 *
 * The overlay and the F12 JSON show the smoothed split; the trace recorder writes every real
 * frame of an N-second window to a CSV next to the perf JSONs, with the GPU frame time,
 * DLSS-G's presented-frame count, the Reflex latency report and the grass load time.
 *
 * Cost: two QPC reads per timed D3D11 call while active (overlay on screen or recording) and
 * "Driver wait timing" is on; a single branch otherwise.
 */
namespace Util::FrameTrace
{
	enum class DriverCall : int
	{
		Draw,      ///< Draw*, DrawIndexed*, the indirect draws
		Map,       ///< Map + Unmap
		Upload,    ///< UpdateSubresource, CopyResource, CopySubresourceRegion
		Dispatch,  ///< Dispatch, DispatchIndirect
		Flush,
		Count
	};

	/// Installs the D3D11 immediate-context timing hooks (call once, after the device exists).
	void InstallContextHooks(ID3D11DeviceContext* a_context);

	/// True while D3D11 calls are being timed this frame.
	bool Active();
	/// True while a trace is being recorded (the GPU timers stay on for it).
	bool IsRecording();

	// ---- collection points -------------------------------------------------------------------

	/// Our own hook work, a QPC interval measured by the caller (State::Draw, SetupGeometry);
	/// the D3D11 time accrued inside it is taken out here. a_lighting: a Lighting draw.
	int64_t Now();
	double DriverMsSoFar();
	/// a_type: the draw's RE::BSShader::Type (or -1).
	void AddOwnDrawHook(int64_t a_start, double a_driverAtStart, int a_type);
	/// Lighting SetupGeometry: counts geometries seen for the first time (cheap set insert while active).
	void OnLightingGeometry(const void* a_geometry);
	/// Shader cache: a permutation missing from the cache this frame (queued async / compiled now).
	void OnShaderMiss(bool a_async);
	/// Main::Update after the Reflex sleep: the CPU frame starts here.
	void OnSimStart();
	void AddOwnSetupGeometry(int64_t a_start, double a_driverAtStart);
	void AddPermutationUpload(int64_t a_start);
	void OnReflexSleep(int64_t a_start);
	/// (45m) Main::Update: the frame-start pacer's wait (LatencyPacer), before the Reflex sleep.
	void OnPacerSleep(int64_t a_start);
	/// Bottom of the Present hook: closes this frame's row. a_presentStart: QPC before the real Present.
	void OnPresentEnd(int64_t a_presentStart, uint64_t a_gpuFrameIndex);
	/// DLSS-G's frames presented for this real frame (DX12SwapChain::Present).
	void OnFramesPresented(uint32_t a_count);
	/// Grass Optimizations' unsmoothed load work this frame.
	void OnGrassApply(float a_ms);
	/// (45) Ray Tracing this frame: D3D12 GPU ms of the last completed submission, CPU ms, BLAS builds (-1 = off).
	void OnRayTracing(float a_gpuMs, float a_cpuMs, uint32_t a_builds);
	/// (45j) Ray Tracing's CPU split this frame (ms; -1 = not run) and its counters.
	struct RayTracingDetail
	{
		float scene = -1, gridDiff = -1, gather = -1, registration = -1, objects = -1, objectCapture = -1, instances = -1, missed = -1;
		float characters = -1, execute = -1, executeBuild = -1, executeSkin = -1, d3d11 = -1, endFrame = -1, release = -1;
		uint32_t recaptures = 0, objectsCaptured = 0, missedJudged = 0, releasePending = 0, tlasInstances = 0, cellsGathering = 0;
		// (45k) D3D12 record + submit, call by call (Backend::ExecuteSplit; -1 = not run), denoiser mode,
		// phase (0 play, 1 loading screen, 2 warm-up after it), last GPU samples of the denoiser and Screen Space Shadows.
		float xCollect = -1, xSignal11 = -1, xFlush11 = -1, xWait12 = -1, xReset = -1, xTlas = -1, xAlpha = -1, xRecord = -1, xExecute = -1, xSignal12 = -1, xWait11 = -1;
		int denoiser = -1, phase = 0;
		float denoiseGpuMs = -1, sssGpuMs = -1;
		// (45l) GPU per pass (last samples, a few frames old; -1 = not run): tiles, H+V filter, SIGMA (prep + NRD), combine,
		// the D3D11 stall at the wait for the trace; on the D3D12 queue builds/skinning/TLAS and the trace itself.
		float tileGpuMs = -1, filterGpuMs = -1, sigmaGpuMs = -1, combineGpuMs = -1, syncGpuMs = -1, prepGpuMs = -1, traceGpuMs = -1;
		int quality = -1, denoiserEffective = -1;
		uint32_t skinDraws = 0, skinReused = 0;
	};
	void OnRayTracingDetail(const RayTracingDetail& a_detail);

	/// (45j) Where the player is, for profiles: worldspace and cell (editor ID, else name, else form
	/// ID), interior, position (rounded), weather, game hour.
	struct Location
	{
		std::string world, cell, weather;
		bool interior = false;
		int x = 0, y = 0, z = 0;
		float hour = -1.0f;
	};
	Location CurrentLocation();
	/// A short file-name-safe tag of a location (worldspace or interior cell).
	std::string LocationTag(const Location& a_location);
	/// A raw GPU frame sample (GpuFrameTimer), for the row of that frame.
	void OnGpuSample(uint64_t a_gpuFrameIndex, float a_workMs, float a_elapsedMs);

	// ---- recorder ------------------------------------------------------------------------------

	/// Starts an a_seconds recording (no-op while one runs). a_stamp: file-name stamp, empty = now.
	void StartRecording(float a_seconds);
	void StopRecording();
	float RecordingSecondsLeft();
	/// Name of the last CSV written (empty until one is), and a short status line.
	std::string LastFile();

	// ---- display -------------------------------------------------------------------------------

	struct Smoothed
	{
		bool valid = false;
		float frameMs = 0.0f;         ///< present-to-present, CPU clock
		float presentWaitMs = 0.0f;
		float reflexSleepMs = 0.0f;
		float driverMs[static_cast<int>(DriverCall::Count)]{};
		float driverTotalMs = 0.0f;
		float ownDrawHookMs = 0.0f;   ///< State::Draw, without the D3D11 time inside it
		float ownSetupGeometryMs = 0.0f;
		float permutationUploadMs = 0.0f;
		float draws = 0.0f;           ///< draws seen by State::Draw
		float lightingDraws = 0.0f;
		float ownPerDrawUs = 0.0f;    ///< (own draw hook + own SetupGeometry) / draws
		// (44f) Engine CPU split (Util::EngineTiming), smoothed and peak (decaying max).
		bool engineValid = false;
		float engineMs[13]{};
		float enginePeakMs[13]{};
		float engineInputMs = 0.0f;
		uint32_t engineTopCallId = 0;     ///< slowest game-update call of the last frame over 5 ms
		uint32_t engineTopCallOffset = 0;
		float engineTopCallMs = 0.0f;
		float engineDtMs = 0.0f;
		float createMs = 0.0f;
		// (44f, A) Reflex: newest reported latency (simulation start to GPU end), smoothed.
		float reflexLatencyMs = -1.0f;
	};
	Smoothed GetSmoothed();

	/// Real-frame CPU vs GPU series for the overlay (last a_seconds). CPU frame = simulation start to
	/// Present start (all CPU submission work, driver blocking included); GPU frame = GPU work span.
	struct SeriesStats
	{
		int frames = 0;
		float cpuAvg = 0, cpuLow1 = 0, cpuMax = 0;  ///< low1 = the 99th-percentile frame time (1% low)
		int gpuFrames = 0;
		float gpuAvg = 0, gpuLow1 = 0, gpuMax = 0;
		float cpuLimitedPct = 0;  ///< frames where CPU frame > GPU frame
	};
	SeriesStats GetSeriesStats(float a_seconds);
	/// Copies the last up-to-a_count CPU and GPU samples (oldest first; GPU -1 = none) for plotting.
	void GetSeries(std::vector<float>& o_cpu, std::vector<float>& o_gpu, int a_count);

	/// The split as JSON for the F12 snapshot.
	nlohmann::json ToJson();
}
