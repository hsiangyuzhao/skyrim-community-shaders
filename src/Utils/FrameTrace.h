#pragma once

#include <cstdint>
#include <string>
#include <string_view>

struct ID3D11DeviceChild;  // (46p)
struct ID3D12Object;

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
	// ---- (46p) what the RT D3D12 prep submission did (the one whose GPU time is rt_prep_gpu_ms) ---------------------
	struct RtPrepWork
	{
		int valid = 0;                 ///< 1 = filled from the submission
		int early = 0;                 ///< recorded at the frame start (Early Prep)
		uint32_t builds = 0;           ///< static BLAS builds (cell chunks, objects, trees, alpha-tested)
		uint32_t buildsAlpha = 0;      ///< of which alpha-tested
		uint64_t buildTriangles = 0;
		uint64_t buildBytes = 0;       ///< BLAS results (before compaction)
		uint32_t compactions = 0;
		uint64_t compactBytes = 0;     ///< compacted copies written
		uint32_t skinUploads = 0;      ///< new character shapes uploaded
		uint32_t skinBuilds = 0;       ///< character shapes with their own BLAS: first builds + periodic rebuilds
		uint32_t skinRefits = 0;
		uint32_t groupBuilds = 0;      ///< merged character BLASes built (member list changed / periodic)
		uint64_t groupTriangles = 0;   ///< triangles in them
		uint32_t groupRefits = 0;
		uint32_t skinDispatches = 0;
		uint64_t skinVertices = 0;     ///< vertices skinned
		uint32_t tlasInstances = 0;
		uint32_t poolBlocks = 0;       ///< pool blocks adopted since the previous submission (growth)
		uint32_t poolReleased = 0;     ///< pool blocks released since the previous submission
		uint64_t submission = 0;       ///< the backend's trace submission number
		uint32_t sampleAge = 0;        ///< submissions between this one and the frame's own (the RT GPU columns on a row are this many frames old)
		uint32_t windBuilds = 0;       ///< (47l) wind BLAS first builds (the build-cost model skips such submissions)
		uint32_t windRefits = 0;       ///< (47l) wind BLAS in-place refits
		uint64_t windVertices = 0;     ///< (47l) vertices posed for them
	};

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
		int64_t plWaterRays = -1;  // (47j) of plRays, traced from the water surface
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
		// (46t) The last RTAO composite (-1 = none): filter 0 quarter (46s) / 1 quarter (46t) / 2 full resolution; near rays per pixel (0 =
		// fine detail off); the near / far split (units); normal-guided (1 / 0); alpha budget mode (0 blocked, 1 half open, 2 open).
		int rtaoFilter = -1, rtaoDetailRays = -1, rtaoNormalGuided = -1, rtaoAlphaBudget = -1;
		float rtaoDetailLength = -1.0f;
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
		// (46x) Point-light selection: direct fills traced, fills left untraced, portal-strict lights behind portals / of which dropped, ranked
		// only inside their rooms' box (-1 = not traced this frame).
		int plFillDirect = -1, plFillAmbient = -1, plPortalAway = -1, plPortalDropped = -1, plPortalBoxed = -1;
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
		// (46p) The work of the prep submission prepGpuMs was measured on (same read-back).
		RtPrepWork prepWork{};
		// (47l) Foliage sway: wind meshes alive (BLASes refit in place), their vertices, refits recorded this frame (-1 = sway off / RT off).
		int64_t windMeshes = -1, windVertices = -1, windRefits = -1;
	};
	void OnRayTracingDetail(const RayTracingDetail& a_detail);

	// ---- (46q) columns of the 46q switches (-1 = not run / not known) -----------------------------------------------
	struct Q46
	{
		// RT: Submit on a Worker Thread: in effect, batches the thread ran since the last frame, the longest (ms in D3D12 calls),
		// the longest wait from hand-over to start (ms), the longest single call (ms).
		int subOn = -1;
		uint32_t subBatches = 0;
		float subMaxBatchMs = -1.0f, subMaxLagMs = -1.0f, subMaxCallMs = -1.0f;
		// RT: Budget Builds by GPU Time: on, measured GPU ms per million triangles, this frame's static triangle budget, static
		// builds still queued, merged-character builds put off, merged BLASes kept without a draw.
		int budOn = -1;
		float budMsPerMtri = -1.0f;
		uint32_t budTris = 0, budStaticQueued = 0, budGroupsDeferred = 0, budGroupsKept = 0;
		// Dynamic Snow footprint shapes: main-thread ms spent on them this frame, state (0 not loaded, 1 loading in the
		// background, 2 ready, 3 none found / failed).
		float snowShapesMs = 0.0f;
		int snowShapesState = -1;
		// Render-resolution sizing: Size Buffers to Render Resolution / SSRT+SSGI at render resolution in effect (1 / 0), the
		// allocation size used (0 = output size).
		int rrB = -1, rrC = -1;
		uint32_t rrWidth = 0, rrHeight = 0;
	};
	/// The RT part (Ray Tracing, once a frame).
	void On46qRt(const Q46& a_rt);
	/// Dynamic Snow's footprint shapes (main-thread ms added up over the frame).
	void On46qSnow(float a_ms, int a_state);
	/// Render-resolution sizing state (Upscaling, once a frame).
	void On46qRenderRes(int a_b, int a_c, uint32_t a_width, uint32_t a_height);

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
		// (46u) Reflex's input sample to simulation start, smoothed (-1 = not reported: no input-sample marker).
		float reflexInputMs = -1.0f;
	};
	Smoothed GetSmoothed();

	/// (46u) Estimate of NVIDIA's "PC Latency" (PCL): the input reaching the PC to the frame starting to be
	/// sent to the display (mouse and panel not included). Only the game + render part (Reflex: simulation
	/// start to GPU end) and, when reported, the input sample are measured; the rest is modelled from the real
	/// frame time, the frame-generation multiplier and the display refresh rate. Shared by the overlay and F7.
	struct LatencyEstimate
	{
		bool valid = false;          ///< false without a game + render latency (the one measured part)
		bool inputMeasured = false;  ///< input wait from Reflex's input sample, else half a real frame
		float inputMs = 0.0f;        ///< input reaching the PC to the game reading it (average wait)
		float renderMs = 0.0f;       ///< Reflex: simulation start to GPU end
		float fgMult = 1.0f;         ///< shown frames per real frame (1 = no frame generation)
		float fgHoldMs = 0.0f;       ///< real frame held while the generated frames before it are shown
		float refreshHz = 0.0f;      ///< display refresh used for the queue term (0 = unknown)
		float queueMs = 0.0f;        ///< present to the start of scan-out (VRR assumed: 0 below the refresh rate)
		float totalMs = 0.0f;        ///< the sum: the PCL estimate
	};
	/// a_realFrameMs: real (rendered) frame period; a_inputMs: measured input-sample wait or < 0; a_renderMs:
	/// Reflex game + render latency or <= 0 (estimate invalid); a_fgMult: shown per real frame (<= 1 = off); a_refreshHz: display Hz or <= 0.
	LatencyEstimate EstimatePcLatency(float a_realFrameMs, float a_inputMs, float a_renderMs, float a_fgMult, float a_refreshHz);
	/// (46u) The model's frame-generation multiplier right now (measured DLSS-G cadence, else the applied multiplier; 1 = off).
	float CurrentFgMultiplier();

	/// Real-frame CPU vs GPU series for the overlay (last a_seconds). CPU frame = simulation start to
	/// Present start (all CPU submission work, driver blocking included); GPU frame = GPU work span.
	struct SeriesStats
	{
		int frames = 0;
		float cpuAvg = 0, cpuLow1 = 0, cpuMax = 0;  ///< low1 = the 99th-percentile frame time (1% low)
		int gpuFrames = 0;
		float gpuAvg = 0, gpuLow1 = 0, gpuMax = 0;
		float cpuLimitedPct = 0;  ///< frames where CPU frame > GPU frame; (47h) the CPU frame without its waits for the GPU (cpuGpuWaitAvg)
		float cpuGpuWaitAvg = 0;  ///< (47h) of cpuAvg, time the CPU spent waiting for the GPU inside the frame (blocking D3D11 calls, hidden waits)
	};
	SeriesStats GetSeriesStats(float a_seconds);
	/// Copies the last up-to-a_count CPU and GPU samples (oldest first; GPU -1 = none) for plotting.
	void GetSeries(std::vector<float>& o_cpu, std::vector<float>& o_gpu, int a_count);

	/// The split as JSON for the F12 snapshot.
	nlohmann::json ToJson();

	// ---- (46o) sub-timers of the Present and of the upscaling pass (recording / overlay only) -----

	enum class Split : int
	{
		PresentFg,        ///< DX12SwapChain::Present: DLSS-G presentation state + options
		PresentHdr,       ///< HDR composite + screenshot hook
		PresentSignal11,  ///< the pacer's submit point + D3D11 Signal
		PresentCopy,      ///< D3D12 queue Wait, allocator / list Reset, the copy to the swap chain, tags, Close, ExecuteCommandLists
		PresentSl,        ///< the Streamline / DXGI Present (DLSS-G's wait for the GPU is in here)
		PresentPost,      ///< after it: DLSS-G state, D3D12 Signal, D3D11 Wait, UI clear, HDR upkeep, the spin limiter (no Reflex)
		UpsCopyIn,        ///< Upscaling: the input color copy to the shared texture
		UpsSignal11,      ///< D3D11 Signal before the upscaler
		UpsWait12,        ///< D3D12 queue Wait
		UpsReset,         ///< allocator / list Reset
		SlSetOpts,        ///< Streamline: frame constants + DLSS / RR options
		SlEvaluate,       ///< Streamline: tags + slEvaluateFeature
		UpsEcl,           ///< Close + ExecuteCommandLists
		UpsSignal12,      ///< D3D12 Signal after it
		UpsWait11,        ///< D3D11 Wait
		UpsRcas,          ///< RCAS sharpening or the copy back
		Count
	};
	/// Adds a_ticks (QPC) to this frame's part (only while Active()).
	void AddSplit(Split a_part, int64_t a_ticks);
	/// Lap timer: Mark bills the time since the last Mark (or the start) to a part; Skip drops it. Nothing while not Active().
	struct Lap
	{
		Lap();
		void Mark(Split a_part);
		void Skip();

	private:
		int64_t t = 0;
	};
	/// (46o) M3: a Streamline evaluate returned eWarnOutOfVRAM (any thread).
	void OnOutOfVram();

	// ---- (46p) video memory by owner (always counted; shown while recording) --------------------------------------
	//
	// Every resource we create (and every D3D11 texture / buffer anyone creates through the game's device) gets a small
	// private-data object; the runtime releases it when the resource is destroyed, which counts the release. So bytes
	// created and destroyed per owner are known per frame, and the bytes alive (since the hooks went in) per owner.
	// D3D11 owners come from the caller's module (our DLL, SkyrimSE.exe, any other module); staging resources (system
	// memory) on their own. Sizes are estimates from the description (mips, array, samples; 64 KB granularity for D3D12).

	enum class VramOwner : int
	{
		RtBlas,       ///< RT BLAS pool blocks (static BLASes, compacted copies, character BLASes)
		RtTlas,       ///< RT TLAS buffer
		RtScratch,    ///< RT build / skinning scratch
		RtSkin,       ///< RT character vertex / skinned-vertex pools
		RtUpload,     ///< RT upload heaps (geometry, instances, bones, alpha records): system memory
		RtReadback,   ///< RT read-back heaps: system memory
		RtOther,      ///< RT other D3D12 buffers (texcoords, counters, post-build sizes)
		D3D11Ours,    ///< D3D11 textures / buffers our DLL created (features, RT's D3D11 side)
		D3D11Shared,  ///< D3D11 shared textures our DLL created (D3D11 <-> D3D12 interop: RT, upscaling)
		D3D11Engine,  ///< D3D11 textures / buffers SkyrimSE.exe created
		D3D11Other,   ///< D3D11 textures / buffers other modules created (other plugins, Streamline, drivers)
		D3D11Staging, ///< D3D11 staging resources (any caller): system memory
		Count
	};
	const char* VramOwnerName(VramOwner a_owner);
	/// Counts a_bytes as created for a_owner now, and as released when a_object is destroyed.
	void TagVram(ID3D11DeviceChild* a_object, VramOwner a_owner, uint64_t a_bytes);
	void TagVram(ID3D12Object* a_object, VramOwner a_owner, uint64_t a_bytes);
	/// A D3D11 texture / buffer created through the game's device (EngineTiming's Create* hooks): owner from a_caller.
	/// a_target: default usage, bound as render target / UAV / depth (ours get DXGI_RESOURCE_PRIORITY_HIGH: evicted last, 46p, always since 47m).
	void OnD3D11Created(ID3D11DeviceChild* a_resource, uint64_t a_bytes, bool a_shared, bool a_staging, bool a_target, const void* a_caller);
	/// (46p) Bytes of D3D11 resources our DLL created and that are alive (tagged), for the overlay / trace.
	uint64_t OurD3D11LiveBytes();
	/// (46p) Our creations on this thread are billed to feature a_name until cleared (empty). Without a scope: the pass open on
	/// the main thread (while the overlay / a trace runs), else "other".
	void SetVramScope(std::string_view a_name);
	/// (46q) Pass scopes (CpuPassScope, EngineTiming::PassScope) push their key on this thread whether or not timing runs: a creation
	/// outside a Setup scope is billed to the innermost one. The key must outlive the scope.
	void PushVramScope(std::string_view a_key);
	void PopVramScope();
	/// (46q) Util::SetResourceName: a resource of ours billed to no feature is billed to the one its name starts with ("Feature::x").
	void OnResourceNamed(ID3D11DeviceChild* a_resource, std::string_view a_name);
	/// (46p) "name MB, ..." alive per feature of ours (largest first), for the trace header.
	std::string DescribeOurVramByFeature();

	// ---- (46p) coarse GPU time per engine segment (recording only) --------------------------------------------------
	/// EngineTiming: the main thread entered segment a_seg (EngineTiming::Seg as int). A D3D11 timestamp at every change
	/// between render segments (at most 16 per frame), read back a few frames later without waiting.
	void OnEngineSegment(int a_seg);
	/// (47j) Every draw (State): whether it uses the Water shader; brackets the runs of water draws with GPU stamps while recording.
	void OnWaterDrawState(bool a_water);

}
