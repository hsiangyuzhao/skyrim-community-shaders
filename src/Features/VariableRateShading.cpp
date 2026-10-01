#include "VariableRateShading.h"

#include "Deferred.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Batch36.h"
#include "Utils/GpuTimers.h"

// NVAPI is only used for its types; every entry point is resolved at runtime through the
// driver's nvapi_QueryInterface, so nothing is linked and a machine without an NVIDIA driver
// simply never finds nvapi64.dll.
#pragma warning(push, 0)
#include <nvapi.h>
#pragma warning(pop)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VariableRateShading::Settings,
	Enabled,
	RateMode,
	Quality,
	CoarsestRate,
	MotionPixels,
	ProtectNormals,
	IncludeGrass,
	IncludeAlphaTested,
	PeripheryRadius,
	DebugOverlay)

namespace
{
	constexpr uint32_t kTileSize = NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH;
	static_assert(NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH == NV_VARIABLE_PIXEL_SHADING_TILE_HEIGHT);

	// Function ids from nvapi_interface.h (same vendored SDK as nvapi.h).
	constexpr uint32_t kIdInitialize = 0x0150e828;
	constexpr uint32_t kIdGetGraphicsCapabilities = 0x52b1499a;
	constexpr uint32_t kIdSetViewportsPixelShadingRates = 0x34f7938f;
	constexpr uint32_t kIdCreateShadingRateResourceView = 0x99ca2dff;
	constexpr uint32_t kIdSetShadingRateResourceView = 0x1b0c2f83;

	using QueryInterfaceFn = void*(__cdecl*)(uint32_t);
	using InitializeFn = NvAPI_Status(__cdecl*)();
	using GetCapsFn = NvAPI_Status(__cdecl*)(IUnknown*, NvU32, NV_D3D1x_GRAPHICS_CAPS*);
	using SetRatesFn = NvAPI_Status(__cdecl*)(IUnknown*, NV_D3D11_VIEWPORTS_SHADING_RATE_DESC*);
	using CreateViewFn = NvAPI_Status(__cdecl*)(ID3D11Device*, ID3D11Resource*, NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC*, ID3D11NvShadingRateResourceView**);
	using SetViewFn = NvAPI_Status(__cdecl*)(IUnknown*, ID3D11NvShadingRateResourceView*);

	struct Nvapi
	{
		bool ok = false;
		std::string error;
		GetCapsFn getCaps = nullptr;
		SetRatesFn setRates = nullptr;
		CreateViewFn createView = nullptr;
		SetViewFn setView = nullptr;
	};

