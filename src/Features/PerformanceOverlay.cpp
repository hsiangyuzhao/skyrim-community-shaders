/**
 * @file PerformanceOverlay.cpp
 * @brief Real-time performance monitoring system for Skyrim Community Shaders
 *
 * This module provides comprehensive performance monitoring capabilities including:
 * - Real-time FPS and frame time tracking with configurable update intervals
 * - Interactive draw call analysis with per-shader type performance breakdown
 * - VRAM usage monitoring with visual progress bars
 * - Frame time graphs for pre and post-frame generation analysis
 * - A/B testing support for performance comparison between configurations
 * - Color-coded performance metrics with customizable thresholds
 * - Movable overlay window with persistent positioning
 *
 * The overlay integrates with the A/B testing system to provide live performance
 * comparisons between different shader configurations, helping users optimize
 * their setup for maximum performance while maintaining visual quality.
 *
 */

#include "PerformanceOverlay.h"
#include "Feature.h"
#include "Features/PerformanceOverlay/ABTesting/ABTestAggregator.h"
#include "Features/GrassOptimizations.h"
#include "Features/NRD.h"
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/Upscaling.h"
#include "Features/Upscaling/NeuralRendering/Integration.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/DenoiserTimers.h"
#include "Utils/Game.h"
#include "Utils/GpuPhaseTimeline.h"
#include "Utils/GpuTimers.h"
#include "Utils/OcclusionDryRun.h"
#include "Utils/UI.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <magic_enum/magic_enum.hpp>
#include <map>
#include <numeric>
#include <string_view>
#include <unordered_set>

// --- Constants ---
constexpr float kDefaultFPS = 60.0f;
constexpr float kDefaultFrameTimeMs = 1000.0f / kDefaultFPS;

// --- Helper Structures and Functions ---

// Helper function to create metric columns with consistent formatting
auto MakeMetricColumn(const auto& theme, auto valueGetter, auto colorGetter, auto formatter, const Util::ColoredTextLines& legend, const Util::ColoredTextLines* cellLegend = nullptr)
{
	return [theme, valueGetter, colorGetter, formatter, legend, cellLegend](const DrawCallRow& row, int) {
		using ValueType = decltype(valueGetter(row));
		if constexpr (std::is_same_v<ValueType, std::optional<float>>) {
			if (!valueGetter(row).has_value()) {
				ImGui::TextDisabled("-");
				return;
			}
			float value = *valueGetter(row);
			ImVec4 color = colorGetter(theme, value, row);
			std::string valueStr = formatter(value, row);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::Text("%s", valueStr.c_str());
			ImGui::PopStyleColor();
		} else {
			float value = valueGetter(row);
			ImVec4 color = colorGetter(theme, value, row);
			std::string valueStr = formatter(value, row);
			ImGui::PushStyleColor(ImGuiCol_Text, color);
			ImGui::Text("%s", valueStr.c_str());
			ImGui::PopStyleColor();
		}
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				const Util::ColoredTextLines& useLegend = cellLegend ? *cellLegend : legend;
				Util::DrawColoredMultiLineTooltip(useLegend);
			}
		}
	};
}

// (batch 36e) "Peak" column: the highest reading inside the smoothing window.
static ColumnConfig MakePeakColumn()
{
	return ColumnConfig{
		"Peak",
		[](const DrawCallRow& row, int) {
			if (!row.peak) {
				ImGui::TextDisabled("-");
				return;
			}
			ImGui::Text("%s", Util::FormatMilliseconds(*row.peak).c_str());
		},
		nullptr,
		[]() {
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper())
					ImGui::TextUnformatted("Highest reading inside the smoothing window: catches spikes the average hides.");
			}
		}
	};
}

// --- Helper Functions ---
/**
  * @brief Calculates summary data (residual frame time, percentages, cost per call).
  *
  * `smoothedFrameTime` must be State::GetAttributionFrameTimeMs() - the wall clock,
  * smoothed with the same coefficients and on the same cadence as the per-type buckets.
  * Passing the overlay's own `smoothFrameTimeMs` here (an instantaneous Present-to-Present
  * sample re-snapped every 0.5 s) mixed two estimators: the buckets could exceed it and
  * the residual went negative.
  *
  * `measuredSum` must contain CPU attribution only. GPU timestamp buckets are shown in
  * their own table and are deliberately NOT subtracted here: they are sampled on the GPU
  * clock, read back several frames later, and overlap both each other and the CPU
  * timeline, so subtracting them from a CPU frame-time sample is not a residual.
  *
  * @param smoothedFrameTime Wall-clock frame time on the attribution clock
  * @param measuredSum The sum of measured per-shader CPU frame times
  * @return Tuple of (otherFrameTime, otherPercent, totalCostPerCall)
  */
static std::tuple<float, float, float> CalculateSummaryData(float smoothedFrameTime, float measuredSum)
{
	float totalSmoothedDrawCalls = globals::state->GetTotalSmoothedDrawCalls();
	// Community Shaders' own CPU cost and the Present wait are measured on the same clock
	// with the same smoothing and are disjoint from the shader-type buckets (State::Debug()
	// subtracts them from the intervals it charges), so they belong in the accounted sum.
	// Every caller gets this for free, which keeps the residual consistent between the live
	// table, the A/B aggregation and the manual-toggle capture.
	auto* cpuTimers = Util::CpuPassTimers::GetSingleton();
	const float accountedSum = measuredSum + cpuTimers->GetFeatureTotalMs() + cpuTimers->GetPresentWaitMs();
	// Same clock, same smoother, and the buckets only ever cover first-draw..last-draw of
	// the frame, so this cannot legitimately go negative any more. The clamp is a belt-and
	// -braces guard for the transient right after the overlay is unhidden, where the
	// bucket EMAs have been frozen for a while and the wall-clock EMA has not.
	float otherFrameTime = std::max(0.0f, Util::CalculateOtherFrameTime(smoothedFrameTime, accountedSum));
	float otherPercent = Util::CalculatePercentage(otherFrameTime, smoothedFrameTime);
	float totalCostPerCall = Util::CalculateCostPerCall(smoothedFrameTime, totalSmoothedDrawCalls);
	return { otherFrameTime, otherPercent, totalCostPerCall };
}

NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
	PerformanceOverlay::Settings,
	ShowInOverlay,
	ShowDrawCalls,
	ShowVRAM,
	ShowFPS,
	ShowPreFGFrameTimeGraph,
	ShowPostFGFrameTimeGraph,
	UpdateInterval,
	FrameHistorySize,
	TextSize,
	BackgroundOpacity,
	ShowBorder,
	Position,
	PositionSet,
	SortMode,
	SmoothingMode,
	SmoothingWindow,
	ShowLiveValues,
	ShowPeakColumn,
	TopN,
	FreezeKey,
	DenoiserLogInterval,
	SectionFrame,
	SectionBottleneck,
	SectionShaderTypes,
	SectionCsCpu,
	SectionGpuPasses,
	SectionEngine,
	SectionDenoiser,
	SectionGrass,
	SectionVram,
	SectionView,
	SectionOcclusion,
	SectionShadows)

static const std::unordered_map<RE::BSShader::Type, std::string> kShaderTypeTooltips = {
	{ RE::BSShader::Type::Grass, "Draw calls using the Grass shader. Typically many, but each is usually cheap.\nWith Grass Optimizations on, each grass type is drawn in one call, so this count is much lower; its section below the GPU tables shows the real grass counts." },
	{ RE::BSShader::Type::Sky, "Draw calls for the sky dome, clouds, and related effects." },
	{ RE::BSShader::Type::Water, "Draw calls for water surfaces and effects." },
	{ RE::BSShader::Type::Lighting, "Draw calls for dynamic and static lighting passes." },
	{ RE::BSShader::Type::Effect, "Draw calls for special effects, particles, and post-processing." },
	{ RE::BSShader::Type::Utility, "Draw calls for utility passes, such as shadow masks or G-buffer fills." },
	{ RE::BSShader::Type::DistantTree, "Draw calls for distant tree rendering (LOD vegetation)." },
	{ RE::BSShader::Type::Particle, "Draw calls for particle systems (smoke, sparks, etc.)." },
	{ RE::BSShader::Type::BloodSplatter, "Draw calls for blood splatter effects." },
	{ RE::BSShader::Type::ImageSpace, "Draw calls for image space post-processing effects." }
};
// ============================================================================
// VIRTUAL OVERRIDES (Feature.h interface)
// ============================================================================

std::pair<std::string, std::vector<std::string>> PerformanceOverlay::GetFeatureSummary()
{
	std::string description = "Real-time performance monitoring system that displays FPS, frame times, draw calls, VRAM usage, and detailed shader performance analysis.";

	std::vector<std::string> keyFeatures = {
		"Real-time FPS and frame time monitoring with configurable update intervals",
		"Interactive draw call analysis with per-shader type performance breakdown",
		"VRAM usage monitoring with visual progress bars",
		"Frame time graphs for pre and post-frame generation analysis",
		"A/B testing support for performance comparison between configurations",
		"Color-coded performance metrics with customizable thresholds",
		"Movable overlay window with persistent positioning"
	};

	return { description, keyFeatures };
}

void PerformanceOverlay::DrawSettings()
{
	auto menu = Menu::GetSingleton();
	const auto& themeSettings = menu->GetTheme();
	const auto& menuSettings = menu->GetSettings();
	ImGui::Checkbox("Show in Overlay", &this->settings.ShowInOverlay);
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::Text("Opens performance overlay in a separate window that stays open\neven when the main menu is closed. ");
		ImGui::Text("Toggle with ");
		ImGui::SameLine();
		ImGui::TextColored(themeSettings.StatusPalette.CurrentHotkey, "%s", Util::Input::KeyIdToString(menuSettings.OverlayToggleKey));
	}

	if (this->settings.ShowInOverlay) {
		ImGui::Indent();

		// Display options
		ImGui::TextUnformatted("Display Options");
		ImGui::Separator();

		ImGui::Checkbox("Show FPS Counter", &this->settings.ShowFPS);
		ImGui::Checkbox("Show Draw Calls", &this->settings.ShowDrawCalls);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Shows the detailed performance tables: bottleneck summary, CPU time per shader type and per Community Shaders feature, and GPU time per effect and per engine stage.");
		}
		ImGui::Checkbox("Show VRAM Usage", &this->settings.ShowVRAM);

		bool isFrameGenerationActive = globals::features::upscaling.IsFrameGenerationActive();
		if (this->settings.ShowFPS && isFrameGenerationActive) {
			ImGui::Checkbox("Show Pre-FG Frametime Graph", &this->settings.ShowPreFGFrameTimeGraph);

			ImGui::Checkbox("Show Post-FG Frametime Graph", &this->settings.ShowPostFGFrameTimeGraph);
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("FSR Frame Generation uses calculated timing data (2x Pre-FG).\nDLSS Frame Generation provides measured timing data at whatever\nmultiplier it is running.");
				}
			}
		} else if (this->settings.ShowFPS) {
			ImGui::Checkbox("Show Frametime Graph", &this->settings.ShowPreFGFrameTimeGraph);
		}

		ImGui::Spacing();
		ImGui::Spacing();

		// Appearance settings
		ImGui::TextUnformatted("Appearance");
		ImGui::Separator();

		ImGui::SliderFloat("Text Size", &this->settings.TextSize, 0.8f, 1.2f, "%.2f");
		ImGui::SliderFloat("Background Opacity", &this->settings.BackgroundOpacity, 0.0f, 1.0f, "%.2f");
		ImGui::Checkbox("Show Border", &this->settings.ShowBorder);
		ImGui::SliderFloat("Update Interval", &this->settings.UpdateInterval, 0.001f, PerformanceOverlay::Settings::kMaxUpdateInterval, "%.2f seconds");
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How often the FPS readout refreshes. Shorter = updates faster, but the number jumps around more.");
		}
		ImGui::SliderInt("Frame History Size", &this->settings.FrameHistorySize,
			this->settings.kMinFrameHistorySize, this->settings.kMaxFrameHistorySize);
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("How many recent frames the frametime graph shows.");
		}

		ImGui::Spacing();
		ImGui::Spacing();

		// (batch 36e) How the tables are smoothed, ordered and trimmed. The same controls are
		// in the overlay's own "View options" section.
		ImGui::TextUnformatted("Readability");
		ImGui::Separator();
		DrawViewOptions();

		ImGui::Separator();
		ImGui::Text("Position:");
		if (ImGui::Button("Reset Position")) {
			this->settings.PositionSet = false;
		}
		ImGui::SameLine();
		if (ImGui::Button("Restore Defaults")) {
			RestoreDefaultSettings();
		}
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted("Restores Performance Overlay settings to defaults, including graphs, appearance, and update intervals.");
		}

		ImGui::Unindent();
	}
}

void PerformanceOverlay::SaveSettings(json& j)
{
	// Persist all overlay settings to JSON
	j = this->settings;  // uses NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT
	// (batch 37a) The occlusion dry run's knobs live with the overlay that shows its numbers.
	Util::OcclusionDryRun::Save(j["OcclusionDryRun"]);
}

void PerformanceOverlay::LoadSettings(json& j)
{
	try {
		// Load all settings from JSON (missing fields use defaults)
		this->settings = j.get<PerformanceOverlay::Settings>();
	} catch (...) {
		// Fallback to defaults if JSON is invalid
		this->settings = PerformanceOverlay::Settings{};
	}
	// (batch 36e) A hand-edited or future config must not index past the option lists.
	this->settings.SortMode = std::clamp(this->settings.SortMode, 0, 2);
	this->settings.SmoothingMode = std::clamp(this->settings.SmoothingMode, 0, 2);
	this->settings.SmoothingWindow = std::clamp(this->settings.SmoothingWindow, 0.1f, 5.0f);
	this->settings.TopN = std::clamp(this->settings.TopN, 0, 64);
	this->settings.DenoiserLogInterval = std::clamp(this->settings.DenoiserLogInterval, 0, 600);
	if (j.is_object() && j.contains("OcclusionDryRun"))
		Util::OcclusionDryRun::Load(j["OcclusionDryRun"]);
	// Ensure history buffers match loaded size
	this->state.frameTimeHistory.Resize(this->settings.FrameHistorySize);
	this->state.postFGFrameTimeHistory.Resize(this->settings.FrameHistorySize);
}

void PerformanceOverlay::RestoreDefaultSettings()
{
	this->settings = PerformanceOverlay::Settings{};
	// Reset runtime buffers/state to match defaults
	this->state.frameTimeHistory.Resize(this->settings.FrameHistorySize);
	this->state.postFGFrameTimeHistory.Resize(this->settings.FrameHistorySize);
	this->state.smoothFps = 0.0f;
	this->state.smoothFrameTimeMs = 0.0f;
	this->state.postFGSmoothFps = 0.0f;
	this->state.postFGSmoothFrameTimeMs = 0.0f;
	this->state.minFrameTime = 1000.0f;
	this->state.maxFrameTime = 0.0f;
	this->state.smoothedMinFrameTime = 0.0f;
	this->state.smoothedMaxFrameTime = 50.0f;
	// Statistics must actually restart, not merely keep their capacity: leaving old
	// samples in place made Avg / 1% Low keep reporting the pre-restore state for
	// seconds afterwards.
	this->state.statsWindow.Resize(PerformanceOverlay::Settings::kStatsWindowMaxFrames);
	this->state.statsWindow.Clear();
	this->state.frameTimeHistory.Clear();
	this->state.postFGFrameTimeHistory.Clear();
	this->state.postFGIsMeasured = false;
	this->state.postFGMultiplier = 0.0f;
	// (batch 36e) Rows and their order restart from scratch too.
	this->view = ViewState{};
}

void PerformanceOverlay::DataLoaded()
{
	// Initialize performance overlay state
	REX::W32::QueryPerformanceFrequency(&this->state.frequency);
	REX::W32::QueryPerformanceCounter(&this->state.lastFrameCounter);
	this->state.frameClockPrimed = true;
	this->state.frameTimeHistory.Resize(this->settings.FrameHistorySize);
	this->state.postFGFrameTimeHistory.Resize(this->settings.FrameHistorySize);
	this->state.statsWindow.Resize(PerformanceOverlay::Settings::kStatsWindowMaxFrames);
}

void PerformanceOverlay::PostPostLoad()
{
	// Hooks that only bracket engine calls with a timeline scope; they cost one branch per
	// call while the overlay table is closed.
	Util::GpuPhaseTimeline::InstallHooks();
	// (batch 37a) Occlusion culling phase 0: counts only, never culls. SE 1.5.97 flat only.
	Util::OcclusionDryRun::Install();
}

void PerformanceOverlay::DrawOverlay()
{
	auto* menu = Menu::GetSingleton();

	if (!globals::state || !menu) {
		return;
	}
	if (!menu->overlayVisible) {
		return;
	}
	if (this->settings.ShowVRAM && (!menu->GetDXGIAdapter3())) {
		return;
	}
	if (!ImGui::GetCurrentContext()) {
		return;
	}
	if (!this->settings.ShowInOverlay) {
		return;
	}

	// Build draw call rows ONCE per frame and reuse
	auto rowSets = this->BuildDrawCallRows();
	std::vector<DrawCallRow> allRows = rowSets.cpuRows;
	allRows.insert(allRows.end(), rowSets.ourCpuRows.begin(), rowSets.ourCpuRows.end());
	allRows.insert(allRows.end(), rowSets.gpuRows.begin(), rowSets.gpuRows.end());
	allRows.insert(allRows.end(), rowSets.summaryRows.begin(), rowSets.summaryRows.end());

	// Set window flags - no decoration and only movable when ShowBorder is true
	ImGuiWindowFlags windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize;

	// Only allow mouse interaction when the main menu is open
	if (!menu->IsEnabled) {
		windowFlags |= ImGuiWindowFlags_NoInputs;
	}

	if (!this->settings.ShowBorder) {
		windowFlags |= ImGuiWindowFlags_NoBackground;
	} else {
		windowFlags &= ~ImGuiWindowFlags_NoDecoration;
		windowFlags &= ~ImGuiWindowFlags_AlwaysAutoResize;
		windowFlags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse;
	}

	// Set background opacity
	ImGui::PushStyleColor(ImGuiCol_WindowBg,
		ImVec4(ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).x,
			ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).y,
			ImGui::GetStyleColorVec4(ImGuiCol_WindowBg).z,
			this->settings.BackgroundOpacity));

	ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, this->settings.ShowBorder ? 1.0f : 0.0f);

	// Set initial position if not already set
	if (!this->settings.PositionSet) {
		ImGui::SetNextWindowPos(ImVec2(PerformanceOverlay::Settings::kDefaultWindowPadding, PerformanceOverlay::Settings::kDefaultWindowPadding));
		this->settings.Position = ImVec2(PerformanceOverlay::Settings::kDefaultWindowPadding, PerformanceOverlay::Settings::kDefaultWindowPadding);
		this->settings.PositionSet = true;
	} else {
		ImGui::SetNextWindowPos(this->settings.Position, ImGuiCond_FirstUseEver);
	}

	// Set window size based on whether graphs are shown, was rapidly changing size based on text
	bool hasGraphs = this->settings.ShowPreFGFrameTimeGraph ||
	                 (this->settings.ShowPostFGFrameTimeGraph && this->state.isFrameGenerationActive);
	if (!hasGraphs) {
		// Calculate minimum width needed based on actual content
		float minWidth = 0.0f;

		// Calculate width needed for each enabled section
		if (this->settings.ShowFPS) {
			// Measure FPS text width
			std::string fpsText = std::format("{:.1f} ({:.2f} ms)", this->state.smoothFps, this->state.smoothFrameTimeMs);
			if (this->state.isFrameGenerationActive) {
				fpsText = std::format("Raw FPS: {:.1f} ({:.2f} ms)", this->state.smoothFps, this->state.smoothFrameTimeMs);
			}
			float fpsWidth = ImGui::CalcTextSize(fpsText.c_str()).x;
			minWidth = std::max(minWidth, fpsWidth + PerformanceOverlay::Settings::kLabelPadding);  // Add padding for labels
		}
		if (this->settings.ShowDrawCalls) {
			// Draw calls table needs significant width for all columns
			minWidth = std::max(minWidth, PerformanceOverlay::Settings::kDrawCallsTableWidth * this->settings.TextSize);
		}
		if (this->settings.ShowVRAM && menu->GetDXGIAdapter3()) {
			// VRAM section needs width for the progress bar and text
			minWidth = std::max(minWidth, PerformanceOverlay::Settings::kVRAMSectionWidth * this->settings.TextSize);
		}

		// Add some padding for window borders and spacing
		minWidth += PerformanceOverlay::Settings::kWindowBorderPadding;

		// Set minimum width, but allow auto-resize for larger content
		ImGui::SetNextWindowSize(ImVec2(minWidth, 0), ImGuiCond_FirstUseEver);
	}

	// Create the window
	ImGui::Begin("Performance Overlay", NULL, windowFlags);

	// Remember window position for next frame
	if (ImGui::IsWindowAppearing()) {
		ImGui::SetWindowPos(this->settings.Position);
	}

	// Track if window has been moved
	ImVec2 currentPos = ImGui::GetWindowPos();
	if (currentPos.x != this->settings.Position.x || currentPos.y != this->settings.Position.y) {
		this->settings.Position = currentPos;
	}

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 1.0f));  // Tighter spacing
	ImGui::SetWindowFontScale(this->settings.TextSize);

	// Frame sampling deliberately does NOT happen here - it runs on every Present via
	// AdvanceFrameClock() so hiding the overlay cannot inject a bogus sample.

	// (batch 36e) Readability layer: feed this frame into the display state of every table,
	// unless frozen. Everything drawn below reads from that state, not from the timers.
	const PerfView::ViewConfig viewCfg = MakeViewConfig();
	{
		LARGE_INTEGER freq, counter;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&counter);
		const double now = static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
		if (!view.frozen)
			UpdateViews(now, rowSets, viewCfg);

		// Periodic denoiser line for the log (option; off by default). Only while the section
		// is open, which is also the only time its timers run.
		if (this->settings.ShowDrawCalls && this->settings.SectionDenoiser && this->settings.DenoiserLogInterval > 0 &&
			now - view.lastDenoiserLog >= static_cast<double>(this->settings.DenoiserLogInterval)) {
			view.lastDenoiserLog = now;
			const std::string line = FormatDenoiserLogLine(viewCfg);
			if (!line.empty())
				logger::info("{}", line);
		}
	}

	// Always on top, never collapsible: the four numbers that answer "how is it going".
	DrawCompactSummary(viewCfg);

	if (Section("View options", this->settings.SectionView, "Sorting, smoothing, Top N, Peak column and Freeze."))
		DrawViewOptions();

	// Show FPS counter if enabled
	if (this->settings.ShowFPS && Section("Frame rate & graphs", this->settings.SectionFrame)) {
		DrawFPS();
	}

	// Show Draw Calls if enabled
	if (this->settings.ShowDrawCalls) {
		if (Section("CPU / GPU split", this->settings.SectionBottleneck))
			DrawBottleneckSummary();
		if (Section("Shader types (CPU)", this->settings.SectionShaderTypes))
			DrawDrawCallsTable(rowSets.cpuRows, rowSets.summaryRows);
		if (Section("Community Shaders (CPU submit)", this->settings.SectionCsCpu))
			DrawOurCpuPassTable(rowSets.ourCpuRows);
		if (Section("GPU passes", this->settings.SectionGpuPasses))
			DrawGpuPassTable(rowSets.gpuRows);
		if (Section("Engine passes (GPU)", this->settings.SectionEngine))
			DrawEngineGpuTable();
		if (Section("Shadow maps", this->settings.SectionShadows,
				"How the sun shadow is set up this frame: cascade count and distances, character and lamp shadow maps. "
				"Their cost is in the Shadows rows of \"Engine passes\"."))
			DrawShadowInfo();
		if (Section("Occlusion (dry run)", this->settings.SectionOcclusion,
				"How many objects an occlusion cull could skip here. Counting only: nothing is ever hidden."))
			Util::OcclusionDryRun::DrawPanel(menu->IsEnabled);
		if (Section("Denoiser breakdown", this->settings.SectionDenoiser,
				"Every pass of the SSRT denoiser chain, timed on its own. Only measured while this section is open."))
			DrawDenoiserTable(viewCfg);
		if (globals::features::grassOptimizations.loaded && Section("Grass Optimizations", this->settings.SectionGrass))
			globals::features::grassOptimizations.DrawOverlayStats();
	}

	// VRAM & GPU Usage
	if (this->settings.ShowVRAM && menu->GetDXGIAdapter3() && Section("VRAM", this->settings.SectionVram)) {
		DrawVRAM();
	}

	// Freeze (and the "Log snapshot" button) asks for a snapshot; it is written here, after
	// every table has its display values for this frame.
	if (view.pendingSnapshot) {
		view.pendingSnapshot = false;
		WriteSnapshotToLog(viewCfg);
	}
	// (batch 37a) "Save frame (JSON)" and the Freeze key: same moment, same values.
	if (view.pendingJsonSave) {
		view.pendingJsonSave = false;
		SaveFrameJson(viewCfg);
	}

	ImGui::PopStyleVar();             // ItemSpacing
	ImGui::SetWindowFontScale(1.0f);  // Reset font scale

	// --- A/B Test Section ---
	DrawABTestSection(allRows);

	// (batch 36e) With a border the window is not auto-sized (its width used to jump with
	// the text), so collapsing a section left an empty box behind and expanding one hid rows
	// below the edge. Fit the height to the content every frame; the width stays the user's.
	if (this->settings.ShowBorder) {
		const float contentHeight = ImGui::GetCursorPosY() - ImGui::GetScrollY() + ImGui::GetStyle().WindowPadding.y;
		const float maxHeight = ImGui::GetIO().DisplaySize.y - ImGui::GetWindowPos().y;
		const float wantHeight = std::clamp(contentHeight, 50.0f, std::max(50.0f, maxHeight));
		if (std::abs(wantHeight - ImGui::GetWindowHeight()) > 1.0f)
			ImGui::SetWindowSize(ImVec2(ImGui::GetWindowWidth(), wantHeight));
	}

	ImGui::End();
	ImGui::PopStyleVar();    // WindowBorderSize
	ImGui::PopStyleColor();  // WindowBg
}
// ============================================================================
// CORE PERFORMANCE DISPLAY FUNCTIONS
// ============================================================================

