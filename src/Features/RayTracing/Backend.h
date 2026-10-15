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

#include "Utils/FrameTrace.h"  // (46p) VramOwner, RtPrepWork

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
		std::uint32_t texturesFallback = 0;  // 256x256 alpha copies
		std::uint32_t texturesPending = 0;   // alpha copies not made yet
		std::uint64_t layersFull = 0;        // copy layers full: shape not ray traced
		std::uint64_t uvFull = 0;            // texcoord buffer full: shape not ray traced
		std::uint64_t infosFull = 0;
		std::uint32_t infos = 0;             // live per-shape records
		std::uint32_t instancesLastFrame = 0;
		std::uint64_t uvBytesUsed = 0;
		std::uint64_t uvBytes = 0;           // texcoord buffer (allocated on first use)
		std::uint64_t layerBytes = 0;        // 256x256 copy layers (allocated on first use)
		std::uint64_t infoBytes = 0;         // per-context upload copies of the records
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
		// (46m) Merge Character Parts: an opaque shape traced only inside its character's merged BLAS (SkinDraw::group);
		// it gets no BLAS of its own.
		bool grouped = false;
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
		std::uint32_t owner = 0;  // (46e) the character's key (InstanceContributionToHitGroupIndex, 24 bits): point lights it carries skip it
		bool reuse = false;  // (45l) not skinned / refit this frame: last frame's BLAS (x / y / z = the origin it was skinned at)
		// (46m) Merge Character Parts: the character's key (0 = a BLAS of its own). Every draw of one group has the same
		// x / y / z, npc and owner; the group's opaque shapes are one BLAS (one geometry each), one TLAS instance.
		std::uint32_t group = 0;
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
		// (46m) Merge Character Parts: merged BLASes in the TLAS last frame, of them built / refit / kept as they were,
		// shapes inside them, character instances in the TLAS (merged + own BLAS), skinning dispatches.
		std::uint32_t groupsLastFrame = 0;
		std::uint32_t groupBuildsLastFrame = 0;
		std::uint32_t groupRefitsLastFrame = 0;
		std::uint32_t groupKeptLastFrame = 0;
		std::uint32_t groupShapesLastFrame = 0;
		std::uint32_t instancesLastFrame = 0;
		std::uint32_t dispatchesLastFrame = 0;
		std::uint64_t buildsTotal = 0;
		std::uint64_t bytes = 0;              // rest, index and vertex buffers + BLASes (live shapes)
		std::uint64_t scratchBytes = 0;
		std::uint64_t boneBytes = 0;          // per-context bone matrix uploads
		std::uint64_t overBudget = 0;         // shapes refused: VRAM budget
		std::uint64_t refused = 0;            // shapes refused: too big / bad data
	};

	/// (46g) A range carved out of one of the backend's GPU pools (GpuArena): no D3D12 resource of its own.
	struct ArenaAlloc
	{
		std::uint32_t block = UINT32_MAX;
		std::uint64_t offset = 0;
		std::uint64_t size = 0;
		[[nodiscard]] bool Valid() const { return block != UINT32_MAX; }
	};

	/// (46g) Where the CPU of the last PrepareFrame (Early Prep) went (ms; -1 = not run).
	struct PrepSplit
	{
		float collect = -1;     // retired objects + pools (deferred frees, adopted blocks) + context choice
		float timestamps = -1;  // read-back of completed contexts' GPU timestamps / counters
		float reset = -1;       // command allocator + list Reset
		float build = -1;       // BLAS builds / compactions recorded
		float skin = -1;        // character uploads / skinning recorded
		float close = -1;       // barriers, timestamps, list Close
		float execute = -1;     // ExecuteCommandLists
	};

	/// (46g) The GPU pools: bytes reserved (the large buffers) and in use (allocations, frees not yet retired included).
	struct PoolStats
	{
		std::uint64_t reservedBytes = 0;
		std::uint64_t usedBytes = 0;
		std::uint32_t blocks = 0;
		std::uint32_t syncBlocksLastFrame = 0;   // created on the render thread this frame (loading screens / black screen only)
		std::uint64_t syncBlocksTotal = 0;
		std::uint64_t asyncBlocksTotal = 0;      // created on the pool thread
		std::uint64_t blocksReleased = 0;
		float pumpMsLastFrame = 0.0f;            // render-thread ms of the pool upkeep this frame (sync creations included)
		float workerLastMs = 0.0f;               // the pool thread's last block creation
		std::uint32_t allocFailsLastFrame = 0;   // allocations that had to wait for a new block
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

	/// (46b) Point-light shadow trace constants (TracePointLightsCS PLConstants).
	struct PointTraceConstants
	{
		float invViewProj[16];
		float renderSize[4];  // w, h, 1/w, 1/h
		float rect[4];        // camera-relative xy min / max
		float params[4];      // sources to scan, frame, flags (bit pattern), K
		float bias[4];        // constant, per unit distance, character ray TMin, emitter radius
		float extra[4];       // least traced share, least absolute contribution, least share of the sun light at a sunlit pixel, unused
		float spheres[8];     // character bounds, as TraceConstants::spheres
		float sun[4];         // to the sun (world axes), its strength (brightest channel; 0 = no sun gate)
		float bounds[4];      // (46c) interior: camera-relative z min / max of the captured interior (with rect = its xy), z ray margin, w unused
	};
	static_assert(sizeof(PointTraceConstants) == 52 * 4);

	/// (46f) RTAO trace constants (TraceAOCS AOConstants).
	struct AOTraceConstants
	{
		float invViewProj[16];
		float renderSize[4];  // w, h, 1/w, 1/h
		float rect[4];        // camera-relative xy min / max
		float params[4];      // frame, flags (bit pattern), ray length (units), unused
		float fade[4];        // weight falls from x to 0 at y (units from the camera), z constant ray offset, w per unit of distance
		float extra[4];       // character ray TMin, unused x3
		float spheres[8];     // character bounds, as TraceConstants::spheres
	};
	static_assert(sizeof(AOTraceConstants) == 44 * 4);

	/// (46f) This frame's RTAO trace (FrameInput::ao; null = none): its own list after the sun trace, its own fence.
	struct AOInput
	{
		AOTraceConstants constants{};
		bool alpha = false;  // alpha-tested instances tested against their cut-out (else culled)
	};

	/// (46b) This frame's point-light trace (FrameInput::pointLights; null = none, the 45 frame exactly).
	struct PointLightInput
	{
		PointTraceConstants constants{};
		const void* sources = nullptr;  // RayTracingPointLights::GpuSource[kMaxSources]
		std::uint32_t sourceBytes = 0;  // of the whole array
		std::uint32_t scan = 0;         // slots to scan (> 0)
		bool alpha = false;             // (46c) alpha-tested instances (window masks) are tested against their cut-out (interiors)
	};

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
		// (46g) Drawn two-sided by the engine (or unknown): keeps TRIANGLE_CULL_DISABLE. Others are one-sided so that the window
		// beam rays (which may cull, Point Light Shadows > Window Beam Culling) pass the room's own shell from inside;
		// the sun, point-light and RTAO rays cull nothing (their results do not change).
		bool twoSided = true;
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
		float buildCpuMs = 0.0f;      // (46g) render-thread CPU for BLAS builds / compactions this frame (0 = the play default)
		float skinUploadCpuMs = 0.0f; // (46g) render-thread CPU for new character shapes this frame (0 = the play default)
		// (45l) The D3D11 queue's wait for the trace is left to WaitForTrace() (called right before the
		// first D3D11 pass that reads the result); false = waited at the end of ExecuteFrame (45k).
		bool deferWait = false;
		// (46b) Point-light shadows traced right after the sun, same list and TLAS (null = none).
		const PointLightInput* pointLights = nullptr;
		// (46c) false = no sun trace (interiors): only the point lights are traced against the TLAS.
		bool sun = true;
		// (46f) RTAO: recorded in its own list, submitted after the trace list, signalling the second fence (null = none).
		const AOInput* ao = nullptr;
		// (46m) Split Trace Sync: with the sun and the point lights traced, the sun goes in its own list and signals the sun
		// fence before the point lights run; the D3D11 queue may then wait for the sun alone (WaitForSun) before the shadow
		// mask passes and for the rest (WaitForTrace) only before the point lights' denoiser.
		bool splitSun = false;
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
		// (46g) Taken out of collect / tlas / record (they no longer include these): GPU timestamp read-back, TLAS /
		// instance buffer growth, descriptor publish, the RTAO list.
		float timestamps = 0;
		float ensure = 0;
		float publish = 0;
		float ao = 0;
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
		PrepSplit prep{};                 // (46g) the last PrepareFrame, call by call
		PoolStats pools{};                // (46g)
		// (46b) Point-light trace: GPU ms of its dispatch (inside lastTraceGpuMs no more: lastTraceGpuMs is the sun's),
		// rays traced per frame (counter read back with the timestamps; -1 = not known), last frame traced or not.
		float lastPointGpuMs = -1.0f;
		std::int64_t pointRaysPerFrame = -1;
		bool pointTracedLastFrame = false;
		// (46g) Point lights: pixels whose top-K source set changed per frame (-1 = not known); window beams per slot: sampled cone
		// pixels traced / blocked per frame (1 of 16 pixels), beamSamples counts the read-backs.
		std::int64_t pointChurnPerFrame = -1;
		std::array<std::uint32_t, 64> beamTraced{}, beamBlocked{};
		std::uint64_t beamSamples = 0;
		// (46f) RTAO: GPU ms of its dispatch (not inside lastGpuMs' trace), rays per frame (read back late; -1 = not known), last frame traced or not.
		float lastAOGpuMs = -1.0f;
		std::int64_t aoRaysPerFrame = -1;
		bool aoTracedLastFrame = false;
		bool aoAlphaLastFrame = false;
		// (46m) Split Trace Sync: the last ExecuteFrame signalled the sun fence between the sun and the point lights.
		bool sunSplitLastFrame = false;
		// (46p) What the submission behind lastPrepGpuMs recorded (set with it).
		Util::FrameTrace::RtPrepWork lastPrepWork{};
		bool parallelBuildsLastFrame = false;  // (46p) the last static batch was built in parallel
	};

	/// (46g) GPU memory carved out of a few large committed buffers ("blocks"), so a BLAS or a character shape
	/// costs no D3D12 resource creation on the render thread (CreateCommittedResource made resident and zeroed its
	/// memory synchronously: 15-125 ms frames right after a cell transition). Ranges are freed behind a fence value
	/// (the same rule as Backend::Retire). Blocks are created ahead of need: on the pool thread while playing, on the
	/// render thread only behind a loading / black screen. Render thread only (except the creation itself).
	struct GpuArena
	{
		struct Block
		{
			ComPtr<ID3D12Resource> resource;
			std::byte* mapped = nullptr;   // upload pools: persistently mapped
			D3D12_GPU_VIRTUAL_ADDRESS va = 0;
			std::uint64_t size = 0;
			std::uint64_t used = 0;        // allocated, deferred frees included
			D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;  // whole-buffer state (pools that transition)
			std::map<std::uint64_t, std::uint64_t> free;  // offset -> size
			std::int64_t adoptedQpc = 0;   // (46i) a new block is never given back before it could be used
			bool live = false;
		};
		struct Deferred
		{
			std::uint64_t fence;
			ArenaAlloc alloc;
			std::uint64_t* counter;  // stat to subtract alloc.size from once it is really free (may be null)
			std::uint64_t bytes;
		};

		const wchar_t* name = L"";
		const char* label = "";  // for the log
		D3D12_HEAP_TYPE heap = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_STATES createState = D3D12_RESOURCE_STATE_COMMON;
		bool uav = false;
		std::uint64_t blockBytes = 0;        // usual block size (a larger request gets a block of its own size)
		std::uint64_t alignment = 256;
		std::uint64_t headroom = 0;          // free bytes kept ready while playing (grown on the pool thread)
		std::uint64_t transitionHeadroom = 0;  // ... behind loading / black screens (grown on the render thread)
		std::uint64_t maxBytes = 0;          // hard cap of the reserved total

		std::vector<Block> blocks;           // a released block's slot stays (dead) until reused
		std::vector<Deferred> deferred;
		std::uint64_t reserved = 0;
		std::uint64_t used = 0;
		std::uint64_t wantBytes = 0;         // an allocation that did not fit: the next block must hold it
		bool requestInFlight = false;
		bool failed = false;                 // a block could not be created (the owner decides what goes off)
		// (46i) Idle release: a pool gives its empty blocks back only after no allocation was asked of it for a while
		// (0 = idle since forever: Ray Tracing switched off). 46g released a block in the same upkeep that adopted it
		// whenever the pool was empty (nothing allocated yet) -- after a switch off / on the queue waited for ever.
		std::int64_t lastAskQpc = 0;
		std::int64_t waitSinceQpc = 0;       // an allocation has been waiting for a block since then (0 = none)
		bool waitLogged = false;

		bool Allocate(std::uint64_t a_bytes, ArenaAlloc& o_alloc);
		void Free(const ArenaAlloc& a_alloc, std::uint64_t a_fence, std::uint64_t* a_counter = nullptr, std::uint64_t a_bytes = 0);
		void Collect(std::uint64_t a_completed);
		void Adopt(ComPtr<ID3D12Resource> a_resource, std::byte* a_mapped, std::uint64_t a_size);
		[[nodiscard]] std::uint64_t FreeBytes() const { return reserved > used ? reserved - used : 0; }
		[[nodiscard]] D3D12_GPU_VIRTUAL_ADDRESS Address(const ArenaAlloc& a_alloc) const { return blocks[a_alloc.block].va + a_alloc.offset; }
		[[nodiscard]] ID3D12Resource* Resource(const ArenaAlloc& a_alloc) const { return blocks[a_alloc.block].resource.Get(); }
		[[nodiscard]] std::byte* Mapped(const ArenaAlloc& a_alloc) const { return blocks[a_alloc.block].mapped + a_alloc.offset; }
		void Reset()
		{
			blocks.clear();
			deferred.clear();
			reserved = used = wantBytes = 0;
			requestInFlight = failed = false;
			lastAskQpc = waitSinceQpc = 0;
			waitLogged = false;
		}
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
		/// (45n) Retired D3D12 objects the GPU is done with are released on a worker thread (freeing committed
		/// resources costs about 0.5 ms per character shape on the render thread). Worker: ms of the last batch released and objects released since start.
		[[nodiscard]] float LastWorkerReleaseMs() const { return workerReleaseMs_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t WorkerReleased() const { return workerReleased_.load(std::memory_order_relaxed); }
		/// (45l) With FrameInput::deferWait: the D3D11 queue waits (on the GPU) for the last trace now.
		/// No-op when nothing is pending. Returns the CPU ms of the call.
		float WaitForTrace();
		[[nodiscard]] bool TraceWaitPending() const { return pendingWait_ != 0; }
		/// (46m) Split Trace Sync: the D3D11 queue waits (on the GPU) for the sun trace alone (the sun fence). No-op when
		/// the last trace was not split (or the wait was taken). WaitForTrace() covers it too (the sun list runs first).
		float WaitForSun();
		[[nodiscard]] bool SunWaitPending() const { return pendingSunWait_ != 0; }
		/// (45k) Loading screens: BLAS builds and compactions only (no trace, no D3D11 interop), with a
		/// larger cap. Never waits; false when no command context is free or nothing is queued.
		/// (46g) a_cpuMs: render-thread budget for the builds / compactions / character uploads of this submission.
		bool BuildOnly(std::uint32_t a_triangleBudget, std::uint32_t a_maxBuilds, float a_cpuMs = 2.0f);
		[[nodiscard]] std::uint32_t QueuedBuilds() const { return static_cast<std::uint32_t>(buildQueue_.size()); }
		/// (46g) Character shapes created but not uploaded yet.
		[[nodiscard]] std::uint32_t PendingSkinUploads() const { return static_cast<std::uint32_t>(skinUploadQueue_.size()); }
		/// (46g) Behind a loading screen or the black screen after it: the pools grow to their larger headroom on the
		/// render thread (nothing is visible), scratch / TLAS / per-context buffers are sized ahead (Prewarm).
		void SetTransition(bool a_transition) { transition_ = a_transition; }
		/// (46i) Ray Tracing switched off: no transition, the pools forget growth they wanted and give their empty
		/// blocks back as soon as the released scene's ranges are retired (switching on again starts from clean pools).
		void OnSwitchedOff();
		/// (46i) The pools for the log: per pool reserved / used MB, blocks, a waiting allocation, a request in flight.
		[[nodiscard]] std::string PoolSummary() const;
		/// (46i) Ms the oldest waiting pool allocation has waited for a block (0 = none waits).
		[[nodiscard]] double PoolWaitMs() const;
		/// (46g) Start of a frame: the per-frame pool counters start from zero.
		void BeginFrame()
		{
			stats_.pools.syncBlocksLastFrame = 0;
			stats_.pools.allocFailsLastFrame = 0;
			stats_.pools.pumpMsLastFrame = 0.0f;
			stats_.prep = {};
		}
		[[nodiscard]] bool InTransition() const { return transition_; }
		/// (46g) Grows what a destination's first frames would otherwise create on the render thread: pool headroom,
		/// the shared scratch buffers, the TLAS and the free contexts' instance / bone buffers. Bounded by a_cpuMs.
		void Prewarm(float a_cpuMs, bool a_characters);
		/// (46g) Character uploads made since start (for the per-phase transition log).
		[[nodiscard]] std::uint64_t SkinUploadsTotal() const { return skinUploadsTotal_; }

		/// (46p) Parallel BLAS Builds: the static builds of one batch get a scratch slice each and no barrier between them.
		void SetParallelBuilds(bool a_on) { parallelBuilds_ = a_on; }
		/// (46p) Trace submissions recorded so far (the newest RtPrepWork::submission).
		[[nodiscard]] std::uint64_t SubmissionsTotal() const { return submissions_; }
		/// (46p) Keep Our GPU Memory Resident: our video-memory resources (pools, TLAS, scratch, shared textures) get
		/// D3D12_RESIDENCY_PRIORITY_HIGH (else NORMAL). Applied to what exists when it changes, and to every later creation.
		void SetResidencyHigh(bool a_high);
		[[nodiscard]] bool ResidencyHigh() const { return residencyHigh_.load(std::memory_order_relaxed); }
		/// (46p) RT queue at D3D12_COMMAND_QUEUE_PRIORITY_HIGH; read by Initialize only (takes effect at the next start-up).
		void SetQueuePriorityHigh(bool a_high) { queuePriorityHigh_ = a_high; }
		[[nodiscard]] bool QueuePriorityHigh() const { return queueCreatedHigh_; }
		/// (46p) Ray Tracing switched off: the shared textures, scratch buffers and the TLAS go (behind the fence, Retire);
		/// recreated on the next switch-on. The pools already give their empty blocks back (OnSwitchedOff).
		void ReleaseIdleResources();

		/// (45e) True when ExecuteFrame will find a free command context. Only the GPU completing
		/// work can change the answer from false to true, never back, so a yes at the start of the
		/// frame still holds when the frame's trace is recorded.
		[[nodiscard]] bool HasFreeContext() const
		{
			if (!ready_ || failed_ || !fence_)
				return false;
			const std::uint64_t completed = fence_->GetCompletedValue();
			const std::uint64_t aoCompleted = AOCompleted();
			for (const auto& context : contexts_)
				if (ContextFree(context, completed, aoCompleted))
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

		// (46b) Point-light shadows. Their textures / buffers are created on first use only.
		/// The point-light trace pipeline exists (the driver accepted it) and nothing of it failed.
		[[nodiscard]] bool PointAvailable() const { return ready_ && !failed_ && pointPipeline_ && pointFault_.empty(); }
		/// (46c) The point-light trace's alpha-tested variant (cut-outs such as Window Shadows Ultimate's window masks) exists.
		[[nodiscard]] bool PointAlphaAvailable() const { return PointAvailable() && pointAlphaPipeline_ && AlphaAvailable(); }
		/// (46c) The last point-light trace used the alpha-tested variant.
		[[nodiscard]] bool PointAlphaLastFrame() const { return pointAlphaLastFrame_; }
		/// Shared textures (sized like the shadow mask) for the point-light trace. False = not available
		/// (PointFault() says why; the rest of Ray Tracing goes on).
		bool EnsurePointTextures(std::uint32_t a_width, std::uint32_t a_height);
		/// Retires the point-light textures and buffers (switch off).
		void ReleasePointResources();
		[[nodiscard]] ID3D11ShaderResourceView* PointVisibilitySRV() const { return pointVis_.srv11.Get(); }
		[[nodiscard]] ID3D11ShaderResourceView* PointSourcesSRV() const { return pointIdx_.srv11.Get(); }
		[[nodiscard]] const std::string& PointFault() const { return pointFault_; }
		void ClearPointFault() { pointFault_.clear(); }
		[[nodiscard]] std::uint64_t PointBytes() const { return pointBytes_; }

		// (46f) RTAO. Its pipeline is created with the others; its texture, ray counter and second fence on first use only.
		[[nodiscard]] bool AOAvailable() const { return ready_ && !failed_ && aoPipeline_ && aoFault_.empty(); }
		/// The alpha-tested variant (traced leaves / cut-outs tested against their cut-out) exists.
		[[nodiscard]] bool AOAlphaAvailable() const { return AOAvailable() && aoAlphaPipeline_ && AlphaAvailable(); }
		/// The shared RTAO texture (render size), its ray counter and the second fence. False = not available (AOFault() says why).
		bool EnsureAOTextures(std::uint32_t a_width, std::uint32_t a_height);
		/// Retires the RTAO texture and counter (switch off). The second fence stays.
		void ReleaseAOResources();
		[[nodiscard]] ID3D11ShaderResourceView* AORawSRV() const { return aoRaw_.srv11.Get(); }
		[[nodiscard]] const std::string& AOFault() const { return aoFault_; }
		void ClearAOFault() { aoFault_.clear(); }
		[[nodiscard]] std::uint64_t AOBytes() const { return aoBytes_; }
		/// The D3D11 queue waits (on the GPU) for the last RTAO trace (its second fence). No-op when nothing is pending.
		/// Right before the first D3D11 pass that reads it; never before the opaque pass. Returns the CPU ms of the call.
		float WaitForAO();
		[[nodiscard]] bool AOWaitPending() const { return pendingAOWait_ != 0; }

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
		// (46g) 1024 -> 16384: with loading-screen batches a slot could be reused before its BLAS read it (an owner
		// check now drops the compaction of such a BLAS instead of reading another build's size).
		static constexpr std::uint32_t kPostbuildSlots = 16384;
		// (45e) small object meshes arrive in bursts. (45j) 48 -> 16: every build created a committed
		// resource on the render thread. (46g) Pools: no creation per build any more; a CPU-time budget per frame
		// (FrameInput::buildCpuMs, kPlayBuildCpuMs) bounds the work, this count only bounds one submission.
		static constexpr std::size_t kMaxBuildsPerFrame = 64;
		static constexpr std::uint32_t kMaxCompactionsPerFrame = 64;  // (45j) the rest wait for the next frame
		static constexpr float kPlayBuildCpuMs = 2.0f;                // (46g) BLAS builds + compactions per frame while playing
		static constexpr float kPlaySkinUploadCpuMs = 1.0f;           // (46g) new character shapes per frame while playing
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
			// (46b) This context's point-light sources (upload heap, persistently mapped) and whether its
			// submission traced them (timestamps / ray counter to read back).
			ComPtr<ID3D12Resource> pointBuffer;
			void* pointMapped = nullptr;
			std::uint32_t pointCapacity = 0;  // bytes
			bool pointPending = false;
			std::uint64_t pointFrame = 0;     // pointFrame_ when it was submitted
			// (46f) RTAO: its own list (same allocator, recorded after the trace list), the second fence's value its
			// submission signalled (0 = none: the context is free once fenceValue completed), read-back pending.
			ComPtr<ID3D12GraphicsCommandList4> aoList;
			std::uint64_t aoFenceValue = 0;
			bool aoPending = false;
			std::uint64_t aoFrame = 0;
			// (46m) Split Trace Sync: the point lights' own list (same allocator, recorded after the sun list); the
			// trace timestamps of this submission then end with the sun (the point lights have their own pair).
			ComPtr<ID3D12GraphicsCommandList4> pointList;
			bool splitTrace = false;
			Util::FrameTrace::RtPrepWork work{};  // (46p) what this submission's prep recorded (read with its timestamps)
		};
		/// (46f) A context may be reused once its submission (and its RTAO list, behind the second fence) completed.
		static bool ContextFree(const CommandContext& a_context, std::uint64_t a_completed, std::uint64_t a_aoCompleted)
		{
			return (a_context.fenceValue == 0 || a_context.fenceValue <= a_completed) && a_context.aoFenceValue <= a_aoCompleted;
		}
		[[nodiscard]] std::uint64_t AOCompleted() const { return aoFence_ ? aoFence_->GetCompletedValue() : UINT64_MAX; }

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
			ArenaAlloc alloc;  // (46g) in asPool_ (was a committed resource per BLAS)
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
		static constexpr std::uint32_t kSkinUploadsPerFrame = 64;         // new shapes made per submission (46g: pools, a CPU budget bounds it)
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
			std::unique_ptr<SkinGeometry> geometry;  // until uploaded
			// (46g) Pool ranges (were four committed resources per shape): rest + index in skinStaticPool_, the skinned
			// vertices in skinOutputPool_, the BLAS in asPool_.
			ArenaAlloc rest, index, output, blas;
			std::uint64_t restBytes = 0, indexBytes = 0, outputBytes = 0, blasBytes = 0;
			std::uint64_t buildScratch = 0, updateScratch = 0;
			std::uint32_t vertices = 0, triangles = 0, boneCount = 0;
			std::uint32_t uvOffset = UINT32_MAX, uvCount = 0;  // alpha texcoords (hair)
			std::uint32_t refits = 0;                          // since the last build
			std::uint64_t lastSubmission = 0;                  // a shape is skinned once per frame
			// (46m) Merge Character Parts: no BLAS of its own (built = inside a built merged BLAS); skinned = its skinned
			// vertices were written at least once (a merged BLAS may be refit from them on a still frame); serial = unique
			// per shape ever made (handles are reused), what a merged BLAS's member list compares.
			bool grouped = false;
			bool skinned = false;
			std::uint64_t serial = 0;
			[[nodiscard]] std::uint64_t Bytes() const { return restBytes + indexBytes + outputBytes + blasBytes; }
		};

		// (46m) Merge Character Parts: one BLAS per character (SkinDraw::group) holding its opaque shapes as geometries,
		// built when the member list changes (equip, a new or released shape) and refit in place on every frame a member
		// was skinned (one refit instead of one per shape: the per-build cost was most of the skinning's GPU time).
		struct SkinGroup
		{
			std::uint32_t key = 0;
			ArenaAlloc blas;
			std::uint64_t blasBytes = 0;  // allocated (a little above the member list's need: small additions fit in place)
			std::uint64_t buildScratch = 0, updateScratch = 0;
			std::vector<std::uint64_t> serials;  // the built BLAS's members, in geometry order
			bool built = false;
			bool seen = false;  // drawn this frame (unseen groups are released)
			std::uint32_t refits = 0;
		};
		struct SkinGroupInstance
		{
			D3D12_GPU_VIRTUAL_ADDRESS blas = 0;
			double x = 0.0, y = 0.0, z = 0.0;
			bool npc = false;
			std::uint32_t owner = 0;
			std::uint32_t members = 0;
		};
		static constexpr float kSkinGroupSlack = 1.25f;  // a merged BLAS is allocated this much above its need

		bool CreateSkinPipelines();
		void SetSkinFault(std::string a_reason);
		/// (46m) Gives a merged BLAS's memory back (behind the fence) and forgets it.
		void ReleaseSkinGroup(SkinGroup& a_group);
		/// (46m) Every merged BLAS (characters released, switched off).
		void ReleaseSkinGroups();
		void UploadSkinShapes(ID3D12GraphicsCommandList4* a_list, float a_cpuMs);
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
		/// (46p) a_owner: the video-memory owner it is counted under (F7 vram_*).
		ComPtr<ID3D12Resource> CreateBuffer(std::uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, bool a_uav,
			Util::FrameTrace::VramOwner a_owner = Util::FrameTrace::VramOwner::RtOther);
		// (46p) Parallel BLAS Builds (a batch's scratch slices fit this, else the shared scratch and a barrier each), residency
		// priority, queue priority.
		static constexpr std::uint64_t kParallelScratchBytes = 64ull << 20;
		bool parallelBuilds_ = false;
		std::atomic<bool> residencyHigh_{ false };  // read on the pool thread too
		bool queuePriorityHigh_ = false;
		bool queueCreatedHigh_ = false;
		/// (46p) Sets a_objects' residency priority to the current one (HIGH / NORMAL); null entries are skipped.
		void ApplyResidency(ID3D12Pageable* const* a_objects, std::size_t a_count);
		void ApplyResidency(ID3D12Pageable* a_object) { ApplyResidency(&a_object, 1); }
		// (46p) The prep work being recorded (stored into the context with its timestamps); pool block totals at the last store.
		Util::FrameTrace::RtPrepWork recWork_{};
		std::uint64_t workBlocksSeen_ = 0;
		std::uint64_t workReleasedSeen_ = 0;
		std::uint64_t submissions_ = 0;  // (46p) trace submissions recorded (RtPrepWork::submission)
		/// (46p) Copies the skinning counts of the RecordSkinning just run (and uploads since a_uploadsBefore) into recWork_.
		void NoteSkinWork(std::uint64_t a_uploadsBefore);
		bool CheckSharedFormat(DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store, const char* a_name, bool a_fatal = true);
		bool CreateSharedTexture(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store,
			SharedTexture& o_texture, const char* a_name, bool a_fatal = true);
		bool CreatePipeline(std::string& o_reason);
		void WriteDescriptors();
		bool EnsureScratch(std::uint64_t a_bytes);
		/// (46p) Grows the shared scratch to a_bytes, making the new buffer before retiring the old; false (nothing changed, no Fail)
		/// when it cannot be made.
		bool TryGrowScratch(std::uint64_t a_bytes);
		bool EnsureTlas(std::uint32_t a_instances);
		bool EnsureInstanceBuffer(CommandContext& a_context, std::uint32_t a_instances);
		void ProcessCompaction(ID3D12GraphicsCommandList4* a_list, std::uint64_t a_completed, std::uint32_t a_max, std::int64_t a_deadline);
		std::uint32_t BuildQueued(ID3D12GraphicsCommandList4* a_list, std::uint32_t a_budget, std::size_t a_maxBuilds, std::int64_t a_deadline);

		// (46g) GPU pools.
		GpuArena asPool_;           // BLAS results, compacted copies, character BLASes (acceleration-structure state, never transitioned)
		GpuArena uploadPool_;       // geometry uploads (upload heap, mapped), freed behind the submission's fence
		GpuArena skinStaticPool_;   // character rest vertices + indices (copy dest -> non-pixel SRV per block)
		GpuArena skinOutputPool_;   // character skinned vertices (UAV <-> non-pixel SRV per block)
		bool transition_ = false;
		std::uint64_t skinUploadsTotal_ = 0;
		std::vector<std::uint64_t> postbuildOwner_;  // per slot: (handle + 1) of the BLAS that owns it, 0 = none
		std::array<GpuArena*, 4> Pools() { return { &asPool_, &uploadPool_, &skinStaticPool_, &skinOutputPool_ }; }
		std::array<const GpuArena*, 4> Pools() const { return { &asPool_, &uploadPool_, &skinStaticPool_, &skinOutputPool_ }; }
		void InitPools();
		/// Deferred frees, finished pool-thread blocks, growth requests (sync behind loading / black screens), empty
		/// blocks released. Every frame from CollectRetired.
		void PumpPools();
		ComPtr<ID3D12Resource> CreatePoolBuffer(const GpuArena& a_pool, std::uint64_t a_bytes, std::byte** o_mapped);
		/// Records whole-block transitions of every block in a_blocks of a_pool to a_state.
		void TransitionBlocks(ID3D12GraphicsCommandList4* a_list, GpuArena& a_pool, const std::vector<std::uint32_t>& a_blocks, D3D12_RESOURCE_STATES a_state);
		void StopPoolThread();
		void PoolThreadMain();
		struct PoolRequest
		{
			std::uint32_t pool;
			std::uint64_t bytes;
		};
		struct PoolResult
		{
			std::uint32_t pool;
			std::uint64_t bytes;
			ComPtr<ID3D12Resource> resource;
			std::byte* mapped;
		};
		std::thread poolThread_;
		std::mutex poolMutex_;
		std::condition_variable poolCv_;
		std::vector<PoolRequest> poolRequests_;
		std::vector<PoolResult> poolResults_;
		bool poolStop_ = false;
		std::atomic<float> poolWorkerMs_{ 0.0f };
		std::atomic<bool> notZeroed_{ true };  // D3D12_HEAP_FLAG_CREATE_NOT_ZEROED accepted (cleared on its first refusal)
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
		std::vector<std::uint32_t> freeAlphaLayers_;
		std::uint32_t nextAlphaLayer_ = 0;  // never used so far
		std::vector<DeferredFree> retiredAlphaLayers_, retiredAlphaUv_;
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
		// (46m) Merge Character Parts.
		std::vector<SkinGroup> skinGroups_;
		std::vector<SkinGroupInstance> skinGroupFrame_;  // this frame's merged BLASes for the TLAS (RecordSkinning)
		std::uint64_t skinSerial_ = 0;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> skinGroupDraws_;  // scratch: (group key, draw)
		std::vector<std::uint32_t> skinGroupMembers_;                         // scratch: member draws of every merged BLAS
		std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> skinGroupDescs_;          // scratch: one merged BLAS's geometries
		std::uint64_t pendingWait_ = 0;        // (45l) done value the D3D11 queue still has to wait for (deferred wait)
		// (46m) Split Trace Sync: the sun fence (shared; only the D3D12 queue signals it, between the sun and the point
		// lights), its last value and the value the D3D11 queue may still wait for (0 = none).
		ComPtr<ID3D12Fence> sunFence_;
		ComPtr<ID3D11Fence> sunFence11_;
		std::uint64_t sunFenceValue_ = 0;
		std::uint64_t pendingSunWait_ = 0;
		bool sunFenceFailed_ = false;
		bool EnsureSunFence();
		// (45n) Early Prep: the context PrepareFrame recorded on (reserved: fenceValue UINT64_MAX until the trace
		// or AbandonPrepared gives it its real done value).
		bool prepared_ = false;
		std::size_t preparedContext_ = 0;
		std::uint64_t retireAhead_ = 1;
		// (45n) Release thread.
		std::thread releaseThread_;
		std::mutex releaseMutex_;
		std::condition_variable releaseCv_;
		std::vector<ComPtr<ID3D12Pageable>> releaseQueue_;
		bool releaseStop_ = false;
		std::atomic<float> workerReleaseMs_{ 0.0f };
		std::atomic<std::uint64_t> workerReleased_{ 0 };

		// (46b) Point-light shadows.
		static constexpr DXGI_FORMAT kPointFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // PLVis / PLIdx (TracePointLightsCS)
		static constexpr std::uint32_t kPointDescriptorBase = kAlphaDescriptorBase + kAlphaTextureSlots;  // depth SRV, sun visibility SRV, PLVis UAV, PLIdx UAV
		static constexpr std::uint32_t kPointDescriptors = 4;
		ComPtr<ID3D12RootSignature> pointRootSignature_;
		ComPtr<ID3D12PipelineState> pointPipeline_;
		ComPtr<ID3D12PipelineState> pointAlphaPipeline_;  // (46c) TracePointLightsCS with RT_ALPHA
		bool pointAlphaLastFrame_ = false;
		SharedTexture pointVis_, pointIdx_;
		std::uint32_t pointWidth_ = 0, pointHeight_ = 0;
		ComPtr<ID3D12Resource> pointCounter_;          // default heap, UAV: rays traced (monotonic, wraps)
		ComPtr<ID3D12Resource> pointCounterReadback_;  // per context one uint32 copy of it
		const std::uint32_t* pointCounterMapped_ = nullptr;
		std::uint32_t pointCounterLast_ = 0;
		// (46g) Counter words (TracePointLightsCS RayCounter): [0] rays, [1] pixels whose top-K source set changed, from
		// kPointBeamBase per slot: sampled cone pixels traced / blocked of a window beam. The whole block is read back per context.
		static constexpr std::uint32_t kPointCounterWords = 256, kPointBeamBase = 64, kPointBeamSlots = 64;
		std::array<std::uint32_t, kPointCounterWords> pointCounterWords_{};
		std::uint64_t pointCounterLastFrame_ = 0;
		bool pointCounterValid_ = false;
		std::uint64_t pointFrame_ = 0;                 // point-light frames submitted
		std::uint64_t pointBytes_ = 0;
		std::string pointFault_;
		bool CreatePointPipeline();
		void SetPointFault(std::string a_reason);
		bool EnsurePointBuffer(CommandContext& a_context, std::uint32_t a_bytes);
		void WritePointDescriptors();
		void RecordPointTrace(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, std::size_t a_index, const FrameInput& a_input, bool a_alphaFrame,
			D3D12_GPU_DESCRIPTOR_HANDLE a_table);

		// (46f) Descriptors that change at run time (texture (re)creation) are written into a CPU-only staging heap and
		// copied into the recording context's own block of the shader-visible heap: a rewrite can never touch a
		// descriptor a submission still in flight reads (each block is only reused once its context completed).
		// Block layout: sun table (depth SRV, visibility UAV), point table (depth SRV, sun visibility SRV, PLVis UAV,
		// PLIdx UAV), RTAO table (depth SRV, AO UAV).
		static constexpr std::uint32_t kDynSun = 0, kDynPoint = 2, kDynAO = 6, kDynamicDescriptors = 8;
		static constexpr std::uint32_t kDynamicBase = kPointDescriptorBase + kPointDescriptors;  // after the (unused) 46b point slots
		ComPtr<ID3D12DescriptorHeap> stagingHeap_;
		[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE StagingCpu(std::uint32_t a_slot) const;
		/// Copies the staging descriptors into context a_index's block; returns the block's GPU handle.
		D3D12_GPU_DESCRIPTOR_HANDLE PublishDescriptors(std::size_t a_index);
		[[nodiscard]] static D3D12_GPU_DESCRIPTOR_HANDLE Offset(D3D12_GPU_DESCRIPTOR_HANDLE a_handle, std::uint32_t a_slots, UINT a_size)
		{
			a_handle.ptr += static_cast<UINT64>(a_size) * a_slots;
			return a_handle;
		}

		// (46f) RTAO.
		static constexpr DXGI_FORMAT kAOFormat = DXGI_FORMAT_R8G8B8A8_UNORM;  // TraceAOCS AOOut
		ComPtr<ID3D12RootSignature> aoRootSignature_;
		ComPtr<ID3D12PipelineState> aoPipeline_;
		ComPtr<ID3D12PipelineState> aoAlphaPipeline_;
		SharedTexture aoRaw_;
		std::uint32_t aoWidth_ = 0, aoHeight_ = 0;
		ComPtr<ID3D12Resource> aoCounter_;          // default heap, UAV: rays traced (monotonic, wraps)
		ComPtr<ID3D12Resource> aoCounterReadback_;  // per context one uint32 copy of it
		const std::uint32_t* aoCounterMapped_ = nullptr;
		std::uint32_t aoCounterLast_ = 0;
		std::uint64_t aoCounterLastFrame_ = 0;
		bool aoCounterValid_ = false;
		std::uint64_t aoFrame_ = 0;
		std::uint64_t aoBytes_ = 0;
		std::string aoFault_;
		// The second fence: only the D3D12 queue signals it (after the RTAO list); the D3D11 queue waits for it right
		// before the AO is used. Never part of the trace's ready / done pair (rt_sync is unchanged).
		ComPtr<ID3D12Fence> aoFence_;
		ComPtr<ID3D11Fence> aoFence11_;
		std::uint64_t aoFenceValue_ = 0;
		std::uint64_t pendingAOWait_ = 0;
		bool CreateAOPipeline();
		void SetAOFault(std::string a_reason);
		void WriteAODescriptors();
		/// After the trace list was closed: records (and closes) the RTAO list on the same context (allocator). ExecuteFrame
		/// submits it right behind the trace and signals the second fence. False = nothing recorded (failed_ on a D3D12 error).
		bool RecordAOTrace(CommandContext& a_context, std::size_t a_index, const FrameInput& a_input, bool a_alphaFrame, D3D12_GPU_DESCRIPTOR_HANDLE a_table);

		HANDLE fenceEvent_ = nullptr;
		Stats stats_{};
		TlasCheck check_{};
		std::string lastError_;
		std::string adapterDescription_;
		bool ready_ = false;
		bool failed_ = false;
	};
}
