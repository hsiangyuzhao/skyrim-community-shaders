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
        // (batch 9) How much of the (1-glossiness)^2 roughness derivation is mixed into the
        // specular-power one, scaled again by (1-glossiness) at the use site
        // (Lighting.hlsl:2408). Introduced to stop metals reading flat.
        //
        // (batch 18) Default back to 0. This was never part of the eye investigation -- it
        // fires on SPECULAR, non-eye pixels only -- but it moved the picture for every
        // vanilla specular material in the game, and six rounds of eye work made it
        // impossible to tell which visible change came from where. 0 makes the lerp at
        // :2409 collapse to its first argument, so the roughness is literally
        // `roughnessFromShininess` again: the pre-batch-9 formula, character for character.
        // The slider stays -- the flat-metal complaint was real and 1.0 is still the fix for
        // it, it just should not have been the default while something else was being
        // diagnosed.
        float SpecularRoughnessBlend = 0.0f;
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
        //
        // (batch 18) Default back to 0. Batch 10 honoured the mask in order to hold eyes down,
        // and that turned out to be doomed on its own terms: the vanilla eye mask is a black
        // field with a *bright* iris ring (73% of texels near black but 12.4% at >= 0.8,
        // p90 = 1.0), so the mask is ~1 exactly on the part of the eye that was complained
        // about and multiplying by it does nothing there. What it did do is cost every other
        // material: across the 270 shipping `_m.dds` masks the median per-texture mean is
        // 0.14, so metal armour lost roughly nine tenths of its reflection to fix an eye it
        // could never have fixed. 0 makes the lerp at Lighting.hlsl:3485 evaluate to 1.0 and
        // the multiply becomes the identity, which is exactly the batch-9 path. Slider kept.
        float EnvMaskStrength = 0.0f;
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
        // (batch 17) How much of the vanilla *soft lighting* term an eye keeps. This is the
        // sixth attempt at "eyes glow in side-on sunlight" and the first one that stops
        // looking at the highlight, because the previous five proved the highlight cannot be
        // the cause: in the default VanillaPhong mode the eye's direct lobe is HdotN^479
        // against the face's ^30, a pinprick that physically cannot wash out an eyeball.
        //
        // Soft lighting is a fill term Skyrim adds on top of the Lambert diffuse. Its
        // multiplier (Lighting.hlsl's GetSoftLightMultiplier) is zero facing the light, zero
        // fully backlit, and peaks *side-on* -- which is exactly the light dependence that
        // was reported, and no hypothesis about the highlight has that shape. On the sun it
        // is also the one term in the file that never gets multiplied by the screen-space
        // shadow, so the light it adds is completely unoccluded by brow, socket or hair.
        // Vanilla sets the strength behind it (Lighting Effect 1) to 1.0 on male and 1.5 on
        // female human eyes against 0.4 on a head, and pairs it with a soft-lighting map
        // (EyeBrown_sk.dds) whose median is 0.87 -- roughly half a full sunlight's worth of
        // unshadowed fill, measured offline from the vanilla NIFs and DDS.
        //
        // Batch 17 shipped this at 0, reasoning that 0 is what the *face* already gets:
        // Lighting.hlsl `#undef`s SOFT_LIGHTING outright whenever Subsurface Scattering is
        // installed, so on any setup with SSS the face has no soft lighting at all, and
        // zeroing it on eyes makes them consistent with the face rather than picking a
        // number. That argument still stands on paper.
        //
        // (batch 18) Default moved to 1 anyway, for a reason that has nothing to do with the
        // argument: batch 17 never actually ran. ValidateDiskCache (src/ShaderCache.cpp:2186)
        // only compares SHADER_CACHE_VERSION and each feature ini's Version -- it never looks
        // at a .hlsl -- and batch 17 bumped neither, so every machine kept serving batch-15
        // bytecode from Data/ShaderCache and this term was never once evaluated. Batch 18
        // bumps the ini, which means this code compiles for the first time. Shipping a
        // never-executed change in its *active* state at the same moment it first becomes
        // reachable would put two untested variables in one package; 1.0 makes the macro at
        // Lighting.hlsl:2661 evaluate to 1.0 on both ternary branches, so the term multiplies
        // by exactly one and the eye keeps the full vanilla fill. The slider is the whole
        // feature now -- the hypothesis is still worth testing, just not by default.
        //
        // This is a pre-existing defect, not one of ours: the term is byte-identical to the
        // December-2025 baseline (bedec8379). What changed is that batch 9 gave eyes a
        // real-time environment reflection they did not have, which lifted the whole eye and
        // pushed the fill over the threshold where it gets noticed.
        float EyeSoftLightingScale = 1.0f;
        // (batch 17) Multiply the *sun's* soft-lighting, rim-lighting and back-lighting terms
        // by the screen-space + parallax shadow, the way the point-light versions of the same
        // three terms in Lighting.hlsl already are. Those three sun terms take dirLightColor
        // before dirDetailShadow is applied; the point-light twins take a lightColor that has
        // already been multiplied by the light's shadow. That is an inconsistency, not a
        // design decision -- but fixing it changes every material carrying those flags, not
        // just eyes (foliage, cloth, leather all lose 40-70% of the term inside their own
        // shadow), so it ships as an off-by-default switch rather than a silent change.
        //
        // Hosted in this struct only because that is where the eye work lives; it is not
        // eye-specific and is read on every permutation, including ones compiled without the
        // VANILLA_FRESNEL define (FeatureData is populated whether or not the feature is on).
        //
        // Deliberately does *not* add saturate(N dot L) to those terms. Reaching around onto
        // the unlit side is the entire point of soft lighting; clamping it would switch the
        // feature off rather than shadow it. Occlusion only.
        uint ShadowSoftLighting = false;
        // Explicit tail padding. 17 slots is 68 bytes and alignas(16) rounds the struct to
        // 80; the three spare slots are named so the HLSL mirror in Common/SharedData.hlsli
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
        //
        // (batch 17) EyeSoftLightingScale and ShadowSoftLighting took pad3 and pad4, so
        // sizeof stays 80 and nothing downstream in FeatureData moves. The six offsetof
        // assertions batch 15 left in src/FeatureBuffer.cpp are what proves that rather than
        // assumes it: if this struct had grown past 80 they would fail to compile.
        float pad0 = 0.0f;
        float pad1 = 0.0f;
        float pad2 = 0.0f;
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
	//
	// (batch 17) Still 80. EyeSoftLightingScale and ShadowSoftLighting went into pad3/pad4,
	// so no downstream offset moved; fxc still reports vanillaFresnelSettings at 1008 size 80
	// and physSkyData at 1088.
	static_assert(sizeof(Settings) == 80,
		"VanillaFresnel::Settings must stay 20 x 4 bytes to match VanillaFresnelSettings in "
		"package/Shaders/Common/SharedData.hlsli.");
};