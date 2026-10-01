#include "Upscaling.h"

#include "Deferred.h"
#include "Hooks.h"
#include "State.h"
#include "Upscaling/DX12SwapChain.h"
#include "Upscaling/FidelityFX.h"
#include "Upscaling/NeuralRendering/Integration.h"
#include "Upscaling/NeuralRendering/Renderer.h"
#include "Upscaling/NeuralRendering/Runtime.h"
#include "Upscaling/Streamline.h"
#include "VR.h"
#include <Windows.h>
#include <algorithm>
#include <directx/d3dx12.h>
#include <format>

#include "Features/PostProcessing.h"

#include "Features/ScreenSpaceRayTracing.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/PerformanceOverlay.h"
#include "Features/SubsurfaceScattering.h"
#include "Menu.h"
#include "Utils/GpuTimers.h"

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::NeuralRenderingSettings,
	enabled,
	intensity,
	localToneStrength,
	localStructureStrength,
	skinStructureStrength,
	style,
	useAutoMask,
	uiCorrection)

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	Upscaling::Settings,
	upscaleMethod,
	upscaleMethodNoDLSS,
	qualityMode,
	frameLimitMode,
	frameGenerationMode,
	frameGenerationBackend,
	dlssGFramesToGenerate,
	frameGenerationForceEnable,
	frameGenerationAllowInMenus,
	streamlineLogLevel,
	sharpnessFSR,
	sharpnessDLSS,
	DLSSPreset,
	enableDLSSRR,
	DLSSDPreset,
	reflexMode,
	reflexFrameLimit,
	neuralRendering);

namespace
{
	constexpr const char* kBatch33Experiments[] = {
		"SSGI: temporal + blur / REBLUR",
		"SSRT specular: Full / Checkerboard",
		"Neural Rendering alone: Off / On",
		"Neural Rendering with DLSS-G 2x: Off / On",
		"DLSS-G: 2x / 3x (NR off)",
		"DLSS-G: 2x / 4x (NR off)",
		"Neural Rendering with DLSS-G 3x: Off / On"
	};
	constexpr DWORD kBatch33SettleMs = 12000;  // Longer than the overlay's 10-second statistics window.

	struct Batch33ABState
	{
		struct OriginalSettings
		{
			bool valid = false;
			uint fgMode = 0;
			uint fgFrames = 1;
			bool nrEnabled = false;
			bool ssgiEnabled = false;
			bool ssgiGI = false;
			uint ssgiDenoiser = 0;
			bool ssrtDiffuse = false;
			bool ssrtSpecular = false;
			uint ssrtSpecularSampling = 0;
			bool overlayVisible = false;
			bool overlayEnabled = false;
			bool overlayDrawCalls = false;
		} original;

		int experiment = 0;
		int pending = -1;  // 0 = A, 1 = B; capture after the settings menu closes.
		ULONGLONG eligibleSince = 0;
		uint64_t runId = 0;
		uint64_t nrAttemptsAtArm = 0;
		uint64_t nrSuccessesAtArm = 0;
		uint64_t nrFailuresAtArm = 0;
		uint64_t fgSamplesAtArm = 0;
		uint64_t fgPresentedAtArm = 0;
		json a;
		json b;
		std::string verdict;
	};

	Batch33ABState& GetBatch33ABState()
	{
		static Batch33ABState state;
		return state;
	}

	void SaveBatch33OriginalSettings(Upscaling& upscaling, Batch33ABState& state)
	{
		if (state.original.valid)
			return;
		auto& original = state.original;
		const auto& ssgi = globals::features::screenSpaceGI;
		const auto& ssrt = globals::features::screenSpaceRayTracing;
		const auto& overlay = globals::features::performanceOverlay;
		original.valid = true;
		original.fgMode = upscaling.settings.frameGenerationMode;
		original.fgFrames = upscaling.settings.dlssGFramesToGenerate;
		original.nrEnabled = upscaling.settings.neuralRendering.enabled;
		original.ssgiEnabled = ssgi.settings.Enabled;
		original.ssgiGI = ssgi.settings.EnableGI;
		original.ssgiDenoiser = ssgi.settings.DenoiserMethod;
		original.ssrtDiffuse = ssrt.settings.EnableDiffuse;
		original.ssrtSpecular = ssrt.settings.EnableSpecular;
		original.ssrtSpecularSampling = ssrt.settings.SpecularSamplingMode;
		original.overlayVisible = globals::menu->overlayVisible;
		original.overlayEnabled = overlay.settings.ShowInOverlay;
		original.overlayDrawCalls = overlay.settings.ShowDrawCalls;
	}

	void RestoreBatch33OriginalSettings(Upscaling& upscaling, Batch33ABState& state)
	{
		const auto& original = state.original;
		if (!original.valid)
			return;
		auto& ssgi = globals::features::screenSpaceGI;
		auto& ssrt = globals::features::screenSpaceRayTracing;
		auto& overlay = globals::features::performanceOverlay;
		upscaling.settings.frameGenerationMode = original.fgMode;
		upscaling.settings.dlssGFramesToGenerate = original.fgFrames;
		upscaling.settings.neuralRendering.enabled = original.nrEnabled;
		ssgi.settings.Enabled = original.ssgiEnabled;
		ssgi.settings.EnableGI = original.ssgiGI;
		ssgi.settings.DenoiserMethod = original.ssgiDenoiser;
		ssgi.recompileFlag = true;
		ssrt.settings.EnableDiffuse = original.ssrtDiffuse;
		ssrt.settings.EnableSpecular = original.ssrtSpecular;
		ssrt.settings.SpecularSamplingMode = original.ssrtSpecularSampling;
		globals::menu->overlayVisible = original.overlayVisible;
		overlay.settings.ShowInOverlay = original.overlayEnabled;
		overlay.settings.ShowDrawCalls = original.overlayDrawCalls;
		state = {};
		logger::info("[B33AB] Original settings restored");
	}

	void ArmBatch33Run(Upscaling& upscaling, Batch33ABState& state, int arm)
	{
		SaveBatch33OriginalSettings(upscaling, state);
		auto& ssgi = globals::features::screenSpaceGI;
		auto& ssrt = globals::features::screenSpaceRayTracing;
		if (arm == 0) {
			++state.runId;
			state.a = json();
			state.b = json();
			state.verdict.clear();
		} else {
			state.b = json();
		}
		const bool variant = arm == 1;
		switch (state.experiment) {
		case 0:
			ssgi.settings.Enabled = true;
			ssgi.settings.EnableGI = true;
			ssrt.settings.EnableDiffuse = false;  // Otherwise SSRT owns diffuse IL.
			ssgi.settings.DenoiserMethod = variant ? ScreenSpaceGI::kDenoiserREBLUR : ScreenSpaceGI::kDenoiserLegacy;
			ssgi.recompileFlag = true;
			break;
		case 1:
			ssrt.settings.EnableSpecular = true;
			ssrt.settings.SpecularSamplingMode = variant ? ScreenSpaceRayTracing::kSamplingCheckerboard : ScreenSpaceRayTracing::kSamplingFull;
			break;
		case 2:
			upscaling.settings.frameGenerationMode = 0;
			upscaling.settings.neuralRendering.enabled = variant;
			break;
		case 3:
			upscaling.settings.frameGenerationMode = 1;
			upscaling.settings.dlssGFramesToGenerate = 1;
			upscaling.settings.neuralRendering.enabled = variant;
			break;
		case 4:
		case 5:
			upscaling.settings.frameGenerationMode = 1;
			upscaling.settings.neuralRendering.enabled = false;
			upscaling.settings.dlssGFramesToGenerate = variant ? static_cast<uint>(state.experiment == 4 ? 2 : 3) : 1;
			break;
		case 6:
			upscaling.settings.frameGenerationMode = 1;
			upscaling.settings.dlssGFramesToGenerate = 2;
			upscaling.settings.neuralRendering.enabled = variant;
			break;
		default:
			return;
		}
		// The GPU pass timers intentionally run only while the draw-call table is visible.
		// Keep that instrumentation on for both samples, including while the menu is closed.
		globals::menu->overlayVisible = true;
		auto& overlay = globals::features::performanceOverlay;
		overlay.settings.ShowInOverlay = true;
		overlay.settings.ShowDrawCalls = true;
		Util::GpuPassTimers::GetSingleton()->Reset();
		state.pending = arm;
		state.eligibleSince = 0;
		logger::info("[B33AB] run={} experiment={} arm={} requestedFG={} requestedNR={} requestedMFG={}; close settings menu and play the same scene",
			state.runId, kBatch33Experiments[state.experiment], variant ? "B" : "A",
			upscaling.settings.frameGenerationMode, upscaling.settings.neuralRendering.enabled,
			upscaling.settings.dlssGFramesToGenerate + 1);
	}

	json CaptureBatch33Snapshot(Upscaling& upscaling, const Batch33ABState& state, int arm)
	{
		const auto& ssgi = globals::features::screenSpaceGI;
		const auto& ssrt = globals::features::screenSpaceRayTracing;
		const auto& overlay = globals::features::performanceOverlay;
		const auto nr = NeuralRendering::GetDiagnostics();
		const auto fg = upscaling.dx12SwapChain.GetDLSSGCadenceTotals();
		const bool nrCountersReset = nr.attempts < state.nrAttemptsAtArm ||
			nr.successes < state.nrSuccessesAtArm || nr.failures < state.nrFailuresAtArm;
		const uint64_t nrAttemptDelta = nrCountersReset ? nr.attempts : nr.attempts - state.nrAttemptsAtArm;
		const uint64_t nrSuccessDelta = nrCountersReset ? nr.successes : nr.successes - state.nrSuccessesAtArm;
		const uint64_t nrFailureDelta = nrCountersReset ? nr.failures : nr.failures - state.nrFailuresAtArm;
		const bool fgCountersReset = fg.samples < state.fgSamplesAtArm || fg.presented < state.fgPresentedAtArm;
		const uint64_t fgSampleDelta = fgCountersReset ? fg.samples : fg.samples - state.fgSamplesAtArm;
		const uint64_t fgPresentedDelta = fgCountersReset ? fg.presented : fg.presented - state.fgPresentedAtArm;
		json snapshot;
		snapshot["run"] = state.runId;
		snapshot["experiment"] = kBatch33Experiments[state.experiment];
		snapshot["arm"] = arm == 0 ? "A" : "B";
		snapshot["request"] = {
			{ "upscaler", static_cast<uint>(upscaling.GetUpscaleMethod()) },
			{ "fgBackend", static_cast<uint>(upscaling.GetFrameGenerationBackend()) },
			{ "fg", upscaling.settings.frameGenerationMode },
			{ "fgGeneratedFrames", upscaling.settings.dlssGFramesToGenerate },
			{ "nr", upscaling.settings.neuralRendering.enabled },
			{ "ssgi", ssgi.settings.Enabled },
			{ "ssgiGI", ssgi.settings.EnableGI },
			{ "ssgiDenoiser", ssgi.settings.DenoiserMethod },
			{ "ssrtDiffuse", ssrt.settings.EnableDiffuse },
			{ "ssrtSpecular", ssrt.settings.EnableSpecular },
			{ "ssrtSpecularSampling", ssrt.settings.SpecularSamplingMode },
			{ "ssrtDenoiser", ssrt.settings.DenoiserMethod },
			{ "dlssRR", upscaling.settings.enableDLSSRR }
		};
		snapshot["effective"] = {
			{ "ssgiReblur", ssgi.WantsReblurGuides() },
			{ "ssgiReblurOutput", ssgi.GetReblurDiffuseOutputSRV() != nullptr },
			{ "ssrtSpecularSampling", ssrt.activeSpecularSamplingMode },
			{ "ssrtSpecularDenoiser", ssrt.EffectiveDenoiser(true) },
			{ "nrApplied", nr.appliedThisFrame },
			{ "nrStatus", nr.status },
			{ "nrSizeContract", nr.sizeContractSatisfied },
			{ "nrFailureLatched", nr.failureLatched },
			{ "nrAttempts", nr.attempts },
			{ "nrSuccesses", nr.successes },
			{ "nrFailures", nr.failures },
			{ "nrCounterResetDuringArm", nrCountersReset },
			{ "nrAttemptsDuringArm", nrAttemptDelta },
			{ "nrSuccessesDuringArm", nrSuccessDelta },
			{ "nrFailuresDuringArm", nrFailureDelta },
			{ "nrSceneWidth", nr.sceneWidth },
			{ "nrSceneHeight", nr.sceneHeight },
			{ "nrDepthWidth", nr.depthWidth },
			{ "nrDepthHeight", nr.depthHeight },
			{ "nrMotionWidth", nr.motionWidth },
			{ "nrMotionHeight", nr.motionHeight },
			{ "nrNgxResult", nr.ngxResult },
			{ "nrSceneResource", nr.sceneResource },
			{ "nrDepthResource", nr.depthResource },
			{ "nrMotionResource", nr.motionResource },
			{ "nrSceneFormat", nr.sceneFormat },
			{ "nrDepthFormat", nr.depthFormat },
			{ "nrMotionFormat", nr.motionFormat },
			{ "fgActive", upscaling.streamline.IsDLSSGActive() },
			{ "fgAppliedGeneratedFrames", upscaling.streamline.GetDLSSGAppliedFramesToGenerate() },
			{ "fgMaxGeneratedFrames", upscaling.streamline.GetDLSSGMaxFramesToGenerate() },
			{ "fgCounterResetDuringArm", fgCountersReset },
			{ "fgPresentSamplesDuringArm", fgSampleDelta },
			{ "fgPresentedFramesDuringArm", fgPresentedDelta },
			{ "fgMeanPresentedPerRenderedFrame", fgSampleDelta ?
				static_cast<float>(fgPresentedDelta) / static_cast<float>(fgSampleDelta) : 0.0f },
			{ "fgRuntimeFaulted", upscaling.streamline.dlssGRuntimeFaulted }
		};
		const auto frameStats = overlay.ComputeFrameStats();
		snapshot["metrics"] = {
			{ "frameStatsValid", frameStats.valid },
			{ "rawMeanMs", frameStats.averageMs },
			{ "rawP99Ms", frameStats.percentile99Ms },
			{ "fgMeasuredMultiplier", upscaling.GetFrameGenerationPresentMultiplier() }
		};
		snapshot["gpuMs"] = json::object();
		Util::GpuPassTimers::GetSingleton()->ForEachActiveBucket([&snapshot](const Util::GpuPassTimers::BucketReport& report) {
			snapshot["gpuMs"][report.label] = report.smoothedMs;
		});
		return snapshot;
	}

