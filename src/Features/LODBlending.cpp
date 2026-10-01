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
	LODGrassGamma)

namespace
{
	constexpr std::array GrassDetectionNames{
		"Shape name is \"grasspassthru\"",
		"No user data and back-lit"
	};
	static_assert(GrassDetectionNames.size() == static_cast<std::size_t>(LODBlending::GrassDetection::Total));
}

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
	ImGui::Checkbox("Disable Terrain Vertex Colors", (bool*)&settings.DisableTerrainVertexColors);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Disables vertex coloring on nearby terrain. "
			"Best combined with terrain LOD generated in xLODGen with Vertex Color Intensity set to 0. ");
	}

	ImGui::SeparatorText("Distant grass");

	ImGui::SliderFloat("LOD Grass Gamma", &settings.LODGrassGamma, 0.1f, 3.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Brightness of distant grass LOD (DynDOLOD grass), which the LOD Object sliders don't affect. Lower = brighter:\n"
			"0.40 roughly doubles mid-tones. Use it to offset DynDOLOD darkening its grass LOD; it can't blow out highlights.");
	}

	{
		int mode = static_cast<int>(grassDetection);
		if (ImGui::Combo("Grass LOD Detection", &mode, GrassDetectionNames.data(),
				static_cast<int>(GrassDetectionNames.size()))) {
			grassDetection = static_cast<GrassDetection>(std::clamp(mode, 0,
				static_cast<int>(GrassDetectionNames.size()) - 1));
		}
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"How distant grass LOD is recognised for the slider above. Both normally pick the same grass.\n"
			"Switch to the second one if your LOD generator names things differently and the slider stops working.");
	}
}

void LODBlending::LoadSettings(json& o_json)
{
	settings = o_json;

	// Not part of Settings because it never reaches the GPU: the hook alone acts on it, and
	// keeping it out of the struct is what leaves the shared feature buffer layout untouched.
	if (o_json.contains("GrassDetection")) {
		const auto mode = o_json["GrassDetection"].get<uint>();
		grassDetection = static_cast<GrassDetection>(
			std::min(mode, static_cast<uint>(GrassDetection::Total) - 1));
	}
}

void LODBlending::SaveSettings(json& o_json)
{
	o_json = settings;
	o_json["GrassDetection"] = static_cast<uint>(grassDetection);
}

void LODBlending::RestoreDefaultSettings()
{
	settings = {};
	grassDetection = GrassDetection::Name;
}