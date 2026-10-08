#pragma once

#include "Menu.h"
#include "OverlayFeature.h"
#include "PerformanceOverlay/ABTesting/ABTestAggregator.h"
#include "PerformanceOverlay/StableTable.h"
#include "Utils/PerfUtils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <optional>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

// Forward declarations
struct DrawCallRow;

// Special shader type enum for summary rows
enum class SpecialShaderType
{
	Total = -1,
	Other = -2,  // the residual; labelled "Engine (untracked)" in the UI
	// Aggregate of Community Shaders' own CPU submit cost. Broken down per feature in its
	// own table; here it is one line so the CPU frame table stays additive.
	OurCpu = -3,
	// CPU time blocked inside Present (GPU wait / vsync / frame limiter).
	PresentWait = -4
};

// Constants for special draw call values
static constexpr int kDrawCallsNotApplicable = -1;  // Special value to indicate draw calls are not applicable

struct DrawCallRow
{
	std::string label;
	int shaderType;  // Use int for consistency with the rest of the codebase
	int drawCalls;
	float frameTime;
	float percent;
	float costPerCall;
	std::string tooltip;
	bool enabled;
	std::optional<float> testFrameTime;
	std::optional<float> testCostPerCall;
	// (batch 36e) Display-layer fields, filled from PerfView::StableTable. hasData is false for
	// a row that is kept in place but has no reading in the smoothing window ("-").
	std::optional<float> peak = std::nullopt;
	bool hasData = true;
};

/// (batch 36e) One row of the "Denoiser breakdown" table.
struct DenoiserRow
{
	std::string label;
	std::string group;
	std::string tooltip;
	uint32_t groupsX = 0;
	uint32_t groupsY = 0;
	uint32_t threadsX = 0;
	uint32_t threadsY = 0;
	int calls = 0;
	bool isFooter = false;
};

struct ShaderRow
{
	std::string label;
	int type;
	std::string tooltip;
};

// Legend and configuration structures
struct ColumnLegend
{
	std::string header;
	Util::ColoredTextLines tooltip;
};

struct ABTestLegends
{
	ColumnLegend shaderType;
	ColumnLegend aAvg;
	ColumnLegend bAvg;
	ColumnLegend delta;
	ColumnLegend aMedian;
	ColumnLegend bMedian;
	ColumnLegend medianDelta;
};

struct DrawCallLegends
{
	ColumnLegend shaderType;
	ColumnLegend drawCalls;
	ColumnLegend frameTime;
	ColumnLegend costPerCall;
	ColumnLegend testFrameTime;
	ColumnLegend testCostPerCall;
};

struct ColumnConfig
{
	std::string header;
	std::function<void(const DrawCallRow&, int colIdx)> cellRender;
	std::function<bool(const DrawCallRow&, const DrawCallRow&, bool)> sortFunc;
	std::function<void()> headerTooltip;
};

/**
 * @brief Fixed-capacity ring buffer of samples in write order.
 *
 * `GetHeadIdx()` is the next write position, which is also the index of the oldest
 * sample once the buffer has wrapped (the convention ImGui::PlotLines expects as its
 * `values_offset`).
 */
template <typename T>
class CircularBuffer
{
	std::vector<T> data = {};
	size_t headIdx = 0;     // next write position == oldest sample once wrapped
	size_t validCount = 0;  // samples actually written, capped at capacity

public:
	explicit CircularBuffer(size_t size)
	{
		data.resize(std::max<size_t>(1, size));
	}
	CircularBuffer() :
		CircularBuffer(1) {}

