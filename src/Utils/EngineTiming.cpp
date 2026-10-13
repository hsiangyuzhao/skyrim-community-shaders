#include "Utils/EngineTiming.h"

#include <array>
#include <atomic>
#include <cstring>
#include <format>
#include <memory>

#include "Features/Upscaling.h"
#include "Utils/FrameTrace.h"
#include "Utils/GpuPhaseTimeline.h"

namespace Util::EngineTiming
{
	namespace
	{
		// Every direct (E8) call in Main::Update, SkyrimSE.exe 1.5.97 (ID 35565, 0x1405B2FF0, 0x764
		// bytes): offset of the call instruction, and the Address Library ID of the function it calls.
		// Read off the binary with dumpbin for 44f; the IDs come from the Address Library database.
		struct Site
		{
			uint32_t offset;
			uint32_t calleeId;
		};
		constexpr Site kSites[] = {
			{ 0x01E, 68163 }, { 0x053, 39366 }, { 0x058, 35605 }, { 0x064, 15159 }, { 0x073, 76071 }, { 0x07F, 34862 },
			{ 0x090, 76072 }, { 0x095, 101821 }, { 0x09D, 101831 }, { 0x0A2, 68506 }, { 0x0A9, 38080 }, { 0x0FE, 79937 },
			{ 0x120, 79937 }, { 0x142, 79937 }, { 0x164, 79937 }, { 0x186, 79937 }, { 0x1A4, 79937 }, { 0x1C9, 35208 },
			{ 0x1F6, 79937 }, { 0x1FF, 35208 }, { 0x206, 35207 }, { 0x24D, 35402 }, { 0x25F, 35318 }, { 0x281, 34555 },
			{ 0x286, 34553 }, { 0x28E, 35578 }, { 0x2BB, 26497 }, { 0x2CC, 68167 }, { 0x2D8, 42911 }, { 0x2E4, 42778 },
			{ 0x2F0, 33909 }, { 0x30A, 79937 }, { 0x31A, 35581 }, { 0x344, 18976 }, { 0x366, 35574 }, { 0x37D, 31719 },
			{ 0x38C, 53115 }, { 0x39B, 53116 }, { 0x3A0, 16188 }, { 0x3AC, 56482 }, { 0x3C0, 67315 }, { 0x3C8, 35566 },
			{ 0x3D6, 45570 }, { 0x3DE, 35572 }, { 0x3EF, 16173 }, { 0x403, 40377 }, { 0x418, 35501 }, { 0x41D, 69181 },
			{ 0x43E, 52050 }, { 0x446, 35569 }, { 0x44E, 35568 }, { 0x469, 39637 }, { 0x47B, 13148 }, { 0x485, 68165 },
			{ 0x48F, 68165 }, { 0x499, 68165 }, { 0x4A5, 15033 }, { 0x4AF, 68165 }, { 0x4D0, 100403 }, { 0x4EF, 100409 },
			{ 0x4F9, 34551 }, { 0x509, 68167 }, { 0x53C, 40376 }, { 0x548, 42914 }, { 0x554, 13152 }, { 0x55C, 35571 },
			{ 0x561, 19437 }, { 0x56D, 90944 }, { 0x57B, 51751 }, { 0x580, 101995 }, { 0x592, 51750 }, { 0x599, 51260 },
			{ 0x5B3, 35589 }, { 0x5BA, 34551 }, { 0x5CA, 79945 }, { 0x5D2, 35556 }, { 0x5E3, 35574 }, { 0x5E8, 34554 },
			{ 0x604, 66862 }, { 0x612, 66841 }, { 0x61A, 66889 }, { 0x62B, 79946 }, { 0x630, 52063 }, { 0x638, 52065 },
			{ 0x644, 39572 }, { 0x66D, 87936 }, { 0x672, 34303 }, { 0x677, 32314 }, { 0x681, 35570 }, { 0x690, 53118 },
			{ 0x69A, 38082 }, { 0x6A0, 21434 }, { 0x6AC, 35914 }, { 0x6B8, 35916 }, { 0x6C4, 35912 }, { 0x6CF, 38083 },
			{ 0x6E9, 79937 }, { 0x707, 26407 }, { 0x723, 66862 }, { 0x731, 66841 }, { 0x739, 66890 }, { 0x73E, 21438 },
			{ 0x743, 21400 }, { 0x748, 76765 }, { 0x754, 100571 },
		};
		constexpr int kSiteCount = static_cast<int>(std::size(kSites));
		constexpr uint32_t kMainUpdateId = 35565;
		constexpr uint32_t kRenderOffset = 0x5D2;  // call 35556: the frame render
		constexpr uint32_t kInputOffset = 0x3C0;   // call 67315: BSInputDeviceManager::PollInputDevices
		constexpr int kSegCount = static_cast<int>(Seg::Count);

