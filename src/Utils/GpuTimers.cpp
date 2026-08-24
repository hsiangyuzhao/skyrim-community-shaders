#include "Utils/GpuTimers.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"

namespace Util
{
	namespace
	{
		struct BucketInfo
		{
			const char* label;
			const char* tooltip;
		};

		// Indexed by GpuBucket. Labels are the overlay row names (the overlay appends ':').
		constexpr BucketInfo kBucketInfo[static_cast<int>(GpuBucket::Count)] = {
			{ "SSRT Trace",
				"GPU time for SSRT ray tracing: depth pyramid, prepare color, specular/diffuse ray march,\nSHARC update/resolve and the diffuse composite. Measured with D3D11 timestamp queries." },
			{ "SSRT SVGF",
				"GPU time for the hand-written SVGF denoiser: temporal, variance and a-trous passes,\ndiffuse and specular chains combined. Only shown while the SSRT Denoiser is set to SVGF." },
			{ "SSRT REBLUR",
				"GPU time for the NRD REBLUR denoiser: front-end pack, all REBLUR dispatches and the\nback-end unpack, diffuse and specular combined. Only shown while the SSRT Denoiser is set to REBLUR." },
			{ "SSGI",
				"GPU time for the Screen Space GI compute chain (prefilter, radiance, GI, blur, upsample),\nexcluding the Contact AO pass which has its own row." },
			{ "SSGI Contact AO",
				"GPU time for the SSGI Contact AO pass." },
			{ "PhysicalSky ShadowAccum",
				"GPU time for the Physical Sky aerial-perspective shadow accumulation pass.\n"
				"Predicted to be Physical Sky's single largest cost: it is dispatched at full\n"
				"resolution while writing a half-resolution target, so ~3/4 of its threads do\n"
				"a full 30-step loop and then throw the result away." },
			{ "PhysicalSky LUTs",
				"GPU time for all four Physical Sky lookup tables together: transmittance,\n"
				"multiscatter, sky-view and aerial perspective. Regenerated every frame even\n"
				"though only UI changes affect the first two; expected to be small (<0.1 ms),\n"
				"so this row mainly confirms that it is not worth optimising." },
			{ "Skylighting Height Map",
				"GPU time for the Skylighting height-map geometry depth pass, rendered every\n"
				"frame with no throttling even when the camera is still and the probes have\n"
				"converged. Includes the precipitation-mask draw when it rains." },
			{ "Skylighting Probes",
				"GPU time for the Skylighting probe volume update dispatch. Dispatched over the\n"
				"whole volume every frame with roughly 3/4 of the threads exiting out of bounds;\n"
				"the real cost is expected to be the in-bounds read-modify-write bandwidth." },
			{ "Terrain Blending",
				"GPU time for Terrain Blending: depth clear, the full-screen blend compute pass,\n"
				"the full-resolution depth copy and the blended terrain passes. This feature has\n"
				"no user toggle, and indoors the whole chain is expected to be a no-op that still\n"
				"costs its full price - compare this row indoors and outdoors." },
			{ "SSPLS",
				"GPU time for the Screen Space Point Light Shadows PrepareDepth chain: depth\n"
				"linearisation, two GenerateMips calls and four blur dispatches. The outputs of\n"
				"this chain have no reader anywhere in the project, so whatever this row shows\n"
				"is the price of work nothing consumes." },
			{ "Post Processing",
				"GPU time for the whole post-processing chain, both legs: the pre-upscale pass\n"
				"and the pre-tonemap pass, including their format-convert and copy tails.\n"
				"Individual effects are not split out because nesting timestamp queries is not\n"
				"safe in D3D11; toggle effects one at a time to attribute the cost." },
			{ "Subsurface Scattering",
				"GPU time for the skin subsurface scattering chain (Separable or Burley) plus its\n"
				"composite. Expected to be dominated by full-screen bandwidth serving under 5%\n"
				"of pixels; only runs on frames that actually draw faces." },
			{ "Light Limit Fix",
				"GPU time for Light Limit Fix cluster building and light culling. Cluster building\n"
				"is dispatched with one thread per group, so most of each GPU wave is idle, and\n"
				"the cluster grid is rebuilt every frame even though it only depends on the\n"
				"projection." },
			{ "Volumetric Lighting",
				"GPU time for volumetric lighting: the generate pass, the raymarch pass and both\n"
				"blur passes, accumulated into one row." },
			{ "Dynamic Cubemaps",
				"GPU time for the dynamic cubemap update. The feature runs one of capture,\n"
				"inferrence or irradiance convolution per frame in a round-robin, so this row is\n"
				"the smoothed per-frame cost across the cycle rather than one pass." },
		};