	/**
	 * @brief Changes capacity while keeping the newest samples in chronological order.
	 *
	 * Resizing the underlying vector directly would splice the buffer at the wrap
	 * point: samples would silently change position in time and stale slots would be
	 * indistinguishable from real ones. Rebuild instead, oldest kept sample first.
	 */
	void Resize(size_t newSize)
	{
		newSize = std::max<size_t>(1, newSize);
		if (data.size() == newSize)
			return;

		const size_t keep = std::min(validCount, newSize);
		std::vector<T> rebuilt(newSize, T{});
		for (size_t i = 0; i < keep; ++i)
			rebuilt[i] = data[(headIdx + data.size() - keep + i) % data.size()];

		data = std::move(rebuilt);
		validCount = keep;
		headIdx = (keep >= newSize) ? 0 : keep;
	}

	/// @brief Drops every sample. Used when a settings change makes old samples meaningless.
	void Clear()
	{
		std::fill(data.begin(), data.end(), T{});
		headIdx = 0;
		validCount = 0;
	}

	void Push(const T& val)
	{
		data[headIdx++] = val;
		if (headIdx >= data.size())
			headIdx = 0;
		if (validCount < data.size())
			++validCount;
	}

	std::span<const T> GetData() const { return { data }; }
	size_t GetHeadIdx() const { return headIdx; }

	/// @brief Number of real samples written so far (never counts never-written slots).
	size_t GetValidCount() const { return validCount; }

	/**
	 * @brief Reads the i-th newest sample (0 == newest).
	 * @pre a_age < GetValidCount()
	 */
	const T& GetNewest(size_t a_age) const
	{
		return data[(headIdx + data.size() - 1 - a_age) % data.size()];
	}
};

struct PerformanceOverlay : OverlayFeature
{
	// ============================================================================
	// VIRTUAL OVERRIDES (Feature.h interface)
	// ============================================================================
	std::string GetName() override { return "Performance Overlay"; }
	std::string GetShortName() override { return "PerformanceOverlay"; }
	virtual bool SupportsVR() override { return true; }
	virtual bool IsCore() const override { return true; }
	virtual bool IsInMenu() const override { return true; }
	bool IsOverlayVisible() const override { return settings.ShowInOverlay; }
	virtual std::pair<std::string, std::vector<std::string>> GetFeatureSummary() override;
	virtual void DrawSettings() override;
	virtual void DataLoaded() override;
	/// @brief (batch 36) Installs the engine hooks behind the "Engine passes (GPU)" table.
	virtual void PostPostLoad() override;
	void DrawOverlay() override;
	// Settings persistence and defaults
	void SaveSettings(json& j) override;
	void LoadSettings(json& j) override;
	void RestoreDefaultSettings() override;

	// ============================================================================
	// CORE PERFORMANCE DISPLAY FUNCTIONS
	// ============================================================================
	/**
	* @brief Updates all runtime state related to the performance overlay graph.
	*
	* This function synchronizes the frame time history buffer, tracks min/max frame times,
	* and computes the normalized Y-axis range for the frame time graph using statistical analysis.
	*
	* Steps performed:
	*   1. Resizes the frameTimeHistory buffer if the user has changed the setting.
	*   2. Inserts the latest frame time into the circular history buffer.
	*   3. Updates instantaneous min/max frame time values, with full rescans if necessary.
	*   4. Calculates the average (mean) and standard deviation of frame times in the buffer.
	*   5. Sets the graph Y-axis range to be centered on the average, with a spread of ±2 standard deviations,
	*      clamped to user-friendly minimum and maximum values.
	*   6. Smooths the min/max Y-axis values for visual stability using exponential smoothing.
	*
	*
	* No parameters; uses settings from the singleton.
	*/
	void UpdateGraphValues();

	/**
	 * @brief Advances the frame clock and samples one frame. Must run on every Present.
	 *
	 * Sampling used to happen inside DrawOverlay(), so the clock froze whenever the
	 * overlay was hidden and the first delta after unhiding covered the entire hidden
	 * period - a single bogus multi-second sample inside the statistics window. Driving
	 * it from Present keeps the window continuous and honest regardless of visibility.
	 */
	void AdvanceFrameClock();

