#pragma once

#include "Buffer.h"
#include "Feature.h"

#include "Features/NRDReblurIntegration.h"
#include "Features/RayTracing/Backend.h"
#include "Features/RayTracing/Characters.h"
#include "Features/RayTracing/PointLights.h"
#include "Features/RayTracing/Scene.h"

/**
 * @brief (45) Ray Tracing (prototype): ray-traced sun shadows inside the loaded exterior cells.
 *
 * One switch. Off (default) = nothing of this feature runs: no D3D12 device, no hook does
 * anything, the shadow mask, Screen Space Shadows and the cascades are exactly the game's.
 * On = the static objects and landscape of the loaded cells are captured into BLASes on a
 * D3D12 device of our own (Backend.h); each frame, after the engine computed its sun shadow
 * mask and before the opaque pass, a ray query pass traces one or two rays per pixel toward
 * the sun and the result is merged into the shadow mask the Lighting shader reads (t14):
 *     mask.r = lerp(mask.r, min(mask.r, rt), w)
 * where w fades from 1 inside the loaded area to 0 over the last part of it (back to the
 * cascades / terrain shadows). Geometry the ray tracer covers is left out of the sun cascades
 * (CasterFilter.h), so the shadow map then only holds the casters the ray tracer does not see
 * (characters, trees, grass, alpha-tested and moving objects), and min() keeps their shadows.
 * Screen Space Shadows are switched off inside the traced area (its texture is set to 1 there).
 *
 * Not touched: baked AO, Skylighting, point-light shadows, LOD shadows, terrain shadows.
 */
