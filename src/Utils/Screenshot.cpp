#include "Utils/Screenshot.h"

#include "Features/PerformanceOverlay.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/HDROutput.h"
#include "State.h"

#include <DirectXTex.h>
#include <thread>

namespace Util::Screenshot
{
	namespace
	{
		struct Pending
		{
			winrt::com_ptr<ID3D11Texture2D> staging;
			D3D11_TEXTURE2D_DESC desc{};
			bool pq = false;
			uint64_t frame = 0;
			std::string stem;
			float paperWhite = 203.0f;
			float peak = 1000.0f;
		};

		bool g_requested = false;
		std::string g_requestedStem;
		uint64_t g_frame = 0;
		bool g_capturedThisFrame = false;
		std::vector<Pending> g_inFlight;
		std::mutex g_fileMutex;
		std::string g_lastFile;

		std::filesystem::path Folder()
		{
			auto dir = logger::log_directory();
			if (!dir)
				return {};
			return *dir / "CommunityShaders" / "Screenshots";
		}

		float SrgbEncode(float c)
		{
			c = std::clamp(c, 0.0f, 1.0f);
			return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
		}

		float PqToNits(float e)
		{
			constexpr float m1 = 2610.0f / 16384.0f, m2 = 2523.0f / 4096.0f * 128.0f;
			constexpr float c1 = 3424.0f / 4096.0f, c2 = 2413.0f / 4096.0f * 32.0f, c3 = 2392.0f / 4096.0f * 32.0f;
			const float p = std::pow(std::clamp(e, 0.0f, 1.0f), 1.0f / m2);
			return 10000.0f * std::pow(std::max(p - c1, 0.0f) / (c2 - c3 * p), 1.0f / m1);
		}

		/// Decodes one texel to {r, g, b} in [0,1] code values (no transfer function applied).
		bool ReadTexel(DXGI_FORMAT a_format, const uint8_t* a_row, uint32_t a_x, float o[3])
		{
			switch (a_format) {
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				{
					const uint8_t* p = a_row + a_x * 4;
					o[0] = p[0] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[2] / 255.0f;
					return true;
				}
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
				{
					const uint8_t* p = a_row + a_x * 4;
					o[0] = p[2] / 255.0f, o[1] = p[1] / 255.0f, o[2] = p[0] / 255.0f;
					return true;
				}
			case DXGI_FORMAT_R10G10B10A2_UNORM:
				{
					uint32_t v;
					std::memcpy(&v, a_row + a_x * 4, 4);
					o[0] = (v & 0x3FF) / 1023.0f, o[1] = ((v >> 10) & 0x3FF) / 1023.0f, o[2] = ((v >> 20) & 0x3FF) / 1023.0f;
					return true;
				}
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
				{
					const auto* p = reinterpret_cast<const uint16_t*>(a_row + a_x * 8);
					// scRGB (linear, 1.0 = 80 nits): shown as SDR at 1.0 = white, sRGB-encoded.
					for (int c = 0; c < 3; ++c)
						o[c] = SrgbEncode(DirectX::PackedVector::XMConvertHalfToFloat(p[c]));
					return true;
				}
			default:
				return false;
			}
		}

