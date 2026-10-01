#pragma once

#include "Buffer.h"

/**
 * @brief Variable Rate Shading (VRS) for the engine's opaque G-buffer pass, via NVAPI.
 *
 * Skyrim renders through a native D3D11 device, and D3D11 has no core VRS API, so this uses
 * NVIDIA's D3D11 extension (Turing and newer): a per-16x16-tile shading-rate image plus a
 * per-viewport lookup table. The driver's own nvapi64.dll is loaded at runtime; on any other
 * GPU, or a driver without VRS, the feature reports why and stays completely inert.
 *
 * Frame flow:
 *   StartDeferred  -> BeginOpaquePass(): build this frame's rate image from last frame's
 *                     tile statistics (reprojected by motion), bind it.
 *   every draw     -> OnDraw(): per draw, pick the rate table (off / full / alpha-tested)
 *                     from the shader class, so only opaque Lighting / Grass / DistantTree
 *                     G-buffer draws are ever coarse-shaded.
 *   EndDeferred    -> EndOpaquePass(): VRS off and unbound before any of our compute work.
 *                  -> AnalyzeFrame(): after the deferred composite, measure this frame's lit
 *                     scene per tile for the next frame; optional debug tint.
 *   Present        -> Reset(): safety net, VRS forced off before the UI is drawn.
 *
 * Coarse shading never changes coverage or depth (those stay per pixel); it only makes one
 * pixel-shader result cover a 2x1..4x4 block of pixels, which includes the G-buffer normal and
 * motion vector the shader writes. See DrawSettings for the user-facing trade-offs.
 */
struct VariableRateShading : Feature
{
	virtual inline std::string GetName() override { return "Variable Rate Shading"; }
	virtual inline std::string GetShortName() override { return "VariableRateShading"; }
	virtual std::string_view GetCategory() const override { return "Display"; }
	virtual bool SupportsVR() override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Lowers shading detail on flat, low-detail parts of the scene to save GPU time, which can make them slightly softer. "
			"Needs an NVIDIA RTX 20-series or newer GPU; not available in VR.",
			{ "Adapts to the picture: lowers detail only where the last frame looked flat",
				"Optional mode that lowers detail towards the screen edges",
				"Only solid scene geometry is affected; shadows, UI and post-processing never are",
				"Debug overlay showing where detail is lowered" }
		};
	}

	enum class Mode : uint32_t
	{
		Adaptive = 0,
		Periphery = 1,
		AdaptivePeriphery = 2,
	};

	struct Settings
	{
		bool Enabled = false;
		uint32_t RateMode = static_cast<uint32_t>(Mode::Adaptive);
		float Quality = 0.5f;             // 0..1, higher keeps more tiles at full rate
		uint32_t CoarsestRate = 0;        // 0 = 2x2, 1 = 4x4
		float MotionPixels = 8.0f;        // speed (px/frame) at which the threshold doubles, 0 = off
		bool ProtectNormals = true;       // keep tiles with G-buffer normal detail at full rate
		bool IncludeGrass = false;        // coarse-shade grass draws
		bool IncludeAlphaTested = false;  // coarse-shade other alpha-tested draws (leaves, hair, LOD trees)
		float PeripheryRadius = 0.8f;     // full-rate centre radius in half screen heights
		bool DebugOverlay = false;
	};

	Settings settings;

	/// Mirrors RateCB in VariableRateShading/Common.hlsli.
	struct alignas(16) RateCB
	{
		uint32_t RenderSize[2];
		uint32_t RenderTiles[2];
		uint32_t ImageTiles[2];
		uint32_t Mode;
		uint32_t MaxRateLog2;
		float Threshold;
		float QuarterFactor;
		float MotionPixels;
		float NormalWeight;
		float PeripheryRadius;
		float EnvLuminance;
		uint32_t HistoryValid;
		float Hysteresis;
	};
	STATIC_ASSERT_ALIGNAS_16(RateCB);

	// Nothing of this feature is compiled into the engine shader permutations the disk cache
	// holds (its compute shaders are compiled directly at runtime), so installing, removing or
	// updating it must not throw that cache away and force a full recompile.
	virtual bool ValidateCache(CSimpleIniA&) override { return true; }

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void Reset() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;

	/// @brief Deferred::StartDeferred, after the feature prepasses: build and bind the rate image.
	void BeginOpaquePass();

	/// @brief Deferred::EndDeferred, before anything else: VRS off and the rate image unbound.
	void EndOpaquePass();

	/// @brief Deferred::EndDeferred, after the deferred composite: per-tile analysis for the next frame.
	void AnalyzeFrame();

	/// @brief State::Draw, i.e. once per engine draw: selects the rate table for this draw.
	inline void OnDraw()
	{
		if (!opaqueWindow && appliedTable == Table::Off)
			return;
		UpdateDrawState();
	}

private:
	enum class Table : uint8_t
	{
		Off,
		Full,         // opaque draws: the rate image as built
		AlphaTested,  // cut-out draws: same image, capped at 2x2 so edges do not get 4 px steps
	};

	bool IsActive() const;
	void UpdateDrawState();
	Table ClassifyDraw() const;
	void ApplyTable(Table a_table);
	void DisableForSession(std::string a_reason);
	void ReadBackRateCounts();
	RateCB BuildConstants(bool a_historyValid) const;
	ID3D11ComputeShader* GetShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path);

	// Why VRS is unavailable this session (empty when it is available).
	std::string unavailableReason = "Not initialised yet.";
	bool hardwareSupported = false;
	bool shaderFailed = false;

	winrt::com_ptr<ID3D11Texture2D> tileStats;
	winrt::com_ptr<ID3D11ShaderResourceView> tileStatsSRV;
	winrt::com_ptr<ID3D11UnorderedAccessView> tileStatsUAV;

	// Ping-pong pair so the build pass can read last frame's rates (hysteresis) through an SRV.
	winrt::com_ptr<ID3D11Texture2D> rateImage[2];
	winrt::com_ptr<ID3D11ShaderResourceView> rateImageSRV[2];
	winrt::com_ptr<ID3D11UnorderedAccessView> rateImageUAV[2];
	IUnknown* rateImageView[2] = {};  // ID3D11NvShadingRateResourceView, owned (Released in SetupResources)
	uint32_t currentImage = 0;

	static constexpr uint32_t kRateIndices = 9;
	static constexpr uint32_t kReadbackSlots = 3;
	winrt::com_ptr<ID3D11Buffer> rateCounts;
	winrt::com_ptr<ID3D11UnorderedAccessView> rateCountsUAV;
	winrt::com_ptr<ID3D11Buffer> rateCountsStaging[kReadbackSlots];
	bool readbackPending[kReadbackSlots] = {};
	uint32_t readbackWrite = 0;
	uint32_t lastRateCounts[kRateIndices] = {};
	bool hasRateCounts = false;

	std::unique_ptr<ConstantBuffer> rateCB;
	winrt::com_ptr<ID3D11ComputeShader> analyzeCS;
	winrt::com_ptr<ID3D11ComputeShader> buildCS;
	winrt::com_ptr<ID3D11ComputeShader> debugCS;

	uint32_t imageTiles[2] = {};

	// Per-frame bookkeeping. frameIndex advances in Reset(), i.e. once per Present.
	uint64_t frameIndex = 1;
	uint64_t lastBuildFrame = 0;
	uint64_t lastAnalysisFrame = 0;
	uint32_t lastAnalysisRenderSize[2] = {};

	bool opaqueWindow = false;  // between BeginOpaquePass and EndOpaquePass with the image bound
	bool imageBound = false;
	Table appliedTable = Table::Off;
};
