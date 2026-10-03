#include "AdvancedSettingsRenderer.h"

#include <algorithm>
#include <format>
#include <imgui.h>
#include <imgui_stdlib.h>
#include <thread>

#include "FeatureIssues.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/NRD.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Fonts.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "TruePBR.h"
#include "Util.h"
#include "Utils/Batch36f.h"
#include "Utils/Batch36g.h"
#include "Utils/Format.h"
#include "Utils/UI.h"

void AdvancedSettingsRenderer::RenderAdvancedSettings(
	const std::function<void()>& drawTruePBRSettings,
	const std::function<void()>& drawDisableAtBootSettings)
{
	// Use TabBar system - tabs sorted alphabetically
	if (ImGui::BeginTabBar("##AdvancedSettingsTabs", ImGuiTabBarFlags_None)) {
		// Batch 36f Tab (first, so the A/B master switch is one click from the Advanced page)
		if (MenuFonts::BeginTabItemWithFont("Batch 36f", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##Batch36fContent", ImVec2(0, 0), false)) {
				RenderBatch36fSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Batch 36g Tab (its own master switch; independent of Batch 36f's)
		if (MenuFonts::BeginTabItemWithFont("Batch 36g", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##Batch36gContent", ImVec2(0, 0), false)) {
				RenderBatch36gSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Developer Tab
		if (MenuFonts::BeginTabItemWithFont("Developer", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##DeveloperContent", ImVec2(0, 0), false)) {
				RenderDeveloperSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Disable at Boot Tab
		if (MenuFonts::BeginTabItemWithFont("Disable at Boot", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##DisableAtBootContent", ImVec2(0, 0), false)) {
				RenderDisableAtBootSection(drawDisableAtBootSettings);
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Logging Tab
		if (MenuFonts::BeginTabItemWithFont("Logging", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##LoggingContent", ImVec2(0, 0), false)) {
				RenderLoggingSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// PBR Settings Tab
		if (MenuFonts::BeginTabItemWithFont("PBR Settings", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##PBRSettingsContent", ImVec2(0, 0), false)) {
				RenderPBRSection(drawTruePBRSettings);
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		// Shader Debug Tab
		if (MenuFonts::BeginTabItemWithFont("Shader Debug", Menu::FontRole::Subheading)) {
			if (ImGui::BeginChild("##ShaderDebugContent", ImVec2(0, 0), false)) {
				RenderShaderDebugSection();
			}
			ImGui::EndChild();
			ImGui::EndTabItem();
		}

		ImGui::EndTabBar();
	}
}

void AdvancedSettingsRenderer::RenderBatch36fSection()
{
	auto& master = Batch36f::settings.master;

	ImGui::Checkbox("Batch 36f denoiser savings (all)", &master);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Off = every item below runs exactly as in batch 36e, whatever its own switch says.\n"
			"Takes effect on the next frame: no restart, no cache clear.");
	}

	ImGui::Spacing();
	ImGui::TextWrapped(
		"Each item keeps its own switch in its feature's menu. \"Now\" is what runs this frame: "
		"the item's own switch, unless the master switch above is off.");
	ImGui::Spacing();

	const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
	const auto& ssrt = globals::features::screenSpaceRayTracing;
	const auto& nrd = globals::features::nrd;

	struct Row
	{
		const char* name;
		bool installed;
		bool own;
		std::string ownText;
		const char* where;
	};
	const Row rows[] = {
		{ "Skip Reflection Pre-pass", ssrt.loaded, ssrt.settings.ReblurSkipSpecularPrepass,
			ssrt.settings.ReblurSkipSpecularPrepass ? "On" : "Off",
			"Lighting > Screen Space Ray Tracing > Denoiser (shown when Denoiser = REBLUR)" },
		{ "Distance Limit", ssrt.loaded, ssrt.settings.DistanceLimit,
			ssrt.settings.DistanceLimit ? std::format("On, {:.0f} m", ssrt.settings.DistanceLimitMeters) : std::string("Off"),
			"Lighting > Screen Space Ray Tracing > Denoiser > Distance Limit" },
		{ "Motion vectors: copy only the render area", nrd.loaded, true, "(no own switch)",
			"- (NRD guide preparation)" },
		{ "Fold Unpack Into Composite", ssrt.loaded, ssrt.settings.ReblurFoldUnpack,
			ssrt.settings.ReblurFoldUnpack ? "On" : "Off",
			"Lighting > Screen Space Ray Tracing > Denoiser (shown when Denoiser = REBLUR)" },
	};

	if (ImGui::BeginTable("##Batch36fItems", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
		ImGui::TableSetupColumn("Item");
		ImGui::TableSetupColumn("Own switch");
		ImGui::TableSetupColumn("Now");
		ImGui::TableSetupColumn("Where", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableHeadersRow();

		for (const auto& row : rows) {
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::TextUnformatted(row.name);
			ImGui::TableNextColumn();
			if (!row.installed)
				ImGui::TextDisabled("not installed");
			else
				ImGui::TextUnformatted(row.ownText.c_str());
			ImGui::TableNextColumn();
			const bool now = row.installed && row.own && master;
			if (!row.installed)
				ImGui::TextDisabled("-");
			else if (row.own && !master)
				ImGui::TextColored(palette.Warning, "Off (master)");
			else
				ImGui::TextColored(now ? palette.SuccessColor : palette.Disable, "%s", now ? "On" : "Off");
			ImGui::TableNextColumn();
			ImGui::TextWrapped("%s", row.where);
		}
		ImGui::EndTable();
	}

	ImGui::Spacing();
	ImGui::TextDisabled(
		"The REBLUR items only do anything while SSRT's Denoiser is REBLUR. The Distance Limit limits bounce light only "
		"while Ambient Reinjection is on. Not affected by this switch: the Denoiser breakdown panel fixes (display only).");
}

void AdvancedSettingsRenderer::RenderBatch36gSection()
{
	auto& master = Batch36g::settings.master;
	const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;
	auto& ssrt = globals::features::screenSpaceRayTracing;

	ImGui::Checkbox("Batch 36g experiments (all)", &master);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Off = the four experiment switches below run at their defaults (the picture is batch 36f's), whatever they are set to.\n"
			"Separate from Batch 36f: this switch does not touch the 36f items, and the 36f master switch does not touch these.\n"
			"Takes effect on the next frame: no restart, no cache clear.");
	}
	ImGui::TextWrapped(
		"Diagnostic matrix for the checkerboard stripes and flicker. Every switch defaults to the batch 36f behaviour; "
		"changing any of them restarts the denoisers' history.");

	if (!ssrt.loaded) {
		ImGui::TextDisabled("Screen Space Ray Tracing is not installed.");
		return;
	}
	auto& s = ssrt.settings;
	using SSRT = ScreenSpaceRayTracing;

	ImGui::SeparatorText("Experiment switches");
	{
		static const char* patterns[] = {
			"Full resolution (default)",
			"A: checkerboard, NRD fills the gaps",
			"B: probabilistic (diffuse or reflection per pixel)",
			"C: checkerboard, our own gap filling"
		};
		int p = (int)std::min(s.B36gPattern, 3u);
		if (ImGui::Combo("Tracing pattern", &p, patterns, 4))
			s.B36gPattern = (uint)p;
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"Which pixels trace bounce light and which trace reflections each frame.\n"
				"Full: both everywhere (36f).\n"
				"A: alternate pixels like a chessboard, swapping every frame; NVIDIA's denoiser fills each gap from its left and right neighbours.\n"
				"B: each pixel picks one of the two by a dither pattern (rough surfaces lean to bounce light); the denoiser fills the rest. NVIDIA's recommended way.\n"
				"C: the same chessboard as A, but our own passes fill the gaps before the denoiser sees them (reflections re-weighted per pixel).\n"
				"A and B need REBLUR on both; A, B and C need both bounce light and reflections on.");
		}

		ImGui::Checkbox("Merge denoisers (one REBLUR instance)", &s.B36gMergedDenoiser);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"One REBLUR denoiser for both bounce light and reflections instead of two. Cheaper, but both then share one set of settings.\n"
				"Needs REBLUR on both.");
		if (s.B36gMergedDenoiser) {
			ImGui::Indent();
			ImGui::Checkbox("Use batch 36b's shared settings (30 / 3 / 15)", &s.B36gMergedUse36bTuning);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text("On = the compromise 36b used. Off = the bounce-light (diffuse) REBLUR settings for both.");
			const NRD::REBLURSettings shown = s.B36gMergedUse36bTuning ?
			                                      NRD::REBLURSettings{ .MaxAccumulatedFrameNum = 30, .MaxFastAccumulatedFrameNum = 3, .MaxStabilizedFrameNum = 15, .FastHistoryClampingSigmaScale = 1.75f } :
			                                      s.ReblurDiffuse;
			ImGui::TextDisabled("Shared: accumulated %u, fast %u, stabilized %u, fast clamp %.2f, blur %.0f-%.0f px",
				shown.MaxAccumulatedFrameNum, shown.MaxFastAccumulatedFrameNum, shown.MaxStabilizedFrameNum,
				shown.FastHistoryClampingSigmaScale, shown.MinBlurRadius, shown.MaxBlurRadius);
			ImGui::Unindent();
		}

		static const char* sources[] = {
			"1: rays, filtered (batch 34, default)",
			"2: REBLUR hit distance (36b, reproduction only)"
		};
		int c = (int)std::min(s.B36gConfidenceSource, 1u);
		if (ImGui::Combo("Confidence source", &c, sources, 2))
			s.B36gConfidenceSource = (uint)c;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Where 'how much of the game's own ambient light to replace' comes from (Ambient Reinjection).\n"
				"1: this frame's rays, smoothed at quarter resolution (the clean batch 34 way).\n"
				"2: what 36b did, kept only to check whether 36b's problem comes back. Needs Ambient Reinjection and REBLUR.");

		ImGui::Checkbox("Diffuse pre-blur", &s.B36gDiffusePrepass);
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text("REBLUR's small blur on bounce light before it averages frames. Pattern B always turns it on.");
		if (s.B36gDiffusePrepass || s.B36gPattern == SSRT::kB36gB) {
			ImGui::Indent();
			ImGui::SliderFloat("Diffuse pre-blur radius", &s.B36gDiffusePrepassRadius, 5.0f, 60.0f, "%.0f px", ImGuiSliderFlags_AlwaysClamp);
			ImGui::Unindent();
		}
		ImGui::TextDisabled(
			"Reflection pre-blur is batch 36f's \"Skip Reflection Pre-pass\" (Lighting > Screen Space Ray Tracing > Denoiser); "
			"patterns A and B turn the pre-pass back on while they run.");
	}

	ImGui::SeparatorText("This frame");
	{
		const auto& f = ssrt.b36g;
		const uint combo = SSRT::B36gCombination(f.drawnPattern, f.mergedDrawn);
		ImGui::TextColored(palette.InfoColor, "Combination #%u%s%s", combo, f.confSource == 1 ? " + confidence 2" : "", f.diffusePrepass ? " + diffuse pre-blur" : "");
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Numbers follow the test table: 1 Full + two denoisers, 2 A + two, 3 A + merged, 4 B + two, 5 B + merged,\n"
				"6 C + two, 7 C + merged, 8 Full + merged. Freezing the performance overlay (F11) writes this line to the log.");

		const bool on = master;
		struct Row
		{
			const char* name;
			std::string set;
			std::string now;
			bool differs;
			const char* note;
		};
		const auto patternShort = [](uint a_p) -> std::string { return a_p == 1 ? "A" : a_p == 2 ? "B" : a_p == 3 ? "C" : "Full"; };
		const bool prepassForced = f.specPrepassForced;
		const std::string specPrepassNow = prepassForced ? "on (forced by pattern " + patternShort(f.drawnPattern) + ")" :
		                                                   (ssrt.SkipSpecularPrepassActive() ? "skipped (36f)" : "on");
		std::vector<Row> rows = {
			{ "Tracing pattern", patternShort(s.B36gPattern), patternShort(f.drawnPattern), (on ? s.B36gPattern : 0u) != f.drawnPattern, f.patternNote },
			{ "Denoiser", s.B36gMergedDenoiser ? "merged" : "two instances", f.mergedDrawn ? "merged" : "two instances",
				(on && s.B36gMergedDenoiser) != f.mergedDrawn, f.mergedNote },
			{ "Confidence source", s.B36gConfidenceSource == 1 ? "2" : "1", f.confSource == 1 ? "2" : "1",
				(on ? std::min(s.B36gConfidenceSource, 1u) : 0u) != f.confSource, f.confNote },
			{ "Diffuse pre-blur", s.B36gDiffusePrepass ? "on" : "off", f.diffusePrepass ? std::format("on, {:.0f} px", s.B36gDiffusePrepassRadius) : std::string("off"),
				(on && s.B36gDiffusePrepass) != f.diffusePrepass, f.prepassNote },
			{ "Reflection pre-pass (36f item)", s.ReblurSkipSpecularPrepass ? "skip" : "keep", specPrepassNow,
				prepassForced && ssrt.SkipSpecularPrepassActive(), prepassForced ? "pre-pass forced on by the pattern (overrides 36f's skip)" : nullptr },
			{ "Low-Res Confidence Filter", s.LowResConfidenceFilter ? "on" : "off", (s.LowResConfidenceFilter || f.lowResFilterForced) ? "on" : "off",
				f.lowResFilterForced, f.lowResFilterForced ? "forced on by the pattern (the full-resolution window cannot skip untraced pixels)" : nullptr },
		};

		if (ImGui::BeginTable("##Batch36gItems", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("Item");
			ImGui::TableSetupColumn("Set");
			ImGui::TableSetupColumn("Now");
			ImGui::TableSetupColumn("Why it differs", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			for (const auto& row : rows) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.name);
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(row.set.c_str());
				ImGui::TableNextColumn();
				if (row.differs)
					ImGui::TextColored(palette.Warning, "%s", row.now.c_str());
				else
					ImGui::TextUnformatted(row.now.c_str());
				ImGui::TableNextColumn();
				if (!on && !row.note && row.differs)
					ImGui::TextWrapped("master switch off");
				else if (row.note)
					ImGui::TextWrapped("%s", row.note);
			}
			ImGui::EndTable();
		}
		if (!on)
			ImGui::TextColored(palette.Warning, "Batch 36g master switch is off: every experiment switch runs at its default.");
		if (f.foldNote)
			ImGui::TextColored(palette.Warning, "Fold Unpack: %s", f.foldNote);
		else if (ssrt.FoldUnpackActive())
			ImGui::TextDisabled("Fold Unpack Into Composite (36f) works with every combination (pattern A falls back to a separate unpack only if REBLUR fails to run).");
		if (f.mergedDrawn)
			ImGui::TextDisabled(
				"Merged: reflections are traced before this frame's bounce light is added to the picture, so mirrored surfaces miss "
				"this frame's SSRT bounce light (slightly darker in reflections).");
		if (ssrt.DistanceLimitActive() && f.mergedDrawn && !ssrt.settings.EnableAmbientReinjection)
			ImGui::TextDisabled("Distance Limit with the merged denoiser and Ambient Reinjection off: tracing is limited, REBLUR denoises the full range.");
	}

	ImGui::SeparatorText("Debug views");
	{
		static const char* views[] = { "Off", "Which signal each pixel traced", "Bounce light: denoiser input before / after gap filling", "Reflections: denoiser input before / after gap filling" };
		int v = (int)std::min(s.B36gDebugView, 3u);
		if (ImGui::Combo("Debug view", &v, views, 4))
			s.B36gDebugView = (uint)v;
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::Text(
				"Which signal: red = bounce light traced, green = reflection traced, yellow = both, dark blue = sky.\n"
				"Before / after: left half = only the pixels traced this frame (black = gap); right half = what the denoiser works from after the gaps are filled\n"
				"(A: an imitation of NVIDIA's left/right fill; B: as NVIDIA receives it, which only rebuilds hit distances; C: our fill). REBLUR only.");
		if (s.B36gDebugView != 0) {
			if (auto* srv = ssrt.texB36gDebug ? ssrt.texB36gDebug->srv.get() : nullptr) {
				const float w = ImGui::GetContentRegionAvail().x;
				const float aspect = (float)ssrt.texB36gDebug->desc.Height / std::max(1.0f, (float)ssrt.texB36gDebug->desc.Width);
				Util::BufferViewerImage(srv, { w, w * aspect });
			} else {
				ImGui::TextDisabled("Nothing drawn yet (the view needs the matching chain to run with REBLUR).");
			}
		}
	}
}

void AdvancedSettingsRenderer::RenderLoggingSection()
{
	auto shaderCache = globals::shaderCache;

	// Log Level selection
	spdlog::level::level_enum logLevel = globals::state->GetLogLevel();
	const char* items[] = {
		"trace",
		"debug",
		"info",
		"warn",
		"err",
		"critical",
		"off"
	};
	static int item_current = static_cast<int>(logLevel);
	if (ImGui::Combo("Log Level", &item_current, items, IM_ARRAYSIZE(items))) {
		ImGui::SameLine();
		globals::state->SetLogLevel(static_cast<spdlog::level::level_enum>(item_current));
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Log level. Trace is most verbose. Default is info.");
	}

	// Shader Defines input
	auto& shaderDefines = globals::state->shaderDefinesString;
	if (ImGui::InputText("Shader Defines", &shaderDefines)) {
		globals::state->SetDefines(shaderDefines);
	}
	if (ImGui::IsItemDeactivatedAfterEdit() || (ImGui::IsItemActive() &&
												   (ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_Enter)) ||
													   ImGui::IsKeyPressed(ImGui::GetKeyIndex(ImGuiKey_KeypadEnter))))) {
		globals::state->SetDefines(shaderDefines);
		shaderCache->Clear();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Defines for Shader Compiler. Semicolon \";\" separated. Clear with space. Rebuild shaders after making change. Compute Shaders require a restart to recompile.");
	}

	ImGui::Spacing();

	// Compiler Thread controls
	ImGui::SliderInt("Compiler Threads", &shaderCache->compilationThreadCount, 1, static_cast<int32_t>(std::thread::hardware_concurrency()));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Number of threads to use to compile shaders. "
			"The more threads the faster compilation will finish but may make the system unresponsive. ");
	}
	ImGui::SliderInt("Background Compiler Threads", &shaderCache->backgroundCompilationThreadCount, 1, static_cast<int32_t>(std::thread::hardware_concurrency()));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Number of threads to use to compile shaders while playing game. "
			"This is activated if the startup compilation is skipped. "
			"The more threads the faster compilation will finish but may make the system unresponsive. ");
	}

	// A/B Testing settings
	auto* abTestingManager = ABTestingManager::GetSingleton();
	abTestingManager->DrawSettingsUI();

	// Dump Ini Settings button
	if (ImGui::Button("Dump Ini Settings", { -1, 0 })) {
		Util::DumpSettingsOptions();
	}
}