	std::string CompareBatch33Snapshots(const json& a, const json& b)
	{
		const auto requestDiff = json::diff(a.at("request"), b.at("request"));
		const bool singleSettingPair = requestDiff.size() == 1;
		const bool metricsValid = a.at("metrics").at("frameStatsValid").get<bool>() &&
			b.at("metrics").at("frameStatsValid").get<bool>();
		bool effectiveValid = true;
		json summary = {
			{ "run", b.at("run") },
			{ "singleSettingPair", singleSettingPair },
			{ "metricsValid", metricsValid },
			{ "requestDiff", requestDiff },
			{ "rawMeanMsDelta", b.at("metrics").at("rawMeanMs").get<float>() - a.at("metrics").at("rawMeanMs").get<float>() },
			{ "gpuMsDelta", json::object() }
		};
		for (auto it = b.at("gpuMs").begin(); it != b.at("gpuMs").end(); ++it) {
			if (a.at("gpuMs").contains(it.key()))
				summary["gpuMsDelta"][it.key()] = it.value().get<float>() - a.at("gpuMs").at(it.key()).get<float>();
		}
		std::string verdict = singleSettingPair ? "one requested setting changed" : "confounded: expected exactly one requested setting change";
		if (!metricsValid)
			verdict += "; frame timing window is invalid";
		const auto& effective = b.at("effective");
		const int experiment = GetBatch33ABState().experiment;
		if (experiment == 0 && (!effective.at("ssgiReblur").get<bool>() ||
			!effective.at("ssgiReblurOutput").get<bool>())) {
			verdict += "; SSGI REBLUR did not publish an output";
			effectiveValid = false;
		}
		if (experiment == 1 && effective.at("ssrtSpecularSampling").get<uint>() != ScreenSpaceRayTracing::kSamplingCheckerboard) {
			verdict += "; SSRT checkerboard fell back to Full";
			effectiveValid = false;
		}
		if ((experiment == 2 || experiment == 3 || experiment == 6) &&
			(!effective.at("nrApplied").get<bool>() ||
			 effective.at("nrSuccessesDuringArm").get<uint64_t>() == 0 ||
			 effective.at("nrCounterResetDuringArm").get<bool>())) {
			verdict += "; NR did not run: " + effective.at("nrStatus").get<std::string>();
			effectiveValid = false;
		}
		if ((experiment == 2 || experiment == 3 || experiment == 6) &&
			effective.at("nrFailuresDuringArm").get<uint64_t>() > 0) {
			verdict += "; NR had failures during the B sample";
			effectiveValid = false;
		}
		if (experiment >= 3) {
			for (const auto* sample : { &a, &b }) {
				const auto& fg = sample->at("effective");
				const uint requested = sample->at("request").at("fgGeneratedFrames").get<uint>();
				const auto arm = sample->at("arm").get<std::string>();
				if (!fg.at("fgActive").get<bool>() || fg.at("fgCounterResetDuringArm").get<bool>() ||
					fg.at("fgAppliedGeneratedFrames").get<uint>() < requested ||
					fg.at("fgPresentSamplesDuringArm").get<uint64_t>() < 8 ||
					fg.at("fgMeanPresentedPerRenderedFrame").get<float>() + 0.25f < static_cast<float>(requested + 1)) {
					verdict += "; DLSS-G " + arm + " did not demonstrate its requested presentation cadence";
					effectiveValid = false;
				}
			}
		}
		summary["effectiveValid"] = effectiveValid;
		summary["cleanPair"] = singleSettingPair && metricsValid && effectiveValid;
		summary["verdict"] = verdict;
		logger::info("[B33AB] compare {}", summary.dump());
		return verdict;
	}

	void DrawBatch33ABLab(Upscaling& upscaling)
	{
		if (globals::game::isVR || !ImGui::TreeNodeEx("Batch33 A/B Lab"))
			return;
		auto& state = GetBatch33ABState();
		if (ImGui::Combo("Experiment", &state.experiment, kBatch33Experiments, IM_ARRAYSIZE(kBatch33Experiments))) {
			state.a = json();
			state.b = json();
			state.pending = -1;
			state.verdict.clear();
		}
		ImGui::TextWrapped("Run A, close this menu and play the fixed scene for 12 seconds. Reopen it, Run B, then repeat. Capture and comparison are automatic; the log contains every requested and effective state plus GPU timing rows.");
		if (ImGui::Button("Run A"))
			ArmBatch33Run(upscaling, state, 0);
		ImGui::SameLine();
		if (ImGui::Button("Run B")) {
			if (state.a.is_object())
				ArmBatch33Run(upscaling, state, 1);
			else
				state.verdict = "Capture A first, then run B in the same scene.";
		}
		ImGui::SameLine();
		if (ImGui::Button("Restore original settings"))
			RestoreBatch33OriginalSettings(upscaling, state);
		ImGui::Text("A: %s  B: %s", state.a.is_object() ? "captured" : "waiting", state.b.is_object() ? "captured" : "waiting");
		if (state.pending >= 0)
			ImGui::TextDisabled("Capture %c armed; close the settings menu and unpause gameplay.", state.pending == 0 ? 'A' : 'B');
		if (!state.verdict.empty())
			ImGui::TextWrapped("%s", state.verdict.c_str());
		if (upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS && state.experiment >= 2)
			ImGui::TextColored({ 1.0f, 0.7f, 0.2f, 1.0f }, "NR experiments require DLSS upscaling.");
		if (state.experiment >= 3 && !upscaling.IsDLSSGBackend())
			ImGui::TextColored({ 1.0f, 0.7f, 0.2f, 1.0f }, "DLSS-G must be selected before launching the game; use the package preflight script.");
		ImGui::TextDisabled("SSRT remains a separate feature in batch33; upstream efficiency candidates have not been ported.");
		ImGui::TreePop();
	}
}

decltype(&D3D11CreateDeviceAndSwapChain) ptrD3D11CreateDeviceAndSwapChainUpscaling;

/**
 * @brief Creates a Direct3D 11 device and swap chain, with support for advanced upscaling and frame generation features.
 *
 * This function intercepts the standard D3D11 device and swap chain creation process to enable integration with Streamline and FidelityFX technologies, as well as optional D3D12 proxying for frame generation. It adjusts swap chain flags for tearing support, manages feature checks, and conditionally routes device creation through Streamline or FidelityFX proxies based on runtime settings and hardware capabilities. If frame generation is enabled and supported, a D3D12 proxy is used; otherwise, the standard D3D11 creation path is followed.
 *
 * @return HRESULT indicating the success or failure of device and swap chain creation.
 */
HRESULT WINAPI hk_D3D11CreateDeviceAndSwapChainUpscaling(
	IDXGIAdapter* pAdapter,
	D3D_DRIVER_TYPE DriverType,
	HMODULE Software,
	UINT Flags,
	[[maybe_unused]] const D3D_FEATURE_LEVEL* pFeatureLevels,
	[[maybe_unused]] UINT FeatureLevels,
	UINT SDKVersion,
	DXGI_SWAP_CHAIN_DESC* pSwapChainDesc,
	IDXGISwapChain** ppSwapChain,
	ID3D11Device** ppDevice,
	D3D_FEATURE_LEVEL* pFeatureLevel,
	ID3D11DeviceContext** ppImmediateContext)
{
	DXGI_ADAPTER_DESC adapterDesc;
	pAdapter->GetDesc(&adapterDesc);
	globals::state->SetAdapterDescription(adapterDesc.Description);

	auto& upscaling = globals::features::upscaling;
	upscaling.LoadUpscalingSDKs();

	if (upscaling.IsBackendInitialized())
		upscaling.CheckBackendFeatures(pAdapter);

	// Use better swap effect to prevent tearing and improve performance
	pSwapChainDesc->SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

	// bool shouldProxy = !globals::game::isVR || upscaling.streamline.featureDLSS;
	// if (shouldProxy)
	// 	if (!pSwapChainDesc->Windowed)
	// 		shouldProxy = false;

	auto refreshRate = Upscaling::GetRefreshRate(pSwapChainDesc->OutputWindow);
	upscaling.refreshRate = refreshRate;

	// if (shouldProxy) {
	// 	if (upscaling.settings.frameGenerationMode)
	// 		if (refreshRate >= 120)
	// 			shouldProxy = true;
	// 		else if (upscaling.settings.frameGenerationForceEnable)
	// 			shouldProxy = true;
	// 		else
	// 			shouldProxy = false;
	// 	else
	// 		shouldProxy = false;
	// 	if (upscaling.settings.upscaleMethod == (uint)Upscaling::UpscaleMethod::kDLSS)
	// 		shouldProxy = true;
	// }

	bool shouldProxy = true;

	upscaling.lowRefreshRate = refreshRate < 120;
	upscaling.isWindowed = pSwapChainDesc->Windowed;

	const D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_1;

	const bool dlssGStartupUnavailable = upscaling.IsDLSSGBackend() && !upscaling.HasFrameGenModule();
	if (dlssGStartupUnavailable)
		logger::error("[Upscaling] DLSS-G was selected at startup but its required Streamline features are unavailable; using the native D3D11 path for this session");

	if (shouldProxy && upscaling.isWindowed && !dlssGStartupUnavailable) {
		logger::info("[Upscaling] Using D3D12 proxy");

		if (upscaling.HasFrameGenModule() || upscaling.streamline.featureDLSS) {
			DX::ThrowIfFailed(D3D11CreateDevice(
				pAdapter,
				DriverType,
				Software,
				Flags,
				&featureLevel,
				1,
				SDKVersion,
				ppDevice,
				pFeatureLevel,
				ppImmediateContext));

			upscaling.SetProxyD3D11Device(*ppDevice);
			upscaling.SetProxyD3D11DeviceContext(*ppImmediateContext);
			upscaling.CreateProxySwapChain(pAdapter, *pSwapChainDesc);
			upscaling.CreateProxyInterop();

			*ppSwapChain = upscaling.GetProxySwapChain();

			upscaling.d3d12SwapChainActive = true;

			if (upscaling.IsBackendInitialized()) {
				if (upscaling.IsDLSSGBackend() && upscaling.streamline.featureDLSS_G && upscaling.streamline.featureReflex && upscaling.streamline.featurePCL) {
					// DLSS-G registered the native D3D12 device and loaded its
					// feature functions before command queue/swap-chain creation.
				} else {
					// Preserve the existing FSR/DLSS Streamline setup. FFX owns
					// the swap chain in this branch, so do not change its creation
					// ordering here.
					auto d3d12Device = upscaling.dx12SwapChain.d3d12Device.get();
					upscaling.UpgradeBackendInterface((void**)&d3d12Device);
					upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
					upscaling.SetBackendD3DDevice((void*)d3d12Device);
				}
				if (!upscaling.IsDLSSGBackend())
					upscaling.PostBackendDevice();
			}

			return S_OK;
		} else {
			logger::warn("[Upscaling] Skipping proxy");
			if (upscaling.GetFrameGenerationBackend() == Upscaling::FrameGenerationBackend::kFSR3FG)
				upscaling.fidelityFXMissing = true;
		}
	}

	auto ret = ptrD3D11CreateDeviceAndSwapChainUpscaling(pAdapter,
		DriverType,
		Software,
		Flags,
		&featureLevel,
		1,
		SDKVersion,
		pSwapChainDesc,
		ppSwapChain,
		ppDevice,
		pFeatureLevel,
		ppImmediateContext);

	// if (upscaling.IsBackendInitialized()) {
	// 	upscaling.UpgradeBackendInterface((void**)&(*ppDevice));
	// 	upscaling.UpgradeBackendInterface((void**)&(*ppSwapChain));
	// 	upscaling.SetBackendD3DDevice(*ppDevice);
	// 	upscaling.PostBackendDevice();
	// }

	return ret;
}

