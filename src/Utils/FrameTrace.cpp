#include "Utils/FrameTrace.h"

#include "Features/ExponentialHeightFog.h"
#include "Features/PerformanceOverlay.h"
#include "Features/RayTracing.h"
#include "Features/ScreenSpaceGI.h"
#include "Features/ScreenSpacePointLightShadows.h"
#include "Features/ScreenSpaceRayTracing.h"
#include "Features/Upscaling.h"
#include "Menu.h"
#include "State.h"
#include "BuildLabel.h"
#include "Utils/EngineTiming.h"
#include "Utils/GpuTimers.h"
#include "Utils/LatencyPacer.h"
#include "Utils/PerfOptions.h"
#include "Utils/WorldTiming.h"

#include <d3d12.h>  // (46p) TagVram on D3D12 objects
#include <algorithm>
#include <deque>
#include <intrin.h>
#include <mutex>
#include <new>
#include <set>
#include <thread>

namespace Util::FrameTrace
{
	namespace
	{
		constexpr int kCalls = static_cast<int>(DriverCall::Count);

		int64_t g_frequency = 0;
		double g_toMs = 0.0;

		// Latched once per frame (OnPresentEnd): every timed call then costs one branch when off.
		bool g_active = false;

		struct FrameAcc
		{
			double driver[kCalls]{};
			double driverTotal = 0.0;
			double ownDrawHook = 0.0;
			double ownSetupGeometry = 0.0;
			double permutationUpload = 0.0;
			double reflexSleep = 0.0;
			double pacerSleep = 0.0;  // (45m) frame-start pacer (predictive sleep + hard bound)
			uint32_t draws = 0;
			uint32_t lightingDraws = 0;
			uint32_t utilityDraws = 0;
			uint32_t firstSeen = 0;
			uint32_t fgPresented = 0;
			float grassApplyMs = -1.0f;
			float rtGpuMs = -1.0f;  // (45) Ray Tracing: D3D12 GPU ms (last completed), CPU ms, BLAS builds
			float rtCpuMs = -1.0f;
			uint32_t rtBuilds = 0;
			RayTracingDetail rt{};  // (45j)
			LightStats lights{};    // (45o)
			// (45j) Map attribution: calls, slow calls (> 0.5 ms), the slowest one's resource and caller.
			uint32_t mapCalls = 0;
			uint32_t mapSlow = 0;
			double mapWorstMs = 0.0;
			const void* mapWorstCaller = nullptr;
			char mapWorstResource[96]{};
			double driverMisc = 0.0;  // (46l) GetData, Clear*, GenerateMips, ResolveSubresource
			uint32_t driverMiscCalls = 0;
			// (46o) Present / upscaling sub-timers (-1 = not run this frame).
			float split[static_cast<int>(Split::Count)]{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
		};
		static_assert(static_cast<int>(Split::Count) == 16);
		FrameAcc g_frame;
		Q46 g_q46{};  // (46q) the latest state of the 46q switches (snow ms: this frame)
		std::atomic<uint32_t> g_vramWarn{ 0 };  // (46o) M3: Streamline eWarnOutOfVRAM since the last row

		// (46o) M3: video memory of the D3D11 adapter (and of the D3D12 device's, if it is another one), MB.
		struct Vram
		{
			float budget = -1.0f, usage = -1.0f, nonLocal = -1.0f, usage12 = -1.0f;
		};
		Vram g_vram;
		uint32_t g_vramFrame = 0;
		winrt::com_ptr<IDXGIAdapter3> g_adapter11, g_adapter12;
		bool g_adaptersLooked = false;
		bool g_adapter12Same = true;
		uint64_t g_adapterTotalMb = 0;

