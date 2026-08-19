#include "Streamline.h"

#include <dxgi.h>
#include <dxgi1_3.h>

#include "../../Deferred.h"
#include "../../Hooks.h"
#include "../../State.h"
#include "../../Util.h"
#include "../Upscaling.h"
#include "DX12SwapChain.h"

#include <limits>
#include <vector>

namespace
{
using DLSSModelPreset = Upscaling::DLSSModelPreset;

constexpr uint kDLSSModelPresetCount = static_cast<uint>(DLSSModelPreset::kCount);
constexpr uint kUnreportedPreset = std::numeric_limits<uint>::max();

const char* GetDLSSModelPresetName(DLSSModelPreset a_preset)
{
	switch (a_preset) {
	case DLSSModelPreset::kF:
		return "F";
	case DLSSModelPreset::kJ:
		return "J";
	case DLSSModelPreset::kK:
		return "K";
	case DLSSModelPreset::kL:
		return "L";
	case DLSSModelPreset::kM:
		return "M";
	case DLSSModelPreset::kSDKDocumentedMapping:
		return "Streamline 2.12 documented mapping (K/K/K/M/L)";
	default:
		return "K (safe fallback)";
	}
}

void SetAllDLSSPresets(sl::DLSSOptions& a_options, sl::DLSSPreset a_preset)
{
	a_options.dlaaPreset = a_preset;
	a_options.qualityPreset = a_preset;
	a_options.balancedPreset = a_preset;
	a_options.performancePreset = a_preset;
	a_options.ultraPerformancePreset = a_preset;
}

void SetSDKDocumentedDLSSPresets(sl::DLSSOptions& a_options)
{
	a_options.dlaaPreset = sl::DLSSPreset::ePresetK;
	a_options.qualityPreset = sl::DLSSPreset::ePresetK;
	a_options.balancedPreset = sl::DLSSPreset::ePresetK;
	a_options.performancePreset = sl::DLSSPreset::ePresetM;
	a_options.ultraPerformancePreset = sl::DLSSPreset::ePresetL;
}

sl::DLSSPreset GetForcedDLSSPreset(DLSSModelPreset a_preset)
{
	switch (a_preset) {
	case DLSSModelPreset::kF:
		return sl::DLSSPreset::ePresetF;
	case DLSSModelPreset::kJ:
		return sl::DLSSPreset::ePresetJ;
	case DLSSModelPreset::kL:
		return sl::DLSSPreset::ePresetL;
	case DLSSModelPreset::kM:
		return sl::DLSSPreset::ePresetM;
	case DLSSModelPreset::kK:
	default:
		return sl::DLSSPreset::ePresetK;
	}
}

template <class T>
bool LoadFeatureFunction(PFun_slGetFeatureFunction* a_getter, sl::Feature a_feature, const char* a_name, T*& a_target, sl::Result& a_result)
{
	void* function = nullptr;
	a_result = a_getter(a_feature, a_name, function);
	if (a_result != sl::Result::eOk || function == nullptr) {
		a_target = nullptr;
		return false;
	}

	a_target = reinterpret_cast<T*>(function);
	return true;
}
}