void Upscaling::DrawSettings()
{
	// Display upscaling options in the UI
	std::vector<std::string> upscaleModes = { "None", "TAA" };

	std::string fsrLabel = "AMD FSR 3.1";
	upscaleModes.push_back(fsrLabel);

	std::string dlssLabel = "NVIDIA DLSS";
	upscaleModes.push_back(dlssLabel);

	// Determine available modes
	bool featureDLSS = streamline.featureDLSS;
	bool featureFSR = true;  // FSR is always available

	uint32_t* currentUpscaleMode = &settings.upscaleMethod;
	uint32_t availableModes = 1;  // Start with TAA
	if (featureFSR)
		availableModes = 2;  // Add FSR
	if (featureDLSS)
		availableModes = 3;  // Add DLSS if available
	else
		currentUpscaleMode = &settings.upscaleMethodNoDLSS;

	// Slider for method selection
	// Clamp the index used to read from the built label vector to avoid OOB if the stored value is stale
	uint32_t modeLabelIndex = std::min(*currentUpscaleMode, static_cast<uint32_t>(upscaleModes.size() - 1));
	std::string currentLabel = upscaleModes[modeLabelIndex];
	ImGui::SliderInt("Method", (int*)currentUpscaleMode, 0, availableModes, currentLabel.c_str());

	*currentUpscaleMode = std::min(availableModes, *currentUpscaleMode);

	// Check the current upscale method
	auto upscaleMethod = GetUpscaleMethod();

	// Display upscaling settings if applicable
	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		const char* upscalePresetsDLSS[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "DLAA" };
		const char* upscalePresets[] = { "Ultra Performance", "Performance", "Balanced", "Quality", "Native AA" };

		// Compute a safe preset index (4 - qualityMode) clamped to [0,4] to avoid negative/overflow indexing
		int presetIndex = 0;
		if (settings.qualityMode <= 4)
			presetIndex = 4 - static_cast<int>(settings.qualityMode);
		presetIndex = std::clamp(presetIndex, 0, 4);

		// Choose preset name set and the corresponding scales once, then show a
		// single SliderInt to avoid duplicated calls.
		const char* baseLabel = nullptr;

		if (upscaleMethod == UpscaleMethod::kFSR) {
			baseLabel = upscalePresets[presetIndex];
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			baseLabel = upscalePresetsDLSS[presetIndex];
		}

		if (baseLabel) {
			// Format the label with preset name and resolution scale
			std::string labelWithScale = std::format("{} ( {:.2f}x )", baseLabel, (resolutionScale.x + resolutionScale.y) * 0.5f);

			ImGui::SliderInt("Upscale Preset", (int*)&settings.qualityMode, 0, 4, labelWithScale.c_str());
		}

		if (upscaleMethod == UpscaleMethod::kFSR) {
			ImGui::SliderFloat("Sharpness", &settings.sharpnessFSR, 0.0f, 1.0f, "%.1f");
		} else if (upscaleMethod == UpscaleMethod::kDLSS) {
			ImGui::SliderFloat("Sharpness", &settings.sharpnessDLSS, 0.0f, 1.0f, "%.1f");

			const bool rayReconstructionActive = settings.enableDLSSRR && streamline.featureDLSS_RR;
			if (rayReconstructionActive)
				ImGui::BeginDisabled();

			const char* presets[] = {
				"F (Legacy forced override)",
				"J (Forced override)",
				"K (Forced override)",
				"L (Forced override)",
				"M (Forced override)",
				"Streamline 2.12 documented mapping (K/K/K/M/L)"
			};
			int dlssPresetIndex = static_cast<int>(settings.DLSSPreset);
			if (ImGui::Combo("DLSS SR Model Preset", &dlssPresetIndex, presets, IM_ARRAYSIZE(presets)))
				settings.DLSSPreset = static_cast<uint>(dlssPresetIndex);

			if (rayReconstructionActive)
				ImGui::EndDisabled();

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("F through M force one DLSS model for the five exposed modes (DLAA/Quality/Balanced/Performance/Ultra Performance).");
				ImGui::TextUnformatted("The Streamline 2.12 documented mapping uses K for DLAA/Quality/Balanced, M for Performance, and L for Ultra Performance.");
			}
		}
	}

	if (upscaleMethod == UpscaleMethod::kDLSS && streamline.featureDLSS_RR && ImGui::TreeNodeEx("Ray Reconstruction")) {
		ImGui::Checkbox("Enable DLSS Ray Reconstruction", &settings.enableDLSSRR);
		if (settings.enableDLSSRR) {
			ImGui::TextDisabled("DLSS SR model presets do not apply while Ray Reconstruction is enabled.");

			// Only D and F are offered. The NVIDIA App's Ray Reconstruction dropdown exposes
			// nothing but Recommended and Preset F, which is the only signal available from
			// outside NVIDIA about which models are worth running at all.
			const char* dlssdPresets[] = {
				"D (previous default)",
				"F (DLSS 4.5, NVIDIA's current default)"
			};
			int dlssdPresetIndex = settings.DLSSDPreset == static_cast<uint>(DLSSDModelPreset::kF) ? 1 : 0;
			if (ImGui::Combo("DLSS RR Model Preset", &dlssdPresetIndex, dlssdPresets, IM_ARRAYSIZE(dlssdPresets)))
				settings.DLSSDPreset = static_cast<uint>(dlssdPresetIndex == 1 ? DLSSDModelPreset::kF : DLSSDModelPreset::kD);

			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Selects the Ray Reconstruction model for all five quality modes. Takes effect on the next frame, so the two can be compared back to back.");
				ImGui::TextUnformatted("F is the DLSS 4.5 second-generation transformer model, and as of the DLSS 310.9.1 / Streamline 2.14.1 SDKs shipped here it is both official and NVIDIA's own default -- their header calls ePresetF the latest and default transformer model, where the previous SDK still listed it as reverting to default. D is kept as the comparison baseline: it is what this feature ran hardcoded before the selector existed.");
				ImGui::TextUnformatted("To check which model was actually requested, read the '[DLSS RR] Requested model preset' line in CommunityShaders.log alongside the 'nvngx_dlssd.dll version' line logged at startup.");
			}
		}
		ImGui::TreePop();
	}

	if (upscaleMethod == UpscaleMethod::kDLSS && !globals::game::isVR && ImGui::TreeNodeEx("Neural Rendering (DLSS 5, experimental)")) {
		auto& nr = settings.neuralRendering;
		ImGui::Checkbox("Enable Neural Rendering", &nr.enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("DLSS 5's neural renderer, run on the finished tonemapped frame just before the interface is drawn. It re-lights the image rather than reconstructing it, so unlike every other feature here it reads no G-buffer and feeds nothing back into lighting.");
			ImGui::TextUnformatted("Expect a large cost: roughly 37% of frame time measured on an RTX 5090, and about 39% on an RTX 4090 with the community Ada build.");
		}

		if (nr.enabled) {
			if (d3d12SwapChainActive && IsDLSSGBackend() && IsFrameGenerationRequestedNow()) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::TextUnformatted("NR + DLSS-G coexistence test is requested. Check the Batch33 A/B Lab and log for actual execution.");
				ImGui::PopStyleColor();
			}

			// Probe as soon as the feature is switched on, rather than waiting for the frame path
			// to reach it. The runtime is otherwise only probed from inside Renderer::Apply, and
			// that happens *after* the D3D12 interop is brought up -- so an interop failure used
			// to leave this panel reading "not probed", which says nothing about the one thing
			// the user can actually act on: whether the DLL is there. Probe() resets on entry, so
			// this is only safe while nothing has been initialised, which NotProbed guarantees.
			// A failed probe moves the status off NotProbed, so this does not retry every frame.
			if (NeuralRendering::Runtime::Instance().Status() == NeuralRendering::RuntimeStatus::NotProbed)
				NeuralRendering::Runtime::Instance().Probe();

			const auto status = NeuralRendering::Runtime::Instance().Status();
			const auto& detail = NeuralRendering::Runtime::Instance().Detail();

			if (NeuralRendering::Renderer::Instance().IsFailureLatched()) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("A pass failed and the feature latched off. See the [DLSSNR] lines in the log.");
				ImGui::PopStyleColor();
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted("The latch exists so a failing pass cannot retry every frame. The D3D12 interop is brought up before the runtime is probed, so an interop failure latches here while the runtime status above still reads as never probed -- the log line names which of the two failed. Reset Neural Rendering clears the latch.");
			}
			if (status == NeuralRendering::RuntimeStatus::Initialized) {
				ImGui::Text("Runtime %s", NeuralRendering::Runtime::Instance().Version().c_str());
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Runtime %s%s%s", NeuralRendering::ToString(status),
					detail.empty() ? "" : ": ", detail.c_str());
				ImGui::PopStyleColor();
			}
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("nvngx_dlssnr.dll is not shipped with this mod and has to be placed in Data/Shaders/Upscaling/Streamline yourself. NVIDIA's own build runs on GeForce RTX 50 only; anything older needs a community-recompiled DLL, and neither is ours to redistribute.");
				ImGui::TextUnformatted("A version of 310.8.x is required -- that check is what stops an unrelated DLL of the same name from being handed to NGX.");
			}

			ImGui::SliderFloat("Intensity", &nr.intensity, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Local Tone Strength", &nr.localToneStrength, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Local Structure Strength", &nr.localStructureStrength, 0.f, 1.f, "%.2f");
			ImGui::SliderFloat("Skin Structure Strength", &nr.skinStructureStrength, 0.f, 1.f, "%.2f");

			int style = static_cast<int>(nr.style);
			if (ImGui::SliderInt("Style", &style, 0, 3, "%d", ImGuiSliderFlags_AlwaysClamp))
				nr.style = static_cast<uint>(std::clamp(style, 0, 3));

			ImGui::Checkbox("Auto Mask", &nr.useAutoMask);
			ImGui::Checkbox("UI Correction", &nr.uiCorrection);

			if (ImGui::Button("Reset Neural Rendering", { -1, 0 }))
				NeuralRendering::Reset();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Drops the runtime, its D3D12 device and every shared texture, then lets the next frame rebuild them. Use it after replacing the DLL.");
		}
	}

	if (!globals::game::isVR) {
		if (ImGui::TreeNodeEx("Frame Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Frame Generation interpolates real frames with generated ones for a smoother experience");

			const char* frameGenerationBackends[] = { "AMD FSR 3.1", "NVIDIA DLSS-G" };
			const auto configuredBackend = GetConfiguredFrameGenerationBackend();
			int backendIndex = static_cast<int>(configuredBackend);
			if (ImGui::Combo("Frame Generation Backend", &backendIndex, frameGenerationBackends, IM_ARRAYSIZE(frameGenerationBackends)))
				settings.frameGenerationBackend = static_cast<uint>(backendIndex);

			const auto activeBackend = GetFrameGenerationBackend();
			if (configuredBackend != activeBackend) {
				ImGui::TextDisabled("Backend availability will be checked after restart.");
			} else if (configuredBackend == FrameGenerationBackend::kFSR3FG) {
				if (fidelityFX.featureFSR3FG)
					ImGui::Text("AMD FSR 3.1 Frame Generation is available.");
			} else if (IsDLSSGAvailable()) {
				ImGui::Text("NVIDIA DLSS-G Frame Generation is available.");
			} else {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: NVIDIA DLSS-G is not available through Streamline");
				ImGui::PopStyleColor();
			}

			ImGui::Text("Requires a D3D11 to D3D12 proxy which can create compatibility issues");
			ImGui::Text("Backend is selected at game startup; changing it requires a restart");
			if (activeBackend == FrameGenerationBackend::kDLSSG) {
				ImGui::Text("On this backend it can be switched off and back on freely, in either direction");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted("A DLSS-G session stands generation up whether or not this setting starts on, so the toggle below is live. Turning it off already happened every time a menu opened, with resources retained so it can resume cheaply; what used to need a restart was turning it back on, because the session only prepared the proxy path when it booted with the setting enabled. It now always does, which is what makes switching generation off to use Neural Rendering a decision you can reverse without leaving the game.");
			} else if (frameGenerationEnabledAtStartup) {
				ImGui::Text("This session booted with it on, so it can be switched off and back on freely");
			} else {
				ImGui::Text("This session booted with it off, so switching it on requires a restart");
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted("FSR frame generation owns its own FidelityFX swapchain and has no equivalent of DLSS-G's per-frame suspend, so its enable state is still read once, at startup. The DLSS-G backend does not have this restriction.");
			}

			if (!isWindowed) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires windowed mode");
				ImGui::PopStyleColor();
			}

			if (lowRefreshRate && !settings.frameGenerationForceEnable) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Requires a high refresh rate monitor or Force Enable Frame Generation");
				ImGui::PopStyleColor();
			}

			if (fidelityFXMissing && activeBackend == FrameGenerationBackend::kFSR3FG) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: FidelityFX DLLs are not loaded");
				ImGui::PopStyleColor();
			}

			const bool backendPendingRestart = frameGenerationBackendLatched && configuredBackend != frameGenerationBackendAtStartup;
			// Only the FSR path still reads its enable state once. On DLSS-G the toggle is live
			// in both directions, so warning about a restart there would be wrong.
			const bool enabledPendingRestart = frameGenerationBackendLatched &&
			                                   activeBackend != FrameGenerationBackend::kDLSSG &&
			                                   (settings.frameGenerationMode != (frameGenerationEnabledAtStartup ? 1u : 0u));
			if (backendPendingRestart || enabledPendingRestart) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Warning: Restart the game to apply frame-generation changes");
				ImGui::PopStyleColor();
			}

			std::string enabledLabel = "Enabled";
			const char* toggleModes[] = { "Disabled", "Enabled" };
			const char* toggleModesFG[] = { "Disabled", enabledLabel.c_str() };

			ImGui::SliderInt("Frame Generation", (int*)&settings.frameGenerationMode, 0, 1, toggleModesFG[settings.frameGenerationMode]);
			if (activeBackend == FrameGenerationBackend::kDLSSG) {
				int generatedFrames = static_cast<int>(settings.dlssGFramesToGenerate);
				if (ImGui::SliderInt("DLSS-G generated frames", &generatedFrames, 1, 3,
						"%d per rendered frame", ImGuiSliderFlags_AlwaysClamp))
					settings.dlssGFramesToGenerate = static_cast<uint>(std::clamp(generatedFrames, 1, 3));
				if (streamline.GetDLSSGMaxFramesToGenerate() == 0)
					ImGui::Text("Requested %ux; applied %ux; hardware maximum pending",
						settings.dlssGFramesToGenerate + 1,
						streamline.GetDLSSGAppliedFramesToGenerate() + 1);
				else
					ImGui::Text("Requested %ux; applied %ux; hardware maximum %ux",
						settings.dlssGFramesToGenerate + 1,
						streamline.GetDLSSGAppliedFramesToGenerate() + 1,
						streamline.GetDLSSGMaxFramesToGenerate() + 1);
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted("1/2/3 generated frames request 2x/3x/4x. Streamline clamps to the hardware maximum; use the measured post-FG multiplier to confirm actual presentation.");
			}

			if (!d3d12SwapChainActive)
				ImGui::BeginDisabled();

			ImGui::SliderInt("Frame Limit (VSync off only)", (int*)&settings.frameLimitMode, 0, 1, std::format("{}", toggleModes[settings.frameLimitMode]).c_str());
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Paces presentation to the refresh rate, or the selected generation fraction while generating. It runs only when the game presents with a sync interval of zero, so anything that turns VSync on -- including SSE Display Tweaks, which owns that setting for most setups -- leaves it inert.");
				ImGui::TextUnformatted("It also spins after Present, which is the wrong side of the frame for a cap: it holds the CPU once the work is already submitted. Under a variable refresh rate prefer the Reflex frame limit below, which is applied before input is sampled and which the driver is aware of.");
			}

			if (!d3d12SwapChainActive)
				ImGui::EndDisabled();

			ImGui::Text("Allows frame generation to function on low refresh rate monitors");
			ImGui::SliderInt("Force Enable Frame Generation", (int*)&settings.frameGenerationForceEnable, 0, 1, std::format("{}", toggleModes[settings.frameGenerationForceEnable]).c_str());

			ImGui::Checkbox("Frame Generation in Menus", &settings.frameGenerationAllowInMenus);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Keeps generation running while a paused menu is open -- inventory, the journal, the system menu. Without it, generation stops the moment the game pauses, which is why the frame rate drops as soon as a menu appears.");
				ImGui::TextUnformatted("The main menu and loading screens stay excluded: there is no world scene behind them, so there would be nothing to interpolate but the menu itself. The map keeps its own staged recovery either way.");
				ImGui::TextUnformatted("Menus are where the latency generation adds is easiest to notice and the extra smoothness worth least, which is why this is off by default.");
			}

			ImGui::TreePop();
		}
	} else {
		if (ImGui::TreeNodeEx("Frame Generation", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::Text("Frame Generation is not available on your system.\nThis requires either NVIDIA DLSS-G or AMD FSR 3.1 Frame Generation support and D3D12 interop.");
			ImGui::TreePop();
		}
	}

	if (!globals::game::isVR && ImGui::TreeNodeEx("Latency (NVIDIA Reflex)")) {
		if (!streamline.featureReflex) {
			ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
			ImGui::Text("Reflex is not available through Streamline on this system.");
			ImGui::PopStyleColor();
		} else {
			const char* reflexModes[] = { "Off", "On", "On + Boost" };
			int reflexModeIndex = static_cast<int>(std::min(settings.reflexMode, 2u));
			if (ImGui::Combo("Reflex Low Latency", &reflexModeIndex, reflexModes, IM_ARRAYSIZE(reflexModes)))
				settings.reflexMode = static_cast<uint>(reflexModeIndex);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Holds the render queue so the CPU does not run further ahead of the GPU than the frame needs. This is where latency is actually removed, and it happens in the game's own update, before input is sampled.");
				ImGui::TextUnformatted("Boost additionally keeps GPU clocks up when the frame is CPU-bound. It costs power for a small further latency reduction and is worth little unless the GPU is idling.");
			}

			if (IsFrameGenerationRequestedNow() && settings.reflexMode == 0) {
				ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
				ImGui::Text("Held at On: Frame Generation requires Reflex.");
				ImGui::PopStyleColor();
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted("NVIDIA requires Reflex whenever DLSS-G is generating, and effectively no shipping game exposes the other combination. DLSS-G paces its own presentation, so without Reflex holding the render queue the game runs ahead of a cadence it no longer controls -- the case where generation adds the most latency. The setting above is left as you chose it and takes effect again once generation is off.");
			}

			int reflexFrameLimit = static_cast<int>(settings.reflexFrameLimit);
			if (ImGui::SliderInt("Reflex Frame Limit", &reflexFrameLimit, 0, 240,
					reflexFrameLimit > 0 ? "%d presented fps" : "Off", ImGuiSliderFlags_AlwaysClamp))
				settings.reflexFrameLimit = static_cast<uint>(std::max(reflexFrameLimit, 0));
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Caps frames as they reach the display, not as they are rendered. The driver knows the generation multiplier, so a 60 presented fps cap targets about 30 rendered fps at 2x or 20 at 3x. Zero leaves it uncapped.");
				ImGui::TextUnformatted("This is the cap to use with a variable refresh rate. It is imposed inside Reflex's own sleep, before input is sampled, and the driver knows about it -- unlike a limiter that spins after Present, which adds the time it waits to the latency of the frame it just submitted.");
				ImGui::TextUnformatted("Interpolation assumes evenly spaced frames, so a cap low enough to hold steady in the worst case is usually worth more than the headroom it gives up. Keep it below the display's maximum refresh so presentation stays inside the variable-refresh window.");
			}

			ImGui::Text("Display reports %.0f Hz", refreshRate);
			if (settings.reflexFrameLimit > 0) {
				if (IsFrameGenerationRequestedNow()) {
					const uint multiplier = IsDLSSGBackend() ?
						std::max(2u, streamline.GetDLSSGAppliedFramesToGenerate() + 1) : 2u;
					ImGui::Text("About %u presented, from %u rendered while generating",
						settings.reflexFrameLimit, settings.reflexFrameLimit / multiplier);
				}
				if (settings.reflexFrameLimit > refreshRate) {
					ImGui::PushStyleColor(ImGuiCol_Text, Util::Colors::GetWarning());
					ImGui::Text("Above the refresh rate: presentation leaves the variable-refresh window.");
					ImGui::PopStyleColor();
				}
			}

			sl::ReflexState reflexState{};
			if (streamline.GetReflexState(reflexState) && reflexState.latencyReportAvailable) {
				// The report is a 64-entry ring whose fill order is Streamline's business, so
				// pick the newest complete frame by frameID rather than assuming an end.
				const sl::ReflexReport* newest = nullptr;
				for (const auto& report : reflexState.frameReport) {
					if (report.gpuRenderEndTime > report.simStartTime && report.simStartTime != 0 &&
						(newest == nullptr || report.frameID > newest->frameID))
						newest = &report;
				}
				if (newest) {
					// Simulation start to GPU render end: the span Reflex itself acts on, and
					// the only one assembled entirely from markers this tree places.
					ImGui::Text("Reported latency %.1f ms",
						static_cast<double>(newest->gpuRenderEndTime - newest->simStartTime) / 1000.0);
					if (auto _tt = Util::HoverTooltipWrapper())
						ImGui::TextUnformatted("Simulation start to GPU render end for the newest completed frame, as the driver measured it. It excludes the display's own pipeline, so treat it as a number to compare against itself rather than as end-to-end click-to-photon.");
				}
			}
		}
		ImGui::TreePop();
	}

	if (ImGui::TreeNodeEx("Backend Diagnostics")) {
		// Streamline log level selection
		const char* logLevels[] = { "Off", "Default", "Verbose" };
		int logLevelIdx = static_cast<int>(settings.streamlineLogLevel);
		if (ImGui::Combo("Streamline Logging", &logLevelIdx, logLevels, IM_ARRAYSIZE(logLevels))) {
			settings.streamlineLogLevel = static_cast<uint>(logLevelIdx);
		}
		ImGui::TextUnformatted("Changing this requires a restart to take effect.");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Streamline logging controls the verbosity of NVIDIA Streamline backend logs. Useful for debugging issues with DLSS/DLSS-G.");
		}
		ImGui::Separator();
		// FidelityFX section
		if (ImGui::Selectable("AMD FidelityFX DLLs (click to open folder)")) {
			ShellExecuteW(nullptr, L"open", FidelityFX::PluginDir, nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::string> headers = { "DLL Name", "Version" };
		std::vector<std::vector<std::string>> ffRows;
		for (const auto& [name, dllVersion] : FidelityFX::dllVersions)
			ffRows.push_back({ name, dllVersion });
		std::vector<Util::TableSortFunc> ffSorters = { nullptr, Util::VersionSortComparator };
		Util::ShowSortedStringTableStrings(
			"ffx_dll_versions",
			headers,
			ffRows,
			0,
			true,
			ffSorters);

		// Streamline section
		if (ImGui::Selectable("NVIDIA Streamline DLLs (click to open folder)")) {
			ShellExecuteW(nullptr, L"open", Streamline::PluginDir, nullptr, nullptr, SW_SHOWNORMAL);
		}
		std::vector<std::vector<std::string>> slRows;
		for (const auto& [name, dllVersion] : Streamline::dllVersions)
			slRows.push_back({ name, dllVersion });
		std::vector<Util::TableSortFunc> slSorters = { nullptr, Util::VersionSortComparator };
		Util::ShowSortedStringTableStrings(
			"sl_dll_versions",
			headers,
			slRows,
			0,
			true,
			slSorters);
		ImGui::TreePop();
	}
	DrawBatch33ABLab(*this);
}

