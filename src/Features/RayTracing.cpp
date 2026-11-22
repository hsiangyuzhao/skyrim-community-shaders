#include "RayTracing.h"

#include "Deferred.h"
#include "Features/RayTracing/CasterFilter.h"
#include "Features/RayTracing/Wind.h"
#include "Features/DynamicCubemaps.h"
#include "Features/ExponentialHeightFog.h"  // (49b) Particles Use RT Shadows needs its volumetric fog grid
#include "Features/GrassOptimizations.h"  // (49c) the loading-screen thread's grass uploads, the grass staging probe
#include "Features/LightLimitFix.h"  // (48g) the hit shading reads its light list
#include "Features/Skylighting.h"    // (48g) and its probes
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpacePointLightShadows.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/ScreenSpaceShadows.h"
#include "Features/Upscaling.h"
#include "State.h"
#include "Utils/FrameTrace.h"
#include "Utils/PerfOptions.h"
#include "Utils/Batch39Engine.h"
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
	PointLightsSize,
	PointLightsInteriors,
	PointLightsPerPixelInterior,
	PointLightsWindowBeams,
	PointLightsPlayer,
	PointLightsEffects,
	PointLightsAlphaOutdoors,
	AO,
	AOMode,
	AORayLength,
	AOStrength,
	AODistance,
	AOInteriors,
	AOModeInterior,
	LoadingPumpMs,
	BlackWindowMs)

namespace
{
	// Quality tiers (rays, distance fall-off, denoiser size). (47q) The sun's cone is a look slider of its own (Settings::SunSize), the
	// small Combine filter follows it; the fall-off of Very Low - High is closer; NPCs beyond npcFull: body only, refit every other frame.
	struct QualityTier
	{
		const char* name;
		std::uint32_t baseRays, extraRays;
		std::uint32_t tracePattern, farPattern;  // 0 full, 1 checkerboard, 2 half (one pixel of each 2x2)
		float falloffStart, falloffEnd;          // units (70 per metre); end <= start: none
		bool npcReduced;
		float denoiseRadius;
		std::uint32_t denoiseTaps;
		bool denoiseHalf;
		float farDenoiseScale;
		float npcFull;  // (47q) units; 0 = no limit
		const char* tooltip;
	};

	constexpr QualityTier kTiers[5] = {
		{ "Very Low", 1, 0, 1, 2, 560.0f, 2100.0f, true, 8.0f, 3, true, 0.5f, 700.0f,
			"Half the pixels traced each frame (checkerboard), a quarter beyond 30 m; no extra rays; other characters at reduced quality, beyond "
			"10 m their body only; half-resolution denoiser. Cheapest." },
		{ "Low", 1, 0, 0, 1, 1050.0f, 3150.0f, true, 12.0f, 4, true, 0.5f, 1400.0f,
			"1 ray per pixel, checkerboard beyond 45 m; no extra rays; other characters at reduced quality, beyond 20 m their body only; "
			"half-resolution denoiser." },
		{ "Medium", 1, 1, 0, 0, 2100.0f, 6300.0f, true, 16.0f, 4, true, 0.75f, 2450.0f,
			"1 ray per pixel (+1 at shadow edges, fading out between 30 and 90 m); other characters at reduced quality, beyond 35 m their "
			"body only; half-resolution denoiser." },
		{ "High", 1, 3, 0, 0, 4200.0f, 10500.0f, false, 16.0f, 6, false, 1.0f, 3150.0f,
			"1 ray per pixel (+3 at shadow edges, fading out between 60 and 150 m); other characters beyond 45 m their body only; "
			"full-resolution denoiser." },
		{ "Ultra", 2, 4, 0, 0, 0.0f, 0.0f, false, 16.0f, 6, false, 1.0f, 0.0f,
			"2 rays per pixel (+4 at shadow edges, no fall-off); every character with all its parts, every frame; full-resolution denoiser. "
			"Cleanest." },
	};
	constexpr float kSunSizeMin = 0.1f, kSunSizeMax = 2.0f, kSunSizeDefault = 0.27f;
	constexpr const char* kQualityNames[6] = { "Very Low", "Low", "Medium", "High", "Ultra", "Custom" };
	constexpr const char* kPatternNames[3] = { "Full", "Checkerboard", "Half" };
	// (47n) The sun's dropdown: Off first (SunShadows false), then the tiers and Custom (QualityLevel 0 - 5).
	constexpr const char* kSunQualityNames[7] = { "Off", "Very Low", "Low", "Medium", "High", "Ultra", "Custom" };
	// (47n) Point-light and RTAO tiers (PointQuality / AOQuality 0 - 3; 4 = Custom), the dropdowns with Off first.
	constexpr int kTierCustom = 4;
	constexpr const char* kTierNames[6] = { "Off", "Low", "Medium", "High", "Ultra", "Custom" };
	struct PointTier
	{
		int k, kInterior, maxSources;
		float distance;
		bool alphaOutdoors;
		bool player, effects;  // (47q)
		const char* tooltip;
	};
	// (47q) Low 1/1/16/20 m, Medium 1/2/24/35 m, High 2/3/48/50 m (plants block), Ultra as 47n. The player's own lights are ray traced from
	// Medium up, spell / effect lights in High and Ultra. (48a) Indoors Medium 3, High 4, Ultra 6 (outdoors unchanged).
	constexpr PointTier kPointTiers[4] = {
		{ 1, 1, 16, 1400.0f, false, false, false,
			"1 light per pixel (indoors too), at most 16 lights, up to about 20 m; plants and cut-outs do not block point lights outdoors; "
			"your own lights and spell / effect lights keep their old shadows." },
		{ 1, 3, 24, 2450.0f, false, true, false,
			"1 light per pixel (3 indoors), at most 24 lights, up to about 35 m; plants and cut-outs do not block point lights outdoors; "
			"spell / effect lights keep their old shadows." },
		{ 2, 4, 48, 3500.0f, true, true, true, "2 lights per pixel (4 indoors), at most 48 lights, up to about 50 m; plants and cut-outs block point lights." },
		{ 4, 6, 64, 6000.0f, true, true, true, "4 lights per pixel (6 indoors), at most 64 lights, up to about 86 m." },
	};
	struct AOTier
	{
		int detailRays;
		float distance;
		int broadRate;  // (47q) 0 every pixel, 1 half, 2 one in four
		bool fullResFilter, temporalV2, smoothNormals;
		const char* tooltip;
	};
	constexpr AOTier kAOTiers[4] = {
		{ 1, 2100.0f, 2, false, false, false,
			"1 detail ray, up to about 30 m, the broad AO on one pixel in four (rotating); the 47k history, facet normals. Cheapest." },
		{ 1, 4200.0f, 1, false, true, true, "1 detail ray, up to about 60 m, the broad AO on half the pixels; Temporal v2 and Smooth Normals." },
		{ 1, 7000.0f, 1, false, true, true, "1 detail ray, up to about 100 m, the broad AO on half the pixels; Temporal v2 and Smooth Normals." },
		{ 3, 8192.0f, 0, true, true, true, "3 detail rays, up to about 117 m, every pixel, the broad AO filtered at full resolution. Cleanest." },
	};
	constexpr const char* kBroadRateNames[3] = { "Every pixel", "Half (checkerboard)", "One in four (rotating)" };
	// (48i) RT reflection quality presets (ReflectionsQuality 0 - 3; 4 = Custom). Order of the values: terrain, full roughness range, rough
	// fade start, end, beyond SSR distance, rough rays leaves solid, capture materials, characters, off-screen hit color (0 Cubemap Approx,
	// 1 Material), sun shadow ray, lamps, glow, prefer brighter cubemap. High = the 48h defaults (RayTracing::ReflectionPresetValues{}).
	constexpr const char* kReflectionQualityNames[5] = { "Low", "Medium", "High", "Ultra", "Custom" };
	constexpr RayTracing::ReflectionPresetValues kReflectionPresets[4] = {
		{ true, false, 0.15f, 0.35f, false, true, false, false, 0, false, false, false, false },
		{ true, false, 0.25f, 0.50f, false, true, true, false, 1, false, true, true, true },
		{ true, false, 0.30f, 0.65f, false, true, true, true, 1, true, true, true, true },
		{ true, false, 0.40f, 0.80f, true, true, true, true, 1, true, true, true, true },
	};
	// One line per row of the preset tooltip (printed with "%s").
	constexpr const char* kReflectionPresetTooltips[4][2] = {
		{ "Low: only mirror-like surfaces trace (roughness fade 0.15-0.35: polished floors, calm water, metal); off-screen hits use",
			"  the cubemap guess (no material capture, no texture copies, no shading pass). Cheapest (estimate: a third to half of High's cost)." },
		{ "Medium: roughness fade 0.25-0.50; off-screen hits shaded with their own texture, lamps and glow, but no reflected characters",
			"  and no sun shadow ray (reflected shadows are lit). Estimate: 60-75 % of High's cost." },
		{ "High (the 48h defaults): roughness fade 0.30-0.65; everything on, including reflected characters and the sun shadow ray.", "" },
		{ "Ultra: High plus a wider roughness fade (0.40-0.80: satin and worn surfaces too) and rays past the SSR distance limit.",
			"  Estimate: 1.4-2x High's cost, most in the rain." },
	};

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
	constexpr std::uint32_t kCombineFlagUniformTiles = 8192u;  // (47r) Common.hlsli kFlagUniformTiles: CombineCS fast path (t6 = the denoiser's tiles)
	// (47y) Common.hlsli: CombineCS motion-vector history candidate (t7 = the motion vectors), CombineCS no-history smoothing, DenoiseSpatialCS
	// per-pixel penumbra radius between tile centres.
	constexpr std::uint32_t kCombineFlagMotionHistory = 16384u, kCombineFlagNoHistorySmooth = 32768u, kCombineFlagSmoothTileRadius = 65536u;
	constexpr float kBlockerRange = 4096.0f;    // TraceShadowsCS kBlockerRange
	// (45k) Loading screens: build budgets (frame time does not matter there), warm-up after them.
	constexpr std::uint32_t kLoadingBuildTriangles = 1u << 20;  // per submission (TDR-safe: about 8 frames' worth of normal builds)
	constexpr std::uint32_t kLoadingMaxBuilds = 128;  // (46g) 64 -> 128 (a CPU budget per frame now; 8 contexts x 128 stay within the postbuild slots)
	// (46g) Transition budgets: render-thread ms per frame. Loading / black screen: nothing visible; WarmUp: visible, not handed off.
	constexpr float kLoadingBuildCpuMs = 12.0f, kBlackBuildCpuMs = 10.0f;
	constexpr float kBlackCaptureBudgetMs = 8.0f;
	constexpr float kBlackSkinUploadCpuMs = 6.0f;
	constexpr double kBlackMaxMs = 4000.0;  // a Fader Menu open longer than this after a loading screen is not the black screen
	constexpr double kLateHoldMs = 4000.0;  // items not ready at the hand-off wait at most this long for the late batch
	constexpr double kWarmUpTimeoutMs = 5000.0;
	constexpr double kFadeInMs = 500.0;
	constexpr std::uint32_t kAlphaCopiesPerFrame = 16;  // (45h) 256x256 alpha copies made per frame (5 small dispatches each)
	// (49c) "Finish Loading Work Behind the Loading Screen".
	constexpr double kArrivalCapMs = 3000.0;          // "Prepare When Arriving": at most this long on the main thread (loading screen up)
	constexpr float kArrivalCaptureMs = 40.0f;        // per iteration: capture / object scan
	constexpr float kArrivalBuildMs = 30.0f;          // per iteration: BLAS build recording (one submission each, TDR-safe size)
	constexpr std::uint32_t kArrivalWaitMs = 50;      // longest wait for our own D3D12 submissions per iteration
	constexpr double kArrivalNoProgressMs = 400.0;    // nothing changed and nothing in flight this long: done
	constexpr double kLoadThreadBudgetMs = 8.0;       // the loading-screen thread's D3D11 work per present
	constexpr std::uint32_t kLoadThreadCopiesMax = 64;  // alpha copies per present there
	constexpr std::uint32_t kBlackAlphaCopies = 256;  // alpha copies per frame while the screen is black after the loading screen
	constexpr double kReadyNoProgressMs = 500.0;      // the hand-off's no-progress rule
	// (48f) Capture Materials: 128x128 texture copies per frame (one dispatch each) while playing / in the black screen and warm-up after a loading
	// screen, each with a render-thread CPU cap (the first copy of a frame always goes).
	constexpr std::uint32_t kMaterialCopiesPerFrame = 12, kMaterialCopiesTransition = 256;
	constexpr float kMaterialCopyCpuMs = 0.3f, kMaterialCopyCpuMsTransition = 4.0f;
	// (48g) The shading half: the Off-screen Hit Color switch fades over this long; a material fades in over this many frames once its copy
	// is ready (no pop when the copies arrive after entering an area).
	constexpr double kShadeFadeMs = 400.0;
	constexpr float kShadeFadeInFrames = 30.0f;
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

	// (47i) WaterPrepCS (Point Light Shadows on Water).
	struct alignas(16) WaterPrepConstants
	{
		float InvViewProj[16];  // row-major, as the trace's
		float RenderSize[4];    // xy size, zw 1 / size
	};

	// (46b) Point-light shadows.
	// (47q) 3 too (High indoors); (48a) up to 6: slots 4 - 5 in a second texture set, allocated only while K > 4.
	int ValidPointK(int a_value) { return std::clamp(a_value, 1, 6); }
	constexpr std::uint32_t kPointWideIdleFrames = 600;  // (48a) point frames at K <= 4 before the slot 4 - 5 textures are freed
	constexpr float kPointMinShare = 0.02f;          // a light under 2 % of the pixel's point light is not traced
	constexpr float kPointMinContribution = 1e-3f;   // nor one that hardly lights it at all
	constexpr float kPointHistoryWeight = 0.25f;     // this frame's weight in the denoiser's history blend
	constexpr float kPointMaxFilterRadius = 12.0f;   // px
	constexpr float kPointSunShare = 0.05f;          // at a sunlit pixel a point light under 5 % of the sun's light there is not traced
	constexpr std::uint32_t kPointFlagCharacters = 1u, kPointFlagFirstPerson = 2u, kPointFlagCharCull = 4u;  // TracePointLightsCS
	constexpr std::uint32_t kPointFlagSticky = 8u;  // (46g) TracePointLightsCS ((47q) 16 = cull front: always, 32 = cull back: gone since 47n)
	constexpr std::uint32_t kPointFlagRtSurface = 64u;  // (46y) TracePointLightsCS: mark traced-caster surfaces for SSPLS
	constexpr std::uint32_t kPointFlagWater = 128u;     // (47i) TracePointLightsCS: trace the sources from last frame's water too
	constexpr std::uint32_t kPointFlagSunProbe = 256u;  // (47q) TracePointLightsCS: reuse the sun pass's surface probe (its alpha variant)
	constexpr std::uint32_t kPointFlagCoverage = 512u;  // (47w) TracePointLightsCS: count the engine shadow-map lights' uncovered pixels
	constexpr std::uint32_t kPointFlagCurvedRanking = 1024u;  // (47x) TracePointLightsCS: wider-fit normal + wrapped facing in the ranking
	constexpr std::uint32_t kPointFlagCloseContact = 2048u;   // (47x) TracePointLightsCS: character ray TMin 2 units (its kCloseContactTMin)
	constexpr std::uint32_t kPointFlagBeamRange = 4096u;        // (47z) TracePointLightsCS: beams rank 0 past their radius, not in the share total
	constexpr std::uint32_t kPointFlagStickyReproject = 8192u;  // (47z) TracePointLightsCS: sticky sources read where the surface was last frame
	constexpr std::uint32_t kPointDenoiseSmooth = 4u;          // (47x) PointDenoiseCS: per-slot rotated Poisson taps (Smoother Shadow Edges)
	constexpr std::uint32_t kPointDenoiseMotion = 8u, kPointDenoiseNoHistorySmooth = 16u;  // (47y) PointDenoiseCS: motion-vector history (t6), no-history smoothing
	// (48a) TracePointLightsCS: slots 4 - 5 (u4), last frame's slots 4 - 5 valid, Window Beams: Smooth Edges. PointDenoiseCS: slots 4 - 5
	// (t7 / u4), their history (t8) valid, Window Beams: Smooth Edges.
	constexpr std::uint32_t kPointFlagWide = 16384u, kPointFlagWidePrev = 32768u, kPointFlagBeamSmooth = 65536u;
	constexpr std::uint32_t kPointDenoiseWide = 32u, kPointDenoiseWideHistory = 64u, kPointDenoiseBeamSmooth = 128u;
	// (48c) TracePointLightsCS kFlagExtra: Extra Lights Where Crowded (K < 6: ranks K..5 traced where they hold kExtraShare of the pixel's point light).
	constexpr std::uint32_t kPointFlagExtra = 131072u;
	// (48k) Point Lights: Noise Fix. TracePointLightsCS: Decorrelated Sampling (every slot's disc sample from the beam pattern), Consistent
	// Dark Side (a ray from a traced surface whose smoothed normal faces away from the light is blocked). PointDenoiseCS (PL_FIX variant):
	// Decorrelated Sampling (taps rotated by a hash), Stable Penumbra (the beam path for every point slot), Close Blockers: Minimum Blur.
	constexpr std::uint32_t kPointFlagNoiseDecorrelate = 262144u, kPointFlagNoiseDarkSide = 524288u;
	// (49m) TracePointLightsCS: Dark Side Test Tolerant, Grazing-Angle Offset, Soft Hand-Off for Weak Lights
	// ((50b) 4194304u was Smooth Shadow Edges (extra rays), deleted with its trace loop).
	constexpr std::uint32_t kPointFlagDarkTolerant = 1048576u, kPointFlagGrazeOffset = 2097152u, kPointFlagSoftCut = 8388608u;
	constexpr std::uint32_t kPointFlagBlueNoise = 16777216u;  // (50b) TracePointLightsCS: Blue-Noise Sampling (t10)
	constexpr std::uint32_t kPointFlagWhiteNoise = 33554432u;  // (50e) TracePointLightsCS: RR: White-Noise Sampling (the DLSS RR route only)
	// (49m) PointDenoiseCS: the trace wrote the fractional encoding (kFlagEdgeRays / kFlagSoftCut).
	constexpr std::uint32_t kPointDenoiseFractional = 65536u;
	constexpr std::uint32_t kPointDenoiseNoiseDecorrelate = 256u, kPointDenoiseNoiseStable = 512u, kPointDenoiseNoiseCloseBlur = 1024u;
	// (48m) PointDenoiseCS PL_CLEAN: Moving Characters: Clean Shadow History used / its moving test runs (t6 bound; also for the debug views).
	constexpr std::uint32_t kPointDenoiseClean = 4096u, kPointDenoiseCleanDetect = 8192u;
	// (49i) PointDenoiseCS PL_FIX: Smooth Hard Edges / Steady Edges (Point Lights > Advanced).
	constexpr std::uint32_t kPointDenoiseSmoothHard = 16384u, kPointDenoiseSteadyEdges = 32768u;
	// (47w) Adaptive skip thresholds (per frame, read back). Bad: more than 1 % of its lit pixels uncovered (and at least 32 pixels, so a
	// light lighting a handful of pixels does not flip on noise), or 4096 uncovered pixels whatever the share. Good: under 0.25 % and
	// under 1024 pixels. Skip again after kCoverageEnterFrames good frames (first time) or kCoverageRestoreFrames (after a bad frame);
	// the counts are stale (unsure: draw the map) kCoverageStaleFrames frames after the last read-back holding the light.
	constexpr float kCoverageBadShare = 0.01f, kCoverageGoodShare = 0.0025f;
	constexpr std::uint32_t kCoverageBadMinPixels = 32, kCoverageBadPixels = 4096, kCoverageGoodPixels = 1024;
	constexpr std::uint32_t kCoverageEnterFrames = 10, kCoverageRestoreFrames = 60, kCoverageStaleFrames = 15, kCoverageForgetFrames = 600;
	constexpr std::uint32_t kFlagProbeShare = 8192u;    // (47q) TraceShadowsCS: write the probe as the point pass makes it (Output.w 64 / 128)
	constexpr std::uint32_t kFlagProbeShareOpaque = 16384u;  // (47r) TraceShadowsCS: ... for the non-alpha point variant (no hit / opaque hit only)
	constexpr std::uint32_t kPointDenoiseHistory = 1u, kPointDenoiseDebug = 2u;                             // PointDenoiseCS
	constexpr float kBeamRayMargin = 64.0f;  // (46c) a window beam's ray goes this far past the interior's box
	// (46f) RTAO.
	// (48a) RT reflections step 1: TraceReflectCS flags, and how far a reflection ray looks (about 117 m; past it: a miss).
	constexpr std::uint32_t kReflectTraceCharacters = 1u, kReflectTraceFirstPerson = 2u, kReflectTraceTerrain = 4u, kReflectTraceRoughSolid = 8u;  // (48d) + Rough Rays: Leaves Solid
	constexpr float kReflectMaxRayLength = 8192.0f;
	constexpr float kAORayLengthMin = 35.0f, kAORayLengthMax = 210.0f;  // 0.5 - 3 m
	constexpr float kAOFadeStartShare = 0.75f;  // the RT weight falls from 75 % of the distance to 0 at it (back to SSGI AO)
	constexpr float kAOHistoryWeight = 0.2f;    // this frame's weight in the clamped history blend
	constexpr float kAOClampSlack = 0.08f;      // the history may stray this far outside this frame's filtered 3x3 range
	constexpr std::uint32_t kAOTraceCharacters = 1u, kAOTraceFirstPerson = 2u;  // TraceAOCS
	constexpr std::uint32_t kAOGapFrames = 30;  // (46h) RTAO not mixed in for more than this many frames: it fades in again
	constexpr double kAOFadeInMs = 400.0;
	// (46t) Two scales and the RTAO filters.
	constexpr float kAODetailLengthMin = 8.0f, kAODetailLengthMax = 48.0f;
	constexpr int kAODetailRaysMax = 4;
	constexpr float kAONormalPower = 8.0f;  // Normal-Guided: tap weight dot(N, N_tap)^8 (0.6 at 20 degrees, 0.06 at the 45 degree gate)
	constexpr std::uint32_t kAOTraceDetail = 4u, kAOTraceAlphaHalf = 8u, kAOTraceAlphaOpen = 16u;  // TraceAOCS
	constexpr std::uint32_t kAOTraceSingleRay = 32u, kAOTraceHalfFar = 64u;  // (46y) TraceAOCS (two scales only)
	constexpr std::uint32_t kAOTraceDither8 = 128u, kAOTraceSelfFix = 256u;  // (47h) TraceAOCS: RGBA8 output dither, Character Self-Occlusion Fix
	constexpr std::uint32_t kAOTraceSmoothNormals = 512u;  // (47j) TraceAOCS: RTAO: Smooth Normals (AONormalPrepCS)
	constexpr std::uint32_t kAOTraceSmoothNormals2 = 1024u;  // (47k) TraceAOCS: RTAO: Smooth Normals v2 (offset along Ns, projection; always since 47n)
	constexpr std::uint32_t kAOTraceDynamicFlag = 2048u;     // (47n) TraceAOCS: mark pixels whose rays hit characters / moving objects (.b + 0.6)
	constexpr std::uint32_t kAOTraceQuarterFar = 4096u;      // (47q) TraceAOCS: the broad ray on one pixel of each 2x2, rotating
	constexpr std::uint32_t kAOTraceBent = 8192u;            // (49a) TraceAOCS: RT Specular Occlusion (the bent normal, u2)
	constexpr std::uint32_t kAOFlagNormalDebug = 128u;       // (47k) RTAOCompositeCS: t5 holds this frame's Smooth Normals debug view
	// (47l) RTAO: Temporal v2. (TraceAOCS 2048 = the removed Blue-Noise Rays: never set since 47m.)
	constexpr std::uint32_t kAOFlagTemporalV2 = 256u;        // RTAOCommon.hlsli: Temporal v2 (RTAOTemporalCS RTAO_TEMPORAL_V2, the filters' world sigma)
	constexpr std::uint32_t kAOFlagGeometryValid = 512u;     // RTAOCommon.hlsli: last frame's geometry / history lengths valid
	constexpr std::uint32_t kAOFlagDynamicHistory = 1024u;   // (47n) RTAOCommon.hlsli: RTAO: Tight History Near Moving Things
	constexpr std::uint32_t kAOFlagTileSkip = 2048u;          // (47r) RTAOCommon.hlsli: RTAO: Filter Only Where Traced (t10 = the tile distances)
	constexpr std::uint32_t kAOFlagContactOnce = 4096u;       // (49h) RTAOCommon.hlsli: RTAO: Count Contact Once
	constexpr float kAOMinHistoryWeight = 0.1f;   // this frame's weight never below 1 / 10 (a = max(1 / (n + 1), 0.1))
	constexpr float kAOHistoryFixFrames = 4.0f;   // the history fix fades out over the first 4 frames of a pixel's history
	constexpr float kAOMaxHistoryLength = 32.0f;  // n is stored up to this (only n < 9 changes the weight)
	constexpr float kAOClampK = 2.0f;      // statistical clamp (both terms): mean +- 2 x the 5x5 standard deviation of this frame's filtered value
	constexpr float kAOClampMinDetail = 0.01f;  // ... at least +- 0.01 (detail term)
	constexpr float kAOClampMinLarge = 0.02f;   // ... at least +- 0.02 (large-scale term: its 5x5 is smooth, its frame-to-frame noise is not)
	constexpr float kAOWorldSigmaShare = 0.05f;   // the large-scale blur's sigma: 5 % of the ray length (100 units: 5 units), 2 - 10
	constexpr float kAOCutDistance = 512.0f;      // a camera cut: moved more than this in a frame (units) ...
	constexpr float kAOCutCos = 0.5f;             // ... or turned more than 60 degrees
	// (47j) AONormalPrepCS.
	struct alignas(16) AONormalConstants
	{
		float InvViewProj[16];
		float PrevViewProj[16];
		float PrevInvViewProj[16];
		float PrevViewInverse[16];
		float CameraDelta[4];
		float RenderSize[4];
		// (47k) x: 1 = Smooth Normals v2; y: debug view written to u1 (0 none, 1 confidence, 2 Ns, 3 Ng); zw unused.
		std::uint32_t Flags[4];
		// (47p) "RTAO: skip the normal prep where it is not used": the trace's rect (camera-relative xy min / max) and x = the distance
		// where its weight reaches 0, y = 1 to skip the pixels the trace never reads (outside the rect / beyond x, with a margin).
		float SkipRect[4];
		float SkipFade[4];
		// (48d) RT_REFLECT only (the plain variant declares the prefix): the pixel selection (roughness fade start / end, SSR distance
		// limit in units or 0) and x frame, y bit 0 = the material from the more confident candidate.
		float ReflectSelect[4];
		std::uint32_t ReflectFlags[4];
	};

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
	/// (49c) Probe, log only: the LoadingMenu's hide messages (time and thread). Nothing is delayed or changed; kForceHide is passed on as is.
	struct LoadingMenuProcessMessage
	{
		static RE::UI_MESSAGE_RESULTS thunk(RE::LoadingMenu* a_menu, RE::UIMessage& a_message)
		{
			const auto type = a_message.type.get();
			if ((type == RE::UI_MESSAGE_TYPE::kHide || type == RE::UI_MESSAGE_TYPE::kForceHide) && globals::features::rayTracing.loaded)
				globals::features::rayTracing.NoteLoadingHideMessage(type == RE::UI_MESSAGE_TYPE::kForceHide);
			return func(a_menu, a_message);
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
			// (49c) Probes of this transition.
			cellEventThreads.clear();
			hideMessageQpc = postLoadGameQpc = 0;
			hideMessageThread = postLoadGameThread = 0;
		} else if (events.pending) {
			events.loadingClose = now;
		}
	}
	if (a_open) {
		scene.RequestFlush();
		characters.RequestFlush();  // (45i)
		loadingOpen.store(true);
		if (globals::features::grassOptimizations.loaded)
			globals::features::grassOptimizations.bucketStore.ResetStaging();  // (49c) log-only grass staging times
	} else {
		loadingOpen.store(false);
		loadingClosed.store(true);
		// (49c) Safety: the loading-screen thread's D3D11 work ends with the loading screen (normally already closed by ArrivalPrepare).
		if (loadThreadGate.load())
			CloseLoadingThreadGate();
	}
}

void RayTracing::NoteLoadingHideMessage(bool a_force)
{
	// (49c) Probe (log only): when the LoadingMenu is told to hide, on which thread (save loads have no teleport end to prepare at).
	std::scoped_lock lock(eventsMutex);
	if (hideMessageQpc != 0)
		return;
	hideMessageQpc = Qpc();
	hideMessageThread = static_cast<std::uint32_t>(GetCurrentThreadId());
	hideMessageForce = a_force;
}

void RayTracing::NotePostLoadGame()
{
	// (49c) Probe (log only): SKSE's post-load-game message.
	std::scoped_lock lock(eventsMutex);
	postLoadGameQpc = Qpc();
	postLoadGameThread = static_cast<std::uint32_t>(GetCurrentThreadId());
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
		// (49c) Probe: which threads deliver the cell events (per transition).
		if (events.pending && events.loadingOpen != 0) {
			const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
			auto it = std::find_if(cellEventThreads.begin(), cellEventThreads.end(), [&](const auto& a_entry) { return a_entry.first == thread; });
			if (it == cellEventThreads.end() && cellEventThreads.size() < 16)
				cellEventThreads.emplace_back(thread, 1u);
			else if (it != cellEventThreads.end())
				++it->second;
		}
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
	if (!RtWanted() || cellEventBroken)  // (47m) loading pump and prepare on cell load: always on
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
		cellEventFault = std::format("fault 0x{:08X} while preparing a cell behind the loading screen (preparing on cell load stopped for this session)", code);
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
	if (!faderOpen.load() || a_sinceCloseMs < 0.0 || a_sinceCloseMs > kBlackMaxMs)
		return 0.0f;
	if (a_sinceCloseMs <= kFadeFullMs)
		return 1.0f;
	return static_cast<float>(std::clamp(1.0 - (a_sinceCloseMs - kFadeFullMs) / (kFadeEndMs - kFadeFullMs), 0.0, 1.0));
}

void RayTracing::OnPositionPlayer(int a_stage)
{
	const bool finish = a_stage == static_cast<int>(RE::PositionPlayerEvent::EVENT_TYPE::kFinish);
	{
		std::scoped_lock lock(eventsMutex);
		const std::int64_t now = Qpc();
		if (a_stage == static_cast<int>(RE::PositionPlayerEvent::EVENT_TYPE::kPre))
			events.teleportPre = now;
		else if (finish)
			events.teleportFinish = now;
	}
	// (49c) The teleport's end: the game closes its loading screen 1 - 2 ms after this returns (49a log: finish +6191 / closed +6192 ms), so
	// work done here keeps the real loading screen up (its own thread keeps animating it).
	if (finish)
		ArrivalPrepare();
}

ID3D11ComputeShader* RayTracing::AlphaMaskShader()
{
	// (49c) The 45h alpha copy shader, for the transition frames and (compiled beforehand on the main thread) the loading-screen thread.
	static bool failed[2] = { false, false };
	const bool rgba = backend.AlphaCopiesRgba();
	if (alphaMaskCS && alphaMaskRgba != rgba)
		alphaMaskCS = nullptr;
	if (!alphaMaskCS && !failed[rgba ? 1 : 0]) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (rgba)
			defines.emplace_back("ALPHA_RGBA", "");
		alphaMaskCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RayTracing\\AlphaMaskCS.hlsl", defines, "cs_5_0")));
		alphaMaskRgba = rgba;
		if (!alphaMaskCS)
			failed[rgba ? 1 : 0] = true;
	}
	return alphaMaskCS.get();
}

void RayTracing::ArrivalPrepare()
{
	// (49c) "Prepare When Arriving". Main thread, inside the PositionPlayer finish event, the game's loading screen still up and presented by its
	// own thread. Everything of the destination that needs the scene graph or D3D12 is done here: the queued cell events, the full capture, an
	// uncapped object scan, the characters whose 3D is attached, the BLAS builds and compactions (waiting only for our own D3D12 fence), pool
	// growth, the full-size alpha record buffers. No D3D11 immediate-context call (the loading-screen thread owns it): the cut-out copies and the
	// grass uploads are made by that thread meanwhile (LoadingThreadWork). At most kArrivalCapMs; a fault switches the path off for the session.
	std::int64_t openQpc = 0;
	{
		std::scoped_lock lock(eventsMutex);
		openQpc = events.loadingOpen;
	}
	arrivalLog = {};
	arrivalLog.openQpc = openQpc;
	if (!loadingOpen.load())
		return;  // a teleport without a loading screen (nothing to hide the work behind)
	auto skip = [&](std::string a_why) {
		arrivalLog.skipped = std::move(a_why);
		logger::info("[RayTracing] Prepare When Arriving: not run ({})", arrivalLog.skipped);
	};
	if (!LoadOn(settings.LoadPrepareArrival)) {
		arrivalLog.skipped = "switched off";
		return;
	}
	const auto thread = static_cast<std::uint32_t>(GetCurrentThreadId());
	if (renderThreadId.load() == 0 || thread != renderThreadId.load())
		return skip(std::format("the teleport ended on thread {}, the world is rendered on thread {}", thread, renderThreadId.load()));
	if (!RtWanted())
		return skip("Ray Tracing off");
	if (status == Status::Unavailable || status == Status::Failed || status == Status::Off)
		return skip("Ray Tracing not running");
	if (!backend.IsReady() || backend.HasFailed())
		return skip("backend not ready");
	if (cellEventBroken)
		return skip(std::format("off after a fault: {}", cellEventFault));
	if (cellEventActive)
		return skip("a cell-event step is running");
	if (backend.TraceWaitPending() || backend.SunWaitPending() || backend.AOWaitPending())
		return skip("a D3D11 wait of the last world frame still pending");
	backend.LogMultithreadProtection();
	// The loading-screen thread's work: the alpha copy shader must exist before (never compiled there).
	const bool threadCopies = LoadOn(settings.LoadThreadCopies);
	if (threadCopies && (scene.AlphaWanted() || settings.Characters) && backend.AlphaAvailable())
		AlphaMaskShader();
	arrivalPresents.store(0);
	arrivalActive.store(true);
	if (threadCopies) {
		std::scoped_lock lock(loadThreadMutex);
		loadThreadLog = {};
		loadThreadGate.store(true);
	}
	cellEventActive = true;  // a cell event during it does not start a step of its own
	const std::int64_t start = Qpc();
	unsigned long code = 0;
	const bool ok = SehInvoke(&RayTracing::ArrivalLoopThunk, this, code);
	scene.SetScanUncapped(false);
	cellEventActive = false;
	// The gate closes (waiting for the loading-screen thread's work in flight) before the main thread returns to the game, which renders next.
	CloseLoadingThreadGate();
	arrivalActive.store(false);
	arrivalLog.ran = true;
	arrivalLog.ms = static_cast<double>(Qpc() - start) * QpcMs();
	arrivalLog.loadingPresents = arrivalPresents.load();
	if (!ok) {
		cellEventBroken = true;
		cellEventFault = std::format("fault 0x{:08X} while preparing on arrival (preparing behind the loading screen stopped for this session)", code);
		arrivalLog.result = cellEventFault;
		logger::error("[RayTracing] {}", cellEventFault);
	}
	arrivalTraceMs = static_cast<float>(arrivalLog.ms);
	const auto& a = arrivalLog;
	const auto& t = loadThreadLog;  // the gate is closed: the loading-screen thread no longer writes it
	logger::info("[RayTracing] Prepare When Arriving (main thread, the game's loading screen still up): {:.0f} ms, {}; {} rounds, {} cells, {} shapes, {} BLAS built, "
				 "{} character shapes uploaded, waited {:.0f} ms for our own GPU work, up to {} cut-out copies queued; the loading screen was presented {} times "
				 "meanwhile (its own thread){}; loading-screen thread: {}",
		a.ms, a.result, a.iterations, a.cells, a.shapes, a.builds, a.characterShapes, a.waitMs, a.alphaQueued, a.loadingPresents,
		a.loadingClosedAtMs >= 0.0 ? std::format("; the loading screen closed {:.0f} ms into it (loadingOpen flipped)", a.loadingClosedAtMs) :
		                             std::string("; the loading screen was still open at the end"),
		!threadCopies     ? std::string("off") :
		t.presents == 0   ? std::string("did not present while it ran (nothing copied there)") :
		                    std::format("{} presents with work, {} cut-out copies ({:.1f} ms), {} grass uploads ({:.1f} ms), longest present work {:.1f} ms", t.presents,
								t.alphaCopies, t.alphaMs, t.grassApplies, t.grassMs, t.maxMs));
}

void RayTracing::ArrivalLoopThunk(void* a_self)
{
	auto* self = static_cast<RayTracing*>(a_self);
	try {
		self->ArrivalLoop();
	} catch (const std::exception& e) {
		self->cellEventBroken = true;
		self->cellEventFault = std::format("exception while preparing on arrival: {}", e.what());
		self->arrivalLog.result = self->cellEventFault;
		logger::error("[RayTracing] {}", self->cellEventFault);
	}
}

void RayTracing::ArrivalLoop()
{
	const std::int64_t start = Qpc();
	const auto elapsedMs = [&]() { return static_cast<double>(Qpc() - start) * QpcMs(); };
	if (status != Status::Waiting) {
		status = Status::Waiting;
		statusReason = "preparing behind the loading screen";
	}
	if (backend.HasPrepared())
		backend.AbandonPrepared();
	const std::uint32_t cellsBefore = scene.GetStats().cellsTracked;
	const std::uint32_t shapesBefore = scene.GetStats().shapes;
	const std::uint64_t buildsBefore = backend.GetStats().buildsTotal;
	const std::uint64_t uploadsBefore = backend.SkinUploadsTotal();
	scene.SetScanUncapped(true);
	std::string waiting;
	std::uint64_t lastSignature = 0;
	std::int64_t lastProgressQpc = start;
	bool alphaReserved = false;
	for (;;) {
		++arrivalLog.iterations;
		UpdatePhase();
		DrainTransitionEvents();
		backend.SetTransition(true);
		scene.SetInteriorEnabled(InteriorWanted());
		{
			const auto& stats = backend.GetStats();
			overVramBudget = stats.blasBytes + stats.scratchBytes > kAccelerationBudgetBytes;
		}
		scene.Update(backend, overVramBudget ? 0.0f : kArrivalCaptureMs, globals::state->frameCount);
		// Characters whose 3D is attached now (the teleport is over; the main thread, which attaches 3D, is ours). The rest come later.
		if (settings.Characters && backend.SkinAvailable()) {
			float world[4];
			if (scene.GetLoadedRect(world, true)) {
				RE::NiPoint3 eye{};
				auto* player = RE::PlayerCharacter::GetSingleton();
				if (player)
					eye = player->GetPosition();
				characters.SetRelevance(nullptr);
				characters.Update(backend, world, 10.0, 64u, player ? &eye : nullptr);
			}
		}
		// 49b E (Reserve Shaders and Memory Up Front): the alpha record buffers at full size (D3D12 upload heaps), once.
		if (!alphaReserved && ReserveOn()) {
			backend.PreallocateAlphaInfoBuffers();
			alphaReserved = true;
			// 49b D: the slot 4 - 5 textures (device calls only, no immediate context), kept for the session once made.
			if (settings.PointLights && !pointWideDenoiseFailed && pointVis[0].texture && !pointWide[0].texture) {
				D3D11_TEXTURE2D_DESC d{};
				pointVis[0].texture->GetDesc(&d);
				if (EnsurePointWide(d.Width, d.Height))
					logger::info("[RayTracing] Reserve Up Front: slot 4 - 5 textures made behind the loading screen ({}x{})", d.Width, d.Height);
			}
		}
		// The alpha copy layers may have been made just now (their format picks the shader variant): the loading-screen thread's shader is
		// (re)compiled here, under its lock (it skips a present meanwhile).
		if (loadThreadGate.load() && backend.AlphaAvailable() && backend.PendingAlphaCopies() > 0 && (!alphaMaskCS || alphaMaskRgba != backend.AlphaCopiesRgba())) {
			std::scoped_lock lock(loadThreadMutex);
			AlphaMaskShader();
		}
		backend.Prewarm(2.0f, settings.Characters);
		const bool submitted = backend.BuildOnly(kLoadingBuildTriangles, kLoadingMaxBuilds, kArrivalBuildMs);
		backend.Tick();
		scene.EndFrame(backend);
		characters.EndFrame(backend);
		arrivalLog.alphaQueued = std::max(arrivalLog.alphaQueued, backend.PendingAlphaCopies());
		const double elapsed = elapsedMs();
		if (!loadingOpen.load() && arrivalLog.loadingClosedAtMs < 0.0)
			arrivalLog.loadingClosedAtMs = elapsed;
		if (ArrivalDone(elapsed, waiting)) {
			arrivalLog.result = "done";
			break;
		}
		if (elapsed >= kArrivalCapMs) {
			arrivalLog.result = std::format("time limit {:.0f} ms reached (waiting for: {})", kArrivalCapMs, waiting);
			break;
		}
		// No progress and nothing in flight for a while: done as far as it goes (the rest follows after the loading screen).
		const auto& sc = scene.GetStats();
		const std::uint64_t signature = (static_cast<std::uint64_t>(sc.shapes) * 0x9E3779B97F4A7C15ull) ^ (backend.GetStats().buildsTotal << 20) ^
		                                (static_cast<std::uint64_t>(backend.PendingAlphaCopies()) << 40) ^ (backend.SkinUploadsTotal() << 8) ^
		                                (static_cast<std::uint64_t>(sc.cellsTracked) << 50) ^ scene.ScansSinceChange() ^ (static_cast<std::uint64_t>(backend.CompactionsDue()) << 30);
		const std::int64_t now = Qpc();
		if (signature != lastSignature) {
			lastSignature = signature;
			lastProgressQpc = now;
		} else if (static_cast<double>(now - lastProgressQpc) * QpcMs() >= kArrivalNoProgressMs && (sc.cellsUntracked == 0 || elapsed > 1500.0)) {
			// (Cells waiting for the gate's frames -- the loading screen's own frames advance the count -- are not "no progress" in the first 1.5 s.)
			arrivalLog.result = std::format("no progress for {:.0f} ms (waiting for: {})", kArrivalNoProgressMs, waiting);
			break;
		}
		// Our own GPU work in flight (builds, compactions): wait for it, briefly. Otherwise the cells settle over the loading screen's frames.
		if (backend.QueuedBuilds() > 0 || backend.CompactionsDue() > 0 || backend.PendingSkinUploads() > 0 || submitted) {
			const std::int64_t waitStart = Qpc();
			backend.WaitSubmitted(kArrivalWaitMs);
			arrivalLog.waitMs += static_cast<double>(Qpc() - waitStart) * QpcMs();
		} else {
			Sleep(2);
		}
	}
	scene.SetScanUncapped(false);
	const auto& sc = scene.GetStats();
	arrivalLog.cells = sc.cellsTracked > cellsBefore ? sc.cellsTracked - cellsBefore : sc.cellsTracked;
	arrivalLog.shapes = sc.shapes > shapesBefore ? sc.shapes - shapesBefore : 0u;
	arrivalLog.builds = static_cast<std::uint32_t>(backend.GetStats().buildsTotal - buildsBefore);
	arrivalLog.characterShapes = static_cast<std::uint32_t>(backend.SkinUploadsTotal() - uploadsBefore);
}

bool RayTracing::ArrivalDone(double a_elapsedMs, std::string& o_waiting)
{
	// (49c) The destination as complete as it can be behind the loading screen.
	if (scene.FlushPending()) {
		o_waiting = "the place left still being released";
		return false;
	}
	if (Util::IsInterior() && !InteriorWanted())
		return true;
	const auto& sc = scene.GetStats();
	const bool sceneMatches = !(Util::IsInterior() && InteriorWanted() && RE::TES::GetSingleton() && RE::TES::GetSingleton()->interiorCell) || sc.interior;
	std::string w;
	const auto add = [&](std::string a_item) { w += (w.empty() ? "" : ", ") + a_item; };
	if (!sceneMatches)
		add("the interior not in the scene yet");
	if (sc.cellsTracked == 0)
		add("no cell captured yet");
	if (sc.cellsGathering > 0)
		add(std::format("{} cells capturing", sc.cellsGathering));
	if (sc.cellsUntracked > 0)
		add(std::format("{} cells not in place yet", sc.cellsUntracked));
	if (sc.cellsNotAttached > 0 && a_elapsedMs < 1500.0)
		add(std::format("{} grid cells not attached", sc.cellsNotAttached));
	if (!scene.ScanPassComplete())
		add("object scan not through every cell");
	if (backend.QueuedBuilds() > 0)
		add(std::format("{} BLAS queued", backend.QueuedBuilds()));
	if (backend.CompactionsDue() > 0)
		add(std::format("{} compactions", backend.CompactionsDue()));
	if (backend.PendingSkinUploads() > 0)
		add(std::format("{} character shapes", backend.PendingSkinUploads()));
	// The loading-screen thread's copies, while it is presenting (none for 300 ms: it is not, they are made after the loading screen).
	if (loadThreadGate.load() && backend.PendingAlphaCopies() > 0 && (arrivalPresents.load() > 0 || a_elapsedMs < 300.0))
		add(std::format("{} cut-out copies (loading-screen thread)", backend.PendingAlphaCopies()));
	if (loadThreadGate.load() && globals::features::grassOptimizations.loaded && globals::features::grassOptimizations.bucketStore.PendingCount() > 0 &&
		(arrivalPresents.load() > 0 || a_elapsedMs < 300.0))
		add("grass uploads (loading-screen thread)");
	o_waiting = w;
	return w.empty();
}

void RayTracing::LoadingThreadWork()
{
	// (49c) "Use the Loading Screen's Thread for GPU Copies". Called from the Present hook of the thread presenting the loading screen, between its
	// frames, only while ArrivalPrepare holds the main thread (the gate). That thread owns the D3D11 immediate context during loading; the main
	// thread makes no immediate-context call in ArrivalPrepare, and the gate closes -- waiting for this function under loadThreadMutex -- before the
	// main thread returns to the game. Community Shaders does not change ID3D11Multithread protection (logged); this gate is the serialisation.
	// The alpha table is shared with the main thread's capture: Backend's alpha lock guards it.
	if (!loadThreadGate.load(std::memory_order_acquire))
		return;
	std::unique_lock lock(loadThreadMutex, std::try_to_lock);
	if (!lock.owns_lock() || !loadThreadGate.load())
		return;
	const std::int64_t start = Qpc();
	const auto elapsedMs = [&]() { return static_cast<double>(Qpc() - start) * QpcMs(); };
	bool worked = false;
	if (backend.IsReady() && !backend.HasFailed() && backend.AlphaAvailable() && alphaMaskCS && alphaMaskRgba == backend.AlphaCopiesRgba()) {
		const std::uint32_t before = backend.PendingAlphaCopies();
		if (before > 0) {
			backend.RecordAlphaCopies(alphaMaskCS.get(), kLoadThreadCopiesMax);
			const std::uint32_t after = backend.PendingAlphaCopies();
			const std::uint32_t made = before > after ? before - after : 0u;
			loadThreadLog.alphaCopies += made;
			loadThreadCopiesTrace.fetch_add(made, std::memory_order_relaxed);
			loadThreadLog.alphaMs += elapsedMs();
			worked = true;
		}
	}
	if (elapsedMs() < kLoadThreadBudgetMs && globals::features::grassOptimizations.loaded) {
		const std::int64_t grassStart = Qpc();
		const float ms = globals::features::grassOptimizations.ApplyFromLoadingThread();
		if (ms >= 0.0f) {
			++loadThreadLog.grassApplies;
			loadThreadLog.grassMs += static_cast<double>(Qpc() - grassStart) * QpcMs();
			worked = true;
		}
	}
	if (worked) {
		++loadThreadLog.presents;
		loadThreadLog.maxMs = std::max(loadThreadLog.maxMs, elapsedMs());
	}
}

void RayTracing::CloseLoadingThreadGate()
{
	// (49c) No new loading-screen-thread work from now; wait for the one in flight (it holds loadThreadMutex for its whole run).
	loadThreadGate.store(false, std::memory_order_release);
	std::scoped_lock lock(loadThreadMutex);
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
		// (49c) Probes (log only): cell-event threads, the LoadingMenu hide message, SKSE's post-load-game message, the engine's grass hand-over.
		const auto rel = [&](std::int64_t a_qpc) { return a_qpc ? std::format("{:+.0f}", static_cast<double>(a_qpc - events.loadingOpen) * QpcMs()) : std::string("-"); };
		std::string threads;
		for (const auto& [thread, count] : cellEventThreads)
			threads += std::format("{}{} on thread {}", threads.empty() ? "" : ", ", count, thread);
		line += std::format("; cell events {}; LoadingMenu hide message {}{} (thread {}); SKSE post-load-game {} (thread {}); main thread {}",
			threads.empty() ? std::string("none") : threads, rel(hideMessageQpc), hideMessageForce ? " (forced)" : "", hideMessageThread, rel(postLoadGameQpc),
			postLoadGameThread, renderThreadId.load());
		if (globals::features::grassOptimizations.loaded)
			line += "; " + globals::features::grassOptimizations.DescribeStaging(events.loadingOpen, events.loadingClose);
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
		pointSelector.ForgetHistory();  // (46s) per-light positions / groups of the place left behind
		ClearCoverage();                // (47w) the adaptive skip states belong to it too
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
	// BlackWindowMs after the loading screen closed count as black (large budgets).
	const double blackLimit = std::clamp(static_cast<double>(settings.BlackWindowMs), 0.0, kBlackMaxMs);
	if (phase == Phase::Black && (!faderOpen.load() || sinceClose > blackLimit))
		phase = Phase::WarmUp;
	blackScreen = inTransition && !open && phase != Phase::Loading && faderOpen.load() && sinceClose <= blackLimit;
	scene.SetLoadGate(true, Arrived());  // (46k) the loading pump: always on (47m)

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
	if (backend.QueuedBuilds() > 0 || backend.PendingSkinUploads() > 0) {
		readyWaiting = std::format("{} BLAS queued, {} character shapes waiting", backend.QueuedBuilds(), backend.PendingSkinUploads());
		return false;
	}
	// (48e) RT reflections: the destination's terrain BLASes too (made behind the loading screen; their builds are in QueuedBuilds).
	if (settings.ReflectionsLoadingPrep && scene.TerrainPending() > 0)
		return false;
	// (46k) The player stands in what was captured (not in the place left, nor beside it).
	auto* player = RE::PlayerCharacter::GetSingleton();
	if (!player)
		return false;
	const auto position = player->GetPosition();
	if (!(std::isfinite(position.x) && std::isfinite(position.y) && scene.Covers(position.x, position.y)))
		return false;
	if (!settings.LoadFinish)
		return true;  // 48o / 49a
	// (49c) The destination complete, not only its cells: every complete cell scanned for objects (trees, doors, items), no cut-out copy left,
	// every held item ready (BLAS built, texture copied). 49a handed off with 243 - 288 items still pending that then built in the visible
	// frames. A no-progress rule keeps an item that never becomes ready from holding the hand-off: nothing changed for kReadyNoProgressMs.
	const std::uint32_t alphaPending = backend.PendingAlphaCopies();
	const auto held = scene.HeldCounts(backend);
	const bool scanned = scene.ScanPassComplete();
	if (scanned && alphaPending == 0 && held.pending == 0) {
		readyStalled = false;
		return true;
	}
	readyWaiting = std::format("{}{} cut-out copies, {} items not ready", scanned ? "" : "object scan not through every cell, ", alphaPending, held.pending);
	const std::uint64_t signature = (static_cast<std::uint64_t>(sc.shapes) * 0x9E3779B97F4A7C15ull) ^ (backend.GetStats().buildsTotal << 20) ^
	                                (static_cast<std::uint64_t>(alphaPending) << 40) ^ (static_cast<std::uint64_t>(held.pending) << 8) ^ scene.ScansSinceChange();
	const std::int64_t now = Qpc();
	if (signature != readySignature || readySignatureQpc == 0) {
		readySignature = signature;
		readySignatureQpc = now;
		return false;
	}
	if (static_cast<double>(now - readySignatureQpc) * QpcMs() < kReadyNoProgressMs)
		return false;
	readyStalled = true;  // logged with the hand-off
	return true;
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
	handOffPendingTrace = static_cast<std::int32_t>(counts.pending);
	handOffSummary = std::format("hand-off {:.0f} ms after the loading screen closed ({}): {} items switched to RT at once, {} pending ({} BLAS queued, "
								 "{} cells capturing, {} character shapes waiting, {} cut-out copies){}",
		static_cast<double>(now - warmUpStart) * QpcMs(), a_when, counts.shown, counts.pending, backend.QueuedBuilds(), sc.cellsGathering,
		backend.PendingSkinUploads(), backend.PendingAlphaCopies(),
		!settings.LoadFinish ? std::string() :
		readyStalled         ? std::format("; no progress for {:.0f} ms while waiting for: {} (handed off anyway)", kReadyNoProgressMs, readyWaiting) :
		counts.pending > 0   ? std::format("; last wait: {}", readyWaiting) :
		                       std::string());
	readyStalled = false;
	readySignatureQpc = 0;
	logger::info("[RayTracing] {}", handOffSummary);
	// (49c) The fallback: whatever is still pending now is built in visible frames at the play budgets. Say why it was not done behind the
	// loading screen.
	if (settings.LoadFinish && (counts.pending > 0 || std::strstr(a_when, "visible"))) {
		std::string why;
		std::int64_t openQpc = 0;
		{
			std::scoped_lock lock(eventsMutex);
			openQpc = events.loadingOpen;
		}
		if (arrivalLog.openQpc != openQpc)
			why = "no teleport end on the main thread behind this loading screen (a save load?): nothing could be prepared behind it";
		else if (!arrivalLog.ran)
			why = arrivalLog.skipped.empty() ? std::string("no preparation behind the loading screen this time (no teleport end on the main thread: a save load?)") :
			                                   std::format("preparation behind the loading screen did not run ({})", arrivalLog.skipped);
		else
			why = std::format("preparation behind the loading screen ended: {}", arrivalLog.result);
		logger::info("[RayTracing] Loading-screen fallback: {} items left for visible frames (hand-off {}); {}", counts.pending, a_when, why);
	}
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
	backend.WaitForReflections();  // (48d)
	const bool loading = phase == Phase::Loading;
	const bool visible = phase == Phase::WarmUp && !blackScreen;
	// (46k) Budgets. The loading pump (behind the loading screen): its own CPU per frame (LoadingPumpMs), split between the
	// capture, the characters, the pools and the builds. Visible frames: what playing uses (never bulk work on a visible
	// frame). (47m) The pump is always on (46i's moderate budgets without it are gone).
	const float pumpMs = std::clamp(settings.LoadingPumpMs, 2.0f, 32.0f);
	float capture = kBlackCaptureBudgetMs, charactersMs = kBlackCaptureBudgetMs, prewarmMs = 4.0f, buildMs = kBlackBuildCpuMs;
	std::uint32_t charactersMax = 64u, buildTriangles = kLoadingBuildTriangles, buildCount = kLoadingMaxBuilds;
	if (loading) {
		capture = pumpMs * 0.4f;
		charactersMs = pumpMs * 0.15f;
		prewarmMs = std::min(2.0f, pumpMs * 0.15f);
		buildMs = kLoadingBuildCpuMs;  // what is left of the pump's budget (below)
	} else if (visible) {
		capture = kCaptureBudgetMs;
		charactersMs = 1.0f;
		charactersMax = 6u;
		prewarmMs = 0.0f;
		buildMs = 2.0f;
		buildTriangles = kBuildBudget;
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
	// (49c) Cut-out texture copies of the destination before the hand-off (49a / 49b made them only once playing: no cut-out could be ready at the
	// hand-off). Never behind the loading screen from here (the loading-screen thread does those itself, LoadingThreadWork); many while the screen
	// is black, the play rate on visible frames (shapes captured after the loading screen: the fallback).
	if (!loading && !inLoadingPump && settings.LoadFinish && (scene.AlphaWanted() || settings.Characters) && backend.AlphaAvailable() &&
		backend.PendingAlphaCopies() > 0) {
		if (auto* shader = AlphaMaskShader())
			backend.RecordAlphaCopies(shader, (!visible || lastFadeFactor > 0.0f) ? kBlackAlphaCopies : kAlphaCopiesPerFrame);
	}
	// (49b) Hitch fix E: the alpha record buffers at full size now, not on the first frames with many new cut-out shapes.
	if (ReserveOn())
		backend.PreallocateAlphaInfoBuffers();
	// (49b) Hitch fix D: the slot 4 - 5 textures while the picture is dark (not behind the loading screen: no D3D11 work there).
	if (!loading && (!visible || lastFadeFactor > 0.0f) && ReserveOn() && settings.PointLights && !pointWideDenoiseFailed &&
		pointVis[0].texture && !pointWide[0].texture) {
		D3D11_TEXTURE2D_DESC d{};
		pointVis[0].texture->GetDesc(&d);
		if (EnsurePointWide(d.Width, d.Height))
			logger::info("[RayTracing] Reserve Up Front: slot 4 - 5 textures made in the dark part of the transition ({}x{})", d.Width, d.Height);
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
			characters.SetRelevance(nullptr);  // (47u) nothing traced here
			characters.Update(backend, world, static_cast<double>(charactersMs), charactersMax, player ? &eye : nullptr);
		}
	}
	cpuLast.characters = static_cast<float>(static_cast<double>(Qpc() - charactersStart) * QpcMs());
	if (prewarmMs > 0.0f)
		backend.Prewarm(prewarmMs, settings.Characters);
	// (48f) Capture Materials: the texture copies of the destination. Never behind the loading screen itself (it is presented from another thread and
	// owns the immediate context); the larger budget in the black screen and the dim part of the fade-in, the play budget once it is visible.
	if (!loading)
		RunMaterialCopies(!visible || lastFadeFactor > 0.0f);
	if (loading)
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
	logger::info("[RayTracing] Loading pump ({:.0f} ms per frame, black window {:.0f} ms): {} frames, RT CPU {:.0f} ms ({} frames on another thread, {} in the middle "
				 "of the teleport: skipped); {} fully-loaded cells reported, {} cells captured behind the loading screen ({} after their event, the rest once the "
				 "player had arrived), {} checks found a cell's 3D not in place yet",
		settings.LoadingPumpMs, static_cast<double>(settings.BlackWindowMs), pumpFrames, pumpCpuTotalMs, pumpForeignFrames, pumpMidTeleport, gate.noted, gate.captured,
		gate.byEvent, gate.unsettledTotal);
	// (46n) The preparation from the cell events (main thread, behind the loading screen) and the fade-in budgets.
	{
		const auto& c = cellEventLog;
		logger::info("[RayTracing] Cell-event preparation ({}, {:.0f} ms per event): {} events, {} steps run ({:.0f} ms CPU, longest {:.1f} ms, "
					 "{} in the middle of the teleport), {} events on another thread, {} skipped{}; {} cells captured, {} shapes, {} BLAS built in the steps",
			cellEventBroken ? "off after a fault" : "on", settings.CellEventMs, c.events, c.steps, c.cpuMs, c.maxMs, c.midTeleport, cellEventForeign.exchange(0, std::memory_order_relaxed), c.skipped,
			c.lastSkip.empty() ? std::string() : std::format(" (last: {})", c.lastSkip), c.cellsCaptured, c.shapes, c.builds);
		cellEventLog = {};
	}
	for (auto& entry : transitionLog)
		entry = {};
	transitionLogSlot = -1;
}

void RayTracing::UpdateBeamShares()
{
	// (46n) The window beams' sampled-cone counters (read back a few frames later) feed the warm-up report. (47m) The 46g
	// Window Beam Guard that also read them is gone (it was off by default since 46o).
	const auto& stats = backend.GetStats();
	const std::uint32_t frame = globals::state->frameCount;
	if (stats.beamSamples != beamSamplesSeen) {
		beamSamplesSeen = stats.beamSamples;
		if (beamSharePending && frame - beamShareFrom < 0x80000000u) {  // (46n) the warm-up report
			for (std::uint32_t slot = 0; slot < lastSlotBeams.size(); ++slot) {
				const auto* light = lastSlotBeams[slot];
				if (!light)
					continue;
				auto& share = beamShares[light];
				share.traced += stats.beamTraced[slot];
				share.blocked += stats.beamBlocked[slot];
			}
		}
	}
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
	if (!RtWanted() || !SunOn() || status != Status::Active || !typedUavLoadR8 || Util::IsInterior() || phase == Phase::Loading || !handedOff)
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
	// (48f) Capture Materials: the backend accepts materials / attributes before the scene captures with them (its config below).
	settings.ReflectionsMode = 0;  // (48k) RT reflections hidden: always Off (the 48j Off path)
	if (backend.IsReady())
		backend.SetMaterialsEnabled(MaterialsWanted());
	if (backend.IsReady() && backend.MaterialsEnabled())
		backend.PrepareMaterialResources(globals::state->frameCount);  // (48g) attribute buffer + first page at enable time, render thread
	characters.SetMaterials(MaterialsWanted() && settings.ReflectionsCharacterMaterials);  // (48g) a change re-captures the characters
	scene.Configure(SceneConfigFromSettings());  // a change re-captures the scene
	const bool alphaInUse = scene.AlphaWanted() || (settings.Characters && backend.SkinAlphaAvailable());
	if (!alphaInUse && backend.IsReady() && backend.AlphaIdle())
		backend.ReleaseAlphaResources();

	// (47l) Foliage sway. Turned on (again) by the user: an earlier automatic switch-off is forgotten; a fault of its D3D12 side
	// switches it off (the leaves then stay frozen as in 47k, the rest of Ray Tracing goes on). No re-capture either way.
	if (settings.FoliageSway && !windRequested) {
		backend.ClearWindFault();
		windOffReason.clear();
	}
	if (settings.FoliageSway && !backend.WindFault().empty()) {
		windOffReason = backend.WindFault();
		settings.FoliageSway = false;
		logger::warn("[RayTracing] Sway in RT Shadows switched off: {}", windOffReason);
	}
	windRequested = settings.FoliageSway;
	scene.SetSway(settings.FoliageSway && backend.WindAvailable());
	if (!scene.SwayActive() && backend.IsReady() && backend.WindIdle())
		backend.ReleaseWindResources();

	// (48a) RT reflections step 1. Turned on (again): an earlier automatic switch-off is forgotten. Off: no terrain (the next scene
	// Update releases every terrain BLAS), no textures, no dispatch.
	settings.ReflectionsMode = 0;  // (48k) RT reflections hidden: always Off ((48d) was 0 - 2)
	if (settings.ReflectionsMode != 0 && !reflectRequested) {
		backend.ClearReflectFault();
		reflectOffReason.clear();
	}
	reflectRequested = settings.ReflectionsMode != 0;
	scene.SetTerrain(ReflectionsWanted() && settings.ReflectionsTerrain);
	scene.SetTerrainLoadingPrep(settings.ReflectionsLoadingPrep);  // (48e)
	if (!ReflectionsWanted() && backend.IsReady() && backend.ReflectBytes())
		backend.ReleaseReflectResources();
	// (48f) Capture Materials off (or RT Reflections Off): no surface record; the rest goes once the re-captured scene no longer uses it.
	if (backend.IsReady()) {
		if (!ShadeWanted() && shadeTexture)
			ReleaseShadeResources();  // (48g)
		if (!MaterialsWanted() && backend.ReflectSurfaceBytes())
			backend.ReleaseReflectSurfaceTextures();
		backend.MaterialHousekeeping(globals::state->frameCount);
	}
}

void RayTracing::FillWind(RayTracingRT::FrameInput& a_input)
{
	// (47n) This frame's pose of every wind BLAS, each plant with its own wind (the engine's per-node values, read after
	// BSTreeManager::Update): at Early Prep, and again before the trace frame for wind BLASes made since (once per BLAS per frame).
	(void)a_input;
	if (!scene.SwayActive() || !globals::state || !backend.IsReady())
		return;
	// (47q) Keep Leaves Moving on Long Frames: this frame's game time step (0 while the game is paused: nothing advanced).
	const bool paused = globals::game::ui && globals::game::ui->GameIsPaused();
	scene.UpdateWindPoses(backend, globals::state->frameCount, settings.FoliageKeepMoving, paused ? 0.0f : RE::GetSecondsSinceLastFrame(),
		settings.FoliageKeepMoving && settings.FoliageLearnAnyFrame, CurrentRelevance());
}

const RayTracingRelevance::Region* RayTracing::CurrentRelevance()
{
	// (47u) Skip Updates That Cannot Affect the Image: the early region, built once a frame from last frame's exact rays (an exterior sun
	// frame), widened for this frame's camera turn and movement (corner tangents x 1.35 -- about 8 degrees more on each side at the
	// user's field of view -- and 384 units), point sources grown by 384 units. Not in interiors, with the sun off, after any frame that
	// did not trace the sun (loading, menus without a world frame): then nothing is culled.
	const std::uint32_t frame = globals::state->frameCount;
	if (relevanceEarlyFrame != frame) {
		relevanceEarlyFrame = frame;
		relevanceEarlyOk = false;
		relNpcCulledThisFrame = 0;
		relFallbackThisFrame = false;
		auto* sky = globals::game::sky;
		if (SunOn() && !Util::IsInterior() && sky && sky->mode.get() == RE::Sky::Mode::kFull && phase == Phase::Play &&
			relevanceLast.valid && relevanceLast.frame + 1 == frame) {
			relevanceEarly.Build(relevanceLast, 1.35, 384.0, 384.0);
			relevanceEarlyOk = relevanceEarly.Valid() && !relevanceEarly.All();
		}
	}
	return relevanceEarlyOk ? &relevanceEarly : nullptr;
}

void RayTracing::BuildExactRelevance(const RayTracingRT::FrameInput& a_input, const float a_traceRect[4], double a_camX, double a_camY, double a_camZ)
{
	// (47u) Every ray this exterior frame traces: the sun's (from the trace rect toward the sun, kMaxRayLength), the point lights' (to a
	// point of their emitter ball; beam / aimed sources cannot be bounded: everything) and the short ones (RTAO, probes). The frustum is
	// the shaders' own (invViewProj), widened by 3 % (the start offsets grow 0.15 % of the distance) with a 64-unit margin.
	RayTracingRelevance::Inputs in;
	in.valid = true;
	in.frame = globals::state->frameCount;
	in.camera[0] = a_camX;
	in.camera[1] = a_camY;
	in.camera[2] = a_camZ;
	std::copy_n(a_input.constants.invViewProj, 16, in.invViewProj);
	in.sun = a_input.sun;
	for (int a = 0; a < 3; ++a)
		in.toSun[a] = a_input.constants.toSun[a];
	in.sunLength = static_cast<double>(kMaxRayLength) + 64.0;
	in.sunSpread = static_cast<double>(std::max(a_input.constants.toSun[3], 0.0f)) * 1.05 + 1e-3;
	in.rect[0] = a_traceRect[0] + a_camX;
	in.rect[1] = a_traceRect[1] + a_camY;
	in.rect[2] = a_traceRect[2] + a_camX;
	in.rect[3] = a_traceRect[3] + a_camY;
	const double aoLength = a_input.ao ? static_cast<double>(std::max(a_input.ao->constants.params[2], 0.0f)) : 0.0;
	in.nearReach = std::max(aoLength + 64.0, 160.0);  // RTAO's long ray; probes <= 96 units
	if (a_input.pointLights && a_input.pointLights->sources) {
		const auto* sources = static_cast<const RayTracingPointLights::GpuSource*>(a_input.pointLights->sources);
		const std::uint32_t count = std::min<std::uint32_t>(a_input.pointLights->scan, a_input.pointLights->sourceBytes / sizeof(RayTracingPointLights::GpuSource));
		const double emitter = std::max(a_input.pointLights->constants.bias[3], 0.0f);
		in.targets.reserve(count);
		for (std::uint32_t s = 0; s < count; ++s) {
			const auto& src = sources[s];
			if (src.info[0] & (RayTracingPointLights::kSourceBeam | RayTracingPointLights::kSourceAim)) {
				in.all = true;
				break;
			}
			const double radius = (src.info[0] & RayTracingPointLights::kSourceEmitter) ? std::max(static_cast<double>(src.spot[3]), emitter) : emitter;
			if (!std::isfinite(radius) || !std::isfinite(src.posRadius[0]) || !std::isfinite(src.posRadius[1]) || !std::isfinite(src.posRadius[2])) {
				in.all = true;
				break;
			}
			in.targets.push_back({ src.posRadius[0] + a_camX, src.posRadius[1] + a_camY, src.posRadius[2] + a_camZ, radius });
		}
	}
	relevanceExact.Build(in, 1.03, 64.0, 16.0);
	relevanceLast = std::move(in);
}

void RayTracing::ResolveRelevance(const RayTracingRelevance::Region* a_exact, const float* a_world)
{
	// (47u) Plants: every culled one this frame's rays reach -> late (RecordLateWind before the TLAS on an Early Prep frame, RecordWind's
	// own refits otherwise). NPCs: any culled one reachable -> this frame's characters again without culling (the Early Prep context is
	// given up first: its list ran; ExecuteFrame then records skinning and the late refits itself).
	if (scene.WindCulledLastFrame() > 0) {
		const std::uint32_t late = scene.MarkLateWind(backend, a_exact);
		relLateTotal += late;
	}
	relNpcCulledThisFrame = characters.CulledLastUpdate();
	if (relNpcCulledThisFrame == 0 || (a_exact && !characters.CulledReachable(*a_exact)))
		return;
	if (!a_world) {
		// Cannot happen (NPCs are culled only on exterior sun frames, which pass their rect); counted, never silently.
		relFallbackThisFrame = true;
		++relFallbacks;
		return;
	}
	relFallbackThisFrame = true;
	++relFallbacks;
	if (backend.HasPrepared())
		backend.AbandonPrepared();
	earlyThisFrame = false;
	charactersUpdatedEarly = false;
	skinDraws.clear();
	skinBones.clear();
	characters.SetRelevance(nullptr);
	if (a_world && settings.Characters && backend.SkinAvailable()) {
		characters.Update(backend, a_world, blackScreen ? 6.0 : 1.0, blackScreen ? 32u : 6u);
		characters.CollectDraws(skinDraws, skinBones);
	}
	relNpcCulledThisFrame = 0;
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
	config.spatialChunks = settings.SpatialStaticChunks;  // (47u) a change re-captures the scene
	config.materials = MaterialsWanted();                 // (48f) likewise
	config.strictAlpha = settings.HairFaintStrands;        // (49h) likewise
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
	// (46m) Read lanes, scan cache and merged BLASes are always on (47m).
	characters.SetStrictAlpha(settings.HairFaintStrands);  // (49h) a change re-captures the characters
	characters.Configure(settings.PhysicsParts, settings.Hair, backend.SkinAlphaAvailable(), settings.FirstPersonPlayer, PerfOptions::settings.RtDropTinyParts);
	characters.SetBonesTolerant(PerfOptions::settings.RtBonesTolerant);  // (47p)
	characters.SetRefreshShapes(settings.CharRefreshShapes);              // (49m)
	characters.SetBodyOnlyDistance(NpcFullDistance());                   // (47q) the NPC rule
}


// ---------------------------------------------------------------------------------------------
// (45l) Quality tiers, denoiser in effect
// ---------------------------------------------------------------------------------------------

RayTracing::QualityParams RayTracing::ResolveQuality() const
{
	QualityParams q;
	const int level = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
	// (47q) The cone is the look slider (all tiers and Custom); the small Combine filter (denoiser Off) follows it, as Custom's did.
	q.coneDegrees = std::isfinite(settings.SunSize) ? std::clamp(settings.SunSize, kSunSizeMin, kSunSizeMax) : kSunSizeDefault;
	q.smallFilterRadius = q.coneDegrees > 0.4f ? 2u : 1u;
	if (level == static_cast<int>(Quality::Custom)) {
		q.baseRays = static_cast<std::uint32_t>(std::clamp(settings.CustomBaseRays, 1, 4));
		q.extraRays = static_cast<std::uint32_t>(std::clamp(settings.CustomExtraRays, 0, 8));
		q.npcFullDistance = std::isfinite(settings.CustomNpcFullDistance) ? std::clamp(settings.CustomNpcFullDistance, 0.0f, 8192.0f) : 0.0f;
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
		q.npcFullDistance = t.npcFull;
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
	settings.CustomNpcFullDistance = t.npcFull;  // (47q)
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

RayTracing::PointTierParams RayTracing::ResolvePointTier() const
{
	PointTierParams p;
	const int tier = std::clamp(settings.PointQuality, 0, kTierCustom);
	if (tier == kTierCustom) {
		p.k = ValidPointK(settings.PointLightsPerPixel);
		p.kInterior = ValidPointK(settings.PointLightsPerPixelInterior);
		p.maxSources = std::clamp(settings.PointLightsMaxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources));
		p.distance = std::clamp(settings.PointLightsDistance, 512.0f, 8192.0f);
		p.alphaOutdoors = settings.PointLightsAlphaOutdoors;
		p.player = settings.PointLightsPlayer;
		p.effects = settings.PointLightsEffects;
	} else {
		const auto& t = kPointTiers[tier];
		p.k = t.k;
		p.kInterior = t.kInterior;
		p.maxSources = std::min(t.maxSources, static_cast<int>(RayTracingPointLights::kMaxSources));
		p.distance = t.distance;
		p.alphaOutdoors = t.alphaOutdoors;
		p.player = t.player;
		p.effects = t.effects;
	}
	return p;
}

float RayTracing::NpcFullDistance() const
{
	// (47q) The sun's tier decides; with the sun Off the point lights' tier (Low .. Ultra = the sun's Low .. Ultra), then RTAO's. Their
	// Custom: no limit.
	if (settings.SunShadows)
		return ResolveQuality().npcFullDistance;
	const int tier = settings.PointLights ? settings.PointQuality : (settings.AO ? settings.AOQuality : kTierCustom);
	if (tier < 0 || tier >= kTierCustom)
		return 0.0f;
	return kTiers[tier + 1].npcFull;
}

RayTracing::AOTierParams RayTracing::ResolveAOTier() const
{
	AOTierParams p;
	const int tier = std::clamp(settings.AOQuality, 0, kTierCustom);
	if (tier == kTierCustom) {
		p.detailRays = std::clamp(settings.AODetailRays, 1, kAODetailRaysMax);
		p.distance = std::clamp(settings.AODistance, 1000.0f, 8192.0f);
		p.broadRate = std::clamp(settings.AOBroadRate, 0, 2);
		p.fullResFilter = settings.AOFullResFilter;
		p.temporalV2 = settings.AOTemporalV2;
		p.smoothNormals = settings.AOSmoothNormals;
	} else {
		const auto& t = kAOTiers[tier];
		p.detailRays = t.detailRays;
		p.distance = t.distance;
		p.broadRate = t.broadRate;
		p.fullResFilter = t.fullResFilter;
		p.temporalV2 = t.temporalV2;
		p.smoothNormals = t.smoothNormals;
	}
	return p;
}

void RayTracing::CopyPointTierToCustom(int a_tier)
{
	const auto& t = kPointTiers[std::clamp(a_tier, 0, kTierCustom - 1)];
	settings.PointLightsPerPixel = t.k;
	settings.PointLightsPerPixelInterior = t.kInterior;
	settings.PointLightsMaxSources = t.maxSources;
	settings.PointLightsDistance = t.distance;
	settings.PointLightsAlphaOutdoors = t.alphaOutdoors;
	settings.PointLightsPlayer = t.player;  // (47q)
	settings.PointLightsEffects = t.effects;
}

void RayTracing::CopyAOTierToCustom(int a_tier)
{
	const auto& t = kAOTiers[std::clamp(a_tier, 0, kTierCustom - 1)];
	settings.AODetailRays = t.detailRays;
	settings.AODistance = t.distance;
	settings.AOBroadRate = t.broadRate;
	settings.AOFullResFilter = t.fullResFilter;
	settings.AOTemporalV2 = t.temporalV2;
	settings.AOSmoothNormals = t.smoothNormals;
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

int RayTracing::EffectivePointDenoiser() const
{
	// (50b) As the sun's: while Upscaling's Ray Reconstruction runs, the point lights take the DLSS RR route too. A failed v2 shader /
	// texture set falls back to Penumbra for the session.
	if (UpscalingRRActive())
		return pointV2Failed ? kPointPenumbra : kPointRR;
	const int mode = std::clamp(settings.PointDenoiser, 0, kPointModeCount - 1);
	return pointV2Failed && mode != kPointPenumbra ? kPointPenumbra : mode;
}

void RayTracing::OnDenoiserPicked(bool a_point)
{
	// (50b) DLSS RR is global for the sun and the point lights. The page that was picked on already holds its new value.
	const bool sunRR = CurrentDenoiser() == DenoiserMode::DLSSRR;
	const bool pointRR = settings.PointDenoiser == kPointRR;
	if (a_point ? pointRR : sunRR) {
		// RR on this page: the other page follows (its last other choice remembered).
		if (a_point && !sunRR) {
			settings.SunDenoiserNonRR = ValidDenoiser(settings.Denoiser);
			settings.Denoiser = static_cast<int>(DenoiserMode::DLSSRR);
		} else if (!a_point && !pointRR) {
			settings.PointDenoiserNonRR = std::clamp(settings.PointDenoiser, 0, kPointModeCount - 1);
			settings.PointDenoiser = kPointRR;
		}
	} else {
		// Something else on this page: remember it; the other page leaves RR for its last other choice.
		if (a_point) {
			settings.PointDenoiserNonRR = settings.PointDenoiser;
			if (sunRR) {
				const int back = ValidDenoiser(settings.SunDenoiserNonRR);
				settings.Denoiser = back == static_cast<int>(DenoiserMode::DLSSRR) ? static_cast<int>(DenoiserMode::Penumbra) : back;
			}
		} else {
			settings.SunDenoiserNonRR = ValidDenoiser(settings.Denoiser);
			if (pointRR) {
				const int back = std::clamp(settings.PointDenoiserNonRR, 0, kPointModeCount - 1);
				settings.PointDenoiser = back == kPointRR ? kPointV2 : back;
			}
		}
	}
	OnDenoiserPicked();
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
		settings.CellEventMs = read("CellEventMs", defaults.CellEventMs);
		settings.PointLightsBeamSoftness = read("PointLightsBeamSoftness", defaults.PointLightsBeamSoftness);  // (46o)
		// (50a) "Lights Without a Visible Flame": the new key; without it the 46s key, whose Snap (the old default, known to shatter the
		// shadows on characters) migrates once to Soft.
		if (o_json.contains("PointLightsFlameless")) {
			settings.PointLightsMeshless = std::clamp(read("PointLightsFlameless", defaults.PointLightsMeshless), 0, 3);
		} else if (o_json.contains("PointLightsMeshless")) {
			const int old = std::clamp(read("PointLightsMeshless", 1), 0, 2);
			settings.PointLightsMeshless = old == static_cast<int>(RayTracingPointLights::kMeshlessSnap) ? static_cast<int>(RayTracingPointLights::kMeshlessSoft) : old;
			if (old == static_cast<int>(RayTracingPointLights::kMeshlessSnap))
				logger::info("[RayTracing] Lights Without a Visible Flame: saved Snap migrated to Soft (50a)");
		}
		// (46t) RTAO ((47n) Fine Detail, Normal-Guided Filter, Single Long Ray, Character Self-Occlusion Fix and Smooth Normals v2
		// are always on: their keys are ignored and dropped on the next save)
		settings.AODetailStrength = read("AODetailStrength", defaults.AODetailStrength);
		settings.AODetailLength = read("AODetailLength", defaults.AODetailLength);
		settings.AODetailRays = read("AODetailRays", defaults.AODetailRays);
		settings.AOFullResFilter = read("AOFullResFilter", defaults.AOFullResFilter);
		settings.AOAlphaBudget = read("AOAlphaBudget", defaults.AOAlphaBudget);
		// (46y)
		// (47q) AOBroadRate; saved before 47q: from AOHalfResFar (half or every pixel).
		settings.AOBroadRate = read("AOBroadRate", read("AOHalfResFar", false) ? 1 : 0);
		// (47h) (47o) Sun Shadow: Depth Check for Translucent Surfaces, Point Light Shadows on Water and (46y) Screen-Space Shadows for
		// Non-RT Casters are permanent (their keys are ignored and dropped on the next save); Keep Screen-Space Contact Shadows is gone.
		settings.AOSmoothNormals = read("AOSmoothNormals", defaults.AOSmoothNormals);  // (47j)
		settings.AOTemporalV2 = read("AOTemporalV2", defaults.AOTemporalV2);  // (47l)
		settings.FoliageSway = read("FoliageSway", defaults.FoliageSway);  // (47l)
		// (47n)
		settings.SunShadows = read("SunShadows", defaults.SunShadows);
		settings.AOSpecOcc = read("AOSpecOcc", defaults.AOSpecOcc);  // (49a)
		settings.AOSpecOccStrength = std::clamp(read("AOSpecOccStrength", defaults.AOSpecOccStrength), 0.0f, 1.0f);
		settings.ParticlesRTShadows = read("ParticlesRTShadows", defaults.ParticlesRTShadows);  // (49b)
		settings.ParticlesSmoothSheets = read("ParticlesSmoothSheets", defaults.ParticlesSmoothSheets);  // (50a)
		// (50b) Point Light Shadows > Denoiser (no key: Penumbra v2), the pages' last choices other than DLSS RR, Blue-Noise Sampling.
		settings.PointDenoiser = std::clamp(read("PointDenoiser", defaults.PointDenoiser), 0, static_cast<int>(kPointModeCount) - 1);
		settings.PointDenoiserNonRR = std::clamp(read("PointDenoiserNonRR", defaults.PointDenoiserNonRR), 0, static_cast<int>(kPointRR) - 1);
		settings.SunDenoiserNonRR = ValidDenoiser(read("SunDenoiserNonRR", defaults.SunDenoiserNonRR));
		if (settings.SunDenoiserNonRR == static_cast<int>(DenoiserMode::DLSSRR))
			settings.SunDenoiserNonRR = static_cast<int>(DenoiserMode::Penumbra);
		settings.PointBlueNoise = read("PointBlueNoise", defaults.PointBlueNoise);
		settings.PointV2CleanHistory = read("PointV2CleanHistory", defaults.PointV2CleanHistory);  // (50e)
		settings.PointRRWhiteNoise = read("PointRRWhiteNoise", defaults.PointRRWhiteNoise);  // (50e)
		settings.LoadFinish = read("LoadFinish", defaults.LoadFinish);  // (49c) (49b Hitch* keys: no longer read)
		settings.PointQuality = read("PointQuality", -1);
		settings.AOQuality = read("AOQuality", -1);
		// (47q) The sun's size left the tiers. Saved before 47q (no key): the cone the tier in effect had -- High / Ultra 0.5, Very Low -
		// Medium 0.27, Custom its CustomSunCone -- so the picture stays as it was.
		settings.CustomNpcFullDistance = read("CustomNpcFullDistance", defaults.CustomNpcFullDistance);
		// (48o) The confirmed fixes and speed-ups of 47n-48m (point-light noise fix and its parts, clean / kept shadow history,
		// smoother edges, curved ranking, close contact, beam range / smooth edges, stable light choice and its jitter fix, extra
		// slots, probe sharing, keep leaves moving and learn on any frame, start sun trace earlier, skip irrelevant updates, the
		// sun's uniform-tile fast path, the SSS tile mask, smooth blur width, keep buildings in the sun map, RTAO tight history and
		// filter only where traced) are fixed on; Spatial Static Chunks and Skip the Game's Shadow Maps (with Adaptive) fixed off.
		// Their keys (and 48l's PointNoiseFastHistory) are ignored and dropped on the next save.
		// (48k) RT reflections hidden: the saved mode is only kept for writing back; the renderer always runs Off.
		reflectionsModeSaved = std::clamp(read("ReflectionsMode", defaults.ReflectionsMode), 0, 2);
		settings.ReflectionsMode = 0;
		settings.ReflectionsTerrain = read("ReflectionsTerrain", defaults.ReflectionsTerrain);
		// (48d)
		settings.ReflectionsRoughStart = std::clamp(read("ReflectionsRoughStart", defaults.ReflectionsRoughStart), 0.0f, 0.95f);
		settings.ReflectionsRoughEnd = std::clamp(read("ReflectionsRoughEnd", defaults.ReflectionsRoughEnd), 0.05f, 1.0f);
		settings.ReflectionsFullRange = read("ReflectionsFullRange", defaults.ReflectionsFullRange);
		settings.ReflectionsBeyondSSR = read("ReflectionsBeyondSSR", defaults.ReflectionsBeyondSSR);
		settings.ReflectionsRoughSolid = read("ReflectionsRoughSolid", defaults.ReflectionsRoughSolid);
		settings.ReflectionsBestCandidate = read("ReflectionsBestCandidate", defaults.ReflectionsBestCandidate);
		settings.ReflectionsLobeCheck = read("ReflectionsLobeCheck", defaults.ReflectionsLobeCheck);
		settings.ReflectionsCubemapMatch = read("ReflectionsCubemapMatch", defaults.ReflectionsCubemapMatch);
		settings.ReflectionsHitDistance = read("ReflectionsHitDistance", defaults.ReflectionsHitDistance);
		settings.ReflectionsLoadingPrep = read("ReflectionsLoadingPrep", defaults.ReflectionsLoadingPrep);  // (48e)
		settings.ReflectionsCubemapWarmUp = read("ReflectionsCubemapWarmUp", defaults.ReflectionsCubemapWarmUp);
		settings.ReflectionsMaterials = read("ReflectionsMaterials", defaults.ReflectionsMaterials);  // (48f)
		settings.ReflectionsOffscreenColor = std::clamp(read("ReflectionsOffscreenColor", defaults.ReflectionsOffscreenColor), 0, 1);  // (48g)
		settings.ReflectionsOffscreenBrightness = std::clamp(read("ReflectionsOffscreenBrightness", defaults.ReflectionsOffscreenBrightness), 0.25f, 4.0f);
		settings.ReflectionsSunRay = read("ReflectionsSunRay", defaults.ReflectionsSunRay);
		settings.ReflectionsShadeLights = read("ReflectionsShadeLights", defaults.ReflectionsShadeLights);
		settings.ReflectionsCharacterMaterials = read("ReflectionsCharacterMaterials", defaults.ReflectionsCharacterMaterials);
		settings.ReflectionsShadeGlow = read("ReflectionsShadeGlow", defaults.ReflectionsShadeGlow);  // (48h)
		settings.ReflectionsPreferBrightCubemap = read("ReflectionsPreferBrightCubemap", defaults.ReflectionsPreferBrightCubemap);
		// (48i) The reflection Quality preset. Loading never changes a value: a saved preset whose values no longer match, or no key
		// (saved before 48i), shows the preset the values match, else Custom.
		{
			const ReflectionPresetValues live = CurrentReflectionValues();
			ReflectionPresetValues custom = live;
			if (const auto it = o_json.find("ReflectionsCustom"); it != o_json.end() && it->is_object()) {
				const json& c = *it;
				const auto field = [&c](const char* a_key, auto a_default) {
					const auto f = c.find(a_key);
					if (f == c.end())
						return a_default;
					try {
						return f->template get<decltype(a_default)>();
					} catch (const std::exception&) {
						return a_default;
					}
				};
				custom.terrain = field("Terrain", live.terrain);
				custom.fullRange = field("FullRange", live.fullRange);
				custom.roughStart = std::clamp(field("RoughStart", live.roughStart), 0.0f, 0.95f);
				custom.roughEnd = std::clamp(field("RoughEnd", live.roughEnd), 0.05f, 1.0f);
				custom.beyondSSR = field("BeyondSSR", live.beyondSSR);
				custom.roughSolid = field("RoughSolid", live.roughSolid);
				custom.materials = field("Materials", live.materials);
				custom.characters = field("CharacterMaterials", live.characters);
				custom.offscreenColor = std::clamp(field("OffscreenColor", live.offscreenColor), 0, 1);
				custom.sunRay = field("SunRay", live.sunRay);
				custom.lamps = field("ShadeLights", live.lamps);
				custom.glow = field("ShadeGlow", live.glow);
				custom.preferBright = field("PreferBrightCubemap", live.preferBright);
			}
			const int saved = read("ReflectionsQuality", -1);
			const int matched = MatchReflectionPreset();
			if (saved == kReflectionCustom)
				settings.ReflectionsQuality = kReflectionCustom;
			else if (saved >= 0 && saved < kReflectionCustom)
				settings.ReflectionsQuality = matched == saved ? saved : kReflectionCustom;
			else
				settings.ReflectionsQuality = matched;
			// In Custom the Custom values are the live ones.
			settings.ReflectionsCustom = settings.ReflectionsQuality == kReflectionCustom ? live : custom;
			if (saved != settings.ReflectionsQuality)
				logger::info("[RayTracing] RT reflections quality (48i): {} ({}; values unchanged)", kReflectionQualityNames[settings.ReflectionsQuality],
					saved < 0 ? "no saved preset: the one the saved values match" : "the saved preset's values did not match the saved values");
		}
		const float sunSize = read("SunSize", -1.0f);
		if (sunSize >= 0.0f) {
			settings.SunSize = sunSize;
		} else {
			const int level = std::clamp(read("QualityLevel", static_cast<int>(Quality::Medium)), 0, static_cast<int>(Quality::Count) - 1);
			if (level == static_cast<int>(Quality::Custom))
				settings.SunSize = read("CustomSunCone", kSunSizeDefault);
			else
				settings.SunSize = level >= static_cast<int>(Quality::High) ? 0.5f : kSunSizeDefault;
			logger::info("[RayTracing] Shadow Edge Softness (sun size, 47q): {:.2f} degrees (the sun quality's cone until 47p)", settings.SunSize);
		}
	}
	// (47m) (47x) The Skip the Game's Shadow Maps migrations (versions 7, 8) are moot: (48o) it is fixed off.
	if (settings.Denoiser == 1)
		logger::info("[RayTracing] Denoiser Temporal was removed: Penumbra");
	settings.Denoiser = ValidDenoiser(settings.Denoiser);
	// (50b) DLSS RR is one choice for the sun and the point lights: a saved RR on either page means both (the other page's own value is
	// kept as its last other choice).
	if (settings.Denoiser == static_cast<int>(DenoiserMode::DLSSRR) && settings.PointDenoiser != kPointRR) {
		settings.PointDenoiserNonRR = settings.PointDenoiser;
		settings.PointDenoiser = kPointRR;
		logger::info("[RayTracing] Point Light Shadows denoiser: DLSS RR (the sun's denoiser is DLSS RR; 50b: one choice for both)");
	} else if (settings.PointDenoiser == kPointRR && settings.Denoiser != static_cast<int>(DenoiserMode::DLSSRR)) {
		settings.SunDenoiserNonRR = settings.Denoiser;
		settings.Denoiser = static_cast<int>(DenoiserMode::DLSSRR);
	}
	settings.SettingsVersion = Settings{}.SettingsVersion;
	settings.QualityLevel = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
	settings.CustomBaseRays = std::clamp(settings.CustomBaseRays, 1, 4);
	settings.CustomExtraRays = std::clamp(settings.CustomExtraRays, 0, 8);
	settings.SunSize = std::isfinite(settings.SunSize) ? std::clamp(settings.SunSize, kSunSizeMin, kSunSizeMax) : kSunSizeDefault;  // (47q)
	settings.CustomNpcFullDistance = std::isfinite(settings.CustomNpcFullDistance) ? std::clamp(settings.CustomNpcFullDistance, 0.0f, 8192.0f) : 3150.0f;
	settings.AOBroadRate = std::clamp(settings.AOBroadRate, 0, 2);
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
	// (50b) Trees and Foliage are one "Trees and Plants" choice: leaves traced without their trunks is no longer offered. Such a saved pair
	// (not the defaults, Trees false with Foliage 0) loads with the trunks traced as well (one log line); every other pair is a choice.
	if (!settings.Trees && settings.Foliage != 0) {
		settings.Trees = true;
		logger::info("[RayTracing] Trees and Plants (50b): leaves were traced without their trunks; the trunks are traced too now");
	}
	settings.RtContactScale = std::isfinite(settings.RtContactScale) ? std::clamp(settings.RtContactScale, 0.0f, 32.0f) : 8.0f;
	// (46b)
	settings.PointLightsPerPixel = ValidPointK(settings.PointLightsPerPixel);
	settings.PointLightsMaxSources = std::clamp(settings.PointLightsMaxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources));
	settings.PointLightsDistance = std::isfinite(settings.PointLightsDistance) ? std::clamp(settings.PointLightsDistance, 512.0f, 8192.0f) : 4000.0f;
	settings.PointLightsSize = std::isfinite(settings.PointLightsSize) ? std::clamp(settings.PointLightsSize, 0.0f, 32.0f) : 6.0f;
	// (46f)
	settings.AOMode = std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1);
	settings.AORayLength = std::isfinite(settings.AORayLength) ? std::clamp(settings.AORayLength, kAORayLengthMin, kAORayLengthMax) : 100.0f;
	settings.AOStrength = std::isfinite(settings.AOStrength) ? std::clamp(settings.AOStrength, 0.0f, 2.0f) : 1.0f;
	settings.AODistance = std::isfinite(settings.AODistance) ? std::clamp(settings.AODistance, 1000.0f, 8192.0f) : 4000.0f;
	settings.AOModeInterior = std::clamp(settings.AOModeInterior, 0, static_cast<int>(AOModeKind::Count) - 1);  // (46h)
	// (46t)
	settings.AODetailStrength = std::isfinite(settings.AODetailStrength) ? std::clamp(settings.AODetailStrength, 0.0f, 2.0f) : 1.0f;
	settings.AODetailLength = std::isfinite(settings.AODetailLength) ? std::clamp(settings.AODetailLength, kAODetailLengthMin, kAODetailLengthMax) : 20.0f;
	settings.AODetailRays = std::clamp(settings.AODetailRays, 1, kAODetailRaysMax);
	settings.AOAlphaBudget = std::clamp(settings.AOAlphaBudget, 0, 2);
	// (46k)
	settings.LoadingPumpMs = std::isfinite(settings.LoadingPumpMs) ? std::clamp(settings.LoadingPumpMs, 2.0f, 32.0f) : 8.0f;
	settings.BlackWindowMs = std::isfinite(settings.BlackWindowMs) ? std::clamp(settings.BlackWindowMs, 0.0f, 2000.0f) : 200.0f;
	settings.CellEventMs = std::isfinite(settings.CellEventMs) ? std::clamp(settings.CellEventMs, 2.0f, 50.0f) : 12.0f;  // (46n)
	settings.PointLightsBeamSoftness = std::isfinite(settings.PointLightsBeamSoftness) ? std::clamp(settings.PointLightsBeamSoftness, 0.0f, 3.0f) : 0.5f;  // (46o)
	settings.PointLightsPerPixelInterior = ValidPointK(settings.PointLightsPerPixelInterior);
	// (47n) Point-light / RTAO tiers. Saved before 47n (no key): the tier whose values equal the saved ones, else Custom (the saved values
	// are Custom's): nothing changes on screen.
	if (settings.PointQuality < 0 || settings.PointQuality > kTierCustom) {
		settings.PointQuality = kTierCustom;
		for (int t = 0; t < kTierCustom; ++t) {
			const auto& tier = kPointTiers[t];
			// (48a) The tiers' indoor K before 48a (1 / 2 / 3 / 4) still identify a tier saved without the key; it then takes the new table.
			constexpr int kLegacyInterior[4] = { 1, 2, 3, 4 };
			const bool interiorMatches = settings.PointLightsPerPixelInterior == tier.kInterior || settings.PointLightsPerPixelInterior == kLegacyInterior[t];
			if (settings.PointLightsPerPixel == tier.k && interiorMatches && settings.PointLightsMaxSources == tier.maxSources &&
				std::abs(settings.PointLightsDistance - tier.distance) < 0.5f && settings.PointLightsAlphaOutdoors == tier.alphaOutdoors) {
				settings.PointQuality = t;
				break;
			}
		}
		logger::info("[RayTracing] Point-light shadows quality (47n): {} (from the saved values)", kTierNames[settings.PointQuality + 1]);
	}
	if (settings.AOQuality < 0 || settings.AOQuality > kTierCustom) {
		settings.AOQuality = kTierCustom;
		for (int t = 0; t < kTierCustom; ++t) {
			const auto& tier = kAOTiers[t];
			if (settings.AODetailRays == tier.detailRays && std::abs(settings.AODistance - tier.distance) < 0.5f && settings.AOBroadRate == tier.broadRate &&
				settings.AOFullResFilter == tier.fullResFilter && settings.AOTemporalV2 == tier.temporalV2 && settings.AOSmoothNormals == tier.smoothNormals) {
				settings.AOQuality = t;
				break;
			}
		}
		logger::info("[RayTracing] RTAO quality (47n): {} (from the saved values)", kTierNames[settings.AOQuality + 1]);
	}
}

void RayTracing::SaveSettings(json& o_json)
{
	o_json = settings;
	// (46n) Outside the macro (it takes at most 64 members).
	o_json["CellEventMs"] = settings.CellEventMs;
	o_json["PointLightsBeamSoftness"] = settings.PointLightsBeamSoftness;  // (46o)
	o_json["PointLightsFlameless"] = settings.PointLightsMeshless;  // (50a)
	// (50a) The 46s key for a package before 50a (Soft has no value there: Snap, its old default).
	o_json["PointLightsMeshless"] = settings.PointLightsMeshless == static_cast<int>(RayTracingPointLights::kMeshlessSoft) ?
	                                    static_cast<int>(RayTracingPointLights::kMeshlessSnap) :
	                                    std::clamp(settings.PointLightsMeshless, 0, 2);
	o_json["AODetailStrength"] = settings.AODetailStrength;  // (46t)
	o_json["AODetailLength"] = settings.AODetailLength;
	o_json["AODetailRays"] = settings.AODetailRays;
	o_json["AOFullResFilter"] = settings.AOFullResFilter;
	o_json["AOAlphaBudget"] = settings.AOAlphaBudget;
	o_json["AOBroadRate"] = settings.AOBroadRate;  // (47q) replaces AOHalfResFar
	o_json["SunSize"] = settings.SunSize;          // (47q)
	o_json["CustomNpcFullDistance"] = settings.CustomNpcFullDistance;
	o_json["ReflectionsMode"] = reflectionsModeSaved;  // (48k) as loaded (the runtime value is always 0)
	o_json["ReflectionsTerrain"] = settings.ReflectionsTerrain;
	o_json["ReflectionsRoughStart"] = settings.ReflectionsRoughStart;  // (48d)
	o_json["ReflectionsRoughEnd"] = settings.ReflectionsRoughEnd;
	o_json["ReflectionsFullRange"] = settings.ReflectionsFullRange;
	o_json["ReflectionsBeyondSSR"] = settings.ReflectionsBeyondSSR;
	o_json["ReflectionsRoughSolid"] = settings.ReflectionsRoughSolid;
	o_json["ReflectionsBestCandidate"] = settings.ReflectionsBestCandidate;
	o_json["ReflectionsLobeCheck"] = settings.ReflectionsLobeCheck;
	o_json["ReflectionsCubemapMatch"] = settings.ReflectionsCubemapMatch;
	o_json["ReflectionsHitDistance"] = settings.ReflectionsHitDistance;
	o_json["ReflectionsLoadingPrep"] = settings.ReflectionsLoadingPrep;  // (48e)
	o_json["ReflectionsCubemapWarmUp"] = settings.ReflectionsCubemapWarmUp;
	o_json["ReflectionsMaterials"] = settings.ReflectionsMaterials;  // (48f)
	o_json["ReflectionsOffscreenColor"] = settings.ReflectionsOffscreenColor;  // (48g)
	o_json["ReflectionsOffscreenBrightness"] = settings.ReflectionsOffscreenBrightness;
	o_json["ReflectionsSunRay"] = settings.ReflectionsSunRay;
	o_json["ReflectionsShadeLights"] = settings.ReflectionsShadeLights;
	o_json["ReflectionsCharacterMaterials"] = settings.ReflectionsCharacterMaterials;
	o_json["ReflectionsShadeGlow"] = settings.ReflectionsShadeGlow;  // (48h)
	o_json["ReflectionsPreferBrightCubemap"] = settings.ReflectionsPreferBrightCubemap;
	o_json["ReflectionsQuality"] = settings.ReflectionsQuality;  // (48i)
	{
		const ReflectionPresetValues c = settings.ReflectionsQuality == kReflectionCustom ? CurrentReflectionValues() : settings.ReflectionsCustom;
		o_json["ReflectionsCustom"] = { { "Terrain", c.terrain }, { "FullRange", c.fullRange }, { "RoughStart", c.roughStart }, { "RoughEnd", c.roughEnd },
			{ "BeyondSSR", c.beyondSSR }, { "RoughSolid", c.roughSolid }, { "Materials", c.materials }, { "CharacterMaterials", c.characters },
			{ "OffscreenColor", c.offscreenColor }, { "SunRay", c.sunRay }, { "ShadeLights", c.lamps }, { "ShadeGlow", c.glow },
			{ "PreferBrightCubemap", c.preferBright } };
	}
	o_json["AOSmoothNormals"] = settings.AOSmoothNormals;  // (47j)
	o_json["AOTemporalV2"] = settings.AOTemporalV2;  // (47l)
	o_json["FoliageSway"] = settings.FoliageSway;  // (47l)
	o_json["SunShadows"] = settings.SunShadows;  // (47n)
	o_json["PointQuality"] = settings.PointQuality;
	o_json["AOQuality"] = settings.AOQuality;
	o_json["AOSpecOcc"] = settings.AOSpecOcc;  // (49a)
	o_json["AOSpecOccStrength"] = settings.AOSpecOccStrength;
	o_json["ParticlesRTShadows"] = settings.ParticlesRTShadows;  // (49b)
	o_json["ParticlesSmoothSheets"] = settings.ParticlesSmoothSheets;  // (50a)
	o_json["PointDenoiser"] = settings.PointDenoiser;  // (50b)
	o_json["PointDenoiserNonRR"] = settings.PointDenoiserNonRR;
	o_json["SunDenoiserNonRR"] = settings.SunDenoiserNonRR;
	o_json["PointBlueNoise"] = settings.PointBlueNoise;
	o_json["PointV2CleanHistory"] = settings.PointV2CleanHistory;  // (50e)
	o_json["PointRRWhiteNoise"] = settings.PointRRWhiteNoise;  // (50e)
	o_json["LoadFinish"] = settings.LoadFinish;  // (49c)
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
	// (49c) Log-only probe of the LoadingMenu's hide message (IMenu::ProcessMessage, slot 4).
	stl::write_vfunc<0x4, LoadingMenuProcessMessage>(RE::VTABLE_LoadingMenu[0]);
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
	// (47w) Adaptive skip: (48o) fixed off (as is Skip the Game's Shadow Maps itself, so this function is never reached).
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

void RayTracing::UpdateCoverage()
{
	// (47w) Adaptive skip: fold in the newest read-back of the trace's per-light coverage counts (render thread, LatchFrame).
	const auto& stats = backend.GetStats();
	const std::uint32_t frame = globals::state->frameCount;
	if (stats.coverageSamples != coverageSamplesSeen) {
		coverageSamplesSeen = stats.coverageSamples;
		coverageShareMax = -1.0f;
		for (std::uint32_t i = 0; i < stats.coverageCount && i < stats.coverage.size(); ++i) {
			const auto& c = stats.coverage[i];
			auto& state = coverageStates[static_cast<const RE::BSLight*>(c.light)];
			// Frames since the last read-back holding this light (several frames can arrive in one read-back): the good run counts frames.
			const std::uint32_t frames = state.lastSampleFrame != 0 ? std::clamp<std::uint32_t>(frame - state.lastSampleFrame, 1u, 8u) : 1u;
			state.lastSampleFrame = frame;
			state.share = c.lit > 0 ? static_cast<float>(c.uncovered) / static_cast<float>(c.lit) : 0.0f;
			coverageShareMax = std::max(coverageShareMax, state.share);
			const bool bad = (state.share > kCoverageBadShare && c.uncovered >= kCoverageBadMinPixels) || c.uncovered >= kCoverageBadPixels;
			const bool good = state.share < kCoverageGoodShare && c.uncovered < kCoverageGoodPixels;
			if (bad) {
				if (state.skipAllowed)
					++restoreEvents;
				state.skipAllowed = false;
				state.everRestored = true;
				state.goodFrames = 0;
			} else if (good) {
				state.goodFrames += frames;
				if (!state.skipAllowed && state.goodFrames >= (state.everRestored ? kCoverageRestoreFrames : kCoverageEnterFrames))
					state.skipAllowed = true;
			} else {
				state.goodFrames = 0;  // between the two thresholds: the current state holds, the good run starts over
			}
		}
	}
	// Lights not counted for a long time (gone, another cell): forgotten.
	if ((frame & 63u) == 0u) {
		for (auto it = coverageStates.begin(); it != coverageStates.end();) {
			if (frame - it->second.lastSampleFrame > kCoverageForgetFrames)
				it = coverageStates.erase(it);
			else
				++it;
		}
	}
}

void RayTracing::ClearCoverage()
{
	coverageStates.clear();
	coverageShareMax = -1.0f;
}

void RayTracing::SetupResources()
{
	combineCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<CombineCB>());
	pointDenoiseCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PointDenoiseCB>());  // (46b)
	pointMaskCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PointMaskCB>());        // (46c)
	aoCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<AOCompositeCB>());            // (46f)
	specOccCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<SpecOccCompositeCB>());  // (49a)

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
	pointDenoiseBeamCS = nullptr;  // (48a)
	pointDenoiseWideCS = nullptr;
	pointDenoiseWideBeamCS = nullptr;
	for (auto& fixShader : pointDenoiseFixCS)  // (48k)
		fixShader = nullptr;
	for (auto& cleanShader : pointDenoiseCleanCS)  // (48m)
		cleanShader = nullptr;
	for (auto& v2Shader : pointDenoiseV2CS)  // (50b)
		v2Shader = nullptr;
	aoNormalReflectCS = nullptr;
	reflectDebugCS = nullptr;
	aoNormalReflectAsync.Reset();  // (48e) compiled again in the background on the next request
	reflectDebugAsync.Reset();
	pointMaskCS = nullptr;     // (46c)
	forwardMergeCS = nullptr;  // (47z)
	// (46f)
	aoTemporalCS = nullptr;
	aoCompositeCS = nullptr;
	// (46t)
	for (auto* shader : { &aoFDownCS, &aoFBlurHCS, &aoFBlurVCS, &aoFUpCS, &aoFFullHCS, &aoFFullVCS, &aoFDetailCS, &aoDetailTemporalCS })
		*shader = nullptr;
	aoTemporalV2CS = nullptr;  // (47l)
	specOccHCS = nullptr;  // (49a)
	specOccVCS = nullptr;
	for (auto* shader : { &aoTileMarkCS, &aoTileDistHCS, &aoTileDistVCS, &aoTemporalV2WideCS, &sssCasterTilesCS })  // (47r)
		*shader = nullptr;
}

void RayTracing::PrecompileShaders()
{
	// (46p) What the first traced frame would compile on the spot (26.6 ms in 46o's post-load log): the same getters, now.
	if (!loaded || !RtWanted())
		return;
	GetShader(prepareCS, L"Data\\Shaders\\RayTracing\\PrepareCS.hlsl", "PrepareCS");
	if (SunOn())
		GetShader(combineCS, L"Data\\Shaders\\RayTracing\\CombineCS.hlsl", "CombineCS");
	if (SunOn())  // (47z) Keep Buildings in the Game's Sun Shadow Map (always on since 48o)
		GetShader(forwardMergeCS, L"Data\\Shaders\\RayTracing\\ForwardShadowMergeCS.hlsl", "ForwardShadowMergeCS");
	if (SunOn() && ValidDenoiser(settings.Denoiser) == static_cast<int>(DenoiserMode::Penumbra)) {
		GetShader(denoiseTileCS, L"Data\\Shaders\\RayTracing\\DenoiseTileCS.hlsl", "DenoiseTileCS");
		GetShaderDefined(denoiseHalfHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (half)", "DENOISE_HALF");
		GetShaderDefined(denoiseHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS", nullptr);
		GetShaderDefined(denoiseHalfVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (half, vertical)", "DENOISE_HALF,DENOISE_VERTICAL");
		GetShaderDefined(denoiseVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (vertical)", "DENOISE_VERTICAL");
	}
	if (settings.PointLights && (settings.PointDenoiser != kPointPenumbra || UpscalingRRActive())) {
		// (50b) Off / Penumbra v2 / DLSS RR: PointDenoiseV2CS (both variants: Extra Lights Where Crowded can ask for slots 4 - 5 anywhere).
		PointDenoiseV2Shader(false);
		PointDenoiseV2Shader(true);
		if (settings.PointV2CleanHistory && !pointV2CleanFailed) {  // (50e) Moving Characters: Clean Shadow History
			PointDenoiseV2Shader(false, true);
			PointDenoiseV2Shader(true, true);
		}
		GetShader(pointMaskCS, L"Data\\Shaders\\RayTracing\\PointMaskCS.hlsl", "PointMaskCS");
	}
	if (settings.PointLights && settings.PointDenoiser == kPointPenumbra) {
		GetShader(pointDenoiseCS, L"Data\\Shaders\\RayTracing\\PointDenoiseCS.hlsl", "PointDenoiseCS");
		GetShader(pointMaskCS, L"Data\\Shaders\\RayTracing\\PointMaskCS.hlsl", "PointMaskCS");
		// (48a) The variants the current settings can use (window beams are indoor-only; K > 4 per the tier).
		const PointTierParams pt = ResolvePointTier();
		// (49b) Hitch fix D: Extra Lights Where Crowded (always on since 48o, K < 6) can ask for slots 4 - 5 at any tier: their variants too.
		const bool wideK = pt.k > 4 || (settings.PointLightsInteriors && pt.kInterior > 4) || ReserveOn();
		const bool beam = settings.PointLightsInteriors && settings.PointLightsWindowBeams;  // (48o) Window Beams: Smooth Edges always on
		if (beam)
			PointDenoiseShader(false, true);
		if (wideK)
			PointDenoiseShader(true, beam);
		// (48k) Point Lights: Noise Fix: its twins of the same variants (the plain one always).
		// (48m) + Moving Characters: Clean Shadow History: the PL_CLEAN twins of what runs.
		const bool fix = PointNoiseFixDenoiserWanted() && !pointNoiseFixFailed;
		const bool clean = !pointCleanFailed;  // (48o) Clean Shadow History always on
		if (fix || clean) {
			PointDenoiseShader(false, false, fix, clean);
			if (beam)
				PointDenoiseShader(false, true, fix, clean);
			if (wideK)
				PointDenoiseShader(true, beam, fix, clean);
			// (49b) Hitch fix D: the wide variant without beam edges too (outdoors the beam mask is empty, so a beam setting does not pick it there).
			if (wideK && beam && ReserveOn())
				PointDenoiseShader(true, false, fix, clean);
		}
	}
	if (settings.AO) {
		// (46t) The RTAO filter of the current switches (the others compile when first switched on).
		const wchar_t* kFilter = L"Data\\Shaders\\RayTracing\\RTAOFilterCS.hlsl";
		GetShader(aoTemporalCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS");
		GetShader(aoCompositeCS, L"Data\\Shaders\\RayTracing\\RTAOCompositeCS.hlsl", "RTAOCompositeCS");
		const AOTierParams aoTier = ResolveAOTier();
		if (aoTier.fullResFilter) {
			GetShaderDefined(aoFFullHCS, kFilter, "RTAOFilterCS (full, horizontal)", "RTAO_FULL");
			GetShaderDefined(aoFFullVCS, kFilter, "RTAOFilterCS (full, vertical)", "RTAO_FULL,RTAO_VERTICAL");
		} else {
			GetShaderDefined(aoFDownCS, kFilter, "RTAOFilterCS (down)", "RTAO_DOWN");
			GetShaderDefined(aoFBlurHCS, kFilter, "RTAOFilterCS (blur)", "RTAO_BLUR");
			GetShaderDefined(aoFBlurVCS, kFilter, "RTAOFilterCS (blur, vertical)", "RTAO_BLUR,RTAO_VERTICAL");
			GetShaderDefined(aoFUpCS, kFilter, "RTAOFilterCS (up)", "RTAO_UP");
		}
		GetShaderDefined(aoFDetailCS, kFilter, "RTAOFilterCS (detail)", "RTAO_DETAIL");
		GetShaderDefined(aoDetailTemporalCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS (detail)", "RTAO_DETAIL_HISTORY");
		if (settings.AOSpecOcc) {  // (49a)
			GetShader(specOccHCS, L"Data\\Shaders\\RayTracing\\RTSpecOccCS.hlsl", "RTSpecOccCS (horizontal)");
			GetShaderDefined(specOccVCS, L"Data\\Shaders\\RayTracing\\RTSpecOccCS.hlsl", "RTSpecOccCS (vertical)", "RTSO_VERTICAL");
		}
		if (settings.AOTemporal && aoTier.temporalV2)  // (47l)
			GetShaderDefined(aoTemporalV2CS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS (v2)", "RTAO_TEMPORAL_V2");
		// (47r) RTAO: Filter Only Where Traced (always on since 48o).
		GetShaderDefined(aoTileMarkCS, kFilter, "RTAOFilterCS (tile mark)", "RTAO_TILE_MARK");
		GetShaderDefined(aoTileDistHCS, kFilter, "RTAOFilterCS (tile distance)", "RTAO_TILE_DIST");
		GetShaderDefined(aoTileDistVCS, kFilter, "RTAOFilterCS (tile distance, vertical)", "RTAO_TILE_DIST,RTAO_VERTICAL");
		if (settings.AOTemporal && aoTier.temporalV2)
			GetShaderDefined(aoTemporalV2WideCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS (v2, 16x16)", "RTAO_TEMPORAL_V2,RTAO_WIDE");
	}
}

bool RayTracing::PointNoiseFixDenoiserWanted() const
{
	// (48o) Point Lights: Noise Fix and its denoiser parts are always on.
	return true;
}

bool RayTracing::PointDenoiserDebugView() const
{
	return (debugView >= DebugView::PLCandidates && debugView <= DebugView::PLDenoised) || debugView == DebugView::PLMoving ||
	       debugView == DebugView::PLFilterPath;  // (48m) + its two views
}

ID3D11ComputeShader* RayTracing::PointDenoiseShader(bool a_wide, bool a_beam, bool a_fix, bool a_clean)
{
	// (48a) The plain variant is the 47z shader; the others compile their extra code in only when it can run this frame.
	const wchar_t* kPath = L"Data\\Shaders\\RayTracing\\PointDenoiseCS.hlsl";
	if (a_clean) {
		// (48m) Moving Characters: Clean Shadow History: PL_CLEAN with any of the others (without it its code is not compiled).
		static constexpr const char* kCleanDefines[8] = { "PL_CLEAN", "PL_WIDE,PL_CLEAN", "PL_BEAM,PL_CLEAN", "PL_WIDE,PL_BEAM,PL_CLEAN", "PL_FIX,PL_CLEAN",
			"PL_WIDE,PL_FIX,PL_CLEAN", "PL_BEAM,PL_FIX,PL_CLEAN", "PL_WIDE,PL_BEAM,PL_FIX,PL_CLEAN" };
		static constexpr const char* kCleanNames[8] = { "PointDenoiseCS (clean history)", "PointDenoiseCS (slots 4-5, clean history)",
			"PointDenoiseCS (beam edges, clean history)", "PointDenoiseCS (slots 4-5, beam edges, clean history)", "PointDenoiseCS (noise fix, clean history)",
			"PointDenoiseCS (slots 4-5, noise fix, clean history)", "PointDenoiseCS (beam edges, noise fix, clean history)",
			"PointDenoiseCS (slots 4-5, beam edges, noise fix, clean history)" };
		const int index = (a_wide ? 1 : 0) | (a_beam ? 2 : 0) | (a_fix ? 4 : 0);
		return GetShaderDefined(pointDenoiseCleanCS[index], kPath, kCleanNames[index], kCleanDefines[index]);
	}
	if (a_fix) {
		// (48k) Point Lights: Noise Fix: the same variants with PL_FIX (without it the fix's code folds away: the 48j shaders).
		static constexpr const char* kFixDefines[4] = { "PL_FIX", "PL_WIDE,PL_FIX", "PL_BEAM,PL_FIX", "PL_WIDE,PL_BEAM,PL_FIX" };
		static constexpr const char* kFixNames[4] = { "PointDenoiseCS (noise fix)", "PointDenoiseCS (slots 4-5, noise fix)", "PointDenoiseCS (beam edges, noise fix)",
			"PointDenoiseCS (slots 4-5, beam edges, noise fix)" };
		const int index = (a_wide ? 1 : 0) | (a_beam ? 2 : 0);
		return GetShaderDefined(pointDenoiseFixCS[index], kPath, kFixNames[index], kFixDefines[index]);
	}
	if (a_wide && a_beam)
		return GetShaderDefined(pointDenoiseWideBeamCS, kPath, "PointDenoiseCS (slots 4-5, beam edges)", "PL_WIDE,PL_BEAM");
	if (a_wide)
		return GetShaderDefined(pointDenoiseWideCS, kPath, "PointDenoiseCS (slots 4-5)", "PL_WIDE");
	if (a_beam)
		return GetShaderDefined(pointDenoiseBeamCS, kPath, "PointDenoiseCS (beam edges)", "PL_BEAM");
	return GetShader(pointDenoiseCS, kPath, "PointDenoiseCS");
}

ID3D11ComputeShader* RayTracing::PointDenoiseV2Shader(bool a_wide, bool a_clean)
{
	// (50b) Penumbra v2 / Off / DLSS RR: one shader, PL_WIDE for slots 4 - 5. (50e) PL_CLEAN twins: Moving Characters: Clean Shadow History
	// (without it its code is not compiled: the 50d shaders).
	const wchar_t* kPath = L"Data\\Shaders\\RayTracing\\PointDenoiseV2CS.hlsl";
	if (a_clean && a_wide)
		return GetShaderDefined(pointDenoiseV2CS[3], kPath, "PointDenoiseV2CS (slots 4-5, clean history)", "PL_WIDE,PL_CLEAN");
	if (a_clean)
		return GetShaderDefined(pointDenoiseV2CS[2], kPath, "PointDenoiseV2CS (clean history)", "PL_CLEAN");
	if (a_wide)
		return GetShaderDefined(pointDenoiseV2CS[1], kPath, "PointDenoiseV2CS (slots 4-5)", "PL_WIDE");
	return GetShader(pointDenoiseV2CS[0], kPath, "PointDenoiseV2CS");
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
	ClearCoverage();  // (47w)
	aoThisFrame = false;  // (46f) SSGI's own AO from the next frame on
	aoHistoryValid = false;
	aoDetailHistoryValid = false;  // (46t)
	aoV2Valid = false;  // (47l)
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
	ReleasePointDenoiser();  // (46b)
	ReleaseAOFilter();       // (46f)
	ReleaseForwardShadowMap();             // (47z)
	RayTracingCasterFilter::ReleaseKept();
	pointPrevValid = false;
	prevCameraValid = false;
	maskDesc = {};
	maskDescValid = false;  // (47n)
	maskTempBytes = 0;
	d3d11Bytes = 0;
}

void RayTracing::ReleaseSunOnlyResources()
{
	// (47n) Sun "Off" outdoors for a while: the sun's work texture and its denoisers go (made again when the sun is back). The point
	// lights' and RTAO's textures, the scene and the shared depth / visibility textures stay.
	const std::uint64_t before = d3d11Bytes;
	ReleaseDenoiser();
	ReleaseSigma();
	if (maskTemp) {
		maskTemp = nullptr;
		maskTempUAV = nullptr;
		d3d11Bytes -= std::min(d3d11Bytes, maskTempBytes);
		maskTempBytes = 0;
	}
	history4Valid = false;
	sigmaHistoryValid = false;
	sunOffReleased = true;
	logger::info("[RayTracing] Sun shadows Off: the sun's work texture and denoisers released ({})", Megabytes(before - std::min(before, d3d11Bytes)));
}

void RayTracing::ReleaseDenoiser()
{
	for (auto* t : { &denoiseTiles, &denoiseH, &denoiseOut, &denoiseHalfIn, &history4[0], &history4[1] })
		*t = {};
	history4Valid = false;
	d3d11Bytes -= std::min(d3d11Bytes, denoiseBytes);
	denoiseBytes = 0;
	if (sssCasterTiles.texture) {  // (47r)
		d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(sssCasterTilesWidth) * sssCasterTilesHeight);
		sssCasterTiles = {};
		sssCasterTilesWidth = sssCasterTilesHeight = 0;
	}
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

bool RayTracing::EnsureD3D11Resources(const D3D11_TEXTURE2D_DESC& a_maskDesc, bool a_maskTemp)
{
	// (47n) The size is tracked on its own (maskDescValid): MaskTemp is the sun's (and the interiors' mask channels'); with the sun
	// "Off" outdoors it is not made (and released after a calm period, ReleaseSunOnlyResources).
	const bool same = maskDescValid && a_maskDesc.Width == maskDesc.Width && a_maskDesc.Height == maskDesc.Height && a_maskDesc.Format == maskDesc.Format;
	if (same && (maskTemp || !a_maskTemp)) {
		EnsureDebugColor();  // (46q)
		return true;
	}
	if (!same)
		ReleaseD3D11Resources();

	DXGI_FORMAT uavFormat = a_maskDesc.Format;
	if (uavFormat == DXGI_FORMAT_R8G8B8A8_TYPELESS)
		uavFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
	if (uavFormat != DXGI_FORMAT_R8G8B8A8_UNORM && uavFormat != DXGI_FORMAT_B8G8R8A8_UNORM && uavFormat != DXGI_FORMAT_R16G16B16A16_FLOAT &&
		uavFormat != DXGI_FORMAT_R10G10B10A2_UNORM) {
		Disable(Status::Unavailable, std::format("unexpected shadow mask format {}", static_cast<int>(a_maskDesc.Format)));
		return false;
	}
	maskDesc = a_maskDesc;
	maskDescValid = true;
	if (!a_maskTemp) {
		EnsureDebugColor();
		return true;
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

	maskTempBytes = static_cast<std::uint64_t>(desc.Width) * desc.Height * 4;
	d3d11Bytes += maskTempBytes;
	sunOffReleased = false;
	// (46q) The debug texture: only while a debug view is on (Size Buffers to Render Resolution, always since 47m); a failure
	// to make it is not fatal (the debug view then shows nothing).
	EnsureDebugColor();
	return true;
}

bool RayTracing::EnsureDebugColor()
{
	const bool wanted = debugView != DebugView::Off || aoDebugView != AODebugView::Off || reflectDebugView != ReflectDebugView::Off;
	if (!wanted) {
		if (debugColor) {
			d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(maskDesc.Width) * maskDesc.Height * 4);
			debugColor = nullptr;
			debugColorUAV = nullptr;
			debugColorSRV = nullptr;
		}
		return true;
	}
	if (debugColor || !maskDescValid)
		return debugColor != nullptr;
	// (47n) From the tracked size (MaskTemp may be absent with the sun "Off").
	D3D11_TEXTURE2D_DESC debugDesc = maskDesc;
	debugDesc.MipLevels = 1;
	debugDesc.ArraySize = 1;
	debugDesc.SampleDesc = { 1, 0 };
	debugDesc.Usage = D3D11_USAGE_DEFAULT;
	debugDesc.CPUAccessFlags = 0;
	debugDesc.MiscFlags = 0;
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

// (49b) Hitch fix E: the backend's per-frame switch (before the early prep records any build). (49c) B removed.
void RayTracing::UpdateHitchState()
{
	if (!backend.IsReady())
		return;
	backend.SetAlphaPrealloc(ReserveOn());
}

void RayTracing::LatchFrame()
{
	UpdateHitchState();  // (49b)
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
	restoredThisFrame = 0;  // (47w)
	UpdateCoverage();       // (47w) the newest read-back's coverage counts decide which maps may be skipped this frame
	skipLatched =RtWanted() && PointLightsOn() && InteriorsOn() &&
	              settings.PointLightsSkipShadowMaps && hooksInstalled && status == Status::Active && phase == Phase::Play && !loadingOpen.load() &&
	              interiorTracedLastFrame && Util::IsInterior() && backend.HasFreeContext() && scene.HasCoverage() && !skipCandidates.empty();
	auto* sky = globals::game::sky;
	// (45k) Not during a loading screen or the warm-up after it (nor while the RT result still fades in).
	const bool active = RtWanted() && SunOn() && status == Status::Active && phase == Phase::Play && fade >= 1.0f &&
	                    debugView != DebugView::Disagreement && !Util::IsInterior() && sky && sky->mode.get() == RE::Sky::Mode::kFull &&
	                    backend.HasFreeContext() && scene.HasCoverage();
	// (45f) Watch for big casters the scene lacks while Ray Tracing runs (also when nothing is left out).
	const bool watch = RtWanted() && SunOn() && status == Status::Active;
	RayTracingCasterFilter::Latch(active, filterRect, watch, settings.KeepBuildingsInSunMap);  // (47z) keep: Keep Buildings in the Game's Sun Shadow Map
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
	// (46p) Per-frame backend switches: Keep Our GPU Memory Resident (Upscaling > Save GPU Memory).
	if (backend.IsReady()) {
		backend.SetResidencyHigh(true);  // fixed (47m): Keep Our GPU Memory Resident
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
	// (47j) Early RT Prep Indoors: in an interior traced by Ray Tracing (its own interior frame), once the interior is in the scene
	// with its captured box (the characters are taken within that box, as RunInteriorFrame does). Always since 47m.
	const bool interior = Util::IsInterior();
	float interiorRect[4]{};
	const bool earlyIndoors = interior && InteriorWanted() && scene.HasCoverage() && scene.GetInteriorRect(interiorRect);
	if (!RtWanted() || status != Status::Active || phase != Phase::Play || loadingOpen.load() || !backend.IsReady() ||
		(interior ? !earlyIndoors : (!sky || sky->mode.get() != RE::Sky::Mode::kFull)) || !backend.HasFreeContext())
		return;
	const std::int64_t start = Qpc();
	try {
		skinDraws.clear();
		skinBones.clear();
		if (settings.Characters && backend.SkinAvailable()) {
			float world[4];
			characters.SetRelevance(interior ? nullptr : CurrentRelevance());  // (47u)
			if (interior)
				characters.Update(backend, interiorRect, blackScreen ? 6.0 : 1.0, blackScreen ? 32u : 6u);
			else if (scene.GetLoadedRect(world, false))
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
		FillWind(input);  // (47l)
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
	sunMaskWrittenThisFrame = false;  // (47h)
	pending.active = false;
	frameStart = Qpc();
	UpdateHitchState();  // (49b)
	// (46n) A step of the cell-event preparation faulted behind the loading screen: off here, on the world's frame.
	if (!cellEventFault.empty() && status != Status::Failed && status != Status::Unavailable)
		Disable(Status::Failed, std::exchange(cellEventFault, {}));
	// (46b) Point lights: nothing published until this frame's trace is submitted; the history only follows
	// a frame that was denoised.
	pointSelector.Publish(false);
	pointThisFrame = false;
	pointK = 0;
	pointWideThisFrame = false;  // (48a)
	pointExtraThisFrame = false;  // (48c)
	waterTraced = false;  // (47i)
	waterLodSkip = false;  // (47l) set again by this frame's point-light preparation
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
	restoredLastFrame = restoredThisFrame;  // (47w)
	// (46f) RTAO: nothing for the AO consumers until this frame's trace carries it.
	aoThisFrame = false;
	aoInteriorThisFrame = false;  // (46h)
	aoModeThisFrame = -1;
	aoComposedLastFrame = aoComposited;  // the F7 row is written before this frame's AO pass (ReportFrame)
	aoComposited = false;
	aoDebugDrawn = false;
	reflectDebugDrawn = false;  // (48a)
	reflectThisFrame = false;   // (48d)
	reflectGapFillUsedLastFrame = reflectGapFillUsed;
	reflectGapFillUsed = false;
	shadeLastFrame = shadeThisFrame;  // (48g)
	shadeThisFrame = false;
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

	if (!RtWanted()) {  // (47n) also with sun, point lights and RTAO all "Off"
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
			backend.WaitForReflections();  // (48d) and the third
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
			// (46p) Save GPU Memory (always, 47m): the shadow-mask work textures, the denoisers, the shared textures, scratch and
			// the TLAS go too (made again on the next switch-on; D3D12 ones behind the fence).
			ReleaseD3D11Resources();
			if (backend.IsReady())
				backend.ReleaseIdleResources();
			// (46i) Switching on again starts like a fresh entry: the interior's entry time, the stuck-state diagnostic.
			interiorCell = nullptr;
			interiorReadyPending = false;
			interiorSceneLogPending = false;
			waitingSinceQpc = 0;
			waitingLogged = false;
			status = Status::Off;
			statusReason.clear();
			lastTlasLog = 0;
			// (47p) No stale averages on the page, the overlay or in F12 while off (they were the last ones before the switch-off).
			cpuLast = {};
			cpuSmoothed = {};
			cpuWorst = {};
			lastCpuMs = 0.0f;
			if (backend.IsReady())
				backend.ResetGpuReadings();
			logger::info("[RayTracing] Switched off");
		}
		scene.Housekeeping();  // (45j) engine objects of the released scene, a bounded number per frame
		// Switching back on is a fresh attempt (also after a failure), never a silent "Starting".
		backendTried = false;
		if (backend.IsReady()) {
			backend.Tick();
			if (backend.AlphaIdle())
				backend.ReleaseAlphaResources();  // (45h) once the GPU is done with the released scene
			backend.SetMaterialsEnabled(false);  // (48f) the released scene's materials and attributes go
			ReleaseShadeResources();  // (48g)
			backend.MaterialHousekeeping(globals::state->frameCount);
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
	PrewarmReflectionShaders();  // (48e) also at the main menu and behind loading screens
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
		// (49c) This is the loading-screen thread: while "Prepare When Arriving" holds the main thread, it makes the D3D11 copies.
		if (arrivalActive.load())
			arrivalPresents.fetch_add(1, std::memory_order_relaxed);
		LoadingThreadWork();
		return;
	}
	if (!RtWanted()) {
		if (status != Status::Off)
			SwitchedOffFrame();  // (46k) switched off behind the loading screen: the same clean reset as on a world frame
		return;
	}
	if (status == Status::Unavailable || status == Status::Failed)
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
	sunMaskWrittenThisFrame = false;  // (47h)
	pending.active = false;
	// (46p) Per-frame backend switches: Keep Our GPU Memory Resident (Upscaling > Save GPU Memory).
	if (backend.IsReady()) {
		backend.SetResidencyHigh(true);  // fixed (47m): Keep Our GPU Memory Resident
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
		inLoadingPump = true;  // (48f) no D3D11 work from here
		TransitionFrame();
		inLoadingPump = false;
		const std::int64_t endStart = Qpc();
		scene.EndFrame(backend);
		characters.EndFrame(backend);
		cpuLast.endFrame = static_cast<float>(static_cast<double>(Qpc() - endStart) * QpcMs());
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	inLoadingPump = false;  // (48f) also after an exception
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
		if (RtWanted() && status == Status::Active && stats.blasCount == 0 && sc.triangles > 0) {
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
	if (!RtWanted() || status != Status::Waiting || inTransition || phase != Phase::Play) {
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
	if (RtWanted() && status != Status::Unavailable && status != Status::Failed) {
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
		detail.plWaterRays = pointThisFrame ? stats.pointWaterRaysPerFrame : 0;  // (47j)
		// (47l) Foliage sway (-1 = off): wind BLASes alive, their vertices, refits recorded for this frame's trace.
		if (scene.SwayActive()) {
			const auto& ws = backend.GetWindStats();
			detail.windMeshes = ws.meshes;
			detail.windVertices = static_cast<std::int64_t>(ws.vertices);
			detail.windRefits = activeThisFrame || earlyThisFrame ? ws.refitsLastFrame : 0;
			const auto& sw = scene.GetWindStats();  // (47n)
			detail.windInRadius = sw.inRadius;
			detail.windSources = ws.sources;
			detail.windBuilds = activeThisFrame || earlyThisFrame ? ws.buildsLastFrame : 0;
			detail.windRefitVertices = activeThisFrame || earlyThisFrame ? static_cast<std::int64_t>(ws.refitVerticesLastFrame) : 0;
			detail.windRadius = sw.radius;
			detail.windCatchups = static_cast<int>(sw.catchups);  // (47q)
			detail.windFrozenUnlearned = static_cast<int>(sw.frozenUnlearned);  // (47r)
			detail.windSharedCatchups = static_cast<int>(sw.catchupsShared);
		}
		// (47q) The NPC rule and tiny face parts (characters traced), static chunks (a statistic, refreshed every 120 frames).
		if (settings.Characters && backend.SkinAvailable()) {
			const auto& ch = characters.GetStats();
			detail.npcBodyOnly = static_cast<int>(ch.bodyOnlyActors);
			detail.npcRefitSkipped = static_cast<int>(ch.refitsSkipped);
			detail.skipTinyFace = PerfOptions::settings.RtDropTinyParts ? static_cast<std::int64_t>(ch.skipTinyFace) : -1;
		}
		if (globals::state->frameCount - chunkStatsFrame >= 120u || chunkStatsFrame == UINT32_MAX) {
			chunkStatsFrame = globals::state->frameCount;
			scene.ChunkOverlapStats(backend, chunkStatsCells, chunkStatsPerCell, chunkStatsOverlap);
		}
		if (chunkStatsCells > 0) {
			detail.chunkCells = static_cast<int>(chunkStatsCells);
			detail.chunksPerCell = chunkStatsPerCell;
			detail.chunkOverlap = chunkStatsOverlap;
		}
		// (47u) Start Sun Trace Earlier, Skip Updates That Cannot Affect the Image, Spatial Static Chunks.
		detail.earlyNormal = earlyNormalThisFrame;
		{
			const auto& ws = backend.GetWindStats();
			detail.relWindCulled = scene.SwayActive() ? static_cast<int>(ws.culledLastFrame) : 0;
			detail.relWindLate = scene.SwayActive() ? static_cast<int>(ws.lateLastFrame) : 0;
			detail.relNpcCulled = static_cast<int>(relNpcCulledThisFrame);
			detail.relFallback = relFallbackThisFrame ? 1 : 0;
		}
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
		// (48a) RT reflections step 1 (-1 = not traced this frame). (48d) Their own list: no longer tied to RTAO.
		if (activeThisFrame && stats.reflectTracedLastFrame) {
			detail.reflGpuMs = stats.lastReflectGpuMs;
			const double pixels = static_cast<double>(reflectInput.constants.renderSize[0]) * reflectInput.constants.renderSize[1];
			detail.reflTracedPct = stats.reflectPixelsPerFrame >= 0 && pixels > 0.0 ? static_cast<float>(100.0 * static_cast<double>(stats.reflectPixelsPerFrame) / pixels) : -1.0f;
		}
		// (48d) Gap Fill (the row is written before this frame's SSRT pass: last frame's): used by the ray march (1 / 0; -1 = mode not Gap
		// Fill) and the game queue's wait for the reflection list right before it.
		detail.reflFill = settings.ReflectionsMode == 2 ? (reflectGapFillUsedLastFrame ? 1 : 0) : -1;
		detail.reflSyncGpuMs = reflectGapFillUsedLastFrame ? std::max(timers->LastSampleMs(Util::GpuBucket::RayTracingReflectSync), 0.0f) : -1.0f;
		// (48f) Capture Materials (-1 = off).
		if (backend.MaterialsEnabled()) {
			const auto& ms = backend.GetMaterialStats();
			detail.matCopies = static_cast<int>(ms.copiesLastFrame);
			detail.matPending = static_cast<int>(ms.texturesPending);
			detail.matTextures = static_cast<int>(ms.texturesReady);
			detail.matAttrMb = static_cast<float>(static_cast<double>(ms.attrBytesUsed) / (1024.0 * 1024.0));
		}
		// (48g) The shading half (last frame's; -1 = not run).
		if (shadeLastFrame) {
			detail.shadeGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingReflectShade);
			detail.shadeMaterialPct = shadeMaterialShare >= 0.0f ? shadeMaterialShare * 100.0f : -1.0f;
			detail.shadeRatio = shadeRatio;                  // (48h) this read-back's geometric mean
			detail.shadeRatioSmoothed = shadeRatioSmoothed;  // (48h) the ~2 s average the panel shows
		}
		// (46h) Where this frame's RTAO was traced and its mode (-1 = not traced this frame).
		detail.rtaoIndoors = aoTraced && aoThisFrame ? (aoInteriorThisFrame ? 1 : 0) : -1;
		detail.rtaoMode = aoTraced && aoThisFrame ? aoModeThisFrame : -1;
		// (46t) The last composite's filter (the F7 row is written before this frame's AO pass, as rtao_filter_gpu_ms).
		detail.rtaoFilter = aoComposedLastFrame ? aoLastFilter : -1;
		detail.rtaoDetailRays = aoComposedLastFrame ? aoLastDetailRays : -1;
		detail.rtaoDetailLength = aoComposedLastFrame ? aoLastDetailLength : -1.0f;
		detail.rtaoNormalGuided = aoComposedLastFrame ? aoLastNormalGuided : -1;
		detail.rtaoAlphaBudget = aoComposedLastFrame ? aoLastAlphaBudget : -1;
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
		// (48c) Extra Lights Where Crowded: pixels with an extra rank traced / pixels with a traced light (newest read-back).
		detail.plExtraPct = pointThisFrame && pointExtraThisFrame && stats.pointTracedPixelsPerFrame > 0 && stats.pointExtraPixelsPerFrame >= 0 ?
		                        static_cast<float>(100.0 * static_cast<double>(stats.pointExtraPixelsPerFrame) / static_cast<double>(stats.pointTracedPixelsPerFrame)) :
		                        -1.0f;
		detail.plBeamsExcluded = 0;  // fixed (47m): the Window Beam Guard is gone
		if (pointThisFrame) {  // (46x)
			detail.plFillDirect = static_cast<int>(ps.fillDirectAimed + ps.fillDirectPoint);
			detail.plFillAmbient = static_cast<int>(ps.fillAmbient);
			detail.plPortalAway = static_cast<int>(ps.portalAway);
			detail.plPortalDropped = static_cast<int>(ps.portalDropped);
			detail.plPortalBoxed = static_cast<int>(ps.portalBoxed);
		}
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
	// (49b) Hitch fixes in effect, the build queue, and the RT fog passes of this frame (F7 rt_hitch_mode, rt_builds_waiting, rt_fog_*).
	if (backend.IsReady()) {
		const auto& fs = backend.GetStats();
		// (49c) Bits: 4 / 8 / 16 Reserve Shaders and Memory Up Front (49b D / E / F), 32 Prepare When Arriving, 64 the loading-screen thread's copies.
		detail.hitchMode = (ReserveOn() ? (4 | 8 | 16) : 0) | (LoadOn(settings.LoadPrepareArrival) ? 32 : 0) | (LoadOn(settings.LoadThreadCopies) ? 64 : 0);
		detail.buildsWaiting = static_cast<int>(backend.QueuedBuilds());
		detail.fogSun = fs.fogTracedLastFrame && fs.fogSunLastFrame ? 1 : 0;
		detail.fogLights = fs.fogTracedLastFrame && fs.fogLightsLastFrame ? 1 : 0;
	}
	// (49c) Loading Screens columns (once each).
	detail.arrivalMs = arrivalTraceMs;
	arrivalTraceMs = -1.0f;
	if (const std::uint32_t copies = loadThreadCopiesTrace.exchange(0); copies > 0)
		detail.loadThreadCopies = static_cast<int>(copies);
	detail.handOffPending = handOffPendingTrace;
	handOffPendingTrace = -1;
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
			q.budOn = 1;  // fixed (47m): builds always budgeted by GPU time
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
	backend.WaitForReflections();  // (48d) the reflection list reads the same depth copy and the reflection prep
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
	const bool sunOn = SunOn();  // (47n) Sun "Off": no MaskTemp, no sun trace / denoiser / combine (RunNoSunFrame)
	if (!EnsureD3D11Resources(desc, sunOn))
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
	const bool playCalm = phase == Phase::Play && !loadingOpen.load() && denoiserUi && !denoiserUi->GameIsPaused() && !denoiserUi->IsMenuOpen(RE::MapMenu::MENU_NAME);
	const bool denoiserCalm = denoiserStableFrames >= 120 && playCalm;
	if (sunOn && denoiserCalm) {  // (47m) always (was Save GPU Memory > Free Ray Tracing Memory When Off)
		if (denoiser != DenoiserMode::Penumbra && (denoiseTiles.texture || history4[0].texture))
			ReleaseDenoiser();
		if (denoiser != DenoiserMode::Sigma && (sigma || sigmaOut.texture))
			ReleaseSigma();
	}
	// (47n) Sun "Off": its work texture and denoisers go once it has stayed off for 120 frames of play.
	sunOffFrames = sunOn ? 0u : std::min(sunOffFrames + (playCalm ? 1u : 0u), 1000000u);
	if (!sunOn && sunOffFrames >= 120 && (maskTemp || denoiseTiles.texture || history4[0].texture || sigma || sigmaOut.texture))
		ReleaseSunOnlyResources();
	const bool penumbra = denoiser == DenoiserMode::Penumbra;
	const bool rrMode = denoiser == DenoiserMode::DLSSRR;
	bool sigmaMode = sunOn && denoiser == DenoiserMode::Sigma;
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
			characters.SetRelevance(CurrentRelevance());  // (47u)
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
	if (!sunOn) {
		// (47n) Sun "Off": point lights and RTAO only (as indoors), the cascades and Screen Space Shadows untouched.
		RunNoSunFrame(depth.depthSRV, mask.SRV, traceRect, camX, camY, camZ, width, height, validate);
		return;
	}

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
	FillWind(input);  // (47l)
	input.skinBones = &skinBones;
	input.deferWait = true;  // (45l) FinishFrame waits (after the other features' prepasses)
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
	// (47q) The point pass reuses the sun's surface probe (both mark the traced-caster surfaces; same depth, rect and probe ray): the sun
	// writes the probe as the point pass makes it where it probed, the point pass (its cut-out variant) takes it there, traces it elsewhere.
	// (47r) "Point Lights: Reuse the Sun's Surface Probe Everywhere" (PointProbeShareAll): the non-alpha variant too (Low / Medium
	// outdoors): the sun marks only the pixels whose probe hit nothing or a forced-opaque instance (kFlagProbeShareOpaque). Exact.
	if (input.pointLights && (traceFlags & kFlagRtSurface)) {  // (48o) PointProbeShareAll always on
		std::uint32_t pointFlags;
		std::memcpy(&pointFlags, &pointInput.constants.params[2], sizeof(pointFlags));
		if (pointFlags & kPointFlagRtSurface) {
			pointFlags |= kPointFlagSunProbe;
			traceFlags |= kFlagProbeShare | (pointInput.alpha ? 0u : kFlagProbeShareOpaque);
			std::memcpy(&pointInput.constants.params[2], &pointFlags, sizeof(pointFlags));
			std::memcpy(&input.constants.params[2], &traceFlags, sizeof(float));
		}
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
	PrepareReflections(input);  // (48d) their own list and fence (no longer inside the RTAO batch)
	// (47u) Skip Updates That Cannot Affect the Image: this frame's exact rays (kept for the next frame's early region); what was culled
	// and is reachable after all is updated before the trace.
	{
		BuildExactRelevance(input, traceRect, camX, camY, camZ);
		float charWorld[4];
		const bool haveWorld = scene.GetLoadedRect(charWorld, false);
		ResolveRelevance(relevanceExact.Valid() ? &relevanceExact : nullptr, haveWorld ? charWorld : nullptr);
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
		                (quality.denoiseHalf ? kFlagDenoiseHalf : 0u) | (falloff ? kFlagFalloffDenoise : 0u) |
		                kCombineFlagMotionHistory | kCombineFlagNoHistorySmooth | kCombineFlagSmoothTileRadius;  // (47y) always on (48o)
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
	RunMaterialCopies(false);  // (48f) Capture Materials: texture copies (D3D11, per-frame budget)

	// 0b. (47j) RTAO: Smooth Normals, from last frame's depth still in DepthMask (before step 1 overwrites it); (47u) or, with Start Sun
	// Trace Earlier, right behind the ready signal from its copy. 1. Depth + engine mask -> the tracer's shared texture.
	PrepareDepthMask(context, prepare, depth.depthSRV, mask.SRV, width, height, input);

	// 2. D3D12: builds, skinning and the TLAS (not waiting for D3D11), then the trace (waiting for the
	// depth copy above, GPU-side). The D3D11 queue waits for the trace in FinishFrame.
	const std::int64_t executeStart = Qpc();
	cpuLast.d3d11 = static_cast<float>(static_cast<double>(executeStart - d3d11Start) * QpcMs());
	PrepareFogInput(input, input.sun);  // (49b)
	const bool traced = backend.ExecuteFrame(input);
	NoteFogTraced(input, traced);  // (49b)
	FlushDeferredAONormal();  // (47u) the frame never reached its ready signal: the deferred normal prep now (nothing else reads it)
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
	reflectThisFrame = input.reflect != nullptr && backend.GetStats().reflectSubmittedLastFrame;  // (48d)
	aoModeThisFrame = aoThisFrame ? std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1) : -1;  // (46h)
	if (!aoThisFrame)
		aoHistoryValid = false;
	// (46b) The point lights went out with it: Light Limit Fix (next in the prepass list) may tag its lights.
	pointThisFrame = input.pointLights != nullptr && backend.GetStats().pointTracedLastFrame;
	pointSelector.Publish(pointThisFrame);
	if (!pointThisFrame) {
		pointK = 0;
		pointWideThisFrame = false;  // (48a)
		pointExtraThisFrame = false;  // (48c)
	}

	// (45l) The D3D11 side of this frame: FinishFrame (now, or after the other prepasses with Late Sync).
	pending.active = true;
	pending.interior = false;
	pending.noSun = false;  // (47n)
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
						 "alpha off {}, alpha failed {}, shader {}, type {}, no CPU data {}, bad data {}, placement {}, budget/size {}, tiny face parts {}; bone "
						 "indices palette {}, global {}; (47q) NPCs body only {} (all parts within {}), of them not refit this frame {}",
				ch.actors, ch.inRange, ch.registered, ch.shapes, ch.skinnedShapes, ch.rigidShapes, ch.physicsShapes, ch.alphaShapes, sk.drawsLastFrame,
				static_cast<double>(sk.verticesLastFrame) / 1e6, sk.refitsLastFrame, sk.buildsLastFrame, sk.reusedLastFrame,
				static_cast<double>(sk.bytes + sk.scratchBytes + sk.boneBytes) / (1024.0 * 1024.0), ch.skipPhysicsOff, ch.skipAlphaOff, ch.skipAlphaFailed,
				ch.skipShader, ch.skipGeometryType, ch.skipNoCpuData, ch.skipBadData, ch.skipPlacement, ch.skipBackend, ch.skipTinyFace, ch.paletteMode,
				ch.globalMode, ch.bodyOnlyActors, NpcFullDistance() > 0.0f ? std::format("{:.0f} units", NpcFullDistance()) : std::string("no limit"),
				ch.refitsSkipped);
		}
		// (47q) Static chunks (a statistic: no re-chunking).
		std::uint32_t cells = 0;
		float perCell = 0.0f, overlap = 0.0f;
		scene.ChunkOverlapStats(backend, cells, perCell, overlap);
		logger::info("[RayTracing] Static chunks (47q statistic): {} exterior cells, {:.2f} chunk BLASes per cell, pairwise box overlap {:.3f} "
					 "(intersection / summed volume; 0 = disjoint)",
			cells, perCell, overlap);
	}
}

void RayTracing::RunNoSunFrame(ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_mask, const float a_traceRect[4], double a_camX, double a_camY,
	double a_camZ, std::uint32_t a_width, std::uint32_t a_height, bool a_validate)
{
	// (47n) Outdoors with Sun Shadows "Off": the point lights (no sun gate: sunStrength 0) and RTAO over the same scene and TLAS, as an
	// interior frame does it; nothing of the sun is traced, denoised or combined, the engine's mask and Screen Space Shadows stay as the
	// game made them (LatchFrame / ClaimScreenSpaceShadows leave the cascades and SSS alone while the sun is Off).
	const auto& fb = globals::game::frameBufferCached;
	const auto& invViewProj = fb.GetCameraViewProjInverse(0);
	RayTracingRT::FrameInput input;
	std::memcpy(input.constants.invViewProj, &invViewProj, sizeof(input.constants.invViewProj));
	input.constants.renderSize[0] = static_cast<float>(a_width);
	input.constants.renderSize[1] = static_cast<float>(a_height);
	input.constants.renderSize[2] = 1.0f / static_cast<float>(a_width);
	input.constants.renderSize[3] = 1.0f / static_cast<float>(a_height);
	std::copy_n(a_traceRect, 4, input.constants.rect);
	if (!skinDraws.empty()) {
		const double cam[3] = { a_camX, a_camY, a_camZ };
		characters.GetBounds(cam, &input.constants.spheres[0], &input.constants.spheres[4]);
	}
	input.renderWidth = a_width;
	input.renderHeight = a_height;
	input.cameraX = a_camX;
	input.cameraY = a_camY;
	input.cameraZ = a_camZ;
	input.instances = &instances;
	input.buildTriangleBudget = kBuildBudget;
	input.validate = a_validate;
	input.skinDraws = &skinDraws;
	input.skinBones = &skinBones;
	input.deferWait = true;
	input.sun = false;
	input.buildCpuMs = blackScreen ? kBlackBuildCpuMs : 0.0f;
	input.skinUploadCpuMs = blackScreen ? kBlackSkinUploadCpuMs : 0.0f;
	FillWind(input);
	{
		const std::int64_t pointStart = Qpc();
		try {
			if (!PreparePointLights(input, a_traceRect, a_camX, a_camY, a_camZ, 0.0f, false))
				input.pointLights = nullptr;
		} catch (const std::exception& e) {
			input.pointLights = nullptr;
			SwitchOffPointLights(std::format("exception in the light selection: {}", e.what()));
		}
		pointCpuMs = static_cast<float>(static_cast<double>(Qpc() - pointStart) * QpcMs());
	}
	{
		const std::int64_t aoStart = Qpc();
		try {
			if (!PrepareAO(input, a_traceRect))
				input.ao = nullptr;
		} catch (const std::exception& e) {
			input.ao = nullptr;
			SwitchOffAO(std::format("exception: {}", e.what()));
		}
		aoCpuMs = static_cast<float>(static_cast<double>(Qpc() - aoStart) * QpcMs());
	}
	PrepareReflections(input);  // (48d) their own list and fence (no longer inside the RTAO batch)
	if (!input.pointLights && !input.ao && !reflectWantedThisFrame) {  // (48d) or reflection rays
		// Nothing to trace (no shadow-casting light in view, RTAO off): the queued builds go on, nothing waits.
		if (backend.HasPrepared()) {
			backend.AbandonPrepared();
			earlyThisFrame = false;
		}
		backend.BuildOnly(kBuildBudget, 64u, blackScreen ? kBlackBuildCpuMs : 2.0f);
		backend.Tick();
		status = Status::Active;
		statusReason = "sun Off, no shadow-casting light in view";
		history4Valid = false;
		sigmaHistoryValid = false;
		++frameIndex;
		return;
	}

	// The constants PrepareCS and the point-light denoiser read (Common.hlsli).
	CombineCB cb{};
	std::memcpy(cb.InvViewProj, &invViewProj, sizeof(cb.InvViewProj));
	std::copy_n(a_traceRect, 4, cb.Rect);
	if (prevCameraValid) {
		std::copy_n(prevViewProj, 16, cb.PrevViewProj);
		cb.CameraDelta[0] = static_cast<float>(a_camX - prevCamera[0]);
		cb.CameraDelta[1] = static_cast<float>(a_camY - prevCamera[1]);
		cb.CameraDelta[2] = static_cast<float>(a_camZ - prevCamera[2]);
	} else {
		std::memcpy(cb.PrevViewProj, &fb.GetCameraViewProj(0), sizeof(cb.PrevViewProj));
	}
	const bool debug = PointDenoiserDebugView();  // (48m) the point views, + Moving Pixels / Filter Path
	cb.Size[0] = a_width;
	cb.Size[1] = a_height;
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
		}
		if (alphaMaskCS)
			backend.RecordAlphaCopies(alphaMaskCS.get(), kAlphaCopiesPerFrame);
	}
	RunMaterialCopies(false);  // (48f) Capture Materials: texture copies (D3D11, per-frame budget)
	ResolveRelevance(nullptr);  // (47u) no exact region on a sun "Off" frame: everything left stale by Skip Updates is updated now
	PrepareDepthMask(context, prepare, a_depth, a_mask, a_width, a_height, input);  // (47u) (47j) normal prep + PrepareCS
	const std::int64_t executeStart = Qpc();
	cpuLast.d3d11 = static_cast<float>(static_cast<double>(executeStart - d3d11Start) * QpcMs());
	PrepareFogInput(input, input.sun);  // (49b)
	const bool traced = backend.ExecuteFrame(input);
	NoteFogTraced(input, traced);  // (49b)
	FlushDeferredAONormal();  // (47u)
	cpuLast.execute = static_cast<float>(static_cast<double>(Qpc() - executeStart) * QpcMs());
	if (!traced && backend.HasFailed()) {
		Disable(Status::Failed, backend.LastError());
		return;
	}
	history4Valid = false;  // nothing of the sun's history survives an "Off" frame
	sigmaHistoryValid = false;
	if (!traced) {
		pointHistoryValid = false;
		++frameIndex;
		return;
	}
	pointThisFrame = input.pointLights != nullptr && backend.GetStats().pointTracedLastFrame;
	pointSelector.Publish(pointThisFrame);
	if (!pointThisFrame) {
		pointK = 0;
		pointWideThisFrame = false;  // (48a)
		pointExtraThisFrame = false;  // (48c)
	}
	aoThisFrame = input.ao != nullptr && backend.GetStats().aoTracedLastFrame;
	reflectThisFrame = input.reflect != nullptr && backend.GetStats().reflectSubmittedLastFrame;  // (48d)
	aoModeThisFrame = aoThisFrame ? std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1) : -1;
	if (!aoThisFrame)
		aoHistoryValid = false;

	pending.active = true;
	pending.interior = false;
	pending.noSun = true;
	pending.traceSync = pointThisFrame;  // RTAO only: DrawAmbientOcclusion's wait covers the trace (as indoors)
	pending.splitSun = false;
	std::memcpy(pending.cbBytes.data(), &cb, sizeof(cb));
	pending.combineFlags = 0;
	pending.width = a_width;
	pending.height = a_height;
	pending.writeSSS = pending.penumbra = pending.spatial = pending.half = pending.sigma = pending.sssNonRT = false;
	pending.debug = debug;
	pending.camera[0] = a_camX;
	pending.camera[1] = a_camY;
	pending.camera[2] = a_camZ;
	std::memcpy(pending.viewProj, &fb.GetCameraViewProj(0), sizeof(pending.viewProj));
	std::memcpy(pending.viewProjUnjittered, &fb.GetCameraViewProjUnjittered(0), sizeof(pending.viewProjUnjittered));

	activeThisFrame = true;
	status = Status::Active;
	statusReason = pointThisFrame ? (aoThisFrame ? "sun Off: point lights + RTAO" : "sun Off: point lights") : (aoThisFrame ? "sun Off: RTAO" : "sun Off");
	++frameIndex;
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
	// (47n) A sun "Off" frame needs no MaskTemp (nothing is combined into the mask).
	if (backend.HasFailed() || !mask.texture || !mask.SRV || (!maskTemp && !pending.noSun)) {
		if (backend.HasFailed())
			Disable(Status::Failed, backend.LastError());
		history4Valid = false;
		sigmaHistoryValid = false;
		return;
	}
	if (pending.interior || pending.noSun) {
		// (46c) Indoors: the point lights only (denoiser, pixel shader binding), then the engine shadow lights' mask channels.
		// (47n) Outdoors with the sun "Off": the point lights only (their mask channels are not written outdoors, as with the sun).
		if (pointThisFrame) {
			try {
				FinishPointLights();
			} catch (const std::exception& e) {
				SwitchOffPointLights(std::format("exception in the denoiser: {}", e.what()));
			}
		}
		if (pending.interior)
			WritePointMask(pointThisFrame && pointDenoised);
		interiorTracedLastFrame = pending.interior && pointThisFrame && pointDenoised;
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
		// (47r) Tile Mask for RT Casters: the per-tile summary first (timed with Screen Space Shadows: its cost is theirs).
		ID3D11ShaderResourceView* casterTiles = MakeSSSCasterTiles();  // (48o) always on
		screenSpaceShadows->DrawDeferred(backend.VisibilitySRV(), settings.RtContactScale, pending.biasConstant, pending.biasPerUnit, casterTiles);
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
		// (47r) Sun: Fast Path for Uniform Tiles: the denoiser's tiles at t6 (Penumbra, this frame's tile pass ran).
		const bool uniformTiles = penumbraActive && (combineFlags & kFlagSpatial) && !sigmaActive && denoiseTiles.srv;
		if (uniformTiles) {
			combineFlags |= kCombineFlagUniformTiles;
			setFlags();
		}
		// (47y) Moving Characters: Keep Shadow History: last frame's motion vectors at t7 (this frame's G-buffer is not drawn yet).
		ID3D11ShaderResourceView* motionSRV = (penumbraActive && (combineFlags & kCombineFlagMotionHistory)) ?
		                                          renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR].SRV :
		                                          nullptr;
		if ((combineFlags & kCombineFlagMotionHistory) && !motionSRV) {
			combineFlags &= ~kCombineFlagMotionHistory;
			setFlags();
		}
		ID3D11ShaderResourceView* srvs[8] = { mask.SRV, backend.VisibilitySRV(), backend.DepthMaskSRV(), sigmaActive ? sigmaOut.srv.get() : nullptr,
			(combineFlags & kFlagSpatial) ? denoiseOut.srv.get() : nullptr, penumbraActive ? history4[read4].srv.get() : nullptr,
			uniformTiles ? denoiseTiles.srv.get() : nullptr, motionSRV };
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
		context->CSSetShaderResources(0, 8, srvs);
		context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
		context->CSSetShader(combine, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		ID3D11ShaderResourceView* nullSrvs[8]{};
		ID3D11UnorderedAccessView* nullUavs[5]{};
		context->CSSetShaderResources(0, 8, nullSrvs);
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
		sunMaskWrittenThisFrame = true;  // (47h) forward Lighting surfaces may check their depth against it (SunMaskDepthCheckActive)
		sunMaskFrame = globals::state->frameCount;
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
	if (!(RtWanted() && (aoDebugDrawn || reflectDebugDrawn))) {  // (48a) or the reflection rays' view
		if (!RtWanted() || debugView == DebugView::Off || !combinedThisFrame || lastOverlayFrame + 1 != frameIndex)
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
	ID3D11ShaderResourceView* nulls[4]{};
	globals::d3d::context->PSSetShaderResources(40, 4, nulls);  // (47i) + the water's t42 / t43
	globals::d3d::context->PSSetShaderResources(46, 1, nulls);  // (48a) slots 4 - 5
	pointBound = false;
}

// ---------------------------------------------------------------------------------------------
// (47i) Point Light Shadows on Water: the water's distance after the water draws (next frame's trace input)
// ---------------------------------------------------------------------------------------------

void RayTracing::OnEndDeferred()
{
	waterEndDeferredFrame = globals::state ? globals::state->frameCount : 0u;
}

bool RayTracing::WaterTracking() const
{
	return pointWaterOn && PointLightsOn() && backend.PointWaterReady();
}

void RayTracing::OnDrawStatesApplied(ID3D11DeviceContext* a_context)
{
	if (!WaterTracking())
		return;
	const uint32_t frame = globals::state->frameCount;
	const auto* shader = globals::state->currentShader;
	if (shader && shader->shaderType.get() == RE::BSShader::Type::Water) {
		// (47l) Distant LOD water (Water technique 9, ShaderCache::WaterShaderTechniques::Lod) lies beyond the loaded cells, out of
		// every point-light source's reach (sources within PointLightsDistance of the camera, radius at most 4096): its distances
		// are never traced from nor matched. Frames that draw only LOD water skip the prep (and its depth copy).
		constexpr uint32_t kWaterTechniqueLod = 9;
		const bool lodOnly = waterLodSkip && ((globals::state->currentPixelDescriptor >> 11) & 0xF) == kWaterTechniqueLod;
		if (waterEndDeferredFrame == frame && !lodOnly) {
			waterSeenFrame = frame;
			waterPrepPending = true;
			// (47p) The water's bound for the prep gate.
			if (waterGateBoundsFrame != frame) {
				waterGateBoundsFrame = frame;
				waterGateBounds.clear();
			}
			if (const auto* pass = Batch39Engine::CurrentPass(); pass && pass->geometry && waterGateBounds.size() < 256)
				waterGateBounds.push_back(pass->geometry->worldBound);
			else
				waterGateBounds.push_back(RE::NiBound{ {}, -1.0f });  // unknown: always reachable
		}
		return;
	}
	if (waterPrepPending && waterSeenFrame == frame) {
		waterPrepPending = false;
		// (47p) Skipped while no traced point light reaches any water drawn, this frame or the last (the next frame then traces
		// nothing from the water: exactly what it would trace when no light reaches it).
		if (PerfOptions::settings.WaterPrepGate) {
			bool reach = false;
			constexpr double kMargin = 1024.0;
			for (const auto& b : waterGateBounds) {
				if (!(b.radius >= 0.0f)) {
					reach = true;
					break;
				}
				for (const auto& l : waterGateLights) {
					const double dx = l[0] - b.center.x, dy = l[1] - b.center.y, dz = l[2] - b.center.z;
					const double r = l[3] + static_cast<double>(b.radius) + kMargin;
					if (dx * dx + dy * dy + dz * dz <= r * r) {
						reach = true;
						break;
					}
				}
				if (reach)
					break;
			}
			const bool skip = !reach && !waterGateReachLast;
			waterGateReachLast = reach;
			if (skip) {
				++waterGateSkips;
				return;
			}
		} else {
			waterGateReachLast = true;
		}
		PrepWaterDistance(a_context);
	}
}

void RayTracing::PrepWaterDistance(ID3D11DeviceContext* a_context)
{
	auto& main = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	if (!main.texture || !main.depthSRV || !backend.PointWaterDistUAV() || !backend.DepthMaskSRV() || waterRenderWidth == 0 || waterRenderHeight == 0)
		return;
	D3D11_TEXTURE2D_DESC desc{};
	main.texture->GetDesc(&desc);
	if (desc.SampleDesc.Count != 1)
		return;
	if (!waterDepthCopy || desc.Width != waterDepthCopyDesc.Width || desc.Height != waterDepthCopyDesc.Height || desc.Format != waterDepthCopyDesc.Format) {
		waterDepthCopy = nullptr;
		waterDepthCopySRV = nullptr;
		D3D11_TEXTURE2D_DESC copyDesc = desc;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.MiscFlags = 0;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		main.depthSRV->GetDesc(&srvDesc);
		auto* device = globals::d3d::device;
		if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, waterDepthCopy.put())) ||
			FAILED(device->CreateShaderResourceView(waterDepthCopy.get(), &srvDesc, waterDepthCopySRV.put()))) {
			waterDepthCopy = nullptr;
			waterDepthCopySRV = nullptr;
			logger::warn("[RayTracing] Point Light Shadows on Water: could not create the depth copy ({}x{}, format {})", desc.Width, desc.Height, static_cast<int>(desc.Format));
			pointWaterOn = false;
			return;
		}
		waterDepthCopyDesc = desc;
	}
	if (!waterPrepCS) {
		waterPrepCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RayTracing\\WaterPrepCS.hlsl", {}, "cs_5_0")));
		if (!waterPrepCS) {
			logger::warn("[RayTracing] Point Light Shadows on Water: WaterPrepCS did not compile; switched off");
			pointWaterOn = false;
			return;
		}
	}
	if (!waterPrepCB)
		waterPrepCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<WaterPrepConstants>());
	WaterPrepConstants cb{};
	std::memcpy(cb.InvViewProj, waterInvViewProj, sizeof(cb.InvViewProj));
	cb.RenderSize[0] = static_cast<float>(waterRenderWidth);
	cb.RenderSize[1] = static_cast<float>(waterRenderHeight);
	cb.RenderSize[2] = 1.0f / static_cast<float>(waterRenderWidth);
	cb.RenderSize[3] = 1.0f / static_cast<float>(waterRenderHeight);
	waterPrepCB->Update(cb);

	GpuBucketScope waterTimer(Util::GpuBucket::RayTracingWaterPrep);  // (47j) F7 water_prep_gpu_ms
	// The depth as it is after the water (GPU copy in command order), then the water's distance where it lies in front of the
	// opaque depth (the trace's DepthMask).
	a_context->CopyResource(waterDepthCopy.get(), main.texture);
	ID3D11Buffer* cbs[1] = { waterPrepCB->CB() };
	ID3D11ShaderResourceView* srvs[2] = { waterDepthCopySRV.get(), backend.DepthMaskSRV() };
	ID3D11UnorderedAccessView* uavs[1] = { backend.PointWaterDistUAV() };
	a_context->CSSetConstantBuffers(0, 1, cbs);
	a_context->CSSetShaderResources(0, 2, srvs);
	a_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	a_context->CSSetShader(waterPrepCS.get(), nullptr, 0);
	a_context->Dispatch((waterRenderWidth + 7) / 8, (waterRenderHeight + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[2]{};
	ID3D11UnorderedAccessView* nullUavs[1]{};
	ID3D11Buffer* nullCbs[1]{};
	a_context->CSSetShaderResources(0, 2, nullSrvs);
	a_context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	a_context->CSSetConstantBuffers(0, 1, nullCbs);
	a_context->CSSetShader(nullptr, nullptr, 0);
	// Binding the UAV unbound the pixel shaders' view of it: back for any later water draw this frame.
	if (pointBound && waterTraced) {
		ID3D11ShaderResourceView* views[2] = { backend.PointWaterVisSRV(), backend.PointWaterDistSRV() };
		a_context->PSSetShaderResources(42, 2, views);
	}
	waterPrepFrame = globals::state->frameCount;
	waterDistValid = true;
}

// (47j) RTAO: Smooth Normals. The trace runs after the depth prepass, before this frame's G-buffer: last frame's normals
// (NORMALROUGHNESS is cleared only in StartDeferred) reprojected by the camera and the motion vectors, checked against last
// frame's depth (the trace's DepthMask, before PrepareCS overwrites it) and blended toward the depth normal by that confidence.
void RayTracing::DispatchAONormal(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_prevDepth, std::uint32_t a_width,
	std::uint32_t a_height, bool a_writeDebug, bool a_reflect)
{
	auto* renderer = globals::game::renderer;
	auto& normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR];
	// (47l) F7 rtao_normal_prep_gpu_ms: its own GPU bucket. Buckets do not nest and every caller (RunFrame, RunNoSunFrame, RunInteriorFrame,
	// and (47u) the deferred dispatch inside ExecuteFrame) is inside the Ray Tracing bucket's scope: that interval pauses around this one.
	auto* gpuTimers = Util::GpuPassTimers::GetSingleton();
	gpuTimers->End(Util::GpuBucket::RayTracing);
	gpuTimers->Begin(Util::GpuBucket::RayTracingAONormalPrep);
	ID3D11Buffer* cbs[1] = { aoNormalCB->CB() };
	ID3D11ShaderResourceView* srvs[4] = { a_depth, a_prevDepth, normal.SRV, motion.SRV };
	ID3D11UnorderedAccessView* uavs[2] = { backend.AONormalUAV(), a_writeDebug ? aoNormalDebug.uav.get() : nullptr };
	a_context->CSSetConstantBuffers(0, 1, cbs);
	a_context->CSSetShaderResources(0, 4, srvs);
	a_context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
	// (48a) RT reflections: the RT_REFLECT variant also reads last frame's REFLECTANCE (t4) and writes the reflection prep (u2).
	const bool reflect = a_reflect && aoNormalReflectCS && backend.ReflectPrepUAV();
	if (reflect) {
		ID3D11ShaderResourceView* reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE].SRV;
		ID3D11UnorderedAccessView* prep = backend.ReflectPrepUAV();
		a_context->CSSetShaderResources(4, 1, &reflectance);
		a_context->CSSetUnorderedAccessViews(2, 1, &prep, nullptr);
	}
	a_context->CSSetShader(reflect ? aoNormalReflectCS.get() : aoNormalCS.get(), nullptr, 0);
	a_context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[5]{};
	ID3D11UnorderedAccessView* nullUavs[3]{};
	ID3D11Buffer* nullCbs[1]{};
	a_context->CSSetShaderResources(0, reflect ? 5 : 4, nullSrvs);
	a_context->CSSetUnorderedAccessViews(0, reflect ? 3 : 2, nullUavs, nullptr);
	gpuTimers->End(Util::GpuBucket::RayTracingAONormalPrep);
	gpuTimers->Begin(Util::GpuBucket::RayTracing);
	if (a_writeDebug)
		aoNormalDebugFrame = globals::state->frameCount;
	a_context->CSSetConstantBuffers(0, 1, nullCbs);
	a_context->CSSetShader(nullptr, nullptr, 0);
}

bool RayTracing::EarlyNormalPrepPossible(std::uint32_t a_width, std::uint32_t a_height)
{
	// (47u) Start Sun Trace Earlier: the normal prep may go behind the ready signal only when its "last frame's depth" can come from the
	// copy (aoPrevDepth holds exactly last frame's DepthMask, at the same size) and the after-ready fence exists. Otherwise the 47t order.
	if (!aoSmoothFault.empty() || !ResolveAOTier().smoothNormals || !aoPrevDepth || !aoPrevDepthSRV)
		return false;
	const std::uint32_t frame = globals::state->frameCount;
	if (aoPrevDepthFrame == UINT32_MAX || aoPrevDepthFrame + 1 != frame || aoNormalPrev.frame != aoPrevDepthFrame || aoNormalPrev.width != a_width ||
		aoNormalPrev.height != a_height)
		return false;
	auto* depthMask = backend.DepthMaskTexture();
	if (!depthMask)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	depthMask->GetDesc(&desc);
	if (desc.Width != aoPrevDepthDesc.Width || desc.Height != aoPrevDepthDesc.Height || desc.Format != aoPrevDepthDesc.Format)
		return false;
	return backend.EnsureAfterReadyFence();
}

bool RayTracing::DispatchDeferredAONormal()
{
	// (47u) Inside ExecuteFrame, right after the D3D11 ready signal (or after it, when the frame never got that far): the normal prep with
	// last frame's depth from its copy, then this frame's DepthMask kept for the next one. True when the RTAO list reads what it wrote.
	if (!aoNormalDeferred.active)
		return false;
	aoNormalDeferred.active = false;
	auto* context = globals::d3d::context;
	DispatchAONormal(context, aoNormalDeferred.depth, aoPrevDepthSRV.get(), aoNormalDeferred.width, aoNormalDeferred.height, aoNormalDeferred.writeDebug,
		aoNormalDeferred.reflect);
	FinishDepthMaskFrame(aoNormalDeferred.width, aoNormalDeferred.height);
	return aoNormalsThisFrame || reflectPrepThisFrame;  // (48a) the reflection rays read it too
}

void RayTracing::FinishDepthMaskFrame(std::uint32_t a_width, std::uint32_t a_height)
{
	// (47u) After PrepareCS wrote this frame's DepthMask (and the normal prep read last frame's): remembered for the next frame's normal
	// prep, and -- with Start Sun Trace Earlier and Smooth Normals -- copied to aoPrevDepth (D3D11 only; the next frame's normal prep reads
	// it behind the ready signal while PrepareCS has already overwritten DepthMask). The copy reads DepthMask while the D3D12 traces read
	// it too (read / read).
	RememberAONormalFrame(a_width, a_height);
	auto* depthMask = backend.DepthMaskTexture();
	const bool wanted = aoSmoothFault.empty() && ResolveAOTier().smoothNormals && AOOn() && depthMask;
	if (!wanted) {
		aoPrevDepthFrame = UINT32_MAX;
		if (aoPrevDepth) {
			d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(aoPrevDepthDesc.Width) * aoPrevDepthDesc.Height * 4);
			aoPrevDepth = nullptr;
			aoPrevDepthSRV = nullptr;
		}
		return;
	}
	D3D11_TEXTURE2D_DESC desc{};
	depthMask->GetDesc(&desc);
	if (!aoPrevDepth || desc.Width != aoPrevDepthDesc.Width || desc.Height != aoPrevDepthDesc.Height || desc.Format != aoPrevDepthDesc.Format) {
		if (aoPrevDepth)
			d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(aoPrevDepthDesc.Width) * aoPrevDepthDesc.Height * 4);
		aoPrevDepth = nullptr;
		aoPrevDepthSRV = nullptr;
		D3D11_TEXTURE2D_DESC copyDesc = desc;
		copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		copyDesc.MiscFlags = 0;
		copyDesc.CPUAccessFlags = 0;
		copyDesc.Usage = D3D11_USAGE_DEFAULT;
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		auto* device = globals::d3d::device;
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		if (!device || FAILED(device->CreateTexture2D(&copyDesc, nullptr, texture.put())) || FAILED(device->CreateShaderResourceView(texture.get(), &srvDesc, srv.put()))) {
			aoPrevDepthFrame = UINT32_MAX;
			logger::warn("[RayTracing] Start Sun Trace Earlier: could not create the depth copy ({}x{}); the normal prep keeps its old place", desc.Width, desc.Height);
			return;
		}
		Util::SetResourceName(texture.get(), "RayTracing RTAO PrevDepth");
		aoPrevDepth = texture;
		aoPrevDepthSRV = srv;
		aoPrevDepthDesc = desc;
		d3d11Bytes += static_cast<std::uint64_t>(desc.Width) * desc.Height * 4;
	}
	globals::d3d::context->CopyResource(aoPrevDepth.get(), depthMask);
	aoPrevDepthFrame = globals::state->frameCount;
}

void RayTracing::PrepareDepthMask(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_prepare, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_mask,
	std::uint32_t a_width, std::uint32_t a_height, RayTracingRT::FrameInput& a_input)
{
	// (47j) RTAO's Smooth Normals read last frame's depth; (47u) with Start Sun Trace Earlier from aoPrevDepth, behind the ready signal.
	earlyNormalThisFrame = -1;
	bool defer = false;
	reflectPrepThisFrame = false;  // (48a)
	// (48d) Also for the reflection rays alone (RTAO off): the prep (RT_REFLECT variant) then writes only their input.
	if (a_input.ao || reflectWantedThisFrame) {
		const bool possible = EarlyNormalPrepPossible(a_width, a_height);
		RunAONormalPrep(a_context, a_depth, a_width, a_height, possible, a_input.ao != nullptr);
		defer = possible && aoNormalDeferred.active;
		earlyNormalThisFrame = aoNormalsThisFrame ? (defer ? 1 : 0) : -1;
	}
	// (48a) RT reflections: their rays go out only on a frame whose prep wrote their input. (48d) Their own list: RTAO not needed.
	a_input.reflect = (reflectWantedThisFrame && reflectPrepThisFrame) ? &reflectInput : nullptr;
	// Depth + engine mask -> the tracer's shared texture (combineCB holds this frame's constants).
	{
		ID3D11Buffer* cbs[1] = { combineCB->CB() };
		ID3D11ShaderResourceView* srvs[2] = { a_depth, a_mask };
		ID3D11UnorderedAccessView* uavs[1] = { backend.DepthMaskUAV() };
		a_context->CSSetConstantBuffers(0, 1, cbs);
		a_context->CSSetShaderResources(0, 2, srvs);
		a_context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		a_context->CSSetShader(a_prepare, nullptr, 0);
		a_context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
		ID3D11ShaderResourceView* nullSrvs[2]{};
		ID3D11UnorderedAccessView* nullUavs[1]{};
		a_context->CSSetShaderResources(0, 2, nullSrvs);
		a_context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
		a_context->CSSetShader(nullptr, nullptr, 0);
	}
	if (defer)
		a_input.afterReady = [this]() { return DispatchDeferredAONormal(); };
	else
		FinishDepthMaskFrame(a_width, a_height);  // DepthMask now holds this frame's depth
}

void RayTracing::FlushDeferredAONormal()
{
	// (47u) After ExecuteFrame: a deferred normal prep the frame never reached (no ready signal) runs now; RTAO did not go out then.
	if (!aoNormalDeferred.active)
		return;
	if (backend.HasFailed()) {
		aoNormalDeferred.active = false;
		return;
	}
	DispatchDeferredAONormal();
}

bool RayTracing::RunAONormalPrep(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_depth, std::uint32_t a_width, std::uint32_t a_height, bool a_defer,
	bool a_ao)
{
	aoNormalsThisFrame = false;
	aoNormalDeferred.active = false;
	reflectPrepThisFrame = false;  // (48a)
	// (47k) The Smooth Normals debug views: their texture only while one of them is on.
	const int normalView = aoDebugView == AODebugView::NormalConfidence ? 1 : (aoDebugView == AODebugView::NormalNs ? 2 : (aoDebugView == AODebugView::NormalNg ? 3 : 0));
	// (47n) Smooth Normals is a tier knob; a fault (its texture / shader) keeps it off for this session.
	// (48d) a_ao: this frame traces RTAO (false: the reflection rays alone use the prep; RTAO's smooth normals texture is not needed).
	const bool tierSmooth = a_ao && ResolveAOTier().smoothNormals && aoSmoothFault.empty();
	// (48a) RT reflections: the same prep (RT_REFLECT variant) feeds the reflection rays; it then runs even where the tier does not want
	// smooth normals for RTAO (RTAO keeps its depth normal then: its flag below only with the tier).
	const bool reflect = reflectWantedThisFrame && aoSmoothFault.empty();
	const bool smooth = tierSmooth || reflect;
	if ((!normalView || !tierSmooth) && aoNormalDebug.texture) {
		aoNormalDebug = {};
		aoNormalDebugWidth = aoNormalDebugHeight = 0;
	}
	if (!smooth) {
		if (backend.HasAONormalTexture())
			backend.ReleaseAONormalTexture();  // only while it is wanted
		return false;
	}
	const uint32_t frame = globals::state->frameCount;
	// Last frame's depth in DepthMask at the same size, and its camera.
	const bool prevValid = aoNormalPrev.frame != UINT32_MAX && aoNormalPrev.frame + 1 == frame && aoNormalPrev.width == a_width && aoNormalPrev.height == a_height;
	auto* renderer = globals::game::renderer;
	auto& normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR];
	if (!prevValid || !a_depth || !normal.SRV || !motion.SRV || !backend.DepthMaskSRV())
		return false;
	// (48d) RTAO's texture (u0) only when RTAO reads it; for the reflection rays alone u0 stays unbound (its writes are dropped).
	if (tierSmooth && !backend.EnsureAONormalTexture(a_width, a_height)) {
		aoSmoothFault = "its shared texture could not be created";
		logger::warn("[RayTracing] RTAO: Smooth Normals: {}; switched off for this session", aoSmoothFault);
		return false;
	}
	if (!tierSmooth && backend.HasAONormalTexture())
		backend.ReleaseAONormalTexture();
	// (48e) The plain variant only where RTAO's smooth normals read it (synchronous on first use, as before 48d); for the reflection rays
	// alone it is not needed (48d compiled it anyway).
	if (tierSmooth && !aoNormalCS) {
		aoNormalCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\RayTracing\\AONormalPrepCS.hlsl", {}, "cs_5_0")));
		if (!aoNormalCS) {
			aoSmoothFault = "AONormalPrepCS did not compile";
			logger::warn("[RayTracing] RTAO: Smooth Normals: {}; switched off for this session", aoSmoothFault);
			backend.ReleaseAONormalTexture();
			return false;
		}
	}
	// (48a) The RT_REFLECT variant (a failure only keeps the reflection rays off; RTAO is unaffected). (48e) Compiled in the background:
	// until it is ready the reflection rays are not prepared (this frame runs as without reflections).
	bool reflectPrep = reflect;
	if (reflectPrep && !aoNormalReflectCS) {
		bool failed = false;
		if (!PollAsyncShader(aoNormalReflectAsync, aoNormalReflectCS, L"Data\\Shaders\\RayTracing\\AONormalPrepCS.hlsl", { { "RT_REFLECT", "" } },
				"Ray Tracing AONormalPrepCS (RT_REFLECT)", failed) &&
			failed) {
			reflectOffReason = "AONormalPrepCS (RT_REFLECT) did not compile";
			logger::warn("[RayTracing] RT reflections: {}", reflectOffReason);
		}
	}
	reflectPrep = reflectPrep && aoNormalReflectCS && backend.ReflectPrepUAV();
	if (!tierSmooth && !reflectPrep)
		return false;
	if (!aoNormalCB)
		aoNormalCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<AONormalConstants>());
	const auto& fb = globals::game::frameBufferCached;
	const float4 camera = fb.GetCameraPosAdjust(0);
	AONormalConstants cb{};
	std::memcpy(cb.InvViewProj, &fb.GetCameraViewProjInverse(0), sizeof(cb.InvViewProj));
	std::copy_n(aoNormalPrev.viewProj, 16, cb.PrevViewProj);
	std::copy_n(aoNormalPrev.invViewProj, 16, cb.PrevInvViewProj);
	std::copy_n(aoNormalPrev.viewInverse, 16, cb.PrevViewInverse);
	cb.CameraDelta[0] = static_cast<float>(static_cast<double>(camera.x) - aoNormalPrev.camera[0]);
	cb.CameraDelta[1] = static_cast<float>(static_cast<double>(camera.y) - aoNormalPrev.camera[1]);
	cb.CameraDelta[2] = static_cast<float>(static_cast<double>(camera.z) - aoNormalPrev.camera[2]);
	cb.RenderSize[0] = static_cast<float>(a_width);
	cb.RenderSize[1] = static_cast<float>(a_height);
	cb.RenderSize[2] = 1.0f / static_cast<float>(a_width);
	cb.RenderSize[3] = 1.0f / static_cast<float>(a_height);
	// (47k) v2 and the debug view (its RGBA8 texture created on demand; a failure only drops the view).
	if (normalView && (!aoNormalDebug.texture || aoNormalDebugWidth != a_width || aoNormalDebugHeight != a_height)) {
		aoNormalDebug = {};
		if (CreateDenoiseTexture(this, aoNormalDebug.texture, aoNormalDebug.srv, aoNormalDebug.uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing RTAO NormalDebug")) {
			aoNormalDebugWidth = a_width;
			aoNormalDebugHeight = a_height;
		} else {
			aoNormalDebug = {};
		}
	}
	const bool writeDebug = normalView && aoNormalDebug.uav;
	cb.Flags[0] = 1u | 2u;  // (47n) v2 always; (47r) bit 2: the motion candidate reuses the camera one's search
	cb.Flags[1] = writeDebug ? static_cast<std::uint32_t>(normalView) : 0u;
	// (48d) The reflection pixel selection (RT_REFLECT only reads it).
	if (reflectPrep) {
		const float start = std::clamp(settings.ReflectionsRoughStart, 0.0f, 0.95f);
		cb.ReflectSelect[0] = settings.ReflectionsFullRange ? 2.0f : start;
		cb.ReflectSelect[1] = settings.ReflectionsFullRange ? 3.0f : std::max(std::clamp(settings.ReflectionsRoughEnd, 0.05f, 1.0f), start + 0.05f);
		cb.ReflectSelect[2] = settings.ReflectionsBeyondSSR ? 0.0f : ReflectionDistanceLimit();
		cb.ReflectFlags[0] = static_cast<std::uint32_t>(frameIndex % 1024u);
		cb.ReflectFlags[1] = settings.ReflectionsBestCandidate ? 1u : 0u;
	}
	// (47p) Pixels the trace does not read (its weight is 0 there: outside its rect or beyond the fade end) keep "no smooth normal".
	if (PerfOptions::settings.RtaoNormalFold && !writeDebug) {
		std::copy_n(aoInput.constants.rect, 4, cb.SkipRect);
		cb.SkipFade[0] = aoInput.constants.fade[1];
		cb.SkipFade[1] = 1.0f;
	}
	aoNormalCB->Update(cb);
	if (a_defer) {
		// (47u) Start Sun Trace Earlier: everything above is this frame's (constants, the trace's flags below); the dispatch itself goes
		// behind the D3D11 ready signal (DispatchDeferredAONormal), reading last frame's depth from its own copy (aoPrevDepth).
		aoNormalDeferred = { true, a_depth, a_width, a_height, writeDebug, reflectPrep };
	} else {
		DispatchAONormal(a_context, a_depth, backend.DepthMaskSRV(), a_width, a_height, writeDebug, reflectPrep);
	}
	reflectPrepThisFrame = reflectPrep;  // (48a)
	if (!tierSmooth)
		return true;  // (48a) only the reflection rays read it: RTAO keeps its depth normal (its flags unchanged)
	// The trace reads it.
	std::uint32_t flags;
	std::memcpy(&flags, &aoInput.constants.params[1], sizeof(flags));
	flags |= kAOTraceSmoothNormals | kAOTraceSmoothNormals2;  // (47n) v2 always
	std::memcpy(&aoInput.constants.params[1], &flags, sizeof(flags));
	aoNormalsThisFrame = true;
	return true;
}

void RayTracing::RememberAONormalFrame(std::uint32_t a_width, std::uint32_t a_height)
{
	const auto& fb = globals::game::frameBufferCached;
	const float4 camera = fb.GetCameraPosAdjust(0);
	aoNormalPrev.frame = globals::state->frameCount;
	aoNormalPrev.width = a_width;
	aoNormalPrev.height = a_height;
	std::memcpy(aoNormalPrev.viewProj, &fb.GetCameraViewProj(0), sizeof(aoNormalPrev.viewProj));
	std::memcpy(aoNormalPrev.invViewProj, &fb.GetCameraViewProjInverse(0), sizeof(aoNormalPrev.invViewProj));
	std::memcpy(aoNormalPrev.viewInverse, &fb.GetCameraViewInverse(0), sizeof(aoNormalPrev.viewInverse));
	aoNormalPrev.camera[0] = camera.x;
	aoNormalPrev.camera[1] = camera.y;
	aoNormalPrev.camera[2] = camera.z;
}

void RayTracing::ReleasePointDenoiser()
{
	UnbindPointLights();
	for (auto* t : { &pointVis[0], &pointVis[1], &pointIdx[0], &pointIdx[1], &pointDist[0], &pointDist[1] })
		*t = {};
	pointHistoryValid = false;
	d3d11Bytes -= std::min(d3d11Bytes, pointD3D11Bytes);
	pointD3D11Bytes = 0;
	ReleasePointWide();  // (48a)
	ReleasePointV2();    // (50b)
}

void RayTracing::ReleasePointV2()
{
	// (50b) The history lengths and the DLSS RR raw-ray textures (unbound first: t40 / t46 may hold the raw ones).
	if (pointBound && pointRRBound)
		UnbindPointLights();
	pointLen[0] = {};
	pointLen[1] = {};
	pointRaw = {};
	pointRawWide = {};
	pointLenValid = false;
	pointRRBound = false;
	pointEffectSwapped = false;
	d3d11Bytes -= std::min(d3d11Bytes, pointV2D3D11Bytes);
	pointV2D3D11Bytes = 0;
}

bool RayTracing::EnsurePointV2(std::uint32_t a_width, std::uint32_t a_height, bool a_rr, bool a_wide)
{
	// (50b) pointLen (both, R32_UINT) for every mode of PointDenoiseV2CS; pointRaw only for DLSS RR, pointRawWide for DLSS RR with slots 4 - 5.
	// Created on first use, at the point textures' size; a size change drops them all (no history length).
	auto sizeOf = [](const DenoiseTexture& a_t, std::uint32_t& o_w, std::uint32_t& o_h) {
		D3D11_TEXTURE2D_DESC d{};
		a_t.texture->GetDesc(&d);
		o_w = d.Width;
		o_h = d.Height;
	};
	if (pointLen[0].texture) {
		std::uint32_t w = 0, h = 0;
		sizeOf(pointLen[0], w, h);
		if (w != a_width || h != a_height)
			ReleasePointV2();
	}
	bool ok = true;
	if (!pointLen[0].texture || !pointLen[1].texture) {
		pointLenValid = false;
		for (int i = 0; i < 2 && ok; ++i)
			ok = CreateDenoiseTexture(this, pointLen[i].texture, pointLen[i].srv, pointLen[i].uav, a_width, a_height, DXGI_FORMAT_R32_UINT, "RayTracing PointLen");
	}
	if (ok && a_rr && !pointRaw.texture)
		ok = CreateDenoiseTexture(this, pointRaw.texture, pointRaw.srv, pointRaw.uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing PointRaw");
	if (ok && a_rr && a_wide && !pointRawWide.texture)
		ok = CreateDenoiseTexture(this, pointRawWide.texture, pointRawWide.srv, pointRawWide.uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing PointRawWide");
	if (!ok) {
		ReleasePointV2();
		return false;
	}
	d3d11Bytes -= std::min(d3d11Bytes, pointV2D3D11Bytes);
	pointV2D3D11Bytes = static_cast<std::uint64_t>(a_width) * a_height * 4 * (2 + (pointRaw.texture ? 1 : 0) + (pointRawWide.texture ? 1 : 0));
	d3d11Bytes += pointV2D3D11Bytes;
	return true;
}

void RayTracing::BindEffectPointVisibility()
{
	// (50b) DLSS RR mode: the effect draw reads the filtered copy (pointVis / pointWide of this frame) at t40 / t46 instead of the raw rays.
	if (!pointBound || !pointRRBound || !pointDenoised)
		return;
	const std::uint32_t latest = pointWrite ^ 1u;  // FinishPointLights flipped it after writing
	ID3D11ShaderResourceView* vis = pointVis[latest].srv.get();
	ID3D11ShaderResourceView* wide = pointWideThisFrame ? pointWide[latest].srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(40, 1, &vis);
	globals::d3d::context->PSSetShaderResources(46, 1, &wide);
	pointEffectSwapped = true;
}

void RayTracing::RestoreEffectPointVisibility()
{
	// (50b) Back to the raw rays for the Lighting draws that follow.
	if (!pointEffectSwapped)
		return;
	pointEffectSwapped = false;
	if (!pointBound || !pointRRBound)
		return;
	ID3D11ShaderResourceView* vis = pointRaw.srv.get();
	ID3D11ShaderResourceView* wide = pointWideThisFrame ? pointRawWide.srv.get() : nullptr;
	globals::d3d::context->PSSetShaderResources(40, 1, &vis);
	globals::d3d::context->PSSetShaderResources(46, 1, &wide);
}

void RayTracing::ReleasePointWide()
{
	// (48a) Both sides: the denoised slots 4 - 5 here, the trace's texture in the backend. The pixel shaders' t46 is unbound first.
	if (pointBound) {
		ID3D11ShaderResourceView* nullSrv = nullptr;
		globals::d3d::context->PSSetShaderResources(46, 1, &nullSrv);
	}
	pointWide[0] = {};
	pointWide[1] = {};
	d3d11Bytes -= std::min(d3d11Bytes, pointWideD3D11Bytes);
	pointWideD3D11Bytes = 0;
	if (backend.IsReady() && backend.PointWideReady())
		backend.ReleasePointWideTextures();
	pointWideThisFrame = false;
	pointWideTraced = false;
	pointWideHistoryValid = false;
	pointWideIdleFrames = 0;
}

bool RayTracing::EnsurePointWide(std::uint32_t a_width, std::uint32_t a_height)
{
	// (48a) Lights per pixel 5 - 6: created on first use; a fresh texture holds no last frame (no sticky / history from it).
	const bool backendWasReady = backend.PointWideReady();
	if (!backend.EnsurePointWideTextures())
		return false;
	if (!backendWasReady)
		pointWideTraced = false;
	if (pointWide[0].texture && pointWide[1].texture) {
		D3D11_TEXTURE2D_DESC d{};
		pointWide[0].texture->GetDesc(&d);
		if (d.Width == a_width && d.Height == a_height)
			return true;
	}
	pointWide[0] = {};
	pointWide[1] = {};
	d3d11Bytes -= std::min(d3d11Bytes, pointWideD3D11Bytes);
	pointWideD3D11Bytes = 0;
	pointWideHistoryValid = false;
	bool ok = true;
	for (int i = 0; i < 2 && ok; ++i)
		ok = CreateDenoiseTexture(this, pointWide[i].texture, pointWide[i].srv, pointWide[i].uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing PointWide");
	if (!ok) {
		pointWide[0] = {};
		pointWide[1] = {};
		logger::warn("[RayTracing] Lights per pixel above 4: could not create the denoiser's slot 4 - 5 textures; 4 lights per pixel are traced");
		return false;
	}
	pointWideD3D11Bytes = static_cast<std::uint64_t>(a_width) * a_height * 2 * 4;
	d3d11Bytes += pointWideD3D11Bytes;
	return true;
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
	pointWideThisFrame = false;  // (48a)
	pointExtraThisFrame = false;  // (48c)
	waterTraced = false;  // (47i)
	waterGateLights.clear();  // (47p)
	if (!PointLightsOn() || !backend.PointAvailable())  // (47n) no MaskTemp needed (the sun may be "Off")
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
	config.ssplsNonRT = true;  // (46y) Screen-Space Shadows for Non-RT Casters: permanent (47o)
	config.waterBed = pointWaterOn;  // (47h) Point Light Shadows on Water: permanent (47o) unless its resources failed
	config.ssplsContact = settings.PointLightsSSPLSContact;
	const PointTierParams tier = ResolvePointTier();  // (47n)
	config.maxSources = static_cast<std::uint32_t>(std::clamp(tier.maxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources)));
	config.maxDistance = tier.distance;
	config.clearance = settings.PointLightsClearance;
	// (46c) Indoors: spot shadow lights, window beams, the lights whose shadow map was left out this frame.
	config.interior = a_interior;
	config.beams = settings.PointLightsWindowBeams;
	// (46d) The player's lights (never shadowed by the player's own body) and ownerless spell / effect lights.
	// (47q) Tier knobs: the player's lights from Medium up, spell / effect lights in High and Ultra (Custom: the two keys). NPC-carried
	// lights are always traced.
	config.player = tier.player;
	config.effects = tier.effects;
	// (46s)
	config.meshless = static_cast<std::uint32_t>(std::clamp(settings.PointLightsMeshless, 0, 3));  // (50a) + Soft
	// (46x) Direct fills' emitter. (47m) Native shadow lights, stable selection, steady flickering lights, merge hysteresis, direct
	// fills, plugin-only fill fixtures, wider window beams, portals and room boxes are always on.
	config.directEmitter = std::min(std::max(settings.PointLightsSize, 0.0f) * 4.0f, 64.0f);
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
	UpdateBeamShares();
	config.forced = a_interior && !skippedLights.empty() ? &skippedLights : nullptr;
	// (46e) The characters in this frame's TLAS: a light one of them carries skips that character's body.
	config.owners = (a_input.skinDraws && !a_input.skinDraws->empty()) ? &characters.Owners() : nullptr;
	// (46q) Carrier Casts Shadows (always, 47m): the player in third person is its own lights' carrier too (first person: as 46p,
	// the player's body is never tested against its own lights; the first-person arm would cover most of the view).
	{
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		config.playerCarrier = playerCamera && !playerCamera->IsInFirstPerson();
	}
	const double camera[3] = { a_camX, a_camY, a_camZ };
	const float worldRect[4] = { static_cast<float>(a_traceRect[0] + a_camX), static_cast<float>(a_traceRect[1] + a_camY),
		static_cast<float>(a_traceRect[2] + a_camX), static_cast<float>(a_traceRect[3] + a_camY) };
	const auto& viewProj = globals::game::frameBufferCached.GetCameraViewProj(0);
	static_assert(sizeof(viewProj) == sizeof(float) * 16);
	float viewProjF[16];
	std::memcpy(viewProjF, &viewProj, sizeof(viewProjF));
	const std::uint32_t scan = pointSelector.Select(config, camera, viewProjF, worldRect, globals::state->frameCount, pointSources.data());
	lastSlotBeams = pointSelector.SlotBeams();  // (46g) the warm-up report reads the counters of these slots a few frames later
	// (47p) The water prep gate's sources (world position, reach).
	waterGateLights.clear();
	for (std::uint32_t s = 0; s < scan && s < pointSources.size(); ++s) {
		const auto& p = pointSources[s].posRadius;
		waterGateLights.push_back({ p[0] + a_camX, p[1] + a_camY, p[2] + a_camZ, static_cast<double>(std::max(p[3], 0.0f)) });
	}
	pointEmitterMask = pointSelector.EmitterSlots();  // (46x)
	pointEmitterRadius = config.directEmitter;
	if (scan == 0)
		return false;

	std::uint32_t K = static_cast<std::uint32_t>(ValidPointK(a_interior ? tier.kInterior : tier.k));
	// (48a) Lights per pixel 5 - 6: slots 4 - 5 in their own textures, made on the first such frame (failing that: 4 slots). Freed again
	// after kPointWideIdleFrames point frames at K <= 4 (outdoors, a lower tier), so K <= 4 costs nothing once they are gone.
	// (48c) Extra Lights Where Crowded (K < 6): the extra ranks K..3 go to the main slots; ranks 4 - 5 need the slot 4 - 5 textures, kept (made)
	// while the trace reports pixels whose extra rank 4 - 5 passed its cut (read back a few frames late; counted also while the textures are
	// missing), freed like 48a after kPointWideIdleFrames point frames without. (48c) After the slot 4 - 5 denoiser failed: 4 slots.
	const bool extra = K < 6;  // (48c) Extra Lights Where Crowded: always on (48o)
	const bool extraWide = extra && backend.GetStats().pointExtraWideWantPerFrame > 0;
	const bool wantWide = !pointWideDenoiseFailed && (K > 4 || extraWide);
	const bool wide = wantWide && EnsurePointWide(width, height);
	if (K > 4 && !wide)
		K = 4;
	pointExtraThisFrame = extra;
	if (wide) {
		pointWideIdleFrames = 0;
	} else if (backend.PointWideReady() || pointWide[0].texture) {
		// (49b) Hitch fix D: kept for the session once made (no free / re-create churn: +15.8 MB at most).
		if (!ReserveOn() && ++pointWideIdleFrames >= kPointWideIdleFrames) {
			logger::info("[RayTracing] Lights per pixel above 4: unused for {} frames, slot 4 - 5 textures freed", kPointWideIdleFrames);
			ReleasePointWide();
		}
	}
	const bool widePrev = wide && pointWideTraced;
	pointWideTraced = wide;
	pointWideThisFrame = wide;
	pointInput.wide = wide;
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
	flags |= kPointFlagSticky;  // (46g) Stable Light Selection: fixed (47m)
	// (46y) Screen-Space Shadows for Non-RT Casters: the trace marks the pixels whose surface is a traced caster (PLIdx.w bit 7).
	if (globals::features::screenSpacePointLightShadows.loaded && globals::features::screenSpacePointLightShadows.settings.Enable)
		flags |= kPointFlagRtSurface;
	// (46n) Window Beam Culling: fixed (47m) at "Faces Toward the Room" ((47q) no flag: the shader always culls so).
	// (47i) Point Light Shadows on Water: the water distance of last frame (its prep ran after last frame's water draws) is traced
	// from; with no prep last frame (no water drawn) the distance is cleared, so the Water shader never matches a stale one.
	std::memcpy(waterInvViewProj, a_input.constants.invViewProj, sizeof(waterInvViewProj));
	waterRenderWidth = width;
	waterRenderHeight = height;
	// (47l) Distant LOD water may be left out of the prep this frame only when the loaded cells' edge lies farther from the camera than
	// any point-light source reaches (sources within PointLightsDistance of the camera, radius at most 4096; 512 units of margin).
	waterLodSkip = false;
	if (!a_interior) {  // (47l) Skip Distant LOD Water: always (47m)
		float core[4];
		if (scene.GetLoadedRect(core, false)) {
			const double edge = std::min({ a_camX - core[0], a_camY - core[1], core[2] - a_camX, core[3] - a_camY });
			waterLodSkip = edge > static_cast<double>(tier.distance) + 4096.0 + 512.0;
		}
	}
	if (pointWaterOn) {
		if (backend.EnsurePointWaterTextures()) {
			const uint32_t frame = globals::state->frameCount;
			if (waterPrepFrame != UINT32_MAX && frame - waterPrepFrame == 1u && waterDistValid) {
				flags |= kPointFlagWater;
				waterTraced = true;
			} else if (waterDistValid) {
				const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
				globals::d3d::context->ClearUnorderedAccessViewFloat(backend.PointWaterDistUAV(), zero);
				waterDistValid = false;
			}
		}
	} else if (backend.PointWaterReady()) {
		backend.ReleasePointWaterTextures();
		waterDistValid = false;
	}
	// (47w) Adaptive skip coverage counts: (48o) never (Skip the Game's Shadow Maps and Adaptive are fixed off).
	pointInput.coverageCount = 0;
	// (48o) Always on: (47x) Trace Lights on Curved Surfaces Reliably; (47z) Window Beams: Respect Their Range; (48a) Window Beams: Smooth
	// Edges; (48k) Noise Fix: Decorrelated Sampling, Consistent Dark Side. (49h) Close-Contact Shadows on Characters: a switch again (default on).
	if constexpr (Settings::PointCloseContact)
		flags |= kPointFlagCloseContact;
	flags |= kPointFlagCurvedRanking | kPointFlagBeamRange | kPointFlagBeamSmooth | kPointFlagNoiseDecorrelate;
	// (49m) Dark Side Test (0 Off / 1 Original = 48k-49j / 2 Tolerant), Grazing-Angle Offset, Smooth Shadow Edges (extra rays), Soft Hand-Off
	// for Weak Lights. The last two switch the trace to the fractional encoding, which the denoiser of this frame must read too.
	if constexpr (Settings::PointDarkSideTest >= 1)
		flags |= kPointFlagNoiseDarkSide;
	if constexpr (Settings::PointDarkSideTest >= 2)
		flags |= kPointFlagDarkTolerant;
	if constexpr (Settings::PointGrazingOffset)
		flags |= kPointFlagGrazeOffset;
	if constexpr (Settings::PointSoftCut)
		flags |= kPointFlagSoftCut;
	pointFractionalThisFrame = settings.PointSoftCut;  // (50b) Smooth Shadow Edges (extra rays) deleted
	// (50b) Blue-Noise Sampling (loaded on first use; its copy goes out in this frame's point list before the trace).
	if (settings.PointBlueNoise && backend.EnsurePointBlueNoise())
		flags |= kPointFlagBlueNoise;
	// (50e) RR: White-Noise Sampling: on the DLSS RR route (the denoiser of this frame, decided as FinishPointLights will) the disc samples
	// come from an independent hash per pixel, frame, slot and source (Ray Reconstruction wants uncorrelated per-pixel noise; the tiled
	// blue noise is a screen-space pattern shared by the lights). Other denoisers: unchanged.
	if (settings.PointRRWhiteNoise && EffectivePointDenoiser() == kPointRR)
		flags |= kPointFlagWhiteNoise;
	if (wide)  // (48a)
		flags |= kPointFlagWide | (widePrev ? kPointFlagWidePrev : 0u);
	if (extra)  // (48c)
		flags |= kPointFlagExtra;
	// (47z) Stable Light Choice When the Camera Moves: last trace's camera -> this frame's camera-relative positions to last trace's clip
	// space, one matrix (last view-projection x the translation by the camera's move), appended to the sources (Backend). The trace
	// reads its copy of last frame's PLIdx there. Without a last camera: 47y (the same pixel).
	pointInput.reproject = false;
	// (48c) This frame's jitter in NDC (x right, y up), the offset between the jittered and the unjittered view-projection of the same frame:
	// jittered row r = unjittered row r + j_r x row 3 (r = 0, 1), so j_r = (difference . row 3) / |row 3|^2. Not finite or more than 4 px:
	// none (a matrix that is not a jittered copy). Recorded every trace, so the first frame after the switch goes on has last frame's too.
	float jitterNow[2] = { 0.0f, 0.0f };
	{
		const auto& unjittered = globals::game::frameBufferCached.GetCameraViewProjUnjittered(0);
		float unjitteredF[16];
		std::memcpy(unjitteredF, &unjittered, sizeof(unjitteredF));
		double w2 = 0.0;
		for (int c4 = 0; c4 < 4; ++c4)
			w2 += static_cast<double>(viewProjF[12 + c4]) * viewProjF[12 + c4];
		for (int r = 0; r < 2 && w2 > 1e-20; ++r) {
			double dot = 0.0;
			for (int c4 = 0; c4 < 4; ++c4)
				dot += (static_cast<double>(viewProjF[r * 4 + c4]) - unjitteredF[r * 4 + c4]) * viewProjF[12 + c4];
			const double j = dot / w2;
			const double limit = 8.0 / static_cast<double>(r == 0 ? width : height);  // 4 px in NDC (2 / size per px)
			jitterNow[r] = std::isfinite(j) && std::abs(j) <= limit ? static_cast<float>(j) : 0.0f;
		}
	}
	if (pointPrevValid) {  // (48o) Stable Light Choice When the Camera Moves always on
		const double delta[3] = { a_camX - pointPrevCamera[0], a_camY - pointPrevCamera[1], a_camZ - pointPrevCamera[2] };
		for (int r = 0; r < 4; ++r) {
			const float* row = pointPrevViewProj + r * 4;
			pointInput.reprojectMatrix[r * 4 + 0] = row[0];
			pointInput.reprojectMatrix[r * 4 + 1] = row[1];
			pointInput.reprojectMatrix[r * 4 + 2] = row[2];
			pointInput.reprojectMatrix[r * 4 + 3] = static_cast<float>(static_cast<double>(row[3]) + row[0] * delta[0] + row[1] * delta[1] + row[2] * delta[2]);
		}
		// (48c) Stable Light Choice: Jitter Fix. Last trace's matrix is jittered by its jitter, this frame's positions come from pixel centres
		// under this frame's jitter: a still camera landed (this jitter - last jitter) off the pixel centre, often in the neighbour (48b).
		// Shift the result by this frame's jitter minus last frame's (clip.xy += delta x clip.w: rows 0 / 1 += delta x row 3), so a still
		// camera maps every pixel centre onto itself; a moving camera keeps the true motion. (48o) Always on.
		{
			const float jx = jitterNow[0] - pointPrevJitter[0], jy = jitterNow[1] - pointPrevJitter[1];
			for (int c4 = 0; c4 < 4; ++c4) {
				const float w = pointInput.reprojectMatrix[12 + c4];
				pointInput.reprojectMatrix[0 + c4] += jx * w;
				pointInput.reprojectMatrix[4 + c4] += jy * w;
			}
		}
		pointInput.reproject = true;
		flags |= kPointFlagStickyReproject;
	}
	std::copy_n(viewProjF, 16, pointPrevViewProj);
	pointPrevJitter[0] = jitterNow[0];  // (48c)
	pointPrevJitter[1] = jitterNow[1];
	pointPrevCamera[0] = a_camX;
	pointPrevCamera[1] = a_camY;
	pointPrevCamera[2] = a_camZ;
	pointPrevValid = true;
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
	c.extra[3] = std::clamp(settings.CarrierClearance, 0.0f, 64.0f);  // (46q) Carrier Casts Shadows: fixed (47m); (47n) the shader's < 0 path is gone
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
		if (pointSources[s].info[0] & (RayTracingPointLights::kSourceBeam | RayTracingPointLights::kSourceAim))  // (46x) aimed direct fills: beam-like rays
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
	pointInput.alpha = (a_interior || tier.alphaOutdoors) && backend.PointAlphaAvailable();
	a_input.pointLights = &pointInput;
	pointK = K;
	return true;
}

void RayTracing::FinishPointLights()
{
	// (50b) Point Light Shadows > Denoiser: Off / Penumbra v2 / DLSS RR (also while Upscaling's Ray Reconstruction runs) take PointDenoiseV2CS;
	// Penumbra this 50a code, unchanged.
	const int mode = EffectivePointDenoiser();
	if (mode != kPointPenumbra) {
		FinishPointLightsV2(mode);
		if (!pointV2Failed)
			return;
	}
	if (pointLen[0].texture || pointRaw.texture)
		ReleasePointV2();  // (50b) not needed by Penumbra
	pointModeThisFrame = kPointPenumbra;
	// (48a) The shader variant this frame needs: slots 4 - 5 (K > 4) and / or Window Beams: Smooth Edges (only with soft beam slots; outdoors
	// and without beams the plain 47z shader runs). A variant that fails to compile falls back to the plain one (its extras off).
	bool wide = pointWideThisFrame && backend.PointWideSRV() && pointWide[0].texture && pointWide[1].texture;
	bool beamSmooth = pointBeamMask != 0 && pointBeamTan > 0.0f;
	// (48k) Point Lights: Noise Fix: the PL_FIX twin of the variant below while a denoiser part is on; it failing to compile = the 48j variants
	// from then on this session (the trace parts stay as set).
	const bool noiseFixWanted = PointNoiseFixDenoiserWanted();
	// (48m) Moving Characters: Clean Shadow History (independent of the noise fix): the PL_CLEAN twin, with last frame's motion vectors bound
	// (whatever Keep Shadow History says). (48o) Always on. It failing to compile = without it from then on this session.
	ID3D11ShaderResourceView* const motionTargetSRV = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
	bool clean = motionTargetSRV && !pointCleanFailed;  // (48o) Clean Shadow History always on
	bool noiseFix = false;
	ID3D11ComputeShader* shader = nullptr;
	if (clean) {
		shader = PointDenoiseShader(wide, beamSmooth, noiseFixWanted && !pointNoiseFixFailed, true);
		if (shader) {
			noiseFix = noiseFixWanted && !pointNoiseFixFailed;
		} else {
			clean = false;
			pointCleanFailed = true;
			logger::warn("[RayTracing] PointDenoiseCS (clean history) did not compile: Moving Characters: Clean Shadow History is off this session");
		}
	}
	if (!shader && noiseFixWanted && !pointNoiseFixFailed) {
		shader = PointDenoiseShader(wide, beamSmooth, true);
		if (shader) {
			noiseFix = true;
		} else {
			pointNoiseFixFailed = true;
			logger::warn("[RayTracing] PointDenoiseCS (noise fix) did not compile: the point-light denoiser runs without the noise fix this session");
		}
	}
	if (!shader)
		shader = (wide || beamSmooth) ? PointDenoiseShader(wide, beamSmooth) : nullptr;
	if (!shader && wide && beamSmooth) {
		// (48c) Keep slots 4 - 5 without the beam edges when only the combined variant failed.
		shader = PointDenoiseShader(true, false);
		if (shader)
			beamSmooth = false;
	}
	if (!shader) {
		if (wide && !pointWideDenoiseFailed) {
			// (48c) The slot 4 - 5 variant does not compile: 4 slots from the next trace on (no K 5 - 6, no extra ranks 4 - 5).
			pointWideDenoiseFailed = true;
			logger::warn("[RayTracing] PointDenoiseCS (slots 4-5) did not compile: 4 lights per pixel are traced from now on");
		}
		wide = false;
		beamSmooth = false;
		shader = PointDenoiseShader(false, false);
	}
	// (48c) This frame denoises (and binds) 4 slots only: the light tags (PointLightTag) and the shadow mask must not look in slots 4 - 5
	// (48b kept tagging kTagWide after the fallback: Lighting looked in the unbound t46).
	if (!wide && pointWideThisFrame) {
		pointWideThisFrame = false;
		pointWideHistoryValid = false;
	}
	if (!shader) {
		SwitchOffPointLights("the point-light denoiser (PointDenoiseCS) did not compile");
		return;
	}
	if (!pointVis[0].texture || !backend.PointVisibilitySRV() || !backend.PointSourcesSRV())
		return;
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	const std::uint32_t write = pointWrite, read = pointWrite ^ 1u;
	const bool plDebug = PointDenoiserDebugView() && debugColorUAV;  // (48m) + Moving Pixels / Filter Path
	// (47y) Moving Characters: Keep Shadow History: last frame's motion vectors (the G-buffer of this frame is not drawn yet; StartDeferred
	// clears the target later), as RTAO's normal prep reads them. The shader validates its guess against the stored history distance.
	// (48m) Also bound for Clean Shadow History (the PL_CLEAN variant runs this frame); kPointDenoiseMotion stays Keep Shadow History's.
	ID3D11ShaderResourceView* motionSRV = motionTargetSRV;  // (48o) Keep Shadow History always on
	PointDenoiseCB cb{};
	cb.Params[0] = settings.PointLightsSize;
	cb.Params[1] = kPointHistoryWeight;
	std::uint32_t flags = (pointHistoryValid && prevCameraValid ? kPointDenoiseHistory : 0u) | (plDebug ? kPointDenoiseDebug : 0u) |
	                            kPointDenoiseSmooth |  // (47x)
	                            (motionSRV ? kPointDenoiseMotion : 0u) | kPointDenoiseNoHistorySmooth |  // (47y)
	                            (beamSmooth ? kPointDenoiseBeamSmooth : 0u);  // (48a)
	// (48k) The noise fix's denoiser parts (only read by the PL_FIX variant; 0 with the master off).
	if (noiseFix)
		flags |= kPointDenoiseNoiseDecorrelate | kPointDenoiseNoiseStable | kPointDenoiseNoiseCloseBlur;
	// (48m) Moving Characters: Clean Shadow History (only read by the PL_CLEAN variant).
	if (clean)
		flags |= kPointDenoiseCleanDetect | kPointDenoiseClean;
	// (49i) Smooth Hard Edges / Steady Edges (only read by the PL_FIX variant).
	if (noiseFix) {
		flags |= settings.PointSmoothHardEdges ? kPointDenoiseSmoothHard : 0u;
		flags |= settings.PointSteadyEdges ? kPointDenoiseSteadyEdges : 0u;
	}
	// (49m) The trace wrote the fractional encoding (Smooth Shadow Edges (extra rays) / Soft Hand-Off for Weak Lights): every variant reads it.
	if (pointFractionalThisFrame)
		flags |= kPointDenoiseFractional;
	// (48a) Lights per pixel 5 - 6: their history only when last frame denoised them too and the main history is valid.
	const bool wideHistory = wide && pointWideHistoryValid && (flags & kPointDenoiseHistory);
	flags |= (wide ? kPointDenoiseWide : 0u) | (wideHistory ? kPointDenoiseWideHistory : 0u);
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
		// (46x) Direct fills without a window spot: their own (larger) emitter radius sets their penumbra width.
		cb.Emitter[0] = static_cast<std::uint32_t>(pointEmitterMask);
		cb.Emitter[1] = static_cast<std::uint32_t>(pointEmitterMask >> 32);
		std::memcpy(&cb.Emitter[2], &pointEmitterRadius, sizeof(float));
		cb.Emitter[3] = frameIndex;  // (47x) Smoother Shadow Edges: the taps' rotation changes every frame
	}
	{
		// (46d) Moving / new sources: their history is dropped in proportion (slot s in byte s % 4 of Motion[s / 4]).
		const auto& motion = pointSelector.SlotMotion();
		for (std::uint32_t s = 0; s < RayTracingPointLights::kMaxSources; ++s)
			cb.Motion[s / 4] |= static_cast<std::uint32_t>(motion[s]) << ((s % 4) * 8);
		// (50a) Soft fill lights: their emitter radius per slot (all 0 without fills: every slot as before).
		const auto& fill = pointSelector.FillEmitterBytes();
		for (std::uint32_t s = 0; s < RayTracingPointLights::kMaxSources; ++s)
			cb.FillEmitter[s / 4] |= static_cast<std::uint32_t>(fill[s]) << ((s % 4) * 8);
	}
	pointDenoiseCB->Update(cb);

	timers->Begin(Util::GpuBucket::RayTracingPointDenoise);
	ID3D11Buffer* cbs[2] = { combineCB->CB(), pointDenoiseCB->CB() };
	// (48a) t7 / t8 / u4: slots 4 - 5 (null without them: the shader does not read or write them then).
	ID3D11ShaderResourceView* srvs[9] = { backend.PointVisibilitySRV(), backend.PointSourcesSRV(), backend.DepthMaskSRV(), pointVis[read].srv.get(),
		pointIdx[read].srv.get(), pointDist[read].srv.get(), motionSRV, wide ? backend.PointWideSRV() : nullptr, wideHistory ? pointWide[read].srv.get() : nullptr };
	ID3D11UnorderedAccessView* uavs[5] = { pointVis[write].uav.get(), pointIdx[write].uav.get(), pointDist[write].uav.get(), plDebug ? debugColorUAV.get() : nullptr,
		wide ? pointWide[write].uav.get() : nullptr };
	context->CSSetConstantBuffers(0, 2, cbs);
	context->CSSetShaderResources(0, 9, srvs);
	context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch((pending.width + 7) / 8, (pending.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[9]{};
	ID3D11UnorderedAccessView* nullUavs[5]{};
	ID3D11Buffer* nullCbs[2]{};
	context->CSSetShaderResources(0, 9, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 5, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 2, nullCbs);
	context->CSSetShader(nullptr, nullptr, 0);
	timers->End(Util::GpuBucket::RayTracingPointDenoise);

	// For the pixel shaders of this frame (RayTracing/PointLightShadows.hlsli).
	// (47i) t42 / t43: the visibility traced from last frame's water and that water's distance (null: GetWater falls back).
	const bool water = waterTraced && backend.PointWaterReady();
	ID3D11ShaderResourceView* views[4] = { pointVis[write].srv.get(), pointIdx[write].srv.get(), water ? backend.PointWaterVisSRV() : nullptr,
		water ? backend.PointWaterDistSRV() : nullptr };
	context->PSSetShaderResources(40, 4, views);
	// (48a) t46: slots 4 - 5 (null with K <= 4; the light tags carry kTagWide only on wide frames).
	ID3D11ShaderResourceView* wideView = wide ? pointWide[write].srv.get() : nullptr;
	context->PSSetShaderResources(46, 1, &wideView);
	pointWideHistoryValid = wide;
	pointBound = true;
	pointWrite ^= 1u;
	pointHistoryValid = true;
	pointDenoised = true;
}

void RayTracing::FinishPointLightsV2(int a_mode)
{
	// (50b) PointDenoiseV2CS: Penumbra v2 (history-length aware), Off (3x3 only, no history) or DLSS RR (Lighting reads the decoded rays;
	// the filtered copy for the lit effects). A shader or texture failure: Penumbra from then on this session (pointV2Failed).
	constexpr std::uint32_t kV2Off = 131072u, kV2RR = 262144u, kV2LenHistory = 524288u;  // PointDenoiseV2CS
	bool wide = pointWideThisFrame && backend.PointWideSRV() && pointWide[0].texture && pointWide[1].texture;
	// (50e) Moving Characters: Clean Shadow History (Point Light Shadows page): the PL_CLEAN twin, with last frame's motion vectors (t6).
	// With the switch off it runs only for the Moving Pixels view (detection only: the picture as without it); otherwise the 50d shader.
	ID3D11ShaderResourceView* const motionSRV = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR].SRV;
	const bool plDebug = PointDenoiserDebugView() && debugColorUAV;
	const bool cleanUsed = settings.PointV2CleanHistory && a_mode != kPointOff && motionSRV && !pointV2CleanFailed;
	bool clean = cleanUsed || (plDebug && debugView == DebugView::PLMoving && a_mode != kPointOff && motionSRV && !pointV2CleanFailed);
	ID3D11ComputeShader* shader = nullptr;
	if (clean) {
		shader = PointDenoiseV2Shader(wide, true);
		if (!shader) {  // either twin failing: without it from then on (the plain shaders below keep their own fallbacks)
			clean = false;
			pointV2CleanFailed = true;
			logger::warn("[RayTracing] PointDenoiseV2CS (clean history) did not compile: Moving Characters: Clean Shadow History is off this session");
		}
	}
	if (!shader)
		shader = PointDenoiseV2Shader(wide);
	if (!shader && wide) {
		if (!pointWideDenoiseFailed) {
			pointWideDenoiseFailed = true;
			logger::warn("[RayTracing] PointDenoiseV2CS (slots 4-5) did not compile: 4 lights per pixel are traced from now on");
		}
		wide = false;
		shader = PointDenoiseV2Shader(false);
	}
	if (!wide && pointWideThisFrame) {
		pointWideThisFrame = false;
		pointWideHistoryValid = false;
	}
	const bool rr = a_mode == kPointRR;
	if (!shader || !EnsurePointV2(pending.width, pending.height, rr, wide)) {
		pointV2Failed = true;
		ReleasePointV2();
		logger::warn("[RayTracing] Point Light Shadows denoiser {}: {}; Penumbra is used this session", a_mode == kPointRR ? "DLSS RR" : (a_mode == kPointOff ? "Off" : "Penumbra v2"),
			shader ? "its textures could not be created" : "PointDenoiseV2CS did not compile");
		return;
	}
	if (!pointVis[0].texture || !backend.PointVisibilitySRV() || !backend.PointSourcesSRV())
		return;
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	const std::uint32_t write = pointWrite, read = pointWrite ^ 1u;
	PointDenoiseCB cb{};
	cb.Params[0] = settings.PointLightsSize;
	cb.Params[1] = kPointHistoryWeight;  // (not read by PointDenoiseV2CS)
	const bool history = pointHistoryValid && prevCameraValid;
	std::uint32_t flags = (history ? kPointDenoiseHistory : 0u) | (plDebug ? kPointDenoiseDebug : 0u) | (motionSRV ? kPointDenoiseMotion : 0u);
	if (clean)  // (50e) only read by the PL_CLEAN twin
		flags |= kPointDenoiseCleanDetect | (cleanUsed ? kPointDenoiseClean : 0u);
	if (pointFractionalThisFrame)
		flags |= kPointDenoiseFractional;
	const bool wideHistory = wide && pointWideHistoryValid && history;
	flags |= (wide ? kPointDenoiseWide : 0u) | (wideHistory ? kPointDenoiseWideHistory : 0u);
	flags |= a_mode == kPointOff ? kV2Off : (rr ? kV2RR : 0u);
	const bool lenHistory = history && pointLenValid;
	flags |= lenHistory ? kV2LenHistory : 0u;
	std::memcpy(&cb.Params[2], &flags, sizeof(float));
	cb.Params[3] = kPointMaxFilterRadius;  // a converged slot's largest radius (young slots up to the shader's kYoungMaxRadius)
	cb.Extra[0] = static_cast<float>(pointK);
	{
		const std::uint32_t lo = static_cast<std::uint32_t>(pointBeamMask), hi = static_cast<std::uint32_t>(pointBeamMask >> 32);
		std::memcpy(&cb.Extra[1], &lo, sizeof(float));
		std::memcpy(&cb.Extra[2], &hi, sizeof(float));
		cb.Extra[3] = pointBeamMask ? pointBeamTan : 0.0f;
	}
	cb.Emitter[0] = static_cast<std::uint32_t>(pointEmitterMask);
	cb.Emitter[1] = static_cast<std::uint32_t>(pointEmitterMask >> 32);
	std::memcpy(&cb.Emitter[2], &pointEmitterRadius, sizeof(float));
	cb.Emitter[3] = frameIndex;  // the taps' rotation per frame
	{
		const auto& motion = pointSelector.SlotMotion();
		for (std::uint32_t s = 0; s < RayTracingPointLights::kMaxSources; ++s)
			cb.Motion[s / 4] |= static_cast<std::uint32_t>(motion[s]) << ((s % 4) * 8);
		const auto& fill = pointSelector.FillEmitterBytes();
		for (std::uint32_t s = 0; s < RayTracingPointLights::kMaxSources; ++s)
			cb.FillEmitter[s / 4] |= static_cast<std::uint32_t>(fill[s]) << ((s % 4) * 8);
	}
	pointDenoiseCB->Update(cb);

	timers->Begin(Util::GpuBucket::RayTracingPointDenoise);
	ID3D11Buffer* cbs[2] = { combineCB->CB(), pointDenoiseCB->CB() };
	ID3D11ShaderResourceView* srvs[10] = { backend.PointVisibilitySRV(), backend.PointSourcesSRV(), backend.DepthMaskSRV(), pointVis[read].srv.get(),
		pointIdx[read].srv.get(), pointDist[read].srv.get(), motionSRV, wide ? backend.PointWideSRV() : nullptr, wideHistory ? pointWide[read].srv.get() : nullptr,
		lenHistory ? pointLen[read].srv.get() : nullptr };
	ID3D11UnorderedAccessView* uavs[8] = { pointVis[write].uav.get(), pointIdx[write].uav.get(), pointDist[write].uav.get(), plDebug ? debugColorUAV.get() : nullptr,
		wide ? pointWide[write].uav.get() : nullptr, pointLen[write].uav.get(), rr ? pointRaw.uav.get() : nullptr, rr && wide ? pointRawWide.uav.get() : nullptr };
	context->CSSetConstantBuffers(0, 2, cbs);
	context->CSSetShaderResources(0, 10, srvs);
	context->CSSetUnorderedAccessViews(0, 8, uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch((pending.width + 7) / 8, (pending.height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[10]{};
	ID3D11UnorderedAccessView* nullUavs[8]{};
	ID3D11Buffer* nullCbs[2]{};
	context->CSSetShaderResources(0, 10, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 8, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 2, nullCbs);
	context->CSSetShader(nullptr, nullptr, 0);
	timers->End(Util::GpuBucket::RayTracingPointDenoise);

	// For the pixel shaders of this frame: the denoised slots, or (DLSS RR) the decoded rays (the effect draws swap in the filtered copy).
	const bool water = waterTraced && backend.PointWaterReady();
	ID3D11ShaderResourceView* views[4] = { rr ? pointRaw.srv.get() : pointVis[write].srv.get(), pointIdx[write].srv.get(),
		water ? backend.PointWaterVisSRV() : nullptr, water ? backend.PointWaterDistSRV() : nullptr };
	context->PSSetShaderResources(40, 4, views);
	ID3D11ShaderResourceView* wideView = wide ? (rr ? pointRawWide.srv.get() : pointWide[write].srv.get()) : nullptr;
	context->PSSetShaderResources(46, 1, &wideView);
	pointRRBound = rr;
	pointEffectSwapped = false;
	pointLenValid = true;
	pointModeThisFrame = a_mode;
	pointWideHistoryValid = wide;
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
	aoV2Valid = false;  // (47l)
	logger::warn("[RayTracing] Ray-traced ambient occlusion switched off: {}", aoOffReason);
}

void RayTracing::UpdateAO()
{
	// Turned on (again) by the user (or Retry): an earlier automatic switch-off is forgotten.
	if (settings.AO && !aoRequested) {
		backend.ClearAOFault();
		aoOffReason.clear();
		aoExtrasFault.clear();  // (46t)
		aoV2Fault.clear();      // (47l)
		aoSmoothFault.clear();  // (47n)
		aoAutoOff = false;
	}
	if (!ResolveAOTier().temporalV2)
		aoV2Fault.clear();  // (47l) turned off: turning it on again retries its shader / textures
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
	for (auto* t : { &aoLoDepth, &aoLoNormal, &aoFiltered, &aoHistory[0], &aoHistory[1], &aoOut })
		*t = {};
	aoWidth = aoHeight = 0;
	aoHistoryValid = false;
	d3d11Bytes -= std::min(d3d11Bytes, aoD3D11Bytes);
	aoD3D11Bytes = 0;
	ReleaseAOExtras();  // (46t)
	ReleaseAOTemporalV2();  // (47l)
	ReleaseAOTiles();  // (47r)
	ReleaseSpecOcc();  // (49a)
	if (aoPrevDepth) {  // (47u)
		d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(aoPrevDepthDesc.Width) * aoPrevDepthDesc.Height * 4);
		aoPrevDepth = nullptr;
		aoPrevDepthSRV = nullptr;
	}
	aoPrevDepthFrame = UINT32_MAX;
	aoNormalDeferred.active = false;
}

void RayTracing::ReleaseAOTemporalV2()
{
	for (auto* t : { &aoV2Length[0], &aoV2Length[1], &aoV2Geometry[0], &aoV2Geometry[1] })
		*t = {};
	aoV2Width = aoV2Height = 0;
	aoV2Valid = false;
	d3d11Bytes -= std::min(d3d11Bytes, aoV2Bytes);
	aoV2Bytes = 0;
}

bool RayTracing::EnsureAOTemporalV2()
{
	// (47l) Optional: on failure this session keeps the 47k history (aoV2Fault), RTAO itself goes on.
	if (!aoV2Fault.empty())
		return false;
	const std::uint32_t width = aoWidth, height = aoHeight;
	if (!width || !height)
		return false;
	if (aoV2Length[0].texture && aoV2Length[1].texture && aoV2Geometry[0].texture && aoV2Geometry[1].texture && aoV2Width == width && aoV2Height == height)
		return true;
	ReleaseAOTemporalV2();
	DenoiseTexture length0, length1, geometry0, geometry1;
	const bool ok = CreateDenoiseTexture(this, length0.texture, length0.srv, length0.uav, width, height, DXGI_FORMAT_R8G8_UNORM, "RayTracing RTAO History Length") &&
	                CreateDenoiseTexture(this, length1.texture, length1.srv, length1.uav, width, height, DXGI_FORMAT_R8G8_UNORM, "RayTracing RTAO History Length") &&
	                CreateDenoiseTexture(this, geometry0.texture, geometry0.srv, geometry0.uav, width, height, DXGI_FORMAT_R32_UINT, "RayTracing RTAO Geometry") &&
	                CreateDenoiseTexture(this, geometry1.texture, geometry1.srv, geometry1.uav, width, height, DXGI_FORMAT_R32_UINT, "RayTracing RTAO Geometry");
	if (!ok) {
		aoV2Fault = "could not create its history-length / geometry textures (the 47k history is used)";
		logger::warn("[RayTracing] RTAO: Temporal v2: {}", aoV2Fault);
		return false;
	}
	aoV2Length[0] = std::move(length0);
	aoV2Length[1] = std::move(length1);
	aoV2Geometry[0] = std::move(geometry0);
	aoV2Geometry[1] = std::move(geometry1);
	aoV2Width = width;
	aoV2Height = height;
	aoV2Valid = false;
	aoV2Bytes = static_cast<std::uint64_t>(width) * height * (2 + 4) * 2;
	d3d11Bytes += aoV2Bytes;
	return true;
}

void RayTracing::ReleaseAOExtras()
{
	for (auto* t : { &aoLoW, &aoLoWBlur, &aoFullTemp, &aoDetailFiltered, &aoDetailHistory[0], &aoDetailHistory[1] })
		*t = {};
	aoExtrasWidth = aoExtrasHeight = 0;
	aoDetailHistoryValid = false;
	d3d11Bytes -= std::min(d3d11Bytes, aoExtrasBytes);
	aoExtrasBytes = 0;
}

void RayTracing::ReleaseAOTiles()
{
	// (47r)
	for (auto* t : { &aoTileMark, &aoTileH, &aoTileDist })
		*t = {};
	aoTilesWidth = aoTilesHeight = 0;
	d3d11Bytes -= std::min(d3d11Bytes, aoTilesBytes);
	aoTilesBytes = 0;
}

bool RayTracing::EnsureAOTiles()
{
	// (47r) RTAO: Filter Only Where Traced: three R8_UINT textures of one texel per 8x8 tile (a few tens of KB). Failing them this session
	// the filter runs everywhere (47q).
	if (!aoTilesFault.empty())
		return false;
	const std::uint32_t tw = (aoWidth + 7) / 8, th = (aoHeight + 7) / 8;
	if (!tw || !th)
		return false;
	if (aoTileDist.texture && aoTilesWidth == tw && aoTilesHeight == th)
		return true;
	ReleaseAOTiles();
	DenoiseTexture mark, h, dist;
	bool ok = CreateDenoiseTexture(this, mark.texture, mark.srv, mark.uav, tw, th, DXGI_FORMAT_R8_UINT, "RayTracing RTAO Tile Mark") &&
	          CreateDenoiseTexture(this, h.texture, h.srv, h.uav, tw, th, DXGI_FORMAT_R8_UINT, "RayTracing RTAO Tile Distance H") &&
	          CreateDenoiseTexture(this, dist.texture, dist.srv, dist.uav, tw, th, DXGI_FORMAT_R8_UINT, "RayTracing RTAO Tile Distance");
	if (!ok) {
		aoTilesFault = "could not create its tile textures (the filter runs everywhere, as 47q)";
		logger::warn("[RayTracing] RTAO: Filter Only Where Traced: {}", aoTilesFault);
		return false;
	}
	aoTileMark = std::move(mark);
	aoTileH = std::move(h);
	aoTileDist = std::move(dist);
	aoTilesWidth = tw;
	aoTilesHeight = th;
	aoTilesBytes = static_cast<std::uint64_t>(tw) * th * 3;
	d3d11Bytes += aoTilesBytes;
	return true;
}

ID3D11ShaderResourceView* RayTracing::MakeSSSCasterTiles()
{
	// (47r) Screen Space Shadows: Tile Mask for RT Casters. One texel per 8x8 tile of the whole visibility texture (the contact-shadow pass
	// reads the mask by its own size); failing the texture or the shader this session: null (the per-pixel mask, 47q).
	if (!sssCasterTilesFault.empty())
		return nullptr;
	auto* visibility = backend.VisibilitySRV();
	if (!visibility)
		return nullptr;
	winrt::com_ptr<ID3D11Resource> resource;
	visibility->GetResource(resource.put());
	auto texture = resource.try_as<ID3D11Texture2D>();
	if (!texture)
		return nullptr;
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	const std::uint32_t tw = (desc.Width + 7) / 8, th = (desc.Height + 7) / 8;
	if (!sssCasterTiles.texture || sssCasterTilesWidth != tw || sssCasterTilesHeight != th) {
		if (sssCasterTiles.texture)
			d3d11Bytes -= std::min<std::uint64_t>(d3d11Bytes, static_cast<std::uint64_t>(sssCasterTilesWidth) * sssCasterTilesHeight);
		sssCasterTiles = {};
		DenoiseTexture tiles;
		if (!CreateDenoiseTexture(this, tiles.texture, tiles.srv, tiles.uav, tw, th, DXGI_FORMAT_R8_UINT, "RayTracing SSS Caster Tiles")) {
			sssCasterTilesFault = "could not create its texture";
			logger::warn("[RayTracing] Screen Space Shadows: Tile Mask for RT Casters: {} (the per-pixel mask is used)", sssCasterTilesFault);
			return nullptr;
		}
		sssCasterTiles = std::move(tiles);
		sssCasterTilesWidth = tw;
		sssCasterTilesHeight = th;
		d3d11Bytes += static_cast<std::uint64_t>(tw) * th;
	}
	auto* shader = GetShaderDefined(sssCasterTilesCS, L"Data\\Shaders\\RayTracing\\DenoiseTileCS.hlsl", "DenoiseTileCS (SSS caster tiles)", "SSS_CASTER_TILES");
	if (!shader) {
		sssCasterTilesFault = "its shader did not compile";
		logger::warn("[RayTracing] Screen Space Shadows: Tile Mask for RT Casters: {} (the per-pixel mask is used)", sssCasterTilesFault);
		return nullptr;
	}
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::ScreenSpaceShadows);
	ID3D11UnorderedAccessView* uav = sssCasterTiles.uav.get();
	context->CSSetShaderResources(0, 1, &visibility);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch(tw, th, 1);
	ID3D11ShaderResourceView* nullSrv = nullptr;
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetShaderResources(0, 1, &nullSrv);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	context->CSSetShader(nullptr, nullptr, 0);
	timers->End(Util::GpuBucket::ScreenSpaceShadows);
	return sssCasterTiles.srv.get();
}

bool RayTracing::EnsureAOExtras(bool a_quarter, bool a_full, bool a_detail)
{
	// (46t) Optional: on failure this session keeps the 46s filter (aoExtrasFault), RTAO itself goes on. Each group of
	// textures is created into locals and kept only when the whole group succeeded.
	if (!aoExtrasFault.empty())
		return false;
	const std::uint32_t width = aoWidth, height = aoHeight;
	if (!width || !height)
		return false;
	if (aoExtrasWidth != width || aoExtrasHeight != height) {
		ReleaseAOExtras();
		aoExtrasWidth = width;
		aoExtrasHeight = height;
	}
	const std::uint32_t lw = (width + 1) / 2, lh = (height + 1) / 2;
	std::uint64_t bytes = 0;
	bool ok = true;
	auto create = [&](DenoiseTexture& o_texture, std::uint32_t a_w, std::uint32_t a_h, DXGI_FORMAT a_format, std::uint32_t a_bytesPerPixel, const char* a_name) {
		if (!ok || !CreateDenoiseTexture(this, o_texture.texture, o_texture.srv, o_texture.uav, a_w, a_h, a_format, a_name)) {
			ok = false;
			return;
		}
		bytes += static_cast<std::uint64_t>(a_w) * a_h * a_bytesPerPixel;
	};
	if (a_quarter && !aoLoW.texture) {
		DenoiseTexture lo, loBlur;
		create(lo, lw, lh, DXGI_FORMAT_R16G16_UNORM, 4, "RayTracing RTAO LoW");
		create(loBlur, lw, lh, DXGI_FORMAT_R16G16_UNORM, 4, "RayTracing RTAO LoWBlur");
		if (ok) {
			aoLoW = std::move(lo);
			aoLoWBlur = std::move(loBlur);
		}
	}
	if (ok && a_full && !aoFullTemp.texture) {
		DenoiseTexture temp;
		create(temp, width, height, DXGI_FORMAT_R16G16_UNORM, 4, "RayTracing RTAO FullTemp");
		if (ok)
			aoFullTemp = std::move(temp);
	}
	if (ok && a_detail && !aoDetailFiltered.texture) {
		DenoiseTexture filtered, history0, history1;
		create(filtered, width, height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO Detail");
		create(history0, width, height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO Detail History");
		create(history1, width, height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO Detail History");
		if (ok) {
			aoDetailFiltered = std::move(filtered);
			aoDetailHistory[0] = std::move(history0);
			aoDetailHistory[1] = std::move(history1);
			aoDetailHistoryValid = false;
		}
	}
	aoExtrasBytes += bytes;
	d3d11Bytes += bytes;
	if (!ok) {
		ReleaseAOExtras();
		aoExtrasFault = "could not create the textures of its detail / full-resolution filter (the 46s filter is used)";
		logger::warn("[RayTracing] RTAO: {}", aoExtrasFault);
		return false;
	}
	return true;
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
	const bool ok = create(aoLoDepth, lw, lh, DXGI_FORMAT_R32_FLOAT, 4, "RayTracing RTAO LoDepth") &&
	                create(aoLoNormal, lw, lh, DXGI_FORMAT_R8G8B8A8_SNORM, 4, "RayTracing RTAO LoNormal") &&
	                create(aoFiltered, a_width, a_height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO Filtered") &&
	                create(aoHistory[0], a_width, a_height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO History") &&
	                create(aoHistory[1], a_width, a_height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO History") &&
	                create(aoOut, a_width, a_height, DXGI_FORMAT_R16_UNORM, 2, "RayTracing RTAO Out");
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
	const float rayLength = std::clamp(settings.AORayLength, kAORayLengthMin, kAORayLengthMax);
	// (46t) Two scales: the near / far split at the Detail Ray Length (at most half the Ray Length, so the far part keeps a
	// range of its own). Only when its textures exist (else this session stays with the 46s single ray and filter).
	// (47n) Fine Detail (two scales) always: without its textures there is no RTAO (the single-ray path is gone).
	const AOTierParams tier = ResolveAOTier();
	AOTraceShape shape{};
	shape.alphaBudget = std::clamp(settings.AOAlphaBudget, 0, 2);
	shape.detail = EnsureAOExtras(!tier.fullResFilter, tier.fullResFilter, true);
	if (!shape.detail) {
		SwitchOffAO(aoExtrasFault.empty() ? std::string("could not create its Fine Detail textures") : aoExtrasFault);
		return false;
	}
	shape.detailRays = std::clamp(tier.detailRays, 1, kAODetailRaysMax);
	shape.detailLength = std::min(std::clamp(settings.AODetailLength, kAODetailLengthMin, kAODetailLengthMax), rayLength * 0.5f);
	// (46y) Single Long Ray (always, 47n) / Half-Resolution Broad Term (a tier knob: the far part on a checkerboard; the filter's far
	// weight .b = 0 elsewhere).
	flags |= kAOTraceDetail | kAOTraceSingleRay;
	if (tier.broadRate == 1)
		flags |= kAOTraceHalfFar;
	else if (tier.broadRate == 2)
		flags |= kAOTraceQuarterFar;  // (47q) one pixel of each 2x2, rotating over 4 frames
	flags |= shape.alphaBudget == 1 ? kAOTraceAlphaHalf : (shape.alphaBudget == 2 ? kAOTraceAlphaOpen : 0u);
	// (47h) The trace output fell back to RGBA8: dither its weight. Character Self-Occlusion Fix (always, 47n; only with characters in
	// the TLAS).
	if (backend.AOIs8Bit())
		flags |= kAOTraceDither8;
	if (flags & kAOTraceCharacters)
		flags |= kAOTraceSelfFix;
	// (47n) RTAO: Tight History Near Moving Things: the trace marks pixels whose rays hit a character or a moving object (.b).
	flags |= kAOTraceDynamicFlag;  // (48o) always on
	// (49a) RT Specular Occlusion: the trace also writes the bent normal (its shared texture at render size). Off (or failed): the
	// texture goes, the trace writes nothing there (null UAV) and the composite keeps the scalar specular AO.
	specOccTracedThisFrame = false;
	if (settings.AOSpecOcc && specOccFault.empty()) {
		if (backend.EnsureAOBentTexture(width, height))
			specOccTracedThisFrame = true;
		else {
			specOccFault = "could not create its shared bent-normal texture";
			logger::warn("[RayTracing] RT Specular Occlusion: {}", specOccFault);
		}
	} else if (backend.AOBentSRV()) {
		backend.ReleaseAOBentTexture();
	}
	if (specOccTracedThisFrame)
		flags |= kAOTraceBent;
	aoShapeThisFrame = shape;
	c.params[0] = static_cast<float>(frameIndex % 1024);
	std::memcpy(&c.params[1], &flags, sizeof(float));
	c.params[2] = rayLength;
	c.params[3] = 0.0f;
	const float distance = std::clamp(tier.distance, 1000.0f, 8192.0f);
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
	c.extra[1] = shape.detail ? shape.detailLength : 0.0f;                         // (46t)
	c.extra[2] = shape.detail ? static_cast<float>(shape.detailRays - 1) : 0.0f;  // (46t) near-only directions besides the long one
	c.extra[3] = 0.0f;
	std::copy_n(a_input.constants.spheres, 8, c.spheres);
	// Leaves / cut-outs the per-kind menu traces block AO rays with their cut-out (bounded candidates per ray).
	aoInput.alpha = backend.AOAlphaAvailable();
	a_input.ao = &aoInput;
	return true;
}

// ---------------------------------------------------------------------------------------------
// (48a) RT reflections step 1 (trace only)
// ---------------------------------------------------------------------------------------------

bool RayTracing::ReflectionsWanted() const
{
	// (48d) Trace Only (1) or Gap Fill (2). Gap Fill without a consumer (SSRT specular off, its cubemap fallback off, no Dynamic Cubemaps)
	// traces nothing: the textures and terrain go as with Off.
	if (settings.ReflectionsMode == 2 && !GapFillConsumer())
		return false;
	return (settings.ReflectionsMode == 1 || settings.ReflectionsMode == 2) && backend.ReflectAvailable() && reflectOffReason.empty();
}

bool RayTracing::MaterialsWanted() const
{
	// (48f) Whenever RT Reflections is not Off (so Trace Only <-> Gap Fill never re-captures), with the switch on and the RT_MATERIAL shaders
	// accepted by the driver. Off: nothing of it (48e).
	return settings.ReflectionsMode != 0 && settings.ReflectionsMaterials && backend.IsReady() && backend.MaterialTraceAvailable() && reflectOffReason.empty();
}

void RayTracing::RunMaterialCopies(bool a_transition)
{
	// (48f) D3D11, on the render thread, never while the loading screen is presented (it owns the immediate context from another thread): finished
	// pages, new layers, the texture copies within a per-frame budget (larger in the black screen / warm-up after a loading screen, when nothing
	// or little is visible), the material table. The copy shader is compiled in the background; until then only the bookkeeping runs.
	if (!backend.IsReady() || !backend.MaterialsEnabled() || inLoadingPump)
		return;
	if (!materialCopyCS && !materialCopyFailed) {
		bool failed = false;
		PollAsyncShader(materialCopyAsync, materialCopyCS, L"Data\\Shaders\\RayTracing\\MaterialCopyCS.hlsl", {}, "Ray Tracing MaterialCopyCS", failed);
		if (failed) {
			materialCopyFailed = true;
			logger::error("[RayTracing] Reflection materials: MaterialCopyCS did not compile; textures stay uncopied (RT Albedo shows them as pending)");
		}
	}
	const std::uint32_t copies = a_transition ? kMaterialCopiesTransition : kMaterialCopiesPerFrame;
	const float cpuMs = a_transition ? kMaterialCopyCpuMsTransition : kMaterialCopyCpuMs;
	backend.UpdateMaterials(materialCopyCS.get(), copies, cpuMs, globals::state->frameCount, a_transition);
}

bool RayTracing::GapFillConsumer()
{
	const auto& ssrt = globals::features::screenSpaceRayTracing;
	const auto& cubemaps = globals::features::dynamicCubemaps;
	return ssrt.loaded && ssrt.settings.EnableSpecular && ssrt.settings.UseDynamicCubemapsAsFallbackSpecular && cubemaps.loaded && !ssrt.rtGapFillFailed;
}

float RayTracing::ReflectionDistanceLimit()
{
	// (48d) The SSR distance limit along the view axis (units; 0 = none): past it the composite never reads the specular result.
	const auto& ssrt = globals::features::screenSpaceRayTracing;
	if (!ssrt.loaded || !ssrt.settings.EnableSpecular || !ssrt.DistanceLimitOnChain(true))
		return 0.0f;
	return ssrt.DistanceLimitUnits();
}

void RayTracing::PrepareReflections(RayTracingRT::FrameInput& a_input)
{
	// (48d) In every trace path, after RTAO (their own list and fence). The rays go out only when the normal prep also writes their input
	// (PrepareDepthMask).
	reflectWantedThisFrame = false;
	a_input.reflectTags = false;
	a_input.materials = false;  // (48f)
	reflectSunRayThisFrame = false;  // (48g)
	try {
		PrepareReflectionsImpl(a_input);
	} catch (const std::exception& e) {
		reflectWantedThisFrame = false;
		a_input.reflectTags = false;
		a_input.materials = false;
		reflectOffReason = std::format("exception: {}", e.what());
		logger::warn("[RayTracing] RT reflections switched off: {}", reflectOffReason);
	}
}

void RayTracing::PrepareReflectionsImpl(RayTracingRT::FrameInput& a_input)
{
	if (!ReflectionsWanted())
		return;
	const std::uint32_t width = std::max(a_input.renderWidth, 1u), height = std::max(a_input.renderHeight, 1u);
	if (!backend.EnsureReflectTextures(width, height)) {
		if (!backend.ReflectFault().empty()) {
			reflectOffReason = backend.ReflectFault();
			logger::warn("[RayTracing] RT reflections switched off: {}", reflectOffReason);
		}
		return;
	}
	auto& c = reflectInput.constants;
	std::memcpy(c.invViewProj, a_input.constants.invViewProj, sizeof(c.invViewProj));
	std::copy_n(a_input.constants.renderSize, 4, c.renderSize);
	std::uint32_t flags = 0;
	if (!skinDraws.empty()) {
		flags |= kReflectTraceCharacters;
		if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); playerCamera && playerCamera->IsInFirstPerson())
			flags |= kReflectTraceFirstPerson;
	}
	if (scene.Terrain() && scene.GetStats().terrainBuilt > 0)
		flags |= kReflectTraceTerrain;
	if (settings.ReflectionsRoughSolid)
		flags |= kReflectTraceRoughSolid;  // (48d)
	c.params[0] = static_cast<float>(frameIndex % 1024);
	std::memcpy(&c.params[1], &flags, sizeof(float));
	c.params[2] = kReflectMaxRayLength;
	c.params[3] = 0.0f;
	c.bias[0] = kRayBiasConstant;
	c.bias[1] = kRayBiasPerUnit;
	c.bias[2] = kCharacterSelfShadowOffset;
	c.bias[3] = 0.0f;
	std::copy_n(a_input.constants.spheres, 8, c.spheres);
	reflectInput.alpha = backend.AOAlphaAvailable();
	a_input.reflectTags = true;
	// (48f) Capture Materials: the surface record (sized like the hit record) and the per-instance table with this frame's TLAS.
	a_input.materials = MaterialsWanted() && backend.MaterialsEnabled() && backend.EnsureReflectSurfaceTextures(width, height);
	// (48g) The sun visibility ray of the material variant (sun path only: interiors and the sun-off path have no sun trace), and the shading
	// pass's output, made with the surface record (enable time / a resize).
	const float* toSun = a_input.constants.toSun;
	const float sunLength = std::sqrt(toSun[0] * toSun[0] + toSun[1] * toSun[1] + toSun[2] * toSun[2]);
	const bool sunRay = a_input.materials && a_input.sun && settings.ReflectionsSunRay && sunLength > 0.5f;
	for (int a = 0; a < 3; ++a)
		c.sun[a] = sunLength > 1e-6f ? toSun[a] / sunLength : 0.0f;
	c.sun[3] = sunRay ? 1.0f : 0.0f;
	reflectSunRayThisFrame = sunRay;
	if (a_input.materials && ShadeWanted())
		EnsureShadeResources(width, height);
	reflectWantedThisFrame = true;
}

bool RayTracing::PollAsyncShader(Util::AsyncComputeShader& a_async, winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path,
	const std::vector<std::pair<std::string, std::string>>& a_defines, std::string_view a_label, bool& a_failed)
{
	// (48e) Never blocks (Request is a no-op while the job exists).
	a_failed = false;
	a_async.Request(a_path, a_defines, a_label);
	switch (a_async.GetStatus()) {
	case Util::AsyncComputeShader::Status::Ready:
		a_shader.copy_from(a_async.Poll());
		return a_shader != nullptr;
	case Util::AsyncComputeShader::Status::Failed:
		a_failed = true;
		return false;
	default:
		return false;
	}
}

void RayTracing::PrewarmReflectionShaders()
{
	// (48e) Started as soon as RT Reflections is not Off (main menu, loading screens, play): the first frame that needs them finds them
	// ready instead of compiling them on the render thread. Each request is a no-op once its job exists.
	if (!RtWanted() || settings.ReflectionsMode == 0)
		return;
	if (!aoNormalReflectCS && reflectOffReason.empty()) {
		bool failed = false;
		PollAsyncShader(aoNormalReflectAsync, aoNormalReflectCS, L"Data\\Shaders\\RayTracing\\AONormalPrepCS.hlsl", { { "RT_REFLECT", "" } },
			"Ray Tracing AONormalPrepCS (RT_REFLECT)", failed);
	}
	if (settings.ReflectionsMode == 2)
		globals::features::screenSpaceRayTracing.PrewarmRTGapFill();
	// (48g) The shading pass (Gap Fill + Capture Materials + Off-screen Hit Color: Material), in the background as well.
	if (settings.ReflectionsMode == 2 && settings.ReflectionsMaterials && settings.ReflectionsOffscreenColor == 1 && !reflectShadeCS && !reflectShadeFailed) {
		bool failed = false;
		PollAsyncShader(reflectShadeAsync, reflectShadeCS, L"Data\\Shaders\\RayTracing\\ReflectShadeCS.hlsl", {}, "Ray Tracing ReflectShadeCS", failed);
		if (failed)
			reflectShadeFailed = true;
	}
}

void RayTracing::DrawReflectionDebugViews()
{
	// (48d) Right before the SSRT specular pass: the reflection list's wait (third fence), then the hit type / distance / confidence view.
	// The "RT Reflection Use" view is written by the ray march itself (AcquireReflectionGapFill).
	if (!reflectThisFrame || reflectDebugView == ReflectDebugView::Off || reflectDebugView == ReflectDebugView::Use || reflectDebugView == ReflectDebugView::ShadeVsScreen)
		return;
	try {
		auto* timers = Util::GpuPassTimers::GetSingleton();
		timers->Begin(Util::GpuBucket::RayTracingReflectSync);
		backend.WaitForReflections();
		timers->End(Util::GpuBucket::RayTracingReflectSync);
		DrawReflectionDebug();
	} catch (const std::exception& e) {
		logger::warn("[RayTracing] Reflection debug view failed: {}", e.what());
		reflectDebugView = ReflectDebugView::Off;
	}
}

bool RayTracing::AcquireReflectionGapFill(std::uint32_t a_width, std::uint32_t a_height, ReflectionGapFill& o_fill, ID3D11ShaderResourceView* a_screenColor,
	ID3D11ShaderResourceView* a_depth)
{
	// (48d) Only this frame's record, at the ray march's size; anything else (not traced, first frame after a resize / load, Trace Only,
	// Off, a failure) leaves the ray march on its old path.
	o_fill = {};
	if (settings.ReflectionsMode != 2 || !reflectThisFrame || !RtWanted() || !backend.ReflectDirSRV() || !backend.ReflectOutSRV())
		return false;
	if (backend.ReflectWidth() != a_width || backend.ReflectHeight() != a_height)
		return false;
	if (static_cast<std::uint32_t>(reflectInput.constants.renderSize[0]) != a_width || static_cast<std::uint32_t>(reflectInput.constants.renderSize[1]) != a_height)
		return false;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::RayTracingReflectSync);
	backend.WaitForReflections();  // no-op when a debug view already took it
	timers->End(Util::GpuBucket::RayTracingReflectSync);
	if (backend.HasFailed())
		return false;
	o_fill.hit = backend.ReflectOutSRV();
	o_fill.dir = backend.ReflectDirSRV();
	if (reflectDebugView == ReflectDebugView::Use && EnsureDebugColor() && debugColorUAV) {
		o_fill.debug = debugColorUAV.get();
		o_fill.flags |= kGapFillDebug;
		reflectDebugDrawn = true;
	}
	if (settings.ReflectionsBeyondSSR)
		o_fill.flags |= kGapFillBeyond;
	if (settings.ReflectionsLobeCheck)
		o_fill.flags |= kGapFillLobe;
	if (settings.ReflectionsCubemapMatch)
		o_fill.flags |= kGapFillMatch;
	if (settings.ReflectionsHitDistance)
		o_fill.flags |= kGapFillHitDistance;
	if (settings.ReflectionsCubemapWarmUp)
		o_fill.flags |= kGapFillWarmUp;  // (48e)
	// (48g) Off-screen Hit Color: the switch fades over ~0.4 s (no pop either way); the shading pass runs while it is above 0 (or for the
	// "RT Shading vs Screen" view) and the ray march blends the shaded colours in by their availability x this fade.
	{
		const std::int64_t now = Qpc();
		const double dtMs = shadeFadeQpc != 0 ? std::clamp(static_cast<double>(now - shadeFadeQpc) * QpcMs(), 0.0, 100.0) : 0.0;
		shadeFadeQpc = now;
		const float target = settings.ReflectionsOffscreenColor == 1 && MaterialsWanted() ? 1.0f : 0.0f;
		const float step = static_cast<float>(dtMs / kShadeFadeMs);
		shadeFade = target > shadeFade ? std::min(target, shadeFade + step) : std::max(target, shadeFade - step);
	}
	if (ShadeWanted() && backend.ReflectMaterialLastFrame() && ShadeReflections(a_width, a_height, a_screenColor, a_depth)) {
		o_fill.shade = shadeSRV.get();
		o_fill.shadeScale = std::clamp(settings.ReflectionsOffscreenBrightness, 0.25f, 4.0f);
		if (shadeFade > 0.0f) {
			o_fill.flags |= kGapFillMaterial;
			if (settings.ReflectionsPreferBrightCubemap)
				o_fill.flags |= kGapFillPreferBright;  // (48h)
		}
	}
	reflectGapFillUsed = true;
	return true;
}

bool RayTracing::ShadeWanted() const
{
	// (48g) Gap Fill with materials, and the Material colour selected (or still fading out), or the comparison view.
	if (settings.ReflectionsMode != 2 || !MaterialsWanted() || reflectShadeFailed)
		return false;
	return settings.ReflectionsOffscreenColor == 1 || shadeFade > 0.0f || reflectDebugView == ReflectDebugView::ShadeVsScreen;
}

bool RayTracing::EnsureShadeResources(std::uint32_t a_width, std::uint32_t a_height)
{
	// (48g) RGBA16F at render size (1440p 28 MB) + a 32-byte stats buffer and its staging ring: made with the surface record (enable time / a
	// resize), on the render thread; never per frame.
	if (shadeTexture && shadeWidth == a_width && shadeHeight == a_height)
		return true;
	ReleaseShadeResources();
	auto* device = globals::d3d::device;
	if (!device || a_width == 0 || a_height == 0)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = a_width;
	desc.Height = a_height;
	desc.MipLevels = 1;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
	HRESULT hr = device->CreateTexture2D(&desc, nullptr, shadeTexture.put());
	if (SUCCEEDED(hr))
		hr = device->CreateShaderResourceView(shadeTexture.get(), nullptr, shadeSRV.put());
	if (SUCCEEDED(hr))
		hr = device->CreateUnorderedAccessView(shadeTexture.get(), nullptr, shadeUAV.put());
	if (SUCCEEDED(hr)) {
		D3D11_BUFFER_DESC statsDesc{};
		statsDesc.ByteWidth = 32;
		statsDesc.Usage = D3D11_USAGE_DEFAULT;
		statsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
		statsDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
		hr = device->CreateBuffer(&statsDesc, nullptr, shadeStats.put());
		if (SUCCEEDED(hr)) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
			uavDesc.Buffer.NumElements = 8;
			uavDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
			hr = device->CreateUnorderedAccessView(shadeStats.get(), &uavDesc, shadeStatsUAV.put());
		}
		D3D11_BUFFER_DESC stagingDesc{};
		stagingDesc.ByteWidth = 32;
		stagingDesc.Usage = D3D11_USAGE_STAGING;
		stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		for (auto& staging : shadeStatsStaging)
			if (SUCCEEDED(hr))
				hr = device->CreateBuffer(&stagingDesc, nullptr, staging.put());
	}
	if (FAILED(hr)) {
		ReleaseShadeResources();
		reflectShadeFailed = true;
		logger::warn("[RayTracing] Reflection shading: could not create its textures (0x{:08X}); off-screen hits keep the cubemap colour",
			static_cast<std::uint32_t>(hr));
		return false;
	}
	Util::SetResourceName(shadeTexture.get(), "RayTracing ReflectShade");
	shadeWidth = a_width;
	shadeHeight = a_height;
	shadeStatsFrame.fill(0);
	logger::info("[RayTracing] Reflection shading: output {}x{} ({:.1f} MB)", a_width, a_height, static_cast<double>(a_width) * a_height * 8.0 / (1024.0 * 1024.0));
	return true;
}

void RayTracing::ReleaseShadeResources()
{
	shadeUAV = nullptr;
	shadeSRV = nullptr;
	shadeTexture = nullptr;
	shadeStatsUAV = nullptr;
	shadeStats = nullptr;
	for (auto& staging : shadeStatsStaging)
		staging = nullptr;
	shadeStatsFrame.fill(0);
	shadeWidth = shadeHeight = 0;
	shadeThisFrame = false;
}

bool RayTracing::ShadeReflections(std::uint32_t a_width, std::uint32_t a_height, ID3D11ShaderResourceView* a_screenColor, ID3D11ShaderResourceView* a_depth)
{
	// (48g) After the reflection list's wait (AcquireReflectionGapFill), right before the SSRT specular ray march. Never blocks: the shader
	// compiles in the background (until then the ray march keeps the cubemap path); the stats are read back a few frames late.
	shadeThisFrame = false;
	if (!a_depth || !backend.ReflectSurf0SRV() || !backend.ReflectSurf1SRV() || !backend.MaterialTableSRV() || !EnsureShadeResources(a_width, a_height))
		return false;
	if (!reflectShadeCS) {
		bool failed = false;
		if (!PollAsyncShader(reflectShadeAsync, reflectShadeCS, L"Data\\Shaders\\RayTracing\\ReflectShadeCS.hlsl", {}, "Ray Tracing ReflectShadeCS", failed)) {
			if (failed) {
				reflectShadeFailed = true;
				logger::error("[RayTracing] Reflection shading: ReflectShadeCS did not compile; off-screen hits keep the cubemap colour");
			}
			return false;
		}
	}
	ReadShadeStats();
	struct alignas(16) ReflectShadeConstants
	{
		std::uint32_t Size[4];
		float Params[4];
	};
	if (!reflectShadeCB)
		reflectShadeCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ReflectShadeConstants>());
	if (!reflectDebugSampler) {
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		globals::d3d::device->CreateSamplerState(&samplerDesc, reflectDebugSampler.put());
	}
	auto& skylighting = globals::features::skylighting;
	auto& llf = globals::features::lightLimitFix;
	const bool sky = skylighting.loaded && skylighting.texProbeArray && skylighting.texProbeArray->srv && skylighting.stbn_vec3_2Dx1D_128x128x64;
	const bool lamps = settings.ReflectionsShadeLights && llf.loaded && llf.lights && llf.lights->srv && llf.lightCount > 0;
	const bool debug = reflectDebugView == ReflectDebugView::ShadeVsScreen && EnsureDebugColor() && debugColorUAV;
	const bool stats = shadeStats && shadeStatsUAV;
	ReflectShadeConstants cb{};
	cb.Size[0] = a_width;
	cb.Size[1] = a_height;
	cb.Size[2] = globals::state->frameCount;
	cb.Size[3] = (sky ? 1u : 0u) | (lamps ? 2u : 0u) | (debug && a_screenColor ? 4u : 0u) | (stats && a_screenColor ? 8u : 0u) | (reflectSunRayThisFrame ? 16u : 0u) |
	              (settings.ReflectionsShadeGlow ? 32u : 0u);  // (48h) Material Hits: Glow
	cb.Params[0] = kShadeFadeInFrames;
	cb.Params[1] = std::clamp(shadeFade, 0.0f, 1.0f);
	cb.Params[2] = lamps ? static_cast<float>(llf.lightCount) : 0.0f;
	reflectShadeCB->Update(cb);
	auto* context = globals::d3d::context;
	auto* timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::RayTracingReflectShade);
	if (stats) {
		const UINT zero[4] = { 0, 0, 0, 0 };
		context->ClearUnorderedAccessViewUint(shadeStatsUAV.get(), zero);
	}
	ID3D11ShaderResourceView* srvs[16] = { backend.ReflectOutSRV(), backend.ReflectDirSRV(), backend.ReflectSurf0SRV(), backend.ReflectSurf1SRV(),
		backend.MaterialTableSRV() };
	for (std::uint32_t page = 0; page < RayTracingRT::kMaterialMaxPages; ++page)
		srvs[5 + page] = backend.MaterialPageSRV(page);
	srvs[11] = sky ? skylighting.texProbeArray->srv.get() : nullptr;
	srvs[12] = sky ? skylighting.stbn_vec3_2Dx1D_128x128x64.get() : nullptr;
	srvs[13] = lamps ? llf.lights->srv.get() : nullptr;
	srvs[14] = a_depth;
	srvs[15] = a_screenColor;
	ID3D11UnorderedAccessView* uavs[3] = { shadeUAV.get(), stats ? shadeStatsUAV.get() : nullptr, debug ? debugColorUAV.get() : nullptr };
	// The ray march's constant buffers (SharedData b5 / b6, FrameBuffer b12) stay; b0 and s0 are put back afterwards.
	winrt::com_ptr<ID3D11Buffer> previousCb;
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	context->CSGetConstantBuffers(0, 1, previousCb.put());
	context->CSGetSamplers(0, 1, previousSampler.put());
	ID3D11Buffer* cbs[1] = { reflectShadeCB->CB() };
	ID3D11SamplerState* sampler = reflectDebugSampler.get();
	context->CSSetConstantBuffers(0, 1, cbs);
	context->CSSetSamplers(0, 1, &sampler);
	context->CSSetShaderResources(0, 16, srvs);
	context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
	context->CSSetShader(reflectShadeCS.get(), nullptr, 0);
	context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[16]{};
	ID3D11UnorderedAccessView* nullUavs[3]{};
	context->CSSetShaderResources(0, 16, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	ID3D11Buffer* restoreCb = previousCb.get();
	ID3D11SamplerState* restoreSampler = previousSampler.get();
	context->CSSetConstantBuffers(0, 1, &restoreCb);
	context->CSSetSamplers(0, 1, &restoreSampler);
	context->CSSetShader(nullptr, nullptr, 0);
	if (stats) {
		const std::uint32_t slot = shadeStatsCursor % kShadeStatsRing;
		context->CopyResource(shadeStatsStaging[slot].get(), shadeStats.get());
		shadeStatsFrame[slot] = std::max(globals::state->frameCount, 1u);
		shadeStatsCursor = (shadeStatsCursor + 1) % kShadeStatsRing;
	}
	timers->End(Util::GpuBucket::RayTracingReflectShade);
	if (debug)
		reflectDebugDrawn = true;
	shadeThisFrame = true;
	return true;
}

void RayTracing::ReadShadeStats()
{
	// (48g) The oldest written staging copy that is at least two frames old, without waiting (DO_NOT_WAIT): the material share and the
	// brightness ratio for the panel / F7 / F12.
	auto* context = globals::d3d::context;
	const std::uint32_t frame = globals::state->frameCount;
	for (std::uint32_t i = 0; i < kShadeStatsRing; ++i) {
		const std::uint32_t slot = (shadeStatsCursor + i) % kShadeStatsRing;
		if (shadeStatsFrame[slot] == 0 || frame - shadeStatsFrame[slot] < 2 || !shadeStatsStaging[slot])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (context->Map(shadeStatsStaging[slot].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK || !mapped.pData)
			continue;
		std::uint32_t values[8]{};
		std::memcpy(values, mapped.pData, sizeof(values));
		context->Unmap(shadeStatsStaging[slot].get(), 0);
		shadeStatsFrame[slot] = 0;
		const double hits = static_cast<double>(values[0]) + static_cast<double>(values[1]);
		shadeMaterialShare = hits > 0.0 ? static_cast<float>(static_cast<double>(values[0]) / hits) : -1.0f;
		shadeRatioGroups = values[4];
		shadeRatioArith = values[4] > 0 && values[3] > 0 ? static_cast<float>(static_cast<double>(values[2]) / static_cast<double>(values[3])) : -1.0f;
		// (48h) The geometric mean (per-pixel log2 ratios, clamped to +-4 on the GPU; [5] signed x 32, [6] pixels), then smoothed over ~2 s of
		// time: a single frame's mean jumps with the changing, stochastic set of hits that are also on screen.
		shadeRatioPixels = values[6];
		const std::int64_t now = Qpc();
		if (values[6] > 0) {
			const double frameLog = static_cast<double>(static_cast<std::int32_t>(values[5])) / 32.0 / static_cast<double>(values[6]);
			shadeRatio = static_cast<float>(std::exp2(frameLog));
			const double dt = shadeSmoothQpc != 0 ? static_cast<double>(now - shadeSmoothQpc) * QpcMs() / 1000.0 : 1e9;
			if (!shadeSmoothValid || dt > kShadeSmoothResetS) {
				shadeSmoothLog = frameLog;
				shadeSmoothPixels = static_cast<double>(values[6]);
				shadeSmoothValid = true;
			} else {
				const double alpha = (1.0 - std::exp(-std::clamp(dt, 0.0, 0.5) / kShadeSmoothTauS)) *
				                     std::min(1.0, static_cast<double>(values[6]) / kShadeSmoothFullPixels);
				shadeSmoothLog += (frameLog - shadeSmoothLog) * alpha;
				shadeSmoothPixels += (static_cast<double>(values[6]) - shadeSmoothPixels) * (1.0 - std::exp(-std::clamp(dt, 0.0, 0.5) / kShadeSmoothTauS));
			}
			shadeSmoothQpc = now;
			shadeRatioSmoothed = static_cast<float>(std::exp2(shadeSmoothLog));
		} else {
			shadeRatio = -1.0f;
			if (shadeSmoothValid && shadeSmoothQpc != 0 && static_cast<double>(now - shadeSmoothQpc) * QpcMs() / 1000.0 > kShadeSmoothResetS) {
				shadeSmoothValid = false;  // nothing comparable for a while: the next sample starts afresh
				shadeRatioSmoothed = -1.0f;
			}
		}
		break;
	}
}

void RayTracing::DrawReflectionDebug()
{
	// (48d) After the reflection list's own wait: the hit type / distance / normal confidence view into debugColor.
	if (reflectDebugView == ReflectDebugView::Off || reflectDebugView == ReflectDebugView::Use || reflectDebugView == ReflectDebugView::ShadeVsScreen || !debugColorUAV ||
		!backend.ReflectOutSRV())
		return;
	if (!reflectDebugCS) {
		// (48e) In the background: the view simply stays undrawn until the shader is ready.
		bool failed = false;
		if (!PollAsyncShader(reflectDebugAsync, reflectDebugCS, L"Data\\Shaders\\RayTracing\\ReflectDebugCS.hlsl", {}, "Ray Tracing ReflectDebugCS", failed)) {
			if (failed) {
				logger::error("[RayTracing] Could not compile ReflectDebugCS");
				reflectDebugView = ReflectDebugView::Off;
			}
			return;
		}
	}
	struct alignas(16) ReflectDebugConstants
	{
		std::uint32_t Size[4];
		float Params[4];
	};
	if (!reflectDebugCB)
		reflectDebugCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ReflectDebugConstants>());
	const std::uint32_t width = static_cast<std::uint32_t>(reflectInput.constants.renderSize[0]);
	const std::uint32_t height = static_cast<std::uint32_t>(reflectInput.constants.renderSize[1]);
	if (width == 0 || height == 0)
		return;
	ReflectDebugConstants cb{};
	cb.Size[0] = width;
	cb.Size[1] = height;
	cb.Size[2] = static_cast<std::uint32_t>(reflectDebugView);
	cb.Params[0] = kReflectMaxRayLength;
	// (48f) RT Albedo: the surface record, the material table and the copy pages (t1 .. t9, s0); w = this frame's record is valid.
	const bool albedo = reflectDebugView == ReflectDebugView::Albedo;
	ID3D11ShaderResourceView* srvs[10] = { backend.ReflectOutSRV() };
	std::uint32_t srvCount = 1;
	if (albedo) {
		const bool record = backend.ReflectMaterialLastFrame() && backend.ReflectSurf0SRV() && backend.ReflectSurf1SRV() && backend.MaterialTableSRV();
		cb.Size[3] = record ? 1u : 0u;
		if (record) {
			srvs[1] = backend.ReflectSurf0SRV();
			srvs[2] = backend.ReflectSurf1SRV();
			srvs[3] = backend.MaterialTableSRV();
			for (std::uint32_t page = 0; page < RayTracingRT::kMaterialMaxPages; ++page)
				srvs[4 + page] = backend.MaterialPageSRV(page);
		}
		srvCount = 10;
		if (!reflectDebugSampler) {
			D3D11_SAMPLER_DESC samplerDesc{};
			samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
			samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
			globals::d3d::device->CreateSamplerState(&samplerDesc, reflectDebugSampler.put());
		}
	}
	reflectDebugCB->Update(cb);
	auto* context = globals::d3d::context;
	ID3D11Buffer* cbs[1] = { reflectDebugCB->CB() };
	ID3D11UnorderedAccessView* uavs[1] = { debugColorUAV.get() };
	winrt::com_ptr<ID3D11SamplerState> previousSampler;
	if (albedo) {
		context->CSGetSamplers(0, 1, previousSampler.put());
		ID3D11SamplerState* sampler = reflectDebugSampler.get();
		context->CSSetSamplers(0, 1, &sampler);
	}
	context->CSSetConstantBuffers(0, 1, cbs);
	context->CSSetShaderResources(0, srvCount, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(reflectDebugCS.get(), nullptr, 0);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[10]{};
	ID3D11UnorderedAccessView* nullUavs[1]{};
	ID3D11Buffer* nullCbs[1]{};
	context->CSSetShaderResources(0, srvCount, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, nullUavs, nullptr);
	context->CSSetConstantBuffers(0, 1, nullCbs);
	if (albedo) {
		ID3D11SamplerState* restore = previousSampler.get();
		context->CSSetSamplers(0, 1, &restore);
	}
	context->CSSetShader(nullptr, nullptr, 0);
	reflectDebugDrawn = true;
}

void RayTracing::DrawAmbientOcclusion()
{
	// (46t) A render hook: nothing may escape it. On an exception only RTAO goes off (SSGI's own AO is used).
	try {
		DrawAmbientOcclusionImpl();
	} catch (const std::exception& e) {
		SwitchOffAO(std::format("its filter failed ({})", e.what()));
	} catch (...) {
		SwitchOffAO("its filter failed (unknown error)");
	}
}

void RayTracing::DrawAmbientOcclusionImpl()
{
	if (!aoThisFrame)
		return;
	aoThisFrame = false;
	// (48d) The reflection debug views moved to DrawReflectionDebugViews (their own fence, right before the SSRT specular pass).
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

	auto* temporal = GetShader(aoTemporalCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS");
	auto* composite = GetShader(aoCompositeCS, L"Data\\Shaders\\RayTracing\\RTAOCompositeCS.hlsl", "RTAOCompositeCS");
	if (!temporal || !composite) {
		SwitchOffAO("a filter shader did not compile (see the log)");
		return;
	}

	auto* renderer = globals::game::renderer;
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto& normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kMOTION_VECTOR];
	if (!depth.depthSRV || !normal.SRV || !motion.SRV)
		return;

	// (46t) RTAOFilterCS: two scales, normal-guided weights (both always since 47n; the 46s SSRT-confidence filter path is gone),
	// quarter or full resolution (a tier knob).
	const AOTraceShape shape = aoShapeThisFrame;
	const AOTierParams aoTier = ResolveAOTier();
	const bool detail = shape.detail && aoDetailFiltered.texture && aoDetailHistory[0].texture && aoDetailHistory[1].texture;
	const bool fullRes = aoTier.fullResFilter && EnsureAOExtras(false, true, false) && aoFullTemp.texture;
	const bool quarterOwn = !fullRes && EnsureAOExtras(true, false, false) && aoLoW.texture && aoLoWBlur.texture;
	if (!detail || !(fullRes || quarterOwn)) {
		// The trace carried two scales (its .r is the far term only) but their filter textures are gone: no RTAO from now on.
		SwitchOffAO(aoExtrasFault.empty() ? std::string("its Fine Detail filter textures are missing") : aoExtrasFault);
		return;
	}

	ID3D11ComputeShader *down = nullptr, *blurH = nullptr, *blurV = nullptr, *up = nullptr, *fullH = nullptr, *fullV = nullptr;
	ID3D11ComputeShader *detailCS = nullptr, *detailTemporal = nullptr;
	const wchar_t* kFilter = L"Data\\Shaders\\RayTracing\\RTAOFilterCS.hlsl";
	if (fullRes) {
		fullH = GetShaderDefined(aoFFullHCS, kFilter, "RTAOFilterCS (full, horizontal)", "RTAO_FULL");
		fullV = GetShaderDefined(aoFFullVCS, kFilter, "RTAOFilterCS (full, vertical)", "RTAO_FULL,RTAO_VERTICAL");
	} else {
		down = GetShaderDefined(aoFDownCS, kFilter, "RTAOFilterCS (down)", "RTAO_DOWN");
		blurH = GetShaderDefined(aoFBlurHCS, kFilter, "RTAOFilterCS (blur)", "RTAO_BLUR");
		blurV = GetShaderDefined(aoFBlurVCS, kFilter, "RTAOFilterCS (blur, vertical)", "RTAO_BLUR,RTAO_VERTICAL");
		up = GetShaderDefined(aoFUpCS, kFilter, "RTAOFilterCS (up)", "RTAO_UP");
	}
	if (detail) {
		detailCS = GetShaderDefined(aoFDetailCS, kFilter, "RTAOFilterCS (detail)", "RTAO_DETAIL");
		detailTemporal = GetShaderDefined(aoDetailTemporalCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS (detail)", "RTAO_DETAIL_HISTORY");
	}
	if ((fullRes && (!fullH || !fullV)) || (!fullRes && (!down || !blurH || !blurV || !up)) || (detail && (!detailCS || !detailTemporal))) {
		SwitchOffAO("a filter shader did not compile (see the log)");
		return;
	}

	const std::uint32_t width = aoWidth, height = aoHeight;
	const UINT groupsX = (width + 7) / 8, groupsY = (height + 7) / 8;
	const UINT loX = ((width + 1) / 2 + 7) / 8, loY = ((height + 1) / 2 + 7) / 8;
	// (47l) RTAO: Temporal v2 (needs Temporal; its shader and textures on first use, failing them this session keeps the 47k
	// history). A change of the switch, a camera cut (moved more than kAOCutDistance or turned more than 60 degrees since the last
	// frame) or a gap of a frame resets every history; this frame's camera is kept for the next one.
	const std::uint32_t frameNow = globals::state->frameCount;
	const bool wantV2 = settings.AOTemporal && aoTier.temporalV2 && aoV2Fault.empty();
	ID3D11ComputeShader* temporalV2 = nullptr;
	if (wantV2) {
		temporalV2 = GetShaderDefined(aoTemporalV2CS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS (v2)", "RTAO_TEMPORAL_V2");
		if (!temporalV2) {
			aoV2Fault = "its shader did not compile (the 47k history is used)";
			logger::warn("[RayTracing] RTAO: Temporal v2: {}", aoV2Fault);
		}
	}
	const bool v2 = wantV2 && temporalV2 && EnsureAOTemporalV2();
	if (!v2 && aoV2Length[0].texture)
		ReleaseAOTemporalV2();
	if (v2 != aoV2Last) {
		aoHistoryValid = false;
		aoDetailHistoryValid = false;
		aoV2Valid = false;
		aoV2Last = v2;
	}
	const auto& fbAO = globals::game::frameBufferCached;
	const float4 cameraPos = fbAO.GetCameraPosAdjust(0);
	float forward[3]{};
	{
		// The view direction: two points on the centre ray (the matrices as the shaders read them: row r = floats r * 4 .. r * 4 + 3).
		float m[16];
		std::memcpy(m, &fbAO.GetCameraViewProjInverse(0), sizeof(m));
		float p[2][3]{};
		for (int k = 0; k < 2; ++k) {
			const float v[4] = { 0.0f, 0.0f, k == 0 ? 0.25f : 0.75f, 1.0f };
			float h[4]{};
			for (int r = 0; r < 4; ++r)
				h[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * v[3];
			for (int i = 0; i < 3; ++i)
				p[k][i] = std::abs(h[3]) > 1e-20f ? h[i] / h[3] : 0.0f;
		}
		const float d[3] = { p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2] };
		const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
		if (len > 1e-6f && std::isfinite(len))
			for (int i = 0; i < 3; ++i)
				forward[i] = d[i] / len;
	}
	const bool prevCameraUsable = aoPrevCamera.valid && aoPrevCamera.frame + 1 == frameNow;
	if (v2 && prevCameraUsable) {
		const double dx = cameraPos.x - aoPrevCamera.camera[0], dy = cameraPos.y - aoPrevCamera.camera[1], dz = cameraPos.z - aoPrevCamera.camera[2];
		const float turn = forward[0] * aoPrevCamera.forward[0] + forward[1] * aoPrevCamera.forward[1] + forward[2] * aoPrevCamera.forward[2];
		if (dx * dx + dy * dy + dz * dz > static_cast<double>(kAOCutDistance) * kAOCutDistance || turn < kAOCutCos) {
			aoHistoryValid = false;
			aoDetailHistoryValid = false;
			aoV2Valid = false;
		}
	}
	const bool geometryValid = v2 && prevCameraUsable && aoV2Valid && aoV2Frame + 1 == frameNow && aoV2Width == width && aoV2Height == height;
	const bool historyValid = settings.AOTemporal && aoHistoryValid && aoHistoryFrame + 1 == globals::state->frameCount;
	const bool detailHistoryValid = detail && settings.AOTemporal && aoDetailHistoryValid && aoDetailHistoryFrame + 1 == globals::state->frameCount;
	const std::uint32_t read = aoHistoryIndex, write = aoHistoryIndex ^ 1u;
	const std::uint32_t detailRead = aoDetailHistoryIndex, detailWrite = aoDetailHistoryIndex ^ 1u;
	ID3D11ShaderResourceView* contact = ssgi.contactAoThisFrame;
	const bool debugOverlay = aoDebugView != AODebugView::Off && debugColorUAV;

	AOCompositeCB cb{};
	cb.Size[0] = width;
	cb.Size[1] = height;
	// (46h) The mode of this frame's trace: AOMode outdoors, AOModeInterior indoors.
	cb.Size[2] = (contact ? kAOFlagContact : 0u) | (aoModeThisFrame == static_cast<int>(AOModeKind::Darker) ? kAOFlagDarker : 0u) |
	             (historyValid ? kAOFlagHistory : 0u) | (aoDebugView == AODebugView::Split ? kAOFlagSplit : 0u) |
	             (detail ? (kAOFlagDetail | kAOFlagValidity) : 0u) | (detailHistoryValid ? kAOFlagDetailHistory : 0u);
	if (contact && settings.AOContactOnce)
		cb.Size[2] |= kAOFlagContactOnce;  // (49h)
	// (47k) The Smooth Normals debug view of this frame (AONormalPrepCS), when it was written at this size.
	const bool normalDebug = debugOverlay && aoNormalDebug.srv && aoNormalDebugFrame == globals::state->frameCount && aoNormalDebugWidth == width &&
	                         aoNormalDebugHeight == height;
	if (normalDebug)
		cb.Size[2] |= kAOFlagNormalDebug;
	// (49b) The shaders keep the 49a view numbers: Specular Occlusion is 11 (48o removed "Temporal v2: History", 10, from the menu).
	cb.Size[3] = debugOverlay ? (aoDebugView == AODebugView::SpecularOcclusion ? 11u : static_cast<std::uint32_t>(aoDebugView)) : 0u;
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
	cb.Detail[0] = std::clamp(settings.AODetailStrength, 0.0f, 2.0f);
	cb.Detail[1] = kAONormalPower;  // (47n) Normal-Guided Filter always
	{
		const float4 cameraData = Util::GetCameraData();
		cb.Camera[0] = cameraData.x;
		cb.Camera[1] = cameraData.y;
		cb.Camera[2] = cameraData.z;
		cb.Camera[3] = cameraData.w;
	}
	// (47l) Temporal v2: its flags and constants (the matrices every frame; without last frame's camera its own matrix, unused then).
	if (v2)
		cb.Size[2] |= kAOFlagTemporalV2 | (geometryValid ? kAOFlagGeometryValid : 0u);
	cb.Size[2] |= kAOFlagDynamicHistory;  // (48o) always on;  // (47n) v2: n <= 1 and a 3x3 clamp; the 47k history: half of this frame, no slack
	// (47r) RTAO: Filter Only Where Traced: the tile passes' shaders and textures (failing them: everywhere, as 47q); never with a debug view.
	ID3D11ComputeShader *tileMarkCS = nullptr, *tileDistHCS = nullptr, *tileDistVCS = nullptr, *temporalWide = nullptr;
	bool tileSkip = aoDebugView == AODebugView::Off && !debugOverlay && EnsureAOTiles();
	if (tileSkip) {
		tileMarkCS = GetShaderDefined(aoTileMarkCS, kFilter, "RTAOFilterCS (tile mark)", "RTAO_TILE_MARK");
		tileDistHCS = GetShaderDefined(aoTileDistHCS, kFilter, "RTAOFilterCS (tile distance)", "RTAO_TILE_DIST");
		tileDistVCS = GetShaderDefined(aoTileDistVCS, kFilter, "RTAOFilterCS (tile distance, vertical)", "RTAO_TILE_DIST,RTAO_VERTICAL");
		if (v2)
			temporalWide = GetShaderDefined(aoTemporalV2WideCS, L"Data\\Shaders\\RayTracing\\RTAOTemporalCS.hlsl", "RTAOTemporalCS (v2, 16x16)", "RTAO_TEMPORAL_V2,RTAO_WIDE");
		if (!tileMarkCS || !tileDistHCS || !tileDistVCS || (v2 && !temporalWide)) {
			aoTilesFault = "a tile shader did not compile (the filter runs everywhere, as 47q)";
			logger::warn("[RayTracing] RTAO: Filter Only Where Traced: {}", aoTilesFault);
			tileSkip = false;
		}
	}
	if (tileSkip)
		cb.Size[2] |= kAOFlagTileSkip;
	std::memcpy(cb.InvViewProj, &fbAO.GetCameraViewProjInverse(0), sizeof(cb.InvViewProj));
	if (prevCameraUsable)
		std::copy_n(aoPrevCamera.viewProj, 16, cb.PrevViewProj);
	else
		std::memcpy(cb.PrevViewProj, &fbAO.GetCameraViewProj(0), sizeof(cb.PrevViewProj));
	std::memcpy(cb.ViewInverse, &fbAO.GetCameraViewInverse(0), sizeof(cb.ViewInverse));
	cb.CameraDelta[0] = prevCameraUsable ? static_cast<float>(static_cast<double>(cameraPos.x) - aoPrevCamera.camera[0]) : 0.0f;
	cb.CameraDelta[1] = prevCameraUsable ? static_cast<float>(static_cast<double>(cameraPos.y) - aoPrevCamera.camera[1]) : 0.0f;
	cb.CameraDelta[2] = prevCameraUsable ? static_cast<float>(static_cast<double>(cameraPos.z) - aoPrevCamera.camera[2]) : 0.0f;
	{
		const float projY = std::abs(fbAO.GetCameraProj(0)._22);
		cb.CameraDelta[3] = 0.5f * static_cast<float>(height) * (std::isfinite(projY) && projY > 1e-3f ? projY : 1.0f);
	}
	cb.Temporal[0] = kAOMinHistoryWeight;
	cb.Temporal[1] = std::clamp(std::clamp(settings.AORayLength, kAORayLengthMin, kAORayLengthMax) * kAOWorldSigmaShare, 2.0f, 10.0f);
	cb.Temporal[2] = kAOHistoryFixFrames;
	cb.Temporal[3] = kAOMaxHistoryLength;
	cb.Clamp[0] = kAOClampK;
	cb.Clamp[1] = kAOClampMinDetail;
	cb.Clamp[2] = kAOClampMinLarge;
	aoCB->Update(cb);
	aoPrevCamera.valid = true;
	aoPrevCamera.frame = frameNow;
	std::memcpy(aoPrevCamera.viewProj, &fbAO.GetCameraViewProj(0), sizeof(aoPrevCamera.viewProj));
	aoPrevCamera.camera[0] = cameraPos.x;
	aoPrevCamera.camera[1] = cameraPos.y;
	aoPrevCamera.camera[2] = cameraPos.z;
	std::copy_n(forward, 3, aoPrevCamera.forward);

	ID3D11ShaderResourceView* nullSrvs[6]{};
	ID3D11UnorderedAccessView* nullUavs[3]{};
	auto clear = [&]() {
		context->CSSetShaderResources(0, 6, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
	};
	auto dispatch = [&](ID3D11ComputeShader* a_shader, std::initializer_list<ID3D11ShaderResourceView*> a_srvs,
						std::initializer_list<ID3D11UnorderedAccessView*> a_uavs, UINT a_x, UINT a_y) {
		context->CSSetShaderResources(0, static_cast<UINT>(a_srvs.size()), a_srvs.begin());
		context->CSSetUnorderedAccessViews(0, static_cast<UINT>(a_uavs.size()), a_uavs.begin(), nullptr);
		context->CSSetShader(a_shader, nullptr, 0);
		context->Dispatch(a_x, a_y, 1);
		clear();
	};
	clear();
	timers->Begin(Util::GpuBucket::RayTracingAO);
	ID3D11Buffer* cbs[1] = { aoCB->CB() };
	context->CSSetConstantBuffers(0, 1, cbs);
	if (tileSkip) {
		// 1b. (47r) Which 8x8 tiles were traced, then the distance (tiles) to the nearest one; t10 for every pass below.
		const UINT tw = (width + 7) / 8, th = (height + 7) / 8;
		dispatch(tileMarkCS, { backend.AORawSRV() }, { aoTileMark.uav.get() }, tw, th);
		ID3D11ShaderResourceView* tileIn = aoTileMark.srv.get();
		context->CSSetShaderResources(6, 1, &tileIn);
		dispatch(tileDistHCS, {}, { aoTileH.uav.get() }, (tw + 7) / 8, (th + 7) / 8);
		tileIn = aoTileH.srv.get();
		context->CSSetShaderResources(6, 1, &tileIn);
		dispatch(tileDistVCS, {}, { aoTileDist.uav.get() }, (tw + 7) / 8, (th + 7) / 8);
		ID3D11ShaderResourceView* tileViews[2] = { nullptr, nullptr };
		context->CSSetShaderResources(6, 1, tileViews);
		ID3D11ShaderResourceView* distView = aoTileDist.srv.get();
		context->CSSetShaderResources(10, 1, &distView);
	}
	if (fullRes) {
		// 2. (46t) Full render resolution: a separable 21-tap edge-aware filter (plane-predicted depth, normal), with two
		//    scales weighted by the far samples' weights.
		dispatch(fullH, { backend.AORawSRV(), depth.depthSRV, normal.SRV }, { aoFullTemp.uav.get() }, groupsX, groupsY);
		dispatch(fullV, { nullptr, depth.depthSRV, normal.SRV, nullptr, nullptr, aoFullTemp.srv.get() }, { aoFiltered.uav.get() }, groupsX, groupsY);
	} else {
		// 2. (46t) The quarter-resolution chain with the far samples' weights (two scales) and normal-guided taps.
		dispatch(down, { backend.AORawSRV(), depth.depthSRV, normal.SRV }, { aoLoW.uav.get(), aoLoDepth.uav.get(), aoLoNormal.uav.get() }, loX, loY);
		dispatch(blurH, { nullptr, nullptr, nullptr, aoLoDepth.srv.get(), aoLoNormal.srv.get(), aoLoW.srv.get() }, { aoLoWBlur.uav.get() }, loX, loY);
		dispatch(blurV, { nullptr, nullptr, nullptr, aoLoDepth.srv.get(), aoLoNormal.srv.get(), aoLoWBlur.srv.get() }, { aoLoW.uav.get() }, loX, loY);
		dispatch(up, { backend.AORawSRV(), depth.depthSRV, normal.SRV, aoLoDepth.srv.get(), aoLoNormal.srv.get(), aoLoW.srv.get() }, { aoFiltered.uav.get() },
			groupsX, groupsY);
	}
	// 3. Optional: the clamped history (after the spatial filter; the history is held inside this frame's filtered
	//    3x3 range plus a small slack). Temporal off: the filtered result is used as it is.
	DenoiseTexture* blended = &aoFiltered;
	DenoiseTexture* detailBlended = detail ? &aoDetailFiltered : nullptr;
	ID3D11SamplerState* sampler = aoLinearSampler.get();
	context->CSSetSamplers(0, 1, &sampler);
	if (v2) {
		// (47l) Temporal v2: the detail term's spatial filter first, then one pass blends both terms with their histories (history
		// lengths, our own last-frame geometry for the disocclusion test, statistical clamp of each term, history fix) and
		// writes this frame's geometry and lengths for the next one. Nothing is blurred after it.
		if (detail)
			dispatch(detailCS, { backend.AORawSRV(), depth.depthSRV, normal.SRV }, { aoDetailFiltered.uav.get() }, groupsX, groupsY);
		const std::uint32_t v2Read = aoV2Index, v2Write = aoV2Index ^ 1u;
		// (47n) t9: the raw trace (its moving-occluder flag); u4: the "Temporal v2: History" debug view ((48o) gone: never bound).
		ID3D11ShaderResourceView* srvs[10] = { aoFiltered.srv.get(), aoHistory[read].srv.get(), motion.SRV, detail ? aoDetailFiltered.srv.get() : nullptr,
			detail ? aoDetailHistory[detailRead].srv.get() : nullptr, aoV2Length[v2Read].srv.get(), aoV2Geometry[v2Read].srv.get(), depth.depthSRV, normal.SRV,
			backend.AORawSRV() };
		ID3D11UnorderedAccessView* uavs[5] = { aoHistory[write].uav.get(), detail ? aoDetailHistory[detailWrite].uav.get() : nullptr,
			aoV2Length[v2Write].uav.get(), aoV2Geometry[v2Write].uav.get(), nullptr };
		context->CSSetShaderResources(0, 10, srvs);
		context->CSSetUnorderedAccessViews(0, 5, uavs, nullptr);
		// (47r) With Filter Only Where Traced: 16x16 groups (the same per-pixel work, less of the apron loaded twice).
		if (tileSkip) {
			context->CSSetShader(temporalWide, nullptr, 0);
			context->Dispatch((width + 15) / 16, (height + 15) / 16, 1);
		} else {
			context->CSSetShader(temporalV2, nullptr, 0);
			context->Dispatch(groupsX, groupsY, 1);
		}
		ID3D11ShaderResourceView* nullSrvs10[10]{};
		ID3D11UnorderedAccessView* nullUavs5[5]{};
		context->CSSetShaderResources(0, 10, nullSrvs10);
		context->CSSetUnorderedAccessViews(0, 5, nullUavs5, nullptr);
		blended = &aoHistory[write];
		aoHistoryIndex = write;
		aoHistoryValid = true;
		aoHistoryFrame = frameNow;
		if (detail) {
			detailBlended = &aoDetailHistory[detailWrite];
			aoDetailHistoryIndex = detailWrite;
			aoDetailHistoryValid = true;
			aoDetailHistoryFrame = frameNow;
		} else {
			aoDetailHistoryValid = false;
		}
		aoV2Index = v2Write;
		aoV2Valid = true;
		aoV2Frame = frameNow;
	} else if constexpr (Settings::AOTemporal) {
		aoV2Valid = false;  // (47l)
		dispatch(temporal, { aoFiltered.srv.get(), aoHistory[read].srv.get(), motion.SRV, backend.AORawSRV() }, { aoHistory[write].uav.get() }, groupsX, groupsY);
		blended = &aoHistory[write];
		aoHistoryIndex = write;
		aoHistoryValid = true;
		aoHistoryFrame = globals::state->frameCount;
	} else {
		aoHistoryValid = false;
		aoV2Valid = false;  // (47l)
	}
	if (v2) {
		// (47l) The detail term was done above.
	} else if (detail) {
		// (46t) The detail term: this frame's near rays, 5x5 edge-aware at full resolution, then its own clamped history.
		dispatch(detailCS, { backend.AORawSRV(), depth.depthSRV, normal.SRV }, { aoDetailFiltered.uav.get() }, groupsX, groupsY);
		if constexpr (Settings::AOTemporal) {
			dispatch(detailTemporal, { aoDetailFiltered.srv.get(), aoDetailHistory[detailRead].srv.get(), motion.SRV, backend.AORawSRV() },
				{ aoDetailHistory[detailWrite].uav.get() }, groupsX, groupsY);
			detailBlended = &aoDetailHistory[detailWrite];
			aoDetailHistoryIndex = detailWrite;
			aoDetailHistoryValid = true;
			aoDetailHistoryFrame = globals::state->frameCount;
		} else {
			aoDetailHistoryValid = false;
		}
	} else {
		aoDetailHistoryValid = false;
	}
	ID3D11SamplerState* nullSampler = nullptr;
	context->CSSetSamplers(0, 1, &nullSampler);
	// 4. The mix with SSGI's AO into the texture its consumers read (DeferredComposite, SSRT) for this frame.
	dispatch(composite, { blended->srv.get(), backend.AORawSRV(), ssgi.GetOwnAoSRV(), contact, detailBlended ? detailBlended->srv.get() : nullptr,
							normalDebug ? aoNormalDebug.srv.get() : nullptr },
		{ aoOut.uav.get(), debugOverlay ? debugColorUAV.get() : nullptr }, groupsX, groupsY);
	if (tileSkip) {
		ID3D11ShaderResourceView* nullTile = nullptr;
		context->CSSetShaderResources(10, 1, &nullTile);
	}
	// 5. (49a) RT Specular Occlusion: this frame's bent normal, blurred for the deferred composite (after the mix: its debug view
	//    reads the final AO). Same constant buffer (b0).
	if (specOccTracedThisFrame && backend.AOBentSRV())
		RunSpecOcc(context, depth.depthSRV, normal.SRV, debugOverlay && aoDebugView == AODebugView::SpecularOcclusion);
	specOccTracedThisFrame = false;
	timers->End(Util::GpuBucket::RayTracingAO);
	ID3D11Buffer* nullCb[1]{};
	context->CSSetConstantBuffers(0, 1, nullCb);
	context->CSSetShader(nullptr, nullptr, 0);

	ssgi.aoOverride = aoOut.srv.get();
	aoComposited = true;
	aoDebugDrawn = debugOverlay;
	// (46t) F7: what this composite used.
	aoLastFilter = fullRes ? 2 : 1;
	aoLastDetailRays = detail ? shape.detailRays : 0;
	aoLastDetailLength = detail ? shape.detailLength : 0.0f;
	aoLastNormalGuided = 1;
	aoLastAlphaBudget = shape.alphaBudget;
	aoCpuMs += static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
}

// ---------------------------------------------------------------------------------------------
// (49a) RT Specular Occlusion (S1)
// ---------------------------------------------------------------------------------------------

bool RayTracing::EnsureSpecOcc(std::uint32_t a_width, std::uint32_t a_height)
{
	if (specOccOut.texture && specOccWidth == a_width && specOccHeight == a_height)
		return true;
	ReleaseSpecOcc();
	if (!CreateDenoiseTexture(this, specOccTemp.texture, specOccTemp.srv, specOccTemp.uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing Specular Occlusion Temp") ||
		!CreateDenoiseTexture(this, specOccOut.texture, specOccOut.srv, specOccOut.uav, a_width, a_height, DXGI_FORMAT_R8G8B8A8_UNORM, "RayTracing Specular Occlusion")) {
		ReleaseSpecOcc();
		return false;
	}
	specOccWidth = a_width;
	specOccHeight = a_height;
	specOccBytes = static_cast<std::uint64_t>(a_width) * a_height * 8;
	d3d11Bytes += specOccBytes;
	return true;
}

void RayTracing::ReleaseSpecOcc()
{
	specOccTemp = {};
	specOccOut = {};
	specOccWidth = specOccHeight = 0;
	d3d11Bytes -= std::min(d3d11Bytes, specOccBytes);
	specOccBytes = 0;
	specOccFrame = UINT32_MAX;
}

void RayTracing::RunSpecOcc(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_normal, bool a_debug)
{
	if (!specOccFault.empty())
		return;
	auto* h = GetShader(specOccHCS, L"Data\\Shaders\\RayTracing\\RTSpecOccCS.hlsl", "RTSpecOccCS (horizontal)");
	auto* v = GetShaderDefined(specOccVCS, L"Data\\Shaders\\RayTracing\\RTSpecOccCS.hlsl", "RTSpecOccCS (vertical)", "RTSO_VERTICAL");
	if (!h || !v) {
		specOccFault = "its blur shader did not compile (see the log)";
		logger::warn("[RayTracing] RT Specular Occlusion: {}", specOccFault);
		return;
	}
	if (!EnsureSpecOcc(aoWidth, aoHeight)) {
		specOccFault = "could not create its blur textures";
		logger::warn("[RayTracing] RT Specular Occlusion: {}", specOccFault);
		return;
	}
	const UINT groupsX = (aoWidth + 7) / 8, groupsY = (aoHeight + 7) / 8;
	ID3D11ShaderResourceView* nullSrvs[4]{};
	ID3D11UnorderedAccessView* nullUavs[2]{};
	ID3D11ShaderResourceView* srvH[3] = { backend.AOBentSRV(), a_depth, a_normal };
	ID3D11UnorderedAccessView* uavH[1] = { specOccTemp.uav.get() };
	a_context->CSSetShaderResources(0, 3, srvH);
	a_context->CSSetUnorderedAccessViews(0, 1, uavH, nullptr);
	a_context->CSSetShader(h, nullptr, 0);
	a_context->Dispatch(groupsX, groupsY, 1);
	a_context->CSSetShaderResources(0, 4, nullSrvs);
	a_context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	ID3D11ShaderResourceView* srvV[4] = { specOccTemp.srv.get(), a_depth, a_normal, aoOut.srv.get() };
	ID3D11UnorderedAccessView* uavV[2] = { specOccOut.uav.get(), a_debug ? debugColorUAV.get() : nullptr };
	a_context->CSSetShaderResources(0, 4, srvV);
	a_context->CSSetUnorderedAccessViews(0, 2, uavV, nullptr);
	a_context->CSSetShader(v, nullptr, 0);
	a_context->Dispatch(groupsX, groupsY, 1);
	a_context->CSSetShaderResources(0, 4, nullSrvs);
	a_context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	specOccFrame = globals::state->frameCount;
}

void RayTracing::BindSpecOccComposite(ID3D11DeviceContext* a_context)
{
	// This frame's blur only (the composite runs in the same frame, after DrawAmbientOcclusion); anything else: the scalar AO.
	const bool on = settings.AOSpecOcc && specOccOut.srv && specOccFrame == globals::state->frameCount;
	SpecOccCompositeCB cb{};
	cb.On = on ? 1u : 0u;
	cb.Strength = std::clamp(settings.AOSpecOccStrength, 0.0f, 1.0f);
	if (specOccCB) {
		specOccCB->Update(cb);
		ID3D11Buffer* buffer = specOccCB->CB();
		a_context->CSSetConstantBuffers(2, 1, &buffer);
	}
	ID3D11ShaderResourceView* srv = on ? specOccOut.srv.get() : nullptr;
	a_context->CSSetShaderResources(21, 1, &srv);
}

// ---------------------------------------------------------------------------------------------
// (46c) Interiors
// ---------------------------------------------------------------------------------------------

// (49b) RT fog / particles (TraceFogCS on the D3D12 side, VolumetricFogRTVisibilityCS in Exponential Height Fog)
void RayTracing::PrepareFogInput(RayTracingRT::FrameInput& a_input, bool a_sun)
{
	a_input.fog = nullptr;
	if (!globals::state)
		return;
	const std::uint32_t frame = globals::state->frameCount;
	const FogGridInfo g = fogGrid;
	// The grid the fog built last frame (it builds after this prepass); a pause in its builds = no RT fog until it builds again.
	if (g.frame == UINT32_MAX || frame - g.frame > 1u || g.x == 0 || g.y == 0 || g.z == 0)
		return;
	const bool sun = a_sun && g.sun;
	const bool lights = g.lights && a_input.pointLights != nullptr;
	if (!sun && !lights)
		return;
	// The textures exist once the fog created its volumes (EnsureFogResources); here only when Ray Tracing started after that (once).
	if (!backend.FogTexturesReady(g.x, g.y, g.z) && !backend.EnsureFogTextures(g.x, g.y, g.z))
		return;
	auto& c = fogInput.constants;
	std::memcpy(c.invViewProj, a_input.constants.invViewProj, sizeof(c.invViewProj));
	c.toSun[0] = a_input.constants.toSun[0];
	c.toSun[1] = a_input.constants.toSun[1];
	c.toSun[2] = a_input.constants.toSun[2];
	c.toSun[3] = kMaxRayLength;
	const bool interior = Util::IsInterior();
	c.grid[0] = g.x;
	c.grid[1] = g.y;
	c.grid[2] = g.z;
	c.grid[3] = interior ? RayTracingRT::kFogFlagInterior : 0u;
	c.zParams[0] = g.zParams[0];
	c.zParams[1] = g.zParams[1];
	c.zParams[2] = g.zParams[2];
	c.zParams[3] = std::clamp(fade, 0.0f, 1.0f);
	const auto cameraData = Util::GetCameraData();
	c.camera[0] = cameraData.x;
	c.camera[1] = cameraData.z;
	c.camera[2] = cameraData.w;
	c.camera[3] = static_cast<float>(frame % 65536u);
	const auto* point = a_input.pointLights;
	if (sun || !point)
		std::copy_n(a_input.constants.rect, 4, c.rect);
	else
		std::copy_n(point->constants.rect, 4, c.rect);
	c.lights[0] = point ? point->constants.params[0] : 0.0f;
	c.lights[1] = kHandOffBand;
	c.lights[2] = 2.0f;  // ray start offset (units): the froxel point is in the air, this only steps off a surface it may sit on
	c.lights[3] = point ? point->constants.bounds[2] : 0.0f;
	c.bounds[0] = point ? point->constants.bounds[0] : 0.0f;
	c.bounds[1] = point ? point->constants.bounds[1] : 0.0f;
	c.bounds[2] = c.bounds[3] = 0.0f;
	c.renderSize[0] = static_cast<float>(a_input.renderWidth);
	c.renderSize[1] = static_cast<float>(a_input.renderHeight);
	c.renderSize[2] = a_input.renderWidth ? 1.0f / static_cast<float>(a_input.renderWidth) : 0.0f;
	c.renderSize[3] = a_input.renderHeight ? 1.0f / static_cast<float>(a_input.renderHeight) : 0.0f;
	fogInput.sun = sun;
	fogInput.lights = lights;
	fogInput.alpha = sun || (point && point->alpha);
	fogInput.set = frame & 1u;
	a_input.fog = &fogInput;
}

void RayTracing::NoteFogTraced(const RayTracingRT::FrameInput& a_input, bool a_traced)
{
	if (!globals::state)
		return;
	const auto& st = backend.GetStats();
	const std::int64_t now = Qpc();
	if (fogCountStart == 0)
		fogCountStart = now;
	if (a_traced && a_input.fog && st.fogTracedLastFrame) {
		auto& r = fogRecord[st.fogSetLastFrame & 1u];
		r.frame = globals::state->frameCount;
		r.sun = st.fogSunLastFrame;
		r.lights = st.fogLightsLastFrame;
		r.grid[0] = a_input.fog->constants.grid[0];
		r.grid[1] = a_input.fog->constants.grid[1];
		r.grid[2] = a_input.fog->constants.grid[2];
		r.fence = st.fogFenceLastFrame;
		fogSunFrames += r.sun ? 1u : 0u;
		fogLightFrames += r.lights ? 1u : 0u;
	}
	if (static_cast<double>(now - fogCountStart) * QpcMs() >= 1000.0) {
		fogShownSun = fogSunFrames;
		fogShownLights = fogLightFrames;
		fogSunFrames = fogLightFrames = 0;
		fogCountStart = now;
	}
}

RayTracing::FogTrace RayTracing::AcquireFogTrace(std::uint32_t a_gridX, std::uint32_t a_gridY, std::uint32_t a_gridZ)
{
	FogTrace t;
	if (!loaded || !settings.Enabled || !globals::state)
		return t;
	const std::uint32_t frame = globals::state->frameCount;
	const std::uint32_t set = (frame - 1u) & 1u;
	const FogRecord& r = fogRecord[set];
	if (r.frame != frame - 1u || r.grid[0] != a_gridX || r.grid[1] != a_gridY || r.grid[2] != a_gridZ || (!r.sun && !r.lights))
		return t;
	if (!backend.FogTexturesReady(a_gridX, a_gridY, a_gridZ))
		return t;
	// GPU-side, for work submitted a frame ago (usually already taken by the guard before this frame's ready signal: then a no-op).
	backend.WaitForFog(r.fence);
	t.sun = r.sun;
	t.lights = r.lights;
	t.sunSRV = backend.FogSunSRV(set);
	t.lightVisSRV = backend.FogLightVisSRV(set);
	t.lightIdSRV = backend.FogLightIdSRV(set);
	std::copy_n(r.grid, 3, t.grid);
	return t;
}

std::uint32_t RayTracing::FogLightId(const RE::BSLight* a_light) const
{
	if (!a_light || !pointThisFrame)
		return 0;
	const std::uint32_t tag = pointSelector.TagFor(a_light) & RayTracingPointLights::kTagSourceMask;
	if (tag != 0)
		return tag;
	// Window beams (spot shadow lights) are not tagged: their slot from the selector's beam table.
	const auto& beams = pointSelector.SlotBeams();
	for (std::uint32_t s = 0; s < RayTracingPointLights::kMaxSources; ++s)
		if (beams[s] == a_light)
			return s + 1;
	return 0;
}

bool RayTracing::EnsureFogResources(std::uint32_t a_gridX, std::uint32_t a_gridY, std::uint32_t a_gridZ)
{
	if (!loaded || !settings.Enabled || !backend.FogAvailable())
		return false;
	return backend.EnsureFogTextures(a_gridX, a_gridY, a_gridZ);
}

std::string RayTracing::FogStatusText() const
{
	if (!loaded || !settings.Enabled)
		return "Ray Tracing is off: the shadow map is used.";
	if (!backend.FogAvailable())
		return backend.FogFault().empty() ? std::string("not available (Ray Tracing has not started)") : "not available: " + backend.FogFault();
	const bool recent = fogCountStart != 0 && static_cast<double>(Qpc() - fogCountStart) * QpcMs() < 2000.0;
	if (!recent)
		return "waiting for Ray Tracing";
	return std::format("traced: sun {} / light {} frames in the last second ({:.1f} MB)", fogShownSun, fogShownLights, backend.FogBytes() / 1048576.0);
}

bool RayTracing::InteriorWanted() const
{
	if (!RtWanted() || !backend.IsReady())
		return false;
	// (46h) The interior is captured for its point lights, or for RTAO indoors (or both: one capture, one trace).
	return (PointLightsOn() && InteriorsOn() && backend.PointAvailable()) || AOInteriorWanted();
}

bool RayTracing::AOInteriorWanted() const
{
	const auto& ssgi = globals::features::screenSpaceGI;
	return RtWanted() && AOInteriorsOn() && backend.IsReady() && backend.AOAvailable() && ssgi.loaded && ssgi.settings.Enabled;
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
	ClearCoverage();  // (47w)
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
		if (backend.HasPrepared()) {  // (47j) an Early Prep context this frame does not trace with
			backend.AbandonPrepared();
			earlyThisFrame = false;
		}
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
	const bool charactersWanted = settings.Characters && backend.SkinAvailable();
	if (charactersUpdatedEarly && charactersWanted && backend.HasPrepared()) {
		// (47j) Early RT Prep Indoors: already done at the frame start (skinDraws / skinBones are this frame's); the prepared
		// context (builds, skinning) is used by ExecuteFrame as outdoors.
		cpuLast.characters = earlyCharactersMs;
	} else {
		if (backend.HasPrepared() && (charactersWanted || charactersUpdatedEarly)) {
			// Prepared without these draws: the trace must not pair other ready flags (as outdoors).
			backend.AbandonPrepared();
			earlyThisFrame = false;
		}
		charactersUpdatedEarly = false;
		skinDraws.clear();
		skinBones.clear();
		if (charactersWanted) {
			characters.SetRelevance(nullptr);  // (47u) interiors: nothing culled
			characters.Update(backend, world, blackScreen ? 6.0 : 1.0, blackScreen ? 32u : 6u);
			characters.CollectDraws(skinDraws, skinBones);
		}
		cpuLast.characters = static_cast<float>(static_cast<double>(Qpc() - charactersStart) * QpcMs());
	}

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
	FillWind(input);  // (47l)
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
	PrepareReflections(input);  // (48d) their own list and fence (no longer inside the RTAO batch)
	if (!input.pointLights && !input.ao && !reflectWantedThisFrame) {  // (48d) or reflection rays
		// Nothing to trace (no shadow-casting light in view, no RTAO): the queued builds go on, nothing waits.
		if (backend.HasPrepared()) {  // (47j) an Early Prep context this frame does not trace with
			backend.AbandonPrepared();
			earlyThisFrame = false;
		}
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
	const bool debug = PointDenoiserDebugView();  // (48m) the point views, + Moving Pixels / Filter Path
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
	RunMaterialCopies(false);  // (48f) Capture Materials: texture copies (D3D11, per-frame budget)
	// (47j) RTAO: Smooth Normals, from last frame's depth still in DepthMask (before PrepareCS overwrites it); (47u) or behind the ready
	// signal from its copy (Start Sun Trace Earlier).
	ResolveRelevance(nullptr);  // (47u) indoors: everything left stale by Skip Updates is updated now
	PrepareDepthMask(context, prepare, depth.depthSRV, mask.SRV, width, height, input);
	const std::int64_t executeStart = Qpc();
	cpuLast.d3d11 = static_cast<float>(static_cast<double>(executeStart - d3d11Start) * QpcMs());
	PrepareFogInput(input, input.sun);  // (49b)
	const bool traced = backend.ExecuteFrame(input);
	NoteFogTraced(input, traced);  // (49b)
	FlushDeferredAONormal();  // (47u)
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
	if (!pointThisFrame) {
		pointK = 0;
		pointWideThisFrame = false;  // (48a)
		pointExtraThisFrame = false;  // (48c)
	}
	// (46h) RTAO went out right behind it (second fence): DrawAmbientOcclusion uses it after Screen Space GI.
	aoThisFrame = input.ao != nullptr && backend.GetStats().aoTracedLastFrame;
	reflectThisFrame = input.reflect != nullptr && backend.GetStats().reflectSubmittedLastFrame;  // (48d)
	aoInteriorThisFrame = aoThisFrame;
	aoModeThisFrame = aoThisFrame ? std::clamp(settings.AOModeInterior, 0, static_cast<int>(AOModeKind::Count) - 1) : -1;
	if (!aoThisFrame)
		aoHistoryValid = false;

	pending.active = true;
	pending.interior = true;
	pending.noSun = false;  // (47n)
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
		SwitchOffInteriors("the shadow mask shader (PointMaskCS) did not compile");
		return;
	}
	// (48a) Slots 4 - 5 (t4) when this frame denoised them (Channels[0].Flags.y = PointMaskCS kWide).
	const std::uint32_t latest = pointWrite ^ 1u;  // FinishPointLights flipped it after writing
	const bool wideMask = rt && pointWideThisFrame && pointWideHistoryValid && pointWide[latest].srv;
	cb.Channels[0].Flags[1] = wideMask ? 1u : 0u;
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
	ID3D11Buffer* cbs[2] = { combineCB->CB(), pointMaskCB->CB() };
	// (50b) DLSS RR mode: the decoded rays, as Lighting reads them (Ray Reconstruction cleans the shadow-mask lights too).
	const bool rawMask = pointRRBound && pointRaw.srv;
	ID3D11ShaderResourceView* srvs[5] = { mask.SRV, depth.depthSRV, rt ? (rawMask ? pointRaw.srv.get() : pointVis[latest].srv.get()) : nullptr,
		rt ? pointIdx[latest].srv.get() : nullptr, wideMask ? ((rawMask && pointRawWide.srv) ? pointRawWide.srv.get() : pointWide[latest].srv.get()) : nullptr };
	ID3D11UnorderedAccessView* uavs[1] = { maskTempUAV.get() };
	context->CSSetConstantBuffers(0, 2, cbs);
	context->CSSetShaderResources(0, 5, srvs);
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
	context->CSSetShader(shader, nullptr, 0);
	context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
	ID3D11ShaderResourceView* nullSrvs[5]{};
	ID3D11UnorderedAccessView* nullUavs[1]{};
	ID3D11Buffer* nullCbs[2]{};
	context->CSSetShaderResources(0, 5, nullSrvs);
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
	// (46r) The "Ray Tracing" entry: the master switch and status, a one-line summary, Advanced (loading screens) and the
	// diagnostics (both collapsed). Sun Shadows, Point Light Shadows, Ambient Occlusion and (48i) Reflections have their own entries in the
	// Ray Tracing category (each shows the switch and the status on top).
	ImGui::PushID("rtMainPage");  // tree-node open states of its own (other pages also have Advanced / Diagnostics)
	DrawMasterHeader(true);
	// (47y) Moving Characters: Keep Shadow History / Smooth When No History: (48o) fixed on (no switches).
	DrawObjectRows();  // (47n) one per-kind choice for the sun, the point lights and RTAO
	// (49f) Particles Use RT Shadows moved to the Fog & Particles entry. (50b) The loading-screen master is in Diagnostics (DrawDiagnostics).
	DrawDiagnostics();
	ImGui::PopID();
}

void RayTracing::DrawFogParticlesPage()
{
	// (49f) The "Fog & Particles" entry of the Ray Tracing category: what reads the volumetric fog's ray-traced visibility. The fog
	// switches stay in Exponential Height Fog's settings, the particle switch in Ray Tracing's (saved values carry over).
	ImGui::PushID("rtFogParticlesPage");
	DrawMasterHeader();
	ImGui::SeparatorText("Fog");
	globals::features::exponentialHeightFog.DrawRayTracedFogSettings();
	ImGui::SeparatorText("Particles");
	DrawParticlesSection();
	// (50b) Smooth Shadows on Smoke and Water Sheets moved here (Diagnostics, collapsed), to be retired after a long quiet period.
	if (ImGui::TreeNodeEx("Diagnostics")) {
		ImGui::BeginDisabled(!settings.ParticlesRTShadows);
		ImGui::Checkbox("Smooth Shadows on Smoke and Water Sheets", &settings.ParticlesSmoothSheets);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): the lamp and torch shadows on smoke, mist and waterfall sheets are blended smoothly between the fog's");
			ImGui::Text("  shadow samples and the shadow on the wall or ground right behind the sheet, by their real distances. No more lit / dark");
			ImGui::Text("  squares (16 pixels at the default fog grid) on sheets that lie on a wall; the edges line up with the shadows on the wall.");
			ImGui::Text("Off: as 49m (the nearest fog sample: the squares). Only with Particles Use RT Shadows on.");
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void RayTracing::DrawParticlesSection()
{
	// (49b) Particles Use RT Shadows (lit effects read the volumetric fog's ray-traced visibility). (49f) Section title by the caller.
	ImGui::Checkbox("Particles Use RT Shadows", &settings.ParticlesRTShadows);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("On (default): smoke, dust and mist sheets (lit effects) take the ray-traced shadows the volumetric fog traces: they darken in the");
		ImGui::Text("  shade of trees, characters and objects (Ray Tracing took those out of the game's shadow map), and lights that Ray Tracing");
		ImGui::Text("  shadows (torches, fireplaces, window beams) light them only where the light is not blocked. Fireplaces and chandeliers that");
		ImGui::Text("  cast engine shadows now light smoke too (before, such lights were skipped for effects).");
		ImGui::Text("Needs Exponential Height Fog's Volumetric Fog (its near grid holds the data; indoors also Skip Volumetric Fog Indoors off).");
		ImGui::Text("Flames, magic and glowing effects are not lit and do not change. Off: as before.");
	}
	// (50a) Smooth Shadows on Smoke and Water Sheets: (50b) in this page's Diagnostics.
	if (settings.ParticlesRTShadows && settings.Enabled) {
		auto& fog = globals::features::exponentialHeightFog;
		if (!fog.loaded || !fog.VolumetricFogActive())
			ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "Volumetric Fog is not built here: particles keep their old lighting.");
	}
}

void RayTracing::DrawLoadingScreens()
{
	// (49c) "Loading Screens" (replaces 49b's Hitch Fixes). (50b) Only the master is left, in the main page's Diagnostics as a kill switch;
	// its three parts (Prepare When Arriving, Use the Loading Screen's Thread for GPU Copies, Reserve Shaders and Memory Up Front) are
	// retired and always on with it. Master off = 48o / 49a.
	ImGui::Checkbox("Finish Loading Work Behind the Loading Screen", &settings.LoadFinish);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("On (default): after fast travel or a door, Ray Tracing's preparation of the new place is done while the game's own loading");
		ImGui::Text("  screen is still showing, instead of in the first seconds you can see. The loading screen may stay up about 0.5 - 1.5 s longer;");
		ImGui::Text("  at most 3 s. Ray Tracing takes over from the shadow map once the place is complete (objects, trees and cut-outs included).");
		ImGui::Text("  Includes: preparing when you arrive, GPU copies on the loading screen's thread (grass uploads too), shaders and memory");
		ImGui::Text("  reserved up front (+16 MB point-light textures, the cut-out record buffers at full size).");
		ImGui::Text("Save loads have no point to prepare at behind the loading screen: they work as before (the log says so).");
		ImGui::Text("Off (emergency switch only, e.g. a loading screen that hangs): all of it off (48o / 49a).");
	}
}

namespace
{
	/// (48i) Two sets of preset values equal (a_tolerance: for the saved roughness floats).
	bool SameReflectionValues(const RayTracing::ReflectionPresetValues& a, const RayTracing::ReflectionPresetValues& b, float a_tolerance)
	{
		return a.terrain == b.terrain && a.fullRange == b.fullRange && std::abs(a.roughStart - b.roughStart) <= a_tolerance &&
		       std::abs(a.roughEnd - b.roughEnd) <= a_tolerance && a.beyondSSR == b.beyondSSR && a.roughSolid == b.roughSolid &&
		       a.materials == b.materials && a.characters == b.characters && a.offscreenColor == b.offscreenColor && a.sunRay == b.sunRay &&
		       a.lamps == b.lamps && a.glow == b.glow && a.preferBright == b.preferBright;
	}
}

RayTracing::ReflectionPresetValues RayTracing::CurrentReflectionValues() const
{
	ReflectionPresetValues v;
	v.terrain = settings.ReflectionsTerrain;
	v.fullRange = settings.ReflectionsFullRange;
	v.roughStart = settings.ReflectionsRoughStart;
	v.roughEnd = settings.ReflectionsRoughEnd;
	v.beyondSSR = settings.ReflectionsBeyondSSR;
	v.roughSolid = settings.ReflectionsRoughSolid;
	v.materials = settings.ReflectionsMaterials;
	v.characters = settings.ReflectionsCharacterMaterials;
	v.offscreenColor = settings.ReflectionsOffscreenColor;
	v.sunRay = settings.ReflectionsSunRay;
	v.lamps = settings.ReflectionsShadeLights;
	v.glow = settings.ReflectionsShadeGlow;
	v.preferBright = settings.ReflectionsPreferBrightCubemap;
	return v;
}

void RayTracing::ApplyReflectionValues(const ReflectionPresetValues& a_values)
{
	settings.ReflectionsTerrain = a_values.terrain;
	settings.ReflectionsFullRange = a_values.fullRange;
	settings.ReflectionsRoughStart = std::clamp(a_values.roughStart, 0.0f, 0.95f);
	settings.ReflectionsRoughEnd = std::clamp(a_values.roughEnd, 0.05f, 1.0f);
	settings.ReflectionsBeyondSSR = a_values.beyondSSR;
	settings.ReflectionsRoughSolid = a_values.roughSolid;
	settings.ReflectionsMaterials = a_values.materials;
	settings.ReflectionsCharacterMaterials = a_values.characters;
	settings.ReflectionsOffscreenColor = std::clamp(a_values.offscreenColor, 0, 1);
	settings.ReflectionsSunRay = a_values.sunRay;
	settings.ReflectionsShadeLights = a_values.lamps;
	settings.ReflectionsShadeGlow = a_values.glow;
	settings.ReflectionsPreferBrightCubemap = a_values.preferBright;
}

int RayTracing::MatchReflectionPreset() const
{
	const ReflectionPresetValues live = CurrentReflectionValues();
	for (int t = 0; t < kReflectionCustom; ++t)
		if (SameReflectionValues(live, kReflectionPresets[t], 0.005f))
			return t;
	return kReflectionCustom;
}

void RayTracing::DrawReflectionsPage()
{
	// (48i) Was the "Reflections (experimental)" section of the main page (48a-48h). Same values, same behaviour; the top holds the switch,
	// the Quality preset and the brightness calibration, Advanced what the presets set, Diagnostics the fixes kept for A/B, the debug view
	// and the detailed status.
	ImGui::PushID("rtReflectPage");
	DrawMasterHeader();
	settings.ReflectionsMode = std::clamp(settings.ReflectionsMode, 0, 2);
	settings.ReflectionsQuality = std::clamp(settings.ReflectionsQuality, 0, kReflectionCustom);
	settings.ReflectionsOffscreenColor = std::clamp(settings.ReflectionsOffscreenColor, 0, 1);
	settings.ReflectionsRoughStart = std::clamp(settings.ReflectionsRoughStart, 0.0f, 0.95f);
	settings.ReflectionsRoughEnd = std::clamp(settings.ReflectionsRoughEnd, 0.05f, 1.0f);
	settings.ReflectionsOffscreenBrightness = std::clamp(settings.ReflectionsOffscreenBrightness, 0.25f, 4.0f);
	if (ImGui::TreeNodeEx("Reflections", ImGuiTreeNodeFlags_DefaultOpen)) {
		// Off / On (Gap Fill). Trace Only (the same rays, nothing in the picture) is under Diagnostics.
		{
			const char* preview = settings.ReflectionsMode == 0 ? "Off" : (settings.ReflectionsMode == 2 ? "On (Gap Fill)" : "Trace Only (see Diagnostics)");
			if (ImGui::BeginCombo("RT Reflections", preview)) {
				if (ImGui::Selectable("Off", settings.ReflectionsMode == 0))
					settings.ReflectionsMode = 0;
				if (ImGui::Selectable("On (Gap Fill)", settings.ReflectionsMode == 2))
					settings.ReflectionsMode = 2;
				ImGui::EndCombo();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Ray-traced reflections fill the gaps of screen-space reflections (SSR).");
				ImGui::Text("  On (Gap Fill): where SSR finds nothing and the old path shows the cubemap (eaves, indoors, the screen edge, wet ground),");
				ImGui::Text("  a reflection ray says what is really there: the sky (cubemap with sky), a point on screen (its colour), or a point off");
				ImGui::Text("  screen (its own shaded colour, or the dynamic cubemap where its stored position matches).");
				ImGui::Text("  Needs Screen Space Ray Tracing specular on with its Dynamic Cubemaps fallback; otherwise nothing is traced.");
				ImGui::Text("Off (default): exactly as 48c (no terrain in the scene, no textures, no rays).");
				ImGui::Text("Trace Only (the same rays, results only in the debug views and F7) is under Diagnostics.");
			}
		}
		ImGui::BeginDisabled(settings.ReflectionsMode == 0);
		{
			int pick = settings.ReflectionsQuality;
			if (ImGui::Combo("Quality##Reflections", &pick, kReflectionQualityNames, IM_ARRAYSIZE(kReflectionQualityNames))) {
				pick = std::clamp(pick, 0, kReflectionCustom);
				if (pick != settings.ReflectionsQuality) {
					if (settings.ReflectionsQuality == kReflectionCustom)
						settings.ReflectionsCustom = CurrentReflectionValues();  // kept for when Custom is picked again
					settings.ReflectionsQuality = pick;
					ApplyReflectionValues(pick == kReflectionCustom ? settings.ReflectionsCustom : kReflectionPresets[pick]);
				}
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Reflections pay off on smooth surfaces (polished floors, calm water, metal, wet stone); the cost grows with how many");
				ImGui::Text("  rougher surfaces also trace, with the material shading, reflected characters and the sun shadow ray.");
				for (const auto& lines : kReflectionPresetTooltips)
					for (const char* line : lines)
						if (line[0] != '\0')
							ImGui::Text("%s", line);
				ImGui::Text("Custom: the values under Advanced (kept when you pick a preset; picking Custom brings them back).");
				ImGui::Text("Changing a value under Advanced switches to Custom. A preset never changes RT Reflections (Off / On), Off-screen Hit");
				ImGui::Text("  Brightness or anything under Diagnostics. Picking one that changes Capture Materials or Characters captures the scene again.");
			}
		}
		ImGui::EndDisabled();
		ImGui::BeginDisabled(settings.ReflectionsMode != 2 || !settings.ReflectionsMaterials);
		ImGui::SliderFloat("Off-screen Hit Brightness", &settings.ReflectionsOffscreenBrightness, 0.25f, 4.0f, "%.2f");
		settings.ReflectionsOffscreenBrightness = std::clamp(settings.ReflectionsOffscreenBrightness, 0.25f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Default 1.00. Scales the Material colour of off-screen hits, to match it by eye to what the screen shows (the shading is");
			ImGui::Text("  simplified: no fog, no shine of the hit surface). Reflection Debug View > RT Shading vs Screen (Diagnostics) and the line");
			ImGui::Text("  below show how much brighter (> 1) or darker (< 1) it measures where a hit is also on screen. Not part of the presets.");
			ImGui::Text("  Needs Off-screen Hit Color: Material (Advanced).");
		}
		ImGui::EndDisabled();
		// (48h) The stable brightness ratio and the slider value it suggests (only while the off-screen shading runs).
		if (settings.ReflectionsMode == 2) {
			const bool shading = settings.ReflectionsMaterials && MaterialsWanted() && ShadeWanted() && !reflectShadeFailed && reflectShadeCS;
			if (!shading) {
				ImGui::TextDisabled("Brightness shading / screen: - (only while Off-screen Hit Color: Material is shading)");
			} else {
				if (shadeRatioSmoothed > 0.0f) {
					const float suggested = std::clamp(1.0f / shadeRatioSmoothed, 0.25f, 4.0f);
					ImGui::Text("Brightness shading / screen (2 s average): %.2f (%.0f px per frame) -> set Off-screen Hit Brightness to about %.2f",
						shadeRatioSmoothed, shadeSmoothPixels, suggested);
				} else {
					ImGui::TextDisabled("Brightness shading / screen: - (no hit that is also on screen yet)");
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Where a ray's hit is also visible on screen, how bright the shading is against the screen there (1.00 = equal), as a");
					ImGui::Text("  typical value (geometric mean of the per-pixel ratios, each limited to 16x either way) averaged over about 2 seconds,");
					ImGui::Text("  so single bright spots no longer make it jump. Measured before Off-screen Hit Brightness: the suggested slider");
					ImGui::Text("  value is 1 / this ratio (look around a little and wait 2-3 s for it to settle).");
					ImGui::Text("  This frame: %s; 48g's old formula (ratio of the means, outlier-prone): %s (%u groups).",
						shadeRatio > 0.0f ? std::format("{:.2f} over {} px", shadeRatio, shadeRatioPixels).c_str() : "-",
						shadeRatioArith >= 0.0f ? std::format("{:.2f}", shadeRatioArith).c_str() : "-", shadeRatioGroups);
				}
			}
		}
		// (48e) Background compiles of the reflection shaders (no first-use freeze).
		if (settings.ReflectionsMode != 0) {
			auto& ssrtFeature = globals::features::screenSpaceRayTracing;
			const bool prepCompiling = aoNormalReflectAsync.GetStatus() == Util::AsyncComputeShader::Status::Compiling;
			const bool fillCompiling = settings.ReflectionsMode == 2 && ssrtFeature.raymarchSpecularRTAsync.GetStatus() == Util::AsyncComputeShader::Status::Compiling;
			if (prepCompiling || fillCompiling)
				ImGui::TextDisabled("Preparing the reflection shaders in the background (the picture uses the old path until they are ready)...");
			if (settings.ReflectionsMode == 2 && !GapFillConsumer())
				ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "Gap Fill needs Screen Space Ray Tracing specular with its Dynamic Cubemaps fallback: nothing is traced.");
			if (!reflectOffReason.empty())
				RtErrorLine("RT reflections were switched off automatically", reflectOffReason);
			else if (backend.IsReady() && !backend.ReflectAvailable())
				RtErrorLine("RT reflections unavailable", backend.ReflectFault().empty() ? std::string("the GPU driver did not accept their shader") : backend.ReflectFault());
		}

		// Advanced (collapsed): what the presets set. Any change switches the Quality to Custom.
		if (ImGui::TreeNodeEx("Advanced")) {
			const ReflectionPresetValues before = CurrentReflectionValues();
			ImGui::BeginDisabled(settings.ReflectionsMode == 0);
			ImGui::Checkbox("Terrain in RT Scene (reflections only)", &settings.ReflectionsTerrain);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (all presets): the ground of the loaded cells goes into the ray-traced scene for the reflection rays only (a separate");
				ImGui::Text("  layer no shadow or ambient-occlusion ray sees: sun shadows, lamp shadows and RTAO stay exactly the same).");
				ImGui::Text("  Takes effect only while RT Reflections is not Off. It also shows in the BLAS Coverage debug view.");
				ImGui::Text("  Off: reflection rays pass through hills (on water they may show sky where a hill should be); saves its video memory.");
			}
			// (48d) Which pixels trace a ray.
			ImGui::Checkbox("Full Roughness Range", &settings.ReflectionsFullRange);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Off (all presets): rough surfaces trace fewer rays, fading smoothly between the two roughness values below (a pixel in the");
				ImGui::Text("  fade traces on some frames and keeps the cubemap on the others; the denoiser blends the two). Reflections never stop at");
				ImGui::Text("  a hard roughness edge. On: every reflecting surface traces, whatever its roughness (much more cost; for comparing).");
			}
			ImGui::BeginDisabled(settings.ReflectionsFullRange);
			ImGui::SliderFloat("Rough Fade Start", &settings.ReflectionsRoughStart, 0.0f, 0.9f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Roughness where the share of traced pixels starts to fall. Below it every reflecting pixel traces.");
				ImGui::Text("  Low 0.15, Medium 0.25, High 0.30 (the 48h default), Ultra 0.40.");
			}
			ImGui::SliderFloat("Rough Fade End", &settings.ReflectionsRoughEnd, 0.1f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Roughness where no pixel traces any more (always at least 0.05 above the start).");
				ImGui::Text("  Low 0.35, Medium 0.50, High 0.65 (the 48h default), Ultra 0.80. The biggest cost lever: a wider fade traces more pixels.");
			}
			ImGui::EndDisabled();
			settings.ReflectionsRoughStart = std::clamp(settings.ReflectionsRoughStart, 0.0f, 0.95f);
			settings.ReflectionsRoughEnd = std::clamp(settings.ReflectionsRoughEnd, 0.05f, 1.0f);
			ImGui::Checkbox("Also Beyond SSR Distance", &settings.ReflectionsBeyondSSR);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Off (Low - High): pixels beyond Screen Space Ray Tracing's distance limit trace nothing (the picture never uses them there).");
				ImGui::Text("  On (Ultra): they trace too, and while Gap Fill runs the specular chain lifts its distance limit for the gap fill (SSR");
				ImGui::Text("  itself still stops at the limit; the denoiser then also works past it). More cost.");
			}
			ImGui::Checkbox("Rough Rays: Leaves Solid", &settings.ReflectionsRoughSolid);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (all presets): rays from rough surfaces (roughness above about 0.35, fading in smoothly) treat leaves and other cut-outs");
				ImGui::Text("  as solid instead of testing up to 16 of them, and look less far (down to 30 %% of the range at roughness 0.9). A rough");
				ImGui::Text("  ray that finds nothing in its shorter range keeps the old cubemap. Off: as 48c (every ray tests its cut-outs, full range).");
			}
			// (48f) The data half of the material step.
			ImGui::Checkbox("Reflections: Capture Materials", &settings.ReflectionsMaterials);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (Medium - Ultra): what the reflection rays need to colour what they hit. Every captured surface keeps its texture");
				ImGui::Text("  coordinates and direction (16 bytes per triangle), every distinct texture gets a small 128x128 copy (a few per frame while");
				ImGui::Text("  playing, many in the black screen after a loading screen), and the rays note the texture and spot they hit. Check it with");
				ImGui::Text("  Reflection Debug View > RT Albedo. Changing it captures the scene again. Used by 'Off-screen Hit Color: Material' below.");
				ImGui::Text("Off (Low): nothing of it (as 48e); off-screen hits keep the cubemap colour.");
			}
			// (48g) The shading half.
			ImGui::BeginDisabled(!settings.ReflectionsMaterials);
			ImGui::Checkbox("Capture Materials: Characters", &settings.ReflectionsCharacterMaterials);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (High, Ultra): characters also keep their texture coordinates and their own texture (faces per person), so reflected");
				ImGui::Text("  people get their real colours (RT Albedo no longer shows them purple). Changing it captures the characters again (a moment");
				ImGui::Text("  of shadow-map shadows on them). Off (Low, Medium): as 48f (purple in RT Albedo, the cubemap colour in the picture).");
			}
			ImGui::EndDisabled();
			ImGui::BeginDisabled(settings.ReflectionsMode != 2 || !settings.ReflectionsMaterials);
			{
				const char* kOffscreen[2] = { "Cubemap Approx", "Material" };
				ImGui::Combo("Off-screen Hit Color", &settings.ReflectionsOffscreenColor, kOffscreen, 2);
				settings.ReflectionsOffscreenColor = std::clamp(settings.ReflectionsOffscreenColor, 0, 1);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Gap Fill + Capture Materials. What a reflection ray that hits something OFF screen shows (hits on screen always take the");
					ImGui::Text("  screen's own colour):");
					ImGui::Text("  Material (Medium - Ultra): the hit's own texture, lit by the sun (with a shadow ray), the sky light, the lamps and its own");
					ImGui::Text("    glow. Where its data is missing (texture copy still pending, no material, captured before) it fades to the cubemap guess;");
					ImGui::Text("    new copies fade in over about half a second, so nothing pops.");
					ImGui::Text("  Cubemap Approx (Low): as 48f (the dynamic cubemap where its stored point matches, else the cubemap without sky).");
					ImGui::Text("  Switching fades over about 0.4 s.");
				}
				ImGui::Checkbox("Material Hits: Sun Shadow Ray", &settings.ReflectionsSunRay);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On (High, Ultra): each hit that faces the sun sends one more ray to the sun (leaves count as solid), so reflected shadows");
					ImGui::Text("  are dark. A small extra cost on the ray-tracing GPU queue. Off (Low, Medium): every hit facing the sun is lit (no shadows");
					ImGui::Text("  in the reflection).");
				}
				ImGui::Checkbox("Material Hits: Lamps", &settings.ReflectionsShadeLights);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On (Medium - Ultra): lamps, torches and fires (Light Limit Fix's lights) light the hits too (no lamp shadows). Off: only");
					ImGui::Text("  the sun and the sky light (cheaper in towns at night).");
				}
				// (48h)
				ImGui::Checkbox("Material Hits: Glow", &settings.ReflectionsShadeGlow);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On (Medium - Ultra): what glows in the game glows in the reflection too, worked out like the game does it: glowing windows");
					ImGui::Text("  (their glow texture is copied as well, and their colour follows the time of day like the real window), lamps, embers.");
					ImGui::Text("  The glow is tinted by the surface's own texture (stained glass stays coloured), so lit lamps are no longer too bright.");
					ImGui::Text("  A glowing window waits until its glow copy is ready (the cubemap until then, then a fade-in).");
					ImGui::Text("Off: as 48g (only the own glow colour of lamp-like shapes, added untinted; windows show as dark glass off screen).");
					ImGui::Text("  The glow data is captured either way, so switching is instant.");
				}
				ImGui::BeginDisabled(settings.ReflectionsOffscreenColor != 1);
				ImGui::Checkbox("Off-screen: Prefer Brighter Cubemap", &settings.ReflectionsPreferBrightCubemap);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Material only. On (Medium - Ultra): where the dynamic cubemap stored the very point a ray hit off screen (Gap Fill:");
					ImGui::Text("  Cubemap Position Check, Diagnostics) and saw it much brighter than the Material colour (from 2x, fully at 4x), the");
					ImGui::Text("  reflection leans on the cubemap's colour instead - for fires, glows and effects the ray-traced scene cannot show.");
					ImGui::Text("  Smooth, no hard edge. (RT Reflection Use still shows them pale yellow.) Off: as 48g.");
				}
				ImGui::EndDisabled();
			}
			ImGui::EndDisabled();
			ImGui::EndDisabled();
			// An edit: Custom, with the values as they are now.
			if (!SameReflectionValues(before, CurrentReflectionValues(), 0.0f))
				settings.ReflectionsQuality = kReflectionCustom;
			ImGui::TreePop();
		}

		// Diagnostics (collapsed): the fixes kept as switches for A/B (always on by default, not touched by the presets), Trace Only,
		// the debug view and the detailed status.
		if (ImGui::TreeNodeEx("Diagnostics")) {
			ImGui::BeginDisabled(settings.ReflectionsMode == 0);
			{
				bool traceOnly = settings.ReflectionsMode == 1;
				if (ImGui::Checkbox("Trace Only (measure the cost, picture unchanged)", &traceOnly))
					settings.ReflectionsMode = traceOnly ? 1 : 2;
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On: the same reflection rays are traced, but their results go only to the debug views and F7; the picture is as with");
					ImGui::Text("  RT Reflections Off (for measuring the cost of the rays alone). Off (default): RT Reflections On (Gap Fill).");
				}
			}
			ImGui::Checkbox("Material From Best Candidate", &settings.ReflectionsBestCandidate);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): roughness and reflectance come from whichever of last frame's two matches (camera / motion) fits better, so a");
				ImGui::Text("  moving character uses its own material. Off: as 48c (the camera match first: it read the background behind a walker).");
			}
			// (48d) Gap Fill only.
			ImGui::BeginDisabled(settings.ReflectionsMode != 2);
			ImGui::Checkbox("Gap Fill: Direction Check", &settings.ReflectionsLobeCheck);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): a ray is used only where its direction fits this frame's surface direction and roughness (it was aimed with");
				ImGui::Text("  last frame's); where it does not (a character turned, a new edge) it fades smoothly to the old path. Off: always used.");
			}
			ImGui::Checkbox("Gap Fill: Cubemap Position Check", &settings.ReflectionsCubemapMatch);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): a hit off screen takes the dynamic cubemap's colour where the cubemap stored that same point; elsewhere the");
				ImGui::Text("  cubemap without sky. Off: always the cubemap without sky (and Prefer Brighter Cubemap has nothing to compare with).");
			}
			ImGui::Checkbox("Gap Fill: Real Hit Distance", &settings.ReflectionsHitDistance);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): the reflection denoiser gets the ray's real hit distance where the ray filled the gap (instead of 'far'),");
				ImGui::Text("  which keeps near reflections steadier when moving. Off: as before.");
			}
			ImGui::Checkbox("Gap Fill: Cubemap Warm-Up Fix", &settings.ReflectionsCubemapWarmUp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): right after a loading screen the dynamic cubemap starts empty and fills in over many frames; the position");
				ImGui::Text("  check and the off-screen colours undo that fill-in, so matched reflections appear at full strength from the first");
				ImGui::Text("  capture of that direction. Once the cubemap has settled it changes nothing. Off: as 48d (they fade in over ~0.5 s).");
			}
			ImGui::EndDisabled();
			ImGui::Checkbox("Reflections: Prepare Behind Loading Screens", &settings.ReflectionsLoadingPrep);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): the ground (terrain) for the reflection rays is built while the loading screen is up, and the switch to");
				ImGui::Text("  RT after it waits for it like for everything else. Off: as 48d (at most 4 cells of ground per frame, after the switch).");
			}
			{
				const char* kViews[] = { "Off", "Hit Type", "Hit Distance", "Normal Confidence", "RT Reflection Use", "RT Albedo", "RT Shading vs Screen" };
				static_assert(IM_ARRAYSIZE(kViews) == static_cast<int>(ReflectDebugView::Count));  // (48j) one name per view
				int view = static_cast<int>(reflectDebugView);
				if (ImGui::Combo("Reflection Debug View", &view, kViews, IM_ARRAYSIZE(kViews)))
					reflectDebugView = static_cast<ReflectDebugView>(std::clamp(view, 0, static_cast<int>(ReflectDebugView::Count) - 1));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Not saved. Hit Type: green building / static, orange object, cyan leaves / cut-out, purple character, brown terrain,");
					ImGui::Text("  blue = the ray left the scene (sky, far away), grey = nothing within a rough ray's shorter range. Hit Distance: white");
					ImGui::Text("  near .. dark far. Normal Confidence: how sure the reprojected last-frame surface direction is (green sure .. red fell");
					ImGui::Text("  back to the depth). Transparent = not traced.");
					ImGui::Text("RT Reflection Use (Gap Fill): what the picture used where SSR found nothing: green = a point on screen, cyan = the cubemap");
					ImGui::Text("  at a matching position, orange = the cubemap without sky (position did not match), blue = sky; brighter = more weight.");
					ImGui::Text("  Transparent = not used (SSR found it, not traced, or the direction check faded it out).");
					ImGui::Text("RT Albedo (Capture Materials): the texture colour of what each traced ray hit (ground: its layers' average colours). Magenta =");
					ImGui::Text("  captured without material data, red-orange = no readable texture, yellow / black checks = its copy is still pending,");
					ImGui::Text("  purple = a character without material data, dim blue / grey = nothing hit. Dark magenta everywhere = no material data this frame.");
					ImGui::Text("RT Reflection Use also shows pale yellow = the hit's own Material colour (Off-screen Hit Color: Material).");
					ImGui::Text("RT Shading vs Screen (Gap Fill + Capture Materials): where a ray's hit is also visible on screen, the shaded colour against");
					ImGui::Text("  the screen's: green = within about 20 %%, red = shading brighter, blue = shading darker (stronger = up to 4x). Elsewhere the");
					ImGui::Text("  shaded colour itself (hits off screen). The mean ratio is on the page's top, under Off-screen Hit Brightness.");
				}
			}
			ImGui::EndDisabled();
			if (settings.ReflectionsMode != 0) {
				const auto& rs = backend.GetStats();
				const auto& sc = scene.GetStats();
				const double pixels = std::max(1.0, static_cast<double>(reflectInput.constants.renderSize[0]) * reflectInput.constants.renderSize[1]);
				ImGui::Text("Rays: %.2f ms (wall), %.0f %% of pixels traced; gap fill %s; terrain: %u of %u cells built, %s", std::max(rs.lastReflectGpuMs, 0.0f),
					rs.reflectPixelsPerFrame > 0 ? 100.0 * static_cast<double>(rs.reflectPixelsPerFrame) / pixels : 0.0, reflectGapFillUsedLastFrame ? "used" : "not used",
					sc.terrainBuilt, sc.terrainCells, Megabytes(sc.terrainBytes + backend.ReflectBytes()).c_str());
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Time between the reflection rays' two GPU timestamps (wall time: other work runs in between; the real cost is the");
					ImGui::Text("  frame's GPU time with and without), the share of the render pixels traced, whether the SSR pass used them last frame,");
					ImGui::Text("  the terrain BLASes (built / made) and the video memory of terrain and the reflection textures. Terrain quadrants: %u from",
						sc.terrainQuadsCpu);
					ImGui::Text("  the game's vertex copy, %u rebuilt from its heights, %u failed.", sc.terrainQuadsHeights, sc.terrainQuadsFailed);
				}
				// (48f) Capture Materials.
				if (settings.ReflectionsMaterials) {
					const auto& ms = backend.GetMaterialStats();
					if (!MaterialsWanted())
						ImGui::TextDisabled("Capture Materials: not available (the GPU driver did not accept the material shader, or reflections are switched off)");
					else {
						ImGui::Text("Materials: %u, textures %u copied / %u waiting (%u copies last frame), %u page(s) %s, attributes %s of %s, record %s", ms.materials,
							ms.texturesReady, ms.texturesPending, ms.copiesLastFrame, ms.pages, Megabytes(ms.textureBytes).c_str(), Megabytes(ms.attrBytesUsed).c_str(),
							Megabytes(ms.attrBytes).c_str(), backend.ReflectMaterialLastFrame() ? "written" : "not written");
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::Text("Registered materials (one per shape / ground quadrant, shared where texture and transform match), the 128x128 texture copies");
							ImGui::Text("  made and still waiting (the backlog after entering a new area), the copy pages (256 textures each; (48g) made on the");
							ImGui::Text("  render thread, last one in %.2f ms), the per-triangle data in use of its buffer, and whether last frame's rays wrote", ms.pageCreateMs);
							ImGui::Text("  the surface record. Pages made while playing (not at the switch-on / after a load): %u.", ms.pagesMadeInPlay);
							ImGui::Text("  Render-thread CPU of the copies last frame: %.2f ms. Surface record: %s. Material table: %s. Refused textures: %llu.",
								ms.copyCpuMs, Megabytes(backend.ReflectSurfaceBytes()).c_str(), Megabytes(ms.tableBytes).c_str(),
								static_cast<unsigned long long>(ms.texturesRefused));
						}
						if (ms.attrFull > 0 || ms.texturesFull > 0) {
							ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "Material buffers full: %llu shapes without data since switched on, %u textures without a copy layer",
								static_cast<unsigned long long>(ms.attrFull), ms.texturesFull);
							if (auto _tt = Util::HoverTooltipWrapper()) {
								ImGui::Text("(48g) Of the refused shapes, %llu found free memory only in pieces (no single range large enough). The last refused",
									static_cast<unsigned long long>(ms.attrRefusedFragmented));
								ImGui::Text("  range was %s with the largest free range %s. The buffer grows (64 -> 96 -> 128 MB) behind the next loading screen,",
									Megabytes(ms.attrLastRefusedBytes).c_str(), Megabytes(ms.attrLargestFreeBytes).c_str());
								ImGui::Text("  never while playing; those shapes get their data when their cell is captured again. Growths so far: %u.", ms.attrGrowths);
							}
						}
						// (48g) The shading half.
						if (settings.ReflectionsMode == 2) {
							if (!ShadeWanted()) {
								ImGui::TextDisabled("Off-screen shading: off (Off-screen Hit Color: Cubemap Approx)");
							} else if (reflectShadeFailed) {
								ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "Off-screen shading: unavailable (its shader did not compile or its textures could not be made)");
							} else if (!reflectShadeCS) {
								ImGui::TextDisabled("Off-screen shading: preparing its shader in the background...");
							} else {
								const float gpu = Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::RayTracingReflectShade);
								const std::string gpuText = gpu >= 0.0f && shadeLastFrame ? std::format("{:.2f} ms", gpu) : std::string("- ms");
								const std::string shareText = shadeMaterialShare >= 0.0f ? std::format("{:.0f} %", shadeMaterialShare * 100.0f) : std::string("-");
								ImGui::Text("Off-screen shading: %s GPU, %s of the hits with material data (rest: cubemap)", gpuText.c_str(), shareText.c_str());
								if (auto _tt = Util::HoverTooltipWrapper()) {
									ImGui::Text("GPU time of the hit shading pass (shown while the Performance Overlay or an F7 recording runs) and the share of the traced");
									ImGui::Text("  hits that had material data (texture copied, attributes captured) and so used their own colour.");
								}
								// (48h) Emission capture.
								ImGui::Text("Glow: %u emitting materials (%u follow a time-of-day source), %u with a glow texture (%u ready); %u glow textures, %u copy layers used",
									ms.emitMaterials, ms.emitSourced, ms.glowMaterials, ms.glowMaterialsReady, ms.glowTextures, ms.glowTexturesReady);
								if (auto _tt = Util::HoverTooltipWrapper()) {
									ImGui::Text("Material Hits: Glow's data (captured whether the switch is on or off): materials with a glow colour (own glow, External");
									ImGui::Text("  Emittance - windows whose colour follows a light / region as the time of day changes it - or a glow texture), the");
									ImGui::Text("  glow-texture ones and how many have their copy, the distinct glow textures and their 128x128 copies (layers in the");
									ImGui::Text("  same pages as the other textures). Colour refreshes that changed something so far: %llu. Switch: %s.",
										static_cast<unsigned long long>(ms.emitRefreshes), settings.ReflectionsShadeGlow ? "on" : "off");
								}
							}
						}
					}
				}
			} else {
				ImGui::TextDisabled("RT Reflections are off.");
			}
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	// In Custom the Custom values are the live ones.
	if (settings.ReflectionsQuality == kReflectionCustom)
		settings.ReflectionsCustom = CurrentReflectionValues();
	ImGui::PopID();
}

void RayTracing::DrawMasterHeader(bool a_switch)
{
	// (50b) The master checkbox only on the main "Ray Tracing" entry; the other entries show whether it is on (and the status).
	if (a_switch) {
		ImGui::Checkbox("Ray Tracing", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: sun shadows of buildings, rocks, furniture, doors, items and characters inside the loaded area are ray traced.");
			ImGui::Text("Trees, leaves, hair (unless changed under Objects in Ray Tracing), grass and the terrain itself (hills) keep their normal shadows;");
			ImGui::Text("  the ground still receives traced shadows.");
			ImGui::Text("Screen Space Shadows are switched off where ray tracing is in charge.");
			ImGui::Text("Off: the game's shadows exactly as before (nothing of this feature runs).");
		}
	} else {
		ImGui::TextDisabled("Ray Tracing: %s (the switch is on the Ray Tracing entry)", settings.Enabled ? "on" : "off");
	}
	const char* statusText = "Off";
	ImVec4 color{ 0.7f, 0.7f, 0.7f, 1.0f };
	switch (status) {
	case Status::Off:
		statusText = !settings.Enabled ? "Off" : (RtWanted() ? "Starting" : "Off (sun, point lights and RT AO are all Off)");
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
		// (47n) Off first (SunShadows false, QualityLevel kept), five tiers, Custom. Picking a tier never touches the Custom values.
		settings.QualityLevel = std::clamp(settings.QualityLevel, 0, static_cast<int>(Quality::Count) - 1);
		{
			int pick = settings.SunShadows ? settings.QualityLevel + 1 : 0;
			if (ImGui::Combo("Quality", &pick, kSunQualityNames, IM_ARRAYSIZE(kSunQualityNames))) {
				pick = std::clamp(pick, 0, static_cast<int>(Quality::Count));
				settings.SunShadows = pick != 0;
				if (pick != 0)
					settings.QualityLevel = pick - 1;
			}
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			if (!settings.SunShadows) {
				ImGui::Text("Off: the sun's shadows are the game's own (shadow cascades and Screen Space Shadows, exactly as with Ray Tracing off).");
				ImGui::Text("  Point-light shadows and RT ambient occlusion keep working. Saves the sun trace, its denoiser and the merge.");
			} else if (settings.QualityLevel < static_cast<int>(Quality::Custom)) {
				ImGui::Text("%s", kTiers[settings.QualityLevel].tooltip);
			} else {
				ImGui::Text("Custom: the values under Advanced.");
			}
			ImGui::Text("Changing a value under Advanced switches to Custom (starting from the tier shown); picking a tier keeps your Custom values.");
		}

		if (settings.SunShadows) {
			// Denoiser: Off / Penumbra / DLSS RR / NRD SIGMA (stored values 0 / 2 / 3 / 4).
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
				OnDenoiserPicked(false);  // (50b) DLSS RR: one choice for the sun and the point lights
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How the grainy soft edges of the traced shadows are cleaned up.");
				ImGui::Text("  Off: only a small blur.");
				ImGui::Text("  Penumbra (default): blurs each soft edge as wide as it really is (wide under tall trees, none at");
				ImGui::Text("    contact shadows), then blends the last ~12 frames; extra rays only at shadow edges.");
				ImGui::Text("  DLSS RR: no cleanup of ours; DLSS Ray Reconstruction cleans up the raw noise.");
				ImGui::Text("    Picking it switches Ray Reconstruction on in Upscaling; picking any other one switches it off there.");
				ImGui::Text("    (50b) One choice for the sun and the point lights: picking it here sets Point Light Shadows > Denoiser to DLSS RR too;");
				ImGui::Text("    picking another one here returns the point lights to their last choice. RT ambient occlusion keeps its own denoiser.");
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
			if (!typedUavLoadR8)
				ImGui::TextDisabled("This GPU cannot rewrite Screen Space Shadows; they are kept as with Ray Tracing off.");
			// (47q) A look slider of its own, outside the tiers and Custom.
			ImGui::SliderFloat("Shadow Edge Softness (sun size)", &settings.SunSize, kSunSizeMin, kSunSizeMax, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How big the sun looks to the shadows (degrees, half angle). 0.27 (default) = the real sun: sharp contact edges that");
				ImGui::Text("  widen with the distance to the caster. Larger = softer, wider shadow edges everywhere (until 47p High and Ultra used 0.5).");
				ImGui::Text("  Not part of the quality tiers: picking a tier keeps it.");
			}
			// (47r) Sun: Fast Path for Uniform Tiles and Screen Space Shadows: Tile Mask for RT Casters: (48o) fixed on (no switches).
		} else {
			ImGui::TextDisabled("Sun shadows: the game's own. Objects in Ray Tracing (main page) still decide what blocks point lights and RT AO.");
		}

		// Advanced (collapsed): every value of the quality in effect (editing one switches to Custom, started from the tier shown).
		if (ImGui::TreeNodeEx("Advanced")) {
			ImGui::BeginDisabled(!settings.SunShadows);
			const QualityParams q = ResolveQuality();
			const bool custom = settings.QualityLevel == static_cast<int>(Quality::Custom);
			// The values in effect, in the Custom keys' form.
			int baseRays = static_cast<int>(q.baseRays), extraRays = static_cast<int>(q.extraRays);
			int traceRes = static_cast<int>(q.tracePattern), farQuality = static_cast<int>(q.farPattern);
			float falloffStart = q.falloffStart, falloffEnd = q.falloffEnd, farScale = q.farDenoiseScale;
			int npc = q.npcReduced ? 1 : 0;
			float radius = q.denoiseRadius;
			int taps = static_cast<int>(q.denoiseTaps);
			bool half = q.denoiseHalf;
			int history = static_cast<int>(q.history);
			float clampSigma = q.clampSigma;
			float npcFull = q.npcFullDistance;
			bool changed = false;
			changed |= ImGui::SliderInt("Rays per Pixel", &baseRays, 1, 4);
			changed |= ImGui::SliderInt("Extra Rays at Shadow Edges", &extraRays, 0, 8);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Extra rays only where a shadow edge is (cleaner soft edges). 0 = none.");
			changed |= ImGui::Combo("Trace Resolution", &traceRes, kPatternNames, 3);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Full: every pixel traced every frame. Checkerboard: half of them, alternating each frame. Half: one pixel of each 2x2, rotating.");
				ImGui::Text("  The others are filled from their traced neighbours (and the denoiser's history averages the frames).");
			}
			changed |= ImGui::SliderFloat("Fall-off Start", &falloffStart, 0.0f, 16384.0f, "%.0f units");
			changed |= ImGui::SliderFloat("Fall-off End", &falloffEnd, 0.0f, 32768.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("From the start to the end distance the extra rays fade out and the denoiser blur shrinks to Far Blur Scale;");
				ImGui::Text("  beyond the end: 1 ray per pixel and the Far Quality pattern. 70 units = 1 m. End at or below the start = no fall-off.");
			}
			changed |= ImGui::Combo("Far Quality", &farQuality, kPatternNames, 3);
			changed |= ImGui::SliderFloat("Far Blur Scale", &farScale, 0.0f, 1.0f, "%.2f");
			const char* npcModes[] = { "Full", "Reduced" };
			changed |= ImGui::Combo("Other Characters", &npc, npcModes, 2);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("The player always gets full quality. Reduced: other characters get no extra edge rays.");
			changed |= ImGui::SliderFloat("Other Characters: All Parts Within", &npcFull, 0.0f, 8192.0f, npcFull > 0.0f ? "%.0f units" : "no limit");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Other characters farther than this from the camera are traced with their body and armour only (one merged shape):");
				ImGui::Text("  capes / skirts with physics, hair and cut-out parts keep the game's shadow-map shadow, and the body is re-posed every");
				ImGui::Text("  other frame (characters take turns). Closer ones and the player: every part, every frame. 0 = no limit (Ultra).");
				ImGui::Text("  Very Low 700 (10 m), Low 1400 (20 m), Medium 2450 (35 m), High 3150 (45 m). Characters are traced up to 40 m at most.");
				ImGui::Text("  With Sun Shadows Off the point-light quality decides (then RT AO's): Low 20 m, Medium 35 m, High 45 m, Ultra / Custom none.");
			}
			changed |= ImGui::SliderFloat("Denoiser Radius", &radius, 2.0f, 32.0f, "%.0f px");
			changed |= ImGui::SliderInt("Denoiser Taps per Side", &taps, 1, 8);
			changed |= ImGui::Checkbox("Denoiser at Half Resolution", &half);
			changed |= ImGui::SliderInt("History Length", &history, 1, 32, "%d frames");
			changed |= ImGui::SliderFloat("History Clamp", &clampSigma, 0.5f, 4.0f, "%.2f sigma");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How far the history may differ from this frame's neighbourhood: smaller = less ghosting, more grain.");
			if (changed) {
				// (47n) An edit: Custom, seeded from the tier in effect, then the edited value (only the one that differs).
				if (!custom) {
					CopyTierToCustom(settings.QualityLevel);
					settings.QualityLevel = static_cast<int>(Quality::Custom);
				}
				const QualityParams seeded = ResolveQuality();
				if (npcFull != seeded.npcFullDistance)
					settings.CustomNpcFullDistance = std::clamp(npcFull, 0.0f, 8192.0f);
				if (baseRays != static_cast<int>(seeded.baseRays))
					settings.CustomBaseRays = std::clamp(baseRays, 1, 4);
				if (extraRays != static_cast<int>(seeded.extraRays))
					settings.CustomExtraRays = std::clamp(extraRays, 0, 8);
				if (traceRes != static_cast<int>(seeded.tracePattern))
					settings.CustomTraceResolution = std::clamp(traceRes, 0, 2);
				if (farQuality != static_cast<int>(seeded.farPattern))
					settings.CustomFarQuality = std::clamp(farQuality, 0, 2);
				if (falloffStart != seeded.falloffStart)
					settings.CustomFalloffStart = std::clamp(falloffStart, 0.0f, 32768.0f);
				if (falloffEnd != seeded.falloffEnd)
					settings.CustomFalloffEnd = std::clamp(falloffEnd, 0.0f, 65536.0f);
				if (farScale != seeded.farDenoiseScale)
					settings.CustomFarDenoiseScale = std::clamp(farScale, 0.0f, 1.0f);
				if ((npc == 1) != seeded.npcReduced)
					settings.CustomNpcQuality = std::clamp(npc, 0, 1);
				if (radius != seeded.denoiseRadius)
					settings.CustomDenoiseRadius = std::clamp(radius, 2.0f, 32.0f);
				if (taps != static_cast<int>(seeded.denoiseTaps))
					settings.CustomDenoiseTaps = std::clamp(taps, 1, 8);
				if (half != seeded.denoiseHalf)
					settings.CustomDenoiseHalf = half;
				if (history != static_cast<int>(seeded.history))
					settings.CustomHistory = std::clamp(history, 1, 32);
				if (clampSigma != seeded.clampSigma)
					settings.CustomDenoiseClamp = std::clamp(clampSigma, 0.5f, 4.0f);
			}
			ImGui::Separator();
			// (47y) Smooth Blur Width Between Tiles and (47z) Keep Buildings in the Game's Sun Shadow Map: (48o) fixed on (no switches).
			ImGui::SliderFloat("RT Caster Contact Length", &settings.RtContactScale, 0.0f, 32.0f, "%.1f x ray offset");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How far (along the sun ray) a ray-traced object may still cast a small screen-space contact shadow at its base,");
				ImGui::Text("  where the ray tracer cannot resolve it. Default 8 = about 16 units near the camera. 0 = none.");
				ImGui::Text("  Larger = softer bases, but thin posts get fatter. Everything the ray tracer does not see keeps its contact shadows.");
			}
			ImGui::EndDisabled();
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
	ImGui::PopID();
}

void RayTracing::DrawObjectRows()
{
	// (47n) What the ray tracer takes over, per kind of object: one choice for the sun, the point lights and RTAO (one scene, one
	// TLAS). Moved here unchanged from Sun Shadows > Advanced.
	if (!ImGui::TreeNodeEx("Objects in Ray Tracing", ImGuiTreeNodeFlags_DefaultOpen))
		return;
	ImGui::TextWrapped("What the ray tracer takes over, per kind of object, for the sun, the point lights and RT ambient occlusion alike. "
					   "\"Shadow map\" = not ray traced: the sun's shadow comes from the game's shadow cascades and Screen Space Shadows, a point "
					   "light's from Screen Space Point Light Shadows (the game's own shadow-casting lights keep their shadow maps). Changing a row "
					   "re-captures the scene over the next frames.");
	if (!alphaOffReason.empty())
		RtErrorLine("Ray-traced foliage / cut-outs were switched off automatically", alphaOffReason);
	if (!charactersOffReason.empty())
		RtErrorLine("Characters was switched off automatically", charactersOffReason);
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
	{
		// (50b) Trees + Foliage merged: Shadow map (Trees off, Foliage 0) / Trunks only (on, 0) / Trunks and leaves (on, 1) / Both (on, 2).
		const char* plantModes[] = { "Shadow map", "Trunks only", "Trunks and leaves", "Both" };
		settings.Foliage = std::clamp(settings.Foliage, 0, 2);
		int plants = settings.Trees ? settings.Foliage + 1 : 0;  // a saved Trees-off / Foliage-on pair loads with Trees on (LoadSettings)
		if (ImGui::Combo("Trees and Plants", &plants, plantModes, 4)) {
			plants = std::clamp(plants, 0, 3);
			settings.Trees = plants != 0;
			settings.Foliage = plants == 0 ? 0 : plants - 1;
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Trees and plants (default: Shadow map).");
			ImGui::Text("  Shadow map: trunks, branches and leaves keep the game's shadows and their screen-space contact shadows.");
			ImGui::Text("  Trunks only: trunks and branches ray traced; leaves, bushes and plants with a cut-out keep the game's shadows.");
			ImGui::Text("  Trunks and leaves: the leaves are traced with their cut-out too (sharper; they sway with Sway in RT Shadows on).");
			ImGui::Text("  Both: trunks and leaves traced AND the leaves kept in the shadow map; the darker of both wins (fullest shadow).");
			ImGui::Text("(50b: was two rows, Trees and Foliage.)");
		}
		// (47l) Foliage sway; (47n) per plant, with each plant's own wind.
		ImGui::Indent();
		ImGui::BeginDisabled(settings.Foliage == static_cast<int>(RayTracingScene::FoliageMode::ShadowMap));
		ImGui::Checkbox("Sway in RT Shadows", &settings.FoliageSway);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): ray-traced leaves, bushes, swaying branches and banners move with the wind in their shadow, each plant");
			ImGui::Text("  exactly as the game moves it (its own wind timer and strength, every frame), for the sun, point lights and RT AO.");
			ImGui::Text("  Costs GPU time (F7: rt_wind_* columns, rt_prep_gpu_ms). Off: they are traced at rest.");
			ImGui::Text("  No effect while the leaves are not ray traced (Trees and Plants: Shadow map or Trunks only).");
		}
		ImGui::EndDisabled();
		// (47q) Keep Leaves Moving on Long Frames, (47r) Learn on Any Frame: (48o) fixed on (no switches). The sway stats line is in
		// Developer (main page).
		if (!windOffReason.empty())
			RtErrorLine("Sway in RT Shadows was switched off automatically", windOffReason);
		ImGui::Unindent();
	}
	row("Cut-outs", settings.Cutouts, "Fences, grilles, static (non-animated) banners and other non-plant cut-outs (default: Shadow map).",
		"Ray traced: traced with their cut-out (the holes stay lit). (47h) Blended-only shapes with a fully opaque material (an arch, a trellis) are traced as cut-outs at half alpha.");
	row("Characters", settings.Characters, "The player and nearby NPCs and creatures: body, clothes, armour, weapons (default: Ray traced).",
		"Up to 16 characters within 40 m inside the ray-traced area, posed every frame.");
	{
		ImGui::Indent();
		ImGui::BeginDisabled(!settings.Characters);
		row("Physics parts", settings.PhysicsParts, "Capes, skirts and other HDT-SMP / HDT-PE parts, not hair (default: Ray traced).", nullptr);
		row("Hair", settings.Hair, "Hair and wigs (default: Shadow map: the hair shader's own shadowing looks better than solid cards).", nullptr);
		// (49h) Hair: Ignore Faint Strands in RT: (50b) retired, always on.
		ImGui::Checkbox("Player in first person", &settings.FirstPersonPlayer);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Off (default): in first person the player keeps the shadow-map shadow (camera mods can hide the arms");
			ImGui::Text("  from a traced shadow). On: the player is ray traced in first person too.");
		}
		// (49m) Refresh Changed Character Shapes: (50b) retired, always on.
		ImGui::EndDisabled();
		ImGui::Unindent();
	}
	// (47u) RT: Start Sun Trace Earlier, RT: Skip Updates That Cannot Affect the Image: (48o) fixed on; RT: Spatial Static Chunks:
	// fixed off (no switches). The skipped-updates stats line is in Developer (main page).
	ImGui::TreePop();
}

void RayTracing::DrawPointLightPage()
{
	ImGui::PushID("rtPointPage");
	DrawMasterHeader();
	if (ImGui::TreeNodeEx("Point Lights", ImGuiTreeNodeFlags_DefaultOpen)) {
		// (47n) Off / Low / Medium / High / Ultra / Custom (Off = PointLights false; PointQuality kept).
		{
			int pick = settings.PointLights ? std::clamp(settings.PointQuality, 0, kTierCustom) + 1 : 0;
			if (ImGui::Combo("Quality##Point", &pick, kTierNames, IM_ARRAYSIZE(kTierNames))) {
				pick = std::clamp(pick, 0, kTierCustom + 1);
				settings.PointLights = pick != 0;
				if (pick != 0)
					settings.PointQuality = pick - 1;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Braziers, campfires, torches, candles, lanterns and the like cast ray-traced shadows (outdoors inside the traced area, and indoors).");
				ImGui::Text("  Window fill / glow lights and fill lights are left as they are.");
				if (!settings.PointLights)
					ImGui::Text("Off: point lights exactly as before. Needs Ray Tracing on.");
				else if (settings.PointQuality < kTierCustom)
					ImGui::Text("%s", kPointTiers[settings.PointQuality].tooltip);
				else
					ImGui::Text("Custom: the values under Advanced.");
				ImGui::Text("Changing a value under Advanced switches to Custom (starting from the tier shown); picking a tier keeps your Custom values.");
			}
		}
		ImGui::BeginDisabled(!settings.PointLights);
		{
			// (50b) Denoiser: Off / Penumbra / Penumbra v2 / DLSS RR (stored as PointDenoiserMode 0 - 3). DLSS RR is one choice with the sun's.
			const char* kPointDenoisers[kPointModeCount] = { "Off", "Penumbra", "Penumbra v2", "DLSS RR" };
			int pick = std::clamp(settings.PointDenoiser, 0, kPointModeCount - 1);
			if (ImGui::Combo("Denoiser##Point", &pick, kPointDenoisers, kPointModeCount)) {
				settings.PointDenoiser = std::clamp(pick, 0, kPointModeCount - 1);
				OnDenoiserPicked(true);
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How the grainy point-light shadows (one ray per light and pixel) are cleaned up.");
				ImGui::Text("  Off: only a 3x3 blur, no history (grainy; a reference).");
				ImGui::Text("  Penumbra: the denoiser of 50a (blurs each soft edge as wide as it is, blends the last frames).");
				ImGui::Text("  Penumbra v2 (default): knows how many frames each pixel has gathered. Where a shadow just appeared or changed it blurs");
				ImGui::Text("    wider (never wider than the real soft edge: contact shadows stay sharp) and catches up within a few frames, so no");
				ImGui::Text("    black / white specks; a still picture looks as with Penumbra.");
				ImGui::Text("  DLSS RR: nothing of ours; DLSS Ray Reconstruction cleans the raw rays. One choice for the sun and the point lights:");
				ImGui::Text("    picking it here sets the sun's denoiser to DLSS RR too and switches Ray Reconstruction on in Upscaling; picking");
				ImGui::Text("    anything else on either page returns the other page to its last choice. While Ray Reconstruction is on in");
				ImGui::Text("    Upscaling, both always take this route. Smoke and other lit effects keep a cheap filtered copy (not the raw rays).");
				ImGui::Text("RT ambient occlusion always keeps its own denoiser (it darkens the ambient light: it must be filtered in the same frame).");
			}
			const int effective = EffectivePointDenoiser();
			if (effective == kPointRR && settings.PointDenoiser != kPointRR) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 0.5f, 0.85f, 1.0f, 1.0f });
				ImGui::TextWrapped("Following Upscaling: Ray Reconstruction is on, so the point-light shadows use the DLSS RR route.");
				ImGui::PopStyleColor();
			} else if (settings.PointDenoiser == kPointRR && !UpscalingRRActive()) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.85f, 0.3f, 1.0f });
				ImGui::TextWrapped("Ray Reconstruction is not running (Upscaling must be DLSS with Ray Reconstruction available): the point-light shadows stay grainy.");
				ImGui::PopStyleColor();
			}
			if (pointV2Failed)
				ImGui::TextColored({ 1.0f, 0.4f, 0.3f, 1.0f }, "Penumbra v2 / Off / DLSS RR could not start this session (see the log): Penumbra is used.");
			ImGui::Checkbox("Blue-Noise Sampling", &settings.PointBlueNoise);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): the point on the flame each ray aims at comes from a spatiotemporal blue-noise pattern (spread evenly at every");
				ImGui::Text("  scale and from frame to frame), so the same one ray per pixel leaves noise the denoiser (or DLSS RR) removes more easily.");
				ImGui::Text("  Practically free. Off: the 50a pattern. Needs the blue-noise texture of Skylighting (installed with it).");
				if (backend.PointBlueNoiseFailed())
					ImGui::Text("Not available this session (the texture could not be loaded): the 50a pattern is used.");
				ImGui::Text("With the DLSS RR route and RR: White-Noise Sampling on, the white noise is used instead.");
			}
			// (50e) RR: White-Noise Sampling (TracePointLightsCS kFlagWhiteNoise, only on the DLSS RR route).
			ImGui::Checkbox("RR: White-Noise Sampling", &settings.PointRRWhiteNoise);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Only while the point lights take the DLSS RR route. On (default): the point on the flame each ray aims at is picked");
				ImGui::Text("  independently per pixel, frame and light (white noise), as Ray Reconstruction wants its input: no screen-space pattern,");
				ImGui::Text("  nothing shared between lights. Off: Blue-Noise Sampling (or the 50a pattern) as with the other denoisers.");
				ImGui::Text("Penumbra, Penumbra v2 and Off are not affected (the blue noise suits them). Practically free.");
			}
			// (50e) Moving Characters: Clean Shadow History for Penumbra v2 (PointDenoiseV2CS PL_CLEAN).
			ImGui::Checkbox("Moving Characters: Clean Shadow History", &settings.PointV2CleanHistory);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Penumbra v2 (and the DLSS RR route's copy for smoke and other lit effects). On (default): a surface that moves by itself");
				ImGui::Text("  (a running character's legs and arms; the game's motion vectors) takes its last frame only from where it really was,");
				ImGui::Text("  with a strict match, else starts fresh with v2's wide, smooth estimate; the blur keeps to the same surface; the old");
				ImGui::Text("  result counts at most as much as this frame; a carried torch that moves lets its old shadow go faster. No more white");
				ImGui::Text("  smears trailing on legs and arms. Off: Penumbra v2 exactly as in 50d.");
				ImGui::Text("Penumbra (the old denoiser) always has its own version of this. Debug View \"Point Lights: Moving Pixels\" shows");
				ImGui::Text("  which pixels count as moving. Practically free.");
				if (pointV2CleanFailed)
					ImGui::Text("Not available this session (its shader did not compile): Penumbra v2 runs without it.");
			}
		}
		ImGui::SliderFloat("Light Size", &settings.PointLightsSize, 0.0f, 32.0f, "%.1f units");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Radius of the flame the rays aim at: larger = softer shadow edges (a little grainier before the clean-up). 0 = hard edges.");

		ImGui::SeparatorText("Indoors");
		ImGui::Checkbox("Indoors Too", &settings.PointLightsInteriors);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On (default): inside houses, inns, castles and dungeons the room itself (walls, furniture, clutter, people) is captured");
			ImGui::Text("  when you come in (behind the loading screen) and the lights cast ray-traced shadows there too.");
			ImGui::Text("Off: indoors the lights' shadows are exactly as before (RT Ambient Occlusion has its own Indoors Too).");
		}
		ImGui::BeginDisabled(!settings.PointLightsInteriors || !settings.PointLightsWindowBeams);  // (50b) Window Light Beams: in Diagnostics
		ImGui::SliderFloat("Window Beam Softness", &settings.PointLightsBeamSoftness, 0.0f, 3.0f, "%.2f deg", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How wide the sun looks through the window (degrees, like the sun's own size): soft edges on the beam's window pattern,");
			ImGui::Text("  growing with the distance to the window frame. 0 = hard, grainy pattern.");
		}
		// (48a) Window Beams: Smooth Edges: (48o) fixed on (no switch).
		ImGui::EndDisabled();
		if (!interiorOffReason.empty()) {
			RtErrorLine("Indoors was switched off automatically", interiorOffReason);
			// The saved setting stays on (the next session tries again); Retry tries now.
			if (interiorAutoOff && ImGui::SmallButton("Retry Indoors"))
				interiorRequested = false;
		}

		ImGui::SeparatorText("Which Lights");
		{
			// (47q) Tier knobs (the values in effect; an edit switches to Custom, seeded from the tier shown). Torches carried by other
			// characters are always ray traced.
			const PointTierParams cur = ResolvePointTier();
			bool player = cur.player, effects = cur.effects;
			bool changed = ImGui::Checkbox("Player Lights Cast RT Shadows", &player);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("The torch in your hand, Quick Light's light and light spells on you cast ray-traced shadows (Medium and above).");
				ImGui::Text("  Off (Low): they keep the old shadows.");
			}
			changed |= ImGui::Checkbox("Spell / Effect Lights Cast RT Shadows", &effects);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Lights of spells and effects that belong to nobody (Magelight on a wall, fire bolts in flight) cast ray-traced");
				ImGui::Text("  shadows (High and Ultra). Off (Low, Medium): they keep the old shadows.");
				ImGui::Text("Torches carried by other characters always cast ray-traced shadows.");
			}
			if (changed) {
				if (settings.PointQuality != kTierCustom) {
					CopyPointTierToCustom(settings.PointQuality);
					settings.PointQuality = kTierCustom;
				}
				settings.PointLightsPlayer = player;
				settings.PointLightsEffects = effects;
			}
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
				// (47n) The tier's values in effect; an edit switches to Custom, seeded from the tier shown, then the edited value.
				const PointTierParams cur = ResolvePointTier();
				PointTierParams v = cur;
				const char* kNames[6] = { "1", "2", "3", "4", "5", "6" };  // (48a) up to 6
				bool changed = false;
				int index = std::clamp(v.k, 1, 6) - 1;
				if (ImGui::Combo("Lights per Pixel", &index, kNames, 6)) {
					v.k = std::clamp(index, 0, 5) + 1;
					changed = true;
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("How many of the lights that light a pixel most are traced there (one ray each). High: 2.");
					ImGui::Text("  Weaker lights at that pixel keep their old shadows. More = slower. 5 or 6 take extra video memory (about 16 bytes");
					ImGui::Text("  per pixel at the render resolution, freed again after a while at 4 or less).");
				}
				ImGui::BeginDisabled(!settings.PointLightsInteriors);
				int indexInterior = std::clamp(v.kInterior, 1, 6) - 1;
				if (ImGui::Combo("Lights per Pixel Indoors", &indexInterior, kNames, 6)) {
					v.kInterior = std::clamp(indexInterior, 0, 5) + 1;
					changed = true;
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Indoors many lights overlap: Medium 3, High 4, Ultra 6 catch nearly all of the light (crowded halls such as the Blue Palace");
					ImGui::Text("  gain most from 4 and up). 5 or 6 trace up to 50 %% more rays than 4 and take extra video memory. 2 or 1 = cheaper.");
				}
				ImGui::EndDisabled();
				// (48c) Extra Lights Where Crowded: (48o) fixed on (no switch).
				changed |= ImGui::SliderInt("Max Lights", &v.maxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources));
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Shadow-casting lights per frame at most (the ones that matter most on screen). Lights within 64 units count as one.");
				changed |= ImGui::SliderFloat("Max Distance", &v.distance, 512.0f, 8192.0f, "%.0f units");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("Lights farther from the camera keep their old shadows. 70 units = 1 m. High: 3500 (about 50 m).");
				changed |= ImGui::Checkbox("Plants and Cut-Outs Block Point Lights Outdoors", &v.alphaOutdoors);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On (High and Ultra): at night leaves, bushes and cut-out shapes (fences, nets) cast shadows from torches and campfires,");
					ImGui::Text("  for the kinds traced with their cut-out (Foliage / Cut-outs under Objects in Ray Tracing). A little slower.");
					ImGui::Text("Off: outdoors only solid shapes block point lights.");
				}
				if (changed) {
					if (settings.PointQuality != kTierCustom) {
						CopyPointTierToCustom(settings.PointQuality);
						settings.PointQuality = kTierCustom;
					}
					if (v.k != cur.k)
						settings.PointLightsPerPixel = ValidPointK(v.k);
					if (v.kInterior != cur.kInterior)
						settings.PointLightsPerPixelInterior = ValidPointK(v.kInterior);
					if (v.maxSources != cur.maxSources)
						settings.PointLightsMaxSources = std::clamp(v.maxSources, 8, static_cast<int>(RayTracingPointLights::kMaxSources));
					if (v.distance != cur.distance)
						settings.PointLightsDistance = std::clamp(v.distance, 512.0f, 8192.0f);
					if (v.alphaOutdoors != cur.alphaOutdoors)
						settings.PointLightsAlphaOutdoors = v.alphaOutdoors;
				}
			}
			// (50b) Retired (fixed at the saved values): Fixture Clearance 20, Carrier Clearance 28, Non-RT Casters: RT Caster Contact Length 8,
			// Close-Contact Shadows on Characters on, Smooth Hard Edges on, Steady Edges on, Dark Side Test Tolerant, Grazing-Angle Offset on,
			// Soft Hand-Off for Weak Lights on; Smooth Shadow Edges (extra rays) deleted.
			// (48o) Fixed on, no switches any more: (47r) Reuse the Sun's Surface Probe Everywhere; (47x) Smoother Shadow Edges, Trace Lights on
			// Curved Surfaces Reliably, Close-Contact Shadows on Characters; (48m) Moving Characters: Clean Shadow History; (48k) Point Lights:
			// Noise Fix with Decorrelated Sampling, Stable Penumbra, Close Blockers: Minimum Blur ((49m) Consistent Dark Side: Dark Side Test above); (47z) Window Beams:
			// Respect Their Range, Stable Light Choice When the Camera Moves, (48c) its Jitter Fix. Fixed off: (46c) Skip the Game's Shadow
			// Maps for RT-Shadowed Lights and its (47w) Adaptive child.
			if (pointCleanFailed)
				ImGui::TextDisabled("Moving Characters: Clean Shadow History: its shader variant did not compile this session (off).");
			if (pointNoiseFixFailed)
				ImGui::TextDisabled("Point Lights: Noise Fix: its denoiser variant did not compile this session (only its ray parts act).");
			ImGui::EndDisabled();
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Diagnostics")) {
			// (50b) Moved here from the page: Window Light Beams (from Indoors) and (50a) Lights Without a Visible Flame (from Which Lights).
			ImGui::BeginDisabled(!settings.PointLights);
			ImGui::BeginDisabled(!settings.PointLightsInteriors);
			ImGui::Checkbox("Window Light Beams", &settings.PointLightsWindowBeams);
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): Window Shadows Ultimate's beams of sunlight through the windows (and the game's other spot lights");
				ImGui::Text("  indoors) get a ray-traced shadow, the window frames give the pattern on the floor. Off: they keep the game's shadow maps.");
				ImGui::Text("  Needs Indoors Too. Window Beam Softness (on the page) applies while this is on.");
			}
			{
				// (50a) "Lights Without a Visible Flame" (was "Meshless Lights Near Fires"): shown Soft, Own, Skip, Snap (old); stored as the
				// selector's mode (RayTracingPointLights::kMeshless*: 0 Own, 1 Snap, 2 Skip, 3 Soft).
				const char* kFlameless[4] = { "Soft", "Own", "Skip", "Snap (old)" };
				constexpr int kModeOfItem[4] = { 3, 0, 2, 1 };
				constexpr int kItemOfMode[4] = { 1, 3, 2, 0 };
				int item = kItemOfMode[std::clamp(settings.PointLightsMeshless, 0, 3)];
				if (ImGui::Combo("Lights Without a Visible Flame", &item, kFlameless, 4))
					settings.PointLightsMeshless = kModeOfItem[std::clamp(item, 0, 3)];
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Some placed lights have nothing visible that burns: an invisible light near braziers and fires that stands for");
					ImGui::Text("  the fire's light bouncing off the surroundings.");
					ImGui::Text("  Soft (default): its shadow comes from where its light comes from (its own spot), but from a big, soft light:");
					ImGui::Text("    as big as the distance to the nearest visible fire or lamp (up to about 4.5 m). A broad, soft darkening");
					ImGui::Text("    instead of a sharp extra shadow.");
					ImGui::Text("  Own: a sharp shadow from its own spot (an extra shadow between two braziers).");
					ImGui::Text("  Skip: no ray-traced shadow for these lights (the old screen-space shadow, if any).");
					ImGui::Text("  Snap (old): the shadow comes from the nearest visible fire instead. Known to break the shadows on characters");
					ImGui::Text("    into dark patches; kept for comparison only.");
					ImGui::Text("A light right next to a candle / torch holder / brazier model is that model's own light and keeps a normal shadow.");
				}
			}
			ImGui::EndDisabled();
			ImGui::Separator();
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
				// (50a)
				ImGui::Text("Lights without a visible flame (data test): %u; not picked by the old name rule %u, picked only by it %u; "
							"Soft: %u with a fire / lamp in range, %u without, largest emitter %.0f units",
					ps.fillLights, ps.fillNewVsName, ps.fillNameOnly, ps.fillSoftNear, ps.fillSoftFar, ps.fillEmitterMax);
				// (46x)
				ImGui::Text("Fill lights: %u direct traced (%u aimed at a window spot, %u soft point), %u taken as fixtures, %u left untraced; "
							"negative lights %u; beams by the wider rule %u",
					ps.fillDirectAimed + ps.fillDirectPoint, ps.fillDirectAimed, ps.fillDirectPoint, ps.fillFixture, ps.fillAmbient, ps.negative, ps.beamsWider);
				if (Util::IsInterior()) {
					if (!ps.cameraInRoom)
						ImGui::Text("Portals: no room bounds around the camera (nothing changed this frame)");
					else
						ImGui::Text("Portals: %u room-bound lights, %u behind portals (%u not traced), %u limited to their rooms' box; %u rooms unreadable (left as 46w)",
							ps.portalStrict, ps.portalAway, ps.portalDropped, ps.portalBoxed, ps.portalBadRooms);
				}
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Counted among the lights that passed the distance / radius / brightness rules. An F6 census lists each light's class");
					ImGui::Text("  (rtClass / rtWhy in the file).");
				}
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
		// (47n) Off / Low / Medium / High / Ultra / Custom (Off = AO false; AOQuality kept).
		{
			int pick = settings.AO ? std::clamp(settings.AOQuality, 0, kTierCustom) + 1 : 0;
			if (ImGui::Combo("Quality##AO", &pick, kTierNames, IM_ARRAYSIZE(kTierNames))) {
				pick = std::clamp(pick, 0, kTierCustom + 1);
				settings.AO = pick != 0;
				if (pick != 0)
					settings.AOQuality = pick - 1;
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Ray-traced ambient occlusion (outdoors inside the traced area, and indoors with Indoors Too): the soft darkening in");
				ImGui::Text("  corners, under ledges and around objects comes from short rays against the ray-traced scene, so it no longer");
				ImGui::Text("  disappears when whatever casts it leaves the screen. Outdoors, farther away it fades back to Screen Space GI's AO.");
				ImGui::Text("Terrain and grass are not in the ray-traced scene: their darkening still comes from Screen Space GI.");
				if (!settings.AO)
					ImGui::Text("Off: ambient occlusion exactly as before (Screen Space GI). Needs Ray Tracing and Screen Space GI on.");
				else if (settings.AOQuality < kTierCustom)
					ImGui::Text("%s", kAOTiers[settings.AOQuality].tooltip);
				else
					ImGui::Text("Custom: the values under Advanced.");
				ImGui::Text("Changing a value under Advanced switches to Custom (starting from the tier shown); picking a tier keeps your Custom values.");
			}
		}
		ImGui::BeginDisabled(!settings.AO);
		ImGui::SliderFloat("Strength", &settings.AOStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("1 = as traced. Lower = lighter, higher = darker.");
			ImGui::Text("The broad darkening only (under roofs, along walls, around large objects); the small creases have their own");
			ImGui::Text("  Detail Strength.");
		}
		ImGui::SliderFloat("Detail Strength", &settings.AODetailStrength, 0.0f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("The fine darkening right next to surfaces: stone joints, where a door frame meets the wall, between ivy layers,");
			ImGui::Text("  under small objects. 1 (default) = as traced; higher = deeper creases without darkening whole areas.");
		}
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

		// (48o) Fixed on, no switches any more: (47n) RTAO: Tight History Near Moving Things and SSGI Contact AO: Uncovered-Area Test,
		// 4-Frame History; (47r) RTAO: Filter Only Where Traced.
		// (50b) Mode (outdoors) and Mode Indoors merged into one choice (it writes both keys). A saved pair that differs shows "Mixed".
		ImGui::BeginDisabled(!settings.AO);
		{
			const char* modes[] = { "RTAO (keeps SSGI contact AO)", "Darker of RTAO and SSGI AO" };
			settings.AOMode = std::clamp(settings.AOMode, 0, static_cast<int>(AOModeKind::Count) - 1);
			settings.AOModeInterior = std::clamp(settings.AOModeInterior, 0, static_cast<int>(AOModeKind::Count) - 1);
			const bool mixed = settings.AOMode != settings.AOModeInterior;
			const std::string preview = mixed ? std::format("Mixed (outdoors: {}, indoors: {})", modes[settings.AOMode], modes[settings.AOModeInterior]) :
			                                    std::string(modes[settings.AOMode]);
			if (ImGui::BeginCombo("Combine with SSGI AO", preview.c_str())) {
				for (int i = 0; i < IM_ARRAYSIZE(modes); ++i) {
					if (ImGui::Selectable(modes[i], !mixed && settings.AOMode == i)) {
						settings.AOMode = i;
						settings.AOModeInterior = i;
					}
				}
				ImGui::EndCombo();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How the ray-traced AO joins Screen Space GI's AO, outdoors and indoors alike.");
				ImGui::Text("RTAO (default): ray-traced AO replaces Screen Space GI's AO; SSGI's small contact AO (cm-scale creases) is kept and both");
				ImGui::Text("  darken together (each crease counted once).");
				ImGui::Text("Darker of RTAO and SSGI AO: per pixel the darker of the two (fullest darkening; SSGI's AO is never lost; may bring");
				ImGui::Text("  back some fine crease darkening).");
				ImGui::Text("(50b: was Mode and Mode Indoors.)");
			}
		}
		ImGui::EndDisabled();

		if (ImGui::TreeNodeEx("Advanced")) {
			ImGui::BeginDisabled(!settings.AO);
			{
				// (47n) The tier's values in effect; an edit switches to Custom, seeded from the tier shown, then the edited value.
				const AOTierParams cur = ResolveAOTier();
				AOTierParams v = cur;
				bool changed = false;
				changed |= ImGui::SliderInt("Detail Rays", &v.detailRays, 1, kAODetailRaysMax);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Short rays per pixel for the fine detail (the long ray's first part is one of them). Low - High 1, Ultra 3.");
					ImGui::Text("  More = smoother creases, costs more GPU time (about +20 %% of the RTAO rays' time per ray).");
				}
				changed |= ImGui::SliderFloat("Max Distance", &v.distance, 1000.0f, 8192.0f, "%.0f units");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Outdoors: RTAO fades back to Screen Space GI's AO over the last quarter of this distance (70 units = 1 m).");
					ImGui::Text("  Shorter = cheaper. Indoors there is no fade: the whole interior is traced.");
				}
				changed |= ImGui::Combo("Broad Ray Rate", &v.broadRate, kBroadRateNames, 3);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Where the broad (long) ray is traced each frame; the fine detail stays on every pixel.");
					ImGui::Text("  Every pixel (Ultra). Half (Medium, High): a checkerboard that swaps every frame. One in four (Low): one pixel of each");
					ImGui::Text("  2 x 2 block, a different one each frame. The blur fills the rest from neighbours on the same surface and the history");
					ImGui::Text("  averages the frames: cheaper, the broad AO a little noisier on thin objects and a little slower to follow motion.");
				}
				changed |= ImGui::Checkbox("Full-Resolution Filter", &v.fullResFilter);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Off (Low to High): the broad AO is blurred at quarter resolution (wide, smooth, cheap).");
					ImGui::Text("On (Ultra): blurred at full render resolution with a smaller reach (sharper edges, a little grainier, more GPU time).");
				}
				changed |= ImGui::Checkbox("Temporal v2", &v.temporalV2);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On (Medium and above): a smarter history. Each pixel remembers how many frames it has been seen and");
					ImGui::Text("  averages more of them the longer it stays (up to 10), so still scenes get much calmer. A spot that was just");
					ImGui::Text("  uncovered is recognised by its depth and direction and starts fresh from a wider blur of this frame. Resets at");
					ImGui::Text("  camera cuts. Off (Low): the 47k history.");
				}
				changed |= ImGui::Checkbox("Smooth Normals", &v.smoothNormals);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("On (Medium and above): the AO rays leave each pixel around the smooth surface direction (last frame's G-buffer");
					ImGui::Text("  normal, moved to this frame) instead of the flat facet seen in the depth: low-poly round things (bottles, legs) no");
					ImGui::Text("  longer get one AO value per facet. Off (Low): the facet direction (cheaper).");
				}
				if (changed) {
					if (settings.AOQuality != kTierCustom) {
						CopyAOTierToCustom(settings.AOQuality);
						settings.AOQuality = kTierCustom;
					}
					if (v.detailRays != cur.detailRays)
						settings.AODetailRays = std::clamp(v.detailRays, 1, kAODetailRaysMax);
					if (v.distance != cur.distance)
						settings.AODistance = std::clamp(v.distance, 1000.0f, 8192.0f);
					if (v.broadRate != cur.broadRate)
						settings.AOBroadRate = std::clamp(v.broadRate, 0, 2);
					if (v.fullResFilter != cur.fullResFilter)
						settings.AOFullResFilter = v.fullResFilter;
					if (v.temporalV2 != cur.temporalV2)
						settings.AOTemporalV2 = v.temporalV2;
					if (v.smoothNormals != cur.smoothNormals)
						settings.AOSmoothNormals = v.smoothNormals;
				}
				if (!aoV2Fault.empty())
					RtErrorLine("Temporal v2 was switched off automatically", aoV2Fault);
				if (!aoSmoothFault.empty())
					RtErrorLine("Smooth Normals was switched off for this session", aoSmoothFault);
			}
			ImGui::Separator();
			ImGui::SliderFloat("Detail Ray Length", &settings.AODetailLength, kAODetailLengthMin, kAODetailLengthMax, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How close an object has to be to count as fine detail (Detail Strength); farther ones count for Strength.");
				ImGui::Text("  Default 20 (about 28 cm). At most half the Ray Length.");
			}
			// (49a) S1 RT Specular Occlusion: (50b) moved here from the page.
			ImGui::BeginDisabled(!settings.AO);
			ImGui::Checkbox("RT Specular Occlusion", &settings.AOSpecOcc);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On (default): the AO rays also note which way each spot is open, and the environment reflection (cubemap) is darkened");
				ImGui::Text("  by how much of the reflected direction is blocked, not just by how dark the AO is overall: shiny floors along walls,");
				ImGui::Text("  under tables, in doorways and inside armour folds stop glowing, while reflections that point into the open stay bright.");
				ImGui::Text("Only where RTAO is traced (about 1.4 m around each surface) and only the cubemap part (SSRT's own reflections are");
				ImGui::Text("  unchanged); rough surfaces keep the old darkening. Off: exactly as before (the composite is rebuilt once).");
			}
			ImGui::BeginDisabled(!settings.AOSpecOcc);
			ImGui::SliderFloat("Specular Occlusion Strength", &settings.AOSpecOccStrength, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("1 (default) = the direction-aware darkening where it is trusted; 0 = the old darkening.");
			ImGui::EndDisabled();
			if (!specOccFault.empty())
				RtErrorLine("RT Specular Occlusion was switched off for this session", specOccFault);
			ImGui::EndDisabled();
			// (50b) Retired, always on: (49h) Count Contact Once, (46f) Temporal.
			{
				const char* kBudget[] = { "Blocked (as before)", "Half Open", "Open" };
				settings.AOAlphaBudget = std::clamp(settings.AOAlphaBudget, 0, 2);
				ImGui::Combo("Dense Leaves (alpha budget)", &settings.AOAlphaBudget, kBudget, IM_ARRAYSIZE(kBudget));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("An AO ray stops checking leaf / cut-out textures after 8 see-through spots (bounded cost). Then it counts as:");
					ImGui::Text("  Blocked (default, as before): dense ivy / bushes may come out too dark.");
					ImGui::Text("  Half Open: half darkened. Open: not darkened (dense foliage may come out too light).");
				}
			}
			if (!aoExtrasFault.empty())
				RtErrorLine("Fine Detail could not start (RTAO is off this session)", aoExtrasFault);
			ImGui::EndDisabled();
			ImGui::TreePop();
		}

		if (ImGui::TreeNodeEx("Diagnostics")) {
			ImGui::BeginDisabled(!settings.AO);
			{
				const char* views[] = { "Off", "Raw RTAO (broad)", "Filtered RTAO (broad)", "RTAO vs SSGI (split)", "Raw Detail", "Filtered Detail", "Final AO",
					"Smooth Normals: Confidence", "Smooth Normals: Ns", "Smooth Normals: Ng", "Specular Occlusion" };  // (48o) "Temporal v2: History" removed
				static_assert(IM_ARRAYSIZE(views) == static_cast<int>(AODebugView::Count));
				int view = static_cast<int>(aoDebugView);
				if (ImGui::Combo("AO Debug View", &view, views, IM_ARRAYSIZE(views)))
					aoDebugView = static_cast<AODebugView>(std::clamp(view, 0, static_cast<int>(AODebugView::Count) - 1));
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Raw RTAO: this frame's rays (white = open, black = blocked); blue tint = not traced (SSGI's AO there).");
					ImGui::Text("Filtered RTAO: after the blur (and the history), as it enters the mix.");
					ImGui::Text("RTAO vs SSGI (split): the real picture; left half with RTAO, right half with Screen Space GI's AO as before.");
					ImGui::Text("Raw RTAO shows the long rays' far part (orange = its near part was blocked: the detail term covers that");
					ImGui::Text("  direction); Raw / Filtered Detail = the short rays (open share) and their filtered result;");
					ImGui::Text("  Final AO = what the lighting gets (white = no darkening).");
					ImGui::Text("(47k) Smooth Normals: Confidence = green where last frame's smooth normal is used (brighter = more trusted),");
					ImGui::Text("  red = fell back to the smoothed depth direction, blue = no data (sky, just uncovered, off screen, first frame);");
					ImGui::Text("  Ns = the direction the AO rays are built around, Ng = the flat facet direction (colours: x red, y green, z blue).");
					ImGui::Text("(49a) Specular Occlusion (RT Specular Occlusion on): how much of the cubemap reflection is kept (white = all,");
					ImGui::Text("  black = none; Strength 1); blue tint = no bent-normal data there (the old darkening applies).");
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
				static constexpr const char* kFilters[] = { "quarter resolution (46s)", "quarter resolution", "full resolution" };
				const std::string detailText = aoLastDetailRays > 0 ? std::format("{} rays x {:.0f} units", aoLastDetailRays, aoLastDetailLength) : std::string("off");
				ImGui::Text("Filter: %s; fine detail %s", aoLastFilter >= 0 ? kFilters[std::clamp(aoLastFilter, 0, 2)] : "-", detailText.c_str());
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
	                           backend.GetSkinStats().bytes + backend.GetSkinStats().scratchBytes + backend.GetSkinStats().boneBytes +
	                           backend.PointWideBytes() + backend.ReflectBytes() + backend.PointBlueNoiseBytes();  // (48a) (50b)

	// Summary for players: where the settings are, what is on, what it costs.
	ImGui::TextWrapped("The settings are on the Sun Shadows, Point Light Shadows and Ambient Occlusion pages (below this one in the list).");
	if (settings.Enabled) {
		// (48k) RT reflections are hidden and always Off: no "reflections" part in this line any more (48i showed its preset).
		ImGui::Text("In use: sun shadows %s, point-light shadows %s, ambient occlusion %s",
			settings.SunShadows ? kQualityNames[std::clamp(settings.QualityLevel, 0, 5)] : "Off", settings.PointLights ? kTierNames[std::clamp(settings.PointQuality, 0, kTierCustom) + 1] : "Off",
			settings.AO ? kTierNames[std::clamp(settings.AOQuality, 0, kTierCustom) + 1] : "Off");
		if (status == Status::Active || status == Status::Inactive || status == Status::Waiting)
			ImGui::Text("Cost: %.1f ms on the graphics card, %s of video memory", rt.smoothedGpuMs, Megabytes(vram).c_str());
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Graphics-card time of the ray-tracing work (scene updates and tracing, smoothed) and the video memory it holds.");
		if (overVramBudget)
			ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "Over the 1.2 GB scene budget: no further cells are captured.");
	}

	// (48o) Developer (collapsed): the loading-screen preparation's budgets (the preparation itself is always on since 47m; this
	// was "Advanced") and a few stats lines for bug hunting (they were shown on the pages before).
	if (ImGui::TreeNodeEx("Developer")) {
		ImGui::SeparatorText("Stats");
		if (scene.SwayActive()) {
			const auto& ws = scene.GetWindStats();
			const auto& bs = backend.GetWindStats();
			ImGui::TextDisabled("Foliage sway: %u of %u plants in reach (%u in all), %llu vertices, %.1f MB, %u refits this frame", ws.swaying, ws.inRadius,
				ws.candidates, static_cast<unsigned long long>(bs.vertices), static_cast<double>(bs.bytes) / (1024.0 * 1024.0), bs.refitsLastFrame);
		} else {
			ImGui::TextDisabled("Foliage sway: not active");
		}
		{
			// (47u) Skip Updates That Cannot Affect the Image (always on since 48o).
			const auto& ws = backend.GetWindStats();
			ImGui::TextDisabled("Skipped updates last frame: %u plant refits skipped, %u refit late; %u NPCs skipped; %llu character redos so far",
				ws.culledLastFrame, ws.lateLastFrame, relNpcCulledThisFrame, static_cast<unsigned long long>(relFallbacks));
		}
		// (47z) Keep Buildings in the Game's Sun Shadow Map (always on since 48o).
		ImGui::TextDisabled("Kept for see-through surfaces: %u draws last frame (%u could not be kept: left out; %u self-check misses), %.1f MB%s",
			RayTracingCasterFilter::KeptLastFrame(), RayTracingCasterFilter::RedirectFailedLastFrame(), RayTracingCasterFilter::RedirectLostLastFrame(),
			static_cast<double>(RayTracingCasterFilter::KeptBytes() + (forwardMerged ? static_cast<std::uint64_t>(forwardMergedDesc.Width) * forwardMergedDesc.Height * forwardMergedDesc.ArraySize * 4u : 0u)) / (1024.0 * 1024.0),
			globals::state && forwardMergedFrame != UINT32_MAX && globals::state->frameCount - forwardMergedFrame <= 1u ? "" : " (not merged this frame)");
		ImGui::SeparatorText("Loading screens");
		ImGui::SliderFloat("Loading Screen Work (ms per frame)", &settings.LoadingPumpMs, 2.0f, 32.0f, "%.0f ms");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("While a loading screen is up, the place you arrive at is captured and built for ray tracing, so the first frames you");
			ImGui::Text("  see do not have to do it. This is the CPU time Ray Tracing may use per loading-screen frame. More: ready sooner, but");
			ImGui::Text("  the game's own loading gets less time.");
		}
		ImGui::SliderFloat("Black Screen Window (ms)", &settings.BlackWindowMs, 0.0f, 2000.0f, "%.0f ms");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("How long after the loading screen the picture still counts as black (large budgets; the switch to RT happens here).");
			ImGui::Text("After it the budgets shrink over the fade-in to the small per-frame budgets of normal play.");
		}
		ImGui::SliderFloat("Work per Loaded Cell (ms)", &settings.CellEventMs, 2.0f, 50.0f, "%.0f ms");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Each cell of the place you arrive at is captured and built for ray tracing the moment it finishes loading, behind the");
			ImGui::Text("  loading screen. CPU time per loaded cell: the loading screen gets longer by about this much per cell (about 25 cells");
			ImGui::Text("  per fast travel).");
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Diagnostics")) {
		DrawLoadingScreens();  // (50b) the loading-screen kill switch, first in Diagnostics
		ImGui::Separator();
		const char* views[] = { "Off", "Shadow Source", "RT vs Shadow Map", "BLAS Coverage", "Raw RT Visibility", "RT Hit vs Depth", "Penumbra Radius",
			"History Length", "Adaptive Rays", "SSS: RT Casters", "Distance Quality", "Character Rays", "Point Lights: Candidates", "Point Lights: Traced Lights",
			"Point Lights: Raw Rays", "Point Lights: Denoised", "Point Lights: SSPLS Coverage", "Point Lights: Moving Pixels", "Point Lights: Filter Path" };  // (48m) + 2
		static_assert(IM_ARRAYSIZE(views) == static_cast<int>(DebugView::Count));
		// (49h) "Point Lights: SSPLS Coverage" is retired (its Lighting code is gone): the list skips it; the enum keeps its value.
		if (debugView == DebugView::PLSsplsCoverage)
			debugView = DebugView::Off;
		const char* shown[IM_ARRAYSIZE(views) - 1];
		int shownView[IM_ARRAYSIZE(views) - 1];
		int shownCount = 0, pick = 0;
		for (int i = 0; i < IM_ARRAYSIZE(views); ++i) {
			if (i == static_cast<int>(DebugView::PLSsplsCoverage))
				continue;
			if (i == static_cast<int>(debugView))
				pick = shownCount;
			shown[shownCount] = views[i];
			shownView[shownCount++] = i;
		}
		if (ImGui::Combo("Debug View", &pick, shown, shownCount))
			debugView = static_cast<DebugView>(shownView[std::clamp(pick, 0, shownCount - 1)]);
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
			// (48m)
			ImGui::Text("Point Lights: Moving Pixels: where the lamp-shadow clean-up sees a moving surface (game motion vectors): red = it took that");
			ImGui::Text("  surface's own last frame, yellow = none usable (this frame only), magenta = moving but Clean Shadow History is not running");
			ImGui::Text("  (Penumbra v2: its switch on the Point Light Shadows page is off; Penumbra: its shader did not compile). (50e) Penumbra v2 too.");
			ImGui::Text("Point Lights: Filter Path: the strongest traced light per pixel. Even rows: red = window beam path, green = Stable Penumbra,");
			ImGui::Text("  blue = standard path, brighter = wider blur (full at 12 px). Odd rows: how much of this frame goes in (dark = 10 %, white = all).");
			// (47h)
			ImGui::Text("Note: see-through surfaces drawn after the solid pass (glass, blended arches and banners, water, particles) are drawn");
			ImGui::Text("  over these views: the colours there describe what lies behind them, not the see-through surface itself.");
		}
		if (!lastEventsSummary.empty())
			ImGui::TextWrapped("Last transition: %s", lastEventsSummary.c_str());
		{
			// (46q) Builds budgeted by GPU time (always, 47m; F7 rt_bud_* columns).
			const auto& bud = backend.GetBudgetSample();
			ImGui::TextDisabled("Build budget: %.1f GPU ms per million triangles, prep base %.2f ms, budget %u triangles", bud.msPerMillionTris, bud.prepBaseMs,
				bud.triBudget);
		}

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
				ImGui::Text("  Of those re-captured because their shape changed (Refresh Changed Character Shapes, 49m): %u.", ch.refreshedShapes);
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
			// (47z) Keep Buildings in the Game's Sun Shadow Map: (48o) its "Kept for see-through surfaces" line is in Developer.
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
				ImGui::Text("Alpha-blended traced as cut-outs (fully opaque material, Cut-outs on) %u", skip.alphaBlendedTraced);  // (47h)
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
		settings.Enabled ? "on" : "off", kStatus[static_cast<int>(status)], settings.SunShadows ? kQualityNames[std::clamp(settings.QualityLevel, 0, 5)] : "sun off", mode(settings.Buildings),
		mode(settings.Objects), mode(settings.Trees), kFoliage[std::clamp(settings.Foliage, 0, 2)], mode(settings.Cutouts), mode(settings.Characters),
		mode(settings.PhysicsParts), mode(settings.Hair), settings.FirstPersonPlayer ? "RT" : "SM", settings.RtContactScale,
		kDenoisers[static_cast<int>(CurrentDenoiser())]);
	// What the tier sets and the denoiser in effect.
	text += std::format(", denoiser in effect {}{}, rays {}+{}, sun cone {:.2f}, pattern {} far {}, fall-off {:.0f}-{:.0f}, other characters {}, denoise radius {:.0f} "
						"taps {} {} far scale {:.2f}, history {:.0f} clamp {:.2f}, SIGMA stabilisation {}",
		kDenoisers[static_cast<int>(EffectiveDenoiser())], UpscalingRRActive() ? " (Upscaling RR on)" : "", q.baseRays, q.extraRays, q.coneDegrees,
		kPatternNames[std::min(q.tracePattern, 2u)], kPatternNames[std::min(q.farPattern, 2u)], q.falloffStart, q.falloffEnd, q.npcReduced ? "reduced" : "full",
		q.denoiseRadius, q.denoiseTaps, q.denoiseHalf ? "half res" : "full res", q.farDenoiseScale, q.history, q.clampSigma, settings.SigmaStabilizedFrames);
	// (46b) (47n) the point tier in effect
	const PointTierParams pt = ResolvePointTier();
	const AOTierParams at = ResolveAOTier();
	text += std::format(", point lights {} {} (K {}, max {}, distance {:.0f}, size {:.1f}{})",
		settings.PointLights ? "on" : "off", kTierNames[std::clamp(settings.PointQuality, 0, kTierCustom) + 1], pt.k, pt.maxSources, pt.distance,
		settings.PointLightsSize, pointOffReason.empty() ? "" : ", switched off: " + pointOffReason);
	// (46c)
	text += std::format(", indoors {} (K {}, window beams {}{})", settings.PointLightsInteriors ? "on" : "off",
		pt.kInterior, settings.PointLightsWindowBeams ? "on" : "off",
		interiorOffReason.empty() ? "" : ", switched off: " + interiorOffReason);
	// (46d)
	text += std::format(", player lights {}, spell / effect lights {}, plants block point lights outdoors {}", pt.player ? "on" : "off",
		pt.effects ? "on" : "off", pt.alphaOutdoors ? "on" : "off");
	// (46o)
	text += std::format(", window beam softness {:.2f} deg", settings.PointLightsBeamSoftness);
	// (50b)
	static constexpr const char* kPointDenoisers[5] = { "off", "penumbra", "penumbra v2", "DLSS RR", "-" };
	text += std::format(", point denoiser {} (in effect {}), point blue noise {}", kPointDenoisers[std::clamp(settings.PointDenoiser, 0, 3)],
		kPointDenoisers[pointModeThisFrame < 0 ? 4 : std::clamp(pointModeThisFrame, 0, 3)], settings.PointBlueNoise ? (backend.PointBlueNoiseFailed() ? "failed" : "on") : "off");
	text += std::format(", point RR white noise {}, v2 clean shadow history {}", settings.PointRRWhiteNoise ? "on" : "off",
		settings.PointV2CleanHistory ? (pointV2CleanFailed ? "failed" : "on") : "off");  // (50e)
	// (48o) The fixed switches (47x - 48m) are no longer listed; the measurements stay.
	text += std::format(", buildings kept in the sun shadow map: {} draws", RayTracingCasterFilter::KeptLastFrame());
	text += std::format(", lights per pixel this frame {}{}",
		pointThisFrame ? pointK : 0u, pointWideThisFrame ? " (slots 4-5 in use)" : "");
	// (48k) (48m) Point Lights: Noise Fix and Clean Shadow History (fixed on): only a failed variant is listed.
	if (pointNoiseFixFailed)
		text += ", point lights noise fix: its denoiser variant did not compile (denoiser parts off)";
	if (pointCleanFailed)
		text += ", clean shadow history: its denoiser variant did not compile (off)";
	// (48k) RT reflections are hidden and always Off (48a - 48j listed their settings here).
	text += ", RT reflections off (hidden)";
	// (46f)
	static constexpr const char* kAOModes[] = { "RTAO (keeps SSGI contact AO)", "darker of RTAO and SSGI AO" };
	static constexpr const char* kAOViews[] = { "off", "raw", "filtered", "split", "raw detail", "filtered detail", "final", "normal confidence", "normal Ns", "normal Ng", "specular occlusion" };
	text += std::format(", RTAO {} {} (mode {}, ray length {:.0f}, strength {:.2f}, max distance {:.0f}, debug view {}{})", settings.AO ? "on" : "off",
		kTierNames[std::clamp(settings.AOQuality, 0, kTierCustom) + 1], kAOModes[std::clamp(settings.AOMode, 0, 1)], settings.AORayLength, settings.AOStrength,
		at.distance, kAOViews[std::clamp(static_cast<int>(aoDebugView), 0, 10)], aoOffReason.empty() ? "" : ", switched off: " + aoOffReason);
	// (46t)
	static constexpr const char* kAOBudget[] = { "blocked", "half open", "open" };
	text += std::format(", RTAO detail strength {:.2f}, detail ray length {:.0f}, detail rays {}, full-resolution filter {}, "
						"dense leaves past the alpha budget {}{}",
		settings.AODetailStrength, settings.AODetailLength, at.detailRays, at.fullResFilter ? "on" : "off", kAOBudget[std::clamp(settings.AOAlphaBudget, 0, 2)],
		aoExtrasFault.empty() ? "" : ", detail / full-res filter switched off: " + aoExtrasFault);
	// (46y)
	text += std::format(", RTAO broad ray rate {}", kBroadRateNames[std::clamp(at.broadRate, 0, 2)]);
	// (47q)
	text += std::format(", NPCs all parts within {}", NpcFullDistance() > 0.0f ? std::format("{:.0f}", NpcFullDistance()) : std::string("no limit"));
	// (47l) (47n)
	text += std::format(", RTAO smooth normals {}{}, temporal v2 {}{}", at.smoothNormals ? "on" : "off",
		aoSmoothFault.empty() ? "" : " (switched off: " + aoSmoothFault + ")", at.temporalV2 ? "on" : "off", aoV2Fault.empty() ? "" : " (switched off: " + aoV2Fault + ")");
	// (46h)
	static constexpr const char* kAOModesInterior[] = { "RTAO + SSGI contact AO", "darker of both" };
	text += std::format(", RTAO indoors {} (mode {}, no distance fade: the interior's box{})", settings.AO && settings.AOInteriors ? "on" : "off",
		kAOModesInterior[std::clamp(settings.AOModeInterior, 0, 1)], AOInteriorWanted() ? "" : (settings.AO && settings.AOInteriors ? ", not available now" : ""));
	// (46k, 46n) Loading-screen preparation (always on since 47m): its budgets.
	text += std::format(", loading screens {:.0f} ms per frame, black window {:.0f} ms, {:.0f} ms per loaded cell", settings.LoadingPumpMs, settings.BlackWindowMs,
		settings.CellEventMs);
	// (46p, 46q)
	text += std::format(", residency priority {}, submit worker in effect {}", backend.ResidencyHigh() ? "high" : "normal",
		backend.SubmitWorkerInEffect() ? "yes" : "no");
	// (46s)
	static constexpr const char* kMeshless[4] = { "own", "snap (old)", "skip", "soft" };  // (50a) by mode
	text += std::format(", lights without a visible flame {}", kMeshless[std::clamp(settings.PointLightsMeshless, 0, 3)]);
	// (47l)
	text += std::format(", sway in RT shadows {}{}", settings.FoliageSway ? "on" : "off",
		settings.FoliageSway && !scene.SwayActive() ? " (not in effect: Foliage is shadow map or the wind shader is unavailable)" : "");
	text += std::format(", point shadows on water {}", pointWaterOn ? "on" : "off (resources failed)");
	return text;
}

nlohmann::json RayTracing::PointSelectionJson() const
{
	const auto& ps = pointSelector.GetStats();
	return { { "traced", pointThisFrame }, { "sources", ps.sources }, { "candidates", ps.candidates }, { "fillDirectAimed", ps.fillDirectAimed },
		{ "fillDirectPoint", ps.fillDirectPoint }, { "fillFixture", ps.fillFixture }, { "fillAmbient", ps.fillAmbient }, { "negative", ps.negative },
		{ "beamsWider", ps.beamsWider }, { "portalCameraInRoom", ps.cameraInRoom }, { "portalStrict", ps.portalStrict }, { "portalAway", ps.portalAway },
		{ "portalDropped", ps.portalDropped }, { "portalBoxed", ps.portalBoxed }, { "portalBadRooms", ps.portalBadRooms },
		{ "flamelessLights", ps.fillLights }, { "flamelessNotByName", ps.fillNewVsName }, { "flamelessByNameOnly", ps.fillNameOnly },
		{ "flamelessSoftNear", ps.fillSoftNear }, { "flamelessSoftFar", ps.fillSoftFar }, { "flamelessEmitterMax", ps.fillEmitterMax } };  // (50a)
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
	j["sun_shadows"] = settings.SunShadows;  // (47n)
	j["point_quality"] = settings.PointQuality;
	j["ao_quality"] = settings.AOQuality;
	j["ao_spec_occ"] = settings.AOSpecOcc ? (specOccFault.empty() ? std::format("on, strength {:.2f}", settings.AOSpecOccStrength) : std::string("failed")) : std::string("off");  // (49a)
	j["buildings_rt"] = settings.Buildings;
	j["objects_rt"] = settings.Objects;
	j["trees_rt"] = settings.Trees;
	j["foliage_mode"] = settings.Foliage;
	j["cutouts_rt"] = settings.Cutouts;
	j["characters_rt"] = settings.Characters;
	j["physics_parts_rt"] = settings.PhysicsParts;
	j["hair_rt"] = settings.Hair;
	j["particles_smooth_sheets"] = settings.ParticlesSmoothSheets;  // (50a)
	j["lights_without_flame"] = settings.PointLightsMeshless;     // (50a) 0 own, 1 snap (old), 2 skip, 3 soft
	j["first_person_player_rt"] = settings.FirstPersonPlayer;
	j["denoiser"] = settings.Denoiser;
	j["point_denoiser"] = settings.PointDenoiser;  // (50b) 0 off, 1 penumbra, 2 penumbra v2, 3 DLSS RR
	j["point_denoiser_effective"] = pointModeThisFrame;
	j["point_blue_noise"] = settings.PointBlueNoise ? (backend.PointBlueNoiseFailed() ? "failed" : "on") : "off";
	j["point_rr_white_noise"] = settings.PointRRWhiteNoise;  // (50e)
	j["point_v2_clean_history"] = settings.PointV2CleanHistory ? (pointV2CleanFailed ? "failed" : "on") : "off";
	j["point_v2_mb"] = static_cast<double>(pointV2D3D11Bytes + backend.PointBlueNoiseBytes()) / (1024.0 * 1024.0);
	j["rt_contact_scale"] = settings.RtContactScale;
	// (48o) The fixed switches (47n - 48m) are no longer written; their measurements stay.
	j["kept_sun_caster_draws"] = RayTracingCasterFilter::KeptLastFrame();
	j["kept_sun_caster_failed"] = RayTracingCasterFilter::RedirectFailedLastFrame();
	j["kept_sun_caster_lost"] = RayTracingCasterFilter::RedirectLostLastFrame();
	j["point_noise_fix"] = { { "clean_variant_failed", pointCleanFailed },  // (48k) (48m)
		{ "denoiser_variant_failed", pointNoiseFixFailed } };
	j["point_k_this_frame"] = pointThisFrame ? pointK : 0u;
	j["point_wide_slots"] = { { "in_use", pointWideThisFrame }, { "allocated", backend.PointWideReady() },
		{ "mb", static_cast<double>(backend.PointWideBytes() + pointWideD3D11Bytes) / (1024.0 * 1024.0) } };
	{
		// (48c)
		const auto& es = backend.GetStats();
		j["point_extra_slots"] = { { "this_frame", pointExtraThisFrame },
			{ "traced_pixels", es.pointTracedPixelsPerFrame }, { "extra_pixels", es.pointExtraPixelsPerFrame },
			{ "wide_want_pixels", es.pointExtraWideWantPerFrame }, { "wide_denoise_failed", pointWideDenoiseFailed } };
	}
	{
		// (48a) RT reflections step 1.
		const auto& sc = scene.GetStats();
		const auto& rs = backend.GetStats();
		j["reflections"] = { { "mode", settings.ReflectionsMode }, { "quality", kReflectionQualityNames[std::clamp(settings.ReflectionsQuality, 0, kReflectionCustom)] },  // (48i)
			{ "terrain_in_rt_scene", settings.ReflectionsTerrain }, { "off_reason", reflectOffReason },
			{ "available", backend.ReflectAvailable() }, { "traced_last_frame", rs.reflectTracedLastFrame }, { "gpu_ms", rs.lastReflectGpuMs },
			{ "pixels_per_frame", rs.reflectPixelsPerFrame }, { "textures_mb", static_cast<double>(backend.ReflectBytes()) / (1024.0 * 1024.0) },
			{ "terrain_cells", sc.terrainCells }, { "terrain_built", sc.terrainBuilt }, { "terrain_triangles", sc.terrainTriangles },
			{ "terrain_mb", static_cast<double>(sc.terrainBytes) / (1024.0 * 1024.0) }, { "terrain_quads_cpu_copy", sc.terrainQuadsCpu },
			{ "terrain_quads_from_heights", sc.terrainQuadsHeights }, { "terrain_quads_failed", sc.terrainQuadsFailed }, { "terrain_cpu_ms", sc.terrainMs },
			{ "debug_view", static_cast<int>(reflectDebugView) }, { "rough_start", settings.ReflectionsRoughStart }, { "rough_end", settings.ReflectionsRoughEnd },
			{ "full_range", settings.ReflectionsFullRange }, { "beyond_ssr", settings.ReflectionsBeyondSSR }, { "rough_solid", settings.ReflectionsRoughSolid },
			{ "best_candidate", settings.ReflectionsBestCandidate }, { "lobe_check", settings.ReflectionsLobeCheck },
			{ "cubemap_match", settings.ReflectionsCubemapMatch }, { "hit_distance", settings.ReflectionsHitDistance },
			{ "gap_fill_consumer", GapFillConsumer() }, { "gap_fill_used_last_frame", reflectGapFillUsedLastFrame },  // (48d)
			{ "cubemap_warm_up_fix", settings.ReflectionsCubemapWarmUp }, { "loading_prep", settings.ReflectionsLoadingPrep },  // (48e)
			{ "terrain_pending", scene.TerrainPending() }, { "prep_shader_ms", aoNormalReflectAsync.CompileMs() },
			{ "gap_fill_shader_ms", globals::features::screenSpaceRayTracing.raymarchSpecularRTAsync.CompileMs() } };
		// (48f) Capture Materials.
		const auto& ms = backend.GetMaterialStats();
		j["reflections"]["materials"] = { { "on", settings.ReflectionsMaterials }, { "wanted", MaterialsWanted() }, { "record_last_frame", backend.ReflectMaterialLastFrame() },
			{ "materials", ms.materials }, { "textures", ms.textures }, { "textures_ready", ms.texturesReady }, { "textures_pending", ms.texturesPending },
			{ "textures_refused", ms.texturesRefused }, { "textures_no_layer", ms.texturesFull }, { "pages", ms.pages },
			{ "texture_mb", static_cast<double>(ms.textureBytes) / (1024.0 * 1024.0) }, { "attr_mb", static_cast<double>(ms.attrBytes) / (1024.0 * 1024.0) },
			{ "attr_used_mb", static_cast<double>(ms.attrBytesUsed) / (1024.0 * 1024.0) }, { "attr_full", ms.attrFull }, { "copies_total", ms.copiesTotal },
			{ "copy_cpu_ms", ms.copyCpuMs }, { "surface_mb", static_cast<double>(backend.ReflectSurfaceBytes()) / (1024.0 * 1024.0) },
			{ "table_kb", static_cast<double>(ms.tableBytes) / 1024.0 }, { "copy_shader_ms", materialCopyAsync.CompileMs() },
			{ "attr_full_fragmented", ms.attrRefusedFragmented }, { "attr_growths", ms.attrGrowths }, { "page_create_ms", ms.pageCreateMs },
			{ "pages_made_in_play", ms.pagesMadeInPlay }, { "character_materials", settings.ReflectionsCharacterMaterials },
			{ "emit_materials", ms.emitMaterials }, { "emit_sourced", ms.emitSourced }, { "glow_materials", ms.glowMaterials },  // (48h)
			{ "glow_materials_ready", ms.glowMaterialsReady }, { "glow_textures", ms.glowTextures }, { "glow_textures_ready", ms.glowTexturesReady },
			{ "emit_refreshes", ms.emitRefreshes } };
		// (48g) The shading half.
		j["reflections"]["shading"] = { { "offscreen_color", settings.ReflectionsOffscreenColor == 1 ? "material" : "cubemap" },
			{ "brightness", settings.ReflectionsOffscreenBrightness }, { "sun_ray", settings.ReflectionsSunRay }, { "lamps", settings.ReflectionsShadeLights },
			{ "wanted", ShadeWanted() }, { "ran_last_frame", shadeLastFrame }, { "fade", shadeFade }, { "failed", reflectShadeFailed },
			{ "gpu_ms", Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::RayTracingReflectShade) },
			{ "material_share", shadeMaterialShare }, { "ratio_shaded_over_screen", shadeRatio }, { "ratio_samples", shadeRatioGroups },
			{ "ratio_pixels", shadeRatioPixels }, { "ratio_smoothed", shadeRatioSmoothed }, { "ratio_smoothed_pixels", shadeSmoothPixels },
			{ "ratio_arithmetic_48g", shadeRatioArith },
			{ "suggested_brightness", shadeRatioSmoothed > 0.0f ? std::clamp(1.0f / shadeRatioSmoothed, 0.25f, 4.0f) : -1.0f },
			{ "glow", settings.ReflectionsShadeGlow }, { "prefer_bright_cubemap", settings.ReflectionsPreferBrightCubemap },  // (48h)
			{ "shader_ms", reflectShadeAsync.CompileMs() }, { "output_mb", static_cast<double>(shadeWidth) * shadeHeight * 8.0 / (1024.0 * 1024.0) },
			{ "late_eviction_priorities", Util::FrameTrace::DeferredEvictionPriorities() } };
	}
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
	{
		// (47q) Static chunks per cell / overlap (a statistic), the NPC rule, the sun size.
		std::uint32_t cells = 0;
		float perCell = 0.0f, overlap = 0.0f;
		scene.ChunkOverlapStats(backend, cells, perCell, overlap);
		j["static_chunks"] = { { "cells", cells }, { "per_cell", perCell }, { "pairwise_overlap", overlap } };
		const auto& ch = characters.GetStats();
		j["npc_rule"] = { { "all_parts_within", NpcFullDistance() }, { "body_only", ch.bodyOnlyActors }, { "refits_skipped", ch.refitsSkipped },
			{ "tiny_face_skipped", ch.skipTinyFace } };
		j["sun_size_deg"] = settings.SunSize;
	}
	{
		// (47u)
		const auto& ws = backend.GetWindStats();
		j["speedups_47u"] = { { "early_normal_last_frame", earlyNormalThisFrame },
			{ "prev_depth_mb", aoPrevDepth ? static_cast<double>(aoPrevDepthDesc.Width) * aoPrevDepthDesc.Height * 4.0 / (1024.0 * 1024.0) : 0.0 },
			{ "wind_refits_skipped", ws.culledLastFrame }, { "wind_refits_late", ws.lateLastFrame },
			{ "wind_late_total", relLateTotal }, { "npcs_skipped", relNpcCulledThisFrame }, { "character_redos_total", relFallbacks } };
	}
	// (46b)
	{
		const auto& ps = pointSelector.GetStats();
		const PointTierParams pt = ResolvePointTier();
		j["point_lights"] = { { "enabled", settings.PointLights }, { "k", pt.k }, { "max_sources", pt.maxSources },
			{ "distance", pt.distance }, { "keep_sspls", false },
			{ "sspls_non_rt", true },  // (46y) ((50b) sspls_contact retired: fixed 8)
			{ "size", settings.PointLightsSize }, { "traced", pointThisFrame }, { "sources", ps.sources },
			{ "candidates", ps.candidates }, { "merged", ps.merged }, { "capped", ps.capped }, { "native_lights", ps.native }, { "gpu_ms", stats.lastPointGpuMs },
			{ "rays_per_frame", stats.pointRaysPerFrame }, { "water_rays_per_frame", stats.pointWaterRaysPerFrame },  // (47j)
			 { "denoise_gpu_ms", Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingPointDenoise) },
			{ "cpu_ms", pointCpuMs }, { "off_reason", pointOffReason }, { "player", pt.player }, { "effects", pt.effects },
			{ "alpha_outdoors", pt.alphaOutdoors }, { "player_sources", ps.player }, { "effect_sources", ps.effects },
			{ "alpha_last_frame", backend.PointAlphaLastFrame() } };
		// (46c)
		const auto& sc = scene.GetStats();
		j["interior"] = { { "enabled", settings.PointLightsInteriors }, { "k", pt.kInterior },
			{ "window_beams", settings.PointLightsWindowBeams }, { "this_frame", interiorThisFrame },
			{ "captured", sc.interior }, { "shapes", sc.interiorShapes }, { "triangles", sc.interiorTriangles }, { "capture_cpu_ms", sc.interiorGatherMs },
			{ "window_masks", sc.windowMasks }, { "ready_ms", interiorReadyMs }, { "beams", ps.beams }, { "spots", ps.spots }, { "spots_invalid", ps.spotsInvalid },
			{ "mask_channels", maskChannelsWritten }, { "shadow_maps_skipped", skippedLastFrame }, { "shadow_maps_skipped_total", skippedTotal },
			{ "fallback_frames", skipFallbackFrames }, { "off_reason", interiorOffReason },
			{ "adaptive_maps_kept", restoredLastFrame }, { "adaptive_uncovered_share_max", coverageShareMax },
			{ "adaptive_switches_to_kept", restoreEvents } };  // (47w)
	}
	// (46f)
	j["rtao"] = { { "enabled", settings.AO }, { "mode", settings.AOMode }, { "ray_length", settings.AORayLength }, { "strength", settings.AOStrength },
		{ "max_distance", ResolveAOTier().distance }, { "debug_view", static_cast<int>(aoDebugView) },
		{ "composited", aoComposited }, { "gpu_ms", stats.lastAOGpuMs }, { "rays_per_frame", stats.aoRaysPerFrame },
		{ "filter_gpu_ms", Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingAO) },
		{ "sync_gpu_ms", Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingAOSync) }, { "off_reason", aoOffReason },
		{ "interiors", settings.AOInteriors }, { "mode_interior", settings.AOModeInterior }, { "interior_wanted", AOInteriorWanted() },
		{ "indoors_this_frame", aoInteriorThisFrame }, { "fade_in", aoRamp } };  // (46h)
	// (47l) Foliage sway.
	{
		const auto& ws = scene.GetWindStats();
		const auto& bs = backend.GetWindStats();
		const auto wind = RayTracingWind::GetStats();
		j["foliage_sway"] = { { "enabled", settings.FoliageSway }, { "active", scene.SwayActive() }, { "candidates", ws.candidates },
			{ "in_radius", ws.inRadius }, { "radius", ws.radius }, { "swaying", ws.swaying }, { "waiting", ws.waiting }, { "still", ws.still },
			{ "instances", bs.meshes }, { "sources", bs.sources }, { "source_mb", static_cast<double>(bs.sourceBytes) / (1024.0 * 1024.0) },
			{ "pending", bs.pending }, { "vertices", bs.vertices }, { "triangles", bs.triangles }, { "mb", static_cast<double>(bs.bytes) / (1024.0 * 1024.0) },
			{ "scratch_mb", static_cast<double>(bs.scratchBytes) / (1024.0 * 1024.0) }, { "builds_last_frame", bs.buildsLastFrame },
			{ "refits_last_frame", bs.refitsLastFrame }, { "refit_vertices_last_frame", bs.refitVerticesLastFrame },
			{ "deferred_last_frame", bs.deferredLastFrame }, { "still_last_frame", bs.stillLastFrame }, { "builds_total", bs.buildsTotal },
			{ "over_budget", bs.overBudget }, { "refused", bs.refused }, { "new_instances", ws.newInstances }, { "new_sources", ws.newSources },
			{ "released", ws.released }, { "poses", ws.poses }, { "pose_faults", ws.poseFaults }, { "available", wind.available },
			{ "node_reads", wind.nodeReads }, { "node_faults", wind.nodeFaults }, { "diag_compared", wind.compared },
			{ "diag_formula_matches", wind.formulaMatches }, { "diag_early_matches", wind.earlyMatches }, { "magnitude", wind.magnitude },
			{ "fade_start", wind.fadeStart }, { "fade_end", wind.fadeEnd }, { "off_reason", windOffReason } };
	}
	return j;
}

bool RayTracing::SunMaskDepthCheckActive() const
{
	// (47h) Only within the frame the mask was written (a stale flag after Ray Tracing stopped running never applies).
	return RtWanted() && SunOn() && sunMaskWrittenThisFrame && globals::state && sunMaskFrame == globals::state->frameCount;
}

std::uint32_t RayTracing::PointLightTag(const RE::BSLight* a_light) const
{
	std::uint32_t tag = pointSelector.TagFor(a_light);
	// (47z) Its engine shadow map was left out this frame (Skip the Game's Shadow Maps): where a pixel did not trace it, PointMaskCS
	// wrote its mask channel unshadowed, so the SSPLS Coverage view counts it as no shadow (red), not as a shadow map (orange). Only
	// that view reads the bit (Lighting.hlsl); no shading depends on it.
	if (a_light && !skippedLights.empty() && std::binary_search(skippedLights.begin(), skippedLights.end(), a_light))
		tag |= RayTracingPointLights::kTagMapSkipped;
	// (48a) Lights per pixel 5 - 6 this frame: Lighting also looks in slots 4 - 5 (t46, bound by FinishPointLights).
	if (pointWideThisFrame && (tag & RayTracingPointLights::kTagSourceMask))
		tag |= RayTracingPointLights::kTagWide;
	return tag;
}

ID3D11ShaderResourceView* RayTracing::ForwardShadowMap(ID3D11ShaderResourceView* a_cascades)
{
	// (47z) Keep Buildings in the Game's Sun Shadow Map. No redirected draw this frame (switch off, Ray Tracing not latched, interior,
	// nothing kept): the engine's cascades, exactly as 47y.
	forwardMergedLastFrame = false;
	if (!a_cascades || !loaded || !globals::state)
		return a_cascades;
	auto* kept = RayTracingCasterFilter::KeptCascades(a_cascades);
	if (!kept)
		return a_cascades;
	const std::uint32_t frame = globals::state->frameCount;
	if (forwardMergedFrame == frame && forwardMergedFrom == a_cascades && forwardMergedSRV) {
		forwardMergedLastFrame = true;
		return forwardMergedSRV.get();  // a second mask draw this frame: already merged
	}
	winrt::com_ptr<ID3D11Resource> resource;
	a_cascades->GetResource(resource.put());
	winrt::com_ptr<ID3D11Texture2D> texture;
	if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(texture.put()))))
		return a_cascades;
	D3D11_TEXTURE2D_DESC desc{};
	texture->GetDesc(&desc);
	if (!forwardMerged || forwardMergedDesc.Width != desc.Width || forwardMergedDesc.Height != desc.Height || forwardMergedDesc.ArraySize != desc.ArraySize) {
		ReleaseForwardShadowMap();
		D3D11_TEXTURE2D_DESC mergedDesc{};
		mergedDesc.Width = desc.Width;
		mergedDesc.Height = desc.Height;
		mergedDesc.MipLevels = 1;
		mergedDesc.ArraySize = desc.ArraySize;
		mergedDesc.Format = DXGI_FORMAT_R32_FLOAT;
		mergedDesc.SampleDesc = { 1, 0 };
		mergedDesc.Usage = D3D11_USAGE_DEFAULT;
		mergedDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		auto* device = globals::d3d::device;
		HRESULT hr = device->CreateTexture2D(&mergedDesc, nullptr, forwardMerged.put());
		if (SUCCEEDED(hr)) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
			srvDesc.Texture2DArray.MipLevels = 1;
			srvDesc.Texture2DArray.ArraySize = desc.ArraySize;
			hr = device->CreateShaderResourceView(forwardMerged.get(), &srvDesc, forwardMergedSRV.put());
		}
		if (SUCCEEDED(hr)) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = DXGI_FORMAT_R32_FLOAT;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.ArraySize = desc.ArraySize;
			hr = device->CreateUnorderedAccessView(forwardMerged.get(), &uavDesc, forwardMergedUAV.put());
		}
		if (FAILED(hr)) {
			logger::warn("[RayTracing] Keep Buildings in the Game's Sun Shadow Map: could not create the merged cascades ({}x{} x {}, hr {:08X})", desc.Width,
				desc.Height, desc.ArraySize, static_cast<std::uint32_t>(hr));
			ReleaseForwardShadowMap();
			return a_cascades;
		}
		Util::SetResourceName(forwardMerged.get(), "RayTracing Forward Sun Cascades");
		forwardMergedDesc = mergedDesc;
	}
	auto* shader = GetShader(forwardMergeCS, L"Data\\Shaders\\RayTracing\\ForwardShadowMergeCS.hlsl", "ForwardShadowMergeCS");
	if (!shader)
		return a_cascades;
	auto* context = globals::d3d::context;
	// The merged array may still be bound for the pixel shaders (last frame's t18): not as SRV while it is written.
	ID3D11ShaderResourceView* nullSrv[2]{};
	context->PSSetShaderResources(18, 1, nullSrv);
	ID3D11ShaderResourceView* srvs[2]{ a_cascades, kept };
	ID3D11UnorderedAccessView* uav = forwardMergedUAV.get();
	context->CSSetShader(shader, nullptr, 0);
	context->CSSetShaderResources(0, 2, srvs);
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->Dispatch((desc.Width + 7) / 8, (desc.Height + 7) / 8, desc.ArraySize);
	uav = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShaderResources(0, 2, nullSrv);
	context->CSSetShader(nullptr, nullptr, 0);
	forwardMergedFrame = frame;
	forwardMergedFrom = a_cascades;
	forwardMergedLastFrame = true;
	return forwardMergedSRV.get();
}

void RayTracing::ReleaseForwardShadowMap()
{
	forwardMerged = nullptr;
	forwardMergedSRV = nullptr;
	forwardMergedUAV = nullptr;
	forwardMergedDesc = {};
	forwardMergedFrame = UINT32_MAX;
	forwardMergedFrom = nullptr;
}

bool RayTracing::SunTracingActive() const
{
	// (49h stage 1, S4a) For the A4 sun-bits distance (UpstreamEngineFixes): the RT sun shadows are drawn this frame.
	return loaded && RtWanted() && SunOn() && status == Status::Active && handedOff;
}