void LoggingCallback(sl::LogType type, const char* msg)
{
	// Remove trailing newlines from the raw message
	std::string rawMsg(msg);
	while (!rawMsg.empty() && (rawMsg.back() == '\n' || rawMsg.back() == '\r'))
		rawMsg.pop_back();

	// Remove leading bracketed metadata
	const char* p = msg;
	while (*p == '[') {
		const char* close = strchr(p, ']');
		if (!close)
			break;
		p = close + 1;
		// Skip whitespace after each bracketed section
		while (*p == ' ' || *p == '\t') ++p;
	}
	// Now p points to the first non-bracketed section (file/line info or message)
	std::string cleanMsg(p);
	// Trim leading/trailing whitespace and newlines
	size_t start = cleanMsg.find_first_not_of(" \t\r\n");
	size_t end = cleanMsg.find_last_not_of(" \t\r\n");
	if (start != std::string::npos && end != std::string::npos)
		cleanMsg = cleanMsg.substr(start, end - start + 1);
	else
		cleanMsg.clear();

	// If the cleaned message is empty or only bracketed tokens, log the raw message
	bool onlyBrackets = true;
	for (char c : cleanMsg) {
		if (c != '[' && c != ']' && c != ' ' && c != '\t') {
			onlyBrackets = false;
			break;
		}
	}
	if (cleanMsg.empty() || onlyBrackets) {
		logger::info("[StreamlineSDK:RAW] {}", rawMsg);
		return;
	}

	// Use a clear prefix
	const char* prefix = "[StreamlineSDK]";
	switch (type) {
	case sl::LogType::eInfo:
		logger::info("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eWarn:
		logger::warn("{} {}", prefix, cleanMsg);
		break;
	case sl::LogType::eError:
		logger::error("{} {}", prefix, cleanMsg);
		break;
	}
}

std::vector<std::pair<std::string, std::string>> Streamline::dllVersions = {};

void Streamline::SelectDLSSGBackendAtBoot(bool a_selected)
{
	if (triedInitialization) {
		logger::warn("[Streamline] DLSS-G backend selection is startup-only; ignoring a late change to {}", a_selected);
		return;
	}

	if (a_selected && REL::Module::IsVR()) {
		logger::warn("[Streamline] DLSS-G is unavailable in VR; keeping the backend disabled");
		dlssGBackendSelectedAtBoot = false;
		return;
	}

	dlssGBackendSelectedAtBoot = a_selected;
}

void Streamline::LoadInterposer()
{
	if (triedInitialization)
		return;

	triedInitialization = true;
	initialized = false;
	deviceRegistered = false;
	featureDLSS_G = false;
	featureReflex = false;
	featurePCL = false;
	dlssGRuntimeFaulted = false;
	dlssGStateFallbackApplied = false;
	dlssGActive = false;
	dlssGOptionsInitialized = false;
	dlssGFunctionsReady = false;
	reflexFunctionsReady = false;
	pclFunctionsReady = false;
	reflexSleepFailureLogged = false;
	pclMarkerFailureLogged = false;

	std::wstring interposerPath = std::wstring(Streamline::PluginDir) + L"\\sl.interposer.dll";
	interposer = LoadLibraryW(interposerPath.c_str());
	if (interposer == nullptr) {
		DWORD errorCode = GetLastError();
		logger::info("[Streamline] Failed to load interposer: Error Code {0:x}", errorCode);
		return;
	} else {
		logger::info("[Streamline] Interposer loaded at address: {0:p}", static_cast<void*>(interposer));
	}

	// Dynamically log all DLL versions in the Streamline plugin directory
	std::filesystem::path pluginDir = std::filesystem::path(Streamline::PluginDir);
	Streamline::dllVersions.clear();
	for (const auto& entry : std::filesystem::directory_iterator(pluginDir)) {
		if (entry.is_regular_file() && entry.path().extension() == L".dll") {
			const auto& path = entry.path();
			auto version = Util::GetDllVersion(path.c_str());
			auto name = path.filename().string();
			std::string versionStr = version ? Util::GetFormattedVersion(*version) : "Unknown";
			Streamline::dllVersions.emplace_back(name, versionStr);
			if (version)
				logger::info("[Streamline] {} version: {}", name, versionStr);
			else
				logger::info("[Streamline] {} version: Unknown", name);
		}
	}

	logger::info("[Streamline] Initializing Streamline");

	sl::Preferences pref;
	const bool isVR = REL::Module::IsVR();
	std::vector<sl::Feature> featuresToLoad = { sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureNIS };
	if (!isVR && dlssGBackendSelectedAtBoot) {
		// DLSS-G owns the presentation path.  Do not request it when the
		// startup-selected backend is FSR3, otherwise both backends can claim
		// the same swap chain.
		featuresToLoad.push_back(sl::kFeatureDLSS_G);
		featuresToLoad.push_back(sl::kFeatureReflex);
		featuresToLoad.push_back(sl::kFeaturePCL);
		logger::info("[Streamline] Startup backend: DLSS-G (2x) + Reflex/PCL");
	} else {
		logger::info("[Streamline] Startup backend: {}", isVR ? "VR/no DLSS-G" : "FSR3/no DLSS-G");
	}

	pref.featuresToLoad = featuresToLoad.data();
	pref.numFeaturesToLoad = static_cast<uint32_t>(featuresToLoad.size());

	// Set log level from settings
	switch (globals::features::upscaling.settings.streamlineLogLevel) {
	case 2:
		pref.logLevel = sl::LogLevel::eVerbose;
		break;
	case 1:
		pref.logLevel = sl::LogLevel::eDefault;
		break;
	case 0:
	default:
		pref.logLevel = sl::LogLevel::eOff;
		break;
	}
	pref.logMessageCallback = LoggingCallback;
	pref.showConsole = false;

	pref.engine = sl::EngineType::eCustom;
	pref.engineVersion = "1.0.0";
	pref.projectId = "f8776929-c969-43bd-ac2b-294b4de58aac";

	pref.renderAPI = sl::RenderAPI::eD3D12;
	pref.flags = sl::PreferenceFlags::eUseManualHooking;
	if (dlssGBackendSelectedAtBoot)
		pref.flags |= sl::PreferenceFlags::eUseFrameBasedResourceTagging;

	// Hook up all of the functions exported by the SL Interposer Library
	slInit = (PFun_slInit*)GetProcAddress(interposer, "slInit");
	slShutdown = (PFun_slShutdown*)GetProcAddress(interposer, "slShutdown");
	slIsFeatureSupported = (PFun_slIsFeatureSupported*)GetProcAddress(interposer, "slIsFeatureSupported");
	slIsFeatureLoaded = (PFun_slIsFeatureLoaded*)GetProcAddress(interposer, "slIsFeatureLoaded");
	slSetFeatureLoaded = (PFun_slSetFeatureLoaded*)GetProcAddress(interposer, "slSetFeatureLoaded");
	slEvaluateFeature = (PFun_slEvaluateFeature*)GetProcAddress(interposer, "slEvaluateFeature");
	slAllocateResources = (PFun_slAllocateResources*)GetProcAddress(interposer, "slAllocateResources");
	slFreeResources = (PFun_slFreeResources*)GetProcAddress(interposer, "slFreeResources");
	slSetTag = (PFun_slSetTag*)GetProcAddress(interposer, "slSetTag");
	slSetTagForFrame = (PFun_slSetTagForFrame*)GetProcAddress(interposer, "slSetTagForFrame");
	slGetFeatureRequirements = (PFun_slGetFeatureRequirements*)GetProcAddress(interposer, "slGetFeatureRequirements");
	slGetFeatureVersion = (PFun_slGetFeatureVersion*)GetProcAddress(interposer, "slGetFeatureVersion");
	slUpgradeInterface = (PFun_slUpgradeInterface*)GetProcAddress(interposer, "slUpgradeInterface");
	slSetConstants = (PFun_slSetConstants*)GetProcAddress(interposer, "slSetConstants");
	slGetNativeInterface = (PFun_slGetNativeInterface*)GetProcAddress(interposer, "slGetNativeInterface");
	slGetFeatureFunction = (PFun_slGetFeatureFunction*)GetProcAddress(interposer, "slGetFeatureFunction");
	slGetNewFrameToken = (PFun_slGetNewFrameToken*)GetProcAddress(interposer, "slGetNewFrameToken");
	slSetD3DDevice = (PFun_slSetD3DDevice*)GetProcAddress(interposer, "slSetD3DDevice");

	const bool resourceTaggingReady = dlssGBackendSelectedAtBoot ? slSetTagForFrame != nullptr : slSetTag != nullptr;
	const bool coreFunctionsReady = slInit && slShutdown && slIsFeatureSupported && slIsFeatureLoaded &&
		slSetFeatureLoaded && slEvaluateFeature && slAllocateResources && slFreeResources && resourceTaggingReady &&
		slGetFeatureRequirements && slGetFeatureVersion && slUpgradeInterface && slSetConstants &&
		slGetNativeInterface && slGetFeatureFunction && slGetNewFrameToken && slSetD3DDevice;
	if (!coreFunctionsReady) {
		logger::critical("[Streamline] Required interposer exports are missing; disabling Streamline");
		FreeLibrary(interposer);
		interposer = nullptr;
		return;
	}

	if (SL_FAILED(res, slInit(pref, sl::kSDKVersion))) {
		logger::critical("[Streamline] Failed to initialize Streamline");
	} else {
		initialized = true;
		logger::info("[Streamline] Successfully initialized Streamline");
	}
}

sl::Result Streamline::SetD3DDevice(void* a_device)
{
	if (!slSetD3DDevice || a_device == nullptr)
		return sl::Result::eErrorInvalidParameter;

	const auto result = slSetD3DDevice(a_device);
	deviceRegistered = result == sl::Result::eOk;
	return result;
}

sl::Result Streamline::UpgradeInterface(void** a_interface)
{
	if (!slUpgradeInterface || a_interface == nullptr || *a_interface == nullptr)
		return sl::Result::eErrorInvalidParameter;

	return slUpgradeInterface(a_interface);
}

sl::Result Streamline::GetNativeInterface(void* a_proxyInterface, void** a_nativeInterface)
{
	if (!slGetNativeInterface || a_proxyInterface == nullptr || a_nativeInterface == nullptr)
		return sl::Result::eErrorInvalidParameter;

	*a_nativeInterface = nullptr;
	return slGetNativeInterface(a_proxyInterface, a_nativeInterface);
}

void Streamline::CheckFeatures(IDXGIAdapter* a_adapter)
{
	logger::info("[Streamline] Checking features");
	featureDLSS = false;
	featureDLSS_RR = false;
	featureNIS = false;
	featureDLSS_G = false;
	featureReflex = false;
	featurePCL = false;

	if (!initialized || !slIsFeatureLoaded || !slIsFeatureSupported || !slGetFeatureRequirements) {
		logger::error("[Streamline] Cannot check features before Streamline/device initialization");
		return;
	}
	if (a_adapter == nullptr) {
		logger::error("[Streamline] Cannot check features without a DXGI adapter");
		return;
	}

	DXGI_ADAPTER_DESC adapterDesc{};
	if (FAILED(a_adapter->GetDesc(&adapterDesc))) {
		logger::error("[Streamline] Failed to query DXGI adapter description");
		return;
	}

	sl::AdapterInfo adapterInfo{};
	adapterInfo.deviceLUID = reinterpret_cast<uint8_t*>(&adapterDesc.AdapterLuid);
	adapterInfo.deviceLUIDSizeInBytes = sizeof(LUID);

	auto checkFeature = [&](sl::Feature a_feature, const char* a_name, bool a_requested, bool& a_available) {
		a_available = false;
		if (!a_requested) {
			logger::info("[Streamline] {} feature was not requested", a_name);
			return;
		}

		bool loaded = false;
		const auto loadedResult = slIsFeatureLoaded(a_feature, loaded);
		if (loadedResult != sl::Result::eOk || !loaded) {
			logger::info("[Streamline] {} feature is not loaded ({})", a_name, magic_enum::enum_name(loadedResult));
			sl::FeatureRequirements requirements{};
			const auto requirementsResult = slGetFeatureRequirements(a_feature, requirements);
			if (requirementsResult != sl::Result::eOk)
				logger::info("[Streamline] {} feature requirements unavailable ({})", a_name, magic_enum::enum_name(requirementsResult));
			return;
		}

		const auto supportResult = slIsFeatureSupported(a_feature, adapterInfo);
		if (supportResult != sl::Result::eOk) {
			logger::info("[Streamline] {} feature is loaded but unsupported ({})", a_name, magic_enum::enum_name(supportResult));
			return;
		}

		a_available = true;
		logger::info("[Streamline] {} feature is loaded and supported", a_name);
	};

	checkFeature(sl::kFeatureDLSS, "DLSS", true, featureDLSS);
	checkFeature(sl::kFeatureDLSS_RR, "DLSS RR", true, featureDLSS_RR);
	checkFeature(sl::kFeatureNIS, "NIS", true, featureNIS);

	const bool requestDLSSG = !REL::Module::IsVR() && dlssGBackendSelectedAtBoot;
	checkFeature(sl::kFeatureDLSS_G, "DLSS-G", requestDLSSG, featureDLSS_G);
	checkFeature(sl::kFeatureReflex, "Reflex", requestDLSSG, featureReflex);
	checkFeature(sl::kFeaturePCL, "PCL", requestDLSSG, featurePCL);

	logger::info("[Streamline] DLSS {} available", featureDLSS ? "is" : "is not");
	logger::info("[Streamline] DLSS RR {} available", featureDLSS_RR ? "is" : "is not");
	logger::info("[Streamline] NIS {} available", featureNIS ? "is" : "is not");
	logger::info("[Streamline] DLSS-G {} available", featureDLSS_G ? "is" : "is not");
	logger::info("[Streamline] Reflex {} available", featureReflex ? "is" : "is not");
	logger::info("[Streamline] PCL {} available", featurePCL ? "is" : "is not");
}

void Streamline::PostDevice()
{
	dlssGFunctionsReady = false;
	reflexFunctionsReady = false;
	pclFunctionsReady = false;

	if (!initialized || !deviceRegistered || !slGetFeatureFunction) {
		logger::error("[Streamline] Cannot load feature functions before the device is registered");
		return;
	}

	auto load = [&](sl::Feature a_feature, const char* a_name, auto& a_target) {
		sl::Result result = sl::Result::eOk;
		if (!LoadFeatureFunction(slGetFeatureFunction, a_feature, a_name, a_target, result)) {
			logger::error("[Streamline] Failed to load {} ({})", a_name, magic_enum::enum_name(result));
			return false;
		}
		return true;
	};

	if (featureDLSS) {
		load(sl::kFeatureDLSS, "slDLSSGetOptimalSettings", slDLSSGetOptimalSettings);
		load(sl::kFeatureDLSS, "slDLSSGetState", slDLSSGetState);
		load(sl::kFeatureDLSS, "slDLSSSetOptions", slDLSSSetOptions);
	}

	if (featureDLSS_RR) {
		load(sl::kFeatureDLSS_RR, "slDLSSDGetOptimalSettings", slDLSSDGetOptimalSettings);
		load(sl::kFeatureDLSS_RR, "slDLSSDGetState", slDLSSDGetState);
		load(sl::kFeatureDLSS_RR, "slDLSSDSetOptions", slDLSSDSetOptions);
	}

	if (featureNIS) {
		load(sl::kFeatureNIS, "slNISSetOptions", slNISSetOptions);
		load(sl::kFeatureNIS, "slNISGetState", slNISGetState);
	}

	if (featureDLSS_G) {
		const bool stateReady = load(sl::kFeatureDLSS_G, "slDLSSGGetState", slDLSSGGetState);
		const bool optionsReady = load(sl::kFeatureDLSS_G, "slDLSSGSetOptions", slDLSSGSetOptions);
		dlssGFunctionsReady = stateReady && optionsReady;
	}

	if (featureReflex) {
		const bool stateReady = load(sl::kFeatureReflex, "slReflexGetState", slReflexGetState);
		const bool sleepReady = load(sl::kFeatureReflex, "slReflexSleep", slReflexSleep);
		const bool optionsReady = load(sl::kFeatureReflex, "slReflexSetOptions", slReflexSetOptions);
		reflexFunctionsReady = stateReady && sleepReady && optionsReady;
	}

	if (featurePCL) {
		const bool stateReady = load(sl::kFeaturePCL, "slPCLGetState", slPCLGetState);
		const bool markerReady = load(sl::kFeaturePCL, "slPCLSetMarker", slPCLSetMarker);
		const bool optionsReady = load(sl::kFeaturePCL, "slPCLSetOptions", slPCLSetOptions);
		pclFunctionsReady = stateReady && markerReady && optionsReady;
	}

	if (reflexFunctionsReady && !SetReflexOptions(sl::ReflexMode::eLowLatency))
		reflexFunctionsReady = false;
	if (pclFunctionsReady) {
		sl::PCLOptions options{};
		if (SL_FAILED(result, slPCLSetOptions(options))) {
			logger::error("[Streamline] Could not initialize PCL options ({})", magic_enum::enum_name(result));
			pclFunctionsReady = false;
		}
	}

	if (dlssGBackendSelectedAtBoot && !IsDLSSGReady())
		logger::error("[Streamline] DLSS-G startup backend is not ready; the caller must fall back to its non-DLSS-G path");
}

bool Streamline::IsDLSSGReady() const
{
	return !REL::Module::IsVR() && dlssGBackendSelectedAtBoot && initialized && deviceRegistered && featureDLSS_G && featureReflex && featurePCL &&
		!dlssGRuntimeFaulted && dlssGFunctionsReady && reflexFunctionsReady && pclFunctionsReady;
}

/**
 * @brief Updates and sets camera and frame constants for the current Streamline frame.
 *
 * Populates and submits camera parameters, projection matrices, motion vector settings, and other per-frame constants to the Streamline SDK for the current frame. Uses cached framebuffer data and global state to ensure correct configuration for upscaling and frame generation features.
 */
bool Streamline::BeginFrameToken()
{
	if (!initialized || !slGetNewFrameToken || globals::state == nullptr)
		return false;

	const uint32_t currentFrame = globals::state->frameCount;
	if (frameToken != nullptr && frameTokenIndex == currentFrame)
		return true;

	frameToken = nullptr;
	frameTokenIndex = UINT32_MAX;
	frameConstantsValid = false;
	if (SL_FAILED(res, slGetNewFrameToken(frameToken, &currentFrame)) || frameToken == nullptr) {
		logger::error("[Streamline] Could not acquire frame token for frame {}", currentFrame);
		frameToken = nullptr;
		return false;
	}
	frameTokenIndex = currentFrame;
	return true;
}

void Streamline::CheckFrameConstants()
{
	if (!slSetConstants || !BeginFrameToken())
		return;
	SubmitFrameConstants();
}

void Streamline::CheckFrameConstantsForLatchedFrame()
{
	// Present is entered after State::Reset has advanced the host counter.
	// Do not acquire that next token: resources still belong to the token
	// latched at SimulationStart for the frame being presented.
	if (!slSetConstants || frameToken == nullptr)
		return;
	SubmitFrameConstants();
}

void Streamline::RequestTemporalReset()
{
	temporalResetRequested = true;
}

void Streamline::SubmitFrameConstants()
{
	if (frameConstantsValid)
		return;
	if (globals::game::cameraNear == nullptr || globals::game::cameraFar == nullptr)
		return;

	auto state = globals::state;
	const uint32_t currentFrame = frameTokenIndex;

	sl::Constants slConstants = {};

	if (globals::game::isVR) {
		slConstants.cameraAspectRatio = (state->screenSize.x * 0.5f) / state->screenSize.y;
	} else {
		slConstants.cameraAspectRatio = state->screenSize.x / state->screenSize.y;
	}

	slConstants.cameraFOV = Util::GetVerticalFOVRad();
	slConstants.cameraNear = *globals::game::cameraNear;
	slConstants.cameraFar = *globals::game::cameraFar;

	auto viewMatrix = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();
	auto cameraViewToClip = globals::game::frameBufferCached.GetCameraProjUnjittered().Transpose();

	slConstants.cameraMotionIncluded = sl::Boolean::eTrue;
	slConstants.cameraPinholeOffset = { 0.f, 0.f };
	slConstants.cameraRight = { viewMatrix._11, viewMatrix._12, viewMatrix._13 };
	slConstants.cameraUp = { viewMatrix._21, viewMatrix._22, viewMatrix._23 };
	slConstants.cameraFwd = { viewMatrix._31, viewMatrix._32, viewMatrix._33 };
	slConstants.cameraPos = *(sl::float3*)&globals::game::frameBufferCached.GetCameraPosAdjust();
	slConstants.cameraViewToClip = *(sl::float4x4*)&cameraViewToClip;
	slConstants.depthInverted = sl::Boolean::eFalse;

	recalculateCameraMatrices(slConstants);

	auto& upscaling = globals::features::upscaling;
	auto jitter = upscaling.jitter;
	slConstants.jitterOffset = { -jitter.x, -jitter.y };

	slConstants.reset = temporalResetRequested ? sl::Boolean::eTrue : sl::Boolean::eFalse;

	slConstants.mvecScale = { (globals::game::isVR ? 0.5f : 1.0f), 1 };
	slConstants.motionVectors3D = sl::Boolean::eFalse;
	slConstants.motionVectorsInvalidValue = FLT_MIN;
	// The world map uses a different camera path. Derive the projection type
	// from the submitted matrix instead of forcing perspective for every frame.
	const bool orthographicProjection = std::abs(cameraViewToClip._44) > 0.5f;
	slConstants.orthographicProjection = orthographicProjection ? sl::Boolean::eTrue : sl::Boolean::eFalse;
	slConstants.motionVectorsDilated = sl::Boolean::eFalse;
	slConstants.motionVectorsJittered = sl::Boolean::eFalse;

	if (SL_FAILED(res, slSetConstants(slConstants, *frameToken, viewport))) {
		logger::error("[Streamline] Could not set constants for frame {}", currentFrame);
		frameConstantsValid = false;
		return;
	}

	if (temporalResetRequested) {
		logger::info("[Streamline] Submitted requested temporal reset for frame {}", currentFrame);
		temporalResetRequested = false;
	}
	frameConstantsValid = true;
}

sl::Result Streamline::SetTagsForCurrentFrame(const sl::ResourceTag* a_tags, uint32_t a_numTags, ID3D12GraphicsCommandList* a_commandList)
{
	CheckFrameConstants();
	if (dlssGBackendSelectedAtBoot)
		return SetTagsForLatchedFrame(a_tags, a_numTags, a_commandList);
	if (a_tags == nullptr || a_numTags == 0)
		return sl::Result::eErrorMissingInputParameter;
	if (slSetTag == nullptr)
		return sl::Result::eErrorFeatureMissing;
	return slSetTag(viewport, a_tags, a_numTags, a_commandList);
}

sl::Result Streamline::SetTagsForLatchedFrame(const sl::ResourceTag* a_tags, uint32_t a_numTags, ID3D12GraphicsCommandList* a_commandList)
{
	if (!frameConstantsValid || frameToken == nullptr)
		return sl::Result::eErrorCommonConstantsMissing;
	if (a_tags == nullptr || a_numTags == 0)
		return sl::Result::eErrorMissingInputParameter;
	if (slSetTagForFrame == nullptr)
		return sl::Result::eErrorFeatureMissing;

	return slSetTagForFrame(*frameToken, viewport, a_tags, a_numTags, a_commandList);
}

void Streamline::FallbackDLSSG(const char* a_reason, sl::Result a_result, sl::DLSSGStatus a_status)
{
	if (dlssGStateFallbackApplied)
		return;

	dlssGStateFallbackApplied = true;
	dlssGRuntimeFaulted = true;
	dlssGActive = false;
	dlssGOptionsInitialized = true;
	logger::error("[Streamline] DLSS-G {} (result={}, status={}); disabling DLSS-G for this session", a_reason, magic_enum::enum_name(a_result), static_cast<uint32_t>(a_status));

	if (slDLSSGSetOptions != nullptr) {
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		options.numFramesToGenerate = 1;
		options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		const auto disableResult = slDLSSGSetOptions(viewport, options);
		if (disableResult != sl::Result::eOk)
			logger::critical("[Streamline] DLSS-G failed to enter the safe off state ({})", magic_enum::enum_name(disableResult));
	}
}

bool Streamline::SetDLSSGMode(bool a_enable, bool a_retainResourcesWhenOff)
{
	if (REL::Module::IsVR() || !dlssGBackendSelectedAtBoot || !featureDLSS_G || slDLSSGSetOptions == nullptr)
		return false;
	if (a_enable && !IsDLSSGReady())
		return false;
	if (a_enable && dlssGRuntimeFaulted)
		return false;

	const auto& swapChain = globals::features::upscaling.dx12SwapChain;
	const auto inputWidth = swapChain.GetDLSSGInputWidth();
	const auto inputHeight = swapChain.GetDLSSGInputHeight();
	const bool inputExtentUnchanged = dlssGConfiguredInputWidth == inputWidth && dlssGConfiguredInputHeight == inputHeight;
	const bool outputExtentUnchanged = dlssGConfiguredOutputWidth == swapChain.swapChainDesc.Width &&
		dlssGConfiguredOutputHeight == swapChain.swapChainDesc.Height;
	const bool retentionUnchanged = dlssGRetainResourcesWhenOff == a_retainResourcesWhenOff;
	if (dlssGOptionsInitialized && dlssGActive == a_enable && retentionUnchanged && (!a_enable || (inputExtentUnchanged && outputExtentUnchanged)))
		return true;

	sl::DLSSGOptions options{};
	options.mode = a_enable ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
	options.numFramesToGenerate = 1;  // Fixed 2x: one generated frame per real frame.
	// FinalColor already contains the scene plus UI. UI recomposition stays off
	// because this path only supplies an exact-format HUD-less snapshot.
	// This integration uses the fixed input ratio selected by the active
	// upscaler mode. Streamline explicitly advises against enabling its dynamic
	// resolution flag for fixed-ratio DLSS; the per-frame tags still need the
	// correct active input extent.
	options.flags = a_retainResourcesWhenOff ? sl::DLSSGFlags::eRetainResourcesWhenOff : sl::DLSSGFlags{};
	options.enableUserInterfaceRecomposition = sl::Boolean::eFalse;

	options.numBackBuffers = 2;
	options.mvecDepthWidth = inputWidth;
	options.mvecDepthHeight = inputHeight;
	options.colorWidth = swapChain.swapChainDesc.Width;
	options.colorHeight = swapChain.swapChainDesc.Height;
	options.colorBufferFormat = static_cast<uint32_t>(swapChain.swapChainDesc.Format);
	if (swapChain.motionVectorBufferShared12)
		options.mvecBufferFormat = static_cast<uint32_t>(swapChain.motionVectorBufferShared12->resource->GetDesc().Format);
	if (swapChain.depthBufferShared12)
		options.depthBufferFormat = static_cast<uint32_t>(swapChain.depthBufferShared12->resource->GetDesc().Format);
	if (swapChain.uiBufferWrapped)
		options.hudLessBufferFormat = static_cast<uint32_t>(swapChain.uiBufferWrapped->resource->GetDesc().Format);

	const auto result = slDLSSGSetOptions(viewport, options);
	if (result != sl::Result::eOk) {
		if (a_enable) {
			FallbackDLSSG("could not apply options", result, sl::DLSSGStatus::eOk);
		} else {
			logger::error("[Streamline] Could not disable DLSS-G ({})", magic_enum::enum_name(result));
		}
		return false;
	}

	dlssGActive = a_enable;
	dlssGOptionsInitialized = true;
	dlssGRetainResourcesWhenOff = a_retainResourcesWhenOff;
	dlssGConfiguredInputWidth = inputWidth;
	dlssGConfiguredInputHeight = inputHeight;
	dlssGConfiguredOutputWidth = swapChain.swapChainDesc.Width;
	dlssGConfiguredOutputHeight = swapChain.swapChainDesc.Height;
	logger::info("[Streamline] DLSS-G mode {} with input extent {}x{} and output extent {}x{} (retainWhenOff={})",
		a_enable ? "enabled" : "disabled",
		inputWidth,
		inputHeight,
		swapChain.swapChainDesc.Width,
		swapChain.swapChainDesc.Height,
		a_retainResourcesWhenOff);
	return true;
}

bool Streamline::GetDLSSGState(sl::DLSSGState& a_state)
{
	a_state = {};
	if (!IsDLSSGReady() || slDLSSGGetState == nullptr)
		return false;

	const auto result = slDLSSGGetState(viewport, a_state, nullptr);
	if (result != sl::Result::eOk) {
		FallbackDLSSG("state query failed", result, a_state.status);
		return false;
	}
	if (a_state.status != sl::DLSSGStatus::eOk) {
		FallbackDLSSG("reported an invalid runtime state", sl::Result::eOk, a_state.status);
		return false;
	}

	return true;
}

bool Streamline::TagDLSSGResources(const DLSSGFrameResources& a_resources, ID3D12GraphicsCommandList* a_commandList)
{
	if (REL::Module::IsVR() || !dlssGBackendSelectedAtBoot || !featureDLSS_G || dlssGRuntimeFaulted)
		return false;
	if (a_resources.depth == nullptr || a_resources.motionVectors == nullptr || a_resources.hudless == nullptr) {
		FallbackDLSSG("is missing a required frame resource", sl::Result::eErrorMissingInputParameter, sl::DLSSGStatus::eOk);
		return false;
	}

	// Required inputs are always submitted, including null tags when a frame
	// is invalid, so Streamline cannot retain stale resources across a menu or
	// loading transition.  UI alpha is preferred when both UI forms exist.
	sl::Resource depthResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.depth), static_cast<uint32_t>(a_resources.depthState) };
	sl::Resource motionVectorsResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.motionVectors), static_cast<uint32_t>(a_resources.motionVectorsState) };
	sl::Resource hudlessResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.hudless), static_cast<uint32_t>(a_resources.hudlessState) };
	sl::Resource uiColorAndAlphaResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.uiColorAndAlpha), static_cast<uint32_t>(a_resources.uiColorAndAlphaState) };
	sl::Resource uiAlphaResource{ sl::ResourceType::eTex2d, reinterpret_cast<void*>(a_resources.uiAlpha), static_cast<uint32_t>(a_resources.uiAlphaState) };

	sl::ResourceTag tags[] = {
		sl::ResourceTag{ a_resources.depth ? &depthResource : nullptr, sl::kBufferTypeDepth, sl::eValidUntilPresent, &a_resources.depthExtent },
		sl::ResourceTag{ a_resources.motionVectors ? &motionVectorsResource : nullptr, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent, &a_resources.motionVectorsExtent },
		sl::ResourceTag{ a_resources.hudless ? &hudlessResource : nullptr, sl::kBufferTypeHUDLessColor, sl::eValidUntilPresent, &a_resources.hudlessExtent },
		sl::ResourceTag{ a_resources.uiColorAndAlpha ? &uiColorAndAlphaResource : nullptr, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent, &a_resources.uiColorAndAlphaExtent },
		sl::ResourceTag{ a_resources.uiAlpha ? &uiAlphaResource : nullptr, sl::kBufferTypeUIAlpha, sl::eValidUntilPresent, &a_resources.uiAlphaExtent }
	};

	// Present is entered after State::Reset has advanced the engine frame
	// counter. Reuse the token acquired at SimulationStart instead of asking
	// for the next frame here.
	const auto result = SetTagsForLatchedFrame(tags, _countof(tags), a_commandList);

	if (result != sl::Result::eOk) {
		FallbackDLSSG("could not tag its frame resources", result, sl::DLSSGStatus::eOk);
		return false;
	}

	return true;
}

