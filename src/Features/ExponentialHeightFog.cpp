#include "ExponentialHeightFog.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/IBL.h"
#include "Features/InverseSquareLighting.h"
#include "Features/LightLimitFix.h"
#include "Features/Skylighting.h"
#include "Features/TerrainBlending.h"
#include "Features/TerrainShadows.h"
#include "Features/VolumetricLighting.h"
#include "Menu.h"
#include "State.h"
#include "Util.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ExponentialHeightFog::Settings,
    enabled,
    useDynamicCubemaps,
    startDistance,
    fogHeight,
    fogHeightFalloff,
    fogDensity,
    directionalInscatteringMultiplier,
    directionalInscatteringExponent,
    inscatteringTint,
    cubemapMipLevel,
    fogHeight2,
    fogHeightFalloff2,
    fogDensity2)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ExponentialHeightFog::VolumetricSettings,
    Enabled,
    Distance,
    StartDistance,
    NearFadeInDistance,
    NearGridDistance,
    ExtinctionScale,
    ScatteringDistribution,
    Albedo,
    Emissive,
    DirectionalIntensity,
    SkyLightingIntensity,
    LocalLightIntensity,
    HistoryWeight,
    HistoryMissSampleCount,
    SampleJitter,
    UpsampleJitter,
    GridPixelSize,
    GridSizeZ,
    FarGridPixelSize,
    FarGridSizeZ,
    ShadowBias,
    DepthDistributionScale,
    NoiseScale,
    NoiseThreshold,
    NoiseVelocity)

namespace
{
    float Halton(uint32_t a_index, uint32_t a_base)
    {
        float result = 0.0f;
        float invBase = 1.0f / static_cast<float>(a_base);
        float fraction = invBase;
        while (a_index > 0) {
            result += static_cast<float>(a_index % a_base) * fraction;
            a_index /= a_base;
            fraction *= invBase;
        }
        return result;
    }

    constexpr const char* kVolumetricKey = "Volumetric Fog (Batch 38)";
}

void ExponentialHeightFog::RestoreDefaultSettings()
{
    settings = {};
    volumetric = {};
    upstreamFixFogDoubleOpacity = true;
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
    settings = o_json;
    if (o_json.contains(kVolumetricKey) && o_json[kVolumetricKey].is_object())
        volumetric = o_json[kVolumetricKey];
    upstreamFixFogDoubleOpacity = o_json.value("UpstreamFixFogDoubleOpacity", true);
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
    o_json = settings;
    o_json[kVolumetricKey] = volumetric;
    o_json["UpstreamFixFogDoubleOpacity"] = upstreamFixFogDoubleOpacity;
}

ExponentialHeightFog::Settings ExponentialHeightFog::GetCommonBufferData() const
{
    Settings data = settings;
    // bit 0 = use cubemaps, bit 1 = upstream fix 22ac9859c (fog colour not pre-weighted by opacity)
    data.useDynamicCubemaps = (settings.useDynamicCubemaps ? 1u : 0u) | (upstreamFixFogDoubleOpacity ? 2u : 0u);
    return data;
}

bool ExponentialHeightFog::VolumetricFogRequested() const
{
    return loaded && volumetric.Enabled && settings.enabled;
}

bool ExponentialHeightFog::VolumetricFogActive() const
{
    if (!VolumetricFogRequested())
        return false;
    const float density2 = GetCommonBufferData().fogDensity2;
    if (settings.fogDensity <= 0.0f && density2 <= 0.0f)
        return false;
    if (volumetric.ExtinctionScale <= 0.0f)
        return false;
    if (globals::game::ui && globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME))
        return false;
    return true;
}

std::string ExponentialHeightFog::VolumetricFogIdleReason() const
{
    if (!VolumetricFogRequested() || VolumetricFogActive())
        return {};
    if (settings.fogDensity <= 0.0f && GetCommonBufferData().fogDensity2 <= 0.0f)
        return "height fog density is 0";
    if (volumetric.ExtinctionScale <= 0.0f)
        return "Volumetric Extinction Scale is 0";
    return "map open";
}

ExponentialHeightFog::GridParams ExponentialHeightFog::ComputeGridParams(uint32_t a_nearSlices, uint32_t a_farSlices) const
{
    GridParams p{};
    const auto cameraData = Util::GetCameraData();
    p.nearPlane = std::max(static_cast<double>(cameraData.y), static_cast<double>(std::max(volumetric.StartDistance, 0.0f)));
    p.totalFar = std::max(p.nearPlane + 1.0, static_cast<double>(std::max(volumetric.Distance, volumetric.StartDistance + 1.0f)));
    p.nearEnd = std::min(std::max(static_cast<double>(std::max(volumetric.NearGridDistance, 0.0f)), p.nearPlane + 1.0), p.totalFar);
    p.farEnabled = p.nearEnd + 1.0 < p.totalFar;

    auto computeZ = [this](double a_near, double a_far, uint32_t a_slices) {
        const double nearWithOffset = a_near + 0.095 * 100.0;
        const double distributionScale = std::max(static_cast<double>(volumetric.DepthDistributionScale), static_cast<double>(a_slices) / 120.0);
        const double farExp = std::exp2(std::min(static_cast<double>(a_slices) / distributionScale, 120.0));
        const double offset = (a_far - nearWithOffset * farExp) / (a_far - nearWithOffset);
        const double scale = (1.0 - offset) / nearWithOffset;
        return float4(static_cast<float>(scale), static_cast<float>(offset), static_cast<float>(distributionScale), static_cast<float>(a_slices));
    };
    p.nearZ = computeZ(p.nearPlane, p.nearEnd, a_nearSlices);
    p.farZ = computeZ(p.nearEnd, p.totalFar, a_farSlices);
    return p;
}