struct RayTracing : Feature
{
public:
	virtual inline std::string GetName() override { return "Ray Tracing"; }
	virtual inline std::string GetShortName() override { return "RayTracing"; }
	virtual inline std::string_view GetCategory() const override { return "Ray Tracing"; }  // (46g) its own category
	virtual inline bool SupportsVR() override { return false; }
	/// (46b) Lighting / Water read the ray-traced point-light shadows (RayTracing/PointLightShadows.hlsli); inert per light
	/// unless Light Limit Fix tags it (Light::rtShadow), so the old path is taken whenever nothing is traced.
	virtual inline std::string_view GetShaderDefineName() override { return "RT_POINT_SHADOWS"; }
	virtual bool HasShaderDefine(RE::BSShader::Type a_type) override { return a_type == RE::BSShader::Type::Lighting || a_type == RE::BSShader::Type::Water; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Ray-traced sun shadows for the loaded area (prototype).",
			{ "One switch: on = ray-traced sun shadows, off = the game's shadows exactly",
				"Static objects and landscape of the loaded cells",
				"Characters, trees and grass keep their shadow-map shadows",
				"Ray-traced point-light shadows outdoors and indoors (incl. window light beams)",
				"Debug views: shadow source, RT vs shadow map, coverage" }
		};
	}

	/// Five tiers + Custom.
	enum class Quality : int
	{
		VeryLow = 0,
		Low = 1,
		Medium = 2,
		High = 3,
		Ultra = 4,
		Custom = 5,
		Count
	};

	enum class DebugView : int
	{
		Off = 0,
		ShadowSource,
		Disagreement,
		Coverage,
		RawVisibility,
		HitDepth,  // (45e) ray-traced first hit vs the game's depth
		PenumbraRadius,  // (45k) Denoiser Penumbra: filter radius
		HistoryLength,   // (45k) Denoiser Penumbra: history length
		AdaptiveRays,    // (45k) pixels with adaptive extra rays
		RtCasterMask,    // (45k) Screen Space Shadows "non-RT casters only": traced casters
		DistanceQuality, // (45l) fall-off and pattern-filled pixels
		CharacterRays,   // (45l) where the character ray ran
		// (46b) Point-light shadows (PointDenoiseCS writes them; the sun views above stay as they are).
		PLCandidates,    // shadow-casting lights that reach the pixel
		PLSources,       // which source each slot traces (colour per source)
		PLRaw,           // this frame's rays (all traced lights multiplied)
		PLDenoised,      // after the denoiser
		PLSsplsCoverage, // (46y) drawn by Lighting.hlsl: per pixel, which shadows its point lights get (RT, SSPLS, both)
		Count
	};

	/// (45k) Shadow denoiser. Stored as an int; the values are kept (1 was Temporal, removed in 45o:
	/// a saved 1 loads as Penumbra).
	enum class DenoiserMode : int
	{
		Off = 0,       // the small Combine blur only
		Penumbra = 2,  // (default) penumbra-aware spatial filter + clamped temporal + adaptive rays
		DLSSRR = 3,    // nothing of ours: raw animated noise for DLSS Ray Reconstruction
		Sigma = 4,     // NVIDIA NRD SIGMA shadow denoiser
		Count
	};

	struct Settings
	{
		bool Enabled = false;
		int QualityLevel = static_cast<int>(Quality::Medium);
		// Per kind of object: ray traced (true) or the shadow map + screen-space shadows.
		bool Characters = true;
		bool PhysicsParts = true;  // HDT-SMP / HDT-PE parts other than hair (capes, skirts)
		// 5 = 45o (debug switches hard-wired; no migrations before it); 6 = 46o (Window Beam Guard off once; the guard is gone since
		// 47m); 7 = 47m (Skip the Game's Shadow Maps for RT-Shadowed Lights on once)
		int SettingsVersion = 7;
		bool Buildings = true;
		bool Objects = true;
		bool Trees = false;
		int Foliage = 0;           // RayTracingScene::FoliageMode
		bool Cutouts = false;      // fences, banners, grilles: traced with their cut-out
		bool Hair = false;
		bool FirstPersonPlayer = false;  // the player stays traced in first person
		int Denoiser = static_cast<int>(DenoiserMode::Penumbra);
		float RtContactScale = 8.0f;  // Screen Space Shadows: contact range of traced casters, x the ray start offset
		// Quality "Custom": what the tiers set.
		int CustomBaseRays = 1;
		int CustomExtraRays = 1;
		// (47q) The sun's cone left the tiers / Custom: SunSize below (the CustomSunCone key is read once, for the migration).
		float CustomFalloffStart = 4096.0f; // units
		float CustomFalloffEnd = 12288.0f;
		int CustomFarQuality = 0;           // beyond the fall-off end: 0 full, 1 checkerboard, 2 half
		int CustomTraceResolution = 0;      // everywhere: 0 full, 1 checkerboard, 2 half
		int CustomNpcQuality = 1;           // 0 full, 1 reduced
		float CustomDenoiseRadius = 16.0f;  // px
		int CustomDenoiseTaps = 4;          // per side
		float CustomDenoiseClamp = 1.5f;    // history clamp, sigmas
		int CustomHistory = 12;             // frames
		bool CustomDenoiseHalf = true;
		float CustomFarDenoiseScale = 0.75f;
		// NRD SIGMA: frames of its temporal stabilisation (0 = none: least lag on moving shadows).
		int SigmaStabilizedFrames = 4;
		// (46b) Point-light shadows (Ray Tracing page, Point Lights).
		bool PointLights = true;
		int PointLightsPerPixel = 2;         // K: 1, 2 or 4 lights traced per pixel
		int PointLightsMaxSources = 64;
		float PointLightsDistance = 4000.0f;  // light centre to camera (units)
		float PointLightsSize = 6.0f;        // emitter radius (soft edges)
		float PointLightsClearance = 32.0f;  // the ray stops this far before the light (its own fixture casts nothing)
		// (46c) Interiors: the interior cell is captured and its point lights (and window beams) traced; no sun trace.
		bool PointLightsInteriors = true;
		int PointLightsPerPixelInterior = 4;    // K indoors: 1, 2 or 4
		bool PointLightsWindowBeams = true;     // Window Shadows Ultimate's window beams traced (their shadow mask channel written)
		bool PointLightsSkipShadowMaps = true;  // the engine does not draw shadow maps for lights fully handled by RT (interiors); (47m) default on
		// (46d)
		bool PointLightsPlayer = true;          // the player's own lights (torch in hand, Quick Light, light spells) cast RT shadows (not of the player)
		bool PointLightsEffects = true;         // spell / effect lights without an owner (Magelight, projectiles) cast RT shadows
		bool PointLightsAlphaOutdoors = true;   // outdoors too, leaves / bushes / cut-outs (as the per-kind menu traces them) block point lights
		// (46o) Window beams' angular size (degrees, half angle, like the sun's cone): their rays are jittered inside it and the point-light
		// denoiser filters their shadow edges by blocker distance x tan(angle). 0 = 46n (the emitter sphere, nearly hard and unfiltered).
		// Outside the settings macro (it takes at most 64 members): read / written in Load / SaveSettings.
		float PointLightsBeamSoftness = 0.5f;
		// (46f) Ray-traced ambient occlusion (Ray Tracing page, Ambient Occlusion). (46h) Indoors too: AOInteriors.
		bool AO = false;
		int AOMode = 0;                // AOMode: 0 RTAO (keeps SSGI contact AO, darker of both), 1 darker of RTAO and SSGI AO
		float AORayLength = 100.0f;    // units (70 = 1 m): 35 - 210
		float AOStrength = 1.0f;
		bool AOTemporal = true;        // clamped history after the spatial filter
		float AODistance = 4000.0f;    // from the camera: RTAO fades back to SSGI AO over the last quarter of it (outdoors)
		// (46h) RTAO indoors (the interior frame; the whole interior is in the TLAS). Only with RTAO on (itself default off), so
		// nothing changes until the user ticks RT Ambient Occlusion; then indoors follows this box (on). No distance fade indoors:
		// the RT weight is 1 everywhere inside the interior's box.
		bool AOInteriors = true;
		int AOModeInterior = 0;        // AOModeKind (indoors)
		// (46k) Transitions (Ray Tracing > Advanced): the loading pump (always on since 47m) prepares the destination behind the
		// loading screen; its CPU per loading-screen frame; the frames after the loading screen still counted as black (the Fader
		// Menu cannot tell when its fade-in starts).
		float LoadingPumpMs = 8.0f;
		float BlackWindowMs = 200.0f;
		// (46n) The destination prepared on the main thread as each cell finishes loading (TESCellFullyLoadedEvent), CellEventMs
		// per event (always on since 47m): the loading screen is presented from another thread, so the per-frame pump never runs
		// behind it. The fade-in after the loading screen (Fader Menu open, the picture still mostly dark) gets budgets that start
		// large and shrink. Outside the settings macro (it takes at most 64 members): read / written in Load / SaveSettings.
		float CellEventMs = 12.0f;
		// (46q) Outside the settings macro too. Carrier Casts Shadows (always on since 47m): a light carried by a traced character is
		// blocked by the carrier's own body except within CarrierClearance units of the light.
		float CarrierClearance = 28.0f;
		// (46s) Outside the settings macro too. Meshless Lights Near Fires: placed lights without a mesh named like a fixture
		// (vanilla "torch" lights at no visible fire) -- 0 Own (46r), 1 Snap to the nearest visible fixture's source, 2 Skip.
		int PointLightsMeshless = 1;
		// (46t) RTAO, outside the settings macro too. Two scales (AODetail): the long ray split at AODetailLength (its far part
		// counted only where the near part is open) plus near-only rays (AODetailRays near rays per pixel in all, the long ray's near part included), the near visibility filtered at full
		// resolution and multiplied in with AODetailStrength (AOStrength is then the far / large-scale term's). AONormalGuided: the
		// filters weight taps by the G-buffer normal (not only the 45 degree gate). AOFullResFilter: the large-scale term filtered at
		// full render resolution instead of 1/4. AOAlphaBudget: an AO ray past the alpha candidate budget counts as 0 blocked (46s),
		// 1 half open, 2 open. All off / 0: 46s.
		// (47n) Fine Detail (two scales) and the Normal-Guided Filter are always on (the AODetail / AONormalGuided keys are gone).
		float AODetailStrength = 1.0f;
		float AODetailLength = 20.0f;  // units: 8 - 48 (at most half the Ray Length)
		int AODetailRays = 2;          // near rays per pixel, the long ray's near part included: 1 - 4 (the RTAO Custom tier's value)
		bool AOFullResFilter = false;  // the RTAO Custom tier's value
		int AOAlphaBudget = 0;
		// (46y) Outside the settings macro too. Screen-Space Shadows for Non-RT Casters (Point Light Shadows): for ray-traced lights
		// Screen Space Point Light Shadows run as well, but only occluders the ray tracer does not see shadow (grass, terrain, LOD,
		// decals; traced casters only within PointLightsSSPLSContact x the ray start offset). Off = 46x (Keep Screen-Space Contact
		// Shadows decides). RTAO Single Long Ray: the long direction is one closest-hit ray classified by its hit distance (same
		// near / far result as the two segments; off = 46x). RTAO Half-Resolution Broad Term: the far part is traced on a checkerboard
		// that alternates every frame (the filter fills the other half from the neighbours; off = 46x, default off).
		float PointLightsSSPLSContact = 8.0f;
		// (47q) AOBroadRate replaces AOHalfResFar (read once, for the migration): the RTAO Custom tier's broad (long) ray rate, 0 every
		// pixel, 1 half (checkerboard swapped every frame), 2 one in four (each 2x2 block, rotating over 4 frames).
		int AOBroadRate = 0;
		// (47h) Outside the settings macro too. AOCharacterSelfFix "RTAO: Character Self-Occlusion Fix" (TraceAOCS kFlagSelfFix).
		// SunDepthCheckTranslucent "Sun Shadow: Depth Check for Translucent Surfaces": a forward (blended) Lighting surface that is not
		// the one in the depth buffer takes the cascades at its own position instead of the ray-traced screen mask (Lighting.hlsl).
		// PointShadowsOnWater "Point Light Shadows on Water": (47i) the point trace also traces from last frame's water surface; the
		// Water shader uses that where its fragment matches (TracePointLightsCS kFlagWater, PointLightShadows.hlsli GetWater). All three off = 47g.
		// (47n) The Character Self-Occlusion Fix is always on (the AOCharacterSelfFix key is gone).
		// (47o) SunDepthCheckTranslucent, PointShadowsOnWater, PointLightsSSPLSNonRT: permanent (not settings); PointLightsKeepSSPLS: gone.
		// (47j) AOSmoothNormals "RTAO: Smooth Normals": the AO hemisphere around last frame's G-buffer normal reprojected to this frame
		// (AONormalPrepCS, confidence-blended toward the depth normal), not the per-facet depth normal. Off = 47i. (47n) The RTAO Custom
		// tier's value; its v2 refinements (the 47k AOSmoothNormals2) are always on.
		bool AOSmoothNormals = true;
		// (47l) Outside the settings macro too. AOTemporalV2 "RTAO: Temporal v2" (needs Temporal): per-pixel history length,
		// disocclusion test against our own last-frame depth and normal, depth-weighted history taps, a statistical clamp (both
		// terms), a history-fix blur while the history is short, and the large-scale blur's radius in world units (RTAOTemporalCS
		// RTAO_TEMPORAL_V2, RTAOFilterCS). Off = 47k. (47m) The 47l Blue-Noise Rays option is gone (interleaved gradient noise only).
		// (47n) The RTAO Custom tier's value.
		bool AOTemporalV2 = true;
		// (47n) Quality tiers, outside the settings macro. SunShadows false = the sun's "Off" (QualityLevel is kept, so turning the sun
		// back on returns to its tier). PointQuality / AOQuality: 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Custom; their Custom values are
		// the older keys (PointLightsPerPixel, -PerPixelInterior, -MaxSources, -Distance, -AlphaOutdoors; AODetailRays, AODistance,
		// AOHalfResFar, AOFullResFilter, AOTemporalV2, AOSmoothNormals). Their "Off" = PointLights / AO false.
		bool SunShadows = true;
		int PointQuality = 2;  // High (the 46b-47m defaults)
		int AOQuality = 1;     // Medium
		// (47n) RTAO ghosting: "RTAO: Tight History Near Moving Things" (AODynamicFlag): pixels whose AO rays hit a character or a moving
		// object (this frame or last) keep at most 1 frame of Temporal v2 history and a tight 3x3 clamp.
		bool AODynamicFlag = true;
		// (47l) Outside the settings macro too. FoliageSway "Sway in RT Shadows" (under Foliage): with Foliage ray traced / Both, tree-
		// animated leaves, bushes and branches are traced with the game's wind (a wind BLAS per mesh, posed and refit every frame).
		// Off = 47k (frozen at their rest pose). No effect with Foliage = Shadow map.
		bool FoliageSway = true;
		// (47q) Outside the settings macro. SunSize "Shadow Edge Softness (sun size)": the sun's cone (degrees, half angle), a look slider
		// outside the tiers and Custom (0.27 = the real sun). Saved before 47q (no key): the cone the tier in effect had (High / Ultra 0.5,
		// the others 0.27, Custom its CustomSunCone), so nothing changes on screen.
		float SunSize = 0.27f;
		// (47q) The sun's Custom tier: NPCs beyond this distance (units, camera to actor) are traced with their merged body only (no physics
		// parts, hair, cut-out parts; those keep the engine cascades) and refit every other frame. 0 = no limit (all parts, every frame).
		float CustomNpcFullDistance = 3150.0f;
		// (47q) Foliage sway: "Keep Leaves Moving on Long Frames". The game itself stops advancing the tree-leaf animation timers on any frame
		// of 35 ms or more (under ~28.6 real fps, typical with frame generation): leaves on screen and their RT shadow freeze until the
		// frames get shorter. On: for the swaying plants in reach, the frozen timers are advanced as the game would (each plant's own
		// learned rate), before anything is drawn, so leaves, their shadow maps and the RT shadow keep moving. Off: as the game.
		bool FoliageKeepMoving = true;
		// (47r) Outside the settings macro. Each on by default; off = 47q exactly.
		// AOPostTracedOnly "RTAO: Filter Only Where Traced": the RTAO filter / history passes skip the 8x8 tiles that are far from every
		// traced pixel (sky, beyond the fade end, outside the traced area; a tile mask with the reach each pass needs), and the
		// history pass works in 16x16 groups. The traced pixels get exactly the 47q values.
		bool AOPostTracedOnly = true;
		// SunCombineTiles "Sun: Fast Path for Uniform Tiles": Combine skips the per-pixel statistics where the denoiser's 5x5 tiles are
		// all fully lit or all fully shadowed (Penumbra denoiser only). Exact.
		bool SunCombineTiles = true;
		// SSSTileMask "Screen Space Shadows: Tile Mask for RT Casters": the contact-shadow pass reads a per-tile summary of the traced-
		// caster mask (all traced / none traced) and the per-pixel mask only in mixed tiles. Exact.
		bool SSSTileMask = true;
		// PointProbeShareAll "Point Lights: Reuse the Sun's Surface Probe Everywhere": the point pass's non-alpha variant (Low / Medium
		// outdoors) reuses the sun's probe too where it means the same (no hit, or a hit on a forced-opaque instance). Exact.
		bool PointProbeShareAll = true;
		// FoliageLearnAnyFrame "Keep Leaves Moving: Learn on Any Frame": a plant's leaf rate is learned from every advance the game
		// makes (any frame time), and a plant that never learned uses the rate shared by the others once they agree. Off = 47q.
		bool FoliageLearnAnyFrame = true;
		// (47u) Outside the settings macro. Each off = 47t exactly (for these paths the same as 47r; 47v removed 47s / 47t).
		// SunTraceEarly "RT: Start Sun Trace Earlier": RTAO's normal prep no longer runs between the depth prepass and the signal the sun
		// trace waits for, but right behind it (beside the traces), reading last frame's depth from its own copy. Exact.
		bool SunTraceEarly = true;
		// SkipIrrelevantUpdates "RT: Skip Updates That Cannot Affect the Image": swaying plants and NPCs whose (swaying / posed) bounds no
		// ray of the frame can reach (outside the view extruded toward the sun, the point lights' reach and the near rays) are not posed /
		// refit; checked again against the frame's exact rays before the trace (anything reachable is refit that same frame first).
		bool SkipIrrelevantUpdates = true;
		// SpatialStaticChunks "RT: Spatial Static Chunks": a captured cell's statics are packed into their 131k-triangle chunks in spatial
		// (Morton) order instead of reference order, and outdoors one- and two-sided shapes share chunks (no ray there culls faces).
		// A change re-captures the scene. Default from the offline traversal test (47u as built).
		bool SpatialStaticChunks = false;
	};
	/// (46f) RTAO modes (Settings::AOMode); with RTAO off the AO is SSGI's, exactly as before.
	enum class AOModeKind : int
	{
		KeepContact = 0,  // RTAO replaces SSGI's AO; SSGI's contact AO kept (both multiplied: the darker of the two terms)
		Darker = 1,       // the darker of RTAO and SSGI's whole AO
		Count
	};
	enum class AODebugView : int
	{
		Off = 0,
		Raw,       // this frame's rays (white open, black blocked)
		Filtered,  // after the filter (and history)
		Split,     // left half RTAO, right half SSGI AO (as before)
		RawDetail,       // (46t) this frame's near rays (open share)
		FilteredDetail,  // (46t) the detail term after its filter (and history)
		Final,           // (46t) 1 - the final AO the lighting gets
		NormalConfidence,  // (47k) Smooth Normals: green = used (by its confidence), red = fell back, blue = no data
		NormalNs,          // (47k) the normal the AO hemisphere is built around (RGB = xyz * 0.5 + 0.5)
		NormalNg,          // (47k) the depth (facet) normal
		History,           // (47n) Temporal v2: red = history rejected, green = clamp active, blue = history length / 32, white dots = moving-occluder flag
		Count
	};
	Settings settings;
	DebugView debugView = DebugView::Off;  // not persisted

	enum class Status
	{
		Off,
		Unavailable,   // initialization failed; reason in statusReason
		Failed,        // a runtime failure (device removed, D3D12 error); reason in statusReason
		Inactive,      // interior / no sky / loading: nothing traced this frame
		Waiting,       // on, nothing captured yet
		Active
	};

	virtual void DrawSettings() override;
	// (46g) The Ray Tracing category: the feature entry (master switch, status, diagnostics) and three more entries.
	void DrawMasterHeader();
	void DrawSunPage();
	/// (47n) Main page: the per-kind rows (sun, point lights and RTAO share them).
	void DrawObjectRows();
	void DrawPointLightPage();
	void DrawAOPage();
	void DrawDiagnostics();
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	/// (46p) Precompile Lazy Shaders: the D3D11 passes the current settings will use (main thread, outside the frame's passes).
	void PrecompileShaders();
	virtual void DataLoaded() override;
	virtual void PostPostLoad() override;
	virtual void Prepass() override;

	/// (46c) The engine is about to draw this spot / omni light's shadow maps (BSShadowLight::RenderShadowmaps hook):
	/// true = skip them (the light is fully handled by RT this frame and its mask channel will be written by RT).
	bool SkipShadowMap(RE::BSShadowLight* a_light);

	/// Deferred::Hooks::Main_RenderShadowMaps, before the engine draws any shadow map: decides
	/// whether this frame's sun cascades leave out the covered casters.
	void LatchFrame();
	/// Deferred::EndDeferred, after the deferred composite: the debug views (no-op when off).
	void DrawDebugOverlay();
	/// A loading screen opened (any thread): every captured cell is dropped on the next frame.
	/// (45k) a_open false: it closed (the warm-up starts).
	void OnLoadingScreen(bool a_open);
	/// (46g) Fader Menu open / closed (the black screen after a loading screen), from the UI thread.
	void OnFader(bool a_open);
	/// (46k) Engine events of a transition (any thread; they only record / queue, the render thread acts on them).
	void OnCellFullyLoaded(const RE::TESObjectCELL* a_cell);
	void OnPositionPlayer(int a_stage);
	void OnFastTravelEnd();
	void OnLoadGame();
	/// (46k) Present hook, every presented frame (State::Reset): the loading pump. On a frame without a world render while a
	/// loading screen is up it captures / builds the destination (D3D12 only, nothing traced); otherwise nothing.
	virtual void Reset() override;
	/// (45k) Screen Space Shadows Prepass: true = Ray Tracing draws them this frame (non-RT casters only,
	/// after its trace, with its caster mask); it then always calls ScreenSpaceShadows::DrawDeferred once.
	bool ClaimScreenSpaceShadows();
	/// (45l) Deferred::PrepassPasses, after every feature's Prepass: the D3D11 side of this frame's trace
	/// (wait, Screen Space Shadows, denoiser, combine) when Late Sync deferred it. No-op otherwise.
	void FinishPrepass();
	/// (46f) Deferred::DeferredPasses, right after Screen Space GI: the D3D11 queue waits for the RTAO trace (second fence),
	/// then its 1/4-resolution bilateral filter, the clamped history and the mix with SSGI's AO (ScreenSpaceGI::aoOverride).
	/// No-op unless this frame traced RTAO.
	void DrawAmbientOcclusion();

	/// (46b) Light Limit Fix, for each light it uploads: the ray-traced shadow tag (LightData::rtShadow), 0 unless
	/// this frame's trace has a shadow source for the light.
	[[nodiscard]] std::uint32_t PointLightTag(const RE::BSLight* a_light) const { return pointSelector.TagFor(a_light); }
	/// (46y) Debug view SSPLS Coverage: Screen Space Point Light Shadows asks Lighting.hlsl to draw it (SSPLS.hlsli CoverageView).
	[[nodiscard]] bool SSPLSCoverageView() const { return debugView == DebugView::PLSsplsCoverage; }
	/// (46x) Light Census: this frame's 46x class of a light (RayTracingPointLights::Decision | why << 8; 0 = none). Render thread.
	[[nodiscard]] std::uint16_t PointLightDecision(const RE::BSLight* a_light) const { return pointSelector.DecisionFor(a_light); }
	/// (46x) Light Census: this frame's point-light selection counts (fill classes, portals).
	nlohmann::json PointSelectionJson() const;
	/// (46s) Light Census: this frame's point-light shadow sources whose range covers the player (render thread).
	nlohmann::json PointSourcesReachingPlayerJson() const;

	/// True when this frame's shadow mask is (being) ray traced.
	[[nodiscard]] bool IsActiveThisFrame() const { return activeThisFrame; }
	/// (47h) This frame's sun shadow mask was written by the trace (outdoors) and "Sun Shadow: Depth Check for Translucent Surfaces" is
	/// on: forward Lighting surfaces that are not the depth buffer's take the cascades at their own position (LightLimitFix strict flags).
	[[nodiscard]] bool SunMaskDepthCheckActive() const;
	[[nodiscard]] Status GetStatus() const { return status; }

	/// For the F7 frame trace: D3D12 GPU ms (last completed), CPU ms this frame, BLAS builds this frame.
	struct TraceSample
	{
		float gpuMs = -1.0f;
		float cpuMs = -1.0f;
		std::uint32_t builds = 0;
		std::uint32_t instances = 0;
	};
	[[nodiscard]] TraceSample GetTraceSample() const { return traceSample; }
	/// (45j) One line of the switches that matter for a profile (trace / F12 JSON headers).
	std::string DescribeForProfile() const;
	nlohmann::json DescribeJson() const;

