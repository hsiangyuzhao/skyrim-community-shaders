#pragma once

#include "PostProcessFeature.h"

#include "Buffer.h"

struct HistogramAutoExposure : public PostProcessFeature
{
	virtual inline std::string GetType() const override { return "Histogram Auto Exposure"; }
	virtual inline std::string GetDesc() const override { return "Auto exposure/Eye adaptation method that uses histogram to calculate average screen brightness. "
																 "Expects HDR linear RGB inputs."; }

	// virtual inline bool DrawBeforeUpscaling() const override { return true; }

	virtual bool SupportsVR() { return true; }

	struct Settings
	{
		float ExposureCompensation = 0.f;
		bool EnableToD = false;
		std::array<float, 6> ToDExposure = { 0.f, 0.f, 0.f, 0.f, 0.f, 0.f };
		bool EnableInterior = false;
		float InteriorExposure = 0.f;

		// auto exposure
		float2 AdaptationRange = { -.5f, .2f };  // EV
		std::array<float2, 6> ToDAdaptationRange = { float2{ -.5f, .2f }, float2{ -.5f, .2f }, float2{ -.5f, .2f }, float2{ -.5f, .2f }, float2{ -.5f, .2f }, float2{ -.5f, .2f } };
		float2 InteriorAdaptationRange = { -.5f, .2f };
		float2 AdaptArea = { .6f, .6f };

		float AdaptSpeed = 1.5f;

		// purkinje
		float PurkinjeStartEV = -1.5f;  // EV
		float PurkinjeMaxEV = -4.f;     // EV
		float PurkinjeStrength = 1.f;

		// (50a) Eye Model (physical lighting redo, stage 1: eye + display, no scene change). Off (default) = Classic exactly (the classic
		// shaders, untouched). On with the defaults below = as close to Classic as the design allows: Night Darkness 100 % keeps the scene's
		// own darkness below the Adaptation Range minimum (Classic's fixed exposure there), Minimum Visibility 0, Mesopic 0 (Classic's
		// Purkinje settings then apply), adaptation from Adaptation Speed. New key names (not 49j's ExposureMode / Phys*).
		bool EyeModel = false;
		float EyeNightDarkness = 1.0f;      // 0..1
		float EyeMinVisibility = 0.0f;      // 0..1 of middle grey
		float EyeMesopic = 0.0f;            // 0..1
		float EyeDarkAdaptSlower = 1.0f;    // into dark: this many times slower than into bright (1 = as Adaptation Speed both ways)
		float EyeMaxChangeEVps = 0.0f;      // 0 = no limit
		float EyeIgnoreSmallEV = 0.0f;      // deadband
		bool EyeKeepAdaptingAfterLoad = false;  // off: snap after loading screens and time skips
	} settings;

	// buffers
	struct alignas(16) AutoExposureCB
	{
		float2 AdaptArea;
		float2 AdaptationRange;
		float AdaptLerp;
		float ExposureCompensation;
		float PurkinjeStartEV;
		float PurkinjeMaxEV;
		float PurkinjeStrength;

		float pad[3];  // HLSL: float pad[3] (Classic variants) / float3 pad (EYE_MODE variants, R5): the eye fields start at offset 48 either way

		// (50a) Eye Model (common.hlsli, EYE_MODE). Not read by the classic shaders.
		float EyeLogLo;
		float EyeLogHi;
		float EyeNightDarkness;
		float EyeMinVisibility;
		float EyeTauBright;
		float EyeTauDark;
		float EyeMaxRate;
		float EyeDeadband;
		float EyeWobble;
		float EyeMesopic;
		float EyeDt;
		uint32_t EyeFlags;
	};
	static constexpr uint32_t kEyeSnap = 1u, kEyeGlobalFloor = 2u, kEyeInstant = 4u;
	std::unique_ptr<ConstantBuffer> autoExposureCB = nullptr;
	std::unique_ptr<StructuredBuffer> histogramSB = nullptr;
	std::unique_ptr<StructuredBuffer> adaptationSB = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> histogramCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> histogramAvgCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> adaptCS = nullptr;
	// (50a) Eye Model variants (EYE_MODE).
	winrt::com_ptr<ID3D11ComputeShader> histogramEyeCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> histogramAvgEyeCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> adaptEyeCS = nullptr;

	/// (50a) Eye Model in effect this frame (switch on and its shaders compiled).
	[[nodiscard]] bool EyeActive() const { return settings.EyeModel && histogramEyeCS && histogramAvgEyeCS && adaptEyeCS; }
	/// (50a) Scene Scale Wobble self-test (Developer; not saved): the frame x 2^sin(2 pi t / 4 s) before the histogram, adaptation instant.
	bool eyeWobble = false;
	double eyeWobbleStart = 0.0;
	/// (50b) Physical Sky's "Sky Light Wobble" self-test: the adaptation holds its level (classic: no lerp; eye: no change, no snap) while set.
	static inline bool freezeAdaptation = false;
	/// (50a) Snap bookkeeping: a loading screen seen since the last Draw, the game hour of the last Draw, the eye mode of the last Draw.
	int eyeSnapFrames = 3;
	float eyeLastGameHours = -1.0f;
	bool eyeWasActive = false;
	/// (50a) Minimum Visibility reaches Local Exposure (its local base) when it runs this frame; the global floor otherwise.
	float eyeLocalMinVisibility = 0.0f;

	std::unique_ptr<Texture2D> texAdapt = nullptr;

	/// (batch 38) Exposure Compensation (linear) of the last Draw, after time-of-day / interior
	/// overrides. Local Exposure pivots around 0.18 x this, the middle grey this pass targets.
	float lastExposureCompensation = 1.f;

	/// (44e) Set by PostProcessing for one Draw: Local Exposure applies the exposure on load, so the
	/// adapt pass (a full-resolution copy) is skipped and the image is handed on unchanged.
	bool foldIntoNext = false;
	bool lastFolded = false;

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

	/// @brief Exposure compensation (EV) and adaptation range (EV) in effect this frame, after the
	/// time-of-day and interior overrides. (batch 38c) Shared with Neural Rendering's before-upscaling
	/// encode, which applies the same exposure this effect will.
	void GetExposureParameters(float& exposureCompensation, float2& adaptationRange) const;
	/// (50a) The user's values (what GetExposureParameters returned before 50a). GetExposureParameters gives the consumers of the adaptation
	/// buffer (Neural Rendering) a pass-through range while the Eye Model is on: buffer [0] then holds the eye's divisor itself.
	void GetUserExposureParameters(float& exposureCompensation, float2& adaptationRange) const;
	static constexpr float kEyePassThroughEV = 60.0f;  // +- this many EV of 0.125: the consumers' clamp never acts
};
static_assert(offsetof(HistogramAutoExposure::AutoExposureCB, EyeLogLo) == 48 && sizeof(HistogramAutoExposure::AutoExposureCB) == 96,
	"AutoExposureCB must match common.hlsli (EYE_MODE layout)");