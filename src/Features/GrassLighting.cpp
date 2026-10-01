#include "GrassLighting.h"

#include "GrassOptimizations.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	GrassLighting::Settings,
	Glossiness,
	SpecularStrength,
	SubsurfaceScatteringAmount,
	OverrideComplexGrassSettings,
	BasicGrassBrightness,
	MidLODBrightness,
	FarLODBrightness)

void GrassLighting::DrawSettings()
{
	if (ImGui::TreeNodeEx("Complex Grass", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextWrapped("Specular highlights for complex grass");
		ImGui::SliderFloat("Glossiness", &settings.Glossiness, 1.0f, 100.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Specular highlight glossiness.");
		}

		ImGui::SliderFloat("Specular Strength", &settings.SpecularStrength, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Specular highlight strength.");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("SSS Amount", &settings.SubsurfaceScatteringAmount, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Subsurface Scattering (SSS) amount. "
				"Soft lighting controls how evenly lit an object is. "
				"Back lighting illuminates the back face of an object. "
				"Combined to model the transport of light through the surface. ");
		}

		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Override Complex Grass Lighting Settings", (bool*)&settings.OverrideComplexGrassSettings);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Override the settings set by the grass mesh author. "
				"Complex grass authors can define the brightness for their grass meshes. "
				"However, some authors may not account for the extra lights available from Community Shaders. "
				"This option will treat their grass settings like non-complex grass. "
				"This was the default in Community Shaders < 0.7.0");
		}
		ImGui::Spacing();
		ImGui::Spacing();
		ImGui::TextWrapped("Basic Grass");
		ImGui::SliderFloat("Brightness", &settings.BasicGrassBrightness, 0.0f, 1.0f);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Darkens the grass textures to look better with the new lighting");
		}

		if (globals::features::grassOptimizations.loaded) {
			ImGui::Spacing();
			ImGui::TextWrapped("Middle LOD Grass");
			ImGui::PushID("midlod");
			ImGui::SliderFloat("Brightness", &settings.MidLODBrightness, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Raise or lower until middle-distance LOD grass matches the full-detail grass around it. Requires Grass Optimizations with Mesh LOD and Middle LOD enabled.");
			}
			ImGui::PopID();

			ImGui::Spacing();
			ImGui::TextWrapped("Far LOD Grass");
			ImGui::PushID("farlod");
			ImGui::SliderFloat("Brightness", &settings.FarLODBrightness, 0.0f, 2.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text("Raise or lower until the most distant LOD grass matches the middle LOD grass in front of it. Requires Grass Optimizations with Mesh LOD and Far LOD enabled.");
			}
			ImGui::PopID();
		}

		ImGui::TreePop();
	}
}

void GrassLighting::LoadSettings(json& o_json)
{
	settings = o_json;
}

void GrassLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void GrassLighting::RestoreDefaultSettings()
{
	settings = {};
}