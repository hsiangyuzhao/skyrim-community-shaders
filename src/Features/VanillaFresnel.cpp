#include "VanillaFresnel.h"

#include "Globals.h"
#include "ShaderCache.h"
#include "State.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
    VanillaFresnel::Settings,
    Enable,
    EnableGGX,
    EnableGGXOnGrass,
    EnableDynamicCubemapsConversion,
    EnableEyeSpecialHandling,
    RoughnessMultiplier,
    SpecularRoughnessBlend,
    BaseF0Multiplier,
    MinF0,
    CubemapToF0Multiplier,
    ComplexMaterialF0Multiplier,
    EyeRoughness,
    EnvMaskStrength,
    EyeSoftLightingScale,
    ShadowSoftLighting,
    EyeReflectionStrength,
    EyeGrazingLimit,
    EyeBackLightCut)
// (44h) Removed from JSON after the user's review, now fixed; their old keys are ignored:
// EyeDirectSpecularMode (pinned to Vanilla Phong), EyeDirectRoughness (only used by the GGX
// modes; pinned to its old default 0.1). Both stay in Settings for the cbuffer layout.

namespace
{
	constexpr auto IsEyeDescriptor = static_cast<std::uint32_t>(State::ExtraShaderDescriptors::IsEye);

	bool ContainsEye(std::string_view a_name)
	{
		constexpr std::string_view eye = "eye";
		return std::search(
				   a_name.begin(), a_name.end(),
				   eye.begin(), eye.end(),
				   [](char a_lhs, char a_rhs) {
					   return static_cast<char>(std::tolower(static_cast<unsigned char>(a_lhs))) == a_rhs;
				   }) != a_name.end();
	}

	// (batch 9) The whole point of the runtime flag. A compile-time EYE macro only covers the
	// first two cases; a large share of Skyrim's eye meshes are ordinary kEnvironmentMap
	// materials whose *geometry name* is the only thing that says "eye". Those never get the
	// EYE macro, so they fell into the generic ENVMAP branch and picked up
	// Lighting.hlsl's cubemap-average F0 (which the x pi there saturates to ~1 on a bright
	// cubemap) instead of the 0.027 an eye should have -- a mirror-metal eyeball.
	bool IsEyePass(const RE::BSLightingShader* a_shader, const RE::BSRenderPass* a_pass)
	{
		if (a_shader) {
			const auto technique = static_cast<SIE::ShaderCache::LightingShaderTechniques>(0x3F & (a_shader->currentRawTechnique >> 24));
			if (technique == SIE::ShaderCache::LightingShaderTechniques::Eye) {
				return true;
			}
		}

		if (!a_pass || !a_pass->shaderProperty) {
			return false;
		}

		const auto* lightingProperty = a_pass->shaderProperty->GetRTTI() == globals::rtti::BSLightingShaderPropertyRTTI.get() ?
		                                   static_cast<const RE::BSLightingShaderProperty*>(a_pass->shaderProperty) :
		                                   nullptr;
		if (!lightingProperty || !lightingProperty->material) {
			return false;
		}

		const auto feature = lightingProperty->material->GetFeature();
		if (feature == RE::BSShaderMaterial::Feature::kEye) {
			return true;
		}

		return feature == RE::BSShaderMaterial::Feature::kEnvironmentMap &&
		       a_pass->geometry &&
		       ContainsEye(a_pass->geometry->name.c_str());
	}

	void UpdateEyePermutation(const RE::BSLightingShader* a_shader, const RE::BSRenderPass* a_pass)
	{
		auto& descriptor = globals::state->permutationData.ExtraShaderDescriptor;
		descriptor &= ~IsEyeDescriptor;

		if (IsEyePass(a_shader, a_pass)) {
			descriptor |= IsEyeDescriptor;
		}
	}

	struct BSLightingShader_SetupGeometry
	{
		static void thunk(RE::BSLightingShader* a_shader, RE::BSRenderPass* a_pass, uint32_t a_renderFlags)
		{
			UpdateEyePermutation(a_shader, a_pass);
			func(a_shader, a_pass, a_renderFlags);
		}
		static inline REL::Relocation<decltype(thunk)> func;
	};
}

