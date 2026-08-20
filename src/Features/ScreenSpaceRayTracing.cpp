#include "ScreenSpaceRayTracing.h"

#include <DDSTextureLoader.h>

#include "Deferred.h"
#include "JiayeStatement.h"
#include "Menu.h"
#include "State.h"
#include "ShaderCache.h"

#include "DynamicCubemaps.h"
#include "ScreenSpaceGI.h"
#include "Skylighting.h"

#ifdef ENABLE_SHARC
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ScreenSpaceRayTracing::Settings,
    EnableSpecular,
    MaxSteps,
    MaxMips,
    Thickness,
    NormalBias,
    BRDFBias,
    UseDynamicCubemapsAsFallback,
    UseDynamicCubemapsAsFallbackSpecular,
    DiffuseSPP,
    EnableDiffuse,
    SpecularMult,
    DiffuseMult,
    AmbientMult,
    OcclusionStrength,
    CubemapNormalization,
    EnableSVGF,
    MaxAccumulatedFrames,
    AtrousIterations,
    ColorPhi,
    NormalPhi,
    AdaptiveFiltering,
    AdaptiveHistoryThreshold,
    AdaptiveVarianceEps,
    FireflyClamp,
    FireflyClampSigma,
    EnableSharc
)
#else
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ScreenSpaceRayTracing::Settings,
    EnableSpecular,
    MaxSteps,
    MaxMips,
    Thickness,
    NormalBias,
    BRDFBias,
    UseDynamicCubemapsAsFallback,
    UseDynamicCubemapsAsFallbackSpecular,
    DiffuseSPP,
    EnableDiffuse,
    SpecularMult,
    DiffuseMult,
    AmbientMult,
    OcclusionStrength,
    CubemapNormalization,
    EnableSVGF,
    MaxAccumulatedFrames,
    AtrousIterations,
    ColorPhi,
    NormalPhi,
    AdaptiveFiltering,
    AdaptiveHistoryThreshold,
    AdaptiveVarianceEps,
    FireflyClamp,
    FireflyClampSigma
)
#endif

