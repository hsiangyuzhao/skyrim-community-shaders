#include "RayTracing.h"

#include "Deferred.h"
#include "Features/RayTracing/CasterFilter.h"
#include "Features/RayTracing/TextureSharing.h"
#include "Features/ScreenSpaceShadows.h"
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
	ShareGameTextures)

namespace
{
	struct QualityPreset
	{
		const char* name;
		std::uint32_t rays;
		float coneDegrees;  // half angle; the sun disc is about 0.27
		std::uint32_t filterRadius;
		std::uint32_t extraRays;  // (45k) adaptive extra rays at shadow edges
		const char* tooltip;
	};

	constexpr QualityPreset kQuality[3] = {
		{ "Low", 1, 0.27f, 1, 1, "1 ray per pixel (+1 at shadow edges with adaptive rays), sharp sun (the real sun disc), light 3x3 smoothing. Cheapest." },
		{ "Medium", 1, 0.5f, 2, 3, "1 ray per pixel (+3 at shadow edges with adaptive rays), slightly softer edges, 5x5 smoothing." },
		{ "High", 2, 0.5f, 2, 4, "2 rays per pixel (+4 at shadow edges with adaptive rays), slightly softer edges, 5x5 smoothing. Cleanest." },
	};

	constexpr float kCaptureBudgetMs = 1.0f;      // CPU per frame for capturing new cells (45j: 1.5 -> 1.0, resumable per shape)
	constexpr std::uint32_t kBuildBudget = 131072;  // triangles of BLAS builds per frame (about one chunk)
	constexpr float kHandOffBand = 2048.0f;       // half a cell: RT fades back to the cascades here
	constexpr float kMaxRayLength = 16384.0f;
	constexpr std::uint64_t kAccelerationBudgetBytes = 1200ull << 20;  // BLAS + scratch; spec target is 1.5 GB in total
	constexpr std::uint32_t kFlagSkipShadowed = 1u, kFlagCoverage = 2u, kFlagFirstPerson = 4u;  // TraceShadowsCS
	constexpr std::uint32_t kFlagWorldNoise = 8u, kFlagStableNormal = 16u, kFlagAlphaFootprint = 32u;  // TraceShadowsCS (45j)
	constexpr std::uint32_t kFlagHandOffSSS = 1u, kFlagDebug = 2u;  // Common.hlsli
	constexpr std::uint32_t kFlagCombineSSS = 4u, kFlagKeepSSSUnderFoliage = 8u, kFlagTemporal = 16u, kFlagHistoryValid = 32u,
							kFlagTraceSkipped = 64u;  // Common.hlsli (45j)
	constexpr float kTemporalBlend = 0.2f;  // (45j) weight of the new frame in the history blend
	// (45k) Denoiser and the trace flags it needs.
	constexpr std::uint32_t kFlagR2Noise = 64u, kFlagAdaptive = 128u, kFlagBlockerDist = 256u, kFlagRtSurface = 512u;  // TraceShadowsCS
	constexpr std::uint32_t kFlagPenumbra = 128u, kFlagCombineBlockerDist = 256u;                                       // Common.hlsli
	constexpr float kDenoiseMaxRadius = 16.0f;  // px at render resolution
	constexpr float kDenoiseMaxHistory = 12.0f; // frames
	constexpr float kDenoiseClampSigma = 1.5f;
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
// Settings
// ---------------------------------------------------------------------------------------------

void RayTracing::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.QualityLevel = std::clamp(settings.QualityLevel, 0, 2);
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
	for (int i = 0; i < 2; ++i) {
		history[i] = nullptr;
		historySRV[i] = nullptr;
		historyUAV[i] = nullptr;
	}
	historyValid = false;
	ReleaseDenoiser();
	maskDesc = {};
	d3d11Bytes = 0;
}

void RayTracing::ReleaseDenoiser()
{
	for (auto* t : { &denoiseTiles, &denoiseH, &denoiseOut, &history4[0], &history4[1] })
		*t = {};
	history4Valid = false;
	d3d11Bytes -= std::min(d3d11Bytes, denoiseBytes);
	denoiseBytes = 0;
}

