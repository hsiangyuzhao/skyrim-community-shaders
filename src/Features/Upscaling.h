#pragma once

#include "Feature.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/HDROutput.h"
#include "Upscaling/RCAS/RCAS.h"
#include "Upscaling/Streamline.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <winrt/base.h>

/**
 * @brief Provides upscaling functionality including DLSS, FSR and TAA.
 *
 * This feature handles various upscaling methods and frame generation technologies
 * to improve performance while maintaining visual quality.
 */
struct Upscaling : Feature
{
public:
	// Feature interface
	virtual inline std::string GetName() override { return "Upscaling"; }
	virtual inline std::string GetShortName() override { return "Upscaling"; }
	virtual inline bool SupportsVR() override { return true; }
	virtual inline bool IsCore() const override { return false; }
	virtual inline std::string_view GetCategory() const override { return "Display"; }

	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override
	{
		return {
			"Advanced upscaling and frame generation technologies for improved performance",
			{ "DLSS (Deep Learning Super Sampling) support",
				"FSR (FidelityFX Super Resolution) support",
				"TAA (Temporal Anti-Aliasing) support",
				"Frame generation for supported systems" }
		};
	}

	float2 jitter = { 0, 0 };

	enum class UpscaleMethod
	{
		kNONE,
		kTAA,
		kFSR,
		kDLSS
	};

	// Stored in SettingsUser.json. Keep existing values stable and append new
	// backends. The old frameGenerationMode setting remains the enable/disable
	// switch; a missing frameGenerationBackend therefore preserves the legacy
	// FSR 3 frame-generation behavior.
	enum class FrameGenerationBackend : uint
	{
		kFSR3FG = 0,
		kDLSSG = 1,
		kCount
	};

	// Stored in SettingsUser.json. Keep existing values stable and append new ones.
	enum class DLSSModelPreset : uint
	{
		kF = 0,
		kJ = 1,
		kK = 2,
		kL = 3,
		kM = 4,
		kSDKDocumentedMapping = 5,
		kCount
	};

	// Stored in SettingsUser.json. Keep existing values stable and append new ones.
	//
	// Ray Reconstruction carries its own model presets, a separate enum from the super
	// resolution ones above (sl::DLSSDPreset, not sl::DLSSPreset). Skyrim is not an official
	// DLSS title, so it has no driver profile and the NVIDIA App's DLSS Override cannot reach
	// it -- selecting the model here is the only way to pick one.
	//
	// Only two Ray Reconstruction models actually exist in the current SDK. Verified against
	// Streamline main, not just our local 2.12 copy -- the enum and its comments are identical
	// in both:
	//   ePresetD = 4   "Default model (transformer)"
	//   ePresetE = 5   "Latest transformer model (must use if DoF guide is needed)"
	//   ePresetF = 6 through ePresetO = 15   all "Reverts to default"
	// and the programming guide states ePresetA through ePresetC are no longer available.
	//
	// kD, kE and kF are offered in the UI (45k: E added on user request, so it can be compared
	// in game; the NVIDIA App itself only exposes Recommended and F, and the header calling E
	// "latest" says nothing about its cost). kDefault keeps its value so a saved profile is never
	// silently remapped onto a different model; LoadSettings coerces it to kD.
	//
	// F is the DLSS 4.5 model. It is official as of the DLSS 310.9.1 / Streamline 2.14.1 SDKs
	// shipped here: their sl_dlss_d.h calls ePresetF "Latest and default transformer model",
	// where 2.12's still called it "Reverts to default", and lists A through C as superseded by
	// "D, E or F". The weights live in nvngx_dlssd.dll rather than in Streamline, and requesting
	// a preset the shipped DLL has no weights for falls back to its default silently with
	// nothing in the API reporting it -- which is why SetDLSSRROptions logs the requested value,
	// and why that log line stays useful even now that the runtime is first-party.
	enum class DLSSDModelPreset : uint
	{
		kDefault = 0,
		kD = 1,
		kE = 2,
		kF = 3,
		kCount
	};

