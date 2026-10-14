#include "RayTracing.h"

#include "Deferred.h"
#include "Features/RayTracing/CasterFilter.h"
#include "Features/RayTracing/TextureSharing.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "State.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RayTracing::Settings,
	Enabled,
	QualityLevel,
	KeepCastersInShadowMap,
	KeepScreenSpaceShadows,
	CutoutAsOpaque,
	AlphaCasters,
	Characters,
	PhysicsParts,
	KeepCharactersInShadowMap,
	CharacterSelfShadowOffset,
	SettingsVersion,
	Buildings,
	Objects,
	Trees,
	Foliage,
	Cutouts,
	Hair,
	FirstPersonPlayer,
	ScreenSpaceShadows,
	KeepSSSUnderFoliage,
	Denoiser,
	Noise,
	AdaptiveRays,
	RtContactScale,
	RayOffsetScale,
	FlushBeforeTrace,
	LoadingBoost,
	WarmUp,
	StableNormals,
	FineAlphaMips,
	ShareGameTextures,
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
	LateSync,
	SplitSubmit,
	CharacterRayCulling,
	BlockerAtEdges,
	DistanceFalloff,
	NpcAlternateFrameSkinning)

namespace
{
	// (45l) Quality tiers. Medium / High / Ultra are 45k's Low / Medium / High (rays, sun cone, small
	// filter) plus the 45l cost cuts; Very Low and Low are new and cheaper.
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
			"45k's Low: 1 ray per pixel (+1 at shadow edges, fading out between 60 and 175 m), the real sun disc; other characters at "
			"reduced quality; half-resolution denoiser." },
		{ "High", 1, 3, 0.5f, 2, 0, 0, 6144.0f, 16384.0f, false, 16.0f, 6, false, 1.0f,
			"45k's Medium: 1 ray per pixel (+3 at shadow edges, fading out between 90 and 230 m), slightly softer edges; full-resolution denoiser." },
		{ "Ultra", 2, 4, 0.5f, 2, 0, 0, 0.0f, 0.0f, false, 16.0f, 6, false, 1.0f,
			"45k's High: 2 rays per pixel (+4 at shadow edges, no fall-off), slightly softer edges; full-resolution denoiser. Cleanest." },
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
	constexpr std::uint32_t kLoadingMaxBuilds = 64;
	constexpr double kWarmUpTimeoutMs = 5000.0;
	constexpr double kFadeInMs = 500.0;
	constexpr std::uint32_t kAlphaCopiesPerFrame = 16;  // (45h) 256x256 alpha copies made per frame (5 small dispatches each)

	struct LoadingMenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			// Load game, fast travel, door into another worldspace: every captured cell is stale.
			// (45k) Also when it closes: the warm-up starts.
			if (a_event && a_event->menuName == RE::LoadingMenu::MENU_NAME)
				globals::features::rayTracing.OnLoadingScreen(a_event->opening);
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	LoadingMenuSink g_loadingSink;

	std::string Megabytes(std::uint64_t a_bytes) { return std::format("{:.1f} MB", static_cast<double>(a_bytes) / (1024.0 * 1024.0)); }

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
	if (a_open) {
		scene.RequestFlush();
		characters.RequestFlush();  // (45i)
		loadingOpen.store(true);
	} else {
		loadingOpen.store(false);
		loadingClosed.store(true);
	}
}

void RayTracing::UpdatePhase()
{
	// (45k) Render thread, once per frame: Play -> Loading (menu open) -> WarmUp (closed, the near scene
	// still being captured / built: the shadow map keeps every caster) -> Play (RT fades in).
	const bool open = loadingOpen.load();
	const bool closed = loadingClosed.exchange(false);
	const auto& sc = scene.GetStats();
	const auto& stats = backend.GetStats();
	if (open && phase != Phase::Loading) {
		phase = Phase::Loading;
		loadShapesAtOpen = sc.shapes;
		loadBuildsAtOpen = static_cast<std::uint32_t>(stats.buildsTotal);
		fade = 0.0f;
	}
	if (!open && (closed || phase == Phase::Loading)) {
		phase = settings.WarmUp ? Phase::WarmUp : Phase::Play;
		warmUpStart = Qpc();
		loadShapesAtClose = sc.shapes;
		loadBuildsAtClose = static_cast<std::uint32_t>(stats.buildsTotal);
		fade = settings.WarmUp ? 0.0f : 1.0f;
	}
	if (phase == Phase::WarmUp) {
		const double elapsed = static_cast<double>(Qpc() - warmUpStart) * QpcMs();
		const bool ready = scene.HasCoverage() && sc.cellsGathering == 0 && backend.QueuedBuilds() == 0 && status == Status::Active;
		if (ready || elapsed > kWarmUpTimeoutMs || !settings.WarmUp) {
			phase = Phase::Play;
			lastTimeToReadyMs = static_cast<float>(elapsed);
			lastLoadingSummary = std::format("behind the loading screen {} shapes captured, {} BLAS built; after it {} shapes, {} BLAS; ready {:.0f} ms after it closed{}",
				loadShapesAtClose > loadShapesAtOpen ? loadShapesAtClose - loadShapesAtOpen : 0u,
				loadBuildsAtClose > loadBuildsAtOpen ? loadBuildsAtClose - loadBuildsAtOpen : 0u, sc.shapes > loadShapesAtClose ? sc.shapes - loadShapesAtClose : 0u,
				static_cast<std::uint32_t>(stats.buildsTotal) > loadBuildsAtClose ? static_cast<std::uint32_t>(stats.buildsTotal) - loadBuildsAtClose : 0u, elapsed,
				ready ? "" : " (timed out)");
			logger::info("[RayTracing] Loading screen: {}", lastLoadingSummary);
		}
	}
	if (phase == Phase::Play && fade < 1.0f) {
		const double elapsed = static_cast<double>(Qpc() - warmUpStart) * QpcMs();
		fade = static_cast<float>(std::clamp(elapsed / kFadeInMs, 0.0, 1.0));
	} else if (phase == Phase::WarmUp) {
		// While the shadow map still holds every caster, RT adds its shadows already (min of both), faded in.
		const double elapsed = static_cast<double>(Qpc() - warmUpStart) * QpcMs();
		fade = static_cast<float>(std::clamp(elapsed / kFadeInMs, 0.0, 1.0));
	}
}

