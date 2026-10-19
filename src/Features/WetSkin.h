#pragma once

#include "Feature.h"

/**
 * @brief (44a) "Wet Skin", (44c) "Character Wetness": everything about wet characters (skin,
 * hair, gear), in one page (Characters).
 *
 * (44c) Hair (New): the hair highlight model gets wet (darker, R lobe narrower and brighter, TRT
 * weaker, less inter-strand scatter, clumps from strand-space noise), no water film, dry
 * G-buffer. Gear (New): a soft per-pixel class (cloth / leather / metal from TruePBR metallic or
 * the env mask and the gloss; fur for back / soft lit gear; optional item-keyword prior), each
 * class with its own darkening, sheen, roughness floor, drying multiplier and G-buffer gloss cap;
 * droplets and rivulets on metal and leather (direct light only), wicking on cloth.
 *
 * Two models, chosen at runtime (A/B Compare works):
 *  - New (default): one wetness per character from the manual slider, swimming (full while in
 *    the water, held, then drying off), rain (sky-occluded) and sweat, each with its own weight;
 *    soft wet patches that grow and merge as the wetness rises; a broad water-film sheen (F0 0.02,
 *    roughness 0.35 -> 0.22, never below 0.2); energy-conserving darkening; droplets (beads with a
 *    small bright point, their own 0.1 roughness, a slightly dark rim, short trails below them)
 *    and rivulets running down along gravity. Droplets and rivulets fade out where they would be
 *    smaller than a pixel.
 *  - Original (pre-42c): the skin wetness of 41b exactly (Advanced Skin's Perlin film), and the
 *    42c character switches (film, darkening, rain on skin) off for gear too. Kept: the 4.4
 *    numeric fixes, and the anti-mirror clamps (reflections on wet skin at roughness >= 0.2,
 *    film energy <= 2).
 *
 * The data reaches the shader through Advanced Skin's per-geometry constant buffer (b7), so
 * FeatureData does not change; Advanced Skin itself must be enabled.
 */
