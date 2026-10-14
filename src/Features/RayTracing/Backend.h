#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <d3d11_4.h>
#include <d3d12.h>
#include <wrl/client.h>

/**
 * @brief (45, RT prototype) The ray tracer's own D3D12 side: device, queue, BLAS/TLAS, trace pass.
 *
 * Interop follows Upscaling/NeuralRendering/D3D12Interop.cpp: textures are created on D3D11 with
 * NT shared handles and opened on D3D12, and one shared fence orders the two queues -- D3D11
 * signals, the D3D12 queue waits on the GPU, D3D12 signals, the D3D11 queue waits on the GPU.
 * The CPU never waits for the GPU: when every command context is still in flight the frame's
 * trace is skipped (counted in stats) instead.
 *
 * Note: D3D12CreateDevice returns one device object per adapter and process, so "our" device is
 * the same object Streamline / DLSS-G and Neural Rendering use; everything of ours on it (queue,
 * lists, fence, resources) is separate. The device is created on first use, never with Ray
 * Tracing off.
 *
 * Render thread only.
 */
namespace RayTracingRT
{
	using Microsoft::WRL::ComPtr;

	/// Triangles of one BLAS, positions relative to the owner's origin (a cell corner).
	struct ChunkGeometry
	{
		std::vector<float> positions;  // xyz
		std::vector<std::uint32_t> indices;
		/// (45h) Alpha-tested mesh: two uints per triangle corner, in index order (x = the vertex's
		/// half2 texcoord as stored by the engine, y = its vertex alpha as float bits). Non-empty =
		/// the BLAS geometry is non-opaque and its triangles are tested against their cut-out.
		std::vector<std::uint32_t> alphaUV;
		[[nodiscard]] std::uint32_t Triangles() const { return static_cast<std::uint32_t>(indices.size() / 3); }
		[[nodiscard]] std::uint32_t Vertices() const { return static_cast<std::uint32_t>(positions.size() / 3); }
		[[nodiscard]] bool Opaque() const { return alphaUV.empty(); }
	};

	using BlasHandle = std::uint32_t;
	inline constexpr BlasHandle kInvalidBlas = UINT32_MAX;

	// (45h) Alpha-tested casters: a texture registered for the any-hit test, and the per-shape
	// record (texture, threshold, texcoord transform) the shader finds through the instance ID.
	using AlphaTexture = std::uint32_t;
	inline constexpr AlphaTexture kInvalidAlphaTexture = UINT32_MAX;
	inline constexpr std::uint32_t kInvalidAlphaInfo = UINT32_MAX;

	struct AlphaMaterial
	{
		float threshold = 0.5f;   // pass when alpha >= threshold (0-1)
		float alphaScale = 1.0f;  // material alpha
		float uvTransform[4] = { 0.0f, 0.0f, 1.0f, 1.0f };  // offset xy, scale zw
	};

	struct AlphaStats
	{
		std::uint32_t texturesShared = 0;    // full-resolution game textures read by D3D12 directly
		std::uint32_t texturesFallback = 0;  // 256x256 alpha copies
		std::uint32_t texturesPending = 0;   // alpha copies not made yet
		std::uint64_t notShareable = 0;      // textures not created shareable (loaded before the switch, refused): copy instead
		std::uint64_t openFailed = 0;        // D3D12 could not open a shared texture: copy instead
		std::uint64_t slotsFull = 0;         // bindless table full: copy instead
		std::uint64_t layersFull = 0;        // copy layers full: shape not ray traced
		std::uint64_t uvFull = 0;            // texcoord buffer full: shape not ray traced
		std::uint64_t infosFull = 0;
		std::uint32_t infos = 0;             // live per-shape records
		std::uint32_t instancesLastFrame = 0;
		std::uint64_t uvBytesUsed = 0;
		std::uint64_t uvBytes = 0;           // texcoord buffer (allocated on first use)
		std::uint64_t layerBytes = 0;        // 256x256 copy layers (allocated on first use)
		std::uint64_t infoBytes = 0;         // per-context upload copies of the records
		bool sharedPathOff = false;          // D3D12 never opened one: only copies this session
	};

	// (45i) Skinned characters: a shape's vertices in the mesh's own space with their bone indices
	// and weights, skinned every frame on our queue (SkinCharactersCS) into a vertex buffer whose
	// BLAS is refit (ALLOW_UPDATE / PERFORM_UPDATE) and rebuilt now and then.
	struct SkinVertex
	{
		float position[3];
		std::uint32_t bones;  // 4 x u8 bone index (into the shape's bone list)
		float weights[4];     // normalized
	};
	static_assert(sizeof(SkinVertex) == 32);

	struct SkinGeometry
	{
		std::vector<SkinVertex> vertices;
		std::vector<std::uint32_t> indices;
		std::vector<std::uint32_t> alphaUV;  // as ChunkGeometry::alphaUV (alpha-tested parts: hair)
		std::uint32_t boneCount = 0;
		[[nodiscard]] std::uint32_t Triangles() const { return static_cast<std::uint32_t>(indices.size() / 3); }
		[[nodiscard]] std::uint32_t Vertices() const { return static_cast<std::uint32_t>(vertices.size()); }
		[[nodiscard]] bool Opaque() const { return alphaUV.empty(); }
	};

