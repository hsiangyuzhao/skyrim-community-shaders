#include "CasterFilter.h"

#include "Globals.h"
#include "Utils/D3D.h"

namespace RayTracingCasterFilter
{
	namespace
	{
		// Geometry -> number of captures holding it (a cell and its replacement can overlap); (47z) keepable by any of them.
		struct Entry
		{
			std::uint32_t count = 0;
			bool keepable = false;
		};
		ankerl::unordered_dense::map<const RE::BSGeometry*, Entry> g_geometries;
		bool g_latched = false;
		bool g_watch = false;
		bool g_keep = false;  // (47z) Keep Buildings in the Game's Sun Shadow Map, this frame
		bool g_inSunCascade = false;
		float g_rect[4]{};
		std::uint32_t g_skipped = 0;
		std::uint32_t g_skippedLast = 0;
		std::uint32_t g_epoch = 1;  // (45i)

		// (45f) Missed casters: seen once each (the set), handed over a few at a time (the queue).
		constexpr std::size_t kMissedSetLimit = 16384;
		constexpr std::size_t kMissedQueueLimit = 64;
		ankerl::unordered_dense::set<const RE::BSGeometry*> g_missed;
		std::vector<RE::NiPointer<RE::BSGeometry>> g_missedQueue;

		// (47z) The forward cascades: a depth array like the engine's sun cascades (kKeepSlices slices at most), holding only the
		// redirected casters of this frame; merged with the engine's in RayTracing::ForwardShadowMap.
		struct Kept
		{
			winrt::com_ptr<ID3D11Texture2D> texture;
			winrt::com_ptr<ID3D11ShaderResourceView> srv;
			winrt::com_ptr<ID3D11DepthStencilView> dsv[kKeepSlices];
			ID3D11Resource* source = nullptr;  // the engine's cascade texture it was made for (identity only, never dereferenced)
			D3D11_TEXTURE2D_DESC sourceDesc{};
			std::uint32_t slices = 0;
			std::uint32_t clearedFrame[kKeepSlices]{};  // g_frame of the last clear (0 = never)
			std::uint64_t bytes = 0;
		} g_kept;
		bool g_keptFailed = false;  // creation failed once: no more tries until ReleaseKept()
		std::uint32_t g_frame = 1;  // Latch() count
		std::uint32_t g_keptDraws = 0, g_keptDrawsLast = 0;
		std::uint32_t g_redirectFailed = 0, g_redirectFailedLast = 0;
		std::uint32_t g_redirectLost = 0, g_redirectLostLast = 0;  // redirected draws that still landed elsewhere (self-check)
		ID3D11DepthStencilView* g_redirectDsv = nullptr;
		// The bound targets of the redirected draw in progress (restored by EndRedirect).
		ID3D11RenderTargetView* g_savedRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
		ID3D11DepthStencilView* g_savedDsv = nullptr;
		UINT g_savedRtvCount = 0;

		bool InsideRect(const RE::NiBound& a_bound)
		{
			return a_bound.center.x - a_bound.radius >= g_rect[0] && a_bound.center.y - a_bound.radius >= g_rect[1] &&
			       a_bound.center.x + a_bound.radius <= g_rect[2] && a_bound.center.y + a_bound.radius <= g_rect[3];
		}

		void NoteMissed(RE::BSGeometry* a_geometry)
		{
			const auto& b = a_geometry->worldBound;
			// A full queue leaves the geometry unmarked: it is noted again on a later frame.
			if (!(b.radius >= kMissedMinRadius) || !InsideRect(b) || g_missed.size() >= kMissedSetLimit || g_missedQueue.size() >= kMissedQueueLimit)
				return;
			if (!g_missed.insert(a_geometry).second)
				return;
			g_missedQueue.emplace_back(a_geometry);
		}

		void ReleaseSaved()
		{
			for (UINT i = 0; i < g_savedRtvCount; ++i) {
				if (g_savedRtv[i])
					g_savedRtv[i]->Release();
				g_savedRtv[i] = nullptr;
			}
			g_savedRtvCount = 0;
			if (g_savedDsv)
				g_savedDsv->Release();
			g_savedDsv = nullptr;
		}

