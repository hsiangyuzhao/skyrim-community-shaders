#pragma once

#include "Buffer.h"

// (47e) The 16-byte aligned members (Settings, WeatherFogData) pad the class once anything else changes size.
#pragma warning(push)
#pragma warning(disable: 4324)

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
		/// (47e) CS Fog Takes Over: the weather's part of the volume also gets the light scattered many times that the
		/// weather's fog colour already holds, so it is as bright as the game's fog (VolumetricFogLightScatteringCS).
		bool MatchWeatherFogBrightness = true;
		/// (47j) "Skip Volumetric Fog Indoors" (was "Volumetric Fog: Off Indoors"): no volumetric fog passes in interior cells
		/// (and nothing of them applied there), saves about 1 ms; its history starts over outside. Exteriors unchanged.
		/// (47m) Default off (the fog is built indoors too); settings saved before 47m are switched off once (OffIndoors47m).
		bool OffIndoors = false;
	} volumetric;

	/// (47b) The game's own fog under the height fog. CPU side only; reaches the shaders through
	/// VolumetricFogPSData::pad0 (see GetVolumetricPSData). Saved under "Vanilla Fog".
	/// (47d) "EHF Follows Weather" (exteriors), formerly a bool (true -> CSFogTakesOver, false -> Off).
	enum WeatherMode : uint32_t
	{
		kWeatherOff = 0,             ///< the height fog replaces the weather's fog (47a2)
		kWeatherBlendVanillaFog = 1, ///< the weather's own fog under the height fog, weighted (47b)
		kWeatherCSFogTakesOver = 2,  ///< the weather's fog as a layer of the height fog (47d)
		kWeatherModeCount
	};

	struct VanillaFogSettings
	{
		/// "EHF Follows Weather" (exteriors), a WeatherMode. Every mode but Off also fixes the fog colour being weighted
		/// by its opacity twice (upstream 22ac9859c), and weights the weather's fog by how foggy the weather is (from
		/// its engine-blended fog distance).
		uint32_t WeatherMode = kWeatherCSFogTakesOver;
		// (47m) "EHF Restores Room Fog" is permanent (interiors, every mode): the cell's own fog comes back in full under the
		// height fog. Old key RestoreRoomFog is ignored.
		/// Weather fog distance (game units) at and above which the weight is 0 (clear) ...
		float ClearFogFar = 120000.0f;
		/// ... and at and below which it is 1 (fog, storm); log-linear in between.
		float FullFogFar = 15000.0f;
		/// (47d) "CS Fog Takes Over": above the camera the weather's fog thins by e every this many game units (~300 m).
		float TakeoverScaleHeight = 21000.0f;
		/// (47g) "Physical Fog Brightness": the fog's brightness from what lights it (the clear physical sky and the sun through
		/// the weather layer), the fog colour giving only its hue. Where the fog takes the fog colour (not Dynamic Cubemaps)
		/// and in Match Weather Fog Brightness; needs Physical Sky outdoors. Off = 47f. (47m) Kept until the Physical Sky repair.
		bool PhysicalFogBrightness = true;
	} vanillaFog;

	/// (47b) The current weight and what it was computed from (also the menu readout).
	struct VanillaFogState
	{
		float weight = 0.0f;
		bool interior = false;
		bool hasWeather = false;  // exterior with a full sky and a weather
		float fogNear = 0.0f, fogFar = 0.0f, fogPower = 0.0f, fogClamp = 0.0f;
		const char* reason = "";  // why the weight is 0, for the readout
		// (47d) "CS Fog Takes Over" layer (takeover = it applies this frame)
		bool takeover = false;
		float gamma = 1.0f;          // Linear Lighting's fog transparency gamma (1 without Linear Lighting)
		float screenCap = 0.0f;      // the weather's on-screen max opacity, clamp^gamma
		float halfDistance = 0.0f;   // on-screen half-cover distance (game units from the camera)
		float sigma = 0.0f;          // extinction per game unit at and below the camera's height
	};
	[[nodiscard]] VanillaFogState GetVanillaFogState() const;
	/// Flag bits in VolumetricFogPSData::pad0[1] (as uint); mirrored by ExponentialHeightFog.hlsli / WeatherFog.hlsli.
	static constexpr uint32_t kFogFlagOpacityFix = 1u;
	static constexpr uint32_t kFogFlagTakeover = 2u;  // (47d)

	/// (47d) Mirrors SharedData::WeatherFogData (HLSL), appended at the very end of FeatureData.
	struct alignas(16) WeatherFogData
	{
		float Sigma = 0.0f;
		float Cap = 0.0f;  // weight x the on-screen max opacity
		float NearDistance = 0.0f;
		float ScaleHeight = 21000.0f;
	};
	STATIC_ASSERT_ALIGNAS_16(WeatherFogData);
	[[nodiscard]] WeatherFogData GetWeatherFogData() const;

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
		float pad0[2];  // (47b) [0] = the game's fog weight, [1] = kFogFlag* bits (as uint)
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

	// Diagnostics for the Batch 38 table
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
		// (47e) mirrored by VolumetricFogCSCommon.hlsli
		float4 weatherNearColor = {};  // rgb, w = 1: "Match Weather Fog Brightness" on
		float4 weatherFarColor = {};   // rgb, w = fog power
		float4 weatherFogParams = {};  // near / (far - near), 1 / (far - near), clamp, debug readout slice
	};
	STATIC_ASSERT_ALIGNAS_16(VolumetricFogCB);
	static_assert(sizeof(VolumetricFogCB) == 656, "VolumetricFogCB must match VolumetricFogCSCommon.hlsli");
	static_assert(offsetof(VolumetricFogCB, noiseVelocity) == 592 && offsetof(VolumetricFogCB, weatherFogParams) == 640, "VolumetricFogCB layout");

	/// (47e) EHF panel readout of the "Match Weather Fog Brightness" terms at one froxel (screen centre, ~100 m), read
	/// back from the GPU a few frames late.
	struct VolumetricDebugState
	{
		float weather[4]{};  // Wfog, sigma_w
		float sun[4]{};      // E Ts / 4 pi, view depth
		float sky[4]{};      // S, the extra source's share of Wfog
		bool valid = false;
		uint32_t frame = 0;
		winrt::com_ptr<ID3D11Buffer> buffer;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
		winrt::com_ptr<ID3D11Buffer> staging[3];
	};
	void ReadVolumetricDebug();

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
	/// (47d) The weather layer the volumes were last built with: a jump (forced weather, mode switch) drops the
	/// temporal history so the old fog does not ghost.
	WeatherFogData lastVolumeWeather{};
	[[nodiscard]] static bool WeatherLayerJumped(const WeatherFogData& a_previous, const WeatherFogData& a_current);
	std::unique_ptr<VolumetricDebugState> volumetricDebug;  // (47e)
};
#pragma warning(pop)
