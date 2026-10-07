#include "Deferred.h"

#include <DDSTextureLoader.h>

#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"

#include "Features/DynamicCubemaps.h"
#include "Features/IBL.h"
#include "Features/NRD.h"
#include "Features/PhysicalSky.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/Skylighting.h"
#include "Features/SubsurfaceScattering.h"
#include "Features/TerrainBlending.h"
#include "Features/Upscaling.h"
#include "Features/VariableRateShading.h"
#include "Features/VolumetricShadows.h"

#include "Hooks.h"
#include "Utils/Batch37b.h"
#include "Utils/GpuPhaseTimeline.h"
#include "Utils/GpuTimers.h"
#include "Utils/OcclusionDryRun.h"

// CPU-side timing of our own work. Purely observational: every one of these is a
// QueryPerformanceCounter bracket around an existing call, and each is a no-op unless the
// Performance Overlay's draw-call table is on screen. No orchestration is changed.
//
// Why it has to happen here: the overlay's CPU attribution charges the gap between two
// engine draw calls to whichever BSShader type is drawing, so all of the work below used
// to be billed to an unrelated shader type. State::Debug() now subtracts these brackets
// from the interval it charges, which both fixes those rows and gives our features their
// own honest rows.
namespace
{
	/**
	 * @brief Stable timing key for a Feature, cached per instance.
	 *
	 * Util::CpuPassScope holds a string_view, so the key has to outlive the scope, and
	 * GetShortName() returns by value. Building a std::string per feature per frame would
	 * be a cost paid even with the overlay off, so cache one string per Feature; the
	 * feature list is fixed after load and unordered_map never moves its nodes, so the
	 * returned reference stays valid for the process lifetime.
	 */
	const std::string& CpuTimerKey(Feature* a_feature)
	{
		static std::unordered_map<const Feature*, std::string> cache;
		auto it = cache.find(a_feature);
		if (it == cache.end())
			it = cache.emplace(a_feature, a_feature->GetShortName()).first;
		return it->second;
	}
}

struct DepthStates
{
	ID3D11DepthStencilState* a[6][40];
};

struct BlendStates
{
	ID3D11BlendState* a[7][2][13][2];

	static BlendStates* GetSingleton()
	{
		static auto blendStates = reinterpret_cast<BlendStates*>(REL::RelocationID(524749, 411364).address());
		return blendStates;
	}
};

namespace
{
	/// @brief One replacement G-buffer target that we own.
	struct HijackedTarget
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11RenderTargetView> rtv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	};

	/// @brief Owning references to the replacement targets, keyed by RE::RENDER_TARGET.
	///
	/// (batch 16, item 3) The five slots we hijack live in the *game's* render-target array as
	/// bare ID3D11* pointers, and Deferred::SetupResources re-runs every time the game
	/// recreates its render targets. The old code overwrote those pointers with no Release on
	/// any path, so each re-entry orphaned five full-screen targets and their views -- 158 MiB
	/// at 4K, gone for the rest of the process.
	///
	/// Holding our own reference is what lets that be fixed without guessing. On re-entry the
	/// new pointers go into the game's slot first, and only then is the previous entry here
	/// destroyed -- so whichever of the two references was the last one standing, ours or the
	/// slot's, the texture is actually freed.
	///
	/// What is deliberately NOT done: releasing whatever the slot contained on entry. On the
	/// first call that is a texture the *game* created, and nothing in the code establishes
	/// that the slot is its sole owner. Being wrong about that is a use-after-free on every
	/// boot for every user, which is a far worse trade than one 158 MiB one-off. A map rather
	/// than an array because RENDER_TARGET::kTOTAL differs between VR and flat and this file
	/// is compiled once for both.
	std::unordered_map<int, HijackedTarget> hijackedTargets;
}

