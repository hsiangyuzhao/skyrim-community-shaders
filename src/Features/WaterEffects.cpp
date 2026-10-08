#include "WaterEffects.h"

#include <DDSTextureLoader.h>

#include "Features/DynamicCubemaps.h"
#include "Utils/Batch39Engine.h"
#include "Utils/UI.h"

void WaterEffects::SetupResources()
{
	auto device = globals::d3d::device;
	auto context = globals::d3d::context;

	DirectX::CreateDDSTextureFromFile(device, context, L"Data\\Shaders\\WaterEffects\\watercaustics.dds", nullptr, causticsView.put());
}

void WaterEffects::Prepass()
{
	auto context = globals::d3d::context;
	auto srv = causticsView.get();
	context->PSSetShaderResources(65, 1, &srv);
}

bool WaterEffects::HasShaderDefine(RE::BSShader::Type)
{
	return true;
}

void WaterEffects::DrawSettings()
{
	ImGui::Checkbox("Upstream fix: water ripple parallax", (bool*)&settings.UpstreamFixParallax);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Water ripple depth now starts from the real surface height, picks texture detail correctly\n"
			"and computes the height in the right order: less ripple misalignment and 'floating' look. Off = old behaviour.");
	ImGui::Checkbox("Upstream fix: blown-out sun glint on water", (bool*)&settings.UpstreamFixSunSpecular);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The sun's reflection on water could blow out to pure white. On: brightness rolls off smoothly (a bit dimmer). Off = old behaviour.");

	// The game's water reflection cubemap. Always on: one face per frame (a whole cube every 6
	// frames) and no distant (LOD) trees in it. Applied in Batch39Engine; saved here.
	ImGui::SeparatorText("Reflection Cubemap");
	auto& refl = Batch39Engine::settings;
	ImGui::Checkbox("Leave Distant Objects out of Water Reflections", &refl.ReflSkipLODObjects);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"Off by default. Sets bReflectLODObjects = 0 while on: cheaper water reflections, but distant\n"
			"mountains and buildings visibly disappear from the reflection.");
	const bool dcLoaded = globals::features::dynamicCubemaps.loaded;
	{
		auto disabled = Util::DisableGuard(!dcLoaded);
		ImGui::Checkbox("Hand Water Reflections to Dynamic Cubemaps", &refl.ReflHandOffToDynamicCubemaps);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Off by default. The game stops drawing its own water cubemap (it keeps only the sky) and water\n"
				"uses Dynamic Cubemaps at every distance, refreshed every frame. Cheaper, but reflections lag\n"
				"a little behind when the view changes quickly.%s",
				dcLoaded ? "" : "\nNeeds Dynamic Cubemaps.");
		if (refl.ReflHandOffToDynamicCubemaps) {
			ImGui::Indent();
			ImGui::SliderFloat("New Capture Weight", &refl.ReflHandOffCaptureWeight, 0.25f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Share of each new capture in the reflection. Higher = less lag, more flicker. Dynamic Cubemaps' own value is 0.5.");
			ImGui::Unindent();
		}
	}
}

void WaterEffects::LoadSettings(json& o_json)
{
	settings = {};
	auto& refl = Batch39Engine::settings;
	refl = {};
	if (!o_json.is_object())
		return;
	settings.UpstreamFixParallax = o_json.value("UpstreamFixParallax", true) ? 1u : 0u;
	settings.UpstreamFixSunSpecular = o_json.value("UpstreamFixSunSpecular", true) ? 1u : 0u;
	if (const auto it = o_json.find("ReflectionSkipLODObjects"); it != o_json.end() && it->is_boolean())
		refl.ReflSkipLODObjects = it->get<bool>();
	if (const auto it = o_json.find("ReflectionHandOffToDynamicCubemaps"); it != o_json.end() && it->is_boolean())
		refl.ReflHandOffToDynamicCubemaps = it->get<bool>();
	if (const auto it = o_json.find("ReflectionHandOffCaptureWeight"); it != o_json.end() && it->is_number())
		refl.ReflHandOffCaptureWeight = std::clamp(it->get<float>(), 0.25f, 1.0f);
}

void WaterEffects::SaveSettings(json& o_json)
{
	o_json["UpstreamFixParallax"] = settings.UpstreamFixParallax != 0;
	o_json["UpstreamFixSunSpecular"] = settings.UpstreamFixSunSpecular != 0;
	const auto& refl = Batch39Engine::settings;
	o_json["ReflectionSkipLODObjects"] = refl.ReflSkipLODObjects;
	o_json["ReflectionHandOffToDynamicCubemaps"] = refl.ReflHandOffToDynamicCubemaps;
	o_json["ReflectionHandOffCaptureWeight"] = refl.ReflHandOffCaptureWeight;
}

void WaterEffects::RestoreDefaultSettings()
{
	settings = {};
	Batch39Engine::settings = {};
}