ExponentialHeightFog::VolumetricFogPSData ExponentialHeightFog::GetVolumetricPSData() const
{
    VolumetricFogPSData data{};
    if (!VolumetricFogActive())
        return data;

    const uint32_t nearSlices = std::clamp(volumetric.GridSizeZ, 16u, 160u);
    const uint32_t farSlices = std::clamp(volumetric.FarGridSizeZ, 16u, 160u);
    const auto p = ComputeGridParams(nearSlices, farSlices);

    data.Enabled = 1;
    data.FarEnabled = p.farEnabled ? 1 : 0;
    data.StartDistance = std::max(volumetric.StartDistance, 0.0f);
    data.EndDistance = static_cast<float>(p.totalFar);
    data.NearGridZParams = p.nearZ;
    data.FarGridZParams = p.farZ;
    data.NearGridEndDistance = static_cast<float>(p.nearEnd);
    data.UpsampleJitter = std::clamp(volumetric.UpsampleJitter, 0.0f, 1.0f);
    return data;
}

void ExponentialHeightFog::SetupResources()
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

    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    shadowSampler = nullptr;
    DX::ThrowIfFailed(globals::d3d::device->CreateSamplerState(&samplerDesc, shadowSampler.put()));

    volumetricFogCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<VolumetricFogCB>());
}

void ExponentialHeightFog::ClearShaderCache()
{
    for (auto** cs : { &materialSetupCS, &farMaterialSetupCS, &conservativeDepthCS, &farConservativeDepthCS,
             &lightScatteringCS, &farLightScatteringCS, &integrationCS, &farIntegrationCS }) {
        if (*cs) {
            (*cs)->Release();
            *cs = nullptr;
        }
    }
}

ID3D11ComputeShader* ExponentialHeightFog::GetShader(ID3D11ComputeShader*& a_cache, const wchar_t* a_file, bool a_far, bool a_lighting)
{
    if (!a_cache) {
        std::vector<std::pair<const char*, const char*>> defines;
        if (a_far)
            defines.emplace_back("VOLUMETRIC_FOG_FAR_GRID", "");
        if (a_lighting) {
            if (globals::features::lightLimitFix.loaded)
                defines.emplace_back("LIGHT_LIMIT_FIX", "");
            if (globals::features::inverseSquareLighting.loaded)
                defines.emplace_back("ISL", "");
            if (globals::features::terrainShadows.loaded)
                defines.emplace_back("TERRAIN_SHADOWS", "");
            if (globals::features::cloudShadows.loaded)
                defines.emplace_back("CLOUD_SHADOWS", "");
        }
        a_cache = static_cast<ID3D11ComputeShader*>(Util::CompileShader(a_file, defines, "cs_5_0"));
    }
    return a_cache;
}

