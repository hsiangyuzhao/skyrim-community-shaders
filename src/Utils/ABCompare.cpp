#include "Utils/ABCompare.h"

#include "Feature.h"
#include "Features/PerformanceOverlay.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/PostProcessing.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Batch39Engine.h"
#include "Utils/FileSystem.h"
#include "Utils/UI.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <imgui.h>

using json = nlohmann::json;

namespace ABCompare
{
	namespace
	{
		using Clock = std::chrono::steady_clock;

		constexpr double kDebounceSeconds = 0.3;
		constexpr double kTagSeconds = 2.0;
		constexpr double kTagFadeSeconds = 0.5;
		constexpr const char* kGlobalName = "Global";
		constexpr const char* kFileName = "SettingsAB.json";

		// Not part of the picture: overlay layout, debugging tools, VR plumbing.
		constexpr const char* kExcludedFeatures[] = { "Performance Overlay", "RenderDoc", "VR", "Weather Picker" };

		struct SlotData
		{
			bool stored = false;
			json global = json::object();    ///< Advanced / General / Replace Original Shaders / Disable at Boot
			json features = json::object();  ///< GetName() -> that feature's SaveSettings() output
		};

		struct DiffRow
		{
			std::string feature;
			std::string key;
			std::string a;
			std::string b;
			std::string note;  ///< non-empty: needs a restart / not applied live
		};

		SlotData slotA;
		SlotData slotB;
		Slot active = Slot::None;
		std::string scope;  ///< empty = all settings, kGlobalName, or a feature's GetName()

		bool pendingToggle = false;
		bool loadedFromFile = false;
		Clock::time_point lastSwitch{};
		Clock::time_point tagShownAt{};
		bool tagVisible = false;
		std::string tagText;
		uint64_t writtenVersion = 0;
		std::string statusText;

		uint64_t slotsVersion = 1;
		uint64_t diffVersion = 0;
		std::string diffScope = "\x01";
		std::vector<DiffRow> diffRows;

		SlotData& Get(Slot a_slot) { return a_slot == Slot::B ? slotB : slotA; }
		const char* SlotName(Slot a_slot) { return a_slot == Slot::A ? "A" : (a_slot == Slot::B ? "B" : "-"); }
		std::string ScopeLabel() { return scope.empty() ? std::string("All settings") : scope; }

		bool IsExcluded(const std::string& a_name)
		{
			return std::ranges::any_of(kExcludedFeatures, [&](const char* n) { return a_name == n; });
		}

		/// Loaded, comparable features (in registration order).
		std::vector<Feature*> ComparableFeatures()
		{
			std::vector<Feature*> out;
			for (auto* feature : Feature::GetFeatureList()) {
				if (feature && feature->loaded && !IsExcluded(feature->GetName()))
					out.push_back(feature);
			}
			return out;
		}

		bool InScope(const std::string& a_name) { return scope.empty() || scope == a_name; }

		std::filesystem::path FilePath() { return Util::PathHelpers::GetSettingsUserPath().parent_path() / kFileName; }

		// ---- capture -------------------------------------------------------------------------

		json CaptureFeature(Feature* a_feature)
		{
			json j = json::object();
			// Upscaling::SaveSettings also writes bUseTAA to SkyrimPrefs.ini; a snapshot must not.
			if (a_feature == &globals::features::upscaling) {
				globals::features::upscaling.SaveSettingsValues(j);
				return j;
			}
			// Post Processing applies a load one frame late; until then its live values are the
			// old ones and the queued object is what will be live.
			if (a_feature == &globals::features::postProcessing && !globals::features::postProcessing.pendingSettings.empty())
				return globals::features::postProcessing.pendingSettings;
			a_feature->SaveSettings(j);
			return j;
		}