	// DLSS Neural Rendering (NGX feature 18). A post-tonemap, pre-UI pass that re-lights the
	// finished LDR frame; it reads no G-buffer and feeds nothing back, so it neither conflicts
	// with nor cooperates with the lighting features -- it sits on top of the result.
	//
	// Off by default and inert without a runtime: the model lives in nvngx_dlssnr.dll, which is
	// deliberately not shipped here. NVIDIA's own builds are Blackwell-only; running on anything
	// older needs a community-recompiled DLL, and neither is ours to redistribute.
	struct NeuralRenderingSettings
	{
		bool enabled = false;
		float intensity = 0.8f;
		float localToneStrength = 0.75f;
		float localStructureStrength = 0.9f;
		float skinStructureStrength = 0.9f;
		uint style = 3;
		bool useAutoMask = true;
		bool uiCorrection = true;  // (45a) pinned on
		// (batch 38a) The four below only act while Advanced > Batch 38 is on; off restores 37c.
		// Run alongside frame generation (DLSS-G or FSR 3). 37c blocked the pass outright.
		bool allowWithFrameGeneration = true;
		// Run on the HDR scene at render resolution, before DLSS upscales it, instead of on the
		// finished output-resolution image. Default off = 37c's place in the frame.
		bool runBeforeUpscaling = false;
		// (batch 42a) Before upscaling only now; after upscaling has modelResolutionPercentAfter.
		// The network's working resolution as a percentage of its input: 100, 67, 50 or 40 (a 75
		// saved by 38c-41b reads as 67). Cost falls
		// roughly with the pixel count. (batch 38c) Our own downscale: the DLL ignores
		// DLSSNR.ScalingRatio, which is what 38a relied on. Only the network's edit is carried back
		// up (depth-aware); the frame's own detail stays full resolution.
		uint modelResolutionPercent = 100;
		// Before upscaling only: add the camera-jitter step to the motion vectors the network
		// reads, so its history lines up with the jittered image it is given.
		bool jitterAwareMotion = true;
		// (batch 38c) The rest act only while Advanced > Batch 38 is on.
		// Pad the network's extent to a multiple of 8 (edge repeated), then crop. Balanced's
		// 2227x1253 was the one extent the network got that is not, and the one that looked different.
		bool padToNetworkGrid = true;
		// Before upscaling only: show the network the frame as the post-processing chain will
		// grade it (its auto exposure and colour grading / tone curve) instead of 38a's flat wrap.
		bool toneMatchedInput = true;
		// Before upscaling only: format the network reads. 0 = 8-bit (38a), 1 = 10-bit, 2 = 16-bit float.
		uint inputPrecision = 0;
		// Lock the picture's low-frequency brightness and colour to the input, 0..1, per placement.
		float tonePreservationBefore = 0.5f;
		float tonePreservationAfter = 0.0f;
		// Give the model its tuning (intensity, strengths, style...) when the feature is created:
		// it reads them only then. 38a set them per frame only, where they were ignored.
		bool tuningAtCreate = true;
		// (batch 42a) Model resolution after upscaling, separate from the before-upscaling one above
		// (modelResolutionPercent): 40, 50, 67 or 100. 67 is exactly 2/3 (2560x1440 on a 4K frame,
		// the same network size as Quality before upscaling). Default 50: the network reads 1920x1080
		// of the finished 4K image, about a quarter of the full-size cost.
		uint modelResolutionPercentAfter = 50;
		// (batch 42a) How the network's edit is carried back up below 100%: 0 = additive (41b),
		// 1 = log-ratio split (a smooth, depth- and colour-guided lighting part plus detail only
		// where the small and the full image agree). FinalizeCS.hlsl.
		uint residualMode = 1;
		// (batch 42a, experimental) Hand the network a DLSSNR.ControlMask marking the sky (depth at
		// the far plane, so the moons too). What the mask's values mean is undocumented, hence the
		// polarity: maskSkyValue is the value written for sky texels (the rest get the other one).
		bool protectSky = false;
		uint maskSkyValue = 1;
	};

