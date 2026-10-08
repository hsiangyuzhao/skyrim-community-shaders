#include "SubsurfaceScattering.h"

#include "Deferred.h"
#include "Features/TerrainBlending.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(SubsurfaceScattering::DiffusionProfile,
	BlurRadius, Thickness, Strength, Falloff)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	SubsurfaceScattering::Settings,
	EnableCharacterLighting,
	CharacterLightingStrength,
	SSMode,
	ScatterMode,
	BaseProfile,
	HumanProfile,
	BurleySamples,
	MeanFreePathBase,
	MeanFreePathHuman)

void SubsurfaceScattering::DrawSettings()
{
	if (ImGui::TreeNodeEx("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable Character Lighting", (bool*)&settings.EnableCharacterLighting);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Vanilla feature, not recommended.");
		}
		if (settings.EnableCharacterLighting) {
			ImGui::SliderFloat("Strength", &settings.CharacterLightingStrength, 0, 5, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Strength of Skyrim's built-in extra light on characters (multiplies the vanilla value).");
		}

		ImGui::RadioButton("Separable SSS", &settings.SSMode, 0);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Classic skin blur, tuned per colour with the profile settings below.");
		ImGui::SameLine();
		ImGui::RadioButton("Burley", &settings.SSMode, 1);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("More physically based skin light scattering, tuned by how far light travels into skin.");

		DrawBatch38Settings();

		if (settings.SSMode == 0) {
			if (ImGui::TreeNodeEx("Base Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat("Blur Radius", &settings.BaseProfile.BlurRadius, 0, 3, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Blur radius.");
				}

				ImGui::SliderFloat("Thickness", &settings.BaseProfile.Thickness, 0, 3, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Blur radius relative to depth.");
				}

				updateKernels = updateKernels || ImGui::ColorEdit3("Strength", (float*)&settings.BaseProfile.Strength);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How much each colour (red, green, blue) scatters under the surface.");
				updateKernels = updateKernels || ImGui::ColorEdit3("Falloff", (float*)&settings.BaseProfile.Falloff);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How far each colour spreads under the surface. Higher = wider, softer glow of that colour.");

				ImGui::TreePop();
			}

			if (ImGui::TreeNodeEx("Human Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::SliderFloat("Blur Radius", &settings.HumanProfile.BlurRadius, 0, 3, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Blur radius.");
				}

				ImGui::SliderFloat("Thickness", &settings.HumanProfile.Thickness, 0, 3, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Blur radius relative to depth.");
				}

				updateKernels = updateKernels || ImGui::ColorEdit3("Strength", (float*)&settings.HumanProfile.Strength);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How much each colour (red, green, blue) scatters under the surface.");
				updateKernels = updateKernels || ImGui::ColorEdit3("Falloff", (float*)&settings.HumanProfile.Falloff);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::Text("How far each colour spreads under the surface. Higher = wider, softer glow of that colour.");

				ImGui::TreePop();
			}
		} else if (settings.SSMode == 1) {
			ImGui::SliderInt("Burley Samples", (int*)&settings.BurleySamples, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Samples per pixel. Higher = smoother, less grainy skin but slower.");
			if (ImGui::TreeNodeEx("Base Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::ColorEdit3("Mean Free Path Color", (float*)&settings.MeanFreePathBase);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Controls how far light goes into the subsurface in the red, green, and blue channel. It is scaled by the Mean Free Path Distance.");
				}
				ImGui::SliderFloat("Mean Free Path Distance", &settings.MeanFreePathBase.w, 0.01f, 10.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Controls the distance that Mean Free Path Color goes into subsurface.");
				}
				ImGui::TreePop();
			}

			if (ImGui::TreeNodeEx("Human Profile", ImGuiTreeNodeFlags_DefaultOpen)) {
				ImGui::ColorEdit3("Mean Free Path Color", (float*)&settings.MeanFreePathHuman);
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Controls how far light goes into the subsurface in the red, green, and blue channel. It is scaled by the Mean Free Path Distance.");
				}
				ImGui::SliderFloat("Mean Free Path Distance", &settings.MeanFreePathHuman.w, 0.01f, 10.0f, "%.2f");
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("Controls the distance that Mean Free Path Color goes into subsurface.");
				}
				ImGui::TreePop();
			}
		}

		ImGui::Spacing();
		ImGui::Spacing();

		ImGui::TreePop();
	}
}

