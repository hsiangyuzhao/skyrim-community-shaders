#pragma once

#include "Backend.h"
#include "Relevance.h"  // (47u)

#include <atomic>

/**
 * @brief (45i, RT characters) Skinned characters for the ray-traced sun shadows.
 *
 * Off (both switches off, the default) nothing here runs and the scene is exactly 45h's.
 *
 * Which actors: the player and the "high" process actors, nearest first, inside the traced area
 * (the captured cells) and within kEnterDistance of the camera (40 m; they leave beyond
 * kLeaveDistance), at most kMaxActors. Their third-person 3D (also for the player in first person:
 * that model is the one the game casts the shadow with).
 *
 * Which shapes: Lighting-shader shapes that cast shadows, are not decals / refraction / purely
 * alpha-blended, under the actor's 3D and not hidden:
 *  - skinned BSTriShape / BSDynamicTriShape (body, head, clothes, armour): vertices, bone indices
 *    and weights from the skin partition's CPU copy (dismembered partitions left out); a
 *    BSDynamicTriShape (the face) takes its morphed positions once, when captured;
 *  - rigid BSTriShape under the skeleton (weapons, shields, quivers): one "bone", the shape itself.
 * Alpha-tested parts (hair, fur trims) only with Alpha-tested casters on (traced with their
 * cut-out); otherwise they keep their shadow-map shadow. Physics parts -- HDT-SMP / HDT-PE shapes
 * (their 3D carries the HDT extra data) and hair -- only with the Physics parts switch.
 *
 * Bone matrices (bone world * skin-to-bone) are read every frame (SEH-guarded, every bone held by
 * an NiPointer) and the backend skins and refits each shape on the GPU. When a shape is first
 * captured its bone index interpretation (partition palette or global) is chosen by checking a
 * sample of skinned vertices against the engine's bounds; a shape that fits neither is left out.
 *
 * Lifetime: an actor that leaves (range, unload, new 3D, disabled) or a shape that changes (equip,
 * dismember, new skin) is "retiring": still traced this frame with its last bone matrices, released
 * in EndFrame() after the trace was submitted. Loading screens, interiors and switch changes drop
 * everything; the GPU resources go with the backend's fences (no CPU wait).
 *
 * Caster filter: a shape leaves the sun cascades once its BLAS is built (and, for an alpha part,
 * its texture is ready) unless "Keep Characters in the Shadow Map" is on; a shape that could not
 * be captured keeps its shadow-map shadow.
 *
 * (46m) CPU: the per-actor reads (scene graph, bone matrices) run on up to kReadLanes lanes (this thread
 * and a few workers that only read the engine's nodes; joined before anything uses them, at the same point
 * of the frame as before); with the scan cache an actor's 3D is walked in full only when it may have
 * changed, its known shapes are checked every frame. Merged: an actor's opaque shapes are one BLAS
 * (SkinDraw::group = Actor::serial); a shape whose matrices are of another origin is rebased.
 *
 * Main / render thread only (NoteEquip: any thread).
 */
namespace RayTracingCharacters
{
	inline constexpr std::uint32_t kMaxActors = 16;
	inline constexpr float kEnterDistance = 2840.0f;  // 40 m
	inline constexpr float kLeaveDistance = 3200.0f;
	inline constexpr std::uint32_t kMaxShapesPerActor = 64;
	// (46m) Character Scan Cache: every other actor's 3D is scanned in full every kRescanFrames frames (staggered; the
	// player every frame), and for kEquipScanFrames frames after it equipped / unequipped something.
	inline constexpr std::uint32_t kRescanFrames = 8;
	inline constexpr std::uint32_t kEquipScanFrames = 30;
	inline constexpr std::uint32_t kReadLanes = 4;  // (46m) per-actor reads: this thread + 3 workers
	// (47q) The NPC rule: an NPC turns body-only beyond the distance + this, back to all its parts within the distance - this.
	inline constexpr float kBodyOnlyHysteresis = 105.0f;  // 1.5 m

