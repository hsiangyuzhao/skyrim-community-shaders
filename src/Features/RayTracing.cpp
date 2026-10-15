#include "RayTracing.h"

#include "Deferred.h"
#include "Features/RayTracing/CasterFilter.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "State.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuTimers.h"
#include "Utils/LightCensus.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RayTracing::Settings,
	Enabled,
	QualityLevel,
	Characters,
	PhysicsParts,
	SettingsVersion,
	Buildings,
	Objects,
	Trees,
	Foliage,
	Cutouts,
	Hair,
	FirstPersonPlayer,
	Denoiser,
	RtContactScale,
	CustomBaseRays,
	CustomExtraRays,
	CustomSunCone,
	CustomFalloffStart,
	CustomFalloffEnd,
	CustomFarQuality,
	CustomTraceResolution,
	CustomNpcQuality,
	CustomDenoiseRadius,
	CustomDenoiseTaps,
	CustomDenoiseClamp,
	CustomHistory,
	CustomDenoiseHalf,
	CustomFarDenoiseScale,
	SigmaStabilizedFrames,
	PointLights,
	PointLightsPerPixel,
	PointLightsMaxSources,
	PointLightsDistance,
	PointLightsNative,
	PointLightsKeepSSPLS,
	PointLightsSize,
	PointLightsClearance,
	PointLightsInteriors,
	PointLightsPerPixelInterior,
	PointLightsWindowBeams,
	PointLightsSkipShadowMaps,
	PointLightsPlayer,
	PointLightsEffects,
	PointLightsAlphaOutdoors,
	PointLightsStable,
	PointLightsBeamCull,
	PointLightsBeamGuard,
	AO,
	AOMode,
	AORayLength,
	AOStrength,
	AOTemporal,
	AODistance,
	AOInteriors,
	AOModeInterior,
	LoadingPump,
	LoadingPumpMs,
	BlackWindowMs,
	CharactersParallel,
	CharactersScanCache,
	CharactersMerge,
	SplitTraceSync)

namespace
{
	// Quality tiers (rays, sun cone, small filter, distance fall-off, denoiser size).
	struct QualityTier
	{
		const char* name;
		std::uint32_t baseRays, extraRays;
		float coneDegrees;  // half angle; the sun disc is about 0.27
		std::uint32_t smallFilterRadius;
		std::uint32_t tracePattern, farPattern;  // 0 full, 1 checkerboard, 2 half (one pixel of each 2x2)
		float falloffStart, falloffEnd;          // units (70 per metre); end <= start: none
		bool npcReduced;
		float denoiseRadius;
		std::uint32_t denoiseTaps;
		bool denoiseHalf;
		float farDenoiseScale;
		const char* tooltip;
	};

	constexpr QualityTier kTiers[5] = {
		{ "Very Low", 1, 0, 0.27f, 1, 1, 2, 1024.0f, 4096.0f, true, 8.0f, 3, true, 0.5f,
			"Half the pixels traced each frame (checkerboard), a quarter beyond 60 m; no extra rays; other characters at reduced quality; "
			"half-resolution denoiser. Cheapest." },
		{ "Low", 1, 0, 0.27f, 1, 0, 1, 2048.0f, 6144.0f, true, 12.0f, 4, true, 0.5f,
			"1 ray per pixel, checkerboard beyond 90 m; no extra rays; other characters at reduced quality; half-resolution denoiser." },
		{ "Medium", 1, 1, 0.27f, 1, 0, 0, 4096.0f, 12288.0f, true, 16.0f, 4, true, 0.75f,
			"1 ray per pixel (+1 at shadow edges, fading out between 60 and 175 m), the real sun disc; other characters at "
			"reduced quality; half-resolution denoiser." },
		{ "High", 1, 3, 0.5f, 2, 0, 0, 6144.0f, 16384.0f, false, 16.0f, 6, false, 1.0f,
			"1 ray per pixel (+3 at shadow edges, fading out between 90 and 230 m), slightly softer edges; full-resolution denoiser." },
		{ "Ultra", 2, 4, 0.5f, 2, 0, 0, 0.0f, 0.0f, false, 16.0f, 6, false, 1.0f,
			"2 rays per pixel (+4 at shadow edges, no fall-off), slightly softer edges; full-resolution denoiser. Cleanest." },
	};
	constexpr const char* kQualityNames[6] = { "Very Low", "Low", "Medium", "High", "Ultra", "Custom" };
	constexpr const char* kPatternNames[3] = { "Full", "Checkerboard", "Half" };

	constexpr float kCaptureBudgetMs = 1.0f;      // CPU per frame for capturing new cells (45j: 1.5 -> 1.0, resumable per shape)
	constexpr std::uint32_t kBuildBudget = 131072;  // triangles of BLAS builds per frame (about one chunk)
	constexpr float kHandOffBand = 2048.0f;       // half a cell: RT fades back to the cascades here
	constexpr float kMaxRayLength = 16384.0f;
	constexpr std::uint64_t kAccelerationBudgetBytes = 1200ull << 20;  // BLAS + scratch; spec target is 1.5 GB in total
	constexpr std::uint32_t kFlagSkipShadowed = 1u, kFlagCoverage = 2u, kFlagFirstPerson = 4u;  // TraceShadowsCS
	constexpr std::uint32_t kFlagWorldNoise = 8u, kFlagStableNormal = 16u, kFlagAlphaFootprint = 32u;  // TraceShadowsCS (45j)
	constexpr std::uint32_t kFlagDebug = 2u;  // Common.hlsli
	constexpr std::uint32_t kFlagCombineSSS = 4u, kFlagHistoryValid = 32u, kFlagTraceSkipped = 64u;  // Common.hlsli (45j)
	// (45k) Denoiser and the trace flags it needs.
	constexpr std::uint32_t kFlagR2Noise = 64u, kFlagAdaptive = 128u, kFlagBlockerDist = 256u, kFlagRtSurface = 512u;  // TraceShadowsCS
	constexpr std::uint32_t kFlagPenumbra = 128u, kFlagCombineBlockerDist = 256u;                                       // Common.hlsli
	// (45l) Cost switches of the trace and the denoiser stages.
	constexpr std::uint32_t kFlagBlockerEdges = 1024u, kFlagCharCull = 2048u, kFlagFalloff = 4096u;  // TraceShadowsCS
	constexpr std::uint32_t kPatternNpcReduced = 16u;                                                  // TraceShadowsCS Extra.w
	constexpr std::uint32_t kFlagSpatial = 512u, kFlagDenoiseHalf = 1024u, kFlagSigma = 2048u, kFlagFalloffDenoise = 4096u;  // Common.hlsli
	constexpr float kBlockerRange = 4096.0f;    // TraceShadowsCS kBlockerRange
	// (45k) Loading screens: capture / build budgets (frame time does not matter there), warm-up after them.
	constexpr float kLoadingCaptureBudgetMs = 8.0f;
	constexpr std::uint32_t kLoadingBuildTriangles = 1u << 20;  // per submission (TDR-safe: about 8 frames' worth of normal builds)
	constexpr std::uint32_t kLoadingMaxBuilds = 128;  // (46g) 64 -> 128 (a CPU budget per frame now; 8 contexts x 128 stay within the postbuild slots)
	// (46g) Transition budgets: render-thread ms per frame. Loading / black screen: nothing visible; WarmUp: visible, not handed off.
	constexpr float kLoadingBuildCpuMs = 12.0f, kBlackBuildCpuMs = 10.0f, kWarmUpBuildCpuMs = 3.0f;
	constexpr float kBlackCaptureBudgetMs = 8.0f, kWarmUpCaptureBudgetMs = 3.0f;
	constexpr float kBlackSkinUploadCpuMs = 6.0f;
	constexpr double kBlackMaxMs = 4000.0;  // a Fader Menu open longer than this after a loading screen is not the black screen
	constexpr double kLateHoldMs = 4000.0;  // items not ready at the hand-off wait at most this long for the late batch
	constexpr double kWarmUpTimeoutMs = 5000.0;
	constexpr double kFadeInMs = 500.0;
	constexpr std::uint32_t kAlphaCopiesPerFrame = 16;  // (45h) 256x256 alpha copies made per frame (5 small dispatches each)
	constexpr float kCharacterSelfShadowOffset = 6.0f;  // hits on characters nearer than this along the sun ray are ignored (units)
	constexpr float kRayBiasConstant = 2.0f, kRayBiasPerUnit = 0.0015f;  // the ray start offset: 2 units + 0.15 % of the distance

	struct LoadingMenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			// Load game, fast travel, door into another worldspace: every captured cell is stale.
			// (45k) Also when it closes: the warm-up starts.
			if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME)
				globals::features::rayTracing.OnLoadingScreen(a_event->opening);
			// (46g) The black screen after a loading screen (the fade back in).
			if (a_event && a_event->menuName == RE::FaderMenu::MENU_NAME)
				globals::features::rayTracing.OnFader(a_event->opening);
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	LoadingMenuSink g_loadingSink;

	// (46k) The engine's own transition events: cells fully loaded (behind the loading screen), the teleport's stages, the end
	// of a fast travel, a loaded game. The handlers only record (Ray Tracing acts on them on the render thread).
	struct TransitionEventSink :
		RE::BSTEventSink<RE::TESCellFullyLoadedEvent>,
		RE::BSTEventSink<RE::TESFastTravelEndEvent>,
		RE::BSTEventSink<RE::TESLoadGameEvent>,
		RE::BSTEventSink<RE::PositionPlayerEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESCellFullyLoadedEvent* a_event, RE::BSTEventSource<RE::TESCellFullyLoadedEvent>*) override
		{
			if (a_event && a_event->cell)
				globals::features::rayTracing.OnCellFullyLoaded(a_event->cell);
			return RE::BSEventNotifyControl::kContinue;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::TESFastTravelEndEvent*, RE::BSTEventSource<RE::TESFastTravelEndEvent>*) override
		{
			globals::features::rayTracing.OnFastTravelEnd();
			return RE::BSEventNotifyControl::kContinue;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::TESLoadGameEvent*, RE::BSTEventSource<RE::TESLoadGameEvent>*) override
		{
			globals::features::rayTracing.OnLoadGame();
			return RE::BSEventNotifyControl::kContinue;
		}
		RE::BSEventNotifyControl ProcessEvent(const RE::PositionPlayerEvent* a_event, RE::BSTEventSource<RE::PositionPlayerEvent>*) override
		{
			if (a_event)
				globals::features::rayTracing.OnPositionPlayer(static_cast<int>(a_event->type.get()));
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	TransitionEventSink g_transitionSink;

	// (46m) Equip / unequip: that actor's 3D is walked in full for the next frames (Character Scan Cache). Records only.
	struct EquipEventSink : RE::BSTEventSink<RE::TESEquipEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::TESEquipEvent* a_event, RE::BSTEventSource<RE::TESEquipEvent>*) override
		{
			if (a_event && a_event->actor)
				RayTracingCharacters::Characters::NoteEquip(a_event->actor.get());
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	EquipEventSink g_equipSink;
	constexpr double kEventsLogAfterCloseMs = 6000.0;  // (46k) the events line at the latest this long after the loading screen closed
	constexpr std::size_t kMaxQueuedCellEvents = 4096;

	std::string Megabytes(std::uint64_t a_bytes) { return std::format("{:.1f} MB", static_cast<double>(a_bytes) / (1024.0 * 1024.0)); }

	// (46b) Point-light shadows.
	int ValidPointK(int a_value) { return a_value >= 4 ? 4 : (a_value >= 2 ? 2 : 1); }
	constexpr float kPointMinShare = 0.02f;          // a light under 2 % of the pixel's point light is not traced
	constexpr float kPointMinContribution = 1e-3f;   // nor one that hardly lights it at all
	constexpr float kPointHistoryWeight = 0.25f;     // this frame's weight in the denoiser's history blend
	constexpr float kPointMaxFilterRadius = 12.0f;   // px
	constexpr float kPointSunShare = 0.05f;          // at a sunlit pixel a point light under 5 % of the sun's light there is not traced
	constexpr std::uint32_t kPointFlagCharacters = 1u, kPointFlagFirstPerson = 2u, kPointFlagCharCull = 4u;  // TracePointLightsCS
	constexpr std::uint32_t kPointFlagSticky = 8u, kPointFlagBeamCullFront = 16u, kPointFlagBeamCullBack = 32u;  // (46g) TracePointLightsCS
	constexpr std::uint32_t kPointDenoiseHistory = 1u, kPointDenoiseDebug = 2u;                             // PointDenoiseCS
	constexpr float kBeamRayMargin = 64.0f;  // (46c) a window beam's ray goes this far past the interior's box
	// (46f) RTAO.
	constexpr float kAORayLengthMin = 35.0f, kAORayLengthMax = 210.0f;  // 0.5 - 3 m
	constexpr float kAOFadeStartShare = 0.75f;  // the RT weight falls from 75 % of the distance to 0 at it (back to SSGI AO)
	constexpr float kAOHistoryWeight = 0.2f;    // this frame's weight in the clamped history blend
	constexpr float kAOClampSlack = 0.08f;      // the history may stray this far outside this frame's filtered 3x3 range
	constexpr std::uint32_t kAOTraceCharacters = 1u, kAOTraceFirstPerson = 2u;  // TraceAOCS
	constexpr std::uint32_t kAOGapFrames = 30;  // (46h) RTAO not mixed in for more than this many frames: it fades in again
	constexpr double kAOFadeInMs = 400.0;

	// (46c) BSShadowFrustumLight / BSShadowParabolicLight::RenderShadowmaps (vfunc 0x0A): Ray Tracing may skip the shadow
	// maps of lights it fully handles this frame (Point Lights > Stop Engine Shadow Maps, interiors only).
	struct FrustumLightRenderShadowmaps
	{
		static void thunk(RE::BSShadowLight* a_light, void* a2)
		{
			if (globals::features::rayTracing.loaded && globals::features::rayTracing.SkipShadowMap(a_light))
				return;
			func(a_light, a2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
	/// The shadow mask channel of a shadow light (UINT32_MAX when unreadable).
	std::uint32_t ReadShadowChannel(RE::BSShadowLight* a_light)
	{
		__try {
			return a_light->GetRuntimeData().shadowLightIndex;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return UINT32_MAX;
		}
	}
	struct ParabolicLightRenderShadowmaps
	{
		static void thunk(RE::BSShadowLight* a_light, void* a2)
		{
			if (globals::features::rayTracing.loaded && globals::features::rayTracing.SkipShadowMap(a_light))
				return;
			func(a_light, a2);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct GpuBucketScope
	{
		explicit GpuBucketScope(Util::GpuBucket a_bucket) :
			bucket(a_bucket) { Util::GpuPassTimers::GetSingleton()->Begin(bucket); }
		~GpuBucketScope()
		{
			if (open)
				Util::GpuPassTimers::GetSingleton()->End(bucket);
		}
		GpuBucketScope(const GpuBucketScope&) = delete;
		GpuBucketScope& operator=(const GpuBucketScope&) = delete;
		// (45k) Another timed pass in between (GPU timer intervals cannot nest).
		void Pause()
		{
			if (open)
				Util::GpuPassTimers::GetSingleton()->End(bucket);
			open = false;
		}
		void Resume()
		{
			if (!open)
				Util::GpuPassTimers::GetSingleton()->Begin(bucket);
			open = true;
		}
		Util::GpuBucket bucket;
		bool open = true;
	};

	std::int64_t Qpc()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	double QpcMs()
	{
		static const double toMs = [] {
			LARGE_INTEGER f;
			QueryPerformanceFrequency(&f);
			return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
		}();
		return toMs;
	}
}

void RayTracing::OnLoadingScreen(bool a_open)
{
	{
		// (46k) The transition's event record (a previous one not logged yet is logged now, marked as cut short).
		std::scoped_lock lock(eventsMutex);
		const std::int64_t now = Qpc();
		if (a_open) {
			if (events.pending)
				logger::info("[RayTracing] Transition events (cut short by the next loading screen): {}", DescribeTransitionEvents(events));
			events = {};
			events.loadingOpen = now;
			events.pending = true;
		} else if (events.pending) {
			events.loadingClose = now;
		}
	}
	if (a_open) {
		scene.RequestFlush();
		characters.RequestFlush();  // (45i)
		loadingOpen.store(true);
	} else {
		loadingOpen.store(false);
		loadingClosed.store(true);
	}
}

void RayTracing::OnFader(bool a_open)
{
	(a_open ? faderOpenQpc : faderCloseQpc).store(Qpc());
	faderOpen.store(a_open);
}

void RayTracing::OnCellFullyLoaded(const RE::TESObjectCELL* a_cell)
{
	{
		std::scoped_lock lock(eventsMutex);
		const std::int64_t now = Qpc();
		if (events.pending && events.loadingOpen != 0) {
			++events.cells;
			if (events.firstCell == 0)
				events.firstCell = now;
			events.lastCell = now;
		}
		if (cellEvents.size() < kMaxQueuedCellEvents)
			cellEvents.emplace_back(a_cell, now);
	}
	PrepareOnCellEvent();  // (46n) outside the lock (the step drains the queue)
}

namespace
{
	/// (46n) Runs a_fn under an SEH guard (a fault in the scene graph must not take the game down mid-load). No C++ objects
	/// with destructors live in this frame (C2712); C++ exceptions are caught inside a_fn.
	bool SehInvoke(void (*a_fn)(void*), void* a_ctx, unsigned long& o_code)
	{
		__try {
			a_fn(a_ctx);
			return true;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			o_code = GetExceptionCode();
			return false;
		}
	}

	constexpr double kFadeFullMs = 1000.0;  // (46n) the fade-in's first second: the picture is still almost black
	constexpr double kFadeEndMs = 3000.0;   // the fade lasts about 3 s (46m log: the Fader Menu closed 2.7-3.2 s after the loading screen)
}

void RayTracing::PrepareOnCellEvent()
{
	// (46n) The engine fires TESCellFullyLoadedEvent on the thread loading the cells. When that is the world's thread (the main
	// thread in SE) and a loading screen is up, the cell is captured and built right here, within CellEventMs: the loading
	// screen itself is presented from another thread, so nothing else runs our preparation behind it.
	if (!loadingOpen.load())
		return;
	const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
	if (renderThreadId.load() == 0 || thread != renderThreadId.load()) {
		// Another thread: only an atomic counter and one log line (everything else here belongs to the world's thread).
		cellEventForeign.fetch_add(1, std::memory_order_relaxed);
		if (!cellEventForeignLogged.exchange(true))
			logger::warn("[RayTracing] Cells finish loading on thread {}, the world is rendered on thread {}: nothing is prepared from the cell events",
				thread, renderThreadId.load());
		return;
	}
	if (cellEventActive)
		return;
	{
		std::scoped_lock lock(eventsMutex);
		if (events.loadingOpen != cellEventOpenSeen) {
			cellEventOpenSeen = events.loadingOpen;
			cellEventLog = {};
		}
	}
	if (!settings.Enabled || !settings.LoadingPump || !settings.PrepareOnCellEvents || cellEventBroken)
		return;
	++cellEventLog.events;
	const char* skip = nullptr;
	if (status == Status::Unavailable || status == Status::Failed || status == Status::Off)
		skip = "Ray Tracing not running";
	else if (!backend.IsReady() || backend.HasFailed())
		skip = "backend not ready";
	else if (backend.TraceWaitPending() || backend.SunWaitPending() || backend.AOWaitPending())
		skip = "a D3D11 wait of the last frame still pending";  // that would need the D3D11 context the loading screen is using
	if (skip) {
		++cellEventLog.skipped;
		cellEventLog.lastSkip = skip;
		return;
	}
	{
		std::scoped_lock lock(eventsMutex);
		if (events.teleportPre != 0 && events.teleportPre > events.teleportFinish)
			++cellEventLog.midTeleport;
	}
	cellEventActive = true;
	cellEventBudgetMs = std::clamp(settings.CellEventMs, 2.0f, 50.0f);
	const std::int64_t start = Qpc();
	unsigned long code = 0;
	const bool ok = SehInvoke(&RayTracing::CellEventStepThunk, this, code);
	const double ms = static_cast<double>(Qpc() - start) * QpcMs();
	cellEventActive = false;
	++cellEventLog.steps;
	cellEventLog.cpuMs += ms;
	cellEventLog.maxMs = std::max(cellEventLog.maxMs, ms);
	if (!ok) {
		// The scene may be half-updated: Ray Tracing goes off at the next world frame (Disable binds D3D11 state: not from here).
		cellEventBroken = true;
		cellEventFault = std::format("fault 0x{:08X} while preparing a cell behind the loading screen (Prepare On Cell Load turned off for this session)", code);
		logger::error("[RayTracing] {}", cellEventFault);
	}
}

void RayTracing::CellEventStepThunk(void* a_self)
{
	auto* self = static_cast<RayTracing*>(a_self);
	try {
		self->CellEventStep(self->cellEventBudgetMs);
	} catch (const std::exception& e) {
		self->cellEventBroken = true;
		self->cellEventFault = std::format("exception while preparing a cell behind the loading screen: {}", e.what());
		logger::error("[RayTracing] {}", self->cellEventFault);
	}
}

void RayTracing::CellEventStep(float a_budgetMs)
{
	// As the loading pump, minus everything that touches D3D11 (the loading screen owns the immediate context now) and the
	// characters (actors are not in place yet): the transition phase, the queued cell events into the scene, the capture, pool
	// headroom, the BLAS builds on our own queue. Nothing traced, nothing waited for.
	const std::int64_t start = Qpc();
	if (status != Status::Waiting) {
		status = Status::Waiting;
		statusReason = "preparing behind the loading screen";
	}
	if (backend.HasPrepared())
		backend.AbandonPrepared();
	UpdatePhase();
	DrainTransitionEvents();
	backend.SetTransition(true);
	scene.SetInteriorEnabled(InteriorWanted());
	const auto& gate = scene.GetLoadGateStats();
	const std::uint32_t capturedBefore = gate.captured;
	const std::uint32_t shapesBefore = scene.GetStats().shapes;
	const std::uint64_t buildsBefore = backend.GetStats().buildsTotal;
	{
		const auto& stats = backend.GetStats();
		overVramBudget = stats.blasBytes + stats.scratchBytes > kAccelerationBudgetBytes;
	}
	scene.Update(backend, overVramBudget ? 0.0f : a_budgetMs * 0.6f, globals::state->frameCount);
	const auto elapsed = [&]() { return static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs()); };
	if (const float left = a_budgetMs - elapsed(); left > 1.0f)
		backend.Prewarm(std::min(2.0f, left * 0.25f), false);
	backend.BuildOnly(kLoadingBuildTriangles, kLoadingMaxBuilds, std::max(a_budgetMs - elapsed(), 1.0f));
	backend.Tick();
	scene.EndFrame(backend);
	const auto& gateAfter = scene.GetLoadGateStats();
	cellEventLog.cellsCaptured += gateAfter.captured >= capturedBefore ? gateAfter.captured - capturedBefore : 0u;
	const std::uint32_t shapesAfter = scene.GetStats().shapes;
	cellEventLog.shapes += shapesAfter > shapesBefore ? shapesAfter - shapesBefore : 0u;
	const std::uint64_t buildsAfter = backend.GetStats().buildsTotal;
	cellEventLog.builds += buildsAfter > buildsBefore ? buildsAfter - buildsBefore : 0u;
}

float RayTracing::FadeInFactor(double a_sinceCloseMs) const
{
	if (!settings.FadeInBudgets || !faderOpen.load() || a_sinceCloseMs < 0.0 || a_sinceCloseMs > kBlackMaxMs)
		return 0.0f;
	if (a_sinceCloseMs <= kFadeFullMs)
		return 1.0f;
	return static_cast<float>(std::clamp(1.0 - (a_sinceCloseMs - kFadeFullMs) / (kFadeEndMs - kFadeFullMs), 0.0, 1.0));
}

void RayTracing::OnPositionPlayer(int a_stage)
{
	std::scoped_lock lock(eventsMutex);
	const std::int64_t now = Qpc();
	if (a_stage == static_cast<int>(RE::PositionPlayerEvent::EVENT_TYPE::kPre))
		events.teleportPre = now;
	else if (a_stage == static_cast<int>(RE::PositionPlayerEvent::EVENT_TYPE::kFinish))
		events.teleportFinish = now;
}

void RayTracing::OnFastTravelEnd()
{
	std::scoped_lock lock(eventsMutex);
	events.fastTravelEnd = Qpc();
}

void RayTracing::OnLoadGame()
{
	std::scoped_lock lock(eventsMutex);
	events.loadGame = Qpc();
}

bool RayTracing::Arrived() const
{
	std::scoped_lock lock(eventsMutex);
	if (events.loadingOpen == 0)
		return false;
	return (events.teleportFinish >= events.loadingOpen && events.teleportFinish >= events.teleportPre) || events.loadGame >= events.loadingOpen;
}

void RayTracing::RegisterEventSinks()
{
	if (!eventSinksRegistered) {
		if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
			holder->AddEventSink<RE::TESCellFullyLoadedEvent>(&g_transitionSink);
			holder->AddEventSink<RE::TESFastTravelEndEvent>(&g_transitionSink);
			holder->AddEventSink<RE::TESLoadGameEvent>(&g_transitionSink);
			holder->AddEventSink<RE::TESEquipEvent>(&g_equipSink);  // (46m)
			eventSinksRegistered = true;
		}
	}
	if (!playerSinkRegistered) {
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			player->AsPositionPlayerEventSource()->AddEventSink(&g_transitionSink);
			playerSinkRegistered = true;
		}
	}
}

void RayTracing::DrainTransitionEvents()
{
	// (46k) Render thread. Only cells that finished loading after this transition's loading screen opened count.
	std::vector<std::pair<const RE::TESObjectCELL*, std::int64_t>> queued;
	std::int64_t openQpc = 0;
	{
		std::scoped_lock lock(eventsMutex);
		queued.swap(cellEvents);
		openQpc = events.loadingOpen;
	}
	if (!inTransition || openQpc == 0)
		return;
	for (const auto& [cell, qpc] : queued)
		if (qpc >= openQpc)
			scene.NoteFullyLoaded(cell);
}

std::string RayTracing::DescribeTransitionEvents(const TransitionEvents& a_events) const
{
	// Ms after the loading screen opened ("-" = not seen).
	const std::int64_t base = a_events.loadingOpen;
	auto at = [&](std::int64_t a_qpc) -> std::string {
		if (a_qpc == 0 || base == 0)
			return "-";
		return std::format("{:+.0f}", static_cast<double>(a_qpc - base) * QpcMs());
	};
	const std::int64_t faderOpened = faderOpenQpc.load(), faderClosed = faderCloseQpc.load();
	std::string line = std::format("ms after the loading screen opened: teleport start {} / finish {}; fast travel end {}; game loaded {}; "
								   "cells fully loaded {} (first {}, last {}); loading screen closed {}",
		at(a_events.teleportPre), at(a_events.teleportFinish), at(a_events.fastTravelEnd), at(a_events.loadGame), a_events.cells, at(a_events.firstCell),
		at(a_events.lastCell), at(a_events.loadingClose));
	// The Fader Menu's last open / close around this loading screen (it may open before the loading screen, for a door).
	const bool faderRecent = faderOpened != 0 && base != 0 && static_cast<double>(base - faderOpened) * QpcMs() < 10000.0;
	line += std::format("; fader opened {}, closed {}", faderRecent ? at(faderOpened) : std::string("-"),
		faderClosed > faderOpened && faderRecent ? at(faderClosed) : std::string("-"));
	if (a_events.loadingClose != 0 && faderRecent && faderClosed > a_events.loadingClose)
		line += std::format(" (the fade-in ended {:.0f} ms after the loading screen closed)", static_cast<double>(faderClosed - a_events.loadingClose) * QpcMs());
	return line;
}

void RayTracing::MaybeLogTransitionEvents()
{
	// (46k) Once the transition is over (the Fader Menu closed after the loading screen, or 6 s after it closed): one line,
	// also with Ray Tracing off (the control run for the loading screen's length).
	std::string line;
	{
		std::scoped_lock lock(eventsMutex);
		if (!events.pending || events.loadingClose == 0)
			return;
		const std::int64_t now = Qpc();
		const bool faded = faderCloseQpc.load() > events.loadingClose && !faderOpen.load();
		if (!faded && static_cast<double>(now - events.loadingClose) * QpcMs() < kEventsLogAfterCloseMs)
			return;
		events.pending = false;
		line = DescribeTransitionEvents(events);
	}
	lastEventsSummary = line;
	logger::info("[RayTracing] Transition events (Ray Tracing {}): {}", settings.Enabled ? "on" : "off", line);
}

void RayTracing::UpdatePhase()
{
	// (45k) Render thread, once per frame. (46g) Loading (menu open: capture / build the destination, nothing traced)
	// -> Black (closed, the Fader Menu still open: nothing visible, large budgets) -> WarmUp (visible, not ready yet)
	// -> the hand-off: every ready cell / object switches from the shadow map to RT at once, one global fade (in the
	// black screen when the scene is ready by then) -> Play. Items not ready at the hand-off wait for one late batch.
	const bool open = loadingOpen.load();
	const bool closed = loadingClosed.exchange(false);
	const auto& sc = scene.GetStats();
	const auto& stats = backend.GetStats();
	const std::int64_t now = Qpc();
	auto beginTransition = [&]() {
		handedOff = false;
		lateHold = false;
		inTransition = true;
		fade = 0.0f;
		beamGuards.clear();  // light pointers of the place left behind
		excludedBeams.clear();
		pointSelector.ForgetHistory();  // (46s) per-light positions / groups of the place left behind
		scene.SetHold(true);
		for (auto& entry : transitionLog)
			entry = {};
		transitionLogSlot = -1;
		handOffSummary.clear();
		// (46k)
		scene.ClearLoadGate();
	};
	if (open && phase != Phase::Loading) {
		if (transitionLogSlot >= 0)
			TransitionLogFlush();  // a transition interrupted by the next one
		phase = Phase::Loading;
		loadShapesAtOpen = sc.shapes;
		loadBuildsAtOpen = static_cast<std::uint32_t>(stats.buildsTotal);
		beginTransition();
		scene.SetTransition(true);  // the cells attached now (the place being left) are not captured again
	}
	if (!open && (closed || phase == Phase::Loading)) {
		if (!inTransition)
			beginTransition();
		phase = faderOpen.load() ? Phase::Black : Phase::WarmUp;
		scene.SetTransition(false);
		warmUpStart = now;
		loadShapesAtClose = sc.shapes;
		loadBuildsAtClose = static_cast<std::uint32_t>(stats.buildsTotal);
		fade = 0.0f;
	}
	const double sinceClose = static_cast<double>(now - warmUpStart) * QpcMs();
	// (46k) The Fader Menu stays open through the whole fade-in and cannot tell when the fade starts: only the first
	// BlackWindowMs after the loading screen closed count as black (large budgets); 46i counted up to 4 s (pump off).
	const double blackLimit = settings.LoadingPump ? std::clamp(static_cast<double>(settings.BlackWindowMs), 0.0, kBlackMaxMs) : kBlackMaxMs;
	if (phase == Phase::Black && (!faderOpen.load() || sinceClose > blackLimit))
		phase = Phase::WarmUp;
	blackScreen = inTransition && !open && phase != Phase::Loading && faderOpen.load() && sinceClose <= blackLimit;
	scene.SetLoadGate(settings.LoadingPump, Arrived());  // (46k)

	if ((phase == Phase::Black || phase == Phase::WarmUp) && !handedOff) {
		// (46k) Only once the destination itself is captured and built (46h handed off the place left: its stats were
		// still the old ones on the first frame after the loading screen), or at the time-out.
		const bool ready = DestinationReady(sinceClose);
		if (ready || sinceClose > kWarmUpTimeoutMs) {
			const bool inBlack = phase == Phase::Black;
			HandOffNow(ready ? (inBlack ? "in the black screen" : "visible") : (inBlack ? "in the black screen, timed out" : "visible, timed out"));
			phase = Phase::Play;
			lastTimeToReadyMs = static_cast<float>(sinceClose);
			lastLoadingSummary = std::format("behind the loading screen {} shapes captured, {} BLAS built; after it {} shapes, {} BLAS; {}",
				loadShapesAtClose > loadShapesAtOpen ? loadShapesAtClose - loadShapesAtOpen : 0u,
				loadBuildsAtClose > loadBuildsAtOpen ? loadBuildsAtClose - loadBuildsAtOpen : 0u, sc.shapes > loadShapesAtClose ? sc.shapes - loadShapesAtClose : 0u,
				static_cast<std::uint32_t>(stats.buildsTotal) > loadBuildsAtClose ? static_cast<std::uint32_t>(stats.buildsTotal) - loadBuildsAtClose : 0u,
				handOffSummary);
			logger::info("[RayTracing] Loading screen: {}", lastLoadingSummary);
		}
	}
	// (46g) The late batch: everything that became ready since the hand-off, switched at once (never one by one),
	// when the queue has drained or kLateHoldMs after the hand-off. From then on cells hand off as they complete.
	if (handedOff && lateHold) {
		const bool drained = sc.cellsGathering == 0 && backend.QueuedBuilds() == 0;
		const auto held = scene.HeldCounts(backend);
		if (drained || now >= lateHoldUntil || (held.shown == 0 && held.pending == 0)) {
			const auto counts = scene.HandOff(backend);
			lateHold = false;
			scene.SetHold(false);
			inTransition = false;
			logger::info("[RayTracing] Late batch {:.0f} ms after the hand-off: {} items switched to RT at once, {} still not ready (they follow as they complete)",
				static_cast<double>(now - handOffQpc) * QpcMs(), counts.shown, counts.pending);
			handOffSummary += std::format("; late batch +{:.0f} ms: {} items, {} left", static_cast<double>(now - handOffQpc) * QpcMs(), counts.shown, counts.pending);
		}
	}
	fade = handedOff ? static_cast<float>(std::clamp(static_cast<double>(now - handOffQpc) * QpcMs() / kFadeInMs, 0.0, 1.0)) : 0.0f;
}

bool RayTracing::DestinationReady(double a_sinceCloseMs) const
{
	if (!backend.IsReady() || scene.FlushPending())
		return false;  // the scene's numbers are still the place left
	if (Util::IsInterior() && !InteriorWanted())
		return true;  // indoors with nothing of Ray Tracing wanted there: nothing to prepare
	const auto& sc = scene.GetStats();
	// (46d) Indoors the scene must be the interior's: behind a door's loading screen the old exterior is still
	// captured (the interior cell is only known once it closes).
	const bool sceneMatches = !(Util::IsInterior() && InteriorWanted() && RE::TES::GetSingleton() && RE::TES::GetSingleton()->interiorCell) || sc.interior;
	if (!sceneMatches || sc.cellsTracked == 0 || sc.cellsGathering > 0 || sc.cellsUntracked > 0)
		return false;
	// Grid cells still attaching (the engine names no state for that: a grid cell that never attaches -- the edge of a
	// small worldspace -- would hold the hand-off for ever, so they count only for the first 1.5 s after the loading screen).
	if (sc.cellsNotAttached > 0 && a_sinceCloseMs < 1500.0)
		return false;
	if (backend.QueuedBuilds() > 0 || backend.PendingSkinUploads() > 0)
		return false;
	// (46k) The player stands in what was captured (not in the place left, nor beside it).
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return false;
	const auto position = player->GetPosition();
	return std::isfinite(position.x) && std::isfinite(position.y) && scene.Covers(position.x, position.y);
}

void RayTracing::HandOffNow(const char* a_when)
{
	const auto& sc = scene.GetStats();
	const std::int64_t now = Qpc();
	const auto counts = scene.HandOff(backend);
	handedOff = true;
	handOffQpc = now;
	handOffThisFrame = true;
	lateHold = true;
	lateHoldUntil = now + static_cast<std::int64_t>(kLateHoldMs / std::max(QpcMs(), 1e-9));
	handOffSummary = std::format("hand-off {:.0f} ms after the loading screen closed ({}): {} items switched to RT at once, {} pending ({} BLAS queued, "
								 "{} cells capturing, {} character shapes waiting)",
		static_cast<double>(now - warmUpStart) * QpcMs(), a_when, counts.shown, counts.pending, backend.QueuedBuilds(), sc.cellsGathering,
		backend.PendingSkinUploads());
	logger::info("[RayTracing] {}", handOffSummary);
}

void RayTracing::TransitionFrame()
{
	// (46g) Behind the loading screen, in the black screen after it, or visible but not handed off yet: the destination
	// is captured and built (large budgets while nothing is visible), its characters prepared and uploaded, the pools /
	// scratch / TLAS / per-context buffers grown ahead. Nothing is traced (the shadow map has every caster); the render
	// thread never waits on the GPU.
	const std::int64_t start = Qpc();
	lastFadeFactor = 0.0f;  // (46n) set below on fade-in frames
	backend.WaitForTrace();
	backend.WaitForAO();  // (46f)
	const bool loading = phase == Phase::Loading;
	const bool visible = phase == Phase::WarmUp && !blackScreen;
	// (46k) Budgets. The loading pump (behind the loading screen): its own CPU per frame (LoadingPumpMs), split between the
	// capture, the characters, the pools and the builds. Visible frames: what playing uses (never bulk work on a visible
	// frame); 46i's moderate ones with the pump off.
	const bool pump = settings.LoadingPump;
	const float pumpMs = std::clamp(settings.LoadingPumpMs, 2.0f, 32.0f);
	float capture = kBlackCaptureBudgetMs, charactersMs = kBlackCaptureBudgetMs, prewarmMs = 4.0f, buildMs = kBlackBuildCpuMs;
	std::uint32_t charactersMax = 64u, buildTriangles = kLoadingBuildTriangles, buildCount = kLoadingMaxBuilds;
	if (loading) {
		capture = pump ? pumpMs * 0.4f : kLoadingCaptureBudgetMs;
		charactersMs = pump ? pumpMs * 0.15f : kLoadingCaptureBudgetMs;
		prewarmMs = pump ? std::min(2.0f, pumpMs * 0.15f) : 4.0f;
		buildMs = kLoadingBuildCpuMs;  // pump: what is left of its budget (below)
	} else if (visible) {
		capture = pump ? kCaptureBudgetMs : kWarmUpCaptureBudgetMs;
		charactersMs = pump ? 1.0f : 2.0f;
		charactersMax = pump ? 6u : 12u;
		prewarmMs = 0.0f;
		buildMs = pump ? 2.0f : kWarmUpBuildCpuMs;
		buildTriangles = pump ? kBuildBudget : kBuildBudget * 2;
		buildCount = 64u;
		// (46n) The fade-in (the Fader Menu still open): the picture is almost black for its first second and dim until about 3 s.
		// The budgets start at the black screen's and shrink to the visible ones over that time.
		const float f = FadeInFactor(static_cast<double>(start - warmUpStart) * QpcMs());
		if (f > 0.0f) {
			const auto mix = [f](float a_visible, float a_black) { return a_visible + (a_black - a_visible) * f; };
			capture = mix(capture, kBlackCaptureBudgetMs);
			charactersMs = mix(charactersMs, kBlackCaptureBudgetMs);
			charactersMax = static_cast<std::uint32_t>(std::lround(mix(static_cast<float>(charactersMax), 64.0f)));
			prewarmMs = mix(prewarmMs, 4.0f);
			buildMs = mix(buildMs, kBlackBuildCpuMs);
			buildTriangles = static_cast<std::uint32_t>(mix(static_cast<float>(buildTriangles), static_cast<float>(kLoadingBuildTriangles)));
			buildCount = static_cast<std::uint32_t>(std::lround(mix(static_cast<float>(buildCount), static_cast<float>(kLoadingMaxBuilds))));
		}
		lastFadeFactor = f;
	}
	{
		const auto& stats = backend.GetStats();
		overVramBudget = stats.blasBytes + stats.scratchBytes > kAccelerationBudgetBytes;
	}
	scene.Update(backend, overVramBudget ? 0.0f : capture, globals::state->frameCount);
	const std::int64_t charactersStart = Qpc();
	if (settings.Characters && backend.SkinAvailable()) {
		float world[4];
		if (scene.GetLoadedRect(world, true)) {
			// (46k) Behind the loading screen the camera is still at the place left: actors are chosen around the player.
			RE::NiPoint3 eye{};
			auto* player = loading ? RE::PlayerCharacter::GetSingleton() : nullptr;
			if (player)
				eye = player->GetPosition();
			characters.Update(backend, world, static_cast<double>(charactersMs), charactersMax, player ? &eye : nullptr);
		}
	}
	cpuLast.characters = static_cast<float>(static_cast<double>(Qpc() - charactersStart) * QpcMs());
	if (prewarmMs > 0.0f)
		backend.Prewarm(prewarmMs, settings.Characters);
	if (loading && pump)
		buildMs = std::max(pumpMs - static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs()), 1.0f);
	backend.BuildOnly(buildTriangles, buildCount, buildMs);
	backend.Tick();
	cpuLast.scene = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
}

void RayTracing::TransitionLogFrame(float a_rtCpuMs)
{
	// (46g) Per-phase work of a transition, logged in one line once the first 3 s after the hand-off are over.
	const std::int64_t now = Qpc();
	int slot = -1;
	const double sinceHandOff = handedOff ? static_cast<double>(now - handOffQpc) * QpcMs() : -1.0;
	if (inTransition || (handedOff && sinceHandOff < 3000.0 && transitionLogSlot >= 0)) {
		// (46k) Black = the black window only (also after a hand-off in it); everything else after the loading screen is visible.
		if (phase == Phase::Loading)
			slot = 0;
		else if (phase == Phase::Black || blackScreen)
			slot = 1;
		else if (!handedOff)
			slot = lastFadeFactor > 0.0f ? 2 : 3;  // (46n) fading in (larger budgets) / visible
		else if (sinceHandOff < 3000.0)
			slot = 4;
	}
	if (slot < 0) {
		if (transitionLogSlot >= 0)
			TransitionLogFlush();
		return;
	}
	const auto& stats = backend.GetStats();
	auto& entry = transitionLog[slot];
	if (!entry.seen) {
		entry.seen = true;
		entry.wallStartQpc = static_cast<double>(now);
		entry.buildsAtStart = stats.buildsTotal;
		entry.uploadsAtStart = backend.SkinUploadsTotal();
		entry.poolBlocksSync = stats.pools.syncBlocksTotal;
		entry.poolBlocksAsync = stats.pools.asyncBlocksTotal;
	}
	++entry.frames;
	entry.rtCpuMs += std::max(a_rtCpuMs, 0.0f);
	entry.wallEndQpc = static_cast<double>(now);
	entry.buildsAtEnd = stats.buildsTotal;
	entry.uploadsAtEnd = backend.SkinUploadsTotal();
	entry.shapesAtEnd = scene.GetStats().shapes;
	transitionLogSlot = slot;
}

void RayTracing::TransitionLogFlush()
{
	static constexpr const char* kNames[5] = { "loading screen", "black screen", "fading in before the hand-off", "visible before the hand-off",
		"visible, first 3 s after the hand-off" };
	static_assert(std::size(kNames) == std::tuple_size_v<decltype(transitionLog)>);
	const auto& stats = backend.GetStats();
	std::string line;
	for (int i = 0; i < static_cast<int>(std::size(kNames)); ++i) {
		const auto& e = transitionLog[i];
		if (!e.seen) {
			line += std::format("{}{}: none", line.empty() ? "" : "; ", kNames[i]);
			continue;
		}
		line += std::format("{}{}: {} frames, {:.0f} ms, RT CPU {:.0f} ms, {} BLAS built, {} character shapes uploaded, scene {} shapes", line.empty() ? "" : "; ",
			kNames[i], e.frames, (e.wallEndQpc - e.wallStartQpc) * QpcMs(), e.rtCpuMs, e.buildsAtEnd - e.buildsAtStart, e.uploadsAtEnd - e.uploadsAtStart,
			e.shapesAtEnd);
	}
	logger::info("[RayTracing] Transition work: {}; pools {:.0f} MB reserved ({} blocks made behind loading / black screens, {} on the pool thread); {}",
		line, static_cast<double>(stats.pools.reservedBytes) / (1024.0 * 1024.0), stats.pools.syncBlocksTotal, stats.pools.asyncBlocksTotal,
		handOffSummary.empty() ? std::string("no hand-off") : handOffSummary);
	// (46k) The loading pump and the loading-screen gate of this transition.
	const auto& gate = scene.GetLoadGateStats();
	logger::info("[RayTracing] Loading pump ({}, black window {:.0f} ms): {} frames, RT CPU {:.0f} ms ({} frames on another thread, {} in the middle of the teleport: "
				 "skipped); {} fully-loaded cells reported, {} cells captured behind the loading screen ({} after their event, the rest once the player "
				 "had arrived), {} checks found a cell's 3D not in place yet",
		settings.LoadingPump ? std::format("on, {:.0f} ms per frame", settings.LoadingPumpMs) : std::string("off"),
		settings.LoadingPump ? static_cast<double>(settings.BlackWindowMs) : kBlackMaxMs, pumpFrames, pumpCpuTotalMs, pumpForeignFrames, pumpMidTeleport,
		gate.noted, gate.captured, gate.byEvent, gate.unsettledTotal);
	// (46n) The preparation from the cell events (main thread, behind the loading screen) and the fade-in budgets.
	{
		const auto& c = cellEventLog;
		logger::info("[RayTracing] Cell-event preparation ({}, {:.0f} ms per event; fade-in budgets {}): {} events, {} steps run ({:.0f} ms CPU, longest {:.1f} ms, "
					 "{} in the middle of the teleport), {} events on another thread, {} skipped{}; {} cells captured, {} shapes, {} BLAS built in the steps",
			!settings.PrepareOnCellEvents ? "off" : (cellEventBroken ? "off after a fault" : "on"), settings.CellEventMs, settings.FadeInBudgets ? "on" : "off",
			c.events, c.steps, c.cpuMs, c.maxMs, c.midTeleport, cellEventForeign.exchange(0, std::memory_order_relaxed), c.skipped,
			c.lastSkip.empty() ? std::string() : std::format(" (last: {})", c.lastSkip), c.cellsCaptured, c.shapes, c.builds);
		cellEventLog = {};
	}
	for (auto& entry : transitionLog)
		entry = {};
	transitionLogSlot = -1;
}

void RayTracing::UpdateBeamGuard()
{
	// (46g) Window beams whose traced cone is (almost) fully blocked are not believed: with the room's own shell in the way
	// the beam's patch would vanish. 30 read-backs over 97 % (with enough sampled pixels) -> no source for 600 frames (the
	// engine's shadow map stays for that beam), then it is tried again.
	const auto& stats = backend.GetStats();
	const std::uint32_t frame = globals::state->frameCount;
	if (stats.beamSamples != beamSamplesSeen) {
		beamSamplesSeen = stats.beamSamples;
		for (std::uint32_t slot = 0; slot < lastSlotBeams.size(); ++slot) {
			const auto* light = lastSlotBeams[slot];
			if (!light)
				continue;
			const std::uint32_t traced = stats.beamTraced[slot], blocked = stats.beamBlocked[slot];
			if (beamSharePending && frame - beamShareFrom < 0x80000000u) {  // (46n) the warm-up report
				auto& share = beamShares[light];
				share.traced += traced;
				share.blocked += blocked;
			}
			if (traced < 64)
				continue;  // too little of the cone on screen to judge
			auto& guard = beamGuards[light];
			if (guard.implausible)
				continue;
			guard.over = static_cast<double>(blocked) > 0.97 * static_cast<double>(traced) ? guard.over + 1 : 0;
			if (guard.over >= 30) {
				guard.implausible = true;
				guard.until = frame + 600;
				++beamsImplausible;
				logger::info("[RayTracing] Window beam {} looks implausible ({} of {} sampled cone pixels blocked): the engine's shadow map is kept for it "
							 "for 600 frames (Point Light Shadows > Window Beam Guard)",
					static_cast<const void*>(light), blocked, traced);
			}
		}
	}
	excludedBeams.clear();
	for (auto it = beamGuards.begin(); it != beamGuards.end();) {
		auto& guard = it->second;
		if (guard.implausible && frame - guard.until < 0x80000000u) {
			// Retry: believed again until the counters say otherwise.
			guard.implausible = false;
			guard.over = 0;
		}
		if (guard.implausible)
			excludedBeams.push_back(it->first);
		if (!guard.implausible && guard.over == 0 && beamGuards.size() > 256)
			it = beamGuards.erase(it);
		else
			++it;
	}
	std::sort(excludedBeams.begin(), excludedBeams.end());
	// (46n) After the warm-up of an interior: the blocked share of every beam that was sampled, once.
	if (beamSharePending && frame - beamShareUntil < 0x80000000u) {
		beamSharePending = false;
		std::vector<std::pair<const RE::BSLight*, BeamShare>> list(beamShares.begin(), beamShares.end());
		std::sort(list.begin(), list.end(), [](const auto& a_l, const auto& a_r) { return a_l.second.traced > a_r.second.traced; });
		std::string text;
		std::uint64_t traced = 0, blocked = 0;
		for (const auto& [light, share] : list) {
			traced += share.traced;
			blocked += share.blocked;
			if (text.size() < 1200)
				text += std::format("{}{}: {:.0f} % of {}", text.empty() ? "" : ", ", static_cast<const void*>(light),
					share.traced ? 100.0 * static_cast<double>(share.blocked) / static_cast<double>(share.traced) : 0.0, share.traced);
		}
		logger::info("[RayTracing] Window beams after the warm-up (Window Beam Culling mode {}): {} beams sampled, {:.0f} % of all sampled cone pixels blocked{}{}",
			EffectiveBeamCull(), list.size(), traced ? 100.0 * static_cast<double>(blocked) / static_cast<double>(traced) : 0.0,
			text.empty() ? "" : "; per beam: ", text);
		beamShares.clear();
	}
}

bool RayTracing::ClaimScreenSpaceShadows()
{
	// (45k) Decided before Ray Tracing's own Prepass (Screen Space Shadows runs first in the list).
	sssClaimed = false;
	if (!settings.Enabled || status != Status::Active || !typedUavLoadR8 || Util::IsInterior() || phase == Phase::Loading || !handedOff)
		return false;
	sssClaimed = true;
	return true;
}

void RayTracing::UpdateAlphaCasters()
{
	// (45h, 45j) Alpha-tested shapes are traced with their cut-out when Foliage is Ray traced / Both
	// or Cut-outs is Ray traced. Turned on (again) by the user: an earlier automatic switch-off is forgotten.
	const bool wanted = settings.Foliage != static_cast<int>(RayTracingScene::FoliageMode::ShadowMap) || settings.Cutouts;
	if (wanted && !alphaRequested) {
		backend.ClearAlphaFault();
		alphaOffReason.clear();
	}
	// Any failure of its D3D12 / D3D11 resources switches it off (the rest of Ray Tracing goes on).
	auto switchOff = [&](std::string a_reason) {
		alphaOffReason = std::move(a_reason);
		settings.Foliage = static_cast<int>(RayTracingScene::FoliageMode::ShadowMap);
		settings.Cutouts = false;
		logger::warn("[RayTracing] Ray-traced foliage / cut-outs switched off: {}", alphaOffReason);
	};
	if (wanted && !backend.AlphaFault().empty())
		switchOff(backend.AlphaFault());
	else if (wanted && backend.IsReady() && !backend.AlphaAvailable())
		switchOff("the GPU driver did not accept the alpha-tested ray query shader");
	alphaRequested = settings.Foliage != static_cast<int>(RayTracingScene::FoliageMode::ShadowMap) || settings.Cutouts;
	scene.Configure(SceneConfigFromSettings());  // a change re-captures the scene
	const bool alphaInUse = scene.AlphaWanted() || (settings.Characters && backend.SkinAlphaAvailable());
	if (!alphaInUse && backend.IsReady() && backend.AlphaIdle())
		backend.ReleaseAlphaResources();
}

RayTracingScene::SceneConfig RayTracing::SceneConfigFromSettings() const
{
	RayTracingScene::SceneConfig config;
	config.buildings = settings.Buildings;
	config.objects = settings.Objects;
	config.trees = settings.Trees;
	const bool alpha = backend.AlphaAvailable();
	config.foliage = alpha ? static_cast<RayTracingScene::FoliageMode>(std::clamp(settings.Foliage, 0, 2)) : RayTracingScene::FoliageMode::ShadowMap;
	config.cutouts = alpha && settings.Cutouts;
	config.cutoutAsOpaque = true;
	// (46c) Window Shadows Ultimate's window masks indoors: with the alpha-tested point-light trace only.
	config.windowMasks = alpha && backend.PointAlphaAvailable() && PointLightsOn() && InteriorsOn();
	return config;
}

void RayTracing::UpdateCharacters()
{
	// (45i) Turned on (again) by the user: an earlier automatic switch-off is forgotten.
	if (settings.Characters && !charactersRequested) {
		backend.ClearSkinFault();
		charactersOffReason.clear();
	}
	// Any failure of its D3D12 resources switches it off (the rest of Ray Tracing goes on).
	if (settings.Characters && !backend.SkinFault().empty()) {
		charactersOffReason = backend.SkinFault();
		settings.Characters = false;
		logger::warn("[RayTracing] Characters switched off: {}", charactersOffReason);
	} else if (settings.Characters && backend.IsReady() && !backend.SkinAvailable()) {
		charactersOffReason = "the GPU driver did not accept the character skinning / ray query shaders";
		settings.Characters = false;
		logger::warn("[RayTracing] Characters switched off: {}", charactersOffReason);
	}
	charactersRequested = settings.Characters;
	if (!settings.Characters || !backend.SkinAvailable()) {
		if (!characters.Empty())
			characters.ReleaseAll(backend);
		if (backend.IsReady() && backend.SkinIdle())
			backend.ReleaseSkinResources();
		return;
	}
	// (45j) Alpha-tested parts (fur trims, cut-out cloth; hair when Hair is ray traced) whenever the
	// alpha pipeline exists; a change of the first three re-captures the characters.
	// (46m) Read lanes, scan cache, merged BLASes (a change of the last captures the characters again).
	characters.Configure(settings.PhysicsParts, settings.Hair, backend.SkinAlphaAvailable(), settings.FirstPersonPlayer, settings.CharactersParallel,
		settings.CharactersScanCache, settings.CharactersMerge);
}


// ---------------------------------------------------------------------------------------------
// (45l) Quality tiers, denoiser in effect
// ---------------------------------------------------------------------------------------------

RayTracing::QualityParams RayTracing::ResolveQuality() const
{
	QualityParams q;
	const int level = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
	if (level == static_cast<int>(Quality::Custom)) {
		q.baseRays = static_cast<std::uint32_t>(std::clamp(settings.CustomBaseRays, 1, 4));
		q.extraRays = static_cast<std::uint32_t>(std::clamp(settings.CustomExtraRays, 0, 8));
		q.coneDegrees = settings.CustomSunCone;
		q.smallFilterRadius = settings.CustomSunCone > 0.4f ? 2u : 1u;
		q.tracePattern = static_cast<std::uint32_t>(std::clamp(settings.CustomTraceResolution, 0, 2));
		q.farPattern = static_cast<std::uint32_t>(std::clamp(settings.CustomFarQuality, 0, 2));
		q.falloffStart = settings.CustomFalloffStart;
		q.falloffEnd = settings.CustomFalloffEnd;
		q.npcReduced = settings.CustomNpcQuality == 1;
		q.denoiseRadius = settings.CustomDenoiseRadius;
		q.denoiseTaps = static_cast<std::uint32_t>(std::clamp(settings.CustomDenoiseTaps, 1, 8));
		q.denoiseHalf = settings.CustomDenoiseHalf;
		q.farDenoiseScale = settings.CustomFarDenoiseScale;
		q.history = static_cast<float>(std::clamp(settings.CustomHistory, 1, 32));
		q.clampSigma = settings.CustomDenoiseClamp;
	} else {
		const auto& t = kTiers[level];
		q.baseRays = t.baseRays;
		q.extraRays = t.extraRays;
		q.coneDegrees = t.coneDegrees;
		q.smallFilterRadius = t.smallFilterRadius;
		q.tracePattern = t.tracePattern;
		q.farPattern = t.farPattern;
		q.falloffStart = t.falloffStart;
		q.falloffEnd = t.falloffEnd;
		q.npcReduced = t.npcReduced;
		q.denoiseRadius = t.denoiseRadius;
		q.denoiseTaps = t.denoiseTaps;
		q.denoiseHalf = t.denoiseHalf;
		q.farDenoiseScale = t.farDenoiseScale;
	}
	return q;
}

void RayTracing::CopyTierToCustom(int a_tier)
{
	const auto& t = kTiers[std::clamp(a_tier, 0, 4)];
	settings.CustomBaseRays = static_cast<int>(t.baseRays);
	settings.CustomExtraRays = static_cast<int>(t.extraRays);
	settings.CustomSunCone = t.coneDegrees;
	settings.CustomTraceResolution = static_cast<int>(t.tracePattern);
	settings.CustomFarQuality = static_cast<int>(t.farPattern);
	settings.CustomFalloffStart = t.falloffStart;
	settings.CustomFalloffEnd = t.falloffEnd;
	settings.CustomNpcQuality = t.npcReduced ? 1 : 0;
	settings.CustomDenoiseRadius = t.denoiseRadius;
	settings.CustomDenoiseTaps = static_cast<int>(t.denoiseTaps);
	settings.CustomDenoiseHalf = t.denoiseHalf;
	settings.CustomFarDenoiseScale = t.farDenoiseScale;
	settings.CustomHistory = 12;
	settings.CustomDenoiseClamp = 1.5f;
}

bool RayTracing::UpscalingRRActive()
{
	auto& upscaling = globals::features::upscaling;
	return upscaling.loaded && upscaling.settings.enableDLSSRR && Upscaling::streamline.featureDLSS_RR &&
	       upscaling.GetUpscaleMethod() == Upscaling::UpscaleMethod::kDLSS;
}

RayTracing::DenoiserMode RayTracing::EffectiveDenoiser() const
{
	// Ray Reconstruction cleans the whole image, shadows included, but only raw per-frame noise:
	// whenever it runs, the shadows take the DLSS RR route whatever the dropdown says.
	if (UpscalingRRActive())
		return DenoiserMode::DLSSRR;
	return CurrentDenoiser();
}

void RayTracing::OnDenoiserPicked()
{
	// The same setting as Upscaling's "Ray Reconstruction" checkbox (Upscaling rebuilds its resources on its next frame).
	auto& upscaling = globals::features::upscaling;
	if (!upscaling.loaded)
		return;
	if (CurrentDenoiser() == DenoiserMode::DLSSRR) {
		if (Upscaling::streamline.featureDLSS_RR && !upscaling.settings.enableDLSSRR) {
			upscaling.settings.enableDLSSRR = true;
			logger::info("[RayTracing] Denoiser DLSS RR: Ray Reconstruction switched on in Upscaling");
		}
	} else if (upscaling.settings.enableDLSSRR) {
		upscaling.settings.enableDLSSRR = false;
		logger::info("[RayTracing] Denoiser changed away from DLSS RR: Ray Reconstruction switched off in Upscaling");
	}
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void RayTracing::LoadSettings(json& o_json)
{
	// Missing keys keep their defaults; keys of removed switches are ignored (and dropped on the next save).
	int loadedVersion = 0;  // (46o)
	if (const auto it = o_json.find("SettingsVersion"); it != o_json.end() && it->is_number_integer())
		loadedVersion = it->get<int>();
	settings = o_json;
	// (46n) Outside the macro (it takes at most 64 members).
	{
		const Settings defaults{};
		const auto read = [&o_json](const char* a_key, auto a_default) {
			const auto it = o_json.find(a_key);
			if (it == o_json.end())
				return a_default;
			try {
				return it->template get<decltype(a_default)>();
			} catch (const std::exception&) {
				return a_default;
			}
		};
		settings.PrepareOnCellEvents = read("PrepareOnCellEvents", defaults.PrepareOnCellEvents);
		settings.CellEventMs = read("CellEventMs", defaults.CellEventMs);
		settings.FadeInBudgets = read("FadeInBudgets", defaults.FadeInBudgets);
		settings.PointLightsBeamSoftness = read("PointLightsBeamSoftness", defaults.PointLightsBeamSoftness);  // (46o)
		settings.ParallelBlasBuilds = read("ParallelBlasBuilds", defaults.ParallelBlasBuilds);  // (46p)
		settings.HighPriorityQueue = read("HighPriorityQueue", defaults.HighPriorityQueue);
		settings.SubmitWorker = read("SubmitWorker", defaults.SubmitWorker);  // (46q)
		settings.BuildGpuBudget = read("BuildGpuBudget", defaults.BuildGpuBudget);
		settings.BuildGpuBudgetMs = std::clamp(read("BuildGpuBudgetMs", defaults.BuildGpuBudgetMs), 0.25f, 4.0f);
		settings.GroupBuildsPerFrame = std::clamp(read("GroupBuildsPerFrame", defaults.GroupBuildsPerFrame), 1, 16);
		settings.GroupGraceMs = std::clamp(read("GroupGraceMs", defaults.GroupGraceMs), 0.0f, 10000.0f);
		settings.CarrierShadows = read("CarrierShadows", defaults.CarrierShadows);
		settings.CarrierClearance = std::clamp(read("CarrierClearance", defaults.CarrierClearance), 0.0f, 64.0f);
		settings.PointLightsSteady = read("PointLightsSteady", defaults.PointLightsSteady);  // (46s)
		settings.PointLightsMergeHysteresis = read("PointLightsMergeHysteresis", defaults.PointLightsMergeHysteresis);
		settings.PointLightsMeshless = std::clamp(read("PointLightsMeshless", defaults.PointLightsMeshless), 0, 2);
	}
	// (46o) Window Beam Guard: off once for settings saved before 46o (its rule misfired on beams that mostly hit walls).
	if (loadedVersion < 6 && settings.PointLightsBeamGuard) {
		settings.PointLightsBeamGuard = false;
		logger::info("[RayTracing] Window Beam Guard switched off (46o default; tick it again in Point Light Shadows if wanted)");
	}
	if (settings.Denoiser == 1)
		logger::info("[RayTracing] Denoiser Temporal was removed: Penumbra");
	settings.Denoiser = ValidDenoiser(settings.Denoiser);
	settings.SettingsVersion = Settings{}.SettingsVersion;
	settings.QualityLevel = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
	settings.CustomBaseRays = std::clamp(settings.CustomBaseRays, 1, 4);
	settings.CustomExtraRays = std::clamp(settings.CustomExtraRays, 0, 8);
	settings.CustomSunCone = std::isfinite(settings.CustomSunCone) ? std::clamp(settings.CustomSunCone, 0.1f, 2.0f) : 0.27f;
	settings.CustomFalloffStart = std::isfinite(settings.CustomFalloffStart) ? std::clamp(settings.CustomFalloffStart, 0.0f, 32768.0f) : 4096.0f;
	settings.CustomFalloffEnd = std::isfinite(settings.CustomFalloffEnd) ? std::clamp(settings.CustomFalloffEnd, 0.0f, 65536.0f) : 12288.0f;
	settings.CustomFarQuality = std::clamp(settings.CustomFarQuality, 0, 2);
	settings.CustomTraceResolution = std::clamp(settings.CustomTraceResolution, 0, 2);
	settings.CustomNpcQuality = std::clamp(settings.CustomNpcQuality, 0, 1);
	settings.CustomDenoiseRadius = std::isfinite(settings.CustomDenoiseRadius) ? std::clamp(settings.CustomDenoiseRadius, 2.0f, 32.0f) : 16.0f;
	settings.CustomDenoiseTaps = std::clamp(settings.CustomDenoiseTaps, 1, 8);
	settings.CustomDenoiseClamp = std::isfinite(settings.CustomDenoiseClamp) ? std::clamp(settings.CustomDenoiseClamp, 0.5f, 4.0f) : 1.5f;
	settings.CustomHistory = std::clamp(settings.CustomHistory, 1, 32);
	settings.CustomFarDenoiseScale = std::isfinite(settings.CustomFarDenoiseScale) ? std::clamp(settings.CustomFarDenoiseScale, 0.0f, 1.0f) : 0.75f;
	settings.SigmaStabilizedFrames = std::clamp(settings.SigmaStabilizedFrames, 0, 5);
	settings.Foliage = std::clamp(settings.Foliage, 0, 2);
	settings.RtContactScale = std::isfinite(settings.RtContactScale) ? std::clamp(settings.RtContactScale, 0.0f, 32.0f) : 8.0f;
	// (46b)
	settings.PointLightsPerPixel = ValidPointK(settings.PointLightsPerPixel);
	settings.PointLightsMaxSources = std::clamp(settings.PointLightsMaxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources));
	settings.PointLightsDistance = std::isfinite(settings.PointLightsDistance) ? std::clamp(settings.PointLightsDistance, 512.0f, 8192.0f) : 4000.0f;
	settings.PointLightsSize = std::isfinite(settings.PointLightsSize) ? std::clamp(settings.PointLightsSize, 0.0f, 32.0f) : 6.0f;
	settings.PointLightsClearance = std::isfinite(settings.PointLightsClearance) ? std::clamp(settings.PointLightsClearance, 0.0f, 128.0f) : 32.0f;
	// (46f)
	settings.AOMode = std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1);
	settings.AORayLength = std::isfinite(settings.AORayLength) ? std::clamp(settings.AORayLength, kAORayLengthMin, kAORayLengthMax) : 100.0f;
	settings.AOStrength = std::isfinite(settings.AOStrength) ? std::clamp(settings.AOStrength, 0.0f, 2.0f) : 1.0f;
	settings.AODistance = std::isfinite(settings.AODistance) ? std::clamp(settings.AODistance, 1000.0f, 8192.0f) : 4000.0f;
	settings.AOModeInterior = std::clamp(settings.AOModeInterior, 0, static_cast<int>(AOModeKind::Count) - 1);  // (46h)
	// (46k)
	settings.LoadingPumpMs = std::isfinite(settings.LoadingPumpMs) ? std::clamp(settings.LoadingPumpMs, 2.0f, 32.0f) : 8.0f;
	settings.BlackWindowMs = std::isfinite(settings.BlackWindowMs) ? std::clamp(settings.BlackWindowMs, 0.0f, 2000.0f) : 200.0f;
	settings.CellEventMs = std::isfinite(settings.CellEventMs) ? std::clamp(settings.CellEventMs, 2.0f, 50.0f) : 12.0f;  // (46n)
	settings.PointLightsBeamSoftness = std::isfinite(settings.PointLightsBeamSoftness) ? std::clamp(settings.PointLightsBeamSoftness, 0.0f, 3.0f) : 0.5f;  // (46o)
}