float3 SubsurfaceScattering::Gaussian(DiffusionProfile& a_profile, float variance, float r)
{
	/**
     * We use a falloff to modulate the shape of the profile. Big falloffs
     * spreads the shape making it wider, while small falloffs make it
     * narrower.
     */
	float falloff[3] = { a_profile.Falloff.x, a_profile.Falloff.y, a_profile.Falloff.z };
	float g[3];
	for (int i = 0; i < 3; i++) {
		float rr = r / (0.001f + falloff[i]);
		g[i] = exp((-(rr * rr)) / (2.0f * variance)) / (2.0f * 3.14f * variance);
	}
	return float3(g[0], g[1], g[2]);
}

float3 SubsurfaceScattering::Profile(DiffusionProfile& a_profile, float r)
{
	/**
     * We used the red channel of the original skin profile defined in
     * [d'Eon07] for all three channels. We noticed it can be used for green
     * and blue channels (scaled using the falloff parameter) without
     * introducing noticeable differences and allowing for total control over
     * the profile. For example, it allows to create blue SSS gradients, which
     * could be useful in case of rendering blue creatures.
     */
	return  // 0.233f * gaussian(0.0064f, r) + /* We consider this one to be directly bounced light, accounted by the strength parameter (see @STRENGTH) */
		0.100f * Gaussian(a_profile, 0.0484f, r) +
		0.118f * Gaussian(a_profile, 0.187f, r) +
		0.113f * Gaussian(a_profile, 0.567f, r) +
		0.358f * Gaussian(a_profile, 1.99f, r) +
		0.078f * Gaussian(a_profile, 7.41f, r);
}

void SubsurfaceScattering::CalculateKernel(DiffusionProfile& a_profile, Kernel& kernel)
{
	uint nSamples = SSSS_N_SAMPLES;

	const float RANGE = nSamples > 20 ? 3.0f : 2.0f;
	const float EXPONENT = 2.0f;

	// Calculate the offsets:
	float step = 2.0f * RANGE / (nSamples - 1);
	for (uint i = 0; i < nSamples; i++) {
		float o = -RANGE + float(i) * step;
		float sign = o < 0.0f ? -1.0f : 1.0f;
		kernel.Sample[i].w = RANGE * sign * abs(pow(o, EXPONENT)) / pow(RANGE, EXPONENT);
	}

	// Calculate the weights:
	for (uint i = 0; i < nSamples; i++) {
		float w0 = i > 0 ? abs(kernel.Sample[i].w - kernel.Sample[i - 1].w) : 0.0f;
		float w1 = i < nSamples - 1 ? abs(kernel.Sample[i].w - kernel.Sample[i + 1].w) : 0.0f;
		float area = (w0 + w1) / 2.0f;
		float3 t = area * Profile(a_profile, kernel.Sample[i].w);
		kernel.Sample[i].x = t.x;
		kernel.Sample[i].y = t.y;
		kernel.Sample[i].z = t.z;
	}

	// We want the offset 0.0 to come first:
	float4 t = kernel.Sample[nSamples / 2];
	for (uint i = nSamples / 2; i > 0; i--)
		kernel.Sample[i] = kernel.Sample[i - 1];
	kernel.Sample[0] = t;

	// Calculate the sum of the weights, we will need to normalize them below:
	float3 sum = float3(0.0f, 0.0f, 0.0f);
	for (uint i = 0; i < nSamples; i++)
		sum += float3(kernel.Sample[i]);

	// Normalize the weights:
	for (uint i = 0; i < nSamples; i++) {
		kernel.Sample[i].x /= sum.x;
		kernel.Sample[i].y /= sum.y;
		kernel.Sample[i].z /= sum.z;
	}

	// Tweak them using the desired strength. The first one is:
	//     lerp(1.0, kernel[0].rgb, strength)
	kernel.Sample[0].x = (1.0f - a_profile.Strength.x) * 1.0f + a_profile.Strength.x * kernel.Sample[0].x;
	kernel.Sample[0].y = (1.0f - a_profile.Strength.y) * 1.0f + a_profile.Strength.y * kernel.Sample[0].y;
	kernel.Sample[0].z = (1.0f - a_profile.Strength.z) * 1.0f + a_profile.Strength.z * kernel.Sample[0].z;

	// The others:
	//     lerp(0.0, kernel[0].rgb, strength)
	for (uint i = 1; i < nSamples; i++) {
		kernel.Sample[i].x *= a_profile.Strength.x;
		kernel.Sample[i].y *= a_profile.Strength.y;
		kernel.Sample[i].z *= a_profile.Strength.z;
	}
}