void PerformanceOverlay::DrawFPS()
{
	// (batch 36e) While frozen, every number and graph here comes from the copy taken at the
	// moment of freezing.
	const State& st = (view.frozen && view.frozenState) ? *view.frozenState : state;
	if (ImGui::BeginTable("FrametimeTargets", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##prop", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 6);
		ImGui::TableSetupColumn("##value");

		ImGui::TableNextColumn();
		ImGui::Text(st.isFrameGenerationActive ? "Raw FPS:" : "FPS:");
		ImGui::TableNextColumn();
		ImGui::Text("%.1f (%.2f ms)", st.smoothFps, st.smoothFrameTimeMs);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Current frame rate, refreshed every Update Interval.");
			}
		}

		// Rolling statistics over the trailing kStatsWindowSeconds of frames: average and
		// 1% Low, shown alongside the instantaneous value so short stutters stay visible
		// in the numbers even when the instant readout looks fine.
		{
			const FrameStats stats = view.frozen ? view.frozenStats : ComputeFrameStats();

			ImGui::TableNextColumn();
			ImGui::Text("Avg (%.0fs):", Settings::kStatsWindowSeconds);
			ImGui::TableNextColumn();
			if (stats.valid) {
				ImGui::Text("%.1f (%.2f ms)", Util::CalcFPS(stats.averageMs), stats.averageMs);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Average over the last %.1f s (%d frames). Frames slower than %.0f ms (loading screens, alt-tab) are left out.",
							stats.seconds, stats.frames, Settings::kStatsMaxSampleMs);
					}
				}

				ImGui::TableNextColumn();
				ImGui::Text("1%% Low:");
				ImGui::TableNextColumn();
				ImGui::Text("%.1f (%.2f ms)", Util::CalcFPS(stats.percentile99Ms), stats.percentile99Ms);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("The frame rate that 99%% of frames beat over the last %.1f s (%d frames). With %d frames this is about the %d-th slowest frame, so it shows repeated stutter rather than a single hitch.",
							stats.seconds, stats.frames, stats.frames, std::max(1, stats.frames / 100));
					}
				}
			} else {
				ImGui::TextDisabled("collecting...");

				ImGui::TableNextColumn();
				ImGui::Text("1%% Low:");
				ImGui::TableNextColumn();
				ImGui::TextDisabled("collecting...");
			}
		}

		if (st.isFrameGenerationActive) {
			ImGui::TableNextColumn();
			ImGui::Text("Post-FG FPS:");
			ImGui::TableNextColumn();
			if (st.postFGIsMeasured) {
				ImGui::Text("%.1f (%.2f ms)", st.postFGSmoothFps, st.postFGSmoothFrameTimeMs);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Measured: %.2f frames shown on screen per frame rendered.", st.postFGMultiplier);
					}
				}
			} else {
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%.1f (%.2f ms) est.",
					st.postFGSmoothFps, st.postFGSmoothFrameTimeMs);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Estimate: this frame generation type does not report shown frames, so the %.0fx setting is assumed. Raw FPS, Avg and 1%% Low above are always measured before frame generation.",
							st.postFGMultiplier);
					}
				}
			}

			// The mode the backend accepted, beside what it measurably presents. This is the
			// line that answers "is my multiplier actually running": the setting can ask for 4x
			// while DLSS-G only reports 2x (or refused more), and then this reads 2x.
			ImGui::TableNextColumn();
			ImGui::Text("FG Mode:");
			ImGui::TableNextColumn();
			if (st.postFGIsMeasured)
				ImGui::Text("%ux (measured %.2fx)", st.appliedFGMultiplier, st.postFGMultiplier);
			else
				ImGui::Text("%ux", st.appliedFGMultiplier);
			if (st.rejectedFGMultiplier > 0) {
				ImGui::SameLine();
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "(%ux refused)", st.rejectedFGMultiplier);
			}
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					if (st.frameGenerationIsDLSSG)
						ImGui::TextUnformatted("Left: the multiplier DLSS-G is actually running, which can differ from the menu setting (\"refused\" = a multiplier it turned down). Right: the measured ratio, a bit lower when DLSS-G skips a generated frame to keep pacing smooth.");
					else
						ImGui::TextUnformatted("FSR 3.1 frame generation only supports 2x.");
				}
			}
		}

		ImGui::EndTable();
	}

	// Show Pre-FG frametime graph if enabled
	if (this->settings.ShowPreFGFrameTimeGraph) {
		// Prepare overlay text
		char overlay_text[128];
		snprintf(overlay_text, IM_ARRAYSIZE(overlay_text),
			"%s%.2f ms (%.1f FPS)",
			st.isFrameGenerationActive ? "Pre-FG: " : "",
			st.smoothFrameTimeMs, st.smoothFps);

		// Set graph colors
		ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));  // Green line

		// Draw the graph
		float graphWidth = ImGui::GetWindowWidth() * 0.9f;
		ImGui::PlotLines("##frametime",
			st.frameTimeHistory.GetData().data(),
			static_cast<int>(st.frameTimeHistory.GetData().size()),
			static_cast<int>(st.frameTimeHistory.GetHeadIdx()),
			overlay_text,
			st.smoothedMinFrameTime, st.smoothedMaxFrameTime,
			ImVec2(graphWidth, 50.0f * this->settings.TextSize));

		ImGui::PopStyleColor();

		// Draw frametime target reference lines
		if (ImGui::BeginTable("FrametimeTargets", 3, ImGuiTableFlags_SizingStretchSame)) {
			ImGui::TableNextColumn();
			ImGui::Text("30 FPS: 33.3 ms");

			ImGui::TableNextColumn();
			ImGui::Text("60 FPS: 16.7 ms");

			ImGui::TableNextColumn();
			ImGui::Text("120 FPS: 8.3 ms");

			ImGui::EndTable();
		}
	}

	// Show Post-FG frametime graph if enabled
	if (this->settings.ShowPostFGFrameTimeGraph && st.isFrameGenerationActive) {
		// State the provenance of the post-FG curve explicitly. Only DLSS-G reports a
		// presented-frame count; FSR 3 frame generation does not, so its curve is the
		// pre-FG curve scaled by a fixed multiplier and must be labelled as an estimate.
		if (st.postFGIsMeasured) {
			ImGui::Text("Post-FG: measured (%.2fx presented frames)", st.postFGMultiplier);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Based on the number of shown frames that frame generation reports for each rendered frame.");
			}
		} else {
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Post-FG: estimated (%.0fx Pre-FG)", st.postFGMultiplier);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("This frame generation type does not report shown frames, so this curve is just the Pre-FG curve divided by the multiplier. Treat it as an estimate.");
			}
		}

		this->DrawPostFGFrameTimeGraph();
	}
}

void PerformanceOverlay::DrawVRAM()
{
	auto menu = Menu::GetSingleton();
	if (!menu)
		return;
	auto dxgiAdapter3 = menu->GetDXGIAdapter3();
	if (!dxgiAdapter3)
		return;
	DXGI_QUERY_VIDEO_MEMORY_INFO videoMemoryInfo{};
	HRESULT hr = dxgiAdapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &videoMemoryInfo);

	// Only proceed if the call succeeded and Budget is not zero
	if (SUCCEEDED(hr) && videoMemoryInfo.Budget > 0) {
		float currentGpuUsage = videoMemoryInfo.CurrentUsage / (1024.f * 1024.f * 1024.f);
		float totalGpuMemory = videoMemoryInfo.Budget / (1024.f * 1024.f * 1024.f);
		float percent = currentGpuUsage / totalGpuMemory;

		// Center the VRAM text
		ImGui::Text("VRAM Usage:");

		// Use a centered text format for the numeric values
		std::string vramText = std::format("{:.2f}GB/{:.2f}GB ({:.1f}%)", currentGpuUsage, totalGpuMemory, 100 * percent);
		float textWidth = ImGui::CalcTextSize(vramText.c_str()).x;
		float windowWidth = ImGui::GetWindowWidth();

		// Center the text if it fits within the window
		if (textWidth < windowWidth) {
			ImGui::SetCursorPosX((windowWidth - textWidth) * 0.5f);
			ImGui::Text("%s", vramText.c_str());
		} else {
			ImGui::Text("%s", vramText.c_str());
		}

		// Only move the progress bar, not the text
		ImGui::ProgressBar(percent, ImVec2(ImGui::GetWindowWidth() * 0.9f, 0.0f), "");
	} else {
		// Display a fallback message if we couldn't get the VRAM info
		ImGui::Text("VRAM Usage: Not available");
	}
}

void PerformanceOverlay::DrawPostFGFrameTimeGraph()
{
	// (batch 36e) While frozen, the graph shows the copy taken at the moment of freezing.
	const State& st = (view.frozen && view.frozenState) ? *view.frozenState : state;
	// Prepare overlay text
	char overlay_text[128];
	snprintf(overlay_text, IM_ARRAYSIZE(overlay_text),
		"Post-FG: %.2f ms (%.1f FPS)",
		st.postFGSmoothFrameTimeMs, st.postFGSmoothFps);

	// Set graph colors - blue for post-FG
	ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.5f, 1.0f, 1.0f));  // Blue line

	// Draw the graph
	float graphWidth = ImGui::GetWindowWidth() * 0.9f;
	ImGui::PlotLines("##postfgframetime",
		st.postFGFrameTimeHistory.GetData().data(),
		static_cast<int>(st.postFGFrameTimeHistory.GetData().size()),
		static_cast<int>(st.postFGFrameTimeHistory.GetHeadIdx()),
		overlay_text,
		st.smoothedMinFrameTime, st.smoothedMaxFrameTime,
		ImVec2(graphWidth, 50.0f * settings.TextSize));

	ImGui::PopStyleColor();

	// Draw frametime target reference lines
	if (ImGui::BeginTable("PostFGFrametimeTargets", 3, ImGuiTableFlags_SizingStretchSame)) {
		ImGui::TableNextColumn();
		ImGui::Text("30 FPS: 33.3 ms");

		ImGui::TableNextColumn();
		ImGui::Text("60 FPS: 16.7 ms");

		ImGui::TableNextColumn();
		ImGui::Text("120 FPS: 8.3 ms");

		ImGui::EndTable();
	}
}

// ============================================================================
// A/B TESTING FUNCTIONS
// ============================================================================

// --- ABTestAggregator integration ---
ABTestAggregator& PerformanceOverlay::GetABTestAggregator()
{
	auto* abTestingManager = ABTestingManager::GetSingleton();
	return abTestingManager->GetAggregator();
}

/**
  * @brief Draws the A/B test results table with comprehensive performance comparison
  *
  * This function renders a detailed table showing performance metrics for both Variant A (USER config)
  * and Variant B (TEST config), including:
  * - Average and median frame times for each shader type
  * - Performance deltas and percentage differences
  * - Color-coded indicators for better/worse performance
  * - Statistical validity assessment with tooltips
  * - Sortable columns for easy analysis
  *
  * The table provides both main rows (individual shader types) and summary rows (Total, Other)
  * to give users a complete picture of performance differences between configurations.
  *
  * @note This function requires an active A/B test with aggregated results
  */
void PerformanceOverlay::DrawABTestResultsTable()
{
	auto* abTestingManager = ABTestingManager::GetSingleton();
	auto& aggregator = abTestingManager->GetAggregator();
	auto results = aggregator.GetAggregatedResults();
	if (results.empty())
		return;

	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	DrawABTestStatisticalValidity(theme, aggregator);

	std::vector<DrawCallRow> mainRows, summaryRows;
	ConvertABTestResultsToRows(results, mainRows, summaryRows);

	ABTestLegends legends = BuildABTestLegends(theme);

	auto columns = BuildABTestResultsTableColumns(theme, legends);

	std::vector<std::function<bool(const DrawCallRow&, const DrawCallRow&, bool)>> sorters;
	for (const auto& col : columns) sorters.push_back(col.sortFunc);
	std::vector<DrawCallRow> mainRowsCopy = mainRows;
	std::vector<DrawCallRow> summaryRowsCopy = summaryRows;
	Util::ShowSortedStringTableCustom<DrawCallRow>(
		"ABTestResultsTable",
		[&columns]() { std::vector<std::string> h; for (const auto& c : columns) h.push_back(c.header); return h; }(),
		mainRowsCopy,
		0,     // Default sort column (Shader Type)
		true,  // Default ascending
		sorters,
		[&columns](int rowIdx, int colIdx, const DrawCallRow& row) {
			(void)rowIdx;
			columns[colIdx].cellRender(row, colIdx);
		},
		summaryRowsCopy);
}

/**
  * @brief Draws statistical validity information for A/B test results
  *
  * This function displays test duration, valid frame counts, and exclusion rates
  * with color-coded indicators for statistical validity. It helps users understand
  * whether the A/B test results are reliable and statistically significant.
  *
  * @param theme The current UI theme settings
  * @param aggregator The A/B test aggregator containing test statistics
  */
void PerformanceOverlay::DrawABTestStatisticalValidity(const Menu::ThemeSettings& theme, const ABTestAggregator& aggregator) const
{
	float totalDuration = aggregator.GetTotalTestDuration();
	int totalFrames = aggregator.GetTotalFrameCount();
	int excludedFrames = 0;
	for (const auto& interval : aggregator.GetIntervals()) {
		excludedFrames += interval.excludedFrames;
	}
	int validFrames = totalFrames;
	int totalWithExcluded = totalFrames + excludedFrames;
	float validPercent = (totalWithExcluded > 0) ? (100.0f * validFrames / totalWithExcluded) : 100.0f;

	bool hasEnoughSamples = validFrames >= kMinimumSamplesForValidity;
	bool hasGoodDuration = totalDuration >= kMinimumTestDuration;
	bool hasLowExclusionRate = validPercent >= kMinimumValidFramesPercent;
	bool isStatisticallyValid = hasEnoughSamples && hasGoodDuration && hasLowExclusionRate;

	ImVec4 validityColor = theme.Palette.Text;
	if (isStatisticallyValid) {
		validityColor = theme.StatusPalette.SuccessColor;
	} else if (validFrames >= kMinimumSamplesForMarginal && totalDuration >= kMinimumDurationForMarginal) {
		validityColor = theme.StatusPalette.Warning;
	} else {
		validityColor = theme.StatusPalette.Error;
	}

	ImGui::PushStyleColor(ImGuiCol_Text, validityColor);
	ImGui::Text("Test Duration: %.1f seconds | Valid Frames: %d/%d (%.1f%%) | Excluded: %d",
		totalDuration, validFrames, totalWithExcluded, validPercent, excludedFrames);
	ImGui::PopStyleColor();
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			char validStr[128], marginalStr[128];
			snprintf(validStr, sizeof(validStr), "Statistically valid (>%d samples, >%.0fs duration, >%.0f%% valid)", kMinimumSamplesForValidity, static_cast<float>(kMinimumTestDuration), kMinimumValidFramesPercent);
			snprintf(marginalStr, sizeof(marginalStr), "Marginal validity (>%d samples, >%.0fs duration)", kMinimumSamplesForMarginal, static_cast<float>(kMinimumDurationForMarginal));
			Util::ColoredTextLines validityLegend = {
				{ "Valid frames are those not excluded as outliers.\nA low percentage may indicate instability or test interruptions.\nExcluded frames are those with frame times > 3x median or > 100ms.\nThis removes shader compilation spikes, JSON loading overhead, and other anomalies\nthat would skew the performance comparison.", theme.Palette.Text },
				{ "", theme.Palette.Text },
				{ validStr, theme.StatusPalette.SuccessColor },
				{ marginalStr, theme.StatusPalette.Warning },
				{ "Insufficient data for reliable results", theme.StatusPalette.Error }
			};
			Util::DrawColoredMultiLineTooltip(validityLegend);
		}
	}
}

/**
  * @brief Converts A/B test aggregated results into table rows
  *
  * This function transforms aggregated A/B test statistics into DrawCallRow structures
  * suitable for display in the performance overlay table. It separates main shader type
  * rows from summary rows (Total, Other) and assigns appropriate tooltips.
  *
  * @param results The aggregated A/B test results
  * @param mainRows Output vector for individual shader type rows
  * @param summaryRows Output vector for summary rows (Total, Other)
  */
void PerformanceOverlay::ConvertABTestResultsToRows(const std::vector<AggregatedDrawCallStats>& results, std::vector<DrawCallRow>& mainRows, std::vector<DrawCallRow>& summaryRows) const
{
	mainRows.clear();
	summaryRows.clear();
	for (const auto& stat : results) {
		DrawCallRow row;
		row.label = stat.label;
		row.shaderType = stat.shaderType;
		row.frameTime = stat.meanA;
		row.percent = (stat.meanA > 0.0f) ? (stat.meanA / (stat.meanA + stat.meanB) * 100.0f) : 0.0f;
		row.costPerCall = stat.medianA;
		row.enabled = true;
		row.testFrameTime = stat.meanB;
		row.testCostPerCall = stat.medianB;
		if (row.shaderType >= 0) {
			auto shaderType = static_cast<RE::BSShader::Type>(row.shaderType);
			auto tipIt = kShaderTypeTooltips.find(shaderType);
			if (tipIt != kShaderTypeTooltips.end()) {
				row.tooltip = tipIt->second;
			} else {
				row.tooltip = "Draw calls for this shader type.";
			}
		} else {
			auto maybeSpecialType = magic_enum::enum_cast<SpecialShaderType>(row.shaderType);
			if (maybeSpecialType.has_value()) {
				switch (*maybeSpecialType) {
				case SpecialShaderType::Total:
					row.tooltip = "Total frame time.";
					break;
				case SpecialShaderType::Other:
					row.tooltip = "Frame time not covered by the other rows: the game engine's own work (scripts, physics, animation, AI, audio, vanilla UI), driver overhead and other SKSE plugins. Cannot be broken down further.";
					break;
				default:
					// OurCpu / PresentWait are live-only rows; the A/B aggregator records
					// Total and Other, so nothing else reaches here.
					break;
				}
			}
		}
		if (row.shaderType < 0) {
			summaryRows.push_back(row);
		} else {
			mainRows.push_back(row);
		}
	}
}

/**
  * @brief Builds color-coded legends for A/B test table columns
  *
  * This function creates comprehensive tooltip legends for each A/B test column,
  * explaining the meaning of colors and values. The legends help users understand
  * performance comparisons between Variant A (USER) and Variant B (TEST) configurations.
  *
  * @param theme The current UI theme settings
  * @return ABTestLegends structure containing all column legends
  */
