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
	CharacterSelfShadowOffset)

namespace
{
	struct QualityPreset
	{
		const char* name;
		std::uint32_t rays;
		float coneDegrees;  // half angle; the sun disc is about 0.27
		std::uint32_t filterRadius;
		const char* tooltip;
	};

	constexpr QualityPreset kQuality[3] = {
		{ "Low", 1, 0.27f, 1, "1 ray per pixel, sharp sun (the real sun disc), light 3x3 smoothing. Cheapest." },
		{ "Medium", 1, 0.5f, 2, "1 ray per pixel, slightly softer edges, 5x5 smoothing." },
		{ "High", 2, 0.5f, 2, "2 rays per pixel, slightly softer edges, 5x5 smoothing. Cleanest, about twice the tracing cost." },
	};

	constexpr float kCaptureBudgetMs = 1.5f;      // CPU per frame for capturing new cells
	constexpr std::uint32_t kBuildBudget = 131072;  // triangles of BLAS builds per frame (about one chunk)
	constexpr float kHandOffBand = 2048.0f;       // half a cell: RT fades back to the cascades here
	constexpr float kMaxRayLength = 16384.0f;
	constexpr std::uint64_t kAccelerationBudgetBytes = 1200ull << 20;  // BLAS + scratch; spec target is 1.5 GB in total
	constexpr std::uint32_t kFlagSkipShadowed = 1u, kFlagCoverage = 2u, kFlagFirstPerson = 4u;  // TraceShadowsCS
	constexpr std::uint32_t kFlagHandOffSSS = 1u, kFlagDebug = 2u;       // Common.hlsli
	constexpr std::uint32_t kAlphaCopiesPerFrame = 16;  // (45h) 256x256 alpha copies made per frame (5 small dispatches each)

	struct LoadingMenuSink : RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
		RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			// Load game, fast travel, door into another worldspace: every captured cell is stale.
			if (a_event && a_event->opening && a_event->menuName == RE::LoadingMenu::MENU_NAME)
				globals::features::rayTracing.OnLoadingScreen();
			return RE::BSEventNotifyControl::kContinue;
		}
	};
	LoadingMenuSink g_loadingSink;

	std::string Megabytes(std::uint64_t a_bytes) { return std::format("{:.1f} MB", static_cast<double>(a_bytes) / (1024.0 * 1024.0)); }

	struct GpuBucketScope
	{
		explicit GpuBucketScope(Util::GpuBucket a_bucket) :
			bucket(a_bucket) { Util::GpuPassTimers::GetSingleton()->Begin(bucket); }
		~GpuBucketScope() { Util::GpuPassTimers::GetSingleton()->End(bucket); }
		GpuBucketScope(const GpuBucketScope&) = delete;
		GpuBucketScope& operator=(const GpuBucketScope&) = delete;
		Util::GpuBucket bucket;
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

void RayTracing::OnLoadingScreen()
{
	scene.RequestFlush();
	characters.RequestFlush();  // (45i)
}

void RayTracing::UpdateAlphaCasters()
{
	// (45h) Turned on (again) by the user: an earlier automatic switch-off is forgotten.
	if (settings.AlphaCasters && !alphaRequested) {
		backend.ClearAlphaFault();
		alphaOffReason.clear();
	}
	// Any failure of its D3D12 / D3D11 resources switches it off (the rest of Ray Tracing goes on).
	if (settings.AlphaCasters && !backend.AlphaFault().empty()) {
		alphaOffReason = backend.AlphaFault();
		settings.AlphaCasters = false;
		logger::warn("[RayTracing] Alpha-tested casters switched off: {}", alphaOffReason);
	} else if (settings.AlphaCasters && backend.IsReady() && !backend.AlphaAvailable()) {
		alphaOffReason = "the GPU driver did not accept the alpha-tested ray query shader";
		settings.AlphaCasters = false;
		logger::warn("[RayTracing] Alpha-tested casters switched off: {}", alphaOffReason);
	}
	alphaRequested = settings.AlphaCasters;
	const bool alpha = settings.AlphaCasters && backend.AlphaAvailable();
	scene.SetAlphaCasters(alpha);  // a change re-captures the scene
	if (!alpha && backend.IsReady() && backend.AlphaIdle())
		backend.ReleaseAlphaResources();
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
	// Alpha-tested parts (hair) need the Alpha-tested casters switch; a change re-captures them.
	characters.Configure(settings.PhysicsParts, settings.AlphaCasters && backend.AlphaAvailable(), settings.KeepCharactersInShadowMap);
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void RayTracing::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.QualityLevel = std::clamp(settings.QualityLevel, 0, 2);
	settings.CharacterSelfShadowOffset = std::isfinite(settings.CharacterSelfShadowOffset) ? std::clamp(settings.CharacterSelfShadowOffset, 0.0f, 32.0f) : 6.0f;
	// (45h) Before the first save is loaded (no frame runs during loading screens): its textures are
	// then created shareable when both switches are on.
	RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.AlphaCasters);
}