void Streamline::DestroyDLSSGResources(bool a_modeSwitch)
{
	if (a_modeSwitch && slDLSSGSetOptions != nullptr && featureDLSS_G) {
		sl::DLSSGOptions options{};
		options.mode = sl::DLSSGMode::eOff;
		options.numFramesToGenerate = 1;
		options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
		if (SL_FAILED(result, slDLSSGSetOptions(viewport, options)))
			logger::error("[Streamline] Could not disable DLSS-G before freeing resources ({})", magic_enum::enum_name(result));
	}

	dlssGActive = false;
	dlssGOptionsInitialized = false;
	if (slFreeResources != nullptr && featureDLSS_G) {
		if (SL_FAILED(result, slFreeResources(sl::kFeatureDLSS_G, viewport)))
			logger::error("[Streamline] Could not free DLSS-G resources ({})", magic_enum::enum_name(result));
	}
}

bool Streamline::GetReflexState(sl::ReflexState& a_state)
{
	a_state = {};
	if (!featureReflex || !slReflexGetState)
		return false;

	if (SL_FAILED(result, slReflexGetState(a_state))) {
		logger::error("[Streamline] Could not query Reflex state ({})", magic_enum::enum_name(result));
		return false;
	}
	return true;
}

bool Streamline::SetReflexOptions(sl::ReflexMode a_mode, uint32_t a_frameLimitUs)
{
	if (!featureReflex || !slReflexSetOptions)
		return false;

	sl::ReflexOptions options{};
	options.mode = a_mode;
	options.frameLimitUs = a_frameLimitUs;
	if (SL_FAILED(result, slReflexSetOptions(options))) {
		logger::error("[Streamline] Could not set Reflex options ({})", magic_enum::enum_name(result));
		return false;
	}
	return true;
}