		void LookUpAdapters()
		{
			if (g_adaptersLooked)
				return;
			g_adaptersLooked = true;
			try {
				winrt::com_ptr<IDXGIDevice> dxgiDevice;
				if (!globals::d3d::device || FAILED(globals::d3d::device->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()))))
					return;
				winrt::com_ptr<IDXGIAdapter> adapter;
				if (FAILED(dxgiDevice->GetAdapter(adapter.put())))
					return;
				adapter->QueryInterface(IID_PPV_ARGS(g_adapter11.put()));
				DXGI_ADAPTER_DESC desc{};
				if (FAILED(adapter->GetDesc(&desc)))
					return;
				g_adapterTotalMb = desc.DedicatedVideoMemory / (1024ull * 1024ull);
				auto* device12 = Upscaling::dx12SwapChain.d3d12Device.get();
				if (!device12)
					return;
				const LUID luid12 = device12->GetAdapterLuid();
				if (luid12.LowPart == desc.AdapterLuid.LowPart && luid12.HighPart == desc.AdapterLuid.HighPart)
					return;
				g_adapter12Same = false;
				winrt::com_ptr<IDXGIFactory4> factory;
				if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(factory.put()))))
					factory->EnumAdapterByLuid(luid12, IID_PPV_ARGS(g_adapter12.put()));
			} catch (...) {
				g_adapter11 = nullptr;
				g_adapter12 = nullptr;
			}
		}

		Vram QueryVram()
		{
			Vram v;
			LookUpAdapters();
			constexpr float kMb = 1.0f / (1024.0f * 1024.0f);
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (g_adapter11 && SUCCEEDED(g_adapter11->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
				v.budget = static_cast<float>(info.Budget) * kMb;
				v.usage = static_cast<float>(info.CurrentUsage) * kMb;
			}
			if (g_adapter11 && SUCCEEDED(g_adapter11->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &info)))
				v.nonLocal = static_cast<float>(info.CurrentUsage) * kMb;
			if (g_adapter12 && SUCCEEDED(g_adapter12->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				v.usage12 = static_cast<float>(info.CurrentUsage) * kMb;
			return v;
		}

		// ---- (46p) video memory by owner (cumulative bytes since the hooks went in; read per row while recording) ----
		constexpr int kVramOwners = static_cast<int>(VramOwner::Count);
		std::array<std::atomic<uint64_t>, kVramOwners> g_vramNew{};
		std::array<std::atomic<uint64_t>, kVramOwners> g_vramFreed{};
		// {6E3D0F2A-91B4-4C7D-8E25-3A9F1C64B70D}: our private-data slot on tagged resources.
		constexpr GUID kVramTagGuid = { 0x6e3d0f2a, 0x91b4, 0x4c7d, { 0x8e, 0x25, 0x3a, 0x9f, 0x1c, 0x64, 0xb7, 0x0d } };

		/// Bytes alive per feature of ours (entries never go away: a deque keeps their addresses).
		struct FeatureVram
		{
			std::string name;
			std::atomic<int64_t> live{ 0 };
			explicit FeatureVram(std::string_view a_name) :
				name(a_name) {}
		};
		// Never destroyed (heap, leaked on purpose): resources may be released after our statics at process exit, and their
		// tags still reach these.
		std::mutex& g_featureMutex = *new std::mutex();
		std::deque<FeatureVram>& g_featureVram = *new std::deque<FeatureVram>();
		std::set<std::string, std::less<>>& g_scopeNames = *new std::set<std::string, std::less<>>();
		thread_local std::string_view t_vramScope;
		// (46q) The pass scopes open on this thread (CpuPassScope / EngineTiming::PassScope push their key whether or not timing runs):
		// a creation outside a Setup scope is billed to the innermost one.
		constexpr int kScopeStack = 16;
		thread_local std::array<std::string_view, kScopeStack> t_scopeStack{};
		thread_local int t_scopeDepth = 0;

		FeatureVram* FeatureEntry(std::string_view a_name)
		{
			std::scoped_lock lock(g_featureMutex);
			for (auto& entry : g_featureVram)
				if (entry.name == a_name)
					return &entry;
			return &g_featureVram.emplace_back(a_name);
		}

		/// Held by the resource as private data: the runtime releases it when the resource is destroyed.
		class VramTag final : public IUnknown
		{
		public:
			VramTag(int a_owner, uint64_t a_bytes, FeatureVram* a_feature) :
				owner(a_owner), bytes(a_bytes), feature(a_feature) {}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID a_riid, void** a_out) override
			{
				if (!a_out)
					return E_POINTER;
				if (IsEqualIID(a_riid, __uuidof(IUnknown))) {
					*a_out = static_cast<IUnknown*>(this);
					AddRef();
					return S_OK;
				}
				*a_out = nullptr;
				return E_NOINTERFACE;
			}
			ULONG STDMETHODCALLTYPE AddRef() override { return refs.fetch_add(1, std::memory_order_relaxed) + 1; }
			ULONG STDMETHODCALLTYPE Release() override
			{
				const ULONG left = refs.fetch_sub(1, std::memory_order_acq_rel) - 1;
				if (left == 0) {
					if (counted) {
						g_vramFreed[owner].fetch_add(bytes, std::memory_order_relaxed);
						if (feature)
							feature->live.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
					}
					delete this;
				}
				return left;
			}

			bool counted = false;

			/// (46q) Bills the bytes to another feature (a resource named after its feature, created where no scope was open).
			void Rebill(FeatureVram* a_to)
			{
				if (!counted || !a_to || a_to == feature)
					return;
				if (feature)
					feature->live.fetch_sub(static_cast<int64_t>(bytes), std::memory_order_relaxed);
				a_to->live.fetch_add(static_cast<int64_t>(bytes), std::memory_order_relaxed);
				feature = a_to;
			}
			[[nodiscard]] FeatureVram* Feature() const { return feature; }

		private:
			std::atomic<ULONG> refs{ 1 };
			int owner;
			uint64_t bytes;
			FeatureVram* feature;
		};

		/// (46q) A pass key as a feature name: "g.SSRTReblur" -> "SSRTReblur", "Upscaling.Streamline" -> "Upscaling".
		std::string_view FeatureNameOfKey(std::string_view a_key)
		{
			if (a_key.starts_with("g."))
				a_key.remove_prefix(2);
			const size_t dot = a_key.find('.');
			return dot == std::string_view::npos ? a_key : a_key.substr(0, dot);
		}

		constexpr std::string_view kVramNoScope = "other (no scope open)";

		/// (46q) "name=MB;..." alive per feature of ours (1 MB or more), largest first: the per-frame F7 column.
		std::string VramByFeatureColumn()
		{
			std::vector<std::pair<int64_t, std::string_view>> rows;
			{
				std::scoped_lock lock(g_featureMutex);
				for (const auto& entry : g_featureVram) {
					const int64_t live = entry.live.load(std::memory_order_relaxed);
					if (live >= (1ll << 20))
						rows.emplace_back(live, entry.name);
				}
			}
			std::sort(rows.begin(), rows.end(), [](const auto& a_l, const auto& a_r) { return a_l.first > a_r.first; });
			std::string text;
			for (const auto& [live, name] : rows)
				text += std::format("{}{}={:.0f}", text.empty() ? "" : ";", name, static_cast<double>(live) / (1024.0 * 1024.0));
			return text;
		}

		template <class T>
		void TagObject(T* a_object, VramOwner a_owner, uint64_t a_bytes, FeatureVram* a_feature = nullptr)
		{
			const int owner = static_cast<int>(a_owner);
			if (!a_object || a_bytes == 0 || owner < 0 || owner >= kVramOwners)
				return;
			auto* tag = new (std::nothrow) VramTag(owner, a_bytes, a_feature);
			if (!tag)
				return;
			if (SUCCEEDED(a_object->SetPrivateDataInterface(kVramTagGuid, tag))) {
				tag->counted = true;
				g_vramNew[owner].fetch_add(a_bytes, std::memory_order_relaxed);
				if (a_feature)
					a_feature->live.fetch_add(static_cast<int64_t>(a_bytes), std::memory_order_relaxed);
			}
			tag->Release();  // the resource holds it now (or nobody: then it goes uncounted)
		}

		struct ModuleRange
		{
			uintptr_t base = 0, end = 0;
			[[nodiscard]] bool Holds(const void* a_address) const
			{
				const auto a = reinterpret_cast<uintptr_t>(a_address);
				return a >= base && a < end;
			}
		};

		ModuleRange RangeOf(HMODULE a_module)
		{
			if (!a_module)
				return {};
			const auto base = reinterpret_cast<uintptr_t>(a_module);
			const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
			if (dos->e_magic != IMAGE_DOS_SIGNATURE)
				return {};
			const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + static_cast<uintptr_t>(dos->e_lfanew));
			return { base, base + nt->OptionalHeader.SizeOfImage };
		}

		VramOwner OwnerOfCaller(const void* a_caller, bool a_shared)
		{
			static const ModuleRange ours = [] {
				HMODULE module = nullptr;
				GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&g_vramNew), &module);
				return RangeOf(module);
			}();
			static const ModuleRange exe = RangeOf(GetModuleHandleW(nullptr));
			if (ours.Holds(a_caller))
				return a_shared ? VramOwner::D3D11Shared : VramOwner::D3D11Ours;
			if (exe.Holds(a_caller))
				return VramOwner::D3D11Engine;
			return VramOwner::D3D11Other;
		}

		/// Owner groups for the columns: ours in video memory, the engine, other modules, system memory (upload / read-back / staging).
		enum class VramGroup : int
		{
			Ours,
			Engine,
			Other,
			SysMem,
			Count
		};
		constexpr int kVramGroups = static_cast<int>(VramGroup::Count);
		VramGroup GroupOf(int a_owner)
		{
			switch (static_cast<VramOwner>(a_owner)) {
			case VramOwner::D3D11Engine:
				return VramGroup::Engine;
			case VramOwner::D3D11Other:
				return VramGroup::Other;
			case VramOwner::RtUpload:
			case VramOwner::RtReadback:
			case VramOwner::D3D11Staging:
				return VramGroup::SysMem;
			default:
				return VramGroup::Ours;
			}
		}

		/// A snapshot of the cumulative counters (and what goes with it) per recorded row, for the deltas and the jump log.
		struct VramSnap
		{
			std::array<uint64_t, kVramOwners> created{}, freed{};
			float usage = -1.0f, budget = -1.0f, nonLocal = -1.0f;
			uint32_t createsOther = 0;  // the engine's device Create* calls on other threads that frame (EngineTiming)
			float createOtherMs = 0.0f;
			size_t row = 0;
		};
		VramSnap TakeVramSnap()
		{
			VramSnap s;
			for (int i = 0; i < kVramOwners; ++i) {
				s.created[i] = g_vramNew[i].load(std::memory_order_relaxed);
				s.freed[i] = g_vramFreed[i].load(std::memory_order_relaxed);
			}
			return s;
		}
		constexpr size_t kVramWindow = 5;          // the row and the 4 before it
		constexpr float kVramJumpMb = 500.0f;      // a usage change this large within 4 frames is logged
		std::array<VramSnap, kVramWindow> g_vramWin{};
		size_t g_vramWinCount = 0, g_vramWinHead = 0;
		int g_vramJumpCooldown = 0;
		uint32_t g_vramJumpLogs = 0;
		VramSnap g_vramPrev{};
		bool g_vramPrevValid = false;

		/// (46o) M6: minutes since the game process started.
		double SessionMinutes()
		{
			FILETIME created{}, exited{}, kernel{}, user{};
			if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user))
				return -1.0;
			FILETIME now{};
			GetSystemTimeAsFileTime(&now);
			const auto toU64 = [](const FILETIME& a_t) { return (static_cast<uint64_t>(a_t.dwHighDateTime) << 32) | a_t.dwLowDateTime; };
			return static_cast<double>(toU64(now) - toU64(created)) / 1e7 / 60.0;
		}

		// (46o) M6 frame_flags bits.
		constexpr uint32_t kFlagLoading = 1u, kFlagMap = 2u, kFlagPaused = 4u, kFlagMenu = 8u, kFlagFader = 16u, kFlagPacerReset = 32u;
		int64_t g_simStart = 0;
		std::atomic<uint32_t> g_shaderAsync{ 0 }, g_shaderSync{ 0 }, g_cellAttach{ 0 }, g_cellDetach{ 0 };
		std::atomic<int64_t> g_mapOtherTicks{ 0 };  // (46l) Map on other contexts
		std::atomic<uint32_t> g_mapOtherCalls{ 0 };
		std::unordered_set<const void*> g_seenGeometry;
		bool g_cellSinkRegistered = false;
		float g_lastYaw = 0.0f, g_lastPitch = 0.0f;
		RE::NiPoint3 g_lastPlayerPos{};
		bool g_motionValid = false;

		// Overlay series: one entry per real frame while active.
		struct HistEntry
		{
			int64_t t = 0;
			float cpuMs = 0.0f;
			float gpuMs = -1.0f;
			uint64_t gpuIndex = 0;
			float gpuWaitMs = 0.0f;  // (47h) of cpuMs, time the CPU spent waiting for the GPU (LatencyPacer::FrameGpuWaitMs)
		};
		constexpr size_t kHist = 4096;
		std::array<HistEntry, kHist> g_hist{};
		size_t g_histHead = 0;  // next write
		size_t g_histCount = 0;

		// ---- (47p) periodic-job stamps and the GPU spike watch ----
		std::atomic<uint32_t> g_jobsThisFrame{ 0 };
		struct JobFrame
		{
			uint64_t gpuIndex = UINT64_MAX;
			uint32_t jobs = 0;
		};
		constexpr size_t kJobRing = 64;
		std::array<JobFrame, kJobRing> g_jobRing{};  // written every frame (OnPresentEnd), render thread
		size_t g_jobRingHead = 0;

		constexpr const char* kJobNames[] = { "fg_status", "hdr_query", "present_probe", "settings_save", "overlay_update", "skin_water",
			"rt_persistent", "rt_pool_release", "light_census", "shader_compiled" };
		static_assert(std::size(kJobNames) == static_cast<size_t>(Job::Count));

		std::string JobNames(uint32_t a_jobs)
		{
			std::string s;
			for (uint32_t i = 0; i < static_cast<uint32_t>(Job::Count); ++i) {
				if (!(a_jobs & (1u << i)))
					continue;
				if (!s.empty())
					s += '|';
				s += kJobNames[i];
			}
			return s;
		}

		uint32_t JobsOfGpuFrame(uint64_t a_gpuIndex)
		{
			for (const auto& f : g_jobRing)
				if (f.gpuIndex == a_gpuIndex)
					return f.jobs;
			return 0;
		}

		// The watch learns the usual GPU work on frames without a spike; a spike is 12 ms+ and 1.4x over it.
		struct SpikeWatch
		{
			float usualMs = 0.0f;
			int learned = 0;
			int64_t lastSpikeQpc = 0;
			std::array<float, 4> intervals{};  // seconds between the last spikes, newest first
			int intervalCount = 0;
			int logs = 0;
			bool periodicLogged = false;
		} g_spike;
		constexpr int kSpikeLogLimit = 60;

		struct CellSink : RE::BSTEventSink<RE::TESCellAttachDetachEvent>
		{
			RE::BSEventNotifyControl ProcessEvent(const RE::TESCellAttachDetachEvent* a_event, RE::BSTEventSource<RE::TESCellAttachDetachEvent>*) override
			{
				if (a_event)
					(a_event->attached ? g_cellAttach : g_cellDetach).fetch_add(1, std::memory_order_relaxed);
				return RE::BSEventNotifyControl::kContinue;
			}
		};
		CellSink g_cellSink;
		int64_t g_lastPresentEnd = 0;

		Smoothed g_smoothed;
		uint32_t g_frameCounter = 0;

		// ---- recorder ----
		struct Row
		{
			double tMs = 0.0;
			float frameMs = 0.0f;
			float presentWaitMs = 0.0f;
			float reflexSleepMs = 0.0f;
			float pacerSleepMs = 0.0f;  // (45m)
			float driver[kCalls]{};
			float ownDrawHookMs = 0.0f;
			float ownSetupGeometryMs = 0.0f;
			float permutationUploadMs = 0.0f;
			uint32_t draws = 0;
			uint32_t lightingDraws = 0;
			uint32_t utilityDraws = 0;
			uint32_t firstSeen = 0;
			uint32_t shaderAsync = 0;
			uint32_t shaderSync = 0;
			uint32_t cellAttach = 0;
			uint32_t cellDetach = 0;
			float cpuFrameMs = -1.0f;
			float yawRate = 0.0f;
			float pitchRate = 0.0f;
			float playerSpeed = 0.0f;
			uint32_t fgPresented = 0;
			float grassApplyMs = -1.0f;
			float rtGpuMs = -1.0f;
			float rtCpuMs = -1.0f;
			uint32_t rtBuilds = 0;
			RayTracingDetail rt{};  // (45j)
			LightStats lights{};    // (45o)
			uint32_t mapCalls = 0, mapSlow = 0;
			float mapWorstMs = 0.0f;
			std::string mapWorstResource, mapWorstCaller;
			std::shared_ptr<const Location> location;  // (45j) shared while it does not change
			std::string cpuTop;  // (45j) our costliest CPU features this frame
			uint64_t gpuFrameIndex = 0;
			float gpuWorkMs = -1.0f;
			float gpuElapsedMs = -1.0f;
			uint32_t reflexFrame = UINT32_MAX;
			// (44f) Engine CPU split, engine time step, device Create* calls.
			EngineTiming::Frame engine;
			bool reflexMarkersAtRenderStart = false;
			// (45m) Reflex / pacer state for this frame.
			LatencyPacer::FrameInfo pacer{};
			bool reflexSleepOk = false;
			int lowLatencyAvailable = -1;
			int reflexModeApplied = -1;
			int reflexLimitApplied = -1;
			bool latencyFix = false;
			// (46e) Screen-space GI / reflections per pass (last GPU samples, -1 = the pass is off): SSRT REBLUR,
			// SSRT diffuse trace, SSRT specular trace, SSGI, SSGI contact AO.
			float ssPassMs[5]{ -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
			// (47f) Volumetric fog per pass: depth, medium, lighting near, lighting far, integration, history (-1 = not built).
			float vfPassMs[6]{ -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };
			uint32_t vfGrid[4]{};  // near x, y, z, far z (0 = far grid off)
			// (46l) World-render split, other blocking D3D11 calls, Map on other contexts.
			WorldTiming::Frame world;
			float driverMiscMs = 0.0f;
			uint32_t driverMiscCalls = 0;
			float mapOtherMs = 0.0f;
			uint32_t mapOtherCalls = 0;
			// (46n) Frames since the loading screen closed (0 = first frame after it; -1 = outside the post-load window).
			int passSinceLoad = -1;
			bool passLogPending = false;
			// (46o) Present / upscaling sub-timers, video memory, Streamline out-of-VRAM warnings, frame flags, stall samples.
			float split[static_cast<int>(Split::Count)]{};
			Vram vram;
			uint32_t vramWarn = 0;
			uint32_t flags = 0;
			std::string stallStack;
			// (46p) The VRAM query's cost, the usage change since the last row, bytes created / destroyed this frame per owner (MB),
			// alive per group (MB, tagged since the hooks went in); GPU ms per engine segment (-1 = none), the rest, total, stamps.
			float vramQueryUs = -1.0f;
			float vramDeltaMb = 0.0f;
			std::array<float, kVramOwners> vramNewMb{}, vramFreedMb{};
			std::array<float, kVramGroups> vramLiveMb{};
			float gpuSegMs[static_cast<int>(EngineTiming::Seg::Count)]{ -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
			float gpuSegOtherMs = -1.0f;
			float gpuSegTotalMs = -1.0f;
			int gpuSegStamps = -1;
			float gpuWaterMs = -1.0f;      // (47j) GPU wall time of the water draws (stamp pairs; -1 = not read back)
			float waterPrepGpuMs = -1.0f;  // (47j) WaterPrepCS (GpuPassTimers RayTracingWaterPrep)
			float aoNormalPrepGpuMs = -1.0f;  // (47l) AONormalPrepCS (GpuPassTimers RayTracingAONormalPrep)
			float postGpuMs = -1.0f;  // (46p) our post-processing chain, last GPU sample (GpuPassTimers PostProcessing)
			float gpuSpikeMs = 0.0f;  // (47p) GPU work over the usual (spike watch), 0 = none
			uint32_t jobs = 0;        // (47p) our periodic jobs that ran in this frame (bit per FrameTrace::Job)
			Q46 q46{};             // (46q)
			std::string vramByFeature;  // (46q) "name=MB;..." alive per feature of ours (1 MB or more)
			// (46u) PC latency estimate inputs taken on the frame (the Reflex report arrives later).
			float pclFgMult = 1.0f;
			float pclRefreshHz = 0.0f;
		};

		struct ReflexTimes
		{
			uint64_t simStart, simEnd, submitStart, submitEnd, presentStart, presentEnd, driverStart, driverEnd, queueStart, queueEnd, gpuStart, gpuEnd;
			uint32_t gpuActiveUs, gpuFrameUs;  // (45m) what the driver counts as GPU work for the frame
			uint64_t inputSample;              // (46u) 0 = not reported
		};

		bool g_recording = false;
		uint32_t g_worldSpikeLogs = 0;  // (46l) world-spike log lines this recording (capped)
		// (46n) cs_passes split: the post-load window, GPU samples that arrived before their row, rows waiting to be logged.
		constexpr int kPassWindowFrames = 120;
		constexpr size_t kPassLogDelayRows = 8;  // GPU samples come back 1-5 frames late
		constexpr uint32_t kMaxPassLogs = 300;
		bool g_loadingWasOpen = false;
		int g_passSinceLoad = -1;
		uint32_t g_passLogs = 0;
		struct PendingPassGpu
		{
			uint64_t frame;
			int bucket;
			float ms;
		};
		std::vector<PendingPassGpu> g_pendingPassGpu;
		std::vector<size_t> g_passLogQueue;
		// (45j) Taken when a recording starts (the CSV is written on another thread).
		std::string g_headerLocation, g_headerRayTracing, g_headerTag, g_headerLatency;
		std::string g_headerRendering;  // (46e) upscaler, resolutions, SSRT / SSGI
		std::string g_header46o;        // (46o) session uptime, video memory, thread CPU time calibration, stall sampler
		std::string g_headerFG;         // (46w) frame-generation DLL and MFG Unlocker preset
		int64_t g_recordStart = 0;
		int64_t g_recordEnd = 0;
		std::vector<Row> g_rows;
		Row g_pendingRow{};
		bool g_hasPendingRow = false;
		std::unordered_map<uint64_t, ReflexTimes> g_reflex;
		uint32_t g_reflexPoll = 0;
		std::mutex g_fileMutex;
		std::string g_lastFile;

		double Ms(int64_t a_ticks) { return static_cast<double>(a_ticks) * g_toMs; }

		// ---- (46n) cs_passes split per pass ----
		constexpr int kPassBlocks = static_cast<int>(EngineTiming::PassBlock::Count);
		constexpr char kPassBlockLetters[kPassBlocks] = { 'm', 'u', 'f', 'q', 'd', 'c', 's' };
		static_assert(kPassBlocks == 7);

		std::string PassName(const EngineTiming::PassItem& a_item)
		{
			if (!a_item.name.empty())
				return std::string(a_item.name);
			return a_item.gpuBucket >= 0 ? std::format("g.bucket{}", a_item.gpuBucket) : std::string("?");
		}

		void ApplyPassGpu(Row& a_row, int a_bucket, float a_ms)
		{
			auto& passes = a_row.engine.passes;
			if (passes.empty())
				return;  // the passes of this frame are not listed
			for (auto& p : passes) {
				if (p.gpuBucket == a_bucket) {
					p.gpuMs = (p.gpuMs < 0.0f ? 0.0f : p.gpuMs) + a_ms;
					return;
				}
			}
			EngineTiming::PassItem item;
			item.name = EngineTiming::GpuPassName(a_bucket);
			item.gpuBucket = a_bucket;
			item.gpuMs = a_ms;
			passes.push_back(item);
		}

		/// Blocked ms of a pass (a CompileShader's own shader Create is counted in both c and s; ignored here).
		float PassBlockTotal(const EngineTiming::PassItem& a_item)
		{
			float total = 0.0f;
			for (int b = 0; b < kPassBlocks; ++b)
				total += a_item.blockMs[b];
			return total;
		}

		/// "name=cpu/gpu;..." costliest CPU first (gpu "-" for passes without GPU timing).
		std::string FormatPassSplit(const Row& a_row)
		{
			std::string out;
			for (const auto& p : a_row.engine.passes) {
				if (p.cpuMs < 0.05f && p.gpuMs < 0.05f && PassBlockTotal(p) < 0.05f)
					continue;
				out += std::format("{}{}={:.2f}/", out.empty() ? "" : ";", PassName(p), p.cpuMs);
				out += p.gpuMs < 0.0f ? std::string("-") : std::format("{:.2f}", p.gpuMs);
			}
			return out;
		}

		/// "name:m<ms>x<calls> ...;..." for passes that blocked (m Map, u copies, f Flush, q GetData/Clear, d Draw/Dispatch,
		/// c Create, s CompileShader).
		std::string FormatPassBlock(const Row& a_row)
		{
			std::string out;
			for (const auto& p : a_row.engine.passes) {
				if (PassBlockTotal(p) < 0.02f)
					continue;
				std::string parts;
				for (int b = 0; b < kPassBlocks; ++b) {
					if (p.blockCalls[b] == 0 || p.blockMs[b] < 0.005f)
						continue;
					parts += std::format("{}{}{:.2f}x{}", parts.empty() ? "" : " ", kPassBlockLetters[b], p.blockMs[b], p.blockCalls[b]);
				}
				if (!parts.empty())
					out += std::format("{}{}:{}", out.empty() ? "" : ";", PassName(p), parts);
			}
			return out;
		}

		void LogPassRow(const Row& a_row, size_t a_index)
		{
			const auto& e = a_row.engine;
			std::vector<const EngineTiming::PassItem*> sorted;
			float block[kPassBlocks]{};
			uint32_t blockCalls[kPassBlocks]{};
			const EngineTiming::PassItem* worstBlock = nullptr;
			for (const auto& p : e.passes) {
				sorted.push_back(&p);
				for (int b = 0; b < kPassBlocks; ++b) {
					block[b] += p.blockMs[b];
					blockCalls[b] += p.blockCalls[b];
				}
				if (!worstBlock || PassBlockTotal(p) > PassBlockTotal(*worstBlock))
					worstBlock = &p;
			}
			std::sort(sorted.begin(), sorted.end(), [](const auto* a_l, const auto* a_r) { return a_l->cpuMs > a_r->cpuMs; });
			std::string top;
			for (size_t i = 0; i < sorted.size() && i < 3; ++i) {
				const auto& p = *sorted[i];
				top += std::format("{}{} {:.2f}/", top.empty() ? "" : ", ", PassName(p), p.cpuMs);
				top += p.gpuMs < 0.0f ? std::string("-") : std::format("{:.2f}", p.gpuMs);
			}
			using B = EngineTiming::PassBlock;
			const auto bi = [](B a_b) { return static_cast<int>(a_b); };
			const std::string where = a_row.passSinceLoad >= 0 ? std::format("{} frames after the loading screen", a_row.passSinceLoad) :
			                                                     std::string("not right after a loading screen");
			logger::info("[FrameTrace] Post-load pass spike: top 3 passes (cpu/gpu ms) [{}]; trace frame {} ({}), frame {:.1f} ms, cs_passes {:.1f} ms "
						 "(passes add up to {:.1f}, untimed {:.2f}); blocking in cs_passes: Map {:.2f} ({}), copies {:.2f} ({}), Flush {:.2f} ({}), "
						 "GetData/Clear {:.2f} ({}), Draw/Dispatch {:.2f} ({}), Create {:.2f} ({}), CompileShader {:.2f} ({}); most blocked pass {} {:.2f} ms",
				top, a_index, where, a_row.frameMs, e.ms[static_cast<int>(EngineTiming::Seg::OurPasses)], e.passSumMs, e.passUntimedMs, block[bi(B::Map)],
				blockCalls[bi(B::Map)], block[bi(B::Upload)], blockCalls[bi(B::Upload)], block[bi(B::Flush)], blockCalls[bi(B::Flush)], block[bi(B::Misc)],
				blockCalls[bi(B::Misc)], block[bi(B::Submit)], blockCalls[bi(B::Submit)], block[bi(B::Create)], blockCalls[bi(B::Create)],
				block[bi(B::Compile)], blockCalls[bi(B::Compile)], worstBlock ? PassName(*worstBlock) : std::string("-"),
				worstBlock ? PassBlockTotal(*worstBlock) : 0.0f);
		}

		/// Logs the queued spike rows whose GPU samples are in (a_all: every queued row, at the end of a recording).
		void DrainPassLogs(bool a_all)
		{
			size_t done = 0;
			for (; done < g_passLogQueue.size(); ++done) {
				const size_t index = g_passLogQueue[done];
				if (!a_all && index + kPassLogDelayRows >= g_rows.size())
					break;
				if (index < g_rows.size())
					LogPassRow(g_rows[index], index);
			}
			g_passLogQueue.erase(g_passLogQueue.begin(), g_passLogQueue.begin() + static_cast<std::ptrdiff_t>(done));
		}

		bool WantedThisFrame()
		{
			auto& overlay = globals::features::performanceOverlay;
			if (!overlay.loaded || !overlay.settings.DriverWaitTiming)
				return false;
			if (g_recording)
				return true;
			return globals::menu && globals::menu->overlayVisible && overlay.IsOverlayVisible();
		}

		void PollReflex()
		{
			auto& streamline = globals::features::upscaling.streamline;
			if (!streamline.reflexFunctionsReady)
				return;
			sl::ReflexState state{};
			if (!streamline.GetReflexState(state) || !state.latencyReportAvailable)
				return;
			for (const auto& r : state.frameReport) {
				if (r.simStartTime == 0)
					continue;
				g_reflex[r.frameID] = { r.simStartTime, r.simEndTime, r.renderSubmitStartTime, r.renderSubmitEndTime, r.presentStartTime, r.presentEndTime,
					r.driverStartTime, r.driverEndTime, r.osRenderQueueStartTime, r.osRenderQueueEndTime, r.gpuRenderStartTime, r.gpuRenderEndTime,
					r.gpuActiveRenderTimeUs, r.gpuFrameTimeUs, r.inputSampleTime };
			}
		}

		/// (46u) Reflex input sample to simulation start in ms, or -1 when not reported (0, after the start, or implausibly early).
		float InputSampleMs(uint64_t a_inputSample, uint64_t a_simStart)
		{
			if (a_inputSample == 0 || a_inputSample > a_simStart || a_simStart - a_inputSample > 200000)
				return -1.0f;
			return static_cast<float>(static_cast<double>(a_simStart - a_inputSample) / 1000.0);
		}

		/// (44f, A) Newest complete Reflex report: simulation start to GPU end, for the overlay line.
		void PollLatency()
		{
			auto& streamline = globals::features::upscaling.streamline;
			if (!streamline.reflexFunctionsReady)
				return;
			sl::ReflexState state{};
			if (!streamline.GetReflexState(state) || !state.latencyReportAvailable)
				return;
			const sl::ReflexReport* newest = nullptr;
			for (const auto& r : state.frameReport)
				if (r.simStartTime != 0 && r.gpuRenderEndTime > r.simStartTime && (!newest || r.frameID > newest->frameID))
					newest = &r;
			if (!newest)
				return;
			const float ms = static_cast<float>(static_cast<double>(newest->gpuRenderEndTime - newest->simStartTime) / 1000.0);
			auto& s = g_smoothed.reflexLatencyMs;
			s = s < 0.0f ? ms : s * 0.8f + ms * 0.2f;
			// (46u) Input sample to simulation start, only when Reflex reports one (we set no input-sample marker,
			// so normally 0 = none; the estimate then assumes half a real frame).
			const float inMs = InputSampleMs(newest->inputSampleTime, newest->simStartTime);
			if (inMs >= 0.0f) {
				auto& in = g_smoothed.reflexInputMs;
				in = in < 0.0f ? inMs : in * 0.8f + inMs * 0.2f;
			}
		}

		/// (46e) The upscaler, render / output size and the screen-space GI / reflection setup, for the trace header.
		std::string DescribeRendering()
		{
			std::string text;
			auto& up = globals::features::upscaling;
			static constexpr const char* kMethods[] = { "none", "TAA", "FSR", "DLSS" };
			static constexpr const char* kQuality[] = { "Native AA (DLAA)", "Quality", "Balanced", "Performance", "Ultra Performance" };
			if (up.loaded) {
				const auto method = static_cast<std::uint32_t>(up.GetUpscaleMethod());
				const auto quality = up.settings.qualityMode;
				text = std::format("upscaler {} {}, DLSS preset {}, ray reconstruction {} (preset {}), sharpening {:.2f}", method < 4 ? kMethods[method] : "?",
					quality < 5 ? kQuality[quality] : "?", up.settings.DLSSPreset, up.settings.enableDLSSRR ? "on" : "off", up.settings.DLSSDPreset,
					up.settings.sharpnessRCAS);
			} else {
				text = "upscaler not loaded";
			}
			if (globals::state) {
				const float2 output = globals::state->screenSize;
				// (46p) The upscaler's render size (ConvertToDynamic here, outside the frame's dynamic-resolution window, gave the output size).
				const float2 render = up.loaded ? float2{ std::floor(output.x * up.resolutionScale.x), std::floor(output.y * up.resolutionScale.y) } :
				                                  Util::ConvertToDynamic(output, true);
				text += std::format("; render {}x{}, output {}x{}", static_cast<int>(std::floor(render.x)), static_cast<int>(std::floor(render.y)),
					static_cast<int>(output.x), static_cast<int>(output.y));
			}
			static constexpr const char* kDenoisers[] = { "off", "SVGF", "REBLUR" };
			const auto& ssrt = globals::features::screenSpaceRayTracing;
			if (ssrt.loaded) {
				const auto& s = ssrt.settings;
				const auto dn = [](std::uint32_t a_value) { return a_value < 3 ? kDenoisers[a_value] : "?"; };
				text += std::format("; SSRT {} (diffuse {}, specular {}, denoiser asked {}, in effect diffuse {} / specular {}, diffuse sampling {}, SPP {})",
					(s.EnableDiffuse || s.EnableSpecular) ? "on" : "off", s.EnableDiffuse ? "on" : "off", s.EnableSpecular ? "on" : "off", dn(s.DenoiserMethod),
					dn(ssrt.EffectiveDenoiser(false)), dn(ssrt.EffectiveDenoiser(true)), s.DiffuseSamplingMode, s.DiffuseSPP);
			} else {
				text += "; SSRT not loaded";
			}
			const auto& ssgi = globals::features::screenSpaceGI;
			if (ssgi.loaded) {
				const auto& s = ssgi.settings;
				text += std::format("; SSGI {} (AO {}, GI {}, contact AO {})", s.Enabled ? "on" : "off", s.Enabled ? "on" : "off", s.Enabled && s.EnableGI ? "on" : "off",
					s.Enabled && s.EnableContactAo && ssgi.contactAoActive ? "on" : "off");
			} else {
				text += "; SSGI not loaded";
			}
			text += "; " + globals::features::screenSpacePointLightShadows.DescribeForTrace();  // (47k)
			return text;
		}

		/// (46e) Per-pass GPU samples of SSRT / SSGI for this row (-1 when the pass is off).
		void SampleScreenSpacePasses(Row& a_row)
		{
			auto* timers = Util::GpuPassTimers::GetSingleton();
			const auto& ssrt = globals::features::screenSpaceRayTracing;
			const auto& ssgi = globals::features::screenSpaceGI;
			const auto sample = [&](bool a_on, Util::GpuBucket a_bucket) { return a_on ? timers->LastSampleMs(a_bucket) : -1.0f; };
			const bool diffuse = ssrt.loaded && ssrt.settings.EnableDiffuse;
			const bool specular = ssrt.loaded && ssrt.settings.EnableSpecular;
			const bool reblur = (diffuse && ssrt.EffectiveDenoiser(false) == ScreenSpaceRayTracing::kDenoiserREBLUR) ||
			                    (specular && ssrt.EffectiveDenoiser(true) == ScreenSpaceRayTracing::kDenoiserREBLUR);
			a_row.ssPassMs[0] = sample(reblur, Util::GpuBucket::SSRTReblur);
			a_row.ssPassMs[1] = sample(diffuse, Util::GpuBucket::SSRTTraceDiffuse);
			a_row.ssPassMs[2] = sample(specular, Util::GpuBucket::SSRTTraceSpecular);
			a_row.ssPassMs[3] = sample(ssgi.loaded && ssgi.settings.Enabled, Util::GpuBucket::SSGI);
			a_row.ssPassMs[4] = sample(ssgi.loaded && ssgi.settings.Enabled && ssgi.settings.EnableContactAo && ssgi.contactAoActive, Util::GpuBucket::SSGIContactAO);
			// (47f) Volumetric fog passes: built within the last few frames (LastSampleMs is -1 once a bucket has gone quiet).
			const auto& ehf = globals::features::exponentialHeightFog;
			const bool vf = ehf.loaded && ehf.lastBuildFrame != UINT32_MAX && globals::state && globals::state->frameCount - ehf.lastBuildFrame < 8;
			const bool vfFar = vf && ehf.currentFarGridSize.z > 0 && timers->LastSampleMs(Util::GpuBucket::VolumetricFogFar) >= 0.0f;
			a_row.vfPassMs[0] = sample(vf, Util::GpuBucket::VolumetricFogDepth);
			a_row.vfPassMs[1] = sample(vf, Util::GpuBucket::VolumetricFogMaterial);
			a_row.vfPassMs[2] = sample(vf, Util::GpuBucket::VolumetricFogNear);
			a_row.vfPassMs[3] = sample(vfFar, Util::GpuBucket::VolumetricFogFar);
			a_row.vfPassMs[4] = sample(vf, Util::GpuBucket::VolumetricFogIntegrate);
			a_row.vfPassMs[5] = sample(vf, Util::GpuBucket::VolumetricFogHistory);
			a_row.waterPrepGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingWaterPrep);  // (47j) -1 once quiet
			a_row.aoNormalPrepGpuMs = timers->LastSampleMs(Util::GpuBucket::RayTracingAONormalPrep);  // (47l) -1 once quiet
			if (vf) {
				a_row.vfGrid[0] = ehf.currentGridSize.x;
				a_row.vfGrid[1] = ehf.currentGridSize.y;
				a_row.vfGrid[2] = ehf.currentGridSize.z;
				a_row.vfGrid[3] = vfFar ? ehf.currentFarGridSize.z : 0u;
			}
		}

		void WriteCsv(std::vector<Row> a_rows, std::unordered_map<uint64_t, ReflexTimes> a_reflex, std::string a_location, std::string a_rayTracing, std::string a_tag,
			std::string a_latency, std::string a_rendering, std::string a_header46o, std::string a_headerFG)
		{
			try {
				auto dir = logger::log_directory();
				if (!dir)
					return;
				std::filesystem::path path = *dir / "CommunityShaders" / "Perf";
				std::filesystem::create_directories(path);
				const auto now = std::chrono::system_clock::now();
				const std::time_t t = std::chrono::system_clock::to_time_t(now);
				std::tm local{};
				localtime_s(&local, &t);
				char stamp[32];
				std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
				const std::string name = a_tag.empty() ? std::format("trace-{}.csv", stamp) : std::format("trace-{}-{}.csv", stamp, a_tag);
				path /= name;

				std::ofstream out(path, std::ios::binary | std::ios::trunc);
				if (!out)
					return;
				out << "# Community Shaders frame trace (" << BuildLabel::kBuild << "). One row per real (rendered) frame. Times in ms unless noted.\n";
				out << "# cpu_busy = frame - present_wait - reflex_sleep (includes driver_*). gpu_* come from GPU timestamps (-1 = no sample).\n";
				out << "# reflex_* in us relative to reflex_sim_start (-1 = no report). fg_presented = frames DLSS-G showed for this frame (0 = not generating).\n";
				out << "# cpu_frame = simulation start (after Reflex sleep) to Present start: all CPU submission work, driver blocking included.\n";
				out << "# eng_* (44f): the main thread from one Present end to the next, split into engine segments; they add up to frame_ms (-1 = probes not installed).\n";
				out << "#   eng_logic = the game update (AI, scripts, physics, animation, ...); eng_top_call = Address Library ID@offset in Main::Update of its slowest call,\n";
				out << "#   eng_calls_over_1ms = every such call over 1 ms (id@offset:ms;...). eng_render_other = rendering minus the named phases (culling, scene traversal).\n";
				out << "# eng_dt_ms / eng_real_dt_ms = the engine's time step (BSTimer). dev_create_* = time inside D3D11 device Create* calls (main thread / others).\n";
				out << "# reflex_markers = 1 when Reflex Markers at Render Start was on for that frame.\n";
				out << "# (45m) reflex_gpu_active_ms / reflex_gpu_frame_ms = GPU time the driver counts for the frame (Reflex sees only the D3D12 interop device).\n";
				out << "#   reflex_sleep_ok = slReflexSleep succeeded; reflex_lowlat = lowLatencyAvailable (slReflexGetState, -1 = not queried); reflex_mode / reflex_limit_fps = applied.\n";
				out << "#   latency_fix = 45m Reflex fix on; pacer_* = frame-start pacer: on, sleep (predictive), cap_wait (hard bound: frame before the previous one),\n";
				out << "#   in_flight = frames the GPU had not finished at frame start, pred = predicted ms until the previous frame finishes, lead = CPU ms reserved (p75),\n";
				out << "#   gpu_period = median ms between GPU frame completions. pacer_sleep_ms = the whole pacer wait (both parts); cpu_busy also excludes it.\n";
				out << "# (45m) latency at start: " << a_latency << "\n";
				out << "# (45j) location at start: " << a_location << "\n";
				out << "# (45j) ray tracing at start: " << a_rayTracing << "\n";
				out << "# (46e) rendering at start: " << a_rendering << "\n";
				out << "# (46w) frame generation at start: " << a_headerFG << "\n";
				out << "# (47p) performance switches when written: " << PerfOptions::Describe() << "\n";
				out << "# (45j) rt_* = Ray Tracing CPU split (ms, -1 = not run): scene update (grid diff, cell capture, filter registration, objects of which new\n";
				out << "#   captures), TLAS instances, missed-caster check, characters, D3D12 record+submit (of which BLAS creation, skinning), D3D11 passes,\n";
				out << "#   end of frame, engine references dropped; counts: recaptures, objects captured, misses examined, references waiting, TLAS instances,\n";
				out << "#   cells being captured. map_* = Map calls this frame, those over 0.5 ms, the slowest (ms, resource, caller module+offset).\n";
				out << "#   loc_* / pos_* / weather / game_hour = where the player was on that frame.\n";
				out << "# (45k) rt_denoiser = shadow denoiser (0 off, 1 temporal 45j, 2 penumbra, 3 DLSS RR); rt_phase = 0 play, 1 loading screen, 2 warm-up after it.\n";
				out << "#   rt_x_* = D3D12 record + submit call by call: context choice, D3D11 Signal, D3D11 Flush, D3D12 queue Wait, allocator/list Reset,\n";
				out << "#   TLAS instances, alpha records, recording, ExecuteCommandLists, D3D12 Signal, D3D11 Wait (with rt_exec_build/skin they add up to rt_exec).\n";
				out << "#   rt_denoise_gpu_ms / sss_gpu_ms = last GPU sample of the denoiser passes / Screen Space Shadows (a few frames old).\n";
				out << "# (45l) rt_denoiser also 4 = NRD SIGMA; rt_denoiser_eff = the one in effect (3 whenever Upscaling's Ray Reconstruction runs); rt_quality 0-4 = Very Low..Ultra, 5 Custom.\n";
				out << "#   rt_denoise_gpu_ms = tiles + filter + SIGMA; rt_tile/filter/sigma/combine_gpu_ms per pass; rt_sync_gpu_ms = the game queue standing still at the wait\n";
				out << "#   for the trace; rt_prep_gpu_ms / rt_trace_gpu_ms = D3D12 builds+skinning+TLAS / the trace dispatch (rt_gpu_ms = their sum); rt_skin_draws / _reused =\n";
				out << "#   character shapes traced / of which with last frame's skinning (other actors at reduced quality).\n";
				out << "# (45n) rt_early = builds / skinning went out at the frame start (Early Prep; rt_exec_build/skin are then that part), rt_early_ms = its CPU;\n";
				out << "#   rt_x_retire_ms / rt_retired_n = retired D3D12 objects collected this frame (inside rt_x_collect; with Release Thread on only handed over),\n";
				out << "#   rt_worker_release_ms = the release thread's last batch; rt_queue_gap_gpu_ms = our D3D12 queue idle between builds/skinning and the trace\n";
				out << "#   (> 0: the builds were done before the game's depth copy was ready); rt_still_reused = character shapes with unchanged bones (not skinned);\n";
				out << "#   pacer_in_flight_after = frames still on the GPU after the pacer's sleep and bound (pacer_in_flight is counted before the sleep).\n";
				out << "# (45o) lights_* = Light Limit Fix this frame: lights in its buffer, of which shadow-casting, fill-light estimate (placed light without a\n";
				out << "#   mesh, no shadow, LIGH editor ID with omni/ambient/fill or a plugin named *ambient*; 46a: never by radius), attached to the player;\n";
				out << "#   lights_max_cluster / lights_over4_share = most lights in one cluster and the share\n";
				out << "#   of screen tiles whose busiest cluster has more than 4 (from a GPU read-back 2-4 frames old); llf_gpu_ms = LLF build + culling (last sample).\n";
				out << "# (46b) rt_pl_* = ray-traced point-light shadows: K (lights traced per pixel, 0 = not traced this frame), shadow sources traced,\n";
				out << "#   candidate lights after the rules (before merging / the cap), rays per frame (read back a few frames late, -1 = not known),\n";
				out << "#   GPU ms of the point-light trace (D3D12 queue, inside rt_gpu_ms) and of its denoiser (D3D11), CPU ms of the light selection.\n";
				out << "# (46c) rt_interior = 0 outdoors / off, 1 indoors capturing, 2 indoors traced; rt_pl_beams = window beams traced; rt_sm_skipped =\n";
				out << "#   engine shadow maps left out (Stop Engine Shadow Maps); rt_mask_channels = shadow mask channels written by RT; rt_interior_shapes = captured.\n";
				out << "# (46e) ssrt_reblur / ssrt_trace_diffuse / ssrt_trace_specular / ssgi / ssgi_contact_ao _gpu_ms = last GPU sample of that pass\n";
				out << "#   (a few frames old; -1 = the pass is off); rt_pl_carried = point-light sources carried by a traced character (its body skipped).\n";
				out << "# (46f) rtao_gpu_ms = RTAO rays on the D3D12 queue (own list after the sun trace, -1 = not traced); rtao_filter_gpu_ms = its D3D11 filter +\n";
				out << "#   history + composite; rtao_sync_gpu_ms = the game queue standing still at the wait for its second fence (right before the AO is used);\n";
				out << "#   rtao_rays = RTAO rays per frame (read back a few frames late, -1 = not known, 0 = not traced). RTAO settings: see the ray tracing line.\n";
				out << "# (46g) rt_phase also 3 = black screen before the hand-off. rt_black = the black screen after a loading screen (Fader Menu open);\n";
				out << "#   rt_since_close_ms = ms since the loading screen closed (-1 = no transition); rt_handoff = 1 on the frame every ready cell / object switched\n";
				out << "#   from the shadow map to RT (one global switch per transition); rt_held_ready / rt_held_pending = items held back then (-1 = no hold).\n";
				out << "#   rt_e_* = Early Prep (PrepareFrame) call by call: retired objects + pools + context choice, GPU timestamp read-back, allocator/list Reset,\n";
				out << "#   BLAS builds, character uploads + skinning, barriers + Close, ExecuteCommandLists (with the characters they add up to rt_early_ms).\n";
				out << "#   rt_x_timestamps / ensure / publish / ao = taken out of rt_x_collect / tlas / record: timestamp read-back, TLAS + instance buffer growth,\n";
				out << "#   descriptor publish, the RTAO list (rt_x_execute now also holds the first ExecuteCommandLists).\n";
				out << "#   rt_pool_ms = render-thread pool upkeep (deferred frees, finished blocks, growth); rt_pool_sync_blocks = blocks created on the render\n";
				out << "#   thread (loading / black screen only); rt_pool_mb = MB reserved; rt_pool_waits = allocations waiting for a new block; rt_pool_worker_ms = last block on the pool thread.\n";
				out << "#   rt_pl_slot_changes / merged / rej_radius / rej_intensity / kept = point-light selection this frame (kept = held by the hysteresis);\n";
				out << "#   rt_pl_churn = pixels whose traced light set changed (GPU, a few frames late, -1 = not known); rt_pl_beams_excluded = implausible window beams.\n";
				out << "# (46h) RTAO indoors too: the rtao_* columns are filled indoors as outdoors; rt_interior 2 = indoors traced (point lights and / or RTAO).\n";
				out << "#   rtao_indoors = this frame's RTAO traced indoors (1) / outdoors (0), -1 = not traced; rtao_mode = its mode (0 RTAO + SSGI contact AO,\n";
				out << "#   1 darker of both), -1 = not traced. Indoors with RTAO only (no light traced) rt_sync_gpu_ms is 0: the AO wait covers the trace.\n";
				out << "# (46k) rt_pump = 1 on a loading-screen frame (no world render) whose RT work came from the loading pump (rt_phase 1); rt_pump_ms = its CPU\n";
				out << "#   (-1 = not run). rt_black is now only the first frames after the loading screen (Black Screen Window), not the whole fade-in.\n";
				out << "# (46l) wt_* = the world segment (eng_world_ms) split while recording (wt_valid 1; wt_total_ms should match eng_world_ms).\n";
				out << "#   By kind, exclusive (they add up to wt_total): engine = engine code outside the per-pass calls (traversal, sorting, groups);\n";
				out << "#   pass = per-pass glue in RenderPassImmediately (technique / material switch, SetDirtyStates incl. our State::Draw, the draw);\n";
				out << "#   technique = BeginTechnique with our shader lookup; material_ours / _engine = SetupMaterial (Lighting) ours / the engine's;\n";
				out << "#   setup_ours / _engine = SetupGeometry ours (Advanced Skin, LLF, SSS, ...) / the engine's (bones, constants, lights);\n";
				out << "#   restore = RestoreGeometry; ours_other = our start / end of the deferred pass and terrain blending.\n";
				out << "#   Of which (inside the kinds above): wt_driver_ms = D3D11 calls, wt_own_draw_ms = State::Draw, wt_lookup_ms = shader-cache lock\n";
				out << "#   wait (wt_lookups lookups), wt_miss_ms / wt_misses = cache misses handed off, wt_skin_* = Advanced Skin characters computed\n";
				out << "#   (wetness / snow), wt_shelter_* = Havok shelter picks, wt_water_* = water-object searches, wt_create_* / wt_tex_creates\n";
				out << "#   (buffers and textures) / wt_view_creates = device Create* calls. Counts: render passes, technique switches, material set-ups,\n";
				out << "#   geometry set-ups, pass groups; wt_stamps = TSC reads (the split's own cost is about wt_stamps x 10 ns).\n";
				out << "#   wt_top3 / wt_places_over_1ms = by place: RW/FA/RD/FP+offset>callee = the call site in Main::RenderWorld /\n";
				out << "#   FinishAccumulating_Standard / Main::RenderDepth / Main::RenderFirstPersonView and the Address Library ID it calls;\n";
				out << "#   <D|W|F>grp<group>:<first>-<last> = BSShaderAccumulator::RenderBatches of that pass group (D depth, W world, F first person);\n";
				out << "#   XX:start = before the first probe. driver_misc_* = GetData / Clear* / GenerateMips / ResolveSubresource (not in driver_total);\n";
				out << "#   map_other_* = Map on contexts other than the immediate one (any thread).\n";
				out << "# (46m) rt_ch_* = Ray Tracing characters' CPU (ms, -1 = not run): cand = actors chosen; reads = per-actor reads (3D scan or check of the\n";
				out << "#   known shapes + bone matrices) on rt_ch_lanes lanes (1 = render thread alone); capture = new shapes captured, changed ones retired;\n";
				out << "#   commit = still-pose check; filter = caster filter; collect = draw list (CollectDraws). rt_chars_ms is all of it. rt_ch_bones = bone\n";
				out << "#   matrices read, rt_ch_rescans = actors whose 3D was walked in full, rt_ch_actors / rt_ch_shapes = traced. rt_skin_groups = merged\n";
				out << "#   character BLASes in the TLAS (Merge Character Parts), rt_skin_group_builds / _refits = of them built / refit, rt_skin_instances =\n";
				out << "#   character TLAS instances, rt_skin_dispatches = skinning dispatches. rt_sync_split = 1 when the sun had its own fence (Split Trace\n";
				out << "#   Sync), 0 not, -1 not traced; rt_sync_sun_gpu_ms / rt_sync_pl_gpu_ms = the game queue's wait for the sun / for the point lights after\n";
				out << "#   the shadow mask passes (rt_sync_gpu_ms = their sum; not split: rt_sync_sun_gpu_ms = the whole wait, rt_sync_pl_gpu_ms 0).\n";
				out << "# (46n) pass_* = eng_cs_passes split by our pass (exclusive: the innermost open pass takes the time, so they add up).\n";
				out << "#   Passes: feature names (the CPU table's keys), g.<bucket> = a GPU Passes bucket (its CPU from Begin to End), Upscaling.* /\n";
				out << "#   Deferred.* = our hook code, untimed.<scope> = cs_passes time with no pass open. pass_sum_ms = their sum (= eng_cs_passes_ms),\n";
				out << "#   pass_untimed_ms = the untimed part. pass_since_load = frames since the loading screen closed (0-119, -1 outside).\n";
				out << "#   pass_split = name=cpu_ms/gpu_ms;... costliest CPU first, listed in that window and on frames with cs_passes > 10 ms (gpu = that\n";
				out << "#   frame's GPU time of the bucket, '-' = no GPU timer). pass_block = name:<kind><ms>x<calls> for blocking calls on the main thread\n";
				out << "#   inside the pass: m Map/Unmap, u Copy*/UpdateSubresource, f Flush, q GetData/Clear*/mips/resolve, d Draw/Dispatch, c device Create*,\n";
				out << "#   s CompileShader (disk cache or compile; its own Create is also in c).\n";
				out << "# (46n) pacer_d3d11_wait_ms = the previous frame's main-thread time inside D3D11 calls of 0.5 ms or more (Map / Flush / GetData:\n";
				out << "#   waits for the GPU), left out of pacer_lead_ms; pacer_learned_ms = the frame-start sleep learned from them (in pacer_sleep_ms).\n";
				out << "# (46o) " << a_header46o << "\n";
				out << "# (46o) eng_<seg>_wait_ms (M1) = the main thread's wall time minus its own CPU time (QueryThreadCycleTime at every segment change)\n";
				out << "#   in that segment: blocked (waiting for the GPU inside D3D11, on a lock or event) or not scheduled; main_cpu_ms = the thread's CPU\n";
				out << "#   time over the whole frame (-1 = not measured: probes off or not calibrated).\n";
				out << "#   pacer_hidden_wait_ms = the previous frame's main-thread wait in the render segments (render_other..present) up to the submit\n";
				out << "#   (measured while recording or pacing; -1 = not measured); pacer_hidden_base_ms = the hidden wait frames\n";
				out << "#   have anyway (p25 over frames that slept 1 ms+: job joins, preemption); pacer_wait_used_ms = what was left out of its lead sample\n";
				out << "#   (the larger of the 46n D3D11 wait and hidden - base when >= 0.5 ms and the GPU was behind at\n";
				out << "#   that frame's start; the learned sleep takes the median of the last 3); pacer_backstop_ms = lead taken off by the Unlock Backstop.\n";
				out << "#   stall_stack (M2, Stall Sampler on) = up to 3 samples of a main thread stuck 8 ms+ in one segment / call: seg@ms_in_it:RIP>return\n";
				out << "#   addresses found on the stack (module+offset, nearest first; candidates, not a verified unwind), samples separated by '|'.\n";
				out << "#   vram_* (M3, every 4th frame; 46p: every frame) = this process's video memory on the D3D11 adapter (MB): local budget, local usage, non-local\n";
				out << "#   (system memory) usage; vram12_local_usage_mb = the D3D12 device's adapter if it is another one (-1 = same adapter);\n";
				out << "#   vram_warn = Streamline evaluations this frame that returned eWarnOutOfVRAM (budget used up).\n";
				out << "#   present_* (M5) = DX12SwapChain::Present split: fg (DLSS-G state / options), hdr (HDR composite + screenshot), signal11 (D3D11\n";
				out << "#   Signal), copy (D3D12 Wait, Reset, copy, tags, ExecuteCommandLists), sl (the Streamline / DXGI Present: DLSS-G waits for the GPU\n";
				out << "#   here), post (D3D12 Signal, D3D11 Wait, clears, HDR upkeep, the spin limiter); -1 = not run.\n";
				out << "#   ups_* / sl_* = the DLSS pass of Upscaling: input copy, D3D11 Signal, D3D12 Wait, Reset, Streamline options + constants,\n";
				out << "#   tags + slEvaluateFeature, Close + ExecuteCommandLists, D3D12 Signal, D3D11 Wait, RCAS or copy back (-1 = not run).\n";
				out << "#   frame_flags (M6) = bits: 1 loading screen, 2 map, 4 game paused, 8 a menu open (application / item / modal), 16 Fader Menu,\n";
				out << "#   32 the pacer cleared its samples this frame (after a pause / loading screen / the map).\n";
				out << "# (46p) vram_* are now sampled every frame (vram_query_us = the cost of that query).\n";
				out << "#   pacer_oversleep_cut_ms = planned sleep not taken because the previous frame finished on the GPU first (Never Over-Sleep: the\n";
				out << "#   sleep ends at that completion + max(0.5, min(2, 0.1 x GPU period)) ms); pacer_decay_ms = learned sleep taken off fast this\n";
				out << "#   frame (cut sleep: down to the part that was of use, at least -2 ms; frames slower than the GPU period by 5 %: -max(2, half));\n";
				out << "#   pacer_spike_skip = 1 when the previous frame's wait was not learned (a GPU frame over 1.5 periods finished since the submit\n";
				out << "#   before it or was running then: that frame and the next); pacer_done_interval_ms = the last GPU completion interval the\n";
				out << "#   waiter saw (per frame); pacer_phase_shift_ms = smoothed minus raw predicted completion (median phase of the last 6\n";
				out << "#   completions; used for pacer_pred_ms only with Smooth GPU Phase on, shown always).\n";
				out << "#   vram_<group>_new_mb / _freed_mb = bytes created / destroyed this frame (estimates from the descriptions) by: ours (RT D3D12\n";
				out << "#   BLAS / TLAS / scratch / character pools / other, our D3D11 textures and buffers, our shared interop textures), engine (D3D11\n";
				out << "#   created by SkyrimSE.exe), other (other modules: plugins, Streamline, drivers), sysmem (upload / read-back heaps, staging);\n";
				out << "#   vram_<group>_live_mb = alive of what was tagged since start-up (resources made before the hooks are not in it);\n";
				out << "#   vram_owners = per owner name:+new/-freed MB this frame (only those that moved); vram_delta_mb = usage change since the last row.\n";
				out << "#   Releases are counted when the runtime destroys the resource (D3D11 destroys late: a Release shows a frame or so after).\n";
				out << "#   gpu_seg_<seg>_ms = GPU wall time between D3D11 timestamps at the main thread's segment changes from the render start to\n";
				out << "#   the Present end (read back a few frames late; -1 = none): GPU idle inside a segment included, the D3D11 queue's waits for\n";
				out << "#   our D3D12 work (RT, upscaling, the present path) land in the segment where the wait was queued. gpu_seg_other_ms = between\n";
				out << "#   stamps in non-render segments; gpu_seg_total_ms = first to last stamp; gpu_seg_stamps = stamps (47j: 65 = capped; repeats of one segment merged; the last\n";
				out << "#   segment ran on to the close).\n";
				out << "#   (47j) water_prep_gpu_ms = WaterPrepCS (Point Light Shadows on Water); water_draw_gpu_ms = GPU wall time of the runs of water\n";
				out << "#   draws (0 = none drawn, -1 = not read back); rt_pl_water_rays = point-light rays traced from the water surface (also in rt_pl_rays).\n";
				out << "# (47l) rt_wind_meshes / rt_wind_verts = Foliage sway: wind BLASes alive and their vertices; rt_wind_refits = wind BLASes refit\n";
				out << "#   in place this frame (0 = the wind pose did not change; -1 = sway off / Ray Tracing off).\n";
				out << "# (47l) rtao_normal_prep_gpu_ms = AONormalPrepCS (RTAO: Smooth Normals, right before the RT depth copy; -1 = not run lately).\n";
				out << "# (47m) pacer_learned_cap_ms = cap on pacer_learned_ms: 1.5 x the largest of the last 60 learned waits, at most 20 (0 = none yet);\n";
				out << "#   pacer_learned_cut_ms = learned sleep not taken because the frame start + half the CPU lead would pass the predicted completion.\n";
				out << "# (47n) Foliage sway per plant (each plant its own wind BLAS within the sway radius): rt_wind_meshes = wind BLASes alive (plants),\n";
				out << "#   rt_wind_in_radius = swayable plants within the radius, rt_wind_sources = engine meshes uploaded (shared), rt_wind_builds = first\n";
				out << "#   builds this frame, rt_wind_refits / rt_wind_refit_verts = plants refit this frame and their vertices, rt_wind_radius = the radius\n";
				out << "#   (the engine's sway fade end + 256, at most 8192). The GPU cost is in rt_prep_gpu_ms. -1 = sway off / Ray Tracing off.\n";
				out << "# (47p) gpu_spike_ms = GPU work of this frame over the usual (learned on frames without a spike), 0 = no spike (12 ms+ and 1.4x);\n";
				out << "#   jobs = our periodic jobs that ran in this frame (| separated). A spike that lands in a different gpu_seg_* each time while no\n";
				out << "#   job of ours ran is another GPU user taking the GPU (another queue or process), not our work.\n";
				out << "#   rt_pw_* = the work of the RT D3D12 prep submission whose GPU time is rt_prep_gpu_ms on the same row (1 = valid; early =\n";
				out << "#   recorded at the frame start): static BLAS builds (of which alpha-tested, triangles, MB before compaction), compactions (MB\n";
				out << "#   written), new character shapes uploaded, own-BLAS character builds / refits, merged character BLAS builds (triangles) /\n";
				out << "#   refits, skinning dispatches (vertices), TLAS instances, pool blocks adopted / released since the previous submission.\n";
				out << "#   rt_pw_sample_age = trace submissions between that one and this frame's: rt_prep_gpu_ms, rt_trace_gpu_ms, rt_queue_gap_gpu_ms,\n";
				out << "#   rt_pl_gpu_ms and rtao_gpu_ms on this row belong to the frame that many rows up (rt_sync_* / rt_denoise_* are D3D11 samples, 1-4 frames).\n";
				out << "#   post_gpu_ms = our post-processing chain (last GPU sample, a few frames old). The F7 header's render size is now the upscaler's.\n";
				out << "# (46q) rt_sub_* = RT: Submit on a Worker Thread: in effect (1 / 0, -1 = RT not ready), batches the submit thread ran since the\n";
				out << "#   last row, the longest batch (ms inside ExecuteCommandLists / Signal / Wait: the block that used to be rt_x_execute on the\n";
				out << "#   main thread), the longest wait from hand-over to its start, the longest single call (-1 = none). With it on rt_x_execute /\n";
				out << "#   rt_x_signal12 / rt_x_wait12 / rt_e_execute are only the hand-over (staging); rt_x_signal11 stays (D3D11 immediate context).\n";
				out << "#   rt_bud_* = RT: Budget Builds by GPU Time: on, measured GPU ms per million BLAS triangles (EMA from rt_prep_gpu_ms minus the\n";
				out << "#   prep base of frames without builds), this frame's static triangle budget (0 = not budgeted), static builds still queued,\n";
				out << "#   merged character builds put off (cap / budget), merged character BLASes kept without a draw (grace period).\n";
				out << "#   snow_shapes_ms = Dynamic Snow footprint shapes: main-thread ms this frame; snow_shapes_state 0 not loaded, 1 loading in\n";
				out << "#   the background, 2 ready, 3 none found / failed. rr_b / rr_c = Size Buffers to Render Resolution / SSRT+SSGI at render\n";
				out << "#   resolution in effect (1 / 0, -1 = n/a), rr_width / rr_height = the size those buffers are allocated at (0 = output size).\n";
				out << "#   vram_by_feature = our video memory alive per feature (MB, >= 1): billed to the Setup scope, else the innermost pass scope\n";
				out << "#   open on the creating thread, else the feature its resource name starts with; 'other (no scope open)' = none of these.\n";
				out << "# (46t) rtao_filter = the last RTAO composite's filter: 0 quarter resolution (46s), 1 quarter resolution with the far samples'\n";
				out << "#   weights and / or normal-guided taps, 2 full resolution (-1 = none); rtao_detail_rays = near (fine detail) rays per pixel\n";
				out << "#   (0 = Fine Detail off); rtao_detail_len = the near / far split (units); rtao_normal_guided 1 / 0; rtao_alpha_budget = an AO\n";
				out << "#   ray past 8 alpha candidates counts as 0 blocked (46s), 1 half open, 2 open. rtao_gpu_ms holds the near rays too (same\n";
				out << "#   dispatch), rtao_filter_gpu_ms the detail filter and its history; rtao_rays counts every segment traced.\n";
				out << "# (46u) pcl_est_* = the overlay's \"PC latency (est.)\": input reaching the PC to the frame starting to be sent to the display\n";
				out << "#   (mouse and panel not included), an ESTIMATE. input = Reflex input sample to simulation start when reported (input_measured 1;\n";
				out << "#   no input-sample marker is set, so normally 0 and input = half of frame_ms, the average wait for the next input read);\n";
				out << "#   render = reflex_latency_ms (measured: simulation start to GPU end); fg_mult = shown frames per real frame (DLSS-G measured\n";
				out << "#   cadence, else the applied multiplier; 1 = off); fg_hold = frame_ms x (fg_mult - 1) / fg_mult (the real frame waits while the\n";
				out << "#   generated ones before it are shown; frame generation's own GPU work not included); refresh_hz = display refresh; queue =\n";
				out << "#   present to scan-out start, VRR assumed: 0 while the shown rate is below the refresh, else half a refresh; pcl_est = the sum.\n";
				out << "#   -1 = no Reflex report for the row.\n";
				out << "# (46x) rt_pl_fill_direct / fill_ambient = fill lights traced as direct light / left untraced; rt_pl_portal_away / dropped /\n";
				out << "#   boxed = room-bound lights behind a portal / of which not traced / ranked only inside their rooms' box (-1 = no point-light trace).\n";
				out << "# (47f) vf_*_gpu_ms = Volumetric Fog per pass, last GPU sample (a few frames old; -1 = not built): depth = conservative depth,\n";
				out << "#   material = the medium (height fog, weather layer, noise), scatter_near / scatter_far = lighting the cells of each grid (sun with\n";
				out << "#   shadows, sky, local lights near only), integrate = adding up along the view rays, history = the temporal copies. depth, material\n";
				out << "#   and integrate hold both grids. vf_grid_* = near grid x, y, slices and far grid slices (0 = far grid off).\n";
				out << "frame,t_ms,frame_ms,cpu_frame_ms,cpu_busy_ms,present_wait_ms,reflex_sleep_ms,"
					   "driver_draw_ms,driver_map_ms,driver_upload_ms,driver_dispatch_ms,driver_flush_ms,driver_total_ms,"
					   "own_draw_hook_ms,own_setup_geometry_ms,perm_cb_upload_ms,draws,lighting_draws,utility_draws,own_per_draw_us,"
					   "first_seen_geometry,shader_miss_async,shader_miss_sync,cell_attach,cell_detach,cam_yaw_dps,cam_pitch_dps,player_speed,"
					   "gpu_work_ms,gpu_elapsed_ms,fg_presented,grass_apply_ms,rt_gpu_ms,rt_cpu_ms,rt_blas_builds,"
					   "reflex_frame,reflex_sim_end,reflex_submit_start,reflex_submit_end,reflex_present_start,reflex_present_end,"
					   "reflex_driver_start,reflex_driver_end,reflex_queue_start,reflex_queue_end,reflex_gpu_start,reflex_gpu_end,reflex_latency_ms,reflex_gpu_active_ms,reflex_gpu_frame_ms,"
					   "reflex_markers,reflex_sleep_ok,reflex_lowlat,reflex_mode,reflex_limit_fps,latency_fix,pacer_on,pacer_sleep_ms,pacer_cap_wait_ms,pacer_in_flight,"
					   "pacer_pred_ms,pacer_lead_ms,pacer_gpu_period_ms";
				for (int s = 0; s < static_cast<int>(EngineTiming::Seg::Count); ++s)
					out << ",eng_" << EngineTiming::SegName(static_cast<EngineTiming::Seg>(s)) << "_ms";
				out << ",eng_input_ms,eng_top_call,eng_top_call_ms,eng_calls_over_1ms,eng_dt_ms,eng_real_dt_ms,dev_create_ms,dev_creates,dev_create_other_ms,dev_creates_other";
				out << ",rt_scene_ms,rt_grid_ms,rt_gather_ms,rt_register_ms,rt_objects_ms,rt_object_capture_ms,rt_instances_ms,rt_missed_ms,rt_chars_ms,rt_exec_ms,"
				       "rt_exec_build_ms,rt_exec_skin_ms,rt_d3d11_ms,rt_endframe_ms,rt_release_ms,rt_recaptures,rt_objects_new,rt_missed_n,rt_release_pending,"
				       "rt_tlas_instances,rt_cells_gathering,map_calls,map_slow,map_worst_ms,map_worst_res,map_worst_caller,loc_world,loc_cell,loc_interior,"
				       "pos_x,pos_y,pos_z,weather,game_hour,rt_denoiser,rt_phase,rt_x_collect_ms,rt_x_signal11_ms,rt_x_flush11_ms,rt_x_wait12_ms,rt_x_reset_ms,"
				       "rt_x_tlas_ms,rt_x_alpha_ms,rt_x_record_ms,rt_x_execute_ms,rt_x_signal12_ms,rt_x_wait11_ms,rt_denoise_gpu_ms,sss_gpu_ms,"
				       "rt_quality,rt_denoiser_eff,rt_tile_gpu_ms,rt_filter_gpu_ms,rt_sigma_gpu_ms,rt_combine_gpu_ms,rt_sync_gpu_ms,rt_prep_gpu_ms,rt_trace_gpu_ms,"
				       "rt_skin_draws,rt_skin_reused,rt_early,rt_early_ms,rt_x_retire_ms,rt_retired_n,rt_worker_release_ms,rt_queue_gap_gpu_ms,rt_still_reused,"
				       "pacer_in_flight_after,lights_total,lights_shadow,lights_fill_est,lights_player,lights_max_cluster,lights_over4_share,llf_gpu_ms,"
				       "rt_pl_k,rt_pl_sources,rt_pl_candidates,rt_pl_rays,rt_pl_gpu_ms,rt_pl_denoise_gpu_ms,rt_pl_cpu_ms,rt_interior,rt_pl_beams,rt_sm_skipped,rt_mask_channels,rt_interior_shapes,"
				       "ssrt_reblur_gpu_ms,ssrt_trace_diffuse_gpu_ms,ssrt_trace_specular_gpu_ms,ssgi_gpu_ms,ssgi_contact_ao_gpu_ms,rt_pl_carried,"
				       "rtao_gpu_ms,rtao_filter_gpu_ms,rtao_sync_gpu_ms,rtao_rays,"
				       "rt_black,rt_since_close_ms,rt_handoff,rt_held_ready,rt_held_pending,rt_e_collect_ms,rt_e_timestamps_ms,rt_e_reset_ms,rt_e_build_ms,rt_e_skin_ms,"
				       "rt_e_close_ms,rt_e_execute_ms,rt_x_timestamps_ms,rt_x_ensure_ms,rt_x_publish_ms,rt_x_ao_ms,rt_pool_ms,rt_pool_sync_blocks,rt_pool_mb,rt_pool_waits,"
				       "rt_pool_worker_ms,rt_pl_slot_changes,rt_pl_merged,rt_pl_rej_radius,rt_pl_rej_intensity,rt_pl_kept,rt_pl_churn,rt_pl_beams_excluded,"
				       "rtao_indoors,rtao_mode,rt_pump,rt_pump_ms,"
				       "rt_ch_cand_ms,rt_ch_reads_ms,rt_ch_capture_ms,rt_ch_commit_ms,rt_ch_filter_ms,rt_ch_collect_ms,rt_ch_bones,rt_ch_rescans,rt_ch_lanes,"
				       "rt_ch_actors,rt_ch_shapes,rt_skin_groups,rt_skin_group_builds,rt_skin_group_refits,rt_skin_instances,rt_skin_dispatches,"
				       "rt_sync_split,rt_sync_sun_gpu_ms,rt_sync_pl_gpu_ms,"
				       "wt_valid,wt_total_ms,wt_engine_ms,wt_pass_ms,wt_technique_ms,wt_material_ours_ms,wt_material_engine_ms,wt_setup_ours_ms,"
				       "wt_setup_engine_ms,wt_restore_ms,wt_ours_other_ms,wt_driver_ms,wt_own_draw_ms,wt_lookup_ms,wt_lookups,"
				       "wt_miss_ms,wt_misses,wt_skin_actors,wt_skin_ms,wt_shelter_picks,wt_shelter_ms,wt_water_searches,wt_water_ms,wt_creates,"
				       "wt_create_ms,wt_tex_creates,wt_view_creates,wt_passes,wt_techniques,wt_materials,wt_setups,wt_groups,wt_stamps,wt_top3,"
				       "wt_places_over_1ms,driver_misc_ms,driver_misc_calls,map_other_ms,map_other_calls,"
				       "cpu_top_features,pass_since_load,pass_sum_ms,pass_untimed_ms,pass_split,pass_block,pacer_d3d11_wait_ms,pacer_learned_ms,"
				       "eng_world_wait_ms,eng_shadows_wait_ms,eng_cs_passes_wait_ms,eng_logic_wait_ms,eng_present_wait_ms,eng_tail_wait_ms,"
				       "eng_render_other_wait_ms,main_cpu_ms,pacer_hidden_wait_ms,pacer_hidden_base_ms,pacer_wait_used_ms,pacer_backstop_ms,stall_stack,"
				       "vram_local_budget_mb,vram_local_usage_mb,vram_nonlocal_usage_mb,vram12_local_usage_mb,vram_warn,"
				       "present_fg_ms,present_hdr_ms,present_signal11_ms,present_copy_ms,present_sl_ms,present_post_ms,"
				       "ups_copyin_ms,ups_signal11_ms,ups_wait12_ms,ups_reset_ms,sl_setopts_ms,sl_evaluate_ms,ups_ecl_ms,ups_signal12_ms,ups_wait11_ms,ups_rcas_ms,"
				       "frame_flags";
				// (46p)
				out << ",pacer_oversleep_cut_ms,pacer_decay_ms,pacer_spike_skip,pacer_done_interval_ms,pacer_phase_shift_ms"
					   ",vram_query_us,vram_delta_mb,vram_ours_new_mb,vram_ours_freed_mb,vram_engine_new_mb,vram_engine_freed_mb,vram_other_new_mb,"
					   "vram_other_freed_mb,vram_sysmem_new_mb,vram_sysmem_freed_mb,vram_ours_live_mb,vram_engine_live_mb,vram_other_live_mb,vram_owners";
				for (int s = static_cast<int>(EngineTiming::Seg::RenderOther); s <= static_cast<int>(EngineTiming::Seg::Present); ++s)
					out << ",gpu_seg_" << EngineTiming::SegName(static_cast<EngineTiming::Seg>(s)) << "_ms";
				out << ",gpu_seg_other_ms,gpu_seg_total_ms,gpu_seg_stamps"
					   ",rt_pw_valid,rt_pw_early,rt_pw_builds,rt_pw_builds_alpha,rt_pw_build_tris,rt_pw_build_mb,rt_pw_compactions,rt_pw_compact_mb,"
					   "rt_pw_skin_uploads,rt_pw_skin_builds,rt_pw_skin_refits,rt_pw_group_builds,rt_pw_group_tris,rt_pw_group_refits,rt_pw_skin_dispatches,"
					   "rt_pw_skin_verts,rt_pw_tlas_instances,rt_pw_pool_blocks,rt_pw_pool_released,rt_pw_sample_age,post_gpu_ms,"
					   "rt_sub_on,rt_sub_batches,rt_sub_max_batch_ms,rt_sub_max_lag_ms,rt_sub_max_call_ms,rt_bud_on,rt_bud_ms_per_mtri,rt_bud_tris,"
					   "rt_bud_static_queued,rt_bud_groups_deferred,rt_bud_groups_kept,snow_shapes_ms,snow_shapes_state,rr_b,rr_c,rr_width,rr_height,"
					   "vram_by_feature,rtao_filter,rtao_detail_rays,rtao_detail_len,rtao_normal_guided,rtao_alpha_budget,"
					   "pcl_est_input_ms,pcl_est_input_measured,pcl_est_render_ms,pcl_est_fg_mult,pcl_est_fg_hold_ms,pcl_est_refresh_hz,pcl_est_queue_ms,pcl_est_ms,"
					   "rt_pl_fill_direct,rt_pl_fill_ambient,rt_pl_portal_away,rt_pl_portal_dropped,rt_pl_portal_boxed,"
					   "vf_depth_gpu_ms,vf_material_gpu_ms,vf_scatter_near_gpu_ms,vf_scatter_far_gpu_ms,vf_integrate_gpu_ms,vf_history_gpu_ms,"
					   "vf_grid_x,vf_grid_y,vf_grid_z,vf_grid_far_z,"
					   "water_prep_gpu_ms,water_draw_gpu_ms,rt_pl_water_rays,"  // (47j)
					   "rt_wind_meshes,rt_wind_verts,rt_wind_refits,"  // (47l)
					   "rtao_normal_prep_gpu_ms,"  // (47l)
					   "pacer_learned_cap_ms,pacer_learned_cut_ms,"  // (47m)
					   "rt_wind_in_radius,rt_wind_sources,rt_wind_builds,rt_wind_refit_verts,rt_wind_radius,"  // (47n)
					   "gpu_spike_ms,jobs\n";  // (47p) the last ones
				auto extraColumns = [](const Row& a_row) {
					const auto& t = a_row.rt;
					std::string s = std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{}", t.scene,
						t.gridDiff, t.gather, t.registration, t.objects, t.objectCapture, t.instances, t.missed, t.characters, t.execute, t.executeBuild, t.executeSkin,
						t.d3d11, t.endFrame, t.release, t.recaptures, t.objectsCaptured, t.missedJudged, t.releasePending, t.tlasInstances, t.cellsGathering);
					s += std::format(",{},{},{:.3f},{},{}", a_row.mapCalls, a_row.mapSlow, a_row.mapWorstMs, a_row.mapWorstResource, a_row.mapWorstCaller);
					if (a_row.location) {
						const auto& l = *a_row.location;
						s += std::format(",{},{},{},{},{},{},{},{:.2f}", l.world, l.cell, l.interior ? 1 : 0, l.x, l.y, l.z, l.weather, l.hour);
					} else {
						s += ",,,,,,,,";
					}
					s += std::format(",{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}", t.denoiser, t.phase, t.xCollect, t.xSignal11,
						t.xFlush11, t.xWait12, t.xReset, t.xTlas, t.xAlpha, t.xRecord, t.xExecute, t.xSignal12, t.xWait11, t.denoiseGpuMs, t.sssGpuMs);
					s += std::format(",{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{}", t.quality, t.denoiserEffective, t.tileGpuMs, t.filterGpuMs, t.sigmaGpuMs,
						t.combineGpuMs, t.syncGpuMs, t.prepGpuMs, t.traceGpuMs, t.skinDraws, t.skinReused);
					// (45n)
					s += std::format(",{},{:.3f},{:.3f},{},{:.3f},{:.3f},{},{}", t.early, t.earlyMs, t.retireMs, t.retiredCount, t.workerReleaseMs, t.queueGapGpuMs,
						t.stillReused, a_row.pacer.inFlightAfter);
					// (45o)
					const auto& l = a_row.lights;
					s += std::format(",{},{},{},{},{},{:.4f},{:.3f}", l.total, l.shadow, l.fill, l.player, l.maxCluster, l.over4Share, l.llfGpuMs);
					// (46b)
					s += std::format(",{},{},{},{},{:.3f},{:.3f},{:.3f}", t.plK, t.plSources, t.plCandidates, t.plRays, t.plGpuMs, t.plDenoiseGpuMs, t.plCpuMs);
						// (46c)
						s += std::format(",{},{},{},{},{}", t.rtInterior, t.plBeams, t.smSkipped, t.maskChannels, t.interiorShapes);
					// (46e)
					s += std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{}", a_row.ssPassMs[0], a_row.ssPassMs[1], a_row.ssPassMs[2], a_row.ssPassMs[3],
						a_row.ssPassMs[4], t.plCarried);
					// (46f)
					s += std::format(",{:.3f},{:.3f},{:.3f},{}", t.rtaoGpuMs, t.rtaoFilterGpuMs, t.rtaoSyncGpuMs, t.rtaoRays);
					// (46g)
					s += std::format(",{},{:.1f},{},{},{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}", t.black, t.sinceCloseMs, t.handOff, t.heldReady, t.heldPending,
						t.eCollect, t.eTimestamps, t.eReset, t.eBuild, t.eSkin, t.eClose, t.eExecute);
					s += std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{:.1f},{},{:.3f}", t.xTimestamps, t.xEnsure, t.xPublish, t.xAo, t.poolMs, t.poolSyncBlocks,
						t.poolMb, t.poolAllocFails, t.poolWorkerMs);
					s += std::format(",{},{},{},{},{},{},{}", t.plSlotChanges, t.plMerged, t.plRejRadius, t.plRejIntensity, t.plKept, t.plChurn, t.plBeamsExcluded);
					s += std::format(",{},{}", t.rtaoIndoors, t.rtaoMode);  // (46h)
					s += std::format(",{},{:.3f}", t.pump, t.pumpMs);         // (46k)
					// (46m)
					s += std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{},{}", t.chCandidates, t.chReads, t.chCapture, t.chCommit, t.chFilter,
						t.chCollect, t.chBones, t.chRescans, t.chLanes, t.chActors, t.chShapes);
					s += std::format(",{},{},{},{},{},{},{:.3f},{:.3f}", t.skinGroups, t.skinGroupBuilds, t.skinGroupRefits, t.skinInstances, t.skinDispatches,
						t.syncSplit, t.syncSunGpuMs, t.syncPlGpuMs);
					{
						// (46l) World-render split.
						const auto& w = a_row.world;
						using K = WorldTiming::Kind;
						const auto k = [&](K a_kind) { return w.kindMs[static_cast<int>(a_kind)]; };
						s += std::format(",{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}", w.valid ? 1 : 0, w.totalMs, k(K::Engine),
							k(K::Pass), k(K::Technique), k(K::MaterialOurs), k(K::MaterialEngine), k(K::SetupOurs), k(K::SetupEngine), k(K::Restore),
							k(K::OursOther));
						s += std::format(",{:.3f},{:.3f},{:.3f},{},{:.3f},{},{},{:.3f},{},{:.3f},{},{:.3f},{},{:.3f},{},{},{},{},{},{},{},{}", w.driverMs, w.ownDrawMs,
							w.lookupMs, w.lookups, w.missMs, w.misses, w.skinActors, w.skinMs, w.shelterPicks, w.shelterMs, w.waterSearches, w.waterMs, w.creates,
							w.createMs, w.textureCreates, w.viewCreates, w.passes, w.techniques, w.materials, w.setups, w.groups, w.stamps);
						s += std::format(",{},{},{:.3f},{},{:.3f},{}", w.top3, w.placesOver1ms, a_row.driverMiscMs, a_row.driverMiscCalls, a_row.mapOtherMs,
							a_row.mapOtherCalls);
					}
					s += "," + a_row.cpuTop;
					// (46n) cs_passes split.
					s += std::format(",{},{:.3f},{:.3f},{},{}", a_row.passSinceLoad, a_row.engine.passSumMs, a_row.engine.passUntimedMs, FormatPassSplit(a_row),
						FormatPassBlock(a_row));
					// (46n) The pacer: the previous frame's wait inside D3D11, the learned frame-start sleep.
					s += std::format(",{:.3f},{:.3f}", a_row.pacer.stallMs, a_row.pacer.correctionMs);
					{
						// (46o) M1 waits, the pacer's 46o state, M2 samples, M3 video memory, M5 / upscaling splits, M6 flags.
						using Seg = EngineTiming::Seg;
						const auto& e = a_row.engine;
						const auto w = [&e](Seg a_seg) { return e.waitMs[static_cast<int>(a_seg)]; };
						s += std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f}", w(Seg::World), w(Seg::Shadows), w(Seg::OurPasses), w(Seg::Logic),
							w(Seg::Present), w(Seg::Tail), w(Seg::RenderOther), e.mainCpuMs);
						s += std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{}", a_row.pacer.hiddenWaitMs, a_row.pacer.hiddenBaseMs, a_row.pacer.waitUsedMs,
							a_row.pacer.backstopMs, a_row.stallStack);
						const auto& v = a_row.vram;
						s += std::format(",{:.0f},{:.0f},{:.0f},{:.0f},{}", v.budget, v.usage, v.nonLocal, v.usage12, a_row.vramWarn);
						for (const float ms : a_row.split)
							s += std::format(",{:.3f}", ms);
						s += std::format(",{}", a_row.flags);
					}
					{
						// (46p) Pacer guard, video memory by owner, GPU per engine segment, RT prep work.
						const auto& p = a_row.pacer;
						s += std::format(",{:.3f},{:.3f},{},{:.3f},{:.3f}", p.overSleepCutMs, p.decayMs, p.spikeSkip, p.doneIntervalMs, p.phaseShiftMs);
						float groupNew[kVramGroups]{}, groupFreed[kVramGroups]{};
						std::string owners;
						for (int i = 0; i < kVramOwners; ++i) {
							groupNew[static_cast<int>(GroupOf(i))] += a_row.vramNewMb[i];
							groupFreed[static_cast<int>(GroupOf(i))] += a_row.vramFreedMb[i];
							if (a_row.vramNewMb[i] >= 0.05f || a_row.vramFreedMb[i] >= 0.05f)
								owners += std::format("{}{}:+{:.1f}/-{:.1f}", owners.empty() ? "" : ";", VramOwnerName(static_cast<VramOwner>(i)), a_row.vramNewMb[i],
									a_row.vramFreedMb[i]);
						}
						using G = VramGroup;
						const auto gi = [](G a_g) { return static_cast<int>(a_g); };
						s += std::format(",{:.1f},{:.0f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.0f},{:.0f},{:.0f},{}", a_row.vramQueryUs, a_row.vramDeltaMb,
							groupNew[gi(G::Ours)], groupFreed[gi(G::Ours)], groupNew[gi(G::Engine)], groupFreed[gi(G::Engine)], groupNew[gi(G::Other)],
							groupFreed[gi(G::Other)], groupNew[gi(G::SysMem)], groupFreed[gi(G::SysMem)], a_row.vramLiveMb[gi(G::Ours)],
							a_row.vramLiveMb[gi(G::Engine)], a_row.vramLiveMb[gi(G::Other)], owners);
						for (int seg = static_cast<int>(EngineTiming::Seg::RenderOther); seg <= static_cast<int>(EngineTiming::Seg::Present); ++seg)
							s += std::format(",{:.3f}", a_row.gpuSegMs[seg]);
						s += std::format(",{:.3f},{:.3f},{}", a_row.gpuSegOtherMs, a_row.gpuSegTotalMs, a_row.gpuSegStamps);
						const auto& w = a_row.rt.prepWork;
						constexpr double kMb = 1.0 / (1024.0 * 1024.0);
						s += std::format(",{},{},{},{},{},{:.2f},{},{:.2f},{},{},{},{},{},{},{},{},{},{},{}\n", w.valid, w.early, w.builds, w.buildsAlpha, w.buildTriangles,
							static_cast<double>(w.buildBytes) * kMb, w.compactions, static_cast<double>(w.compactBytes) * kMb, w.skinUploads, w.skinBuilds, w.skinRefits,
							w.groupBuilds, w.groupTriangles, w.groupRefits, w.skinDispatches, w.skinVertices, w.tlasInstances, w.poolBlocks, w.poolReleased);
						s.pop_back();  // the line end, after the two below
						s += std::format(",{},{:.3f}", w.sampleAge, a_row.postGpuMs);
						// (46q)
						const auto& q = a_row.q46;
						s += std::format(",{},{},{:.3f},{:.3f},{:.3f},{},{:.2f},{},{},{},{},{:.3f},{},{},{},{},{},{}\n", q.subOn, q.subBatches, q.subMaxBatchMs, q.subMaxLagMs,
							q.subMaxCallMs, q.budOn, q.budMsPerMtri, q.budTris, q.budStaticQueued, q.budGroupsDeferred, q.budGroupsKept, q.snowShapesMs, q.snowShapesState,
							q.rrB, q.rrC, q.rrWidth, q.rrHeight, a_row.vramByFeature);
						// (46t) RTAO filter (after vram_by_feature, the old last column)
						s.pop_back();
						s += std::format(",{},{},{:.0f},{},{}\n", t.rtaoFilter, t.rtaoDetailRays, t.rtaoDetailLength, t.rtaoNormalGuided, t.rtaoAlphaBudget);
					}
					return s;
				};
				auto engineColumns = [](const Row& a_row) {
					const auto& e = a_row.engine;
					const auto& p = a_row.pacer;
					std::string s = std::format(",{},{},{},{},{},{},{},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f}", a_row.reflexMarkersAtRenderStart ? 1 : 0, a_row.reflexSleepOk ? 1 : 0,
						a_row.lowLatencyAvailable, a_row.reflexModeApplied, a_row.reflexLimitApplied, a_row.latencyFix ? 1 : 0, p.enabled ? 1 : 0, a_row.pacerSleepMs, p.capWaitMs,
						p.inFlight, p.predictedMs, p.leadMs, p.gpuPeriodMs);
					for (int i = 0; i < static_cast<int>(EngineTiming::Seg::Count); ++i)
						s += e.valid ? std::format(",{:.3f}", e.ms[i]) : std::string(",-1");
					s += e.valid ? std::format(",{:.3f},{}@{:X},{:.3f},{}", e.inputMs, e.topCallId, e.topCallOffset, e.topCallMs, e.callsOver1ms) : std::string(",-1,-1,-1,");
					s += std::format(",{:.3f},{:.3f},{:.3f},{},{:.3f},{}", e.engineDtMs, e.engineRealDtMs, e.createMs, e.creates, e.createOtherMs, e.createsOther);
					return s;
				};
				uint32_t index = 0;
				for (const auto& r : a_rows) {
					float driverTotal = 0.0f;
					for (float d : r.driver)
						driverTotal += d;
					const float busy = std::max(0.0f, r.frameMs - r.presentWaitMs - r.reflexSleepMs - r.pacerSleepMs);
					const float perDraw = r.draws ? (r.ownDrawHookMs + r.ownSetupGeometryMs) * 1000.0f / static_cast<float>(r.draws) : 0.0f;
					out << std::format("{},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{:.3f},{},{},{},{},{},{:.1f},{:.1f},{:.1f},{:.3f},{:.3f},{},{:.3f},{:.3f},{:.3f},{},",
						index++, r.tMs, r.frameMs, r.cpuFrameMs, busy, r.presentWaitMs, r.reflexSleepMs,
						r.driver[0], r.driver[1], r.driver[2], r.driver[3], r.driver[4], driverTotal,
						r.ownDrawHookMs, r.ownSetupGeometryMs, r.permutationUploadMs, r.draws, r.lightingDraws, r.utilityDraws, perDraw,
						r.firstSeen, r.shaderAsync, r.shaderSync, r.cellAttach, r.cellDetach, r.yawRate, r.pitchRate, r.playerSpeed,
						r.gpuWorkMs, r.gpuElapsedMs, r.fgPresented, r.grassApplyMs, r.rtGpuMs, r.rtCpuMs, r.rtBuilds);
					auto it = r.reflexFrame != UINT32_MAX ? a_reflex.find(r.reflexFrame) : a_reflex.end();
					// (46u) PC latency estimate columns, appended after rtao_alpha_budget (the old last column).
					auto pclColumns = [&r](const ReflexTimes* a_x) {
						const float renderMs = a_x && a_x->gpuEnd > a_x->simStart ? static_cast<float>(static_cast<double>(a_x->gpuEnd - a_x->simStart) / 1000.0) : -1.0f;
						const float inputMs = a_x ? InputSampleMs(a_x->inputSample, a_x->simStart) : -1.0f;
						const auto e = EstimatePcLatency(r.frameMs, inputMs, renderMs, r.pclFgMult, r.pclRefreshHz);
						if (!e.valid)
							return std::format(",-1,-1,-1,{:.2f},-1,{:.0f},-1,-1\n", r.pclFgMult, r.pclRefreshHz);
						return std::format(",{:.3f},{},{:.3f},{:.2f},{:.3f},{:.0f},{:.3f},{:.3f}\n", e.inputMs, e.inputMeasured ? 1 : 0, e.renderMs, e.fgMult, e.fgHoldMs,
							e.refreshHz, e.queueMs, e.totalMs);
					};
					std::string extra = extraColumns(r);
					if (!extra.empty() && extra.back() == '\n')
						extra.pop_back();
					extra += pclColumns(it == a_reflex.end() ? nullptr : &it->second);
					// (46x) Point-light classes, after pcl_est_ms (the old last column).
					if (!extra.empty() && extra.back() == '\n')
						extra.pop_back();
					extra += std::format(",{},{},{},{},{}\n", r.rt.plFillDirect, r.rt.plFillAmbient, r.rt.plPortalAway, r.rt.plPortalDropped, r.rt.plPortalBoxed);
					// (47f) Volumetric fog per pass, after rt_pl_portal_boxed (the old last column).
					extra.pop_back();
					extra += std::format(",{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{:.3f},{},{},{},{}\n", r.vfPassMs[0], r.vfPassMs[1], r.vfPassMs[2], r.vfPassMs[3], r.vfPassMs[4],
						r.vfPassMs[5], r.vfGrid[0], r.vfGrid[1], r.vfGrid[2], r.vfGrid[3]);
					// (47j) Water prep / water draws GPU time and the rays traced from the water, after vf_grid_far_z (the old last column).
					extra.pop_back();
					extra += std::format(",{:.3f},{:.3f},{}\n", r.waterPrepGpuMs, r.gpuWaterMs, r.rt.plWaterRays);
					// (47l) Foliage sway, after rt_pl_water_rays (the old last column).
					extra.pop_back();
					extra += std::format(",{},{},{}\n", r.rt.windMeshes, r.rt.windVertices, r.rt.windRefits);
					// (47l) RTAO: AONormalPrepCS GPU time, after rt_wind_refits (the old last column).
					extra.pop_back();
					extra += std::format(",{:.3f}\n", r.aoNormalPrepGpuMs);
					// (47m) The pacer's learned-sleep cap and the part of it not taken, after rtao_normal_prep_gpu_ms (the old last column).
					extra.pop_back();
					extra += std::format(",{:.3f},{:.3f}\n", r.pacer.learnedCapMs, r.pacer.learnedCutMs);
					// (47n) Foliage sway per plant, after pacer_learned_cut_ms (the old last column).
					extra.pop_back();
					extra += std::format(",{},{},{},{},{:.0f}\n", r.rt.windInRadius, r.rt.windSources, r.rt.windBuilds, r.rt.windRefitVertices, r.rt.windRadius);
					// (47p) GPU spike and our periodic jobs, after rt_wind_radius (the old last column).
					extra.pop_back();
					extra += std::format(",{:.3f},{}\n", r.gpuSpikeMs, JobNames(r.jobs));
					if (it == a_reflex.end()) {
						out << (r.reflexFrame == UINT32_MAX ? std::string("-1") : std::to_string(r.reflexFrame)) << ",-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1" << engineColumns(r) << extra;
						continue;
					}
					const auto& x = it->second;
					auto rel = [&](uint64_t v) { return v >= x.simStart && v != 0 ? static_cast<int64_t>(v - x.simStart) : -1; };
					out << std::format("{},{},{},{},{},{},{},{},{},{},{},{},{:.3f},{:.3f},{:.3f}", r.reflexFrame, rel(x.simEnd), rel(x.submitStart), rel(x.submitEnd), rel(x.presentStart),
						rel(x.presentEnd), rel(x.driverStart), rel(x.driverEnd), rel(x.queueStart), rel(x.queueEnd), rel(x.gpuStart), rel(x.gpuEnd),
						x.gpuEnd > x.simStart ? static_cast<double>(x.gpuEnd - x.simStart) / 1000.0 : -1.0, x.gpuActiveUs / 1000.0, x.gpuFrameUs / 1000.0)
						<< engineColumns(r) << extra;
				}
				out.close();
				{
					std::scoped_lock lk(g_fileMutex);
					g_lastFile = name;
				}
				logger::info("[FrameTrace] Wrote {} frames to {}", a_rows.size(), path.string());
			} catch (const std::exception& e) {
				logger::error("[FrameTrace] Writing the trace failed: {}", e.what());
			}
		}

		// ---- D3D11 immediate-context hooks ----
		template <DriverCall C>
		struct Timed
		{
			ID3D11DeviceContext* ctx;
			int64_t start = 0;
			explicit Timed(ID3D11DeviceContext* a_ctx) :
				ctx(a_ctx)
			{
				if (g_active && a_ctx == globals::d3d::context)
					start = Now();
			}
			~Timed()
			{
				if (start) {
					const double ms = Ms(Now() - start);
					g_frame.driver[static_cast<int>(C)] += ms;
					g_frame.driverTotal += ms;
					if (WorldTiming::On())  // (46l) inside the world render
						WorldTiming::AddDriverMs(ms);
					// (46n) Billed to the pass running now (inside cs_passes only).
					using B = EngineTiming::PassBlock;
					constexpr B kBlock = C == DriverCall::Map ? B::Map : C == DriverCall::Upload ? B::Upload :
					                     C == DriverCall::Flush                                  ? B::Flush :
					                                                                               B::Submit;
					EngineTiming::AddPassBlock(kBlock, ms);
				}
			}
		};

		/// (46l) Other immediate-context calls that can block (query read-back, clears, resolves, mips):
		/// timed on their own (driver_misc_*), not part of driver_total.
		struct TimedMisc
		{
			int64_t start = 0;
			explicit TimedMisc(ID3D11DeviceContext* a_ctx)
			{
				if (g_active && a_ctx == globals::d3d::context)
					start = Now();
			}
			~TimedMisc()
			{
				if (start) {
					const double ms = Ms(Now() - start);
					g_frame.driverMisc += ms;
					++g_frame.driverMiscCalls;
					if (WorldTiming::On())
						WorldTiming::AddDriverMs(ms);
					EngineTiming::AddPassBlock(EngineTiming::PassBlock::Misc, ms);  // (46n)
				}
			}
		};
		struct GetData
		{
			static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Asynchronous* a, void* d, UINT s, UINT f)
			{
				TimedMisc t(This);
				if (This != globals::d3d::context)
					return func(This, a, d, s, f);
				const uint64_t t0 = __rdtsc();  // (46n) for the pacer
				const HRESULT hr = func(This, a, d, s, f);
				LatencyPacer::AddD3D11WaitTsc(__rdtsc() - t0);
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ClearRenderTargetView
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11RenderTargetView* v, const FLOAT* c)
			{
				TimedMisc t(This);
				func(This, v, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ClearUnorderedAccessViewUint
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11UnorderedAccessView* v, const UINT* c)
			{
				TimedMisc t(This);
				func(This, v, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ClearUnorderedAccessViewFloat
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11UnorderedAccessView* v, const FLOAT* c)
			{
				TimedMisc t(This);
				func(This, v, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ClearDepthStencilView
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s)
			{
				TimedMisc t(This);
				func(This, v, f, d, s);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct GenerateMips
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11ShaderResourceView* v)
			{
				TimedMisc t(This);
				func(This, v);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct ResolveSubresource
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f)
			{
				TimedMisc t(This);
				func(This, d, ds, s, ss, f);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct DrawIndexed
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, INT c)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Draw
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		/// (45j) Remembers the slowest Map of the frame: what was mapped, how, and by whom.
		void NoteMap(ID3D11Resource* a_resource, D3D11_MAP a_type, UINT a_flags, double a_ms, const void* a_caller)
		{
			++g_frame.mapCalls;
			if (a_ms > 0.5)
				++g_frame.mapSlow;
			if (a_ms <= g_frame.mapWorstMs || a_ms < 0.1)
				return;
			g_frame.mapWorstMs = a_ms;
			g_frame.mapWorstCaller = a_caller;
			static constexpr const char* kTypes[] = { "?", "read", "write", "read-write", "write-discard", "write-no-overwrite" };
			const char* type = static_cast<unsigned>(a_type) < 6 ? kTypes[a_type] : "?";
			const char* noWait = (a_flags & D3D11_MAP_FLAG_DO_NOT_WAIT) ? " no-wait" : "";
			D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
			if (a_resource)
				a_resource->GetType(&dimension);
			if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
				D3D11_BUFFER_DESC d{};
				static_cast<ID3D11Buffer*>(a_resource)->GetDesc(&d);
				std::snprintf(g_frame.mapWorstResource, sizeof(g_frame.mapWorstResource), "buffer %u B usage %u bind 0x%X %s%s", d.ByteWidth,
					static_cast<unsigned>(d.Usage), d.BindFlags, type, noWait);
			} else if (dimension == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
				D3D11_TEXTURE2D_DESC d{};
				static_cast<ID3D11Texture2D*>(a_resource)->GetDesc(&d);
				std::snprintf(g_frame.mapWorstResource, sizeof(g_frame.mapWorstResource), "tex2d %ux%u fmt %u usage %u misc 0x%X %s%s", d.Width, d.Height,
					static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Usage), d.MiscFlags, type, noWait);
			} else {
				std::snprintf(g_frame.mapWorstResource, sizeof(g_frame.mapWorstResource), "resource dim %d %s", static_cast<int>(dimension), type);
			}
		}

		/// Module + offset of a code address ("SkyrimSE.exe+1A2B3C").
		std::string DescribeAddress(const void* a_address)
		{
			if (!a_address)
				return {};
			HMODULE module = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(a_address), &module) ||
				!module)
				return std::format("{:X}", reinterpret_cast<std::uintptr_t>(a_address));
			char path[MAX_PATH]{};
			GetModuleFileNameA(module, path, MAX_PATH);
			const char* slash = std::strrchr(path, '\\');
			return std::format("{}+{:X}", slash ? slash + 1 : path, reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module));
		}

		struct Map
		{
			static HRESULT thunk(ID3D11DeviceContext* This, ID3D11Resource* r, UINT s, D3D11_MAP m, UINT f, D3D11_MAPPED_SUBRESOURCE* o)
			{
				if (!g_active) {
					if (This != globals::d3d::context)
						return func(This, r, s, m, f, o);
					// (46n) The pacer learns waits for the GPU inside the frame (two TSC reads per call).
					const uint64_t t0 = __rdtsc();
					const HRESULT waitHr = func(This, r, s, m, f, o);
					LatencyPacer::AddD3D11WaitTsc(__rdtsc() - t0);
					return waitHr;
				}
				if (This != globals::d3d::context) {
					// (46l) Map on another context (deferred contexts, other threads): counted and timed apart.
					const int64_t otherStart = Now();
					const HRESULT otherHr = func(This, r, s, m, f, o);
					g_mapOtherTicks.fetch_add(Now() - otherStart, std::memory_order_relaxed);
					g_mapOtherCalls.fetch_add(1, std::memory_order_relaxed);
					return otherHr;
				}
				const int64_t start = Now();
				HRESULT hr;
				{
					Timed<DriverCall::Map> t(This);
					hr = func(This, r, s, m, f, o);
				}
				const double mapMs = Ms(Now() - start);
				NoteMap(r, m, f, mapMs, _ReturnAddress());
				LatencyPacer::AddD3D11WaitMs(mapMs);  // (46n)
				return hr;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Unmap
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* r, UINT s)
			{
				Timed<DriverCall::Map> t(This);
				func(This, r, s);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawIndexedInstanced
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, UINT c, INT d, UINT e)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b, c, d, e);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawInstanced
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, UINT c, UINT d)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b, c, d);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawIndexedInstancedIndirect
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Buffer* a, UINT b)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DrawInstancedIndirect
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Buffer* a, UINT b)
			{
				Timed<DriverCall::Draw> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Dispatch
		{
			static void thunk(ID3D11DeviceContext* This, UINT a, UINT b, UINT c)
			{
				Timed<DriverCall::Dispatch> t(This);
				func(This, a, b, c);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct DispatchIndirect
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Buffer* a, UINT b)
			{
				Timed<DriverCall::Dispatch> t(This);
				func(This, a, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct CopySubresourceRegion
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* b)
			{
				Timed<DriverCall::Upload> t(This);
				func(This, d, ds, x, y, z, s, ss, b);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct CopyResource
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, ID3D11Resource* s)
			{
				Timed<DriverCall::Upload> t(This);
				func(This, d, s);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct UpdateSubresource
		{
			static void thunk(ID3D11DeviceContext* This, ID3D11Resource* d, UINT ds, const D3D11_BOX* b, const void* p, UINT rp, UINT dp)
			{
				Timed<DriverCall::Upload> t(This);
				func(This, d, ds, b, p, rp, dp);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct Flush
		{
			static void thunk(ID3D11DeviceContext* This)
			{
				Timed<DriverCall::Flush> t(This);
				const uint64_t t0 = __rdtsc();  // (46n) for the pacer
				func(This);
				if (This == globals::d3d::context)
					LatencyPacer::AddD3D11WaitTsc(__rdtsc() - t0);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	namespace
	{
		// ---- (46p) coarse GPU time per engine segment (recording only) ----
		//
		// A D3D11 timestamp (End only) on the immediate context at every segment change from the render start to the
		// Present end, one degenerate disjoint query per frame (frequency; never nested in a GpuPassTimers interval), read
		// back without waiting (DONOTFLUSH) a few frames later and put on that frame's row. GPU time between two stamps goes
		// to the segment that was open (GPU idle inside the interval included: it is GPU wall time, not GPU work).
		constexpr int kSegSlots = 8;
		constexpr int kSegStamps = 65;  // (47j) 64 intervals, then the close (was 17: interiors ran out early and the rest of the frame went to "shadows")
		constexpr int kSegs = static_cast<int>(EngineTiming::Seg::Count);
		struct SegSlot
		{
			winrt::com_ptr<ID3D11Query> disjoint;
			std::array<winrt::com_ptr<ID3D11Query>, kSegStamps> stamps;
			std::array<int8_t, kSegStamps> seg{};  // the segment that starts at stamp i (-1 = not a render segment / the close)
			// (47j) The water draws: begin / end stamp pairs (each run of Water-shader draws), summed into gpu water ms.
			std::array<winrt::com_ptr<ID3D11Query>, 16> water;
			int waterCount = 0;
			bool waterOpen = false;
			int count = 0;
			uint64_t frameId = 0;
			bool open = false;
			bool pending = false;
		};
		std::array<SegSlot, kSegSlots> g_segSlots;
		int g_segWrite = 0;
		bool g_segBroken = false;  // a query could not be made: off for the session
		ID3D11Device* g_segDevice = nullptr;

		bool IsRenderSeg(int a_seg)
		{
			return a_seg >= static_cast<int>(EngineTiming::Seg::RenderOther) && a_seg <= static_cast<int>(EngineTiming::Seg::Present);
		}

		bool EnsureSegQueries(SegSlot& a_slot)
		{
			auto* device = globals::d3d::device;
			if (!device)
				return false;
			if (g_segDevice != device) {
				for (auto& slot : g_segSlots)
					slot = SegSlot{};
				g_segDevice = device;
			}
			D3D11_QUERY_DESC disjointDesc{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
			D3D11_QUERY_DESC stampDesc{ D3D11_QUERY_TIMESTAMP, 0 };
			if (!a_slot.disjoint && FAILED(device->CreateQuery(&disjointDesc, a_slot.disjoint.put()))) {
				g_segBroken = true;
				return false;
			}
			for (auto& stamp : a_slot.stamps) {
				if (!stamp && FAILED(device->CreateQuery(&stampDesc, stamp.put()))) {
					g_segBroken = true;
					return false;
				}
			}
			for (auto& stamp : a_slot.water) {
				if (!stamp && FAILED(device->CreateQuery(&stampDesc, stamp.put()))) {
					g_segBroken = true;
					return false;
				}
			}
			return true;
		}

		/// GetData without waiting, past our own GetData timer (not a wait of the frame).
		template <class T>
		HRESULT ReadQuery(ID3D11DeviceContext* a_context, ID3D11Query* a_query, T& o_data)
		{
			if (GetData::func.address())
				return GetData::func(a_context, a_query, &o_data, static_cast<UINT>(sizeof(T)), static_cast<UINT>(D3D11_ASYNC_GETDATA_DONOTFLUSH));
			return a_context->GetData(a_query, &o_data, static_cast<UINT>(sizeof(T)), D3D11_ASYNC_GETDATA_DONOTFLUSH);
		}

		void ResetSegSlots()
		{
			for (auto& slot : g_segSlots) {
				slot.open = false;
				slot.pending = false;
				slot.count = 0;
			}
			g_segWrite = 0;
		}

		/// Present end: the frame's last stamp; the slot is read back from now on.
		void CloseSegFrame()
		{
			auto& slot = g_segSlots[g_segWrite];
			if (!slot.open)
				return;
			slot.open = false;
			if (auto* context = globals::d3d::context) {
				if (slot.waterOpen && slot.waterCount < static_cast<int>(slot.water.size())) {  // (47j) a run of water draws to the end of the frame
					context->End(slot.water[slot.waterCount].get());
					++slot.waterCount;
				}
				slot.waterOpen = false;
				context->End(slot.stamps[slot.count].get());
				slot.seg[slot.count] = -1;
				++slot.count;
				slot.pending = true;
			}
			g_segWrite = (g_segWrite + 1) % kSegSlots;
		}

		/// (46p) This row's video memory by owner (created / destroyed since the last row, alive per group), the usage change,
		/// and a log line when the usage moved by more than 500 MB within 4 frames.
		void SampleVramOwners(Row& a_row, const EngineTiming::Frame& a_engine)
		{
			constexpr double kMb = 1.0 / (1024.0 * 1024.0);
			VramSnap now = TakeVramSnap();
			now.usage = a_row.vram.usage;
			now.budget = a_row.vram.budget;
			now.nonLocal = a_row.vram.nonLocal;
			now.createsOther = a_engine.createsOther;
			now.createOtherMs = a_engine.createOtherMs;
			now.row = g_rows.size();
			std::array<double, kVramGroups> live{};
			for (int i = 0; i < kVramOwners; ++i) {
				if (g_vramPrevValid) {
					a_row.vramNewMb[i] = static_cast<float>(static_cast<double>(now.created[i] - g_vramPrev.created[i]) * kMb);
					a_row.vramFreedMb[i] = static_cast<float>(static_cast<double>(now.freed[i] - g_vramPrev.freed[i]) * kMb);
				}
				live[static_cast<int>(GroupOf(i))] += (static_cast<double>(now.created[i]) - static_cast<double>(now.freed[i])) * kMb;
			}
			for (int g = 0; g < kVramGroups; ++g)
				a_row.vramLiveMb[g] = static_cast<float>(live[g]);
			a_row.vramDeltaMb = g_vramPrevValid && g_vramPrev.usage >= 0.0f && now.usage >= 0.0f ? now.usage - g_vramPrev.usage : 0.0f;
			g_vramPrev = now;
			g_vramPrevValid = true;

			g_vramWin[g_vramWinHead] = now;
			g_vramWinHead = (g_vramWinHead + 1) % kVramWindow;
			g_vramWinCount = std::min(g_vramWinCount + 1, kVramWindow);
			if (g_vramJumpCooldown > 0) {
				--g_vramJumpCooldown;
				return;
			}
			if (now.usage < 0.0f || g_vramJumpLogs >= 300)
				return;
			// The largest move from any of the 4 rows before this one.
			const VramSnap* from = nullptr;
			for (size_t k = 1; k < g_vramWinCount; ++k) {
				const auto& s = g_vramWin[(g_vramWinHead + kVramWindow - 1 - k) % kVramWindow];
				if (s.usage < 0.0f)
					continue;
				if (std::abs(now.usage - s.usage) > kVramJumpMb && (!from || std::abs(now.usage - s.usage) > std::abs(now.usage - from->usage)))
					from = &s;
			}
			if (!from)
				return;
			++g_vramJumpLogs;
			g_vramJumpCooldown = static_cast<int>(kVramWindow) - 1;
			double groupNew[kVramGroups]{}, groupFreed[kVramGroups]{};
			std::string owners;
			for (int i = 0; i < kVramOwners; ++i) {
				const double c = static_cast<double>(now.created[i] - from->created[i]) * kMb;
				const double f = static_cast<double>(now.freed[i] - from->freed[i]) * kMb;
				groupNew[static_cast<int>(GroupOf(i))] += c;
				groupFreed[static_cast<int>(GroupOf(i))] += f;
				if (c >= 0.05 || f >= 0.05)
					owners += std::format("{}{} +{:.1f}/-{:.1f}", owners.empty() ? "" : ", ", VramOwnerName(static_cast<VramOwner>(i)), c, f);
			}
			// The engine's Create* calls on other threads in the window (the rows after `from`, and this one).
			uint32_t creates = now.createsOther;
			float createMs = now.createOtherMs;
			for (size_t k = from->row + 1; k < g_rows.size(); ++k) {
				creates += g_rows[k].engine.createsOther;
				createMs += g_rows[k].engine.createOtherMs;
			}
			using G = VramGroup;
			const auto gi = [](G a_g) { return static_cast<int>(a_g); };
			logger::info("[FrameTrace] VRAM jump {:+.0f} MB in {} frames (trace frame {}): local usage {:.0f} -> {:.0f} MB, budget {:.0f} -> {:.0f} MB, "
						 "non-local {:.0f} -> {:.0f} MB; created / destroyed in the window: ours +{:.0f}/-{:.0f} MB, engine D3D11 +{:.0f}/-{:.0f} MB, "
						 "other modules +{:.0f}/-{:.0f} MB, system memory (upload / read-back / staging) +{:.0f}/-{:.0f} MB [{}]; engine Create* on other threads "
						 "{} ({:.1f} ms); alive (tagged): ours {:.0f} MB, engine {:.0f} MB, other modules {:.0f} MB",
				now.usage - from->usage, now.row - from->row, now.row, from->usage, now.usage, from->budget, now.budget, from->nonLocal, now.nonLocal,
				groupNew[gi(G::Ours)], groupFreed[gi(G::Ours)], groupNew[gi(G::Engine)], groupFreed[gi(G::Engine)], groupNew[gi(G::Other)], groupFreed[gi(G::Other)],
				groupNew[gi(G::SysMem)], groupFreed[gi(G::SysMem)], owners.empty() ? std::string("nothing tagged") : owners, creates, createMs,
				live[gi(G::Ours)], live[gi(G::Engine)], live[gi(G::Other)]);
		}

		// (46p) RT prep spikes: rt_prep_gpu_ms over twice its median (last 64 new samples) and 1 ms above it names the work of
		// that submission in the log (at most 200 lines a recording).
		constexpr size_t kPrepRing = 64;
		std::array<float, kPrepRing> g_prepRing{};
		size_t g_prepCount = 0, g_prepHead = 0;
		uint32_t g_prepLogs = 0;
		float g_prepLast = -2.0f;  // the last sample seen (the same read-back is on several rows)
		uint32_t g_prepLastTlas = 0;

		void NotePrepSpike(const Row& a_row, size_t a_index)
		{
			const float ms = a_row.rt.prepGpuMs;
			const auto& w = a_row.rt.prepWork;
			if (ms < 0.0f || !w.valid)
				return;
			if (ms == g_prepLast && w.tlasInstances == g_prepLastTlas)
				return;  // no new read-back since the last row
			g_prepLast = ms;
			g_prepLastTlas = w.tlasInstances;
			float median = -1.0f;
			if (g_prepCount >= 16) {
				std::array<float, kPrepRing> v{};
				std::copy_n(g_prepRing.begin(), g_prepCount, v.begin());
				const auto mid = v.begin() + static_cast<std::ptrdiff_t>(g_prepCount / 2);
				std::nth_element(v.begin(), mid, v.begin() + static_cast<std::ptrdiff_t>(g_prepCount));
				median = *mid;
			}
			g_prepRing[g_prepHead] = ms;
			g_prepHead = (g_prepHead + 1) % kPrepRing;
			g_prepCount = std::min(g_prepCount + 1, kPrepRing);
			if (median < 0.0f || ms <= 2.0f * median || ms < median + 1.0f || g_prepLogs >= 200)
				return;
			++g_prepLogs;
			logger::info("[FrameTrace] RT prep spike {:.2f} ms (median {:.2f}) on trace frame {} ({}): static BLAS builds {} (alpha {}, {} triangles, {:.1f} MB), "
						 "compactions {} ({:.1f} MB); characters: new shapes {}, own-BLAS builds {} / refits {}, merged builds {} ({} triangles) / refits {}, "
						 "skinning dispatches {} ({} vertices); TLAS instances {}; pool blocks adopted {} / released {} since the last submission",
				ms, median, a_index, w.early ? "early prep" : "with the trace", w.builds, w.buildsAlpha, w.buildTriangles,
				static_cast<double>(w.buildBytes) / (1024.0 * 1024.0), w.compactions, static_cast<double>(w.compactBytes) / (1024.0 * 1024.0), w.skinUploads,
				w.skinBuilds, w.skinRefits, w.groupBuilds, w.groupTriangles, w.groupRefits, w.skinDispatches, w.skinVertices, w.tlasInstances, w.poolBlocks,
				w.poolReleased);
		}

		/// The closed frames whose stamps are in, oldest first, onto their rows (never waits).
		void CollectSegFrames()
		{
			auto* context = globals::d3d::context;
			if (!context)
				return;
			for (int age = 0; age < kSegSlots; ++age) {
				auto& slot = g_segSlots[(g_segWrite + age) % kSegSlots];
				if (!slot.pending || slot.open)
					continue;
				D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
				HRESULT hr = ReadQuery(context, slot.disjoint.get(), disjoint);
				if (hr == S_FALSE)
					continue;
				std::array<UINT64, kSegStamps> ticks{};
				bool ready = SUCCEEDED(hr);
				bool waiting = false;
				for (int i = 0; ready && i < slot.count; ++i) {
					hr = ReadQuery(context, slot.stamps[i].get(), ticks[i]);
					if (hr == S_FALSE)
						waiting = true;
					if (hr != S_OK)
						ready = false;
				}
				std::array<UINT64, 16> waterTicks{};  // (47j)
				for (int i = 0; ready && i < slot.waterCount; ++i) {
					hr = ReadQuery(context, slot.water[i].get(), waterTicks[i]);
					if (hr == S_FALSE)
						waiting = true;
					if (hr != S_OK)
						ready = false;
				}
				if (waiting)
					continue;  // asked again next frame
				slot.pending = false;
				if (!ready || disjoint.Disjoint || disjoint.Frequency == 0 || slot.count < 2)
					continue;
				const double toMs = 1000.0 / static_cast<double>(disjoint.Frequency);
				float ms[kSegs];
				std::fill(std::begin(ms), std::end(ms), -1.0f);
				float other = 0.0f;
				bool ordered = true;
				for (int i = 0; i + 1 < slot.count; ++i) {
					if (ticks[i + 1] < ticks[i]) {
						ordered = false;
						break;
					}
					const float d = static_cast<float>(static_cast<double>(ticks[i + 1] - ticks[i]) * toMs);
					const int seg = slot.seg[i];
					if (seg >= 0 && seg < kSegs)
						ms[seg] = std::max(ms[seg], 0.0f) + d;
					else
						other += d;
				}
				if (!ordered)
					continue;
				int looked = 0;
				for (auto it = g_rows.rbegin(); it != g_rows.rend() && looked < 32; ++it, ++looked) {
					if (it->engine.id != slot.frameId)
						continue;
					std::copy(std::begin(ms), std::end(ms), it->gpuSegMs);
					it->gpuSegOtherMs = other;
					it->gpuSegTotalMs = static_cast<float>(static_cast<double>(ticks[slot.count - 1] - ticks[0]) * toMs);
					it->gpuSegStamps = slot.count;
					// (47j) The water draws: the stamp pairs summed (0 = no water drawn).
					double water = 0.0;
					for (int i = 0; i + 1 < slot.waterCount; i += 2)
						if (waterTicks[i + 1] >= waterTicks[i])
							water += static_cast<double>(waterTicks[i + 1] - waterTicks[i]) * toMs;
					it->gpuWaterMs = static_cast<float>(water);
					break;
				}
			}
		}
	}

	int64_t Now()
	{
		LARGE_INTEGER t;
		QueryPerformanceCounter(&t);
		return t.QuadPart;
	}

	void InstallContextHooks(ID3D11DeviceContext* a_context)
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_frequency = f.QuadPart;
		g_toMs = g_frequency ? 1000.0 / static_cast<double>(g_frequency) : 0.0;

		stl::detour_vfunc<12, DrawIndexed>(a_context);
		stl::detour_vfunc<13, Draw>(a_context);
		stl::detour_vfunc<14, Map>(a_context);
		stl::detour_vfunc<15, Unmap>(a_context);
		stl::detour_vfunc<20, DrawIndexedInstanced>(a_context);
		stl::detour_vfunc<21, DrawInstanced>(a_context);
		stl::detour_vfunc<39, DrawIndexedInstancedIndirect>(a_context);
		stl::detour_vfunc<40, DrawInstancedIndirect>(a_context);
		stl::detour_vfunc<41, Dispatch>(a_context);
		stl::detour_vfunc<42, DispatchIndirect>(a_context);
		stl::detour_vfunc<46, CopySubresourceRegion>(a_context);
		stl::detour_vfunc<47, CopyResource>(a_context);
		stl::detour_vfunc<48, UpdateSubresource>(a_context);
		stl::detour_vfunc<111, Flush>(a_context);
		// (46l) Other calls that can block, timed apart (driver_misc_*).
		stl::detour_vfunc<29, GetData>(a_context);
		stl::detour_vfunc<50, ClearRenderTargetView>(a_context);
		stl::detour_vfunc<51, ClearUnorderedAccessViewUint>(a_context);
		stl::detour_vfunc<52, ClearUnorderedAccessViewFloat>(a_context);
		stl::detour_vfunc<53, ClearDepthStencilView>(a_context);
		stl::detour_vfunc<54, GenerateMips>(a_context);
		stl::detour_vfunc<57, ResolveSubresource>(a_context);
		logger::info("[FrameTrace] D3D11 call timing hooks installed");
	}

	bool Active() { return g_active; }
	bool IsRecording() { return g_recording; }

	double DriverMsSoFar() { return g_frame.driverTotal; }

	void AddOwnDrawHook(int64_t a_start, double a_driverAtStart, int a_type)
	{
		if (!g_active)
			return;
		const double ms = Ms(Now() - a_start) - (g_frame.driverTotal - a_driverAtStart);
		g_frame.ownDrawHook += std::max(ms, 0.0);
		if (WorldTiming::On())  // (46l)
			WorldTiming::AddOwnDrawMs(std::max(ms, 0.0));
		++g_frame.draws;
		if (a_type == static_cast<int>(RE::BSShader::Type::Lighting))
			++g_frame.lightingDraws;
		else if (a_type == static_cast<int>(RE::BSShader::Type::Utility))
			++g_frame.utilityDraws;
	}

	void OnLightingGeometry(const void* a_geometry)
	{
		if (!g_active)
			return;
		if (g_seenGeometry.size() > 2000000)
			g_seenGeometry.clear();
		if (g_seenGeometry.insert(a_geometry).second)
			++g_frame.firstSeen;
	}

	void OnShaderMiss(bool a_async)
	{
		(a_async ? g_shaderAsync : g_shaderSync).fetch_add(1, std::memory_order_relaxed);
	}

	void OnSimStart()
	{
		g_simStart = Now();
	}

	SeriesStats GetSeriesStats(float a_seconds)
	{
		SeriesStats s;
		if (!g_histCount || !g_frequency)
			return s;
		const int64_t cutoff = Now() - static_cast<int64_t>(static_cast<double>(a_seconds) * static_cast<double>(g_frequency));
		std::vector<float> cpu, gpu;
		cpu.reserve(g_histCount);
		gpu.reserve(g_histCount);
		int cpuLimited = 0, both = 0;
		double waitSum = 0.0;
		for (size_t i = 0; i < g_histCount; ++i) {
			const auto& e = g_hist[(g_histHead + kHist - 1 - i) % kHist];
			if (e.t < cutoff)
				break;
			if (e.cpuMs > 0.0f) {
				cpu.push_back(e.cpuMs);
				waitSum += e.gpuWaitMs;
			}
			if (e.gpuMs > 0.0f)
				gpu.push_back(e.gpuMs);
			if (e.cpuMs > 0.0f && e.gpuMs > 0.0f) {
				++both;
				// (47h) The CPU's own work: its time waiting for the GPU inside the frame is the GPU's (GPU-bound with DLSS-G, the
				// render thread blocks in D3D11 calls and read the CPU as the limit before).
				if (e.cpuMs - e.gpuWaitMs > e.gpuMs)
					++cpuLimited;
			}
		}
		s.cpuGpuWaitAvg = cpu.empty() ? 0.0f : static_cast<float>(waitSum / static_cast<double>(cpu.size()));
		auto stats = [](std::vector<float>& v, float& avg, float& low1, float& mx) {
			if (v.empty())
				return;
			double sum = 0.0;
			for (float x : v)
				sum += x;
			avg = static_cast<float>(sum / static_cast<double>(v.size()));
			const size_t k = std::min(v.size() - 1, static_cast<size_t>(static_cast<double>(v.size()) * 0.99));
			std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
			low1 = v[k];
			mx = *std::max_element(v.begin(), v.end());
		};
		s.frames = static_cast<int>(cpu.size());
		s.gpuFrames = static_cast<int>(gpu.size());
		stats(cpu, s.cpuAvg, s.cpuLow1, s.cpuMax);
		stats(gpu, s.gpuAvg, s.gpuLow1, s.gpuMax);
		s.cpuLimitedPct = both ? 100.0f * static_cast<float>(cpuLimited) / static_cast<float>(both) : 0.0f;
		return s;
	}

	void GetSeries(std::vector<float>& o_cpu, std::vector<float>& o_gpu, int a_count)
	{
		o_cpu.clear();
		o_gpu.clear();
		const size_t n = std::min(g_histCount, static_cast<size_t>(std::max(a_count, 0)));
		for (size_t i = 0; i < n; ++i) {
			const auto& e = g_hist[(g_histHead + kHist - n + i) % kHist];
			o_cpu.push_back(e.cpuMs);
			o_gpu.push_back(e.gpuMs);
		}
	}

	void AddOwnSetupGeometry(int64_t a_start, double a_driverAtStart)
	{
		if (!g_active)
			return;
		const double ms = Ms(Now() - a_start) - (g_frame.driverTotal - a_driverAtStart);
		g_frame.ownSetupGeometry += std::max(ms, 0.0);
	}

	void AddPermutationUpload(int64_t a_start)
	{
		if (g_active)
			g_frame.permutationUpload += Ms(Now() - a_start);
	}

	void OnPacerSleep(int64_t a_start)
	{
		if (g_active)
			g_frame.pacerSleep += Ms(Now() - a_start);
	}

	void OnReflexSleep(int64_t a_start)
	{
		if (g_active)
			g_frame.reflexSleep += Ms(Now() - a_start);
	}

	void OnFramesPresented(uint32_t a_count) { g_frame.fgPresented = a_count; }
	void OnGrassApply(float a_ms) { g_frame.grassApplyMs = a_ms; }
	void OnRayTracing(float a_gpuMs, float a_cpuMs, uint32_t a_builds)
	{
		g_frame.rtGpuMs = a_gpuMs;
		g_frame.rtCpuMs = a_cpuMs;
		g_frame.rtBuilds = a_builds;
	}

	void OnRayTracingDetail(const RayTracingDetail& a_detail) { g_frame.rt = a_detail; }
	// (46q)
	void On46qRt(const Q46& a_rt)
	{
		g_q46.subOn = a_rt.subOn;
		g_q46.subBatches = a_rt.subBatches;
		g_q46.subMaxBatchMs = a_rt.subMaxBatchMs;
		g_q46.subMaxLagMs = a_rt.subMaxLagMs;
		g_q46.subMaxCallMs = a_rt.subMaxCallMs;
		g_q46.budOn = a_rt.budOn;
		g_q46.budMsPerMtri = a_rt.budMsPerMtri;
		g_q46.budTris = a_rt.budTris;
		g_q46.budStaticQueued = a_rt.budStaticQueued;
		g_q46.budGroupsDeferred = a_rt.budGroupsDeferred;
		g_q46.budGroupsKept = a_rt.budGroupsKept;
	}
	void On46qSnow(float a_ms, int a_state)
	{
		g_q46.snowShapesMs += a_ms;
		g_q46.snowShapesState = a_state;
	}
	void On46qRenderRes(int a_b, int a_c, uint32_t a_width, uint32_t a_height)
	{
		g_q46.rrB = a_b;
		g_q46.rrC = a_c;
		g_q46.rrWidth = a_width;
		g_q46.rrHeight = a_height;
	}
	void OnLights(const LightStats& a_stats) { g_frame.lights = a_stats; }

	namespace
	{
		std::string FormName(RE::TESForm* a_form)
		{
			if (!a_form)
				return {};
			const char* edid = a_form->GetFormEditorID();
			if (edid && *edid)
				return edid;
			const char* name = a_form->GetName();
			if (name && *name)
				return name;
			return std::format("{:08X}", a_form->GetFormID());
		}

		/// CSV-safe: no commas, quotes or line breaks.
		std::string CsvSafe(std::string a_text)
		{
			for (char& c : a_text)
				if (c == ',' || c == '"' || c == '\n' || c == '\r')
					c = ';';
			return a_text;
		}
	}

	Location CurrentLocation()
	{
		Location location;
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player)
			return location;
		const auto position = player->GetPosition();
		location.x = static_cast<int>(std::lround(position.x));
		location.y = static_cast<int>(std::lround(position.y));
		location.z = static_cast<int>(std::lround(position.z));
		// Names only change with the cell / worldspace / weather: cached by pointer.
		static const void* lastCell = nullptr;
		static const void* lastWorld = nullptr;
		static const void* lastWeather = nullptr;
		static std::string cellName, worldName, weatherName;
		static bool interior = false;
		auto* cell = player->GetParentCell();
		if (cell != lastCell) {
			lastCell = cell;
			cellName = CsvSafe(FormName(cell));
			interior = cell && cell->IsInteriorCell();
		}
		auto* world = player->GetWorldspace();
		if (world != lastWorld) {
			lastWorld = world;
			worldName = CsvSafe(FormName(world));
		}
		auto* sky = RE::Sky::GetSingleton();
		auto* weather = sky ? sky->currentWeather : nullptr;
		if (weather != lastWeather) {
			lastWeather = weather;
			weatherName = CsvSafe(FormName(weather));
		}
		location.cell = cellName;
		location.world = worldName;
		location.weather = weatherName;
		location.interior = interior;
		if (auto* calendar = RE::Calendar::GetSingleton())
			location.hour = calendar->GetHour();
		return location;
	}

	std::string LocationTag(const Location& a_location)
	{
		const std::string& source = a_location.interior || a_location.world.empty() ? a_location.cell : a_location.world;
		std::string tag;
		for (const char c : source) {
			if (std::isalnum(static_cast<unsigned char>(c)))
				tag += c;
			if (tag.size() >= 24)
				break;
		}
		return tag;
	}

	void NoteJob(Job a_job)
	{
		const auto bit = static_cast<uint32_t>(a_job);
		if (bit < static_cast<uint32_t>(Job::Count))
			g_jobsThisFrame.fetch_or(1u << bit, std::memory_order_relaxed);
	}

	namespace
	{
		/// (47p) The GPU spike watch (runs while the GPU frame timer does: overlay shown or F7 recording). Returns the excess
		/// over the usual GPU work (0 = no spike) and logs spikes with the time since the previous one and our jobs of that
		/// frame and the one before; three intervals in a row within 10 % of each other are reported as a periodic interruption.
		float WatchGpuSpike(uint64_t a_gpuFrameIndex, float a_workMs)
		{
			auto& w = g_spike;
			if (!(a_workMs > 0.0f) || a_workMs > 1000.0f)
				return 0.0f;
			if (w.learned < 30) {
				w.usualMs = w.learned == 0 ? a_workMs : w.usualMs * 0.9f + a_workMs * 0.1f;
				++w.learned;
				return 0.0f;
			}
			const float excess = a_workMs - w.usualMs;
			if (!(excess >= 12.0f && a_workMs >= 1.4f * w.usualMs)) {
				w.usualMs = w.usualMs * 0.97f + a_workMs * 0.03f;
				return 0.0f;
			}
			const int64_t now = Now();
			float sinceS = -1.0f;
			if (w.lastSpikeQpc != 0 && g_frequency != 0)
				sinceS = static_cast<float>(static_cast<double>(now - w.lastSpikeQpc) / static_cast<double>(g_frequency));
			w.lastSpikeQpc = now;
			if (sinceS > 0.0f) {
				for (size_t i = w.intervals.size() - 1; i > 0; --i)
					w.intervals[i] = w.intervals[i - 1];
				w.intervals[0] = sinceS;
				w.intervalCount = std::min(w.intervalCount + 1, static_cast<int>(w.intervals.size()));
			}
			const uint32_t jobs = JobsOfGpuFrame(a_gpuFrameIndex) | JobsOfGpuFrame(a_gpuFrameIndex - 1);
			if (w.logs < kSpikeLogLimit) {
				++w.logs;
				logger::info("[FrameTrace] (47p) GPU spike: +{:.1f} ms over the usual {:.1f} ms (GPU frame {}), {}; our periodic jobs in it and the frame before: {}",
					excess, w.usualMs, a_gpuFrameIndex, sinceS > 0.0f ? std::format("{:.2f} s after the previous spike", sinceS) : std::string("the first one seen"),
					jobs ? JobNames(jobs) : std::string("none"));
			}
			if (w.intervalCount >= 3) {
				const float mean = (w.intervals[0] + w.intervals[1] + w.intervals[2]) / 3.0f;
				bool periodic = mean >= 1.0f;
				for (int i = 0; i < 3 && periodic; ++i)
					periodic = std::abs(w.intervals[i] - mean) <= 0.1f * mean;
				if (periodic && !w.periodicLogged) {
					w.periodicLogged = true;
					logger::warn("[FrameTrace] (47p) Periodic GPU interruption: about every {:.2f} s (+{:.0f} ms each). {}", mean, excess,
						jobs ? std::format("Our job(s) {} ran with it.", JobNames(jobs)) :
							   std::string("None of our periodic jobs ran with it: another GPU user (another queue, overlay, monitoring tool) is the likely cause."));
				}
			}
			return excess;
		}
	}

	void OnGpuSample(uint64_t a_gpuFrameIndex, float a_workMs, float a_elapsedMs)
	{
		for (size_t i = 0; i < std::min<size_t>(g_histCount, 16); ++i) {
			auto& e = g_hist[(g_histHead + kHist - 1 - i) % kHist];
			if (e.gpuIndex == a_gpuFrameIndex) {
				e.gpuMs = a_workMs;
				break;
			}
		}
		const float spike = WatchGpuSpike(a_gpuFrameIndex, a_workMs);  // (47p)
		if (!g_recording)
			return;
		for (auto it = g_rows.rbegin(); it != g_rows.rend() && it - g_rows.rbegin() < 16; ++it) {
			if (it->gpuFrameIndex == a_gpuFrameIndex) {
				it->gpuWorkMs = a_workMs;
				it->gpuElapsedMs = a_elapsedMs;
				it->gpuSpikeMs = spike;
				return;
			}
		}
	}

	void OnPassGpuSample(uint64_t a_frameId, int a_bucket, float a_ms)
	{
		if (!g_recording)
			return;
		int looked = 0;
		for (auto it = g_rows.rbegin(); it != g_rows.rend() && looked < 32; ++it, ++looked) {
			if (it->engine.id == a_frameId) {
				ApplyPassGpu(*it, a_bucket, a_ms);
				return;
			}
			if (it->engine.id < a_frameId)
				break;
		}
		// The frame's row is not written yet (its own later passes collected it): kept until it is.
		if ((g_rows.empty() || a_frameId > g_rows.back().engine.id) && g_pendingPassGpu.size() < 4096)
			g_pendingPassGpu.push_back({ a_frameId, a_bucket, a_ms });
	}

	void OnPresentEnd(int64_t a_presentStart, uint64_t a_gpuFrameIndex)
	{
		const int64_t now = Now();
		const bool wasActive = g_active;
		// (47p) Our periodic jobs of this frame, by GPU frame index (the spike watch looks them up when the GPU sample arrives).
		const uint32_t frameJobs = g_jobsThisFrame.exchange(0, std::memory_order_relaxed);
		g_jobRing[g_jobRingHead] = { a_gpuFrameIndex, frameJobs };
		g_jobRingHead = (g_jobRingHead + 1) % kJobRing;

		if (!g_cellSinkRegistered) {
			if (auto* holder = RE::ScriptEventSourceHolder::GetSingleton()) {
				holder->AddEventSink<RE::TESCellAttachDetachEvent>(&g_cellSink);
				g_cellSinkRegistered = true;
			}
		}

		// (46n) The post-load window: the first kPassWindowFrames frames after the loading screen closes list
		// every pass of cs_passes in the trace (frames with cs_passes over kPassSpikeMs always do).
		bool passWindow = false;
		if (g_recording) {
			auto* ui = globals::game::ui;
			const bool loadingOpen = ui && ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME);
			if (loadingOpen)
				g_passSinceLoad = -1;
			else if (g_loadingWasOpen)
				g_passSinceLoad = 0;
			else if (g_passSinceLoad >= 0 && ++g_passSinceLoad >= kPassWindowFrames)
				g_passSinceLoad = -1;
			g_loadingWasOpen = loadingOpen;
			passWindow = g_passSinceLoad >= 0;
		}

		// (44f) Close the engine CPU split for this frame (also resets it while inactive).
		const auto engine = EngineTiming::TakeFrame(wasActive && g_recording, passWindow);
		// (46l) Close the world-render split too (also resets it while not recording).
		auto world = WorldTiming::TakeFrame(g_recording);

		if (wasActive && g_lastPresentEnd) {
			const float frameMs = static_cast<float>(Ms(now - g_lastPresentEnd));
			const float presentWait = static_cast<float>(Ms(now - a_presentStart));
			// CPU frame: simulation start (after the Reflex sleep) to Present start. Without a
			// simulation marker this frame, from the previous Present's end minus the sleep.
			float cpuFrameMs = (g_simStart > g_lastPresentEnd && g_simStart < a_presentStart) ?
			                       static_cast<float>(Ms(a_presentStart - g_simStart)) :
			                       std::max(0.0f, static_cast<float>(Ms(a_presentStart - g_lastPresentEnd) - g_frame.reflexSleep - g_frame.pacerSleep));

			// Camera turn rate and player speed, per real frame.
			float yawRate = 0.0f, pitchRate = 0.0f, speed = 0.0f;
			if (auto* cam = RE::Main::WorldRootCamera()) {
				const auto& r = cam->world.rotate;
				const float fx = r.entry[0][0], fy = r.entry[1][0], fz = r.entry[2][0];
				const float yaw = std::atan2(fy, fx) * 57.29578f;
				const float pitch = std::asin(std::clamp(fz, -1.0f, 1.0f)) * 57.29578f;
				if (g_motionValid && frameMs > 0.0f) {
					float dy = yaw - g_lastYaw;
					dy -= 360.0f * std::round(dy / 360.0f);
					yawRate = std::abs(dy) * 1000.0f / frameMs;
					pitchRate = std::abs(pitch - g_lastPitch) * 1000.0f / frameMs;
				}
				g_lastYaw = yaw;
				g_lastPitch = pitch;
			}
			if (auto* player = RE::PlayerCharacter::GetSingleton()) {
				const auto pos = player->GetPosition();
				if (g_motionValid && frameMs > 0.0f)
					speed = pos.GetDistance(g_lastPlayerPos) * 1000.0f / frameMs;
				g_lastPlayerPos = pos;
			}
			g_motionValid = true;

			{
				auto& e = g_hist[g_histHead];
				e = {};
				e.t = now;
				e.cpuMs = cpuFrameMs;
				e.gpuIndex = a_gpuFrameIndex;
				// (47h) Blocking D3D11 calls and the render segments' hidden wait (when the frame started with the GPU behind): GPU
				// time as far as the bottleneck reading goes. Never more than the CPU frame itself.
				e.gpuWaitMs = std::clamp(LatencyPacer::FrameGpuWaitMs(), 0.0f, cpuFrameMs);
				g_histHead = (g_histHead + 1) % kHist;
				g_histCount = std::min(g_histCount + 1, kHist);
			}

			const uint32_t shaderAsync = g_shaderAsync.exchange(0, std::memory_order_relaxed);
			const uint32_t shaderSync = g_shaderSync.exchange(0, std::memory_order_relaxed);
			const uint32_t cellAttach = g_cellAttach.exchange(0, std::memory_order_relaxed);
			const uint32_t cellDetach = g_cellDetach.exchange(0, std::memory_order_relaxed);
			const int64_t mapOtherTicks = g_mapOtherTicks.exchange(0, std::memory_order_relaxed);  // (46l)
			const uint32_t mapOtherCalls = g_mapOtherCalls.exchange(0, std::memory_order_relaxed);

			if (g_recording) {
				Row r;
				r.cpuFrameMs = cpuFrameMs;
				r.utilityDraws = g_frame.utilityDraws;
				r.firstSeen = g_frame.firstSeen;
				r.shaderAsync = shaderAsync;
				r.shaderSync = shaderSync;
				r.jobs = frameJobs;  // (47p)
				r.cellAttach = cellAttach;
				r.cellDetach = cellDetach;
				r.yawRate = yawRate;
				r.pitchRate = pitchRate;
				r.playerSpeed = speed;
				g_pendingRow = r;
				g_hasPendingRow = true;
			}

			if (frameMs > 0.0f && frameMs < 1000.0f) {
				auto ema = [](float& s, float v) { s = s * 0.95f + v * 0.05f; };
				auto& s = g_smoothed;
				if (!s.valid) {
					s = {};
					s.valid = true;
					s.frameMs = frameMs;
				}
				ema(s.frameMs, frameMs);
				ema(s.presentWaitMs, presentWait);
				ema(s.reflexSleepMs, static_cast<float>(g_frame.reflexSleep));
				for (int i = 0; i < kCalls; ++i)
					ema(s.driverMs[i], static_cast<float>(g_frame.driver[i]));
				ema(s.driverTotalMs, static_cast<float>(g_frame.driverTotal));
				ema(s.ownDrawHookMs, static_cast<float>(g_frame.ownDrawHook));
				ema(s.ownSetupGeometryMs, static_cast<float>(g_frame.ownSetupGeometry));
				ema(s.permutationUploadMs, static_cast<float>(g_frame.permutationUpload));
				ema(s.draws, static_cast<float>(g_frame.draws));
				ema(s.lightingDraws, static_cast<float>(g_frame.lightingDraws));
				s.ownPerDrawUs = s.draws > 0.5f ? (s.ownDrawHookMs + s.ownSetupGeometryMs) * 1000.0f / s.draws : 0.0f;
				// (44f) Engine CPU split: EMA, and a peak that decays over about two seconds.
				s.engineValid = engine.valid;
				if (engine.valid) {
					static_assert(static_cast<int>(EngineTiming::Seg::Count) == 13);
					for (int i = 0; i < static_cast<int>(EngineTiming::Seg::Count); ++i) {
						ema(s.engineMs[i], engine.ms[i]);
						s.enginePeakMs[i] = std::max(engine.ms[i], s.enginePeakMs[i] * 0.99f);
					}
					ema(s.engineInputMs, engine.inputMs);
					if (engine.topCallMs > 5.0f) {
						s.engineTopCallId = engine.topCallId;
						s.engineTopCallOffset = engine.topCallOffset;
						s.engineTopCallMs = engine.topCallMs;
					}
				}
				if (engine.engineDtMs >= 0.0f)
					ema(s.engineDtMs, engine.engineDtMs);
				ema(s.createMs, engine.createMs);
			}
			if ((g_frameCounter++ & 15) == 0)
				PollLatency();
			// (46q) The snow column is this frame's ms: not carried from before the recording started.
			const float snowShapesMsThisFrame = g_q46.snowShapesMs;
			g_q46.snowShapesMs = 0.0f;

			if (g_recording) {
				Row r = g_hasPendingRow ? g_pendingRow : Row{};
				g_hasPendingRow = false;
				r.tMs = Ms(now - g_recordStart);
				r.frameMs = frameMs;
				r.presentWaitMs = presentWait;
				r.reflexSleepMs = static_cast<float>(g_frame.reflexSleep);
				r.pacerSleepMs = static_cast<float>(g_frame.pacerSleep);
				for (int i = 0; i < kCalls; ++i)
					r.driver[i] = static_cast<float>(g_frame.driver[i]);
				r.ownDrawHookMs = static_cast<float>(g_frame.ownDrawHook);
				r.ownSetupGeometryMs = static_cast<float>(g_frame.ownSetupGeometry);
				r.permutationUploadMs = static_cast<float>(g_frame.permutationUpload);
				r.draws = g_frame.draws;
				r.lightingDraws = g_frame.lightingDraws;
				r.fgPresented = g_frame.fgPresented;
				r.grassApplyMs = g_frame.grassApplyMs;
				r.rtGpuMs = g_frame.rtGpuMs;
				r.rtCpuMs = g_frame.rtCpuMs;
				r.rtBuilds = g_frame.rtBuilds;
				r.rt = g_frame.rt;
				r.lights = g_frame.lights;
				SampleScreenSpacePasses(r);  // (46e)
				r.postGpuMs = Util::GpuPassTimers::GetSingleton()->LastSampleMs(Util::GpuBucket::PostProcessing);  // (46p)
				r.q46 = g_q46;  // (46q)
				r.q46.snowShapesMs = snowShapesMsThisFrame;
				r.vramByFeature = VramByFeatureColumn();
				r.pclFgMult = CurrentFgMultiplier();  // (46u)
				r.pclRefreshHz = static_cast<float>(globals::features::upscaling.refreshRate);
				r.cpuTop = Util::CpuPassTimers::GetSingleton()->TopOfCurrentFrame(3);
				r.world = std::move(world);
				r.driverMiscMs = static_cast<float>(g_frame.driverMisc);
				r.driverMiscCalls = g_frame.driverMiscCalls;
				r.mapOtherMs = static_cast<float>(Ms(mapOtherTicks));
				r.mapOtherCalls = mapOtherCalls;
				r.mapCalls = g_frame.mapCalls;
				r.mapSlow = g_frame.mapSlow;
				r.mapWorstMs = static_cast<float>(g_frame.mapWorstMs);
				if (g_frame.mapWorstMs > 0.0) {
					r.mapWorstResource = g_frame.mapWorstResource;
					for (char& c : r.mapWorstResource)
						if (c == ',')
							c = ';';
					r.mapWorstCaller = DescribeAddress(g_frame.mapWorstCaller);
				}
				{
					// (45j) Where the player is, shared between rows while nothing changes.
					static std::shared_ptr<const Location> last;
					auto here = CurrentLocation();
					if (!last || last->world != here.world || last->cell != here.cell || last->weather != here.weather || last->x != here.x ||
						last->y != here.y || last->z != here.z || std::abs(last->hour - here.hour) > 0.01f)
						last = std::make_shared<const Location>(std::move(here));
					r.location = last;
				}
				r.gpuFrameIndex = a_gpuFrameIndex;
				r.reflexFrame = globals::features::upscaling.streamline.frameTokenIndex;
				r.engine = engine;
				r.reflexMarkersAtRenderStart = globals::features::upscaling.WantsReflexMarkersAtRenderStart() && EngineTiming::ProbesInstalled();
				{
					auto& up = globals::features::upscaling;
					r.pacer = LatencyPacer::Last();
					r.reflexSleepOk = up.streamline.lastReflexSleepOk;
					r.lowLatencyAvailable = up.streamline.lastLowLatencyAvailable;
					r.reflexModeApplied = up.GetReflexModeApplied() == UINT_MAX ? -1 : static_cast<int>(up.GetReflexModeApplied());
					r.reflexLimitApplied = up.GetReflexFrameLimitApplied() == UINT_MAX ? -1 : static_cast<int>(up.GetReflexFrameLimitApplied());
					r.latencyFix = true;  // (45o) hard-wired on
				}
				// (45j) A spike frame names its suspects in the log (at most one line a second).
				if (frameMs > 1.5f * g_smoothed.frameMs + 5.0f) {
					static int64_t lastSpikeLog = 0;
					if (Ms(now - lastSpikeLog) > 1000.0) {
						lastSpikeLog = now;
						logger::info("[FrameTrace] Spike {:.1f} ms (avg {:.1f}): our CPU features [{}], RT CPU {:.2f} ms (scene {:.2f}, chars {:.2f}, exec {:.2f}), "
									 "slowest Map {:.2f} ms ({} from {}), engine CS passes {:.2f} ms, device creates on other threads {:.2f} ms",
							frameMs, g_smoothed.frameMs, r.cpuTop, r.rtCpuMs, r.rt.scene, r.rt.characters, r.rt.execute, r.mapWorstMs, r.mapWorstResource,
							r.mapWorstCaller, engine.valid ? engine.ms[static_cast<int>(EngineTiming::Seg::OurPasses)] : -1.0f, engine.createOtherMs);
					}
				}
				// (46l) A slow world render names its three costliest places and the split by kind.
				if (engine.valid && engine.ms[static_cast<int>(EngineTiming::Seg::World)] > 10.0f && r.world.valid && g_worldSpikeLogs < 400) {
					++g_worldSpikeLogs;
					const auto& w = r.world;
					logger::info("[FrameTrace] World spike {:.1f} ms (split {:.1f}): top places [{}] kinds [{}] of which D3D11 {:.2f}, State::Draw {:.2f}, "
								 "lookup wait {:.2f} ({}), misses {:.2f} ({}), skin {:.2f} ({} chars), shelter {:.2f} ({}), water {:.2f} ({}), creates {:.2f} ({}); "
								 "passes {} techniques {} draws {}",
						engine.ms[static_cast<int>(EngineTiming::Seg::World)], w.totalMs, w.top3, w.kindsText, w.driverMs, w.ownDrawMs, w.lookupMs, w.lookups,
						w.missMs, w.misses, w.skinMs, w.skinActors, w.shelterMs, w.shelterPicks, w.waterMs, w.waterSearches, w.createMs, w.creates, w.passes,
						w.techniques, r.draws);
				}
				// (46n) cs_passes split: the window, GPU samples that came in early, the spike log (once its GPU samples are in).
				r.passSinceLoad = passWindow ? g_passSinceLoad : -1;
				r.passLogPending = engine.valid && engine.ms[static_cast<int>(EngineTiming::Seg::OurPasses)] > EngineTiming::kPassSpikeMs &&
				                   !engine.passes.empty() && g_passLogs < kMaxPassLogs;
				{
					// (46o) Sub-timers, video memory (every 4th frame), out-of-VRAM warnings, flags, stall samples.
					std::copy_n(g_frame.split, static_cast<int>(Split::Count), r.split);
					{
						// (46p) Every frame (was every 4th), with its cost.
						const int64_t q0 = Now();
						g_vram = QueryVram();
						r.vramQueryUs = static_cast<float>(Ms(Now() - q0) * 1000.0);
						++g_vramFrame;
					}
					r.vram = g_vram;
					SampleVramOwners(r, engine);
					r.vramWarn = g_vramWarn.exchange(0, std::memory_order_relaxed);
					if (auto* ui = globals::game::ui) {
						r.flags |= ui->IsMenuOpen(RE::LoadingMenu::MENU_NAME) ? kFlagLoading : 0u;
						r.flags |= ui->IsMenuOpen(RE::MapMenu::MENU_NAME) ? kFlagMap : 0u;
						r.flags |= ui->GameIsPaused() ? kFlagPaused : 0u;
						r.flags |= (ui->IsApplicationMenuOpen() || ui->IsItemMenuOpen() || ui->IsModalMenuOpen()) ? kFlagMenu : 0u;
						r.flags |= ui->IsMenuOpen(RE::FaderMenu::MENU_NAME) ? kFlagFader : 0u;
					}
					r.flags |= r.pacer.reset ? kFlagPacerReset : 0u;
					r.stallStack = EngineTiming::TakeStallSamples();
				}
				g_rows.push_back(std::move(r));
				{
					auto& row = g_rows.back();
					std::erase_if(g_pendingPassGpu, [&row](const PendingPassGpu& a_p) {
						if (a_p.frame == row.engine.id)
							ApplyPassGpu(row, a_p.bucket, a_p.ms);
						return a_p.frame <= row.engine.id;
					});
					if (row.passLogPending) {
						++g_passLogs;
						g_passLogQueue.push_back(g_rows.size() - 1);
					}
					DrainPassLogs(false);
					NotePrepSpike(row, g_rows.size() - 1);  // (46p)
				}
				if ((++g_reflexPoll & 15) == 0)
					PollReflex();
				if (now >= g_recordEnd)
					StopRecording();
			}
		}

		// (46p) This frame's GPU segment stamps are closed; earlier frames' are read back onto their rows.
		if (g_recording) {
			CloseSegFrame();
			CollectSegFrames();
		}

		g_frame = {};
		g_lastPresentEnd = now;
		g_active = WantedThisFrame() && g_frequency != 0;
		// (46o) M2: the stall sampler runs only while recording with its switch on.
		EngineTiming::SetStallSampler(g_recording && globals::features::performanceOverlay.loaded && globals::features::performanceOverlay.settings.StallSampler);
		if (!g_recording)
			g_vramWarn.store(0, std::memory_order_relaxed);
		if (!g_active)
			g_lastPresentEnd = 0;
	}

	void StartRecording(float a_seconds)
	{
		if (g_recording || g_frequency == 0)
			return;
		g_rows.clear();
		g_worldSpikeLogs = 0;
		// (46p) GPU segment stamps, VRAM window, RT prep spikes start over.
		ResetSegSlots();
		g_vramWinCount = g_vramWinHead = 0;
		g_vramJumpCooldown = 0;
		g_vramJumpLogs = 0;
		g_vramPrevValid = false;
		g_prepCount = g_prepHead = 0;
		g_prepLogs = 0;
		g_prepLast = -2.0f;
		g_passLogs = 0;  // (46n)
		g_passLogQueue.clear();
		g_pendingPassGpu.clear();
		g_passSinceLoad = -1;
		g_loadingWasOpen = false;
		// (44f) "First seen" geometry counts from the start of every recording.
		g_seenGeometry.clear();
		g_rows.reserve(static_cast<size_t>(std::clamp(a_seconds, 1.0f, 600.0f) * 250.0f));
		g_reflex.clear();
		g_recordStart = Now();
		g_recordEnd = g_recordStart + static_cast<int64_t>(static_cast<double>(std::clamp(a_seconds, 1.0f, 600.0f)) * static_cast<double>(g_frequency));
		g_recording = true;
		{
			const auto location = CurrentLocation();
			g_headerLocation = std::format("world {}, cell {}, {}, position {} {} {}, weather {}, game hour {:.2f}", location.world.empty() ? "-" : location.world,
				location.cell.empty() ? "-" : location.cell, location.interior ? "interior" : "exterior", location.x, location.y, location.z,
				location.weather.empty() ? "-" : location.weather, location.hour);
			g_headerTag = LocationTag(location);
			g_headerRayTracing = std::format("{}; build {}", globals::features::rayTracing.DescribeForProfile(), BuildLabel::kBuild);
			g_headerLatency = globals::features::upscaling.DescribeLatencyForTrace();
			g_headerRendering = DescribeRendering();        // (46e)
			g_headerFG = Streamline::DescribeFGForTrace();  // (46w)
			{
				// (46o) M6 session uptime, M3 video memory at the start, M1 calibration, M2 switch.
				const Vram v = QueryVram();
				g_header46o = std::format("session uptime at start {:.1f} min; VRAM {} MB dedicated (D3D12 device on {}), at start: budget {:.0f} MB, usage {:.0f} MB, "
										  "non-local {:.0f} MB; thread CPU time {:.0f} cycles/ms, one read {:.0f} ns; stall sampler {}",
					SessionMinutes(), g_adapterTotalMb, g_adapter12Same ? "the same adapter" : "another adapter", v.budget, v.usage, v.nonLocal,
					EngineTiming::CyclesPerMs(), EngineTiming::CycleReadCostNs(),
					globals::features::performanceOverlay.settings.StallSampler ? "on" : "off");
				g_vram = v;
				g_vramFrame = 1;
				// (46p) Alive per owner at the start (bytes tagged since the hooks went in).
				const VramSnap snap = TakeVramSnap();
				std::string owners;
				for (int i = 0; i < kVramOwners; ++i)
					owners += std::format("{}{} {:.0f}", owners.empty() ? "" : ", ", VramOwnerName(static_cast<VramOwner>(i)),
						(static_cast<double>(snap.created[i]) - static_cast<double>(snap.freed[i])) / (1024.0 * 1024.0));
				g_header46o += std::format("; (46p) alive by owner at start (MB, tagged since start-up): {}", owners);
				// (46p) The main render target's format (bUse64bitsHDRRenderTarget: RGBA16F = 8 bytes a pixel) and ours per feature.
				D3D11_TEXTURE2D_DESC mainDesc{};
				if (auto* renderer = globals::game::renderer) {
					auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
					if (main.texture)
						main.texture->GetDesc(&mainDesc);
				}
				g_header46o += std::format("; kMAIN {}x{} format {} ({}); our video memory per feature at start (MB, tagged D3D11 + RT D3D12): {}",
					mainDesc.Width, mainDesc.Height, static_cast<int>(mainDesc.Format), magic_enum::enum_name(mainDesc.Format), DescribeOurVramByFeature());
			}
		}
		logger::info("[FrameTrace] Recording {:.0f} s", a_seconds);
	}

	void StopRecording()
	{
		if (!g_recording)
			return;
		g_recording = false;
		DrainPassLogs(true);  // (46n) before the rows go to the writer thread
		g_pendingPassGpu.clear();
		PollReflex();
		std::thread(WriteCsv, std::move(g_rows), std::move(g_reflex), g_headerLocation, g_headerRayTracing, g_headerTag, g_headerLatency, g_headerRendering, g_header46o, g_headerFG)
			.detach();
		g_rows = {};
		g_reflex = {};
	}

	float RecordingSecondsLeft()
	{
		if (!g_recording)
			return 0.0f;
		return static_cast<float>(std::max<double>(0.0, Ms(g_recordEnd - Now()) / 1000.0));
	}

	std::string LastFile()
	{
		std::scoped_lock lk(g_fileMutex);
		return g_lastFile;
	}

	Smoothed GetSmoothed() { return g_smoothed; }

	LatencyEstimate EstimatePcLatency(float a_realFrameMs, float a_inputMs, float a_renderMs, float a_fgMult, float a_refreshHz)
	{
		LatencyEstimate e;
		if (a_renderMs <= 0.0f || a_realFrameMs <= 0.0f)
			return e;
		e.valid = true;
		// Input reaching the PC to the game reading it: the measured sample when Reflex reports one, else
		// the average wait for the next read (input arrives at a random moment of a real frame).
		e.inputMeasured = a_inputMs >= 0.0f;
		e.inputMs = e.inputMeasured ? a_inputMs : 0.5f * a_realFrameMs;
		e.renderMs = a_renderMs;
		// Frame generation interpolates: the generated frames between the previous real frame and this one are
		// shown first, spread over one real frame, so this one waits (N - 1) / N of it (its own GPU work not included).
		e.fgMult = std::max(1.0f, a_fgMult);
		e.fgHoldMs = e.fgMult > 1.01f ? a_realFrameMs * (e.fgMult - 1.0f) / e.fgMult : 0.0f;
		// VRR: a frame flips as soon as it is presented while the shown rate stays below the refresh rate; at
		// the refresh rate (V-Sync at the top of the range) it waits half a refresh on average.
		e.refreshHz = std::max(0.0f, a_refreshHz);
		if (e.refreshHz > 0.0f) {
			const float refreshMs = 1000.0f / e.refreshHz;
			const float shownMs = a_realFrameMs / e.fgMult;
			e.queueMs = shownMs <= refreshMs * 1.03f ? 0.5f * refreshMs : 0.0f;
		}
		e.totalMs = e.inputMs + e.renderMs + e.fgHoldMs + e.queueMs;
		return e;
	}

	float CurrentFgMultiplier()
	{
		auto& up = globals::features::upscaling;
		if (!up.loaded || !up.IsFrameGenerationActive())
			return 1.0f;
		float m = up.GetFrameGenerationPresentMultiplier();  // DLSS-G's measured cadence, 0 = none
		if (m <= 1.01f)
			m = static_cast<float>(up.GetFrameGenerationAppliedMultiplier());
		return std::max(1.0f, m);
	}

	// ---- (46o) sub-timers, out-of-VRAM warnings ----

	void AddSplit(Split a_part, int64_t a_ticks)
	{
		if (!g_active)
			return;
		float& v = g_frame.split[static_cast<int>(a_part)];
		v = std::max(v, 0.0f) + static_cast<float>(Ms(a_ticks));
	}

	Lap::Lap()
	{
		if (g_active)
			t = Now();
	}

	void Lap::Mark(Split a_part)
	{
		if (!t)
			return;
		const int64_t n = Now();
		AddSplit(a_part, n - t);
		t = n;
	}

	void Lap::Skip()
	{
		if (t)
			t = Now();
	}

	void OnOutOfVram()
	{
		if (g_recording)
			g_vramWarn.fetch_add(1, std::memory_order_relaxed);
	}

	// ---- (46p) video memory by owner ----

	const char* VramOwnerName(VramOwner a_owner)
	{
		static constexpr const char* kNames[] = { "rt_blas", "rt_tlas", "rt_scratch", "rt_skin", "rt_upload", "rt_readback", "rt_other", "d3d11_ours",
			"d3d11_shared", "engine", "other_modules", "staging" };
		static_assert(std::size(kNames) == static_cast<size_t>(VramOwner::Count));
		const int i = static_cast<int>(a_owner);
		return i >= 0 && i < kVramOwners ? kNames[i] : "?";
	}

	void TagVram(ID3D11DeviceChild* a_object, VramOwner a_owner, uint64_t a_bytes) { TagObject(a_object, a_owner, a_bytes); }
	void TagVram(ID3D12Object* a_object, VramOwner a_owner, uint64_t a_bytes)
	{
		// (46p) Every D3D12 resource of ours is ray tracing's (system-memory heaps not billed to it).
		static FeatureVram* const rt = FeatureEntry("RayTracing (D3D12)");
		const bool sysmem = a_owner == VramOwner::RtUpload || a_owner == VramOwner::RtReadback;
		TagObject(a_object, a_owner, a_bytes, sysmem ? nullptr : rt);
	}

	void SetVramScope(std::string_view a_name)
	{
		if (a_name.empty()) {
			t_vramScope = {};
			return;
		}
		std::scoped_lock lock(g_featureMutex);
		auto it = g_scopeNames.find(a_name);
		if (it == g_scopeNames.end())
			it = g_scopeNames.emplace(a_name).first;
		t_vramScope = *it;
	}

	void PushVramScope(std::string_view a_key)
	{
		if (t_scopeDepth < kScopeStack)
			t_scopeStack[t_scopeDepth] = a_key;
		++t_scopeDepth;
	}

	void PopVramScope()
	{
		if (t_scopeDepth > 0)
			--t_scopeDepth;
	}

	void OnResourceNamed(ID3D11DeviceChild* a_resource, std::string_view a_name)
	{
		// (46q) A resource of ours billed to no feature, named "Feature::thing" / "Feature thing": billed to that feature.
		if (!a_resource || a_name.empty())
			return;
		IUnknown* tagUnknown = nullptr;
		UINT size = sizeof(tagUnknown);
		if (FAILED(a_resource->GetPrivateData(kVramTagGuid, &size, &tagUnknown)) || !tagUnknown)
			return;
		auto* tag = static_cast<VramTag*>(tagUnknown);  // only VramTag is ever stored under kVramTagGuid
		const FeatureVram* current = tag->Feature();
		if (!current || current->name != kVramNoScope) {  // null = not ours (staging / engine): never re-billed
			tagUnknown->Release();
			return;
		}
		size_t end = a_name.find("::");
		if (end == std::string_view::npos)
			end = a_name.find(' ');
		const std::string_view feature = end == std::string_view::npos ? a_name : a_name.substr(0, end);
		if (!feature.empty() && feature.size() < 48)
			tag->Rebill(FeatureEntry(feature));
		tagUnknown->Release();
	}

	std::string DescribeOurVramByFeature()
	{
		std::vector<std::pair<int64_t, std::string>> rows;
		{
			std::scoped_lock lock(g_featureMutex);
			for (const auto& entry : g_featureVram) {
				const int64_t live = entry.live.load(std::memory_order_relaxed);
				if (live >= (512ll << 10))
					rows.emplace_back(live, entry.name);
			}
		}
		std::sort(rows.begin(), rows.end(), [](const auto& a_l, const auto& a_r) { return a_l.first > a_r.first; });
		std::string text;
		for (const auto& [live, name] : rows)
			text += std::format("{}{} {:.0f}", text.empty() ? "" : ", ", name, static_cast<double>(live) / (1024.0 * 1024.0));
		return text.empty() ? std::string("nothing tagged") : text;
	}

	uint64_t OurD3D11LiveBytes()
	{
		uint64_t live = 0;
		for (const auto owner : { VramOwner::D3D11Ours, VramOwner::D3D11Shared }) {
			const int i = static_cast<int>(owner);
			const uint64_t created = g_vramNew[i].load(std::memory_order_relaxed), freed = g_vramFreed[i].load(std::memory_order_relaxed);
			live += created > freed ? created - freed : 0;
		}
		return live;
	}

	void OnD3D11Created(ID3D11DeviceChild* a_resource, uint64_t a_bytes, bool a_shared, bool a_staging, bool a_target, const void* a_caller)
	{
		const VramOwner owner = a_staging ? VramOwner::D3D11Staging : OwnerOfCaller(a_caller, a_shared);
		// (46p) Ours: billed to the feature setting up (scope), else the pass open on the main thread, else "other".
		FeatureVram* feature = nullptr;
		if (owner == VramOwner::D3D11Ours || owner == VramOwner::D3D11Shared) {
			// (46q) Setup scope, else the innermost pass scope open on this thread (always tracked), else the timed pass.
			std::string_view name = t_vramScope;
			if (name.empty() && t_scopeDepth > 0)
				name = FeatureNameOfKey(t_scopeStack[std::min(t_scopeDepth, kScopeStack) - 1]);
			if (name.empty())
				name = FeatureNameOfKey(EngineTiming::CurrentPassName());
			feature = FeatureEntry(name.empty() ? kVramNoScope : name);
		}
		TagObject(a_resource, owner, a_bytes, feature);
		// (46p) Keep Our GPU Memory Resident (always, 47m): our render targets / UAVs (histories, interop, work textures) are evicted last.
		if (a_target && a_resource && (owner == VramOwner::D3D11Ours || owner == VramOwner::D3D11Shared)) {
			winrt::com_ptr<IDXGIResource> dxgi;
			if (SUCCEEDED(a_resource->QueryInterface(IID_PPV_ARGS(dxgi.put()))))
				dxgi->SetEvictionPriority(DXGI_RESOURCE_PRIORITY_HIGH);
		}
	}

	// ---- (46p) coarse GPU time per engine segment ----

	void OnEngineSegment(int a_seg)
	{
		if (!g_recording || g_segBroken)
			return;
		auto* context = globals::d3d::context;
		if (!context)
			return;
		auto& slot = g_segSlots[g_segWrite];
		if (!slot.open) {
			// Opens at the frame's first render segment.
			if (!IsRenderSeg(a_seg))
				return;
			if (Util::GpuPassTimers::GetSingleton()->HasOpenInterval())
				return;  // its disjoint query is open: ours would nest (this frame goes untimed)
			slot.pending = false;  // a sample never read back is dropped
			if (!EnsureSegQueries(slot))
				return;
			context->Begin(slot.disjoint.get());
			context->End(slot.stamps[0].get());
			context->End(slot.disjoint.get());
			slot.seg[0] = static_cast<int8_t>(a_seg);
			slot.count = 1;
			slot.waterCount = 0;  // (47j)
			slot.waterOpen = false;
			slot.frameId = EngineTiming::FrameId();
			slot.open = true;
			return;
		}
		// (47j) The same segment again (or "not a render segment" again): the open interval goes on, no stamp spent.
		const int8_t next = static_cast<int8_t>(IsRenderSeg(a_seg) ? a_seg : -1);
		if (slot.count > 0 && slot.seg[slot.count - 1] == next)
			return;
		if (slot.count >= kSegStamps - 1)
			return;  // full: the last interval runs on to the close (gpu_seg_stamps = kSegStamps)
		context->End(slot.stamps[slot.count].get());
		slot.seg[slot.count] = next;
		++slot.count;
	}

	// (47j) The water draws on the GPU: a stamp where a run of Water-shader draws begins and where it ends (inside the frame's
	// segment stamps: the same disjoint query), summed on read-back (water_draw_gpu_ms; GPU wall time, idle included).
	void OnWaterDrawState(bool a_water)
	{
		if (!g_recording || g_segBroken)
			return;
		auto* context = globals::d3d::context;
		if (!context)
			return;
		auto& slot = g_segSlots[g_segWrite];
		if (!slot.open || slot.waterOpen == a_water)
			return;
		// A begin needs room for its end too.
		const int room = static_cast<int>(slot.water.size()) - slot.waterCount;
		if (a_water ? room < 2 : room < 1)
			return;
		context->End(slot.water[slot.waterCount].get());
		++slot.waterCount;
		slot.waterOpen = a_water;
	}

	nlohmann::json ToJson()
	{
		const auto s = g_smoothed;
		nlohmann::json j;
		j["valid"] = s.valid;
		j["note"] = "Smoothed (0.95 EMA). driver_* = CPU time blocked inside D3D11 calls on the immediate context; own_* = our per-draw hook work without the D3D11 time inside it.";
		j["frame_ms"] = s.frameMs;
		j["present_wait_ms"] = s.presentWaitMs;
		j["reflex_sleep_ms"] = s.reflexSleepMs;
		j["driver_draw_ms"] = s.driverMs[0];
		j["driver_map_ms"] = s.driverMs[1];
		j["driver_upload_ms"] = s.driverMs[2];
		j["driver_dispatch_ms"] = s.driverMs[3];
		j["driver_flush_ms"] = s.driverMs[4];
		j["driver_total_ms"] = s.driverTotalMs;
		j["own_draw_hook_ms"] = s.ownDrawHookMs;
		j["own_setup_geometry_ms"] = s.ownSetupGeometryMs;
		j["perm_cb_upload_ms"] = s.permutationUploadMs;
		j["draws"] = s.draws;
		j["lighting_draws"] = s.lightingDraws;
		j["own_per_draw_us"] = s.ownPerDrawUs;
		if (s.engineValid) {
			nlohmann::json e;
			for (int i = 0; i < static_cast<int>(EngineTiming::Seg::Count); ++i)
				e[EngineTiming::SegName(static_cast<EngineTiming::Seg>(i))] = { { "ms", s.engineMs[i] }, { "peak_ms", s.enginePeakMs[i] } };
			e["input_ms"] = s.engineInputMs;
			e["last_slow_call"] = std::format("{}@{:X}", s.engineTopCallId, s.engineTopCallOffset);
			e["last_slow_call_ms"] = s.engineTopCallMs;
			j["engine_split"] = std::move(e);
		}
		j["engine_dt_ms"] = s.engineDtMs;
		j["device_create_ms"] = s.createMs;
		j["reflex_latency_ms"] = s.reflexLatencyMs;
		j["reflex_markers_at_render_start"] = globals::features::upscaling.WantsReflexMarkersAtRenderStart();
		j["reflex_latency_fix"] = true;  // (45o) hard-wired on
		j["pacer_enabled"] = globals::features::upscaling.IsLatencyPacerEnabled();
		j["pacer_in_flight_now"] = LatencyPacer::Last().inFlight;
		j["pacer_gpu_period_ms"] = LatencyPacer::Last().gpuPeriodMs;
		return j;
	}
}