		json CaptureGlobal()
		{
			auto* state = globals::state;
			json g = json::object();
			g["Advanced"] = {
				{ "Shader Defines", state->shaderDefinesString },
				{ "Engine", Batch39Engine::Save() },
			};
			g["General"] = { { "Enable Shaders", globals::shaderCache->IsEnabled() } };
			json classes = json::object();
			State::ForEachShaderTypeWithIndex([&](auto type, int classIndex) {
				classes[std::string(magic_enum::enum_name(type))] = state->enabledClasses[classIndex];
			});
			g["Replace Original Shaders"] = classes;
			json disabled = json::object();
			for (const auto& [name, isDisabled] : state->GetDisabledFeatures())
				disabled[name] = isDisabled;
			g["Disable at Boot"] = disabled;
			return g;
		}

		/// Live settings into a slot: everything, or only the current scope.
		void CaptureInto(SlotData& a_slot, bool a_scopeOnly)
		{
			if (!a_scopeOnly || scope.empty()) {
				a_slot.global = CaptureGlobal();
				a_slot.features = json::object();
				for (auto* feature : ComparableFeatures())
					a_slot.features[feature->GetName()] = CaptureFeature(feature);
			} else if (scope == kGlobalName) {
				a_slot.global = CaptureGlobal();
			} else {
				for (auto* feature : ComparableFeatures()) {
					if (feature->GetName() == scope)
						a_slot.features[scope] = CaptureFeature(feature);
				}
			}
			a_slot.stored = true;
		}

		// ---- persistence ---------------------------------------------------------------------

		json SlotToJson(const SlotData& a_slot)
		{
			if (!a_slot.stored)
				return nullptr;
			return { { "Global", a_slot.global }, { "Features", a_slot.features } };
		}

		void SlotFromJson(SlotData& a_slot, const json& a_json)
		{
			a_slot = {};
			if (!a_json.is_object())
				return;
			if (a_json.contains("Global") && a_json["Global"].is_object())
				a_slot.global = a_json["Global"];
			if (a_json.contains("Features") && a_json["Features"].is_object())
				a_slot.features = a_json["Features"];
			a_slot.stored = true;
		}

		void WriteFile()
		{
			if (!globals::menu->GetSettings().ABComparePersist || writtenVersion == slotsVersion)
				return;
			writtenVersion = slotsVersion;
			const auto path = FilePath();
			try {
				json j = { { "Version", 1 }, { "A", SlotToJson(slotA) }, { "B", SlotToJson(slotB) } };
				std::ofstream o(path, std::ios::trunc);
				if (!o.is_open()) {
					logger::warn("[A/B Compare] Cannot write {}", path.string());
					return;
				}
				o << j.dump();
			} catch (const std::exception& e) {
				logger::warn("[A/B Compare] Writing {} failed: {}", path.string(), e.what());
			}
		}

		void EnsureLoaded()
		{
			if (loadedFromFile)
				return;
			loadedFromFile = true;
			if (!globals::menu->GetSettings().ABComparePersist)
				return;
			const auto path = FilePath();
			std::error_code ec;
			if (!std::filesystem::exists(path, ec))
				return;
			try {
				std::ifstream i(path);
				json j;
				i >> j;
				if (j.contains("A"))
					SlotFromJson(slotA, j["A"]);
				if (j.contains("B"))
					SlotFromJson(slotB, j["B"]);
				++slotsVersion;
				writtenVersion = slotsVersion;
				logger::info("[A/B Compare] Slots loaded from {} (A: {}, B: {})", path.string(), slotA.stored, slotB.stored);
			} catch (const std::exception& e) {
				logger::warn("[A/B Compare] Ignoring unreadable {}: {}", path.string(), e.what());
				slotA = {};
				slotB = {};
			}
		}

		// ---- restart-only keys ---------------------------------------------------------------

