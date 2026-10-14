#pragma once

#include "Backend.h"

#include <deque>

/**
 * @brief (45, RT prototype) Scene-graph capture of the loaded exterior cells.
 *
 * Each frame the TES grid is compared with the cells already captured: new cells are captured
 * over the next frames (a time budget per frame, nearest cells first), cells that left the grid
 * are dropped (their BLASes retire once the GPU is done with them). A captured cell is a few
 * BLASes of up to kChunkTriangles each, positions relative to the cell's south-west corner, so a
 * cell needs exactly one TLAS instance per chunk with an identity rotation.
 *
 * What goes in: references whose base is a Static, Furniture or Container -- the cell's own and
 * (45f) the persistent ones standing in its square -- only BSTriShape geometry drawn with the
 * Lighting shader that casts shadows and is neither alpha-tested, alpha-blended, skinned, a decal
 * nor tree-animated. (45f) Landscape is not captured (kCaptureLandscape): it only receives.
 * Everything else is counted per reason (shown on the page) and keeps its shadow-map shadow.
 * (45g) Alpha-tested shapes are taken as opaque when CutoutAsOpaque allows it (large, one-sided,
 * edge-only cutout); shapes under camera-facing billboard nodes and refraction (heat haze) shapes
 * never go in; of a NiSwitchNode only the drawn child is captured; statics with a Havok-driven
 * part (a child node with its own collision: hinged signs) are traced as moving objects.
 * (45h) With Alpha-tested casters on, alpha-tested shapes whose cut-out can be traced (diffuse
 * texture, texcoords, a "greater" alpha test; tree-animated leaves too) get a non-opaque BLAS per
 * mesh with their texcoords and an instance per shape with a record (texture, threshold) -- in
 * cells and objects alike; they register with the caster filter once their BLAS is built and their
 * texture ready. A shape whose cut-out cannot be traced falls back to the 45g rules. Off = 45g.
 *
 * (45e) Objects that can move or need their own placement -- doors, activators, movable statics
 * and lights; loose items; trees and plants (only their opaque, non alpha-tested shapes: trunks
 * and branches; leaf cards are alpha-tested and stay in the shadow map) -- are not baked into the
 * cell chunks. Each distinct engine mesh gets one BLAS in its own space (shared by every copy),
 * and each shape one TLAS instance carrying the shape's world transform of the frame, so a door
 * that swings or an item that falls is traced where it is. They are found by a slow scan of the
 * captured cells' references and dropped when their reference is disabled, gets new 3D or leaves
 * the captured cells.
 *
 * A new cell is captured once it has been in the grid for a moment (its 3D streams in first).
 * Cells are re-captured (at most every 1.5 s or so) when their landscape or the 3D / enabled
 * state of a captured reference changes (a few hundred references are re-checked per frame),
 * and everything is dropped when a loading screen opens or the player is in an interior.
 *
 * (45e) Nothing leaves the ray-traced scene in the middle of a frame. A re-captured cell keeps
 * its old capture (BLASes, TLAS instances, caster-filter entries) until the new capture's BLASes
 * are all built, and then swaps; cells leaving the grid (and replaced captures) are "retiring":
 * they are still traced this frame and only released in EndFrame(), after the trace was
 * submitted. So every caster the shadow map left out at the start of the frame is in that
 * frame's TLAS.
 *
 * Every captured geometry is held by an NiPointer while its cell is captured, so the caster
 * filter (CasterFilter.h) can never match a freed and reused address.
 *
 * Main / render thread only (SE renders on the main thread).
 */
namespace RayTracingScene
{
	inline constexpr float kCellUnits = 4096.0f;
	/// (45f) Landscape is not an RT occluder (user decision): it stays a shadow receiver and keeps
	/// casting through the shadow map / Terrain Shadows. The capture path stays behind this switch.
	inline constexpr bool kCaptureLandscape = false;
	/// (45f) References held by the worldspace's persistent cell (not by the grid cell they stand
	/// in) are captured with the grid cell whose square they stand in.
	inline constexpr bool kCapturePersistent = true;
	/// (45g) CutoutAsOpaque criteria: alpha test "greater (or equal) than" at most this threshold
	/// (0-255; roof tiles use 80, plank cut-outs 128) on a one-sided shape at least this large.
	inline constexpr std::uint8_t kCutoutMaxThreshold = 128;
	inline constexpr float kCutoutMinRadius = 128.0f;
	/// (45h) Alpha-tested casters: shapes smaller than this keep their shadow-map shadow (tiny
	/// cut-outs are many instances for little shadow), and one mesh's BLAS is capped.
	inline constexpr float kAlphaMinRadius = 24.0f;
	inline constexpr std::uint32_t kAlphaMaxTriangles = 131072;

