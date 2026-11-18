#include "UpstreamEngineFixes.h"

#include <charconv>
#include <imgui.h>
#include <immintrin.h>
#include <intrin.h>

#include "Globals.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/UI.h"

namespace UpstreamEngineFixes
{
	namespace
	{
		// ------------------------------------------------------------------ helpers

		/** @brief True if the bytes at a_address match a "41 B1 ?? ..." pattern (?? = any byte). */
		bool MatchesPattern(std::uintptr_t a_address, std::string_view a_pattern)
		{
			const auto* bytes = reinterpret_cast<const std::uint8_t*>(a_address);
			std::size_t i = 0;
			std::size_t pos = 0;
			while (pos < a_pattern.size()) {
				if (a_pattern[pos] == ' ') {
					++pos;
					continue;
				}
				if (pos + 2 > a_pattern.size())
					return false;
				const std::string_view token = a_pattern.substr(pos, 2);
				pos += 2;
				if (token != "??") {
					unsigned value = 0;
					if (std::from_chars(token.data(), token.data() + token.size(), value, 16).ec != std::errc{})
						return false;
					if (bytes[i] != static_cast<std::uint8_t>(value))
						return false;
				}
				++i;
			}
			return i > 0;
		}

		/** @brief True if a_callSite holds a rel32 CALL to a_target, i.e. no other plugin owns it. */
		bool IsCallTo(std::uintptr_t a_callSite, std::uintptr_t a_target)
		{
			if (*reinterpret_cast<const std::uint8_t*>(a_callSite) != 0xE8)
				return false;
			std::int32_t displacement{};
			std::memcpy(&displacement, reinterpret_cast<const void*>(a_callSite + 1), sizeof(displacement));
			return a_callSite + 5 + displacement == a_target;
		}

		bool IEquals(std::string_view a, std::string_view b)
		{
			return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
				return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
			});
		}

		enum class Status
		{
			NotTried,
			Installed,
			CodeMismatch,
			Skipped,
		};

		const char* StatusText(Status a_status)
		{
			switch (a_status) {
			case Status::Installed:
				return "active";
			case Status::CodeMismatch:
				return "not installed: unexpected game code (another mod?)";
			case Status::Skipped:
				return "not installed (see the log)";
			default:
				return "not installed";
			}
		}

		Status sunPollStatus = Status::NotTried;
		Status lightListStatus = Status::NotTried;
		Status overlayStatus = Status::NotTried;
		Status sunMaskStatus = Status::NotTried;
		Status lodRingStatus = Status::NotTried;

		// ------------------------------------------------------------------ A1: sun occlusion poll (#2901)

		/** @brief The poll's "flush" immediate (r9b = 1), 0 when the code did not verify. */
		std::uintptr_t sunPollFlushByte = 0;

		void ApplySunPoll()
		{
			if (!sunPollFlushByte)
				return;
			const std::uint8_t wanted = settings.SunOcclusionNoFlush ? 0 : 1;
			if (*reinterpret_cast<const std::uint8_t*>(sunPollFlushByte) != wanted)
				REL::safe_write(sunPollFlushByte, &wanted, 1);
		}

		void InstallSunPoll()
		{
			// Poll arguments as encoded on SE and AE: r9b = flush flag, rcx = renderer data, r8d = wait flag.
			constexpr std::string_view pollArguments = "41 B1 01 48 8D 0D ?? ?? ?? ?? 45 33 C0";
			constexpr std::uintptr_t pollArgumentsSize = 13;
			constexpr std::uintptr_t flushFlagOffset = 2;
			const auto pollCall = REL::RelocationID(99967, 106605).address() + REL::Relocate(0x42, 0x85);
			const auto args = pollCall - pollArgumentsSize;
			if (!MatchesPattern(args, pollArguments) || !IsCallTo(pollCall, REL::RelocationID(75569, 77370).address())) {
				logger::warn("[Engine Fixes] Sun Occlusion Poll Fix not applied: unexpected code at the sun occlusion poll");
				sunPollStatus = Status::CodeMismatch;
				return;
			}
			sunPollFlushByte = args + flushFlagOffset;
			sunPollStatus = Status::Installed;
			ApplySunPoll();
		}

		// ------------------------------------------------------------------ A2: light list search (#3031)

		bool HasAvx2()
		{
			static const bool supported = [] {
				int regs[4]{};
				__cpuidex(regs, 0, 0);
				if (regs[0] < 7)
					return false;
				__cpuidex(regs, 1, 0);
				constexpr int kOsXsave = 1 << 27;
				constexpr int kAvx = 1 << 28;
				if ((regs[2] & kOsXsave) == 0 || (regs[2] & kAvx) == 0)
					return false;
				constexpr unsigned long long kXmmYmmState = 0x6;
				if ((_xgetbv(0) & kXmmYmmState) != kXmmYmmState)
					return false;
				__cpuidex(regs, 7, 0);
				constexpr int kAvx2 = 1 << 5;
				return (regs[1] & kAvx2) != 0;
			}();
			return supported;
		}

		bool ContainsPointerScalar(const void* const* a_data, std::uint32_t a_size, const void* a_needle)
		{
			return std::find(a_data, a_data + a_size, a_needle) != a_data + a_size;
		}

