#pragma once

#include "../../Buffer.h"
#include "../../State.h"

#include <d3d11_4.h>
#include <d3d12.h>

#define NV_WINDOWS

#pragma warning(push)
#pragma warning(disable: 4471)
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_g.h>
#include <sl_dlss_d.h>
#include <sl_matrix_helpers.h>
#include <sl_nis.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <sl_version.h>
#pragma warning(pop)

class Streamline
{
public:
	static constexpr const wchar_t* PluginDir = L"Data\\Shaders\\Upscaling\\Streamline";

	Streamline() = default;

	inline std::string GetShortName() { return "Streamline"; }

	bool enabledAtBoot = false;
	bool initialized = false;
	bool deviceRegistered = false;
	bool triedInitialization = false;

	bool featureDLSS = false;
	bool featureDLSS_RR = false;
	bool featureNIS = false;
	bool featureDLSS_G = false;
	bool featureReflex = false;
	bool featurePCL = false;

	// DLSS-G is a startup-selected backend.  It must be selected before
	// LoadInterposer() so that the feature is loaded before the swap chain is
	// created; loading it after an FSR3 swap chain exists is not supported.
	bool dlssGBackendSelectedAtBoot = false;
	bool dlssGRuntimeFaulted = false;
	bool dlssGStateFallbackApplied = false;
	bool dlssGActive = false;
	bool dlssGOptionsInitialized = false;
	bool dlssGRetainResourcesWhenOff = true;
	bool dlssGFunctionsReady = false;
	bool reflexFunctionsReady = false;
	bool pclFunctionsReady = false;
	bool reflexSleepFailureLogged = false;
	bool pclMarkerFailureLogged = false;
	uint32_t dlssGConfiguredInputWidth = 0;
	uint32_t dlssGConfiguredInputHeight = 0;
	uint32_t dlssGConfiguredOutputWidth = 0;
	uint32_t dlssGConfiguredOutputHeight = 0;
	uint32_t dlssGTaggedInputWidth = 0;
	uint32_t dlssGTaggedInputHeight = 0;

	// Multi-frame generation. All counts here are Streamline's numFramesToGenerate, i.e. the
	// presentation multiplier minus one (1 = 2x, 2 = 3x, 3 = 4x).
	//
	// The ceiling is whatever slDLSSGGetState last reported in numFramesToGenerateMax; 0 means
	// no report has arrived yet, and until one does only 2x is requested. The applied count is
	// the value slDLSSGSetOptions last accepted, which is what the overlay reports as the
	// running mode. A rejected count is one that either SetOptions refused or that put DLSS-G
	// into an invalid runtime state; it is held at 2x until the user picks a multiplier again
	// or the reported ceiling changes, so a failing request is not retried every frame.
	uint32_t dlssGFramesToGenerateMax = 0;
	uint32_t dlssGAppliedFramesToGenerate = 1;
	uint32_t dlssGRejectedFramesToGenerate = 0;
	uint32_t dlssGCapabilityQueryCooldown = 0;
	bool dlssSRLocalTagsLogged = false;
	bool dlssRRLocalTagsLogged = false;

	sl::ViewportHandle viewport{ 0 };

	HMODULE interposer = NULL;

	// SL Interposer Functions
	PFun_slInit* slInit{};
	PFun_slShutdown* slShutdown{};
	PFun_slIsFeatureSupported* slIsFeatureSupported{};
	PFun_slIsFeatureLoaded* slIsFeatureLoaded{};
	PFun_slSetFeatureLoaded* slSetFeatureLoaded{};
	PFun_slEvaluateFeature* slEvaluateFeature{};
	PFun_slAllocateResources* slAllocateResources{};
	PFun_slFreeResources* slFreeResources{};
	PFun_slSetTag* slSetTag{};
	PFun_slSetTagForFrame* slSetTagForFrame{};
	PFun_slGetFeatureRequirements* slGetFeatureRequirements{};
	PFun_slGetFeatureVersion* slGetFeatureVersion{};
	PFun_slUpgradeInterface* slUpgradeInterface{};
	PFun_slSetConstants* slSetConstants{};
	PFun_slGetNativeInterface* slGetNativeInterface{};
	PFun_slGetFeatureFunction* slGetFeatureFunction{};
	PFun_slGetNewFrameToken* slGetNewFrameToken{};
	PFun_slSetD3DDevice* slSetD3DDevice{};

	// DLSS specific functions
	PFun_slDLSSGetOptimalSettings* slDLSSGetOptimalSettings{};
	PFun_slDLSSGetState* slDLSSGetState{};
	PFun_slDLSSSetOptions* slDLSSSetOptions{};