	struct SkipCounts
	{
		std::uint32_t actors = 0;          // NPCs and creatures (skinned, not in the prototype)
		std::uint32_t markers = 0;         // markers, sounds, hazards and other reference types without a shadow
		std::uint32_t foliage = 0;         // tree-animated shapes inside static objects
		std::uint32_t disabled = 0;        // disabled / deleted references or no 3D
		std::uint32_t hidden = 0;          // hidden nodes and editor markers
		std::uint32_t otherGeometry = 0;   // particles, dynamic / multi-index / sub-index shapes
		std::uint32_t notLighting = 0;     // effect, water, sky shaders
		std::uint32_t alphaTested = 0;     // includes the leaves of trees and plants
		std::uint32_t alphaBlended = 0;
		std::uint32_t decals = 0;
		std::uint32_t skinned = 0;
		std::uint32_t noShadow = 0;        // shape does not cast shadows in the game either
		std::uint32_t noCpuData = 0;       // the engine kept no CPU copy of the vertices
		std::uint32_t badData = 0;         // index out of range, unknown vertex layout
		std::uint32_t billboard = 0;       // (45g) under a camera-facing NiBillboardNode (fire haze planes, glows)
		std::uint32_t refraction = 0;      // (45g) refraction shader (heat haze): invisible distortion
		std::uint32_t switchedOff = 0;     // (45g) inactive children of a NiSwitchNode (the game draws one child)
		std::uint32_t cutoutAsOpaque = 0;  // (45g) alpha-tested shapes taken as opaque (CutoutAsOpaque), not a skip
		std::uint32_t physicsParts = 0;    // (45g) statics with a Havok-driven part (swinging signs): own instances
		std::uint32_t alphaAnyHit = 0;     // (45h) alpha-tested shapes traced with their cut-out, not a skip
		std::uint32_t alphaUnusable = 0;   // (45h) alpha-tested, but no texture / texcoords / usable test: 45g rules
	};

	/// (45h) Alpha-tested casters traced with their cut-out (Alpha-tested casters switch).
	struct AlphaSceneStats
	{
		std::uint32_t captured = 0;       // shapes held now (cells and objects)
		std::uint32_t registered = 0;     // ... in the TLAS and out of the shadow map
		std::uint32_t meshes = 0;         // distinct alpha meshes (one non-opaque BLAS each)
		std::uint64_t meshTriangles = 0;
		std::uint32_t failedMesh = 0;     // mesh unreadable, too big or no texcoords: 45g rules (total)
		std::uint32_t failedTexture = 0;  // texture could not be registered: 45g rules (total)
		std::uint32_t failedRecord = 0;   // texcoord buffer / record table full: 45g rules (total)
	};

	/// (45j) Foliage (leaves, bushes, plants with a cut-out): kept in the shadow map (and its
	/// screen-space contact shadows), ray traced with the cut-out instead, or both (min of the two).
	enum class FoliageMode : int
	{
		ShadowMap = 0,
		RayTraced = 1,
		Both = 2
	};

	/// (45j) Per kind of object: ray traced (true) or left to the shadow map + screen-space shadows.
	struct SceneConfig
	{
		bool buildings = true;        // statics, furniture, containers baked into the cell chunks (incl. edge-only cut-outs taken as opaque)
		bool objects = true;          // doors, activators, movable statics, lights, loose items
		bool trees = false;           // opaque parts of trees and plants (trunks, branches)
		FoliageMode foliage = FoliageMode::ShadowMap;  // vegetation cut-outs (leaves, bushes)
		bool cutouts = false;         // other cut-outs (fences, banners, grilles) traced with their cut-out
		bool cutoutAsOpaque = true;   // (45g) large one-sided edge-only cut-outs (roof tiles) as opaque
		// (46c) Interiors: Window Shadows Ultimate's window masks (alpha-tested shapes with a textures\masks\ texture: the
		// window pattern of its beams) traced with their cut-out whatever the Cut-outs setting (needs the alpha pipeline).
		bool windowMasks = false;
		bool operator==(const SceneConfig&) const = default;
	};

	/// (45j) CPU of the scene's per-frame work (ms), for the page and the F7 trace.
	struct FrameTimes
	{
		float update = 0.0f;        // whole Update
		float gridDiff = 0.0f;      // grid diff, re-validation of captured references, new cells
		float gather = 0.0f;        // cell capture (budgeted)
		float registration = 0.0f;  // caster filter registration of built cells / shapes
		float objects = 0.0f;       // object validation + scan (doors, items, trees)
		float objectCapture = 0.0f; // ... of which new objects captured
		float instances = 0.0f;     // CollectInstances
		float missed = 0.0f;        // DescribeMissedCasters
		float endFrame = 0.0f;      // EndFrame (retiring captures released)
		float release = 0.0f;       // engine references dropped (deferred, bounded)
		std::uint32_t recaptures = 0;      // cell recaptures started this frame
		std::uint32_t objectsCaptured = 0; // object references captured this frame
		std::uint32_t missedJudged = 0;    // missed casters examined this frame
		std::uint32_t releasePending = 0;  // engine references still waiting to be dropped
	};