void RayTracing::SaveSettings(json& o_json)
{
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
	maskDesc = {};
	d3d11Bytes = 0;
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
	const bool active = settings.Enabled && status == Status::Active && !settings.KeepCastersInShadowMap &&
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
	RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.AlphaCasters);

	if (!settings.Enabled) {
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
	if (status == Status::Unavailable || status == Status::Failed)
		return;

	try {
		RunFrame();
		// (45e) Captures that left the scene this frame go only now, after the trace was submitted.
		if (backend.IsReady()) {
			scene.EndFrame(backend);
			characters.EndFrame(backend);  // (45i)
		}
	} catch (const std::exception& e) {
		Disable(Status::Failed, std::format("exception: {}", e.what()));
	}

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
}

void RayTracing::RunFrame()
{
	if (!EnsureBackend())
		return;
	scene.SetCutoutAsOpaque(settings.CutoutAsOpaque);  // (45g) a change re-captures the scene
	UpdateAlphaCasters();                              // (45h) likewise
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
	skinDraws.clear();
	skinBones.clear();
	if (settings.Characters && backend.SkinAvailable()) {
		float world[4];
		if (scene.GetLoadedRect(world, false))
			characters.Update(backend, globals::state->frameCount, world);
		// Every captured (and retiring) shape is traced, also on a frame without an update.
		characters.CollectDraws(skinDraws, skinBones);
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
	input.constants.params[0] = static_cast<float>(preset.rays);
	input.constants.params[1] = static_cast<float>(frameIndex % 1024);
	std::memcpy(&input.constants.params[2], &traceFlags, sizeof(float));
	input.constants.params[3] = kMaxRayLength;
	input.constants.renderSize[0] = static_cast<float>(width);
	input.constants.renderSize[1] = static_cast<float>(height);
	input.constants.renderSize[2] = 1.0f / static_cast<float>(width);
	input.constants.renderSize[3] = 1.0f / static_cast<float>(height);
	std::copy_n(traceRect, 4, input.constants.rect);
	input.constants.bias[0] = 2.0f;
	input.constants.bias[1] = 0.0015f;
	input.constants.bias[2] = 8.0f;
	input.constants.bias[3] = skinDraws.empty() ? 0.0f : settings.CharacterSelfShadowOffset;  // (45i) character ray TMin (character variants only)
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
	const bool handOffSSS = !settings.KeepScreenSpaceShadows && typedUavLoadR8 && screenSpaceShadows->loaded &&
	                        screenSpaceShadows->bendSettings.Enable && screenSpaceShadows->screenSpaceShadowsTexture;
	const bool debug = debugView != DebugView::Off;

	CombineCB cb{};
	std::memcpy(cb.InvViewProj, &invViewProj, sizeof(cb.InvViewProj));
	std::copy_n(traceRect, 4, cb.Rect);
	cb.Band[0] = kHandOffBand;
	cb.Band[1] = static_cast<float>(preset.filterRadius);
	const std::uint32_t combineFlags = (handOffSSS ? kFlagHandOffSSS : 0u) | (debug ? kFlagDebug : 0u);
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
	if (settings.AlphaCasters && backend.AlphaAvailable()) {
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
				settings.AlphaCasters = false;
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
	const bool traced = backend.ExecuteFrame(input);
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
		context->CSSetShader(nullptr, nullptr, 0);
		++frameIndex;
		return;
	}
	if (!scene.HasCoverage() && !filterLatched) {
		// The first BLASes are being built: leave the mask and Screen Space Shadows alone.
		context->CSSetShader(nullptr, nullptr, 0);
		status = Status::Waiting;
		statusReason = "building the scene";
		++frameIndex;
		return;
	}

	// 3. Merge into the engine's mask, fade Screen Space Shadows inside the traced area.
	{
		ID3D11ShaderResourceView* srvs[3] = { mask.SRV, backend.VisibilitySRV(), backend.DepthMaskSRV() };
		ID3D11UnorderedAccessView* uavs[3] = {
			maskTempUAV.get(),
			handOffSSS ? screenSpaceShadows->screenSpaceShadowsTexture->uav.get() : nullptr,
			debug ? debugColorUAV.get() : nullptr
		};
		// Screen Space Shadows bound its texture to PS t45 in its own Prepass; a UAV write needs it unbound.
		ID3D11ShaderResourceView* nullPs[1]{};
		if (handOffSSS)
			context->PSSetShaderResources(45, 1, nullPs);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 3, uavs, nullptr);
		context->CSSetShader(combine, nullptr, 0);
		context->Dispatch(groupsX, groupsY, 1);
		ID3D11ShaderResourceView* nullSrvs[3]{};
		ID3D11UnorderedAccessView* nullUavs[3]{};
		context->CSSetShaderResources(0, 3, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 3, nullUavs, nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
		ID3D11Buffer* nullCb[1]{};
		context->CSSetConstantBuffers(0, 1, nullCb);
		if (handOffSSS) {
			ID3D11ShaderResourceView* sssView = screenSpaceShadows->screenSpaceShadowsTexture->srv.get();
			context->PSSetShaderResources(45, 1, &sssView);
		}
		// Only the render area was written; the rest of the work texture is never read.
		D3D11_BOX box{ 0, 0, 0, width, height, 1 };
		context->CopySubresourceRegion(mask.texture, 0, 0, 0, 0, maskTemp.get(), 0, &box);
	}

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
			ImGui::Text("On: sun shadows of buildings, rocks, furniture, doors, items and tree trunks inside the loaded area are ray traced.");
			ImGui::Text("Characters and leaves (unless switched on below), grass and the terrain itself (hills) keep their normal shadows; the ground still receives traced shadows.");
			ImGui::Text("Screen Space Shadows are switched off where ray tracing is in charge.");
			ImGui::Text("Off: the game's shadows exactly as before (nothing of this feature runs).");
		}

		const char* qualityNames[3] = { kQuality[0].name, kQuality[1].name, kQuality[2].name };
		settings.QualityLevel = std::clamp(settings.QualityLevel, 0, 2);
		ImGui::Combo("Quality", &settings.QualityLevel, qualityNames, 3);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", kQuality[settings.QualityLevel].tooltip);

		// (45h)
		ImGui::Checkbox("Alpha-tested casters (foliage)", &settings.AlphaCasters);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: leaves, bushes, banners, cloth awnings, fences and other cut-out shapes cast ray-traced sun shadows");
			ImGui::Text("  that follow their cut-out (the holes between the leaves stay lit). Grass keeps its normal shadows.");
			ImGui::Text("  Leaves are traced where the tree stands still: their sway in the wind is not followed.");
			ImGui::Text("  Textures loaded while this is on are read by the ray tracer directly; others get a small 256x256 copy.");
			ImGui::Text("  Most textures are loaded with the save, so a restart (or loading a save) after switching on gives the sharpest result.");
			ImGui::Text("Off: these shapes keep their shadow-map shadow, exactly as before. Switching re-captures the scene.");
		}
		RayTracingTextureSharing::SetEnabled(settings.Enabled && settings.AlphaCasters);  // also from the main menu
		if (!alphaOffReason.empty()) {
			ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{ 1.0f, 0.4f, 0.3f, 1.0f });
			ImGui::TextWrapped("Alpha-tested casters were switched off automatically: %s", alphaOffReason.c_str());
			ImGui::PopStyleColor();
		}

		// (45i)
		ImGui::Checkbox("Characters", &settings.Characters);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("On: the player and nearby NPCs and creatures (up to %u, within 40 m, inside the ray-traced area) cast ray-traced sun shadows:",
				RayTracingCharacters::kMaxActors);
			ImGui::Text("  body, head, clothes, armour and weapons, posed every frame (skinned on the GPU).");
			ImGui::Text("  Hair and other cut-out parts need Alpha-tested casters too; hair and HDT-SMP parts need Physics parts.");
			ImGui::Text("  Anything that cannot be traced keeps its normal shadow-map shadow.");
			ImGui::Text("Off: characters keep their shadow-map shadow, exactly as before.");
		}
		{
			ImGui::Indent();
			if (!settings.Characters)
				ImGui::BeginDisabled();
			ImGui::Checkbox("Physics parts (HDT-SMP, hair)", &settings.PhysicsParts);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("On: hair, capes, skirts and other parts moved by HDT-SMP / HDT-PE are ray traced as well (with the bones' pose of the frame).");
				ImGui::Text("  Hair is alpha-tested: it also needs Alpha-tested casters, otherwise it keeps its shadow-map shadow.");
				ImGui::Text("  CBPC / body physics needs nothing extra: it moves the body's own bones, which Characters follows.");
				ImGui::Text("Off: these parts keep their shadow-map shadow. Switching re-captures the characters.");
			}
			if (!settings.Characters)
				ImGui::EndDisabled();
			ImGui::Unindent();
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
		const char* views[] = { "Off", "Shadow Source", "RT vs Shadow Map", "BLAS Coverage", "Raw RT Visibility", "RT Hit vs Depth" };
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
		ImGui::Checkbox("Keep Screen Space Shadows", &settings.KeepScreenSpaceShadows);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("A/B: normally Screen Space Shadows (sun contact shadows) are switched off where ray tracing is in charge.");
		if (!typedUavLoadR8)
			ImGui::TextDisabled("This GPU cannot hand Screen Space Shadows off; they stay on.");

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
			ImGui::Text("Characters: %u actors (%u in range), %u shapes traced of %u captured (%u skinned, %u rigid, %u physics, %u alpha-tested)",
				ch.actors, ch.inRange, ch.registered, ch.shapes, ch.skinnedShapes, ch.rigidShapes, ch.physicsShapes, ch.alphaShapes);
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
