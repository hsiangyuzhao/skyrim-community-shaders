#pragma once

#include <Tracy/Tracy.hpp>
#include <Tracy/TracyD3D11.hpp>

#include <Buffer.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

#include <FeatureBuffer.h>

#include <Hooks.h>
#include <mutex>

class State
{
public:
	State()
	{
		std::lock_guard<std::mutex> lock(statsMutex);
		for (auto& v : smoothDrawCalls) v = 0.0;
		for (auto& v : drawCalls) v = 0;
		for (auto& v : frameTimePerType) v = 0.0f;
		for (auto& v : smoothFrameTimePerType) v = 0.0f;

		// Initialize QueryPerformanceCounter frequency
		frameTimingFrequency.QuadPart = 0;
		frameStartTime.QuadPart = 0;
	}
	std::lock_guard<std::mutex> Lock() { return std::lock_guard<std::mutex>(statsMutex); }

	static State* GetSingleton()
	{
		static State singleton;
		return &singleton;
	}

	bool enabledClasses[RE::BSShader::Type::Total - 1];
	bool enablePShaders = true;
	bool enableVShaders = true;
	bool enableCShaders = true;

	bool updateShader = true;
	bool settingCustomShader = false;
	RE::BSShader* currentShader = nullptr;
	std::string adapterDescription = "";

	uint32_t currentVertexDescriptor = 0;
	uint32_t currentPixelDescriptor = 0;
	spdlog::level::level_enum logLevel = spdlog::level::info;
	std::string shaderDefinesString = "";
	std::vector<std::pair<std::string, std::string>> shaderDefines{};  // data structure to parse string into; needed to avoid dangling pointers

	float timer = 0;
	double smoothDrawCalls[RE::BSShader::Type::Total + 1];
	int drawCalls[RE::BSShader::Type::Total + 1];

	// Frame time tracking per shader type (in milliseconds)
	float frameTimePerType[RE::BSShader::Type::Total + 1];
	float smoothFrameTimePerType[RE::BSShader::Type::Total + 1];

	// Timing state for per-type frame time tracking using QueryPerformanceCounter
	LARGE_INTEGER frameTimingFrequency;
	LARGE_INTEGER frameStartTime;
	bool frameTimingActive = false;

	// --- Wall-clock frame time on the attribution clock -------------------------------
	//
	// Everything the overlay's CPU attribution table shows must come from ONE clock and
	// ONE smoother, otherwise the numbers do not add up:
	//
	//   * the per-type rows are EMA-smoothed (0.95/0.05) and stepped once per frame here
	//     in Debug(), and they only ever cover first-draw..last-draw of the frame;
	//   * the overlay's own `smoothFrameTimeMs` is an *instantaneous* Present-to-Present
	//     sample re-snapped every UpdateInterval (0.5 s), i.e. a completely different
	//     estimator.
	//
	// Mixing the two produced both reported bugs: percentages were divided by the sum of
	// the smoothed buckets (a much smaller number, so Utility read 57.9% instead of ~28%)
	// while the residual was computed against the instantaneous wall clock, and the
	// residual could go negative whenever the smoothed buckets happened to exceed the
	// instantaneous sample.
	//
	// This is the same wall clock, sampled between consecutive new-frame detections and
	// smoothed with the same coefficients on the same cadence as the buckets, so
	// sum(buckets) <= smoothWallFrameTimeMs holds by construction.
	float smoothWallFrameTimeMs = 0.0f;
	LARGE_INTEGER lastFrameMarkTime{};
	bool wallFrameTimePrimed = false;
	// Running total from Util::CpuPassTimers at the previous draw call. Its delta is our
	// own CPU work inside the interval about to be charged to a shader type, and is
	// removed from it - see Debug().
	float accountedCpuSnapshotMs = 0.0f;
	// Longer than this (< 2 FPS) is a loading screen, an alt-tab or a period where the
	// overlay was hidden and Debug() did not run; folding it in would poison the average
	// for seconds. Matches PerformanceOverlay::Settings::kStatsMaxSampleMs.
	static constexpr float kMaxWallFrameSampleMs = 500.0f;

	/**
	 * @brief The denominator every CPU attribution number in the overlay must use.
	 *
	 * Wall-clock frame time on the same clock and the same smoother as the per-type
	 * buckets. Falls back to the overlay's own sampled frame time until the EMA has been
	 * primed (first frames after the overlay is shown), so the table is never blank.
	 */
	float GetAttributionFrameTimeMs() const;

	enum ConfigMode
	{
		DEFAULT,
		USER,
		TEST,
		THEME
	};

	void Draw();
	void Debug();
	void Reset();
	void Setup();

	void Load(ConfigMode a_configMode = ConfigMode::USER, bool a_allowReload = true);
	void Save(ConfigMode a_configMode = ConfigMode::USER);