void VanillaFresnel::PostPostLoad()
{
	stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
	logger::info("[VanillaFresnel] Installed hooks - BSLightingShader_SetupGeometry");
}

void VanillaFresnel::PinEyeDirectSpecular()
{
	settings.EyeDirectSpecularMode = static_cast<uint>(EyeDirectSpecular::VanillaPhong);
	settings.EyeDirectRoughness = 0.1f;
}

void VanillaFresnel::RestoreDefaultSettings()
{
	settings = {};
	PinEyeDirectSpecular();
}

void VanillaFresnel::LoadSettings(json& o_json)
{
    settings = o_json;

    // (batch 9) Reconcile the one combination that cannot be right: the auto cubemap
    // conversion feeding a GGX-shaped F0/roughness into a pixel where GGX is switched off,
    // so vanilla phong specular runs on GGX inputs. Upstream resolves this by clearing
    // conversion; we resolve it the other way and raise GGX instead. Conversion is the only
    // thing that stops a material reflecting a baked, location-independent DDS cubemap, so a
    // saved profile with it on is a deliberate choice -- silently switching it off would take
    // away the setting the user actually wanted. Raising GGX keeps both halves consistent.
    if (settings.EnableDynamicCubemapsConversion && !settings.EnableGGX) {
        settings.EnableGGX = true;
        logger::info("[VanillaFresnel] Auto Cubemaps Conversion was enabled with Phong to GGX disabled; "
                     "enabling Phong to GGX. Conversion prepares GGX-shaped F0/roughness, which vanilla "
                     "phong specular cannot consume correctly.");
    }

    // (44h) No longer user settings; write the fixed values the shader reads.
    PinEyeDirectSpecular();

    // (batch 17) Same reasoning as the clamp above: this one is multiplied straight into a
    // light term, so a hand-edited 50 would not fail, it would just make eyes glow far worse
    // than the defect this setting exists to fix. The UI range is 0..1.
    settings.EyeSoftLightingScale = std::clamp(settings.EyeSoftLightingScale, 0.0f, 1.0f);
    settings.EyeReflectionStrength = std::clamp(settings.EyeReflectionStrength, 0.0f, 1.0f);
    settings.EyeGrazingLimit = std::clamp(settings.EyeGrazingLimit, 0.0f, 1.0f);
    settings.EyeBackLightCut = std::clamp(settings.EyeBackLightCut, 0.0f, 1.0f);
}

void VanillaFresnel::SaveSettings(json& o_json)
{
    o_json = settings;
}