void RayTracing::SaveSettings(json& o_json)
{
	o_json = settings;
	// (46n) Outside the macro (it takes at most 64 members).
	o_json["PrepareOnCellEvents"] = settings.PrepareOnCellEvents;
	o_json["CellEventMs"] = settings.CellEventMs;
	o_json["FadeInBudgets"] = settings.FadeInBudgets;
	o_json["PointLightsBeamSoftness"] = settings.PointLightsBeamSoftness;  // (46o)
	o_json["ParallelBlasBuilds"] = settings.ParallelBlasBuilds;  // (46p)
	o_json["HighPriorityQueue"] = settings.HighPriorityQueue;
	o_json["SubmitWorker"] = settings.SubmitWorker;  // (46q)
	o_json["BuildGpuBudget"] = settings.BuildGpuBudget;
	o_json["BuildGpuBudgetMs"] = settings.BuildGpuBudgetMs;
	o_json["GroupBuildsPerFrame"] = settings.GroupBuildsPerFrame;
	o_json["GroupGraceMs"] = settings.GroupGraceMs;
	o_json["CarrierShadows"] = settings.CarrierShadows;
	o_json["CarrierClearance"] = settings.CarrierClearance;
	o_json["PointLightsSteady"] = settings.PointLightsSteady;  // (46s)
	o_json["PointLightsMergeHysteresis"] = settings.PointLightsMergeHysteresis;
	o_json["PointLightsMeshless"] = settings.PointLightsMeshless;
}

void RayTracing::RestoreDefaultSettings()
{
	settings = {};
	debugView = DebugView::Off;
	aoDebugView = AODebugView::Off;
}

void RayTracing::DataLoaded()
{
	if (!loadingFlushRegistered) {
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_loadingSink);
			loadingFlushRegistered = true;
		}
	}
	// (46k) The engine's transition events; the main thread (SKSE's data-loaded message) until a Prepass names the render
	// thread (the same one in SE).
	RegisterEventSinks();
	if (renderThreadId.load() == 0)
		renderThreadId.store(static_cast<std::uint32_t>(GetCurrentThreadId()));
}

void RayTracing::PostPostLoad()
{
	// (46c) Inert unless Stop Engine Shadow Maps is on indoors (SkipShadowMap answers false otherwise). SE / AE only
	// (Ray Tracing does not run in VR).
	if (hooksInstalled || REL::Module::IsVR())
		return;
	stl::write_vfunc<0xA, FrustumLightRenderShadowmaps>(RE::VTABLE_BSShadowFrustumLight[0]);
	stl::write_vfunc<0xA, ParabolicLightRenderShadowmaps>(RE::VTABLE_BSShadowParabolicLight[0]);
	hooksInstalled = true;
	logger::info("[RayTracing] Hooked the spot / omni shadow map rendering (used only with Stop Engine Shadow Maps)");
}

bool RayTracing::SkipShadowMap(RE::BSShadowLight* a_light)
{
	// Render thread, while the engine draws its shadow maps (after LatchFrame decided this frame).
	if (!skipLatched || !a_light || skippedMaps.size() >= 4)
		return false;
	if (!std::binary_search(skipCandidates.begin(), skipCandidates.end(), static_cast<const RE::BSLight*>(a_light)))
		return false;
	const std::uint32_t channel = ReadShadowChannel(a_light);
	if (channel > 3)
		return false;
	SkippedMap skipped{ a_light, channel };
	// (46e) Its cone from last frame's trace (the fallback, should this frame's RT result be missing).
	const auto it = std::lower_bound(skipEntries.begin(), skipEntries.end(), static_cast<const RE::BSLight*>(a_light),
		[](const RayTracingPointLights::MaskEntry& a_entry, const RE::BSLight* a_key) { return a_entry.light < a_key; });
	if (it != skipEntries.end() && it->light == a_light) {
		skipped.hasEntry = true;
		skipped.entry = *it;
	}
	skippedMaps.push_back(skipped);
	skippedLights.insert(std::upper_bound(skippedLights.begin(), skippedLights.end(), static_cast<const RE::BSLight*>(a_light)), a_light);
	++skippedTotal;
	return true;
}

void RayTracing::SetupResources()
{
	combineCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CombineCB>());
	pointDenoiseCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PointDenoiseCB>());  // (46b)
	pointMaskCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PointMaskCB>());        // (46c)
	aoCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<AOCompositeCB>());            // (46f)

	// Screen Space Shadows hand-off reads its R8 texture through a UAV.
	D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support{ DXGI_FORMAT_R8_UNORM, 0 };
	if (SUCCEEDED(globals::d3d::device->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support, sizeof(support))))
		typedUavLoadR8 = (support.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD) != 0;
}

void RayTracing::ClearShaderCache()
{
	prepareCS = nullptr;
	combineCS = nullptr;
	overlayCS = nullptr;
	alphaMaskCS = nullptr;
	denoiseTileCS = nullptr;
	denoiseHCS = nullptr;
	denoiseVCS = nullptr;
	denoiseHalfHCS = nullptr;
	denoiseHalfVCS = nullptr;
	sigmaPrepCS = nullptr;
	pointDenoiseCS = nullptr;  // (46b)
	pointMaskCS = nullptr;     // (46c)
	// (46f)
	aoDownCS = nullptr;
	aoBlurHCS = nullptr;
	aoBlurVCS = nullptr;
	aoUpCS = nullptr;
	aoTemporalCS = nullptr;
	aoCompositeCS = nullptr;
}

void RayTracing::PrecompileShaders()
{
	// (46p) What the first traced frame would compile on the spot (26.6 ms in 46o's post-load log): the same getters, now.
	if (!loaded || !settings.Enabled)
		return;
	GetShader(prepareCS, L"Data\\Shaders\\RayTracing\\PrepareCS.hlsl", "PrepareCS");
	GetShader(combineCS, L"Data\\Shaders\\RayTracing\\CombineCS.hlsl", "CombineCS");
	if (ValidDenoiser(settings.Denoiser) == static_cast<int>(DenoiserMode::Penumbra)) {
		GetShader(denoiseTileCS, L"Data\\Shaders\\RayTracing\\DenoiseTileCS.hlsl", "DenoiseTileCS");
		GetShaderDefined(denoiseHalfHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (half)", "DENOISE_HALF");
		GetShaderDefined(denoiseHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS", nullptr);
		GetShaderDefined(denoiseHalfVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (half, vertical)", "DENOISE_HALF,DENOISE_VERTICAL");
		GetShaderDefined(denoiseVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (vertical)", "DENOISE_VERTICAL");
	}
	if (settings.PointLights) {
		GetShader(pointDenoiseCS, L"Data\\Shaders\\RayTracing\\PointDenoiseCS.hlsl", "PointDenoiseCS");
		GetShader(pointMaskCS, L"Data\\Shaders\\RayTracing\\PointMaskCS.hlsl", "PointMaskCS");
	}
}

ID3D11ComputeShader* RayTracing::GetShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name)
{
	if (!a_shader) {
		a_shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(a_path, {}, "cs_5_0")));
		if (!a_shader)
			logger::error("[RayTracing] Could not compile {}", a_name);
	}
	return a_shader.get();
}

// ---------------------------------------------------------------------------------------------
// Lifetime
// ---------------------------------------------------------------------------------------------

void RayTracing::Disable(Status a_status, std::string a_reason)
{
	status = a_status;
	statusReason = a_reason.empty() ? std::string("unknown error (see the log)") : std::move(a_reason);
	lastFailureReason = statusReason;
	activeThisFrame = false;
	pending.active = false;  // (45l)
	// (46b) No point-light tags, nothing bound for the pixel shaders.
	pointSelector.Clear();
	pointThisFrame = false;
	pointHistoryValid = false;
	UnbindPointLights();
	// (46c) Nothing indoors either: the engine draws every shadow map again from the next frame on.
	interiorThisFrame = false;
	interiorTracedLastFrame = false;
	skipLatched = false;
	skipCandidates.clear(); skipEntries.clear();
	aoThisFrame = false;  // (46f) SSGI's own AO from the next frame on
	aoHistoryValid = false;
	RayTracingCasterFilter::Clear();
	characters.ReleaseAll(backend);  // (45i) (a backend that is gone ignores the handles)
	scene.ReleaseAll(backend);
	// After a D3D12 failure the device is gone or unusable: no GPU wait, drop everything.
	if (backend.HasFailed())
		backend.Shutdown(false);
	logger::warn("[RayTracing] Off: {}", statusReason);
}

bool RayTracing::EnsureBackend()
{
	if (backend.IsReady())
		return true;
	if (backendTried) {
		// Never sit at "Starting": a start-up that already failed shows its reason (and Try Again).
		if (status != Status::Unavailable && status != Status::Failed) {
			status = Status::Unavailable;
			statusReason = lastFailureReason.empty() ? std::string("start-up failed (see the log)") : lastFailureReason;
		}
		return false;
	}
	backendTried = true;
	std::string reason;
	backend.SetQueuePriorityHigh(settings.HighPriorityQueue);  // (46p) read at start-up only
	try {
		if (!backend.Initialize(globals::d3d::device, globals::d3d::context, reason)) {
			status = Status::Unavailable;
			statusReason = reason;
			lastFailureReason = reason;
			return false;
		}
	} catch (const std::exception& e) {
		status = Status::Unavailable;
		statusReason = std::format("exception during start-up: {}", e.what());
		lastFailureReason = statusReason;
		logger::error("[RayTracing] {}", statusReason);
		backend.Shutdown(false);
		return false;
	}
	logger::info("[RayTracing] Started");
	return true;
}

void RayTracing::ReleaseD3D11Resources()
{
	maskTemp = nullptr;
	maskTempUAV = nullptr;
	debugColor = nullptr;
	debugColorUAV = nullptr;
	debugColorSRV = nullptr;
	ReleaseDenoiser();
	ReleaseSigma();
	ReleasePointDenoiser();  // (46b)
	ReleaseAOFilter();       // (46f)
	prevCameraValid = false;
	maskDesc = {};
	d3d11Bytes = 0;
}

void RayTracing::ReleaseDenoiser()
{
	for (auto* t : { &denoiseTiles, &denoiseH, &denoiseOut, &denoiseHalfIn, &history4[0], &history4[1] })
		*t = {};
	history4Valid = false;
	d3d11Bytes -= std::min(d3d11Bytes, denoiseBytes);
	denoiseBytes = 0;
}

namespace
{
	bool CreateDenoiseTexture(RayTracing* a_self, winrt::com_ptr<ID3D11Texture2D>& o_texture, winrt::com_ptr<ID3D11ShaderResourceView>& o_srv,
		winrt::com_ptr<ID3D11UnorderedAccessView>& o_uav, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, const char* a_name)
	{
		(void)a_self;
		D3D11_TEXTURE2D_DESC d{};
		d.Width = std::max(a_width, 1u);
		d.Height = std::max(a_height, 1u);
		d.MipLevels = 1;
		d.ArraySize = 1;
		d.Format = a_format;
		d.SampleDesc = { 1, 0 };
		d.Usage = D3D11_USAGE_DEFAULT;
		d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
		auto* device = globals::d3d::device;
		if (FAILED(device->CreateTexture2D(&d, nullptr, o_texture.put())) || FAILED(device->CreateShaderResourceView(o_texture.get(), nullptr, o_srv.put())) ||
			FAILED(device->CreateUnorderedAccessView(o_texture.get(), nullptr, o_uav.put())))
			return false;
		Util::SetResourceName(o_texture.get(), a_name);
		return true;
	}
}

bool RayTracing::EnsureDenoiser(bool a_half)
{
	// (45k) Denoiser Penumbra: created on first use (render-target size of the shadow
	// mask). (45l) The filter textures are half size with a half-resolution filter.
	if (denoiseTiles.texture && denoiseH.texture && denoiseOut.texture && history4[0].texture && history4[1].texture && denoiseIsHalf == a_half &&
		(!a_half || denoiseHalfIn.texture))
		return true;
	if (!maskTemp)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	maskTemp->GetDesc(&desc);
	ReleaseDenoiser();
	auto create = [&](DenoiseTexture& o_texture, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint32_t a_bytesPerPixel,
					  const char* a_name) {
		if (!CreateDenoiseTexture(this, o_texture.texture, o_texture.srv, o_texture.uav, a_width, a_height, a_format, a_name))
			return false;
		denoiseBytes += static_cast<std::uint64_t>(a_width) * a_height * a_bytesPerPixel;
		return true;
	};
	const std::uint32_t fw = a_half ? (desc.Width + 1) / 2 : desc.Width, fh = a_half ? (desc.Height + 1) / 2 : desc.Height;
	bool ok = create(denoiseTiles, (desc.Width + 7) / 8, (desc.Height + 7) / 8, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "RayTracing DenoiseTiles") &&
	          create(denoiseH, fw, fh, DXGI_FORMAT_R16G16_FLOAT, 4, "RayTracing DenoiseH") &&
	          create(denoiseOut, fw, fh, DXGI_FORMAT_R16G16_FLOAT, 4, "RayTracing Denoised") &&
	          create(history4[0], desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "RayTracing History4") &&
	          create(history4[1], desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "RayTracing History4");
	if (ok && a_half)
		ok = create(denoiseHalfIn, fw, fh, DXGI_FORMAT_R16G16_FLOAT, 4, "RayTracing DenoiseHalfIn");
	if (!ok) {
		ReleaseDenoiser();
		logger::warn("[RayTracing] Denoiser: could not create its textures; Denoiser set to Off");
		settings.Denoiser = static_cast<int>(DenoiserMode::Off);
		return false;
	}
	denoiseIsHalf = a_half;
	d3d11Bytes += denoiseBytes;
	history4Valid = false;
	return true;
}

ID3D11ComputeShader* RayTracing::GetShaderDefined(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name, const char* a_define)
{
	if (!a_shader) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (a_define) {
			// (45l) Several defines separated by ','.
			static thread_local std::vector<std::string> parts;
			parts.clear();
			std::string all = a_define;
			for (std::size_t start = 0; start <= all.size();) {
				const std::size_t comma = all.find(',', start);
				const std::size_t end = comma == std::string::npos ? all.size() : comma;
				if (end > start)
					parts.push_back(all.substr(start, end - start));
				start = end + 1;
			}
			for (const auto& part : parts)
				defines.emplace_back(part.c_str(), "");
		}
		a_shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(a_path, defines, "cs_5_0")));
		if (!a_shader)
			logger::error("[RayTracing] Could not compile {}", a_name);
	}
	return a_shader.get();
}

