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
	LinearComposite,
	EnclosureFallback,
	HueFalloff,
	EnableContactOcclusion,
	ContactRadius,
	ContactStrength,
	AOExponent)

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

		// Linear scale: the working range in practice is the high end, around 0.8-0.9, where a
		// logarithmic slider has almost no resolution.
		ImGui::SliderFloat("Blend", &settings.Blend, 0.0f, 1.0f, "%.3f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"0.0 = vanilla directional ambient only (identical to this feature being off).\n"
				"1.0 = environment light only.\n"
				"\n"
				"This is the same dial SSRT diffuse spells as Ambient Multiplier, inverted:\n"
				"Blend = 1 - Ambient Multiplier. To port an SSRT setup that kept some vanilla\n"
				"ambient, use Blend = 1 - AmbientMult with Intensity = 1 / (1 - AmbientMult), so\n"
				"the environment light keeps its full capture brightness.");
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
					"How much of the raw Skylighting sky openness is reused as an ambient occlusion\n"
					"factor on the whole environment term. 1.0 matches the SSRT fallback's\n"
					"Occlusion Strength default. Screen Space GI occlusion, when it is loaded, is\n"
					"always multiplied in on top of this, exactly as the fallback did.\n"
					"\n"
					"Raw openness means before Skylighting's upward-normal brightness boost and its\n"
					"Minimum Diffuse Visibility floor, so upward-facing surfaces darken too - the\n"
					"ground at a wall junction, not just the wall. The sky part of the cubemap is\n"
					"already attenuated by the boosted visibility, so it ends up carrying openness\n"
					"twice where the fallback had two independent signals. If eaves and junctions\n"
					"now read darker than the SSRT reference, lower this towards 0.5.");
			}
		}

		ImGui::SliderFloat("AO Exponent", &settings.AOExponent, 1.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Total power of the composite's multi-bounce AO factor on the environment term.\n"
				"\n"
				"1.5 is the SSRT diffuse path: its cubemap ambient is added to MAIN before the\n"
				"deferred composite runs, so it pays that factor once itself and once more at half\n"
				"power inside the composite. This term is added after the composite has already\n"
				"applied its half power, so without the extra exponent it pays it only once and\n"
				"reads about 25 percent brighter in creases than the SSRT reference.\n"
				"\n"
				"1.0 is the behaviour before this control existed. Raise towards 2.0 for deeper\n"
				"creases.");
		}

		if (!screenSpaceGI.loaded || !screenSpaceGI.settings.Enabled) {
			ImGui::TextColored({ 1, 0.65f, 0, 1 }, "Screen Space GI is off: only Skylighting occlusion is applied.");
		}

		ImGui::Checkbox("Enclosed Hue Fallback", (bool*)&settings.EnclosureFallback);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"In closed-off places, fade the environment light back to the vanilla ambient\n"
				"colour instead of only dimming the cubemap.\n"
				"\n"
				"The SSRT fallback does not dim the cubemap where the sky is blocked: rays that hit\n"
				"nearby geometry take that geometry's on-screen colour instead, so an enclosed spot\n"
				"is lit by bounced local light. Dimming a sky-blue cubemap without changing its hue\n"
				"leaves interiors of arches, eaves and alcoves dark but still visibly blue. The\n"
				"vanilla ambient term is the right stand-in: it already carries the true ambient\n"
				"brightness from the G-buffer and has no sky blue in it.\n"
				"\n"
				"Open areas are unaffected. Turn off to see the blue wash this removes.");
		}

		{
			auto hueGuard = Util::DisableGuard(!settings.EnclosureFallback);
			ImGui::SliderFloat("Hue Falloff", &settings.HueFalloff, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Exponent on the openness signal that drives the fallback. Above 1.0 the\n"
					"fallback reaches further out of enclosed areas (more aggressive de-blueing);\n"
					"below 1.0 it stays confined to the most enclosed spots. 1.0 is linear.");
			}
		}

		///////////////////////////////
		ImGui::SeparatorText("Contact Occlusion");

		ImGui::Checkbox("Enable Contact Occlusion", (bool*)&settings.EnableContactOcclusion);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Near-field ambient occlusion from the full-resolution depth buffer, ten taps per\n"
				"pixel.\n"
				"\n"
				"This covers the last scale the other occlusion sources cannot reach. Skylighting\n"
				"works on a metre-scale probe grid and Screen Space GI is half-resolution with a\n"
				"large radius, while the SSRT fallback this feature reproduces occluded its rays\n"
				"at full resolution and centimetre scale. That difference is what shows up as\n"
				"missing absorption where hair meets a face, where cloth meets skin, and around\n"
				"a window frame behind a character.\n"
				"\n"
				"Applies both as a multiplier on the environment light and as part of the\n"
				"enclosure signal, so contacts shift towards the local ambient colour rather than\n"
				"just darkening. Independent of Screen Space GI: they act at different scales and\n"
				"compose, so leaving both on is correct.");
		}

		{
			auto contactGuard = Util::DisableGuard(!settings.EnableContactOcclusion);

			ImGui::SliderFloat("Contact Radius", &settings.ContactRadius, 2.0f, 60.0f, "%.1f cm", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"World-space radius of the search, in centimetres. 15 cm (default) is contact\n"
					"scale: strand-to-skin, cloth-to-skin, frame-to-wall. Raising it starts to\n"
					"overlap Screen Space GI's job and costs temporal stability, because the same\n"
					"ten taps then have to cover a larger area.\n"
					"\n"
					"The pixel radius is derived from this per pixel and clamped to 2-64 pixels, so\n"
					"distant geometry keeps distinct taps and a near-field surface cannot turn this\n"
					"into a full-screen pass.");
			}

			ImGui::SliderFloat("Contact Strength", &settings.ContactStrength, 0.0f, 2.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Scales the occlusion. At 1.0 a 90-degree corner lands near 0.4 occlusion and a\n"
					"tight contact such as hair against skin reaches full absorption, matching the\n"
					"SSRT fallback this reproduces. The upper half of the slider is headroom rather\n"
					"than the working range; use 0.5 for the levels this had before that\n"
					"recalibration. 0.0 disables the effect without removing its cost - use the\n"
					"checkbox for that.");
			}
		}

		///////////////////////////////
		ImGui::SeparatorText("Advanced");

		ImGui::Checkbox("Enable in Interiors", (bool*)&settings.EnableInterior);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Interiors use a separate composite permutation without Skylighting, so only the\n"
				"no-sky cubemap is used - which is the whole of the environment light indoors\n"
				"anyway, and is exactly what Skylighting would report there: its probe volume\n"
				"returns full visibility inside, so the sky visibility and enclosure terms would\n"
				"both be constant 1.\n"
				"\n"
				"On by default. Interiors are most of the play time, and leaving them on the\n"
				"vanilla ambient path meant the directional environment light simply did not\n"
				"exist in dungeons, caves and houses. Contact occlusion and the enclosure hue\n"
				"fallback still work there; they run off contact and Screen Space GI, not off\n"
				"Skylighting.");
		}

		ImGui::SliderFloat("Normalization", &settings.Normalization, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Rescales the cubemap brightness towards the vanilla directional ambient\n"
				"brightness, using the SSRT fallback's formula verbatim. 0.0 (default) takes the\n"
				"brightness straight from the cubemap capture, matching the fallback's own\n"
				"Cubemap Normalization default. 1.0 makes the Blend slider energy neutral.");
		}

		ImGui::Checkbox("Linear Composite", (bool*)&settings.LinearComposite);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Add the environment light to the direct light in linear space, as the SSRT\n"
				"diffuse composite does, and fade the vanilla ambient out with Blend instead of\n"
				"cross-fading against it. Blend 0 stays bit-identical to vanilla either way.\n"
				"\n"
				"Off routes the environment light through the vanilla deferred composite, which\n"
				"converts both terms to gamma, adds them, and converts back. Without Linear\n"
				"Lighting that soft-add is pow(a^(1/1.6) + b^(1/1.6), 1.6): it brightens the sum\n"
				"by up to 30% and pulls every channel towards the ambient hue, so the image reads\n"
				"brighter, flatter and less saturated. That was the main colour error against the\n"
				"SSRT reference, so this is On by default. It makes no difference when Linear\n"
				"Lighting is enabled, where both conversions are identities.");
		}

	}
}