bool RayTracing::EnsureDenoiser()
{
	// (45k) Denoiser Penumbra: created on first use (render-target size of the shadow mask).
	if (denoiseTiles.texture && denoiseH.texture && denoiseOut.texture && history4[0].texture && history4[1].texture)
		return true;
	if (!maskTemp)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	maskTemp->GetDesc(&desc);
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	auto* device = globals::d3d::device;
	auto create = [&](DenoiseTexture& o_texture, std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, std::uint32_t a_bytesPerPixel,
					  const char* a_name) {
		D3D11_TEXTURE2D_DESC d = desc;
		d.Width = a_width;
		d.Height = a_height;
		d.Format = a_format;
		if (FAILED(device->CreateTexture2D(&d, nullptr, o_texture.texture.put())) ||
			FAILED(device->CreateShaderResourceView(o_texture.texture.get(), nullptr, o_texture.srv.put())) ||
			FAILED(device->CreateUnorderedAccessView(o_texture.texture.get(), nullptr, o_texture.uav.put())))
			return false;
		Util::SetResourceName(o_texture.texture.get(), a_name);
		denoiseBytes += static_cast<std::uint64_t>(a_width) * a_height * a_bytesPerPixel;
		return true;
	};
	ReleaseDenoiser();
	const bool ok = create(denoiseTiles, (desc.Width + 7) / 8, (desc.Height + 7) / 8, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "RayTracing DenoiseTiles") &&
	                create(denoiseH, desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, 4, "RayTracing DenoiseH") &&
	                create(denoiseOut, desc.Width, desc.Height, DXGI_FORMAT_R16G16_FLOAT, 4, "RayTracing Denoised") &&
	                create(history4[0], desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "RayTracing History4") &&
	                create(history4[1], desc.Width, desc.Height, DXGI_FORMAT_R16G16B16A16_FLOAT, 8, "RayTracing History4");
	if (!ok) {
		ReleaseDenoiser();
		logger::warn("[RayTracing] Denoiser Penumbra: could not create its textures; Denoiser set to Off");
		settings.Denoiser = static_cast<int>(DenoiserMode::Off);
		return false;
	}
	d3d11Bytes += denoiseBytes;
	history4Valid = false;
	return true;
}

ID3D11ComputeShader* RayTracing::GetShaderDefined(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name, const char* a_define)
{
	if (!a_shader) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (a_define)
			defines.emplace_back(a_define, "");
		a_shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(a_path, defines, "cs_5_0")));
		if (!a_shader)
			logger::error("[RayTracing] Could not compile {}", a_name);
	}
	return a_shader.get();
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