struct WetSkin : Feature
{
	// (44c) Renamed from "Wet Skin" (the settings key migrates, see GetLegacySettingsName); the short
	// name stays, so WetSkin.ini and the disk cache check do not change.
	virtual inline std::string GetName() override { return "Character Wetness"; }
	virtual std::string_view GetLegacySettingsName() const override { return "Wet Skin"; }
	virtual inline std::string GetShortName() override { return "WetSkin"; }
	virtual std::string_view GetCategory() const override { return "Characters"; }
	virtual inline bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Wet characters: skin, hair and gear get wet together from swimming, rain, sweat and a manual slider, and dry at their own pace.",
			{ "Skin: soft water sheen, droplets and rivulets, never a mirror",
				"Hair: darker, sharper highlight, clumps",
				"Gear: cloth darkens and dries slowest, leather shines a little, metal gets beads",
				"Original (pre-42c) model for A/B comparison" }
		};
	}

	/// Saved values. (47f) Shown as Legacy (1) / Simple (0, the 44a-47e "Advanced" model, unchanged) / Physical (2).
	enum class Model : uint32_t
	{
		New = 0,
		Original = 1,
		Physical = 2,
	};

	/// (47f) Physical: "Manual Look", what the manual slider puts on the skin.
	enum class ManualLook : uint32_t
	{
		Sweat = 0,
		Rain = 1,
		Swim = 2,
		Oil = 3,
	};

	struct Settings
	{
		uint32_t Model = 0;  // 0 = New (Simple), 1 = Original (Legacy, pre-42c), 2 = Physical (47f)

		// (47f) Physical model. (47i) Water amounts per character (um of water film): sweat, rain and a per-height swim
		// profile, one evaporation flux (WetSkin::UpdatePhysical); the look follows the water (specs/spec-batch47.md 47i).
		// Not in the JSON macro (64-argument limit): PhysicalToJson / PhysicalFromJson.
		float SweatBuildup = 1.0f;     // x how fast sweat comes (full exertion: ~80 % after a minute at 1x)
		float RainWetting = 1.0f;      // x how fast rain wets an exposed character
		float MaxRainWetness = 0.8f;   // rain water at most, x the skin's hold capacity (120 um)
		uint32_t ManualLookType = 0;   // ManualLook
		bool OilRepelsWater = true;
		float Beading = 0.5f;          // (47i) share of the water that gathers in beads (the rest is film)
		float PhysDryTime = 170.0f;    // (47i) s for soaked skin to dry fully at a neutral environment
		float PhysDropletSize = 1.0f;  // (47i) coalescence / runoff size scale (bigger: fewer, bigger beads)
		// (47j) A/B switches, all on by default; off = 47i exactly. Not part of presets.
		bool PhysWaterLine = true;     // "Physical: Water Line Like Legacy": one water line (deepest since dry) + the current surface
		bool PhysBeadsFadeIn = true;   // "Physical: Beads Fade In From the Film": the swim film breaks up gradually, beads grow in
		bool PhysBeadsShrink = true;   // "Physical: Beads Shrink and Fade Out": beads shrink to 0.6x, then fade, staggered
		// (47k) A/B switches, all on by default; off = 47j exactly. Not part of presets.
		bool PhysBodyLine = true;      // "Physical: Water Line Follows the Body": the stored line by the bind-pose height (Skin::CharBodyHeight)
		bool PhysBeadCap = true;       // "Physical: Bead Coalescence Cap": coverage <= 0.40 Beading^1.5, runoff keeps the coverage
		bool PhysLessGrid = true;      // "Physical: Less Grid-like Bead Placement": bead radius 0.36 cell (was 0.42), more jitter
		// (47l) A/B switch, on by default; off = 47k exactly. Not part of presets.
		bool PhysSweat2 = true;        // "Physical: Sweat and Drying v2": sweat made in um/s by body heat, one smooth drying law (WetSkin::Phys kLaw*)
		bool PhysLimbLines = true;     // "Physical: Water Line per Limb (bones)": a water line per limb, recorded in body units from its bones
		// (47i) Moved from Advanced Skin (used by every model): sweat from stamina, or from Dynamic Wetness.
		float StartSweat = 0.75f;
		float FullSweat = 0.15f;
		bool UseDynamicWetness = false;

		// (47c) Performance, both on by default; both off = 47b exactly. Not part of presets.
		// SkipInvisibleWork: leave out work whose result is multiplied by 0 (same picture): faded
		// noise octaves, rivulets narrower than half a pixel, rain droplets / rivulets on skin under
		// a roof when rain is its only water, dry hair's clump noise, the see-through-clothes depth
		// copy while nobody is in water, the GPU values once per frame.
		// FasterNoiseLOD: a cheaper wet-patch noise (same parameters and statistics, patches in
		// other places) and no droplets / rivulets on characters far away (about 13 m+ at 1440p).
		bool SkipInvisibleWork = true;
		bool FasterNoiseLOD = true;

		// Wetness input
		float ExtraWetness = 0.0f;  // manual; Original reads it as before (0..2), New uses 0..1
		float SwimWeight = 1.0f;
		float RainWeight = 0.7f;
		float SweatWeight = 0.6f;
		float HoldTime = 20.0f;   // s fully wet after leaving the water (New)
		float DryTime = 150.0f;   // s to dry off after that (New)

		// Sheen (New)
		float RoughnessLow = 0.35f;
		float RoughnessFull = 0.22f;
		float FullWetnessStrength = 1.8f;

		// Coverage: the Perlin noise (both models) and its soft patches (New)
		float4 WetParams = { 512.0f, 0.7f, 10.0f, 4.0f };  // scale, lacunarity, persistence, normal scale (Original)
		float PatchSoftness = 0.35f;
		float PatchContrast = 0.35f;

		// Darkening (New; also Wetness Effects' "Porosity: Skin" for skin without Advanced Skin)
		float Darkening = 0.6f;

		// Original film strength (Advanced Skin "Wetness Film Strength")
		float FilmStrength = 1.5f;

		// Droplets (New)
		float DropletAmount = 1.0f;
		float DropletDensity = 0.45f;
		float DropletSize = 1.0f;
		float DropletSizeRandom = 0.6f;
		float FaceDroplets = 0.4f;
		float DropletNormal = 1.0f;
		float DropletRim = 0.25f;
		float DropletTrails = 0.5f;
		float RainDroplets = 0.4f;

		// Rivulets (New)
		float StreakAmount = 0.5f;
		float StreakLength = 1.0f;
		float StreakWidth = 1.0f;
		// (44e) RivuletContrast2: higher = more visible trails (was StreakContrast, reversed: new = 1 - old).
		// RivuletBetweenDulling: the film between the trails (was the same old value). FaceRivulets: x on the face.
		float RivuletContrast2 = 0.5f;
		float RivuletBetweenDulling = 0.5f;
		float FaceRivulets = 1.0f;

		// (44c) Hair (New model). Hair Specular on: its Marschner / Kajiya-Kay highlight gets wet;
		// off: the vanilla highlight. No water film, dry G-buffer.
		uint32_t HairEnabled = 1;
		float HairDarkening = 0.8f;
		float HairSharpness = 0.55f;       // primary highlight width at full wetness (1 = unchanged)
		float HairHighlightGain = 1.15f;   // primary highlight strength
		float HairSecondaryLoss = 0.65f;   // TRT (secondary, coloured highlight) x (1 - loss)
		float HairScatterLoss = 0.25f;     // inter-strand scatter x (1 - loss)
		float HairClumping = 0.7f;
		float HairClumpCount = 48.0f;      // clumps across one uv
		float HairClumpShift = 0.18f;      // how far each clump's highlight moves
		float HairGapDarkening = 0.35f;
		float HairDryTime = 1.5f;          // x the skin's hold and dry-off times
		float HairSweatShare = 0.3f;

		// (44c) Gear (New model): soft per-pixel classes. GearModel 0 = New, 1 = 42c (44b, A/B).
		uint32_t GearEnabled = 1;
		uint32_t GearModel = 0;
		uint32_t GearKeywords = 1;         // (44f) use the worn item's keywords as a prior
		float GearKeywordTrust = 0.5f;
		uint32_t DebugClassTint = 0;
		float GearSheen = 1.0f;            // overall sheen multiplier
		float GearPatchContrast = 0.25f;
		float GearWicking = 4.0f;          // game units the water climbs up cloth above the water line
		float GearSweatShare = 0.3f;
		float4 GearCloth = { 1.0f, 0.25f, 0.45f, 2.5f };    // darkening, sheen, roughness floor, dry-time multiplier
		float4 GearLeather = { 0.55f, 0.7f, 0.30f, 1.5f };
		float4 GearMetal = { 0.0f, 0.6f, 0.25f, 0.5f };
		float4 GearFur = { 0.7f, 0.15f, 0.50f, 2.0f };
		float LeatherDroplets = 0.6f;
		float LeatherRivulets = 0.4f;
		float MetalDroplets = 1.0f;
		float MetalRivulets = 0.6f;
		// (46a) See-through (alpha-blended) clothes of a character standing in water vanished below the
		// surface: they are drawn after the water, whose depth hides them. On: those draws test against the
		// depth from before the water (Skin::OnDrawStatesApplied), and their part below the surface is
		// tinted and faded by UnderWaterTint. Off = the game's behaviour.
		bool TranslucentUnderWater = false;
		float UnderWaterTint = 0.6f;
	} settings;

	/// What the shader reads (Skin.hlsli wetSkinParams[6]).
	struct alignas(16) GPUParams
	{
		float4 p[6];
		float4 c[12];  // (44c) Skin.hlsli wetCharParams[12]: hair and gear
	};

	/// (44c) The longest drying multiplier of any class (skin = 1): the water line is kept until the
	/// slowest one is dry.
	float MaxDryMultiplier() const;
	GPUParams GetGPUParams() const;

	bool IsOriginal() const { return settings.Model == static_cast<uint32_t>(Model::Original); }
	/// (47f) The Physical model (a superset of Simple: every "!IsOriginal()" path applies to it too).
	bool IsPhysical() const { return settings.Model == static_cast<uint32_t>(Model::Physical); }

	// ---- (47f) Physical model; (47i) water amounts ---------------------------------------------------
	/// (47i) Constants of the water model (specs/spec-batch47.md 47i; Skin.hlsli kPhys* must match). Water in um of film,
	/// bead sizes in cm.
	struct Phys
	{
		static constexpr uint32_t kBands = 8;           // swim height profile, feet to head (Skin.hlsli wetPhysBands)
		static constexpr float kSoak = 200.0f;          // in the water
		static constexpr float kHold = 120.0f;          // what the skin holds; more runs off
		static constexpr float kRunoffTau = 3.0f;       // s
		static constexpr float kFilmFull = 80.0f;       // film water at full sheen
		static constexpr float kSweatMax = 150.0f;      // x Sweat x heat
		static constexpr float kSweatTau = 20.0f;       // s, / Sweat Build-up
		static constexpr float kHeatUp = 20.0f;         // s
		static constexpr float kHeatDown = 90.0f;       // s
		static constexpr float kRainRate = 2.0f;        // um/s at full intensity, x Rain Wetting
		static constexpr float kRainDrain = 45.0f;      // s: while it rains, the impacts shed water (lighter rain settles lower)
		static constexpr float kBeadKappa = 1.44f;      // bead volume = kappa r^3 (a cap of ~75 degrees, r = base radius)
		static constexpr float kSweatPores = 100.0f;    // per cm2
		static constexpr float kSweatCoalesce = 0.05f;  // coverage, / Droplet Size (few, larger beads)
		static constexpr float kRainRadius = 0.065f;    // one impact
		static constexpr float kRainCoalesce = 0.4f;    // coverage, / Droplet Size
		static constexpr float kSwimBeads = 1.5f;       // per cm2, / Droplet Size^2
		static constexpr float kVisibleR0 = 0.05f;      // beads show from r 0.5 .. 0.7 mm (below: sheen only)
		static constexpr float kVisibleR1 = 0.07f;
		static constexpr float kRunoffRadius = 0.125f;  // x Droplet Size: larger beads run off
		static constexpr float kDrawRadius = 0.2f;      // drawn at least this large (at the same coverage): 2 mm, visible up close
		static constexpr float kMaxCoverage = 0.554f;   // the cell grid at presence 1 (pi 0.42^2)
		static constexpr float kMaxProgress = 4.0f;     // drying progress kept (the slowest class multiplier)
		// (47j) Beads Fade In: the swim film breaks into beads between these amounts (Skin.hlsli kPhysBreak*).
		static constexpr float kBreakHi = 200.0f;
		static constexpr float kBreakLo = 120.0f;
		static constexpr float kLineSplit = 3.0f;  // (47j) units the water must come back up inside the lower zone to re-soak it
		// (47j) Flags in wetPhysParams[3].w (Skin.hlsli WetPhysFlag).
		static constexpr uint32_t kFlagWaterLine = 1u;
		static constexpr uint32_t kFlagFadeIn = 2u;
		static constexpr uint32_t kFlagShrink = 4u;
		// (47k) Bead Coalescence Cap (per-pixel presence clamp in wetPhysParams[3].z, halo normal clamp) / Less Grid-like Bead
		// Placement (bead radius kLessGridRadius per cell). Skin.hlsli kPhysFlag*.
		static constexpr uint32_t kFlagBeadCap = 8u;
		static constexpr uint32_t kFlagLessGrid = 16u;
		static constexpr float kCapScale = 0.40f;             // c_cap(B) = kCapScale B^1.5
		static constexpr float kLessGridRadius = 0.36f;       // bead radius per cell (47j: 0.42)
		static constexpr float kLessGridCoverage = 0.4072f;   // pi 0.36^2: the cell grid at presence 1
		static constexpr float kPresenceMax = 0.75f;          // per-pixel presence never above this (Bead Coalescence Cap)
		// (47l) Sweat and Drying v2 (specs/spec-batch47.md 47l). One loss law for every source, smooth (no regime switch):
		//   E(V) = env x kLawLambda / Dry Time x [ln(1 + U / kLawV0) + kLawB U^2] um/s, U = V + kLawVe (V = the water there, um):
		// thin films go slowly (linear below ~3 um), thick water fast (the U^2 term); soaked skin (200 um) is below 5 um after Dry
		// Time (calibrated with the swim runoff), the last 0.3 um (invisible) goes in finite time. Sweat is made at kSweatRate2 um/s
		// x Sweat x Build-up x heat (8 s up / 60 s down), and over kDripLevel x Sweat it drips (tau kRunoffTau) into the trails.
		static constexpr uint32_t kFlagDryLaw = 32u;    // wetPhysParams[3].w: the swim zones' held water follows kDryCurve (Skin.hlsli)
		static constexpr float kLawV0 = 3.0f;           // um
		static constexpr float kLawB = 0.002f;          // 1 / um
		static constexpr float kLawVe = 0.3f;           // um
		static constexpr float kLawLambda = 18.8325f;   // calibration (Dry Time meaning)
		static constexpr float kLawTauEnd = 1.56747f;   // Dry Time units from held 120 um to 0 (the swim zone's progress 0 .. 1)
		static constexpr float kSweatRate2 = 2.4f;      // um/s
		static constexpr float kHeatUp2 = 8.0f;         // s
		static constexpr float kHeatDown2 = 60.0f;      // s
		static constexpr float kDripLevel = 150.0f;     // um x Sweat
		static constexpr float kDripSoft = 4.0f;        // um (softplus width: no kink)
		// (47l) Water Line per Limb: left leg, right leg, torso / head, left arm, right arm (Skin.hlsli wetPhysLimbs).
		static constexpr uint32_t kLimbs = 5;
		static constexpr uint32_t kFlagLimbs = 64u;  // wetPhysParams[3].w: the lines are per limb (Skin.hlsli kPhysFlagLimbs)
		static constexpr uint32_t kDryCurveN = 24;
		/// The held water of a swim zone (share of kHold) at progress p = u^2, u = i / kDryCurveN (Skin.hlsli kPhysDryCurve).
		static constexpr float kDryCurve[kDryCurveN + 1] = { 1.000000f, 0.986224f, 0.946868f, 0.887163f, 0.813968f, 0.734044f, 0.652900f,
			0.574383f, 0.500785f, 0.433202f, 0.371927f, 0.316773f, 0.267314f, 0.223037f, 0.183439f, 0.148090f, 0.116668f, 0.088982f,
			0.064987f, 0.044771f, 0.028500f, 0.016274f, 0.007911f, 0.002796f, 0.000000f };
	};
	/// (47l) Water Line per Limb: one limb's water line (body units: the bind-pose height x the skeleton's scale, from its bones) and
	/// its two evenly drying zones, exactly as the 47j line (PhysicalState line* / mid*).
	struct LimbLine
	{
		float lineHeight = 0.0f;
		float lineP = Phys::kMaxProgress;
		float lineX = 0.0f;
		float midHeight = 0.0f;
		float midP = Phys::kMaxProgress;
		float midX = 0.0f;
		float dipMin = 0.0f;
		float depth = 0.0f;      // how far up the limb (body units) is under water this frame (0 = none)
		float lastDepth = 0.0f;  // last frame's: the line rises only as far as two frames agree (a one-frame glitch never raises it)
	};
	/// One character's water, advanced on the CPU once per frame (Skin::GetWetness).
	struct PhysicalState
	{
		bool valid = false;
		float heat = 0.0f;         // exertion lag (20 s up, 90 s down)
		float sweat = 0.0f;        // um
		float rain = 0.0f;         // um
		float rainRunoff = 0.0f;   // 0..1, rain running off (at the cap and still raining)
		std::array<float, Phys::kBands> bandP{};  // drying progress per height band (0 soaked .. 1 dry skin .. kMaxProgress)
		std::array<float, Phys::kBands> bandX{};  // share of the above-capacity water still on (1 in the water)
		float bandHeight = 0.0f;   // units per band (actor height / kBands)
		// (47j) Water Line Like Legacy: the deepest water line above the feet since last fully dry (lineHeight, 0 = none). Below the
		// current surface: soaked. Above it at most two zones, each drying evenly as one: the lower one (surface .. midHeight, state
		// mid*) emerged last; the upper one (midHeight .. lineHeight, state line*) earlier. Usually only one is there.
		float lineHeight = 0.0f;
		float lineP = Phys::kMaxProgress;
		float lineX = 0.0f;
		float midHeight = 0.0f;
		float midP = Phys::kMaxProgress;
		float midX = 0.0f;
		float dipMin = 0.0f;      // the lowest surface since the lower zone was last soaked
		bool lineMode = false;     // the state above was advanced by the water-line model (a switch starts afresh)
		bool dryLaw = false;       // (47l) the swim progress above follows kDryCurve (Sweat and Drying v2; a switch starts afresh)
		float sweatDrip = 0.0f;    // (47l) 0..1, sweat dripping over kDripLevel x Sweat (trails)
		bool limbMode = false;     // (47l) the swim water is in limbs[] (Water Line per Limb, a body-mapped character; a switch starts afresh)
		std::array<LimbLine, Phys::kLimbs> limbs{};
		float depth = 0.0f;        // current water depth above the feet (0 = not in the water)
		bool wasInWater = false;
		float exposure = 0.0f;     // last rain exposure (status)
		float evaporation = 1.0f;  // last evaporation environment factor (1 = neutral)
	};
	/// What a Physical character hands the shader (Skin::BSLightingShader_SetupGeometry; Skin.hlsli wetPhysParams / wetPhysBands).
	struct PhysicalLook
	{
		std::array<float4, 4> params{};
		std::array<float4, Phys::kBands> bands{};
		float oil = 0.0f;          // wetCharParams[11].w
		float filmMult = 1.0f;     // x Wet Look Strength (wetSkinParams[1].w): Oil Repels Water
		float beadTrails = 0.0f;   // x Droplet Trails (wetSkinParams[4].x): runoff only
		float dome = 1.0f;         // x Droplet Normal Strength (wetSkinParams[5].z)
		float sweatLevel = 0.0f;   // skinPerGeometry.x: sweat / max (0..1)
		// (47l) Water Line per Limb (Skin.hlsli wetPhysLimbs): per limb (line, lineP, lineX, mid), (midP, midX, 0, 0).
		bool limbMode = false;
		std::array<float4, 2 * Phys::kLimbs> limbs{};
		// Status
		float beadRadius = 0.0f;   // cm, physical (mixed)
		float coverage = 0.0f;
		float visible = 0.0f;
		float swimMax = 0.0f;      // um, the wettest band (skin)
		float sweatRunoff = 0.0f;
	};
	/// Per frame, shared by every character.
	struct PhysicalFrame
	{
		uint32_t frame = UINT32_MAX;
		float rainIntensity = 0.0f;  // 0..1 (Wetness Effects)
		float snowing = 0.0f;        // 0..1 snow weather share
		float fogHumidity = 0.0f;    // (47i) 0..1 from the weather's fog distance
	};
	/// Inputs of one character this frame.
	struct PhysicalInput
	{
		float dt = 0.0f;        // game seconds since the last update
		float exertion = 0.0f;  // the sweat source (stamina or Dynamic Wetness), 0..1
		float depth = 0.0f;     // (47i) local water surface above the feet (0 = not in the water)
		float height = 128.0f;  // (47i) the actor's height (units)
		bool outdoors = false;  // (47i) interiors: neutral air
		float exposure = 0.0f;  // 0 sheltered / indoors .. 1 under the open sky
		float cold = 0.0f;      // 0..1 (cold-region map / snow line / snow weather)
		// (47l) Water Line per Limb: the character is body-mapped (Skin::UpdateBodyLimbs), how far up each limb is under water and each
		// limb's range of body heights (body units).
		bool limbs = false;
		std::array<float, Phys::kLimbs> limbDepth{};
		std::array<float, Phys::kLimbs> limbLo{};
		std::array<float, Phys::kLimbs> limbHi{};
	};
	const PhysicalFrame& GetPhysicalFrame();
	/// Advances a character's sweat, rain and swim water.
	void UpdatePhysical(PhysicalState& a_state, const PhysicalInput& a_in) const;
	/// Turns the water into what the shader draws (CPU, once per character per frame).
	PhysicalLook GetPhysicalLook(const PhysicalState& a_state) const;
	/// (47i) Skin swim water of a band (um, before the Swimming weight). (47l) a_law: the held part by kDryCurve (else linear in p).
	static float PhysBandWater(float a_p, float a_x, bool a_law = false);
	/// (47l) Sweat and Drying v2: the held share of a swim zone at progress a_p (kDryCurve; 0 from 1 on).
	static float PhysDryCurve(float a_p);
	/// (47l) The loss law's E(V) / env x Dry Time (um per Dry Time) at water a_v.
	static float PhysLawRate(float a_v);
	/// (47j) The character's swim bead gate (share of the swim water that beads, before Beading) at swim water a_v (um).
	float PhysSwimBeadGate(float a_v) const;

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/// (44a) Migration from 43b: Advanced Skin and Wetness Effects hand over their raw JSON while
	/// they load; taken when "Wet Skin" has no settings of its own yet.
	static void StashLegacySkinJson(const json& a_skin);
	static void StashLegacyWetnessJson(const json& a_wetness);

	/// (46a) User presets: every Character Wetness setting, one JSON file per preset in
	/// kPresetDirectory (after PostProcessing's SavePresetTo). No presets ship with the plugin.
	static constexpr const char* kPresetDirectory = "Data\\SKSE\\Plugins\\CommunityShaders\\CharacterWetness";
	std::vector<std::string> ListPresets() const;
	bool SavePreset(const std::string& a_name);
	bool LoadPreset(const std::string& a_name);
	bool RenamePreset(const std::string& a_from, const std::string& a_to);
	bool DeletePreset(const std::string& a_name);
	/// A usable preset name (no path characters, not empty, at most 64 bytes) or empty.
	static std::string SanitizePresetName(const std::string& a_name);

private:
	PhysicalFrame physicalFrame;
	void DrawStatus();
	void DrawPresets();
	/// (47b) Overwrite / Delete / Rename / Save as New, in the "Manage Presets" node under "Presets".
	void DrawManagePresets(bool a_selected);
	std::vector<std::string> presetNames;
	int presetIndex = -1;
	bool presetsListed = false;
	std::string presetNewName;
	std::string presetRenameTo;
	std::string presetStatus;
};
