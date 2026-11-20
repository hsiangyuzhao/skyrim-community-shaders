#include "PhysicalBrightness.h"

#include <numbers>

#include "Features/InverseSquareLighting.h"
#include "Features/LinearLighting.h"
#include "Features/PhysicalSky.h"
#include "Features/PostProcessing.h"
#include "State.h"
#include "Util.h"
#include "Utils/UI.h"

namespace
{
	constexpr float kPi = std::numbers::pi_v<float>;
	/// Largest value an emitter may reach in the buffers (R11G11B10 / fp16 hold ~65000; one step of headroom for blending).
	constexpr float kBufferPeak = 16384.f;
	/// The P Wobble Test: P x 2^sin(2 pi t / period), one EV each way.
	constexpr float kWobblePeriod = 4.f;
	/// The full moon picture's mean value after the sky gamma (Sky.hlsl MoonSky::PlanetshineGrey).
	constexpr float kMoonPictureMean = 0.26f;
	/// Inverse Square Lighting's constants (InverseSquareLighting.h, private there): 0.8 x 70^2 units^2, 4.5 x 0.8 x 70 units.
	constexpr float kIslScaledUnitsSq = 0.8f * 70.f * 70.f;
	constexpr float kIslFadeZoneBase = 4.5f * 0.8f * 70.f;

	HistogramAutoExposure* ActiveAutoExposure()
	{
		auto& pp = globals::features::postProcessing;
		if (!pp.loaded)
			return nullptr;
		auto& pipe = pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::AutoExposure)];
		if (!pipe || !pipe->enabled || !pipe->RuntimeGateOpen())
			return nullptr;
		return static_cast<HistogramAutoExposure*>(pipe.get());
	}

	float Softplus2(float a_x)
	{
		// log2(1 + 2^x), without overflow.
		return a_x > 30.f ? a_x : std::log2(1.f + std::exp2(a_x));
	}
}

bool PhysicalBrightness::MasterOn() const
{
	const auto& physicalSky = globals::features::physicalSky;
	return settings.enabled && physicalSky.loaded;
}

float PhysicalBrightness::VanillaDiffuseMult()
{
	const auto& ll = globals::features::linearLighting.settings;
	return ll.enableLinearLighting && std::isfinite(ll.vanillaDiffuseMult) ? std::clamp(ll.vanillaDiffuseMult, 0.01f, 10.f) : 1.f;
}

float PhysicalBrightness::CappedP(float a_peak) const
{
	const float p = GetP();
	return a_peak > 0.f ? std::min(p, kBufferPeak / a_peak) : p;
}

float PhysicalBrightness::LampMult() const
{
	return std::clamp(settings.lampStrength, 0.f, 16.f) * std::max(settings.calLamp, 0.f) * GetP() / (kPi * VanillaDiffuseMult());
}

float PhysicalBrightness::EmissiveMult() const
{
	const float cal = std::max(settings.calEmissive, 0.f);
	return std::clamp(settings.emissiveStrength, 0.f, 16.f) * cal * CappedP(cal * 4.f);
}

float PhysicalBrightness::FireSelfLitScale() const
{
	const float cal = std::max(settings.calFire, 0.f);
	return cal * CappedP(cal * 4.f);
}

float PhysicalBrightness::MoonDiscScale() const
{
	const float peak = std::max(settings.calMoonDisc, 0.f) / kMoonPictureMean;
	return peak * CappedP(peak);
}

float PhysicalBrightness::SunUnitsToPipeline(float a_sunLuminance) const
{
	return a_sunLuminance > 1e-12f ? GetP() * std::max(settings.sunLux, 1.f) / a_sunLuminance : GetP();
}

bool PhysicalBrightness::ReadAdaptedLuminance(float& o_luminance) const
{
	const auto* ae = ActiveAutoExposure();
	if (!ae || !ae->readbackValid || !ae->readbackPhysical || !(ae->readback[1] > 0.f) || !std::isfinite(ae->readback[1]))
		return false;
	o_luminance = ae->readback[1];
	return true;
}