ABTestLegends PerformanceOverlay::BuildABTestLegends(const Menu::ThemeSettings& theme) const
{
	ABTestLegends legends;

	legends.shaderType = {
		"Shader Type",
		{ { "Shader Type: The type of shader being measured.", theme.Palette.Text },
			{ "Click to toggle shader on/off for performance testing.", theme.Palette.Text } }
	};

	legends.aAvg = {
		"A Avg (ms)",
		{ { "A Avg (ms): Average frame time for Variant A (USER config).", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (compared to Variant B):", theme.Palette.Text },
			{ "  Better (lower than B)", theme.StatusPalette.SuccessColor },
			{ "  Worse (higher than B)", theme.StatusPalette.Error },
			{ "  Same as B", theme.Palette.Text } }
	};

	legends.bAvg = {
		"B Avg (ms)",
		{ { "B Avg (ms): Average frame time for Variant B (TEST config).", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (compared to Variant A):", theme.Palette.Text },
			{ "  Better (lower than A)", theme.StatusPalette.SuccessColor },
			{ "  Worse (higher than A)", theme.StatusPalette.Error },
			{ "  Same as A", theme.Palette.Text } }
	};

	legends.delta = {
		"Delta (ms)",
		{ { "Delta (ms): Difference between Variant B and Variant A (B - A).", theme.Palette.Text },
			{ "Negative values indicate Variant B is better (lower frame time).", theme.Palette.Text },
			{ "Positive values indicate Variant A is better (lower frame time).", theme.Palette.Text },
			{ "Percentage shows relative performance difference.", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend:", theme.Palette.Text },
			{ "  Negative (B better)", theme.StatusPalette.SuccessColor },
			{ "  Positive (A better)", theme.StatusPalette.Error },
			{ "  Zero (same)", theme.Palette.Text } }
	};

	legends.aMedian = {
		"A Median (ms)",
		{ { "A Median: Median frame time for Variant A (USER config).", theme.Palette.Text },
			{ "Median is less sensitive to outliers than average.", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (compared to Variant B median):", theme.Palette.Text },
			{ "  Better (lower than B)", theme.StatusPalette.SuccessColor },
			{ "  Worse (higher than B)", theme.StatusPalette.Error },
			{ "  Same as B", theme.Palette.Text } }
	};

	legends.bMedian = {
		"B Median (ms)",
		{ { "B Median: Median frame time for Variant B (TEST config).", theme.Palette.Text },
			{ "Median is less sensitive to outliers than average.", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (compared to Variant A median):", theme.Palette.Text },
			{ "  Better (lower than A)", theme.StatusPalette.SuccessColor },
			{ "  Worse (higher than A)", theme.StatusPalette.Error },
			{ "  Same as A", theme.Palette.Text } }
	};

	legends.medianDelta = {
		"Median Delta (ms)",
		{ { "Median Delta: Difference between Variant B and Variant A medians (B - A).", theme.Palette.Text },
			{ "Negative values indicate Variant B is better (lower median).", theme.Palette.Text },
			{ "Positive values indicate Variant A is better (lower median).", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend:", theme.Palette.Text },
			{ "  Negative (B better)", theme.StatusPalette.SuccessColor },
			{ "  Positive (A better)", theme.StatusPalette.Error },
			{ "  Zero (same)", theme.Palette.Text } }
	};

	return legends;
}

/**
  * @brief Builds column configurations for the A/B test results table
  *
  * This function creates column configurations for displaying A/B test results,
  * including average and median frame times for both variants, performance deltas,
  * and color-coded indicators for better/worse performance comparisons.
  *
  * @param theme The current UI theme settings
  * @param legends The color-coded legends for tooltips
  * @return Vector of column configurations for the table
  */
std::vector<ColumnConfig> PerformanceOverlay::BuildABTestResultsTableColumns(const Menu::ThemeSettings& theme, const ABTestLegends& legends) const
{
	std::vector<ColumnConfig> columns = {
		{ legends.shaderType.header,
			[theme](const DrawCallRow& row, int) {
				ImGui::TextUnformatted(row.label.c_str());
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted(row.tooltip.c_str());
						// Add FPS for Total row
						if (row.label == "Total:") {
							float fps = row.frameTime > 0.0f ? 1000.0f / row.frameTime : 0.0f;
							ImGui::Text("FPS: %.2f", fps);
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.label < b.label) : (a.label > b.label); },
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.shaderType.tooltip);
					}
				}
			} },
		{ legends.aAvg.header,
			[theme, legends](const DrawCallRow& row, int) {
				float value = row.frameTime;
				// Color A relative to B
				ImVec4 color = theme.Palette.Text;
				if (row.testFrameTime.has_value()) {
					if (value < *row.testFrameTime) {
						color = theme.StatusPalette.SuccessColor;  // A is better (lower) than B
					} else if (value > *row.testFrameTime) {
						color = theme.StatusPalette.Error;  // A is worse (higher) than B
					}
				}
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::Text("%s", Util::FormatMilliseconds(value).c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.label == "Total:") {
							ImGui::Text("A (USER) FPS: %.2f", Util::CalcFPS(value));
						} else {
							Util::DrawColoredMultiLineTooltip(legends.aAvg.tooltip);
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.frameTime < b.frameTime) : (a.frameTime > b.frameTime); },
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.aAvg.tooltip);
					}
				}
			} },
		{ legends.bAvg.header,
			[theme, legends](const DrawCallRow& row, int) {
				if (!row.testFrameTime.has_value()) {
					ImGui::TextDisabled("-");
					return;
				}
				float value = *row.testFrameTime;
				// Color B relative to A
				ImVec4 color = theme.Palette.Text;
				if (value < row.frameTime) {
					color = theme.StatusPalette.SuccessColor;  // B is better (lower) than A
				} else if (value > row.frameTime) {
					color = theme.StatusPalette.Error;  // B is worse (higher) than A
				}
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::Text("%s", Util::FormatMilliseconds(value).c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.label == "Total:") {
							ImGui::Text("B (TEST) FPS: %.2f", Util::CalcFPS(value));
						} else {
							Util::DrawColoredMultiLineTooltip(legends.bAvg.tooltip);
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				float aVal = a.testFrameTime.value_or(FLT_MAX);
				float bVal = b.testFrameTime.value_or(FLT_MAX);
				return asc ? (aVal < bVal) : (aVal > bVal);
			},
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.bAvg.tooltip);
					}
				}
			} },
		{ legends.delta.header,
			[theme, legends](const DrawCallRow& row, int) {
				if (!row.testFrameTime.has_value()) {
					ImGui::TextDisabled("-");
					return;
				}
				float delta = *row.testFrameTime - row.frameTime;
				// Color based on delta
				ImVec4 color = theme.Palette.Text;
				if (delta < 0.0f) {
					color = theme.StatusPalette.SuccessColor;  // Better performance (negative delta)
				} else if (delta > 0.0f) {
					color = theme.StatusPalette.Error;  // Worse performance (positive delta)
				}
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::Text("%s", Util::FormatDeltaWithPercent(row.frameTime, *row.testFrameTime, PerformanceOverlay::Settings::kPercentDisplayThreshold).c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.testFrameTime.has_value()) {
							// Show detailed values for rows with test data
							if (row.label == "Total:") {
								ImGui::TextUnformatted("Delta (B - A):");
								ImGui::Separator();
								ImGui::Text("A (USER) FPS: %.2f", Util::CalcFPS(row.frameTime));
								ImGui::Text("B (TEST) FPS: %.2f", Util::CalcFPS(*row.testFrameTime));
							} else {
								ImGui::TextUnformatted("Delta (B - A):");
								ImGui::Separator();
								ImGui::Text("A (USER): %.3f ms", row.frameTime);
								ImGui::Text("B (TEST): %.3f ms", *row.testFrameTime);
							}
							ImGui::Separator();
						}
						// Always show the delta legend for explanation
						Util::DrawColoredMultiLineTooltip(legends.delta.tooltip);
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				float aDelta = a.testFrameTime.value_or(0.0f) - a.frameTime;
				float bDelta = b.testFrameTime.value_or(0.0f) - b.frameTime;
				return asc ? (aDelta < bDelta) : (aDelta > bDelta);
			},
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.delta.tooltip);
					}
				}
			} },
		{ legends.aMedian.header,
			[theme, legends](const DrawCallRow& row, int) {
				float value = row.costPerCall;
				// Color A median relative to B median (stored in testCostPerCall for now)
				ImVec4 color = theme.Palette.Text;
				if (row.testCostPerCall.has_value()) {
					if (value < *row.testCostPerCall) {
						color = theme.StatusPalette.SuccessColor;  // A is better (lower) than B
					} else if (value > *row.testCostPerCall) {
						color = theme.StatusPalette.Error;  // A is worse (higher) than B
					}
				}
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::Text("%s", Util::FormatMilliseconds(value).c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.label == "Total:") {
							Util::ColoredTextLines fpsTooltip{
								{ std::format("A (USER) Median FPS: {:.2f}", Util::CalcFPS(value)), ImVec4(1.0f, 1.0f, 1.0f, 1.0f) }
							};
							Util::DrawColoredMultiLineTooltip(fpsTooltip);
						} else {
							Util::DrawColoredMultiLineTooltip(legends.aMedian.tooltip);
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.costPerCall < b.costPerCall) : (a.costPerCall > b.costPerCall); },
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.aMedian.tooltip);
					}
				}
			} },
		{ legends.bMedian.header,
			[theme, legends](const DrawCallRow& row, int) {
				if (!row.testCostPerCall.has_value()) {
					ImGui::TextDisabled("-");
					return;
				}
				float value = *row.testCostPerCall;
				// Color B median relative to A median
				ImVec4 color = theme.Palette.Text;
				if (value < row.costPerCall) {
					color = theme.StatusPalette.SuccessColor;  // B is better (lower) than A
				} else if (value > row.costPerCall) {
					color = theme.StatusPalette.Error;  // B is worse (higher) than A
				}
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				ImGui::Text("%s", Util::FormatMilliseconds(value).c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.label == "Total:") {
							Util::ColoredTextLines fpsTooltip{
								{ std::format("B (TEST) Median FPS: {:.2f}", Util::CalcFPS(value)), ImVec4(1.0f, 1.0f, 1.0f, 1.0f) }
							};
							Util::DrawColoredMultiLineTooltip(fpsTooltip);
						} else {
							Util::DrawColoredMultiLineTooltip(legends.bMedian.tooltip);
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				float aVal = a.testCostPerCall.value_or(FLT_MAX);
				float bVal = b.testCostPerCall.value_or(FLT_MAX);
				return asc ? (aVal < bVal) : (aVal > bVal);
			},
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.bMedian.tooltip);
					}
				}
			} },
		{ legends.medianDelta.header,
			[theme, legends](const DrawCallRow& row, int) {
				if (!row.testCostPerCall.has_value()) {
					ImGui::TextDisabled("-");
					return;
				}
				float delta = *row.testCostPerCall - row.costPerCall;
				// Color based on delta
				ImVec4 color = theme.Palette.Text;
				if (delta < 0.0f) {
					color = theme.StatusPalette.SuccessColor;  // Better performance (negative delta)
				} else if (delta > 0.0f) {
					color = theme.StatusPalette.Error;  // Worse performance (positive delta)
				}
				ImGui::PushStyleColor(ImGuiCol_Text, color);
				std::string deltaStr = (delta > 0.0f) ? "+" + Util::FormatMilliseconds(delta) : Util::FormatMilliseconds(delta);
				ImGui::Text("%s", deltaStr.c_str());
				ImGui::PopStyleColor();
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.label == "Total:" && row.testCostPerCall.has_value()) {
							Util::ColoredTextLines fpsTooltip{
								{ "Median Delta (B - A):", ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
								{ "", ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
								{ std::format("A (USER) Median FPS: {:.2f}", Util::CalcFPS(row.costPerCall)), ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
								{ std::format("B (TEST) Median FPS: {:.2f}", Util::CalcFPS(*row.testCostPerCall)), ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
								{ "", ImVec4(1.0f, 1.0f, 1.0f, 1.0f) },
								{ "Median is less sensitive to outliers than average.", ImVec4(1.0f, 1.0f, 1.0f, 1.0f) }
							};
							Util::DrawColoredMultiLineTooltip(fpsTooltip);
						} else {
							Util::DrawColoredMultiLineTooltip(legends.medianDelta.tooltip);
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				float aDelta = a.testCostPerCall.value_or(0.0f) - a.costPerCall;
				float bDelta = b.testCostPerCall.value_or(0.0f) - b.costPerCall;
				return asc ? (aDelta < bDelta) : (aDelta > bDelta);
			},
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.medianDelta.tooltip);
					}
				}
			} }
	};

	return columns;
}

/**
  * @brief Draws the A/B testing section of the performance overlay
  *
  * This function handles all A/B testing related UI including:
  * - A/B test state management and data collection
  * - Display of aggregated A/B test results
  * - Settings difference comparison table
  * - A/B test controls (clear results, show/hide settings diff)
  *
  * @param allRows The current draw call rows for data collection
  */
void PerformanceOverlay::DrawABTestSection(const std::vector<DrawCallRow>& allRows)
{
	auto* menu = Menu::GetSingleton();
	auto* abTestingManager = ABTestingManager::GetSingleton();
	bool abTestingEnabled = abTestingManager && abTestingManager->IsEnabled();
	static ABVariant lastVariant = ABVariant::A;
	static bool lastUsingTestConfig = false;
	static bool wasAbTestActive = false;
	bool currentUsingTestConfig = abTestingManager && abTestingManager->IsUsingTestConfig();
	static std::string lastSettingsA, lastSettingsB;
	std::string currentSettingsA, currentSettingsB;
	auto& aggregator = abTestingManager->GetAggregator();
	if (abTestingEnabled) {
		// Serialize current settings for A and B from the aggregator
		if (aggregator.HasSettingsA())
			currentSettingsA = aggregator.GetSettingsA().dump();
		if (aggregator.HasSettingsB())
			currentSettingsB = aggregator.GetSettingsB().dump();
	}
	// Detect A/B test start/stop and variant switches
	bool settingsChanged = (currentSettingsA != lastSettingsA) || (currentSettingsB != lastSettingsB);
	if (abTestingEnabled && (!wasAbTestActive || settingsChanged)) {
		aggregator.Clear();
		aggregator.OnABSwitch(currentUsingTestConfig ? ABVariant::B : ABVariant::A);
		lastSettingsA = currentSettingsA;
		lastSettingsB = currentSettingsB;
	}
	if (abTestingEnabled && (currentUsingTestConfig != lastUsingTestConfig)) {
		aggregator.OnABSwitch(currentUsingTestConfig ? ABVariant::B : ABVariant::A);
	}
	if (!abTestingEnabled && wasAbTestActive) {
		aggregator.OnTestEnd();
	}
	wasAbTestActive = abTestingEnabled;
	lastUsingTestConfig = currentUsingTestConfig;

	// --- A/B Test Data Collection ---
	if (abTestingEnabled) {
		aggregator.OnFrame(allRows);  // Pass both main and summary rows
	}

	// Display A/B test results if available
	if (aggregator.HasResults()) {
		this->DrawABTestResultsTable();
		ImGui::Separator();
		// --- A/B Results Controls ---
		static bool showSettingsDiff = false;
		ImGui::BeginGroup();
		if (ImGui::Button(showSettingsDiff ? "Hide Settings Diff" : "Show Settings Diff")) {
			showSettingsDiff = !showSettingsDiff;
		}
		ImGui::SameLine();
		if (ImGui::Button("Clear A/B Test Results")) {
			aggregator.Clear();
			this->settingsDiff.clear();
			this->settingsDiffLoaded = false;
			showSettingsDiff = false;
			ImGui::EndGroup();
			ImGui::Separator();
			return;
		}
		ImGui::EndGroup();
		// --- Settings diff section (inline, toggled) ---
		if (showSettingsDiff) {
			if (!this->settingsDiffLoaded) {
				std::filesystem::path userPath = Util::PathHelpers::GetDataPath() / "SKSE/Plugins/CommunityShaders/SettingsUser.json";
				std::filesystem::path testPath = Util::PathHelpers::GetDataPath() / "SKSE/Plugins/CommunityShaders/SettingsTest.json";
				this->settingsDiff = Util::FileSystem::LoadJsonDiff(userPath, testPath);
				this->settingsDiffLoaded = true;
			}
			ImGui::TextUnformatted("Differences between USER (A) and TEST (B) configs:");
			if (this->settingsDiff.empty()) {
				ImGui::TextUnformatted("No setting changes detected between USER (A) and TEST (B) configs.");
			} else if (ImGui::BeginTable("ABSettingsDiffTable", 3, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Sortable)) {
				ImGui::TableSetupColumn("Setting Path", ImGuiTableColumnFlags_DefaultSort);
				ImGui::TableSetupColumn("A Value");
				ImGui::TableSetupColumn("B Value");
				ImGui::TableHeadersRow();

				// Determine which variant performed better based on Total row
				bool variantABetter = false;
				bool variantBBetter = false;
				auto results = aggregator.GetAggregatedResults();
				for (const auto& stat : results) {
					auto maybeSpecialType = magic_enum::enum_cast<SpecialShaderType>(stat.shaderType);
					if (maybeSpecialType.has_value() && *maybeSpecialType == SpecialShaderType::Total) {  // Total row
						if (stat.meanA < stat.meanB) {
							variantABetter = true;  // A has lower frame time (better)
						} else if (stat.meanB < stat.meanA) {
							variantBBetter = true;  // B has lower frame time (better)
						}
						break;
					}
				}

				// Get theme for color coding
				const auto& theme = menu->GetTheme();

				// Sort the settings diff if needed
				std::vector<SettingsDiffEntry> sortedDiff = this->settingsDiff;
				if (const ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
					if (sortSpecs->SpecsCount > 0) {
						int sortCol = sortSpecs->Specs->ColumnIndex;
						bool sortAsc = sortSpecs->Specs->SortDirection == ImGuiSortDirection_Ascending;
						std::sort(sortedDiff.begin(), sortedDiff.end(), [sortCol, sortAsc](const SettingsDiffEntry& a, const SettingsDiffEntry& b) {
							if (sortCol == 0)
								return sortAsc ? (a.path < b.path) : (a.path > b.path);
							if (sortCol == 1)
								return sortAsc ? (a.aValue < b.aValue) : (a.aValue > b.aValue);
							if (sortCol == 2)
								return sortAsc ? (a.bValue < b.bValue) : (a.bValue > b.bValue);
							return false;
						});
					}
				}
				for (const auto& entry : sortedDiff) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::TextUnformatted(entry.path.c_str());
					// Only show the path as text, no custom tooltip guessing
					ImGui::TableSetColumnIndex(1);
					// Color A value based on performance
					if (variantABetter) {
						ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.SuccessColor);
						ImGui::TextUnformatted(entry.aValue.c_str());
						ImGui::PopStyleColor();
					} else if (variantBBetter) {
						ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.Error);
						ImGui::TextUnformatted(entry.aValue.c_str());
						ImGui::PopStyleColor();
					} else {
						ImGui::TextUnformatted(entry.aValue.c_str());
					}
					ImGui::TableSetColumnIndex(2);
					// Color B value based on performance
					if (variantBBetter) {
						ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.SuccessColor);
						ImGui::TextUnformatted(entry.bValue.c_str());
						ImGui::PopStyleColor();
					} else if (variantABetter) {
						ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.Error);
						ImGui::TextUnformatted(entry.bValue.c_str());
						ImGui::PopStyleColor();
					} else {
						ImGui::TextUnformatted(entry.bValue.c_str());
					}
				}
				ImGui::EndTable();
			}
			ImGui::Separator();
		}
	}
}
// ============================================================================
// TABLE BUILDING AND RENDERING FUNCTIONS
// ============================================================================

// Private helper for table rendering
void PerformanceOverlay::DrawDrawCallsTable(const std::vector<DrawCallRow>& mainRows, const std::vector<DrawCallRow>& summaryRows)
{
	static bool clearTestDataRequested = false;
	auto& overlay = globals::features::performanceOverlay;
	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	// Capture test data and handle clear button
	overlay.CaptureTestData();
	bool anyTestData = !overlay.testData.empty();
	if (anyTestData) {
		if (ImGui::Button("Clear Test Data")) {
			clearTestDataRequested = true;
		}
	}

	// Build legends and column configurations
	auto legends = overlay.BuildDrawCallLegends(theme, anyTestData);
	auto columns = overlay.BuildDrawCallTableColumns(theme, legends, anyTestData);

	// (batch 36e) The live rows only feed the view layer (UpdateViews); what is drawn is the
	// stable, smoothed row set in its hysteresis order, with the summary rows pinned below.
	(void)mainRows;
	(void)summaryRows;
	const auto cfg = overlay.MakeViewConfig();
	float totalMs = 0.0f;
	overlay.view.shaderFooter.DisplayValue(magic_enum::enum_integer(SpecialShaderType::Total), cfg, totalMs);
	const auto mainView = MaterializeRows(overlay.view.shaderTypes, overlay.view.shaderTypes.DisplayOrder(cfg), cfg, totalMs, true);
	PerfView::ViewConfig footerCfg = cfg;
	footerCfg.sort = PerfView::SortMode::Fixed;
	footerCfg.topN = 0;
	const auto footerView = MaterializeRows(overlay.view.shaderFooter, overlay.view.shaderFooter.DisplayOrder(footerCfg), cfg, totalMs, true);

	// Create table row handler
	auto rowHandler = overlay.CreateTableRowHandler(columns);
	DrawStableTable("DrawCallOverlayTable", columns, mainView, footerView, rowHandler);

	// Handle clear test data request
	if (clearTestDataRequested) {
		overlay.ClearTestData();
		clearTestDataRequested = false;
	}
}

/**
 * @brief Renders the GPU timestamp buckets as their own table.
 *
 * Kept separate from the draw-call table on purpose. These numbers come from D3D11
 * timestamp queries on the GPU clock, are read back a few frames late, and can overlap
 * each other and the CPU timeline. Mixing them into the CPU attribution and subtracting
 * them from "Other" was the bug that produced negative residuals; here they are simply
 * reported, and the summary row is an honest sum of the buckets rather than a residual.
 *
 * @param gpuRows Bucket rows built by BuildDrawCallRows(); empty when no bucket is active.
 */
void PerformanceOverlay::DrawGpuPassTable(const std::vector<DrawCallRow>& gpuRows)
{
	(void)gpuRows;  // (batch 36e) fed into view.gpuPasses by UpdateViews; drawn from there
	auto& overlay = globals::features::performanceOverlay;
	if (overlay.view.gpuPasses.RowCount() == 0 && overlay.view.gpuFooter.RowCount() == 0) {
		ImGui::TextDisabled("No GPU pass has run yet.");
		return;
	}

	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	ImGui::TextDisabled("(hover for how to read this)");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"GPU time of Community Shaders' own effects. It is separate from the CPU table above, "
				"and passes overlap, so these rows do not add up to frame time.\n\n"
				"A row stays in place once it has appeared; \"-\" means its effect did not run recently. "
				"\"< 0.01 ms\" means it ran but was too small to measure.\n\n"
				"The four rows at the bottom do add up to the whole GPU frame:\n"
				"  Measured GPU + Untracked GPU + Gap = GPU frame (elapsed)");
		}
	}

	bool anyTestData = !overlay.testData.empty();
	auto legends = overlay.BuildDrawCallLegends(theme, anyTestData);
	auto columns = overlay.BuildPassTableColumns(theme, legends, anyTestData, "GPU Time (%)",
		"Intervals: how many separately timed pieces of work this row adds up each frame "
		"(Volumetric Lighting has four, for example).");

	const auto cfg = overlay.MakeViewConfig();
	// Percentages stay a share of the CPU frame (the Total row of the shader table), as before.
	float totalMs = 0.0f;
	overlay.view.shaderFooter.DisplayValue(magic_enum::enum_integer(SpecialShaderType::Total), cfg, totalMs);

	const auto mainView = MaterializeRows(overlay.view.gpuPasses, overlay.view.gpuPasses.DisplayOrder(cfg), cfg, totalMs, false);
	PerfView::ViewConfig footerCfg = cfg;
	footerCfg.sort = PerfView::SortMode::Fixed;
	footerCfg.topN = 0;
	const auto footerView = MaterializeRows(overlay.view.gpuFooter, overlay.view.gpuFooter.DisplayOrder(footerCfg), cfg, totalMs, false);

	// Plain handler: pass rows have no toggle and no summary-row special cases.
	std::function<void(int, int, const DrawCallRow&)> rowHandler =
		[&columns](int, int colIdx, const DrawCallRow& row) { columns[colIdx].cellRender(row, colIdx); };
	DrawStableTable("GpuPassOverlayTable", columns, mainView, footerView, rowHandler);
}

namespace
{
	// (batch 36) Layout of the "Engine passes (GPU)" table. A group with a label gets a header
	// row carrying the sum of its rows; a group without one shows its rows at the top level.
	struct EnginePhaseRow
	{
		Util::GpuPhase phase;
		const char* label;
		const char* tooltip;
	};

	struct EnginePhaseGroup
	{
		const char* label;  // nullptr: rows are shown at the top level
		const char* tooltip;
		std::vector<EnginePhaseRow> rows;
		bool countsDraws = true;  // false: our own GPU work, where "draws" mostly means nothing
	};

	const std::vector<EnginePhaseGroup>& EnginePhaseLayout()
	{
		using P = Util::GpuPhase;
		static const std::vector<EnginePhaseGroup> layout = {
			{ nullptr, nullptr,
				{ { P::WaterPrep, "Water prep",
					"Water work done before the main scene: water reflections and ripples." } } },
			{ "Shadows", "Drawing the shadow maps, and applying them to the screen.",
				{
					// (batch 37a) Named by the shadow map each draw goes into, not by arrival order.
					// The labels of these rows are filled in per frame (EngineRowLabel) from the
					// number of sun cascades the game is set to (iNumSplits).
					{ P::ShadowGodRay1, "God-ray shadow, cascade 1",
						"A second copy of the sun shadow map, cascade by cascade, used only for god rays\n"
						"(volumetric light). The game draws it before the real sun shadows, with fewer objects." },
					{ P::ShadowGodRay2, "God-ray shadow, cascade 2", "God-ray shadow map, next cascade out." },
					{ P::ShadowGodRay3, "God-ray shadow, cascade 3", "God-ray shadow map, third cascade." },
					{ P::ShadowSun1, "Sun shadow, cascade 1",
						"Sun/moon shadows are drawn in distance bands (cascades).\n"
						"The near cascade is the sharp one around you; the far one covers everything out to the shadow distance." },
					{ P::ShadowSun2, "Sun shadow, cascade 2", "Sun/moon shadow map, next cascade out." },
					{ P::ShadowSun3, "Sun shadow, cascade 3", "Sun/moon shadow map, third cascade." },
					{ P::ShadowFocus, "Character shadows",
						"Extra close-up shadow maps the game draws for a few characters near you (iNumFocusShadow)." },
					{ P::ShadowLocalLights, "Lamps & torches", "Shadow maps of shadow-casting lamps, torches and spells." },
					{ P::ShadowMask, "Shadow mask", "Full-screen passes that work out which pixels are in shadow." },
					{ P::ShadowOther, "Other shadow work", "Shadow-pass time outside the per-light drawing (setup, clears)." },
				} },
			{ nullptr, nullptr,
				{ { P::DepthPrepass, "Depth prepass", "The engine's depth-only pass over the scene,\nbefore the main geometry is drawn." } } },
			{ "Opaque geometry", "The main pass that draws all solid geometry.",
				{
					{ P::OpaqueTerrain, "Terrain", "Landscape around the player." },
					{ P::OpaqueObjects, "Objects", "Solid objects: buildings, rocks, clutter, furniture..." },
					{ P::OpaqueCharacters, "Characters", "People and creatures: bodies, faces, hair, eyes." },
					{ P::OpaqueTrees, "Trees", "Full-detail trees." },
					{ P::OpaqueGrass, "Grass", "Grass." },
					{ P::OpaqueDistant, "Distant LOD", "Far-away terrain, objects and trees (LOD)." },
					{ P::OpaqueOther, "Other", "Anything else in the solid pass: decals, effect meshes, clears." },
				} },
			{ nullptr, nullptr,
				{
					{ P::Sky, "Sky", "Sky, clouds, sun, moons and stars." },
					{ P::Water, "Water", "Water surfaces in the main view." },
					{ P::Transparent, "Transparent & effects",
						"See-through things drawn after the solid pass:\n"
						"glass, particles, spell effects, fire, smoke, rain." },
					{ P::WorldOther, "Other world work", "Main scene work that could not be assigned to any row here." },
					{ P::FirstPerson, "First person", "Your hands and weapon in first person." },
					{ P::Reflections, "Reflections", "The engine's cubemap reflections." },
					{ P::Imagespace, "Post-processing (game)",
						"The game's own image effects: bloom, tonemapping, TAA,\n"
						"depth of field, underwater, and similar." },
					{ P::UI, "UI", "HUD and menus." },
				} },
			{ "Community Shaders", "Our own GPU work, listed so the table adds up to the whole frame.",
				{
					{ P::CsPasses, "Timed passes",
						"Our passes from the GPU Passes table above.\n"
						"Should be close to its \"Measured GPU\" line." },
					{ P::CsOther, "Other CS work",
						"Our work without a GPU Passes row:\n"
						"the deferred lighting composite, feature prepasses, setup." },
					{ P::CsUpscaling, "Upscaling",
						"DLSS / FSR upscaling, sharpening and the copies around them.\n"
						"With frame generation on, part of DLSS can run on another\n"
						"GPU queue and then does not show here." },
					{ P::CsOverlay, "This overlay", "Drawing this overlay and the Community Shaders menu." },
				},
				false },
			{ nullptr, nullptr,
				{ { P::Untracked, "Untracked",
					"GPU time inside the frame that no row above covers.\n"
					"Small is good: the rows above then explain the whole frame." } } },
		};
		return layout;
	}