void AdvancedSettingsRenderer::RenderShaderDebugSection()
{
	auto shaderCache = globals::shaderCache;
	auto state = globals::state;

	// Dump Shaders option
	bool useDump = shaderCache->IsDump();
	if (ImGui::Checkbox("Dump Shaders", &useDump)) {
		shaderCache->SetDump(useDump);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Dump shaders at startup. This should be used only when reversing shaders. Normal users don't need this.");
	}

	// Clear Shader Cache button
	if (ImGui::Button("Clear Shader Cache", { -1, 0 })) {
		shaderCache->Clear();
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Clear all compiled shaders from memory. Forces recompilation of all shaders on next use.");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Shader Replacement section
	Util::DrawSectionHeader("Replace Original Shaders");

	if (ImGui::BeginTable("##ReplaceToggles", 3, ImGuiTableFlags_SizingStretchSame)) {
		globals::state->ForEachShaderTypeWithIndex([&](auto type, int classIndex) {
			ImGui::TableNextColumn();

			if (!(SIE::ShaderCache::IsSupportedShader(type) || state->IsDeveloperMode())) {
				ImGui::BeginDisabled();
				ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
				ImGui::EndDisabled();
			} else
				ImGui::Checkbox(std::format("{}", magic_enum::enum_name(type)).c_str(), &state->enabledClasses[classIndex]);
		});
		if (state->IsDeveloperMode()) {
			ImGui::Checkbox("Vertex", &state->enableVShaders);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Replace Vertex Shaders. "
					"When false, will disable the custom Vertex Shaders for the types above. "
					"For developers to test whether CS shaders match vanilla behavior. ");
			}

			ImGui::Checkbox("Pixel", &state->enablePShaders);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Replace Pixel Shaders. "
					"When false, will disable the custom Pixel Shaders for the types above. "
					"For developers to test whether CS shaders match vanilla behavior. ");
			}

			ImGui::Checkbox("Compute", &state->enableCShaders);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::Text(
					"Replace Compute Shaders. "
					"When false, will disable the custom Compute Shaders for the types above. "
					"For developers to test whether CS shaders match vanilla behavior. ");
			}
		}
		ImGui::EndTable();
	}

	// Only show shader blocking section in developer mode
	if (!globals::state->IsDeveloperMode()) {
		return;
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Show blocked shader status as a regular section
	if (!shaderCache->blockedKey.empty()) {
		// Create a visually distinct box for the blocked shader info with rounded corners and border
		ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
		ImVec4 blockedBgColor = Util::Colors::GetError();
		blockedBgColor.w = 0.15f;  // Semi-transparent background
		ImGui::PushStyleColor(ImGuiCol_ChildBg, blockedBgColor);

		if (ImGui::BeginChild("##BlockedShaderInfo", ImVec2(0, 0), true, ImGuiChildFlags_AutoResizeY)) {
			ImGui::TextColored(Util::Colors::GetError(), "Shader Blocking Active");
			ImGui::SameLine();
			if (ImGui::SmallButton("Stop Blocking##Section")) {
				shaderCache->DisableShaderBlocking();
			}

			ImGui::Text("Blocked: %s", shaderCache->blockedKey.c_str());

			// Try to get more details from active shaders
			auto activeShaders = shaderCache->GetActiveShaders();
			for (const auto& shader : activeShaders) {
				if (shader.key == shaderCache->blockedKey) {
					ImGui::Text("Type: %s", magic_enum::enum_name(shader.shaderType).data());
					ImGui::Text("Class: %s", magic_enum::enum_name(shader.shaderClass).data());
					ImGui::Text("Descriptor: 0x%X", shader.descriptor);

					// Add button to copy shader info to clipboard
					ImGui::PushID(shader.key.c_str());
					if (ImGui::SmallButton("Copy Info##BlockedShader")) {
						std::string diskPathStr;
						diskPathStr.reserve(shader.diskPath.size());
						for (wchar_t wc : shader.diskPath) {
							diskPathStr += static_cast<char>(wc);
						}

						std::string fullInfo = std::format("Type: {}\nClass: {}\nDescriptor: 0x{:X}\nKey: {}\nCache Path: {}",
							magic_enum::enum_name(shader.shaderType).data(),
							magic_enum::enum_name(shader.shaderClass).data(),
							shader.descriptor,
							shader.key,
							diskPathStr);
						ImGui::SetClipboardText(fullInfo.c_str());
					}
					ImGui::PopID();
					if (ImGui::IsItemHovered()) {
						if (auto _tt = Util::HoverTooltipWrapper()) {
							ImGui::Text("Copy complete shader information including cache path to clipboard");
						}
					}

					break;
				}
			}
		}
		ImGui::EndChild();

		ImGui::PopStyleVar();    // ChildRounding
		ImGui::PopStyleVar();    // WindowBorderSize
		ImGui::PopStyleColor();  // ChildBg
	}

	// Shader Debug section
	if (ImGui::CollapsingHeader("Shader Debug")) {
		auto menu = globals::menu;
		auto& menuSettings = menu->GetSettings();
		auto& themeSettings = menuSettings.Theme;

		if (ImGui::Checkbox("Enable Shader Blocking", &menuSettings.EnableShaderBlocking)) {
			// Setting saved automatically on next save
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Enables hotkeys to cycle through and block individual shaders for debugging purposes.");
		}

		if (menuSettings.EnableShaderBlocking) {
			ImGui::Indent();

			// Shader Block Previous Key
			if (menu->settingShaderBlockPrevKey) {
				ImGui::Text("Press any key for Shader Block Previous...");
			} else {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Block Previous:");
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s", Util::Input::KeyIdToString(menuSettings.ShaderBlockPrevKey));
				ImGui::SameLine();
				if (ImGui::Button("Change##ShaderBlockPrev")) {
					menu->settingShaderBlockPrevKey = true;
				}
			}

			// Shader Block Next Key
			if (menu->settingShaderBlockNextKey) {
				ImGui::Text("Press any key for Shader Block Next...");
			} else {
				ImGui::AlignTextToFramePadding();
				ImGui::Text("Block Next:");
				ImGui::SameLine();
				ImGui::AlignTextToFramePadding();
				ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s", Util::Input::KeyIdToString(menuSettings.ShaderBlockNextKey));
				ImGui::SameLine();
				if (ImGui::Button("Change##ShaderBlockNext")) {
					menu->settingShaderBlockNextKey = true;
				}
			}

			ImGui::Unindent();
		}
	}

	// Active shaders list
	if (ImGui::CollapsingHeader("Active Shaders", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text("Active Shaders (Used Recently)");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text(
				"List of shaders that have been used in recent frames. "
				"Enable Shader Blocking above to use hotkeys to cycle through and block shaders for debugging. "
				"Shaders not used for ~1 second are removed from this list.");
		}

		// Get fresh active shaders data for accurate count and table
		auto activeShaders = shaderCache->GetActiveShaders();
		uint32_t totalDrawCalls = 0;
		for (const auto& shader : activeShaders) {
			totalDrawCalls += shader.drawCalls;
		}

		// Static variables to maintain table filter state
		static char filterText[256] = "";
		static int searchColumn = 0;        // 0 = All Columns, 1 = Type, 2 = Class, 3 = Descriptor, 4 = Draw Calls, 5 = Key
		static size_t sortColumn = 4;       // Default sort by Frame % (draw calls)
		static bool sortAscending = false;  // Descending by default (highest usage first)		// Create shader rows for the table utility (simplified - no filter data needed)
		struct ShaderRow
		{
			SIE::ShaderCache::ActiveShaderInfo shader;
			uint32_t totalDrawCalls;
		};

		std::vector<ShaderRow> shaderRows;
		for (const auto& shader : activeShaders) {
			shaderRows.push_back({ shader, totalDrawCalls });
		}

		// Build column configurations
		std::vector<Util::TableColumnConfig<ShaderRow>> columns = {
			{ "Type", "Shader type", [](const ShaderRow& row) {
				 return std::string(magic_enum::enum_name(row.shader.shaderType));
			 } },
			{ "Class", "Shader class", [](const ShaderRow& row) {
				 return std::string(magic_enum::enum_name(row.shader.shaderClass));
			 } },
			{ "Descriptor", "Shader descriptor", [](const ShaderRow& row) {
				 return std::format("0x{:X}", row.shader.descriptor);
			 } },
			{ "Frame %", "Percentage of draw calls this frame", [](const ShaderRow& row) {
				 float percentage = Util::CalculatePercentage(static_cast<float>(row.shader.drawCalls), static_cast<float>(row.totalDrawCalls));
				 return Util::FormatPercent(percentage);
			 } },
			{ "Key", "Shader key", [](const ShaderRow& row) {
				 return row.shader.key;
			 } }
		};

		// Row click callbacks
		auto onRowLeftClick = [shaderCache](const ShaderRow& row) {
			if (row.shader.key == shaderCache->blockedKey) {
				shaderCache->DisableShaderBlocking();
			} else {
				// Block this shader - use IterateShaderBlock to find and block it
				// Or set blockedKey directly (simpler for click-to-block)
				shaderCache->blockedKey = row.shader.key;
				logger::info("Blocking shader: {}", row.shader.key);
			}
		};

		auto onRowRightClick = [shaderCache](const ShaderRow& row) {
			std::string diskPathStr;
			diskPathStr.reserve(row.shader.diskPath.size());
			for (wchar_t wc : row.shader.diskPath) {
				diskPathStr += static_cast<char>(wc);
			}

			std::string fullInfo = std::format("Type: {}\nClass: {}\nDescriptor: 0x{:X}\nKey: {}\nCache Path: {}",
				magic_enum::enum_name(row.shader.shaderType).data(),
				magic_enum::enum_name(row.shader.shaderClass).data(),
				row.shader.descriptor,
				row.shader.key,
				diskPathStr);
			ImGui::SetClipboardText(fullInfo.c_str());
		};
		auto getRowTooltip = [shaderCache](const ShaderRow& row) {
			std::string clickAction = (row.shader.key == shaderCache->blockedKey) ? "Left-click to unblock this shader" : "Left-click to block this shader";

			return std::format("Type: {}\nClass: {}\nDescriptor: 0x{:X}\nKey: {}\n\n{}",
				magic_enum::enum_name(row.shader.shaderType).data(),
				magic_enum::enum_name(row.shader.shaderClass).data(),
				row.shader.descriptor,
				row.shader.key,
				clickAction);
		};

		// Define function to extract filterable fields (for TableFilterState)
		auto getFilterableFields = [](const ShaderRow& row) -> std::vector<std::string> {
			return {
				std::string(magic_enum::enum_name(row.shader.shaderType)),                                                                         // Type
				std::string(magic_enum::enum_name(row.shader.shaderClass)),                                                                        // Class
				std::format("0x{:X}", row.shader.descriptor),                                                                                      // Descriptor
				Util::FormatPercent(Util::CalculatePercentage(static_cast<float>(row.shader.drawCalls), static_cast<float>(row.totalDrawCalls))),  // Frame %
				row.shader.key                                                                                                                     // Key
			};
		};

		// Define sorting comparators (customSorts parameter)
		std::vector<std::function<bool(const ShaderRow&, const ShaderRow&, bool)>> sorters = {
			// Type - string sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				std::string aVal = std::string(magic_enum::enum_name(a.shader.shaderType));
				std::string bVal = std::string(magic_enum::enum_name(b.shader.shaderType));
				return ascending ? (aVal < bVal) : (aVal > bVal);
			},
			// Class - string sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				std::string aVal = std::string(magic_enum::enum_name(a.shader.shaderClass));
				std::string bVal = std::string(magic_enum::enum_name(b.shader.shaderClass));
				return ascending ? (aVal < bVal) : (aVal > bVal);
			},
			// Descriptor - numeric sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				return ascending ? (a.shader.descriptor < b.shader.descriptor) : (a.shader.descriptor > b.shader.descriptor);
			},
			// Frame % - numeric sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				float aPercent = Util::CalculatePercentage(static_cast<float>(a.shader.drawCalls), static_cast<float>(a.totalDrawCalls));
				float bPercent = Util::CalculatePercentage(static_cast<float>(b.shader.drawCalls), static_cast<float>(b.totalDrawCalls));
				return ascending ? (aPercent < bPercent) : (aPercent > bPercent);
			},
			// Key - string sort
			[](const ShaderRow& a, const ShaderRow& b, bool ascending) {
				return ascending ? (a.shader.key < b.shader.key) : (a.shader.key > b.shader.key);
			}
		};

		// Create filter state
		Util::TableFilterState<ShaderRow> filterState(getFilterableFields);

		// Initialize filter state from existing variables
		filterState.filterText = std::string(filterText, filterText + strlen(filterText));
		filterState.searchColumn = searchColumn;

		// Define input events for row interactions
		std::vector<Util::TableInputEvent<ShaderRow>> inputEvents = {
			// Left-click to block/unblock shader
			{ Util::TableInputEventType::MouseClick, onRowLeftClick, "", 0 },
			// Right-click context menu for copying info
			{ Util::TableInputEventType::ContextMenu, onRowRightClick, "Copy Info", 1 }
		};

		// Render the table with all configurations
		Util::ShowInteractiveTable<ShaderRow>(
			"##ActiveShadersTable",
			columns,
			shaderRows,
			sortColumn,
			sortAscending,
			sorters,
			filterState,
			inputEvents,
			getRowTooltip);

		// Update static variables with modified filter state
		strncpy_s(filterText, filterState.filterText.c_str(), sizeof(filterText) - 1);
		filterText[sizeof(filterText) - 1] = '\0';
		searchColumn = filterState.searchColumn;
	}
}

