#pragma once

#include "Buffer.h"

struct ScreenSpaceShadows : Feature
{
private:
	static constexpr std::string_view MOD_ID = "93209";

public:
	virtual inline std::string GetName() override { return "Screen Space Shadows"; }
	virtual inline std::string GetShortName() override { return "ScreenSpaceShadows"; }
	virtual inline std::string GetFeatureModLink() override { return MakeNexusModURL(MOD_ID); }
	virtual inline std::string_view GetShaderDefineName() override { return "SCREEN_SPACE_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Screen Space Shadows enhances shadow quality by adding detailed contact shadows and improving shadow accuracy.\n"
			"This technique adds fine-detail shadows that traditional shadow mapping might miss.",
			{ "Enhanced contact shadows",
				"Improved shadow detail",
				"Better shadow accuracy",
				"Fine-scale shadow effects",
				"Configurable shadow contrast" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct BendSettings
	{
		// (45 SSS) Code defaults 0.02 -> 0.01 and 1.0 -> 2.5 (Bend recommends contrast 2-4); saved values are kept.
		float SurfaceThickness = 0.01f;
		float BilinearThreshold = 0.02f;
		float ShadowContrast = 2.5f;
		uint Enable = 1;
		uint SampleCount = 1;
		// (47q) World-Space Thickness, Min Thickness and Contact Length are gone (the original Bend math only): three unused slots keep
		// the constant buffer's layout (RaymarchCB.hlsli Unused1-2; (47r) the first is RtTileMask).
		uint RtTileMask = 0;  ///< (47r) set per frame by Ray Tracing, not saved: t2 holds the per-tile summary of the caster mask (RaymarchCB.hlsli)
		float Unused1 = 0.0f;
		float Unused2 = 0.0f;
		uint EdgeSmoothing = 1;       ///< (45 SSS) 3x3 depth-aware blur of the mask, per frame only; (45o) always on, not saved
		// (45k) Set per frame by Ray Tracing ("non-RT casters only"), not saved: contact range of
		// ray-traced occluders in multiples of the ray tracer's start offset (constant + per unit).
		float RtContactScale = 0.0f;
		float RtBiasConstant = 0.0f;
		float RtBiasPerUnit = 0.0f;
	};
	static_assert(sizeof(BendSettings) == 48, "BendSettings must match ScreenSpaceShadows/RaymarchCB.hlsli");

	BendSettings bendSettings;

	struct alignas(16) RaymarchCB
	{
		// Runtime data returned from BuildDispatchList():
		float LightCoordinate[4];  // Values stored in DispatchList::LightCoordinate_Shader by BuildDispatchList()
		int WaveOffset[2];         // Values stored in DispatchData::WaveOffset_Shader by BuildDispatchList()

		// Renderer Specific Values:
		float FarDepthValue;   // Set to the Depth Buffer Value for the far clip plane, as determined by renderer projection matrix setup (typically 0).
		float NearDepthValue;  // Set to the Depth Buffer Value for the near clip plane, as determined by renderer projection matrix setup (typically 1).

		// Sampling data:
		float InvDepthTextureSize[2];  // Inverse of the texture dimensions for 'DepthTexture' (used to convert from pixel coordinates to UVs)
									   // If 'PointBorderSampler' is an Unnormalized sampler, then this value can be hard-coded to 1.
									   // The 'USE_HALF_PIXEL_OFFSET' macro might need to be defined if sampling at exact pixel coordinates isn't precise (e.g., if odd patterns appear in the shadow).

		float2 DynamicRes;

		// (45 SSS) DynamicSampleCount / DynamicReadCount removed: SAMPLE_COUNT is compiled from the
		// render size, so the shader no longer needs a runtime loop limit.

		BendSettings settings;

		// (45 SSS) Edge Smoothing data; (45k) the RT_CASTER_MASK contact range uses CameraData / SunDirVS / ProjInfo too
		float CameraData[4];  // far, near, far - near, far * near (Util::GetCameraData)
		float SunDirVS[4];    // towards the sun, view space
		float ProjInfo[4];    // 1 / P00, 1 / P11, render width, render height
	};
	STATIC_ASSERT_ALIGNAS_16(RaymarchCB);
	static_assert(offsetof(RaymarchCB, settings) == 48, "RaymarchCB must match ScreenSpaceShadows/RaymarchCB.hlsli");
	static_assert(offsetof(RaymarchCB, CameraData) == 96, "RaymarchCB must match ScreenSpaceShadows/RaymarchCB.hlsli");

	ID3D11SamplerState* pointBorderSampler = nullptr;

	std::unique_ptr<ConstantBuffer> raymarchCB;
	ID3D11ComputeShader* raymarchCS = nullptr;
	ID3D11ComputeShader* raymarchRightCS = nullptr;
	ID3D11ComputeShader* edgeSmoothCS = nullptr;  ///< (45 SSS) Edge Smoothing
	ID3D11ComputeShader* raymarchMaskCS = nullptr;  ///< (45k) RT_CASTER_MASK variant (Ray Tracing "non-RT casters only")

	std::unique_ptr<Texture2D> screenSpaceShadowsTexture;
	/// (45 SSS) Ray march output before Edge Smoothing (same format as screenSpaceShadowsTexture).
	std::unique_ptr<Texture2D> rawShadowsTexture;

	virtual void SetupResources() override;

	virtual void DrawSettings() override;

	virtual void ClearShaderCache() override;
	void ReleaseRaymarchShaders();
	/** @brief Ray sample count for the current render (dynamic) resolution, quantized to steps of 8. */
	uint GetScaledSampleCount();
	/// SAMPLE_COUNT the raymarch shaders were last compiled with (0 = none yet).
	uint lastCompiledSampleCount = 0;
	/** @brief (Re)compiles the raymarch shaders when the render-size sample count changed. */
	void CompileRaymarchShaders();
	ID3D11ComputeShader* GetComputeRaymarch();
	ID3D11ComputeShader* GetComputeRaymarchRight();
	ID3D11ComputeShader* GetComputeEdgeSmooth();

	virtual void Prepass() override;

	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	/// (45k) a_rtMask: Ray Tracing's visibility texture (alpha bit 4 = ray-traced caster); null = the normal ray march.
	void DrawShadows(ID3D11ShaderResourceView* a_rtMask = nullptr, ID3D11ShaderResourceView* a_rtTiles = nullptr);
	/// (45k) Ray Tracing draws the shadows itself this frame (after its trace, with the caster mask).
	/// Set in Prepass when Ray Tracing claims them; DrawDeferred must then be called once.
	bool deferredThisFrame = false;
	/// (47r) a_rtTiles: the per-8x8-tile summary of a_rtMask (bit 0 some traced caster, bit 1 some other pixel), or null.
	void DrawDeferred(ID3D11ShaderResourceView* a_rtMask, float a_contactScale, float a_biasConstant, float a_biasPerUnit, ID3D11ShaderResourceView* a_rtTiles = nullptr);

	virtual void RestoreDefaultSettings() override;

	virtual bool SupportsVR() override { return false; };
};