void PhysicalBrightness::BeginFrame()
{
	const float dt = std::clamp(RE::GetSecondsSinceLastFrame(), 0.f, 0.5f);
	timeSeconds = std::fmod(timeSeconds + dt, 3600.f);

	// ---- Where are we ----
	scene = Scene::None;
	auto* ui = globals::game::ui;
	const bool inMenu = ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
	const auto& ll = globals::features::linearLighting;
	if (MasterOn() && ll.loaded && ll.settings.enableLinearLighting && !inMenu) {
		// (step 5, design 5.1 / 9) Every place is in the model: the physical sky's worldspaces; interiors (the game's ambient and lamps, calibrated);
		// other exteriors (the game's sun, ambient and sky, calibrated).
		auto& physicalSky = globals::features::physicalSky;
		if (Util::IsInterior())
			scene = Scene::Interior;
		else if (physicalSky.WillDraw())
			scene = Scene::OutdoorPhysical;
		else
			scene = Scene::OutdoorVanilla;
	}
	if (scene != lastScene) {
		sceneChangeFrame = globals::state->frameCount;
		lastScene = scene;
	}
	if (!Active()) {
		P = pBase = 1.f;
		deltaK = 0.f;
		adaptedFromAE = false;
		return;
	}

	// (step 2) The game's own brightness: the Night Darkness curve's vanilla end and the Minimum Visibility reference.
	ComputeVanillaReference();

	// ---- The eye's adapted luminance (cd/m^2) ----
	// CPU estimate: a grey (0.18) surface in this frame's light (the physical scene of the last frame). Used before the auto exposure has
	// a Physical Brightness reading and while it is off.
	if (scene == Scene::OutdoorPhysical) {
		const auto& ps = globals::features::physicalSky.physScene;
		const float sunLum = 0.2126f * globals::features::physicalSky.settings.sunlightColor.x + 0.7152f * globals::features::physicalSky.settings.sunlightColor.y +
		                     0.0722f * globals::features::physicalSky.settings.sunlightColor.z;
		const float lux = ps.eReal > 0.f && sunLum > 1e-12f ? ps.eReal * std::max(settings.sunLux, 1.f) / sunLum : 0.f;
		estimatedLuminance = lux > 0.f ? std::clamp(0.18f * lux / kPi, 1e-5f, 1e5f) : 100.f;  // no scene yet: a dull day
	} else if (scene == Scene::Interior) {
		// (step 5) The interior ambient in lamp units (x calLamp: lux) on a grey surface, x 2 for the lamps.
		estimatedLuminance = std::clamp(2.f * 0.18f * vanillaAmbientNow / VanillaDiffuseMult() * settings.calLamp / kPi, 1e-5f, 1e5f);
	} else {
		// (step 5) The game's sun (x calSunVanilla) and ambient (x calAmbientVanilla) on a grey surface.
		estimatedLuminance = std::clamp(0.18f * (vanillaSunNow * 0.5f * settings.calSunVanilla / kPi + vanillaAmbientNow * settings.calAmbientVanilla), 1e-5f, 1e5f);
	}
	float luminance = 0.f;
	adaptedFromAE = ReadAdaptedLuminance(luminance);
	if (!adaptedFromAE)
		luminance = estimatedLuminance;
	adaptedLuminance = std::clamp(luminance, 1e-6f, 1e6f);

	// Display key offset on the CPU (the auto exposure computes the same on the GPU; this copy is for the status and the no-AE case).
	{
		const float t = std::clamp(settings.nightDarkness, 0.f, 1.f);
		const float D = std::log2(std::max(settings.fullAdaptation, 1e-3f) / adaptedLuminance);
		const float physical = -std::clamp(settings.physSlope, 0.f, 1.f) * Softplus2(D);
		const float bright = std::clamp(settings.brightBoost, 0.f, 1.f) * Softplus2(std::log2(adaptedLuminance / std::max(settings.brightAbove, 1e-3f)));
		deltaK = (1.f - t) * deltaKVanilla + t * physical + bright + std::clamp(settings.exposureCompensation, -5.f, 5.f);
		deltaK = std::clamp(deltaK, -12.f, 4.f);
	}

	// ---- P ----
	// With the auto exposure: P = 0.18 / L_a (its exposure divides P out again, design 3.1). Without it the eye runs here: P carries the
	// exposure itself (0.18 x 2^dK / L_a), so the picture is still adapted.
	if (ActiveAutoExposure())
		pBase = 0.18f / adaptedLuminance;
	else
		pBase = 0.18f * std::exp2(deltaK) / adaptedLuminance;
	pBase = std::clamp(pBase, std::exp2(-20.f), std::exp2(14.f));
	P = pBase;
	if (settings.pWobble)
		P *= std::exp2(std::sin(2.f * kPi * timeSeconds / kWobblePeriod));

	// (step 5) The game-colour factor outside the physical sky: the same factor as the game's ambient takes (EngineAmbientScale).
	vanillaUnlit = scene == Scene::OutdoorPhysical ? 1.f : EngineAmbientScale();
}

float PhysicalBrightness::EngineAmbientScale() const
{
	// (step 5, design 3.2 row 7, design 4) The game's own ambient (decoded) x this = pipeline luminance of a white surface.
	// Interior: in lamp units (the same units as the game's lamps: x calLamp / (pi x VDM) x P, so the game's ratio of lamps to ambient
	// holds), x Interior Ambient. Other exteriors: calAmbientVanilla x P, x Sky Ambient Strength.
	if (scene == Scene::Interior)
		return std::clamp(settings.interiorAmbient, 0.f, 4.f) * std::max(settings.calLamp, 0.f) * GetP() / (kPi * VanillaDiffuseMult());
	if (scene == Scene::OutdoorVanilla)
		return std::clamp(settings.skyAmbientStrength, 0.f, 4.f) * std::max(settings.calAmbientVanilla, 0.f) * GetP();
	return 1.f;
}