	void LoadTheme();
	void SaveTheme();

	bool ValidateCache(CSimpleIniA& a_ini);
	void WriteDiskCacheInfo(CSimpleIniA& a_ini);

	void SetLogLevel(spdlog::level::level_enum a_level = spdlog::level::info);
	spdlog::level::level_enum GetLogLevel();

	void SetDefines(std::string defines);
	std::vector<std::pair<std::string, std::string>>* GetDefines();

	/*
     * Whether a_type is currently enabled in Community Shaders
     *
     * @param a_type The type of shader to check
     * @return Whether the shader has been enabled.
     */
	bool ShaderEnabled(const RE::BSShader::Type a_type);

	/*
     * Whether a_shader is currently enabled in Community Shaders
     *
     * @param a_shader The shader to check
     * @return Whether the shader has been enabled.
     */
	bool IsShaderEnabled(const RE::BSShader& a_shader);

	/*
     * Whether developer mode is enabled allowing advanced options.
	 * Use at your own risk! No support provided.
     *
	 * <p>
	 * Developer mode is active when the log level is trace or debug.
	 * </p>
	 *
     * @return Whether in developer mode.
     */
	bool IsDeveloperMode();

	void ModifyRenderTarget(RE::RENDER_TARGETS::RENDER_TARGET a_targetIndex, RE::BSGraphics::RenderTargetProperties* a_properties);

	void SetupResources();
	void ModifyShaderLookup(const RE::BSShader& a_shader, uint& a_vertexDescriptor, uint& a_pixelDescriptor, bool a_forceDeferred = false);

	void BeginPerfEvent(std::string_view title);
	void EndPerfEvent();
	void SetPerfMarker(std::string_view title);

	void SetAdapterDescription(const std::wstring& description);

	bool frameAnnotations = false;

	uint lastVertexDescriptor = 0;
	uint lastPixelDescriptor = 0;
	uint modifiedVertexDescriptor = 0;
	uint modifiedPixelDescriptor = 0;
	uint lastModifiedVertexDescriptor = 0;
	uint lastModifiedPixelDescriptor = 0;
	uint lastExtraDescriptor = 0;
	uint lastExtraFeatureDescriptor = 0;

	enum class ExtraShaderDescriptors : uint32_t
	{
		InWorld = 1 << 0,
		IsReflections = 1 << 1,
		IsBeastRace = 1 << 2,
		EffectShadows = 1 << 3,
		IsTree = 1 << 4,
		GrassSphereNormal = 1 << 5,
		// (batch 9) Set per draw by VanillaFresnel's BSLightingShader::SetupGeometry hook.
		// Bit 6 is the only free slot: bits 3/4/5 mean different things here than they do
		// upstream, so only this one bit can be taken over, not the block.
		IsEye = 1 << 6,
		// (batch 19) Set per draw by the BSLightingShader::SetupGeometry hook in Hooks.cpp.
		// Grass LOD is the one distant class no compile-time macro can reach: the LOD defines
		// come from GetLightingShaderDefines, which forwards straight to the vanilla engine
		// decoder, and that hands out LODOBJECTS/LODOBJECTSHD/LODLANDSCAPE from the geometry's
		// own LOD property flags. Merged grass LOD carries none of them (measured flags2
		// 0x08000021 = ZBufWrite|VertexColors|BackLighting), so this bit is the only way to
		// tell the shader "this draw is grass LOD".
		IsLODGrass = 1 << 7,
		// (batch 37b) Set per draw by PhysicalSky's BSSkyShader::SetupGeometry hook from the
		// sky object type: the vanilla sun quad (SO_SUN) and its glare (SO_SUN_GLARE). The
		// procedural sun is drawn on, and replaces, only the former.
		IsSun = 1 << 8,
		IsSunGlare = 1 << 9
	};

	enum class ExtraFeatureDescriptors : uint32_t
	{
		THLand0HasDisplacement = 1 << 0,
		THLand1HasDisplacement = 1 << 1,
		THLand2HasDisplacement = 1 << 2,
		THLand3HasDisplacement = 1 << 3,
		THLand4HasDisplacement = 1 << 4,
		THLand5HasDisplacement = 1 << 5,
		ETMaterialModel = 0b111 << 6,
		THLandHasDisplacement = 1 << 9
	};

	bool inWorld = false;
	bool activeReflections = false;

	void UpdateSharedData(bool a_inWorld, bool a_prepass);

	struct PermutationCB
	{
		uint VertexShaderDescriptor;
		uint PixelShaderDescriptor;
		uint ExtraShaderDescriptor;
		uint ExtraFeatureDescriptor;

		bool operator==(const PermutationCB& other) const
		{
			return PixelShaderDescriptor == other.PixelShaderDescriptor &&
			       ExtraShaderDescriptor == other.ExtraShaderDescriptor &&
			       ExtraFeatureDescriptor == other.ExtraFeatureDescriptor;
		}
	};
	STATIC_ASSERT_ALIGNAS_16(PermutationCB);

