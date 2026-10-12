#include "DisplayHDRInfo.h"

#include <dxgi1_6.h>
#include <winrt/base.h>

#include "Features/Upscaling.h"
#include "Features/Upscaling/HDROutput.h"
#include "Utils/UI.h"

namespace DisplayHDRInfo
{
	namespace
	{
		constexpr double kCheckIntervalSeconds = 2.0;

		struct SwapChainInfo
		{
			std::string label;
			uint32_t format = 0;
			bool queried = false;  // IDXGISwapChain3 available and the calls succeeded
			UINT hdr10 = 0;        // DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG bits, G2084 / P2020
			UINT scRGB = 0;        // G10 / P709
			UINT sRGB = 0;         // G22 / P709
		};

		struct Snapshot
		{
			bool valid = false;
			std::string reason;
			std::string timeLocal;

			// IDXGIOutput6::GetDesc1
			bool haveOutput = false;
			std::string deviceName;
			std::string monitorName;
			uint32_t colorSpace = 0;
			float minLuminance = 0.0f;
			float maxLuminance = 0.0f;
			float maxFullFrameLuminance = 0.0f;
			uint32_t bitsPerColor = 0;
			bool windowsHdrByColorSpace = false;

			// Windows display config (advanced colour)
			bool haveAdvancedColor = false;
			bool advancedColorSupported = false;
			bool advancedColorEnabled = false;
			bool advancedColorForceDisabled = false;
			uint32_t advancedColorBits = 0;
			bool haveSdrWhite = false;
			float sdrWhiteNits = 0.0f;

			// (43a) Windows 11 24H2 DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2: HDR (not just wide colour)
			// actually active. Ported from upstream HDRDisplay (credit there: renodx, MIT).
			bool haveAdvancedColor2 = false;
			bool hdrSupported2 = false;
			bool hdrActive2 = false;

			std::vector<SwapChainInfo> swapChains;
		};

		// (43a) Windows 11 24H2 display-config query, ported from upstream HDRDisplay (which credits
		// renodx by clshortfuse, MIT). Compat_ names avoid clashing with newer SDK headers.
		typedef enum
		{
			Compat_DISPLAYCONFIG_ADVANCED_COLOR_MODE_SDR = 0,
			Compat_DISPLAYCONFIG_ADVANCED_COLOR_MODE_WCG = 1,
			Compat_DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR = 2
		} Compat_DISPLAYCONFIG_ADVANCED_COLOR_MODE;

		typedef struct Compat_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2
		{
			DISPLAYCONFIG_DEVICE_INFO_HEADER header;
			union
			{
				struct
				{
					UINT32 advancedColorSupported: 1;
					UINT32 advancedColorActive: 1;
					UINT32 reserved1: 1;
					UINT32 advancedColorLimitedByPolicy: 1;
					UINT32 highDynamicRangeSupported: 1;
					UINT32 highDynamicRangeUserEnabled: 1;
					UINT32 wideColorSupported: 1;
					UINT32 wideColorUserEnabled: 1;
					UINT32 reserved: 24;
				};
				UINT32 value;
			};
			DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
			UINT32 bitsPerColorChannel;
			Compat_DISPLAYCONFIG_ADVANCED_COLOR_MODE activeColorMode;
		} Compat_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2;

		constexpr DISPLAYCONFIG_DEVICE_INFO_TYPE kDisplayConfigGetAdvancedColorInfo2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);

		std::mutex g_mutex;
		Snapshot g_snapshot;
		winrt::com_ptr<IDXGIFactory1> g_factory;
		HMONITOR g_monitor = nullptr;
		bool g_requested = true;
		bool g_first = true;
		std::chrono::steady_clock::time_point g_lastCheck{};

		std::string Narrow(const wchar_t* a_text)
		{
			if (!a_text || !*a_text)
				return {};
			const int size = WideCharToMultiByte(CP_UTF8, 0, a_text, -1, nullptr, 0, nullptr, nullptr);
			if (size <= 1)
				return {};
			std::string out(static_cast<size_t>(size - 1), '\0');
			WideCharToMultiByte(CP_UTF8, 0, a_text, -1, out.data(), size, nullptr, nullptr);
			return out;
		}

