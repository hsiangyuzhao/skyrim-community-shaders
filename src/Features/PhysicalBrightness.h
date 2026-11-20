#pragma once

#include "Features/LightLimitFix.h"

/// (49j) "Physical Brightness": one brightness model in four layers (scene / eye / screen / art), specs/design-exposure-model-2026-11.md.
///
/// Scene: every light in one physical unit system (surface luminance cd/m^2, illuminance lux), written to the buffers as physical x P, where
/// P (the pre-exposure) is one number per frame, the same for every light and every shader. P only keeps the values in the buffers' range:
/// the auto exposure divides it out again (histogram.cs.hlsl meters luminance / P), so P late or wobbling leaves the picture unchanged.
/// Eye: the auto exposure adapts to the physical luminance and shows dark scenes darker by the Night Darkness curve (design 2.2.2).
/// Screen: the tone curve (hue-preserving Lottes, design 6.2). Art: a few multipliers (design 7).
///
/// An independent module (own settings, saved under the Physical Sky JSON key "physicalBrightness"; own state); its page is drawn in
/// F11 > Sky > Physical Sky > Scene Lighting. Master off = brightness as before 49h (no S, vanilla light units, Classic auto exposure).
struct PhysicalBrightness
{
	static PhysicalBrightness* GetSingleton()
	{
		static PhysicalBrightness singleton;
		return &singleton;
	}

	enum LampFalloff : int
	{
		kFalloffVanilla = 0,        // the game's 1 - (d / r)^2, calibrated at d = r / 2
		kFalloffInverseSquare = 1,  // Inverse Square Lighting's 1 / d^2 for every lamp (needs Inverse Square Lighting)
	};
	enum AdaptAfterLoad : int
	{
		kAdaptInstant = 0,  // the eye is adapted when the loading screen ends (as before)
		kAdaptKeep = 1,     // the eye starts from the last scene's adaptation and adapts (a cave is dark for a few seconds)
	};

	struct Settings
	{
		bool enabled = false;
		// ---- Light Sources (scene layer) ----
		int lampFalloff = kFalloffVanilla;
		float lampStrength = 1.f;        // point lights, relative to the calibration
		float emissiveStrength = 1.f;    // emissive / glow maps (windows, wicks)
		float fireMagicStrength = 1.f;   // fire and magic effects
		float skyAmbientStrength = 1.f;  // the physical sky's ambient outdoors
		float envBounceStrength = 1.f;   // Image Based Lighting's bounce (Env IBL Scale) outdoors
		float interiorAmbient = 1.f;     // the game's interior ambient, x its calibration
		float weatherTint = 1.f;         // relative weather tint of the ambient, clouds and fog (design 7)
		float weatherBrightness = 0.5f;  // 0 physical cloud amount only, 1 the weather's brightness intent relative to physical
		bool groundBounce = true;        // the ground's bounce light from below (49i)
		bool probeForget = true;         // the IBL probe forgets by the camera's displacement (49j 9-1)
		// ---- Eye ----
		float nightDarkness = 0.6f;          // t: 0 = the weather's own day/night ratio, 1 = the physiological curve
		float minVisibility = 0.3f;          // f: no area darker than f x what the game shows there now (lamps excluded)
		float exposureCompensation = 0.f;    // EV
		float localAdaptation = 0.5f;        // Local Exposure's strength while the master is on
		float nightVision = 0.5f;            // per-pixel mesopic desaturation and blue shift
		int adaptAfterLoad = kAdaptInstant;
		// ---- Screen ----
		bool huePreserving = true;  // Lottes (Hue Preserving) in the colour grading LUT while the master is on
		// ---- Developer (calibration and constants; design 4, 2.2.2) ----
		float sunLux = 1.28e5f;           // the sun above the atmosphere, lux (the sky model applies the air's transmittance)
		float calLamp = 8.f;              // lux per unit of the game's lamp value c^1.8 x fade x (1 - (d/r)^2)
		float calEmissive = 10.f;         // cd/m^2 per unit of EmitColor^2.2 x glow map (x albedo, as Lighting.hlsl adds it)
		float calFire = 200.f;            // cd/m^2 per unit of a fire / magic effect's colour
		float calMoonDisc = 2500.f;       // the full moon disc's mean luminance, cd/m^2
		float physSlope = 0.3f;           // s: on-screen EV per real EV below full adaptation (Stevens' 1/3 power law)
		float fullAdaptation = 64.f;      // L_full, cd/m^2: brighter scenes are shown at the day key
		float brightBoost = 0.15f;        // beta: bright scenes a little brighter on screen (Stevens effect)
		float brightAbove = 2000.f;       // L_bright, cd/m^2
		float winsorEV = 3.f;             // the auto exposure counts samples brighter than median + this as median + this
		float darkTrim = 5.f;             // percent of the darkest samples not counted
		float bloomClamp = 64.f;          // COD Bloom's input clamp (after exposure) while the master is on
		bool measuredClouds = true;       // cloud cover from Cloud Shadows' cube (49h T3)
		bool ambientFit = false;          // "Accurate Ambient Directions" (49i)
		float overcastKappa = 0.25f;      // light an overcast layer passes on
		float airglowRatio = 25.f;        // full-moon sky / airglow
		float smoothingSeconds = 1.5f;    // the physical ambient follows the sky with this time constant
		bool pWobble = false;             // P x 2^sin: every path that has P stays still; one that lacks it pulses
	} settings;