void SubsurfaceScattering::DrawSSS()
{
	if (!validMaterials)
		return;

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering");

	validMaterials = false;

	const bool upgrade = UpgradeActive();
	lastDrawUsedUpgrade = upgrade;
	lastDrawTime = std::chrono::steady_clock::now();

	Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::SubsurfaceScattering);

	auto dispatchCount = Util::GetScreenDispatchCount();

	{
		auto cameraData = Util::GetCameraData(0);

		blurCBData.SSSS_FOVY = atan(1.0f / cameraData.projMat.m[0][0]) * 2.0f * (180.0f / 3.14159265359f);

		blurCBData.BaseProfile = { settings.BaseProfile.BlurRadius, settings.BaseProfile.Thickness, 0, 0 };
		blurCBData.HumanProfile = { settings.HumanProfile.BlurRadius, settings.HumanProfile.Thickness, 0, 0 };

		blurCBData.BurleySamples = settings.BurleySamples;
		// (batch 38) Burley always takes the albedo out fully; the scatter mode is Separable's.
		// The 37c shaders treat both fields as padding.
		blurCBData.ScatterMode = (settings.SSMode == 0) ?
		                             (uint)std::clamp(settings.ScatterMode, (int)kPreScatter, (int)kPreAndPostScatter) :
		                             (uint)kPostScatter;
		blurCBData.PrepassMaskOnly = settings.SSMode == 1 ? 1u : 0u;

		blurCBData.MeanFreePathBase = settings.MeanFreePathBase;
		blurCBData.MeanFreePathHuman = settings.MeanFreePathHuman;

		blurCB->Update(blurCBData);
	}

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	{
		ID3D11Buffer* buffer[1] = { blurCB->CB() };
		context->CSSetConstantBuffers(1, 1, buffer);

		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		auto mask = renderer->GetRuntimeData().renderTargets[MASKS];
		auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
		auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

		ID3D11UnorderedAccessView* uav = blurHorizontalTemp->uav.get();

		auto& terrainBlending = globals::features::terrainBlending;

		ID3D11ShaderResourceView* views[5];
		views[0] = main.SRV;
		views[1] = terrainBlending.IsBlendingActive() ? terrainBlending.blendedDepthTexture16->srv.get() : depth.depthSRV;
		views[2] = mask.SRV;
		views[3] = albedo.SRV;
		views[4] = normal.SRV;

		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		if (upgrade) {
			DrawSSSUpgrade(dispatchCount);
		} else if (settings.SSMode == 0) {
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			// Horizontal pass to temporary texture
			{
				TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Horizontal");

				auto shader = GetComputeShaderHorizontalBlur();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
			}

			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

			// Vertical pass to main texture
			{
				TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Vertical");

				views[0] = blurHorizontalTemp->srv.get();
				context->CSSetShaderResources(0, 1, views);

				context->CopyResource(sssResult->resource.get(), main.texture);
				ID3D11UnorderedAccessView* uavs[1] = { sssResult->uav.get() };
				context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

				auto shader = GetComputeShaderVerticalBlur();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
			}
		} else if (settings.SSMode == 1) {
			// Burley pass to main texture
			{
				TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Burley");

				uav = sssResult->uav.get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

				auto shader = GetComputeShaderBurley();
				context->CSSetShader(shader, nullptr, 0);

				context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
			}
		}

		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

		// Composite SSS result back to main render target. (batch 38) The upgrade writes MAIN and the
		// DLSS-RR guide in its last pass, so it needs neither this pass nor the copy into sssResult.
		if (!upgrade) {
			TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Composite");

			views[0] = sssResult->srv.get();
			context->CSSetShaderResources(0, 1, views);

			ID3D11UnorderedAccessView* uavs[2] = { main.UAV, sssGuide->uav.get() };
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			auto shader = GetComputeShaderComposite();
			context->CSSetShader(shader, nullptr, 0);
			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);

			uavs[0] = nullptr;
			uavs[1] = nullptr;
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
		}
	}

	Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::SubsurfaceScattering);

	ID3D11Buffer* buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);

	ID3D11ShaderResourceView* views[5]{ nullptr, nullptr, nullptr, nullptr, nullptr };
	context->CSSetShaderResources(0, ARRAYSIZE(views), views);

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11ComputeShader* shader = nullptr;
	context->CSSetShader(shader, nullptr, 0);
}