float PhysicalBrightness::DirectionalLightMult() const
{
	// (step 5) Linear Lighting's directionalLightMult for the game's own directional light (not Physical Sky's): interiors in lamp units,
	// other exteriors x calSunVanilla / (pi x VDM) x P (Lighting multiplies VDM again: albedo x E / pi x P).
	if (scene == Scene::Interior)
		return std::max(settings.calLamp, 0.f) * GetP() / (kPi * VanillaDiffuseMult());
	if (scene == Scene::OutdoorVanilla)
		return std::max(settings.calSunVanilla, 0.f) * GetP() / (kPi * VanillaDiffuseMult());
	return 1.f;
}

float PhysicalBrightness::GameColourFactor() const
{
	if (!Active())
		return 1.f;
	return scene == Scene::OutdoorPhysical ? globals::features::physicalSky.physScene.rAmb : vanillaUnlit;
}

void PhysicalBrightness::AdjustLight(LightLimitFix::LightData& a_light, bool a_isl, bool a_pointLight) const
{
	if (!Active())
		return;
	using Flags = LightLimitFix::LightFlags;
	// Inverse square falloff for every lamp ("Lamp Falloff: Physical"): the game's lamp keeps its value at half its radius
	// (0.75 x fade there), from where it falls off as 1 / (d^2 + size bias); it reaches 1.5x as far.
	if (settings.lampFalloff == kFalloffInverseSquare && a_isl && a_pointLight && a_light.lightFlags.none(Flags::InverseSquare) && a_light.radius > 1.f) {
		const float r = a_light.radius;
		const float dRef = 0.5f * r;
		const float sizeBias = kIslScaledUnitsSq;  // ISL's size sqrt(2): ScaledUnitsSq x 2 x 0.5
		a_light.sizeBias = sizeBias;
		a_light.fade *= 0.75f * (dRef * dRef + sizeBias) / kIslScaledUnitsSq;
		a_light.radius = 1.5f * r;
		a_light.invRadius = 1.f / a_light.radius;
		a_light.fadeZone = 1.f / (a_light.radius * std::clamp(kIslFadeZoneBase * a_light.invRadius, 1e-3f, 1.f));
		a_light.lightFlags.set(Flags::InverseSquare);
	}
	// Linear-flagged lamps skip Linear Lighting's pointLightMult in the shaders: their fade takes the same factor.
	if (a_light.lightFlags.any(Flags::Linear))
		a_light.fade *= LampMult();
}

