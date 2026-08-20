#include "EnvironmentAmbient.h"

#include "Features/DynamicCubemaps.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/Skylighting.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	EnvironmentAmbient::Settings,
	Enabled,
	Blend,
	Intensity,
	Normalization,
	EnvMip,
	Saturation,
	Spread,
	OcclusionStrength,
	SampleCount,
	ApplyAO,
	EnableInterior,
	LinearComposite)

void EnvironmentAmbient::RestoreDefaultSettings()
{
	settings = {};
}

void EnvironmentAmbient::LoadSettings(json& o_json)
{
	settings = o_json;
}

void EnvironmentAmbient::SaveSettings(json& o_json)
{
	o_json = settings;
}

void EnvironmentAmbient::DrawSettings()
{
	auto& dynamicCubemaps = globals::features::dynamicCubemaps;
	auto& skylighting = globals::features::skylighting;
	auto& screenSpaceGI = globals::features::screenSpaceGI;
	auto& ssrt = globals::features::screenSpaceRayTracing;

	// Hard requirement: the effect samples the dynamic cubemaps, and the ENV_AMBIENT define is
	// compiled out of DeferredCompositeCS when Dynamic Cubemaps is missing.
	if (!dynamicCubemaps.loaded) {
		ImGui::TextColored({ 1, 0, 0, 1 }, "Requires Dynamic Cubemaps, which is not loaded. This feature does nothing.");
	}

	// Runtime mutual exclusion with SSRT diffuse. SSRT drives the forward directional ambient to
	// zero through AmbientMult and adds its own cubemap ambient, so running both would double the
	// ambient contribution. The shader reads ssrtSettings.DiffuseMult directly, so this follows the
	// SSRT toggle without a composite recompile.
	const bool ssrtDiffuseActive = ssrt.loaded && ssrt.settings.EnableDiffuse && ssrt.settings.DiffuseMult > 0.0f;
	if (ssrtDiffuseActive) {
		ImGui::TextColored({ 1, 0.65f, 0, 1 }, "Inactive: Screen Space Ray Tracing diffuse is enabled.");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"SSRT diffuse already applies its own dynamic cubemap ambient and zeroes the\n"
				"vanilla directional ambient through its Ambient Mult setting. Environment\n"
				"Ambient stays disabled while that is the case so the scene is not lit twice.\n"
				"Turn SSRT diffuse off to use Environment Ambient; no restart is needed.");
		}
		if (ImGui::Button("Disable SSRT Diffuse", { -1, 0 }))
			ssrt.settings.EnableDiffuse = false;
	}

	///////////////////////////////
	ImGui::SeparatorText("Blend & Intensity");

	ImGui::Checkbox("Enabled", (bool*)&settings.Enabled);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Replace the vanilla directional ambient light in the deferred composite with\n"
			"environment light sampled from the dynamic cubemaps. When off, the composite\n"
			"behaves exactly as it does without this feature installed.");
	}

	{
		auto settingsGuard = Util::DisableGuard(!settings.Enabled);

		// Logarithmic so the 0.0-0.3 range - where mixing a small amount of environment light
		// into a low directional-ambient setup lives - stays comfortably adjustable.
		ImGui::SliderFloat("Blend", &settings.Blend, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"0.0 = vanilla directional ambient only (identical to this feature being off).\n"
				"1.0 = environment light only.\n"
				"The slider is logarithmic so the low end is easy to dial in.");
		}

		ImGui::SliderFloat("Intensity", &settings.Intensity, 0.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Scalar multiplier on the environment light before it is blended in.");
		}

		///////////////////////////////
		ImGui::SeparatorText("Environment");

		ImGui::SliderFloat("Cubemap Mip", &settings.EnvMip, 0.0f, 7.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Prefiltered cubemap mip used for each hemisphere sample. 2.0 (default) is what the\n"
				"SSRT dynamic-cubemap fallback used; the angular width of the lobe comes from the\n"
				"cosine-hemisphere sample directions, not from the mip. Raising it blurs sky and\n"
				"ground into each other, which desaturates the light and darkens upward-facing\n"
				"surfaces. Mip 7 is a single averaged colour with no directionality left.");
		}

		ImGui::SliderFloat("Saturation", &settings.Saturation, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Saturation of the environment light, applied before albedo. 1.0 is off.");
		}

		{
			int sampleCount = std::clamp((int)settings.SampleCount, 1, 8);
			if (ImGui::SliderInt("Hemisphere Samples", &sampleCount, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp))
				settings.SampleCount = (uint)sampleCount;
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Number of cosine-weighted hemisphere directions averaged per pixel, using the\n"
					"same concentric Hammersley set the SSRT fallback traced its rays with. 1 samples\n"
					"the surface normal only, which biases every surface towards whatever sits\n"
					"straight above it (a blue zenith outdoors) and is the main reason a single tap\n"
					"reads cool and desaturated. Each extra sample costs two cubemap taps from mips\n"
					"that fit in cache.");
			}
		}

		ImGui::SliderFloat("Hemisphere Spread", &settings.Spread, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"1.0 (default) is the full cosine-weighted hemisphere, matching the SSRT fallback's\n"
				"ray distribution. Lower values pull the samples back towards the surface normal,\n"
				"narrowing the lobe: useful to isolate sampling effects while debugging, but it\n"
				"starves vertical surfaces of sky light.");
		}

		if (!skylighting.loaded) {
			ImGui::TextColored({ 1, 0.65f, 0, 1 }, "Skylighting is not loaded: sky visibility and ambient occlusion are not applied.");
		}

		///////////////////////////////
		ImGui::SeparatorText("Occlusion");

		ImGui::Checkbox("Apply Occlusion", (bool*)&settings.ApplyAO);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Darken the environment light where the sky is occluded, in addition to attenuating\n"
				"the sky part of the cubemap. The SSRT fallback multiplied its whole environment\n"
				"colour by a second, independent screen-space occlusion factor, which is what gave\n"
				"backlit walls their gradient under eaves and at ground junctions. Turn off to see\n"
				"the unoccluded environment light on its own.");
		}

		{
			auto aoGuard = Util::DisableGuard(!settings.ApplyAO);
			ImGui::SliderFloat("Occlusion Strength", &settings.OcclusionStrength, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"How much of the Skylighting sky visibility is reused as an ambient occlusion\n"
					"factor on the whole environment term. 1.0 matches the SSRT fallback's\n"
					"Occlusion Strength default. Screen Space GI occlusion, when it is loaded, is\n"
					"always multiplied in on top of this, exactly as the fallback did.");
			}
		}

		if (!screenSpaceGI.loaded || !screenSpaceGI.settings.Enabled) {
			ImGui::TextColored({ 1, 0.65f, 0, 1 }, "Screen Space GI is off: only Skylighting occlusion is applied.");
		}

		///////////////////////////////
		ImGui::SeparatorText("Advanced");

		ImGui::Checkbox("Enable in Interiors", (bool*)&settings.EnableInterior);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Interiors use a separate composite permutation without Skylighting, so the sky\n"
				"visibility term is unavailable and only the no-sky cubemap is used. Off by\n"
				"default, which leaves interiors on the vanilla ambient path.");
		}

		ImGui::SliderFloat("Normalization", &settings.Normalization, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Rescales the cubemap brightness towards the vanilla directional ambient\n"
				"brightness, using the SSRT fallback's formula verbatim. 0.0 (default) takes the\n"
				"brightness straight from the cubemap capture, matching the fallback's own\n"
				"Cubemap Normalization default. 1.0 makes the Blend slider energy neutral.");
		}

	}
}
