#pragma once

#include "Buffer.h"

#include <array>
#include <unordered_map>

/**
 * @brief (batch 39, items 5 and 6) Dynamic snow: accumulation, and footprints / trails.
 *
 * Item 5 - accumulation. Built like Wetness Effects: a global "snow amount" rises while the
 * current weather is snowing and falls after it stops (game time, so waiting and sleeping count).
 * Lighting.hlsl turns it into a per-pixel coverage from how much the surface faces up, a
 * world-space noise, and the Skylighting probes' sky visibility (the same occlusion Wetness
 * Effects uses, so nothing builds up under roofs; interiors are skipped outright). Covered
 * pixels take the snow colour and a rougher, flatter surface before anything reads them, so the
 * G-buffer, SSRT and NRD see an ordinary material change. The material's own snow (landscape
 * snow textures, directional snow projection, snow-flagged meshes) is kept and only the rest of
 * the surface is covered, so nothing is snowed twice.
 *
 * Item 6 - footprints. A top-down trail map around the player (Barré-Brisebois, "Deformable
 * Snow Rendering in Batman: Arkham Origins", GDC 2014): R32_UINT, toroidally addressed so it
 * scrolls with the player without moving data. The feet of the player and nearby actors are
 * stamped into it every frame they touch the ground, and a decay pass fills prints back in over
 * time. Lighting.hlsl reads it on snow (and, optionally, any terrain as mud): the print darkens
 * the snow and bends the normal along the print's slopes, with one parallax step so it reads as
 * a dent. Skyrim's terrain is not tessellated, so the depth is shading only.
 *
 * Batch 39c:
 * - Coverage comes from the geometry slope on a soft ramp (SlopeStart..SlopeFull); the normal
 *   map only shapes partial cover, and full cover flattens the normal and fades the parallax
 *   self-shadow. 39b gated half on the normal map against a hard 0.6 threshold, which thatch
 *   roofs (vertex normals 0.45..0.85) straddled: thin, streaky snow.
 * - Vegetation: animated trees and foliage (Lighting TREE_ANIM), grass (RunGrass) and distant
 *   billboard trees (DistantTree), each with its own switch and strength.
 * - Footprints: shaped prints from the installed footprint mods' height maps (loaded from the
 *   game's data at first use, never shipped), oriented by the foot, with a rim; the trail map
 *   stores signed height (dent / rim). (44h) The Footprints mod is no longer deferred to.
 *   Ragdolls and loose objects carve trenches from their Havok
 *   shapes (after community-shaders PR #2659).

 */
