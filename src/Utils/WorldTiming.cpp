#include "Utils/WorldTiming.h"

#include <array>
#include <cstring>
#include <format>
#include <intrin.h>
#include <vector>

#include "Utils/EngineTiming.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuPhaseTimeline.h"

namespace Util::WorldTiming
{
	bool g_on = false;

	namespace
	{
		constexpr int kKinds = static_cast<int>(Kind::Count);

		// ---- call sites probed (SkyrimSE.exe 1.5.97, read off the binary with dumpbin for 46l) ----
		// Every direct (E8) call of four world-render functions past their first 0x20 bytes (a detour
		// on the function entry may have moved those): offset in the function, Address Library ID called.
		struct Fn
		{
			uint32_t id;
			const char* name;
		};
		constexpr Fn kFns[] = {
			{ 100424, "RW" },  // Main::RenderWorld
			{ 99938, "FA" },   // BSShaderAccumulator::FinishAccumulating_Standard (opaque, decals, blended, ...)
			{ 100421, "RD" },  // Main::RenderDepth (depth prepass)
			{ 100411, "FP" },  // Main::RenderFirstPersonView
		};
		constexpr int kFnCount = static_cast<int>(std::size(kFns));
		struct Site
		{
			uint8_t fn;
			uint16_t offset;
			uint32_t calleeId;
		};
		constexpr Site kSites[] = {
			// Main::RenderWorld (0x1412E3E70)
			{ 0, 0x055, 25801 }, { 0, 0x0F2, 99790 }, { 0, 0x111, 75646 }, { 0, 0x12C, 75646 }, { 0, 0x147, 75646 }, { 0, 0x164, 75646 },
			{ 0, 0x188, 75646 }, { 0, 0x1A3, 75646 }, { 0, 0x1C5, 100520 }, { 0, 0x1F0, 100537 }, { 0, 0x20A, 75646 }, { 0, 0x226, 100498 },
			{ 0, 0x24C, 100500 }, { 0, 0x259, 100496 }, { 0, 0x28D, 100520 }, { 0, 0x2B4, 99791 }, { 0, 0x2CA, 99690 }, { 0, 0x2E0, 99952 },
			{ 0, 0x2EA, 99964 }, { 0, 0x308, 100269 }, { 0, 0x312, 100211 }, { 0, 0x3D2, 100213 }, { 0, 0x3E4, 99789 }, { 0, 0x476, 100437 },
			{ 0, 0x49D, 25799 },
			// FinishAccumulating_Standard (0x1412CACD0); +0x8E (opaque RenderBatches) and +0x319 (blended decals) carry Deferred's hooks
			{ 1, 0x070, 100407 }, { 1, 0x08E, 99963 }, { 1, 0x0AE, 100840 }, { 1, 0x0CE, 99963 }, { 1, 0x0EA, 99963 }, { 1, 0x10A, 100840 },
			{ 1, 0x12A, 99963 }, { 1, 0x147, 100840 }, { 1, 0x167, 99963 }, { 1, 0x1C6, 100840 }, { 1, 0x1E2, 99963 }, { 1, 0x1EC, 100408 },
			{ 1, 0x247, 99963 }, { 1, 0x284, 100840 }, { 1, 0x2A4, 99963 }, { 1, 0x2CB, 100407 }, { 1, 0x2F2, 99941 }, { 1, 0x319, 99942 },
			{ 1, 0x377, 100861 }, { 1, 0x387, 75650 }, { 1, 0x3C5, 100861 }, { 1, 0x3DA, 75466 }, { 1, 0x3F1, 75463 }, { 1, 0x3F6, 100460 },
			{ 1, 0x410, 75646 }, { 1, 0x42E, 75646 }, { 1, 0x44D, 75646 }, { 1, 0x46B, 75646 }, { 1, 0x48A, 75646 }, { 1, 0x4A6, 99963 },
			{ 1, 0x4AE, 75464 }, { 1, 0x4E8, 75647 }, { 1, 0x507, 75646 }, { 1, 0x526, 75646 }, { 1, 0x544, 75646 }, { 1, 0x563, 75646 },
			{ 1, 0x56D, 75462 }, { 1, 0x572, 100428 }, { 1, 0x58D, 75646 }, { 1, 0x599, 75650 }, { 1, 0x5B3, 75647 }, { 1, 0x5F8, 100408 },
			// Main::RenderDepth (0x1412E3520); +0x0E is __chkstk
			{ 2, 0x056, 75650 }, { 2, 0x06F, 75647 }, { 2, 0x08E, 75646 }, { 2, 0x0A9, 75646 }, { 2, 0x0C4, 75646 }, { 2, 0x0DE, 75648 },
			{ 2, 0x15F, 99790 }, { 2, 0x184, 99952 }, { 2, 0x18E, 99964 }, { 2, 0x1AD, 100407 }, { 2, 0x1D8, 99925 }, { 2, 0x25D, 99790 },
			{ 2, 0x270, 100408 }, { 2, 0x2F5, 75703 }, { 2, 0x31B, 75702 }, { 2, 0x34C, 74802 }, { 2, 0x357, 100211 }, { 2, 0x3FA, 100406 },
			{ 2, 0x409, 100213 }, { 2, 0x40E, 100405 }, { 2, 0x427, 99789 }, { 2, 0x46A, 75702 }, { 2, 0x4ED, 74803 }, { 2, 0x4F9, 75650 },
			// Main::RenderFirstPersonView (0x1412E21A0)
			{ 3, 0x064, 75703 }, { 3, 0x08A, 75702 }, { 3, 0x0C7, 75646 }, { 3, 0x0E2, 75647 }, { 3, 0x103, 99789 }, { 3, 0x13E, 75702 },
			{ 3, 0x160, 99954 }, { 3, 0x183, 75646 }, { 3, 0x1A1, 75646 }, { 3, 0x1DD, 75646 }, { 3, 0x22E, 100211 }, { 3, 0x2D7, 100213 },
			{ 3, 0x2E8, 99789 }, { 3, 0x335, 16099 }, { 3, 0x364, 75646 }, { 3, 0x38A, 25799 }, { 3, 0x405, 99025 },
		};
		constexpr int kSiteCount = static_cast<int>(std::size(kSites));