void PhysicalBrightness::Load(const json& a_json)
{
	settings = {};
	if (!a_json.is_object())
		return;
	auto& s = settings;
	auto get = [&](const char* a_key, auto& a_value) {
		if (const auto it = a_json.find(a_key); it != a_json.end()) {
			try {
				a_value = it->get<std::remove_reference_t<decltype(a_value)>>();
			} catch (...) {
			}
		}
	};
	get("enabled", s.enabled);
	get("lampFalloff", s.lampFalloff);
	get("lampStrength", s.lampStrength);
	get("emissiveStrength", s.emissiveStrength);
	get("fireMagicStrength", s.fireMagicStrength);
	get("skyAmbientStrength", s.skyAmbientStrength);
	get("envBounceStrength", s.envBounceStrength);
	get("interiorAmbient", s.interiorAmbient);
	get("weatherTint", s.weatherTint);
	get("weatherBrightness", s.weatherBrightness);
	get("groundBounce", s.groundBounce);
	get("probeForget", s.probeForget);
	get("nightDarkness", s.nightDarkness);
	get("minVisibility", s.minVisibility);
	get("exposureCompensation", s.exposureCompensation);
	get("localAdaptation", s.localAdaptation);
	get("nightVision", s.nightVision);
	get("adaptAfterLoad", s.adaptAfterLoad);
	get("huePreserving", s.huePreserving);
	get("sunLux", s.sunLux);
	get("calLamp", s.calLamp);
	get("calEmissive", s.calEmissive);
	get("calFire", s.calFire);
	get("calMoonDisc", s.calMoonDisc);
	get("physSlope", s.physSlope);
	get("fullAdaptation", s.fullAdaptation);
	get("brightBoost", s.brightBoost);
	get("brightAbove", s.brightAbove);
	get("winsorEV", s.winsorEV);
	get("darkTrim", s.darkTrim);
	get("bloomClamp", s.bloomClamp);
	get("calSunVanilla", s.calSunVanilla);
	get("calAmbientVanilla", s.calAmbientVanilla);
	get("clearDayReference", s.clearDayReference);
	get("measuredClouds", s.measuredClouds);
	get("ambientFit", s.ambientFit);
	get("overcastKappa", s.overcastKappa);
	get("airglowRatio", s.airglowRatio);
	get("smoothingSeconds", s.smoothingSeconds);
	// pWobble is a test switch: never saved, always off after a load.
	s.lampFalloff = std::clamp(s.lampFalloff, 0, 1);
	s.adaptAfterLoad = std::clamp(s.adaptAfterLoad, 0, 1);
	auto finite = [](float& a_v, float a_lo, float a_hi, float a_default) { a_v = std::isfinite(a_v) ? std::clamp(a_v, a_lo, a_hi) : a_default; };
	finite(s.lampStrength, 0.f, 16.f, 1.f);
	finite(s.emissiveStrength, 0.f, 16.f, 1.f);
	finite(s.fireMagicStrength, 0.f, 16.f, 1.f);
	finite(s.skyAmbientStrength, 0.f, 4.f, 1.f);
	finite(s.envBounceStrength, 0.f, 10.f, 1.f);
	finite(s.interiorAmbient, 0.f, 4.f, 1.f);
	finite(s.weatherTint, 0.f, 1.f, 1.f);
	finite(s.weatherBrightness, 0.f, 1.f, 0.5f);
	finite(s.nightDarkness, 0.f, 1.f, 0.6f);
	finite(s.minVisibility, 0.f, 1.f, 0.3f);
	finite(s.exposureCompensation, -5.f, 5.f, 0.f);
	finite(s.localAdaptation, 0.f, 1.f, 0.5f);
	finite(s.nightVision, 0.f, 1.f, 0.5f);
	finite(s.sunLux, 1e3f, 1e6f, 1.28e5f);
	finite(s.calLamp, 0.f, 1e4f, 8.f);
	finite(s.calEmissive, 0.f, 1e5f, 10.f);
	finite(s.calFire, 0.f, 1e6f, 200.f);
	finite(s.calMoonDisc, 0.f, 1e6f, 2500.f);
	finite(s.physSlope, 0.f, 1.f, 0.3f);
	finite(s.fullAdaptation, 0.01f, 1e5f, 64.f);
	finite(s.brightBoost, 0.f, 1.f, 0.15f);
	finite(s.brightAbove, 1.f, 1e6f, 2000.f);
	finite(s.winsorEV, 0.5f, 10.f, 3.f);
	finite(s.darkTrim, 0.f, 40.f, 5.f);
	finite(s.bloomClamp, 1.f, 65000.f, 64.f);
	finite(s.calSunVanilla, 1e2f, 1e7f, 1.0e5f);
	finite(s.calAmbientVanilla, 1.f, 1e7f, 1.0e4f);
	finite(s.clearDayReference, 1e-3f, 10.f, 0.12f);
	finite(s.overcastKappa, 0.f, 1.f, 0.25f);
	finite(s.airglowRatio, 1.f, 1000.f, 25.f);
	finite(s.smoothingSeconds, 0.1f, 10.f, 1.5f);
}

void PhysicalBrightness::Save(json& a_json) const
{
	const auto& s = settings;
	a_json = json{
		{ "enabled", s.enabled },
		{ "lampFalloff", s.lampFalloff },
		{ "lampStrength", s.lampStrength },
		{ "emissiveStrength", s.emissiveStrength },
		{ "fireMagicStrength", s.fireMagicStrength },
		{ "skyAmbientStrength", s.skyAmbientStrength },
		{ "envBounceStrength", s.envBounceStrength },
		{ "interiorAmbient", s.interiorAmbient },
		{ "weatherTint", s.weatherTint },
		{ "weatherBrightness", s.weatherBrightness },
		{ "groundBounce", s.groundBounce },
		{ "probeForget", s.probeForget },
		{ "nightDarkness", s.nightDarkness },
		{ "minVisibility", s.minVisibility },
		{ "exposureCompensation", s.exposureCompensation },
		{ "localAdaptation", s.localAdaptation },
		{ "nightVision", s.nightVision },
		{ "adaptAfterLoad", s.adaptAfterLoad },
		{ "huePreserving", s.huePreserving },
		{ "sunLux", s.sunLux },
		{ "calLamp", s.calLamp },
		{ "calEmissive", s.calEmissive },
		{ "calFire", s.calFire },
		{ "calMoonDisc", s.calMoonDisc },
		{ "physSlope", s.physSlope },
		{ "fullAdaptation", s.fullAdaptation },
		{ "brightBoost", s.brightBoost },
		{ "brightAbove", s.brightAbove },
		{ "winsorEV", s.winsorEV },
		{ "darkTrim", s.darkTrim },
		{ "bloomClamp", s.bloomClamp },
		{ "calSunVanilla", s.calSunVanilla },
		{ "calAmbientVanilla", s.calAmbientVanilla },
		{ "clearDayReference", s.clearDayReference },
		{ "measuredClouds", s.measuredClouds },
		{ "ambientFit", s.ambientFit },
		{ "overcastKappa", s.overcastKappa },
		{ "airglowRatio", s.airglowRatio },
		{ "smoothingSeconds", s.smoothingSeconds },
	};
}

