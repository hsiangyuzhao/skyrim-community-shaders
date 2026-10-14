#pragma once

#include "Backend.h"

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
 * Main / render thread only.
 */
namespace RayTracingCharacters
{
	inline constexpr std::uint32_t kMaxActors = 16;
	inline constexpr float kEnterDistance = 2840.0f;  // 40 m
	inline constexpr float kLeaveDistance = 3200.0f;
	inline constexpr std::uint32_t kMaxShapesPerActor = 64;

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
		std::uint32_t paletteMode = 0;     // shapes whose bone indices are partition-local (palette)
		std::uint32_t globalMode = 0;      // ... global
		std::uint32_t dynamicShapes = 0;   // BSDynamicTriShape positions taken at capture
		float lastMs = 0.0f;               // CPU this frame
		float maxMs = 0.0f;
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
		/// a_keepInShadowMap: never leave the cascades; a_firstPersonPlayer (45j): the player is traced
		/// in first person too (off: in first person it stays in the shadow map). A change of the first
		/// three drops everything (next Update).
		void Configure(bool a_physics, bool a_hair, bool a_alpha, bool a_keepInShadowMap, bool a_firstPersonPlayer);
		/// Drops everything on the next Update (loading screens, from any thread).
		void RequestFlush() { flushRequested_.store(true); }
		/// a_rect: world xy min / max of the traced area (actors outside are not taken).
		void Update(RayTracingRT::Backend& a_backend, std::uint32_t a_frame, const float a_rect[4]);
		/// This frame's draws (retiring ones too) and their bone matrices.
		void CollectDraws(std::vector<RayTracingRT::SkinDraw>& o_draws, std::vector<float>& o_bones) const;
		/// After this frame's trace was submitted (or skipped): releases the retiring shapes.
		void EndFrame(RayTracingRT::Backend& a_backend);
		void ReleaseAll(RayTracingRT::Backend& a_backend);
		[[nodiscard]] bool Empty() const { return actors_.empty() && retiring_.empty(); }
		[[nodiscard]] const Stats& GetStats() const { return stats_; }
		/// (45l) Other actors (not the player) at reduced quality: skinned and refit every other frame.
		void SetNpcReduced(bool a_reduced) { npcReduced_ = a_reduced; }
		/// (45l) Bounding spheres of the traced characters, camera-relative xyz + radius (radius 0 = none):
		/// the player, and one sphere around every other actor. For the trace's character-ray culling.
		void GetBounds(const double a_camera[3], float o_player[4], float o_others[4]) const;
		void ResetPeaks() { stats_.maxMs = 0.0f; }

	private:
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
			bool reuse = false;             // (45l) this frame: last frame's skinned BLAS (other actors, reduced quality)
			std::uint32_t vertices = 0;
			std::vector<float> matrices;    // last bone matrices (12 floats each), relative to origin
			double origin[3]{};
		};
		struct Actor
		{
			RE::NiPointer<RE::TESObjectREFR> ref;
			RE::NiPointer<RE::NiAVObject> root;
			bool player = false;
			std::vector<std::unique_ptr<Shape>> shapes;
			std::vector<std::unique_ptr<Shape>> retiringShapes;  // traced this frame, released in EndFrame
			ankerl::unordered_dense::set<std::uint64_t> refused;  // signatures of shapes judged not capturable (counted once)
		};
		struct Found
		{
			RE::BSGeometry* geometry;
			std::uint64_t signature;
		};

		void ReleaseShape(RayTracingRT::Backend& a_backend, Shape& a_shape);
		void ReleaseActor(RayTracingRT::Backend& a_backend, Actor& a_actor);
		void Enumerate(Actor& a_actor, std::vector<Found>& o_found);
		/// Reads, checks and hands a shape to the backend; nullptr when it is not taken (counted).
		std::unique_ptr<Shape> Capture(RayTracingRT::Backend& a_backend, Actor& a_actor, RE::BSGeometry* a_geometry, std::uint64_t a_signature);
		/// This frame's bone matrices; false when a bone could not be read (the last ones are kept).
		static bool ReadBones(const Shape& a_shape, const double a_origin[3], std::vector<float>& o_matrices);

		std::vector<std::unique_ptr<Actor>> actors_;
		std::vector<std::unique_ptr<Actor>> retiring_;  // traced this frame, released in EndFrame
		std::atomic<bool> flushRequested_{ false };
		bool physics_ = false;
		bool hair_ = false;               // (45j)
		bool firstPersonPlayer_ = false;  // (45j)
		bool alpha_ = false;
		bool keepInShadowMap_ = false;
		bool npcReduced_ = false;  // (45l)
		std::uint32_t filterEpoch_ = 0;
		std::uint32_t loggedPlacement_ = 0;
		std::uint32_t loggedOther_ = 0;
		Stats stats_{};
	};
}