void RayTracing::ReleaseSigma()
{
	sigma.reset();
	for (auto* t : { &sigmaViewZ, &sigmaNormal, &sigmaMotion, &sigmaPenumbra, &sigmaOut })
		*t = {};
	d3d11Bytes -= std::min(d3d11Bytes, sigmaBytes);
	sigmaBytes = 0;
	sigmaHistoryValid = false;
}

bool RayTracing::EnsureSigma()
{
	// (45l) Denoiser NRD SIGMA: its own NRD instance (SIGMA_SHADOW, identifier 45) and its inputs at the
	// size of the shadow mask; the render rect is passed every frame.
	if (sigma && sigma->IsValid() && sigmaOut.texture)
		return true;
	if (!maskTemp || !sigmaFault.empty())
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	maskTemp->GetDesc(&desc);
	ReleaseSigma();
	try {
		auto create = [&](DenoiseTexture& o_texture, DXGI_FORMAT a_format, std::uint32_t a_bytesPerPixel, const char* a_name) {
			if (!CreateDenoiseTexture(this, o_texture.texture, o_texture.srv, o_texture.uav, desc.Width, desc.Height, a_format, a_name))
				return false;
			sigmaBytes += static_cast<std::uint64_t>(desc.Width) * desc.Height * a_bytesPerPixel;
			return true;
		};
		const bool ok = create(sigmaViewZ, DXGI_FORMAT_R32_FLOAT, 4, "RayTracing SIGMA ViewZ") &&
		                create(sigmaNormal, DXGI_FORMAT_R10G10B10A2_UNORM, 4, "RayTracing SIGMA NormalRoughness") &&
		                create(sigmaMotion, DXGI_FORMAT_R16G16_FLOAT, 4, "RayTracing SIGMA Motion") &&
		                create(sigmaPenumbra, DXGI_FORMAT_R16_FLOAT, 2, "RayTracing SIGMA Penumbra") &&
		                create(sigmaOut, DXGI_FORMAT_R8_UNORM, 1, "RayTracing SIGMA Shadow");
		if (!ok)
			throw std::runtime_error("could not create its textures");
		sigma = std::make_unique<NRDReblurIntegration>();
		if (!sigma->Init(desc.Width, desc.Height, nrd::Denoiser::SIGMA_SHADOW, 45))
			throw std::runtime_error("NRD did not create the SIGMA instance");
		sigma->SetTimingGroup("NRD SIGMA (RT shadows)");
	} catch (const std::exception& e) {
		sigmaFault = e.what();
		logger::warn("[RayTracing] NRD SIGMA: {}; Denoiser set to Penumbra", sigmaFault);
		ReleaseSigma();
		settings.Denoiser = static_cast<int>(DenoiserMode::Penumbra);
		return false;
	}
	d3d11Bytes += sigmaBytes;
	sigmaHistoryValid = false;
	logger::info("[RayTracing] NRD SIGMA ready ({}x{}, {})", desc.Width, desc.Height, Megabytes(sigmaBytes));
	return true;
}

