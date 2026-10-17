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

		// (47f) Physical model: per-character sweat / rain / swim levels with their own wetting and drying on the CPU
		// (WetSkin::UpdatePhysical), each source with its own look (beads, trails, film). Multipliers, 1 = the base
		// rates derived from Hold / Dry-off Time.
		float SweatBuildup = 1.0f;
		float SweatDrying = 1.0f;
		float RainWetting = 1.0f;
		float RainDrying = 1.0f;
		float SwimDrying = 1.0f;
		float MaxRainWetness = 0.8f;  // wetness that only the heaviest rain reaches on an exposed character
		uint32_t ManualLookType = 0;  // ManualLook
		bool OilRepelsWater = true;

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

	// ---- (47f) Physical model -----------------------------------------------------------------------
	/// One character's water, advanced on the CPU once per frame (Skin::GetWetness).
	struct PhysicalState
	{
		bool valid = false;
		float heat1 = 0.0f;     // exertion through two first-order lags: the onset of sweating and its after-run
		float heat2 = 0.0f;
		float sweat = 0.0f;     // 0..1 sweat on the skin (production = evaporation at equilibrium)
		float rain = 0.0f;      // 0..Max Rain Wetness, rain water on the character
		float swimRun = 1e6f;   // s since leaving the water, x Swim Drying (runoff clock; 0 in the water)
		float swimEvap = 1e6f;  // the same, x the evaporation environment (evaporation clock)
		bool wasInWater = false;
		float exposure = 0.0f;  // last rain exposure (0 sheltered / indoors, 1 open sky), for the status line
		float evaporation = 1.0f;  // last evaporation environment factor (1 = neutral)
	};
	/// What a Physical character hands the shader on top of the frame's GPUParams (Skin::BSLightingShader_SetupGeometry).
	struct PhysicalLook
	{
		float swim = 0.0f;        // skin swim level (wetSkinPerGeometry.x)
		float swimClock = 1e6f;   // runoff clock (wetCharPerGeometry.x; hair and gear divide it by their dry multiplier)
		float runoffTime = 10.0f; // wetCharParams[0].x
		float evapTime = 170.0f;  // wetCharParams[0].y: the evaporation time on the runoff clock
		float sweat = 0.0f;       // skinPerGeometry.x
		float rain = 0.0f;        // wetSkinParams[0].w: the rain level itself
		float manualWater = 0.0f; // wetSkinParams[0].y (0 when the manual look is Oil)
		float oil = 0.0f;         // wetCharParams[11].w
		float peak = 0.0f;        // wetCharParams[11].z: the character's strongest source (droplet gate)
		float beads = 0.0f;       // wetSkinPerGeometry.w: droplet driver
		float trails = 0.0f;      // wetCharPerGeometry.w: rivulet driver
		float film = 1.0f;        // x Wet Look Strength (wetSkinParams[1].w)
		float density = 1.0f;     // x Droplet Density (wetSkinParams[2].w)
		float size = 1.0f;        // x Droplet Size (wetSkinParams[3].x)
		float sizeRandom = 1.0f;  // x Size Variation (wetSkinParams[3].y)
		float beadTrails = 1.0f;  // x Droplet Trails (wetSkinParams[4].x)
		float dome = 1.0f;        // x Droplet Normal Strength (wetSkinParams[5].z)
		// (47g) Gear and hair: sweat (and the manual Sweat look) makes no beads or trails there, only wicking darkening and
		// a gloss change. The drivers of the other sources, and the flag, packed into wetCharParams[3].w (PackGearDrivers).
		float gearBeads = 0.0f;
		float gearTrails = 0.0f;
		bool manualSweat = false;
	};
	/// Per frame, shared by every character.
	struct PhysicalFrame
	{
		uint32_t frame = UINT32_MAX;
		float rainIntensity = 0.0f;  // 0..1 (Wetness Effects)
		float snowing = 0.0f;        // 0..1 snow weather share
	};
	/// Inputs of one character this frame.
	struct PhysicalInput
	{
		float dt = 0.0f;        // game seconds since the last update
		float exertion = 0.0f;  // the sweat source (stamina or Dynamic Wetness), 0..1
		bool inWater = false;
		float exposure = 0.0f;  // 0 sheltered / indoors .. 1 under the open sky
		float cold = 0.0f;      // 0..1 (cold-region map / snow line / snow weather)
	};
	const PhysicalFrame& GetPhysicalFrame();
	/// Advances a character's sweat, rain and swim water (exact exponential steps, any dt).
	void UpdatePhysical(PhysicalState& a_state, const PhysicalInput& a_in) const;
	/// Turns the levels into the per-source look (CPU, once per character per frame).
	PhysicalLook GetPhysicalLook(const PhysicalState& a_state) const;
	/// The base times the multipliers scale: runoff (s), evaporation of a soaked skin (s, = Hold + Dry-off Time).
	float PhysicalRunoffTime() const;
	float PhysicalEvaporationTime() const;
	/// Swim level of a class with a_dryMult at runoff clock a_run (the shader's CharSwimClass, Physical branch).
	static float PhysicalSwimLevel(float a_run, float a_runoffTime, float a_evapTime, float a_dryMult);
	static constexpr float kRunoffShare = 0.45f;  // share of a soaked skin's water that runs off rather than evaporates

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