	/// (45g) Why a missed caster (a big shape drawn into the sun cascades inside the traced area but
	/// absent from the scene) is missing; counted once per distinct shape after it was judged final.
	enum class MissReason : std::uint8_t
	{
		NeverCaptured,        // its reference was never captured (not in its cell's reference list when captured)
		ShapeSkipped,         // its reference was captured, this shape was not (hidden then, no CPU copy, placement, ...)
		RootMismatch,         // captured with older 3D (a recapture is requested)
		PersistentUnmatched,  // a persistent reference whose square has no captured cell
		AlphaTested,
		Decal,
		Skinned,
		NotLighting,
		Billboard,
		Character,
		NoOwner,              // not part of a placed reference
		ByChoice,             // (45j) its kind is left to the shadow map (Ray Tracing > Advanced)
		Other,                // other shape rules (no shadow flag, geometry type, LOD flag, refraction, ...)
		Count
	};
	inline constexpr const char* kMissReasonNames[] = { "never captured", "captured but shape skipped", "3D root mismatch",
		"persistent, no captured square", "alpha-tested", "decal", "skinned", "not Lighting shader", "billboard", "character",
		"no owning reference", "left to the shadow map (Advanced)", "other shape rule" };
	static_assert(std::size(kMissReasonNames) == static_cast<std::size_t>(MissReason::Count));

	/// (45e) Objects with their own TLAS instances, moved every frame.
	enum class ObjectKind : std::uint8_t
	{
		Movable,  // doors, activators, movable statics, lights
		Item,     // loose items
		Tree      // trees and plants: opaque (non alpha-tested) parts only
	};

	struct ObjectStats
	{
		std::uint32_t refs[3]{};            // captured references per ObjectKind (total since the last reset)
		std::uint32_t captured = 0;         // references captured now
		std::uint32_t shapes = 0;           // shapes captured (total since the last reset)
		std::uint32_t registeredShapes = 0; // shapes now in the TLAS and out of the shadow map
		std::uint32_t meshes = 0;           // distinct meshes (one BLAS each, shared by every copy)
		std::uint64_t meshTriangles = 0;
		std::uint32_t overBudget = 0;       // new meshes not captured (VRAM budget)
	};

	struct Stats
	{
		std::uint32_t cellsTracked = 0;
		std::uint32_t cellsComplete = 0;
		std::uint32_t cellsGathering = 0;
		std::uint32_t recaptures = 0;       // cells captured again after a change (total)
		std::uint32_t shapes = 0;           // captured shapes in tracked cells
		std::uint32_t landQuads = 0;
		std::uint64_t triangles = 0;        // captured triangles in tracked cells
		float lastGatherMs = 0.0f;          // CPU, this frame
		float maxGatherMs = 0.0f;           // CPU, worst frame since the last reset of the stats
		float lastCellMs = 0.0f;            // CPU, the whole capture of the last finished cell
		SkipCounts skipped{};
		// (45e) Placement check of captured shapes against the engine's own world bound.
		std::uint32_t placementChecked = 0;
		std::uint32_t placementFixed = 0;     // fitted only with the other position format (half / float)
		std::uint32_t placementRejected = 0;  // fitted with neither: left out
		std::uint32_t placementUnchecked = 0; // no usable world bound
		std::uint32_t placementLogged = 0;
		ObjectStats objects{};
		// (45f) References of the captured cells' squares that live in the worldspace's persistent cell.
		std::uint32_t persistentRefs = 0;     // found while capturing cells (total since the last reset)
		std::uint32_t persistentObjects = 0;  // found by the object scan
		// (45f) Missed casters described in the log (CasterFilter self-check).
		std::uint32_t missedDescribed = 0;   // lines written to the log this session
		std::uint32_t missedCapturable = 0;  // the shape itself is fine: its reference was not captured
		std::uint32_t missedRejected = 0;    // the shape was rejected (reason in the log)
		// (45g) Judged misses in the current area (distinct shapes; reset with the scene), by reason.
		std::uint32_t missedConfirmed = 0;
		std::uint32_t missedByReason[static_cast<int>(MissReason::Count)]{};
		std::uint32_t missedPending = 0;           // re-armed: cell still capturing, BLAS building, ... (total)
		std::uint32_t missedRecaptures = 0;        // recaptures the self-check requested (total)
		AlphaSceneStats alpha{};                   // (45h)
		// (46c) The captured interior cell (Ray Tracing's point lights indoors).
		bool interior = false;                     // an interior cell is captured / being captured
		std::uint32_t interiorShapes = 0;          // its shapes (statics, furniture, alpha-tested) captured so far
		std::uint64_t interiorTriangles = 0;
		std::uint32_t windowMasks = 0;             // Window Shadows Ultimate mask shapes traced with their cut-out (total)
		float interiorGatherMs = 0.0f;             // CPU of its capture in total
		float interiorBounds[6]{};                 // world min xyz, max xyz of what was captured
	};

