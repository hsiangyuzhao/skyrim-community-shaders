#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace RE
{
	class BSLight;
}

/**
 * @brief (46b) Ray-traced point-light shadows: the per-frame CPU selection of shadow sources.
 *
 * Every frame (Ray Tracing's Prepass, before Light Limit Fix gathers its lights) the lights the
 * renderer uses this frame (ShadowSceneNode activeLights / activeShadowLights, read the way Light
 * Limit Fix reads them) are filtered (LLF would upload it, lodDimmer, radius, intensity, distance,
 * inside the traced area, the census category), lights within the merge distance become one shadow
 * source (intensity-weighted centre, largest radius), and the sources are ranked by their influence
 * on the screen and capped. Each source keeps its slot (0..63) from frame to frame while it exists,
 * so the denoiser's history can follow it.
 *
 * The trace (TracePointLightsCS) picks, per pixel, the K sources that light it most and traces one
 * ray toward each; Light Limit Fix writes each light's tag (TagFor) into its buffer entry, and the
 * Lighting / Water shaders use the traced visibility for a light whose source is in the pixel's slots.
 *
 * Designed for 46c (interiors, Window Shadows Ultimate's window spots): a source has a kind (point /
 * spot with a cone) and a ray end of its own; the interior bounds and portal-strict lights only need
 * new rules in Select().
 *
 * Render thread only. Engine reads are SEH-protected; the work per frame is bounded.
 */
namespace RayTracingPointLights
{
	inline constexpr std::uint32_t kMaxSources = 64;     // slots (TracePointLightsCS: one bit each in a uint2 mask)
	inline constexpr std::uint32_t kMaxSlotsPerPixel = 4;

	// LightLimitFix::LightData::rtShadow (RayTracing/PointLightShadows.hlsli).
	inline constexpr std::uint32_t kTagSourceMask = 0x7Fu;  // source slot + 1 (0 = none)
	inline constexpr std::uint32_t kTagNative = 1u << 8;    // the engine's shadow-map light: RT replaces its shadow map
	inline constexpr std::uint32_t kTagKeepSSPLS = 1u << 9; // keep Screen Space Point Light Shadows as contact shadows (min with RT)
	inline constexpr std::uint32_t kTagSplit = 1u << 10;    // debug A/B: RT on the left half of the screen only

	// GpuSource::info[0] bits (TracePointLightsCS).
	inline constexpr std::uint32_t kSourceISL = 1u;     // inverse-square attenuation (InverseSquareLighting)
	inline constexpr std::uint32_t kSourceNative = 4u;  // holds an engine shadow-map light
	inline constexpr std::uint32_t kSourceSpot = 8u;    // (46c) spot: spot[] = direction, cos(outer half angle)

	/// One shadow source as the trace reads it (StructuredBuffer, 64 bytes).
	struct GpuSource
	{
		float posRadius[4];  // camera-relative position, radius (attenuation range)
		float atten[4];      // inverse radius, ISL fade zone, ISL size bias, intensity (brightest channel x fade)
		float spot[4];       // (46c) direction, cos(outer half angle); unused for points
		std::uint32_t info[4];  // flags, ray end clearance (float bits), (46c) reserved x2
	};
	static_assert(sizeof(GpuSource) == 64);

	struct Config
	{
		bool native = true;         // include the engine's shadow-map lights (RT replaces their shadow map in Lighting)
		bool keepSSPLS = false;     // tag: keep SSPLS as contact shadows for RT lights
		bool split = false;         // tag: debug split screen
		std::uint32_t maxSources = 64;
		float maxDistance = 4000.0f;  // light centre to camera
		float mergeDistance = 64.0f;
		float minRadius = 128.0f, maxRadius = 4096.0f;
		float minIntensity = 0.5f;    // brightest channel x fade, before lodDimmer
		float minLodDimmer = 0.3f;
		float clearance = 32.0f;      // ray end short of the light (its own fixture must not shadow it)
		std::uint32_t classifyBudget = 6;   // new lights read by the classifier per frame (about 20-50 us each)
	};

	struct Stats
	{
		std::uint32_t lights = 0;        // in the engine's lists this frame
		std::uint32_t used = 0;          // of which LLF uploads (valid, not faded out, shadow slot)
		std::uint32_t candidates = 0;    // passed every rule
		std::uint32_t sources = 0;       // after merging and the cap
		std::uint32_t merged = 0;        // lights folded into another source
		std::uint32_t capped = 0;        // sources dropped by the cap
		std::uint32_t native = 0;        // engine shadow-map lights among the candidates
		std::uint32_t pending = 0;       // not classified yet (budget): skipped this frame
		std::uint32_t readErrors = 0;
		std::uint32_t slotChanges = 0;   // sources that took a new slot this frame
		std::uint32_t byReason[8]{};     // rejected: lodDimmer, radius, intensity, distance, outside, category, frustum, other
		float cpuMs = 0.0f;
	};
	inline constexpr const char* kRejectNames[8] = { "faded (lodDimmer)", "radius", "intensity", "distance", "outside traced area", "category",
		"off screen", "unused by LLF" };

	class Selector
	{
	public:
		/// Builds this frame's sources. a_camera: world camera position; a_viewProj: FrameBuffer::CameraViewProj
		/// (camera-relative, row-major as HLSL row_major); a_rect: world xy min / max of the traced area.
		/// o_sources: kMaxSources entries (unused slots zero). Returns the number of slots to scan (highest
		/// live slot + 1, 0 = none). The tags are not published until Publish(true).
		std::uint32_t Select(const Config& a_config, const double a_camera[3], const float a_viewProj[16], const float a_rect[4], std::uint32_t a_frame,
			GpuSource* o_sources);
		/// This frame's trace was submitted (true) or not (false: every tag reads 0).
		void Publish(bool a_valid) { published_ = a_valid; }
		[[nodiscard]] bool Published() const { return published_; }
		/// LightLimitFix: the tag of a light it uploads (0 = no source this frame).
		[[nodiscard]] std::uint32_t TagFor(const RE::BSLight* a_light) const;
		/// Forget every slot (feature off, loading screen, error).
		void Clear();
		[[nodiscard]] const Stats& GetStats() const { return stats_; }

	private:
		struct Slot
		{
			bool live = false;
			double pos[3]{};
			std::uint32_t freedFrame = 0;
		};
		std::array<Slot, kMaxSources> slots_{};
		std::vector<std::pair<const RE::BSLight*, std::uint32_t>> tags_;  // sorted by pointer
		bool published_ = false;
		Stats stats_{};
	};
}
