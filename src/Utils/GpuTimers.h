#pragma once

#include <d3d11.h>
#include <functional>
#include <string>
#include <string_view>
#include <winrt/base.h>

#include "Utils/Game.h"

namespace Util
{
	/**
	 * @brief GPU pass timing buckets shown as their own rows in the Performance Overlay.
	 *
	 * Each bucket covers one of Community Shaders' own compute-pass groups whose GPU cost
	 * previously landed in the overlay's "Other" residual. The SVGF and REBLUR buckets are
	 * deliberately separate so the two denoisers can be compared directly while switching
	 * the SSRT Denoiser dropdown.
	 */
	enum class GpuBucket : int
	{
		// (batch 11, item B2) The former single SSRTTrace row, split four ways. It was one bucket
		// accumulating four disjoint stretches of the frame, which made "SSRT costs N ms" the only
		// answer it could give -- and the question people actually have is how that N divides
		// between the diffuse and the specular chain, because those have independent toggles.
		SSRTDepthPyramid = 0,    // SSRT prepass: depth linearise + Hi-Z pyramid build
		SSRTTraceDiffuse,        // diffuse ray march (and the SHARC update/resolve when built in)
		SSRTTraceSpecular,       // specular prepare-color + specular ray march
		SSRTComposite,           // diffuse composite, including the full-res 7x7 confidence window
		SSRTSvgf,                // hand-written SVGF: temporal + variance + a-trous (diffuse and specular)
		SSRTReblur,              // NRD REBLUR dispatches + back-end unpack (diffuse and specular)
		SSRTConfidenceFilter,    // reinjection confidence: quarter-res downsample + separable blur + upsample
		NRDGuides,               // (batch 11, item B1) NRD::PrepareGuides: viewZ + normal/roughness + MV copy
		SSGI,                    // Screen Space GI compute chain (excluding Contact AO)
		SSGIContactAO,           // SSGI Contact AO pass
		PhysicalSkyShadowAccum,  // Physical Sky aerial-perspective shadow accumulation
		PhysicalSkyLuts,         // Physical Sky transmittance / multiscatter / sky-view / aerial LUTs
		SkylightingHeightMap,    // Skylighting height-map geometry depth pass
		SkylightingProbes,       // Skylighting probe volume update
		TerrainBlending,         // Terrain Blending clear + blend CS + depth copy + blended passes
		SSPLS,                   // Screen Space Point Light Shadows PrepareDepth chain
		PostProcessing,          // whole post-processing chain (pre-upscale and pre-tonemap legs)
		SubsurfaceScattering,    // Separable / Burley SSS blur chain and composite
		LightLimitFix,           // cluster building + light culling
		VolumetricLighting,      // generate + raymarch + both blur passes
		DynamicCubemaps,         // capture / inferrence / irradiance convolution round-robin
		GrassOptimizations,      // grass cell uploads + Hi-Z pyramid + per-instance cull dispatches
		VariableRateShading,     // shading-rate image build + per-tile scene analysis (+ debug tint)
		Count
	};

	/**
	 * @brief D3D11 timestamp-query pass timer feeding the Performance Overlay.
	 *
	 * Render-thread only (all methods must be called on the thread that owns the D3D11
	 * immediate context, which is where every feature pass and the overlay itself run).
	 *
	 * Design constraints honoured here:
	 * - Results are read back 1..kFramesInFlight-1 frames later with non-blocking GetData
	 *   (D3D11_ASYNC_GETDATA_DONOTFLUSH); there is never a synchronous wait.
	 * - Every interval is a complete disjoint-begin / t0 / t1 / disjoint-end quadruple, and
	 *   frames whose disjoint query reports Disjoint are dropped instead of folded in.
	 * - Begin/End become no-ops while the overlay (or its draw-call table) is hidden, so
	 *   normal gameplay pays nothing for the queries.
	 * - A bucket whose passes stop running (e.g. switching the Denoiser dropdown away from
	 *   SVGF) decays and drops out of the overlay after kActiveTimeoutFrames frames.
	 */
	class GpuPassTimers
	{
	public:
		static GpuPassTimers* GetSingleton();

		/**
		 * @brief Opens a timing interval for a bucket around GPU work.
		 *
		 * May be called several times per frame per bucket (e.g. diffuse and specular legs);
		 * the intervals accumulate into one per-frame sample. Unbalanced calls are ignored.
		 */
		void Begin(GpuBucket a_bucket);

