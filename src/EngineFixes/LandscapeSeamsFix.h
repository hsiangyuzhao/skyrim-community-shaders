#pragma once

#include "EngineFix.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

/**
 * @brief (49d) Landscape Seams: blends land textures across quad borders and lets a quad borrow up to two of its
 * neighbours' textures at the border. Port of official Community Shaders #2841 (from InTheBottle's Bottled-Shaders),
 * with jiayev's 01f2e838d (compile fix) and the precompile of #2917.
 *
 * Differences from upstream:
 *  - No hooks of its own. TESObjectLAND::SetupMaterial (Hooks.cpp), BSLightingShaderProperty::GetRenderPasses and
 *    BSLightingShader::SetupGeometry (TruePBR.cpp) already have one detour each in this fork; they call in here.
 *  - All weight work (border healing, the 17x17x8 weight maps, the quad data buffer, loading the borrowed textures)
 *    runs on one worker thread of its own, fed by the land setup hook. The setup hook only copies the quad's weights
 *    and slots (a few KB). Until the worker has built a quad's resources the quad draws as normal terrain.
 *  - Runtime switch "Blend Terrain Seams" (Advanced > PBR Settings). Off: the seams bit is never set, which draws
 *    exactly what 49b drew. The worker still prepares quads while off so that switching on is instant.
 *  - Slots t104-t111 (upstream t104 before jiayev moved them to t117 for his physical sky, which this fork does not have).
 */
struct LandscapeSeamsFix : EngineFix
{
	std::string GetName() override { return "Landscape Seams Fix"; }

	void Install() override;

	/** @brief Requests the seams pixel shaders of the vanilla landscape permutations so they precompile (#2917). */
	void GenerateShaderPermutations(RE::BSShader* a_shader) override;

	static LandscapeSeamsFix& GetInstance();

	static constexpr uint32_t GridSize = 17;
	static constexpr uint32_t GridVertices = GridSize * GridSize;
	static constexpr uint32_t PerimeterVertices = 64;
	static constexpr uint32_t EngineLayers = 6;
	static constexpr uint32_t MaxExtraLayers = 2;
	static constexpr uint32_t MaxLayers = EngineLayers + MaxExtraLayers;
	static constexpr uint32_t TexturesPerExtra = 3;
	static constexpr uint32_t FirstPSTexture = 104;
	static constexpr uint32_t NumPSTextures = 2 + MaxExtraLayers * TexturesPerExtra;

	static constexpr uint32_t BlendRadius = 3;

	struct QuadKey
	{
		uint32_t worldSpace = 0;
		int32_t x = 0;
		int32_t y = 0;

		bool operator==(const QuadKey&) const = default;
	};

	struct QuadKeyHash
	{
		size_t operator()(const QuadKey& a_key) const noexcept
		{
			uint64_t hash = a_key.worldSpace;
			hash = hash * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(a_key.x);
			hash = hash * 0x9E3779B97F4A7C15ull + static_cast<uint32_t>(a_key.y);
			return static_cast<size_t>(hash ^ (hash >> 32));
		}
	};

	using WeightGrid = std::array<std::array<uint8_t, GridVertices>, EngineLayers>;
	using Border = std::array<std::array<uint8_t, PerimeterVertices>, EngineLayers>;

	/** Mirrors LandscapeSeams::QuadData in Common/LandscapeSeams.hlsli (structured buffer, tightly packed). */
	struct alignas(16) QuadData
	{
		float2 Origin;
		uint32_t Flags;
		uint32_t ExtraCount;
		float2 IsSnow;
		float2 SpecPower;
		float4 PBRParams[MaxExtraLayers];
		float4 GlintParams[MaxExtraLayers];
	};
	static_assert(sizeof(QuadData) == 96);

	struct Resources
	{
		winrt::com_ptr<ID3D11ShaderResourceView> weights;
		winrt::com_ptr<ID3D11ShaderResourceView> data;
		std::array<std::array<RE::NiSourceTexturePtr, TexturesPerExtra>, MaxExtraLayers> extras;
	};

	struct Quad
	{
		std::array<RE::TESLandTexture*, EngineLayers> slots{};
		Border border{};
		std::unique_ptr<WeightGrid> grid;
		RE::BSGeometry* geometry = nullptr;
		std::shared_ptr<Resources> resources;
		uint64_t generation = 0;
		bool pbr = false;
	};

	struct Healed
	{
		QuadKey key;
		uint64_t generation = 0;
		bool active = false;
		bool pbr = false;
		uint32_t extraCount = 0;
		std::array<RE::TESLandTexture*, MaxExtraLayers> extras{};
		std::array<std::array<uint8_t, GridVertices>, MaxLayers> weights{};
	};

	/** @brief Releases land geometry the engine has dropped. Once per frame (State::Reset); cheap, never blocks. */
	void Reset();
	/** @brief True while the switch is on and this land quad geometry has blend resources. Any thread (GetRenderPasses). */
	bool ShouldTag(RE::BSGeometry* a_geometry);
	/** @brief Binds the quad's weights, quad data and borrowed textures to t104-t111. Render thread, before the draw. */
	void Bind(RE::BSGeometry* a_geometry);
	/**
	 * @brief Copies a newly set up land record's quads and queues them and their neighbours for the worker.
	 * @param a_pbr True when True PBR gave this land its PBR material (TruePBR::TESObjectLAND_SetupMaterial returned true).
	 */
	void OnLandSetup(RE::TESObjectLAND* a_land, bool a_pbr);

	bool IsEnabled() const { return enabled.load(std::memory_order_relaxed); }
	void Load(const nlohmann::json& a_json);
	nlohmann::json Save() const;
	/** @brief F11 > Advanced > PBR Settings, top: the switch and a status line. */
	void DrawSettings();

private:
	LandscapeSeamsFix() = default;

	bool IsBlended(RE::BSGeometry* a_geometry);
	void Heal(const QuadKey& a_key, const Quad& a_quad, Healed& a_out) const;
	std::shared_ptr<Resources> CreateResources(const Healed& a_healed) const;
	void Rebuild(const std::vector<QuadKey>& a_keys);
	void Enqueue(const std::vector<QuadKey>& a_keys);
	void WorkerLoop(std::stop_token a_stop);

	std::atomic<bool> enabled{ true };

	std::shared_mutex mutex;
	std::unordered_map<QuadKey, Quad, QuadKeyHash> quads;
	std::unordered_map<RE::BSGeometry*, QuadKey> loadedQuads;
	std::vector<RE::BSGeometry*> retiredGeometry;
	uint32_t currentWorldSpace = 0;

	// Worker: keys waiting for Rebuild, coalesced.
	std::mutex queueMutex;
	std::condition_variable_any queueCondition;
	std::vector<QuadKey> queuedKeys;
	std::jthread worker;

	// Status line (menu only).
	std::atomic<uint32_t> rebuiltQuads{ 0 };
	std::atomic<double> lastRebuildMs{ 0.0 };
};
