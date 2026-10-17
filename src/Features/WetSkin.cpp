#include "WetSkin.h"

#include "Features/Skin.h"
#include "Features/WetnessEffects.h"
#include "State.h"
#include "Util.h"
#include "imgui_stdlib.h"

#include <fstream>

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
	RivuletContrast2,
	RivuletBetweenDulling,
	FaceRivulets,
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
	MetalRivulets,
	TranslucentUnderWater,
	SkipInvisibleWork,
	FasterNoiseLOD,
	UnderWaterTint)

namespace
{
	// (47f) The macro above is at nlohmann's 64-argument limit: the Physical model's settings are written and read
	// here, next to it (settings JSON and presets alike).
	void PhysicalToJson(json& o_json, const WetSkin::Settings& a_s)
	{
		o_json["SweatBuildup"] = a_s.SweatBuildup;
		o_json["SweatDrying"] = a_s.SweatDrying;
		o_json["RainWetting"] = a_s.RainWetting;
		o_json["RainDrying"] = a_s.RainDrying;
		o_json["SwimDrying"] = a_s.SwimDrying;
		o_json["MaxRainWetness"] = a_s.MaxRainWetness;
		o_json["ManualLookType"] = a_s.ManualLookType;
		o_json["OilRepelsWater"] = a_s.OilRepelsWater;
	}

	void PhysicalFromJson(const json& a_json, WetSkin::Settings& o_s)
	{
		if (!a_json.is_object())
			return;
		const auto number = [&](const char* a_key, float& o_value, float a_min, float a_max) {
			if (auto it = a_json.find(a_key); it != a_json.end() && it->is_number()) {
				const float v = it->get<float>();
				if (std::isfinite(v))
					o_value = std::clamp(v, a_min, a_max);
			}
		};
		number("SweatBuildup", o_s.SweatBuildup, 0.1f, 5.0f);
		number("SweatDrying", o_s.SweatDrying, 0.1f, 5.0f);
		number("RainWetting", o_s.RainWetting, 0.1f, 5.0f);
		number("RainDrying", o_s.RainDrying, 0.1f, 5.0f);
		number("SwimDrying", o_s.SwimDrying, 0.1f, 5.0f);
		number("MaxRainWetness", o_s.MaxRainWetness, 0.0f, 1.0f);
		if (auto it = a_json.find("ManualLookType"); it != a_json.end() && it->is_number_integer())
			o_s.ManualLookType = std::min(it->get<uint32_t>(), static_cast<uint32_t>(WetSkin::ManualLook::Oil));
		if (auto it = a_json.find("OilRepelsWater"); it != a_json.end() && it->is_boolean())
			o_s.OilRepelsWater = it->get<bool>();
	}