void Upscaling::TickBatch33AB()
{
	auto& state = GetBatch33ABState();
	if (state.pending < 0 || !globals::menu || globals::menu->IsEnabled ||
		!globals::game::ui || globals::game::ui->GameIsPaused()) {
		state.eligibleSince = 0;
		return;
	}
	const auto now = GetTickCount64();
	if (state.eligibleSince == 0) {
		// Begin the counter window only when gameplay is eligible. A pause or
		// settings-menu interruption restarts both the clock and these baselines.
		const auto nr = NeuralRendering::GetDiagnostics();
		state.nrAttemptsAtArm = nr.attempts;
		state.nrSuccessesAtArm = nr.successes;
		state.nrFailuresAtArm = nr.failures;
		const auto fg = dx12SwapChain.GetDLSSGCadenceTotals();
		state.fgSamplesAtArm = fg.samples;
		state.fgPresentedAtArm = fg.presented;
		state.eligibleSince = now;
		return;
	}
	if (now - state.eligibleSince < kBatch33SettleMs)
		return;
	const int arm = state.pending;
	json snapshot = CaptureBatch33Snapshot(*this, state, arm);
	logger::info("[B33AB] capture {}", snapshot.dump());
	if (arm == 0)
		state.a = std::move(snapshot);
	else
		state.b = std::move(snapshot);
	state.pending = -1;
	state.eligibleSince = 0;
	if (state.a.is_object() && state.b.is_object())
		state.verdict = CompareBatch33Snapshots(state.a, state.b);
}

void Upscaling::SaveSettings(json& o_json)
{
	o_json = settings;
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->WriteSetting(setting);
		}
	}
}

void Upscaling::LoadSettings(json& o_json)
{
	settings = o_json;

	// Sanitize loaded settings to ensure enum indices are valid
	constexpr auto enumCount = 4;  // UpscaleMethod has 4 values: kNONE, kTAA, kFSR, kDLSS
	if (settings.upscaleMethod >= static_cast<uint>(enumCount)) {
		logger::warn("[Upscaling] Loaded upscaleMethod {} out of range, clamping to {}", settings.upscaleMethod, enumCount ? enumCount - 1 : 0);
		settings.upscaleMethod = enumCount ? enumCount - 1 : 0;
	}
	if (settings.upscaleMethodNoDLSS >= static_cast<uint>(enumCount)) {
		logger::warn("[Upscaling] Loaded upscaleMethodNoDLSS {} out of range, clamping to {}", settings.upscaleMethodNoDLSS, enumCount ? enumCount - 1 : 0);
		settings.upscaleMethodNoDLSS = enumCount ? enumCount - 1 : 0;
	}
	if (settings.frameGenerationMode > 1) {
		logger::warn("[Upscaling] Loaded frameGenerationMode {} out of range, clamping to enabled", settings.frameGenerationMode);
		settings.frameGenerationMode = 1;
	}
	constexpr auto frameGenerationBackendCount = static_cast<uint>(FrameGenerationBackend::kCount);
	if (settings.frameGenerationBackend >= frameGenerationBackendCount) {
		logger::warn("[Upscaling] Loaded frameGenerationBackend {} out of range, falling back to FSR 3.1", settings.frameGenerationBackend);
		settings.frameGenerationBackend = static_cast<uint>(FrameGenerationBackend::kFSR3FG);
	}
	if (settings.dlssGFramesToGenerate < 1 || settings.dlssGFramesToGenerate > 3) {
		logger::warn("[Upscaling] Loaded dlssGFramesToGenerate {} out of range, clamping to 1..3", settings.dlssGFramesToGenerate);
		settings.dlssGFramesToGenerate = std::clamp(settings.dlssGFramesToGenerate, 1u, 3u);
	}
	constexpr auto dlssPresetCount = static_cast<uint>(DLSSModelPreset::kCount);
	if (settings.DLSSPreset >= dlssPresetCount) {
		const auto fallback = static_cast<uint>(DLSSModelPreset::kK);
		logger::warn("[Upscaling] Loaded DLSSPreset {} out of range, falling back to K ({})", settings.DLSSPreset, fallback);
		settings.DLSSPreset = fallback;
	}
	// D and F are the only selectable Ray Reconstruction models. The enum keeps its original
	// values so a saved profile is never silently remapped onto a different model; anything
	// that is no longer offered is coerced to D, which is what the RR path ran hardcoded
	// before the setting existed.
	if (settings.DLSSDPreset != static_cast<uint>(DLSSDModelPreset::kD) &&
		settings.DLSSDPreset != static_cast<uint>(DLSSDModelPreset::kF)) {
		const auto fallback = static_cast<uint>(DLSSDModelPreset::kD);
		logger::warn("[Upscaling] Loaded DLSSDPreset {} is not a selectable model, falling back to D ({})", settings.DLSSDPreset, fallback);
		settings.DLSSDPreset = fallback;
	}
	auto iniSettingCollection = globals::game::iniPrefSettingCollection;
	if (iniSettingCollection) {
		auto setting = iniSettingCollection->GetSetting("bUseTAA:Display");
		if (setting) {
			iniSettingCollection->ReadSetting(setting);
		}
	}
}

void Upscaling::RestoreDefaultSettings()
{
	settings = {};
}

void Upscaling::DataLoaded()
{
	// Fix screenshots fix from Engine Fixes
	RE::GetINISetting("bUseTAA:Display")->data.b = false;
}

void Upscaling::Load()
{
	*(uintptr_t*)&ptrD3D11CreateDeviceAndSwapChainUpscaling = SKSE::PatchIAT(hk_D3D11CreateDeviceAndSwapChainUpscaling, "d3d11.dll", "D3D11CreateDeviceAndSwapChain");
}

struct BSImageSpace_Init_FXAA
{
	static void thunk()
	{
		func();

		// Force FXAA off safely
		auto fxaaEnabled = reinterpret_cast<bool*>(REL::RelocationID(513281, 391028).address());
		*fxaaEnabled = false;
	}
	static inline REL::Relocation<decltype(thunk)> func;
};
void Upscaling::PostPostLoad()
{
	bool isGOG = !GetModuleHandle(L"steam_api64.dll");
	stl::detour_thunk<MenuManagerDrawInterfaceStartHook>(REL::RelocationID(79947, 82084));

	// Calculates resolution and jitter
	stl::write_thunk_call<Main_UpdateJitter>(REL::RelocationID(75460, 77245).address() + REL::Relocate(0xE5, isGOG ? 0x133 : 0xE2, 0x104));

	// Disables the original dynamic resolution system
	REL::safe_write(REL::RelocationID(35556, 36555).address() + REL::Relocate(0x2D, 0x2D, 0x25), REL::NOP5, sizeof(REL::NOP5));

	// Performs upscaling in between volumetric lighting and post processing
	stl::write_thunk_call<Main_PostProcessing>(REL::RelocationID(100430, 107148).address() + REL::Relocate(0x1F0, 0x1E7, 0x206));

	// Patches RSSetScissorRect calls to use dynamic resolution
	// This is a PC-specific function hence it was missing
	if (!globals::game::isVR)
		stl::detour_thunk<SetScissorRect>(REL::RelocationID(75564, 77365));

	// Patches facegen texture generation to not use dynamic resolution
	stl::detour_thunk<BSFaceGenManager_UpdatePendingCustomizationTextures>(REL::RelocationID(26455, 27041));

	// Patches precipitation camera to not use dynamic resolution
	stl::write_thunk_call<Main_RenderPrecipitation>(REL::RelocationID(35560, 36559).address() + REL::Relocate(0x3A1, 0x3A1, 0x2FA));

	// Forces FXAA off
	stl::detour_thunk<BSImageSpace_Init_FXAA>(REL::RelocationID(98974, 105626));

	logger::info("[Upscaling] Installed hooks");
}

Upscaling::UpscaleMethod Upscaling::GetUpscaleMethod()
{
	if (streamline.featureDLSS)
		return (UpscaleMethod)settings.upscaleMethod;
	return (UpscaleMethod)settings.upscaleMethodNoDLSS;
}

void Upscaling::CreateUpscalingTextureResources(UpscaleMethod a_upscalemethod, bool a_enableDLSSRR)
{
	logger::debug("[Upscaling] Creating texture resources for method {} ({}), DLSS-RR enabled: {}", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod), a_enableDLSSRR);

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	if (a_upscalemethod == UpscaleMethod::kDLSS || a_upscalemethod == UpscaleMethod::kFSR) {
		texDesc.Format = DXGI_FORMAT_R8_UNORM;
		srvDesc.Format = texDesc.Format;
		uavDesc.Format = texDesc.Format;

		if (!reactiveMaskTexture) {
			reactiveMaskTexture = std::make_unique<Texture2D>(texDesc);
			reactiveMaskTexture->CreateSRV(srvDesc);
			reactiveMaskTexture->CreateUAV(uavDesc);
		}

		if (!transparencyCompositionMaskTexture) {
			transparencyCompositionMaskTexture = std::make_unique<Texture2D>(texDesc);
			transparencyCompositionMaskTexture->CreateSRV(srvDesc);
			transparencyCompositionMaskTexture->CreateUAV(uavDesc);
		}
	}

	// Motion vector copy texture is only needed for DLSS
	if (a_upscalemethod == UpscaleMethod::kDLSS) {
		if (!motionVectorCopyTexture) {
			auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

			D3D11_TEXTURE2D_DESC motionTexDesc{};
			motionVector.texture->GetDesc(&motionTexDesc);

			texDesc.Format = motionTexDesc.Format;
			srvDesc.Format = texDesc.Format;
			uavDesc.Format = texDesc.Format;

			motionVectorCopyTexture = std::make_unique<Texture2D>(motionTexDesc);
			motionVectorCopyTexture->CreateSRV(srvDesc);
			motionVectorCopyTexture->CreateUAV(uavDesc);
		}
	}
}