bool RayTracing::RunSigma(const float a_toSun[3])
{
	// (45l) Prepare SIGMA's inputs from the trace and the depth, then its dispatches. CommonSettings are
	// our own (not the NRD service's: its per-frame cache feeds the SSRT instances' history): matrices as
	// the NRD service builds them, jitter from Upscaling, frameIndex +1 per SIGMA frame only (hard constraint
	// #4; timeDeltaBetweenFrames is left to NRD).
	auto* prep = GetShader(sigmaPrepCS, L"Data\\Shaders\\RayTracing\\SigmaPrepCS.hlsl", "SigmaPrepCS");
	if (!prep || !sigma)
		return false;
	auto* context = globals::d3d::context;
	const std::uint32_t width = pending.width, height = pending.height;
	{
		ID3D11Buffer* cbs[1] = { combineCB->CB() };
		ID3D11ShaderResourceView* srvs[2] = { backend.VisibilitySRV(), backend.DepthMaskSRV() };
		ID3D11UnorderedAccessView* uavs[4] = { sigmaViewZ.uav.get(), sigmaNormal.uav.get(), sigmaMotion.uav.get(), sigmaPenumbra.uav.get() };
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
		context->CSSetShader(prep, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrvs[2]{};
		ID3D11UnorderedAccessView* nullUavs[4]{};
		context->CSSetShaderResources(0, 2, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
	}

	D3D11_TEXTURE2D_DESC desc{};
	sigmaOut.texture->GetDesc(&desc);
	nrd::CommonSettings common{};
	const auto& fb = globals::game::frameBufferCached;
	const Matrix viewMat = fb.GetCameraView().Transpose();
	const Matrix projMat = fb.GetCameraProj().Transpose();
	const float3 cameraWorldPos = float3(fb.GetCameraPosAdjust());
	const DirectX::XMMATRIX translationMat = DirectX::XMMatrixTranslation(-cameraWorldPos.x, -cameraWorldPos.y, -cameraWorldPos.z);
	const Matrix worldToView = DirectX::XMMatrixMultiply(translationMat, viewMat);
	std::memcpy(common.viewToClipMatrix, &projMat, sizeof(float) * 16);
	std::memcpy(common.worldToViewMatrix, &worldToView, sizeof(float) * 16);
	const bool restart = !sigmaHistoryValid;
	std::memcpy(common.viewToClipMatrixPrev, restart ? static_cast<const void*>(&projMat) : sigmaPrevProj, sizeof(float) * 16);
	std::memcpy(common.worldToViewMatrixPrev, restart ? static_cast<const void*>(&worldToView) : sigmaPrevWorldToView, sizeof(float) * 16);
	const auto jitter = globals::features::upscaling.jitter;
	common.cameraJitter[0] = jitter.x;
	common.cameraJitter[1] = jitter.y;
	common.cameraJitterPrev[0] = restart ? jitter.x : sigmaPrevJitter[0];
	common.cameraJitterPrev[1] = restart ? jitter.y : sigmaPrevJitter[1];
	common.motionVectorScale[0] = 1.0f;
	common.motionVectorScale[1] = 1.0f;
	common.motionVectorScale[2] = 0.0f;
	common.isMotionVectorInWorldSpace = false;
	common.resourceSize[0] = common.resourceSizePrev[0] = static_cast<std::uint16_t>(desc.Width);
	common.resourceSize[1] = common.resourceSizePrev[1] = static_cast<std::uint16_t>(desc.Height);
	common.rectSize[0] = static_cast<std::uint16_t>(width);
	common.rectSize[1] = static_cast<std::uint16_t>(height);
	common.rectSizePrev[0] = restart ? common.rectSize[0] : sigmaPrevRect[0];
	common.rectSizePrev[1] = restart ? common.rectSize[1] : sigmaPrevRect[1];
	common.frameIndex = sigmaFrame++;
	common.denoisingRange = 1e6f;
	common.accumulationMode = restart ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
	sigma->SetCommonSettings(common);

	nrd::SigmaSettings sigmaSettings{};
	sigmaSettings.lightDirection[0] = a_toSun[0];
	sigmaSettings.lightDirection[1] = a_toSun[1];
	sigmaSettings.lightDirection[2] = a_toSun[2];
	sigmaSettings.planeDistanceSensitivity = 0.02f;
	sigmaSettings.maxStabilizedFrameNum = static_cast<std::uint32_t>(std::clamp(settings.SigmaStabilizedFrames, 0, 5));
	sigma->SetDenoiserSettings(&sigmaSettings);

	sigma->SetNamedSRV(nrd::ResourceType::IN_VIEWZ, sigmaViewZ.srv.get());
	sigma->SetNamedSRV(nrd::ResourceType::IN_NORMAL_ROUGHNESS, sigmaNormal.srv.get());
	sigma->SetNamedSRV(nrd::ResourceType::IN_MV, sigmaMotion.srv.get());
	sigma->SetNamedSRV(nrd::ResourceType::IN_PENUMBRA, sigmaPenumbra.srv.get());
	sigma->SetNamedSRV(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, sigmaOut.srv.get());  // its history
	sigma->SetNamedUAV(nrd::ResourceType::OUT_SHADOW_TRANSLUCENCY, sigmaOut.uav.get());
	const bool ok = sigma->Dispatch();
	ID3D11Buffer* cbs[1] = { combineCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);  // NRD bound its own
	std::memcpy(sigmaPrevProj, &projMat, sizeof(sigmaPrevProj));
	std::memcpy(sigmaPrevWorldToView, &worldToView, sizeof(sigmaPrevWorldToView));
	sigmaPrevJitter[0] = jitter.x;
	sigmaPrevJitter[1] = jitter.y;
	sigmaPrevRect[0] = common.rectSize[0];
	sigmaPrevRect[1] = common.rectSize[1];
	sigmaHistoryValid = ok;
	return ok;
}

namespace
{
	/// (46q) Size Buffers to Render Resolution: the work textures (MaskTemp, DebugColor, the shared DepthMask / Visibility, the
	/// denoisers sized from MaskTemp) at the upscaler's render size instead of the shadow mask's. Every pass is pixel-indexed over
	/// min(render size, this size); the combine copies that box into the full-size mask.
	void ShrinkToRenderAlloc(D3D11_TEXTURE2D_DESC& a_desc)
	{
		const auto [w, h] = globals::features::upscaling.GetRenderAllocSize(false);
		a_desc.Width = std::min<UINT>(a_desc.Width, w);
		a_desc.Height = std::min<UINT>(a_desc.Height, h);
	}
}

bool RayTracing::EnsureD3D11Resources(const D3D11_TEXTURE2D_DESC& a_maskDesc)
{
	if (maskTemp && a_maskDesc.Width == maskDesc.Width && a_maskDesc.Height == maskDesc.Height && a_maskDesc.Format == maskDesc.Format) {
		EnsureDebugColor();  // (46q)
		return true;
	}
	ReleaseD3D11Resources();

	DXGI_FORMAT uavFormat = a_maskDesc.Format;
	if (uavFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS)
		uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (uavFormat != DXGI_FORMAT_R8G8B8A8_UNORM && uavFormat != DXGI_FORMAT_B8G8R8A8_UNORM && uavFormat != DXGI_FORMAT_R16G16B16A16_FLOAT &&
		uavFormat != DXGI_FORMAT_R10G10B10A2_UNORM) {
		Disable(Status::Unavailable, std::format("unexpected shadow mask format {}", static_cast<int>(a_maskDesc.Format)));
		return false;
	}

	auto* device = globals::d3d::device;
	D3D11_TEXTURE2D_DESC desc = a_maskDesc;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.SampleDesc = { 1, 0 };
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	desc.CPUAccessFlags = 0;
	desc.MiscFlags = 0;
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
	uavDesc.Format = uavFormat;
	uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
	if (FAILED(device->CreateTexture2D(&desc, nullptr, maskTemp.put())) ||
		FAILED(device->CreateUnorderedAccessView(maskTemp.get(), &uavDesc, maskTempUAV.put()))) {
		Disable(Status::Unavailable, "could not create the shadow mask work texture");
		return false;
	}
	Util::SetResourceName(maskTemp.get(), "RayTracing MaskTemp");

	maskDesc = a_maskDesc;
	d3d11Bytes = static_cast<std::uint64_t>(desc.Width) * desc.Height * 4;
	// (46q) The debug texture: only while a debug view is on (Size Buffers to Render Resolution; off: always, as 46p).
	if (!EnsureDebugColor() && !globals::features::upscaling.settings.renderResBuffers) {
		Disable(Status::Unavailable, "could not create the debug texture");
		return false;
	}
	return true;
}

bool RayTracing::EnsureDebugColor()
{
	const bool wanted = !globals::features::upscaling.settings.renderResBuffers || debugView != DebugView::Off || aoDebugView != AODebugView::Off;
	if (!wanted) {
		if (debugColor) {
			d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(maskDesc.Width) * maskDesc.Height * 4);
			debugColor = nullptr;
			debugColorUAV = nullptr;
			debugColorSRV = nullptr;
		}
		return true;
	}
	if (debugColor || !maskTemp)
		return debugColor != nullptr;
	D3D11_TEXTURE2D_DESC debugDesc{};
	maskTemp->GetDesc(&debugDesc);
	debugDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	debugDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	// Into locals first: a failure leaves no half-made texture behind (the debug views then draw nothing).
	winrt::com_ptr<ID3D11Texture2D> texture;
	winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	winrt::com_ptr<ID3D11ShaderResourceView> srv;
	auto* device = globals::d3d::device;
	if (!device || FAILED(device->CreateTexture2D(&debugDesc, nullptr, texture.put())) || FAILED(device->CreateUnorderedAccessView(texture.get(), nullptr, uav.put())) ||
		FAILED(device->CreateShaderResourceView(texture.get(), nullptr, srv.put()))) {
		logger::warn("[RayTracing] Could not create the debug texture; debug views draw nothing");
		return false;
	}
	Util::SetResourceName(texture.get(), "RayTracing DebugColor");
	debugColor = texture;
	debugColorUAV = uav;
	debugColorSRV = srv;
	d3d11Bytes += static_cast<std::uint64_t>(debugDesc.Width) * debugDesc.Height * 4;
	return true;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void RayTracing::LatchFrame()
{
	// Decided before the engine draws the sun cascades. The covered casters leave the cascades
	// only while the trace ran last frame (so it is running), and never while the disagreement view
	// compares against a complete shadow map.
	// (45e) Also only when this frame's trace is certain to run: a D3D12 command context is free
	// now (it stays free until the trace is recorded), the scene has built cells, and the sky /
	// exterior conditions RunFrame checks hold. Otherwise the shadow map keeps every caster and a
	// frame that cannot trace falls back to it cleanly.
	// (46b) Until this frame's trace: no point-light result for the pixel shaders (passes before the prepass, e.g.
	// reflections, see last frame's Light Limit Fix tags; with t40 / t41 null they take the old path).
	UnbindPointLights();
	// (46c) Stop Engine Shadow Maps (interiors): the spot / omni shadow lights RT handled last frame are not drawn
	// into shadow maps this frame -- only when this frame's interior trace is as certain as the sun's is below (a
	// free D3D12 context, the interior captured, last frame traced indoors). Their mask channels are written by RT
	// after the trace (or with the fallback, should it not run after all).
	skippedMaps.clear();
	skippedLights.clear();
	skipLatched = settings.Enabled && PointLightsOn() && InteriorsOn() && settings.PointLightsNative &&
	              settings.PointLightsSkipShadowMaps && hooksInstalled && status == Status::Active && phase == Phase::Play && !loadingOpen.load() &&
	              interiorTracedLastFrame && Util::IsInterior() && backend.HasFreeContext() && scene.HasCoverage() && !skipCandidates.empty();
	auto* sky = globals::game::sky;
	// (45k) Not during a loading screen or the warm-up after it (nor while the RT result still fades in).
	const bool active = settings.Enabled && status == Status::Active && phase == Phase::Play && fade >= 1.0f &&
	                    debugView != DebugView::Disagreement && !Util::IsInterior() && sky && sky->mode.get() == RE::Sky::Mode::kFull &&
	                    backend.HasFreeContext() && scene.HasCoverage();
	// (45f) Watch for big casters the scene lacks while Ray Tracing runs (also when nothing is left out).
	const bool watch = settings.Enabled && status == Status::Active;
	RayTracingCasterFilter::Latch(active, filterRect, watch);
	filterLatched = RayTracingCasterFilter::IsLatched();
	RunEarlyPrep();
}

void RayTracing::RunEarlyPrep()
{
	// (45n) Early Prep: this frame's characters (bones, captures) and the D3D12 builds / skinning go out now,
	// before the game draws its shadow maps, instead of after the depth prepass. Same frame, same data (the
	// game's update is finished; rendering does not move bones), so nothing becomes stale; only the TLAS and
	// the trace are left for Prepass. Nothing waits. Whenever a condition fails (loading screens, the warm-up after
	// them, no exterior sky) Prepass does all of it later in the frame (the fallback).
	// (46p) Per-frame backend switches: Parallel BLAS Builds, Keep Our GPU Memory Resident (Upscaling > Save GPU Memory).
	if (backend.IsReady()) {
		backend.SetParallelBuilds(settings.ParallelBlasBuilds);
		backend.SetSubmitWorker(settings.SubmitWorker);  // (46q)
		backend.SetBuildBudget(settings.BuildGpuBudget, settings.BuildGpuBudgetMs, static_cast<std::uint32_t>(settings.GroupBuildsPerFrame), settings.GroupGraceMs);
		backend.SetResidencyHigh(globals::features::upscaling.settings.keepGpuMemoryResident);
	}
	earlyThisFrame = false;
	charactersUpdatedEarly = false;
	earlyCpuMs = 0.0f;
	earlyCharactersMs = 0.0f;
	if (backend.IsReady() && beginFrameIndex != globals::state->frameCount) {
		beginFrameIndex = globals::state->frameCount;
		backend.BeginFrame();  // (46g) per-frame pool / Early Prep counters
	}
	if (backend.IsReady() && backend.HasPrepared())
		backend.AbandonPrepared();  // a frame that never reached its Prepass
	auto* sky = globals::game::sky;
	if (!settings.Enabled || status != Status::Active || phase != Phase::Play || loadingOpen.load() ||
		!backend.IsReady() || Util::IsInterior() || !sky || sky->mode.get() != RE::Sky::Mode::kFull || !backend.HasFreeContext())
		return;
	const std::int64_t start = Qpc();
	try {
		skinDraws.clear();
		skinBones.clear();
		if (settings.Characters && backend.SkinAvailable()) {
			float world[4];
			if (scene.GetLoadedRect(world, false))
				characters.Update(backend, world, blackScreen ? 6.0 : 1.0, blackScreen ? 32u : 6u);
			characters.CollectDraws(skinDraws, skinBones);
			charactersUpdatedEarly = true;
		}
		earlyCharactersMs = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
		RayTracingRT::FrameInput input;
		input.buildTriangleBudget = kBuildBudget;
		input.skinDraws = &skinDraws;
		input.skinBones = &skinBones;
		// (46g) Leftovers in the black screen get the larger budgets (nothing visible yet).
		input.buildCpuMs = blackScreen ? kBlackBuildCpuMs : 0.0f;
		input.skinUploadCpuMs = blackScreen ? kBlackSkinUploadCpuMs : 0.0f;
		earlyThisFrame = backend.PrepareFrame(input);
		if (!earlyThisFrame && backend.HasFailed())
			Disable(Status::Failed, backend.LastError());
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	earlyCpuMs = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
}

bool RayTracing::ComputeRects(float o_traceRect[4], float o_coreRect[4], double a_camX, double a_camY)
{
	float world[4];
	if (!scene.GetLoadedRect(world, true))
		return false;
	o_traceRect[0] = static_cast<float>(world[0] - a_camX);
	o_traceRect[1] = static_cast<float>(world[1] - a_camY);
	o_traceRect[2] = static_cast<float>(world[2] - a_camX);
	o_traceRect[3] = static_cast<float>(world[3] - a_camY);
	// Casters fully inside this world rect leave the cascades (CasterFilter): it stops short of the
	// hand-off band, so the band, where the shadow map still counts, keeps their shadow-map shadow.
	// (45e) Without the retiring cells: the next frame's trace rect still holds every cell that is
	// captured now (cells leave only through "retiring", which is traced for one more frame), so
	// the cascades never leave out a caster the next trace would fade out in its band.
	float core[4];
	if (!scene.GetLoadedRect(core, false)) {
		o_coreRect[0] = o_coreRect[1] = FLT_MAX;
		o_coreRect[2] = o_coreRect[3] = -FLT_MAX;
		return true;
	}
	const float inset = kHandOffBand + 256.0f;
	o_coreRect[0] = core[0] + inset;
	o_coreRect[1] = core[1] + inset;
	o_coreRect[2] = core[2] - inset;
	o_coreRect[3] = core[3] - inset;
	return true;
}

void RayTracing::Prepass()
{
	activeThisFrame = false;
	combinedThisFrame = false;
	pending.active = false;
	frameStart = Qpc();
	// (46n) A step of the cell-event preparation faulted behind the loading screen: off here, on the world's frame.
	if (!cellEventFault.empty() && status != Status::Failed && status != Status::Unavailable)
		Disable(Status::Failed, std::exchange(cellEventFault, {}));
	// (46b) Point lights: nothing published until this frame's trace is submitted; the history only follows
	// a frame that was denoised.
	pointSelector.Publish(false);
	pointThisFrame = false;
	pointK = 0;
	pointCpuMs = 0.0f;
	pointHistoryValid = pointHistoryValid && pointDenoised;
	pointDenoised = false;
	UnbindPointLights();
	// (46c) This frame's shadow maps are drawn: the latch ends here (skippedMaps stays for this frame's mask write).
	skipLatched = false;
	interiorThisFrame = false;
	interiorTracedLastFrame = false;  // set again by this frame's interior trace (FinishFrame)
	skipCandidates.clear();           // likewise (the lights it handled: next frame's shadow maps may skip them)
	skipEntries.clear();              // (46e) their cones (copied into skippedMaps by SkipShadowMap already)
	pointMaskDone = false;
	maskChannelsWritten = 0;
	skippedLastFrame = static_cast<std::uint32_t>(skippedMaps.size());
	// (46f) RTAO: nothing for the AO consumers until this frame's trace carries it.
	aoThisFrame = false;
	aoInteriorThisFrame = false;  // (46h)
	aoModeThisFrame = -1;
	aoComposedLastFrame = aoComposited;  // the F7 row is written before this frame's AO pass (ReportFrame)
	aoComposited = false;
	aoDebugDrawn = false;
	aoCpuMs = 0.0f;
	// (45n) Whatever path this Prepass takes: an Early Prep context that the trace did not use is released
	// (D3D12 signal only), and the frame-start flags end with this frame.
	struct EarlyPrepEnd
	{
		RayTracing* rt;
		~EarlyPrepEnd()
		{
			if (rt->backend.IsReady() && rt->backend.HasPrepared())
				rt->backend.AbandonPrepared();
			rt->earlyThisFrame = false;
			rt->charactersUpdatedEarly = false;
		}
	} earlyPrepEnd{ this };
	// (46c) A light whose shadow map was left out this frame always gets its mask channel written: by this frame's
	// trace, or (no trace after all: error, switch-off) with the unshadowed fallback -- never the stale shadow map.
	struct SkippedMapsEnd
	{
		RayTracing* rt;
		~SkippedMapsEnd()
		{
			if (!rt->pending.active && !rt->skippedMaps.empty() && !rt->pointMaskDone)
				rt->WritePointMask(false);
		}
	} skippedMapsEnd{ this };

	// (46k) The world is rendered this frame (the loading pump stays idle), on this thread.
	lastPrepassFrame = globals::state->frameCount;
	pumpThisFrame = false;
	if (const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId()); thread != renderThreadId.load()) {
		if (renderThreadId.load() != 0)
			logger::info("[RayTracing] The world is rendered on thread {} (was {})", thread, renderThreadId.load());
		renderThreadId.store(thread);
	}

	if (!settings.Enabled) {
		if (sssClaimed)
			globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
		sssClaimed = false;
		SwitchedOffFrame();
		return;
	}
	if (status == Status::Unavailable || status == Status::Failed) {
		scene.Housekeeping();
		if (sssClaimed)
			globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
		sssClaimed = false;
		return;
	}

	if (backend.IsReady() && beginFrameIndex != globals::state->frameCount) {
		beginFrameIndex = globals::state->frameCount;  // (46g) no Early Prep call this frame (loading screens)
		backend.BeginFrame();
	}
	cpuLast = {};
	UpdatePhase();
	DrainTransitionEvents();  // (46k)
	// (46g) Pools grow on the render thread only while nothing is visible.
	if (backend.IsReady())
		backend.SetTransition(phase == Phase::Loading || phase == Phase::Black || blackScreen);
	scene.SetInteriorEnabled(InteriorWanted());  // (46c) also behind loading screens: an interior is captured there
	PrepassFrame();
}

void RayTracing::SwitchedOffFrame()
{
	{
		phase = Phase::Play;
		fade = 1.0f;
		// (46g) No transition hold while off (the scene is released anyway).
		handedOff = true;
		inTransition = false;
		lateHold = false;
		blackScreen = false;
		scene.SetHold(false);
		scene.SetTransition(false);
		if (backend.IsReady()) {
			backend.WaitForTrace();  // (45l) a deferred wait left over (GPU-side only)
			backend.WaitForAO();     // (46f) likewise the second fence
		}
		if (status != Status::Off) {
			// Switched off: the scene goes (BLASes retire on the GPU's schedule); the device stays.
			// (46b) The point lights' textures too. (46f) And RTAO's.
			pointSelector.Clear();
			ReleasePointDenoiser();
			ReleaseAOFilter();
			if (backend.IsReady()) {
				backend.ReleasePointResources();
				backend.ReleaseAOResources();
			}
			RayTracingCasterFilter::Clear();
			characters.ReleaseAll(backend);  // (45i)
			if (backend.IsReady()) {
				scene.ReleaseAll(backend);
				backend.OnSwitchedOff();  // (46i) clean pools for the next switch-on (see GpuArena::lastAskQpc)
			}
			// (46p) Save GPU Memory: the shadow-mask work textures, the denoisers, the shared textures, scratch and the TLAS go
			// too (made again on the next switch-on; D3D12 ones behind the fence).
			if (globals::features::upscaling.settings.saveVramFreeRtWhenOff) {
				ReleaseD3D11Resources();
				if (backend.IsReady())
					backend.ReleaseIdleResources();
			}
			// (46i) Switching on again starts like a fresh entry: the interior's entry time, the beam guards of
			// this place, the stuck-state diagnostic.
			interiorCell = nullptr;
			interiorReadyPending = false;
			interiorSceneLogPending = false;
			beamGuards.clear();
			excludedBeams.clear();
			waitingSinceQpc = 0;
			waitingLogged = false;
			status = Status::Off;
			statusReason.clear();
			lastTlasLog = 0;
			logger::info("[RayTracing] Switched off");
		}
		scene.Housekeeping();  // (45j) engine objects of the released scene, a bounded number per frame
		// Switching back on is a fresh attempt (also after a failure), never a silent "Starting".
		backendTried = false;
		if (backend.IsReady()) {
			backend.Tick();
			if (backend.AlphaIdle())
				backend.ReleaseAlphaResources();  // (45h) once the GPU is done with the released scene
			if (backend.SkinIdle())
				backend.ReleaseSkinResources();  // (45i)
		}
		traceSample = {};
		activeNoBlasSince = 0;
		activeNoBlasLogged = false;
		// (46k) Queued fully-loaded cells are of no use while off.
		std::scoped_lock lock(eventsMutex);
		cellEvents.clear();
	}
}

void RayTracing::PrepassFrame()
{
	try {
		if ((phase == Phase::Loading || phase == Phase::Black || (phase == Phase::WarmUp && !handedOff)) && backend.IsReady()) {
			// (45k) Behind the loading screen: capture and build with a large budget, trace nothing. (46g) Also the black
			// screen after it and the visible frames before the hand-off (TransitionFrame).
			TransitionFrame();
		} else {
			RunFrame();
		}
		// (45e) Captures that left the scene this frame go only now, after the trace was submitted.
		if (backend.IsReady()) {
			const std::int64_t endStart = Qpc();
			scene.EndFrame(backend);
			characters.EndFrame(backend);  // (45i)
			cpuLast.endFrame = static_cast<float>(static_cast<double>(Qpc() - endStart) * QpcMs());
		}
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	if (pending.active) {
		// (45l) The rest after every other feature's prepass (FinishPrepass).
		prepassCpuMs = static_cast<float>(static_cast<double>(Qpc() - frameStart) * QpcMs());
		return;
	}
	// (45k) Screen Space Shadows claimed but not drawn by the trace (nothing traced this frame): the
	// normal ray march, without the caster mask.
	if (sssClaimed)
		globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
	sssClaimed = false;
	lastCpuMs = static_cast<float>(static_cast<double>(Qpc() - frameStart) * QpcMs());
	ReportFrame();
}

void RayTracing::Reset()
{
	// (46k) Present hook, once per presented frame, before the real Present (State::Reset; frameCount is still this frame's).
	RegisterEventSinks();
	MaybeLogTransitionEvents();
	{
		std::int64_t openQpc = 0;
		{
			std::scoped_lock lock(eventsMutex);
			openQpc = events.loadingOpen;
		}
		if (openQpc != pumpOpenSeen) {
			// A new loading screen: the pump's counters are this transition's.
			pumpOpenSeen = openQpc;
			pumpFrames = pumpForeignFrames = pumpMidTeleport = 0;
			pumpCpuTotalMs = 0.0;
		}
	}
	if (lastPrepassFrame == globals::state->frameCount || !loadingOpen.load())
		return;  // the world was rendered this frame (Prepass did its work), or no loading screen is up
	// A presented frame without a world render behind a loading screen. Scene-graph access only on the thread that renders
	// the world (the main thread in SE), never in the middle of the teleport.
	const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
	if (renderThreadId.load() == 0 || thread != renderThreadId.load()) {
		++pumpForeignFrames;
		if (!pumpForeignLogged) {
			pumpForeignLogged = true;
			logger::warn("[RayTracing] The loading screen is presented on thread {}, the world on thread {}: the loading pump does not run there", thread,
				renderThreadId.load());
		}
		return;
	}
	if (!settings.Enabled) {
		if (status != Status::Off && settings.LoadingPump)
			SwitchedOffFrame();  // (46k) switched off behind the loading screen: the same clean reset as on a world frame
		return;
	}
	if (!settings.LoadingPump || status == Status::Unavailable || status == Status::Failed)
		return;
	{
		std::scoped_lock lock(eventsMutex);
		if (events.teleportPre != 0 && events.teleportPre > events.teleportFinish) {
			++pumpMidTeleport;
			return;
		}
	}
	LoadingPump();
}

void RayTracing::LoadingPump()
{
	// (46k) Behind the loading screen: the destination's cells (as they finish loading), their BLASes, its characters and the
	// pools / scratch / TLAS, within LoadingPumpMs of CPU per frame. D3D12 only: no D3D11 pass, nothing traced, nothing waits.
	const std::int64_t start = Qpc();
	activeThisFrame = false;
	combinedThisFrame = false;
	pending.active = false;
	// (46p) Per-frame backend switches: Parallel BLAS Builds, Keep Our GPU Memory Resident (Upscaling > Save GPU Memory).
	if (backend.IsReady()) {
		backend.SetParallelBuilds(settings.ParallelBlasBuilds);
		backend.SetSubmitWorker(settings.SubmitWorker);  // (46q)
		backend.SetBuildBudget(settings.BuildGpuBudget, settings.BuildGpuBudgetMs, static_cast<std::uint32_t>(settings.GroupBuildsPerFrame), settings.GroupGraceMs);
		backend.SetResidencyHigh(globals::features::upscaling.settings.keepGpuMemoryResident);
	}
	earlyThisFrame = false;
	charactersUpdatedEarly = false;
	earlyCpuMs = 0.0f;
	earlyCharactersMs = 0.0f;
	if (!EnsureBackend())
		return;
	if (status != Status::Waiting) {
		status = Status::Waiting;
		statusReason = "preparing behind the loading screen";
	}
	if (beginFrameIndex != globals::state->frameCount) {
		beginFrameIndex = globals::state->frameCount;
		backend.BeginFrame();
	}
	if (backend.HasPrepared())
		backend.AbandonPrepared();
	cpuLast = {};
	try {
		// The per-kind choices first (as RunFrame): a scene captured here with another configuration would be captured again.
		UpdateAlphaCasters();
		UpdateCharacters();
		UpdatePointLights();
		UpdateAO();
		UpdatePhase();
		DrainTransitionEvents();
		backend.SetTransition(true);
		scene.SetInteriorEnabled(InteriorWanted());
		TransitionFrame();
		const std::int64_t endStart = Qpc();
		scene.EndFrame(backend);
		characters.EndFrame(backend);
		cpuLast.endFrame = static_cast<float>(static_cast<double>(Qpc() - endStart) * QpcMs());
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	pumpThisFrame = true;
	pumpCpuMs = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
	++pumpFrames;
	pumpCpuTotalMs += pumpCpuMs;
	lastCpuMs = pumpCpuMs;
	ReportFrame();
}

void RayTracing::FinishPrepass()
{
	if (!pending.active)
		return;
	const std::int64_t start = Qpc();
	try {
		FinishFrame();
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	if (!skippedMaps.empty() && !pointMaskDone)
		WritePointMask(false);  // (46c) see Prepass
	if (sssClaimed)
		globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
	sssClaimed = false;
	// The CPU of this frame: Prepass plus this second half (the other features' prepasses in between are theirs).
	lastCpuMs = prepassCpuMs + static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
	ReportFrame();
}

void RayTracing::ReportFrame()
{
	const auto& stats = backend.GetStats();
	if (status != loggedStatus) {
		static constexpr const char* kNames[] = { "off", "unavailable", "failed", "paused", "loading the scene", "active" };
		const auto& sc = scene.GetStats();
		logger::info("[RayTracing] {}{}{} (cells {}, {:.2f} M triangles, {} BLAS, {:.0f} MB)", kNames[static_cast<int>(status)],
			statusReason.empty() ? "" : ": ", statusReason, sc.cellsComplete, static_cast<double>(sc.triangles) / 1e6, stats.blasCount,
			static_cast<double>(stats.blasBytes + stats.scratchBytes + stats.tlasBytes) / (1024.0 * 1024.0));
		loggedStatus = status;
	}
	LogStuckWaiting();  // (46i)
	{
		// (46k) "Active" while not a single BLAS exists (46h arrived in Solitude that way: cells without geometry counted as
		// covered while the pools never built the rest): one warning with what the scene and the pools hold.
		const auto& sc = scene.GetStats();
		if (settings.Enabled && status == Status::Active && stats.blasCount == 0 && sc.triangles > 0) {
			const std::int64_t now = Qpc();
			if (activeNoBlasSince == 0)
				activeNoBlasSince = now;
			if (!activeNoBlasLogged && static_cast<double>(now - activeNoBlasSince) * QpcMs() > 2000.0) {
				activeNoBlasLogged = true;
				logger::warn("[RayTracing] Active for 2 s without a single BLAS: cells {} tracked, {} complete, {} gathering, {:.2f} M triangles captured; {} BLAS "
							 "queued, {} built so far; hold {}, handed off {}, phase {}; pools: {}",
					sc.cellsTracked, sc.cellsComplete, sc.cellsGathering, static_cast<double>(sc.triangles) / 1e6, backend.QueuedBuilds(), stats.buildsTotal,
					scene.Held(), handedOff, static_cast<int>(phase), backend.IsReady() ? backend.PoolSummary() : std::string("-"));
			}
		} else {
			activeNoBlasSince = 0;
			activeNoBlasLogged = false;
		}
	}
	lastCpuMs += earlyCpuMs;  // (45n) the frame-start part (Early Prep) belongs to this frame too
	TransitionLogFrame(lastCpuMs);  // (46g)
	traceSample.cpuMs = lastCpuMs;
	traceSample.gpuMs = activeThisFrame ? stats.lastGpuMs : -1.0f;
	traceSample.builds = activeThisFrame ? stats.buildsLastFrame : 0;
	traceSample.instances = activeThisFrame ? stats.instancesLastFrame : 0;
	Util::FrameTrace::OnRayTracing(traceSample.gpuMs, traceSample.cpuMs, traceSample.builds);
	PublishCpuSplit();
}

void RayTracing::LogStuckWaiting()
{
	// (46i) On, playing (no loading screen involved), and still nothing traced after 10 s: one line that says what
	// the scene waits for (cells, the build queue, the GPU pools, the hand-off). Logged once per wait.
	if (!settings.Enabled || status != Status::Waiting || inTransition || phase != Phase::Play) {
		waitingSinceQpc = 0;
		waitingLogged = false;
		return;
	}
	const std::int64_t now = Qpc();
	if (waitingSinceQpc == 0) {
		waitingSinceQpc = now;
		return;
	}
	const double waitedMs = static_cast<double>(now - waitingSinceQpc) * QpcMs();
	if (waitingLogged || waitedMs < 10000.0)
		return;
	waitingLogged = true;
	const auto& sc = scene.GetStats();
	const auto& st = backend.GetStats();
	logger::warn("[RayTracing] Still loading the scene after {:.0f} s ({}): cells {} tracked, {} gathering, {} complete, {} shapes; {} BLAS built, {} queued, "
				 "{} character shapes waiting; hold {}, handed off {}, interior {}; pools: {}",
		waitedMs / 1000.0, statusReason, sc.cellsTracked, sc.cellsGathering, sc.cellsComplete, sc.shapes, st.blasCount, backend.QueuedBuilds(),
		backend.PendingSkinUploads(), scene.Held(), handedOff, Util::IsInterior(), backend.IsReady() ? backend.PoolSummary() : std::string("-"));
}

void RayTracing::PublishCpuSplit()
{
	// (45j) Where this frame's CPU went: the page (smoothed / worst) and the F7 trace (every frame).
	const auto& times = scene.GetFrameTimes();
	const auto& stats = backend.GetStats();
	cpuLast.total = lastCpuMs;
	cpuLast.scene = times.update;
	cpuLast.instances = times.instances;
	cpuLast.missed = times.missed;
	auto blend = [](float& a_smoothed, float& a_worst, float a_value) {
		a_smoothed = a_smoothed * 0.95f + a_value * 0.05f;
		a_worst = std::max(a_worst, a_value);
	};
	blend(cpuSmoothed.total, cpuWorst.total, cpuLast.total);
	blend(cpuSmoothed.scene, cpuWorst.scene, cpuLast.scene);
	blend(cpuSmoothed.characters, cpuWorst.characters, cpuLast.characters);
	blend(cpuSmoothed.instances, cpuWorst.instances, cpuLast.instances);
	blend(cpuSmoothed.missed, cpuWorst.missed, cpuLast.missed);
	blend(cpuSmoothed.execute, cpuWorst.execute, cpuLast.execute);
	blend(cpuSmoothed.d3d11, cpuWorst.d3d11, cpuLast.d3d11);
	blend(cpuSmoothed.endFrame, cpuWorst.endFrame, cpuLast.endFrame);

	Util::FrameTrace::RayTracingDetail detail;
	if (settings.Enabled && status != Status::Unavailable && status != Status::Failed) {
		detail.scene = times.update;
		detail.gridDiff = times.gridDiff;
		detail.gather = times.gather;
		detail.registration = times.registration;
		detail.objects = times.objects;
		detail.objectCapture = times.objectCapture;
		detail.instances = times.instances;
		detail.missed = times.missed;
		detail.characters = cpuLast.characters;
		detail.execute = cpuLast.execute;
		detail.executeBuild = stats.cpuBuildMs;
		detail.executeSkin = stats.cpuSkinMs;
		detail.d3d11 = cpuLast.d3d11;
		detail.endFrame = cpuLast.endFrame;
		detail.release = times.release;
		detail.recaptures = times.recaptures;
		detail.objectsCaptured = times.objectsCaptured;
		detail.missedJudged = times.missedJudged;
		detail.releasePending = times.releasePending;
		detail.tlasInstances = activeThisFrame ? stats.instancesLastFrame : 0;
		detail.cellsGathering = scene.GetStats().cellsGathering;
		// (45k)
		if (activeThisFrame) {
			const auto& x = stats.split;
			detail.xCollect = x.collect;
			detail.xSignal11 = x.signal11;
			detail.xFlush11 = x.flush11;
			detail.xWait12 = x.wait12;
			detail.xReset = x.reset;
			detail.xTlas = x.tlas;
			detail.xAlpha = x.alpha;
			detail.xRecord = x.record;
			detail.xExecute = x.execute;
			detail.xSignal12 = x.signal12;
			detail.xWait11 = x.wait11;
		}
		detail.denoiser = settings.Denoiser;
		detail.phase = static_cast<int>(phase);
		// (45l) Per pass.
		auto* timers = Util::GpuPassTimers::GetSingleton();
		// (46d) A pass that did not run this frame reports 0, not the last sample it left (stale for as long as it is off).
		const bool combined = activeThisFrame && combinedThisFrame;
		auto passMs = [&](bool a_ran, Util::GpuBucket a_bucket) { return a_ran ? timers->LastSampleMs(a_bucket) : 0.0f; };
		detail.tileGpuMs = passMs(combined && pending.spatial, Util::GpuBucket::RayTracingDenoise);
		detail.filterGpuMs = passMs(combined && pending.spatial, Util::GpuBucket::RayTracingDenoiseFilter);
		detail.sigmaGpuMs = passMs(combined && pending.sigma, Util::GpuBucket::RayTracingSigma);
		detail.combineGpuMs = passMs(combined && !pending.interior, Util::GpuBucket::RayTracingCombine);
		detail.syncGpuMs = passMs(combined && pending.traceSync, Util::GpuBucket::RayTracingSync);  // (46h) RTAO-only interior frames: no rt_sync
		float denoise = -1.0f;
		for (const float part : { detail.tileGpuMs, detail.filterGpuMs, detail.sigmaGpuMs })
			if (part >= 0.0f)
				denoise = std::max(denoise, 0.0f) + part;
		detail.denoiseGpuMs = denoise;
		if (activeThisFrame) {
			detail.prepGpuMs = stats.lastPrepGpuMs;
			detail.prepWork = stats.lastPrepWork;  // (46p) the same submission's work
		if (detail.prepWork.valid && backend.SubmissionsTotal() >= detail.prepWork.submission)
			detail.prepWork.sampleAge = static_cast<std::uint32_t>(backend.SubmissionsTotal() - detail.prepWork.submission);
			detail.traceGpuMs = stats.lastTraceGpuMs;
		}
		detail.quality = settings.QualityLevel;
		detail.denoiserEffective = static_cast<int>(lastEffectiveDenoiser);
		detail.skinDraws = backend.GetSkinStats().drawsLastFrame;
		detail.skinReused = backend.GetSkinStats().reusedLastFrame;
		// (45n)
		detail.early = activeThisFrame && stats.split.early ? 1 : 0;
		detail.earlyMs = earlyCpuMs;
		detail.retireMs = stats.retireLastMs;
		detail.retiredCount = stats.retiredLastCollect;
		detail.workerReleaseMs = backend.LastWorkerReleaseMs();
		detail.queueGapGpuMs = activeThisFrame ? stats.lastQueueGapMs : -1.0f;
		detail.stillReused = characters.StillReusedLastFrame();
		// (46b) Point-light shadows.
		const auto& ps = pointSelector.GetStats();
		detail.plK = pointThisFrame ? static_cast<int>(pointK) : 0;
		detail.plSources = pointThisFrame ? ps.sources : 0;
		detail.plCandidates = ps.candidates;
		detail.plGpuMs = pointThisFrame ? stats.lastPointGpuMs : 0.0f;
		detail.plDenoiseGpuMs = passMs(pointThisFrame && pointDenoised, Util::GpuBucket::RayTracingPointDenoise);
		detail.plRays = pointThisFrame ? stats.pointRaysPerFrame : 0u;
		detail.plCpuMs = PointLightsOn() ? pointCpuMs : -1.0f;
		// (46c)
		// (46h) 2 = indoors traced: point lights and / or RTAO.
		detail.rtInterior = interiorThisFrame ? ((pointThisFrame || (aoThisFrame && aoInteriorThisFrame)) ? 2 : 1) : 0;
		detail.plBeams = pointThisFrame ? ps.beams : 0;
		detail.plCarried = pointThisFrame ? ps.carried : 0;  // (46e)
		detail.smSkipped = static_cast<std::uint32_t>(skippedMaps.size());
		detail.maskChannels = maskChannelsWritten;
		detail.interiorShapes = scene.GetStats().interior ? scene.GetStats().interiorShapes : 0;
		// (46f) RTAO (-1 = not run this frame).
		const bool aoTraced = activeThisFrame && stats.aoTracedLastFrame && AOOn();
		detail.rtaoGpuMs = aoTraced ? stats.lastAOGpuMs : -1.0f;
		detail.rtaoFilterGpuMs = aoComposedLastFrame ? std::max(timers->LastSampleMs(Util::GpuBucket::RayTracingAO), 0.0f) : -1.0f;
		detail.rtaoSyncGpuMs = aoComposedLastFrame ? std::max(timers->LastSampleMs(Util::GpuBucket::RayTracingAOSync), 0.0f) : -1.0f;
		detail.rtaoRays = aoTraced ? stats.aoRaysPerFrame : 0;
		// (46h) Where this frame's RTAO was traced and its mode (-1 = not traced this frame).
		detail.rtaoIndoors = aoTraced && aoThisFrame ? (aoInteriorThisFrame ? 1 : 0) : -1;
		detail.rtaoMode = aoTraced && aoThisFrame ? aoModeThisFrame : -1;
		// (46g) Transitions, Early Prep split, pools.
		detail.black = blackScreen ? 1 : 0;
		detail.pump = pumpThisFrame ? 1 : 0;  // (46k)
		detail.pumpMs = pumpThisFrame ? pumpCpuMs : -1.0f;
		detail.handOff = handOffThisFrame ? 1 : 0;
		detail.sinceCloseMs = inTransition || fade < 1.0f ? static_cast<float>(static_cast<double>(Qpc() - warmUpStart) * QpcMs()) : -1.0f;
		if (scene.Held()) {
			const auto held = scene.HeldCounts(backend);
			detail.heldReady = static_cast<int>(held.shown);
			detail.heldPending = static_cast<int>(held.pending);
		}
		const auto& e = stats.prep;
		detail.eCollect = e.collect;
		detail.eTimestamps = e.timestamps;
		detail.eReset = e.reset;
		detail.eBuild = e.build;
		detail.eSkin = e.skin;
		detail.eClose = e.close;
		detail.eExecute = e.execute;
		if (activeThisFrame) {
			detail.xTimestamps = stats.split.timestamps;
			detail.xEnsure = stats.split.ensure;
			detail.xPublish = stats.split.publish;
			detail.xAo = stats.split.ao;
		}
		detail.poolMs = stats.pools.pumpMsLastFrame;
		detail.poolSyncBlocks = stats.pools.syncBlocksLastFrame;
		detail.poolMb = static_cast<float>(static_cast<double>(stats.pools.reservedBytes) / (1024.0 * 1024.0));
		detail.poolAllocFails = stats.pools.allocFailsLastFrame;
		detail.poolWorkerMs = stats.pools.workerLastMs;
		detail.plSlotChanges = ps.slotChanges;
		detail.plMerged = ps.merged;
		detail.plRejRadius = ps.byReason[1];
		detail.plRejIntensity = ps.byReason[2];
		detail.plKept = ps.kept;
		detail.plChurn = pointThisFrame ? stats.pointChurnPerFrame : -1;
		detail.plBeamsExcluded = static_cast<std::uint32_t>(excludedBeams.size());
		// (46m) Characters step by step (this frame's Update / CollectDraws), merged BLASes, the split sync.
		if (activeThisFrame && settings.Characters && cpuLast.characters >= 0.0f) {
			const auto& ch = characters.GetStats();
			detail.chCandidates = ch.candidatesMs;
			detail.chReads = ch.readsMs;
			detail.chCapture = ch.captureMs;
			detail.chCommit = ch.commitMs;
			detail.chFilter = ch.filterMs;
			detail.chCollect = characters.LastCollectMs();
			detail.chBones = ch.bonesRead;
			detail.chRescans = ch.rescans;
			detail.chLanes = ch.lanes;
			detail.chActors = ch.actors;
			detail.chShapes = ch.shapes;
			const auto& sk = backend.GetSkinStats();
			detail.skinGroups = sk.groupsLastFrame;
			detail.skinGroupBuilds = sk.groupBuildsLastFrame;
			detail.skinGroupRefits = sk.groupRefitsLastFrame;
			detail.skinInstances = sk.instancesLastFrame;
			detail.skinDispatches = sk.dispatchesLastFrame;
		}
		if (combined && pending.traceSync) {
			const float sun = passMs(true, Util::GpuBucket::RayTracingSync);
			const float points = pending.splitSun ? passMs(true, Util::GpuBucket::RayTracingSyncPoint) : 0.0f;
			detail.syncSplit = pending.splitSun ? 1 : 0;
			detail.syncSunGpuMs = sun;
			detail.syncPlGpuMs = points;
			detail.syncGpuMs = std::max(sun, 0.0f) + std::max(points, 0.0f);  // rt_sync_gpu_ms: the whole wait, as before
		}
	}
	detail.sssGpuMs = Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::ScreenSpaceShadows);
	// (45k) A slow D3D12 record + submit names its slowest call in the log (at most every 2 s).
	if (activeThisFrame && cpuLast.execute > 4.0f) {
		static std::int64_t lastSpikeLog = 0;
		const std::int64_t now = Qpc();
		if (static_cast<double>(now - lastSpikeLog) * QpcMs() > 2000.0) {
			lastSpikeLog = now;
			const auto& x = stats.split;
			// (45n) "collect" = releasing retired D3D12 objects (x.retire of it) + the context choice (never waits).
			const std::pair<const char*, float> parts[] = { { "collect (retired objects + context choice)", x.collect }, { "D3D11 Signal", x.signal11 }, { "D3D11 Flush", x.flush11 },
				{ "D3D12 queue Wait", x.wait12 }, { "allocator/list Reset", x.reset }, { "BLAS creation", stats.cpuBuildMs }, { "characters", stats.cpuSkinMs },
				{ "TLAS instances", x.tlas }, { "alpha records", x.alpha }, { "recording", x.record }, { "ExecuteCommandLists", x.execute },
				{ "D3D12 Signal", x.signal12 }, { "D3D11 Wait", x.wait11 } };
			const auto* worst = &parts[0];
			for (const auto& part : parts)
				if (part.second > worst->second)
					worst = &part;
			logger::info("[RayTracing] D3D12 record + submit took {:.1f} ms; slowest: {} {:.1f} ms (Signal11 {:.2f}, Flush11 {:.2f}, Wait12 {:.2f}, Execute {:.2f}, "
						 "Signal12 {:.2f}, Wait11 {:.2f}, builds {:.2f}, characters {:.2f}; retired objects {} in {:.2f} ms, early prep {})",
				cpuLast.execute, worst->first, worst->second, x.signal11, x.flush11, x.wait12, x.execute, x.signal12, x.wait11, stats.cpuBuildMs, stats.cpuSkinMs,
				x.retiredCount, x.retire, x.early ? "yes" : "no");
		}
	}
	Util::FrameTrace::OnRayTracingDetail(detail);
	{
		// (46q) Submit on a Worker Thread / Budget Builds by GPU Time.
		Util::FrameTrace::Q46 q{};
		if (backend.IsReady()) {
			const auto sub = backend.TakeSubmitSample();
			q.subOn = backend.SubmitWorkerInEffect() ? 1 : 0;
			q.subBatches = sub.batches;
			q.subMaxBatchMs = sub.maxBatchMs;
			q.subMaxLagMs = sub.maxLagMs;
			q.subMaxCallMs = sub.maxCallMs;
			const auto& bud = backend.GetBudgetSample();
			q.budOn = settings.BuildGpuBudget ? 1 : 0;
			q.budMsPerMtri = bud.msPerMillionTris;
			q.budTris = bud.triBudget;
			q.budStaticQueued = bud.staticDeferred;
			q.budGroupsDeferred = bud.groupsDeferred;
			q.budGroupsKept = bud.groupsKept;
		}
		Util::FrameTrace::On46qRt(q);
	}
	handOffThisFrame = false;
}

void RayTracing::RunFrame()
{
	if (!EnsureBackend())
		return;
	// (45l) A deferred D3D11 wait that was never issued (a frame without FinishPrepass): before this
	// frame's depth copy overwrites what the last trace read. (46f) Likewise RTAO's (it reads the same depth copy).
	backend.WaitForTrace();
	backend.WaitForAO();
	UpdateAlphaCasters();  // (45h, 45j) per-kind choices: a change re-captures the scene
	UpdateCharacters();                                // (45i)
	UpdatePointLights();                               // (46b)
	UpdateAO();                                        // (46f)
	if (status == Status::Off || status == Status::Unavailable)
		status = Status::Waiting;

	auto* sky = globals::game::sky;
	// (46c) Indoors with the point lights on: the interior's own frame (no sun).
	scene.SetInteriorEnabled(InteriorWanted());
	if (Util::IsInterior() && InteriorWanted()) {
		try {
			RunInteriorFrame();
		} catch (const std::exception& e) {
			pending.active = false;
			SwitchOffInteriors(std::format("exception: {}", e.what()));
		}
		return;
	}
	if (Util::IsInterior() || !sky || sky->mode.get() != RE::Sky::Mode::kFull) {
		status = Status::Inactive;
		statusReason = Util::IsInterior() ? "interior" : "no sky";
		scene.Update(backend, kCaptureBudgetMs, globals::state->frameCount);  // drops exterior cells in interiors
		if (!characters.Empty())
			characters.ReleaseAll(backend);  // (45i) nothing is left out of the shadow maps here
		backend.Tick();
		return;
	}

	auto* renderer = globals::game::renderer;
	auto& mask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (!mask.texture || !mask.SRV || !depth.depthSRV) {
		status = Status::Inactive;
		statusReason = "shadow mask not available";
		return;
	}
	D3D11_TEXTURE2D_DESC desc{};
	mask.texture->GetDesc(&desc);
	ShrinkToRenderAlloc(desc);  // (46q) Size Buffers to Render Resolution
	if (!EnsureD3D11Resources(desc))
		return;
	std::string reason;
	if (!backend.EnsureTextures(desc.Width, desc.Height, reason)) {
		Disable(Status::Failed, reason);
		return;
	}

	// VRAM guard: past the budget no new cells are captured (they keep their shadow-map shadows).
	{
		const auto& stats = backend.GetStats();
		overVramBudget = stats.blasBytes + stats.scratchBytes > kAccelerationBudgetBytes;
	}
	scene.Update(backend, overVramBudget ? 0.0f : (blackScreen ? kBlackCaptureBudgetMs : kCaptureBudgetMs), globals::state->frameCount);

	const auto& fb = globals::game::frameBufferCached;
	const float4 camera = fb.GetCameraPosAdjust(0);
	const double camX = camera.x, camY = camera.y, camZ = camera.z;
	float traceRect[4], coreRect[4];
	if (!ComputeRects(traceRect, coreRect, camX, camY)) {
		status = Status::Waiting;
		statusReason = "waiting for loaded cells";
		backend.Tick();
		return;
	}
	std::copy_n(coreRect, 4, filterRect);
	// (45l) Quality tier (or Custom) and the denoiser in effect (DLSS RR whenever Upscaling's RR runs).
	const QualityParams quality = ResolveQuality();
	lastQuality = quality;
	const DenoiserMode denoiser = EffectiveDenoiser();
	lastEffectiveDenoiser = denoiser;
	// (46p) Save GPU Memory: the denoiser not in effect gives its textures back (made again on first use) -- only once the
	// effective one has held for 120 frames of play (RR can flip it on map / menu warm-up frames: no free / remake churn).
	if (denoiser == denoiserStableMode) {
		denoiserStableFrames = std::min(denoiserStableFrames + 1, 1000000);
	} else {
		denoiserStableMode = denoiser;
		denoiserStableFrames = 0;
	}
	auto* denoiserUi = globals::game::ui;
	const bool denoiserCalm = denoiserStableFrames >= 120 && phase == Phase::Play && !loadingOpen.load() && denoiserUi && !denoiserUi->GameIsPaused() &&
	                          !denoiserUi->IsMenuOpen(RE::MapMenu::MENU_NAME);
	if (globals::features::upscaling.settings.saveVramFreeRtWhenOff && denoiserCalm) {
		if (denoiser != DenoiserMode::Penumbra && (denoiseTiles.texture || history4[0].texture))
			ReleaseDenoiser();
		if (denoiser != DenoiserMode::Sigma && (sigma || sigmaOut.texture))
			ReleaseSigma();
	}
	const bool penumbra = denoiser == DenoiserMode::Penumbra;
	const bool rrMode = denoiser == DenoiserMode::DLSSRR;
	bool sigmaMode = denoiser == DenoiserMode::Sigma;
	if (sigmaMode && !EnsureSigma())
		sigmaMode = false;
	// (45f) TLAS self-check twice a second or so; missed casters noted by the shadow map are described.
	const bool validate = frameIndex % 30 == 0;
	scene.CollectInstances(instances, validate);
	scene.DescribeMissedCasters();
	// (45i) Characters in the traced area: capture, bone matrices, draws (none with the switch off).
	// Other actors at reduced quality only get fewer rays (they are skinned every frame like the player).
	const std::int64_t charactersStart = Qpc();
	const bool charactersWanted = settings.Characters && backend.SkinAvailable();
	if (charactersUpdatedEarly && charactersWanted) {
		// (45n) Early Prep: already done at the frame start (skinDraws / skinBones are that frame's).
		cpuLast.characters = earlyCharactersMs;
	} else {
		if (backend.HasPrepared() && (charactersWanted || charactersUpdatedEarly)) {
			// Prepared without these draws (should not happen): the trace must not pair other ready flags.
			backend.AbandonPrepared();
			earlyThisFrame = false;
		}
		skinDraws.clear();
		skinBones.clear();
		if (settings.Characters && backend.SkinAvailable()) {
			float world[4];
			if (scene.GetLoadedRect(world, false))
				characters.Update(backend, world, blackScreen ? 6.0 : 1.0, blackScreen ? 32u : 6u);
			// Every captured (and retiring) shape is traced, also on a frame without an update.
			characters.CollectDraws(skinDraws, skinBones);
		}
		cpuLast.characters = static_cast<float>(static_cast<double>(Qpc() - charactersStart) * QpcMs());
	}

	const float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::uint32_t width = std::min<std::uint32_t>(static_cast<std::uint32_t>(renderSize.x), desc.Width);
	const std::uint32_t height = std::min<std::uint32_t>(static_cast<std::uint32_t>(renderSize.y), desc.Height);
	if (width == 0 || height == 0)
		return;

	auto* accumulator = *globals::game::currentAccumulator.get();
	auto* shadowSceneNode = accumulator ? accumulator->GetRuntimeData().activeShadowSceneNode : nullptr;
	auto* sunLight = shadowSceneNode && shadowSceneNode->GetRuntimeData().sunLight ? skyrim_cast<RE::NiDirectionalLight*>(shadowSceneNode->GetRuntimeData().sunLight->light.get()) : nullptr;
	if (!sunLight) {
		status = Status::Inactive;
		statusReason = "no sun light";
		return;
	}
	auto direction = sunLight->GetWorldDirection();
	float3 toSun = { -direction.x, -direction.y, -direction.z };
	toSun.Normalize();
	// (46b) The sun's (or the moon's) brightness, for the point lights' sun gate.
	float sunStrength = 0.0f;
	{
		const auto& sunData = sunLight->GetLightRuntimeData();
		sunStrength = std::max({ sunData.diffuse.red, sunData.diffuse.green, sunData.diffuse.blue });
		if (!std::isfinite(sunStrength) || sunStrength < 0.0f)
			sunStrength = 0.0f;
	}

	const auto& invViewProj = fb.GetCameraViewProjInverse(0);
	static_assert(sizeof(invViewProj) == sizeof(float) * 16);

	RayTracingRT::FrameInput input;
	std::memcpy(input.constants.invViewProj, &invViewProj, sizeof(input.constants.invViewProj));
	input.constants.toSun[0] = toSun.x;
	input.constants.toSun[1] = toSun.y;
	input.constants.toSun[2] = toSun.z;
	input.constants.toSun[3] = std::tan(quality.coneDegrees * 3.14159265f / 180.0f);
	std::uint32_t traceFlags = 0;
	if (debugView != DebugView::Disagreement && debugView != DebugView::RawVisibility)
		traceFlags |= kFlagSkipShadowed;
	if (debugView == DebugView::Coverage || debugView == DebugView::HitDepth)
		traceFlags |= kFlagCoverage;
	// (45i) First person: no character ray for the arms / weapon (inside the third-person body).
	if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); !skinDraws.empty() && playerCamera && playerCamera->IsInFirstPerson())
		traceFlags |= kFlagFirstPerson;
	// Noise / adaptive rays: world-locked noise and no extra rays only with Off; animated noise (R2-stepped) and
	// extra rays with every denoiser that averages frames. DLSS RR and SIGMA need fresh noise every real frame;
	// SIGMA takes one ray per pixel (its input is one shadow ray's penumbra).
	const bool worldNoise = denoiser == DenoiserMode::Off;
	bool adaptive = denoiser != DenoiserMode::Off;
	std::uint32_t baseRays = quality.baseRays, tracePattern = quality.tracePattern, farPattern = quality.farPattern;
	if (sigmaMode) {
		adaptive = false;
		baseRays = 1;
		tracePattern = farPattern = 0;
	}
	if (worldNoise)
		traceFlags |= kFlagWorldNoise;
	else
		traceFlags |= kFlagR2Noise;  // (45k) decorrelated animated rays
	traceFlags |= kFlagStableNormal | kFlagAlphaFootprint;
	if (adaptive && quality.extraRays > 0)
		traceFlags |= kFlagAdaptive;
	const bool blockerDist = (penumbra || sigmaMode) && !(traceFlags & kFlagCoverage);
	if (blockerDist)
		traceFlags |= kFlagBlockerDist;
	// (45l) Closest hit only at the edges (Penumbra with extra rays); SIGMA keeps it on the first ray everywhere.
	if (blockerDist && penumbra && (traceFlags & kFlagAdaptive))
		traceFlags |= kFlagBlockerEdges;
	const bool falloff = quality.falloffEnd > quality.falloffStart;
	if (falloff)
		traceFlags |= kFlagFalloff;
	// (45l) The character ray only where the sun ray passes a character's bounds.
	if (!skinDraws.empty()) {
		const double cam[3] = { camX, camY, camZ };
		characters.GetBounds(cam, &input.constants.spheres[0], &input.constants.spheres[4]);
		traceFlags |= kFlagCharCull;
	}
	// (45k) Screen Space Shadows "non-RT casters only": the trace marks the surfaces that are traced casters.
	const bool sssNonRT = sssClaimed;
	if (sssNonRT)
		traceFlags |= kFlagRtSurface;
	input.constants.extra[0] = (traceFlags & kFlagAdaptive) ? static_cast<float>(quality.extraRays) : 0.0f;
	input.constants.extra[1] = quality.falloffStart;
	input.constants.extra[2] = quality.falloffEnd;
	const std::uint32_t patternBits = (tracePattern & 3u) | ((farPattern & 3u) << 2) | (quality.npcReduced ? kPatternNpcReduced : 0u);
	std::memcpy(&input.constants.extra[3], &patternBits, sizeof(float));
	input.constants.params[0] = static_cast<float>(baseRays);
	input.constants.params[1] = static_cast<float>(frameIndex % 1024);
	std::memcpy(&input.constants.params[2], &traceFlags, sizeof(float));
	input.constants.params[3] = kMaxRayLength;
	input.constants.renderSize[0] = static_cast<float>(width);
	input.constants.renderSize[1] = static_cast<float>(height);
	input.constants.renderSize[2] = 1.0f / static_cast<float>(width);
	input.constants.renderSize[3] = 1.0f / static_cast<float>(height);
	std::copy_n(traceRect, 4, input.constants.rect);
	const float biasConstant = kRayBiasConstant, biasPerUnit = kRayBiasPerUnit;
	input.constants.bias[0] = biasConstant;
	input.constants.bias[1] = biasPerUnit;
	input.constants.bias[2] = 8.0f;
	input.constants.bias[3] = skinDraws.empty() ? 0.0f : kCharacterSelfShadowOffset;  // (45i) character ray TMin (character variants only)
	input.constants.world[0] = static_cast<float>(camX);  // (45j) world-locked noise
	input.constants.world[1] = static_cast<float>(camY);
	input.constants.world[2] = static_cast<float>(camZ);
	input.renderWidth = width;
	input.renderHeight = height;
	input.cameraX = camX;
	input.cameraY = camY;
	input.cameraZ = camZ;
	input.instances = &instances;
	input.buildTriangleBudget = kBuildBudget;
	input.validate = validate;
	input.skinDraws = &skinDraws;  // (45i) empty: the 45h pipelines run
	input.buildCpuMs = blackScreen ? kBlackBuildCpuMs : 0.0f;  // (46g)
	input.skinUploadCpuMs = blackScreen ? kBlackSkinUploadCpuMs : 0.0f;
	input.skinBones = &skinBones;
	input.deferWait = true;  // (45l) FinishFrame waits (after the other features' prepasses)
	input.splitSun = settings.SplitTraceSync;  // (46m) used only with point lights traced too
	// (46b) Point-light shadows: this frame's sources, traced right after the sun in the same submission.
	{
		const std::int64_t pointStart = Qpc();
		try {
			if (!PreparePointLights(input, traceRect, camX, camY, camZ, sunStrength))
				input.pointLights = nullptr;
		} catch (const std::exception& e) {
			input.pointLights = nullptr;
			SwitchOffPointLights(std::format("exception in the light selection: {}", e.what()));
		}
		pointCpuMs = static_cast<float>(static_cast<double>(Qpc() - pointStart) * QpcMs());
	}
	// (46f) RTAO: its own list right behind the trace, its own fence (the D3D11 queue waits for it in DrawAmbientOcclusion).
	{
		const std::int64_t aoStart = Qpc();
		try {
			if (!PrepareAO(input, traceRect))
				input.ao = nullptr;
		} catch (const std::exception& e) {
			input.ao = nullptr;
			SwitchOffAO(std::format("exception: {}", e.what()));
		}
		aoCpuMs = static_cast<float>(static_cast<double>(Qpc() - aoStart) * QpcMs());
	}

	auto* context = globals::d3d::context;
	auto* screenSpaceShadows = &globals::features::screenSpaceShadows;
	// Screen Space Shadows inside the traced area ("non-RT casters only"): the texture holds only non-traced
	// casters (and traced ones within their contact range), merged with the mask as min() so shared shadows
	// are not darkened twice.
	const bool combineSSS = typedUavLoadR8 && screenSpaceShadows->loaded && screenSpaceShadows->bendSettings.Enable && screenSpaceShadows->screenSpaceShadowsTexture;
	// Penumbra: spatial filter + clamped history (History4).
	const bool historyStage = penumbra && EnsureDenoiser(quality.denoiseHalf);
	if (!historyStage)
		history4Valid = false;
	if (!sigmaMode)
		sigmaHistoryValid = false;
	const bool debug = debugView != DebugView::Off;

	CombineCB cb{};
	std::memcpy(cb.InvViewProj, &invViewProj, sizeof(cb.InvViewProj));
	std::copy_n(traceRect, 4, cb.Rect);
	cb.Band[0] = kHandOffBand;
	// (45k) DLSS RR: no filter of ours at all (radius 0 = the pixel itself).
	cb.Band[1] = (rrMode || sigmaMode) ? 0.0f : static_cast<float>(quality.smallFilterRadius);
	std::uint32_t combineFlags = (debug ? kFlagDebug : 0u) | (combineSSS ? kFlagCombineSSS : 0u) | ((traceFlags & kFlagSkipShadowed) ? kFlagTraceSkipped : 0u) |
	                             (blockerDist ? kFlagCombineBlockerDist : 0u);
	if (historyStage)
		combineFlags |= kFlagPenumbra | (history4Valid && prevCameraValid ? kFlagHistoryValid : 0u) | kFlagSpatial |
		                (quality.denoiseHalf ? kFlagDenoiseHalf : 0u) | (falloff ? kFlagFalloffDenoise : 0u);
	if (sigmaMode)
		combineFlags |= kFlagSigma;
	const auto& viewProjUnjittered = fb.GetCameraViewProjUnjittered(0);
	std::memcpy(cb.ViewProjUnjittered, &viewProjUnjittered, sizeof(cb.ViewProjUnjittered));
	if (prevCameraValid) {
		std::copy_n(prevViewProj, 16, cb.PrevViewProj);
		std::copy_n(prevViewProjUnjittered, 16, cb.PrevViewProjUnjittered);
		cb.CameraDelta[0] = static_cast<float>(camX - prevCamera[0]);
		cb.CameraDelta[1] = static_cast<float>(camY - prevCamera[1]);
		cb.CameraDelta[2] = static_cast<float>(camZ - prevCamera[2]);
	} else {
		std::memcpy(cb.PrevViewProj, &fb.GetCameraViewProj(0), sizeof(cb.PrevViewProj));
		std::memcpy(cb.PrevViewProjUnjittered, &viewProjUnjittered, sizeof(cb.PrevViewProjUnjittered));
	}
	cb.Denoise[0] = input.constants.toSun[3];
	cb.Denoise[1] = quality.denoiseRadius;
	cb.Denoise[2] = quality.history;
	cb.Denoise[3] = quality.clampSigma;
	cb.Extra[0] = fade;
	cb.Extra[1] = kBlockerRange;
	cb.Extra[3] = static_cast<float>(quality.denoiseTaps);
	cb.Falloff[0] = falloff ? quality.falloffStart : 0.0f;
	cb.Falloff[1] = falloff ? quality.falloffEnd : 0.0f;
	cb.Falloff[2] = quality.farDenoiseScale;
	cb.Falloff[3] = static_cast<float>(farPattern);
	const float4 cameraData = Util::GetCameraData();
	cb.CameraData[0] = cameraData.x;
	cb.CameraData[1] = cameraData.y;
	cb.CameraData[2] = cameraData.z;
	cb.CameraData[3] = cameraData.w;
	cb.SunDirection[0] = toSun.x;
	cb.SunDirection[1] = toSun.y;
	cb.SunDirection[2] = toSun.z;
	cb.SunDirection[3] = input.constants.toSun[3];
	std::memcpy(&cb.Band[2], &combineFlags, sizeof(float));
	cb.Size[0] = width;
	cb.Size[1] = height;
	cb.Size[2] = static_cast<std::uint32_t>(debugView);
	cb.Size[3] = frameIndex;
	combineCB->Update(cb);

	auto* prepare = GetShader(prepareCS, L"Data\\Shaders\\RayTracing\\PrepareCS.hlsl", "PrepareCS");
	auto* combine = GetShader(combineCS, L"Data\\Shaders\\RayTracing\\CombineCS.hlsl", "CombineCS");
	if (!prepare || !combine) {
		Disable(Status::Failed, "a Ray Tracing shader did not compile (see the log)");
		return;
	}

	GpuBucketScope gpuTimer(Util::GpuBucket::RayTracing);
	ID3D11Buffer* cbs[1] = { combineCB->CB() };
	const UINT groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;

	// 0. (45h) Alpha copies of textures D3D12 cannot open (before the trace: the D3D12 queue waits for this).
	const std::int64_t d3d11Start = Qpc();
	if ((scene.AlphaWanted() || settings.Characters) && backend.AlphaAvailable()) {
		const bool rgba = backend.AlphaCopiesRgba();
		if (alphaMaskCS && alphaMaskRgba != rgba)
			alphaMaskCS = nullptr;
		if (!alphaMaskCS) {
			std::vector<std::pair<const char*, const char*>> defines;
			if (rgba)
				defines.emplace_back("ALPHA_RGBA", "");
			alphaMaskCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RayTracing\\AlphaMaskCS.hlsl", defines, "cs_5_0")));
			alphaMaskRgba = rgba;
			if (!alphaMaskCS) {
				// Without it the copies are never made: those shapes would only wait (they keep the shadow map).
				alphaOffReason = "the alpha copy shader (AlphaMaskCS) did not compile";
				logger::error("[RayTracing] Alpha-tested casters: {} -- switched off", alphaOffReason);
				settings.Foliage = static_cast<int>(RayTracingScene::FoliageMode::ShadowMap);
				settings.Cutouts = false;
			}
		}
		if (alphaMaskCS)
			backend.RecordAlphaCopies(alphaMaskCS.get(), kAlphaCopiesPerFrame);
	}

	// 1. Depth + engine mask -> the tracer's shared texture.
	{
		ID3D11ShaderResourceView* srvs[2] = { depth.depthSRV, mask.SRV };
		ID3D11UnorderedAccessView* uavs[1] = { backend.DepthMaskUAV() };
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(prepare, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		ID3D11ShaderResourceView* nullSrvs[2]{};
		ID3D11UnorderedAccessView* nullUavs[1]{};
		context->CSSetShaderResources(0, 2, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}

	// 2. D3D12: builds, skinning and the TLAS (not waiting for D3D11), then the trace (waiting for the
	// depth copy above, GPU-side). The D3D11 queue waits for the trace in FinishFrame.
	const std::int64_t executeStart = Qpc();
	cpuLast.d3d11 = static_cast<float>(static_cast<double>(executeStart - d3d11Start) * QpcMs());
	const bool traced = backend.ExecuteFrame(input);
	const std::int64_t executeEnd = Qpc();
	cpuLast.execute = static_cast<float>(static_cast<double>(executeEnd - executeStart) * QpcMs());
	if (!traced && backend.HasFailed()) {
		Disable(Status::Failed, backend.LastError());
		return;
	}
	if (!traced) {
		// (45e) No trace this frame: the engine's mask stays as it is. Never combine an older
		// trace (it was made from another frame's depth and camera, which is what made the
		// ray-traced shadow jump or vanish while moving). LatchFrame only lets the shadow map
		// drop casters when a free context guaranteed this frame's trace, so this frame's shadow
		// map is complete -- unless that guarantee was broken, which is counted.
		if (filterLatched)
			++latchedWithoutTrace;
		history4Valid = false;
		sigmaHistoryValid = false;
		++frameIndex;
		return;
	}
	if (!scene.HasCoverage() && !filterLatched) {
		// The first BLASes are being built: leave the mask and Screen Space Shadows alone.
		backend.WaitForTrace();
		status = Status::Waiting;
		statusReason = "building the scene";
		history4Valid = false;
		sigmaHistoryValid = false;
		++frameIndex;
		return;
	}

	// (46f) RTAO went out right behind it (second fence): DrawAmbientOcclusion uses it after Screen Space GI.
	aoThisFrame = input.ao != nullptr && backend.GetStats().aoTracedLastFrame;
	aoModeThisFrame = aoThisFrame ? std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1) : -1;  // (46h)
	if (!aoThisFrame)
		aoHistoryValid = false;
	// (46b) The point lights went out with it: Light Limit Fix (next in the prepass list) may tag its lights.
	pointThisFrame = input.pointLights != nullptr && backend.GetStats().pointTracedLastFrame;
	pointSelector.Publish(pointThisFrame);
	if (!pointThisFrame)
		pointK = 0;

	// (45l) The D3D11 side of this frame: FinishFrame (now, or after the other prepasses with Late Sync).
	pending.active = true;
	pending.interior = false;
	pending.traceSync = true;
	pending.splitSun = backend.GetStats().sunSplitLastFrame && backend.SunWaitPending();  // (46m)
	std::memcpy(pending.cbBytes.data(), &cb, sizeof(cb));
	pending.combineFlags = combineFlags;
	pending.width = width;
	pending.height = height;
	pending.writeSSS = combineSSS;
	pending.debug = debug;
	pending.penumbra = historyStage;
	pending.spatial = historyStage;
	pending.half = pending.spatial && quality.denoiseHalf;
	pending.sigma = sigmaMode;
	pending.sssNonRT = sssNonRT;
	pending.biasConstant = biasConstant;
	pending.biasPerUnit = biasPerUnit;
	pending.toSun[0] = toSun.x;
	pending.toSun[1] = toSun.y;
	pending.toSun[2] = toSun.z;
	pending.camera[0] = camX;
	pending.camera[1] = camY;
	pending.camera[2] = camZ;
	std::memcpy(pending.viewProj, &fb.GetCameraViewProj(0), sizeof(pending.viewProj));
	std::memcpy(pending.viewProjUnjittered, &viewProjUnjittered, sizeof(pending.viewProjUnjittered));

	activeThisFrame = true;
	status = Status::Active;
	statusReason.clear();
	++frameIndex;

	// (45f) What the TLAS holds, in the log: on the first active frame and then every 30 s.
	const std::int64_t now = Qpc();
	if (lastTlasLog == 0 || static_cast<double>(now - lastTlasLog) * QpcMs() > 30000.0) {
		lastTlasLog = now;
		const auto& check = backend.GetTlasCheck();
		const auto& sc = scene.GetStats();
		std::uint32_t notBuilt = 0;
		for (const auto count : check.notBuilt)
			notBuilt += count;
		std::uint32_t mismatched = 0;
		for (const auto count : check.mismatched)
			mismatched += count;
		std::string reasons;
		for (int r = 0; r < static_cast<int>(RayTracingScene::MissReason::Count); ++r) {
			if (sc.missedByReason[r])
				reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", RayTracingScene::kMissReasonNames[r], sc.missedByReason[r]);
		}
		logger::info("[RayTracing] TLAS: {} cell chunks, {} doors/activators/movable, {} items, {} trees/plants, {} alpha-tested ({} instances not built yet); "
					 "every instance mask 0x{:02X}, flags 0x{:X} (triangle cull disable, force opaque); self-check: {} of {} instances outside the "
					 "engine's bounds (cell chunks {}, objects {}; {} in total); missed casters in this area {} ({}; {} re-checked later while pending, "
					 "{} recaptures requested); alpha-tested shapes taken as opaque {}, traced with their cut-out {}; persistent references taken: {} statics, {} objects",
			check.written[RayTracingRT::kCategoryCell], check.written[RayTracingRT::kCategoryMovable], check.written[RayTracingRT::kCategoryItem],
			check.written[RayTracingRT::kCategoryTree], check.written[RayTracingRT::kCategoryAlpha], notBuilt, RayTracingRT::Backend::kInstanceMask,
			static_cast<std::uint32_t>(RayTracingRT::Backend::kInstanceFlags), mismatched, check.checked, check.mismatched[RayTracingRT::kCategoryCell],
			mismatched - check.mismatched[RayTracingRT::kCategoryCell], check.mismatchedTotal, sc.missedConfirmed, reasons.empty() ? "none" : reasons,
			sc.missedPending, sc.missedRecaptures, sc.skipped.cutoutAsOpaque, sc.alpha.registered, sc.persistentRefs, sc.persistentObjects);
		if (settings.Characters) {
			// (45i)
			const auto& ch = characters.GetStats();
			const auto& sk = backend.GetSkinStats();
			logger::info("[RayTracing] Characters: {} actors ({} in range), {} of {} shapes traced ({} skinned, {} rigid, {} physics, {} alpha-tested), "
						 "{} in the TLAS, {:.2f} M vertices skinned, {} refits / {} builds / {} reused last frame, {:.0f} MB; left to the shadow map: physics off {}, "
						 "alpha off {}, alpha failed {}, shader {}, type {}, no CPU data {}, bad data {}, placement {}, budget/size {}; bone indices palette {}, global {}",
				ch.actors, ch.inRange, ch.registered, ch.shapes, ch.skinnedShapes, ch.rigidShapes, ch.physicsShapes, ch.alphaShapes, sk.drawsLastFrame,
				static_cast<double>(sk.verticesLastFrame) / 1e6, sk.refitsLastFrame, sk.buildsLastFrame, sk.reusedLastFrame,
				static_cast<double>(sk.bytes + sk.scratchBytes + sk.boneBytes) / (1024.0 * 1024.0), ch.skipPhysicsOff, ch.skipAlphaOff, ch.skipAlphaFailed,
				ch.skipShader, ch.skipGeometryType, ch.skipNoCpuData, ch.skipBadData, ch.skipPlacement, ch.skipBackend, ch.paletteMode, ch.globalMode);
		}
	}
}

void RayTracing::FinishFrame()
{
	// (45l) The D3D11 side of a traced frame. GPU-side ordering only: nothing here waits on the CPU.
	if (!pending.active)
		return;
	pending.active = false;
	const std::int64_t start = Qpc();
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	auto* renderer = globals::game::renderer;
	auto& mask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto* screenSpaceShadows = &globals::features::screenSpaceShadows;
	const std::uint32_t width = pending.width, height = pending.height;
	const UINT groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;

	// 1. The D3D11 queue waits for the trace (on the GPU). Timed: the GPU stall it causes.
	// (46h) Not on an interior frame that traced RTAO only: nothing here reads the trace then, and the AO list behind it on
	// the same queue signals the second fence after it, so DrawAmbientOcclusion's wait covers it (rt_sync is not extended).
	// (46m) Split Trace Sync: here the sun alone; the point lights' part right before their denoiser (step 5).
	if (pending.traceSync) {
		timers->Begin(Util::GpuBucket::RayTracingSync);
		if (pending.splitSun)
			backend.WaitForSun();
		else
			backend.WaitForTrace();
		timers->End(Util::GpuBucket::RayTracingSync);
	}
	// Every return before step 5 still leaves no point-light wait behind: the next trace (RunFrame) takes it first.
	if (backend.HasFailed() || !mask.texture || !mask.SRV || !maskTemp) {
		if (backend.HasFailed())
			Disable(Status::Failed, backend.LastError());
		history4Valid = false;
		sigmaHistoryValid = false;
		return;
	}
	if (pending.interior) {
		// (46c) Indoors: the point lights only (denoiser, pixel shader binding), then the engine shadow lights' mask channels.
		if (pointThisFrame) {
			try {
				FinishPointLights();
			} catch (const std::exception& e) {
				SwitchOffPointLights(std::format("exception in the denoiser: {}", e.what()));
			}
		}
		WritePointMask(pointThisFrame && pointDenoised);
		interiorTracedLastFrame = pointThisFrame && pointDenoised;
		if (interiorTracedLastFrame) {
			for (const auto& entry : pointSelector.MaskEntries()) {
				skipCandidates.push_back(entry.light);
				skipEntries.push_back(entry);
			}
			std::sort(skipCandidates.begin(), skipCandidates.end());
			std::sort(skipEntries.begin(), skipEntries.end(),
				[](const RayTracingPointLights::MaskEntry& a, const RayTracingPointLights::MaskEntry& b) { return a.light < b.light; });
		}
		std::copy_n(pending.viewProj, 16, prevViewProj);
		std::copy_n(pending.viewProjUnjittered, 16, prevViewProjUnjittered);
		std::copy_n(pending.camera, 3, prevCamera);
		prevCameraValid = true;
		combinedThisFrame = true;
		if (pending.debug)
			lastOverlayFrame = frameIndex - 1;
		cpuLast.d3d11 += static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
		return;
	}
	CombineCB cb;
	std::memcpy(&cb, pending.cbBytes.data(), sizeof(cb));
	std::uint32_t combineFlags = pending.combineFlags;
	auto setFlags = [&]() {
		std::memcpy(&cb.Band[2], &combineFlags, sizeof(float));
		combineCB->Update(cb);
	};
	ID3D11Buffer* cbs[1] = { combineCB->CB() };

	// 2. (45k) Screen Space Shadows "non-RT casters only", now that the trace marked the traced casters.
	if (pending.sssNonRT && sssClaimed) {
		screenSpaceShadows->DrawDeferred(backend.VisibilitySRV(), settings.RtContactScale, pending.biasConstant, pending.biasPerUnit);
		sssClaimed = false;
	}

	// 3. (45k) Denoiser Penumbra: tiles, then the horizontal and vertical penumbra filter of this frame
	// ((45l) at half resolution with the half input the tile pass writes).
	if (pending.spatial) {
		auto* tileShader = GetShader(denoiseTileCS, L"Data\\Shaders\\RayTracing\\DenoiseTileCS.hlsl", "DenoiseTileCS");
		auto* hShader = pending.half ? GetShaderDefined(denoiseHalfHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (half)", "DENOISE_HALF") :
		                               GetShaderDefined(denoiseHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS", nullptr);
		auto* vShader = pending.half ? GetShaderDefined(denoiseHalfVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (half, vertical)", "DENOISE_HALF,DENOISE_VERTICAL") :
		                               GetShaderDefined(denoiseVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (vertical)", "DENOISE_VERTICAL");
		if (!tileShader || !hShader || !vShader) {
			logger::warn("[RayTracing] Denoiser Penumbra: a shader did not compile; Denoiser set to Off");
			settings.Denoiser = static_cast<int>(DenoiserMode::Off);
			history4Valid = false;
			combineFlags &= ~(kFlagPenumbra | kFlagHistoryValid | kFlagSpatial | kFlagDenoiseHalf);
			pending.penumbra = false;
			setFlags();
		} else {
			context->CSSetConstantBuffers(0, 1, cbs);
			ID3D11ShaderResourceView* nullSrvs[3]{};
			ID3D11UnorderedAccessView* nullUavs[2]{};
			timers->Begin(Util::GpuBucket::RayTracingDenoise);
			{
				ID3D11ShaderResourceView* srvs[2] = { backend.VisibilitySRV(), backend.DepthMaskSRV() };
				ID3D11UnorderedAccessView* uavs[2] = { denoiseTiles.uav.get(), pending.half ? denoiseHalfIn.uav.get() : nullptr };
				context->CSSetShaderResources(0, 2, srvs);
				context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
				context->CSSetShader(tileShader, nullptr, 0);
				context->Dispatch(groupsX, groupsY, 1);
				context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
			}
			timers->End(Util::GpuBucket::RayTracingDenoise);
			timers->Begin(Util::GpuBucket::RayTracingDenoiseFilter);
			const UINT fx = pending.half ? ((width + 1) / 2 + 7) / 8 : groupsX, fy = pending.half ? ((height + 1) / 2 + 7) / 8 : groupsY;
			{
				ID3D11ShaderResourceView* srvs[3] = { pending.half ? denoiseHalfIn.srv.get() : backend.VisibilitySRV(), backend.DepthMaskSRV(), denoiseTiles.srv.get() };
				ID3D11UnorderedAccessView* uav = denoiseH.uav.get();
				context->CSSetShaderResources(0, 3, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->CSSetShader(hShader, nullptr, 0);
				context->Dispatch(fx, fy, 1);
				context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
				context->CSSetShaderResources(0, 3, nullSrvs);
			}
			{
				ID3D11ShaderResourceView* srvs[3] = { denoiseH.srv.get(), backend.DepthMaskSRV(), denoiseTiles.srv.get() };
				ID3D11UnorderedAccessView* uav = denoiseOut.uav.get();
				context->CSSetShaderResources(0, 3, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->CSSetShader(vShader, nullptr, 0);
				context->Dispatch(fx, fy, 1);
				context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
				context->CSSetShaderResources(0, 3, nullSrvs);
			}
			timers->End(Util::GpuBucket::RayTracingDenoiseFilter);
		}
	}

	// 3b. (45l) Denoiser NRD SIGMA.
	if (pending.sigma) {
		timers->Begin(Util::GpuBucket::RayTracingSigma);
		const bool ok = RunSigma(pending.toSun);
		timers->End(Util::GpuBucket::RayTracingSigma);
		if (!ok) {
			// This frame: the small filter instead; the history restarts.
			combineFlags &= ~kFlagSigma;
			cb.Band[1] = static_cast<float>(lastQuality.smallFilterRadius);
			setFlags();
		}
	}
	const bool penumbraActive = (combineFlags & kFlagPenumbra) != 0u;
	const bool sigmaActive = (combineFlags & kFlagSigma) != 0u;

	// 4. Merge into the engine's mask, Screen Space Shadows inside the traced area.
	timers->Begin(Util::GpuBucket::RayTracingCombine);
	{
		auto* combine = GetShader(combineCS, L"Data\\Shaders\\RayTracing\\CombineCS.hlsl", "CombineCS");
		const std::uint32_t read4 = history4Index, write4 = history4Index ^ 1u;
		const bool writeSSS = pending.writeSSS;
		ID3D11ShaderResourceView* srvs[6] = { mask.SRV, backend.VisibilitySRV(), backend.DepthMaskSRV(), sigmaActive ? sigmaOut.srv.get() : nullptr,
			(combineFlags & kFlagSpatial) ? denoiseOut.srv.get() : nullptr, penumbraActive ? history4[read4].srv.get() : nullptr };
		ID3D11UnorderedAccessView* uavs[5] = {
			maskTempUAV.get(),
			writeSSS ? screenSpaceShadows->screenSpaceShadowsTexture->uav.get() : nullptr,
			pending.debug ? debugColorUAV.get() : nullptr,
			nullptr,
			penumbraActive ? history4[write4].uav.get() : nullptr
		};
		// Screen Space Shadows bound its texture to PS t45 in its own Prepass; a UAV write needs it unbound.
		ID3D11ShaderResourceView* nullPs[1]{};
		if (writeSSS)
			context->PSSetShaderResources(45, 1, nullPs);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 6, srvs);
		context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
		context->CSSetShader(combine, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		ID3D11ShaderResourceView* nullSrvs[6]{};
		ID3D11UnorderedAccessView* nullUavs[5]{};
		context->CSSetShaderResources(0, 6, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 5, nullUavs, nullptr);
		if (penumbraActive) {
			history4Index = write4;
			history4Valid = true;
		}
		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullCb[1]{};
		context->CSSetConstantBuffers(0, 1, nullCb);
		if (writeSSS) {
			ID3D11ShaderResourceView* sssView = screenSpaceShadows->screenSpaceShadowsTexture->srv.get();
			context->PSSetShaderResources(45, 1, &sssView);
		}
		// Only the render area was written; the rest of the work texture is never read.
		D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		context->CopySubresourceRegion(mask.texture, 0, 0, 0, 0, maskTemp.get(), 0, &box);
	}
	timers->End(Util::GpuBucket::RayTracingCombine);
	// 5. (46m) Split Trace Sync: the rest of the trace (the point lights) before anything reads it.
	if (pending.splitSun) {
		timers->Begin(Util::GpuBucket::RayTracingSyncPoint);
		backend.WaitForTrace();
		timers->End(Util::GpuBucket::RayTracingSyncPoint);
		if (backend.HasFailed()) {
			Disable(Status::Failed, backend.LastError());
			return;
		}
	}
	// (46b) Point-light shadows: denoise, then bind them for the pixel shaders.
	if (pointThisFrame) {
		try {
			FinishPointLights();
		} catch (const std::exception& e) {
			SwitchOffPointLights(std::format("exception in the denoiser: {}", e.what()));
		}
	}
	// This frame's camera is the history's next time.
	std::copy_n(pending.viewProj, 16, prevViewProj);
	std::copy_n(pending.viewProjUnjittered, 16, prevViewProjUnjittered);
	std::copy_n(pending.camera, 3, prevCamera);
	prevCameraValid = true;
	combinedThisFrame = true;
	if (pending.debug)
		lastOverlayFrame = frameIndex - 1;
	cpuLast.d3d11 += static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
}

void RayTracing::DrawDebugOverlay()
{
	// (46f) RTAO's views (Raw / Filtered / the split line) wrote debugColor this frame, whatever the shadow view.
	if (!(settings.Enabled && aoDebugDrawn)) {
		if (!settings.Enabled || debugView == DebugView::Off || !combinedThisFrame || lastOverlayFrame + 1 != frameIndex)
			return;
		// (46c) Indoors only the point-light views have content (nothing of the sun was traced or combined).
		if (interiorThisFrame && (debugView < DebugView::PLCandidates || !pointDenoised))
			return;
	}
	auto* overlay = GetShader(overlayCS, L"Data\\Shaders\\RayTracing\\OverlayCS.hlsl", "OverlayCS");
	auto* renderer = globals::game::renderer;
	const auto& main = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[0]];
	if (!overlay || !main.UAV || !debugColorSRV)
		return;
	auto* context = globals::d3d::context;
	ID3D11Buffer* cbs[1] = { combineCB->CB() };
	ID3D11ShaderResourceView* srvs[1] = { debugColorSRV.get() };
	ID3D11UnorderedAccessView* uavs[1] = { main.UAV };
	context->CSSetConstantBuffers(0, 1, cbs);
	context->CSSetShaderResources(0, 1, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(overlay, nullptr, 0);
	const float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	context->Dispatch((static_cast<UINT>(renderSize.x) + 7) / 8, (static_cast<UINT>(renderSize.y) + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[1]{};
	ID3D11UnorderedAccessView* nullUavs[1]{};
	ID3D11Buffer* nullCb[1]{};
	context->CSSetShaderResources(0, 1, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);
}

// ---------------------------------------------------------------------------------------------
// (46b) Point-light shadows
// ---------------------------------------------------------------------------------------------

void RayTracing::SwitchOffPointLights(std::string a_reason)
{
	// Only the point lights: the sun keeps going. The page shows why; Retry (or ticking the box off and on) retries.
	// (46f) A runtime state, not the setting: a later save keeps the user's choice, and the next session tries again.
	pointOffReason = std::move(a_reason);
	pointAutoOff = true;
	pointSelector.Clear();
	pointThisFrame = false;
	pointHistoryValid = false;
	UnbindPointLights();
	logger::warn("[RayTracing] Point-light shadows switched off: {}", pointOffReason);
}

void RayTracing::UpdatePointLights()
{
	// Turned on (again) by the user (or Retry): an earlier automatic switch-off is forgotten.
	if (settings.PointLights && !pointRequested) {
		backend.ClearPointFault();
		pointOffReason.clear();
		pointAutoOff = false;
	}
	if (PointLightsOn() && !backend.PointFault().empty())
		SwitchOffPointLights(backend.PointFault());
	else if (PointLightsOn() && backend.IsReady() && !backend.PointAvailable())
		SwitchOffPointLights("the GPU driver did not accept the point-light ray query shader");
	pointRequested = settings.PointLights;
	// (46c) Interiors switched on (again) by the user: an earlier automatic switch-off is forgotten.
	if (settings.PointLightsInteriors && !interiorRequested) {
		interiorOffReason.clear();
		interiorAutoOff = false;
	}
	interiorRequested = settings.PointLightsInteriors;
	if (!PointLightsOn()) {
		pointSelector.Clear();
		if (pointVis[0].texture)
			ReleasePointDenoiser();
		if (backend.IsReady() && backend.PointBytes())
			backend.ReleasePointResources();
	}
}

void RayTracing::UnbindPointLights()
{
	if (!pointBound)
		return;
	ID3D11ShaderResourceView* nulls[2]{};
	globals::d3d::context->PSSetShaderResources(40, 2, nulls);
	pointBound = false;
}

void RayTracing::ReleasePointDenoiser()
{
	UnbindPointLights();
	for (auto* t : { &pointVis[0], &pointVis[1], &pointIdx[0], &pointIdx[1], &pointDist[0], &pointDist[1] })
		*t = {};
	pointHistoryValid = false;
	d3d11Bytes -= std::min(d3d11Bytes, pointD3D11Bytes);
	pointD3D11Bytes = 0;
}

bool RayTracing::EnsurePointDenoiser(std::uint32_t a_width, std::uint32_t a_height)
{
	if (pointVis[0].texture && pointVis[1].texture && pointIdx[0].texture && pointIdx[1].texture && pointDist[0].texture && pointDist[1].texture) {
		D3D11_TEXTURE2D_DESC d{};
		pointVis[0].texture->GetDesc(&d);
		if (d.Width == a_width && d.Height == a_height)
			return true;
	}
	ReleasePointDenoiser();
	bool ok = true;
	for (int i = 0; i < 2 && ok; ++i) {
		ok = CreateDenoiseTexture(this, pointVis[i].texture, pointVis[i].srv, pointVis[i].uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing PointVis") &&
		     CreateDenoiseTexture(this, pointIdx[i].texture, pointIdx[i].srv, pointIdx[i].uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing PointIdx") &&
		     CreateDenoiseTexture(this, pointDist[i].texture, pointDist[i].srv, pointDist[i].uav, a_width, a_height, DXGI_FORMAT_R16_FLOAT, "RayTracing PointDist");
	}
	if (!ok) {
		ReleasePointDenoiser();
		SwitchOffPointLights("could not create the denoiser textures");
		return false;
	}
	pointD3D11Bytes = static_cast<std::uint64_t>(a_width) * a_height * 2 * (4 + 4 + 2);
	d3d11Bytes += pointD3D11Bytes;
	pointHistoryValid = false;
	pointWrite = 0;
	return true;
}

bool RayTracing::PreparePointLights(RayTracingRT::FrameInput& a_input, const float a_traceRect[4], double a_camX, double a_camY, double a_camZ, float a_sunStrength,
	bool a_interior)
{
	pointK = 0;
	if (!PointLightsOn() || !backend.PointAvailable() || !maskTemp)
		return false;
	// (46c) Sized to the render area (2560x1440 behind DLSS Quality at 4K), not to the shadow mask (the output size): the
	// trace and the pixel shaders only ever address the render area. Recreated when the render size changes.
	const std::uint32_t width = std::max(a_input.renderWidth, 1u), height = std::max(a_input.renderHeight, 1u);
	if (!backend.EnsurePointTextures(width, height)) {
		if (!backend.PointFault().empty())
			SwitchOffPointLights(backend.PointFault());
		return false;
	}
	if (!EnsurePointDenoiser(width, height))
		return false;

	RayTracingPointLights::Config config;
	config.native = settings.PointLightsNative;
	config.keepSSPLS = settings.PointLightsKeepSSPLS;
	config.split = pointSplit;
	config.maxSources = static_cast<std::uint32_t>(std::clamp(settings.PointLightsMaxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources)));
	config.maxDistance = settings.PointLightsDistance;
	config.clearance = settings.PointLightsClearance;
	// (46c) Indoors: spot shadow lights, window beams, the lights whose shadow map was left out this frame.
	config.interior = a_interior;
	config.beams = settings.PointLightsWindowBeams;
	// (46d) The player's lights (never shadowed by the player's own body) and ownerless spell / effect lights.
	config.player = settings.PointLightsPlayer;
	config.effects = settings.PointLightsEffects;
	config.stable = settings.PointLightsStable;  // (46g)
	// (46s)
	config.steady = settings.PointLightsSteady;
	config.mergeHysteresis = settings.PointLightsMergeHysteresis;
	config.meshless = static_cast<std::uint32_t>(std::clamp(settings.PointLightsMeshless, 0, 2));
	double reachPoint[3]{};
	if (auto* player = RE::PlayerCharacter::GetSingleton()) {
		const auto p = player->GetPosition();
		if (std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z)) {
			reachPoint[0] = p.x;
			reachPoint[1] = p.y;
			reachPoint[2] = p.z + 64.0;  // about the chest
			config.reachPoint = reachPoint;
		}
	}
	UpdateBeamGuard();
	config.excludedBeams = settings.PointLightsBeamGuard && !excludedBeams.empty() ? &excludedBeams : nullptr;
	config.forced = a_interior && !skippedLights.empty() ? &skippedLights : nullptr;
	// (46e) The characters in this frame's TLAS: a light one of them carries skips that character's body.
	config.owners = (a_input.skinDraws && !a_input.skinDraws->empty()) ? &characters.Owners() : nullptr;
	// (46q) Carrier Casts Shadows: the player in third person is its own lights' carrier too (first person: as 46p, the player's
	// body is never tested against its own lights; the first-person arm would cover most of the view).
	{
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		config.playerCarrier = settings.CarrierShadows && playerCamera && !playerCamera->IsInFirstPerson();
	}
	const double camera[3] = { a_camX, a_camY, a_camZ };
	const float worldRect[4] = { static_cast<float>(a_traceRect[0] + a_camX), static_cast<float>(a_traceRect[1] + a_camY),
		static_cast<float>(a_traceRect[2] + a_camX), static_cast<float>(a_traceRect[3] + a_camY) };
	const auto& viewProj = globals::game::frameBufferCached.GetCameraViewProj(0);
	static_assert(sizeof(viewProj) == sizeof(float) * 16);
	float viewProjF[16];
	std::memcpy(viewProjF, &viewProj, sizeof(viewProjF));
	const std::uint32_t scan = pointSelector.Select(config, camera, viewProjF, worldRect, globals::state->frameCount, pointSources.data());
	lastSlotBeams = pointSelector.SlotBeams();  // (46g) the beam guard reads the counters of these slots a few frames later
	if (scan == 0)
		return false;

	const std::uint32_t K = static_cast<std::uint32_t>(ValidPointK(a_interior ? settings.PointLightsPerPixelInterior : settings.PointLightsPerPixel));
	auto& c = pointInput.constants;
	std::memcpy(c.invViewProj, a_input.constants.invViewProj, sizeof(c.invViewProj));
	std::copy_n(a_input.constants.renderSize, 4, c.renderSize);
	std::copy_n(a_traceRect, 4, c.rect);
	std::uint32_t flags = 0;
	if (a_input.skinDraws && !a_input.skinDraws->empty()) {
		flags |= kPointFlagCharacters | kPointFlagCharCull;
		if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->IsInFirstPerson())
			flags |= kPointFlagFirstPerson;
	}
	if (settings.PointLightsStable)
		flags |= kPointFlagSticky;  // (46g)
	const int beamCull = EffectiveBeamCull();  // (46n)
	if (beamCull == 1)
		flags |= kPointFlagBeamCullFront;
	else if (beamCull == 2)
		flags |= kPointFlagBeamCullBack;
	c.params[0] = static_cast<float>(scan);
	c.params[1] = static_cast<float>(frameIndex % 1024);
	std::memcpy(&c.params[2], &flags, sizeof(float));
	c.params[3] = static_cast<float>(K);
	c.bias[0] = kRayBiasConstant;
	c.bias[1] = kRayBiasPerUnit;
	c.bias[2] = kCharacterSelfShadowOffset;
	c.bias[3] = settings.PointLightsSize;
	c.extra[0] = kPointMinShare;
	c.extra[1] = kPointMinContribution;
	c.extra[2] = kPointSunShare;
	c.extra[3] = settings.CarrierShadows ? std::clamp(settings.CarrierClearance, 0.0f, 64.0f) : -1.0f;  // (46q) < 0: as 46p
	std::copy_n(a_input.constants.spheres, 8, c.spheres);
	std::copy_n(a_input.constants.toSun, 3, c.sun);
	c.sun[3] = a_sunStrength;
	// (46c) The interior's box (z; the rect is its xy): window beams' rays end where they leave it.
	c.bounds[0] = -1e9f;
	c.bounds[1] = 1e9f;
	c.bounds[2] = kBeamRayMargin;
	// (46o) Window beams' angular size: their rays are jittered inside this cone (tan of the half angle; 0 = 46n).
	pointBeamTan = settings.PointLightsWindowBeams && settings.PointLightsBeamSoftness > 0.0f ?
	                   std::tan(std::clamp(settings.PointLightsBeamSoftness, 0.0f, 3.0f) * 0.017453293f) :
	                   0.0f;
	c.bounds[3] = pointBeamTan;
	pointBeamMask = 0;
	for (std::uint32_t s = 0; s < scan && s < 64; ++s)
		if (pointSources[s].info[0] & RayTracingPointLights::kSourceBeam)
			pointBeamMask |= 1ull << s;
	if (a_interior) {
		double lo[3], hi[3];
		if (scene.GetInteriorBounds(lo, hi)) {
			c.bounds[0] = static_cast<float>(lo[2] - a_camZ);
			c.bounds[1] = static_cast<float>(hi[2] - a_camZ);
		}
	}
	pointInput.sources = pointSources.data();
	pointInput.sourceBytes = static_cast<std::uint32_t>(sizeof(pointSources));
	pointInput.scan = scan;
	// (46d) Outdoors too (leaves, bushes, cut-outs as the per-kind menu traces them), with the switch.
	pointInput.alpha = (a_interior || settings.PointLightsAlphaOutdoors) && backend.PointAlphaAvailable();
	a_input.pointLights = &pointInput;
	pointK = K;
	return true;
}

void RayTracing::FinishPointLights()
{
	auto* shader = GetShader(pointDenoiseCS, L"Data\\Shaders\\RayTracing\\PointDenoiseCS.hlsl", "PointDenoiseCS");
	if (!shader) {
		SwitchOffPointLights("the point-light denoiser (PointDenoiseCS) did not compile");
		return;
	}
	if (!pointVis[0].texture || !backend.PointVisibilitySRV() || !backend.PointSourcesSRV())
		return;
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	const std::uint32_t write = pointWrite, read = pointWrite ^ 1u;
	const bool plDebug = debugView >= DebugView::PLCandidates && debugView <= DebugView::PLDenoised && debugColorUAV;
	PointDenoiseCB cb{};
	cb.Params[0] = settings.PointLightsSize;
	cb.Params[1] = kPointHistoryWeight;
	const std::uint32_t flags = (pointHistoryValid && prevCameraValid ? kPointDenoiseHistory : 0u) | (plDebug ? kPointDenoiseDebug : 0u);
	std::memcpy(&cb.Params[2], &flags, sizeof(float));
	cb.Params[3] = kPointMaxFilterRadius;
	cb.Extra[0] = static_cast<float>(pointK);
	{
		// (46o) Which source slots are window beams, and their softness: their penumbra width is blocker distance x tan(angle).
		const std::uint32_t lo = static_cast<std::uint32_t>(pointBeamMask), hi = static_cast<std::uint32_t>(pointBeamMask >> 32);
		std::memcpy(&cb.Extra[1], &lo, sizeof(float));
		std::memcpy(&cb.Extra[2], &hi, sizeof(float));
		cb.Extra[3] = pointBeamMask ? pointBeamTan : 0.0f;
	}
	{
		// (46d) Moving / new sources: their history is dropped in proportion (slot s in byte s % 4 of Motion[s / 4]).
		const auto& motion = pointSelector.SlotMotion();
		for (std::uint32_t s = 0; s < RayTracingPointLights::kMaxSources; ++s)
			cb.Motion[s / 4] |= static_cast<std::uint32_t>(motion[s]) << ((s % 4) * 8);
	}
	pointDenoiseCB->Update(cb);

	timers->Begin(Util::GpuBucket::RayTracingPointDenoise);
	ID3D11Buffer* cbs[2] = { combineCB->CB(), pointDenoiseCB->CB() };
	ID3D11ShaderResourceView* srvs[6] = { backend.PointVisibilitySRV(), backend.PointSourcesSRV(), backend.DepthMaskSRV(), pointVis[read].srv.get(),
		pointIdx[read].srv.get(), pointDist[read].srv.get() };
	ID3D11UnorderedAccessView* uavs[4] = { pointVis[write].uav.get(), pointIdx[write].uav.get(), pointDist[write].uav.get(), plDebug ? debugColorUAV.get() : nullptr };
	context->CSSetConstantBuffers(0, 2, cbs);
	context->CSSetShaderResources(0, 6, srvs);
	context->CSSetUnorderedAccessViews(0, 4, uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch((pending.width + 7) / 8, (pending.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[6]{};
	ID3D11UnorderedAccessView* nullUavs[4]{};
	ID3D11Buffer* nullCbs[2]{};
	context->CSSetShaderResources(0, 6, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 4, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 2, nullCbs);
	context->CSSetShader(nullptr, nullptr, 0);
	timers->End(Util::GpuBucket::RayTracingPointDenoise);

	// For the pixel shaders of this frame (RayTracing/PointLightShadows.hlsli).
	ID3D11ShaderResourceView* views[2] = { pointVis[write].srv.get(), pointIdx[write].srv.get() };
	context->PSSetShaderResources(40, 2, views);
	pointBound = true;
	pointWrite ^= 1u;
	pointHistoryValid = true;
	pointDenoised = true;
}

// ---------------------------------------------------------------------------------------------
// (46f) Ray-traced ambient occlusion (RTAO)
// ---------------------------------------------------------------------------------------------

void RayTracing::SwitchOffAO(std::string a_reason)
{
	// Only RTAO: the sun and the point lights keep going. A runtime state (the saved setting stays on and is retried
	// next session); the page shows why, Retry (or ticking the box off and on) tries again now.
	aoOffReason = std::move(a_reason);
	aoAutoOff = true;
	aoThisFrame = false;
	aoHistoryValid = false;
	logger::warn("[RayTracing] Ray-traced ambient occlusion switched off: {}", aoOffReason);
}

void RayTracing::UpdateAO()
{
	// Turned on (again) by the user (or Retry): an earlier automatic switch-off is forgotten.
	if (settings.AO && !aoRequested) {
		backend.ClearAOFault();
		aoOffReason.clear();
		aoAutoOff = false;
	}
	if (AOOn() && !backend.AOFault().empty())
		SwitchOffAO(backend.AOFault());
	else if (AOOn() && backend.IsReady() && !backend.AOAvailable())
		SwitchOffAO("the GPU driver did not accept the RTAO ray query shader");
	aoRequested = settings.AO;
	// (46h) RTAO indoors ticked (again) by the user, or its Retry Indoors: an earlier automatic switch-off of the interior part
	// (shared with the point lights' interiors: one interior capture) is forgotten.
	if (settings.AO && settings.AOInteriors && !aoInteriorRequested) {
		interiorOffReason.clear();
		interiorAutoOff = false;
	}
	aoInteriorRequested = settings.AO && settings.AOInteriors;
	if (!AOOn()) {
		aoHistoryValid = false;
		if (aoOut.texture)
			ReleaseAOFilter();
		if (backend.IsReady() && backend.AOBytes())
			backend.ReleaseAOResources();
	}
}

void RayTracing::ReleaseAOFilter()
{
	for (auto* t : { &aoLo, &aoLoBlur, &aoLoDepth, &aoLoNormal, &aoFiltered, &aoHistory[0], &aoHistory[1], &aoOut })
		*t = {};
	aoWidth = aoHeight = 0;
	aoHistoryValid = false;
	d3d11Bytes -= std::min(d3d11Bytes, aoD3D11Bytes);
	aoD3D11Bytes = 0;
}

bool RayTracing::EnsureAOFilter(std::uint32_t a_width, std::uint32_t a_height)
{
	// Render-sized (the render area at the origin, as every consumer of SSGI's AO addresses it); the filter's
	// quarter-resolution grid is half width, half height (the SSRT confidence filter's).
	if (aoOut.texture && aoWidth == a_width && aoHeight == a_height)
		return true;
	ReleaseAOFilter();
	const std::uint32_t lw = (a_width + 1) / 2, lh = (a_height + 1) / 2;
	std::uint64_t bytes = 0;
	auto create = [&](DenoiseTexture& o_texture, std::uint32_t a_w, std::uint32_t a_h, DXGI_FORMAT a_format, std::uint32_t a_bytesPerPixel, const char* a_name) {
		if (!CreateDenoiseTexture(this, o_texture.texture, o_texture.srv, o_texture.uav, a_w, a_h, a_format, a_name))
			return false;
		bytes += static_cast<std::uint64_t>(a_w) * a_h * a_bytesPerPixel;
		return true;
	};
	const bool ok = create(aoLo, lw, lh, DXGI_FORMAT_R8_UNORM, 1, "RayTracing RTAO Lo") && create(aoLoBlur, lw, lh, DXGI_FORMAT_R8_UNORM, 1, "RayTracing RTAO LoBlur") &&
	                create(aoLoDepth, lw, lh, DXGI_FORMAT_R32_FLOAT, 4, "RayTracing RTAO LoDepth") &&
	                create(aoLoNormal, lw, lh, DXGI_FORMAT_R8G8B8A8_SNORM, 4, "RayTracing RTAO LoNormal") &&
	                create(aoFiltered, a_width, a_height, DXGI_FORMAT_R8_UNORM, 1, "RayTracing RTAO Filtered") &&
	                create(aoHistory[0], a_width, a_height, DXGI_FORMAT_R8_UNORM, 1, "RayTracing RTAO History") &&
	                create(aoHistory[1], a_width, a_height, DXGI_FORMAT_R8_UNORM, 1, "RayTracing RTAO History") &&
	                create(aoOut, a_width, a_height, DXGI_FORMAT_R8_UNORM, 1, "RayTracing RTAO Out");
	if (!ok) {
		ReleaseAOFilter();
		SwitchOffAO("could not create its filter textures");
		return false;
	}
	if (!aoLinearSampler) {
		D3D11_SAMPLER_DESC sd{};
		sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sd.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(globals::d3d::device->CreateSamplerState(&sd, aoLinearSampler.put()))) {
			ReleaseAOFilter();
			SwitchOffAO("could not create its sampler");
			return false;
		}
	}
	aoWidth = a_width;
	aoHeight = a_height;
	aoD3D11Bytes = bytes;
	d3d11Bytes += aoD3D11Bytes;
	aoHistoryValid = false;
	return true;
}

bool RayTracing::PrepareAO(RayTracingRT::FrameInput& a_input, const float a_traceRect[4], bool a_interior)
{
	if (!AOOn() || !backend.AOAvailable())
		return false;
	if (a_interior && !AOInteriorsOn())  // (46h)
		return false;
	// Only with a consumer: Screen Space GI's AO is what RTAO replaces / joins (its composite reads it).
	const auto& ssgi = globals::features::screenSpaceGI;
	if (!ssgi.loaded || !ssgi.settings.Enabled)
		return false;
	const std::uint32_t width = std::max(a_input.renderWidth, 1u), height = std::max(a_input.renderHeight, 1u);
	if (!backend.EnsureAOTextures(width, height)) {
		if (!backend.AOFault().empty())
			SwitchOffAO(backend.AOFault());
		return false;
	}
	if (!EnsureAOFilter(width, height))
		return false;
	auto& c = aoInput.constants;
	std::memcpy(c.invViewProj, a_input.constants.invViewProj, sizeof(c.invViewProj));
	std::copy_n(a_input.constants.renderSize, 4, c.renderSize);
	std::copy_n(a_traceRect, 4, c.rect);
	std::uint32_t flags = 0;
	if (!skinDraws.empty()) {
		flags |= kAOTraceCharacters;
		if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->IsInFirstPerson())
			flags |= kAOTraceFirstPerson;
	}
	c.params[0] = static_cast<float>(frameIndex % 1024);
	std::memcpy(&c.params[1], &flags, sizeof(float));
	c.params[2] = std::clamp(settings.AORayLength, kAORayLengthMin, kAORayLengthMax);
	c.params[3] = 0.0f;
	const float distance = std::clamp(settings.AODistance, 1000.0f, 8192.0f);
	c.fade[0] = distance * kAOFadeStartShare;
	c.fade[1] = distance;
	if (a_interior) {
		// (46h) Indoors the whole interior is in the TLAS (no terrain / grass / LOD to hand back to SSGI): no distance fade.
		// The weight starts to fall only beyond the camera's farthest corner of the interior's box, so it is 1 for every
		// surface inside it (the trace's rect test still leaves whatever lies outside the box to SSGI's AO).
		const auto& sc = scene.GetStats();
		const float4 camera = globals::game::frameBufferCached.GetCameraPosAdjust(0);
		const float cam[3] = { camera.x, camera.y, camera.z };
		float farthest = 0.0f;
		for (int axis = 0; axis < 3; ++axis) {
			const float d = std::max(std::abs(sc.interiorBounds[axis] - cam[axis]), std::abs(sc.interiorBounds[axis + 3] - cam[axis]));
			farthest += d * d;
		}
		farthest = std::sqrt(farthest);
		if (!std::isfinite(farthest) || farthest < 1.0f)
			farthest = 65536.0f;  // no box yet: no fade within any interior
		c.fade[0] = farthest + 64.0f;
		c.fade[1] = farthest + 1024.0f;
	}
	c.fade[2] = kRayBiasConstant;
	c.fade[3] = kRayBiasPerUnit;
	c.extra[0] = kCharacterSelfShadowOffset;
	c.extra[1] = c.extra[2] = c.extra[3] = 0.0f;
	std::copy_n(a_input.constants.spheres, 8, c.spheres);
	// Leaves / cut-outs the per-kind menu traces block AO rays with their cut-out (bounded candidates per ray).
	aoInput.alpha = backend.AOAlphaAvailable();
	a_input.ao = &aoInput;
	return true;
}

void RayTracing::DrawAmbientOcclusion()
{
	if (!aoThisFrame)
		return;
	aoThisFrame = false;
	auto& ssgi = globals::features::screenSpaceGI;
	// SSGI did not run this frame (switched off since the trace, shaders recompiling): its own (cleared) AO stays; the
	// pending second-fence wait is taken before the next trace (Backend::ExecuteFrame / RunFrame).
	if (!ssgi.loaded || !ssgi.settings.Enabled || !ssgi.ranThisFrame || !ssgi.GetOwnAoSRV() || !aoOut.texture || !backend.AORawSRV())
		return;
	const std::int64_t start = Qpc();
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();

	// 1. The D3D11 queue waits for the RTAO trace (second fence, on the GPU). Timed: the stall it causes.
	timers->Begin(Util::GpuBucket::RayTracingAOSync);
	backend.WaitForAO();
	timers->End(Util::GpuBucket::RayTracingAOSync);
	// (46h) An interior frame with RTAO only left the trace's wait for here: the trace list ran before the AO list on the
	// same queue, so its fence value is reached by now (no stall); taken so it does not wait for the next frame.
	backend.WaitForTrace();
	if (backend.HasFailed()) {
		Disable(Status::Failed, backend.LastError());
		return;
	}

	auto* down = GetShader(aoDownCS, L"Data\\Shaders\\ScreenSpaceRayTracing\\ssrt_conf_downsample.hlsl", "ssrt_conf_downsample (RTAO)");
	auto* blurH = GetShader(aoBlurHCS, L"Data\\Shaders\\ScreenSpaceRayTracing\\ssrt_conf_blur.hlsl", "ssrt_conf_blur (RTAO)");
	auto* blurV = GetShaderDefined(aoBlurVCS, L"Data\\Shaders\\ScreenSpaceRayTracing\\ssrt_conf_blur.hlsl", "ssrt_conf_blur vertical (RTAO)", "SSRT_CONF_BLUR_VERTICAL");
	auto* up = GetShader(aoUpCS, L"Data\\Shaders\\ScreenSpaceRayTracing\\ssrt_conf_upsample.hlsl", "ssrt_conf_upsample (RTAO)");
	auto* temporal = GetShader(aoTemporalCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS");
	auto* composite = GetShader(aoCompositeCS, L"Data\\Shaders\\RayTracing\\RTAOCompositeCS.hlsl", "RTAOCompositeCS");
	if (!down || !blurH || !blurV || !up || !temporal || !composite) {
		SwitchOffAO("a filter shader did not compile (see the log)");
		return;
	}

	auto* renderer = globals::game::renderer;
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR];
	if (!depth.depthSRV || !normal.SRV || !motion.SRV)
		return;

	const std::uint32_t width = aoWidth, height = aoHeight;
	const UINT groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;
	const UINT loX = ((width + 1) / 2 + 7) / 8, loY = ((height + 1) / 2 + 7) / 8;
	const bool historyValid = settings.AOTemporal && aoHistoryValid && aoHistoryFrame + 1 == globals::state->frameCount;
	const std::uint32_t read = aoHistoryIndex, write = aoHistoryIndex ^ 1u;
	ID3D11ShaderResourceView* contact = ssgi.contactAoThisFrame;
	const bool debugOverlay = (aoDebugView == AODebugView::Raw || aoDebugView == AODebugView::Filtered || aoDebugView == AODebugView::Split) && debugColorUAV;

	AOCompositeCB cb{};
	cb.Size[0] = width;
	cb.Size[1] = height;
	// (46h) The mode of this frame's trace: AOMode outdoors, AOModeInterior indoors.
	cb.Size[2] = (contact ? kAOFlagContact : 0u) | (aoModeThisFrame == static_cast<int>(AOModeKind::Darker) ? kAOFlagDarker : 0u) |
	             (historyValid ? kAOFlagHistory : 0u) | (aoDebugView == AODebugView::Split ? kAOFlagSplit : 0u);
	cb.Size[3] = debugOverlay ? static_cast<std::uint32_t>(aoDebugView) : 0u;
	cb.Params[0] = std::clamp(settings.AOStrength, 0.0f, 2.0f);
	cb.Params[1] = kAOHistoryWeight;
	cb.Params[2] = kAOClampSlack;
	{
		// (46h) The RT weight: the transition's fade (one global hand-off) times RTAO's own fade-in after a gap.
		const std::uint32_t now = globals::state->frameCount;
		if (aoLastCompositeFrame == UINT32_MAX || now - aoLastCompositeFrame > kAOGapFrames)
			aoRampStart = Qpc();
		aoLastCompositeFrame = now;
		aoRamp = static_cast<float>(std::clamp(static_cast<double>(Qpc() - aoRampStart) * QpcMs() / kAOFadeInMs, 0.0, 1.0));
	}
	cb.Params[3] = fade * aoRamp;
	aoCB->Update(cb);

	ID3D11ShaderResourceView* nullSrvs[6]{};
	ID3D11UnorderedAccessView* nullUavs[3]{};
	auto clear = [&]() {
		context->CSSetShaderResources(0, 6, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	};
	clear();
	timers->Begin(Util::GpuBucket::RayTracingAO);
	// 2. The 1/4-resolution bilateral filter of this frame's rays (the SSRT confidence filter's passes, unchanged):
	//    depth- and normal-aware 2x2 average, separable 15-tap joint bilateral blur, joint bilateral upsample.
	//    Spatial, same frame: nothing of NRD, no history (hard constraint #3).
	{
		ID3D11ShaderResourceView* srvs[3] = { backend.AORawSRV(), depth.depthSRV, normal.SRV };
		ID3D11UnorderedAccessView* uavs[3] = { aoLo.uav.get(), aoLoDepth.uav.get(), aoLoNormal.uav.get() };
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->CSSetShader(down, nullptr, 0);
		context->Dispatch(loX, loY, 1);
		clear();
	}
	auto blurPass = [&](ID3D11ComputeShader* a_shader, DenoiseTexture& a_source, DenoiseTexture& a_target) {
		ID3D11ShaderResourceView* srvs[4] = { a_source.srv.get(), aoLoDepth.srv.get(), nullptr, aoLoNormal.srv.get() };
		ID3D11UnorderedAccessView* uav = a_target.uav.get();
		context->CSSetShaderResources(0, 4, srvs);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(a_shader, nullptr, 0);
		context->Dispatch(loX, loY, 1);
		clear();
	};
	blurPass(blurH, aoLo, aoLoBlur);
	blurPass(blurV, aoLoBlur, aoLo);
	{
		ID3D11ShaderResourceView* srvs[6] = { backend.AORawSRV(), depth.depthSRV, normal.SRV, aoLo.srv.get(), aoLoDepth.srv.get(), aoLoNormal.srv.get() };
		ID3D11UnorderedAccessView* uav = aoFiltered.uav.get();
		context->CSSetShaderResources(0, 6, srvs);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(up, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		clear();
	}
	// 3. Optional: the clamped history (after the spatial filter; the history is held inside this frame's filtered
	//    3x3 range plus a small slack). Temporal off: the filtered result is used as it is.
	DenoiseTexture* blended = &aoFiltered;
	ID3D11Buffer* cbs[1] = { aoCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	if (settings.AOTemporal) {
		ID3D11ShaderResourceView* srvs[3] = { aoFiltered.srv.get(), aoHistory[read].srv.get(), motion.SRV };
		ID3D11UnorderedAccessView* uav = aoHistory[write].uav.get();
		ID3D11SamplerState* sampler = aoLinearSampler.get();
		context->CSSetSamplers(0, 1, &sampler);
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(temporal, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		clear();
		ID3D11SamplerState* nullSampler = nullptr;
		context->CSSetSamplers(0, 1, &nullSampler);
		blended = &aoHistory[write];
		aoHistoryIndex = write;
		aoHistoryValid = true;
		aoHistoryFrame = globals::state->frameCount;
	} else {
		aoHistoryValid = false;
	}
	// 4. The mix with SSGI's AO into the texture its consumers read (DeferredComposite, SSRT) for this frame.
	{
		ID3D11ShaderResourceView* srvs[4] = { blended->srv.get(), backend.AORawSRV(), ssgi.GetOwnAoSRV(), contact };
		ID3D11UnorderedAccessView* uavs[2] = { aoOut.uav.get(), debugOverlay ? debugColorUAV.get() : nullptr };
		context->CSSetShaderResources(0, 4, srvs);
		context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		context->CSSetShader(composite, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		clear();
	}
	timers->End(Util::GpuBucket::RayTracingAO);
	ID3D11Buffer* nullCb[1]{};
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	ssgi.aoOverride = aoOut.srv.get();
	aoComposited = true;
	aoDebugDrawn = debugOverlay;
	aoCpuMs += static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
}

// ---------------------------------------------------------------------------------------------
// (46c) Interiors
// ---------------------------------------------------------------------------------------------

bool RayTracing::InteriorWanted() const
{
	if (!settings.Enabled || !backend.IsReady())
		return false;
	// (46h) The interior is captured for its point lights, or for RTAO indoors (or both: one capture, one trace).
	return (PointLightsOn() && InteriorsOn() && backend.PointAvailable()) || AOInteriorWanted();
}

bool RayTracing::AOInteriorWanted() const
{
	const auto& ssgi = globals::features::screenSpaceGI;
	return settings.Enabled && AOInteriorsOn() && backend.IsReady() && backend.AOAvailable() && ssgi.loaded && ssgi.settings.Enabled;
}

void RayTracing::SwitchOffInteriors(std::string a_reason)
{
	// Only the interior part: outdoors goes on. The page shows why; ticking the box again retries.
	// (46f) A runtime state, not the setting (the saved choice stays on and is tried again next session).
	interiorOffReason = std::move(a_reason);
	interiorAutoOff = true;
	scene.SetInteriorEnabled(false);  // the next Update drops the interior capture
	pointSelector.Clear();
	pointThisFrame = false;
	pointHistoryValid = false;
	interiorTracedLastFrame = false;
	skipCandidates.clear(); skipEntries.clear();
	UnbindPointLights();
	if (status == Status::Active && Util::IsInterior()) {
		status = Status::Inactive;
		statusReason = "interior";
	}
	logger::warn("[RayTracing] Interiors switched off: {}", interiorOffReason);
}

void RayTracing::RunInteriorFrame()
{
	// Indoors: no sun. The interior cell is captured (Scene, interior mode) and only the point lights are traced,
	// then denoised and their engine shadow lights' mask channels written (FinishFrame, pending.interior).
	interiorThisFrame = true;
	auto* renderer = globals::game::renderer;
	auto& mask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (!mask.texture || !mask.SRV || !depth.depthSRV) {
		status = Status::Inactive;
		statusReason = "shadow mask not available";
		return;
	}
	D3D11_TEXTURE2D_DESC desc{};
	mask.texture->GetDesc(&desc);
	ShrinkToRenderAlloc(desc);  // (46q) Size Buffers to Render Resolution
	if (!EnsureD3D11Resources(desc))
		return;
	std::string reason;
	if (!backend.EnsureTextures(desc.Width, desc.Height, reason)) {
		Disable(Status::Failed, reason);
		return;
	}

	// Entry time (the cell, or the loading screen closing after it): to the first traced frame.
	auto* tes = RE::TES::GetSingleton();
	const RE::TESObjectCELL* cell = tes ? tes->interiorCell : nullptr;
	if (cell != interiorCell) {
		interiorCell = cell;
		interiorEntryQpc = Qpc();
		interiorReadyPending = true;
		interiorSceneLogPending = true;
		interiorProgressQpc = Qpc();
	}
	if (phase == Phase::WarmUp && warmUpStart > interiorEntryQpc)
		interiorEntryQpc = warmUpStart;

	{
		const auto& stats = backend.GetStats();
		overVramBudget = stats.blasBytes + stats.scratchBytes > kAccelerationBudgetBytes;
	}
	scene.Update(backend, overVramBudget ? 0.0f : (blackScreen ? kBlackCaptureBudgetMs : kCaptureBudgetMs), globals::state->frameCount);

	const auto& fb = globals::game::frameBufferCached;
	const float4 camera = fb.GetCameraPosAdjust(0);
	const double camX = camera.x, camY = camera.y, camZ = camera.z;
	float world[4];
	if (!scene.HasCoverage() || !scene.GetLoadedRect(world, true)) {
		// (46d) The interior's BLASes are built here: outdoors the trace submission (ExecuteFrame) builds the
		// queue even before any cell is covered, but indoors nothing is traced until the interior is covered,
		// so without this its queued builds never ran (46c: "capturing the interior" for ever). Our own queue,
		// nothing waits on the CPU; a larger cap while warming up after the loading screen.
		backend.BuildOnly(blackScreen ? kLoadingBuildTriangles : kBuildBudget * 2, blackScreen ? kLoadingMaxBuilds : 64u, blackScreen ? kBlackBuildCpuMs : 2.0f);
		backend.Tick();
		status = Status::Waiting;
		statusReason = "capturing the interior";
		LogInteriorProgress(false);
		return;
	}
	LogInteriorProgress(true);
	const float traceRect[4] = { static_cast<float>(world[0] - camX), static_cast<float>(world[1] - camY), static_cast<float>(world[2] - camX),
		static_cast<float>(world[3] - camY) };
	const bool validate = frameIndex % 30 == 0;
	scene.CollectInstances(instances, validate);

	// Characters (as outdoors, within the interior's box).
	const std::int64_t charactersStart = Qpc();
	if (backend.HasPrepared())
		backend.AbandonPrepared();
	skinDraws.clear();
	skinBones.clear();
	if (settings.Characters && backend.SkinAvailable()) {
		characters.Update(backend, world, blackScreen ? 6.0 : 1.0, blackScreen ? 32u : 6u);
		characters.CollectDraws(skinDraws, skinBones);
	}
	cpuLast.characters = static_cast<float>(static_cast<double>(Qpc() - charactersStart) * QpcMs());

	const float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::uint32_t width = std::min<std::uint32_t>(static_cast<std::uint32_t>(renderSize.x), desc.Width);
	const std::uint32_t height = std::min<std::uint32_t>(static_cast<std::uint32_t>(renderSize.y), desc.Height);
	if (width == 0 || height == 0)
		return;

	const auto& invViewProj = fb.GetCameraViewProjInverse(0);
	RayTracingRT::FrameInput input;
	std::memcpy(input.constants.invViewProj, &invViewProj, sizeof(input.constants.invViewProj));
	input.constants.renderSize[0] = static_cast<float>(width);
	input.constants.renderSize[1] = static_cast<float>(height);
	input.constants.renderSize[2] = 1.0f / static_cast<float>(width);
	input.constants.renderSize[3] = 1.0f / static_cast<float>(height);
	std::copy_n(traceRect, 4, input.constants.rect);
	if (!skinDraws.empty()) {
		const double cam[3] = { camX, camY, camZ };
		characters.GetBounds(cam, &input.constants.spheres[0], &input.constants.spheres[4]);
	}
	input.renderWidth = width;
	input.renderHeight = height;
	input.cameraX = camX;
	input.cameraY = camY;
	input.cameraZ = camZ;
	input.instances = &instances;
	input.buildTriangleBudget = kBuildBudget;
	input.validate = validate;
	input.skinDraws = &skinDraws;
	input.skinBones = &skinBones;
	input.deferWait = true;
	input.sun = false;
	input.buildCpuMs = blackScreen ? kBlackBuildCpuMs : 0.0f;  // (46g)
	input.skinUploadCpuMs = blackScreen ? kBlackSkinUploadCpuMs : 0.0f;
	// (46h) The point lights only with their own Indoors Too (the interior may be captured for RTAO alone).
	if (PointLightsOn() && InteriorsOn()) {
		const std::int64_t pointStart = Qpc();
		try {
			if (!PreparePointLights(input, traceRect, camX, camY, camZ, 0.0f, true))
				input.pointLights = nullptr;
		} catch (const std::exception& e) {
			input.pointLights = nullptr;
			SwitchOffPointLights(std::format("exception in the light selection: {}", e.what()));
		}
		pointCpuMs = static_cast<float>(static_cast<double>(Qpc() - pointStart) * QpcMs());
	}
	// (46h) RTAO indoors: its own list right behind the point lights, its own fence (as outdoors). Traced whether or not a
	// light is in view (else the AO would switch between RT and SSGI as lights come and go).
	{
		const std::int64_t aoStart = Qpc();
		try {
			if (!PrepareAO(input, traceRect, true))
				input.ao = nullptr;
		} catch (const std::exception& e) {
			input.ao = nullptr;
			SwitchOffAO(std::format("exception: {}", e.what()));
		}
		aoCpuMs = static_cast<float>(static_cast<double>(Qpc() - aoStart) * QpcMs());
	}
	if (!input.pointLights && !input.ao) {
		// Nothing to trace (no shadow-casting light in view, no RTAO): the queued builds go on, nothing waits.
		backend.BuildOnly(kBuildBudget, 64u, blackScreen ? kBlackBuildCpuMs : 2.0f);
		backend.Tick();
		status = Status::Active;
		statusReason = "interior, no shadow-casting light in view";
		++frameIndex;
		return;
	}

	// The constants PrepareCS and the point-light denoiser read (Common.hlsli).
	CombineCB cb{};
	std::memcpy(cb.InvViewProj, &invViewProj, sizeof(cb.InvViewProj));
	std::copy_n(traceRect, 4, cb.Rect);
	if (prevCameraValid) {
		std::copy_n(prevViewProj, 16, cb.PrevViewProj);
		cb.CameraDelta[0] = static_cast<float>(camX - prevCamera[0]);
		cb.CameraDelta[1] = static_cast<float>(camY - prevCamera[1]);
		cb.CameraDelta[2] = static_cast<float>(camZ - prevCamera[2]);
	} else {
		std::memcpy(cb.PrevViewProj, &fb.GetCameraViewProj(0), sizeof(cb.PrevViewProj));
	}
	const bool debug = debugView >= DebugView::PLCandidates && debugView <= DebugView::PLDenoised;
	cb.Size[0] = width;
	cb.Size[1] = height;
	cb.Size[2] = static_cast<std::uint32_t>(debugView);
	cb.Size[3] = frameIndex;
	combineCB->Update(cb);

	auto* prepare = GetShader(prepareCS, L"Data\\Shaders\\RayTracing\\PrepareCS.hlsl", "PrepareCS");
	if (!prepare) {
		Disable(Status::Failed, "a Ray Tracing shader did not compile (see the log)");
		return;
	}
	GpuBucketScope gpuTimer(Util::GpuBucket::RayTracing);
	auto* context = globals::d3d::context;
	const std::int64_t d3d11Start = Qpc();
	// Alpha copies (window masks) before the trace: the D3D12 queue waits for this.
	if (scene.AlphaWanted() && backend.AlphaAvailable()) {
		const bool rgba = backend.AlphaCopiesRgba();
		if (alphaMaskCS && alphaMaskRgba != rgba)
			alphaMaskCS = nullptr;
		if (!alphaMaskCS) {
			std::vector<std::pair<const char*, const char*>> defines;
			if (rgba)
				defines.emplace_back("ALPHA_RGBA", "");
			alphaMaskCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RayTracing\\AlphaMaskCS.hlsl", defines, "cs_5_0")));
			alphaMaskRgba = rgba;
		}
		if (alphaMaskCS)
			backend.RecordAlphaCopies(alphaMaskCS.get(), kAlphaCopiesPerFrame);
	}
	{
		ID3D11Buffer* cbs[1] = { combineCB->CB() };
		ID3D11ShaderResourceView* srvs[2] = { depth.depthSRV, mask.SRV };
		ID3D11UnorderedAccessView* uavs[1] = { backend.DepthMaskUAV() };
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 2, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetShader(prepare, nullptr, 0);
		context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrvs[2]{};
		ID3D11UnorderedAccessView* nullUavs[1]{};
		context->CSSetShaderResources(0, 2, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	const std::int64_t executeStart = Qpc();
	cpuLast.d3d11 = static_cast<float>(static_cast<double>(executeStart - d3d11Start) * QpcMs());
	const bool traced = backend.ExecuteFrame(input);
	cpuLast.execute = static_cast<float>(static_cast<double>(Qpc() - executeStart) * QpcMs());
	if (!traced && backend.HasFailed()) {
		Disable(Status::Failed, backend.LastError());
		return;
	}
	if (!traced) {
		pointHistoryValid = false;
		++frameIndex;
		return;
	}
	pointThisFrame = input.pointLights != nullptr && backend.GetStats().pointTracedLastFrame;
	pointSelector.Publish(pointThisFrame);
	if (!pointThisFrame)
		pointK = 0;
	// (46h) RTAO went out right behind it (second fence): DrawAmbientOcclusion uses it after Screen Space GI.
	aoThisFrame = input.ao != nullptr && backend.GetStats().aoTracedLastFrame;
	aoInteriorThisFrame = aoThisFrame;
	aoModeThisFrame = aoThisFrame ? std::clamp(settings.AOModeInterior, 0, static_cast<int>(AOModeKind::Count) - 1) : -1;
	if (!aoThisFrame)
		aoHistoryValid = false;

	pending.active = true;
	pending.interior = true;
	pending.traceSync = pointThisFrame;  // (46h) RTAO only: no rt_sync (DrawAmbientOcclusion's wait covers the trace)
	pending.splitSun = false;  // (46m) no sun trace indoors
	std::memcpy(pending.cbBytes.data(), &cb, sizeof(cb));
	pending.combineFlags = 0;
	pending.width = width;
	pending.height = height;
	pending.writeSSS = pending.penumbra = pending.spatial = pending.half = pending.sigma = pending.sssNonRT = false;
	pending.debug = debug;
	pending.camera[0] = camX;
	pending.camera[1] = camY;
	pending.camera[2] = camZ;
	std::memcpy(pending.viewProj, &fb.GetCameraViewProj(0), sizeof(pending.viewProj));
	std::memcpy(pending.viewProjUnjittered, &fb.GetCameraViewProjUnjittered(0), sizeof(pending.viewProjUnjittered));

	activeThisFrame = true;
	status = Status::Active;
	statusReason = pointThisFrame ? (aoThisFrame ? "interior: point lights + RTAO" : "interior: point lights") : (aoThisFrame ? "interior: RTAO" : "interior");
	++frameIndex;
	if (interiorReadyPending) {
		interiorReadyPending = false;
		interiorReadyMs = static_cast<float>(static_cast<double>(Qpc() - interiorEntryQpc) * QpcMs());
		const auto& sc = scene.GetStats();
		const auto& st = backend.GetStats();
		std::uint32_t oneSided = 0, twoSided = 0;  // (46n)
		scene.CountChunkSides(oneSided, twoSided, true);
		logger::info("[RayTracing] Interior ready {:.0f} ms after entering: {} shapes, {:.2f} M triangles, {} BLAS, {:.0f} MB, capture {:.1f} ms CPU, {} window masks; "
					 "box {:.0f} x {:.0f} x {:.0f}; cell chunks {} one-sided, {} two-sided (never mixed since 46n)",
			interiorReadyMs, sc.interiorShapes, static_cast<double>(sc.interiorTriangles) / 1e6, st.blasCount,
			static_cast<double>(st.blasBytes + st.scratchBytes + st.tlasBytes) / (1024.0 * 1024.0), sc.interiorGatherMs, sc.windowMasks,
			sc.interiorBounds[3] - sc.interiorBounds[0], sc.interiorBounds[4] - sc.interiorBounds[1], sc.interiorBounds[5] - sc.interiorBounds[2], oneSided,
			twoSided);
		// (46n) The beams' blocked share, read back from 60 to 300 frames from now.
		beamShares.clear();
		beamShareFrom = globals::state->frameCount + 60;
		beamShareUntil = globals::state->frameCount + 300;
		beamSharePending = true;
	}
}

void RayTracing::LogInteriorProgress(bool a_covered)
{
	// (46d) The interior's capture in the log: every 2 s until it is covered, then one "ready" line.
	const auto& sc = scene.GetStats();
	const auto& st = backend.GetStats();
	if (a_covered) {
		if (!interiorSceneLogPending)
			return;
		interiorSceneLogPending = false;
		logger::info("[RayTracing] Interior scene ready {:.0f} ms after entering: {} shapes, {:.2f} M triangles, {} BLAS built ({} queued), {:.0f} MB, "
					 "box {:.0f} x {:.0f} x {:.0f}",
			static_cast<double>(Qpc() - interiorEntryQpc) * QpcMs(), sc.interiorShapes, static_cast<double>(sc.interiorTriangles) / 1e6, st.blasCount,
			st.blasQueued, static_cast<double>(st.blasBytes + st.scratchBytes + st.tlasBytes) / (1024.0 * 1024.0), sc.interiorBounds[3] - sc.interiorBounds[0],
			sc.interiorBounds[4] - sc.interiorBounds[1], sc.interiorBounds[5] - sc.interiorBounds[2]);
		return;
	}
	const std::int64_t now = Qpc();
	if (static_cast<double>(now - interiorProgressQpc) * QpcMs() < 2000.0)
		return;
	interiorProgressQpc = now;
	auto* tes = RE::TES::GetSingleton();
	const auto* cell = tes ? tes->interiorCell : nullptr;
	logger::info("[RayTracing] Interior capture: {:.0f} ms after entering; cell {} ({}), scene interior {}, cells {} ({} gathering, {} complete), {} shapes, "
				 "{} BLAS built, {} queued, phase {}; pools: {}",
		static_cast<double>(now - interiorEntryQpc) * QpcMs(), cell ? std::format("{:08X}", cell->GetFormID()) : std::string("none"),
		cell ? (cell->IsAttached() ? "attached" : "not attached") : "-", scene.InteriorEnabled() ? "on" : "off", sc.cellsTracked, sc.cellsGathering,
		sc.cellsComplete, sc.interiorShapes, st.blasCount, st.blasQueued, static_cast<int>(phase), backend.PoolSummary());
}

void RayTracing::WritePointMask(bool a_traced)
{
	pointMaskDone = true;
	auto* renderer = globals::game::renderer;
	auto& mask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	if (!mask.texture || !mask.SRV || !depth.depthSRV || !pointMaskCB || !combineCB)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	mask.texture->GetDesc(&desc);
	ShrinkToRenderAlloc(desc);  // (46q) Size Buffers to Render Resolution
	if (!maskTemp || desc.Width != maskDesc.Width || desc.Height != maskDesc.Height || desc.Format != maskDesc.Format) {
		if (!EnsureD3D11Resources(desc))
			return;
	}
	const bool rt = a_traced && pointThisFrame && pointDenoised && pointVis[0].texture;
	PointMaskCB cb{};
	std::uint32_t written = 0;
	if (rt) {
		for (const auto& e : pointSelector.MaskEntries()) {
			auto& ch = cb.Channels[std::min<std::uint32_t>(e.channel, 3)];
			std::copy_n(e.pos, 3, ch.Pos);
			ch.Pos[3] = static_cast<float>(e.source);
			std::copy_n(e.dir, 3, ch.Dir);
			ch.Dir[3] = e.falloff;
			std::copy_n(e.right, 3, ch.Right);
			ch.Right[3] = e.semiWidth;
			std::copy_n(e.up, 3, ch.Up);
			ch.Up[3] = e.semiHeight;
			ch.Flags[0] |= kMaskWrite | (e.spot ? kMaskSpot : 0u);
		}
	}
	const float4 cameraNow = globals::game::frameBufferCached.GetCameraPosAdjust(0);
	for (const auto& skipped : skippedMaps) {
		auto& ch = cb.Channels[std::min<std::uint32_t>(skipped.channel, 3)];
		// (46e) No traced entry for this channel this frame: its cone from last frame's (else a skipped spot light, a
		// window beam, would light the whole screen for this frame).
		if (!(ch.Flags[0] & kMaskWrite) && skipped.hasEntry) {
			const auto& e = skipped.entry;
			ch.Pos[0] = static_cast<float>(e.world[0] - cameraNow.x);
			ch.Pos[1] = static_cast<float>(e.world[1] - cameraNow.y);
			ch.Pos[2] = static_cast<float>(e.world[2] - cameraNow.z);
			ch.Pos[3] = 0.0f;  // no traced result
			std::copy_n(e.dir, 3, ch.Dir);
			ch.Dir[3] = e.falloff;
			std::copy_n(e.right, 3, ch.Right);
			ch.Right[3] = e.semiWidth;
			std::copy_n(e.up, 3, ch.Up);
			ch.Up[3] = e.semiHeight;
			ch.Flags[0] |= e.spot ? kMaskSpot : 0u;
		}
		ch.Flags[0] |= kMaskWrite | kMaskSkipped;
	}
	for (const auto& ch : cb.Channels)
		written += (ch.Flags[0] & kMaskWrite) ? 1 : 0;
	if (!skippedMaps.empty() && !rt)
		++skipFallbackFrames;
	if (written == 0)
		return;
	auto* shader = GetShader(pointMaskCS, L"Data\\Shaders\\RayTracing\\PointMaskCS.hlsl", "PointMaskCS");
	if (!shader) {
		if (!skippedMaps.empty())
			logger::error("[RayTracing] PointMaskCS did not compile: the shadow maps left out this frame cannot be replaced");
		settings.PointLightsSkipShadowMaps = false;
		SwitchOffInteriors("the shadow mask shader (PointMaskCS) did not compile");
		return;
	}
	pointMaskCB->Update(cb);

	// This frame's camera and render size (also when nothing was traced).
	const float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);
	const std::uint32_t width = std::min<std::uint32_t>(static_cast<std::uint32_t>(renderSize.x), desc.Width);
	const std::uint32_t height = std::min<std::uint32_t>(static_cast<std::uint32_t>(renderSize.y), desc.Height);
	if (width == 0 || height == 0)
		return;
	{
		CombineCB c{};
		std::memcpy(c.InvViewProj, &globals::game::frameBufferCached.GetCameraViewProjInverse(0), sizeof(c.InvViewProj));
		c.Size[0] = width;
		c.Size[1] = height;
		c.Size[2] = static_cast<std::uint32_t>(debugView);
		c.Size[3] = frameIndex;
		combineCB->Update(c);
	}
	auto* context = globals::d3d::context;
	const std::uint32_t latest = pointWrite ^ 1u;  // FinishPointLights flipped it after writing
	ID3D11Buffer* cbs[2] = { combineCB->CB(), pointMaskCB->CB() };
	ID3D11ShaderResourceView* srvs[4] = { mask.SRV, depth.depthSRV, rt ? pointVis[latest].srv.get() : nullptr, rt ? pointIdx[latest].srv.get() : nullptr };
	ID3D11UnorderedAccessView* uavs[1] = { maskTempUAV.get() };
	context->CSSetConstantBuffers(0, 2, cbs);
	context->CSSetShaderResources(0, 4, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[4]{};
	ID3D11UnorderedAccessView* nullUavs[1]{};
	ID3D11Buffer* nullCbs[2]{};
	context->CSSetShaderResources(0, 4, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 2, nullCbs);
	context->CSSetShader(nullptr, nullptr, 0);
	D3D11_BOX box{ 0, 0, 0, width, height, 1 };
	context->CopySubresourceRegion(mask.texture, 0, 0, 0, 0, maskTemp.get(), 0, &box);
	maskChannelsWritten = written;
}

// ---------------------------------------------------------------------------------------------
// Page
// ---------------------------------------------------------------------------------------------

namespace
{
	/// Red wrapped line: an automatic switch-off or a failure, with its reason.
	void RtErrorLine(const char* a_what, const std::string& a_reason)
	{
		ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.4f, 0.3f, 1.0f });
		ImGui::TextWrapped("%s: %s", a_what, a_reason.c_str());
		ImGui::PopStyleColor();
	}
}

void RayTracing::DrawSettings()
{
	// (46r) The "Ray Tracing" entry: the master switch and status, a one-line summary, Advanced (loading screens), the A/B
	// switches and the diagnostics. Sun Shadows, Point Light Shadows and Ambient Occlusion have their own entries in the
	// Ray Tracing category (each shows the switch and the status on top).
	ImGui::PushID("rtMainPage");  // tree-node open states of its own (other pages also have Advanced / Diagnostics)
	DrawMasterHeader();
	DrawDiagnostics();
	ImGui::PopID();
}

void RayTracing::DrawMasterHeader()
{
	ImGui::Checkbox("Ray Tracing", &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("On: sun shadows of buildings, rocks, furniture, doors, items and characters inside the loaded area are ray traced.");
		ImGui::Text("Trees, leaves, hair (unless changed under Sun Shadows > Advanced), grass and the terrain itself (hills) keep their normal shadows;");
		ImGui::Text("  the ground still receives traced shadows.");
		ImGui::Text("Screen Space Shadows are switched off where ray tracing is in charge.");
		ImGui::Text("Off: the game's shadows exactly as before (nothing of this feature runs).");
	}
	const char* statusText = "Off";
	ImVec4 color{ 0.7f, 0.7f, 0.7f, 1.0f };
	switch (status) {
	case Status::Off:
		statusText = settings.Enabled ? "Starting" : "Off";
		break;
	case Status::Unavailable:
		statusText = "Off, could not start (the game's shadows are used)";
		color = { 1.0f, 0.4f, 0.3f, 1.0f };
		break;
	case Status::Failed:
		statusText = "Off, stopped after an error (the game's shadows are used)";
		color = { 1.0f, 0.4f, 0.3f, 1.0f };
		break;
	case Status::Inactive:
		statusText = "Paused";
		break;
	case Status::Waiting:
		statusText = "Loading the scene";
		color = { 1.0f, 0.85f, 0.3f, 1.0f };
		break;
	case Status::Active:
		statusText = "Active";
		color = { 0.4f, 1.0f, 0.4f, 1.0f };
		break;
	}
	ImGui::TextColored(color, "Status: %s", statusText);
	if (!statusReason.empty()) {
		if (status == Status::Unavailable || status == Status::Failed) {
			// The reason is the whole point here (and can be long): its own wrapped line.
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::TextWrapped("Reason: %s", statusReason.c_str());
			ImGui::PopStyleColor();
		} else {
			ImGui::SameLine();
			ImGui::TextDisabled("(%s)", statusReason.c_str());
		}
	}
	if (status == Status::Unavailable || status == Status::Failed) {
		if (ImGui::Button("Try Again")) {
			backendTried = false;
			status = Status::Off;
			statusReason.clear();
		}
	}
	ImGui::Separator();
}

void RayTracing::DrawSunPage()
{
	ImGui::PushID("rtSunPage");
	DrawMasterHeader();
	if (ImGui::TreeNodeEx("Sun Shadows", ImGuiTreeNodeFlags_DefaultOpen)) {
		// Five tiers + Custom; picking Custom starts from the tier that was selected.
		settings.QualityLevel = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
		{
			const int before = settings.QualityLevel;
			if (ImGui::Combo("Quality", &settings.QualityLevel, kQualityNames, IM_ARRAYSIZE(kQualityNames)) &&
				settings.QualityLevel == static_cast<int>(Quality::Custom) && before != static_cast<int>(Quality::Custom))
				CopyTierToCustom(before);
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			if (settings.QualityLevel < static_cast<int>(Quality::Custom))
				ImGui::Text("%s", kTiers[settings.QualityLevel].tooltip);
			else
				ImGui::Text("Custom: the values under Custom Quality below (started from the tier you had picked).");
		}
		if (settings.QualityLevel == static_cast<int>(Quality::Custom) && ImGui::TreeNodeEx("Custom Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderFloat("Sun Softness", &settings.CustomSunCone, 0.1f, 2.0f, "%.2f deg");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How big the sun looks to the shadows: 0.27 = the real sun (sharp edges), larger = softer, wider shadow edges.");
			ImGui::SliderInt("Rays per Pixel", &settings.CustomBaseRays, 1, 4);
			ImGui::SliderInt("Extra Rays at Shadow Edges", &settings.CustomExtraRays, 0, 8);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra rays only where a shadow edge is (cleaner soft edges). 0 = none.");
			ImGui::Combo("Trace Resolution", &settings.CustomTraceResolution, kPatternNames, 3);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Full: every pixel traced every frame. Checkerboard: half of them, alternating each frame. Half: one pixel of each 2x2, rotating.");
				ImGui::Text("  The others are filled from their traced neighbours (and the denoiser's history averages the frames).");
			}
			ImGui::SliderFloat("Fall-off Start", &settings.CustomFalloffStart, 0.0f, 16384.0f, "%.0f units");
			ImGui::SliderFloat("Fall-off End", &settings.CustomFalloffEnd, 0.0f, 32768.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("From the start to the end distance the extra rays fade out and the denoiser blur shrinks to Far Blur Scale;");
				ImGui::Text("  beyond the end: 1 ray per pixel and the Far Quality pattern. 70 units = 1 m. End at or below the start = no fall-off.");
			}
			ImGui::Combo("Far Quality", &settings.CustomFarQuality, kPatternNames, 3);
			ImGui::SliderFloat("Far Blur Scale", &settings.CustomFarDenoiseScale, 0.0f, 1.0f, "%.2f");
			const char* npcModes[] = { "Full", "Reduced" };
			ImGui::Combo("Other Characters", &settings.CustomNpcQuality, npcModes, 2);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("The player always gets full quality. Reduced: other characters get no extra edge rays.");
			ImGui::SliderFloat("Denoiser Radius", &settings.CustomDenoiseRadius, 2.0f, 32.0f, "%.0f px");
			ImGui::SliderInt("Denoiser Taps per Side", &settings.CustomDenoiseTaps, 1, 8);
			ImGui::Checkbox("Denoiser at Half Resolution", &settings.CustomDenoiseHalf);
			ImGui::SliderInt("History Length", &settings.CustomHistory, 1, 32, "%d frames");
			ImGui::SliderFloat("History Clamp", &settings.CustomDenoiseClamp, 0.5f, 4.0f, "%.2f sigma");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How far the history may differ from this frame's neighbourhood: smaller = less ghosting, more grain.");
			ImGui::TreePop();
		}

		// Denoiser: Off / Penumbra / DLSS RR / NRD SIGMA (stored values 0 / 2 / 3 / 4).
		{
			static constexpr DenoiserMode kOrder[] = { DenoiserMode::Off, DenoiserMode::Penumbra, DenoiserMode::DLSSRR, DenoiserMode::Sigma };
			const char* denoisers[] = { "Off", "Penumbra", "DLSS RR", "NRD SIGMA" };
			settings.Denoiser = ValidDenoiser(settings.Denoiser);
			int denoiserIndex = 1;
			for (int i = 0; i < IM_ARRAYSIZE(kOrder); ++i)
				if (settings.Denoiser == static_cast<int>(kOrder[i]))
					denoiserIndex = i;
			const bool rrActive = UpscalingRRActive();
			if (ImGui::Combo("Denoiser", &denoiserIndex, denoisers, IM_ARRAYSIZE(denoisers))) {
				settings.Denoiser = static_cast<int>(kOrder[std::clamp(denoiserIndex, 0, IM_ARRAYSIZE(kOrder) - 1)]);
				OnDenoiserPicked();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How the grainy soft edges of the traced shadows are cleaned up.");
				ImGui::Text("  Off: only a small blur.");
				ImGui::Text("  Penumbra (default): blurs each soft edge as wide as it really is (wide under tall trees, none at");
				ImGui::Text("    contact shadows), then blends the last ~12 frames; extra rays only at shadow edges.");
				ImGui::Text("  DLSS RR: no cleanup of ours; DLSS Ray Reconstruction cleans up the raw noise.");
				ImGui::Text("    Picking it switches Ray Reconstruction on in Upscaling; picking any other one switches it off there.");
				ImGui::Text("    While Ray Reconstruction is on in Upscaling, the shadows always take this route.");
				ImGui::Text("  NRD SIGMA: NVIDIA's shadow denoiser (one ray per pixel, its own blur and history).");
			}
			auto& upscaling = globals::features::upscaling;
			if (rrActive && settings.Denoiser != static_cast<int>(DenoiserMode::DLSSRR)) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.5f, 0.85f, 1.0f, 1.0f });
				ImGui::TextWrapped("Following Upscaling: Ray Reconstruction is on, so the shadows use the DLSS RR route.");
				ImGui::PopStyleColor();
			} else if (settings.Denoiser == static_cast<int>(DenoiserMode::DLSSRR) && !rrActive) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.85f, 0.3f, 1.0f });
				if (!upscaling.loaded || !Upscaling::streamline.featureDLSS_RR)
					ImGui::TextWrapped("Ray Reconstruction is not available on this setup: the shadows stay grainy. Pick another denoiser.");
				else
					ImGui::TextWrapped("Ray Reconstruction is switched on but not running: set Upscaling to DLSS. Until then the shadows stay grainy.");
				ImGui::PopStyleColor();
			}
			if (settings.Denoiser == static_cast<int>(DenoiserMode::Sigma)) {
				ImGui::Indent();
				ImGui::SliderInt("SIGMA Stabilisation", &settings.SigmaStabilizedFrames, 0, 5, "%d frames");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Frames SIGMA's last stage averages. More = calmer shadows, but shadows of moving characters trail a little.");
					ImGui::Text("  0 = no trailing at all (a little more flicker).");
				}
				if (!sigmaFault.empty())
					ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "SIGMA could not start: %s", sigmaFault.c_str());
				ImGui::Unindent();
			}
		}
		if (!typedUavLoadR8)
			ImGui::TextDisabled("This GPU cannot rewrite Screen Space Shadows; they are kept as with Ray Tracing off.");
		if (!alphaOffReason.empty())
			RtErrorLine("Ray-traced foliage / cut-outs were switched off automatically", alphaOffReason);
		if (!charactersOffReason.empty())
			RtErrorLine("Characters was switched off automatically", charactersOffReason);

		// Advanced: per kind of object, ray traced or the shadow map + screen-space shadows.
		if (ImGui::TreeNodeEx("Advanced")) {
			ImGui::TextWrapped("What the ray tracer takes over, per kind of object. \"Shadow map\" = the game's own shadows and Screen Space "
							   "Shadows, exactly as with Ray Tracing off. Changing a row re-captures the scene over the next frames.");
			const char* twoModes[] = { "Shadow map", "Ray traced" };
			auto row = [&](const char* a_label, bool& a_value, const char* a_tip1, const char* a_tip2) {
				int value = a_value ? 1 : 0;
				if (ImGui::Combo(a_label, &value, twoModes, 2))
					a_value = value == 1;
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("%s", a_tip1);
					if (a_tip2)
						ImGui::Text("%s", a_tip2);
				}
			};
			row("Buildings", settings.Buildings, "Statics, furniture, containers, rocks, walls (default: Ray traced).",
				"Also big one-sided shapes whose cut-out only trims their edges (roof tiles, plank ends), taken as solid.");
			row("Objects", settings.Objects, "Doors, activators, movable objects, lights and loose items (default: Ray traced).", nullptr);
			row("Trees", settings.Trees, "Trunks and branches of trees and plants (default: Shadow map).",
				"Their leaves are Foliage.");
			{
				const char* foliageModes[] = { "Shadow map", "Ray traced", "Both" };
				settings.Foliage = std::clamp(settings.Foliage, 0, 2);
				ImGui::Combo("Foliage", &settings.Foliage, foliageModes, 3);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Leaves, bushes and plants with a cut-out (default: Shadow map).");
					ImGui::Text("  Shadow map: the leaves keep the game's shadows and their screen-space contact shadows (they sway with the wind).");
					ImGui::Text("  Ray traced: traced with their cut-out instead (sharper, but they do not sway in the shadow).");
					ImGui::Text("  Both: traced AND kept in the shadow map; the darker of both wins (fullest shadow).");
				}
			}
			row("Cut-outs", settings.Cutouts, "Fences, grilles, banners and other non-plant cut-outs (default: Shadow map).",
				"Ray traced: traced with their cut-out (the holes stay lit).");
			row("Characters", settings.Characters, "The player and nearby NPCs and creatures: body, clothes, armour, weapons (default: Ray traced).",
				"Up to 16 characters within 40 m inside the ray-traced area, posed every frame.");
			{
				ImGui::Indent();
				ImGui::BeginDisabled(!settings.Characters);
				row("Physics parts", settings.PhysicsParts, "Capes, skirts and other HDT-SMP / HDT-PE parts, not hair (default: Ray traced).", nullptr);
				row("Hair", settings.Hair, "Hair and wigs (default: Shadow map: the hair shader's own shadowing looks better than solid cards).", nullptr);
				ImGui::Checkbox("Player in first person", &settings.FirstPersonPlayer);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Off (default): in first person the player keeps the shadow-map shadow (camera mods can hide the arms");
					ImGui::Text("  from a traced shadow). On: the player is ray traced in first person too.");
				}
				ImGui::EndDisabled();
				ImGui::Unindent();
			}
			ImGui::Separator();
			ImGui::SliderFloat("RT Caster Contact Length", &settings.RtContactScale, 0.0f, 32.0f, "%.1f x ray offset");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How far (along the sun ray) a ray-traced object may still cast a small screen-space contact shadow at its base,");
				ImGui::Text("  where the ray tracer cannot resolve it. Default 8 = about 16 units near the camera. 0 = none.");
				ImGui::Text("  Larger = softer bases, but thin posts get fatter. Everything the ray tracer does not see keeps its contact shadows.");
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void RayTracing::DrawPointLightPage()
{
	ImGui::PushID("rtPointPage");
	DrawMasterHeader();
	if (ImGui::TreeNodeEx("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Ray-Traced Point-Light Shadows", &settings.PointLights);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Braziers, campfires, torches, candles, lanterns and the like cast ray-traced shadows (outdoors inside the traced area, and indoors).");
			ImGui::Text("  Window fill / glow lights and fill lights are left as they are.");
			ImGui::Text("Off: point lights exactly as before. Needs Ray Tracing on.");
		}
		ImGui::BeginDisabled(!settings.PointLights);
		ImGui::SliderFloat("Light Size", &settings.PointLightsSize, 0.0f, 32.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Radius of the flame the rays aim at: larger = softer shadow edges (a little grainier before the clean-up). 0 = hard edges.");
		ImGui::Checkbox("Player Lights Cast RT Shadows", &settings.PointLightsPlayer);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): the torch in your hand, Quick Light's light and light spells on you cast ray-traced shadows.");
			ImGui::Text("  Off: they keep the old shadows.");
		}
		ImGui::Checkbox("Carrier Casts Shadows from Its Own Light", &settings.CarrierShadows);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): someone holding a torch (an NPC, or you in third person) casts a body shadow from it on the ground and");
			ImGui::Text("  walls behind them; only the hand and forearm near the flame are ignored (Advanced > Carrier Clearance).");
			ImGui::Text("  In first person your body never blocks your own light. Off: the carrier never blocks its own light.");
		}

		ImGui::SeparatorText("Indoors");
		ImGui::Checkbox("Indoors Too", &settings.PointLightsInteriors);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): inside houses, inns, castles and dungeons the room itself (walls, furniture, clutter, people) is captured");
			ImGui::Text("  when you come in (behind the loading screen) and the lights cast ray-traced shadows there too.");
			ImGui::Text("Off: indoors the lights' shadows are exactly as before (RT Ambient Occlusion has its own Indoors Too).");
		}
		ImGui::BeginDisabled(!settings.PointLightsInteriors);
		ImGui::Checkbox("Window Light Beams", &settings.PointLightsWindowBeams);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Window Shadows Ultimate's beams of sunlight through the windows (and the game's other spot lights indoors):");
			ImGui::Text("  their shadow is ray traced, the window frames give the pattern on the floor. Off: they keep the game's shadow maps.");
		}
		ImGui::SliderFloat("Window Beam Softness", &settings.PointLightsBeamSoftness, 0.0f, 3.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How wide the sun looks through the window (degrees, like the sun's own size): soft edges on the beam's window pattern,");
			ImGui::Text("  growing with the distance to the window frame. 0 = hard, grainy pattern.");
		}
		ImGui::EndDisabled();
		if (!interiorOffReason.empty()) {
			RtErrorLine("Indoors was switched off automatically", interiorOffReason);
			// The saved setting stays on (the next session tries again); Retry tries now.
			if (interiorAutoOff && ImGui::SmallButton("Retry Indoors"))
				interiorRequested = false;
		}
		ImGui::EndDisabled();
		if (!pointOffReason.empty()) {
			RtErrorLine("Point-light shadows were switched off automatically", pointOffReason);
			// The saved setting stays on (the next session tries again); Retry tries now.
			if (pointAutoOff && ImGui::SmallButton("Retry Point Lights"))
				pointRequested = false;
		}

		if (ImGui::TreeNodeEx("Advanced")) {
			ImGui::BeginDisabled(!settings.PointLights);
			{
				static constexpr int kKs[3] = { 1, 2, 4 };
				const char* kNames[3] = { "1", "2", "4" };
				int index = settings.PointLightsPerPixel >= 4 ? 2 : (settings.PointLightsPerPixel >= 2 ? 1 : 0);
				if (ImGui::Combo("Lights per Pixel", &index, kNames, 3))
					settings.PointLightsPerPixel = kKs[std::clamp(index, 0, 2)];
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("How many of the lights that light a pixel most are traced there (one ray each). Default 2.");
					ImGui::Text("  Weaker lights at that pixel keep their old shadows. More = slower.");
				}
			}
			ImGui::SliderInt("Max Lights", &settings.PointLightsMaxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Shadow-casting lights per frame at most (the ones that matter most on screen). Lights within 64 units count as one.");
			ImGui::SliderFloat("Max Distance", &settings.PointLightsDistance, 512.0f, 8192.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Lights farther from the camera keep their old shadows. 70 units = 1 m. Default 4000 (about 57 m).");
			ImGui::Checkbox("Native Shadow Lights Use RT Shadows", &settings.PointLightsNative);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("The game's own shadow-casting lights (fireplaces, some campfires): their ray-traced shadow replaces the game's");
				ImGui::Text("  shadow map in the lighting (default on). Off: they keep the game's shadow map.");
				ImGui::Text("  Outdoors spot lights keep the game's shadow map; indoors their cone is taken from the game.");
			}
			ImGui::Checkbox("Stable Light Selection", &settings.PointLightsStable);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): flickering candles and lights whose range changes do not make an area jump between the ray-traced");
				ImGui::Text("  shadow and the old one. Off: the lights are picked again every frame.");
			}
			// (46s)
			ImGui::Checkbox("Steady Flickering Lights", &settings.PointLightsSteady);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): the game wobbles flickering torches and fires around their spot (up to about half a metre). Their ray-traced");
				ImGui::Text("  shadows now come from the light's resting spot, so shadows stop swaying (the lighting itself still flickers as before).");
				ImGui::Text("  Only for lights fixed in the world; carried torches, your own lights and spells follow their light as before.");
				ImGui::Text("Off: shadows follow the wobbling light (46r).");
			}
			ImGui::Checkbox("Merge Hysteresis", &settings.PointLightsMergeHysteresis);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): two lights close together that share one shadow keep sharing it until they are clearly apart,");
				ImGui::Text("  so the shadow does not jump between one and two sources. Off: decided again every frame (46r).");
			}
			{
				const char* kMeshless[3] = { "Own", "Snap", "Skip" };
				settings.PointLightsMeshless = std::clamp(settings.PointLightsMeshless, 0, 2);
				ImGui::Combo("Meshless Lights Near Fires", &settings.PointLightsMeshless, kMeshless, 3);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Some of the game's \"torch\" lights have no torch: an invisible light placed near braziers and fires.");
					ImGui::Text("  Own (46r): it casts its own shadow, from a point where nothing burns (an extra shadow between two braziers).");
					ImGui::Text("  Snap (default): its shadow comes from the nearest visible fire or lamp within about 4.5 m; none in range, or a torch");
				ImGui::Text("    holder / candle right next to it (then it is that fixture's light): its own.");
					ImGui::Text("  Skip: it casts no ray-traced shadow (the old screen-space shadow, if any).");
				}
			}
			ImGui::Checkbox("Keep Screen-Space Contact Shadows", &settings.PointLightsKeepSSPLS);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Off (default): Screen Space Point Light Shadows are skipped for ray-traced lights.");
				ImGui::Text("  On: they stay on as well; the darker of both counts (small contact shadows of grass and leaves the ray tracer does not see).");
			}
			ImGui::SliderFloat("Fixture Clearance", &settings.PointLightsClearance, 0.0f, 128.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Rays stop this far before the light, so the lamp, brazier bowl or torch holding it does not shadow its own light.");
				ImGui::Text("  Too small: a dark ring or a black patch around a lamp. Too large: objects right next to a light cast nothing.");
			}
			ImGui::BeginDisabled(!settings.CarrierShadows);
			ImGui::SliderFloat("Carrier Clearance (units)", &settings.CarrierClearance, 0.0f, 64.0f, "%.0f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Body parts closer than this to a carried light do not block it (70 units = 1 m). NPC lights also stop %.0f units before the flame.",
					settings.PointLightsClearance);
			ImGui::EndDisabled();
			ImGui::Checkbox("Spell / Effect Lights Cast RT Shadows", &settings.PointLightsEffects);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): lights of spells and effects that belong to nobody (Magelight on a wall, fire bolts in flight)");
				ImGui::Text("  cast ray-traced shadows. Off: they keep the old shadows.");
			}
			ImGui::Checkbox("Plants and Cut-Outs Block Point Lights Outdoors", &settings.PointLightsAlphaOutdoors);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): at night leaves, bushes and cut-out shapes (fences, nets) cast shadows from torches and campfires,");
				ImGui::Text("  for the kinds traced with their cut-out (Foliage / Cut-outs under Sun Shadows > Advanced). A little slower.");
				ImGui::Text("Off: outdoors only solid shapes block point lights.");
			}
			ImGui::BeginDisabled(!settings.PointLightsInteriors);
			{
				static constexpr int kKs[3] = { 1, 2, 4 };
				const char* kNames[3] = { "1", "2", "4" };
				int index = settings.PointLightsPerPixelInterior >= 4 ? 2 : (settings.PointLightsPerPixelInterior >= 2 ? 1 : 0);
				if (ImGui::Combo("Lights per Pixel Indoors", &index, kNames, 3))
					settings.PointLightsPerPixelInterior = kKs[std::clamp(index, 0, 2)];
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Indoors many lights overlap; 4 (default) catches nearly all of the light. 2 or 1 = cheaper.");
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::TreePop();
		}

		// A/B switches (saved settings; the defaults are the tested ones).
		bool reset = false;
		if (Util::BeginABSwitchGroup(nullptr, reset)) {
			if (reset) {
				const Settings d{};
				settings.PointLightsBeamGuard = d.PointLightsBeamGuard;
				settings.PointLightsSkipShadowMaps = d.PointLightsSkipShadowMaps;
				settings.PointLightsBeamCull = d.PointLightsBeamCull;
			}
			ImGui::BeginDisabled(!settings.PointLights || !settings.PointLightsInteriors);
			ImGui::Checkbox("Window Beam Guard", &settings.PointLightsBeamGuard);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Off (default). On: a window beam whose whole cone comes out blocked keeps the game's shadow map for a while,");
				ImGui::Text("  then it is tried again. It also catches normal beams whose cone mostly hits walls, which flickers.");
				ImGui::Text("  %u beams caught so far.", beamsImplausible);
			}
			ImGui::Checkbox("Stop Engine Shadow Maps for RT Lights", &settings.PointLightsSkipShadowMaps);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Off (default): the game still draws its shadow maps for the lights ray tracing handles (wasted work, but a safe fallback).");
				ImGui::Text("On: indoors the game skips them (saves about 0.7-1.2 ms); ray tracing fills in their shadows. Whenever the ray-traced");
				ImGui::Text("  result might be missing in a frame, the game draws them as usual.");
			}
			// The other two modes block Window Shadows Ultimate's beams on its one-sided glass pane by design: developer mode only.
			if (globals::state->IsDeveloperMode()) {
				const char* cullModes[] = { "None (debug: blocks the beams)", "Faces Toward the Room", "Faces Away (debug: blocks the beams)" };
				settings.PointLightsBeamCull = std::clamp(settings.PointLightsBeamCull, 0, 2);
				ImGui::Combo("Window Beam Culling (debug)", &settings.PointLightsBeamCull, cullModes, 3);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Which wall faces a window beam's rays ignore. \"Faces Toward the Room\" (default) lets the beam pass the room's shell");
					ImGui::Text("  and Window Shadows Ultimate's one-sided glass pane, as the game's beam shadow maps do. The other two block the beam.");
				}
			}
			ImGui::EndDisabled();
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Diagnostics")) {
			ImGui::Checkbox("Split Screen (RT left, old right)", &pointSplit);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Left half of the screen: ray-traced point-light shadows; right half: Screen Space Point Light Shadows / shadow maps as before. Not saved.");
			if (PointLightsOn() && InteriorsOn() && settings.Enabled && Util::IsInterior()) {
				const auto& sc = scene.GetStats();
				const auto& st = backend.GetStats();
				const auto& ps = pointSelector.GetStats();
				ImGui::Text("Interior: %s, %u shapes, %.2f M triangles, %u BLAS (%s), capture %.1f ms CPU in total", sc.interior ? (scene.HasCoverage() ? "captured" : "capturing") : "not captured",
					sc.interiorShapes, static_cast<double>(sc.interiorTriangles) / 1e6, st.blasCount, Megabytes(st.blasBytes + st.scratchBytes + st.tlasBytes).c_str(),
					sc.interiorGatherMs);
				ImGui::Text("  Ready %s after entering; window masks %u; window beams %u, spot lights %u traced; mask channels written %u",
					interiorReadyMs >= 0.0f ? std::format("{:.0f} ms", interiorReadyMs).c_str() : "-", sc.windowMasks, pointThisFrame ? ps.beams : 0u,
					pointThisFrame ? ps.spots : 0u, maskChannelsWritten);
				ImGui::Text("  Engine shadow maps left out: %u this frame (%llu so far, %llu frames with the fallback only)%s", skippedLastFrame,
					static_cast<unsigned long long>(skippedTotal), static_cast<unsigned long long>(skipFallbackFrames),
					ps.spotsInvalid ? std::format("; {} spot lights left to the game (cone unreadable)", ps.spotsInvalid).c_str() : "");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Window masks: Window Shadows Ultimate's window shapes traced with their cut-out (needs the alpha-tested trace).");
					ImGui::Text("Mask channels: the game's shadow lights whose shadow (the shadow mask) ray tracing writes this frame.");
					ImGui::Text("Fallback: a light whose shadow map was left out but whose ray-traced result was missing gets no shadow that frame (should stay 0).");
					ImGui::Text("Box: %.0f x %.0f x %.0f units", sc.interiorBounds[3] - sc.interiorBounds[0], sc.interiorBounds[4] - sc.interiorBounds[1],
						sc.interiorBounds[5] - sc.interiorBounds[2]);
				}
			}
			if (PointLightsOn() && settings.Enabled) {
				const auto& ps = pointSelector.GetStats();
				const auto& rt = backend.GetStats();
				ImGui::Text("This frame: %u lights traced (K %u) of %u candidates (%u merged, %u over the cap, %u engine shadow lights); %u lights in use",
					pointThisFrame ? ps.sources : 0u, pointThisFrame ? pointK : 0u, ps.candidates, ps.merged, ps.capped, ps.native, ps.used);
				ImGui::Text("  of which the player's own lights %u, spell / effect lights %u, carried by other characters %u; plants / cut-outs tested %s",
					pointThisFrame ? ps.player : 0u, pointThisFrame ? ps.effects : 0u, pointThisFrame ? ps.carried : 0u, backend.PointAlphaLastFrame() ? "yes" : "no");
				ImGui::Text("Cost: trace %.2f ms GPU, %s rays per frame; denoiser %.2f ms GPU; selection %.3f ms CPU",
					std::max(rt.lastPointGpuMs, 0.0f), rt.pointRaysPerFrame >= 0 ? std::format("{:.2f} M", static_cast<double>(rt.pointRaysPerFrame) / 1e6).c_str() : "?",
					std::max(Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::RayTracingPointDenoise), 0.0f), pointCpuMs);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("GPU times are the last samples (the denoiser's only while the Performance Overlay or an F7 trace runs).");
					std::string reasons;
					for (int i = 0; i < 8; ++i)
						if (ps.byReason[i])
							reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", RayTracingPointLights::kRejectNames[i], ps.byReason[i]);
					ImGui::Text("Left out this frame: %s", reasons.empty() ? "none" : reasons.c_str());
					ImGui::Text("Not classified yet (asked again next frame): %u; classifier cache %u lights, %llu read so far; new slots this frame %u",
						ps.pending, Util::LightCensus::ClassifierCacheSize(), static_cast<unsigned long long>(Util::LightCensus::ClassifierReads()), ps.slotChanges);
				}
				// (46s)
				ImGui::Text("Steady positions: %u at the rest point, %u averaged; merge kept %u; meshless lights: %u snapped, %u own, %u skipped",
					ps.steadyParent, ps.steadyAverage, ps.mergeKept, ps.snapped, ps.meshlessOwn, ps.meshlessSkipped);
				const auto& reach = pointSelector.Reaching();
				if (!pointThisFrame || reach.empty()) {
					ImGui::Text("Lights reaching the player: none traced");
				} else {
					ImGui::Text("Lights reaching the player (%u):", static_cast<unsigned>(reach.size()));
					for (const auto& e : reach) {
						static constexpr const char* kKinds[3] = { "world", "player's", "spell / effect" };
						ImGui::Text("  #%u %s %s \"%s\": %.0f of %.0f units away%s%s%s", e.slot, kKinds[std::min<std::uint32_t>(e.kind, 2u)],
							Util::LightCensus::CategoryName(static_cast<Util::LightCensus::Category>(e.category)), e.label[0] ? e.label : "?", e.distance, e.radius,
							e.members > 1 ? std::format(", {} lights{}", e.members, e.snapped ? std::format(" ({} snapped)", e.snapped) : std::string()).c_str() : "",
							e.native ? ", game shadow light" : "", e.steady ? ", steady" : "");
					}
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Ray-traced shadow sources whose light range covers you this frame (#slot, kind, census category, name of its brightest light).");
					ImGui::Text("A shadow on you comes from one of these, or from a light that is not ray traced. An F6 census records the same list.");
				}
			} else {
				ImGui::TextDisabled("Not tracing point lights right now.");
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void RayTracing::DrawAOPage()
{
	ImGui::PushID("rtAOPage");
	DrawMasterHeader();
	if (ImGui::TreeNodeEx("Ambient Occlusion", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("RT Ambient Occlusion", &settings.AO);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Ray-traced ambient occlusion (outdoors inside the traced area, and indoors with Indoors Too): the soft darkening in");
			ImGui::Text("  corners, under ledges and around objects comes from short rays against the ray-traced scene, so it no longer");
			ImGui::Text("  disappears when whatever casts it leaves the screen. Outdoors, farther away it fades back to Screen Space GI's AO.");
			ImGui::Text("Terrain and grass are not in the ray-traced scene: their darkening still comes from Screen Space GI.");
			ImGui::Text("Off (default): ambient occlusion exactly as before (Screen Space GI). Needs Ray Tracing and Screen Space GI on.");
		}
		ImGui::BeginDisabled(!settings.AO);
		ImGui::SliderFloat("Strength", &settings.AOStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("1 = as traced. Lower = lighter, higher = darker.");
		ImGui::SliderFloat("Ray Length", &settings.AORayLength, kAORayLengthMin, kAORayLengthMax, "%.0f units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How far an object may be and still darken a surface. 70 units = 1 m. Default 100 (about 1.4 m). Longer = wider, darker AO.");
		ImGui::Checkbox("Indoors Too##AO", &settings.AOInteriors);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): inside houses, inns, castles and dungeons the room itself (walls, furniture, clutter, people) is captured");
			ImGui::Text("  when you come in (behind the loading screen), and the AO there is ray traced too, everywhere in the interior.");
			ImGui::Text("Off: indoors the AO is Screen Space GI's, exactly as before.");
		}
		if (!interiorOffReason.empty()) {
			RtErrorLine("Indoors was switched off automatically", interiorOffReason);
			if (interiorAutoOff && ImGui::SmallButton("Retry Indoors##AO"))
				aoInteriorRequested = false;
		}
		ImGui::EndDisabled();
		if (!aoOffReason.empty()) {
			RtErrorLine("RT ambient occlusion was switched off automatically", aoOffReason);
			if (aoAutoOff && ImGui::SmallButton("Retry RT Ambient Occlusion"))
				aoRequested = false;
		}
		if (AOOn() && settings.Enabled) {
			const auto& ssgi = globals::features::screenSpaceGI;
			if (!ssgi.loaded || !ssgi.settings.Enabled)
				ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "Screen Space GI is off: RTAO needs it (it replaces / joins its AO).");
		}

		if (ImGui::TreeNodeEx("Advanced")) {
			ImGui::BeginDisabled(!settings.AO);
			{
				const char* modes[] = { "RTAO (keeps SSGI contact AO)", "Darker of RTAO and SSGI AO" };
				settings.AOMode = std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1);
				ImGui::Combo("Mode", &settings.AOMode, modes, IM_ARRAYSIZE(modes));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Outdoors (indoors has its own Mode Indoors below).");
					ImGui::Text("RTAO (default): ray-traced AO replaces Screen Space GI's AO; SSGI's small contact AO (cm-scale creases) is kept");
					ImGui::Text("  and both darken together.");
					ImGui::Text("Darker of RTAO and SSGI AO: per pixel the darker of the two (fullest darkening; SSGI's AO is never lost).");
				}
			}
			ImGui::Checkbox("Temporal", &settings.AOTemporal);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): after the edge-aware blur the last frames are blended in (held close to this frame's own values,");
				ImGui::Text("  so moving objects leave no dark trail). Off: this frame's blurred rays only (a little grainier).");
			}
			ImGui::SliderFloat("Max Distance", &settings.AODistance, 1000.0f, 8192.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Outdoors: RTAO fades back to Screen Space GI's AO over the last quarter of this distance. Default 4000 (about 57 m).");
				ImGui::Text("  Shorter = cheaper. Indoors there is no fade: the whole interior is traced.");
			}
			ImGui::BeginDisabled(!settings.AOInteriors);
			{
				const char* modes[] = { "RTAO + SSGI contact AO", "Darker of both" };
				settings.AOModeInterior = std::clamp(settings.AOModeInterior, 0, static_cast<int>(AOModeKind::Count) - 1);
				ImGui::Combo("Mode Indoors", &settings.AOModeInterior, modes, IM_ARRAYSIZE(modes));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("RTAO + SSGI contact AO (default): indoors the ray-traced AO replaces Screen Space GI's AO; SSGI's small contact AO");
					ImGui::Text("  (cm-scale creases) is kept and both darken together.");
					ImGui::Text("Darker of both: per pixel the darker of RTAO and SSGI's whole AO.");
				}
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Diagnostics")) {
			ImGui::BeginDisabled(!settings.AO);
			{
				const char* views[] = { "Off", "Raw RTAO", "Filtered RTAO", "RTAO vs SSGI (split)" };
				int view = static_cast<int>(aoDebugView);
				if (ImGui::Combo("AO Debug View", &view, views, IM_ARRAYSIZE(views)))
					aoDebugView = static_cast<AODebugView>(std::clamp(view, 0, static_cast<int>(AODebugView::Count) - 1));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Raw RTAO: this frame's rays (white = open, black = blocked); blue tint = not traced (SSGI's AO there).");
					ImGui::Text("Filtered RTAO: after the blur (and the history), as it enters the mix.");
					ImGui::Text("RTAO vs SSGI (split): the real picture; left half with RTAO, right half with Screen Space GI's AO as before.");
					ImGui::Text("Not saved.");
				}
			}
			ImGui::EndDisabled();
			if (AOOn() && settings.Enabled) {
				if (Util::IsInterior()) {
					const auto& sc = scene.GetStats();
					if (!AOInteriorsOn())
						ImGui::TextDisabled("Indoors: not traced (Indoors Too is off).");
					else
						ImGui::Text("Indoors: %s, %u shapes; mode %s; fade-in %.0f %%", sc.interior ? (scene.HasCoverage() ? "interior captured" : "capturing the interior") : "interior not captured",
							sc.interiorShapes, settings.AOModeInterior == static_cast<int>(AOModeKind::Darker) ? "darker of both" : "RTAO + SSGI contact AO",
							static_cast<double>(fade * aoRamp) * 100.0);
				}
				const auto& rt = backend.GetStats();
				auto* timers = Util::GpuPassTimers::GetSingleton();
				ImGui::Text("This frame: %s; rays %.2f ms GPU (%s per frame), filter %.2f ms, game queue waiting %.2f ms; leaves / cut-outs tested %s",
					aoComposited ? "mixed in" : "not used", std::max(rt.lastAOGpuMs, 0.0f),
					rt.aoRaysPerFrame >= 0 ? std::format("{:.2f} M", static_cast<double>(rt.aoRaysPerFrame) / 1e6).c_str() : "?",
					std::max(timers->LastSampleMs(Util::GpuBucket::RayTracingAO), 0.0f), std::max(timers->LastSampleMs(Util::GpuBucket::RayTracingAOSync), 0.0f),
					rt.aoAlphaLastFrame ? "yes" : "no");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("GPU times are the last samples (the D3D11 ones only while the Performance Overlay or an F7 trace runs).");
			} else {
				ImGui::TextDisabled("Not tracing ambient occlusion right now.");
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void RayTracing::DrawDiagnostics()
{
	const auto& rt = backend.GetStats();
	const auto& alphaBytes = backend.GetAlphaStats();
	const std::uint64_t vram = rt.blasBytes + rt.scratchBytes + rt.tlasBytes + rt.uploadBytes + rt.sharedTextureBytes + d3d11Bytes +
	                           alphaBytes.uvBytes + alphaBytes.layerBytes + alphaBytes.infoBytes +
	                           backend.GetSkinStats().bytes + backend.GetSkinStats().scratchBytes + backend.GetSkinStats().boneBytes;

	// Summary for players: where the settings are, what is on, what it costs.
	ImGui::TextWrapped("The settings are on the Sun Shadows, Point Light Shadows and Ambient Occlusion pages (below this one in the list).");
	if (settings.Enabled) {
		ImGui::Text("In use: sun shadows (%s), point-light shadows %s, ambient occlusion %s", kQualityNames[std::clamp(settings.QualityLevel, 0, 5)],
			settings.PointLights ? "on" : "off", settings.AO ? "on" : "off");
		if (status == Status::Active || status == Status::Inactive || status == Status::Waiting)
			ImGui::Text("Cost: %.1f ms on the graphics card, %s of video memory", rt.smoothedGpuMs, Megabytes(vram).c_str());
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Graphics-card time of the ray-tracing work (scene updates and tracing, smoothed) and the video memory it holds.");
		if (overVramBudget)
			ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "Over the 1.2 GB scene budget: no further cells are captured.");
	}

	// Advanced: the loading-screen preparation.
	if (ImGui::TreeNodeEx("Advanced")) {
		ImGui::SeparatorText("Loading screens");
		ImGui::Checkbox("Prepare Behind Loading Screens", &settings.LoadingPump);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): while a loading screen is up, the place you arrive at is captured and built for ray tracing as its cells");
			ImGui::Text("  finish loading, so the first frames you see do not have to do it. Off: everything starts when the world is drawn again.");
		}
		ImGui::BeginDisabled(!settings.LoadingPump);
		ImGui::SliderFloat("Loading Screen Work (ms per frame)", &settings.LoadingPumpMs, 2.0f, 32.0f, "%.0f ms");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("CPU time Ray Tracing may use per loading-screen frame. More: ready sooner, but the game's own loading gets less time.");
		ImGui::SliderFloat("Black Screen Window (ms)", &settings.BlackWindowMs, 0.0f, 2000.0f, "%.0f ms");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How long after the loading screen the picture still counts as black (large budgets; the switch to RT happens here).");
			ImGui::Text("After it only the small per-frame budgets of normal play are used.");
		}
		ImGui::Checkbox("Prepare On Cell Load", &settings.PrepareOnCellEvents);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): each cell of the place you arrive at is captured and built for ray tracing the moment it finishes loading,");
			ImGui::Text("  behind the loading screen. Off: only the per-frame work above.");
		}
		ImGui::BeginDisabled(!settings.PrepareOnCellEvents);
		ImGui::SliderFloat("Work per Loaded Cell (ms)", &settings.CellEventMs, 2.0f, 50.0f, "%.0f ms");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("CPU time per loaded cell. The loading screen gets longer by about this much per cell (about 25 cells per fast travel).");
		ImGui::EndDisabled();
		ImGui::Checkbox("Use the Fade-In", &settings.FadeInBudgets);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): while the picture fades in after a loading screen (about 3 s), the first second still works as hard as the");
			ImGui::Text("  black screen, then less and less. Off: small budgets as soon as the black window is over.");
		}
		ImGui::EndDisabled();
		ImGui::TreePop();
	}

	// A/B switches (saved settings; the defaults are the tested ones). Same picture either way.
	bool reset = false;
	if (Util::BeginABSwitchGroup(nullptr, reset)) {
		if (reset) {
			const Settings d{};
			settings.CharactersParallel = d.CharactersParallel;
			settings.CharactersScanCache = d.CharactersScanCache;
			settings.CharactersMerge = d.CharactersMerge;
			settings.SplitTraceSync = d.SplitTraceSync;
			settings.ParallelBlasBuilds = d.ParallelBlasBuilds;
			settings.HighPriorityQueue = d.HighPriorityQueue;
			settings.SubmitWorker = d.SubmitWorker;
			settings.BuildGpuBudget = d.BuildGpuBudget;
			settings.BuildGpuBudgetMs = d.BuildGpuBudgetMs;
			settings.GroupBuildsPerFrame = d.GroupBuildsPerFrame;
			settings.GroupGraceMs = d.GroupGraceMs;
		}
		ImGui::Checkbox("Parallel Character Reads", &settings.CharactersParallel);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): the characters' bone positions and parts are read on a few helper threads (the frame waits for them");
			ImGui::Text("  at the same point). Off: all on the render thread. F7: rt_ch_reads_ms, rt_ch_lanes.");
		}
		ImGui::Checkbox("Character Scan Cache", &settings.CharactersScanCache);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): a character's whole 3D is searched for parts only when it may have changed (equipping, a new character)");
			ImGui::Text("  and every 8 frames; in between only its known parts are checked. Off: searched every frame. F7: rt_ch_rescans.");
		}
		ImGui::Checkbox("Merge Character Parts", &settings.CharactersMerge);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): each character's solid parts (body, clothes, armour, weapons) are one ray-tracing object: one update per");
			ImGui::Text("  character per frame instead of one per part. Hair and cut-out parts stay separate. Off: one object per part.");
			ImGui::Text("  Changing it rebuilds the characters (a moment). F7: rt_skin_groups, rt_prep_gpu_ms, rt_e_skin_ms.");
		}
		ImGui::Checkbox("Split Trace Sync", &settings.SplitTraceSync);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default, with point-light shadows traced): the game's GPU queue waits only for the sun's shadow before merging it,");
			ImGui::Text("  and for the point lights only right before their clean-up. Off: one wait for both.");
			ImGui::Text("  F7: rt_sync_sun_gpu_ms, rt_sync_pl_gpu_ms (rt_sync_gpu_ms = both).");
		}
		ImGui::Checkbox("Parallel BLAS Builds", &settings.ParallelBlasBuilds);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): the scenery shapes that enter in one frame are built side by side on the graphics card (at most 64 MB");
			ImGui::Text("  of scratch memory together). Shorter hitches when new cells or objects arrive. Off: one by one.");
			ImGui::Text("  F7: rt_prep_gpu_ms with rt_pw_builds.");
		}
		ImGui::Checkbox("RT Queue High Priority (restart)", &settings.HighPriorityQueue);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Experiment, off by default: ray tracing's own GPU queue asks for high priority, which may shorten the long waits for it");
			ImGui::Text("  when the card is busy. Takes effect after restarting the game. In effect now: %s.", backend.QueuePriorityHigh() ? "high" : "normal");
		}
		ImGui::Checkbox("RT: Submit on a Worker Thread", &settings.SubmitWorker);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): handing ray tracing's work to the graphics card is done by a helper thread, so the game's main thread");
			ImGui::Text("  does not stall there when Windows is busy moving video memory. Same order, same picture. Off: on the main thread.");
			ImGui::Text("  In effect now: %s. F7: rt_sub_* columns.", backend.SubmitWorkerInEffect() ? "on" : "off");
		}
		ImGui::Checkbox("RT: Budget Builds by GPU Time", &settings.BuildGpuBudget);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): new ray-tracing shapes (scenery and whole characters) are built a few per frame within a graphics-card");
			ImGui::Text("  time budget, and a character that leaves view for a moment keeps its shape for a short while (no rebuild when it");
			ImGui::Text("  comes back). Off: built as they come. F7: rt_bud_* columns.");
		}
		if (settings.BuildGpuBudget) {
			ImGui::Indent();
			ImGui::SliderFloat("Build Budget (GPU ms per frame)", &settings.BuildGpuBudgetMs, 0.25f, 4.0f, "%.2f");
			ImGui::SliderInt("Character Builds per Frame", &settings.GroupBuildsPerFrame, 1, 16);
			ImGui::SliderFloat("Character Keep Time (ms)", &settings.GroupGraceMs, 0.0f, 10000.0f, "%.0f");
			const auto& bud = backend.GetBudgetSample();
			ImGui::TextDisabled("Measured: %.1f GPU ms per million triangles, prep base %.2f ms, budget %u triangles", bud.msPerMillionTris, bud.prepBaseMs, bud.triBudget);
			ImGui::Unindent();
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Diagnostics")) {
		const char* views[] = { "Off", "Shadow Source", "RT vs Shadow Map", "BLAS Coverage", "Raw RT Visibility", "RT Hit vs Depth", "Penumbra Radius",
			"History Length", "Adaptive Rays", "SSS: RT Casters", "Distance Quality", "Character Rays", "Point Lights: Candidates", "Point Lights: Traced Lights",
			"Point Lights: Raw Rays", "Point Lights: Denoised" };
		static_assert(IM_ARRAYSIZE(views) == static_cast<int>(DebugView::Count));
		int view = static_cast<int>(debugView);
		if (ImGui::Combo("Debug View", &view, views, IM_ARRAYSIZE(views)))
			debugView = static_cast<DebugView>(std::clamp(view, 0, static_cast<int>(DebugView::Count) - 1));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Not saved.");
			ImGui::Text("Shadow Source: red = ray-traced shadow, blue = shadow-map shadow (characters, trees, grass, terrain), green = lit,");
			ImGui::Text("  grey = outside the ray-traced area (cascades and terrain shadows, unchanged), yellow = hand-off band.");
			ImGui::Text("  Terrain shadows are applied later by the lighting and are not shown.");
			ImGui::Text("RT vs Shadow Map: inside the ray-traced area, red = only RT sees a shadow, blue = only the shadow map does.");
			ImGui::Text("  While this view is on, the shadow map keeps every caster so the two can be compared.");
			ImGui::Text("BLAS Coverage: green = the surface is in the ray-traced scene, magenta = it is not (character, leaves, grass, terrain, skipped object),");
			ImGui::Text("  yellow/orange = the ray-traced scene has a surface up to a few metres IN FRONT of the depth the game wrote here:");
			ImGui::Text("  something the game does not draw (faded out, switched off), a part that moved since capture, or a surface");
			ImGui::Text("  missing from the depth buffer (seen through alpha-tested cut-outs). Small yellow seams along edges are harmless.");
			ImGui::Text("  Cyan = an alpha-tested surface traced with its cut-out, purple = a ray-traced character.");
			ImGui::Text("Raw RT Visibility: the traced result before smoothing and merging.");
			ImGui::Text("RT Hit vs Depth: white = the ray-traced surface sits exactly where the game draws it,");
			ImGui::Text("  red = it is in front (full red = 256 units), blue = behind, black = nothing there.");
			ImGui::Text("Penumbra Radius (Denoiser Penumbra): blur width per pixel, clear = none (sharp), blue = small, red = 16 px.");
			ImGui::Text("History Length (Denoiser Penumbra): red = this frame only (new, uncovered or fast camera), green = 12 frames.");
			ImGui::Text("Adaptive Rays: magenta = extra rays were traced here (shadow edges).");
			ImGui::Text("SSS: RT Casters (Screen-space shadows: Non-RT casters only): yellow = a ray-traced caster; its screen-space");
			ImGui::Text("  contact shadow is limited to the contact length.");
			ImGui::Text("Distance Quality: green = full quality, towards red = fewer extra rays and a smaller blur with distance,");
			ImGui::Text("  blue = pixels not traced this frame (checkerboard / half pattern), filled from their neighbours.");
			ImGui::Text("Character Rays: cyan = the extra ray against characters ran here, purple = a character shadows it.");
			ImGui::Text("Point Lights: Candidates: shadow-casting lights that reach the pixel (its lit side): green 1, yellow 2, orange 3-4, red 5+.");
			ImGui::Text("Point Lights: Traced Lights: a colour per light; stripes = a second (third, fourth) light traced there too.");
			ImGui::Text("Point Lights: Raw Rays / Denoised: the point-light shadows before / after the clean-up (white = lit).");
		}
		if (!lastEventsSummary.empty())
			ImGui::TextWrapped("Last transition: %s", lastEventsSummary.c_str());

		const auto& sc = scene.GetStats();
		const auto& skip = sc.skipped;
		const auto& check = backend.GetTlasCheck();
		std::uint32_t mismatched = 0, notBuilt = 0;
		for (int c = 0; c < RayTracingRT::kInstanceCategories; ++c) {
			mismatched += check.mismatched[c];
			notBuilt += check.notBuilt[c];
		}
		const float frameMs = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracing);
		ImGui::Text("GPU: %.2f ms on the RT queue (builds + trace)%s", rt.smoothedGpuMs,
			frameMs > 0.0f ? std::format(", {:.2f} ms in the frame (Performance Overlay row)", frameMs).c_str() : "");
		ImGui::Text("CPU: %.2f ms this frame, capture %.2f ms (worst %.2f ms, last cell %.1f ms in total)", lastCpuMs, sc.lastGatherMs,
			sc.maxGatherMs, sc.lastCellMs);
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset worst")) {
			scene.ResetPeaks();
			characters.ResetPeaks();
			cpuWorst = {};
		}
		ImGui::Text("VRAM: %s total", Megabytes(vram).c_str());
		const ImVec4 bad{ 1.0f, 0.5f, 0.3f, 1.0f }, good{ 0.6f, 0.9f, 0.6f, 1.0f };
		ImGui::TextColored(mismatched ? bad : good, "Self-check: %u of %u instances outside the game's own bounds (%llu so far, should be 0)", mismatched,
			check.checked, static_cast<unsigned long long>(check.mismatchedTotal));
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Each ray-traced instance, exactly as handed to the GPU, must lie inside the bounds the game computed for its shapes. Details in the log.");
		ImGui::TextColored(sc.missedConfirmed ? bad : good, "Missed casters: %u big shapes cast a shadow-map shadow inside the traced area but are not in the scene",
			sc.missedConfirmed);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Shapes with a bound radius of %.0f units or more drawn into the sun shadow map inside the traced area.", RayTracingCasterFilter::kMissedMinRadius);
			ImGui::Text("Counted once each, only after their cell finished capturing and their BLAS was built (%u were re-checked later).", sc.missedPending);
			ImGui::Text("Each distinct one is described in the log (8 per 10 s, 200 per session; characters and tree leaves are not).");
			ImGui::Text("A reference its cell did not have when captured gets one recapture first (%u requested so far).", sc.missedRecaptures);
		}
		{
			static constexpr const char* kDenoiserNames[] = { "Off", "Penumbra", "Penumbra", "DLSS RR", "NRD SIGMA" };
			const auto& q = lastQuality;
			ImGui::Text("In effect: denoiser %s, %u+%u rays, pattern %s / far %s, fall-off %.0f-%.0f, other characters %s (%u of %u shapes re-used)",
				kDenoiserNames[static_cast<int>(lastEffectiveDenoiser)], q.baseRays, q.extraRays, kPatternNames[std::min(q.tracePattern, 2u)],
				kPatternNames[std::min(q.farPattern, 2u)], q.falloffStart, q.falloffEnd, q.npcReduced ? "reduced" : "full", backend.GetSkinStats().reusedLastFrame,
				backend.GetSkinStats().drawsLastFrame);
		}

		if (ImGui::TreeNodeEx("Details")) {
			if (!lastLoadingSummary.empty())
				ImGui::TextWrapped("Last loading screen: %s", lastLoadingSummary.c_str());
			{
				const auto& pools = rt.pools;
				ImGui::Text("  Pools: %s reserved in %u blocks, %s in use; blocks made behind loading / black screens %llu, on the pool thread %llu, released %llu",
					Megabytes(pools.reservedBytes).c_str(), pools.blocks, Megabytes(pools.usedBytes).c_str(), static_cast<unsigned long long>(pools.syncBlocksTotal),
					static_cast<unsigned long long>(pools.asyncBlocksTotal), static_cast<unsigned long long>(pools.blocksReleased));
			}
			if (!backend.AdapterDescription().empty())
				ImGui::Text("GPU: %s", backend.AdapterDescription().c_str());
			ImGui::Text("Cells: %u captured, %u loading, %u recaptured (total)", sc.cellsComplete, sc.cellsGathering, sc.recaptures);
			ImGui::Text("Scene: %u shapes, %.2f M triangles (persistent references taken: %u statics, %u objects)", sc.shapes,
				static_cast<double>(sc.triangles) / 1e6, sc.persistentRefs, sc.persistentObjects);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Persistent references are kept by the worldspace, not by the cell they stand in; they are taken with that cell.");
			{
				const auto& ob = sc.objects;
				ImGui::Text("Objects: %u now (captured so far: doors/activators/movable %u, items %u, trees/plants %u), %u shapes in the scene,",
					ob.captured, ob.refs[0], ob.refs[1], ob.refs[2], ob.registeredShapes);
				ImGui::Text("  %u distinct meshes (%.2f M triangles)%s", ob.meshes, static_cast<double>(ob.meshTriangles) / 1e6,
					ob.overBudget ? std::format(", {} new meshes left out (VRAM budget)", ob.overBudget).c_str() : "");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Doors, activators, movable statics, lights, loose items and the opaque parts of trees and plants (trunks, branches).");
					ImGui::Text("Each is traced where it is this frame. Leaves and other alpha-tested parts keep their shadow-map shadow");
					ImGui::Text("  unless Foliage / Cut-outs are ray traced.");
				}
			}
			{
				// Alpha-tested casters.
				const auto& al = sc.alpha;
				const auto& ab = alphaBytes;
				ImGui::Text("Alpha-tested: %u shapes traced with their cut-out (%u captured, %u meshes, %.2f M triangles, %u instances last frame)",
					al.registered, al.captured, al.meshes, static_cast<double>(al.meshTriangles) / 1e6, ab.instancesLastFrame);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Captured shapes wait until their BLAS is built and their texture is ready; then they leave the shadow map.");
					ImGui::Text("Cyan in BLAS Coverage = an alpha-tested surface the ray tracer sees with its cut-out.");
				}
				ImGui::Text("  Textures: %u as 256x256 alpha copies (%u being made)", ab.texturesFallback, ab.texturesPending);
				ImGui::Text("  VRAM: texcoords %s of %s, copies %s, records %s; left to the shadow map: %u mesh, %u texture, %u buffer full",
					Megabytes(ab.uvBytesUsed).c_str(), Megabytes(ab.uvBytes).c_str(), Megabytes(ab.layerBytes).c_str(), Megabytes(ab.infoBytes).c_str(),
					al.failedMesh, al.failedTexture, al.failedRecord);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Texcoords: 24 bytes per alpha-tested triangle (one buffer of %s, allocated on first use). Copies: 256x256 alpha layers.",
						Megabytes(64ull << 20).c_str());
					ImGui::Text("Left to the shadow map (since start): mesh unreadable or too big, texture not registrable, texcoord buffer / copy layers / records full");
					ImGui::Text("  (copy layers full %llu, texcoords full %llu, records full %llu).", static_cast<unsigned long long>(ab.layersFull),
						static_cast<unsigned long long>(ab.uvFull), static_cast<unsigned long long>(ab.infosFull));
				}
			}
			{
				// Characters.
				const auto& ch = characters.GetStats();
				const auto& sk = backend.GetSkinStats();
				ImGui::Text("Characters: %u actors (%u in range), %u shapes traced of %u captured (%u skinned, %u rigid, %u physics, %u hair, %u alpha-tested)",
					ch.actors, ch.inRange, ch.registered, ch.shapes, ch.skinnedShapes, ch.rigidShapes, ch.physicsShapes, ch.hairShapes, ch.alphaShapes);
				ImGui::Text("  Hair recognised (since capture): %u by the hair tint material, %u by name / texture; left to the shadow map (Hair off) %u",
					ch.hairByTint, ch.hairByName, ch.skipHairOff);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Traced = in the TLAS and left out of the shadow map. Rigid = weapons, shields, quivers (one bone: the shape itself).");
					ImGui::Text("Purple in BLAS Coverage = a character surface the ray tracer sees.");
				}
				ImGui::Text("  Skinned %.2f M vertices last frame, %u in the TLAS; refits %u, builds %u (%llu total), %u not ready; %u waiting for upload",
					static_cast<double>(sk.verticesLastFrame) / 1e6, sk.drawsLastFrame, sk.refitsLastFrame, sk.buildsLastFrame,
					static_cast<unsigned long long>(sk.buildsTotal), sk.notReadyLastFrame, sk.pending);
				ImGui::Text("  VRAM: %s (of %s budget), scratch %s, bones %s; CPU %.2f ms (worst %.2f ms)", Megabytes(sk.bytes).c_str(),
					Megabytes(RayTracingRT::Backend::kSkinBudgetBytes).c_str(), Megabytes(sk.scratchBytes).c_str(), Megabytes(sk.boneBytes).c_str(), ch.lastMs, ch.maxMs);
				ImGui::Text("  CPU ms: actors %.2f, reads %.2f (%u lanes, %u bones, %u full scans), captures %.2f, still check %.2f, filter %.2f, draw list %.2f",
					ch.candidatesMs, ch.readsMs, ch.lanes, ch.bonesRead, ch.rescans, ch.captureMs, ch.commitMs, ch.filterMs, characters.LastCollectMs());
				ImGui::Text("  Merged: %u characters (%u parts), %u built, %u refit, %u kept; %u TLAS instances, %u skinning dispatches", sk.groupsLastFrame,
					sk.groupShapesLastFrame, sk.groupBuildsLastFrame, sk.groupRefitsLastFrame, sk.groupKeptLastFrame, sk.instancesLastFrame, sk.dispatchesLastFrame);
				ImGui::Text("  Left to the shadow map: physics off %u, alpha off %u, alpha failed %u, shader %u, type %u, no CPU data %u, bad data %u,",
					ch.skipPhysicsOff, ch.skipAlphaOff, ch.skipAlphaFailed, ch.skipShader, ch.skipGeometryType, ch.skipNoCpuData, ch.skipBadData);
				ImGui::Text("    placement %u, budget/size %u (VRAM %llu), shape cap %u; bone indices: palette %u, global %u; faces (morphs at capture) %u",
					ch.skipPlacement, ch.skipBackend, static_cast<unsigned long long>(sk.overBudget), ch.skipShapeCap, ch.paletteMode, ch.globalMode,
					ch.dynamicShapes);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Each distinct shape is counted once. Placement: its skinned vertices did not fit the game's own bounds (details in the log).");
					ImGui::Text("Shader: decals, effect shaders, alpha-blended only, does not cast shadows. Changes (equip, dismember) so far: %u.", ch.retiredShapes);
				}
			}
			ImGui::Text("BLAS: %u built (%.2f M triangles), %u queued, %u instances, %llu builds total", rt.blasCount,
				static_cast<double>(rt.blasTriangles) / 1e6, rt.blasQueued, rt.instancesLastFrame, static_cast<unsigned long long>(rt.buildsTotal));
			ImGui::Text("VRAM: BLAS %s, scratch %s, TLAS %s, uploads %s, textures %s", Megabytes(rt.blasBytes).c_str(), Megabytes(rt.scratchBytes).c_str(),
				Megabytes(rt.tlasBytes).c_str(), Megabytes(rt.uploadBytes).c_str(), Megabytes(rt.sharedTextureBytes + d3d11Bytes).c_str());
			if (rt.compactedSavedBytes)
				ImGui::Text("  Compaction saved %s so far", Megabytes(rt.compactedSavedBytes).c_str());
			{
				// Last samples per pass (the GPU timers run while the overlay table or an F7 trace is on).
				auto* timers = Util::GpuPassTimers::GetSingleton();
				auto ms = [&](Util::GpuBucket a_bucket) { return std::max(timers->LastSampleMs(a_bucket), 0.0f); };
				ImGui::Text("GPU last frame: builds/skinning/TLAS %.2f, trace %.2f; denoiser tiles %.2f, filter %.2f, SIGMA %.2f; combine %.2f; game queue waiting %.2f ms",
					std::max(rt.lastPrepGpuMs, 0.0f), std::max(rt.lastTraceGpuMs, 0.0f), ms(Util::GpuBucket::RayTracingDenoise), ms(Util::GpuBucket::RayTracingDenoiseFilter),
					ms(Util::GpuBucket::RayTracingSigma), ms(Util::GpuBucket::RayTracingCombine), ms(Util::GpuBucket::RayTracingSync));
			}
			{
				// Where the CPU goes (average / worst since reset); every frame of it is in the F7 trace (rt_* columns).
				const auto& a = cpuSmoothed;
				const auto& w = cpuWorst;
				ImGui::Text("  avg / worst ms: scene %.2f / %.1f, characters %.2f / %.1f, instances %.2f / %.1f, missed casters %.2f / %.1f", a.scene, w.scene,
					a.characters, w.characters, a.instances, w.instances, a.missed, w.missed);
				ImGui::Text("  D3D12 record + submit %.2f / %.1f, D3D11 passes %.2f / %.1f, end of frame %.2f / %.1f; engine objects waiting to be dropped %u",
					a.execute, w.execute, a.d3d11, w.d3d11, a.endFrame, w.endFrame, scene.GetFrameTimes().releasePending);
			}
			ImGui::Text("Shadow map: %u casters handed to RT, %u draws left out last frame%s", RayTracingCasterFilter::RegisteredCount(),
				RayTracingCasterFilter::SkippedLastFrame(), filterLatched ? "" : " (not active)");
			ImGui::Text("TLAS: %u cell chunks, %u doors/activators, %u items, %u trees, %u alpha-tested, %u character parts (%u not built yet)",
				check.written[RayTracingRT::kCategoryCell], check.written[RayTracingRT::kCategoryMovable], check.written[RayTracingRT::kCategoryItem],
				check.written[RayTracingRT::kCategoryTree], check.written[RayTracingRT::kCategoryAlpha], check.written[RayTracingRT::kCategoryCharacter], notBuilt);
			{
				// Missed casters by reason (this area).
				std::string reasons;
				for (int r = 0; r < static_cast<int>(RayTracingScene::MissReason::Count); ++r) {
					if (sc.missedByReason[r])
						reasons += std::format("{}{} {}", reasons.empty() ? "" : ", ", RayTracingScene::kMissReasonNames[r], sc.missedByReason[r]);
				}
				if (!reasons.empty())
					ImGui::TextWrapped("  By reason: %s", reasons.c_str());
			}
			if (rt.skippedFramesBusy || latchedWithoutTrace)
				ImGui::Text("Frames without a trace (D3D12 busy): %u, of which with casters left out of the shadow map: %u (should be 0)",
					rt.skippedFramesBusy, latchedWithoutTrace);
			if (ImGui::TreeNodeEx("Skipped objects")) {
				ImGui::Text("Characters %u, markers/other %u, disabled/no 3D %u, tree-animated parts of static objects %u",
					skip.actors, skip.markers, skip.disabled, skip.foliage);
				ImGui::Text("Shapes: hidden %u, alpha-tested %u, alpha-blended %u, decals %u, skinned %u, no shadow %u",
					skip.hidden, skip.alphaTested, skip.alphaBlended, skip.decals, skip.skinned, skip.noShadow);
				ImGui::Text("Shapes: not Lighting shader %u, other geometry types %u, no CPU vertex copy %u, bad data %u",
					skip.notLighting, skip.otherGeometry, skip.noCpuData, skip.badData);
				ImGui::Text("Shapes: camera-facing billboards %u, refraction (heat haze) %u, switched-off children %u; alpha-tested taken as opaque %u",
					skip.billboard, skip.refraction, skip.switchedOff, skip.cutoutAsOpaque);
				ImGui::Text("Statics with a Havok-driven part (hinged signs) traced as moving objects: %u", skip.physicsParts);
				ImGui::Text("Alpha-tested traced with their cut-out %u, alpha-tested that could not be (small, no texture / texcoords, test) %u",
					skip.alphaAnyHit, skip.alphaUnusable);
				ImGui::Text("Placement check: %u shapes checked, %u fixed (other vertex format), %u left out (wrong place), %u without bound",
					sc.placementChecked, sc.placementFixed, sc.placementRejected, sc.placementUnchecked);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Each captured shape must fit inside the bound sphere the game computed for it. Details of the first misfits are in the log.");
				ImGui::TreePop();
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
}

