#pragma once

#include <d3d11_1.h>

#include "Buffer.h"
#include "GrassOptimizations/GrassBucketStore.h"
#include "GrassOptimizations/GrassRE.h"
#include "GrassOptimizations/HiZPyramid.h"

/**
 * @brief Rewrites vanilla grass rendering with a bucket based system utilizing indirect draws and compute shader per instance culling.
 *
 * Port of upstream Grass Optimizations (#2688 and its follow-up fixes). Differences from upstream:
 * - The optimized path can be switched on and off in-game (Settings::Enabled). Off, every grass shape is
 *   culled and drawn by the engine exactly as before; the GRASS_OPTIMIZATIONS shader permutation is selected
 *   per draw through ShaderCache::GrassShaderFlags::Optimized instead of being compiled into every grass shader.
 * - The vanilla per-instance fade buffer is still filled (upstream skipped it): the vanilla path reads it.
 * - Complex-grass detection stays per pixel in RunGrass.hlsl, as before, so there is no detection compute pass.
 */
struct GrassOptimizations : Feature
{
public:
	virtual inline std::string GetName() override { return "Grass Optimizations"; }
	virtual inline std::string GetShortName() override { return "GrassOptimizations"; }
	virtual inline std::string_view GetShaderDefineName() override { return "GRASS_OPTIMIZATIONS"; }
	virtual std::string_view GetCategory() const override { return "Grass"; }

