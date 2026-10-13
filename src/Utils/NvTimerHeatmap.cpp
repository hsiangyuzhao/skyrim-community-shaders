#include "Utils/NvTimerHeatmap.h"

#include "State.h"
#include "Utils/Nvapi.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <vector>

#include <imgui.h>

namespace Util::NvTimerHeatmap
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr uint32_t kSlots = 3;
		/// The overlay stays up this long after the last heatmap dispatch (SSRT switched off, menu, ...).
		constexpr double kOverlayHoldSeconds = 1.0;

		struct ReadbackSlot
		{
			winrt::com_ptr<ID3D11Texture2D> staging;
			bool pending = false;
			uint32_t frameWritten = 0;
			uint32_t threadsPerTile = 1;
		};

		winrt::com_ptr<ID3D11Texture2D> tileTexture;
		winrt::com_ptr<ID3D11UnorderedAccessView> tileUAV;
		uint32_t tileWidth = 0;
		uint32_t tileHeight = 0;
		uint32_t currentThreadsPerTile = 1;
		bool begun = false;
		bool createFailed = false;

		std::array<ReadbackSlot, kSlots> slots;
		uint32_t writeSlot = 0;

		winrt::com_ptr<ID3D11Texture2D> colorTexture;
		winrt::com_ptr<ID3D11ShaderResourceView> colorSRV;
		uint32_t colorWidth = 0;
		uint32_t colorHeight = 0;

		std::vector<float> tileUs;  ///< microseconds per thread, per tile
		std::vector<float> scratch;
		std::vector<uint32_t> pixels;

		float scaleUs = 0.0f;  ///< smoothed 98th percentile: the value drawn full red
		float meanUs = 0.0f;
		float p95Us = 0.0f;
		float maxUs = 0.0f;
		bool haveImage = false;

		Clock::time_point lastBegin{};

		uint32_t FrameIndex() { return globals::state ? globals::state->frameCount : 0u; }

		void ReleaseAll()
		{
			tileTexture = nullptr;
			tileUAV = nullptr;
			for (auto& slot : slots)
				slot = {};
			tileWidth = tileHeight = 0;
		}

		bool EnsureTiles(ID3D11Device* a_device, uint32_t a_width, uint32_t a_height)
		{
			if (tileTexture && tileWidth == a_width && tileHeight == a_height)
				return true;
			ReleaseAll();

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R32_UINT;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, tileTexture.put())))
				return false;
			if (FAILED(a_device->CreateUnorderedAccessView(tileTexture.get(), nullptr, tileUAV.put()))) {
				ReleaseAll();
				return false;
			}
			desc.Usage = D3D11_USAGE_STAGING;
			desc.BindFlags = 0;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			for (auto& slot : slots) {
				if (FAILED(a_device->CreateTexture2D(&desc, nullptr, slot.staging.put()))) {
					ReleaseAll();
					return false;
				}
			}
			tileWidth = a_width;
			tileHeight = a_height;
			return true;
		}

		bool EnsureColor(ID3D11Device* a_device, uint32_t a_width, uint32_t a_height)
		{
			if (colorTexture && colorWidth == a_width && colorHeight == a_height)
				return true;
			colorTexture = nullptr;
			colorSRV = nullptr;
			colorWidth = colorHeight = 0;

			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = a_width;
			desc.Height = a_height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			if (FAILED(a_device->CreateTexture2D(&desc, nullptr, colorTexture.put())))
				return false;
			if (FAILED(a_device->CreateShaderResourceView(colorTexture.get(), nullptr, colorSRV.put()))) {
				colorTexture = nullptr;
				return false;
			}
			colorWidth = a_width;
			colorHeight = a_height;
			return true;
		}

		/// 0 = blue (cheap) -> cyan -> green -> yellow -> red (at or above the scale).
		uint32_t HeatColor(float a_t)
		{
			static constexpr float kStops[5][3] = {
				{ 0.05f, 0.10f, 0.60f }, { 0.00f, 0.75f, 0.95f }, { 0.10f, 0.85f, 0.20f }, { 1.00f, 0.90f, 0.10f }, { 0.95f, 0.10f, 0.05f }
			};
			const float x = std::clamp(a_t, 0.0f, 1.0f) * 4.0f;
			const int i = std::min(static_cast<int>(x), 3);
			const float f = x - static_cast<float>(i);
			uint32_t rgba = 0xFF000000u;
			for (int c = 0; c < 3; ++c) {
				const float v = kStops[i][c] + (kStops[i + 1][c] - kStops[i][c]) * f;
				rgba |= static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f) << (8 * c);
			}
			return rgba;
		}

		/// Folds the oldest finished readback into the image. Never waits on the GPU.
		void Collect(ID3D11DeviceContext* a_context)
		{
			const uint32_t frame = FrameIndex();
			ReadbackSlot* oldest = nullptr;
			for (uint32_t i = 0; i < kSlots; ++i) {
				auto& slot = slots[(writeSlot + i) % kSlots];
				if (slot.pending) {
					oldest = &slot;
					break;
				}
			}
			if (!oldest || frame - oldest->frameWritten < 2)
				return;

			D3D11_MAPPED_SUBRESOURCE mapped{};
			if (a_context->Map(oldest->staging.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped) != S_OK)
				return;
			const size_t count = static_cast<size_t>(tileWidth) * tileHeight;
			tileUs.resize(count);
			const float toUs = 1.0f / (1000.0f * static_cast<float>(std::max(oldest->threadsPerTile, 1u)));
			for (uint32_t y = 0; y < tileHeight; ++y) {
				const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch);
				for (uint32_t x = 0; x < tileWidth; ++x)
					tileUs[static_cast<size_t>(y) * tileWidth + x] = static_cast<float>(row[x]) * toUs;
			}
			a_context->Unmap(oldest->staging.get(), 0);
			oldest->pending = false;

			scratch = tileUs;
			const auto percentile = [&](float a_p) {
				const size_t k = std::min(scratch.size() - 1, static_cast<size_t>(a_p * static_cast<float>(scratch.size() - 1)));
				std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(k), scratch.end());
				return scratch[k];
			};
			double sum = 0.0;
			maxUs = 0.0f;
			for (float v : tileUs) {
				sum += v;
				maxUs = std::max(maxUs, v);
			}
			meanUs = static_cast<float>(sum / static_cast<double>(count));
			p95Us = percentile(0.95f);
			const float p98 = std::max(percentile(0.98f), 1e-3f);
			scaleUs = scaleUs > 0.0f ? scaleUs * 0.9f + p98 * 0.1f : p98;

			auto* device = globals::d3d::device;
			if (!device || !EnsureColor(device, tileWidth, tileHeight))
				return;
			pixels.resize(count);
			const float inv = 1.0f / scaleUs;
			for (size_t i = 0; i < count; ++i)
				pixels[i] = HeatColor(tileUs[i] * inv);
			D3D11_MAPPED_SUBRESOURCE dst{};
			if (FAILED(a_context->Map(colorTexture.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &dst)))
				return;
			for (uint32_t y = 0; y < tileHeight; ++y)
				std::memcpy(static_cast<uint8_t*>(dst.pData) + static_cast<size_t>(y) * dst.RowPitch, pixels.data() + static_cast<size_t>(y) * tileWidth, static_cast<size_t>(tileWidth) * sizeof(uint32_t));
			a_context->Unmap(colorTexture.get(), 0);
			haveImage = true;
		}
	}

	ID3D11UnorderedAccessView* Begin(ID3D11DeviceContext* a_context, uint32_t a_tilesX, uint32_t a_tilesY, uint32_t a_threadsPerTile)
	{
		begun = false;
		if (!a_context || !a_tilesX || !a_tilesY || createFailed || !Util::Nvapi::Use(Util::Nvapi::Item::TimerHeatmap))
			return nullptr;
		auto* device = globals::d3d::device;
		if (!device || !EnsureTiles(device, a_tilesX, a_tilesY)) {
			createFailed = true;
			Util::Nvapi::DisableForSession(Util::Nvapi::Item::TimerHeatmap, "Could not create the heatmap surfaces");
			return nullptr;
		}
		const UINT zeros[4] = { 0, 0, 0, 0 };
		a_context->ClearUnorderedAccessViewUint(tileUAV.get(), zeros);
		currentThreadsPerTile = std::max(a_threadsPerTile, 1u);
		begun = true;
		lastBegin = Clock::now();
		return tileUAV.get();
	}

	void End(ID3D11DeviceContext* a_context)
	{
		if (!begun || !a_context)
			return;
		begun = false;
		auto& slot = slots[writeSlot];
		a_context->CopyResource(slot.staging.get(), tileTexture.get());
		slot.pending = true;  // an older unread result in this slot is simply replaced
		slot.frameWritten = FrameIndex();
		slot.threadsPerTile = currentThreadsPerTile;
		writeSlot = (writeSlot + 1) % kSlots;
	}

	bool WantsOverlayFrame()
	{
		return Util::Nvapi::Use(Util::Nvapi::Item::TimerHeatmap) &&
		       std::chrono::duration<double>(Clock::now() - lastBegin).count() < kOverlayHoldSeconds;
	}

	void DrawOverlay()
	{
		if (!WantsOverlayFrame()) {
			haveImage = false;
			return;
		}
		if (auto* context = globals::d3d::context)
			Collect(context);
		if (!haveImage || !colorSRV)
			return;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		const float width = std::min(vp->Size.x * 0.32f, 640.0f);
		const float height = width * static_cast<float>(colorHeight) / static_cast<float>(std::max(colorWidth, 1u));
		ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - width - 24.0f, vp->Pos.y + vp->Size.y - height - 120.0f), ImGuiCond_FirstUseEver);
		ImGui::SetNextWindowBgAlpha(0.8f);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if (ImGui::Begin("SSRT ray march cost (NVIDIA timer)", nullptr, flags)) {
			ImGui::Image(colorSRV.get(), ImVec2(width, height));
			ImGui::Text("Per 8x8 block, average time per GPU thread. Blue = cheap, red = %.1f us or more.", scaleUs);
			ImGui::Text("Mean %.1f us   95%% of blocks under %.1f us   slowest %.1f us   (%ux%u blocks)", meanUs, p95Us, maxUs, colorWidth, colorHeight);
			ImGui::TextDisabled("Diffuse ray march only. Turn off in Advanced > NVIDIA > Debug.");
		}
		ImGui::End();
	}
}