		// Places: 0 = unknown, 1-3 = the scope entries (depth prepass, world, first person), then the
		// probed call sites, then the pass groups as they are first seen.
		constexpr int kPlaceUnknown = 0;
		constexpr int kPlaceDepth = 1;
		constexpr int kPlaceWorld = 2;
		constexpr int kPlaceFirstPerson = 3;
		constexpr int kFirstSitePlace = 4;
		constexpr int kMaxPlaces = 320;
		std::vector<std::string> g_placeNames;
		std::array<uint64_t, kMaxPlaces> g_placeTicks{};

		struct GroupKey
		{
			uint32_t first, last;
			int group;
			char scope;
			int place;
		};
		std::vector<GroupKey> g_groupPlaces;
		char g_scopeLetter = 'W';

		const char* g_status = "not installed";
		bool g_inner = false;
		bool g_outer = false;
		DWORD g_mainThread = 0;

		// Exclusive state.
		int g_kind = static_cast<int>(Kind::Engine);
		int g_place = kPlaceUnknown;
		uint64_t g_last = 0;
		std::array<uint64_t, kKinds> g_kindTicks{};
		constexpr int kMaxDepth = 32;
		std::array<int, kMaxDepth> g_stack{};
		int g_depth = 0;
		int g_overflow = 0;

		// "Of which" and counters.
		uint64_t g_lookupTicks = 0, g_missTicks = 0, g_skinTicks = 0, g_shelterTicks = 0, g_waterTicks = 0;
		uint32_t g_lookups = 0, g_misses = 0, g_skinActors = 0, g_shelterPicks = 0, g_waterSearches = 0;
		double g_driverMs = 0.0, g_ownDrawMs = 0.0, g_createMs = 0.0;
		uint32_t g_creates = 0, g_textureCreates = 0, g_viewCreates = 0;
		uint32_t g_passes = 0, g_techniques = 0, g_materials = 0, g_setups = 0, g_groups = 0, g_stamps = 0;
		bool g_worldThisFrame = false;

