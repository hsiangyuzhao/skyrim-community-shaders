#include "LocalExposure.h"

#include "Features/PostProcessing.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"
#include "Utils/UI.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LocalExposure::Settings,
	Exposure,
	Strength,
	HighlightContrast,
	ShadowContrast,
	DetailStrength,
	BaseBlend,
	BlurredLuminanceKernelSize,
	MiddleGreyBias,
	HighlightThreshold,
	ShadowThreshold,
	HighlightThresholdStrength,
	ShadowThresholdStrength)

namespace
{
	/// The auto exposure effect, when it is switched on (it then runs before us in the same chain).
	HistogramAutoExposure* GetActiveAutoExposure()
	{
		auto& pipe = globals::features::postProcessing.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::AutoExposure)];
		if (!pipe || !pipe->enabled || !pipe->RuntimeGateOpen())
			return nullptr;
		return static_cast<HistogramAutoExposure*>(pipe.get());
	}
}

void LocalExposure::DrawSettings()
{
	if (!GetActiveAutoExposure()) {
		ImGui::SliderFloat("Exposure", &settings.Exposure, 0.f, 4.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Only used while Histogram Auto Exposure is off: how bright the image is assumed to be when deciding what\n"
				"counts as a highlight or a shadow. Higher = the scene is treated as brighter. Does not brighten the image itself.");
	}

	ImGui::SliderFloat("Strength", &settings.Strength, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How much of the local adjustment is applied. 0 = none, 1 = full (upstream default).");

	ImGui::SliderFloat("Highlight Contrast", &settings.HighlightContrast, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Contrast of large bright areas (windows, sky through a door). Lower = bright areas are pulled down more.");

	ImGui::SliderFloat("Shadow Contrast", &settings.ShadowContrast, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Contrast of large dark areas. Lower = dark areas are lifted more.");

	ImGui::SliderFloat("Detail Strength", &settings.DetailStrength, 0.f, 2.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Fine detail on top of the large areas. 1.0 keeps the original detail contrast; higher sharpens it.");

	ImGui::SliderFloat("Soft Base Blend", &settings.BaseBlend, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Mixes the edge-aware analysis with a plain wide blur. Higher = fewer halos around bright objects, gentler effect.");

	ImGui::SliderFloat("Blurred Luminance Kernel Size", &settings.BlurredLuminanceKernelSize, 0.f, 100.f, "%.1f%%", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Size of the wide blur, as a percentage of the screen width.");

	if (ImGui::TreeNodeEx("Advanced", ImGuiTreeNodeFlags_None)) {
		ImGui::SliderFloat("Middle Grey Bias", &settings.MiddleGreyBias, -4.f, 4.f, "%+.2f EV");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Moves the brightness that splits highlights from shadows.");

		ImGui::SliderFloat("Highlight Threshold", &settings.HighlightThreshold, 0.f, 4.f, "%.2f EV");
		ImGui::SliderFloat("Highlight Threshold Strength", &settings.HighlightThresholdStrength, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Areas less than this many stops above middle grey are left alone.");

		ImGui::SliderFloat("Shadow Threshold", &settings.ShadowThreshold, 0.f, 4.f, "%.2f EV");
		ImGui::SliderFloat("Shadow Threshold Strength", &settings.ShadowThresholdStrength, 0.f, 1.f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Areas less than this many stops below middle grey are left alone.");

		ImGui::TreePop();
	}

	if (ImGui::TreeNode("Debug")) {
		ImGui::Text("Last frame: %s", lastDrawStatus);
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);
		BUFFER_VIEWER_NODE(texLogLuminance, debugRescale)
		BUFFER_VIEWER_NODE(texBlurredLuminance, 8.f * debugRescale)
		ImGui::TreePop();
	}
}

void LocalExposure::RestoreDefaultSettings()
{
	settings = {};
}

void LocalExposure::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LocalExposure::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LocalExposure::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];
	D3D11_TEXTURE2D_DESC mainDesc;
	gameTexMainCopy.texture->GetDesc(&mainDesc);

	const uint fullW = mainDesc.Width;
	const uint fullH = mainDesc.Height;

	numMips = 1;
	{
		uint w = fullW, h = fullH;
		while (w > 1 && h > 1 && numMips < s_MaxMips) {
			w = (w + 1) / 2;
			h = (h + 1) / 2;
			numMips++;
		}
	}

	// Log-luminance pyramid (mip 0 = full resolution; mips 1..s_BlurMip feed the wide blur).
	{
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = fullW;
		texDesc.Height = fullH;
		texDesc.MipLevels = numMips;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		texLogLuminance = eastl::make_unique<Texture2D>(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC fullSrvDesc = {};
		fullSrvDesc.Format = texDesc.Format;
		fullSrvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		fullSrvDesc.Texture2D.MostDetailedMip = 0;
		fullSrvDesc.Texture2D.MipLevels = numMips;
		texLogLuminance->CreateSRV(fullSrvDesc);

		for (uint i = 0; i < numMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
			srvDesc.Format = texDesc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = i;
			srvDesc.Texture2D.MipLevels = 1;
			DX::ThrowIfFailed(device->CreateShaderResourceView(texLogLuminance->resource.get(), &srvDesc, logLuminanceMipSRVs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = i;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texLogLuminance->resource.get(), &uavDesc, logLuminanceMipUAVs[i].put()));
		}
	}

	// Edge-aware luminance grid: one 64x64-pixel tile per cell, 32 log-luminance bins.
	{
		D3D11_TEXTURE3D_DESC texDesc = {};
		texDesc.Width = (fullW + s_GridTileSize - 1) / s_GridTileSize;
		texDesc.Height = (fullH + s_GridTileSize - 1) / s_GridTileSize;
		texDesc.Depth = s_GridDepth;
		texDesc.MipLevels = 1;
		texDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		texLuminanceGrid = eastl::make_unique<Texture3D>(texDesc);

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
		srvDesc.Texture3D.MostDetailedMip = 0;
		srvDesc.Texture3D.MipLevels = 1;
		texLuminanceGrid->CreateSRV(srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
		uavDesc.Texture3D.MipSlice = 0;
		uavDesc.Texture3D.FirstWSlice = 0;
		uavDesc.Texture3D.WSize = texDesc.Depth;
		texLuminanceGrid->CreateUAV(uavDesc);
	}

	// Low-resolution textures for the wide blur (mip s_BlurMip of the pyramid: 120x68 at 4K).
	{
		const uint blurMip = std::min(s_BlurMip, numMips - 1);
		D3D11_TEXTURE2D_DESC texDesc = {};
		texDesc.Width = std::max(1u, fullW >> blurMip);
		texDesc.Height = std::max(1u, fullH >> blurMip);
		texDesc.MipLevels = 1;
		texDesc.ArraySize = 1;
		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		texDesc.SampleDesc.Count = 1;
		texDesc.Usage = D3D11_USAGE_DEFAULT;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		uavDesc.Format = texDesc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		uavDesc.Texture2D.MipSlice = 0;

		texBlurTemp = eastl::make_unique<Texture2D>(texDesc);
		texBlurTemp->CreateSRV(srvDesc);
		texBlurTemp->CreateUAV(uavDesc);

		texBlurredLuminance = eastl::make_unique<Texture2D>(texDesc);
		texBlurredLuminance->CreateSRV(srvDesc);
		texBlurredLuminance->CreateUAV(uavDesc);
	}

	// Output colour, same format and size as the game's buffer (as Histogram Auto Exposure's).
	{
		D3D11_TEXTURE2D_DESC texDesc = mainDesc;
		texDesc.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MostDetailedMip = 0, .MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		texOutput = eastl::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);
	}

	localExposureCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<LocalExposureCB>());

	if (!linearSampler) {
		D3D11_SAMPLER_DESC sampDesc = {};
		sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		sampDesc.MaxLOD = D3D11_FLOAT32_MAX;
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, linearSampler.put()));

		sampDesc.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR;
		sampDesc.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR;
		DX::ThrowIfFailed(device->CreateSamplerState(&sampDesc, mirrorSampler.put()));
	}
}

void LocalExposure::SetupShaders()
{
	CompileComputeShaders();
}

void LocalExposure::ReleaseResources()
{
	localExposureCB = nullptr;
	texLogLuminance = nullptr;
	texLuminanceGrid = nullptr;
	texBlurTemp = nullptr;
	texBlurredLuminance = nullptr;
	texOutput = nullptr;
	for (auto& srv : logLuminanceMipSRVs)
		srv = nullptr;
	for (auto& uav : logLuminanceMipUAVs)
		uav = nullptr;
	numMips = 0;
}

void LocalExposure::ClearShaderCache()
{
	for (auto shader : { &setupCS, &downsampleCS, &blurHorizontalCS, &blurVerticalCS, &gridCS, &applyCS })
		*shader = nullptr;

	CompileComputeShaders();
}

void LocalExposure::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		const char* entry;
	};

	const ShaderCompileInfo shaderInfos[] = {
		{ &setupCS, "CSSetupLogLuminance" },
		{ &downsampleCS, "CSDownsampleLogLuminance" },
		{ &blurHorizontalCS, "CSBlurHorizontal" },
		{ &blurVerticalCS, "CSBlurVertical" },
		{ &gridCS, "CSBuildLuminanceGrid" },
		{ &applyCS, "CSApplyLocalExposure" },
	};

	const auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\LocalExposure\\localexposure.cs.hlsl");
	for (const auto& info : shaderInfos) {
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), {}, "cs_5_0", info.entry)))
			info.programPtr->attach(rawPtr);
	}
}