// ---------------------------------------------------------------------------------------------
// (45j) Profile headers
// ---------------------------------------------------------------------------------------------

std::string RayTracing::DescribeForProfile() const
{
	if (!loaded)
		return "not loaded";
	static constexpr const char* kStatus[] = { "off", "unavailable", "failed", "paused", "loading", "active" };
	static constexpr const char* kFoliage[] = { "shadow map", "ray traced", "both" };
	static constexpr const char* kDenoisers[] = { "off", "penumbra", "penumbra", "DLSS RR", "NRD SIGMA" };
	auto mode = [](bool a_rt) { return a_rt ? "RT" : "SM"; };
	const QualityParams q = ResolveQuality();
	std::string text = std::format("RT {} ({}), quality {}, buildings {}, objects {}, trees {}, foliage {}, cut-outs {}, characters {}, physics parts {}, hair {}, "
								   "first-person player {}, SSS contact {:.1f}, denoiser {}",
		settings.Enabled ? "on" : "off", kStatus[static_cast<int>(status)], kQualityNames[std::clamp(settings.QualityLevel, 0, 5)], mode(settings.Buildings),
		mode(settings.Objects), mode(settings.Trees), kFoliage[std::clamp(settings.Foliage, 0, 2)], mode(settings.Cutouts), mode(settings.Characters),
		mode(settings.PhysicsParts), mode(settings.Hair), settings.FirstPersonPlayer ? "RT" : "SM", settings.RtContactScale,
		kDenoisers[static_cast<int>(CurrentDenoiser())]);
	// What the tier sets and the denoiser in effect.
	text += std::format(", denoiser in effect {}{}, rays {}+{}, sun cone {:.2f}, pattern {} far {}, fall-off {:.0f}-{:.0f}, other characters {}, denoise radius {:.0f} "
						"taps {} {} far scale {:.2f}, history {:.0f} clamp {:.2f}, SIGMA stabilisation {}",
		kDenoisers[static_cast<int>(EffectiveDenoiser())], UpscalingRRActive() ? " (Upscaling RR on)" : "", q.baseRays, q.extraRays, q.coneDegrees,
		kPatternNames[std::min(q.tracePattern, 2u)], kPatternNames[std::min(q.farPattern, 2u)], q.falloffStart, q.falloffEnd, q.npcReduced ? "reduced" : "full",
		q.denoiseRadius, q.denoiseTaps, q.denoiseHalf ? "half res" : "full res", q.farDenoiseScale, q.history, q.clampSigma, settings.SigmaStabilizedFrames);
	// (46b)
	text += std::format(", point lights {} (K {}, max {}, distance {:.0f}, native shadow lights {}, keep SSPLS {}, size {:.1f}, clearance {:.0f}{}{})",
		settings.PointLights ? "on" : "off", ValidPointK(settings.PointLightsPerPixel), settings.PointLightsMaxSources, settings.PointLightsDistance,
		settings.PointLightsNative ? "RT" : "shadow map", settings.PointLightsKeepSSPLS ? "yes" : "no", settings.PointLightsSize, settings.PointLightsClearance,
		pointSplit ? ", split screen" : "", pointOffReason.empty() ? "" : ", switched off: " + pointOffReason);
	// (46c)
	text += std::format(", indoors {} (K {}, window beams {}, stop engine shadow maps {}{})", settings.PointLightsInteriors ? "on" : "off",
		ValidPointK(settings.PointLightsPerPixelInterior), settings.PointLightsWindowBeams ? "on" : "off", settings.PointLightsSkipShadowMaps ? "on" : "off",
		interiorOffReason.empty() ? "" : ", switched off: " + interiorOffReason);
	// (46d)
	text += std::format(", player lights {}, spell / effect lights {}, plants block point lights outdoors {}", settings.PointLightsPlayer ? "on" : "off",
		settings.PointLightsEffects ? "on" : "off", settings.PointLightsAlphaOutdoors ? "on" : "off");
	// (46g)
	static constexpr const char* kBeamCull[] = { "none", "faces toward the room", "faces away" };
	text += std::format(", stable light selection {}, window beam culling {}, beam guard {}, (46o) window beam softness {:.2f} deg", settings.PointLightsStable ? "on" : "off",
		kBeamCull[EffectiveBeamCull()], settings.PointLightsBeamGuard ? "on" : "off", settings.PointLightsBeamSoftness);
	// (46f)
	static constexpr const char* kAOModes[] = { "RTAO (keeps SSGI contact AO)", "darker of RTAO and SSGI AO" };
	static constexpr const char* kAOViews[] = { "off", "raw", "filtered", "split" };
	text += std::format(", RTAO {} (mode {}, ray length {:.0f}, strength {:.2f}, temporal {}, max distance {:.0f}, debug view {}{})", settings.AO ? "on" : "off",
		kAOModes[std::clamp(settings.AOMode, 0, 1)], settings.AORayLength, settings.AOStrength, settings.AOTemporal ? "on" : "off", settings.AODistance,
		kAOViews[std::clamp(static_cast<int>(aoDebugView), 0, 3)], aoOffReason.empty() ? "" : ", switched off: " + aoOffReason);
	// (46h)
	static constexpr const char* kAOModesInterior[] = { "RTAO + SSGI contact AO", "darker of both" };
	text += std::format(", RTAO indoors {} (mode {}, no distance fade: the interior's box{})", settings.AO && settings.AOInteriors ? "on" : "off",
		kAOModesInterior[std::clamp(settings.AOModeInterior, 0, 1)], AOInteriorWanted() ? "" : (settings.AO && settings.AOInteriors ? ", not available now" : ""));
	// (46k)
	text += settings.LoadingPump ? std::format(", prepare behind loading screens on ({:.0f} ms per frame, black window {:.0f} ms)", settings.LoadingPumpMs,
									   settings.BlackWindowMs) :
	                               std::string(", prepare behind loading screens off (46i)");
	// (46n)
	text += std::format(", prepare on cell load {} ({:.0f} ms per cell), fade-in budgets {}", settings.PrepareOnCellEvents ? "on" : "off", settings.CellEventMs,
		settings.FadeInBudgets ? "on" : "off");
	// (46m)
	text += std::format(", parallel character reads {}, character scan cache {}, merge character parts {}, split trace sync {}",
		settings.CharactersParallel ? "on" : "off", settings.CharactersScanCache ? "on" : "off", settings.CharactersMerge ? "on" : "off",
		settings.SplitTraceSync ? "on" : "off");
	// (46p)
	text += std::format(", parallel BLAS builds {}, high-priority queue {} (in effect {}), residency priority {}", settings.ParallelBlasBuilds ? "on" : "off",
		settings.HighPriorityQueue ? "asked" : "off", backend.QueuePriorityHigh() ? "high" : "normal", backend.ResidencyHigh() ? "high" : "normal");
	// (46q)
	text += std::format(", submit on a worker thread {} (in effect {}), budget builds by GPU time {} ({:.2f} ms, {} character builds, keep {:.0f} ms), "
						"carrier casts shadows {} (clearance {:.0f})",
		settings.SubmitWorker ? "on" : "off", backend.SubmitWorkerInEffect() ? "on" : "off", settings.BuildGpuBudget ? "on" : "off", settings.BuildGpuBudgetMs,
		settings.GroupBuildsPerFrame, settings.GroupGraceMs, settings.CarrierShadows ? "on" : "off", settings.CarrierClearance);
	// (46s)
	static constexpr const char* kMeshless[3] = { "own", "snap", "skip" };
	text += std::format(", steady flickering lights {}, merge hysteresis {}, meshless lights near fires {}", settings.PointLightsSteady ? "on" : "off",
		settings.PointLightsMergeHysteresis ? "on" : "off", kMeshless[std::clamp(settings.PointLightsMeshless, 0, 2)]);
	return text;
}