	using SkinHandle = std::uint32_t;
	inline constexpr SkinHandle kInvalidSkin = UINT32_MAX;

	/// One skinned shape in this frame's TLAS: its bones are FrameInput::skinBones[boneOffset ..]
	/// (12 floats each: row-major 3x4, translation relative to x, y, z).
	struct SkinDraw
	{
		SkinHandle shape = kInvalidSkin;
		double x = 0.0, y = 0.0, z = 0.0;
		std::uint32_t boneOffset = 0;
		std::uint32_t alphaInfo = UINT32_MAX;  // alpha-tested parts: the record (CreateSkinAlphaInfo)
		bool npc = false;    // (45l) not the player: instance mask kNpcMask
		bool reuse = false;  // (45l) not skinned / refit this frame: last frame's BLAS (x / y / z = the origin it was skinned at)
	};

	struct SkinStats
	{
		std::uint32_t shapes = 0;             // live skinned shapes
		std::uint32_t pending = 0;            // waiting for their upload
		std::uint64_t vertices = 0;           // of the live shapes
		std::uint64_t triangles = 0;
		std::uint32_t drawsLastFrame = 0;     // skinned and in the TLAS last frame
		std::uint64_t verticesLastFrame = 0;  // skinned last frame
		std::uint32_t refitsLastFrame = 0;
		std::uint32_t buildsLastFrame = 0;    // first builds and periodic rebuilds
		std::uint32_t notReadyLastFrame = 0;  // draws not traced (not uploaded / built yet, build cap)
		std::uint32_t reusedLastFrame = 0;    // (45l) draws traced with last frame's skinning (other actors, reduced quality)
		std::uint64_t buildsTotal = 0;
		std::uint64_t bytes = 0;              // rest, index and vertex buffers + BLASes (live shapes)
		std::uint64_t scratchBytes = 0;
		std::uint64_t boneBytes = 0;          // per-context bone matrix uploads
		std::uint64_t overBudget = 0;         // shapes refused: VRAM budget
		std::uint64_t refused = 0;            // shapes refused: too big / bad data
	};

	struct TraceConstants
	{
		float invViewProj[16];
		float toSun[4];      // xyz, w = tan(cone half angle)
		float params[4];     // rays, frame, flags (bit pattern), max ray length
		float renderSize[4]; // w, h, 1/w, 1/h
		float rect[4];       // camera-relative xy min / max
		float bias[4];       // constant, per unit distance, coverage probe
		float world[4];      // (45j) camera world position (world-locked noise)
		float extra[4];      // (45k) x adaptive extra rays; (45l) y / z distance fall-off start / end (units), w pattern bits
		float spheres[8];    // (45l) character bounds, camera-relative xyz + radius: [0..3] player, [4..7] the other actors (r 0 = none)
	};
	static_assert(sizeof(TraceConstants) == 52 * 4);

	/// (45f) What a TLAS instance holds, for the per-category counts and the self-check.
	enum InstanceCategory : std::uint8_t
	{
		kCategoryCell,     // a chunk of a captured cell (static / furniture / container references)
		kCategoryMovable,  // doors, activators, movable statics, lights
		kCategoryItem,     // loose items
		kCategoryTree,     // opaque parts of trees and plants
		kCategoryAlpha,    // (45h) alpha-tested shapes traced with their cut-out (leaves, banners, fences)
		kCategoryCharacter,  // (45i) skinned character shapes
		kInstanceCategories
	};
	inline constexpr const char* kInstanceCategoryNames[kInstanceCategories] = { "cell chunk", "door/activator/movable", "item", "tree/plant",
		"alpha-tested", "character" };

	struct Instance
	{
		BlasHandle blas;
		double x, y, z;  // world translation of the BLAS origin
		// (45e) Rotation * scale, row-major (row r = world axis r); identity for cell chunks.
		float m[3][3] = { { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } };
		InstanceCategory category = kCategoryCell;
		// (45f) Self-check: the world box the engine's own bounds give for this instance's shapes
		// (live worldBound spheres). The instance as written into the TLAS must lie inside it.
		bool hasExpected = false;
		double expectMin[3]{}, expectMax[3]{};
		// (45g) For the self-check log: the owning reference and shape name (objects only).
		std::uint32_t refID = 0;
		const char* label = nullptr;
		// (45h) Alpha-tested: the per-shape record (CreateAlphaInfo); the instance is non-opaque.
		std::uint32_t alphaInfo = kInvalidAlphaInfo;
	};

	struct FrameInput
	{
		TraceConstants constants{};
		std::uint32_t renderWidth = 0;
		std::uint32_t renderHeight = 0;
		double cameraX = 0.0, cameraY = 0.0, cameraZ = 0.0;
		const std::vector<Instance>* instances = nullptr;
		std::uint32_t buildTriangleBudget = 0;
		bool validate = false;  // (45f) run the TLAS self-check on this frame's instances
		// (45i) Skinned characters (null / empty = none: the 45h pipelines run).
		const std::vector<SkinDraw>* skinDraws = nullptr;
		const std::vector<float>* skinBones = nullptr;
		std::uint32_t maxBuilds = 0;  // (45k) BLAS builds / compactions this frame (0 = the default cap)
		// (45l) Builds, skinning and the TLAS in their own submission that does not wait for D3D11
		// (only the trace does); false = one list behind the D3D11 wait (45k).
		bool splitSubmit = true;
		// (45l) The D3D11 queue's wait for the trace is left to WaitForTrace() (called right before the
		// first D3D11 pass that reads the result); false = waited at the end of ExecuteFrame (45k).
		bool deferWait = false;
	};