void ExponentialHeightFog::EnsureVolumetricResources()
{
    uint32_t pixelSize = std::clamp(volumetric.GridPixelSize, 4u, 64u);
    const uint32_t gridZ = std::clamp(volumetric.GridSizeZ, 16u, 160u);
    uint32_t farPixelSize = std::clamp(volumetric.FarGridPixelSize, 4u, 64u);
    const uint32_t farGridZ = std::clamp(volumetric.FarGridSizeZ, 16u, 160u);
    // The volume covers the rendered region (internal resolution under DLSS/FSR, both eyes
    // side by side in VR).
    const auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);

    auto getGridSize = [&renderSize](uint32_t a_pixelSize, uint32_t a_gridZ) {
        return DirectX::XMUINT4{
            std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.x / static_cast<float>(a_pixelSize)))),
            std::max(1u, static_cast<uint32_t>(std::ceil(renderSize.y / static_cast<float>(a_pixelSize)))),
            a_gridZ,
            0u
        };
    };
    DirectX::XMUINT4 gridSize = getGridSize(pixelSize, gridZ);

    constexpr uint64_t maxVolumeVoxels = 16ull * 1024ull * 1024ull;
    while (pixelSize < 64u && static_cast<uint64_t>(gridSize.x) * gridSize.y * gridSize.z > maxVolumeVoxels) {
        pixelSize++;
        gridSize = getGridSize(pixelSize, gridZ);
    }

    // The far volume must be coarser than the near volume.
    farPixelSize = std::max(farPixelSize, pixelSize);
    DirectX::XMUINT4 farGridSize = getGridSize(farPixelSize, farGridZ);
    while (farPixelSize < 64u && static_cast<uint64_t>(farGridSize.x) * farGridSize.y * farGridSize.z > maxVolumeVoxels / 4ull) {
        farPixelSize++;
        farGridSize = getGridSize(farPixelSize, farGridZ);
    }

    if (vBufferA &&
        currentGridSize.x == gridSize.x && currentGridSize.y == gridSize.y && currentGridSize.z == gridSize.z &&
        currentFarGridSize.x == farGridSize.x && currentFarGridSize.y == farGridSize.y && currentFarGridSize.z == farGridSize.z)
        return;

    currentGridSize = gridSize;
    currentFarGridSize = farGridSize;

    auto make3D = [](const DirectX::XMUINT4& a_size, bool a_uav) {
        D3D11_TEXTURE3D_DESC texDesc{};
        texDesc.Width = a_size.x;
        texDesc.Height = a_size.y;
        texDesc.Depth = a_size.z;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (a_uav ? D3D11_BIND_UNORDERED_ACCESS : 0);

        auto tex = std::make_unique<Texture3D>(texDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Texture3D.MipLevels = 1;
        tex->CreateSRV(srvDesc);
        if (a_uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = texDesc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE3D;
            uavDesc.Texture3D.MipSlice = 0;
            uavDesc.Texture3D.FirstWSlice = 0;
            uavDesc.Texture3D.WSize = a_size.z;
            tex->CreateUAV(uavDesc);
        }
        return tex;
    };

    auto make2D = [](const DirectX::XMUINT4& a_size, bool a_uav) {
        D3D11_TEXTURE2D_DESC texDesc{};
        texDesc.Width = a_size.x;
        texDesc.Height = a_size.y;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R32_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (a_uav ? D3D11_BIND_UNORDERED_ACCESS : 0);

        auto tex = std::make_unique<Texture2D>(texDesc);
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
        srvDesc.Format = texDesc.Format;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        tex->CreateSRV(srvDesc);
        if (a_uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
            uavDesc.Format = texDesc.Format;
            uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            tex->CreateUAV(uavDesc);
        }
        return tex;
    };

    vBufferA = make3D(gridSize, true);
    conservativeDepth = make2D(gridSize, true);
    conservativeDepthHistory = make2D(gridSize, false);
    lightScattering = make3D(gridSize, true);
    lightScatteringHistory = make3D(gridSize, false);
    integratedLightScattering = make3D(gridSize, true);

    vBufferAFar = make3D(farGridSize, true);
    conservativeDepthFar = make2D(farGridSize, true);
    conservativeDepthFarHistory = make2D(farGridSize, false);
    lightScatteringFar = make3D(farGridSize, true);
    lightScatteringFarHistory = make3D(farGridSize, false);
    integratedLightScatteringFar = make3D(farGridSize, true);

    hasLightScatteringHistory = false;
    hasConservativeDepthHistory = false;
    hasLightScatteringFarHistory = false;
    hasConservativeDepthFarHistory = false;
    lastPrepassFrame = UINT32_MAX;
}

void ExponentialHeightFog::ReleaseVolumetricResources()
{
    if (!vBufferA)
        return;
    vBufferA.reset();
    vBufferAFar.reset();
    conservativeDepth.reset();
    conservativeDepthHistory.reset();
    conservativeDepthFar.reset();
    conservativeDepthFarHistory.reset();
    lightScattering.reset();
    lightScatteringHistory.reset();
    lightScatteringFar.reset();
    lightScatteringFarHistory.reset();
    integratedLightScattering.reset();
    integratedLightScatteringFar.reset();
    currentGridSize = {};
    currentFarGridSize = {};
    hasLightScatteringHistory = false;
    hasConservativeDepthHistory = false;
    hasLightScatteringFarHistory = false;
    hasConservativeDepthFarHistory = false;
    lastPrepassFrame = UINT32_MAX;
    BindIntegratedLightScattering(false);
}

void ExponentialHeightFog::BindIntegratedLightScattering(bool a_valid)
{
    // PS t21 / t22 (upstream t19 / t22; our t19 is ShadowSampling's SharedShadowData).
    ID3D11ShaderResourceView* srvs[2]{
        a_valid && integratedLightScattering ? integratedLightScattering->srv.get() : nullptr,
        a_valid && integratedLightScatteringFar ? integratedLightScatteringFar->srv.get() : nullptr
    };
    globals::d3d::context->PSSetShaderResources(21, 2, srvs);
}

