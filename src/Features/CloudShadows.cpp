#include "CloudShadows.h"

#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	CloudShadows::Settings,
	Opacity)

void CloudShadows::DrawSettings()
{
	ImGui::SliderFloat("Opacity", &settings.Opacity, 0.0f, 1.0f, "%.1f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Higher values make cloud shadows darker.");
	}

	// (48o) Capture Clouds from the Main View (48n, official CS PR #3166) is always on. The capture status shows only when the
	// cloud picture is stale: one orange line.
	const uint32_t age = FramesSinceCapture();
	if (age != UINT32_MAX && age > kCaptureMaxAgeFrames) {
		ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "Cloud shadows are off: the cloud picture is %u frames old", age);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"The cloud picture the shadows come from was not updated for a while, so cloud shadows are off until the next update.\n"
				"Indoors, in menus and under a sky without clouds that is expected. Outdoors under clouds it should not stay.");
		}
	}
	if (viewCaptureShaderMissing)
		ImGui::TextDisabled("Main-view capture: waiting for its shader to compile");
}

void CloudShadows::LoadSettings(json& o_json)
{
	// (48o) The 48n "CaptureFromView" key is ignored (the capture is always on); it is dropped on the next save.
	settings = o_json;
}

void CloudShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

void CloudShadows::RestoreDefaultSettings()
{
	settings = {};
}

uint32_t CloudShadows::FramesSinceCapture() const
{
	if (lastReflectionsCaptureFrame == UINT32_MAX && lastViewCaptureFrame == UINT32_MAX)
		return UINT32_MAX;
	return globals::state->frameCount - lastCaptureFrame;
}

void CloudShadows::CheckResourcesSide(int side)
{
	static Util::FrameChecker frame_checker[6];
	if (!frame_checker[side].IsNewFrame())
		return;

	auto context = globals::d3d::context;

	float black[4] = { 0, 0, 0, 0 };
	context->ClearRenderTargetView(cubemapCloudOccRTVs[side], black);
}

void CloudShadows::SkyShaderHacks()
{
	if (overrideSky) {
		auto renderer = globals::game::renderer;
		auto context = globals::d3d::context;

		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		// render targets
		ID3D11RenderTargetView* rtvs[4];
		ID3D11DepthStencilView* dsv;
		context->OMGetRenderTargets(3, rtvs, &dsv);

		int side = -1;
		for (int i = 0; i < 6; ++i)
			if (rtvs[0] == reflections.cubeSideRTV[i]) {
				side = i;
				break;
			}
		if (side == -1)
			return;

		CheckResourcesSide(side);

		rtvs[3] = cubemapCloudOccRTVs[side];
		context->OMSetRenderTargets(4, rtvs, nullptr);

		float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
		UINT sampleMask = 0xffffffff;

		context->OMSetBlendState(cloudShadowBlendState, blendFactor, sampleMask);

		auto cubemapDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kCUBEMAP_REFLECTIONS];
		context->PSSetShaderResources(17, 1, &cubemapDepth.depthSRV);

		// Release COM objects to prevent memory leaks
		for (int i = 0; i < 3; ++i) {
			if (rtvs[i])
				rtvs[i]->Release();
		}
		if (dsv)
			dsv->Release();

		overrideSky = false;
		lastCaptureFrame = lastReflectionsCaptureFrame = globals::state->frameCount;  // (48n)
	}
}

void CloudShadows::ModifySky(RE::BSRenderPass* Pass)
{
	auto shadowState = globals::game::shadowState;

	GET_INSTANCE_MEMBER(cubeMapRenderTarget, shadowState);

	auto skyProperty = static_cast<const RE::BSSkyShaderProperty*>(Pass->shaderProperty);
	if (skyProperty->uiSkyObjectType != RE::BSSkyShaderProperty::SkyObject::SO_CLOUDS)
		return;

	if (cubeMapRenderTarget == RE::RENDER_TARGETS_CUBEMAP::kREFLECTIONS) {
		overrideSky = true;
		return;
	}

	// (48n) A main-view cloud layer: capture it when the reflection pass did not capture this frame.
	const auto state = globals::state;
	if (globals::game::isVR || !state->inWorld || lastReflectionsCaptureFrame == state->frameCount)
		return;
	if (mapMenuCheckedFrame != state->frameCount) {
		mapMenuCheckedFrame = state->frameCount;
		mapMenuOpen = globals::game::ui && globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
	}
	if (mapMenuOpen)
		return;
	viewCapturePending = true;
	captureGeometry = Pass->geometry;
}