bool SubsurfaceScattering::UpgradeActive() const
{
	return true;
}

void SubsurfaceScattering::DrawBatch38Settings()
{
	if (settings.SSMode == 0) {
		ImGui::Text("Albedo Handling");
		ImGui::RadioButton("Pre-scatter", &settings.ScatterMode, kPreScatter);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Blur the lit colour directly. Blurs skin texture detail along with the light.");
		ImGui::SameLine();
		ImGui::RadioButton("Post-scatter", &settings.ScatterMode, kPostScatter);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Take the skin colour out, blur the light, put the colour back. Keeps texture detail sharpest.");
		ImGui::SameLine();
		ImGui::RadioButton("Pre and Post", &settings.ScatterMode, kPreAndPostScatter);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Half of the skin colour before the blur, half after (square root on each side). The default, a middle ground.");
	}
	ImGui::Spacing();
}

void SubsurfaceScattering::DrawSSSUpgrade(const Util::DispatchCount& a_dispatchCount)
{
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto* timers = Util::GpuPassTimers::GetSingleton();

	// Allocated on first use (and again after a resolution change): 37c never needs it.
	if (!diffuseNoAlbedoTex || diffuseNoAlbedoTex->desc.Width != sssResult->desc.Width || diffuseNoAlbedoTex->desc.Height != sssResult->desc.Height) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		sssResult->srv->GetDesc(&srvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		sssResult->uav->GetDesc(&uavDesc);
		diffuseNoAlbedoTex = std::make_unique<Texture2D>(sssResult->desc);
		diffuseNoAlbedoTex->CreateSRV(srvDesc);
		diffuseNoAlbedoTex->CreateUAV(uavDesc);
	}

	ID3D11SamplerState* sampler = globals::deferred->pointSampler.get();
	context->CSSetSamplers(0, 1, &sampler);

	timers->End(Util::GpuBucket::SubsurfaceScattering);
	timers->Begin(Util::GpuBucket::SubsurfaceScatteringPrepass);
	{
		TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Prepass");

		ID3D11UnorderedAccessView* uav = diffuseNoAlbedoTex->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		context->CSSetShader(GetComputeShaderPrepassV2(), nullptr, 0);
		context->Dispatch(a_dispatchCount.x, a_dispatchCount.y, 1);
		uav = nullptr;
		context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
	}
	timers->End(Util::GpuBucket::SubsurfaceScatteringPrepass);
	timers->Begin(Util::GpuBucket::SubsurfaceScattering);

	// Colour input of the blur: the pre-pass output (t1..t4 stay as bound by DrawSSS).
	ID3D11ShaderResourceView* colorSrv = diffuseNoAlbedoTex->srv.get();
	context->CSSetShaderResources(0, 1, &colorSrv);

	// Last pass: skin pixels straight into MAIN (each reads its own original colour back from there),
	// and the DLSS-RR guide for every pixel, as the 37c composite did. No copy, no composite.
	ID3D11UnorderedAccessView* outUavs[2] = { main.UAV, sssGuide->uav.get() };
	ID3D11UnorderedAccessView* nullUavs[2] = { nullptr, nullptr };

	if (settings.SSMode == 0) {
		{
			TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Horizontal");
			ID3D11UnorderedAccessView* uav = blurHorizontalTemp->uav.get();
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
			context->CSSetShader(GetComputeShaderHorizontalBlurV2(), nullptr, 0);
			context->Dispatch(a_dispatchCount.x, a_dispatchCount.y, 1);
			uav = nullptr;
			context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
		}
		{
			TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Vertical");
			colorSrv = blurHorizontalTemp->srv.get();
			context->CSSetShaderResources(0, 1, &colorSrv);
			context->CSSetUnorderedAccessViews(0, 2, outUavs, nullptr);
			context->CSSetShader(GetComputeShaderVerticalBlurV2(), nullptr, 0);
			context->Dispatch(a_dispatchCount.x, a_dispatchCount.y, 1);
			context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
		}
	} else {
		TracyD3D11Zone(globals::state->tracyCtx, "Subsurface Scattering - Burley");
		context->CSSetUnorderedAccessViews(0, 2, outUavs, nullptr);
		context->CSSetShader(GetComputeShaderBurleyV2(), nullptr, 0);
		context->Dispatch(a_dispatchCount.x, a_dispatchCount.y, 1);
		context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
	}

	colorSrv = nullptr;
	context->CSSetShaderResources(0, 1, &colorSrv);
	sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);
}

