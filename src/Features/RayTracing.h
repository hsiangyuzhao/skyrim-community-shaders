#pragma once

#include "Buffer.h"
#include "Feature.h"

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

	enum class Quality : int
	{
		Low = 0,
		Medium = 1,
		High = 2
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
		Count
	};

	/// (45j) Screen Space Shadows inside the ray-traced area.
	enum class SSSMode : int
	{
		Combine = 0,  // kept; merged with the mask as min() (grass, leaves, characters keep their contact shadows)
		HandOff = 1,  // faded out (45i): the ray tracer alone (grass loses its contact shadows)
		Keep = 2,     // kept and multiplied as with Ray Tracing off (shared shadows get darker twice)
		NonRT = 3     // (45k, default) every caster the ray tracer does not see keeps its contact shadow; traced
		              // casters only within a short contact range (the gap the ray start offset leaves); merged as min()
	};

	/// (45k) Shadow denoiser. Stored as an int: keep the values; 4 and 5 are reserved for NRD SIGMA
	/// (45l) and AMD FidelityFX Shadows Denoiser.
	enum class DenoiserMode : int
	{
		Off = 0,       // 45j without Temporal: the small Combine blur only
		Temporal = 1,  // the 45j "Temporal smoothing" path exactly
		Penumbra = 2,  // (45k, default) penumbra-aware spatial filter + clamped temporal + adaptive rays
		DLSSRR = 3,    // nothing of ours: raw animated noise for DLSS Ray Reconstruction
		Count
	};

	/// (45k) Noise / adaptive rays: Auto = what the denoiser mode wants.
	enum class AutoMode : int
	{
		Auto = 0,
		First = 1,   // Noise: world-locked; adaptive rays: on
		Second = 2   // Noise: animated; adaptive rays: off
	};

	struct Settings
	{
		bool Enabled = false;
		int QualityLevel = static_cast<int>(Quality::Medium);
		// Debug A/B switches (persisted so a test survives a restart).
		bool KeepCastersInShadowMap = false;
		bool KeepScreenSpaceShadows = false;  // (45i key, read once by the 45j migration: true = SSS mode Keep)
		// (45g) Large one-sided alpha-tested shapes with an edge-only cutout (roof tiles) are RT occluders.
		bool CutoutAsOpaque = true;
		// (45h key) Alpha-tested casters: read once by the 45j migration (now Foliage / Cut-outs).
		bool AlphaCasters = false;
		// (45i, 45j) Per kind of object: ray traced (true) or the shadow map + screen-space shadows.
		bool Characters = true;
		bool PhysicsParts = true;  // HDT-SMP / HDT-PE parts other than hair (capes, skirts)
		bool KeepCharactersInShadowMap = false;  // debug A/B
		float CharacterSelfShadowOffset = 6.0f;  // debug: hits on characters nearer than this are ignored (units)
		// (45j) Ray Tracing > Advanced.
		int SettingsVersion = 3;  // 45j migration marker (absent in older JSON); 3 = 45k (SSS mode "non-RT casters only")
		bool Buildings = true;
		bool Objects = true;
		bool Trees = false;
		int Foliage = 0;           // RayTracingScene::FoliageMode
		bool Cutouts = false;      // fences, banners, grilles: traced with their cut-out
		bool Hair = false;
		bool FirstPersonPlayer = false;  // the player stays traced in first person
		int ScreenSpaceShadows = static_cast<int>(SSSMode::NonRT);
		bool KeepSSSUnderFoliage = true;  // hand-off mode: not under leaves
		// (45k) Denoiser / Noise (Auto, World-locked, Animated) / Adaptive Rays (Auto, On, Off).
		int Denoiser = static_cast<int>(DenoiserMode::Penumbra);
		int Noise = 0;
		int AdaptiveRays = 0;
		float RtContactScale = 8.0f;    // (45k) SSS "non-RT casters only": contact range of traced casters, x the ray start offset
		float RayOffsetScale = 1.0f;    // (45k) debug: the ray start offset (bias) x this
		bool FlushBeforeTrace = false;  // (45k) debug: 45j flushed D3D11 explicitly after the ready Signal
		bool LoadingBoost = true;       // (45k) capture and build with a large budget behind loading screens
		bool WarmUp = true;             // (45k) after a loading screen: shadow map kept until the near scene is built, RT fades in
		bool StableNormals = true;
		bool FineAlphaMips = true;
		// (45j) Game textures created shareable for ray-traced cut-outs (off: 256x256 copies only). Off by
		// default: every 45i trace (Ray Tracing on or off, in sessions started with Alpha-tested casters on)
		// moved the GPU wait from Present into a Map, the only always-on difference from 45d being the
		// thousands of shareable textures; under test as a suspect.
		bool ShareGameTextures = false;
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
		float Extra[4];          // (45k) fade-in, blocker range, unused
	};
	STATIC_ASSERT_ALIGNAS_16(CombineCB);

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
	// (45j) Temporal: ping-pong history (r visibility, g distance), last camera.
	winrt::com_ptr<ID3D11Texture2D> history[2];
	winrt::com_ptr<ID3D11ShaderResourceView> historySRV[2];
	winrt::com_ptr<ID3D11UnorderedAccessView> historyUAV[2];
	std::uint32_t historyIndex = 0;
	bool historyValid = false;
	float prevViewProj[16]{};
	double prevCamera[3]{};
	bool EnsureHistory();
	// (45k) Denoiser Penumbra: tiles (RGBA16F, 1/8 size), horizontal and final filter (RG16F), history ping-pong (RGBA16F).
	struct DenoiseTexture
	{
		winrt::com_ptr<ID3D11Texture2D> texture;
		winrt::com_ptr<ID3D11ShaderResourceView> srv;
		winrt::com_ptr<ID3D11UnorderedAccessView> uav;
	};
	DenoiseTexture denoiseTiles, denoiseH, denoiseOut, history4[2];
	std::uint32_t history4Index = 0;
	bool history4Valid = false;
	std::uint64_t denoiseBytes = 0;
	bool EnsureDenoiser();
	void ReleaseDenoiser();
	winrt::com_ptr<ID3D11ComputeShader> denoiseTileCS, denoiseHCS, denoiseVCS;
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
	[[nodiscard]] DenoiserMode CurrentDenoiser() const { return static_cast<DenoiserMode>(std::clamp(settings.Denoiser, 0, static_cast<int>(DenoiserMode::Count) - 1)); }
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