		/**
		 * @brief Closes the interval opened by the matching Begin. Safe to call even if the
		 * matching Begin was suppressed (it is then a no-op), so gate flips mid-frame cannot
		 * leave a query pair dangling.
		 */
		void End(GpuBucket a_bucket);

		/// @brief One row's worth of bucket state, as handed to ForEachActiveBucket.
		struct BucketReport
		{
			const char* label;
			int rowId;
			float smoothedMs;
			/// Timing intervals folded into the most recently collected frame sample. This
			/// is what replaces the meaningless "Draw Calls" / "Cost/Call" columns for GPU
			/// rows: a bucket can measure several disjoint stretches of one frame (SSRT
			/// Trace has four, Volumetric Lighting four), and the row is their sum.
			int intervalsPerFrame;
			const char* tooltip;
		};

		/**
		 * @brief Iterates buckets that reported GPU work within the activity timeout.
		 */
		void ForEachActiveBucket(const std::function<void(const BucketReport&)>& a_callback);

		/**
		 * @brief Releases every query and clears all smoothing/pending state.
		 *
		 * Timestamp queries belong to the D3D11 device that created them; using them on a
		 * new device is a debug-layer error at best. Begin() detects a device change and
		 * calls this automatically, so no external wiring is required, but it is public so
		 * a device-lost or render-target rebuild path can also call it explicitly.
		 * Render-thread only, like everything else here.
		 */
		void Reset();

		/**
		 * @brief True while a pass interval (and therefore a disjoint query) is open.
		 *
		 * D3D11 forbids nesting queries, so GpuFrameTimer asks before opening its own
		 * disjoint window. Through the public API this can only be false at Present -
		 * every Begin is matched by an End earlier in the frame - but "cannot happen"
		 * is not a guarantee, and the failure mode without the check is a debug-layer
		 * error plus garbage timings rather than one missing sample.
		 */
		bool HasOpenInterval() const { return openBucket >= 0; }

		// Overlay row ids for GPU buckets: kRowIdBase + bucket index. Chosen well above the
		// RE::BSShader::Type range so they can never collide with the per-shader rows and so
		// magic_enum::enum_cast<RE::BSShader::Type> rejects them (no toggle handling).
		static constexpr int kRowIdBase = 100;

	private:
		static constexpr int kFramesInFlight = 5;  // readback latency budget in frames
		// Begin/End pairs one bucket may accumulate per frame. The busiest bucket needs 4
		// (Volumetric Lighting: generate + raymarch + both blurs). SSRT used to need 4 as well;
		// batch 11 item B2 split that bucket into the four stretches it was summing, so each of
		// those now opens exactly one interval. VR does NOT double these: the eyes
		// are packed double-wide or into an array and handled by the same host dispatch,
		// so it is the dispatch extent that grows, not the number of intervals. 12 leaves
		// headroom instead of silently dropping intervals; each unused slot is a null
		// pointer, so the spare capacity costs nothing at runtime.
		static constexpr int kMaxIntervalsPerFrame = 12;
		static constexpr int kActiveTimeoutFrames = 60;  // hide a bucket ~1s after its passes stop
		static constexpr float kSmoothingOld = 0.95f;    // matches State::Debug() smoothing
		static constexpr float kSmoothingNew = 0.05f;

		struct Interval
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			winrt::com_ptr<ID3D11Query> start;
			winrt::com_ptr<ID3D11Query> end;
		};

		struct FrameSlot
		{
			Interval intervals[kMaxIntervalsPerFrame];
			int used = 0;         // intervals issued for this frame
			bool pending = false;  // queries issued, result not yet folded in
		};

		struct Bucket
		{
			FrameSlot slots[kFramesInFlight];
			float smoothedMs = 0.0f;
			uint64_t lastActiveFrame = 0;
			int openInterval = -1;      // interval index while between Begin and End, else -1
			int lastSampleIntervals = 0;  // intervals in the most recently collected sample
			bool everActive = false;
			// True once at least one frame's queries actually came back. Without this a
			// bucket whose queries never resolve (disjoint every frame, a driver that
			// refuses the query, a Begin/End pair around no GPU work at all) sat in the
			// table showing a permanent 0 ms, which reads as a broken measurement. No
			// sample means no row.
			bool hasSample = false;
		};

		enum class SlotStatus
		{
			NotReady,
			Ready,
			Invalid
		};

		bool OverlayWantsTimings() const;
		void AdvanceFrameIfNew();
		SlotStatus TryCollectSlot(FrameSlot& a_slot, float& a_outMs) const;
		bool EnsureQueries(Interval& a_interval) const;