bool RayTracing::ClaimScreenSpaceShadows()
{
	// (45k) Decided before Ray Tracing's own Prepass (Screen Space Shadows runs first in the list).
	sssClaimed = false;
	if (!settings.Enabled || status != Status::Active || static_cast<SSSMode>(settings.ScreenSpaceShadows) != SSSMode::NonRT || !typedUavLoadR8 ||
		Util::IsInterior() || phase == Phase::Loading)
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
	config.cutoutAsOpaque = settings.CutoutAsOpaque;
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
	characters.Configure(settings.PhysicsParts, settings.Hair, backend.SkinAlphaAvailable(), settings.KeepCharactersInShadowMap,
		settings.FirstPersonPlayer);
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
	// Debug A/B: no distance fall-off at all (the far pattern goes with it).
	if (!settings.DistanceFalloff) {
		q.falloffStart = q.falloffEnd = 0.0f;
		q.farPattern = 0;
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
	// (45l) Ray Reconstruction cleans the whole image, shadows included, but only raw per-frame noise:
	// whenever it runs, the shadows take the DLSS RR route whatever the dropdown says.
	if (UpscalingRRActive())
		return DenoiserMode::DLSSRR;
	return CurrentDenoiser();
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void RayTracing::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.CharacterSelfShadowOffset = std::isfinite(settings.CharacterSelfShadowOffset) ? std::clamp(settings.CharacterSelfShadowOffset, 0.0f, 32.0f) : 6.0f;
	// (45j) Once, from a 45d-45i save: the per-kind choices start at their new defaults (foliage and
	// trees in the shadow map, buildings / objects / characters / physics parts ray traced, hair in
	// the shadow map); the old switches are only logged. "Keep Screen Space Shadows" maps to Keep.
	if (!o_json.is_object() || !o_json.contains("SettingsVersion")) {
		logger::info("[RayTracing] Settings from before 45j: Alpha-tested casters {}, Characters {}, Physics parts {}, Keep Screen Space Shadows {} -> "
					 "new defaults per kind (foliage and trees: shadow map; buildings, objects, characters, physics parts: ray traced; hair: shadow map)",
			settings.AlphaCasters, o_json.is_object() && o_json.value("Characters", false), o_json.is_object() && o_json.value("PhysicsParts", false),
			settings.KeepScreenSpaceShadows);
		const Settings defaults{};
		settings.Buildings = defaults.Buildings;
		settings.Objects = defaults.Objects;
		settings.Trees = defaults.Trees;
		settings.Foliage = defaults.Foliage;
		settings.Cutouts = defaults.Cutouts;
		settings.Characters = defaults.Characters;
		settings.PhysicsParts = defaults.PhysicsParts;
		settings.Hair = defaults.Hair;
		settings.ScreenSpaceShadows = static_cast<int>(settings.KeepScreenSpaceShadows ? SSSMode::Keep : SSSMode::Combine);
		settings.SettingsVersion = 2;
	}
	// (45k) Once, from a save before 45k: Screen Space Shadows Combine becomes "non-RT casters only" (the new
	// default); Denoiser / Noise / Adaptive Rays start at their defaults (Penumbra, Auto, Auto).
	if (!o_json.is_object() || o_json.value("SettingsVersion", 0) < 3) {
		if (settings.ScreenSpaceShadows == static_cast<int>(SSSMode::Combine))
			settings.ScreenSpaceShadows = static_cast<int>(SSSMode::NonRT);
		logger::info("[RayTracing] Settings from before 45k: Screen Space Shadows mode {}, Denoiser Penumbra", settings.ScreenSpaceShadows);
		settings.SettingsVersion = 3;
	}
	// (45l) Once, from a save before 45l: the tiers moved up by two names (45k Low / Medium / High = Medium / High /
	// Ultra, the same settings); Screen Space Shadows "Hand off" becomes "Non-RT casters only" with contact length 0.
	if (!o_json.is_object() || o_json.value("SettingsVersion", 0) < 4) {
		const int oldQuality = settings.QualityLevel;
		if (o_json.is_object() && o_json.contains("QualityLevel"))
			settings.QualityLevel = std::clamp(oldQuality, 0, 2) + 2;
		if (settings.ScreenSpaceShadows == static_cast<int>(SSSMode::HandOff)) {
			settings.ScreenSpaceShadows = static_cast<int>(SSSMode::NonRT);
			settings.RtContactScale = 0.0f;
		}
		logger::info("[RayTracing] Settings from before 45l: quality {} -> {} ({}), Screen Space Shadows mode {}", oldQuality, settings.QualityLevel,
			kQualityNames[std::clamp(settings.QualityLevel, 0, 5)], settings.ScreenSpaceShadows);
		settings.SettingsVersion = 4;
	}
	settings.QualityLevel = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
	if (settings.ScreenSpaceShadows == static_cast<int>(SSSMode::HandOff))
		settings.ScreenSpaceShadows = static_cast<int>(SSSMode::NonRT);
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
	settings.ScreenSpaceShadows = std::clamp(settings.ScreenSpaceShadows, 0, 3);
	settings.Denoiser = std::clamp(settings.Denoiser, 0, static_cast<int>(DenoiserMode::Count) - 1);
	settings.Noise = std::clamp(settings.Noise, 0, 2);
	settings.AdaptiveRays = std::clamp(settings.AdaptiveRays, 0, 2);
	settings.RtContactScale = std::isfinite(settings.RtContactScale) ? std::clamp(settings.RtContactScale, 0.0f, 32.0f) : 8.0f;
	settings.RayOffsetScale = std::isfinite(settings.RayOffsetScale) ? std::clamp(settings.RayOffsetScale, 0.25f, 2.0f) : 1.0f;
	settings.AlphaCasters = settings.Foliage != 0 || settings.Cutouts;  // written back for older builds only
	settings.KeepScreenSpaceShadows = settings.ScreenSpaceShadows == static_cast<int>(SSSMode::Keep);
	// (45h) Before the first save is loaded (no frame runs during loading screens): its textures are
	// then created shareable when ray-traced foliage / cut-outs are on.
	RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.ShareGameTextures && (settings.Foliage != 0 || settings.Cutouts));
}

void RayTracing::SaveSettings(json& o_json)
{
	settings.AlphaCasters = settings.Foliage != 0 || settings.Cutouts;
	settings.KeepScreenSpaceShadows = settings.ScreenSpaceShadows == static_cast<int>(SSSMode::Keep);
	o_json = settings;
}

void RayTracing::RestoreDefaultSettings()
{
	settings = {};
	debugView = DebugView::Off;
}

void RayTracing::DataLoaded()
{
	if (!loadingFlushRegistered) {
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->AddEventSink<RE::MenuOpenCloseEvent>(&g_loadingSink);
			loadingFlushRegistered = true;
		}
	}
}

void RayTracing::SetupResources()
{
	combineCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CombineCB>());

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
	// (45k) Denoiser Penumbra / (45l) Temporal: created on first use (render-target size of the shadow
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