	struct Settings
	{
		uint upscaleMethod = (uint)UpscaleMethod::kDLSS;
		uint upscaleMethodNoDLSS = (uint)UpscaleMethod::kFSR;
		uint qualityMode = 1;  // Default to Quality (1=Quality, 2=Balanced, 3=Performance, 4=Ultra Performance, 0=Native AA)
		uint frameGenerationMode = 1;
		uint frameGenerationBackend = static_cast<uint>(FrameGenerationBackend::kFSR3FG);
		uint frameGenerationForceEnable = 0;
		// DLSS-G presentation multiplier: presented frames per rendered frame (2 = one generated
		// frame, the only mode before multi-frame generation). The saved value is a preference;
		// what actually runs is clamped to what DLSS-G reports this session, without rewriting
		// this, so a session that reports less does not erase the choice for the next one.
		uint frameGenerationMultiplier = 2;
		// Keeps generation running while a paused menu is open. Off by default because a menu
		// is where the added input latency is most noticeable and the smoothness least useful.
		bool frameGenerationAllowInMenus = false;
		uint streamlineLogLevel = 0;  // 0=Off, 1=Default, 2=Verbose
		float sharpnessFSR = 1.0f;
		// DLSS output sharpening (RCAS) strength 0..1, mapped like FSR's slider: con = exp2(2s - 2). 0 = off.
		// (45o) The Off / RCAS / NIS choice and NIS are gone; LoadSettings migrates a profile that has them.
		float sharpnessRCAS = 0.2f;
		uint DLSSPreset = static_cast<uint>(DLSSModelPreset::kK);
		bool enableDLSSRR = false;
		// kD reproduces what was hardcoded before this setting existed, so an existing profile
		// keeps its exact behaviour.
		uint DLSSDPreset = static_cast<uint>(DLSSDModelPreset::kD);

		// NVIDIA Reflex. 0 = Off, 1 = On, 2 = On + Boost, matching sl::ReflexMode. The
		// default reproduces what was hardcoded at initialization before this was exposed.
		uint reflexMode = 1;
		// Frames per second as they reach the display, 0 to leave uncapped. This is Reflex's
		// own limiter, not the spin below Present: it is applied before input is sampled and
		// the driver is aware of it, which is what makes it the correct cap to pair with a
		// variable refresh rate.
		//
		// It caps the *presented* rate, not the rendered one. The driver knows the generation
		// multiplier, so with DLSS-G running a cap of 60 renders 30 and presents 60, and the
		// same cap in a paused menu with generation off renders and presents 60. Measured in
		// game, against an earlier assumption here that it capped rendered frames.
		//
		// Multi-frame generation is expected to keep that meaning with nothing changed on this
		// side: frameLimitUs reaches the driver's sleep mode unscaled (sl.chi setSleepMode), and
		// the multiplier reaches the driver on a separate channel (sl.chi setReflexSyncFG, the
		// fgMultiplier of NvAPI_D3D_SetReflexSync), so a cap of 240 at 4x should render 60 and
		// present 240. The value is therefore never divided by the multiplier here; only the
		// "rendered" figure shown in the UI is. Measured at 2x only; >2x is still to be confirmed.
		//
		// (45o) The single frame-rate cap. 0 = the automatic cap (GetEffectiveReflexFrameLimit). Without
		// Reflex the spin limiter after Present keeps the same cap (FrameLimiter), never both at once.
		uint reflexFrameLimit = 0;
		// (45m, hard-wired in 45o) Reflex is attached to the D3D12 interop device and cannot see the D3D11
		// rendering (reviews/diag-reflex-2026-10.md): latency markers sit at the engine's render call (old
		// placement only when the engine probes are missing) and the GPU-aware frame-start pacer runs.
		// Extra CPU time the pacer keeps in hand (ms). More = safer for frame rate, less = lower lag.
		float reflexPacerMarginMs = 1.0f;
		// (46n) Time the frame spent blocked inside D3D11 waiting for the GPU (Map / Flush / GetData) moves to the frame-start
		// sleep (learned over a few frames), and is not counted as CPU work the pacer must keep in hand.
		bool reflexPacerStallFeedback = true;
		// (46o) F1a: the main thread's wait in the render segments (wall - thread CPU time) counts as a wait for the GPU too.
		bool reflexPacerHiddenWaits = true;
		// (46o) F1b: when the pacer stays locked out (no sleep for 30 frames while the GPU is behind), its reserved lead shrinks.
		bool reflexPacerBackstop = true;
		// (46p) The frame-start sleep ends once the previous frame is done on the GPU (+ slack); learned sleep that was cut, or frames
		// slower than the GPU period, decay it fast; a frame right after a GPU spike does not feed it. Off = 46o.
		bool reflexPacerNoOverSleep = true;
		// (46p) The predicted completion from the median phase of the last GPU completions instead of the last one alone. Off = 46o.
		bool reflexPacerSmoothPhase = false;
		// (46p) GPU memory and copies (Advanced / Diagnostics > Save GPU Memory); each off = 46o.
		bool keepGpuMemoryResident = true;   // our RT D3D12 resources HIGH residency priority, our D3D11 render targets / UAVs HIGH eviction priority
		bool saveVramRROnly = true;          // DLSS RR-only interop textures only while RR is on (A2)
		bool saveVramSsgi = true;            // SSGI's indirect-light textures only while GI / specular GI is on (A1)
		bool saveVramFreeRtWhenOff = true;   // Ray Tracing switched off / a denoiser not in use: its textures and buffers go (A8)
		bool saveVramFreeFgWhenOff = false;  // frame generation switched off in this menu: DLSS-G's resources go (A9; off by default: experiment)
		bool saveVramUpscalerMasks = true;   // the D3D11 reactive / transparency masks only for FSR (A7)
		bool saveVramNrdShared = true;       // NRD instances share their transient (scratch) pool textures (A5; from the next pool creation)
		bool copyRenderedAreaOnly = true;    // per-frame interop copies only of the render rect (item 7)
		bool precompileLazyShaders = true;   // first-use shaders compiled behind the first loading screen (item 9)