	struct Stats
	{
		std::uint32_t actors = 0;          // tracked now
		std::uint32_t inRange = 0;         // actors in range this frame (before the cap)
		std::uint32_t shapes = 0;          // captured shapes now
		std::uint32_t registered = 0;      // ... traced and out of the shadow map
		std::uint32_t skinnedShapes = 0;
		std::uint32_t rigidShapes = 0;
		std::uint32_t physicsShapes = 0;
		std::uint32_t alphaShapes = 0;
		std::uint64_t vertices = 0;        // of the captured shapes
		std::uint32_t retiredShapes = 0;   // total: equip / dismember / unload changes
		// Left to the shadow map (totals since the scene was dropped; each distinct shape once).
		std::uint32_t skipPhysicsOff = 0;  // physics part with the switch off
		std::uint32_t skipHairOff = 0;     // (45j) hair with Hair on Shadow map
		std::uint32_t hairByTint = 0;      // (45j) hair recognised by its material (hair tint), by its name / texture, by HDT data
		std::uint32_t hairByName = 0;
		std::uint32_t hairShapes = 0;      // (45j) hair shapes traced now
		std::uint32_t skipAlphaOff = 0;    // alpha-tested part without Alpha-tested casters
		std::uint32_t skipAlphaFailed = 0; // alpha-tested part whose texture / record failed
		std::uint32_t skipShader = 0;      // decal, effect shader, alpha-blended, no shadow, refraction
		std::uint32_t skipGeometryType = 0;
		std::uint32_t skipNoCpuData = 0;   // no CPU copy of the vertices / indices
		std::uint32_t skipBadData = 0;     // index or bone out of range, unknown vertex layout, limits
		std::uint32_t skipPlacement = 0;   // skinned vertices outside the engine's bounds
		std::uint32_t skipBackend = 0;     // refused by the backend (VRAM budget, size)
		std::uint32_t skipShapeCap = 0;    // past kMaxShapesPerActor
		std::uint32_t skipTinyFace = 0;    // (47p) eyes, brows, mouth left out (they cast nothing visible)
		// (47q) The NPC rule, this frame: NPCs traced with their merged body only, and of them those whose merged BLAS was not refit
		// (their off frame: last frame's pose, moved with the actor).
		std::uint32_t bodyOnlyActors = 0;
		std::uint32_t refitsSkipped = 0;
		std::uint32_t paletteMode = 0;     // shapes whose bone indices are partition-local (palette)
		std::uint32_t globalMode = 0;      // ... global
		std::uint32_t dynamicShapes = 0;   // BSDynamicTriShape positions taken at capture
		float lastMs = 0.0f;               // CPU this frame
		float maxMs = 0.0f;
		// (46m) This frame's CPU, step by step (ms; F7 rt_ch_*): the actors to trace; the per-actor reads (scene graph
		// scan or check of the known shapes, bone matrices; on the read lanes); new shapes captured / changed ones retired;
		// still-pose check; caster filter (CollectDraws: LastCollectMs). Counts: bone matrices read, actors whose 3D was scanned in full,
		// lanes the reads ran on (1 = this thread alone).
		float candidatesMs = 0.0f;
		float readsMs = 0.0f;
		float captureMs = 0.0f;
		float commitMs = 0.0f;
		float filterMs = 0.0f;
		std::uint32_t bonesRead = 0;
		std::uint32_t rescans = 0;
		std::uint32_t lanes = 0;
	};

	class Characters
	{
	public:
		Characters() = default;
		Characters(const Characters&) = delete;
		Characters& operator=(const Characters&) = delete;
		/// Process exit: the game's objects are not released (its memory manager may be gone already).
		~Characters();

