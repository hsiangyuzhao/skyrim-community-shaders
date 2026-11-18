#pragma once

// TODO: consider cam height in cloud intersection

struct CloudShadows : Feature
{
private:
	static constexpr std::string_view MOD_ID = "139185";

public:
	struct alignas(16) Settings
	{
		float Opacity = 0.8f;
		float pad[3];
	};

	Settings settings;

	virtual inline std::string GetName() override { return "Cloud Shadows"; }
	virtual inline std::string GetShortName() override { return "CloudShadows"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual std::string_view GetCategory() const override { return "Sky"; }
	virtual inline std::string_view GetShaderDefineName() override { return "CLOUD_SHADOWS"; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Adds realistic cloud shadows that move across the landscape, creating dynamic lighting changes as clouds pass overhead, enhancing atmospheric immersion.",
			{ "Dynamic cloud shadow projection on terrain and objects",
				"Configurable shadow opacity for artistic control",
				"Real-time shadow movement synchronized with cloud motion",
				"Cubemap-based shadow calculation for accurate projection",
				"Enhanced sky rendering integration" }
		};
	}
	virtual inline bool HasShaderDefine(RE::BSShader::Type) override { return true; }

	bool overrideSky = false;
	void SkyShaderHacks();

	// ---- (48n) Capture from the main view (official CS PR #3166, adapted to this single-cubemap version) ----
	// The game renders the reflection cube (where clouds are captured) only for water with a dynamic cubemap reflection.
	// With none in range nothing was captured: the cloud shadows froze (or stayed empty after a load). On frames the
	// reflection pass did not capture, each main-view cloud layer is redrawn into two faces of the occlusion cube with
	// a CLOUD_SHADOWS_CAPTURE variant of the sky vertex shader (a whole cube every three frames).
	static constexpr uint32_t kCaptureFacesPerFrame = 2;
	static constexpr uint32_t kCaptureFramesPerCube = 6 / kCaptureFacesPerFrame;
	/// The occlusion is bound only while the last capture is at most this many frames old (the reflection pass draws one
	/// face per frame since batch 39, so a whole cube every 6 frames).
	static constexpr uint32_t kCaptureMaxAgeFrames = 6;
	static constexpr UINT kCaptureFaceSlot = 7;  ///< VS cbuffer CloudShadowsCapture in Sky.hlsl
	uint32_t lastCaptureFrame = 0;
	uint32_t lastReflectionsCaptureFrame = UINT32_MAX;
	uint32_t lastViewCaptureFrame = UINT32_MAX;
	uint32_t viewCaptureFrame = UINT32_MAX;
	uint32_t viewCaptureFaces[kCaptureFacesPerFrame] = {};
	uint32_t mapMenuCheckedFrame = UINT32_MAX;
	bool mapMenuOpen = false;
	bool viewCapturePending = false;
	bool viewCaptureShaderMissing = false;
	RE::BSGeometry* captureGeometry = nullptr;
	winrt::com_ptr<ID3D11RasterizerState> captureRasterizerState;
	winrt::com_ptr<ID3D11Buffer> captureFaceCBs[6];
	/** @brief Redraws the current main-view cloud layer into this frame's capture faces, then restores the bound state. State::Draw. */
	void CaptureFromViewIfPending();
	/** @brief Frames since the last capture from either source, or UINT32_MAX before the first. */
	uint32_t FramesSinceCapture() const;

	std::unique_ptr<Texture2D> texCubemapCloudOcc;
	std::unique_ptr<Texture2D> texCubemapCloudOccCopy;

	ID3D11RenderTargetView* cubemapCloudOccRTVs[6] = { nullptr };
	ID3D11RenderTargetView* cubemapCloudOccCopyRTVs[6] = { nullptr };

	ID3D11BlendState* cloudShadowBlendState = nullptr;

	virtual void SetupResources() override;

	virtual void DrawSettings() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void RestoreDefaultSettings() override;

	void CheckResourcesSide(int side);
	void ModifySky(RE::BSRenderPass* Pass);

	virtual void ReflectionsPrepass() override;
	virtual void EarlyPrepass() override;

	virtual inline void PostPostLoad() override { Hooks::Install(); }

	struct Hooks
	{
		struct BSSkyShader_SetupMaterial
		{
			static void thunk(RE::BSShader* This, RE::BSRenderPass* Pass, uint32_t RenderFlags);
			static inline REL::Relocation<decltype(thunk)> func;
		};

		static void Install()
		{
			stl::write_vfunc<0x6, BSSkyShader_SetupMaterial>(RE::VTABLE_BSSkyShader[0]);
			logger::info("[Cloud Shadows] Installed hooks");
		}
	};
	virtual bool SupportsVR() override { return true; };
};
