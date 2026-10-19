#include "WetSkin.h"

#include "Features/Skin.h"
#include "Features/WetnessEffects.h"
#include "State.h"
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
	// here, next to it.
	void PhysicalToJson(json& o_json, const WetSkin::Settings& a_s)
	{
		o_json["SweatBuildup"] = a_s.SweatBuildup;
		o_json["RainWetting"] = a_s.RainWetting;
		o_json["MaxRainWetness"] = a_s.MaxRainWetness;
		o_json["ManualLookType"] = a_s.ManualLookType;
		o_json["OilRepelsWater"] = a_s.OilRepelsWater;
		// (47i)
		o_json["Beading"] = a_s.Beading;
		o_json["PhysDryTime"] = a_s.PhysDryTime;
		o_json["PhysDropletSize"] = a_s.PhysDropletSize;
		o_json["StartSweat"] = a_s.StartSweat;
		o_json["FullSweat"] = a_s.FullSweat;
		o_json["UseDynamicWetness"] = a_s.UseDynamicWetness;
		// (47j) A/B switches
		o_json["PhysWaterLine"] = a_s.PhysWaterLine;
		o_json["PhysBeadsFadeIn"] = a_s.PhysBeadsFadeIn;
		o_json["PhysBeadsShrink"] = a_s.PhysBeadsShrink;
		// (47k) A/B switches ((47m) Water Line Follows the Body: permanent)
		o_json["PhysBeadCap"] = a_s.PhysBeadCap;
		o_json["PhysLessGrid"] = a_s.PhysLessGrid;
		o_json["PhysSweat2"] = a_s.PhysSweat2;  // (47l) A/B switch ((47m) Water Line per Limb: permanent)
		o_json["Physical47i"] = true;
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
		number("RainWetting", o_s.RainWetting, 0.1f, 5.0f);
		number("MaxRainWetness", o_s.MaxRainWetness, 0.0f, 1.0f);
		if (auto it = a_json.find("ManualLookType"); it != a_json.end() && it->is_number_integer())
			o_s.ManualLookType = std::min(it->get<uint32_t>(), static_cast<uint32_t>(WetSkin::ManualLook::Oil));
		if (auto it = a_json.find("OilRepelsWater"); it != a_json.end() && it->is_boolean())
			o_s.OilRepelsWater = it->get<bool>();
		// (47i)
		number("Beading", o_s.Beading, 0.0f, 1.0f);
		number("PhysDryTime", o_s.PhysDryTime, 10.0f, 1800.0f);
		number("PhysDropletSize", o_s.PhysDropletSize, 0.5f, 3.0f);
		number("StartSweat", o_s.StartSweat, 0.0f, 1.0f);
		number("FullSweat", o_s.FullSweat, 0.0f, 1.0f);
		if (auto it = a_json.find("UseDynamicWetness"); it != a_json.end() && it->is_boolean())
			o_s.UseDynamicWetness = it->get<bool>();
		// (47j)
		for (auto [key, value] : { std::pair{ "PhysWaterLine", &o_s.PhysWaterLine }, std::pair{ "PhysBeadsFadeIn", &o_s.PhysBeadsFadeIn }, std::pair{ "PhysBeadsShrink", &o_s.PhysBeadsShrink },
				 std::pair{ "PhysBeadCap", &o_s.PhysBeadCap }, std::pair{ "PhysLessGrid", &o_s.PhysLessGrid }, std::pair{ "PhysSweat2", &o_s.PhysSweat2 } }) {
			if (auto it = a_json.find(key); it != a_json.end() && it->is_boolean())
				*value = it->get<bool>();
		}
	}

	/// (47i) A settings JSON from before 47i: the Physical options take the user's old values, once. Beading from
	/// Droplet Amount x Density and Droplets from Rain, Dry Time from Hold + Dry-off Time / Swim Drying, the coalescence scale
	/// from Droplet Size (its old meaning was the drawn size itself); the sweat thresholds from Advanced Skin (a_skin, may be
	/// empty: the defaults stay).
	void MigratePhysical47i(const json& a_json, const json& a_skin, WetSkin::Settings& o_s)
	{
		if (!a_json.is_object() || a_json.contains("Physical47i"))
			return;
		const auto value = [](const json& a_j, const char* a_key, float a_default) {
			if (a_j.is_object()) {
				if (auto it = a_j.find(a_key); it != a_j.end() && it->is_number()) {
					const float v = it->get<float>();
					if (std::isfinite(v))
						return v;
				}
			}
			return a_default;
		};
		const float amount = std::clamp(value(a_json, "DropletAmount", 1.0f), 0.0f, 1.0f);
		const float density = std::clamp(value(a_json, "DropletDensity", 0.45f), 0.0f, 1.0f);
		const float rainDroplets = std::clamp(value(a_json, "RainDroplets", 0.4f), 0.0f, 1.0f);
		o_s.Beading = std::clamp(0.75f * std::sqrt(amount * density) * (0.8f + 0.5f * rainDroplets), 0.05f, 1.0f);
		const float hold = std::clamp(value(a_json, "HoldTime", 20.0f), 0.0f, 600.0f);
		const float dry = std::clamp(value(a_json, "DryTime", 150.0f), 1.0f, 1200.0f);
		const float swimDrying = std::clamp(value(a_json, "SwimDrying", 1.0f), 0.1f, 5.0f);
		o_s.PhysDryTime = std::clamp((hold + dry) / swimDrying, 10.0f, 1800.0f);
		o_s.PhysDropletSize = std::clamp(std::cbrt(std::clamp(value(a_json, "DropletSize", 1.0f), 0.25f, 4.0f)), 0.5f, 3.0f);
		if (a_skin.is_object()) {
			o_s.StartSweat = std::clamp(value(a_skin, "StartSweat", o_s.StartSweat), 0.0f, 1.0f);
			o_s.FullSweat = std::clamp(value(a_skin, "FullSweat", o_s.FullSweat), 0.0f, 1.0f);
			if (auto it = a_skin.find("UseDynamicWetness"); it != a_skin.end() && it->is_boolean())
				o_s.UseDynamicWetness = it->get<bool>();
		}
		logger::info("[Character Wetness] 47i Physical settings taken from the old ones: Beading {:.2f}, Dry Time {:.0f} s, Droplet Size {:.2f}, sweat {:.2f} / {:.2f}{}",
			o_s.Beading, o_s.PhysDryTime, o_s.PhysDropletSize, o_s.StartSweat, o_s.FullSweat, o_s.UseDynamicWetness ? ", Dynamic Wetness" : "");
	}

	float Smooth(float a_edge0, float a_edge1, float a_x)
	{
		const float t = std::clamp((a_x - a_edge0) / (a_edge1 - a_edge0), 0.0f, 1.0f);
		return t * t * (3.0f - 2.0f * t);
	}

	/// (47i) One source's beads at its bead water (um): base radius r (cm, at most the runoff size), coverage, the visible
	/// share (sub-visible beads are sheen only) and how far past the runoff size the water would make them (trails).
	struct BeadSet
	{
		float water = 0.0f;
		float r = 0.0f;
		float coverage = 0.0f;
		float visible = 0.0f;
		float runoff = 0.0f;
		float kept = 1.0f;       // (47k) Bead Coalescence Cap: share of the water still held in the beads (the rest runs off / is film)
		float n0 = 0.0f;         // (47k) nucleation density (per cm2) before coalescence
		bool coalesced = false;  // (47k)
	};

	using Phys = WetSkin::Phys;
	constexpr float kPi = 3.14159265f;

	/// (47k) How the beads of every source are formed this frame. The default is 47j exactly.
	struct BeadLaw
	{
		bool cap = false;                         // Bead Coalescence Cap
		float cCap = 1.0f;                        // its coverage cap: kCapScale x Beading^1.5
		float beading = 0.5f;                     // Beading (after Oil Repels Water)
		float maxCoverage = Phys::kMaxCoverage;   // the cell grid at presence 1 (Less Grid-like Bead Placement: kLessGridCoverage)
	};

	/// (47k) Merges the beads once they cover a_merge (at the same water: fewer, bigger beads).
	void Coalesce(BeadSet& a_b, float a_merge)
	{
		if (a_b.coverage > a_merge) {
			const float v = a_b.water * 1e-4f;
			a_b.coverage = a_merge;
			a_b.r = v * kPi / (std::max(a_merge, 1e-6f) * Phys::kBeadKappa);
			a_b.coalesced = true;
		}
	}

	BeadSet FinishBeads(BeadSet a_b, float a_runoffR, const BeadLaw& a_law)
	{
		if (a_law.cap) {
			// (47k) Beads past the runoff size run off; what stays is in beads of the runoff size at the coverage they had (an
			// uncoalesced set: its nucleation count at the runoff size). The excess water drives the trails, nothing is repacked.
			if (a_b.r > a_runoffR) {
				if (!a_b.coalesced)
					a_b.coverage = a_b.n0 * kPi * a_runoffR * a_runoffR;
				a_b.r = a_runoffR;
				const float v = a_b.water * 1e-4f;
				const float held = a_b.coverage * Phys::kBeadKappa * a_runoffR / kPi;
				a_b.kept = v > 1e-12f ? std::clamp(held / v, 0.0f, 1.0f) : 1.0f;
				a_b.runoff = held > 1e-12f ? std::clamp((v - held) / held, 0.0f, 1.0f) : 1.0f;
			}
		} else if (a_b.r > a_runoffR) {
			a_b.runoff = std::clamp((a_b.r - a_runoffR) / a_runoffR, 0.0f, 1.0f);
			// Larger beads run off: the water left stays in beads of the runoff size (more of them).
			const float v = a_b.water * 1e-4f;
			a_b.r = a_runoffR;
			a_b.coverage = v * kPi / (Phys::kBeadKappa * a_b.r);
		}
		a_b.coverage = std::clamp(a_b.coverage, 0.0f, a_law.maxCoverage);
		a_b.visible = Smooth(Phys::kVisibleR0, Phys::kVisibleR1, a_b.r);
		return a_b;
	}

	/// Sweat: pores fill (kSweatPores per cm2), then the beads merge once they cover kSweatCoalesce / S ((47k) cap: or c_cap).
	BeadSet SweatBeads(float a_water, float a_size, const BeadLaw& a_law)
	{
		BeadSet b;
		b.water = std::max(a_water, 0.0f);
		if (b.water <= 1e-4f)
			return b;
		const float v = b.water * 1e-4f;
		b.n0 = Phys::kSweatPores;
		b.r = std::cbrt(v / (Phys::kSweatPores * Phys::kBeadKappa));
		b.coverage = Phys::kSweatPores * kPi * b.r * b.r;
		const float merge = Phys::kSweatCoalesce / a_size;
		Coalesce(b, a_law.cap ? std::min(merge, a_law.cCap) : merge);
		return FinishBeads(b, Phys::kRunoffRadius * a_size, a_law);
	}

	/// Rain: each impact one bead of kRainRadius; they merge once they cover kRainCoalesce / S ((47k) cap: or c_cap).
	BeadSet RainBeads(float a_water, float a_size, const BeadLaw& a_law)
	{
		BeadSet b;
		b.water = std::max(a_water, 0.0f);
		if (b.water <= 1e-4f)
			return b;
		const float v = b.water * 1e-4f;
		b.r = Phys::kRainRadius;
		b.coverage = v * kPi / (Phys::kBeadKappa * b.r);
		b.n0 = b.coverage / (kPi * b.r * b.r);
		const float merge = Phys::kRainCoalesce / a_size;
		Coalesce(b, a_law.cap ? std::min(merge, a_law.cCap) : merge);
		return FinishBeads(b, Phys::kRunoffRadius * a_size, a_law);
	}

	/// Swim: the film breaks into kSwimBeads / S^2 beads per cm2, which then shrink in place. (47k) Cap: n0 = 1.5 / S^2 x
	/// (0.25 + 3 B^2) (the same 1.5 at Beading 0.5), and they merge at c_cap too.
	BeadSet SwimBeads(float a_water, float a_size, const BeadLaw& a_law)
	{
		BeadSet b;
		b.water = std::max(a_water, 0.0f);
		if (b.water <= 1e-4f)
			return b;
		const float v = b.water * 1e-4f;
		const float n = Phys::kSwimBeads / (a_size * a_size) * (a_law.cap ? 0.25f + 3.0f * a_law.beading * a_law.beading : 1.0f);
		b.n0 = n;
		b.r = std::cbrt(v / (n * Phys::kBeadKappa));
		b.coverage = n * kPi * b.r * b.r;
		if (a_law.cap)
			Coalesce(b, a_law.cCap);
		return FinishBeads(b, Phys::kRunoffRadius * a_size, a_law);
	}

	/// The swim water above which no beads form (a closed film), as Skin.hlsli WetPhysSwimBeadShare.
	float SwimBeadGate(float a_water)
	{
		return 1.0f - Smooth(120.0f, 160.0f, a_water);
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

	// (46a) The built-in look presets (44k Natural / Drenched) were removed; (47m) so were the user presets.
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
	g.p[0] = float4(isNew ? (IsPhysical() ? 2.0f : 1.0f) : 0.0f, std::clamp(s.ExtraWetness, 0.0f, 1.0f), std::clamp(s.SwimWeight, 0.0f, 1.0f), std::clamp(s.RainWeight, 0.0f, 1.0f));
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

float WetSkin::PhysBandWater(float a_p, float a_x, bool a_law)
{
	const float held = a_law ? PhysDryCurve(a_p) : std::clamp(1.0f - a_p, 0.0f, 1.0f);
	return Phys::kHold * held + (Phys::kSoak - Phys::kHold) * std::clamp(a_x, 0.0f, 1.0f);
}

float WetSkin::PhysDryCurve(float a_p)
{
	// (47l) The held water of a swim zone drying alone by the loss law (PhysLawRate), sampled at p = u^2 (denser where it is steep).
	if (!(a_p > 0.0f))
		return 1.0f;
	if (a_p >= 1.0f)
		return 0.0f;
	const float u = std::sqrt(a_p) * static_cast<float>(Phys::kDryCurveN);
	const uint32_t i = std::min(static_cast<uint32_t>(u), Phys::kDryCurveN - 1);
	const float f = u - static_cast<float>(i);
	return Phys::kDryCurve[i] + (Phys::kDryCurve[i + 1] - Phys::kDryCurve[i]) * f;
}

float WetSkin::PhysLawRate(float a_v)
{
	const float u = std::max(a_v, 0.0f) + Phys::kLawVe;
	return Phys::kLawLambda * (std::log1p(u / Phys::kLawV0) + Phys::kLawB * u * u);
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
	float fogHumidity = 0.0f;
	if (const auto* sky = globals::game::sky; sky && sky->mode.get() == RE::Sky::Mode::kFull) {
		const auto isSnow = [](const RE::TESWeather* a_weather) {
			return a_weather && a_weather->data.flags.any(RE::TESWeather::WeatherDataFlag::kSnow) ? 1.0f : 0.0f;
		};
		// (47i) Fog humidity, generic: the weather's own fog distance (day and night alike), short in fog and mist.
		const auto fogHumid = [](const RE::TESWeather* a_weather) {
			if (!a_weather)
				return 0.0f;
			const float fogFar = std::min(a_weather->fogData.dayFar, a_weather->fogData.nightFar);
			return std::isfinite(fogFar) && fogFar > 0.0f ? 1.0f - Smooth(8000.0f, 30000.0f, fogFar) : 0.0f;
		};
		const float pct = std::clamp(sky->currentWeatherPct, 0.0f, 1.0f);
		snowing = isSnow(sky->currentWeather) * pct + isSnow(sky->lastWeather) * (1.0f - pct);
		fogHumidity = fogHumid(sky->currentWeather) * pct + fogHumid(sky->lastWeather) * (1.0f - pct);
	}
	physicalFrame.snowing = std::clamp(snowing, 0.0f, 1.0f);
	physicalFrame.fogHumidity = std::clamp(fogHumidity, 0.0f, 1.0f);
	return physicalFrame;
}

void WetSkin::UpdatePhysical(PhysicalState& a_state, const PhysicalInput& a_in) const
{
	const auto& s = settings;
	const float dtAll = std::clamp(std::isfinite(a_in.dt) ? a_in.dt : 0.0f, 0.0f, 3600.0f);
	const float cold = a_in.outdoors ? std::clamp(a_in.cold, 0.0f, 1.0f) : 0.0f;
	const float exposure = a_in.outdoors ? std::clamp(a_in.exposure, 0.0f, 1.0f) : 0.0f;
	const float intensity = std::clamp(physicalFrame.rainIntensity, 0.0f, 1.0f);
	// Evaporation environment (generic only): humid air (rain, even light rain; snow weather; fog) and cold dry slower; interiors neutral.
	const float rainHumid = (0.6f + 0.4f * intensity) * Smooth(0.0f, 0.05f, intensity);
	const float humid = a_in.outdoors ? std::max({ rainHumid, 0.7f * physicalFrame.snowing, physicalFrame.fogHumidity }) : 0.0f;
	const float evaporation = (1.0f - 0.7f * humid) * (1.0f - 0.4f * cold);
	const float exertion = std::clamp(std::isfinite(a_in.exertion) ? a_in.exertion : 0.0f, 0.0f, 1.0f);
	const float buildup = std::clamp(s.SweatBuildup, 0.1f, 5.0f);
	const float sweatTarget = Phys::kSweatMax * std::clamp(s.SweatWeight, 0.0f, 1.0f) * (1.0f - 0.6f * cold);
	const float rainCap = std::clamp(s.MaxRainWetness, 0.0f, 1.0f) * Phys::kHold;
	const float rainRate = Phys::kRainRate * intensity * exposure * std::clamp(s.RainWetting, 0.1f, 5.0f);
	const float flux = Phys::kHold / std::clamp(s.PhysDryTime, 10.0f, 1800.0f) * evaporation;  // um/s
	// (47l) Sweat and Drying v2: sweat made in um/s by the body heat (not toward a target), one smooth loss law for all the water
	// (PhysLawRate), sweat over kDripLevel x Sweat drips into the trails. Off: 47k.
	const bool law = s.PhysSweat2;
	const float dryTime = std::clamp(s.PhysDryTime, 10.0f, 1800.0f);
	const float lawScale = evaporation / dryTime;                         // E(V) = lawScale x PhysLawRate(V), um/s
	const float zoneRate = evaporation / (dryTime * Phys::kLawTauEnd);    // progress per s of a swim zone drying alone
	const float sweatRate = Phys::kSweatRate2 * std::clamp(s.SweatWeight, 0.0f, 1.0f) * buildup * (1.0f - 0.6f * cold);  // um/s at full heat
	const float dripLevel = Phys::kDripLevel * std::clamp(s.SweatWeight, 0.0f, 1.0f);
	const float depth = std::max(std::isfinite(a_in.depth) ? a_in.depth : 0.0f, 0.0f);
	const float bandHeight = std::max(std::isfinite(a_in.height) ? a_in.height : 128.0f, 32.0f) / static_cast<float>(Phys::kBands);
	const auto submerged = [&](uint32_t a_band) { return depth > (static_cast<float>(a_band) + 0.5f) * bandHeight; };
	// (47j) Water Line Like Legacy: one water line (the deepest since last fully dry) instead of the height bands.
	const bool lineMode = s.PhysWaterLine;
	const float actorHeight = bandHeight * static_cast<float>(Phys::kBands);
	const float lineDry = std::min(MaxDryMultiplier(), Phys::kMaxProgress);  // every class (hair, slowest gear) dry
	// (47l) Water Line per Limb: a line per limb in body units (a body-mapped character: Skin::UpdateBodyLimbs); else the one line.
	const bool limbMode = lineMode && a_in.limbs;  // (47m) per Limb is permanent
	std::array<float, Phys::kLimbs> limbDepth{};
	if (limbMode) {
		for (uint32_t i = 0; i < Phys::kLimbs; ++i)
			limbDepth[i] = std::max(std::isfinite(a_in.limbDepth[i]) ? a_in.limbDepth[i] : 0.0f, 0.0f);
	}
	const auto resetSwim = [&]() {
		for (uint32_t b = 0; b < Phys::kBands; ++b) {
			a_state.bandP[b] = submerged(b) ? 0.0f : Phys::kMaxProgress;
			a_state.bandX[b] = submerged(b) ? 1.0f : 0.0f;
		}
		a_state.lineHeight = depth;
		a_state.lineP = depth > 0.0f ? 0.0f : Phys::kMaxProgress;
		a_state.lineX = depth > 0.0f ? 1.0f : 0.0f;
		a_state.midHeight = depth;
		a_state.midP = a_state.lineP;
		a_state.midX = a_state.lineX;
		a_state.dipMin = depth;
		a_state.lineMode = lineMode;
		a_state.dryLaw = law;
		a_state.limbMode = limbMode;
		for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
			auto& l = a_state.limbs[i];
			const float d = limbDepth[i];
			l = {};
			l.lineHeight = l.midHeight = l.dipMin = l.depth = l.lastDepth = d;
			l.lineP = l.midP = d > 0.0f ? 0.0f : Phys::kMaxProgress;
			l.lineX = l.midX = d > 0.0f ? 1.0f : 0.0f;
		}
	};

	if (a_state.valid && (a_state.lineMode != lineMode || a_state.dryLaw != law || a_state.limbMode != limbMode))
		resetSwim();  // (47j) switched between the line and the bands ((47l) or the drying law): the swim water starts afresh (sweat and rain kept)

	if (!a_state.valid) {
		// First seen: as if it had been like this for a while (a character met in the rain is wet already; nobody is
		// wet from a swim they were not seen taking).
		a_state = {};
		a_state.valid = true;
		a_state.heat = exertion;
		a_state.sweat = sweatTarget * exertion * 0.8f;
		if (law) {
			// (47l) Where making and losing sweat balance at this exertion (bisection; at most the drip level).
			const float make = sweatRate * exertion;
			float lo = 0.0f, hi = std::max(dripLevel, 0.0f);
			if (make <= 0.0f || hi <= 0.0f) {
				hi = 0.0f;
			} else {
				for (int i = 0; i < 24; ++i) {
					const float mid = 0.5f * (lo + hi);
					(lawScale * PhysLawRate(mid) > make ? hi : lo) = mid;
				}
			}
			a_state.sweat = 0.5f * (lo + hi);
		}
		a_state.rain = rainRate > 0.0f ? std::min(rainCap, rainRate * 60.0f) : 0.0f;
		resetSwim();
	} else if (dtAll > 0.0f) {
		// (47l) Per limb: a line rises only as far as this and last frame agree (a one-frame glitch of the bones or the water never
		// raises it); dropping is immediate.
		if (limbMode) {
			for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
				auto& l = a_state.limbs[i];
				const float d = limbDepth[i];
				limbDepth[i] = std::min(d, std::max(l.lineHeight, l.lastDepth));
				l.lastDepth = d;
			}
		}
		// Sub-steps of at most 2 s (the shared evaporation couples the sources), at most 64 (a character back after a long
		// absence just catches up coarser).
		// (47l) v2: at most 0.5 s (the loss law is steep for thick water; the step itself stays stable at any length).
		const uint32_t steps = std::clamp(static_cast<uint32_t>(std::ceil(dtAll / (law ? 0.5f : 2.0f))), 1u, 64u);
		const float dt = dtAll / static_cast<float>(steps);
		for (uint32_t step = 0; step < steps; ++step) {
			// Sweat: body heat follows the exertion; production fills the skin toward the target (one way: a lower target
			// leaves the water to evaporate).
			if (law) {
				// (47l) Body heat follows the exertion (8 s up, 60 s down: sweat goes on for a while after stopping); sweat is made at
				// kSweatRate2 um/s x Sweat x Build-up x heat (x 0.4 in the cold) and lost by the shared law below; over the drip level
				// it drips off (softplus: no kink), feeding the sweat trails.
				const float tau = exertion > a_state.heat ? Phys::kHeatUp2 : Phys::kHeatDown2;
				a_state.heat += (exertion - a_state.heat) * (1.0f - std::exp(-dt / tau));
				a_state.sweat += sweatRate * a_state.heat * dt;
				const float over = (a_state.sweat - dripLevel) / Phys::kDripSoft;
				const float drip = Phys::kDripSoft * (over > 20.0f ? over : std::log1p(std::exp(over))) / Phys::kRunoffTau;  // um/s
				a_state.sweat = std::max(a_state.sweat - drip * dt, std::min(a_state.sweat, dripLevel));
				const float dripTarget = Smooth(0.2f, 1.0f, drip);
				a_state.sweatDrip += (dripTarget - a_state.sweatDrip) * (1.0f - std::exp(-dt / (dripTarget > a_state.sweatDrip ? 6.0f : 15.0f)));
			} else {
				const float tau = exertion > a_state.heat ? Phys::kHeatUp : Phys::kHeatDown;
				a_state.heat += (exertion - a_state.heat) * (1.0f - std::exp(-dt / tau));
				const float target = sweatTarget * a_state.heat;
				if (a_state.sweat < target)
					a_state.sweat = target + (a_state.sweat - target) * std::exp(-dt * buildup / Phys::kSweatTau);
			}

			// Rain: what the sky reaches, up to the cap; while it rains the impacts shed water (kRainDrain), so lighter rain settles
			// lower (equilibrium rate x kRainDrain); the water over the cap runs off. Both make trails.
			if (a_state.rain >= rainCap) {
				a_state.rain = rainCap + (a_state.rain - rainCap) * std::exp(-dt / Phys::kRunoffTau);  // over the cap (lowered): runs off
			} else if (rainRate > 0.0f) {
				const float settle = rainRate * Phys::kRainDrain;
				a_state.rain = std::min(settle + (a_state.rain - settle) * std::exp(-dt / Phys::kRainDrain), rainCap);
			}
			const bool runningOff = rainRate > 0.0f && a_state.rain >= rainCap - 0.5f;
			const float drainTrail = rainRate > 0.0f ? Smooth(0.6f, 1.6f, a_state.rain / Phys::kRainDrain) : 0.0f;  // um/s shed
			const float runoffTarget = std::max(runningOff ? std::clamp(rainRate / 1.0f, 0.0f, 1.0f) : 0.0f, drainTrail);
			a_state.rainRunoff += (runoffTarget - a_state.rainRunoff) * (1.0f - std::exp(-dt / (runoffTarget > a_state.rainRunoff ? 6.0f : 15.0f)));

			// Swim: bands under the surface are soaked; above it the water over the hold capacity runs off.
			// (47j) Water line: under the current surface soaked; above it, up to the water line (the deepest since dry), at most two
			// zones that each dry evenly (WetSkin.h PhysicalState); as deep as ever: the line rises and everything below is soaked.
			float swimMean = 0.0f;
			float swimHeld = 0.0f;  // (47l) v2: the held part only (the water over it runs off on its own)
			const auto heldOf = [&](float a_p) { return Phys::kHold * (law ? PhysDryCurve(a_p) : std::clamp(1.0f - a_p, 0.0f, 1.0f)); };
			if (limbMode) {
				// (47l) Water Line per Limb: each limb's line (the 47j two zones) by how far up that limb is under water (body units, from
				// its bones); the mean water for the shared loss from each limb's range of body heights, weighted by its share of the skin.
				constexpr float kLimbShare[Phys::kLimbs] = { 0.18f, 0.18f, 0.46f, 0.09f, 0.09f };
				for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
					auto& st = a_state.limbs[i];
					const float d = limbDepth[i];
					st.dipMin = std::min(st.dipMin, d);
					if (d > 0.0f && d >= st.lineHeight) {
						st.lineHeight = st.midHeight = st.dipMin = d;
						st.lineP = st.midP = 0.0f;
						st.lineX = st.midX = 1.0f;
					} else if (d > st.midHeight) {
						st.midHeight = st.dipMin = d;
						st.midP = 0.0f;
						st.midX = 1.0f;
					} else if (d > st.dipMin + Phys::kLineSplit) {
						const float hA = std::max(st.lineHeight - st.midHeight, 0.0f), hB = std::max(st.midHeight - d, 0.0f);
						if (hA + hB > 0.0f) {
							st.lineP = (st.lineP * hA + st.midP * hB) / (hA + hB);
							st.lineX = (st.lineX * hA + st.midX * hB) / (hA + hB);
						}
						st.midHeight = st.dipMin = d;
						st.midP = 0.0f;
						st.midX = 1.0f;
					} else {
						st.midX *= std::exp(-dt / Phys::kRunoffTau);
					}
					st.lineX *= st.lineHeight > st.midHeight ? std::exp(-dt / Phys::kRunoffTau) : 1.0f;
					const float lo = std::isfinite(a_in.limbLo[i]) ? a_in.limbLo[i] : 0.0f;
					const float hi = std::max(std::isfinite(a_in.limbHi[i]) ? a_in.limbHi[i] : actorHeight, lo + 1.0f);
					const float cd = std::clamp(d, lo, hi), cm = std::clamp(st.midHeight, lo, hi), cl = std::clamp(st.lineHeight, lo, hi);
					const float under = cd - lo, zoneB = std::max(cm - cd, 0.0f), zoneA = std::max(cl - std::max(cm, cd), 0.0f);
					swimMean += kLimbShare[i] * (under * Phys::kSoak + zoneB * PhysBandWater(st.midP, st.midX, law) + zoneA * PhysBandWater(st.lineP, st.lineX, law)) / (hi - lo);
					swimHeld += kLimbShare[i] * (under * Phys::kHold + zoneB * heldOf(st.midP) + zoneA * heldOf(st.lineP)) / (hi - lo);
				}
			} else if (lineMode) {
				auto& st = a_state;
				st.dipMin = std::min(st.dipMin, depth);
				if (depth > 0.0f && depth >= st.lineHeight) {
					// As deep as ever since dry: everything below is soaked again, the line rises.
					st.lineHeight = st.midHeight = st.dipMin = depth;
					st.lineP = st.midP = 0.0f;
					st.lineX = st.midX = 1.0f;
				} else if (depth > st.midHeight) {
					// Into the upper zone: the lower zone is all under water (soaked); the upper zone keeps its state above it.
					st.midHeight = st.dipMin = depth;
					st.midP = 0.0f;
					st.midX = 1.0f;
				} else if (depth > st.dipMin + Phys::kLineSplit) {
					// Back up inside the lower zone: its part still above the surface joins the upper zone (height-weighted, so the
					// water is kept), and the part that was under water again becomes the fresh lower zone.
					const float hA = std::max(st.lineHeight - st.midHeight, 0.0f), hB = std::max(st.midHeight - depth, 0.0f);
					if (hA + hB > 0.0f) {
						st.lineP = (st.lineP * hA + st.midP * hB) / (hA + hB);
						st.lineX = (st.lineX * hA + st.midX * hB) / (hA + hB);
					}
					st.midHeight = st.dipMin = depth;
					st.midP = 0.0f;
					st.midX = 1.0f;
				} else {
					st.midX *= std::exp(-dt / Phys::kRunoffTau);
				}
				st.lineX *= st.lineHeight > st.midHeight ? std::exp(-dt / Phys::kRunoffTau) : 1.0f;
				const float under = std::min(depth, actorHeight);
				const float zoneB = std::max(std::min(st.midHeight, actorHeight) - under, 0.0f);
				const float zoneA = std::max(std::min(st.lineHeight, actorHeight) - std::max(std::min(st.midHeight, actorHeight), under), 0.0f);
				swimMean = (under * Phys::kSoak + zoneB * PhysBandWater(st.midP, st.midX, law) + zoneA * PhysBandWater(st.lineP, st.lineX, law)) / actorHeight;
				swimHeld = (under * Phys::kHold + zoneB * heldOf(st.midP) + zoneA * heldOf(st.lineP)) / actorHeight;
			} else {
				float swimSum = 0.0f;
				for (uint32_t b = 0; b < Phys::kBands; ++b) {
					if (submerged(b)) {
						a_state.bandP[b] = 0.0f;
						a_state.bandX[b] = 1.0f;
					} else {
						a_state.bandX[b] *= std::exp(-dt / Phys::kRunoffTau);
					}
					swimSum += PhysBandWater(a_state.bandP[b], a_state.bandX[b], law);
					swimHeld += heldOf(a_state.bandP[b]);
				}
				swimMean = swimSum / static_cast<float>(Phys::kBands);
				swimHeld /= static_cast<float>(Phys::kBands);
			}

			// One evaporation flux, shared by the water there in proportion (more water: longer).
			// (47l) v2: the loss law at the water there (sweat, rain and the mean held swim water), shared in proportion; the step is
			// exponential in U = V + Ve (never below 0, stable at any length).
			const float loss = flux * dt;
			const float total = a_state.sweat + a_state.rain + swimMean;
			if (law) {
				const float vc = a_state.sweat + a_state.rain + swimHeld;
				if (a_state.sweat + a_state.rain > 0.0f) {
					const float u = vc + Phys::kLawVe;
					const float uNew = u * std::exp(-lawScale * PhysLawRate(vc) / u * dt);
					const float f = std::clamp((uNew - Phys::kLawVe) / std::max(vc, 1e-6f), 0.0f, 1.0f);
					a_state.sweat *= f;
					a_state.rain *= f;
				}
			} else if (total > 1e-4f) {
				a_state.sweat = std::max(a_state.sweat - loss * a_state.sweat / total, 0.0f);
				a_state.rain = std::max(a_state.rain - loss * a_state.rain / total, 0.0f);
			}
			// A swim zone's (band's) drying progress. (47l) v2: its held water follows kDryCurve (progress 1 = dry skin) at the law's pace,
			// faster or slower by the law's rate per um at the water there (the zone with this frame's sweat and rain) against the
			// zone alone; dry skin (p >= 1): the slower classes go on at the zone's own pace.
			const auto dryProgress = [&](float& o_p) {
				if (law) {
					float g = 1.0f;
					if (o_p < 1.0f) {
						const float hz = heldOf(o_p);
						const float loc = hz + a_state.sweat + a_state.rain;
						g = (PhysLawRate(loc) / (loc + Phys::kLawVe)) / std::max(PhysLawRate(hz) / (hz + Phys::kLawVe), 1e-6f);
					}
					o_p = std::min(o_p + zoneRate * g * dt, Phys::kMaxProgress);
					return;
				}
				const float held = Phys::kHold * std::clamp(1.0f - o_p, 0.0f, 1.0f);
				// Dry skin (p >= 1): the slower classes (hair, cloth ...) go on drying at the full flux.
				const float share = o_p < 1.0f ? held / std::max(held + a_state.sweat + a_state.rain, 1e-3f) : 1.0f;
				o_p = std::min(o_p + loss * share / Phys::kHold, Phys::kMaxProgress);
			};
			if (limbMode) {
				// (47l) Each limb's zones dry as the 47j line's (removal included, at that limb's own depth).
				for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
					auto& st = a_state.limbs[i];
					const float d = limbDepth[i];
					if (st.midHeight > d)
						dryProgress(st.midP);
					if (st.lineHeight > st.midHeight)
						dryProgress(st.lineP);
					if (st.lineHeight > st.midHeight && st.lineP >= lineDry) {
						st.lineHeight = st.midHeight;
						st.lineP = st.midP;
						st.lineX = st.midX;
					}
					if (st.lineHeight <= st.midHeight && st.midHeight > d && st.midP >= lineDry) {
						st.lineHeight = st.midHeight = st.dipMin = d;
						st.lineP = st.midP;
						st.lineX = st.midX;
					}
				}
				continue;
			}
			if (lineMode) {
				auto& st = a_state;
				const auto dryZone = [&](float& o_p) { dryProgress(o_p); };
				if (st.midHeight > depth)
					dryZone(st.midP);
				if (st.lineHeight > st.midHeight)
					dryZone(st.lineP);
				// A zone where everything (hair, the slowest gear) is dry goes: the upper one ends at the lower one; with the
				// upper one gone, a dry lower one ends at the surface (no water line left out of the water).
				if (st.lineHeight > st.midHeight && st.lineP >= lineDry) {
					st.lineHeight = st.midHeight;
					st.lineP = st.midP;
					st.lineX = st.midX;
				}
				if (st.lineHeight <= st.midHeight && st.midHeight > depth && st.midP >= lineDry) {
					st.lineHeight = st.midHeight = st.dipMin = depth;
					st.lineP = st.midP;
					st.lineX = st.midX;
				}
				continue;
			}
			for (uint32_t b = 0; b < Phys::kBands; ++b) {
				if (!submerged(b))
					dryProgress(a_state.bandP[b]);
			}
		}
	}
	a_state.bandHeight = bandHeight;
	a_state.depth = depth;
	if (depth > 0.0f)
		a_state.wasInWater = true;
	a_state.exposure = exposure;
	a_state.evaporation = evaporation;
	if (!law)
		a_state.sweatDrip = 0.0f;
	for (float* v : { &a_state.heat, &a_state.rainRunoff, &a_state.sweatDrip }) {
		if (!std::isfinite(*v))
			*v = 0.0f;
		*v = std::clamp(*v, 0.0f, 1.0f);
	}
	for (float* v : { &a_state.sweat, &a_state.rain }) {
		if (!std::isfinite(*v))
			*v = 0.0f;
		*v = std::clamp(*v, 0.0f, Phys::kSoak);
	}
	for (uint32_t b = 0; b < Phys::kBands; ++b) {
		if (!std::isfinite(a_state.bandP[b]))
			a_state.bandP[b] = Phys::kMaxProgress;
		if (!std::isfinite(a_state.bandX[b]))
			a_state.bandX[b] = 0.0f;
	}
	// (47j)
	if (!std::isfinite(a_state.lineHeight))
		a_state.lineHeight = 0.0f;
	a_state.lineHeight = std::clamp(a_state.lineHeight, 0.0f, actorHeight + 64.0f);
	a_state.lineP = std::isfinite(a_state.lineP) ? std::clamp(a_state.lineP, 0.0f, Phys::kMaxProgress) : Phys::kMaxProgress;
	a_state.lineX = std::isfinite(a_state.lineX) ? std::clamp(a_state.lineX, 0.0f, 1.0f) : 0.0f;
	a_state.midHeight = std::isfinite(a_state.midHeight) ? std::clamp(a_state.midHeight, 0.0f, a_state.lineHeight) : 0.0f;
	a_state.midP = std::isfinite(a_state.midP) ? std::clamp(a_state.midP, 0.0f, Phys::kMaxProgress) : Phys::kMaxProgress;
	a_state.midX = std::isfinite(a_state.midX) ? std::clamp(a_state.midX, 0.0f, 1.0f) : 0.0f;
	a_state.dipMin = std::isfinite(a_state.dipMin) ? std::clamp(a_state.dipMin, 0.0f, a_state.midHeight) : 0.0f;
	// (47l)
	for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
		auto& l = a_state.limbs[i];
		l.depth = limbMode ? limbDepth[i] : 0.0f;
		l.lineHeight = std::isfinite(l.lineHeight) ? std::clamp(l.lineHeight, 0.0f, actorHeight + 64.0f) : 0.0f;
		l.lineP = std::isfinite(l.lineP) ? std::clamp(l.lineP, 0.0f, Phys::kMaxProgress) : Phys::kMaxProgress;
		l.lineX = std::isfinite(l.lineX) ? std::clamp(l.lineX, 0.0f, 1.0f) : 0.0f;
		l.midHeight = std::isfinite(l.midHeight) ? std::clamp(l.midHeight, 0.0f, l.lineHeight) : 0.0f;
		l.midP = std::isfinite(l.midP) ? std::clamp(l.midP, 0.0f, Phys::kMaxProgress) : Phys::kMaxProgress;
		l.midX = std::isfinite(l.midX) ? std::clamp(l.midX, 0.0f, 1.0f) : 0.0f;
		l.dipMin = std::isfinite(l.dipMin) ? std::clamp(l.dipMin, 0.0f, l.midHeight) : 0.0f;
		l.lastDepth = std::isfinite(l.lastDepth) ? std::max(l.lastDepth, 0.0f) : 0.0f;
	}
}