void Upscaling::DestroyUpscalingTextureResources(UpscaleMethod a_upscalemethod)
{
	logger::debug("[Upscaling] Destroying texture resources for method {} ({})", static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod));

	// Clean up D3D11 textures that are no longer needed
	// Only destroy textures when switching away from methods that use them
	// (batch 16, item 3) These three were the one place in the repo that already released
	// correctly, but by hand: null the three views, then `delete`. As unique_ptrs a single
	// reset() does the same thing, and the members can no longer be leaked by a future edit
	// that forgets the delete.
	if (a_upscalemethod != UpscaleMethod::kDLSS && a_upscalemethod != UpscaleMethod::kFSR) {
		reactiveMaskTexture.reset();
		transparencyCompositionMaskTexture.reset();
	}

	// Motion vector copy texture is only needed for DLSS - destroy when switching away from DLSS
	if (a_upscalemethod != UpscaleMethod::kDLSS) {
		motionVectorCopyTexture.reset();
	}
}

void Upscaling::CheckResources(UpscaleMethod a_upscalemethod)
{
	static auto previousUpscaleMode = UpscaleMethod::kTAA;
	static bool previousFrameGenMode = false;
	static bool previousRR = false;

	bool frameGenModeCurrent = (IsFrameGenerationEnabled() && d3d12SwapChainActive);
	bool frameGenModeChanged = frameGenModeCurrent != previousFrameGenMode;
	bool upscaleModeChanged = (previousUpscaleMode != a_upscalemethod);
	bool rrChanged = (settings.enableDLSSRR != previousRR);

	if (upscaleModeChanged || frameGenModeChanged || rrChanged) {
		logger::debug("[Upscaling] Resource change detected - Upscale: {} ({}) -> {} ({}), FrameGen: {} -> {} (d3d12Active={}), DLSS RR: {} -> {}",
			static_cast<int>(previousUpscaleMode), magic_enum::enum_name(previousUpscaleMode), static_cast<int>(a_upscalemethod), magic_enum::enum_name(a_upscalemethod), previousFrameGenMode, frameGenModeCurrent, d3d12SwapChainActive, previousRR, settings.enableDLSSRR);

		// Destroy previous upscaling method resources (only if they were actually active)
		if (upscaleModeChanged) {
			DestroyUpscalingTextureResources(a_upscalemethod);

			// Only destroy SDK resources if the previous method was actually performing upscaling
			if (previousUpscalingWasActive) {
				if (previousUpscaleMode == UpscaleMethod::kDLSS)
					if (previousRR)
					streamline.DestroyDLSSRRResources(true);
				else
					streamline.DestroyDLSSResources(true);
				else if (previousUpscaleMode == UpscaleMethod::kFSR)
					fidelityFX.DestroyFSRResources();
			}
			if (a_upscalemethod == UpscaleMethod::kFSR)
				fidelityFX.CreateFSRResources();
		}

		if (a_upscalemethod == UpscaleMethod::kDLSS) {
			if (!upscaleModeChanged && rrChanged) {
				if (previousRR)
					streamline.DestroyDLSSRRResources();
				else
					streamline.DestroyDLSSResources();
			}
		}

		// Create new upscaling method resources
		if (upscaleModeChanged || rrChanged) {
			CreateUpscalingTextureResources(a_upscalemethod, settings.enableDLSSRR);
		}

		// Update tracking for next call
		previousUpscaleMode = a_upscalemethod;
		previousFrameGenMode = (IsFrameGenerationEnabled() && d3d12SwapChainActive);
		previousRR = settings.enableDLSSRR;
		previousUpscalingWasActive = IsUpscalingActive();
	}
}

ID3D11ComputeShader* Upscaling::GetEncodeTexturesCS()
{
	auto upscaleMethod = GetUpscaleMethod();
	uint methodIndex = (uint)upscaleMethod;

	if (!encodeTexturesCS[methodIndex]) {
		logger::debug("Compiling EncodeTexturesCS.hlsl for upscale method {}", methodIndex);

		std::vector<std::pair<const char*, const char*>> defines;

		// Add upscale method define
		switch (upscaleMethod) {
		case UpscaleMethod::kDLSS:
			defines.push_back({ "DLSS", "" });
			break;
		case UpscaleMethod::kFSR:
			defines.push_back({ "FSR", "" });
			break;
		default:
			// No define for NONE or TAA
			break;
		}

		encodeTexturesCS[methodIndex].attach((ID3D11ComputeShader*)Util::CompileShader(L"Data/Shaders/Upscaling/EncodeTexturesCS.hlsl", defines, "cs_5_0"));
	}
	return encodeTexturesCS[methodIndex].get();
}

ID3D11PixelShader* Upscaling::GetDepthRefractionUpscalePS()
{
	if (!depthRefractionUpscalePS) {
		logger::debug("Compiling DepthRefractionUpscalePS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		depthRefractionUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/DepthRefractionUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return depthRefractionUpscalePS.get();
}

ID3D11PixelShader* Upscaling::GetUnderwaterMaskUpscalePS()
{
	if (!underwaterMaskUpscalePS) {
		logger::debug("Compiling UnderwaterMaskPS.hlsl");
		std::vector<std::pair<const char*, const char*>> defines = { { "PSHADER", "" } };
		underwaterMaskUpscalePS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UnderwaterMaskUpscalePS.hlsl", defines, "ps_5_0"));
	}

	return underwaterMaskUpscalePS.get();
}

ID3D11VertexShader* Upscaling::GetUpscaleVS()
{
	if (!upscaleVS) {
		logger::debug("Compiling UpscaleVS.hlsl");
		upscaleVS.attach((ID3D11VertexShader*)Util::CompileShader(L"Data/Shaders/Upscaling/UpscaleVS.hlsl", { { "VSHADER", "" } }, "vs_5_0"));
	}

	return upscaleVS.get();
}

int32_t GetJitterPhaseCount(int32_t renderWidth, int32_t displayWidth)
{
	const float basePhaseCount = 8.0f;
	const int32_t jitterPhaseCount = int32_t(basePhaseCount * pow((float(displayWidth) / renderWidth), 2.0f));
	return jitterPhaseCount;
}

// Calculate halton number for index and base.
static float Halton(int32_t index, int32_t base)
{
	float f = 1.0f, result = 0.0f;

	for (int32_t currentIndex = index; currentIndex > 0;) {
		f /= (float)base;
		result = result + f * (float)(currentIndex % base);
		currentIndex = (uint32_t)(floorf((float)(currentIndex) / (float)(base)));
	}

	return result;
}

void GetJitterOffset(float* outX, float* outY, int32_t index, int32_t phaseCount)
{
	const float x = Halton((index % phaseCount) + 1, 2) - 0.5f;
	const float y = Halton((index % phaseCount) + 1, 3) - 0.5f;

	*outX = x;
	*outY = y;
}

void Upscaling::ConfigureTAA()
{
	auto upscaleMethod = GetUpscaleMethod();

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);

	// Disable water TAA when upscaling is enabled
	bool* enableWaterTAA = reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(BSImagespaceShaderISTemporalAA) + 0x38LL);
	*enableWaterTAA = upscaleMethod == UpscaleMethod::kNONE || upscaleMethod == UpscaleMethod::kTAA;

	// Force enable TAA if needed
	BSImagespaceShaderISTemporalAA->taaEnabled = upscaleMethod != UpscaleMethod::kNONE;
}

void Upscaling::ConfigureUpscaling(RE::BSGraphics::State* a_viewport)
{
	auto upscaleMethod = GetUpscaleMethod();
	const bool mapRenderingContext = IsDLSSGMapRenderingContext();
	if (mapRenderingContext != dlssGMapRenderingContextActive) {
		if (mapRenderingContext) {
			// The map uses a different camera domain. Reset SR on its first frame,
			// while the Present state machine keeps FG off for that token only.
			streamline.RequestTemporalReset();
			logger::info("[Upscaling] MapMenu opened; resetting DLSS SR while frame generation remains off for one token");
		} else {
			// ConfigureUpscaling runs before the deferred constants and DLSS SR
			// dispatch for this world frame, so the reset reaches SR before it can
			// consume the preceding map-camera history.
			streamline.RequestTemporalReset();
			logger::info("[Upscaling] MapMenu closed; resetting DLSS SR history for the first world frame");
		}
		dlssGMapRenderingContextActive = mapRenderingContext;
	}

	// Delete or create resources as necessary
	CheckResources(upscaleMethod);

	// The game defaults this to a non-zero value
	auto fDRClampOffset = RE::GetINISetting("fDRClampOffset:Display");
	fDRClampOffset->data.f = 0.0f;

	// Cache original TAA values for UI
	projectionPosScaleX = a_viewport->projectionPosScaleX;
	projectionPosScaleY = a_viewport->projectionPosScaleY;

	// Get full screen size
	auto state = globals::state;
	auto screenSize = state->screenSize;

	auto screenWidth = static_cast<int>(screenSize.x);
	auto screenHeight = static_cast<int>(screenSize.y);

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA) {
		float2 resolutionScaleBase = { 1.0f, 1.0f };

		if (upscaleMethod == UpscaleMethod::kDLSS && !settings.enableDLSSRR) {
			resolutionScaleBase = streamline.GetInputResolutionScale((uint32_t)screenSize.x, (uint32_t)screenSize.y, settings.qualityMode);
		} else if (upscaleMethod == UpscaleMethod::kDLSS && settings.enableDLSSRR) {
			resolutionScaleBase = streamline.GetInputResolutionScaleRR((uint32_t)screenSize.x, (uint32_t)screenSize.y, settings.qualityMode);
		} else if (upscaleMethod == UpscaleMethod::kFSR) {
			resolutionScaleBase = fidelityFX.GetInputResolutionScale((uint32_t)screenSize.x, (uint32_t)screenSize.y, settings.qualityMode);
		}

		auto renderWidth = static_cast<int>(screenWidth * resolutionScaleBase.x);
		auto renderHeight = static_cast<int>(screenHeight * resolutionScaleBase.y);

		// Use precise scale if the integer conversion doesn't change the dimensions
		if (renderWidth == screenWidth && renderHeight == screenHeight) {
			// For DLAA and other 1:1 modes, ensure exactly 1.0
			resolutionScale.x = 1.0f;
			resolutionScale.y = 1.0f;
		} else {
			resolutionScale.x = static_cast<float>(renderWidth) / static_cast<float>(screenWidth);
			resolutionScale.y = static_cast<float>(renderHeight) / static_cast<float>(screenHeight);
		}

		auto phaseCount = GetJitterPhaseCount(renderWidth, screenWidth);

		GetJitterOffset(&jitter.x, &jitter.y, state->frameCount, phaseCount);

		if (globals::game::isVR)
			a_viewport->projectionPosScaleX = -jitter.x / renderWidth;
		else
			a_viewport->projectionPosScaleX = -2.0f * jitter.x / renderWidth;

		a_viewport->projectionPosScaleY = 2.0f * jitter.y / renderHeight;
	} else {
		resolutionScale = { 1.0f, 1.0f };

		if (globals::game::isVR)
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth;
		else
			jitter.x = -a_viewport->projectionPosScaleX * screenWidth / 2.0f;

		jitter.y = a_viewport->projectionPosScaleY * screenHeight / 2.0f;
	}

	auto& runtimeData = a_viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = dynamicResolutionWidthRatio;
	runtimeData.dynamicResolutionPreviousHeightRatio = dynamicResolutionHeightRatio;
	runtimeData.dynamicResolutionWidthRatio = resolutionScale.x;
	runtimeData.dynamicResolutionHeightRatio = resolutionScale.y;

	dynamicResolutionWidthRatio = resolutionScale.x;
	dynamicResolutionHeightRatio = resolutionScale.y;

	// Disable dynamic resolution unless the game explicitly enables it
	if (!globals::game::isVR)
		runtimeData.dynamicResolutionLock = 1;

	// If running in VR and an external upscaler is active, force-disable
	// the engine's depth-buffer culling immediately. This ensures that
	// enabling upscaling at runtime (after game load) does not leave the
	// VR depth-buffer culling enabled which can cause incorrect occlusion.
	if (globals::game::isVR) {
		auto& vr = globals::features::vr;
		if (IsUpscalingActive()) {
			if (vr.gDepthBufferCulling) {
				if (*vr.gDepthBufferCulling) {
					*vr.gDepthBufferCulling = false;
					logger::info("[Upscaling] VR detected - forcing depth buffer culling OFF due to active downscaling upscaler (scale={})", resolutionScale.x);
				}
			} else {
				logger::warn("[Upscaling] VR depth buffer culling pointer is null, cannot force disable");
			}
		}
	}
}

