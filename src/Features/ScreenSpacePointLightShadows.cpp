#include "ScreenSpacePointLightShadows.h"

#include "JiayeStatement.h"
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
	FixedMath,
	StableNoise)

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
	ImGui::Checkbox("Enable Soft Shadows", (bool*)&settings.EnableSoftShadows);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Blurs the shadow edges based on the light's size.");
	ImGui::SliderFloat("Soft Shadow Scale", &settings.SoftShadowScale, 0.0f, 50.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How soft the edges get with Enable Soft Shadows. Higher = softer.");

	ImGui::Spacing();
	ImGui::Checkbox("Fixed Math", (bool*)&settings.FixedMath);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Corrected shadow search: more steps only make shadows more accurate instead of changing their shape, no speckles on the surface itself, smoother soft edges, smooth fade with distance, and lights that already cast real shadows only add short contact shadows. Off = the old behaviour.");
	ImGui::Checkbox("Stable Noise", (bool*)&settings.StableNoise);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Calmer, finer grain that changes a little every frame, so DLSS can smooth it out. Off = the old 16-frame noise pattern.");

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void ScreenSpacePointLightShadows::RestoreDefaultSettings()
{
	settings = {};
}

void ScreenSpacePointLightShadows::LoadSettings(json& o_json)
{
	settings = o_json;
}

void ScreenSpacePointLightShadows::SaveSettings(json& o_json)
{
	o_json = settings;
}

ScreenSpacePointLightShadows::PerFrame ScreenSpacePointLightShadows::GetCommonBufferData()
{
	PerFrame data = {
		// (45) FixedMath / StableNoise travel as bits of Enable (see SSPLS.hlsli SettingFlags); 0 = off.
		.Enable = settings.Enable ? (1u | (settings.FixedMath ? 2u : 0u) | (settings.StableNoise ? 4u : 0u)) : 0u,
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