	/// Loaded once per process. NvAPI_Initialize is reference counted, so it coexists with
	/// Streamline and anything else in the process that also initialises NVAPI.
	const Nvapi& GetNvapi()
	{
		static const Nvapi nvapi = [] {
			Nvapi result;
			HMODULE module = LoadLibraryW(L"nvapi64.dll");
			if (!module) {
				result.error = "No NVIDIA driver interface (nvapi64.dll) found; VRS needs an NVIDIA RTX GPU.";
				return result;
			}
			auto queryInterface = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(module, "nvapi_QueryInterface"));
			if (!queryInterface) {
				result.error = "nvapi64.dll has no nvapi_QueryInterface export.";
				return result;
			}
			auto initialize = reinterpret_cast<InitializeFn>(queryInterface(kIdInitialize));
			result.getCaps = reinterpret_cast<GetCapsFn>(queryInterface(kIdGetGraphicsCapabilities));
			result.setRates = reinterpret_cast<SetRatesFn>(queryInterface(kIdSetViewportsPixelShadingRates));
			result.createView = reinterpret_cast<CreateViewFn>(queryInterface(kIdCreateShadingRateResourceView));
			result.setView = reinterpret_cast<SetViewFn>(queryInterface(kIdSetShadingRateResourceView));
			if (!initialize || !result.getCaps || !result.setRates || !result.createView || !result.setView) {
				result.error = "The NVIDIA driver does not expose the D3D11 VRS functions; update the driver.";
				return result;
			}
			if (const NvAPI_Status status = initialize(); status != NVAPI_OK) {
				result.error = std::format("NvAPI_Initialize failed ({}).", static_cast<int>(status));
				return result;
			}
			result.ok = true;
			return result;
		}();
		return nvapi;
	}

	/// Rate-image index (log2 width * 3 + log2 height) to hardware rate. 1x4 and 4x1 do not
	/// exist; the build pass never emits them, and if it ever did they map to the finer rate.
	constexpr NV_PIXEL_SHADING_RATE kFullTable[NV_MAX_PIXEL_SHADING_RATES] = {
		NV_PIXEL_X1_PER_RASTER_PIXEL,       // 0: 1x1
		NV_PIXEL_X1_PER_1X2_RASTER_PIXELS,  // 1: 1x2
		NV_PIXEL_X1_PER_1X2_RASTER_PIXELS,  // 2: 1x4 -> 1x2
		NV_PIXEL_X1_PER_2X1_RASTER_PIXELS,  // 3: 2x1
		NV_PIXEL_X1_PER_2X2_RASTER_PIXELS,  // 4: 2x2
		NV_PIXEL_X1_PER_2X4_RASTER_PIXELS,  // 5: 2x4
		NV_PIXEL_X1_PER_2X1_RASTER_PIXELS,  // 6: 4x1 -> 2x1
		NV_PIXEL_X1_PER_4X2_RASTER_PIXELS,  // 7: 4x2
		NV_PIXEL_X1_PER_4X4_RASTER_PIXELS,  // 8: 4x4
		NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL,
		NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL,
		NV_PIXEL_X1_PER_RASTER_PIXEL
	};

	/// Alpha-tested draws: the same image capped at 2x2. A discard in a coarse pixel removes the
	/// whole block, so a 4-pixel step on a leaf or grass edge is far more visible than on a wall.
	constexpr NV_PIXEL_SHADING_RATE kAlphaTestedTable[NV_MAX_PIXEL_SHADING_RATES] = {
		NV_PIXEL_X1_PER_RASTER_PIXEL,
		NV_PIXEL_X1_PER_1X2_RASTER_PIXELS,
		NV_PIXEL_X1_PER_1X2_RASTER_PIXELS,
		NV_PIXEL_X1_PER_2X1_RASTER_PIXELS,
		NV_PIXEL_X1_PER_2X2_RASTER_PIXELS,
		NV_PIXEL_X1_PER_2X2_RASTER_PIXELS,  // 2x4 -> 2x2
		NV_PIXEL_X1_PER_2X1_RASTER_PIXELS,
		NV_PIXEL_X1_PER_2X2_RASTER_PIXELS,  // 4x2 -> 2x2
		NV_PIXEL_X1_PER_2X2_RASTER_PIXELS,  // 4x4 -> 2x2
		NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL,
		NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL, NV_PIXEL_X1_PER_RASTER_PIXEL,
		NV_PIXEL_X1_PER_RASTER_PIXEL
	};

	/// Pixels covered by one shading result at a rate-image index, after the table mapping.
	constexpr uint32_t kRateArea[9] = { 1, 2, 2, 2, 4, 8, 2, 8, 16 };

	constexpr const char* kRateNames[9] = { "1x1", "1x2", "1x4", "2x1", "2x2", "2x4", "4x1", "4x2", "4x4" };

	// Debug overlay colours, matching DebugOverlayCS.hlsl.
	const ImVec4 kRateColours[9] = {
		ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
		ImVec4(1.0f, 0.55f, 0.0f, 1.0f),
		ImVec4(1.0f, 0.55f, 0.0f, 1.0f),
		ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
		ImVec4(0.0f, 1.0f, 0.0f, 1.0f),
		ImVec4(0.1f, 0.3f, 1.0f, 1.0f),
		ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
		ImVec4(0.0f, 0.9f, 1.0f, 1.0f),
		ImVec4(1.0f, 0.0f, 0.8f, 1.0f),
	};

	// Tuning constants behind the user-facing sliders.
	constexpr float kThresholdAtLowestQuality = 0.15f;  // relative RMS error allowed for half rate at Quality 0
	constexpr float kThresholdRange = 0.1f;             // Quality 1 is 10x stricter than Quality 0
	constexpr float kQuarterFactor = 2.13f;             // NVIDIA Adaptive Shading: quarter-rate error ~2.13x half-rate
	constexpr float kNormalWeight = 0.5f;               // normal chord (~radians) to luminance-error units
	constexpr float kEnvLuminance = 0.02f;              // keeps near-black tiles from reading as infinitely detailed
	constexpr float kHysteresis = 0.8f;                 // must beat 80% of the threshold to get coarser than last frame

	uint32_t DivideRoundUp(uint32_t a_value, uint32_t a_divisor) { return (a_value + a_divisor - 1) / a_divisor; }

	struct GpuTimerScope
	{
		GpuTimerScope() { Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::VariableRateShading); }
		~GpuTimerScope() { Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::VariableRateShading); }
		GpuTimerScope(const GpuTimerScope&) = delete;
		GpuTimerScope& operator=(const GpuTimerScope&) = delete;
	};

	void UnbindCompute(ID3D11DeviceContext* a_context, UINT a_srvCount, UINT a_uavCount)
	{
		ID3D11ShaderResourceView* nullSRVs[3]{};
		ID3D11UnorderedAccessView* nullUAVs[2]{};
		ID3D11Buffer* nullCB = nullptr;
		a_context->CSSetShaderResources(0, a_srvCount, nullSRVs);
		a_context->CSSetUnorderedAccessViews(0, a_uavCount, nullUAVs, nullptr);
		a_context->CSSetConstantBuffers(0, 1, &nullCB);
		a_context->CSSetShader(nullptr, nullptr, 0);
	}
}

