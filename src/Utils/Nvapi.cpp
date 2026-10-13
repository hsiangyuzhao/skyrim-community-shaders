#include "Utils/Nvapi.h"

#include "Utils/UI.h"

#include <array>
#include <format>

// NVAPI is only used for its types; every entry point is resolved at runtime through the
// driver's nvapi_QueryInterface, so nothing is linked and a machine without an NVIDIA driver
// simply never finds nvapi64.dll.
#pragma warning(push, 0)
#include <nvShaderExtnEnums.h>
#include <nvapi.h>
#pragma warning(pop)

namespace Util::Nvapi
{
	namespace
	{
		// Function ids from nvapi_interface.h (same vendored SDK as nvapi.h).
		constexpr uint32_t kIdInitialize = 0x0150e828;
		constexpr uint32_t kIdIsNvShaderExtnOpCodeSupported = 0x5f68da40;
		constexpr uint32_t kIdSetNvShaderExtnSlot = 0x8e90bb9f;  // global, not the LocalThread variant
		constexpr uint32_t kIdBeginUAVOverlapEx = 0xba08208a;
		constexpr uint32_t kIdBeginUAVOverlap = 0x65b93ca8;
		constexpr uint32_t kIdEndUAVOverlap = 0x2216a357;
		constexpr uint32_t kIdMultiDrawInstancedIndirect = 0xd4e26bbf;
		constexpr uint32_t kIdMultiDrawIndexedInstancedIndirect = 0x59e890f9;

		using QueryInterfaceFn = void*(__cdecl*)(uint32_t);
		using InitializeFn = NvAPI_Status(__cdecl*)();
		using IsOpCodeSupportedFn = NvAPI_Status(__cdecl*)(IUnknown*, NvU32, bool*);
		using SetExtnSlotFn = NvAPI_Status(__cdecl*)(IUnknown*, NvU32);
		using BeginUAVOverlapExFn = NvAPI_Status(__cdecl*)(IUnknown*, NvU32);
		using BeginUAVOverlapFn = NvAPI_Status(__cdecl*)(IUnknown*);
		using EndUAVOverlapFn = NvAPI_Status(__cdecl*)(IUnknown*);
		using MultiDrawFn = NvAPI_Status(__cdecl*)(ID3D11DeviceContext*, NvU32, ID3D11Buffer*, NvU32, NvU32);

		struct Api
		{
			bool ok = false;
			std::string error;
			QueryInterfaceFn queryInterface = nullptr;
			IsOpCodeSupportedFn isOpCodeSupported = nullptr;
			SetExtnSlotFn setExtnSlot = nullptr;
			BeginUAVOverlapExFn beginUAVOverlapEx = nullptr;
			BeginUAVOverlapFn beginUAVOverlap = nullptr;
			EndUAVOverlapFn endUAVOverlap = nullptr;
			MultiDrawFn multiDrawInstancedIndirect = nullptr;
			MultiDrawFn multiDrawIndexedInstancedIndirect = nullptr;
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
				const auto q = result.queryInterface;
				result.isOpCodeSupported = reinterpret_cast<IsOpCodeSupportedFn>(q(kIdIsNvShaderExtnOpCodeSupported));
				result.setExtnSlot = reinterpret_cast<SetExtnSlotFn>(q(kIdSetNvShaderExtnSlot));
				result.beginUAVOverlapEx = reinterpret_cast<BeginUAVOverlapExFn>(q(kIdBeginUAVOverlapEx));
				result.beginUAVOverlap = reinterpret_cast<BeginUAVOverlapFn>(q(kIdBeginUAVOverlap));
				result.endUAVOverlap = reinterpret_cast<EndUAVOverlapFn>(q(kIdEndUAVOverlap));
				result.multiDrawInstancedIndirect = reinterpret_cast<MultiDrawFn>(q(kIdMultiDrawInstancedIndirect));
				result.multiDrawIndexedInstancedIndirect = reinterpret_cast<MultiDrawFn>(q(kIdMultiDrawIndexedInstancedIndirect));
				result.ok = true;
				return result;
			}();
			return api;
		}

		Settings settings;
		/// Saved with the settings. 1 = 45d (master switch default off, see Load).
		constexpr int kSettingsRevision = 1;

