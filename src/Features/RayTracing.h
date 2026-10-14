#pragma once

#include "Buffer.h"
#include "Feature.h"

#include "Features/NRDReblurIntegration.h"
#include "Features/RayTracing/Backend.h"
#include "Features/RayTracing/Characters.h"
#include "Features/RayTracing/Scene.h"

/**
 * @brief (45) Ray Tracing (prototype): ray-traced sun shadows inside the loaded exterior cells.
 *
 * One switch. Off (default) = nothing of this feature runs: no D3D12 device, no hook does
 * anything, the shadow mask, Screen Space Shadows and the cascades are exactly the game's.
 * On = the static objects and landscape of the loaded cells are captured into BLASes on a
 * D3D12 device of our own (Backend.h); each frame, after the engine computed its sun shadow
 * mask and before the opaque pass, a ray query pass traces one or two rays per pixel toward
 * the sun and the result is merged into the shadow mask the Lighting shader reads (t14):
 *     mask.r = lerp(mask.r, min(mask.r, rt), w)
 * where w fades from 1 inside the loaded area to 0 over the last part of it (back to the
 * cascades / terrain shadows). Geometry the ray tracer covers is left out of the sun cascades
 * (CasterFilter.h), so the shadow map then only holds the casters the ray tracer does not see
 * (characters, trees, grass, alpha-tested and moving objects), and min() keeps their shadows.
 * Screen Space Shadows are switched off inside the traced area (its texture is set to 1 there).
 *
 * Not touched: baked AO, Skylighting, point-light shadows, LOD shadows, terrain shadows.
 */