		static_assert(sizeof(kBucketInfo) / sizeof(kBucketInfo[0]) == static_cast<size_t>(GpuBucket::Count),
			"kBucketInfo must have one entry per GpuBucket");
	}

	GpuPassTimers* GpuPassTimers::GetSingleton()
	{
		static GpuPassTimers singleton;
		return &singleton;
	}

	bool GpuPassTimers::OverlayWantsTimings() const
	{
		auto& overlay = globals::features::performanceOverlay;
		return globals::d3d::device && globals::d3d::context &&
		       globals::menu && globals::menu->overlayVisible &&
		       overlay.loaded && overlay.IsOverlayVisible() &&
		       overlay.settings.ShowDrawCalls;
	}

	bool GpuPassTimers::EnsureQueries(Interval& a_interval) const
	{
		if (a_interval.disjoint && a_interval.start && a_interval.end)
			return true;

		auto device = globals::d3d::device;
		if (!device)
			return false;

		D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC timestampDesc{ D3D11_QUERY_TIMESTAMP, 0 };

		if (!a_interval.disjoint && FAILED(device->CreateQuery(&disjointDesc, a_interval.disjoint.put())))
			return false;
		if (!a_interval.start && FAILED(device->CreateQuery(&timestampDesc, a_interval.start.put())))
			return false;
		if (!a_interval.end && FAILED(device->CreateQuery(&timestampDesc, a_interval.end.put())))
			return false;
		return true;
	}

