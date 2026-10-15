#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
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
	/// (46d) A source that moved this far (world units) since last frame drops all of its denoiser history.
	inline constexpr double kMotionFullUnits = 24.0;

	// LightLimitFix::LightData::rtShadow (RayTracing/PointLightShadows.hlsli).
	inline constexpr std::uint32_t kTagSourceMask = 0x7Fu;  // source slot + 1 (0 = none)
	inline constexpr std::uint32_t kTagNative = 1u << 8;    // the engine's shadow-map light: RT replaces its shadow map
	inline constexpr std::uint32_t kTagKeepSSPLS = 1u << 9; // keep Screen Space Point Light Shadows as contact shadows (min with RT)
	inline constexpr std::uint32_t kTagSplit = 1u << 10;    // debug A/B: RT on the left half of the screen only

	// GpuSource::info[0] bits (TracePointLightsCS).
	inline constexpr std::uint32_t kSourceISL = 1u;     // inverse-square attenuation (InverseSquareLighting)
	inline constexpr std::uint32_t kSourceNative = 4u;  // holds an engine shadow-map light
	inline constexpr std::uint32_t kSourceSpot = 8u;    // (46c) spot: spot[] = direction, tan of the larger half width; info[2] = falloff
	inline constexpr std::uint32_t kSourceBeam = 16u;   // (46c) window beam: no attenuation in the ranking, ray ends at the interior's bounds
	inline constexpr std::uint32_t kSourceNoPlayer = 32u;  // (46d) the player's own light: the player's body is not tested (character mask 0x04 only)

	/// One shadow source as the trace reads it (StructuredBuffer, 64 bytes).
	struct GpuSource
	{
		float posRadius[4];  // camera-relative position, radius (attenuation range)
		float atten[4];      // inverse radius, ISL fade zone, ISL size bias, intensity (brightest channel x fade)
		float spot[4];       // (46c) spot: direction (light to surface), tan of the larger half width; unused for points
		std::uint32_t info[4];  // flags, ray end clearance (float bits), (46c) spot falloff exponent (float bits),
		                        // (46e) owner key: the character whose TLAS instances its rays ignore (0 = none)
	};
	static_assert(sizeof(GpuSource) == 64);

	/// (46c) An engine shadow-map light with a source this frame (interiors): Ray Tracing writes its shadow mask
	/// channel (RayTracing/PointMaskCS.hlsl) -- the traced visibility times the spot's cone where the pixel traced
	/// the source; else the engine's value (or, when its shadow map was not drawn, the cone / 1).
	struct MaskEntry
	{
		const RE::BSLight* light = nullptr;
		std::uint32_t channel = 0;    // shadow mask channel (BSShadowLight::shadowLightIndex)
		std::uint32_t source = 0;     // source slot + 1
		bool spot = false;
		float pos[3]{};               // camera-relative position of the light
		double world[3]{};            // (46e) world position (the next frame's fallback cone of a skipped shadow map)
		float dir[3]{}, right[3]{}, up[3]{};  // spot axes (world)
		float semiWidth = 1.0f, semiHeight = 1.0f, falloff = 1.0f;  // spot: tan of the half widths, mask falloff exponent
	};

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
		// (46c) Interiors: the engine's spot shadow lights take part (with their cone), and Window Shadows Ultimate's
		// window beams when beams is on (no radius / distance / area limits: their light stands far outside).
		bool interior = false;
		bool beams = true;
		// (46c) Engine shadow lights whose shadow map was not drawn this frame: always a source (sorted by pointer).
		const std::vector<const RE::BSLight*>* forced = nullptr;
		// (46d) The player's own lights (census "player") and ownerless spell / effect lights (census "effect").
		bool player = true;
		bool effects = true;
		float ownClearance = 8.0f;  // their ray end clearance: no fixture of theirs is in the scene (the torch is the player's, excluded)
		// (46e) The characters in this frame's TLAS (owner reference -> key, sorted by pointer, Characters::Owners): a light
		// carried by one of them (a torch in an NPC's hand) does not test that character's own body and gear.
		const std::vector<std::pair<const void*, std::uint32_t>>* owners = nullptr;
		// (46q) Carrier Casts Shadows from Its Own Light, third person: the player's own lights take the player's key as their carrier
		// (the body blocks except near the light, TracePointLightsCS) instead of skipping the player (kSourceNoPlayer).
		bool playerCarrier = false;
		// (46g) Stable selection: per light a smoothed intensity (EMA 0.1 per frame) and radius (rises at once, shrinks 2 % per
		// frame) drive the rules, the merge and the GPU source; a light tagged last frame stays while it is above 80 % of the cuts.
		bool stable = true;
		// (46g) Window beams found implausible (almost every traced pixel of their cone blocked): no source, the engine's shadow
		// map stays for them (sorted by pointer; null = none).
		const std::vector<const RE::BSLight*>* excludedBeams = nullptr;
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
		std::uint32_t beams = 0;         // (46c) window beams among the sources
		std::uint32_t spots = 0;         // (46c) spot shadow lights among the sources (beams included)
		std::uint32_t spotsInvalid = 0;  // (46c) spot shadow lights left to the engine: their cone could not be read
		std::uint32_t forced = 0;        // (46c) lights whose shadow map was not drawn (always sources)
		std::uint32_t forcedMissing = 0; // (46c) ... of which got no source (their mask channel gets the fallback)
		std::uint32_t player = 0;        // (46d) the player's own lights among the sources
		std::uint32_t effects = 0;       // (46d) spell / effect lights among the sources
		std::uint32_t carried = 0;       // (46e) sources carried by a traced character (its own body is not tested)
		std::uint32_t kept = 0;          // (46g) lights kept by the hysteresis (below the cut, tagged last frame)
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
		/// (46c) This frame's engine shadow lights with a source (interiors only; empty outdoors).
		[[nodiscard]] const std::vector<MaskEntry>& MaskEntries() const { return maskEntries_; }
		/// (46d) Per slot, how much of the denoiser's history to drop this frame (0 = keep it, 255 = none: a new source
		/// in the slot, or a source that moved a long way since last frame -- a carried torch, a flying spell).
		[[nodiscard]] const std::array<std::uint8_t, kMaxSources>& SlotMotion() const { return motion_; }
		/// (46g) Per slot: the window beam light it holds this frame (null = none / not a beam).
		[[nodiscard]] const std::array<const RE::BSLight*, kMaxSources>& SlotBeams() const { return slotBeams_; }

	private:
		std::vector<MaskEntry> maskEntries_;
		struct Slot
		{
			bool live = false;
			double pos[3]{};
			std::uint32_t freedFrame = 0;
			std::uint8_t kind = 0;  // (46d) 0 world light, 1 the player's, 2 spell / effect: a slot keeps its kind
		};
		std::array<Slot, kMaxSources> slots_{};
		std::array<std::uint8_t, kMaxSources> motion_{};
		std::array<const RE::BSLight*, kMaxSources> slotBeams_{};  // (46g)
		std::vector<std::pair<const RE::BSLight*, std::uint32_t>> tags_;  // sorted by pointer
		// (46g) Per light: smoothed intensity / radius, the last frame it was seen and whether it was tagged then.
		struct History
		{
			float intensity = 0.0f;
			float radius = 0.0f;
			std::uint32_t frame = 0;
			bool tagged = false;
		};
		std::unordered_map<const RE::BSLight*, History> history_;
		bool published_ = false;
		Stats stats_{};
	};
}