		/// a_physics: HDT-SMP / HDT-PE parts (capes, skirts); a_hair (45j): hair, split from the physics
		/// parts; a_alpha: alpha-tested parts with their cut-out (the alpha pipeline exists);
		/// a_firstPersonPlayer (45j): the player is traced in first person too (off: in first person it stays
		/// in the shadow map). A change of the first three drops everything (next Update).
		/// (46m, always since 47m) The per-actor reads run on a few read lanes (joined before anything uses them); an
		/// actor's 3D is scanned in full only when it may have changed (equip, a new actor) and every kRescanFrames
		/// frames, the known shapes are checked every frame; an actor's opaque shapes are one merged BLAS.
		/// (47p) a_dropTiny: tiny face parts (eyes, brows, mouth) are not traced (a change drops everything, like the first three).
		void Configure(bool a_physics, bool a_hair, bool a_alpha, bool a_firstPersonPlayer, bool a_dropTiny);
		/// (47p) Bone matrices within float noise of the last skinned ones count as a still pose (no re-skinning).
		void SetBonesTolerant(bool a_on) { bonesTolerant_ = a_on; }
		/// (47q) The NPC rule: NPCs farther than this from the camera (with kBodyOnlyHysteresis) are traced with their merged opaque body /
		/// armour only -- physics parts, hair and cut-out parts leave the TLAS and go back to the engine cascades -- and their merged BLAS
		/// is refit every other frame (staggered by actor). The player and nearer NPCs: every part, every frame. 0 = no limit.
		void SetBodyOnlyDistance(float a_distance) { bodyOnlyDistance_ = std::isfinite(a_distance) ? std::max(a_distance, 0.0f) : 0.0f; }
		/// Drops everything on the next Update (loading screens, from any thread).
		void RequestFlush() { flushRequested_.store(true); }
		/// (46m) An actor equipped / unequipped something (any thread): its 3D is scanned in full for the next frames.
		static void NoteEquip(const void* a_reference);
		/// a_rect: world xy min / max of the traced area (actors outside are not taken).
		/// (46g) a_budgetMs / a_maxCaptures: new shapes read this frame (larger behind loading / black screens).
		/// (46k) a_eye: the point actors are sorted by / chosen within (default the camera; behind a loading screen the camera is
		/// still at the place left, so the loading pump passes the player's position).
		void Update(RayTracingRT::Backend& a_backend, const float a_rect[4], double a_budgetMs = 1.0, std::uint32_t a_maxCaptures = 6,
			const RE::NiPoint3* a_eye = nullptr);
		/// This frame's draws (retiring ones too) and their bone matrices.
		void CollectDraws(std::vector<RayTracingRT::SkinDraw>& o_draws, std::vector<float>& o_bones) const;
		/// (46e) The characters of the last CollectDraws: reference -> key (SkinDraw::owner, 1..), sorted by pointer.
		[[nodiscard]] const std::vector<std::pair<const void*, std::uint32_t>>& Owners() const { return owners_; }
		/// After this frame's trace was submitted (or skipped): releases the retiring shapes.
		void EndFrame(RayTracingRT::Backend& a_backend);
		void ReleaseAll(RayTracingRT::Backend& a_backend);
		[[nodiscard]] bool Empty() const { return actors_.empty() && retiring_.empty(); }
		[[nodiscard]] const Stats& GetStats() const { return stats_; }
		/// (45l) Bounding spheres of the traced characters, camera-relative xyz + radius (radius 0 = none):
		/// the player, and one sphere around every other actor. For the trace's character-ray culling.
		void GetBounds(const double a_camera[3], float o_player[4], float o_others[4]) const;
		void ResetPeaks() { stats_.maxMs = 0.0f; }
		/// (46m) CPU ms of the last CollectDraws.
		[[nodiscard]] float LastCollectMs() const { return collectMs_; }
		/// (45n) Still poses: a shape whose bone matrices are bit-identical to the ones it was last skinned
		/// with keeps its skinned BLAS (no upload, skinning or refit). Count of last frame:
		[[nodiscard]] std::uint32_t StillReusedLastFrame() const { return stillReused_; }
		/// (47u) Skip Updates That Cannot Affect the Image: the early region for the next Update (null = nothing culled). An NPC (never the
		/// player) whose current bounds and the bounds of the pose its BLAS holds stay outside it for kCullAfterFrames Updates keeps its
		/// skinned BLASes (moved with its root) without skinning or refits, as a body-only NPC on its off frame.
		void SetRelevance(const RayTracingRelevance::Region* a_region) { relevance_ = a_region; }
		/// (47u) Whether a_exact (this frame's rays) reaches an NPC culled by the last Update.
		[[nodiscard]] bool CulledReachable(const RayTracingRelevance::Region& a_exact) const;
		/// (47u) NPCs culled by the last Update.
		[[nodiscard]] std::uint32_t CulledLastUpdate() const { return culledActors_; }