struct DynamicSnow : Feature
{
public:
	virtual inline std::string GetName() override { return "Dynamic Snow"; }
	virtual inline std::string GetShortName() override { return "DynamicSnow"; }
	virtual inline std::string_view GetShaderDefineName() override { return "DYNAMIC_SNOW"; }
	virtual std::string_view GetCategory() const override { return "Landscape & Textures"; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Snow builds up on the ground, roofs and rocks while it snows and melts away afterwards, "
			"and people and creatures leave footprints in snow.",
			{ "Snow builds up on upward-facing surfaces while it snows, not under roofs",
				"Melts gradually after the snow stops",
				"Snow on trees, bushes and grass",
				"Only in cold regions (from the regions' weather), never on distant LOD",
				"Footprints and trails from the player and nearby actors, filling back in over time",
				"Optional prints in mud and dirt" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	/// Which weather builds snow up. Weather mods that make it snow outside the snowy regions
	/// are the reason this is a choice.
	enum class SnowCondition : int
	{
		SnowWeather = 0,          ///< any weather with the Snow flag, anywhere (default)
		SnowWeatherColdRegion,    ///< Snow flag AND the region is a cold one
		SnowOrColdPrecipitation,  ///< Snow flag, OR any precipitation in a cold region
		Count
	};

	struct Settings
	{
		// ---- Item 5: accumulation ----
		bool EnableAccumulation = true;
		int Condition = static_cast<int>(SnowCondition::SnowWeather);
		/// Share of snow weathers (by chance) in the region's weather list from which a region
		/// counts as cold.
		float ColdRegionSnowShare = 0.25f;
		/// Game hours of steady snowfall from bare to fully built up.
		float AccumulationHours = 1.5f;
		/// Game hours from fully built up to bare after the snow stops.
		float MeltHours = 6.0f;
		float MaxCoverage = 1.0f;
		/// World-up . normal from which a surface starts to hold snow (0.6 = about 53 degrees).
		float NormalThreshold = 0.6f;
		float3 SnowColor = { 0.86f, 0.88f, 0.92f };
		float SnowRoughness = 0.75f;

		// ---- Item 6: footprints ----
		bool EnableTrails = true;
		bool TrailsOnSnow = true;         ///< landscape snow textures and snow materials
		bool TrailsOnAccumulated = true;  ///< item 5's accumulated snow
		bool MudTrails = false;           ///< any other terrain, as mud/dirt
		bool TrailsFromNPCs = true;       ///< nearby actors and creatures, not only the player
		int TrailResolution = 1;          ///< 0 = 1024 texels (4 units each), 1 = 2048 (2 units each), 2 = 4096 (1 unit each)
		// (44f) Permanent: bicubic print reconstruction (39b), snowy ground recognised from its land
		// texture / white directional snow (39b), shaped prints from installed footprint textures
		// (39c, ovals when none are found), rigid gear taking its slope from world up (40e). Removed:
		// "Also Guess From Colour" and the "Turn Footprint Shapes Around" debug option. Old keys
		// SmoothTrails / DetectAuthoredSnow / AlbedoSnowGuess / UseModFootprintShapes /
		// RigidGearWorldUp / FlipPrintShapes are ignored.
		bool SnowOnCharacters = false;    ///< (39b) accumulated snow on actors and their gear
		float TrailSize = 1.0f;
		float TrailDepth = 5.0f;            ///< game units
		float TrailRefillSeconds = 180.0f;  ///< real seconds for a full print to fill back in
		float TrailDarken = 0.25f;
		float MudStrength = 0.6f;

		// ---- (batch 39c) ----
		// (44h) Removed after the user's review, now fixed; their old keys are ignored: SlopeCoverage
		// ("Even Cover on Roofs and Slopes", off: coverage is the 39b NormalThreshold rule) with
		// SlopeStart / SlopeFull / SteepCover / SnowSmoothing, the one-time "EvenCoverOn44f" migration
		// marker, YieldToFootprintsMod (off: our prints everywhere, Footprints.esp no longer looked up),
		// TrailTimeStamps (on) and TrailPauseWhenNoSnow (on).
		bool SnowOnTrees = true;   ///< animated trees, bushes, ferns (Lighting TREE_ANIM)
		float TreeCoverage = 0.8f;
		bool SnowOnGrass = true;
		float GrassCoverage = 0.9f;
		// (44h) "Snow on Distant Trees" / "Distant Tree Snow Amount" removed: distant LOD takes no
		// accumulation under NoLodSnow, and the old LOD-tree tint (0.5) with it off. Old keys
		// SnowOnLodTrees / LodTreeCoverage are ignored.
		/// (44h) Accumulation only where the region is cold (the same snow share as Cold Region
		/// Threshold, mapped over the whole worldspace). Off = the global amount everywhere.
		bool ColdRegionsOnly = true;
		/// (44h) LOD terrain, LOD objects and distant trees take no accumulation (their textures carry
		/// snow already); near geometry thins out to none at the edge of the loaded cells.
		bool NoLodSnow = true;
		/// Ragdolls being dragged or sliding and loose objects moving through snow carve trenches
		/// (Havok collision shapes, after community-shaders PR #2659).
		bool BodyAndObjectTrails = true;
		float TrailRim = 0.35f;  ///< pushed-up rim around a print, relative to its depth

		// ---- Debug ----
		bool OverrideAmount = false;
		bool ShowColdMap = false;  ///< (44h) tint surfaces red (no snow) / blue (full snow)
		float AmountOverride = 1.0f;
	};

	Settings settings;

	/// Mirrors SharedData::DynamicSnowSettings (HLSL), appended to FeatureData.
	struct alignas(16) CommonBufferData
	{
		uint Flags;
		float Amount;
		float MaxCoverage;
		float NormalThreshold;

		float3 SnowColor;
		float SnowRoughness;

		float2 TrailOrigin;
		int TrailWrapX;
		int TrailWrapY;

		uint TrailMapSize;
		float TrailTexelSize;
		float TrailDepth;
		float TrailDarken;

		float TrailZTolerance;
		float TrailMudStrength;
		float TrailEdgeFade;
		float pad0;

		// (batch 39c)
		float SlopeStart;
		float SlopeFull;
		float TreeCoverage;
		float GrassCoverage;

		float LodTreeCoverage;
		float TrailRim;
		// (40e)
		float SnowSmoothing;
		float SteepCover;
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBufferData);
	static_assert(sizeof(CommonBufferData) == 112);

	/// Mirrors DynamicSnow::Flag* in DynamicSnow.hlsli.
	enum Flag : uint
	{
		FlagAccumulation = 1 << 0,
		FlagTrails = 1 << 1,
		FlagTrailsOnSnow = 1 << 2,
		FlagTrailsOnAccumulated = 1 << 3,
		FlagMudTrails = 1 << 4,
		// 1 << 5, 6, 7: (39b) smooth prints, snowy-ground recognition, colour guess; free since 44f
		// (the first two are permanent in the shader, the third was removed).
		FlagSnowOnCharacters = 1 << 8,
		// (batch 39c)
		FlagCoverageSlope = 1 << 9,
		FlagTrees = 1 << 10,
		FlagGrass = 1 << 11,
		FlagLodTrees = 1 << 12,
		// 1 << 13: (40e) rigid gear uses world up; free since 44f (permanent in the shader).
		// (44e)
		FlagTrailTimeStamps = 1 << 14,
		// (44h)
		FlagColdMap = 1 << 15,
		FlagNoLodSnow = 1 << 16,
		FlagShowColdMap = 1 << 17,
	};

	[[nodiscard]] CommonBufferData GetCommonBufferData();

	/// (44h) Mirrors SharedData::DynamicSnowMapData (HLSL), appended at the very end of FeatureData.
	struct alignas(16) MapCbData
	{
		float2 MaskOrigin;
		float MaskInvTexelSize;
		float pad0;

		uint MaskSizeX;
		uint MaskSizeY;
		float2 LoadedCenter;

		float FadeStart;
		float FadeEnd;
		float2 pad1;
	};
	STATIC_ASSERT_ALIGNAS_16(MapCbData);
	static_assert(sizeof(MapCbData) == 48);

	/// (44h) Cold-region mask placement and the near-snow fade. Updates the frame state itself, so it
	/// does not depend on the order FeatureBuffer evaluates its arguments in.
	[[nodiscard]] MapCbData GetMapBufferData();

	/// Accumulation is wanted this frame (master && setting).
	[[nodiscard]] bool AccumulationActive() const;
	/// Trails are wanted this frame (master && setting).
	[[nodiscard]] bool TrailsActive() const;

	virtual void Prepass() override;
	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	static constexpr uint32_t kTrailSlot = 101;          ///< PS register of the trail map (t101)
	static constexpr float kTrailWindowUnits = 4096.0f;  ///< trail window side, game units (58.5 m)
	static constexpr float kTrailZWrap = 1024.0f;        ///< mirrors DynamicSnow::kTrailZWrap (HLSL)
	static constexpr uint32_t kMaxStamps = 512;  ///< (39c) 256 -> 512: body and object trenches too

	// ---- Diagnostics (menu, Batch 39 table) ----
	struct Status
	{
		bool exterior = false;
		bool snowWeather = false;    ///< current or fading weather carries the Snow flag
		bool precipitation = false;  ///< current or fading weather has any precipitation
		bool coldRegion = false;
		bool snowing = false;            ///< the Condition says snow is falling now
		float snowfall = 0.0f;           ///< 0..1 strength of the snowfall feeding accumulation
		float amount = 0.0f;             ///< 0..1 accumulated
		bool accumulationDrawn = false;  ///< Flags carried FlagAccumulation last frame
		bool trailsDrawn = false;        ///< Flags carried FlagTrails last frame
		uint32_t stamps = 0;             ///< prints stamped last frame
		uint32_t actorsTracked = 0;      ///< actors whose feet were checked last frame
		uint32_t trailMapSize = 0;
		const char* trailUpkeep = "-";  ///< (44e) how the trail map is kept this frame
		std::string region;  ///< name / EditorID of the region that decided "cold"
		// (batch 39c)
		uint32_t shapesLoaded = 0;       ///< print shapes found in the installed footprint mods
		std::string shapeSource;         ///< where they came from
		uint32_t bodyStamps = 0;         ///< trench stamps from bodies and objects last frame
		// (44h) Cold-region map
		bool coldMapValid = false;       ///< a mask is built and bound for this worldspace
		std::string coldMapNote;         ///< why there is none, or where it came from
		uint32_t coldMapRegions = 0;     ///< weather regions rasterised
		uint32_t coldMapColdRegions = 0; ///< ... of which cold at the current threshold
		float coldMapColdPercent = 0.0f; ///< share of the map's texels that are cold (before the blur)
		uint32_t coldMapWidth = 0;
		uint32_t coldMapHeight = 0;
		float coldMapBuildMs = 0.0f;
		float playerMask = 1.0f;         ///< mask value at the player
		uint32_t gridsToLoad = 0;        ///< loaded cells per side (uGridsToLoad)
		float fadeStart = 0.0f;          ///< near-snow fade, game units from the loaded area's centre
		float fadeEnd = 0.0f;
	} status;

private:
	/// Mirrors Stamp in SnowTrailsCS.hlsl.
	struct Stamp
	{
		float2 Center;  ///< window texel coordinates (capsule: one end)
		float2 Axis;    ///< unit vector along the print's length, towards the toe
		float2 Radii;   ///< ellipse: semi-axes; shape: half extents (length, width); capsule: radius
		uint Z16;       ///< foot height modulo kTrailZWrap, UNORM16
		float Strength;
		float2 End;     ///< capsule: the other end, window texels
		uint Shape;     ///< kShapeEllipse, kShapeCapsule or a shape-atlas slice (| kShapeMirror)
		float Rim;      ///< ellipse / capsule: rim height relative to the depth
	};
	static_assert(sizeof(Stamp) == 48);
	static constexpr uint kShapeEllipse = 0xFFFF;
	static constexpr uint kShapeCapsule = 0xFFFE;
	static constexpr uint kShapeMirror = 0x10000;

	struct alignas(16) TrailCB
	{
		int RectMin[2];
		int RectSize[2];
		int Wrap[2];
		uint MapSize;
		uint DecayStep;
		uint StampCount;
		uint ShapeCount;
		uint pad[2];
	};
	STATIC_ASSERT_ALIGNAS_16(TrailCB);

	/// (batch 39c) Footprint shapes from the installed footprint mods. One kind per animal; its
	/// left/right (and front/back) prints are separate atlas slices.
	enum class PrintKind : int
	{
		None = -1,  ///< procedural oval
		Human = 0,
		Horse,
		Canine,
		Bear,
		Sabrecat,
		Deer,
		Cow,
		Giant,
		Mammoth,
		Troll,
		Werewolf,
		Skeever,
		Count
	};
	struct PrintKindInfo
	{
		const char* name;           ///< for the menu
		const char* files[4];       ///< texture base names: left, right, back-left, back-right (nullptr = reuse)
		float decalUnits;           ///< the Footprints mod's decal size for this animal (TXST DODT), game units
		const char* raceKeys;       ///< '|'-separated lowercase race EditorID fragments
	};
	static const PrintKindInfo kPrintKinds[static_cast<int>(PrintKind::Count)];
	struct ShapeSlots
	{
		int slot[4] = { -1, -1, -1, -1 };  ///< atlas slice per file, -1 = not found
	};

	/// Per-actor foot tracking. Feet nodes are found once per loaded 3D.
	struct ActorFeet
	{
		RE::NiPointer<RE::NiAVObject> root;
		std::array<RE::NiPointer<RE::NiAVObject>, 4> feet;
		std::array<RE::NiPointer<RE::NiAVObject>, 2> toes;  ///< (39c) humanoid toe nodes, for the foot's yaw
		std::array<float, 4> restHeight{};  ///< lowest foot height above the actor's position seen
		std::array<bool, 4> planted{};      ///< (39c) foot was down last frame
		std::array<float2, 4> plantedDir{};  ///< (39c) foot yaw latched when it came down
		uint32_t count = 0;
		bool humanoid = false;
		PrintKind kind = PrintKind::None;  ///< (39c) which installed print shape to use
		float sizeFactor = 1.0f;           ///< (39c) smaller animals sharing a texture (fox, goat)
		// Stride fallback (no feet found, or the player in first person)
		RE::NiPoint3 lastStride{};
		bool strideValid = false;
		bool strideLeft = false;
		uint64_t lastSeenFrame = 0;
	};

	void UpdateFrameState();
	void UpdateAccumulation();
	bool IsColdRegion(RE::TESObjectCELL* a_cell, RE::TESWorldSpace* a_worldSpace);
	void GatherStamps(const RE::NiPoint3& a_center);
	void FindFeet(RE::Actor* a_actor, ActorFeet& a_feet);
	void AddStamp(float a_x, float a_y, float a_z, float a_dirX, float a_dirY, float a_lengthUnits, float a_widthUnits, uint a_shape = kShapeEllipse);
	void AddCapsule(const RE::NiPoint3& a_from, const RE::NiPoint3& a_to, float a_radius);
	void GatherBodyAndObjectStamps(const RE::NiPoint3& a_center, std::vector<RE::Actor*>& a_deadActors);
	static PrintKind ClassifyRace(RE::Actor* a_actor, bool a_humanoid);
	void LoadPrintShapes();
	void UpdateTrailMap();
	bool EnsureTrailResources();
	void ReleaseTrailResources();
	void CompileShaders();

	Util::FrameChecker frameChecker;
	uint64_t frameIndex = 0;

	// Accumulation state
	float amount = 0.0f;
	double lastGameHours = -1.0;
	bool amountInitialised = false;
	RE::TESObjectCELL* coldCacheCell = nullptr;
	float coldCacheShare = -1.0f;  ///< snow share of coldCacheCell's regions (-1 = no weather list)

	// Trail window state
	bool trailsWanted = false;  ///< this frame's decision, made in UpdateFrameState
	uint32_t trailMapSize = 0;  ///< size of the live texture (0 = none)
	float trailTexelSize = 2.0f;
	int64_t windowOrigin[2]{};       ///< absolute texel index of the window's corner, this frame
	int64_t committedOrigin[2]{};    ///< origin the texture content was last scrolled to
	bool trailContentValid = false;  ///< false = clear the whole map before use
	RE::TESWorldSpace* trailWorldSpace = nullptr;
	float decayAccumulator = 0.0f;  ///< strength units waiting to be removed
	std::vector<Stamp> stamps;
	std::unordered_map<RE::FormID, ActorFeet> actorFeet;

	// (batch 39c) Print shapes and the Footprints mod
	bool shapesTried = false;  ///< LoadPrintShapes ran (it runs once, in game, on first use)
	std::array<ShapeSlots, static_cast<size_t>(PrintKind::Count)> shapeSlots{};
	uint32_t shapeCount = 0;
	winrt::com_ptr<ID3D11ShaderResourceView> shapeSRV;
	winrt::com_ptr<ID3D11SamplerState> shapeSampler;
	// (batch 39c) Body and object trails: last position per collision shape, key = formID << 16 | shape
	std::unordered_map<uint64_t, RE::NiPoint3> bodyPrevPositions;
	std::unordered_map<uint64_t, RE::NiPoint3> bodyCurPositions;

	std::unique_ptr<ConstantBuffer> trailCB;
	eastl::unique_ptr<Buffer> stampBuffer;
	winrt::com_ptr<ID3D11Texture2D> trailTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trailSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> trailUAV;

	ID3D11ComputeShader* clearCS = nullptr;
	ID3D11ComputeShader* decayCS = nullptr;
	ID3D11ComputeShader* stampCS = nullptr;
	// (44e) Time-stamped trail map.
	ID3D11ComputeShader* stampTimeCS = nullptr;
	ID3D11ComputeShader* sweepCS = nullptr;
	static constexpr uint32_t kTrailTicksPerRefill = 256;  ///< mirrors TrailTime::kTicksPerRefill
	double trailClock = 0.0;              ///< game-running seconds (the refill clock)
	float trailClockRefill = 0.0f;        ///< TrailRefillSeconds the ticks were defined with
	bool trailContentTimeStamped = false; ///< the map content uses the time-stamped encoding
	uint32_t sweepRow = 0;
	bool trailMaintenancePaused = false;
	double trailPausedAt = 0.0;
	bool snowNearby = true;
	uint64_t snowNearbyFrame = 0;
	uint32_t NowTick() const;
	bool SnowNearby();

	// (44h) Cold-region map. The share grid (highest snow share of the weather regions covering each
	// texel, the climate's share where none does) is built once per worldspace; the mask (share >=
	// threshold, blurred) is rebuilt from it whenever the threshold changes.
	static constexpr uint32_t kColdMaskSlot = 102;    ///< PS register of the mask (t102)
	static constexpr float kColdMapTexelUnits = 2048.0f;  ///< two texels per 4096-unit cell
	static constexpr uint32_t kColdMapMaxSize = 512;
	void UpdateColdMap();
	void BuildColdShareGrid(RE::TESWorldSpace* a_worldSpace);
	void BuildColdMask();
	void ReleaseColdMap();
	[[nodiscard]] float SampleColdMaskCPU(float a_x, float a_y) const;
	void UpdateLoadedArea();

	RE::TESWorldSpace* coldMapWorldSpace = nullptr;  ///< worldspace the share grid was built for
	bool coldMapTried = false;                       ///< BuildColdShareGrid ran for coldMapWorldSpace
	float coldMapThreshold = -1.0f;                  ///< threshold the mask was built with
	std::vector<float> coldShare;                    ///< per texel snow share
	std::vector<float> coldShareRegion;              ///< per weather region (for the cold count)
	std::vector<float> coldMaskCPU;                  ///< the uploaded mask, for the status line
	float coldClimateShare = -1.0f;                  ///< share where no weather region lies (-1 = none)
	float coldShareBuildMs = 0.0f;
	bool coldMapLogPending = false;                  ///< log the next mask build in full
	uint32_t coldMapW = 0;
	uint32_t coldMapH = 0;
	float coldMapTexel = kColdMapTexelUnits;
	float coldMapOriginX = 0.0f;
	float coldMapOriginY = 0.0f;
	winrt::com_ptr<ID3D11Texture2D> coldMaskTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> coldMaskSRV;
	float loadedCenter[2]{};
	float loadedFadeStart = 1e9f;
	float loadedFadeEnd = 2e9f;
};