	/**
	 * @brief Rolling statistics over the trailing kStatsWindowSeconds of frames.
	 *
	 * The window is defined in time, not in frames, so its meaning does not change with
	 * frame rate. It is measured by walking the ring buffer backwards and accumulating
	 * frame times until the requested duration is covered.
	 */
	struct FrameStats
	{
		bool valid = false;
		int frames = 0;              ///< frames the window actually covered
		float seconds = 0.0f;        ///< wall time the window actually covered
		float averageMs = 0.0f;      ///< mean frame time over the window
		float percentile99Ms = 0.0f;  ///< 99th-percentile frame time ("1% Low")
	};
	FrameStats ComputeFrameStats() const;

	void DrawFPS();
	void DrawVRAM();
	void DrawPostFGFrameTimeGraph();

	// ============================================================================
	// A/B TESTING FUNCTIONS
	// ============================================================================
	void DrawABTestSection(const std::vector<DrawCallRow>& allRows);
	void DrawABTestResultsTable();
	void DrawABTestStatisticalValidity(const Menu::ThemeSettings& theme, const ABTestAggregator& aggregator) const;
	void ConvertABTestResultsToRows(const std::vector<AggregatedDrawCallStats>& results, std::vector<DrawCallRow>& mainRows, std::vector<DrawCallRow>& summaryRows) const;
	ABTestLegends BuildABTestLegends(const Menu::ThemeSettings& theme) const;
	std::vector<ColumnConfig> BuildABTestResultsTableColumns(const Menu::ThemeSettings& theme, const ABTestLegends& legends) const;
	static ABTestAggregator& GetABTestAggregator();

	// ============================================================================
	// TABLE BUILDING AND RENDERING FUNCTIONS
	// ============================================================================
	/**
	 * @brief Row sets for the overlay tables, kept apart because they are not the same
	 *        kind of measurement.
	 *
	 * `cpuRows` + `summaryRows` are CPU QueryPerformanceCounter attribution per shader
	 * type, where "Other" is a meaningful residual of the CPU frame time.
	 * `gpuRows` are D3D11 timestamp intervals around our own passes. They live in a
	 * different clock domain, are read back several frames late, and overlap each other
	 * and the CPU timeline, so they must never be subtracted from the CPU residual -
	 * doing that produced negative "Other" values.
	 */
	struct DrawCallRowSets
	{
		std::vector<DrawCallRow> cpuRows;
		std::vector<DrawCallRow> summaryRows;
		std::vector<DrawCallRow> gpuRows;
		/// Community Shaders' own CPU submit cost, per feature. Sums exactly to the
		/// "CS features (CPU)" summary row, and is disjoint from the shader-type rows
		/// because State::Debug() removes it from the intervals it charges.
		std::vector<DrawCallRow> ourCpuRows;
	};

	void DrawDrawCallsTable(const std::vector<DrawCallRow>& mainRows, const std::vector<DrawCallRow>& summaryRows);
	void DrawGpuPassTable(const std::vector<DrawCallRow>& gpuRows);
	void DrawOurCpuPassTable(const std::vector<DrawCallRow>& ourCpuRows);
	/// @brief (batch 36) The whole GPU frame split into engine stages (Util::GpuPhaseTimeline).
	void DrawEngineGpuTable();
	/// @brief CPU / GPU bottleneck readout drawn above the tables.
	void DrawBottleneckSummary();

	// ============================================================================
	// (batch 36e) READABILITY LAYER
	// ============================================================================
	// Everything below only changes what is printed and in which row. The timers, their hook
	// points and their own smoothing are untouched; see PerfView::StableTable.

