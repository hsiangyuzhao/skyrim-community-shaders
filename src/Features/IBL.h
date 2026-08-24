#pragma once

#include "Buffer.h"

struct IBL : Feature
{
public:
	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return false; };

	virtual inline std::string GetName() override { return "Image Based Lighting"; }
	virtual inline std::string GetShortName() override { return "ImageBasedLighting"; }
	virtual inline std::string_view GetShaderDefineName() override { return "IBL"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Image Based Lighting provides realistic diffuse ambient lighting for exteriors.",
			{ "Realistic diffuse ambient lighting from environment maps",
				"Spherical harmonics-based ambient light calculation",
				"Enhanced exterior ambient lighting quality",
				"Configurable intensity and saturation, mixing with DALC" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	Texture2D* diffuseIBLTexture = nullptr;
	Texture2D* diffuseSkyIBLTexture = nullptr;
	ID3D11ComputeShader* diffuseIBLCS = nullptr;

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void EarlyPrepass() override;
	virtual void Prepass() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;

	struct Settings
	{
		uint EnableDiffuseIBL = 1;
		uint PreserveFogLuminance = 0;
		uint UseStaticIBL = 1;
		uint EnableInterior = 0;
		float DiffuseIBLScale = 1.0f;
		float DALCAmount = 0.33f;
		float IBLSaturation = 1.0f;
		float FogAmount = 0.0f;
		uint EffectNormalization = 0;
		float EffectNormalizationMult = 5.0f;
		float MinEffectMult = 1.0f;
		// (B7) Per-source trim on the two probes GetIBLColor splits the ambient into. All four
		// default to 1.0 and each is an exact no-op at that value, so a default install renders
		// bit-for-bit what it rendered before they existed; see IBL.hlsli's GetIBLColor.
		// DiffuseIBLScale / IBLSaturation above stay where they are - they remain the master
		// gate applied by the callers on the combined result, these four sit underneath it.
		float EnvIBLScale = 1.0f;
		float SkyIBLScale = 1.0f;
		float EnvIBLSaturation = 1.0f;
		float SkyIBLSaturation = 1.0f;
		float pad;
	} settings;
	STATIC_ASSERT_ALIGNAS_16(Settings);

	eastl::unique_ptr<Texture2D> staticDiffuseIBLTexture = nullptr;
	eastl::unique_ptr<Texture2D> staticSpecularIBLTexture = nullptr;

	ID3D11ComputeShader* GetDiffuseIBLCS();
};
