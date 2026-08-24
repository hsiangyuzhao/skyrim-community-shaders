#pragma once

#include "Buffer.h"
#include "Feature.h"
#include "NRDReblurIntegration.h"

#include <NRDSettings.h>

/**
 * NRD: NVIDIA Real-Time Denoisers — core top-level service.
 *
 * Owns the per-frame guide textures shared by every NRD-consuming feature
 * (motion vectors, viewZ, packed normal+roughness) and runs the guide
 * preparation compute pass once per frame. Consumers (Screen Space Ray
 * Tracing's REBLUR denoiser path in this fork) construct their own
 * NRDReblurIntegration instances and consume the guides published here.
 *
 * Ported from the upstream reference integration (batch C1). Adaptations:
 * feature categories are plain strings in this fork, Texture2D carries no
 * name parameter (Util::SetResourceName is called explicitly), and the
 * screen extent comes from State::screenSize. Flat-screen only: SupportsVR
 * stays false, so in VR the feature never loads and consumers fall back to
 * their non-NRD denoiser paths.
 */
struct NRD : Feature
{
	virtual inline std::string GetName() override { return "NRD"; }
	virtual inline std::string GetShortName() override { return "NRD"; }
	virtual std::string_view GetCategory() const override { return "Lighting"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return std::make_pair(
			std::string(
				"NVIDIA Real-Time Denoisers (NRD) integration. Provides the shared "
				"viewZ / normal+roughness / motion-vector guide textures and the "
				"REBLUR denoiser plumbing used by features that produce noisy "
				"radiance signals (Screen Space Ray Tracing)."),
			std::vector<std::string>{
				"Shared NRD guide textures",
				"REBLUR denoiser infrastructure",
				"Common camera/jitter/frame-index state" });
	}

	virtual void RestoreDefaultSettings() override;
	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;

	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	void CompileComputeShaders();

	// Per-frame service entry point. Called once at the start of the deferred
	// pass so every consumer sees consistent guides for this frame.
	void PrepareGuides();

	// (batch 11, item C2) Allocate the three guide textures on first need. Called from
	// PrepareGuides, and only after AnyConsumerNeedsGuides() has passed, so a session that never
	// selects REBLUR never pays for them (~25 MB at 1080p, ~100 MB at a 4K allocation). Returns
	// false if the set is not complete afterwards, which PrepareGuides treats as a failure like
	// any other. Idempotent; the early-out makes the steady-state call free.
	bool EnsureGuides();

	// True when PrepareGuides has run successfully for this frame. Consumers
	// should skip their dispatches if guides aren't ready (e.g. world not
	// loaded, shader failed to compile).
	bool AreGuidesReady() const { return guidesReadyThisFrame; }

	// (S1.3) Whether PrepareGuides *would* succeed if it were called now — exactly the
	// precondition set PrepareGuides itself tests, minus anything per-frame.
	//
	// Consumers need this because the frame's pass order puts every feature's Prepass()
	// ahead of PrepareGuides(): a consumer deciding at Prepass time whether to bring its
	// REBLUR path up cannot ask AreGuidesReady() without reading last frame's answer.
	// This is the question that has a stable answer at that point.
	//
	// (batch 11, item C2) It deliberately does *not* ask whether the guide textures exist any
	// more, and that omission is load-bearing in exactly the way ReblurResourcesReady's is.
	// The guides are allocated by EnsureGuides on the far side of AnyConsumerNeedsGuides(),
	// which asks whether a consumer resolved to REBLUR — and this predicate is one of the
	// inputs to that resolution. Asking about the allocation here would close the loop: no
	// consumer would select REBLUR because no guides existed, and no guides would be allocated
	// because no consumer had selected REBLUR. What is left is a fact about configuration, which
	// is the only kind of fact this question can safely be built on.
	bool CanPrepareGuides() const
	{
		return settings.Enabled && prepareNRDGuidesCompute;
	}

	// Build a CommonSettings block shared by every denoiser instance this
	// frame. Centralizing this guarantees frameIndex and prev-frame matrices
	// stay in lockstep across consumers.
	const nrd::CommonSettings& GetCommonSettings();