void ExponentialHeightFog::Prepass()
{
    if (!VolumetricFogRequested()) {
        ReleaseVolumetricResources();
        return;
    }
    if (!VolumetricFogActive()) {
        hasLightScatteringHistory = false;
        hasConservativeDepthHistory = false;
        hasLightScatteringFarHistory = false;
        hasConservativeDepthFarHistory = false;
        lastPrepassFrame = UINT32_MAX;
        BindIntegratedLightScattering(false);
        return;
    }

    EnsureVolumetricResources();

    auto* materialCS = GetShader(materialSetupCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", false, false);
    auto* farMaterialCS = GetShader(farMaterialSetupCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogMaterialCS.hlsl", true, false);
    auto* depthCS = GetShader(conservativeDepthCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", false, false);
    auto* farDepthCS = GetShader(farConservativeDepthCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogConservativeDepthCS.hlsl", true, false);
    auto* scatteringCS = GetShader(lightScatteringCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", false, true);
    auto* farScatteringCS = GetShader(farLightScatteringCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogLightScatteringCS.hlsl", true, true);
    auto* integrateCS = GetShader(integrationCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", false, false);
    auto* farIntegrateCS = GetShader(farIntegrationCS, L"Data\\Shaders\\ExponentialHeightFog\\VolumetricFogIntegrationCS.hlsl", true, false);
    if (!materialCS || !farMaterialCS || !depthCS || !farDepthCS || !scatteringCS || !farScatteringCS || !integrateCS || !farIntegrateCS) {
        BindIntegratedLightScattering(false);
        return;
    }

    auto& deferred = *globals::deferred;
    auto& lightLimitFix = globals::features::lightLimitFix;
    auto& ibl = globals::features::ibl;
    auto& skylighting = globals::features::skylighting;

    // Shared sun shadow capture (Deferred::CopyShadowData): the cascade array plus our
    // VR-aware PerGeometry copy at t19.
    const bool hasShadowMap = deferred.HasFreshShadowCapture() && deferred.perShadow && !Util::IsInterior();
    const bool hasLocalLights = lightLimitFix.loaded && lightLimitFix.lights && lightLimitFix.lightIndexList && lightLimitFix.lightGrid;
    const auto& depthStencil = globals::game::renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
    auto& terrainBlending = globals::features::terrainBlending;
    ID3D11ShaderResourceView* depthSrv = terrainBlending.IsBlendingActive() && terrainBlending.blendedDepthTexture16 ? terrainBlending.blendedDepthTexture16->srv.get() : depthStencil.depthSRV;
    const bool hasIBL = ibl.loaded && ibl.settings.EnableDiffuseIBL && ibl.diffuseIBLTexture && ibl.diffuseSkyIBLTexture;
    const bool hasSkylighting = skylighting.loaded && skylighting.texProbeArray;

    const bool temporalReprojection = Util::GetTemporal();
    const bool frameContinuous = lastPrepassFrame != UINT32_MAX && globals::state->frameCount == lastPrepassFrame + 1u;
    const bool temporalHistoryValid = temporalReprojection && hasLightScatteringHistory && frameContinuous;
    const bool temporalHistoryValidFar = temporalReprojection && hasLightScatteringFarHistory && frameContinuous;

    const auto p = ComputeGridParams(currentGridSize.z, currentFarGridSize.z);

    const uint32_t commonFlags =
        (hasShadowMap ? 1u : 0u) |
        (depthSrv ? 2u : 0u) |
        (hasIBL ? 4u : 0u) |
        (hasSkylighting ? 8u : 0u);

    VolumetricFogCB cb{};
    cb.gridSizeAndFlags = { currentGridSize.x, currentGridSize.y, currentGridSize.z,
        commonFlags | (depthSrv && temporalHistoryValid && hasConservativeDepthHistory ? 16u : 0u) | (hasLocalLights ? 32u : 0u) };
    const float nearFadeInv = volumetric.NearFadeInDistance > 0.0f ? 1.0f / volumetric.NearFadeInDistance : 100000000.0f;
    cb.invGridSizeAndNearFade = { 1.0f / static_cast<float>(currentGridSize.x), 1.0f / static_cast<float>(currentGridSize.y), 1.0f / static_cast<float>(currentGridSize.z), nearFadeInv };
    cb.gridZParams = { p.nearZ.x, p.nearZ.y, p.nearZ.z, 0.0f };

    cb.farGridSizeAndFlags = { currentFarGridSize.x, currentFarGridSize.y, currentFarGridSize.z,
        commonFlags | (depthSrv && temporalHistoryValidFar && hasConservativeDepthFarHistory ? 16u : 0u) };
    cb.farInvGridSizeAndNearFade = { 1.0f / static_cast<float>(currentFarGridSize.x), 1.0f / static_cast<float>(currentFarGridSize.y), 1.0f / static_cast<float>(currentFarGridSize.z), nearFadeInv };
    cb.farGridZParams = { p.farZ.x, p.farZ.y, p.farZ.z, 0.0f };
    cb.farRange = { static_cast<float>(p.nearEnd), static_cast<float>(p.totalFar), 0.0f, 0.0f };

    const auto& fb = globals::game::frameBufferCached;
    cb.clipToWorld[0] = fb.GetCameraViewProjUnjittered(0).Invert();
    cb.clipToWorld[1] = REL::Module::IsVR() ? fb.GetCameraViewProjUnjittered(1).Invert() : cb.clipToWorld[0];

    for (uint32_t i = 0; i < std::size(cb.frameJitterOffsets); i++) {
        const uint32_t temporalFrame = (globals::state->frameCount - i) & 1023u;
        cb.frameJitterOffsets[i] = {
            temporalReprojection ? Halton(temporalFrame, 2) : 0.5f,
            temporalReprojection ? Halton(temporalFrame, 3) : 0.5f,
            temporalReprojection ? Halton(temporalFrame, 5) : 0.5f,
            0.0f
        };
    }
    cb.historyParameters = { std::clamp(volumetric.HistoryWeight, 0.0f, 0.99f), static_cast<float>(std::clamp(volumetric.HistoryMissSampleCount, 1u, 16u)), 0.0f, 0.0f };
    cb.jitterParameters = { temporalReprojection ? std::max(volumetric.SampleJitter, 0.0f) : 0.0f, static_cast<float>(globals::state->frameCount % 8u), 0.0f, 0.0f };
    cb.albedo = volumetric.Albedo;
    cb.emissive = volumetric.Emissive;
    cb.lighting = { volumetric.DirectionalIntensity, volumetric.SkyLightingIntensity, volumetric.LocalLightIntensity, std::clamp(volumetric.ScatteringDistribution, -0.95f, 0.95f) };
    cb.misc = { volumetric.ShadowBias, volumetric.ExtinctionScale, std::max(volumetric.NoiseScale, 0.0f), volumetric.NoiseThreshold };
    cb.noiseVelocity = { volumetric.NoiseVelocity.x, volumetric.NoiseVelocity.y, volumetric.NoiseVelocity.z, 0.0f };
    volumetricFogCB->Update(cb);

    auto context = globals::d3d::context;

    // The PS-side slots must not be bound as SRV while the volumes are written.
    BindIntegratedLightScattering(false);

    ID3D11Buffer* cbuffers[1]{ volumetricFogCB->CB() };
    context->CSSetConstantBuffers(0, 1, cbuffers);
    ID3D11Buffer* sharedBuffers[2]{ globals::state->sharedDataCB->CB(), globals::state->featureDataCB->CB() };
    context->CSSetConstantBuffers(5, 2, sharedBuffers);
    ID3D11Buffer* frameBuffers[1]{ *globals::game::perFrame.get() };
    context->CSSetConstantBuffers(12, 1, frameBuffers);

    ID3D11SamplerState* samplers[2]{ linearSampler.get(), shadowSampler.get() };
    context->CSSetSamplers(0, 2, samplers);

    ID3D11ShaderResourceView* shadowDataSrv = hasShadowMap ? deferred.perShadow->srv.get() : nullptr;
    ID3D11ShaderResourceView* skylightingSrv = hasSkylighting ? skylighting.texProbeArray->srv.get() : nullptr;
    ID3D11ShaderResourceView* iblSrvs[2]{
        hasIBL ? ibl.diffuseIBLTexture->srv.get() : nullptr,
        hasIBL ? ibl.diffuseSkyIBLTexture->srv.get() : nullptr
    };
    ID3D11ShaderResourceView* localLightSrvs[3]{
        hasLocalLights ? lightLimitFix.lights->srv.get() : nullptr,
        hasLocalLights ? lightLimitFix.lightIndexList->srv.get() : nullptr,
        hasLocalLights ? lightLimitFix.lightGrid->srv.get() : nullptr
    };
    ID3D11ShaderResourceView* cloudSrv = nullptr;
    auto& cloudShadows = globals::features::cloudShadows;
    if (cloudShadows.loaded && cloudShadows.texCubemapCloudOcc && globals::game::sky &&
        globals::game::sky->mode.get() == RE::Sky::Mode::kFull && globals::game::sky->currentClimate)
        cloudSrv = cloudShadows.texCubemapCloudOcc->srv.get();
    ID3D11ShaderResourceView* terrainSrv = nullptr;
    auto& terrainShadows = globals::features::terrainShadows;
    if (terrainShadows.loaded && terrainShadows.texShadowHeight)
        terrainSrv = terrainShadows.texShadowHeight->srv.get();

    context->CSSetShaderResources(17, 1, &depthSrv);
    context->CSSetShaderResources(19, 1, &shadowDataSrv);
    context->CSSetShaderResources(25, 1, &cloudSrv);
    context->CSSetShaderResources(35, 3, localLightSrvs);
    context->CSSetShaderResources(50, 1, &skylightingSrv);
    context->CSSetShaderResources(60, 1, &terrainSrv);
    context->CSSetShaderResources(76, 2, iblSrvs);

    struct VolumetricPassDesc
    {
        DirectX::XMUINT4 gridSize;
        Texture3D* vBuffer;
        Texture2D* conservativeDepth;
        Texture2D* conservativeDepthHistory;  // may be null
        Texture3D* scattering;
        Texture3D* scatteringHistory;  // may be null
        Texture3D* integrated;
        ID3D11ComputeShader* materialSetupCS;
        ID3D11ComputeShader* conservativeDepthCS;
        ID3D11ComputeShader* lightScatteringCS;
        ID3D11ComputeShader* integrationCS;
        Util::GpuBucket bucket;
    };

    auto timers = Util::GpuPassTimers::GetSingleton();
    ID3D11ShaderResourceView* shadowMapSrv = hasShadowMap ? deferred.capturedShadowMap.get() : nullptr;

    auto runVolumetricPass = [&](const VolumetricPassDesc& a_pass) {
        const uint32_t groupX = (a_pass.gridSize.x + 7) / 8;
        const uint32_t groupY = (a_pass.gridSize.y + 7) / 8;
        const uint32_t groupZ = (a_pass.gridSize.z + 3) / 4;
        ID3D11UnorderedAccessView* nullUav = nullptr;

        timers->Begin(a_pass.bucket);

        if (depthSrv) {
            ID3D11UnorderedAccessView* uav = a_pass.conservativeDepth->uav.get();
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetShader(a_pass.conservativeDepthCS, nullptr, 0);
            context->Dispatch(groupX, groupY, 1);
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        }

        {
            ID3D11UnorderedAccessView* uav = a_pass.vBuffer->uav.get();
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetShader(a_pass.materialSetupCS, nullptr, 0);
            context->Dispatch(groupX, groupY, groupZ);
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        }

        {
            ID3D11ShaderResourceView* srvs[5]{
                a_pass.vBuffer->srv.get(),
                shadowMapSrv,
                a_pass.scatteringHistory ? a_pass.scatteringHistory->srv.get() : nullptr,
                a_pass.conservativeDepth->srv.get(),
                a_pass.conservativeDepthHistory ? a_pass.conservativeDepthHistory->srv.get() : nullptr
            };
            ID3D11UnorderedAccessView* uav = a_pass.scattering->uav.get();
            context->CSSetShaderResources(0, 5, srvs);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetShader(a_pass.lightScatteringCS, nullptr, 0);
            context->Dispatch(groupX, groupY, groupZ);
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
        }

        {
            ID3D11ShaderResourceView* srvs[5]{ a_pass.scattering->srv.get(), nullptr, nullptr, nullptr, nullptr };
            ID3D11UnorderedAccessView* uav = a_pass.integrated->uav.get();
            context->CSSetShaderResources(0, 5, srvs);
            context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
            context->CSSetShader(a_pass.integrationCS, nullptr, 0);
            context->Dispatch(groupX, groupY, 1);
            context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
            srvs[0] = nullptr;
            context->CSSetShaderResources(0, 1, srvs);
        }

        timers->End(a_pass.bucket);
    };

    const bool nearDepthHistory = depthSrv && temporalHistoryValid && hasConservativeDepthHistory;
    runVolumetricPass({ currentGridSize,
        vBufferA.get(),
        conservativeDepth.get(),
        nearDepthHistory ? conservativeDepthHistory.get() : nullptr,
        lightScattering.get(),
        temporalHistoryValid ? lightScatteringHistory.get() : nullptr,
        integratedLightScattering.get(),
        materialCS, depthCS, scatteringCS, integrateCS,
        Util::GpuBucket::VolumetricFogNear });

    if (p.farEnabled) {
        const bool farDepthHistory = depthSrv && temporalHistoryValidFar && hasConservativeDepthFarHistory;
        runVolumetricPass({ currentFarGridSize,
            vBufferAFar.get(),
            conservativeDepthFar.get(),
            farDepthHistory ? conservativeDepthFarHistory.get() : nullptr,
            lightScatteringFar.get(),
            temporalHistoryValidFar ? lightScatteringFarHistory.get() : nullptr,
            integratedLightScatteringFar.get(),
            farMaterialCS, farDepthCS, farScatteringCS, farIntegrateCS,
            Util::GpuBucket::VolumetricFogFar });
    } else {
        const float clearValue[4]{ 0.0f, 0.0f, 0.0f, 1.0f };
        context->ClearUnorderedAccessViewFloat(integratedLightScatteringFar->uav.get(), clearValue);
    }

    ID3D11ShaderResourceView* nullSrvs[5]{};
    ID3D11UnorderedAccessView* nullUav = nullptr;
    ID3D11SamplerState* nullSamplers[2]{};
    ID3D11Buffer* nullCb = nullptr;
    context->CSSetShaderResources(0, 5, nullSrvs);
    context->CSSetShaderResources(17, 1, nullSrvs);
    context->CSSetShaderResources(19, 1, nullSrvs);
    context->CSSetShaderResources(25, 1, nullSrvs);
    context->CSSetShaderResources(35, 3, nullSrvs);
    context->CSSetShaderResources(50, 1, nullSrvs);
    context->CSSetShaderResources(60, 1, nullSrvs);
    context->CSSetShaderResources(76, 2, nullSrvs);
    context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    context->CSSetSamplers(0, 2, nullSamplers);
    context->CSSetConstantBuffers(0, 1, &nullCb);
    context->CSSetShader(nullptr, nullptr, 0);

    if (temporalReprojection) {
        context->CopyResource(lightScatteringHistory->resource.get(), lightScattering->resource.get());
        hasLightScatteringHistory = true;
        if (depthSrv) {
            context->CopyResource(conservativeDepthHistory->resource.get(), conservativeDepth->resource.get());
            hasConservativeDepthHistory = true;
        } else {
            hasConservativeDepthHistory = false;
        }
        if (p.farEnabled) {
            context->CopyResource(lightScatteringFarHistory->resource.get(), lightScatteringFar->resource.get());
            hasLightScatteringFarHistory = true;
            if (depthSrv) {
                context->CopyResource(conservativeDepthFarHistory->resource.get(), conservativeDepthFar->resource.get());
                hasConservativeDepthFarHistory = true;
            } else {
                hasConservativeDepthFarHistory = false;
            }
        } else {
            hasLightScatteringFarHistory = false;
            hasConservativeDepthFarHistory = false;
        }
    } else {
        hasLightScatteringHistory = false;
        hasConservativeDepthHistory = false;
        hasLightScatteringFarHistory = false;
        hasConservativeDepthFarHistory = false;
    }

    lastPrepassFrame = globals::state->frameCount;
    lastBuildFrame = globals::state->frameCount;
    lastBuildHadShadows = hasShadowMap;
    lastBuildHadLocalLights = hasLocalLights;
    BindIntegratedLightScattering(true);
}

void ExponentialHeightFog::DrawSettings()
{
    ImGui::Checkbox("Enable Exponential Height Fog", (bool*)&settings.enabled);
    ImGui::SliderFloat("Start Distance", &settings.startDistance, 0.0f, 100000.0f, "%.1f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("No fog closer than this distance from the camera (game units).");
    ImGui::SliderFloat("Fog Height", &settings.fogHeight, -22000.0f, 22000.0f, "%.1f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Fog is at full thickness below this world height and thins out above it.");
    ImGui::SliderFloat("Fog Height Falloff", &settings.fogHeightFalloff, 0.001f, 2.0f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How quickly fog thins with height. Higher = fog hugs the ground.");
    ImGui::SliderFloat("Fog Density", &settings.fogDensity, 0.0f, 1.0f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How thick the fog is. Higher = things fade into fog sooner.");
    ImGui::SliderFloat("Directional Light Inscattering Multiplier", &settings.directionalInscatteringMultiplier, 0.0f, 10.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Brightness of the sun/moon glow in the fog when looking toward it. 0 = off.");
    ImGui::SliderFloat("Directional Light Inscattering Exponent", &settings.directionalInscatteringExponent, 1.0f, 128.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Size of that glow. Higher = tighter glow around the sun/moon.");
    ImGui::Checkbox("Use Dynamic Cubemaps for Inscattering", (bool*)&settings.useDynamicCubemaps);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Colours the fog with the surroundings (needs Dynamic Cubemaps) instead of a flat fog colour.");
    ImGui::Checkbox("Upstream fix: fog colour darkened twice", &upstreamFixFogDoubleOpacity);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The cubemap fog colour and the sun glow in fog were dimmed by the fog amount twice, so thin fog looked too dark. Off = old behaviour.");
    ImGui::ColorEdit3("Inscattering Cubemap Tint", (float*)&settings.inscatteringTint);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Tint colour for the cubemap-coloured fog.");
    ImGui::SliderFloat("Inscattering Cubemap Tint Alpha", &settings.inscatteringTint.w, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How much the tint replaces the normal fog colour. 0 = normal fog colour, 1 = tint colour.");
    ImGui::SliderFloat("Cubemap Mip Level", &settings.cubemapMipLevel, 1.0f, 7.0f, "%.1f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How blurred the surroundings are when colouring the fog. Higher = smoother colours.");

    const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;

    if (ImGui::TreeNode("Second Fog Layer")) {
        ImGui::SliderFloat("Fog Height 2", &settings.fogHeight2, -22000.0f, 22000.0f, "%.1f");
        ImGui::SliderFloat("Fog Height Falloff 2", &settings.fogHeightFalloff2, 0.001f, 2.0f, "%.3f");
        ImGui::SliderFloat("Fog Density 2", &settings.fogDensity2, 0.0f, 1.0f, "%.3f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "A second fog layer stacked on the first, with its own height, thinning and thickness.\n"
                "Use it for high haze above ground fog, or a separate low ground fog. 0 = off (default).");
        ImGui::TreePop();
    }

    ImGui::SeparatorText("Volumetric Fog");
    ImGui::Checkbox("Enable Volumetric Fog", &volumetric.Enabled);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Off by default. Turns the near part of the height fog into real 3D fog that the sun, the sky\n"
            "and nearby lights shine into: torches and lanterns get a glow in fog, sunlight through trees\n"
            "leaves beams that tree trunks block. Uses the fog settings above for where the fog is.\n"
            "While it is on, vanilla Volumetric Lighting (light shafts) is paused so the sun's shafts are\n"
            "not drawn twice. Costs roughly 0.5-1.5 ms.");
    if (volumetric.Enabled) {
        if (VolumetricFogRequested() && globals::features::volumetricLighting.loaded)
            ImGui::TextColored(palette.InfoColor, "Vanilla Volumetric Lighting is paused while this is on.");
        if (VolumetricFogActive() && lastBuildFrame != UINT32_MAX)
            ImGui::TextDisabled("Sun shadow in the fog: %s", lastBuildHadShadows ? "on" : "off (no shadow capture this frame)");
        if (!settings.enabled)
            ImGui::TextColored(palette.Warning, "Needs Enable Exponential Height Fog (top of this page).");
        else if (const auto reason = VolumetricFogIdleReason(); !reason.empty())
            ImGui::TextColored(palette.Warning, "Idle: %s.", reason.c_str());

        ImGui::SliderFloat("Temporal History Weight", &volumetric.HistoryWeight, 0.0f, 0.99f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "How much of the previous frame's fog is kept. Higher = smoother fog but light changes\n"
                "(torch flicker, turning quickly) trail behind. Lower = more responsive but grainier.\n"
                "0.9 default (about 10 frames). Upstream uses 0.96. If you see smearing with frame\n"
                "generation, try 0.8.");
        ImGui::SliderFloat("Volumetric View Distance", &volumetric.Distance, 1000.0f, 200000.0f, "%.0f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("How far the 3D fog reaches. Beyond it the normal height fog takes over.");
        ImGui::SliderFloat("Volumetric Start Distance", &volumetric.StartDistance, 0.0f, 20000.0f, "%.0f");
        ImGui::SliderFloat("Near Fade In Distance", &volumetric.NearFadeInDistance, 0.0f, 20000.0f, "%.0f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("The 3D fog fades in over this distance in front of the camera.");
        ImGui::SliderFloat("Near Grid Distance", &volumetric.NearGridDistance, 256.0f, 50000.0f, "%.0f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Distance covered by the detailed near grid. A coarser far grid covers the rest\n"
                "up to the View Distance.");
        ImGui::SliderFloat("Volumetric Extinction Scale", &volumetric.ExtinctionScale, 0.0f, 10.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Thickness of the 3D fog relative to the height fog density.");
        ImGui::SliderFloat("Scattering Distribution", &volumetric.ScatteringDistribution, -0.9f, 0.9f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Above 0: fog glows more when looking toward a light. 0: same from every side.");
        ImGui::SliderFloat("Sun Scattering Intensity", &volumetric.DirectionalIntensity, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Sky Lighting Scattering Intensity", &volumetric.SkyLightingIntensity, 0.0f, 10.0f, "%.2f");
        ImGui::SliderFloat("Local Light Scattering Intensity", &volumetric.LocalLightIntensity, 0.0f, 10.0f, "%.2f");
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Glow of torches, lanterns and other lights in the fog (needs Light Limit Fix).");
        ImGui::ColorEdit4("Volumetric Albedo", (float*)&volumetric.Albedo);
        ImGui::ColorEdit4("Volumetric Emissive", (float*)&volumetric.Emissive);
        if (ImGui::TreeNode("Volumetric Noise")) {
            ImGui::SliderFloat("Noise Scale", &volumetric.NoiseScale, 0.0f, 0.01f, "%.6f");
            ImGui::SliderFloat("Noise Threshold", &volumetric.NoiseThreshold, 0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat3("Noise Velocity", &volumetric.NoiseVelocity.x, -1.0f, 1.0f, "%.3f");
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text(
                    "Breaks the 3D fog into drifting patches.\n"
                    "Noise Scale: size of the patches (0 = off). Threshold: how much is carved away.\n"
                    "Velocity: drift speed.");
            ImGui::TreePop();
        }
        if (ImGui::TreeNode("Advanced")) {
            uint32_t minPixel = 4, maxPixel = 64, minZ = 16, maxZ = 160, minMiss = 1, maxMiss = 16;
            ImGui::SliderScalar("Grid Pixel Size", ImGuiDataType_U32, &volumetric.GridPixelSize, &minPixel, &maxPixel, "%u", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderScalar("Grid Depth Slices", ImGuiDataType_U32, &volumetric.GridSizeZ, &minZ, &maxZ, "%u", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderScalar("Far Grid Pixel Size", ImGuiDataType_U32, &volumetric.FarGridPixelSize, &minPixel, &maxPixel, "%u", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderScalar("Far Grid Depth Slices", ImGuiDataType_U32, &volumetric.FarGridSizeZ, &minZ, &maxZ, "%u", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Sun Shadow Bias", &volumetric.ShadowBias, 0.0f, 0.05f, "%.4f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Depth Distribution Scale", &volumetric.DepthDistributionScale, 1.0f, 128.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderScalar("History Miss Samples", ImGuiDataType_U32, &volumetric.HistoryMissSampleCount, &minMiss, &maxMiss, "%u", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Sample Jitter", &volumetric.SampleJitter, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderFloat("Upsample Jitter", &volumetric.UpsampleJitter, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
            if (auto _tt = Util::HoverTooltipWrapper())
                ImGui::Text("Hides the blocky grid by jittering each pixel's lookup; DLSS smooths it out.");
            ImGui::Text("Near grid: %u x %u x %u, far grid: %u x %u x %u", currentGridSize.x, currentGridSize.y, currentGridSize.z,
                currentFarGridSize.x, currentFarGridSize.y, currentFarGridSize.z);
            const bool built = lastBuildFrame != UINT32_MAX && globals::state->frameCount - lastBuildFrame <= 1u;
            ImGui::Text("Built this frame: %s, sun shadows: %s, local lights: %s", built ? "yes" : "no",
                built && lastBuildHadShadows ? "yes" : "no", built && lastBuildHadLocalLights ? "yes" : "no");
            ImGui::TreePop();
        }
    }
}
