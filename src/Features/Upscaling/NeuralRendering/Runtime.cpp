// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#include "Runtime.h"

#include "Integration.h"
#include "Util.h"

#include <Windows.h>
#include <Psapi.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>

#include <array>
#include <cstring>
#include <format>
#include <vector>

#include <nvsdk_ngx_helpers.h>

namespace NeuralRendering
{
	namespace
	{
		constexpr wchar_t kRuntimeName[] = L"nvngx_dlssnr.dll";
		constexpr auto kFeatureDlssNr = static_cast<NVSDK_NGX_Feature>(18);
		constexpr std::array<const char*, 5> kRequiredExports{
			"NVSDK_NGX_D3D12_Init_Ext",
			"NVSDK_NGX_D3D12_CreateFeature",
			"NVSDK_NGX_D3D12_EvaluateFeature",
			"NVSDK_NGX_D3D12_ReleaseFeature",
			"NVSDK_NGX_D3D12_Shutdown1",
		};

		using GetUnsignedValue = unsigned int(NVSDK_CONV*)();
		using InitD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_Parameter*);
		using ShutdownD3D12 = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);
		using AllocateParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);
		using DestroyParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using CreateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
		using EvaluateFeature = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
		using ReleaseFeature = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
		using GetModuleFileNameWFunction = DWORD(WINAPI*)(HMODULE, LPWSTR, DWORD);

		GetModuleFileNameWFunction g_originalGetModuleFileNameW = nullptr;
		HMODULE g_callerModule = nullptr;
		std::wstring g_spoofedRuntimePath;
		std::uint32_t g_proxyHits = 0;

		HMODULE FindNgxCoreModule()
		{
			std::array<HMODULE, 1024> modules{};
			DWORD bytesNeeded = 0;
			if (!K32EnumProcessModules(GetCurrentProcess(), modules.data(), static_cast<DWORD>(sizeof(modules)), &bytesNeeded))
				return nullptr;
			const std::size_t count = std::min<std::size_t>(modules.size(), bytesNeeded / sizeof(HMODULE));
			for (std::size_t index = 0; index < count; ++index) {
				if (GetProcAddress(modules[index], "NVSDK_NGX_D3D12_AllocateParameters") &&
					GetProcAddress(modules[index], "NVSDK_NGX_D3D12_DestroyParameters"))
					return modules[index];
			}
			return nullptr;
		}

		DWORD WINAPI SignedRuntimeGetModuleFileNameW(HMODULE module, LPWSTR filename, DWORD size)
		{
			if (module == g_callerModule && filename && size && !g_spoofedRuntimePath.empty()) {
				++g_proxyHits;
				const DWORD length = static_cast<DWORD>(g_spoofedRuntimePath.size());
				const DWORD copyLength = std::min(length, size - 1);
				std::memcpy(filename, g_spoofedRuntimePath.data(), copyLength * sizeof(wchar_t));
				filename[copyLength] = L'\0';
				return copyLength < length ? size : length;
			}
			return g_originalGetModuleFileNameW ? g_originalGetModuleFileNameW(module, filename, size) : 0;
		}

		class SignedRuntimePathScope
		{
		public:
			SignedRuntimePathScope(HMODULE runtime, const std::filesystem::path& ngxPath)
			{
				if (!runtime)
					return;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
					reinterpret_cast<LPCWSTR>(&SignedRuntimeGetModuleFileNameW), &g_callerModule);
				g_spoofedRuntimePath = ngxPath.wstring();
				g_proxyHits = 0;

				auto* base = reinterpret_cast<std::byte*>(runtime);
				auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
				if (dos->e_magic != IMAGE_DOS_SIGNATURE)
					return;
				auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
				if (nt->Signature != IMAGE_NT_SIGNATURE)
					return;
				const auto& imports = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
				if (!imports.VirtualAddress)
					return;

				auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
				for (; descriptor->Name; ++descriptor) {
					if (!descriptor->OriginalFirstThunk)
						continue;
					auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
					auto* functions = reinterpret_cast<IMAGE_THUNK_DATA*>(base + descriptor->FirstThunk);
					for (; names->u1.AddressOfData; ++names, ++functions) {
						if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal))
							continue;
						auto* import = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
						if (std::strcmp(reinterpret_cast<const char*>(import->Name), "GetModuleFileNameW"))
							continue;
						slot_ = reinterpret_cast<void**>(&functions->u1.Function);
						g_originalGetModuleFileNameW = reinterpret_cast<GetModuleFileNameWFunction>(*slot_);
						DWORD oldProtection = 0;
						if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
							*slot_ = reinterpret_cast<void*>(&SignedRuntimeGetModuleFileNameW);
							DWORD ignored = 0;
							VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
							FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
							installed_ = true;
						}
						return;
					}
				}
			}

			~SignedRuntimePathScope()
			{
				if (installed_ && slot_) {
					DWORD oldProtection = 0;
					if (VirtualProtect(slot_, sizeof(*slot_), PAGE_READWRITE, &oldProtection)) {
						*slot_ = reinterpret_cast<void*>(g_originalGetModuleFileNameW);
						DWORD ignored = 0;
						VirtualProtect(slot_, sizeof(*slot_), oldProtection, &ignored);
						FlushInstructionCache(GetCurrentProcess(), slot_, sizeof(*slot_));
					}
				}
				g_spoofedRuntimePath.clear();
				g_callerModule = nullptr;
				g_originalGetModuleFileNameW = nullptr;
			}

			[[nodiscard]] bool IsInstalled() const { return installed_; }
			[[nodiscard]] std::uint32_t Hits() const { return g_proxyHits; }

		private:
			void** slot_ = nullptr;
			bool installed_ = false;
		};

		// (batch 42a) NGX results that have a known cause outside our code, as a hint for the log.
		const char* NgxResultHint(std::uint32_t result)
		{
			// DLSS5oneclick v0.13.0: a second Neural Rendering user in the same process makes the second
			// CreateFeature(18) fail with this. In Skyrim that is a RenoDX DLSS 5 add-on or an OptiScaler
			// NR build left in the game folder.
			if (result == 0xBAD0000Bu)
				return " -- hint: leftover RenoDX/OptiScaler NR add-ons in the game folder (another Neural Rendering user in this "
				       "process, e.g. renodx-dlss5.addon64 or an OptiScaler DLSSNR build next to SkyrimSE.exe); remove them";
			return "";
		}

		using ScalingRatioCallback = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
		using GetCapabilityParameters = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter**);

		// No C++ objects here, so it can hold an SEH frame: the callback's signature is inferred
		// (NGX's usual "callback(parameters)" shape, as DLSS's optimal-settings callback), not documented.
		NVSDK_NGX_Result CallScalingCallbackGuarded(ScalingRatioCallback callback, NVSDK_NGX_Parameter* parameters, bool* faulted)
		{
			__try {
				return callback(parameters);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				*faulted = true;
				return NVSDK_NGX_Result_Fail;
			}
		}

		std::filesystem::path ResolveRuntimePath(const std::filesystem::path& explicitPath)
		{
			if (!explicitPath.empty())
				return std::filesystem::is_directory(explicitPath) ? explicitPath / kRuntimeName : explicitPath;
			const auto dataPath = Util::PathHelpers::GetDataPath();
			const std::array candidates{
				dataPath / L"Shaders/Upscaling/Streamline" / kRuntimeName,
				dataPath / L"Shaders/Upscaling/StreamlineDX12" / kRuntimeName,
			};
			for (const auto& candidate : candidates) {
				std::error_code error;
				if (std::filesystem::is_regular_file(candidate, error))
					return candidate;
			}
			return {};
		}
	}

	Runtime& Runtime::Instance()
	{
		static Runtime instance;
		return instance;
	}

	Runtime::~Runtime() { Shutdown(); }

	bool Runtime::Probe(const std::filesystem::path& explicitPath)
	{
		Shutdown();
		path_ = ResolveRuntimePath(explicitPath);
		if (path_.empty()) {
			status_ = RuntimeStatus::NotFound;
			detail_ = "nvngx_dlssnr.dll was not found";
			return false;
		}
		const auto version = Util::GetDllVersion(path_.wstring());
		if (!version) {
			status_ = RuntimeStatus::VersionUnavailable;
			detail_ = "DLL version resource is unavailable";
			return false;
		}
		version_ = Util::GetFormattedVersion(*version);
		if (version->major() != 310 || version->minor() != 8) {
			status_ = RuntimeStatus::UnsupportedVersion;
			detail_ = std::format("expected DLSSNR 310.8.x, found {}", version_);
			return false;
		}
		module_ = LoadLibraryExW(path_.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
		if (!module_) {
			status_ = RuntimeStatus::LoadFailed;
			detail_ = std::format("LoadLibraryExW failed with {}", GetLastError());
			return false;
		}
		for (const char* exportName : kRequiredExports) {
			if (!GetProcAddress(static_cast<HMODULE>(module_), exportName)) {
				status_ = RuntimeStatus::MissingExport;
				detail_ = std::format("missing export {}", exportName);
				FreeLibrary(static_cast<HMODULE>(module_));
				module_ = nullptr;
				return false;
			}
		}
		auto getAppId = reinterpret_cast<GetUnsignedValue>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_GetApplicationId"));
		auto getApi = reinterpret_cast<GetUnsignedValue>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_GetAPIVersion"));
		if (!getAppId || !getApi) {
			status_ = RuntimeStatus::MissingExport;
			detail_ = "signed runtime identity exports are missing";
			FreeLibrary(static_cast<HMODULE>(module_));
			module_ = nullptr;
			return false;
		}
		applicationId_ = getAppId();
		apiVersion_ = getApi();
		status_ = RuntimeStatus::Ready;
		return true;
	}

	bool Runtime::Initialize(ID3D12Device* device, const std::filesystem::path& dataPath)
	{
		if (!device)
			return false;
		if (status_ == RuntimeStatus::Initialized && device_ != device)
			Shutdown();
		if (!module_ && !Probe())
			return false;
		if (status_ == RuntimeStatus::Initialized && device_ == device)
			return true;

		auto initialize = reinterpret_cast<InitD3D12>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_Init_Ext"));
		std::filesystem::path writablePath = dataPath;
		if (writablePath.empty()) {
			wchar_t tempPath[MAX_PATH]{};
			GetTempPathW(MAX_PATH, tempPath);
			writablePath = std::filesystem::path(tempPath) / L"OpenShaders-NGX";
		}
		std::error_code error;
		std::filesystem::create_directories(writablePath, error);
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		if (!scope.IsInstalled()) {
			status_ = RuntimeStatus::InitializationFailed;
			detail_ = "failed to install signed-runtime path proxy";
			return false;
		}
		ngxResult_ = static_cast<std::uint32_t>(initialize(applicationId_, writablePath.c_str(), device,
			static_cast<NVSDK_NGX_Version>(apiVersion_), nullptr));
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			status_ = RuntimeStatus::InitializationFailed;
			detail_ = std::format("NGX init failed 0x{:08X}, proxyHits={}", ngxResult_, scope.Hits());
			return false;
		}
		device_ = device;
		device_->AddRef();

		HMODULE core = FindNgxCoreModule();
		if (!core) {
			status_ = RuntimeStatus::CoreUnavailable;
			detail_ = "NGX parameter API module was not found";
			Shutdown();
			status_ = RuntimeStatus::CoreUnavailable;
			detail_ = "NGX parameter API module was not found";
			return false;
		}
		auto allocate = reinterpret_cast<AllocateParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
		NVSDK_NGX_Parameter* parameters = nullptr;
		ngxResult_ = static_cast<std::uint32_t>(allocate(&parameters));
		if (ngxResult_ != NVSDK_NGX_Result_Success || !parameters) {
			const auto allocationResult = ngxResult_;
			Shutdown();
			status_ = RuntimeStatus::ParameterAllocationFailed;
			ngxResult_ = allocationResult;
			detail_ = std::format("NGX parameter allocation failed 0x{:08X}", allocationResult);
			return false;
		}
		parameters_ = parameters;
		status_ = RuntimeStatus::Initialized;
		return true;
	}

	bool Runtime::Execute(ID3D12GraphicsCommandList* commandList, std::uint32_t slot,
		ID3D12Resource* color, ID3D12Resource* depth, ID3D12Resource* motionVectors, ID3D12Resource* output,
		std::uint32_t inputWidth, std::uint32_t inputHeight, std::uint32_t outputWidth, std::uint32_t outputHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning, bool reset,
		float modelScale, bool tuningAtCreate, ID3D12Resource* controlMask, std::uint32_t maskSkyValue)
	{
		if (status_ != RuntimeStatus::Initialized || !commandList || slot >= 2 || !color || !depth || !motionVectors || !output)
			return false;
		auto* parameters = static_cast<NVSDK_NGX_Parameter*>(parameters_);
		auto create = reinterpret_cast<CreateFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_CreateFeature"));
		auto evaluate = reinterpret_cast<EvaluateFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_EvaluateFeature"));
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_ReleaseFeature"));
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		if (!scope.IsInstalled())
			return false;

		// (batch 38a) modelScale > 0 is the Batch 38 path. 0 keeps the 37c value, the ratio of the
		// colour extent to the guide extent, which only ever came out at 1 because the two were always
		// the same size.
		//
		// (batch 38c) 38a passed the model-resolution option here as DLSSNR.ScalingRatio. The DLL does
		// not act on it: the user's log shows the same video memory for the feature at 50% and 100%
		// (+1028..1160 MB either way, after upscaling) and the GPU time did not move. The name is in the
		// DLL, but only beside its ComputeScalingRatio callback, which answers it rather than reads it.
		// Batch 38 now always passes 1 and shrinks the input itself (Renderer::Run).
		const float scalingRatio = modelScale > 0.0f ? modelScale : static_cast<float>(outputWidth) / inputWidth;
		const bool dimensionsChanged = featureInputWidth_[slot] != inputWidth || featureInputHeight_[slot] != inputHeight ||
			featureOutputWidth_[slot] != outputWidth || featureOutputHeight_[slot] != outputHeight ||
			featureScalingRatio_[slot] != scalingRatio || featureControlMask_[slot] != (controlMask != nullptr);
		if (featureHandles_[slot] && dimensionsChanged) {
			release(static_cast<NVSDK_NGX_Handle*>(featureHandles_[slot]));
			featureHandles_[slot] = nullptr;
		}

		if (!featureHandles_[slot]) {
			// (batch 42a) One-off: what the DLL's own scaling callback answers (logged only).
			if (!scalingProbeDone_) {
				scalingProbeDone_ = true;
				ProbeScalingCallback(outputWidth, outputHeight);
			}
			const auto vramBefore = QueryProcessVramMB();
			parameters->Reset();
			parameters->Set("Width", outputWidth);
			parameters->Set("Height", outputHeight);
			// Not in nvngx_dlssnr.dll (byte scan); kept because the NGX core may read them.
			parameters->Set("OutWidth", outputWidth);
			parameters->Set("OutHeight", outputHeight);
			parameters->Set("DLSSNR.Width", outputWidth);
			parameters->Set("DLSSNR.Height", outputHeight);
			// (batch 42a) Removed, none of them is anywhere in nvngx_dlssnr.dll 310.8 (byte scan of
			// the shipped DLL, reviews/research-dlss5-update-2026-10-12.md 3.6): DLSSNR.InputWidth,
			// .InputHeight, .OutputWidth, .OutputHeight, .Output.Width, .Output.Height, .Scale,
			// .Upscaling.
			parameters->Set("DLSSNR.ScalingRatio", scalingRatio);
			parameters->Set("DLSSNR.Hint.Render.Preset", 0u);
			// (batch 38c) The model reads its tuning when the feature is built (OptiScaler's DLSSNR
			// branch found the evaluate-time values ignored; its README: "the model's tuning ... is
			// latched at feature creation"). 38a set them only per evaluate, so the menu's sliders
			// never reached the model. Given here as well; changes rebuild the feature (Renderer).
			if (tuningAtCreate) {
				parameters->Set("DLSSNR.Enabled", 1u);
				parameters->Set("DLSSNR.Intensity", tuning.intensity);
				parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
				parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
				parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
				parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
				parameters->Set("DLSSNR.Style", tuning.style);
				parameters->Set("DLSSNR.UICorrection", tuning.uiCorrection ? 1u : 0u);
			}
			if (controlMask) {
				// (batch 42a) Given at creation as well as per evaluate: whether the model latches its
				// presence at creation is not known.
				parameters->Set("DLSSNR.ControlMask", controlMask);
				parameters->Set("DLSSNR.ControlMaskSubrectBaseX", 0u);
				parameters->Set("DLSSNR.ControlMaskSubrectBaseY", 0u);
				parameters->Set("DLSSNR.ControlMaskSubrectWidth", inputWidth);
				parameters->Set("DLSSNR.ControlMaskSubrectHeight", inputHeight);
			}
			NVSDK_NGX_Handle* handle = nullptr;
			ngxResult_ = static_cast<std::uint32_t>(create(commandList, kFeatureDlssNr, parameters, &handle));
			if (ngxResult_ != NVSDK_NGX_Result_Success || !handle) {
				detail_ = std::format("Feature 18 create failed slot={} network={}x{} result=0x{:08X} proxyHits={}{}", slot,
					outputWidth, outputHeight, ngxResult_, scope.Hits(), NgxResultHint(ngxResult_));
				logger::error("[DLSSNR] {}", detail_);
				return false;
			}
			++featureCreations_;
			const auto vramAfter = QueryProcessVramMB();
			const auto growth = static_cast<std::int64_t>(vramAfter) - static_cast<std::int64_t>(vramBefore);
			logger::info("[DLSSNR] feature created #{} slot={}: network {}x{} (colour = guides = output, given to NGX as Width/Height), "
						 "ScalingRatio {:.3f}, tuning at creation {}, control mask {}; video memory {} MB -> {} MB ({:+} MB)",
				featureCreations_, slot, outputWidth, outputHeight, scalingRatio, tuningAtCreate ? "on" : "off",
				controlMask ? std::format("on (sky = {})", maskSkyValue) : std::string("off"), vramBefore, vramAfter, growth);
			// A leak shows as the process holding more after each rebuild at the same size than it
			// did after the previous one (the old feature is released before the new one is made).
			// Warned past 300 MB per rebuild. A bigger network (higher Model Resolution) legitimately
			// takes more, so the sizes are in the line.
			if (lastCreationVramMB_ && vramAfter > lastCreationVramMB_ + 300)
				logger::warn("[DLSSNR] video memory after this rebuild is {} MB above the previous build ({} MB -> {} MB; network {}x{}, "
							 "previous {}x{}). At the same or a smaller size this is a leak: avoid toggling Neural Rendering settings "
							 "repeatedly and keep the log",
					vramAfter - lastCreationVramMB_, lastCreationVramMB_, vramAfter, outputWidth, outputHeight,
					lastCreationWidth_, lastCreationHeight_);
			lastCreationVramMB_ = vramAfter;
			lastCreationWidth_ = outputWidth;
			lastCreationHeight_ = outputHeight;
			featureHandles_[slot] = handle;
			featureInputWidth_[slot] = inputWidth;
			featureInputHeight_[slot] = inputHeight;
			featureOutputWidth_[slot] = outputWidth;
			featureOutputHeight_[slot] = outputHeight;
			featureScalingRatio_[slot] = scalingRatio;
			featureTuning_[slot] = tuning;
			featureTuningAtCreate_[slot] = tuningAtCreate;
			featureControlMask_[slot] = controlMask != nullptr;
			reset = true;
		}

		parameters->Reset();
		parameters->Set("DLSSNR.Color", color);
		parameters->Set("DLSSNR.Depth", depth);
		parameters->Set("DLSSNR.MVec", motionVectors);
		parameters->Set("DLSSNR.Output", output);
		parameters->Set("DLSSNR.ColorSubrectBaseX", 0u);
		parameters->Set("DLSSNR.ColorSubrectBaseY", 0u);
		parameters->Set("DLSSNR.ColorSubrectWidth", outputWidth);
		parameters->Set("DLSSNR.ColorSubrectHeight", outputHeight);
		parameters->Set("DLSSNR.DepthSubrectBaseX", 0u);
		parameters->Set("DLSSNR.DepthSubrectBaseY", 0u);
		parameters->Set("DLSSNR.DepthSubrectWidth", inputWidth);
		parameters->Set("DLSSNR.DepthSubrectHeight", inputHeight);
		parameters->Set("DLSSNR.MVecSubrectBaseX", 0u);
		parameters->Set("DLSSNR.MVecSubrectBaseY", 0u);
		parameters->Set("DLSSNR.MVecSubrectWidth", inputWidth);
		parameters->Set("DLSSNR.MVecSubrectHeight", inputHeight);
		parameters->Set("DLSSNR.OutputSubrectBaseX", 0u);
		parameters->Set("DLSSNR.OutputSubrectBaseY", 0u);
		parameters->Set("DLSSNR.OutputSubrectWidth", outputWidth);
		parameters->Set("DLSSNR.OutputSubrectHeight", outputHeight);
		if (controlMask) {
			parameters->Set("DLSSNR.ControlMask", controlMask);
			parameters->Set("DLSSNR.ControlMaskSubrectBaseX", 0u);
			parameters->Set("DLSSNR.ControlMaskSubrectBaseY", 0u);
			parameters->Set("DLSSNR.ControlMaskSubrectWidth", inputWidth);
			parameters->Set("DLSSNR.ControlMaskSubrectHeight", inputHeight);
		}
		parameters->Set("DLSSNR.MVecScaleX", motionVectorScaleX);
		parameters->Set("DLSSNR.MVecScaleY", motionVectorScaleY);
		parameters->Set("DLSSNR.DepthInverted", 0u);
		parameters->Set("DLSSNR.Enabled", 1u);
		parameters->Set("DLSSNR.Reset", reset ? 1u : 0u);
		if (modelScale > 0.0f)
			parameters->Set("DLSSNR.ScalingRatio", scalingRatio);
		parameters->Set("DLSSNR.Intensity", tuning.intensity);
		parameters->Set("DLSSNR.LocalToneStrength", tuning.localToneStrength);
		parameters->Set("DLSSNR.LocalStructureStrength", tuning.localStructureStrength);
		parameters->Set("DLSSNR.SkinStructureStrength", tuning.skinStructureStrength);
		parameters->Set("DLSSNR.UseAutoMask", tuning.useAutoMask ? 1u : 0u);
		parameters->Set("DLSSNR.Style", tuning.style);
		parameters->Set("DLSSNR.UICorrection", tuning.uiCorrection ? 1u : 0u);
		ngxResult_ = static_cast<std::uint32_t>(evaluate(commandList,
			static_cast<NVSDK_NGX_Handle*>(featureHandles_[slot]), parameters, nullptr));
		if (ngxResult_ != NVSDK_NGX_Result_Success) {
			detail_ = std::format("Feature 18 evaluate failed slot={} result=0x{:08X}{}", slot, ngxResult_, NgxResultHint(ngxResult_));
			return false;
		}
		++successfulFrames_;
		return true;
	}

	void Runtime::ProbeScalingCallback(std::uint32_t width, std::uint32_t height)
	{
		// (batch 42a) Debug probe, logged once per session at the first feature creation. The DLL has a
		// DLSSNRComputeScalingRatioCallback and reads PerfQualityValue beside it, which suggests the
		// official "model resolution" is a quality mode answered by that callback rather than a ratio
		// the caller sets. Look in the capability parameters (where NGX publishes such callbacks) and in
		// our own block; when found, ask it for every quality mode 0..5 on a scratch block and log what
		// comes back. Nothing here changes what is created.
		HMODULE core = FindNgxCoreModule();
		if (!core) {
			logger::info("[DLSSNR] scaling probe: NGX parameter module not found");
			return;
		}
		auto getCaps = reinterpret_cast<GetCapabilityParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_GetCapabilityParameters"));
		auto allocate = reinterpret_cast<AllocateParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_AllocateParameters"));
		auto destroy = reinterpret_cast<DestroyParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
		NVSDK_NGX_Parameter* caps = nullptr;
		NVSDK_NGX_Result capsResult = NVSDK_NGX_Result_Fail;
		if (getCaps)
			capsResult = getCaps(&caps);

		void* callback = nullptr;
		const char* foundIn = "nowhere";
		auto lookIn = [&](NVSDK_NGX_Parameter* block, const char* name) {
			if (!block || callback)
				return;
			void* pointer = nullptr;
			if (NVSDK_NGX_SUCCEED(block->Get("DLSSNRComputeScalingRatioCallback", &pointer)) && pointer) {
				callback = pointer;
				foundIn = name;
			}
			int perfQuality = -1;
			unsigned int perfQualityU = 0xFFFFFFFFu;
			const bool haveInt = NVSDK_NGX_SUCCEED(block->Get("PerfQualityValue", &perfQuality));
			const bool haveUint = NVSDK_NGX_SUCCEED(block->Get("PerfQualityValue", &perfQualityU));
			logger::info("[DLSSNR] scaling probe: {} block: callback {}, PerfQualityValue {}", name,
				pointer ? "present" : "absent",
				haveInt ? std::format("{} (int)", perfQuality) : haveUint ? std::format("{} (uint)", perfQualityU) : std::string("not set"));
		};
		lookIn(caps, "capability");
		lookIn(static_cast<NVSDK_NGX_Parameter*>(parameters_), "feature");
		if (!getCaps || !NVSDK_NGX_SUCCEED(capsResult))
			logger::info("[DLSSNR] scaling probe: capability parameters unavailable (export {}, result 0x{:08X})",
				getCaps ? "present" : "absent", static_cast<std::uint32_t>(capsResult));

		if (callback && allocate && destroy) {
			NVSDK_NGX_Parameter* scratch = nullptr;
			if (NVSDK_NGX_SUCCEED(allocate(&scratch)) && scratch) {
				for (int quality = 0; quality <= 5; ++quality) {
					scratch->Reset();
					scratch->Set("Width", width);
					scratch->Set("Height", height);
					scratch->Set("DLSSNR.Width", width);
					scratch->Set("DLSSNR.Height", height);
					scratch->Set("PerfQualityValue", quality);
					bool faulted = false;
					const auto result = CallScalingCallbackGuarded(reinterpret_cast<ScalingRatioCallback>(callback), scratch, &faulted);
					float ratio = -1.0f;
					unsigned int outWidth = 0, outHeight = 0, renderWidth = 0, renderHeight = 0;
					scratch->Get("DLSSNR.ScalingRatio", &ratio);
					scratch->Get("OutWidth", &outWidth);
					scratch->Get("OutHeight", &outHeight);
					scratch->Get("DLSSNR.Width", &renderWidth);
					scratch->Get("DLSSNR.Height", &renderHeight);
					logger::info("[DLSSNR] scaling probe: PerfQualityValue {} at {}x{} -> result 0x{:08X}{} ScalingRatio {:.4f} OutWidth/Height {}x{} DLSSNR.Width/Height {}x{}",
						quality, width, height, static_cast<std::uint32_t>(result), faulted ? " (faulted)" : "", ratio,
						outWidth, outHeight, renderWidth, renderHeight);
					if (faulted)
						break;
				}
				destroy(scratch);
			}
		} else {
			logger::info("[DLSSNR] scaling probe: no DLSSNRComputeScalingRatioCallback published ({}); model resolution stays our own downscale",
				foundIn);
		}
		if (caps && destroy)
			destroy(caps);
	}

	void Runtime::ResetFeature(std::uint32_t slot)
	{
		if (!module_ || slot >= 2)
			return;
		SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
		auto release = reinterpret_cast<ReleaseFeature>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_ReleaseFeature"));
		if (featureHandles_[slot] && release) {
			const auto result = release(static_cast<NVSDK_NGX_Handle*>(featureHandles_[slot]));
			if (result != NVSDK_NGX_Result_Success)
				logger::warn("[DLSSNR] Feature 18 release failed slot={} result=0x{:08X} proxyInstalled={}",
					slot, static_cast<std::uint32_t>(result), scope.IsInstalled());
		}
		featureHandles_[slot] = nullptr;
		featureInputWidth_[slot] = featureInputHeight_[slot] = 0;
		featureOutputWidth_[slot] = featureOutputHeight_[slot] = 0;
		featureScalingRatio_[slot] = 0.0f;
		featureTuning_[slot] = {};
		featureTuningAtCreate_[slot] = false;
		featureControlMask_[slot] = false;
	}

	void Runtime::ResetFeatures()
	{
		for (std::uint32_t slot = 0; slot < 2; ++slot)
			ResetFeature(slot);
		successfulFrames_ = 0;
	}

	void Runtime::Shutdown(bool unloadModule)
	{
		if (device_ && module_) {
			ResetFeatures();
			HMODULE core = FindNgxCoreModule();
			if (parameters_ && core) {
				auto destroy = reinterpret_cast<DestroyParameters>(GetProcAddress(core, "NVSDK_NGX_D3D12_DestroyParameters"));
				if (destroy) destroy(static_cast<NVSDK_NGX_Parameter*>(parameters_));
			}
			parameters_ = nullptr;
			auto shutdown = reinterpret_cast<ShutdownD3D12>(GetProcAddress(static_cast<HMODULE>(module_), "NVSDK_NGX_D3D12_Shutdown1"));
			SignedRuntimePathScope scope(static_cast<HMODULE>(module_), path_.parent_path() / L"nvngx.dll");
			if (shutdown) {
				const auto result = shutdown(device_);
				if (result != NVSDK_NGX_Result_Success)
					logger::warn("[DLSSNR] NGX shutdown failed result=0x{:08X} proxyInstalled={}",
						static_cast<std::uint32_t>(result), scope.IsInstalled());
			}
			device_->Release();
			device_ = nullptr;
		}
		// (batch 38a) Switching the feature off releases everything NGX made on our device, and the device
		// itself, but keeps the DLL mapped: it holds no video memory, and unloading a 158 MB runtime
		// that talks to the driver, only to map it again on the next switch-on, is the riskier half of a
		// toggle. NGX is built to be initialised again after Shutdown1.
		if (!unloadModule && module_) {
			status_ = RuntimeStatus::Ready;
			detail_.clear();
			ngxResult_ = 0;
			successfulFrames_ = 0;
			return;
		}
		if (module_) FreeLibrary(static_cast<HMODULE>(module_));
		module_ = nullptr;
		status_ = RuntimeStatus::NotProbed;
		path_.clear();
		version_.clear();
		detail_.clear();
		ngxResult_ = applicationId_ = apiVersion_ = 0;
		successfulFrames_ = 0;
	}

	const char* ToString(RuntimeStatus status)
	{
		switch (status) {
		case RuntimeStatus::NotProbed: return "not-probed";
		case RuntimeStatus::NotFound: return "not-found";
		case RuntimeStatus::VersionUnavailable: return "version-unavailable";
		case RuntimeStatus::UnsupportedVersion: return "unsupported-version";
		case RuntimeStatus::LoadFailed: return "load-failed";
		case RuntimeStatus::MissingExport: return "missing-export";
		case RuntimeStatus::Ready: return "ready";
		case RuntimeStatus::InitializationFailed: return "initialization-failed";
		case RuntimeStatus::CoreUnavailable: return "core-unavailable";
		case RuntimeStatus::ParameterAllocationFailed: return "parameter-allocation-failed";
		case RuntimeStatus::Initialized: return "initialized";
		}
		return "unknown";
	}
}