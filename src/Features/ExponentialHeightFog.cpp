#include "ExponentialHeightFog.h"

#include "Deferred.h"
#include "Features/CloudShadows.h"
#include "Features/IBL.h"
#include "Features/InverseSquareLighting.h"
#include "Features/LightLimitFix.h"
#include "Features/LinearLighting.h"
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
    NoiseVelocity,
    MatchWeatherFogBrightness)

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

    constexpr const char* kVolumetricKey = "Volumetric Fog";
    constexpr const char* kVanillaFogKey = "Vanilla Fog";  // (47b)
    constexpr float kMinTakeoverScaleHeight = 3500.0f;     // (47d) ~50 m
    constexpr float kMaxTakeoverScaleHeight = 140000.0f;   // (47d) ~2 km
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    ExponentialHeightFog::VanillaFogSettings,
    WeatherMode,
    RestoreRoomFog,
    ClearFogFar,
    FullFogFar,
    TakeoverScaleHeight)

void ExponentialHeightFog::RestoreDefaultSettings()
{
    settings = {};
    volumetric = {};
    vanillaFog = {};
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
    settings = o_json;
    if (o_json.contains(kVolumetricKey) && o_json[kVolumetricKey].is_object())
        volumetric = o_json[kVolumetricKey];
    vanillaFog = {};
    if (o_json.contains(kVanillaFogKey) && o_json[kVanillaFogKey].is_object()) {
        const auto& vanillaFogJson = o_json[kVanillaFogKey];
        vanillaFog = vanillaFogJson;
        // (47d) The 47b bool "FollowWeather": on -> "CS Fog Takes Over" (the new default), off -> Off.
        if (!vanillaFogJson.contains("WeatherMode") && vanillaFogJson.contains("FollowWeather") && vanillaFogJson["FollowWeather"].is_boolean())
            vanillaFog.WeatherMode = vanillaFogJson["FollowWeather"].get<bool>() ? kWeatherCSFogTakesOver : kWeatherOff;
    }
    if (vanillaFog.WeatherMode >= kWeatherModeCount)
        vanillaFog.WeatherMode = kWeatherCSFogTakesOver;
    if (!std::isfinite(vanillaFog.TakeoverScaleHeight))
        vanillaFog.TakeoverScaleHeight = VanillaFogSettings{}.TakeoverScaleHeight;
    vanillaFog.TakeoverScaleHeight = std::clamp(vanillaFog.TakeoverScaleHeight, kMinTakeoverScaleHeight, kMaxTakeoverScaleHeight);
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
    o_json = settings;
    o_json[kVolumetricKey] = volumetric;
    o_json[kVanillaFogKey] = vanillaFog;
}