		// TSC -> ms, calibrated against QPC over the whole session.
		uint64_t g_tsc0 = 0;
		int64_t g_qpc0 = 0;
		double g_qpcPerSecond = 0.0;

		inline void Bill(uint64_t a_now)
		{
			if (g_last) {
				const uint64_t d = a_now - g_last;
				g_kindTicks[g_kind] += d;
				g_placeTicks[g_place] += d;
			}
			g_last = a_now;
			++g_stamps;
		}

		inline void SetPlace(int a_place)
		{
			if (g_on)
				Bill(Stamp());
			g_place = a_place;
		}

		int GroupPlace(uint32_t a_first, uint32_t a_last, int a_group)
		{
			for (const auto& g : g_groupPlaces)
				if (g.first == a_first && g.last == a_last && g.group == a_group && g.scope == g_scopeLetter)
					return g.place;
			if (static_cast<int>(g_placeNames.size()) >= kMaxPlaces)
				return kPlaceUnknown;
			const int place = static_cast<int>(g_placeNames.size());
			g_placeNames.push_back(std::format("{}grp{}:{:X}-{:X}", g_scopeLetter, a_group, a_first, a_last));
			g_groupPlaces.push_back({ a_first, a_last, a_group, g_scopeLetter, place });
			return place;
		}

		/// Called by every world probe stub (any thread).
		void OnProbe(int a_index)
		{
			if (!g_on || !OnMainThread() || a_index < 0 || a_index >= kSiteCount)
				return;
			SetPlace(kFirstSitePlace + a_index);
		}

		// ---- hooks ----

