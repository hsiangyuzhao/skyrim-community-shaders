#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

/**
 * @brief (batch 37a) Occlusion culling, phase 0: the Hi-Z test runs on every object of the main
 *        view, but its answer is never used. Nothing is skipped; the engine always gets its own
 *        verdict back. Only counts what a real cull WOULD have removed.
 *
 * Ported from doodlum's `codex/hiz-culling-clean` (eeba1ce, MOC.cpp / OcclusionCulling.cpp):
 *  - at Deferred::EndDeferred (the opaque depth is complete) a second HiZPyramid instance
 *    max-reduces the live depth; its 1/16 level is copied into a 3-slot staging ring and read
 *    back a few frames later with DO_NOT_WAIT, together with the camera that rendered it;
 *  - BSCullingProcess::Process1 (74804 + parabolic 101597) and TestBaseVisibility1 (74816 +
 *    101605) are detoured; for the main render camera only, each object's bound is projected
 *    with the snapshot camera and tested against the farthest depth of the cells it covers.
 *
 * Differences from the original:
 *  - the screen-to-grid mapping uses the rendered sub-rectangle (DLSS / DRS), not the full
 *    depth texture: the original put every object into the wrong cells whenever the render
 *    size was smaller than the texture (DLSS Quality: only the top-left 2/3 is rendered);
 *  - two verdicts per object: "raw" (the original's rules) and "guarded" (camera-motion guard,
 *    bound margin, near no-cull distance from the settings below), so the panel shows what a
 *    conservative setup would still save;
 *  - would-be-culled objects under an object already counted this frame are not counted again,
 *    and each counted object's drawable geometry is estimated by walking its subtree.
 *
 * SE 1.5.97 flat only: not installed on AE (IDs unverified) or VR (one eye's depth cannot
 * judge the other's view).
 */
namespace Util::OcclusionDryRun
{
	struct Settings
	{
		bool Enabled = true;                ///< run the dry-run test (only while the overlay is on screen)
		float MotionGuardDistance = 40.0f;  ///< camera moved more than this since the snapshot: guarded verdict keeps everything
		float MotionGuardAngle = 3.0f;      ///< degrees of camera turn that trip the same guard
		float BoundsMargin = 16.0f;         ///< world units added to every bound for the guarded verdict
		float MotionMarginScale = 1.0f;     ///< extra margin per unit of camera travel since the snapshot
		float NearNoCullDistance = 600.0f;  ///< guarded verdict never culls anything closer than this
		float MinTestRadius = 0.0f;         ///< smaller objects are not tested at all
		int SettleFrames = 240;             ///< frames after a loading screen before testing resumes
	};

	/// One frame of counts. "Raw" = the original's rules, "guarded" = with the settings above.
	struct Counts
	{
		uint32_t objectsTested = 0;
		uint32_t containersTested = 0;
		uint32_t occludedRaw = 0;  ///< objects (with something drawable) that would be culled
		uint32_t occludedGuarded = 0;
		uint32_t occludedEmpty = 0;  ///< would be culled but hold nothing drawable here (LOD/grass-only nodes)
		uint32_t containersOccludedRaw = 0;
		uint32_t containersOccludedGuarded = 0;
		uint32_t geomsRaw = 0;  ///< drawable geometry under the would-be-culled objects
		uint32_t geomsGuarded = 0;
		uint32_t visibleGeoms = 0;  ///< geometry the main view accepted (denominator for draws per geometry)
		uint32_t keptActor = 0;     ///< would be culled, kept: actor
		uint32_t keptLod = 0;       ///< would be culled, kept: object/terrain/tree LOD
		uint32_t keptGrass = 0;     ///< would be culled, kept: grass
		uint32_t keptNear = 0;      ///< raw culls the near no-cull distance keeps
		uint32_t keptMotion = 0;    ///< raw culls the motion guard keeps
		double testMs = 0.0;        ///< summed CPU time of the tests over all threads
	};

	struct Report
	{
		bool installed = false;
		std::string status;   ///< one line for the panel
		bool active = false;  ///< testing this frame
		uint64_t frames = 0;  ///< frames with counts so far
		Counts last;          ///< last frame
		// Smoothed (about one second) versions of the main numbers.
		float objectsTested = 0, occludedRaw = 0, occludedGuarded = 0, geomsRaw = 0, geomsGuarded = 0;
		float containersTested = 0, containersOccludedRaw = 0, containersOccludedGuarded = 0;
		float estDrawsRaw = 0, estDrawsGuarded = 0, testMs = 0, motionGuardShare = 0;
		float drawsPerGeom = 0.0f;  ///< main-view draws per accepted geometry (last frame)
		bool drawsPerGeomMeasured = false;
		uint32_t mainViewDraws = 0;  ///< depth prepass + opaque draws, last frame (engine table)
		bool motionGuard = false;    ///< guard tripped last frame
		float cameraMove = 0.0f;     ///< world units the camera moved since the snapshot
		float cameraTurn = 0.0f;     ///< degrees
		int latencyFrames = -1;      ///< age of the depth snapshot the tests used
		uint32_t gridW = 0, gridH = 0, renderW = 0, renderH = 0;
	};

	Settings& GetSettings();
	const Report& GetReport();

	/// @brief Detours the culling functions. SE 1.5.97 flat only; records why otherwise.
	void Install();
	/// @brief Render thread, Deferred::EndDeferred: reduce this frame's depth, read back an old one.
	void OnEndDeferred();
	/// @brief Render thread, right before Present: close this frame's counts.
	void OnFrameEnd(uint32_t a_mainViewDraws, bool a_drawsValid);

	void Load(const nlohmann::json& a_json);
	void Save(nlohmann::json& a_json);
	/// @brief The panel section: numbers, and the settings while the CS menu is open.
	void DrawPanel(bool a_menuOpen);
	/// @brief Everything in the report, for the JSON export.
	nlohmann::json ToJson();
}