void Upscaling::SetupResources()
{
	QueryPerformanceFrequency(&qpf);

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	D3D11_TEXTURE2D_DESC texDesc{};
	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
	D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};

	main.texture->GetDesc(&texDesc);
	main.SRV->GetDesc(&srvDesc);
	main.UAV->GetDesc(&uavDesc);

	texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.Format = texDesc.Format;
	uavDesc.Format = texDesc.Format;

	D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
	depthStencilDesc.DepthEnable = true;                           // Enable depth testing
	depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;  // Write to all depth bits
	depthStencilDesc.DepthFunc = D3D11_COMPARISON_ALWAYS;          // Always pass depth test (write all depths)

	if (globals::game::isVR) {
		depthStencilDesc.StencilEnable = true;     // Enable stencil testing
		depthStencilDesc.StencilReadMask = 0xFF;   // Read all stencil bits
		depthStencilDesc.StencilWriteMask = 0xFF;  // Write to all stencil bits

		// Configure front-facing stencil operations
		depthStencilDesc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;       // Replace on stencil fail
		depthStencilDesc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;  // Replace on depth fail
		depthStencilDesc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_REPLACE;    // Replace on pass
		depthStencilDesc.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;       // Always pass stencil test

		// Configure back-facing stencil operations (same as front)
		depthStencilDesc.BackFace.StencilFailOp = depthStencilDesc.FrontFace.StencilFailOp;
		depthStencilDesc.BackFace.StencilDepthFailOp = depthStencilDesc.FrontFace.StencilDepthFailOp;
		depthStencilDesc.BackFace.StencilPassOp = depthStencilDesc.FrontFace.StencilPassOp;
		depthStencilDesc.BackFace.StencilFunc = depthStencilDesc.FrontFace.StencilFunc;
	} else {
		depthStencilDesc.StencilEnable = false;  // Disable stencil testing
	}

	DX::ThrowIfFailed(globals::d3d::device->CreateDepthStencilState(&depthStencilDesc, upscaleDepthStencilState.put()));

	// Create jitter offset constant buffer for depth upscaling
	jitterCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<JitterCB>());

	// Create upscaling data constant buffer for encode textures compute shader
	upscalingDataCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<UpscalingDataCB>());

	// Create blend state for depth upscaling
	D3D11_BLEND_DESC blendDesc = {};
	blendDesc.AlphaToCoverageEnable = false;
	blendDesc.IndependentBlendEnable = false;
	blendDesc.RenderTarget[0].BlendEnable = false;
	blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	DX::ThrowIfFailed(globals::d3d::device->CreateBlendState(&blendDesc, upscaleBlendState.put()));

	// Create rasterizer state for fullscreen rendering
	D3D11_RASTERIZER_DESC rasterizerDesc = {};
	rasterizerDesc.FillMode = D3D11_FILL_SOLID;
	rasterizerDesc.CullMode = D3D11_CULL_NONE;
	rasterizerDesc.FrontCounterClockwise = false;
	rasterizerDesc.DepthBias = 0;
	rasterizerDesc.DepthBiasClamp = 0.0f;
	rasterizerDesc.SlopeScaledDepthBias = 0.0f;
	rasterizerDesc.DepthClipEnable = false;
	rasterizerDesc.ScissorEnable = false;
	rasterizerDesc.MultisampleEnable = false;
	rasterizerDesc.AntialiasedLineEnable = false;
	DX::ThrowIfFailed(globals::d3d::device->CreateRasterizerState(&rasterizerDesc, upscaleRasterizerState.put()));

	CheckResources(GetUpscaleMethod());

	if (d3d12SwapChainActive)
		dx12SwapChain.CreateSharedResources();

	copyDepthToSharedBufferPS.attach((ID3D11PixelShader*)Util::CompileShader(L"Data\\Shaders\\Upscaling\\CopyDepthToSharedBufferPS.hlsl", { { "PSHADER", "" } }, "ps_5_0"));
}

void Upscaling::ClearShaderCache()
{
	for (int i = 0; i < 5; ++i) {
		encodeTexturesCS[i] = nullptr;  // com_ptr automatically releases
	}

	depthRefractionUpscalePS = nullptr;  // com_ptr automatically releases
	underwaterMaskUpscalePS = nullptr;   // com_ptr automatically releases
	upscaleVS = nullptr;                 // com_ptr automatically releases
}

void Upscaling::CopySharedD3D12Resources()
{
	globals::state->BeginPerfEvent("Copy Shared D3D12 Resources");

	auto renderer = globals::game::renderer;
	auto context = globals::d3d::context;

	if (IsDLSSGBackend()) {
		// Capture this while the rendering scale for the current frame is still
		// active. PostDisplay restores the game's viewport ratios before Present,
		// so deriving the extent there would incorrectly produce output size.
		const auto screenSize = globals::state->screenSize;
		const auto depthDesc = dx12SwapChain.depthBufferShared12->resource->GetDesc();
		const auto motionVectorDesc = dx12SwapChain.motionVectorBufferShared12->resource->GetDesc();
		const auto maxInputWidth = static_cast<uint32_t>(std::min(depthDesc.Width, motionVectorDesc.Width));
		const auto maxInputHeight = std::min(depthDesc.Height, motionVectorDesc.Height);
		const auto inputWidth = std::clamp(
			static_cast<uint32_t>(std::lround(screenSize.x * resolutionScale.x)),
			1u,
			maxInputWidth);
		const auto inputHeight = std::clamp(
			static_cast<uint32_t>(std::lround(screenSize.y * resolutionScale.y)),
			1u,
			maxInputHeight);
		dx12SwapChain.SetDLSSGInputExtent(inputWidth, inputHeight, streamline.GetLatchedFrameTokenIndex());
	}

	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	context->CopyResource(dx12SwapChain.motionVectorBufferShared12->resource11, motionVector.texture);
	if (dx12SwapChain.motionVectorFrameGenerationShared12) {
		// Preserve the engine's original MV field for DLSS-G before the DLSS SR
		// encode pass overwrites its own interop buffer with depth-aware dilation.
		context->CopyResource(dx12SwapChain.motionVectorFrameGenerationShared12->resource11, motionVector.texture);
	}
	auto& albedo = renderer->GetRuntimeData().renderTargets[ALBEDO];
	context->CopyResource(dx12SwapChain.albedoShared12->resource11, albedo.texture);
	auto& reflectance = renderer->GetRuntimeData().renderTargets[REFLECTANCE];
	context->CopyResource(dx12SwapChain.reflectanceShared12->resource11, reflectance.texture);

	if (settings.enableDLSSRR) {
		auto& ssrt = globals::features::screenSpaceRayTracing;
		if (ssrt.loaded && ssrt.settings.EnableSpecular) {
			if (ssrt.texHitDistance) {
				context->CopyResource(dx12SwapChain.specHitDistanceShared12->resource11, ssrt.texHitDistance->resource.get());
			}
		}

		auto& sss = globals::features::subsurfaceScattering;
		if (sss.loaded) {
			if (sss.sssGuide) {
				context->CopyResource(dx12SwapChain.sssGuide->resource11, sss.sssGuide->resource.get());
			}
		}
	}

	auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

	{
		// Set up viewport for fullscreen rendering
		auto screenSize = globals::state->screenSize;

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set up Input Assembler for fullscreen triangle
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader
		context->VSSetShader(GetUpscaleVS(), nullptr, 0);

		// Set up rasterizer and blend states
		context->RSSetState(upscaleRasterizerState.get());
		context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

		// Set up pixel shader resources
		ID3D11ShaderResourceView* views[1] = { depth.depthSRV };
		context->PSSetShaderResources(0, ARRAYSIZE(views), views);

		// Set render target view for pixel shader output
		ID3D11RenderTargetView* rtvs[1] = { dx12SwapChain.depthBufferShared12->rtv };
		context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

		context->PSSetShader(copyDepthToSharedBufferPS.get(), nullptr, 0);

		context->Draw(3, 0);
	}

	// Clean up
	ID3D11ShaderResourceView* views[1] = { nullptr };
	context->PSSetShaderResources(0, ARRAYSIZE(views), views);

	context->OMSetRenderTargets(0, nullptr, nullptr);
	context->PSSetShader(nullptr, nullptr, 0);
	context->VSSetShader(nullptr, nullptr, 0);

	if (IsDLSSGBackend())
		dx12SwapChain.MarkDLSSGSceneResourcesReady(streamline.GetLatchedFrameTokenIndex());

	globals::state->EndPerfEvent();
}

void UpdateCameraData()
{
	using func_t = decltype(&UpdateCameraData);
	static REL::Relocation<func_t> func{ RELOCATION_ID(75472, 77258) };
	func();
}

void Upscaling::PostDisplay()
{
	auto viewport = globals::game::graphicsState;

	viewport->projectionPosScaleX = projectionPosScaleX;
	viewport->projectionPosScaleY = projectionPosScaleY;

	auto& runtimeData = viewport->GetRuntimeData();

	runtimeData.dynamicResolutionPreviousWidthRatio = 1;
	runtimeData.dynamicResolutionPreviousHeightRatio = 1;
	runtimeData.dynamicResolutionWidthRatio = 1;
	runtimeData.dynamicResolutionHeightRatio = 1;
	runtimeData.dynamicResolutionLock = 1;

	globals::game::renderer->UpdateViewPort(0, 0, 1);
	UpdateCameraData();

	if (d3d12SwapChainActive)
		SetUIBuffer();

	globals::state->UpdateSharedData(false, false);
}

bool Upscaling::ApplyReflexSettings(bool a_force)
{
	if (!streamline.featureReflex)
		return false;

	uint mode = std::min(settings.reflexMode, static_cast<uint>(sl::ReflexMode::eLowLatencyWithBoost));

	// NVIDIA requires Reflex to be at least on whenever DLSS-G is generating, and effectively
	// no shipping game exposes the combination of frame generation on with Reflex off. The
	// reason is structural rather than a recommendation: DLSS-G paces its own presentation, and
	// without Reflex holding the render queue the game runs ahead of a display cadence it no
	// longer controls, which is the case where generation adds the most latency. Held here
	// rather than by rewriting the setting, so the user's own choice survives switching
	// generation back off.
	if (IsFrameGenerationRequestedNow() && mode == static_cast<uint>(sl::ReflexMode::eOff))
		mode = static_cast<uint>(sl::ReflexMode::eLowLatency);

	const uint frameLimit = settings.reflexFrameLimit;
	if (!a_force && mode == reflexModeApplied && frameLimit == reflexFrameLimitApplied)
		return true;

	// Reflex takes a frame *period*, not a rate. Zero stays zero, which is how it is told the
	// limiter is off; anything else rounds down to whole microseconds, so the cap is at or
	// fractionally below the requested rate rather than above it.
	const uint32_t frameLimitUs = frameLimit > 0 ? static_cast<uint32_t>(1000000u / frameLimit) : 0u;

	if (!streamline.SetReflexOptions(static_cast<sl::ReflexMode>(mode), frameLimitUs))
		return false;

	// Only logged when the mode moves. Dragging the frame-limit slider applies on every step,
	// and logging each one buried the rest of the session in a hundred near-identical lines.
	// The cap is carried on the mode line and shown live in the panel, which is where it is
	// actually read; the per-step value goes to debug.
	const bool modeChanged = mode != reflexModeApplied;

	// Only cached on success, so a rejected call is retried on the next frame instead of being
	// silently remembered as applied.
	reflexModeApplied = mode;
	reflexFrameLimitApplied = frameLimit;

	const auto limitText = frameLimit > 0 ? std::format("{} fps ({} us)", frameLimit, frameLimitUs) : std::string("off");
	if (modeChanged)
		logger::info("[Streamline] Reflex mode {}, frame limit {}",
			magic_enum::enum_name(static_cast<sl::ReflexMode>(mode)), limitText);
	else
		logger::debug("[Streamline] Reflex frame limit {}", limitText);
	return true;
}

void Upscaling::TimerSleepQPC(int64_t targetQPC)
{
	LARGE_INTEGER currentQPC;
	do {
		QueryPerformanceCounter(&currentQPC);
	} while (currentQPC.QuadPart < targetQPC);
}

void Upscaling::FrameLimiter()
{
	if (d3d12SwapChainActive) {
		// Use frame latency waitable object if available for better frame pacing
		HANDLE waitableObject = GetFrameLatencyWaitableObject();

		// Wait for the next frame presentation slot
		WaitForSingleObject(waitableObject, INFINITE);

		if (settings.frameLimitMode) {
			// Fall back to the original timing method
			// Use integer arithmetic for more precise timing
			// SetOptions ran earlier in this Present. Pace only when generation really
			// stayed active; a failed or suspended DLSS-G request must not cap raw FPS.
			const bool generatingNow = IsFrameGenerationActive();
			const uint multiplier = generatingNow && IsDLSSGBackend() ?
				std::max(2u, streamline.GetDLSSGAppliedFramesToGenerate() + 1) : 2u;
			int64_t targetFrameTimeNS = int64_t(1000000000.0 /
				(refreshRate * (generatingNow ? 1.0 / multiplier : 1.0)));
			int64_t targetFrameTicks = (targetFrameTimeNS * qpf.QuadPart) / 1000000000LL;

			static LARGE_INTEGER lastFrame = {};
			LARGE_INTEGER timeNow;
			QueryPerformanceCounter(&timeNow);

			int64_t delta = timeNow.QuadPart - lastFrame.QuadPart;
			if (delta < targetFrameTicks) {
				TimerSleepQPC(lastFrame.QuadPart + targetFrameTicks);
			}
			QueryPerformanceCounter(&lastFrame);
		}
	}
}

/*
* Copyright (c) 2022-2023 NVIDIA CORPORATION. All rights reserved
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in all
* copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
* SOFTWARE.
*/

double Upscaling::GetRefreshRate(HWND a_window)
{
	HMONITOR monitor = MonitorFromWindow(a_window, MONITOR_DEFAULTTONEAREST);
	MONITORINFOEXW info;
	info.cbSize = sizeof(info);
	if (GetMonitorInfoW(monitor, &info) != 0) {
		// using the CCD get the associated path and display configuration
		UINT32 requiredPaths, requiredModes;
		if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) == ERROR_SUCCESS) {
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(requiredPaths);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes2(requiredModes);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, paths.data(), &requiredModes, modes2.data(), nullptr) == ERROR_SUCCESS) {
				// iterate through all the paths until find the exact source to match
				for (auto& p : paths) {
					DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;
					sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
					sourceName.header.size = sizeof(sourceName);
					sourceName.header.adapterId = p.sourceInfo.adapterId;
					sourceName.header.id = p.sourceInfo.id;
					if (DisplayConfigGetDeviceInfo(&sourceName.header) == ERROR_SUCCESS) {
						// find the matched device which is associated with current device
						// there may be the possibility that display may be duplicated and windows may be one of them in such scenario
						// there may be two callback because source is same target will be different
						// as window is on both the display so either selecting either one is ok
						// get the refresh rate
						UINT numerator = p.targetInfo.refreshRate.Numerator;
						UINT denominator = p.targetInfo.refreshRate.Denominator;
						return (double)numerator / (double)denominator;
					}
				}
			}
		}
	}
	logger::error("Failed to retrieve refresh rate from swap chain");
	return 60;
}

bool Upscaling::IsFrameGenerationActive() const
{
	const bool backendActive = IsDLSSGBackend() ? streamline.IsDLSSGActive() : fidelityFX.isFrameGenActive;
	return d3d12SwapChainActive && IsFrameGenerationRequestedNow() && backendActive && !globals::game::isVR;
}

bool Upscaling::IsUpscalingActive()
{
	auto method = GetUpscaleMethod();

	// Only consider vendor upscalers (FSR/DLSS) as "active" when the
	// selected method actually produces a downscale. If the renderer is
	// currently running at 1:1 (no downscale) then depth-buffer culling and
	// other VR-sensitive behavior can remain enabled.
	if (!(method == UpscaleMethod::kFSR || method == UpscaleMethod::kDLSS)) {
		return false;
	}

	// resolutionScale.x represents renderWidth / displayWidth.
	return resolutionScale.x < .99f;
}

