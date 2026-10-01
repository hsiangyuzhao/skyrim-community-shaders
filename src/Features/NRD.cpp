#include "NRD.h"

#include "Deferred.h"
#include "Menu.h"
#include "ScreenSpaceRayTracing.h"
#include "State.h"
#include "Upscaling.h"
#include "Utils/D3D.h"
#include "Utils/GpuTimers.h"

namespace
{
	// (S2.6) Whether anything is going to read this frame's guides.
	//
	// PrepareGuides is one compute dispatch writing viewZ and packed normal+roughness plus a
	// full-resource copy of the motion-vector target: ~16 bytes per pixel of read+write
	// traffic, ~190 MB/frame at a 4K allocation. It ran on every frame the NRD feature was
	// loaded and enabled, whether or not any consumer had selected REBLUR -- which, with SVGF
	// or Off selected, is nobody.
	//
	// Screen Space Ray Tracing's two chains are the entire consumer set in this fork. The
	// question asked is the *effective* denoiser, not the requested one, and it is safe to ask
	// here because SSRT resolves it in Prepass and every feature's Prepass runs ahead of the
	// deferred passes that call PrepareGuides.
	bool AnyConsumerNeedsGuides()
	{
		auto& ssrt = globals::features::screenSpaceRayTracing;
		if (!ssrt.loaded)
			return false;

		const bool diffuse = ssrt.settings.EnableDiffuse &&
		                     ssrt.EffectiveDenoiser(false) == ScreenSpaceRayTracing::kDenoiserREBLUR;
		const bool specular = ssrt.settings.EnableSpecular &&
		                      ssrt.EffectiveDenoiser(true) == ScreenSpaceRayTracing::kDenoiserREBLUR;
		return diffuse || specular;
	}
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NRD::Settings,
	Enabled)

////////////////////////////////////////////////////////////////////////////////////

void NRD::RestoreDefaultSettings()
{
	settings = {};
}

void NRD::DrawSettings()
{
	ImGui::TextWrapped(
		"NVIDIA's REBLUR denoiser for Screen Space Ray Tracing. Whether it runs "
		"is chosen there, with the Denoiser setting; its tuning options are there too.");

	ImGui::Separator();
	ImGui::Checkbox("Enabled", &settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Off = REBLUR is unavailable, and Screen Space Ray Tracing switches "
			"to its other denoiser (SVGF) instead.");
	}

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		// (batch 11, item C2) The three entries appear once the guides exist, i.e. once a consumer
		// has selected REBLUR. Before that they are genuinely absent rather than showing
		// uninitialised video memory, which is what they used to do: the textures were allocated
		// unconditionally in SetupResources and nothing ever cleared them, so with SVGF or Off
		// selected these panels displayed whatever the driver handed out.
		if (texNRDViewZ)
			BUFFER_VIEWER_NODE(texNRDViewZ, debugRescale)
		if (texNRDNormalRoughness)
			BUFFER_VIEWER_NODE(texNRDNormalRoughness, debugRescale)
		if (texNRDMV)
			BUFFER_VIEWER_NODE(texNRDMV, debugRescale)
		if (!texNRDViewZ && !texNRDNormalRoughness && !texNRDMV)
			ImGui::TextDisabled("Nothing to show until REBLUR is selected as the denoiser in Screen Space Ray Tracing.");

		ImGui::TreePop();
	}
}

void NRD::LoadSettings(json& o_json)
{
	settings = o_json;
}

void NRD::SaveSettings(json& o_json)
{
	o_json = settings;
}

void NRD::SetupResources()
{
	commonSettingsValidThisFrame = false;
	guidesReadyThisFrame = false;
	hasCommonFrameHistory = false;
	lastCommonGameFrame = 0;
	prevResourceSize[0] = prevResourceSize[1] = 0;
	prevRectSize[0] = prevRectSize[1] = 0;

	// (batch 11, item C2) The three guide textures are allocated on first need, not here.
	//
	// SetupResources also runs on a resolution change (BSShaderRenderTargets_Create re-runs
	// State::Setup), and these are sized from kMAIN, so anything left resident would keep the old
	// extent forever. Dropping them is what makes the next frame that needs guides rebuild them
	// at the current size -- the same pattern ScreenSpaceRayTracing uses for its own lazily built
	// REBLUR surfaces.
	texNRDViewZ = nullptr;
	texNRDNormalRoughness = nullptr;
	texNRDMV = nullptr;

	CompileComputeShaders();
}