std::string PhysicalBrightness::DescribeForTrace() const
{
	static constexpr const char* kScenes[] = { "not in effect", "outdoors (physical sky)", "outdoors (vanilla sky)", "interior" };
	const auto& s = settings;
	return std::format(
		"Physical Brightness {} ({}): P {:+.2f} EV, adapted {:.3g} cd/m2 ({}), display key {:+.2f} EV (vanilla {:+.2f}); night darkness {:.2f}, minimum visibility {:.0f}%, "
		"exposure {:+.2f}, local adaptation {:.2f}, night vision {:.2f}, lamps {} x{:.2f}, emissive x{:.2f}, fire x{:.2f}, sky ambient x{:.2f}, bounce x{:.2f}, "
		"weather tint {:.2f}, weather brightness {:.2f}, after loads {}, hue-preserving curve {}{}",
		s.enabled ? "on" : "off", kScenes[static_cast<int>(scene)], std::log2(std::max(GetP(), 1e-30f)), adaptedLuminance, adaptedFromAE ? "auto exposure" : "estimate",
		deltaK, deltaKVanilla, s.nightDarkness, s.minVisibility * 100.f, s.exposureCompensation, s.localAdaptation, s.nightVision,
		s.lampFalloff == kFalloffInverseSquare ? "inverse square" : "vanilla shape", s.lampStrength, s.emissiveStrength, s.fireMagicStrength, s.skyAmbientStrength,
		s.envBounceStrength, s.weatherTint, s.weatherBrightness, s.adaptAfterLoad == kAdaptKeep ? "keep adapting" : "instant", s.huePreserving ? "on" : "off",
		s.pWobble ? ", P WOBBLE TEST ON" : "");
}

void PhysicalBrightness::DrawStatus()
{
	static constexpr const char* kScenes[] = { "not in effect", "outdoors, physical sky", "outdoors, vanilla sky", "interior" };
	ImGui::TextDisabled("Now: %s", kScenes[static_cast<int>(scene)]);
	if (!Active())
		return;
	ImGui::TextDisabled("Pre-exposure P: %+.2f EV%s", std::log2(std::max(GetP(), 1e-30f)), settings.pWobble ? " (wobble test on)" : "");
	ImGui::TextDisabled("Adapted to: %.3g cd/m2 (%s)", adaptedLuminance, adaptedFromAE ? "auto exposure" : "estimate: auto exposure off or not ready");
	ImGui::TextDisabled("Night darkness in effect: %+.2f EV on screen (the weather's own: %+.2f EV)", deltaK, deltaKVanilla);
	ImGui::TextDisabled("Game colours (fog, rain, water, unlit smoke) x%.3g", GameColourFactor());
	if (vanillaValid)
		ImGui::TextDisabled("The game's own brightness (no lamps): now %.3g, day %.3g (game units); weather brightness x%.2f", vanillaNow, vanillaDay, globals::features::physicalSky.physScene.weatherFactor);
}

