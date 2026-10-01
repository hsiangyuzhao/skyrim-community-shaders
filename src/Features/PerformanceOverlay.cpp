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
#include "Features/PerformanceOverlay/ABTesting/ABTesting.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"
#include "Utils/FileSystem.h"
#include "Utils/Format.h"
#include "Utils/Game.h"
#include "Utils/GpuTimers.h"
#include "Utils/UI.h"
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <magic_enum/magic_enum.hpp>
#include <map>
#include <numeric>

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
	PositionSet)

static const std::unordered_map<RE::BSShader::Type, std::string> kShaderTypeTooltips = {
	{ RE::BSShader::Type::Grass, "Draw calls using the Grass shader. Typically many, but each is usually cheap.\nWith Grass Optimizations on, each grass type is one instanced indirect draw per pass, counted once here; see its block below the GPU table for instance counts." },
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
		ImGui::Checkbox("Show VRAM Usage", &this->settings.ShowVRAM);

		bool isFrameGenerationActive = globals::features::upscaling.IsFrameGenerationActive();
		if (this->settings.ShowFPS && isFrameGenerationActive) {
			ImGui::Checkbox("Show Pre-FG Frametime Graph", &this->settings.ShowPreFGFrameTimeGraph);

			ImGui::Checkbox("Show Post-FG Frametime Graph", &this->settings.ShowPostFGFrameTimeGraph);
			if (ImGui::IsItemHovered()) {
				if (auto _tt = Util::HoverTooltipWrapper()) {
					ImGui::Text("FSR Frame Generation uses calculated timing data (2x Pre-FG).\nDLSS Frame Generation provides measured timing data.");
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
		ImGui::SliderInt("Frame History Size", &this->settings.FrameHistorySize,
			this->settings.kMinFrameHistorySize, this->settings.kMaxFrameHistorySize);

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

	// Show FPS counter if enabled
	if (this->settings.ShowFPS) {
		DrawFPS();
	}

	// Show Draw Calls if enabled
	if (this->settings.ShowDrawCalls) {
		DrawBottleneckSummary();
		DrawDrawCallsTable(rowSets.cpuRows, rowSets.summaryRows);
		DrawOurCpuPassTable(rowSets.ourCpuRows);
		DrawGpuPassTable(rowSets.gpuRows);
		if (globals::features::grassOptimizations.loaded)
			globals::features::grassOptimizations.DrawOverlayStats();
	}

	// VRAM & GPU Usage
	if (this->settings.ShowVRAM && menu->GetDXGIAdapter3()) {
		DrawVRAM();
	}

	ImGui::PopStyleVar();             // ItemSpacing
	ImGui::SetWindowFontScale(1.0f);  // Reset font scale

	// --- A/B Test Section ---
	DrawABTestSection(allRows);

	ImGui::End();
	ImGui::PopStyleVar();    // WindowBorderSize
	ImGui::PopStyleColor();  // WindowBg
}
// ============================================================================
// CORE PERFORMANCE DISPLAY FUNCTIONS
// ============================================================================

void PerformanceOverlay::DrawFPS()
{
	if (ImGui::BeginTable("FrametimeTargets", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##prop", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 6);
		ImGui::TableSetupColumn("##value");

		ImGui::TableNextColumn();
		ImGui::Text(this->state.isFrameGenerationActive ? "Raw FPS:" : "FPS:");
		ImGui::TableNextColumn();
		ImGui::Text("%.1f (%.2f ms)", this->state.smoothFps, this->state.smoothFrameTimeMs);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Instantaneous frame rate, sampled at the configured Update Interval.");
			}
		}

		// Rolling statistics over the trailing kStatsWindowSeconds of frames: average and
		// 1% Low, shown alongside the instantaneous value so short stutters stay visible
		// in the numbers even when the instant readout looks fine.
		{
			const FrameStats stats = ComputeFrameStats();

			ImGui::TableNextColumn();
			ImGui::Text("Avg (%.0fs):", Settings::kStatsWindowSeconds);
			ImGui::TableNextColumn();
			if (stats.valid) {
				ImGui::Text("%.1f (%.2f ms)", Util::CalcFPS(stats.averageMs), stats.averageMs);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Mean frame time over the last %.1f s (%d frames).\nSampled every Present, so hiding the overlay does not\nbreak the window. Frames slower than %.0f ms (loading\nscreens, alt-tab) are excluded.",
							stats.seconds, stats.frames, Settings::kStatsMaxSampleMs);
					}
				}

				ImGui::TableNextColumn();
				ImGui::Text("1%% Low:");
				ImGui::TableNextColumn();
				ImGui::Text("%.1f (%.2f ms)", Util::CalcFPS(stats.percentile99Ms), stats.percentile99Ms);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Exact definition: 99th-percentile frame time over the\nsame %.1f s / %d frame window, i.e. the (0.99 x N)-th\nslowest frame. 99%% of frames were faster than this.\nWith %d frames in the window it sits on roughly the\n%d-th slowest frame, so it reflects repeated stutter\nrather than one outlier.",
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

		if (this->state.isFrameGenerationActive) {
			ImGui::TableNextColumn();
			ImGui::Text("Post-FG FPS:");
			ImGui::TableNextColumn();
			if (this->state.postFGIsMeasured) {
				ImGui::Text("%.1f (%.2f ms)", this->state.postFGSmoothFps, this->state.postFGSmoothFrameTimeMs);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Measured: the frame-generation backend reported %.2f\npresented frames per rendered frame.", this->state.postFGMultiplier);
					}
				}
			} else {
				ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%.1f (%.2f ms) est.",
					this->state.postFGSmoothFps, this->state.postFGSmoothFrameTimeMs);
				if (ImGui::IsItemHovered()) {
					if (auto _tt = Util::HoverTooltipWrapper()) {
						ImGui::Text("Estimate, not a measurement: the backend reports no\npresentation cadence, so a fixed %.0fx multiplier is\nassumed. Raw FPS, Avg and 1%% Low above are always\nmeasured pre-frame-generation values.",
							Settings::kFrameGenerationMultiplier);
					}
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
			this->state.isFrameGenerationActive ? "Pre-FG: " : "",
			this->state.smoothFrameTimeMs, this->state.smoothFps);

		// Set graph colors
		ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));  // Green line

		// Draw the graph
		float graphWidth = ImGui::GetWindowWidth() * 0.9f;
		ImGui::PlotLines("##frametime",
			this->state.frameTimeHistory.GetData().data(),
			this->settings.FrameHistorySize,
			static_cast<int>(this->state.frameTimeHistory.GetHeadIdx()),
			overlay_text,
			this->state.smoothedMinFrameTime, this->state.smoothedMaxFrameTime,
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
	if (this->settings.ShowPostFGFrameTimeGraph && this->state.isFrameGenerationActive) {
		// State the provenance of the post-FG curve explicitly. Only DLSS-G reports a
		// presented-frame count; FSR 3 frame generation does not, so its curve is the
		// pre-FG curve scaled by a fixed multiplier and must be labelled as an estimate.
		if (this->state.postFGIsMeasured) {
			ImGui::Text("Post-FG: measured (%.2fx presented frames)", this->state.postFGMultiplier);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Presented-frame count reported by the frame-generation backend,\nsampled once per rendered frame.");
			}
		} else {
			ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Post-FG: estimated (%.0fx Pre-FG)", Settings::kFrameGenerationMultiplier);
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("The active frame-generation backend reports no presentation\ncadence, so this curve is the Pre-FG curve divided by a fixed\nmultiplier. Treat it as an estimate.");
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
	// Prepare overlay text
	char overlay_text[128];
	snprintf(overlay_text, IM_ARRAYSIZE(overlay_text),
		"Post-FG: %.2f ms (%.1f FPS)",
		state.postFGSmoothFrameTimeMs, state.postFGSmoothFps);

	// Set graph colors - blue for post-FG
	ImGui::PushStyleColor(ImGuiCol_PlotLines, ImVec4(0.0f, 0.5f, 1.0f, 1.0f));  // Blue line

	// Draw the graph
	float graphWidth = ImGui::GetWindowWidth() * 0.9f;
	ImGui::PlotLines("##postfgframetime",
		state.postFGFrameTimeHistory.GetData().data(),
		settings.FrameHistorySize,
		static_cast<int>(state.postFGFrameTimeHistory.GetHeadIdx()),
		overlay_text,
		state.smoothedMinFrameTime, state.smoothedMaxFrameTime,
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
					row.tooltip = "Frame time left after the shader types, Community Shaders' own CPU cost and the Present wait: the engine's own work (culling, animation, scripts, physics, audio, vanilla UI), driver overhead and other SKSE plugins. Not attributable from inside a plugin.";
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

	// Build sorters
	std::vector<std::function<bool(const DrawCallRow&, const DrawCallRow&, bool)>> sorters;
	for (const auto& col : columns) sorters.push_back(col.sortFunc);

	// Create non-const copies for the table function
	std::vector<DrawCallRow> mainRowsCopy = mainRows;
	std::vector<DrawCallRow> summaryRowsCopy = summaryRows;

	// Create table row handler
	auto rowHandler = overlay.CreateTableRowHandler(columns);

	// Render the table. Default sort: Frame Time descending, so the most expensive
	// buckets are at the top; the Other/Total summary rows stay pinned at the bottom.
	Util::ShowSortedStringTableCustom<DrawCallRow>(
		"DrawCallOverlayTable",
		[&columns]() { std::vector<std::string> h; for (const auto& c : columns) h.push_back(c.header); return h; }(),
		mainRowsCopy,
		2,      // Default sort column (Frame Time %)
		false,  // Default descending (most expensive first)
		sorters,
		rowHandler,
		summaryRowsCopy);

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
	if (gpuRows.empty())
		return;

	auto& overlay = globals::features::performanceOverlay;
	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	ImGui::Spacing();
	ImGui::TextUnformatted("GPU Passes (timestamp queries)");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"GPU time for Community Shaders' own passes, measured with D3D11 timestamp\n"
				"queries and smoothed. Separate from the table above:\n"
				"  - GPU clock, not the CPU QueryPerformanceCounter clock\n"
				"  - read back a few frames late, so give it a second to settle\n"
				"  - passes overlap each other and the CPU, so these do NOT sum to frame time\n"
				"  - never subtracted from the CPU table above\n"
				"A row disappears about a second after its feature stops running, so the table\n"
				"shows exactly what the current settings are actually executing. A feature that\n"
				"is switched off has no row here rather than a row of zeros, and so does one\n"
				"whose queries never came back. \"< 0.01 ms\" means measured but below the GPU\n"
				"timer's resolution - real, just too small to put a number on.\n"
				"\n"
				"The four summary rows at the bottom DO add up, and they account for the whole\n"
				"GPU frame:\n"
				"  Measured GPU + Untracked GPU + Gap = GPU frame (elapsed)\n"
				"They come from a second, frame-spanning timestamp pair around Present. Hover\n"
				"each one: \"elapsed\" is not \"busy\", and the gap row is not pure idle.");
		}
	}

	bool anyTestData = !overlay.testData.empty();
	auto legends = overlay.BuildDrawCallLegends(theme, anyTestData);
	auto columns = overlay.BuildPassTableColumns(theme, legends, anyTestData, "GPU Time (%)",
		"Intervals: how many separate timestamp intervals this frame's sample is the sum of.\n"
		"A bucket can measure several disjoint stretches of one frame - Volumetric Lighting\n"
		"has four (generate, raymarch, both blurs). This replaces Draw Calls and\n"
		"Cost/Call, which are not defined for a compute pass: a GPU bucket has no draw calls,\n"
		"so Cost/Call was a hard zero on every row.");

	std::vector<std::function<bool(const DrawCallRow&, const DrawCallRow&, bool)>> sorters;
	for (const auto& col : columns)
		sorters.push_back(col.sortFunc);

	std::vector<DrawCallRow> gpuRowsCopy = gpuRows;

	// Sum of the buckets. Not a residual and not a share of frame time; just "how much
	// GPU time the instrumented passes accounted for".
	float bucketSum = 0.0f;
	int intervalSum = 0;
	for (const auto& row : gpuRows) {
		bucketSum += row.frameTime;
		if (row.drawCalls != kDrawCallsNotApplicable)
			intervalSum += row.drawCalls;
	}
	const float smoothedFrameTime = globals::state->GetAttributionFrameTimeMs();

	std::vector<DrawCallRow> gpuSummaryRows;
	gpuSummaryRows.push_back(DrawCallRow{
		"Measured GPU:", kGpuTotalRowId, intervalSum, bucketSum,
		Util::CalculatePercentage(bucketSum, smoothedFrameTime), 0.0f,
		std::string("Sum of the GPU buckets above: Community Shaders' own passes and nothing "
					"else, so a LOWER BOUND on how busy the GPU actually was.\n\n"
					"What is missing from it now has its own row: see \"Untracked GPU\" below "
					"for the engine's own rendering plus DLSS super resolution, and \"Gap\" for "
					"the Present window where DLSS-G frame generation runs."),
		true, std::nullopt, std::nullopt });

	// Whole-frame GPU timeline (batch 14). The bucket rows above only ever cover our own
	// passes, which left the engine's own rendering, DLSS super resolution and DLSS-G frame
	// generation with no row anywhere - the reason "GPU (ours)" could only ever be a
	// ">=". Util::GpuFrameTimer supplies the frame-spanning pair that closes the gap, and
	// the three rows below are additive: untracked + gap + measured == frame (elapsed).
	const auto frameGpu = Util::GpuFrameTimer::GetSingleton()->Get();
	if (frameGpu.hasSample) {
		// Clamped at zero on purpose. The bucket sum and the frame span are collected from
		// different frames and smoothed independently, so on a settings change the
		// difference can dip slightly negative for a few frames; a negative millisecond
		// count would read as a broken measurement rather than as transient skew.
		const float untrackedMs = std::max(0.0f, frameGpu.workSpanMs - bucketSum);

		gpuSummaryRows.push_back(DrawCallRow{
			"Untracked GPU (engine + DLSS):", kGpuUntrackedRowId, kDrawCallsNotApplicable, untrackedMs,
			Util::CalculatePercentage(untrackedMs, smoothedFrameTime), 0.0f,
			std::string("GPU work in this frame that is NOT one of our passes. This is the row that "
						"used to be missing entirely.\n\n"
						"Computed as the GPU-timeline stretch between the end of the previous Present "
						"and the start of this one, minus \"Measured GPU\" above.\n\n"
						"What lives here: the game engine's own rendering (shadow maps, the opaque and "
						"alpha passes, water, the vanilla post chain), DLSS/FSR super resolution, and "
						"any gaps inside the frame where the GPU had nothing submitted to it. DLSS-G "
						"frame generation is NOT here - it runs while Present is executing and lands "
						"in the gap row below.\n\n"
						"Large here and small in \"Measured GPU\" means Community Shaders is not what "
						"is costing you the frame. The reverse means it is."),
			true, std::nullopt, std::nullopt });

		gpuSummaryRows.push_back(DrawCallRow{
			"Gap: idle / flip / frame-gen:", kGpuGapRowId, kDrawCallsNotApplicable, frameGpu.presentSpanMs,
			Util::CalculatePercentage(frameGpu.presentSpanMs, smoothedFrameTime), 0.0f,
			std::string("Elapsed GPU clock across the Present call itself, i.e. the part of the frame "
						"in which nothing of the frame's own rendering is running.\n\n"
						"It is a MIXTURE, not pure idle: the flip, vsync or frame-limiter pacing, "
						"DLSS-G frame generation (which executes on its own queue during Present and "
						"is therefore invisible to our timestamps), and genuine GPU idle while the CPU "
						"is blocked. Treat it as an UPPER BOUND on how idle the GPU was.\n\n"
						"With frame generation on, a large number here is expected and does not mean "
						"there is headroom: half of it is the generated frame being produced."),
			true, std::nullopt, std::nullopt });

		gpuSummaryRows.push_back(DrawCallRow{
			"GPU frame (elapsed):", kGpuFrameElapsedRowId, kDrawCallsNotApplicable, frameGpu.frameElapsedMs,
			Util::CalculatePercentage(frameGpu.frameElapsedMs, smoothedFrameTime), 0.0f,
			std::string("Present-to-present distance measured on the GPU's own clock.\n\n"
						"ELAPSED, NOT BUSY. A GPU timestamp reads a clock that keeps ticking while the "
						"GPU has nothing to do, so this number includes every idle microsecond and in "
						"steady state simply tracks wall-clock frame time. It is here as the "
						"denominator for the two rows above and as a sanity check: if it does not "
						"match \"Total\" in the table further up, the two clocks disagree and none of "
						"the GPU numbers should be trusted.\n\n"
						"There is no way to measure whole-frame GPU BUSY time from inside a D3D11 "
						"plugin. What can be done is what the rows above do: bound it from below with "
						"our own passes, size the rest of the frame's rendering, and bound the idle "
						"from above."),
			true, std::nullopt, std::nullopt });
	}

	// Plain handler: pass rows have no toggle and no summary-row special cases.
	std::function<void(int, int, const DrawCallRow&)> rowHandler =
		[&columns](int, int colIdx, const DrawCallRow& row) { columns[colIdx].cellRender(row, colIdx); };

	Util::ShowSortedStringTableCustom<DrawCallRow>(
		"GpuPassOverlayTable",
		[&columns]() { std::vector<std::string> h; for (const auto& c : columns) h.push_back(c.header); return h; }(),
		gpuRowsCopy,
		2,      // Default sort column (GPU Time %)
		false,  // Default descending (most expensive first)
		sorters,
		rowHandler,
		gpuSummaryRows);
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
	if (ourCpuRows.empty())
		return;

	auto& overlay = globals::features::performanceOverlay;
	auto* menu = Menu::GetSingleton();
	const auto& theme = menu->GetTheme();

	ImGui::Spacing();
	ImGui::TextUnformatted("Community Shaders (CPU submit)");
	if (ImGui::IsItemHovered()) {
		if (auto _tt = Util::HoverTooltipWrapper()) {
			ImGui::TextUnformatted(
				"What Community Shaders costs the CPU: building constant buffers, binding\n"
				"resources and issuing dispatches. Wall clock, same smoothing as the table above.\n"
				"  - these rows sum to the \"CS features (CPU)\" line above, exactly\n"
				"  - they are NOT inside the shader-type rows; that time is subtracted there\n"
				"  - so the table above still adds up to the frame time\n"
				"Same feature names as the GPU table, so submit cost and execution cost can be\n"
				"read side by side. A feature with no measurable cost gets no row.\n\n"
				"Coverage: everything reachable from the deferred renderer's own orchestration.\n"
				"Passes driven straight from engine hooks (the post-processing draw legs, the\n"
				"upscale itself) are not in here; their GPU cost still is.");
		}
	}

	bool anyTestData = !overlay.testData.empty();
	auto legends = overlay.BuildDrawCallLegends(theme, anyTestData);
	auto columns = overlay.BuildPassTableColumns(theme, legends, false, "CPU Time (%)",
		"Calls: how many times this bucket was entered in the last frame. A feature with\n"
		"several instrumented entry points (a prepass plus a deferred pass) counts more\n"
		"than one. Draw Calls and Cost/Call are not shown: these are compute submissions,\n"
		"not draws, so both would be meaningless.");

	std::vector<std::function<bool(const DrawCallRow&, const DrawCallRow&, bool)>> sorters;
	for (const auto& col : columns)
		sorters.push_back(col.sortFunc);

	std::vector<DrawCallRow> rowsCopy = ourCpuRows;

	std::function<void(int, int, const DrawCallRow&)> rowHandler =
		[&columns](int, int colIdx, const DrawCallRow& row) { columns[colIdx].cellRender(row, colIdx); };

	Util::ShowSortedStringTableCustom<DrawCallRow>(
		"OurCpuPassOverlayTable",
		[&columns]() { std::vector<std::string> h; for (const auto& c : columns) h.push_back(c.header); return h; }(),
		rowsCopy,
		2,      // Default sort column (CPU Time %)
		false,  // Default descending (most expensive first)
		sorters,
		rowHandler);
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

	auto* cpuTimers = Util::CpuPassTimers::GetSingleton();
	const float frameMs = globals::state->GetAttributionFrameTimeMs();
	const float waitMs = cpuTimers->GetPresentWaitMs();
	const float busyMs = std::max(0.0f, frameMs - waitMs);

	float gpuMeasuredMs = 0.0f;
	Util::GpuPassTimers::GetSingleton()->ForEachActiveBucket(
		[&gpuMeasuredMs](const Util::GpuPassTimers::BucketReport& report) { gpuMeasuredMs += report.smoothedMs; });

	const float waitShare = (frameMs > 0.0f) ? (waitMs / frameMs) : 0.0f;

	// Thresholds are deliberately coarse and the middle band is named rather than forced
	// into one of the two answers.
	const char* verdict = "collecting...";
	ImVec4 verdictColor = theme.StatusPalette.Disable;
	if (frameMs > 0.0f) {
		if (waitShare >= 0.25f) {
			verdict = "GPU-bound (or frame-limited)";
			verdictColor = theme.StatusPalette.Warning;
		} else if (waitShare <= 0.10f) {
			verdict = "CPU-bound";
			verdictColor = theme.StatusPalette.Error;
		} else {
			verdict = "balanced";
			verdictColor = theme.StatusPalette.SuccessColor;
		}
	}

	if (ImGui::BeginTable("BottleneckSummary", 2, ImGuiTableFlags_SizingStretchProp)) {
		ImGui::TableSetupColumn("##prop", ImGuiTableColumnFlags_WidthFixed, ImGui::GetTextLineHeight() * 6);
		ImGui::TableSetupColumn("##value");

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Bottleneck:");
		ImGui::TableNextColumn();
		ImGui::TextColored(verdictColor, "%s", verdict);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"How this is decided: the share of the frame the CPU spends blocked inside\n"
					"Present. Over 25% means the CPU is waiting on something downstream of it;\n"
					"under 10% means the CPU never gets to idle.\n"
					"\n"
					"IMPORTANT: this is a CPU test, not a GPU test. A big Present wait does NOT\n"
					"mean the GPU is busy. Vsync, a frame-rate cap, and Streamline pacing a\n"
					"frame-generated presentation queue all park the CPU in Present with the GPU\n"
					"mostly idle. \"GPU-bound (or frame-limited)\" really does mean \"or\".\n"
					"\n"
					"To find out where the GPU time goes, read the GPU Passes table instead:\n"
					"\"Untracked GPU\" is the engine's own rendering plus DLSS super resolution,\n"
					"and \"Gap\" is the Present window (flip, pacing, frame generation, idle).\n"
					"\n"
					"Error sources, honestly:\n"
					"  - a Present wait can be the GPU, vsync, or a frame-rate limiter. Uncap the\n"
					"    frame rate and switch frame generation off to tell them apart.\n"
					"  - \"GPU (ours)\" is only OUR passes, so it is a LOWER BOUND on GPU busy\n"
					"    time. The engine's own draws are not instrumented.\n"
					"  - whole-frame GPU BUSY time still cannot be measured from inside a D3D11\n"
					"    plugin. The frame-spanning timestamps are elapsed GPU clock, which keeps\n"
					"    running while the GPU is idle; they are only useful because they are\n"
					"    split at Present, which separates rendering from flip and frame-gen.\n"
					"  - GPU numbers are read back a few frames late and passes overlap, so they\n"
					"    do not line up frame-for-frame with the CPU numbers.\n"
					"  - everything here is smoothed over roughly 20 frames.");
			}
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("CPU busy:");
		ImGui::TableNextColumn();
		ImGui::Text("%.2f ms of %.2f ms (%.0f%%)", busyMs, frameMs,
			(frameMs > 0.0f) ? (busyMs / frameMs * 100.0f) : 0.0f);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted("Frame time minus the time blocked in Present. Wall clock, smoothed.");
			}
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("Present wait:");
		ImGui::TableNextColumn();
		ImGui::Text("%.2f ms (%.0f%%)", waitMs, waitShare * 100.0f);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"Measured with QueryPerformanceCounter around the real Present call: CPU time,\n"
					"not GPU time.\n"
					"\n"
					"A GPU wait, a vsync wait, a frame-limiter wait and Streamline's frame-\n"
					"generation pacing all land here and cannot be told apart. So this being large\n"
					"does NOT mean the GPU was busy - and it does not mean the GPU was idle either.\n"
					"For that, read \"Untracked GPU\" and \"Gap\" in the GPU Passes table.");
			}
		}

		ImGui::TableNextColumn();
		ImGui::TextUnformatted("GPU (ours):");
		ImGui::TableNextColumn();
		ImGui::Text("%.2f ms", gpuMeasuredMs);
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"Sum of the GPU pass buckets: Community Shaders' own passes and nothing else.\n"
					"An exact figure for OUR cost, and a lower bound on total GPU busy time.\n"
					"\n"
					"It used to be shown as \">= x ms\" because everything else on the GPU was\n"
					"unmeasured. The line below now measures that remainder, so this one no longer\n"
					"has to stand in for the whole GPU.\n"
					"\n"
					"Not comparable 1:1 with the CPU numbers - different clock, read back a few\n"
					"frames late, and the passes overlap each other.");
			}
		}

		// The batch 14 addition: the engine's own rendering plus DLSS super resolution,
		// which is what the 65% of unattributed frame time mostly was.
		const auto frameGpu = Util::GpuFrameTimer::GetSingleton()->Get();
		ImGui::TableNextColumn();
		ImGui::TextUnformatted("GPU (other):");
		ImGui::TableNextColumn();
		if (frameGpu.hasSample) {
			const float untrackedMs = std::max(0.0f, frameGpu.workSpanMs - gpuMeasuredMs);
			ImGui::Text("%.2f ms rendering + %.2f ms gap", untrackedMs, frameGpu.presentSpanMs);
		} else {
			ImGui::TextUnformatted("collecting...");
		}
		if (ImGui::IsItemHovered()) {
			if (auto _tt = Util::HoverTooltipWrapper()) {
				ImGui::TextUnformatted(
					"From the frame-spanning timestamp pair around Present, split into the two\n"
					"halves of the frame:\n"
					"\n"
					"  rendering - GPU time in the frame that is not one of our passes: the\n"
					"              engine's own draws and DLSS/FSR super resolution.\n"
					"  gap       - GPU time while Present is executing: the flip, vsync or\n"
					"              frame-limiter pacing, DLSS-G frame generation on its own\n"
					"              queue, and genuine idle. A mixture, so an upper bound on\n"
					"              idle rather than a measurement of it.\n"
					"\n"
					"\"GPU (ours)\" + rendering + gap is the whole GPU frame. The GPU Passes table\n"
					"shows the same three numbers as rows, with the total.");
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
					ImGui::TextUnformatted("The instrumented pass group. Hover a name for what it covers.");
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
		MakeMetricColumn(theme, [](const DrawCallRow& row) { return row.frameTime; }, [](const auto& theme, float value, const DrawCallRow&) { return Util::GetThresholdColor(value, PerformanceOverlay::Settings::kFrameTimeGoodThreshold, PerformanceOverlay::Settings::kFrameTimeWarningThreshold, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error); }, [](float /*value*/, const DrawCallRow& row) {
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
		MakeMetricColumn(theme, [](const DrawCallRow& row) { return row.frameTime; }, [](const auto& theme, float value, const DrawCallRow&) { return Util::GetThresholdColor(value, PerformanceOverlay::Settings::kFrameTimeGoodThreshold, PerformanceOverlay::Settings::kFrameTimeWarningThreshold, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error); }, [](float /*value*/, const DrawCallRow& row) { return Util::FormatMilliseconds(row.frameTime) + " (" + Util::FormatPercent(row.percent) + ")"; }, legends.frameTime.tooltip), [](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.percent < b.percent) : (a.percent > b.percent); }, [legends]() {
			 if (ImGui::IsItemHovered()) {
				 if (auto _tt = Util::HoverTooltipWrapper()) {
					 Util::DrawColoredMultiLineTooltip(legends.frameTime.tooltip);
				 }
			 } } });

	columns.push_back(ColumnConfig{
		legends.costPerCall.header,
		MakeMetricColumn(theme, [](const DrawCallRow& row) { return row.costPerCall; }, [](const auto& theme, float value, const DrawCallRow&) { return Util::GetThresholdColor(value, PerformanceOverlay::Settings::kCostPerCallGoodThreshold, PerformanceOverlay::Settings::kCostPerCallWarningThreshold, theme.StatusPalette.SuccessColor, theme.StatusPalette.Warning, theme.StatusPalette.Error); }, [](float value, const DrawCallRow&) { return (value < PerformanceOverlay::Settings::kMicrosecondThreshold && value > 0.0f) ? Util::FormatMicroseconds(value * 1000.0f) : Util::FormatMilliseconds(value); }, legends.costPerCall.tooltip), [](const DrawCallRow& a, const DrawCallRow& b, bool asc) { return asc ? (a.costPerCall < b.costPerCall) : (a.costPerCall > b.costPerCall); }, [legends]() {
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
		std::string("CPU time Community Shaders itself spends preparing and submitting work: "
					"constant buffers, resource binds and dispatch calls.\n\n"
					"Broken down per feature in the \"Community Shaders (CPU submit)\" table below, "
					"which sums exactly to this line. It is NOT part of the shader-type rows above - "
					"those intervals have this time removed - so the whole table adds up."),
		true, std::nullopt, std::nullopt
	};

	DrawCallRow presentWaitRow = {
		// Deliberately no longer called "Present / GPU wait": the label itself was the source
		// of the "the GPU was idle 70% of the frame" misreading. It is CPU blocked time.
		"Present wait (CPU blocked):", magic_enum::enum_integer(SpecialShaderType::PresentWait), kDrawCallsNotApplicable,
		presentWaitMs, Util::CalculatePercentage(presentWaitMs, smoothedFrameTime), 0.0f,
		std::string("CPU time blocked inside Present, measured around the real Present call. This "
					"is a CPU measurement. It says nothing about whether the GPU was busy.\n\n"
					"A large number here means only one thing: the CPU had nothing left to do. The "
					"wait can be the GPU finishing, vsync, a frame-rate limiter, or - on D3D11 with "
					"Streamline in the chain - the proxy Present pacing out a frame-generated "
					"presentation queue. With DLSS-G on, most of this row is usually pacing, and "
					"reading it as \"the GPU was busy 70% of the frame\" is the specific mistake it "
					"invites.\n\n"
					"For where the GPU time actually went, read the GPU Passes table: "
					"\"Untracked GPU\" is the engine's own rendering plus super resolution, "
					"\"Gap\" is the Present window itself (flip, pacing, frame generation, idle), "
					"and \"GPU frame (elapsed)\" is the total they add up to.\n\n"
					"To separate a real GPU wait from pacing: uncap the frame rate and switch frame "
					"generation off, then watch whether this row stays large."),
		true, std::nullopt, std::nullopt
	};

	DrawCallRow otherRow = {
		"Engine (untracked):", magic_enum::enum_integer(SpecialShaderType::Other), kDrawCallsNotApplicable, otherFrameTime, otherPercent,
		0.0f,
		std::string("What is left of the frame after the shader-type rows, Community Shaders' own "
					"CPU cost and the Present wait. It is not a Community Shaders cost and we "
					"cannot break it down from inside a plugin.\n\n"
					"What lives here: the engine's own per-frame work - visibility culling, "
					"animation and skinning, Papyrus scripts, physics, navmesh and AI, audio, the "
					"vanilla UI - plus D3D11 driver overhead and any other SKSE plugins in the "
					"load order.\n\n"
					"Why we cannot split it: attribution comes from hooking Community Shaders' own "
					"call sites and the engine's draw submissions. Everything in this row happens "
					"between draw submissions in engine code we do not hook, so there is no marker "
					"to charge it against. Splitting it needs an external profiler.\n\n"
					"A big number here is normal in Skyrim and usually means the game is CPU-bound "
					"on engine work, not on rendering."),
		true, otherTestFrameTime, otherTestCostPerCall
	};
	// Always use the actual total frame time for live data
	float totalFrameTime = smoothedFrameTime;
	float totalPercent = 100.0f;  // Total is always 100% of total

	DrawCallRow totalRow = {
		"Total:", magic_enum::enum_integer(SpecialShaderType::Total), static_cast<int>(globals::state->GetTotalSmoothedDrawCalls()), totalFrameTime, totalPercent,
		totalCostPerCall,
		std::string("Wall-clock frame time, smoothed the same way as every row above, so the rows "
					"add up to it. The FPS readout at the top of the panel is an instantaneous "
					"sample re-taken every Update Interval, so it can differ by a millisecond or "
					"two - that is the difference between the two estimators, not an error."),
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
			// No cadence reported: fall back to the fixed estimate. The UI labels every
			// number derived from this as an estimate.
			state.postFGIsMeasured = false;
			state.postFGMultiplier = Settings::kFrameGenerationMultiplier;
			state.postFGFrameTimeMs = state.frameTimeMs / Settings::kFrameGenerationMultiplier;
			state.postFGFps = state.fps * Settings::kFrameGenerationMultiplier;
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