		/// The forward cascades for the engine's cascade texture a_source (DSV a_template's format); created on first use / a change.
		bool EnsureKept(ID3D11Resource* a_source, ID3D11DepthStencilView* a_template)
		{
			winrt::com_ptr<ID3D11Texture2D> source;
			if (FAILED(a_source->QueryInterface(IID_PPV_ARGS(source.put()))))
				return false;
			D3D11_TEXTURE2D_DESC desc{};
			source->GetDesc(&desc);
			if (g_kept.texture && g_kept.source == a_source && g_kept.sourceDesc.Width == desc.Width && g_kept.sourceDesc.Height == desc.Height &&
				g_kept.sourceDesc.ArraySize == desc.ArraySize && g_kept.sourceDesc.Format == desc.Format)
				return true;
			if (g_keptFailed)
				return false;
			ReleaseKept();
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvTemplate{};
			a_template->GetDesc(&dsvTemplate);
			D3D11_TEXTURE2D_DESC keptDesc = desc;
			keptDesc.ArraySize = std::min<UINT>(desc.ArraySize, kKeepSlices);
			keptDesc.MipLevels = 1;
			keptDesc.Usage = D3D11_USAGE_DEFAULT;
			keptDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
			keptDesc.CPUAccessFlags = 0;
			keptDesc.MiscFlags = 0;
			keptDesc.SampleDesc = { 1, 0 };
			// The SRV format of the depth format (the merge reads depth only).
			DXGI_FORMAT srvFormat = DXGI_FORMAT_UNKNOWN;
			switch (dsvTemplate.Format) {
			case DXGI_FORMAT_D32_FLOAT:
				srvFormat = DXGI_FORMAT_R32_FLOAT;
				break;
			case DXGI_FORMAT_D24_UNORM_S8_UINT:
				srvFormat = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
				break;
			case DXGI_FORMAT_D16_UNORM:
				srvFormat = DXGI_FORMAT_R16_UNORM;
				break;
			case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
				srvFormat = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
				break;
			default:
				break;
			}
			auto* device = globals::d3d::device;
			HRESULT hr = srvFormat == DXGI_FORMAT_UNKNOWN ? E_INVALIDARG : device->CreateTexture2D(&keptDesc, nullptr, g_kept.texture.put());
			if (SUCCEEDED(hr)) {
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = srvFormat;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				srvDesc.Texture2DArray.MipLevels = 1;
				srvDesc.Texture2DArray.ArraySize = keptDesc.ArraySize;
				hr = device->CreateShaderResourceView(g_kept.texture.get(), &srvDesc, g_kept.srv.put());
			}
			for (UINT s = 0; SUCCEEDED(hr) && s < keptDesc.ArraySize; ++s) {
				D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
				dsvDesc.Format = dsvTemplate.Format;
				dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
				dsvDesc.Texture2DArray.FirstArraySlice = s;
				dsvDesc.Texture2DArray.ArraySize = 1;
				hr = device->CreateDepthStencilView(g_kept.texture.get(), &dsvDesc, g_kept.dsv[s].put());
			}
			if (FAILED(hr)) {
				logger::warn("[RayTracing] Keep Buildings in the Game's Sun Shadow Map: could not create its cascades ({}x{}, depth format {}, hr {:08X}); "
							 "those casters stay out of the shadow map (as 47y)",
					desc.Width, desc.Height, static_cast<int>(dsvTemplate.Format), static_cast<std::uint32_t>(hr));
				ReleaseKept();
				g_keptFailed = true;
				return false;
			}
			Util::SetResourceName(g_kept.texture.get(), "RayTracing Kept Sun Casters");
			g_kept.source = a_source;
			g_kept.sourceDesc = desc;
			g_kept.slices = keptDesc.ArraySize;
			std::uint32_t texelBytes = 4;
			if (dsvTemplate.Format == DXGI_FORMAT_D16_UNORM)
				texelBytes = 2;
			else if (dsvTemplate.Format == DXGI_FORMAT_D32_FLOAT_S8X24_UINT)
				texelBytes = 8;
			g_kept.bytes = static_cast<std::uint64_t>(desc.Width) * desc.Height * keptDesc.ArraySize * texelBytes;
			logger::info("[RayTracing] Keep Buildings in the Game's Sun Shadow Map: {}x{} x {} slices ({:.1f} MB)", desc.Width, desc.Height,
				keptDesc.ArraySize, static_cast<double>(g_kept.bytes) / (1024.0 * 1024.0));
			return true;
		}
	}

	void Latch(bool a_active, const float a_rectMinMax[4], bool a_watch, bool a_keep)
	{
		g_skippedLast = g_skipped;
		g_skipped = 0;
		g_keptDrawsLast = g_keptDraws;
		g_keptDraws = 0;
		g_redirectFailedLast = g_redirectFailed;
		g_redirectFailed = 0;
		g_redirectLostLast = g_redirectLost;
		g_redirectLost = 0;
		if (++g_frame == 0)
			g_frame = 1;
		g_latched = a_active && !g_geometries.empty();
		g_watch = a_watch;
		g_keep = g_latched && a_keep;
		if (a_rectMinMax)
			std::copy_n(a_rectMinMax, 4, g_rect);
	}

	bool IsLatched() { return g_latched; }

	void SetInSunCascade(bool a_inside) { g_inSunCascade = a_inside; }

	Decision Decide(const RE::BSRenderPass* a_pass)
	{
		if (!g_inSunCascade || !a_pass || !a_pass->geometry || (!g_latched && !g_watch))
			return Decision::kDraw;
		const auto it = g_geometries.find(a_pass->geometry);
		if (it == g_geometries.end()) {
			if (g_watch)
				NoteMissed(a_pass->geometry);
			return Decision::kDraw;
		}
		if (!g_latched)
			return Decision::kDraw;
		// (45e) The live bound: objects that move (doors, items) are tested where they are now.
		// Whole bound inside the rect: its shadow on anything nearby is traced, not blended.
		const auto& bound = a_pass->geometry->worldBound;
		if (!InsideRect(bound))
			return Decision::kDraw;
		// (47z) Kept for the forward readers (drawn into the forward cascades, not the engine's).
		if (g_keep && it->second.keepable && bound.radius >= kKeepMinRadius)
			return Decision::kRedirect;
		++g_skipped;
		return Decision::kSkip;
	}

