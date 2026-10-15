#pragma once

struct PhysicalSky final : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	static PhysicalSky* GetSingleton()
	{
		static PhysicalSky singleton;
		return &singleton;
	}

	// Metadata
	inline std::string GetName() override { return "Physical Sky"; }
	inline std::string GetShortName() override { return "PhysicalSky"; }
	inline std::string_view GetCategory() const override { return "Sky"; }
	inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Physically based sky models for photorealistic sky gradients, plus other astronomical effects.",
			{
				"Sky.",
				"Cheese.",
			}
		};
	}

	// Functionality
	bool inline SupportsVR() override { return true; }
	inline std::string_view GetShaderDefineName() override { return "PHYSICAL_SKY"; }
	inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	// Settings & UI
	void DataLoaded() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	void DrawSettings() override;
	void SettingsGeneral();
	void SettingsWorldspaces();
	void SettingsCelestials();
	void SettingsAtmosphere();
	void SettingsClouds();
	void SettingsDebug();

	// Resources
	void SetupResources() override;
	void ClearShaderCache() override;
	void CompileShaders();
	bool ShadersOK();

	// Draw
	void Reset() override;
	void EarlyPrepass() override;
	void ReflectionsPrepass() override;
	void Prepass() override;
	void GenerateLuts();
	void AccumShadow();
	inline void PostPostLoad() override { Hooks::Install(); }

	////////////////////////////////////////////////// Feature Specific Data
	constexpr static uint16_t kTrLutW = 256;
	constexpr static uint16_t kTrLutH = 64;
	constexpr static uint16_t kMsLutW = 32;
	constexpr static uint16_t kMsLutH = 32;
	constexpr static uint16_t kSvLutW = 200;
	constexpr static uint16_t kSvLutH = 150;
	constexpr static uint16_t kApLutW = 32;
	constexpr static uint16_t kApLutH = 32;
	constexpr static uint16_t kApLutD = 32;
	/// (46z, E1) "High-Resolution Aerial Perspective": directions of the AP LUT when on (depth stays kApLutD).
	constexpr static uint16_t kApLutHiResW = 64;
	constexpr static uint16_t kApLutHiResH = 64;

	struct WorldspaceInfo
	{
		float zBottom = -14500.f;
	};

	/// The 9 hard-coded worldspaces of 37a and earlier. Used as-is while the 37b master is off.
	static const std::map<std::string, WorldspaceInfo>& LegacyWorldspaceWhitelist();
	/// (batch 37b) Legacy list + the Dawnguard exteriors that are earthly skies:
	/// DLC1HunterHQWorld (Fort Dawnguard), DLC1VampireCastleCourtyard (Castle Volkihar
	/// courtyard), DLC1AncestorsGladeWorld. EDIDs and heights read from Dawnguard.esm.
	static std::map<std::string, WorldspaceInfo> DefaultWorldspaceWhitelist();
	/// (batch 37b) Never physical sky, even if listed: other realms (Soul Cairn, Boneyard,
	/// Apocrypha, Sovngarde) and any worldspace flagged "No Sky" (Blackreach, Darkfall Passage...).
	static bool IsExcludedWorldspace(const RE::TESWorldSpace* a_worldspace);
	/// (batch 37b) Planet ground for worldspaces not in the list: default water height (following
	/// the parent when the worldspace uses the parent's land or water) minus 500, the same offset
	/// Tamriel's -14500 has from its -14000 sea level.
	static float FallbackZBottom(const RE::TESWorldSpace* a_worldspace);

	/// Current exterior worldspace; falls back to the player cell's worldspace (upstream 693f6a35e).
	static RE::TESWorldSpace* GetCurrentWorldspace();

	enum class WorldspaceStatus
	{
		Unknown,
		Interior,
		Whitelist,
		AllExteriors,
		Excluded,
		NotListed
	};
	/// What Reset() decides for the current worldspace, also used by the menu.
	WorldspaceStatus GetWorldspaceStatus(float& a_zBottom) const;

	/// (41b, F2) "Moon Glow Fades".
	enum MoonGlowFade : int
	{
		kMoonGlowFadeWithDisc = 0,  // 40d: the vanilla disc's fade alpha (gone 16 degrees up)
		kMoonGlowFadeAtHorizon,     // the moon's true elevation, -2 -> +3 degrees
		kMoonGlowFadeCount
	};
	/// (42b) "Cloud Lighting".
	enum CloudLighting : int
	{
		kCloudLightingSingle = 0,  // 42a and earlier: the one directional light (sun or a moon)
		kCloudLightingAll,         // the sun, both moons and the sky, each on its own
		kCloudLightingCount
	};
	/// (41b, F5) "Sky Model".
	enum SkyModel : int
	{
		kSkyModelCurrent = 0,
		kSkyModelLegacy36f,
		kSkyModelCount
	};
	/// (46z, A1) "Vanilla Sun Halo (with Physical Sky)": the large halo baked into the game's sun picture
	/// (and the sun glare picture). Their 8-bit alpha steps show as rings over the dark physical sky.
	enum SunHalo : int
	{
		kSunHaloOff = 0,  // only the opaque disc body of the sun picture; no glare
		kSunHaloSmooth,   // halo smoothed (steps blurred along the radius, dithered), faded with the LUT sun
		kSunHaloKeep,     // 46y and earlier: drawn as the picture has it
		kSunHaloCount
	};
	/// (46z, A2) "Sky Dither": noise on the physical sky dome against colour steps.
	enum SkyDither : int
	{
		kSkyDitherOff = 0,  // 46y and earlier
		kSkyDitherFine,     // per pixel and frame, about half a step of the R11G11B10 main target
		kSkyDitherVanilla,  // the vanilla sky's own noise texture at full strength (zero mean)
		kSkyDitherCount
	};
	/// (46z, C2) "Aerosol Preset" / (46z, C3) "Ozone Preset": applied when the constants are uploaded;
	/// the saved Atmosphere values are never written.
	enum AtmospherePreset : int
	{
		kAtmospherePresetCurrent = 0,  // the Atmosphere tab's values
		kAtmospherePresetEarth,        // Hillaire 2020 / Bruneton 2017 Earth values
		kAtmospherePresetCount
	};

	struct Settings
	{
		bool enabled = true;
		bool overrideDirLight = false;
		int tonemapper = 2;
		float vanillaMix = 0;
		float trMix = 0;
		float apLumMix = 1;
		float apTrMix = 1;

		float2 cloudShadowRemapRange = float2{ 0, 1.f };

		float3 sunlightColor = float3{ 1.0f, 0.97f, 0.95f } * 1e3f;
		float3 masserColor = float3{ 1.0f, 0.6f, 0.6f } * 5e-3f;
		float3 secundaColor = float3{ 0.8f, 1.0f, 1.0f } * 5e-3f;

		bool proceduralSun = true;
		float sunDiskRad = DirectX::XMConvertToRadians(0.53f);  // 37a size (really a diameter)

		// (batch 37b) Procedural sun v2. Each follows its own setting (the old master switch is gone).
		/// Disk centred where the vanilla sun is drawn (Sky Sync's apparent direction, dipped
		/// with altitude, in the sky root's frame) instead of the raw sun direction. Also moves
		/// the sky's sun glow and the scattering LUTs onto that direction.
		bool sunAlignToVanilla = true;
		/// Disk replaces the vanilla sun texture (upstream 45ad2c6f7 + 735ec68e4): drawn only on
		/// the sun quad, quad pixels outside the disk cleared.
		bool sunReplaceVanilla = true;
		/// Soft disk edge (upstream fec65ed15).
		bool sunSoftEdge = true;
		/// Disk radiance = light colour / disk solid angle x transmittance (upstream 728eedd61 +
		/// d08484aef), capped at sunRadianceCap (upstream: fixed 62250).
		bool sunPhysicalRadiance = true;
		float sunRadianceCap = 1000.f;
		/// Hide the vanilla sun glare (large halo) while the procedural sun is on.
		bool sunHideVanillaGlare = false;
		/// Angular radius in degrees; 0.27 = the real sun (0.53 across).
		float sunDiskRadiusDeg = 0.27f;
		/// (batch 37c) Glow drawn into the physical sky around the procedural sun. Nothing in
		/// this pipeline blooms the disk (vanilla bloom is skipped with "Disable Vanilla
		/// Tonemapping", COD Bloom is opt-in), so without it a real-size disk is a small flat
		/// white dot. Intensity is relative to a sunlit white wall; 0 = no glow.
		float sunGlowIntensity = 6.f;
		float sunGlowWidthDeg = 0.8f;
		/// (batch 37c) Last "Sun Look" preset picked (SunLook); Custom once a slider is moved.
		int sunLook = 1;
		/// (batch 37c) Hide the solid black disc the game draws for a new moon (both moons share
		/// the vanilla phase cycle, so both go black on the same nights).
		bool hideNewMoonDisc = true;
		/// (40d) Scales only the glow each moon puts into the physical sky (the scattering LUTs),
		/// not the moonlight used for lighting and shadows. At 1 the glow next to the disc is about
		/// as bright as the vanilla disc itself with strong moonlight, so the discs look washed out;
		/// 0.2 keeps the disc about 5x brighter than its glow while the night sky keeps a moonlit
		/// tint.
		float moonGlowStrength = 0.2f;
		/// (40d) Each moon's glow follows its phase (new moon ~0) and its vanilla visibility
		/// (hidden or faded out = no glow).
		bool moonGlowFollowsMoon = true;
		/// (40d) Moon disc brightness = moonlight / disc solid angle (like the sun's Physical
		/// Brightness), capped at moonRadianceCap; the phase picture keeps its shading.
		/// (44h) skyTrueSunHeight, hideNewMoonDisc, moonGlowFollowsMoon, moonGlowFade,
		/// moonPhysicalRadiance and moonsFadeInDaylight are no longer in the menu or the JSON:
		/// UpdateEffective() pins them at these defaults outside Legacy (36f). Code paths kept for a
		/// possible rollback (specs/physsky-rollback-candidates.md).
		bool moonPhysicalRadiance = true;  // (44h) default false -> true (pinned)
		float moonRadianceCap = 2.f;  // (44h) default 8 -> 2 (user's tuned value)
		/// (41b, F2) What fades each moon's sky glow (with moonGlowFollowsMoon): MoonGlowFade.
		int moonGlowFade = kMoonGlowFadeAtHorizon;

		/// (41b, F1) Night sky base light: the current weather's night sky colours (SkyUpper,
		/// Horizon, SkyLower) x this, with a faint airglow floor, faded out as the sun rises from
		/// -6 to +4 degrees. Added to the sky-view LUT as a gradient and to the aerial
		/// perspective LUT as an even glow of the air. 0 = the 41a image exactly.
		float nightBaseLight = 0.2f;
		/// (41b, F4a) The scattering LUTs use the sun's true height. The vanilla altitude dip
		/// stays on the disc and its glow (sunAlignToVanilla).
		bool skyTrueSunHeight = true;
		/// (41b, F4b) In the LUTs only, a sun below the horizon is taken as elevation / this:
		/// 1 = real twilight, 2 = twilight twice as long. Never the directional light.
		float twilightLength = 1.f;
		/// (41b, F5) SkyModel: Current, or Legacy (36f) for comparison (see Effective()).
		int skyModel = kSkyModelCurrent;

		/// (42b) CloudLighting. Single: clouds are lit only by the directional light (whichever body
		/// Sky Sync made it), so they went black whenever it was off (dawn, dusk) and never caught the
		/// sun after sunset. All: the sun (through the air above the cloud, with the planet's shadow),
		/// both moons and the sky's own light, each on its own.
		int cloudLighting = kCloudLightingAll;
		/// (42b) Sky light on clouds (the sky-view LUT, which holds the 41b night base light): 0 = none.
		float cloudSkyAmbient = 1.f;
		/// (42b) Height of the clouds above you used for their sunlight only, km. Higher = they keep
		/// the sun (pink/orange) longer after sunset. The drawn cloud layer is unchanged.
		float cloudLightHeightKm = 2.f;
		/// (42b) Moon discs lose contrast against a bright sky, as a real daytime moon does.
		bool moonsFadeInDaylight = true;
		/// (42b) How strongly: disc alpha x 1 / (1 + this x sky luminance / disc luminance).
		float moonDaylightFade = 20.f;
		/// (44h) "Moonlight on Clouds": the moons light the clouds after the weather's cloud colour
		/// (near black at night) instead of through it. Off = the 42b picture exactly. Needs
		/// Cloud Lighting "Sun + Moons + Sky".
		bool moonCloudLight = true;
		/// (44h) "Moonlight Strength", 0-3.
		float moonCloudStrength = 0.5f;
		/// (44h) "Moon Halo": forward-scatter glow on thin cloud near the moon, 0-4.
		float moonCloudHalo = 1.f;

		// (batch 37b) Upstream correctness fixes (the old master switch is gone).
		// (44h) removed from the menu and the JSON; now fixed on (UpdateEffective() forces them on
		// outside Legacy (36f)). Code paths kept for a possible rollback
		// (specs/physsky-rollback-candidates.md).
		bool fixSkyAlpha = true;  // 5846ad833: sky dome written opaque
		bool fixApShadowDepth = true;  // 224312a11 (depth read only): AP shadow under dynamic resolution
		bool fixReflectionSky = true;  // 23156dc5f: reflected sky takes cloud-cube shadow, not TexApShadow
		bool fixMultiScatter = true;  // c14664115 (LutGen part): full-sphere, isotropic MS LUT. Changes sky colour
		bool fixTrLutEdge = true;  // 9fbd052ad: transmittance LUT read on texel centres, clamp sampler

		float adaptationStart = DirectX::XMConvertToRadians(-2);
		float adaptationEnd = DirectX::XMConvertToRadians(-15);
		float dayExposure = 1e-2f;
		float nightExposure = 1e2f;

		// (batch 37b) Saved and editable now. Starts as DefaultWorldspaceWhitelist(); entries the
		// user removes from that default set are remembered in worldspaceRemovedDefaults, so a
		// later default addition still reaches old configs without resurrecting removed ones.
		std::map<std::string, WorldspaceInfo> worldspaceWhitelist = DefaultWorldspaceWhitelist();
		std::vector<std::string> worldspaceRemovedDefaults = {};
		/// (batch 37b) Enable in every exterior worldspace, whitelisted or not, except the hard
		/// exclusions (IsExcludedWorldspace). zBottom then comes from FallbackZBottom().
		bool enableAllExteriorWorldspaces = false;
		float3 groundAlbedo = { .2f, .2f, .2f };

		float planetRadius = 6.36e3f;      // in km
		float atmosphereRadius = 6.42e3f;  // in km

		float rayleighFalloff = 1 / 8.69645f;                    // in km^-1
		float3 rayleighScatter = { 6.6049f, 12.345f, 29.413f };  // in megameter^-1
		float aerosolFalloff = 1 / 1.2f;
		float aerosolPhaseG = 0.8f;
		float3 aerosolScatter = { 39.96f, 39.96f, 39.96f };
		float3 aerosolAbsorption = { 4.44f, 4.44f, 4.44f };
		float ozoneAltitude = 22.3499f + 35.66071f * .5f;  // in km
		float ozoneThickness = 35.66071f;
		float3 ozoneAbsorption = { 2.2911f, 1.5404f, 0 };

		float cloudRelightMix = 1.f;
		float cloudOriginalMix = 0.5f;
		float silverLiningMix = 1.f;
		float silverLiningSpread = 0.f;

		// (46z) Physical Sky fixes. Outside the NLOHMANN macro (64-field limit): read and written in
		// Load/SaveSettings, tolerant of missing or mistyped keys. The two banding fixes default on,
		// everything else defaults to the 46y look.
		/// A1 SunHalo.
		int sunHalo = kSunHaloSmooth;
		/// A2 SkyDither.
		int skyDither = kSkyDitherFine;
		/// C4 + C5 "Accurate LUT Integration": the LUT ray marches sample the middle of each step (not its
		/// far end), and the multiple-scattering LUT's ground bounce gets its cos/pi and is lit by the sun
		/// at the ground point.
		bool accurateLutIntegration = false;
		/// C1 "Vanilla Mix Follows Sun Height": Vanilla Mix in full only at night (sun below -6 degrees),
		/// gone by sunrise (0 degrees).
		bool vanillaMixFollowsSun = false;
		/// C2 AtmospherePreset for the aerosol scatter / absorption.
		int aerosolPreset = kAtmospherePresetCurrent;
		/// C3 AtmospherePreset for the ozone absorption and layer.
		int ozonePreset = kAtmospherePresetCurrent;
		/// D1 "Stars Fade With Sky Brightness": star alpha x 1 / (1 + strength x sky / star), from twilight on.
		bool starsFadeWithSky = false;
		float starsFadeStrength = 1000.f;
		/// E1 "High-Resolution Aerial Perspective": the AP LUT at 64 x 64 directions instead of 32 x 32.
		bool highResAerialPerspective = false;
	} settings;

	/// (41b, F5) The settings as used this frame. In "Legacy (36f)" mode every option added after
	/// 36f is temporarily at its 36f value (and the 41b options at their "off" values); the saved
	/// settings above are never written. Colours, exposures, atmosphere and clouds are the user's.
	/// Rebuilt by UpdateEffective() (every Reset and after a load).
	const Settings& Effective() const { return effective; }
	/// (41b, F5) Legacy (36f) mode is active.
	bool IsLegacy() const { return loaded && settings.skyModel == kSkyModelLegacy36f; }
	/// (41b, F5) 36f code paths for what the old Batch 37b master switched: hard-coded worldspace
	/// list, 37a sun disc size, no 37b/37c shader flags.
	bool UseLegacyPaths() const;
	void UpdateEffective();
	Settings effective;

	struct CbData
	{
		// DYNAMIC
		float2 texDim;
		float2 rcpTexDim;  //
		float2 frameDim;
		float2 rcpFrameDim;  //

		float zCameraPlanet;
		float3 sunDir;  //
		float3 sunlightColor;
		float trMix;  //
		float3 masserDir;
		float apLumMix;  //
		float3 masserColor;
		float apTrMix;  //
		float3 secundaDir;
		float sunDiskCos;  //
		float3 secundaColor;

		// GENERAL
		uint enabled;  //
		int tonemapper;
		float vanillaMix;

		// WORLD
		float zBottom;
		float rPlanet;  //
		float rAtmosphere;
		float3 groundAlbedo;  //

		// ATMOSPHERE
		float2 cloudShadowRemapRange;

		float aerosolFalloff;
		float aerosolPhaseG;  //
		float3 aerosolScatter;
		float _pad5;  //
		float3 aerosolAbsorption;

		float rayleighFalloff;
		float3 rayleighScatter;  //

		float ozoneAltitude;  //
		float ozoneThickness;
		float3 ozoneAbsorption;  //

		// CLOUDS (VANILLA)
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;
		float silverLiningSpread;  //
	} cbData;
	static_assert(sizeof(CbData) % 16 == 0);

	/// (batch 37b) Mirrors SharedData::PhysSkyExtData (HLSL), appended at the end of FeatureData.
	/// Flags are the 37b switches ANDed with the master; all zero = the 37a shader paths.
	enum ExtFlags : uint32_t
	{
		kExtSunReplace = 1u << 0,
		kExtSunSoftEdge = 1u << 1,
		kExtSunPhysicalRadiance = 1u << 2,
		kExtHideSunGlare = 1u << 3,
		kExtSkyAlphaOpaque = 1u << 4,
		kExtTrLutEdgeFix = 1u << 5,
		kExtApShadowDepthFix = 1u << 6,
		kExtReflectionSkyFix = 1u << 7,
		kExtMultiScatterFix = 1u << 8,
		kExtMoonPhysicalRadiance = 1u << 9,  // (40d)
		// (46z) Defined again as local constants in Sky.hlsl / LutGen.cs.hlsl (SharedData.hlsli is not
		// touched, so Lighting.hlsl and its includes stay byte-identical).
		kExtSunHaloSmooth = 1u << 10,   // A1 Smooth (neither A1 bit = Keep)
		kExtSunHaloOff = 1u << 11,      // A1 Off
		kExtSkyDitherFine = 1u << 12,   // A2 R11G11B10-matched
		kExtSkyDitherVanilla = 1u << 13,  // A2 vanilla strength
		kExtAccurateLut = 1u << 14,     // C4 + C5 (LutGen.cs.hlsl)
		kExtStarsFade = 1u << 15,       // D1 (strength in NightCbData::starsFadeStrength)
	};
	struct ExtCbData
	{
		uint flags = 0;
		float sunRadianceCap = 62250.f;
		float sunGlowIntensity = 0.f;  // (batch 37c) 0 = no glow
		float sunGlowWidth = 0.f;      // (batch 37c) radians
	} extCbData;
	static_assert(sizeof(ExtCbData) == 16);
	void UpdateExtCbData();

	/// (40d) Mirrors SharedData::PhysSkyMoonData (HLSL), appended at the very end of FeatureData.
	/// Multipliers for the vanilla moon disc colour under "Moon Physical Brightness".
	struct MoonCbData
	{
		float masserDiskScale = 1.f;
		float secundaDiskScale = 1.f;
		float pad0 = 0.f;
		float pad1 = 0.f;
	} moonCbData;
	static_assert(sizeof(MoonCbData) == 16);
	/// (40d) Moon glow scale for the LUTs (strength x phase x visibility) and disc scales.
	void UpdateMoonData(float a_exposure, float3& a_masserGlow, float3& a_secundaGlow);

	/// (41b) Mirrors SharedData::PhysSkyNightData (HLSL), appended after PhysSkyMoonData. Read only
	/// by LutGen (sky-view and aerial perspective LUTs).
	enum NightFlags : uint32_t
	{
		kNightBaseLight = 1u << 0,  // F1: add the Base* colours below
	};
	struct NightCbData
	{
		float3 lutSunDir = { 0.f, 0.f, 1.f };  // F4a/F4b: sun direction the LUTs use
		uint flags = 0;
		float3 baseUpper = {};    // F1: linear, x strength x fade (zenith)
		float starsFadeStrength = 0.f;  // (46z, D1) read by Sky.hlsl as PhysSkyNightData.pad0

		float3 baseHorizon = {};  // horizon; also the aerial perspective's even glow
		float pad1 = 0.f;
		float3 baseLower = {};    // below the horizon
		float pad2 = 0.f;
	} nightCbData;
	static_assert(sizeof(NightCbData) == 64);
	static_assert(offsetof(NightCbData, starsFadeStrength) == 28);  // HLSL PhysSkyNightData.pad0
	/// (41b, F1) Fills nightCbData's base light from the current weather; a_sunElevation in radians.
	void UpdateNightBaseLight(float a_sunElevation);
	/// (41b, F2) Glow visibility of a moon by its true elevation; 0 when the weather hides it.
	static float MoonHorizonVisibility(const RE::Moon* a_moon, const RE::Sky* a_sky, float a_sinElevation);

	/// (42b) Mirrors SharedData::PhysSkyCloudData (HLSL), appended after PhysSkyNightData. Read only
	/// by Sky.hlsl.
	enum CloudFlags : uint32_t
	{
		kCloudMultiLight = 1u << 0,        // "Cloud Lighting: Sun + Moons + Sky"
		kCloudMoonDaylightFade = 1u << 1,  // "Moons Fade in Daylight"
		kCloudMoonDirect = 1u << 2,        // (44h) "Moonlight on Clouds": moons added after the weather cloud colour
	};
	struct CloudCbData
	{
		float3 sunColor = {};  // DirLightColor units, before transmittance
		uint flags = 0;
		float3 masserColor = {};
		float skyAmbient = 0.f;
		float3 secundaColor = {};
		float heightOffset = 0.f;  // game units
		float moonDaylightFade = 0.f;
		float moonCloudStrength = 0.f;    // (44h) "Moonlight Strength"
		float moonCloudHalo = 0.f;        // (44h) "Moon Halo"
		float moonCloudSaturation = 0.f;  // (44h) colour kept in the moon term, fixed 0.35
	} cloudCbData;
	static_assert(sizeof(CloudCbData) == 64);
	/// (42b) Fills cloudCbData. a_exposure as for the LUTs, a_sunElevation (radians) of the LUTs' sun.
	void UpdateCloudData(float a_exposure, float a_sunElevation);

	/// (batch 37c) "Sun Look" presets.
	enum SunLook : int
	{
		kSunLookCustom = 0,
		kSunLookBright,   // real size, strong glow: reads as a blinding sun without bloom
		kSunLookSoft,     // slightly larger, gentle glow
		kSunLookVanilla,  // procedural sun off: the game's own sun picture
		kSunLookCount
	};
	void ApplySunLook(int a_look);

	eastl::unique_ptr<Texture2D> texTrLut = nullptr;  // transmittance
	eastl::unique_ptr<Texture2D> texMsLut = nullptr;  // multiscattering
	eastl::unique_ptr<Texture2D> texSvLut = nullptr;  // sky view
	eastl::unique_ptr<Texture3D> texApLut = nullptr;  // aerial perspective
	eastl::unique_ptr<Texture2D> texApShadow = nullptr;
	/// (46z, E1) Directions of texApLut as created (kApLutW x kApLutH, or the hi-res size).
	uint16_t apLutW = kApLutW;
	uint16_t apLutH = kApLutH;
	/// (46z, E1) Creates texApLut at a_w x a_h x kApLutD. On failure the old texture (if any) is kept.
	bool CreateApLut(uint16_t a_w, uint16_t a_h);
	/// (46z, E1) Recreates texApLut when "High-Resolution Aerial Perspective" was toggled. Called before
	/// the LUTs are generated, the first use of the frame.
	void UpdateApLutSize();

	winrt::com_ptr<ID3D11SamplerState> sampTr = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampSv = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampNoise = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csTrLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csMsLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSvLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csApLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccum = nullptr;

	ID3D11SamplerState* originalPSSamplers[2] = { nullptr, nullptr };

	void ModifySky();
	void RestoreSamplers();
	/// (batch 37b) Marks the vanilla sun / sun glare quads for Sky.hlsl (ExtraShaderDescriptors).
	static void SetSunDrawFlags(const RE::BSRenderPass* a_pass);
	static void ClearSunDrawFlags();
	static bool IsNewMoonDraw(const RE::BSRenderPass* a_pass);
	struct Hooks
	{
		struct BSSkyShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSSkyShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupGeometry>(RE::VTABLE_BSSkyShader[0]);
			stl::write_vfunc<0x7, BSSkyShader_RestoreGeometry>(RE::VTABLE_BSSkyShader[0]);
		}
	};
};