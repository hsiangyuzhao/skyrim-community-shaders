#include "Utils/Nvapi.h"

#include <format>

// NVAPI is only used for its types; every entry point is resolved at runtime through the
// driver's nvapi_QueryInterface, so nothing is linked and a machine without an NVIDIA driver
// simply never finds nvapi64.dll.
#pragma warning(push, 0)
#include <nvapi.h>
#pragma warning(pop)

namespace Util::Nvapi
{
	namespace
	{
		// Function ids from nvapi_interface.h (same vendored SDK as nvapi.h).
		constexpr uint32_t kIdInitialize = 0x0150e828;

		using QueryInterfaceFn = void*(__cdecl*)(uint32_t);
		using InitializeFn = NvAPI_Status(__cdecl*)();

		struct Api
		{
			bool ok = false;
			std::string error;
			QueryInterfaceFn queryInterface = nullptr;
		};

		/// Loaded once per process. NvAPI_Initialize is reference counted, so it coexists with
		/// Streamline and anything else in the process that also initialises NVAPI.
		const Api& GetApi()
		{
			static const Api api = [] {
				Api result;
				HMODULE module = LoadLibraryW(L"nvapi64.dll");
				if (!module) {
					result.error = "No NVIDIA driver interface (nvapi64.dll) found; this needs an NVIDIA GPU.";
					return result;
				}
				result.queryInterface = reinterpret_cast<QueryInterfaceFn>(GetProcAddress(module, "nvapi_QueryInterface"));
				if (!result.queryInterface) {
					result.error = "nvapi64.dll has no nvapi_QueryInterface export.";
					return result;
				}
				auto initialize = reinterpret_cast<InitializeFn>(result.queryInterface(kIdInitialize));
				if (!initialize) {
					result.error = "The NVIDIA driver does not expose NvAPI_Initialize.";
					return result;
				}
				if (const NvAPI_Status status = initialize(); status != NVAPI_OK) {
					result.error = std::format("NvAPI_Initialize failed ({}).", static_cast<int>(status));
					return result;
				}
				result.ok = true;
				return result;
			}();
			return api;
		}
	}

	bool Loaded() { return GetApi().ok; }

	const std::string& LoadError() { return GetApi().error; }

	void* QueryInterface(uint32_t a_id)
	{
		const Api& api = GetApi();
		return api.ok ? api.queryInterface(a_id) : nullptr;
	}
}