private:
	struct alignas(16) CombineCB
	{
		float InvViewProj[16];
		float Rect[4];       // camera-relative xy min / max of the ray-traced area
		float Band[4];       // x hand-off band width, y filter radius (px), z flags (bits), w temporal blend
		std::uint32_t Size[4];  // render width, height, debug view, frame
		float PrevViewProj[16];  // (45j) last combined frame's CameraViewProj
		float CameraDelta[4];    // (45j) camera position now minus then
		float Denoise[4];        // (45k) tan(cone), max radius px, max history, clamp sigmas
		float Extra[4];          // (45k) fade-in, blocker range, unused, (45l) max filter taps per side
		float Falloff[4];        // (45l) fall-off start, end, filter radius scale beyond the end, far pattern
		float ViewProjUnjittered[16];      // (45l) SIGMA motion vectors
		float PrevViewProjUnjittered[16];  // (45l)
		float CameraData[4];     // (45l) SIGMA viewZ (Util::GetCameraData)
		float SunDirection[4];   // (45l) to the sun, tan(cone)
	};
	STATIC_ASSERT_ALIGNAS_16(CombineCB);

	/// (45l) What a quality tier (or Custom) sets.
	struct QualityParams
	{
		std::uint32_t baseRays = 1, extraRays = 1;
		float coneDegrees = 0.27f;
		std::uint32_t smallFilterRadius = 1;    // the small Combine filter (Off)
		std::uint32_t tracePattern = 0, farPattern = 0;  // 0 full, 1 checkerboard, 2 half
		float falloffStart = 0.0f, falloffEnd = 0.0f;  // end <= start: no fall-off
		bool npcReduced = false;
		float denoiseRadius = 16.0f;
		std::uint32_t denoiseTaps = 6;
		bool denoiseHalf = false;
		float farDenoiseScale = 1.0f;
		float history = 12.0f;
		float clampSigma = 1.5f;
		float npcFullDistance = 0.0f;  // (47q) NPCs beyond it: body only, refit every other frame (0 = no limit)
	};
	QualityParams ResolveQuality() const;
	void CopyTierToCustom(int a_tier);
	/// (47n) What a point-light / RTAO tier (or Custom: the older keys) sets.
	struct PointTierParams
	{
		int k = 2, kInterior = 4, maxSources = 64;
		float distance = 4000.0f;
		bool alphaOutdoors = true;
		bool player = true, effects = true;  // (47q) tier knobs (Custom: PointLightsPlayer / PointLightsEffects)
	};
	struct AOTierParams
	{
		int detailRays = 1;
		float distance = 5000.0f;
		int broadRate = 1;  // (47q) 0 every pixel, 1 half, 2 one in four
		bool fullResFilter = false, temporalV2 = true, smoothNormals = true;
	};
	[[nodiscard]] PointTierParams ResolvePointTier() const;
	[[nodiscard]] AOTierParams ResolveAOTier() const;
	void CopyPointTierToCustom(int a_tier);
	void CopyAOTierToCustom(int a_tier);
	/// (47q) The NPC rule's distance: the sun tier's; with the sun Off the point-light tier's, then RTAO's (0 = no limit).
	[[nodiscard]] float NpcFullDistance() const;
	/// (47n) The sun's shadows are traced (Sun Shadows not "Off").
	[[nodiscard]] bool SunOn() const { return settings.SunShadows; }
	/// (47n) Ray Tracing has something to do: on, and at least one of sun / point lights / RTAO not "Off" (else: as switched off).
	[[nodiscard]] bool RtWanted() const { return settings.Enabled && (settings.SunShadows || settings.PointLights || settings.AO); }
	// (47n) Sun Off outdoors: frames since it was picked (its sun-only textures go after a calm period).
	std::uint32_t sunOffFrames = 0;
	bool sunOffReleased = false;
	void ReleaseSunOnlyResources();
	/// (45l) Upscaling's Ray Reconstruction is running (DLSS, RR switched on and available).
	static bool UpscalingRRActive();
	/// (45l) The denoiser in effect: DLSS RR whenever Upscaling's Ray Reconstruction runs, else the chosen one.
	[[nodiscard]] DenoiserMode EffectiveDenoiser() const;

	bool EnsureBackend();
	bool EnsureD3D11Resources(const D3D11_TEXTURE2D_DESC& a_maskDesc, bool a_maskTemp = true);
	/// (46q) The debug texture only while a debug view is on (Size Buffers to Render Resolution); false = not available.
	bool EnsureDebugColor();
	void ReleaseD3D11Resources();
	void Disable(Status a_status, std::string a_reason);
	void RunFrame();
	/// (47n) RunFrame's outdoor frame with Sun Shadows "Off": point lights and RTAO only (as RunInteriorFrame).
	void RunNoSunFrame(ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_mask, const float a_traceRect[4], double a_camX, double a_camY, double a_camZ,
		std::uint32_t a_width, std::uint32_t a_height, bool a_validate);
	bool ComputeRects(float o_traceRect[4], float o_coreRect[4], double a_camX, double a_camY);

	ID3D11ComputeShader* GetShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name);

	RayTracingRT::Backend backend;
	RayTracingScene::Scene scene;
	RayTracingCharacters::Characters characters;  // (45i)
	std::vector<RayTracingRT::SkinDraw> skinDraws;
	std::vector<float> skinBones;
	std::vector<RayTracingRT::Instance> instances;

	winrt::com_ptr<ID3D11ComputeShader> prepareCS;
	winrt::com_ptr<ID3D11ComputeShader> combineCS;
	winrt::com_ptr<ID3D11ComputeShader> overlayCS;
	winrt::com_ptr<ID3D11ComputeShader> alphaMaskCS;  // (45h) 256x256 alpha copies
	ID3D11ComputeShader* GetShaderDefined(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name, const char* a_define);
	bool alphaMaskRgba = false;                        // the variant alphaMaskCS was compiled as
	std::unique_ptr<ConstantBuffer> combineCB;
	winrt::com_ptr<ID3D11Texture2D> maskTemp;  // the combined mask before it is copied over the engine's
	winrt::com_ptr<ID3D11UnorderedAccessView> maskTempUAV;
	winrt::com_ptr<ID3D11Texture2D> debugColor;  // debug view colour + opacity (RGBA8), written by CombineCS
	winrt::com_ptr<ID3D11UnorderedAccessView> debugColorUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> debugColorSRV;
	// Last combined frame's camera (history reprojection). (45l) The 45j history textures are gone: Temporal
	// uses the Penumbra history (History4) now.
	float prevViewProj[16]{};
	float prevViewProjUnjittered[16]{};  // (45l) SIGMA motion vectors
	double prevCamera[3]{};
	bool prevCameraValid = false;
	// (45k) Denoiser Penumbra: tiles (RGBA16F, 1/8 size), horizontal and final filter (RG16F), history ping-pong (RGBA16F).
	struct DenoiseTexture
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	};
	// (45l) denoiseHalfIn: the half-resolution filter input (RG16F); denoiseH / denoiseOut are half size when denoiseIsHalf.
	DenoiseTexture denoiseTiles, denoiseH, denoiseOut, denoiseHalfIn, history4[2];
	bool denoiseIsHalf = false;
	std::uint32_t history4Index = 0;
	bool history4Valid = false;
	std::uint64_t denoiseBytes = 0;
	bool EnsureDenoiser(bool a_half);
	void ReleaseDenoiser();
	winrt::com_ptr<ID3D11ComputeShader> denoiseTileCS, denoiseHCS, denoiseVCS, denoiseHalfHCS, denoiseHalfVCS;
	// (45l) Denoiser NRD SIGMA: its own NRD instance (not the SSRT ones) and its inputs / output.
	std::unique_ptr<NRDReblurIntegration> sigma;
	DenoiseTexture sigmaViewZ, sigmaNormal, sigmaMotion, sigmaPenumbra, sigmaOut;
	winrt::com_ptr<ID3D11ComputeShader> sigmaPrepCS;
	std::uint64_t sigmaBytes = 0;
	std::uint32_t sigmaFrame = 0;
	bool sigmaHistoryValid = false;
	std::string sigmaFault;
	float sigmaPrevWorldToView[16]{}, sigmaPrevProj[16]{};
	float sigmaPrevJitter[2]{};
	std::uint16_t sigmaPrevRect[2]{};
	bool EnsureSigma();
	void ReleaseSigma();
	bool RunSigma(const float a_toSun[3]);
	// (45l) The second half of a traced frame (the D3D11 wait for the trace, Screen Space Shadows, the
	// denoiser, the combine), run right after the trace (Late Sync off) or after every other prepass.
	struct PendingFrame
	{
		bool active = false;
		std::array<std::uint8_t, sizeof(CombineCB)> cbBytes{};  // the frame's CombineCB (bytes: no alignment padding here)
		std::uint32_t combineFlags = 0;
		std::uint32_t width = 0, height = 0;
		bool writeSSS = false, debug = false, penumbra = false, spatial = false, half = false, sigma = false, sssNonRT = false;
		bool interior = false;  // (46c) point lights only: no sun combine
		bool noSun = false;     // (47n) outdoors with the sun "Off": point lights / RTAO only (no combine, no mask channels)
		bool traceSync = true;  // (46h) FinishFrame waits for the trace (false: an interior frame with RTAO only; the AO wait covers it)
		bool splitSun = false;  // (46m) Split Trace Sync: the sun fence first (shadow mask passes), the rest before the point lights' denoiser
		float biasConstant = 0.0f, biasPerUnit = 0.0f;
		float toSun[3]{};
		double camera[3]{};
		float viewProj[16]{}, viewProjUnjittered[16]{};
	};
	PendingFrame pending;
	void FinishFrame();
	void ReportFrame();
	/// (45n) Early Prep (LatchFrame): characters and this frame's builds / skinning on the D3D12 queue.
	void RunEarlyPrep();
	bool earlyThisFrame = false;       // PrepareFrame succeeded this frame
	bool charactersUpdatedEarly = false;  // characters.Update / CollectDraws already ran this frame
	float earlyCpuMs = 0.0f;           // CPU of RunEarlyPrep this frame
	float earlyCharactersMs = 0.0f;
	std::int64_t frameStart = 0;
	float prepassCpuMs = 0.0f;
	QualityParams lastQuality{};
	DenoiserMode lastEffectiveDenoiser = DenoiserMode::Off;
	DenoiserMode denoiserStableMode = DenoiserMode::Off;  // (46p) the effective denoiser and for how many frames it has held
	int denoiserStableFrames = 0;
	// (45k) Screen Space Shadows drawn by us this frame (non-RT casters only).
	bool sssClaimed = false;
	// (45k) Loading screens and the warm-up after them.
	std::atomic<bool> loadingOpen{ false };
	std::atomic<bool> loadingClosed{ false };  // set by the menu event, consumed on the render thread
	// (46g) Black: the loading screen closed, the screen is still black (Fader Menu open). Nothing visible: large
	// budgets, the hand-off and the first traced frames (denoiser history) happen here when the scene is ready.
	// WarmUp: visible, not handed off yet (moderate budgets). Play: handed off (the fade may still run).
	enum class Phase
	{
		Play,
		Loading,
		WarmUp,
		Black
	};
	Phase phase = Phase::Play;
	std::int64_t warmUpStart = 0;      // QPC when the loading screen closed
	float fade = 1.0f;                 // RT result fade-in after a loading screen
	std::uint32_t loadShapesAtOpen = 0, loadShapesAtClose = 0, loadBuildsAtOpen = 0, loadBuildsAtClose = 0;
	float lastTimeToReadyMs = -1.0f;
	std::string lastLoadingSummary;
	void UpdatePhase();
	// (46g) Transitions: the Fader Menu (black screen after a loading screen), the one global hand-off, the late batch.
	std::atomic<bool> faderOpen{ false };
	bool blackScreen = false;          // this frame: after a loading screen, the Fader Menu still open (F7 rt_black)
	bool inTransition = false;         // from the loading screen opening until the late batch is handed off
	bool handedOff = true;
	std::int64_t handOffQpc = 0;
	bool lateHold = false;             // after the hand-off: items not ready then wait for one late batch
	std::int64_t lateHoldUntil = 0;
	void HandOffNow(const char* a_when);
	/// (46g) Loading / black screen, or visible before the hand-off: capture, builds, character uploads, pool growth; no trace.
	void TransitionFrame();
	struct TransitionPhaseLog
	{
		std::uint32_t frames = 0;
		double wallStartQpc = 0.0, wallEndQpc = 0.0;
		double rtCpuMs = 0.0;
		std::uint64_t buildsAtStart = 0, buildsAtEnd = 0;
		std::uint64_t uploadsAtStart = 0, uploadsAtEnd = 0;
		std::uint64_t poolBlocksSync = 0, poolBlocksAsync = 0;
		std::uint32_t shapesAtEnd = 0;
		bool seen = false;
	};
	// [0] loading screen, [1] black screen, [2] (46n) fading in before the hand-off, [3] visible before the hand-off, [4] the first 3 s after it.
	std::array<TransitionPhaseLog, 5> transitionLog{};
	int transitionLogSlot = -1;
	std::int64_t transitionLogEndQpc = 0;
	std::string handOffSummary;
	void TransitionLogFrame(float a_rtCpuMs);
	void TransitionLogFlush();
	std::uint32_t beginFrameIndex = UINT32_MAX;  // (46g) frame of the last Backend::BeginFrame
	bool handOffThisFrame = false;               // (46g) F7 rt_handoff
	/// (46k) The destination (not the place left) is captured and built: no flush pending, every attached cell of the grid in the
	/// scene and complete, none still attaching, the player inside a captured cell, the build / upload queues empty.
	[[nodiscard]] bool DestinationReady(double a_sinceCloseMs) const;
	// (46k) Transition events (recorded by the event sinks on any thread, guarded by eventsMutex) and the loading pump.
	struct TransitionEvents
	{
		std::int64_t loadingOpen = 0, loadingClose = 0, teleportPre = 0, teleportFinish = 0, fastTravelEnd = 0, loadGame = 0;
		std::int64_t firstCell = 0, lastCell = 0;
		std::uint32_t cells = 0;
		bool pending = false;  // its events line is not logged yet
	};
	mutable std::mutex eventsMutex;
	TransitionEvents events;                                                      // guarded by eventsMutex
	std::vector<std::pair<const RE::TESObjectCELL*, std::int64_t>> cellEvents;  // guarded by eventsMutex
	std::atomic<std::int64_t> faderOpenQpc{ 0 }, faderCloseQpc{ 0 };
	std::atomic<std::uint32_t> renderThreadId{ 0 };  // the thread that renders the world (Prepass), the main thread in SE
	std::uint32_t lastPrepassFrame = UINT32_MAX;     // State::frameCount of the last Prepass (a world frame)
	bool pumpThisFrame = false;                      // F7 rt_pump
	float pumpCpuMs = 0.0f;
	std::uint32_t pumpFrames = 0, pumpForeignFrames = 0, pumpMidTeleport = 0;  // this transition
	double pumpCpuTotalMs = 0.0;
	bool pumpForeignLogged = false;
	std::int64_t pumpOpenSeen = 0;  // the loading screen the pump counters belong to (TransitionEvents::loadingOpen)
	// (46n) Preparation on the main thread from TESCellFullyLoadedEvent (behind the loading screen). Counters of this transition.
	struct CellEventLog
	{
		std::uint32_t events = 0;       // events while a loading screen was up and the path was on
		std::uint32_t steps = 0;        // steps run
		std::uint32_t skipped = 0;      // steps not run (backend not ready, a D3D11 wait pending, ...)
		std::uint32_t midTeleport = 0;  // steps run between the teleport's start and finish
		std::uint32_t cellsCaptured = 0;  // cells the scene took in during the steps (gate: captured)
		std::uint32_t shapes = 0;       // shapes captured during the steps
		std::uint64_t builds = 0;       // BLAS built during the steps
		double cpuMs = 0.0, maxMs = 0.0;
		std::string lastSkip;
	};
	CellEventLog cellEventLog;
	std::int64_t cellEventOpenSeen = 0;
	bool cellEventActive = false;     // re-entrancy guard
	bool cellEventBroken = false;     // a fault in a step: the path stays off for the session
	std::atomic<std::uint32_t> cellEventForeign{ 0 };  // events on another thread than the world's (nothing done there)
	std::atomic<bool> cellEventForeignLogged{ false };
	std::string cellEventFault;       // set by a faulting step (main thread), acted on by the next Prepass
	/// From OnCellFullyLoaded: one budgeted step on the world's thread while a loading screen is up.
	void PrepareOnCellEvent();
	/// The step itself (capture, pools, BLAS builds; D3D12 only, nothing D3D11). Called through an SEH guard.
	void CellEventStep(float a_budgetMs);
	static void CellEventStepThunk(void* a_self);
	float cellEventBudgetMs = 0.0f;
	/// (46n) The fade-in after the loading screen: 1 for the first second while the Fader Menu is open, then down to 0 at 3 s.
	[[nodiscard]] float FadeInFactor(double a_sinceCloseMs) const;
	float lastFadeFactor = 0.0f;
	bool eventSinksRegistered = false, playerSinkRegistered = false;
	std::string lastEventsSummary;  // the last transition's events line (page)
	std::int64_t activeNoBlasSince = 0;  // (46k) status active while no BLAS exists (should never last)
	bool activeNoBlasLogged = false;
	void RegisterEventSinks();
	/// The player has arrived during this transition (the teleport finished, or the game loaded).
	[[nodiscard]] bool Arrived() const;
	/// Fully-loaded cells queued by the event sink go to the scene (only during a transition, only those after it began).
	void DrainTransitionEvents();
	/// Behind a loading screen, from Reset(): capture / build / prepare characters, no trace.
	void LoadingPump();
	/// Ray Tracing switched off (Prepass, or the pump during a loading screen): the scene, the pools and the states go.
	void SwitchedOffFrame();
	/// Prepass after the phase update: the transition frame or the traced frame, then the end of the frame.
	void PrepassFrame();
	/// The events line once the transition is over (also with Ray Tracing off: the control run).
	void MaybeLogTransitionEvents();
	std::string DescribeTransitionEvents(const TransitionEvents& a_events) const;
	// (46g) The window beams of last frame's slots (their sampled-cone counters are read back a few frames later).
	std::uint64_t beamSamplesSeen = 0;
	std::array<const RE::BSLight*, 64> lastSlotBeams{};
	// (46n) One line per interior after its warm-up: how much of each window beam's sampled cone was blocked (with the 46n chunk split
	// expected well below 100 %). Collected from read-back 60 to 300 frames after "Interior ready".
	struct BeamShare
	{
		std::uint64_t traced = 0, blocked = 0;
	};
	std::unordered_map<const RE::BSLight*, BeamShare> beamShares;
	std::uint32_t beamShareFrom = 0, beamShareUntil = 0;
	bool beamSharePending = false;
	void UpdateBeamShares();
	/// (46n) Window Beam Culling in effect: fixed (47m) at "Faces Toward the Room" (1).
	[[nodiscard]] static constexpr int EffectiveBeamCull() { return 1; }
	/// Off, Penumbra, DLSS RR or SIGMA; anything else (the removed Temporal = 1) reads as Penumbra.
	[[nodiscard]] static int ValidDenoiser(int a_value)
	{
		return (a_value == 0 || (a_value >= 2 && a_value < static_cast<int>(DenoiserMode::Count))) ? a_value : static_cast<int>(DenoiserMode::Penumbra);
	}
	[[nodiscard]] DenoiserMode CurrentDenoiser() const { return static_cast<DenoiserMode>(ValidDenoiser(settings.Denoiser)); }
	/// (45o) Picking a denoiser in the dropdown: DLSS RR switches Upscaling's Ray Reconstruction on, any other
	/// one switches it off while it is on (the same setting as Upscaling's checkbox).
	void OnDenoiserPicked();
	// (45j) CPU split of the last frame (also in the F7 trace), smoothed and worst for the page.
	struct CpuSplit
	{
		float total = 0, scene = 0, characters = 0, instances = 0, missed = 0, execute = 0, d3d11 = 0, endFrame = 0;
	};
	CpuSplit cpuLast{}, cpuSmoothed{}, cpuWorst{};
	void PublishCpuSplit();
	RayTracingScene::SceneConfig SceneConfigFromSettings() const;
	D3D11_TEXTURE2D_DESC maskDesc{};
	bool maskDescValid = false;       // (47n) maskDesc holds the current size (MaskTemp itself may be absent: sun Off)
	std::uint64_t maskTempBytes = 0;  // (47n)
	std::uint64_t d3d11Bytes = 0;
	bool typedUavLoadR8 = false;

	Status status = Status::Off;
	std::string statusReason;
	std::string lastFailureReason;  // shown again if the start-up is not retried (45d)
	bool backendTried = false;
	bool activeThisFrame = false;
	bool sunMaskWrittenThisFrame = false;  // (47h) FinishFrame merged the trace into the engine's sun shadow mask this frame
	std::uint32_t sunMaskFrame = UINT32_MAX;  // (47h) State::frameCount of that frame
	bool combinedThisFrame = false;
	bool filterLatched = false;
	bool loadingFlushRegistered = false;
	std::uint32_t frameIndex = 0;
	std::uint32_t lastOverlayFrame = UINT32_MAX;
	float filterRect[4]{};
	float lastCpuMs = 0.0f;
	TraceSample traceSample{};
	std::uint32_t latchedWithoutTrace = 0;  // (45e) frames whose shadow map left casters out but that could not trace (should stay 0)
	bool overVramBudget = false;
	Status loggedStatus = Status::Off;
	// (46i) "Loading the scene" for long while playing: one diagnostic line (scene, build queue, pools, phase).
	std::int64_t waitingSinceQpc = 0;
	bool waitingLogged = false;
	void LogStuckWaiting();
	std::int64_t lastTlasLog = 0;  // (45f) QPC of the last TLAS summary in the log (0 = log on the next active frame)
	// (45h) Alpha-tested casters (45j: Foliage / Cut-outs): as last seen (a change by the user clears an
	// old fault) and why they were switched off automatically (shown on the page).
	bool alphaRequested = false;
	std::string alphaOffReason;
	void UpdateAlphaCasters();
	// (45i) Characters: likewise.
	bool charactersRequested = false;
	std::string charactersOffReason;
	void UpdateCharacters();
	// (47l) Foliage sway: likewise (a wind fault switches FoliageSway off); this frame's wind into an RT frame input.
	bool windRequested = false;
	std::string windOffReason;
	void FillWind(RayTracingRT::FrameInput& a_input);
	// (47q) Static chunks per cell / overlap (F7, F12): refreshed every 120 frames.
	std::uint32_t chunkStatsFrame = UINT32_MAX;
	std::uint32_t chunkStatsCells = 0;
	float chunkStatsPerCell = 0.0f, chunkStatsOverlap = 0.0f;

	// (46b) Point-light shadows.
	struct alignas(16) PointDenoiseCB
	{
		float Params[4];  // emitter radius, this frame's weight in the history, flags (bits), max filter radius (px)
		float Extra[4];   // K, (46o) window-beam source slots (bits 0-31, 32-63 as uint bits), tan of the beam softness (0 = 46n)
		std::uint32_t Motion[16];  // (46d) per source slot one byte: history dropped this frame (0 keep .. 255 none), 4 slots per uint
		// (46x) Source slots with their own emitter radius (bits 0-31, 32-63; direct fills without a window spot), that radius, unused.
		std::uint32_t Emitter[4];
	};
	STATIC_ASSERT_ALIGNAS_16(PointDenoiseCB);
	RayTracingPointLights::Selector pointSelector;
	std::array<RayTracingPointLights::GpuSource, RayTracingPointLights::kMaxSources> pointSources{};
	RayTracingRT::PointLightInput pointInput{};
	bool pointThisFrame = false;      // the point-light trace was submitted with this frame's trace
	bool pointSplit = false;          // debug A/B: RT point-light shadows on the left half only (not persisted)
	bool pointWaterOn = true;         // (47o) Point Light Shadows on Water (permanent): false only after its resources failed this session
	bool pointRequested = false;
	std::string pointOffReason;       // why they were switched off automatically
	std::uint32_t pointK = 0;         // K of this frame's trace (0 = none)
	std::uint64_t pointBeamMask = 0;  // (46o) source slots of this frame's trace that are window beams
	float pointBeamTan = 0.0f;        // (46o) tan of the window-beam softness this frame (0 = 46n)
	std::uint64_t pointEmitterMask = 0;  // (46x) source slots with their own emitter radius (direct fills without a window spot)
	float pointEmitterRadius = 0.0f;     // (46x) ... that radius
	float pointCpuMs = 0.0f;
	// Denoiser outputs (also the history, ping-pong): visibility per slot, sources per slot, distance.
	DenoiseTexture pointVis[2], pointIdx[2], pointDist[2];
	std::uint32_t pointWrite = 0;
	bool pointHistoryValid = false;
	bool pointDenoised = false;       // FinishPointLights ran this frame (the history is next frame's)
	bool pointBound = false;          // t40 / t41 of the pixel shaders hold pointVis / pointIdx
	std::uint64_t pointD3D11Bytes = 0;
	std::unique_ptr<ConstantBuffer> pointDenoiseCB;
	winrt::com_ptr<ID3D11ComputeShader> pointDenoiseCS;
	/// Turns the point lights off (not the rest of Ray Tracing) with a reason for the page.
	void SwitchOffPointLights(std::string a_reason);
	void UpdatePointLights();
	bool EnsurePointDenoiser(std::uint32_t a_width, std::uint32_t a_height);
	void ReleasePointDenoiser();
	/// Selection + this frame's trace input (RunFrame, before the trace is recorded). False = no point lights this frame.
	/// (46c) a_interior: K indoors, spot lights / window beams / forced sources, interior bounds for the beams' rays.
	bool PreparePointLights(RayTracingRT::FrameInput& a_input, const float a_traceRect[4], double a_camX, double a_camY, double a_camZ, float a_sunStrength,
		bool a_interior = false);
	/// After the combine (FinishFrame): the denoiser, then the result bound for the pixel shaders.
	void FinishPointLights();
	/// Pixel shader slots t40 / t41 back to null (before any pass writes those textures, and when nothing was traced).
	void UnbindPointLights();

	// (47i) Point Light Shadows on Water: after this frame's water draws the depth (with the water) is copied and turned into the
	// water's distance from the camera per pixel (WaterPrepCS, into the backend's shared texture); next frame's point trace traces
	// the pixel's sources from there (TracePointLightsCS kFlagWater) and the Water shader reads the result (t42 / t43).
