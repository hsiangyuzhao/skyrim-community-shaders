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
	ImGui::SliderFloat("LOD Object Brightness", &settings.LODObjectBrightness, 0.01f, 5.f, "%.2f");
	ImGui::SliderFloat("LOD Object Snow Brightness", &settings.LODObjectSnowBrightness, 0.01f, 5.f, "%.2f");
	ImGui::SliderFloat("LOD Terrain Gamma", &settings.LODTerrainGamma, 0.1f, 3.f, "%.2f");
	ImGui::SliderFloat("LOD Object Gamma", &settings.LODObjectGamma, 0.1f, 3.f, "%.2f");
	ImGui::SliderFloat("LOD Object Snow Gamma", &settings.LODObjectSnowGamma, 0.1f, 3.f, "%.2f");
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
			"Brightness of distant grass LOD, as a gamma curve on its diffuse texture. "
			"Lower is brighter: 0.40 lifts a mid-tone by roughly 2x, 0.30 by roughly 2.4x.\n\n"
			"Distant grass needs a control of its own because the engine never tags it as LOD "
			"geometry, so LOD Object Brightness and LOD Object Gamma cannot reach it -- their "
			"shader code is not even compiled into the permutations grass LOD draws with. "
			"Merged grass LOD is also darkened where it is generated (DynDOLOD's "
			"ComplexGrassBrightness, default 0.5), which is what this compensates for.\n\n"
			"Gamma rather than a multiplier on purpose: gamma cannot lift a value above 1, so it "
			"cannot blow out highlights, and it cannot feed an albedo above 1 into screen-space "
			"GI, which would be creating light out of nothing.");
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
			"How a draw is recognised as grass LOD. Both judgements are counted every frame "
			"regardless of which one is selected, so the readout below tells you whether the "
			"other one would have worked.\n\n"
			"The default rests on a naming convention, which is why the alternative exists: it "
			"keys off merged LOD having no object reference behind it and the material being "
			"back-lit, and so does not care what the LOD generator names its shapes.");
	}

	ImGui::Text("Detected this frame -- name: %u draws, fallback: %u draws",
		grassNameHitsLastFrame, grassFallbackHitsLastFrame);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Both counts are taken every frame, whichever mode is selected.\n\n"
			"If the selected mode reads 0 while the other reads more than 0, switch modes. "
			"If both read 0 in an exterior with grass LOD in view, the draws are not reaching "
			"this hook at all and the gamma slider cannot work -- that is a different bug from "
			"the slider having no visible effect.");
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