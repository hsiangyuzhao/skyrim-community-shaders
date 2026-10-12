#pragma once

#include "Feature.h"

/**
 * @brief (44a) "Wet Skin": everything about wet skin on characters, in one page (Characters).
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
	virtual inline std::string GetName() override { return "Wet Skin"; }
	virtual inline std::string GetShortName() override { return "WetSkin"; }
	virtual std::string_view GetCategory() const override { return "Characters"; }
	virtual inline bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Wet skin for characters: swimming, rain, sweat and a manual slider, with a soft water sheen, droplets and rivulets.",
			{ "Strong but soft sheen when soaked, never a mirror",
				"Droplets and rivulets that follow gravity and stay on the skin",
				"Wet patches that spread as the skin gets wetter",
				"Original (pre-42c) model for A/B comparison" }
		};
	}

	enum class Model : uint32_t
	{
		New = 0,
		Original = 1,
	};

	struct Settings
	{
		uint32_t Model = 0;  // 0 = New, 1 = Original (pre-42c)

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
		float StreakContrast = 0.5f;
	} settings;

	/// What the shader reads (Skin.hlsli wetSkinParams[6]).
	struct alignas(16) GPUParams
	{
		float4 p[6];
	};
	GPUParams GetGPUParams() const;

	bool IsOriginal() const { return settings.Model == static_cast<uint32_t>(Model::Original); }

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/// (44a) Migration from 43b: Advanced Skin and Wetness Effects hand over their raw JSON while
	/// they load; taken when "Wet Skin" has no settings of its own yet.
	static void StashLegacySkinJson(const json& a_skin);
	static void StashLegacyWetnessJson(const json& a_wetness);
};
