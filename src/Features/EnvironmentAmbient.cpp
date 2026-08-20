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
	AOPower,
	ApplyAO,
	EnableInterior,
	NormalizationMode,
	JitteredSampling,
	JitterAngle)

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
				"Prefiltered cubemap mip used for the diffuse hemisphere. Higher is blurrier and\n"
				"closer to a true cosine lobe; lower keeps more directional detail but can show\n"
				"blotches. Mip 7 is a single averaged colour with no directionality left.");
		}

		ImGui::SliderFloat("Saturation", &settings.Saturation, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Saturation of the environment light, applied before albedo.");
		}

		ImGui::Checkbox("Jittered Sampling", (bool*)&settings.JitteredSampling);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Average three cubemap directions instead of one: the surface normal plus a\n"
				"rotating pair tilted away from it. Widens the effective filter kernel towards a\n"
				"cosine lobe, which a single mip cannot express, and hides cubemap face seams.\n"
				"Costs four extra cubemap samples from mips that fit entirely in cache.\n"
				"Turn off to isolate single-direction behaviour while debugging.");
		}

		{
			auto jitterGuard = Util::DisableGuard(!settings.JitteredSampling);
			ImGui::SliderAngle("Jitter Angle", &settings.JitterAngle, 0.0f, 90.0f, "%.0f deg", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Angular offset of the two extra directions from the surface normal. Larger\n"
					"widens the lobe, which can be traded against a lower Cubemap Mip to keep more\n"
					"directional detail.");
			}
		}

		if (!skylighting.loaded) {
			ImGui::TextColored({ 1, 0.65f, 0, 1 }, "Skylighting is not loaded: sky visibility is not applied.");
		}

		///////////////////////////////
		ImGui::SeparatorText("Occlusion");

		if (!screenSpaceGI.loaded || !screenSpaceGI.settings.Enabled) {
			ImGui::TextColored({ 1, 0.65f, 0, 1 }, "Screen Space GI is off: no contact occlusion is applied.");
		}

		ImGui::Checkbox("Apply Occlusion", (bool*)&settings.ApplyAO);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Apply the Screen Space GI occlusion exponent to the environment light. Turn off\n"
				"to isolate the environment light from the occlusion contribution while tuning.");
		}

		{
			auto aoGuard = Util::DisableGuard(!settings.ApplyAO);
			ImGui::SliderFloat("Occlusion Power", &settings.AOPower, 0.5f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Exponent on the Screen Space GI occlusion for the ambient term only. Direct\n"
					"light keeps the unmodified occlusion. Raise it to darken contact regions such\n"
					"as hair over a face or the base of plants, which the SSRT fallback darkened\n"
					"with an extra self-intersection factor that is not available here.");
			}
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
				"Rescales the cubemap brightness towards the vanilla ambient brightness.\n"
				"0.0 (default) takes the brightness straight from the cubemap capture, matching\n"
				"the SSRT fallback default. 1.0 makes the Blend slider energy neutral.");
		}

		{
			auto normGuard = Util::DisableGuard(settings.Normalization <= 0.0f);
			const char* modes[] = { "Directional ambient luminance", "G-buffer ambient luminance" };
			int mode = settings.NormalizationMode > 1u ? 1 : (int)settings.NormalizationMode;
			if (ImGui::Combo("Normalization Target", &mode, modes, IM_ARRAYSIZE(modes)))
				settings.NormalizationMode = (uint)mode;
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Which brightness the cubemap is normalised against.\n"
					"Directional ambient luminance reproduces the SSRT fallback formula.\n"
					"G-buffer ambient luminance uses the forward ambient value carried in the\n"
					"G-buffer, which already includes IBL and Skylighting and behaves identically\n"
					"with and without Linear Lighting.");
			}
		}
	}
}