struct RayTracing : Feature
{
public:
	virtual inline std::string GetName() override { return "Ray Tracing"; }
	virtual inline std::string GetShortName() override { return "RayTracing"; }
	virtual inline std::string_view GetCategory() const override { return "Lighting"; }
	virtual inline bool SupportsVR() override { return false; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Ray-traced sun shadows for the loaded area (prototype).",
			{ "One switch: on = ray-traced sun shadows, off = the game's shadows exactly",
				"Static objects and landscape of the loaded cells",
				"Characters, trees and grass keep their shadow-map shadows",
				"Debug views: shadow source, RT vs shadow map, coverage" }
		};
	}

	/// Five tiers + Custom.
	enum class Quality : int
	{
		VeryLow = 0,
		Low = 1,
		Medium = 2,
		High = 3,
		Ultra = 4,
		Custom = 5,
		Count
	};

	enum class DebugView : int
	{
		Off = 0,
		ShadowSource,
		Disagreement,
		Coverage,
		RawVisibility,
		HitDepth,  // (45e) ray-traced first hit vs the game's depth
		PenumbraRadius,  // (45k) Denoiser Penumbra: filter radius
		HistoryLength,   // (45k) Denoiser Penumbra: history length
		AdaptiveRays,    // (45k) pixels with adaptive extra rays
		RtCasterMask,    // (45k) Screen Space Shadows "non-RT casters only": traced casters
		DistanceQuality, // (45l) fall-off and pattern-filled pixels
		CharacterRays,   // (45l) where the character ray ran
		Count
	};

	/// (45k) Shadow denoiser. Stored as an int; the values are kept (1 was Temporal, removed in 45o:
	/// a saved 1 loads as Penumbra).
	enum class DenoiserMode : int
	{
		Off = 0,       // the small Combine blur only
		Penumbra = 2,  // (default) penumbra-aware spatial filter + clamped temporal + adaptive rays
		DLSSRR = 3,    // nothing of ours: raw animated noise for DLSS Ray Reconstruction
		Sigma = 4,     // NVIDIA NRD SIGMA shadow denoiser
		Count
	};

	struct Settings
	{
		bool Enabled = false;
		int QualityLevel = static_cast<int>(Quality::Medium);
		// Per kind of object: ray traced (true) or the shadow map + screen-space shadows.
		bool Characters = true;
		bool PhysicsParts = true;  // HDT-SMP / HDT-PE parts other than hair (capes, skirts)
		int SettingsVersion = 5;   // 5 = 45o (debug switches hard-wired; no migrations before it)
		bool Buildings = true;
		bool Objects = true;
		bool Trees = false;
		int Foliage = 0;           // RayTracingScene::FoliageMode
		bool Cutouts = false;      // fences, banners, grilles: traced with their cut-out
		bool Hair = false;
		bool FirstPersonPlayer = false;  // the player stays traced in first person
		int Denoiser = static_cast<int>(DenoiserMode::Penumbra);
		float RtContactScale = 8.0f;  // Screen Space Shadows: contact range of traced casters, x the ray start offset
		// Quality "Custom": what the tiers set.
		int CustomBaseRays = 1;
		int CustomExtraRays = 1;
		float CustomSunCone = 0.27f;        // degrees, half angle
		float CustomFalloffStart = 4096.0f; // units
		float CustomFalloffEnd = 12288.0f;
		int CustomFarQuality = 0;           // beyond the fall-off end: 0 full, 1 checkerboard, 2 half
		int CustomTraceResolution = 0;      // everywhere: 0 full, 1 checkerboard, 2 half
		int CustomNpcQuality = 1;           // 0 full, 1 reduced
		float CustomDenoiseRadius = 16.0f;  // px
		int CustomDenoiseTaps = 4;          // per side
		float CustomDenoiseClamp = 1.5f;    // history clamp, sigmas
		int CustomHistory = 12;             // frames
		bool CustomDenoiseHalf = true;
		float CustomFarDenoiseScale = 0.75f;
		// NRD SIGMA: frames of its temporal stabilisation (0 = none: least lag on moving shadows).
		int SigmaStabilizedFrames = 4;
	};
	Settings settings;
	DebugView debugView = DebugView::Off;  // not persisted

	enum class Status
	{
		Off,
		Unavailable,   // initialization failed; reason in statusReason
		Failed,        // a runtime failure (device removed, D3D12 error); reason in statusReason
		Inactive,      // interior / no sky / loading: nothing traced this frame
		Waiting,       // on, nothing captured yet
		Active
	};

	virtual void DrawSettings() override;
	virtual void LoadSettings(json& o_json) override;
	virtual void SaveSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void SetupResources() override;
	virtual void ClearShaderCache() override;
	virtual void DataLoaded() override;
	virtual void Prepass() override;

	/// Deferred::Hooks::Main_RenderShadowMaps, before the engine draws any shadow map: decides
	/// whether this frame's sun cascades leave out the covered casters.
	void LatchFrame();
	/// Deferred::EndDeferred, after the deferred composite: the debug views (no-op when off).
	void DrawDebugOverlay();
	/// A loading screen opened (any thread): every captured cell is dropped on the next frame.
	/// (45k) a_open false: it closed (the warm-up starts).
	void OnLoadingScreen(bool a_open);
	/// (45k) Screen Space Shadows Prepass: true = Ray Tracing draws them this frame (non-RT casters only,
	/// after its trace, with its caster mask); it then always calls ScreenSpaceShadows::DrawDeferred once.
	bool ClaimScreenSpaceShadows();
	/// (45l) Deferred::PrepassPasses, after every feature's Prepass: the D3D11 side of this frame's trace
	/// (wait, Screen Space Shadows, denoiser, combine) when Late Sync deferred it. No-op otherwise.
	void FinishPrepass();

	/// True when this frame's shadow mask is (being) ray traced.
	[[nodiscard]] bool IsActiveThisFrame() const { return activeThisFrame; }
	[[nodiscard]] Status GetStatus() const { return status; }

	/// For the F7 frame trace: D3D12 GPU ms (last completed), CPU ms this frame, BLAS builds this frame.
	struct TraceSample
	{
		float gpuMs = -1.0f;
		float cpuMs = -1.0f;
		std::uint32_t builds = 0;
		std::uint32_t instances = 0;
	};
	[[nodiscard]] TraceSample GetTraceSample() const { return traceSample; }
	/// (45j) One line of the switches that matter for a profile (trace / F12 JSON headers).
	std::string DescribeForProfile() const;
	nlohmann::json DescribeJson() const;

