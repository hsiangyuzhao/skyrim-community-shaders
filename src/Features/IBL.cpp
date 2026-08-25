#include "IBL.h"

#include "Deferred.h"
#include "DynamicCubemaps.h"
#include "JiayeStatement.h"
#include "Shadercache.h"
#include "State.h"

#include <DDSTextureLoader.h>
#include <DirectXTex.h>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	IBL::Settings,
	EnableDiffuseIBL,
	PreserveFogLuminance,
	UseStaticIBL,
	EnableInterior,
	DiffuseIBLScale,
	DALCAmount,
	IBLSaturation,
	FogAmount,
	EffectNormalization,
	EffectNormalizationMult,
	MinEffectMult,
	EnvIBLScale,
	SkyIBLScale,
	EnvIBLSaturation,
	SkyIBLSaturation)

void IBL::DrawSettings()
{
	ImGui::Checkbox("Enable Diffuse IBL", (bool*)&settings.EnableDiffuseIBL);
	ImGui::SliderFloat("Diffuse IBL Scale", &settings.DiffuseIBLScale, 0.0f, 10.0f, "%.2f");
	ImGui::SliderFloat("Diffuse IBL Saturation", &settings.IBLSaturation, 0.0f, 2.0f, "%.2f");

	// (B7) The two halves the ambient probe is split into, trimmable independently. The pair
	// above stays the master gate on the combined result; these four sit underneath it.
	ImGui::SliderFloat("Env IBL Scale", &settings.EnvIBLScale, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Intensity of the environment half of the ambient probe - the bounce light from the "
			"surrounding geometry, with the sky excluded.\n\n"
			"This half is not attenuated by skylighting, so it is what fills shadowed pockets that "
			"face the camera: gateway arches, undersides of eaves, alley corners. Turn it down if "
			"those read as too bright or too strongly tinted by nearby walls.");
	}
	ImGui::SliderFloat("Sky IBL Scale", &settings.SkyIBLScale, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Intensity of the sky half of the ambient probe - what the sky adds on top of the "
			"environment bounce.\n\n"
			"This half is scaled by skylighting visibility, so it is strongest on open ground and "
			"upward-facing surfaces and fades to nothing under cover. Turn it down if open "
			"exteriors are too blue.");
	}
	ImGui::SliderFloat("Env IBL Saturation", &settings.EnvIBLSaturation, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Colour saturation of the environment half only. 0 makes the bounce light grey; 1 leaves it untouched.");
	}
	ImGui::SliderFloat("Sky IBL Saturation", &settings.SkyIBLSaturation, 0.0f, 2.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Colour saturation of the sky half only. 0 makes the sky contribution grey; 1 leaves it untouched.");
	}

	ImGui::SliderFloat("DALC Amount", &settings.DALCAmount, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox("Enable Interior", (bool*)&settings.EnableInterior);
	ImGui::Checkbox("Use Static IBL For Out-of-World Objects", (bool*)&settings.UseStaticIBL);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enables the use of static IBL textures for objects that are not in the world (e.g. inventory items).");
	}
	ImGui::SliderFloat("Fog Mix", &settings.FogAmount, 0.0f, 1.0f, "%.2f");
	ImGui::Checkbox("Preserve Fog Luminance", (bool*)&settings.PreserveFogLuminance);
	ImGui::Checkbox("Effect Normalization", (bool*)&settings.EffectNormalization);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Normalizes effect shader brightness based on IBL brightness.");
	}
	ImGui::SliderFloat("Effect Normalization Multiplier", &settings.EffectNormalizationMult, 0.0f, 100.0f, "%.2f");
	ImGui::SliderFloat("Minimum Effect Multiplier", &settings.MinEffectMult, 0.0f, 10.0f, "%.2f");

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void IBL::LoadSettings(json& o_json)
{
	settings = o_json;
}

void IBL::SaveSettings(json& o_json)
{
	o_json = settings;
}

void IBL::RestoreDefaultSettings()
{
	settings = {};
}

void IBL::EarlyPrepass()
{
	if (loaded && diffuseIBLTexture && diffuseSkyIBLTexture && staticDiffuseIBLTexture && staticSpecularIBLTexture) {
		auto context = globals::d3d::context;

		// Set PS shader resource
		{
			std::array<ID3D11ShaderResourceView*, 4> srvs = {
				diffuseIBLTexture->srv.get(),
				diffuseSkyIBLTexture->srv.get(),
				staticDiffuseIBLTexture->srv.get(),
				staticSpecularIBLTexture->srv.get()
			};
			context->PSSetShaderResources(76, 4, srvs.data());
		}
	}
}