	// DLSS-G specific functions
	PFun_slDLSSGGetState* slDLSSGGetState{};
	PFun_slDLSSGSetOptions* slDLSSGSetOptions{};

	// Reflex specific functions
	PFun_slReflexGetState* slReflexGetState{};
	PFun_slReflexSleep* slReflexSleep{};
	PFun_slReflexSetOptions* slReflexSetOptions{};

	// PCL specific functions
	PFun_slPCLGetState* slPCLGetState{};
	PFun_slPCLSetMarker* slPCLSetMarker{};
	PFun_slPCLSetOptions* slPCLSetOptions{};

	// DLSSD specific functions
	PFun_slDLSSDGetOptimalSettings* slDLSSDGetOptimalSettings{};
	PFun_slDLSSDGetState* slDLSSDGetState{};
	PFun_slDLSSDSetOptions* slDLSSDSetOptions{};

	// NIS specific functions
	PFun_slNISSetOptions* slNISSetOptions{};
	PFun_slNISGetState* slNISGetState{};

	Util::FrameChecker frameChecker;
	sl::FrameToken* frameToken = nullptr;
	uint32_t frameTokenIndex = UINT32_MAX;
	bool frameConstantsValid = false;
	bool temporalResetRequested = false;

	/**
	 * @brief Native D3D12 resources consumed by DLSS-G at Present time.
	 *
	 * The state fields must describe the state at the point Streamline consumes
	 * each resource.  With manual hooking, passing the correct state is
	 * mandatory.  The default COMMON state matches the interop resources after
	 * the normal copy/transition path, but callers should override it when the
	 * resource is left in another state.
	 */
	struct DLSSGFrameResources
	{
		ID3D12Resource* depth = nullptr;
		ID3D12Resource* motionVectors = nullptr;
		ID3D12Resource* hudless = nullptr;
		ID3D12Resource* uiColorAndAlpha = nullptr;
		ID3D12Resource* uiAlpha = nullptr;

		D3D12_RESOURCE_STATES depthState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES motionVectorsState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES hudlessState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES uiColorAndAlphaState = D3D12_RESOURCE_STATE_COMMON;
		D3D12_RESOURCE_STATES uiAlphaState = D3D12_RESOURCE_STATE_COMMON;

		sl::Extent depthExtent{};
		sl::Extent motionVectorsExtent{};
		sl::Extent hudlessExtent{};
		sl::Extent uiColorAndAlphaExtent{};
		sl::Extent uiAlphaExtent{};
	};

	// Cached DLL version info for Streamline plugin directory
	static std::vector<std::pair<std::string, std::string>> dllVersions;

	void LoadInterposer();

	/**
	 * @brief Selects DLSS-G as the startup frame-generation backend.
	 *
	 * Must be called before LoadInterposer().  FSR3 and DLSS-G are separate
	 * swap-chain owners and are intentionally not hot-swappable in one process.
	 */
	void SelectDLSSGBackendAtBoot(bool a_selected);
	bool IsDLSSGBackendSelectedAtBoot() const { return dlssGBackendSelectedAtBoot; }
	bool IsDLSSGReady() const;
	bool IsDLSSGActive() const { return dlssGActive; }

	// Manual-hooking device/proxy helpers.  Callers must check the returned
	// result before using the converted interface.
	sl::Result SetD3DDevice(void* a_device);
	sl::Result UpgradeInterface(void** a_interface);
	sl::Result GetNativeInterface(void* a_proxyInterface, void** a_nativeInterface);

	void CheckFeatures(IDXGIAdapter* a_adapter);

	void PostDevice();

	bool BeginFrameToken();
	void CheckFrameConstants();
	void CheckFrameConstantsForLatchedFrame();
	void RequestTemporalReset();
	void SubmitFrameConstants();
	const sl::FrameToken* GetFrameToken() const { return frameConstantsValid ? frameToken : nullptr; }
	bool HasLatchedFrameToken() const { return frameConstantsValid && frameToken != nullptr; }
	uint32_t GetLatchedFrameTokenIndex() const { return HasLatchedFrameToken() ? frameTokenIndex : UINT32_MAX; }

