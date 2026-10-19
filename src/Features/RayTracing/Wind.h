#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace RE
{
	class BSGeometry;
	class BSRenderPass;
}

/**
 * @brief (47l) Foliage sway: the wind inputs of the game's tree animation, for the ray tracer's wind BLASes.
 *
 * Per shape: its leaf amplitude / frequency (TreeParams.z / .w of the Lighting vertex shader), read from the vertex shader's
 * per-geometry constants right after BSLightingShader::SetupGeometry wrote them (Light Limit Fix's hook calls OnSetupGeometry),
 * once for every shape the scene asked for (Request) and again now and then (spot checks), never on every draw: a tree-animated
 * draw costs a flag test and one hash-map lookup. Per frame: the wind magnitude and the tree timer (TreeParams.y, WindTimers.x)
 * from BSTreeManager at the frame start, as long as the shaders' own values (compared on every sample) agree with it; otherwise
 * the shaders' last values extrapolated by one frame (WindTimers.x + (x - y)).
 *
 * Diagnostics (log, once per engine mesh, capped): the sampled constants against BSTreeManager's values and the TREE form's leaf
 * amplitude / frequency, and whether a second copy of the mesh (at another distance) has the same parameters.
 *
 * Render / main thread only (SE draws on the main thread); the hook never blocks (try-lock).
 */
namespace RayTracingWind
{
	/// Amplitude / frequency as a wind mesh key: shapes within 1/4096 of each other share a wind BLAS.
	[[nodiscard]] inline std::int32_t Quantize(float a_value)
	{
		return std::isfinite(a_value) ? static_cast<std::int32_t>(std::lround(std::clamp(a_value, -100000.0f, 100000.0f) * 4096.0f)) : 0;
	}

	/// One sample of a tree-animated shape's vertex-shader constants.
	struct Params
	{
		float treeParams[4]{};       // x (the shadow map's timer), y wind magnitude, z amplitude, w leaf frequency
		float windTimers[2]{};       // x this frame's timer, y last frame's
		std::uint32_t frame = 0;     // when it was sampled
		std::uint32_t samples = 0;   // 0 = not sampled yet
	};

	/// This frame's wind for the RT frame input.
	struct FrameWind
	{
		bool valid = false;        // false: no refit this frame (the wind BLASes keep their pose)
		float magnitude = 0.0f;
		float timer = 0.0f;
		bool fromManager = false;  // BSTreeManager (else the shaders' values extrapolated)
	};

	struct Stats
	{
		std::uint32_t requested = 0;        // shapes waiting for / holding samples
		std::uint32_t sampled = 0;          // ... of which sampled at least once
		std::uint64_t samplesTotal = 0;     // constant reads (new shapes, spot checks, the fallback's per-frame sample)
		std::uint64_t readFailures = 0;     // reads refused (no constants, implausible values, fault)
		std::uint64_t managerMatches = 0;   // samples whose magnitude / timer equal BSTreeManager's
		std::uint64_t managerMismatches = 0;
		std::uint64_t spotChanges = 0;      // spot checks that found other amplitude / frequency than the shape's last sample
		std::uint64_t copiesSame = 0;       // second copies of a mesh with the same amplitude / frequency as the first
		std::uint64_t copiesDifferent = 0;
		bool managerReadable = false;       // BSTreeManager found and read
		bool usingManager = false;          // this frame's wind from BSTreeManager
		float magnitude = 0.0f, timer = 0.0f;  // this frame's
	};

	/// On while some wind mesh may be wanted (render thread). Off forgets every request and sample (the hook then does nothing).
	void SetActive(bool a_active);
	[[nodiscard]] bool IsActive();
	/// The shape's constants are sampled on its next draw. a_predicted (optional): leaf amplitude / frequency of its TREE form,
	/// for the diagnostics.
	void Request(const RE::BSGeometry* a_geometry, const float* a_predicted);
	void Forget(const RE::BSGeometry* a_geometry);
	/// The shape's latest sample; false = none yet (or not requested).
	[[nodiscard]] bool Get(const RE::BSGeometry* a_geometry, Params& o_params);
	/// The scene's per-frame pass over every candidate: one lock for all of it (the *Locked calls below need it held).
	[[nodiscard]] std::unique_lock<std::mutex> LockForBatch();
	void RequestLocked(const RE::BSGeometry* a_geometry, const float* a_predicted);
	void ForgetLocked(const RE::BSGeometry* a_geometry);
	[[nodiscard]] bool GetLocked(const RE::BSGeometry* a_geometry, Params& o_params);
	/// Light Limit Fix's BSLightingShader::SetupGeometry hook, right after the engine's function.
	void OnSetupGeometry(RE::BSRenderPass* a_pass);
	/// This frame's wind (computed by the first call of a frame, then the same values).
	FrameWind Compute(std::uint32_t a_frame);
	/// The game animates trees (BSTreeManager::enableTreeAnims; true when it cannot be read). Off: the game draws them at rest,
	/// so the ray tracer keeps their frozen BLASes.
	[[nodiscard]] bool TreeAnimsEnabled();
	[[nodiscard]] Stats GetStats();
}