void SetupRenderTarget(RE::RENDER_TARGET target, D3D11_TEXTURE2D_DESC texDesc, D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc, D3D11_RENDER_TARGET_VIEW_DESC rtvDesc, D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc, DXGI_FORMAT format, uint bindFlags)
{
	auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	texDesc.BindFlags = bindFlags;
	texDesc.Format = format;
	srvDesc.Format = format;
	rtvDesc.Format = format;
	uavDesc.Format = format;

	HijackedTarget fresh;
	DX::ThrowIfFailed(device->CreateTexture2D(&texDesc, nullptr, fresh.texture.put()));

	if (texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE)
		DX::ThrowIfFailed(device->CreateShaderResourceView(fresh.texture.get(), &srvDesc, fresh.srv.put()));

	if (texDesc.BindFlags & D3D11_BIND_RENDER_TARGET)
		DX::ThrowIfFailed(device->CreateRenderTargetView(fresh.texture.get(), &rtvDesc, fresh.rtv.put()));

	if (texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS)
		DX::ThrowIfFailed(device->CreateUnorderedAccessView(fresh.texture.get(), &uavDesc, fresh.uav.put()));

	// Publish into the game's slot. Only the fields we actually built are written, exactly as
	// before: none of the five callers asks for a UAV, and overwriting data.UAV with null would
	// be a behaviour change, not a fix.
	auto& data = renderer->GetRuntimeData().renderTargets[target];
	data.texture = fresh.texture.get();
	if (fresh.srv)
		data.SRV = fresh.srv.get();
	if (fresh.rtv)
		data.RTV = fresh.rtv.get();
	if (fresh.uav)
		data.UAV = fresh.uav.get();

	// Last: drop the previous generation. The game's slot no longer points at it, so if the
	// game had not already released it, the reference we are dropping here is the final one.
	hijackedTargets[static_cast<int>(target)] = std::move(fresh);
}

void Deferred::SetupResources()
{
	auto renderer = globals::game::renderer;

	{
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

		D3D11_TEXTURE2D_DESC texDesc{};
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

		main.texture->GetDesc(&texDesc);
		main.SRV->GetDesc(&srvDesc);
		main.RTV->GetDesc(&rtvDesc);
		main.UAV->GetDesc(&uavDesc);

		// Available targets:
		// MAIN ONLY ALPHA
		// WATER REFLECTIONS
		// BLURFULL_BUFFER
		// LENSFLAREVIS
		// SAO DOWNSCALED
		// SAO CAMERAZ+MIP_LEVEL_0_ESRAM
		// SAO_RAWAO_DOWNSCALED
		// SAO_RAWAO_PREVIOUS_DOWNSCALDE
		// SAO_TEMP_BLUR_DOWNSCALED
		// INDIRECT
		// INDIRECT_DOWNSCALED
		// RAWINDIRECT
		// RAWINDIRECT_DOWNSCALED
		// RAWINDIRECT_PREVIOUS
		// RAWINDIRECT_PREVIOUS_DOWNSCALED
		// RAWINDIRECT_SWAP
		// VOLUMETRIC_LIGHTING_HALF_RES
		// VOLUMETRIC_LIGHTING_BLUR_HALF_RES
		// VOLUMETRIC_LIGHTING_QUARTER_RES
		// VOLUMETRIC_LIGHTING_BLUR_QUARTER_RES
		// TEMPORAL_AA_WATER_1
		// TEMPORAL_AA_WATER_2

		// Albedo
		SetupRenderTarget(ALBEDO, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
		// Specular
		SetupRenderTarget(SPECULAR, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
		// Reflectance
		SetupRenderTarget(REFLECTANCE, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
		// Normal + Roughness
		SetupRenderTarget(NORMALROUGHNESS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
		// Masks
		SetupRenderTarget(MASKS, texDesc, srvDesc, rtvDesc, uavDesc, DXGI_FORMAT_R11G11B10_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
	}

	{
		auto device = globals::d3d::device;

		D3D11_SAMPLER_DESC samplerDesc = {};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
		samplerDesc.MaxAnisotropy = 1;
		samplerDesc.MinLOD = 0;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		linearSampler = nullptr;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));

		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
		pointSampler = nullptr;
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, pointSampler.put()));
	}

	{
		D3D11_BUFFER_DESC sbDesc{};
		sbDesc.Usage = D3D11_USAGE_DEFAULT;
		sbDesc.CPUAccessFlags = 0;
		sbDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		sbDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_UNKNOWN;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
		srvDesc.Buffer.FirstElement = 0;

		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_UNKNOWN;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
		uavDesc.Buffer.FirstElement = 0;
		uavDesc.Buffer.Flags = 0;

		std::uint32_t numElements = 1;

		sbDesc.StructureByteStride = sizeof(PerGeometry);
		sbDesc.ByteWidth = sizeof(PerGeometry) * numElements;
		perShadow = std::make_unique<Buffer>(sbDesc);
		srvDesc.Buffer.NumElements = numElements;
		perShadow->CreateSRV(srvDesc);
		uavDesc.Buffer.NumElements = numElements;
		perShadow->CreateUAV(uavDesc);

		copyShadowCS = nullptr;
		copyShadowCS.attach(static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\CopyShadowDataCS.hlsl", {}, "cs_5_0")));
	}

	{
		D3D11_TEXTURE2D_DESC texDesc;
		auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		mainTex.texture->GetDesc(&texDesc);

		texDesc.Format = DXGI_FORMAT_R11G11B10_FLOAT;
		texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
			.Texture2D = {
				.MostDetailedMip = 0,
				.MipLevels = 1 }
		};
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {
			.Format = texDesc.Format,
			.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
			.Texture2D = { .MipSlice = 0 }
		};
	}
}

void Deferred::CopyShadowData()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "CopyShadowData");

	auto context = globals::d3d::context;

	ID3D11UnorderedAccessView* uavs[1]{ perShadow->uav.get() };
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	ID3D11Buffer* buffers[3];
	context->PSGetConstantBuffers(0, 3, buffers);
	context->PSGetConstantBuffers(12, 1, buffers + 1);

	context->CSSetConstantBuffers(0, 3, buffers);

	context->CSSetShader(copyShadowCS.get(), nullptr, 0);

	context->Dispatch(1, 1, 1);

	uavs[0] = nullptr;
	context->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);

	std::fill(buffers, buffers + ARRAYSIZE(buffers), nullptr);
	context->CSSetConstantBuffers(0, 3, buffers);

	context->CSSetShader(nullptr, nullptr, 0);

	{
		context->PSGetShaderResources(4, 1, &shadowView);

		ID3D11ShaderResourceView* srvs[2]{
			shadowView,
			perShadow->srv.get(),
		};

		context->PSSetShaderResources(18, ARRAYSIZE(srvs), srvs);

		// (batch 38) Shared capture for Volumetric Fog / Volumetric Shadows (see Deferred.h).
		capturedShadowMap.copy_from(shadowView);
		capturedShadowFrame = globals::state->frameCount;

		// Release COM object to prevent memory leak
		if (shadowView)
			shadowView->Release();
	}

	// (batch 38) Volumetric Shadows builds its VSM from the capture right here, while the
	// cascades are fresh and before any effect or particle that samples it is drawn.
	if (globals::features::volumetricShadows.loaded)
		globals::features::volumetricShadows.OnShadowCapture(capturedShadowMap.get());
}