// (batch 11, item C2) Bring the three guides up on the first frame that is actually going to
// write them.
//
// They used to be created unconditionally in SetupResources: 12 bytes per output pixel between
// them -- R32 viewZ, R10G10B10A2 normal+roughness, R16G16 motion vectors -- i.e. ~25 MB at 1080p
// and ~100 MB at a 4K allocation, held for the whole session by every user with the NRD feature
// installed. That included users on SVGF or Off, for whom nothing reads any of the three, and
// users with `Enabled` unticked on this very page.
//
// This is the only allocation in this batch that a user who never opens the menu still gets back,
// which is why it is worth doing even though the dispatch itself was already correctly gated by
// AnyConsumerNeedsGuides.
//
// SetupResources' clear-on-allocate problem goes away with it. Nothing zeroed these three, so
// with SVGF or Off selected the Buffer Viewer entries below showed uninitialised video memory
// rather than an honest "not in use". Now the entries are simply absent -- they are guarded by
// `if (tex)` and the pointers are null -- which is the accurate presentation.
//
// Returns false if allocation was not attempted or did not produce all three, which PrepareGuides
// treats exactly like a failed shader compile: guidesReadyThisFrame stays false and every consumer
// falls back to its own denoiser, which is a path that already exists and is already tested.
bool NRD::EnsureGuides(bool a_needMotionCopy)
{
	// (batch 36) With the direct motion-vector path nothing reads texNRDMV, so it is dropped
	// rather than held: 4 bytes per pixel of the allocation, 32 MiB at 4K. Switching back
	// re-creates it on the next frame through the normal path below.
	if (!a_needMotionCopy)
		texNRDMV = nullptr;

	if (texNRDViewZ && texNRDNormalRoughness && (texNRDMV || !a_needMotionCopy))
		return true;

	logger::debug("Creating NRD guide textures...");

	auto renderer = globals::game::renderer;
	auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC mainDesc;
	mainTex.texture->GetDesc(&mainDesc);

	D3D11_TEXTURE2D_DESC texDesc{
		.Width = mainDesc.Width,
		.Height = mainDesc.Height,
		.MipLevels = 1,
		.ArraySize = 1,
		.Format = DXGI_FORMAT_R16G16_FLOAT,
		.SampleDesc = { 1, 0 },
		.Usage = D3D11_USAGE_DEFAULT,
		.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
	};
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

	if (a_needMotionCopy && !texNRDMV) {
		texNRDMV = eastl::make_unique<Texture2D>(texDesc);
		texNRDMV->CreateSRV(srvDesc);
		texNRDMV->CreateUAV(uavDesc);
		Util::SetResourceName(texNRDMV->resource.get(), "NRD::MV");
	}

	srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	if (!texNRDViewZ) {
		texNRDViewZ = eastl::make_unique<Texture2D>(texDesc);
		texNRDViewZ->CreateSRV(srvDesc);
		texNRDViewZ->CreateUAV(uavDesc);
		Util::SetResourceName(texNRDViewZ->resource.get(), "NRD::ViewZ");
	}

	srvDesc.Format = uavDesc.Format = texDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	if (!texNRDNormalRoughness) {
		texNRDNormalRoughness = eastl::make_unique<Texture2D>(texDesc);
		texNRDNormalRoughness->CreateSRV(srvDesc);
		texNRDNormalRoughness->CreateUAV(uavDesc);
		Util::SetResourceName(texNRDNormalRoughness->resource.get(), "NRD::NormalRoughness");
	}

	return texNRDViewZ && texNRDNormalRoughness && (texNRDMV || !a_needMotionCopy);
}

void NRD::ClearShaderCache()
{
	prepareNRDGuidesCompute = nullptr;
	CompileComputeShaders();
}

void NRD::CompileComputeShaders()
{
	std::vector<std::pair<const char*, const char*>> defines;

	auto path = std::filesystem::path("Data\\Shaders\\NRD") / "prepareNRDGuides.cs.hlsl";
	if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), defines, "cs_5_0")))
		prepareNRDGuidesCompute.attach(rawPtr);
}