void PhysicalBrightness::DrawSettings()
{
	auto& s = settings;
	const auto& ll = globals::features::linearLighting;
	const bool llOn = ll.loaded && ll.settings.enableLinearLighting;
	ImGui::Checkbox("Physical Brightness", &s.enabled);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"One brightness model for everything: the sun, the sky, the moon, lamps, windows, fire and the game's fog and rain are all\n"
			"in real units (calibrated), and the eye adapts to them (Histogram Auto Exposure) with the two Eye sliders below deciding how\n"
			"dark a night looks. Off: brightness as before 49h (no sky compression, the game's light units, Classic auto exposure).\n"
			"Needs Linear Lighting.");
	if (s.enabled && !llOn)
		ImGui::TextColored(ImVec4(1.f, 0.6f, 0.3f, 1.f), "Linear Lighting is off: Physical Brightness is not in effect.");
	ImGui::BeginDisabled(!s.enabled);

	ImGui::SeparatorText("Light Sources");
	ImGui::TextDisabled("How bright each kind of light is, in real units; 1 = the calibration.");
	{
		static constexpr const char* kFalloff[] = { "Game Shape (calibrated)", "Physical (inverse square)" };
		const bool isl = globals::features::inverseSquareLighting.loaded;
		ImGui::BeginDisabled(!isl);
		ImGui::Combo("Lamp Falloff", &s.lampFalloff, kFalloff, 2);
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Game Shape (default): lamps keep the game's falloff, calibrated at half their radius.\n"
				"Physical: every lamp falls off as 1 / distance^2 from the same brightness at half its radius: brighter close to the lamp,\n"
				"darker further away, reaching 1.5x as far. Needs Inverse Square Lighting%s.",
				isl ? "" : " (not loaded)");
	}
	ImGui::SliderFloat("Lamp Strength", &s.lampStrength, 0.25f, 4.f, "x%.2f", ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Torches, candles, braziers and every other lamp, relative to the calibration (a torch ~150 lumen).\nReplaces Linear Lighting's Point Light Multiplier while on.");
	ImGui::SliderFloat("Emissive Strength", &s.emissiveStrength, 0.25f, 4.f, "x%.2f", ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Lit windows, glowing wicks, glow maps. Replaces Linear Lighting's Emit Color / Glowmap Multipliers while on.");
	ImGui::SliderFloat("Fire and Magic Strength", &s.fireMagicStrength, 0.25f, 4.f, "x%.2f", ImGuiSliderFlags_Logarithmic);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Flames, magic and other glowing effects. Replaces Linear Lighting's Fire and Magic Multiplier while on.");
	ImGui::SliderFloat("Sky Ambient Strength", &s.skyAmbientStrength, 0.5f, 2.f, "x%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The physical sky's light in shade, outdoors (the light that replaces the weather's ambient).");
	ImGui::Indent();
	ImGui::Checkbox("Ground Bounce", &s.groundBounce);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The ground lights things from below with the light it reflects (albedo 0.2, more on snow with Dynamic Snow).");
	ImGui::Checkbox("Probe Forgets Out-of-View Light", &s.probeForget);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"The screen light probe (Dynamic Cubemaps) that Image Based Lighting takes the surroundings' bounce from counts the sky it sees\n"
			"as no bounce. What it saw stays while you only turn around; once you have moved sideways by more than about a third of its\n"
			"distance it is replaced by an estimate (grey ground in the ambient and some sun), never black.");
	ImGui::Unindent();
	ImGui::SliderFloat("Bounce Strength", &s.envBounceStrength, 0.f, 3.f, "x%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Image Based Lighting's bounce from the surroundings outdoors (Env IBL Scale while on; your saved Env IBL Scale applies indoors).");
	ImGui::SliderFloat("Interior Ambient", &s.interiorAmbient, 0.f, 2.f, "x%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("The game's interior ambient light, relative to its calibration (in the same units as the lamps).");
	ImGui::SliderFloat("Weather Tint Strength", &s.weatherTint, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How much of the weather's colour mood tints the ambient light, the clouds and the fog, relative to its own sky colour:\n"
			"0 = the physical sky's colour only, 1 = the full weather tint (a grey weather ambient under a grey weather sky is no tint).");
	ImGui::SliderFloat("Weather Brightness Strength", &s.weatherBrightness, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How much of the weather's own brightness intent (a storm darker than its clouds alone would make it) is kept, relative to\n"
			"the physical sky: 0 = the physical cloud amount only, 1 = the weather's day-to-day brightness ratio in full.");

	ImGui::SeparatorText("Eye");
	ImGui::TextDisabled("How the eye adapts and how dark the night looks; the game's own brightness is the reference.");
	ImGui::SliderFloat("Night Darkness", &s.nightDarkness, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How much darker than the day the night looks after the eye has adapted: 0 = as dark as the weather mod's own day/night\n"
			"difference, 1 = as the eye sees it (full moon about -3.7 EV, moonless about -5.8 EV on screen). Default 0.6.");
	ImGui::SliderFloat("Minimum Visibility", &s.minVisibility, 0.f, 1.f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"No area of the screen is darker than this share of what the game itself shows there now (areas lit by lamps are not\n"
			"touched; the sky is left out). Done by Local Exposure; with Local Exposure off the whole picture is lifted. 0 = off.");
	ImGui::SliderFloat("Exposure Compensation", &s.exposureCompensation, -3.f, 3.f, "%+.2f EV");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("Brighter or darker everywhere, in EV (+1 = twice as bright).");
	ImGui::SliderFloat("Local Adaptation Strength", &s.localAdaptation, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("How strongly the eye adapts to bright and dark areas separately (Local Exposure's strength while on; Local Exposure is on with the master).");
	ImGui::SliderFloat("Night Vision Colour", &s.nightVision, 0.f, 1.f, "%.2f");
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text("In dim areas colours fade and turn bluish, per pixel (lamp-lit areas keep their warm colour). 0 = off.");
	{
		static constexpr const char* kAdapt[] = { "Instant", "Keep Adapting" };
		ImGui::Combo("Adaptation After Loading Screens", &s.adaptAfterLoad, kAdapt, 2);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("Instant (default): the eye is adapted when the loading screen ends. Keep Adapting: it starts from the last place and adapts\n(entering a dark cave from daylight you see little for a few seconds).");
	}

	ImGui::SeparatorText("Screen");
	ImGui::TextDisabled("How the result is put on the display.");
	ImGui::Checkbox("Hue-Preserving Tone Curve", &s.huePreserving);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"On (default): the Lottes tone curve compresses brightness without changing the colour: bright orange stays orange and only\n"
			"turns white near the top (the per-channel curve made torchlight yellow). Only with Lottes in Color Grading.");
	ImGui::TextDisabled("Paper white and peak: Display > HDR Output (unchanged by this page).");

	ImGui::SeparatorText("Status");
	ImGui::TextDisabled("Read only.");
	DrawStatus();

	if (ImGui::TreeNodeEx("Developer##physicalBrightness", ImGuiTreeNodeFlags_None)) {
		ImGui::Checkbox("P Wobble Test", &s.pWobble);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Test only (not saved): the pre-exposure P swings one EV up and down every %.0f seconds. Everything that is correctly in the\n"
				"model stays still on screen; anything that pulses with that rhythm is a light path P does not reach.",
				kWobblePeriod);
		ImGui::SliderFloat("Sun Above the Atmosphere", &s.sunLux, 5e4f, 2e5f, "%.0f lux");
		ImGui::SliderFloat("Lamp Calibration", &s.calLamp, 0.5f, 64.f, "%.1f lux / unit", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Emissive Calibration", &s.calEmissive, 0.5f, 200.f, "%.1f cd/m2 / unit", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Fire Calibration", &s.calFire, 5.f, 5000.f, "%.0f cd/m2 / unit", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Moon Disc", &s.calMoonDisc, 50.f, 5000.f, "%.0f cd/m2", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Physiological Slope s", &s.physSlope, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat("Full Adaptation Above", &s.fullAdaptation, 1.f, 1000.f, "%.0f cd/m2", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Bright Scene Boost", &s.brightBoost, 0.f, 0.5f, "%.2f");
		ImGui::SliderFloat("Bright Scenes Above", &s.brightAbove, 100.f, 20000.f, "%.0f cd/m2", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Highlight Cut (winsorize)", &s.winsorEV, 1.f, 8.f, "median + %.1f EV");
		ImGui::SliderFloat("Ignore Darkest", &s.darkTrim, 0.f, 40.f, "%.0f%%");
		ImGui::SliderFloat("Bloom Input Clamp", &s.bloomClamp, 4.f, 4096.f, "%.0f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Vanilla Sun Calibration", &s.calSunVanilla, 1e4f, 3e5f, "%.0f lux / unit", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Vanilla Ambient Calibration", &s.calAmbientVanilla, 1e3f, 1e5f, "%.0f cd/m2 / unit", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Clear Weather Reference", &s.clearDayReference, 0.01f, 1.f, "%.3f", ImGuiSliderFlags_Logarithmic);
		ImGui::Checkbox("Overcast From Measured Clouds", &s.measuredClouds);
		ImGui::Checkbox("Accurate Ambient Directions (experimental)", &s.ambientFit);
		ImGui::SliderFloat("Overcast Pass-Through", &s.overcastKappa, 0.f, 1.f, "%.2f");
		ImGui::SliderFloat("Full Moon Sky / Airglow", &s.airglowRatio, 5.f, 100.f, "%.0f");
		ImGui::SliderFloat("Ambient Smoothing", &s.smoothingSeconds, 0.1f, 10.f, "%.1f s");
		ImGui::TreePop();
	}
	ImGui::EndDisabled();
}

bool PhysicalBrightness::LocalAdaptationRuns() const
{
	auto& pp = globals::features::postProcessing;
	if (!pp.loaded)
		return false;
	auto& pipe = pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::LocalExposure)];
	if (!pipe || !pipe->RuntimeGateOpen())
		return false;
	return Active() ? settings.localAdaptation > 1e-4f : pipe->enabled;
}

void PhysicalBrightness::ComputeVanillaReference()
{
	// (49j step 2, design 2.2.5) The game's own brightness, in its own units, as the reference for Night Darkness (relative: now / the same
	// weather's day key) and Minimum Visibility. Lamps are left out on purpose: the reference is how bright the game wants the places no lamp
	// reaches. Outdoors: the six directional ambient colours as the engine blended them (Sky::directionalAmbientColors; Physical Brightness
	// rewrites the shaders' transform, not these) linearised with Linear Lighting's ambient gamma, plus the weather's sunlight colour
	// (skyColor[kSunlight]) linearised with the light gamma x VDM x 0.5 (mean cosine) x (1 - cloud cover). Day: the same of the weather's
	// kDay keyframes (blended through a transition like the game blends).
	const auto* sky = globals::game::sky;
	const auto& ll = globals::features::linearLighting.settings;
	const float gammaA = ll.enableLinearLighting && std::isfinite(ll.ambientGamma) ? std::clamp(ll.ambientGamma, 0.1f, 5.f) : 1.f;
	const float gammaL = ll.enableLinearLighting && std::isfinite(ll.lightGamma) ? std::clamp(ll.lightGamma, 0.1f, 5.f) : 1.f;
	const float vdm = VanillaDiffuseMult();
	auto lum = [](float r, float g, float b) { return 0.2126f * r + 0.7152f * g + 0.0722f * b; };
	auto lin = [](float v, float g) { return std::pow(std::isfinite(v) ? std::max(v, 0.f) : 0.f, g); };

	if (scene == Scene::Interior) {
		// The interior's ambient as the engine set it before Physical Brightness rewrote it (PhysicalSky::dalcEngine), vs the last exterior day.
		const auto& physicalSky = globals::features::physicalSky;
		if (physicalSky.dalcEngineValid) {
			const auto& t = physicalSky.dalcEngine.translate;
			vanillaNow = 0.18f * lum(lin(t.x, gammaA), lin(t.y, gammaA), lin(t.z, gammaA));
			vanillaAmbientNow = vanillaNow / 0.18f;
			vanillaSunNow = 0.f;
			vanillaDay = vanillaDayLastExterior > 0.f ? vanillaDayLastExterior : settings.clearDayReference;
			vanillaValid = true;
		}
	} else if (sky && sky->currentWeather) {
		float ambNow = 0.f;
		for (int axis = 0; axis < 3; ++axis)
			for (int side = 0; side < 2; ++side) {
				const auto& c = sky->directionalAmbientColors[axis][side];
				ambNow += lum(lin(c.red, gammaA), lin(c.green, gammaA), lin(c.blue, gammaA));
			}
		ambNow /= 6.f;
		const float cover = std::clamp(globals::features::physicalSky.physScene.cover, 0.f, 1.f);
		const auto& sun = sky->skyColor[RE::TESWeather::ColorTypes::kSunlight];
		const float sunNow = lum(lin(sun.red, gammaL), lin(sun.green, gammaL), lin(sun.blue, gammaL)) * vdm * 0.5f * (1.f - cover);

		const float pct = std::isfinite(sky->currentWeatherPct) ? std::clamp(sky->currentWeatherPct, 0.f, 1.f) : 1.f;
		const RE::TESWeather* last = (sky->lastWeather && sky->lastWeather != sky->currentWeather && pct < 1.f) ? sky->lastWeather : nullptr;
		auto channel = [](const RE::Color& c, int i) { return (i == 0 ? c.red : i == 1 ? c.green : c.blue) * (1.f / 255.f); };
		auto blended = [&](auto&& a_get, int i) {
			const float cur = channel(a_get(sky->currentWeather), i);
			return last ? std::lerp(channel(a_get(last), i), cur, pct) : cur;
		};
		float ambDay = 0.f;
		for (int axis = 0; axis < 3; ++axis)
			for (int side = 0; side < 2; ++side) {
				auto get = [&](const RE::TESWeather* w) -> const RE::Color& {
					const auto& d = w->directionalAmbientLightingColors[RE::TESWeather::ColorTime::kDay].directional;
					const auto& mm = axis == 0 ? d.x : axis == 1 ? d.y : d.z;
					return side == 0 ? mm.max : mm.min;
				};
				ambDay += lum(lin(blended(get, 0), gammaA), lin(blended(get, 1), gammaA), lin(blended(get, 2), gammaA));
			}
		ambDay /= 6.f;
		auto getSun = [](const RE::TESWeather* w) -> const RE::Color& { return w->colorData[RE::TESWeather::ColorTypes::kSunlight][RE::TESWeather::ColorTime::kDay]; };
		const float sunDay = lum(lin(blended(getSun, 0), gammaL), lin(blended(getSun, 1), gammaL), lin(blended(getSun, 2), gammaL)) * vdm * 0.5f * (1.f - cover);

		vanillaNow = 0.18f * (ambNow + sunNow);
		vanillaAmbientNow = ambNow;
		vanillaSunNow = lum(lin(sun.red, gammaL), lin(sun.green, gammaL), lin(sun.blue, gammaL)) * (1.f - cover);
		vanillaDay = 0.18f * (ambDay + sunDay);
		if (vanillaDay > 1e-6f)
			vanillaDayLastExterior = vanillaDay;
		vanillaValid = vanillaDay > 1e-6f;
	}
	if (vanillaValid && vanillaDay > 1e-6f) {
		deltaKVanilla = std::clamp(std::log2(std::max(vanillaNow, 1e-9f) / vanillaDay), -8.f, 0.f);
		weatherDayRatio = std::clamp(vanillaDay / std::max(settings.clearDayReference, 1e-4f), 1.f / 64.f, 64.f);
	} else {
		deltaKVanilla = 0.f;
		weatherDayRatio = 1.f;
	}
}

float PhysicalBrightness::WeatherBrightnessFactor(float a_physicalCloudRatio) const
{
	const float strength = std::clamp(settings.weatherBrightness, 0.f, 1.f);
	if (!(strength > 0.f) || !(a_physicalCloudRatio > 0.f))
		return 1.f;
	const float factor = std::pow(weatherDayRatio / a_physicalCloudRatio, strength);
	return std::isfinite(factor) ? std::clamp(factor, 1.f / 16.f, 4.f) : 1.f;
}
