#pragma once

#include "Buffer.h"

/**
 * @brief (batch 38, item A2) Variance shadow map of the sun for transparent things.
 *
 * Ported from upstream Volumetric Shadows (0f79d567a, fixes df08ea281, e214c451f, 7d3e6a7c7).
 * The sun's two near cascades are downsampled into one 512x512 R16G16 moment texture (mip 0 =
 * cascade 1 at 512, mip 1 = cascade 0 at 256), blurred 11x11, and bound at PS t23. Smoke,
 * steam and other lit effects then march a short segment along the view ray through it, so a
 * puff darkens where it passes into a shadow instead of glowing at full sunlight.
 *
 * Built from the shared capture in Deferred::CopyShadowData (the engine's shadow-mask draw),
 * which Volumetric Fog also reads. Unlike upstream we keep our own SharedShadowData (t19)
 * cascade matrices (VR-aware, camera-relative, per eye) rather than upstream's t98 buffer.
 *
 * Every switch is ANDed with Batch38::IsOn(); with the master off nothing is built and the
 * shaders take their 37c paths.
 */
struct VolumetricShadows : Feature
{
public:
	virtual inline std::string GetName() override { return "Volumetric Shadows"; }
	virtual inline std::string GetShortName() override { return "VolumetricShadows"; }
	virtual inline std::string_view GetShaderDefineName() override { return "VOLUMETRIC_SHADOWS"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }
	virtual bool IsCore() const override { return true; }
	virtual bool SupportsVR() override { return true; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Lets smoke, steam, mist and other see-through effects receive the sun's shadow (Batch 38).\n"
			"A small blurred copy of the sun shadow map is made each frame and effects sample it.",
			{ "Smoke and steam darken in shadow",
				"Soft, cheap shadow lookups (variance shadow map)",
				"Optional soft sun shadow for forward-rendered objects" }
		};
	}

	bool HasShaderDefine(RE::BSShader::Type shaderType) override;

	struct Settings
	{
		/// Lit effects and particles sample the VSM along the view ray (default on).
		bool ParticleShadows = true;
		/// Forward-rendered objects without a shadow mask use the VSM instead of the 16-tap
		/// Poisson PCF (default off; see DrawSettings for why).
		bool ForwardSoftShadows = false;
		/// Also take the vanilla god-ray shadow maps into account (upstream does, min() of both).
		/// Only used while vanilla Volumetric Lighting is running, since the maps are not
		/// re-rendered otherwise.
		bool MergeGodRayMaps = true;
	} settings;

	/// Mirrors SharedData::VolumetricShadowsSettings (HLSL), appended to FeatureData.
	struct alignas(16) CommonBufferData
	{
		uint ParticleShadows;     ///< effective: master && setting && VSM valid
		uint ForwardSoftShadows;  ///< effective: master && setting && VSM valid
		uint pad0;
		uint pad1;
	};
	STATIC_ASSERT_ALIGNAS_16(CommonBufferData);

	[[nodiscard]] CommonBufferData GetCommonBufferData() const;

	/// Whether any consumer wants the VSM this frame (master && a switch on).
	[[nodiscard]] bool WantsVsm() const;
	[[nodiscard]] bool ParticleShadowsActive() const;
	[[nodiscard]] bool ForwardSoftShadowsActive() const;
	/// True if the VSM was rebuilt this frame or the previous one.
	[[nodiscard]] bool VsmValid() const;

	/// Called from Deferred::CopyShadowData with the captured cascade array.
	void OnShadowCapture(ID3D11ShaderResourceView* a_shadowMap);

	virtual void DrawSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	static constexpr uint32_t kVsmSlot = 23;  ///< PS register of the VSM (t23)
	static constexpr uint32_t kVsmSize = 512;

	// Diagnostics for the Batch 38 table
	uint32_t lastBuildFrame = UINT32_MAX;
	uint32_t sourceWidth = 0;
	bool usedGodRayMaps = false;

private:
	void EnsureTextures();
	void CompileShaders();
	void BindVsm(ID3D11ShaderResourceView* a_srv);

	ID3D11ComputeShader* downsampleMip0CS = nullptr;
	ID3D11ComputeShader* downsampleMip1CS = nullptr;
	ID3D11ComputeShader* blurHCS = nullptr;
	ID3D11ComputeShader* blurVCS = nullptr;

	winrt::com_ptr<ID3D11Texture2D> vsmTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> vsmSRV;
	winrt::com_ptr<ID3D11ShaderResourceView> vsmMipSRV[2];
	winrt::com_ptr<ID3D11UnorderedAccessView> vsmMipUAV[2];
	winrt::com_ptr<ID3D11Texture2D> blurTexture;
	winrt::com_ptr<ID3D11ShaderResourceView> blurMipSRV[2];
	winrt::com_ptr<ID3D11UnorderedAccessView> blurMipUAV[2];
	winrt::com_ptr<ID3D11SamplerState> linearSampler;
};
