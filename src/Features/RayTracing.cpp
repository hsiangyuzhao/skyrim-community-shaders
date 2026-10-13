#include "RayTracing.h"

#include "Deferred.h"
#include "Features/RayTracing/CasterFilter.h"
#include "Features/ScreenSpaceShadows.h"
#include "State.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	RayTracing::Settings,
	Enabled,
	QualityLevel,
	KeepCastersInShadowMap,
	KeepScreenSpaceShadows)

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
	constexpr std::uint32_t kFlagSkipShadowed = 1u, kFlagCoverage = 2u;  // TraceShadowsCS
	constexpr std::uint32_t kFlagHandOffSSS = 1u, kFlagDebug = 2u;       // Common.hlsli

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
}

// ---------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------

void RayTracing::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.QualityLevel = std::clamp(settings.QualityLevel, 0, 2);
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
	const bool active = settings.Enabled && status == Status::Active && !settings.KeepCastersInShadowMap &&
	                    debugView != DebugView::Disagreement && !Util::IsInterior();
	RayTracingCasterFilter::Latch(active, filterRect);
	filterLatched = RayTracingCasterFilter::IsLatched();
}

bool RayTracing::ComputeRects(float o_traceRect[4], float o_coreRect[4], double a_camX, double a_camY)
{
	float world[4];
	if (!scene.GetLoadedRect(world))
		return false;
	o_traceRect[0] = static_cast<float>(world[0] - a_camX);
	o_traceRect[1] = static_cast<float>(world[1] - a_camY);
	o_traceRect[2] = static_cast<float>(world[2] - a_camX);
	o_traceRect[3] = static_cast<float>(world[3] - a_camY);
	// Casters fully inside this world rect leave the cascades (CasterFilter): it stops short of the
	// hand-off band, so the band, where the shadow map still counts, keeps their shadow-map shadow.
	const float inset = kHandOffBand + 256.0f;
	o_coreRect[0] = world[0] + inset;
	o_coreRect[1] = world[1] + inset;
	o_coreRect[2] = world[2] - inset;
	o_coreRect[3] = world[3] - inset;
	return true;
}

