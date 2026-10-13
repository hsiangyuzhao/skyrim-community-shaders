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
	};

	class Scene
	{
	public:
		Scene() = default;
		Scene(const Scene&) = delete;
		Scene& operator=(const Scene&) = delete;
		/// Process exit: the game's objects are not released (its memory manager may be gone already).
		~Scene() { new std::vector<std::unique_ptr<Cell>>(std::move(cells_)); }

		/// Drops everything (BLASes are retired through the backend, filter entries removed).
		void ReleaseAll(RayTracingRT::Backend& a_backend);
		/// Asks the next Update to drop everything (loading screens, from the UI thread too).
		void RequestFlush() { flushRequested_.store(true); }

		/// Per frame: grid diff, re-validation, a_budgetMs of capture, BLAS creation, filter registration.
		void Update(RayTracingRT::Backend& a_backend, float a_budgetMs, std::uint32_t a_frame);

		/// TLAS instances of every chunk (the backend skips the ones not built yet).
		void CollectInstances(std::vector<RayTracingRT::Instance>& o_instances) const;

		/// World-space xy rect of the attached grid (min x, min y, max x, max y); false when none.
		bool GetLoadedRect(float o_rect[4]) const;
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
			std::uint32_t shapes = 0;
			std::uint32_t landQuads = 0;
			std::uint64_t triangles = 0;
			double gatherMs = 0.0;
		};

		void Release(RayTracingRT::Backend& a_backend, Cell& a_cell);
		static const void* LandSignature(RE::TESObjectCELL* a_cell);
		bool Validate(const Cell& a_cell) const;
		/// Captures until the deadline; true when the cell is finished.
		bool Gather(RayTracingRT::Backend& a_backend, Cell& a_cell, std::int64_t a_deadline);
		void CaptureObject(Cell& a_cell, RE::NiAVObject* a_object, int a_depth);
		bool AppendShape(Cell& a_cell, RE::BSTriShape* a_shape, bool a_landscape);
		void CloseChunk(RayTracingRT::Backend& a_backend, Cell& a_cell);

		std::vector<std::unique_ptr<Cell>> cells_;
		std::atomic<bool> flushRequested_{ false };
		std::size_t validationCursor_ = 0;
		std::uint32_t cellsReady_ = 0;
		Stats stats_{};
		RayTracingRT::Backend* gatherBackend_ = nullptr;
		std::uint32_t currentFrame_ = 0;
	};
}