void VanillaFresnel::DrawSettings()
{
    ImGui::Checkbox("Enable Vanilla Fresnel", reinterpret_cast<bool*>(&settings.Enable));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Gives vanilla materials environment reflections that get stronger at glancing angles (Fresnel). "
            "Every other option here needs this on.");
    ImGui::Checkbox("Enable Phong to GGX", reinterpret_cast<bool*>(&settings.EnableGGX));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Replaces Skyrim's old shiny highlights with a modern, more realistic highlight model (GGX).");
    ImGui::Checkbox("Enable Phong to GGX on Grass", reinterpret_cast<bool*>(&settings.EnableGGXOnGrass));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Same as above, but for grass highlights.");

    // (batch 9) Conversion is only meaningful with GGX on -- see LoadSettings. Turning GGX off
    // here leaves whatever conversion held, so the box is disabled rather than cleared: the
    // choice survives, it just cannot be edited while it has no consistent meaning, and it
    // comes back the moment GGX is switched on again.
    ImGui::BeginDisabled(!settings.EnableGGX);
    ImGui::Checkbox("Enable Auto Cubemaps Conversion", reinterpret_cast<bool*>(&settings.EnableDynamicCubemapsConversion));
    ImGui::EndDisabled();
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Replaces baked static reflections (cubemaps) with live ones and tints them by the old cubemap's colour.\n"
            "Needs Enable Phong to GGX (greyed out without it); otherwise specular looks blown out or metallic.");

    ImGui::Checkbox("Enable Eye Special Handling", reinterpret_cast<bool*>(&settings.EnableEyeSpecialHandling));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Gives eyes their own fixed reflection settings (the Eye options below) so they don't look like chrome balls.\n"
            "Off treats eyes like any other reflective material.");

    ImGui::SliderFloat("Roughness Multiplier", &settings.RoughnessMultiplier, 0.0f, 10.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Scales how rough (blurry) reflections are on everything except eyes. Higher = blurrier, duller reflections.");
    ImGui::SliderFloat("Specular Roughness Blend", &settings.SpecularRoughnessBlend, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Also uses the gloss map to decide roughness on dull surfaces; glossy surfaces are left alone.\n"
            "0 = original behaviour. Raise it if metals look flat and washed out.");

    ImGui::SliderFloat("Base F0 Multiplier", &settings.BaseF0Multiplier, 0.0f, 10.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Scales the base reflection strength (F0) taken from a material's specular. Higher = shinier surfaces.");
    ImGui::SliderFloat("Min F0", &settings.MinF0, 0.0f, 0.04f, "%.3f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("The minimum reflection strength every surface gets. Higher = even matte surfaces reflect a little more.");
    ImGui::SliderFloat("Cubemap to F0 Multiplier", &settings.CubemapToF0Multiplier, 0.0f, 10.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("With Auto Cubemaps Conversion: how strongly the old cubemap's colour sets reflection strength. Higher = brighter reflections.");
    ImGui::SliderFloat("Complex Material Env F0 Multiplier", &settings.ComplexMaterialF0Multiplier, 0.0f, 10.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Reflection strength of metal parts on Complex Material meshes (needs Extended Materials). Higher = shinier.");

    ImGui::SliderFloat("Eye Roughness", &settings.EyeRoughness, 0.04f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How blurry the room/sky reflection in eyes is (not the sun or torch highlight). Needs Enable Eye Special Handling.\n"
            "0.10 = mirror-sharp (old behaviour, can look like chrome); 1.00 = one flat average colour. Roughness Multiplier does not affect it.");

    // (44h) "Eye Direct Specular Mode" and "Eye Direct Light Roughness" removed after the
    // user's review; eye direct highlights are fixed to Vanilla Phong (see PinEyeDirectSpecular).

    // (batch 17) The one that actually matches the reported symptom. See the long note on
    // Settings::EyeSoftLightingScale for why five previous attempts at the eye all looked at
    // the highlight and none of them could have worked.
    ImGui::SliderFloat("Eye Soft Lighting Scale", &settings.EyeSoftLightingScale, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How much of Skyrim's \"soft lighting\" (fake light glowing through thin surfaces) eyes keep. Needs Enable Eye Special Handling.\n"
            "It ignores shadows and peaks in side-on sun, so it can make eyes glow. 0 = none (matches skin with Subsurface Scattering), 1 = vanilla.");

    // (44h) Eye night-glow controls. All need Enable Eye Special Handling.
    ImGui::SliderFloat("Eye Reflection Strength", &settings.EyeReflectionStrength, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("How bright the room/sky reflection in eyes is. Lower stops eyes shining at night. 1 = old behaviour.");
    ImGui::SliderFloat("Eye Grazing Limit", &settings.EyeGrazingLimit, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Removes the bright ring at the edge of the eyeball. Higher = weaker ring. 0 = old behaviour.");
    ImGui::SliderFloat("Eye Back-Light Cut", &settings.EyeBackLightCut, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text("Stops lights behind the head from lighting up the eyes. Higher = cuts more. 0 = off (old behaviour).");

    ImGui::Checkbox("Shadow Soft/Rim/Back Lighting (Sun)", reinterpret_cast<bool*>(&settings.ShadowSoftLighting));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Makes the sun's soft, rim and back lighting respect shadows, like torch light already does. Off = unchanged picture.\n"
            "Affects all materials using these effects (foliage, cloth, skin, eyes), which get noticeably darker in their own shadow.");

    ImGui::SliderFloat("Environment Mask Strength", &settings.EnvMaskStrength, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How much a material's own reflection mask (where it may reflect) is obeyed. 0 = ignore masks, 1 = obey fully.\n"
            "Vanilla masks are dark, so high values cut metal armour and weapon reflections a lot.");
}