bool Streamline::ReflexSleep()
{
	if (!featureReflex || !slReflexSleep)
		return false;

	// Reflex sleep and PCL markers are bound to the frame token but do not
	// require rendering constants.  Requiring frameConstantsValid here made
	// every SimulationStart call fail immediately after BeginFrameToken().
	if (frameToken == nullptr)
		return false;

	if (SL_FAILED(result, slReflexSleep(*frameToken))) {
		if (!reflexSleepFailureLogged) {
			logger::error("[Streamline] Reflex sleep failed for frame {} ({})", frameTokenIndex, magic_enum::enum_name(result));
			reflexSleepFailureLogged = true;
		}
		return false;
	}
	return true;
}

bool Streamline::GetPCLState(sl::PCLState& a_state)
{
	a_state = {};
	if (!featurePCL || !slPCLGetState)
		return false;

	if (SL_FAILED(result, slPCLGetState(a_state))) {
		logger::error("[Streamline] Could not query PCL state ({})", magic_enum::enum_name(result));
		return false;
	}
	return true;
}

bool Streamline::SetPCLMarker(sl::PCLMarker a_marker)
{
	if (!featurePCL || !slPCLSetMarker)
		return false;

	if (frameToken == nullptr)
		return false;

	if (SL_FAILED(result, slPCLSetMarker(a_marker, *frameToken))) {
		if (!pclMarkerFailureLogged) {
			logger::error("[Streamline] PCL marker {} failed for frame {} ({})", static_cast<uint32_t>(a_marker), frameTokenIndex, magic_enum::enum_name(result));
			pclMarkerFailureLogged = true;
		}
		return false;
	}
	return true;
}