		int g_renderIndex = -1;
		int g_inputIndex = -1;
		bool g_probes = false;

		DWORD g_mainThread = 0;
		double g_toMs = 0.0;

		// Attribution state: the segment (and game-update call) the main thread is in right now.
		int g_seg = static_cast<int>(Seg::Outer);
		int g_call = -1;
		int64_t g_last = 0;
		std::array<int64_t, kSegCount> g_acc{};
		std::array<int64_t, kSiteCount> g_callAcc{};

		struct StackEntry
		{
			int scope;  ///< GpuScope as int, or -1 for a ScopedSeg
			int seg;    ///< segment to return to
			int call;   ///< game-update call to return to
		};
		constexpr int kMaxDepth = 32;
		std::array<StackEntry, kMaxDepth> g_stack{};
		int g_depth = 0;
		int g_overflow = 0;

		// (44f, A) Reflex markers.
		bool g_frameToken = false;
		bool g_renderMarked = false;

		// Device Create* calls.
		int64_t g_createTicks = 0;
		uint32_t g_creates = 0;
		std::atomic<int64_t> g_createOtherTicks{ 0 };
		std::atomic<uint32_t> g_createsOther{ 0 };

		bool OnMainThread() { return g_mainThread != 0 && GetCurrentThreadId() == g_mainThread; }

		void SwitchTo(int a_seg, int a_call)
		{
			if (FrameTrace::Active()) {
				const int64_t now = FrameTrace::Now();
				if (g_last) {
					const int64_t d = now - g_last;
					g_acc[g_seg] += d;
					if (g_call >= 0)
						g_callAcc[g_call] += d;
				}
				g_last = now;
			} else {
				g_last = 0;
			}
			g_seg = a_seg;
			g_call = a_call;
		}

		void Push(int a_scope, Seg a_seg)
		{
			if (g_depth >= kMaxDepth) {
				++g_overflow;
				return;
			}
			g_stack[g_depth++] = { a_scope, g_seg, g_call };
			SwitchTo(static_cast<int>(a_seg), -1);
		}

		void Pop(int a_scope)
		{
			if (g_overflow > 0) {
				--g_overflow;
				return;
			}
			for (int i = g_depth - 1; i >= 0; --i) {
				if (g_stack[i].scope == a_scope) {
					const StackEntry e = g_stack[i];
					g_depth = i;
					SwitchTo(e.seg, e.call);
					return;
				}
			}
		}

		Seg SegOfScope(GpuScope a_scope)
		{
			switch (a_scope) {
			case GpuScope::ShadowMaps:
			case GpuScope::ShadowSun:
			case GpuScope::ShadowSlice:
			case GpuScope::ShadowLocal:
			case GpuScope::ShadowMask:
				return Seg::Shadows;
			case GpuScope::WaterPrep:
			case GpuScope::Reflections:
				return Seg::Reflections;
			case GpuScope::DepthPrepass:
			case GpuScope::World:
			case GpuScope::Opaque:
			case GpuScope::FirstPerson:
				return Seg::World;
			case GpuScope::Imagespace:
				return Seg::ImageSpace;
			case GpuScope::UI:
				return Seg::UI;
			case GpuScope::CsOverlay:
				return Seg::Overlay;
			default:
				return Seg::OurPasses;
			}
		}

		void RenderStartMarkers()
		{
			if (!g_frameToken)
				return;
			auto& upscaling = globals::features::upscaling;
			if (!upscaling.settings.reflexMarkersAtRenderStart || !upscaling.streamline.pclFunctionsReady)
				return;
			upscaling.streamline.SetPCLMarker(sl::PCLMarker::eSimulationEnd);
			upscaling.streamline.SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);
			g_renderMarked = true;
		}

