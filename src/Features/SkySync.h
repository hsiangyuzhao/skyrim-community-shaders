#pragma once
#include "RE/M/Moon.h"

struct PhysicalSky;

struct SkySync : Feature
{
private:
	static constexpr std::string_view MOD_ID = "153543";

public:
	friend struct PhysicalSky;

	virtual inline std::string GetName() override { return "Sky Sync"; }
	virtual inline std::string GetShortName() override { return "SkySync"; }
	virtual std::string_view GetCategory() const override { return "Sky"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Synchronizes volumetric lighting and shadows with the actual sun and moon positions in the sky.",
			{ "Fixes the mismatch between the positions of the sun and moons and the lighting direction",
				"Includes a configurable alternative sun path for more realistic and dramatic lighting",
				"Smoothly switches the light source between the sun and moons based on visibility",
				"Moon light source can be switched between Masser, Secunda, or the brightest",
				"Automatic calculation of moon lighting intensity based on moon phase",
				"Fixes the sun appearing higher on the horizon when the player gains altitude" }
		};
	}

	struct Settings
	{
		bool Enabled = true;
		bool UseAlternateSunPath = true;
		int32_t MoonLightSource = 0;
		int32_t SunPath = 0;
		float CustomAngle = -35.0f;
		// (44h) The fields below are no longer in the menu or the JSON: Effective() pins MoonOrbit =
		// Stable, KeepMoonPosition = on, LightSourceModel = Combined, BothMoonsLight = on (Legacy (36f)
		// still overrides). Kept for a possible rollback (specs/physsky-rollback-candidates.md).
		/// (40e) Where the moons are placed (MoonOrbit values):
		/// 0 Vanilla: the engine steps each orbit angle forward every frame (a backwards time change
		///   counts as +24 h; Secunda, 20% faster than Masser, lands somewhere new on every skip).
		/// 1 Stable (40d fix C): the angle follows whole days + the clock at each moon's own speed.
		///   Masser is overhead at midnight; Secunda drifts 72 degrees a day (a 5-day cycle in
		///   which it is up at night on about two nights).
		/// 2 Night Sky (default): both moons are tied to the clock and stay up all night: they are
		///   up at dusk, cross the sky slowly and set after dawn, Secunda a little ahead of Masser, a
		///   fixed distance apart. Each day both pass below the horizon, so phases still change.
		int32_t MoonOrbit = 2;
		/// (40e) Night Sky: each moon's place on its orbit at the middle of the night, in degrees
		/// from its highest point (negative = earlier, towards the side it rises on).
		float MasserNightPosition = -15.0f;
		float SecundaNightPosition = 15.0f;
		/// (40e) Night Sky: degrees each moon moves along its orbit from dusk to dawn.
		float NightArc = 80.0f;
		/// (40d) Leave the moons where the game puts them: no altitude dip written into their
		/// rotation (upstream 54962ef3b). Physical Sky then reads each moon's on-screen
		/// direction (upstream b2d671ba8's idea), so its glow sits on the disc.
		bool KeepMoonPosition = true;
		/// (42b) LightSourceModel: what the one directional (shadow-casting) light is.
		/// 0 Clock (42a and earlier): the sun from 6:30 to 19:30 by the climate's clock, a moon outside
		///   it with clock fades, so there was a stretch at dawn and dusk with no light at all.
		/// 1 Combined (default): sun + moons added, each by its true height above the horizon; the
		///   direction follows the brightest body and hands over smoothly (spec-skysync D1-D6).
		int32_t LightSourceModel = 1;
		/// (42b) Combined: both moons add their light (Secunda at 0.67, by phase). Off: only the moon
		/// picked by "Moon light source" (Brightest = a smooth blend that is as bright as the brighter).
		bool BothMoonsLight = false;
	};

	Settings settings;

	/// (41b, F5) Settings as used this frame: in Physical Sky's "Legacy (36f)" mode the moon options
	/// added after 36f are at their 36f values (Vanilla orbit, discs lowered by altitude). The saved
	/// settings are never changed.
	Settings Effective() const;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; }

	virtual void PostPostLoad() override;
	virtual void DataLoaded() override;

	struct Sky_Update
	{
		static void thunk(RE::Sky* sky);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Sky_OnNewClimate
	{
		static void thunk(RE::Sky* sky);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Moon_Update
	{
		static void thunk(RE::Moon* moon, RE::Sky* sky);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct VolumetricLightingDescriptor
	{
		float lightingIntensity;
	};

	struct ApplyVolumetricLighting_VolumetricLightingDescriptor_Get
	{
		static VolumetricLightingDescriptor* thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

private:
	enum class MoonLightSource : uint8_t
	{
		Brightest,
		Masser,
		Secunda,
		Count
	};

	enum class Caster : uint8_t
	{
		Sun,
		Masser,
		Secunda,
		None
	};

	enum class MoonOrbit : int32_t
	{
		Vanilla,
		Stable,
		NightSky,
		Count
	};

	enum class LightSourceModel : int32_t
	{
		Clock,
		Combined,
		Count
	};

	enum class SunPath : uint8_t
	{
		Southern,
		Northern,
		Vanilla,
		Custom,
		Count
	};

	const char* MoonLightSourceNames[static_cast<uint8_t>(MoonLightSource::Count)] = { "Brightest", "Masser", "Secunda" };
	const char* SunPathNames[static_cast<uint8_t>(SunPath::Count)] = { "Southern Sky", "Northern Sky", "Vanilla", "Custom" };
	const char* MoonOrbitNames[static_cast<int32_t>(MoonOrbit::Count)] = { "Vanilla", "Stable", "Night Sky" };
	const char* LightSourceModelNames[static_cast<int32_t>(LightSourceModel::Count)] = { "Clock (old)", "Combined" };

	struct ClimateTimings
	{
		float sunriseFadeOutMoonStart;
		float sunriseBegin;
		float sunriseFadeOutMoonEnd;
		float sunrise;
		float sunriseEnd;
		float sunsetBegin;
		float sunset;
		float sunsetFadeInMoonStart;
		float sunsetEnd;
		float sunsetFadeInMoonEnd;

		void Update(const RE::TESClimate* climate);
	};

	struct ShadowFader
	{
		enum class Phase : uint8_t
		{
			None,
			FadeOut,
			FadeIn
		};

		static constexpr float FadeTime = 100.0f;  // 5 seconds at timescale 20

		Phase fadePhase = Phase::None;
		Caster current = Caster::None;
		Caster target = Caster::None;
		float fadeTimer = 0.0f;
		float previousHoursPassed = 0.0f;

		void Update(const RE::Sun* sun, RE::NiPoint3 dirs[], float intensities[], std::optional<std::array<RE::NiColor, 3>> colors, bool isDayTime);
		static void SetLighting(const RE::Sun* sun, RE::NiPoint3 dir, float intensity, std::optional<RE::NiColor> color);
		static void ClampDirection(RE::NiPoint3& dir);
		void Reset();
	};

	/// (42b) Direction hand-over of the Combined light model (spec-skysync D2): the light's direction
	/// follows `dominant`. A new dominant body close in the sky (TurnMaxAngle) is turned to over
	/// FadeTime at full brightness; one far away is reached by fading the light to 0 on the old
	/// direction, switching while it is 0, and fading back in (FadeTime in all), so the direction
	/// never sweeps across the sky under visible light.
	struct CombinedHandover
	{
		enum class Phase : uint8_t
		{
			Steady,
			Turn,
			FadeOut,
			FadeIn
		};

		static constexpr float FadeTime = 100.0f;      // game seconds: 5 real seconds at timescale 20, as ShadowFader
		static constexpr float TurnMaxAngle = 25.0f;   // degrees: closer than this, turn instead of fading
		static constexpr float Hysteresis = 1.25f;     // a new body must be this much brighter to take over

		Phase phase = Phase::Steady;
		Caster dominant = Caster::None;  // body whose direction the light uses now
		Caster target = Caster::None;    // body it is heading to
		float weight = 1.0f;             // hand-over brightness (1 outside a fade)
		float turnTimer = 0.0f;
		RE::NiPoint3 turnFrom = { 0.0f, 0.0f, 1.0f };
		RE::NiPoint3 lastDir = { 0.0f, 0.0f, 1.0f };
		float previousHoursPassed = -1.0f;

		/// Advances by a_advance game seconds towards a_best; a_snap jumps straight there (time skip,
		/// load). Returns the direction to use this frame.
		RE::NiPoint3 Step(Caster a_best, const RE::NiPoint3 a_dirs[3], float a_advance, bool a_snap);
		void Reset();
	};

	// (42b) Combined model constants (degrees of true elevation).
	static constexpr float SunFullElevation = 3.0f;         // sun at full strength from here up
	static constexpr float SunResidual = 0.1f;              // sun strength left at the horizon...
	static constexpr float SunResidualEndElevation = -4.0f;  // ...fading to 0 here (x Physical Sky's Twilight Length)
	static constexpr float MoonStartElevation = -1.0f;      // moon light 0 here...
	static constexpr float MoonFullElevation = 3.0f;        // ...full here
	static constexpr float MoonDayFadeStart = -2.0f;        // moons fade out as the sun climbs from here...
	static constexpr float MoonDayFadeEnd = 6.0f;           // ...to here
	/// Warm tint of the twilight sun while the light points at a moon: the shaders take the air's
	/// colouring along the light's direction, which is then the moon's, not the low sun's.
	static constexpr float SunTwilightTint[3] = { 1.0f, 0.5f, 0.25f };

	static constexpr float RenderDistance = 325000.0f;
	static constexpr float SunHorizonDistance = 280.0f;
	static constexpr float SunPeakDistance = 400.0f;
	static constexpr float SunScaleFactor = 48.0f / 2048.0f;
	static constexpr float MinElevation = 0.25f;

	static constexpr float SouthernSunAngle = 90.0f - 35.0f;
	static constexpr float NorthernSunAngle = 90.0f + 35.0f;
	static constexpr float VanillaSunAngle = 90.0f + 5.0f;

	static constexpr float SecundaIntensityFactor = 0.67f;
	static constexpr float NewMoonIntensityFactor = 0.05f;
	static constexpr float CrescentMoonIntensityFactor = 0.25f;
	static constexpr float FullMoonIntensityFactor = 1.0f;

	inline static RE::NiPoint3* gSunPosition = nullptr;
	inline static float* gSunGlareSize = nullptr;
	inline static uint32_t* gMasserSize = nullptr;
	inline static uint32_t* gSecundaSize = nullptr;

	inline static float volumetricLightingIntensityFactor = 1.0f;
	/// (batch 37b, A3) The shadow fader's current light source is Masser or Secunda.
	inline static bool volumetricLightingIsMoon = false;

	bool moonAndStarsLoaded = false;
	RE::TESObjectCELL* currentCell = nullptr;
	float sunAngle = 90.0f;
	float currentSkyRotation = D3D11_FLOAT32_MAX;
	float masserPhaseIntensityFactor = 0.0f;
	float secundaPhaseIntensityFactor = 0.0f;

	ClimateTimings timings = {};

	RE::NiPoint3 rawDirections[3];
	RE::NiPoint3 directions[3];
	float intensities[3] = {};
	std::optional<std::array<RE::NiColor, 3>> lightColors = {};  // assigned externally by Physical Sky
	ShadowFader shadowFader;
	CombinedHandover combinedHandover;
	int32_t lastLightSourceModel = -1;

	void DisableOnConflict(std::string_view conflictName);

	/// (42b) The Combined light model: sums sun + moons by true elevation and writes the light.
	void UpdateCombined(const RE::Sky* sky);
	/// (42b) Writes the light for the Combined model. With Physical Sky's colours the brightness goes
	/// into the colour and fade stays 1 (upstream 56784ccd1); without them (vanilla colour, which the
	/// game writes) fade is still the dimmer.
	static void SetLightingCombined(const RE::Sun* sun, RE::NiPoint3 dir, std::optional<RE::NiColor> color, float vanillaFade, float vlFactor);

	void Update(const RE::Sky* sky);

	void SetSunAngle();

	void SetSkyRotation(const RE::Sky* sky, RE::TESObjectCELL* cell);

	void ProcessSun(const RE::Sky* sky, const RE::Sun* sun, float time, float altitude, bool isDayTime);

	void ProcessMoon(const RE::Moon* moon, float time, Caster type, float altitude, bool isDayTime);

	static void CalculateSunDirectionAndDistance(const RE::Sun* sun, RE::NiPoint3& outDir, float& outDistance);

	static void CalculateAlternateSunDirectionAndDistance(RE::NiPoint3& outDir, float& outDist, float time, float sunrise, float sunset, float sunAngle);

	static RE::NiPoint3 GetApparentDirection(const RE::NiPoint3& dir, float altitude);

	static void SetSunPosition(const RE::Sun* sun, const RE::NiPoint3& dir, float distance);

	static void SetMoonDirection(const RE::Moon* moon, const RE::NiPoint3& dir);

	static float CalculateVisibility(const RE::NiPoint3& dir, float dist, float radius);

	static void SetSunBaseVisibility(const RE::Sun* sun, float visibility);

	/// (40d, 40e) Writes the orbit angle (Moon+0xCC) for the Stable or Night Sky orbit and sets
	/// the last hour (+0xD0) to the current hour, so the engine's own update adds nothing.
	/// SE 1.5.97 only.
	static void SetMoonAngle(RE::Moon* moon, const RE::Sky* sky);

public:
	/// (40d) Brightness factor of the moon's current phase picture (new 0.05 .. full 1), read
	/// from the texture on its disc, as the phase factor above.
	static float PhaseFactorFromTexture(const RE::Moon* moon);
	/// (40d) The moon's vanilla visibility: 0 when hidden, else its disc's fade alpha.
	static float VanillaVisibility(const RE::Moon* moon);
	/// (40d) World-space direction from the sky's centre to the moon's disc; false if unknown.
	static bool OnScreenDirection(const RE::Moon* moon, const RE::Sky* sky, RE::NiPoint3& outDir);

	static float SmoothStep(float start, float end, float x);

	/// (42b) How much of the moons' light is left with the sun at this true elevation (degrees):
	/// 1 below MoonDayFadeStart, 0 above MoonDayFadeEnd. Used by the Combined light model and by
	/// Physical Sky's cloud lighting.
	/// (47a) With Physical Sky's "Sky Fixes" on: 1 below -8 degrees, falling linearly to 0 at -1
	/// (upstream's timing: 7 degrees below its -1 degree effective horizon).
	static float MoonDayFade(float sunElevationDeg);

	/// (47a) Physical Sky's "Sky Fixes" is on (Physical Sky loaded and enabled, not Legacy (36f)).
	static bool SkyFixesActive();

	/// (47a) Sky Fixes timing. The phase of the alternate sun path is warped through the climate's
	/// colour keys (monotone C1 PCHIP), so the physical sun sits at -12 degrees when the weather starts
	/// to leave its night colours, at 0 where its sunrise colours peak, at +12 where it is full day,
	/// at the top of the path at noon, and mirrored in the evening. Pure function of the hour and the
	/// climate timing; invalid (= the 46y formula) for all-zero, overlapping, inverted or crowded
	/// windows and for paths flatter than 10 degrees.
	struct SunWarp
	{
		static constexpr int KnotCount = 10;
		static constexpr double NightElevation = -12.0;  // degrees, at the start of the colour change
		static constexpr double DayElevation = 12.0;     // degrees, where the day colours are full
		bool valid = false;
		double x[KnotCount] = {};  // hours (the first and the last two wrap round the day)
		double y[KnotCount] = {};  // path phase, radians
		double m[KnotCount] = {};  // PCHIP slopes
		// What the knots were built from; Build() is a no-op while these match.
		bool built = false;
		uint8_t key[4] = {};
		float keyExtension = 0.f;
		float keySunAngle = 0.f;
		uint32_t generation = 0;  // bumped whenever the knots change (climate, sun path)

		void Build(const RE::TESClimate::Timing& a_timing, float a_colorExtension, float a_sunAngleDeg);
		double Phase(double a_hour) const;
	};
	SunWarp sunWarp;
	/// (47a) Bumped when the warp's knots change, so Physical Sky can snap its smoothing.
	uint32_t GetSunWarpGeneration() const { return sunWarp.generation; }
	/// (47a) GMST fDaytimeColorExtension (0.5 if missing).
	static float GetDaytimeColorExtension();

private:
	inline static RE::Setting* gDaytimeColorExtension = nullptr;
	/// (47a) Sky Fixes: alternate-path sun direction (sky-local) and quad distance at an hour, through
	/// the warp when it is valid, else the 46y formula. The one source of the sun's position.
	void GetSunDirectionAtHour(const RE::Sky* sky, float hour, RE::NiPoint3& outDir, float& outDist);
	/// (47a) Sky Fixes: the vanilla sun disc and glare follow the sun's true elevation instead of the
	/// clock: disc 0 at -3 degrees -> 1 at -1 (upstream), glare = the weathers' own glare (DATA sun
	/// glare / 255, blended like the game) x 0 at the horizon -> 1 at +1 degree.
	static void FadeSunAtHorizon(const RE::Sky* sky, float sunElevationDeg);
};