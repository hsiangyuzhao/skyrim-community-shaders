#pragma once

#include "PostProcessFeature.h"

#include "Buffer.h"

/**
 * @brief (batch 38, item 4) Local Exposure. Port of upstream PostProcessing/LocalExposure
 * (86ae0fb3c .. b4eb15b70, final form after the 8bb3247a3 rewrite).
 *
 * Splits scene luminance into an edge-aware base layer (bilateral grid in log luminance,
 * blended with a broad blur) and a detail layer, compresses only the base around middle grey
 * and keeps the detail, so a bright window and a dark room are both readable without the
 * picture going flat.
 *
 * Upstream applies the result in its own Composite pass together with the global exposure. We
 * have no Composite: our Histogram Auto Exposure writes the exposed image itself, so this effect
 * sits right after it in the chain and applies the local adjustment to the exposed image in its
 * final pass (fused with upstream's resolve pass, so the full-resolution base texture is never
 * written). With Auto Exposure on, the pivot is the auto-exposure middle grey (0.18 x its
 * Exposure Compensation); with it off, "Exposure" says how exposed the image is assumed to be.
 * It never scales the image as a whole.
 *
 * Runs after upscaling, at output resolution, before Color Grading / tone mapping, so it works
 * the same with "Disable Vanilla Tonemapping" on or off.
 */
struct LocalExposure : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Local Exposure"; }
	virtual inline std::string GetDesc() const override
	{
		return "Balances bright and dark areas (a sunlit doorway seen from a dark room, a cave mouth) while keeping "
		       "edge detail. Works on the image after Histogram Auto Exposure, before Color Grading.";
	}
	virtual inline bool DisableInMainLoadingMenu() const override { return true; }
	virtual bool SupportsVR() const override { return false; }
	virtual bool RuntimeGateOpen() const override;

	struct Settings
	{
		float Exposure = 0.7f;
		float Strength = 1.0f;
		float HighlightContrast = 0.75f;
		float ShadowContrast = 0.8f;
		float DetailStrength = 1.0f;
		float BaseBlend = 0.6f;
		float BlurredLuminanceKernelSize = 50.0f;
		float MiddleGreyBias = 0.0f;
		float HighlightThreshold = 1.0f;
		float ShadowThreshold = 1.0f;
		float HighlightThresholdStrength = 1.0f;
		float ShadowThresholdStrength = 1.0f;
	} settings;

	struct alignas(16) LocalExposureCB
	{
		float GlobalExposure;  // 1 after Histogram Auto Exposure (already applied), else Settings::Exposure
		float Strength;
		float HighlightContrast;
		float ShadowContrast;

		float DetailStrength;
		float BaseBlend;
		float BlurRadius;
		float MiddleGreyBias;

		float HighlightThreshold;
		float ShadowThreshold;
		float HighlightThresholdStrength;
		float ShadowThresholdStrength;

		uint InputWidth;
		uint InputHeight;
		uint BlurredWidth;
		uint BlurredHeight;

		float LogLuminanceMin;
		float LogLuminanceMax;
		float MiddleGreyCompensation;  // auto exposure's Exposure Compensation (linear), else 1
		float pad;
	};
	STATIC_ASSERT_ALIGNAS_16(LocalExposureCB);

	static constexpr uint s_MaxMips = 10;
	static constexpr uint s_BlurMip = 5;
	static constexpr uint s_MaxBlurRadius = 64;
	static constexpr uint s_GridDepth = 32;
	static constexpr uint s_GridTileSize = 64;

	std::unique_ptr<ConstantBuffer> localExposureCB = nullptr;

	eastl::unique_ptr<Texture2D> texLogLuminance = nullptr;
	eastl::unique_ptr<Texture3D> texLuminanceGrid = nullptr;
	eastl::unique_ptr<Texture2D> texBlurTemp = nullptr;
	eastl::unique_ptr<Texture2D> texBlurredLuminance = nullptr;
	eastl::unique_ptr<Texture2D> texOutput = nullptr;

	std::array<winrt::com_ptr<ID3D11ShaderResourceView>, s_MaxMips> logLuminanceMipSRVs = {};
	std::array<winrt::com_ptr<ID3D11UnorderedAccessView>, s_MaxMips> logLuminanceMipUAVs = {};
	uint numMips = 0;

	winrt::com_ptr<ID3D11SamplerState> linearSampler = nullptr;
	winrt::com_ptr<ID3D11SamplerState> mirrorSampler = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> setupCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> downsampleCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurHorizontalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> blurVerticalCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> gridCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> applyCS = nullptr;

	/// Whether the last Draw ran (for the Batch 38 table), and why not.
	bool lastDrawRan = false;
	const char* lastDrawStatus = "Not run yet";

	virtual void SetupResources() override;
	virtual void SetupShaders() override;
	virtual void ReleaseResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	virtual void RestoreDefaultSettings() override;
	virtual void LoadSettings(json&) override;
	virtual void SaveSettings(json&) override;
	virtual void DrawSettings() override;

	virtual void Draw(TextureInfo&) override;
};