	class Scene
	{
	public:
		Scene() = default;
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;
		/// Process exit: the game's objects are not released (its memory manager may be gone already).
		~Scene()
		{
			new std::vector<std::unique_ptr<Cell>>(std::move(cells_));
			new std::vector<std::unique_ptr<Cell>>(std::move(retiring_));
			new std::vector<std::unique_ptr<ObjectRef>>(std::move(objects_));
			new std::vector<std::unique_ptr<ObjectRef>>(std::move(retiringObjects_));
			new std::vector<RE::NiPointer<RE::TESObjectREFR>>(std::move(scanList_));
			new std::vector<RE::NiPointer<RE::BSGeometry>>(std::move(geometryGrave_));
			new std::vector<RE::NiPointer<RE::TESObjectREFR>>(std::move(refGrave_));
			new PersistentIndex(std::move(persistent_));
		}

		/// Drops everything (BLASes are retired through the backend, filter entries removed).
		void ReleaseAll(RayTracingRT::Backend& a_backend);
		/// Asks the next Update to drop everything (loading screens, from the UI thread too).
		void RequestFlush() { flushRequested_.store(true); }

		/// Per frame: grid diff, re-validation, a_budgetMs of capture, BLAS creation, filter registration.
		void Update(RayTracingRT::Backend& a_backend, float a_budgetMs, std::uint32_t a_frame);

		/// TLAS instances of every chunk (the backend skips the ones not built yet). a_expected
		/// (45f): also fill each instance's box of the engine's live bounds for the self-check.
		void CollectInstances(std::vector<RayTracingRT::Instance>& o_instances, bool a_expected) const;

		/// (45f) Logs why big casters the shadow map drew inside the traced area are not in the
		/// scene (CasterFilter::TakeMissed). (45g) Misses whose cell is still being captured or whose
		/// BLAS is being built are re-armed instead of counted; judged ones are counted per
		/// MissReason and each distinct reference + shape is logged once (rate-limited, capped).
		void DescribeMissedCasters();

		/// After this frame's trace was submitted (or skipped): releases the retiring captures.
		void EndFrame(RayTracingRT::Backend& a_backend);
		/// (45j) Drops a bounded number of held engine objects (also while Ray Tracing is off or failed).
		void Housekeeping() { DrainGraves(0.3); }

		/// World-space xy rect of the captured cells (min x, min y, max x, max y); false when none.
		/// a_includeRetiring: also the cells still traced this frame but released at its end.
		bool GetLoadedRect(float o_rect[4], bool a_includeRetiring) const;
		/// True once at least one cell is captured with all its BLASes built.
		[[nodiscard]] bool HasCoverage() const { return cellsReady_ > 0; }

		/// (46g) Hand-off hold (cell transitions): while held, a cell / object / cut-out whose BLAS is ready is neither
		/// traced nor taken out of the cascades -- it waits for HandOff(), which switches every ready item at once (one
		/// global switch per transition instead of objects popping from the shadow map to RT one by one). Off: an item
		/// is handed off as soon as it is ready (a whole cell at once), as while walking outdoors.
		void SetHold(bool a_hold) { hold_ = a_hold; }
		[[nodiscard]] bool Held() const { return hold_; }
		struct HandOffCounts
		{
			std::uint32_t shown = 0;    // items switched now
			std::uint32_t pending = 0;  // captured items not ready yet (BLAS / texture), or cells still being captured
		};
		/// Every ready item is handed off now (registered with the caster filter on the next Update).
		HandOffCounts HandOff(const RayTracingRT::Backend& a_backend);
		/// Items held back now: ready ones and not ready ones.
		[[nodiscard]] HandOffCounts HeldCounts(const RayTracingRT::Backend& a_backend) const;
		/// (46g) Cell transitions: on the next flush the cells attached then (the place being left) are remembered and
		/// not captured again while a_on (behind the loading screen the old grid is still attached); new cells settle
		/// faster. Off forgets them.
		void SetTransition(bool a_on)
		{
			transition_ = a_on;
			if (!a_on)
				departing_.clear();
		}
		[[nodiscard]] std::uint32_t DepartingSkipped() const { return departingSkipped_; }

		/// (46c) Interiors: whether the player's interior cell may be captured (Ray Tracing's point lights indoors).
		/// Off: Update drops an interior capture (and never starts one), as before 46c.
		void SetInteriorEnabled(bool a_enabled) { interiorEnabled_ = a_enabled; }
		[[nodiscard]] bool InteriorEnabled() const { return interiorEnabled_; }
		/// (46c) The world box of the captured interior (min xyz, max xyz, with a margin); false when no interior is captured
		/// or nothing of it yet.
		bool GetInteriorBounds(double o_min[3], double o_max[3]) const;

		[[nodiscard]] const Stats& GetStats() const { return stats_; }
		void ResetPeaks() { stats_.maxGatherMs = 0.0f; }