nlohmann::json RayTracing::PointSourcesReachingPlayerJson() const
{
	auto j = nlohmann::json::array();
	if (!pointThisFrame)
		return j;
	static constexpr const char* kKinds[3] = { "world", "player", "effect" };
	for (const auto& e : pointSelector.Reaching()) {
		j.push_back({ { "slot", e.slot }, { "kind", kKinds[std::min<std::uint32_t>(e.kind, 2u)] },
			{ "category", Util::LightCensus::CategoryName(static_cast<Util::LightCensus::Category>(e.category)) }, { "name", std::string(e.label) },
			{ "distance", std::round(e.distance) }, { "radius", std::round(e.radius) }, { "intensity", e.intensity }, { "lights", e.members },
			{ "snapped", e.snapped }, { "native", e.native }, { "spot", e.spot }, { "steady", e.steady } });
	}
	return j;
}

nlohmann::json RayTracing::DescribeJson() const
{
	nlohmann::json j;
	j["summary"] = DescribeForProfile();
	j["enabled"] = settings.Enabled;
	j["active"] = status == Status::Active;
	j["quality"] = settings.QualityLevel;
	j["buildings_rt"] = settings.Buildings;
	j["objects_rt"] = settings.Objects;
	j["trees_rt"] = settings.Trees;
	j["foliage_mode"] = settings.Foliage;
	j["cutouts_rt"] = settings.Cutouts;
	j["characters_rt"] = settings.Characters;
	j["physics_parts_rt"] = settings.PhysicsParts;
	j["hair_rt"] = settings.Hair;
	j["first_person_player_rt"] = settings.FirstPersonPlayer;
	j["denoiser"] = settings.Denoiser;
	j["rt_contact_scale"] = settings.RtContactScale;
	j["denoiser_effective"] = static_cast<int>(EffectiveDenoiser());
	j["upscaling_rr_active"] = UpscalingRRActive();
	j["sigma_stabilized_frames"] = settings.SigmaStabilizedFrames;
	{
		auto* timers = Util::GpuPassTimers::GetSingleton();
		j["gpu_pass_ms"] = { { "tiles", timers->SmoothedMs(Util::GpuBucket::RayTracingDenoise) }, { "filter", timers->SmoothedMs(Util::GpuBucket::RayTracingDenoiseFilter) },
			{ "sigma", timers->SmoothedMs(Util::GpuBucket::RayTracingSigma) }, { "combine", timers->SmoothedMs(Util::GpuBucket::RayTracingCombine) },
			{ "sync", timers->SmoothedMs(Util::GpuBucket::RayTracingSync) } };
	}
	j["denoise_gpu_ms"] = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingDenoise) +
	                      Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingDenoiseFilter) +
	                      Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingSigma);
	const auto& stats = backend.GetStats();
	j["gpu_ms_smoothed"] = stats.smoothedGpuMs;
	j["cpu_ms"] = { { "total", cpuSmoothed.total }, { "scene", cpuSmoothed.scene }, { "characters", cpuSmoothed.characters },
		{ "instances", cpuSmoothed.instances }, { "missed", cpuSmoothed.missed }, { "execute", cpuSmoothed.execute }, { "d3d11", cpuSmoothed.d3d11 },
		{ "end_frame", cpuSmoothed.endFrame } };
	j["cpu_worst_ms"] = { { "total", cpuWorst.total }, { "scene", cpuWorst.scene }, { "characters", cpuWorst.characters },
		{ "instances", cpuWorst.instances }, { "missed", cpuWorst.missed }, { "execute", cpuWorst.execute }, { "d3d11", cpuWorst.d3d11 },
		{ "end_frame", cpuWorst.endFrame } };
	j["tlas_instances"] = stats.instancesLastFrame;
	// (46b)
	{
		const auto& ps = pointSelector.GetStats();
		j["point_lights"] = { { "enabled", settings.PointLights }, { "k", ValidPointK(settings.PointLightsPerPixel) }, { "max_sources", settings.PointLightsMaxSources },
			{ "distance", settings.PointLightsDistance }, { "native", settings.PointLightsNative }, { "keep_sspls", settings.PointLightsKeepSSPLS },
			{ "size", settings.PointLightsSize }, { "clearance", settings.PointLightsClearance }, { "traced", pointThisFrame }, { "sources", ps.sources },
			{ "candidates", ps.candidates }, { "merged", ps.merged }, { "capped", ps.capped }, { "native_lights", ps.native }, { "gpu_ms", stats.lastPointGpuMs },
			{ "rays_per_frame", stats.pointRaysPerFrame }, { "denoise_gpu_ms", Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingPointDenoise) },
			{ "cpu_ms", pointCpuMs }, { "off_reason", pointOffReason }, { "player", settings.PointLightsPlayer }, { "effects", settings.PointLightsEffects },
			{ "alpha_outdoors", settings.PointLightsAlphaOutdoors }, { "player_sources", ps.player }, { "effect_sources", ps.effects },
			{ "alpha_last_frame", backend.PointAlphaLastFrame() } };
		// (46c)
		const auto& sc = scene.GetStats();
		j["interior"] = { { "enabled", settings.PointLightsInteriors }, { "k", ValidPointK(settings.PointLightsPerPixelInterior) },
			{ "window_beams", settings.PointLightsWindowBeams }, { "skip_shadow_maps", settings.PointLightsSkipShadowMaps }, { "this_frame", interiorThisFrame },
			{ "captured", sc.interior }, { "shapes", sc.interiorShapes }, { "triangles", sc.interiorTriangles }, { "capture_cpu_ms", sc.interiorGatherMs },
			{ "window_masks", sc.windowMasks }, { "ready_ms", interiorReadyMs }, { "beams", ps.beams }, { "spots", ps.spots }, { "spots_invalid", ps.spotsInvalid },
			{ "mask_channels", maskChannelsWritten }, { "shadow_maps_skipped", skippedLastFrame }, { "shadow_maps_skipped_total", skippedTotal },
			{ "fallback_frames", skipFallbackFrames }, { "off_reason", interiorOffReason } };
	}
	// (46f)
	j["rtao"] = { { "enabled", settings.AO }, { "mode", settings.AOMode }, { "ray_length", settings.AORayLength }, { "strength", settings.AOStrength },
		{ "temporal", settings.AOTemporal }, { "max_distance", settings.AODistance }, { "debug_view", static_cast<int>(aoDebugView) },
		{ "composited", aoComposited }, { "gpu_ms", stats.lastAOGpuMs }, { "rays_per_frame", stats.aoRaysPerFrame },
		{ "filter_gpu_ms", Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingAO) },
		{ "sync_gpu_ms", Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingAOSync) }, { "off_reason", aoOffReason },
		{ "interiors", settings.AOInteriors }, { "mode_interior", settings.AOModeInterior }, { "interior_wanted", AOInteriorWanted() },
		{ "indoors_this_frame", aoInteriorThisFrame }, { "fade_in", aoRamp } };  // (46h)
	return j;
}

int RayTracing::EffectiveBeamCull() const
{
	// (46n) The other modes block Window Shadows Ultimate's beams on its one-sided glass pane by design: debug only.
	return globals::state && globals::state->IsDeveloperMode() ? std::clamp(settings.PointLightsBeamCull, 0, 2) : 1;
}