		void Encode(std::vector<uint8_t> a_pixels, uint32_t a_rowPitch, Pending a_p)
		{
			const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			try {
				const auto folder = Folder();
				if (folder.empty())
					throw std::runtime_error("SKSE log folder not found");
				std::filesystem::create_directories(folder);
				const uint32_t w = a_p.desc.Width, h = a_p.desc.Height;

				DirectX::ScratchImage sdr;
				DX::ThrowIfFailed(sdr.Initialize2D(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1));
				const DirectX::Image* out = sdr.GetImage(0, 0, 0);

				// BT.2020 -> BT.709 (linear).
				constexpr float m[3][3] = { { 1.6605f, -0.5876f, -0.0728f }, { -0.1246f, 1.1329f, -0.0083f }, { -0.0182f, -0.1006f, 1.1187f } };
				const float paper = std::max(a_p.paperWhite, 80.0f);
				const float peakRel = std::max(a_p.peak / paper, 1.0f);
				constexpr float knee = 0.75f;

				for (uint32_t y = 0; y < h; ++y) {
					const uint8_t* row = a_pixels.data() + static_cast<size_t>(y) * a_rowPitch;
					uint8_t* dst = out->pixels + static_cast<size_t>(y) * out->rowPitch;
					for (uint32_t x = 0; x < w; ++x) {
						float c[3]{};
						ReadTexel(a_p.desc.Format, row, x, c);
						if (a_p.pq) {
							// PQ -> nits -> BT.709 relative to paper white -> soft shoulder from 0.75 to the peak -> sRGB.
							float lin[3];
							for (int i = 0; i < 3; ++i)
								lin[i] = PqToNits(c[i]) / paper;
							float r709[3];
							for (int i = 0; i < 3; ++i)
								r709[i] = std::max(m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2], 0.0f);
							const float mx = std::max({ r709[0], r709[1], r709[2] });
							if (mx > knee) {
								const float range = std::max(peakRel - knee, 1e-3f);
								const float t = (mx - knee) / range;
								const float mapped = knee + (1.0f - knee) * (1.0f - std::exp(-t * 3.0f)) / (1.0f - std::exp(-3.0f));
								const float s = std::min(mapped, 1.0f) / mx;
								for (float& v : r709)
									v *= s;
							}
							for (int i = 0; i < 3; ++i)
								c[i] = SrgbEncode(r709[i]);
						}
						dst[x * 4 + 0] = static_cast<uint8_t>(std::clamp(c[0], 0.0f, 1.0f) * 255.0f + 0.5f);
						dst[x * 4 + 1] = static_cast<uint8_t>(std::clamp(c[1], 0.0f, 1.0f) * 255.0f + 0.5f);
						dst[x * 4 + 2] = static_cast<uint8_t>(std::clamp(c[2], 0.0f, 1.0f) * 255.0f + 0.5f);
						dst[x * 4 + 3] = 255;
					}
				}

				const auto pngPath = folder / (a_p.stem + ".png");
				DX::ThrowIfFailed(DirectX::SaveToWICFile(*out, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), pngPath.c_str()));
				std::string saved = pngPath.filename().string();

				// The untouched HDR10 image as a 16-bit PNG (PQ code values, BT.2020).
				auto& overlay = globals::features::performanceOverlay;
				if (a_p.pq && overlay.settings.ScreenshotSaveHDR) {
					DirectX::ScratchImage hdr;
					DX::ThrowIfFailed(hdr.Initialize2D(DXGI_FORMAT_R16G16B16A16_UNORM, w, h, 1, 1));
					const DirectX::Image* img = hdr.GetImage(0, 0, 0);
					for (uint32_t y = 0; y < h; ++y) {
						const uint8_t* row = a_pixels.data() + static_cast<size_t>(y) * a_rowPitch;
						auto* dst = reinterpret_cast<uint16_t*>(img->pixels + static_cast<size_t>(y) * img->rowPitch);
						for (uint32_t x = 0; x < w; ++x) {
							float c[3]{};
							ReadTexel(a_p.desc.Format, row, x, c);
							for (int i = 0; i < 3; ++i)
								dst[x * 4 + i] = static_cast<uint16_t>(std::clamp(c[i], 0.0f, 1.0f) * 65535.0f + 0.5f);
							dst[x * 4 + 3] = 65535;
						}
					}
					const auto hdrPath = folder / (a_p.stem + "-hdr10-pq.png");
					if (SUCCEEDED(DirectX::SaveToWICFile(*img, DirectX::WIC_FLAGS_NONE, DirectX::GetWICCodec(DirectX::WIC_CODEC_PNG), hdrPath.c_str())))
						saved += " + " + hdrPath.filename().string();
				}

