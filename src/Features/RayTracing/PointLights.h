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
	// (46y) Screen-Space Shadows for Non-RT Casters: Screen Space Point Light Shadows run beside RT for this light, but an occluder
	// sample that is a ray-traced caster (TracePointLightsCS marks those pixels) shadows only within the contact length; bits 16-23
	// = that length in eighths of the ray tracer's start offset (RT Caster Contact Length x 8).
	inline constexpr std::uint32_t kTagSSPLSNonRT = 1u << 11;
	inline constexpr std::uint32_t kTagContactShift = 16u;
	// (47h) "Point Light Shadows on Water": the water above a shallow bed takes the bed's traced visibility (PointLightShadows.hlsli GetWater).
	inline constexpr std::uint32_t kTagWaterBed = 1u << 12;

	// GpuSource::info[0] bits (TracePointLightsCS).
	inline constexpr std::uint32_t kSourceISL = 1u;     // inverse-square attenuation (InverseSquareLighting)
	inline constexpr std::uint32_t kSourceNative = 4u;  // holds an engine shadow-map light
	inline constexpr std::uint32_t kSourceSpot = 8u;    // (46c) spot: spot[] = direction, tan of the larger half width; info[2] = falloff
	inline constexpr std::uint32_t kSourceBeam = 16u;   // (46c) window beam: no attenuation in the ranking, ray ends at the interior's bounds
	inline constexpr std::uint32_t kSourceNoPlayer = 32u;  // (46d) the player's own light: the player's body is not tested (character mask 0x04 only)
	// (46x) A direct fill light (the floor patch of a light shaft) paired with the window spot shadow light above it: spot[0..2] = the
	// ray target (the spot's camera-relative position); its rays go there like a window beam's (no clearance, they end where they
	// leave the interior, the beam culling, the beam softness). Ranked by its own attenuation (it lights the floor patch).
	inline constexpr std::uint32_t kSourceAim = 64u;
	// (46x) spot[3] = this source's emitter radius (a direct fill without a window spot: a larger, softer light).
	inline constexpr std::uint32_t kSourceEmitter = 128u;
	// (46x) Portal-strict light with rooms: spot[0..2] (uint bits) = its rooms' box, camera-relative, as half floats (lo.x | lo.y << 16,
	// lo.z | hi.x << 16, hi.y | hi.z << 16): pixels outside it do not rank it (Light Limit Fix does not light them with it).
	inline constexpr std::uint32_t kSourceRoomBox = 256u;

	/// (46x) What the selection decided for a light of the 46x classes this frame (Light Census "rtClass" / "rtWhy").
	enum class Decision : std::uint8_t
	{
		None = 0,
		Negative,         // why: 0 LIGH flag, 1 negative colour, 2 negative fade
		FillAmbient,      // why: 0 not direct light by name / place, 1 too dim (< 1.0 before the fade), 2 tinted, 3 Trace Direct Fill Lights off
		FillDirectAimed,  // why: 0 named like direct light, 1 on the axis of a window spot shadow light, 2 both
		FillDirectPoint,  // why: 0 named like direct light, no window spot above it
		FillFixture,      // why: 0 named like a fire / candle / torch ..., 1 within 128 units of a fixture light
		WiderBeam,        // why: 0 window spot with radius 4096 - 10000 (or a Window Shadows Ultimate spot by its plugin)
		PortalBoxed,      // why: 0 its rooms hold the camera, 1 behind a portal (its rooms do not hold the camera)
		PortalDropped,    // why: 0 behind a portal, its rooms' box off screen, 1 behind a portal (per-pixel room box off)
		Count
	};
	const char* DecisionName(Decision a_decision);
	const char* DecisionWhy(Decision a_decision, std::uint8_t a_why);

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
		bool ssplsNonRT = false;    // (46y) tag: SSPLS beside RT for occluders the ray tracer does not see (wins over keepSSPLS)
		bool waterBed = false;      // (47h) tag: Point Light Shadows on Water
		float ssplsContact = 8.0f;  // (46y) their contact length for traced occluders, x the ray start offset (0 - 31.875)
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
		// (46s) Steady Flickering Lights: static world lights (fixtures, standalone, mesh-attached; not carried, not the player's, not
		// spells / effects, not spots) take a steady position instead of the engine's flicker-jittered one: their parent node's when
		// the light's local offset is within 2 x its LIGH's movement amplitude + 1, else a per-light average (EMA 0.05 per frame,
		// snaps on a jump over 128 units). Used for the rules, the merge, the slots (and so the denoiser's motion), the GPU source.
		bool steady = true;
		// (46s) Merge hysteresis: a light merged into a group last frame stays in the group of the same seed light up to 1.5 x the
		// merge distance.
		bool mergeHysteresis = true;
		// (46s) Meshless lights near fires (placed LIGH without a mesh, classified a fixture by its name only, not shadow-casting):
		// MeshlessOwn = a source of its own (46r), MeshlessSnap = its tag goes to the nearest visible fixture's source within
		// snapDistance (its intensity added, no weight in the centre; none in range: own), MeshlessSkip = no RT source.
		std::uint32_t meshless = 1;
		float snapDistance = 320.0f;
		// (46s) Diagnostics: the sources whose range covers this point (world; the player's chest). Not used when null.
		const double* reachPoint = nullptr;
		// (46x) Fill lights (placed, no mesh, named / from a plugin like fill lights; 46w: never traced). Trace Direct Fill Lights: a fill
		// named like direct light (sun / shaft / beam / ray) or lying on the axis of a window spot shadow light (within 128 units
		// horizontally, the spot above it), at least 1.0 before the fade and near-neutral (max / min channel <= 1.3), is traced: aimed
		// at that spot (kSourceAim), or without one as a soft point light of emitter radius directEmitter. Plugin-only fills (an
		// "ambient" plugin made them fills) named like a fire / candle / torch, or within 128 units of a fixture light, are fixtures
		// (the meshless rules: Snap / Own / Skip).
		bool directFills = true;
		float directEmitter = 24.0f;
		bool pluginFillFixtures = true;
		// (46x) The window-beam radius in effect (LightCensus::WindowBeamRadius(): 4096 with Wider Window Beam Detection, else 10000).
		float beamRadius = 10000.0f;
		// (46x) Skip Lights Behind Portals (interiors with room bounds): a portal-strict light none of whose rooms (or portals) holds the
		// camera takes no source when roomBoxes is off, or when its rooms' box is off screen; with roomBoxes every portal-strict light with
		// readable rooms is ranked only at pixels inside its rooms' box (kSourceRoomBox). The camera in no known room: nothing changes.
		bool portals = true;
		bool roomBoxes = true;
	};
	inline constexpr std::uint32_t kMeshlessOwn = 0, kMeshlessSnap = 1, kMeshlessSkip = 2;

	/// (46s) A source whose range covers the player this frame (diagnostics).
	struct Reach
	{
		std::uint32_t slot = 0;
		std::uint8_t kind = 0;      // 0 world light, 1 the player's, 2 spell / effect
		std::uint8_t category = 0;  // Util::LightCensus::Category of its strongest light
		bool native = false;
		bool spot = false;
		bool steady = false;        // its strongest light has a steady position
		std::uint16_t members = 0;  // lights sharing it (merged + snapped)
		std::uint16_t snapped = 0;  // of which meshless lights snapped to it
		float distance = 0.0f, radius = 0.0f, intensity = 0.0f;
		char label[40]{};           // strongest light's LIGH / base editor ID
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
		std::uint32_t steadyParent = 0;  // (46s) candidates at their parent node's position
		std::uint32_t steadyAverage = 0; // (46s) candidates at their averaged position
		std::uint32_t mergeKept = 0;     // (46s) lights kept in last frame's group by the merge hysteresis
		std::uint32_t snapped = 0;       // (46s) meshless lights tagged to a visible fixture's source
		std::uint32_t meshlessOwn = 0;   // (46s) meshless lights with a source of their own (Own, or Snap with no fixture in range)
		std::uint32_t meshlessSkipped = 0;  // (46s) meshless lights left without a source (Skip)
		// (46x) Fill lights and negative lights (at the category rule: past the radius / intensity / distance / area rules).
		std::uint32_t negative = 0;         // negative lights (never traced)
		std::uint32_t fillAmbient = 0;      // fill lights left untraced
		std::uint32_t fillDirectAimed = 0;  // direct fills traced toward their window spot
		std::uint32_t fillDirectPoint = 0;  // direct fills traced as a soft point light
		std::uint32_t fillFixture = 0;      // plugin-only fills taken as fixtures
		std::uint32_t beamsWider = 0;       // window beams by the wider rule only (radius 4096 - 10000)
		// (46x) Portals (indoors): candidates that are portal-strict with readable rooms; of which none of whose rooms holds the camera;
		// dropped; ranked only inside their rooms' box; rooms unreadable / not around the light (left as 46w). cameraInRoom: false = the
		// camera is in none of the rooms seen (portal rules not applied this frame).
		std::uint32_t portalStrict = 0;
		std::uint32_t portalAway = 0;
		std::uint32_t portalDropped = 0;
		std::uint32_t portalBoxed = 0;
		std::uint32_t portalBadRooms = 0;
		bool cameraInRoom = false;
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
		/// (46s) This frame's sources whose range covers Config::reachPoint (nearest first, at most 8).
		[[nodiscard]] const std::vector<Reach>& Reaching() const { return reach_; }
		/// (46s) A transition (loading screen): the per-light history belongs to the place left behind.
		void ForgetHistory()
		{
			history_.clear();
			reach_.clear();
			decisions_.clear();
		}
		/// (46x) This frame's decision for a light of the 46x classes (Decision | why << 8; 0 = none). Render thread.
		[[nodiscard]] std::uint16_t DecisionFor(const RE::BSLight* a_light) const;
		/// (46x) Slots of this frame's sources with their own emitter radius (kSourceEmitter), for the denoiser.
		[[nodiscard]] std::uint64_t EmitterSlots() const { return emitterSlots_; }

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
			const void* niLight = nullptr;  // (46s) the same BSLight address with another NiLight is another light
			std::uint32_t seen = 0;         // (46s) last frame it was looked at (the clean-up)
			float intensity = 0.0f;
			float radius = 0.0f;
			std::uint32_t frame = 0;        // (46g) last frame of the smoothed intensity / radius (0 = none)
			bool tagged = false;
			// (46s) Steady position.
			bool steadyValid = false;
			bool steadyParent = false;
			std::uint32_t steadyFrame = 0;
			double steady[3]{};
			// (46s) Merge hysteresis: the seed light of the group it joined (null = it seeded one / none).
			const RE::BSLight* seed = nullptr;
			std::uint32_t seedFrame = 0;
			// (46s) Snap: the visible fixture its tag went to last time (kept while in range).
			const RE::BSLight* anchor = nullptr;
			std::uint32_t anchorFrame = 0;
			// (46x) A direct fill's window spot (world position; kept 120 frames while the spot is not in this frame's lights).
			double aim[3]{};
			std::uint32_t aimFrame = 0;
		};
		std::unordered_map<const RE::BSLight*, History> history_;
		std::vector<Reach> reach_;  // (46s)
		std::vector<std::pair<const RE::BSLight*, std::uint16_t>> decisions_;  // (46x) sorted by pointer
		std::uint64_t emitterSlots_ = 0;                                        // (46x)
		bool published_ = false;
		Stats stats_{};
	};
}