	float Smooth(float a_edge0, float a_edge1, float a_x)
	{
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/// (47f) One source's look at level a_level (what the shader sees of it, 0..1).
	struct SourceLook
	{
		float level = 0.0f;
		float beads = 0.0f;       // droplet driver
		float streak = 0.0f;      // rivulet driver
		float film = 1.0f;        // x sheen strength
		float density = 1.0f;     // x droplet density
		float size = 1.0f;        // x droplet size
		float sizeRandom = 1.0f;  // x size variation
		float trails = 1.0f;      // x droplet trails
	};

	/// (47g) The droplet driver for a visible bead count per skin area a_count at bead size a_size (both x the user's
	/// values): the shader's droplet cells shrink with the bead size (cells per uv = 320 / size), so the share of cells
	/// holding a bead is count x size^2.
	/// (47h) Stronger Low-Wetness Look: droplet density x the user's value (the shader clamps the product to 1).
	constexpr float kStrongDensity = 2.2f;

	float BeadDriver(float a_count, float a_size)
	{
		return std::clamp(a_count * a_size * a_size, 0.0f, 1.0f);
	}

	/// Sweat: a few tiny beads at the onset, more and larger toward mid wetness, then they merge into trails and a thin
	/// film. (47g) The count rises gradually and peaks around mid wetness (it saturated at 0.15-0.2 before, with 1.8x
	/// density at low wetness); the size grows with the wetness. Fully wet unchanged (driver 0.6 at size 0.9).
	/// (47h) a_strong "Physical: Stronger Low-Wetness Look": beads are the visible signal from the onset (presence share, no
	/// size^2), 2.2x density, size from a bead that resolves at 1.5 m; trails, streaks and the film come in earlier (reviews/wetness-render).
	SourceLook SweatLook(float a_level, bool a_strong)
	{
		SourceLook o;
		const float l = std::clamp(a_level, 0.0f, 1.0f);
		o.level = l;
		if (a_strong) {
			o.size = std::lerp(0.55f, 0.95f, Smooth(0.0f, 0.7f, l));
			const float count = 0.30f + 0.70f * Smooth(0.02f, 0.30f, l) - 0.25f * Smooth(0.6f, 1.0f, l);
			o.beads = std::clamp(count * Smooth(0.0f, 0.04f, l), 0.0f, 1.0f);
			o.density = kStrongDensity;
			o.sizeRandom = 0.5f;
			o.trails = Smooth(0.15f, 0.5f, l);
			o.streak = 0.7f * Smooth(0.3f, 0.75f, l);
			o.film = std::lerp(0.6f, 1.0f, Smooth(0.3f, 0.9f, l));
			return o;
		}
		o.size = std::lerp(0.3f, 0.9f, Smooth(0.0f, 0.8f, l));
		const float count = (0.6f / 0.81f) * (1.4f * Smooth(0.02f, 0.5f, l) - 0.4f * Smooth(0.55f, 1.0f, l));
		o.beads = BeadDriver(count, o.size);
		o.density = 1.0f;
		o.sizeRandom = 0.5f;
		o.trails = Smooth(0.4f, 0.8f, l);
		o.streak = 0.7f * Smooth(0.55f, 0.95f, l);
		o.film = std::lerp(0.2f, 1.0f, Smooth(0.5f, 1.0f, l));
		return o;
	}

	/// Rain: sparse beads of mixed sizes (the shader puts more on up-facing surfaces), runoff trails, a film in heavy rain.
	/// (47g) The count grows with the water received (it was at its maximum from level 0.15); fully wet unchanged.
	SourceLook RainLook(float a_level, bool a_strong)
	{
		SourceLook o;
		const float l = std::clamp(a_level, 0.0f, 1.0f);
		o.level = l;
		if (a_strong) {  // (47h)
			o.size = std::lerp(0.75f, 1.1f, Smooth(0.05f, 0.7f, l));
			o.beads = std::clamp(0.35f + 0.55f * Smooth(0.02f, 0.5f, l), 0.0f, 1.0f) * Smooth(0.0f, 0.04f, l);
			o.density = kStrongDensity;
			o.sizeRandom = 1.5f;
			o.trails = Smooth(0.1f, 0.45f, l);
			o.streak = Smooth(0.15f, 0.55f, l);
			o.film = std::lerp(0.6f, 1.0f, Smooth(0.25f, 0.8f, l));
			return o;
		}
		o.size = std::lerp(0.6f, 1.1f, Smooth(0.1f, 0.8f, l));
		o.beads = BeadDriver((0.6f / 1.21f) * Smooth(0.02f, 0.8f, l), o.size);
		o.density = 1.0f;
		o.sizeRandom = 1.5f;
		o.trails = Smooth(0.2f, 0.6f, l);
		o.streak = Smooth(0.25f, 0.7f, l);
		o.film = std::lerp(0.3f, 1.0f, Smooth(0.45f, 0.9f, l));
		return o;
	}

	/// Swimming: a thick film with runoff trails; drying, the film breaks into beads, which then shrink and go.
	/// a_phase: 0 just out of the water .. 1 dry (evaporation clock / evaporation time); a_runoff: the runoff left (1 .. 0).
	/// (47g) Late in the drying both the count and the size fall (the small beads evaporate first); the count rose 20x
	/// before (density up while the size shrank). Just out of the water unchanged.
	SourceLook SwimLook(float a_level, float a_phase, float a_runoff, bool a_strong)
	{
		SourceLook o;
		const float l = std::clamp(a_level, 0.0f, 1.0f);
		const float p = std::clamp(a_phase, 0.0f, 1.0f);
		o.level = l;
		if (a_strong) {  // (47h)
			o.film = std::lerp(1.0f, 0.55f, Smooth(0.1f, 0.6f, p));
			o.size = std::lerp(1.1f, 0.6f, Smooth(0.3f, 0.95f, p));
			const float count = std::lerp(0.55f, 0.9f, Smooth(0.05f, 0.3f, p)) * (1.0f - 0.85f * Smooth(0.6f, 1.0f, p));
			o.beads = std::clamp(count, 0.0f, 1.0f) * Smooth(0.0f, 0.05f, l);
			o.density = kStrongDensity;
			o.sizeRandom = 0.7f;
			o.trails = std::lerp(1.0f, 0.5f, Smooth(0.1f, 0.6f, p));
			o.streak = std::max(std::clamp(a_runoff, 0.0f, 1.0f), 0.5f * (1.0f - p)) * Smooth(0.0f, 0.06f, l);
			return o;
		}
		o.film = std::lerp(1.0f, 0.3f, Smooth(0.1f, 0.55f, p));
		o.size = std::lerp(1.1f, 0.45f, Smooth(0.3f, 0.95f, p));
		const float count = (0.5f / 1.21f) * std::lerp(1.0f, 1.6f, Smooth(0.05f, 0.3f, p)) * (1.0f - Smooth(0.5f, 1.0f, p));
		o.beads = BeadDriver(count, o.size) * Smooth(0.0f, 0.12f, l);
		o.density = 1.0f;
		o.sizeRandom = 0.7f;
		o.trails = std::lerp(1.0f, 0.3f, Smooth(0.1f, 0.5f, p));
		o.streak = std::max(std::clamp(a_runoff, 0.0f, 1.0f), 0.25f * (1.0f - p)) * Smooth(0.0f, 0.1f, l);
		return o;
	}

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

	// (46a) The built-in look presets (44k Natural / Drenched) were removed; presets are the user's own files.
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
	// (47f) x: 0 Legacy, 1 Simple, 2 Physical (Skin.hlsli WetSkinNewModel() is x > 0.5, WetPhysical() x > 1.5).
	// (47h) Physical: + 0.25 Stronger Low-Wetness Look, + 0.5 Smaller Visible Bead Size (Skin.hlsli WetStrongLowLook / WetSmallBeads).
	const float physicalBits = IsPhysical() ? (s.PhysicalStrongLowLook ? 0.25f : 0.0f) + (s.PhysicalSmallBeads ? 0.5f : 0.0f) : 0.0f;
	g.p[0] = float4(isNew ? (IsPhysical() ? 2.0f + physicalBits : 1.0f) : 0.0f, std::clamp(s.ExtraWetness, 0.0f, 1.0f), std::clamp(s.SwimWeight, 0.0f, 1.0f), std::clamp(s.RainWeight, 0.0f, 1.0f));
	// Sheen: roughness never below 0.2 (anti-mirror), strength at most 2.5.
	g.p[1] = float4(std::clamp(s.SweatWeight, 0.0f, 1.0f), std::clamp(s.RoughnessLow, 0.2f, 0.6f), std::clamp(s.RoughnessFull, 0.2f, 0.6f), std::clamp(s.FullWetnessStrength, 0.0f, 2.5f));
	g.p[2] = float4(std::clamp(s.PatchSoftness, 0.05f, 0.6f), std::clamp(s.PatchContrast, 0.0f, 1.0f), std::clamp(s.DropletAmount, 0.0f, 1.0f), std::clamp(s.DropletDensity, 0.0f, 1.0f));
	g.p[3] = float4(std::clamp(s.DropletSize, 0.25f, 4.0f), std::clamp(s.DropletSizeRandom, 0.0f, 1.0f), std::clamp(s.FaceDroplets, 0.0f, 1.0f), std::clamp(s.DropletRim, 0.0f, 1.0f));
	g.p[4] = float4(std::clamp(s.DropletTrails, 0.0f, 1.0f), std::clamp(s.StreakAmount, 0.0f, 1.0f), std::clamp(s.StreakLength, 0.1f, 4.0f), std::clamp(s.StreakWidth, 0.1f, 3.0f));
	g.p[5] = float4(std::clamp(s.RivuletContrast2, 0.0f, 1.0f), std::clamp(s.Darkening, 0.0f, 2.0f), std::clamp(s.DropletNormal, 0.0f, 2.0f), std::clamp(s.RainDroplets, 0.0f, 1.0f));

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
	// (44e) z: Rivulets on Face, w: Between-Trail Dulling (the old Contrast's film half).
	g.c[6] = float4(std::clamp(s.MetalDroplets, 0.0f, 1.0f), std::clamp(s.MetalRivulets, 0.0f, 1.0f), std::clamp(s.FaceRivulets, 0.0f, 1.0f), std::clamp(s.RivuletBetweenDulling, 0.0f, 1.0f));
	g.c[7] = classParams(s.GearCloth);
	g.c[8] = classParams(s.GearLeather);
	g.c[9] = classParams(s.GearMetal);
	g.c[10] = classParams(s.GearFur);
	// (47c) x: Skip Invisible Work, y: Faster Noise & Distance LOD (Skin.hlsli WetSkipInvisible / WetFastNoiseLOD).
	g.c[11] = float4(s.SkipInvisibleWork ? 1.0f : 0.0f, s.FasterNoiseLOD ? 1.0f : 0.0f, 0.0f, 0.0f);
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

// ---------------------------------------------------------------------------------------------
// (47f) Physical model
// ---------------------------------------------------------------------------------------------

float WetSkin::PhysicalRunoffTime() const
{
	// Simple holds a character fully wet for Hold Time after leaving the water: the runoff takes about that long.
	return std::clamp(settings.HoldTime * 0.5f, 3.0f, 60.0f);
}

float WetSkin::PhysicalEvaporationTime() const
{
	// Simple is dry after Hold + Dry-off Time: a soaked skin's evaporation takes that long here.
	return std::clamp(settings.HoldTime + settings.DryTime, 10.0f, 1800.0f);
}

float WetSkin::PhysicalSwimLevel(float a_run, float a_runoffTime, float a_evapTime, float a_dryMult)
{
	const float t = std::max(a_run, 0.0f) / std::max(a_dryMult, 0.25f);
	const float runoff = std::exp(-t / std::max(a_runoffTime, 0.1f));
	const float evaporation = std::clamp(1.0f - t / std::max(a_evapTime, 1.0f), 0.0f, 1.0f);
	return kRunoffShare * runoff + (1.0f - kRunoffShare) * evaporation;
}

const WetSkin::PhysicalFrame& WetSkin::GetPhysicalFrame()
{
	const uint32_t frame = globals::state ? globals::state->frameCount : 0u;
	if (physicalFrame.frame == frame)
		return physicalFrame;
	physicalFrame.frame = frame;
	const auto& wetness = globals::features::wetnessEffects;
	physicalFrame.rainIntensity = wetness.loaded ? wetness.GetCharacterRainIntensity() : 0.0f;
	float snowing = 0.0f;
	if (const auto* sky = globals::game::sky; sky && sky->mode.get() == RE::Sky::Mode::kFull) {
		const auto isSnow = [](const RE::TESWeather* a_weather) {
			return a_weather && a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow) ? 1.0f : 0.0f;
		};
		snowing = isSnow(sky->currentWeather) * sky->currentWeatherPct + isSnow(sky->lastWeather) * (1.0f - sky->currentWeatherPct);
	}
	physicalFrame.snowing = std::clamp(snowing, 0.0f, 1.0f);
	return physicalFrame;
}

void WetSkin::UpdatePhysical(PhysicalState& a_state, const PhysicalInput& a_in) const
{
	const auto& s = settings;
	const float dt = std::clamp(std::isfinite(a_in.dt) ? a_in.dt : 0.0f, 0.0f, 3600.0f);
	const float evapTime = PhysicalEvaporationTime();
	const float cold = std::clamp(a_in.cold, 0.0f, 1.0f);
	const float exposure = std::clamp(a_in.exposure, 0.0f, 1.0f);
	const float intensity = std::clamp(physicalFrame.rainIntensity, 0.0f, 1.0f);
	// Evaporation environment (generic only): humid while it rains, slower in the cold (dry, windy air is the 1).
	const float evaporation = (1.0f - 0.5f * intensity) * (1.0f - 0.3f * cold);
	const float exertion = std::clamp(a_in.exertion, 0.0f, 1.0f);
	const float buildup = std::clamp(s.SweatBuildup, 0.1f, 5.0f);
	const float sweatDrying = std::clamp(s.SweatDrying, 0.1f, 5.0f);
	const float maxRain = std::clamp(s.MaxRainWetness, 0.0f, 1.0f);
	const float rainTarget = maxRain * intensity * exposure;

	// Sweat: production follows the exertion through two lags (onset after about a minute, still sweating for a
	// while after stopping); the skin's sweat moves to production / evaporation (above the production when humid).
	const float kEvap = sweatDrying * evaporation / (0.6f * evapTime);
	const float kProd = buildup / (0.6f * evapTime);
	const auto production = [&](float a_heat) { return Smooth(0.3f, 0.9f, a_heat) * (1.0f - 0.6f * cold); };

	if (!a_state.valid) {
		a_state = {};
		a_state.valid = true;
		a_state.heat1 = a_state.heat2 = exertion;
		a_state.sweat = std::min(kProd * production(exertion) / std::max(kEvap, 1e-6f), 1.0f);
		a_state.rain = rainTarget;  // a character met in the rain is wet already
	} else if (dt > 0.0f) {
		const auto lag = [dt](float& a_value, float a_target, float a_up, float a_down) {
			const float tau = a_target > a_value ? a_up : a_down;
			a_value += (a_target - a_value) * (1.0f - std::exp(-dt / tau));
		};
		lag(a_state.heat1, exertion, 45.0f / buildup, 150.0f);
		lag(a_state.heat2, a_state.heat1, 45.0f / buildup, 150.0f);
		// Unclamped: humid air raises it above 1, so the sweat climbs as fast as anywhere and stays at the top (clamped below).
		const float equilibrium = kProd * production(a_state.heat2) / std::max(kEvap, 1e-6f);
		a_state.sweat = equilibrium + (a_state.sweat - equilibrium) * std::exp(-kEvap * dt);

		// Rain: toward intensity x exposure x Max Rain Wetness. Drying: what is above the skin's damp hold (or above
		// what the rain still brings) runs off within seconds, the damp rest evaporates.
		const float rainWetting = std::clamp(s.RainWetting, 0.1f, 5.0f);
		const float rainDrying = std::clamp(s.RainDrying, 0.1f, 5.0f);
		if (rainTarget > a_state.rain) {
			const float tau = 40.0f / (rainWetting * std::max(intensity, 0.15f));
			a_state.rain += (rainTarget - a_state.rain) * (1.0f - std::exp(-dt / tau));
		} else {
			const float damp = std::min(a_state.rain, 0.35f * maxRain);
			const float floor = std::max(rainTarget, damp);
			if (a_state.rain > floor)
				a_state.rain = floor + (a_state.rain - floor) * std::exp(-dt * rainDrying / 12.0f);
			const float rate = 0.35f * maxRain * rainDrying * evaporation / (0.6f * evapTime);
			a_state.rain = std::max(rainTarget, a_state.rain - rate * dt);
		}
	}

	// Swimming: both clocks start when the character leaves the water.
	const float swimDrying = std::clamp(s.SwimDrying, 0.1f, 5.0f);
	if (a_in.inWater) {
		a_state.swimRun = 0.0f;
		a_state.swimEvap = 0.0f;
		a_state.wasInWater = true;
	} else if (dt > 0.0f) {
		a_state.swimRun = std::min(a_state.swimRun + dt * swimDrying, 1e6f);
		a_state.swimEvap = std::min(a_state.swimEvap + dt * swimDrying * evaporation, 1e6f);
	}

	a_state.exposure = exposure;
	a_state.evaporation = evaporation;
	for (float* v : { &a_state.heat1, &a_state.heat2, &a_state.sweat, &a_state.rain }) {
		if (!std::isfinite(*v))
			*v = 0.0f;
		*v = std::clamp(*v, 0.0f, 1.0f);
	}
}

WetSkin::PhysicalLook WetSkin::GetPhysicalLook(const PhysicalState& a_state) const
{
	const auto& s = settings;
	PhysicalLook look;
	const float runoffTime = PhysicalRunoffTime();
	const float evapTime = PhysicalEvaporationTime();
	look.runoffTime = runoffTime;
	look.swimClock = std::min(a_state.swimRun, 1e6f);
	// The evaporation clock ran slower or faster than the runoff clock (environment): the shader only gets the runoff
	// clock, so the evaporation time is stretched to match (exact for every dry multiplier).
	look.evapTime = a_state.swimEvap > 1e-3f ? std::clamp(evapTime * a_state.swimRun / a_state.swimEvap, 1.0f, 1e6f) : evapTime;
	look.swim = a_state.wasInWater ? PhysicalSwimLevel(a_state.swimRun, runoffTime, look.evapTime, 1.0f) : 0.0f;
	look.sweat = a_state.sweat;
	look.rain = a_state.rain;

	const float manual = std::clamp(s.ExtraWetness, 0.0f, 1.0f);
	const auto manualLook = static_cast<ManualLook>(std::min(s.ManualLookType, static_cast<uint32_t>(ManualLook::Oil)));
	look.oil = manualLook == ManualLook::Oil ? manual : 0.0f;
	look.manualWater = manualLook == ManualLook::Oil ? 0.0f : manual;

	// Each source at the level the shader will see (x its weight), then the stronger effect wins; the droplet
	// shape parameters are blended by how much each source's beads count.
	std::array<SourceLook, 4> sources{};
	const bool strong = s.PhysicalStrongLowLook;  // (47h)
	sources[0] = SweatLook(a_state.sweat * std::clamp(s.SweatWeight, 0.0f, 1.0f), strong);
	sources[1] = RainLook(a_state.rain, strong);
	{
		const float phase = std::clamp(a_state.swimEvap / evapTime, 0.0f, 1.0f);
		const float runoff = std::exp(-a_state.swimRun / runoffTime);
		sources[2] = SwimLook(look.swim * std::clamp(s.SwimWeight, 0.0f, 1.0f), phase, runoff, strong);
	}
	switch (manualLook) {
	case ManualLook::Sweat:
		sources[3] = SweatLook(look.manualWater, strong);
		break;
	case ManualLook::Rain:
		sources[3] = RainLook(look.manualWater, strong);
		break;
	case ManualLook::Swim:
		sources[3] = SwimLook(look.manualWater, 1.0f - look.manualWater, Smooth(0.8f, 1.0f, look.manualWater), strong);
		break;
	default:
		break;  // Oil: no water
	}
	float levelSum = 0.0f, filmSum = 0.0f, beadSum = 0.0f;
	float density = 0.0f, size = 0.0f, sizeRandom = 0.0f, trails = 0.0f;
	for (const auto& src : sources) {
		look.peak = std::max(look.peak, src.level);
		look.beads = std::max(look.beads, src.beads);
		look.trails = std::max(look.trails, src.streak);
		levelSum += src.level;
		filmSum += src.film * src.level;
		const float w = src.beads * src.level;
		beadSum += w;
		density += src.density * w;
		size += src.size * w;
		sizeRandom += src.sizeRandom * w;
		trails += src.trails * w;
	}
	// (47g) Gear and hair: beads and trails only from rain, swimming and a manual look other than Sweat.
	look.manualSweat = manualLook == ManualLook::Sweat;
	for (std::size_t i = 1; i < sources.size(); ++i) {
		if (i == 3 && look.manualSweat)
			continue;
		look.gearBeads = std::max(look.gearBeads, sources[i].beads);
		look.gearTrails = std::max(look.gearTrails, sources[i].streak);
	}
	look.film = levelSum > 1e-5f ? filmSum / levelSum : 1.0f;
	if (beadSum > 1e-6f) {
		look.density = density / beadSum;
		look.size = size / beadSum;
		look.sizeRandom = sizeRandom / beadSum;
		look.beadTrails = trails / beadSum;
	}

	// Oil Repels Water: on oiled skin the water stays in round, distinct beads and forms no film (CPU parameters only).
	if (s.OilRepelsWater && look.oil > 0.0f) {
		const float k = look.oil;
		look.film *= 1.0f - 0.9f * k;
		look.beads = std::lerp(look.beads, std::max(look.beads, Smooth(0.02f, 0.12f, look.peak)), k);
		look.density *= 1.0f + 0.4f * k;
		look.size *= std::lerp(1.0f, 1.15f, k);
		look.sizeRandom *= 1.0f - 0.6f * k;
		look.beadTrails *= 1.0f - 0.8f * k;
		look.trails *= 1.0f - 0.7f * k;
		look.dome *= 1.0f + 0.6f * k;
	}
	return look;
}

void WetSkin::DrawSettings()
{
	auto& s = settings;
	if (!globals::features::skin.loaded || !globals::features::skin.settings.EnableSkin)
		ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Needs Advanced Skin enabled.");
	if (!globals::features::wetnessEffects.loaded)
		ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Hair and gear need Wetness Effects enabled (skin works without it).");

	// 1. (46a) User presets (the built-in Natural / Drenched looks were removed). They hold the model too.
	DrawPresets();

	// 2. Model. (47f) Saved values: 1 = Legacy, 0 = Simple (the 44a-47e "Advanced" model, unchanged), 2 = Physical.
	{
		const char* models[] = { "Legacy", "Simple", "Physical" };
		constexpr uint32_t kSaved[] = { 1u, 0u, 2u };
		int shown = IsOriginal() ? 0 : (IsPhysical() ? 2 : 1);
		if (ImGui::Combo("Wetness Model", &shown, models, IM_ARRAYSIZE(models)))
			s.Model = kSaved[std::clamp(shown, 0, 2)];
		Tooltip({ "Legacy (pre-42c): every character exactly as before the wetness work (41b): skin, hair and gear. Only safety limits are kept.",
			"Simple: skin, hair and gear get wet together and dry at their own pace (cloth slowest). Soft sheen that gets strong",
			"when soaked (never a mirror), droplets, rivulets, darkening per material. (Called Advanced before 47f; unchanged.)",
			"Physical: as Simple, but sweat, rain and swimming each build up and dry like real water, and each has its own look:",
			"sweat starts as small beads after a minute or two of exertion, rain wets only what the sky reaches and runs off",
			"under a roof, a swimmer's film breaks into beads while drying. Costs about the same as Simple on the GPU." });
	}

	// 3. (47c) Performance switches (not saved in presets).
	ImGui::SeparatorText("Performance");
	ImGui::Checkbox("Wetness: Skip Invisible Work", &s.SkipInvisibleWork);
	Tooltip({ "Same picture, less GPU work: skips noise layers too fine to see, rivulets thinner than half a pixel,",
		"rain droplets and rivulets on skin under a roof (when rain is the only water), dry hair's clump pattern,",
		"the see-through-clothes depth copy while nobody is in water. Off = as in 47b." });
	ImGui::Checkbox("Wetness: Faster Noise & Distance LOD", &s.FasterNoiseLOD);
	Tooltip({ "Simple and Physical. A much cheaper noise for the wet patches (skin and gear): same sliders, same look on average,",
		"the patches sit in other places. Droplets and rivulets fade out on characters far away (about 13 m and farther);",
		"their wet sheen and darkening stay. Off = as in 47b." });

	if (IsOriginal()) {
		// 5. Legacy's own look, 6. the manual slider (0-2, as before).
		ImGui::SeparatorText("Legacy (pre-42c)");
		ImGui::SliderFloat("Wetness Film Strength", &s.FilmStrength, 0.0f, 3.0f, "%.2f");
		Tooltip({ "Advanced Skin's water film, as before. Its energy is capped at 2 (no mirror-like reflections)." });
		ImGui::SliderFloat("Wetness Perlin Noise Scale", &s.WetParams.x, 0.0f, 1024.0f, "%1.f");
		ImGui::SliderFloat("Wetness Perlin Noise Lacunarity", &s.WetParams.y, 0.0f, 2.0f, "%.1f");
		ImGui::SliderFloat("Wetness Perlin Noise Persistence", &s.WetParams.z, 0.0f, 20.0f, "%.2f");
		ImGui::SliderFloat("Wetness Normal Scale", &s.WetParams.w, 0.0f, 20.0f, "%.1f");
		ImGui::TextDisabled("Sweat thresholds, Dynamic Wetness and the fade-out time are on the Advanced Skin page.");
		ImGui::SeparatorText("Manual");
		ImGui::SliderFloat("Extra Wetness (manual)", &s.ExtraWetness, 0.0f, 2.0f, "%.2f");
		Tooltip({ "Makes skin and hair this wet (0-2, as before)." });
		return;
	}

	// 4. (47f) Physical: how fast each source wets and dries.
	if (IsPhysical()) {
		ImGui::SeparatorText("Physical");
		ImGui::SliderFloat("Sweat Build-up", &s.SweatBuildup, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "How soon and how much a character sweats when exerted (low stamina, or Dynamic Wetness). 1: the first sweat",
			"shows after about a minute or two, then it slowly builds up to where it dries as fast as it comes." });
		ImGui::SliderFloat("Sweat Drying", &s.SweatDrying, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "How fast sweat dries. It keeps coming for a while after stopping, then fades slowly; slower in humid or rainy air", "(and it builds up higher there), drier in the cold." });
		ImGui::SliderFloat("Rain Wetting", &s.RainWetting, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "How fast rain wets a character under the open sky. Heavier rain wets faster and wetter." });
		ImGui::SliderFloat("Rain Drying", &s.RainDrying, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "Under a roof or when the rain stops: the water first runs off within seconds, the damp rest then dries slowly." });
		ImGui::SliderFloat("Swim Drying", &s.SwimDrying, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "Out of the water: most of the water runs off in the first seconds, the rest dries slowly.",
			"Hair and gear dry slower by their own Dry Time x (Hair and Gear sections)." });
		ImGui::SliderFloat("Max Rain Wetness", &s.MaxRainWetness, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How wet the heaviest rain makes an exposed character. Lighter rain settles lower: wetness follows",
			"rain intensity x how open the sky is above the character." });
		// (47h) Not saved in presets.
		ImGui::Checkbox("Physical: Stronger Low-Wetness Look", &s.PhysicalStrongLowLook);
		Tooltip({ "On (default): a slightly wet character (below about half wet) already shows it: more and larger visible beads from",
			"the first sweat or rain drops, trails and a thin sheen earlier. Fully wet looks the same. Off: as in 47g." });
		ImGui::Checkbox("Physical: Smaller Visible Bead Size", &s.PhysicalSmallBeads);
		Tooltip({ "On (default): beads and thin rivulets show from 1-2 screen pixels across (before: hidden below 2 px, full at 3.5 px),",
			"so small beads stay visible a little farther away. Off: as in 47g." });
	}

	// 5. Look: how wet each source makes a character at most (shared by Simple and Physical).
	ImGui::SeparatorText("Look (fully wet)");
	ImGui::SliderFloat("Swimming", &s.SwimWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet everything under the water line gets. 1 = soaked." });
	ImGui::SliderFloat("Stay Wet After Leaving Water (s)", &s.HoldTime, 0.0f, 120.0f, "%.0f s");
	ImGui::SliderFloat("Dry-off Time (s)", &s.DryTime, 5.0f, 600.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
	if (IsPhysical())
		Tooltip({ "Physical: the base times. The runoff after leaving the water takes about half of Stay Wet, a soaked skin dries",
			"in Stay Wet + Dry-off; sweat and rain dry on the same scale. The Physical multipliers above scale them." });
	else
		Tooltip({ "For skin. Hair and each kind of gear multiply both times by their own Dry Time x (cloth 2.5x: stays wet longest)." });
	if (!IsPhysical()) {
		ImGui::SliderFloat("Rain", &s.RainWeight, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How wet rain makes skin and hair (only where the sky reaches them: under a roof they stay dry).",
			"Gear follows the rain wetness of Wetness Effects." });
	}
	ImGui::SliderFloat("Sweat", &s.SweatWeight, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How wet sweat (low stamina, or Dynamic Wetness) makes skin. Hair and gear take their own share of it.",
		"Thresholds are on the Advanced Skin page." });

	// 6. Manual.
	ImGui::SeparatorText("Manual");
	{
		float manual = std::clamp(s.ExtraWetness, 0.0f, 1.0f);
		if (ImGui::SliderFloat("Extra Wetness (manual)", &manual, 0.0f, 1.0f, "%.2f"))
			s.ExtraWetness = manual;
		Tooltip({ "Makes every character this wet (skin, hair and gear), on top of swimming, rain and sweat. 1 = soaked.",
			"No building up or drying: it stays as set." });
	}
	if (IsPhysical()) {
		const char* looks[] = { "Sweat", "Rain", "Swim", "Oil" };
		int look = static_cast<int>(std::min(s.ManualLookType, 3u));
		if (ImGui::Combo("Manual Look", &look, looks, IM_ARRAYSIZE(looks)))
			s.ManualLookType = static_cast<uint32_t>(std::clamp(look, 0, 3));
		Tooltip({ "What the manual wetness looks like: Sweat (beads, then a thin film), Rain (sparse beads, trails, a film),",
			"Swim (a film breaking into beads), or Oil: an even oily film on the skin only (no beads, no trails), a little",
			"shinier than water with crisp highlights, the skin a little deeper in colour. Oil never dries." });
		ImGui::BeginDisabled(s.ManualLookType != static_cast<uint32_t>(ManualLook::Oil));
		ImGui::Checkbox("Oil Repels Water", &s.OilRepelsWater);
		Tooltip({ "On oiled skin, sweat, rain and swimming water gather in rounder, separate beads and form no film." });
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader("Skin")) {
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
		Tooltip({ "The Perlin noise the patches come from (shared with the Legacy model and gear). Lacunarity 2 / Persistence 0.5 gives finer patches." });

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
		ImGui::SliderFloat("Rivulet Contrast", &s.RivuletContrast2, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Higher = the trails stand out more (they run longer before breaking up). Never dry holes." });
		ImGui::SliderFloat("Between-Trail Dulling", &s.RivuletBetweenDulling, 0.0f, 1.0f, "%.2f");
		Tooltip({ "Higher = the wet film between the trails is a little duller, so the trails stand out more.",
			"(Before 44e this was part of Rivulet Contrast.)" });
		ImGui::TextDisabled("Contrast now works the intuitive way; your previous value was converted automatically.");
		ImGui::SliderFloat("Rivulets on the Face", &s.FaceRivulets, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How much rivulet the face gets compared with the body (1 = the same, as before 44e)." });
	}

	if (ImGui::CollapsingHeader("Hair")) {
		ImGui::Checkbox("Wet Hair", reinterpret_cast<bool*>(&s.HairEnabled));
		Tooltip({ "Off: hair never gets wet (Advanced model). Hair never gets a mirror-like water film either way:",
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

	if (ImGui::CollapsingHeader("Gear")) {
		ImGui::Checkbox("Wet Gear", reinterpret_cast<bool*>(&s.GearEnabled));
		Tooltip({ "Off: armour, clothes and weapons never get wet (Advanced model)." });
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

	// (46a) See-through clothes in water.
	ImGui::SeparatorText("See-through clothes in water");
	ImGui::Checkbox("Show see-through clothes below the water surface (experimental)", &s.TranslucentUnderWater);
	Tooltip({ "Sheer or see-through clothing (stockings, veils) of a character standing in water used to disappear below the",
		"surface and near it: the game draws it after the water, and the water hides it.",
		"On: it is drawn against the scene as it was before the water, and its part below the surface is tinted and faded",
		"like seen through water. Only for characters in water, seen from above the surface. Off (default): as the game does it.",
		"Needs Advanced Skin enabled." });
	ImGui::BeginDisabled(!s.TranslucentUnderWater);
	ImGui::SliderFloat("Under-Water Tint", &s.UnderWaterTint, 0.0f, 1.0f, "%.2f");
	Tooltip({ "How much the part below the surface is tinted and faded (0 = shown as above the water)." });
	{
		const auto& status = globals::features::skin.waterDepthStatus;
		ImGui::TextDisabled("Last frame: %u draws shown through the water, %u skipped (%s)", status.swapped, status.skipped, status.note ? status.note : "-");
	}
	ImGui::EndDisabled();

	// 8. (47f) Status (read-only, closed by default).
	DrawStatus();
}

void WetSkin::DrawStatus()
{
	if (!ImGui::TreeNode("Status"))
		return;
	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto& skinFeature = globals::features::skin;
	const Skin::ActorWetState* state = nullptr;
	if (player) {
		if (auto it = skinFeature.actorWetStates.find(player->GetFormID()); it != skinFeature.actorWetStates.end() && it->second.valid)
			state = &it->second;
	}
	if (!IsPhysical()) {
		ImGui::TextDisabled("The levels below are kept by the Physical model only.");
	} else if (!state) {
		ImGui::TextDisabled("The player has not been drawn yet.");
	} else {
		const auto& phys = state->phys;
		const auto& look = state->look;
		ImGui::Text("Player sweat: %.2f   (exertion lag %.2f)", phys.sweat, phys.heat2);
		ImGui::Text("Player rain: %.2f   (rain %.0f%%, open sky %.0f%%)", phys.rain, physicalFrame.rainIntensity * 100.0f, phys.exposure * 100.0f);
		ImGui::Text("Player swim: %.2f   (%s)", look.swim, state->wetChar.z > 0.5f ? "in the water" : (phys.wasInWater ? std::format("out {:.0f} s", phys.swimRun / std::clamp(settings.SwimDrying, 0.1f, 5.0f)).c_str() : "dry"));
		ImGui::Text("Oil: %.2f   Drying speed (air): %.2fx", look.oil, phys.evaporation);
		ImGui::TextDisabled("Look: beads %.2f, trails %.2f, film x%.2f, bead size x%.2f", look.beads, look.trails, look.film, look.size);
	}
	ImGui::TreePop();
}

void WetSkin::LoadSettings(json& o_json)
{
	settings = o_json;
	PhysicalFromJson(o_json, settings);  // (47f)
	// (47h) Not in the macro (its 64-argument limit) and not part of presets.
	if (o_json.is_object()) {
		if (auto it = o_json.find("PhysicalStrongLowLook"); it != o_json.end() && it->is_boolean())
			settings.PhysicalStrongLowLook = it->get<bool>();
		if (auto it = o_json.find("PhysicalSmallBeads"); it != o_json.end() && it->is_boolean())
			settings.PhysicalSmallBeads = it->get<bool>();
	}
	if (settings.Model > static_cast<uint32_t>(Model::Physical))
		settings.Model = static_cast<uint32_t>(Model::New);
	// (44e) Rivulet Contrast reversed. Old key StreakContrast c (0..1): trail threshold lerp(0.48, 0.68, c)
	// and film-between dulling 0.35 c. New: RivuletContrast2 = 1 - c gives the same threshold
	// (lerp(0.68, 0.48, 1 - c)), RivuletBetweenDulling = c the same dulling. Exact, and taken from the
	// saved value, once (a config that has RivuletContrast2 is never migrated again).
	if (o_json.is_object() && o_json.contains("StreakContrast") && !o_json.contains("RivuletContrast2")) {
		try {
			const float c = std::clamp(o_json["StreakContrast"].get<float>(), 0.0f, 1.0f);
			settings.RivuletContrast2 = 1.0f - c;
			settings.RivuletBetweenDulling = c;
			logger::info("[Character Wetness] Rivulet Contrast {:.3f} converted: Contrast {:.3f}, Between-Trail Dulling {:.3f}", c, settings.RivuletContrast2, settings.RivuletBetweenDulling);
		} catch (...) {
		}
	}
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
	PhysicalToJson(o_json, settings);  // (47f)
	o_json["PhysicalStrongLowLook"] = settings.PhysicalStrongLowLook;  // (47h)
	o_json["PhysicalSmallBeads"] = settings.PhysicalSmallBeads;
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

// ---------------------------------------------------------------------------------------------
// (46a) User presets
// ---------------------------------------------------------------------------------------------

namespace
{
	std::filesystem::path Utf8Path(const std::string& a_text)
	{
		return std::filesystem::path(std::u8string(a_text.begin(), a_text.end()));
	}

	std::string PathUtf8(const std::filesystem::path& a_path)
	{
		const auto text = a_path.u8string();
		return std::string(text.begin(), text.end());
	}

	std::filesystem::path PresetFile(const std::string& a_name)
	{
		return Utf8Path(WetSkin::kPresetDirectory) / Utf8Path(a_name + ".json");
	}
}

std::string WetSkin::SanitizePresetName(const std::string& a_name)
{
	std::string out;
	for (const char ch : a_name) {
		const auto c = static_cast<unsigned char>(ch);
		if (c < 32 || std::string_view("\\/:*?\"<>|").find(ch) != std::string_view::npos)
			continue;
		out.push_back(ch);
	}
	// No leading / trailing spaces or dots (Windows drops trailing ones), nothing like "..".
	while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
		out.pop_back();
	size_t start = 0;
	while (start < out.size() && (out[start] == ' ' || out[start] == '.'))
		++start;
	out = out.substr(start);
	if (out.size() > 64) {
		out.resize(64);
		// Do not cut a UTF-8 character in half.
		while (!out.empty() && (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80)
			out.pop_back();
		if (!out.empty() && (static_cast<unsigned char>(out.back()) & 0x80))
			out.pop_back();
	}
	return out;
}

std::vector<std::string> WetSkin::ListPresets() const
{
	std::vector<std::string> names;
	std::error_code ec;
	const auto dir = Utf8Path(kPresetDirectory);
	if (!std::filesystem::is_directory(dir, ec))
		return names;
	for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
		if (entry.is_regular_file(ec) && entry.path().extension() == ".json")
			names.push_back(PathUtf8(entry.path().stem()));
	}
	std::sort(names.begin(), names.end());
	return names;
}

bool WetSkin::SavePreset(const std::string& a_name)
{
	const std::string name = SanitizePresetName(a_name);
	if (name.empty())
		return false;
	std::error_code ec;
	std::filesystem::create_directories(Utf8Path(kPresetDirectory), ec);
	if (ec) {
		logger::warn("[Character Wetness] Could not create the preset folder {}: {}", kPresetDirectory, ec.message());
		return false;
	}
	json j;
	j["preset_name"] = name;
	j["settings"] = settings;
	PhysicalToJson(j["settings"], settings);  // (47f) the model is in the macro, its Physical options here
	// (47c) The performance switches are not part of a look.
	j["settings"].erase("SkipInvisibleWork");
	j["settings"].erase("FasterNoiseLOD");
	try {
		std::ofstream o{ PresetFile(name) };
		if (!o)
			return false;
		o << std::setw(4) << j;
		logger::info("[Character Wetness] Saved preset \"{}\"", name);
		return static_cast<bool>(o);
	} catch (const std::exception& e) {
		logger::warn("[Character Wetness] Could not save preset \"{}\": {}", name, e.what());
		return false;
	}
}

bool WetSkin::LoadPreset(const std::string& a_name)
{
	json j;
	try {
		std::ifstream i{ PresetFile(a_name) };
		if (!i)
			return false;
		i >> j;
	} catch (const std::exception& e) {
		logger::warn("[Character Wetness] Could not read preset \"{}\": {}", a_name, e.what());
		return false;
	}
	const json& values = (j.is_object() && j.contains("settings")) ? j["settings"] : j;
	if (!values.is_object())
		return false;
	// Laid over the current values: a preset saved by an older build leaves newer options as they are.
	try {
		json merged = settings;
		merged.update(values);
		merged["SkipInvisibleWork"] = settings.SkipInvisibleWork;  // (47c) not part of a look
		merged["FasterNoiseLOD"] = settings.FasterNoiseLOD;
		Settings fromPreset = merged.get<Settings>();
		// (47f) Not in the macro: kept unless the preset has them.
		fromPreset.SweatBuildup = settings.SweatBuildup;
		fromPreset.SweatDrying = settings.SweatDrying;
		fromPreset.RainWetting = settings.RainWetting;
		fromPreset.RainDrying = settings.RainDrying;
		fromPreset.SwimDrying = settings.SwimDrying;
		fromPreset.MaxRainWetness = settings.MaxRainWetness;
		fromPreset.ManualLookType = settings.ManualLookType;
		fromPreset.OilRepelsWater = settings.OilRepelsWater;
		fromPreset.PhysicalStrongLowLook = settings.PhysicalStrongLowLook;  // (47h) not part of a look preset
		fromPreset.PhysicalSmallBeads = settings.PhysicalSmallBeads;
		PhysicalFromJson(values, fromPreset);
		if (fromPreset.Model > static_cast<uint32_t>(Model::Physical))
			fromPreset.Model = static_cast<uint32_t>(Model::New);
		settings = fromPreset;
	} catch (const std::exception& e) {
		logger::warn("[Character Wetness] Preset \"{}\" is not valid: {}", a_name, e.what());
		return false;
	}
	logger::info("[Character Wetness] Loaded preset \"{}\"", a_name);
	return true;
}

bool WetSkin::RenamePreset(const std::string& a_from, const std::string& a_to)
{
	const std::string to = SanitizePresetName(a_to);
	if (to.empty() || to == a_from)
		return false;
	std::error_code ec;
	if (std::filesystem::exists(PresetFile(to), ec))
		return false;
	std::filesystem::rename(PresetFile(a_from), PresetFile(to), ec);
	if (ec) {
		logger::warn("[Character Wetness] Could not rename preset \"{}\": {}", a_from, ec.message());
		return false;
	}
	// Keep the name stored inside the file in step.
	try {
		json j;
		{
			std::ifstream i{ PresetFile(to) };
			i >> j;
		}
		if (j.is_object()) {
			j["preset_name"] = to;
			std::ofstream o{ PresetFile(to) };
			o << std::setw(4) << j;
		}
	} catch (...) {
	}
	return true;
}

bool WetSkin::DeletePreset(const std::string& a_name)
{
	std::error_code ec;
	const bool removed = std::filesystem::remove(PresetFile(a_name), ec);
	if (ec)
		logger::warn("[Character Wetness] Could not delete preset \"{}\": {}", a_name, ec.message());
	return removed && !ec;
}

void WetSkin::DrawPresets()
{
	if (!ImGui::TreeNodeEx("Presets", ImGuiTreeNodeFlags_DefaultOpen))
		return;
	if (!presetsListed) {
		presetNames = ListPresets();
		presetsListed = true;
	}
	if (presetIndex >= static_cast<int>(presetNames.size()))
		presetIndex = -1;
	const bool selected = presetIndex >= 0;
	const std::string current = selected ? presetNames[presetIndex] : std::string(presetNames.empty() ? "(no presets saved yet)" : "(choose a preset)");

	if (ImGui::BeginCombo("Preset", current.c_str())) {
		presetNames = ListPresets();
		for (int i = 0; i < static_cast<int>(presetNames.size()); ++i) {
			if (ImGui::Selectable(presetNames[i].c_str(), presetIndex == i)) {
				presetIndex = i;
				presetRenameTo = presetNames[i];
			}
		}
		ImGui::EndCombo();
	}
	Tooltip({ "Your own saved Character Wetness settings (every option on this page).",
		"Files are kept in Data\\SKSE\\Plugins\\CommunityShaders\\CharacterWetness." });

	ImGui::BeginDisabled(!selected);
	if (ImGui::Button("Load")) {
		const std::string name = presetNames[presetIndex];
		presetStatus = LoadPreset(name) ? std::format("Loaded \"{}\".", name) : std::format("Could not load \"{}\".", name);
	}
	Tooltip({ "Replace the settings on this page with the chosen preset." });
	ImGui::EndDisabled();

	// (47b) Everything that changes the preset files sits in a nested node, closed by default.
	if (ImGui::TreeNode("Manage Presets")) {
		DrawManagePresets(selected);
		ImGui::TreePop();
	}
	if (!presetStatus.empty())
		ImGui::TextDisabled("%s", presetStatus.c_str());
	ImGui::TreePop();
}

void WetSkin::DrawManagePresets(bool a_selected)
{
	const bool selected = a_selected && presetIndex >= 0 && presetIndex < static_cast<int>(presetNames.size());
	ImGui::BeginDisabled(!selected);
	if (ImGui::Button("Overwrite")) {
		const std::string name = presetNames[presetIndex];
		presetStatus = SavePreset(name) ? std::format("Saved the current settings into \"{}\".", name) : std::format("Could not save \"{}\".", name);
	}
	Tooltip({ "Save the current settings into the chosen preset (replaces what it held)." });
	ImGui::SameLine();
	if (ImGui::Button("Delete"))
		ImGui::OpenPopup("Delete Character Wetness Preset");
	Tooltip({ "Delete the chosen preset file (asks first)." });
	if (ImGui::BeginPopup("Delete Character Wetness Preset")) {
		ImGui::Text("Delete \"%s\"?", selected ? presetNames[presetIndex].c_str() : "");
		if (ImGui::Button("Yes, delete") && selected) {
			const std::string name = presetNames[presetIndex];
			presetStatus = DeletePreset(name) ? std::format("Deleted \"{}\".", name) : std::format("Could not delete \"{}\".", name);
			presetNames = ListPresets();
			presetIndex = -1;
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel"))
			ImGui::CloseCurrentPopup();
		ImGui::EndPopup();
	}
	ImGui::InputText("##RenameTo", &presetRenameTo);
	ImGui::SameLine();
	if (ImGui::Button("Rename") && selected) {
		const std::string from = presetNames[presetIndex];
		const std::string to = SanitizePresetName(presetRenameTo);
		if (RenamePreset(from, to)) {
			presetStatus = std::format("Renamed \"{}\" to \"{}\".", from, to);
			presetNames = ListPresets();
			const auto it = std::find(presetNames.begin(), presetNames.end(), to);
			presetIndex = it != presetNames.end() ? static_cast<int>(it - presetNames.begin()) : -1;
		} else {
			presetStatus = to.empty() ? std::string("Enter a new name first.") : std::format("Could not rename to \"{}\" (the name may be in use).", to);
		}
	}
	Tooltip({ "Give the chosen preset the name typed on the left." });
	ImGui::EndDisabled();

	ImGui::InputText("##NewPresetName", &presetNewName);
	ImGui::SameLine();
	if (ImGui::Button("Save as New Preset")) {
		const std::string name = SanitizePresetName(presetNewName);
		std::error_code ec;
		if (name.empty()) {
			presetStatus = "Enter a name first.";
		} else if (std::filesystem::exists(PresetFile(name), ec)) {
			presetStatus = std::format("\"{}\" already exists: choose it and use Overwrite.", name);
		} else if (SavePreset(name)) {
			presetStatus = std::format("Saved \"{}\".", name);
			presetNames = ListPresets();
			const auto it = std::find(presetNames.begin(), presetNames.end(), name);
			presetIndex = it != presetNames.end() ? static_cast<int>(it - presetNames.begin()) : -1;
			presetRenameTo = name;
			presetNewName.clear();
		} else {
			presetStatus = std::format("Could not save \"{}\".", name);
		}
	}
	Tooltip({ "Save every setting on this page as a new preset under the name typed on the left." });
}
