#pragma once

#include "Backend.h"

/**
 * @brief (45, RT prototype) Scene-graph capture of the loaded exterior cells.
 *
 * Each frame the TES grid is compared with the cells already captured: new cells are captured
 * over the next frames (a time budget per frame, nearest cells first), cells that left the grid
 * are dropped (their BLASes retire once the GPU is done with them). A captured cell is a few
 * BLASes of up to kChunkTriangles each, positions relative to the cell's south-west corner, so a
 * cell needs exactly one TLAS instance per chunk with an identity rotation.
 *
 * What goes in: references whose base is a Static, Furniture or Container, and the cell's four
 * landscape quadrants -- only BSTriShape geometry drawn with the Lighting shader that casts
 * shadows and is neither alpha-tested, alpha-blended, skinned, a decal nor tree-animated.
 * Everything else is counted per reason (shown on the page) and keeps its shadow-map shadow.
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

	struct SkipCounts
	{
		std::uint32_t actors = 0;          // NPCs and creatures (skinned, not in the prototype)
		std::uint32_t movable = 0;         // doors, activators, movable statics, lights: they can move
		std::uint32_t foliage = 0;         // trees, flora, tree-animated shapes
		std::uint32_t items = 0;           // other reference types (items, markers, sounds, ...)
		std::uint32_t disabled = 0;        // disabled / deleted references or no 3D
		std::uint32_t hidden = 0;          // hidden nodes and editor markers
		std::uint32_t otherGeometry = 0;   // particles, dynamic / multi-index / sub-index shapes
		std::uint32_t notLighting = 0;     // effect, water, sky shaders
		std::uint32_t alphaTested = 0;
		std::uint32_t alphaBlended = 0;
		std::uint32_t decals = 0;
		std::uint32_t skinned = 0;
		std::uint32_t noShadow = 0;        // shape does not cast shadows in the game either
		std::uint32_t noCpuData = 0;       // the engine kept no CPU copy of the vertices
		std::uint32_t badData = 0;         // index out of range, unknown vertex layout
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
		}

		/// Drops everything (BLASes are retired through the backend, filter entries removed).
		void ReleaseAll(RayTracingRT::Backend& a_backend);
		/// Asks the next Update to drop everything (loading screens, from the UI thread too).
		void RequestFlush() { flushRequested_.store(true); }

		/// Per frame: grid diff, re-validation, a_budgetMs of capture, BLAS creation, filter registration.
		void Update(RayTracingRT::Backend& a_backend, float a_budgetMs, std::uint32_t a_frame);

		/// TLAS instances of every chunk (the backend skips the ones not built yet).
		void CollectInstances(std::vector<RayTracingRT::Instance>& o_instances) const;

		/// After this frame's trace was submitted (or skipped): releases the retiring captures.
		void EndFrame(RayTracingRT::Backend& a_backend);

		/// World-space xy rect of the captured cells (min x, min y, max x, max y); false when none.
		/// a_includeRetiring: also the cells still traced this frame but released at its end.
		bool GetLoadedRect(float o_rect[4], bool a_includeRetiring) const;
		/// True once at least one cell is captured with all its BLASes built.
		[[nodiscard]] bool HasCoverage() const { return cellsReady_ > 0; }

		[[nodiscard]] const Stats& GetStats() const { return stats_; }
		void ResetPeaks() { stats_.maxGatherMs = 0.0f; }

	private:
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
			bool filterRegistered = false;
			bool replaces = false;   // (45e) a re-capture of a cell whose old capture is still live
			bool replacing = false;  // (45e) an old capture waiting for its replacement
			std::uint32_t shapes = 0;
			std::uint32_t landQuads = 0;
			std::uint64_t triangles = 0;
			double gatherMs = 0.0;
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
		/// Captures until the deadline; true when the cell is finished.
		bool Gather(RayTracingRT::Backend& a_backend, Cell& a_cell, std::int64_t a_deadline);
		void CaptureObject(Cell& a_cell, RE::NiAVObject* a_object, int a_depth);
		bool AppendShape(Cell& a_cell, RE::BSTriShape* a_shape, bool a_landscape);
		/// Decodes the positions (world minus the origin, or local with a_local) after checking
		/// them against the shape's world bound; false = left out (counted in the stats).
		bool PlaceVertices(RE::BSGeometry* a_shape, const std::uint8_t* a_bytes, std::uint32_t a_count, std::uint32_t a_stride,
			std::uint32_t a_positionOffset, bool a_fullPrecision, double a_originX, double a_originY, std::vector<float>& o_positions,
			bool a_local);
		void CloseChunk(RayTracingRT::Backend& a_backend, Cell& a_cell);

		std::vector<std::unique_ptr<Cell>> cells_;
		std::vector<std::unique_ptr<Cell>> retiring_;  // traced this frame, released in EndFrame
		std::atomic<bool> flushRequested_{ false };
		std::size_t validationCursor_ = 0;
		std::uint32_t cellsReady_ = 0;
		Stats stats_{};
		RayTracingRT::Backend* gatherBackend_ = nullptr;
		std::uint32_t currentFrame_ = 0;
	};
}