	private:
		static constexpr std::uint32_t kCullAfterFrames = 8;  // (47u)
		const RayTracingRelevance::Region* relevance_ = nullptr;
		std::uint32_t culledActors_ = 0;
		struct Bone
		{
			RE::NiPointer<RE::NiAVObject> node;
			RE::NiTransform skinToBone;
		};
		struct Shape
		{
			RE::NiPointer<RE::BSGeometry> geometry;
			RE::NiPointer<RE::NiSkinInstance> skin;  // held: its bone list and partitions stay valid
			std::uint64_t signature = 0;
			RayTracingRT::SkinHandle handle = RayTracingRT::kInvalidSkin;
			std::vector<Bone> bones;
			bool physics = false;
			bool hair = false;  // (45j)
			bool alpha = false;
			RayTracingRT::AlphaTexture texture = RayTracingRT::kInvalidAlphaTexture;
			std::uint32_t info = RayTracingRT::kInvalidAlphaInfo;
			bool registered = false;
			bool drawable = false;          // in the TLAS (an alpha part only once its texture is ready)
			bool reuse = false;             // this frame: last frame's skinned BLAS (still pose)
			std::uint32_t vertices = 0;
			std::vector<float> matrices;    // last bone matrices (12 floats each), relative to origin
			double origin[3]{};
			// (46m) This frame's read (per-actor reads, possibly on a read lane): the matrices, whether they could be read.
			std::vector<float> fresh;
			bool freshOk = false;
			bool grouped = false;  // (46m) in its actor's merged BLAS (opaque, Merge Character Parts when captured)
			/// (47q) Part of a body-only NPC's trace: in the merged BLAS and neither a physics part nor hair.
			[[nodiscard]] bool BodyPart() const { return grouped && !physics && !hair; }
		};
		struct Found
		{
			RE::BSGeometry* geometry;
			std::uint64_t signature;
		};
		struct Actor
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			RE::NiPointer<RE::NiAVObject> root;
			bool player = false;
			std::vector<std::unique_ptr<Shape>> shapes;
			std::vector<std::unique_ptr<Shape>> retiringShapes;  // traced this frame, released in EndFrame
			ankerl::unordered_dense::set<std::uint64_t> refused;  // signatures of shapes judged not capturable (counted once)
			// (46m) Its merged BLAS's key (unique per Actor entry: a new 3D is a new entry), the origin of this frame's bone
			// matrices, full scans until this frame (equip, new), this frame's shapes found (scan or check) and bones read.
			std::uint32_t serial = 0;
			double origin[3]{};
			std::uint64_t scanUntil = 0;
			std::vector<Found> found;
			std::uint32_t bonesRead = 0;
			bool scanned = false;
			bool readOk = false;  // this frame's reads completed (otherwise its shapes stay as they were)
			bool bodyOnly = false;  // (47q) the NPC rule (with hysteresis)
			// (47u) Skip Updates: frames outside the early region, culled by this Update, and the root's bound sphere (relative to the origin)
			// of the last Update that posed it (the pose its BLASes hold while culled, moved with the origin).
			std::uint32_t outsideFrames = 0;
			bool culled = false;
			bool skinBoundValid = false;
			double skinBoundOffset[3]{};
			double skinBoundRadius = 0.0;
		};
		/// (47u) The world boxes an NPC's BLASes can occupy: its root's bound now and the posed bound moved to its origin (+ kMargin).
		static bool ActorBoxes(const Actor& a_actor, double o_lo[2][3], double o_hi[2][3]);

		void ReleaseShape(RayTracingRT::Backend& a_backend, Shape& a_shape);
		void ReleaseActor(RayTracingRT::Backend& a_backend, Actor& a_actor);
		static void Enumerate(Actor& a_actor, std::vector<Found>& o_found);
		/// (46m) One actor's reads (any read lane; touches nothing but this actor's entry): its shapes (a full scan, or a
		/// check of the known ones: still attached under the root, not hidden, signature), its known shapes' bone matrices.
		static void ReadActor(Actor& a_actor, bool a_scan);
		/// Reads, checks and hands a shape to the backend; nullptr when it is not taken (counted).
		std::unique_ptr<Shape> Capture(RayTracingRT::Backend& a_backend, Actor& a_actor, RE::BSGeometry* a_geometry, std::uint64_t a_signature);
		/// This frame's bone matrices; false when a bone could not be read (the last ones are kept).
		static bool ReadBones(const Shape& a_shape, const double a_origin[3], std::vector<float>& o_matrices);

		std::vector<std::unique_ptr<Actor>> actors_;
		std::vector<std::unique_ptr<Actor>> retiring_;  // traced this frame, released in EndFrame
		mutable std::vector<std::pair<const void*, std::uint32_t>> owners_;  // (46e) CollectDraws
		mutable float collectMs_ = 0.0f;  // (46m)
		std::atomic<bool> flushRequested_{ false };
		bool physics_ = false;
		bool hair_ = false;               // (45j)
		bool firstPersonPlayer_ = false;  // (45j)
		bool alpha_ = false;
		bool dropTiny_ = false;  // (47p)
		bool bonesTolerant_ = false;  // (47p) "Ray tracing: ignore bone noise"
		float bodyOnlyDistance_ = 0.0f;  // (47q) 0 = no limit
		std::uint32_t stillReused_ = 0;  // (45n)
		std::uint64_t frame_ = 0;
		std::uint32_t actorSerial_ = 0;
		std::vector<std::pair<float, RE::Actor*>> candidates_;  // scratch
		std::uint32_t filterEpoch_ = 0;
		std::uint32_t loggedPlacement_ = 0;
		std::uint32_t loggedOther_ = 0;
		Stats stats_{};
	};
}
