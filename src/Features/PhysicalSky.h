#pragma once

#include "Buffer.h"
#include "PhysicalSkyModel.h"

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

	struct WorldspaceInfo
	{
		float zBottom = -14500.f;
	};

	/// The 9 hard-coded worldspaces of 37a and earlier (the base of DefaultWorldspaceWhitelist()).
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
	/// (49j) Reset() will draw the physical sky this frame (master, shaders, Sky Sync, worldspace, not in the main / loading menu): Physical
	/// Brightness decides its scene before the features' Reset.
	bool WillDraw();

	/// (42b) "Cloud Lighting".
	enum CloudLighting : int
	{
		kCloudLightingSingle = 0,  // 42a and earlier: the one directional light (sun or a moon)
		kCloudLightingAll,         // the sun, both moons and the sky, each on its own
		kCloudLightingCount
	};
	/// (47b) "Aerosol".
	enum AerosolPreset : int
	{
		kAerosolCurrent = 0,  // the saved Aerosol (Mie) values
		kAerosolContinental,  // upstream 846c97ede: OPAC continental clean, 50% humidity, reference loading
		kAerosolPresetCount
	};
	/// (47b) The aerosol optics as uploaded (Mm^-1): the saved values, or the preset's.
	void GetUploadedAerosol(float3& a_scatter, float3& a_absorption, float& a_phaseG) const;
	// (47m) "Sky Model: Legacy (36f)" removed: the Current model is the only one (the old skyModel key is ignored on
	// load). The 44h fixes and celestial options it switched off are fixed on in the code.

	struct Settings
	{
		bool enabled = true;
		/// (47a) "Sky Fixes": one switch for the batch 47a sky (timing from the climate, sky brightness
		/// following the ground, ozone 10-40 km, midpoint LUTs, ground bounce fix, 64x64 aerial
		/// perspective, stars fading with the sky, the vanilla sun by its true height, glare hidden by
		/// ridges, dither, earlier moonlight retreat). (48o) Always on: no switch, the old skyFixes key is ignored.
		static constexpr bool skyFixes = true;
		/// (47b) "Sky Precision": upstream's LUT precision work (Bruneton transmittance LUT with analytic planet
		/// occlusion, nonuniform midpoint quadrature, sky-view rows around the true horizon, quadratic aerial
		/// perspective slices). (48o) Always on: no switch, the old skyPrecision key is ignored.
		static constexpr bool skyPrecision = true;
		// (47m) "Horizon Fix" (47f) removed: fixed off (the old horizonFix key is ignored on load).
		/// (47b) "Aerosol": AerosolPreset. Applied at upload; the saved aerosol values are never changed.
		int aerosolPreset = 0;
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

		// (49h) The procedural sun is retired (Sky.hlsl draws the game's sun with the Sky Fixes halo); the keys are ignored on load.
		static constexpr bool proceduralSun = false;
		// (47m) sunDiskRad (the 37a disc size, read only by Legacy (36f)) removed; the old key is ignored on load.

		// (batch 37b) Procedural sun v2. Each follows its own setting (the old master switch is gone).
		/// Disk centred where the vanilla sun is drawn (Sky Sync's apparent direction, dipped
		/// with altitude, in the sky root's frame) instead of the raw sun direction. Also moves
		/// the sky's sun glow and the scattering LUTs onto that direction.
		/// (49h) Fixed on (it still sets the scattering LUTs' and the light's sun direction); the key is ignored on load.
		static constexpr bool sunAlignToVanilla = true;
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
		/// (40d) Scales only the glow each moon puts into the physical sky (the scattering LUTs),
		/// not the moonlight used for lighting and shadows. At 1 the glow next to the disc is about
		/// as bright as the vanilla disc itself with strong moonlight, so the discs look washed out;
		/// 0.2 keeps the disc about 5x brighter than its glow while the night sky keeps a moonlit
		/// tint.
		float moonGlowStrength = 0.2f;
		// (47m) The 44h pins skyTrueSunHeight, hideNewMoonDisc, moonGlowFollowsMoon, moonGlowFade (At Horizon),
		// moonPhysicalRadiance and moonsFadeInDaylight (all on) are no longer fields: the code always takes that side.
		/// (40d) Moon disc brightness = moonlight / disc solid angle (like the sun's Physical Brightness),
		/// capped at this; the phase picture keeps its shading.
		float moonRadianceCap = 2.f;  // (44h) default 8 -> 2 (user's tuned value)
		/// (48b) "Moonlight Fades Stars": stars and the Milky Way fade with the moonlit sky, worked out from the full
		/// moonlight (phase x height x weather x night x light colour, both moons, x 1 - cloud cover around each), not
		/// from the glow above (Moon Sky Glow Strength). Faint stars and the Milky Way go first. Off = 48a exactly.
		/// Saved outside the NLOHMANN macro.
		bool moonFadesStars = true;
		/// (49h) "Sun Highlight on Water Follows the Atmosphere" (Celestials, under Transmittance Mix): Water.hlsl's sun highlight x the
		/// transmittance toward the light (PhysSky::SampleTr), as every surface gets it (NightFlags kNightWaterSunTr). Off = 49g.
		bool waterSunTransmittance = true;
		/// (49j, design 9-4) "Rain Without Colour" (Scene Lighting > Related): Particle.hlsl's precipitation keeps only its texture's brightness
		/// (PhysLightFlags kPLRainNoColour). Independent of the Physical Lighting / Physical Brightness masters. Off = 49i.
		bool rainNoColour = true;
		/// (49h stage 1) "Physical Lighting" (Scene Lighting tab): one master and one checkbox per rule (S1-S8, brief-49h-stage1.md).
		/// Master off = 49h groups 1-7. Saved outside the NLOHMANN macro (key "physicalLighting").
		struct PhysicalLighting
		{
			bool enabled = true;
			bool sameDirectLight = true;   // S1: clouds / lit effects / moonlight in ground units, weather hides the moon on the ground too
			bool moonCloudGrey = false;    // S1 option: keep the old partial desaturation of moonlight on clouds
			bool cloudAmbient = true;      // S2: clouds get the ground's sky ambient (not x the weather cloud colour)
			bool iblNoFeedback = true;     // S3a: the screen probe is captured without the IBL ambient already in the frame
			bool iblEnvOcclusion = true;   // S3b: the environment probe's upper hemisphere is blocked by skylighting
			bool farShadows = true;        // S4a: with RT sun shadows, full objects are shadow-tagged out to the RT trace extent
			bool farSkylight = true;       // S4b: beyond the skylighting probe volume, a fallback visibility instead of open sky
			bool lodScale = true;          // S5: LOD land in the near land's light scale (PBR land), LOD gamma on both LOD land techniques
			bool farFog = true;            // S6: the far height fog continues the volumetric fog's light
			bool forwardCubemap = true;    // S7: forward cubemap reflections at the cubemap's own brightness, ambient converted
			bool smallFixes = true;        // S8: small consistency fixes
			// (49h stage 2, design-49h-stage2.md): four checkboxes under the master, and the engineering constants of the collapsed
			// "Developer" node. Off = stage 1 exactly (S = 1, no ambient write, the user's IBL values).
			bool realSkyRatios = true;           // T1: one compression S of the real sun / moon / sky ratios on every physical source
			bool physicalAmbient = true;         // T2: the engine ambient (DALC) rewritten from the physical sky with the weather's hue
			bool measuredClouds = true;          // T3: cloud cover and the clouds in front of the sun / moons from Cloud Shadows' cube
			bool cloudsStarsWaterFollow = true;  // T4: stars and the water's body colour follow the physical night
			// (49i) Under Physical Ambient, default on (off = 49h): the ground's bounce light from below, and the ambient's direction fit.
			bool groundBounce = true;  // keys "groundBounce": rho x (direct + sky on a horizontal face) x (1 - n.z) / 2 added to the ambient
			// "ambientFit": the engine transform fitted so the decoded ambient matches at 26 normals. Default OFF: offline (synthetic clear
			// sky, sun 60 / 30 / 5 deg) the 49h construction errs +17..+30 % up and -18..-29 % on shaded sides without the bounce, 2-15 % with
			// Ground Bounce, and the fit does not lower the largest error (its readout shows both in game).
			bool ambientFit = false;
			bool probeForget = true;   // "probeForget": the IBL environment probe captures the sky in view as no bounce, out-of-view texels fade
			float compressionK = 0.3f;           // Developer: E_pipe = E_ref (E_real / E_ref)^k
			float overcastKappa = 0.25f;         // Developer: light an overcast layer passes on (of the blocked sun + the clear sky)
			float airglowRatio = 25.f;           // Developer: full-moon sky luminance / airglow luminance
			float smoothingSeconds = 1.5f;       // Developer: S and the ambient follow the sky with this time constant
			float hueSaturationCap = 0.6f;       // Developer: the weather hue's saturation at most
		} physicalLighting;
		/// (48b) Its strength, 0-3. 1: Masser full and high on a clear night keeps about 40% of the Milky Way's light.
		float moonStarFadeStrength = 1.f;
		/// (48b) "Moon Planetshine": a faint grey fill on the unlit side of each moon disc (strongest at crescent, none at
		/// full or new moon), so in front of the Milky Way it reads as a dim disc instead of a hole. (48o) Always on (its
		/// strength slider stays); the old moonPlanetshine key is ignored.
		static constexpr bool moonPlanetshine = true;
		/// (48b) Its strength, 0-0.5: the fill's opacity at a thin crescent / 0.75.
		float moonPlanetshineStrength = 0.1f;

		/// (41b, F1) Night sky base light: the current weather's night sky colours (SkyUpper,
		/// Horizon, SkyLower) x this, with a faint airglow floor, faded out as the sun rises from
		/// -6 to +4 degrees. Added to the sky-view LUT as a gradient and to the aerial
		/// perspective LUT as an even glow of the air. 0 = the 41a image exactly.
		float nightBaseLight = 0.2f;
		/// (41b, F4b) In the LUTs only, a sun below the horizon is taken as elevation / this:
		/// 1 = real twilight, 2 = twilight twice as long. Never the directional light.
		float twilightLength = 1.f;

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
		/// (42b) Moon discs lose contrast against a bright sky, as a real daytime moon does. How strongly: disc alpha x 1 / (1 + this x sky luminance / disc luminance).
		float moonDaylightFade = 20.f;
		/// (44h) "Moonlight on Clouds": the moons light the clouds after the weather's cloud colour
		/// (near black at night) instead of through it. Off = the 42b picture exactly. Needs
		/// Cloud Lighting "Sun + Moons + Sky".
		bool moonCloudLight = true;
		/// (44h) "Moonlight Strength", 0-3.
		float moonCloudStrength = 0.5f;
		/// (44h) "Moon Halo": forward-scatter glow on thin cloud near the moon, 0-4.
		float moonCloudHalo = 1.f;

		// (47m) The 37b upstream fixes fixSkyAlpha, fixApShadowDepth, fixReflectionSky, fixMultiScatter and fixTrLutEdge
		// (pinned on since 44h) are no longer fields: UpdateExtCbData() always writes their bits.

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
	} settings;

	/// (41b, F5) The settings as used this frame: the saved settings with what Sky Fixes takes over
	/// (Twilight Length) replaced; the saved settings above are never written.
	/// Rebuilt by UpdateEffective() (every Reset and after a load).
	const Settings& Effective() const { return effective; }
	/// (47a) "Sky Fixes" is in effect: on, Physical Sky loaded and enabled. Sky Sync's timing, sun
	/// fade and moonlight retreat follow this too (also where the physical sky itself is not drawn,
	/// e.g. interiors, so the sun does not jump at a worldspace border).
	bool SkyFixesActive() const { return loaded && settings.enabled && settings.skyFixes; }
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
		kExtSkyPrecision = 1u << 10,         // (47b) "Sky Precision" (Common.hlsli kPhysSkyPrecisionFlag)
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
		/// (48b, HLSL pad0.x) "Moonlight Fades Stars" amounts, two halves: low 16 bits Masser, high Secunda (Sky.hlsl MoonSky).
		uint32_t starFadeHalves = 0;
		/// (48b, HLSL pad0.y) "Moon Planetshine" fill opacities, two halves: low 16 bits Masser, high Secunda.
		uint32_t planetshineHalves = 0;
	} moonCbData;
	static_assert(sizeof(MoonCbData) == 16);
	/// (40d) Moon glow scale for the LUTs (strength x phase x visibility) and disc scales.
	/// (48b) Also the "Moonlight Fades Stars" / "Moon Planetshine" values and their NightFlags bits (after
	/// UpdateNightBaseLight, which resets the flags). a_sunElevation: the sun's true elevation, radians.
	void UpdateMoonData(float a_exposure, float a_sunElevation, float3& a_masserGlow, float3& a_secundaGlow);
	/// (48b) Fills the starFade / planetshine halves and their NightFlags bits.
	void UpdateMoonSky(float a_exposure, float a_sunElevation);
	/// (48b) Values written this frame, for the menu and the F7 header: [0] Masser, [1] Secunda.
	float moonStarFadeAmount[2] = { 0.f, 0.f };
	float moonPlanetshineAmount[2] = { 0.f, 0.f };
	/// (48b) Drowning amount per unit of moonlight luminance at strength 1 (see UpdateMoonData).
	static constexpr float kMoonStarFadeScale = 0.15f;
	/// (48b) V-band extinction (magnitudes per airmass) for the moon-height weight (Krisciunas & Schaefer 1991).
	static constexpr float kMoonSkyExtinction = 0.172f;
	/// (48b) "Moonlight Fades Stars" / "Moon Planetshine" in the F7 trace header.
	std::string DescribeForTrace() const;

	/// (41b) Mirrors SharedData::PhysSkyNightData (HLSL), appended after PhysSkyMoonData. Read only
	/// by LutGen (sky-view and aerial perspective LUTs).
	enum NightFlags : uint32_t
	{
		kNightBaseLight = 1u << 0,  // F1: add the Base* colours below
		// (47a) Sky Fixes. Read by LutGen.cs.hlsl / Sky.hlsl through local constants (SharedData.hlsli,
		// which every Lighting shader includes, is untouched).
		kNightMidpoint = 1u << 1,   // LUT ray-march samples in the middle of each step (all four passes)
		kNightGroundFix = 1u << 2,  // multiscatter ground bounce x cos / pi, sun tested at the ground point
		kNightSkyFixes = 1u << 3,   // Sky.hlsl: dither, sun halo smoothing, glare ridge test, star fade
		// (47b) Sky Fixes additions.
		kNightDistantSea = 1u << 4,  // LutGen.cs.hlsl: below the planet horizon the sky view sees a calm sea (no dark band)
		kNightWaterAp = 1u << 5,     // Water.hlsl: the water surface gets the aerial perspective
		// (48b) Sky.hlsl only (MoonSky), independent of Sky Fixes.
		kNightMoonFadesStars = 1u << 6,  // "Moonlight Fades Stars": MoonCbData::starFadeHalves
		kNightPlanetshine = 1u << 7,     // "Moon Planetshine": MoonCbData::planetshineHalves
		// (49h) Water.hlsl only: its sun highlight x the transmittance toward the light ("Sun Highlight on Water Follows the Atmosphere").
		kNightWaterSunTr = 1u << 8,
	};
	/// (49h stage 1) Physical Lighting rule bits in SharedData::horizonFixData.Flags (HorizonFixCbData::flags, unused since 47m).
	/// Mirrored by Common/PhysicalLighting.hlsli.
	enum PhysLightFlags : uint32_t
	{
		kPLDirect = 1u << 0,
		kPLMoonGrey = 1u << 1,
		kPLCloudAmbient = 1u << 2,
		kPLIblNoFeedback = 1u << 3,
		kPLEnvOcclusion = 1u << 4,
		kPLFarSkylight = 1u << 5,
		kPLLodScale = 1u << 6,
		kPLFarFog = 1u << 7,
		kPLForwardCubemap = 1u << 8,
		kPLSmallFixes = 1u << 9,
		kPLLandPbr = 1u << 10,  // information: the loaded land uses True PBR texture sets
		// (49h stage 2) information bits, in effect this frame (Sky.hlsl / Water.hlsl read kPLFollow through local constants).
		kPLRealRatios = 1u << 11,
		kPLPhysAmbient = 1u << 12,
		kPLMeasuredClouds = 1u << 13,
		kPLFollow = 1u << 14,
		kPLRainNoColour = 1u << 15,  // (49j) Particle.hlsl: precipitation without the texture's colour (independent of the master)
	};
	struct NightCbData
	{
		float3 lutSunDir = { 0.f, 0.f, 1.f };  // F4a/F4b: sun direction the LUTs use
		uint flags = 0;
		float3 baseUpper = {};    // F1: linear, x strength x fade (zenith)
		float skyScale = 1.f;     // (47a, HLSL pad0) sky brightness factor on the sun/moon in-scatter (1 = none)
		float3 baseHorizon = {};  // horizon; also the aerial perspective's even glow
		float starFadeStrength = 0.f;  // (47a, HLSL pad1) star fade T (0 = off)
		float3 baseLower = {};    // below the horizon
		float nightBaseUnfade = 0.f;   // (47a, HLSL pad2) 1 / the night base fade (0 = faded out or off)
	} nightCbData;
	static_assert(sizeof(NightCbData) == 64);
	/// (41b, F1) Fills nightCbData's base light from the current weather; a_sunElevation in radians.
	void UpdateNightBaseLight(float a_sunElevation);
	/// (49h stage 2) A moon's light at full phase: the user's colour, or under Real Sky Ratios its hue at the real ratio to the sun.
	[[nodiscard]] float3 MoonLightBase(int a_moon) const { return physScene.active ? physScene.moonReal[a_moon] : (a_moon == 0 ? settings.masserColor : settings.secundaColor); }
	/// (49h stage 2) The weather's night sky colours (SkyUpper, Horizon, SkyLower; blended through a transition) with the airglow floor,
	/// linearised as the sky colours are; before any strength or fade.
	void NightWeatherColors(float3& a_upper, float3& a_horizon, float3& a_lower) const;
	/// (47a) The night base light at full strength (fade 1) and its fade this frame, for Sky Fixes.
	float3 nightBaseUnfadedUpper = {};
	float3 nightBaseUnfadedHorizon = {};
	float nightBaseFade = 0.f;

	/// (47a) Sky Fixes constants.
	static constexpr float kSkyFixesDayElevationDeg = 12.f;  // = SkySync::SunWarp::DayElevation
	static constexpr float kSkyFixesScaleCap = 32.f;  // largest cap whose sun-side horizon still brightens monotonically through NAT's dawn (v5 cap study)
	static constexpr float kSkyFixesRingElevationDeg = 85.f;
	static constexpr float kSkyFixesSmoothingSeconds = 1.5f;
	static constexpr float kSkyFixesStarFade = 0.0035f;
	static constexpr float kOzoneFixAltitude = 25.f;   // km: layer 10-40 km
	static constexpr float kOzoneFixThickness = 30.f;  // km
	/// (47a) Sky brightness follows the ground: sets nightCbData.skyScale (smoothed) and the Sky Fixes
	/// flags and star values. a_lutSunDir is the LUTs' sun (world frame).
	void UpdateSkyFixes(bool a_fixes, const RE::NiPoint3& a_lutSunDir);
	bool UpdateCpuSkyModel();  // (47g) skyModelCpu with the atmosphere as uploaded
	/// (47a) g: the weather's blended ground ambient (DALC) between its night and day keys, 0-1; false
	/// when there is no usable difference (|day - night| < 10%) or no weather.
	static bool GetAmbientProgress(float& a_g);
	PhysicalSkyModel::Model skyModelCpu;
	float skyScaleSmoothed = -1.f;  // < 0: snap next time
	float skyScaleLastHours = -1.f;
	uint32_t skyScaleLastWarp = 0;
	struct SkyFixesDebug
	{
		bool valid = false;
		float g = 0.f, zScatter = 0.f, zMoon = 0.f, zBase = 0.f, zNight = 0.f, zDay = 0.f, target = 1.f;
	} skyFixesDebug;

	/// (47a) Aerial perspective LUT direction size: 32x32, or 64x64 with Sky Fixes (depth stays 32).
	static constexpr uint16_t kApLutFixW = 64;
	static constexpr uint16_t kApLutFixH = 64;
	uint16_t apLutW = kApLutW;
	uint16_t apLutH = kApLutH;
	bool CreateApLut(uint16_t a_w, uint16_t a_h);
	void UpdateApLutSize();

	/// (47a) Sky Fixes: scene depth bound for the vanilla sun glare draw (PS t65), for its ridge test.
	static constexpr UINT kGlareDepthSlot = 65;
	ID3D11ShaderResourceView* glarePreviousSrv = nullptr;
	bool glareDepthBound = false;
	void BindGlareDepth(const RE::BSRenderPass* a_pass);
	void UnbindGlareDepth();
	/// (41b, F2) Glow visibility of a moon by its true elevation; 0 when the weather hides it.
	static float MoonHorizonVisibility(const RE::Moon* a_moon, const RE::Sky* a_sky, float a_sinElevation);

	/// (42b) Mirrors SharedData::PhysSkyCloudData (HLSL), appended after PhysSkyNightData. Read only
	/// by Sky.hlsl.
	enum CloudFlags : uint32_t
	{
		kCloudMultiLight = 1u << 0,        // "Cloud Lighting: Sun + Moons + Sky"
		kCloudMoonDaylightFade = 1u << 1,  // "Moons Fade in Daylight" (fixed on since 44h)
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

	/// (47f) Mirrors SharedData::HorizonFixData (HLSL), appended after WeatherFogData at the very end of FeatureData.
	/// (47m) Horizon Fix is removed; (47o) its shader side too: anchorOffset, altitude and flags are padding (0, not read); only
	/// fogSkyLight is live (WeatherFog.hlsli). The layout must not change (SharedData).
	struct HorizonFixCbData
	{
		float anchorOffset = 0.f;  // (47m) always 0 (was: camera height over the weather fog layer's base)
		float altitude = 0.f;      // (47m) always 0 (was: camera height over the ground sphere)
		uint flags = 0;            // (47m) always 0 (was: bit 0 Horizon Fix in effect)
		float fogSkyLight = 0.f;   // (47g) EHF Physical Fog Brightness: the clear sky's light on a white surface facing up (0 = off)
	} horizonFixCbData;
	/// (47g) The clear physical sky's light on a white surface facing up (its cosine-weighted mean radiance, as drawn:
	/// Sky Fixes' brightness factor, the moons' glow, the night base light), from the CPU model; 0 when it cannot be had.
	float ComputeFogSkyLight(const RE::NiPoint3& a_lutSunDir);
	/// (49h stage 1) The Physical Lighting rule bits in effect (0 with the master off).
	[[nodiscard]] uint32_t PhysLightingFlags() const;
	/// (49h stage 1) A Physical Lighting rule in effect (master and its checkbox) -- for the C++ side of S1 / S4a.
	[[nodiscard]] bool PhysLightingOn(bool a_rule) const { return loaded && settings.physicalLighting.enabled && a_rule; }
	void SettingsSceneLighting();

	////////////////////////////////////////////////// (49h stage 2) Physical outdoor lighting
	/// Real ratios of the sources, before the compression: the real full moon's irradiance relative to the sun.
	static constexpr float kMoonToSun = 2.5e-6f;
	/// Overcast sky's light on a vertical surface / on a horizontal one (CIE overcast sky).
	static constexpr float kOvercastVerticalRatio = 0.396f;
	/// Number of upper-hemisphere directions of the CPU sky integration.
	static constexpr int kSceneDirections = 60;
	/// (49i) Normals the physical ambient is evaluated and fitted at: the 6 axes (engine order), 12 edge and 8 corner diagonals.
	static constexpr int kAmbientNormals = 26;
	/// Real brightness of a moon phase picture relative to full (the picture factor 0.05 new .. 0.25 crescent .. 1 full).
	static float RealMoonPhase(float a_pictureFactor);
	/// The physical scene of this frame (Reset). All neutral while inactive.
	struct PhysScene
	{
		bool active = false;          // Real Sky Ratios in effect (exterior, physical sky drawn, master + T1)
		bool ambientActive = false;   // Physical Ambient in effect (T2, outdoors)
		bool followActive = false;    // T4 in effect
		float S = 1.f;                // the compression in effect (smoothed)
		float sTarget = 1.f;
		float sLog = 0.f;             // smoothed log(S)
		bool sValid = false;
		float eReal = 0.f;            // horizontal illuminance, sun units (luminance), clouds applied
		float eRef = 0.f;             // the same for a clear sky with the sun at 30 degrees
		float cover = 0.f;            // cloud cover 0-1
		float coverTr[3] = { 1.f, 1.f, 1.f };  // cloud transmission toward the sun, Masser, Secunda
		bool coverMeasured = false;   // from the cube (else the weather estimate)
		float3 moonReal[2] = {};      // full-phase moon irradiance at the real ratio (sun units), the moon colour's hue
		float realPhase[2] = { 1.f, 1.f };
		float3 ambient[kAmbientNormals] = {};  // physical ambient per fit normal ((49i) the first six: +x -x +y -y +z -z), linear, shader units (smoothed)
		bool ambientValid = false;
		float rAmb = 1.f;             // physical ambient / the engine's (luminance)
		float starScale = 1.f;        // physical night base / the stage-1 night base (luminance)
		float unlitScale = 1.f;       // physical ambient now / at the clear-sky anchor, for unlit effects
		float airglowZenith = 0.f;    // sun units, unscaled
		uint32_t heavyFrame = UINT32_MAX;
		float lastHours = -1.f;
		uint32_t lastWarp = 0;
		// Anchor cache (clear sky, sun at 30 degrees): E_ref, the full-moon sky luminance, the anchor's ambient mean.
		uint32_t anchorFrame = UINT32_MAX;
		float anchorSunLum = -1.f;
		float anchorFullMoonSky = 0.f;
		float anchorAmbientMean = 0.f;
		float3 targetAmbient[kAmbientNormals] = {};
		// (49i) Readouts of the Scene Lighting tab: the sky (clouds applied) and the direct sun / moons on a horizontal face (luminance, sun
		// units, unscaled), the ground bounce's albedo and snow share, and the ambient fit's largest error at the 26 normals (49h's
		// construction / what is applied).
		float skyUpLum = 0.f;
		float directLum = 0.f;
		float3 bounceAlbedo = {};
		float bounceSnow = 0.f;
		float fitErrorOld = 0.f;
		float fitErrorNew = 0.f;
		bool fitApplied = false;
	} physScene;
	/// (49i) Ground Bounce's albedo: Physical Sky's Ground Albedo, toward kSnowAlbedo by Dynamic Snow's accumulated snow at the player
	/// (o_snow: that share, 0 without Dynamic Snow / outside its cold area).
	float3 GroundBounceAlbedo(float& o_snow) const;
	static constexpr float kSnowAlbedo = 0.7f;
	/// Fills physScene (S, the ambient, the ratios). a_*Dir: the LUTs' sun and the moons' glow directions (world).
	void UpdatePhysicalScene(const RE::NiPoint3& a_lutSunDir, const RE::NiPoint3& a_masserDir, const RE::NiPoint3& a_secundaDir, float a_cameraRadiusKm);
	/// T2: writes the physical ambient into the engine's directional ambient (State::UpdateSharedData, before it is copied), or puts the
	/// engine's own value back once after it is switched off.
	void ApplyPhysicalAmbient();
	/// T2 in effect this frame (the IBL upload overrides: DALCAmount 1, SkyIBLScale 0 ((49i) EnvIBLScale no longer); the fog's sky light from the ambient).
	[[nodiscard]] bool PhysicalAmbientActive() const { return physScene.ambientActive; }
	/// T1 in effect this frame (Sky Sync: no sun residual below the horizon, the moons at the real ratio and phase, no day fade).
	[[nodiscard]] bool RealSkyRatiosActive() const { return physScene.active; }
	/// T2: the unlit effects' scene scale (physical ambient now / clear-sky anchor); false while inactive.
	bool GetPhysicalUnlitScale(float& a_scale) const;
	RE::NiTransform dalcPhysical = {};
	RE::NiTransform dalcLastWritten = {};
	RE::NiTransform dalcEngine = {};
	bool dalcWritten = false;
	bool dalcEngineValid = false;
	/// T3: Cloud Shadows' cube reduced to 4 numbers (cover, and the cloud in front of the sun / Masser / Secunda) and read back late.
	struct CloudCoverCB
	{
		float4 sunDir;
		float4 masserDir;
		float4 secundaDir;
	};
	std::unique_ptr<ConstantBuffer> cloudCoverCB;
	std::unique_ptr<StructuredBuffer> cloudCoverSB;
	winrt::com_ptr<ID3D11ComputeShader> csCloudCover = nullptr;
	static constexpr int kCloudReadbackCount = 3;
	winrt::com_ptr<ID3D11Buffer> cloudReadback[kCloudReadbackCount];
	bool cloudReadbackPending[kCloudReadbackCount] = {};
	uint32_t cloudReadbackIndex = 0;
	float cloudMeasured[4] = { 0.f, 0.f, 0.f, 0.f };
	uint32_t cloudMeasuredFrame = UINT32_MAX;
	void DispatchCloudCover();
	/// The weather's cloud cover estimate (flags x transition) for when the cube is not available.
	static float WeatherCloudCover();
	/// (49h) EHF "Stable Fog Colour": Physical Sky's transmittance from the camera toward a_dir (towards the light) as
	/// PhysSky::SampleTr gives the shaders (Transmittance Mix applied), from the CPU model; 1 while Physical Sky is off or the
	/// model cannot be had.
	float3 ComputeLightTransmittance(const float3& a_dir);
	uint32_t fogSkyLightFrame = UINT32_MAX;
	float fogSkyLightCached = 0.f;
	static_assert(sizeof(HorizonFixCbData) == 16);

	/// (47f) Sky.hlsl's cloud vertex shader still declares VS b11 (HorizonFixVS, the removed Horizon Fix's cloud dome warp).
	/// (47m) Every cloud draw keeps getting a buffer of zeros there (exactly what it got with Horizon Fix off), so the
	/// shader never reads someone else's b11; the previous b11 is put back after the draw. Goes with the Lighting package.
	// (47o) The Horizon Fix cloud warp's VS b11 binding is gone (Sky.hlsl no longer declares it).

	/// (batch 37c) "Sun Look" presets.

	eastl::unique_ptr<Texture2D> texTrLut = nullptr;  // transmittance
	eastl::unique_ptr<Texture2D> texMsLut = nullptr;  // multiscattering
	eastl::unique_ptr<Texture2D> texSvLut = nullptr;  // sky view
	eastl::unique_ptr<Texture3D> texApLut = nullptr;  // aerial perspective
	eastl::unique_ptr<Texture2D> texApShadow = nullptr;

	winrt::com_ptr<ID3D11SamplerState> sampTr = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampSv = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampNoise = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csTrLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csMsLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSvLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csApLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccum = nullptr;
	// (49h stage 2) csCloudCover is declared with the scene data above.

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