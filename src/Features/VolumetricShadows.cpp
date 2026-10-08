#include "VolumetricShadows.h"

#include "Deferred.h"
#include "Features/VolumetricLighting.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/Batch38.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	VolumetricShadows::Settings,
	ParticleShadows,
	ForwardSoftShadows,
	MergeGodRayMaps)

bool VolumetricShadows::HasShaderDefine(RE::BSShader::Type shaderType)
{
	switch (shaderType) {
	case RE::BSShader::Type::Lighting:
	case RE::BSShader::Type::Effect:
		return true;
	default:
		return false;
	}
}

bool VolumetricShadows::ParticleShadowsActive() const
{
	return loaded && Batch38::IsOn() && settings.ParticleShadows;
}

bool VolumetricShadows::ForwardSoftShadowsActive() const
{
	return loaded && Batch38::IsOn() && settings.ForwardSoftShadows;
}

bool VolumetricShadows::WantsVsm() const
{
	return ParticleShadowsActive() || ForwardSoftShadowsActive();
}

bool VolumetricShadows::VsmValid() const
{
	if (lastBuildFrame == UINT32_MAX || !globals::state)
		return false;
	return globals::state->frameCount - lastBuildFrame <= 1u;
}

VolumetricShadows::CommonBufferData VolumetricShadows::GetCommonBufferData() const
{
	CommonBufferData data{};
	const bool valid = VsmValid();
	data.ParticleShadows = ParticleShadowsActive() && valid;
	data.ForwardSoftShadows = ForwardSoftShadowsActive() && valid;
	return data;
}

void VolumetricShadows::SetupResources()
{
	D3D11_SAMPLER_DESC samplerDesc = {};
	samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	samplerDesc.MaxAnisotropy = 1;
	samplerDesc.MinLOD = 0;
	samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
	linearSampler = nullptr;
	DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, linearSampler.put()));

	CompileShaders();
}

void VolumetricShadows::CompileShaders()
{
	auto compile = [](const char* a_define) {
		return static_cast<ID3D11ComputeShader*>(Util::CompileShader(
			a_define[0] == 'D' ? L"Data\\Shaders\\VolumetricShadows\\DownsampleShadowCS.hlsl" : L"Data\\Shaders\\VolumetricShadows\\BlurShadowCS.hlsl",
			{ { a_define, "" } }, "cs_5_0"));
	};
	if (!downsampleMip0CS)
		downsampleMip0CS = compile("DOWNSAMPLE_SHADOW_MIP0");
	if (!downsampleMip1CS)
		downsampleMip1CS = compile("DOWNSAMPLE_SHADOW_MIP1");
	if (!blurHCS)
		blurHCS = compile("BLUR_HORIZONTAL");
	if (!blurVCS)
		blurVCS = compile("BLUR_VERTICAL");
}

void VolumetricShadows::ClearShaderCache()
{
	for (auto** cs : { &downsampleMip0CS, &downsampleMip1CS, &blurHCS, &blurVCS }) {
		if (*cs) {
			(*cs)->Release();
			*cs = nullptr;
		}
	}
	CompileShaders();
}