	/// (batch 37a) Display name of an engine row. The shadow rows are named after the shadow
	/// map they render, with the cascade called near / far (or near / middle / far) from the
	/// cascade count the directional light reported this frame.
	std::string EngineRowLabel(const EnginePhaseRow& a_row, const Util::GpuPhaseTimeline::ShadowInfo& a_shadow)
	{
		using P = Util::GpuPhase;
		const auto cascadeName = [&a_shadow](int a_index) -> std::string {
			const uint32_t n = a_shadow.valid ? a_shadow.sunCascades : 0;
			if (n == 1)
				return "single cascade";
			if (n == 2)
				return a_index == 0 ? "near cascade" : (a_index == 1 ? "far cascade" : "cascade 3 (unused)");
			if (n == 3)
				return a_index == 0 ? "near cascade" : (a_index == 1 ? "middle cascade" : "far cascade");
			return std::format("cascade {}", a_index + 1);
		};
		switch (a_row.phase) {
		case P::ShadowGodRay1:
		case P::ShadowGodRay2:
		case P::ShadowGodRay3:
			return "God-ray shadow, " + cascadeName(static_cast<int>(a_row.phase) - static_cast<int>(P::ShadowGodRay1));
		case P::ShadowSun1:
		case P::ShadowSun2:
		case P::ShadowSun3:
			return "Sun shadow, " + cascadeName(static_cast<int>(a_row.phase) - static_cast<int>(P::ShadowSun1));
		case P::ShadowFocus:
			return a_shadow.valid ? std::format("Character shadows ({} maps)", a_shadow.focusShadows) : std::string(a_row.label);
		case P::ShadowLocalLights:
			return a_shadow.valid || a_shadow.localShadowMaps ? std::format("Lamps & torches ({} lights)", a_shadow.localShadowMaps) : std::string(a_row.label);
		default:
			return a_row.label;
		}
	}

	/// (batch 37a) Stable machine name of an engine row, used as the JSON key.
	const char* EnginePhaseKey(Util::GpuPhase a_phase)
	{
		using P = Util::GpuPhase;
		switch (a_phase) {
		case P::Untracked:
			return "untracked";
		case P::ShadowGodRay1:
			return "shadow_godray_c1";
		case P::ShadowGodRay2:
			return "shadow_godray_c2";
		case P::ShadowGodRay3:
			return "shadow_godray_c3";
		case P::ShadowSun1:
			return "shadow_sun_c1";
		case P::ShadowSun2:
			return "shadow_sun_c2";
		case P::ShadowSun3:
			return "shadow_sun_c3";
		case P::ShadowFocus:
			return "shadow_focus";
		case P::ShadowLocalLights:
			return "shadow_local";
		case P::ShadowMask:
			return "shadow_mask";
		case P::ShadowOther:
			return "shadow_other";
		case P::WaterPrep:
			return "water_prep";
		case P::DepthPrepass:
			return "depth_prepass";
		case P::OpaqueTerrain:
			return "opaque_terrain";
		case P::OpaqueObjects:
			return "opaque_objects";
		case P::OpaqueCharacters:
			return "opaque_characters";
		case P::OpaqueTrees:
			return "opaque_trees";
		case P::OpaqueGrass:
			return "opaque_grass";
		case P::OpaqueDistant:
			return "opaque_distant";
		case P::OpaqueOther:
			return "opaque_other";
		case P::Sky:
			return "sky";
		case P::Water:
			return "water";
		case P::Transparent:
			return "transparent";
		case P::WorldOther:
			return "world_other";
		case P::FirstPerson:
			return "first_person";
		case P::Reflections:
			return "reflections";
		case P::Imagespace:
			return "imagespace";
		case P::UI:
			return "ui";
		case P::CsPasses:
			return "cs_timed_passes";
		case P::CsOther:
			return "cs_other";
		case P::CsUpscaling:
			return "cs_upscaling";
		case P::CsOverlay:
			return "cs_overlay";
		default:
			return "unknown";
		}
	}

	// Below this a row is noise from timer resolution, not a stage that ran.
	constexpr float kEnginePhaseVisibleMs = 0.005f;

	// (batch 36e) Row ids of the engine table's group header rows and its Total row in the
	// view layer. Far above every Util::GpuPhase value.
	constexpr int kEngineGroupRowIdBase = 10000;
	constexpr int kEngineTotalRowId = 20000;

	// (batch 36e) Rows of the view layer's summary table (the compact summary + CPU/GPU split).
	enum SummaryRowId : int
	{
		kSummaryCpuFrame = 0,  // attribution clock frame time (same as the shader table's Total)
		kSummaryPresentWait,
		kSummaryGpuOurs,
		kSummaryGpuUntracked,
		kSummaryGpuGap,
		kSummaryFrame,  // the overlay's own Present-to-Present frame time
		kSummaryPostFgFrame
	};

	/// CPU-bound / GPU-bound verdict from the share of the frame the CPU spends in Present.
	/// Thresholds are deliberately coarse and the middle band is named rather than forced
	/// into one of the two answers.
	std::pair<const char*, ImVec4> BottleneckVerdict(const Menu::ThemeSettings& a_theme, float a_frameMs, float a_waitMs)
	{
		if (a_frameMs <= 0.0f)
			return { "collecting...", a_theme.StatusPalette.Disable };
		const float waitShare = a_waitMs / a_frameMs;
		if (waitShare >= 0.25f)
			return { "GPU-bound (or frame-limited)", a_theme.StatusPalette.Warning };
		if (waitShare <= 0.10f)
			return { "CPU-bound", a_theme.StatusPalette.Error };
		return { "balanced", a_theme.StatusPalette.SuccessColor };
	}
}

/**
 * @brief (batch 36) Where the whole GPU frame goes, stage by stage.
 *
 * Fed by Util::GpuPhaseTimeline: one GPU timestamp per stage switch, every row exclusive,
 * so the rows add up to Total by construction. Nested work (our passes inside the opaque
 * pass, a shadow light inside the shadow pass) is billed to the inner row only, which is
 * what keeps the sum free of double counting.
 */
void PerformanceOverlay::DrawEngineGpuTable()
{
	// (batch 36e) Fed by UpdateViews from Util::GpuPhaseTimeline; drawn from view.engine so
	// the numbers are smoothed and a row, once it has appeared, keeps its place. The layout is
	// a fixed hierarchy, so the sort options do not apply here.
	const auto& table = view.engine;
	if (table.RowCount() == 0) {
		ImGui::TextDisabled("collecting...");
		return;
	}
	const auto cfg = MakeViewConfig();

	float totalMs = 0.0f;
	table.DisplayValue(kEngineTotalRowId, cfg, totalMs);

	ImGui::TextDisabled("(hover for how to read this)");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"Where each frame's GPU time goes, stage by stage. Rows do not overlap, so they add up to Total.\n\n"
				"Read it with the frame rate uncapped: otherwise time the GPU spends waiting for the CPU "
				"is counted in whichever stage was running.\n\n"
				"CPU: how long the game's render thread spent in that stage (preparing and sending its draws). "
				"\"Untracked\" holds everything else the CPU does in a frame, game logic included.");
		}
	}

	const bool peakColumn = settings.ShowPeakColumn;
	if (!ImGui::BeginTable("EngineGpuPhases", peakColumn ? 6 : 5, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg))
		return;

	ImGui::TableSetupColumn("Stage");
	ImGui::TableSetupColumn("GPU Time");
	ImGui::TableSetupColumn("% of GPU frame");
	ImGui::TableSetupColumn("CPU Time");
	ImGui::TableSetupColumn("Draws");
	if (peakColumn)
		ImGui::TableSetupColumn("Peak");
	ImGui::TableHeadersRow();

	const auto tooltip = [](const std::string& a_text) {
		if (!a_text.empty() && ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(a_text.c_str());
		}
	};

	const auto drawRow = [&](int a_id, bool a_showDraws, bool a_indent) {
		const DrawCallRow* row = table.GetRow(a_id);
		if (!row)
			return;
		float ms = 0.0f;
		const bool has = table.DisplayValue(a_id, cfg, ms);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		if (a_indent)
			ImGui::Indent();
		ImGui::TextUnformatted(row->label.c_str());
		tooltip(row->tooltip);
		if (a_indent)
			ImGui::Unindent();

		ImGui::TableNextColumn();
		if (!has)
			ImGui::TextDisabled("-");
		else if (ms > 0.0f && ms < 0.01f)
			ImGui::TextUnformatted("< 0.01 ms");
		else
			ImGui::Text("%.2f ms", ms);

		ImGui::TableNextColumn();
		if (has)
			ImGui::Text("%.1f%%", totalMs > 0.0f ? ms / totalMs * 100.0f : 0.0f);
		else
			ImGui::TextDisabled("-");

		// (batch 37a) Render-thread CPU time of the same stage.
		ImGui::TableNextColumn();
		{
			float cpuMs = 0.0f;
			if (!view.engineCpu.DisplayValue(a_id, cfg, cpuMs))
				ImGui::TextDisabled("-");
			else if (cpuMs > 0.0f && cpuMs < 0.01f)
				ImGui::TextUnformatted("< 0.01 ms");
			else
				ImGui::Text("%.2f ms", cpuMs);
		}

		ImGui::TableNextColumn();
		if (a_showDraws && row->drawCalls >= 0)
			ImGui::Text("%d", row->drawCalls);
		else
			ImGui::TextDisabled("-");

		if (peakColumn) {
			ImGui::TableNextColumn();
			const auto values = table.GetValues(a_id);
			if (values.hasData)
				ImGui::Text("%.2f ms", values.peak);
			else
				ImGui::TextDisabled("-");
		}
	};

	const auto& layout = EnginePhaseLayout();
	for (size_t g = 0; g < layout.size(); ++g) {
		const auto& group = layout[g];
		if (group.label)
			drawRow(kEngineGroupRowIdBase + static_cast<int>(g), group.countsDraws, false);
		for (const auto& row : group.rows) {
			const int id = static_cast<int>(row.phase);
			const DrawCallRow* stored = table.GetRow(id);
			// Our rows: show a count only where engine draws really happen inside our work.
			const bool showDraws = group.countsDraws || (stored && stored->drawCalls > 0);
			drawRow(id, showDraws, group.label != nullptr);
		}
	}
	drawRow(kEngineTotalRowId, true, false);

	ImGui::EndTable();
}

/**
 * @brief Renders Community Shaders' own CPU submit cost, per feature.
 *
 * These rows sum exactly to the "CS features (CPU)" line in the table above, and they are
 * disjoint from the shader-type rows there (State::Debug() removes this time from the
 * intervals it charges). A feature whose cost rounds to nothing gets no row at all rather
 * than a row of zeros.
 */
void PerformanceOverlay::DrawOurCpuPassTable(const std::vector<DrawCallRow>& ourCpuRows)
{
	(void)ourCpuRows;  // (batch 36e) fed into view.csCpu by UpdateViews; drawn from there
	auto& overlay = globals::features::performanceOverlay;
	if (overlay.view.csCpu.RowCount() == 0) {
		ImGui::TextDisabled("No feature has a measurable CPU cost yet.");
		return;
	}

	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	ImGui::TextDisabled("(hover for how to read this)");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"CPU time Community Shaders spends preparing each effect's work and sending it to the GPU. "
				"These rows add up to the \"CS features (CPU)\" line in the shader table, and use the same names as the GPU table.\n\n"
				"\"-\" means the feature had no measurable cost recently. Work started directly by the game "
				"(most post-processing, the upscaling itself) is not included here.");
		}
	}

	bool anyTestData = !overlay.testData.empty();
	auto legends = overlay.BuildDrawCallLegends(theme, anyTestData);
	auto columns = overlay.BuildPassTableColumns(theme, legends, false, "CPU Time (%)",
		"Calls: how many times this feature's CPU work ran in the last frame.");

	const auto cfg = overlay.MakeViewConfig();
	float totalMs = 0.0f;
	overlay.view.shaderFooter.DisplayValue(magic_enum::enum_integer(SpecialShaderType::Total), cfg, totalMs);
	const auto rows = MaterializeRows(overlay.view.csCpu, overlay.view.csCpu.DisplayOrder(cfg), cfg, totalMs, false);

	std::function<void(int, int, const DrawCallRow&)> rowHandler =
		[&columns](int, int colIdx, const DrawCallRow& row) { columns[colIdx].cellRender(row, colIdx); };
	DrawStableTable("OurCpuPassOverlayTable", columns, rows, {}, rowHandler);
}

/**
 * @brief CPU-bound / GPU-bound readout.
 *
 * The verdict still comes from how long the CPU spends blocked in Present, because that is
 * the only signal that is cheap, always available, and unambiguous about the CPU: it is
 * exactly "the CPU had nothing to do but wait". What it is NOT is a GPU-bound test - it
 * cannot tell a GPU wait from a vsync wait, a frame-limiter wait, or (the case that made
 * this misleading in practice) Streamline's proxy Present pacing a frame-generated
 * presentation queue. All of those park the CPU in Present with the GPU largely idle.
 *
 * Batch 14 therefore stops treating the Present wait as the whole story and adds the
 * frame-spanning GPU timestamps of Util::GpuFrameTimer:
 *
 *   - a frame-spanning pair still does NOT measure GPU busy time. GPU timestamps read a
 *     clock that runs while the GPU is idle, so the span itself just reproduces wall-clock
 *     frame time - it is reported as "elapsed" and never as "busy".
 *   - what carries information is where the pair is SPLIT. Both markers bracket the real
 *     Present, so the frame's own rendering lands on one side and the flip, the pacing and
 *     DLSS-G frame generation land on the other.
 *   - the sum of our GPU buckets is a genuine measurement but only of OUR passes, so it
 *     stays a lower bound; the difference against the rendering side of the split is the
 *     engine plus DLSS super resolution, which is the "Untracked GPU" line.
 */
void PerformanceOverlay::DrawBottleneckSummary()
{
	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	// (batch 36e) Every number here comes from view.summary (smoothed, frozen with the rest).
	const auto cfg = MakeViewConfig();
	const auto value = [this, &cfg](int a_id, float& o_value) { return view.summary.DisplayValue(a_id, cfg, o_value); };

	float frameMs = 0.0f, waitMs = 0.0f, gpuMeasuredMs = 0.0f;
	value(kSummaryCpuFrame, frameMs);
	value(kSummaryPresentWait, waitMs);
	value(kSummaryGpuOurs, gpuMeasuredMs);
	const float busyMs = std::max(0.0f, frameMs - waitMs);
	const float waitShare = (frameMs > 0.0f) ? (waitMs / frameMs) : 0.0f;
	const auto verdict = BottleneckVerdict(theme, frameMs, waitMs);

	if (ImGui::BeginTable("BottleneckSummary", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##prop", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 6);
		ImGui::TableSetupColumn("##value");

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Bottleneck:");
		ImGui::TableNextColumn();
		ImGui::TextColored(verdict.second, "%s", verdict.first);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"Based on how much of the frame the CPU spends waiting for it to be shown (Present wait): "
					"over 25% = GPU-bound, under 10% = CPU-bound, in between = balanced.\n\n"
					"V-Sync, a frame-rate cap and frame generation also make the CPU wait, so "
					"\"GPU-bound (or frame-limited)\" really means either. For a clean reading, uncap the "
					"frame rate and turn frame generation off.");
			}
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("CPU busy:");
		ImGui::TableNextColumn();
		ImGui::Text("%.2f ms of %.2f ms (%.0f%%)", busyMs, frameMs,
			(frameMs > 0.0f) ? (busyMs / frameMs * 100.0f) : 0.0f);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Time the CPU spent working this frame (frame time minus Present wait).");
			}
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Present wait:");
		ImGui::TableNextColumn();
		ImGui::Text("%.2f ms (%.0f%%)", waitMs, waitShare * 100.0f);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"Time the CPU spent waiting for the frame to be shown. Waiting on the GPU, V-Sync, a frame-rate cap "
					"or frame generation pacing all look the same here, so a large value does not by itself mean the GPU is busy.\n\n"
					"For the GPU side, see \"Untracked GPU\" and \"Gap\" in the GPU passes table.");
			}
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("GPU (ours):");
		ImGui::TableNextColumn();
		ImGui::Text("%.2f ms", gpuMeasuredMs);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"GPU time of Community Shaders' own effects (the GPU passes rows added up). "
					"Not directly comparable with the CPU numbers above.");
			}
		}

		// The engine's own rendering plus DLSS super resolution, and the Present-side gap.
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("GPU (other):");
		ImGui::TableNextColumn();
		float untrackedMs = 0.0f, gapMs = 0.0f;
		if (value(kSummaryGpuUntracked, untrackedMs) && value(kSummaryGpuGap, gapMs)) {
			ImGui::Text("%.2f ms rendering + %.2f ms gap", untrackedMs, gapMs);
		} else {
			ImGui::TextUnformatted("collecting...");
		}
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"rendering = the game's own rendering plus DLSS/FSR upscaling.\n"
					"gap = time while the frame is being shown: flip, V-Sync or frame-cap waiting, frame generation, and idle.\n\n"
					"\"GPU (ours)\" + rendering + gap = the whole GPU frame (also shown as rows in the GPU passes table).");
			}
		}

		ImGui::EndTable();
	}
}

DrawCallLegends PerformanceOverlay::BuildDrawCallLegends(const Menu::ThemeSettings& theme, bool anyTestData) const
{
	(void)anyTestData;
	DrawCallLegends legends;

	legends.shaderType = {
		"Shader Type",
		{ { "Shader Type: The type of shader being measured.", theme.Palette.Text },
			{ "Click to toggle shader on/off for performance testing.", theme.Palette.Text } }
	};

	legends.drawCalls = {
		"Draw Calls",
		{ { "Draw Calls: Number of draw calls for this shader type in the current frame.", theme.Palette.Text } }
	};

	legends.frameTime = {
		"Frame Time (%)",
		{ { GetTestDataTooltip(), theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Performance Color Legend (ms):", theme.Palette.Text },
			{ "  <= 2 ms", theme.StatusPalette.SuccessColor },
			{ "  > 2 ms and <= 5 ms", theme.StatusPalette.Warning },
			{ "  > 5 ms", theme.StatusPalette.Error } }
	};

	legends.costPerCall = {
		"Cost/Call",
		{ { "Cost/Call: Average time per draw call for this shader type.", theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (ms/call):", theme.Palette.Text },
			{ "  <= 0.05 ms/call", theme.StatusPalette.SuccessColor },
			{ "  > 0.05 ms and <= 0.2 ms/call", theme.StatusPalette.Warning },
			{ "  > 0.2 ms/call", theme.StatusPalette.Error } }
	};

	legends.testFrameTime = {
		"Test Frame Time (%)",
		{ { GetTestDataTooltip(), theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (compared to live data):", theme.Palette.Text },
			{ "  Better (lower than live)", theme.StatusPalette.SuccessColor },
			{ "  Worse (higher than live)", theme.StatusPalette.Error },
			{ "  Same as live", theme.Palette.Text } }
	};

	legends.testCostPerCall = {
		"Test Cost/Call",
		{ { GetTestDataTooltip(), theme.Palette.Text },
			{ "", theme.Palette.Text },
			{ "Color Legend (compared to live data):", theme.Palette.Text },
			{ "  Better (lower than live)", theme.StatusPalette.SuccessColor },
			{ "  Worse (higher than live)", theme.StatusPalette.Error },
			{ "  Same as live", theme.Palette.Text } }
	};

	return legends;
}

std::vector<ColumnConfig> PerformanceOverlay::BuildPassTableColumns(const Menu::ThemeSettings& theme, const DrawCallLegends& legends, bool anyTestData, const char* timeHeader, const char* intervalTooltip)
{
	const std::string intervalTip = intervalTooltip;

	std::vector<ColumnConfig> columns;

	// Pass name. Not clickable: unlike a shader type there is nothing to toggle from here.
	columns.push_back(ColumnConfig{
		"Pass",
		[](const DrawCallRow& row, int) {
			ImGui::TextUnformatted(row.label.c_str());
			if (!row.tooltip.empty() && ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted(row.tooltip.c_str());
				}
			}
		},
		[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.label < b.label) : (a.label > b.label); },
		[]() {
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted("Which effect is timed. Hover a name to see what it covers.");
				}
			}
		} });

	columns.push_back(ColumnConfig{
		"Intervals",
		[intervalTip](const DrawCallRow& row, int) {
			if (row.drawCalls == kDrawCallsNotApplicable)
				ImGui::TextDisabled("-");
			else
				ImGui::Text("%d", row.drawCalls);
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted(intervalTip.c_str());
				}
			}
		},
		[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.drawCalls < b.drawCalls) : (a.drawCalls > b.drawCalls); },
		[intervalTip]() {
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::TextUnformatted(intervalTip.c_str());
				}
			}
		} });

	columns.push_back(ColumnConfig{
		timeHeader,
		// A pass row is only in the table because its work ran, so a rendered "0 ms" would
		// mean "below the timer's resolution", not "free" - and it reads as a broken
		// measurement. Say what is actually known instead.
		MakeMetricColumn(theme, [](const DrawCallRow& row) -> std::optional<float> { return row.hasData ? std::optional<float>(row.frameTime) : std::nullopt; }, [](const auto& theme, float value, const DrawCallRow&) { return Util::GetThresholdColor(value, PerformanceOverlay::Settings::kFrameTimeGoodThreshold, PerformanceOverlay::Settings::kFrameTimeWarningThreshold, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error); }, [](float /*value*/, const DrawCallRow& row) {
				const std::string time = (row.frameTime < 0.01f) ? std::string("< 0.01 ms") : Util::FormatMilliseconds(row.frameTime);
				return time + " (" + Util::FormatPercent(row.percent) + ")"; }, legends.frameTime.tooltip),
		[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.frameTime < b.frameTime) : (a.frameTime > b.frameTime); },
		[legends]() {
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					Util::DrawColoredMultiLineTooltip(legends.frameTime.tooltip);
				}
			}
		} });

	if (anyTestData) {
		columns.push_back(ColumnConfig{
			legends.testFrameTime.header,
			MakeMetricColumn(theme, [](const DrawCallRow& row) { return row.testFrameTime; }, [](const auto& theme, float value, const DrawCallRow& row) {
					 if (value < row.frameTime)
						 return theme.StatusPalette.SuccessColor;
					 if (value > row.frameTime)
						 return theme.StatusPalette.Error;
					 return theme.Palette.Text; }, [](float value, const DrawCallRow&) { return Util::FormatMilliseconds(value); }, legends.testFrameTime.tooltip),
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				 float aVal = a.testFrameTime.value_or(FLT_MAX);
				 float bVal = b.testFrameTime.value_or(FLT_MAX);
				 return asc ? (aVal < bVal) : (aVal > bVal); },
			[legends]() {
				 if (ImGui::IsItemHovered()) {
					 if (auto _tt = Util::HoverTooltipWrapper()) {
						 Util::DrawColoredMultiLineTooltip(legends.testFrameTime.tooltip);
					 }
				 } } });
	}

	// (batch 36e) Highest reading inside the smoothing window, when enabled.
	if (settings.ShowPeakColumn)
		columns.push_back(MakePeakColumn());
	return columns;
}