void Streamline::SetDLSSOptions()
{
	sl::DLSSOptions dlssOptions{};

	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	switch (qualityMode) {
	case 1:
		dlssOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}

	auto state = globals::state;

	dlssOptions.outputWidth = (uint)state->screenSize.x;
	dlssOptions.outputHeight = (uint)state->screenSize.y;
	dlssOptions.colorBuffersHDR = sl::Boolean::eTrue;
	dlssOptions.useAutoExposure = sl::Boolean::eTrue;

	dlssOptions.preExposure = 1.0f;
	dlssOptions.sharpness = 0.0f;

	auto& settings = globals::features::upscaling.settings;
	auto requestedPresetValue = settings.DLSSPreset;
	if (requestedPresetValue >= kDLSSModelPresetCount) {
		logger::warn("[Streamline] Invalid DLSS model preset {}, falling back to K", requestedPresetValue);
		requestedPresetValue = static_cast<uint>(DLSSModelPreset::kK);
		settings.DLSSPreset = requestedPresetValue;
	}

	const auto requestedPreset = static_cast<DLSSModelPreset>(requestedPresetValue);
	static uint lastLoggedPreset = kUnreportedPreset;
	static uint lastRequestedPreset = kUnreportedPreset;
	static uint sessionFallbackPreset = kUnreportedPreset;
	static uint lastFallbackFailurePreset = kUnreportedPreset;
	if (lastRequestedPreset != requestedPresetValue) {
		lastRequestedPreset = requestedPresetValue;
		sessionFallbackPreset = kUnreportedPreset;
		lastFallbackFailurePreset = kUnreportedPreset;
	}

	if (sessionFallbackPreset == requestedPresetValue) {
		SetAllDLSSPresets(dlssOptions, sl::DLSSPreset::ePresetK);
		const auto fallbackResult = slDLSSSetOptions(viewport, dlssOptions);
		if (fallbackResult != sl::Result::eOk && lastFallbackFailurePreset != requestedPresetValue) {
			logger::critical("[Streamline] DLSS SR K fallback failed ({})", magic_enum::enum_name(fallbackResult));
			lastFallbackFailurePreset = requestedPresetValue;
		}
		return;
	}

	if (requestedPreset == DLSSModelPreset::kSDKDocumentedMapping)
		SetSDKDocumentedDLSSPresets(dlssOptions);
	else
		SetAllDLSSPresets(dlssOptions, GetForcedDLSSPreset(requestedPreset));

	const auto result = slDLSSSetOptions(viewport, dlssOptions);
	if (result == sl::Result::eOk) {
		if (lastLoggedPreset != requestedPresetValue) {
			logger::info("[Streamline] Requested DLSS SR model preset: {}", GetDLSSModelPresetName(requestedPreset));
			lastLoggedPreset = requestedPresetValue;
		}
		return;
	}

	logger::error("[Streamline] slDLSSSetOptions failed for requested DLSS SR model preset {} ({}); using K fallback for this selection", GetDLSSModelPresetName(requestedPreset), magic_enum::enum_name(result));
	sessionFallbackPreset = requestedPresetValue;
	SetAllDLSSPresets(dlssOptions, sl::DLSSPreset::ePresetK);
	const auto fallbackResult = slDLSSSetOptions(viewport, dlssOptions);
	if (fallbackResult == sl::Result::eOk) {
		logger::warn("[Streamline] DLSS SR is using K fallback after the requested {} preset failed", GetDLSSModelPresetName(requestedPreset));
	} else {
		logger::critical("[Streamline] DLSS SR K fallback failed ({})", magic_enum::enum_name(fallbackResult));
		lastFallbackFailurePreset = requestedPresetValue;
	}
}

