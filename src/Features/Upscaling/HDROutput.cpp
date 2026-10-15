#include "HDROutput.h"

#include <dxgi1_6.h>

#include "Buffer.h"
#include "Features/LinearLighting.h"
#include "Features/Upscaling.h"
#include "State.h"
#include "Util.h"
#include "Features/HDROutputFeature.h"
#include "Utils/DisplayHDRInfo.h"
#include "Utils/FileSystem.h"
#include "Utils/GpuTimers.h"

namespace HDROutput
{
	namespace
	{
		constexpr float kAutoPeakMin = 400.0f;
		constexpr float kAutoPeakMax = 1000.0f;
		constexpr float kPeakFallback = 450.0f;
		constexpr float kManualPeakMin = 200.0f;
		constexpr float kManualPeakMax = 2000.0f;
		constexpr float kPaperWhiteMin = 80.0f;
		constexpr float kPaperWhiteMax = 400.0f;

		struct alignas(16) OutputCB
		{
			float paperWhiteNits;
			float peakRel;
			float hudScale;
			float sceneIsSDR;
			uint compositeMode;
			uint useOverlay;
			uint sceneBT2020;  // (44a) the scene is already BT.2020 (Color Grading HDR output)
			uint pad1;
		};
		static_assert(sizeof(OutputCB) % 16 == 0);

		enum class Path : uint8_t
		{
			kNone,            // nothing decided yet this frame
			kSeparated,       // HDR scene + the UI on its own layer, composited at Present
			kWholeFrameSDR,   // everything in the back buffer as SDR, shown at paper white
		};

		const char* PathName(Path a_path)
		{
			switch (a_path) {
			case Path::kSeparated:
				return "HDR scene + separate HUD layer";
			case Path::kWholeFrameSDR:
				return "whole frame as SDR (menu / loading / no scene)";
			default:
				return "-";
			}
		}

		struct FrameState
		{
			bool redirected = false;   // the frame-buffer slot pointed at hdrScene for the image-space chain
			bool sceneFresh = false;   // ... and the chain's final target was confirmed to be hdrScene
			bool sceneHDR = false;     // Color Grading was asked for HDR highlights this frame
			bool uiStart = false;      // the UI-start hook ran
			bool resolved = false;     // the scene was put into the back buffer (SDR or PQ)
			bool uiRedirected = false; // the frame-buffer RTV points at the UI layer (separated path)
			Path path = Path::kNone;
		};

		struct SavedRenderTarget
		{
			RE::RENDER_TARGET target;
			ID3D11Texture2D* texture;
			ID3D11RenderTargetView* rtv;
			ID3D11ShaderResourceView* srv;
			ID3D11UnorderedAccessView* uav;
		};

		// session
		bool g_session = false;
		DXGI_FORMAT g_chainFormat = DXGI_FORMAT_UNKNOWN;

		// colour space actually applied to the swap chain
		bool g_outputActive = false;
		bool g_colorSpaceFailed = false;  // the last attempt for the wanted state failed; retried when it changes
		bool g_lastWanted = false;
		HRESULT g_lastColorSpaceResult = S_OK;
		std::string g_colorSpaceWhere = "not set";

		// resources
		std::unique_ptr<Texture2D> g_scene;
		// (43b) The game's UI, third-party UI drawn through the game, and ImGui, during gameplay. 8-bit
		// like upstream's: the UI is SDR, and 8 bits round near-black fringes to zero.
		std::unique_ptr<Texture2D> g_uiLayer;
		std::unique_ptr<ConstantBuffer> g_cb;
		winrt::com_ptr<ID3D11ComputeShader> g_encodeCS;
		winrt::com_ptr<ID3D11ComputeShader> g_resolveCS;
		winrt::com_ptr<ID3D11ComputeShader> g_compositeCS;
		std::vector<SavedRenderTarget> g_upgradedTargets;

		// frame
		FrameState g_frame;
		Path g_lastPath = Path::kNone;
		ID3D11Texture2D* g_savedFramebufferTexture = nullptr;
		ID3D11ShaderResourceView* g_savedFramebufferSRV = nullptr;
		ID3D11RenderTargetView* g_savedFramebufferRTV = nullptr;
		ID3D11RenderTargetView* g_savedUIFramebufferRTV = nullptr;  // (43b) while the UI layer is the target
		ID3D11RenderTargetView* g_backBufferRTV = nullptr;  // (43b) the slot's own RTV, last seen
		D3D11_VIEWPORT g_savedImGuiViewport{};
		bool g_imGuiRedirected = false;

		// (43c) DLSS-G
		uint32_t g_lastDLSSGStatus = 0;
		bool g_dlssGRejectedHDR = false;

		// (44a) settings, migration, display format
		Settings g_settings;
		nlohmann::json g_legacyJson;  // 43b Upscaling.hdrOutput, until taken
		bool g_legacySettingsPending = false;
		bool g_legacyCurvePending = false;
		bool g_tenBitWanted = false;          // what WantsTenBitSwapChain decided at startup
		std::string g_tenBitDecision = "not decided";
		std::string g_tenBitFallbackReason;  // non-empty: the on-disk fallback marker's reason
		bool g_tenBitFallbackLoaded = false;
		bool g_sceneBT2020 = false;          // this frame: Color Grading wrote the scene in BT.2020
		bool g_lastSceneBT2020 = false;      // the previous frame's value (status / F12)

		// diagnostics
		uint64_t g_redirectConfirmed = 0;
		uint64_t g_redirectUnconfirmed = 0;
		bool g_loggedConfirmed = false;
		bool g_loggedUnconfirmed = false;
		float g_lastPeakNits = kPeakFallback;
		const char* g_lastPeakSource = "fallback";

		// (44d) chain-end recovery and copy-format diagnostics
		uint64_t g_frameCounter = 0;
		uint64_t g_chainRecovered = 0;      // frames whose scene was taken from the fp16 target although another target was bound last
		bool g_lastChainEndOk = true;       // the previous redirected frame ended on the scene (episode tracking for the log)
		uint32_t g_chainLogLines = 0;
		std::string g_lastChainEndNote;     // the last "ended on another target" situation, for the status page
		constexpr uint32_t kMaxChainLogLines = 40;

		DX12SwapChain& SwapChain()
		{
			return Upscaling::dx12SwapChain;
		}

		bool IsMainOrLoadingMenu()
		{
			auto* ui = globals::game::ui;
			return ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
		}

		float ResolvePeakNits()
		{
			const auto& s = GetSettings();
			if (s.peakMode == 1) {
				g_lastPeakSource = "manual";
				return std::clamp(s.peakNitsManual, kManualPeakMin, kManualPeakMax);
			}
			const auto display = DisplayHDRInfo::GetDisplayState();
			if (display.valid && display.maxLuminance >= 80.0f) {
				g_lastPeakSource = "display";
				return std::clamp(display.maxLuminance, kAutoPeakMin, kAutoPeakMax);
			}
			g_lastPeakSource = "fallback";
			return kPeakFallback;
		}

