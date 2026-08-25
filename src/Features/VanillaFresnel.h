#pragma once

struct VanillaFresnel : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	// Metadata
	virtual inline std::string GetName() override { return "Vanilla Fresnel"; }
	virtual inline std::string GetShortName() override { return "VanillaFresnel"; }
	virtual inline std::string_view GetCategory() const override { return "Lighting"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Add realistic environmental reflections to vanilla materials.",
            {
                "Add environmental reflections to all materials",
                "Supports vanilla and complex materials",
                "Optionally turn vanilla phong specular into GGX",
                "Optionally turn static cubemaps into dynamic reflections"
            }
		};
	}

	// Functionality
	virtual bool inline SupportsVR() override { return true; }
	virtual inline std::string_view GetShaderDefineName() override { return "VANILLA_FRESNEL"; }
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };
	virtual void PostPostLoad() override;

	// Settings & UI
	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void DrawSettings() override;

	struct alignas(16) Settings
	{
		uint Enable = true;
        // (batch 9) GGX and the cubemap conversion now default on together. Conversion sets
        // F0/roughness into the shape the split-sum GGX lobe consumes, and with GGX off the
        // vanilla phong specular still runs and gets that F0 mixed into its SpecularColor --
        // two lighting models in one pixel. Upstream forbids the pairing outright; we default
        // to the half that is self-consistent instead. See DrawSettings/LoadSettings.
        uint EnableGGX = true;
        uint EnableGGXOnGrass = false;
        uint EnableDynamicCubemapsConversion = true;
        uint EnableEyeSpecialHandling = true;
        float RoughnessMultiplier = 1.0f;
        float SpecularRoughnessBlend = 1.0f;
        float BaseF0Multiplier = 0.32f;
        float MinF0 = 0.02f;
        float CubemapToF0Multiplier = 1.0f;
        float ComplexMaterialF0Multiplier = 1.0f;
        // (batch 10b) Absolute roughness for eye materials, replacing the hardcoded 0.1 in
        // Lighting.hlsl. 0.1 is physically right for a cornea, but it was calibrated against
        // vanilla eye cubemaps -- 32x32 and nearly black, so mip 0 held nothing to reflect.
        // With Auto Cubemaps Conversion the reflection source becomes a full-resolution
        // real-time HDR environment and the same 0.1 turns an eyeball into a mirror. This is
        // a deliberate default change: 0.1 was the old behaviour.
        float EyeRoughness = 0.7f;
        // (batch 10b) How much of an authored environment mask batch 10 honours. 0 restores
        // the pre-batch-10 behaviour (mask ignored), 1 applies the full mask.
        float EnvMaskStrength = 1.0f;
        // (batch 13) Roughness for eye materials in the *direct* light lobe only -- the sun and
        // every point light. EyeRoughness above now applies solely to the environment/cubemap
        // reflection and to the glossiness written into the G-buffer.
        //
        // One roughness could not serve both. Lighting.hlsl's GetLightSpecularInput feeds
        // roughness straight into BRDF::D_GGX, and every direct light calls it, so EyeRoughness
        // = 0.7 widened the sun's GGX lobe until the highlight stopped being a glint and became
        // a sheet of white spread over the whole eyeball -- the reported "eyes glow in side-on
        // sunlight". 0.7 was only ever a way to blur an environment reflection whose *intensity*
        // is too high; the direct lobe never needed that compensation and is physically right at
        // a cornea's 0.1. Default 0.1 therefore restores the pre-batch-10b direct-light look
        // while leaving the environment blur where batch 10b put it.
        float EyeDirectRoughness = 0.1f;
        // Explicit tail padding. 14 floats is 56 bytes and alignas(16) rounds the struct to
        // 64; the two slots are named so the HLSL mirror in Common/SharedData.hlsli can
        // spell out the same 16 slots. That cbuffer is a naked concatenation of every
        // feature's settings (src/FeatureBuffer.cpp), so the two sides must agree on the
        // struct's size, not only on its fields.
        //
        // (batch 13) EyeDirectRoughness was appended here, into what used to be pad0, so
        // sizeof stays 64 and nothing downstream in FeatureData moves. Contrast batch 10b,
        // which had no pad left to eat and shifted physSkyData, ssrtSettings,
        // exponentialHeightFogSettings and ssgiSettings by 16 bytes.
        float pad0 = 0.0f;
        float pad1 = 0.0f;
	} settings;

	// (batch 10b) FeatureData is a naked concatenation of every feature's settings struct
	// (src/FeatureBuffer.cpp), so this struct's *size* is load-bearing: get it wrong and every
	// feature struct after this one in the cbuffer silently misaligns instead of failing to
	// build. fxc reports the HLSL mirror at offset 1008, size 64, in FeatureData; the two new
	// floats grew it from 48, which shifts physSkyData, ssrtSettings,
	// exponentialHeightFogSettings and ssgiSettings by 16 bytes on both sides at once.
	//
	// (batch 13) Still 64: EyeDirectRoughness went into a pad slot, so no downstream offsets
	// moved this time. The next field added here has no pad left to take and will grow the
	// struct to 80.
	static_assert(sizeof(Settings) == 64,
		"VanillaFresnel::Settings must stay 16 x 4 bytes to match VanillaFresnelSettings in "
		"package/Shaders/Common/SharedData.hlsli.");
};