void NRD::PrepareGuides()
{
	commonSettingsValidThisFrame = false;
	guidesReadyThisFrame = false;
	directMotionVectorsThisFrame = false;
	directMotionVectorSRV = nullptr;
	directMotionVectorUAV = nullptr;

	if (!settings.Enabled || !prepareNRDGuidesCompute)
		return;

	// (S2.6) No REBLUR consumer this frame means no reader for any of the three guides.
	// Leaving guidesReadyThisFrame false is exactly right: a consumer that changes its mind
	// mid-frame finds the guides absent and keeps its own fallback, which is the same answer
	// it would get from a genuine failure.
	if (!AnyConsumerNeedsGuides())
		return;

	// (batch 11, item C2) ...and this is where the guides come into existence, on the far side of
	// that gate. The ordering is the whole point: with the surfaces allocated in SetupResources,
	// a user on SVGF or Off -- or with this feature's own Enabled unticked -- still paid ~100 MB
	// of a 4K allocation for three textures nothing read. A failure here is treated exactly like
	// a failed shader compile above: guidesReadyThisFrame stays false and every consumer keeps
	// its own fallback denoiser.
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;
	auto rts = renderer->GetRuntimeData().renderTargets;
	auto state = globals::state;

	// (batch 36) Direct motion vectors: bind the game's target instead of a snapshot of it. Only
	// when the target actually carries both views -- CS's ModifyRenderTarget hook adds the UAV to
	// it, and anything that ever stops doing that makes this fall back to the copy rather than
	// hand REBLUR a null storage binding (which NRDReblurIntegration::Dispatch would abort on).
	// See Settings::ReblurDirectMotionVectors for why the two are interchangeable.
	{
		const auto& motionRT = rts[RE::RENDER_TARGETS::kMOTION_VECTOR];
		directMotionVectorsThisFrame =
			globals::features::screenSpaceRayTracing.settings.ReblurDirectMotionVectors &&
			motionRT.SRV && motionRT.UAV;
		if (directMotionVectorsThisFrame) {
			directMotionVectorSRV = motionRT.SRV;
			directMotionVectorUAV = motionRT.UAV;
		}
	}

	if (!EnsureGuides(!directMotionVectorsThisFrame)) {
		directMotionVectorsThisFrame = false;
		directMotionVectorSRV = nullptr;
		directMotionVectorUAV = nullptr;
		return;
	}

	state->BeginPerfEvent("NRD - Prepare Guides");
	// (batch 11, item B1) This pass had a PIX marker but no GPU timing row, which made it the one
	// stretch of the REBLUR path the overlay could not price: one full-screen dispatch writing
	// viewZ and packed normal+roughness, plus a full-resource copy of the motion-vector target.
	// Purely instrumentation -- it changes no dispatch and no binding.
	Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::NRDGuides);

	auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
	auto normal = rts[NORMALROUGHNESS];
	auto motion = rts[RE::RENDER_TARGETS::kMOTION_VECTOR];

	float2 dynres = Util::ConvertToDynamic(state->screenSize);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto* sharedDataBuf = state->sharedDataCB->CB();
	context->CSSetConstantBuffers(5, 1, &sharedDataBuf);

	std::array<ID3D11ShaderResourceView*, 2> guideSRVs = {
		depth.depthSRV,
		normal.SRV
	};
	std::array<ID3D11UnorderedAccessView*, 2> guideUAVs = {
		texNRDViewZ->uav.get(),
		texNRDNormalRoughness->uav.get()
	};

	context->CSSetShaderResources(0, (uint)guideSRVs.size(), guideSRVs.data());
	context->CSSetUnorderedAccessViews(0, (uint)guideUAVs.size(), guideUAVs.data(), nullptr);
	context->CSSetShader(prepareNRDGuidesCompute.get(), nullptr, 0);
	context->Dispatch(((uint)dynres.x + 7) / 8, ((uint)dynres.y + 7) / 8, 1);

	std::array<ID3D11ShaderResourceView*, 2> nullSRVs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> nullUAVs = { nullptr };
	context->CSSetShaderResources(0, (uint)nullSRVs.size(), nullSRVs.data());
	context->CSSetUnorderedAccessViews(0, (uint)nullUAVs.size(), nullUAVs.data(), nullptr);
	context->CSSetShader(nullptr, nullptr, 0);

	// Motion Vector is used as both SRV and UAV by ReBLUR; snapshot the game's
	// MV target so we can rebind it through NRD's UAV slot without aliasing.
	// (batch 36) ...unless the direct path is on, in which case no pass binds the target as SRV
	// and UAV at once anyway (each REBLUR dispatch binds it one way only), and the snapshot is
	// skipped. Deferred's own composite already binds this same target's UAV every frame.
	if (!directMotionVectorsThisFrame)
		context->CopyResource(texNRDMV->resource.get(), motion.texture);

	Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::NRDGuides);
	state->EndPerfEvent();

	guidesReadyThisFrame = true;
}