ExponentialHeightFog::VanillaFogState ExponentialHeightFog::GetVanillaFogState() const
{
    // (47b) Design C (reviews/analysis-ehf-weather-fog-2026-10.md): how much of the game's own fog comes back under
    // the height fog. Outdoors from the weather's fog distance as the engine has blended it this frame (time of day,
    // weather transitions), so clear and cloudy skies (fog far 150000+) get 0 and fog, storms and blizzards (5000-15000)
    // get 1; indoors the cell's own fog in full. Generic: only RE::Sky's blended values are read.
    VanillaFogState state;
    if (!loaded || !settings.enabled) {
        state.reason = "height fog off";
        return state;
    }
    if (globals::game::ui && globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME)) {
        state.reason = "map open";
        return state;
    }
    state.interior = Util::IsInterior();
    if (state.interior) {
        if (vanillaFog.RestoreRoomFog)
            state.weight = 1.0f;
        else
            state.reason = "EHF Restores Room Fog off";
        return state;
    }
    if (vanillaFog.WeatherMode == kWeatherOff) {
        state.reason = "EHF Follows Weather off";
        return state;
    }
    const auto* sky = globals::game::sky;
    if (!sky || sky->mode.get() != RE::Sky::Mode::kFull || !sky->currentWeather) {
        state.reason = "no weather";
        return state;
    }
    state.hasWeather = true;
    state.fogNear = sky->fogNear;
    state.fogFar = sky->fogFar;
    state.fogPower = sky->fogPower;
    state.fogClamp = sky->fogClamp;
    const float clear = vanillaFog.ClearFogFar;
    const float full = vanillaFog.FullFogFar;
    if (!std::isfinite(state.fogFar) || !(state.fogFar > 0.0f) || !std::isfinite(clear) || !std::isfinite(full) || !(full > 0.0f) || !(clear > full)) {
        state.reason = "no usable fog distance";
        return state;
    }
    const float weight = std::log(clear / state.fogFar) / std::log(clear / full);
    state.weight = std::isfinite(weight) ? std::clamp(weight, 0.0f, 1.0f) : 0.0f;
    if (!(state.weight > 0.0f)) {
        state.reason = "clear weather";
        return state;
    }
    if (vanillaFog.WeatherMode != kWeatherCSFogTakesOver)
        return state;

    // (47d) "CS Fog Takes Over": the weather's fog as an exponential layer (WeatherFog.hlsli) fitted to what the game
    // shows. On screen the game's fog covers min(clamp, x^power)^gamma at x = (d - near) / (far - near) (gamma = Linear
    // Lighting's fog transparency gamma, Color::FogAlpha); the layer takes the same cap, clamp^gamma, and the same
    // half-cover distance d50 = near + (far - near) (clamp 2^(-1/gamma))^(1/power): sigma = ln 2 / (d50 - near).
    const auto& linearLighting = globals::features::linearLighting.settings;
    float gamma = linearLighting.enableLinearLighting ? linearLighting.fogAlphaGamma : 1.0f;
    if (!std::isfinite(gamma) || !(gamma > 0.0f))
        gamma = 1.0f;
    const double clampValue = std::isfinite(state.fogClamp) ? std::clamp(static_cast<double>(state.fogClamp), 0.0, 1.0) : 0.0;
    const double nearDistance = std::isfinite(state.fogNear) ? std::max(static_cast<double>(state.fogNear), 0.0) : 0.0;
    const double farDistance = state.fogFar;
    // Power 0 (or almost) is a wall at the near distance (NAT's night snow): held at 0.05, a few dozen units deep.
    const double power = std::isfinite(state.fogPower) ? std::max(static_cast<double>(state.fogPower), 0.05) : 1.0;
    if (!(clampValue > 0.0) || !(farDistance > nearDistance + 1.0)) {
        // Nothing the layer could match (no fog on screen, or far not beyond near): the height fog alone.
        state.weight = 0.0f;
        state.reason = "weather fog has no visible amount";
        return state;
    }
    const double halfRatio = std::pow(std::clamp(clampValue * std::exp2(-1.0 / gamma), 1e-6, 1.0), 1.0 / power);
    const double halfDistance = std::max(nearDistance + (farDistance - nearDistance) * halfRatio, nearDistance + 64.0);
    state.gamma = gamma;
    state.screenCap = static_cast<float>(std::pow(clampValue, static_cast<double>(gamma)));
    state.halfDistance = static_cast<float>(halfDistance);
    state.sigma = static_cast<float>(std::numbers::ln2 / (halfDistance - nearDistance));
    state.takeover = std::isfinite(state.sigma) && state.sigma > 0.0f && state.screenCap > 0.0f;
    if (!state.takeover) {
        state.weight = 0.0f;
        state.reason = "weather fog has no visible amount";
    }
    return state;
}

ExponentialHeightFog::WeatherFogData ExponentialHeightFog::GetWeatherFogData() const
{
    WeatherFogData data{};
    data.ScaleHeight = std::clamp(vanillaFog.TakeoverScaleHeight, kMinTakeoverScaleHeight, kMaxTakeoverScaleHeight);
    const auto state = GetVanillaFogState();
    if (state.takeover) {
        data.Sigma = state.sigma;
        data.Cap = std::clamp(state.weight * state.screenCap, 0.0f, 1.0f);
        data.NearDistance = std::max(state.fogNear, 0.0f);
    }
    return data;
}