#pragma warning(push)
#pragma warning(disable: 4752)  // AVX2 is gated at runtime by HasAvx2, not by /arch
		bool ContainsPointerAvx2(const void* const* a_data, std::uint32_t a_size, const void* a_needle)
		{
			constexpr std::uint32_t kLanes = 4;
			const auto needle = _mm256_set1_epi64x(static_cast<long long>(reinterpret_cast<std::uintptr_t>(a_needle)));
			std::uint32_t i = 0;
			for (; i + kLanes <= a_size; i += kLanes) {
				const auto equal = _mm256_cmpeq_epi64(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(a_data + i)), needle);
				if (_mm256_movemask_pd(_mm256_castsi256_pd(equal)))
					return true;
			}
			return ContainsPointerScalar(a_data + i, a_size - i, a_needle);
		}
#pragma warning(pop)

		bool ContainsPointer(const void* const* a_data, std::uint32_t a_size, const void* a_needle)
		{
			return HasAvx2() ? ContainsPointerAvx2(a_data, a_size, a_needle) : ContainsPointerScalar(a_data, a_size, a_needle);
		}

		struct AddLight
		{
			// Engine sentinel for "not rebuilding": AddLight then only appends lights it does not find.
			static constexpr std::int32_t FenceUnset = -1;

			static void thunk(RE::BSShaderPropertyLightData* a_this, RE::BSLight* a_light)
			{
				if (settings.LightListSearch && a_this->lightListFence == FenceUnset) {
					const auto& lights = a_this->lights;
					if (ContainsPointer(reinterpret_cast<const void* const*>(lights.data()), lights.size(), a_light))
						return;
				}
				func(a_this, a_light);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void InstallLightList()
		{
			// Prologue through the lightListFence load, identical on SE and AE.
			constexpr std::string_view addLightPrologue =
				"48 89 54 24 10 41 56 48 83 EC 50 48 C7 44 24 30 FE FF FF FF 48 89 5C 24 60 48 89 74 24 70 48 89 7C 24 78 48 8B FA 48 8B D9 44 8B 51 18";
			const REL::RelocationID addLight(100990, 107777);
			if (!MatchesPattern(addLight.address(), addLightPrologue)) {
				logger::warn("[Engine Fixes] Light List Search Fix not applied: unexpected code at BSShaderPropertyLightData::AddLight");
				lightListStatus = Status::CodeMismatch;
				return;
			}
			stl::detour_thunk<AddLight>(addLight);
			lightListStatus = Status::Installed;
			logger::info("[Engine Fixes] Light List Search Fix: {} search", HasAvx2() ? "AVX2" : "scalar (no AVX2 on this CPU)");
		}

		// ------------------------------------------------------------------ A3: empty RaceMenu overlays (#3050)

		// RaceMenu's sDefaultTexture when the setting is unset.
		constexpr std::string_view RaceMenuDefaultTexture = "textures\\actors\\character\\overlays\\default.dds";
		std::string emptyOverlayTexture;

		// RaceMenu reads this one setting through SKSE's GetConfigOption, so from skse.ini; the copy in skee64.ini is never read.
		std::string ReadRaceMenuDefaultTexture()
		{
			const auto iniPath = Util::PathHelpers::GetDataPath() / "SKSE" / "skse.ini";
			std::array<wchar_t, MAX_PATH> value{};
			GetPrivateProfileStringW(L"Overlays/Data", L"sDefaultTexture", L"", value.data(), static_cast<DWORD>(value.size()), iniPath.c_str());
			return value[0] != L'\0' ? Util::WStringToString(value.data()) : std::string(RaceMenuDefaultTexture);
		}

		// RaceMenu's default iAlphaFlags 4845 blends SrcAlpha / InvSrcAlpha, so alpha 0 leaves the target untouched.
		// Alpha testing alone proves nothing: at RaceMenu's threshold 0 the engine's test passes alpha 0.
		bool BlendsAwayZeroAlpha(const RE::NiAlphaProperty* a_alpha)
		{
			using enum RE::NiAlphaProperty::AlphaFunction;
			return a_alpha && a_alpha->GetAlphaBlending() && a_alpha->GetSrcBlendMode() == kSrcAlpha && a_alpha->GetDestBlendMode() == kInvSrcAlpha;
		}

		std::string_view GetDiffusePath(RE::BSLightingShaderProperty* a_property)
		{
			auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_property->material);
			const char* path = material && material->textureSet ? material->textureSet->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse) : nullptr;
			return path ? path : "";
		}

		// Ordered cheapest first: most lighting geometry fails the flag test before any pointer is followed.
		// RaceMenu's overlay nifs name the texture "Textures\Actors\...", its built-in default "textures\actors\...", so case is ignored.
		bool IsEmptyOverlay(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry)
		{
			if (!a_property->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSkinned))
				return false;
			const auto* alpha = static_cast<const RE::NiAlphaProperty*>(a_geometry->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kProperty].get());
			return BlendsAwayZeroAlpha(alpha) && IEquals(GetDiffusePath(a_property), emptyOverlayTexture);
		}

		/** @brief Clear returns each pass to the engine's pool; dropping the head would leak them. */
		void ClearPasses(RE::BSShaderProperty::RenderPassArray& a_list)
		{
			while (a_list.head != nullptr) {
				RE::BSRenderPass* next = a_list.head->next;
				a_list.head->ClearRenderPass();
				a_list.head = next;
			}
		}

		// ------------------------------------------------------------------ A4: sun shadow mask (#2925)

		constexpr std::uint32_t LightMaskBits = 32;
		/** @brief Light mask bits the sun shadow light occupies, zero when the fix is inactive. Written once per frame. */
		std::atomic<std::uint32_t> sunLightMask = 0;
		RE::BSShaderAccumulator** mainAccumulator = nullptr;
		const std::uint8_t* sunShadowSkipped = nullptr;
		const float* shadowDistance = nullptr;
		bool sunMaskReady = false;
		bool overlayReady = false;

		/** @brief BSShaderAccumulator::eyePosition on SE/AE (0x58 + 0x114). */
		constexpr std::uintptr_t AccumulatorEyePositionOffset = 0x16C;

		void UpdateSunMask()
		{
			std::uint32_t mask = 0;
			const auto* state = globals::game::smState;
			if (sunMaskReady && settings.SunShadowMask && state) {
				const auto* shadowSceneNode = state->shadowSceneNode[0];
				if (shadowSceneNode && !state->interior && !*sunShadowSkipped) {
					const auto& runtimeData = shadowSceneNode->GetRuntimeData();
					const auto* sunLight = static_cast<const RE::BSShadowLight*>(runtimeData.shadowDirLight);
					if (sunLight) {
						// Each cascade holds its own slot, so the sun can own several bits.
						const auto& shadowLights = runtimeData.shadowLightsAccum;
						const std::uint32_t count = std::min<std::uint32_t>(shadowLights.size(), LightMaskBits);
						for (std::uint32_t slot = 0; slot < count; ++slot) {
							if (shadowLights[slot] == sunLight)
								mask |= 1u << slot;
						}
					}
				}
			}
			sunLightMask.store(mask, std::memory_order_relaxed);
		}

		bool ShouldSetSunBits(const RE::BSLightingShaderProperty* a_property, const RE::BSGeometry* a_geometry, std::uint32_t a_renderMode, const RE::BSShaderAccumulator* a_accumulator, std::uint32_t a_mask)
		{
			using enum RE::BSShaderProperty::EShaderPropertyFlag;

			constexpr std::uint32_t RenderModeNormal = 0;
			if (!a_mask || !a_geometry || a_renderMode != RenderModeNormal || !a_accumulator || a_accumulator != *mainAccumulator)
				return false;

			const auto* lightData = a_property->lightData;
			// LOD and cloud geometry take their light from the sun light, not the sun shadow slots.
			if (!lightData || (lightData->unk1C & a_mask) || !a_property->flags.all(kReceiveShadows) || a_property->flags.any(kLODLandscape, kLODObjects, kCloudLOD, kHDLODObjects))
				return false;

			const auto& eye = *reinterpret_cast<const RE::NiPoint3*>(reinterpret_cast<std::uintptr_t>(a_accumulator) + AccumulatorEyePositionOffset);
			const auto& bound = a_geometry->worldBound;
			return (bound.center - eye).Length() - bound.radius < *shadowDistance;
		}

		/** @brief One hook on BSLightingShaderProperty::GetRenderPasses (vtable 0x2A) for both A4 and A3. */
		struct GetRenderPasses
		{
			static RE::BSShaderProperty::RenderPassArray* thunk(RE::BSLightingShaderProperty* a_property, RE::BSGeometry* a_geometry, std::uint32_t a_renderMode, RE::BSShaderAccumulator* a_accumulator)
			{
				if (sunMaskReady) {
					const std::uint32_t mask = sunLightMask.load(std::memory_order_relaxed);
					if (ShouldSetSunBits(a_property, a_geometry, a_renderMode, a_accumulator, mask))
						// Atomic so this worker write cannot drop bits the render thread's cascade cull sets concurrently.
						std::atomic_ref<std::uint32_t>(a_property->lightData->unk1C).fetch_or(mask);
				}
				if (overlayReady && settings.EmptyOverlayCull && a_geometry && IsEmptyOverlay(a_property, a_geometry)) {
					ClearPasses(a_property->renderPassList);
					return &a_property->renderPassList;
				}
				return func(a_property, a_geometry, a_renderMode, a_accumulator);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		void InstallRenderPassHook()
		{
			emptyOverlayTexture = ReadRaceMenuDefaultTexture();
			overlayReady = true;
			overlayStatus = Status::Installed;
			logger::info("[Engine Fixes] Empty Overlay Cull Fix skips skinned SrcAlpha/InvSrcAlpha shapes whose diffuse is {}", emptyOverlayTexture);

			// A plugin that already tags the sun bits itself (official CS lists ShadowFixes.dll as incompatible with this fix).
			if (GetModuleHandleW(L"ShadowFixes.dll")) {
				logger::warn("[Engine Fixes] Sun Shadow Mask Fix not applied: ShadowFixes.dll is loaded");
				sunMaskStatus = Status::Skipped;
			} else {
				mainAccumulator = reinterpret_cast<RE::BSShaderAccumulator**>(REL::RelocationID(528064, 415009).address());
				sunShadowSkipped = reinterpret_cast<const std::uint8_t*>(REL::RelocationID(528095, 415040).address());
				shadowDistance = reinterpret_cast<const float*>(REL::RelocationID(528314, 415263).address());
				sunMaskReady = true;
				sunMaskStatus = Status::Installed;
			}

			stl::write_vfunc<0x2A, GetRenderPasses>(RE::VTABLE_BSLightingShaderProperty[0]);
		}

		// ------------------------------------------------------------------ A5: LOD blend ring (#2894)

		// Band radii mirror the landscape vertex shader in Lighting.hlsl.
		constexpr float BandOuterRadius = 9625.59961f;
		constexpr float BandInnerRadius = BandOuterRadius - 1.0f / 0.000375600968f;
		constexpr float HalfCellSize = 2048.0f;
		constexpr float SlideSeconds = 0.25f;

		/** @brief Per-axis distance the ring center can move off the grid center while the band stays inside the loaded cells. */
		constexpr float LeanSlack(std::uint32_t a_gridsToLoad)
		{
			return std::max(0.0f, static_cast<float>(a_gridsToLoad) * HalfCellSize - BandOuterRadius);
		}

		// On 5x5 the engine skips LOD blending on the plus-shaped inner cells, so the leaning band must stay clear of them.
		static_assert(BandInnerRadius - LeanSlack(5) > 3.0f * HalfCellSize);

		struct LodRing
		{
			bool* blendInProgress = nullptr;
			RE::Setting* gridsToLoad = nullptr;
			float2 center;
			float2 gridCenter;
			float2 slideOrigin;
			float slideProgress = 0.0f;
			bool sliding = false;
			bool hasCenter = false;
		} lodRing;

		void EndSlide()
		{
			*lodRing.blendInProgress = false;
			lodRing.sliding = false;
		}

		/** @brief Hands the ring back to the engine: its own grid center, the game-time lerp finished. */
		void ReleaseLodRing()
		{
			if (!lodRing.hasCenter)
				return;
			if (lodRing.sliding)
				EndSlide();
			if (auto* state = globals::game::smState) {
				state->oldGridArrayCenter = state->gridArrayCenter = RE::NiPoint2(lodRing.gridCenter.x, lodRing.gridCenter.y);
				state->gridArrayLerpStart = 0.0f;
			}
			lodRing.hasCenter = false;
		}

		void UpdateLodRing()
		{
			if (!lodRing.blendInProgress)
				return;
			if (!settings.LODBlendRing) {
				ReleaseLodRing();
				return;
			}
			auto* tes = RE::TES::GetSingleton();
			auto* player = RE::PlayerCharacter::GetSingleton();
			if (!tes || !player || tes->interiorCell || !tes->GetRuntimeData2().worldSpace || !globals::game::smState) {
				if (lodRing.sliding)
					EndSlide();
				lodRing.hasCenter = false;
				return;
			}

			auto& state = *globals::game::smState;
			const float2 engineCenter(state.gridArrayCenter.x, state.gridArrayCenter.y);
			if (!lodRing.hasCenter || engineCenter != lodRing.center) {
				lodRing.gridCenter = engineCenter;
				// The engine writes old == new on loads, teleports and multi-cell jumps.
				lodRing.sliding = lodRing.hasCenter && float2(state.oldGridArrayCenter.x, state.oldGridArrayCenter.y) != engineCenter;
				if (lodRing.sliding) {
					lodRing.slideOrigin = lodRing.center;
					lodRing.slideProgress = 0.0f;
				} else
					EndSlide();
				lodRing.hasCenter = true;
			}

			if (!lodRing.gridsToLoad)
				lodRing.gridsToLoad = RE::GetINISetting("uGridsToLoad:General");
			const float slack = lodRing.gridsToLoad ? LeanSlack(lodRing.gridsToLoad->data.u) : 0.0f;
			const auto position = player->GetPosition();
			float2 lean = float2(position.x, position.y) - lodRing.gridCenter;
			lean.Clamp(float2(-slack, -slack), float2(slack, slack));
			const float2 desired = lodRing.gridCenter + lean;

			if (lodRing.sliding) {
				lodRing.slideProgress = std::min(1.0f, lodRing.slideProgress + RE::GetSecondsSinceLastFrame() / SlideSeconds);
				lodRing.center = float2::SmoothStep(lodRing.slideOrigin, desired, lodRing.slideProgress);
				if (lodRing.slideProgress >= 1.0f)
					EndSlide();
			} else
				lodRing.center = desired;

			state.oldGridArrayCenter = state.gridArrayCenter = RE::NiPoint2(lodRing.center.x, lodRing.center.y);
			// Pins the renderer's own game-time lerp at t = 0 so it never releases blendInProgress early.
			state.gridArrayLerpStart = std::numeric_limits<float>::max();
		}

		void InstallLodRing()
		{
			// Raised by TES on every grid shift; TES defers its per-cell LOD blend reflag until it clears.
			lodRing.blendInProgress = reinterpret_cast<bool*>(REL::RelocationID(527641, 414593).address());
			lodRingStatus = Status::Installed;
		}

		struct Item
		{
			bool Settings::*field;
			const char* key;
			const char* label;
			Status* status;
			const char* tooltip;
		};

		constexpr Item kItems[] = {
			{ &Settings::SunOcclusionNoFlush, "SunOcclusionNoFlush", "Sun glare check: no mid-frame GPU flush", &sunPollStatus,
				"The game checks every frame whether the sun is hidden (for the sun glare). When the answer is not ready yet it\n"
				"used to push the queued GPU work out early, which can unsettle frame pacing. Now it just asks again next frame:\n"
				"the glare may react one frame later. (official CS #2901)" },
			{ &Settings::LightListSearch, "LightListSearch", "Faster light list search (AVX2)", &lightListStatus,
				"When the game attaches lights to objects it checks each light against the object's list one by one. This\n"
				"answers the common \"already attached\" case with a vectorised search. Saves CPU in towns and lit interiors.\n"
				"No visible change. (official CS #3031)" },
			{ &Settings::EmptyOverlayCull, "EmptyOverlayCull", "Skip empty RaceMenu overlay slots", &overlayStatus,
				"RaceMenu adds every overlay slot (tattoos, paint) as a full copy of the body, even when the slot is empty; the\n"
				"empty ones still ran the full lighting shader and blended to nothing. Those are now skipped. No visible change.\n"
				"(official CS #3050)" },
			{ &Settings::SunShadowMask, "SunShadowMask", "No one-frame full-sunlit flash on new objects", &sunMaskStatus,
				"Objects that come into view could be drawn for one frame without the sun shadow (shadow map and ray-traced\n"
				"sun shadow alike): a brief bright flash when turning the camera. They now get the sun shadow from the first\n"
				"frame. (official CS #2925)" },
			{ &Settings::LODBlendRing, "LODBlendRing", "Distant terrain blend ring follows the frame clock", &lodRingStatus,
				"The ring where near terrain blends into distant LOD terrain moved on game time: it stuck when time was\n"
				"frozen and jumped at midnight or when crossing cells. It now slides smoothly (0.25 s) and leans toward you.\n"
				"(official CS #2894)" },
		};
	}

	void Install()
	{
		if (REL::Module::IsVR()) {
			logger::info("[Engine Fixes] (48n) upstream engine fixes are flat-only; none installed in VR");
			for (auto* s : { &sunPollStatus, &lightListStatus, &overlayStatus, &sunMaskStatus, &lodRingStatus })
				*s = Status::Skipped;
			return;
		}
		InstallSunPoll();
		InstallLightList();
		InstallRenderPassHook();
		InstallLodRing();
		logger::info("[Engine Fixes] (48n) {}", Describe());
	}

	void OnFrame()
	{
		if (sunMaskReady)
			UpdateSunMask();
		UpdateLodRing();
	}

	void Load(const json& a_json)
	{
		settings = {};
		if (a_json.is_object()) {
			for (const auto& item : kItems)
				if (a_json.contains(item.key) && a_json[item.key].is_boolean())
					settings.*item.field = a_json[item.key].get<bool>();
		}
		ApplySunPoll();
	}

	json Save()
	{
		json j;
		for (const auto& item : kItems)
			j[item.key] = settings.*item.field;
		return j;
	}

	std::string Describe()
	{
		std::string s;
		for (const auto& item : kItems) {
			if (!s.empty())
				s += " ";
			s += std::format("{}{}{}", item.key, settings.*item.field ? "+" : "-", *item.status == Status::Installed ? "" : "(n/a)");
		}
		return s;
	}

	void DrawMenu()
	{
		ImGui::Spacing();
		ImGui::SeparatorText("Engine Fixes (48n)");
		ImGui::TextWrapped("Small engine fixes ported from the official Community Shaders. Each works on its own and takes effect at once.");
		for (const auto& item : kItems) {
			bool value = settings.*item.field;
			const bool installed = *item.status == Status::Installed;
			if (ImGui::Checkbox(item.label, &value)) {
				settings.*item.field = value;
				if (item.field == &Settings::SunOcclusionNoFlush)
					ApplySunPoll();
			}
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(item.tooltip);
			if (!installed) {
				ImGui::SameLine();
				ImGui::TextDisabled("(%s)", StatusText(*item.status));
			}
		}
	}
}