		const char* ColorSpaceName(uint32_t a_cs)
		{
			switch (a_cs) {
			case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
				return "sRGB (G22 P709)";
			case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709:
				return "scRGB (G10 P709)";
			case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020:
				return "HDR10 (G2084 P2020)";
			case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P2020:
				return "G22 P2020";
			default:
				return "other";
			}
		}

		std::string SupportText(UINT a_flags)
		{
			if (a_flags & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT)
				return (a_flags & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_OVERLAY_PRESENT) ? "yes (+overlay)" : "yes";
			return "no";
		}

		HWND GetGameWindow()
		{
			auto* swapChain = globals::d3d::swapChain;
			if (!swapChain)
				return nullptr;
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(swapChain->GetDesc(&desc)))
				return nullptr;
			return desc.OutputWindow;
		}

		void QuerySwapChain(Snapshot& a_snap, const char* a_label, IUnknown* a_swapChain)
		{
			SwapChainInfo info;
			info.label = a_label;
			if (a_swapChain) {
				winrt::com_ptr<IDXGISwapChain3> sc3;
				if (SUCCEEDED(a_swapChain->QueryInterface(IID_PPV_ARGS(sc3.put())))) {
					DXGI_SWAP_CHAIN_DESC1 desc{};
					if (SUCCEEDED(sc3->GetDesc1(&desc)))
						info.format = static_cast<uint32_t>(desc.Format);
					const bool a = SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020, &info.hdr10));
					const bool b = SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &info.scRGB));
					const bool c = SUCCEEDED(sc3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709, &info.sRGB));
					info.queried = a && b && c;
				}
			}
			a_snap.swapChains.push_back(std::move(info));
		}

		void QueryAdvancedColor(Snapshot& a_snap, const wchar_t* a_gdiDeviceName)
		{
			UINT32 pathCount = 0, modeCount = 0;
			if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS || pathCount == 0)
				return;
			std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
			if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount, modes.data(), nullptr) != ERROR_SUCCESS)
				return;

			for (UINT32 i = 0; i < pathCount; ++i) {
				const auto& path = paths[i];
				DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
				source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				source.header.size = sizeof(source);
				source.header.adapterId = path.sourceInfo.adapterId;
				source.header.id = path.sourceInfo.id;
				if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS)
					continue;
				if (wcscmp(source.viewGdiDeviceName, a_gdiDeviceName) != 0)
					continue;

				DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
				target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
				target.header.size = sizeof(target);
				target.header.adapterId = path.targetInfo.adapterId;
				target.header.id = path.targetInfo.id;
				if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
					a_snap.monitorName = Narrow(target.monitorFriendlyDeviceName);

				DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
				color.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
				color.header.size = sizeof(color);
				color.header.adapterId = path.targetInfo.adapterId;
				color.header.id = path.targetInfo.id;
				if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS) {
					a_snap.haveAdvancedColor = true;
					a_snap.advancedColorSupported = color.advancedColorSupported != 0;
					a_snap.advancedColorEnabled = color.advancedColorEnabled != 0;
					a_snap.advancedColorForceDisabled = color.advancedColorForceDisabled != 0;
					a_snap.advancedColorBits = static_cast<uint32_t>(color.bitsPerColorChannel);
				}

				Compat_DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO_2 color2{};
				color2.header.type = kDisplayConfigGetAdvancedColorInfo2;
				color2.header.size = sizeof(color2);
				color2.header.adapterId = path.targetInfo.adapterId;
				color2.header.id = path.targetInfo.id;
				if (DisplayConfigGetDeviceInfo(&color2.header) == ERROR_SUCCESS) {
					a_snap.haveAdvancedColor2 = true;
					a_snap.hdrSupported2 = color2.highDynamicRangeSupported != 0;
					a_snap.hdrActive2 = color2.activeColorMode == Compat_DISPLAYCONFIG_ADVANCED_COLOR_MODE_HDR;
				}

				DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
				white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
				white.header.size = sizeof(white);
				white.header.adapterId = path.targetInfo.adapterId;
				white.header.id = path.targetInfo.id;
				if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS) {
					a_snap.haveSdrWhite = true;
					a_snap.sdrWhiteNits = static_cast<float>(white.SDRWhiteLevel) / 1000.0f * 80.0f;  // 1000 = 80 nits
				}
				return;
			}
		}

		// A fresh factory every query: an existing one keeps reporting the output state it saw
		// when it was created (IsCurrent turns false instead), e.g. across a Windows HDR toggle.
		winrt::com_ptr<IDXGIFactory1> CreateFactory()
		{
			winrt::com_ptr<IDXGIFactory1> factory;
			if (auto dxgi = GetModuleHandleW(L"dxgi.dll")) {
				using CreateFn = HRESULT(WINAPI*)(REFIID, void**);
				if (auto create = reinterpret_cast<CreateFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")))
					create(IID_PPV_ARGS(factory.put()));
			}
			return factory;
		}

		void Query(const std::string& a_reason)
		{
			Snapshot snap;
			snap.reason = a_reason;
			{
				const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
				std::tm local{};
				localtime_s(&local, &now);
				char buf[32];
				std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &local);
				snap.timeLocal = buf;
			}

			const HWND window = GetGameWindow();
			const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
			g_monitor = monitor;
			g_factory = CreateFactory();

			if (g_factory && monitor) {
				winrt::com_ptr<IDXGIAdapter1> adapter;
				for (UINT a = 0; !snap.haveOutput && g_factory->EnumAdapters1(a, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++a) {
					winrt::com_ptr<IDXGIOutput> output;
					for (UINT o = 0; adapter->EnumOutputs(o, output.put()) != DXGI_ERROR_NOT_FOUND; ++o) {
						DXGI_OUTPUT_DESC desc{};
						if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor) {
							winrt::com_ptr<IDXGIOutput6> output6;
							DXGI_OUTPUT_DESC1 desc1{};
							if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(output6.put()))) && SUCCEEDED(output6->GetDesc1(&desc1))) {
								snap.haveOutput = true;
								snap.deviceName = Narrow(desc1.DeviceName);
								snap.colorSpace = static_cast<uint32_t>(desc1.ColorSpace);
								snap.minLuminance = desc1.MinLuminance;
								snap.maxLuminance = desc1.MaxLuminance;
								snap.maxFullFrameLuminance = desc1.MaxFullFrameLuminance;
								snap.bitsPerColor = desc1.BitsPerColor;
								snap.windowsHdrByColorSpace = desc1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
								QueryAdvancedColor(snap, desc1.DeviceName);
							}
							break;
						}
						output = nullptr;
					}
					adapter = nullptr;
				}
			}

			// The swap chains this plugin owns. Under DLSS-G the game presents through the
			// Streamline proxy; the native D3D12 chain sits below it. Without the proxy path the
			// game's own D3D11 chain is the only one.
			auto& upscaling = globals::features::upscaling;
			if (upscaling.d3d12SwapChainActive) {
				auto& dx12 = Upscaling::dx12SwapChain;
				IDXGISwapChain4* front = dx12.swapChain;
				IDXGISwapChain4* native = dx12.GetNativeSwapChain();
				if (native && native != front) {
					QuerySwapChain(snap, "D3D12 Streamline proxy (DLSS-G)", front);
					QuerySwapChain(snap, "D3D12 native (below Streamline)", native);
				} else {
					QuerySwapChain(snap, "D3D12 (FidelityFX / proxy)", front);
				}
			} else {
				QuerySwapChain(snap, "D3D11 native", globals::d3d::swapChain);
			}

			snap.valid = true;

			logger::info("[Display/HDR] ({}) monitor '{}' {}: Windows HDR {} (output colour space {} = {}), luminance min {:.4f} / max {:.1f} / full-frame {:.1f} nits, {} bits per colour",
				snap.reason, snap.monitorName, snap.deviceName,
				snap.haveAdvancedColor ? (snap.advancedColorEnabled ? "ON" : "off") : (snap.windowsHdrByColorSpace ? "ON" : "off"),
				snap.colorSpace, ColorSpaceName(snap.colorSpace),
				snap.minLuminance, snap.maxLuminance, snap.maxFullFrameLuminance, snap.bitsPerColor);
			if (!snap.haveOutput)
				logger::warn("[Display/HDR] Could not read IDXGIOutput6::GetDesc1 for the game window's monitor");
			if (snap.haveAdvancedColor)
				logger::info("[Display/HDR] Advanced colour: supported {}, enabled {}, force-disabled {}, {} bits per channel; SDR white {:.0f} nits",
					snap.advancedColorSupported, snap.advancedColorEnabled, snap.advancedColorForceDisabled, snap.advancedColorBits,
					snap.haveSdrWhite ? snap.sdrWhiteNits : 0.0f);
			if (snap.haveAdvancedColor2)
				logger::info("[Display/HDR] Windows 11 24H2 query: HDR supported {}, HDR active {}", snap.hdrSupported2, snap.hdrActive2);
			for (const auto& sc : snap.swapChains) {
				if (sc.queried)
					logger::info("[Display/HDR] Swap chain {}: format {}, CheckColorSpaceSupport HDR10 0x{:X} ({}), scRGB 0x{:X} ({}), sRGB 0x{:X} ({})",
						sc.label, sc.format, sc.hdr10, SupportText(sc.hdr10), sc.scRGB, SupportText(sc.scRGB), sc.sRGB, SupportText(sc.sRGB));
				else
					logger::info("[Display/HDR] Swap chain {}: IDXGISwapChain3 not available", sc.label);
			}

			std::lock_guard lock(g_mutex);
			g_snapshot = std::move(snap);
		}
	}

	void Tick()
	{
		const auto now = std::chrono::steady_clock::now();
		if (g_first) {
			g_first = false;
			g_requested = false;
			g_lastCheck = now;
			Query("startup");
			return;
		}
		if (!g_requested && std::chrono::duration<double>(now - g_lastCheck).count() < kCheckIntervalSeconds)
			return;
		g_lastCheck = now;

		std::string reason;
		if (g_requested) {
			reason = "refresh";
		} else if (!g_factory || !g_factory->IsCurrent()) {
			reason = "display change";
		} else {
			const HWND window = GetGameWindow();
			const HMONITOR monitor = window ? MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST) : nullptr;
			if (monitor != g_monitor)
				reason = "monitor change";
		}
		g_requested = false;
		if (!reason.empty())
			Query(reason);
	}

	void RequestRefresh()
	{
		g_requested = true;
	}

	DisplayState GetDisplayState()
	{
		std::lock_guard lock(g_mutex);
		DisplayState state;
		state.valid = g_snapshot.valid && g_snapshot.haveOutput;
		if (!state.valid)
			return state;
		state.windowsHDR = g_snapshot.haveAdvancedColor2 ? g_snapshot.hdrActive2 : g_snapshot.windowsHdrByColorSpace;
		state.maxLuminance = g_snapshot.maxLuminance;
		state.maxFullFrameLuminance = g_snapshot.maxFullFrameLuminance;
		state.sdrWhiteNits = g_snapshot.haveSdrWhite ? g_snapshot.sdrWhiteNits : 0.0f;
		return state;
	}

	void DrawSettings()
	{
		Snapshot snap;
		{
			std::lock_guard lock(g_mutex);
			snap = g_snapshot;
		}

		if (!ImGui::TreeNodeEx("Display / HDR info"))
			return;

		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Read-only. What Windows and the monitor report, and what native HDR output is doing. Nothing here changes the picture.");
		}

		// (43a) Native HDR output: state, the format and colour space actually set, cost.
		HDROutput::DrawStatus();
		ImGui::Separator();

		if (!snap.valid) {
			ImGui::TextUnformatted("Not queried yet.");
		} else {
			ImGui::Text("Monitor: %s (%s)", snap.monitorName.empty() ? "?" : snap.monitorName.c_str(), snap.deviceName.c_str());
			const bool hdrOn = snap.haveAdvancedColor ? snap.advancedColorEnabled : snap.windowsHdrByColorSpace;
			ImGui::Text("Windows HDR: %s", hdrOn ? "ON" : "off");
			if (snap.haveOutput) {
				ImGui::Text("Output colour space: %s (%u)", ColorSpaceName(snap.colorSpace), snap.colorSpace);
				ImGui::Text("Max luminance: %.1f nits", snap.maxLuminance);
				ImGui::Text("Max full-frame luminance: %.1f nits", snap.maxFullFrameLuminance);
				ImGui::Text("Min luminance: %.4f nits", snap.minLuminance);
				ImGui::Text("Bits per colour: %u", snap.bitsPerColor);
			} else {
				ImGui::TextUnformatted("IDXGIOutput6::GetDesc1: not available");
			}
			if (snap.haveSdrWhite)
				ImGui::Text("Windows SDR content brightness: %.0f nits", snap.sdrWhiteNits);
			if (snap.haveAdvancedColor2)
				ImGui::Text("Windows 11 24H2 query: HDR supported %s, HDR active %s", snap.hdrSupported2 ? "yes" : "no", snap.hdrActive2 ? "yes" : "no");
			for (const auto& sc : snap.swapChains) {
				if (sc.queried)
					ImGui::Text("%s (format %u): HDR10 %s, scRGB %s, sRGB %s", sc.label.c_str(), sc.format,
						SupportText(sc.hdr10).c_str(), SupportText(sc.scRGB).c_str(), SupportText(sc.sRGB).c_str());
				else
					ImGui::Text("%s: not queryable", sc.label.c_str());
			}
			ImGui::TextDisabled("Queried %s (%s)", snap.timeLocal.c_str(), snap.reason.c_str());
		}
		if (ImGui::SmallButton("Refresh"))
			RequestRefresh();
		ImGui::TreePop();
	}

	nlohmann::json GetMetaJson()
	{
		Snapshot snap;
		{
			std::lock_guard lock(g_mutex);
			snap = g_snapshot;
		}
		nlohmann::json j = nlohmann::json::object();
		j["valid"] = snap.valid;
		j["hdr_output"] = HDROutput::GetMetaJson();  // (43a)
		if (!snap.valid)
			return j;
		j["queried"] = snap.timeLocal;
		j["reason"] = snap.reason;
		j["monitor"] = snap.monitorName;
		j["device"] = snap.deviceName;
		j["windows_hdr"] = snap.haveAdvancedColor ? snap.advancedColorEnabled : snap.windowsHdrByColorSpace;
		if (snap.haveOutput) {
			j["color_space"] = snap.colorSpace;
			j["color_space_name"] = ColorSpaceName(snap.colorSpace);
			j["min_luminance"] = snap.minLuminance;
			j["max_luminance"] = snap.maxLuminance;
			j["max_full_frame_luminance"] = snap.maxFullFrameLuminance;
			j["bits_per_color"] = snap.bitsPerColor;
		}
		if (snap.haveAdvancedColor) {
			j["advanced_color"] = { { "supported", snap.advancedColorSupported }, { "enabled", snap.advancedColorEnabled },
				{ "force_disabled", snap.advancedColorForceDisabled }, { "bits_per_channel", snap.advancedColorBits } };
		}
		if (snap.haveSdrWhite)
			j["sdr_white_nits"] = snap.sdrWhiteNits;
		nlohmann::json chains = nlohmann::json::array();
		for (const auto& sc : snap.swapChains) {
			nlohmann::json c = { { "label", sc.label }, { "queried", sc.queried } };
			if (sc.queried) {
				c["format"] = sc.format;
				c["hdr10_g2084_p2020"] = sc.hdr10;
				c["scrgb_g10_p709"] = sc.scRGB;
				c["srgb_g22_p709"] = sc.sRGB;
			}
			chains.push_back(std::move(c));
		}
		j["swap_chains"] = std::move(chains);
		return j;
	}
}
