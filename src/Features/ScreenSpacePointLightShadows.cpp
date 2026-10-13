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
	SoftShadowScale)

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
		.Enable = settings.Enable,
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