bool RayTracing::EnsureD3D11Resources(const D3D11_TEXTURE2D_DESC& a_maskDesc)
{
	if (maskTemp && a_maskDesc.Width == maskDesc.Width && a_maskDesc.Height == maskDesc.Height && a_maskDesc.Format == maskDesc.Format)
		return true;
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

	D3D11_TEXTURE2D_DESC debugDesc = desc;
	debugDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	debugDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	if (FAILED(device->CreateTexture2D(&debugDesc, nullptr, debugColor.put())) ||
		FAILED(device->CreateUnorderedAccessView(debugColor.get(), nullptr, debugColorUAV.put())) ||
		FAILED(device->CreateShaderResourceView(debugColor.get(), nullptr, debugColorSRV.put()))) {
		Disable(Status::Unavailable, "could not create the debug texture");
		return false;
	}
	Util::SetResourceName(debugColor.get(), "RayTracing DebugColor");

	maskDesc = a_maskDesc;
	d3d11Bytes = static_cast<std::uint64_t>(desc.Width) * desc.Height * (4 + 4);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------------------------

void RayTracing::LatchFrame()
{
	// Decided before the engine draws the sun cascades. The covered casters leave the cascades
	// only while the trace ran last frame (so it is running), never while the disagreement view
	// compares against a complete shadow map, and never with the A/B switch.
	// (45e) Also only when this frame's trace is certain to run: a D3D12 command context is free
	// now (it stays free until the trace is recorded), the scene has built cells, and the sky /
	// exterior conditions RunFrame checks hold. Otherwise the shadow map keeps every caster and a
	// frame that cannot trace falls back to it cleanly.
	auto* sky = globals::game::sky;
	// (45k) Not during a loading screen or the warm-up after it (nor while the RT result still fades in).
	const bool active = settings.Enabled && status == Status::Active && !settings.KeepCastersInShadowMap && phase == Phase::Play && fade >= 1.0f &&
	                    debugView != DebugView::Disagreement && !Util::IsInterior() && sky && sky->mode.get() == RE::Sky::Mode::kFull &&
	                    backend.HasFreeContext() && scene.HasCoverage();
	// (45f) Watch for big casters the scene lacks while Ray Tracing runs (also when nothing is left out).
	const bool watch = settings.Enabled && status == Status::Active;
	RayTracingCasterFilter::Latch(active, filterRect, watch);
	filterLatched = RayTracingCasterFilter::IsLatched();
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
	// (45h) Diffuse textures loaded from now on are created shareable only while both switches are on.
	RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.ShareGameTextures && (settings.Foliage != 0 || settings.Cutouts));

	if (!settings.Enabled) {
		if (sssClaimed)
			globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
		sssClaimed = false;
		phase = Phase::Play;
		fade = 1.0f;
		if (backend.IsReady())
			backend.WaitForTrace();  // (45l) a deferred wait left over (GPU-side only)
		if (status != Status::Off) {
			// Switched off: the scene goes (BLASes retire on the GPU's schedule); the device stays.
			RayTracingCasterFilter::Clear();
			characters.ReleaseAll(backend);  // (45i)
			if (backend.IsReady())
				scene.ReleaseAll(backend);
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
		return;
	}
	if (status == Status::Unavailable || status == Status::Failed) {
		scene.Housekeeping();
		if (sssClaimed)
			globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
		sssClaimed = false;
		return;
	}

	cpuLast = {};
	UpdatePhase();
	try {
		if (phase == Phase::Loading && settings.LoadingBoost && backend.IsReady()) {
			// (45k) Behind the loading screen: capture and build with a large budget, trace nothing. The
			// render thread never waits on the GPU; one build submission per frame, capped.
			const std::int64_t loadStart = Qpc();
			backend.WaitForTrace();
			scene.Update(backend, overVramBudget ? 0.0f : kLoadingCaptureBudgetMs, globals::state->frameCount);
			backend.BuildOnly(kLoadingBuildTriangles, kLoadingMaxBuilds);
			backend.Tick();
			cpuLast.scene = static_cast<float>(static_cast<double>(Qpc() - loadStart) * QpcMs());
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
		// (45l) Late Sync off: the D3D11 side of the trace right away (45k's order).
		if (pending.active && !settings.LateSync)
			FinishFrame();
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	if (pending.active) {
		// (45l) Late Sync: the rest after every other feature's prepass (FinishPrepass).
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
	traceSample.cpuMs = lastCpuMs;
	traceSample.gpuMs = activeThisFrame ? stats.lastGpuMs : -1.0f;
	traceSample.builds = activeThisFrame ? stats.buildsLastFrame : 0;
	traceSample.instances = activeThisFrame ? stats.instancesLastFrame : 0;
	Util::FrameTrace::OnRayTracing(traceSample.gpuMs, traceSample.cpuMs, traceSample.builds);
	PublishCpuSplit();
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
		detail.tileGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingDenoise);
		detail.filterGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingDenoiseFilter);
		detail.sigmaGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingSigma);
		detail.combineGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingCombine);
		detail.syncGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingSync);
		float denoise = -1.0f;
		for (const float part : { detail.tileGpuMs, detail.filterGpuMs, detail.sigmaGpuMs })
			if (part >= 0.0f)
				denoise = std::max(denoise, 0.0f) + part;
		detail.denoiseGpuMs = denoise;
		if (activeThisFrame) {
			detail.prepGpuMs = stats.lastPrepGpuMs;
			detail.traceGpuMs = stats.lastTraceGpuMs;
		}
		detail.quality = settings.QualityLevel;
		detail.denoiserEffective = static_cast<int>(lastEffectiveDenoiser);
		detail.skinDraws = backend.GetSkinStats().drawsLastFrame;
		detail.skinReused = backend.GetSkinStats().reusedLastFrame;
	}
	detail.sssGpuMs = Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::ScreenSpaceShadows);
	// (45k) A slow D3D12 record + submit names its slowest call in the log (at most every 2 s).
	if (activeThisFrame && cpuLast.execute > 4.0f) {
		static std::int64_t lastSpikeLog = 0;
		const std::int64_t now = Qpc();
		if (static_cast<double>(now - lastSpikeLog) * QpcMs() > 2000.0) {
			lastSpikeLog = now;
			const auto& x = stats.split;
			const std::pair<const char*, float> parts[] = { { "context choice", x.collect }, { "D3D11 Signal", x.signal11 }, { "D3D11 Flush", x.flush11 },
				{ "D3D12 queue Wait", x.wait12 }, { "allocator/list Reset", x.reset }, { "BLAS creation", stats.cpuBuildMs }, { "characters", stats.cpuSkinMs },
				{ "TLAS instances", x.tlas }, { "alpha records", x.alpha }, { "recording", x.record }, { "ExecuteCommandLists", x.execute },
				{ "D3D12 Signal", x.signal12 }, { "D3D11 Wait", x.wait11 } };
			const auto* worst = &parts[0];
			for (const auto& part : parts)
				if (part.second > worst->second)
					worst = &part;
			logger::info("[RayTracing] D3D12 record + submit took {:.1f} ms; slowest: {} {:.1f} ms (Signal11 {:.2f}, Flush11 {:.2f}, Wait12 {:.2f}, Execute {:.2f}, "
						 "Signal12 {:.2f}, Wait11 {:.2f}, builds {:.2f}, characters {:.2f})",
				cpuLast.execute, worst->first, worst->second, x.signal11, x.flush11, x.wait12, x.execute, x.signal12, x.wait11, stats.cpuBuildMs, stats.cpuSkinMs);
		}
	}
	Util::FrameTrace::OnRayTracingDetail(detail);
}

