#pragma once

#include <d3d11.h>
#include <functional>
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
		SSRTTrace = 0,           // depth pyramid, prepare color, ray march, SHARC, diffuse composite
		SSRTSvgf,                // hand-written SVGF: temporal + variance + a-trous (diffuse and specular)
		SSRTReblur,              // NRD REBLUR: pack + REBLUR dispatches + unpack (diffuse and specular)
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

		// Overlay row ids for GPU buckets: kRowIdBase + bucket index. Chosen well above the
		// RE::BSShader::Type range so they can never collide with the per-shader rows and so
		// magic_enum::enum_cast<RE::BSShader::Type> rejects them (no toggle handling).
		static constexpr int kRowIdBase = 100;

	private:
		static constexpr int kFramesInFlight = 5;  // readback latency budget in frames
		// Begin/End pairs one bucket may accumulate per frame. The busiest buckets need 4
		// (SSRT Trace: prepass + diffuse/specular chains + composite) and 4 (Volumetric
		// Lighting: generate + raymarch + both blurs). VR does NOT double these: the eyes
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
}
