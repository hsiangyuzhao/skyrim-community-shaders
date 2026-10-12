#include "HDROutput.h"

#include <dxgi1_6.h>

#include "Buffer.h"
#include "Features/LinearLighting.h"
#include "Features/Upscaling.h"
#include "State.h"
#include "Util.h"
#include "Utils/DisplayHDRInfo.h"
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
			uint pad0;
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
		uint g_colorSpaceChainLatched = 0;

		// resources
		std::unique_ptr<Texture2D> g_scene;
		// (43b) The game's UI, third-party UI drawn through the game, and ImGui, during gameplay. 8-bit
		// like upstream's: the UI is SDR, and 8 bits round near-black fringes to zero.
		std::unique_ptr<Texture2D> g_uiLayer;
		std::unique_ptr<ConstantBuffer> g_cb;
		winrt::com_ptr<ID3D11ComputeShader> g_encodeCS;
		winrt::com_ptr<ID3D11ComputeShader> g_resolveCS;
		winrt::com_ptr<ID3D11ComputeShader> g_compositeCS;
		winrt::com_ptr<ID3D11ComputeShader> g_nisDecodeCS;  // (43c)
		winrt::com_ptr<ID3D11ComputeShader> g_nisEncodeCS;
		std::vector<SavedRenderTarget> g_upgradedTargets;

		// frame
		FrameState g_frame;
		Path g_lastPath = Path::kNone;
		ID3D11Texture2D* g_savedFramebufferTexture = nullptr;
		ID3D11ShaderResourceView* g_savedFramebufferSRV = nullptr;
		ID3D11RenderTargetView* g_savedFramebufferRTV = nullptr;
		ID3D11RenderTargetView* g_savedUIFramebufferRTV = nullptr;  // (43b) while the UI layer is the target
		D3D11_VIEWPORT g_savedImGuiViewport{};
		bool g_imGuiRedirected = false;

		// (43b) Third-party UI alpha-blend fix (upstream HDRDisplay's OMSetBlendState detour)
		bool g_blendHookInstalled = false;
		struct PatchedBlend
		{
			D3D11_BLEND_DESC original;
			winrt::com_ptr<ID3D11BlendState> patched;  // null: no change needed
		};
		std::unordered_map<ID3D11BlendState*, PatchedBlend> g_patchedBlends;
		std::unordered_map<ID3D11BlendState*, ID3D11BlendState*> g_patchedToOriginal;
		uint64_t g_blendPatchCount = 0;

		// (43c) DLSS-G
		uint32_t g_lastDLSSGStatus = 0;
		bool g_dlssGRejectedHDR = false;

		// diagnostics
		uint64_t g_redirectConfirmed = 0;
		uint64_t g_redirectUnconfirmed = 0;
		bool g_loggedConfirmed = false;
		bool g_loggedUnconfirmed = false;
		float g_lastPeakNits = kPeakFallback;
		const char* g_lastPeakSource = "fallback";

		Settings& GetSettings()
		{
			return globals::features::upscaling.settings.hdrOutput;
		}

		DX12SwapChain& SwapChain()
		{
			return Upscaling::dx12SwapChain;
		}

		bool IsMainOrLoadingMenu()
		{
			auto* ui = globals::game::ui;
			return ui && (ui->IsMenuOpen(RE::MainMenu::MENU_NAME) || ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME));
		}

		/// A menu that takes over the screen (pauses, or is an item / application / modal menu, or the map).
		bool IsMenuScene()
		{
			auto* ui = globals::game::ui;
			if (!ui)
				return false;
			return ui->GameIsPaused() || ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen() ||
			       ui->IsMenuOpen(RE::MapMenu::MENU_NAME) || ui->IsMenuOpen(RE::LockpickingMenu::MENU_NAME);
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
				if (auto raw = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Upscaling\\HDR\\HDROutputCS.hlsl", { { a_define, "" } }, "cs_5_0", a_entry)))
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

		OutputCB MakeCB(const Levels& a_levels, bool a_sceneIsSDR, uint a_compositeMode = 0, bool a_useOverlay = false)
		{
			OutputCB data{};
			data.paperWhiteNits = a_levels.paperWhiteNits;
			data.peakRel = a_levels.peakRel;
			data.hudScale = a_levels.hudNits / a_levels.paperWhiteNits;
			data.sceneIsSDR = a_sceneIsSDR ? 1.0f : 0.0f;
			data.compositeMode = a_compositeMode;
			data.useOverlay = a_useOverlay ? 1u : 0u;
			return data;
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
			Dispatch(shader, { g_scene->srv.get() }, backBuffer->uav, sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), true));
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
			Dispatch(shader, { g_scene->srv.get() }, a_target, sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), a_sceneIsSDR));
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
			// (43b) Menus: shown whole as SDR by default (no layer split, nothing for a menu's own
			// background effects to lose); "Separate UI layer" keeps them like gameplay.
			if (GetSettings().menuMode == 0 && IsMenuScene())
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
			HRESULT result = E_FAIL;
			std::string where;
			switch (g_colorSpaceChainLatched) {
			case 1:
				result = SetColorSpaceOn(native, distinct ? "native (below Streamline)" : "D3D12", a_hdr10);
				where = distinct ? "native chain" : "swap chain";
				break;
			case 2:
				{
					const HRESULT a = SetColorSpaceOn(proxy, distinct ? "Streamline proxy" : "D3D12", a_hdr10);
					const HRESULT b = distinct ? SetColorSpaceOn(native, "native (below Streamline)", a_hdr10) : a;
					result = SUCCEEDED(a) || SUCCEEDED(b) ? S_OK : a;
					where = distinct ? std::format("both (proxy 0x{:08X}, native 0x{:08X})", static_cast<uint32_t>(a), static_cast<uint32_t>(b)) : "swap chain";
					break;
				}
			default:
				result = SetColorSpaceOn(proxy, distinct ? "Streamline proxy" : "D3D12", a_hdr10);
				where = distinct ? "Streamline proxy chain" : "swap chain";
				if (FAILED(result) && distinct) {
					logger::warn("[HDR] The Streamline proxy refused the colour space; trying the native chain");
					result = SetColorSpaceOn(native, "native (below Streamline)", a_hdr10);
					where = "native chain (proxy refused)";
				}
				break;
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

		// ---- (43b) third-party UI alpha blending ------------------------------------------------------
		//
		// Ported from upstream HDRDisplay::GetPatchedAlphaBlendState and its OMSetBlendState detour. The UI
		// layer is composited by its alpha. A mod that blends alpha as [SrcAlpha, InvSrcAlpha] or
		// [InvSrcAlpha, Zero] (IED, for one) leaves alpha*(1-alpha) or less there, and its opaque panels
		// then let most of the scene through. While the UI layer is the target, every blended state's
		// alpha is rewritten to premultiplied [One, InvSrcAlpha, Add]; colour blending is untouched.
		// Only the immediate context, only during the UI-capture window, and the patched state is swapped
		// back for its original at the end of the window so the engine's state cache stays truthful.

		ID3D11BlendState* GetPatchedAlphaBlendState(ID3D11BlendState* a_original)
		{
			D3D11_BLEND_DESC desc{};
			a_original->GetDesc(&desc);
			if (auto it = g_patchedBlends.find(a_original); it != g_patchedBlends.end()) {
				if (std::memcmp(&it->second.original, &desc, sizeof(desc)) == 0)
					return it->second.patched ? it->second.patched.get() : a_original;
				// The address was released and reused for a different state.
				if (it->second.patched)
					g_patchedToOriginal.erase(it->second.patched.get());
				g_patchedBlends.erase(it);
			}

			PatchedBlend entry{ desc, nullptr };
			const int slots = desc.IndependentBlendEnable ? 8 : 1;
			bool needsPatch = false;
			for (int i = 0; i < slots; ++i) {
				const auto& rt = desc.RenderTarget[i];
				if (rt.BlendEnable && (rt.SrcBlendAlpha != D3D11_BLEND_ONE || rt.DestBlendAlpha != D3D11_BLEND_INV_SRC_ALPHA || rt.BlendOpAlpha != D3D11_BLEND_OP_ADD))
					needsPatch = true;
			}
			if (needsPatch) {
				D3D11_BLEND_DESC patchedDesc = desc;
				for (int i = 0; i < slots; ++i) {
					auto& rt = patchedDesc.RenderTarget[i];
					if (rt.BlendEnable) {
						rt.SrcBlendAlpha = D3D11_BLEND_ONE;
						rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
						rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
					}
				}
				if (FAILED(globals::d3d::device->CreateBlendState(&patchedDesc, entry.patched.put())))
					entry.patched = nullptr;
				if (entry.patched)
					g_patchedToOriginal[entry.patched.get()] = a_original;
			}
			auto* result = entry.patched ? entry.patched.get() : a_original;
			g_patchedBlends.emplace(a_original, std::move(entry));
			return result;
		}

		struct ID3D11DeviceContext_OMSetBlendState
		{
			static void WINAPI thunk(ID3D11DeviceContext* This, ID3D11BlendState* a_blendState, const FLOAT a_blendFactor[4], UINT a_sampleMask)
			{
				if (a_blendState && This == globals::d3d::context && g_frame.uiRedirected && GetSettings().uiBlendFix) {
					auto* patched = GetPatchedAlphaBlendState(a_blendState);
					if (patched != a_blendState)
						++g_blendPatchCount;
					a_blendState = patched;
				}
				func(This, a_blendState, a_blendFactor, a_sampleMask);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void RestoreUnpatchedBlendState()
		{
			if (!g_blendHookInstalled || g_patchedToOriginal.empty())
				return;
			auto* context = globals::d3d::context;
			ID3D11BlendState* current = nullptr;
			FLOAT factor[4]{};
			UINT mask = 0;
			context->OMGetBlendState(&current, factor, &mask);
			if (!current)
				return;
			if (auto it = g_patchedToOriginal.find(current); it != g_patchedToOriginal.end())
				ID3D11DeviceContext_OMSetBlendState::func(context, it->second, factor, mask);
			current->Release();
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
			if (!g_frame.uiRedirected)
				return;
			auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
			if (g_savedUIFramebufferRTV)
				fb.RTV = g_savedUIFramebufferRTV;
			g_savedUIFramebufferRTV = nullptr;
			RestoreUnpatchedBlendState();
			g_frame.uiRedirected = false;
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
		}
	}

	// ---- startup ---------------------------------------------------------------------------------------

	bool WantsTenBitSwapChain()
	{
		return !globals::game::isVR && GetSettings().enabled;
	}

	void OnSwapChainCreated()
	{
		auto& sc = SwapChain();
		g_chainFormat = sc.swapChainDesc.Format;
		g_session = g_chainFormat == DXGI_FORMAT_R10G10B10A2_UNORM;
		g_colorSpaceChainLatched = std::min(GetSettings().colorSpaceChain, 2u);
		logger::info("[HDR] Swap chain format {} -> native HDR output {} for this session", static_cast<uint32_t>(g_chainFormat),
			g_session ? "available" : "not available (HDR was off at startup)");
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

		if (!g_blendHookInstalled) {
			stl::detour_vfunc<35, ID3D11DeviceContext_OMSetBlendState>(globals::d3d::context);
			g_blendHookInstalled = true;
		}

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
		g_nisDecodeCS = nullptr;
		g_nisEncodeCS = nullptr;
	}

	// ---- per frame ---------------------------------------------------------------------------------------

	void BeginPostProcessing()
	{
		if (!g_session || !g_scene || g_frame.redirected)
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

	void EndPostProcessing()
	{
		if (!g_frame.redirected || g_savedFramebufferRTV == nullptr)
			return;

		// The chain's last pass should have drawn into the slot we pointed at hdrScene. Anything else
		// means the redirect did not take, and the scene went where it always went: show that as SDR.
		ID3D11RenderTargetView* bound = nullptr;
		globals::d3d::context->OMGetRenderTargets(1, &bound, nullptr);
		bool confirmed = false;
		uint32_t boundFormat = 0;
		if (bound) {
			winrt::com_ptr<ID3D11Resource> resource;
			bound->GetResource(resource.put());
			confirmed = resource.get() == g_scene->resource.get();
			D3D11_RENDER_TARGET_VIEW_DESC desc{};
			bound->GetDesc(&desc);
			boundFormat = static_cast<uint32_t>(desc.Format);
			bound->Release();
		}
		if (confirmed) {
			++g_redirectConfirmed;
			if (!g_loggedConfirmed) {
				logger::info("[HDR] Image-space output confirmed in the fp16 scene target");
				g_loggedConfirmed = true;
			}
		} else {
			++g_redirectUnconfirmed;
			if (!g_loggedUnconfirmed) {
				logger::warn("[HDR] Image-space chain ended on another target (format {}); this frame is shown as SDR", boundFormat);
				g_loggedUnconfirmed = true;
			}
		}
		g_frame.sceneFresh = confirmed;

		auto& fb = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
		fb.texture = g_savedFramebufferTexture;
		fb.SRV = g_savedFramebufferSRV;
		fb.RTV = g_savedFramebufferRTV;
		g_savedFramebufferTexture = nullptr;
		g_savedFramebufferSRV = nullptr;
		g_savedFramebufferRTV = nullptr;
	}

	float4 GetColorGradingParams()
	{
		if (!g_frame.redirected || !g_frame.sceneHDR)
			return { 0.0f, 0.0f, 0.0f, 0.0f };
		const auto& s = GetSettings();
		const auto levels = ResolveLevels();
		const auto& ll = globals::features::linearLighting;
		const bool displayLinear = ll.loaded && ll.settings.enableLinearLighting && ll.settings.enableGammaCorrection;
		const float mode = s.toneCurve == 2 ? 2.0f : 1.0f;
		return { mode, levels.peakRel, std::clamp(s.rolloffStart, 0.3f, 0.9f), displayLinear ? 0.0f : 1.0f };
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

	bool PrepareNISInput(ID3D11Resource* a_bound, ID3D11UnorderedAccessView* a_nisInput, bool& a_linear)
	{
		a_linear = false;
		if (!g_session || !g_scene || !a_nisInput || a_bound != g_scene->resource.get())
			return false;
		a_linear = IsSceneHDRThisFrame();
		if (a_linear && GetSettings().nisMode == 0)
			return false;  // "Sharpening in HDR: Off"
		auto* shader = a_linear ? GetShader(g_nisDecodeCS, "NIS_DECODE", "CSGammaToLinear") : GetShader(g_resolveCS, "RESOLVE_SDR", "CSResolveSDR");
		if (!shader || !EnsureCB())
			return false;
		Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::HDROutput);
		Dispatch(shader, { g_scene->srv.get() }, a_nisInput, g_scene->desc.Width, g_scene->desc.Height, MakeCB(ResolveLevels(), true));
		Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::HDROutput);
		return true;
	}

	void FinishNIS(ID3D11ShaderResourceView* a_nisOutput, bool a_linear)
	{
		if (!g_scene || !a_nisOutput)
			return;
		auto* shader = a_linear ? GetShader(g_nisEncodeCS, "NIS_ENCODE", "CSLinearToGamma") : GetShader(g_resolveCS, "RESOLVE_SDR", "CSResolveSDR");
		if (!shader || !EnsureCB())
			return;
		Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::HDROutput);
		Dispatch(shader, { a_nisOutput }, g_scene->uav.get(), g_scene->desc.Width, g_scene->desc.Height, MakeCB(ResolveLevels(), true));
		Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::HDROutput);
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	}

	void NoteDLSSGStatus(uint32_t a_status)
	{
		g_lastDLSSGStatus = a_status;
		if (a_status & (1u << 2))
			g_dlssGRejectedHDR = true;
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
		g_savedUIFramebufferRTV = fb.RTV;
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
				sc.swapChainDesc.Width, sc.swapChainDesc.Height, MakeCB(ResolveLevels(), false, 1, true));
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
		g_frame = {};
		if (!g_session)
			return;

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
			ImGui::TextUnformatted("The first time it is turned on the game must be restarted (the display format changes to 10-bit).");
			ImGui::TextUnformatted("After that it switches instantly, so it works with A/B Compare.");
		}

		if (globals::game::isVR) {
			ImGui::TextDisabled("Not available in VR.");
		} else if (!dlssGPath) {
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Needs Frame Generation backend = DLSS-G (the D3D12 path). Not active this session.");
		} else if (s.enabled && !g_session) {
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Restart the game to switch the display to 10-bit HDR10.");
		} else if (!s.enabled && g_session) {
			ImGui::TextDisabled("Off. The display stays 10-bit until the next restart (the picture is SDR).");
		} else if (s.enabled && display.valid && !display.windowsHDR) {
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Windows HDR is off for this monitor: turn it on in Windows display settings.");
		} else if (s.enabled && g_colorSpaceFailed) {
			ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "The swap chain refused the HDR10 colour space (see the log). Showing SDR.");
		} else if (IsOutputActive()) {
			ImGui::TextColored({ 0.5f, 1.0f, 0.5f, 1.0f }, "HDR10 output active.");
		}

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

		const char* menuModes[] = { "Show whole frame as SDR (safe)", "Separate UI layer, like gameplay" };
		int menuMode = s.menuMode == 1 ? 1 : 0;
		if (ImGui::Combo("Menus", &menuMode, menuModes, IM_ARRAYSIZE(menuModes)))
			s.menuMode = static_cast<uint>(menuMode);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Inventory, map, pause and other full-screen menus. SDR: the frame behind the menu loses its highlights while");
			ImGui::TextUnformatted("the menu is open, but nothing can go wrong with the menu itself. Separate: the world keeps its HDR highlights.");
		}

		ImGui::Checkbox("Fix Mod UI Transparency", &s.uiBlendFix);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Some mods' UI (IED, for one) writes transparency in a way that makes its panels see-through once the HUD is on its own layer.");
			ImGui::TextUnformatted("This corrects it while the HUD is drawn. Only matters with HDR on.");
		}

		const char* curves[] = { "Extend my tone mapper (keeps the SDR look)", "GT7 HDR curve" };
		int curve = s.toneCurve == 2 ? 1 : 0;
		if (ImGui::Combo("HDR Tone Curve", &curve, curves, IM_ARRAYSIZE(curves)))
			s.toneCurve = curve == 1 ? 2u : 1u;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Extend: the tone mapper chosen in Post Processing > Color Grading stays; only highlights above the roll-off");
			ImGui::TextUnformatted("start are allowed to go brighter, smoothly up to the peak.");
			ImGui::TextUnformatted("GT7: Gran Turismo 7's own HDR curve replaces the selected tone mapper (darker mid-tones unless you re-tune exposure).");
		}

		const char* nisModes[] = { "Off", "Linear mode" };
		int nisMode = s.nisMode == 1 ? 1 : 0;
		if (ImGui::Combo("Sharpening in HDR", &nisMode, nisModes, IM_ARRAYSIZE(nisModes)))
			s.nisMode = static_cast<uint>(nisMode);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("The DLSS Sharpness slider (NIS) while HDR is on. Off: no sharpening in HDR.");
			ImGui::TextUnformatted("Linear mode: the picture is handed to NIS in linear light, NIS's own HDR mode. Menus (shown as SDR) always sharpen as before.");
		}

		const char* chains[] = { "Streamline proxy (native if refused)", "Native chain below Streamline", "Both" };
		int chain = static_cast<int>(std::min(s.colorSpaceChain, 2u));
		if (ImGui::Combo("Colour Space Set On (restart)", &chain, chains, IM_ARRAYSIZE(chains)))
			s.colorSpaceChain = static_cast<uint>(chain);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Which swap chain is told the picture is HDR10. Under DLSS-G there are two: Streamline's and the real one below it.");
			ImGui::TextUnformatted("Only change this if the picture looks washed out or too dark with HDR on. Takes effect after a restart.");
		}
		if (s.colorSpaceChain != g_colorSpaceChainLatched && g_session)
			ImGui::TextColored({ 1.0f, 0.75f, 0.3f, 1.0f }, "Restart to apply the colour-space choice.");

		ImGui::SliderFloat("Highlight Roll-off Start", &s.rolloffStart, 0.3f, 0.9f, "%.2f");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			const auto levels = ResolveLevels();
			if (s.toneCurve == 2) {
				ImGui::Text("GT7: the curve stays straight up to this fraction of the peak (now %.0f nits), then bends toward it.", s.rolloffStart * levels.peakNits);
			} else {
				ImGui::Text("Extend: everything up to this fraction of SDR white (now %.0f nits) is exactly as in SDR;", s.rolloffStart * levels.paperWhiteNits);
				ImGui::TextUnformatted("above it highlights bend smoothly up toward the peak. Lower = more highlight punch, higher = closer to SDR.");
			}
		}

		ImGui::EndDisabled();
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
			ImGui::Text("Last frame: %s", PathName(g_lastPath));
			ImGui::Text("Scene in fp16 target: confirmed %llu, not confirmed %llu frames; %zu image-space targets upgraded",
				g_redirectConfirmed, g_redirectUnconfirmed, g_upgradedTargets.size());
			ImGui::Text("Mod UI transparency fixes: %llu (%zu blend states seen)", g_blendPatchCount, g_patchedBlends.size());
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
			j["tone_curve"] = s.toneCurve == 2 ? "gt7" : "extend";
			j["rolloff_start"] = s.rolloffStart;
			j["last_path"] = PathName(g_lastPath);
			j["scene_confirmed_frames"] = g_redirectConfirmed;
			j["scene_unconfirmed_frames"] = g_redirectUnconfirmed;
			j["menu_mode"] = s.menuMode == 1 ? "separate" : "sdr";
			j["ui_blend_fix"] = s.uiBlendFix;
			j["ui_blend_patches"] = g_blendPatchCount;
			j["nis_in_hdr"] = s.nisMode == 1 ? "linear" : "off";
			j["color_space_chain_setting"] = s.colorSpaceChain;
			j["color_space_chain_session"] = g_colorSpaceChainLatched;
			j["dlssg_last_failure_status"] = g_lastDLSSGStatus;
			j["dlssg_rejected_hdr_format"] = g_dlssGRejectedHDR;
			j["gpu_ms"] = Util::GpuPassTimers::GetSingleton()->SmoothedMs(Util::GpuBucket::HDROutput);
		}
		return j;
	}

	std::string RestartNote(const std::string& a_key)
	{
		if (a_key == "hdrOutput/enabled" && !g_session)
			return "needs a restart (10-bit display format)";
		if (a_key == "hdrOutput/colorSpaceChain")
			return "needs a restart";
		return {};
	}
}