void CloudShadows::CaptureFromViewIfPending()
{
	if (!viewCapturePending)
		return;
	viewCapturePending = false;

	auto* geometry = std::exchange(captureGeometry, nullptr);
	auto* triShape = geometry ? geometry->AsTriShape() : nullptr;
	if (!triShape)
		return;
	const UINT indexCount = 3u * triShape->GetTrishapeRuntimeData().triangleCount;
	if (!indexCount)
		return;

	auto context = globals::d3d::context;
	winrt::com_ptr<ID3D11Buffer> indexBuffer;
	DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
	UINT indexOffset = 0;
	context->IAGetIndexBuffer(indexBuffer.put(), &indexFormat, &indexOffset);
	if (!indexBuffer)
		return;

	// The bound sky vertex shader's capture variant, null until the shader cache has compiled it.
	const auto state = globals::state;
	if (!state->currentShader || state->currentShader->shaderType.get() != RE::BSShader::Type::Sky)
		return;
	constexpr auto captureFlag = static_cast<uint32_t>(SIE::ShaderCache::SkyShaderFlags::CloudShadowsCapture);
	auto* captureShader = globals::shaderCache->GetVertexShader(*state->currentShader, state->modifiedVertexDescriptor | captureFlag);
	viewCaptureShaderMissing = captureShader == nullptr;
	if (!captureShader)
		return;
	auto* captureVS = reinterpret_cast<ID3D11VertexShader*>(captureShader->shader);

	const uint32_t frame = state->frameCount;
	float black[4] = { 0, 0, 0, 0 };
	if (viewCaptureFrame != frame) {
		viewCaptureFrame = frame;
		for (uint32_t i = 0; i < kCaptureFacesPerFrame; i++) {
			const uint32_t face = (frame % kCaptureFramesPerCube) * kCaptureFacesPerFrame + i;
			viewCaptureFaces[i] = face;
			context->ClearRenderTargetView(cubemapCloudOccRTVs[face], black);
		}
	}

	ID3D11RenderTargetView* savedRTVs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
	ID3D11DepthStencilView* savedDSV = nullptr;
	context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, &savedDSV);
	ID3D11BlendState* savedBlendState = nullptr;
	float savedBlendFactor[4] = {};
	UINT savedSampleMask = 0;
	context->OMGetBlendState(&savedBlendState, savedBlendFactor, &savedSampleMask);
	ID3D11RasterizerState* savedRasterizerState = nullptr;
	context->RSGetState(&savedRasterizerState);
	D3D11_VIEWPORT savedViewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};
	UINT savedViewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
	context->RSGetViewports(&savedViewportCount, savedViewports);
	ID3D11VertexShader* savedVS = nullptr;
	context->VSGetShader(&savedVS, nullptr, nullptr);
	ID3D11Buffer* savedFaceCB = nullptr;
	context->VSGetConstantBuffers(kCaptureFaceSlot, 1, &savedFaceCB);

	D3D11_TEXTURE2D_DESC desc{};
	texCubemapCloudOcc->resource->GetDesc(&desc);
	const D3D11_VIEWPORT viewport = { 0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0.0f, 1.0f };
	context->RSSetViewports(1, &viewport);
	context->RSSetState(captureRasterizerState.get());
	const float blendFactor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
	context->OMSetBlendState(cloudShadowBlendState, blendFactor, 0xffffffff);
	context->VSSetShader(captureVS, nullptr, 0);

	for (const uint32_t face : viewCaptureFaces) {
		ID3D11Buffer* faceCB = captureFaceCBs[face].get();
		context->VSSetConstantBuffers(kCaptureFaceSlot, 1, &faceCB);
		// The occlusion is the pixel shader's fourth target; null targets discard its other outputs.
		ID3D11RenderTargetView* captureRTVs[4] = { nullptr, nullptr, nullptr, cubemapCloudOccRTVs[face] };
		context->OMSetRenderTargets(4, captureRTVs, nullptr);
		context->DrawIndexed(indexCount, 0, 0);
	}
	lastCaptureFrame = lastViewCaptureFrame = frame;

	context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, savedRTVs, savedDSV);
	context->OMSetBlendState(savedBlendState, savedBlendFactor, savedSampleMask);
	context->RSSetState(savedRasterizerState);
	context->RSSetViewports(savedViewportCount, savedViewports);
	context->VSSetShader(savedVS, nullptr, 0);
	context->VSSetConstantBuffers(kCaptureFaceSlot, 1, &savedFaceCB);

	for (auto* rtv : savedRTVs) {
		if (rtv)
			rtv->Release();
	}
	for (IUnknown* object : std::initializer_list<IUnknown*>{ savedDSV, savedBlendState, savedRasterizerState, savedVS, savedFaceCB }) {
		if (object)
			object->Release();
	}
}