void SubsurfaceScattering::SetupResources()
{
	{
		blurCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<BlurCB>());
	}

	auto renderer = globals::game::renderer;

	{
		auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		main.texture->GetDesc(&texDesc);

		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		main.SRV->GetDesc(&srvDesc);

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
		main.UAV->GetDesc(&uavDesc);

		blurHorizontalTemp = std::make_unique<Texture2D>(texDesc);
		blurHorizontalTemp->CreateSRV(srvDesc);
		blurHorizontalTemp->CreateUAV(uavDesc);

		sssResult = std::make_unique<Texture2D>(texDesc);
		sssResult->CreateSRV(srvDesc);
		sssResult->CreateUAV(uavDesc);

		texDesc.Format = DXGI_FORMAT_R16_FLOAT;
		srvDesc.Format = DXGI_FORMAT_R16_FLOAT;
		uavDesc.Format = DXGI_FORMAT_R16_FLOAT;
		sssGuide = std::make_unique<Texture2D>(texDesc);
		sssGuide->CreateSRV(srvDesc);
		sssGuide->CreateUAV(uavDesc);
	}
}

void SubsurfaceScattering::Reset()
{
	auto shaderManager = globals::game::smState;
	auto shaderCache = globals::shaderCache;
	shaderManager->characterLightEnabled = shaderCache->IsEnabled() ? settings.EnableCharacterLighting : true;
	if (shaderManager->characterLightEnabled) {
		if (CharacterLightingStrengthOriginal == -1.0f) {
			CharacterLightingStrengthOriginal = shaderManager->characterLightParams[2];
		}
		shaderManager->characterLightParams[2] = settings.CharacterLightingStrength * CharacterLightingStrengthOriginal;
	}

	if (updateKernels) {
		updateKernels = false;
		CalculateKernel(settings.BaseProfile, blurCBData.BaseKernel);
		CalculateKernel(settings.HumanProfile, blurCBData.HumanKernel);
	}
}

void SubsurfaceScattering::RestoreDefaultSettings()
{
	settings = {};
}

void SubsurfaceScattering::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.ScatterMode = std::clamp(settings.ScatterMode, (int)kPreScatter, (int)kPreAndPostScatter);
}

void SubsurfaceScattering::SaveSettings(json& o_json)
{
	o_json = settings;
}

