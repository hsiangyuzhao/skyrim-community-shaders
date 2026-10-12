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
	StreakContrast,
	HairEnabled,
	HairDarkening,
	HairSharpness,
	HairHighlightGain,
	HairSecondaryLoss,
	HairScatterLoss,
	HairClumping,
	HairClumpCount,
	HairClumpShift,
	HairGapDarkening,
	HairDryTime,
	HairSweatShare,
	GearEnabled,
	GearModel,
	GearKeywords,
	GearKeywordTrust,
	DebugClassTint,
	GearSheen,
	GearPatchContrast,
	GearWicking,
	GearSweatShare,
	GearCloth,
	GearLeather,
	GearMetal,
	GearFur,
	LeatherDroplets,
	LeatherRivulets,
	MetalDroplets,
	MetalRivulets)

namespace
{
	json g_legacySkin;
	json g_legacyWetness;
	bool g_legacySkinPending = false;
	bool g_legacyWetnessPending = false;

	// (44c) The 42c character options (Wetness Effects) become the Gear and Hair defaults, once:
	// porosity -> darkening, film strength -> overall sheen, film roughness -> leather floor, and
	// switches the user had turned off stay off.
	void MigrateGearFrom42c(WetSkin::Settings& a_s, const json& a_w)
	{
		const float cloth = a_w.value("PorosityCloth", a_s.GearCloth.x);
		const float other = a_w.value("PorosityOther", a_s.GearFur.x);
		a_s.GearCloth.x = cloth;
		a_s.GearLeather.x = cloth * 0.55f;
		a_s.GearMetal.x = a_w.value("PorosityMetal", a_s.GearMetal.x);
		a_s.GearFur.x = other;
		a_s.HairDarkening = std::clamp(other, 0.0f, 1.5f);
		a_s.GearSheen = a_w.value("WetFilmStrength", a_s.GearSheen);
		a_s.GearLeather.z = std::max(0.2f, a_w.value("WetFilmRoughness", 0.18f) + 0.12f);
		if (a_w.value("EnableWetDarkening", 1u) == 0) {
			a_s.GearCloth.x = a_s.GearLeather.x = a_s.GearMetal.x = a_s.GearFur.x = 0.0f;
			a_s.HairDarkening = 0.0f;
		}
		if (a_w.value("EnableWetFilm", 1u) == 0)
			a_s.GearSheen = 0.0f;
		logger::info("[Character Wetness] Gear and hair settings taken from the 42c Wetness Effects options");
	}

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

	// (44c) Hair and gear (Skin.hlsli wetCharParams). Original: hair and gear off (the shader does
	// not read these then; zeroed so nothing can leak in).
	const auto classParams = [](const float4& a_v) {
		return float4(std::clamp(a_v.x, 0.0f, 2.0f), std::clamp(a_v.y, 0.0f, 2.0f), std::clamp(a_v.z, 0.2f, 0.8f), std::clamp(a_v.w, 0.25f, 4.0f));
	};
	const bool keywords = s.GearKeywords != 0;
	const float gearMode = !isNew || !s.GearEnabled ? 0.0f : (s.GearModel == 1 ? 2.0f : 1.0f);
	g.c[0] = float4(std::clamp(s.HoldTime, 0.0f, 600.0f), std::clamp(s.DryTime, 1.0f, 1200.0f), std::clamp(s.HairDryTime, 0.25f, 4.0f), std::clamp(s.HairSweatShare, 0.0f, 1.0f));
	g.c[1] = float4(isNew && s.HairEnabled ? 1.0f : 0.0f, std::clamp(s.HairDarkening, 0.0f, 1.5f), std::clamp(s.HairSharpness, 0.4f, 1.0f), std::clamp(s.HairHighlightGain, 0.5f, 1.6f));
	g.c[2] = float4(std::clamp(s.HairSecondaryLoss, 0.0f, 1.0f), std::clamp(s.HairScatterLoss, 0.0f, 1.0f), std::clamp(s.HairClumping, 0.0f, 1.0f), std::clamp(s.HairClumpCount, 16.0f, 128.0f));
	g.c[3] = float4(std::clamp(s.HairClumpShift, 0.0f, 0.4f), std::clamp(s.HairGapDarkening, 0.0f, 0.7f), isNew && s.DebugClassTint ? 1.0f : 0.0f, 0.0f);
	g.c[4] = float4(gearMode, std::clamp(s.GearSheen, 0.0f, 3.0f), std::clamp(s.GearPatchContrast, 0.0f, 1.0f), std::clamp(s.GearWicking, 0.0f, 16.0f));
	g.c[5] = float4(std::clamp(s.GearSweatShare, 0.0f, 1.0f), keywords ? std::clamp(s.GearKeywordTrust, 0.0f, 1.0f) : 0.0f, std::clamp(s.LeatherDroplets, 0.0f, 1.0f), std::clamp(s.LeatherRivulets, 0.0f, 1.0f));
	g.c[6] = float4(std::clamp(s.MetalDroplets, 0.0f, 1.0f), std::clamp(s.MetalRivulets, 0.0f, 1.0f), 0.0f, 0.0f);
	g.c[7] = classParams(s.GearCloth);
	g.c[8] = classParams(s.GearLeather);
	g.c[9] = classParams(s.GearMetal);
	g.c[10] = classParams(s.GearFur);
	g.c[11] = float4(0.0f, 0.0f, 0.0f, 0.0f);
	return g;
}

