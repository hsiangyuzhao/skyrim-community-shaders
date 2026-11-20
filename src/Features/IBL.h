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

	std::unique_ptr<Texture2D> diffuseIBLTexture;
	std::unique_ptr<Texture2D> diffuseSkyIBLTexture;
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

	/// (49i) Physical Sky's Scene Lighting readout "CPU sky vs the IBL sky probe": the sky probe (DiffuseSkyIBLTexture, 3 texels = the R / G / B
	/// SH) copied to a staging texture and read back without waiting (a few frames late), only while the readout is drawn.
	void RequestSkyProbeReadback();
	/// The probe's value for an upward face as the shaders evaluate it (IBL.hlsli GetSkyDiffuseIBL(-N) with N = up: irradiance / pi).
	bool GetSkyProbeUp(float3& o_value) const;
	void QueueSkyProbeReadback();
	static constexpr int kSkyProbeReadbackCount = 3;
	winrt::com_ptr<ID3D11Texture2D> skyProbeStaging[kSkyProbeReadbackCount];
	bool skyProbePending[kSkyProbeReadbackCount] = {};
	uint32_t skyProbeIndex = 0;
	float skyProbeSH[3][4] = {};
	bool skyProbeValid = false;
	bool skyProbeWanted = false;
	uint32_t skyProbeWantedFrame = 0;
	uint32_t skyProbeReadFrame = 0;
};
