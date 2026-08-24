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
    EnvMaskStrength)

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

void VanillaFresnel::RestoreDefaultSettings()
{
	settings = {};
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
}

void VanillaFresnel::SaveSettings(json& o_json)
{
    o_json = settings;
}

void VanillaFresnel::DrawSettings()
{
    ImGui::Checkbox("Enable Vanilla Fresnel", reinterpret_cast<bool*>(&settings.Enable));
    ImGui::Checkbox("Enable Phong to GGX", reinterpret_cast<bool*>(&settings.EnableGGX));
    ImGui::Checkbox("Enable Phong to GGX on Grass", reinterpret_cast<bool*>(&settings.EnableGGXOnGrass));

    // (batch 9) Conversion is only meaningful with GGX on -- see LoadSettings. Turning GGX off
    // here leaves whatever conversion held, so the box is disabled rather than cleared: the
    // choice survives, it just cannot be edited while it has no consistent meaning, and it
    // comes back the moment GGX is switched on again.
    ImGui::BeginDisabled(!settings.EnableGGX);
    ImGui::Checkbox("Enable Auto Cubemaps Conversion", reinterpret_cast<bool*>(&settings.EnableDynamicCubemapsConversion));
    ImGui::EndDisabled();
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Turns a material's baked static cubemap into a dynamic reflection, and takes the "
            "cubemap's average colour as the material's reflectance instead.\n\n"
            "Requires Enable Phong to GGX, which is why this is greyed out above. Conversion "
            "hands the pixel an F0 and a roughness shaped for the GGX reflection lobe. With GGX "
            "off, the vanilla phong specular runs instead and gets that F0 mixed into its own "
            "specular colour -- two different lighting models sharing one set of numbers, which "
            "reads as blown-out or metallic-looking specular. Switch Phong to GGX on and this "
            "becomes editable again.");

    ImGui::Checkbox("Enable Eye Special Handling", reinterpret_cast<bool*>(&settings.EnableEyeSpecialHandling));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Forces eye materials to a fixed reflectance (F0 0.027) plus the Eye Roughness set below, "
            "instead of whatever the generic environment-map path works out.\n\n"
            "Eyes are now recognised per draw at runtime -- by shader technique, by material "
            "feature, or by \"eye\" appearing in the mesh name -- rather than by a compile-time "
            "flag. That third case is the one that matters: many of Skyrim's eye meshes are "
            "ordinary environment-map materials, so without it they took the average colour of "
            "the room's cubemap as their reflectance and came out looking like chrome balls, "
            "most obviously with Auto Cubemaps Conversion on.\n\n"
            "Turn this off to go back to treating those materials as plain environment-map "
            "surfaces.");

    ImGui::SliderFloat("Roughness Multiplier", &settings.RoughnessMultiplier, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Specular Roughness Blend", &settings.SpecularRoughnessBlend, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Mixes a second way of deriving roughness from the vanilla specular values into the "
            "first.\n\n"
            "At 0 the roughness comes purely from the material's specular power, which is the "
            "only source that used to exist. Turning this up blends in a estimate derived from "
            "the gloss map, weighted by how un-glossy the pixel is -- so it changes dull "
            "surfaces and leaves glossy ones alone.\n\n"
            "Roughness picks which blur level of the cubemap gets sampled, so this is directly "
            "how sharp reflections come out. Metals reading flat and washed out is the symptom "
            "of roughness being too high.");

    ImGui::SliderFloat("Base F0 Multiplier", &settings.BaseF0Multiplier, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Min F0", &settings.MinF0, 0.0f, 0.04f, "%.3f");
    ImGui::SliderFloat("Cubemap to F0 Multiplier", &settings.CubemapToF0Multiplier, 0.0f, 10.0f, "%.2f");
    ImGui::SliderFloat("Complex Material Env F0 Multiplier", &settings.ComplexMaterialF0Multiplier, 0.0f, 10.0f, "%.2f");

    ImGui::SliderFloat("Eye Roughness", &settings.EyeRoughness, 0.04f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How blurred the reflection in an eye is. Only applies while Enable Eye Special "
            "Handling is on.\n\n"
            "This used to be fixed at 0.10, which is the physically correct value for a cornea "
            "-- a real eye is a mirror. It was calibrated against the vanilla eye cubemap, "
            "which is 32x32 and almost black, so a mirror-sharp eye had nothing to reflect. "
            "With Auto Cubemaps Conversion the reflection source becomes the full-resolution "
            "real-time environment and the same value turns eyes into chrome beads.\n\n"
            "0.10 is the old behaviour. 1.00 samples the coarsest cubemap level, which is the "
            "whole environment averaged to one colour, and looks about like switching Auto "
            "Cubemaps Conversion off.\n\n"
            "This is an absolute value: Roughness Multiplier does not scale it. If you raised "
            "that multiplier to fix eyes, set it back to 1.00 or every metal surface stays "
            "flat.");

    ImGui::SliderFloat("Environment Mask Strength", &settings.EnvMaskStrength, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How much of a material's own environment mask is honoured on materials that ship "
            "one. Materials without an authored mask are unaffected either way.\n\n"
            "An authored mask says where a surface reflects and how strongly. Vanilla's masks "
            "are dark: across all 270 of them the median average value is 0.14 and nearly half "
            "of all texels are close to black, so applying them in full drops metal armour and "
            "weapon reflections to roughly a tenth of what they were.\n\n"
            "Eyes are the exception, and the reason this path exists: the vanilla eye mask is "
            "mostly black with a bright iris ring, so honouring it is what stops an eye "
            "reflecting the room over its whole surface.\n\n"
            "0 ignores authored masks. 1 applies them in full. Anything between fades "
            "smoothly.");
}
