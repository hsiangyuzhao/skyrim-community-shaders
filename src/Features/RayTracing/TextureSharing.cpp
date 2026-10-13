#include "TextureSharing.h"

#include <atomic>

#include <d3d11.h>

namespace RayTracingTextureSharing
{
	namespace
	{
		std::atomic<bool> g_enabled{ false };
		std::atomic<bool> g_installed{ false };
		std::atomic<bool> g_gaveUp{ false };
		std::atomic<std::uint64_t> g_loads{ 0 }, g_diffuse{ 0 }, g_shared{ 0 }, g_refused{ 0 };
		std::atomic<std::uint32_t> g_refusedStreak{ 0 };
		constexpr std::uint32_t kRefusedStreakLimit = 64;

		thread_local bool t_shareNext = false;  // inside a DDS load of a diffuse texture on this thread

		bool SuffixIs(const char* a_begin, const char* a_end, const char* a_suffix)
		{
			const std::size_t length = static_cast<std::size_t>(a_end - a_begin);
			if (std::strlen(a_suffix) != length)
				return false;
			for (std::size_t i = 0; i < length; ++i) {
				char c = a_begin[i];
				if (c >= 'A' && c <= 'Z')
					c = static_cast<char>(c - 'A' + 'a');
				if (c != a_suffix[i])
					return false;
			}
			return true;
		}

		/// Upstream's TextureSharing::GetTextureType: the part of the file name after its last '_'
		/// names the map; everything that is not a known other map counts as a diffuse texture.
		bool IsDiffusePathUnguarded(const char* a_path)
		{
			if (!a_path || !a_path[0])
				return false;
			const char* stem = a_path;
			const char* end = a_path;
			for (const char* c = a_path; *c && c - a_path < 1024; ++c) {
				if (*c == '\\' || *c == '/')
					stem = c + 1;
				end = c + 1;
			}
			const char* dot = nullptr;
			for (const char* c = stem; c < end; ++c)
				if (*c == '.')
					dot = c;
			if (dot)
				end = dot;
			const char* underscore = nullptr;
			for (const char* c = stem; c < end; ++c)
				if (*c == '_')
					underscore = c;
			if (!underscore)
				return true;
			static constexpr const char* kOtherMaps[] = { "n", "p", "s", "g", "sk", "e", "m", "em", "msn", "rmaos", "b", "bl" };
			for (const char* other : kOtherMaps)
				if (SuffixIs(underscore + 1, end, other))
					return false;
			return true;
		}

		bool IsDiffusePath(const char* a_path)
		{
			__try {
				return IsDiffusePathUnguarded(a_path);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool Shareable(const D3D11_TEXTURE2D_DESC& a_desc)
		{
			constexpr UINT kExcluded = D3D11_RESOURCE_MISC_TEXTURECUBE | D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
			                           D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_GDI_COMPATIBLE | D3D11_RESOURCE_MISC_GENERATE_MIPS |
			                           D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TILE_POOL;
			return (a_desc.MiscFlags & kExcluded) == 0 && a_desc.ArraySize == 1 && a_desc.SampleDesc.Count == 1 && a_desc.CPUAccessFlags == 0 &&
			       a_desc.BindFlags == D3D11_BIND_SHADER_RESOURCE && (a_desc.Usage == D3D11_USAGE_DEFAULT || a_desc.Usage == D3D11_USAGE_IMMUTABLE) &&
			       a_desc.Width >= 4 && a_desc.Height >= 4;
		}

		struct ID3D11Device_CreateTexture2D
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* This, const D3D11_TEXTURE2D_DESC* a_desc, const D3D11_SUBRESOURCE_DATA* a_data,
				ID3D11Texture2D** o_texture)
			{
				if (!t_shareNext || !a_desc || !o_texture || g_gaveUp.load(std::memory_order_relaxed) || !Shareable(*a_desc))
					return func(This, a_desc, a_data, o_texture);
				t_shareNext = false;  // one texture per DDS load
				D3D11_TEXTURE2D_DESC desc = *a_desc;
				desc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED;
				const HRESULT hr = func(This, &desc, a_data, o_texture);
				if (SUCCEEDED(hr)) {
					g_shared.fetch_add(1, std::memory_order_relaxed);
					g_refusedStreak.store(0, std::memory_order_relaxed);
					return hr;
				}
				// The driver refused the flag for this texture: create it exactly as the game asked.
				g_refused.fetch_add(1, std::memory_order_relaxed);
				if (g_refusedStreak.fetch_add(1, std::memory_order_relaxed) + 1 >= kRefusedStreakLimit && !g_gaveUp.exchange(true))
					logger::warn("[RayTracing] Texture sharing: the driver refused {} shareable textures in a row; no longer tried this session", kRefusedStreakLimit);
				return func(This, a_desc, a_data, o_texture);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/// The engine's texture-from-DDS loader (upstream: RelocationID(69334, 70716)). Every argument
		/// is forwarded as a full register / stack slot; only the path (second argument) is read.
		struct CreateTextureFromDDS
		{
			static std::uintptr_t thunk(std::uintptr_t a1, const char* a_path, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5)
			{
				const bool previous = t_shareNext;
				bool share = false;
				if (g_enabled.load(std::memory_order_relaxed)) {
					g_loads.fetch_add(1, std::memory_order_relaxed);
					share = IsDiffusePath(a_path);
					if (share)
						g_diffuse.fetch_add(1, std::memory_order_relaxed);
				}
				t_shareNext = share;
				const std::uintptr_t result = func(a1, a_path, a3, a4, a5);
				t_shareNext = previous;
				return result;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void Install(ID3D11Device* a_device)
	{
		if (!a_device || g_installed.load() || REL::Module::IsVR())
			return;
		stl::detour_vfunc<5, ID3D11Device_CreateTexture2D>(a_device);
		stl::detour_thunk<CreateTextureFromDDS>(REL::RelocationID(69334, 70716));
		g_installed.store(true);
		logger::info("[RayTracing] Texture sharing hooks installed (inactive until Alpha-tested casters is on)");
	}

	void SetEnabled(bool a_enabled) { g_enabled.store(a_enabled, std::memory_order_relaxed); }

	bool IsInstalled() { return g_installed.load(); }

	Counts GetCounts()
	{
		Counts counts;
		counts.loads = g_loads.load(std::memory_order_relaxed);
		counts.diffuse = g_diffuse.load(std::memory_order_relaxed);
		counts.shared = g_shared.load(std::memory_order_relaxed);
		counts.refused = g_refused.load(std::memory_order_relaxed);
		counts.gaveUp = g_gaveUp.load(std::memory_order_relaxed);
		return counts;
	}
}
