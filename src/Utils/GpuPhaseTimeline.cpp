#include "Utils/GpuPhaseTimeline.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Game.h"
#include "Utils/GpuTimers.h"

namespace Util
{
	namespace
	{
		constexpr size_t kPhaseCount = static_cast<size_t>(GpuPhase::Count);
		static_assert(GpuPhaseTimeline::kShaderTypeCount == static_cast<size_t>(RE::BSShader::Type::Total));

		constexpr GpuPhase InitialPhase(GpuScope a_scope)
		{
			switch (a_scope) {
			case GpuScope::ShadowMaps:
				return GpuPhase::ShadowOther;
			case GpuScope::ShadowSun:
				return GpuPhase::ShadowSunCascade1;
			case GpuScope::ShadowLocal:
				return GpuPhase::ShadowLocalLights;
			case GpuScope::ShadowMask:
				return GpuPhase::ShadowMask;
			case GpuScope::WaterPrep:
				return GpuPhase::WaterPrep;
			case GpuScope::DepthPrepass:
				return GpuPhase::DepthPrepass;
			case GpuScope::World:
				return GpuPhase::WorldOther;
			case GpuScope::Opaque:
				return GpuPhase::OpaqueOther;
			case GpuScope::FirstPerson:
				return GpuPhase::FirstPerson;
			case GpuScope::Reflections:
				return GpuPhase::Reflections;
			case GpuScope::Imagespace:
				return GpuPhase::Imagespace;
			case GpuScope::UI:
				return GpuPhase::UI;
			case GpuScope::CsPasses:
				return GpuPhase::CsPasses;
			case GpuScope::CsOther:
				return GpuPhase::CsOther;
			case GpuScope::CsUpscaling:
				return GpuPhase::CsUpscaling;
			case GpuScope::CsOverlay:
				return GpuPhase::CsOverlay;
			default:
				return GpuPhase::Untracked;
			}
		}

		/// Opaque G-buffer draws, by shader type and, for Lighting, by technique.
		GpuPhase ClassifyOpaque(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
		{
			using Tech = SIE::ShaderCache::LightingShaderTechniques;
			switch (a_shader->shaderType.get()) {
			case RE::BSShader::Type::Lighting:
				{
					switch (static_cast<Tech>(0x3F & (a_pixelDescriptor >> 24))) {
					case Tech::MTLand:
					case Tech::MTLandLODBlend:
						return GpuPhase::OpaqueTerrain;
					case Tech::LODLand:
					case Tech::LODLandNoise:
					case Tech::LODObjects:
					case Tech::LODObjectHD:
						return GpuPhase::OpaqueDistant;
					case Tech::TreeAnim:
						return GpuPhase::OpaqueTrees;
					case Tech::Facegen:
					case Tech::FacegenRGBTint:
					case Tech::Hair:
					case Tech::Eye:
						return GpuPhase::OpaqueCharacters;
					default:
						break;
					}
					if (a_vertexDescriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned))
						return GpuPhase::OpaqueCharacters;
					return GpuPhase::OpaqueObjects;
				}
			case RE::BSShader::Type::Grass:
				return GpuPhase::OpaqueGrass;
			case RE::BSShader::Type::DistantTree:
				return GpuPhase::OpaqueDistant;
			default:
				return GpuPhase::OpaqueOther;
			}
		}

		/// World draws outside the opaque pass. Lighting / Effect / Particle draws here are the
		/// alpha-blended pass; they are deliberately not split further, because they are sorted
		/// back to front and would switch type on almost every draw.
		GpuPhase ClassifyWorld(RE::BSShader* a_shader)
		{
			switch (a_shader->shaderType.get()) {
			case RE::BSShader::Type::Sky:
				return GpuPhase::Sky;
			case RE::BSShader::Type::Water:
				return GpuPhase::Water;
			case RE::BSShader::Type::ImageSpace:
				return GpuPhase::Imagespace;
			default:
				return GpuPhase::Transparent;
			}
		}

		bool TableIsOnScreen()
		{
			auto& overlay = globals::features::performanceOverlay;
			return globals::state && globals::menu && globals::menu->overlayVisible &&
			       overlay.loaded && overlay.IsOverlayVisible() &&
			       overlay.settings.ShowDrawCalls;
		}

		// ---------------------------------------------------------------------------------
		// Engine hooks owned by the timeline. Each one only brackets the original call with
		// a scope. The signatures were checked against SkyrimSE.exe 1.5.97 (and match the
		// ones FrameAnnotations uses for the same functions); the four integer registers are
		// forwarded untouched so a runtime whose variant takes more register arguments
		// still receives exactly what its caller passed.
		// ---------------------------------------------------------------------------------

