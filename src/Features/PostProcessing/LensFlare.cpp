#include "LensFlare.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LensFlare::Settings,
	LensFlareCurve,
	GhostStrength,
	HaloStrength,
	HaloRadius,
	HaloWidth,
	LensFlareCA,
	LFStrength,
	GLocalMask)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LensFlare::debugSettings,
	downsampleTimes,
	upsampleTimes,
	disableDownsample,
	disableUpsample)

void LensFlare::DrawSettings()
{
	ImGui::SliderFloat("Lens Flare Curve", &settings.LensFlareCurve, 0.0f, 2.0f, "%.3f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("What parts of the image produce lens flares");
	}

	ImGui::SliderFloat("Lens Flare Strength", &settings.LFStrength, 0.0f, 1.0f, "%.3f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Master intensity control for the entire lens flare effect");
	}

	ImGui::Checkbox("Non-intrusive Lens Flares", &settings.GLocalMask);
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Only apply flaring when looking directly at light sources");
	}

	// Ghost Settings
	ImGui::Spacing();
	ImGui::Text("Ghost Settings");
	ImGui::Separator();

	ImGui::SliderFloat("Ghost Strength", &settings.GhostStrength, 0.0f, 1.0f, "%.3f");
	if (ImGui::IsItemHovered()) {
		ImGui::SetTooltip("Intensity of ghost artifacts");
	}

	// Halo Settings
	ImGui::Spacing();
	ImGui::Text("Halo Settings");
	ImGui::Separator();

	ImGui::SliderFloat("Halo Strength", &settings.HaloStrength, 0.0f, 1.0f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of the ring-shaped halo flare around bright lights.");
	ImGui::SliderFloat("Halo Radius", &settings.HaloRadius, 0.0f, 0.8f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Size of the halo ring.");
	ImGui::SliderFloat("Halo Width", &settings.HaloWidth, 0.0f, 1.0f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Thickness of the halo ring.");

	// Chromatic Aberration
	ImGui::Spacing();
	ImGui::Text("Chromatic Aberration");
	ImGui::Separator();

	ImGui::SliderFloat("CA Amount", &settings.LensFlareCA, 0.0f, 2.0f, "%.3f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Colour fringing (chromatic aberration) on the flares. Higher = stronger rainbow edges.");

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	if (ImGui::CollapsingHeader("Debug")) {
		ImGui::Checkbox("Disable Downsample", &debugsettings.disableDownsample);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Debug only.");
		ImGui::Checkbox("Disable Upsample", &debugsettings.disableUpsample);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Debug only.");
		ImGui::SliderInt("Downsample Times", &debugsettings.downsampleTimes, 1, 8);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Debug only.");
		ImGui::SliderInt("Upsample Times", &debugsettings.upsampleTimes, 1, 8);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Debug only.");
	}
}

void LensFlare::RestoreDefaultSettings()
{
	settings = {};
}

void LensFlare::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LensFlare::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LensFlare::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
		lensFlareCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<LensFlareCB>());
	}

	logger::debug("Creating 2D textures...");
	{
		auto gameTexMainCopy = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN_COPY];

		D3D11_TEXTURE2D_DESC texDesc;
		gameTexMainCopy.texture->GetDesc(&texDesc);
		texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		logger::debug("Creating output texture with format: {}", (uint32_t)texDesc.Format);

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

		texDesc.MipLevels = srvDesc.Texture2D.MipLevels = 1;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		texDesc.MiscFlags = 0;

		texOutput = eastl::make_unique<Texture2D>(texDesc);
		texOutput->CreateSRV(srvDesc);
		texOutput->CreateUAV(uavDesc);

		texFlare = eastl::make_unique<Texture2D>(texDesc);
		texFlare->CreateSRV(srvDesc);
		texFlare->CreateUAV(uavDesc);

		texFlareD = eastl::make_unique<Texture2D>(texDesc);
		texFlareD->CreateSRV(srvDesc);
		texFlareD->CreateUAV(uavDesc);

		texFlareDCopy = eastl::make_unique<Texture2D>(texDesc);
		texFlareDCopy->CreateSRV(srvDesc);
		texFlareDCopy->CreateUAV(uavDesc);

		texFlareU = eastl::make_unique<Texture2D>(texDesc);
		texFlareU->CreateSRV(srvDesc);
		texFlareU->CreateUAV(uavDesc);

		texFlareUCopy = eastl::make_unique<Texture2D>(texDesc);
		texFlareUCopy->CreateSRV(srvDesc);
		texFlareUCopy->CreateUAV(uavDesc);
	}

	logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_BORDER,
			.AddressV = D3D11_TEXTURE_ADDRESS_BORDER,
			.AddressW = D3D11_TEXTURE_ADDRESS_BORDER,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};

		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, colorSampler.put()));

		D3D11_SAMPLER_DESC resizesamplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_MIRROR,
			.AddressV = D3D11_TEXTURE_ADDRESS_MIRROR,
			.AddressW = D3D11_TEXTURE_ADDRESS_MIRROR,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};

		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, resizeSampler.put()));
	}

}