bool Deferred::HasFreshShadowCapture() const
{
	if (!capturedShadowMap || capturedShadowFrame == UINT32_MAX)
		return false;
	return globals::state->frameCount - capturedShadowFrame <= 1u;
}

void Deferred::ReflectionsPrepasses()
{
	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	auto state = globals::state;

	state->activeReflections = true;
	{
		Util::CpuPassScope timer("Shared Data");
		state->UpdateSharedData(false, false);
	}

	ZoneScoped;
	TracyD3D11Zone(globals::game::graphicsState->tracyCtx, "Early Prepass");

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			Util::CpuPassScope timer(CpuTimerKey(feature));
			feature->ReflectionsPrepass();
		}
	}
}

void Deferred::EarlyPrepasses()
{
	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	{
		Util::CpuPassScope timer("Shared Data");
		globals::state->UpdateSharedData(false, true);
	}

	ZoneScoped;
	TracyD3D11Zone(globals::game::graphicsState->tracyCtx, "Early Prepass");

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			Util::CpuPassScope timer(CpuTimerKey(feature));
			feature->EarlyPrepass();
		}
	}
}

void Deferred::PrepassPasses()
{
	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Prepass");

	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	{
		Util::CpuPassScope timer("TruePBR");
		globals::truePBR->PrePass();
	}
	for (auto* feature : Feature::GetFeatureList()) {
		if (feature->loaded) {
			Util::CpuPassScope timer(CpuTimerKey(feature));
			feature->Prepass();
		}
	}
}