void ScreenSpaceRayTracing::DrawSettings()
{
    ImGui::Checkbox("Enable Specular", &settings.EnableSpecular);
    ImGui::SameLine();
    ImGui::Checkbox("Enable Diffuse", &settings.EnableDiffuse);
    ImGui::SliderInt("Max Steps", (int*)&settings.MaxSteps, 1, 256);
    // (audit P3) The traversal can load exactly mip SSRTCB::MaxMips, so the highest
    // legal setting is maxMips - 1; the old bound of maxMips let the ray sample a mip
    // that does not exist, and an out-of-range Load returns 0 == near plane, i.e. an
    // instant false hit.
    ImGui::SliderInt("Max Mip Level", (int*)&settings.MaxMips, 1, maxMips - 1, "%d", ImGuiSliderFlags_AlwaysClamp);
    recompileFlag |= ImGui::SliderInt("Diffuse SPP", (int*)&settings.DiffuseSPP, 1, 16, "%d", ImGuiSliderFlags_AlwaysClamp);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Samples per pixel for diffuse component. Higher values reduce noise but impact performance.");
    ImGui::SliderFloat("Specular Multiplier", &settings.SpecularMult, 0.0f, 5.0f, "%.2f");
    ImGui::SliderFloat("Diffuse Multiplier", &settings.DiffuseMult, 0.01f, 5.0f, "%.2f");
    ImGui::SliderFloat("Occlusion Strength", &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f");
    ImGui::SliderFloat("Ambient Multiplier", &settings.AmbientMult, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Mix diffuse with vanilla ambient color. Not suggested if using dynamic cubemaps as fallback.");

    ImGui::Separator();

    ImGui::SliderFloat("Thickness", &settings.Thickness, 0.0f, 50.0f, "%.2f");
    ImGui::SliderFloat("Normal Bias", &settings.NormalBias, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("To avoid false hits from nearby geometry, increase this value to push the ray origin along the normal.");
    ImGui::SliderFloat("BRDF Bias", &settings.BRDFBias, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Specular only. Higher BRDF bias reduces noise but makes reflections more glossy.");
    ImGui::Checkbox("Use Dynamic Cubemaps as Fallback for Diffuse", &settings.UseDynamicCubemapsAsFallback);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("When ray marching misses, use dynamic cubemaps for reflections.");
    ImGui::Checkbox("Use Dynamic Cubemaps as Fallback for Specular", &settings.UseDynamicCubemapsAsFallbackSpecular);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("When ray marching misses, use dynamic cubemaps for reflections. Recommended for specular.");
    ImGui::SliderFloat("Cubemap Normalization", &settings.CubemapNormalization, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Matches cubemap luminance with ambient color.");

    ImGui::Separator();

    ImGui::Checkbox("Enable Spatiotemporal Variance-Guided Filtering", &settings.EnableSVGF);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("SVGF denoiser. This may introduce some blurriness and temporal artifacts but significantly reduces noise.");
    if (settings.EnableSVGF) {
        ImGui::SliderInt("Max Accumulated Frames", (int*)&settings.MaxAccumulatedFrames, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
        ImGui::SliderInt("À Trous Iterations", (int*)&settings.AtrousIterations, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Number of À Trous wavelet filter iterations. More iterations yield smoother results but may blur details and have a higher computational cost.");
        ImGui::SliderFloat("Color Phi", &settings.ColorPhi, 0.01f, 32.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Controls sensitivity to color differences in the À Trous filter. Lower values preserve more detail but may retain noise.");
        ImGui::SliderFloat("Normal Phi", &settings.NormalPhi, 1.0f, 1024.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Controls sensitivity to normal differences in the À Trous filter. Higher values preserve more detail but may retain noise.");

        ImGui::Checkbox("Firefly Clamp", &settings.FireflyClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Clamps single-pixel radiance outliers against their 3x3 neighbourhood "
                "before the temporal accumulation sees them. Fireflies are the one artefact "
                "the A Trous filter makes worse rather than better -- it spreads them into "
                "slowly fading blobs -- so this is what keeps a low iteration count safe. "
                "Turn off for a bit-exact classic SVGF temporal pass.");
        if (settings.FireflyClamp) {
            ImGui::SliderFloat("Firefly Clamp Sigma", &settings.FireflyClampSigma, 1.0f, 8.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "Standard deviations above the neighbourhood mean a pixel may reach before "
                    "it counts as a firefly. Below 2.65 the clamp starts reaching values a "
                    "neighbour also produced, i.e. real signal; higher values only catch the "
                    "most extreme spikes.");
        }

        ImGui::Checkbox("Adaptive Filtering", &settings.AdaptiveFiltering);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Lets an 8x8 tile whose pixels have all converged skip an À Trous iteration, "
                "spending the filter only where the temporal accumulation is still noisy "
                "(motion, disocclusion). Turn off for a bit-exact classic SVGF.");
        if (settings.AdaptiveFiltering) {
            ImGui::SliderInt("Adaptive History Threshold", (int*)&settings.AdaptiveHistoryThreshold, 4, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("Accumulated frames a pixel needs before it may count as converged. Matching Max Accumulated Frames is a good default.");
            ImGui::SliderFloat("Adaptive Variance Threshold", &settings.AdaptiveVarianceEps, 1e-6f, 1e-2f, "%.6f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("Luminance variance below which a pixel counts as converged. Higher values skip more tiles at the cost of residual noise.");
        }
    }
#ifdef ENABLE_SHARC
    ImGui::Checkbox("(Broken) Enable SHARC", &settings.EnableSharc);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("(Experimental) Enables Spatially Hashed Radiance Cache (SHARC) to improve diffuse quality. This requires more memory and might impact performance.");
#endif
    ImGui::SeparatorText("Debug");

	if (ImGui::TreeNode("Buffer Viewer")) {
		static float debugRescale = .3f;
		ImGui::SliderFloat("View Resize", &debugRescale, 0.f, 1.f);

		BUFFER_VIEWER_NODE(texDepth, debugRescale)
        BUFFER_VIEWER_NODE(texColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRColor, debugRescale)
        BUFFER_VIEWER_NODE(texSSRTDiffuseColor, debugRescale)
        BUFFER_VIEWER_NODE(texHistory, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryDiffuse, debugRescale)
        BUFFER_VIEWER_NODE(texTemporal, debugRescale)
        BUFFER_VIEWER_NODE(texMoments, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryMoments, debugRescale)
        BUFFER_VIEWER_NODE(texHistoryMomentsDiffuse, debugRescale)
        BUFFER_VIEWER_NODE(texVariance, debugRescale)

		ImGui::TreePop();
	}

    JiayeStatement::GetSingleton()->DrawJSInfo();
}

void ScreenSpaceRayTracing::RestoreDefaultSettings()
{
    settings = {};
}

void ScreenSpaceRayTracing::LoadSettings(json& o_json)
{
    settings = o_json;
}

void ScreenSpaceRayTracing::SaveSettings(json& o_json)
{
    o_json = settings;
}

void ScreenSpaceRayTracing::SetupResources()
{
    auto renderer = globals::game::renderer;
	auto device = globals::d3d::device;

	logger::debug("Creating buffers...");
	{
        ssrtCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<SSRTCB>());
        denoiserCB = eastl::make_unique<ConstantBuffer>(ConstantBufferDesc<DenoiserCB>());
    }

    logger::debug("Creating textures...");
    {
        auto mainTex = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
        D3D11_TEXTURE2D_DESC texDesc = {};
        mainTex.texture->GetDesc(&texDesc);
        // (audit P9) None of these textures is ever bound as a render target or passed
        // to GenerateMips -- the only GenerateMips call in this feature is commented out
        // in Prepass, and no RTV is ever created. D3D11_RESOURCE_MISC_GENERATE_MIPS also
        // *requires* BIND_RENDER_TARGET, so the two go together; dropping both lets the
        // driver pick a layout without RT compression metadata for ~14 full-screen
        // surfaces (plus the whole depth pyramid).
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        // Explicitly 0 rather than leaving whatever kMAIN carried: MiscFlags used to be
        // OR-ed into, and any inherited GENERATE_MIPS would now fail creation because it
        // requires BIND_RENDER_TARGET.
        texDesc.MiscFlags = 0;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

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

        texColor = eastl::make_unique<Texture2D>(texDesc);
        texColor->CreateSRV(srvDesc);
        texColor->CreateUAV(uavDesc);
        texSSRColor = eastl::make_unique<Texture2D>(texDesc);
        texSSRColor->CreateSRV(srvDesc);
        texSSRColor->CreateUAV(uavDesc);
        texSSRTDiffuseColor = eastl::make_unique<Texture2D>(texDesc);
        texSSRTDiffuseColor->CreateSRV(srvDesc);
        texSSRTDiffuseColor->CreateUAV(uavDesc);
        texHistory = eastl::make_unique<Texture2D>(texDesc);
        texHistory->CreateSRV(srvDesc);
        texHistory->CreateUAV(uavDesc);
        texHistoryDiffuse = eastl::make_unique<Texture2D>(texDesc);
        texHistoryDiffuse->CreateSRV(srvDesc);
        texHistoryDiffuse->CreateUAV(uavDesc);
        texTemporal = eastl::make_unique<Texture2D>(texDesc);
        texTemporal->CreateSRV(srvDesc);
        texTemporal->CreateUAV(uavDesc);
        texVariance = eastl::make_unique<Texture2D>(texDesc);
        texVariance->CreateSRV(srvDesc);
        texVariance->CreateUAV(uavDesc);

        texDesc.Format = srvDesc.Format = uavDesc.Format =  DXGI_FORMAT_R11G11B10_FLOAT;

        texMoments = eastl::make_unique<Texture2D>(texDesc);
        texMoments->CreateSRV(srvDesc);
        texMoments->CreateUAV(uavDesc);
        texHistoryMoments = eastl::make_unique<Texture2D>(texDesc);
        texHistoryMoments->CreateSRV(srvDesc);
        texHistoryMoments->CreateUAV(uavDesc);
        texHistoryMomentsDiffuse = eastl::make_unique<Texture2D>(texDesc);
        texHistoryMomentsDiffuse->CreateSRV(srvDesc);
        texHistoryMomentsDiffuse->CreateUAV(uavDesc);

        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
        texHistoryNormals = eastl::make_unique<Texture2D>(texDesc);
        texHistoryNormals->CreateSRV(srvDesc);
        texHistoryNormals->CreateUAV(uavDesc);

        texDesc.Format = srvDesc.Format = uavDesc.Format = DXGI_FORMAT_R32_FLOAT;

        // (audit #20) Was a bare `new Texture2D` and therefore leaked. It is *not*
        // dead: Upscaling.cpp copies it into specHitDistanceShared12 as the DLSS-RR
        // specular hit-distance guide whenever Ray Reconstruction is enabled.
        texHitDistance = eastl::make_unique<Texture2D>(texDesc);
        texHitDistance->CreateSRV(srvDesc);
        texHitDistance->CreateUAV(uavDesc);

        texDesc.MipLevels = maxMips;
        srvDesc.Texture2D.MipLevels = texDesc.MipLevels;
        texDepth = eastl::make_unique<Texture2D>(texDesc);
        texDepth->CreateSRV(srvDesc);
        texDepth->CreateUAV(uavDesc);

        for (uint i = 0; i < maxMips; i++) {
			D3D11_SHADER_RESOURCE_VIEW_DESC mipSrvDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MostDetailedMip = i, .MipLevels = 1 }
			};
			DX::ThrowIfFailed(device->CreateShaderResourceView(texDepth->resource.get(), &mipSrvDesc, depthSRVs[i].put()));

			D3D11_UNORDERED_ACCESS_VIEW_DESC mipUavDesc = {
				.Format = texDesc.Format,
				.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D,
				.Texture2D = { .MipSlice = i }
			};
			DX::ThrowIfFailed(device->CreateUnorderedAccessView(texDepth->resource.get(), &mipUavDesc, depthUAVs[i].put()));
		}
    }

    logger::debug("Creating samplers...");
	{
		D3D11_SAMPLER_DESC samplerDesc = {
			.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR,
			.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP,
			.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP,
			.MaxAnisotropy = 1,
			.MinLOD = 0,
			.MaxLOD = D3D11_FLOAT32_MAX
		};
		DX::ThrowIfFailed(device->CreateSamplerState(&samplerDesc, linearSampler.put()));
	}

    logger::debug("Loading noise texture...");
    {
        DirectX::CreateDDSTextureFromFile(device, globals::d3d::context, L"Data\\Shaders\\ScreenSpaceRayTracing\\noise.dds",
            nullptr, noiseSRV.put());
    }

	CompileComputeShaders();
}

#ifdef ENABLE_SHARC
// (audit P6) The four SHARC buffers are 44 MB that used to be allocated at feature
// setup even though SHARC is experimental, marked "(Broken)" in the UI and off by
// default. Create them the first time SHARC is actually switched on instead. Called
// from DrawSSRTDiffuse before the first dispatch that binds them, so a runtime enable
// never dispatches against null UAVs; disabling again binds nullptr rather than
// releasing, so there is nothing to dangle.
void ScreenSpaceRayTracing::EnsureSharcResources()
{
    if (sharcHashEntries && sharcHashCopyOffsets && sharcVoxelData && sharcVoxelDataPrev)
        return;

    logger::debug("Creating SHARC buffers...");
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

        std::uint32_t numEntries = sharcNumEntries;

        // Hash entries buffer - structured buffer with 64-bits entries to store the hashes
        // Voxel data buffer - structured buffer with 128-bit entries which stores accumulated radiance and sample count. Two instances are used to store current and previous frame data

        sbDesc.StructureByteStride = sizeof(std::uint64_t);
        sbDesc.ByteWidth = sbDesc.StructureByteStride * numEntries;
        sharcHashEntries = eastl::make_unique<Buffer>(sbDesc);
        srvDesc.Buffer.NumElements = numEntries;
        uavDesc.Buffer.NumElements = numEntries;
        sharcHashEntries->CreateSRV(srvDesc);
        sharcHashEntries->CreateUAV(uavDesc);

        sbDesc.StructureByteStride = sizeof(std::uint32_t);
        sbDesc.ByteWidth = sbDesc.StructureByteStride * numEntries;
        sharcHashCopyOffsets = eastl::make_unique<Buffer>(sbDesc);
        srvDesc.Buffer.NumElements = numEntries;
        uavDesc.Buffer.NumElements = numEntries;
        sharcHashCopyOffsets->CreateSRV(srvDesc);
        sharcHashCopyOffsets->CreateUAV(uavDesc);

        sbDesc.StructureByteStride = 4 * sizeof(uint32_t);
        sbDesc.ByteWidth = numEntries * sizeof(float4);
        sharcVoxelData = eastl::make_unique<Buffer>(sbDesc);
        srvDesc.Buffer.NumElements = numEntries;
        uavDesc.Buffer.NumElements = numEntries;
        sharcVoxelData->CreateSRV(srvDesc);
        sharcVoxelData->CreateUAV(uavDesc);
        sharcVoxelDataPrev = eastl::make_unique<Buffer>(sbDesc);
        sharcVoxelDataPrev->CreateSRV(srvDesc);
        sharcVoxelDataPrev->CreateUAV(uavDesc);
	}
}
#endif

void ScreenSpaceRayTracing::ClearShaderCache()
{
    static const std::vector<winrt::com_ptr<ID3D11ComputeShader>*> shaderPtrs = {
        &raymarchSpecularCS, &raymarchDiffuseCS, &prepareColorCS, &preprocessDepthCS, &depthDownsampleCS, &diffuseCompositeCS, &temporalCS, &varianceCS, &spatialCS,
#ifdef ENABLE_SHARC
        &raymarchDiffuseSharcCS, &sharcUpdateRaymarchCS, &sharcResolveCS
#endif
    };

    for (auto shader : shaderPtrs)
        *shader = nullptr;

    CompileComputeShaders();
}

void ScreenSpaceRayTracing::CompileComputeShaders()
{
    struct ShaderCompileInfo
    {
        winrt::com_ptr<ID3D11ComputeShader>* programPtr;
        std::string_view filename;
        std::vector<std::pair<const char*, const char*>> defines;
    };

    std::vector<std::pair<const char*, const char*>> defines;

    if (globals::features::dynamicCubemaps.loaded)
		defines.push_back({ "DYNAMIC_CUBEMAPS", nullptr });

    if (globals::features::screenSpaceGI.loaded)
		defines.push_back({ "SSGI", nullptr });

    if (globals::features::skylighting.loaded)
		defines.push_back({ "SKYLIGHTING", nullptr });

    const std::string DiffuseSPPStr = std::to_string(settings.DiffuseSPP);

    defines.push_back({ "DIFFUSE_SPP", DiffuseSPPStr.c_str() });

#ifdef ENABLE_SHARC
    auto definesSharcUpdate = defines;
    definesSharcUpdate.push_back({ "SHARC_UPDATE", "1" });

    auto definesSharc = defines;
    definesSharc.push_back({ "SHARC_RENDER", "1" });
#endif

    auto definesSpecular = defines;
    definesSpecular.push_back({ "SSRT_SPECULAR", nullptr });

    std::vector<ShaderCompileInfo>
        shaderInfos = {
            { &raymarchDiffuseCS, "ssrt_raymarch.hlsl", defines },
            { &raymarchSpecularCS, "ssrt_raymarch.hlsl", definesSpecular },
            { &prepareColorCS, "ssrt_prepare_color.hlsl", {} },
            { &preprocessDepthCS, "ssrt_preprocess_depth.hlsl", {} },
            { &depthDownsampleCS, "ssrt_depth_downsample.hlsl", {} },
            { &diffuseCompositeCS, "ssrt_diffuse_composite.hlsl", {} },
            { &temporalCS, "ssrt_temporal.hlsl", {} },
            { &varianceCS, "ssrt_variance.hlsl", {} },
            { &spatialCS, "ssrt_spatial.hlsl", {} },
            { &spatialSpecularCS, "ssrt_spatial.hlsl", definesSpecular },
#ifdef ENABLE_SHARC
            { &raymarchDiffuseSharcCS, "ssrt_raymarch.hlsl", definesSharc },
            { &sharcUpdateRaymarchCS, "ssrt_raymarch.hlsl", definesSharcUpdate },
            { &sharcResolveCS, "sharc_resolve.hlsl", {} }
#endif
        };

    for (auto& info : shaderInfos) {
        auto path = std::filesystem::path("Data\\Shaders\\ScreenSpaceRayTracing") / info.filename;
        if (auto rawPtr = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path.c_str(), info.defines, "cs_5_0")))
            info.programPtr->attach(rawPtr);
    }
}

void ScreenSpaceRayTracing::Prepass()
{
    if (recompileFlag) {
        recompileFlag = false;
        CompileComputeShaders();
    }

    // (audit P8) The Hi-Z pyramid this pass builds is only ever read by the two SSRT
    // raymarch passes, so with both switched off it was 1 copy + 8 downsample
    // dispatches of pure waste every frame.
    if (!settings.EnableDiffuse && !settings.EnableSpecular)
        return;

    // (audit P9) Cache the interior test once per frame instead of repeating the cell
    // lookup in DrawSSRTSpecular and DrawSSRTDiffuse. Safe because Prepass runs from
    // StartDeferred before either draw, and the gate above only fires when neither of
    // them will run at all.
    inInterior = true;
    if (auto player = RE::PlayerCharacter::GetSingleton()) {
        if (auto parentCell = player->GetParentCell()) {
            inInterior = parentCell->IsInteriorCell();
        }
    }

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];

    float2 size = Util::ConvertToDynamic(state->screenSize);
    float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };

    std::array<ID3D11ShaderResourceView*, 5> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 1> uavs = { nullptr };

    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };
    context->CSSetSamplers(0, 1, samplers.data());

    state->BeginPerfEvent("SSRT Prepass");

    // (audit #8) texDepth is allocated at full resolution but only the dynamic-resolution
    // sub-rect is ever written, and it was never cleared. The region beyond the dispatch
    // therefore held undefined data that the downsample's min() drags into the coarse
    // mips; a value near 0 (= near plane) makes any ray landing in such a tile report an
    // instant hit. Clearing every mip to the far plane makes the pyramid well defined:
    // combined with the out-of-sub-rect writes in ssrt_preprocess_depth.hlsl, min() with
    // 1.0 is a no-op so each level inherits "1.0 outside the valid area" by induction.
    // Only needed when the extent changes -- with dynamic resolution off that is once,
    // and even with it on it is one clear per resolution change, not per frame.
    if (size.x != lastDepthExtent.x || size.y != lastDepthExtent.y) {
        lastDepthExtent = size;
        const float farPlane[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        for (uint i = 0; i < maxMips; ++i)
            context->ClearUnorderedAccessViewFloat(depthUAVs[i].get(), farPlane);
    }

    // preprocess depth
    {
        uavs.at(0) = texDepth->uav.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
        context->CSSetShader(preprocessDepthCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

        // context->GenerateMips(texDepth->srv.get());

        resetViews();
    }

    // downsample depth
    {
        state->BeginPerfEvent("Downsample Depth - HiZ Buffer");
        for (int i = 0; i < maxMips - 1; ++i) {
            uavs.at(0) = depthUAVs[i + 1].get();
            srvs.at(0) = depthSRVs[i].get();

            context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
            context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
            context->CSSetShader(depthDownsampleCS.get(), nullptr, 0);

            // (audit P3) One thread per *destination* pixel, i.e. per pixel of mip i+1.
            // The old `dispatchCount >> i` sized the dispatch for mip i and launched 4x
            // the threads actually needed at every level. Note this cannot be written
            // as `dispatchCount >> (i + 1)`: that is floor(ceil(size/8) / 2^(i+1)) and
            // under-covers whenever the group count is odd (e.g. 1080 -> 135 groups ->
            // 67 instead of the 68 needed for mip 1, leaving the bottom rows unwritten).
            const uint mipWidth = std::max(1u, (uint)size.x >> (i + 1));
            const uint mipHeight = std::max(1u, (uint)size.y >> (i + 1));
            context->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);
            resetViews();
        }
        state->EndPerfEvent();
    }

    state->EndPerfEvent();

    auto view = texDepth->srv.get();
    context->PSSetShaderResources(99, 1, &view);
}

void ScreenSpaceRayTracing::DrawSSRTSpecular()
{
    if (!settings.EnableSpecular)
        return;

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    state->BeginPerfEvent("SSRT Compute");

    auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
    auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    auto motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

    auto& dynamicCubemaps = globals::features::dynamicCubemaps;
    auto& ssgi = globals::features::screenSpaceGI;
    auto& skylighting = globals::features::skylighting;

    float2 size = Util::ConvertToDynamic(state->screenSize);
    float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };
    
    SSRTCB ssrCBData;
    {
        ssrCBData.MaxSteps = settings.MaxSteps;
        // (audit P3) Clamp against the allocated mip count: a config saved by an older
        // build may hold a value above maxMips - 1, and loading a mip that does not
        // exist returns 0 == near plane, i.e. an immediate false hit.
        ssrCBData.MaxMips = std::min(settings.MaxMips, maxMips - 1);
        ssrCBData.Thickness = settings.Thickness;
        ssrCBData.NormalBias = settings.NormalBias;
        ssrCBData.BRDFBias = settings.BRDFBias;
        ssrCBData.UseDynamicCubemapsAsFallback = (uint)settings.UseDynamicCubemapsAsFallbackSpecular && dynamicCubemaps.loaded;
        ssrCBData.OcclusionStrength = settings.OcclusionStrength;
        ssrCBData.CubemapNormalization = settings.CubemapNormalization;
    }
    ssrtCB->Update(ssrCBData);
    auto buffer = ssrtCB->CB();
    context->CSSetConstantBuffers(1, 1, &buffer);

    // (audit P6) Specular raymarch UAV slots: u0 radiance/confidence,
	// u1 hit distance (was u2; u1 came free when texHitPDF was dropped). The SVGF
	// temporal pass below reuses u0/u1 for its own two outputs.
    std::array<ID3D11ShaderResourceView*, 12> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 2> uavs = { nullptr };

    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

    context->CSSetSamplers(0, 1, samplers.data());

    // prepare color
    srvs.at(0) = main.SRV;
    srvs.at(1) = specular.SRV;
    srvs.at(2) = normal.SRV;
    uavs.at(0) = texColor->uav.get();

    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->CSSetShader(prepareColorCS.get(), nullptr, 0);

    context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

    resetViews();

    const auto envTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr;
	const auto envReflectionsTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr;

    auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec] = ssgi.GetOutputTextures();

    // raymarch
    state->BeginPerfEvent("Raymarch");
    
    uavs.at(0) = texSSRColor->uav.get();
    uavs.at(1) = texHitDistance->uav.get();  // (audit P6) was u2; u1 freed by dropping texHitPDF

    srvs.at(0) = texHistory->srv.get();
    srvs.at(1) = motion.SRV;
    srvs.at(2) = normal.SRV;
    srvs.at(3) = texColor->srv.get();
    srvs.at(4) = depth.depthSRV;
    srvs.at(5) = texDepth->srv.get();
    srvs.at(6) = noiseSRV.get();
    srvs.at(7) = envTexture;
    srvs.at(8) = inInterior ? envTexture : envReflectionsTexture;
    srvs.at(9) = ssgi_ao;
    srvs.at(10) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr;
    srvs.at(11) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.stbn_vec3_2Dx1D_128x128x64.get() : nullptr;

    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->CSSetShader(raymarchSpecularCS.get(), nullptr, 0);
    context->CSSetConstantBuffers(1, 1, &buffer);

    context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

    state->EndPerfEvent();

    resetViews();

    if (settings.EnableSVGF) {
        DenoiserCB denoiserCBData = GetDenoiserCBData();
        denoiserCB->Update(denoiserCBData);
        auto denoiserBuffer = denoiserCB->CB();
        context->CSSetConstantBuffers(2, 1, &denoiserBuffer);

        // temporal filter
        uavs.at(0) = texTemporal->uav.get();
        uavs.at(1) = texMoments->uav.get();
        srvs.at(0) = texHistory->srv.get();
        srvs.at(1) = motion.SRV;
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texSSRColor->srv.get();
        srvs.at(4) = depth.depthSRV;
        srvs.at(5) = texHistoryMoments->srv.get();
        srvs.at(6) = texHistoryNormals->srv.get();

        context->CSSetShaderResources(0, 7, srvs.data());
        context->CSSetUnorderedAccessViews(0, 2, uavs.data(), nullptr);
        context->CSSetShader(temporalCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        context->CopyResource(texHistoryMoments->resource.get(), texMoments->resource.get());

        // variance filter
        uavs.at(0) = texVariance->uav.get();
        srvs.at(0) = texHistory->srv.get();
        srvs.at(1) = texMoments->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texTemporal->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, 5, srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(varianceCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // spatial filter
        for (int i = 0; i < (int)settings.AtrousIterations; ++i)
        {
            denoiserCBData.atrousIterations = i;
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);
            uavs.at(0) = (i % 2 == 0) ? texSSRColor->uav.get() : texVariance->uav.get();
            srvs.at(0) = texHistory->srv.get();
            // (spec A1) t1 carries the moments texture, whose .z is the accumulated frame
            // count the adaptive early-out votes on. It used to receive the motion-vector
            // target, which ssrt_spatial.hlsl never declared.
            srvs.at(1) = texMoments->srv.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = (i % 2 == 0) ? texVariance->srv.get() : texSSRColor->srv.get();
            srvs.at(4) = depth.depthSRV;

            context->CSSetShaderResources(0, 5, srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(spatialSpecularCS.get(), nullptr, 0);

            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

            resetViews();
        }

        if (settings.AtrousIterations % 2 == 0) {
            context->CopyResource(texSSRColor->resource.get(), texVariance->resource.get());
        }
    }

    // output
    // (audit P6) texOutput was a byte-identical copy of texSSRColor whose only reader
    // was the deferred composite's SRV; that now binds texSSRColor->srv directly
    // (Deferred.cpp), saving a full-screen R16G16B16A16 CopyResource per frame plus the
    // texture itself.
    // (audit #13) Specular runs after diffuse, so it owns the once-per-frame snapshot.
    CopyHistoryNormals();
    context->CopyResource(texHistory->resource.get(), texSSRColor->resource.get());

    context->CSSetShader(nullptr, nullptr, 0);

    state->EndPerfEvent();
}

// (audit #13) texHistoryNormals is the previous frame's normal-roughness buffer that
// ssrt_temporal.hlsl validates its reprojected history against. The copy used to live
// only at the end of DrawSSRTSpecular, so with EnableSpecular off and EnableSVGF on the
// texture stayed at its cleared contents forever, IsValidHistory() rejected every
// candidate and the diffuse temporal filter never accumulated -- exactly the
// configuration the diffuse+fallback setup runs in.
//
// It must happen after every temporal pass of the frame has read it, and exactly once.
// Deferred::DeferredPasses calls DrawSSRTDiffuse then DrawSSRTSpecular, so specular
// takes it when enabled and diffuse takes it otherwise.
void ScreenSpaceRayTracing::CopyHistoryNormals()
{
    auto normal = globals::game::renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    globals::d3d::context->CopyResource(texHistoryNormals->resource.get(), normal.texture);
}

void ScreenSpaceRayTracing::DrawSSRTDiffuse()
{
    if (!settings.EnableDiffuse)
        return;

    auto renderer = globals::game::renderer;
    auto context = globals::d3d::context;
    auto state = globals::state;

    state->BeginPerfEvent("SSRT Diffuse Compute");

    auto main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
    auto depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    auto specular = renderer->GetRuntimeData().renderTargets[SPECULAR];
    auto normal = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];
    auto albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
    auto motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

    auto& dynamicCubemaps = globals::features::dynamicCubemaps;
    auto& ssgi = globals::features::screenSpaceGI;
    auto& skylighting = globals::features::skylighting;

    float2 size = Util::ConvertToDynamic(state->screenSize);
    float2 dispatchCount = { (size.x + 7) / 8, (size.y + 7) / 8 };
    
    SSRTCB ssrCBData;
    {
        ssrCBData.MaxSteps = settings.MaxSteps;
        // (audit P3) See DrawSSRTSpecular: clamp to the allocated mip count.
        ssrCBData.MaxMips = std::min(settings.MaxMips, maxMips - 1);
        ssrCBData.Thickness = settings.Thickness;
        ssrCBData.NormalBias = settings.NormalBias;
        ssrCBData.BRDFBias = settings.BRDFBias;
        ssrCBData.UseDynamicCubemapsAsFallback = (uint)settings.UseDynamicCubemapsAsFallback && dynamicCubemaps.loaded;
        ssrCBData.OcclusionStrength = settings.OcclusionStrength;
        ssrCBData.CubemapNormalization = settings.CubemapNormalization;
    }
    ssrtCB->Update(ssrCBData);
    auto buffer = ssrtCB->CB();
    context->CSSetConstantBuffers(1, 1, &buffer);

    // (audit P6) Raymarch UAV slots: u0 radiance/confidence, u1..u4 SHARC (bound only
	// while SHARC is enabled, and only declared by the SHARC shader permutations). Keep
	// this in lockstep with the register map at the top of ssrt_raymarch.hlsl and
	// sharc_resolve.hlsl. The SVGF temporal pass below reuses slots u0/u1 for its own
	// two outputs, so the array is never smaller than 2.
    std::array<ID3D11ShaderResourceView*, 13> srvs = { nullptr };
	std::array<ID3D11UnorderedAccessView*, 5> uavs = { nullptr };

    auto resetViews = [&]() {
		srvs.fill(nullptr);
		uavs.fill(nullptr);

		context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
		context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
	};

    std::array<ID3D11SamplerState*, 1> samplers = { linearSampler.get() };

    context->CSSetSamplers(0, 1, samplers.data());

    const auto envTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envTexture->srv.get() : nullptr;
	const auto envReflectionsTexture = dynamicCubemaps.loaded ? dynamicCubemaps.envReflectionsTexture->srv.get() : nullptr;

    auto [ssgi_ao, ssgi_y, ssgi_cocg, ssgi_gi_spec] = ssgi.GetOutputTextures();

    uavs.at(0) = texSSRTDiffuseColor->uav.get();
#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        EnsureSharcResources();  // (audit P6) allocate on first enable, before any dispatch binds them
        uavs.at(1) = sharcHashEntries->uav.get();
        uavs.at(2) = sharcHashCopyOffsets->uav.get();
        uavs.at(3) = sharcVoxelData->uav.get();
        uavs.at(4) = sharcVoxelDataPrev->uav.get();
    }
#endif

    srvs.at(0) = texHistoryDiffuse->srv.get();
    srvs.at(1) = motion.SRV;
    srvs.at(2) = normal.SRV;
    srvs.at(3) = main.SRV;
    srvs.at(4) = depth.depthSRV;
    srvs.at(5) = texDepth->srv.get();
    srvs.at(6) = noiseSRV.get();
    srvs.at(7) = envTexture;
    srvs.at(8) = inInterior ? envTexture : envReflectionsTexture;
    srvs.at(9) = ssgi_ao;
    srvs.at(10) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.texProbeArray->srv.get() : nullptr;
    srvs.at(11) = dynamicCubemaps.loaded && skylighting.loaded ? skylighting.stbn_vec3_2Dx1D_128x128x64.get() : nullptr;
    srvs.at(12) = albedo.SRV;

    context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
    context->CSSetUnorderedAccessViews(0, (uint)uavs.size(), uavs.data(), nullptr);
    context->CSSetConstantBuffers(1, 1, &buffer);
#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        state->BeginPerfEvent("SHARC Update");
        context->CSSetShader(sharcUpdateRaymarchCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        state->EndPerfEvent();

        state->BeginPerfEvent("SHARC Resolve");
        context->CSSetShader(sharcResolveCS.get(), nullptr, 0);

        context->Dispatch(sharcNumEntries / 256u, 1, 1);
        state->EndPerfEvent();
    }

    context->CSSetShader(settings.EnableSharc ? raymarchDiffuseSharcCS.get() : raymarchDiffuseCS.get(), nullptr, 0);
#else
    context->CSSetShader(raymarchDiffuseCS.get(), nullptr, 0);
#endif
    context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
    resetViews();

#ifdef ENABLE_SHARC
    if (settings.EnableSharc) {
        std::swap(sharcVoxelData, sharcVoxelDataPrev);
    }
#endif

    if (settings.EnableSVGF) {
        DenoiserCB denoiserCBData = GetDenoiserCBData();
        denoiserCB->Update(denoiserCBData);
        auto denoiserBuffer = denoiserCB->CB();
        context->CSSetConstantBuffers(2, 1, &denoiserBuffer);
        // temporal filter
        uavs.at(0) = texTemporal->uav.get();
        uavs.at(1) = texMoments->uav.get();
        srvs.at(0) = texHistoryDiffuse->srv.get();
        srvs.at(1) = motion.SRV;
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texSSRTDiffuseColor->srv.get();
        srvs.at(4) = depth.depthSRV;
        srvs.at(5) = texHistoryMomentsDiffuse->srv.get();
        srvs.at(6) = texHistoryNormals->srv.get();

        context->CSSetShaderResources(0, 7, srvs.data());
        context->CSSetUnorderedAccessViews(0, 2, uavs.data(), nullptr);
        context->CSSetShader(temporalCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        context->CopyResource(texHistoryMomentsDiffuse->resource.get(), texMoments->resource.get());

        // variance filter
        uavs.at(0) = texVariance->uav.get();
        srvs.at(0) = texHistoryDiffuse->srv.get();
        srvs.at(1) = texMoments->srv.get();
        srvs.at(2) = normal.SRV;
        srvs.at(3) = texTemporal->srv.get();
        srvs.at(4) = depth.depthSRV;

        context->CSSetShaderResources(0, 5, srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(varianceCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);
        resetViews();

        // spatial filter
        for (int i = 0; i < (int)settings.AtrousIterations; ++i)
        {
            denoiserCBData.atrousIterations = i;
            denoiserCB->Update(denoiserCBData);
            denoiserBuffer = denoiserCB->CB();
            context->CSSetConstantBuffers(2, 1, &denoiserBuffer);
            uavs.at(0) = (i % 2 == 0) ? texSSRTDiffuseColor->uav.get() : texVariance->uav.get();
            srvs.at(0) = texHistoryDiffuse->srv.get();
            // (spec A1) t1 = moments; see the matching binding in DrawSSRTSpecular.
            srvs.at(1) = texMoments->srv.get();
            srvs.at(2) = normal.SRV;
            srvs.at(3) = (i % 2 == 0) ? texVariance->srv.get() : texSSRTDiffuseColor->srv.get();
            srvs.at(4) = depth.depthSRV;

            context->CSSetShaderResources(0, 5, srvs.data());
            context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
            context->CSSetShader(spatialCS.get(), nullptr, 0);

            context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

            resetViews();
        }

        if (settings.AtrousIterations % 2 == 0) {
            context->CopyResource(texSSRTDiffuseColor->resource.get(), texVariance->resource.get());
        }
    }

    context->CopyResource(texHistoryDiffuse->resource.get(), texSSRTDiffuseColor->resource.get());

    // composite
    {
        uavs.at(0) = main.UAV;
        srvs.at(0) = texSSRTDiffuseColor->srv.get();
        srvs.at(1) = albedo.SRV;

        context->CSSetShaderResources(0, (uint)srvs.size(), srvs.data());
        context->CSSetUnorderedAccessViews(0, 1, uavs.data(), nullptr);
        context->CSSetShader(diffuseCompositeCS.get(), nullptr, 0);

        context->Dispatch((uint)dispatchCount.x, (uint)dispatchCount.y, 1);

        resetViews();
    }

    // (audit #13) Only when specular will not run afterwards, so the snapshot still
    // happens exactly once per frame and after every temporal pass has read it.
    if (!settings.EnableSpecular)
        CopyHistoryNormals();

    state->EndPerfEvent();

    context->CSSetShader(nullptr, nullptr, 0);
}

ScreenSpaceRayTracing::DenoiserCB ScreenSpaceRayTracing::GetDenoiserCBData() const
{
    DenoiserCB data;
    data.invMaxAccumulatedFrames = 1.0f / (settings.MaxAccumulatedFrames + 1.0f);
    data.atrousIterations = settings.AtrousIterations;
    data.colorPhi = settings.ColorPhi;
    data.normalPhi = settings.NormalPhi;
    data.adaptiveFiltering = settings.AdaptiveFiltering ? 1u : 0u;
    data.adaptiveHistoryThreshold = (float)settings.AdaptiveHistoryThreshold;
    data.adaptiveVarianceEps = settings.AdaptiveVarianceEps;
    // (spec S1) The bool collapses into the strength: 0 sigmas is the off state the
    // shader tests, so ssrt_temporal.hlsl needs a single group-uniform predicate rather
    // than two.
    data.fireflyClampSigma = settings.FireflyClamp ? settings.FireflyClampSigma : 0.0f;
    return data;
}

ScreenSpaceRayTracing::SharedData ScreenSpaceRayTracing::GetCommonBufferData()
{
    SharedData data;
    data.EnableSpecular = settings.EnableSpecular;
    data.SpecularMult = settings.SpecularMult;
    data.DiffuseMult = settings.EnableDiffuse ? settings.DiffuseMult : 0.0f;
    data.AmbientMult = settings.AmbientMult;
    return data;
}