// (batch 16, item P14) Shader compilation is no longer part of SetupResources, so that
// the memory SetupResources allocates can be handed back while the effect is off
// without paying a D3DCompileFromFile on every re-enable.
void LensFlare::SetupShaders()
{
	CompileComputeShaders();
}

// (batch 16, item P14) Mirror of SetupResources: every texture/buffer it assigns is
// nulled here. Shaders and sampler states are deliberately kept -- they are a few
// hundred bytes and keeping them is what makes turning the effect back on cheap.
void LensFlare::ReleaseResources()
{
	lensFlareCB = nullptr;
	texOutput = nullptr;
	texFlare = nullptr;
	texFlareD = nullptr;
	texFlareDCopy = nullptr;
	texFlareU = nullptr;
	texFlareUCopy = nullptr;
}

void LensFlare::ClearShaderCache()
{
	const auto shaderPtrs = std::array{
		&lensFlareCS
	};

	for (auto shader : shaderPtrs)
		if ((*shader)) {
			(*shader)->Release();
			shader->detach();
		}

	CompileComputeShaders();
}

void LensFlare::CompileComputeShaders()
{
	struct ShaderCompileInfo
	{
		winrt::com_ptr<ID3D11ComputeShader>* programPtr;
		std::string_view filename;
		std::vector<std::pair<const char*, const char*>> defines = {};
		std::string entry = "main";
	};

	std::vector<ShaderCompileInfo>
		shaderInfos = {
			{ &lensFlareCS, "lensflare.cs.hlsl", {}, "CSLensflare" },
			{ &downsampleCS, "lensflare.cs.hlsl", {}, "CSFlareDown" },
			{ &upsampleCS, "lensflare.cs.hlsl", {}, "CSFlareUp" },
			{ &compositeCS, "lensflare.cs.hlsl", {}, "CSComposite" }
		};

	for (auto& info : shaderInfos) {
		auto path = std::filesystem::path("Data\\Shaders\\PostProcessing\\LensFlare") / info.filename;
		if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0", info.entry.c_str())))
			info.programPtr->attach(rawPtr);
	}

	if (!lensFlareCS) {
		logger::error("Failed to compile lens flare compute shader!");
		return;
	}
}