		std::string RestartNote(const std::string& a_feature, const std::string& a_key)
		{
			if (a_feature == kGlobalName) {
				if (a_key.starts_with("Disable at Boot"))
					return "restart only - not switched";
				if (a_key == "Advanced/Shader Defines")
					return "compute shaders need a restart";
				return {};
			}
			if (a_feature == "Upscaling") {
				if (a_key == "frameGenerationBackend" || a_key == "streamlineLogLevel")
					return "needs a restart";
				if (a_key == "frameGenerationMode" && globals::features::upscaling.GetFrameGenerationBackend() != Upscaling::FrameGenerationBackend::kDLSSG)
					return "FSR frame generation: may need a restart";
			}
			return {};
		}

		// ---- diff ----------------------------------------------------------------------------

		std::string FormatValue(const json& a_value)
		{
			if (a_value.is_null())
				return "-";
			if (a_value.is_boolean())
				return a_value.get<bool>() ? "on" : "off";
			if (a_value.is_number_float())
				return std::format("{:.4g}", a_value.get<double>());
			if (a_value.is_number())
				return a_value.dump();
			if (a_value.is_string()) {
				auto s = a_value.get<std::string>();
				return s.size() > 48 ? s.substr(0, 45) + "..." : s;
			}
			if (a_value.is_array()) {
				std::string s = "[";
				for (size_t i = 0; i < a_value.size(); ++i) {
					if (i)
						s += ", ";
					s += FormatValue(a_value[i]);
				}
				s += "]";
				return s.size() > 60 ? s.substr(0, 57) + "..." : s;
			}
			auto s = a_value.dump();
			return s.size() > 60 ? s.substr(0, 57) + "..." : s;
		}

		bool IsSmallPrimitiveArray(const json& a_value)
		{
			if (!a_value.is_array() || a_value.size() > 4)
				return false;
			return std::ranges::all_of(a_value, [](const json& e) { return e.is_primitive(); });
		}

		void Flatten(const json& a_value, const std::string& a_prefix, std::map<std::string, json>& a_out)
		{
			if (a_value.is_object()) {
				for (auto& [k, v] : a_value.items())
					Flatten(v, a_prefix.empty() ? k : a_prefix + "/" + k, a_out);
			} else if (a_value.is_array() && !IsSmallPrimitiveArray(a_value)) {
				for (size_t i = 0; i < a_value.size(); ++i)
					Flatten(a_value[i], std::format("{}[{}]", a_prefix, i), a_out);
			} else {
				a_out[a_prefix] = a_value;
			}
		}

		void DiffSection(const std::string& a_feature, const json& a_a, const json& a_b)
		{
			std::map<std::string, json> fa, fb;
			Flatten(a_a, "", fa);
			Flatten(a_b, "", fb);
			auto add = [&](const std::string& key, const json* va, const json* vb) {
				DiffRow row{ a_feature, key, va ? FormatValue(*va) : "-", vb ? FormatValue(*vb) : "-", RestartNote(a_feature, key) };
				diffRows.push_back(std::move(row));
			};
			for (auto& [k, va] : fa) {
				auto it = fb.find(k);
				if (it == fb.end())
					add(k, &va, nullptr);
				else if (it->second != va)
					add(k, &va, &it->second);
			}
			for (auto& [k, vb] : fb) {
				if (!fa.contains(k))
					add(k, nullptr, &vb);
			}
		}

		void RebuildDiffIfNeeded()
		{
			if (diffVersion == slotsVersion && diffScope == scope)
				return;
			diffVersion = slotsVersion;
			diffScope = scope;
			diffRows.clear();
			if (!slotA.stored || !slotB.stored)
				return;
			if (InScope(kGlobalName))
				DiffSection(kGlobalName, slotA.global, slotB.global);
			for (auto* feature : ComparableFeatures()) {
				const std::string name = feature->GetName();
				if (!InScope(name))
					continue;
				const json empty = json::object();
				const json& a = slotA.features.contains(name) ? slotA.features[name] : empty;
				const json& b = slotB.features.contains(name) ? slotB.features[name] : empty;
				if (a != b)
					DiffSection(name, a, b);
			}
		}