/**
 * @brief Retrieves the current frame time for frame generation.
 *
 * Returns the frame time from the D3D12 swap chain if frame generation is active; otherwise, returns 0.
 *
 * @return float The current frame time in seconds, or 0 if frame generation is inactive.
 */
float Upscaling::GetFrameGenerationFrameTime() const
{
	if (!IsFrameGenerationActive())
		return 0.0f;

	// Get the current frame time from D3D12 swapchain
	if (dx12SwapChain.swapChain) {
		// Get frame time from the D3D12 SwapChain
		return GetFrameTime();
	}

	return 0.0f;
}

/**
 * @brief Measured DLSS-G presentation cadence for the Performance Overlay.
 *
 * @return presented frames per rendered frame (>1 while multi-frame generation runs),
 *         or 0 when frame generation is inactive or the backend reports no cadence
 *         (FSR 3 frame generation never does).
 */
float Upscaling::GetFrameGenerationPresentMultiplier() const
{
	if (!IsFrameGenerationActive() || !IsDLSSGBackend())
		return 0.0f;
	return dx12SwapChain.GetMeasuredPresentMultiplier();
}

// Unified interface methods
void Upscaling::LoadUpscalingSDKs()
{
	// Initialize upscaling SDK components during plugin startup
	// This ensures all SDKs are available before any D3D device creation
	LatchFrameGenerationBackend();
	streamline.SelectDLSSGBackendAtBoot(IsDLSSGBackend());
	streamline.LoadInterposer();
	if (!IsDLSSGBackend())
		fidelityFX.LoadFFX();  // Only the cold-selected FSR3 frame-generation path owns these DLLs.
	else
		logger::info("[Upscaling] Skipping FidelityFX frame-generation runtime because DLSS-G is selected for this session");
}

void Upscaling::LatchFrameGenerationBackend()
{
	if (frameGenerationBackendLatched)
		return;

	frameGenerationBackendAtStartup = GetConfiguredFrameGenerationBackend();
	frameGenerationEnabledAtStartup = settings.frameGenerationMode != 0;
	frameGenerationBackendLatched = true;

	logger::info("[Upscaling] Frame generation backend latched at startup: {} (enabled={})",
		magic_enum::enum_name(frameGenerationBackendAtStartup),
		frameGenerationEnabledAtStartup);
}

Upscaling::FrameGenerationBackend Upscaling::GetConfiguredFrameGenerationBackend() const
{
	const auto backend = static_cast<FrameGenerationBackend>(settings.frameGenerationBackend);
	if (backend >= FrameGenerationBackend::kCount)
		return FrameGenerationBackend::kFSR3FG;
	return backend;
}

Upscaling::FrameGenerationBackend Upscaling::GetFrameGenerationBackend() const
{
	return frameGenerationBackendLatched ? frameGenerationBackendAtStartup : GetConfiguredFrameGenerationBackend();
}

bool Upscaling::IsFrameGenerationEnabled() const
{
	if (!frameGenerationBackendLatched)
		return settings.frameGenerationMode != 0;

	// A DLSS-G session always stands frame generation up, whether or not the setting is on.
	//
	// It costs nothing to do so: the proxy swap chain is created for any windowed DLSS-G
	// session already, and CreateProxyInterop allocates the frame-generation motion-vector
	// buffer on IsDLSSGBackend() alone -- so the resources this query appeared to gate were
	// never actually keyed to the setting. What it did gate was the ability to turn generation
	// back on, which produced the case this removes: switching generation off to use Neural
	// Rendering, then needing two more launches to get it back -- one to set the toggle, one to
	// boot with it set.
	//
	// The FSR path keeps the startup latch. Its swap chain is FidelityFX's own and it has no
	// equivalent of SetDLSSGMode to suspend and resume generation within a session.
	if (IsDLSSGBackend() && HasFrameGenModule())
		return true;

	return frameGenerationEnabledAtStartup;
}

// (batch 29) What the frame actually wants, as opposed to what the session was configured for.
//
// The two have to stay separate. IsFrameGenerationEnabled decides whether frame-generation
// resources exist at all, and CreateUpscalingTextureResources rebuilds its whole working set
// whenever that answer changes -- so letting the live setting reach it would tear down and
// recreate textures in the middle of a frame every time the checkbox moved. This one carries the
// live setting and is read only by paths that decide something for the current frame.
//
// Asymmetric on purpose, and the asymmetry is physical rather than cautious. Turning frame
// generation off needs nothing new: DLSS-G already goes to eOff every time a menu opens, with
// eRetainResourcesWhenOff so it can come back cheaply. Turning it on needs the D3D11-to-D3D12
// proxy swapchain, and that is created once, at device creation, from the latched state -- with
// no proxy there is nothing to present generated frames through. So the boot state still gates
// the on direction, and the live setting only ever subtracts.
bool Upscaling::IsFrameGenerationRequestedNow() const
{
	return IsFrameGenerationEnabled() && settings.frameGenerationMode != 0;
}

bool Upscaling::IsDLSSGBackend() const
{
	return GetFrameGenerationBackend() == FrameGenerationBackend::kDLSSG;
}

bool Upscaling::IsDLSSGAvailable() const
{
	return streamline.IsDLSSGReady();
}

bool Upscaling::IsFrameGenerationAllowedWhilePaused() const
{
	if (!settings.frameGenerationAllowInMenus)
		return false;

	// The main menu and loading screens are never eligible: there is no world scene behind
	// them, so generation would be interpolating between two images of a menu. Everything
	// else that pauses -- inventory, the journal, the system menu -- is a real rendered frame
	// with the world still behind it, which is what the setting opens up.
	auto* ui = globals::game::ui;
	return ui != nullptr &&
	       !ui->IsMenuOpen(RE::MainMenu::MENU_NAME) &&
	       !ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
}

bool Upscaling::IsDLSSGMapRenderingContext()
{
	auto* ui = globals::game::ui;
	return ui != nullptr &&
		d3d12SwapChainActive &&
		IsDLSSGBackend() &&
		IsFrameGenerationEnabled() &&
		GetUpscaleMethod() == UpscaleMethod::kDLSS &&
		!settings.enableDLSSRR &&
		ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
}

void Upscaling::PresentFrameGeneration(bool a_useFrameGeneration, bool a_retainDLSSGResourcesWhenOff)
{
	if (!IsDLSSGBackend()) {
		fidelityFX.Present(a_useFrameGeneration);
		return;
	}

	if (!IsDLSSGAvailable()) {
		static bool unavailableLogged = false;
		if (a_useFrameGeneration && !unavailableLogged) {
			logger::warn("[Upscaling] DLSS-G is selected but its Streamline interface is unavailable");
			unavailableLogged = true;
		}
		return;
	}

	streamline.SetDLSSGMode(a_useFrameGeneration, a_retainDLSSGResourcesWhenOff);
}

void Upscaling::CheckFrameConstants()
{
	streamline.CheckFrameConstants();
}

void Upscaling::SetUIBuffer()
{
	dx12SwapChain.SetUIBuffer();
}

HANDLE Upscaling::GetFrameLatencyWaitableObject() const
{
	return dx12SwapChain.GetFrameLatencyWaitableObject();
}

float Upscaling::GetFrameTime() const
{
	return dx12SwapChain.GetFrameTime();
}

// Backend interface methods
bool Upscaling::IsBackendInitialized() const
{
	return streamline.initialized;
}

void Upscaling::CheckBackendFeatures(IDXGIAdapter* adapter)
{
	streamline.CheckFeatures(adapter);
}

void Upscaling::UpgradeBackendInterface(void** ppInterface)
{
	if (const auto result = streamline.UpgradeInterface(ppInterface); result != sl::Result::eOk)
		logger::warn("[Streamline] Failed to upgrade a D3D12 interface ({})", magic_enum::enum_name(result));
}

void Upscaling::SetBackendD3DDevice(void* device)
{
	if (const auto result = streamline.SetD3DDevice(device); result != sl::Result::eOk)
		logger::warn("[Streamline] Failed to register the D3D12 device ({})", magic_enum::enum_name(result));
}

void Upscaling::PostBackendDevice()
{
	streamline.PostDevice();
}

// Module availability methods
bool Upscaling::HasFrameGenModule() const
{
	if (IsDLSSGBackend())
		// This query is used before PostDevice() to decide whether the proxy
		// swap chain must be created. Function pointers are not ready yet, so
		// use the feature support result latched by CheckFeatures().
		return streamline.featureDLSS_G && streamline.featureReflex && streamline.featurePCL;
	return fidelityFX.featureFSR3FG;
}

// Proxy interface methods
void Upscaling::SetProxyD3D11Device(ID3D11Device* device)
{
	dx12SwapChain.SetD3D11Device(device);
}

void Upscaling::SetProxyD3D11DeviceContext(ID3D11DeviceContext* context)
{
	dx12SwapChain.SetD3D11DeviceContext(context);
}

void Upscaling::CreateProxySwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc)
{
	dx12SwapChain.CreateSwapChain(adapter, swapChainDesc);
}

void Upscaling::CreateProxyInterop()
{
	dx12SwapChain.CreateInterop();
}

IDXGISwapChain* Upscaling::GetProxySwapChain()
{
	return dx12SwapChain.GetSwapChainProxy();
}

void Upscaling::Upscale()
{
	auto upscaleMethod = GetUpscaleMethod();

	auto state = globals::state;
	auto context = globals::d3d::context;
	auto renderer = globals::game::renderer;

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];

	auto dispatchCount = Util::GetScreenDispatchCount(true);

	{
		state->BeginPerfEvent("Encode Upscaling Textures");

		auto& temporalAAMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kTEMPORAL_AA_MASK];
		auto& normals = renderer->GetRuntimeData().renderTargets[globals::deferred->forwardRenderTargets[2]];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& normalroughness = renderer->GetRuntimeData().renderTargets[NORMALROUGHNESS];

		{
			// Set up upscaling data constant buffer
			auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);
			UpscalingDataCB upscalingData;
			upscalingData.trueSamplingDim = renderSize;

			upscalingDataCB->Update(upscalingData);
			auto upscalingBuffer = upscalingDataCB->CB();
			context->CSSetConstantBuffers(0, 1, &upscalingBuffer);

			ID3D11ShaderResourceView* views[5] = { temporalAAMask.SRV, normals.SRV, motionVector.SRV, depth.depthSRV, normalroughness.SRV };
			context->CSSetShaderResources(0, ARRAYSIZE(views), views);

			bool isDLSS = (upscaleMethod == UpscaleMethod::kDLSS);

			ID3D11UnorderedAccessView* reactiveMaskUAV = isDLSS ? dx12SwapChain.reactiveMaskShared12->uav : reactiveMaskTexture->uav.get();
			ID3D11UnorderedAccessView* transparencyUAV = isDLSS ? dx12SwapChain.transparencyCompositionMaskShared12->uav : transparencyCompositionMaskTexture->uav.get();
			ID3D11UnorderedAccessView* motionVectorUAV = isDLSS ? dx12SwapChain.motionVectorBufferShared12->uav : nullptr;
			ID3D11UnorderedAccessView* packedNormalUAV = isDLSS ? dx12SwapChain.packedNormalShared12->uav : nullptr;

			ID3D11UnorderedAccessView* uavs[4] = { reactiveMaskUAV, transparencyUAV, motionVectorUAV, packedNormalUAV };
			context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

			context->CSSetShader(GetEncodeTexturesCS(), nullptr, 0);

			context->Dispatch(dispatchCount.x, dispatchCount.y, 1);
		}

		ID3D11ShaderResourceView* views[5] = { nullptr, nullptr, nullptr, nullptr, nullptr };
		context->CSSetShaderResources(0, ARRAYSIZE(views), views);

		ID3D11UnorderedAccessView* uavs[4] = { nullptr, nullptr, nullptr, nullptr };
		context->CSSetUnorderedAccessViews(0, ARRAYSIZE(uavs), uavs, nullptr);

		ID3D11Buffer* nullBuffer = nullptr;
		context->CSSetConstantBuffers(7, 1, &nullBuffer);

		ID3D11ComputeShader* shader = nullptr;
		context->CSSetShader(shader, nullptr, 0);

		state->EndPerfEvent();
	}

	{
		state->BeginPerfEvent("Upscaling");

		if ((upscaleMethod == UpscaleMethod::kDLSS) && d3d12SwapChainActive) {
			HRESULT hr = dx12SwapChain.d3d12Device->GetDeviceRemovedReason();
			if (hr != S_OK) {
				logger::error("D3D12 device removed: 0x{:08X}", static_cast<uint32_t>(hr));
				settings.upscaleMethod = (uint)UpscaleMethod::kTAA;
				d3d12SwapChainActive = false;
				return;
			}
			auto renderSize = Util::ConvertToDynamic(globals::state->screenSize);

			// Copy input color texture to shared D3D12 resource
			context->CopyResource(dx12SwapChain.inputColorBufferShared12->resource11, main.texture);

			// Wait for D3D11 to finish
			DX::ThrowIfFailed(context->QueryInterface(IID_PPV_ARGS(dx12SwapChain.d3d11Context.put())));
			auto fence = dx12SwapChain.fenceValue;
			logger::trace("Signaling shared fence {} before upscaling", fence);
			DX::ThrowIfFailed(dx12SwapChain.d3d11Context->Signal(dx12SwapChain.d3d11Fence.get(), fence));
			logger::trace("Waiting for shared fence {} before upscaling", fence);
			DX::ThrowIfFailed(dx12SwapChain.commandQueue->Wait(dx12SwapChain.d3d12Fence.get(), fence));
			dx12SwapChain.fenceValue++;

			auto frameIndex = dx12SwapChain.frameIndex;

			// Reset command allocator and list
			DX::ThrowIfFailed(dx12SwapChain.dlssCommandAllocator[frameIndex]->Reset());
			DX::ThrowIfFailed(dx12SwapChain.dlssCommandList[frameIndex]->Reset(dx12SwapChain.dlssCommandAllocator[frameIndex].get(), nullptr));

			if (!settings.enableDLSSRR) {
				streamline.Upscale(
					dx12SwapChain.inputColorBufferShared12->resource.get(),
					dx12SwapChain.motionVectorBufferShared12->resource.get(),
					dx12SwapChain.depthBufferShared12->resource.get(),
					dx12SwapChain.reactiveMaskShared12->resource.get(),
					dx12SwapChain.transparencyCompositionMaskShared12->resource.get(),
					dx12SwapChain.outputColorBufferShared12->resource.get(),
					dx12SwapChain.dlssCommandList[frameIndex].get()
				);
			} else {
				logger::debug("Call DLSS RR");
				streamline.RayReconstruction(
					dx12SwapChain.inputColorBufferShared12->resource.get(),
					dx12SwapChain.motionVectorBufferShared12->resource.get(),
					dx12SwapChain.depthBufferShared12->resource.get(),
					dx12SwapChain.albedoShared12->resource.get(),
					dx12SwapChain.reflectanceShared12->resource.get(),
					dx12SwapChain.packedNormalShared12->resource.get(),
					dx12SwapChain.specHitDistanceShared12->resource.get(),
					dx12SwapChain.colorBeforeTransparencySnapshot->resource.get(),
					dx12SwapChain.sssGuide->resource.get(),
					dx12SwapChain.outputColorBufferShared12->resource.get(),
					dx12SwapChain.dlssCommandList[frameIndex].get()
				);
			}

			// Close and execute command list
			DX::ThrowIfFailed(dx12SwapChain.dlssCommandList[frameIndex]->Close());

			ID3D12CommandList* commandLists[] = { dx12SwapChain.dlssCommandList[frameIndex].get() };
			dx12SwapChain.commandQueue->ExecuteCommandLists(1, commandLists);

			// Wait for D3D12 to finish
			fence = dx12SwapChain.fenceValue;
			logger::trace("Signaling shared fence {} after upscaling", fence);
			DX::ThrowIfFailed(dx12SwapChain.commandQueue->Signal(dx12SwapChain.d3d12Fence.get(), fence));
			logger::trace("Waiting for shared fence {} after upscaling", fence);
			DX::ThrowIfFailed(dx12SwapChain.d3d11Context->Wait(dx12SwapChain.d3d11Fence.get(), fence));
			dx12SwapChain.fenceValue++;

			// Copy back to main buffer
			context->CopyResource(main.texture, dx12SwapChain.outputColorBufferShared12->resource11);
		} else if (upscaleMethod == UpscaleMethod::kFSR) {
			fidelityFX.Upscale(main.texture, reactiveMaskTexture->resource.get(), transparencyCompositionMaskTexture->resource.get(), motionVector.texture, settings.sharpnessFSR);
		}

		state->EndPerfEvent();
	}
}