void Streamline::Upscale(ID3D12Resource* a_inputColorTexture,
	ID3D12Resource* a_motionVectorTexture,
	ID3D12Resource* a_depthTexture,
	ID3D12Resource* a_reactiveMask,
	ID3D12Resource* a_transparencyCompositionMask,
	ID3D12Resource* a_outputTexture,
	ID3D12GraphicsCommandList* a_commandList)
{
	CheckFrameConstants();
	SetDLSSOptions();

	auto state = globals::state;

	{
		auto screenSize = state->screenSize;
		auto renderSize = Util::ConvertToDynamic(screenSize);

		sl::Extent lowResExtent{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
		sl::Extent fullExtent{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

		sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColorTexture, 0 };
		sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputTexture, 0 };
		sl::Resource depth = { sl::ResourceType::eTex2d, a_depthTexture, 0 };
		sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectorTexture, 0 };

		sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &lowResExtent };
		sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent };
		sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };
		sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };

		sl::Resource reactiveMask = { sl::ResourceType::eTex2d, a_reactiveMask, 0 };
		sl::ResourceTag reactiveMaskTag = sl::ResourceTag{ &reactiveMask, sl::kBufferTypeBiasCurrentColorHint, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };

		sl::Resource transparencyCompositionMask = { sl::ResourceType::eTex2d, a_transparencyCompositionMask, 0 };
		sl::ResourceTag transparencyCompositionMaskTag = sl::ResourceTag{ &transparencyCompositionMask, sl::kBufferTypeTransparencyHint, sl::ResourceLifecycle::eValidUntilPresent, &lowResExtent };

		sl::ResourceTag resourceTags[] = { colorInTag, colorOutTag, depthTag, mvecTag, reactiveMaskTag, transparencyCompositionMaskTag };

		if (SL_FAILED(result, SetTagsForCurrentFrame(resourceTags, _countof(resourceTags), a_commandList))) {
			logger::error("[Streamline] Failed to set DLSS SR resource tags ({})", magic_enum::enum_name(result));
			return;
		}
	}

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };
	slEvaluateFeature(sl::kFeatureDLSS, *frameToken, inputs, _countof(inputs), a_commandList);
}