void VariableRateShading::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.RateMode = std::min(settings.RateMode, static_cast<uint32_t>(Mode::AdaptivePeriphery));
	settings.CoarsestRate = std::min(settings.CoarsestRate, 1u);
	settings.Quality = std::clamp(settings.Quality, 0.0f, 1.0f);
	settings.MotionPixels = std::clamp(settings.MotionPixels, 0.0f, 64.0f);
	settings.PeripheryRadius = std::clamp(settings.PeripheryRadius, 0.1f, 2.0f);
}

void VariableRateShading::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VariableRateShading::RestoreDefaultSettings()
{
	settings = {};
}

bool VariableRateShading::IsActive() const
{
	// The Batch 36 master switch can only force VRS off, never on.
	return loaded && settings.Enabled && Batch36::IsOn() && hardwareSupported && !shaderFailed && rateCB;
}

void VariableRateShading::SetupResources()
{
	// Re-entered on every render-target re-creation: drop the previous generation first.
	EndOpaquePass();
	for (auto& view : rateImageView) {
		if (view) {
			view->Release();
			view = nullptr;
		}
	}
	tileStats = nullptr;
	tileStatsSRV = nullptr;
	tileStatsUAV = nullptr;
	for (uint32_t i = 0; i < 2; i++) {
		rateImage[i] = nullptr;
		rateImageSRV[i] = nullptr;
		rateImageUAV[i] = nullptr;
	}
	rateCounts = nullptr;
	rateCountsUAV = nullptr;
	for (uint32_t i = 0; i < kReadbackSlots; i++) {
		rateCountsStaging[i] = nullptr;
		readbackPending[i] = false;
	}
	hasRateCounts = false;
	lastAnalysisFrame = 0;
	lastBuildFrame = 0;
	hardwareSupported = false;

	if (REL::Module::IsVR()) {
		unavailableReason = "Not supported in VR.";
		return;
	}

	const Nvapi& nvapi = GetNvapi();
	if (!nvapi.ok) {
		unavailableReason = nvapi.error;
		logger::info("[VRS] Unavailable: {}", unavailableReason);
		return;
	}

	auto device = globals::d3d::device;

	NV_D3D1x_GRAPHICS_CAPS caps{};
	const NvAPI_Status capsStatus = nvapi.getCaps(device, NV_D3D1x_GRAPHICS_CAPS_VER, &caps);
	if (capsStatus != NVAPI_OK || !caps.bVariablePixelRateShadingSupported) {
		unavailableReason = capsStatus != NVAPI_OK ?
		                        std::format("Could not query GPU capabilities ({}); the game device may not be a native NVIDIA D3D11 device.", static_cast<int>(capsStatus)) :
		                        "This GPU does not support variable rate shading (needs RTX 20 series or newer).";
		logger::info("[VRS] Unavailable: {}", unavailableReason);
		return;
	}

	D3D11_TEXTURE2D_DESC mainDesc{};
	globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN].texture->GetDesc(&mainDesc);
	if (mainDesc.SampleDesc.Count > 1) {
		unavailableReason = "Multisampled render targets are not supported.";
		return;
	}

	// The rate image must cover the bound render target in 16x16 tiles; the G-buffer targets
	// are all main-sized, and dynamic resolution only shrinks the viewport inside them.
	imageTiles[0] = DivideRoundUp(mainDesc.Width, kTileSize);
	imageTiles[1] = DivideRoundUp(mainDesc.Height, kTileSize);

	auto fail = [&](const char* a_what, HRESULT a_hr) {
		unavailableReason = std::format("Could not create {} (0x{:08X}).", a_what, static_cast<uint32_t>(a_hr));
		logger::error("[VRS] {}", unavailableReason);
	};

	D3D11_TEXTURE2D_DESC texDesc{};
	texDesc.Width = imageTiles[0];
	texDesc.Height = imageTiles[1];
	texDesc.MipLevels = 1;
	texDesc.ArraySize = 1;
	texDesc.SampleDesc.Count = 1;
	texDesc.Usage = D3D11_USAGE_DEFAULT;
	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	if (HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, tileStats.put()); FAILED(hr))
		return fail("the tile statistics texture", hr);
	if (HRESULT hr = device->CreateShaderResourceView(tileStats.get(), nullptr, tileStatsSRV.put()); FAILED(hr))
		return fail("the tile statistics SRV", hr);
	if (HRESULT hr = device->CreateUnorderedAccessView(tileStats.get(), nullptr, tileStatsUAV.put()); FAILED(hr))
		return fail("the tile statistics UAV", hr);

	// Start from 1x1 everywhere (index 0), so the first frame is never coarse.
	std::vector<uint8_t> zeros(static_cast<size_t>(imageTiles[0]) * imageTiles[1], 0);
	D3D11_SUBRESOURCE_DATA initial{ zeros.data(), imageTiles[0], 0 };
	texDesc.Format = DXGI_FORMAT_R8_UINT;
	for (uint32_t i = 0; i < 2; i++) {
		if (HRESULT hr = device->CreateTexture2D(&texDesc, &initial, rateImage[i].put()); FAILED(hr))
			return fail("the shading-rate image", hr);
		if (HRESULT hr = device->CreateShaderResourceView(rateImage[i].get(), nullptr, rateImageSRV[i].put()); FAILED(hr))
			return fail("the shading-rate image SRV", hr);
		if (HRESULT hr = device->CreateUnorderedAccessView(rateImage[i].get(), nullptr, rateImageUAV[i].put()); FAILED(hr))
			return fail("the shading-rate image UAV", hr);

		NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC viewDesc{};
		viewDesc.version = NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_VER;
		viewDesc.Format = DXGI_FORMAT_R8_UINT;
		viewDesc.ViewDimension = NV_SRRV_DIMENSION_TEXTURE2D;
		viewDesc.Texture2D.MipSlice = 0;
		ID3D11NvShadingRateResourceView* view = nullptr;
		if (const NvAPI_Status status = nvapi.createView(device, rateImage[i].get(), &viewDesc, &view); status != NVAPI_OK || !view) {
			unavailableReason = std::format("NvAPI_D3D11_CreateShadingRateResourceView failed ({}).", static_cast<int>(status));
			logger::error("[VRS] {}", unavailableReason);
			return;
		}
		rateImageView[i] = view;
	}

	D3D11_BUFFER_DESC countsDesc{};
	countsDesc.ByteWidth = 16 * sizeof(uint32_t);
	countsDesc.Usage = D3D11_USAGE_DEFAULT;
	countsDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
	countsDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;
	if (HRESULT hr = device->CreateBuffer(&countsDesc, nullptr, rateCounts.put()); FAILED(hr))
		return fail("the rate statistics buffer", hr);
	D3D11_UNORDERED_ACCESS_VIEW_DESC countsUAVDesc{};
	countsUAVDesc.Format = DXGI_FORMAT_R32_TYPELESS;
	countsUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
	countsUAVDesc.Buffer.NumElements = 16;
	countsUAVDesc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
	if (HRESULT hr = device->CreateUnorderedAccessView(rateCounts.get(), &countsUAVDesc, rateCountsUAV.put()); FAILED(hr))
		return fail("the rate statistics UAV", hr);
	countsDesc.Usage = D3D11_USAGE_STAGING;
	countsDesc.BindFlags = 0;
	countsDesc.MiscFlags = 0;
	countsDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	for (uint32_t i = 0; i < kReadbackSlots; i++) {
		if (HRESULT hr = device->CreateBuffer(&countsDesc, nullptr, rateCountsStaging[i].put()); FAILED(hr))
			return fail("the rate statistics readback buffer", hr);
	}

	if (!rateCB)
		rateCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<RateCB>());

	hardwareSupported = true;
	unavailableReason.clear();
	logger::info("[VRS] NVIDIA variable rate shading available; rate image {}x{} tiles", imageTiles[0], imageTiles[1]);
}