	/// (45k) Where the CPU time of the last ExecuteFrame went, call by call (ms). The sum of these
	/// is cpuRecordMs + cpuBuildMs + cpuSkinMs; a GPU-progress wait shows up in exactly one of them.
	struct ExecuteSplit
	{
		float collect = 0;    // retired resources, timestamps, context choice
		float signal11 = 0;   // ID3D11DeviceContext4::Signal (ready)
		float flush11 = 0;    // ID3D11DeviceContext::Flush
		float wait12 = 0;     // ID3D12CommandQueue::Wait (ready)
		float reset = 0;      // command allocator + list Reset
		float tlas = 0;       // TLAS instance descriptions (incl. instance buffer)
		float alpha = 0;      // alpha record copy
		float record = 0;     // TLAS build, barriers, dispatch, close
		float execute = 0;    // ExecuteCommandLists
		float signal12 = 0;   // ID3D12CommandQueue::Signal (done)
		float wait11 = 0;     // ID3D11DeviceContext4::Wait (done)
		bool flushSkipped = false;
		// (45n) Inside collect: the retired objects the GPU is done with (released here, or only handed to the
		// release thread), and how many there were.
		float retire = 0;
		std::uint32_t retiredCount = 0;
		bool early = false;  // (45n) builds / skinning were recorded and submitted at the frame start (PrepareFrame)
	};

	/// (45f) TLAS contents and the placement self-check.
	struct TlasCheck
	{
		std::uint32_t written[kInstanceCategories]{};   // instances written into this frame's TLAS
		std::uint32_t notBuilt[kInstanceCategories]{};  // instances skipped: BLAS not built yet
		std::uint32_t checked = 0;                      // instances compared with the engine's bounds (last check)
		std::uint32_t mismatched[kInstanceCategories]{};  // ... and found outside them (last check)
		std::uint64_t mismatchedTotal = 0;
		std::uint32_t logged = 0;
		// (45g) Each misfit (reference + BLAS) is logged again at most every 30 s, 200 lines in total.
		std::unordered_map<std::uint64_t, std::int64_t> lastLogged;
	};

	struct Stats
	{
		std::uint32_t blasCount = 0;        // live BLASes with GPU memory
		std::uint32_t blasQueued = 0;       // waiting to be built
		std::uint64_t blasTriangles = 0;    // triangles in built BLASes
		std::uint64_t blasBytes = 0;
		std::uint64_t scratchBytes = 0;
		std::uint64_t tlasBytes = 0;
		std::uint64_t uploadBytes = 0;      // instance rings and in-flight geometry uploads
		std::uint64_t sharedTextureBytes = 0;
		std::uint32_t instancesLastFrame = 0;
		std::uint32_t buildsLastFrame = 0;
		std::uint32_t compactionsLastFrame = 0;
		std::uint64_t buildsTotal = 0;
		std::uint64_t compactedSavedBytes = 0;
		std::uint32_t skippedFramesBusy = 0;  // every command context still in flight
		float lastGpuMs = 0.0f;               // trace + builds on our queue, from D3D12 timestamps
		float smoothedGpuMs = 0.0f;
		float lastPrepGpuMs = -1.0f;          // (45l) of which builds, skinning, TLAS (the part that does not wait for D3D11)
		float lastTraceGpuMs = -1.0f;         // (45l) of which the trace dispatch
		// (45n) Our queue standing idle between the end of the builds / skinning and the start of the trace: it
		// was waiting for the game's depth copy (> 0 = the builds were not what the game queue waited for).
		float lastQueueGapMs = -1.0f;
		float earlyCpuMs = 0.0f;              // (45n) CPU of the last PrepareFrame (frame start)
		std::uint32_t retiredLastCollect = 0; // (45n) objects released (or handed to the release thread) by the last collect
		float retireLastMs = 0.0f;            // (45n) render-thread ms of that
		// (45j) CPU of the last ExecuteFrame, split: BLAS / compaction resource creation and uploads,
		// character uploads / skinning records, everything else (TLAS instances, record, submit).
		float cpuBuildMs = 0.0f;
		float cpuSkinMs = 0.0f;
		float cpuRecordMs = 0.0f;
		ExecuteSplit split{};  // (45k)
		std::uint32_t loadingBuilds = 0;  // (45k) BLAS builds submitted by BuildOnly (loading screens) since start
	};

	class Backend
	{
	public:
		~Backend();

		/// Creates the device, queue, fence, pipeline. On failure fills o_reason and returns false.
		bool Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string& o_reason);
		/// Releases everything. a_waitForGpu: a bounded CPU wait so resources can go now (exit / disable only).
		void Shutdown(bool a_waitForGpu);
		[[nodiscard]] bool IsReady() const { return ready_; }