		/// (45j) What goes into the ray-traced scene, per kind of object (Ray Tracing > Advanced). A
		/// change re-captures everything (bounded per frame like any capture).
		void Configure(const SceneConfig& a_config)
		{
			if (!(a_config == config_)) {
				config_ = a_config;
				RequestFlush();
			}
		}
		[[nodiscard]] const SceneConfig& GetConfig() const { return config_; }
		/// (45j) Alpha-tested shapes may be traced with their cut-out (some category asks for it).
		/// (46c) Also the window masks of a captured interior.
		[[nodiscard]] bool AlphaWanted() const
		{
			return config_.foliage != FoliageMode::ShadowMap || config_.cutouts || (config_.windowMasks && interiorEnabled_ && stats_.interior);
		}
		/// (45j) CPU split of the last Update / CollectInstances / DescribeMissedCasters / EndFrame.
		[[nodiscard]] const FrameTimes& GetFrameTimes() const { return times_; }

	private:
		// (45e) Objects: one BLAS per distinct engine mesh (in the shape's own space), one TLAS
		// instance per shape with the shape's world transform of the frame.
		struct MeshKey
		{
			const void* data = nullptr;  // BSGraphics::TriShape, shared by every copy of a mesh
			std::uint32_t vertices = 0;
			std::uint32_t triangles = 0;
			std::uint8_t kind = 0;       // (45h) 0 opaque, 1 alpha-tested, 2 alpha-tested with vertex alpha
			bool operator==(const MeshKey&) const = default;
		};
		struct MeshKeyHash
		{
			std::size_t operator()(const MeshKey& a_key) const noexcept
			{
				return std::hash<const void*>()(a_key.data) ^ (static_cast<std::size_t>(a_key.vertices) << 20) ^ a_key.triangles ^
				       (static_cast<std::size_t>(a_key.kind) << 40);
			}
		};
		struct SharedMesh
		{
			RayTracingRT::BlasHandle blas = RayTracingRT::kInvalidBlas;
			std::uint32_t users = 0;
			std::uint32_t triangles = 0;
		};
		struct ObjectShape
		{
			RE::NiPointer<RE::BSGeometry> geometry;  // holds the mesh (the key) alive too
			MeshKey key;
			RayTracingRT::BlasHandle blas = RayTracingRT::kInvalidBlas;
			bool registered = false;  // in the caster filter
			bool ready = false;       // (45j) BLAS (and texture) ready: in the TLAS
			bool shown = false;       // (46g) handed off: in the TLAS and (unless Foliage Both) out of the cascades
			bool twoSided = false;    // (46g) drawn two-sided by the engine: its instance keeps triangle-cull-disable (beam rays cull)
			bool foliage = false;     // (45j) a vegetation cut-out (Foliage setting)
			// (45h) Alpha-tested: its texture and record (both released with the shape).
			bool alpha = false;
			RayTracingRT::AlphaTexture texture = RayTracingRT::kInvalidAlphaTexture;
			std::uint32_t info = RayTracingRT::kInvalidAlphaInfo;
		};

		struct TrackedRef
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			const RE::NiAVObject* root = nullptr;
			bool disabled = false;
		};

		struct Cell
		{
			RE::TESObjectCELL* cell = nullptr;
			RE::FormID formID = 0;
			std::int32_t x = 0, y = 0;
			bool gathering = true;
			bool landDone = false;
			std::size_t cursor = 0;
			bool snapshotTaken = false;
			bool dirty = false;
			std::uint32_t seenFrame = 0;
			std::uint32_t completedFrame = 0;
			const void* landSignature = nullptr;
			std::vector<RE::NiPointer<RE::TESObjectREFR>> pending;
			std::vector<TrackedRef> tracked;
			std::vector<RE::NiPointer<RE::BSGeometry>> geometries;
			std::unique_ptr<RayTracingRT::ChunkGeometry> current;
			std::vector<RayTracingRT::BlasHandle> blas;
			// (45f) Per BLAS: the range of `geometries` it was made from (for the self-check).
			std::vector<std::pair<std::uint32_t, std::uint32_t>> chunkGeometries;
			std::vector<std::uint8_t> chunkTwoSided;  // (46g) per BLAS: a two-sided shape is in it (keeps triangle-cull-disable)
			std::uint32_t chunkStart = 0;  // first geometry of the chunk being filled
			// (45j) Resumable capture: shapes of pending[cursor] already taken, and its 3D then.
			std::uint32_t shapeSkip = 0;
			const RE::NiAVObject* resumeRoot = nullptr;
			// (45h) Alpha-tested shapes of the cell: own BLAS per mesh and instance per shape (like objects).
			std::vector<ObjectShape> alphaShapes;
			bool shown = false;  // (46g) handed off: its chunks are traced (all built), its shapes leave the cascades
			bool filterRegistered = false;
			bool replaces = false;   // (45e) a re-capture of a cell whose old capture is still live
			bool replacing = false;  // (45e) an old capture waiting for its replacement
			std::uint32_t shapes = 0;
			std::uint32_t landQuads = 0;
			std::uint64_t triangles = 0;
			double gatherMs = 0.0;
			// (46c) An interior cell: x / y are only the origin of its chunks (the 4096 square the player entered in);
			// its extent is the box of what was captured.
			bool interior = false;
			bool hasBounds = false;
			double boundsMin[3]{}, boundsMax[3]{};
			void AddBounds(const RE::NiBound& a_bound)
			{
				if (!std::isfinite(a_bound.radius) || a_bound.radius <= 0.0f || a_bound.radius > 100000.0f || !std::isfinite(a_bound.center.x) ||
					!std::isfinite(a_bound.center.y) || !std::isfinite(a_bound.center.z))
					return;
				const double c[3] = { a_bound.center.x, a_bound.center.y, a_bound.center.z };
				for (int a = 0; a < 3; ++a) {
					boundsMin[a] = hasBounds ? std::min(boundsMin[a], c[a] - a_bound.radius) : c[a] - a_bound.radius;
					boundsMax[a] = hasBounds ? std::max(boundsMax[a], c[a] + a_bound.radius) : c[a] + a_bound.radius;
				}
				hasBounds = true;
			}
		};