	/// @brief The display config the tables use, from the settings.
	PerfView::ViewConfig MakeViewConfig() const;
	/// @brief Feeds this frame's readings into every table's display state (skipped while frozen).
	void UpdateViews(double a_now, const DrawCallRowSets& a_rowSets, const PerfView::ViewConfig& a_cfg);
	/// @brief Always-visible two-line summary: FPS, frame time, our GPU total, bottleneck.
	void DrawCompactSummary(const PerfView::ViewConfig& a_cfg);
	/// @brief Sort / smoothing / Top N / Peak / Freeze controls. Shared by the menu page and the overlay.
	void DrawViewOptions();
	/// @brief (batch 36e) Per-dispatch timing of the SSRT denoiser chain (Util::DenoiserTimers).
	void DrawDenoiserTable(const PerfView::ViewConfig& a_cfg);
	/// @brief (batch 37a) Sun cascade setup (count, distances, texel size), character and lamp shadow maps.
	void DrawShadowInfo();
	/// @brief A collapsible section whose open state lives in the settings (so it is saved).
	static bool Section(const char* a_label, bool& a_open, const char* a_tooltip = nullptr);
	/// @brief Freeze / unfreeze every number on the overlay. Freezing also writes a snapshot to the log.
	void ToggleFreeze();
	bool IsFrozen() const { return view.frozen; }
	/// @brief Writes every table, as currently displayed, to CommunityShaders.log.
	void WriteSnapshotToLog(const PerfView::ViewConfig& a_cfg);
	/// @brief (batch 37a) Everything the overlay shows for this (or the frozen) frame, as one JSON document.
	nlohmann::json BuildFrameJson(const PerfView::ViewConfig& a_cfg);
	/// @brief (batch 37a) Writes BuildFrameJson to SKSE/CommunityShaders/Perf/perf-<time>.json; sets the panel message.
	void SaveFrameJson(const PerfView::ViewConfig& a_cfg);
	/// @brief (batch 37a) Moves the Freeze key off a key another Community Shaders hotkey already owns.
	void ResolveFreezeKeyConflict();
	/// @brief (batch 37a) Shows a short message at the top of the overlay for a few seconds.
	void FlashMessage(std::string a_text, bool a_error = false);
	/// @brief One line of smoothed denoiser timings, for the periodic log option.
	std::string FormatDenoiserLogLine(const PerfView::ViewConfig& a_cfg) const;
	/// @brief Rows of a DrawCallRow table, in display order, with the display values filled in.
	static std::vector<DrawCallRow> MaterializeRows(const PerfView::StableTable<DrawCallRow>& a_table, const std::vector<int>& a_order,
		const PerfView::ViewConfig& a_cfg, float a_percentBase, bool a_recomputeCostPerCall);
	/// @brief Renders rows in the given order (no header-click sorting: order comes from the view layer).
	static void DrawStableTable(const char* a_id, const std::vector<ColumnConfig>& a_columns, const std::vector<DrawCallRow>& a_rows,
		const std::vector<DrawCallRow>& a_footer, const std::function<void(int, int, const DrawCallRow&)>& a_cellRender);

	/// Set while the menu is waiting for the next key press to become the Freeze key.
	bool capturingFreezeKey = false;
	DrawCallLegends BuildDrawCallLegends(const Menu::ThemeSettings& theme, bool anyTestData) const;
	std::vector<ColumnConfig> BuildDrawCallTableColumns(const Menu::ThemeSettings& theme, const DrawCallLegends& legends, bool anyTestData);

	/**
	 * @brief Columns for the pass tables (GPU buckets, and our own CPU submit buckets).
	 *
	 * Deliberately NOT the draw-call column set. A GPU bucket has no draw calls, so the
	 * old shared columns showed "-" for Draw Calls and a hard 0 ms for Cost/Call
	 * (cost = time / drawCalls, and drawCalls is zero) on every single row. The count
	 * that is meaningful for a pass row is how many timing intervals the row is the sum
	 * of, so that is what this column set shows instead.
	 */
	std::vector<ColumnConfig> BuildPassTableColumns(const Menu::ThemeSettings& theme, const DrawCallLegends& legends, bool anyTestData, const char* timeHeader, const char* intervalTooltip);
	DrawCallRowSets BuildDrawCallRows() const;
	std::function<void(int, int, const DrawCallRow&)> CreateTableRowHandler(const std::vector<ColumnConfig>& columns);