void LocalExposure::Draw(TextureInfo& inout_tex)
{
	lastDrawRan = false;

	if (!setupCS || !downsampleCS || !blurHorizontalCS || !blurVerticalCS || !gridCS || !applyCS) {
		lastDrawStatus = "Shaders failed to compile";
		return;
	}
	if (!texOutput || !texLogLuminance || numMips == 0) {
		lastDrawStatus = "Resources not allocated";
		return;
	}

	auto context = globals::d3d::context;
	auto state = globals::state;

	// Own row in the performance overlay: step out of the Post Processing row for our passes
	// (D3D11 timestamp intervals cannot nest) and back in afterwards.
	auto* timers = Util::GpuPassTimers::GetSingleton();
	timers->End(Util::GpuBucket::PostProcessing);
	timers->Begin(Util::GpuBucket::LocalExposure);

	state->BeginPerfEvent("Local Exposure");

	D3D11_TEXTURE2D_DESC inDesc;
	inout_tex.tex->GetDesc(&inDesc);
	const uint fullW = std::min(inDesc.Width, texOutput->desc.Width);
	const uint fullH = std::min(inDesc.Height, texOutput->desc.Height);
	const uint blurMip = std::min(s_BlurMip, numMips - 1);
	const uint blurWidth = texBlurredLuminance->desc.Width;
	const uint blurHeight = texBlurredLuminance->desc.Height;
	const float blurRadius = std::min(
		blurWidth * std::clamp(settings.BlurredLuminanceKernelSize, 0.f, 100.f) * 0.005f,
		(float)s_MaxBlurRadius);

	// After Histogram Auto Exposure the image is already exposed: global exposure 1, middle grey
	// 0.18 x its compensation. Without it, Exposure is the assumed exposure (never applied).
	const auto* autoExposure = GetActiveAutoExposure();

	LocalExposureCB cbData = {
		.GlobalExposure = autoExposure ? 1.f : std::max(settings.Exposure, 1e-3f),
		.Strength = std::clamp(settings.Strength, 0.f, 1.f),
		.HighlightContrast = std::clamp(settings.HighlightContrast, 0.f, 1.f),
		.ShadowContrast = std::clamp(settings.ShadowContrast, 0.f, 1.f),
		.DetailStrength = std::clamp(settings.DetailStrength, 0.f, 2.f),
		.BaseBlend = std::clamp(settings.BaseBlend, 0.f, 1.f),
		.BlurRadius = blurRadius,
		.MiddleGreyBias = settings.MiddleGreyBias,
		.HighlightThreshold = std::max(settings.HighlightThreshold, 0.f),
		.ShadowThreshold = std::max(settings.ShadowThreshold, 0.f),
		.HighlightThresholdStrength = std::clamp(settings.HighlightThresholdStrength, 0.f, 1.f),
		.ShadowThresholdStrength = std::clamp(settings.ShadowThresholdStrength, 0.f, 1.f),
		.InputWidth = fullW,
		.InputHeight = fullH,
		.BlurredWidth = blurWidth,
		.BlurredHeight = blurHeight,
		.LogLuminanceMin = -13.f,
		.LogLuminanceMax = 18.f,
		.MiddleGreyCompensation = autoExposure ? std::max(autoExposure->lastExposureCompensation, 1e-3f) : 1.f,
		.pad = 0.f,
	};
	localExposureCB->Update(cbData);

	ID3D11Buffer* cb = localExposureCB->CB();
	context->CSSetConstantBuffers(1, 1, &cb);

	std::array<ID3D11SamplerState*, 2> samplers = { linearSampler.get(), mirrorSampler.get() };
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	std::array<ID3D11ShaderResourceView*, 4> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { nullptr };

	auto bind = [&]() {
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};
	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		bind();
	};
	auto mipDim = [](uint dim, uint mip) {
		return std::max(1u, (dim + ((1u << mip) - 1u)) >> mip);
	};

	// Pass 1: scene log luminance (full resolution).
	{
		srvs[0] = inout_tex.srv;
		uavs[0] = logLuminanceMipUAVs[0].get();
		bind();
		context->CSSetShader(setupCS.get(), nullptr, 0);
		context->Dispatch((fullW + 7) >> 3, (fullH + 7) >> 3, 1);
		resetViews();
	}

	// Pass 2: mip chain down to the wide-blur level.
	for (uint i = 1; i <= blurMip; i++) {
		srvs[1] = logLuminanceMipSRVs[i - 1].get();
		uavs[0] = logLuminanceMipUAVs[i].get();
		bind();
		context->CSSetShader(downsampleCS.get(), nullptr, 0);
		context->Dispatch((mipDim(texLogLuminance->desc.Width, i) + 7) >> 3, (mipDim(texLogLuminance->desc.Height, i) + 7) >> 3, 1);
		resetViews();
	}

	// Pass 3: separable wide blur at low resolution.
	{
		srvs[1] = logLuminanceMipSRVs[blurMip].get();
		uavs[0] = texBlurTemp->uav.get();
		bind();
		context->CSSetShader(blurHorizontalCS.get(), nullptr, 0);
		context->Dispatch((blurWidth + 7) >> 3, (blurHeight + 7) >> 3, 1);
		resetViews();

		srvs[1] = texBlurTemp->srv.get();
		uavs[0] = texBlurredLuminance->uav.get();
		bind();
		context->CSSetShader(blurVerticalCS.get(), nullptr, 0);
		context->Dispatch((blurWidth + 7) >> 3, (blurHeight + 7) >> 3, 1);
		resetViews();
	}

	// Pass 4: edge-aware luminance grid (one group per 64x64 tile).
	{
		srvs[1] = logLuminanceMipSRVs[0].get();
		uavs[1] = texLuminanceGrid->uav.get();
		bind();
		context->CSSetShader(gridCS.get(), nullptr, 0);
		context->Dispatch(texLuminanceGrid->desc.Width, texLuminanceGrid->desc.Height, 1);
		resetViews();
	}

	// Pass 5: base layer + local adjustment, written as the new image (upstream's resolve pass and
	// the local-exposure part of its Composite, fused).
	{
		srvs[0] = inout_tex.srv;
		srvs[1] = logLuminanceMipSRVs[0].get();
		srvs[2] = texLuminanceGrid->srv.get();
		srvs[3] = texBlurredLuminance->srv.get();
		uavs[0] = texOutput->uav.get();
		bind();
		context->CSSetShader(applyCS.get(), nullptr, 0);
		context->Dispatch((fullW + 7) >> 3, (fullH + 7) >> 3, 1);
		resetViews();
	}

	context->CSSetShader(nullptr, nullptr, 0);
	cb = nullptr;
	context->CSSetConstantBuffers(1, 1, &cb);
	samplers.fill(nullptr);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };

	state->EndPerfEvent();

	timers->End(Util::GpuBucket::LocalExposure);
	timers->Begin(Util::GpuBucket::PostProcessing);

	lastDrawRan = true;
	lastDrawStatus = autoExposure ? "On (after Histogram Auto Exposure)" : "On (Auto Exposure off: assumed exposure from the slider)";
}