		NeuralRenderingSettings neuralRendering;
	};

	Settings settings;

	// Bounds for Settings::frameGenerationMultiplier. 6x is the highest multiplier the shipped
	// sl.dlss_g can be asked for (numFramesToGenerateMax is compiled in at 5); the per-session
	// ceiling comes from Streamline and is usually lower.
	static constexpr uint kMinFrameGenerationMultiplier = 2;
	static constexpr uint kMaxFrameGenerationMultiplier = 6;

	struct JitterCB
	{
		float2 jitter;
		float2 pad0;
	};

	struct UpscalingDataCB
	{
		float2 trueSamplingDim;  // BufferDim.xy * ResolutionScale
		float2 pad0;
	};

	std::unique_ptr<ConstantBuffer> jitterCB;
	std::unique_ptr<ConstantBuffer> upscalingDataCB;

	// Runtime state
	bool isWindowed = false;
	bool lowRefreshRate = false;
	bool fidelityFXMissing = false;
	bool d3d12SwapChainActive = false;
	bool frameGenerationBackendLatched = false;
	bool frameGenerationEnabledAtStartup = false;
	FrameGenerationBackend frameGenerationBackendAtStartup = FrameGenerationBackend::kFSR3FG;

	// Last values Streamline accepted, so ApplyReflexSettings can run per frame without
	// calling into the plugin on frames where nothing moved. UINT_MAX is a value neither
	// setting can hold, which makes "nothing applied yet" distinguishable from "applied 0".
	uint reflexModeApplied = UINT_MAX;
	uint reflexFrameLimitApplied = UINT_MAX;

	// Timing and scaling
	double refreshRate = 0.0f;
	float2 resolutionScale = { 1.0f, 1.0f };
	LARGE_INTEGER qpf;

	// FG FPS Measurement for Overlay
	bool IsFrameGenerationActive() const;
	float GetFrameGenerationFrameTime() const;
	/// @brief Measured presented-frames-per-rendered-frame, or 0 when unavailable (see DX12SwapChain).
	float GetFrameGenerationPresentMultiplier() const;
	/// @brief The multiplier the backend is configured to run, as it reported it -- not the setting.
	///
	/// DLSS-G: the numFramesToGenerate Streamline last accepted, plus one. FSR 3: always 2.
	/// Used wherever a rate has to be converted between rendered and presented frames.
	uint GetFrameGenerationAppliedMultiplier() const;
	/// @brief numFramesToGenerate requested by the saved multiplier (before the session ceiling).
	uint32_t GetRequestedDLSSGFramesToGenerate() const;
	bool IsUpscalingActive();