void VolumetricShadows::EnsureTextures()
{
	if (vsmTexture)
		return;

	auto device = globals::d3d::device;

	D3D11_TEXTURE2D_DESC desc{};
	desc.Width = kVsmSize;
	desc.Height = kVsmSize;
	desc.MipLevels = 2;
	desc.ArraySize = 1;
	desc.Format = DXGI_FORMAT_R16G16_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage = D3D11_USAGE_DEFAULT;
	desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	auto makeViews = [&](winrt::com_ptr<ID3D11Texture2D>& o_tex, winrt::com_ptr<ID3D11ShaderResourceView>(&o_mipSrv)[2],
						 winrt::com_ptr<ID3D11UnorderedAccessView>(&o_mipUav)[2], const char* a_name) {
		DX::ThrowIfFailed(device->CreateTexture2D(&desc, nullptr, o_tex.put()));
		Util::SetResourceName(o_tex.get(), a_name);
		for (uint32_t mip = 0; mip < 2; ++mip) {
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MostDetailedMip = mip;
			srvDesc.Texture2D.MipLevels = 1;
			DX::ThrowIfFailed(device->CreateShaderResourceView(o_tex.get(), &srvDesc, o_mipSrv[mip].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = desc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = mip;
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(o_tex.get(), &uavDesc, o_mipUav[mip].put()));
		}
	};

	makeViews(vsmTexture, vsmMipSRV, vsmMipUAV, "VolumetricShadows::VSM");
	makeViews(blurTexture, blurMipSRV, blurMipUAV, "VolumetricShadows::BlurTemp");

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format = desc.Format;
	srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MostDetailedMip = 0;
	srvDesc.Texture2D.MipLevels = 2;
	DX::ThrowIfFailed(device->CreateShaderResourceView(vsmTexture.get(), &srvDesc, vsmSRV.put()));
}

void VolumetricShadows::BindVsm(ID3D11ShaderResourceView* a_srv)
{
	globals::d3d::context->PSSetShaderResources(kVsmSlot, 1, &a_srv);
}

void VolumetricShadows::OnShadowCapture(ID3D11ShaderResourceView* a_shadowMap)
{
	// Interiors without Interior Sun have no sun cascades to speak of; the shaders gate on
	// !InInterior as well (upstream e214c451f's C++ half: do not build from a stale map).
	const bool interior = Util::IsInterior();
	if (!WantsVsm() || !a_shadowMap || interior) {
		BindVsm(nullptr);
		return;
	}

	if (!downsampleMip0CS || !downsampleMip1CS || !blurHCS || !blurVCS) {
		BindVsm(nullptr);
		return;
	}

	// One build per frame, however many shadow-mask draws the engine issues.
	if (lastBuildFrame == globals::state->frameCount && vsmSRV) {
		BindVsm(vsmSRV.get());
		return;
	}

	winrt::com_ptr<ID3D11Resource> shadowResource;
	a_shadowMap->GetResource(shadowResource.put());
	auto shadowTexture = shadowResource.try_as<ID3D11Texture2D>();
	if (!shadowTexture) {
		BindVsm(nullptr);
		return;
	}
	D3D11_TEXTURE2D_DESC srcDesc{};
	shadowTexture->GetDesc(&srcDesc);
	// Two cascades are packed into mips 0/1; the downsample reduces 2x per thread and up to
	// 8x more per group, so the source has to be at least 2x the VSM.
	if (srcDesc.ArraySize < 2 || srcDesc.Width < kVsmSize * 2 || srcDesc.Width != srcDesc.Height) {
		BindVsm(nullptr);
		return;
	}
	sourceWidth = srcDesc.Width;

	EnsureTextures();

	auto context = globals::d3d::context;

	// The god-ray maps (vanilla Volumetric Lighting's own sun shadow maps) are only re-rendered
	// while vanilla VL runs; otherwise they hold whatever the last VL frame left behind. Fall
	// back to the cascade array itself, for which min(a, a) = a.
	ID3D11ShaderResourceView* godRaySrv = a_shadowMap;
	usedGodRayMaps = false;
	if (settings.MergeGodRayMaps && globals::features::volumetricLighting.IsVanillaVLRunning()) {
		auto& esram = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM];
		if (esram.depthSRV && esram.texture) {
			D3D11_TEXTURE2D_DESC esramDesc{};
			esram.texture->GetDesc(&esramDesc);
			if (esramDesc.ArraySize >= 2) {
				godRaySrv = esram.depthSRV;
				usedGodRayMaps = true;
			}
		}
	}

	auto timers = Util::GpuPassTimers::GetSingleton();
	timers->Begin(Util::GpuBucket::VolumetricShadows);

	ID3D11ShaderResourceView* srvs[2]{ a_shadowMap, godRaySrv };
	context->CSSetShaderResources(0, 2, srvs);
	ID3D11SamplerState* sampler = linearSampler.get();
	context->CSSetSamplers(0, 1, &sampler);

	// Each group covers 16x16 source texels (8x8 threads gathering 2x2).
	const uint32_t groups = srcDesc.Width / 16;

	ID3D11UnorderedAccessView* uav = vsmMipUAV[0].get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(downsampleMip0CS, nullptr, 0);
	context->Dispatch(groups, groups, 1);

	uav = vsmMipUAV[1].get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	context->CSSetShader(downsampleMip1CS, nullptr, 0);
	context->Dispatch(groups, groups, 1);

	ID3D11ShaderResourceView* nullSrvs[2]{};
	ID3D11UnorderedAccessView* nullUav = nullptr;
	context->CSSetShaderResources(0, 2, nullSrvs);
	context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

	// 11-tap separable Gaussian, per mip: VSM -> temp (horizontal), temp -> VSM (vertical).
	constexpr uint32_t kGroup = 128;
	for (uint32_t mip = 0; mip < 2; ++mip) {
		const uint32_t size = kVsmSize >> mip;

		ID3D11ShaderResourceView* srv = vsmMipSRV[mip].get();
		uav = blurMipUAV[mip].get();
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(blurHCS, nullptr, 0);
		context->Dispatch((size + kGroup - 1) / kGroup, size, 1);
		context->CSSetShaderResources(0, 1, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);

		srv = blurMipSRV[mip].get();
		uav = vsmMipUAV[mip].get();
		context->CSSetShaderResources(0, 1, &srv);
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(blurVCS, nullptr, 0);
		context->Dispatch(size, (size + kGroup - 1) / kGroup, 1);
		context->CSSetShaderResources(0, 1, nullSrvs);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
	}

	ID3D11SamplerState* nullSampler = nullptr;
	context->CSSetSamplers(0, 1, &nullSampler);
	context->CSSetShader(nullptr, nullptr, 0);

	timers->End(Util::GpuBucket::VolumetricShadows);

	lastBuildFrame = globals::state->frameCount;
	BindVsm(vsmSRV.get());
}

