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

	// (batch 15) Values for Settings::EyeDirectSpecularMode. Kept as a plain uint in the
	// struct so the cbuffer layout stays a flat run of 4-byte slots; this enum only names the
	// three cases for C++ and the UI.
	enum class EyeDirectSpecular : std::uint32_t
	{
		VanillaPhong = 0,  // direct highlight uses the vanilla phong lobe and its two gates
		GGXGated = 1,      // GGX lobe, with the gloss map and environment mask put back
		GGXRaw = 2,        // GGX lobe with no gates -- the post-batch-9 behaviour
		Total
	};

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
        // (batch 15) Which lighting model the *direct* highlight on an eye uses. This is the
        // fifth attempt at "eyes glow in side-on sunlight" and the first one that touches the
        // switch instead of the numbers behind it.
        //
        // Batch 9 flipped EnableGGX's default from false to true. That put Lighting.hlsl's
        // GGX block (:696) in charge of the direct lobe for the first time and, at the same
        // moment, made the SPECULAR assignment further down skip itself -- so eyes lost the
        // two per-pixel gates that used to hold their highlight down: the gloss map
        // (normal.w) and the environment mask. Eyes are the only surface in the game whose
        // GGX F0 and roughness are hardcoded instead of read from the material, so they are
        // the only surface where losing those gates leaves nothing to restrain the lobe:
        // BRDF::D_GGX peaks at 3183 for roughness 0.1 where the vanilla phong lobe peaks at 1.
        //
        // Batches 10b and 13 both moved EyeRoughness/EyeDirectRoughness and neither fixed it,
        // because the GGX lobe is energy-normalised -- roughness only decides how wide the
        // same total energy is smeared. 0.7 spread it over the whole eyeball (batch 10b's
        // "smeared glow"), 0.1 concentrated it into one point (batch 13's "glowing dot").
        //
        // 0 = VanillaPhong, 1 = GGXGated, 2 = GGXRaw; see the EyeDirectSpecular enum above.
        // Default 0 reproduces the December-2025 baseline (bedec8379), which is the only
        // configuration with a known-correct reference.
        uint EyeDirectSpecularMode = 0;
        // Explicit tail padding. 15 slots is 60 bytes and alignas(16) rounds the struct to
        // 80; the five spare slots are named so the HLSL mirror in Common/SharedData.hlsli
        // can spell out the same 20 slots. That cbuffer is a naked concatenation of every
        // feature's settings (src/FeatureBuffer.cpp), so the two sides must agree on the
        // struct's size, not only on its fields.
        //
        // (batch 13) EyeDirectRoughness was appended into what used to be pad0, so sizeof
        // stayed 64 and nothing downstream in FeatureData moved.
        //
        // (batch 15) EyeDirectSpecularMode had no pad left to take, so the struct grows
        // 64 -> 80 and physSkyData, ssrtSettings, exponentialHeightFogSettings and
        // ssgiSettings each shift 16 bytes later -- on both sides at once, which is what
        // makes it safe. Same shift batch 7 and batch 10b performed.
        float pad0 = 0.0f;
        float pad1 = 0.0f;
        float pad2 = 0.0f;
        float pad3 = 0.0f;
        float pad4 = 0.0f;
	} settings;

	// (batch 10b) FeatureData is a naked concatenation of every feature's settings struct
	// (src/FeatureBuffer.cpp), so this struct's *size* is load-bearing: get it wrong and every
	// feature struct after this one in the cbuffer silently misaligns instead of failing to
	// build. fxc reports the HLSL mirror at offset 1008, size 64, in FeatureData; the two new
	// floats grew it from 48, which shifts physSkyData, ssrtSettings,
	// exponentialHeightFogSettings and ssgiSettings by 16 bytes on both sides at once.
	//
	// (batch 13) Still 64: EyeDirectRoughness went into a pad slot, so no downstream offsets
	// moved that time.
	//
	// (batch 15) Now 80. EyeDirectSpecularMode took the 15th slot and alignas(16) rounded up,
	// so the four structs after this one in FeatureData move 16 bytes later. Verified against
	// fxc's reflection listing for Lighting.hlsl before and after; see the batch 15 report.
	static_assert(sizeof(Settings) == 80,
		"VanillaFresnel::Settings must stay 20 x 4 bytes to match VanillaFresnelSettings in "
		"package/Shaders/Common/SharedData.hlsli.");
};