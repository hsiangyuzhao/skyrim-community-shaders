#pragma once

#include "Buffer.h"

struct ExponentialHeightFog : Feature
{
	virtual bool SupportsVR() override { return true; };
	virtual inline std::string GetName() override { return "Exponential Height Fog"; }
	virtual inline std::string GetShortName() override { return "ExponentialHeightFog"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
    virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Exponential Height Fog adds a realistic fog effect that increases in density with height, enhancing atmospheric depth and immersion in the game environment.",
			{
				"Added exponential height fog effect",
				"Adapted to vanilla fog settings",
				"Creates atmospheric depth",
				"Optional volumetric fog lit by the sun, sky and nearby lights",
			}
		};
	}

	virtual inline std::string_view GetShaderDefineName() override { return "EXP_HEIGHT_FOG"; }
	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	/// (batch 38, A1) Builds the froxel volumes: conservative depth, medium, light scattering
	/// (with temporal reprojection) and front-to-back integration, near then far grid.
	virtual void Prepass() override;

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/// Mirrors SharedData::ExponentialHeightFogSettings (HLSL). Its size is pinned in
	/// FeatureBuffer.cpp: the batch 38 second layer took the old float3 pad.
	struct alignas(16) Settings
	{
		uint enabled = 1;
		uint useDynamicCubemaps = 0;
		float startDistance = 0.0f;
		float fogHeight = 0.0f;
		float fogHeightFalloff = 0.2f;
		float fogDensity = 0.02f;
		float directionalInscatteringMultiplier = 1.0f;
		float directionalInscatteringExponent = 2.0f;
		float4 inscatteringTint = { 1.0f, 1.0f, 1.0f, 1.0f };
		float cubemapMipLevel = 2.0f;
		// (batch 38, upstream #2831) second stacked fog layer; density 0 = off (37c)
		float fogHeight2 = 0.0f;
		float fogHeightFalloff2 = 0.2f;
		float fogDensity2 = 0.0f;
	} settings;
	STATIC_ASSERT_ALIGNAS_16(Settings);

	/// Upstream fix 22ac9859c: fog colour was weighted by fog opacity twice (cubemap colour and
	/// sun glow). CPU side only; reaches the shaders as bit 1 of useDynamicCubemaps.
	bool upstreamFixFogDoubleOpacity = true;

	/// (batch 38, A1) Volumetric fog settings. CPU side only: the compute passes get them in
	/// their own constant buffer, pixel shaders get VolumetricFogPSData.
	struct VolumetricSettings
	{
		bool Enabled = false;  // default off (spec: highest visual risk of batch 38)
		float Distance = 60000.0f;
		float StartDistance = 0.0f;
		float NearFadeInDistance = 1000.0f;
		float NearGridDistance = 8000.0f;
		float ExtinctionScale = 1.0f;
		float ScatteringDistribution = 0.2f;
		float4 Albedo = { 1.0f, 1.0f, 1.0f, 1.0f };
		float4 Emissive = { 0.0f, 0.0f, 0.0f, 0.0f };
		float DirectionalIntensity = 1.0f;
		float SkyLightingIntensity = 1.0f;
		float LocalLightIntensity = 1.0f;
		/// Temporal history weight. Upstream 0.96 (about 25 frames); 0.9 here (about 10 real
		/// frames) because torches flicker and DLSS-G multiplies how long a lag is visible.
		float HistoryWeight = 0.9f;
		uint HistoryMissSampleCount = 4;
		float SampleJitter = 0.0f;
		float UpsampleJitter = 1.0f;
		uint GridPixelSize = 16;
		uint GridSizeZ = 64;
		uint FarGridPixelSize = 64;
		uint FarGridSizeZ = 32;
		float ShadowBias = 0.002f;
		float DepthDistributionScale = 8.0f;
		float NoiseScale = 0.0f;  // 0 = no noise
		float NoiseThreshold = 0.5f;
		float3 NoiseVelocity = { 0.0f, 0.0f, 0.0f };
	} volumetric;

	/// Mirrors SharedData::VolumetricFogSettings (HLSL), appended to FeatureData.
	struct alignas(16) VolumetricFogPSData
	{
		uint Enabled;
		uint FarEnabled;
		float StartDistance;
		float EndDistance;
		float4 NearGridZParams;
		float4 FarGridZParams;
		float NearGridEndDistance;
		float UpsampleJitter;
		float pad0[2];
	};
	STATIC_ASSERT_ALIGNAS_16(VolumetricFogPSData);

	/// Settings as the shaders see them: the second layer is 37c-off with the master off.
	[[nodiscard]] Settings GetCommonBufferData() const;
	[[nodiscard]] VolumetricFogPSData GetVolumetricPSData() const;

	/// Volumetric fog switched on (master && own switch). Vanilla Volumetric Lighting pauses
	/// while this is true, whether or not there is fog to build this frame.
	[[nodiscard]] bool VolumetricFogRequested() const;
	/// Requested and there is a medium to build: height fog on, some density, not the map.
	[[nodiscard]] bool VolumetricFogActive() const;
	/// Why VolumetricFogActive() is false while requested (for the UI), or empty.
	[[nodiscard]] std::string VolumetricFogIdleReason() const;

	// Diagnostics (shown under Volumetric Fog)
	DirectX::XMUINT4 currentGridSize = {};
	DirectX::XMUINT4 currentFarGridSize = {};
	uint32_t lastBuildFrame = UINT32_MAX;
	bool lastBuildHadShadows = false;
	bool lastBuildHadLocalLights = false;

