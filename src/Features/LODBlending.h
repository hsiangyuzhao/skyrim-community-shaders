#pragma once

struct LODBlending : Feature
{
	virtual inline std::string GetName() override { return "LOD Blending"; }
	virtual inline std::string GetShortName() override { return "LODBlending"; }
	virtual inline std::string_view GetShaderDefineName() override { return "LOD_BLENDING"; }
	virtual std::string_view GetCategory() const override { return "Landscape & Textures"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Provides seamless visual transitions between Level of Detail (LOD) objects and full-detail objects, eliminating harsh transitions and creating smooth visual continuity.",
			{ "Smooth LOD object brightness blending",
				"Enhanced terrain LOD appearance matching",
				"Snow-specific LOD brightness adjustment",
				"Optional terrain vertex color modification",
				"Seamless transition between detail levels" }
		};
	}
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	struct Settings
	{
		float LODTerrainBrightness = 1;
		float LODObjectBrightness = 1;
		float LODObjectSnowBrightness = 1;
		uint DisableTerrainVertexColors = false;
		float LODTerrainGamma = 1;
		float LODObjectGamma = 1;
		float LODObjectSnowGamma = 1;
		// (batch 19) Claims the struct's spare float, so the shared feature buffer layout is
		// byte-for-byte unchanged and the pinned offsets in FeatureBuffer.cpp still hold.
		float LODGrassGamma = 1;
	};

	Settings settings;

	// (batch 19) Grass LOD has no LOD macro, so it is identified per draw in Hooks.cpp instead.
	// Two judgements, because the primary one rests on a naming convention:
	//   - name: shape name is exactly "grasspassthru" (every grass-LOD vertex in the measured
	//     object LOD sits under that one name, no variants)
	//   - fallback: no user data (merged LOD has no TESObjectREFR, placed foliage does) and the
	//     material is back-lit -- independent of any generator's naming
	//
	// They are not symmetric: a shape named grasspassthru is necessarily merged and back-lit, so
	// the name set is a subset of the fallback set. In-game draw counts for the two came out
	// equal, and a subset of equal size is the same set -- so on the measured content the two
	// agree exactly, which is what rules out the fallback over-reaching into placed back-lit
	// foliage. That is why the counters they were measured with are gone again.
	enum class GrassDetection : uint
	{
		Name = 0,
		NoUserDataAndBackLit,
		Total
	};

	GrassDetection grassDetection = GrassDetection::Name;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };
};