bool ExponentialHeightFog::WeatherLayerJumped(const WeatherFogData& a_previous, const WeatherFogData& a_current)
{
    // Engine weather transitions move these a little per frame; a forced weather or a mode switch moves them at once.
    if ((a_previous.Cap > 0.0f) != (a_current.Cap > 0.0f))
        return true;
    if (!(a_current.Cap > 0.0f))
        return false;
    if (std::abs(a_current.Cap - a_previous.Cap) > 0.05f)
        return true;
    if (!(a_previous.Sigma > 0.0f) || std::abs(std::log(a_current.Sigma / a_previous.Sigma)) > 0.1f)
        return true;
    if (std::abs(a_current.ScaleHeight - a_previous.ScaleHeight) > 0.05f * a_previous.ScaleHeight)
        return true;
    return std::abs(a_current.NearDistance - a_previous.NearDistance) > 500.0f;
}

ExponentialHeightFog::Settings ExponentialHeightFog::GetCommonBufferData() const
{
    return settings;
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
    if (settings.fogDensity <= 0.0f && density2 <= 0.0f && !GetVanillaFogState().takeover)  // (47d) or the weather's fog layer
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
    if (settings.fogDensity <= 0.0f && GetCommonBufferData().fogDensity2 <= 0.0f && !GetVanillaFogState().takeover)
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
    // (47b) The game's fog under the height fog: written before any early return (the shaders read it whether or not
    // the volumes are built). Both 0 with the two switches off = the 47a2 fog exactly.
    // (47d) Blend Vanilla Fog = 47b exactly. CS Fog Takes Over also flags the weather's fog layer (SharedData::weatherFogData)
    // while it applies (outdoors, w > 0); the shaders then take the layer instead of the weight in pad0[0].
    const auto vanillaFogState = GetVanillaFogState();
    data.pad0[0] = vanillaFogState.weight;
    uint32_t fogFlags = (loaded && settings.enabled && vanillaFog.WeatherMode != kWeatherOff) ? kFogFlagOpacityFix : 0u;
    if (vanillaFogState.takeover)
        fogFlags |= kFogFlagTakeover;
    data.pad0[1] = std::bit_cast<float>(fogFlags);
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

    // (47e) Readout of the "Match Weather Fog Brightness" terms (3 x float4) and its staging ring.
    auto debugState = std::make_unique<VolumetricDebugState>();
    D3D11_BUFFER_DESC debugDesc{};
    debugDesc.ByteWidth = sizeof(float) * 12;
    debugDesc.Usage = D3D11_USAGE_DEFAULT;
    debugDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    debugDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    debugDesc.StructureByteStride = sizeof(float) * 4;
    DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&debugDesc, nullptr, debugState->buffer.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC debugUavDesc{};
    debugUavDesc.Format = DXGI_FORMAT_UNKNOWN;
    debugUavDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    debugUavDesc.Buffer.NumElements = 3;
    DX::ThrowIfFailed(globals::d3d::device->CreateUnorderedAccessView(debugState->buffer.get(), &debugUavDesc, debugState->uav.put()));
    D3D11_BUFFER_DESC stagingDesc = debugDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.MiscFlags = 0;
    stagingDesc.StructureByteStride = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& staging : debugState->staging)
        DX::ThrowIfFailed(globals::d3d::device->CreateBuffer(&stagingDesc, nullptr, staging.put()));
    volumetricDebug = std::move(debugState);
}

