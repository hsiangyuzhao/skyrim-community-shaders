#pragma once

#include "Features/WetSkin.h"  // (47f) WetSkin::PhysicalState / PhysicalLook in ActorWetState

struct Skin : Feature
{
	static Skin* GetSingleton()
	{
		static Skin singleton;
		return &singleton;
	}

	virtual inline std::string GetName() override { return "Advanced Skin"; }
	virtual inline std::string GetShortName() override { return "Skin"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CS_SKIN"; }
	virtual std::string_view GetCategory() const override { return "Characters"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Advanced Skin enhances character skin rendering with multiple techniques.",
			{
				"Physically-based dual specular lobes for realistic skin highlights",
				"Tiled skin detail textures for enhanced realism",
				"Extra textures support for roughness, translucency, and more",
				"Reworked wetness system for dynamic skin effects"
			}
		};
	}
	virtual inline bool HasShaderDefine(RE::BSShader::Type t) override
	{
		return t == RE::BSShader::Type::Lighting;
	};

	virtual inline bool SupportsVR() { return true; }

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void Prepass() override;
	virtual void PostPostLoad() override;

	virtual void SetupResources() override;

	void ReloadSkinDetail();

	struct Settings
	{
		bool EnableSkin = true;
		float SkinMainRoughness = 0.7f;
		float SkinSecondRoughness = 0.35f;
		float SkinSpecularTexMultiplier = 1.0f;
		float SecondarySpecularStrength = 0.15f;
		float F0 = 0.0278f;
		float BaseColorMultiplier = 1.0f;
		float PhysicalMainRoughnessMultiplier = 1.3f;
		float PhysicalSecondRoughnessMultiplier = 0.75f;
		float PhysicalSpecularStrength = 1.0f;
		float ExtraEdgeRoughness = 0.25f;
		bool EnableSkinDetail = true;
		float SkinDetailStrength = 0.25f;
		float SkinDetailTiling = 10.0f;
		float BodyTilingMultiplier = 2.0f;
		float WetFadeTime = 10.0f;
		float StartSweat = 0.75f;
		float FullSweat = 0.15f;
		float Translucency = 0.1f;
		float sssWidth = 0.2f;
		bool UseSSS = true;
		float FuzzStrength = 1.0f;
		float FuzzRoughness = 0.35f;
		float FuzzF0 = 0.045f;
		bool UseDynamicWetness = false;
		// (44a) Extra wetness, the Perlin wetness parameters, the film strength and the 43b
		// coverage options moved to the Wet Skin page (WetSkin.h).
	} settings;

	struct alignas(16) SkinData
	{
		float4 skinParams;
		float4 skinParams2;
		float4 skinDetailParams;
		float4 sssParams;
		float4 fuzzParams;
		float4 physicalParams;
		float4 wetParams;
	};

	struct alignas(16) PerGeometryData
	{
		float4 skinPerGeometry;
		// (44a) Wet Skin: x swim wetness with hold + dry-off (New), y deepest submersion, z in water.
		float4 wetSkinPerGeometry;
		float4 wetSkinParams[6];  // WetSkin::GPUParams
		// (44c) Character Wetness: x s since leaving the water (0 in it), y item class prior of this
		// geometry (0 unknown, 1 cloth, 2 leather, 3 metal, 4 fur), z in water, w 0.
		float4 wetCharPerGeometry;
		float4 wetCharParams[12];  // WetSkin::GPUParams::c
		// (46a) Dynamic Snow, per character: x own snow amount (< 0 = none kept, use the global
		// amount), y amount on the part that was under water, z height of that part above the feet
		// (0 = none), w the feet's world Z.
		float4 snowPerGeometry;
		// (46a) See-through clothes in water: x water surface world Z, y 1 = this draw tests against the
		// pre-water depth (tint below x), z tint strength, w 0.
		float4 waterPerGeometry;
		// (47i) Character Wetness Physical, per character (WetSkin::PhysicalLook; zero in the other models): water amounts,
		// bead parameters and the per-height swim profile (Skin.hlsli wetPhysParams / wetPhysBands).
		float4 wetPhysParams[4];
		float4 wetPhysBands[WetSkin::Phys::kBands];
		// (47k) "Physical: Water Line Follows the Body" (Skin.hlsli bodyPerGeometry / CharBodyHeight): x the pelvis's bone-matrix
		// scale (0 = the world height), y NPC Root's world Z - the actor's, zw the bind-pose shoulder (|x|, z; z 0 = unknown).
		float4 bodyPerGeometry;
		float4 bodyPerGeometry2;  // x the actor's height (sanity range of the body height)
	};