void IBL::Prepass()
{
	auto context = globals::d3d::context;
	auto state = globals::state;

	auto& dynamicCubemaps = globals::features::dynamicCubemaps;

	auto& envTexture = dynamicCubemaps.envTexture;
	auto& envReflectionsTexture = dynamicCubemaps.envReflectionsTexture;

	// Unset PS shader resource
	{
		ID3D11ShaderResourceView* views[2]{ nullptr, nullptr };
		context->PSSetShaderResources(76, 2, views);
	}

	state->BeginPerfEvent("IBL");
	std::array<ID3D11ShaderResourceView*, 1> srvs = { (dynamicCubemaps.loaded && envTexture) ? envTexture->srv.get() : nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { diffuseIBLTexture->uav.get() };
	std::array<ID3D11SamplerState*, 1> samplers = { Deferred::GetSingleton()->linearSampler.get() };

	// IBL
	{
		samplers[0] = Deferred::GetSingleton()->linearSampler.get();

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(GetDiffuseIBLCS(), nullptr, 0);
		context->Dispatch(1, 1, 1);
	}

	// IBL with sky
	{
		srvs.at(0) = (dynamicCubemaps.loaded && envReflectionsTexture) ? envReflectionsTexture->srv.get() : nullptr;
		uavs.at(0) = diffuseSkyIBLTexture->uav.get();

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->Dispatch(1, 1, 1);
	}

	// Reset
	{
		srvs.fill(nullptr);
		uavs.fill(nullptr);
		samplers.fill(nullptr);

		context->CSSetSamplers(0, (uint)samplers.size(), samplers.data());
		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
		context->CSSetShader(nullptr, nullptr, 0);
	}
	state->EndPerfEvent();

	// Set PS shader resource
	{
		ID3D11ShaderResourceView* views[2]{ diffuseIBLTexture->srv.get(), diffuseSkyIBLTexture->srv.get() };
		context->PSSetShaderResources(76, 2, views);
	}
}

void IBL::SetupResources()
{
	GetDiffuseIBLCS();

	{
		D3D11_TEXTURE2D_DESC texDesc{
			.Width = 3,
			.Height = 1,
			.MipLevels = 1,
			.ArraySize = 1,
			.Format = DXGI_FORMAT_R16G16B16A16_FLOAT,
			.SampleDesc = { 1, 0 },
			.Usage = D3D11_USAGE_DEFAULT,
			.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
			.CPUAccessFlags = 0,
			.MiscFlags = 0
		};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = texDesc.MipLevels }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};

		diffuseIBLTexture = std::make_unique<Texture2D>(texDesc);
		diffuseIBLTexture->CreateSRV(srvDesc);
		diffuseIBLTexture->CreateUAV(uavDesc);
		diffuseSkyIBLTexture = std::make_unique<Texture2D>(texDesc);
		diffuseSkyIBLTexture->CreateSRV(srvDesc);
		diffuseSkyIBLTexture->CreateUAV(uavDesc);
	}

	auto device = globals::d3d::device;

	logger::debug("Loading static Diffuse IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\DiffuseIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			loaded = false;
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			loaded = false;
			return;
		}

		staticDiffuseIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		staticDiffuseIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticDiffuseIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		staticDiffuseIBLTexture->CreateSRV(srvDesc);
	}

	logger::debug("Loading static Specular IBL textures...");
	{
		DirectX::ScratchImage image;
		try {
			std::filesystem::path path = "Data\\Shaders\\IBL\\SpecIBL.dds";

			DX::ThrowIfFailed(LoadFromDDSFile(path.c_str(), DirectX::DDS_FLAGS_NONE, nullptr, image));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			loaded = false;
			return;
		}

		ID3D11Resource* pResource = nullptr;
		try {
			DX::ThrowIfFailed(CreateTexture(device,
				image.GetImages(), image.GetImageCount(),
				image.GetMetadata(), &pResource));
		} catch (const DX::com_exception& e) {
			logger::error("{}", e.what());
			loaded = false;
			return;
		}

		staticSpecularIBLTexture = eastl::make_unique<Texture2D>(reinterpret_cast<ID3D11Texture2D*>(pResource));

		staticSpecularIBLTexture->desc.MiscFlags |= D3D11_RESOURCE_MISC_TEXTURECUBE;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = staticSpecularIBLTexture->desc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE,
			.TextureCube = {
				.MostDetailedMip = 0,
				.MipLevels = 8 }
		};
		staticSpecularIBLTexture->CreateSRV(srvDesc);
	}
}

void IBL::ClearShaderCache()
{
	if (diffuseIBLCS)
		diffuseIBLCS->Release();
	diffuseIBLCS = nullptr;
}

ID3D11ComputeShader* IBL::GetDiffuseIBLCS()
{
	std::vector<std::pair<const char*, const char*>> defines;
	if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });
	if (!diffuseIBLCS)
		diffuseIBLCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\IBL\\DiffuseIBLCS.hlsl", defines, "cs_5_0"));
	return diffuseIBLCS;
}