private:
	struct GridParams
	{
		double nearPlane;
		double nearEnd;
		double totalFar;
		bool farEnabled;
		float4 nearZ;  // scale, offset, distribution, slices
		float4 farZ;
	};
	[[nodiscard]] GridParams ComputeGridParams(uint32_t a_nearSlices, uint32_t a_farSlices) const;

	struct VolumetricFogCB
	{
		DirectX::XMUINT4 gridSizeAndFlags = {};
		float4 invGridSizeAndNearFade = {};
		float4 gridZParams = {};
		float4x4 clipToWorld[2] = {};
		float4 frameJitterOffsets[16] = {};
		float4 historyParameters = {};
		float4 jitterParameters = {};
		DirectX::XMUINT4 farGridSizeAndFlags = {};
		float4 farInvGridSizeAndNearFade = {};
		float4 farGridZParams = {};
		float4 farRange = {};
		float4 albedo = {};
		float4 emissive = {};
		float4 lighting = {};
		float4 misc = {};
		float4 noiseVelocity = {};
	};
	STATIC_ASSERT_ALIGNAS_16(VolumetricFogCB);

	void EnsureVolumetricResources();
	void ReleaseVolumetricResources();
	void BindIntegratedLightScattering(bool a_valid);
	ID3D11ComputeShader* GetShader(ID3D11ComputeShader*& a_cache, const wchar_t* a_file, bool a_far, bool a_lighting);

	std::unique_ptr<Texture3D> vBufferA;
	std::unique_ptr<Texture3D> vBufferAFar;
	std::unique_ptr<Texture2D> conservativeDepth;
	std::unique_ptr<Texture2D> conservativeDepthHistory;
	std::unique_ptr<Texture2D> conservativeDepthFar;
	std::unique_ptr<Texture2D> conservativeDepthFarHistory;
	std::unique_ptr<Texture3D> lightScattering;
	std::unique_ptr<Texture3D> lightScatteringHistory;
	std::unique_ptr<Texture3D> lightScatteringFar;
	std::unique_ptr<Texture3D> lightScatteringFarHistory;
	std::unique_ptr<Texture3D> integratedLightScattering;
	std::unique_ptr<Texture3D> integratedLightScatteringFar;
	std::unique_ptr<ConstantBuffer> volumetricFogCB;
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
	winrt::com_ptr<ID3D11SamplerState> shadowSampler;
	ID3D11ComputeShader* materialSetupCS = nullptr;
	ID3D11ComputeShader* farMaterialSetupCS = nullptr;
	ID3D11ComputeShader* conservativeDepthCS = nullptr;
	ID3D11ComputeShader* farConservativeDepthCS = nullptr;
	ID3D11ComputeShader* lightScatteringCS = nullptr;
	ID3D11ComputeShader* farLightScatteringCS = nullptr;
	ID3D11ComputeShader* integrationCS = nullptr;
	ID3D11ComputeShader* farIntegrationCS = nullptr;
	bool hasLightScatteringHistory = false;
	bool hasConservativeDepthHistory = false;
	bool hasLightScatteringFarHistory = false;
	bool hasConservativeDepthFarHistory = false;
	uint32_t lastPrepassFrame = UINT32_MAX;
};
