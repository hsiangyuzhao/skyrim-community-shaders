#pragma once

/**
 * @brief Environment Ambient ("L1"): explicit, directional ambient light layer.
 *
 * Replaces the vanilla directional-ambient (DALC) term inside the deferred composite with
 * prefiltered dynamic-cubemap radiance modulated by Skylighting sky visibility and Screen Space
 * GI occlusion. This reproduces the Screen Space Ray Tracing dynamic-cubemap diffuse fallback
 * (ssrt_raymarch.hlsl) at one sample per pixel instead of one per ray, without depending on the
 * implicit AmbientMult override.
 *
 * The feature owns no GPU resources: every input it needs (Masks, Albedo, NormalRoughness, Depth,
 * EnvTexture, EnvReflectionsTexture, SkylightingProbeArray, SsgiAo) is already bound by
 * Deferred::DeferredPasses(). Its only shader-side footprint is the ENV_AMBIENT define on
 * DeferredCompositeCS and the settings block in the shared FeatureData constant buffer.
 */
struct EnvironmentAmbient : Feature
{
	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return false; };

	virtual inline std::string GetName() override { return "Environment Ambient"; }
	virtual inline std::string GetShortName() override { return "EnvironmentAmbient"; }
	virtual inline std::string_view GetShaderDefineName() override { return "ENV_AMBIENT"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Environment Ambient replaces vanilla directional ambient light with prefiltered "
			"dynamic cubemap radiance, giving backlit surfaces a real environment colour.",
			{
				"Directional ambient light sampled from the dynamic cubemaps",
				"Sky contribution modulated by Skylighting visibility",
				"Contact darkening from Screen Space GI occlusion",
				"Explicit blend against vanilla directional ambient",
			}
		};
	}

	// L1 lives entirely in DeferredCompositeCS; it needs no BSShader permutation define.
	bool HasShaderDefine(RE::BSShader::Type) override { return false; };

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/**
	 * @brief Settings block appended to the shared FeatureData constant buffer (b6).
	 *
	 * Field order and types must match SharedData::EnvAmbientSettings in
	 * package/Shaders/Common/SharedData.hlsli byte for byte.
	 */
	struct alignas(16) Settings
	{
		uint Enabled = 1;               // 0
		float Blend = 1.0f;             // 4
		float Intensity = 1.0f;         // 8
		float Normalization = 0.0f;     // 12
		float EnvMip = 4.0f;            // 16
		float Saturation = 1.0f;        // 20
		float AOPower = 1.5f;           // 24
		uint ApplyAO = 1;               // 28
		uint EnableInterior = 0;        // 32
		uint NormalizationMode = 1;     // 36
		float pad0[2] = { 0.0f, 0.0f }; // 40
	} settings;
	static_assert(sizeof(Settings) == 48, "EnvironmentAmbient::Settings must stay 48 bytes (3 constant buffer rows).");
};
