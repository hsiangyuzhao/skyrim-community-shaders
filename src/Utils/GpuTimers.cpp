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
		};
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

		float totalMs = 0.0f;
		for (int i = 0; i < a_slot.used; ++i) {
			auto& interval = a_slot.intervals[i];

			D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointData{};
			if (context->GetData(interval.disjoint.get(), &disjointData, sizeof(disjointData), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return SlotStatus::NotReady;

			UINT64 t0 = 0, t1 = 0;
			if (context->GetData(interval.start.get(), &t0, sizeof(t0), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return SlotStatus::NotReady;
			if (context->GetData(interval.end.get(), &t1, sizeof(t1), D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK)
				return SlotStatus::NotReady;

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

	void GpuPassTimers::Begin(GpuBucket a_bucket)
	{
		if (!OverlayWantsTimings())
			return;

		AdvanceFrameIfNew();

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
	}

	void GpuPassTimers::ForEachActiveBucket(const std::function<void(const char*, int, float, const char*)>& a_callback)
	{
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
			a_callback(kBucketInfo[i].label, kRowIdBase + i, bucket.smoothedMs, kBucketInfo[i].tooltip);
		}
	}

	float GpuPassTimers::GetActiveBucketsTotalMs()
	{
		float total = 0.0f;
		ForEachActiveBucket([&total](const char*, int, float a_ms, const char*) {
			total += a_ms;
		});
		return total;
	}
}
