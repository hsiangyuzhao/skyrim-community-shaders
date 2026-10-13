#pragma once

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
		// (45g) Lower bound on the skin roughness (both lobes, the reflections' G-buffer gloss and
		// the wet film), dry or wet. 0 = 45f.
		float SkinMinRoughness = 0.2f;
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
	};

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
		// (44f) Item class prior per geometry of this character, from the worn items' keywords.
		std::unordered_map<const RE::BSGeometry*, uint8_t> itemClass;
		uint32_t itemClassFrame = 0;
		bool itemClassValid = false;
		bool animal = false;
	};
	/// (44f) Item class prior (0 unknown, 1 cloth, 2 leather, 3 metal, 4 fur) of one geometry of a
	/// character, from the keywords of the worn item it belongs to (rebuilt on a miss, at most
	/// every 15 frames; animals are fur).
	uint8_t GetItemClassPrior(RE::Actor* a_actor, ActorWetState& a_state, const RE::BSGeometry* a_geometry, uint32_t a_frame);
	ActorWetState* lastWetState = nullptr;  // (44c) the state GetWetness last returned (this draw)
	static constexpr size_t kMaxActorWetStates = 1024;
	static constexpr uint32_t kActorWetStaleFrames = 600;
	std::unordered_map<RE::FormID, ActorWetState> actorWetStates;
	double wetClock = 0.0;          // game seconds, advanced once per frame
	uint32_t wetClockFrame = UINT32_MAX;
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
	float GetWaterHeight(const RE::TESObjectREFR* a_ref, const RE::NiPoint3& a_pos);
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

		static void Install()
		{
			stl::write_vfunc<0x6, BSLightingShader_SetupGeometry>(RE::VTABLE_BSLightingShader[0]);
			logger::info("[Advanced Skin] Installed hooks");
			return;
		}
	};

	bool isDynamicWetnessAvailable = false;
};
