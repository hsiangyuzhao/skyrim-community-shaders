#include "Utils/DenoiserTimers.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "State.h"
#include "Utils/GpuTimers.h"

namespace Util
{
	DenoiserTimers* DenoiserTimers::GetSingleton()
	{
		static DenoiserTimers singleton;
		return &singleton;
	}

	bool DenoiserTimers::Wanted() const
	{
		auto& overlay = globals::features::performanceOverlay;
		return globals::d3d::device && globals::d3d::context && globals::state && globals::menu &&
		       globals::menu->overlayVisible && overlay.loaded && overlay.IsOverlayVisible() &&
		       overlay.settings.ShowDrawCalls && overlay.settings.SectionDenoiser;
	}

	bool DenoiserTimers::EnsureDisjoint(FrameSlot& a_slot) const
	{
		if (a_slot.disjointBegin && a_slot.disjointEnd)
			return true;
		auto device = globals::d3d::device;
		if (!device)
			return false;
		D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
		if (!a_slot.disjointBegin && FAILED(device->CreateQuery(&desc, a_slot.disjointBegin.put())))
			return false;
		if (!a_slot.disjointEnd && FAILED(device->CreateQuery(&desc, a_slot.disjointEnd.put())))
			return false;
		return true;
	}

	bool DenoiserTimers::EnsureStamp(FrameSlot& a_slot, int a_index) const
	{
		if (a_index < 0 || a_index >= kMaxStamps)
			return false;
		// Created on first use and kept, so a frame only pays for as many queries as the
		// busiest frame so far needed.
		while (static_cast<int>(a_slot.stamps.size()) <= a_index) {
			auto device = globals::d3d::device;
			if (!device)
				return false;
			D3D11_QUERY_DESC desc{ D3D11_QUERY_TIMESTAMP, 0 };
			winrt::com_ptr<ID3D11Query> query;
			if (FAILED(device->CreateQuery(&desc, query.put())))
				return false;
			a_slot.stamps.push_back(std::move(query));
		}
		return true;
	}

	void DenoiserTimers::Reset()
	{
		for (auto& slot : slots) {
			slot.disjointBegin = nullptr;
			slot.disjointEnd = nullptr;
			slot.stamps.clear();
			slot.scopes.clear();
			slot.stampCount = 0;
			slot.frameIndex = 0;
			slot.pending = false;
		}
		writeSlot = 0;
		frameActive = false;
		frameIndex = 0;
		latestFrame = 0;
		queryDevice = nullptr;
		rows.clear();
		rowIndex.clear();
		occurrences.clear();
		nextOrder = 0;
	}

	int DenoiserTimers::FindOrAddRow(std::string_view a_group, std::string_view a_pass, int a_occurrence)
	{
		std::string key;
		key.reserve(a_group.size() + a_pass.size() + 4);
		key.append(a_group).append("|").append(a_pass);
		if (a_occurrence > 0)
			key.append("#").append(std::to_string(a_occurrence + 1));

		if (auto it = rowIndex.find(key); it != rowIndex.end())
			return it->second;

		Row row;
		row.group.assign(a_group);
		row.pass.assign(a_pass);
		if (a_occurrence > 0)
			row.pass.append(" #").append(std::to_string(a_occurrence + 1));
		row.order = nextOrder++;
		rows.push_back(std::move(row));
		const int index = static_cast<int>(rows.size()) - 1;
		rowIndex.emplace(std::move(key), index);
		return index;
	}

	void DenoiserTimers::BeginFrame()
	{
		if (!Wanted()) {
			// Whatever is still in flight describes a moment long gone by the time the section
			// is opened again; start over from fresh frames.
			frameActive = false;
			for (auto& slot : slots)
				slot.pending = false;
			return;
		}

		// Queries belong to the device that created them.
		if (queryDevice != globals::d3d::device) {
			Reset();
			queryDevice = globals::d3d::device;
		}

		Collect();

		auto& slot = slots[writeSlot];
		slot.pending = false;  // never arrived within the ring: dropped, not waited for
		slot.scopes.clear();
		slot.stampCount = 0;
		occurrences.clear();

		// The opening window must not nest inside one of GpuPassTimers'. Nothing of ours is
		// mid-pass at Present; skip the frame if something is.
		if (GpuPassTimers::GetSingleton()->HasOpenInterval())
			return;
		if (!EnsureDisjoint(slot))
			return;

		auto context = globals::d3d::context;
		context->Begin(slot.disjointBegin.get());
		context->End(slot.disjointBegin.get());
		slot.frameIndex = ++frameIndex;
		frameActive = true;
	}

	void DenoiserTimers::EndFrame()
	{
		if (!frameActive)
			return;
		frameActive = false;

		auto& slot = slots[writeSlot];
		bool complete = !slot.scopes.empty() && !GpuPassTimers::GetSingleton()->HasOpenInterval();
		for (const auto& scope : slot.scopes)
			complete = complete && scope.endStamp >= 0;

		if (slot.scopes.empty()) {
			// Nothing in the chain ran this frame (SSRT off, denoiser not REBLUR, ...). Advance
			// the clock anyway so every row reads as "no data" instead of its last value.
			latestFrame = std::max(latestFrame, slot.frameIndex);
		} else if (complete) {
			auto context = globals::d3d::context;
			context->Begin(slot.disjointEnd.get());
			context->End(slot.disjointEnd.get());
			slot.pending = true;
		}
		writeSlot = (writeSlot + 1) % kFramesInFlight;
	}