bool RayTracing::EnsureHistory()
{
	// (45j) Temporal: two R16G16_FLOAT textures (visibility, distance), created on first use.
	if (history[0] && history[1])
		return true;
	if (!maskTemp)
		return false;
	D3D11_TEXTURE2D_DESC desc{};
	maskTemp->GetDesc(&desc);
	desc.Format = DXGI_FORMAT_R16G16_FLOAT;
	desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
	auto* device = globals::d3d::device;
	for (int i = 0; i < 2; ++i) {
		if (FAILED(device->CreateTexture2D(&desc, nullptr, history[i].put())) || FAILED(device->CreateShaderResourceView(history[i].get(), nullptr, historySRV[i].put())) ||
			FAILED(device->CreateUnorderedAccessView(history[i].get(), nullptr, historyUAV[i].put()))) {
			for (int k = 0; k < 2; ++k) {
				history[k] = nullptr;
				historySRV[k] = nullptr;
				historyUAV[k] = nullptr;
			}
			logger::warn("[RayTracing] Temporal: could not create the history textures; switched off");
			settings.Denoiser = static_cast<int>(DenoiserMode::Off);
			return false;
		}
		Util::SetResourceName(history[i].get(), "RayTracing History");
	}
	d3d11Bytes += static_cast<std::uint64_t>(desc.Width) * desc.Height * 4 * 2;
	historyValid = false;
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
	const std::int64_t start = Qpc();
	// (45h) Diffuse textures loaded from now on are created shareable only while both switches are on.
	RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.ShareGameTextures && (settings.Foliage != 0 || settings.Cutouts));

	if (!settings.Enabled) {
		if (sssClaimed)
			globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
		sssClaimed = false;
		phase = Phase::Play;
		fade = 1.0f;
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
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}
	// (45k) Screen Space Shadows claimed but not drawn by the trace (nothing traced this frame): the
	// normal ray march, without the caster mask.
	if (sssClaimed)
		globals::features::screenSpaceShadows.DrawDeferred(nullptr, 0.0f, 0.0f, 0.0f);
	sssClaimed = false;

	lastCpuMs = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
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
		detail.denoiseGpuMs = Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::RayTracingDenoise);
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
	// (45f) TLAS self-check twice a second or so; missed casters noted by the shadow map are described.
	const bool validate = frameIndex % 30 == 0;
	scene.CollectInstances(instances, validate);
	scene.DescribeMissedCasters();
	// (45i) Characters in the traced area: capture, bone matrices, draws (none with the switch off).
	const std::int64_t charactersStart = Qpc();
	skinDraws.clear();
	skinBones.clear();
	if (settings.Characters && backend.SkinAvailable()) {
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

	const auto& preset = kQuality[std::clamp(settings.QualityLevel, 0, 2)];
	const auto& invViewProj = fb.GetCameraViewProjInverse(0);
	static_assert(sizeof(invViewProj) == sizeof(float) * 16);

	RayTracingRT::FrameInput input;
	std::memcpy(input.constants.invViewProj, &invViewProj, sizeof(input.constants.invViewProj));
	input.constants.toSun[0] = toSun.x;
	input.constants.toSun[1] = toSun.y;
	input.constants.toSun[2] = toSun.z;
	input.constants.toSun[3] = std::tan(preset.coneDegrees * 3.14159265f / 180.0f);
	std::uint32_t traceFlags = 0;
	if (debugView != DebugView::Disagreement && debugView != DebugView::RawVisibility)
		traceFlags |= kFlagSkipShadowed;
	if (debugView == DebugView::Coverage || debugView == DebugView::HitDepth)
		traceFlags |= kFlagCoverage;
	// (45i) First person: no character ray for the arms / weapon (inside the third-person body).
	if (auto* playerCamera = RE::PlayerCamera::GetSingleton(); !skinDraws.empty() && playerCamera && playerCamera->IsInFirstPerson())
		traceFlags |= kFlagFirstPerson;
	// (45k) Denoiser mode and what it picks for Noise / Adaptive Rays (Auto).
	const DenoiserMode denoiser = CurrentDenoiser();
	const bool penumbra = denoiser == DenoiserMode::Penumbra;
	const bool rrMode = denoiser == DenoiserMode::DLSSRR;
	bool worldNoise = settings.Noise == static_cast<int>(AutoMode::Auto) ? !(penumbra || rrMode) : settings.Noise == static_cast<int>(AutoMode::First);
	if (rrMode)
		worldNoise = false;  // DLSS RR needs fresh noise every real frame
	const bool adaptive = settings.AdaptiveRays == static_cast<int>(AutoMode::Auto) ? (penumbra || rrMode) : settings.AdaptiveRays == static_cast<int>(AutoMode::First);
	// (45j) Runtime switches of the trace (each off = 45i).
	if (worldNoise)
		traceFlags |= kFlagWorldNoise;
	else if (penumbra || rrMode)
		traceFlags |= kFlagR2Noise;  // (45k) decorrelated animated rays; Off / Temporal keep the 45j pattern
	if (settings.StableNormals)
		traceFlags |= kFlagStableNormal;
	if (settings.FineAlphaMips)
		traceFlags |= kFlagAlphaFootprint;
	if (adaptive && preset.extraRays > 0)
		traceFlags |= kFlagAdaptive;
	const bool blockerDist = penumbra && !(traceFlags & kFlagCoverage);
	if (blockerDist)
		traceFlags |= kFlagBlockerDist;
	// (45k) Screen Space Shadows "non-RT casters only": the trace marks the surfaces that are traced casters.
	const bool sssNonRT = sssClaimed;
	if (sssNonRT)
		traceFlags |= kFlagRtSurface;
	input.constants.extra[0] = (traceFlags & kFlagAdaptive) ? static_cast<float>(preset.extraRays) : 0.0f;
	input.constants.params[0] = static_cast<float>(preset.rays);
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

	auto* context = globals::d3d::context;
	auto* screenSpaceShadows = &globals::features::screenSpaceShadows;
	// (45j) Screen Space Shadows inside the traced area: combined with min() (default), handed off (45i) or kept.
	const bool sssAvailable = typedUavLoadR8 && screenSpaceShadows->loaded && screenSpaceShadows->bendSettings.Enable && screenSpaceShadows->screenSpaceShadowsTexture;
	const auto sssMode = static_cast<SSSMode>(std::clamp(settings.ScreenSpaceShadows, 0, 3));
	const bool handOffSSS = sssAvailable && sssMode == SSSMode::HandOff;
	// (45k) Non-RT casters only: the texture then holds only non-traced casters (and traced ones within
	// their contact range): merged with min() like Combine, so shared shadows are not darkened twice.
	const bool combineSSS = sssAvailable && (sssMode == SSSMode::Combine || sssMode == SSSMode::NonRT);
	const bool writeSSS = handOffSSS || combineSSS;
	const bool temporal = denoiser == DenoiserMode::Temporal && EnsureHistory();
	if (!temporal)
		historyValid = false;
	const bool penumbraReady = penumbra && EnsureDenoiser();
	if (!penumbraReady)
		history4Valid = false;
	const bool debug = debugView != DebugView::Off;

	CombineCB cb{};
	std::memcpy(cb.InvViewProj, &invViewProj, sizeof(cb.InvViewProj));
	std::copy_n(traceRect, 4, cb.Rect);
	cb.Band[0] = kHandOffBand;
	// (45k) DLSS RR: no filter of ours at all (radius 0 = the pixel itself).
	cb.Band[1] = rrMode ? 0.0f : static_cast<float>(preset.filterRadius);
	std::uint32_t combineFlags = (handOffSSS ? kFlagHandOffSSS : 0u) | (debug ? kFlagDebug : 0u) | (combineSSS ? kFlagCombineSSS : 0u) |
	                             (handOffSSS && settings.KeepSSSUnderFoliage ? kFlagKeepSSSUnderFoliage : 0u) |
	                             ((traceFlags & kFlagSkipShadowed) ? kFlagTraceSkipped : 0u) | (blockerDist ? kFlagCombineBlockerDist : 0u);
	if (temporal || penumbraReady) {
		std::copy_n(prevViewProj, 16, cb.PrevViewProj);
		cb.CameraDelta[0] = static_cast<float>(camX - prevCamera[0]);
		cb.CameraDelta[1] = static_cast<float>(camY - prevCamera[1]);
		cb.CameraDelta[2] = static_cast<float>(camZ - prevCamera[2]);
	}
	if (temporal) {
		combineFlags |= kFlagTemporal | (historyValid ? kFlagHistoryValid : 0u);
		cb.Band[3] = kTemporalBlend;
	}
	if (penumbraReady)
		combineFlags |= kFlagPenumbra | (history4Valid ? kFlagHistoryValid : 0u);
	cb.Denoise[0] = input.constants.toSun[3];
	cb.Denoise[1] = kDenoiseMaxRadius;
	cb.Denoise[2] = kDenoiseMaxHistory;
	cb.Denoise[3] = kDenoiseClampSigma;
	cb.Extra[0] = fade;
	cb.Extra[1] = kBlockerRange;
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
	}

	// 2. D3D12: BLAS builds, TLAS, trace (GPU-side fence waits on both sides).
	const std::int64_t executeStart = Qpc();
	cpuLast.d3d11 = static_cast<float>(static_cast<double>(executeStart - d3d11Start) * QpcMs());
	backend.SetExplicitFlush(settings.FlushBeforeTrace);  // (45k)
	const bool traced = backend.ExecuteFrame(input);
	const std::int64_t executeEnd = Qpc();
	cpuLast.execute = static_cast<float>(static_cast<double>(executeEnd - executeStart) * QpcMs());
	if (!traced && backend.HasFailed()) {
		context->CSSetShader(nullptr, nullptr, 0);
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
		historyValid = false;
		history4Valid = false;
		context->CSSetShader(nullptr, nullptr, 0);
		++frameIndex;
		return;
	}
	if (!scene.HasCoverage() && !filterLatched) {
		// The first BLASes are being built: leave the mask and Screen Space Shadows alone.
		context->CSSetShader(nullptr, nullptr, 0);
		status = Status::Waiting;
		statusReason = "building the scene";
		historyValid = false;
		history4Valid = false;
		++frameIndex;
		return;
	}

	// 2b. (45k) Screen Space Shadows "non-RT casters only", now that the trace marked the traced casters
	// (the D3D11 queue already waits for the trace on the GPU; nothing waits on the CPU).
	if (sssNonRT && sssClaimed) {
		gpuTimer.Pause();
		screenSpaceShadows->DrawDeferred(backend.VisibilitySRV(), settings.RtContactScale, biasConstant, biasPerUnit);
		sssClaimed = false;
		gpuTimer.Resume();
	}

	// 2c. (45k) Denoiser Penumbra: tiles, then the horizontal and vertical penumbra filter of this frame.
	if (penumbraReady) {
		auto* tileShader = GetShader(denoiseTileCS, L"Data\\Shaders\\RayTracing\\DenoiseTileCS.hlsl", "DenoiseTileCS");
		auto* hShader = GetShaderDefined(denoiseHCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS", nullptr);
		auto* vShader = GetShaderDefined(denoiseVCS, L"Data\\Shaders\\RayTracing\\DenoiseSpatialCS.hlsl", "DenoiseSpatialCS (vertical)", "DENOISE_VERTICAL");
		if (!tileShader || !hShader || !vShader) {
			logger::warn("[RayTracing] Denoiser Penumbra: a shader did not compile; Denoiser set to Off");
			settings.Denoiser = static_cast<int>(DenoiserMode::Off);
			history4Valid = false;
			combineFlags &= ~(kFlagPenumbra | kFlagHistoryValid);
			std::memcpy(&cb.Band[2], &combineFlags, sizeof(float));
			combineCB->Update(cb);
		} else {
			gpuTimer.Pause();
			Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::RayTracingDenoise);
			context->CSSetConstantBuffers(0, 1, cbs);
			ID3D11ShaderResourceView* nullSrvs[3]{};
			ID3D11UnorderedAccessView* nullUav[1]{};
			{
				ID3D11ShaderResourceView* srvs[2] = { backend.VisibilitySRV(), backend.DepthMaskSRV() };
				ID3D11UnorderedAccessView* uav = denoiseTiles.uav.get();
				context->CSSetShaderResources(0, 2, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->CSSetShader(tileShader, nullptr, 0);
				context->Dispatch(groupsX, groupsY, 1);
				context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
			}
			{
				ID3D11ShaderResourceView* srvs[3] = { backend.VisibilitySRV(), backend.DepthMaskSRV(), denoiseTiles.srv.get() };
				ID3D11UnorderedAccessView* uav = denoiseH.uav.get();
				context->CSSetShaderResources(0, 3, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->CSSetShader(hShader, nullptr, 0);
				context->Dispatch(groupsX, groupsY, 1);
				context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
				context->CSSetShaderResources(0, 3, nullSrvs);
			}
			{
				ID3D11ShaderResourceView* srvs[3] = { denoiseH.srv.get(), backend.DepthMaskSRV(), denoiseTiles.srv.get() };
				ID3D11UnorderedAccessView* uav = denoiseOut.uav.get();
				context->CSSetShaderResources(0, 3, srvs);
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->CSSetShader(vShader, nullptr, 0);
				context->Dispatch(groupsX, groupsY, 1);
				context->CSSetUnorderedAccessViews(0, 1, nullUav, nullptr);
				context->CSSetShaderResources(0, 3, nullSrvs);
			}
			Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::RayTracingDenoise);
			gpuTimer.Resume();
		}
	}
	const bool penumbraActive = (combineFlags & kFlagPenumbra) != 0u;

	// 3. Merge into the engine's mask, fade Screen Space Shadows inside the traced area.
	{
		const std::uint32_t readHistory = historyIndex, writeHistory = historyIndex ^ 1u;
		const std::uint32_t read4 = history4Index, write4 = history4Index ^ 1u;
		ID3D11ShaderResourceView* srvs[6] = { mask.SRV, backend.VisibilitySRV(), backend.DepthMaskSRV(), temporal ? historySRV[readHistory].get() : nullptr,
			penumbraActive ? denoiseOut.srv.get() : nullptr, penumbraActive ? history4[read4].srv.get() : nullptr };
		ID3D11UnorderedAccessView* uavs[5] = {
			maskTempUAV.get(),
			writeSSS ? screenSpaceShadows->screenSpaceShadowsTexture->uav.get() : nullptr,
			debug ? debugColorUAV.get() : nullptr,
			temporal ? historyUAV[writeHistory].get() : nullptr,
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
		if (temporal) {
			// (45j) This frame becomes the history of the next one.
			historyIndex = writeHistory;
			historyValid = true;
		}
		if (temporal || penumbraActive) {
			std::memcpy(prevViewProj, &fb.GetCameraViewProj(0), sizeof(prevViewProj));
			prevCamera[0] = camX;
			prevCamera[1] = camY;
			prevCamera[2] = camZ;
		}
	}
	cpuLast.d3d11 += static_cast<float>(static_cast<double>(Qpc() - executeEnd) * QpcMs());

	activeThisFrame = true;
	combinedThisFrame = true;
	if (debug)
		lastOverlayFrame = frameIndex;
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
						 "{} in the TLAS, {:.2f} M vertices skinned, {} refits / {} builds last frame, {:.0f} MB; left to the shadow map: physics off {}, "
						 "alpha off {}, alpha failed {}, shader {}, type {}, no CPU data {}, bad data {}, placement {}, budget/size {}; bone indices palette {}, global {}",
				ch.actors, ch.inRange, ch.registered, ch.shapes, ch.skinnedShapes, ch.rigidShapes, ch.physicsShapes, ch.alphaShapes, sk.drawsLastFrame,
				static_cast<double>(sk.verticesLastFrame) / 1e6, sk.refitsLastFrame, sk.buildsLastFrame,
				static_cast<double>(sk.bytes + sk.scratchBytes + sk.boneBytes) / (1024.0 * 1024.0), ch.skipPhysicsOff, ch.skipAlphaOff, ch.skipAlphaFailed,
				ch.skipShader, ch.skipGeometryType, ch.skipNoCpuData, ch.skipBadData, ch.skipPlacement, ch.skipBackend, ch.paletteMode, ch.globalMode);
		}
	}
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

		const char* qualityNames[3] = { kQuality[0].name, kQuality[1].name, kQuality[2].name };
		settings.QualityLevel = std::clamp(settings.QualityLevel, 0, 2);
		ImGui::Combo("Quality", &settings.QualityLevel, qualityNames, 3);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", kQuality[settings.QualityLevel].tooltip);

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
				const char* sssModes[] = { "Combine (min)", "Hand off (45i)", "Keep (multiply)", "Non-RT casters only" };
				settings.ScreenSpaceShadows = std::clamp(settings.ScreenSpaceShadows, 0, 3);
				ImGui::Combo("Screen-space shadows", &settings.ScreenSpaceShadows, sssModes, 4);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Screen Space Shadows (small contact shadows) inside the ray-traced area:");
					ImGui::Text("  Non-RT casters only (default): everything the ray tracer does not see (grass, distant trees, shadow-map kinds)");
					ImGui::Text("    keeps its contact shadows; ray-traced casters only fill the few units right at their base that the");
					ImGui::Text("    ray tracer cannot resolve (RT Caster Contact Length), so thin posts and trims are not fattened.");
					ImGui::Text("  Combine: kept; where they and the ray tracer agree the shadow is not darkened twice.");
					ImGui::Text("    Grass, leaves and characters left to the shadow map keep their contact shadows.");
					ImGui::Text("  Hand off: switched off (45i). Grass loses its contact shadows.");
					ImGui::Text("  Keep: kept and multiplied as with Ray Tracing off (shared shadows get a little darker).");
				}
				if (settings.ScreenSpaceShadows == static_cast<int>(SSSMode::HandOff)) {
					ImGui::Indent();
					ImGui::Checkbox("Keep them under leaves", &settings.KeepSSSUnderFoliage);
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::Text("Hand off: not where a sun ray passed ray-traced leaves (the traced leaf shadow can be thinner).");
					ImGui::Unindent();
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
			const char* denoisers[] = { "Off", "Temporal (45j)", "Penumbra (45k)", "DLSS RR" };
			static_assert(IM_ARRAYSIZE(denoisers) == static_cast<int>(DenoiserMode::Count));
			settings.Denoiser = std::clamp(settings.Denoiser, 0, static_cast<int>(DenoiserMode::Count) - 1);
			ImGui::Combo("Denoiser", &settings.Denoiser, denoisers, IM_ARRAYSIZE(denoisers));
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("How the grainy soft edges of the traced shadows are cleaned up (cost: F7 trace column rt_denoise_gpu_ms).");
				ImGui::Text("  Off: only the small blur of 45j.");
				ImGui::Text("  Temporal (45j): 45j's 'Temporal smoothing' exactly (blend with the last frames).");
				ImGui::Text("  Penumbra (default): blurs each soft edge as wide as it really is (wide under tall trees, none at");
				ImGui::Text("    contact shadows), then blends the last ~12 frames; extra rays only at shadow edges.");
				ImGui::Text("  DLSS RR: no cleanup of ours; fresh noise every frame for DLSS Ray Reconstruction to clean up.");
				ImGui::Text("    Needs Ray Reconstruction switched on in Upscaling (DLSS); otherwise the shadows stay grainy.");
			}
			const char* noiseModes[] = { "Auto", "World-locked", "Animated" };
			const char* adaptiveModes[] = { "Auto", "On", "Off" };
			ImGui::Indent();
			settings.Noise = std::clamp(settings.Noise, 0, 2);
			if (CurrentDenoiser() == DenoiserMode::DLSSRR)
				ImGui::BeginDisabled();
			ImGui::Combo("Noise", &settings.Noise, noiseModes, 3);
			if (CurrentDenoiser() == DenoiserMode::DLSSRR)
				ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Auto: Animated with Penumbra and DLSS RR (they average it over frames), World-locked with Off / Temporal (45j).");
				ImGui::Text("World-locked: each spot of the world keeps its rays while the camera moves (frozen grain).");
				ImGui::Text("Animated: new rays every frame (the history averages them). DLSS RR always uses Animated.");
			}
			settings.AdaptiveRays = std::clamp(settings.AdaptiveRays, 0, 2);
			ImGui::Combo("Adaptive Rays", &settings.AdaptiveRays, adaptiveModes, 3);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Extra rays only where a shadow edge is (Quality: Low +1, Medium +3, High +4); deep shadow and open light keep one.");
				ImGui::Text("Auto: on with Penumbra and DLSS RR, off with Off / Temporal (45j).");
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
			"History Length", "Adaptive Rays", "SSS: RT Casters" };
		static_assert(IM_ARRAYSIZE(views) == static_cast<int>(DebugView::Count));
		int view = static_cast<int>(debugView);
		if (ImGui::Combo("Debug View", &view, views, IM_ARRAYSIZE(views)))
			debugView = static_cast<DebugView>(std::clamp(view, 0, static_cast<int>(DebugView::Count) - 1));
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Shadow Source: red = ray-traced shadow, blue = shadow-map shadow (characters, trees, grass, terrain), green = lit,");
			ImGui::Text("  orange = Screen Space Shadows had a contact shadow here that the hand-off removed,");
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
	static constexpr const char* kDenoisers[] = { "off", "temporal (45j)", "penumbra", "DLSS RR" };
	static constexpr const char* kAuto[3][3] = { { "auto", "world-locked", "animated" }, { "auto", "on", "off" }, { "", "", "" } };
	auto mode = [](bool a_rt) { return a_rt ? "RT" : "SM"; };
	return std::format("RT {} ({}), quality {}, buildings {}, objects {}, trees {}, foliage {}, cut-outs {}, characters {}, physics parts {}, hair {}, "
					   "first-person player {}, SSS {} (contact {:.1f}), denoiser {}, noise {}, adaptive rays {}, ray offset {:.2f}, D3D11 flush {}, loading build {}, warm-up {}",
		settings.Enabled ? "on" : "off", kStatus[static_cast<int>(status)], kQuality[std::clamp(settings.QualityLevel, 0, 2)].name, mode(settings.Buildings),
		mode(settings.Objects), mode(settings.Trees), kFoliage[std::clamp(settings.Foliage, 0, 2)], mode(settings.Cutouts), mode(settings.Characters),
		mode(settings.PhysicsParts), mode(settings.Hair), settings.FirstPersonPlayer ? "RT" : "SM", kSSS[std::clamp(settings.ScreenSpaceShadows, 0, 3)],
		settings.RtContactScale, kDenoisers[static_cast<int>(CurrentDenoiser())], kAuto[0][std::clamp(settings.Noise, 0, 2)], kAuto[1][std::clamp(settings.AdaptiveRays, 0, 2)],
		settings.RayOffsetScale, settings.FlushBeforeTrace ? "on" : "off", settings.LoadingBoost ? "on" : "off", settings.WarmUp ? "on" : "off");
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
	j["denoise_gpu_ms"] = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::RayTracingDenoise);
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