		/// Called by every probe stub (any thread; only the main thread is counted).
		void OnProbe(int a_index)
		{
			if (!OnMainThread() || a_index < 0 || a_index >= kSiteCount)
				return;
			if (a_index == g_renderIndex) {
				RenderStartMarkers();
				SwitchTo(static_cast<int>(Seg::RenderOther), -1);
				return;
			}
			SwitchTo(static_cast<int>(kSites[a_index].offset < kRenderOffset ? Seg::Logic : Seg::Tail), a_index);
		}

		// Probe stub: notes the call, then jumps to the original target as if called directly.
		// Saves the flags and every volatile register (rax, rcx, rdx, r8-r11, xmm0-xmm5), so the
		// callee sees exactly the registers and the stack the engine set up; the C++ callback
		// preserves the non-volatile ones itself. On entry rsp is 8 mod 16 (the engine's call
		// pushed the return address); 8 pushes + 0x88 keep the callback's call 16-byte aligned.
		void EmitStub(Xbyak::CodeGenerator& a_gen, int a_index, uintptr_t a_target)
		{
			using namespace Xbyak::util;
			Xbyak::Label callback, target;
			a_gen.pushfq();
			a_gen.push(rax);
			a_gen.push(rcx);
			a_gen.push(rdx);
			a_gen.push(r8);
			a_gen.push(r9);
			a_gen.push(r10);
			a_gen.push(r11);
			a_gen.sub(rsp, 0x88);
			a_gen.movdqu(ptr[rsp + 0x20], xmm0);
			a_gen.movdqu(ptr[rsp + 0x30], xmm1);
			a_gen.movdqu(ptr[rsp + 0x40], xmm2);
			a_gen.movdqu(ptr[rsp + 0x50], xmm3);
			a_gen.movdqu(ptr[rsp + 0x60], xmm4);
			a_gen.movdqu(ptr[rsp + 0x70], xmm5);
			a_gen.mov(ecx, a_index);
			a_gen.call(ptr[rip + callback]);
			a_gen.movdqu(xmm0, ptr[rsp + 0x20]);
			a_gen.movdqu(xmm1, ptr[rsp + 0x30]);
			a_gen.movdqu(xmm2, ptr[rsp + 0x40]);
			a_gen.movdqu(xmm3, ptr[rsp + 0x50]);
			a_gen.movdqu(xmm4, ptr[rsp + 0x60]);
			a_gen.movdqu(xmm5, ptr[rsp + 0x70]);
			a_gen.add(rsp, 0x88);
			a_gen.pop(r11);
			a_gen.pop(r10);
			a_gen.pop(r9);
			a_gen.pop(r8);
			a_gen.pop(rdx);
			a_gen.pop(rcx);
			a_gen.pop(rax);
			a_gen.popfq();
			a_gen.jmp(ptr[rip + target]);
			a_gen.L(callback);
			a_gen.dq(reinterpret_cast<uint64_t>(&OnProbe));
			a_gen.L(target);
			a_gen.dq(static_cast<uint64_t>(a_target));
		}

		// ---- device Create* timing ----
		struct CreateTimer
		{
			int64_t start = 0;
			bool main = false;
			CreateTimer()
			{
				if (FrameTrace::Active()) {
					main = OnMainThread();
					start = FrameTrace::Now();
				}
			}
			~CreateTimer()
			{
				if (!start)
					return;
				const int64_t d = FrameTrace::Now() - start;
				if (main) {
					g_createTicks += d;
					++g_creates;
				} else {
					g_createOtherTicks.fetch_add(d, std::memory_order_relaxed);
					g_createsOther.fetch_add(1, std::memory_order_relaxed);
				}
			}
		};

		template <std::size_t Index, class Sig>
		struct DeviceCreate;

		template <std::size_t Index, class... Args>
		struct DeviceCreate<Index, HRESULT(Args...)>
		{
			static HRESULT STDMETHODCALLTYPE thunk(ID3D11Device* This, Args... a_args)
			{
				CreateTimer timer;
				return func(This, a_args...);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		template <std::size_t Index, class Sig>
		void HookCreate(ID3D11Device* a_device)
		{
			stl::detour_vfunc<Index, DeviceCreate<Index, Sig>>(a_device);
		}
	}