void AdvancedSettingsRenderer::RenderPBRSection(const std::function<void()>& drawTruePBRSettings)
{
	drawTruePBRSettings();
}

void AdvancedSettingsRenderer::RenderDisableAtBootSection(const std::function<void()>& drawDisableAtBootSettings)
{
	drawDisableAtBootSettings();
}

void AdvancedSettingsRenderer::RenderDeveloperSection()
{
	auto shaderCache = globals::shaderCache;

	// File Watcher option (moved from Advanced/Logging)
	bool useFileWatcher = shaderCache->UseFileWatcher();
	if (ImGui::Checkbox("Enable File Watcher", &useFileWatcher)) {
		shaderCache->SetFileWatcher(useFileWatcher);
	}
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text(
			"Automatically recompile shaders on file change. "
			"Intended for developing.");
	}

	// Debug addresses section (moved from Advanced/Logging)
	if (ImGui::TreeNodeEx("Addresses")) {
		auto Renderer = globals::game::renderer;
		auto BSShaderAccumulator = *globals::game::currentAccumulator.get();
		auto RendererShadowState = globals::game::shadowState;
		ADDRESS_NODE(Renderer)
		ADDRESS_NODE(BSShaderAccumulator)
		ADDRESS_NODE(RendererShadowState)
		ImGui::TreePop();
	}

	// Statistics section (moved from Advanced/Logging)
	if (ImGui::TreeNodeEx("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Text(std::format("Shader Compiler : {}", shaderCache->GetShaderStatsString()).c_str());
		ImGui::TreePop();
	}

	// Frame annotations toggle (moved from Advanced/Logging)
	ImGui::Checkbox("Frame Annotations", &globals::state->frameAnnotations);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Enable detailed frame annotations for debugging render passes and draw calls.");
	}

	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();

	// Developer Mode Testing Section
	if (globals::state->IsDeveloperMode()) {
		FeatureIssues::Test::DrawDeveloperModeTestingUI();
	}
}