		// ---- apply ---------------------------------------------------------------------------

		/// Applies the slot's in-scope settings. Returns the number of sections that changed.
		int Apply(const SlotData& a_slot)
		{
			int changed = 0;
			auto* state = globals::state;
			auto shaderCache = globals::shaderCache;

			if (InScope(kGlobalName) && a_slot.global.is_object()) {
				const json& g = a_slot.global;
				const json live = CaptureGlobal();
				if (g != live) {
					++changed;
					if (g.contains("Advanced") && g["Advanced"].is_object()) {
						const json& adv = g["Advanced"];
						if (adv.contains("Shader Defines") && adv["Shader Defines"].is_string()) {
							const auto defines = adv["Shader Defines"].get<std::string>();
							if (defines != state->shaderDefinesString) {
								state->SetDefines(defines);
								shaderCache->Clear();
							}
						}
						if (adv.contains("Engine"))
							Batch39Engine::Load(adv["Engine"]);
					}
					if (g.contains("General") && g["General"].is_object()) {
						const json& gen = g["General"];
						if (gen.contains("Enable Shaders") && gen["Enable Shaders"].is_boolean() &&
							gen["Enable Shaders"].get<bool>() != shaderCache->IsEnabled())
							shaderCache->SetEnabled(gen["Enable Shaders"].get<bool>());
					}
					if (g.contains("Replace Original Shaders") && g["Replace Original Shaders"].is_object()) {
						const json& classes = g["Replace Original Shaders"];
						State::ForEachShaderTypeWithIndex([&](auto type, int classIndex) {
							const std::string name(magic_enum::enum_name(type));
							if (classes.contains(name) && classes[name].is_boolean())
								state->enabledClasses[classIndex] = classes[name].get<bool>();
						});
					}
					// "Disable at Boot" is deliberately not applied: it only acts at the next start.
				}
			}

			for (auto* feature : ComparableFeatures()) {
				const std::string name = feature->GetName();
				if (!InScope(name) || !a_slot.features.contains(name))
					continue;
				const json& target = a_slot.features[name];
				if (!target.is_object())
					continue;
				json live = CaptureFeature(feature);
				if (live == target)
					continue;  // identical: no LoadSettings, so nothing is rebuilt or recompiled
				++changed;
				json copy = target;  // LoadSettings takes a non-const reference
				try {
					feature->LoadSettings(copy);
				} catch (const std::exception& e) {
					logger::warn("[A/B Compare] {}: slot settings rejected ({}); keeping the previous values", name, e.what());
					try {
						feature->LoadSettings(live);
					} catch (...) {
						feature->RestoreDefaultSettings();
					}
				} catch (...) {
					logger::warn("[A/B Compare] {}: slot settings rejected; keeping the previous values", name);
					try {
						feature->LoadSettings(live);
					} catch (...) {
						feature->RestoreDefaultSettings();
					}
				}
			}
			return changed;
		}

		std::string KeyName(uint32_t a_key)
		{
			if (a_key == 0)
				return "none";
			const char* s = Util::Input::KeyIdToString(a_key);
			return (s && *s) ? s : std::format("key {}", a_key);
		}

		/// Other CS hotkeys that use the same key.
		std::string KeyConflicts(uint32_t a_key)
		{
			if (a_key == 0)
				return {};
			const auto& s = globals::menu->GetSettings();
			std::string out;
			auto check = [&](uint32_t k, const char* what) {
				if (k == a_key)
					out += out.empty() ? what : std::string(", ") + what;
			};
			check(s.ToggleKey, "CS menu");
			check(s.OverlayToggleKey, "overlay");
			check(s.EffectToggleKey, "effect toggle");
			check(s.SkipCompilationKey, "skip compilation");
			check(globals::features::performanceOverlay.settings.FreezeKey, "overlay freeze / F12 save");
			if (s.EnableShaderBlocking) {
				check(s.ShaderBlockPrevKey, "shader block prev");
				check(s.ShaderBlockNextKey, "shader block next");
			}
			return out;
		}

