#include "ScreenSpacePointLightShadows.h"

#include "JiayeStatement.h"
#include "RayTracing.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	ScreenSpacePointLightShadows::Settings,
	Enable,
	Strength,
	StepLimit,
	RayLength,
	CompareToleranceScale,
	MaxDistance,
	EnableSoftShadows,
	SoftShadowScale,
	MaxShadowedLights)

void ScreenSpacePointLightShadows::DrawSettings()
{
	ImGui::Checkbox("Enable Screen Space Point Light Shadows", (bool*)&settings.Enable);

	ImGui::Spacing();
	ImGui::Spacing();

	ImGui::SliderFloat("Strength", &settings.Strength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How dark these shadows are. 0 = none, 1 = full.");
	ImGui::SliderInt("Max Step", (int*)&settings.StepLimit, 1, 64, "%d", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Search steps per pixel (fewer farther away). Higher = more accurate shadows but slower.");
	ImGui::SliderFloat("Ray Length", &settings.RayLength, 0.0f, 1.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How much of the way toward the light is searched for blockers. Higher = longer shadows. 0 = off.");
	ImGui::SliderFloat("Compare Tolerance Scale", &settings.CompareToleranceScale, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How thick objects are assumed to be when blocking light. Higher = fuller shadows, but more false shadows.");
	ImGui::SliderFloat("Max Distance", &settings.MaxDistance, 0.0f, 8192.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Shadows fade out toward this distance from the camera (game units). Lower = cheaper.");

	ImGui::Spacing();
	// (47k) Cost
	ImGui::SeparatorText("Cost");
	{
		int maxLights = static_cast<int>(std::clamp(settings.MaxShadowedLights, 1u, 8u));
		if (ImGui::SliderInt("Max Shadowed Lights per Pixel", &maxLights, 1, 8, maxLights >= 8 ? "unlimited" : "%d", ImGuiSliderFlags_AlwaysClamp))
			settings.MaxShadowedLights = static_cast<uint>(maxLights);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Each pixel searches screen-space shadows only for its brightest few point lights (brightness x distance falloff x");
			ImGui::Text("  how much it faces the light); the others still light it, just without these shadows (like the small \"simple\"");
			ImGui::Text("  lights). The last one fades in and out smoothly, so a light dropping out of the set does not leave a seam.");
			ImGui::Text("  Default 3. 8 = unlimited (every light, as in 47j). Rooms with many candles / braziers are much cheaper.");
		}
		// (47o) Skip on Translucent Surfaces (47h), Skip Weak Lights, Smarter Steps and Skip SSPLS on Water (47k) are permanent.
		ImGui::TextDisabled("Max Step: the default is 4; 8 costs about 5 ms more in rooms with many lights.");
	}
	ImGui::Spacing();

	ImGui::Checkbox("Enable Soft Shadows", (bool*)&settings.EnableSoftShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Blurs the shadow edges based on the light's size.");
	ImGui::SliderFloat("Soft Shadow Scale", &settings.SoftShadowScale, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How soft the edges get with Enable Soft Shadows. Higher = softer.");

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

std::string ScreenSpacePointLightShadows::DescribeForTrace() const
{
	if (!loaded)
		return "SSPLS not loaded";
	const auto& s = settings;
	const uint maxLights = std::clamp(s.MaxShadowedLights, 1u, 8u);
	return std::format("SSPLS {} (strength {:.2f}, step limit {}, ray length {:.2f}, compare tolerance {:.2f}, max distance {:.0f}, soft shadows {} x{:.2f}; "
					   "(47k) max shadowed lights per pixel {}; skip translucent / weak lights / smarter steps / skip on water permanent (47o))",
		s.Enable ? "on" : "off", s.Strength, s.StepLimit, s.RayLength, s.CompareToleranceScale, s.MaxDistance, s.EnableSoftShadows ? "on" : "off", s.SoftShadowScale,
		maxLights >= 8u ? std::string("unlimited") : std::to_string(maxLights));
}

void ScreenSpacePointLightShadows::RestoreDefaultSettings()
{
	settings = {};
}

void ScreenSpacePointLightShadows::LoadSettings(json& o_json)
{
	settings = o_json;
	settings.MaxShadowedLights = std::clamp(settings.MaxShadowedLights, 1u, 8u);  // (47k)
}

void ScreenSpacePointLightShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

ScreenSpacePointLightShadows::PerFrame ScreenSpacePointLightShadows::GetCommonBufferData()
{
	PerFrame data = {
		// (45) Fixed Math / Stable Noise travel as bits of Enable (see SSPLS.hlsli SettingFlags); (45o) always on.
		// (46y) bit 3: Ray Tracing debug view SSPLS Coverage (drawn by Lighting.hlsl; only while SSPLS is on).
		// (47h) bit 4 Skip on Translucent Surfaces, (47k) bits 5-7 Skip Weak Lights, Smarter Steps, Skip SSPLS on Water: (47o) permanent in
		// the shaders, still set (reserved); bits 8-11: Max Shadowed Lights per Pixel (0 = unlimited).
		.Enable = settings.Enable ? (1u | 2u | 4u | (globals::features::rayTracing.SSPLSCoverageView() ? 8u : 0u) | 16u | 32u | 64u | 128u |
										((settings.MaxShadowedLights >= 1u && settings.MaxShadowedLights < 8u) ? (settings.MaxShadowedLights << 8) : 0u)) :
		                            0u,
		.Strength = settings.Strength,
		.StepLimit = settings.StepLimit,
		.RayLength = settings.RayLength,
		.CompareToleranceScale = settings.CompareToleranceScale,
		.MaxDistance = settings.MaxDistance,
		.EnableSoftShadows = settings.EnableSoftShadows,
		.SoftShadowScale = settings.SoftShadowScale
	};
	return data;
}
