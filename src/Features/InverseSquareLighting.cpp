#include "InverseSquareLighting.h"
#include "Features/InverseSquareLighting/Common.h"
#include "LightLimitFix.h"
#include <DirectXPackedVector.h>
#include <numbers>

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	InverseSquareLighting::Settings,
	lampFalloff)  // (50c)

void InverseSquareLighting::DrawSettings()
{
	// (50c) Lamp Falloff.
	settings.lampFalloff = std::isfinite(settings.lampFalloff) ? std::clamp(settings.lampFalloff, 0.0f, 1.0f) : 0.0f;
	ImGui::SliderFloat("Lamp Falloff", &settings.lampFalloff, 0.0f, 1.0f, settings.lampFalloff <= 0.0f ? "Game" : (settings.lampFalloff >= 1.0f ? "Inverse Square" : "%.2f"));
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::Text(
			"How a lamp's light fades with distance.\n"
			"Game (left): the game's curve, which fades smoothly to zero at the light's radius (exactly as before).\n"
			"Inverse Square (right): real-world falloff - much brighter close to the lamp, darker further away. Both curves are equally bright "
			"at half the radius, so a room's overall brightness changes little. In between: a blend of the two.\n"
			"For torches, candles, lamps and spells handled by Light Limit Fix. Lights a mod already made inverse-square and window light keep "
			"their own curve.");
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	editor.DrawSettings();
}

void InverseSquareLighting::LoadSettings(json& o_json)
{
	// "Inverse Square Lighting": null in settings saved before 50c: the defaults.
	if (o_json.is_object())
		settings = o_json;
}

void InverseSquareLighting::SaveSettings(json& o_json)
{
	o_json = settings;
}

void InverseSquareLighting::RestoreDefaultSettings()
{
	settings = {};
}

void InverseSquareLighting::EarlyPrepass()
{
	editor.GatherLights();
}

void InverseSquareLighting::PostPostLoad()
{
	stl::detour_thunk<CreatePointLight>(REL::RelocationID(17208, 17610));
	stl::detour_thunk<BSLight_GetLuminance>(REL::RelocationID(101303, 108292));

	logger::info("[InverseSquareLighting] Installed hooks");
}

RE::NiPointLight* InverseSquareLighting::CreatePointLight::thunk(RE::TESObjectLIGH* ligh, RE::TESObjectREFR* refr, RE::NiAVObject* root, bool forceDynamic, bool useLightRadius, bool affectRequesterOnly)
{
	const auto niLight = func(ligh, refr, root, forceDynamic, useLightRadius, affectRequesterOnly);

	if (ligh && root && niLight)
		SetExtLightData(niLight, ligh);

	return niLight;
}

void InverseSquareLighting::SetExtLightData(RE::NiLight* niLight, const RE::TESObjectLIGH* ligh)
{
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);
	runtimeData->flags.set(LightLimitFix::LightFlags::Initialised);
	if (ligh->data.flags.any(static_cast<RE::TES_LIGHT_FLAGS>(ISLCommon::TES_LIGHT_FLAGS_EXT::kInverseSquare)))
		runtimeData->flags.set(LightLimitFix::LightFlags::InverseSquare);
	if (ligh->data.flags.any(static_cast<RE::TES_LIGHT_FLAGS>(ISLCommon::TES_LIGHT_FLAGS_EXT::kLinear)))
		runtimeData->flags.set(LightLimitFix::LightFlags::Linear);
	runtimeData->cutoffOverride = std::clamp(ligh->data.fallofExponent, 0.01f, 1.f);
	runtimeData->lighFormId = ligh->formID;
	const float size = ligh->data.fov >= 50.0f ? std::numbers::sqrt2_v<float> : ligh->data.fov;
	runtimeData->size = std::clamp(size, 0.01f, 50.0f);
}

void InverseSquareLighting::ProcessLight(LightLimitFix::LightData& light, RE::BSLight* bsLight, RE::NiLight* niLight) const
{
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);

	if (light.lightFlags.none(LightLimitFix::LightFlags::Initialised)) {
		const auto userData = niLight->GetUserData();
		logger::debug("[InverseSquareLighting] FormID: 0x{:08X} | Light*: {:p} | Name: {} - light uninitialised", userData ? userData->formID : 0, static_cast<void*>(niLight), niLight->name);
		runtimeData->flags.set(LightLimitFix::LightFlags::Initialised);
	}

	const bool isInvSq = light.lightFlags.any(LightLimitFix::LightFlags::InverseSquare);
	if (bsLight->pointLight && editor.enabled && ((isInvSq && editor.disableInvSqLights) || (!isInvSq && editor.disableRegularLights)))
		light.lightFlags.set(LightLimitFix::LightFlags::Disabled);

	if (bsLight->pointLight && isInvSq) {
		const float intensity = runtimeData->fade * 4;
		light.radius = CalculateRadius(intensity, bsLight->IsShadowLight(), runtimeData->cutoffOverride, runtimeData->size);
		runtimeData->radius = light.radius;
		light.invRadius = 1.f / light.radius;
		light.fadeZone = 1.f / (light.radius * std::clamp(FadeZoneBase * light.invRadius, 0.f, 1.f));
		light.sizeBias = ScaledUnitsSq * runtimeData->size * runtimeData->size * 0.5f;
		light.color /= std::max(0.001f, std::max(light.color.x, std::max(light.color.y, light.color.z)));
		// light.color *= intensity;
		light.fade = intensity;
	} else {
		light.radius = runtimeData->radius;
		light.invRadius = 1.f / light.radius;
		// light.color *= runtimeData->fade;
		light.fade = runtimeData->fade;
	}
}

