#include "ScreenSpaceShadows.h"

#include "Features/TerrainBlending.h"
#include "State.h"
#include "Utils/GpuTimers.h"

#pragma warning(push)
#pragma warning(disable: 4838 4244)
#include "ScreenSpaceShadows/bend_sss_cpu.h"
#pragma warning(pop)

using RE::RENDER_TARGETS;

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpaceShadows::BendSettings,
	Enable,
	SampleCount,
	SurfaceThickness,
	BilinearThreshold,
	ShadowContrast,
	WorldSpaceThickness,
	MinThickness,
	ContactLength,
	EdgeSmoothing)

void ScreenSpaceShadows::DrawSettings()
{
	if (ImGui::TreeNodeEx("General", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Enable", (bool*)&bendSettings.Enable);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Adds small, sharp sun shadows (contact shadows) that normal shadow maps miss, e.g. under feet and in crevices.");
		ImGui::SliderInt("Sample Count Multiplier", (int*)&bendSettings.SampleCount, 1, 4);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How far each pixel searches for a shadow caster. Higher = longer contact shadows but slower.");
		ImGui::SliderFloat("Surface Thickness", &bendSettings.SurfaceThickness, 0.005f, 0.05f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How thick objects are assumed to be when casting these shadows. Higher = fuller shadows, but thin objects may cast too much.");
		ImGui::SliderFloat("Bilinear Threshold", &bendSettings.BilinearThreshold, 0.02f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("How big a depth jump counts as an object edge, where depth is not smoothed. Tweak if shadow edges look jagged or leak.");
		ImGui::SliderFloat("Shadow Contrast", &bendSettings.ShadowContrast, 0.0f, 4.0f);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Sharpens the fade into and out of shadow. Higher = harder, darker contact shadows.");

		ImGui::Spacing();
		ImGui::Checkbox("World-Space Thickness", (bool*)&bendSettings.WorldSpaceThickness);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Treats objects as at least Min Thickness deep, so long shadows come out solid instead of hollow, and fades shadows from casters farther than Contact Length. Off = the old behaviour.");
		if (bendSettings.WorldSpaceThickness) {
			ImGui::SliderFloat("Min Thickness", &bendSettings.MinThickness, 0.0f, 100.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("How deep an object is assumed to be at least, in game units (70 = 1 m). Higher = fuller shadows, but more shadow leaking behind thin objects.");
			ImGui::SliderFloat("Contact Length", &bendSettings.ContactLength, 30.0f, 1000.0f, "%.0f units");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Shadows from casters farther away than this (along the sun's direction) fade out. Lower = only short contact shadows; higher = longer ones, with more mistakes far away.");
		}
		ImGui::Checkbox("Edge Smoothing", (bool*)&bendSettings.EdgeSmoothing);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Softens the jagged, flickering 1-pixel edges of these shadows without blurring across object outlines. Uses only the current frame, so nothing smears. About 0.05-0.1 ms.");

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}
}

void ScreenSpaceShadows::ReleaseRaymarchShaders()
{
	if (raymarchCS) {
		raymarchCS->Release();
		raymarchCS = nullptr;
	}
	if (raymarchRightCS) {
		raymarchRightCS->Release();
		raymarchRightCS = nullptr;
	}
}

void ScreenSpaceShadows::ClearShaderCache()
{
	ReleaseRaymarchShaders();
	if (edgeSmoothCS) {
		edgeSmoothCS->Release();
		edgeSmoothCS = nullptr;
	}
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeEdgeSmooth()
{
	if (!edgeSmoothCS) {
		std::vector<std::pair<const char*, const char*>> defines;
		if (globals::features::terrainBlending.loaded)
			defines.push_back({ "TERRAIN_BLENDING", "" });
		edgeSmoothCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\EdgeSmoothCS.hlsl", defines, "cs_5_0");
	}
	return edgeSmoothCS;
}

uint ScreenSpaceShadows::GetScaledSampleCount()
{
	// (45 SSS, upstream c021f3e1d) Scale from the render (dynamic) size, not the output size. Before,
	// the shader was compiled for the output size (240 samples at 4K) and cut back at run time to the
	// render-size count (160 at 2560x1440) with a loop break -- same picture, more registers.
	const float2 renderSize = Util::ConvertToDynamic(globals::state->screenSize);

	// Scale sample count based on both dimensions relative to 1920x1080 reference
	float2 referenceRes = { 1920.0f, 1080.0f };
	float referenceArea = referenceRes.x * referenceRes.y;
	float currentArea = renderSize.x * renderSize.y;
	float areaScale = std::sqrt(currentArea / referenceArea);
	uint scaledSampleCount = static_cast<uint>(std::round(std::clamp(bendSettings.SampleCount, 1u, 4u) * 60 * areaScale));

	// Quantize to steps of 8 so small dynamic-resolution changes do not recompile the shader.
	scaledSampleCount = ((scaledSampleCount + 7u) / 8u) * 8u;
	return std::max(scaledSampleCount, 8u);
}

void ScreenSpaceShadows::CompileRaymarchShaders()
{
	const uint scaledSampleCount = GetScaledSampleCount();
	if (scaledSampleCount != lastCompiledSampleCount) {
		lastCompiledSampleCount = scaledSampleCount;
		ReleaseRaymarchShaders();
	}

	if (raymarchCS && (raymarchRightCS || !globals::game::isVR))
		return;

	const auto sampleCount = std::format("{}", scaledSampleCount);
	std::vector<std::pair<const char*, const char*>> defines{ { "SAMPLE_COUNT", sampleCount.c_str() } };
	// (45 SSS, upstream) TerrainBlending swaps its R32_FLOAT blended depth into kPOST_ZPREPASS_COPY;
	// the shader must then declare the depth as float, not unorm.
	if (globals::features::terrainBlending.loaded)
		defines.push_back({ "TERRAIN_BLENDING", "" });

	if (!raymarchCS)
		raymarchCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");

	if (globals::game::isVR && !raymarchRightCS) {
		defines.push_back({ "RIGHT", "" });
		raymarchRightCS = (ID3D11ComputeShader*)Util::CompileShader(L"Data\\Shaders\\ScreenSpaceShadows\\RaymarchCS.hlsl", defines, "cs_5_0");
	}
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarch()
{
	CompileRaymarchShaders();
	return raymarchCS;
}

ID3D11ComputeShader* ScreenSpaceShadows::GetComputeRaymarchRight()
{
	CompileRaymarchShaders();
	return raymarchRightCS;
}

void ScreenSpaceShadows::DrawShadows()
{
	ZoneScoped;
	auto state = globals::state;
	TracyD3D11Zone(state->tracyCtx, "Screen Space Shadows");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto accumulator = *globals::game::currentAccumulator.get();
	auto dirLight = skyrim_cast<RE::NiDirectionalLight*>(accumulator->GetRuntimeData().activeShadowSceneNode->GetRuntimeData().sunLight->light.get());

	auto& directionNi = dirLight->GetWorldDirection();
	float3 light = { directionNi.x, directionNi.y, directionNi.z };
	light.Normalize();
	float4 lightProjection = float4(-light.x, -light.y, -light.z, 0.0f);

	// Helper lambda to calculate light projection for a given eye
	auto CalculateLightProjection = [&](uint32_t eyeIndex = 0) -> std::array<float, 4> {
		auto viewProjMat = globals::game::frameBufferCached.GetCameraViewProj(eyeIndex).Transpose();
		auto projectedLight = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewProjMat);
		return { projectedLight.x, projectedLight.y, projectedLight.z, projectedLight.w };
	};

	auto lightProjectionF = CalculateLightProjection(0);

	float2 renderSize = Util::ConvertToDynamic(state->screenSize);
	int viewportSize[2] = { (int)renderSize.x, (int)renderSize.y };

	if (globals::game::isVR)
		viewportSize[0] /= 2;

	int minRenderBounds[2] = { 0, 0 };
	int maxRenderBounds[2] = { viewportSize[0], viewportSize[1] };

	// Setup common render state
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	context->CSSetShaderResources(0, 1, &depth.depthSRV);

	// (45 SSS) With Edge Smoothing the ray march writes into rawShadowsTexture and the smoothing pass
	// writes the final texture; otherwise the ray march writes the final texture directly.
	ID3D11ComputeShader* smoothShader = (bendSettings.EdgeSmoothing && rawShadowsTexture && !globals::game::isVR) ? GetComputeEdgeSmooth() : nullptr;
	if (smoothShader) {
		float white[4] = { 1, 1, 1, 1 };
		context->ClearUnorderedAccessViewFloat(rawShadowsTexture->uav.get(), white);
	}

	auto uav = smoothShader ? rawShadowsTexture->uav.get() : screenSpaceShadowsTexture->uav.get();
	context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);

	context->CSSetSamplers(0, 1, &pointBorderSampler);

	auto buffer = raymarchCB->CB();
	context->CSSetConstantBuffers(1, 1, &buffer);

	auto viewport = globals::game::graphicsState;

	float2 dynamicRes = { viewport->GetRuntimeData().dynamicResolutionWidthRatio, viewport->GetRuntimeData().dynamicResolutionHeightRatio };

	// Data shared by every dispatch (and by the edge smoothing pass)
	RaymarchCB common{};
	common.FarDepthValue = 1.0f;
	common.NearDepthValue = 0.0f;
	common.DynamicRes = dynamicRes;
	common.InvDepthTextureSize[0] = 1.0f / (float)viewportSize[0];
	common.InvDepthTextureSize[1] = 1.0f / (float)viewportSize[1];
	common.settings = bendSettings;
	common.settings.SampleCount = std::clamp(bendSettings.SampleCount, 1u, 4u);
	{
		// (45 SSS) World-Space Thickness: linear depth, the sun in view space and the projection scale.
		const float4 cameraData = Util::GetCameraData();
		common.CameraData[0] = cameraData.x;
		common.CameraData[1] = cameraData.y;
		common.CameraData[2] = cameraData.z;
		common.CameraData[3] = cameraData.w;

		auto viewMat = globals::game::frameBufferCached.GetCameraView(0).Transpose();
		auto sunVS = DirectX::SimpleMath::Vector4::Transform(lightProjection, viewMat);
		float3 sunDirVS = { sunVS.x, sunVS.y, sunVS.z };
		sunDirVS.Normalize();
		common.SunDirVS[0] = sunDirVS.x;
		common.SunDirVS[1] = sunDirVS.y;
		common.SunDirVS[2] = sunDirVS.z;
		common.SunDirVS[3] = 0.0f;

		const auto& proj = globals::game::frameBufferCached.GetCameraProj(0);
		common.ProjInfo[0] = proj._11 != 0.0f ? 1.0f / proj._11 : 1.0f;
		common.ProjInfo[1] = proj._22 != 0.0f ? 1.0f / proj._22 : 1.0f;
		common.ProjInfo[2] = (float)viewportSize[0];
		common.ProjInfo[3] = (float)viewportSize[1];
	}

	// Shared dispatch logic for both VR and non-VR
	auto DispatchEye = [&](const char* eyeName, ID3D11ComputeShader* shader, const float* lightProj,
						   float invTexSizeX, float invTexSizeY) {
		std::string eventName;
		const char* tracyName = "SSS - Ray March";

		if (globals::state->frameAnnotations && eyeName) {
			eventName = std::format("SSS - Ray March ({})", eyeName);
			tracyName = eventName.c_str();
			globals::state->BeginPerfEvent(eventName);
		} else if (globals::state->frameAnnotations) {
			globals::state->BeginPerfEvent("SSS - Ray March");
		}

		context->CSSetShader(shader, nullptr, 0);

		auto dispatchList = Bend::BuildDispatchList(const_cast<float*>(lightProj), viewportSize, minRenderBounds, maxRenderBounds);

		for (int i = 0; i < dispatchList.DispatchCount; i++) {
			TracyD3D11Zone(globals::state->tracyCtx, tracyName);

			auto dispatchData = dispatchList.Dispatch[i];

			RaymarchCB data = common;
			data.LightCoordinate[0] = dispatchList.LightCoordinate_Shader[0];
			data.LightCoordinate[1] = dispatchList.LightCoordinate_Shader[1];
			data.LightCoordinate[2] = dispatchList.LightCoordinate_Shader[2];
			data.LightCoordinate[3] = dispatchList.LightCoordinate_Shader[3];

			data.WaveOffset[0] = dispatchData.WaveOffset_Shader[0];
			data.WaveOffset[1] = dispatchData.WaveOffset_Shader[1];

			data.InvDepthTextureSize[0] = invTexSizeX;
			data.InvDepthTextureSize[1] = invTexSizeY;

			raymarchCB->Update(data);

			context->Dispatch(dispatchData.WaveCount[0], dispatchData.WaveCount[1], dispatchData.WaveCount[2]);
		}

		if (globals::state->frameAnnotations) {
			globals::state->EndPerfEvent();
		}
	};

	float InvTexSizeX = 1.0f / (float)viewportSize[0];
	float InvTexSizeY = 1.0f / (float)viewportSize[1];

	if (!globals::game::isVR) {
		DispatchEye(nullptr, GetComputeRaymarch(), lightProjectionF.data(), InvTexSizeX, InvTexSizeY);
	} else {
		DispatchEye("Left Eye", GetComputeRaymarch(), lightProjectionF.data(), InvTexSizeX, InvTexSizeY);

		// Calculate light projection for right eye
		auto lightProjectionRightF = CalculateLightProjection(1);
		DispatchEye("Right Eye", GetComputeRaymarchRight(), lightProjectionRightF.data(), InvTexSizeX, InvTexSizeY);
	}

	ID3D11UnorderedAccessView* uavs[1]{ nullptr };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	if (smoothShader) {
		// (45 SSS) Edge Smoothing: 3x3 depth-aware blur, this frame's ray march only (no history).
		TracyD3D11Zone(globals::state->tracyCtx, "SSS - Edge Smoothing");
		if (globals::state->frameAnnotations)
			globals::state->BeginPerfEvent("SSS - Edge Smoothing");

		raymarchCB->Update(common);
		ID3D11ShaderResourceView* smoothSrvs[2] = { depth.depthSRV, rawShadowsTexture->srv.get() };
		context->CSSetShaderResources(0, 2, smoothSrvs);
		auto finalUav = screenSpaceShadowsTexture->uav.get();
		context->CSSetUnorderedAccessViews(0, 1, &finalUav, nullptr);
		context->CSSetShader(smoothShader, nullptr, 0);
		context->Dispatch(((uint)viewportSize[0] + 7) / 8, ((uint)viewportSize[1] + 7) / 8, 1);

		context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

		if (globals::state->frameAnnotations)
			globals::state->EndPerfEvent();
	}

	ID3D11ShaderResourceView* views[2]{ nullptr, nullptr };
	context->CSSetShaderResources(0, 2, views);

	context->CSSetShader(nullptr, nullptr, 0);

	ID3D11SamplerState* sampler = nullptr;
	context->CSSetSamplers(0, 1, &sampler);

	buffer = nullptr;
	context->CSSetConstantBuffers(1, 1, &buffer);
}

void ScreenSpaceShadows::Prepass()
{
	auto context = globals::d3d::context;

	float white[4] = { 1, 1, 1, 1 };
	context->ClearUnorderedAccessViewFloat(screenSpaceShadowsTexture->uav.get(), white);

	if (auto sky = globals::game::sky) {
		if (bendSettings.Enable && sky->mode.get() == RE::Sky::Mode::kFull) {
			// (45 SSS) Own row in the Performance Overlay's GPU table (was folded into "Other").
			Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::ScreenSpaceShadows);
			DrawShadows();
			Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::ScreenSpaceShadows);
		}
	}

	auto view = screenSpaceShadowsTexture->srv.get();
	context->PSSetShaderResources(45, 1, &view);
}

void ScreenSpaceShadows::LoadSettings(json& o_json)
{
	bendSettings = o_json;
}

void ScreenSpaceShadows::SaveSettings(json& o_json)
{
	o_json = bendSettings;
}

void ScreenSpaceShadows::RestoreDefaultSettings()
{
	bendSettings = {};
}

bool ScreenSpaceShadows::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void ScreenSpaceShadows::SetupResources()
{
	raymarchCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<RaymarchCB>());

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		samplerDesc.BorderColor[0] = 1.0f;
		samplerDesc.BorderColor[1] = 1.0f;
		samplerDesc.BorderColor[2] = 1.0f;
		samplerDesc.BorderColor[3] = 1.0f;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, &pointBorderSampler));
	}

	{
		auto renderer = globals::game::renderer;
		auto shadowMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSHADOW_MASK];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};

		shadowMask.texture->GetDesc(&texDesc);
		shadowMask.SRV->GetDesc(&srvDesc);

		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		srvDesc.Format = texDesc.Format;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
		screenSpaceShadowsTexture = std::make_unique<Texture2D>(texDesc);
		screenSpaceShadowsTexture->CreateSRV(srvDesc);
		screenSpaceShadowsTexture->CreateUAV(uavDesc);

		// (45 SSS) Edge Smoothing source. Same format; the final texture keeps its name, format and t45 slot.
		rawShadowsTexture = std::make_unique<Texture2D>(texDesc);
		rawShadowsTexture->CreateSRV(srvDesc);
		rawShadowsTexture->CreateUAV(uavDesc);
	}
}