void Deferred::StartDeferred()
{
	// (batch 36) Our setup and prepasses before the opaque pass; the passes among them that
	// have a GPU Passes row take their own time out of this.
	Util::GpuPhaseTimeline::GetSingleton()->Push(Util::GpuScope::CsOther);

	{
		Util::CpuPassScope timer("Shared Data");
		globals::state->UpdateSharedData(true, false);
	}

	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(renderTargets, shadowState)
	GET_INSTANCE_MEMBER(setRenderTargetMode, shadowState)
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	// Backup original render targets
	for (uint i = 0; i < 4; i++) {
		forwardRenderTargets[i] = renderTargets[i];
	}

	RE::RENDER_TARGET targets[8]{
		RE::RENDER_TARGET::kMAIN,
		RE::RENDER_TARGET::kMOTION_VECTOR,
		NORMALROUGHNESS,
		ALBEDO,
		SPECULAR,
		REFLECTANCE,
		MASKS,
		RE::RENDER_TARGET::kNONE
	};

	for (uint i = 2; i < 8; i++) {
		renderTargets[i] = targets[i];                                             // We must use unused targets to be indexable
		setRenderTargetMode[i] = RE::BSGraphics::SetRenderTargetMode::SRTM_CLEAR;  // Dirty from last frame, this calls ClearRenderTargetView once
	}

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	deferredPass = true;

	{
		auto context = globals::d3d::context;

		ID3D11Buffer* buffers[1] = { *globals::game::perFrame.get() };

		ID3D11Buffer* vrBuffer = nullptr;

		if (REL::Module::IsVR()) {
			static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
			vrBuffer = *VRValues.get();
		}
		if (vrBuffer) {
			context->CSSetConstantBuffers(12, 1, buffers);
			context->CSSetConstantBuffers(13, 1, &vrBuffer);
		} else {
			context->CSSetConstantBuffers(12, 1, buffers);
		}
	}

	PrepassPasses();

	OverrideBlendStates();

	// (batch 36) Everything the GPU does from here until EndDeferred is the engine's opaque
	// pass, split per draw into terrain / objects / characters / trees / grass / LOD.
	auto* timeline = Util::GpuPhaseTimeline::GetSingleton();
	timeline->Pop(Util::GpuScope::CsOther);
	timeline->Push(Util::GpuScope::Opaque);

	// Last, so none of the prepass work above can run under a coarse shading rate. Inside the
	// Opaque scope; its own rate-image dispatch is billed to "our timed passes" by GpuPassTimers.
	globals::features::variableRateShading.BeginOpaquePass();
}