	// Row id for the "GPU Passes Total" summary row. Above the GPU bucket ids and far
	// outside the RE::BSShader::Type range, so it can never be mistaken for a toggle.
	static constexpr int kGpuTotalRowId = 999;

	// Row ids for the whole-frame GPU timeline rows fed by Util::GpuFrameTimer. Same
	// reasoning as kGpuTotalRowId: above every bucket id and far outside
	// RE::BSShader::Type, so magic_enum::enum_cast rejects them and no toggle can fire.
	static constexpr int kGpuUntrackedRowId = 1000;
	static constexpr int kGpuGapRowId = 1001;
	static constexpr int kGpuFrameElapsedRowId = 1002;

	// ============================================================================
	// EVENT HANDLING FUNCTIONS
	// ============================================================================
	void HandleShaderToggle(const DrawCallRow& row, bool wasEnabled);
	void HandleTotalRowToggle();

	// ============================================================================
	// TEST DATA MANAGEMENT FUNCTIONS
	// ============================================================================
	void UpdateShaderTestData(int shaderType, float frameTime, float costPerCall);
	void UpdateAllShaderTestData();
	void UpdateShaderTestDataEntry(int shaderType, float frameTime, float costPerCall, float percent = 0.0f);
	void UpdateSummaryTestData(float smoothedFrameTime, float otherFrameTime, float otherPercent, float totalCostPerCall);
	std::string GetTestDataTooltip() const;

	// ============================================================================
	// PERFORMANCE OVERLAY STATE MANAGEMENT
	// ============================================================================

	struct State
	{
		// Frame time history buffers
		CircularBuffer<float> frameTimeHistory;
		CircularBuffer<float> postFGFrameTimeHistory;

		// Rolling window feeding the Avg / 1% Low readouts (independent of the user-sized
		// graph history so the statistics window never changes meaning). Sized for the
		// worst-case frame rate; the readouts trim it to a fixed duration.
		CircularBuffer<float> statsWindow;

		// State flags
		bool isFrameGenerationActive = false;

		// True when Post-FG numbers come from the backend's reported presentation cadence,
		// false when they are the fixed-multiplier estimate. Surfaced in the UI so an
		// estimate is never mistaken for a measurement.
		bool postFGIsMeasured = false;
		// Presented frames per rendered frame actually used for the Post-FG numbers.
		float postFGMultiplier = 0.0f;
		// The multiplier the frame-generation backend is configured to run, as it reported it
		// (DLSS-G: the numFramesToGenerate Streamline accepted, plus one; FSR: 2). Shown next to
		// the measured cadence so a request that did not take -- e.g. an external unlock not
		// loaded -- is visible as "running 2x" rather than inferred from frame rates.
		uint appliedFGMultiplier = 0;
		// Multiplier DLSS-G refused this session and degraded to 2x, or 0.
		uint rejectedFGMultiplier = 0;
		bool frameGenerationIsDLSSG = false;

		// False until the first Present has established a baseline for the QPC delta.
		bool frameClockPrimed = false;

		// Performance counters. Zero-initialised so AdvanceFrameClock() can tell "not yet
		// primed" from a real sample even if it runs before DataLoaded().
		int64_t frequency = 0;
		int64_t lastFrameCounter = 0;
		int64_t currentFrameCounter = 0;

		// Current frame metrics
		float frameTimeMs = 0.0f;
		float fps = 0.0f;
		float postFGFrameTimeMs = 0.0f;
		float postFGFps = 0.0f;

		// Smoothed metrics
		float smoothFps = 0.0f;
		float smoothFrameTimeMs = 0.0f;
		float postFGSmoothFps = 0.0f;
		float postFGSmoothFrameTimeMs = 0.0f;

		// Update timing using QueryPerformanceCounter
		float updateTimer = 0.0f;
		LARGE_INTEGER overlayTimingFrequency = { 0 };
		LARGE_INTEGER lastUpdateTime = { 0 };