		/// Shared textures sized to the game's shadow mask. Recreated when the size changes.
		bool EnsureTextures(std::uint32_t a_width, std::uint32_t a_height, std::string& o_reason);
		[[nodiscard]] ID3D11UnorderedAccessView* DepthMaskUAV() const { return depthMask_.uav11.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* DepthMaskSRV() const { return depthMask_.srv11.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* VisibilitySRV() const { return visibility_.srv11.Get(); }

		/// Queues a BLAS build for this geometry (takes it). Built over the next frames.
		BlasHandle CreateBlas(std::unique_ptr<ChunkGeometry> a_geometry);
		/// The BLAS goes away once the GPU is done with every submission that may use it.
		void ReleaseBlas(BlasHandle a_handle);
		void ReleaseAllBlas();
		[[nodiscard]] bool IsBlasBuilt(BlasHandle a_handle) const;

		/// Records and submits this frame's D3D12 work between two GPU-side fence waits.
		/// Returns false when nothing was traced (busy ring or a failure; see LastError()).
		bool ExecuteFrame(const FrameInput& a_input);
		/// (45n) Early Prep: at the start of the frame (before the shadow maps) records and submits this frame's
		/// BLAS builds, compactions, character uploads and skinning (a_input.skinDraws / skinBones / build budget)
		/// on a command context it keeps for the frame; the next ExecuteFrame then records only the TLAS and the
		/// trace on it. Nothing waits (CPU or GPU). False = nothing submitted (ExecuteFrame then does everything).
		bool PrepareFrame(const FrameInput& a_input);
		/// (45n) A frame prepared by PrepareFrame that will not trace: the context is released with a D3D12
		/// signal (no D3D11 involvement). No-op when nothing is prepared.
		void AbandonPrepared();
		[[nodiscard]] bool HasPrepared() const { return prepared_; }
		/// (45n) Release Thread: retired D3D12 objects the GPU is done with are released on a worker thread
		/// (freeing committed resources costs about 0.5 ms per character shape on the render thread).
		void SetReleaseOnWorker(bool a_worker) { releaseOnWorker_ = a_worker; }
		/// (45n) Worker: ms of the last batch released and objects released since start.
		[[nodiscard]] float LastWorkerReleaseMs() const { return workerReleaseMs_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t WorkerReleased() const { return workerReleased_.load(std::memory_order_relaxed); }
		/// (45l) With FrameInput::deferWait: the D3D11 queue waits (on the GPU) for the last trace now.
		/// No-op when nothing is pending. Returns the CPU ms of the call.
		float WaitForTrace();
		[[nodiscard]] bool TraceWaitPending() const { return pendingWait_ != 0; }
		/// (45k) Loading screens: BLAS builds and compactions only (no trace, no D3D11 interop), with a
		/// larger cap. Never waits; false when no command context is free or nothing is queued.
		bool BuildOnly(std::uint32_t a_triangleBudget, std::uint32_t a_maxBuilds);
		/// (45k) Explicit D3D11 Flush after the ready Signal (45j: always). Off = the Signal alone.
		void SetExplicitFlush(bool a_flush) { explicitFlush_ = a_flush; }
		[[nodiscard]] std::uint32_t QueuedBuilds() const { return static_cast<std::uint32_t>(buildQueue_.size()); }

		/// (45e) True when ExecuteFrame will find a free command context. Only the GPU completing
		/// work can change the answer from false to true, never back, so a yes at the start of the
		/// frame still holds when the frame's trace is recorded.
		[[nodiscard]] bool HasFreeContext() const
		{
			if (!ready_ || failed_ || !fence_)
				return false;
			const std::uint64_t completed = fence_->GetCompletedValue();
			for (const auto& context : contexts_)
				if (context.fenceValue == 0 || context.fenceValue <= completed)
					return true;
			return false;
		}

		/// Frees retired resources the GPU is done with (also while nothing is traced).
		void Tick()
		{
			if (ready_)
				CollectRetired();
		}

		// (45h) Alpha-tested casters. All of it is created on first use only (never with the switch off).
		/// The alpha variant of the trace pipeline exists (the driver accepted it).
		[[nodiscard]] bool AlphaAvailable() const { return ready_ && !failed_ && alphaPipeline_ && alphaFault_.empty(); }
		/// Registers the texture a shape's cut-out comes from: the game's texture itself when it is
		/// shareable, else a 256x256 alpha copy made on D3D11 (RecordAlphaCopies). Counted per user.
		AlphaTexture AcquireAlphaTexture(ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_view);
		void ReleaseAlphaTexture(AlphaTexture a_texture);
		[[nodiscard]] bool IsAlphaTextureReady(AlphaTexture a_texture) const;
		/// The per-shape record; a_blas must be an alpha BLAS (CreateBlas with alphaUV).
		std::uint32_t CreateAlphaInfo(BlasHandle a_blas, AlphaTexture a_texture, const AlphaMaterial& a_material);
		void ReleaseAlphaInfo(std::uint32_t a_info);
		/// D3D11, before ExecuteFrame: makes up to a_max pending alpha copies with a_shader (AlphaMaskCS).
		void RecordAlphaCopies(ID3D11ComputeShader* a_shader, std::uint32_t a_max);
		/// Nothing uses the alpha resources any more (switch off and the scene released).
		[[nodiscard]] bool AlphaIdle() const { return alphaTextureIndex_.empty() && alphaInfoLive_ == 0 && alphaUvUsed_ == 0; }
		void ReleaseAlphaResources();
		/// Set when an alpha resource could not be created: the feature turns the switch off.
		[[nodiscard]] const std::string& AlphaFault() const { return alphaFault_; }
		void ClearAlphaFault() { alphaFault_.clear(); }
		[[nodiscard]] const AlphaStats& GetAlphaStats() const { return alphaStats_; }
		/// Format of the copy layers for AlphaMaskCS (R8_UNORM, or R8G8B8A8_UNORM where R8 is not shareable).
		[[nodiscard]] bool AlphaCopiesRgba() const { return alphaLayerFormat_ == DXGI_FORMAT_R8G8B8A8_UNORM; }

		// (45i) Skinned characters. Created on first use only (never with the switch off).
		/// The skinning shader and the character variants of the trace pipeline exist.
		/// (With the alpha variant present, its character counterpart is needed too: a frame may hold both.)
		[[nodiscard]] bool SkinAvailable() const
		{
			return ready_ && !failed_ && skinPipeline_ && charPipeline_ && (charAlphaPipeline_ || !alphaPipeline_) && skinFault_.empty();
		}
		/// Character variant with alpha-tested parts (hair) available too.
		[[nodiscard]] bool SkinAlphaAvailable() const { return SkinAvailable() && charAlphaPipeline_ && AlphaAvailable(); }
		/// Takes the geometry; its buffers are created and uploaded over the next frames (a few per
		/// frame). kInvalidSkin when refused (counted: VRAM budget, size, alpha texcoord buffer).
		SkinHandle CreateSkinShape(std::unique_ptr<SkinGeometry> a_geometry);
		/// Its GPU resources go once every submission that may use them has completed.
		void ReleaseSkinShape(SkinHandle a_shape);
		/// Built at least once: from now on it is skinned, refit and traced on every frame it is drawn.
		[[nodiscard]] bool IsSkinShapeReady(SkinHandle a_shape) const;
		/// The record of an alpha-tested skinned shape (texture, threshold), as CreateAlphaInfo.
		std::uint32_t CreateSkinAlphaInfo(SkinHandle a_shape, AlphaTexture a_texture, const AlphaMaterial& a_material);
		[[nodiscard]] bool SkinIdle() const { return skinLive_ == 0; }
		/// Scratch and bone uploads (only when SkinIdle()).
		void ReleaseSkinResources();
		/// Set when a skinning resource could not be created: the feature turns the switch off.
		[[nodiscard]] const std::string& SkinFault() const { return skinFault_; }
		void ClearSkinFault() { skinFault_.clear(); }
		[[nodiscard]] const SkinStats& GetSkinStats() const { return skinStats_; }
		/// Instance mask of character instances (TraceShadowsCS kCharacterMask); every other instance is 0xFF.
		static constexpr std::uint32_t kCharacterMask = 0x02;
		/// (45l) Instance mask of the other actors' parts (TraceShadowsCS kNpcMask): the player keeps kCharacterMask.
		static constexpr std::uint32_t kNpcMask = 0x04;
		/// VRAM for skinned characters (buffers + BLASes); shapes past it keep their shadow-map shadow.
		static constexpr std::uint64_t kSkinBudgetBytes = 512ull << 20;

		/// True when the device was removed or a D3D12 call failed; the feature then turns itself off.
		[[nodiscard]] bool HasFailed() const { return failed_; }
		[[nodiscard]] const std::string& LastError() const { return lastError_; }
		[[nodiscard]] const Stats& GetStats() const { return stats_; }
		[[nodiscard]] const TlasCheck& GetTlasCheck() const { return check_; }
		void ResetTlasCheck() { check_ = {}; }
		/// D3D12 instance mask of every scene (non-character) instance, and the flags of opaque ones.
		/// (45j) 0x01, not 0xFF: 0xFF contained kCharacterMask, so the character ray (mask 0x02) of
		/// the RT_CHARACTERS variants walked the whole static scene a second time per pixel (and the
		/// coverage view marked every static surface as a character). The scene ray's mask
		/// (0xFF without characters, 0x01 with them) still takes every scene instance.
		static constexpr std::uint32_t kInstanceMask = 0x01;
		static constexpr D3D12_RAYTRACING_INSTANCE_FLAGS kInstanceFlags =
			D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
		[[nodiscard]] std::string AdapterDescription() const { return adapterDescription_; }

	private:
		static constexpr std::size_t kContexts = 8;
		static constexpr std::uint32_t kPostbuildSlots = 1024;
		// (45e) small object meshes arrive in bursts. (45j) 48 -> 16: every build creates a committed
		// resource on the render thread (and its compaction a second one later); bursts of them were
		// CPU spikes of several ms.
		static constexpr std::size_t kMaxBuildsPerFrame = 16;
		static constexpr std::uint32_t kMaxCompactionsPerFrame = 16;  // (45j) the rest wait for the next frame
		// Both on D3D11's guaranteed-shareable list; the shaders' declared types follow these.
		static constexpr DXGI_FORMAT kDepthMaskFormat = DXGI_FORMAT_R32_FLOAT;        // depth; negative = engine mask already 0
		static constexpr DXGI_FORMAT kVisibilityFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // r visibility, g coverage

		struct SharedTexture
		{
			ComPtr<ID3D11Texture2D> resource11;
			ComPtr<ID3D11ShaderResourceView> srv11;
			ComPtr<ID3D11UnorderedAccessView> uav11;
			ComPtr<ID3D12Resource> resource12;
		};

		struct CommandContext
		{
			ComPtr<ID3D12CommandAllocator> allocator;
			ComPtr<ID3D12GraphicsCommandList4> list;
			ComPtr<ID3D12GraphicsCommandList4> traceList;  // (45l) the trace alone, behind the D3D11 wait (same allocator)
			ComPtr<ID3D12Resource> instanceBuffer;  // upload heap, persistently mapped
			void* instanceMapped = nullptr;
			std::uint32_t instanceCapacity = 0;
			std::uint64_t fenceValue = 0;
			bool timestampsPending = false;
			// (45h) This context's copy of the alpha records (upload heap, persistently mapped).
			ComPtr<ID3D12Resource> alphaInfoBuffer;
			void* alphaInfoMapped = nullptr;
			std::uint32_t alphaInfoCapacity = 0;
			std::uint64_t alphaInfoVersion = 0;
			// (45i) This context's bone matrices (upload heap, persistently mapped).
			ComPtr<ID3D12Resource> boneBuffer;
			void* boneMapped = nullptr;
			std::uint64_t boneCapacity = 0;  // bytes
		};

		enum class BlasState : std::uint8_t
		{
			Free,
			Queued,
			Built,      // usable; compaction size not read back yet
			Ready       // compacted (or compaction not worth it)
		};

		struct Blas
		{
			BlasState state = BlasState::Free;
			std::unique_ptr<ChunkGeometry> geometry;
			ComPtr<ID3D12Resource> resource;
			std::uint64_t bytes = 0;
			std::uint32_t triangles = 0;
			std::uint32_t postbuildSlot = UINT32_MAX;
			std::uint64_t buildFence = 0;
			float boundsMin[3]{}, boundsMax[3]{};  // (45f) local box of the vertices, for the self-check
			// (45h) Alpha BLAS: its texcoords in the shared texcoord buffer (uint2 elements).
			std::uint32_t uvOffset = UINT32_MAX;
			std::uint32_t uvCount = 0;
		};

		// (45h) Alpha-tested casters.
		static constexpr std::uint32_t kAlphaTextureSlots = 4096;        // bindless table (TraceShadowsCS kAlphaTextureSlots)
		static constexpr std::uint32_t kAlphaDescriptorBase = 3;         // heap: depth SRV, visibility UAV, copy layers SRV, textures
		static constexpr std::uint32_t kAlphaLayerSize = 256;
		static constexpr std::uint32_t kAlphaLayerMips = 5;              // 256 .. 16
		static constexpr std::uint32_t kAlphaMaxInfos = 65536;
		static constexpr std::uint64_t kAlphaUvBytes = 64ull << 20;      // about 2.8 M alpha-tested triangles
		static constexpr std::uint32_t kAlphaFallbackBit = 0x80000000u;  // record texture = copy layer (TraceShadowsCS)

		struct AlphaInfoGpu
		{
			std::uint32_t uvBase;
			std::uint32_t triangles;
			std::uint32_t texture;  // bindless slot, or kAlphaFallbackBit | layer
			std::uint32_t flags;
			float threshold;
			float alphaScale;
			float pad[2];
			float uvTransform[4];
		};
		static_assert(sizeof(AlphaInfoGpu) == 48);

		struct AlphaTextureEntry
		{
			ComPtr<ID3D11Texture2D> texture11;        // held: the key can never be a reused address
			ComPtr<ID3D11ShaderResourceView> view11;  // source of the copy until it is made
			ComPtr<ID3D12Resource> resource12;        // shared path
			std::uint32_t users = 0;
			std::uint32_t gpuIndex = UINT32_MAX;
			std::uint32_t sourceSize = 0;             // larger side of the game texture (copy mip choice)
			bool live = false;
			bool ready = false;
		};

		struct DeferredFree
		{
			std::uint64_t fence;
			std::uint32_t value;
			std::uint32_t count;  // texcoord ranges only
		};

		bool EnsureAlphaUv();
		bool EnsureAlphaLayers();
		bool AllocateAlphaUv(std::uint32_t a_count, std::uint32_t& o_offset);
		void FreeAlphaUv(std::uint32_t a_offset, std::uint32_t a_count);
		void SetAlphaFault(std::string a_reason);
		void CollectAlphaRetired(std::uint64_t a_completed);
		bool EnsureAlphaInfoBuffer(CommandContext& a_context);
		void UpdateAlphaTextureStats();

		// (45i) Skinned characters.
		static constexpr std::uint32_t kSkinMaxVertices = 65536;          // per shape (16-bit indices in the game's data)
		static constexpr std::uint32_t kSkinMaxTriangles = 131072;        // per shape
		static constexpr std::uint32_t kSkinMaxBones = 256;               // per shape (8-bit bone indices)
		static constexpr std::uint32_t kSkinUploadsPerFrame = 4;          // new shapes made per frame (45k: 8 -> 4, plus a CPU budget)
		static constexpr double kSkinUploadBudgetMs = 1.0;                // (45k) buffer creation per frame (at least one shape)
		static constexpr std::uint32_t kSkinUploadVerticesPerFrame = 400000;
		static constexpr std::uint32_t kSkinBuildsPerFrame = 8;           // first builds per frame (refits are never capped)
		static constexpr std::uint32_t kSkinRebuildsPerFrame = 2;         // periodic rebuilds per frame
		static constexpr std::uint32_t kSkinRebuildFrames = 240;          // a shape is rebuilt after this many refits
		static constexpr std::uint64_t kSkinMaxBoneFloats = 4ull << 20;   // 16 MB of bone matrices per frame

		struct SkinShape
		{
			bool live = false;
			bool uploaded = false;  // buffers created and filled
			bool built = false;     // BLAS built at least once
			bool outputInUav = true;
			std::unique_ptr<SkinGeometry> geometry;  // until uploaded
			ComPtr<ID3D12Resource> rest, index, output, blas;
			std::uint64_t restBytes = 0, indexBytes = 0, outputBytes = 0, blasBytes = 0;
			std::uint64_t buildScratch = 0, updateScratch = 0;
			std::uint32_t vertices = 0, triangles = 0, boneCount = 0;
			std::uint32_t uvOffset = UINT32_MAX, uvCount = 0;  // alpha texcoords (hair)
			std::uint32_t refits = 0;                          // since the last build
			std::uint64_t lastSubmission = 0;                  // a shape is skinned once per frame
			[[nodiscard]] std::uint64_t Bytes() const { return restBytes + indexBytes + outputBytes + blasBytes; }
		};

		bool CreateSkinPipelines();
		void SetSkinFault(std::string a_reason);
		void UploadSkinShapes(ID3D12GraphicsCommandList4* a_list);
		/// Skins this frame's draws, builds / refits their BLASes; o_ready[i] = draw i can be traced.
		void RecordSkinning(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, const FrameInput& a_input,
			std::vector<std::uint8_t>& o_ready);
		std::uint32_t CreateAlphaInfoAt(std::uint32_t a_uvOffset, std::uint32_t a_triangles, AlphaTexture a_texture, const AlphaMaterial& a_material);

		struct Retired
		{
			std::uint64_t fence;
			ComPtr<ID3D12Pageable> object;
			std::uint64_t bytes;
			std::uint64_t* counter;  // stat to subtract from when released (may be null)
		};

		bool Fail(const std::string& a_operation, HRESULT a_result);
		/// The fence value after which an object retired now is no longer used by the GPU. (45n) While builds /
		/// skinning are recorded or in flight without the D3D11 ready signal behind them (split submission, Early
		/// Prep) that is the trace's done value (+2), not the D3D11 ready value (+1): the ready value is signalled
		/// by the game's queue, which is not ordered after our builds.
		[[nodiscard]] std::uint64_t RetireValue() const { return fenceValue_ + retireAhead_; }
		void Retire(ComPtr<ID3D12Pageable> a_object, std::uint64_t a_bytes, std::uint64_t* a_counter);
		void StopReleaseThread();
		void ReleaseThreadMain();
		void CollectRetired();
		void CollectTimestamps(std::size_t a_index);
		ComPtr<ID3D12Resource> CreateBuffer(std::uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, bool a_uav);
		bool CheckSharedFormat(DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store, const char* a_name, bool a_fatal = true);
		bool CreateSharedTexture(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store,
			SharedTexture& o_texture, const char* a_name);
		bool CreatePipeline(std::string& o_reason);
		void WriteDescriptors();
		bool EnsureScratch(std::uint64_t a_bytes);
		bool EnsureTlas(std::uint32_t a_instances);
		bool EnsureInstanceBuffer(CommandContext& a_context, std::uint32_t a_instances);
		void ProcessCompaction(ID3D12GraphicsCommandList4* a_list, std::uint64_t a_completed, std::uint32_t a_max);
		std::uint32_t BuildQueued(ID3D12GraphicsCommandList4* a_list, std::uint32_t a_budget, std::size_t a_maxBuilds);
		BlasHandle AllocateHandle();
		void CheckInstance(const D3D12_RAYTRACING_INSTANCE_DESC& a_desc, const Instance& a_instance, const Blas& a_blas, const FrameInput& a_input);

		ComPtr<ID3D11Device5> device11_;
		ComPtr<ID3D11DeviceContext4> context11_;
		ComPtr<ID3D11Fence> fence11_;
		ComPtr<ID3D12Device5> device_;
		ComPtr<ID3D12CommandQueue> queue_;
		ComPtr<ID3D12Fence> fence_;
		std::array<CommandContext, kContexts> contexts_;
		std::size_t cursor_ = 0;
		std::uint64_t fenceValue_ = 0;
		std::uint64_t lastSubmitted_ = 0;

		ComPtr<ID3D12RootSignature> rootSignature_;
		ComPtr<ID3D12PipelineState> pipeline_;
		ComPtr<ID3D12PipelineState> alphaPipeline_;  // (45h) TraceShadowsCS with RT_ALPHA
		ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
		UINT descriptorSize_ = 0;

		SharedTexture depthMask_;   // kDepthMaskFormat
		SharedTexture visibility_;  // kVisibilityFormat
		std::uint32_t textureWidth_ = 0, textureHeight_ = 0;

		ComPtr<ID3D12Resource> tlas_;
		ComPtr<ID3D12Resource> scratch_;
		std::uint64_t tlasBytes_ = 0, scratchBytes_ = 0;
		std::uint32_t tlasCapacity_ = 0;

		ComPtr<ID3D12Resource> postbuild_;           // default heap, UAV
		ComPtr<ID3D12Resource> postbuildReadback_;   // readback heap
		const std::uint64_t* postbuildMapped_ = nullptr;
		std::uint32_t postbuildCursor_ = 0;

		ComPtr<ID3D12QueryHeap> timestampHeap_;
		ComPtr<ID3D12Resource> timestampReadback_;
		std::uint64_t timestampFrequency_ = 0;

		std::vector<Blas> blas_;
		std::vector<BlasHandle> freeHandles_;
		std::deque<BlasHandle> buildQueue_;
		std::vector<Retired> retired_;

		// (45h) Alpha-tested casters.
		std::vector<AlphaTextureEntry> alphaTextures_;
		std::vector<AlphaTexture> freeAlphaTextures_;
		std::unordered_map<const ID3D11Texture2D*, AlphaTexture> alphaTextureIndex_;
		std::deque<AlphaTexture> alphaCopyQueue_;
		std::vector<std::uint32_t> freeAlphaSlots_, freeAlphaLayers_;
		std::uint32_t nextAlphaSlot_ = 0, nextAlphaLayer_ = 0;  // never used so far
		std::vector<DeferredFree> retiredAlphaSlots_, retiredAlphaLayers_, retiredAlphaUv_;
		std::vector<AlphaInfoGpu> alphaInfos_;
		std::vector<std::uint32_t> freeAlphaInfos_;
		std::uint32_t alphaInfoLive_ = 0;
		std::uint64_t alphaInfoVersion_ = 1;
		ComPtr<ID3D12Resource> alphaUv_;
		std::map<std::uint32_t, std::uint32_t> alphaUvFree_;  // offset -> count (uint2 elements)
		std::uint64_t alphaUvUsed_ = 0;                        // elements allocated (including deferred frees)
		ComPtr<ID3D11Texture2D> alphaLayers11_;
		std::array<ComPtr<ID3D11UnorderedAccessView>, kAlphaLayerMips> alphaLayerUavs_;
		ComPtr<ID3D12Resource> alphaLayers12_;
		ComPtr<ID3D11Buffer> alphaCopyCB_;
		ComPtr<ID3D11SamplerState> alphaCopySampler_;
		DXGI_FORMAT alphaLayerFormat_ = DXGI_FORMAT_UNKNOWN;
		std::uint32_t alphaLayerCount_ = 0;
		bool alphaLayersFailed_ = false;
		std::uint64_t alphaSharedOpened_ = 0;
		std::string alphaFault_;
		AlphaStats alphaStats_{};

		// (45i) Skinned characters.
		ComPtr<ID3D12RootSignature> skinRootSignature_;
		ComPtr<ID3D12PipelineState> skinPipeline_;       // SkinCharactersCS
		ComPtr<ID3D12PipelineState> charPipeline_;       // TraceShadowsCS with RT_CHARACTERS
		ComPtr<ID3D12PipelineState> charAlphaPipeline_;  // ... and RT_ALPHA
		std::vector<SkinShape> skins_;
		std::vector<SkinHandle> freeSkins_;
		std::deque<SkinHandle> skinUploadQueue_;
		ComPtr<ID3D12Resource> skinScratch_;
		std::uint64_t skinScratchBytes_ = 0;
		std::uint32_t skinLive_ = 0;
		std::string skinFault_;
		SkinStats skinStats_{};
		std::vector<std::uint8_t> skinReady_;  // per draw, this frame
		bool skinRecorded_ = false;            // uploads or builds recorded in this frame's list
		std::uint64_t pendingWait_ = 0;        // (45l) done value the D3D11 queue still has to wait for (deferred wait)
		// (45n) Early Prep: the context PrepareFrame recorded on (reserved: fenceValue UINT64_MAX until the trace
		// or AbandonPrepared gives it its real done value).
		bool prepared_ = false;
		std::size_t preparedContext_ = 0;
		std::uint64_t retireAhead_ = 1;
		// (45n) Release thread.
		bool releaseOnWorker_ = true;
		std::thread releaseThread_;
		std::mutex releaseMutex_;
		std::condition_variable releaseCv_;
		std::vector<ComPtr<ID3D12Pageable>> releaseQueue_;
		bool releaseStop_ = false;
		std::atomic<float> workerReleaseMs_{ 0.0f };
		std::atomic<std::uint64_t> workerReleased_{ 0 };

		HANDLE fenceEvent_ = nullptr;
		Stats stats_{};
		TlasCheck check_{};
		std::string lastError_;
		std::string adapterDescription_;
		bool ready_ = false;
		bool failed_ = false;
		bool explicitFlush_ = true;  // (45k)
	};
}