void Deferred::DeferredPasses()
{
	{
		Util::CpuPassScope timer("Upscaling");
		globals::features::upscaling.CheckFrameConstants();
	}

	ZoneScoped;
	TracyD3D11Zone(globals::state->tracyCtx, "Deferred");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	{
		ID3D11Buffer* buffers[1] = { *globals::game::perFrame };
		ID3D11Buffer* vrBuffer = nullptr;

		if (REL::Module::IsVR()) {
			static REL::Relocation<ID3D11Buffer**> VRValues{ REL::Offset(0x3180688) };
			vrBuffer = *VRValues.get();
		}
		if (vrBuffer) {
			context->CSSetConstantBuffers(12, 1, buffers);
			context->CSSetConstantBuffers(13, 1, &vrBuffer);
		} else {
			context->CSSetConstantBuffers(12, 1, buffers);
		}
	}

	auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
	auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	auto normalRoughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
	auto masks = renderer->GetRuntimeData().renderTargets[MASKS];

	auto main = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[0]];
	auto normals = renderer->GetRuntimeData().renderTargets[forwardRenderTargets[2]];
	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];

	auto motionVectors = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	bool interior = Util::IsInterior();

	auto& skylighting = globals::features::skylighting;

	// (batch C1) NRD guide service: publish this frame's viewZ / normal+roughness /
	// motion-vector guides before any consumer dispatches. Must run before
	// DrawSSRTDiffuse / DrawSSRTSpecular so their REBLUR instances see guides that
	// describe this frame.
	auto& nrdService = globals::features::nrd;
	if (nrdService.loaded) {
		Util::CpuPassScope timer("NRD");
		nrdService.PrepareGuides();
	}

	auto& ssgi = globals::features::screenSpaceGI;
	if (ssgi.loaded) {
		Util::CpuPassScope timer("ScreenSpaceGI");
		ssgi.DrawSSGI();
	}
	auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec] = ssgi.GetOutputTextures();
	bool ssgi_hq_spec = ssgi.settings.EnableExperimentalSpecularGI;

	auto& ssrt = globals::features::screenSpaceRayTracing;
	if (ssrt.loaded && ssrt.settings.EnableDiffuse) {
		Util::CpuPassScope timer("ScreenSpaceRayTracing");
		ssrt.DrawSSRTDiffuse();
	}

	auto dispatchCount = Util::GetScreenDispatchCount(true);

	auto& sss = globals::features::subsurfaceScattering;
	if (sss.loaded) {
		Util::CpuPassScope timer("SubsurfaceScattering");
		sss.DrawSSS();
	}

	auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	if (dynamicCubemaps.loaded) {
		Util::CpuPassScope timer("DynamicCubemaps");
		dynamicCubemaps.UpdateCubemap();
	}

	auto& terrainBlending = globals::features::terrainBlending;

	auto& ibl = globals::features::ibl;

	auto& physSky = globals::features::physicalSky;

	if (ssrt.loaded && ssrt.settings.EnableSpecular) {
		Util::CpuPassScope timer("ScreenSpaceRayTracing");
		ssrt.DrawSSRTSpecular();
	}

	// Deferred Composite
	{
		TracyD3D11Zone(globals::state->tracyCtx, "Deferred Composite");
		Util::CpuPassScope timer("Deferred Composite");

		ID3D11ShaderResourceView* srvs[]{
			specular.SRV,
			albedo.SRV,
			normalRoughness.SRV,
			masks.SRV,
			dynamicCubemaps.loaded || REL::Module::IsVR() ? (terrainBlending.loaded ? terrainBlending.blendedDepthTexture16->srv.get() : depth.depthSRV) : nullptr,
			dynamicCubemaps.loaded ? reflectance.SRV : nullptr,
			dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr,
			dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr,
			dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr,
			dynamicCubemaps.loaded && skylighting.loaded ? skylighting.stbn_vec3_2Dx1D_128x128x64.get() : nullptr,
			ssgi_ao,
			ssgi_hq_spec ? nullptr : ssgi_y,
			ssgi_hq_spec ? nullptr : ssgi_cocg,
			ssgi_hq_spec ? ssgi_gi_spec : nullptr,
			ibl.loaded ? ibl.diffuseIBLTexture->srv.get() : nullptr,
			ibl.loaded ? ibl.diffuseSkyIBLTexture->srv.get() : nullptr,
			// (audit P6) Bind the specular result directly; texOutput was a redundant
			// full-screen copy of it made at the end of DrawSSRTSpecular.
			(ssrt.loaded && ssrt.settings.EnableSpecular) ? ssrt.GetSpecularCompositeSRV() : nullptr,
			physSky.loaded ? physSky.texApLut->srv.get() : nullptr,
			physSky.loaded ? physSky.texApShadow->srv.get() : nullptr,
			// t19 (ambient reinjection) Smoothed SSRT diffuse hit confidence. Written by
			// ssrt_diffuse_composite.hlsl inside DrawSSRTDiffuse above, i.e. earlier in this
			// same frame. Bound only when that pass ran; the shader side is additionally gated
			// on ssrtSettings.DiffuseMult > 0 and AmbientReinjection != 0, both of which
			// GetCommonBufferData clears when EnableDiffuse is off, so a null binding here can
			// never be read.
			(ssrt.loaded && ssrt.settings.EnableDiffuse) ? ssrt.texSSRTDiffuseConfidenceSmooth->srv.get() : nullptr,
			// t20 (batch 36f) kPOST_ZPREPASS_COPY, the depth SSRT and NRD classify the distance limit by.
			ssrt.loaded ? renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY].depthSRV : nullptr,
		};

		ID3D11SamplerState* samplers[]{
			dynamicCubemaps.loaded ? linearSampler.get() : nullptr,
			physSky.loaded ? physSky.sampSv.get() : nullptr,
		};
		context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

		context->CSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

		ID3D11UnorderedAccessView* uavs[3]{ main.UAV, normals.UAV, motionVectors.UAV };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		// (batch 36f) b1: what t16 holds this frame (folded unpack) and the distance limit.
		if (ssrt.loaded)
			ssrt.BindCompositeConstants();

		// (batch 37b, C-4) "SSGI AO does not darken direct light" is a compile-time branch of the
		// composite; when its effective state flips, both composite variants are rebuilt once.
		if (const bool aoSparesDirect = Batch37b::SsgiAoSparesDirectActive(); aoSparesDirect != compositeAoSparesDirect) {
			ClearShaderCache();
			compositeAoSparesDirect = aoSparesDirect;
		}
		auto shader = interior ? GetComputeMainCompositeInterior() : GetComputeMainComposite();
		context->CSSetShader(shader, nullptr, 0);

		context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
	}

	// Clear
	{
		ID3D11ShaderResourceView* views[21]{};
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[3]{ nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11Buffer* buffers[1] = { nullptr };
		context->CSSetConstantBuffers(12, 1, buffers);
		context->CSSetConstantBuffers(1, 1, buffers);  // (batch 36f) SSRT composite constants

		ID3D11SamplerState* samplers[2]{ nullptr, nullptr };
		context->CSSetSamplers(0, ARRAYSIZE(samplers), samplers);

		context->CSSetShader(nullptr, nullptr, 0);
	}

	if (dynamicCubemaps.loaded) {
		Util::CpuPassScope timer("DynamicCubemaps");
		dynamicCubemaps.PostDeferred();
	}

	auto& upscaling = globals::features::upscaling;
	if (upscaling.loaded && upscaling.settings.enableDLSSRR) {
		Util::CpuPassScope timer("Upscaling");
		upscaling.SnapshotBeforeTransparency();
	}
}

