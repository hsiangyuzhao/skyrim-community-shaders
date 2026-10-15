#pragma once

#include "Buffer.h"

struct WetnessEffects : Feature
{
private:
	static constexpr std::string_view MOD_ID = "112739";

public:
	virtual inline std::string GetName() override { return "Wetness Effects"; }
	virtual inline std::string GetShortName() override { return "WetnessEffects"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "WETNESS_EFFECTS"; }
	virtual std::string_view GetCategory() const override { return "Water"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Adds realistic wetness effects including rain-based surface wetness, puddle formation, shore wetness, and dynamic raindrop effects for enhanced weather immersion.",
			{ "Dynamic surface wetness based on weather conditions",
				"Realistic puddle formation and shore wetness effects",
				"Animated raindrop effects with splashes and ripples",
				"Configurable wetness intensity and weather transitions",
				"Support for skin wetness and material-specific responses" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	struct Settings
	{
		uint EnableWetnessEffects = true;
		float MaxRainWetness = 1.0f;
		float MaxPuddleWetness = 2.5f;
		float MaxShoreWetness = 1.0f;
		uint ShoreRange = 32;
		float PuddleRadius = 1.0f;
		float PuddleMaxAngle = 0.95f;
		float PuddleMinWetness = 0.85f;
		float MinRainWetness = 0.65f;
		float SkinWetness = 0.95f;
		float WeatherTransitionSpeed = 3.0f;

		// Raindrop fx settings
		uint EnableRaindropFx = true;
		uint EnableSplashes = true;
		uint EnableRipples = true;
		uint EnableVanillaRipples = false;
		float RaindropFxRange = 1000.f;
		float RaindropGridSize = 4.f;
		float RaindropInterval = 1.0f;
		float RaindropChance = 1.0f;
		float SplashesLifetime = 10.0f;
		float SplashesStrength = 1.05f;
		float SplashesMinRadius = .3f;
		float SplashesMaxRadius = .5f;
		float RippleStrength = 1.f;
		float RippleRadius = 1.f;
		float RippleBreadth = .5f;
		float RippleLifetime = .5f;

		// (42c) Character wetness. Each switch applies to characters only: skin, hair,
		// and whatever an actor wears or carries (IsActorGeometry). World surfaces keep
		// the old model. With all three switches off the characters look as before
		// (apart from the always-on numeric fixes, 4.4).
		uint EnableWetFilm = true;       // 4.1 clear-coat water lobe on direct light
		float WetFilmRoughness = 0.2f;   // 4.1 fixed film roughness (was 1 - wetness)
		float WetFilmStrength = 1.0f;    // 4.1 film multiplier, 1 = physical
		uint EnableWetDarkening = true;  // 4.2 darkening before the skin takes its albedo, face included
		float PorositySkin = 0.6f;       // 4.2 how far each material darkens when soaked
		float PorosityCloth = 1.0f;      //     cloth / leather (non-metal gear)
		float PorosityMetal = 0.0f;      //     metal (metallic / env-mapped gear)
		float PorosityOther = 0.8f;      //     hair, fur
		uint EnableRainSkinFilm = true;  // 4.3 rain feeds the Advanced Skin wet film
	};

	struct alignas(16) PerFrame
	{
		REX::W32::XMFLOAT4X4 OcclusionViewProj;
		float Time;
		float Raining;
		float Wetness;
		float PuddleWetness;
		Settings settings;  // 36 x 4 bytes: the (42c) fields took the old pad0
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);

	struct DebugSettings
	{
		bool EnableWetnessOverride = false;
		bool EnablePuddleOverride = false;
		bool EnableRainOverride = false;
		bool EnableIntExOverride = false;
		float2 WetnessOverride = float2(0.0f, 0.0f);
		float2 PuddleWetnessOverride = float2(0.0f, 0.0f);
		float2 RainOverride = float2(0.0f, 0.0f);
	} debugSettings;

	// (46y) Raindrops: Fast Path (Raindrop Effects page). Outside Settings: Settings is copied into the FeatureData cbuffer
	// as is, so these travel as bits 1 / 2 of the GPU copy of EnableVanillaRipples (no shader reads its bit 0).
	// RaindropFastPath: the Lighting shader computes splashes only where they can raise the wetness and ripples only where
	// they have strength, and skips neighbour cells out of reach (identical result). RaindropSubPixelSkip: also fades the
	// drops out where a raindrop cell is smaller than a pixel. Both off = 46x.
	bool RaindropFastPath = true;
	bool RaindropSubPixelSkip = true;

	Settings settings;
	// Climate preset system
	enum class ClimatePreset : uint32_t
	{
		Custom = 0,
		Legacy = 1,
		NordicStandard = 2,
		ArcticTundra = 3,
		TemperateCoastal = 4,
		MonsoonExtreme = 5
	};
	struct ClimateSettings
	{
		float wetnessMultiplier;
		float puddleMultiplier;
		float transitionSpeed;
		float raindropChance;
		float raindropGridSize;
		float raindropInterval;
	};
	static constexpr ClimatePreset defaultPreset = ClimatePreset::NordicStandard;
	ClimatePreset climatePreset = defaultPreset;

	PerFrame GetCommonBufferData() const;

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };

	// Override to provide weather analysis configuration
	virtual WeatherAnalysisConfig GetWeatherAnalysisConfig() const override
	{
		return WeatherAnalysisConfig("Rain & Wetness Analysis", [this]() {
			this->DrawWeatherAnalysis();
		});
	}

	// Constants and utilities for rain intensity calculations
	static constexpr float MAX_RAIN_PARTICLE_DENSITY = 3.0f;

	// Helper function to extract rain intensity from precipitation object and weather
	static float GetRainIntensity(RE::NiPointer<RE::BSGeometry> precipObject, RE::TESWeather* weather);  // Helper function to calculate precipitation rate from shader data and settings
	float CalculatePrecipitationRate(float raindropChance, float raindropGridSizeGameUnits, float raindropIntervalSeconds, float mlPerDrop = 0.01f) const;
	static const ClimateSettings& GetClimateSettings(ClimatePreset preset);
	void ApplyClimatePreset(ClimatePreset preset);
	bool DoesCurrentSettingsMatchPreset(ClimatePreset preset) const;
	void DetectCurrentPreset();

private:
	void DrawWeatherAnalysis() const;

	bool splashesOfStormsLoaded = false;

	// Weather wetness calculation result for debug display
	struct WeatherWetnessResult
	{
		float wetness = 0.0f;
		float puddleWetness = 0.0f;
	};

	WeatherWetnessResult CalculateWeatherWetness(RE::TESWeather* weather, float weatherPct, bool isCurrentWeather) const;
};
