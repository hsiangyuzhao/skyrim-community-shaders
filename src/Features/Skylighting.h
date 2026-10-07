#pragma once

struct Skylighting : Feature
{
private:
	static constexpr std::string_view MOD_ID = "139352";

public:
	virtual bool SupportsVR() override { return true; };

	virtual inline std::string GetName() override { return "Skylighting"; }
	virtual inline std::string GetShortName() override { return "Skylighting"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "SKYLIGHTING"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Simulates realistic ambient lighting by calculating sky occlusion and directional lighting, providing more accurate and natural illumination in outdoor environments.",
			{ "Sky occlusion calculation for ambient lighting",
				"Directional skylighting based on environment geometry",
				"Enhanced ambient lighting for outdoor scenes",
				"Support for varying sky illumination intensities",
				"Integration with existing lighting systems" }
		};
	}
	virtual bool HasShaderDefine(RE::BSShader::Type) override { return true; };

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void Prepass() override;

	virtual void PostPostLoad() override;

	//////////////////////////////////////////////////////////////////////////////////

	struct Settings
	{
		float MaxZenith = 3.1415926f / 2.f;  // 90 deg
		float MinDiffuseVisibility = 0.1f;
		float MinSpecularVisibility = 0.1f;

		// (batch 38, item 5) Upstream Skylighting fixes, one switch each, every one ANDed with
		// Batch38::IsOn() (see the *Active() helpers). Off = the 37c code path.
		bool FixRoofMarkers = true;          // ca63a41d5: meshes flagged "editor marker" (roof markers) occlude the sky
		bool FixResetClearsProbes = true;    // 5b5361f53 (probe part): Rebuild / load screen resets probes to open sky
		bool FixZenithClamp = true;          // bff82b03e: Max Zenith kept in 0..90 deg and finite
		bool FixZenithRadius = true;         // 4b5b99783: sampling disc radius sin(zenith), not sqrt(sin(zenith))
		bool FixFadeOutGridOffset = true;    // 4b5b99783: edge fade measured from the probe grid's centre
		bool SkipOccludersBelowGrid = true;  // 816888f04: height map skips objects entirely below the probe grid
	} settings;

	bool RoofMarkersActive() const;
	bool ResetClearsProbesActive() const;
	bool FadeOutGridOffsetActive() const;
	bool SkipOccludersBelowGridActive() const;
	/// @brief (batch 38) Max Zenith as used for sampling: clamped to [0, 90 deg] and finite when
	/// FixZenithClamp is active, the raw setting otherwise.
	float EffectiveMaxZenith() const;
	/// @brief (batch 38) Radius of the sky-direction sampling disc for a uniform u in [0,1).
	float SampleDiscRadius(float a_u) const;
	void DrawBatch38Settings();

	struct SkylightingCB
	{
		REX::W32::XMFLOAT4X4 OcclusionViewProj;
		float4 OcclusionDir;

		float3 PosOffset;  // cell origin in camera model space
		float FadeOutUsesGridOffset;  // (batch 38, 4b5b99783) 1 = getFadeOutFactor subtracts PosOffset; 0 = 37c (HLSL PosOffset.w)
		uint ArrayOrigin[3];  // xyz: array origin, w: max accum frames
		uint _pad1;
		int ValidMargin[4];

		float MinDiffuseVisibility;
		float MinSpecularVisibility;
		uint _pad2[2];
	};
	static_assert(sizeof(SkylightingCB) % 16 == 0);

	SkylightingCB GetCommonBufferData(bool a_inWorld);

	winrt::com_ptr<ID3D11SamplerState> comparisonSampler = nullptr;

	std::unique_ptr<Texture2D> texOcclusion;
	std::unique_ptr<Texture3D> texProbeArray;
	std::unique_ptr<Texture3D> texAccumFramesArray;

	winrt::com_ptr<ID3D11ComputeShader> probeUpdateCompute = nullptr;
	winrt::com_ptr<ID3D11ShaderResourceView> stbn_vec3_2Dx1D_128x128x64;

	// misc parameters
	uint probeArrayDims[3] = { 256, 256, 128 };
	float occlusionDistance = 4096.f * 2.5f;  // 5 ugrids
	// (batch 38, 816888f04) Slack below the probe grid for eye movement between the grid update and
	// the occlusion render.
	static constexpr float OCCLUSION_BELOW_GRID_MARGIN = 512.f;

	// cached variables
	bool queuedResetSkylighting = true;
	bool inOcclusion = false;
	// (batch 38, 816888f04) World height of the probe grid's bottom layer, from the snapped grid origin.
	float probeGridBottomZ = -FLT_MAX;
	// (batch 38) Height-map occluders skipped below the grid during the last occlusion render.
	uint occludersSkippedBelowGrid = 0;
	uint occludersSkippedBelowGridLast = 0;
	REX::W32::XMFLOAT4X4 OcclusionTransform;
	float4 OcclusionDir;
	uint frameCount = 0;

	void ResetSkylighting();

	std::chrono::time_point<std::chrono::system_clock> lastUpdateTimer = std::chrono::system_clock::now();

	//////////////////////////////////////////////////////////////////////////////////

	// Hooks
	struct BSLightingShaderProperty_GetPrecipitationOcclusionMapRenderPassesImpl
	{
		static RE::BSLightingShaderProperty::Data* thunk(RE::BSLightingShaderProperty* property, RE::BSGeometry* geometry, uint32_t renderMode, RE::BSGraphics::BSShaderAccumulator* accumulator);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	void RenderOcclusion();

	struct Main_Precipitation_RenderOcclusion
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetViewFrustum
	{
		static void thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetViewFrustumVR
	{
		static void thunk(RE::NiCamera* a_camera, RE::NiFrustum* a_frustum, uint a_eyeIndex);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	// Event handler
	class MenuOpenCloseEventHandler : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		virtual RE::BSEventNotifyControl ProcessEvent(const RE::MenuOpenCloseEvent* a_event, RE::BSTEventSource<RE::MenuOpenCloseEvent>*);

		static bool Register()
		{
			static MenuOpenCloseEventHandler singleton;
			auto ui = globals::game::ui;

			if (!ui) {
				logger::error("UI event source not found");
				return false;
			}

			ui->GetEventSource<RE::MenuOpenCloseEvent>()->AddEventSink(&singleton);

			logger::info("Registered {}", typeid(singleton).name());

			return true;
		}
	};
};
