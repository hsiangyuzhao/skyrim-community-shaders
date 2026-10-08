#pragma once

#include "Buffer.h"
#include <filesystem>

struct TerrainShadows : public Feature
{
private:
	static constexpr std::string_view MOD_ID = "135817";

public:
	virtual inline std::string GetName() override { return "Terrain Shadows"; }
	virtual inline std::string GetShortName() override { return "TerrainShadows"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "TERRAIN_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return "Landscape & Textures"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Adds realistic shadow casting from terrain features using heightmap data to create accurate terrain shadows that enhance depth perception and visual realism.",
			{ "Heightmap-based terrain shadow calculation",
				"Dynamic shadow updates based on sun position",
				"Support for custom heightmap files",
				"Real-time shadow preprocessing and computation",
				"Integration with existing shadow systems" }
		};
	}
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	struct Settings
	{
		bool EnableTerrainShadow = true;
		// (batch 40) Ported from upstream (jiayev) and adapted; each one off = the batch 39 behaviour.
		/// Upstream #2729 (652521d42 + df687ca41): a steadier heightmap sweep, a 1 degree soft
		/// angle plus a small z blur instead of 4 degrees, and the texel half-offset fix.
		bool StablePenumbrae = true;
		/// Rebuild the whole shadow map in one frame when the sun jumps (wait, sleep, fast travel,
		/// sun/moon switch) or a new heightmap loads, instead of sweeping it in over ~1-2 seconds.
		/// Stands in for upstream #2617, which hooks the wait/sleep/travel events instead.
		bool RefreshOnSunJump = true;
		/// Upstream fc46f1a66: a worldspace that borrows its parent's land (Use Land Data) also
		/// uses the parent's heightmap. Upstream only did the loading half; both halves here.
		bool UseParentHeightmap = true;
	} settings;

	bool needPrecompute = false;
	uint shadowUpdateIdx = 0;
	/// (batch 40) Kernel / texture format the current shadow map was built with.
	bool builtStable = true;
	/// (batch 40) Sun direction the current sweep was set up with (RefreshOnSunJump).
	float3 sweepLightDir = { 0.f, 0.f, 0.f };
	bool sweepLightDirValid = false;
	/// (batch 40) Full refreshes done since load (shown in the debug section).
	uint fullRefreshCount = 0;

	struct HeightMapMetadata
	{
		std::wstring dir;
		std::string filename;
		std::string worldspace;
		float3 pos0, pos1;  // left-top-z=0 vs right-bottom-z=1
		float2 zRange;
	};
	std::unordered_map<std::string, HeightMapMetadata> heightmaps;
	HeightMapMetadata* cachedHeightmap = nullptr;

	struct ShadowUpdateCB
	{
		float2 LightPxDir;   // direction on which light descends, from one pixel to next via dda
		float2 LightDeltaZ;  // per LightUVDir, upper penumbra and lower, should be negative
		uint StartPxCoord;
		float2 PxSize;
		float BlendWeight;  // (batch 40) share of this update: 0.5 normally, 1 on a full refresh
		float2 PosRange;
		float2 ZRange;
	} shadowUpdateCBData;
	static_assert(sizeof(ShadowUpdateCB) % 16 == 0);
	std::unique_ptr<ConstantBuffer> shadowUpdateCB = nullptr;

	struct alignas(16) PerFrame
	{
		uint EnableTerrainShadow;
		float3 Scale;
		float2 ZRange;
		float2 Offset;
		// (batch 40) mirrors SharedData::TerraOccSettings
		float ZBlur;
		float SelfShadowBias;
		uint StablePenumbrae;
		float pad0;
	};
	STATIC_ASSERT_ALIGNAS_16(PerFrame);
	static_assert(sizeof(PerFrame) == 48);

	PerFrame GetCommonBufferData();

	winrt::com_ptr<ID3D11ComputeShader> shadowUpdateProgram = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> shadowUpdateProgramLegacy = nullptr;  ///< (batch 40) LEGACY_UPDATE kernel

	std::unique_ptr<Texture2D> texHeightMap = nullptr;
	std::unique_ptr<Texture2D> texShadowHeight = nullptr;

	bool IsHeightMapReady();

	virtual void SetupResources() override;
	void ParseHeightmapPath(std::filesystem::path p, bool xlodgen_style);
	void CompileComputeShaders();

	virtual void DrawSettings() override;

	virtual void EarlyPrepass() override;
	/// @brief Worldspace whose heightmap applies here (the parent when Use Land Data is set and UseParentHeightmap is on).
	RE::TESWorldSpace* GetHeightmapWorldspace() const;
	void LoadHeightmap();
	void Precompute();
	/// @brief One sweep step, or (a_refreshImmediately) the whole map at full weight. Returns false when nothing ran.
	bool UpdateShadow(bool a_refreshImmediately);

	virtual void ReflectionsPrepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual inline void RestoreDefaultSettings() override { settings = {}; }
	virtual void ClearShaderCache() override;
	virtual bool SupportsVR() override { return true; };
};