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
 * Every switch is ANDed with Batch39::IsOn(); with the master off nothing is built or bound and
 * Lighting.hlsl skips the whole block (Flags == 0): the 38b path.
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
			"and people and creatures leave footprints in snow (Batch 39).",
			{ "Snow builds up on upward-facing surfaces while it snows, not under roofs",
				"Melts gradually after the snow stops",
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
		bool SmoothTrails = true;         ///< (39b) bicubic reconstruction; off = 39a's bilinear (mosaic)
		bool DetectAuthoredSnow = true;   ///< (39b) snow ground recognised from its land texture (PBR terrain too) and white directional snow
		bool AlbedoSnowGuess = false;     ///< (39b) also bright grey-white terrain (last resort)
		bool SnowOnCharacters = false;    ///< (39b) accumulated snow on actors and their gear
		float TrailSize = 1.0f;
		float TrailDepth = 5.0f;            ///< game units
		float TrailRefillSeconds = 180.0f;  ///< real seconds for a full print to fill back in
		float TrailDarken = 0.25f;
		float MudStrength = 0.6f;

		// ---- Debug ----
		bool OverrideAmount = false;
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
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBufferData);
	static_assert(sizeof(CommonBufferData) == 80);

	/// Mirrors DynamicSnow::Flag* in DynamicSnow.hlsli.
	enum Flag : uint
	{
		FlagAccumulation = 1 << 0,
		FlagTrails = 1 << 1,
		FlagTrailsOnSnow = 1 << 2,
		FlagTrailsOnAccumulated = 1 << 3,
		FlagMudTrails = 1 << 4,
		FlagSmoothTrails = 1 << 5,
		FlagLandSnowDetect = 1 << 6,
		FlagAlbedoSnowGuess = 1 << 7,
		FlagSnowOnCharacters = 1 << 8,
	};

	[[nodiscard]] CommonBufferData GetCommonBufferData();

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
	static constexpr uint32_t kMaxStamps = 256;

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
		std::string region;  ///< name / EditorID of the region that decided "cold"
	} status;

private:
	struct Stamp
	{
		float2 Center;  ///< window texel coordinates
		float2 Axis;    ///< unit vector along the print's length
		float2 Radii;   ///< semi-axes in texels
		uint Z16;       ///< foot height modulo kTrailZWrap, UNORM16
		float Strength;
	};
	static_assert(sizeof(Stamp) == 32);

	struct alignas(16) TrailCB
	{
		int RectMin[2];
		int RectSize[2];
		int Wrap[2];
		uint MapSize;
		uint DecayStep;
		uint StampCount;
		uint pad[3];
	};
	STATIC_ASSERT_ALIGNAS_16(TrailCB);

	/// Per-actor foot tracking. Feet nodes are found once per loaded 3D.
	struct ActorFeet
	{
		RE::NiPointer<RE::NiAVObject> root;
		std::array<RE::NiPointer<RE::NiAVObject>, 4> feet;
		std::array<float, 4> restHeight{};  ///< lowest foot height above the actor's position seen
		uint32_t count = 0;
		bool humanoid = false;
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
	void AddStamp(float a_x, float a_y, float a_z, float a_dirX, float a_dirY, float a_lengthUnits, float a_widthUnits);
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

	std::unique_ptr<ConstantBuffer> trailCB;
	eastl::unique_ptr<Buffer> stampBuffer;
	winrt::com_ptr<ID3D11Texture2D> trailTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> trailSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> trailUAV;

	ID3D11ComputeShader* clearCS = nullptr;
	ID3D11ComputeShader* decayCS = nullptr;
	ID3D11ComputeShader* stampCS = nullptr;
};