void LensFlare::Draw(TextureInfo& inout_tex)
{
	auto state = globals::state;
	auto context = globals::d3d::context;

	state->BeginPerfEvent("Lens Flare");

	uint width = texOutput->desc.Width;
	uint height = texOutput->desc.Height;

	LensFlareCB data = {
		.settings = settings,
		.ScreenWidth = (float)width,
		.ScreenHeight = (float)height,
		.downsizeScale = 1
	};

	int downsampleTimes = debugsettings.downsampleTimes;
	int upsampleTimes = debugsettings.upsampleTimes;

	lensFlareCB->Update(data);

	std::array<ID3D11ShaderResourceView*, 2> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };
	std::array<ID3D11SamplerState*, 2> samplers = { colorSampler.get(), resizeSampler.get() };
	auto cb = lensFlareCB->CB();

	auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		cb = nullptr;

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetConstantBuffers(1, 1, &cb);
	};

	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());

	// Get Lens Flare
	srvs.at(0) = inout_tex.srv;
	uavs.at(0) = texFlare->uav.get();

	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	context->CSSetShader(lensFlareCS.get(), nullptr, 0);
	uint dispatchX = (width + 7) >> 3;
	uint dispatchY = (height + 7) >> 3;
	context->Dispatch(dispatchX, dispatchY, 1);
	resetViews();

	// int numDownsamples = 1;
	// if (height > 1024) numDownsamples = 2;
	// if (height > 2048) numDownsamples = 3;
	// if (height > 4096) numDownsamples = 4;

	context->CopyResource(texFlareD->resource.get(), texFlare->resource.get());
	context->CopyResource(texFlareDCopy->resource.get(), texFlare->resource.get());

	if (!debugsettings.disableDownsample) {
		// Downsample passes
		context->CSSetShader(downsampleCS.get(), nullptr, 0);
		for (int i = 0; i < downsampleTimes; i++) {
			// When i == 0, downsizeScale is 8.
			// When i == 3, downsizeScale is 64.
			data.downsizeScale = 2 ^ (i + 3);
			lensFlareCB->Update(data);
			cb = lensFlareCB->CB();
			context->CSSetConstantBuffers(1, 1, &cb);
			srvs.at(1) = texFlareD->srv.get();
			uavs.at(0) = texFlareDCopy->uav.get();
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->Dispatch(dispatchX, dispatchY, 1);
			context->CopyResource(texFlareD->resource.get(), texFlareDCopy->resource.get());
			resetViews();
		}
		context->CopyResource(texFlare->resource.get(), texFlareD->resource.get());
		// No Flush here: D3D11 already orders the copy against everything that follows on this
		// context, so flushing only drained the command queue mid-frame for no correctness gain.
	}

	context->CopyResource(texFlareU->resource.get(), texFlareD->resource.get());
	context->CopyResource(texFlareUCopy->resource.get(), texFlareD->resource.get());

	if (!debugsettings.disableUpsample) {
		// Upsample passes
		context->CSSetShader(upsampleCS.get(), nullptr, 0);
		for (int i = 0; i < upsampleTimes; i++) {
			// When i == 3, downsizeScale is 8.
			// When i == 0, downsizeScale is 64.
			data.downsizeScale = 2 ^ (6 - i);
			lensFlareCB->Update(data);
			cb = lensFlareCB->CB();
			context->CSSetConstantBuffers(1, 1, &cb);
			srvs.at(1) = texFlareU->srv.get();
			uavs.at(0) = texFlareUCopy->uav.get();
			context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
			context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
			context->Dispatch(dispatchX, dispatchY, 1);
			context->CopyResource(texFlareU->resource.get(), texFlareUCopy->resource.get());
			resetViews();
		}
		context->CopyResource(texFlare->resource.get(), texFlareU->resource.get());
		// See above: the mid-frame Flush bought nothing but a command-queue drain.
	}

	// Final composite
	cb = lensFlareCB->CB();
	srvs.at(0) = inout_tex.srv;
	srvs.at(1) = texFlare->srv.get();
	uavs.at(0) = texOutput->uav.get();
	context->CSSetConstantBuffers(1, 1, &cb);
	context->CSSetShader(compositeCS.get(), nullptr, 0);
	context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
	context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	context->Dispatch(dispatchX, dispatchY, 1);

	// Cleanup
	resetViews();
	cb = nullptr;
	samplers.fill(nullptr);

	context->CSSetConstantBuffers(0, 1, &cb);
	context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
	context->CSSetShader(nullptr, nullptr, 0);

	inout_tex = { texOutput->resource.get(), texOutput->srv.get() };
	state->EndPerfEvent();
}
