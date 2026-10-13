#include "LODBlending.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LODBlending::Settings,
	LODTerrainBrightness,
	LODObjectBrightness,
	LODObjectSnowBrightness,
	DisableTerrainVertexColors,
	LODTerrainGamma,
	LODObjectGamma,
	LODObjectSnowGamma,
	LODGrassGamma,
	LODGrassBrightness)

void LODBlending::DrawSettings()
{
	ImGui::SliderFloat("LOD Terrain Brightness", &settings.LODTerrainBrightness, 0.01f, 5.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of distant (LOD) terrain, to match it with nearby terrain. Higher = brighter.");
	ImGui::SliderFloat("LOD Object Brightness", &settings.LODObjectBrightness, 0.01f, 5.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of distant (LOD) objects such as buildings, rocks and mountains. Higher = brighter.");
	ImGui::SliderFloat("LOD Object Snow Brightness", &settings.LODObjectSnowBrightness, 0.01f, 5.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of snow on distant (LOD) objects. Higher = brighter.");
	ImGui::SliderFloat("LOD Terrain Gamma", &settings.LODTerrainGamma, 0.1f, 3.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Contrast curve for distant terrain colours. Lower = brighter (mostly the darker tones), higher = darker.");
	ImGui::SliderFloat("LOD Object Gamma", &settings.LODObjectGamma, 0.1f, 3.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Contrast curve for distant object colours. Lower = brighter (mostly the darker tones), higher = darker.");
	ImGui::SliderFloat("LOD Object Snow Gamma", &settings.LODObjectSnowGamma, 0.1f, 3.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Contrast curve for snow on distant objects. Lower = brighter (mostly the darker tones), higher = darker.");
	// (44h) Distant grass (DynDOLOD grass LOD, which the LOD Object sliders don't affect), in the main list.
	ImGui::SliderFloat("LOD Grass Brightness", &settings.LODGrassBrightness, 0.01f, 5.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of distant grass LOD (DynDOLOD grass). Higher = brighter.");
	ImGui::SliderFloat("LOD Grass Gamma", &settings.LODGrassGamma, 0.1f, 3.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Contrast curve for distant grass LOD colours. Lower = brighter (mostly the darker tones), higher = darker.\n"
			"0.40 roughly doubles mid-tones. Use it to offset DynDOLOD darkening its grass LOD; it can't blow out highlights.");
	}
	ImGui::Checkbox("Disable Terrain Vertex Colors", (bool*)&settings.DisableTerrainVertexColors);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Disables vertex coloring on nearby terrain. "
			"Best combined with terrain LOD generated in xLODGen with Vertex Color Intensity set to 0. ");
	}
}

void LODBlending::LoadSettings(json& o_json)
{
	settings = o_json;
}

void LODBlending::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LODBlending::RestoreDefaultSettings()
{
	settings = {};
}
