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
    EyeDirectRoughness,
    EyeDirectSpecularMode,
    EyeSoftLightingScale,
    ShadowSoftLighting)

namespace
{
	// (batch 15) Labels for VanillaFresnel::EyeDirectSpecular, in enum order.
	constexpr std::array EyeDirectSpecularModeNames{
		"Vanilla Phong (matches Dec-2025)",
		"GGX, gated by gloss + mask",
		"GGX, raw (current)"
	};
	static_assert(EyeDirectSpecularModeNames.size() == static_cast<std::size_t>(VanillaFresnel::EyeDirectSpecular::Total));
}

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

    // (batch 15) A hand-edited or older json can hold anything here; an out-of-range value
    // would index past the label array in DrawSettings and would fall through every branch in
    // Lighting.hlsl, silently landing on GGXRaw. Clamp to the safe default instead.
    if (settings.EyeDirectSpecularMode >= static_cast<uint>(EyeDirectSpecular::Total)) {
        logger::warn("[VanillaFresnel] Eye Direct Specular Mode was {}, which is not a valid mode; "
                     "resetting to Vanilla Phong.",
            settings.EyeDirectSpecularMode);
        settings.EyeDirectSpecularMode = static_cast<uint>(EyeDirectSpecular::VanillaPhong);
    }

    // (batch 17) Same reasoning as the clamp above: this one is multiplied straight into a
    // light term, so a hand-edited 50 would not fail, it would just make eyes glow far worse
    // than the defect this setting exists to fix. The UI range is 0..1.
    settings.EyeSoftLightingScale = std::clamp(settings.EyeSoftLightingScale, 0.0f, 1.0f);
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
            "How blurred the *environment reflection* in an eye is -- the room, the sky, the "
            "cubemap. Since batch 13 it no longer touches direct light; that has its own slider "
            "below. Only applies while Enable Eye Special Handling is on.\n\n"
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
            "flat.\n\n"
            "Raising this is a workaround, not a fix. The real problem is that the environment "
            "reflection arrives too bright, and blurring it only spreads the excess out.");

    // (batch 15) The switch, not another number. See the long note on
    // Settings::EyeDirectSpecularMode for why four previous attempts at the glowing eye all
    // moved a roughness and none of them helped.
    {
        int mode = static_cast<int>(settings.EyeDirectSpecularMode);
        if (ImGui::Combo("Eye Direct Specular Mode", &mode, EyeDirectSpecularModeNames.data(),
                static_cast<int>(EyeDirectSpecularModeNames.size()))) {
            settings.EyeDirectSpecularMode = static_cast<uint>(std::clamp(mode, 0,
                static_cast<int>(EyeDirectSpecularModeNames.size()) - 1));
        }
    }
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Which lighting model draws the highlight an actual light source -- the sun, a "
            "torch -- leaves on an eye. Only applies while Enable Eye Special Handling is on, "
            "and only to direct light: the environment reflection in an eye is controlled by "
            "Eye Roughness above and is the same in all three modes.\n\n"
            "Eyes need their own setting because they are the only surface in the game whose "
            "GGX reflectance and roughness are fixed numbers instead of being read from the "
            "material's own textures. Every other surface feeds its gloss map and specular "
            "colour into the GGX highlight, so those values keep it in check. An eye has "
            "nothing doing that job, and the GGX highlight is roughly three thousand times "
            "more concentrated than the old one at the same settings.\n\n"
            "Vanilla Phong (matches Dec-2025): the eye's direct highlight uses the original "
            "Skyrim highlight, multiplied by the gloss map and the environment mask the way it "
            "always was, even with Enable Phong to GGX switched on. This is the December 2025 "
            "behaviour and it is the only one of the three we have a known-good reference for, "
            "which is why it is the default.\n\n"
            "GGX, gated by gloss + mask: keeps the newer GGX highlight but puts the gloss map "
            "and the environment mask back in front of it. In principle the better answer -- "
            "it is the modern lighting model *and* it respects what the eye texture says. "
            "Untested, so it is not the default.\n\n"
            "GGX, raw (current): what the mod has been doing since the Phong-to-GGX default "
            "was switched on. Kept so you can flip between them and see the difference for "
            "yourself.");

    ImGui::BeginDisabled(settings.EyeDirectSpecularMode == static_cast<uint>(EyeDirectSpecular::VanillaPhong));
    ImGui::SliderFloat("Eye Direct Light Roughness", &settings.EyeDirectRoughness, 0.04f, 1.0f, "%.2f");
    ImGui::EndDisabled();
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How wide the highlight from an actual light source -- the sun, a torch, a candle -- "
            "is on an eye. Only applies while Enable Eye Special Handling is on.\n\n"
            "This used to share one value with Eye Roughness above, and that was the bug: "
            "Eye Roughness had been raised to 0.70 to blur the environment reflection, and the "
            "same 0.70 widened the sun's highlight until it stopped being a glint and covered "
            "the whole eyeball. In side-on sunlight the eyes read as glowing white.\n\n"
            "The two needs are not the same. The environment reflection is being blurred to "
            "compensate for arriving too bright; a direct highlight has no such problem and "
            "wants the physically correct cornea value. 0.10 is that value and is the default. "
            "Raise it only if you want a deliberately soft, matte highlight.\n\n"
            "Greyed out in Vanilla Phong mode: the original Skyrim highlight has no roughness, "
            "it takes its width from the material's own specular power, so this slider does "
            "nothing there.\n\n"
            "It also turned out not to be the fix. Both GGX modes spread a fixed amount of "
            "energy over a wider or narrower patch -- lowering this concentrates the same total "
            "brightness into a smaller spot rather than removing any of it.");

    // (batch 17) The one that actually matches the reported symptom. See the long note on
    // Settings::EyeSoftLightingScale for why five previous attempts at the eye all looked at
    // the highlight and none of them could have worked.
    ImGui::SliderFloat("Eye Soft Lighting Scale", &settings.EyeSoftLightingScale, 0.0f, 1.0f, "%.2f");
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "How much of Skyrim's \"soft lighting\" fill an eye keeps. 0 removes it, 1 is the "
            "vanilla amount. Only applies while Enable Eye Special Handling is on, and only to "
            "eyes.\n\n"
            "Soft lighting is extra light Skyrim adds on top of normal diffuse lighting to fake "
            "light bleeding through a thin surface. It is at its strongest when the light is "
            "hitting the surface side-on, and fades to nothing both facing the light and fully "
            "behind it -- so it does nothing at night and nothing in full backlight.\n\n"
            "On eyes it is the reason for \"my character's eyes glow in side-on sunlight\". Two "
            "things stack up: vanilla sets this fill two to four times stronger on eyes than on "
            "a face and pairs it with an almost-white texture, and on the sun this one term "
            "never gets multiplied by the shadow, so brow, eye socket and hair do not block any "
            "of it. Together that is roughly half a full sunbeam landing on the eye with "
            "nothing in the way.\n\n"
            "0 is the default because that is what the face already gets: whenever Subsurface "
            "Scattering is installed, soft lighting on skin is switched off completely in the "
            "shader. Setting eyes to 0 makes them match the face instead of picking an "
            "arbitrary number. Turn it up if the eyes look too dead for you -- that is a taste "
            "call and there is no wrong answer.\n\n"
            "This is an old defect, not a new one: it has been in the shader since at least "
            "December 2025. What changed is that eyes gained a real-time environment "
            "reflection, which brightened them enough for the fill to become obvious.");

    ImGui::Checkbox("Shadow Soft/Rim/Back Lighting (Sun)", reinterpret_cast<bool*>(&settings.ShadowSoftLighting));
    if (auto _tt = Util::HoverTooltipWrapper())
        ImGui::Text(
            "Makes the sun's soft-lighting, rim-lighting and back-lighting fill obey shadows. "
            "Off by default, and off means exactly today's picture -- nothing changes until you "
            "tick it.\n\n"
            "These three fill terms have a gap: the versions driven by torches and other point "
            "lights are multiplied by that light's shadow, but the versions driven by the sun "
            "are not. The sun's fill therefore passes straight through anything casting a "
            "shadow -- a brow over an eye, a leaf over the leaf behind it, a fold in cloth.\n\n"
            "Ticking this multiplies the sun's three terms by the same screen-space and "
            "parallax shadow the rest of the sunlight already uses, matching what the point "
            "lights do.\n\n"
            "It is off by default because it is not an eyes-only change. Every material that "
            "uses these flags is affected -- foliage, cloth, leather, skin -- and inside their "
            "own shadow the term drops by roughly 40 to 70 percent. That is more correct, but "
            "it is a broad change to how the game looks, so it is your call rather than ours. "
            "Worth trying with the Eye Soft Lighting Scale slider above put back up.");

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