		void Release(RayTracingRT::Backend& a_backend, Cell& a_cell);
		/// Moves the cells matching a_predicate out of cells_ into retiring_.
		template <class Predicate>
		void RetireIf(Predicate a_predicate)
		{
			std::vector<std::unique_ptr<Cell>> keep;
			keep.reserve(cells_.size());
			for (auto& cell : cells_) {
				if (a_predicate(*cell))
					retiring_.push_back(std::move(cell));
				else
					keep.push_back(std::move(cell));
			}
			cells_ = std::move(keep);
		}
		static const void* LandSignature(RE::TESObjectCELL* a_cell);
		bool Validate(const Cell& a_cell) const;
		/// (46d) An interior cell is loaded: attached, or the player stands in it with its 3D loaded.
		static bool InteriorLoaded(const RE::TESObjectCELL* a_cell);
		/// Captures until the deadline; true when the cell is finished.
		bool Gather(RayTracingRT::Backend& a_backend, Cell& a_cell, std::int64_t a_deadline);
		void CaptureObject(Cell& a_cell, RE::NiAVObject* a_object, int a_depth);
		/// (45j) Captures a reference, or the part of it that fits before a_deadline (at least one
		/// shape); false = not finished, resumed from the next shape on the next frame.
		bool CaptureResumable(Cell& a_cell, RE::NiAVObject* a_root, std::int64_t a_deadline);
		std::uint32_t leafIndex_ = 0, leafSkip_ = 0;
		bool gatherAborted_ = false;
		std::int64_t gatherDeadline_ = 0;
		bool AppendShape(Cell& a_cell, RE::BSTriShape* a_shape, bool a_landscape);
		/// Decodes the positions (world minus the origin, or local with a_local) after checking
		/// them against the shape's world bound; false = left out (counted in the stats).
		bool PlaceVertices(RE::BSGeometry* a_shape, const std::uint8_t* a_bytes, std::uint32_t a_count, std::uint32_t a_stride,
			std::uint32_t a_positionOffset, bool a_fullPrecision, double a_originX, double a_originY, std::vector<float>& o_positions,
			bool a_local);
		void CloseChunk(RayTracingRT::Backend& a_backend, Cell& a_cell);