		// Min/max tracking
		float minFrameTime = 1000.0f;
		float maxFrameTime = 0.0f;
		float smoothedMinFrameTime = 0.0f;
		float smoothedMaxFrameTime = 50.0f;
	};
	State state;

	// ============================================================================
	// SETTINGS STRUCTURE
	// ============================================================================
	struct Settings
	{
		// Performance threshold constants
		static constexpr float kSmoothingFactor = 0.15f;             // Smoothing factor: 0.1f = slow, 0.3f = fast.
		static constexpr float kFrameTimeGoodThreshold = 2.0f;       // ms - Good performance threshold
		static constexpr float kFrameTimeWarningThreshold = 5.0f;    // ms - Warning performance threshold
		static constexpr float kCostPerCallGoodThreshold = 0.05f;    // ms/call - Good cost per call threshold
		static constexpr float kCostPerCallWarningThreshold = 0.2f;  // ms/call - Warning cost per call threshold
		static constexpr float kMicrosecondThreshold = 0.01f;        // ms - Threshold for showing microseconds
		static constexpr float kPercentDisplayThreshold = 0.01f;     // Minimum percent difference to display
		static constexpr float kGraphSpreadMultiplier = 2.0f;        // Standard deviation multiplier for graph range
		static constexpr float kGraphMinSpread = 2.0f;               // ms - Minimum graph spread
		static constexpr float kGraphMaxSpread = 20.0f;              // ms - Maximum graph spread
		// Fallback presentation multiplier used only when the frame-generation backend
		// reports no cadence of its own (FSR 3 frame generation never does). Anything
		// derived from it is labelled as an estimate in the UI; DLSS-G reports a real
		// presented-frame count, which is used instead when available.
		static constexpr float kFrameGenerationMultiplier = 2.0f;
		static constexpr float kMaxUpdateInterval = 2.0f;            // seconds - Maximum update interval
		static constexpr float kDefaultWindowPadding = 10.0f;        // pixels - Default window padding
		static constexpr float kLabelPadding = 100.0f;               // pixels - Padding for labels
		static constexpr float kDrawCallsTableWidth = 600.0f;        // pixels - Draw calls table width
		static constexpr float kVRAMSectionWidth = 300.0f;           // pixels - VRAM section width
		static constexpr float kWindowBorderPadding = 20.0f;         // pixels - Window border padding
		static constexpr float kDefaultFrameTimeMs = 16.67f;         // ms - Default frame time (60 FPS)
		static constexpr int kMinFrameHistorySize = 120;             // 2s @ 60fps, 0.5s @ 240fps
		static constexpr int kMaxFrameHistorySize = 1800;            // 30s @ 60fps, 7.5s @ 240fps
		// Avg / 1% Low window. Defined in seconds so its meaning is frame-rate
		// independent: a 120-frame window held only ~1.2 samples in its top 1%, making
		// the "1% Low" readout little more than the single worst frame. Ten seconds is
		// ~600 frames at 60 FPS, so the 99th percentile sits on the ~6th worst frame.
		static constexpr float kStatsWindowSeconds = 10.0f;
		// Ring capacity: 10 s at 300 FPS. Excess capacity costs 12 KB and is harmless.
		static constexpr int kStatsWindowMaxFrames = 3000;
		// Minimum coverage before Avg / 1% Low are shown at all.
		static constexpr float kStatsWindowMinSeconds = 2.0f;
		// Frames slower than this (< 2 FPS) are loading screens, alt-tabs or breakpoints,
		// not gameplay stutter. They are still plotted but kept out of the statistics.
		static constexpr float kStatsMaxSampleMs = 500.0f;