void Deferred::EndDeferred()
{
	// Before the early-outs: the opaque pass is over whatever happens next. VRS is switched off
	// first so the shading-rate window nests inside the timeline's Opaque scope.
	globals::features::variableRateShading.EndOpaquePass();

	// (batch 36) Closed before the early returns; a no-op if StartDeferred never opened it.
	Util::GpuPhaseTimeline::GetSingleton()->Pop(Util::GpuScope::Opaque);

	if (!globals::state->inWorld)
		return;

	auto shaderCache = globals::shaderCache;

	if (!shaderCache->IsEnabled())
		return;

	auto shadowState = globals::game::shadowState;
	GET_INSTANCE_MEMBER(renderTargets, shadowState)
	GET_INSTANCE_MEMBER(stateUpdateFlags, shadowState)

	// Do not render to our targets past this point
	for (uint i = 0; i < 4; i++) {
		renderTargets[i] = forwardRenderTargets[i];
	}

	for (uint i = 4; i < 8; i++) {
		renderTargets[i] = RE::RENDER_TARGET::kNONE;
	}

	auto context = globals::d3d::context;
	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	{
		Util::GpuPhaseScope gpuPhase(Util::GpuScope::CsOther);

		// (batch 37a) Occlusion dry run: max-reduce the finished opaque depth into its own Hi-Z
		// and queue a readback. Writes only its own textures; a no-op unless the test is running.
		Util::OcclusionDryRun::OnEndDeferred();

		DeferredPasses();  // Perform deferred passes and composite forward buffers

		// Measures the lit opaque scene for next frame's shading rates. Our work, so it stays
		// in the CsOther scope; its dispatches are billed to "our timed passes" by GpuPassTimers.
		globals::features::variableRateShading.AnalyzeFrame();
	}

	stateUpdateFlags.set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	deferredPass = false;

	ResetBlendStates();
}

