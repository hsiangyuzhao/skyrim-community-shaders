#pragma once

struct LinearLighting : Feature
{
	static LinearLighting* GetSingleton()
	{
		static LinearLighting singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Linear Lighting"; }
	virtual inline std::string GetShortName() override { return "LinearLighting"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Linear Lighting does internal color space conversion to improve lighting calculation accuracy.",
			{ "Customizable gamma correction",
				"Corrects lighting calculations",
				"Makes PBR really work" }
		};
	}

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };

	struct Settings
	{
		uint enableLinearLighting = true;
		uint enableGammaCorrection = true;
		float lightGamma = 1.8f;
		float colorGamma = 2.2f;
		float emitColorGamma = 2.2f;
		float glowmapGamma = 2.2f;
		float ambientGamma = 1.8f;
		float fogGamma = 2.2f;
		float fogAlphaGamma = 1.0f;
		float effectGamma = 1.8f;
		float effectAlphaGamma = 1.0f;
		float skyGamma = 1.8f;
		float waterGamma = 1.8f;
		float vlGamma = 1.8f;

		// Lighting multipliers
		float vanillaDiffuseMult = 0.32f;
		float vanillaSpecularMult = 0.32f;
		float grassDiffuseMult = 0.32f;
		float grassSpecularMult = 0.32f;
		float vanillaDiffuseColorMult = 1.5f;
		float lightMult = 1.0f;
		float directionalLightMult = 1.0f;
		float pointLightMult = 1.0f;
		float emitColorMult = 1.0f;
		float glowmapMult = 1.0f;

		// Effect multipliers
		float effectLightingMult = 0.32f;
		float membraneEffectMult = 1.0f;
		float bloodEffectMult = 1.0f;
		float projectedEffectMult = 1.0f;
		float deferredEffectMult = 1.0f;
		float otherEffectMult = 1.0f;  ///< used while "Effects Follow Scene Light" is off (the old single multiplier)

		// (49g) "Effects Follow Scene Light" (Effects tree). Off: exactly the old effect lighting with otherEffectMult.
		bool effectsFollowScene = true;
		bool effectsFollowLit = true;    ///< Lit Smoke and Mist: the light a matte surface gets at that spot
		bool effectsFollowUnlit = true;  ///< Unlit Smoke and Mist: x the scene's ambient brightness relative to midday
		// The three multipliers that replace otherEffectMult while it is on (missing in old settings: all take otherEffectMult).
		float fireMagicEffectMult = 1.0f;
		float litSmokeEffectMult = 1.0f;
		float unlitSmokeEffectMult = 1.0f;

		// (50c) Window Light (lights and glass tied to a region's emittance; WindowLight.h). Three switches off + Mod's Keys: the engine's colour exactly.
		bool windowNoExtraSaturation = true;  ///< the light gamma on each key's luminance only (its colour is not saturated further)
		bool windowFollowSun = true;          ///< key blend from the sun's true elevation instead of the climate clock
		int windowBrightness = 0;             ///< 0 Smooth (brightness from the sun's elevation only), 1 Mod's Keys
		bool windowFollowWeather = true;      ///< x outdoor direct light now / the same under a clear sky (moon phase and height at night)
	} settings;

	/** (49g) Effect.hlsl EffectSceneCB (PS b7), bound for every effect draw. */
	struct alignas(16) EffectSceneData
	{
		uint flags;  // 1 master, 2 lit, 4 unlit (0 while Linear Lighting is off)
		float fireMagicMult;
		float litSmokeMult;
		float unlitSmokeMult;
		float unlitSceneScale;
		float pad[3];
	};
	static_assert(sizeof(EffectSceneData) == 32);
	std::unique_ptr<ConstantBuffer> EffectSceneCB;

	/** Lowest scale for unlit smoke and mist (very dark scenes keep a trace of it). */
	static constexpr float kMinUnlitSceneScale = 0.05f;
	float unlitSceneScale = 1.0f;        ///< this frame's value (menu status)
	float unlitAmbientNow = 0.0f;        ///< luminance of the ambient now (menu status)
	float unlitAmbientMidday = -1.0f;    ///< this weather's midday ambient luminance; last exterior value indoors (< 0: none yet)

	/** @brief (49g) Binds EffectSceneCB to PS b7 (Hooks.cpp, BSEffectShader::SetupGeometry). */
	void BindEffectScene();
	/** @brief (49g) Unbinds PS b7 after an effect draw (BSEffectShader::RestoreGeometry): b7 is Advanced Skin's slot in Lighting. */
	void UnbindEffectScene();
	/** @brief (49g) The unlit smoke and mist scale: ambient luminance now / the current weather's midday ambient, clamped. */
	float ComputeUnlitSceneScale();

	struct alignas(16) PerFrameData
	{
		uint enableLinearLighting;
		uint enableGammaCorrection;
		uint isDirLightLinear;
		float dirLightMult;
		float lightGamma;
		float colorGamma;
		float emitColorGamma;
		float glowmapGamma;
		float ambientGamma;
		float fogGamma;
		float fogAlphaGamma;
		float effectGamma;
		float effectAlphaGamma;
		float skyGamma;
		float waterGamma;
		float vlGamma;
		float vanillaDiffuseMult;
		float vanillaSpecularMult;
		float grassDiffuseMult;
		float grassSpecularMult;
		float vanillaDiffuseColorMult;
		float lightMult;
		float directionalLightMult;
		float pointLightMult;
		float emitColorMult;
		float glowmapMult;
		float effectLightingMult;
		float membraneEffectMult;
		float bloodEffectMult;
		float projectedEffectMult;
		float deferredEffectMult;
		float otherEffectMult;
	};

	struct alignas(16) PerGeometryData
	{
		float emissiveMult;
		float pad0[3];
	};

	std::unique_ptr<ConstantBuffer> PerGeometryCB;

	uint isDirLightLinear = false;
	float dirLightMult = 1.0f;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	virtual void SetupResources() override;

	PerFrameData GetCommonBufferData();

	RE::NiColor ColorToLinear(RE::NiColor inColor, float gamma);

	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);

	struct Hooks;
};
