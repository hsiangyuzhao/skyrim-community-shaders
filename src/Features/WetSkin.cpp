#include "WetSkin.h"

#include "Features/Skin.h"
#include "Features/WetnessEffects.h"
#include "Util.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	WetSkin::Settings,
	Model,
	ExtraWetness,
	SwimWeight,
	RainWeight,
	SweatWeight,
	HoldTime,
	DryTime,
	RoughnessLow,
	RoughnessFull,
	FullWetnessStrength,
	WetParams,
	PatchSoftness,
	PatchContrast,
	Darkening,
	FilmStrength,
	DropletAmount,
	DropletDensity,
	DropletSize,
	DropletSizeRandom,
	FaceDroplets,
	DropletNormal,
	DropletRim,
	DropletTrails,
	RainDroplets,
	StreakAmount,
	StreakLength,
	StreakWidth,
	StreakContrast)

namespace
{
	json g_legacySkin;
	json g_legacyWetness;
	bool g_legacySkinPending = false;
	bool g_legacyWetnessPending = false;

	void Tooltip(std::initializer_list<const char*> a_lines)
	{
		if (auto _tt = Util::HoverTooltipWrapper()) {
			for (const char* line : a_lines)
				ImGui::TextUnformatted(line);
		}
	}
}

void WetSkin::StashLegacySkinJson(const json& a_skin)
{
	if (!a_skin.is_object())
		return;
	g_legacySkin = a_skin;
	g_legacySkinPending = true;
}

void WetSkin::StashLegacyWetnessJson(const json& a_wetness)
{
	if (!a_wetness.is_object())
		return;
	g_legacyWetness = a_wetness;
	g_legacyWetnessPending = true;
}

WetSkin::GPUParams WetSkin::GetGPUParams() const
{
	const auto& s = settings;
	GPUParams g{};
	const bool isNew = !IsOriginal();
	g.p[0] = float4(isNew ? 1.0f : 0.0f, std::clamp(s.ExtraWetness, 0.0f, 1.0f), std::clamp(s.SwimWeight, 0.0f, 1.0f), std::clamp(s.RainWeight, 0.0f, 1.0f));
	// Sheen: roughness never below 0.2 (anti-mirror), strength at most 2.5.
	g.p[1] = float4(std::clamp(s.SweatWeight, 0.0f, 1.0f), std::clamp(s.RoughnessLow, 0.2f, 0.6f), std::clamp(s.RoughnessFull, 0.2f, 0.6f), std::clamp(s.FullWetnessStrength, 0.0f, 2.5f));
	g.p[2] = float4(std::clamp(s.PatchSoftness, 0.05f, 0.6f), std::clamp(s.PatchContrast, 0.0f, 1.0f), std::clamp(s.DropletAmount, 0.0f, 1.0f), std::clamp(s.DropletDensity, 0.0f, 1.0f));
	g.p[3] = float4(std::clamp(s.DropletSize, 0.25f, 4.0f), std::clamp(s.DropletSizeRandom, 0.0f, 1.0f), std::clamp(s.FaceDroplets, 0.0f, 1.0f), std::clamp(s.DropletRim, 0.0f, 1.0f));
	g.p[4] = float4(std::clamp(s.DropletTrails, 0.0f, 1.0f), std::clamp(s.StreakAmount, 0.0f, 1.0f), std::clamp(s.StreakLength, 0.1f, 4.0f), std::clamp(s.StreakWidth, 0.1f, 3.0f));
	g.p[5] = float4(std::clamp(s.StreakContrast, 0.0f, 1.0f), std::clamp(s.Darkening, 0.0f, 2.0f), std::clamp(s.DropletNormal, 0.0f, 2.0f), std::clamp(s.RainDroplets, 0.0f, 1.0f));
	return g;
}

