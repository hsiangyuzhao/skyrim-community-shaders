#include "ExponentialHeightFog.h"

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
    cubemapMipLevel)

void ExponentialHeightFog::RestoreDefaultSettings()
{
    settings = {};
}

void ExponentialHeightFog::LoadSettings(json& o_json)
{
    settings = o_json;
}

void ExponentialHeightFog::SaveSettings(json& o_json)
{
    o_json = settings;
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
}