	// Per-frame ReBLUR settings published by NRD with the consumer-tuned
	// parameters folded in. Consumers call this just before SetDenoiserSettings.
	struct REBLURSettings
	{
		uint32_t MaxAccumulatedFrameNum = 30;
		uint32_t MaxFastAccumulatedFrameNum = 6;
		uint32_t MaxStabilizedFrameNum = nrd::REBLUR_MAX_HISTORY_FRAME_NUM;
		uint32_t HistoryFixFrameNum = 3;
		uint32_t HistoryFixBasePixelStride = 14;
		uint32_t HistoryFixAlternatePixelStride = 14;
		float FastHistoryClampingSigmaScale = 2.0f;
		float MinHitDistanceWeight = 0.1f;
		float MinBlurRadius = 1.0f;
		float MaxBlurRadius = 30.0f;
		float LobeAngleFraction = 0.15f;
		float RoughnessFraction = 0.15f;
		float PlaneDistanceSensitivity = 0.02f;
		float SplitScreen = 0.0f;
		uint32_t HitDistanceReconstructionMode = 0;
		bool EnableValidation = false;
		bool ReturnHistoryLength = false;
	};

	// Translate the UI struct into an nrd::ReblurSettings, applying common
	// clamps. Callers can still overwrite specific fields (e.g. hitDistanceParameters
	// for specular) after this returns.
	//
	// (batch C1) Antilag is disabled here for every consumer — NVIDIA's own
	// first-integration guidance is to bring REBLUR up with antilag off and
	// checkerboard off, and this fork's consumers all pass CheckerboardMode::OFF.
	void ApplyReblurSettings(nrd::ReblurSettings& out, const REBLURSettings& in, nrd::CheckerboardMode checkerboard) const;

	// Render an ImGui block for a REBLURSettings instance. Returns true if any
	// value changed. Centralized so every consumer exposes the same controls
	// with the same labels and ranges.
	bool DrawReblurSettings(REBLURSettings& s, bool showAdvanced, const char* tag);

	// Guide accessors. Valid after PrepareGuides() returns guidesReadyThisFrame=true.
	ID3D11ShaderResourceView* GetViewZSRV() const { return texNRDViewZ ? texNRDViewZ->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetNormalRoughnessSRV() const { return texNRDNormalRoughness ? texNRDNormalRoughness->srv.get() : nullptr; }
	ID3D11ShaderResourceView* GetMotionVectorSRV() const { return texNRDMV ? texNRDMV->srv.get() : nullptr; }
	ID3D11UnorderedAccessView* GetMotionVectorUAV() const { return texNRDMV ? texNRDMV->uav.get() : nullptr; }

	//////////////////////////////////////////////////////////////////////////

	struct Settings
	{
		bool Enabled = true;
	} settings;

	// Shared per-frame state. Tracked here so every consumer sees identical
	// prev-frame matrices and a single monotonic frame counter.
	Matrix worldToViewMat{};
	Matrix prevWorldToViewMat{};
	Matrix prevProjMatrix{};
	float2 prevJitter{};
	nrd::CommonSettings commonSettings{};
	bool commonSettingsValidThisFrame = false;
	bool guidesReadyThisFrame = false;
	bool hasCommonFrameHistory = false;
	uint32_t lastCommonGameFrame = 0;
	uint16_t prevResourceSize[2] = {};
	uint16_t prevRectSize[2] = {};

	// Shared guide textures owned by NRD. (batch 11, item C2) Null until EnsureGuides runs, i.e.
	// until a consumer has actually resolved to REBLUR; dropped again by SetupResources so a
	// resolution change cannot leave them at the old extent.
	eastl::unique_ptr<Texture2D> texNRDViewZ = nullptr;
	eastl::unique_ptr<Texture2D> texNRDNormalRoughness = nullptr;
	eastl::unique_ptr<Texture2D> texNRDMV = nullptr;

	winrt::com_ptr<ID3D11ComputeShader> prepareNRDGuidesCompute = nullptr;
};

// REBLURSettings is consumed by every feature that owns an NRDReblurIntegration
// instance. Its serializer must be visible in every such translation unit, so
// the macro lives here, not in NRD.cpp.
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	NRD::REBLURSettings,
	MaxAccumulatedFrameNum,
	MaxFastAccumulatedFrameNum,
	MaxStabilizedFrameNum,
	HistoryFixFrameNum,
	HistoryFixBasePixelStride,
	HistoryFixAlternatePixelStride,
	FastHistoryClampingSigmaScale,
	MinHitDistanceWeight,
	MinBlurRadius,
	MaxBlurRadius,
	LobeAngleFraction,
	RoughnessFraction,
	PlaneDistanceSensitivity,
	SplitScreen,
	HitDistanceReconstructionMode,
	EnableValidation,
	ReturnHistoryLength)