		// One template instance per hooked function (the scope is the key), so each keeps
		// its own trampoline in `func`.
		template <GpuScope Scope>
		struct ScopedCall
		{
			static void thunk(uintptr_t a1, uintptr_t a2, uintptr_t a3, uintptr_t a4)
			{
				GpuPhaseScope scope(Scope);
				func(a1, a2, a3, a4);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		using Main_RenderWaterEffects = ScopedCall<GpuScope::WaterPrep>;  // void()
		using Main_RenderDepth = ScopedCall<GpuScope::DepthPrepass>;      // void(bool, bool)
		using Main_RenderShadowmasks = ScopedCall<GpuScope::ShadowMask>;  // void(bool)
		using Main_RenderImagespace = ScopedCall<GpuScope::Imagespace>;   // void(uint32_t)
		using MenuManager_DrawInterface = ScopedCall<GpuScope::UI>;       // void(MenuManager*)

		struct BSShadowDirectionalLight_RenderShadowmaps
		{
			static void thunk(RE::BSShadowLight* a_light, void* a2)
			{
				GpuPhaseScope scope(GpuScope::ShadowSun);
				func(a_light, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowFrustumLight_RenderShadowmaps
		{
			static void thunk(RE::BSShadowLight* a_light, void* a2)
			{
				GpuPhaseScope scope(GpuScope::ShadowLocal);
				func(a_light, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowParabolicLight_RenderShadowmaps
		{
			static void thunk(RE::BSShadowLight* a_light, void* a2)
			{
				GpuPhaseScope scope(GpuScope::ShadowLocal);
				func(a_light, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};
	}

	void GpuPhaseTimeline::InstallHooks()
	{
		// Whole-function detours on functions this repo already hooks the same way
		// (FrameAnnotations, Upscaling); MS Detours chains with those when both are on.
		stl::detour_thunk<Main_RenderWaterEffects>(REL::RelocationID(35561, 36560));
		stl::detour_thunk<Main_RenderDepth>(REL::RelocationID(100421, 107139));
		stl::detour_thunk<Main_RenderShadowmasks>(REL::RelocationID(100422, 107140));
		stl::detour_thunk<Main_RenderImagespace>(REL::RelocationID(100430, 107148));
		stl::detour_thunk<MenuManager_DrawInterface>(REL::RelocationID(79947, 82084));

		stl::write_vfunc<0xA, BSShadowDirectionalLight_RenderShadowmaps>(RE::VTABLE_BSShadowDirectionalLight[0]);
		stl::write_vfunc<0xA, BSShadowFrustumLight_RenderShadowmaps>(RE::VTABLE_BSShadowFrustumLight[0]);
		stl::write_vfunc<0xA, BSShadowParabolicLight_RenderShadowmaps>(RE::VTABLE_BSShadowParabolicLight[0]);

		logger::info("[GpuPhaseTimeline] Installed hooks");
	}

	GpuPhaseTimeline* GpuPhaseTimeline::GetSingleton()
	{
		static GpuPhaseTimeline singleton;
		return &singleton;
	}

	bool GpuPhaseTimeline::OverlayWantsTimings() const
	{
		return globals::d3d::device && globals::d3d::context && TableIsOnScreen();
	}

	bool GpuPhaseTimeline::EnsureDisjoint(FrameSlot& a_slot) const
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

	bool GpuPhaseTimeline::EnsureStamp(FrameSlot& a_slot, int a_index) const
	{
		if (a_index < 0 || a_index >= kMaxTimestamps)
			return false;
		if (a_slot.phases.size() < static_cast<size_t>(kMaxTimestamps)) {
			a_slot.phases.resize(kMaxTimestamps, GpuPhase::Untracked);
			a_slot.stamps.reserve(kMaxTimestamps);
		}
		// Created on first use and kept: a frame only ever pays for as many queries as the
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

	void GpuPhaseTimeline::Reset()
	{
		for (auto& slot : slots) {
			slot.disjointBegin = nullptr;
			slot.disjointEnd = nullptr;
			slot.stamps.clear();
			slot.phases.clear();
			slot.count = 0;
			slot.pending = false;
			slot.truncated = false;
		}
		writeSlot = 0;
		frameActive = false;
		reseed = true;
		reseedDraws = true;
		queryDevice = nullptr;
		depth = 0;
		overflowDepth = 0;
		current = GpuPhase::Untracked;
		cascadeCount = 0;
		frameDraws.fill(0);
		frameUntrackedTypes.fill(0);
		report = {};
	}

	void GpuPhaseTimeline::SwitchTo(GpuPhase a_phase)
	{
		if (a_phase == current)
			return;
		current = a_phase;

		auto& slot = slots[writeSlot];
		if (slot.truncated)
			return;
		// The last index is reserved for EndFrame's closing timestamp.
		if (slot.count >= kMaxTimestamps - 1 || !EnsureStamp(slot, slot.count)) {
			slot.truncated = true;
			return;
		}
		globals::d3d::context->End(slot.stamps[slot.count].get());
		slot.phases[slot.count] = a_phase;
		++slot.count;
	}

	void GpuPhaseTimeline::Push(GpuScope a_scope)
	{
		if (!frameActive)
			return;
		if (depth >= kMaxDepth) {
			++overflowDepth;
			return;
		}
		if (a_scope == GpuScope::ShadowSun)
			cascadeCount = 0;
		const GpuPhase phase = InitialPhase(a_scope);
		stack[depth++] = StackEntry{ a_scope, phase };
		SwitchTo(phase);
	}

	void GpuPhaseTimeline::Pop(GpuScope a_scope)
	{
		if (!frameActive)
			return;
		if (overflowDepth > 0) {
			--overflowDepth;
			return;
		}
		for (int i = depth - 1; i >= 0; --i) {
			if (stack[i].scope == a_scope) {
				depth = i;
				SwitchTo(depth > 0 ? stack[depth - 1].phase : GpuPhase::Untracked);
				return;
			}
		}
	}

	GpuPhase GpuPhaseTimeline::ClassifySunCascade()
	{
		// A cascade is wherever the draws go: the shadow-map slice and viewport the engine
		// has set up for this draw. Numbered in the order the engine renders them.
		auto shadowState = globals::game::shadowState;
		if (!shadowState)
			return GpuPhase::ShadowSunCascade1;
		GET_INSTANCE_MEMBER(depthStencil, shadowState)
		GET_INSTANCE_MEMBER(depthStencilSlice, shadowState)
		GET_INSTANCE_MEMBER(viewPort, shadowState)
		const uint64_t key = (static_cast<uint64_t>(depthStencil & 0xFFFF) << 48) |
		                     (static_cast<uint64_t>(depthStencilSlice & 0xFFFF) << 32) |
		                     (static_cast<uint64_t>(static_cast<uint32_t>(viewPort.TopLeftX) & 0xFFFF) << 16) |
		                     static_cast<uint64_t>(static_cast<uint32_t>(viewPort.TopLeftY) & 0xFFFF);

		int index = -1;
		for (int i = 0; i < cascadeCount; ++i) {
			if (cascadeKeys[i] == key) {
				index = i;
				break;
			}
		}
		if (index < 0) {
			if (cascadeCount < kMaxCascades) {
				cascadeKeys[cascadeCount] = key;
				index = cascadeCount++;
			} else {
				index = kMaxCascades - 1;
			}
		}
		return static_cast<GpuPhase>(static_cast<int>(GpuPhase::ShadowSunCascade1) + index);
	}

	void GpuPhaseTimeline::OnDrawActive(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
	{
		// Same counting rule as the overlay's CPU draw-call table: a draw with a BSShader.
		if (!a_shader)
			return;

		if (overflowDepth == 0 && depth > 0) {
			auto& top = stack[depth - 1];
			switch (top.scope) {
			case GpuScope::ShadowSun:
				top.phase = ClassifySunCascade();
				break;
			case GpuScope::Opaque:
				top.phase = ClassifyOpaque(a_shader, a_vertexDescriptor, a_pixelDescriptor);
				break;
			case GpuScope::World:
				top.phase = ClassifyWorld(a_shader);
				break;
			default:
				break;
			}
			SwitchTo(top.phase);
		}

		++frameDraws[static_cast<size_t>(current)];
		if (current == GpuPhase::Untracked) {
			const auto type = static_cast<size_t>(a_shader->shaderType.get());
			if (type < kShaderTypeCount)
				++frameUntrackedTypes[type];
		}
	}

	void GpuPhaseTimeline::BeginFrame()
	{
		if (!OverlayWantsTimings()) {
			// Whatever is still in the ring describes a moment that is long gone by the time
			// the table is opened again; start over from fresh frames.
			frameActive = false;
			reseed = true;
			reseedDraws = true;
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
		slot.count = 0;
		slot.truncated = false;
		depth = 0;
		overflowDepth = 0;
		cascadeCount = 0;
		current = GpuPhase::Untracked;
		frameDraws.fill(0);
		frameUntrackedTypes.fill(0);

		// The opening timestamp gets a degenerate disjoint window, which must not nest inside
		// one of GpuPassTimers'. Nothing of ours is mid-pass at Present; skip the frame if so.
		if (GpuPassTimers::GetSingleton()->HasOpenInterval())
			return;
		if (!EnsureDisjoint(slot) || !EnsureStamp(slot, 0))
			return;

		auto context = globals::d3d::context;
		context->Begin(slot.disjointBegin.get());
		context->End(slot.stamps[0].get());
		context->End(slot.disjointBegin.get());
		slot.phases[0] = GpuPhase::Untracked;
		slot.count = 1;
		frameActive = true;
	}

	void GpuPhaseTimeline::EndFrame()
	{
		if (!frameActive)
			return;
		frameActive = false;

		auto& slot = slots[writeSlot];
		if (slot.truncated) {
			++report.droppedFrames;
		} else if (!GpuPassTimers::GetSingleton()->HasOpenInterval() && EnsureStamp(slot, slot.count)) {
			auto context = globals::d3d::context;
			context->Begin(slot.disjointEnd.get());
			context->End(slot.stamps[slot.count].get());
			context->End(slot.disjointEnd.get());
			slot.phases[slot.count] = current;
			++slot.count;
			slot.pending = true;
		}

		// Draw counts are known now; the times arrive a few frames later.
		for (size_t i = 0; i < kPhaseCount; ++i) {
			const float v = static_cast<float>(frameDraws[i]);
			report.draws[i] = reseedDraws ? v : report.draws[i] * kSmoothingOld + v * kSmoothingNew;
		}
		for (size_t i = 0; i < kShaderTypeCount; ++i) {
			const float v = static_cast<float>(frameUntrackedTypes[i]);
			report.untrackedDrawsByType[i] = reseedDraws ? v : report.untrackedDrawsByType[i] * kSmoothingOld + v * kSmoothingNew;
		}
		reseedDraws = false;

		writeSlot = (writeSlot + 1) % kFramesInFlight;
		depth = 0;
		overflowDepth = 0;
		current = GpuPhase::Untracked;
	}

	GpuPhaseTimeline::SlotStatus GpuPhaseTimeline::TryRead(FrameSlot& a_slot, std::array<double, kPhaseCount>& a_outMs, double& a_outTotalMs)
	{
		auto context = globals::d3d::context;
		if (!context || a_slot.count < 2)
			return SlotStatus::Invalid;

		const auto poll = [context](ID3D11Query* a_query, void* a_out, UINT a_size) {
			return context->GetData(a_query, a_out, a_size, D3D11_ASYNC_GETDATA_DONOTFLUSH);
		};

		// The closing window was issued last, so it is the one to ask first: if it is not
		// back yet, nothing before it needs polling this frame.
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

		scratchTicks.resize(a_slot.count);
		for (int i = 0; i < a_slot.count; ++i) {
			hr = poll(a_slot.stamps[i].get(), &scratchTicks[i], sizeof(UINT64));
			if (hr == S_FALSE)
				return SlotStatus::NotReady;
			if (FAILED(hr))
				return SlotStatus::Invalid;
		}

		const double toMs = 1000.0 / static_cast<double>(beginData.Frequency);
		a_outMs.fill(0.0);
		for (int i = 0; i + 1 < a_slot.count; ++i) {
			if (scratchTicks[i + 1] < scratchTicks[i])
				return SlotStatus::Invalid;
			a_outMs[static_cast<size_t>(a_slot.phases[i])] += static_cast<double>(scratchTicks[i + 1] - scratchTicks[i]) * toMs;
		}
		a_outTotalMs = static_cast<double>(scratchTicks[a_slot.count - 1] - scratchTicks[0]) * toMs;
		if (a_outTotalMs < kMinPlausibleMs || a_outTotalMs > kMaxPlausibleMs)
			return SlotStatus::Invalid;
		return SlotStatus::Ready;
	}

	void GpuPhaseTimeline::Collect()
	{
		std::array<double, kPhaseCount> sampleMs{};
		// Oldest first, so the smoothing sees frames in order.
		for (int age = 0; age < kFramesInFlight; ++age) {
			auto& slot = slots[(writeSlot + age) % kFramesInFlight];
			if (!slot.pending)
				continue;

			double totalMs = 0.0;
			const SlotStatus status = TryRead(slot, sampleMs, totalMs);
			if (status == SlotStatus::NotReady)
				continue;
			slot.pending = false;
			if (status == SlotStatus::Invalid)
				continue;

			// The same linear smoothing on every row and on the total keeps the rows adding
			// up to the total after smoothing, not just per raw frame.
			for (size_t i = 0; i < kPhaseCount; ++i) {
				const float v = static_cast<float>(sampleMs[i]);
				report.ms[i] = reseed ? v : report.ms[i] * kSmoothingOld + v * kSmoothingNew;
			}
			const float total = static_cast<float>(totalMs);
			const float stamps = static_cast<float>(slot.count);
			report.totalMs = reseed ? total : report.totalMs * kSmoothingOld + total * kSmoothingNew;
			report.timestampsPerFrame = reseed ? stamps : report.timestampsPerFrame * kSmoothingOld + stamps * kSmoothingNew;
			report.hasSample = true;
			reseed = false;
		}
	}
}