private:
	struct alignas(16) CombineCB
	{
		float InvViewProj[16];
		float Rect[4];       // camera-relative xy min / max of the ray-traced area
		float Band[4];       // x hand-off band width, y filter radius (px), z flags (bits), w temporal blend
		std::uint32_t Size[4];  // render width, height, debug view, frame
		float PrevViewProj[16];  // (45j) last combined frame's CameraViewProj
		float CameraDelta[4];    // (45j) camera position now minus then
		float Denoise[4];        // (45k) tan(cone), max radius px, max history, clamp sigmas
		float Extra[4];          // (45k) fade-in, blocker range, unused, (45l) max filter taps per side
		float Falloff[4];        // (45l) fall-off start, end, filter radius scale beyond the end, far pattern
		float ViewProjUnjittered[16];      // (45l) SIGMA motion vectors
		float PrevViewProjUnjittered[16];  // (45l)
		float CameraData[4];     // (45l) SIGMA viewZ (Util::GetCameraData)
		float SunDirection[4];   // (45l) to the sun, tan(cone)
	};
	STATIC_ASSERT_ALIGNAS_16(CombineCB);

	/// (45l) What a quality tier (or Custom) sets.
	struct QualityParams
	{
		std::uint32_t baseRays = 1, extraRays = 1;
		float coneDegrees = 0.27f;
		std::uint32_t smallFilterRadius = 1;    // the small Combine filter (Off)
		std::uint32_t tracePattern = 0, farPattern = 0;  // 0 full, 1 checkerboard, 2 half
		float falloffStart = 0.0f, falloffEnd = 0.0f;  // end <= start: no fall-off
		bool npcReduced = false;
		float denoiseRadius = 16.0f;
		std::uint32_t denoiseTaps = 6;
		bool denoiseHalf = false;
		float farDenoiseScale = 1.0f;
		float history = 12.0f;
		float clampSigma = 1.5f;
	};
	QualityParams ResolveQuality() const;
	void CopyTierToCustom(int a_tier);
	/// (45l) Upscaling's Ray Reconstruction is running (DLSS, RR switched on and available).
	static bool UpscalingRRActive();
	/// (45l) The denoiser in effect: DLSS RR whenever Upscaling's Ray Reconstruction runs, else the chosen one.
	[[nodiscard]] DenoiserMode EffectiveDenoiser() const;

	bool EnsureBackend();
	bool EnsureD3D11Resources(const D3D11_TEXTURE2D_DESC& a_maskDesc);
	void ReleaseD3D11Resources();
	void Disable(Status a_status, std::string a_reason);
	void RunFrame();
	bool ComputeRects(float o_traceRect[4], float o_coreRect[4], double a_camX, double a_camY);

	ID3D11ComputeShader* GetShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name);

	RayTracingRT::Backend backend;
	RayTracingScene::Scene scene;
	RayTracingCharacters::Characters characters;  // (45i)
	std::vector<RayTracingRT::SkinDraw> skinDraws;
	std::vector<float> skinBones;
	std::vector<RayTracingRT::Instance> instances;

	winrt::com_ptr<ID3D11ComputeShader> prepareCS;
	winrt::com_ptr<ID3D11ComputeShader> combineCS;
	winrt::com_ptr<ID3D11ComputeShader> overlayCS;
	winrt::com_ptr<ID3D11ComputeShader> alphaMaskCS;  // (45h) 256x256 alpha copies
	ID3D11ComputeShader* GetShaderDefined(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const wchar_t* a_path, const char* a_name, const char* a_define);
	bool alphaMaskRgba = false;                        // the variant alphaMaskCS was compiled as
	std::unique_ptr<ConstantBuffer> combineCB;
	winrt::com_ptr<ID3D11Texture2D> maskTemp;  // the combined mask before it is copied over the engine's
	winrt::com_ptr<ID3D11UnorderedAccessView> maskTempUAV;
	winrt::com_ptr<ID3D11Texture2D> debugColor;  // debug view colour + opacity (RGBA8), written by CombineCS
	winrt::com_ptr<ID3D11UnorderedAccessView> debugColorUAV;
	winrt::com_ptr<ID3D11ShaderResourceView> debugColorSRV;
	// Last combined frame's camera (history reprojection). (45l) The 45j history textures are gone: Temporal
	// uses the Penumbra history (History4) now.
	float prevViewProj[16]{};
	float prevViewProjUnjittered[16]{};  // (45l) SIGMA motion vectors
	double prevCamera[3]{};
	bool prevCameraValid = false;
	// (45k) Denoiser Penumbra: tiles (RGBA16F, 1/8 size), horizontal and final filter (RG16F), history ping-pong (RGBA16F).
	struct DenoiseTexture
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	};
	// (45l) denoiseHalfIn: the half-resolution filter input (RG16F); denoiseH / denoiseOut are half size when denoiseIsHalf.
	DenoiseTexture denoiseTiles, denoiseH, denoiseOut, denoiseHalfIn, history4[2];
	bool denoiseIsHalf = false;
	std::uint32_t history4Index = 0;
	bool history4Valid = false;
	std::uint64_t denoiseBytes = 0;
	bool EnsureDenoiser(bool a_half);
	void ReleaseDenoiser();
	winrt::com_ptr<ID3D11ComputeShader> denoiseTileCS, denoiseHCS, denoiseVCS, denoiseHalfHCS, denoiseHalfVCS;
	// (45l) Denoiser NRD SIGMA: its own NRD instance (not the SSRT ones) and its inputs / output.
	std::unique_ptr<NRDReblurIntegration> sigma;
	DenoiseTexture sigmaViewZ, sigmaNormal, sigmaMotion, sigmaPenumbra, sigmaOut;
	winrt::com_ptr<ID3D11ComputeShader> sigmaPrepCS;
	std::uint64_t sigmaBytes = 0;
	std::uint32_t sigmaFrame = 0;
	bool sigmaHistoryValid = false;
	std::string sigmaFault;
	float sigmaPrevWorldToView[16]{}, sigmaPrevProj[16]{};
	float sigmaPrevJitter[2]{};
	std::uint16_t sigmaPrevRect[2]{};
	bool EnsureSigma();
	void ReleaseSigma();
	bool RunSigma(const float a_toSun[3]);
	// (45l) The second half of a traced frame (the D3D11 wait for the trace, Screen Space Shadows, the
	// denoiser, the combine), run right after the trace (Late Sync off) or after every other prepass.
	struct PendingFrame
	{
		bool active = false;
		std::array<std::uint8_t, sizeof(CombineCB)> cbBytes{};  // the frame's CombineCB (bytes: no alignment padding here)
		std::uint32_t combineFlags = 0;
		std::uint32_t width = 0, height = 0;
		bool writeSSS = false, debug = false, penumbra = false, spatial = false, half = false, sigma = false, sssNonRT = false;
		float biasConstant = 0.0f, biasPerUnit = 0.0f;
		float toSun[3]{};
		double camera[3]{};
		float viewProj[16]{}, viewProjUnjittered[16]{};
	};
	PendingFrame pending;
	void FinishFrame();
	void ReportFrame();
	/// (45n) Early Prep (LatchFrame): characters and this frame's builds / skinning on the D3D12 queue.
	void RunEarlyPrep();
	bool earlyThisFrame = false;       // PrepareFrame succeeded this frame
	bool charactersUpdatedEarly = false;  // characters.Update / CollectDraws already ran this frame
	float earlyCpuMs = 0.0f;           // CPU of RunEarlyPrep this frame
	float earlyCharactersMs = 0.0f;
	std::int64_t frameStart = 0;
	float prepassCpuMs = 0.0f;
	QualityParams lastQuality{};
	DenoiserMode lastEffectiveDenoiser = DenoiserMode::Off;
	// (45k) Screen Space Shadows drawn by us this frame (non-RT casters only).
	bool sssClaimed = false;
	// (45k) Loading screens and the warm-up after them.
	std::atomic<bool> loadingOpen{ false };
	std::atomic<bool> loadingClosed{ false };  // set by the menu event, consumed on the render thread
	enum class Phase
	{
		Play,
		Loading,
		WarmUp
	};
	Phase phase = Phase::Play;
	std::int64_t warmUpStart = 0;      // QPC when the loading screen closed
	float fade = 1.0f;                 // RT result fade-in after a loading screen
	std::uint32_t loadShapesAtOpen = 0, loadShapesAtClose = 0, loadBuildsAtOpen = 0, loadBuildsAtClose = 0;
	float lastTimeToReadyMs = -1.0f;
	std::string lastLoadingSummary;
	void UpdatePhase();
	/// Off, Penumbra, DLSS RR or SIGMA; anything else (the removed Temporal = 1) reads as Penumbra.
	[[nodiscard]] static int ValidDenoiser(int a_value)
	{
		return (a_value == 0 || (a_value >= 2 && a_value < static_cast<int>(DenoiserMode::Count))) ? a_value : static_cast<int>(DenoiserMode::Penumbra);
	}
	[[nodiscard]] DenoiserMode CurrentDenoiser() const { return static_cast<DenoiserMode>(ValidDenoiser(settings.Denoiser)); }
	/// (45o) Picking a denoiser in the dropdown: DLSS RR switches Upscaling's Ray Reconstruction on, any other
	/// one switches it off while it is on (the same setting as Upscaling's checkbox).
	void OnDenoiserPicked();
	// (45j) CPU split of the last frame (also in the F7 trace), smoothed and worst for the page.
	struct CpuSplit
	{
		float total = 0, scene = 0, characters = 0, instances = 0, missed = 0, execute = 0, d3d11 = 0, endFrame = 0;
	};
	CpuSplit cpuLast{}, cpuSmoothed{}, cpuWorst{};
	void PublishCpuSplit();
	RayTracingScene::SceneConfig SceneConfigFromSettings() const;
	D3D11_TEXTURE2D_DESC maskDesc{};
	std::uint64_t d3d11Bytes = 0;
	bool typedUavLoadR8 = false;

	Status status = Status::Off;
	std::string statusReason;
	std::string lastFailureReason;  // shown again if the start-up is not retried (45d)
	bool backendTried = false;
	bool activeThisFrame = false;
	bool combinedThisFrame = false;
	bool filterLatched = false;
	bool loadingFlushRegistered = false;
	std::uint32_t frameIndex = 0;
	std::uint32_t lastOverlayFrame = UINT32_MAX;
	float filterRect[4]{};
	float lastCpuMs = 0.0f;
	TraceSample traceSample{};
	std::uint32_t latchedWithoutTrace = 0;  // (45e) frames whose shadow map left casters out but that could not trace (should stay 0)
	bool overVramBudget = false;
	Status loggedStatus = Status::Off;
	std::int64_t lastTlasLog = 0;  // (45f) QPC of the last TLAS summary in the log (0 = log on the next active frame)
	// (45h) Alpha-tested casters (45j: Foliage / Cut-outs): as last seen (a change by the user clears an
	// old fault) and why they were switched off automatically (shown on the page).
	bool alphaRequested = false;
	std::string alphaOffReason;
	void UpdateAlphaCasters();
	// (45i) Characters: likewise.
	bool charactersRequested = false;
	std::string charactersOffReason;
	void UpdateCharacters();
};