	/// (47k) Water Line Follows the Body: per geometry of a character, what maps its bind-pose (pre-skinning) positions to a height on
	/// the body. Built once per geometry / skin instance (bone names and bind data under SEH), validated every draw.
	struct BodyGeometry
	{
		const void* skin = nullptr;  // the skin instance it was read from (another one rebuilds it)
		bool usable = false;         // skinned, third person, a central bone in its list
		int32_t refBone = -1;        // index in the skin's bone list of its central bone (pelvis / spine / thigh / calf / head)
		int32_t refRank = 0;         // its place in the preference list (from the neck on: the pelvis's world scale is used)
		float refBindZ = 0.0f;       // that bone's bind-pose joint height (model space)
		float stbScale = 1.0f;       // its skin-to-bone scale
		bool shoulderKnown = false;  // "NPC L UpperArm [LUar]" in its list
		float shoulderX = 0.0f;      // its bind-pose joint, |x| and z (model space)
		float shoulderZ = 0.0f;
		uint32_t usedFrame = 0;
	};
	/// (47k) Per character: NPC Root (high heels lift), whether the skeleton is a humanoid's, the shoulder from any of its geometries.
	struct BodyActor
	{
		const void* root = nullptr;  // the 3D (third person) the lookups were made in (compare only)
		uint32_t lookupFrame = UINT32_MAX;
		RE::NiPointer<RE::NiAVObject> npcRoot;
		RE::NiPointer<RE::NiAVObject> pelvis;
		bool humanoid = false;
		float pelvisScale = 0.0f;  // its world scale this frame (0 = unreadable)
		uint32_t frame = UINT32_MAX;  // rootOffset / feetZ / height of this frame
		float rootOffset = 0.0f;
		float feetZ = 0.0f;
		float height = 128.0f;
		bool shoulderKnown = false;
		float shoulderX = 0.0f;
		float shoulderZ = 0.0f;
	};
	/// (47k) Status: geometries drawn with the body height / the world height last frame (Physical, switch on).
	struct BodyLineStats
	{
		uint32_t body = 0;
		uint32_t world = 0;
	};
	BodyLineStats bodyLineStats, bodyLineCounting;
	uint32_t bodyLineStatsFrame = UINT32_MAX;

	/// (46a) See-through (alpha-blended) clothes of characters in water (Character Wetness > See-through
	/// clothes in water). Their draws come after the water, whose depth hid everything below the surface.
	/// At the first water draw after the opaque pass the main depth is copied (GPU copy, our own texture);
	/// a flagged draw gets that copy bound as its depth target right after the engine applied its states
	/// (State::Draw), and the engine's exact binding is put back in Lighting RestoreGeometry, before the
	/// engine issues anything else, so its cached render-target state stays true.
	void OnDrawStatesApplied(ID3D11DeviceContext* a_context);
	void RestoreWaterDepth(ID3D11DeviceContext* a_context);
	void OnEndDeferred();
	/// waterPerGeometry for this geometry, and waterDepthPending set, when it is a flagged draw.
	float4 GetWaterDepthParams(RE::BSGeometry* a_geometry);
	void CopyPreWaterDepth(ID3D11DeviceContext* a_context, ID3D11DepthStencilView* a_boundDSV);
	void ReleaseSavedTargets();
	struct WaterDepthStatus
	{
		uint32_t swapped = 0;   // flagged draws drawn against the pre-water depth (last frame)
		uint32_t skipped = 0;   // flagged draws left alone (unexpected targets)
		const char* note = nullptr;
	} waterDepthStatus;
	bool waterDepthPending = false;  // the current Lighting pass is a flagged draw
	bool waterDepthSwapped = false;  // our depth is bound right now
	std::array<ID3D11RenderTargetView*, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> savedRTVs{};
	ID3D11DepthStencilView* savedDSV = nullptr;
	UINT savedRTVCount = 0;
	winrt::com_ptr<ID3D11Texture2D> preWaterDepth;
	winrt::com_ptr<ID3D11DepthStencilView> preWaterDSV;
	D3D11_TEXTURE2D_DESC preWaterDesc{};
	DXGI_FORMAT preWaterDSVFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t preWaterDepthFrame = UINT32_MAX;  // frame the copy was taken in
	uint32_t endDeferredFrame = UINT32_MAX;
	uint32_t waterStatusFrame = UINT32_MAX;
	WaterDepthStatus waterStatusCounting;

