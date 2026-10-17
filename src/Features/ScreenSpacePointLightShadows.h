#pragma once

struct ScreenSpacePointLightShadows : Feature
{
	static ScreenSpacePointLightShadows* GetSingleton()
	{
		static ScreenSpacePointLightShadows singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Screen Space Point Light Shadows"; }
	virtual inline std::string GetShortName() override { return "ScreenSpacePointLightShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "SSPLS"; }
	bool HasShaderDefine(RE::BSShader::Type) override { return true; };
	virtual std::string_view GetCategory() const override { return "Lighting"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Screen Space Point Light Shadows adds contact shadows and soft shadows for point lights in the game.",
			{
				"Adjustable settings",
				"Drain your GPU",
				"Greatly enhances vibes",
				"Won't really look correct"
			}
		};
	}

	struct Settings
	{
		uint Enable = true;
		float Strength = 1.0f;
		uint StepLimit = 4;
		float RayLength = 0.2f;
		float CompareToleranceScale = 0.2f;
		float MaxDistance = 2048.0f;
		uint EnableSoftShadows = true;
		float SoftShadowScale = 2.5f;
		bool SkipTranslucent = true;  // (47h) "Skip Screen-Space Light Shadows on Translucent Surfaces" (Enable bit 4)
	} settings;

	/// Mirrors SharedData::SSPLSSettings (package/Shaders/Common/SharedData.hlsli). Its size is pinned
	/// by the FeatureData layout asserts in FeatureBuffer.cpp, so new switches travel as bits in Enable.
	struct alignas(16) PerFrame
	{
		uint Enable;
		float Strength;
		uint StepLimit;
		float RayLength;
		float CompareToleranceScale;
		float MaxDistance;
		uint EnableSoftShadows;
		float SoftShadowScale;
	};

	// (45 SSPLS) The orphan PrepareDepth chain (depth copy + linear depth mips + blur, bound to
	// t56/t57 that no shader read) and the never-dispatched depth-aware blur/upscale shaders were
	// removed, together with the ClearShaderCache double Release (E6). The shadows themselves are
	// traced inline in Lighting.hlsl / Water.hlsl, so their GPU cost sits in the engine's object rows.

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return true; };

	PerFrame GetCommonBufferData();
};
