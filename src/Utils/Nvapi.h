#pragma once

#include <cstdint>
#include <d3d11.h>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * @brief (45c) Shared NVAPI access for D3D11: runtime loader, shader-extension slot, UAV overlap,
 *        and the "NVIDIA Driver Extensions" user switch.
 *
 * Nothing is linked: nvapi64.dll is loaded at runtime and every entry point is resolved through
 * nvapi_QueryInterface (ids from the vendored nvapi_interface.h). On a machine without an NVIDIA
 * driver, or when the driver refuses a call, every user of this file falls back to the code path it
 * had before 45c; nothing here throws.
 *
 * Settings live in SettingsUser.json under Advanced/NVIDIA and are part of A/B Compare (Global).
 * Every item is a hot switch: the extension-enabled shader variants are compiled whenever the driver
 * supports them, independent of the switches, so flipping a switch never recompiles anything.
 */
namespace Util::Nvapi
{
	/// UAV slot handed to NvAPI_D3D11_SetNvShaderExtnSlot. Must match NV_SHADER_EXTN_SLOT in
	/// package/Shaders/Common/NvShaderExtn.hlsli. Needs feature level 11_1 (64 UAV slots), which
	/// both device creation paths request.
	inline constexpr uint32_t kShaderExtnSlot = 63;

	/// Individually switchable uses. Each one also needs the master switch.
	enum class Item : uint32_t
	{
		WarpAtomics,       ///< grass culling: one atomic per warp instead of one per surviving instance
		UavOverlap,        ///< no driver wait between proven-independent compute dispatches
		ShaderExtensions,  ///< other shader-extension users: SSRT variance vote, grass Hi-Z quad reduce
		TimerHeatmap,      ///< diagnostic: per-tile cost heatmap of the SSRT diffuse ray march
		kCount
	};

	struct Settings
	{
		bool Enabled = true;  ///< "NVIDIA Driver Extensions" (master)
		bool WarpAtomics = true;
		bool UavOverlap = true;
		bool ShaderExtensions = true;
		bool TimerHeatmap = false;  ///< debug, off by default
	};

	/// Live settings. Render thread / UI thread (both run on the render thread here).
	Settings& GetSettings();

	/// True once nvapi64.dll is loaded and NvAPI_Initialize succeeded (loads on first call).
	bool Loaded();
	/// Why Loaded() is false; empty when it is true.
	const std::string& LoadError();
	/// Raw nvapi_QueryInterface lookup through the shared loader; nullptr when not loaded or unknown.
	void* QueryInterface(uint32_t a_id);

	/**
	 * @brief Called by both D3D11 device creation hooks right after the device exists, before the
	 *        engine or any feature creates a shader. Sets the global extension slot and probes the
	 *        opcodes the 45c shaders use. Safe to call more than once (only the first device counts).
	 */
	void OnDeviceCreated(ID3D11Device* a_device);

	/// The driver supports the item on this device (independent of the user switches).
	bool Available(Item a_item);
	/// Short plain-language reason when Available() is false.
	std::string_view UnavailableReason(Item a_item);
	/// Master switch && item switch && Available().
	bool Use(Item a_item);

	/// Shader defines that turn on the extension path of a shader for this item: {"NV_SHADER_EXTN","1"}
	/// when Available(a_item), otherwise empty. Independent of the switches (see the file comment).
	std::vector<std::pair<const char*, const char*>> ShaderDefines(Item a_item);

	/// Called when a UAV-overlap call failed: the item is turned off for the session with a reason.
	void DisableForSession(Item a_item, std::string a_reason);

	/**
	 * @brief RAII scope around dispatches that are proven not to touch each other's UAVs.
	 *
	 * Opens NvAPI_D3D11_BeginUAVOverlapEx with graphics + compute wait-for-idle at the start, so
	 * everything before the scope (clears, the pass that produced the inputs) is finished before the
	 * first dispatch inside it; inside, the driver no longer waits between dispatches. Closing it
	 * restores normal synchronisation. A no-op when the item is off or unavailable.
	 * Only put dispatches inside whose UAVs are disjoint and that read nothing another one writes,
	 * and write a one-line justification at each use.
	 */
	class UavOverlapScope
	{
	public:
		explicit UavOverlapScope(ID3D11DeviceContext* a_context);
		UavOverlapScope(ID3D11DeviceContext* a_context, bool a_wanted);
		~UavOverlapScope();
		UavOverlapScope(const UavOverlapScope&) = delete;
		UavOverlapScope& operator=(const UavOverlapScope&) = delete;

		/// True when the overlap is really open (the caller may then use its batched path).
		bool Active() const { return active; }

	private:
		ID3D11DeviceContext* context = nullptr;
		bool active = false;
	};

	/// NvAPI_D3D11_MultiDrawIndexedInstancedIndirect, resolved for completeness; see the 45c notes
	/// in GrassOptimizations.cpp for why grass does not use it. Returns false when unavailable/failed.
	bool MultiDrawIndexedInstancedIndirect(ID3D11DeviceContext* a_context, uint32_t a_drawCount, ID3D11Buffer* a_args, uint32_t a_offset, uint32_t a_stride);

	nlohmann::json Save();
	void Load(const nlohmann::json& a_json);
	void RestoreDefaults();

	/// Advanced > NVIDIA tab.
	void DrawSettings();
}