void ExponentialHeightFog::ReadVolumetricDebug()
{
    // (47e) Copy this frame's readout into the ring and read the oldest copy without waiting for the GPU.
    if (!volumetricDebug)
        return;
    auto& debug = *volumetricDebug;
    auto context = globals::d3d::context;
    const uint32_t count = static_cast<uint32_t>(std::size(debug.staging));
    context->CopyResource(debug.staging[debug.frame % count].get(), debug.buffer.get());
    debug.frame++;
    if (debug.frame < count)
        return;
    auto* oldest = debug.staging[debug.frame % count].get();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(context->Map(oldest, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) && mapped.pData) {
        const auto* values = static_cast<const float*>(mapped.pData);
        std::copy_n(values, 4, debug.weather);
        std::copy_n(values + 4, 4, debug.sun);
        std::copy_n(values + 8, 4, debug.sky);
        debug.valid = true;
        context->Unmap(oldest, 0);
    }
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
    ID3D11ShaderResourceView* depthSrv = terrainBlending.loaded && terrainBlending.blendedDepthTexture16 ? terrainBlending.blendedDepthTexture16->srv.get() : depthStencil.depthSRV;
    const bool hasIBL = ibl.loaded && ibl.settings.EnableDiffuseIBL && ibl.diffuseIBLTexture && ibl.diffuseSkyIBLTexture;
    const bool hasSkylighting = skylighting.loaded && skylighting.texProbeArray;

    const bool temporalReprojection = Util::GetTemporal();
    const bool frameContinuous = lastPrepassFrame != UINT32_MAX && globals::state->frameCount == lastPrepassFrame + 1u;
    // (47d) A sudden change of the weather's fog layer (forced weather, mode switch): start the history over.
    const auto weatherFog = GetWeatherFogData();
    const bool weatherJumped = WeatherLayerJumped(lastVolumeWeather, weatherFog);
    lastVolumeWeather = weatherFog;
    const bool temporalHistoryValid = temporalReprojection && hasLightScatteringHistory && frameContinuous && !weatherJumped;
    const bool temporalHistoryValidFar = temporalReprojection && hasLightScatteringFarHistory && frameContinuous && !weatherJumped;

    const auto p = ComputeGridParams(currentGridSize.z, currentFarGridSize.z);

    const uint32_t commonFlags =
        (hasShadowMap ? 1u : 0u) |
        (depthSrv ? 2u : 0u) |
        (hasIBL ? 4u : 0u) |
        (hasSkylighting ? 8u : 0u);

    VolumetricFogCB cb{};
    cb.gridSizeAndFlags = { currentGridSize.x, currentGridSize.y, currentGridSize.z,
        commonFlags | (depthSrv && temporalHistoryValid && hasConservativeDepthHistory ? 16u : 0u) | (hasLocalLights ? 32u : 0u) };
    // (47e) While the weather's fog layer is in the volume the near fade-in stays off: it would thin the layer in front of
    // the camera, which the game's fog (and the analytical layer) never does.
    const bool nearFadeIn = volumetric.NearFadeInDistance > 0.0f && !(weatherFog.Cap > 0.0f);
    const float nearFadeInv = nearFadeIn ? 1.0f / volumetric.NearFadeInDistance : 100000000.0f;
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
    {
        // (47e) "Match Weather Fog Brightness": the weather's fog colours and distances as ISSAOComposite gets them
        // (FogNearColor / FogFarColor / FogParam). Only used while the layer is in the volume (sigma_w > 0).
        const auto* sky = globals::game::sky;
        const auto vanillaFogState = GetVanillaFogState();
        const bool matchBrightness = volumetric.MatchWeatherFogBrightness && vanillaFogState.takeover && sky && weatherFog.Cap > 0.0f;
        if (matchBrightness) {
            const auto& nearColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogNear];
            const auto& farColor = sky->skyColor[RE::TESWeather::ColorTypes::kFogFar];
            const float fogNear = std::max(vanillaFogState.fogNear, 0.0f);
            const float invRange = 1.0f / std::max(vanillaFogState.fogFar - fogNear, 1.0f);
            cb.weatherNearColor = { nearColor.red, nearColor.green, nearColor.blue, 1.0f };
            cb.weatherFarColor = { farColor.red, farColor.green, farColor.blue, std::max(vanillaFogState.fogPower, 0.05f) };
            cb.weatherFogParams = { fogNear * invRange, invRange, std::clamp(vanillaFogState.fogClamp, 0.0f, 1.0f), 0.0f };
            for (auto* value : { &cb.weatherNearColor.x, &cb.weatherNearColor.y, &cb.weatherNearColor.z, &cb.weatherFarColor.x, &cb.weatherFarColor.y,
                     &cb.weatherFarColor.z, &cb.weatherFarColor.w, &cb.weatherFogParams.x, &cb.weatherFogParams.y, &cb.weatherFogParams.z }) {
                if (!std::isfinite(*value))
                    *value = 0.0f;
            }
        }
        // Readout froxel: the near-grid slice at ~100 m (7000 units), screen centre.
        const double readoutDepth = std::min(7000.0, p.nearEnd);
        const double readoutSlice = std::log2(std::max(readoutDepth * p.nearZ.x + p.nearZ.y, 1e-6)) * p.nearZ.z;
        cb.weatherFogParams.w = static_cast<float>(std::clamp(std::floor(readoutSlice), 0.0, static_cast<double>(currentGridSize.z - 1)));
    }
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
        bool debugReadout;  // (47e)
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
            // u1: (47e) the readout froxel (near grid only; the far shader has no u1).
            ID3D11UnorderedAccessView* uavs[2]{ a_pass.scattering->uav.get(), a_pass.debugReadout && volumetricDebug ? volumetricDebug->uav.get() : nullptr };
            ID3D11UnorderedAccessView* nullUavs[2]{};
            context->CSSetShaderResources(0, 5, srvs);
            context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            context->CSSetShader(a_pass.lightScatteringCS, nullptr, 0);
            context->Dispatch(groupX, groupY, groupZ);
            context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
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
        Util::GpuBucket::VolumetricFogNear, true });

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
            Util::GpuBucket::VolumetricFogFar, false });
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
    ReadVolumetricDebug();
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

    // (47b) The game's own fog under the height fog.
    ImGui::SeparatorText("Game Fog");
    {
        // (47d) Off = 47a2, Blend Vanilla Fog = 47b, CS Fog Takes Over = the weather's fog as a layer of the height fog.
        static constexpr const char* kWeatherModeNames[kWeatherModeCount] = { "Off", "Blend Vanilla Fog", "CS Fog Takes Over" };
        int weatherMode = static_cast<int>(std::min<uint32_t>(vanillaFog.WeatherMode, kWeatherModeCount - 1));
        if (ImGui::Combo("EHF Follows Weather", &weatherMode, kWeatherModeNames, static_cast<int>(kWeatherModeCount)))
            vanillaFog.WeatherMode = static_cast<uint32_t>(weatherMode);
    }
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Outdoors: how foggy weather keeps its fog. None of it comes back in clear or cloudy weather, some in\n"
            "overcast and rain, all of it in fog, storms and blizzards.\n"
            "  Off: as before 47b (the height fog replaces the weather's fog).\n"
            "  Blend Vanilla Fog: the game's own flat fog comes back under the height fog (47b).\n"
            "  CS Fog Takes Over (default): the weather's fog becomes part of the height fog, as thick as the game's\n"
            "  on screen but thinning above your height (mountains rise out of it), the sky fades with height, the sun\n"
            "  glow and the sunlight on the ground dim in it, and Volumetric Fog (if you turned it on) carries it too.\n"
            "Every mode but Off also fixes the height fog's colour being dimmed twice. The distant physical-sky haze\n"
            "steps back behind the weather's fog.");
    ImGui::Checkbox("EHF Restores Room Fog", &vanillaFog.RestoreRoomFog);
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Indoors. On: caves, dungeons and houses keep their own fog, with the height fog on top.\nOff = as before (the height fog replaces it).");
    {
        const auto state = GetVanillaFogState();
        if (state.hasWeather)
            ImGui::Text("Weather fog: near %.0f, far %.0f, power %.2f, max %.2f -> weight %.2f", state.fogNear, state.fogFar, state.fogPower, state.fogClamp, state.weight);
        else
            ImGui::Text("Game fog weight %.2f%s%s", state.weight, *state.reason ? " - " : "", state.reason);
        if (state.takeover)  // (47d) the layer as the shaders get it; 70 game units ~ 1 m
            ImGui::Text("Takeover layer: sigma %.2e / unit, cap %.2f (x weight %.2f), half cover at %.0f m", state.sigma, state.screenCap,
                state.weight * state.screenCap, state.halfDistance / 70.0f);
        else if (state.hasWeather && vanillaFog.WeatherMode == kWeatherCSFogTakesOver && *state.reason)
            ImGui::Text("Takeover layer: none - %s", state.reason);
    }
    if (ImGui::TreeNode("Advanced##GameFog")) {
        ImGui::SliderFloat("Clear Fog Distance", &vanillaFog.ClearFogFar, 20000.0f, 400000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Weather fog distance (far) at and above which none of the weather's fog comes back. Default 120000.");
        ImGui::SliderFloat("Full Fog Distance", &vanillaFog.FullFogFar, 1000.0f, 100000.0f, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text("Weather fog distance at and below which all of it comes back. Default 15000.");
        ImGui::SliderFloat("Takeover Thinning Height", &vanillaFog.TakeoverScaleHeight, kMinTakeoverScaleHeight, kMaxTakeoverScaleHeight, "%.0f", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_Logarithmic);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "CS Fog Takes Over: above your own height the weather's fog thins out by about two thirds every this\n"
                "many game units (70 units ~ 1 m). Lower = mountains and the sky come out of the fog sooner.\n"
                "Default 21000 (~300 m).");
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
            ImGui::Text("The 3D fog fades in over this distance in front of the camera.\nNot while CS Fog Takes Over has the weather's fog in it (the game's fog does not fade in).");
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
        // (47e)
        ImGui::Checkbox("Match Weather Fog Brightness", &volumetric.MatchWeatherFogBrightness);
        if (auto _tt = Util::HoverTooltipWrapper())
            ImGui::Text(
                "Only with \"EHF Follows Weather\" = CS Fog Takes Over, outdoors in foggy weather.\n"
                "On (default): the weather's fog inside the 3D fog is as bright as the game's own fog. The weather's\n"
                "fog colour already holds the light bounced around inside the fog; the 3D fog only lit it once, with\n"
                "the sun mostly taken out by the fog above, so it came out about half as bright (looked thinner).\n"
                "Lit fog now matches Volumetric Fog off, plus the glow toward the sun; fog in shadow keeps the sun's\n"
                "share out, so shafts stay. With this on the Scattering Intensities can go back to about 1.\n"
                "Off: as in 47d.");
        {
            const bool built = lastBuildFrame != UINT32_MAX && globals::state->frameCount - lastBuildFrame <= 1u;
            if (built && volumetricDebug && volumetricDebug->valid && volumetricDebug->weather[3] > 0.0f) {
                const auto& d = *volumetricDebug;
                ImGui::Text("At screen centre, %.0f m: weather fog %.3f %.3f %.3f | sun E*Ts/4pi %.3f %.3f %.3f | sky S %.3f %.3f %.3f | added %.0f%% of fog",
                    d.sun[3] / 70.0f, d.weather[0], d.weather[1], d.weather[2], d.sun[0], d.sun[1], d.sun[2], d.sky[0], d.sky[1], d.sky[2], d.sky[3] * 100.0f);
            } else if (built && volumetricDebug && volumetricDebug->valid) {
                ImGui::TextDisabled("Readout: no extra source at screen centre (switch off, clear weather, other mode, indoors, or something closer than 100 m).");
            }
        }
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
