#pragma once

#include <cstdint>

namespace RE
{
	class BSGeometry;
	class BSRenderPass;
	class NiNode;
}

/**
 * @brief (47l, 47n) Foliage sway: the wind inputs of the game's tree animation, for the ray tracer's wind BLASes.
 *
 * (47n) Per plant, exactly as BSLightingShader::SetupGeometry (SE 1.5.97 0x1412F4FD0) computes the Lighting vertex shader's
 * TreeParams / WindTimers for a draw: the node is the shader property's fade node when it is a BSLeafAnimNode / BSTreeNode
 * (BSFadeNode::AsLeafAnimNode), and
 *   TreeParams.x = 0
 *   TreeParams.y = *(float*)(*(void**)ID 513211 + 0x304)                                   (the same for every plant)
 *   TreeParams.z = the node's amplitude (+0x15C) faded by its distance d = sqrt(+0x158) between ID 513234 / 513235:
 *                  f = (1 - (d - start) / (end - start)) x amplitude; z = amplitude >= max(f, 0) ? max(f, 0) : amplitude
 *                  (the engine's one-step reciprocal square root, bit for bit; a negative amplitude is used as it is)
 *   TreeParams.w = the node's leaf frequency (+0x160)
 *   WindTimers.x = 6 x the node's timer (+0x164)
 * and without such a node (0, y, min(1, ...) with distance 0, 1) and timer 0. BSTreeManager::Update advances the node's timer /
 * amplitude / distance once per frame on the main thread before the frame is drawn, so the values read at Early Prep are the ones
 * the frame's draws get (one diagnostic line in the log compares them with the vertex shader's constants).
 *
 * Render / main thread only (SE draws on the main thread); the SetupGeometry hook never blocks (try-lock).
 */
namespace RayTracingWind
{
	/// This frame's values every plant shares (Early Prep).
	struct Globals
	{
		bool valid = false;
		float magnitude = 0.0f;   // TreeParams.y
		float fadeStart = 0.0f;   // ID 513234
		float fadeEnd = 0.0f;     // ID 513235
	};
	/// One plant's pose, as the vertex shader gets it (TreeParams.y / .z / .w, WindTimers.x).
	struct Pose
	{
		float magnitude = 0.0f;
		float amplitude = 0.0f;
		float frequency = 1.0f;
		float timer = 0.0f;
	};

	struct Stats
	{
		bool available = false;      // SE, the engine addresses found
		std::uint64_t nodeReads = 0;     // node reads (per plant per frame)
		std::uint64_t nodeFaults = 0;    // refused (fault / implausible): the plant keeps its last pose
		std::uint32_t compared = 0;      // diagnostic samples against the vertex shader's constants
		std::uint32_t formulaMatches = 0;  // ... equal to the formula evaluated at draw time
		std::uint32_t earlyMatches = 0;    // ... equal to the values read at Early Prep this frame
		float magnitude = 0.0f, fadeStart = 0.0f, fadeEnd = 0.0f;  // this frame's
	};

	/// The engine's addresses (SE only); false = unavailable (logged once).
	[[nodiscard]] bool Available();
	/// This frame's shared values (computed at the first call of a frame, then the same).
	[[nodiscard]] Globals FrameGlobals(std::uint32_t a_frame);
	/// The shape's leaf-animation node as the draw uses it (its shader property's fade node when that is a BSLeafAnimNode /
	/// BSTreeNode), nullptr when none. SEH-guarded.
	[[nodiscard]] RE::NiNode* LeafNodeOf(RE::BSGeometry* a_geometry);
	/// The pose of a plant (a_node may be null: the engine's no-node values). False = unreadable now (keep the last pose).
	[[nodiscard]] bool Evaluate(const RE::NiNode* a_node, const Globals& a_globals, Pose& o_pose);
	/// (Diagnostics) The pose read at Early Prep this frame for a node (the hook compares the draw's constants with it).
	void NoteEarly(const RE::NiNode* a_node, const Pose& a_pose, std::uint32_t a_frame);
	/// On while sway is in use (the hook does nothing otherwise).
	void SetActive(bool a_active);
	[[nodiscard]] bool IsActive();
	/// Light Limit Fix's BSLightingShader::SetupGeometry hook, right after the engine's function: a few samples of the vertex
	/// shader's constants for the one diagnostic log line.
	void OnSetupGeometry(RE::BSRenderPass* a_pass);
	/// The game animates trees (BSTreeManager::enableTreeAnims; true when it cannot be read). Off: the game draws them at rest,
	/// so the ray tracer keeps their frozen BLASes.
	[[nodiscard]] bool TreeAnimsEnabled();
	[[nodiscard]] Stats GetStats();

	/// (47q) The node's leaf timer (+0x164): read / written (SEH-guarded; false = refused). "Keep Leaves Moving on Long Frames": the
	/// game does not advance its leaf timers on a frame of 35 ms or more (trace-20261020-164255: every frame with eng_dt < 35 ms
	/// refit, none at 35.2 ms and up, the same frame's dt); the scene advances the frozen ones of its swaying plants itself.
	bool ReadTimer(const RE::NiNode* a_node, float& o_timer);
	bool WriteTimer(RE::NiNode* a_node, float a_timer);
	/// (47r) The leaf node's frequency (BSLeafAnimNode +0x160; SEH: false when it cannot be read or is not finite).
	bool ReadFrequency(const RE::NiNode* a_node, float& o_frequency);
}