std::vector<ColumnConfig> PerformanceOverlay::BuildDrawCallTableColumns(const Menu::ThemeSettings& theme, const DrawCallLegends& legends, bool anyTestData)
{
	// Build column configurations
	std::vector<ColumnConfig> columns = {
		{ legends.shaderType.header,
			[theme, this](const DrawCallRow& row, int) {
				if (!row.enabled)
					ImGui::PushStyleColor(ImGuiCol_Text, theme.StatusPalette.Disable);
				bool wasEnabled = row.enabled;
				if (ImGui::Selectable(row.label.c_str(), false)) {
					auto maybeType = magic_enum::enum_cast<RE::BSShader::Type>(row.shaderType);
					if (maybeType.has_value()) {
						auto classIndex = magic_enum::enum_integer(*maybeType) - 1;
						if (classIndex >= 0 && classIndex < magic_enum::enum_integer(RE::BSShader::Type::Total) - 1) {
							HandleShaderToggle(row, wasEnabled);
						}
					}
				}
				if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted(row.tooltip.c_str());
					}
				}
				if (!row.enabled)
					ImGui::PopStyleColor();
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.label < b.label) : (a.label > b.label); },
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.shaderType.tooltip);
					}
				}
			} },
		{ legends.drawCalls.header,
			[](const DrawCallRow& row, int) {
				if (row.drawCalls == kDrawCallsNotApplicable) {
					ImGui::TextDisabled("-");
				} else {
					ImGui::Text("%d", row.drawCalls);
				}
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						if (row.drawCalls == kDrawCallsNotApplicable) {
							ImGui::TextUnformatted("Draw Calls: Not applicable for unmeasured GPU time.");
						} else {
							ImGui::TextUnformatted("Draw Calls: Number of draw calls for this shader type in the current frame.");
						}
					}
				}
			},
			[](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.drawCalls < b.drawCalls) : (a.drawCalls > b.drawCalls); },
			[legends]() {
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						Util::DrawColoredMultiLineTooltip(legends.drawCalls.tooltip);
					}
				}
			} }
	};

	columns.push_back(ColumnConfig{
		legends.frameTime.header,
		MakeMetricColumn(theme, [](const DrawCallRow& row) -> std::optional<float> { return row.hasData ? std::optional<float>(row.frameTime) : std::nullopt; }, [](const auto& theme, float value, const DrawCallRow&) { return Util::GetThresholdColor(value, PerformanceOverlay::Settings::kFrameTimeGoodThreshold, PerformanceOverlay::Settings::kFrameTimeWarningThreshold, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error); }, [](float /*value*/, const DrawCallRow& row) { return Util::FormatMilliseconds(row.frameTime) + " (" + Util::FormatPercent(row.percent) + ")"; }, legends.frameTime.tooltip), [](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.percent < b.percent) : (a.percent > b.percent); }, [legends]() {
			 if (ImGui::IsItemHovered()) {
				 if (auto _tt = Util::HoverTooltipWrapper()) {
					 Util::DrawColoredMultiLineTooltip(legends.frameTime.tooltip);
				 }
			 } } });

	columns.push_back(ColumnConfig{
		legends.costPerCall.header,
		MakeMetricColumn(theme, [](const DrawCallRow& row) -> std::optional<float> { return row.hasData ? std::optional<float>(row.costPerCall) : std::nullopt; }, [](const auto& theme, float value, const DrawCallRow&) { return Util::GetThresholdColor(value, PerformanceOverlay::Settings::kCostPerCallGoodThreshold, PerformanceOverlay::Settings::kCostPerCallWarningThreshold, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error); }, [](float value, const DrawCallRow&) { return (value < PerformanceOverlay::Settings::kMicrosecondThreshold && value > 0.0f) ? Util::FormatMicroseconds(value * 1000.0f) : Util::FormatMilliseconds(value); }, legends.costPerCall.tooltip), [](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.costPerCall < b.costPerCall) : (a.costPerCall > b.costPerCall); }, [legends]() {
			 if (ImGui::IsItemHovered()) {
				 if (auto _tt = Util::HoverTooltipWrapper()) {
					 Util::DrawColoredMultiLineTooltip(legends.costPerCall.tooltip);
				 }
			 } } });

	// Add test columns if present
	if (anyTestData) {
		columns.push_back(ColumnConfig{
			legends.testFrameTime.header,
			MakeMetricColumn(theme, [](const DrawCallRow& row) { return row.testFrameTime; }, [](const auto& theme, float value, const DrawCallRow& row) {
					 if (value < row.frameTime)
						 return theme.StatusPalette.SuccessColor;
					 if (value > row.frameTime)
						 return theme.StatusPalette.Error;
					 return theme.Palette.Text; }, [this](float value, const DrawCallRow& row) { return Util::FormatMilliseconds(value) + " (" + Util::FormatPercent(testData[row.shaderType].percent) + ")"; }, legends.testFrameTime.tooltip), [](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				 float aVal = a.testFrameTime.value_or(FLT_MAX);
				 float bVal = b.testFrameTime.value_or(FLT_MAX);
				 return asc ? (aVal < bVal) : (aVal > bVal); }, [legends]() {
				 if (ImGui::IsItemHovered()) {
					 if (auto _tt = Util::HoverTooltipWrapper()) {
						 Util::DrawColoredMultiLineTooltip(legends.testFrameTime.tooltip);
					 }
				 } } });

		columns.push_back(ColumnConfig{
			legends.testCostPerCall.header,
			MakeMetricColumn(theme, [](const DrawCallRow& row) { return row.testCostPerCall; }, [](const auto& theme, float value, const DrawCallRow& row) {
					 if (value < row.costPerCall)
						 return theme.StatusPalette.SuccessColor;
					 if (value > row.costPerCall)
						 return theme.StatusPalette.Error;
					 return theme.Palette.Text; }, [](float value, const DrawCallRow&) { return (value < PerformanceOverlay::Settings::kMicrosecondThreshold && value > 0.0f) ? Util::FormatMicroseconds(value * 1000.0f) : Util::FormatMilliseconds(value); }, legends.testCostPerCall.tooltip), [](const DrawCallRow& a, const DrawCallRow& b, bool asc) {
				 float aVal = a.testCostPerCall.value_or(FLT_MAX);
				 float bVal = b.testCostPerCall.value_or(FLT_MAX);
				 return asc ? (aVal < bVal) : (aVal > bVal); }, [legends]() {
				 if (ImGui::IsItemHovered()) {
					 if (auto _tt = Util::HoverTooltipWrapper()) {
						 Util::DrawColoredMultiLineTooltip(legends.testCostPerCall.tooltip);
					 }
				 } } });
	}

	// (batch 36e) Highest reading inside the smoothing window, when enabled.
	if (settings.ShowPeakColumn)
		columns.push_back(MakePeakColumn());
	return columns;
}

PerformanceOverlay::DrawCallRowSets PerformanceOverlay::BuildDrawCallRows() const
{
	std::vector<DrawCallRow> mainRows;
	std::vector<DrawCallRow> gpuRows;
	float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();
	float measuredSum = 0.0f;

	globals::state->ForEachShaderTypeWithMetrics([&mainRows, &measuredSum, smoothedFrameTime, this](auto type, int typeIndex, float drawCalls, float frameTime, float percent, float costPerCall) {
		bool enabled = globals::state->enabledClasses[typeIndex - 1];
		std::optional<float> testFrameTime, testCostPerCall;
		auto it = this->testData.find(typeIndex);
		if (it != this->testData.end()) {
			testFrameTime = it->second.frameTime;
			testCostPerCall = it->second.costPerCall;
		}
		std::string label = std::string(magic_enum::enum_name(type)) + ":";
		std::string tooltip = "Draw calls for this shader type.";
		auto tipIt = kShaderTypeTooltips.find(type);
		if (tipIt != kShaderTypeTooltips.end()) {
			tooltip = tipIt->second;
		}
		mainRows.push_back({ label, typeIndex, static_cast<int>(drawCalls), frameTime, percent, costPerCall, tooltip, enabled, testFrameTime, testCostPerCall });
		measuredSum += frameTime;
	});

	// GPU-timed feature buckets (D3D11 timestamp queries around our own passes). These
	// rows are what makes an SVGF vs REBLUR comparison directly readable: each denoiser
	// has its own bucket, so switching the SSRT Denoiser dropdown swaps which row is
	// shown and the milliseconds can be compared 1:1.
	//
	// They go into their own row set, not into the CPU table: GPU timestamps are a
	// different clock, arrive several frames late, and overlap each other and the CPU
	// timeline. They are therefore also deliberately absent from measuredSum - the old
	// code subtracted them from the CPU residual, which could drive "Other" negative.
	Util::GpuPassTimers::GetSingleton()->ForEachActiveBucket(
		[&gpuRows, smoothedFrameTime, this](const Util::GpuPassTimers::BucketReport& report) {
			float percent = Util::CalculatePercentage(report.smoothedMs, smoothedFrameTime);
			std::optional<float> testFrameTime, testCostPerCall;
			auto it = this->testData.find(report.rowId);
			if (it != this->testData.end()) {
				testFrameTime = it->second.frameTime;
				testCostPerCall = it->second.costPerCall;
			}
			gpuRows.push_back({ std::string(report.label) + ":", report.rowId, report.intervalsPerFrame,
				report.smoothedMs, percent, 0.0f, report.tooltip, true, testFrameTime, testCostPerCall });
		});

	// Community Shaders' own CPU submit cost. These milliseconds are NOT part of any
	// shader-type row: State::Debug() subtracts them from the interval it charges, so the
	// per-feature rows here and the shader-type rows above are disjoint and both are part
	// of the same additive breakdown of the frame.
	auto* cpuTimers = Util::CpuPassTimers::GetSingleton();
	std::vector<DrawCallRow> ourCpuRows;
	float ourCpuSum = 0.0f;
	cpuTimers->ForEachActiveBucket([&ourCpuRows, &ourCpuSum, smoothedFrameTime](const Util::CpuPassTimers::Report& report) {
		ourCpuRows.push_back({ std::string(report.label) + ":", report.rowId, report.callsPerFrame,
			report.smoothedMs, Util::CalculatePercentage(report.smoothedMs, smoothedFrameTime),
			0.0f, report.tooltip, true, std::nullopt, std::nullopt });
		ourCpuSum += report.smoothedMs;
	});
	// GetFeatureTotalMs() includes buckets too small to earn a row, so the aggregate line
	// is the honest total rather than the sum of what happens to be displayed.
	const float ourCpuTotal = std::max(ourCpuSum, cpuTimers->GetFeatureTotalMs());
	const float presentWaitMs = cpuTimers->GetPresentWaitMs();

	auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);
	if (std::abs(otherFrameTime) < 1e-4f)
		otherFrameTime = 0.0f;
	std::optional<float> otherTestFrameTime, otherTestCostPerCall, totalTestFrameTime, totalTestCostPerCall;
	auto itOther = this->testData.find(magic_enum::enum_integer(SpecialShaderType::Other));
	if (itOther != this->testData.end()) {
		otherTestFrameTime = itOther->second.frameTime;
		otherTestCostPerCall = itOther->second.costPerCall;
	}
	auto itTotal = this->testData.find(magic_enum::enum_integer(SpecialShaderType::Total));
	if (itTotal != this->testData.end()) {
		totalTestFrameTime = itTotal->second.frameTime;
		totalTestCostPerCall = itTotal->second.costPerCall;
	}
	DrawCallRow ourCpuRow = {
		"CS features (CPU):", magic_enum::enum_integer(SpecialShaderType::OurCpu), kDrawCallsNotApplicable,
		ourCpuTotal, Util::CalculatePercentage(ourCpuTotal, smoothedFrameTime), 0.0f,
		std::string("CPU time Community Shaders itself spends preparing work for the GPU. "
					"Broken down per feature in the \"Community Shaders (CPU submit)\" table below; "
					"it is not counted again in the shader rows above, so the table still adds up."),
		true, std::nullopt, std::nullopt
	};

	DrawCallRow presentWaitRow = {
		// Deliberately no longer called "Present / GPU wait": the label itself was the source
		// of the "the GPU was idle 70% of the frame" misreading. It is CPU blocked time.
		"Present wait (CPU blocked):", magic_enum::enum_integer(SpecialShaderType::PresentWait), kDrawCallsNotApplicable,
		presentWaitMs, Util::CalculatePercentage(presentWaitMs, smoothedFrameTime), 0.0f,
		std::string("Time the CPU spent waiting for the frame to be shown, i.e. it had nothing left to do. "
					"This is not GPU load: waiting on the GPU, V-Sync, a frame-rate cap or DLSS-G pacing all land here, "
					"and with DLSS-G on most of it is usually pacing.\n\n"
					"For where GPU time goes, see the GPU Passes table. To tell a real GPU wait from pacing, "
					"uncap the frame rate and turn frame generation off."),
		true, std::nullopt, std::nullopt
	};

	DrawCallRow otherRow = {
		"Engine (untracked):", magic_enum::enum_integer(SpecialShaderType::Other), kDrawCallsNotApplicable, otherFrameTime, otherPercent,
		0.0f,
		std::string("Frame time not covered by the rows above: the game engine's own work (scripts, physics, "
					"animation, AI, audio, vanilla UI), driver overhead and other SKSE plugins. Not a Community "
					"Shaders cost, and it cannot be broken down further.\n\n"
					"A large value is normal in Skyrim and usually means the game is limited by engine work, not rendering."),
		true, otherTestFrameTime, otherTestCostPerCall
	};
	// Always use the actual total frame time for live data
	float totalFrameTime = smoothedFrameTime;
	float totalPercent = 100.0f;  // Total is always 100% of total

	DrawCallRow totalRow = {
		"Total:", magic_enum::enum_integer(SpecialShaderType::Total), static_cast<int>(globals::state->GetTotalSmoothedDrawCalls()), totalFrameTime, totalPercent,
		totalCostPerCall,
		std::string("Frame time, smoothed the same way as the rows above so they add up to it. "
					"It can differ by a millisecond or two from the FPS readout at the top, which is refreshed "
					"every Update Interval; that is normal."),
		true, totalTestFrameTime, totalTestCostPerCall
	};
	std::vector<DrawCallRow> summaryRows;
	summaryRows.push_back(ourCpuRow);
	summaryRows.push_back(presentWaitRow);
	summaryRows.push_back(otherRow);
	summaryRows.push_back(totalRow);
	return { std::move(mainRows), std::move(summaryRows), std::move(gpuRows), std::move(ourCpuRows) };
}

/**
  * @brief Creates a table row handler for the draw calls table
  *
  * This function creates a lambda that handles rendering individual table rows,
  * including special handling for summary rows (Total, Other) and normal shader
  * type rows. It ensures proper tooltip display and click handling for each row type.
  *
  * @param columns The column configurations for the table
  * @param overlay Pointer to the performance overlay instance
  * @return Function that handles row rendering and interaction
  */
std::function<void(int, int, const DrawCallRow&)> PerformanceOverlay::CreateTableRowHandler(const std::vector<ColumnConfig>& columns)
{
	return [&columns, this](int rowIdx, int colIdx, const DrawCallRow& row) {
		(void)rowIdx;
		// Summary rows are identified by their negative pseudo shader type, not by their
		// label text: there are four of them now and matching on strings meant renaming a
		// row silently turned it back into a clickable shader row.
		const bool isSummary = row.shaderType < 0;
		const bool isTotal = row.shaderType == magic_enum::enum_integer(SpecialShaderType::Total);

		if (isSummary && colIdx == 0) {
			if (isTotal) {
				if (ImGui::Selectable(row.label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
					HandleTotalRowToggle();
				}
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted(row.tooltip.c_str());
						float _fps = row.frameTime > 0.0f ? 1000.0f / row.frameTime : 0.0f;
						ImGui::Text("FPS: %.2f", _fps);
					}
				}
			} else {
				ImGui::TextUnformatted(row.label.c_str());
				if (!row.tooltip.empty() && ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::TextUnformatted(row.tooltip.c_str());
					}
				}
			}
		} else {
			// Normal row (and summary rows in non-label columns): tooltips never modify
			// cell content.
			columns[colIdx].cellRender(row, colIdx);
		}
	};
}
// ============================================================================
// EVENT HANDLING FUNCTIONS
// ============================================================================

/**
  * @brief Handles shader type toggle functionality
  *
  * This function processes user clicks on shader type rows in the performance table.
  * When a shader is disabled, it captures the current performance data as test data
  * for comparison. This allows users to see the performance impact of disabling
  * specific shader types.
  *
  * @param row The draw call row that was clicked
  * @param wasEnabled Whether the shader was enabled before the click
  */
void PerformanceOverlay::HandleShaderToggle(const DrawCallRow& row, bool wasEnabled)
{
	auto maybeType = magic_enum::enum_cast<RE::BSShader::Type>(row.shaderType);
	if (!maybeType.has_value()) {
		return;
	}

	auto classIndex = magic_enum::enum_integer(*maybeType) - 1;
	if (classIndex < 0 || classIndex >= magic_enum::enum_integer(RE::BSShader::Type::Total) - 1) {
		return;
	}

	bool isDisabling = wasEnabled;
	float prevFrameTime = row.frameTime;
	float prevCostPerCall = row.costPerCall;

	// Capture live data for Total and Other before toggling
	float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();
	float measuredSum = 0.0f;
	globals::state->ForEachShaderTypeWithMetrics([&measuredSum]([[maybe_unused]] auto type, [[maybe_unused]] int typeIndex, [[maybe_unused]] float drawCalls, float frameTime, [[maybe_unused]] float percent, [[maybe_unused]] float costPerCall) {
		measuredSum += frameTime;
	});
	auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);

	// Toggle the shader
	globals::state->enabledClasses[classIndex] = !wasEnabled;

	if (isDisabling) {
		// Save the last live value before disabling
		this->UpdateShaderTestData(row.shaderType, prevFrameTime, prevCostPerCall);
		// Save Total and Other test data as well
		this->testData[magic_enum::enum_integer(SpecialShaderType::Total)] = { smoothedFrameTime, totalCostPerCall, 100.0f };
		this->testData[magic_enum::enum_integer(SpecialShaderType::Other)] = { otherFrameTime, 0.0f, otherPercent };
		this->testDataSource = TestDataSource::ManualShaderToggle;
		QueryPerformanceCounter(&this->testDataLastUpdated);
	}
}

/**
  * @brief Handles the Total row toggle functionality
  *
  * This function processes clicks on the Total row in the performance table.
  * It toggles all shader types on/off simultaneously, allowing users to quickly
  * enable or disable all shaders for performance testing.
  */
void PerformanceOverlay::HandleTotalRowToggle()
{
	bool anyDisabled = false;
	globals::state->ForEachShaderTypeWithIndex([&anyDisabled]([[maybe_unused]] auto type, int classIndex) {
		if (!globals::state->enabledClasses[classIndex]) {
			anyDisabled = true;
		}
	});
	globals::state->ForEachShaderTypeWithIndex([&anyDisabled]([[maybe_unused]] auto type, int classIndex) {
		globals::state->enabledClasses[classIndex] = anyDisabled;
	});

	// Update test data and timestamp for manual toggling (not just A/B test mode)
	auto* abTestingManager = ABTestingManager::GetSingleton();
	bool abTest = abTestingManager && abTestingManager->IsEnabled() && abTestingManager->IsUsingTestConfig();
	if (abTest) {
		this->UpdateAllShaderTestData();
	} else {
		// Manual toggle: update test data and timestamp
		float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();
		float measuredSum = 0.0f;
		globals::state->ForEachShaderTypeWithMetrics([&measuredSum]([[maybe_unused]] auto type, [[maybe_unused]] int typeIndex, [[maybe_unused]] float drawCalls, float frameTime, [[maybe_unused]] float percent, [[maybe_unused]] float costPerCall) {
			measuredSum += frameTime;
		});
		auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);
		this->testData[magic_enum::enum_integer(SpecialShaderType::Total)] = { smoothedFrameTime, totalCostPerCall, 100.0f };
		this->testData[magic_enum::enum_integer(SpecialShaderType::Other)] = { otherFrameTime, 0.0f, otherPercent };
		this->testDataSource = TestDataSource::ManualShaderToggle;
		QueryPerformanceCounter(&this->testDataLastUpdated);
	}
}
// ============================================================================
// TEST DATA MANAGEMENT FUNCTIONS
// ============================================================================

// Static test data state

// Implement static member functions
/**
  * @brief Updates test data for a specific shader type during manual shader toggling
  *
  * This function captures performance data for a shader type when it's manually disabled,
  * allowing users to compare performance with/without specific shaders enabled.
  *
  * @param shaderType The shader type index to update test data for
  * @param frameTime The frame time contribution of this shader type (ms)
  * @param costPerCall The cost per draw call for this shader type (ms/call)
  *
  * @note This function also updates the Total and Other summary rows to maintain
  *       consistency with the current performance state
  */
void PerformanceOverlay::UpdateShaderTestData(int shaderType, float frameTime, float costPerCall)
{
	UpdateShaderTestDataEntry(shaderType, frameTime, costPerCall);

	float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();
	float measuredSum = 0.0f;
	for (const auto& [type, data] : testData) {
		if (type >= 0)
			measuredSum += data.frameTime;
	}

	auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);
	UpdateSummaryTestData(smoothedFrameTime, otherFrameTime, otherPercent, totalCostPerCall);

	testDataSource = TestDataSource::ManualShaderToggle;
	QueryPerformanceCounter(&testDataLastUpdated);
}

/**
  * @brief Updates test data for all shader types during A/B test Variant B execution
  *
  * This function captures comprehensive performance data for all shader types when
  * running in A/B test mode with Variant B (test config) active. It ensures that
  * all shader types, including Total and Other summary rows, have current test data
  * for accurate performance comparison.
  *
  * @note This function only captures data when A/B testing is enabled and using
  *       Variant B (test config). It does nothing in manual shader toggle mode.
  */
void PerformanceOverlay::UpdateAllShaderTestData()
{
	// Check if all shaders are disabled
	bool allDisabled = true;
	globals::state->ForEachShaderTypeWithIndex([&allDisabled]([[maybe_unused]] auto type, int classIndex) {
		if (globals::state->enabledClasses[classIndex]) {
			allDisabled = false;
		}
	});
	if (allDisabled) {
		testData.clear();
		testDataSource = TestDataSource::None;
		return;
	}

	// Only capture test data if we're in A/B test mode AND using Variant B (test config)
	auto* abTestingManager = ABTestingManager::GetSingleton();
	bool abTest = abTestingManager && abTestingManager->IsEnabled() && abTestingManager->IsUsingTestConfig();
	if (!abTest) {
		// If not in A/B test Variant B, don't capture test data
		return;
	}

	float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();
	float measuredSum = 0.0f;

	globals::state->ForEachShaderTypeWithMetrics([&measuredSum, smoothedFrameTime, this]([[maybe_unused]] auto type, int typeIndex, [[maybe_unused]] float drawCalls, float frameTime, float percent, float costPerCall) {
		this->UpdateShaderTestDataEntry(typeIndex, frameTime, costPerCall, percent);
		measuredSum += frameTime;
	});

	auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);
	UpdateSummaryTestData(smoothedFrameTime, otherFrameTime, otherPercent, totalCostPerCall);
	testDataSource = TestDataSource::ABTest_VariantB;
	QueryPerformanceCounter(&testDataLastUpdated);
}

