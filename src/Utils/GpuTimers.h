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
		SSRTTrace = 0,   // depth pyramid, prepare color, ray march, SHARC, diffuse composite
		SSRTSvgf,        // hand-written SVGF: temporal + variance + a-trous (diffuse and specular)
		SSRTReblur,      // NRD REBLUR: pack + REBLUR dispatches + unpack (diffuse and specular)
		SSGI,            // Screen Space GI compute chain (excluding Contact AO)
		SSGIContactAO,   // SSGI Contact AO pass
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

		/**
		 * @brief Iterates buckets that reported GPU work within the activity timeout.
		 * @param a_callback (label, overlay row id, smoothed milliseconds, tooltip)
		 */
		void ForEachActiveBucket(const std::function<void(const char*, int, float, const char*)>& a_callback);

		/**
		 * @brief Sum of smoothed milliseconds over recently active buckets.
		 *
		 * Used by the overlay to shrink the "Other" residual by what the buckets explain.
		 */
		float GetActiveBucketsTotalMs();

		// Overlay row ids for GPU buckets: kRowIdBase + bucket index. Chosen well above the
		// RE::BSShader::Type range so they can never collide with the per-shader rows and so
		// magic_enum::enum_cast<RE::BSShader::Type> rejects them (no toggle handling).
		static constexpr int kRowIdBase = 100;

	private:
		static constexpr int kFramesInFlight = 5;      // readback latency budget in frames
		static constexpr int kMaxIntervalsPerFrame = 8;  // prepass + 2 chains + composite, x2 for VR
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
			int openInterval = -1;  // interval index while between Begin and End, else -1
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
	};
}