void VariableRateShading::ClearShaderCache()
{
	analyzeCS = nullptr;
	buildCS = nullptr;
	debugCS = nullptr;
	shaderFailed = false;
}

ID3D11ComputeShader* VariableRateShading::GetShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path)
{
	if (!a_shader && !shaderFailed) {
		a_shader.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(a_path, {}, "cs_5_0")));
		if (!a_shader) {
			shaderFailed = true;
			logger::error("[VRS] Failed to compile {}; variable rate shading disabled", Util::WStringToString(a_path));
		}
	}
	return a_shader.get();
}

VariableRateShading::RateCB VariableRateShading::BuildConstants(bool a_historyValid) const
{
	const float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);

	RateCB cb{};
	cb.RenderSize[0] = std::clamp(static_cast<uint32_t>(std::ceil(renderSize.x)), 1u, imageTiles[0] * kTileSize);
	cb.RenderSize[1] = std::clamp(static_cast<uint32_t>(std::ceil(renderSize.y)), 1u, imageTiles[1] * kTileSize);
	cb.RenderTiles[0] = std::min(DivideRoundUp(cb.RenderSize[0], kTileSize), imageTiles[0]);
	cb.RenderTiles[1] = std::min(DivideRoundUp(cb.RenderSize[1], kTileSize), imageTiles[1]);
	cb.ImageTiles[0] = imageTiles[0];
	cb.ImageTiles[1] = imageTiles[1];
	cb.Mode = std::min(settings.RateMode, static_cast<uint32_t>(Mode::AdaptivePeriphery));
	cb.MaxRateLog2 = settings.CoarsestRate ? 2 : 1;
	cb.Threshold = kThresholdAtLowestQuality * std::pow(kThresholdRange, std::clamp(settings.Quality, 0.0f, 1.0f));
	cb.QuarterFactor = kQuarterFactor;
	cb.MotionPixels = std::max(settings.MotionPixels, 0.0f);
	cb.NormalWeight = settings.ProtectNormals ? kNormalWeight : 0.0f;
	cb.PeripheryRadius = std::clamp(settings.PeripheryRadius, 0.1f, 2.0f);
	cb.EnvLuminance = kEnvLuminance;
	cb.HistoryValid = a_historyValid ? 1u : 0u;
	cb.Hysteresis = kHysteresis;
	return cb;
}