std::string PerformanceOverlay::GetTestDataTooltip() const
{
	switch (testDataSource) {
	case TestDataSource::ABTest_VariantB:
		return std::string("Test data from Test (Variant B).\nLast updated: ") + Util::TimeAgoStringQPC(testDataLastUpdated, state.overlayTimingFrequency) + " ago.";
	case TestDataSource::ManualShaderToggle:
		return std::string("Test data from manual shader toggle.\nLast updated: ") + Util::TimeAgoStringQPC(testDataLastUpdated, state.overlayTimingFrequency) + " ago.";
	default:
		return "No test data available.";
	}
}

// --- TEST DATA CAPTURE LOGIC ---
// Test data is captured in two scenarios:
// 1. A/B Test Mode (Variant B): If abTestingEnabled && usingTestConfig, we continuously capture test data
//    for all shader types, "Other", and "Total" every frame. This allows live comparison between
//    Variant A (user config) and Variant B (test config).
// 2. Manual Shader Toggle: If any shader is disabled, we capture test data for the disabled shaders
//    (and summary rows) at the moment of disabling, and keep it until cleared. This allows users to
//    compare performance with/without specific shaders enabled.
// Test data is only cleared by the "Clear Test Data" button or if all shaders are disabled (rare edge case).
void PerformanceOverlay::CaptureTestData()
{
	auto* abTestingManager = ABTestingManager::GetSingleton();
	bool abTestActive = (abTestingManager && abTestingManager->IsEnabled() && abTestingManager->IsUsingTestConfig());
	bool anyShaderDisabled = false;
	globals::state->ForEachShaderTypeWithIndex([&anyShaderDisabled]([[maybe_unused]] auto type, int classIndex) {
		if (!globals::state->enabledClasses[classIndex]) {
			anyShaderDisabled = true;
		}
	});
	float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();
	float measuredSum = 0.0f;
	if (abTestActive) {
		measuredSum = 0.0f;
		globals::state->ForEachShaderTypeWithMetrics([&measuredSum, smoothedFrameTime, this]([[maybe_unused]] auto type, int typeIndex, [[maybe_unused]] float drawCalls, float frameTime, float percent, float costPerCall) {
			this->UpdateShaderTestDataEntry(typeIndex, frameTime, costPerCall, percent);
			measuredSum += frameTime;
		});
		auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);
		UpdateSummaryTestData(smoothedFrameTime, otherFrameTime, otherPercent, totalCostPerCall);
		testDataSource = TestDataSource::ABTest_VariantB;
		QueryPerformanceCounter(&testDataLastUpdated);
	} else if (anyShaderDisabled) {
		measuredSum = 0.0f;
		globals::state->ForEachShaderTypeWithMetrics([&measuredSum, smoothedFrameTime, this]([[maybe_unused]] auto type, int typeIndex, [[maybe_unused]] float drawCalls, float frameTime, float percent, float costPerCall) {
			bool enabled = globals::state->enabledClasses[typeIndex - 1];
			if (!enabled) {
				this->UpdateShaderTestDataEntry(typeIndex, frameTime, costPerCall, percent);
			}
			measuredSum += frameTime;
		});
		auto [otherFrameTime, otherPercent, totalCostPerCall] = CalculateSummaryData(smoothedFrameTime, measuredSum);
		UpdateSummaryTestData(smoothedFrameTime, otherFrameTime, otherPercent, totalCostPerCall);
		testDataSource = TestDataSource::ManualShaderToggle;
		QueryPerformanceCounter(&testDataLastUpdated);
	}
}

void PerformanceOverlay::ClearTestData()
{
	testData.clear();
	testDataSource = TestDataSource::None;
}

// Static helper method implementations
void PerformanceOverlay::UpdateShaderTestDataEntry(int shaderType, float frameTime, float costPerCall, float percent)
{
	testData[shaderType] = { frameTime, costPerCall, percent };
}

void PerformanceOverlay::UpdateSummaryTestData(float smoothedFrameTime, float otherFrameTime, float otherPercent, float totalCostPerCall)
{
	testData[magic_enum::enum_integer(SpecialShaderType::Other)] = { otherFrameTime, 0.0f, otherPercent };
	testData[magic_enum::enum_integer(SpecialShaderType::Total)] = { smoothedFrameTime, totalCostPerCall, 100.0f };
}
// ============================================================================
// PERFORMANCE OVERLAY STATE MANAGEMENT
// ============================================================================

void PerformanceOverlay::AdvanceFrameClock()
{
	// Runs on every Present, including frames where the overlay is not drawn. Without
	// this the frame clock froze while hidden and the first delta after unhiding covered
	// the whole hidden period, which then sat inside the Avg / 1% Low window.
	if (!loaded)
		return;
	// (batch 37a) Every Present, so the Freeze key is usable even while the overlay is hidden.
	ResolveFreezeKeyConflict();
	UpdateGraphValues();
}

PerformanceOverlay::FrameStats PerformanceOverlay::ComputeFrameStats() const
{
	FrameStats stats;

	const size_t available = state.statsWindow.GetValidCount();
	if (available == 0)
		return stats;

	// Walk backwards from the newest sample, accumulating frame times until the window
	// duration is covered. The sum of frame times *is* the elapsed wall time, so the
	// window stays exactly kStatsWindowSeconds long at any frame rate.
	const float windowMs = Settings::kStatsWindowSeconds * 1000.0f;
	std::vector<float> samples;
	samples.reserve(std::min<size_t>(available, Settings::kStatsWindowMaxFrames));

	float coveredMs = 0.0f;
	for (size_t age = 0; age < available && coveredMs < windowMs; ++age) {
		const float sampleMs = state.statsWindow.GetNewest(age);
		if (sampleMs <= 0.0f)
			break;  // never-written slot: nothing older is valid either
		samples.push_back(sampleMs);
		coveredMs += sampleMs;
	}

	if (samples.empty() || coveredMs < Settings::kStatsWindowMinSeconds * 1000.0f)
		return stats;

	stats.valid = true;
	stats.frames = static_cast<int>(samples.size());
	stats.seconds = coveredMs / 1000.0f;
	stats.averageMs = coveredMs / static_cast<float>(samples.size());

	// "1% Low" == 99th-percentile frame time: the (0.99 * N)-th slowest frame.
	size_t p99Index = (samples.size() * 99) / 100;
	if (p99Index >= samples.size())
		p99Index = samples.size() - 1;
	std::nth_element(samples.begin(), samples.begin() + p99Index, samples.end());
	stats.percentile99Ms = samples[p99Index];

	return stats;
}

void PerformanceOverlay::UpdateGraphValues()
{
	// Check if Frame Generation is active
	state.isFrameGenerationActive = globals::features::upscaling.IsFrameGenerationActive();

	// Sync frame history buffer size with user settings
	settings.FrameHistorySize = std::clamp(
		settings.FrameHistorySize,
		settings.kMinFrameHistorySize,
		settings.kMaxFrameHistorySize);
	state.frameTimeHistory.Resize(settings.FrameHistorySize);
	state.postFGFrameTimeHistory.Resize(settings.FrameHistorySize);

	// Calculate counter deltas. The counter is always primed by DataLoaded(); the guard
	// covers the case where this runs before it (frequency 0 would divide by zero) and
	// discards that one sample instead of reporting an absurd frame time.
	if (state.frequency == 0) {
		REX::W32::QueryPerformanceFrequency(&state.frequency);
		REX::W32::QueryPerformanceCounter(&state.lastFrameCounter);
		state.frameClockPrimed = state.frequency != 0;
		return;
	}
	REX::W32::QueryPerformanceCounter(&state.currentFrameCounter);
	int64_t elapsedCounter = state.currentFrameCounter - state.lastFrameCounter;
	state.lastFrameCounter = state.currentFrameCounter;
	if (!state.frameClockPrimed) {
		state.frameClockPrimed = true;
		return;
	}

	// Calculate frametime and fps
	state.frameTimeMs = Util::CalcFrameTime(elapsedCounter, state.frequency);
	state.fps = Util::CalcFPS(state.frameTimeMs);

	// Calculate smooth values for display using the user-defined update interval
	// Initialize overlay timing frequency if needed
	if (state.overlayTimingFrequency.QuadPart == 0) {
		QueryPerformanceFrequency(&state.overlayTimingFrequency);
		QueryPerformanceCounter(&state.lastUpdateTime);
	}

	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	float deltaTime = (now.QuadPart - state.lastUpdateTime.QuadPart) /
	                  static_cast<float>(state.overlayTimingFrequency.QuadPart);
	state.lastUpdateTime = now;

	// Insert latest frame time into circular buffer. oldFrameTime is the sample about to
	// be overwritten; it lets the min/max tracking below skip a full rescan unless the
	// value that just left the window was the current extreme.
	float oldFrameTime = state.frameTimeHistory.GetData()[state.frameTimeHistory.GetHeadIdx()];
	state.frameTimeHistory.Push(state.frameTimeMs);

	// Feed the rolling window behind the Avg / 1% Low readouts. Ridiculous samples
	// (loading screens, alt-tab, a debugger break) are kept out so a single multi-second
	// frame cannot dominate the window for the next ten seconds.
	state.statsWindow.Resize(Settings::kStatsWindowMaxFrames);
	if (state.frameTimeMs > 0.0f && state.frameTimeMs <= Settings::kStatsMaxSampleMs)
		state.statsWindow.Push(state.frameTimeMs);

	// Maintain instantaneous min/max tracking
	if (state.frameTimeMs > state.maxFrameTime) {
		state.maxFrameTime = state.frameTimeMs;
	} else if (state.frameTimeMs < state.minFrameTime) {
		state.minFrameTime = state.frameTimeMs;
	} else if (oldFrameTime == state.minFrameTime) {
		state.minFrameTime = *std::ranges::min_element(state.frameTimeHistory.GetData());
	} else if (oldFrameTime == state.maxFrameTime) {
		state.maxFrameTime = *std::ranges::max_element(state.frameTimeHistory.GetData());
	}

	float avgFrameTime = kDefaultFrameTimeMs,
		  stdDev = 0.0f,
		  graphMin = 0.0f,
		  graphMax = Settings::kGraphSpreadMultiplier * kDefaultFrameTimeMs;
	// Calculate mean and standard deviation for normalized graph range
	if (!state.frameTimeHistory.GetData().empty()) {
		// Calculate average frame time
		avgFrameTime = std::accumulate(state.frameTimeHistory.GetData().begin(), state.frameTimeHistory.GetData().end(), 0.0f) / state.frameTimeHistory.GetData().size();

		// Calculate standard deviation
		float variance = 0.0f;
		for (float ft : state.frameTimeHistory.GetData()) {
			float diff = ft - avgFrameTime;
			variance += diff * diff;
		}
		variance /= state.frameTimeHistory.GetData().size();
		stdDev = std::sqrt(variance);

		// Calculate graph range
		float spread = std::clamp(stdDev * Settings::kGraphSpreadMultiplier, Settings::kGraphMinSpread, Settings::kGraphMaxSpread);
		graphMin = std::max(0.0f, avgFrameTime - spread);
		graphMax = avgFrameTime + spread;
	}

	// Exponential smoothing for stable graph scaling
	state.smoothedMinFrameTime = state.smoothedMinFrameTime + Settings::kSmoothingFactor * (graphMin - state.smoothedMinFrameTime);
	state.smoothedMaxFrameTime = state.smoothedMaxFrameTime + Settings::kSmoothingFactor * (graphMax - state.smoothedMaxFrameTime);

	if (state.isFrameGenerationActive) {
		auto& upscaling = globals::features::upscaling;
		state.frameGenerationIsDLSSG = upscaling.IsDLSSGBackend();
		state.appliedFGMultiplier = upscaling.GetFrameGenerationAppliedMultiplier();
		const uint32_t rejectedFrames = state.frameGenerationIsDLSSG ? upscaling.streamline.GetDLSSGRejectedFramesToGenerate() : 0u;
		state.rejectedFGMultiplier = rejectedFrames > 0 ? rejectedFrames + 1u : 0u;

		// Presented frames per rendered frame, as reported by the backend. DLSS-G reports
		// this; FSR 3 frame generation does not and returns 0.
		//
		// The previous code asked for a measured value and then required frame generation
		// to be INACTIVE to use it - inside a branch that already required it to be
		// ACTIVE. The measured path was therefore unreachable and the fixed 2x estimate
		// was always used, silently, with no indication in the UI.
		const float measuredMultiplier = globals::features::upscaling.GetFrameGenerationPresentMultiplier();
		if (measuredMultiplier > 1.01f) {
			state.postFGIsMeasured = true;
			state.postFGMultiplier = measuredMultiplier;
			state.postFGFrameTimeMs = state.frameTimeMs / measuredMultiplier;
			state.postFGFps = state.fps * measuredMultiplier;
		} else {
			// No cadence reported: fall back to an estimate. The UI labels every number
			// derived from this as an estimate. DLSS-G is estimated from the multiplier it
			// accepted (it reports none for the first frames after a switch); FSR is fixed 2x.
			const float estimatedMultiplier = state.frameGenerationIsDLSSG ?
			                                      static_cast<float>(state.appliedFGMultiplier) :
			                                      Settings::kFrameGenerationMultiplier;
			state.postFGIsMeasured = false;
			state.postFGMultiplier = estimatedMultiplier;
			state.postFGFrameTimeMs = state.frameTimeMs / estimatedMultiplier;
			state.postFGFps = state.fps * estimatedMultiplier;
		}

		// Update post-FG smooth values when timer elapses
		if (state.updateTimer <= 0.0f) {
			state.postFGSmoothFps = state.postFGFps;
			state.postFGSmoothFrameTimeMs = state.postFGFrameTimeMs;
		}

		// Update post-FG frametime history
		state.postFGFrameTimeHistory.Push(state.postFGFrameTimeMs);
	}

	// Update smooth values with user-specified interval
	state.updateTimer += deltaTime;
	if (state.updateTimer >= settings.UpdateInterval) {
		state.smoothFps = state.fps;  // Sampling white noise won't give you smoothed noise. This is useless.
		state.smoothFrameTimeMs = state.frameTimeMs;
		state.updateTimer = 0.0f;
	}
}

// ============================================================================
// (batch 36e) READABILITY LAYER
// ============================================================================

namespace
{
	/// Stable row id from a string, for rows that have no natural integer id (denoiser passes).
	int StableId(std::string_view a_key)
	{
		return static_cast<int>(std::hash<std::string_view>{}(a_key) & 0x3FFFFFFF);
	}

	// (batch 37a) How long a panel message ("Saved ...") stays up.
	constexpr double kMessageSeconds = 8.0;

	double NowSeconds()
	{
		LARGE_INTEGER freq, counter;
		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&counter);
		return static_cast<double>(counter.QuadPart) / static_cast<double>(freq.QuadPart);
	}

	const char* DenoiserPassTooltip(const std::string& a_group, const std::string& a_pass)
	{
		if (a_group == "Guides") {
			if (a_pass.starts_with("ViewZ"))
				return "Our pass that prepares REBLUR's depth and normal/roughness inputs (once per frame, shared by both REBLUR instances).";
			return "Copy of the game's motion vectors for REBLUR (a plain GPU copy, no shader).";
		}
		if (a_group == "SSRT") {
			if (a_pass == "Sparse Resolve")
				return "Our pass that fills the pixels the sparse (checkerboard) diffuse trace skipped, before denoising.";
			return "Our pass that turns REBLUR's output back into the colour SSRT composites. Packing REBLUR's input "
			       "has no pass of its own: it is done inside the ray march, so its cost is in SSRT Trace.";
		}
		if (a_group == "Confidence")
			return "A stage of the low-resolution confidence filter (how much SSRT light replaces the game's ambient).";
		return "One of REBLUR's own passes, named by NRD. Groups = thread groups NRD dispatched; Covers = groups x group "
		       "size, i.e. the pixel area the pass works on. Compare Covers with the render resolution above.";
	}

	std::string FormatMsForLog(bool a_has, float a_ms)
	{
		return a_has ? std::format("{:.3f} ms", a_ms) : std::string("-");
	}

	constexpr const char* kSortModeNames[] = { "By cost (smoothed)", "Fixed order", "By cost (live, jumps)" };
	constexpr const char* kSmoothModeNames[] = { "Off", "Window average", "EMA" };
	constexpr float kWindowChoices[] = { 0.25f, 0.5f, 1.0f, 2.0f };
	constexpr const char* kWindowNames[] = { "0.25 s", "0.5 s", "1 s", "2 s" };
	constexpr int kLogChoices[] = { 0, 5, 10, 30, 60 };
	constexpr const char* kLogNames[] = { "Off", "Every 5 s", "Every 10 s", "Every 30 s", "Every 60 s" };
}

PerfView::ViewConfig PerformanceOverlay::MakeViewConfig() const
{
	PerfView::ViewConfig cfg;
	cfg.sort = static_cast<PerfView::SortMode>(std::clamp(settings.SortMode, 0, 2));
	cfg.smooth = static_cast<PerfView::SmoothMode>(std::clamp(settings.SmoothingMode, 0, 2));
	cfg.windowSeconds = std::clamp(settings.SmoothingWindow, 0.1f, 5.0f);
	cfg.showLive = settings.ShowLiveValues;
	cfg.topN = std::max(0, settings.TopN);
	return cfg;
}

bool PerformanceOverlay::Section(const char* a_label, bool& a_open, const char* a_tooltip)
{
	// The setting is the source of truth: forced every frame, and whatever the click made of
	// it is written back, so the state is saved with the rest of the settings.
	ImGui::SetNextItemOpen(a_open, ImGuiCond_Always);
	a_open = ImGui::CollapsingHeader(a_label);
	if (a_tooltip && ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(a_tooltip);
	}
	return a_open;
}

void PerformanceOverlay::ToggleFreeze()
{
	if (!view.frozen) {
		view.frozen = true;
		view.frozenAt = NowSeconds();
		view.frozenState = state;
		view.frozenStats = ComputeFrameStats();
		view.pendingSnapshot = true;  // written on the next overlay frame, once tables are drawn
		view.pendingJsonSave = true;  // (batch 37a) and the same frame as a JSON file
	} else {
		view.frozen = false;
		view.frozenState.reset();
	}
}