void Upscaling::PerformUpscaling()
{
	Upscale();
	UpscaleDepth();

	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();

	// Disable dynamic resolution past this point
	runtimeData.dynamicResolutionLock = 1;

	// Updates the PerFrame constant buffer so that dynamic resolution settings are disabled
	UpdateCameraData();
}

void Upscaling::UpscaleDepth()
{
	if (resolutionScale.x != 1.0f) {
		globals::state->BeginPerfEvent("Render Target Upscaling");

		auto& renderer = globals::game::renderer;
		auto context = globals::d3d::context;

		// Set up Input Assembler for fullscreen triangle (no vertex/index buffers needed)
		context->IASetInputLayout(nullptr);
		context->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
		context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
		context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

		// Set up vertex shader that generates fullscreen triangle using SV_VertexID
		context->VSSetShader(GetUpscaleVS(), nullptr, 0);

		// Set up viewport for fullscreen rendering
		auto screenSize = globals::state->screenSize;

		D3D11_VIEWPORT viewport = {};
		viewport.TopLeftX = 0.0f;
		viewport.TopLeftY = 0.0f;
		viewport.Width = screenSize.x;
		viewport.Height = screenSize.y;
		viewport.MinDepth = 0.0f;
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);

		// Set rasterizer state
		context->RSSetState(upscaleRasterizerState.get());

		// Set blend state
		context->OMSetBlendState(upscaleBlendState.get(), nullptr, 0xffffffff);

		// Set up pixel shader resources
		auto deferred = globals::deferred;

		ID3D11SamplerState* samplers[] = { deferred->linearSampler.get() };
		context->PSSetSamplers(0, ARRAYSIZE(samplers), samplers);

		// Set up jitter constant buffer for upscaling
		JitterCB jitterData;
		jitterData.jitter = jitter;

		jitterCB->Update(jitterData);
		auto bufferArray = jitterCB->CB();
		context->PSSetConstantBuffers(0, 1, &bufferArray);

		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];

		{
			auto& refractionNormals = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kREFRACTION_NORMALS];
			auto& saoCameraZ = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kSAO_CAMERAZ];

			auto& depthCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN_COPY];

			// Sometimes this is not already copied e.g. map menu
			context->CopyResource(depthCopy.texture, depth.texture);

			// Clear stencil to be 0xFF
			if (globals::game::isVR)
				context->ClearDepthStencilView(depthCopy.views[0], D3D11_CLEAR_STENCIL, 1.0f, 0xFF);

			// Set depth stencil state to write 0x00
			context->OMSetDepthStencilState(upscaleDepthStencilState.get(), 0x00);

			context->CopyResource(refractionNormals.textureCopy, refractionNormals.texture);

			ID3D11ShaderResourceView* srvs[] = { refractionNormals.SRVCopy, depthCopy.depthSRV, depthCopy.stencilSRV };
			context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11RenderTargetView* rtvs[] = { refractionNormals.RTV, saoCameraZ.RTV };
			context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, depth.views[0]);

			context->PSSetShader(GetDepthRefractionUpscalePS(), nullptr, 0);
			context->Draw(3, 0);

			// Depth copy is also used on VR
			if (globals::game::isVR)
				context->CopyResource(depthCopy.texture, depth.texture);
		}

		{
			viewport.Width = screenSize.x * 0.5f;
			viewport.Height = screenSize.y * 0.5f;
			context->RSSetViewports(1, &viewport);

			auto& underwaterMask = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kUNDERWATER_MASK];

			context->CopyResource(underwaterMask.textureCopy, underwaterMask.texture);

			context->OMSetDepthStencilState(nullptr, 0x00);

			ID3D11ShaderResourceView* srvs[] = { underwaterMask.SRVCopy };
			context->PSSetShaderResources(0, ARRAYSIZE(srvs), srvs);

			ID3D11RenderTargetView* rtvs[] = { underwaterMask.RTV };
			context->OMSetRenderTargets(ARRAYSIZE(rtvs), rtvs, nullptr);

			context->PSSetShader(GetUnderwaterMaskUpscalePS(), nullptr, 0);
			context->Draw(3, 0);
		}

		ID3D11ShaderResourceView* nullPSResources[3] = { nullptr, nullptr, nullptr };
		context->PSSetShaderResources(0, ARRAYSIZE(nullPSResources), nullPSResources);

		globals::state->EndPerfEvent();
	}
}

void Upscaling::ApplyNISSharpening()
{
	if (!streamline.featureNIS || settings.sharpnessDLSS <= 0.0f) {
		return;
	}

	auto context = globals::d3d::context;

	ID3D11RenderTargetView* renderTarget = nullptr;
	context->OMGetRenderTargets(1, &renderTarget, nullptr);

	winrt::com_ptr<ID3D11Resource> mainResource;
	renderTarget->GetResource(mainResource.put());

	context->OMSetRenderTargets(0, nullptr, nullptr);  // Unbind all bound render targets

	context->CopyResource(dx12SwapChain.nisSharpenerInputShared12->resource11, mainResource.get());

	// Wait for D3D11 to finish
	auto fence = dx12SwapChain.fenceValue;
	DX::ThrowIfFailed(context->QueryInterface(IID_PPV_ARGS(dx12SwapChain.d3d11Context.put())));
	logger::trace("Signaling shared fence {} before NIS sharpening", fence);
	DX::ThrowIfFailed(dx12SwapChain.d3d11Context->Signal(dx12SwapChain.d3d11Fence.get(), fence));
	logger::trace("Waiting for shared fence {} before NIS sharpening", fence);
	DX::ThrowIfFailed(dx12SwapChain.commandQueue->Wait(dx12SwapChain.d3d12Fence.get(), fence));
	dx12SwapChain.fenceValue++;

	auto frameIndex = dx12SwapChain.frameIndex;

	// Reset command allocator and list
	DX::ThrowIfFailed(dx12SwapChain.nisSharpenerCommandAllocator[frameIndex]->Reset());
	DX::ThrowIfFailed(dx12SwapChain.nisSharpenerCommandList[frameIndex]->Reset(dx12SwapChain.nisSharpenerCommandAllocator[frameIndex].get(), nullptr));

	streamline.ApplyNISSharpening(dx12SwapChain.nisSharpenerInputShared12->resource.get(), dx12SwapChain.nisSharpenerOutputShared12->resource.get(), settings.sharpnessDLSS, dx12SwapChain.nisSharpenerCommandList[frameIndex].get());

	// Close and execute command list
	DX::ThrowIfFailed(dx12SwapChain.nisSharpenerCommandList[frameIndex]->Close());

	ID3D12CommandList* commandLists[] = { dx12SwapChain.nisSharpenerCommandList[frameIndex].get() };
	dx12SwapChain.commandQueue->ExecuteCommandLists(1, commandLists);

	// Wait for D3D12 to finish
	fence = dx12SwapChain.fenceValue;
	logger::trace("Signaling shared fence {} after NIS sharpening", fence);
	DX::ThrowIfFailed(dx12SwapChain.commandQueue->Signal(dx12SwapChain.d3d12Fence.get(), fence));
	logger::trace("Waiting for shared fence {} after NIS sharpening", fence);
	DX::ThrowIfFailed(dx12SwapChain.d3d11Context->Wait(dx12SwapChain.d3d11Fence.get(), fence));
	dx12SwapChain.fenceValue++;

	// Copy back to main buffer
	context->CopyResource(mainResource.get(), dx12SwapChain.nisSharpenerOutputShared12->resource11);

	globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);  // Run OMSetRenderTargets again

	if (renderTarget)
		renderTarget->Release();
}

void Upscaling::SnapshotBeforeTransparency()
{
	if (!d3d12SwapChainActive)
		return;

	auto context = globals::d3d::context;

	auto renderer = globals::game::renderer;
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];

	context->CopyResource(dx12SwapChain.colorBeforeTransparencySnapshot->resource11, main.texture);
}

void Upscaling::Main_UpdateJitter::thunk(RE::BSGraphics::State* a_state)
{
	globals::features::upscaling.ConfigureTAA();
	func(a_state);
	globals::features::upscaling.ConfigureUpscaling(a_state);
}

void Upscaling::MenuManagerDrawInterfaceStartHook::thunk(int64_t a1)
{
	globals::features::upscaling.PostDisplay();
	func(a1);
}

void Upscaling::Main_PostProcessing::thunk(RE::ImageSpaceManager* a_this, uint32_t a3, RE::RENDER_TARGET a_target, void* a_4, bool a_5)
{
	auto& postProcessing = globals::features::postProcessing;
	if (postProcessing.loaded) {
		postProcessing.DrawBeforeUpscaling();
	}

	auto& upscaling = globals::features::upscaling;
	auto upscaleMethod = upscaling.GetUpscaleMethod();

	if (upscaling.d3d12SwapChainActive && (upscaling.IsFrameGenerationEnabled() || upscaleMethod == UpscaleMethod::kDLSS))
		upscaling.CopySharedD3D12Resources();

	if (upscaling.d3d12SwapChainActive) {
		dx12SwapChain.upscalingFenceValue++;
		logger::trace("[Upscaling Begin] Clearing queue using upscaling fence {}", dx12SwapChain.upscalingFenceValue);
		DX::ThrowIfFailed(dx12SwapChain.commandQueue->Signal(dx12SwapChain.upscalingFence.get(), dx12SwapChain.upscalingFenceValue));
		DX::ThrowIfFailed(dx12SwapChain.commandQueue->Wait(dx12SwapChain.upscalingFence.get(), dx12SwapChain.upscalingFenceValue));
	}

	if (upscaleMethod != UpscaleMethod::kNONE && upscaleMethod != UpscaleMethod::kTAA)
		upscaling.PerformUpscaling();

	auto imageSpaceManager = RE::ImageSpaceManager::GetSingleton();
	GET_INSTANCE_MEMBER(BSImagespaceShaderISTemporalAA, imageSpaceManager);

	BSImagespaceShaderISTemporalAA->taaEnabled = upscaleMethod == UpscaleMethod::kTAA;

	func(a_this, a3, a_target, a_4, a_5);

	if (upscaling.d3d12SwapChainActive) {
		dx12SwapChain.upscalingFenceValue++;
		logger::trace("[NIS] Clearing queue using upscaling fence {}", dx12SwapChain.upscalingFenceValue);
		DX::ThrowIfFailed(dx12SwapChain.commandQueue->Signal(dx12SwapChain.upscalingFence.get(), dx12SwapChain.upscalingFenceValue));
		DX::ThrowIfFailed(dx12SwapChain.commandQueue->Wait(dx12SwapChain.upscalingFence.get(), dx12SwapChain.upscalingFenceValue));
	}

	// (batch 26b) Neural Rendering runs before the sharpener, not after. Both take the scene
	// from the currently bound render target, and ApplyNISSharpening leaves targets unbound --
	// so after it there is nothing to read. It also early-returns when sharpening is off, which
	// would have made the binding's presence depend on an unrelated setting. Running the network
	// on the unsharpened image and sharpening its output is the right order anyway.
	if (!globals::game::isVR)
		NeuralRendering::ApplyLdr();

	if (upscaleMethod == UpscaleMethod::kDLSS)
		upscaling.ApplyNISSharpening();

	if (upscaling.d3d12SwapChainActive) {
		dx12SwapChain.upscalingFenceValue++;
		logger::trace("[Upscaling End] Clearing queue using upscaling fence {}", dx12SwapChain.upscalingFenceValue);
		DX::ThrowIfFailed(dx12SwapChain.commandQueue->Signal(dx12SwapChain.upscalingFence.get(), dx12SwapChain.upscalingFenceValue));
		DX::ThrowIfFailed(dx12SwapChain.commandQueue->Wait(dx12SwapChain.upscalingFence.get(), dx12SwapChain.upscalingFenceValue));
	}

	// Disable TAA in some menus
	BSImagespaceShaderISTemporalAA->taaEnabled = false;
}

void Upscaling::SetScissorRect::thunk(RE::BSGraphics::Renderer* This, int a_left, int a_top, int a_right, int a_bottom)
{
	auto viewport = globals::game::graphicsState;
	auto& runtimeData = viewport->GetRuntimeData();

	if (!runtimeData.dynamicResolutionLock) {
		a_left = static_cast<int>(a_left * runtimeData.dynamicResolutionWidthRatio);
		a_right = static_cast<int>(a_right * runtimeData.dynamicResolutionWidthRatio);

		a_top = static_cast<int>(a_top * runtimeData.dynamicResolutionHeightRatio);
		a_bottom = static_cast<int>(a_bottom * runtimeData.dynamicResolutionHeightRatio);
	}

	func(This, a_left, a_top, a_right, a_bottom);
}

void Upscaling::Main_RenderPrecipitation::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}

void Upscaling::BSFaceGenManager_UpdatePendingCustomizationTextures::thunk()
{
	auto& runtimeData = globals::game::graphicsState->GetRuntimeData();
	runtimeData.dynamicResolutionLock = 1;
	func();
	runtimeData.dynamicResolutionLock = 0;
}