		const char* SkyrimKeyUse(uint32_t a_key)
		{
			switch (a_key) {
			case VK_F5:
				return "Quicksave";
			case VK_F9:
				return "Quickload";
			case VK_ESCAPE:
				return "the game menu";
			case VK_TAB:
				return "the character menu";
			default:
				return nullptr;
			}
		}
	}

	// ==========================================================================================

	void Store(Slot a_slot)
	{
		if (a_slot == Slot::None)
			return;
		EnsureLoaded();
		CaptureInto(Get(a_slot), false);
		active = a_slot;
		++slotsVersion;
		WriteFile();
		statusText = std::format("Stored the current settings as {}.", SlotName(a_slot));
		logger::info("[A/B Compare] Stored slot {}", SlotName(a_slot));
	}

	void RequestToggle()
	{
		EnsureLoaded();
		if (pendingToggle)
			return;
		if (lastSwitch != Clock::time_point{} &&
			std::chrono::duration<double>(Clock::now() - lastSwitch).count() < kDebounceSeconds)
			return;  // debounce: a key held or hammered does not queue a burst of reloads
		pendingToggle = true;
	}

	void ProcessPending()
	{
		if (!pendingToggle)
			return;
		pendingToggle = false;

		if (ABTestingManager::GetSingleton()->IsEnabled()) {
			statusText = "The Performance Overlay's timed A/B test is running; stop it first.";
			return;
		}
		if (!slotA.stored || !slotB.stored) {
			statusText = "Store both A and B first.";
			tagText = "A/B: store both A and B first";
			tagShownAt = Clock::now();
			tagVisible = true;
			return;
		}

		const Slot target = active == Slot::A ? Slot::B : Slot::A;

		// Edits made since the last switch belong to the slot that was showing.
		if (active != Slot::None && globals::menu->GetSettings().ABCompareKeepEdits) {
			SlotData& current = Get(active);
			const json before = SlotToJson(current);
			CaptureInto(current, true);
			if (SlotToJson(current) != before)
				++slotsVersion;
		}

		const int changed = Apply(Get(target));
		active = target;
		lastSwitch = Clock::now();
		tagShownAt = lastSwitch;
		tagVisible = true;
		tagText = scope.empty() ? std::string(SlotName(target)) : std::format("{}  {}", SlotName(target), scope);
		WriteFile();
		statusText = std::format("Now showing {} ({}; {} section{} changed).", SlotName(target), ScopeLabel(), changed, changed == 1 ? "" : "s");
		logger::info("[A/B Compare] Switched to {} (scope: {}, {} sections changed)", SlotName(target), ScopeLabel(), changed);
	}

	void OnSettingsReloaded()
	{
		active = Slot::None;
		statusText = "Saved settings restored; they are not slot A or B.";
	}

	bool WantsOverlayFrame()
	{
		if (!tagVisible)
			return false;
		if (std::chrono::duration<double>(Clock::now() - tagShownAt).count() > kTagSeconds) {
			tagVisible = false;
			return false;
		}
		return true;
	}