void VariableRateShading::BeginOpaquePass()
{
	opaqueWindow = false;
	if (!IsActive())
		return;

	Util::CpuPassScope cpuTimer("VariableRateShading");
	auto context = globals::d3d::context;

	if (lastBuildFrame != frameIndex) {
		auto shader = GetShader(buildCS, L"Data\\Shaders\\VariableRateShading\\BuildRateImageCS.hlsl");
		if (!shader)
			return;

		RateCB cb = BuildConstants(false);
		// Last frame's statistics are only meaningful if they were measured on the frame right
		// before this one, at the same render resolution.
		cb.HistoryValid = (lastAnalysisFrame != 0 && lastAnalysisFrame + 1 == frameIndex &&
							  lastAnalysisRenderSize[0] == cb.RenderSize[0] && lastAnalysisRenderSize[1] == cb.RenderSize[1]) ?
		                      1u :
		                      0u;
		rateCB->Update(cb);

		const uint32_t previous = currentImage;
		currentImage ^= 1;

		{
			GpuTimerScope gpuTimer;

			UINT zero[4]{};
			context->ClearUnorderedAccessViewUint(rateCountsUAV.get(), zero);

			ID3D11ShaderResourceView* srvs[2]{ tileStatsSRV.get(), rateImageSRV[previous].get() };
			ID3D11UnorderedAccessView* uavs[2]{ rateImageUAV[currentImage].get(), rateCountsUAV.get() };
			ID3D11Buffer* cbs[1]{ rateCB->CB() };
			context->CSSetShaderResources(0, 2, srvs);
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			context->CSSetConstantBuffers(0, 1, cbs);
			context->CSSetShader(shader, nullptr, 0);
			context->Dispatch(DivideRoundUp(imageTiles[0], 8), DivideRoundUp(imageTiles[1], 8), 1);
			UnbindCompute(context, 2, 2);
		}

		context->CopyResource(rateCountsStaging[readbackWrite].get(), rateCounts.get());
		readbackPending[readbackWrite] = true;
		readbackWrite = (readbackWrite + 1) % kReadbackSlots;

		lastBuildFrame = frameIndex;
	}

	const Nvapi& nvapi = GetNvapi();
	if (const NvAPI_Status status = nvapi.setView(context, static_cast<ID3D11NvShadingRateResourceView*>(rateImageView[currentImage])); status != NVAPI_OK) {
		DisableForSession(std::format("NvAPI_D3D11_RSSetShadingRateResourceView failed ({}).", static_cast<int>(status)));
		return;
	}
	imageBound = true;
	opaqueWindow = true;
	// The rate table itself stays off until the first draw that qualifies (see OnDraw).
}

void VariableRateShading::EndOpaquePass()
{
	opaqueWindow = false;
	if (appliedTable != Table::Off)
		ApplyTable(Table::Off);
	if (imageBound) {
		GetNvapi().setView(globals::d3d::context, nullptr);
		imageBound = false;
	}
}