const nrd::CommonSettings& NRD::GetCommonSettings()
{
	if (commonSettingsValidThisFrame)
		return commonSettings;

	float2 screenSize = globals::state->screenSize;
	float2 dynres = Util::ConvertToDynamic(screenSize);
	dynres = { floor(dynres.x), floor(dynres.y) };

	auto fullW = texNRDViewZ ? texNRDViewZ->desc.Width : (uint32_t)screenSize.x;
	auto fullH = texNRDViewZ ? texNRDViewZ->desc.Height : (uint32_t)screenSize.y;

	commonSettings = nrd::CommonSettings{};
	const uint16_t resourceSize[2] = { (uint16_t)fullW, (uint16_t)fullH };
	const uint16_t rectSize[2] = { (uint16_t)dynres.x, (uint16_t)dynres.y };
	const bool resourceSizeChanged = hasCommonFrameHistory &&
	                                 (resourceSize[0] != prevResourceSize[0] || resourceSize[1] != prevResourceSize[1]);

	commonSettings.resourceSize[0] = resourceSize[0];
	commonSettings.resourceSize[1] = resourceSize[1];
	commonSettings.resourceSizePrev[0] = hasCommonFrameHistory ? prevResourceSize[0] : resourceSize[0];
	commonSettings.resourceSizePrev[1] = hasCommonFrameHistory ? prevResourceSize[1] : resourceSize[1];
	commonSettings.rectSize[0] = rectSize[0];
	commonSettings.rectSize[1] = rectSize[1];
	commonSettings.rectSizePrev[0] = hasCommonFrameHistory ? prevRectSize[0] : rectSize[0];
	commonSettings.rectSizePrev[1] = hasCommonFrameHistory ? prevRectSize[1] : rectSize[1];

	auto viewMat = globals::game::frameBufferCached.GetCameraView().Transpose();
	auto projMat = globals::game::frameBufferCached.GetCameraProj().Transpose();

	float3 cameraWorldPos = float3(globals::game::frameBufferCached.GetCameraPosAdjust());
	DirectX::XMMATRIX translationMat = DirectX::XMMatrixTranslation(-cameraWorldPos.x, -cameraWorldPos.y, -cameraWorldPos.z);
	worldToViewMat = DirectX::XMMatrixMultiply(translationMat, viewMat);

	memcpy(commonSettings.viewToClipMatrix, &projMat, sizeof(float) * 16);
	memcpy(commonSettings.viewToClipMatrixPrev, &prevProjMatrix, sizeof(float) * 16);
	memcpy(commonSettings.worldToViewMatrix, &worldToViewMat, sizeof(float) * 16);
	memcpy(commonSettings.worldToViewMatrixPrev, &prevWorldToViewMat, sizeof(float) * 16);

	commonSettings.motionVectorScale[0] = 1.0f;
	commonSettings.motionVectorScale[1] = 1.0f;
	commonSettings.motionVectorScale[2] = 0.0f;
	commonSettings.isMotionVectorInWorldSpace = false;

	auto jitter = globals::features::upscaling.jitter;
	commonSettings.cameraJitter[0] = jitter.x;
	commonSettings.cameraJitter[1] = jitter.y;
	commonSettings.cameraJitterPrev[0] = prevJitter.x;
	commonSettings.cameraJitterPrev[1] = prevJitter.y;

	const uint32_t gameFrame = globals::state->frameCount;
	commonSettings.frameIndex = gameFrame;
	commonSettings.denoisingRange = 1e6f;

	// A changing rect is normal dynamic-resolution operation; NRD consumes both
	// rectSize and rectSizePrev and should retain history across that transition.
	if (!hasCommonFrameHistory || gameFrame != lastCommonGameFrame + 1 || resourceSizeChanged)
		commonSettings.accumulationMode = nrd::AccumulationMode::CLEAR_AND_RESTART;
	lastCommonGameFrame = gameFrame;
	hasCommonFrameHistory = true;

	prevWorldToViewMat = worldToViewMat;
	prevProjMatrix = projMat;
	prevJitter = jitter;
	prevResourceSize[0] = resourceSize[0];
	prevResourceSize[1] = resourceSize[1];
	prevRectSize[0] = rectSize[0];
	prevRectSize[1] = rectSize[1];

	commonSettingsValidThisFrame = true;
	return commonSettings;
}

