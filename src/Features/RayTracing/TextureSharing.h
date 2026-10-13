#pragma once

#include <cstdint>

struct ID3D11Device;

/**
 * @brief (45h) Makes the game's diffuse textures shareable with the ray tracer's D3D12 device.
 *
 * Follows upstream's raytracing branch (TextureSharing.h + its CreateTextureFromDDS and
 * ID3D11Device::CreateTexture2D hooks): while the engine creates a texture from a DDS file whose
 * name is not a known non-diffuse map (_n, _s, _g, _m, ...), D3D11_RESOURCE_MISC_SHARED is added
 * to the texture's description, so D3D12 can later open it (legacy shared handle) and sample the
 * full-resolution texture directly: no copy, no extra VRAM.
 *
 * Only while SetEnabled(true) (Ray Tracing and "Alpha-tested casters" both on): textures loaded
 * before that stay unshareable and the ray tracer uses its 256x256 alpha copy for them. A
 * creation the driver refuses with the flag is repeated without it (counted); after many refusals
 * in a row the flag is no longer tried this session. With the switch off both hooks only pass
 * the call through.
 *
 * Thread-safe: textures load on the engine's loader threads; the "inside a DDS load" mark is per
 * thread.
 */
namespace RayTracingTextureSharing
{
	/// Installs the hooks (once, after the D3D11 device exists). Not on VR (Ray Tracing is SE/AE only).
	void Install(ID3D11Device* a_device);
	void SetEnabled(bool a_enabled);
	[[nodiscard]] bool IsInstalled();

	struct Counts
	{
		std::uint64_t loads = 0;      // DDS loads seen while enabled
		std::uint64_t diffuse = 0;    // ... of a diffuse texture (by file name)
		std::uint64_t shared = 0;     // textures created shareable
		std::uint64_t refused = 0;    // creations the driver refused with the flag (created without it)
		bool gaveUp = false;          // too many refusals in a row: no longer tried this session
	};
	[[nodiscard]] Counts GetCounts();
}