	void DrawIndicator()
	{
		if (!WantsOverlayFrame())
			return;
		const double age = std::chrono::duration<double>(Clock::now() - tagShownAt).count();
		const float alpha = static_cast<float>(std::clamp((kTagSeconds - age) / kTagFadeSeconds, 0.0, 1.0));

		const bool isSlotTag = tagText.size() <= 2 || tagText[1] == ' ';  // "A" / "B" / "A  Physical Sky"
		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.06f), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
		ImGui::SetNextWindowBgAlpha(0.55f * alpha);
		ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
		constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
		                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav;
		if (ImGui::Begin("##ABCompareTag", nullptr, flags)) {
			ImGui::SetWindowFontScale(isSlotTag ? 1.6f : 1.0f);
			const ImVec4 color = active == Slot::B ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f);
			ImGui::TextColored(color, "%s", tagText.c_str());
			ImGui::SetWindowFontScale(1.0f);
		}
		ImGui::End();
		ImGui::PopStyleVar();
	}

	void DrawOverlayHeaderLine()
	{
		if (active == Slot::None && !slotA.stored && !slotB.stored)
			return;
		const ImVec4 color = active == Slot::B ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f);
		if (active == Slot::None)
			ImGui::TextDisabled("A/B: live settings (%s)", ScopeLabel().c_str());
		else
			ImGui::TextColored(color, "A/B: %s  (%s)", SlotName(active), ScopeLabel().c_str());
	}

	json GetMetaJson()
	{
		return {
			{ "active", SlotName(active) },
			{ "scope", ScopeLabel() },
			{ "a_stored", slotA.stored },
			{ "b_stored", slotB.stored },
		};
	}

	bool ShouldBlockKeyFromGame(uint32_t a_dikCode)
	{
		const uint32_t abKey = globals::menu->GetSettings().ABCompareKey;
		if (abKey == 0)
			return false;
		uint32_t key = Util::Input::DIKToVK(a_dikCode);
		if (key == a_dikCode)
			key = MapVirtualKeyEx(a_dikCode, MAPVK_VSC_TO_VK_EX, GetKeyboardLayout(0));
		return key == abKey;
	}

	void DrawMenuStrip()
	{
		EnsureLoaded();
		auto& settings = globals::menu->GetSettings();
		const auto& theme = settings.Theme;

		ImGui::PushID("ABCompare");
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("A/B Compare:");
		ImGui::SameLine();
		if (ImGui::Button("Store current as A"))
			Store(Slot::A);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Remembers every feature's settings as they are now (in memory; not written to your settings file).");
		ImGui::SameLine();
		if (ImGui::Button("Store current as B"))
			Store(Slot::B);
		ImGui::SameLine();
		const std::string switchLabel = std::format("Switch A <-> B ({})", KeyName(settings.ABCompareKey));
		ImGui::BeginDisabled(!slotA.stored || !slotB.stored);
		if (ImGui::Button(switchLabel.c_str()))
			RequestToggle();
		ImGui::EndDisabled();
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("Applies the other slot at the start of the next frame. Your settings file is only written by Save Settings.");

		ImGui::SameLine();
		ImGui::AlignTextToFramePadding();
		const ImVec4 activeColor = active == Slot::B ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.45f, 0.8f, 1.0f, 1.0f);
		if (active == Slot::None)
			ImGui::TextDisabled("Showing: live");
		else
			ImGui::TextColored(activeColor, "Showing: %s", SlotName(active));

		ImGui::SameLine();
		ImGui::SetNextItemWidth(ImGui::GetFontSize() * 13.0f);
		if (ImGui::BeginCombo("Scope", ScopeLabel().c_str())) {
			if (ImGui::Selectable("All settings", scope.empty()))
				scope.clear();
			if (ImGui::Selectable("Global (defines, shader classes)", scope == kGlobalName))
				scope = kGlobalName;
			auto features = ComparableFeatures();
			std::ranges::sort(features, [](Feature* a, Feature* b) { return a->GetName() < b->GetName(); });
			for (auto* feature : features) {
				const std::string name = feature->GetName();
				if (ImGui::Selectable(name.c_str(), scope == name))
					scope = name;
			}
			ImGui::EndCombo();
		}
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted("With one feature picked, the switch only swaps that feature's settings; everything else stays as it is.");

		RebuildDiffIfNeeded();
		const bool bothStored = slotA.stored && slotB.stored;
		const std::string nodeLabel = bothStored ? std::format("Details - {} difference{} in scope###ABDetails", diffRows.size(), diffRows.size() == 1 ? "" : "s") :
		                                           std::string("Details###ABDetails");
		if (ImGui::TreeNode(nodeLabel.c_str())) {
			if (!statusText.empty())
				ImGui::TextDisabled("%s", statusText.c_str());

			// Options
			ImGui::Checkbox("Keep edits in the showing slot", &settings.ABCompareKeepEdits);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("On: a change you make while A (or B) is showing is kept in A (or B) when you switch away.\nOff: the slots only change when you press Store.");
			ImGui::SameLine();
			if (ImGui::Checkbox("Keep slots after restart", &settings.ABComparePersist) && settings.ABComparePersist)
				WriteFile();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(std::format("Saves A and B to {} next to SettingsUser.json. Your own settings file is never touched.", kFileName).c_str());

			// Hotkey
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted("Switch key:");
			ImGui::SameLine();
			if (capturingKey) {
				ImGui::TextUnformatted("press a key (Esc cancels)...");
			} else {
				ImGui::TextColored(theme.StatusPalette.CurrentHotkey, "%s", KeyName(settings.ABCompareKey).c_str());
				ImGui::SameLine();
				if (ImGui::SmallButton("Change"))
					capturingKey = true;
				ImGui::SameLine();
				if (ImGui::SmallButton("None"))
					settings.ABCompareKey = 0;
			}
			if (const auto conflicts = KeyConflicts(settings.ABCompareKey); !conflicts.empty())
				ImGui::TextColored(theme.StatusPalette.Warning, "This key is also used by: %s. Pick another key.", conflicts.c_str());
			if (const char* gameUse = SkyrimKeyUse(settings.ABCompareKey))
				ImGui::TextDisabled("In Skyrim this key is %s; CS keeps it from reaching the game while it is the switch key.", gameUse);

			// Differences
			if (!bothStored) {
				ImGui::TextDisabled("Store both A and B to see what differs.");
			} else if (diffRows.empty()) {
				ImGui::TextDisabled("A and B are identical in this scope.");
			} else {
				const bool anyRestart = std::ranges::any_of(diffRows, [](const DiffRow& r) { return !r.note.empty(); });
				if (anyRestart)
					ImGui::TextColored(theme.StatusPalette.Warning, "Some differences only take effect after a restart (marked below).");
				const float rowH = ImGui::GetTextLineHeightWithSpacing();
				const float height = std::min(static_cast<float>(diffRows.size() + 1), 14.0f) * rowH + ImGui::GetStyle().CellPadding.y * 4.0f;
				constexpr ImGuiTableFlags tflags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY |
				                                   ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable;
				if (ImGui::BeginTable("##ABDiff", 4, tflags, ImVec2(0.0f, height))) {
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("Feature", ImGuiTableColumnFlags_None, 1.0f);
					ImGui::TableSetupColumn("Setting", ImGuiTableColumnFlags_None, 2.0f);
					ImGui::TableSetupColumn("A", ImGuiTableColumnFlags_None, 1.2f);
					ImGui::TableSetupColumn("B", ImGuiTableColumnFlags_None, 1.2f);
					ImGui::TableHeadersRow();
					ImGuiListClipper clipper;
					clipper.Begin(static_cast<int>(diffRows.size()));
					while (clipper.Step()) {
						for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
							const auto& row = diffRows[static_cast<size_t>(r)];
							ImGui::TableNextRow();
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(row.feature.c_str());
							ImGui::TableNextColumn();
							if (row.note.empty()) {
								ImGui::TextUnformatted(row.key.c_str());
							} else {
								ImGui::TextColored(theme.StatusPalette.Warning, "%s (%s)", row.key.c_str(), row.note.c_str());
							}
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(row.a.c_str());
							ImGui::TableNextColumn();
							ImGui::TextUnformatted(row.b.c_str());
						}
					}
					ImGui::EndTable();
				}
			}
			ImGui::TreePop();
		}
		ImGui::PopID();
	}
}