void NRD::ApplyReblurSettings(nrd::ReblurSettings& out, const REBLURSettings& in, nrd::CheckerboardMode checkerboard) const
{
	out.maxAccumulatedFrameNum = std::min((uint32_t)in.MaxAccumulatedFrameNum, nrd::REBLUR_MAX_HISTORY_FRAME_NUM);
	out.maxFastAccumulatedFrameNum = std::min((uint32_t)in.MaxFastAccumulatedFrameNum, out.maxAccumulatedFrameNum);
	out.maxStabilizedFrameNum = std::min((uint32_t)in.MaxStabilizedFrameNum, out.maxAccumulatedFrameNum);
	out.historyFixFrameNum = out.maxFastAccumulatedFrameNum > 0 ? std::min((uint32_t)in.HistoryFixFrameNum, out.maxFastAccumulatedFrameNum - 1) : 0;
	out.historyFixBasePixelStride = std::max(in.HistoryFixBasePixelStride, 1u);
	out.historyFixAlternatePixelStride = std::max(in.HistoryFixAlternatePixelStride, 1u);
	out.fastHistoryClampingSigmaScale = std::clamp(in.FastHistoryClampingSigmaScale, 1.0f, 3.0f);
	out.diffusePrepassBlurRadius = 0.0f;
	out.minHitDistanceWeight = std::clamp(in.MinHitDistanceWeight, 0.0001f, 0.2f);
	out.minBlurRadius = std::max(in.MinBlurRadius, 0.0f);
	out.maxBlurRadius = std::max(in.MaxBlurRadius, out.minBlurRadius);
	out.lobeAngleFraction = std::clamp(in.LobeAngleFraction, 0.0f, 1.0f);
	out.roughnessFraction = std::clamp(in.RoughnessFraction, 0.0f, 1.0f);
	out.planeDistanceSensitivity = std::max(in.PlaneDistanceSensitivity, 0.0f);
	out.enableAntiFirefly = true;
	out.hitDistanceReconstructionMode = static_cast<nrd::HitDistanceReconstructionMode>(std::min(in.HitDistanceReconstructionMode, 2u));
	out.checkerboardMode = checkerboard;
	out.returnHistoryLengthInsteadOfOcclusion = in.ReturnHistoryLength;

	// (batch C1) Antilag off for the first integration, per NRD's own bring-up
	// guidance. ComputeAntilag in REBLUR_Common.hlsli evaluates
	//   d = (|h - a| - sigma * luminanceSigmaScale) / (max(h, a) + ...)
	// and a huge sigma scale keeps d negative, which LinearStep resolves to 1.0 —
	// i.e. the history is never shortened. luminanceSensitivity only moves the
	// LinearStep edge, raised alongside for the same direction of effect.
	out.antilagSettings.luminanceSigmaScale = 1e3f;
	out.antilagSettings.luminanceSensitivity = 1e3f;
}

