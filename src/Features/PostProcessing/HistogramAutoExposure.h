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

		// (49h stage 2) "Exposure Mode" (ExposureModes). Classic = the controls above, as before; Physical = the wide-range trimmed
		// histogram, asymmetric adaptation and the dark-scene compensation curve below (design-49h-stage2.md B).
		int ExposureMode = 0;
		float PhysDarkCompensation = 0.6f;    // alpha: on-screen darkening per EV the scene is darker than Full Adaptation Above
		float PhysFullAdaptationAbove = -1.f; // EV_ref
		float PhysDarkestAdapted = -6.5f;     // EV_min: the adaptation stops here
		float PhysBrightBoost = 0.25f;        // beta above EV_hi (+1.5)
		float PhysAdaptIntoDark = 3.5f;       // seconds
		float PhysAdaptIntoBright = 0.8f;     // seconds
		float PhysIgnoreDarkest = 10.f;       // percent of the samples
		float PhysIgnoreBrightest = 5.f;      // percent
		float PhysCentreWeight = 0.f;         // 0-1: the centre counts up to 3x
		bool PhysHoldSmallChanges = true;     // deadband 0.10-0.35 EV
		float PhysPurkinjeStartEV = -3.f;     // the Purkinje fade-in in this mode (the strength above is shared)
		float PhysPurkinjeMaxEV = -7.5f;
	} settings;

	/// (49h stage 2) Exposure Mode values.
	enum ExposureModes : int
	{
		kExposureAuto = 0,  // Physical while Physical Sky's "Physical Lighting" master is on, else Classic
		kExposureClassic,
		kExposurePhysical,
		kExposureModeCount
	};
	/// (49h stage 2) The mode in effect this frame (Auto resolved).
	[[nodiscard]] bool PhysicalMode() const;
	/// (49h stage 2) Compensation curve constants that are not in the menu.
	static constexpr float kPhysEVHi = 1.5f;
	static constexpr float kPhysAdaptMinEV = -9.5f;
	static constexpr float kPhysAdaptMaxEV = 5.f;
	static constexpr float kPhysSlewDark = 1.5f;    // EV per second
	static constexpr float kPhysSlewBright = 3.f;
	static constexpr float kPhysMinLogLum = -15.f;  // EV -12 .. +11
	static constexpr float kPhysLogLumRange = 23.f;
	/// (49h stage 2) Snap / hold bookkeeping (loads, fast travel, time skips, first frame, mode switch; menus hold).
	int snapFrames = 2;
	bool lastModePhysical = false;
	float lastHoursPassed = -1.f;
	/// (49h stage 2) The adaptation buffer read back 2-3 frames late, for the status line, the F7 header and Local Exposure's pivot:
	/// [0] divisor, [1] adapted luminance, [2] filtered target, [3] compensation (linear).
	static constexpr int kReadbackCount = 3;
	winrt::com_ptr<ID3D11Buffer> readbackBuffers[kReadbackCount];
	bool readbackPending[kReadbackCount] = {};
	uint32_t readbackIndex = 0;
	float readback[4] = { 0.f, 0.f, 0.f, 1.f };
	bool readbackValid = false;
	bool readbackPhysical = false;
	void QueueReadback();
	/// (49h stage 2) One line for the F7 trace header (mode, scene EV, compensation, exposure).
	std::string DescribeForTrace() const;
	/// (49h stage 2) The last exposure multiplier as EV (0.18 x compensation / divisor), from the readback.
	[[nodiscard]] float LastExposureEV() const;
	/// (49i) The compensation (linear) and the adaptation range (linear luminance) of the last Draw, for LastExposureEV.
	float lastAppliedCompensation = 1.f;
	float2 lastAppliedRange = { 0.f, 1e30f };

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

		// (49i) Mirrors HLSL's packing of common.hlsli's `float pad[3]`: an array in a cbuffer starts on a 16-byte register and every
		// element takes one register (offsets 48 / 64 / 80, 36 bytes), so the Phys* fields start at byte 84 there. 49h declared a plain
		// float[3] here (bytes 36-48): every Phys* value landed 9 floats early and the last nine past the 128-byte buffer (read as 0),
		// which pinned the Physical adaptation to EV 0 (clamp [-3 + 0, -3 + 0] in log2 L) with no compensation.
		float padAlign[3];     // bytes 36-48: up to HLSL's register boundary
		float padHlslArray[9]; // bytes 48-84: HLSL's float pad[3] (three registers, the last one partly)

		// (49h stage 2) Physical mode (common.hlsli); Classic leaves them 0 and never reads them.
		float PhysMinLogLum;
		float PhysLogLumRange;
		uint PhysMode;
		float PhysDeltaTime;
		float PhysTrimLow;
		float PhysTrimHigh;
		float PhysCentreWeight;
		float PhysDeadband;
		float PhysTauDark;
		float PhysTauBright;
		float PhysSlewDark;
		float PhysSlewBright;
		float PhysAlpha;
		float PhysEVRef;
		float PhysEVMin;
		float PhysBeta;
		float PhysEVHi;
		float PhysAdaptMinEV;
		float PhysAdaptMaxEV;
		uint PhysFlags;
		float padTail[3];
	};
	// (49i) Offsets as fxc reports them for common.hlsli (histogram.cs.hlsl /Fc listing): PhysMinLogLum 84, PhysMode 92, PhysFlags 160.
	static_assert(offsetof(AutoExposureCB, PurkinjeStrength) == 32);
	static_assert(offsetof(AutoExposureCB, PhysMinLogLum) == 84);
	static_assert(offsetof(AutoExposureCB, PhysMode) == 92);
	static_assert(offsetof(AutoExposureCB, PhysFlags) == 160);
	static_assert(sizeof(AutoExposureCB) == 176);
	std::unique_ptr<ConstantBuffer> autoExposureCB = nullptr;
	std::unique_ptr<StructuredBuffer> histogramSB = nullptr;
	std::unique_ptr<StructuredBuffer> adaptationSB = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> histogramCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> histogramAvgCS = nullptr;
	winrt::com_ptr<ID3D11ComputeShader> adaptCS = nullptr;

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
};