	/// (46a) Per-character snow (Dynamic Snow > Snow builds up per character). Game hours, like the
	/// global amount, so waiting and sleeping count.
	struct ActorSnow
	{
		double hours = -1.0;     // calendar hours at the last update (< 0 = never updated)
		float amount = 0.0f;     // own snow, 0..the global amount
		float washed = 0.0f;     // snow on the part that was under water (builds up again from 0)
		float washDepth = 0.0f;  // height above the feet the water reached (0 = nothing washed)
		// (46d) Roofs stop build-up: an upward Havok ray from the head, throttled (player 0.25 s, others 1 s).
		bool sheltered = false;
		std::int64_t shelterQpc = 0;  // QPC of the last test (0 = never)
	};
	/// (46d) The water at a character: the local surface only (Havok's relevant water height when it has
	/// one, else the flat water object whose bounds hold the character), never the cell's default water
	/// plane (TESObjectREFR::GetWaterHeight falls back to it: in streams and dips below that plane the
	/// whole body counted as under water). Shared by Wet Skin, Character Wetness and the snow.
	struct WaterSample
	{
		float relevant = -1e30f;     // loadedData->relevantWaterHeight (-1e30 = none)
		float cellDefault = -1e30f;  // the cell's default water plane (diagnostics only)
		float multibound = -1e30f;   // the flat water object under the character (-1e30 = none)
		float surfaceZ = -1e30f;     // the surface used (-1e30 = no water)
		float feetZ = 0.0f;
		float depth = 0.0f;     // surface above the feet, clamped to a plausible depth (0 = not in water)
		float rawDepth = 0.0f;  // before the clamp
		bool inWater = false;
		bool swimming = false;
		bool clamped = false;
	};
	static WaterSample SampleWater(RE::Actor* a_actor);
	/// (46a) Advances one character's snow and returns its snowPerGeometry.
	float4 UpdateActorSnow(ActorSnow& a_snow, RE::Actor* a_actor, const WaterSample& a_water);
	/// (46d) Whether something is above the character's head (roof, eaves, overhang), tested at most every
	/// a_intervalMs (the last answer in between).
	static bool IsSheltered(RE::Actor* a_actor, ActorSnow& a_snow, double a_intervalMs);
	/// (46d) A save was loaded / a new game started: every character's snow starts afresh.
	void OnGameLoaded();
	WaterSample playerWater;  // (46d) the player's last sample (Dynamic Snow status line)
	ActorSnow playerSnow;     // the player's, advanced every frame in Prepass
	float4 playerSnowGPU = { -1.0f, 0.0f, 0.0f, 0.0f };

	/// (44b) Per character (form ID), computed once per frame and shared by every geometry of that
	/// character: skin, hair and gear read the same wetness and drying timers.
	struct ActorWetState
	{
		uint32_t frame = 0;       // State::frameCount of the last update
		double clock = 0.0;       // wetClock at the last update
		bool valid = false;
		float timeOut = 1e6f;     // (44a) s since leaving the water (New model)
		float maxDepth = 0.0f;    // (44a) deepest submersion since entering it
		float4 old{};             // the Original model's sweat and swim fade (Advanced Skin WetFadeTime)
		float4 skin{};            // skinPerGeometry
		float4 wetSkin{};         // wetSkinPerGeometry
		float4 wetChar{};         // (44c) wetCharPerGeometry without the per-geometry prior
		ActorSnow snow;           // (46a) this character's own snow (NPCs; the player uses playerSnow)
		float4 snowGPU = { -1.0f, 0.0f, 0.0f, 0.0f };  // (46a) snowPerGeometry
		// (44f) Item class prior per geometry of this character, from the worn items' keywords.
		std::unordered_map<const RE::BSGeometry*, uint8_t> itemClass;
		uint32_t itemClassFrame = 0;
		bool itemClassValid = false;
		bool animal = false;
		// (47f) Character Wetness Physical: this character's water and its look this frame; the shelter test for the rain
		// (its own throttle, apart from the snow's).
		WetSkin::PhysicalState phys;
		WetSkin::PhysicalLook look;
		ActorSnow shelter;
		// (47k) Water Line Follows the Body.
		BodyActor body;
		std::unordered_map<const RE::BSGeometry*, BodyGeometry> bodyGeometry;
	};
	/// (44f) Item class prior (0 unknown, 1 cloth, 2 leather, 3 metal, 4 fur) of one geometry of a
	/// character, from the keywords of the worn item it belongs to (rebuilt on a miss, at most
	/// every 15 frames; animals are fur).
	uint8_t GetItemClassPrior(RE::Actor* a_actor, ActorWetState& a_state, const RE::BSGeometry* a_geometry, uint32_t a_frame);
	/// (47k) bodyPerGeometry / bodyPerGeometry2 for one geometry of a Physical character (x = 0: the world height).
	void GetBodyFrame(RE::BSGeometry* a_geometry, RE::Actor* a_actor, ActorWetState& a_state, float4& o_body, float4& o_body2);
	ActorWetState* lastWetState = nullptr;  // (44c) the state GetWetness last returned (this draw)
	static constexpr size_t kMaxActorWetStates = 1024;
	static constexpr uint32_t kActorWetStaleFrames = 600;
	std::unordered_map<RE::FormID, ActorWetState> actorWetStates;
	double wetClock = 0.0;          // game seconds, advanced once per frame
	uint32_t wetClockFrame = UINT32_MAX;
	// (47c) Skip Invisible Work: WetSkin::GetGPUParams() once per frame (was once per Lighting draw).
	std::array<float4, 6> wetSkinParamsCache{};
	std::array<float4, 12> wetCharParamsCache{};
	uint32_t wetParamsCacheFrame = UINT32_MAX;
	// (47c) Last frame a character's wetness state found it in water (TranslucentUnderWater's depth
	// copy is only needed then).
	uint32_t actorInWaterFrame = UINT32_MAX;
	void PruneActorWetStates(uint32_t a_frame);
	std::array<float, sizeof(PerGeometryData) / sizeof(float)> currentPerGeometry{};  // last uploaded (raw, avoids padding Skin)
	bool perGeometryValid = false;