	// Feature interface overrides
	virtual void DrawSettings() override;
	virtual void SaveSettings(json& o_json) override;
	/// (41a) The settings object only, without SaveSettings' bUseTAA write to SkyrimPrefs.ini (A/B Compare snapshots).
	void SaveSettingsValues(json& o_json) const;
	virtual void LoadSettings(json& o_json) override;
	virtual void RestoreDefaultSettings() override;
	virtual void DataLoaded() override;

	/**
	 * @brief Installs Direct3D-related hooks for device and factory creation.
	 *
	 * Loads FidelityFX support and patches the import address table (IAT) to redirect D3D11 device and DXGI factory creation functions to custom hook implementations.
	**/
	virtual void Load() override;
	virtual void PostPostLoad() override;
	virtual void SetupResources() override;

	UpscaleMethod GetUpscaleMethod();

	void CheckResources(UpscaleMethod a_upscalemethod);
	void CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod, bool a_enableDLSSRR);
	void DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod);

	winrt::com_ptr<ID3D11ComputeShader> encodeTexturesCS[5];  // One for each UpscaleMethod
	ID3D11ComputeShader* GetEncodeTexturesCS();

	winrt::com_ptr<ID3D11PixelShader> depthRefractionUpscalePS;
	ID3D11PixelShader* GetDepthRefractionUpscalePS();

	winrt::com_ptr<ID3D11PixelShader> underwaterMaskUpscalePS;
	ID3D11PixelShader* GetUnderwaterMaskUpscalePS();

	winrt::com_ptr<ID3D11VertexShader> upscaleVS;
	ID3D11VertexShader* GetUpscaleVS();

	winrt::com_ptr<ID3D11DepthStencilState> upscaleDepthStencilState;
	winrt::com_ptr<ID3D11BlendState> upscaleBlendState;
	winrt::com_ptr<ID3D11RasterizerState> upscaleRasterizerState;

	void ConfigureTAA();
	void ConfigureUpscaling(RE::BSGraphics::State* a_state);
	void Upscale();

	// D3D11 textures
	std::unique_ptr<Texture2D> reactiveMaskTexture;
	std::unique_ptr<Texture2D> transparencyCompositionMaskTexture;
	std::unique_ptr<Texture2D> motionVectorCopyTexture;

	virtual void ClearShaderCache() override;

	// Static instances instead of singletons
	static inline Streamline streamline;
	static inline FidelityFX fidelityFX;  // Only for frame generation
	static inline DX12SwapChain dx12SwapChain;
	static inline RCAS rcas;  // (45a) DLSS output sharpening, HDR-safe

	winrt::com_ptr<ID3D11PixelShader> copyDepthToSharedBufferPS;

	float projectionPosScaleX = 0.0f;
	float projectionPosScaleY = 0.0f;

	float dynamicResolutionWidthRatio = 1.0f;
	float dynamicResolutionHeightRatio = 1.0f;

	bool previousUpscalingWasActive = false;

	void CopySharedD3D12Resources();
	/// (46p) Copy Only the Rendered Area: copies the render rect (+1 texel, clamped to the output size) of a_src into a_dst
	/// (output-sized colour textures, no depth-stencil / MSAA); a full CopyResource with the switch off.
	void CopyRenderedArea(ID3D11DeviceContext* a_context, ID3D11Resource* a_dst, ID3D11Resource* a_src) const;
	void PostDisplay();
	void PerformUpscaling();
	void UpscaleDepth();

	void SnapshotBeforeTransparency();

	static void TimerSleepQPC(int64_t targetQPC);

	void FrameLimiter();

	/// @brief Pushes settings.reflexMode and settings.reflexFrameLimit to Streamline.
	///
	/// Cheap to call every frame: it compares against what was last accepted and only crosses
	/// into Streamline when one of them has actually changed. Pass a_force at initialization,
	/// where the cache is empty but the call still has to happen so a failure can invalidate
	/// the feature.
	///
	/// @return false only when Streamline rejected the options, or Reflex is unavailable.
	bool ApplyReflexSettings(bool a_force = false);

	/// @brief Frame-start pacing on (always, except VR).
	bool IsLatencyPacerEnabled() const;
	/// @brief Latency markers at the engine's render call (always; EngineTiming falls back without its probes).
	bool WantsReflexMarkersAtRenderStart() const;
	/// @brief The cap actually used: the user's, or the automatic one when that is Off.
	uint GetEffectiveReflexFrameLimit() const;
	uint GetReflexModeApplied() const { return reflexModeApplied; }
	uint GetReflexFrameLimitApplied() const { return reflexFrameLimitApplied; }
	/// @brief (45m) One line for the frame-trace header: every Reflex and pacing setting in effect.
	std::string DescribeLatencyForTrace();

	static double GetRefreshRate(HWND a_window);

	// Unified interface methods - external code should use these instead of direct access
	void LoadUpscalingSDKs();  // Loads all SDKs at once
	void LatchFrameGenerationBackend();
	FrameGenerationBackend GetFrameGenerationBackend() const;
	FrameGenerationBackend GetConfiguredFrameGenerationBackend() const;
	bool IsFrameGenerationEnabled() const;
	/// @brief Whether this frame should generate, as opposed to whether the session is configured
	/// for it. Carries the live setting; only ever subtracts from IsFrameGenerationEnabled, because
	/// the proxy swapchain the on direction needs is created once at device creation.
	bool IsFrameGenerationRequestedNow() const;
	/// @brief Whether a paused frame should still generate, per Frame Generation in Menus.
	///
	/// Two places decide independently whether a paused frame is eligible -- the Present gate
	/// and the HUD-less capture that gate depends on -- and the first version of this setting
	/// changed only the former, so generation stayed held waiting for a snapshot that was never
	/// taken. They share this now.
	bool IsFrameGenerationAllowedWhilePaused() const;
	bool IsDLSSGBackend() const;
	bool IsDLSSGAvailable() const;
	bool IsDLSSGMapRenderingContext();
	void PresentFrameGeneration(bool a_useFrameGeneration, bool a_retainDLSSGResourcesWhenOff = true);
	void CheckFrameConstants();
	void SetUIBuffer();
	HANDLE GetFrameLatencyWaitableObject() const;
	float GetFrameTime() const;

	// Backend interface methods
	bool IsBackendInitialized() const;
	void CheckBackendFeatures(IDXGIAdapter* adapter);
	void UpgradeBackendInterface(void** ppInterface);
	void SetBackendD3DDevice(void* device);
	void PostBackendDevice();

	// Module availability methods
	bool HasFrameGenModule() const;

	// Proxy interface methods
	void SetProxyD3D11Device(ID3D11Device* device);
	void SetProxyD3D11DeviceContext(ID3D11DeviceContext* context);
	void CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);
	void CreateProxyInterop();
	IDXGISwapChain* GetProxySwapChain();

private:
	bool dlssGMapRenderingContextActive = false;

	// (45a) DrawSettings sections, in page order.
	void DrawUpscalingSettings();
	void DrawSharpeningSettings();
	void DrawFrameGenerationSettings();
	void DrawLatencySettings();
	void DrawNeuralRenderingSettings();
	void DrawAdvancedSettings();

	struct Main_UpdateJitter
	{
		static void thunk(RE::BSGraphics::State* a_state);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct MenuManagerDrawInterfaceStartHook
	{
		static void thunk(int64_t a1);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_PostProcessing
	{
		static void thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct SetScissorRect
	{
		static void thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom);
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct Main_RenderPrecipitation
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};

	struct BSFaceGenManager_UpdatePendingCustomizationTextures
	{
		static void thunk();
		static inline REL::Relocation<decltype(thunk)> func;
	};
};
