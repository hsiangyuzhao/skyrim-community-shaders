#include "Utils/GpuTimers.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"
#include "Utils/GpuPhaseTimeline.h"

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
			{ "SSRT Depth Pyramid",
				"GPU time for SSRT's depth preparation, shared by its diffuse and specular parts." },
			{ "SSRT Trace Diffuse",
				"GPU time for SSRT's diffuse (bounce light) ray tracing." },
			{ "SSRT Sparse Resolve",
				"GPU time for filling in the full-resolution result when Diffuse Sampling is not Full. "
				"Sparse sampling only pays off if it lowers SSRT Trace Diffuse by more than this row costs." },
			{ "SSRT Trace Specular",
				"GPU time for SSRT's specular (reflection) ray tracing." },
			{ "SSRT Composite",
				"GPU time for adding SSRT's diffuse light to the image. With the Low-Resolution "
				"Confidence Filter off, the confidence filter's cost is counted here too." },
			{ "SSRT SVGF",
				"GPU time for the SVGF denoiser (diffuse and specular). Only shown while the SSRT Denoiser is set to SVGF." },
			{ "SSRT REBLUR",
				"GPU time for the REBLUR denoiser (diffuse and specular). Only shown while the SSRT Denoiser is set to REBLUR." },
			{ "SSRT Confidence Filter",
				"GPU time for the low-resolution confidence filter. Only shown while Ambient Reinjection and the "
				"Low-Resolution Confidence Filter are both on; otherwise its cost is in SSRT Composite." },
			{ "NRD Guides",
				"GPU time for preparing the extra inputs the REBLUR denoiser needs. Only runs while REBLUR is in use." },
			{ "SSGI",
				"GPU time for Screen Space GI, not counting Contact AO (which has its own row)." },
			{ "SSGI Contact AO",
				"GPU time for the SSGI Contact AO pass." },
			{ "PhysicalSky ShadowAccum",
				"GPU time for Physical Sky's shadows in the air (aerial perspective). Expected to be Physical Sky's largest cost." },
			{ "PhysicalSky LUTs",
				"GPU time for rebuilding Physical Sky's lookup tables each frame. Expected to be very small (under 0.1 ms)." },
			{ "Skylighting Height Map",
				"GPU time for drawing Skylighting's height map of the scene, done every frame. Includes the rain mask when it rains." },
			{ "Skylighting Probes",
				"GPU time for updating Skylighting's light probes, done every frame." },
			{ "Terrain Blending",
				"GPU time for Terrain Blending. It has no on/off switch and still runs indoors." },
			{ "SSPLS",
				"GPU time for Screen Space Point Light Shadows' depth preparation. "
				"Nothing currently uses its result, so this time is wasted." },
			{ "Post Processing",
				"GPU time for all post-processing effects combined. To see one effect's cost, turn effects off one at a time." },
			{ "Subsurface Scattering",
				"GPU time for skin subsurface scattering. Only runs on frames that draw faces." },
			{ "Light Limit Fix",
				"GPU time for Light Limit Fix sorting lights into screen areas." },
			{ "Volumetric Lighting",
				"GPU time for volumetric lighting (light shafts), all passes combined." },
			{ "Dynamic Cubemaps",
				"GPU time for updating dynamic cubemap reflections. The work is spread over several frames, "
				"so this is the average per frame." },
			{ "Grass Optimizations",
				"GPU time Grass Optimizations adds before grass is drawn (loading new grass, culling hidden grass). "
				"Drawing the grass itself is not included." },
			{ "Variable Rate Shading",
				"GPU time Variable Rate Shading itself costs (plus its debug overlay when shown). "
				"Its savings show up in the engine's opaque geometry time, not here." },
			{ "Neural Rendering (DLSS 5)",
				"GPU time the DLSS 5 Neural Rendering pass adds to the frame: preparing its inputs, copying to and from its own "
				"D3D12 device, and waiting there while the network runs. The network alone is shown under Upscaling > Neural Rendering." },
			{ "SSS Pre-pass (38)",
				"GPU time of the batch 38 skin SSS upgrade's pre-pass (takes the skin colour out of the light before the blur). "
				"Not counted in the Subsurface Scattering row. Only runs on frames that draw faces. The upgrade also drops the "
				"37c copy and composite passes, so compare the sum of both rows with the 37c Subsurface Scattering row." },
			{ "Local Exposure (38)",
				"GPU time for Post Processing > Local Exposure (batch 38), all of its passes. Not counted in the Post Processing row." },
			{ "Volumetric Fog (near)",
				"GPU time for the detailed near grid of Volumetric Fog (Batch 38): finding where the scene is, the fog medium, "
				"lighting every fog cell by the sun, sky and nearby lights, and adding it up along each view ray." },
			{ "Volumetric Fog (far)",
				"GPU time for the coarse far grid of Volumetric Fog (Batch 38), the same four passes. No local lights out here." },
			{ "Volumetric Shadows",
				"GPU time for the small blurred sun shadow map that lets smoke and other effects darken in shadow (Batch 38)." },
			{ "Depth refresh (39)",
				"Batch 39: only while grass or LOD terrain skips the depth prepass. Copies the finished depth (and folds it into "
				"Terrain Blending's copy) so SSAO, SSRT, fog and water see what the prepass no longer drew." },
			{ "Snow Footprints (39)",
				"GPU time for keeping the footprint map around the player up to date (Dynamic Snow, Batch 39): clearing the "
				"strip that scrolls into view, filling old prints back in, and stamping this frame's footprints. Reading the "
				"map, and drawing accumulated snow, happen inside the normal object and ground drawing and are not in this row." },
			{ "Terrain Shadows",
				"GPU time for updating the mountain-shadow map (Terrain Shadows): one strip of the map per frame, the whole map "
				"in one frame after a time jump or a new worldspace (Batch 40). Reading the shadows happens inside the normal "
				"drawing and is not in this row." },
		};

		static_assert(sizeof(kBucketInfo) / sizeof(kBucketInfo[0]) == static_cast<size_t>(GpuBucket::Count),
			"kBucketInfo must have one entry per GpuBucket");
	}

	namespace
	{
		/// Shared gate for both timers: instrumentation is free unless the table is on screen.
		bool TableIsOnScreen()
		{
			auto& overlay = globals::features::performanceOverlay;
			// globals::state is required because the frame counter both timers key their
			// per-frame bookkeeping on lives there.
			return globals::state && globals::menu && globals::menu->overlayVisible &&
			       overlay.loaded && overlay.IsOverlayVisible() &&
			       overlay.settings.ShowDrawCalls;
		}
	}

	GpuPassTimers* GpuPassTimers::GetSingleton()
	{
		static GpuPassTimers singleton;
		return &singleton;
	}

	bool GpuPassTimers::OverlayWantsTimings() const
	{
		return globals::d3d::device && globals::d3d::context && TableIsOnScreen();
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
					bucket.hasSample = true;
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
			bucket.hasSample = false;
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

		// (batch 36) The frame timeline bills this stretch to "our timed passes", so the engine
		// row the pass is nested in (Terrain Blending inside the opaque pass, ...) does not
		// count it again. Issued before the disjoint Begin; a timestamp needs no window.
		GpuPhaseTimeline::GetSingleton()->Push(GpuScope::CsPasses);

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

		GpuPhaseTimeline::GetSingleton()->Pop(GpuScope::CsPasses);
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
			if (!bucket.everActive || !bucket.hasSample)
				continue;
			if (frameIndex - bucket.lastActiveFrame > static_cast<uint64_t>(kActiveTimeoutFrames))
				continue;
			a_callback(BucketReport{ kBucketInfo[i].label, kRowIdBase + i, bucket.smoothedMs,
				bucket.lastSampleIntervals, kBucketInfo[i].tooltip });
		}
	}

	// ========================================================================
	// GpuFrameTimer
	// ========================================================================

	GpuFrameTimer* GpuFrameTimer::GetSingleton()
	{
		static GpuFrameTimer singleton;
		return &singleton;
	}

	bool GpuFrameTimer::OverlayWantsTimings() const
	{
		return globals::d3d::device && globals::d3d::context && TableIsOnScreen();
	}

	bool GpuFrameTimer::EnsureQueries(Slot& a_slot) const
	{
		if (a_slot.disjointBegin && a_slot.disjointEnd && a_slot.timestampBegin && a_slot.timestampEnd)
			return true;

		auto device = globals::d3d::device;
		if (!device)
			return false;

		D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		D3D11_QUERY_DESC timestampDesc{ D3D11_QUERY_TIMESTAMP, 0 };

		if (!a_slot.disjointBegin && FAILED(device->CreateQuery(&disjointDesc, a_slot.disjointBegin.put())))
			return false;
		if (!a_slot.disjointEnd && FAILED(device->CreateQuery(&disjointDesc, a_slot.disjointEnd.put())))
			return false;
		if (!a_slot.timestampBegin && FAILED(device->CreateQuery(&timestampDesc, a_slot.timestampBegin.put())))
			return false;
		if (!a_slot.timestampEnd && FAILED(device->CreateQuery(&timestampDesc, a_slot.timestampEnd.put())))
			return false;
		return true;
	}

	GpuFrameTimer::SlotStatus GpuFrameTimer::TryReadSlot(Slot& a_slot, UINT64& a_outBegin, UINT64& a_outEnd, UINT64& a_outFrequency) const
	{
		auto context = globals::d3d::context;
		if (!context)
			return SlotStatus::Invalid;

		// Same three-way handling as GpuPassTimers::TryCollectSlot: S_OK is a result,
		// S_FALSE means ask again next frame, and a real failure means the query is
		// unusable and the sample must be discarded rather than retried forever.
		const auto poll = [context](ID3D11Query* a_query, void* a_out, UINT a_size) {
			return context->GetData(a_query, a_out, a_size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		};

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointBeginData{};
		HRESULT hr = poll(a_slot.disjointBegin.get(), &disjointBeginData, sizeof(disjointBeginData));
		if (hr == S_FALSE)
			return SlotStatus::NotReady;
		if (FAILED(hr))
			return SlotStatus::Invalid;

		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjointEndData{};
		hr = poll(a_slot.disjointEnd.get(), &disjointEndData, sizeof(disjointEndData));
		if (hr == S_FALSE)
			return SlotStatus::NotReady;
		if (FAILED(hr))
			return SlotStatus::Invalid;

		UINT64 beginTicks = 0, endTicks = 0;
		hr = poll(a_slot.timestampBegin.get(), &beginTicks, sizeof(beginTicks));
		if (hr == S_FALSE)
			return SlotStatus::NotReady;
		if (FAILED(hr))
			return SlotStatus::Invalid;
		hr = poll(a_slot.timestampEnd.get(), &endTicks, sizeof(endTicks));
		if (hr == S_FALSE)
			return SlotStatus::NotReady;
		if (FAILED(hr))
			return SlotStatus::Invalid;

		// The two disjoint windows are degenerate (opened and closed around a single
		// timestamp), so their Disjoint flag can only report a clock change happening at
		// that exact instant. It is still respected - a set flag means the reading is
		// worthless - but the check that actually covers the frame-spanning interval is
		// the frequency comparison below and in Collect().
		if (disjointBeginData.Disjoint || disjointEndData.Disjoint)
			return SlotStatus::Invalid;
		if (disjointBeginData.Frequency == 0 || disjointBeginData.Frequency != disjointEndData.Frequency)
			return SlotStatus::Invalid;

		a_outBegin = beginTicks;
		a_outEnd = endTicks;
		a_outFrequency = disjointBeginData.Frequency;
		return SlotStatus::Ready;
	}

	void GpuFrameTimer::Collect()
	{
		// Oldest first: results complete in submission order, and a sample is built from
		// two consecutive frames, so they must be folded in chronologically.
		for (int age = 0; age < kFramesInFlight; ++age) {
			auto& slot = slots[(writeSlot + age) % kFramesInFlight];
			if (!slot.pending)
				continue;

			UINT64 beginTicks = 0, endTicks = 0, frequency = 0;
			const SlotStatus status = TryReadSlot(slot, beginTicks, endTicks, frequency);
			if (status == SlotStatus::NotReady)
				continue;

			slot.pending = false;
			if (status == SlotStatus::Invalid) {
				// The chain is broken: the next slot must not treat a discarded frame as
				// its predecessor.
				previous.valid = false;
				continue;
			}

			// A sample needs the immediately preceding frame. Anything else (a dropped
			// slot, a frame whose queries were never issued because the overlay was
			// hidden, a clock frequency change) means there is no interval to measure.
			const bool chained = previous.valid &&
			                     slot.frameIndex == previous.frameIndex + 1 &&
			                     previous.frequency == frequency;
			if (chained && beginTicks >= previous.endTicks && previous.endTicks >= previous.beginTicks) {
				const double toMs = 1000.0 / static_cast<double>(frequency);
				const double elapsedMs = static_cast<double>(beginTicks - previous.beginTicks) * toMs;
				const double presentMs = static_cast<double>(previous.endTicks - previous.beginTicks) * toMs;
				const double workMs = static_cast<double>(beginTicks - previous.endTicks) * toMs;

				if (elapsedMs >= kMinPlausibleMs && elapsedMs <= kMaxPlausibleMs) {
					smoothedFrameElapsedMs = smoothedFrameElapsedMs * kSmoothingOld + static_cast<float>(elapsedMs) * kSmoothingNew;
					smoothedPresentSpanMs = smoothedPresentSpanMs * kSmoothingOld + static_cast<float>(presentMs) * kSmoothingNew;
					smoothedWorkSpanMs = smoothedWorkSpanMs * kSmoothingOld + static_cast<float>(workMs) * kSmoothingNew;
					hasSample = true;
				}
			}

			previous.valid = true;
			previous.frameIndex = slot.frameIndex;
			previous.beginTicks = beginTicks;
			previous.endTicks = endTicks;
			previous.frequency = frequency;
		}
	}

	void GpuFrameTimer::Reset()
	{
		for (auto& slot : slots) {
			slot.disjointBegin = nullptr;
			slot.disjointEnd = nullptr;
			slot.timestampBegin = nullptr;
			slot.timestampEnd = nullptr;
			slot.frameIndex = 0;
			slot.pending = false;
		}
		previous = {};
		frameIndex = 0;
		writeSlot = 0;
		openSlot = -1;
		queryDevice = nullptr;
		smoothedFrameElapsedMs = 0.0f;
		smoothedWorkSpanMs = 0.0f;
		smoothedPresentSpanMs = 0.0f;
		hasSample = false;
	}

	void GpuFrameTimer::MarkPresentBegin()
	{
		if (!OverlayWantsTimings())
			return;

		// A query belongs to the device that created it; a device rebuild invalidates
		// everything, including the smoothed values.
		if (queryDevice != globals::d3d::device) {
			Reset();
			queryDevice = globals::d3d::device;
		}

		// Every path below that abandons a frame relies on the same mechanism rather than
		// on invalidating the history directly: an abandoned frame never becomes a
		// collected slot, so the "frameIndex == previous.frameIndex + 1" test in Collect()
		// refuses to build a sample across the hole on its own. Clearing previous.valid
		// here instead would additionally throw away the still-unread samples in the ring,
		// which are perfectly good.

		// A slot left open by a missing MarkPresentEnd (the overlay was hidden between the
		// two calls) has an unpaired timestamp. Its result would never arrive, so it is
		// discarded here instead of being allowed to pin the ring.
		if (openSlot >= 0) {
			slots[openSlot].pending = false;
			openSlot = -1;
		}

		Collect();

		++frameIndex;
		auto& slot = slots[writeSlot];
		// The slot about to be reused must be free. If its result never arrived within the
		// latency budget, drop that sample rather than stalling on it.
		slot.pending = false;

		// Belt and braces against query nesting: if a pass interval is somehow still open
		// at Present, its disjoint query is open too and ours would nest inside it. Skip
		// the frame instead - one missing sample, no debug-layer error.
		if (GpuPassTimers::GetSingleton()->HasOpenInterval())
			return;

		if (!EnsureQueries(slot))
			return;

		auto context = globals::d3d::context;
		// Degenerate disjoint window: opened and closed around the single timestamp, so no
		// GpuPassTimers pass interval can ever be nested inside it. Nothing of ours is
		// mid-interval at this point in the frame either, so this Begin is never itself
		// nested. Timestamp queries are issued with End() only.
		context->Begin(slot.disjointBegin.get());
		context->End(slot.timestampBegin.get());
		context->End(slot.disjointBegin.get());

		slot.frameIndex = frameIndex;
		openSlot = writeSlot;
	}

	void GpuFrameTimer::MarkPresentEnd()
	{
		if (openSlot < 0)
			return;  // begin marker was suppressed; nothing to pair with

		auto& slot = slots[openSlot];
		if (auto context = globals::d3d::context) {
			context->Begin(slot.disjointEnd.get());
			context->End(slot.timestampEnd.get());
			context->End(slot.disjointEnd.get());
			// Only a complete pair may be collected, so pending is set here and not in
			// MarkPresentBegin.
			slot.pending = true;
		}

		openSlot = -1;
		writeSlot = (writeSlot + 1) % kFramesInFlight;
	}

	GpuFrameTimer::Report GpuFrameTimer::Get() const
	{
		Report report;
		report.hasSample = hasSample;
		report.frameElapsedMs = smoothedFrameElapsedMs;
		report.workSpanMs = smoothedWorkSpanMs;
		report.presentSpanMs = smoothedPresentSpanMs;
		return report;
	}

	// ========================================================================
	// CpuPassTimers
	// ========================================================================

	namespace
	{
		struct CpuBucketDoc
		{
			const char* key;      // Feature short name, or an orchestration key from Deferred.cpp
			const char* label;    // Overlay row name; kept aligned with the GPU bucket labels
			const char* tooltip;
		};

		// Labels match the GPU pass table on purpose: the same feature appears under the
		// same name in both tables, so its CPU submit cost and its GPU execution cost can
		// be read side by side. Keys not listed here still work - the row is then labelled
		// with the raw key.
		constexpr CpuBucketDoc kCpuBucketDocs[] = {
			{ "ScreenSpaceRayTracing", "SSRT",
				"CPU time spent sending SSRT work to the GPU. Its GPU cost is in the SSRT rows of the GPU table." },
			{ "ScreenSpaceGI", "SSGI",
				"CPU time spent sending Screen Space GI work to the GPU. Its GPU cost is in the SSGI row of the GPU table." },
			{ "Skylighting", "Skylighting",
				"CPU time spent on Skylighting's probe update and height-map setup." },
			{ "PhysicalSky", "PhysicalSky",
				"CPU time spent sending Physical Sky's work to the GPU." },
			{ "DynamicCubemaps", "Dynamic Cubemaps",
				"CPU time spent on the dynamic cubemap reflection update." },
			{ "LightLimitFix", "Light Limit Fix",
				"CPU time spent building the light list and sending light sorting work to the GPU. "
				"Can be much larger than the GPU row of the same name." },
			{ "PostProcessing", "Post Processing",
				"CPU time for a small part of post-processing setup only. Most post-processing CPU work "
				"is not counted here; its GPU cost is in the GPU table." },
			{ "TerrainBlending", "Terrain Blending",
				"CPU time spent on Terrain Blending's preparation step." },
			{ "VolumetricLighting", "Volumetric Lighting",
				"CPU time spent sending volumetric lighting work to the GPU." },
			{ "SubsurfaceScattering", "Subsurface Scattering",
				"CPU time spent sending skin subsurface scattering work to the GPU." },
			{ "ScreenSpacePointLightShadows", "SSPLS",
				"CPU time spent sending Screen Space Point Light Shadows work to the GPU." },
			{ "ScreenSpaceShadows", "Screen Space Shadows",
				"CPU time spent sending screen space shadow work to the GPU." },
			{ "TerrainShadows", "Terrain Shadows",
				"CPU time spent sending terrain shadow work to the GPU." },
			{ "NRD", "NRD Guides",
				"CPU time spent preparing the extra inputs the REBLUR denoiser needs." },
			{ "Upscaling", "Upscaling",
				"CPU time for the upscaler's per-frame bookkeeping. The upscaling itself is not included." },
			{ "Deferred Composite", "Deferred Composite",
				"CPU time spent setting up the final lighting pass that combines every feature's output." },
			{ "Shared Data", "Shared Data",
				"CPU time spent gathering the per-frame data every Community Shaders shader reads. "
				"Runs up to three times per frame." },
			{ "TruePBR", "TruePBR",
				"CPU time spent on TruePBR's per-frame setup." },
			{ "DynamicSnow", "Dynamic Snow",
				"CPU time for Dynamic Snow (Batch 39): following the weather for snow build-up, finding the feet of nearby "
				"actors and sending the footprint map update to the GPU." },
			{ "GrassOptimizations", "Grass Optimizations",
				"CPU time for Grass Optimizations' per-frame work: sorting newly loaded grass and choosing what to draw. "
				"Drawing the grass itself stays in the Grass row of the table above." },
		};

		const CpuBucketDoc* FindCpuDoc(std::string_view a_key)
		{
			for (const auto& doc : kCpuBucketDocs) {
				if (a_key == doc.key)
					return &doc;
			}
			return nullptr;
		}

		constexpr const char* kCpuGenericTooltip =
			"CPU time Community Shaders spends preparing this feature's work and sending it to the GPU. "
			"The GPU's own time for it is measured separately.";
	}

	CpuPassTimers* CpuPassTimers::GetSingleton()
	{
		static CpuPassTimers singleton;
		return &singleton;
	}

	bool CpuPassTimers::WantsTimings() const
	{
		return TableIsOnScreen();
	}

	int64_t CpuPassTimers::Now() const
	{
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return now.QuadPart;
	}

	void CpuPassTimers::AdvanceFrameIfNew()
	{
		if (frequency == 0) {
			LARGE_INTEGER freq;
			QueryPerformanceFrequency(&freq);
			frequency = freq.QuadPart;
		}

		if (!frameChecker.IsNewFrame())
			return;

		++frameIndex;

		// An interval left open across a frame boundary would otherwise measure a whole
		// frame. Discard it rather than record a lie.
		openBucket = -1;
		openDepth = 0;

		for (auto& bucket : buckets) {
			if (!bucket.inUse)
				continue;
			if (bucket.frameCalls > 0) {
				bucket.lastActiveFrame = frameIndex;
				bucket.everActive = true;
			}
			bucket.smoothedMs = bucket.smoothedMs * kSmoothingOld +
			                    static_cast<float>(bucket.frameMs) * kSmoothingNew;
			bucket.lastFrameCalls = bucket.frameCalls;
			bucket.frameMs = 0.0;
			bucket.frameCalls = 0;
		}

		presentWaitSmoothedMs = presentWaitSmoothedMs * kSmoothingOld +
		                        static_cast<float>(presentWaitFrameMs) * kSmoothingNew;
		presentWaitFrameMs = 0.0;
		frameAccountedMs = 0.0;
	}

	int CpuPassTimers::Acquire(std::string_view a_key)
	{
		int freeSlot = -1;
		for (int i = 0; i < kMaxBuckets; ++i) {
			if (buckets[i].inUse) {
				if (buckets[i].key == a_key)
					return i;
			} else if (freeSlot < 0) {
				freeSlot = i;
			}
		}
		if (freeSlot < 0)
			return -1;  // table full: drop the sample rather than mis-attribute it

		auto& bucket = buckets[freeSlot];
		bucket.key.assign(a_key);
		if (const auto* doc = FindCpuDoc(a_key)) {
			bucket.label = doc->label;
			bucket.tooltip = doc->tooltip;
		} else {
			// key is stored in the bucket's own std::string, which lives as long as the
			// singleton and is never reassigned while in use, so c_str() is stable.
			bucket.label = bucket.key.c_str();
			bucket.tooltip = kCpuGenericTooltip;
		}
		bucket.inUse = true;
		return freeSlot;
	}

	void CpuPassTimers::Begin(std::string_view a_key)
	{
		if (!WantsTimings())
			return;

		AdvanceFrameIfNew();
		if (frequency == 0)
			return;

		if (openBucket >= 0) {
			// Nested. The outer bucket already covers this time; counting it again would
			// break the "buckets sum to frame time" identity.
			++openDepth;
			return;
		}

		const int index = Acquire(a_key);
		if (index < 0)
			return;

		openBucket = index;
		openDepth = 0;
		openStart = Now();
	}

	void CpuPassTimers::End(std::string_view a_key)
	{
		if (openBucket < 0)
			return;

		if (buckets[openBucket].key != a_key) {
			// Close of an absorbed nested interval (or an unbalanced call). Either way the
			// outer interval must stay open.
			if (openDepth > 0)
				--openDepth;
			return;
		}
		if (openDepth > 0) {
			--openDepth;
			return;
		}

		const double ms = static_cast<double>(Now() - openStart) * 1000.0 / static_cast<double>(frequency);
		auto& bucket = buckets[openBucket];
		if (ms > 0.0) {
			bucket.frameMs += ms;
			frameAccountedMs += ms;
		}
		++bucket.frameCalls;
		openBucket = -1;
	}

	void CpuPassTimers::BeginPresentWait()
	{
		if (!WantsTimings())
			return;
		AdvanceFrameIfNew();
		if (frequency == 0)
			return;
		presentWaitStart = Now();
	}

	void CpuPassTimers::EndPresentWait()
	{
		if (presentWaitStart == 0 || frequency == 0)
			return;
		const double ms = static_cast<double>(Now() - presentWaitStart) * 1000.0 / static_cast<double>(frequency);
		presentWaitStart = 0;
		if (ms > 0.0) {
			presentWaitFrameMs += ms;
			frameAccountedMs += ms;
		}
	}

	float CpuPassTimers::GetFrameAccountedMs()
	{
		if (!WantsTimings())
			return 0.0f;
		AdvanceFrameIfNew();
		return static_cast<float>(frameAccountedMs);
	}

	float CpuPassTimers::GetPresentWaitMs()
	{
		return presentWaitSmoothedMs;
	}

	float CpuPassTimers::GetFeatureTotalMs()
	{
		float total = 0.0f;
		for (const auto& bucket : buckets) {
			if (bucket.inUse)
				total += bucket.smoothedMs;
		}
		return total;
	}

	void CpuPassTimers::ForEachActiveBucket(const std::function<void(const Report&)>& a_callback)
	{
		// Also drives the per-frame fold when nothing is instrumented any more, so stale
		// rows decay and time out instead of freezing at their last value.
		if (WantsTimings())
			AdvanceFrameIfNew();

		for (int i = 0; i < kMaxBuckets; ++i) {
			auto& bucket = buckets[i];
			if (!bucket.inUse || !bucket.everActive)
				continue;
			if (frameIndex - bucket.lastActiveFrame > static_cast<uint64_t>(kActiveTimeoutFrames))
				continue;
			// A bucket whose cost rounds to nothing gets no row at all. Showing "0 ms" for
			// a feature that is switched off (or whose prepass is an early return) is worse
			// than showing nothing: it reads as a broken measurement.
			if (bucket.smoothedMs < kVisibleThresholdMs)
				continue;
			a_callback(Report{ bucket.label, kRowIdBase + i, bucket.smoothedMs,
				bucket.lastFrameCalls, bucket.tooltip });
		}
	}

	void CpuPassTimers::Reset()
	{
		for (auto& bucket : buckets) {
			bucket.key.clear();
			bucket.label = nullptr;
			bucket.tooltip = nullptr;
			bucket.frameMs = 0.0;
			bucket.frameCalls = 0;
			bucket.smoothedMs = 0.0f;
			bucket.lastFrameCalls = 0;
			bucket.lastActiveFrame = 0;
			bucket.inUse = false;
			bucket.everActive = false;
		}
		frameIndex = 0;
		frameAccountedMs = 0.0;
		presentWaitFrameMs = 0.0;
		presentWaitSmoothedMs = 0.0f;
		presentWaitStart = 0;
		openBucket = -1;
		openDepth = 0;
	}

}