void WetSkin::DrawSettings()
{
	auto& s = settings;
	if (!globals::features::skin.loaded || !globals::features::skin.settings.EnableSkin)
		ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Needs Advanced Skin enabled.");

	const char* models[] = { "New", "Original (pre-42c)" };
	int model = IsOriginal() ? 1 : 0;
	if (ImGui::Combo("Wet Skin Model", &model, models, IM_ARRAYSIZE(models)))
		s.Model = static_cast<uint32_t>(model);
	Tooltip({ "New: soft sheen that gets strong when soaked (never a mirror), droplets, rivulets, darkening, wet patches.",
		"Original (pre-42c): skin wetness exactly as before the wetness work (41b), and the 42c character options",
		"(wet film, darkening, rain on skin) switched off for gear too. Only safety limits are kept." });

	ImGui::SliderFloat("Extra Wetness (manual)", &s.ExtraWetness, 0.0f, 2.0f, "%.2f");
	Tooltip({ "Makes every character this wet, on top of swimming, rain and sweat. New uses 0-1 (1 = soaked); Original reads 0-2 as before." });

	if (IsOriginal()) {
		ImGui::SeparatorText("Original (pre-42c)");
		ImGui::SliderFloat("Wetness Film Strength", &s.FilmStrength, 0.0f, 3.0f, "%.2f");
		Tooltip({ "Advanced Skin's water film, as before. Its energy is capped at 2 (no mirror-like reflections)." });
		ImGui::SliderFloat("Wetness Perlin Noise Scale", &s.WetParams.x, 0.0f, 1024.0f, "%1.f");
		ImGui::SliderFloat("Wetness Perlin Noise Lacunarity", &s.WetParams.y, 0.0f, 2.0f, "%.1f");
		ImGui::SliderFloat("Wetness Perlin Noise Persistence", &s.WetParams.z, 0.0f, 20.0f, "%.2f");
		ImGui::SliderFloat("Wetness Normal Scale", &s.WetParams.w, 0.0f, 20.0f, "%.1f");
		ImGui::TextDisabled("Sweat thresholds, Dynamic Wetness and the fade-out time are on the Advanced Skin page.");
		return;
	}

	ImGui::SeparatorText("Wetness sources");
	ImGui::SliderFloat("Swimming", &s.SwimWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet the skin under the water line gets. 1 = soaked." });
	ImGui::SliderFloat("Stay Wet After Leaving Water (s)", &s.HoldTime, 0.0f, 120.0f, "%.0f s");
	ImGui::SliderFloat("Dry-off Time (s)", &s.DryTime, 5.0f, 600.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
	Tooltip({ "After the hold, how long the skin takes to dry. Droplets go first, the sheen last." });
	ImGui::SliderFloat("Rain", &s.RainWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet rain makes skin (only where the sky reaches it: under a roof it stays dry)." });
	ImGui::SliderFloat("Sweat", &s.SweatWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet sweat (low stamina, or Dynamic Wetness) makes skin. Thresholds are on the Advanced Skin page." });

	ImGui::SeparatorText("Sheen");
	ImGui::SliderFloat("Wet Look Strength at Full Wetness", &s.FullWetnessStrength, 0.0f, 2.5f, "%.2f");
	Tooltip({ "How strong the water sheen is on soaked skin. It grows faster than the wetness: barely there when damp,", "clearly glossy when soaked. 1 = physically plain water; the default is a bit stronger, like skin straight out of a pool." });
	ImGui::SliderFloat("Sheen Roughness (slightly wet)", &s.RoughnessLow, 0.2f, 0.6f, "%.2f");
	ImGui::SliderFloat("Sheen Roughness (full wetness)", &s.RoughnessFull, 0.2f, 0.6f, "%.2f");
	Tooltip({ "Lower = smaller, sharper highlights; higher = broader, softer sheen. Never below 0.2, so skin never turns into a mirror." });
	ImGui::SliderFloat("Darkening", &s.Darkening, 0.0f, 2.0f, "%.2f");
	Tooltip({ "How much darker (and a little more saturated) soaked skin gets. Follows the wet patches, no blotches." });

	ImGui::SeparatorText("Wet patches");
	ImGui::SliderFloat("Patch Contrast", &s.PatchContrast, 0.0f, 1.0f, "%.2f");
	Tooltip({ "0 = an even film. Higher = the wet areas form patches while the skin is only partly wet;", "they grow and merge as it gets wetter, and are even at full wetness." });
	ImGui::SliderFloat("Patch Softness", &s.PatchSoftness, 0.05f, 0.6f, "%.2f");
	Tooltip({ "How soft the patch edges are. Higher = smoother, no hard wet/dry lines." });
	ImGui::SliderFloat("Patch Scale", &s.WetParams.x, 1.0f, 1024.0f, "%1.f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Patch Lacunarity", &s.WetParams.y, 0.0f, 2.0f, "%.1f");
	ImGui::SliderFloat("Patch Persistence", &s.WetParams.z, 0.0f, 20.0f, "%.2f");
	Tooltip({ "The Perlin noise the patches come from (shared with the Original model). Lacunarity 2 / Persistence 0.5 gives finer patches." });

	ImGui::SeparatorText("Droplets");
	ImGui::SliderFloat("Droplet Amount", &s.DropletAmount, 0.0f, 1.0f, "%.2f");
	Tooltip({ "Water beads on the skin: a tiny bright point each, with a slightly darker edge. Mostly when coming out of the water", "and when soaked; they disappear one by one as the skin dries. Seen up close only (they fade out when smaller than a pixel)." });
	ImGui::SliderFloat("Droplet Density", &s.DropletDensity, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Droplet Size", &s.DropletSize, 0.25f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Droplet Size Variation", &s.DropletSizeRandom, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Droplets on the Face", &s.FaceDroplets, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Droplet Normal Strength", &s.DropletNormal, 0.0f, 2.0f, "%.2f");
	Tooltip({ "How round the beads look (their own little dome, inside the bead only). 0 = flat spots of shine." });
	ImGui::SliderFloat("Droplet Edge", &s.DropletRim, 0.0f, 1.0f, "%.2f");
	ImGui::SliderFloat("Droplet Trails", &s.DropletTrails, 0.0f, 1.0f, "%.2f");
	Tooltip({ "A short wet trail running down from each bead." });
	ImGui::SliderFloat("Droplets from Rain", &s.RainDroplets, 0.0f, 1.0f, "%.2f");
	Tooltip({ "Rain makes fewer beads than coming out of the water." });

	ImGui::SeparatorText("Rivulets");
	ImGui::SliderFloat("Rivulet Amount", &s.StreakAmount, 0.0f, 1.0f, "%.2f");
	Tooltip({ "Faint downward water trails, following the body's actual orientation. Shinier and slightly darker trails;", "present when soaked, while drying after leaving the water, and in rain." });
	ImGui::SliderFloat("Rivulet Length", &s.StreakLength, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Rivulet Width", &s.StreakWidth, 0.1f, 3.0f, "%.2f");
	ImGui::SliderFloat("Rivulet Contrast", &s.StreakContrast, 0.0f, 1.0f, "%.2f");
	Tooltip({ "Higher = fewer, crisper trails and the film between them a little less wet. Never dry holes." });
}

void WetSkin::LoadSettings(json& o_json)
{
	settings = o_json;
	// Own settings exist: whatever 43b left in Advanced Skin / Wetness Effects is not used.
	g_legacySkinPending = false;
	g_legacyWetnessPending = false;
}

void WetSkin::SaveSettings(json& o_json)
{
	o_json = settings;
}

void WetSkin::RestoreDefaultSettings()
{
	settings = {};
	// First start after 43b: take the skin wetness options from where they used to live.
	if (g_legacySkinPending) {
		g_legacySkinPending = false;
		const auto& j = g_legacySkin;
		settings.ExtraWetness = j.value("ExtraSkinWetness", settings.ExtraWetness);
		settings.FilmStrength = j.value("WetnessFilmStrength", settings.FilmStrength);
		if (j.contains("WetParams")) {
			try {
				settings.WetParams = j["WetParams"].get<float4>();
			} catch (...) {
			}
		}
		// 43b "Uniform (natural)" (1) is gone: New. "Advanced Skin noise (original)" (0): Original.
		if (j.contains("SkinWetnessCoverage"))
			settings.Model = j.value("SkinWetnessCoverage", 1u) == 0 ? static_cast<uint32_t>(Model::Original) : static_cast<uint32_t>(Model::New);
		logger::info("[Wet Skin] Settings migrated from Advanced Skin (model {})", IsOriginal() ? "Original" : "New");
	}
	if (g_legacyWetnessPending) {
		g_legacyWetnessPending = false;
		settings.Darkening = g_legacyWetness.value("PorositySkin", settings.Darkening);
	}
}

void WetSkin::DataLoaded()
{
	g_legacySkinPending = false;
	g_legacyWetnessPending = false;
}