void PerformanceOverlay::UpdateViews(double a_now, const DrawCallRowSets& a_rowSets, const PerfView::ViewConfig& a_cfg)
{
	using DcIn = PerfView::StableTable<DrawCallRow>::Input;

	// --- CPU shader-type table and its pinned summary rows ---
	{
		std::vector<DcIn> in;
		for (const auto& r : a_rowSets.cpuRows)
			in.push_back(DcIn{ r.shaderType, r, r.frameTime, r.shaderType });
		view.shaderTypes.Update(a_now, std::move(in), a_cfg);

		std::vector<DcIn> footer;
		int order = 0;
		for (const auto& r : a_rowSets.summaryRows)
			footer.push_back(DcIn{ r.shaderType, r, r.frameTime, order++ });
		view.shaderFooter.Update(a_now, std::move(footer), a_cfg);
	}

	// --- Community Shaders CPU submit ---
	{
		std::vector<DcIn> in;
		for (const auto& r : a_rowSets.ourCpuRows)
			in.push_back(DcIn{ r.shaderType, r, r.frameTime, 0 });
		view.csCpu.Update(a_now, std::move(in), a_cfg);
	}

	// --- GPU passes, and the four whole-frame rows below them ---
	float bucketSum = 0.0f;
	const auto frameGpu = Util::GpuFrameTimer::GetSingleton()->Get();
	{
		std::vector<DcIn> in;
		int intervalSum = 0;
		for (const auto& r : a_rowSets.gpuRows) {
			in.push_back(DcIn{ r.shaderType, r, r.frameTime, r.shaderType });
			bucketSum += r.frameTime;
			if (r.drawCalls != kDrawCallsNotApplicable)
				intervalSum += r.drawCalls;
		}
		view.gpuPasses.Update(a_now, std::move(in), a_cfg);

		std::vector<DcIn> footer;
		if (!a_rowSets.gpuRows.empty()) {
			footer.push_back(DcIn{ kGpuTotalRowId,
				DrawCallRow{ "Measured GPU:", kGpuTotalRowId, intervalSum, bucketSum, 0.0f, 0.0f,
					std::string("Sum of the rows above: only Community Shaders' own effects, so the GPU was busy at least this long. "
								"The game's own rendering is in \"Untracked GPU\" below, and frame generation in \"Gap\"."),
					true, std::nullopt, std::nullopt },
				bucketSum, 0 });
		}
		// Whole-frame GPU timeline (batch 14): untracked + gap + measured == frame (elapsed).
		if (frameGpu.hasSample) {
			// Clamped at zero: the bucket sum and the frame span come from different frames,
			// so right after a settings change the difference can dip below zero for a moment.
			const float untrackedMs = std::max(0.0f, frameGpu.workSpanMs - bucketSum);
			footer.push_back(DcIn{ kGpuUntrackedRowId,
				DrawCallRow{ "Untracked GPU (engine + DLSS):", kGpuUntrackedRowId, kDrawCallsNotApplicable, untrackedMs, 0.0f, 0.0f,
					std::string("GPU time this frame that is not one of our effects: the game's own rendering "
								"(shadows, scenery, water, its post-processing) and DLSS/FSR upscaling. "
								"Frame generation is not here; it is in \"Gap\".\n\n"
								"Large here and small in \"Measured GPU\" means Community Shaders is not what is "
								"costing you the frame. The \"Engine passes (GPU)\" section breaks this down by stage."),
					true, std::nullopt, std::nullopt },
				untrackedMs, 1 });
			footer.push_back(DcIn{ kGpuGapRowId,
				DrawCallRow{ "Gap: idle / flip / frame-gen:", kGpuGapRowId, kDrawCallsNotApplicable, frameGpu.presentSpanMs, 0.0f, 0.0f,
					std::string("GPU time while the finished frame is being shown: the display flip, V-Sync or "
								"frame-cap waiting, DLSS-G frame generation, and real idle time. Because it is a mix, "
								"the GPU was idle at most this long.\n\n"
								"With frame generation on, a large value is normal and does not mean spare GPU power; "
								"it grows with the multiplier."),
					true, std::nullopt, std::nullopt },
				frameGpu.presentSpanMs, 2 });
			footer.push_back(DcIn{ kGpuFrameElapsedRowId,
				DrawCallRow{ "GPU frame (elapsed):", kGpuFrameElapsedRowId, kDrawCallsNotApplicable, frameGpu.frameElapsedMs, 0.0f, 0.0f,
					std::string("Time from one frame to the next on the GPU's clock. It includes idle time, so it "
								"is not how busy the GPU was.\n\n"
								"It should match \"Total\" in the shader-type table; if it does not, do not trust the GPU numbers."),
					true, std::nullopt, std::nullopt },
				frameGpu.frameElapsedMs, 3 });
		}
		view.gpuFooter.Update(a_now, std::move(footer), a_cfg);
	}

	// --- Engine passes (GPU) ---
	{
		std::vector<DcIn> in;
		std::vector<DcIn> cpuIn;  // (batch 37a) same rows, CPU time
		const auto& report = Util::GpuPhaseTimeline::GetSingleton()->Get();
		if (report.hasSample) {
			const auto msOf = [&report](Util::GpuPhase p) { return report.ms[static_cast<size_t>(p)]; };
			const auto drawsOf = [&report](Util::GpuPhase p) { return report.draws[static_cast<size_t>(p)]; };
			const auto cpuOf = [&report](Util::GpuPhase p) { return report.cpuMs[static_cast<size_t>(p)]; };
			const auto rowVisible = [&](const EnginePhaseRow& a_row) {
				return a_row.phase == Util::GpuPhase::Untracked || msOf(a_row.phase) >= kEnginePhaseVisibleMs || drawsOf(a_row.phase) >= 0.5f ||
				       cpuOf(a_row.phase) >= kEnginePhaseVisibleMs;
			};
			const auto makeRow = [](std::string a_label, std::string a_tooltip, int a_id, float a_ms, float a_draws) {
				return DrawCallRow{ std::move(a_label), a_id, static_cast<int>(std::lround(a_draws)), a_ms, 0.0f, 0.0f, std::move(a_tooltip), true, std::nullopt, std::nullopt };
			};

			const auto& layout = EnginePhaseLayout();
			for (size_t g = 0; g < layout.size(); ++g) {
				const auto& group = layout[g];
				float groupMs = 0.0f, groupDraws = 0.0f, groupCpu = 0.0f;
				bool anyVisible = false;
				for (const auto& row : group.rows) {
					groupMs += msOf(row.phase);
					groupDraws += drawsOf(row.phase);
					groupCpu += cpuOf(row.phase);
					anyVisible |= rowVisible(row);
					if (!rowVisible(row))
						continue;
					std::string tooltip = row.tooltip ? row.tooltip : "";
					if (row.phase == Util::GpuPhase::Untracked) {
						// Which shader types the untracked draws were: tells where a missing hook is.
						std::string byType;
						for (size_t t = 0; t < report.untrackedDrawsByType.size(); ++t) {
							const float n = report.untrackedDrawsByType[t];
							if (n < 0.5f)
								continue;
							const auto name = magic_enum::enum_name(static_cast<RE::BSShader::Type>(t));
							byType += std::format("\n  {}: {}", name.empty() ? std::string_view("?") : name, std::lround(n));
						}
						if (!byType.empty())
							tooltip += "\n\nDraws in here, by shader:" + byType;
					}
					const int id = static_cast<int>(row.phase);
					std::string label = EngineRowLabel(row, report.shadow);
					cpuIn.push_back(DcIn{ id, makeRow(label, {}, id, cpuOf(row.phase), drawsOf(row.phase)), cpuOf(row.phase), id });
					in.push_back(DcIn{ id, makeRow(std::move(label), std::move(tooltip), id, msOf(row.phase), drawsOf(row.phase)), msOf(row.phase), id });
				}
				if (group.label && anyVisible) {
					const int id = kEngineGroupRowIdBase + static_cast<int>(g);
					in.push_back(DcIn{ id, makeRow(group.label, group.tooltip ? group.tooltip : "", id, groupMs, groupDraws), groupMs, id });
					cpuIn.push_back(DcIn{ id, makeRow(group.label, {}, id, groupCpu, groupDraws), groupCpu, id });
				}
			}

			float totalDraws = 0.0f;
			for (float d : report.draws)
				totalDraws += d;
			std::string totalTooltip =
				"Sum of all rows: the GPU time for one frame, not counting the \"Gap\" where frame generation runs. "
				"Should match \"Measured GPU\" + \"Untracked GPU\" in the GPU passes table.\n\n";
			totalTooltip += std::format("Timestamps per frame: {:.0f}", report.timestampsPerFrame);
			if (report.droppedFrames > 0)
				totalTooltip += std::format("\nFrames skipped (too many stage switches): {}", report.droppedFrames);
			in.push_back(DcIn{ kEngineTotalRowId, makeRow("Total", std::move(totalTooltip), kEngineTotalRowId, report.totalMs, totalDraws), report.totalMs, kEngineTotalRowId });
			float totalCpu = 0.0f;
			for (float c : report.cpuMs)
				totalCpu += c;
			cpuIn.push_back(DcIn{ kEngineTotalRowId, makeRow("Total", {}, kEngineTotalRowId, totalCpu, totalDraws), totalCpu, kEngineTotalRowId });
		}
		view.engine.Update(a_now, std::move(in), a_cfg);
		view.engineCpu.Update(a_now, std::move(cpuIn), a_cfg);
	}

	// --- Denoiser breakdown ---
	{
		using DnIn = PerfView::StableTable<DenoiserRow>::Input;
		std::vector<DnIn> in;
		std::vector<DnIn> footer;
		auto* timers = Util::DenoiserTimers::GetSingleton();
		const uint64_t latest = timers->LatestFrame();
		if (latest > 0) {
			std::vector<std::pair<std::string, float>> groupSums;
			float total = 0.0f;
			for (const auto& r : timers->Rows()) {
				if (r.lastFrame != latest)
					continue;
				DenoiserRow row;
				row.group = r.group;
				row.label = r.group + ": " + r.pass;
				row.tooltip = DenoiserPassTooltip(r.group, r.pass);
				if (r.pass == "One-time clears")
					row.tooltip = std::format(
						"NRD clearing its history surfaces, all in one row ({} dispatches the last time). Runs only when REBLUR "
						"restarts (first frame, a settings change, a load), so it normally shows \"-\".",
						r.lastCalls);
				row.groupsX = r.groupsX;
				row.groupsY = r.groupsY;
				row.threadsX = r.threadsX;
				row.threadsY = r.threadsY;
				row.calls = r.lastCalls;
				in.push_back(DnIn{ StableId(row.label), std::move(row), r.lastMs, r.order });

				auto it = std::find_if(groupSums.begin(), groupSums.end(), [&r](const auto& p) { return p.first == r.group; });
				if (it == groupSums.end())
					groupSums.emplace_back(r.group, r.lastMs);
				else
					it->second += r.lastMs;
				total += r.lastMs;
			}
			int order = 0;
			for (const auto& [group, ms] : groupSums) {
				DenoiserRow row;
				row.group = group;
				row.label = group + " total";
				row.tooltip = "Sum of this group's rows above.";
				row.isFooter = true;
				footer.push_back(DnIn{ StableId("sum|" + group), std::move(row), ms, order++ });
			}
			if (!groupSums.empty()) {
				DenoiserRow row;
				row.label = "Denoiser chain total";
				row.tooltip =
					"Every row above added up. The two NRD groups together correspond to the \"SSRT REBLUR\" row of the "
					"GPU passes table minus the unpacks; Guides corresponds to \"NRD Guides\".";
				row.isFooter = true;
				footer.push_back(DnIn{ 1, std::move(row), total, 1000 });
			}
		}
		view.denoiser.Update(a_now, std::move(in), a_cfg);
		view.denoiserFooter.Update(a_now, std::move(footer), a_cfg);

		if (globals::state) {
			const float2 output = globals::state->screenSize;
			// (batch 36f) The overlay is drawn after the upscaler, where the dynamic-resolution lock
			// is set and ConvertToDynamic returns the output size unchanged -- which is why this used
			// to read 3840x2160 under DLSS Quality. Prefer the rectangle NRD was actually given this
			// frame; otherwise ask for the ratio past the lock.
			const float2 render = Util::ConvertToDynamic(output, true);
			view.outputWidth = static_cast<uint32_t>(output.x);
			view.outputHeight = static_cast<uint32_t>(output.y);
			view.renderWidth = static_cast<uint32_t>(std::floor(render.x));
			view.renderHeight = static_cast<uint32_t>(std::floor(render.y));
			const auto& nrd = globals::features::nrd;
			if (nrd.loaded && nrd.hasCommonFrameHistory && nrd.prevRectSize[0] > 0 && nrd.prevRectSize[1] > 0) {
				view.renderWidth = nrd.prevRectSize[0];
				view.renderHeight = nrd.prevRectSize[1];
			}
		}
	}

	// --- Compact summary ---
	{
		using SIn = PerfView::StableTable<int>::Input;
		std::vector<SIn> in;
		const float cpuFrameMs = globals::state->GetAttributionFrameTimeMs();
		if (cpuFrameMs > 0.0f)
			in.push_back(SIn{ kSummaryCpuFrame, 0, cpuFrameMs, 0 });
		in.push_back(SIn{ kSummaryPresentWait, 0, Util::CpuPassTimers::GetSingleton()->GetPresentWaitMs(), 0 });
		in.push_back(SIn{ kSummaryGpuOurs, 0, bucketSum, 0 });
		if (frameGpu.hasSample) {
			in.push_back(SIn{ kSummaryGpuUntracked, 0, std::max(0.0f, frameGpu.workSpanMs - bucketSum), 0 });
			in.push_back(SIn{ kSummaryGpuGap, 0, frameGpu.presentSpanMs, 0 });
		}
		if (state.frameTimeMs > 0.0f)
			in.push_back(SIn{ kSummaryFrame, 0, state.frameTimeMs, 0 });
		if (state.isFrameGenerationActive && state.postFGFrameTimeMs > 0.0f)
			in.push_back(SIn{ kSummaryPostFgFrame, 0, state.postFGFrameTimeMs, 0 });
		view.summary.Update(a_now, std::move(in), a_cfg);
	}
}

void PerformanceOverlay::DrawCompactSummary(const PerfView::ViewConfig& a_cfg)
{
	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();
	const auto value = [this, &a_cfg](int a_id, float& o_value) { return view.summary.DisplayValue(a_id, a_cfg, o_value); };

	// (batch 37a) Top of the panel: save this frame as JSON (clickable while the CS menu is
	// open, the overlay ignores the mouse otherwise), the hotkey that does the same, and the
	// result of the last save.
	if (menu->IsEnabled) {
		if (ImGui::SmallButton("Save frame (JSON)"))
			view.pendingJsonSave = true;
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Saves everything this overlay shows for the current frame (or the frozen one) to\n"
					"Documents/My Games/Skyrim Special Edition/SKSE/CommunityShaders/Perf/perf-<time>.json");
		}
		ImGui::SameLine(0.0f, 12.0f);
	}
	ImGui::TextDisabled("%s: freeze + save", Util::Input::KeyIdToString(settings.FreezeKey));
	if (!view.message.empty() && NowSeconds() < view.messageUntil) {
		// Blinks for the first second so it is noticed, then stays steady.
		const double age = view.messageUntil - NowSeconds();
		const bool blinkOff = age > kMessageSeconds - 1.0 && std::fmod(age * 4.0, 1.0) < 0.35;
		const ImVec4 color = view.messageIsError ? theme.StatusPalette.Error : theme.StatusPalette.SuccessColor;
		ImGui::TextColored(blinkOff ? ImVec4(color.x, color.y, color.z, 0.35f) : color, "%s", view.message.c_str());
	}

	float frameMs = 0.0f;
	if (value(kSummaryFrame, frameMs) && frameMs > 0.0f)
		ImGui::Text("%s %.1f (%.2f ms)", state.isFrameGenerationActive ? "Raw FPS" : "FPS", 1000.0f / frameMs, frameMs);
	else
		ImGui::TextDisabled("FPS: collecting...");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::Text("Frame rate before frame generation, %s.",
				a_cfg.showLive ? "live" : std::format("averaged over {:.2f} s", a_cfg.windowSeconds).c_str());
		}
	}
	float postFgMs = 0.0f;
	if (value(kSummaryPostFgFrame, postFgMs) && postFgMs > 0.0f) {
		ImGui::SameLine(0.0f, 16.0f);
		ImGui::Text("Post-FG %.1f", 1000.0f / postFgMs);
	}
	if (view.frozen) {
		ImGui::SameLine(0.0f, 16.0f);
		ImGui::TextColored(theme.StatusPalette.Warning, "FROZEN");
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"All numbers are held. Press %s again (or the Unfreeze button) to resume. "
					"A snapshot was written to CommunityShaders.log and a JSON file to SKSE/CommunityShaders/Perf.",
					Util::Input::KeyIdToString(settings.FreezeKey));
		}
	}

	if (settings.ShowDrawCalls) {
		float gpuOursMs = 0.0f, cpuFrameMs = 0.0f, waitMs = 0.0f;
		value(kSummaryGpuOurs, gpuOursMs);
		value(kSummaryCpuFrame, cpuFrameMs);
		value(kSummaryPresentWait, waitMs);
		ImGui::Text("GPU (ours) %.2f ms", gpuOursMs);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("GPU time of Community Shaders' own effects (the GPU passes rows added up).");
		}
		ImGui::SameLine(0.0f, 16.0f);
		const auto verdict = BottleneckVerdict(theme, cpuFrameMs, waitMs);
		ImGui::TextUnformatted("Bottleneck:");
		ImGui::SameLine();
		ImGui::TextColored(verdict.second, "%s", verdict.first);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("From how long the CPU waits for each frame to be shown. Details in \"CPU / GPU split\".");
		}
	}

	// Only clickable while the CS menu is open (the overlay ignores the mouse otherwise); the
	// hotkey works any time.
	if (menu->IsEnabled) {
		if (ImGui::SmallButton(view.frozen ? "Unfreeze" : "Freeze"))
			ToggleFreeze();
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::Text(
					"Holds every number so it can be read or screenshotted, writes them all to CommunityShaders.log "
					"and saves the frame as JSON. Hotkey: %s.",
					Util::Input::KeyIdToString(settings.FreezeKey));
		}
		ImGui::SameLine();
		if (ImGui::SmallButton("Log snapshot"))
			view.pendingSnapshot = true;
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Writes every table, as shown right now, to CommunityShaders.log without freezing.");
		}
	}
}

void PerformanceOverlay::DrawViewOptions()
{
	ImGui::PushID("PerfViewOptions");

	ImGui::Combo("Sort rows", &settings.SortMode, kSortModeNames, IM_ARRAYSIZE(kSortModeNames));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(
			"By cost (smoothed): most expensive first, using the smoothed numbers. Two rows only swap once one has been "
			"more than 10% more expensive for half a second, so the order does not flicker.\n"
			"Fixed order: rows never move.\n"
			"By cost (live, jumps): the old behaviour, re-sorted every frame. For debugging.");
	}

	ImGui::Combo("Smoothing", &settings.SmoothingMode, kSmoothModeNames, IM_ARRAYSIZE(kSmoothModeNames));
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(
			"Window average: the average over the last Window seconds (default).\n"
			"EMA: a running average that reacts a little faster to changes.\n"
			"Both are measured in time, not frames, so they behave the same with frame generation on or off.");
	}

	int windowIdx = 1;
	for (int i = 0; i < IM_ARRAYSIZE(kWindowChoices); ++i) {
		if (std::abs(settings.SmoothingWindow - kWindowChoices[i]) < 0.01f)
			windowIdx = i;
	}
	if (ImGui::Combo("Window", &windowIdx, kWindowNames, IM_ARRAYSIZE(kWindowNames)))
		settings.SmoothingWindow = kWindowChoices[windowIdx];
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("How much time the smoothing and the Peak column look back over. Longer = steadier numbers, slower to react.");

	int valuesIdx = settings.ShowLiveValues ? 1 : 0;
	const char* valueNames[] = { "Smoothed", "Live" };
	if (ImGui::Combo("Numbers shown", &valuesIdx, valueNames, IM_ARRAYSIZE(valueNames)))
		settings.ShowLiveValues = valuesIdx == 1;
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Smoothed is easier to read. Live is the timer's own current reading, as the panel showed before.");

	ImGui::Checkbox("Peak column", &settings.ShowPeakColumn);
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Adds a column with the highest reading inside the window, to catch spikes the average hides.");

	ImGui::SliderInt("Top N rows", &settings.TopN, 0, 30, settings.TopN == 0 ? "all" : "%d");
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(
			"Show only the N most expensive rows of each sortable table (0 = all). The N are picked with the same "
			"half-second rule as the sorting, so the row count stays constant. Totals are always shown.");
	}

	int logIdx = 0;
	for (int i = 0; i < IM_ARRAYSIZE(kLogChoices); ++i) {
		if (settings.DenoiserLogInterval == kLogChoices[i])
			logIdx = i;
	}
	if (ImGui::Combo("Denoiser log", &logIdx, kLogNames, IM_ARRAYSIZE(kLogNames)))
		settings.DenoiserLogInterval = kLogChoices[logIdx];
	if (auto _tt = Util::HoverTooltipWrapper()) {
		ImGui::TextUnformatted(
			"While the \"Denoiser breakdown\" section is open, also write one line of its smoothed numbers to "
			"CommunityShaders.log this often. Freeze always writes a full snapshot regardless.");
	}

	ImGui::TextUnformatted("Freeze key:");
	ImGui::SameLine();
	if (capturingFreezeKey) {
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.CurrentHotkey, "press a key...");
	} else {
		ImGui::TextColored(Menu::GetSingleton()->GetTheme().StatusPalette.CurrentHotkey, "%s", Util::Input::KeyIdToString(settings.FreezeKey));
		ImGui::SameLine();
		if (ImGui::SmallButton("Change"))
			capturingFreezeKey = true;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Holds every number on the overlay (and writes a snapshot to the log). Works while playing, without the menu open.");

	ImGui::SameLine();
	if (ImGui::SmallButton(view.frozen ? "Unfreeze" : "Freeze"))
		ToggleFreeze();
	ImGui::SameLine();
	if (ImGui::SmallButton("Forget old rows")) {
		const bool frozen = view.frozen;
		auto frozenState = view.frozenState;
		const auto frozenStats = view.frozenStats;
		view = ViewState{};
		view.frozen = frozen;
		view.frozenState = frozenState;
		view.frozenStats = frozenStats;
	}
	if (auto _tt = Util::HoverTooltipWrapper())
		ImGui::TextUnformatted("Rows stay in place once they have appeared. This clears them, e.g. after switching an effect off for good.");

	ImGui::PopID();
}

void PerformanceOverlay::DrawDenoiserTable(const PerfView::ViewConfig& a_cfg)
{
	ImGui::Text("Render %ux%u, output %ux%u", view.renderWidth, view.renderHeight, view.outputWidth, view.outputHeight);
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"Render = the resolution the game draws at before DLSS/FSR upscales it; output = the screen. "
				"A pass whose Covers matches Render runs at render resolution.\n\n"
				"Packing REBLUR's input has no pass of its own (it is done inside the ray march), so it has no row.\n\n"
				"Each row costs two GPU timestamps while this section is open; nothing when it is closed.");
		}
	}

	const auto& table = view.denoiser;
	const auto& footerTable = view.denoiserFooter;
	if (table.RowCount() == 0) {
		ImGui::TextDisabled("Nothing measured yet: needs SSRT with the REBLUR denoiser (or the confidence filter) running.");
		return;
	}

	float totalMs = 0.0f;
	footerTable.DisplayValue(1, a_cfg, totalMs);

	const bool peakColumn = settings.ShowPeakColumn;
	if (!ImGui::BeginTable("DenoiserBreakdown", peakColumn ? 5 : 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp))
		return;
	ImGui::TableSetupColumn("Pass");
	ImGui::TableSetupColumn("Groups");
	ImGui::TableSetupColumn("Covers");
	ImGui::TableSetupColumn("GPU Time (%)");
	if (peakColumn)
		ImGui::TableSetupColumn("Peak");
	ImGui::TableHeadersRow();

	const auto drawRow = [&](const PerfView::StableTable<DenoiserRow>& a_table, int a_id) {
		const DenoiserRow* row = a_table.GetRow(a_id);
		if (!row)
			return;
		float ms = 0.0f;
		const bool has = a_table.DisplayValue(a_id, a_cfg, ms);

		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(row->label.c_str());
		if (!row->tooltip.empty() && ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(row->tooltip.c_str());
		}

		ImGui::TableNextColumn();
		if (!row->isFooter && row->groupsX > 0)
			ImGui::Text("%ux%u", row->groupsX, row->groupsY);
		else if (!row->isFooter)
			ImGui::TextDisabled("copy");
		else
			ImGui::TextDisabled("-");

		ImGui::TableNextColumn();
		if (!row->isFooter && row->threadsX > 0)
			ImGui::Text("%ux%u", row->threadsX, row->threadsY);
		else
			ImGui::TextDisabled("-");

		ImGui::TableNextColumn();
		if (!has)
			ImGui::TextDisabled("-");
		else
			ImGui::Text("%s (%.1f%%)", ms < 0.01f ? "< 0.01 ms" : std::format("{:.2f} ms", ms).c_str(), totalMs > 0.0f ? ms / totalMs * 100.0f : 0.0f);

		if (peakColumn) {
			ImGui::TableNextColumn();
			const auto values = a_table.GetValues(a_id);
			if (values.hasData)
				ImGui::Text("%.2f ms", values.peak);
			else
				ImGui::TextDisabled("-");
		}
	};

	for (int id : table.DisplayOrder(a_cfg))
		drawRow(table, id);

	PerfView::ViewConfig footerCfg = a_cfg;
	footerCfg.sort = PerfView::SortMode::Fixed;
	footerCfg.topN = 0;
	const auto footerOrder = footerTable.DisplayOrder(footerCfg);
	if (!footerOrder.empty()) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Separator();
	}
	for (int id : footerOrder)
		drawRow(footerTable, id);

	ImGui::EndTable();
}

std::vector<DrawCallRow> PerformanceOverlay::MaterializeRows(const PerfView::StableTable<DrawCallRow>& a_table, const std::vector<int>& a_order,
	const PerfView::ViewConfig& a_cfg, float a_percentBase, bool a_recomputeCostPerCall)
{
	std::vector<DrawCallRow> rows;
	rows.reserve(a_order.size());
	for (int id : a_order) {
		const DrawCallRow* stored = a_table.GetRow(id);
		if (!stored)
			continue;
		DrawCallRow row = *stored;
		float value = 0.0f;
		row.hasData = a_table.DisplayValue(id, a_cfg, value);
		row.frameTime = row.hasData ? value : 0.0f;
		row.percent = (row.hasData && a_percentBase > 0.0f) ? (value / a_percentBase * 100.0f) : 0.0f;
		const auto values = a_table.GetValues(id);
		row.peak = values.hasData ? std::optional<float>(values.peak) : std::nullopt;
		if (a_recomputeCostPerCall)
			row.costPerCall = (row.hasData && row.drawCalls > 0) ? (value / static_cast<float>(row.drawCalls)) : 0.0f;
		rows.push_back(std::move(row));
	}
	return rows;
}

void PerformanceOverlay::DrawStableTable(const char* a_id, const std::vector<ColumnConfig>& a_columns, const std::vector<DrawCallRow>& a_rows,
	const std::vector<DrawCallRow>& a_footer, const std::function<void(int, int, const DrawCallRow&)>& a_cellRender)
{
	// Deliberately not sortable by header click: the order is the view layer's (see the
	// "Sort rows" option), and clicking a header used to re-sort by the jumping live value.
	const ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp;
	if (!ImGui::BeginTable(a_id, static_cast<int>(a_columns.size()), flags))
		return;
	for (const auto& col : a_columns)
		ImGui::TableSetupColumn(col.header.c_str());

	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (size_t c = 0; c < a_columns.size(); ++c) {
		ImGui::TableSetColumnIndex(static_cast<int>(c));
		ImGui::TableHeader(a_columns[c].header.c_str());
		if (a_columns[c].headerTooltip)
			a_columns[c].headerTooltip();
	}

	int rowIdx = 0;
	const auto drawRows = [&](const std::vector<DrawCallRow>& a_list) {
		for (const auto& row : a_list) {
			ImGui::TableNextRow();
			for (size_t c = 0; c < a_columns.size(); ++c) {
				ImGui::TableSetColumnIndex(static_cast<int>(c));
				ImGui::PushID(rowIdx * 64 + static_cast<int>(c));
				a_cellRender(rowIdx, static_cast<int>(c), row);
				ImGui::PopID();
			}
			++rowIdx;
		}
	};
	drawRows(a_rows);
	if (!a_footer.empty() && !a_rows.empty()) {
		ImGui::TableNextRow();
		ImGui::TableSetColumnIndex(0);
		ImGui::Separator();
	}
	drawRows(a_footer);
	ImGui::EndTable();
}

std::string PerformanceOverlay::FormatDenoiserLogLine(const PerfView::ViewConfig& a_cfg) const
{
	PerfView::ViewConfig cfg = a_cfg;
	cfg.showLive = false;  // the log line is always the smoothed value
	cfg.sort = PerfView::SortMode::Fixed;
	cfg.topN = 0;

	float total = 0.0f;
	if (!view.denoiserFooter.DisplayValue(1, cfg, total))
		return {};

	std::string line = std::format("[PerfDenoiser] window {:.2f} s | render {}x{} | total {:.3f} ms",
		cfg.windowSeconds, view.renderWidth, view.renderHeight, total);
	for (int id : view.denoiser.DisplayOrder(cfg)) {
		const DenoiserRow* row = view.denoiser.GetRow(id);
		float ms = 0.0f;
		if (!row || !view.denoiser.DisplayValue(id, cfg, ms))
			continue;
		line += std::format(" | {} {:.3f}", row->label, ms);
	}
	return line;
}