		constexpr size_t kItemCount = static_cast<size_t>(Item::kCount);
		/// Set by OnDeviceCreated (and DisableForSession); empty = available.
		std::array<std::string, kItemCount> itemReason = {
			"Waiting for the game's D3D11 device.",
			"Waiting for the game's D3D11 device.",
			"Waiting for the game's D3D11 device.",
			"Waiting for the game's D3D11 device.",
		};
		bool deviceSeen = false;
		bool slotSet = false;
		std::string slotStatus = "Not set yet.";

		size_t Index(Item a_item) { return static_cast<size_t>(a_item); }

		bool& Switch(Item a_item)
		{
			switch (a_item) {
			case Item::WarpAtomics:
				return settings.WarpAtomics;
			case Item::UavOverlap:
				return settings.UavOverlap;
			case Item::ShaderExtensions:
				return settings.ShaderExtensions;
			default:
				return settings.TimerHeatmap;
			}
		}

		struct OpcodeInfo
		{
			uint32_t opcode;
			const char* name;
		};

		/// Opcodes each shader-extension item needs (from nvShaderExtnEnums.h).
		std::vector<OpcodeInfo> RequiredOpcodes(Item a_item)
		{
			switch (a_item) {
			case Item::WarpAtomics:  // GrassCullingCS: NvBallot + NvGetLaneId + NvShfl
				return { { NV_EXTN_OP_VOTE_BALLOT, "vote.ballot" }, { NV_EXTN_OP_GET_LANE_ID, "lane id" }, { NV_EXTN_OP_SHFL, "shuffle" } };
			case Item::ShaderExtensions:  // ssrt_variance: NvAny; SPD: NvShflXor
				return { { NV_EXTN_OP_VOTE_ANY, "vote.any" }, { NV_EXTN_OP_SHFL_XOR, "shuffle xor" } };
			case Item::TimerHeatmap:  // ssrt_raymarch heatmap permutation: NvGetSpecial(global timer)
				return { { NV_EXTN_OP_GET_SPECIAL, "special register" } };
			default:
				return {};
			}
		}
	}

	Settings& GetSettings() { return settings; }

	bool Loaded() { return GetApi().ok; }

	const std::string& LoadError() { return GetApi().error; }

	void* QueryInterface(uint32_t a_id)
	{
		const Api& api = GetApi();
		return api.ok ? api.queryInterface(a_id) : nullptr;
	}

	void OnDeviceCreated(ID3D11Device* a_device)
	{
		if (deviceSeen || !a_device)
			return;
		deviceSeen = true;

		const Api& api = GetApi();
		if (!api.ok) {
			for (auto& reason : itemReason)
				reason = api.error;
			slotStatus = api.error;
			logger::info("[NVAPI] Driver extensions unavailable: {}", api.error);
			return;
		}

		// UAV overlap needs no shader support, only the context calls. The Ex form is required: it is
		// the one that waits for earlier work at the start of a scope (see UavOverlapScope).
		itemReason[Index(Item::UavOverlap)] = (api.beginUAVOverlapEx && api.endUAVOverlap) ?
		                                          std::string() :
		                                          std::string("The driver does not expose NvAPI_D3D11_BeginUAVOverlapEx / EndUAVOverlap.");

		// The extension slot is global for the device: every shader created after this call treats
		// u63 as the extension channel. It has to happen before any shader that uses it is created,
		// which is why this runs inside device creation (the disk cache and the shader-cache worker
		// threads only start much later).
		if (!api.setExtnSlot || !api.isOpCodeSupported) {
			slotStatus = "The driver does not expose NvAPI_D3D11_SetNvShaderExtnSlot.";
		} else if (const NvAPI_Status status = api.setExtnSlot(a_device, kShaderExtnSlot); status != NVAPI_OK) {
			slotStatus = std::format("NvAPI_D3D11_SetNvShaderExtnSlot(u{}) failed ({}); the game device may not be a native NVIDIA D3D11 device.", kShaderExtnSlot, static_cast<int>(status));
		} else {
			slotSet = true;
			slotStatus = std::format("Shader extension slot u{} set.", kShaderExtnSlot);
		}

		for (Item item : { Item::WarpAtomics, Item::ShaderExtensions, Item::TimerHeatmap }) {
			if (!slotSet) {
				itemReason[Index(item)] = slotStatus;
				continue;
			}
			std::string missing;
			for (const auto& op : RequiredOpcodes(item)) {
				bool supported = false;
				const NvAPI_Status status = api.isOpCodeSupported(a_device, op.opcode, &supported);
				if (status != NVAPI_OK || !supported)
					missing += missing.empty() ? op.name : std::string(", ") + op.name;
			}
			itemReason[Index(item)] = missing.empty() ? std::string() : "The driver does not support: " + missing + ".";
		}

		logger::info("[NVAPI] {} Warp atomics: {}. UAV overlap: {}. Shader extension use: {}. Timer heatmap: {}.", slotStatus,
			Available(Item::WarpAtomics) ? "yes" : itemReason[Index(Item::WarpAtomics)],
			Available(Item::UavOverlap) ? "yes" : itemReason[Index(Item::UavOverlap)],
			Available(Item::ShaderExtensions) ? "yes" : itemReason[Index(Item::ShaderExtensions)],
			Available(Item::TimerHeatmap) ? "yes" : itemReason[Index(Item::TimerHeatmap)]);
	}

	bool Available(Item a_item)
	{
		return a_item < Item::kCount && itemReason[Index(a_item)].empty();
	}

	std::string_view UnavailableReason(Item a_item)
	{
		return a_item < Item::kCount ? std::string_view(itemReason[Index(a_item)]) : std::string_view("Unknown item.");
	}

	bool Use(Item a_item)
	{
		return settings.Enabled && a_item < Item::kCount && Switch(a_item) && Available(a_item);
	}

	std::vector<std::pair<const char*, const char*>> ShaderDefines(Item a_item)
	{
		if (a_item == Item::UavOverlap || !Available(a_item))
			return {};
		return { { "NV_SHADER_EXTN", "1" } };
	}

	void DisableForSession(Item a_item, std::string a_reason)
	{
		if (a_item >= Item::kCount || !Available(a_item))
			return;
		logger::error("[NVAPI] {} - turned off for this session.", a_reason);
		itemReason[Index(a_item)] = std::move(a_reason);
	}

	UavOverlapScope::UavOverlapScope(ID3D11DeviceContext* a_context) :
		UavOverlapScope(a_context, true)
	{
	}

	UavOverlapScope::UavOverlapScope(ID3D11DeviceContext* a_context, bool a_wanted) :
		context(a_context)
	{
		if (!a_wanted || !context || !Use(Item::UavOverlap))
			return;
		const Api& api = GetApi();
		// Graphics + compute wait-for-idle at the start: the first dispatch in the scope must still see
		// everything issued before it (clears, the pass that produced its inputs).
		const NvAPI_Status status = api.beginUAVOverlapEx(context, static_cast<NvU32>(NVAPI_D3D_BEGIN_UAV_OVERLAP_GFX_WFI) | static_cast<NvU32>(NVAPI_D3D_BEGIN_UAV_OVERLAP_COMP_WFI));
		if (status != NVAPI_OK) {
			DisableForSession(Item::UavOverlap, std::format("NvAPI_D3D11_BeginUAVOverlapEx failed ({})", static_cast<int>(status)));
			return;
		}
		active = true;
	}

	UavOverlapScope::~UavOverlapScope()
	{
		if (!active)
			return;
		const NvAPI_Status status = GetApi().endUAVOverlap(context);
		if (status != NVAPI_OK)
			DisableForSession(Item::UavOverlap, std::format("NvAPI_D3D11_EndUAVOverlap failed ({})", static_cast<int>(status)));
	}

	bool MultiDrawIndexedInstancedIndirect(ID3D11DeviceContext* a_context, uint32_t a_drawCount, ID3D11Buffer* a_args, uint32_t a_offset, uint32_t a_stride)
	{
		const Api& api = GetApi();
		if (!settings.Enabled || !api.ok || !api.multiDrawIndexedInstancedIndirect || !a_context || !a_args)
			return false;
		return api.multiDrawIndexedInstancedIndirect(a_context, a_drawCount, a_args, a_offset, a_stride) == NVAPI_OK;
	}

	nlohmann::json Save()
	{
		return {
			{ "Revision", kSettingsRevision },
			{ "Enabled", settings.Enabled },
			{ "WarpAtomics", settings.WarpAtomics },
			{ "UavOverlap", settings.UavOverlap },
			{ "ShaderExtensions", settings.ShaderExtensions },
			{ "TimerHeatmap", settings.TimerHeatmap },
		};
	}

	void Load(const nlohmann::json& a_json)
	{
		if (!a_json.is_object())
			return;
		const auto read = [&](const char* a_key, bool& a_value) {
			if (a_json.contains(a_key) && a_json[a_key].is_boolean())
				a_value = a_json[a_key].get<bool>();
		};
		read("Enabled", settings.Enabled);
		// 45d: the master switch now defaults to off. Settings saved before that (no Revision, all
		// from 45c with it on) are switched off once; from then on the saved choice is respected.
		const bool current = a_json.contains("Revision") && a_json["Revision"].is_number_integer() &&
		                     a_json["Revision"].get<int>() >= kSettingsRevision;
		if (!current) {
			if (settings.Enabled)
				logger::info("[NVAPI] NVIDIA Driver Extensions switched off once (new default in 45d)");
			settings.Enabled = false;
		}
		read("WarpAtomics", settings.WarpAtomics);
		read("UavOverlap", settings.UavOverlap);
		read("ShaderExtensions", settings.ShaderExtensions);
		read("TimerHeatmap", settings.TimerHeatmap);
	}

	void RestoreDefaults()
	{
		settings = {};
	}

	void DrawSettings()
	{
		const auto statusLine = [](Item a_item) {
			if (!settings.Enabled) {
				ImGui::TextDisabled("    off (NVIDIA Driver Extensions is off)");
			} else if (!Available(a_item)) {
				ImGui::TextDisabled("    unavailable: %s", std::string(UnavailableReason(a_item)).c_str());
			} else if (!Switch(a_item)) {
				ImGui::TextDisabled("    off");
			} else {
				ImGui::TextDisabled("    in use");
			}
		};

		ImGui::SeparatorText("NVIDIA Driver Extensions");
		ImGui::Checkbox("NVIDIA Driver Extensions", &settings.Enabled);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Lets a few GPU passes use extra NVIDIA driver features that plain D3D11 does not have:\n"
				"grass culling counts its results in bigger batches, some independent passes may run side\n"
				"by side, and two small passes swap a slow group-wide step for a fast per-warp one.\n"
				"The picture should not change, only the timings. Compare with F9 (A/B Compare) and the\n"
				"Performance Overlay rows Grass Optimizations / Grass Culling, SSRT, Volumetric Shadows.\n"
				"Switching it never recompiles shaders. Without an NVIDIA GPU nothing happens.");

		if (!Loaded()) {
			ImGui::TextDisabled("Unavailable: %s", LoadError().c_str());
			return;
		}
		ImGui::TextDisabled("%s", slotStatus.c_str());

		if (ImGui::TreeNodeEx("Debug", ImGuiTreeNodeFlags_None)) {
			ImGui::BeginDisabled(!settings.Enabled);

			ImGui::Checkbox("Warp atomics (grass culling)", &settings.WarpAtomics);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Grass culling: the 32 threads of a warp reserve their output slots with one counter\n"
					"update instead of one each. Off: one counter update per surviving grass instance.");
			statusLine(Item::WarpAtomics);

			ImGui::Checkbox("UAV overlap", &settings.UavOverlap);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Lets the driver run compute passes back to back without waiting in between, only where\n"
					"they provably do not touch each other's outputs: the per-grass-type culling passes, and\n"
					"Volumetric Shadows' two shadow-map downsamples and its blur passes.");
			statusLine(Item::UavOverlap);

			ImGui::BeginDisabled();
			bool multiDraw = false;
			ImGui::Checkbox("Multi-draw", &multiDraw);
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Not used. Every grass draw has its own vertex, index and instance buffers, and every grass\n"
					"type its own textures set by the game right before its draw, so there is nothing to merge\n"
					"into one multi-draw call without rebuilding how grass buffers are stored. It would only save\n"
					"CPU time anyway, and the frame is GPU-bound.");
			ImGui::TextDisabled("    not used (see tooltip)");

			ImGui::Checkbox("Shader extension use (SSRT variance, grass Hi-Z)", &settings.ShaderExtensions);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"SSRT's variance pass decides per warp instead of with a group-wide counter, and the grass\n"
					"occlusion pyramid reduces 2x2 blocks with warp shuffles instead of shared memory.\n"
					"Same results either way.");
			statusLine(Item::ShaderExtensions);

			ImGui::Checkbox("Timer heatmap (SSRT diffuse ray march)", &settings.TimerHeatmap);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Diagnostic, off by default. Shows a small window with how long the SSRT diffuse ray march\n"
					"spends on each 8x8 block of the screen (blue = cheap, red = expensive), measured with the\n"
					"GPU's own clock. Costs a little while it is on. Needs SSRT on with Diffuse Sampling = Full\n"
					"and SHARC off.");
			statusLine(Item::TimerHeatmap);

			ImGui::EndDisabled();
			ImGui::TreePop();
		}
	}
}
