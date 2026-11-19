#include "LinearLighting.h"

#include "JiayeStatement.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	LinearLighting::Settings,
	enableLinearLighting,
	enableGammaCorrection,
	lightGamma,
	colorGamma,
	emitColorGamma,
	glowmapGamma,
	ambientGamma,
	fogGamma,
	fogAlphaGamma,
	effectGamma,
	effectAlphaGamma,
	skyGamma,
	waterGamma,
	vlGamma,
	vanillaDiffuseMult,
	vanillaSpecularMult,
	grassDiffuseMult,
	grassSpecularMult,
	vanillaDiffuseColorMult,
	lightMult,
	directionalLightMult,
	pointLightMult,
	emitColorMult,
	glowmapMult,
	effectLightingMult,
	membraneEffectMult,
	bloodEffectMult,
	projectedEffectMult,
	deferredEffectMult,
	otherEffectMult,
	effectsFollowScene,
	effectsFollowLit,
	effectsFollowUnlit,
	fireMagicEffectMult,
	litSmokeEffectMult,
	unlitSmokeEffectMult)

void LinearLighting::DrawSettings()
{
	ImGui::Checkbox("Enable Linear Lighting", (bool*)&settings.enableLinearLighting);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Does lighting maths in linear colour space (more physically correct light mixing). Every setting below only applies while this is on.");
	ImGui::Checkbox("Enable Gamma Correction", (bool*)&settings.enableGammaCorrection);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Converts the final image back to normal display brightness at the end. Leave on; off makes the image much darker.");
	ImGui::Text("Gamma Settings");
	ImGui::SliderFloat("Light Gamma", &settings.lightGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How light colours are converted for linear lighting. Higher = darker, more contrast.");
	ImGui::SliderFloat("Color Gamma", &settings.colorGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How material (texture) colours are converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Emissive Color Gamma", &settings.emitColorGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How glowing (emissive) colours are converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Glowmap Gamma", &settings.glowmapGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How glow map colours are converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Ambient Gamma", &settings.ambientGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How ambient light is converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Fog Gamma", &settings.fogGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How fog colour is converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Fog Transparency Gamma", &settings.fogAlphaGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How fog thickness is converted. Higher = thinner fog.");
	ImGui::SliderFloat("Effect Gamma", &settings.effectGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How effect colours (magic, fire, particles) are converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Effect Transparency Gamma", &settings.effectAlphaGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How effect transparency is converted. Higher = more see-through effects.");
	ImGui::SliderFloat("Sky Gamma", &settings.skyGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How sky colours are converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Water Gamma", &settings.waterGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How water colours are converted. Higher = darker, more contrast.");
	ImGui::SliderFloat("Volumetric Lighting Gamma", &settings.vlGamma, 0.1f, 3.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How volumetric light (light shafts) is converted. Higher = darker, more contrast.");

	ImGui::SeparatorText("Multipliers");
	ImGui::SliderFloat("Vanilla Diffuse Multiplier", &settings.vanillaDiffuseMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of sun and light shading on vanilla (non-PBR) materials.");
	ImGui::SliderFloat("Vanilla Specular Multiplier", &settings.vanillaSpecularMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Strength of highlights on vanilla (non-PBR) materials.");
	ImGui::SliderFloat("Grass Diffuse Multiplier", &settings.grassDiffuseMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of sun and light shading on grass.");
	ImGui::SliderFloat("Grass Specular Multiplier", &settings.grassSpecularMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Strength of highlights on grass.");
	ImGui::SliderFloat("Vanilla Diffuse Color Multiplier", &settings.vanillaDiffuseColorMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Scales vanilla texture colours. Higher = lighter surfaces.");
	ImGui::SliderFloat("Light Multiplier", &settings.lightMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of all lights (sun and point lights).");
	ImGui::SliderFloat("Directional Light Multiplier", &settings.directionalLightMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of the sun/moon light.");
	ImGui::SliderFloat("Point Light Multiplier", &settings.pointLightMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of point lights (torches, candles, spells).");
	ImGui::SliderFloat("Emissive Color Multiplier", &settings.emitColorMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of glowing (emissive) colours.");
	ImGui::SliderFloat("Glowmap Multiplier", &settings.glowmapMult, 0.0f, 10.0f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brightness of glow maps.");
	if (ImGui::TreeNodeEx("Effects", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SliderFloat("Effect Lighting Multiplier", &settings.effectLightingMult, 0.0f, 10.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"How strongly lights affect effects.\n"
				"With \"Effects Follow Scene Light\" > \"Lit Smoke and Mist\" on: a trim on the sun and point lights on lit smoke and mist "
				"(1 = exactly as on a matte surface).");
		ImGui::SliderFloat("Membrane Effects Multiplier", &settings.membraneEffectMult, 0.0f, 10.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Brightness of membrane effects (glowing shells on characters, e.g. magic armour).");
		ImGui::SliderFloat("Blood Effects Multiplier", &settings.bloodEffectMult, 0.0f, 10.0f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Brightness of blood effects.");
		ImGui::SliderFloat("Projected Effects Multiplier", &settings.projectedEffectMult, 0.0f, 10.0f, "%.2f");
		ImGui::SliderFloat("Deferred Effects Multiplier", &settings.deferredEffectMult, 0.0f, 10.0f, "%.2f");

		// (49g) Effects Follow Scene Light: one master, two parts, and three multipliers in place of "Other Effects Multiplier".
		ImGui::Checkbox("Effects Follow Scene Light", &settings.effectsFollowScene);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Smoke, mist, dust and spray get as bright as the scene around them: darker at night, in shade and indoors.\n"
				"Fire, magic, sparks and glows (effects that shine by themselves) are never changed; membrane, blood, projected "
				"and deferred effects keep their own multipliers above.\n"
				"On: the two parts below and three multipliers by kind of effect. Off: exactly the old effects, with one "
				"\"Other Effects Multiplier\" for all of them.");
		ImGui::Indent();
		ImGui::BeginDisabled(!settings.effectsFollowScene);
		ImGui::Checkbox("Lit Smoke and Mist", &settings.effectsFollowLit);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Effects that take light (their Lighting Influence is above 0): they get the light a plain matte surface would get "
				"at that spot - sky light, sunlight with its shadow (including ray-traced and volumetric shadows) and point lights, "
				"scaled like surfaces by \"Vanilla Diffuse Multiplier\". Before, they used the game's own light colour.");
		ImGui::Checkbox("Unlit Smoke and Mist", &settings.effectsFollowUnlit);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Effects that take no light (plain see-through fog and mist cards): scaled by how bright the sky light is now "
				"compared with this weather at midday, so they dim at night and indoors (never brighter than before).\n"
				"Not for effects coloured by a palette (usually flames and magic) or effects that darken what is behind them.");
		ImGui::EndDisabled();
		ImGui::Unindent();
		if (settings.effectsFollowScene) {
			ImGui::SliderFloat("Fire and Magic Multiplier", &settings.fireMagicEffectMult, 0.0f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brightness of effects that shine by themselves: fire, magic, sparks, glows and light sprites (additive effects), and unlit palette-coloured effects.");
			ImGui::SliderFloat("Lit Smoke and Mist Multiplier", &settings.litSmokeEffectMult, 0.0f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brightness of effects that take light (smoke, dust, spray with Lighting Influence).");
			ImGui::SliderFloat("Unlit Smoke and Mist Multiplier", &settings.unlitSmokeEffectMult, 0.0f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brightness of the remaining effects that take no light (fog and mist cards).");
			if (settings.effectsFollowUnlit)
				ImGui::TextDisabled("Unlit smoke and mist now: x%.2f (sky light now %.3f, this weather at midday %.3f)", unlitSceneScale, unlitAmbientNow,
					std::max(unlitAmbientMidday, 0.0f));
		} else {
			ImGui::SliderFloat("Other Effects Multiplier", &settings.otherEffectMult, 0.0f, 10.0f, "%.2f");
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("Brightness of all remaining effects (shown while \"Effects Follow Scene Light\" is off).");
		}
		ImGui::TreePop();
	}

	JiayeStatement::GetSingleton()->DrawJSInfo();
}

void LinearLighting::LoadSettings(json& o_json)
{
	settings = o_json;
	// (49g) Settings saved before the split: the three new multipliers start at the old Other Effects Multiplier.
	if (o_json.is_object()) {
		if (!o_json.contains("fireMagicEffectMult"))
			settings.fireMagicEffectMult = settings.otherEffectMult;
		if (!o_json.contains("litSmokeEffectMult"))
			settings.litSmokeEffectMult = settings.otherEffectMult;
		if (!o_json.contains("unlitSmokeEffectMult"))
			settings.unlitSmokeEffectMult = settings.otherEffectMult;
	}
}

void LinearLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void LinearLighting::RestoreDefaultSettings()
{
	settings = {};
}

void LinearLighting::SetupResources()
{
	PerGeometryCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PerGeometryData>());
	// (49g) Zero until the first Prepass: master off (the old effect lighting).
	EffectSceneCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<EffectSceneData>());
	EffectSceneCB->Update(EffectSceneData{});
}

void LinearLighting::BindEffectScene()
{
	ID3D11Buffer* buffer = EffectSceneCB ? EffectSceneCB->CB() : nullptr;
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
}

void LinearLighting::UnbindEffectScene()
{
	ID3D11Buffer* buffer = nullptr;
	globals::d3d::context->PSSetConstantBuffers(7, 1, &buffer);
}

float LinearLighting::ComputeUnlitSceneScale()
{
	// Ambient luminance as the shaders see it: the directional ambient's isotropic part (DALC x (0,0,0,1) = its translation, the mean of
	// its six colours) through Linear Lighting's ambient gamma. Midday: the same of the current weather's day colours (blended with the
	// previous weather during a transition, as PhysicalSky::GetAmbientProgress). The sun is left out: with Physical Sky's sun its
	// colour is not in the weather's units.
	const float gamma = std::isfinite(settings.ambientGamma) ? std::clamp(settings.ambientGamma, 0.1f, 5.0f) : 1.0f;
	auto luminance = [gamma](float r, float g, float b) {
		auto lin = [gamma](float v) { return std::pow(std::isfinite(v) ? std::max(v, 0.0f) : 0.0f, gamma); };
		return 0.2126f * lin(r) + 0.7152f * lin(g) + 0.0722f * lin(b);
	};

	const auto* smState = globals::game::smState;
	if (smState == nullptr)
		return 1.0f;
	const auto& t = smState->directionalAmbientTransform.translate;
	unlitAmbientNow = luminance(t.x, t.y, t.z);

	if (const auto* sky = globals::game::sky; sky != nullptr && sky->currentWeather != nullptr) {
		const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.0f, 1.0f) : 1.0f;
		const RE::TESWeather* last = (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.0f) ? sky->lastWeather : nullptr;
		auto meanDay = [](const RE::TESWeather* a_weather, int a_channel) {
			const auto& d = a_weather->directionalAmbientLightingColors[RE::TESWeather::ColorTime::kDay].directional;
			auto channel = [a_channel](const RE::Color& c) { return (a_channel == 0 ? c.red : a_channel == 1 ? c.green : c.blue) * (1.0f / 255.0f); };
			return (channel(d.x.max) + channel(d.x.min) + channel(d.y.max) + channel(d.y.min) + channel(d.z.max) + channel(d.z.min)) / 6.0f;
		};
		float rgb[3];
		for (int i = 0; i < 3; ++i)
			rgb[i] = last ? std::lerp(meanDay(last, i), meanDay(sky->currentWeather, i), pct) : meanDay(sky->currentWeather, i);
		const float midday = luminance(rgb[0], rgb[1], rgb[2]);
		if (midday > 1e-4f)
			unlitAmbientMidday = midday;  // kept for interiors without a weather
	}

	if (unlitAmbientMidday <= 0.0f || !std::isfinite(unlitAmbientNow))
		return 1.0f;
	return std::clamp(unlitAmbientNow / unlitAmbientMidday, kMinUnlitSceneScale, 1.0f);
}

void LinearLighting::Prepass()
{
	bool isMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));

	// (49g) Effects Follow Scene Light: this frame's effect constants (Effect.hlsl EffectSceneCB).
	if (EffectSceneCB) {
		EffectSceneData effectScene{};
		if (settings.enableLinearLighting && settings.effectsFollowScene && !isMainLoadingMenu) {
			effectScene.flags = 1u | (settings.effectsFollowLit ? 2u : 0u) | (settings.effectsFollowUnlit ? 4u : 0u);
			unlitSceneScale = settings.effectsFollowUnlit ? ComputeUnlitSceneScale() : 1.0f;
		} else {
			unlitSceneScale = 1.0f;
		}
		effectScene.fireMagicMult = settings.fireMagicEffectMult;
		effectScene.litSmokeMult = settings.litSmokeEffectMult;
		effectScene.unlitSmokeMult = settings.unlitSmokeEffectMult;
		effectScene.unlitSceneScale = unlitSceneScale;
		EffectSceneCB->Update(effectScene);
	}

	dirLightMult = 1.0f;
	if (!settings.enableLinearLighting || isMainLoadingMenu)
		return;

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	if (!imageSpaceManager)
		return;

	dirLightMult = !globals::game::isVR ? imageSpaceManager->GetRuntimeData().data.baseData.hdr.sunlightScale : imageSpaceManager->GetVRRuntimeData().data.baseData.hdr.sunlightScale;
}

struct LinearLighting::Hooks
{
	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags)
		{
			globals::features::linearLighting.BSLightingShader_SetupGeometry(Pass);
			func(This, Pass, RenderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};

	static void Install()
	{
		stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
		logger::info("[LinearLighting] Installed hooks - BSLightingShader_SetupGeometry");
	}
};

void LinearLighting::PostPostLoad()
{
	LinearLighting::Hooks::Install();
}

LinearLighting::PerFrameData LinearLighting::GetCommonBufferData()
{
	bool isMainLoadingMenu = globals::game::ui && (globals::game::ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || globals::game::ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
	auto data = PerFrameData{};
	data.enableLinearLighting = settings.enableLinearLighting && !isMainLoadingMenu;
	data.enableGammaCorrection = settings.enableGammaCorrection;
	data.isDirLightLinear = isDirLightLinear;
	data.dirLightMult = dirLightMult;
	data.lightGamma = settings.lightGamma;
	data.colorGamma = settings.colorGamma;
	data.emitColorGamma = settings.emitColorGamma;
	data.glowmapGamma = settings.glowmapGamma;
	data.ambientGamma = settings.ambientGamma;
	data.fogGamma = settings.fogGamma;
	data.fogAlphaGamma = settings.fogAlphaGamma;
	data.effectGamma = settings.effectGamma;
	data.effectAlphaGamma = settings.effectAlphaGamma;
	data.skyGamma = settings.skyGamma;
	data.waterGamma = settings.waterGamma;
	data.vlGamma = settings.vlGamma;
	data.vanillaDiffuseMult = settings.vanillaDiffuseMult;
	data.vanillaSpecularMult = settings.vanillaSpecularMult;
	data.grassDiffuseMult = settings.grassDiffuseMult;
	data.grassSpecularMult = settings.grassSpecularMult;
	data.vanillaDiffuseColorMult = settings.vanillaDiffuseColorMult;
	data.lightMult = settings.lightMult;
	data.directionalLightMult = settings.directionalLightMult;
	data.pointLightMult = settings.pointLightMult;
	data.emitColorMult = settings.emitColorMult;
	data.glowmapMult = settings.glowmapMult;
	data.effectLightingMult = settings.effectLightingMult;
	data.membraneEffectMult = settings.membraneEffectMult;
	data.bloodEffectMult = settings.bloodEffectMult;
	data.projectedEffectMult = settings.projectedEffectMult;
	data.deferredEffectMult = settings.deferredEffectMult;
	data.otherEffectMult = settings.otherEffectMult;
	return data;
}

RE::NiColor LinearLighting::ColorToLinear(RE::NiColor inColor, float gamma)
{
	RE::NiColor outColor;
	outColor.red = std::pow(inColor.red, gamma);
	outColor.green = std::pow(inColor.green, gamma);
	outColor.blue = std::pow(inColor.blue, gamma);
	return outColor;
}

void LinearLighting::BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass)
{
	auto& property1 = a_pass->geometry->GetGeometryRuntimeData().properties[1];
	auto lightProperty = property1 && property1->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ? static_cast<RE::BSLightingShaderProperty*>(property1.get()) : nullptr;

	if (lightProperty != nullptr) {
		float emissiveMult = 1.0f;
		if (settings.enableLinearLighting) {
			emissiveMult = lightProperty->emissiveMult;
			PerGeometryData perGeometryData{};
			perGeometryData.emissiveMult = emissiveMult;
			PerGeometryCB->Update(perGeometryData);

			ID3D11Buffer* buffer = { PerGeometryCB->CB() };
			auto context = globals::d3d::context;
			context->PSSetConstantBuffers(8, 1, &buffer);
		}
	}
}