		/// The engine's CPU copy of a shape's mesh, read and validated.
		struct RawMesh
		{
			std::vector<std::uint8_t> vertexBytes;
			std::vector<std::uint32_t> indices;
			std::uint32_t vertexCount = 0;
			std::uint32_t triangleCount = 0;
			std::uint32_t stride = 0;
			std::uint32_t positionOffset = 0;
			bool fullPrecision = false;
			std::uint32_t uvOffset = UINT32_MAX;     // (45h) half2 texcoord 0, when the mesh has one
			std::uint32_t colorOffset = UINT32_MAX;  // (45h) ubyte4 vertex colour, when the mesh has one
		};
		bool ReadMesh(RE::BSTriShape* a_shape, bool a_landscape, RawMesh& o_mesh);
		/// The shape if it may go into the scene (Lighting shader, opaque, casts shadows, ...); the
		/// reason is counted otherwise.
		RE::BSTriShape* CheckShape(RE::NiAVObject* a_object, bool a_allowTreeAnim, bool a_allowCutout, std::uint8_t a_alphaKinds = 0,
			bool* o_alpha = nullptr, bool* o_foliage = nullptr);
		/// (45f) CheckShape's verdict without counting: nullptr = accepted, else the reason.
		/// (45g) a_allowCutout: alpha-tested shapes passing CutoutAsOpaque's criteria are accepted;
		/// o_reason (optional) receives the reason's MissReason bucket.
		/// (45h) a_alphaKinds (45j: kAlphaFoliage / kAlphaCutouts): alpha-tested shapes whose cut-out can
		/// be traced (texture, texcoords, a "greater" test) are accepted as alpha shapes (*o_alpha = true),
		/// tree-animated ones too; *o_foliage tells vegetation (leaves, bushes) from other cut-outs.
		static const char* ShapeRejectReason(RE::NiAVObject* a_object, bool a_allowTreeAnim, bool a_allowCutout, SkipCounts* o_counts,
			MissReason* o_reason = nullptr, std::uint8_t a_alphaKinds = 0, bool* o_alpha = nullptr, bool* o_foliage = nullptr);
		static constexpr std::uint8_t kAlphaFoliage = 1, kAlphaCutouts = 2;
		static constexpr std::uint8_t kAlphaMasks = 4;  // (46c) Window Shadows Ultimate window masks (interiors)
		/// (46c) A Window Shadows Ultimate window mask: its diffuse texture is under textures\masks\.
		static bool IsWindowMask(RE::BSGeometry* a_geometry);
		/// (45j) Vegetation (leaves, bushes, plants): tree-animated, owned by a tree / flora / grass
		/// reference, or a plant word in its texture, its name or its model path. Such shapes are never
		/// taken as opaque occluders (their cut-out is real), and they follow the Foliage setting.
		static bool IsVegetation(RE::BSGeometry* a_geometry);
		/// (46g) The engine draws it two-sided (BSLightingShaderProperty kTwoSided).
		static bool IsTwoSided(const RE::BSGeometry* a_geometry);
		[[nodiscard]] std::uint8_t AlphaKinds() const
		{
			return static_cast<std::uint8_t>((config_.foliage != FoliageMode::ShadowMap ? kAlphaFoliage : 0) | (config_.cutouts ? kAlphaCutouts : 0) |
											 (config_.windowMasks && gatherInterior_ ? kAlphaMasks : 0));
		}
		bool gatherInterior_ = false;  // (46c) the cell being captured is an interior
		bool interiorEnabled_ = false;  // (46c)
		/// (45h) The shape's cut-out as the ray tracer needs it: the game's diffuse texture, threshold,
		/// material alpha and texcoord transform; false when it cannot be traced (SEH-guarded).
		struct AlphaSource
		{
			ID3D11Texture2D* texture = nullptr;
			ID3D11ShaderResourceView* view = nullptr;
			RayTracingRT::AlphaMaterial material{};
			bool vertexAlpha = false;  // the Lighting shader multiplies the vertex colour's alpha in
		};
		static bool GetAlphaSource(RE::BSGeometry* a_geometry, AlphaSource& o_source);
		/// (45h) An alpha-tested shape as an object shape (shared non-opaque mesh BLAS, texture,
		/// record); false = not possible (counted), the caller then applies the 45g rules.
		bool CaptureAlphaShape(RayTracingRT::Backend& a_backend, RE::BSTriShape* a_shape, bool a_allowNewMeshes, ObjectShape& o_shape);
		/// Releases an object / alpha shape: filter entry, mesh reference, texture and record.
		void ReleaseShape(RayTracingRT::Backend& a_backend, ObjectShape& a_shape);
		/// Appends the TLAS instance of an object / alpha shape (when it is drawn and, for alpha shapes, registered).
		static void AppendShapeInstance(const ObjectShape& a_shape, RayTracingRT::InstanceCategory a_category, std::uint32_t a_refID, bool a_expected,
			std::vector<RayTracingRT::Instance>& o_instances);
		/// (45g) Statics with a Havok-driven part (a node below the root with its own collision
		/// object: hinged signs and the like) move: they get per-frame object instances.
		static bool HasPhysicsPart(RE::NiAVObject* a_object, int a_depth);
		/// (45g) Children of a node that the game draws (NiSwitchNode: the active one only); false
		/// for camera-facing billboard nodes (their transform follows the main camera).
		bool VisitChildren(RE::NiNode* a_node);
		/// (45f) The grid cell's references plus the persistent-cell references standing in its square.
		void CollectCellRefs(RE::TESObjectCELL* a_cell, std::int32_t a_x, std::int32_t a_y, std::vector<RE::NiPointer<RE::TESObjectREFR>>& o_refs,
			std::uint32_t* o_persistent) const;
		/// (45f) The captured cell whose square holds the reference (its parent cell, or by position
		/// for references of the persistent cell); nullptr when none.
		[[nodiscard]] const Cell* CapturedCellOf(RE::TESObjectREFR* a_ref) const;

