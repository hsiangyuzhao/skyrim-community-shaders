#pragma once

struct PhysicalSky final : public Feature
{
	////////////////////////////////////////////////// Boilerplate
	static PhysicalSky* GetSingleton()
	{
		static PhysicalSky singleton;
		return &singleton;
	}

	// Metadata
	inline std::string GetName() override { return "Physical Sky"; }
	inline std::string GetShortName() override { return "PhysicalSky"; }
	inline std::string_view GetCategory() const override { return "Sky"; }
	inline std::string GetFeatureModLink() override { return MakeNexusModURL("999999"); }
	inline std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Physically based sky models for photorealistic sky gradients, plus other astronomical effects.",
			{
				"Sky.",
				"Cheese.",
			}
		};
	}

	// Functionality
	bool inline SupportsVR() override { return true; }
	inline std::string_view GetShaderDefineName() override { return "PHYSICAL_SKY"; }
	inline bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	// Settings & UI
	void DataLoaded() override;
	void RestoreDefaultSettings() override;
	void LoadSettings(json& o_json) override;
	void SaveSettings(json& o_json) override;

	void DrawSettings() override;
	void SettingsGeneral();
	void SettingsWorldspaces();
	void SettingsCelestials();
	void SettingsAtmosphere();
	void SettingsClouds();
	void SettingsDebug();

	// Resources
	void SetupResources() override;
	void ClearShaderCache() override;
	void CompileShaders();
	bool ShadersOK();

	// Draw
	void Reset() override;
	void EarlyPrepass() override;
	void ReflectionsPrepass() override;
	void Prepass() override;
	void GenerateLuts();
	void AccumShadow();
	inline void PostPostLoad() override { Hooks::Install(); }

	////////////////////////////////////////////////// Feature Specific Data
	constexpr static uint16_t kTrLutW = 256;
	constexpr static uint16_t kTrLutH = 64;
	constexpr static uint16_t kMsLutW = 32;
	constexpr static uint16_t kMsLutH = 32;
	constexpr static uint16_t kSvLutW = 200;
	constexpr static uint16_t kSvLutH = 150;
	constexpr static uint16_t kApLutW = 32;
	constexpr static uint16_t kApLutH = 32;
	constexpr static uint16_t kApLutD = 32;

	struct WorldspaceInfo
	{
		float zBottom = -14500.f;
	};

	/// The 9 hard-coded worldspaces of 37a and earlier. Used as-is while the 37b master is off.
	static const std::map<std::string, WorldspaceInfo>& LegacyWorldspaceWhitelist();
	/// (batch 37b) Legacy list + the Dawnguard exteriors that are earthly skies:
	/// DLC1HunterHQWorld (Fort Dawnguard), DLC1VampireCastleCourtyard (Castle Volkihar
	/// courtyard), DLC1AncestorsGladeWorld. EDIDs and heights read from Dawnguard.esm.
	static std::map<std::string, WorldspaceInfo> DefaultWorldspaceWhitelist();
	/// (batch 37b) Never physical sky, even if listed: other realms (Soul Cairn, Boneyard,
	/// Apocrypha, Sovngarde) and any worldspace flagged "No Sky" (Blackreach, Darkfall Passage...).
	static bool IsExcludedWorldspace(const RE::TESWorldSpace* a_worldspace);
	/// (batch 37b) Planet ground for worldspaces not in the list: default water height (following
	/// the parent when the worldspace uses the parent's land or water) minus 500, the same offset
	/// Tamriel's -14500 has from its -14000 sea level.
	static float FallbackZBottom(const RE::TESWorldSpace* a_worldspace);

	/// Current exterior worldspace; falls back to the player cell's worldspace (upstream 693f6a35e).
	static RE::TESWorldSpace* GetCurrentWorldspace();

	enum class WorldspaceStatus
	{
		Unknown,
		Interior,
		Whitelist,
		AllExteriors,
		Excluded,
		NotListed
	};
	/// What Reset() decides for the current worldspace, also used by the menu.
	WorldspaceStatus GetWorldspaceStatus(float& a_zBottom) const;

	struct Settings
	{
		bool enabled = true;
		bool overrideDirLight = false;
		int tonemapper = 2;
		float vanillaMix = 0;
		float trMix = 0;
		float apLumMix = 1;
		float apTrMix = 1;

		float2 cloudShadowRemapRange = float2{ 0, 1.f };

		float3 sunlightColor = float3{ 1.0f, 0.97f, 0.95f } * 1e3f;
		float3 masserColor = float3{ 1.0f, 0.6f, 0.6f } * 5e-3f;
		float3 secundaColor = float3{ 0.8f, 1.0f, 1.0f } * 5e-3f;

		bool proceduralSun = true;
		float sunDiskRad = DirectX::XMConvertToRadians(0.53f);  // 37a size (really a diameter)

		// (batch 37b) Procedural sun v2. Each is ANDed with Batch37b::IsOn().
		/// Disk centred where the vanilla sun is drawn (Sky Sync's apparent direction, dipped
		/// with altitude, in the sky root's frame) instead of the raw sun direction. Also moves
		/// the sky's sun glow and the scattering LUTs onto that direction.
		bool sunAlignToVanilla = true;
		/// Disk replaces the vanilla sun texture (upstream 45ad2c6f7 + 735ec68e4): drawn only on
		/// the sun quad, quad pixels outside the disk cleared.
		bool sunReplaceVanilla = true;
		/// Soft disk edge (upstream fec65ed15).
		bool sunSoftEdge = true;
		/// Disk radiance = light colour / disk solid angle x transmittance (upstream 728eedd61 +
		/// d08484aef), capped at sunRadianceCap (upstream: fixed 62250).
		bool sunPhysicalRadiance = true;
		float sunRadianceCap = 1000.f;
		/// Hide the vanilla sun glare (large halo) while the procedural sun is on.
		bool sunHideVanillaGlare = false;
		/// Angular radius in degrees; 0.27 = the real sun (0.53 across).
		float sunDiskRadiusDeg = 0.27f;

		float adaptationStart = DirectX::XMConvertToRadians(-2);
		float adaptationEnd = DirectX::XMConvertToRadians(-15);
		float dayExposure = 1e-2f;
		float nightExposure = 1e2f;

		// (batch 37b) Saved and editable now. Starts as DefaultWorldspaceWhitelist(); entries the
		// user removes from that default set are remembered in worldspaceRemovedDefaults, so a
		// later default addition still reaches old configs without resurrecting removed ones.
		std::map<std::string, WorldspaceInfo> worldspaceWhitelist = DefaultWorldspaceWhitelist();
		std::vector<std::string> worldspaceRemovedDefaults = {};
		/// (batch 37b) Enable in every exterior worldspace, whitelisted or not, except the hard
		/// exclusions (IsExcludedWorldspace). zBottom then comes from FallbackZBottom().
		bool enableAllExteriorWorldspaces = false;
		float3 groundAlbedo = { .2f, .2f, .2f };

		float planetRadius = 6.36e3f;      // in km
		float atmosphereRadius = 6.42e3f;  // in km

		float rayleighFalloff = 1 / 8.69645f;                    // in km^-1
		float3 rayleighScatter = { 6.6049f, 12.345f, 29.413f };  // in megameter^-1
		float aerosolFalloff = 1 / 1.2f;
		float aerosolPhaseG = 0.8f;
		float3 aerosolScatter = { 39.96f, 39.96f, 39.96f };
		float3 aerosolAbsorption = { 4.44f, 4.44f, 4.44f };
		float ozoneAltitude = 22.3499f + 35.66071f * .5f;  // in km
		float ozoneThickness = 35.66071f;
		float3 ozoneAbsorption = { 2.2911f, 1.5404f, 0 };

		float cloudRelightMix = 1.f;
		float cloudOriginalMix = 0.5f;
		float silverLiningMix = 1.f;
		float silverLiningSpread = 0.f;
	} settings;

	struct CbData
	{
		// DYNAMIC
		float2 texDim;
		float2 rcpTexDim;  //
		float2 frameDim;
		float2 rcpFrameDim;  //

		float zCameraPlanet;
		float3 sunDir;  //
		float3 sunlightColor;
		float trMix;  //
		float3 masserDir;
		float apLumMix;  //
		float3 masserColor;
		float apTrMix;  //
		float3 secundaDir;
		float sunDiskCos;  //
		float3 secundaColor;

		// GENERAL
		uint enabled;  //
		int tonemapper;
		float vanillaMix;

		// WORLD
		float zBottom;
		float rPlanet;  //
		float rAtmosphere;
		float3 groundAlbedo;  //

		// ATMOSPHERE
		float2 cloudShadowRemapRange;

		float aerosolFalloff;
		float aerosolPhaseG;  //
		float3 aerosolScatter;
		float _pad5;  //
		float3 aerosolAbsorption;

		float rayleighFalloff;
		float3 rayleighScatter;  //

		float ozoneAltitude;  //
		float ozoneThickness;
		float3 ozoneAbsorption;  //

		// CLOUDS (VANILLA)
		float cloudRelightMix;
		float cloudOriginalMix;
		float silverLiningMix;
		float silverLiningSpread;  //
	} cbData;
	static_assert(sizeof(CbData) % 16 == 0);

	/// (batch 37b) Mirrors SharedData::PhysSkyExtData (HLSL), appended at the end of FeatureData.
	/// Flags are the 37b switches ANDed with the master; all zero = the 37a shader paths.
	enum ExtFlags : uint32_t
	{
		kExtSunReplace = 1u << 0,
		kExtSunSoftEdge = 1u << 1,
		kExtSunPhysicalRadiance = 1u << 2,
		kExtHideSunGlare = 1u << 3,
		kExtSkyAlphaOpaque = 1u << 4,
		kExtTrLutEdgeFix = 1u << 5,
		kExtApShadowDepthFix = 1u << 6,
		kExtReflectionSkyFix = 1u << 7,
		kExtMultiScatterFix = 1u << 8,
	};
	struct ExtCbData
	{
		uint flags = 0;
		float sunRadianceCap = 62250.f;
		float pad0[2] = {};
	} extCbData;
	static_assert(sizeof(ExtCbData) == 16);
	void UpdateExtCbData();

	eastl::unique_ptr<Texture2D> texTrLut = nullptr;  // transmittance
	eastl::unique_ptr<Texture2D> texMsLut = nullptr;  // multiscattering
	eastl::unique_ptr<Texture2D> texSvLut = nullptr;  // sky view
	eastl::unique_ptr<Texture3D> texApLut = nullptr;  // aerial perspective
	eastl::unique_ptr<Texture2D> texApShadow = nullptr;

	winrt::com_ptr<ID3D11SamplerState> sampTr = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampSv = nullptr;
	winrt::com_ptr<ID3D11SamplerState> sampNoise = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> csTrLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csMsLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csSvLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csApLutGen = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> csShadowAccum = nullptr;

	ID3D11SamplerState* originalPSSamplers[2] = { nullptr, nullptr };

	void ModifySky();
	void RestoreSamplers();
	/// (batch 37b) Marks the vanilla sun / sun glare quads for Sky.hlsl (ExtraShaderDescriptors).
	static void SetSunDrawFlags(const RE::BSRenderPass* a_pass);
	static void ClearSunDrawFlags();
	struct Hooks
	{
		struct BSSkyShader_SetupGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSSkyShader_RestoreGeometry
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupGeometry>(RE::VTABLE_BSSkyShader[0]);
			stl::write_vfunc<0x7, BSSkyShader_RestoreGeometry>(RE::VTABLE_BSSkyShader[0]);
		}
	};
};