	bool BeginRedirect()
	{
		auto* context = globals::d3d::context;
		ReleaseSaved();
		context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, g_savedRtv, &g_savedDsv);
		// Only the bound slots are set again (a larger count would also unbind pixel-shader UAVs bound past them).
		g_savedRtvCount = 0;
		for (UINT i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; ++i) {
			if (g_savedRtv[i])
				g_savedRtvCount = i + 1;
		}
		bool ok = false;
		if (g_savedDsv) {
			D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc{};
			g_savedDsv->GetDesc(&dsvDesc);
			std::uint32_t slice = UINT32_MAX;
			if (dsvDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2DARRAY && dsvDesc.Texture2DArray.ArraySize == 1 && dsvDesc.Texture2DArray.MipSlice == 0)
				slice = dsvDesc.Texture2DArray.FirstArraySlice;
			else if (dsvDesc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D && dsvDesc.Texture2D.MipSlice == 0)
				slice = 0;
			if (slice < kKeepSlices) {
				winrt::com_ptr<ID3D11Resource> resource;
				g_savedDsv->GetResource(resource.put());
				if (resource && EnsureKept(resource.get(), g_savedDsv) && slice < g_kept.slices) {
					auto* dsv = g_kept.dsv[slice].get();
					if (g_kept.clearedFrame[slice] != g_frame) {
						context->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
						g_kept.clearedFrame[slice] = g_frame;
					}
					context->OMSetRenderTargets(g_savedRtvCount, g_savedRtv, dsv);
					g_redirectDsv = dsv;
					ok = true;
				}
			}
		}
		if (!ok) {
			ReleaseSaved();
			++g_redirectFailed;
			++g_skipped;
		}
		return ok;
	}

	void EndRedirect()
	{
		auto* context = globals::d3d::context;
		// Self-check: the draw went where it was sent (the engine did not re-bind its own target inside it).
		ID3D11DepthStencilView* bound = nullptr;
		context->OMGetRenderTargets(0, nullptr, &bound);
		if (bound != g_redirectDsv)
			++g_redirectLost;
		if (bound)
			bound->Release();
		context->OMSetRenderTargets(g_savedRtvCount, g_savedRtv, g_savedDsv);
		ReleaseSaved();
		++g_keptDraws;
	}

	ID3D11ShaderResourceView* KeptCascades(ID3D11ShaderResourceView* a_cascades)
	{
		if (!a_cascades || g_keptDraws == 0 || !g_kept.texture)
			return nullptr;
		winrt::com_ptr<ID3D11Resource> resource;
		a_cascades->GetResource(resource.put());
		if (resource.get() != g_kept.source)
			return nullptr;
		for (std::uint32_t s = 0; s < g_kept.slices; ++s) {
			if (g_kept.clearedFrame[s] != g_frame) {
				globals::d3d::context->ClearDepthStencilView(g_kept.dsv[s].get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
				g_kept.clearedFrame[s] = g_frame;
			}
		}
		return g_kept.srv.get();
	}

	std::uint32_t KeptLastFrame() { return g_keptDrawsLast; }

	std::uint32_t RedirectFailedLastFrame() { return g_redirectFailedLast; }

	std::uint32_t RedirectLostLastFrame() { return g_redirectLostLast; }

	std::uint64_t KeptBytes() { return g_kept.texture ? g_kept.bytes : 0; }

	void ReleaseKept()
	{
		g_kept = {};
		g_keptFailed = false;
	}

	void Add(const RE::BSGeometry* a_geometry, bool a_keepable)
	{
		if (a_geometry) {
			auto& entry = g_geometries[a_geometry];
			++entry.count;
			entry.keepable = entry.keepable || a_keepable;
		}
	}

	void Remove(const RE::BSGeometry* a_geometry)
	{
		const auto it = g_geometries.find(a_geometry);
		if (it != g_geometries.end() && --it->second.count == 0)
			g_geometries.erase(it);
	}

	void Clear()
	{
		g_geometries.clear();
		++g_epoch;
		g_latched = false;
		g_watch = false;
		g_keep = false;
		g_missedQueue.clear();
	}

	std::uint32_t Epoch() { return g_epoch; }

	std::uint32_t RegisteredCount() { return static_cast<std::uint32_t>(g_geometries.size()); }

	std::uint32_t SkippedLastFrame() { return g_skippedLast; }

	std::uint32_t MissedCount() { return static_cast<std::uint32_t>(g_missed.size()); }

	std::vector<RE::NiPointer<RE::BSGeometry>> TakeMissed()
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> out;
		out.swap(g_missedQueue);
		return out;
	}

	void ForgetMissed(const RE::BSGeometry* a_geometry)
	{
		g_missed.erase(a_geometry);
	}

	void ResetMissed()
	{
		g_missed.clear();
		g_missedQueue.clear();
	}
}