		/// Innermost: the engine's own SetupGeometry (installed before every other hook on the slot).
		template <int Tag>
		struct SetupInner
		{
			static void thunk(void* a_shader, void* a_pass, uint32_t a_flags)
			{
				Scope scope(Kind::SetupEngine);
				func(a_shader, a_pass, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		/// Outermost: SetupGeometry with every hook of ours (installed after them).
		template <int Tag>
		struct SetupOuter
		{
			static void thunk(void* a_shader, void* a_pass, uint32_t a_flags)
			{
				Scope scope(Kind::SetupOurs);
				func(a_shader, a_pass, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		template <int Tag>
		struct RestoreOuter
		{
			static void thunk(void* a_shader, void* a_pass, uint32_t a_flags)
			{
				Scope scope(Kind::Restore);
				func(a_shader, a_pass, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct MaterialInner
		{
			static void thunk(void* a_shader, const void* a_material)
			{
				Scope scope(Kind::MaterialEngine);
				func(a_shader, a_material);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		struct MaterialOuter
		{
			static void thunk(void* a_shader, const void* a_material)
			{
				Scope scope(Kind::MaterialOurs);
				func(a_shader, a_material);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		/// BSBatchRenderer::RenderPassImmediately (100854): one render pass, outermost detour.
		struct RenderPass
		{
			static void thunk(void* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_flags)
			{
				Scope scope(Kind::Pass);
				func(a_pass, a_technique, a_alphaTest, a_flags);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
		/// BSShaderAccumulator::RenderBatches (99963): one pass group; names the place.
		struct RenderGroup
		{
			static void thunk(void* a_accumulator, uint32_t a_first, uint32_t a_last, uint32_t a_flags, int a_group)
			{
				if (!On()) {
					func(a_accumulator, a_first, a_last, a_flags, a_group);
					return;
				}
				const int previous = g_place;
				++g_groups;
				SetPlace(GroupPlace(a_first, a_last, a_group));
				func(a_accumulator, a_first, a_last, a_flags, a_group);
				SetPlace(previous);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <class Hook>
		void WriteSlot(REL::VariantID a_vtable, std::size_t a_slot)
		{
			REL::Relocation<std::uintptr_t> vtbl{ a_vtable };
			Hook::func = vtbl.write_vfunc(a_slot, Hook::thunk);
		}

		/// Whole-function detour on an SE address (chains with the detours already there).
		template <class Hook>
		void DetourAddress(uintptr_t a_address)
		{
			Hook::func = a_address;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<PVOID*>(&Hook::func), reinterpret_cast<PVOID>(Hook::thunk));
			DetourTransactionCommit();
		}

		void InstallProbes()
		{
			const auto blocker = EngineTiming::ProbeBlocker();
			if (blocker) {
				logger::info("[WorldTiming] world-render probes SKIPPED: {}", blocker);
				return;
			}
			// Per function: every site must still be a direct call; a function failing this gets no probe.
			std::array<bool, kFnCount> fnOk{};
			std::array<int, kFnCount> redirected{};
			std::array<uintptr_t, kSiteCount> targets{};
			for (int f = 0; f < kFnCount; ++f)
				fnOk[f] = true;
			for (int i = 0; i < kSiteCount; ++i) {
				const auto& s = kSites[i];
				const uintptr_t base = REL::ID(kFns[s.fn].id).address();
				const auto* p = reinterpret_cast<const uint8_t*>(base + s.offset);
				if (p[0] != 0xE8) {
					if (fnOk[s.fn])
						logger::warn("[WorldTiming] {}+0x{:X} is not a call (0x{:02X}); no probes in {}", kFns[s.fn].name, s.offset, p[0], kFns[s.fn].name);
					fnOk[s.fn] = false;
					continue;
				}
				int32_t rel = 0;
				std::memcpy(&rel, p + 1, sizeof(rel));
				targets[i] = base + s.offset + 5 + static_cast<intptr_t>(rel);
				if (targets[i] != REL::ID(s.calleeId).address())
					++redirected[s.fn];
			}
			int count = 0;
			for (int i = 0; i < kSiteCount; ++i)
				count += fnOk[kSites[i].fn] ? 1 : 0;
			if (!count)
				return;

			const size_t codeSize = static_cast<size_t>(count) * 256;
			void* code = VirtualAlloc(nullptr, codeSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
			if (!code) {
				logger::warn("[WorldTiming] probes FAILED: VirtualAlloc error {}", GetLastError());
				return;
			}
			auto* gen = new Xbyak::CodeGenerator(codeSize, code);
			std::array<const uint8_t*, kSiteCount> stubs{};
			for (int i = 0; i < kSiteCount; ++i) {
				if (!fnOk[kSites[i].fn])
					continue;
				gen->align(16);
				stubs[i] = gen->getCurr();
				EngineTiming::EmitProbeStub(*gen, i, targets[i], &OnProbe);
			}
			gen->ready();
			DWORD oldProtect = 0;
			if (!VirtualProtect(code, codeSize, PAGE_EXECUTE_READ, &oldProtect)) {
				logger::warn("[WorldTiming] probes FAILED: VirtualProtect error {}", GetLastError());
				return;
			}
			FlushInstructionCache(GetCurrentProcess(), code, codeSize);

			SKSE::AllocTrampoline(static_cast<size_t>(count) * 14);
			auto& trampoline = SKSE::GetTrampoline();
			for (int i = 0; i < kSiteCount; ++i) {
				if (!stubs[i])
					continue;
				const uintptr_t site = REL::ID(kFns[kSites[i].fn].id).address() + kSites[i].offset;
				trampoline.write_call<5>(site, reinterpret_cast<uintptr_t>(stubs[i]));
			}
			for (int f = 0; f < kFnCount; ++f)
				logger::info("[WorldTiming] {} (ID {}): {} ({} sites already redirected by other hooks, chained)", kFns[f].name, kFns[f].id,
					fnOk[f] ? "probes installed" : "no probes", redirected[f]);
		}
	}

	const char* KindName(Kind a_kind)
	{
		static constexpr const char* kNames[] = { "engine", "pass", "technique", "material_ours", "material_engine", "setup_ours", "setup_engine",
			"restore", "ours_other" };
		static_assert(std::size(kNames) == static_cast<size_t>(Kind::Count));
		const int i = static_cast<int>(a_kind);
		return i >= 0 && i < kKinds ? kNames[i] : "?";
	}

	bool OnMainThread() { return g_mainThread != 0 && GetCurrentThreadId() == g_mainThread; }

	uint64_t Stamp() { return __rdtsc(); }

	void Scope::Begin(Kind a_kind)
	{
		Bill(Stamp());
		if (g_depth >= kMaxDepth) {
			++g_overflow;
		} else {
			g_stack[g_depth++] = g_kind;
			g_kind = static_cast<int>(a_kind);
		}
		pushed = true;
		switch (a_kind) {
		case Kind::Pass:
			++g_passes;
			break;
		case Kind::Technique:
			++g_techniques;
			break;
		case Kind::MaterialEngine:
			++g_materials;
			break;
		case Kind::SetupEngine:
			++g_setups;
			break;
		default:
			break;
		}
	}

	void Scope::End()
	{
		if (g_on)
			Bill(Stamp());
		if (g_overflow > 0) {
			--g_overflow;
			return;
		}
		if (g_depth > 0)
			g_kind = g_stack[--g_depth];
	}

	void AddLookup(uint64_t a_start)
	{
		g_lookupTicks += Stamp() - a_start;
		++g_lookups;
	}
	void AddMiss(uint64_t a_start)
	{
		g_missTicks += Stamp() - a_start;
		++g_misses;
	}
	void AddSkinActor(uint64_t a_start)
	{
		g_skinTicks += Stamp() - a_start;
		++g_skinActors;
	}
	void AddShelterPick(uint64_t a_start)
	{
		g_shelterTicks += Stamp() - a_start;
		++g_shelterPicks;
	}
	void AddWaterSearch(uint64_t a_start)
	{
		g_waterTicks += Stamp() - a_start;
		++g_waterSearches;
	}
	void AddDriverMs(double a_ms) { g_driverMs += a_ms; }
	void AddOwnDrawMs(double a_ms) { g_ownDrawMs += a_ms; }
	void AddCreate(int a_vfuncIndex, double a_ms)
	{
		g_createMs += a_ms;
		++g_creates;
		if (a_vfuncIndex >= 3 && a_vfuncIndex <= 6)  // buffer / texture 1D / 2D / 3D
			++g_textureCreates;
		else if (a_vfuncIndex >= 7 && a_vfuncIndex <= 10)  // SRV / UAV / RTV / DSV
			++g_viewCreates;
	}

	void OnWorldSegment(bool a_in)
	{
		if (a_in) {
			g_on = g_outer && FrameTrace::IsRecording();
			if (g_on) {
				g_last = Stamp();
				++g_stamps;
				g_worldThisFrame = true;
			}
		} else if (g_on) {
			Bill(Stamp());
			g_on = false;
			g_last = 0;
		}
	}

	void OnScopePush(int a_gpuScope)
	{
		switch (static_cast<GpuScope>(a_gpuScope)) {
		case GpuScope::DepthPrepass:
			g_scopeLetter = 'D';
			SetPlace(kPlaceDepth);
			break;
		case GpuScope::World:
			g_scopeLetter = 'W';
			SetPlace(kPlaceWorld);
			break;
		case GpuScope::FirstPerson:
			g_scopeLetter = 'F';
			SetPlace(kPlaceFirstPerson);
			break;
		default:
			break;
		}
	}

	void OnFrameStart()
	{
		g_mainThread = GetCurrentThreadId();
		g_on = false;
		g_last = 0;
		g_depth = 0;
		g_overflow = 0;
		g_kind = static_cast<int>(Kind::Engine);
		g_place = kPlaceUnknown;
	}

	void InstallInner()
	{
		if (const auto blocker = EngineTiming::ProbeBlocker()) {
			g_status = "off (engine timing probes off)";
			logger::info("[WorldTiming] SKIPPED: {}", blocker);
			return;
		}
		// Slot 6 (SetupGeometry) of every shader, slot 4 (SetupMaterial) of Lighting: the engine's own function.
		WriteSlot<SetupInner<0>>(RE::VTABLE_BSLightingShader[0], 6);
		WriteSlot<SetupInner<1>>(RE::VTABLE_BSEffectShader[0], 6);
		WriteSlot<SetupInner<2>>(RE::VTABLE_BSWaterShader[0], 6);
		WriteSlot<SetupInner<3>>(RE::VTABLE_BSUtilityShader[0], 6);
		WriteSlot<SetupInner<4>>(RE::VTABLE_BSParticleShader[0], 6);
		WriteSlot<SetupInner<5>>(RE::VTABLE_BSGrassShader[0], 6);
		WriteSlot<SetupInner<6>>(RE::VTABLE_BSDistantTreeShader[0], 6);
		WriteSlot<SetupInner<7>>(RE::VTABLE_BSBloodSplatterShader[0], 6);
		WriteSlot<SetupInner<8>>(RE::VTABLE_BSSkyShader[0], 6);
		WriteSlot<MaterialInner>(RE::VTABLE_BSLightingShader[0], 4);
		g_inner = true;
		g_status = "inner installed";
		logger::info("[WorldTiming] inner SetupGeometry / SetupMaterial wrappers installed");
	}

	void InstallOuter()
	{
		if (!g_inner) {
			logger::info("[WorldTiming] outer hooks SKIPPED: the inner wrappers are not installed ({})", g_status);
			return;
		}
		{
			LARGE_INTEGER f, q;
			QueryPerformanceFrequency(&f);
			QueryPerformanceCounter(&q);
			g_qpcPerSecond = static_cast<double>(f.QuadPart);
			g_qpc0 = q.QuadPart;
			g_tsc0 = Stamp();
		}
		g_placeNames.reserve(kMaxPlaces);
		g_placeNames.clear();
		g_placeNames.emplace_back("unknown");
		g_placeNames.emplace_back("RD:start");
		g_placeNames.emplace_back("RW:start");
		g_placeNames.emplace_back("FP:start");
		for (const auto& s : kSites)
			g_placeNames.push_back(std::format("{}+{:X}>{}", kFns[s.fn].name, s.offset, s.calleeId));

		// Outermost wrappers: after every hook of ours on these slots.
		WriteSlot<SetupOuter<0>>(RE::VTABLE_BSLightingShader[0], 6);
		WriteSlot<SetupOuter<1>>(RE::VTABLE_BSEffectShader[0], 6);
		WriteSlot<SetupOuter<2>>(RE::VTABLE_BSWaterShader[0], 6);
		WriteSlot<SetupOuter<5>>(RE::VTABLE_BSGrassShader[0], 6);
		WriteSlot<SetupOuter<8>>(RE::VTABLE_BSSkyShader[0], 6);
		WriteSlot<RestoreOuter<0>>(RE::VTABLE_BSLightingShader[0], 7);
		WriteSlot<RestoreOuter<1>>(RE::VTABLE_BSEffectShader[0], 7);
		WriteSlot<RestoreOuter<8>>(RE::VTABLE_BSSkyShader[0], 7);
		WriteSlot<MaterialOuter>(RE::VTABLE_BSLightingShader[0], 4);
		DetourAddress<RenderPass>(REL::ID(100854).address());
		DetourAddress<RenderGroup>(REL::ID(99963).address());
		InstallProbes();
		g_outer = true;
		g_status = "installed";
		// Cost of one switch, for reading the overhead off wt_stamps.
		LARGE_INTEGER a, b;
		QueryPerformanceCounter(&a);
		uint64_t sink = 0;
		for (int i = 0; i < 100000; ++i)
			sink += Stamp();
		QueryPerformanceCounter(&b);
		logger::info("[WorldTiming] installed: {} places; one TSC read {:.1f} ns ({})", g_placeNames.size(),
			static_cast<double>(b.QuadPart - a.QuadPart) * 1e9 / g_qpcPerSecond / 100000.0, sink & 1);
	}

	const char* Status() { return g_status; }

	Frame TakeFrame(bool a_detail)
	{
		Frame f;
		const bool ran = g_worldThisFrame && g_outer;
		double ticksPerMs = 0.0;
		if (ran) {
			LARGE_INTEGER q;
			QueryPerformanceCounter(&q);
			const double seconds = static_cast<double>(q.QuadPart - g_qpc0) / g_qpcPerSecond;
			if (seconds > 0.05)
				ticksPerMs = static_cast<double>(Stamp() - g_tsc0) / (seconds * 1000.0);
		}
		if (ran && ticksPerMs > 0.0) {
			f.valid = true;
			const auto ms = [&](uint64_t a_ticks) { return static_cast<float>(static_cast<double>(a_ticks) / ticksPerMs); };
			double total = 0.0;
			for (int i = 0; i < kKinds; ++i) {
				f.kindMs[i] = ms(g_kindTicks[i]);
				total += f.kindMs[i];
				if (f.kindMs[i] >= 0.05f)
					f.kindsText += std::format("{}:{:.2f};", KindName(static_cast<Kind>(i)), f.kindMs[i]);
			}
			f.totalMs = static_cast<float>(total);
			f.driverMs = static_cast<float>(g_driverMs);
			f.ownDrawMs = static_cast<float>(g_ownDrawMs);
			f.lookupMs = ms(g_lookupTicks);
			f.missMs = ms(g_missTicks);
			f.lookups = g_lookups;
			f.misses = g_misses;
			f.skinMs = ms(g_skinTicks);
			f.shelterMs = ms(g_shelterTicks);
			f.waterMs = ms(g_waterTicks);
			f.skinActors = g_skinActors;
			f.shelterPicks = g_shelterPicks;
			f.waterSearches = g_waterSearches;
			f.createMs = static_cast<float>(g_createMs);
			f.creates = g_creates;
			f.textureCreates = g_textureCreates;
			f.viewCreates = g_viewCreates;
			f.passes = g_passes;
			f.techniques = g_techniques;
			f.materials = g_materials;
			f.setups = g_setups;
			f.groups = g_groups;
			f.stamps = g_stamps;
			// The three costliest places, and (recording detail) every place over 1 ms.
			const int places = static_cast<int>(g_placeNames.size());
			int top[3] = { -1, -1, -1 };
			for (int i = 0; i < places; ++i) {
				const uint64_t t = g_placeTicks[i];
				if (!t)
					continue;
				for (int k = 0; k < 3; ++k) {
					if (top[k] < 0 || t > g_placeTicks[top[k]]) {
						for (int j = 2; j > k; --j)
							top[j] = top[j - 1];
						top[k] = i;
						break;
					}
				}
				if (a_detail) {
					const float v = ms(t);
					if (v > 1.0f)
						f.placesOver1ms += std::format("{}:{:.2f};", g_placeNames[i], v);
				}
			}
			for (int k = 0; k < 3; ++k)
				if (top[k] >= 0)
					f.top3 += std::format("{}:{:.2f};", g_placeNames[top[k]], ms(g_placeTicks[top[k]]));
		}

		g_kindTicks.fill(0);
		g_placeTicks.fill(0);
		g_lookupTicks = g_missTicks = g_skinTicks = g_shelterTicks = g_waterTicks = 0;
		g_lookups = g_misses = g_skinActors = g_shelterPicks = g_waterSearches = 0;
		g_driverMs = g_ownDrawMs = g_createMs = 0.0;
		g_creates = g_textureCreates = g_viewCreates = 0;
		g_passes = g_techniques = g_materials = g_setups = g_groups = g_stamps = 0;
		g_worldThisFrame = false;
		return f;
	}
}