	const char* SegName(Seg a_seg)
	{
		static constexpr const char* kNames[] = { "outer", "reflex_sleep", "logic", "render_other", "shadows", "reflections", "world",
			"imagespace", "ui", "cs_passes", "overlay", "present", "tail" };
		static_assert(std::size(kNames) == static_cast<size_t>(Seg::Count));
		const int i = static_cast<int>(a_seg);
		return i >= 0 && i < kSegCount ? kNames[i] : "?";
	}

	bool ProbesInstalled() { return g_probes; }

	void InstallUpdateProbes()
	{
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		g_toMs = f.QuadPart ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;

		if (!REL::Module::IsSE()) {
			logger::info("[EngineTiming] Main::Update probes are for SE 1.5.97 only; engine split off");
			return;
		}
		const uintptr_t base = REL::ID(kMainUpdateId).address();

		// Every site must still be a direct call. Most must still call the function the table
		// says (another plugin may have redirected a few, which chains fine); if many do not,
		// this is not the Main::Update the table was read from and nothing is installed.
		std::array<uintptr_t, kSiteCount> targets{};
		int mismatched = 0;
		for (int i = 0; i < kSiteCount; ++i) {
			const auto* p = reinterpret_cast<const uint8_t*>(base + kSites[i].offset);
			if (p[0] != 0xE8) {
				logger::warn("[EngineTiming] Main::Update+0x{:X} is not a call (0x{:02X}); engine split off", kSites[i].offset, p[0]);
				return;
			}
			int32_t rel = 0;
			std::memcpy(&rel, p + 1, sizeof(rel));
			targets[i] = base + kSites[i].offset + 5 + static_cast<intptr_t>(rel);
			if (targets[i] != REL::ID(kSites[i].calleeId).address())
				++mismatched;
			if (kSites[i].offset == kRenderOffset)
				g_renderIndex = i;
			if (kSites[i].offset == kInputOffset)
				g_inputIndex = i;
		}
		if (mismatched > 8) {
			logger::warn("[EngineTiming] {} of {} Main::Update calls do not match SE 1.5.97; engine split off", mismatched, kSiteCount);
			return;
		}

		// One code buffer for every stub, read/execute once written.
		auto* gen = new Xbyak::CodeGenerator(static_cast<size_t>(kSiteCount) * 256, Xbyak::DontSetProtectRWE);
		std::array<const uint8_t*, kSiteCount> stubs{};
		for (int i = 0; i < kSiteCount; ++i) {
			gen->align(16);
			stubs[i] = gen->getCurr();
			EmitStub(*gen, i, targets[i]);
		}
		gen->ready(Xbyak::CodeGenerator::PROTECT_RE);

		SKSE::AllocTrampoline(static_cast<size_t>(kSiteCount) * 14);
		auto& trampoline = SKSE::GetTrampoline();
		for (int i = 0; i < kSiteCount; ++i) {
			const uintptr_t previous = trampoline.write_call<5>(base + kSites[i].offset, reinterpret_cast<uintptr_t>(stubs[i]));
			if (previous != targets[i])
				logger::warn("[EngineTiming] Main::Update+0x{:X} changed while installing", kSites[i].offset);
		}
		g_probes = true;
		logger::info("[EngineTiming] {} Main::Update probes installed ({} calls already redirected by other plugins)", kSiteCount, mismatched);
	}

	void InstallDeviceHooks(ID3D11Device* a_device)
	{
		if (!a_device)
			return;
		HookCreate<3, HRESULT(const D3D11_BUFFER_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Buffer**)>(a_device);
		HookCreate<4, HRESULT(const D3D11_TEXTURE1D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture1D**)>(a_device);
		HookCreate<5, HRESULT(const D3D11_TEXTURE2D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture2D**)>(a_device);
		HookCreate<6, HRESULT(const D3D11_TEXTURE3D_DESC*, const D3D11_SUBRESOURCE_DATA*, ID3D11Texture3D**)>(a_device);
		HookCreate<7, HRESULT(ID3D11Resource*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, ID3D11ShaderResourceView**)>(a_device);
		HookCreate<8, HRESULT(ID3D11Resource*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, ID3D11UnorderedAccessView**)>(a_device);
		HookCreate<9, HRESULT(ID3D11Resource*, const D3D11_RENDER_TARGET_VIEW_DESC*, ID3D11RenderTargetView**)>(a_device);
		HookCreate<10, HRESULT(ID3D11Resource*, const D3D11_DEPTH_STENCIL_VIEW_DESC*, ID3D11DepthStencilView**)>(a_device);
		HookCreate<11, HRESULT(const D3D11_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, ID3D11InputLayout**)>(a_device);
		// 12 (vertex) and 15 (pixel shaders) are hooked by Hooks.cpp in shader-dump mode only;
		// those two are timed through ShaderCache's own counters instead.
		HookCreate<13, HRESULT(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**)>(a_device);
		HookCreate<18, HRESULT(const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**)>(a_device);
		HookCreate<20, HRESULT(const D3D11_BLEND_DESC*, ID3D11BlendState**)>(a_device);
		HookCreate<21, HRESULT(const D3D11_DEPTH_STENCIL_DESC*, ID3D11DepthStencilState**)>(a_device);
		HookCreate<22, HRESULT(const D3D11_RASTERIZER_DESC*, ID3D11RasterizerState**)>(a_device);
		HookCreate<24, HRESULT(const D3D11_QUERY_DESC*, ID3D11Query**)>(a_device);
		logger::info("[EngineTiming] Device Create* timers installed");
	}