void Deferred::OverrideBlendStates()
{
	auto blendStates = BlendStates::GetSingleton();

	static std::once_flag setup;
	std::call_once(setup, [&]() {
		auto device = globals::d3d::device;

		for (int a = 0; a < 7; a++) {
			for (int b = 0; b < 2; b++) {
				for (int c = 0; c < 13; c++) {
					for (int d = 0; d < 2; d++) {
						forwardBlendStates[a][b][c][d] = blendStates->a[a][b][c][d];

						if (auto blendState = forwardBlendStates[a][b][c][d]) {
							D3D11_BLEND_DESC blendDesc;
							forwardBlendStates[a][b][c][d]->GetDesc(&blendDesc);

							blendDesc.IndependentBlendEnable = true;

							// Default to original blending method
							for (int i = 1; i < 8; i++) {
								blendDesc.RenderTarget[i].BlendEnable = blendDesc.RenderTarget[0].BlendEnable;
								blendDesc.RenderTarget[i].SrcBlend = blendDesc.RenderTarget[0].SrcBlend;
								blendDesc.RenderTarget[i].DestBlend = blendDesc.RenderTarget[0].DestBlend;
								blendDesc.RenderTarget[i].BlendOp = blendDesc.RenderTarget[0].BlendOp;
								blendDesc.RenderTarget[i].SrcBlendAlpha = blendDesc.RenderTarget[0].SrcBlendAlpha;
								blendDesc.RenderTarget[i].DestBlendAlpha = blendDesc.RenderTarget[0].DestBlendAlpha;
								blendDesc.RenderTarget[i].BlendOpAlpha = blendDesc.RenderTarget[0].BlendOpAlpha;
								blendDesc.RenderTarget[i].RenderTargetWriteMask = blendDesc.RenderTarget[0].RenderTargetWriteMask;
							}

							// Normals and motion vectors must use alpha blending
							for (int i = 1; i < 3; i++) {
								blendDesc.RenderTarget[i].BlendEnable = blendDesc.RenderTarget[0].BlendEnable;
								blendDesc.RenderTarget[i].SrcBlend = D3D11_BLEND_SRC_ALPHA;
								blendDesc.RenderTarget[i].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
								blendDesc.RenderTarget[i].BlendOp = D3D11_BLEND_OP_ADD;
								blendDesc.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_SRC_ALPHA;
								blendDesc.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
								blendDesc.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD;
								blendDesc.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
							}

							DX::ThrowIfFailed(device->CreateBlendState(&blendDesc, &deferredBlendStates[a][b][c][d]));
						} else {
							deferredBlendStates[a][b][c][d] = nullptr;
						}
					}
				}
			}
		}
	});

	// Set modified blend states
	for (int a = 0; a < 7; a++) {
		for (int b = 0; b < 2; b++) {
			for (int c = 0; c < 13; c++) {
				for (int d = 0; d < 2; d++) {
					blendStates->a[a][b][c][d] = deferredBlendStates[a][b][c][d];
				}
			}
		}
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

void Deferred::ResetBlendStates()
{
	auto blendStates = BlendStates::GetSingleton();

	// Restore modified blend states
	for (int a = 0; a < 7; a++) {
		for (int b = 0; b < 2; b++) {
			for (int c = 0; c < 13; c++) {
				for (int d = 0; d < 2; d++) {
					blendStates->a[a][b][c][d] = forwardBlendStates[a][b][c][d];
				}
			}
		}
	}

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_ALPHA_BLEND);
}

void Deferred::ClearShaderCache()
{
	if (mainCompositeCS) {
		mainCompositeCS->Release();
		mainCompositeCS = nullptr;
	}
	if (mainCompositeInteriorCS) {
		mainCompositeInteriorCS->Release();
		mainCompositeInteriorCS = nullptr;
	}
}

ID3D11ComputeShader* Deferred::GetComputeMainComposite()
{
	if (!mainCompositeCS) {
		logger::debug("Compiling DeferredCompositeCS");

		std::vector<std::pair<const char*, const char*>> defines;

		if (globals::features::dynamicCubemaps.loaded)
			defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });

		if (globals::features::skylighting.loaded)
			defines.push_back({ "SKYLIGHTING", nullptr });

		if (globals::features::screenSpaceGI.loaded)
			defines.push_back({ "SSGI", nullptr });
		if (globals::features::screenSpaceGI.loaded && compositeAoSparesDirect)
			defines.push_back({ "SSGI_AO_SPARES_DIRECT", nullptr });

		if (globals::features::ibl.loaded)
			defines.push_back({ "IBL", nullptr });

		if (globals::features::screenSpaceRayTracing.loaded)
			defines.push_back({ "SSRT", nullptr });

		if (globals::features::physicalSky.loaded)
			defines.push_back({ "PHYSICAL_SKY", nullptr });

		if (REL::Module::IsVR())
			defines.push_back({ "FRAMEBUFFER", nullptr });

		mainCompositeCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", defines, "cs_5_0"));
	}
	return mainCompositeCS;
}