void Streamline::SetDLSSRROptions() {
	sl::DLSSDOptions dlssdOptions{};

	// Map quality mode to DLSS mode
	uint32_t qualityMode = globals::features::upscaling.settings.qualityMode;
	switch (qualityMode) {
	case 1:
		dlssdOptions.mode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssdOptions.mode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssdOptions.mode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssdOptions.mode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssdOptions.mode = sl::DLSSMode::eDLAA;
		break;
	}

	auto worldToCameraView = globals::game::frameBufferCached.GetCameraView().Transpose();
	auto cameraViewToWorld = globals::game::frameBufferCached.GetCameraViewInverse().Transpose();

	auto state = globals::state;

	dlssdOptions.outputWidth = (uint)state->screenSize.x;
	dlssdOptions.outputHeight = (uint)state->screenSize.y;
	dlssdOptions.colorBuffersHDR = sl::Boolean::eTrue;
	dlssdOptions.normalRoughnessMode = sl::DLSSDNormalRoughnessMode::ePacked;
	dlssdOptions.alphaUpscalingEnabled = sl::Boolean::eFalse;

	dlssdOptions.worldToCameraView = sl::float4x4{
		sl::float4{ worldToCameraView._11, worldToCameraView._12, worldToCameraView._13, worldToCameraView._14 },
		sl::float4{ worldToCameraView._21, worldToCameraView._22, worldToCameraView._23, worldToCameraView._24 },
		sl::float4{ worldToCameraView._31, worldToCameraView._32, worldToCameraView._33, worldToCameraView._34 },
		sl::float4{ worldToCameraView._41, worldToCameraView._42, worldToCameraView._43, worldToCameraView._44 }
	};
	dlssdOptions.cameraViewToWorld = sl::float4x4{
		sl::float4{ cameraViewToWorld._11, cameraViewToWorld._12, cameraViewToWorld._13, cameraViewToWorld._14 },
		sl::float4{ cameraViewToWorld._21, cameraViewToWorld._22, cameraViewToWorld._23, cameraViewToWorld._24 },
		sl::float4{ cameraViewToWorld._31, cameraViewToWorld._32, cameraViewToWorld._33, cameraViewToWorld._34 },
		sl::float4{ cameraViewToWorld._41, cameraViewToWorld._42, cameraViewToWorld._43, cameraViewToWorld._44 }
	};
	dlssdOptions.dlaaPreset = sl::DLSSDPreset::ePresetD;
	dlssdOptions.qualityPreset = sl::DLSSDPreset::ePresetD;
	dlssdOptions.balancedPreset = sl::DLSSDPreset::ePresetD;
	dlssdOptions.performancePreset = sl::DLSSDPreset::ePresetD;
	dlssdOptions.ultraPerformancePreset = sl::DLSSDPreset::ePresetD;

	if(SL_FAILED(result, slDLSSDSetOptions(viewport, dlssdOptions))) {
		logger::critical("[DLSS RR] Could not set DLSS RR options");
		return;
	}
}