		bool ShowInOverlay = true;  // was: Enabled
		bool ShowDrawCalls = true;
		bool ShowVRAM = true;
		bool ShowFPS = true;
		bool ShowPreFGFrameTimeGraph = true;
		bool ShowPostFGFrameTimeGraph = true;
		float UpdateInterval = 0.5f;
		int FrameHistorySize = 600;  // 10s @ 60fps, 2.5s @ 240fps
		float TextSize = 1.0f;

		float BackgroundOpacity = 0.5f;
		bool ShowBorder = true;
		ImVec2 Position = ImVec2(10.f, 10.f);
		bool PositionSet = false;

		// (batch 36e) Readability. Values are PerfView::SortMode / SmoothMode.
		int SortMode = 0;               // 0 smoothed (hysteresis), 1 fixed order, 2 live
		int SmoothingMode = 1;          // 0 off, 1 window average, 2 EMA
		float SmoothingWindow = 0.5f;   // seconds
		bool ShowLiveValues = false;    // print the live reading instead of the smoothed one
		bool ShowPeakColumn = false;    // max of the live reading over the window
		int TopN = 0;                   // 0 = every row
		uint32_t FreezeKey = VK_F11;    // freeze / unfreeze every number (also logs a snapshot)
		int DenoiserLogInterval = 0;    // seconds between denoiser log lines, 0 = off

		// Collapsible sections; open state is part of the saved settings.
		bool SectionFrame = true;
		bool SectionBottleneck = false;
		bool SectionShaderTypes = true;
		bool SectionCsCpu = false;
		bool SectionGpuPasses = true;
		bool SectionEngine = false;
		bool SectionDenoiser = false;
		bool SectionGrass = false;
		bool SectionVram = true;
		bool SectionView = false;
		bool SectionShadows = false;    // (batch 37a) "Shadow maps"
	};
	Settings settings;

	/// (batch 36e) Display state of every table. Not saved.
	struct ViewState
	{
		PerfView::StableTable<DrawCallRow> shaderTypes;
		PerfView::StableTable<DrawCallRow> shaderFooter;
		PerfView::StableTable<DrawCallRow> csCpu;
		PerfView::StableTable<DrawCallRow> gpuPasses;
		PerfView::StableTable<DrawCallRow> gpuFooter;
		PerfView::StableTable<DrawCallRow> engine;
		PerfView::StableTable<DrawCallRow> engineCpu;  ///< (batch 37a) same rows as `engine`, render-thread CPU ms
		PerfView::StableTable<DenoiserRow> denoiser;
		PerfView::StableTable<DenoiserRow> denoiserFooter;
		PerfView::StableTable<int> summary;

		bool frozen = false;
		bool pendingSnapshot = false;
		bool pendingJsonSave = false;  // (batch 37a) written after the tables, like the log snapshot
		std::string message;           // (batch 37a) "Saved perf-....json" and similar
		bool messageIsError = false;
		double messageUntil = 0.0;
		std::string lastSavedFile;
		double frozenAt = 0.0;
		double lastDenoiserLog = 0.0;
		// Copies taken at the moment of freezing, so the FPS block and graphs freeze too.
		std::optional<State> frozenState;
		FrameStats frozenStats{};
		// Render / output resolution at the last update, for the denoiser table header.
		uint32_t renderWidth = 0, renderHeight = 0, outputWidth = 0, outputHeight = 0;
	};
	ViewState view;

private:
	// ============================================================================
	// PRIVATE DATA STRUCTURES
	// ============================================================================
	struct TestData
	{
		float frameTime;
		float costPerCall;
		float percent;
	};

	enum class TestDataSource
	{
		None,
		ABTest_VariantB,
		ManualShaderToggle
	};

	// A/B testing settings diff data
	std::vector<SettingsDiffEntry> settingsDiff;
	bool settingsDiffLoaded = false;

	// Test data management
	void CaptureTestData();
	void ClearTestData();
	TestDataSource testDataSource = TestDataSource::None;
	LARGE_INTEGER testDataLastUpdated = { 0 };
	std::unordered_map<int, TestData> testData;
};