		Bucket buckets[static_cast<int>(GpuBucket::Count)];
		Util::FrameChecker frameChecker;
		uint64_t frameIndex = 0;
		int writeSlot = 0;
		// Device that created the currently held queries. A query may only be used with the
		// device that created it, so a device swap must release everything.
		ID3D11Device* queryDevice = nullptr;
		// Bucket currently between Begin and End, or -1. D3D11 queries must not be nested,
		// so while one interval is open no other bucket may open one. Buckets are placed
		// around disjoint stretches of the frame, so this never actually rejects work; it
		// exists so a future call site in an unexpected place degrades into a missing
		// sample instead of a debug-layer error and garbage timings.
		int openBucket = -1;
	};

	/**
	 * @brief Whole-frame GPU timeline instrumentation: the frame-spanning timestamp pair.
	 *
	 * GpuPassTimers only ever sees Community Shaders' own passes, so the overlay could only
	 * ever say "GPU >= sum of our buckets". Everything else on the GPU - the engine's own
	 * draws, DLSS super resolution and DLSS-G frame generation (both inside Streamline /
	 * driver code we do not hook) - had no row anywhere. This class supplies the missing
	 * denominator by putting two timestamps per frame on the GPU timeline, both issued from
	 * the one place that is guaranteed to run exactly once per presented frame: the
	 * IDXGISwapChain::Present hook.
	 *
	 *     MarkPresentBegin()  -> ts_begin, issued immediately before the real Present
	 *     MarkPresentEnd()    -> ts_end,   issued immediately after the real Present returns
	 *
	 * From ts_begin(n), ts_end(n) and the previous frame's pair, three quantities follow,
	 * and they are additive by construction (see Report):
	 *
	 *     frameElapsedMs  = ts_begin(n)   - ts_begin(n-1)
	 *     presentSpanMs   = ts_end(n-1)   - ts_begin(n-1)
	 *     workSpanMs      = ts_begin(n)   - ts_end(n-1)
	 *
	 * What these are NOT: GPU busy time. A GPU timestamp is a reading of the GPU's clock,
	 * and that clock keeps running while the GPU has nothing to do, so a frame-spanning
	 * pair measures *elapsed* GPU time and in steady state simply reproduces wall-clock
	 * frame time. That is exactly why the pair is only useful together with the split: it
	 * is the split point (the Present call) that carries the information, because
	 * everything the frame submitted lands on one side of it and the flip, the frame-rate
	 * pacing and any other-queue work land on the other.
	 *
	 * Design constraints honoured here:
	 * - No disjoint query is ever nested. D3D11 forbids nesting queries, and GpuPassTimers
	 *   opens a disjoint query around each of our passes; a disjoint query spanning the
	 *   whole frame would contain those. Instead each marker gets its own degenerate
	 *   disjoint window (Begin, End(timestamp), End) at a point in the frame where no pass
	 *   interval is open. Timestamp queries themselves are not restricted this way, which
	 *   is what makes the frame-spanning pair possible at all.
	 * - Because a degenerate disjoint window cannot report a clock change that happens
	 *   later in the frame, validity is established by requiring the reported Frequency to
	 *   be identical at both markers and across the two frames a sample spans, plus a
	 *   sanity band on the result. A frequency change is precisely the event the Disjoint
	 *   flag exists to catch, so this is the same check applied across the real interval
	 *   rather than across a zero-length one.
	 * - Results are read back with non-blocking GetData (D3D11_ASYNC_GETDATA_DONOTFLUSH)
	 *   out of a kFramesInFlight ring, so there is never a synchronous wait, and a slot
	 *   whose result did not arrive within the budget is dropped rather than stalled on.
	 * - Both entry points return immediately while the overlay's pass table is hidden, so
	 *   normal gameplay pays a single boolean check and creates no queries at all.
	 *
	 * Render-thread only, like GpuPassTimers.
	 */
	class GpuFrameTimer
	{
	public:
		static GpuFrameTimer* GetSingleton();

		/// @brief Issues this frame's first marker. Call immediately before the real Present.
		void MarkPresentBegin();

		/// @brief Issues this frame's second marker. Call immediately after the real Present
		///        returns. A no-op if MarkPresentBegin was suppressed, so a gate flip in
		///        between cannot leave a marker unpaired.
		void MarkPresentEnd();

		/**
		 * @brief The three spans, smoothed. All three are elapsed GPU clock, not busy time.
		 *
		 * By construction frameElapsedMs == workSpanMs + presentSpanMs for every raw
		 * sample, and the same smoothing is applied to all three, so the identity survives
		 * smoothing and the overlay can present them as an additive breakdown.
		 */
		struct Report
		{
			/// False until at least one sample has been collected; the overlay then shows
			/// no rows at all rather than a row of zeros.
			bool hasSample = false;
			/// Present-to-present elapsed GPU clock. Tracks wall-clock frame time.
			float frameElapsedMs = 0.0f;
			/// Elapsed GPU clock from the end of the previous Present to the start of this
			/// one: everything the engine, Community Shaders and DLSS super resolution
			/// submitted for the frame, plus any GPU idle inside the frame.
			float workSpanMs = 0.0f;
			/// Elapsed GPU clock across the Present call itself: the flip, vsync or
			/// frame-limiter pacing, DLSS-G frame generation on its own queue, and GPU
			/// idle while the CPU is blocked.
			float presentSpanMs = 0.0f;
		};

		Report Get() const;

		/// @brief Releases every query and clears all state. Called automatically on a
		///        device change, like GpuPassTimers::Reset().
		void Reset();

	private:
		static constexpr int kFramesInFlight = 5;
		static constexpr float kSmoothingOld = 0.95f;  // matches State::Debug() smoothing
		static constexpr float kSmoothingNew = 0.05f;
		// Sanity band on a raw sample, in milliseconds. Anything outside it is a clock
		// artefact, a wrapped counter or a paused/alt-tabbed frame, not a measurement: a
		// GPU frame under 0.05 ms means a 20000 fps frame and over 2000 ms means the game
		// was not rendering. Dropping those keeps a single bad frame out of the average.
		static constexpr double kMinPlausibleMs = 0.05;
		static constexpr double kMaxPlausibleMs = 2000.0;

		struct Slot
		{
			winrt::com_ptr<ID3D11Query> disjointBegin;
			winrt::com_ptr<ID3D11Query> disjointEnd;
			winrt::com_ptr<ID3D11Query> timestampBegin;
			winrt::com_ptr<ID3D11Query> timestampEnd;
			uint64_t frameIndex = 0;
			bool pending = false;  // both markers issued, result not yet folded in
		};

		enum class SlotStatus
		{
			NotReady,
			Ready,
			Invalid
		};

		/// One collected frame's raw readings, kept so the next frame's sample can span the
		/// Present boundary.
		struct Previous
		{
			bool valid = false;
			uint64_t frameIndex = 0;
			UINT64 beginTicks = 0;
			UINT64 endTicks = 0;
			UINT64 frequency = 0;
		};

		bool OverlayWantsTimings() const;
		bool EnsureQueries(Slot& a_slot) const;
		void Collect();
		SlotStatus TryReadSlot(Slot& a_slot, UINT64& a_outBegin, UINT64& a_outEnd, UINT64& a_outFrequency) const;

		Slot slots[kFramesInFlight];
		Previous previous;
		uint64_t frameIndex = 0;
		int writeSlot = 0;
		int openSlot = -1;  // slot whose begin marker is issued but whose end marker is not
		ID3D11Device* queryDevice = nullptr;

		float smoothedFrameElapsedMs = 0.0f;
		float smoothedWorkSpanMs = 0.0f;
		float smoothedPresentSpanMs = 0.0f;
		bool hasSample = false;
	};

	/**
	 * @brief Wall-clock (QueryPerformanceCounter) timing of Community Shaders' own CPU work.
	 *
	 * The overlay's CPU table attributes frame time by BSShader type, and it does so by
	 * charging the interval between two consecutive engine draw calls to whichever shader
	 * type is drawing. That means our own CPU cost - building constant buffers, binding
	 * resources, issuing dispatches - was never in the "Other" residual at all: it was
	 * silently folded into whichever shader type happened to draw next.
	 *
	 * This timer measures those stretches explicitly, and State::Debug() subtracts them
	 * from the interval it is about to charge to a shader type. The result is an additive
	 * breakdown:
	 *
	 *     sum(shader types) + our CPU buckets + Present/wait + Engine (untracked)
	 *         == wall-clock frame time
	 *
	 * Same clock and same 0.95/0.05 smoothing as the shader-type buckets, stepped once per
	 * frame on the same frame counter, so the identity survives smoothing.
	 *
	 * Render-thread only (every call site is on the thread owning the immediate context).
	 * Every entry point is a no-op while the overlay's draw-call table is hidden, so normal
	 * gameplay pays nothing.
	 */
	class CpuPassTimers
	{
	public:
		static CpuPassTimers* GetSingleton();

		/**
		 * @brief Opens an interval attributed to a named bucket.
		 *
		 * @param a_key Stable identifier; a Feature's short name, or one of the
		 *              orchestration keys used in Deferred.cpp. The overlay label and
		 *              tooltip are looked up from it (see kCpuBucketDocs), falling back to
		 *              the key itself so a new call site never needs a table edit to work.
		 *
		 * Nested intervals are absorbed into the outermost one rather than double counted,
		 * so wrapping a call that internally wraps another is safe.
		 */
		void Begin(std::string_view a_key);

		/// @brief Closes the interval opened by the matching Begin. Unbalanced calls are ignored.
		void End(std::string_view a_key);

		/// @brief Brackets the real Present call, i.e. the time the CPU spends blocked
		///        waiting on the GPU, on vsync, or on a frame-rate limiter.
		void BeginPresentWait();
		void EndPresentWait();

		struct Report
		{
			const char* label;
			int rowId;
			float smoothedMs;
			int callsPerFrame;  ///< intervals this bucket recorded in the last completed frame
			const char* tooltip;
		};

		/// @brief Iterates feature buckets with a non-trivial cost. Present/wait is excluded
		///        (it gets its own summary row) and so is anything under kVisibleThresholdMs,
		///        so a feature that is switched off leaves no row behind instead of showing 0.
		void ForEachActiveBucket(const std::function<void(const Report&)>& a_callback);

		/**
		 * @brief Milliseconds accumulated in instrumented buckets since this frame started.
		 *
		 * State::Debug() takes the delta of this between two draw calls and removes it from
		 * the interval it charges to a shader type; that is what keeps our CPU cost out of
		 * the shader rows so it can be reported on its own.
		 */
		float GetFrameAccountedMs();

		/// @brief Smoothed CPU time blocked in Present (GPU wait / vsync / frame limiter).
		float GetPresentWaitMs();

		/// @brief Smoothed sum of all feature buckets, including ones too small to get a row.
		float GetFeatureTotalMs();

		void Reset();

		// Overlay row ids for CPU buckets. Above the GPU bucket range (100..115) and far
		// outside RE::BSShader::Type, so magic_enum::enum_cast rejects them and no toggle
		// handling can ever fire on one.
		static constexpr int kRowIdBase = 200;

	private:
		static constexpr int kMaxBuckets = 48;
		static constexpr int kActiveTimeoutFrames = 60;
		static constexpr float kSmoothingOld = 0.95f;  // matches State::Debug() smoothing
		static constexpr float kSmoothingNew = 0.05f;
		// Below this a row is noise, not information: it is the cost of a few binds that
		// rounds to nothing, or a feature whose Prepass is an early return. Hiding it is
		// what makes "this row is 0 ms" impossible - a bucket either has a real number or
		// no row at all.
		static constexpr float kVisibleThresholdMs = 0.005f;

		struct Bucket
		{
			std::string key;
			const char* label = nullptr;
			const char* tooltip = nullptr;
			double frameMs = 0.0;
			int frameCalls = 0;
			float smoothedMs = 0.0f;
			int lastFrameCalls = 0;
			uint64_t lastActiveFrame = 0;
			bool inUse = false;
			bool everActive = false;
		};

		bool WantsTimings() const;
		void AdvanceFrameIfNew();
		int Acquire(std::string_view a_key);
		int64_t Now() const;

		Bucket buckets[kMaxBuckets];
		Util::FrameChecker frameChecker;
		uint64_t frameIndex = 0;
		int64_t frequency = 0;

		double frameAccountedMs = 0.0;

		// Present/wait is kept out of the bucket array: it is not a feature, it is the
		// opposite of one (time the CPU is doing nothing), and it needs its own row.
		double presentWaitFrameMs = 0.0;
		float presentWaitSmoothedMs = 0.0f;
		int64_t presentWaitStart = 0;

		int openBucket = -1;
		int openDepth = 0;
		int64_t openStart = 0;
	};

	/// @brief RAII wrapper for CpuPassTimers. The key must outlive the scope.
	struct CpuPassScope
	{
		explicit CpuPassScope(std::string_view a_key) :
			key(a_key)
		{
			CpuPassTimers::GetSingleton()->Begin(key);
		}
		~CpuPassScope() { CpuPassTimers::GetSingleton()->End(key); }

		CpuPassScope(const CpuPassScope&) = delete;
		CpuPassScope& operator=(const CpuPassScope&) = delete;

	private:
		std::string_view key;
	};
}