float WetSkin::PhysSwimBeadGate(float a_v) const
{
	// (47j) Beads Fade In: the film breaks up between kBreakHi and kBreakLo (per pixel modulated by the patch noise; this is
	// the character's average). Off: 47i (a closed film above 160 um).
	if (settings.PhysBeadsFadeIn)
		return Smooth(0.0f, 1.0f, 0.2f * std::log2((Phys::kBreakHi - Phys::kBreakLo) / std::max(a_v - Phys::kBreakLo, 1e-3f)));  // even in time (Skin.hlsli WetPhysBreakup)
	return SwimBeadGate(a_v);
}

WetSkin::PhysicalLook WetSkin::GetPhysicalLook(const PhysicalState& a_state) const
{
	const auto& s = settings;
	PhysicalLook look;
	const float size = std::clamp(s.PhysDropletSize, 0.5f, 3.0f);
	float beading = std::clamp(s.Beading, 0.0f, 1.0f);

	// Manual: a fixed amount of water with the chosen source's look; Oil is its own film (never dries).
	const float manual = std::clamp(s.ExtraWetness, 0.0f, 1.0f);
	const auto manualLook = static_cast<ManualLook>(std::min(s.ManualLookType, static_cast<uint32_t>(ManualLook::Oil)));
	const float manualWater = manualLook == ManualLook::Oil ? 0.0f : manual * Phys::kSoak;
	look.oil = manualLook == ManualLook::Oil ? manual : 0.0f;
	const float sweat = a_state.sweat + (manualLook == ManualLook::Sweat ? manualWater : 0.0f);
	const float rain = a_state.rain + (manualLook == ManualLook::Rain ? manualWater : 0.0f);
	const float manualSwim = manualLook == ManualLook::Swim ? manualWater : 0.0f;
	const float swimWeight = std::clamp(s.SwimWeight, 0.0f, 1.0f);

	// Oil Repels Water: on oiled skin the water stays in round, distinct beads and forms almost no film.
	look.filmMult = 1.0f;
	look.dome = 1.0f;
	if (s.OilRepelsWater && look.oil > 0.0f) {
		beading = std::lerp(beading, 0.9f, look.oil);
		look.filmMult = 1.0f - 0.8f * look.oil;
		look.dome = 1.0f + 0.6f * look.oil;
	}

	// The beads of each source at the character's water (the shader scales the count with each pixel's own water).
	// (47k) Bead Coalescence Cap / Less Grid-like Bead Placement (both off: 47j).
	BeadLaw law;
	law.cap = s.PhysBeadCap;
	law.beading = beading;
	law.cCap = Phys::kCapScale * beading * std::sqrt(beading);
	law.maxCoverage = s.PhysLessGrid ? Phys::kLessGridCoverage : Phys::kMaxCoverage;
	const BeadSet sweatBeads = SweatBeads(sweat * beading, size, law);
	const BeadSet rainBeads = RainBeads(rain * beading, size, law);
	float swimBeadWater = 0.0f, swimBeadWeight = 0.0f, swimMax = 0.0f, swimTrail = 0.0f;
	const bool dryLaw = s.PhysSweat2 && a_state.dryLaw;  // (47l)
	const auto addSwim = [&](float a_p, float a_x) {
		const float v = PhysBandWater(a_p, a_x, dryLaw) * swimWeight;
		swimMax = std::max(swimMax, v);
		swimTrail = std::max(swimTrail, swimWeight > 0.0f ? std::pow(std::clamp(a_x, 0.0f, 1.0f), 0.25f) : 0.0f);
		const float bead = v * beading * PhysSwimBeadGate(v);
		swimBeadWater += bead * bead;  // weighted by itself: the bands that bead most set the size
		swimBeadWeight += bead;
	};
	const bool lineMode = s.PhysWaterLine && a_state.lineMode;
	const bool limbMode = lineMode && a_state.limbMode;  // (47l); (47m) per Limb is permanent
	if (limbMode) {
		// (47l) Water Line per Limb: every limb's two zones; under the surface soaked.
		for (const auto& l : a_state.limbs) {
			if (l.midHeight > l.depth)
				addSwim(l.midP, l.midX);
			if (l.lineHeight > std::max(l.midHeight, l.depth))
				addSwim(l.lineP, l.lineX);
			if (l.depth > 0.0f)
				swimMax = std::max(swimMax, Phys::kSoak * swimWeight);
		}
		if (a_state.depth > 0.0f)
			swimMax = std::max(swimMax, Phys::kSoak * swimWeight);
	} else if (lineMode) {
		// (47j) Water line: the part between the surface and the line (one state) and the part under the surface (soaked).
		if (a_state.midHeight > a_state.depth)
			addSwim(a_state.midP, a_state.midX);
		if (a_state.lineHeight > std::max(a_state.midHeight, a_state.depth))
			addSwim(a_state.lineP, a_state.lineX);
		if (a_state.depth > 0.0f)
			swimMax = std::max(swimMax, Phys::kSoak * swimWeight);  // under the surface: soaked, a closed film (no beads, no tails)
	} else {
		for (uint32_t b = 0; b < Phys::kBands; ++b)
			addSwim(a_state.bandP[b], a_state.bandX[b]);
	}
	{
		const float bead = manualSwim * beading * PhysSwimBeadGate(manualSwim);
		swimBeadWater += bead * bead;
		swimBeadWeight += bead;
	}
	const BeadSet swimBeads = SwimBeads(swimBeadWeight > 1e-4f ? swimBeadWater / swimBeadWeight : 0.0f, size, law);

	// One parameter set, weighted by bead water; coverage adds up.
	float water = 0.0f, radius = 0.0f, visible = 0.0f, coverage = 0.0f, kept = 0.0f;
	for (const BeadSet* b : { &sweatBeads, &rainBeads, &swimBeads }) {
		water += b->water;
		radius += b->r * b->water;
		visible += b->visible * b->water;
		kept += b->kept * b->water;
		coverage += b->coverage;
	}
	if (water > 1e-4f) {
		radius /= water;
		visible /= water;
		kept /= water;
	} else {
		kept = 1.0f;
	}
	// (47k) Cap: the summed coverage never above c_cap (47j: the full grid's 0.554).
	coverage = std::min(coverage, law.cap ? std::min(law.cCap, law.maxCoverage) : law.maxCoverage);
	const float drawRadius = std::max(radius, Phys::kDrawRadius);
	// (47k) Cap: beads drawn larger than they are (the 2 mm minimum) are drawn fewer: coverage x sqrt(r / r_draw); the bead water
	// not drawn (run off, or the fewer drawn beads) goes to the film sheen (the visible share sent to the shader).
	const float drawFactor = law.cap ? std::sqrt(std::min(1.0f, radius / Phys::kDrawRadius)) : 1.0f;
	const float drawnShare = law.cap ? visible * kept * drawFactor : visible;
	// presence = pixel bead water x this (the visible share in it): presence 1 = the grid's full coverage.
	const float presencePerWater = water > 1e-4f ? coverage * drawFactor / (law.maxCoverage * water) * visible : 0.0f;
	// (47k) Cap: no pixel above c_cap's presence (nor 0.75): never a full lattice where a region gathers more water.
	const float presenceMax = law.cap ? std::min(law.cCap / law.maxCoverage, Phys::kPresenceMax) : 0.0f;
	const float rainRunoff = law.cap ? std::max(a_state.rainRunoff, rainBeads.runoff) : a_state.rainRunoff;
	// (47l) v2: sweat dripping over the drip level runs in trails too (with the beads' own runoff).
	const float sweatRunoff = dryLaw ? std::max(sweatBeads.runoff, a_state.sweatDrip) : sweatBeads.runoff;

	look.beadRadius = radius;
	look.coverage = coverage;
	look.visible = visible;
	look.swimMax = swimMax;
	look.sweatRunoff = sweatRunoff;
	// Droplet tails only where water runs off.
	look.beadTrails = std::clamp(std::max({ rainRunoff, sweatRunoff, swimTrail, law.cap ? swimBeads.runoff : 0.0f }), 0.0f, 1.0f);
	look.sweatLevel = std::clamp(a_state.sweat / Phys::kSweatMax, 0.0f, 1.0f);

	look.params[0] = float4(sweat, rain, manualSwim, a_state.bandHeight);
	look.params[1] = float4(beading, beading, beading, swimWeight);
	look.params[2] = float4(drawRadius, presencePerWater, rainRunoff, sweatRunoff);
	// (47j) Flags (Skin.hlsli WetPhysFlag): the water line, beads fading in from the film, beads shrinking out. (47k) The bead cap
	// (presenceMax in [3].z) and the smaller bead per cell.
	const uint32_t flags = (lineMode ? Phys::kFlagWaterLine : 0u) | (s.PhysBeadsFadeIn ? Phys::kFlagFadeIn : 0u) | (s.PhysBeadsShrink ? Phys::kFlagShrink : 0u) |
	                       (law.cap ? Phys::kFlagBeadCap : 0u) | (s.PhysLessGrid ? Phys::kFlagLessGrid : 0u) | (dryLaw ? Phys::kFlagDryLaw : 0u) |
	                       (limbMode ? Phys::kFlagLimbs : 0u);
	look.params[3] = float4(drawnShare, a_state.depth, presenceMax, static_cast<float>(flags));
	if (limbMode) {
		// (47l) Per limb (line, lineP, lineX, mid), (midP, midX); the one-line slots hold the torso's (geometry not mapped to the body:
		// rigid gear, by the world height).
		for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
			const auto& l = a_state.limbs[i];
			look.limbs[2 * i] = float4(l.lineHeight, l.lineP, l.lineX, l.midHeight);
			look.limbs[2 * i + 1] = float4(l.midP, l.midX, 0.0f, 0.0f);
		}
		look.limbMode = true;
		const auto& torso = a_state.limbs[2];
		look.params[0].w = torso.lineHeight;
		look.bands[0] = float4(torso.lineP, torso.lineX, torso.midHeight, 0.0f);
		look.bands[1] = float4(torso.midP, torso.midX, 0.0f, 0.0f);
	} else if (lineMode) {
		// (47j) The water line height (units above the feet, 0 = none) in place of the band height; the two zones in bands 0 and 1.
		look.params[0].w = a_state.lineHeight;
		look.bands[0] = float4(a_state.lineP, a_state.lineX, a_state.midHeight, 0.0f);  // upper zone; z the lower zone's top
		look.bands[1] = float4(a_state.midP, a_state.midX, 0.0f, 0.0f);                // lower zone
	} else {
		for (uint32_t b = 0; b < Phys::kBands; ++b)
			look.bands[b] = float4(a_state.bandP[b], a_state.bandX[b], 0.0f, 0.0f);
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

	// (47m) The user presets (46a) were removed.

	// 2. Model. (47f) Saved values: 1 = Legacy, 0 = Simple (the 44a-47e "Advanced" model, unchanged), 2 = Physical.
	{
		const char* models[] = { "Legacy", "Simple (old, for comparison)", "Physical" };
		constexpr uint32_t kSaved[] = { 1u, 0u, 2u };
		int shown = IsOriginal() ? 0 : (IsPhysical() ? 2 : 1);
		if (ImGui::Combo("Wetness Model", &shown, models, IM_ARRAYSIZE(models)))
			s.Model = kSaved[std::clamp(shown, 0, 2)];
		Tooltip({ "Legacy (pre-42c): every character exactly as before the wetness work (41b): skin, hair and gear. Only safety limits are kept.",
			"Simple (old): the 44a-47e model, kept only to compare against; it will be removed once Physical is confirmed.",
			"Physical: every character carries real amounts of water from sweat, rain and swimming. Water arrives, runs off when",
			"there is too much, and dries at one pace; how it looks (sheen, beads, trails) follows how much water is where." });
	}

	// 3. (47c) Performance switches.
	ImGui::SeparatorText("Performance");
	ImGui::Checkbox("Wetness: Skip Invisible Work", &s.SkipInvisibleWork);
	Tooltip({ "Same picture, less GPU work: skips noise layers too fine to see, rivulets thinner than half a pixel,",
		"rain droplets and rivulets on skin under a roof (when rain is the only water), dry hair's clump pattern,",
		"the see-through-clothes depth copy while nobody is in water. Off = as in 47b." });
	ImGui::Checkbox("Wetness: Faster Noise & Distance LOD", &s.FasterNoiseLOD);
	Tooltip({ "Simple and Physical. A much cheaper noise for the wet patches (skin and gear): same sliders, same look on average,",
		"the patches sit in other places. Droplets and rivulets fade out on characters far away (about 13 m and farther);",
		"their wet sheen and darkening stay. Off = as in 47b." });

	// (47i) Sweat from stamina (or Dynamic Wetness), for every model (moved here from Advanced Skin).
	const auto drawSweatSource = [&]() {
		auto& skin = globals::features::skin;
		if (skin.isDynamicWetnessAvailable) {
			ImGui::Checkbox("Use Dynamic Wetness", &s.UseDynamicWetness);
			Tooltip({ "Dynamic Wetness (a mod) is installed: let it decide how exerted a character is, instead of the stamina thresholds below." });
		}
		const bool dynamic = s.UseDynamicWetness && skin.isDynamicWetnessAvailable;
		ImGui::BeginDisabled(dynamic);
		ImGui::SliderFloat("Stamina Threshold for Sweat", &s.StartSweat, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Tooltip({ "A character starts to exert itself (and to sweat) when its stamina drops below this share (0.75 = 75 %)." });
		ImGui::SliderFloat("Full Sweat Threshold", &s.FullSweat, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp);
		Tooltip({ "Stamina share at which the exertion is full." });
		ImGui::EndDisabled();
	};

	if (IsOriginal()) {
		// 5. Legacy's own look, 6. the manual slider (0-2, as before).
		ImGui::SeparatorText("Legacy (pre-42c)");
		ImGui::SliderFloat("Wetness Film Strength", &s.FilmStrength, 0.0f, 3.0f, "%.2f");
		Tooltip({ "Advanced Skin's water film, as before. Its energy is capped at 2 (no mirror-like reflections)." });
		ImGui::SliderFloat("Wetness Perlin Noise Scale", &s.WetParams.x, 0.0f, 1024.0f, "%1.f");
		ImGui::SliderFloat("Wetness Perlin Noise Lacunarity", &s.WetParams.y, 0.0f, 2.0f, "%.1f");
		ImGui::SliderFloat("Wetness Perlin Noise Persistence", &s.WetParams.z, 0.0f, 20.0f, "%.2f");
		ImGui::SliderFloat("Wetness Normal Scale", &s.WetParams.w, 0.0f, 20.0f, "%.1f");
		// (47i) Moved here from Advanced Skin.
		ImGui::SliderFloat("Wetness Fade Out Time", &globals::features::skin.settings.WetFadeTime, 0.0f, 50.0f, "%.2f");
		Tooltip({ "Legacy only: time for the wetness to go from 1 to 0 after leaving the water (and for sweat to fade)." });
		drawSweatSource();
		ImGui::SeparatorText("Manual");
		ImGui::SliderFloat("Extra Wetness (manual)", &s.ExtraWetness, 0.0f, 2.0f, "%.2f");
		Tooltip({ "Makes skin and hair this wet (0-2, as before)." });
		return;
	}

	if (IsPhysical()) {
		// (47i) Physical: grouped by what the water does.
		ImGui::SeparatorText("Sweat");
		drawSweatSource();
		ImGui::SliderFloat("Sweat", &s.SweatWeight, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How much a fully exerted character sweats (1 = heavily). Most on the forehead, neck, chest and back, least on the legs.",
			"Hair and gear take their own share (Hair / Gear sections); sweat never makes beads on hair or gear." });
		ImGui::SliderFloat("Sweat Build-up", &s.SweatBuildup, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		if (s.PhysSweat2)
			Tooltip({ "How fast sweat comes (x the sweat made per second). Body heat follows the exertion over about 8 s and stays up",
				"for about a minute after stopping, so the sweat goes on for a while. Cold air makes less sweat. The sweat dries like any",
				"water (Drying); much more than the skin can hold drips off in trails." });
		else
			Tooltip({ "How fast sweat comes. 1: body heat follows the exertion over about 20 s, and a fully exerted character reaches",
				"most of its sweat in about a minute. A short sprint gives a slight sheen, a long fight a few beads on the forehead.",
				"Cold air makes less sweat. After stopping, the sweat stays and dries like any water (Drying)." });
		ImGui::Checkbox("Physical: Sweat and Drying v2", &s.PhysSweat2);
		Tooltip({ "On (47l): sweat is made by body heat at a steady rate (not topped up to a target), and all water (sweat, rain, swimming)",
			"dries by one smooth rule: thick water goes fast, a thin film slowly, so a sprint or a fight leaves a visible sheen that",
			"fades over two to four minutes. Soaked skin still looks dry after about Dry Time. Off: 47k." });

		ImGui::SeparatorText("Rain");
		ImGui::SliderFloat("Rain Wetting", &s.RainWetting, 0.1f, 5.0f, "%.2fx", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "How fast rain wets a character standing under the open sky. Light rain settles at a little water (a sheen, a few beads);",
			"heavy rain soaks up to Max Rain Wetness and runs off in trails. Under a roof or indoors no rain arrives." });
		ImGui::SliderFloat("Max Rain Wetness", &s.MaxRainWetness, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How much rain water skin can hold at most (1 = what soaked skin holds). More rain runs off and leaves trails." });

		ImGui::SeparatorText("Swim");
		ImGui::SliderFloat("Swimming", &s.SwimWeight, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How wet the water makes everything below its surface (1 = soaked). Each height of the body keeps its own water:",
			"standing with only the feet in the water, the rest of the body dries normally. Out of the water, what skin cannot hold",
			"runs off within seconds (trails), the rest dries (Drying)." });
		ImGui::Checkbox("Physical: Water Line Like Legacy", &s.PhysWaterLine);
		Tooltip({ "On (47j): like Legacy, one water line: the deepest the water reached since the character was last fully dry.",
			"Everything below it dries together, evenly. Under the current water surface it stays soaked; standing with the feet in",
			"the water, the rest below the line dries normally; going deeper than the line raises it and soaks that part again.",
			"Each leg, each arm and the torso keep their own line, measured on the body along their bones, so it stays on the same",
			"spot of the skin while moving (creatures without a usable skeleton keep one line by the world height).",
			"Off: 47i (eight height bands, each drying on its own)." });

		ImGui::SeparatorText("Drying");
		ImGui::SliderFloat("Dry Time (s)", &s.PhysDryTime, 10.0f, 1800.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
		if (s.PhysSweat2)
			Tooltip({ "How long soaked skin takes to look dry in neutral air (the last, invisible film is gone about half as long again).",
				"Thick water leaves fast, a thin film slowly; sweat, rain and swim water dry by the same rule, together.",
				"Rain, snow and fog (humid air) and cold air dry slower; indoors is neutral. Hair and gear multiply it by their own Dry Time x." });
		else
			Tooltip({ "How long soaked skin takes to dry completely in neutral air. Water dries at a steady pace, so a little water dries",
				"quickly and a lot takes longer; sweat, rain and swim water share the same pace. Rain, snow and fog (humid air) and",
				"cold air dry slower; indoors is neutral. Hair and gear multiply it by their own Dry Time x." });

		ImGui::SeparatorText("Beads");
		ImGui::SliderFloat("Beading", &s.Beading, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How much of the water gathers into beads; the rest spreads as a thin film (a sheen). 0 = film only.",
			"Beads only show once they are big enough to see (about 1.2 mm), so a little water is only a sheen, never tiny dots." });
		ImGui::SliderFloat("Droplet Size", &s.PhysDropletSize, 0.5f, 3.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "How big beads grow before they merge and run off. Larger = fewer, bigger beads for the same water." });
		ImGui::SliderFloat("Droplet Size Variation", &s.DropletSizeRandom, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How different the beads are in size (kept narrow)." });
		ImGui::Checkbox("Physical: Beads Fade In From the Film", &s.PhysBeadsFadeIn);
		Tooltip({ "On (47j): after a swim the water film breaks into beads gradually, starting in patches and spreading; each bead grows",
			"in from small while the film fades. Off: 47i (the film turns into full-size beads at once, like rain hitting the body)." });
		ImGui::Checkbox("Physical: Beads Shrink and Fade Out", &s.PhysBeadsShrink);
		Tooltip({ "On (47j): a bead that dries up first shrinks (to about 60 % of its size, never to a pixel), then fades out; the beads",
			"do so one after another, and their shine goes to the film. Off: 47i (each bead disappears at once)." });
		ImGui::Checkbox("Physical: Bead Coalescence Cap", &s.PhysBeadCap);
		Tooltip({ "On (47k): beads merge before they cover much of the skin (at most 0.40 x Beading^1.5 of it, about 14 % at Beading 0.5),",
			"bigger ones run off without packing the rest into more beads, and no pixel is ever a full grid of beads; the water not in",
			"beads shows as film sheen. Also no bright ring around a bead's edge. Off: 47j (up to a full lattice of beads)." });
		ImGui::Checkbox("Physical: Less Grid-like Bead Placement", &s.PhysLessGrid);
		Tooltip({ "On (47k): each bead is a little smaller than its cell (0.36 instead of 0.42 of it), so the beads sit more randomly and",
			"never line up in rows; the same water and coverage. Off: 47j." });
		ImGui::SliderFloat("Droplets on the Face", &s.FaceDroplets, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Droplet Normal Strength", &s.DropletNormal, 0.0f, 2.0f, "%.2f");
		Tooltip({ "How round the beads look (their own little dome, inside the bead only). 0 = flat spots of shine." });
		ImGui::SliderFloat("Droplet Edge", &s.DropletRim, 0.0f, 1.0f, "%.2f");
		ImGui::SliderFloat("Droplet Trails", &s.DropletTrails, 0.0f, 1.0f, "%.2f");
		Tooltip({ "A short wet trail below each bead, only while water is running off." });
	} else {
		// 5. Look: how wet each source makes a character at most (Simple).
		ImGui::SeparatorText("Look (fully wet)");
		ImGui::SliderFloat("Swimming", &s.SwimWeight, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How wet everything under the water line gets. 1 = soaked." });
		ImGui::SliderFloat("Stay Wet After Leaving Water (s)", &s.HoldTime, 0.0f, 120.0f, "%.0f s");
		ImGui::SliderFloat("Dry-off Time (s)", &s.DryTime, 5.0f, 600.0f, "%.0f s", ImGuiSliderFlags_Logarithmic);
		Tooltip({ "For skin. Hair and each kind of gear multiply both times by their own Dry Time x (cloth 2.5x: stays wet longest)." });
		ImGui::SliderFloat("Rain", &s.RainWeight, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How wet rain makes skin and hair (only where the sky reaches them: under a roof they stay dry).",
			"Gear follows the rain wetness of Wetness Effects." });
		ImGui::SliderFloat("Sweat", &s.SweatWeight, 0.0f, 1.0f, "%.2f");
		Tooltip({ "How wet sweat (low stamina, or Dynamic Wetness) makes skin. Hair and gear take their own share of it." });
		drawSweatSource();
	}

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
		Tooltip({ "Where the manual water sits and how it looks: Sweat (forehead, chest, back), Rain (shoulders, head, up-facing",
			"skin), Swim (everywhere, a film that breaks into beads below full), or Oil: an even oily film on the skin only (no beads,",
			"no trails), a little shinier than water with crisp highlights, the skin a little deeper in colour. Oil never dries." });
		ImGui::BeginDisabled(s.ManualLookType != static_cast<uint32_t>(ManualLook::Oil));
		ImGui::Checkbox("Oil Repels Water", &s.OilRepelsWater);
		Tooltip({ "On oiled skin, sweat, rain and swimming water gather in rounder, separate beads and form almost no film." });
		ImGui::EndDisabled();
	}

	if (ImGui::CollapsingHeader(IsPhysical() ? "Skin look###Skin" : "Skin")) {
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

		if (!IsPhysical()) {  // (47i) Physical: the Beads section above
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
		}

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

	// 8. (47f) Status (read-only); (47m) with the material tint debug view, in a "Diagnostics" node closed by default.
	if (ImGui::TreeNode("Diagnostics")) {
		ImGui::BeginDisabled(!s.GearEnabled || s.GearModel == 1);
		ImGui::Checkbox("Debug: Tint by Material Class", reinterpret_cast<bool*>(&s.DebugClassTint));
		Tooltip({ "Paints characters by what the wetness thinks they are: red cloth, yellow leather, blue metal, green fur / other, purple hair." });
		ImGui::EndDisabled();
		DrawStatus();
		ImGui::TreePop();
	}
}

void WetSkin::DrawStatus()
{
	ImGui::SeparatorText("Status");
	const auto* player = RE::PlayerCharacter::GetSingleton();
	const auto& skinFeature = globals::features::skin;
	const Skin::ActorWetState* state = nullptr;
	if (player) {
		if (auto it = skinFeature.actorWetStates.find(player->GetFormID()); it != skinFeature.actorWetStates.end() && it->second.valid)
			state = &it->second;
	}
	if (!IsPhysical()) {
		ImGui::TextDisabled("The water amounts below are kept by the Physical model only.");
	} else if (!state) {
		ImGui::TextDisabled("The player has not been drawn yet.");
	} else {
		// (47i) Water amounts in micrometres of water film (soaked skin holds 120).
		const auto& phys = state->phys;
		const auto& look = state->look;
		ImGui::Text("Player sweat: %.0f um   (body heat %.2f%s)", phys.sweat, phys.heat, phys.sweatDrip > 0.05f ? ", dripping" : "");
		ImGui::Text("Player rain: %.0f um   (rain %.0f%%, open sky %.0f%%%s)", phys.rain, physicalFrame.rainIntensity * 100.0f, phys.exposure * 100.0f,
			phys.rainRunoff > 0.05f ? ", running off" : "");
		const std::string inWater = phys.depth > 0.0f ? std::format("   (in the water, {:.0f} units deep)", phys.depth) : std::string();
		if (settings.PhysWaterLine && phys.limbMode) {
			// (47l) The line of each limb (body units above the soles).
			static constexpr const char* kLimbNames[Phys::kLimbs] = { "left leg", "right leg", "torso", "left arm", "right arm" };
			std::string lines;
			for (uint32_t i = 0; i < Phys::kLimbs; ++i) {
				const auto& l = phys.limbs[i];
				lines += std::format("{}{} {:.0f}", i ? ", " : "", kLimbNames[i], l.lineHeight);
				if (l.depth > 0.0f)
					lines += std::format(" (under water up to {:.0f})", l.depth);
			}
			ImGui::TextWrapped("Player water lines per limb: %s%s", lines.c_str(), inWater.c_str());
		} else if (settings.PhysWaterLine && phys.lineMode) {
			// (47j) The water line and the one state below it.
			if (phys.lineHeight > phys.depth)
				ImGui::Text("Player swim: %.0f um up to %.0f units, %.0f um up to the water line at %.0f units%s", PhysBandWater(phys.midP, phys.midX, phys.dryLaw), phys.midHeight,
					PhysBandWater(phys.lineP, phys.lineX, phys.dryLaw), phys.lineHeight, inWater.c_str());
			else
				ImGui::Text("Player swim: no water line%s", inWater.c_str());
		} else {
			std::string bands;
			for (uint32_t b = 0; b < Phys::kBands; ++b)
				bands += std::format("{}{:.0f}", b ? " " : "", PhysBandWater(phys.bandP[b], phys.bandX[b], phys.dryLaw));
			ImGui::Text("Player swim, feet to head: %s um%s", bands.c_str(), inWater.c_str());
		}
		ImGui::Text("Oil: %.2f   Drying speed (air): %.2fx", look.oil, phys.evaporation);
		ImGui::TextDisabled("Beads: radius %.2f mm, cover %.1f%%, visible %.0f%%; trails %.2f", look.beadRadius * 10.0f, look.coverage * 100.0f, look.visible * 100.0f, look.beadTrails);
		// (47k)
		const auto& stats = globals::features::skin.bodyLineStats;
		ImGui::TextDisabled("Water line on the body: %u character draws by the body, %u by the world height (rigid gear, first person, creatures)",
			stats.body, stats.world);
	}
}

void WetSkin::LoadSettings(json& o_json)
{
	settings = o_json;
	PhysicalFromJson(o_json, settings);  // (47f)
	// (47i) Saved before 47i: the new Physical options from the old ones, and the sweat thresholds from Advanced Skin, once.
	try {
		MigratePhysical47i(o_json, g_legacySkin, settings);
	} catch (...) {
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
	// (47i) The sweat thresholds and Dynamic Wetness moved here from Advanced Skin.
	if (g_legacySkin.is_object()) {
		try {
			settings.StartSweat = std::clamp(g_legacySkin.value("StartSweat", settings.StartSweat), 0.0f, 1.0f);
			settings.FullSweat = std::clamp(g_legacySkin.value("FullSweat", settings.FullSweat), 0.0f, 1.0f);
			settings.UseDynamicWetness = g_legacySkin.value("UseDynamicWetness", settings.UseDynamicWetness);
		} catch (...) {
		}
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