void RayTracing::Prepass()
{
	activeThisFrame = false;
	combinedThisFrame = false;
	const std::int64_t start = Qpc();

	if (!settings.Enabled) {
		if (status != Status::Off) {
			// Switched off: the scene goes (BLASes retire on the GPU's schedule); the device stays.
			RayTracingCasterFilter::Clear();
			if (backend.IsReady())
				scene.ReleaseAll(backend);
			status = Status::Off;
			statusReason.clear();
			logger::info("[RayTracing] Switched off");
		}
		// Switching back on is a fresh attempt (also after a failure), never a silent "Starting".
		backendTried = false;
		if (backend.IsReady())
			backend.Tick();
		traceSample = {};
		return;
	}
	if (status == Status::Unavailable || status == Status::Failed)
		return;

	try {
		RunFrame();
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
	if (status == Status::Off || status == Status::Unavailable)
		status = Status::Waiting;

	auto* sky = globals::game::sky;
	if (Util::IsInterior() || !sky || sky->mode.get() != RE::Sky::Mode::kFull) {
		status = Status::Inactive;
		statusReason = Util::IsInterior() ? "interior" : "no sky";
		scene.Update(backend, kCaptureBudgetMs, globals::state->frameCount);  // drops exterior cells in interiors
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
	scene.CollectInstances(instances);

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
	input.renderWidth = width;
	input.renderHeight = height;
	input.cameraX = camX;
	input.cameraY = camY;
	input.cameraZ = camZ;
	input.instances = &instances;
	input.buildTriangleBudget = kBuildBudget;

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
		// Every D3D12 context still busy: reuse the previous trace once rather than lose the
		// static shadows the cascades no longer hold this frame.
		if (staleFrames > 0 || frameIndex == 0) {
			context->CSSetShader(nullptr, nullptr, 0);
			++frameIndex;
			return;
		}
		++staleFrames;
	} else {
		staleFrames = 0;
	}
	if (!scene.HasCoverage()) {
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
			ImGui::Text("On: sun shadows of buildings, rocks, furniture and the ground inside the loaded area are ray traced.");
			ImGui::Text("Characters, trees and grass keep their normal shadows. Screen Space Shadows are switched off where ray tracing is in charge.");
			ImGui::Text("Off: the game's shadows exactly as before (nothing of this feature runs).");
		}

		const char* qualityNames[3] = { kQuality[0].name, kQuality[1].name, kQuality[2].name };
		settings.QualityLevel = std::clamp(settings.QualityLevel, 0, 2);
		ImGui::Combo("Quality", &settings.QualityLevel, qualityNames, 3);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("%s", kQuality[settings.QualityLevel].tooltip);

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
			ImGui::Text("Shadow Source: red = ray-traced shadow, blue = shadow-map shadow (characters, trees, grass), green = lit,");
			ImGui::Text("  grey = outside the ray-traced area (cascades and terrain shadows, unchanged), yellow = hand-off band.");
			ImGui::Text("  Terrain shadows are applied later by the lighting and are not shown.");
			ImGui::Text("RT vs Shadow Map: inside the ray-traced area, red = only RT sees a shadow, blue = only the shadow map does.");
			ImGui::Text("  While this view is on, the shadow map keeps every caster so the two can be compared.");
			ImGui::Text("BLAS Coverage: green = the surface is in the ray-traced scene, magenta = it is not (character, tree, grass, skipped object),");
			ImGui::Text("  orange = the ray-traced scene has a surface in front of what you see (geometry in the wrong place).");
			ImGui::Text("Raw RT Visibility: the traced result before smoothing and merging.");
			ImGui::Text("RT Hit vs Depth: white = the ray-traced surface sits exactly where the game draws it,");
			ImGui::Text("  red = it is in front (full red = 256 units), blue = behind, black = nothing there.");
		}
		ImGui::Checkbox("Keep Static Casters in the Shadow Map", &settings.KeepCastersInShadowMap);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("A/B: normally objects the ray tracer covers are left out of the sun shadow map. On = they stay (the darker of both wins).");
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
		ImGui::Text("Scene: %u shapes, %u ground quads, %.2f M triangles", sc.shapes, sc.landQuads, static_cast<double>(sc.triangles) / 1e6);
		ImGui::Text("BLAS: %u built (%.2f M triangles), %u queued, %u instances, %llu builds total", rt.blasCount,
			static_cast<double>(rt.blasTriangles) / 1e6, rt.blasQueued, rt.instancesLastFrame, static_cast<unsigned long long>(rt.buildsTotal));
		const std::uint64_t vram = rt.blasBytes + rt.scratchBytes + rt.tlasBytes + rt.uploadBytes + rt.sharedTextureBytes + d3d11Bytes;
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
		if (ImGui::SmallButton("Reset worst"))
			scene.ResetPeaks();
		ImGui::Text("Shadow map: %u casters handed to RT, %u draws left out last frame%s", RayTracingCasterFilter::RegisteredCount(),
			RayTracingCasterFilter::SkippedLastFrame(), filterLatched ? "" : " (not active)");
		if (rt.skippedFramesBusy || staleFrames)
			ImGui::Text("Frames without a new trace (D3D12 busy): %u", rt.skippedFramesBusy);
		if (ImGui::TreeNodeEx("Skipped objects")) {
			ImGui::Text("Characters %u, doors/activators/movable %u, trees/plants %u, items/markers %u, disabled/no 3D %u",
				skip.actors, skip.movable, skip.foliage, skip.items, skip.disabled);
			ImGui::Text("Shapes: hidden %u, alpha-tested %u, alpha-blended %u, decals %u, skinned %u, no shadow %u",
				skip.hidden, skip.alphaTested, skip.alphaBlended, skip.decals, skip.skinned, skip.noShadow);
			ImGui::Text("Shapes: not Lighting shader %u, other geometry types %u, no CPU vertex copy %u, bad data %u",
				skip.notLighting, skip.otherGeometry, skip.noCpuData, skip.badData);
			ImGui::Text("Placement check: %u shapes checked, %u fixed (other vertex format), %u left out (wrong place), %u without bound",
				sc.placementChecked, sc.placementFixed, sc.placementRejected, sc.placementUnchecked);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Each captured shape must fit inside the bound sphere the game computed for it. Details of the first misfits are in the log.");
			ImGui::TreePop();
		}
		ImGui::TreePop();
	}
}