void RayTracing::RunFrame()
{
	if (!EnsureBackend())
		return;
	// (45l) A deferred D3D11 wait that was never issued (a frame without FinishPrepass): before this
	// frame's depth copy overwrites what the last trace read.
	backend.WaitForTrace();
	UpdateAlphaCasters();  // (45h, 45j) per-kind choices: a change re-captures the scene
	UpdateCharacters();                                // (45i)
	if (status == Status::Off || status == Status::Unavailable)
		status = Status::Waiting;

	auto* sky = globals::game::sky;
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
	scene.Update(backend, overVramBudget ? 0.0f : kCaptureBudgetMs, globals::state->frameCount);

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
	const bool penumbra = denoiser == DenoiserMode::Penumbra;
	const bool temporalMode = denoiser == DenoiserMode::Temporal;
	const bool rrMode = denoiser == DenoiserMode::DLSSRR;
	bool sigmaMode = denoiser == DenoiserMode::Sigma;
	if (sigmaMode && !EnsureSigma())
		sigmaMode = false;
	// (45f) TLAS self-check twice a second or so; missed casters noted by the shadow map are described.
	const bool validate = frameIndex % 30 == 0;
	scene.CollectInstances(instances, validate);
	scene.DescribeMissedCasters();
	// (45i) Characters in the traced area: capture, bone matrices, draws (none with the switch off).
	// (45l) Other actors at reduced quality (45m: skinned every frame unless the Debug switch asks for 45l).
	const std::int64_t charactersStart = Qpc();
	skinDraws.clear();
	skinBones.clear();
	if (settings.Characters && backend.SkinAvailable()) {
		// (45m) Other actors are skinned and refit every frame again (the 45l alternate frames lagged visibly);
		// reduced quality now only lowers their ray work. The 45l path stays as a Debug switch.
		characters.SetNpcReduced(quality.npcReduced && settings.NpcAlternateFrameSkinning);
		float world[4];
		if (scene.GetLoadedRect(world, false))
			characters.Update(backend, globals::state->frameCount, world);
		// Every captured (and retiring) shape is traced, also on a frame without an update.
		characters.CollectDraws(skinDraws, skinBones);
	}
	cpuLast.characters = static_cast<float>(static_cast<double>(Qpc() - charactersStart) * QpcMs());

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
	// (45k, 45l) Noise / Adaptive Rays, Auto: world-locked noise and no extra rays only with Off; animated noise
	// (R2-stepped) and extra rays with every denoiser that averages frames. DLSS RR and SIGMA need fresh noise
	// every real frame; SIGMA takes one ray per pixel (its input is one shadow ray's penumbra).
	bool worldNoise = settings.Noise == static_cast<int>(AutoMode::Auto) ? denoiser == DenoiserMode::Off : settings.Noise == static_cast<int>(AutoMode::First);
	if (rrMode || sigmaMode)
		worldNoise = false;
	bool adaptive = settings.AdaptiveRays == static_cast<int>(AutoMode::Auto) ? denoiser != DenoiserMode::Off : settings.AdaptiveRays == static_cast<int>(AutoMode::First);
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
	if (settings.StableNormals)
		traceFlags |= kFlagStableNormal;
	if (settings.FineAlphaMips)
		traceFlags |= kFlagAlphaFootprint;
	if (adaptive && quality.extraRays > 0)
		traceFlags |= kFlagAdaptive;
	const bool blockerDist = (penumbra || sigmaMode) && !(traceFlags & kFlagCoverage);
	if (blockerDist)
		traceFlags |= kFlagBlockerDist;
	// (45l) Closest hit only at the edges (Penumbra with extra rays); SIGMA keeps it on the first ray everywhere.
	if (blockerDist && penumbra && settings.BlockerAtEdges && (traceFlags & kFlagAdaptive))
		traceFlags |= kFlagBlockerEdges;
	const bool falloff = quality.falloffEnd > quality.falloffStart;
	if (falloff)
		traceFlags |= kFlagFalloff;
	// (45l) The character ray only where the sun ray passes a character's bounds.
	if (settings.CharacterRayCulling && !skinDraws.empty()) {
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
	// (45k) Debug: Ray Start Offset scales the 45j bias (2 units + 0.0015 per unit of distance).
	const float biasConstant = 2.0f * settings.RayOffsetScale, biasPerUnit = 0.0015f * settings.RayOffsetScale;
	input.constants.bias[0] = biasConstant;
	input.constants.bias[1] = biasPerUnit;
	input.constants.bias[2] = 8.0f;
	input.constants.bias[3] = skinDraws.empty() ? 0.0f : settings.CharacterSelfShadowOffset;  // (45i) character ray TMin (character variants only)
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
	input.skinBones = &skinBones;
	input.splitSubmit = settings.SplitSubmit;  // (45l)
	input.deferWait = true;                    // (45l) FinishFrame waits (right away without Late Sync)

	auto* context = globals::d3d::context;
	auto* screenSpaceShadows = &globals::features::screenSpaceShadows;
	// (45j) Screen Space Shadows inside the traced area: combined with min() or kept. (45l) Hand off is gone.
	const bool sssAvailable = typedUavLoadR8 && screenSpaceShadows->loaded && screenSpaceShadows->bendSettings.Enable && screenSpaceShadows->screenSpaceShadowsTexture;
	const auto sssMode = static_cast<SSSMode>(std::clamp(settings.ScreenSpaceShadows, 0, 3));
	// (45k) Non-RT casters only: the texture then holds only non-traced casters (and traced ones within
	// their contact range): merged with min() like Combine, so shared shadows are not darkened twice.
	const bool combineSSS = sssAvailable && (sssMode == SSSMode::Combine || sssMode == SSSMode::NonRT);
	// (45l) Temporal uses Penumbra's clamped history (History4); Penumbra adds its spatial filter.
	const bool historyStage = (penumbra || temporalMode) && EnsureDenoiser(quality.denoiseHalf && penumbra);
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
	if (historyStage) {
		combineFlags |= kFlagPenumbra | (history4Valid && prevCameraValid ? kFlagHistoryValid : 0u);
		if (penumbra)
			combineFlags |= kFlagSpatial | (quality.denoiseHalf ? kFlagDenoiseHalf : 0u) | (falloff ? kFlagFalloffDenoise : 0u);
	}
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
	backend.SetExplicitFlush(settings.FlushBeforeTrace);  // (45k)
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

	// (45l) The D3D11 side of this frame: FinishFrame (now, or after the other prepasses with Late Sync).
	pending.active = true;
	std::memcpy(pending.cbBytes.data(), &cb, sizeof(cb));
	pending.combineFlags = combineFlags;
	pending.width = width;
	pending.height = height;
	pending.writeSSS = combineSSS;
	pending.debug = debug;
	pending.penumbra = historyStage;
	pending.spatial = historyStage && penumbra;
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
	timers->Begin(Util::GpuBucket::RayTracingSync);
	backend.WaitForTrace();
	timers->End(Util::GpuBucket::RayTracingSync);
	if (backend.HasFailed() || !mask.texture || !mask.SRV || !maskTemp) {
		if (backend.HasFailed())
			Disable(Status::Failed, backend.LastError());
		history4Valid = false;
		sigmaHistoryValid = false;
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
	if (!settings.Enabled || debugView == DebugView::Off || !combinedThisFrame || lastOverlayFrame + 1 != frameIndex)
		return;
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
// Page
// ---------------------------------------------------------------------------------------------

void RayTracing::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Ray Tracing", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: sun shadows of buildings, rocks, furniture, doors, items and characters inside the loaded area are ray traced.");
			ImGui::Text("Trees, leaves, hair (unless changed under Advanced), grass and the terrain itself (hills) keep their normal shadows; the ground still receives traced shadows.");
			ImGui::Text("Screen Space Shadows are switched off where ray tracing is in charge.");
			ImGui::Text("Off: the game's shadows exactly as before (nothing of this feature runs).");
		}

		// (45l) Five tiers + Custom; picking Custom starts from the tier that was selected.
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
			ImGui::Text("Medium / High / Ultra are 45k's Low / Medium / High under new names (a 45k Low save now reads Medium).");
		}
		if (settings.QualityLevel == static_cast<int>(Quality::Custom) && ImGui::TreeNodeEx("Custom Quality", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::SliderInt("Rays per Pixel", &settings.CustomBaseRays, 1, 4);
			ImGui::SliderInt("Extra Rays at Shadow Edges", &settings.CustomExtraRays, 0, 8);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Adaptive rays: only where a shadow edge is. 0 = none.");
			ImGui::SliderFloat("Sun Softness", &settings.CustomSunCone, 0.1f, 2.0f, "%.2f deg");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Half angle of the sun disc the rays aim at: 0.27 = the real sun (sharp), larger = softer, wider shadow edges.");
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
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("The player always gets full quality. Reduced: other characters get no extra edge rays.");
				ImGui::Text("  They are re-posed every frame like the player (45m; the 45l every-other-frame mode is under Debug).");
			}
			ImGui::SliderFloat("Denoiser Radius", &settings.CustomDenoiseRadius, 2.0f, 32.0f, "%.0f px");
			ImGui::SliderInt("Denoiser Taps per Side", &settings.CustomDenoiseTaps, 1, 8);
			ImGui::Checkbox("Denoiser at Half Resolution", &settings.CustomDenoiseHalf);
			ImGui::SliderInt("History Length", &settings.CustomHistory, 1, 32, "%d frames");
			ImGui::SliderFloat("History Clamp", &settings.CustomDenoiseClamp, 0.5f, 4.0f, "%.2f sigma");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How far the history may differ from this frame's neighbourhood: smaller = less ghosting, more grain.");
			ImGui::TreePop();
		}

		// (45j) Advanced: per kind of object, ray traced or the shadow map + screen-space shadows.
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
				"Also the big roof tiles and plank edges taken as solid (Debug: Treat Mostly-Opaque ...).");
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
					ImGui::Text("  Ray traced / Both: cut-outs are read from 256x256 copies (Debug: Share Game Textures for full resolution).");
				}
			}
			row("Cut-outs", settings.Cutouts, "Fences, grilles, banners and other non-plant cut-outs (default: Shadow map).",
				"Ray traced: traced with their cut-out (the holes stay lit).");
			row("Characters", settings.Characters, "The player and nearby NPCs and creatures: body, clothes, armour, weapons (default: Ray traced).",
				"Up to 16 characters within 40 m inside the ray-traced area, posed every frame.");
			{
				ImGui::Indent();
				if (!settings.Characters)
					ImGui::BeginDisabled();
				row("Physics parts", settings.PhysicsParts, "Capes, skirts and other HDT-SMP / HDT-PE parts, not hair (default: Ray traced).", nullptr);
				row("Hair", settings.Hair, "Hair and wigs (default: Shadow map: the hair shader's own shadowing looks better than solid cards).",
					"Recognised by the hair tint material, or 'hair' in the shape or texture name.");
				ImGui::Checkbox("Player in first person", &settings.FirstPersonPlayer);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Off (default): in first person the player keeps the shadow-map shadow.");
					ImGui::Text("  Camera mods hide or fold the third-person arms outside the shadow pass (weapons drawn), so a traced");
					ImGui::Text("  player shadow lost its arms. On: the player is ray traced in first person too.");
				}
				if (!settings.Characters)
					ImGui::EndDisabled();
				ImGui::Unindent();
			}
			ImGui::Separator();
			{
				// (45l) Hand off (45i) is gone: "Non-RT casters only" with contact length 0 is what it was meant to be
				// (no screen-space shadow from ray-traced casters, every other caster keeps its own).
				static constexpr SSSMode kSssOrder[] = { SSSMode::NonRT, SSSMode::Combine, SSSMode::Keep };
				const char* sssModes[] = { "Non-RT casters only", "Combine (min)", "Keep (multiply)" };
				int sssIndex = 0;
				for (int i = 0; i < 3; ++i)
					if (settings.ScreenSpaceShadows == static_cast<int>(kSssOrder[i]))
						sssIndex = i;
				if (ImGui::Combo("Screen-space shadows", &sssIndex, sssModes, 3))
					settings.ScreenSpaceShadows = static_cast<int>(kSssOrder[std::clamp(sssIndex, 0, 2)]);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Screen Space Shadows (small contact shadows) inside the ray-traced area:");
					ImGui::Text("  Non-RT casters only (default): everything the ray tracer does not see (grass, distant trees, shadow-map kinds)");
					ImGui::Text("    keeps its contact shadows; ray-traced casters only fill the few units right at their base that the");
					ImGui::Text("    ray tracer cannot resolve (RT Caster Contact Length; 0 = none at all, what 45k's Hand off was meant to be).");
					ImGui::Text("  Combine: kept; where they and the ray tracer agree the shadow is not darkened twice.");
					ImGui::Text("  Keep: kept and multiplied as with Ray Tracing off (shared shadows get a little darker).");
				}
				if (settings.ScreenSpaceShadows == static_cast<int>(SSSMode::NonRT)) {
					ImGui::Indent();
					ImGui::SliderFloat("RT Caster Contact Length", &settings.RtContactScale, 0.0f, 32.0f, "%.1f x ray offset");
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("How far (along the sun ray) a ray-traced object may still cast a screen-space contact shadow, in multiples of");
						ImGui::Text("  the ray tracer's start offset (2 units + 0.15 %% of the distance). Default 8 = about 16 units near the camera.");
						ImGui::Text("  0 = ray-traced objects cast no screen-space shadow at all. Larger = softer bases, but thin posts get fatter again.");
					}
					ImGui::Unindent();
				}
			}
			ImGui::TreePop();
		}

		// (45k) Denoiser: one dropdown to compare quality and cost in one session.
		{
			const char* denoisers[] = { "Off", "Temporal", "Penumbra", "DLSS RR", "NRD SIGMA" };
			static_assert(IM_ARRAYSIZE(denoisers) == static_cast<int>(DenoiserMode::Count));
			settings.Denoiser = std::clamp(settings.Denoiser, 0, static_cast<int>(DenoiserMode::Count) - 1);
			const bool rrActive = UpscalingRRActive();
			if (ImGui::Combo("Denoiser", &settings.Denoiser, denoisers, IM_ARRAYSIZE(denoisers)) && settings.Denoiser == static_cast<int>(DenoiserMode::DLSSRR)) {
				// (45l) DLSS RR here switches Ray Reconstruction on in Upscaling (the same setting as its checkbox; Upscaling
				// rebuilds its resources on its next frame). Switching away never turns it off.
				auto& upscaling = globals::features::upscaling;
				if (upscaling.loaded && Upscaling::streamline.featureDLSS_RR && !upscaling.settings.enableDLSSRR) {
					upscaling.settings.enableDLSSRR = true;
					logger::info("[RayTracing] Denoiser DLSS RR: Ray Reconstruction switched on in Upscaling");
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How the grainy soft edges of the traced shadows are cleaned up (cost: F7 trace columns rt_*_gpu_ms).");
				ImGui::Text("  Off: only a small blur.");
				ImGui::Text("  Temporal: the small blur, then the last ~12 frames blended (Penumbra's history, without its edge blur). Cheap.");
				ImGui::Text("  Penumbra (default): blurs each soft edge as wide as it really is (wide under tall trees, none at");
				ImGui::Text("    contact shadows), then blends the last ~12 frames; extra rays only at shadow edges.");
				ImGui::Text("  DLSS RR: no cleanup of ours; fresh noise every frame for DLSS Ray Reconstruction to clean up.");
				ImGui::Text("    Picking it switches Ray Reconstruction on in Upscaling. While Ray Reconstruction is on, the shadows always");
				ImGui::Text("    take this route, whatever is picked here (RR cleans only raw noise).");
				ImGui::Text("  NRD SIGMA: NVIDIA's shadow denoiser (one ray per pixel, its own blur and history).");
			}
			auto& upscaling = globals::features::upscaling;
			if (rrActive && settings.Denoiser != static_cast<int>(DenoiserMode::DLSSRR)) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.5f, 0.85f, 1.0f, 1.0f });
				ImGui::TextWrapped("Following Upscaling: Ray Reconstruction is on, so the shadows use the DLSS RR route (%s applies again when RR is off).",
					denoisers[settings.Denoiser]);
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
			const char* noiseModes[] = { "Auto", "World-locked", "Animated" };
			const char* adaptiveModes[] = { "Auto", "On", "Off" };
			ImGui::Indent();
			settings.Noise = std::clamp(settings.Noise, 0, 2);
			const DenoiserMode effective = EffectiveDenoiser();
			const bool noiseFixed = effective == DenoiserMode::DLSSRR || effective == DenoiserMode::Sigma;
			if (noiseFixed)
				ImGui::BeginDisabled();
			ImGui::Combo("Noise", &settings.Noise, noiseModes, 3);
			if (noiseFixed)
				ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Auto: Animated with every denoiser that averages frames, World-locked with Off.");
				ImGui::Text("World-locked: each spot of the world keeps its rays while the camera moves (frozen grain; nothing for a history to average).");
				ImGui::Text("Animated: new rays every frame (the history averages them). DLSS RR and NRD SIGMA always use Animated.");
			}
			settings.AdaptiveRays = std::clamp(settings.AdaptiveRays, 0, 2);
			ImGui::Combo("Adaptive Rays", &settings.AdaptiveRays, adaptiveModes, 3);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Extra rays only where a shadow edge is (by quality: Very Low / Low none, Medium +1, High +3, Ultra +4).");
				ImGui::Text("Auto: on with every denoiser but Off. NRD SIGMA always traces one ray per pixel.");
			}
			ImGui::Unindent();
		}
		RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.ShareGameTextures && (settings.Foliage != 0 || settings.Cutouts));  // also from the main menu
		if (!alphaOffReason.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.4f, 0.3f, 1.0f });
			ImGui::TextWrapped("Ray-traced foliage / cut-outs were switched off automatically: %s", alphaOffReason.c_str());
			ImGui::PopStyleColor();
		}
		if (!charactersOffReason.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.4f, 0.3f, 1.0f });
			ImGui::TextWrapped("Characters was switched off automatically: %s", charactersOffReason.c_str());
			ImGui::PopStyleColor();
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
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Debug")) {
		const char* views[] = { "Off", "Shadow Source", "RT vs Shadow Map", "BLAS Coverage", "Raw RT Visibility", "RT Hit vs Depth", "Penumbra Radius",
			"History Length", "Adaptive Rays", "SSS: RT Casters", "Distance Quality", "Character Rays" };
		static_assert(IM_ARRAYSIZE(views) == static_cast<int>(DebugView::Count));
		int view = static_cast<int>(debugView);
		if (ImGui::Combo("Debug View", &view, views, IM_ARRAYSIZE(views)))
			debugView = static_cast<DebugView>(std::clamp(view, 0, static_cast<int>(DebugView::Count) - 1));
		if (auto _tt = Util::HoverTooltipWrapper()) {
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
			ImGui::Text("Distance Quality (45l): green = full quality, towards red = fewer extra rays and a smaller blur with distance,");
			ImGui::Text("  blue = pixels not traced this frame (checkerboard / half pattern), filled from their neighbours.");
			ImGui::Text("Character Rays (45l): cyan = the extra ray against characters ran here, purple = a character shadows it.");
		}
		ImGui::Checkbox("Keep Static Casters in the Shadow Map", &settings.KeepCastersInShadowMap);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("A/B: normally objects the ray tracer covers are left out of the sun shadow map. On = they stay (the darker of both wins).");
		ImGui::Checkbox("Treat Mostly-Opaque Alpha-Tested Shapes as Opaque", &settings.CutoutAsOpaque);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: big one-sided shapes whose cut-out only trims their edges (roof tiles, plank ends, stall canopies) block the sun");
			ImGui::Text("  in the ray tracer as if solid. Two-sided cut-outs (banners, leaves, fences with holes) and small shapes stay out.");
			ImGui::Text("Off: every alpha-tested shape keeps its shadow-map shadow (magenta in BLAS Coverage). Switching re-captures the scene.");
		}
		// (45i)
		ImGui::Checkbox("Keep Characters in the Shadow Map", &settings.KeepCharactersInShadowMap);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("A/B: normally a ray-traced character part is left out of the sun shadow map.");
			ImGui::Text("On = it stays there too (the darker of both wins): a safety net if a character shadow goes missing.");
		}
		ImGui::SliderFloat("Character Self-Shadow Offset", &settings.CharacterSelfShadowOffset, 0.0f, 16.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Character surfaces nearer than this along a sun ray are ignored. Larger = less blocky self-shadowing");
			ImGui::Text("  (shadow 'acne' on the body where it turns away from the sun), smaller = tighter contact shadows (feet, arms).");
		}
		// (45j)
		// (45k)
		ImGui::SliderFloat("Ray Start Offset", &settings.RayOffsetScale, 0.25f, 2.0f, "%.2f x");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Sun rays start this far off the surface: 1.0 = 2 units + 0.15 %% of the distance (45j). Smaller = contact shadows");
			ImGui::Text("  reach closer to the caster's base (low curbs, steps), but flat ground may start to show speckles (shadow acne).");
		}
		ImGui::Checkbox("Flush D3D11 Before the Trace", &settings.FlushBeforeTrace);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("45j flushed the game's D3D11 queue explicitly before every trace. Off (default): the fence Signal alone submits it.");
			ImGui::Text("  A/B for the D3D12 record + submit spikes: the F7 trace columns rt_x_* show which call waits.");
		}
		// (45l) Cost and sync A/B (each off = 45k).
		ImGui::Checkbox("Late Sync", &settings.LateSync);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: the game's GPU queue waits for the ray-traced shadows only after every other feature's preparation passes,");
			ImGui::Text("  so that work runs while the trace runs. Off = 45k (it waited right after the trace was sent). Trace: rt_sync_gpu_ms.");
		}
		ImGui::Checkbox("Other Characters Skinned Every Other Frame (45l)", &settings.NpcAlternateFrameSkinning);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On = 45l: with other characters at reduced quality, their skinning and BLAS refit run every other frame (one frame late).");
			ImGui::Text("  Off (default, 45m): every frame, like the player; reduced quality then only means fewer rays for them.");
		}
		ImGui::Checkbox("Split D3D12 Submission", &settings.SplitSubmit);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("On: BLAS builds, character skinning and the TLAS start at once; only the trace waits for the game's depth. Off = 45k.");
		ImGui::Checkbox("Character Ray Culling", &settings.CharacterRayCulling);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: the extra ray against characters is only traced where the sun ray passes near a character (debug view Character Rays).");
			ImGui::Text("  Off = 45k: traced for every pixel while any character is traced.");
		}
		ImGui::Checkbox("Blocker Distance at Edges Only", &settings.BlockerAtEdges);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Penumbra: the costly 'nearest blocker' search (for the blur width) only at shadow edges, with the first extra ray.");
			ImGui::Text("  Off = 45k: on every pixel's first ray.");
		}
		ImGui::Checkbox("Distance Fall-off", &settings.DistanceFalloff);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("On: the quality tier's fall-off with distance (fewer edge rays, smaller blur, the far pattern). Off: full quality at any distance.");
		ImGui::Checkbox("Build Behind Loading Screens", &settings.LoadingBoost);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("On: while a loading screen is up, cells are captured and built with a large budget (nothing is traced there).");
		ImGui::Checkbox("Warm-Up After Loading Screens", &settings.WarmUp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: after a loading screen the shadow map keeps every caster until the nearby scene is built (5 s at most),");
			ImGui::Text("  and the ray-traced shadows fade in over half a second. Off: as 45j.");
		}
		if (!lastLoadingSummary.empty())
			ImGui::TextWrapped("Last loading screen: %s", lastLoadingSummary.c_str());
		ImGui::Checkbox("Stable Normals at Depth Edges", &settings.StableNormals);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("On: thin edges (leaves, blades, branch silhouettes) use the view direction as normal instead of a depth normal that flips every frame.");
		ImGui::Checkbox("Fine Cut-out Mips", &settings.FineAlphaMips);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: ray-traced cut-outs are read at a detail that depends only on the distance to the hit (not on the camera),");
			ImGui::Text("  never below 256 texels, with the alpha threshold eased slightly at coarser mips (fuller leaf shadows). Off = 45i.");
		}
		ImGui::Checkbox("Share Game Textures with the Ray Tracer", &settings.ShareGameTextures);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Ray-traced foliage / cut-outs: textures loaded while this is on are created shareable and read at full resolution.");
			ImGui::Text("  Off (default): every cut-out is read from a 256x256 copy (slightly softer edges).");
			ImGui::Text("  Suspect of the 45i frame-pacing change (the GPU wait moving from Present into a Map); test it with an F7 trace.");
			ImGui::Text("  Takes effect for textures loaded afterwards (restart or load a save).");
		}
		if (!typedUavLoadR8)
			ImGui::TextDisabled("This GPU cannot rewrite Screen Space Shadows; they are kept as with Ray Tracing off.");

		const auto& rt = backend.GetStats();
		const auto& sc = scene.GetStats();
		const auto& skip = sc.skipped;
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
				ImGui::Text("  unless Alpha-tested casters is on.");
			}
		}
		{
			// (45h) Alpha-tested casters.
			const auto& al = sc.alpha;
			const auto& ab = backend.GetAlphaStats();
			const auto share = RayTracingTextureSharing::GetCounts();
			ImGui::Text("Alpha-tested: %u shapes traced with their cut-out (%u captured, %u meshes, %.2f M triangles, %u instances last frame)",
				al.registered, al.captured, al.meshes, static_cast<double>(al.meshTriangles) / 1e6, ab.instancesLastFrame);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Captured shapes wait until their BLAS is built and their texture is ready; then they leave the shadow map.");
				ImGui::Text("Cyan in BLAS Coverage = an alpha-tested surface the ray tracer sees with its cut-out.");
			}
			ImGui::Text("  Textures: %u shared at full resolution, %u as 256x256 copies (%u being made)%s", ab.texturesShared, ab.texturesFallback,
				ab.texturesPending, ab.sharedPathOff ? " -- sharing gave up this session" : "");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Shared: the game's own texture, read directly (no copy, no extra VRAM). Copy: textures loaded before the switch");
				ImGui::Text("  was on, or not shareable; a 256x256 alpha copy is made (slightly softer cut-out). A restart / load with the");
				ImGui::Text("  switch on shares more of them.");
				ImGui::Text("Not shareable %llu, D3D12 could not open %llu, bindless table full %llu.", static_cast<unsigned long long>(ab.notShareable),
					static_cast<unsigned long long>(ab.openFailed), static_cast<unsigned long long>(ab.slotsFull));
			}
			ImGui::Text("  Texture loads made shareable: %llu of %llu diffuse loads (%llu refused by the driver%s)", static_cast<unsigned long long>(share.shared),
				static_cast<unsigned long long>(share.diffuse), static_cast<unsigned long long>(share.refused), share.gaveUp ? ", gave up" : "");
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
			// (45i) Characters.
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
		const auto& alphaBytes = backend.GetAlphaStats();
		const std::uint64_t vram = rt.blasBytes + rt.scratchBytes + rt.tlasBytes + rt.uploadBytes + rt.sharedTextureBytes + d3d11Bytes +
		                           alphaBytes.uvBytes + alphaBytes.layerBytes + alphaBytes.infoBytes +
		                           backend.GetSkinStats().bytes + backend.GetSkinStats().scratchBytes + backend.GetSkinStats().boneBytes;  // (45i)
		ImGui::Text("VRAM: %s total (BLAS %s, scratch %s, TLAS %s, uploads %s, textures %s)", Megabytes(vram).c_str(),
			Megabytes(rt.blasBytes).c_str(), Megabytes(rt.scratchBytes).c_str(), Megabytes(rt.tlasBytes).c_str(),
			Megabytes(rt.uploadBytes).c_str(), Megabytes(rt.sharedTextureBytes + d3d11Bytes).c_str());
		if (overVramBudget)
			ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "  Over the 1.2 GB scene budget: no further cells are captured.");
		if (rt.compactedSavedBytes)
			ImGui::Text("  Compaction saved %s so far", Megabytes(rt.compactedSavedBytes).c_str());
		const float frameMs = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracing);
		ImGui::Text("GPU: %.2f ms on the RT queue (builds + trace)%s", rt.smoothedGpuMs,
			frameMs > 0.0f ? std::format(", {:.2f} ms in the frame (Performance Overlay row)", frameMs).c_str() : "");
		{
			// (45l) Last samples per pass (the GPU timers run while the overlay table or an F7 trace is on).
			auto* timers = Util::GpuPassTimers::GetSingleton();
			auto ms = [&](Util::GpuBucket a_bucket) { return std::max(timers->LastSampleMs(a_bucket), 0.0f); };
			ImGui::Text("  last frame: builds/skinning/TLAS %.2f, trace %.2f; denoiser tiles %.2f, filter %.2f, SIGMA %.2f; combine %.2f; game queue waiting %.2f ms",
				std::max(rt.lastPrepGpuMs, 0.0f), std::max(rt.lastTraceGpuMs, 0.0f), ms(Util::GpuBucket::RayTracingDenoise), ms(Util::GpuBucket::RayTracingDenoiseFilter),
				ms(Util::GpuBucket::RayTracingSigma), ms(Util::GpuBucket::RayTracingCombine), ms(Util::GpuBucket::RayTracingSync));
			static constexpr const char* kDenoiserNames[] = { "Off", "Temporal", "Penumbra", "DLSS RR", "NRD SIGMA" };
			const auto& q = lastQuality;
			ImGui::Text("  in effect: denoiser %s, %u+%u rays, pattern %s / far %s, fall-off %.0f-%.0f, other characters %s (%u of %u shapes re-used)",
				kDenoiserNames[static_cast<int>(lastEffectiveDenoiser)], q.baseRays, q.extraRays, kPatternNames[std::min(q.tracePattern, 2u)],
				kPatternNames[std::min(q.farPattern, 2u)], q.falloffStart, q.falloffEnd, q.npcReduced ? "reduced" : "full", backend.GetSkinStats().reusedLastFrame,
				backend.GetSkinStats().drawsLastFrame);
		}
		ImGui::Text("CPU: %.2f ms this frame, capture %.2f ms (worst %.2f ms, last cell %.1f ms in total)", lastCpuMs, sc.lastGatherMs,
			sc.maxGatherMs, sc.lastCellMs);
		ImGui::SameLine();
		if (ImGui::SmallButton("Reset worst")) {
			scene.ResetPeaks();
			characters.ResetPeaks();
			cpuWorst = {};
		}
		{
			// (45j) Where the CPU goes (average / worst since reset); every frame of it is in the F7 trace (rt_* columns).
			const auto& a = cpuSmoothed;
			const auto& w = cpuWorst;
			ImGui::Text("  avg / worst ms: scene %.2f / %.1f, characters %.2f / %.1f, instances %.2f / %.1f, missed casters %.2f / %.1f", a.scene, w.scene,
				a.characters, w.characters, a.instances, w.instances, a.missed, w.missed);
			ImGui::Text("  D3D12 record + submit %.2f / %.1f, D3D11 passes %.2f / %.1f, end of frame %.2f / %.1f; engine objects waiting to be dropped %u",
				a.execute, w.execute, a.d3d11, w.d3d11, a.endFrame, w.endFrame, scene.GetFrameTimes().releasePending);
		}
		ImGui::Text("Shadow map: %u casters handed to RT, %u draws left out last frame%s", RayTracingCasterFilter::RegisteredCount(),
			RayTracingCasterFilter::SkippedLastFrame(), filterLatched ? "" : " (not active)");
		{
			// (45f) Self-checks.
			const auto& check = backend.GetTlasCheck();
			std::uint32_t mismatched = 0, notBuilt = 0;
			for (int c = 0; c < RayTracingRT::kInstanceCategories; ++c) {
				mismatched += check.mismatched[c];
				notBuilt += check.notBuilt[c];
			}
			ImGui::Text("TLAS: %u cell chunks, %u doors/activators, %u items, %u trees, %u alpha-tested, %u character parts (%u not built yet)",
				check.written[RayTracingRT::kCategoryCell], check.written[RayTracingRT::kCategoryMovable], check.written[RayTracingRT::kCategoryItem],
				check.written[RayTracingRT::kCategoryTree], check.written[RayTracingRT::kCategoryAlpha], check.written[RayTracingRT::kCategoryCharacter], notBuilt);
			const ImVec4 bad{ 1.0f, 0.5f, 0.3f, 1.0f }, good{ 0.6f, 0.9f, 0.6f, 1.0f };
			ImGui::TextColored(mismatched ? bad : good, "Self-check: %u of %u instances outside the game's own bounds (%llu so far, should be 0)", mismatched,
				check.checked, static_cast<unsigned long long>(check.mismatchedTotal));
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Each ray-traced instance, exactly as handed to the GPU, must lie inside the bounds the game computed for its shapes. Details in the log.");
			const std::uint32_t missed = sc.missedConfirmed;
			ImGui::TextColored(missed ? bad : good, "Missed casters: %u big shapes cast a shadow-map shadow inside the traced area but are not in the scene", missed);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Shapes with a bound radius of %.0f units or more drawn into the sun shadow map inside the traced area.", RayTracingCasterFilter::kMissedMinRadius);
				ImGui::Text("Counted once each, only after their cell finished capturing and their BLAS was built (%u were re-checked later).", sc.missedPending);
				ImGui::Text("Each distinct one is described in the log (8 per 10 s, 200 per session; characters and tree leaves are not).");
				ImGui::Text("A reference its cell did not have when captured gets one recapture first (%u requested so far).", sc.missedRecaptures);
			}
			// (45g) Why, by reason (this area).
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
	static constexpr const char* kSSS[] = { "combine", "hand off", "keep", "non-RT casters only" };
	static constexpr const char* kDenoisers[] = { "off", "temporal", "penumbra", "DLSS RR", "NRD SIGMA" };
	static constexpr const char* kAuto[3][3] = { { "auto", "world-locked", "animated" }, { "auto", "on", "off" }, { "", "", "" } };
	auto mode = [](bool a_rt) { return a_rt ? "RT" : "SM"; };
	auto onOff = [](bool a_on) { return a_on ? "on" : "off"; };
	const QualityParams q = ResolveQuality();
	std::string text = std::format("RT {} ({}), quality {}, buildings {}, objects {}, trees {}, foliage {}, cut-outs {}, characters {}, physics parts {}, hair {}, "
								   "first-person player {}, SSS {} (contact {:.1f}), denoiser {}, noise {}, adaptive rays {}, ray offset {:.2f}, D3D11 flush {}, loading build {}, warm-up {}",
		settings.Enabled ? "on" : "off", kStatus[static_cast<int>(status)], kQualityNames[std::clamp(settings.QualityLevel, 0, 5)], mode(settings.Buildings),
		mode(settings.Objects), mode(settings.Trees), kFoliage[std::clamp(settings.Foliage, 0, 2)], mode(settings.Cutouts), mode(settings.Characters),
		mode(settings.PhysicsParts), mode(settings.Hair), settings.FirstPersonPlayer ? "RT" : "SM", kSSS[std::clamp(settings.ScreenSpaceShadows, 0, 3)],
		settings.RtContactScale, kDenoisers[static_cast<int>(CurrentDenoiser())], kAuto[0][std::clamp(settings.Noise, 0, 2)], kAuto[1][std::clamp(settings.AdaptiveRays, 0, 2)],
		settings.RayOffsetScale, onOff(settings.FlushBeforeTrace), onOff(settings.LoadingBoost), onOff(settings.WarmUp));
	// (45l) What the tier sets, the denoiser in effect and the A/B switches.
	text += std::format(", denoiser in effect {}{}, rays {}+{}, sun cone {:.2f}, pattern {} far {}, fall-off {:.0f}-{:.0f}, other characters {}, denoise radius {:.0f} "
						"taps {} {} far scale {:.2f}, history {:.0f} clamp {:.2f}, SIGMA stabilisation {}, late sync {}, split submit {}, character ray culling {}, "
						"blocker at edges {}, distance fall-off {}",
		kDenoisers[static_cast<int>(EffectiveDenoiser())], UpscalingRRActive() ? " (Upscaling RR on)" : "", q.baseRays, q.extraRays, q.coneDegrees,
		kPatternNames[std::min(q.tracePattern, 2u)], kPatternNames[std::min(q.farPattern, 2u)], q.falloffStart, q.falloffEnd, q.npcReduced ? "reduced" : "full",
		q.denoiseRadius, q.denoiseTaps, q.denoiseHalf ? "half res" : "full res", q.farDenoiseScale, q.history, q.clampSigma, settings.SigmaStabilizedFrames,
		onOff(settings.LateSync), onOff(settings.SplitSubmit), onOff(settings.CharacterRayCulling), onOff(settings.BlockerAtEdges), onOff(settings.DistanceFalloff));
	return text;
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
	j["screen_space_shadows_mode"] = settings.ScreenSpaceShadows;
	j["denoiser"] = settings.Denoiser;
	j["noise"] = settings.Noise;
	j["adaptive_rays"] = settings.AdaptiveRays;
	j["rt_contact_scale"] = settings.RtContactScale;
	j["ray_offset_scale"] = settings.RayOffsetScale;
	j["flush_before_trace"] = settings.FlushBeforeTrace;
	j["denoiser_effective"] = static_cast<int>(EffectiveDenoiser());
	j["upscaling_rr_active"] = UpscalingRRActive();
	j["sigma_stabilized_frames"] = settings.SigmaStabilizedFrames;
	j["late_sync"] = settings.LateSync;
	j["split_submit"] = settings.SplitSubmit;
	j["character_ray_culling"] = settings.CharacterRayCulling;
	j["blocker_at_edges"] = settings.BlockerAtEdges;
	j["distance_falloff"] = settings.DistanceFalloff;
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
	return j;
}