bool NRD::DrawReblurSettings(REBLURSettings& s, bool showAdvanced, const char* tag)
{
	bool changed = false;
	ImGui::PushID(tag);

	if (showAdvanced) {
		ImGui::SeparatorText("Accumulation");
		{
			int v = (int)s.MaxAccumulatedFrameNum;
			if (ImGui::SliderInt("Max Accumulated Frames", &v, 1, (int)nrd::REBLUR_MAX_HISTORY_FRAME_NUM)) {
				s.MaxAccumulatedFrameNum = (uint32_t)v;
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How many past frames are averaged together. Higher = cleaner and steadier, but lighting reacts more slowly and moving things can smear.");

			v = (int)s.MaxFastAccumulatedFrameNum;
			if (ImGui::SliderInt("Max Fast Accumulated Frames", &v, 1, (int)s.MaxAccumulatedFrameNum)) {
				s.MaxFastAccumulatedFrameNum = (uint32_t)v;
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("A short backup history that catches quick lighting changes. Lower = reacts faster but noisier; setting it equal to the slider above turns it off.");

			v = (int)s.MaxStabilizedFrameNum;
			if (ImGui::SliderInt("Max Stabilized Frames", &v, 0, (int)s.MaxAccumulatedFrameNum)) {
				s.MaxStabilizedFrameNum = (uint32_t)v;
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Extra smoothing over time on the final result to reduce shimmer. Higher = steadier but laggier; 0 = off.");
		}

		ImGui::SeparatorText("Spatial Filter");
		{
			changed |= ImGui::SliderFloat("Min Blur Radius", &s.MinBlurRadius, 0.0f, 10.0f, "%.1f px");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Blur size once the image has settled. Higher = smoother but softer.");
			changed |= ImGui::SliderFloat("Max Blur Radius", &s.MaxBlurRadius, 0.0f, 60.0f, "%.1f px");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Blur size right after a change; it shrinks as the image settles. Higher = less grain in newly revealed areas, but blurrier.");
			changed |= ImGui::SliderFloat("Lobe Angle Fraction", &s.LobeAngleFraction, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How freely the blur mixes surfaces facing different directions. Higher = smoother; lower = bumps and surface detail stay crisper, but noisier.");
			changed |= ImGui::SliderFloat("Roughness Fraction", &s.RoughnessFraction, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How freely the blur mixes shiny and rough surfaces. Higher = smoother; lower = sharper material edges. Mainly matters for reflections.");
			changed |= ImGui::SliderFloat("Plane Distance Sensitivity", &s.PlaneDistanceSensitivity, 0.0f, 0.1f, "%.4f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How strictly the blur stops at depth edges between objects. Lower = sharper edges; higher = smoother, but can bleed across edges.");
		}

		ImGui::SeparatorText("Quality");
		{
			changed |= ImGui::SliderFloat("Fast History Clamping Sigma", &s.FastHistoryClampingSigmaScale, 1.0f, 3.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How far old frames may drift from the current lighting before being pulled back. Lower = less ghosting and lag; higher = smoother.");
			changed |= ImGui::SliderFloat("Min Hit Distance Weight", &s.MinHitDistanceWeight, 0.0001f, 0.2f, "%.4f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Higher = smoother, but small contact shadows and creases get blurred away. Lower keeps them, but noisier.");

			int v = (int)s.HistoryFixFrameNum;
			if (ImGui::SliderInt("History Fix Frame Num", &v, 0, 4)) {
				s.HistoryFixFrameNum = (uint32_t)v;
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("For how many frames newly revealed areas (e.g. behind a moving object) get extra cleanup. Higher = less grain there, but softer.");

			v = (int)s.HistoryFixBasePixelStride;
			if (ImGui::SliderInt("History Fix Pixel Stride", &v, 1, 20)) {
				s.HistoryFixBasePixelStride = (uint32_t)v;
				changed = true;
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("How wide that extra cleanup reaches. Higher = smoother newly revealed areas, but blurrier.");
		}

		ImGui::SeparatorText("Debug");
		{
			changed |= ImGui::SliderFloat("Split Screen", &s.SplitScreen, 0.0f, 1.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Shows the raw, un-denoised image on the left part of the screen for comparison. 0 = off.");
			changed |= ImGui::Checkbox("NRD Validation Overlay", &s.EnableValidation);
			changed |= ImGui::Checkbox("Output History Length", &s.ReturnHistoryLength);

			static const char* hitDistReconModes[] = { "OFF", "AREA_3X3", "AREA_5X5" };
			int hdMode = (int)s.HitDistanceReconstructionMode;
			if (ImGui::Combo("Hit Distance Reconstruction", &hdMode, hitDistReconModes, 3)) {
				s.HitDistanceReconstructionMode = (uint32_t)hdMode;
				changed = true;
			}
		}
	}

	ImGui::PopID();
	return changed;
}