				{
					std::scoped_lock lk(g_fileMutex);
					g_lastFile = saved;
				}
				logger::info("[Screenshot] Saved {} ({}x{}, {})", (folder / (a_p.stem + ".png")).string(), w, h, a_p.pq ? "HDR10 tone-mapped to SDR" : "SDR");
			} catch (const std::exception& e) {
				logger::error("[Screenshot] Saving failed: {}", e.what());
				std::scoped_lock lk(g_fileMutex);
				g_lastFile = std::string("failed: ") + e.what();
			}
			if (SUCCEEDED(co))
				CoUninitialize();
		}
	}

	std::string MakeStamp()
	{
		const auto now = std::chrono::system_clock::now();
		const std::time_t t = std::chrono::system_clock::to_time_t(now);
		std::tm local{};
		localtime_s(&local, &t);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
		return std::format("{}-{:03}", stamp, ms);
	}

	void Request(const std::string& a_stem)
	{
		g_requested = true;
		g_requestedStem = a_stem.empty() ? "cs-" + MakeStamp() : a_stem;
	}

	void Poll()
	{
		++g_frame;
		g_capturedThisFrame = false;
		if (g_inFlight.empty())
			return;
		auto* context = globals::d3d::context;
		for (auto it = g_inFlight.begin(); it != g_inFlight.end();) {
			if (g_frame < it->frame + 2) {
				++it;
				continue;
			}
			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (context->Map(it->staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK) {
				// Not finished yet; give up after a few seconds' worth of frames.
				if (g_frame > it->frame + 600) {
					logger::warn("[Screenshot] Readback never completed; dropped {}", it->stem);
					it = g_inFlight.erase(it);
				} else {
					++it;
				}
				continue;
			}
			std::vector<uint8_t> pixels(static_cast<size_t>(mapped.RowPitch) * it->desc.Height);
			std::memcpy(pixels.data(), mapped.pData, pixels.size());
			const uint32_t pitch = mapped.RowPitch;
			context->Unmap(it->staging.get(), 0);
			Pending p = std::move(*it);
			p.staging = nullptr;
			std::thread(Encode, std::move(pixels), pitch, std::move(p)).detach();
			it = g_inFlight.erase(it);
		}
	}

	void OnFinalImage(ID3D11Texture2D* a_image, bool a_pq)
	{
		if (!g_requested || g_capturedThisFrame || !a_image)
			return;
		g_capturedThisFrame = true;
		g_requested = false;

		D3D11_TEXTURE2D_DESC desc{};
		a_image->GetDesc(&desc);
		float dummy[3];
		if (desc.SampleDesc.Count != 1 || !ReadTexel(desc.Format, reinterpret_cast<const uint8_t*>(&dummy), 0, dummy)) {
			logger::warn("[Screenshot] Unsupported back-buffer format {} (or multisampled)", static_cast<uint32_t>(desc.Format));
			std::scoped_lock lk(g_fileMutex);
			g_lastFile = "failed: unsupported image format";
			return;
		}

		Pending p;
		p.desc = desc;
		D3D11_TEXTURE2D_DESC sdesc = desc;
		sdesc.MipLevels = 1;
		sdesc.ArraySize = 1;
		sdesc.Usage = D3D11_USAGE_STAGING;
		sdesc.BindFlags = 0;
		sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
		sdesc.MiscFlags = 0;
		if (FAILED(globals::d3d::device->CreateTexture2D(&sdesc, nullptr, p.staging.put()))) {
			logger::warn("[Screenshot] Could not create the readback texture");
			return;
		}
		globals::d3d::context->CopySubresourceRegion(p.staging.get(), 0, 0, 0, 0, a_image, 0, nullptr);
		p.pq = a_pq;
		p.frame = g_frame;
		p.stem = g_requestedStem;
		if (a_pq) {
			const auto levels = HDROutput::GetDisplayLevels();
			if (levels.paperWhiteNits > 0.0f)
				p.paperWhite = levels.paperWhiteNits;
			if (levels.peakNits > 0.0f)
				p.peak = levels.peakNits;
		}
		g_inFlight.push_back(std::move(p));
	}

	void OnD3D11Present(IDXGISwapChain* a_swapChain)
	{
		if (!g_requested || g_capturedThisFrame || !a_swapChain)
			return;
		auto* proxy = globals::features::upscaling.dx12SwapChain.swapChainProxy;
		if (proxy && static_cast<IDXGISwapChain*>(proxy) == a_swapChain)
			return;  // the D3D12 path captures after its HDR composite
		winrt::com_ptr<ID3D11Texture2D> backBuffer;
		if (FAILED(a_swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.put()))))
			return;
		OnFinalImage(backBuffer.get(), false);
	}

	std::string LastFile()
	{
		std::scoped_lock lk(g_fileMutex);
		return g_lastFile;
	}
}
