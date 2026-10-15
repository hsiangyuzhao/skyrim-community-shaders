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
		// (45n) Early Prep used this frame, its frame-start CPU ms; render-thread ms / count of the last retired-object
		// collect (inside rt_x_collect); the release thread's last batch ms; our queue idle before the trace (GPU ms);
		// character shapes reused because their bones did not change.
		int early = 0;
		float earlyMs = -1, retireMs = -1, workerReleaseMs = -1, queueGapGpuMs = -1;
		uint32_t retiredCount = 0, stillReused = 0;
		// (46b) Point-light shadows: K (0 = off / not traced), sources traced, candidates after the rules (before merging and the
		// cap), rays per frame (counter read back a few frames late, -1 = not known), GPU ms of the trace (D3D12) and of the
		// denoiser (D3D11), CPU ms of the selection.
		int plK = 0;
		uint32_t plSources = 0, plCandidates = 0;
		int64_t plRays = -1;
		float plGpuMs = -1, plDenoiseGpuMs = -1, plCpuMs = -1;
		// (46c) Interiors: 0 outdoors / off, 1 indoors capturing, 2 indoors traced; window beams traced; engine shadow maps
		// left out this frame; shadow mask channels written by RT; interior shapes captured.
		int rtInterior = 0;
		uint32_t plBeams = 0, smSkipped = 0, maskChannels = 0, interiorShapes = 0;
		uint32_t plCarried = 0;  // (46e) sources carried by a traced character (its own body not tested)
		// (46f) RTAO: D3D12 GPU ms of its rays, D3D11 filter + composite, the game queue's wait for its second fence (-1 = not run),
		// rays per frame (read back a few frames late; -1 = not known, 0 = not traced).
		float rtaoGpuMs = -1.0f, rtaoFilterGpuMs = -1.0f, rtaoSyncGpuMs = -1.0f;
		int64_t rtaoRays = 0;
		int rtaoIndoors = -1, rtaoMode = -1;  // (46h) this frame's RTAO: 1 indoors / 0 outdoors, its mode (0 keep contact AO, 1 darker); -1 = not traced
		// (46g) Transitions and the pools: black screen (Fader Menu after a loading screen), ms since the loading screen closed
		// (-1 = no transition), hand-off this frame, items held back (ready / not ready); Early Prep call by call; the
		// parts taken out of rt_x_collect / tlas / record; pool upkeep ms, blocks made on the render thread, MB reserved,
		// allocations that waited for a block, the pool thread's last block ms.
		int black = 0, handOff = 0;
		float sinceCloseMs = -1.0f;
		int heldReady = -1, heldPending = -1;
		float eCollect = -1, eTimestamps = -1, eReset = -1, eBuild = -1, eSkin = -1, eClose = -1, eExecute = -1;
		float xTimestamps = -1, xEnsure = -1, xPublish = -1, xAo = -1;
		float poolMs = -1, poolMb = -1, poolWorkerMs = -1;
		uint32_t poolSyncBlocks = 0, poolAllocFails = 0;
		// (46g) Point-light selection: slot changes, lights merged, rejected by radius / by intensity, kept by the hysteresis,
		// pixels whose top-K source set changed (GPU, a few frames late; -1 = not known), window beams found implausible.
		uint32_t plSlotChanges = 0, plMerged = 0, plRejRadius = 0, plRejIntensity = 0, plKept = 0, plBeamsExcluded = 0;
		int64_t plChurn = -1;
		// (46k) This frame's RT work came from the loading pump (a loading-screen frame, no world render), its CPU ms (-1 = not run).
		int pump = 0;
		float pumpMs = -1.0f;
		// (46m) Characters' CPU step by step (ms, -1 = not run): actors chosen, per-actor reads (scan / check + bone matrices, on the
		// read lanes), captures / retires, still-pose commit, caster filter, CollectDraws; bone matrices read, actors scanned in full,
		// lanes, actors, shapes. Skinning: merged BLASes in the TLAS, built / refit, character TLAS instances, skinning dispatches.
		float chCandidates = -1, chReads = -1, chCapture = -1, chCommit = -1, chFilter = -1, chCollect = -1;
		uint32_t chBones = 0, chRescans = 0, chLanes = 0, chActors = 0, chShapes = 0;
		uint32_t skinGroups = 0, skinGroupBuilds = 0, skinGroupRefits = 0, skinInstances = 0, skinDispatches = 0;
		// (46m) Split Trace Sync this frame (1 split, 0 not, -1 not traced); the game queue's wait for the sun / for the point lights (GPU ms).
		int syncSplit = -1;
		float syncSunGpuMs = -1, syncPlGpuMs = -1;
	};
	void OnRayTracingDetail(const RayTracingDetail& a_detail);

	/// (45o) Light Census per-frame numbers (only filled while a trace records; -1 = not known).
	struct LightStats
	{
		int total = -1;       ///< lights in LLF's buffer
		int shadow = -1;      ///< of which shadow-casting
		int fill = -1;        ///< fill-light estimate: placed light without a mesh, radius >= 512, no shadow
		int player = -1;      ///< attached to the player
		int maxCluster = -1;  ///< most lights in one cluster (from a read-back a few frames old)
		float over4Share = -1.0f;  ///< share of screen tiles whose busiest cluster has > 4 lights (same read-back)
		float llfGpuMs = -1.0f;    ///< LLF cluster build + culling, last GPU sample
	};
	void OnLights(const LightStats& a_stats);

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
	/// (46n) One GpuPassTimers interval's GPU ms, for the cs_passes split of the row of frame a_frameId (EngineTiming::FrameId()).
	void OnPassGpuSample(uint64_t a_frameId, int a_bucket, float a_ms);

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