void InverseSquareLighting::ApplyLampFalloff(LightLimitFix::LightData& light) const
{
	// (50c) Lamp Falloff. The shader (InverseSquareLighting.hlsli GetAttenuation) adds falloff.y x invSq - falloff.x x reg to its result, where
	// reg = 1 - (d / r)^2 (the game's curve) and invSq = ScaledUnitsSq / (d^2 + sizeBias) x smoothstep((r - d) x fadeZone) (ISL's curve).
	// falloffBlend = 0 (knob at Game, inverse-square lights): exactly the old value. Else (t, t x k) with k matching both curves at r / 2:
	// result = (1 - t) reg + t k invSq. The light keeps its radius (cluster culling, the fade to zero at the radius); the inverse-square shape
	// is ISL's default for a light without its own size (size sqrt 2: sizeBias = ScaledUnitsSq) and ISL's fade zone.
	const float t = settings.lampFalloff;
	if (!(t > 0.0f) || light.lightFlags.any(LightLimitFix::LightFlags::InverseSquare))
		return;
	const float radius = light.radius;
	if (!(radius > 1.0f) || !std::isfinite(radius))
		return;
	const float sizeBias = ScaledUnitsSq;
	const float fadeZone = 1.0f / (radius * std::clamp(FadeZoneBase / radius, 0.0f, 1.0f));
	const float half = std::clamp(0.5f * radius * fadeZone, 0.0f, 1.0f);
	const float window = half * half * (3.0f - 2.0f * half);
	const float invSqHalf = ScaledUnitsSq / (0.25f * radius * radius + sizeBias) * window;
	if (!(invSqHalf > 1e-6f))
		return;
	const float k = 0.75f / invSqHalf;  // the game's curve is 0.75 at r / 2
	const float blend = std::min(t, 1.0f);
	light.sizeBias = sizeBias;
	light.fadeZone = fadeZone;
	const uint32_t hi = DirectX::PackedVector::XMConvertFloatToHalf(blend);
	const uint32_t lo = DirectX::PackedVector::XMConvertFloatToHalf(std::min(blend * k, 65000.0f));
	light.falloffBlend = (hi << 16) | lo;
}

float InverseSquareLighting::CalculateRadius(const float intensity, const bool shadowCaster, const float cutoffOverride, const float size)
{
	float cutoff = shadowCaster ? DefaultShadowCasterCutoff : DefaultCutoff;
	cutoff = cutoffOverride == 1.f ? cutoff : cutoffOverride;
	const float radius = std::sqrt(ScaledUnitsSq * ((2 * intensity - cutoff * size * size) / (2 * cutoff)));
	return isnan(radius) ? 1.f : radius;
}

inline float InverseSquareLighting::SmoothStep(const float edge0, const float edge1, const float x)
{
	const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
	return t * t * (3.0f - 2.0f * t);
}

float InverseSquareLighting::GetAttenuation(const float distance, const float radius, const float size)
{
	const float attenuation = ScaledUnitsSq / (distance * distance + ScaledUnitsSq * size * size / 2);
	const float fadeZone = std::clamp(FadeZoneBase / radius, 0.0f, 1.0f);
	const float fade = SmoothStep(0, radius * fadeZone, radius - distance);
	return attenuation * fade;
}

float InverseSquareLighting::BSLight_GetLuminance::thunk(RE::BSLight* bsLight, RE::NiPoint3* targetPosition, RE::NiLight* refLight)
{
	auto* niLight = bsLight->light.get();
	const auto runtimeData = ISLCommon::RuntimeLightDataExt::Get(niLight);

	if (refLight == niLight || runtimeData->flags.any(LightLimitFix::LightFlags::Disabled))
		return 0.0f;

	if (!bsLight->pointLight || runtimeData->flags.none(LightLimitFix::LightFlags::InverseSquare))
		return func(bsLight, targetPosition, refLight);

	const float dist = niLight->world.translate.GetDistance(*targetPosition);
	const float attenuation = GetAttenuation(dist, runtimeData->radius, runtimeData->size);
	const float luminance = (runtimeData->diffuse.red + runtimeData->diffuse.green + runtimeData->diffuse.blue) * runtimeData->fade * 4 * attenuation * (1.0f / 3.0f);
	bsLight->luminance = luminance;

	return luminance;
}