void VolumetricShadows::DrawSettings()
{
	ImGui::SeparatorText("Sun Shadow on Effects");

	ImGui::Checkbox("Smoke & Particles Receive Sun Shadow", &settings.ParticleShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Smoke, steam, mist and other lit see-through effects get darker where they pass into\n"
			"the shadow of a building, tree or cliff, instead of glowing in full sunlight.\n"
			"Cheap: one small blurred shadow map per frame.");

	ImGui::Checkbox("Soft Sun Shadow on Forward Objects", &settings.ForwardSoftShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Off by default. Objects drawn without the screen shadow mask (glass, some hair and\n"
			"alpha-blended meshes) normally use a sharp 16-sample shadow lookup. On: they use the\n"
			"same soft blurred shadow map as smoke. Cheaper and softer, but it can let a little\n"
			"light through where an object touches the thing shadowing it, and small shadows\n"
			"(thin branches) get washed out.");

	if (ImGui::TreeNode("Advanced")) {
		ImGui::Checkbox("Merge Vanilla God-Ray Shadow Maps", &settings.MergeGodRayMaps);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Upstream behaviour: also uses the shadow maps vanilla Volumetric Lighting draws for its\n"
				"light shafts. Only applied while vanilla Volumetric Lighting is running.");
		ImGui::Text("Status: %s", VsmValid() ? "built this frame" : (WantsVsm() ? "not built (no sun shadow here)" : "off"));
		if (sourceWidth)
			ImGui::Text("Sun shadow map: %u px, god-ray maps merged: %s", sourceWidth, usedGodRayMaps ? "yes" : "no");
		ImGui::TreePop();
	}
}

void VolumetricShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void VolumetricShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void VolumetricShadows::RestoreDefaultSettings()
{
	settings = {};
}