	int DenoiserTimers::Begin(std::string_view a_group, std::string_view a_pass, uint32_t a_groupsX, uint32_t a_groupsY, uint32_t a_threadsX, uint32_t a_threadsY, bool a_merge)
	{
		if (!frameActive)
			return -1;

		auto& slot = slots[writeSlot];
		const int stamp = slot.stampCount;
		if (!EnsureStamp(slot, stamp) || !EnsureStamp(slot, stamp + 1))
			return -1;

		std::string occKey;
		occKey.reserve(a_group.size() + a_pass.size() + 1);
		occKey.append(a_group).append("|").append(a_pass);
		const int occurrence = a_merge ? 0 : occurrences[occKey]++;

		const int row = FindOrAddRow(a_group, a_pass, occurrence);
		auto& r = rows[row];
		r.groupsX = a_groupsX;
		r.groupsY = a_groupsY;
		r.threadsX = a_threadsX;
		r.threadsY = a_threadsY;

		globals::d3d::context->End(slot.stamps[stamp].get());
		slot.stampCount += 2;  // the end stamp is reserved now so scopes never interleave indices
		slot.scopes.push_back(Scope{ row, stamp, -1 });
		// The token carries the frame it belongs to, so a scope left open across a Present can
		// never close a scope of the next frame that happens to have the same index.
		return static_cast<int>(((frameIndex & 0x7FFF) << 16) | (slot.scopes.size() - 1));
	}

	void DenoiserTimers::End(int a_token)
	{
		if (a_token < 0 || !frameActive)
			return;
		if (static_cast<uint64_t>(a_token >> 16) != (frameIndex & 0x7FFF))
			return;
		auto& slot = slots[writeSlot];
		const int index = a_token & 0xFFFF;
		if (index >= static_cast<int>(slot.scopes.size()))
			return;
		auto& scope = slot.scopes[index];
		if (scope.endStamp >= 0)
			return;
		scope.endStamp = scope.beginStamp + 1;
		globals::d3d::context->End(slot.stamps[scope.endStamp].get());
	}

	DenoiserTimers::SlotStatus DenoiserTimers::TryRead(FrameSlot& a_slot)
	{
		auto context = globals::d3d::context;
		if (!context || a_slot.scopes.empty())
			return SlotStatus::Invalid;

		const auto poll = [context](ID3D11Query* a_query, void* a_out, UINT a_size) {
			return context->GetData(a_query, a_out, a_size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		};

		// The closing window was issued last: if it is not back, nothing before it is either.
		D3D11_QUERY_DATA_TIMESTAMP_DISJOINT endData{}, beginData{};
		HRESULT hr = poll(a_slot.disjointEnd.get(), &endData, sizeof(endData));
		if (hr == S_FALSE)
			return SlotStatus::NotReady;
		if (FAILED(hr))
			return SlotStatus::Invalid;
		hr = poll(a_slot.disjointBegin.get(), &beginData, sizeof(beginData));
		if (hr == S_FALSE)
			return SlotStatus::NotReady;
		if (FAILED(hr))
			return SlotStatus::Invalid;
		if (beginData.Disjoint || endData.Disjoint || beginData.Frequency == 0 || beginData.Frequency != endData.Frequency)
			return SlotStatus::Invalid;

		scratchTicks.assign(a_slot.stampCount, 0);
		for (const auto& scope : a_slot.scopes) {
			for (int s : { scope.beginStamp, scope.endStamp }) {
				hr = poll(a_slot.stamps[s].get(), &scratchTicks[s], sizeof(UINT64));
				if (hr == S_FALSE)
					return SlotStatus::NotReady;
				if (FAILED(hr))
					return SlotStatus::Invalid;
			}
		}

		const double toMs = 1000.0 / static_cast<double>(beginData.Frequency);
		scratchMs.assign(rows.size(), 0.0);
		scratchCalls.assign(rows.size(), 0);
		for (const auto& scope : a_slot.scopes) {
			const UINT64 t0 = scratchTicks[scope.beginStamp];
			const UINT64 t1 = scratchTicks[scope.endStamp];
			if (t1 < t0)
				return SlotStatus::Invalid;
			const double ms = static_cast<double>(t1 - t0) * toMs;
			if (ms > kMaxPlausibleMs)
				return SlotStatus::Invalid;
			scratchMs[scope.row] += ms;
			++scratchCalls[scope.row];
		}

		for (size_t i = 0; i < rows.size(); ++i) {
			if (scratchCalls[i] == 0)
				continue;
			rows[i].lastMs = static_cast<float>(scratchMs[i]);
			rows[i].lastCalls = scratchCalls[i];
			rows[i].lastFrame = a_slot.frameIndex;
		}
		latestFrame = std::max(latestFrame, a_slot.frameIndex);
		return SlotStatus::Ready;
	}

	void DenoiserTimers::Collect()
	{
		// Oldest first, so LatestFrame() only ever moves forward.
		for (int age = 0; age < kFramesInFlight; ++age) {
			auto& slot = slots[(writeSlot + age) % kFramesInFlight];
			if (!slot.pending)
				continue;
			// Results complete in submission order: once one frame is not back, no newer one is.
			if (TryRead(slot) == SlotStatus::NotReady)
				break;
			slot.pending = false;
		}
	}
}