		static constexpr std::uint32_t kMissedLogLimit = 32;
		struct ObjectRef
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			const RE::NiAVObject* root = nullptr;
			ObjectKind kind = ObjectKind::Movable;
			std::vector<ObjectShape> shapes;
		};

		void UpdateObjects(RayTracingRT::Backend& a_backend, std::int64_t a_deadline, bool a_allowNewMeshes);
		struct ObjectCandidate
		{
			RE::BSTriShape* shape;
			bool alpha;    // taken as an alpha shape (45h)
			bool foliage;  // a vegetation cut-out (45j)
		};
		void CollectObjectShapes(RE::NiAVObject* a_object, bool a_allowTreeAnim, bool a_allowCutout, int a_depth, std::vector<ObjectCandidate>& o_shapes);
		void CaptureObjectRef(RayTracingRT::Backend& a_backend, const RE::NiPointer<RE::TESObjectREFR>& a_ref, RE::NiAVObject* a_root,
			ObjectKind a_kind, bool a_allowNewMeshes);
		void ReleaseObject(RayTracingRT::Backend& a_backend, ObjectRef& a_object);
		void AppendObjectInstances(const std::vector<std::unique_ptr<ObjectRef>>& a_objects, std::vector<RayTracingRT::Instance>& o_instances,
			bool a_expected) const;

		std::vector<std::unique_ptr<Cell>> cells_;
		std::vector<std::unique_ptr<Cell>> retiring_;  // traced this frame, released in EndFrame
		std::vector<std::unique_ptr<ObjectRef>> objects_;
		std::vector<std::unique_ptr<ObjectRef>> retiringObjects_;  // traced this frame, released in EndFrame
		ankerl::unordered_dense::map<const RE::TESObjectREFR*, ObjectRef*> objectIndex_;  // references in objects_
		std::unordered_map<MeshKey, SharedMesh, MeshKeyHash> meshes_;
		std::vector<RE::NiPointer<RE::TESObjectREFR>> scanList_;  // the cell being scanned for objects
		std::size_t scanCursor_ = 0;
		std::size_t scanCell_ = 0;
		std::atomic<bool> flushRequested_{ false };
		// (46g) Hand-off hold and the cells of the place being left.
		bool hold_ = false;
		bool transition_ = false;
		bool departingPending_ = false;  // the next Update takes the attached cells as departing (after a flush)
		std::vector<const RE::TESObjectCELL*> departing_;
		std::uint32_t departingSkipped_ = 0;
		std::size_t validationCursor_ = 0;
		std::uint32_t cellsReady_ = 0;
		Stats stats_{};
		RayTracingRT::Backend* gatherBackend_ = nullptr;
		std::uint32_t currentFrame_ = 0;
		std::uint32_t missedRejectedLogged_ = 0;  // (45f) rejected-shape misses described in the log
		// (45g) Missed-caster bookkeeping.
		static constexpr std::uint32_t kMissedLogCap = 200;      // log lines per session
		static constexpr std::uint32_t kMissedLogBurst = 8;      // new lines per window
		static constexpr double kMissedLogWindowMs = 10000.0;
		ankerl::unordered_dense::set<const RE::BSGeometry*> missedJudged_;  // judged in this area (counted once)
		ankerl::unordered_dense::set<std::uint64_t> missedLogged_;          // reference + shape name, whole session
		ankerl::unordered_dense::set<const RE::TESObjectREFR*> recaptureAsked_;  // never-captured refs given one recapture
		std::deque<std::string> missedBacklog_;  // judged misses waiting for the log's rate limit
		std::int64_t missedWindowStart_ = 0;
		std::uint32_t missedWindowCount_ = 0;
		SceneConfig config_{};  // (45j)
		mutable FrameTimes times_{};  // (45j)
		// (45j) Indices for the missed-caster diagnosis (no linear walks over every captured shape).
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> capturedGeometries_;
		struct TrackedEntry
		{
			std::uint32_t count = 0;
			const RE::NiAVObject* root = nullptr;  // of the latest capture
		};
		ankerl::unordered_dense::map<const RE::TESObjectREFR*, TrackedEntry> trackedRefs_;
		void IndexGeometry(const RE::BSGeometry* a_geometry, bool a_add);
		// (45j) Persistent references of the worldspace by cell square, rebuilt only when the
		// persistent cell changes (the object scan used to walk all of them for every cell it scanned).
		struct PersistentIndex
		{
			const RE::TESObjectCELL* sky = nullptr;
			std::size_t references = SIZE_MAX;
			std::uint32_t builtFrame = 0;
			ankerl::unordered_dense::map<std::uint64_t, std::vector<RE::NiPointer<RE::TESObjectREFR>>> bySquare;
		};
		mutable PersistentIndex persistent_;
		void RefreshPersistent(bool a_force) const;
		// (45j) Engine objects whose last holder may be us: dropped a bounded number per frame (a
		// whole cell at once ran thousands of engine destructors on the render thread).
		std::vector<RE::NiPointer<RE::BSGeometry>> geometryGrave_;
		std::vector<RE::NiPointer<RE::TESObjectREFR>> refGrave_;
		void Bury(Cell& a_cell);
		void DrainGraves(double a_budgetMs);
		// (45g) Statics with a Havok-driven part (reference -> its 3D when judged): object path.
		ankerl::unordered_dense::map<const RE::TESObjectREFR*, const RE::NiAVObject*> physicsRefs_;
	};
}