public:
	/// State::Draw, every draw: notes water draws after the opaque pass; the first other draw after them runs the water prep.
	void OnDrawStatesApplied(ID3D11DeviceContext* a_context);
	/// Deferred::EndDeferred: the opaque pass is over (water draws from here on count).
	void OnEndDeferred();

private:
	void PrepWaterDistance(ID3D11DeviceContext* a_context);
	bool WaterTracking() const;
	uint32_t waterEndDeferredFrame = UINT32_MAX;
	uint32_t waterSeenFrame = UINT32_MAX;     // a water draw after the opaque pass this frame
	bool waterPrepPending = false;            // water drawn since the last prep
	uint32_t waterPrepFrame = UINT32_MAX;     // frame of the last prep (its distances are next frame's trace input)
	bool waterDistValid = false;              // the distance texture holds a prep (cleared when none ran last frame)
	bool waterTraced = false;                 // this frame's point trace traced from the water (t42 / t43 bound)
	bool waterLodSkip = false;                // (47l) distant LOD water draws do not ask for the prep this frame (out of every source's reach)
	// (47p, "Ray tracing: water prep only when a lamp can reach water") This frame's traced sources (world xyz, reach radius) and the
	// water drawn (world bounds); the prep runs when one reaches the other (1024 units of margin) on this frame or the last.
	std::vector<std::array<double, 4>> waterGateLights;
	std::vector<RE::NiBound> waterGateBounds;
	uint32_t waterGateBoundsFrame = UINT32_MAX;
	bool waterGateReachLast = true;
	uint32_t waterGateSkips = 0;  // preps left out (session)
	float waterInvViewProj[16]{};             // this frame's (the trace's) inverse view-projection
	uint32_t waterRenderWidth = 0, waterRenderHeight = 0;
	winrt::com_ptr<ID3D11Texture2D> waterDepthCopy;
	winrt::com_ptr<ID3D11ShaderResourceView> waterDepthCopySRV;
	D3D11_TEXTURE2D_DESC waterDepthCopyDesc{};
	winrt::com_ptr<ID3D11ComputeShader> waterPrepCS;
	std::unique_ptr<ConstantBuffer> waterPrepCB;
	// (47j) RTAO: Smooth Normals. Last frame's camera and size (DepthMask still holds its depth until PrepareCS overwrites it).
	winrt::com_ptr<ID3D11ComputeShader> aoNormalCS;
	std::unique_ptr<ConstantBuffer> aoNormalCB;
	struct AONormalPrev
	{
		uint32_t frame = UINT32_MAX;
		uint32_t width = 0, height = 0;
		float viewProj[16]{};
		float invViewProj[16]{};
		float viewInverse[16]{};
		double camera[3]{};
	} aoNormalPrev;
	bool aoNormalsThisFrame = false;
	std::string aoSmoothFault;  // (47n) Smooth Normals failed this session (its texture / shader): off until RTAO is switched on again
	// (47k) RTAO debug views "Smooth Normals: Confidence" / "Ns" / "Ng": AONormalPrepCS writes the chosen view here (only while one of
	// them is on); RTAOCompositeCS copies it into the overlay.
	DenoiseTexture aoNormalDebug;
	uint32_t aoNormalDebugWidth = 0, aoNormalDebugHeight = 0;
	uint32_t aoNormalDebugFrame = UINT32_MAX;  // frame it was written in
	/// Before PrepareCS: the smooth normals for this frame's RTAO trace (sets its flag); false = the trace keeps the depth normal.
	/// (47u) a_defer: everything but the dispatch now (constants, the trace's flag); the dispatch goes behind the ready signal
	/// (DispatchDeferredAONormal, reading last frame's depth from aoPrevDepth).
	bool RunAONormalPrep(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_depth, std::uint32_t a_width, std::uint32_t a_height, bool a_defer = false);
	/// After PrepareCS: this frame's camera, for next frame's reprojection.
	void RememberAONormalFrame(std::uint32_t a_width, std::uint32_t a_height);
	// (47u) "RT: Start Sun Trace Earlier" (Settings::SunTraceEarly): RTAO's normal prep (0.27-0.43 ms of D3D11 work that only RTAO reads)
	// no longer sits between the depth prepass and the ready signal the sun trace waits for; it runs right behind that signal, beside
	// the traces, reading last frame's depth from a D3D11 copy of DepthMask (aoPrevDepth) instead of DepthMask itself. RTAO's list waits
	// for it through the after-ready fence. Exact: the same inputs, the same dispatch.
	void DispatchAONormal(ID3D11DeviceContext* a_context, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_prevDepth, std::uint32_t a_width,
		std::uint32_t a_height, bool a_writeDebug);
	bool EarlyNormalPrepPossible(std::uint32_t a_width, std::uint32_t a_height);
	bool DispatchDeferredAONormal();
	void FlushDeferredAONormal();
	void FinishDepthMaskFrame(std::uint32_t a_width, std::uint32_t a_height);
	/// PrepareCS + (Smooth Normals) the normal prep in the order Start Sun Trace Earlier allows; a_input.afterReady set when deferred.
	void PrepareDepthMask(ID3D11DeviceContext* a_context, ID3D11ComputeShader* a_prepare, ID3D11ShaderResourceView* a_depth, ID3D11ShaderResourceView* a_mask,
		std::uint32_t a_width, std::uint32_t a_height, RayTracingRT::FrameInput& a_input);
	struct AONormalDeferred
	{
		bool active = false;
		ID3D11ShaderResourceView* depth = nullptr;  // the engine's depth SRV of this frame (alive for the frame)
		std::uint32_t width = 0, height = 0;
		bool writeDebug = false;
	} aoNormalDeferred;
	winrt::com_ptr<ID3D11Texture2D> aoPrevDepth;
	winrt::com_ptr<ID3D11ShaderResourceView> aoPrevDepthSRV;
	D3D11_TEXTURE2D_DESC aoPrevDepthDesc{};
	std::uint32_t aoPrevDepthFrame = UINT32_MAX;  // frame whose DepthMask aoPrevDepth holds
	int earlyNormalThisFrame = -1;                // (47u) F7 rt_early_normal: 1 deferred, 0 the 47t order, -1 no normal prep
	// (47u) "RT: Skip Updates That Cannot Affect the Image" (Settings::SkipIrrelevantUpdates). Early Prep culls with the early region (last
	// frame's exact rays, widened: relevanceLast); before this frame's trace, everything culled is checked against this frame's exact rays
	// (ResolveRelevance): plants reachable after all are refit before the TLAS, a reachable NPC redoes the frame's characters unculled.
	RayTracingRelevance::Inputs relevanceLast;
	RayTracingRelevance::Region relevanceEarly;
	std::uint32_t relevanceEarlyFrame = UINT32_MAX;
	bool relevanceEarlyOk = false;
	RayTracingRelevance::Region relevanceExact;
	std::uint32_t relNpcCulledThisFrame = 0;
	bool relFallbackThisFrame = false;
	std::uint64_t relFallbacks = 0, relLateTotal = 0;
	/// This frame's early region (null: nothing is culled this frame -- switch off, interiors, sun off, no exact region last frame).
	const RayTracingRelevance::Region* CurrentRelevance();
	/// The exact region of this exterior frame (its sun, point and RTAO inputs), kept for the next frame's early region.
	void BuildExactRelevance(const RayTracingRT::FrameInput& a_input, const float a_traceRect[4], double a_camX, double a_camY, double a_camZ);
	/// Checks what this frame culled against a_exact (null: everything is reachable) and updates what is reachable before the trace.
	/// a_world: the rect a character redo takes (null: none possible).
	void ResolveRelevance(const RayTracingRelevance::Region* a_exact, const float* a_world = nullptr);

	// (46c) Interiors: the interior cell's capture, its point lights and window beams, and the engine's shadow maps.
	struct alignas(16) PointMaskCB
	{
		struct Channel
		{
			float Pos[4];    // camera-relative light position, w source slot + 1 (0 = no traced result)
			float Dir[4];    // spot direction, w falloff exponent
			float Right[4];  // w tan half width
			float Up[4];     // w tan half height
			std::uint32_t Flags[4];  // x: kMask* bits
		};
		Channel Channels[4];
	};
	STATIC_ASSERT_ALIGNAS_16(PointMaskCB);
	static constexpr std::uint32_t kMaskWrite = 1u, kMaskSpot = 2u, kMaskSkipped = 4u;  // PointMaskCS
	/// Indoors this frame (an interior cell, Ray Tracing and its point lights or (46h) RTAO indoors on): RunInteriorFrame instead of the sun.
	[[nodiscard]] bool InteriorWanted() const;
	/// (46h) RTAO indoors wanted (RTAO on, Indoors Too, its shader available, Screen Space GI on as its consumer).
	[[nodiscard]] bool AOInteriorWanted() const;
	void RunInteriorFrame();
	/// Switches the interior part off (not the rest of Ray Tracing) with a reason for the page.
	void SwitchOffInteriors(std::string a_reason);
	/// After the point-light denoiser (or without a trace: a_traced false): writes the shadow mask channels of the
	/// engine shadow lights with a source (and of every light whose shadow map was not drawn).
	void WritePointMask(bool a_traced);
	std::unique_ptr<ConstantBuffer> pointMaskCB;
	winrt::com_ptr<ID3D11ComputeShader> pointMaskCS;
	bool interiorThisFrame = false;      // this frame's trace is an interior one
	bool interiorTracedLastFrame = false;
	bool interiorRequested = false;
	std::string interiorOffReason;
	const RE::TESObjectCELL* interiorCell = nullptr;  // the interior seen last (entry time)
	std::int64_t interiorEntryQpc = 0;
	float interiorReadyMs = -1.0f;       // from entering (or the loading screen closing) to the first traced frame
	bool interiorReadyPending = false;
	// (46d) Interior capture progress in the log (every 2 s while capturing) and one line when the scene is ready.
	void LogInteriorProgress(bool a_covered);
	bool interiorSceneLogPending = false;
	std::int64_t interiorProgressQpc = 0;
	std::uint32_t maskChannelsWritten = 0;  // last frame
	bool pointMaskDone = false;             // WritePointMask ran this frame
	// Engine shadow maps left out (PointLightsSkipShadowMaps).
	bool skipLatched = false;
	std::vector<const RE::BSLight*> skipCandidates;  // last frame's engine shadow lights with a source (sorted)
	struct SkippedMap
	{
		const RE::BSLight* light;
		std::uint32_t channel;
		// (46e) The light as last frame's trace had it (its spot cone): the fallback without this frame's RT result is
		// the cone, not the whole screen lit.
		bool hasEntry = false;
		RayTracingPointLights::MaskEntry entry{};
	};
	std::vector<RayTracingPointLights::MaskEntry> skipEntries;  // (46e) last frame's mask entries, sorted by light
	std::vector<SkippedMap> skippedMaps;            // this frame
	std::vector<const RE::BSLight*> skippedLights;  // this frame, sorted (the selection's forced sources)
	std::uint32_t skippedLastFrame = 0;
	std::uint64_t skippedTotal = 0;
	std::uint64_t skipFallbackFrames = 0;           // frames whose skipped maps got only the fallback (no trace)
	bool hooksInstalled = false;

	// (46f) Automatic switch-offs are a runtime state, not the saved setting: the user's choice stays on and is retried
	// next session (or with Retry / ticking the box off and on).
	bool pointAutoOff = false;
	bool interiorAutoOff = false;
	[[nodiscard]] bool PointLightsOn() const { return settings.PointLights && !pointAutoOff; }
	[[nodiscard]] bool InteriorsOn() const { return settings.PointLightsInteriors && !interiorAutoOff; }

	// (46f) RTAO.
	struct alignas(16) AOCompositeCB
	{
		std::uint32_t Size[4];  // render width, height, flags, debug view
		float Params[4];        // strength, this frame's weight in the history, history clamp slack, RT fade-in
		float Detail[4];        // (46t) detail strength, normal weight power (0 = the 45 degree gate only), unused x2
		float Camera[4];        // (46t) the game's CameraData (linear depth in RTAOFilterCS)
		// (47l) Temporal v2 (RTAOCommon.hlsli; appended at byte 64, the fields above unchanged). Filled every frame.
		float InvViewProj[16];   // 64: this frame's (jittered, camera-relative), as the trace's
		float PrevViewProj[16];  // 128: last frame's (jittered), relative to its own camera
		float ViewInverse[16];   // 192: this frame's view-to-world
		float CameraDelta[4];    // 256: xyz this camera minus last frame's; w pixels per unit at linear depth 1
		float Temporal[4];       // 272: min history weight, large-scale sigma (units), history-fix frames, max history length
		float Clamp[4];          // 288: statistical clamp k (both terms), minimum half width detail / large-scale, unused
	};
	STATIC_ASSERT_ALIGNAS_16(AOCompositeCB);
	static_assert(sizeof(AOCompositeCB) == 304, "RTAOCommon.hlsli RTAOCB");
	static constexpr std::uint32_t kAOFlagContact = 1u, kAOFlagDarker = 2u, kAOFlagHistory = 4u, kAOFlagSplit = 8u;  // RTAO*CS.hlsl (RTAOCommon.hlsli)
	static constexpr std::uint32_t kAOFlagDetail = 16u, kAOFlagValidity = 32u, kAOFlagDetailHistory = 64u;                         // (46t)
	[[nodiscard]] bool AOOn() const { return settings.AO && !aoAutoOff; }
	/// Switches RTAO off for this session (not the setting) with a reason for the page; the rest of Ray Tracing goes on.
	void SwitchOffAO(std::string a_reason);
	void UpdateAO();
	/// RunFrame / (46h) RunInteriorFrame, before the trace is recorded: this frame's RTAO input. False = no RTAO this frame.
	/// a_interior: the RT weight is 1 everywhere inside the interior's box (no distance fade).
	bool PrepareAO(RayTracingRT::FrameInput& a_input, const float a_traceRect[4], bool a_interior = false);
	[[nodiscard]] bool AOInteriorsOn() const { return AOOn() && settings.AOInteriors && !interiorAutoOff; }
	bool aoInteriorThisFrame = false;  // (46h) this frame's RTAO is the interior one (its mode: AOModeInterior)
	bool aoInteriorRequested = false;  // (46h) AOInteriors as last seen (ticking it again retries an interior switch-off)
	int aoModeThisFrame = -1;          // (46h) F7 rtao_mode: the mode of this frame's RTAO (-1 = not traced)
	// (46h) RTAO fade-in whenever it starts after a gap (entering an interior whose capture finished after the hand-off fade, RTAO
	// switched on, Screen Space GI back on): never a pop from SSGI's AO to RTAO, also outside the transition's own fade.
	std::uint32_t aoLastCompositeFrame = UINT32_MAX;
	std::int64_t aoRampStart = 0;
	float aoRamp = 0.0f;
	bool EnsureAOFilter(std::uint32_t a_width, std::uint32_t a_height);
	void ReleaseAOFilter();
	RayTracingRT::AOInput aoInput{};
	bool aoAutoOff = false;
	bool aoRequested = false;
	std::string aoOffReason;
	AODebugView aoDebugView = AODebugView::Off;  // not persisted
	bool aoThisFrame = false;      // this frame's trace carried RTAO (DrawAmbientOcclusion consumes it)
	bool aoComposited = false;     // DrawAmbientOcclusion mixed it into the AO this frame
	bool aoComposedLastFrame = false;
	bool aoDebugDrawn = false;     // its debug view wrote debugColor this frame (the overlay draws it)
	std::uint32_t aoWidth = 0, aoHeight = 0;
	DenoiseTexture aoLoDepth, aoLoNormal, aoFiltered, aoHistory[2], aoOut;
	std::uint32_t aoHistoryIndex = 0;
	bool aoHistoryValid = false;
	std::uint32_t aoHistoryFrame = 0;  // State::frameCount when the history was written (valid for the next frame only)
	std::uint64_t aoD3D11Bytes = 0;
	std::unique_ptr<ConstantBuffer> aoCB;
	winrt::com_ptr<ID3D11ComputeShader> aoTemporalCS, aoCompositeCS;
	winrt::com_ptr<ID3D11SamplerState> aoLinearSampler;
	float aoCpuMs = 0.0f;
	// (46t) Two scales, normal-guided and full-resolution filtering (RTAOFilterCS). The extra textures are made only when a
	// switch needs them (EnsureAOExtras: created into locals, kept only when all succeed); failing that, this session falls
	// back to the 46s filter (aoExtrasFault) -- RTAO and the rest of Ray Tracing go on.
	bool EnsureAOExtras(bool a_quarter, bool a_full, bool a_detail);
	void ReleaseAOExtras();
	struct AOTraceShape
	{
		bool detail = false;     // the trace carried the two scales (TraceAOCS .b / .a)
		int detailRays = 0;      // near rays per pixel
		float detailLength = 0;  // the near / far split (units)
		int alphaBudget = 0;     // AOAlphaBudget in effect
	};
	AOTraceShape aoShapeThisFrame{};  // what PrepareAO put into this frame's trace (DrawAmbientOcclusion filters accordingly)
	DenoiseTexture aoLoW, aoLoWBlur;                            // quarter resolution (value, weight), R8G8
	DenoiseTexture aoFullTemp;                                  // full resolution (value, weight) between the separable passes
	DenoiseTexture aoDetailFiltered, aoDetailHistory[2];        // the detail term
	std::uint32_t aoExtrasWidth = 0, aoExtrasHeight = 0;
	std::uint64_t aoExtrasBytes = 0;
	std::string aoExtrasFault;
	std::uint32_t aoDetailHistoryIndex = 0;
	bool aoDetailHistoryValid = false;
	std::uint32_t aoDetailHistoryFrame = 0;
	winrt::com_ptr<ID3D11ComputeShader> aoFDownCS, aoFBlurHCS, aoFBlurVCS, aoFUpCS, aoFFullHCS, aoFFullVCS, aoFDetailCS, aoDetailTemporalCS;
	// F7 (the last composite): filter path -1 none, 0 quarter (46s), 1 quarter (46t), 2 full resolution; its switches.
	int aoLastFilter = -1;
	int aoLastDetailRays = -1;
	float aoLastDetailLength = -1.0f;
	int aoLastNormalGuided = -1;
	int aoLastAlphaBudget = -1;
	// (47l) RTAO: Temporal v2. History lengths (R8G8_UNORM: n / 255, large / detail) and our own last-frame geometry (R32_UINT:
	// linear depth f16 | octahedral world normal 8:8), both ping-pong at render size, made on first use (failing that: 47k this
	// session), released with the filter textures or when the switch goes off. Every history resets when the switch changes, at a
	// camera cut (the camera moved more than kAOCutDistance or turned more than 60 degrees in a frame), after a gap of a frame.
	bool EnsureAOTemporalV2();
	void ReleaseAOTemporalV2();
	DenoiseTexture aoV2Length[2], aoV2Geometry[2];
	std::uint32_t aoV2Index = 0;
	std::uint32_t aoV2Width = 0, aoV2Height = 0;
	bool aoV2Valid = false;           // the textures at aoV2Index hold last frame's (written in frame aoV2Frame)
	std::uint32_t aoV2Frame = 0;
	bool aoV2Last = false;            // Temporal v2 as last composited (a change resets every history)
	std::uint64_t aoV2Bytes = 0;
	std::string aoV2Fault;
	winrt::com_ptr<ID3D11ComputeShader> aoTemporalV2CS;
	// (47r) RTAO: Filter Only Where Traced (Settings::AOPostTracedOnly). Per 8x8 tile of the render size: traced or not (RTAO_TILE_MARK),
	// then the Chebyshev distance in tiles to the nearest traced tile (RTAO_TILE_DIST, separable, capped; R8_UINT, 255 = farther); each
	// filter / history pass skips the groups farther than the reach it needs. Made on first use; failing that: 47q this session.
	bool EnsureAOTiles();
	void ReleaseAOTiles();
	DenoiseTexture aoTileMark, aoTileH, aoTileDist;
	std::uint32_t aoTilesWidth = 0, aoTilesHeight = 0;
	std::uint64_t aoTilesBytes = 0;
	std::string aoTilesFault;
	winrt::com_ptr<ID3D11ComputeShader> aoTileMarkCS, aoTileDistHCS, aoTileDistVCS, aoTemporalV2WideCS;
	// (47r) Screen Space Shadows: Tile Mask for RT Casters (Settings::SSSTileMask): per 8x8 tile of the visibility texture, which kinds of
	// pixels it has (DenoiseTileCS SSS_CASTER_TILES; R8_UINT). Null = the per-pixel mask only (47q).
	ID3D11ShaderResourceView* MakeSSSCasterTiles();
	DenoiseTexture sssCasterTiles;
	std::uint32_t sssCasterTilesWidth = 0, sssCasterTilesHeight = 0;
	std::string sssCasterTilesFault;
	winrt::com_ptr<ID3D11ComputeShader> sssCasterTilesCS;
	struct AOPrevCamera
	{
		bool valid = false;
		std::uint32_t frame = 0;
		float viewProj[16]{};
		double camera[3]{};
		float forward[3]{};
	} aoPrevCamera;
	/// The RTAO filter and mix (DrawAmbientOcclusion, inside its try / catch).
	void DrawAmbientOcclusionImpl();
};