	std::unique_ptr<ConstantBuffer> PerGeometryCB;
	float4 currentWetness = { 0.0f, 0.0f, 0.0f, 0.0f };
	float playerStamina = 0.0f;
	float playerStaminaMax = 0.0f;

	struct ExtraTextures
	{
		RE::NiSourceTexturePtr rfaosTexture;
		RE::NiSourceTexturePtr wetnessTexture;
		std::string extraTexturePath;
		std::string wetnessTexturePath;
		bool hasExtraTexture = false;
		bool hasWetnessTexture = false;
		/// @brief steady_clock tick of the last draw that read this entry.
		///
		/// (batch 16, item 4) Recency signal for the bound below. Written on the render thread
		/// from BSLightingShader_SetupMaterial, which is the only reader of the map.
		std::int64_t lastUsed = 0;
	};

	/// @brief Upper bound on how many facegen skin materials keep their extra textures resident.
	///
	/// (batch 16, item 4) skinExtraTextures pins two RE::NiSourceTexturePtr per distinct facegen
	/// material -- the RFAOS map and the wetness map -- and nothing ever removed an entry, so on
	/// an NPC-heavy load order it grew for the whole session and the textures it pinned could
	/// never be unloaded by the game.
	///
	/// Evicting here is safe in a way that it would not be for a general texture cache, and the
	/// reason is worth stating: BSLightingShader_SetupMaterial already treats a miss as "build
	/// it now" (it calls SetupExtraTexture and falls back to the default black texture if that
	/// fails). So the map is a cache with a working repopulate path, and the only cost of an
	/// eviction is that the next draw of that material pays the path-derivation and texture load
	/// again. Nothing can be left dangling and nothing can be left visually wrong.
	///
	/// 512 is chosen to be well above the number of distinct facegen materials on screen at once
	/// -- eviction should only ever reach NPCs that are long gone -- while still bounding the
	/// session. Two textures per entry at 512 entries is the ceiling, not the typical case,
	/// because NiSourceTexture deduplicates by path.
	static constexpr size_t maxSkinExtraTextures = 512;

	/// @brief Drop the least recently drawn entries if the map is over maxSkinExtraTextures.
	void PruneSkinExtraTextures();

	eastl::unique_ptr<Texture2D> texSkinDetail = nullptr;
	std::unordered_map<uint32_t, ExtraTextures> skinExtraTextures;

	SkinData GetCommonBufferData();
	/// (46f) The flat water object whose bounds hold the position in XY and whose surface is above the feet by at most
	/// a_maxDepth (the nearest such surface); -1e30 = none. A pool below a bridge or the water of a waterfall basin far
	/// above / below no longer counts.
	static float GetWaterObjectHeight(const RE::NiPoint3& a_pos, float a_maxDepth);
	float4 GetWetness(RE::BSGeometry* geometry, float4& o_wetSkin, float4& o_wetChar);

	void SetupExtraTexture(RE::BSLightingShaderMaterialBase const* material, RE::BSTextureSet* inTextureSet, uint32_t i_hashKey);
	void BSLightingShader_SetupMaterial(RE::BSLightingShaderMaterialBase const* material);
	void BSLightingShader_SetupGeometry(RE::BSRenderPass* a_pass);
	void SetShaderResouces(ID3D11DeviceContext* a_context);

	struct Hooks
	{
		struct BSLightingShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/// (46a) Puts the engine's render targets back after a see-through-clothes-in-water draw.
		struct BSLightingShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			stl::write_vfunc<0x7, BSLightingShader_RestoreGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[Advanced Skin] Installed hooks");
			return;
		}
	};

	bool isDynamicWetnessAvailable = false;
};
