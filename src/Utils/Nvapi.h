#pragma once

#include <cstdint>
#include <string>

/**
 * @brief Shared NVAPI runtime loader for D3D11 (used by Variable Rate Shading).
 *
 * Nothing is linked: nvapi64.dll is loaded at runtime and every entry point is resolved through
 * nvapi_QueryInterface (ids from the vendored nvapi_interface.h). On a machine without an NVIDIA
 * driver Loaded() is false and QueryInterface() returns nullptr; nothing here throws.
 */
namespace Util::Nvapi
{
	/// True once nvapi64.dll is loaded and NvAPI_Initialize succeeded (loads on first call).
	bool Loaded();
	/// Why Loaded() is false; empty when it is true.
	const std::string& LoadError();
	/// Raw nvapi_QueryInterface lookup through the shared loader; nullptr when not loaded or unknown.
	void* QueryInterface(uint32_t a_id);
}
