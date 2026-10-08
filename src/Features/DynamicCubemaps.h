#pragma once

#include "Buffer.h"

class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
{
public:
	virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>* a_eventSource);
	static bool Register();
};

struct DynamicCubemaps : Feature
{
public:
	const std::string defaultDynamicCubeMapSavePath = "Data\\textures\\DynamicCubemaps";

	// Specular irradiance

	ID3D11SamplerState* computeSampler = nullptr;

	struct alignas(16) SpecularMapFilterSettingsCB
	{
		float roughness;
		float pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(SpecularMapFilterSettingsCB);

	ID3D11ComputeShader* specularIrradianceCS = nullptr;
	std::unique_ptr<ConstantBuffer> spmapCB;
	std::unique_ptr<Texture2D> envTexture;
	std::unique_ptr<Texture2D> envReflectionsTexture;
	/// @brief Per-mip UAVs onto envTexture / envReflectionsTexture.
	///
	/// (batch 16, item 3) These were bare `ID3D11UnorderedAccessView*[7]`: uninitialised, and
	/// overwritten without a Release whenever SetupResources re-ran. The twelve descriptors
	/// themselves are negligible, but each one holds a COM reference on the cubemap it views,
	/// so they were what kept the *previous* set of cubemaps alive after the unique_ptrs above
	/// let go of them. com_ptr's assignment releases the old view, which is what actually lets
	/// the old cubemaps die.
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 7> uavArray = {};
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, 7> uavReflectionsArray = {};

	// Reflection capture

	struct alignas(16) UpdateCubemapCB
	{
		float3 CameraPreviousPosAdjust;
		float CaptureWeight;  ///< (batch 39b) share of a new capture (was a fixed 0.5 in the shader)
	};
	STATIC_ASSERT_ALIGNAS_16(UpdateCubemapCB);

	ID3D11ComputeShader* updateCubemapCS = nullptr;
	ID3D11ComputeShader* updateCubemapReflectionsCS = nullptr;
	ID3D11ComputeShader* updateCubemapFakeReflectionsCS = nullptr;

	std::unique_ptr<ConstantBuffer> updateCubemapCB;

	ID3D11ComputeShader* inferCubemapCS = nullptr;
	ID3D11ComputeShader* inferCubemapReflectionsCS = nullptr;
	ID3D11ComputeShader* inferCubemapFakeReflectionsCS = nullptr;

	std::unique_ptr<Texture2D> envCaptureTexture;
	std::unique_ptr<Texture2D> envCaptureRawTexture;
	std::unique_ptr<Texture2D> envCapturePositionTexture;

	std::unique_ptr<Texture2D> envCaptureReflectionsTexture;
	std::unique_ptr<Texture2D> envCaptureRawReflectionsTexture;
	std::unique_ptr<Texture2D> envCapturePositionReflectionsTexture;

	std::unique_ptr<Texture2D> envInferredTexture;

	ID3D11ShaderResourceView* defaultCubemap = nullptr;

	bool activeReflections = false;
	bool fakeReflections = false;

	bool resetCapture[2] = { true, true };
	bool recompileFlag = false;

	enum class NextTask
	{
		kCapture,
		kInferrence,
		kIrradiance,
		kCapture2,
		kInferrence2,
		kIrradiance2
	};

	NextTask nextTask = NextTask::kCapture;
	uint32_t fastPlainCounter = 0;  ///< (batch 39b) frames since the plain chain last ran in the fast-capture schedule

	// Editor window

	struct Settings
	{
		uint EnabledCreator = false;
		uint EnabledSSR = true;
		uint pad0[2];
		float4 CubemapColor{ 1.0f, 1.0f, 1.0f, 0.0f };
	};

	Settings settings;
	bool enabledAtBoot = false;
	void UpdateCubemap();

	void PostDeferred();

	virtual inline std::string GetName() override { return "Dynamic Cubemaps"; }
	virtual inline std::string GetShortName() override { return "DynamicCubemaps"; }
	virtual inline std::string_view GetShaderDefineName() override { return "DYNAMIC_CUBEMAPS"; }
	virtual std::string_view GetCategory() const override { return "Materials"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Provides real-time environment mapping and reflections by generating dynamic cube maps that capture the surrounding environment, enabling realistic reflections on surfaces.",
			{ "Real-time environment capture for realistic reflections",
				"Dynamic cube map generation based on camera position",
				"Enhanced water reflections with environmental details",
				"Support for both standard and VR rendering modes",
				"Optimized cubemap inference and irradiance calculation" }
		};
	}
	virtual std::vector<std::pair<std::string_view, std::string_view>> GetShaderDefineOptions() override;

	bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void SetupResources() override;
	virtual void Reset() override;

	virtual void SaveSettings(json&) override;
	virtual void LoadSettings(json&) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void DataLoaded() override;
	virtual void PostPostLoad() override;

	std::map<std::string, Util::GameSetting> iniVRCubeMapSettings{
		{ "bAutoWaterSilhouetteReflections:Water", { "Auto Water Silhouette Reflections", "Automatically reflects silhouettes on water surfaces.", 0, true, false, true } },
		{ "bForceHighDetailReflections:Water", { "Force High Detail Reflections", "Forces the use of high-detail reflections on water surfaces.", 0, true, false, true } }
	};

	std::map<std::string, Util::GameSetting> hiddenVRCubeMapSettings{
		{ "bReflectExplosions:Water", { "Reflect Explosions", "Enables reflection of explosions on water surfaces.", 0x1eaa000, true, false, true } },
		{ "bReflectLODLand:Water", { "Reflect LOD Land", "Enables reflection of low-detail (LOD) terrain on water surfaces.", 0x1eaa060, true, false, true } },
		{ "bReflectLODObjects:Water", { "Reflect LOD Objects", "Enables reflection of low-detail (LOD) objects on water surfaces.", 0x1eaa078, true, false, true } },
		{ "bReflectLODTrees:Water", { "Reflect LOD Trees", "Enables reflection of low-detail (LOD) trees on water surfaces.", 0x1eaa090, true, false, true } },
		{ "bReflectSky:Water", { "Reflect Sky", "Enables reflection of the sky on water surfaces.", 0x1eaa0a8, true, false, true } },
		{ "bUseWaterRefractions:Water", { "Use Water Refractions", "Enables refractions for water surfaces, affecting how light bends through water.", 0x1eaa0c0, true, false, true } }
	};

	virtual void ClearShaderCache() override;
	ID3D11ComputeShader* GetComputeShaderUpdate();
	ID3D11ComputeShader* GetComputeShaderUpdateReflections();
	ID3D11ComputeShader* GetComputeShaderUpdateFakeReflections();

	ID3D11ComputeShader* GetComputeShaderInferrence();
	ID3D11ComputeShader* GetComputeShaderInferrenceReflections();
	ID3D11ComputeShader* GetComputeShaderInferrenceFakeReflections();

	ID3D11ComputeShader* GetComputeShaderSpecularIrradiance();

	void UpdateCubemapCapture(bool a_reflections);

	void Inferrence(bool a_reflections);

	void Irradiance(bool a_reflections);

	virtual bool SupportsVR() override { return true; };
	virtual bool IsCore() const override { return true; };
};