	/**
	 * @brief Enables or disables non-VR DLSS-G in the current viewport.
	 *
	 * @param a_framesToGenerate Requested generated frames per rendered frame
	 *        (multiplier - 1). Clamped to the reported ceiling; a rejected
	 *        multi-frame request is retried once at 2x before the session-wide
	 *        fallback is considered. Ignored when disabling: eOff keeps the
	 *        count last accepted, so a menu suspend/resume changes only the mode.
	 *
	 * Must be called on the presenting thread. Changing the multiplier is a
	 * plain slDLSSGSetOptions update; no resources or swap chain are rebuilt.
	 * Turning it off retains plugin resources to avoid a pause/menu stutter;
	 * long-term shutdown can call DestroyDLSSGResources().
	 */
	bool SetDLSSGMode(bool a_enable, bool a_retainResourcesWhenOff = true, uint32_t a_framesToGenerate = 1);
	bool GetDLSSGState(sl::DLSSGState& a_state);

	/// @brief Queries numFramesToGenerateMax without judging the runtime status.
	///
	/// Unlike GetDLSSGState this never triggers the session fallback, so it is safe before the
	/// first generated Present. It does consume the numFramesActuallyPresented counter, so it is
	/// only called while generation is not running (the per-Present query covers that case).
	bool RefreshDLSSGCapabilities();
	/// @brief Highest numFramesToGenerate DLSS-G reported, or 0 before any report.
	uint32_t GetDLSSGFramesToGenerateMax() const { return dlssGFramesToGenerateMax; }
	/// @brief numFramesToGenerate last accepted by slDLSSGSetOptions (1 = 2x).
	uint32_t GetDLSSGAppliedFramesToGenerate() const { return dlssGAppliedFramesToGenerate; }
	/// @brief numFramesToGenerate that was rejected and degraded to 2x, or 0.
	uint32_t GetDLSSGRejectedFramesToGenerate() const { return dlssGRejectedFramesToGenerate; }
	/// @brief Lets a previously rejected multiplier be tried again (called when the user re-selects).
	void ClearDLSSGMultiFrameRejection() { dlssGRejectedFramesToGenerate = 0; }
	/// @brief The count SetDLSSGMode would request for a_requested right now.
	uint32_t ResolveDLSSGFramesToGenerate(uint32_t a_requested) const;
	bool TagDLSSGResources(const DLSSGFrameResources& a_resources, ID3D12GraphicsCommandList* a_commandList);
	void DestroyDLSSGResources(bool a_modeSwitch = true);

	// Reflex/PCL calls are intentionally exposed as small frame-token-bound
	// wrappers so game hooks can place markers without creating another token.
	bool GetReflexState(sl::ReflexState& a_state);
	bool SetReflexOptions(sl::ReflexMode a_mode, uint32_t a_frameLimitUs = 0);
	bool ReflexSleep();
	bool GetPCLState(sl::PCLState& a_state);
	bool SetPCLMarker(sl::PCLMarker a_marker);

	void SetDLSSOptions();
	void SetDLSSRROptions();

	void Upscale(ID3D12Resource* a_inputColorTexture,
		ID3D12Resource* a_motionVectorTexture,
		ID3D12Resource* a_depthTexture,
		ID3D12Resource* a_reactiveMask,
		ID3D12Resource* a_transparencyCompositionMask,
		ID3D12Resource* a_outputTexture,
		ID3D12GraphicsCommandList* a_commandList);

	void RayReconstruction(ID3D12Resource* a_inputColorTexture,
		ID3D12Resource* a_motionVectorTexture,
		ID3D12Resource* a_depthTexture,
		ID3D12Resource* a_albedoTexture,
		ID3D12Resource* a_reflectanceTexture,
		ID3D12Resource* a_normalRoughness,
		ID3D12Resource* a_specularHitDistance,
		ID3D12Resource* a_colorBeforeTransparency,
		ID3D12Resource* a_sssGuide,
		ID3D12Resource* a_outputTexture,
		ID3D12GraphicsCommandList* a_commandList);

	float2 GetInputResolutionScale(uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityPreset);
	float2 GetInputResolutionScaleRR(uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityPreset);

	void DestroyDLSSResources(bool modeSwitch = false);
	void DestroyDLSSRRResources(bool modeSwitch = false);

	void ApplyNISSharpening(ID3D12Resource* a_inputColorTexture, ID3D12Resource* a_outputTexture, float sharpness, ID3D12GraphicsCommandList* a_commandList);

private:
	void FallbackDLSSG(const char* a_reason, sl::Result a_result, sl::DLSSGStatus a_status);
	void UpdateDLSSGFramesToGenerateMax(uint32_t a_reportedMax);
	sl::Result SetTagsForCurrentFrame(const sl::ResourceTag* a_tags, uint32_t a_numTags, ID3D12GraphicsCommandList* a_commandList);
	sl::Result SetTagsForLatchedFrame(const sl::ResourceTag* a_tags, uint32_t a_numTags, ID3D12GraphicsCommandList* a_commandList);
};