	/// Where the frame is: decides what P scales and how the eye's reference is found.
	enum class Scene : int
	{
		None = 0,         // master off, Linear Lighting off, menus, loading: the model is not in effect
		OutdoorPhysical,  // a Physical Sky worldspace outdoors
		OutdoorVanilla,   // outdoors without the physical sky (not whitelisted, Physical Sky off)
		Interior,         // an interior
	};

	// ---- Per frame (BeginFrame) ----
	Scene scene = Scene::None;
	float P = 1.f;              // the pre-exposure in effect this frame (1 while not in effect)
	float pBase = 1.f;          // before the P Wobble Test
	float adaptedLuminance = 0.f;  // L_a, cd/m^2 (auto exposure readback, or the CPU estimate)
	bool adaptedFromAE = false;    // L_a came from the auto exposure
	float estimatedLuminance = 0.f;  // CPU estimate of the adapted luminance (fallback)
	float deltaK = 0.f;              // display key offset, EV (CPU copy of the auto exposure's)
	float deltaKVanilla = 0.f;       // the vanilla reference's night offset, EV (step 2)
	float vanillaUnlit = 1.f;        // pipeline factor for the game's unlit colours (fog, rain, water, unlit smoke): the ambient now / the game's ambient
	float timeSeconds = 0.f;
	Scene lastScene = Scene::None;
	uint32_t sceneChangeFrame = 0;

	/// State::Reset, before every feature's Reset: decides the scene and P for the whole frame.
	void BeginFrame();
	/// The model is in effect this frame.
	[[nodiscard]] bool Active() const { return scene != Scene::None; }
	[[nodiscard]] bool OutdoorPhysical() const { return scene == Scene::OutdoorPhysical; }
	/// The master is on (the model may not be in effect here, e.g. in a menu).
	[[nodiscard]] bool MasterOn() const;
	/// Pre-exposure.
	[[nodiscard]] float GetP() const { return Active() ? P : 1.f; }
	/// Linear Lighting's pointLightMult while in effect: lamp strength x calLamp x P / (pi x Vanilla Diffuse Mult) (Lighting multiplies the
	/// lamp by Vanilla Diffuse Mult again: the diffuse result is albedo x E / pi x P).
	[[nodiscard]] float LampMult() const;
	/// Linear Lighting's emitColorMult / glowmapMult while in effect: strength x calEmissive x min(P, P_cap).
	[[nodiscard]] float EmissiveMult() const;
	/// Effect.hlsl's self-lit scale of fire and magic while in effect: calFire x min(P, P_cap) (the strength is the effect multiplier).
	[[nodiscard]] float FireSelfLitScale() const;
	/// The moon disc's scale on the game's disc colour while in effect (Sky.hlsl x MasserDiskScale): calMoonDisc / 0.26 x min(P, P_cap).
	[[nodiscard]] float MoonDiscScale() const;
	/// Sun units of Physical Sky -> pipeline: P x sunLux / luminance(Physical Sky's sun colour).
	[[nodiscard]] float SunUnitsToPipeline(float a_sunLuminance) const;
	/// P_cap of an emitter class whose brightest value is a_peak cd/m^2: P never takes it past 2^14 in the buffer (R11G11B10 / fp16).
	[[nodiscard]] float CappedP(float a_peak) const;

	/// Linear Lighting's Vanilla Diffuse Mult as the shaders apply it (1 without Linear Lighting).
	[[nodiscard]] static float VanillaDiffuseMult();

	/// LightLimitFix: a lamp's LLF data while in effect (Linear-flagged lamps skip pointLightMult: their fade takes it; the inverse-square
	/// falloff for every lamp). a_isl: Inverse Square Lighting is loaded.
	void AdjustLight(LightLimitFix::LightData& a_light, bool a_isl, bool a_pointLight) const;

	/// Local Exposure runs as the eye's local adaptation (Physical Brightness: its Local Adaptation Strength > 0; else Local Exposure's own switch).
	[[nodiscard]] bool LocalAdaptationRuns() const;
	/// Minimum Visibility applies to the whole picture (the auto exposure's floor) because Local Exposure does not run.
	[[nodiscard]] bool MinVisibilityIsGlobal() const { return settings.minVisibility > 1e-4f && !LocalAdaptationRuns(); }
	/// Reads the auto exposure's adapted luminance (Physical Brightness mode readback).
	bool ReadAdaptedLuminance(float& o_luminance) const;

	// ---- Settings / UI ----
	void Load(const json& a_json);
	void Save(json& a_json) const;
	void DrawSettings();
	void DrawStatus();
	std::string DescribeForTrace() const;
};