void PerformanceOverlay::WriteSnapshotToLog(const PerfView::ViewConfig& a_cfg)
{
	// Everything in the order and with the values the overlay shows, but never trimmed by
	// Top N: the log is for reading later, the whole table is wanted.
	PerfView::ViewConfig cfg = a_cfg;
	cfg.topN = 0;
	PerfView::ViewConfig fixedCfg = cfg;
	fixedCfg.sort = PerfView::SortMode::Fixed;

	const char* valueKind = cfg.showLive ? "live" : (cfg.smooth == PerfView::SmoothMode::Off ? "unsmoothed" : "smoothed");
	logger::info("[PerfSnapshot] ===== begin{} | values {} | smoothing {} {:.2f} s | sort {} =====",
		view.frozen ? " (frozen)" : "", valueKind, kSmoothModeNames[static_cast<int>(cfg.smooth)], cfg.windowSeconds,
		kSortModeNames[static_cast<int>(cfg.sort)]);

	const auto summary = [this, &cfg](int a_id) {
		float v = 0.0f;
		return view.summary.DisplayValue(a_id, cfg, v) ? v : -1.0f;
	};
	const float frameMs = summary(kSummaryFrame);
	const float cpuFrame = summary(kSummaryCpuFrame);
	const float wait = summary(kSummaryPresentWait);
	const float postFg = summary(kSummaryPostFgFrame);
	logger::info("[PerfSnapshot] Summary | FPS {:.1f} | frame {:.2f} ms{} | CPU frame {:.2f} ms | present wait {:.2f} ms | GPU ours {:.2f} ms | GPU untracked {:.2f} ms | GPU gap {:.2f} ms",
		frameMs > 0.0f ? 1000.0f / frameMs : 0.0f, std::max(0.0f, frameMs),
		postFg > 0.0f ? std::format(" | post-FG FPS {:.1f}", 1000.0f / postFg) : std::string(),
		std::max(0.0f, cpuFrame), std::max(0.0f, wait), std::max(0.0f, summary(kSummaryGpuOurs)),
		std::max(0.0f, summary(kSummaryGpuUntracked)), std::max(0.0f, summary(kSummaryGpuGap)));
	logger::info("[PerfSnapshot] Resolution | render {}x{} | output {}x{}", view.renderWidth, view.renderHeight, view.outputWidth, view.outputHeight);

	const auto logTable = [&](const char* a_name, const PerfView::StableTable<DrawCallRow>& a_table, const PerfView::ViewConfig& a_order) {
		for (int id : a_table.DisplayOrder(a_order)) {
			const DrawCallRow* row = a_table.GetRow(id);
			if (!row)
				continue;
			float v = 0.0f;
			const bool has = a_table.DisplayValue(id, cfg, v);
			const auto values = a_table.GetValues(id);
			logger::info("[PerfSnapshot] {} | {} | {} | peak {} | count {}", a_name, row->label, FormatMsForLog(has, v),
				FormatMsForLog(values.hasData, values.peak), row->drawCalls);
		}
	};
	logTable("Shader types (CPU)", view.shaderTypes, cfg);
	logTable("Shader types (CPU)", view.shaderFooter, fixedCfg);
	logTable("CS CPU submit", view.csCpu, cfg);
	logTable("GPU passes", view.gpuPasses, cfg);
	logTable("GPU passes", view.gpuFooter, fixedCfg);
	logTable("Engine passes (GPU)", view.engine, fixedCfg);
	logTable("Engine passes (CPU)", view.engineCpu, fixedCfg);

	const auto logDenoiser = [&](const PerfView::StableTable<DenoiserRow>& a_table, const PerfView::ViewConfig& a_order) {
		for (int id : a_table.DisplayOrder(a_order)) {
			const DenoiserRow* row = a_table.GetRow(id);
			if (!row)
				continue;
			float v = 0.0f;
			const bool has = a_table.DisplayValue(id, cfg, v);
			const auto values = a_table.GetValues(id);
			if (row->isFooter) {
				logger::info("[PerfSnapshot] Denoiser | {} | {} | peak {}", row->label, FormatMsForLog(has, v), FormatMsForLog(values.hasData, values.peak));
			} else {
				logger::info("[PerfSnapshot] Denoiser | {} | {} | peak {} | groups {}x{} | covers {}x{}", row->label, FormatMsForLog(has, v),
					FormatMsForLog(values.hasData, values.peak), row->groupsX, row->groupsY, row->threadsX, row->threadsY);
			}
		}
	};
	if (view.denoiser.RowCount() == 0)
		logger::info("[PerfSnapshot] Denoiser | (not measured: open the \"Denoiser breakdown\" section first)");
	logDenoiser(view.denoiser, cfg);
	logDenoiser(view.denoiserFooter, fixedCfg);

	logger::info("[PerfSnapshot] ===== end =====");
}

// ============================================================================
// (batch 37a) SHADOW INFO, FRAME JSON, FREEZE KEY
// ============================================================================

namespace
{
	const char* CascadeName(uint32_t a_count, uint32_t a_index)
	{
		if (a_count == 1)
			return "single";
		if (a_count == 2)
			return a_index == 0 ? "near" : (a_index == 1 ? "far" : "unused");
		if (a_count == 3)
			return a_index == 0 ? "near" : (a_index == 1 ? "middle" : "far");
		return "?";
	}

	const char* QualityModeName(uint a_mode)
	{
		switch (a_mode) {
		case 0:
			return "Native AA (DLAA)";
		case 1:
			return "Quality";
		case 2:
			return "Balanced";
		case 3:
			return "Performance";
		case 4:
			return "Ultra Performance";
		default:
			return "?";
		}
	}

	/// The menu's own hotkeys, which win over the Freeze key when they are the same key.
	std::vector<uint32_t> OtherHotkeys()
	{
		std::vector<uint32_t> keys;
		if (auto* menu = Menu::GetSingleton()) {
			auto& s = menu->GetSettings();
			keys = { s.ToggleKey, s.SkipCompilationKey, s.EffectToggleKey, s.OverlayToggleKey, s.ShaderBlockPrevKey, s.ShaderBlockNextKey };
		}
		return keys;
	}
}

void PerformanceOverlay::FlashMessage(std::string a_text, bool a_error)
{
	view.message = std::move(a_text);
	view.messageIsError = a_error;
	view.messageUntil = NowSeconds() + kMessageSeconds;
}

void PerformanceOverlay::ResolveFreezeKeyConflict()
{
	// Menu::ProcessInputEventQueue runs the FIRST action bound to a released key and stops, and
	// the CS menu's own keys come first. A Freeze key equal to one of them (the 36g report: the
	// menu was on F11, the default Freeze key) therefore never froze anything and never wrote a
	// snapshot; F11 only opened and closed the menu. Move it to a free key and say so.
	if (capturingFreezeKey || settings.FreezeKey == 0)
		return;
	const auto taken = OtherHotkeys();
	if (std::find(taken.begin(), taken.end(), settings.FreezeKey) == taken.end())
		return;
	const uint32_t old = settings.FreezeKey;
	for (uint32_t candidate : { (uint32_t)VK_F12, (uint32_t)VK_F8, (uint32_t)VK_F7, (uint32_t)VK_F6, (uint32_t)VK_PAUSE }) {
		if (std::find(taken.begin(), taken.end(), candidate) == taken.end()) {
			settings.FreezeKey = candidate;
			break;
		}
	}
	const std::string oldName = Util::Input::KeyIdToString(old);
	const std::string newName = Util::Input::KeyIdToString(settings.FreezeKey);
	logger::warn("[PerformanceOverlay] Freeze key {} is also a Community Shaders menu hotkey, which takes it first; Freeze moved to {}",
		oldName, newName);
	FlashMessage(std::format("Freeze key {} is your CS menu key - Freeze + save is now {}", oldName, newName), true);
	view.messageUntil = NowSeconds() + 30.0;
}

void PerformanceOverlay::DrawShadowInfo()
{
	const auto& report = Util::GpuPhaseTimeline::GetSingleton()->Get();
	const auto& s = report.shadow;
	if (!report.hasCpuSample) {
		ImGui::TextDisabled("Open \"Engine passes (GPU)\" once to start measuring.");
		return;
	}
	if (!s.valid) {
		ImGui::TextDisabled("No sun shadow this frame.");
		ImGui::Text("Lamp & torch shadow maps: %u", s.localShadowMaps);
		return;
	}
	ImGui::Text("Sun cascades: %u   Character shadow maps: %u%s   Lamp & torch shadow maps: %u", s.sunCascades, s.focusShadows,
		s.drawFocusShadows ? "" : " (off)", s.localShadowMaps);
	for (uint32_t i = 0; i < std::min(s.sunCascades, 3u); ++i) {
		ImGui::Text("  %s: %.0f - %.0f units, %.1f units per shadow texel", CascadeName(s.sunCascades, i), s.startSplit[i], s.endSplit[i],
			s.unitsPerTexel[i]);
	}
	ImGui::TextDisabled("God-ray maps %s | per-map timing %s", s.godRayPass ? "drawn" : "not drawn",
		s.sliceHooks ? "exact (SE 1.5.97 hooks)" : "by draw target");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper())
			ImGui::TextUnformatted(
				"God-ray maps: the game draws a second, lighter copy of each sun cascade for god rays (volumetric light).\n"
				"Per-map timing: on SE 1.5.97 the clear and all draws of each shadow map are timed as one block; otherwise each draw is "
				"assigned by the shadow map it goes into.");
	}
}

nlohmann::json PerformanceOverlay::BuildFrameJson(const PerfView::ViewConfig& a_cfg)
{
	using nlohmann::json;
	PerfView::ViewConfig cfg = a_cfg;
	cfg.topN = 0;
	PerfView::ViewConfig fixedCfg = cfg;
	fixedCfg.sort = PerfView::SortMode::Fixed;

	json j;
	j["schema"] = "cs-perf-frame";
	j["schema_version"] = 1;

	// ---- meta ----
	{
		json& m = j["meta"];
		const auto now = std::chrono::system_clock::now();
		const std::time_t t = std::chrono::system_clock::to_time_t(now);
		std::tm local{};
		localtime_s(&local, &t);
		char buf[64];
		std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &local);
		m["time_local"] = buf;
		m["time_unix"] = static_cast<int64_t>(t);
		m["plugin_version"] = Util::GetFormattedVersion(Plugin::VERSION);
		m["build"] = "batch37a";
		m["game_version"] = Util::GetFormattedVersion(REL::Module::get().version());
		m["frozen"] = view.frozen;
		m["values"] = cfg.showLive ? "live" : (cfg.smooth == PerfView::SmoothMode::Off ? "unsmoothed" : "smoothed");
		m["smoothing"] = { { "mode", kSmoothModeNames[static_cast<int>(cfg.smooth)] }, { "window_s", cfg.windowSeconds } };

		json loc = json::object();
		if (auto* player = RE::PlayerCharacter::GetSingleton()) {
			const auto pos = player->GetPosition();
			loc["position"] = { pos.x, pos.y, pos.z };
			if (auto* cell = player->GetParentCell()) {
				const char* name = cell->GetName();
				const char* edid = cell->GetFormEditorID();
				loc["cell_name"] = name ? name : "";
				loc["cell_editor_id"] = edid ? edid : "";
				loc["cell_form_id"] = std::format("{:08X}", cell->GetFormID());
				loc["interior"] = cell->IsInteriorCell();
			}
			if (auto* ws = player->GetWorldspace()) {
				const char* name = ws->GetName();
				const char* edid = ws->GetFormEditorID();
				loc["worldspace_name"] = name ? name : "";
				loc["worldspace_editor_id"] = edid ? edid : "";
				loc["worldspace_form_id"] = std::format("{:08X}", ws->GetFormID());
			}
		}
		m["location"] = std::move(loc);

		m["resolution"] = { { "render", { view.renderWidth, view.renderHeight } }, { "output", { view.outputWidth, view.outputHeight } } };
		auto& up = globals::features::upscaling;
		m["upscaling"] = {
			{ "loaded", up.loaded },
			{ "method", std::string(magic_enum::enum_name(up.GetUpscaleMethod())) },
			{ "quality_mode", up.settings.qualityMode },
			{ "quality", QualityModeName(up.settings.qualityMode) },
			{ "scale", { up.resolutionScale.x, up.resolutionScale.y } },
			{ "dlss_preset", up.settings.DLSSPreset },
			{ "reflex_mode", up.settings.reflexMode },
		};
		// (batch 38a) What DLSS 5 Neural Rendering did, and what its network alone cost on its own D3D12 queue.
		m["upscaling"]["neural_rendering"] = NeuralRendering::StatusJson();
		const State& st = (view.frozen && view.frozenState) ? *view.frozenState : state;
		m["frame_generation"] = {
			{ "active", st.isFrameGenerationActive },
			{ "dlssg", st.frameGenerationIsDLSSG },
			{ "multiplier_applied", st.appliedFGMultiplier },
			{ "multiplier_used_for_post_fg", st.postFGMultiplier },
			{ "multiplier_measured", st.postFGIsMeasured },
			{ "multiplier_rejected", st.rejectedFGMultiplier },
		};
		m["effects_enabled"] = globals::shaderCache ? globals::shaderCache->IsEnabled() : false;

		// Every feature's load state, and the full settings of the ones whose numbers this
		// overlay is usually read for. Their SaveSettings only copies the settings struct.
		json features = json::object();
		static const std::unordered_set<std::string> kSettingsDumped = { "ScreenSpaceRayTracing", "NRD", "VariableRateShading", "GrassOptimizations",
			"ScreenSpaceGI", "Skylighting", "ScreenSpaceShadows", "TerrainShadows", "VolumetricLighting" };
		json featureSettings = json::object();
		for (auto* feature : Feature::GetFeatureList()) {
			if (!feature)
				continue;
			const std::string name = feature->GetShortName();
			features[name] = { { "loaded", feature->loaded }, { "version", feature->version } };
			if (feature->loaded && kSettingsDumped.contains(name)) {
				json fs;
				feature->SaveSettings(fs);
				featureSettings[name] = std::move(fs);
			}
		}
		m["features"] = std::move(features);
		m["feature_settings"] = std::move(featureSettings);
	}

	// ---- summary ----
	{
		const auto summary = [this, &cfg](int a_id) -> json {
			float v = 0.0f;
			return view.summary.DisplayValue(a_id, cfg, v) ? json(v) : json(nullptr);
		};
		float frameMs = 0.0f, cpuFrame = 0.0f, wait = 0.0f, postFg = 0.0f;
		view.summary.DisplayValue(kSummaryFrame, cfg, frameMs);
		view.summary.DisplayValue(kSummaryCpuFrame, cfg, cpuFrame);
		view.summary.DisplayValue(kSummaryPresentWait, cfg, wait);
		const bool hasPostFg = view.summary.DisplayValue(kSummaryPostFgFrame, cfg, postFg);
		const auto verdict = BottleneckVerdict(Menu::GetSingleton()->GetTheme(), cpuFrame, wait);
		const FrameStats stats = view.frozen ? view.frozenStats : ComputeFrameStats();
		j["summary"] = {
			{ "fps", frameMs > 0.0f ? 1000.0f / frameMs : 0.0f },
			{ "frame_ms", summary(kSummaryFrame) },
			{ "post_fg_fps", hasPostFg && postFg > 0.0f ? json(1000.0f / postFg) : json(nullptr) },
			{ "cpu_frame_ms", summary(kSummaryCpuFrame) },
			{ "present_wait_ms", summary(kSummaryPresentWait) },
			{ "gpu_ours_ms", summary(kSummaryGpuOurs) },
			{ "gpu_untracked_ms", summary(kSummaryGpuUntracked) },
			{ "gpu_gap_ms", summary(kSummaryGpuGap) },
			{ "bottleneck", verdict.first },
			{ "stats_10s", { { "valid", stats.valid }, { "frames", stats.frames }, { "avg_ms", stats.averageMs }, { "low_1pct_ms", stats.percentile99Ms } } },
		};
	}

	// ---- tables ----
	{
		const auto rowsOf = [&](const PerfView::StableTable<DrawCallRow>& a_table, const PerfView::ViewConfig& a_order, const char* a_countName) {
			json rows = json::array();
			for (int id : a_table.DisplayOrder(a_order)) {
				const DrawCallRow* row = a_table.GetRow(id);
				if (!row)
					continue;
				float v = 0.0f;
				const bool has = a_table.DisplayValue(id, cfg, v);
				const auto values = a_table.GetValues(id);
				json r = {
					{ "id", id },
					{ "label", row->label },
					{ "ms", has ? json(v) : json(nullptr) },
					{ "smoothed_ms", values.smoothed },
					{ "live_ms", values.live },
					{ "peak_ms", values.peak },
					{ "has_data", values.hasData },
				};
				if (row->drawCalls >= 0) {
					r[a_countName] = row->drawCalls;
					if (row->drawCalls > 0 && has)
						r["ms_per_count"] = v / static_cast<float>(row->drawCalls);
				}
				rows.push_back(std::move(r));
			}
			return rows;
		};
		json& t = j["tables"];
		t["shader_types_cpu"] = rowsOf(view.shaderTypes, cfg, "draws");
		t["shader_types_cpu_footer"] = rowsOf(view.shaderFooter, fixedCfg, "draws");
		t["cs_cpu_submit"] = rowsOf(view.csCpu, cfg, "calls");
		t["gpu_passes"] = rowsOf(view.gpuPasses, cfg, "intervals");
		t["gpu_passes_footer"] = rowsOf(view.gpuFooter, fixedCfg, "intervals");

		// Engine passes: every row of the layout, always, in layout order (a stable shape).
		const auto& report = Util::GpuPhaseTimeline::GetSingleton()->Get();
		const auto shown = [&](const PerfView::StableTable<DrawCallRow>& a_table, int a_id) -> json {
			float v = 0.0f;
			return a_table.DisplayValue(a_id, cfg, v) ? json(v) : json(nullptr);
		};
		json engine = json::array();
		const auto& layout = EnginePhaseLayout();
		for (size_t g = 0; g < layout.size(); ++g) {
			for (const auto& row : layout[g].rows) {
				const size_t p = static_cast<size_t>(row.phase);
				const int id = static_cast<int>(row.phase);
				engine.push_back({
					{ "key", EnginePhaseKey(row.phase) },
					{ "group", layout[g].label ? layout[g].label : "" },
					{ "label", EngineRowLabel(row, report.shadow) },
					{ "gpu_ms", shown(view.engine, id) },
					{ "gpu_ms_peak", view.engine.GetValues(id).peak },
					{ "gpu_ms_last_frame", report.lastMs[p] },
					{ "cpu_ms", shown(view.engineCpu, id) },
					{ "cpu_ms_last_frame", report.lastCpuMs[p] },
					{ "draws", report.draws[p] },
					{ "draws_last_frame", report.lastDraws[p] },
				});
			}
		}
		t["engine_passes"] = std::move(engine);
		t["engine_total"] = {
			{ "gpu_ms", shown(view.engine, kEngineTotalRowId) },
			{ "cpu_ms", shown(view.engineCpu, kEngineTotalRowId) },
			{ "timestamps_per_frame", report.timestampsPerFrame },
			{ "dropped_frames", report.droppedFrames },
			{ "measuring", report.hasSample },
		};
	}

	// ---- denoiser ----
	{
		json d = json::object();
		d["measured"] = view.denoiser.RowCount() > 0;
		json rows = json::array();
		const auto add = [&](const PerfView::StableTable<DenoiserRow>& a_table, const PerfView::ViewConfig& a_order) {
			for (int id : a_table.DisplayOrder(a_order)) {
				const DenoiserRow* row = a_table.GetRow(id);
				if (!row)
					continue;
				float v = 0.0f;
				const bool has = a_table.DisplayValue(id, cfg, v);
				const auto values = a_table.GetValues(id);
				rows.push_back({
					{ "label", row->label },
					{ "group", row->group },
					{ "footer", row->isFooter },
					{ "ms", has ? json(v) : json(nullptr) },
					{ "live_ms", values.live },
					{ "peak_ms", values.peak },
					{ "groups", { row->groupsX, row->groupsY } },
					{ "covers", { row->threadsX, row->threadsY } },
					{ "calls", row->calls },
				});
			}
		};
		add(view.denoiser, cfg);
		add(view.denoiserFooter, fixedCfg);
		d["rows"] = std::move(rows);
		j["denoiser"] = std::move(d);
	}

	// ---- shadows ----
	{
		const auto& report = Util::GpuPhaseTimeline::GetSingleton()->Get();
		const auto& s = report.shadow;
		const auto stage = [&](Util::GpuPhase a_phase) -> json {
			const size_t p = static_cast<size_t>(a_phase);
			const int id = static_cast<int>(a_phase);
			float gpu = 0.0f, cpu = 0.0f;
			const bool hasGpu = view.engine.DisplayValue(id, cfg, gpu);
			const bool hasCpu = view.engineCpu.DisplayValue(id, cfg, cpu);
			return {
				{ "gpu_ms", hasGpu ? json(gpu) : json(nullptr) },
				{ "cpu_ms", hasCpu ? json(cpu) : json(nullptr) },
				{ "draws", report.draws[p] },
				{ "gpu_ms_last_frame", report.lastMs[p] },
				{ "cpu_ms_last_frame", report.lastCpuMs[p] },
				{ "draws_last_frame", report.lastDraws[p] },
			};
		};
		json sh;
		sh["measuring"] = report.hasCpuSample;
		sh["per_map_hooks"] = s.sliceHooks;
		sh["directional_light_seen"] = s.valid;
		sh["sun_cascades"] = s.sunCascades;
		sh["draw_focus_shadows"] = s.drawFocusShadows;
		sh["focus_maps"] = s.focusShadows;
		sh["local_shadow_lights"] = s.localShadowMaps;
		sh["god_ray_maps_drawn"] = s.godRayPass;
		json cascades = json::array();
		const uint32_t cascadeRows = std::clamp(s.sunCascades, 1u, 3u);
		for (uint32_t i = 0; i < cascadeRows; ++i) {
			cascades.push_back({
				{ "index", i },
				{ "name", CascadeName(s.sunCascades, i) },
				{ "start", s.startSplit[i] },
				{ "end", s.endSplit[i] },
				{ "units_per_texel", s.unitsPerTexel[i] },
				{ "viewport_lrtb", { s.port[i][0], s.port[i][1], s.port[i][2], s.port[i][3] } },
				{ "sun", stage(static_cast<Util::GpuPhase>(static_cast<int>(Util::GpuPhase::ShadowSun1) + static_cast<int>(i))) },
				{ "god_ray", stage(static_cast<Util::GpuPhase>(static_cast<int>(Util::GpuPhase::ShadowGodRay1) + static_cast<int>(i))) },
			});
		}
		sh["cascades"] = std::move(cascades);
		sh["focus"] = stage(Util::GpuPhase::ShadowFocus);
		sh["local"] = stage(Util::GpuPhase::ShadowLocalLights);
		sh["mask"] = stage(Util::GpuPhase::ShadowMask);
		sh["other"] = stage(Util::GpuPhase::ShadowOther);
		j["shadows"] = std::move(sh);
	}

	// ---- occlusion dry run ----
	j["occlusion"] = Util::OcclusionDryRun::ToJson();
	return j;
}

void PerformanceOverlay::SaveFrameJson(const PerfView::ViewConfig& a_cfg)
{
	try {
		auto dir = logger::log_directory();
		if (!dir) {
			FlashMessage("Save failed: SKSE log folder not found", true);
			return;
		}
		std::filesystem::path path = *dir / "CommunityShaders" / "Perf";
		std::filesystem::create_directories(path);

		const auto now = std::chrono::system_clock::now();
		const std::time_t t = std::chrono::system_clock::to_time_t(now);
		std::tm local{};
		localtime_s(&local, &t);
		char stamp[32];
		std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
		const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
		const std::string name = std::format("perf-{}-{:03}.json", stamp, ms);
		path /= name;

		const nlohmann::json j = BuildFrameJson(a_cfg);
		std::ofstream out(path, std::ios::binary | std::ios::trunc);
		if (!out) {
			FlashMessage("Save failed: cannot write " + name, true);
			return;
		}
		out << j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
		out.close();
		view.lastSavedFile = name;
		logger::info("[PerfSnapshot] Saved frame JSON: {}", path.string());
		FlashMessage("Saved " + name);
	} catch (const std::exception& e) {
		logger::error("[PerfSnapshot] Saving frame JSON failed: {}", e.what());
		FlashMessage(std::string("Save failed: ") + e.what(), true);
	}
}