void Streamline::RayReconstruction(ID3D12Resource* a_inputColorTexture,
	ID3D12Resource* a_motionVectorTexture,
	ID3D12Resource* a_depthTexture,
	ID3D12Resource* a_albedoTexture,
	ID3D12Resource* a_reflectanceTexture,
	ID3D12Resource* a_normalRoughness,
	ID3D12Resource* a_specularHitDistance,
	ID3D12Resource* a_colorBeforeTransparency,
	ID3D12Resource* a_sssGuide,
	ID3D12Resource* a_outputTexture,
	ID3D12GraphicsCommandList* a_commandList)
{
	if (!featureDLSS_RR)
		return;

	logger::debug("[DLSS RR] Starting Ray Reconstruction");

	CheckFrameConstants();
	logger::debug("[DLSS RR] Frame constants set");
	SetDLSSRROptions();
	logger::debug("[DLSS RR] DLSS RR options set");

	auto state = globals::state;

	{
		auto screenSize = state->screenSize;
		auto renderSize = Util::ConvertToDynamic(screenSize);

		sl::Extent inputExtent{ 0, 0, (uint)renderSize.x, (uint)renderSize.y };
		sl::Extent outputExtent{ 0, 0, (uint)screenSize.x, (uint)screenSize.y };

		sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColorTexture, 0 };
		sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputTexture, 0 };
		sl::Resource depth = { sl::ResourceType::eTex2d, a_depthTexture, 0 };
		sl::Resource mvec = { sl::ResourceType::eTex2d, a_motionVectorTexture, 0 };
		sl::Resource diffuseAlbedo = { sl::ResourceType::eTex2d, a_albedoTexture, 0 };
		sl::Resource specularAlbedo = { sl::ResourceType::eTex2d, a_reflectanceTexture, 0 };
		sl::Resource normalRoughness = { sl::ResourceType::eTex2d, a_normalRoughness, 0 };
		sl::Resource specHitDistance = { sl::ResourceType::eTex2d, a_specularHitDistance, 0 };
		sl::Resource colorBeforeTransparency = { sl::ResourceType::eTex2d, a_colorBeforeTransparency, 0 };
		sl::Resource sssGuide = { sl::ResourceType::eTex2d, a_sssGuide, 0 };

		sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &inputExtent };
		sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &outputExtent };
		sl::ResourceTag depthTag = sl::ResourceTag{ &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag mvecTag = sl::ResourceTag{ &mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag diffuseAlbedoTag = sl::ResourceTag{ &diffuseAlbedo, sl::kBufferTypeAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag specularAlbedoTag = sl::ResourceTag{ &specularAlbedo, sl::kBufferTypeSpecularAlbedo, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag normalRoughnessTag = sl::ResourceTag{ &normalRoughness, sl::kBufferTypeNormalRoughness, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag specHitDistanceTag = sl::ResourceTag{ &specHitDistance, sl::kBufferTypeSpecularHitDistance, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag colorBeforeTransparencyTag = sl::ResourceTag{ &colorBeforeTransparency, sl::kBufferTypeColorBeforeTransparency, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };
		sl::ResourceTag sssGuideTag = sl::ResourceTag{ &sssGuide, sl::kBufferTypeScreenSpaceSubsurfaceScatteringGuide, sl::ResourceLifecycle::eValidUntilPresent, &inputExtent };

		sl::ResourceTag resourceTags[] = { colorInTag, colorOutTag, depthTag, mvecTag, diffuseAlbedoTag, specularAlbedoTag, normalRoughnessTag, specHitDistanceTag, colorBeforeTransparencyTag, sssGuideTag };
		if (SL_FAILED(result, SetTagsForCurrentFrame(resourceTags, _countof(resourceTags), a_commandList))) {
			logger::error("[DLSS RR] Failed to set DLSS RR tags, error code: {}", (int)result);
			return;
		}
	}

	logger::debug("[DLSS RR] DLSS RR resources set");

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };

	if (SL_FAILED(result, slEvaluateFeature(sl::kFeatureDLSS_RR, *frameToken, inputs, _countof(inputs), a_commandList))) {
		logger::error("[DLSS RR] Failed to evaluate DLSS RR feature, error code: {}", (int)result);
		return;
	} else {
		logger::debug("[DLSS RR] slEvaluateFeature executed successfully, output texture updated");
	}
	logger::debug("[DLSS RR] slEvaluateFeature completed");
}

float2 Streamline::GetInputResolutionScale(uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityMode)
{
	sl::DLSSMode dlssMode;
	switch (qualityMode) {
	case 1:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssMode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssMode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssMode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssMode = sl::DLSSMode::eDLAA;
		break;
	}

	sl::DLSSOptions dlssOptions{};
	dlssOptions.mode = dlssMode;
	dlssOptions.outputWidth = outputWidth;
	dlssOptions.outputHeight = outputHeight;

	sl::DLSSOptimalSettings optimalSettings{};
	sl::Result result = slDLSSGetOptimalSettings(dlssOptions, optimalSettings);
	if (result != sl::Result::eOk) {
		logger::critical("[Streamline] Failed to get DLSS optimal settings, error code: {}", (int)result);
		return { 1.0f, 1.0f };
	}

	float scaleX;
	float scaleY;

	if (globals::game::ui->GameIsPaused()) {
		// Calculate scale as ratio of minimum render resolution to output resolution
		scaleX = (float)optimalSettings.renderWidthMin / (float)outputWidth;
		scaleY = (float)optimalSettings.renderHeightMin / (float)outputHeight;
	} else {
		// Calculate scale as ratio of optimal render resolution to output resolution
		scaleX = (float)optimalSettings.optimalRenderWidth / (float)outputWidth;
		scaleY = (float)optimalSettings.optimalRenderHeight / (float)outputHeight;
	}

	// Return separate X and Y scales for more precision
	return { scaleX, scaleY };
}

float2 Streamline::GetInputResolutionScaleRR(uint32_t outputWidth, uint32_t outputHeight, uint32_t qualityMode)
{
	logger::debug("[DLSS RR] Getting input resolution scale for output {}x{} and quality mode {}", outputWidth, outputHeight, qualityMode);
	sl::DLSSMode dlssMode;
	switch (qualityMode) {
	case 1:
		dlssMode = sl::DLSSMode::eMaxQuality;
		break;
	case 2:
		dlssMode = sl::DLSSMode::eBalanced;
		break;
	case 3:
		dlssMode = sl::DLSSMode::eMaxPerformance;
		break;
	case 4:
		dlssMode = sl::DLSSMode::eUltraPerformance;
		break;
	default:
		dlssMode = sl::DLSSMode::eDLAA;
		break;
	}

	sl::DLSSDOptions dlssdOptions{};
	dlssdOptions.mode = dlssMode;
	dlssdOptions.outputWidth = outputWidth;
	dlssdOptions.outputHeight = outputHeight;

	sl::DLSSDOptimalSettings optimalSettings{};
	sl::Result result = slDLSSDGetOptimalSettings(dlssdOptions, optimalSettings);
	if (result != sl::Result::eOk) {
		logger::critical("[Streamline] Failed to get DLSS RR optimal settings, error code: {}", (int)result);
		return { 1.0f, 1.0f };
	}

	float scaleX;
	float scaleY;

	if (globals::game::ui->GameIsPaused()) {
		// Calculate scale as ratio of minimum render resolution to output resolution
		scaleX = (float)optimalSettings.renderWidthMin / (float)outputWidth;
		scaleY = (float)optimalSettings.renderHeightMin / (float)outputHeight;
	} else {
		// Calculate scale as ratio of optimal render resolution to output resolution
		scaleX = (float)optimalSettings.optimalRenderWidth / (float)outputWidth;
		scaleY = (float)optimalSettings.optimalRenderHeight / (float)outputHeight;
	}

	// Return separate X and Y scales for more precision
	return { scaleX, scaleY };
}

/**
 * @brief Releases DLSS resources and disables DLSS for the current viewport.
 *
 * Sets the DLSS mode to off and frees all DLSS-related resources associated with the viewport.
 */
void Streamline::DestroyDLSSResources(bool modeSwitch)
{
	if (modeSwitch) {
		sl::DLSSOptions dlssOptions{};
		dlssOptions.mode = sl::DLSSMode::eOff;
		slDLSSSetOptions(viewport, dlssOptions);
	}
	slFreeResources(sl::kFeatureDLSS, viewport);
}

void Streamline::DestroyDLSSRRResources(bool modeSwitch)
{
	logger::debug("[Streamline] Destroying DLSS-RR resources");
	if (modeSwitch) {
		sl::DLSSDOptions dlssdOptions{};
		dlssdOptions.mode = sl::DLSSMode::eOff;
		slDLSSDSetOptions(viewport, dlssdOptions);
	}
	slFreeResources(sl::kFeatureDLSS_RR, viewport);
}

void Streamline::ApplyNISSharpening(ID3D12Resource* a_inputColorTexture, ID3D12Resource* a_outputTexture, float sharpness, ID3D12GraphicsCommandList* a_commandList)
{
	if (!featureNIS) {
		return;
	}

	CheckFrameConstants();

	sl::NISOptions nisOptions{};
	nisOptions.mode = sl::NISMode::eSharpen;
	nisOptions.sharpness = std::clamp(sharpness, 0.0f, 1.0f);
	nisOptions.hdrMode = sl::NISHDR::eNone;

	if (SL_FAILED(result, slNISSetOptions(viewport, nisOptions))) {
		logger::error("[Streamline] Could not set NIS options");
		return;
	}

	auto state = globals::state;
	sl::Extent fullExtent{ 0, 0, (uint)state->screenSize.x, (uint)state->screenSize.y };

	sl::Resource colorIn = { sl::ResourceType::eTex2d, a_inputColorTexture, 0 };
	sl::Resource colorOut = { sl::ResourceType::eTex2d, a_outputTexture, 0 };

	sl::ResourceTag colorInTag = sl::ResourceTag{ &colorIn, sl::kBufferTypeScalingInputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent };
	sl::ResourceTag colorOutTag = sl::ResourceTag{ &colorOut, sl::kBufferTypeScalingOutputColor, sl::ResourceLifecycle::eOnlyValidNow, &fullExtent };

	sl::ResourceTag resourceTags[] = { colorInTag, colorOutTag };

	if (SL_FAILED(result, SetTagsForCurrentFrame(resourceTags, _countof(resourceTags), a_commandList))) {
		logger::error("[Streamline] Failed to set NIS resource tags ({})", magic_enum::enum_name(result));
		return;
	}

	sl::ViewportHandle view(viewport);
	const sl::BaseStructure* inputs[] = { &view };
	if (SL_FAILED(result, slEvaluateFeature(sl::kFeatureNIS, *frameToken, inputs, _countof(inputs), a_commandList))) {
		logger::error("[Streamline] Failed to evaluate NIS feature");
	}
}