		struct Levels
		{
			float peakNits;
			float paperWhiteNits;
			float peakRel;
			float hudNits;
		};

		Levels ResolveLevels()
		{
			const auto& s = GetSettings();
			Levels l{};
			l.peakNits = ResolvePeakNits();
			g_lastPeakNits = l.peakNits;
			l.paperWhiteNits = std::clamp(s.paperWhiteNits, kPaperWhiteMin, std::min(kPaperWhiteMax, l.peakNits * 0.9f));
			l.peakRel = l.peakNits / l.paperWhiteNits;
			l.hudNits = std::clamp(s.hudNits, 40.0f, l.peakNits);
			return l;
		}

		ID3D11ComputeShader* GetShader(winrt::com_ptr<ID3D11ComputeShader>& a_shader, const char* a_define, const char* a_entry)
		{
			if (!a_shader) {
				if (auto raw = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\HDROutput\\HDROutputCS.hlsl", { { a_define, "" } }, "cs_5_0", a_entry)))
					a_shader.attach(raw);
				else
					logger::error("[HDR] Failed to compile HDROutputCS.hlsl ({})", a_entry);
			}
			return a_shader.get();
		}

		bool EnsureCB()
		{
			if (!g_cb)
				g_cb = std::make_unique<ConstantBuffer>(ConstantBufferDesc<OutputCB>());
			return g_cb != nullptr;
		}

		/// One full-screen dispatch: up to three SRVs, one UAV, the output CB.
		void Dispatch(ID3D11ComputeShader* a_shader, std::initializer_list<ID3D11ShaderResourceView*> a_srvs, ID3D11UnorderedAccessView* a_uav,
			uint32_t a_width, uint32_t a_height, const OutputCB& a_data)
		{
			auto* context = globals::d3d::context;
			g_cb->Update(a_data);

			// Nothing we read may still be bound as a render target (D3D11 would null the SRV).
			context->OMSetRenderTargets(0, nullptr, nullptr);

			std::array<ID3D11ShaderResourceView*, 3> srvs{};
			size_t i = 0;
			for (auto* srv : a_srvs)
				if (i < srvs.size())
					srvs[i++] = srv;
			ID3D11Buffer* cb = g_cb->CB();
			context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
			context->CSSetUnorderedAccessViews(0, 1, &a_uav, nullptr);
			context->CSSetConstantBuffers(0, 1, &cb);
			context->CSSetShader(a_shader, nullptr, 0);
			context->Dispatch((a_width + 7) / 8, (a_height + 7) / 8, 1);

			srvs.fill(nullptr);
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetShaderResources(0, static_cast<UINT>(srvs.size()), srvs.data());
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			context->CSSetShader(nullptr, nullptr, 0);
		}

		OutputCB MakeCB(const Levels& a_levels, bool a_sceneIsSDR, uint a_compositeMode = 0, bool a_useOverlay = false, bool a_sceneBT2020 = false)
		{
			OutputCB data{};
			data.paperWhiteNits = a_levels.paperWhiteNits;
			data.peakRel = a_levels.peakRel;
			data.hudScale = a_levels.hudNits / a_levels.paperWhiteNits;
			data.sceneIsSDR = a_sceneIsSDR ? 1.0f : 0.0f;
			data.compositeMode = a_compositeMode;
			data.useOverlay = a_useOverlay ? 1u : 0u;
			data.sceneBT2020 = a_sceneBT2020 ? 1u : 0u;
			return data;
		}

		/// (44a) This frame's scene was graded to BT.2020 (only ever for an HDR scene).
		bool SceneIsBT2020()
		{
			return g_sceneBT2020 && g_frame.sceneHDR;
		}

		/// The scene as plain SDR into the back buffer (menus, HDR off in an HDR session).
		void ResolveSceneToBackBuffer()
		{
			auto& sc = SwapChain();
			auto* backBuffer = sc.swapChainBufferWrapped.get();
			auto* shader = GetShader(g_resolveCS, "RESOLVE_SDR", "CSResolveSDR");
			if (!g_scene || !backBuffer || !backBuffer->uav || !shader || !EnsureCB())
				return;
			Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::HDROutput);
			// (44a) A BT.2020 scene that ends up shown as SDR goes back to BT.709 first.
			Dispatch(shader, { g_scene->srv.get() }, backBuffer->uav, sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), true, 0, false, SceneIsBT2020()));
			Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::HDROutput);
			g_frame.resolved = true;
		}

		/// The HDR scene, PQ-encoded, into a 10-bit UAV target.
		bool EncodeSceneTo(ID3D11UnorderedAccessView* a_target, bool a_sceneIsSDR)
		{
			auto& sc = SwapChain();
			auto* shader = GetShader(g_encodeCS, "ENCODE_SCENE", "CSEncodeScene");
			if (!g_scene || !a_target || !shader || !EnsureCB())
				return false;
			Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::HDROutput);
			Dispatch(shader, { g_scene->srv.get() }, a_target, sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), a_sceneIsSDR, 0, false, SceneIsBT2020()));
			Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::HDROutput);
			return true;
		}

		void RebindFramebuffer()
		{
			auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
			globals::d3d::context->OMSetRenderTargets(1, &fb.RTV, nullptr);
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}

		/// Whether this frame's scene should be produced as HDR (otherwise SDR, shown whole at paper white).
		bool WantsSceneHDR()
		{
			if (!g_outputActive || IsMainOrLoadingMenu())
				return false;
			return true;
		}

		bool WantsOutput()
		{
			const auto& s = GetSettings();
			if (!g_session || !s.enabled)
				return false;
			// Windows HDR off (known): nothing to show HDR10 on. Unknown (not queried yet): try, the
			// colour-space support check decides.
			const auto display = DisplayHDRInfo::GetDisplayState();
			return !display.valid || display.windowsHDR;
		}

		HRESULT SetColorSpaceOn(IDXGISwapChain4* a_chain, const char* a_name, bool a_hdr10)
		{
			if (!a_chain)
				return E_POINTER;
			const auto colorSpace = a_hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
			UINT support = 0;
			const HRESULT check = a_chain->CheckColorSpaceSupport(colorSpace, &support);
			if (FAILED(check) || !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)) {
				logger::warn("[HDR] {} swap chain: {} colour space not supported (check 0x{:08X}, flags 0x{:X})",
					a_name, a_hdr10 ? "HDR10 (G2084/P2020)" : "sRGB (G22/P709)", static_cast<uint32_t>(check), support);
				return FAILED(check) ? check : DXGI_ERROR_UNSUPPORTED;
			}
			const HRESULT result = a_chain->SetColorSpace1(colorSpace);
			logger::info("[HDR] SetColorSpace1({}) on the {} swap chain: 0x{:08X}{}",
				a_hdr10 ? "HDR10 G2084/P2020" : "sRGB G22/P709", a_name, static_cast<uint32_t>(result), SUCCEEDED(result) ? " (ok)" : " (FAILED)");
			if (SUCCEEDED(result)) {
				// No static HDR10 metadata, as upstream: the game does its own roll-off, and some
				// displays tone-map a second time when it is sent.
				const HRESULT meta = a_chain->SetHDRMetaData(DXGI_HDR_METADATA_TYPE_NONE, 0, nullptr);
				if (FAILED(meta))
					logger::warn("[HDR] SetHDRMetaData(NONE) on the {} swap chain: 0x{:08X}", a_name, static_cast<uint32_t>(meta));
			}
			return result;
		}

		bool ApplyColorSpace(bool a_hdr10)
		{
			auto& sc = SwapChain();
			IDXGISwapChain4* proxy = sc.swapChain;
			IDXGISwapChain4* native = sc.GetNativeSwapChain();
			const bool distinct = native && native != proxy;
			// (44g) Always the Streamline proxy, the native chain only if the proxy refuses.
			HRESULT result = SetColorSpaceOn(proxy, distinct ? "Streamline proxy" : "D3D12", a_hdr10);
			std::string where = distinct ? "Streamline proxy chain" : "swap chain";
			if (FAILED(result) && distinct) {
				logger::warn("[HDR] The Streamline proxy refused the colour space; trying the native chain");
				result = SetColorSpaceOn(native, "native (below Streamline)", a_hdr10);
				where = "native chain (proxy refused)";
			}
			g_lastColorSpaceResult = result;
			g_colorSpaceWhere = std::format("{} -> {}", a_hdr10 ? "HDR10" : "sRGB", where);
			return SUCCEEDED(result);
		}

		void UpgradeLDRRenderTargets()
		{
			// Ported from upstream HDRDisplay::UpgradeLDRRenderTargets: the image-space targets after
			// tone mapping are 8-bit; with the frame-buffer slot fp16 they must be too, or a pass that
			// round-trips through them clips everything above SDR white.
			static constexpr RE::RENDER_TARGET kTargets[] = {
				RE::RENDER_TARGET::kLDR_DOWNSAMPLE0,
				RE::RENDER_TARGET::kLDR_BLURSWAP,
				RE::RENDER_TARGET::kBLURFULL_BUFFER,
				RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY,
				RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY2,
				RE::RENDER_TARGET::kTEMPORAL_AA_UI_ACCUMULATION_1,
				RE::RENDER_TARGET::kTEMPORAL_AA_UI_ACCUMULATION_2,
			};
			if (!g_upgradedTargets.empty())
				return;

			auto* device = globals::d3d::device;
			auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			for (auto id : kTargets) {
				auto& rt = targets[id];
				if (!rt.texture)
					continue;
				D3D11_TEXTURE2D_DESC desc{};
				rt.texture->GetDesc(&desc);
				if (desc.Format == DXGI_FORMAT_R16G16B16A16_FLOAT)
					continue;
				const auto oldFormat = desc.Format;
				desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

				ID3D11Texture2D* texture = nullptr;
				if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture)))
					continue;
				ID3D11RenderTargetView* rtv = nullptr;
				ID3D11ShaderResourceView* srv = nullptr;
				ID3D11UnorderedAccessView* uav = nullptr;
				D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
				rtvDesc.Format = desc.Format;
				rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = desc.Format;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
				srvDesc.Texture2D.MipLevels = 1;
				bool ok = (desc.BindFlags & D3D11_BIND_RENDER_TARGET) == 0 || SUCCEEDED(device->CreateRenderTargetView(texture, &rtvDesc, &rtv));
				ok = ok && ((desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || SUCCEEDED(device->CreateShaderResourceView(texture, &srvDesc, &srv)));
				if (ok && rt.UAV) {
					D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
					rt.UAV->GetDesc(&uavDesc);
					uavDesc.Format = desc.Format;
					ok = SUCCEEDED(device->CreateUnorderedAccessView(texture, &uavDesc, &uav));
				}
				if (!ok) {
					if (uav)
						uav->Release();
					if (srv)
						srv->Release();
					if (rtv)
						rtv->Release();
					texture->Release();
					continue;
				}

				g_upgradedTargets.push_back({ id, rt.texture, rt.RTV, rt.SRV, rt.UAV });
				rt.texture = texture;
				rt.RTV = rtv;
				rt.SRV = srv;
				rt.UAV = uav;
				logger::info("[HDR] Upgraded render target {} to R16G16B16A16_FLOAT (was format {})", static_cast<int>(id), static_cast<int>(oldFormat));
			}
		}

		void CreateUILayer(uint32_t a_width, uint32_t a_height)
		{
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
			g_uiLayer = std::make_unique<Texture2D>(desc);
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = desc.Format;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
			srvDesc.Texture2D.MipLevels = 1;
			g_uiLayer->CreateSRV(srvDesc);
			D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
			rtvDesc.Format = desc.Format;
			rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
			g_uiLayer->CreateRTV(rtvDesc);
		}

		/// (43b) The frame-buffer slot's RTV back to the back buffer, if the UI layer had it.
		void RestoreUIRedirect()
		{
			auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
			const bool slotOnUILayer = g_uiLayer && fb.RTV == g_uiLayer->rtv.get();
			if (!g_frame.uiRedirected && !slotOnUILayer)
				return;
			if (auto* original = g_savedUIFramebufferRTV ? g_savedUIFramebufferRTV : g_backBufferRTV)
				fb.RTV = original;
			g_savedUIFramebufferRTV = nullptr;
			g_frame.uiRedirected = false;
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}

		// ---- (44d) which target is this, chain end, format-mismatched copies ----------------------------

		constexpr int kTargetUnknown = -1;
		constexpr int kTargetScene = -2;
		constexpr int kTargetBackBuffer = -3;
		constexpr int kTargetUILayer = -4;
		constexpr int kTargetOldTextureBase = 1000;  // + id: the original 8-bit texture of an upgraded target

		/// Which render target (RE::RENDER_TARGET index) or texture of ours a resource is. Only used for
		/// log lines and rare checks: a linear scan of the engine's table.
		int IdentifyTexture(ID3D11Resource* a_resource)
		{
			if (!a_resource)
				return kTargetUnknown;
			if (g_scene && a_resource == g_scene->resource.get())
				return kTargetScene;
			if (g_uiLayer && a_resource == g_uiLayer->resource.get())
				return kTargetUILayer;
			if (auto* backBuffer = SwapChain().swapChainBufferWrapped.get(); backBuffer && a_resource == backBuffer->resource11)
				return kTargetBackBuffer;
			const auto& targets = globals::game::renderer->GetRuntimeData().renderTargets;
			for (int i = 0; i < static_cast<int>(std::size(targets)); ++i) {
				if (targets[i].texture && targets[i].texture == a_resource)
					return i;
			}
			for (const auto& saved : g_upgradedTargets) {
				if (saved.texture == a_resource)
					return kTargetOldTextureBase + static_cast<int>(saved.target);
			}
			return kTargetUnknown;
		}

		std::string TargetName(int a_id)
		{
			switch (a_id) {
			case kTargetUnknown:
				return "an untracked texture";
			case kTargetScene:
				return "the fp16 HDR scene";
			case kTargetBackBuffer:
				return "the back buffer";
			case kTargetUILayer:
				return "the HDR UI layer";
			default:
				break;
			}
			if (a_id >= kTargetOldTextureBase)
				return std::format("the old 8-bit texture of render target {}", a_id - kTargetOldTextureBase);
			const char* name = nullptr;
			switch (static_cast<RE::RENDER_TARGET>(a_id)) {
			case RE::RENDER_TARGET::kFRAMEBUFFER:
				name = "FRAMEBUFFER";
				break;
			case RE::RENDER_TARGET::kMAIN:
				name = "MAIN";
				break;
			case RE::RENDER_TARGET::kMAIN_COPY:
				name = "MAIN_COPY";
				break;
			case RE::RENDER_TARGET::kUNDERWATER_MASK:
				name = "UNDERWATER_MASK";
				break;
			case RE::RENDER_TARGET::kMENUBG:
				name = "MENUBG";
				break;
			case RE::RENDER_TARGET::kBLURFULL_BUFFER:
				name = "BLURFULL_BUFFER";
				break;
			case RE::RENDER_TARGET::kLDR_BLURSWAP:
				name = "LDR_BLURSWAP";
				break;
			case RE::RENDER_TARGET::kLDR_DOWNSAMPLE0:
				name = "LDR_DOWNSAMPLE0";
				break;
			case RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY:
				name = "IMAGESPACE_TEMP_COPY";
				break;
			case RE::RENDER_TARGET::kIMAGESPACE_TEMP_COPY2:
				name = "IMAGESPACE_TEMP_COPY2";
				break;
			case RE::RENDER_TARGET::kSAVE_GAME_SCREENSHOT:
				name = "SAVE_GAME_SCREENSHOT";
				break;
			case RE::RENDER_TARGET::kSCREENSHOT:
				name = "SCREENSHOT";
				break;
			case RE::RENDER_TARGET::kTEMPORAL_AA_UI_ACCUMULATION_1:
				name = "TEMPORAL_AA_UI_ACCUMULATION_1";
				break;
			case RE::RENDER_TARGET::kTEMPORAL_AA_UI_ACCUMULATION_2:
				name = "TEMPORAL_AA_UI_ACCUMULATION_2";
				break;
			case RE::RENDER_TARGET::kWATER_1:
				name = "WATER_1";
				break;
			case RE::RENDER_TARGET::kWATER_2:
				name = "WATER_2";
				break;
			case RE::RENDER_TARGET::kRAW_WATER:
				name = "RAW_WATER";
				break;
			default:
				break;
			}
			return name ? std::format("render target {} ({})", a_id, name) : std::format("render target {}", a_id);
		}
	}

	// ---- settings, migration ---------------------------------------------------------------------

	Settings& GetSettings()
	{
		return g_settings;
	}

	void StashLegacyUpscalingJson(const nlohmann::json& a_upscalingJson)
	{
		if (!a_upscalingJson.is_object() || !a_upscalingJson.contains("hdrOutput") || !a_upscalingJson["hdrOutput"].is_object())
			return;
		g_legacyJson = a_upscalingJson["hdrOutput"];
		g_legacySettingsPending = true;
		g_legacyCurvePending = true;
	}

	bool TakeLegacySettings(Settings& o_settings)
	{
		if (!g_legacySettingsPending)
			return false;
		g_legacySettingsPending = false;
		try {
			o_settings = g_legacyJson;  // unknown keys (toneCurve, rolloffStart) are ignored here
			logger::info("[HDR] Settings migrated from Upscaling > HDR Output (43b)");
			return true;
		} catch (const std::exception& e) {
			logger::warn("[HDR] Could not migrate the 43b HDR settings: {}", e.what());
			return false;
		}
	}

	bool TakeLegacyCurve(uint& o_toneCurve, float& o_rolloffStart)
	{
		if (!g_legacyCurvePending)
			return false;
		g_legacyCurvePending = false;
		o_toneCurve = g_legacyJson.value("toneCurve", 1u);
		o_rolloffStart = g_legacyJson.value("rolloffStart", 0.6f);
		return true;
	}

	void ClearLegacy()
	{
		// The curve part stays until Color Grading has loaded (it may load later than this).
		g_legacySettingsPending = false;
	}

	namespace
	{
		std::filesystem::path FallbackMarkerPath()
		{
			return Util::PathHelpers::GetSettingsUserPath().parent_path() / "HDROutputFormatFallback.txt";
		}

		void LoadFallbackMarker()
		{
			if (g_tenBitFallbackLoaded)
				return;
			g_tenBitFallbackLoaded = true;
			std::error_code ec;
			const auto path = FallbackMarkerPath();
			if (!std::filesystem::exists(path, ec))
				return;
			std::ifstream file(path);
			std::getline(file, g_tenBitFallbackReason);
			if (g_tenBitFallbackReason.empty())
				g_tenBitFallbackReason = "unknown";
		}

		void ClearFallbackMarker()
		{
			std::error_code ec;
			std::filesystem::remove(FallbackMarkerPath(), ec);
			g_tenBitFallbackReason.clear();
		}
	}

	void NoteTenBitFailure(const char* a_reason)
	{
		LoadFallbackMarker();
		if (!g_tenBitFallbackReason.empty())
			return;
		g_tenBitFallbackReason = a_reason ? a_reason : "unknown";
		logger::warn("[HDR] 10-bit display format failed ({}); the next start uses 8-bit (HDR Output > Retry 10-bit to undo)", g_tenBitFallbackReason);
		std::error_code ec;
		std::filesystem::create_directories(FallbackMarkerPath().parent_path(), ec);
		std::ofstream file(FallbackMarkerPath(), std::ios::trunc);
		file << g_tenBitFallbackReason << "\n";
	}

	// ---- startup ---------------------------------------------------------------------------------------

	bool WantsTenBitSwapChain(HWND a_window)
	{
		const auto& s = GetSettings();
		g_tenBitWanted = false;
		if (globals::game::isVR) {
			g_tenBitDecision = "8-bit: VR";
			return false;
		}
		if (!globals::features::hdrOutput.loaded) {
			g_tenBitDecision = "8-bit: the HDR Output feature is not loaded";
			return false;
		}
		if (s.displayFormat == 1) {
			g_tenBitWanted = s.enabled;
			g_tenBitDecision = s.enabled ? "10-bit: Display format 8-bit (legacy) with HDR saved on" : "8-bit: Display format 8-bit (legacy), HDR off";
			return g_tenBitWanted;
		}
		LoadFallbackMarker();
		if (!g_tenBitFallbackReason.empty()) {
			g_tenBitDecision = std::format("8-bit: automatic fallback after a failure ({})", g_tenBitFallbackReason);
			return false;
		}
		const auto cap = DisplayHDRInfo::QueryCapability(a_window);
		logger::info("[HDR] Display format Auto: {}", cap.known ? cap.detail : ("not known: " + cap.detail));
		if (!cap.known && s.enabled) {
			// Could not read the display: trust the saved HDR choice.
			g_tenBitWanted = true;
			g_tenBitDecision = "10-bit: Auto, display not readable, HDR saved on";
		} else {
			g_tenBitWanted = cap.known && cap.hdrCapable;
			g_tenBitDecision = g_tenBitWanted ? "10-bit: Auto, the display is HDR-capable" : "8-bit: Auto, the display does not report HDR";
		}
		return g_tenBitWanted;
	}

	void OnSwapChainCreated()
	{
		auto& sc = SwapChain();
		g_chainFormat = sc.swapChainDesc.Format;
		g_session = g_chainFormat == DXGI_FORMAT_R10G10B10A2_UNORM;
		if (g_tenBitWanted && !g_session)
			NoteTenBitFailure("the 10-bit swap chain could not be created");
		logger::info("[HDR] Swap chain format {} -> 10-bit session {} ({})", static_cast<uint32_t>(g_chainFormat),
			g_session ? "yes, HDR switches live" : "no", g_tenBitDecision);
		if (g_session && WantsOutput()) {
			g_lastWanted = true;
			g_outputActive = ApplyColorSpace(true);
			g_colorSpaceFailed = !g_outputActive;
		}
	}

	bool IsSession()
	{
		return g_session;
	}

	void SetupResources()
	{
		if (!g_session)
			return;

		auto& sc = SwapChain();
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = sc.swapChainDesc.Width;
		desc.Height = sc.swapChainDesc.Height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS;
		g_scene = std::make_unique<Texture2D>(desc);
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = desc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = 1;
		g_scene->CreateSRV(srvDesc);
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc{};
		rtvDesc.Format = desc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		g_scene->CreateRTV(rtvDesc);
		D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = desc.Format;
		uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
		g_scene->CreateUAV(uavDesc);

		CreateUILayer(desc.Width, desc.Height);
		EnsureCB();
		UpgradeLDRRenderTargets();

		// (port of ba9b41e81 / f748118bb) New targets start black, not with whatever the memory held:
		// the first frames (startup, main menu) can be shown before anything has drawn into them.
		auto* context = globals::d3d::context;
		const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		context->ClearRenderTargetView(g_scene->rtv.get(), clear);
		context->ClearRenderTargetView(g_uiLayer->rtv.get(), clear);
		if (sc.swapChainBufferWrapped && sc.swapChainBufferWrapped->rtv)
			context->ClearRenderTargetView(sc.swapChainBufferWrapped->rtv, clear);
		if (sc.uiBufferWrapped && sc.uiBufferWrapped->rtv)
			context->ClearRenderTargetView(sc.uiBufferWrapped->rtv, clear);
		if (sc.hdrOutputWrapped && sc.hdrOutputWrapped->uav)
			context->ClearUnorderedAccessViewFloat(sc.hdrOutputWrapped->uav, clear);

		logger::info("[HDR] Resources ready: scene {}x{} fp16, UI layer 8-bit, {} image-space targets upgraded, UI alpha fix hook installed",
			desc.Width, desc.Height, g_upgradedTargets.size());
	}

	void ClearShaderCache()
	{
		g_encodeCS = nullptr;
		g_resolveCS = nullptr;
		g_compositeCS = nullptr;
	}

	// ---- per frame ---------------------------------------------------------------------------------------

	void BeginPostProcessing()
	{
		if (!g_session || !g_scene || g_frame.redirected)
			return;
		// (44a) HDR off: the lean path. The game draws into the 10-bit back buffer exactly as it
		// would into an 8-bit one; nothing of ours runs this frame.
		if (!g_outputActive)
			return;

		g_frame.sceneHDR = WantsSceneHDR();

		auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
		g_savedFramebufferTexture = fb.texture;
		g_savedFramebufferSRV = fb.SRV;
		g_savedFramebufferRTV = fb.RTV;
		fb.texture = g_scene->resource.get();
		fb.SRV = g_scene->srv.get();
		fb.RTV = g_scene->rtv.get();
		g_frame.redirected = true;
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	}

	void EndPostProcessing(int a_target)
	{
		if (!g_frame.redirected || g_savedFramebufferRTV == nullptr)
			return;

		// The chain's last pass normally leaves the slot we pointed at hdrScene bound.
		auto* context = globals::d3d::context;
		ID3D11RenderTargetView* bound = nullptr;
		context->OMGetRenderTargets(1, &bound, nullptr);
		bool onScene = false;
		uint32_t boundFormat = 0;
		int boundId = kTargetUnknown;
		if (bound) {
			winrt::com_ptr<ID3D11Resource> resource;
			bound->GetResource(resource.put());
			onScene = resource.get() == g_scene->resource.get();
			if (!onScene)
				boundId = IdentifyTexture(resource.get());
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			bound->GetDesc(&desc);
			boundFormat = static_cast<uint32_t>(desc.Format);
			bound->Release();
		}

		bool confirmed = onScene;
		if (onScene) {
			++g_redirectConfirmed;
			if (!g_loggedConfirmed) {
				logger::info("[HDR] Image-space output confirmed in the fp16 scene target");
				g_loggedConfirmed = true;
			}
		} else {
			++g_redirectUnconfirmed;
			// (44d) At the water surface (camera half under water) the game's depth-of-field effect is the
			// last of the chain: it draws the final image into the frame buffer -- hdrScene, through our
			// redirect -- and afterwards binds and clears UNDERWATER_MASK (R8, format 61) for the next
			// frame, setting its own render-target state back without re-binding it (SkyrimSE 1.5.97,
			// ImageSpaceEffectDepthOfField::Render, the tail after its last pass). 44c read that as "the
			// redirect did not take", showed the back buffer -- black at that point -- and drew the HUD
			// over it: the black scene with a visible UI. What the chain was drawn into is its own target
			// argument; when that is the frame buffer, the image is in hdrScene.
			int pendingTarget = -1;
			{
				auto* shadowState = globals::game::shadowState;
				GET_INSTANCE_MEMBER(renderTargets, shadowState)
				pendingTarget = static_cast<int>(renderTargets[0]);
			}
			const bool chainToFramebuffer = a_target == static_cast<int>(RE::RENDER_TARGET::kFRAMEBUFFER);
			const bool recover = chainToFramebuffer;
			if (recover) {
				confirmed = true;
				++g_chainRecovered;
				// Leave the scene bound as on any other frame: the sharpener and Neural Rendering read
				// the bound target. The engine re-binds its own state on its next draw.
				ID3D11RenderTargetView* rtv = g_scene->rtv.get();
				context->OMSetRenderTargets(1, &rtv, nullptr);
				globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
			}
			g_lastChainEndNote = std::format("chain into {}, ended on {} (format {}), engine's pending target {}: {}",
				TargetName(a_target), TargetName(boundId), boundFormat, pendingTarget,
				recover ? "scene taken from the fp16 target" : (chainToFramebuffer ? "shown as SDR (Chain End Check = 44c)" : "shown as SDR (not a frame-buffer chain)"));
			// One line per occurrence (the first frame after a frame that ended normally).
			if (g_lastChainEndOk && g_chainLogLines < kMaxChainLogLines) {
				++g_chainLogLines;
				logger::warn("[HDR] Image-space chain ended on another target (frame {}): {}", g_frameCounter, g_lastChainEndNote);
				if (g_chainLogLines == kMaxChainLogLines)
					logger::warn("[HDR] Further chain-end cases are only counted (HDR Output status page, F12 JSON)");
			}
			if (!g_loggedUnconfirmed)
				g_loggedUnconfirmed = true;
		}
		g_lastChainEndOk = onScene;
		g_frame.sceneFresh = confirmed;

		auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
		fb.texture = g_savedFramebufferTexture;
		fb.SRV = g_savedFramebufferSRV;
		fb.RTV = g_savedFramebufferRTV;
		g_savedFramebufferTexture = nullptr;
		g_savedFramebufferSRV = nullptr;
		g_savedFramebufferRTV = nullptr;
	}

	SceneLevels GetDisplayLevels()
	{
		SceneLevels out;
		const auto levels = ResolveLevels();
		out.peakRel = levels.peakRel;
		out.peakNits = levels.peakNits;
		out.paperWhiteNits = levels.paperWhiteNits;
		const auto& ll = globals::features::linearLighting;
		out.displayGamma = !(ll.loaded && ll.settings.enableLinearLighting && ll.settings.enableGammaCorrection);
		out.hdr = IsOutputActive();
		return out;
	}

	SceneLevels GetSceneLevels()
	{
		if (!g_frame.redirected || !g_frame.sceneHDR)
			return {};
		auto out = GetDisplayLevels();
		out.hdr = true;
		return out;
	}

	void NoteSceneBT2020()
	{
		if (g_frame.redirected && g_frame.sceneHDR)
			g_sceneBT2020 = true;
	}

	bool IsSceneHDRThisFrame()
	{
		return g_frame.redirected && g_frame.sceneHDR;
	}

	bool IsOutputActive()
	{
		return g_session && g_outputActive;
	}

	void OnUIStartBeforeHUDLess()
	{
		if (!g_session)
			return;
		g_frame.uiStart = true;

		if (!g_frame.sceneFresh) {
			// No scene of ours this frame (main menu, loading screen, or the redirect did not take):
			// the game drew straight into the back buffer.
			g_frame.path = Path::kWholeFrameSDR;
			return;
		}

		if (IsOutputActive() && g_frame.sceneHDR && g_uiLayer && SwapChain().hdrOutputWrapped) {
			// The scene stays in the fp16 target; the Present composite reads it from there, and
			// DLSS-G's HUD-less image is encoded from it (WriteHUDLess).
			g_frame.resolved = true;
			g_frame.path = Path::kSeparated;
			return;
		}

		ResolveSceneToBackBuffer();
		g_frame.path = Path::kWholeFrameSDR;
		RebindFramebuffer();
	}

	bool WriteHUDLess(WrappedResource* a_hudless)
	{
		if (!g_session || !IsOutputActive() || !g_frame.sceneFresh || !a_hudless || !a_hudless->uav)
			return false;
		// Separated: the HDR scene. Whole frame as SDR: the same SDR scene the back buffer got, at
		// paper white -- in both cases the encoding of the FinalColor composite.
		const bool sdr = g_frame.path != Path::kSeparated;
		if (!EncodeSceneTo(a_hudless->uav, sdr))
			return false;
		RebindFramebuffer();
		return true;
	}

	bool WillSceneBeHDR()
	{
		return g_session && g_scene && WantsSceneHDR();
	}

	void NoteDLSSGStatus(uint32_t a_status)
	{
		g_lastDLSSGStatus = a_status;
		if (a_status & (1u << 2)) {
			g_dlssGRejectedHDR = true;
			// (44a) Refused while HDR is off: the 10-bit chain itself is the problem (the default the
			// user did not ask for), so the next start goes back to 8-bit. Refused with HDR on is the
			// 43b situation and only reported.
			if (g_session && !g_outputActive && GetSettings().displayFormat == 0)
				NoteTenBitFailure("DLSS-G refused the 10-bit format (eFailHDRFormatNotSupported)");
		}
	}

	void OnUIStartAfterHUDLess()
	{
		if (!g_session || g_frame.path != Path::kSeparated || !g_uiLayer)
			return;

		// (43b) Split the UI off. The back buffer is cleared and becomes the overlay layer (whatever
		// Present-hook mods draw into it); the game's UI, and every mod that draws through the game's
		// frame-buffer slot, goes to the 8-bit UI layer.
		auto* context = globals::d3d::context;
		const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
		if (auto* backBuffer = SwapChain().swapChainBufferWrapped.get(); backBuffer && backBuffer->rtv)
			context->ClearRenderTargetView(backBuffer->rtv, clear);
		context->ClearRenderTargetView(g_uiLayer->rtv.get(), clear);

		auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
		if (fb.RTV != g_uiLayer->rtv.get()) {
			g_savedUIFramebufferRTV = fb.RTV;
			g_backBufferRTV = fb.RTV;
		}
		fb.RTV = g_uiLayer->rtv.get();
		g_frame.uiRedirected = true;
		RebindFramebuffer();
	}

	void BeginImGui()
	{
		g_imGuiRedirected = false;
		if (!g_session || !g_frame.uiRedirected || !g_uiLayer)
			return;
		// (43b) ImGui (the CS menu and overlays) inherits whatever is bound; on the separated path that
		// must be the UI layer, at its own size, so it is composited at the HUD brightness.
		auto* context = globals::d3d::context;
		UINT count = 1;
		context->RSGetViewports(&count, &g_savedImGuiViewport);
		if (count == 0)
			g_savedImGuiViewport = {};
		ID3D11RenderTargetView* rtv = g_uiLayer->rtv.get();
		context->OMSetRenderTargets(1, &rtv, nullptr);
		D3D11_VIEWPORT viewport{};
		viewport.Width = static_cast<float>(g_uiLayer->desc.Width);
		viewport.Height = static_cast<float>(g_uiLayer->desc.Height);
		viewport.MaxDepth = 1.0f;
		context->RSSetViewports(1, &viewport);
		g_imGuiRedirected = true;
	}

	void EndImGui()
	{
		if (!g_imGuiRedirected)
			return;
		if (g_savedImGuiViewport.Width > 0.0f)
			globals::d3d::context->RSSetViewports(1, &g_savedImGuiViewport);
		g_imGuiRedirected = false;
	}

	ID3D12Resource* CompositeForPresent()
	{
		if (!g_session)
			return nullptr;

		// A scene that never reached the UI-start hook still has to be put on screen.
		if (g_frame.sceneFresh && !g_frame.resolved)
			ResolveSceneToBackBuffer();

		if (!IsOutputActive()) {
			RestoreUIRedirect();
			g_lastPath = Path::kNone;
			return nullptr;
		}

		auto& sc = SwapChain();
		auto* backBuffer = sc.swapChainBufferWrapped.get();
		auto* output = sc.hdrOutputWrapped.get();
		auto* shader = GetShader(g_compositeCS, "COMPOSITE", "CSComposite");
		const bool separated = g_frame.path == Path::kSeparated && g_frame.uiRedirected && g_scene && g_uiLayer;
		if (!backBuffer || !backBuffer->srv || !output || !output->uav || !shader || !EnsureCB()) {
			RestoreUIRedirect();
			return nullptr;
		}

		// Last D3D11 work of the frame: every UI layer is final now.
		RestoreUIRedirect();

		Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::HDROutput);
		if (separated) {
			// Scene (HDR) + game UI layer + what Present-hook mods drew into the cleared back buffer.
			Dispatch(shader, { g_scene->srv.get(), g_uiLayer->srv.get(), backBuffer->srv }, output->uav,
				sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), false, 1, true, SceneIsBT2020()));
		} else {
			// Everything already in the back buffer as SDR: shown at paper white.
			Dispatch(shader, { backBuffer->srv }, output->uav, sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), true, 0));
		}
		Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::HDROutput);
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		g_lastPath = separated ? Path::kSeparated : Path::kWholeFrameSDR;
		return output->resource.get();
	}

	void AfterPresent()
	{
		if (!g_session)
			return;
		// Normally done by CompositeForPresent; never leave the frame-buffer slot on the UI layer.
		RestoreUIRedirect();
		g_frame = {};
		++g_frameCounter;
		g_lastSceneBT2020 = g_sceneBT2020;
		g_sceneBT2020 = false;

		// Colour-space changes land between frames, so the next frame is produced for the new one.
		const bool wanted = WantsOutput();
		if (wanted != g_lastWanted) {
			g_lastWanted = wanted;
			g_colorSpaceFailed = false;
		}
		if (wanted != g_outputActive && !g_colorSpaceFailed) {
			if (ApplyColorSpace(wanted)) {
				g_outputActive = wanted;
				// (43c) The encoding of FinalColor and the HUD-less image changes with the next frame:
				// DLSS-G must not interpolate between an SDR and an HDR10 frame.
				SwapChain().RequestDLSSGResume(wanted ? "HDR output switched on" : "HDR output switched off");
			} else {
				g_colorSpaceFailed = true;
			}
			logger::info("[HDR] Output {} ({})", g_outputActive ? "HDR10" : "SDR", g_colorSpaceWhere);
		}

		// (port of ba9b41e81 / f748118bb) Start every HDR frame from a cleared back buffer, so a frame
		// the game only partly draws (startup, loading) cannot show what an earlier one left behind.
		if (g_outputActive) {
			auto* backBuffer = SwapChain().swapChainBufferWrapped.get();
			if (backBuffer && backBuffer->rtv) {
				const float clear[4]{ 0.0f, 0.0f, 0.0f, 0.0f };
				globals::d3d::context->ClearRenderTargetView(backBuffer->rtv, clear);
			}
		}
	}

	// ---- UI ----------------------------------------------------------------------------------------------

	void DrawSettings()
	{
		auto& s = GetSettings();
		const auto display = DisplayHDRInfo::GetDisplayState();
		auto& upscaling = globals::features::upscaling;
		const bool dlssGPath = upscaling.d3d12SwapChainActive && upscaling.IsDLSSGBackend();

		ImGui::Checkbox("Enable HDR Output", &s.enabled);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Real HDR10 for an HDR monitor: highlights (sun, sky, fire, metal glints) go brighter than white,");
			ImGui::TextUnformatted("the rest of the picture stays as you tuned it. Needs Windows HDR on, and NVIDIA RTX HDR / Windows Auto HDR off.");
			ImGui::TextUnformatted("With Display Format = Auto the display is already 10-bit, so this switches instantly (and with A/B Compare).");
			ImGui::TextUnformatted("The tone curve in HDR is set in Post Processing > Color Grading and Tone Mapping (HDR Mode).");
		}

		if (globals::game::isVR) {
			ImGui::TextDisabled("Not available in VR.");
		} else if (!dlssGPath) {
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Needs Frame Generation backend = DLSS-G (the D3D12 path). Not active this session.");
		} else if (s.enabled && !g_session) {
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "The display is 8-bit this session (%s). Restart to switch it to 10-bit.", g_tenBitDecision.c_str());
		} else if (!s.enabled && g_session) {
			ImGui::TextDisabled("Off: SDR on the 10-bit display chain (switches on instantly).");
		} else if (s.enabled && display.valid && !display.windowsHDR) {
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Windows HDR is off for this monitor: turn it on in Windows display settings.");
		} else if (s.enabled && g_colorSpaceFailed) {
			ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "The swap chain refused the HDR10 colour space (see the log). Showing SDR.");
		} else if (IsOutputActive()) {
			ImGui::TextColored({ 0.5f, 1.0f, 0.5f, 1.0f }, "HDR10 output active.");
		}

		LoadFallbackMarker();
		if (!g_tenBitFallbackReason.empty() && s.displayFormat == 0)
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Auto fell back to 8-bit: %s (Advanced below)", g_tenBitFallbackReason.c_str());

		ImGui::BeginDisabled(!s.enabled);

		const char* peakModes[] = { "Auto (from the display)", "Manual" };
		int peakMode = s.peakMode == 1 ? 1 : 0;
		if (ImGui::Combo("Peak Brightness", &peakMode, peakModes, IM_ARRAYSIZE(peakModes)))
			s.peakMode = static_cast<uint>(peakMode);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("The brightest the picture may get. Auto uses what the display reports to Windows, kept between 400 and 1000 nits");
			ImGui::TextUnformatted("(450 if it reports nothing). AW2725Q in True Black 400 reports about 465.");
		}
		if (s.peakMode == 1) {
			ImGui::SliderFloat("Peak (nits)", &s.peakNitsManual, kManualPeakMin, kManualPeakMax, "%.0f nits", ImGuiSliderFlags_Logarithmic);
		} else {
			ImGui::TextDisabled("Display reports %.0f nits -> using %.0f (%s)", display.valid ? display.maxLuminance : 0.0f, ResolvePeakNits(), g_lastPeakSource);
		}

		ImGui::SliderFloat("Paper White (nits)", &s.paperWhiteNits, kPaperWhiteMin, kPaperWhiteMax, "%.0f nits");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How bright the game's ordinary white is (and everything below it, in proportion).");
			ImGui::TextUnformatted("203 is the HDR standard. Raise it if the picture looks darker than SDR; Windows' own SDR brightness is shown under Display / HDR info.");
		}

		ImGui::SliderFloat("HUD Brightness (nits)", &s.hudNits, 80.0f, 400.0f, "%.0f nits");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How bright the HUD, the CS menu and other mods' widgets are during play (they are drawn on their own layer).");
			ImGui::TextUnformatted("A little under paper white is easier on the eyes and on an OLED. Menus, the main menu and loading screens use paper white.");
		}

		ImGui::EndDisabled();

		ImGui::Spacing();
		ImGui::TextDisabled("Tone curve in HDR (Extend / Native, roll-off): Post Processing > Color Grading and Tone Mapping.");
		ImGui::TextDisabled("Sharpening in HDR: Upscaling > Sharpening (RCAS, works with HDR on or off).");

		// (44g) Fixed now: lean path with HDR off, menus on their own UI layer, colour space on the
		// Streamline proxy, the 44d water-surface fix, no sharpening in HDR, no copy-format logging.
		if (ImGui::TreeNodeEx("Advanced")) {
			const char* formats[] = { "Auto (10-bit on an HDR display)", "8-bit (legacy, HDR needs a restart)" };
			int format = s.displayFormat == 1 ? 1 : 0;
			if (ImGui::Combo("Display Format (restart)", &format, formats, IM_ARRAYSIZE(formats)))
				s.displayFormat = static_cast<uint>(format);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Auto: whenever Windows says the monitor can do HDR, the game's display chain is 10-bit (HDR10's format),");
				ImGui::TextUnformatted("so HDR can be switched on and off at any time. With HDR off it looks the same as 8-bit, gradients slightly finer.");
				ImGui::TextUnformatted("8-bit (legacy): the game's own format; HDR then takes a restart to turn on.");
				ImGui::TextUnformatted("If the 10-bit chain or DLSS-G fails, the next start goes back to 8-bit by itself.");
			}
			ImGui::TextDisabled("This session: %s", g_tenBitDecision.c_str());
			if (!g_tenBitFallbackReason.empty() && s.displayFormat == 0 && ImGui::SmallButton("Retry 10-bit (next start)"))
				ClearFallbackMarker();
			ImGui::TreePop();
		}

		// (44a) Moved here from Upscaling: read-only display / swap-chain / HDR status.
		DisplayHDRInfo::DrawSettings();
	}

	void DrawStatus()
	{
		const auto& s = GetSettings();
		ImGui::Text("HDR Output: %s", IsOutputActive() ? "ON (HDR10)" : (g_session ? (s.enabled ? "waiting / refused (see below)" : "off (10-bit chain, SDR)") : "off"));
		ImGui::Text("Swap chain format: %u (%s)", static_cast<uint32_t>(g_chainFormat),
			g_chainFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? "R10G10B10A2, 10-bit" : (g_chainFormat == DXGI_FORMAT_R8G8B8A8_UNORM ? "R8G8B8A8, 8-bit" : "other"));
		ImGui::Text("Colour space set: %s (0x%08X)", g_colorSpaceWhere.c_str(), static_cast<uint32_t>(g_lastColorSpaceResult));
		if (g_session) {
			const auto levels = ResolveLevels();
			ImGui::Text("Peak %.0f nits (%s), paper white %.0f nits, HUD %.0f nits", levels.peakNits, g_lastPeakSource, levels.paperWhiteNits, levels.hudNits);
			ImGui::Text("Last frame: %s%s", PathName(g_lastPath), g_lastSceneBT2020 ? ", scene graded in BT.2020" : "");
			ImGui::Text("Scene in fp16 target: chain ended on it %llu, elsewhere %llu frames (%llu of those taken from it anyway); %zu image-space targets upgraded",
				g_redirectConfirmed, g_redirectUnconfirmed, g_chainRecovered, g_upgradedTargets.size());
			if (!g_lastChainEndNote.empty())
				ImGui::TextDisabled("Last chain end elsewhere: %s", g_lastChainEndNote.c_str());
			if (g_dlssGRejectedHDR)
				ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "DLSS-G rejected the HDR format (eFailHDRFormatNotSupported, status 0x%X)", g_lastDLSSGStatus);
			else if (g_lastDLSSGStatus)
				ImGui::Text("DLSS-G last failure status: 0x%X (see the log)", g_lastDLSSGStatus);
			else
				ImGui::TextDisabled("DLSS-G: no failure reported");
			const float ms = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::HDROutput);
			if (ms > 0.0f)
				ImGui::Text("GPU cost: %.3f ms per frame (HDR Output row)", ms);
			else
				ImGui::TextDisabled("GPU cost: open the Performance Overlay's pass table (F10) to measure");
		}
	}

	nlohmann::json GetMetaJson()
	{
		const auto& s = GetSettings();
		nlohmann::json j = nlohmann::json::object();
		j["enabled_setting"] = s.enabled;
		j["session_10bit"] = g_session;
		j["display_format"] = s.displayFormat == 1 ? "8-bit legacy" : "auto";
		j["format_decision"] = g_tenBitDecision;
		j["format_fallback"] = g_tenBitFallbackReason;
		j["scene_bt2020_last"] = g_lastSceneBT2020;
		j["active"] = IsOutputActive();
		j["swap_chain_format"] = static_cast<uint32_t>(g_chainFormat);
		j["color_space"] = g_colorSpaceWhere;
		j["color_space_hresult"] = static_cast<uint32_t>(g_lastColorSpaceResult);
		if (g_session) {
			const auto levels = ResolveLevels();
			j["peak_nits"] = levels.peakNits;
			j["peak_source"] = g_lastPeakSource;
			j["paper_white_nits"] = levels.paperWhiteNits;
			j["hud_nits"] = levels.hudNits;
			j["last_path"] = PathName(g_lastPath);
			j["scene_confirmed_frames"] = g_redirectConfirmed;
			j["scene_unconfirmed_frames"] = g_redirectUnconfirmed;
			j["scene_recovered_frames"] = g_chainRecovered;
			j["chain_end_last_note"] = g_lastChainEndNote;
			j["dlssg_last_failure_status"] = g_lastDLSSGStatus;
			j["dlssg_rejected_hdr_format"] = g_dlssGRejectedHDR;
			j["gpu_ms"] = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::HDROutput);
		}
		return j;
	}

	std::string RestartNote(const std::string& a_key)
	{
		if (a_key == "enabled" && !g_session)
			return "needs a restart (8-bit display this session)";
		if (a_key == "displayFormat")
			return "needs a restart";
		return {};
	}
}