	void OnUpdateBegin(bool a_frameToken)
	{
		g_mainThread = GetCurrentThreadId();
		g_frameToken = a_frameToken;
		g_renderMarked = false;
		g_depth = 0;
		g_overflow = 0;
		SwitchTo(static_cast<int>(Seg::Logic), -1);
	}

	void OnUpdateEnd()
	{
		if (!OnMainThread())
			return;
		g_depth = 0;
		g_overflow = 0;
		SwitchTo(static_cast<int>(Seg::Outer), -1);
	}

	ScopedSeg::ScopedSeg(Seg a_seg)
	{
		if (!OnMainThread())
			return;
		Push(-1, a_seg);
		pushed = true;
	}

	ScopedSeg::~ScopedSeg()
	{
		if (pushed)
			Pop(-1);
	}

	void OnPhasePush(int a_gpuScope)
	{
		if (!g_probes || !OnMainThread())
			return;
		Push(a_gpuScope, SegOfScope(static_cast<GpuScope>(a_gpuScope)));
	}

	void OnPhasePop(int a_gpuScope)
	{
		if (!g_probes || !OnMainThread())
			return;
		Pop(a_gpuScope);
	}

	bool RenderStartMarked() { return g_renderMarked; }

	Frame TakeFrame(bool a_detail)
	{
		Frame f;
		SwitchTo(g_seg, g_call);  // close the open interval at "now"

		if (g_probes) {
			f.valid = true;
			for (int i = 0; i < kSegCount; ++i)
				f.ms[i] = static_cast<float>(static_cast<double>(g_acc[i]) * g_toMs);
			if (g_inputIndex >= 0)
				f.inputMs = static_cast<float>(static_cast<double>(g_callAcc[g_inputIndex]) * g_toMs);
			int top = -1;
			for (int i = 0; i < kSiteCount; ++i) {
				if (i == g_renderIndex)
					continue;
				if (top < 0 || g_callAcc[i] > g_callAcc[top])
					top = i;
				if (a_detail) {
					const double ms = static_cast<double>(g_callAcc[i]) * g_toMs;
					if (ms > 1.0)
						f.callsOver1ms += std::format("{}@{:X}:{:.2f};", kSites[i].calleeId, kSites[i].offset, ms);
				}
			}
			if (top >= 0 && g_callAcc[top] > 0) {
				f.topCallId = kSites[top].calleeId;
				f.topCallOffset = kSites[top].offset;
				f.topCallMs = static_cast<float>(static_cast<double>(g_callAcc[top]) * g_toMs);
			}
		}
		if (auto* timer = RE::BSTimer::GetSingleton()) {
			f.engineDtMs = timer->delta * 1000.0f;
			f.engineRealDtMs = timer->realTimeDelta * 1000.0f;
		}
		f.createMs = static_cast<float>(static_cast<double>(g_createTicks) * g_toMs);
		f.creates = g_creates;
		f.createOtherMs = static_cast<float>(static_cast<double>(g_createOtherTicks.exchange(0, std::memory_order_relaxed)) * g_toMs);
		f.createsOther = g_createsOther.exchange(0, std::memory_order_relaxed);

		g_acc.fill(0);
		g_callAcc.fill(0);
		g_createTicks = 0;
		g_creates = 0;
		return f;
	}
}