ID3D11ComputeShader* Deferred::GetComputeMainCompositeInterior()
{
	if (!mainCompositeInteriorCS) {
		logger::debug("Compiling DeferredCompositeCS INTERIOR");

		std::vector<std::pair<const char*, const char*>> defines;
		defines.push_back({ "INTERIOR", nullptr });

		if (globals::features::dynamicCubemaps.loaded)
			defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });

		if (globals::features::screenSpaceGI.loaded)
			defines.push_back({ "SSGI", nullptr });
		if (globals::features::screenSpaceGI.loaded && compositeAoSparesDirect)
			defines.push_back({ "SSGI_AO_SPARES_DIRECT", nullptr });

		if (globals::features::ibl.loaded)
			defines.push_back({ "IBL", nullptr });

		if (globals::features::screenSpaceRayTracing.loaded)
			defines.push_back({ "SSRT", nullptr });

		if (REL::Module::IsVR())
			defines.push_back({ "FRAMEBUFFER", nullptr });

		mainCompositeInteriorCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\DeferredCompositeCS.hlsl", defines, "cs_5_0"));
	}
	return mainCompositeInteriorCS;
}

void Deferred::Hooks::Main_RenderShadowMaps::thunk()
{
	// (batch 36) The engine's shadow map pass; the per-light hooks split it further.
	{
		Util::GpuPhaseScope gpuPhase(Util::GpuScope::ShadowMaps);
		func();
	}
	Util::GpuPhaseScope gpuPhase(Util::GpuScope::CsOther);
	globals::deferred->EarlyPrepasses();
};

void Deferred::Hooks::Main_RenderWorld::thunk(bool a1)
{
	auto* const state = globals::state;
	state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
	state->inWorld = true;
	{
		// (batch 36) Sky, water, transparent and our deferred work are split out from here.
		Util::GpuPhaseScope gpuPhase(Util::GpuScope::World);
		func(a1);
	}
	state->inWorld = false;
	state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
};

void Deferred::Hooks::Main_RenderWorld_Start::thunk(RE::BSBatchRenderer* This, uint32_t StartRange, uint32_t EndRanges, uint32_t RenderFlags, int GeometryGroup)
{
	if (globals::shaderCache->IsEnabled() && globals::state->inWorld) {
		// Here is where the first opaque objects start rendering
		globals::deferred->StartDeferred();
	}

	func(This, StartRange, EndRanges, RenderFlags, GeometryGroup);  // RenderBatches
};

void Deferred::Hooks::Main_RenderWorld_BlendedDecals::thunk(RE::BSShaderAccumulator* This, uint32_t RenderFlags)
{
	auto deferred = globals::deferred;

	if (globals::shaderCache->IsEnabled() && globals::state->inWorld) {
		auto& terrainBlending = globals::features::terrainBlending;
		// Defer terrain rendering until after everything else
		if (terrainBlending.loaded)
			terrainBlending.RenderTerrainBlendingPasses();
	}

	// Deferred blended decals

	func(This, RenderFlags);

	deferred->EndDeferred();

	// Copy depth from before water
	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
	auto depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

	context->CopyResource(depthCopy.texture, depth.texture);

	// After this point, water starts rendering
};

void Deferred::Hooks::BSCubeMapCamera_RenderCubemap::thunk(RE::NiAVObject* camera, int a2, bool a3, bool a4, bool a5)
{
	auto deferred = globals::deferred;
	auto state = globals::state;

	{
		Util::GpuPhaseScope gpuPhase(Util::GpuScope::CsOther);
		deferred->ReflectionsPrepasses();
	}
	state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
	{
		Util::GpuPhaseScope gpuPhase(Util::GpuScope::Reflections);
		func(camera, a2, a3, a4, a5);
	}
	state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::IsReflections);
}

void Deferred::Hooks::Main_RenderFirstPersonView::thunk(bool a1, bool a2)
{
	auto* const state = globals::state;
	state->permutationData.ExtraShaderDescriptor |= static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
	{
		Util::GpuPhaseScope gpuPhase(Util::GpuScope::FirstPerson);
		func(a1, a2);
	}
	state->permutationData.ExtraShaderDescriptor &= ~static_cast<uint32_t>(State::ExtraShaderDescriptors::InWorld);
}

void Deferred::Hooks::Renderer_ResetState::thunk(void* This)
{
	func(This);

	auto* const state = globals::state;
	auto* const context = globals::d3d::context;

	ID3D11Buffer* buffers[3] = { state->permutationCB->CB(), state->sharedDataCB->CB(), state->featureDataCB->CB() };
	context->PSSetConstantBuffers(4, 3, buffers);
	context->CSSetConstantBuffers(5, 2, buffers + 1);

	auto* singleton = globals::truePBR;
	singleton->SetupFrame();
}