	std::unique_ptr<ConstantBuffer> permutationCB;

	struct alignas(16) SharedDataCB
	{
		float4 WaterData[25];
		DirectX::XMFLOAT3X4 DirectionalAmbient;
		float4 DirLightDirection;
		float4 DirLightColor;
		float4 CameraData;
		float4 BufferDim;
		float Timer;
		uint FrameCount;
		uint FrameCountAlwaysActive;
		uint InInterior;
		uint InMapMenu;
		uint HideSky;
		float MipBias;
		float pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(SharedDataCB);

	std::unique_ptr<ConstantBuffer> sharedDataCB;
	std::unique_ptr<ConstantBuffer> featureDataCB;

	PermutationCB permutationData{};
	PermutationCB permutationDataPrevious{};

	Util::FrameChecker frameChecker;
	uint frameCount = 0;

	// Skyrim constants
	float2 screenSize = {};
	D3D_FEATURE_LEVEL featureLevel;

	TracyD3D11Ctx tracyCtx = nullptr;  // Tracy context

	void ClearDisabledFeatures();
	bool SetFeatureDisabled(const std::string& featureName, bool isDisabled);
	bool IsFeatureDisabled(const std::string& featureName);
	std::unordered_map<std::string, bool>& GetDisabledFeatures();

	bool useFrameAnnotations = false;

	// --- Utility Methods ---
	/**
	 * @brief Gets the total smoothed draw calls from the global state
	 * @return Total number of draw calls as float
	 */
	float GetTotalSmoothedDrawCalls() const;

	/**
	 * @brief Base helper that iterates through valid shader types (excluding None and Total)
	 * @param callback Function to call for each valid shader type with parameters: (type, typeIndex, classIndex)
	 */
	template <typename Callback>
	static void ForEachValidShaderType(Callback callback)
	{
		for (auto type : magic_enum::enum_values<RE::BSShader::Type>()) {
			if (type == RE::BSShader::Type::None || type == RE::BSShader::Type::Total)
				continue;
			int typeIndex = magic_enum::enum_integer(type);
			int classIndex = typeIndex - 1;
			callback(type, typeIndex, classIndex);
		}
	}

	/**
	 * @brief Iterates through valid shader types with performance metrics
	 * @param callback Function to call for each shader type with parameters: (type, typeIndex, drawCalls, frameTime, percent, costPerCall)
	 */
	template <typename Callback>
	static void ForEachShaderTypeWithMetrics(Callback callback)
	{
		ForEachValidShaderType([&](auto type, int typeIndex, [[maybe_unused]] int classIndex) {
			float drawCalls = static_cast<float>(GetSingleton()->smoothDrawCalls[typeIndex]);
			float frameTime = static_cast<float>(GetSingleton()->smoothFrameTimePerType[typeIndex]);
			// Share of the WALL-CLOCK frame, not of the attributed sum. Dividing by
			// smoothFrameTimePerType[Total] (the sum of the buckets) made every row's
			// percentage relative to a ~20 ms denominator while the Other/Total rows used
			// the ~42 ms real frame time, so the column silently mixed two scales and the
			// rows added up to 100% of something the user never saw.
			// smoothFrameTimePerType[Total] is still maintained - it is the "attributed
			// sum" and is what Other is subtracted from - it just is not a percentage base.
			const float denominator = GetSingleton()->GetAttributionFrameTimeMs();
			float percent = (frameTime > 0.0f && denominator > 0.0f) ? (frameTime / denominator * 100.0f) : 0.0f;
			float costPerCall = (drawCalls > 0.0f) ? (frameTime / drawCalls) : 0.0f;
			callback(type, typeIndex, drawCalls, frameTime, percent, costPerCall);
		});
	}

	/**
	 * @brief Iterates through valid shader types with class indices for UI operations
	 * @param callback Function to call for each shader type with parameters: (type, classIndex)
	 */
	template <typename Callback>
	static void ForEachShaderTypeWithIndex(Callback callback)
	{
		ForEachValidShaderType([&](auto type, [[maybe_unused]] int typeIndex, int classIndex) {
			callback(type, classIndex);
		});
	}

	// Features that are more special then others
	std::unordered_map<std::string, bool> specialFeatures = {
		{ "TruePBR", false }
	};
	std::unordered_map<std::string, bool> disabledFeatures;

	inline ~State()
	{
#ifdef TRACY_ENABLE
		if (tracyCtx)
			TracyD3D11Destroy(tracyCtx);
#endif
	}

private:
	std::shared_ptr<REX::W32::ID3DUserDefinedAnnotation> pPerf;
	std::mutex statsMutex;
};