	/** @brief Never adds the define globally: it is only compiled into the grass permutations selected by the Optimized descriptor flag. */
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return false; }

	/** @brief Returns a description and list of key features for the UI summary. */
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return { "Draws grass much faster: the GPU batches it and skips grass that is off-screen, too small or hidden.",
			{ "One draw per grass type instead of thousands of small draws",
				"Skips grass that is off-screen, too far or too small to see",
				"Skips grass hidden behind objects (occlusion culling)",
				"Grass render distance beyond the vanilla INI cap, thinned with distance",
				"Optional simpler models (LOD) and simpler shading for distant grass",
				"Can be switched off in-game for A/B comparison" } };
	};

	struct Settings
	{
		// Runtime switch for the optimized path. Off, grass is culled and drawn exactly as without the feature.
		bool Enabled = true;
		float MinPixelSize = 2.0f;
		float FullDetailPixelSize = 16.0f;
		float MinDensity = 0.03f;
		float MeshCostBias = 0.4f;
		float CostBiasStartDistance = 6000.0f;
		float InvisibleFadeCull = 0.0f;
		float RenderDistanceOverride = 0.0f;
		float EdgeFadeStart = 0.85f;
		bool EnableOcclusionCulling = true;
		float SimpleShadingPixelSize = 0.0f;
		float OcclusionBias = 0.001f;
		float CollisionDistance = 2048.0f;
		bool EnableMeshLOD = false;
		bool EnableMidLOD = true;
		float MidLODPixelSize = 8.0f;
		bool EnableFarLOD = true;
		float FarLODPixelSize = 4.0f;
		float MeshLODBandPixels = 3.0f;
	};

	Settings settings;

	/** @brief Draws the ImGui settings panel for grass optimizations configuration. */
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/** @brief Creates the constant buffers, bucket store resources and deferred context. */
	virtual void SetupResources() override;

	/** @brief Releases the cached compute shaders so they recompile on next use. */
	virtual void ClearShaderCache() override;

	/** @brief Installs the grass capture, culling and draw hooks after all plugins have loaded. */
	virtual void PostPostLoad() override;

	/** @brief Returns the instance culling compute shader, compiling it on first use. */
	ID3D11ComputeShader* GetCullCS();

	/** @brief Draws the per-frame grass statistics block of the Performance Overlay. */
	void DrawOverlayStats();

	struct alignas(16) CullParamsCB
	{
		float frustumPlanes[6][4];

		float minPixelSize;
		float fullDetailPixelSize;
		float lodMinKeep;
		float lodFadeBand;

		float meshCostBias;
		float projScale;
		float maxDistSq;
		float edgeFadeStart;

		float alphaParam1;
		float alphaParam2;
		float fadeNow;
		float fadeInTimeRcp;

		float invisibleFadeCull;
		float simpleShadingPixelSize;
		float collisionDistSq;
		float midLODPixelSize;

		float meshLODBandPx;
		float hiZEnabled;
		float hiZSizeX;
		float hiZSizeY;

		float hiZTexelPixels;
		float hiZMipCount;
		float occlusionBias;
		float costBiasStartDist;

		float farLODPixelSize;
		float pad0[3];
	};
	STATIC_ASSERT_ALIGNAS_16(CullParamsCB);

	struct alignas(16) CullBucketCB
	{
		uint32_t instanceCount;
		float wavePeriod;
		float timeBase;
		float prevTimeBase;
		float boundCenter[3];
		float modelRadius;
		float distScale;
		float minPixelScale;
		float isComplex;
		float midLODEnabled;
		uint32_t sliceTableOffset;
		uint32_t sliceCount;
		float farLODEnabled;
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(CullBucketCB);

	/** @brief The six frustum planes transposed to a structure of arrays, with two padding slots to fit optimized SSE/AVX instructions  */
	struct FrustumSoA
	{
		__m128 nx[2], ny[2], nz[2], d[2];
	};

	/** @brief Transposes the frustum once per frame. Inactive planes become always-pass slots. */
	static void BuildFrustumSoA(FrustumSoA& out, const RE::NiFrustumPlanes& f);

	/** @brief AABB vs frustum, corners passed as SIMD vectors (xyz in lanes 0-2). */
	static bool AabbVisible(const FrustumSoA& f, __m128 lo, __m128 hi);

	/** @brief Derives world-space frustum planes from the camera frustum and transform. */
	void ComputeFrustumPlanes(RE::NiFrustumPlanes& out, const RE::NiFrustum& viewFrustum, const RE::NiTransform& transform);

	/** @brief Once-per-frame grass update called in BSGrassShader::SetupGeometry: applies staged captures/removals, uploads dirty buckets and, when the optimized path is on, builds the Hi-Z pyramid and issues the culling dispatches. */
	void UpdateGrass();

	/** @brief Merges this bucket's slices into runs of contiguous buffer ranges that share a cell, for the per-bucket slice table. */
	void MergeSlicesIntoRuns(GrassBucket& b);

	/** @brief Appends this bucket's visible slice runs to sliceTableCPU and records the window in the bucket. */
	void CullBucketSlices(GrassBucket& b, const FrustumSoA& frustumSoA, __m128 camPosV);

	/** @brief Fills the per-bucket cull constant buffer, uploads the slice table and issues the cull dispatches. */
	void UploadCullState(ID3D11Device* device, ID3D11DeviceContext* ctx, uint32_t visibleBuckets);

	/** @brief Binds a bucket's resources and dispatches the instance culling compute shader. */
	void CullBucket(GrassBucket& b, ID3D11DeviceContext* ctx);

	/** @brief Grows the slotted per-bucket constant buffer to hold at least `slots` entries. */
	bool EnsureCullBucketCapacity(uint32_t slots, ID3D11Device* device);

	/**
	 * @brief Decides whether the optimized permutations for every grass technique seen so far are compiled.
	 * Requests the missing ones as a side effect, so the shader cache compiles them in the background.
	 */
	bool OptimizedShadersReady();

	/** @brief Records a grass technique's descriptors so its optimized permutation can be requested ahead of use. */
	void NoteTechnique(RE::BSShader* shader, uint32_t vertexDescriptor, uint32_t pixelDescriptor, bool hasPixelShader);

	/** @brief True while the Performance Overlay is on screen, which is the only time statistics are gathered. */
	static bool StatsWanted();

	/** @brief Closes the previous frame's statistics when a new frame starts. */
	void RollStats(uint32_t frame);

	/** @brief Queues this frame's GPU-surviving instance counts for a delayed, non-blocking readback. */
	void GatherDrawnInstanceCounts(ID3D11DeviceContext* ctx);

	/** @brief Folds any completed drawn-instance readback into the statistics. Never waits on the GPU. */
	void CollectDrawnInstanceCounts(ID3D11DeviceContext* ctx);

	GrassBucketStore bucketStore;
	HiZPyramid hiZ;

	uint32_t lastFrame = UINT32_MAX;

	ID3D11DeviceContext1* ctx1 = nullptr;

	ID3D11ComputeShader* cullCS = nullptr;
	// Set on a failed GetCullCS() compile so the per-frame caller doesn't retry the compile and
	// re-log the failure every frame; cleared by ClearShaderCache() to allow a retry.
	bool cullCSFailed = false;

	std::unique_ptr<ConstantBuffer> cullParamsCB;
	// Slotted per-bucket constants bound via CSSetConstantBuffers1: one 256-byte slot per visible
	// bucket, one map fills them all, recreated when the bucket count outgrows it.
	std::unique_ptr<ConstantBuffer> cullBucketCB;
	uint32_t cullBucketCBSlots = 0;
	static constexpr uint32_t kSlotBytes = 256;

	// Shared per-frame table of visible slice ranges, indexed by each bucket's window.
	std::unique_ptr<Buffer> sliceTable;
	uint32_t sliceTableCapacity = 0;
	std::vector<std::pair<uint32_t, uint32_t>> sliceTableCPU;

	float timeAccum = 0.0f;
	float fadeInTimeRcp = 0.0f;
	float timeBase = 0.0f;
	float prevTimeBase = 0.0f;
	float grassStartFadeDistance = 0.0f;
	float vanillaMaxDistance = 0.0f;
	float maxGrassDistance = 0.0f;
	float maxDistSq = 0.0f;

	// Read by the OnVisible hook on the engine's culling threads. Set once per frame by UpdateGrass for the
	// next frame's culling, and read there first to learn how this frame was culled, so the draws always
	// match what culling did, including on the frame the switch is flipped.
	std::atomic<bool> collapseActive{ false };
	// The frame whose culling folded buckets and whose cull dispatches were issued; only that frame's draws
	// look buckets up at all. Every other frame is drawn entirely by the game.
	uint32_t optimizedDrawFrame = UINT32_MAX;

	// Every (vertex, pixel) grass descriptor pair seen at draw time. The optimized path is only engaged once
	// all of them have a compiled optimized permutation, so switching it on never leaves a technique without
	// a shader for a frame.
	struct TechniqueKey
	{
		uint32_t vertexDescriptor;
		uint32_t pixelDescriptor;
		bool hasPixelShader;
		bool operator==(const TechniqueKey&) const = default;
	};
	std::vector<TechniqueKey> seenTechniques;
	TechniqueKey lastTechnique{ UINT32_MAX, UINT32_MAX, false };
	std::mutex techniqueMutex;
	// The engine's grass shader, captured at draw time; the optimized permutations are looked up against it.
	RE::BSShader* grassShader = nullptr;
	bool shadersReady = false;

	/** @brief One frame's grass counters, shown by the Performance Overlay. */
	struct FrameStats
	{
		uint32_t frame = UINT32_MAX;
		bool optimizedPath = false;     // the optimized path drew this frame
		bool waitingForShaders = false;  // enabled, but optimized permutations are still compiling
		uint32_t engineDraws = 0;        // per-group engine instanced draws (vanilla path and fallbacks)
		uint64_t engineInstances = 0;    // instances submitted by those draws
		uint32_t indirectDraws = 0;      // DrawIndexedInstancedIndirect calls issued by the optimized path
		uint32_t bucketsTotal = 0;       // grass types with captured instances
		uint32_t bucketsVisible = 0;     // grass types that survived the CPU slice cull
		uint64_t instancesResident = 0;  // captured instances held on the GPU
		uint64_t instancesTested = 0;    // instances dispatched to the GPU cull
	};
	FrameStats currentStats;
	FrameStats lastStats;

	// Drawn-instance readback: per visible bucket the main count and both LOD-tier counts are copied into
	// gatherBuffer, then into one ring slot read back a few frames later without stalling.
	static constexpr uint32_t kReadbackSlots = 4;
	static constexpr uint32_t kCountsPerBucket = 3;
	struct ReadbackSlot
	{
		winrt::com_ptr<ID3D11Buffer> staging;
		uint32_t capacity = 0;
		uint32_t count = 0;
		bool pending = false;
	};
	std::array<ReadbackSlot, kReadbackSlots> readbackSlots;
	uint32_t readbackWrite = 0;
	winrt::com_ptr<ID3D11Buffer> gatherBuffer;
	uint32_t gatherCapacity = 0;
	uint64_t drawnInstances = 0;
	bool drawnInstancesValid = false;

	struct Hooks
	{
		struct BSMultiStreamInstanceTriShape_dtor
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSMultiStreamInstanceTriShape_OnVisible
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* This, RE::NiCullingProcess* process, std::int32_t alphaGroupIndex);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DoneAddingInstances
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* geometry, RE::BSTArray<std::uint32_t>& a_instances);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSGrassShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* a2, std::uint32_t flags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddQueuedGroupGIDBuffer
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, GrassRE::GroupHeader* a2, std::uint16_t* a3, RE::BSTArray<std::uint32_t>& a4);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroupGIDBuffer
		{
			static std::uint32_t thunk(RE::BSMultiStreamInstanceTriShape* a1, GrassRE::GroupHeader* a2, std::uint16_t* a3);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ReadGroupHeaderStreamTraits
		{
			static void thunk(RE::BSStreamHeader* streamHeader, GrassRE::GroupHeader* groupHeader, uint32_t size);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct ReadInstanceGroupStreamTraits
		{
			static void thunk(RE::BSStreamHeader* streamHeader, uint16_t* groupHeader, uint32_t size);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroupQueuedGIDFile
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2, RE::BSTArray<std::uint32_t>& a3);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct AddGroupGIDFile
		{
			static void thunk(RE::BSMultiStreamInstanceTriShape* a1, RE::BSStream* a2);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DrawInstanceTriShape
		{
			static void thunk(RE::BSRenderPass* curPass, RE::BSMultiStreamInstanceTriShape* geometry);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct LoadGrassType
		{
			static RE::BSMultiStreamInstanceTriShape* thunk(RE::BGSGrassManager* grassManager, RE::GrassParam* a_param, uint32_t CellXDivided, uint32_t CellYDivided, uint64_t* typeKey, RE::BSFixedString* modelPath);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x0, BSMultiStreamInstanceTriShape_dtor>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x34, BSMultiStreamInstanceTriShape_OnVisible>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);
			stl::write_vfunc<0x3A, DoneAddingInstances>(RE::VTABLE_BSMultiStreamInstanceTriShape[0]);

			stl::write_vfunc<0x6, BSGrassShader_SetupGeometry>(RE::VTABLE_BSGrassShader[0]);

			// Capture raw instance data for cached grass.
			stl::write_thunk_call<AddQueuedGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x7FF, 0x756));
			stl::write_thunk_call<AddGroupGIDBuffer>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x806, 0x75D));
			stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74599, 76327).address() + REL::Relocate(0x36, 0x36));
			stl::write_thunk_call<ReadGroupHeaderStreamTraits>(REL::RelocationID(74596, 76324).address() + REL::Relocate(0x2F, 0x33));
			stl::write_thunk_call<ReadInstanceGroupStreamTraits>(REL::RelocationID(74607, 76339).address() + REL::Relocate(0xCF, 0xCF));
			stl::write_thunk_call<AddGroupQueuedGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x394, 0x384));
			stl::write_thunk_call<AddGroupGIDFile>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x39B, 0x38B));

			// Record each grass type's source .nif path alongside its shape.
			stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15204, 15372).address() + REL::Relocate(0x2F5, 0x2F5));
			stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15205, 15373).address() + REL::Relocate(0x62B, 0x597));
			stl::write_thunk_call<LoadGrassType>(REL::RelocationID(15206, 15374).address() + REL::Relocate(0x25C, 0x25C));

			// Replace the engine's per-instance-group draw loop with a call that either reissues it
			// (VanillaDrawInstanceTriShape) or draws the whole bucket with indirect draws.
			std::uint8_t patch[] = { 0x4C, 0x89, 0xF2 };  // mov rdx, r14
			REL::safe_write(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x660, 0x648), patch, sizeof(patch));
			stl::write_thunk_call<DrawInstanceTriShape>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x663, 0x64B));
			SKSE::AllocTrampoline(14);
			SKSE::GetTrampoline().write_branch<5>(REL::RelocationID(100847, 107637).address() + REL::Relocate(0x668, 0x650), REL::RelocationID(100847, 107637).address() + REL::Relocate(0x759, 0x73A));

			// Upstream also skipped mapping the vanilla per-instance fade buffer here. That buffer is
			// read by the vanilla grass vertex shader, which this port keeps using whenever the
			// optimized path is off, so the engine is left to fill it.

			logger::info("[GRASS OPTIMIZATIONS] Installed hooks");
		}
	};
};
