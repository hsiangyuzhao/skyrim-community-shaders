// (46g) The ray tracer's GPU pools (GpuArena): BLASes, character buffers and geometry uploads are ranges of a few
// large committed buffers instead of a committed resource each.
//
// Why: every CreateCommittedResource makes its memory resident and zeroes it before it returns. Right after a cell
// transition the game streams hundreds of MB of textures and the driver's memory manager is busy, so a single
// creation could block the render thread for 15-125 ms (F7: rt_exec_skin_ms / rt_exec_build_ms spikes in the first
// seconds after a door or a fast travel). Now a BLAS or a character shape is an offset into a block; blocks are made
// ahead of need -- on the pool thread while playing (the render thread never waits for it: an allocation that does not
// fit simply waits a frame), on the render thread only behind a loading screen or the black screen after it.
// Freed ranges return behind a fence value (the same rule as Backend::Retire); empty blocks past the headroom go
// through Retire too. Blocks that need whole-buffer state transitions (character pools) track their state here;
// recording order equals submission order on our single queue, so the tracked state is the GPU's.

#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>

namespace RayTracingRT
{
	namespace
	{
		std::uint64_t AlignArena(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) & ~(a_alignment - 1); }

		std::int64_t ArenaQpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double ArenaQpcToMs(std::int64_t a_ticks)
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return static_cast<double>(a_ticks) * toMs;
		}

		constexpr std::uint64_t kMB = 1024ull * 1024ull;
		constexpr std::uint64_t kBlockGranularity = 64 * 1024;
		constexpr double kSyncCreateBudgetMs = 8.0;  // render-thread block creation per frame behind loading / black screens
		constexpr double kIdleReleaseMs = 20000.0;   // (46i) no allocation asked for this long: the pool's empty blocks go
		constexpr double kMinBlockAgeMs = 3000.0;    // (46i) a new block stays at least this long
		constexpr double kWaitLogMs = 5000.0;        // (46i) a waiting allocation is logged past this
	}

	bool GpuArena::Allocate(std::uint64_t a_bytes, ArenaAlloc& o_alloc)
	{
		const std::uint64_t size = AlignArena(std::max<std::uint64_t>(a_bytes, alignment), alignment);
		lastAskQpc = ArenaQpc();  // (46i) in use: empty blocks stay
		for (std::uint32_t b = 0; b < blocks.size(); ++b) {
			auto& block = blocks[b];
			if (!block.live || block.size - block.used < size)
				continue;
			for (auto it = block.free.begin(); it != block.free.end(); ++it) {
				if (it->second < size)
					continue;
				const std::uint64_t offset = it->first;
				const std::uint64_t rest = it->second - size;
				block.free.erase(it);
				if (rest > 0)
					block.free.emplace(offset + size, rest);
				block.used += size;
				used += size;
				o_alloc = ArenaAlloc{ b, offset, size };
				return true;
			}
		}
		// Does not fit: the next block must hold it (made on the pool thread, or behind a loading screen).
		wantBytes = std::max(wantBytes, size);
		if (waitSinceQpc == 0)
			waitSinceQpc = lastAskQpc;
		o_alloc = {};
		return false;
	}

	void GpuArena::Free(const ArenaAlloc& a_alloc, std::uint64_t a_fence, std::uint64_t* a_counter, std::uint64_t a_bytes)
	{
		if (!a_alloc.Valid())
			return;
		deferred.push_back(Deferred{ a_fence, a_alloc, a_counter, a_bytes });
	}

	void GpuArena::Collect(std::uint64_t a_completed)
	{
		if (deferred.empty())
			return;
		std::erase_if(deferred, [&](const Deferred& a_entry) {
			if (a_entry.fence > a_completed)
				return false;
			const auto& alloc = a_entry.alloc;
			if (a_entry.counter)
				*a_entry.counter -= std::min(*a_entry.counter, a_entry.bytes);
			if (alloc.block < blocks.size() && blocks[alloc.block].live) {
				auto& block = blocks[alloc.block];
				std::uint64_t offset = alloc.offset, size = alloc.size;
				// Coalesce with the neighbours.
				auto next = block.free.lower_bound(offset);
				if (next != block.free.end() && next->first == offset + size) {
					size += next->second;
					next = block.free.erase(next);
				}
				if (next != block.free.begin()) {
					auto prev = std::prev(next);
					if (prev->first + prev->second == offset) {
						offset = prev->first;
						size += prev->second;
						block.free.erase(prev);
					}
				}
				block.free.emplace(offset, size);
				block.used -= std::min(block.used, alloc.size);
			}
			used -= std::min(used, alloc.size);
			return true;
		});
	}

	void GpuArena::Adopt(ComPtr<ID3D12Resource> a_resource, std::byte* a_mapped, std::uint64_t a_size)
	{
		Block block;
		block.va = a_resource->GetGPUVirtualAddress();
		block.resource = std::move(a_resource);
		block.mapped = a_mapped;
		block.size = a_size;
		block.state = createState;
		block.free.emplace(0, a_size);
		block.adoptedQpc = ArenaQpc();
		block.live = true;
		reserved += a_size;
		for (auto& slot : blocks) {
			if (!slot.live && !slot.resource) {
				slot = std::move(block);
				return;
			}
		}
		blocks.push_back(std::move(block));
	}

	void Backend::InitPools()
	{
		auto setup = [](GpuArena& a_pool, const wchar_t* a_name, const char* a_label, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, bool a_uav, std::uint64_t a_block,
						 std::uint64_t a_headroom, std::uint64_t a_transition, std::uint64_t a_max) {
			a_pool.Reset();
			a_pool.name = a_name;
			a_pool.label = a_label;
			a_pool.heap = a_heap;
			a_pool.createState = a_state;
			a_pool.uav = a_uav;
			a_pool.blockBytes = a_block;
			a_pool.alignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;  // 256 for every pool
			a_pool.headroom = a_headroom;
			a_pool.transitionHeadroom = a_transition;
			a_pool.maxBytes = a_max;
		};
		setup(asPool_, L"CS RayTracing BLAS pool", "BLAS", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, 64 * kMB, 48 * kMB, 160 * kMB,
			2048 * kMB);
		setup(uploadPool_, L"CS RayTracing upload pool", "upload", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false, 16 * kMB, 16 * kMB, 48 * kMB, 512 * kMB);
		setup(skinStaticPool_, L"CS RayTracing character vertex pool", "character vertices", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, false, 16 * kMB, 8 * kMB, 32 * kMB,
			512 * kMB);
		setup(skinOutputPool_, L"CS RayTracing character skinned pool", "character skinned vertices", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true, 8 * kMB, 4 * kMB, 16 * kMB,
			256 * kMB);
		postbuildOwner_.assign(kPostbuildSlots, 0);
		stats_.pools = {};
	}

	ComPtr<ID3D12Resource> Backend::CreatePoolBuffer(const GpuArena& a_pool, std::uint64_t a_bytes, std::byte** o_mapped)
	{
		// Thread-safe (the device is free-threaded): called on the pool thread and on the render thread.
		*o_mapped = nullptr;
		auto device = device_;
		if (!device)
			return nullptr;
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = a_pool.heap;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = a_bytes;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = a_pool.uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		ComPtr<ID3D12Resource> resource;
		// Never zeroed: every range is written before it is read (uploads, builds, skinning).
		HRESULT hr = E_FAIL;
		if (notZeroed_.load(std::memory_order_relaxed)) {
			hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_CREATE_NOT_ZEROED, &desc, a_pool.createState, nullptr, IID_PPV_ARGS(&resource));
			if (hr == E_INVALIDARG)
				notZeroed_.store(false, std::memory_order_relaxed);
		}
		if (!resource)
			hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_pool.createState, nullptr, IID_PPV_ARGS(&resource));
		if (FAILED(hr) || !resource)
			return nullptr;
		resource->SetName(a_pool.name);
		// (46p) Counted per owner while it lives (F7 vram_*).
		{
			using Owner = Util::FrameTrace::VramOwner;
			const Owner owner = a_pool.heap == D3D12_HEAP_TYPE_UPLOAD ? Owner::RtUpload : (&a_pool == &asPool_ ? Owner::RtBlas : Owner::RtSkin);
			Util::FrameTrace::TagVram(resource.Get(), owner, (a_bytes + 0xFFFFull) & ~0xFFFFull);
			if (owner != Owner::RtUpload && ResidencyHigh())
				ApplyResidency(resource.Get());  // Keep Our GPU Memory Resident
		}
		if (a_pool.heap == D3D12_HEAP_TYPE_UPLOAD) {
			void* mapped = nullptr;
			if (FAILED(resource->Map(0, nullptr, &mapped)) || !mapped)
				return nullptr;
			*o_mapped = static_cast<std::byte*>(mapped);
		}
		return resource;
	}

	void Backend::PumpPools()
	{
		if (!ready_ || !fence_)
			return;
		const std::int64_t start = ArenaQpc();
		auto& ps = stats_.pools;
		const std::uint64_t completed = fence_->GetCompletedValue();
		auto pools = Pools();
		for (auto* pool : pools)
			pool->Collect(completed);

		// Blocks the pool thread finished.
		std::vector<PoolResult> results;
		{
			std::scoped_lock lock(poolMutex_);
			results.swap(poolResults_);
		}
		for (auto& result : results) {
			auto& pool = *pools[result.pool];
			pool.requestInFlight = false;
			if (!result.resource) {
				if (!pool.failed)
					logger::warn("[RayTracing] Pool: a {:.0f} MB block of '{}' could not be created", static_cast<double>(result.bytes) / kMB,
						pool.label);
				pool.failed = true;
				continue;
			}
			pool.Adopt(std::move(result.resource), result.mapped, result.bytes);
			if (pool.wantBytes <= result.bytes)
				pool.wantBytes = 0;
			++ps.asyncBlocksTotal;
		}

		// Growth: a request that did not fit, or less free room than the headroom (pools in use only).
		double syncMs = 0.0;
		for (std::uint32_t i = 0; i < pools.size(); ++i) {
			auto& pool = *pools[i];
			if (pool.failed)
				continue;
			const std::uint64_t target = transition_ ? pool.transitionHeadroom : pool.headroom;
			const bool need = pool.wantBytes > 0 || (pool.reserved > 0 && pool.FreeBytes() < target);
			if (!need)
				continue;
			const std::uint64_t bytes = AlignArena(std::max(pool.blockBytes, pool.wantBytes), kBlockGranularity);
			if (pool.reserved + bytes > pool.maxBytes)
				continue;  // full: allocations keep failing (the owner counts them as over budget)
			if (transition_) {
				// Nothing is visible: made now, within a per-frame budget.
				if (syncMs >= kSyncCreateBudgetMs || pool.requestInFlight)
					continue;
				const std::int64_t t0 = ArenaQpc();
				std::byte* mapped = nullptr;
				auto resource = CreatePoolBuffer(pool, bytes, &mapped);
				syncMs += ArenaQpcToMs(ArenaQpc() - t0);
				if (!resource) {
					logger::warn("[RayTracing] Pool: a {:.0f} MB block of '{}' could not be created", static_cast<double>(bytes) / kMB,
						pool.label);
					pool.failed = true;
					continue;
				}
				pool.Adopt(std::move(resource), mapped, bytes);
				if (pool.wantBytes <= bytes)
					pool.wantBytes = 0;
				++ps.syncBlocksLastFrame;
				++ps.syncBlocksTotal;
			} else if (!pool.requestInFlight) {
				pool.requestInFlight = true;
				{
					std::scoped_lock lock(poolMutex_);
					if (!poolThread_.joinable()) {
						poolStop_ = false;
						poolThread_ = std::thread([this] { PoolThreadMain(); });
					}
					poolRequests_.push_back(PoolRequest{ i, bytes });
				}
				poolCv_.notify_one();
			}
		}

		// (46i) A waiting allocation in the log (once per wait): the diagnostic for "loading the scene" for ever.
		const std::int64_t now = ArenaQpc();
		for (auto* pool : pools) {
			if (pool->wantBytes == 0) {
				pool->waitSinceQpc = 0;
				pool->waitLogged = false;
				continue;
			}
			if (pool->waitSinceQpc == 0)
				pool->waitSinceQpc = now;
			const double waitedMs = ArenaQpcToMs(now - pool->waitSinceQpc);
			if (!pool->waitLogged && waitedMs > kWaitLogMs) {
				pool->waitLogged = true;
				logger::warn("[RayTracing] Pool '{}': an allocation of {:.1f} MB has waited {:.0f} ms for a block ({:.0f} MB reserved, {:.0f} MB in use, request in flight {}, "
							 "failed {}, transition {}, {} blocks released so far)",
					pool->label, static_cast<double>(pool->wantBytes) / kMB, waitedMs, static_cast<double>(pool->reserved) / kMB,
					static_cast<double>(pool->used) / kMB, pool->requestInFlight, pool->failed, transition_, ps.blocksReleased);
			}
		}

		// Empty blocks past twice the headroom go (one per upkeep, never behind loading screens: the destination is
		// about to fill them). A pool nothing has used for a while (Ray Tracing off, characters off) gives everything back.
		// (46i) Never a block adopted moments ago, never while the pool waits for growth: 46g released a pool's only
		// block in the upkeep that adopted it whenever the pool held no allocation yet (after a switch off / on, or the
		// upload pool once its last uploads retired) -- the allocation failed again, a new block came, went again: the
		// build queue waited for ever ("loading the scene").
		if (!transition_) {
			for (auto* pool : pools) {
				if (pool->wantBytes > 0 || pool->requestInFlight)
					continue;
				bool released = false;
				const bool unused = pool->lastAskQpc == 0 || ArenaQpcToMs(now - pool->lastAskQpc) > kIdleReleaseMs;
				for (auto& block : pool->blocks) {
					if (!block.live || block.used != 0)
						continue;
					if (ArenaQpcToMs(now - block.adoptedQpc) < kMinBlockAgeMs)
						continue;
					const bool idle = pool->used == 0 && unused;
					if (!idle && pool->FreeBytes() < block.size + 2 * pool->headroom)
						continue;
					pool->reserved -= std::min(pool->reserved, block.size);
					Retire(block.resource, 0, nullptr);
					block = {};
					++ps.blocksReleased;
					released = true;
					break;
				}
				if (released)
					break;
			}
		}

		ps.reservedBytes = ps.usedBytes = 0;
		ps.blocks = 0;
		for (auto* pool : pools) {
			ps.reservedBytes += pool->reserved;
			ps.usedBytes += pool->used;
			for (const auto& block : pool->blocks)
				ps.blocks += block.live ? 1 : 0;
		}
		ps.workerLastMs = poolWorkerMs_.load(std::memory_order_relaxed);
		ps.pumpMsLastFrame += static_cast<float>(ArenaQpcToMs(ArenaQpc() - start));
	}

	void Backend::TransitionBlocks(ID3D12GraphicsCommandList4* a_list, GpuArena& a_pool, const std::vector<std::uint32_t>& a_blocks, D3D12_RESOURCE_STATES a_state)
	{
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		for (const std::uint32_t b : a_blocks) {
			if (b >= a_pool.blocks.size() || !a_pool.blocks[b].live)
				continue;
			auto& block = a_pool.blocks[b];
			if (block.state == a_state)
				continue;
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = block.resource.Get();
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = block.state;
			barrier.Transition.StateAfter = a_state;
			barriers.push_back(barrier);
			block.state = a_state;
		}
		if (!barriers.empty())
			a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
	}

	void Backend::PoolThreadMain()
	{
		SetThreadDescription(GetCurrentThread(), L"CS RayTracing pools");
		for (;;) {
			PoolRequest request{};
			{
				std::unique_lock lock(poolMutex_);
				poolCv_.wait(lock, [this] { return poolStop_ || !poolRequests_.empty(); });
				if (poolStop_)
					return;
				request = poolRequests_.front();
				poolRequests_.erase(poolRequests_.begin());
			}
			const std::int64_t start = ArenaQpc();
			auto pools = Pools();
			std::byte* mapped = nullptr;
			auto resource = CreatePoolBuffer(*pools[request.pool], request.bytes, &mapped);
			poolWorkerMs_.store(static_cast<float>(ArenaQpcToMs(ArenaQpc() - start)), std::memory_order_relaxed);
			std::scoped_lock lock(poolMutex_);
			poolResults_.push_back(PoolResult{ request.pool, request.bytes, std::move(resource), mapped });
		}
	}

	void Backend::OnSwitchedOff()
	{
		transition_ = false;
		for (auto* pool : Pools()) {
			pool->wantBytes = 0;     // growth for the released scene is not wanted any more (a block in flight is adopted, then goes)
			pool->lastAskQpc = 0;    // unused from now on: empty blocks go once their ranges are retired
			pool->waitSinceQpc = 0;
			pool->waitLogged = false;
			pool->failed = false;    // switching on again is a fresh attempt
		}
	}

	std::string Backend::PoolSummary() const
	{
		std::string line;
		const std::int64_t now = ArenaQpc();
		for (const auto* pool : Pools()) {
			std::uint32_t blocks = 0;
			for (const auto& block : pool->blocks)
				blocks += block.live ? 1 : 0;
			line += std::format("{}{} {:.0f}/{:.0f} MB in {} blocks", line.empty() ? "" : ", ", pool->label, static_cast<double>(pool->used) / kMB,
				static_cast<double>(pool->reserved) / kMB, blocks);
			if (pool->wantBytes > 0)
				line += std::format(" (waiting {:.0f} ms for {:.1f} MB{})", pool->waitSinceQpc ? ArenaQpcToMs(now - pool->waitSinceQpc) : 0.0,
					static_cast<double>(pool->wantBytes) / kMB, pool->requestInFlight ? ", block in the making" : "");
			if (pool->failed)
				line += " (failed)";
		}
		return line;
	}

	double Backend::PoolWaitMs() const
	{
		const std::int64_t now = ArenaQpc();
		double waited = 0.0;
		for (const auto* pool : Pools())
			if (pool->wantBytes > 0 && pool->waitSinceQpc != 0)
				waited = std::max(waited, ArenaQpcToMs(now - pool->waitSinceQpc));
		return waited;
	}

	void Backend::StopPoolThread()
	{
		{
			std::scoped_lock lock(poolMutex_);
			poolStop_ = true;
		}
		poolCv_.notify_one();
		if (poolThread_.joinable())
			poolThread_.join();  // a creation in progress finishes first (bounded)
		std::scoped_lock lock(poolMutex_);
		poolRequests_.clear();
		poolResults_.clear();
		poolStop_ = false;
	}

	void Backend::Prewarm(float a_cpuMs, bool a_characters)
	{
		if (!ready_ || failed_)
			return;
		const std::int64_t start = ArenaQpc();
		auto inBudget = [&]() { return ArenaQpcToMs(ArenaQpc() - start) < a_cpuMs; };
		// Pools in use (or about to be) get their first block now; PumpPools grows them to the transition headroom.
		for (auto* pool : { &asPool_, &uploadPool_ })
			if (pool->reserved == 0 && pool->wantBytes == 0)
				pool->wantBytes = pool->blockBytes;
		if (a_characters && SkinAvailable()) {
			for (auto* pool : { &skinStaticPool_, &skinOutputPool_ })
				if (pool->reserved == 0 && pool->wantBytes == 0)
					pool->wantBytes = pool->blockBytes;
		}
		PumpPools();
		// Shared scratch buffers and the TLAS sized for a full scene (each grows on the render thread otherwise).
		if (inBudget() && scratchBytes_ < (32ull << 20) && !EnsureScratch(32ull << 20))
			return;
		if (inBudget() && tlasCapacity_ < 8192 && !EnsureTlas(8192))
			return;
		if (a_characters && SkinAvailable() && inBudget() && skinScratchBytes_ < (16ull << 20)) {
			auto scratch = CreateBuffer(16ull << 20, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, Util::FrameTrace::VramOwner::RtScratch);
			if (scratch) {
				if (skinScratch_)
					Retire(skinScratch_, skinScratchBytes_, &skinStats_.scratchBytes);
				skinScratch_ = std::move(scratch);
				skinScratch_->SetName(L"CS RayTracing character scratch");
				skinScratchBytes_ = 16ull << 20;
				skinStats_.scratchBytes += skinScratchBytes_;
			}
		}
		// Per-context upload buffers of the free contexts (a context in flight keeps its own until it is free).
		const std::uint64_t completed = fence_->GetCompletedValue();
		const std::uint64_t aoCompleted = AOCompleted();
		for (std::size_t i = 0; i < kContexts && inBudget(); ++i) {
			auto& context = contexts_[i];
			if ((prepared_ && preparedContext_ == i) || !ContextFree(context, completed, aoCompleted))
				continue;
			if (!EnsureInstanceBuffer(context, 8192))
				return;
			if (a_characters && SkinAvailable() && context.boneCapacity < (2ull << 20)) {
				auto bones = CreateBuffer(2ull << 20, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
				void* mapped = nullptr;
				if (bones && SUCCEEDED(bones->Map(0, nullptr, &mapped)) && mapped) {
					if (context.boneBuffer) {
						if (context.boneMapped)
							context.boneBuffer->Unmap(0, nullptr);
						skinStats_.boneBytes -= std::min(skinStats_.boneBytes, context.boneCapacity);
					}
					context.boneBuffer = std::move(bones);
					context.boneBuffer->SetName(L"CS RayTracing character bones");
					context.boneMapped = mapped;
					context.boneCapacity = 2ull << 20;
					skinStats_.boneBytes += context.boneCapacity;
				}
			}
		}
	}
}