void CloudShadows::ReflectionsPrepass()
{
	Util::FrameChecker frameChecker;
	if (frameChecker.IsNewFrame()) {
		if ((globals::game::sky->mode.get() != RE::Sky::Mode::kFull) ||
			!globals::game::sky->currentClimate)
			return;

		auto context = globals::d3d::context;

		context->CopyResource(texCubemapCloudOccCopy->resource.get(), texCubemapCloudOcc->resource.get());

		ID3D11ShaderResourceView* srv = texCubemapCloudOccCopy->srv.get();
		context->PSSetShaderResources(25, 1, &srv);
		context->CSSetShaderResources(25, 1, &srv);
	}
}

void CloudShadows::EarlyPrepass()
{
	if ((globals::game::sky->mode.get() != RE::Sky::Mode::kFull) ||
		!globals::game::sky->currentClimate)
		return;

	auto context = globals::d3d::context;

	// (48n) The main view writes the live occlusion later this frame (the sky draws after the opaque pass), so this
	// frame's draws read a copy; an occlusion older than kCaptureMaxAgeFrames belongs to another sky and is not bound.
	const uint32_t age = FramesSinceCapture();
	const bool current = age <= kCaptureMaxAgeFrames;
	if (current)
		context->CopyResource(texCubemapCloudOccCopy->resource.get(), texCubemapCloudOcc->resource.get());
	ID3D11ShaderResourceView* srv = current ? texCubemapCloudOccCopy->srv.get() : nullptr;
	context->PSSetShaderResources(25, 1, &srv);
	context->CSSetShaderResources(25, 1, &srv);
}

void CloudShadows::SetupResources()
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	{
		auto reflections = renderer->GetRendererData().cubemapRenderTargets[RE::RENDER_TARGET_CUBEMAP::kREFLECTIONS];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};

		reflections.texture->GetDesc(&texDesc);
		reflections.SRV->GetDesc(&srvDesc);

		texDesc.Format = srvDesc.Format = DXGI_FORMAT_R8_UNORM;

		texCubemapCloudOcc = std::make_unique<Texture2D>(texDesc);
		texCubemapCloudOcc->CreateSRV(srvDesc);

		for (int i = 0; i < 6; ++i) {
			reflections.cubeSideRTV[i]->GetDesc(&rtvDesc);
			rtvDesc.Format = texDesc.Format;
			DX::ThrowIfFailed(device->CreateRenderTargetView(texCubemapCloudOcc->resource.get(), &rtvDesc, cubemapCloudOccRTVs + i));
		}

		texCubemapCloudOccCopy = std::make_unique<Texture2D>(texDesc);
		texCubemapCloudOccCopy->CreateSRV(srvDesc);

		for (int i = 0; i < 6; ++i) {
			reflections.cubeSideRTV[i]->GetDesc(&rtvDesc);
			rtvDesc.Format = texDesc.Format;
			DX::ThrowIfFailed(device->CreateRenderTargetView(texCubemapCloudOccCopy->resource.get(), &rtvDesc, cubemapCloudOccCopyRTVs + i));
		}
	}
	{
		D3D11_BLEND_DESC blendDesc = {};
		blendDesc.AlphaToCoverageEnable = false;
		blendDesc.IndependentBlendEnable = false;

		blendDesc.RenderTarget[0].BlendEnable = true;
		blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
		blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
		blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
		blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

		DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &cloudShadowBlendState));
	}
	{
		// (48n) A face projection can flip the winding the engine culls by, so the main-view capture culls nothing.
		D3D11_RASTERIZER_DESC rasterizerDesc = {};
		rasterizerDesc.FillMode = D3D11_FILL_SOLID;
		rasterizerDesc.CullMode = D3D11_CULL_NONE;
		rasterizerDesc.DepthClipEnable = true;
		DX::ThrowIfFailed(device->CreateRasterizerState(&rasterizerDesc, captureRasterizerState.put()));
	}
	{
		// CreateBuffer reads the whole ByteWidth, which ConstantBufferDesc rounds up.
		struct CaptureFace
		{
			uint32_t index;
			uint32_t pad[15];
		};
		static_assert(sizeof(CaptureFace) == GetCBufferSize(sizeof(CaptureFace)));

		for (uint32_t face = 0; face < 6; ++face) {
			const CaptureFace data = { face, {} };
			D3D11_BUFFER_DESC cbDesc = ConstantBufferDesc<CaptureFace>(false);
			cbDesc.Usage = D3D11_USAGE_IMMUTABLE;
			cbDesc.CPUAccessFlags = 0;
			const D3D11_SUBRESOURCE_DATA initData = { &data, 0, 0 };
			DX::ThrowIfFailed(device->CreateBuffer(&cbDesc, &initData, captureFaceCBs[face].put()));
		}
	}
}

void CloudShadows::Hooks::BSSkyShader_SetupMaterial::thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
{
	globals::features::cloudShadows.ModifySky(Pass);
	func(This, Pass, RenderFlags);
}