	GpuPassTimers::SlotStatus GpuPassTimers::TryCollectSlot(FrameSlot& a_slot, float& a_outMs) const
	{
		auto context = globals::d3d::context;
		if (!context)
			return SlotStatus::Invalid;

		// GetData has three distinct outcomes and they must not be conflated:
		//   S_OK      - the result is available
		//   S_FALSE   - not finished yet, ask again next frame
		//   a failure - the query is unusable (wrong device, removed device, bad args);
		//               retrying forever would pin the slot as permanently pending, so the
		//               sample is discarded instead.
		// Treating every non-S_OK as "not ready", as before, meant a failing query silently
		// froze that bucket's row at its last value for the rest of the session.
		const auto poll = [context](ID3D11Query* a_query, void* a_out, UINT a_size) {
			return context->GetData(a_query, a_out, a_size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		};

		float totalMs = 0.0f;
		for (int i = 0; i < a_slot.used; ++i) {
			auto& interval = a_slot.intervals[i];

			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
			HRESULT hr = poll(interval.disjoint.get(), &disjointData, sizeof(disjointData));
			if (hr == S_FALSE)
				return SlotStatus::NotReady;
			if (FAILED(hr))
				return SlotStatus::Invalid;

			UINT64 t0 = 0, t1 = 0;
			hr = poll(interval.start.get(), &t0, sizeof(t0));
			if (hr == S_FALSE)
				return SlotStatus::NotReady;
			if (FAILED(hr))
				return SlotStatus::Invalid;
			hr = poll(interval.end.get(), &t1, sizeof(t1));
			if (hr == S_FALSE)
				return SlotStatus::NotReady;
			if (FAILED(hr))
				return SlotStatus::Invalid;

			// A disjoint frame (clock changes, driver hiccup) yields meaningless deltas;
			// drop the whole frame sample rather than folding garbage into the average.
			if (disjointData.Disjoint || disjointData.Frequency == 0)
				return SlotStatus::Invalid;

			if (t1 > t0)
				totalMs += static_cast<float>(static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(disjointData.Frequency));
		}

		a_outMs = totalMs;
		return SlotStatus::Ready;
	}

	void GpuPassTimers::AdvanceFrameIfNew()
	{
		if (!frameChecker.IsNewFrame())
			return;

		++frameIndex;
		const int finishedSlot = writeSlot;
		writeSlot = (writeSlot + 1) % kFramesInFlight;

		for (auto& bucket : buckets) {
			// Defensive: a Begin whose End never ran must not leak an open D3D11 query pair
			// into the next frame. This cannot happen through the public API (End always
			// closes when an interval is open), but a complete pairing per frame is a hard
			// requirement for timestamp queries, so enforce it here too.
			if (bucket.openInterval >= 0) {
				auto& interval = bucket.slots[finishedSlot].intervals[bucket.openInterval];
				if (auto context = globals::d3d::context) {
					context->End(interval.end.get());
					context->End(interval.disjoint.get());
				}
				bucket.openInterval = -1;
				openBucket = -1;
			}

			// A recently active bucket that issued no work in the frame that just ended is
			// idle (feature/denoiser switched off): decay its average toward zero so the row
			// reflects the switch instead of freezing at the last measured value.
			if (bucket.everActive && bucket.slots[finishedSlot].used == 0 &&
				frameIndex - bucket.lastActiveFrame <= static_cast<uint64_t>(kActiveTimeoutFrames)) {
				bucket.smoothedMs *= kSmoothingOld;
			}

			// Fold in every slot whose results have arrived, oldest first (results complete
			// in submission order, so this feeds the smoothing in chronological order).
			for (int age = 0; age < kFramesInFlight; ++age) {
				auto& slot = bucket.slots[(writeSlot + age) % kFramesInFlight];
				if (!slot.pending)
					continue;
				float sampleMs = 0.0f;
				switch (TryCollectSlot(slot, sampleMs)) {
				case SlotStatus::Ready:
					bucket.smoothedMs = bucket.smoothedMs * kSmoothingOld + sampleMs * kSmoothingNew;
					bucket.lastSampleIntervals = slot.used;
					slot.pending = false;
					slot.used = 0;
					break;
				case SlotStatus::Invalid:
					slot.pending = false;
					slot.used = 0;
					break;
				case SlotStatus::NotReady:
					break;
				}
			}

			// The slot we are about to reuse must be free; if its results never arrived
			// within the latency budget, drop that sample rather than stalling.
			auto& reused = bucket.slots[writeSlot];
			reused.pending = false;
			reused.used = 0;
		}
	}

	void GpuPassTimers::Reset()
	{
		for (auto& bucket : buckets) {
			for (auto& slot : bucket.slots) {
				for (auto& interval : slot.intervals) {
					interval.disjoint = nullptr;
					interval.start = nullptr;
					interval.end = nullptr;
				}
				slot.used = 0;
				slot.pending = false;
			}
			bucket.smoothedMs = 0.0f;
			bucket.lastActiveFrame = 0;
			bucket.openInterval = -1;
			bucket.lastSampleIntervals = 0;
			bucket.everActive = false;
		}
		writeSlot = 0;
		frameIndex = 0;
		queryDevice = nullptr;
		openBucket = -1;
	}

	void GpuPassTimers::Begin(GpuBucket a_bucket)
	{
		if (!OverlayWantsTimings())
			return;

		// A query belongs to the device that created it. After a device rebuild the old
		// queries must never be handed to the new context, so drop everything - including
		// the smoothed values, which describe a GPU that no longer exists.
		if (queryDevice != globals::d3d::device) {
			Reset();
			queryDevice = globals::d3d::device;
		}

		AdvanceFrameIfNew();

		if (openBucket >= 0)
			return;  // another bucket is mid-interval; queries must not be nested

		auto& bucket = buckets[static_cast<int>(a_bucket)];
		if (bucket.openInterval >= 0)
			return;  // unbalanced Begin; ignore

		auto& slot = bucket.slots[writeSlot];
		if (slot.used >= kMaxIntervalsPerFrame)
			return;

		auto& interval = slot.intervals[slot.used];
		if (!EnsureQueries(interval))
			return;

		auto context = globals::d3d::context;
		context->Begin(interval.disjoint.get());
		context->End(interval.start.get());  // timestamp queries are issued with End() only

		bucket.openInterval = slot.used;
		openBucket = static_cast<int>(a_bucket);
		bucket.lastActiveFrame = frameIndex;
		bucket.everActive = true;
		slot.pending = true;
		++slot.used;
	}

	void GpuPassTimers::End(GpuBucket a_bucket)
	{
		auto& bucket = buckets[static_cast<int>(a_bucket)];
		if (bucket.openInterval < 0)
			return;  // matching Begin was suppressed (overlay hidden); nothing to close

		auto& interval = bucket.slots[writeSlot].intervals[bucket.openInterval];
		if (auto context = globals::d3d::context) {
			context->End(interval.end.get());
			context->End(interval.disjoint.get());
		}
		bucket.openInterval = -1;
		if (openBucket == static_cast<int>(a_bucket))
			openBucket = -1;
	}

	void GpuPassTimers::ForEachActiveBucket(const std::function<void(const BucketReport&)>& a_callback)
	{
		// Same device-identity guard as Begin(), for the case where the overlay is drawn
		// after a device rebuild but before any instrumented pass has run again.
		if (queryDevice && queryDevice != globals::d3d::device) {
			Reset();
			return;
		}

		// Also drives readback/decay when no feature pass is running any more, so stale
		// rows still time out and disappear.
		if (OverlayWantsTimings())
			AdvanceFrameIfNew();

		for (int i = 0; i < static_cast<int>(GpuBucket::Count); ++i) {
			auto& bucket = buckets[i];
			if (!bucket.everActive)
				continue;
			if (frameIndex - bucket.lastActiveFrame > static_cast<uint64_t>(kActiveTimeoutFrames))
				continue;
			a_callback(BucketReport{ kBucketInfo[i].label, kRowIdBase + i, bucket.smoothedMs,
				bucket.lastSampleIntervals, kBucketInfo[i].tooltip });
		}
	}

}