float WetSkin::MaxDryMultiplier() const
{
	const auto& s = settings;
	float m = 1.0f;
	if (s.HairEnabled)
		m = std::max(m, std::clamp(s.HairDryTime, 0.25f, 4.0f));
	if (s.GearEnabled) {
		for (const auto* c : { &s.GearCloth, &s.GearLeather, &s.GearMetal, &s.GearFur })
			m = std::max(m, std::clamp(c->w, 0.25f, 4.0f));
	}
	return m;
}

void WetSkin::DrawSettings()
{
	auto& s = settings;
	if (!globals::features::skin.loaded || !globals::features::skin.settings.EnableSkin)
		ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Needs Advanced Skin enabled.");
	if (!globals::features::wetnessEffects.loaded)
		ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Hair and gear need Wetness Effects enabled (skin works without it).");

	const char* models[] = { "New", "Original (pre-42c)" };
	int model = IsOriginal() ? 1 : 0;
	if (ImGui::Combo("Wetness Model", &model, models, IM_ARRAYSIZE(models)))
		s.Model = static_cast<uint32_t>(model);
	Tooltip({ "New: skin, hair and gear get wet together and dry at their own pace (cloth slowest). Soft sheen",
		"that gets strong when soaked (never a mirror), droplets, rivulets, darkening per material.",
		"Original (pre-42c): every character exactly as before the wetness work (41b): skin, hair and gear.",
		"Only safety limits are kept." });

	ImGui::SliderFloat("Extra Wetness (manual)", &s.ExtraWetness, 0.0f, 2.0f, "%.2f");
	Tooltip({ "Makes every character this wet (skin, hair and gear), on top of swimming, rain and sweat.",
		"New uses 0-1 (1 = soaked); Original reads 0-2 as before (skin and hair only)." });

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

	ImGui::SeparatorText("Wetness sources (skin, hair, gear)");
	ImGui::SliderFloat("Swimming", &s.SwimWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet everything under the water line gets. 1 = soaked." });
	ImGui::SliderFloat("Stay Wet After Leaving Water (s)", &s.HoldTime, 0.0f, 120.0f, "%.0f s");
	ImGui::SliderFloat("Dry-off Time (s)", &s.DryTime, 5.0f, 600.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
	Tooltip({ "For skin. Hair and each kind of gear multiply both times by their own Dry Time x (cloth 2.5x: stays wet longest)." });
	ImGui::SliderFloat("Rain", &s.RainWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet rain makes skin and hair (only where the sky reaches them: under a roof they stay dry).",
		"Gear follows the rain wetness of Wetness Effects." });
	ImGui::SliderFloat("Sweat", &s.SweatWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet sweat (low stamina, or Dynamic Wetness) makes skin. Hair and gear take their own share of it.",
		"Thresholds are on the Advanced Skin page." });

	if (ImGui::CollapsingHeader("Skin", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::SeparatorText("Sheen");
		ImGui::SliderFloat("Wet Look Strength at Full Wetness", &s.FullWetnessStrength, 0.0f, 2.5f, "%.2f");
		Tooltip({ "How strong the water sheen is on soaked skin (gear uses it too). It grows faster than the wetness: barely there when damp,",
			"clearly glossy when soaked. 1 = physically plain water; the default is a bit stronger, like skin straight out of a pool." });
		ImGui::SliderFloat("Sheen Roughness (slightly wet)", &s.RoughnessLow, 0.2f, 0.6f, "%.2f");
		ImGui::SliderFloat("Sheen Roughness (full wetness)", &s.RoughnessFull, 0.2f, 0.6f, "%.2f");
		Tooltip({ "Lower = smaller, sharper highlights; higher = broader, softer sheen. Never below 0.2, so skin never turns into a mirror." });
		ImGui::SliderFloat("Darkening", &s.Darkening, 0.0f, 2.0f, "%.2f");
		Tooltip({ "How much darker (and a little more saturated) soaked skin gets. Follows the wet patches, no blotches." });

		ImGui::SeparatorText("Wet patches");
		ImGui::SliderFloat("Patch Contrast", &s.PatchContrast, 0.0f, 1.0f, "%.2f");
		Tooltip({ "0 = an even film. Higher = the wet areas form patches while the skin is only partly wet;",
			"they grow and merge as it gets wetter, and are even at full wetness." });
		ImGui::SliderFloat("Patch Softness", &s.PatchSoftness, 0.05f, 0.6f, "%.2f");
		Tooltip({ "How soft the patch edges are (gear too). Higher = smoother, no hard wet/dry lines." });
		ImGui::SliderFloat("Patch Scale", &s.WetParams.x, 1.0f, 1024.0f, "%1.f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Patch Lacunarity", &s.WetParams.y, 0.0f, 2.0f, "%.1f");
		ImGui::SliderFloat("Patch Persistence", &s.WetParams.z, 0.0f, 20.0f, "%.2f");
		Tooltip({ "The Perlin noise the patches come from (shared with the Original model and gear). Lacunarity 2 / Persistence 0.5 gives finer patches." });

		ImGui::SeparatorText("Droplets");
		ImGui::SliderFloat("Droplet Amount", &s.DropletAmount, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Water beads on the skin: a tiny bright point each, with a slightly darker edge. Mostly when coming out of the water",
			"and when soaked; they disappear one by one as the skin dries. Seen up close only (they fade out when smaller than a pixel)." });
		ImGui::SliderFloat("Droplet Density", &s.DropletDensity, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Also scales the beads on metal and leather gear." });
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
		Tooltip({ "Faint downward water trails, following the body's actual orientation. Shinier and slightly darker trails;",
			"present when soaked, while drying after leaving the water, and in rain." });
		ImGui::SliderFloat("Rivulet Length", &s.StreakLength, 0.1f, 4.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		ImGui::SliderFloat("Rivulet Width", &s.StreakWidth, 0.1f, 3.0f, "%.2f");
		ImGui::SliderFloat("Rivulet Contrast", &s.StreakContrast, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Higher = fewer, crisper trails and the film between them a little less wet. Never dry holes." });
	}

	if (ImGui::CollapsingHeader("Hair", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Wet Hair", reinterpret_cast<bool*>(&s.HairEnabled));
		Tooltip({ "Off: hair never gets wet (New model). Hair never gets a mirror-like water film either way:",
			"its own highlight model gets wet, and what the reflections see of it stays as when dry." });
		ImGui::BeginDisabled(!s.HairEnabled);
		ImGui::SliderFloat("Hair Darkening", &s.HairDarkening, 0.0f, 1.5f, "%.2f");
		Tooltip({ "How much darker soaked hair gets (water fills the gaps between the strands). 0.8: brown hair about a third darker." });
		ImGui::SliderFloat("Highlight Sharpness", &s.HairSharpness, 0.4f, 1.0f, "%.2f");
		Tooltip({ "How narrow the main highlight gets when soaked (1 = unchanged). Narrower = brighter at its centre.",
			"Never narrower than about 6 degrees, and wider again where the hair turns quickly on screen (no sparkle)." });
		ImGui::SliderFloat("Highlight Strength", &s.HairHighlightGain, 0.5f, 1.6f, "%.2f");
		ImGui::SliderFloat("Secondary Highlight Loss", &s.HairSecondaryLoss, 0.0f, 1.0f, "%.2f");
		Tooltip({ "The second, coloured highlight mostly disappears on wet hair. 0.65 = a third of it is left." });
		ImGui::SliderFloat("Scatter Loss", &s.HairScatterLoss, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Less light bouncing between the strands (Marschner)." });
		ImGui::SeparatorText("Clumps");
		ImGui::SliderFloat("Clumping", &s.HairClumping, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Wet hair sticks together in clumps: the highlight breaks into streaks and the gaps get darker.",
			"Only where a clump is at least a few pixels wide (it fades out with distance). Needs Hair Specular." });
		ImGui::SliderFloat("Clump Count", &s.HairClumpCount, 16.0f, 128.0f, "%.0f");
		Tooltip({ "Clumps across the hair texture. Higher = finer clumps." });
		ImGui::SliderFloat("Clump Streakiness", &s.HairClumpShift, 0.0f, 0.4f, "%.2f");
		ImGui::SliderFloat("Gap Darkening", &s.HairGapDarkening, 0.0f, 0.7f, "%.2f");
		ImGui::SeparatorText("Drying");
		ImGui::SliderFloat("Hair Dry Time x", &s.HairDryTime, 0.25f, 4.0f, "%.2f");
		Tooltip({ "Multiplies the hold and dry-off times for hair. 1.5 = hair stays wet half as long again as skin." });
		ImGui::SliderFloat("Hair Sweat Share", &s.HairSweatShare, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Gear", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("Wet Gear", reinterpret_cast<bool*>(&s.GearEnabled));
		Tooltip({ "Off: armour, clothes and weapons never get wet (New model)." });
		ImGui::BeginDisabled(!s.GearEnabled);
		const char* gearModels[] = { "New (per material)", "42c (as in 44b)" };
		int gearModel = s.GearModel == 1 ? 1 : 0;
		if (ImGui::Combo("Gear Model", &gearModel, gearModels, IM_ARRAYSIZE(gearModels)))
			s.GearModel = static_cast<uint32_t>(gearModel);
		Tooltip({ "New: every pixel is classified as cloth, leather or metal (a soft blend, no seams), each with its own darkening,",
			"sheen and drying time; beads on metal and leather. 42c: the 44b gear (Wetness Effects > Characters options), for A/B." });
		ImGui::BeginDisabled(s.GearModel == 1);
		ImGui::Checkbox("Use Item Keywords", reinterpret_cast<bool*>(&s.GearKeywords));
		Tooltip({ "Also look at what each worn item is (clothing, leather or metal armour; animals count as fur) to help the pixel guess.",
			"The pixel still decides the details: a metal buckle on a shirt stays metal." });
		ImGui::BeginDisabled(!s.GearKeywords);
		ImGui::SliderFloat("Item Keyword Trust", &s.GearKeywordTrust, 0.0f, 1.0f, "%.2f");
		ImGui::EndDisabled();
		ImGui::Checkbox("Debug: Tint by Material Class", reinterpret_cast<bool*>(&s.DebugClassTint));
		Tooltip({ "Paints characters by what the wetness thinks they are: red cloth, yellow leather, blue metal, green fur / other, purple hair." });
		ImGui::SliderFloat("Overall Sheen x", &s.GearSheen, 0.0f, 3.0f, "%.2f");
		ImGui::SliderFloat("Gear Patch Contrast", &s.GearPatchContrast, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Lower than skin: cloth soaks in sheets." });
		ImGui::SliderFloat("Cloth Wicking", &s.GearWicking, 0.0f, 16.0f, "%.1f");
		Tooltip({ "How far water creeps up cloth above the water line (game units; 4 = about 6 cm), with an irregular tide mark.",
			"While drying the line moves down: the top dries first, the hem last." });
		ImGui::SliderFloat("Gear Sweat Share", &s.GearSweatShare, 0.0f, 1.0f, "%.2f");

		const auto classUI = [](const char* a_name, float4& a_v, float* a_droplets, float* a_rivulets, const char* a_darkeningTip) {
			if (ImGui::TreeNodeEx(a_name, ImGuiTreeNodeFlags_None)) {
				ImGui::SliderFloat("Darkening", &a_v.x, 0.0f, 2.0f, "%.2f");
				if (a_darkeningTip)
					Tooltip({ a_darkeningTip });
				ImGui::SliderFloat("Sheen", &a_v.y, 0.0f, 2.0f, "%.2f");
				ImGui::SliderFloat("Roughness Floor", &a_v.z, 0.2f, 0.8f, "%.2f");
				Tooltip({ "The wet sheen is never smoother than this (and never below 0.2: no mirror)." });
				ImGui::SliderFloat("Dry Time x", &a_v.w, 0.25f, 4.0f, "%.2f");
				if (a_droplets)
					ImGui::SliderFloat("Droplets", a_droplets, 0.0f, 1.0f, "%.2f");
				if (a_rivulets)
					ImGui::SliderFloat("Rivulets", a_rivulets, 0.0f, 1.0f, "%.2f");
				ImGui::TreePop();
			}
		};
		classUI("Cloth", s.GearCloth, nullptr, nullptr, nullptr);
		classUI("Leather", s.GearLeather, &s.LeatherDroplets, &s.LeatherRivulets, nullptr);
		classUI("Metal", s.GearMetal, &s.MetalDroplets, &s.MetalRivulets, "Bare metal does not darken when wet (0). Raise only for painted or dirty metal.");
		classUI("Fur / Other", s.GearFur, nullptr, nullptr, nullptr);
		ImGui::EndDisabled();
		ImGui::EndDisabled();
	}
}

void WetSkin::LoadSettings(json& o_json)
{
	settings = o_json;
	// (44c) Saved before the Gear section existed: take it from the 42c options once.
	if (!o_json.contains("GearCloth") && g_legacyWetness.is_object()) {
		try {
			MigrateGearFrom42c(settings, g_legacyWetness);
		} catch (...) {
		}
	}
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
		try {
			MigrateGearFrom42c(settings, g_legacyWetness);
		} catch (...) {
		}
	}
}

void WetSkin::DataLoaded()
{
	g_legacySkinPending = false;
	g_legacyWetnessPending = false;
}