void SubsurfaceScattering::ClearShaderCache()
{
	if (horizontalSSBlur) {
		horizontalSSBlur->Release();
		horizontalSSBlur = nullptr;
	}
	if (verticalSSBlur) {
		verticalSSBlur->Release();
		verticalSSBlur = nullptr;
	}
	if (burleySS) {
		burleySS->Release();
		burleySS = nullptr;
	}
	if (compositeSSS) {
		compositeSSS->Release();
		compositeSSS = nullptr;
	}
	for (auto* shader : { &prepassSSV2, &horizontalSSBlurV2, &verticalSSBlurV2, &burleySSV2 }) {
		if (*shader) {
			(*shader)->Release();
			*shader = nullptr;
		}
	}
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderPrepassV2()
{
	if (!prepassSSV2) {
		logger::debug("Compiling prepassSSV2");
		prepassSSV2 = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\DiffuseExtractionCS.hlsl", {}, "cs_5_0");
	}
	return prepassSSV2;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderHorizontalBlurV2()
{
	if (!horizontalSSBlurV2) {
		logger::debug("Compiling horizontalSSBlurV2");
		horizontalSSBlurV2 = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSV2CS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlurV2;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderVerticalBlurV2()
{
	if (!verticalSSBlurV2) {
		logger::debug("Compiling verticalSSBlurV2");
		verticalSSBlurV2 = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSV2CS.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlurV2;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderBurleyV2()
{
	if (!burleySSV2) {
		logger::debug("Compiling burleySSV2");
		burleySSV2 = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSV2CS.hlsl", { { "BURLEY", "" } }, "cs_5_0");
	}
	return burleySSV2;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderHorizontalBlur()
{
	if (!horizontalSSBlur) {
		logger::debug("Compiling horizontalSSBlur");
		horizontalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "HORIZONTAL", "" } }, "cs_5_0");
	}
	return horizontalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderVerticalBlur()
{
	if (!verticalSSBlur) {
		logger::debug("Compiling verticalSSBlur");
		verticalSSBlur = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", {}, "cs_5_0");
	}
	return verticalSSBlur;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderBurley()
{
	if (!burleySS) {
		logger::debug("Compiling burleySS");
		burleySS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SeparableSSSCS.hlsl", { { "BURLEY", "" } }, "cs_5_0");
	}
	return burleySS;
}

ID3D11ComputeShader* SubsurfaceScattering::GetComputeShaderComposite()
{
	if (!compositeSSS) {
		logger::debug("Compiling compositeSSS");
		compositeSSS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\SubsurfaceScattering\\SSSCompositeCS.hlsl", {}, "cs_5_0");
	}
	return compositeSSS;
}

void SubsurfaceScattering::DataLoaded()
{
	// (batch 38, upstream c321154fa) A load order without the keyword used to crash here.
	auto form = RE::TESForm::LookupByEditorID("IsBeastRace");
	isBeastRaceKeyword = form ? form->As<RE::BGSKeyword>() : nullptr;
	if (!isBeastRaceKeyword)
		logger::warn("[SSS] IsBeastRace keyword is unavailable; every face uses the base (beast) profile");
}

void SubsurfaceScattering::PostPostLoad()
{
	Hooks::Install();
}

void SubsurfaceScattering::BSLightingShader_SetupSkin(RE::BSRenderPass* a_pass)
{
	auto deferred = globals::deferred;
	auto state = globals::state;

	if (deferred->deferredPass) {
		if (a_pass->shaderProperty->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kFace, RE::BSShaderProperty::EShaderPropertyFlag::kFaceGenRGBTint)) {
			bool isBeastRace = true;

			auto geometry = a_pass->geometry;
			if (isBeastRaceKeyword)
				if (auto userData = geometry->GetUserData())
					if (auto actor = userData->As<RE::Actor>())
						if (auto race = actor->GetRace())
							isBeastRace = race->HasKeyword(isBeastRaceKeyword);

			validMaterials = true;

			if (isBeastRace)
				state->permutationData.ExtraShaderDescriptor |= (uint)State::ExtraShaderDescriptors::IsBeastRace;
			else
				state->permutationData.ExtraShaderDescriptor &= ~(uint)State::ExtraShaderDescriptors::IsBeastRace;
		}
	}
}

void SubsurfaceScattering::Hooks::BSLightingShader_SetupGeometry::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::subsurfaceScattering.BSLightingShader_SetupSkin(Pass);
	func(This, Pass, RenderFlags);
}