VariableRateShading::Table VariableRateShading::ClassifyDraw() const
{
	const auto* state = globals::state;
	const auto* shader = state->currentShader;
	if (!shader || !globals::deferred->deferredPass)
		return Table::Off;
	if (state->permutationData.ExtraShaderDescriptor & static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections))
		return Table::Off;

	if (diagnostics.FullRateEverywhere)
		return Table::Off;

	using Technique = SIE::ShaderCache::LightingShaderTechniques;
	const uint32_t pixelDescriptor = state->currentPixelDescriptor;
	switch (shader->shaderType.get()) {
	case RE::BSShader::Type::Lighting:
		{
			const auto technique = static_cast<Technique>((pixelDescriptor >> 24) & 0x3F);

			// Landscape (the multi-texture ground of the loaded cells) is never coarse-shaded.
			// Terrain Blending draws it last, alpha-blended, with an alpha computed per pixel
			// from SV_Position.z against the prepass depth read at SV_Position.xy. Under a coarse
			// rate both describe the coarse-pixel centre, not the pixel: the exact-equality test
			// distant terrain relies on fails, the alpha drops to 0 for the whole block and the
			// ground turns see-through, leaving only the fog / aerial-perspective colour of its
			// depth (pale, sky-coloured blocks on distant ground, clipped to whatever stands in
			// front). One alpha per block cannot represent a per-pixel depth comparison.
			// The landscape shader is also the most sensitive to attribute extrapolation (blend
			// weights renormalised, vertex colour divided by its own maximum); Lighting.hlsl
			// reads those centroid-interpolated, which covers the other coarse-shaded draws.
			if (technique == Technique::MTLand || technique == Technique::MTLandLODBlend)
				return diagnostics.CoarseTerrain ? Table::Full : Table::Off;

			// Anything blended writes a per-pixel alpha; one coarse alpha per block would
			// cut or fade whole blocks (blended decals and the like).
			if (IsAlphaBlendedDraw())
				return Table::Off;

			// AdditionalAlphaMask is the 4x4 screen-door dither of a fading object; a coarse
			// pixel would turn it into visible 2x2 blocks, so it counts as alpha-tested too.
			// MultiIndexSparkle can discard (projected sparkle), so it is a cut-out as well.
			constexpr uint32_t cutOut = static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::DoAlphaTest) |
			                            static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::AdditionalAlphaMask);
			if ((pixelDescriptor & cutOut) || technique == Technique::MultiIndexSparkle)
				return settings.IncludeAlphaTested ? Table::AlphaTested : Table::Off;
			return Table::Full;
		}
	case RE::BSShader::Type::Grass:
		if (!settings.IncludeGrass || IsAlphaBlendedDraw())
			return Table::Off;
		return (pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::GrassShaderFlags::AlphaTest)) ? Table::AlphaTested : Table::Full;
	case RE::BSShader::Type::DistantTree:
		if (IsAlphaBlendedDraw())
			return Table::Off;
		if (pixelDescriptor & static_cast<uint32_t>(SIE::ShaderCache::DistantTreeShaderFlags::AlphaTest))
			return settings.IncludeAlphaTested ? Table::AlphaTested : Table::Off;
		return Table::Full;
	default:
		// Sky, water, effects, particles, utility (shadow / depth), image space, ...
		return Table::Off;
	}
}

bool VariableRateShading::IsAlphaBlendedDraw()
{
	// The engine's requested blend mode for this draw; State::Draw runs right after
	// BSGraphics::SetDirtyStates applied it, so it describes the draw about to be issued.
	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(alphaBlendMode, shadowState)
	return alphaBlendMode != 0;
}

void VariableRateShading::UpdateDrawState()
{
	const Table wanted = opaqueWindow ? ClassifyDraw() : Table::Off;
	if (wanted != appliedTable)
		ApplyTable(wanted);
}

void VariableRateShading::ApplyTable(Table a_table)
{
	const Nvapi& nvapi = GetNvapi();
	if (!nvapi.ok) {
		appliedTable = Table::Off;
		return;
	}

	NV_D3D11_VIEWPORT_SHADING_RATE_DESC viewport{};
	viewport.enableVariablePixelShadingRate = a_table != Table::Off;
	const auto& table = a_table == Table::AlphaTested ? kAlphaTestedTable : kFullTable;
	std::copy(std::begin(table), std::end(table), std::begin(viewport.shadingRateTable));

	NV_D3D11_VIEWPORTS_SHADING_RATE_DESC desc{};
	desc.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
	// Zero viewports switches VRS off globally, whatever the engine's viewport count is.
	desc.numViewports = a_table == Table::Off ? 0u : 1u;
	desc.pViewports = &viewport;

	const NvAPI_Status status = nvapi.setRates(globals::d3d::context, &desc);
	appliedTable = a_table;
	if (status != NVAPI_OK && a_table != Table::Off)
		DisableForSession(std::format("NvAPI_D3D11_RSSetViewportsPixelShadingRates failed ({}).", static_cast<int>(status)));
}

void VariableRateShading::DisableForSession(std::string a_reason)
{
	logger::error("[VRS] {} Variable rate shading disabled for this session.", a_reason);
	unavailableReason = std::move(a_reason);
	hardwareSupported = false;

	const Nvapi& nvapi = GetNvapi();
	if (nvapi.ok) {
		NV_D3D11_VIEWPORTS_SHADING_RATE_DESC off{};
		off.version = NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_VER;
		off.numViewports = 0;
		nvapi.setRates(globals::d3d::context, &off);
		nvapi.setView(globals::d3d::context, nullptr);
	}
	appliedTable = Table::Off;
	imageBound = false;
	opaqueWindow = false;
}

void VariableRateShading::AnalyzeFrame()
{
	if (!IsActive() || lastAnalysisFrame == frameIndex)
		return;

	const bool needAnalysis = settings.RateMode != static_cast<uint32_t>(Mode::Periphery);
	const bool needDebug = settings.DebugOverlay && lastBuildFrame == frameIndex;
	if (!needAnalysis && !needDebug)
		return;

	Util::CpuPassScope cpuTimer("VariableRateShading");
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto& targets = renderer->GetRuntimeData().renderTargets;
	const auto& main = targets[globals::deferred->forwardRenderTargets[0]];

	const RateCB cb = BuildConstants(false);
	rateCB->Update(cb);
	ID3D11Buffer* cbs[1]{ rateCB->CB() };

	GpuTimerScope gpuTimer;

	if (needAnalysis) {
		auto shader = GetShader(analyzeCS, L"Data\\Shaders\\VariableRateShading\\AnalyzeCS.hlsl");
		if (!shader)
			return;

		ID3D11ShaderResourceView* srvs[3]{
			main.SRV,
			targets[RE::RENDER_TARGETS::kMOTION_VECTOR].SRV,
			targets[NORMALROUGHNESS].SRV,
		};
		ID3D11UnorderedAccessView* uavs[1]{ tileStatsUAV.get() };
		context->CSSetShaderResources(0, 3, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch(cb.RenderTiles[0], cb.RenderTiles[1], 1);
		UnbindCompute(context, 3, 1);

		lastAnalysisFrame = frameIndex;
		lastAnalysisRenderSize[0] = cb.RenderSize[0];
		lastAnalysisRenderSize[1] = cb.RenderSize[1];
	}

	if (needDebug && main.UAV) {
		auto shader = GetShader(debugCS, L"Data\\Shaders\\VariableRateShading\\DebugOverlayCS.hlsl");
		if (!shader)
			return;

		ID3D11ShaderResourceView* srvs[1]{ rateImageSRV[currentImage].get() };
		ID3D11UnorderedAccessView* uavs[1]{ main.UAV };
		context->CSSetShaderResources(0, 1, srvs);
		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
		context->CSSetConstantBuffers(0, 1, cbs);
		context->CSSetShader(shader, nullptr, 0);
		context->Dispatch(DivideRoundUp(cb.RenderSize[0], 8), DivideRoundUp(cb.RenderSize[1], 8), 1);
		UnbindCompute(context, 1, 1);
	}
}

void VariableRateShading::ReadBackRateCounts()
{
	if (!rateCounts)
		return;
	auto context = globals::d3d::context;
	// Oldest slot first; each map is non-blocking, an unfinished copy is simply tried again later.
	for (uint32_t n = 0; n < kReadbackSlots; n++) {
		const uint32_t slot = (readbackWrite + n) % kReadbackSlots;
		if (!readbackPending[slot])
			continue;
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (context->Map(rateCountsStaging[slot].get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
			continue;
		std::memcpy(lastRateCounts, mapped.pData, sizeof(lastRateCounts));
		context->Unmap(rateCountsStaging[slot].get(), 0);
		readbackPending[slot] = false;
		hasRateCounts = true;
	}
}

void VariableRateShading::Reset()
{
	// Present: nothing of the engine frame is left to draw, and the UI is about to be. The
	// NVAPI rate state survives even ID3D11DeviceContext::ClearState, so never leave it on.
	if (opaqueWindow || imageBound || appliedTable != Table::Off)
		EndOpaquePass();
	ReadBackRateCounts();
	frameIndex++;
}

void VariableRateShading::DrawSettings()
{
	const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;

	if (!hardwareSupported) {
		ImGui::TextColored(palette.Error, "Unavailable: %s", unavailableReason.c_str());
		ImGui::Spacing();
	} else if (shaderFailed) {
		ImGui::TextColored(palette.Error, "Unavailable: a VRS shader failed to compile (see CommunityShaders.log).");
		ImGui::Spacing();
	}

	ImGui::Checkbox("Enable", &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Lowers shading detail on flat, low-detail parts of the scene to save GPU time. Off by default because it can make parts of the image softer; needs an NVIDIA RTX 20-series or newer card.");
	}

	if (settings.Enabled && !Batch36::IsOn())
		ImGui::TextColored(palette.Warning, "Forced off: the Batch 36 master switch is off (Advanced > Batch 36).");

	ImGui::BeginDisabled(!settings.Enabled);

	ImGui::SeparatorText("Where to save");

	const char* modes[] = { "Adaptive (by content)", "Screen edges (fixed)", "Adaptive + screen edges" };
	int mode = static_cast<int>(std::min(settings.RateMode, 2u));
	if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes)))
		settings.RateMode = static_cast<uint32_t>(mode);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Adaptive lowers detail only where last frame looked flat; Screen edges always lowers it towards the borders; the third does both.");
	}

	if (settings.RateMode != static_cast<uint32_t>(Mode::Periphery)) {
		Util::PercentageSlider("Quality", &settings.Quality, 0.0f, 100.0f, "%.0f %%");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Higher keeps more of the screen at full detail (safer); lower saves more but can soften fine texture.");
		}

		ImGui::SliderFloat("Motion Boost", &settings.MotionPixels, 0.0f, 32.0f, settings.MotionPixels > 0.0f ? "%.0f px/frame" : "Off");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Lets fast-moving parts of the screen drop more detail, since motion hides it. Lower values = stronger effect, more savings; 0 turns it off.");
		}

		ImGui::Checkbox("Protect Normal Detail", &settings.ProtectNormals);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Keeps bumpy surfaces at full detail so screen-space lighting (SSGI, SSRT) still sees their fine surface shape. Turning it off saves a bit more.");
		}
	}

	if (settings.RateMode != static_cast<uint32_t>(Mode::Adaptive)) {
		ImGui::SliderFloat("Full-Detail Centre Size", &settings.PeripheryRadius, 0.1f, 2.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Size of the circle in the middle of the screen that always stays at full detail. 1.0 reaches the top and bottom edges.");
		}
	}

	const char* rates[] = { "2x2", "4x4" };
	int coarsest = static_cast<int>(std::min(settings.CoarsestRate, 1u));
	if (ImGui::Combo("Coarsest Rate", &coarsest, rates, IM_ARRAYSIZE(rates)))
		settings.CoarsestRate = static_cast<uint32_t>(coarsest);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("The largest block of pixels that may share one shading result. 4x4 saves more but can look blocky.");
	}

	ImGui::SeparatorText("What to include");

	ImGui::Checkbox("Include Grass", &settings.IncludeGrass);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Also lowers detail on grass, which saves the most in grassy areas but can make blade edges look blocky (capped at 2x2).");
	}

	ImGui::Checkbox("Include Other Cut-Out Objects", &settings.IncludeAlphaTested);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Also lowers detail on leaves, hair, fences and distant trees, whose cut-out edges can look blocky (capped at 2x2).");
	}

	ImGui::TextDisabled("Never affected: the ground, blended decals, shadows, sky, water, transparent effects, UI, post-processing.");

	ImGui::SeparatorText("Debug");

	ImGui::Checkbox("Show Shading Rate Overlay", &settings.DebugOverlay);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Tints each 16x16 block by how coarsely it is shaded, so you can see where detail is being saved.");
	}
	if (settings.DebugOverlay) {
		const int legend[] = { 3, 1, 4, 7, 5, 8 };
		for (int i = 0; i < IM_ARRAYSIZE(legend); i++) {
			if (i > 0)
				ImGui::SameLine();
			ImGui::TextColored(kRateColours[legend[i]], "%s", kRateNames[legend[i]]);
		}
	}

	if (IsActive() && hasRateCounts) {
		uint32_t total = 0;
		float work = 0.0f;
		for (uint32_t i = 0; i < kRateIndices; i++) {
			total += lastRateCounts[i];
			work += static_cast<float>(lastRateCounts[i]) / static_cast<float>(kRateArea[i]);
		}
		if (total > 0) {
			ImGui::Text("Tiles this frame:");
			for (uint32_t i = 0; i < kRateIndices; i++) {
				if (lastRateCounts[i] == 0)
					continue;
				ImGui::SameLine();
				ImGui::TextColored(kRateColours[i], "%s %.0f%%", kRateNames[i], 100.0f * lastRateCounts[i] / total);
			}
			ImGui::Text("Opaque pixel-shader work: about %.0f%% of full rate", 100.0f * work / total);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Counts screen tiles, not objects. Anything excluded (the ground, and grass or cut-outs when off) still runs at full detail, so the real saving is smaller.");
			}
		}
	}

	ImGui::SeparatorText("Diagnostics (not saved)");

	ImGui::Checkbox("Diagnostic: Coarse Ground (old)", &diagnostics.CoarseTerrain);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Lets the ground drop detail again, like the previous build. Only for checking whether the pale blocks on distant ground come back; leave off for normal play.");
	}

	ImGui::Checkbox("Diagnostic: Full Detail Everywhere", &diagnostics.FullRateEverywhere);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Keeps all of VRS running but shades everything at full detail. If a problem stays with this on, VRS lowering detail is not what causes it.");
	}

	ImGui::EndDisabled();
}
