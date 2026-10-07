#include "Utils/GpuPhaseTimeline.h"

#include "Features/PerformanceOverlay.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Utils/Batch39Engine.h"
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
			case GpuScope::ShadowSlice:  // always pushed with an explicit phase
				return GpuPhase::ShadowOther;
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

		/// (batch 39) What a draw belongs to, read off the render pass being submitted (its shader
		/// property flags), with the shader descriptors as the fallback. Shared by the depth
		/// prepass and the reflection cubemap breakdowns.
		enum class DrawKind
		{
			Terrain,
			Objects,
			Characters,
			Trees,
			Grass,
			LODLand,
			LODObjects,
			LODTrees,
			Sky,
			Other
		};

		DrawKind ClassifyDrawKind(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
		{
			using Flag = RE::BSShaderProperty::EShaderPropertyFlag;
			const auto type = a_shader->shaderType.get();
			switch (type) {
			case RE::BSShader::Type::Grass:
				return DrawKind::Grass;
			case RE::BSShader::Type::DistantTree:
				return DrawKind::LODTrees;
			case RE::BSShader::Type::Sky:
				return DrawKind::Sky;
			case RE::BSShader::Type::Lighting:
			case RE::BSShader::Type::Utility:
				break;
			default:
				return DrawKind::Other;
			}

			if (const auto* pass = Batch39Engine::CurrentPass(); pass && pass->shaderProperty) {
				const auto& flags = pass->shaderProperty->flags;
				if (flags.any(Flag::kLODLandscape))
					return DrawKind::LODLand;
				if (flags.any(Flag::kLODObjects, Flag::kHDLODObjects))
					return DrawKind::LODObjects;
				if (flags.any(Flag::kMultiTextureLandscape))
					return DrawKind::Terrain;
				if (flags.any(Flag::kTreeAnim))
					return DrawKind::Trees;
				if (flags.any(Flag::kSkinned, Flag::kFace))
					return DrawKind::Characters;
				return DrawKind::Objects;
			}

			if (type == RE::BSShader::Type::Lighting) {
				using Tech = SIE::ShaderCache::LightingShaderTechniques;
				switch (static_cast<Tech>(0x3F & (a_pixelDescriptor >> 24))) {
				case Tech::MTLand:
				case Tech::MTLandLODBlend:
					return DrawKind::Terrain;
				case Tech::LODLand:
				case Tech::LODLandNoise:
					return DrawKind::LODLand;
				case Tech::LODObjects:
				case Tech::LODObjectHD:
					return DrawKind::LODObjects;
				case Tech::TreeAnim:
					return DrawKind::Trees;
				default:
					break;
				}
				return (a_vertexDescriptor & static_cast<uint32_t>(SIE::ShaderCache::LightingShaderFlags::Skinned)) ? DrawKind::Characters : DrawKind::Objects;
			}

			using UFlag = SIE::ShaderCache::UtilityShaderFlags;
			const uint64_t vd = a_vertexDescriptor;
			if (vd & static_cast<uint64_t>(UFlag::LodLandscape))
				return DrawKind::LODLand;
			if (vd & static_cast<uint64_t>(UFlag::LodObject))
				return DrawKind::LODObjects;
			if (vd & static_cast<uint64_t>(UFlag::TreeAnim))
				return DrawKind::Trees;
			if (vd & static_cast<uint64_t>(UFlag::Skinned))
				return DrawKind::Characters;
			return DrawKind::Objects;
		}

		GpuPhase ClassifyDepth(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
		{
			switch (ClassifyDrawKind(a_shader, a_vertexDescriptor, a_pixelDescriptor)) {
			case DrawKind::Terrain:
				return GpuPhase::DepthTerrain;
			case DrawKind::Objects:
				return GpuPhase::DepthObjects;
			case DrawKind::Characters:
				return GpuPhase::DepthCharacters;
			case DrawKind::Trees:
				return GpuPhase::DepthTrees;
			case DrawKind::Grass:
				return GpuPhase::DepthGrass;
			case DrawKind::LODLand:
				return GpuPhase::DepthLODLand;
			case DrawKind::LODObjects:
				return GpuPhase::DepthLODObjects;
			case DrawKind::LODTrees:
				return GpuPhase::DepthLODTrees;
			default:
				return GpuPhase::DepthPrepass;
			}
		}

		GpuPhase ClassifyReflection(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor)
		{
			switch (ClassifyDrawKind(a_shader, a_vertexDescriptor, a_pixelDescriptor)) {
			case DrawKind::LODLand:
				return GpuPhase::ReflLODLand;
			case DrawKind::LODObjects:
				return GpuPhase::ReflLODObjects;
			case DrawKind::LODTrees:
				return GpuPhase::ReflLODTrees;
			case DrawKind::Sky:
				return GpuPhase::ReflSky;
			default:
				return GpuPhase::Reflections;
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
				auto* timeline = GpuPhaseTimeline::GetSingleton();
				if (timeline->IsRecording())
					timeline->NoteDirectionalLight(a_light);
				GpuPhaseScope scope(GpuScope::ShadowSun);
				func(a_light, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowFrustumLight_RenderShadowmaps
		{
			static void thunk(RE::BSShadowLight* a_light, void* a2)
			{
				GpuPhaseTimeline::GetSingleton()->NoteLocalShadowMap();
				GpuPhaseScope scope(GpuScope::ShadowLocal);
				func(a_light, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct BSShadowParabolicLight_RenderShadowmaps
		{
			static void thunk(RE::BSShadowLight* a_light, void* a2)
			{
				GpuPhaseTimeline::GetSingleton()->NoteLocalShadowMap();
				GpuPhaseScope scope(GpuScope::ShadowLocal);
				func(a_light, a2);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------------------------------------------------------------------------------
		// (batch 37a) The three calls of BSShadowLight::RenderShadowmap (ID 100820) inside
		// BSShadowDirectionalLight::RenderShadowmaps (ID 101495), SE 1.5.97:
		//   +0x6F  god-ray (volumetric) maps, target 3, slice = cascade, flags 0x100
		//   +0xC6  sun cascades,              target 2, slice = cascade
		//   +0x12B character (focus) maps,    target 4
		// Each call clears one shadow map, submits every draw collected for it and writes its
		// light matrix. Bracketing the call bills the clear and the submit to the right row on
		// both clocks. Measurement only: the original always runs with its own arguments.
		// ---------------------------------------------------------------------------------
		enum class SliceKind
		{
			GodRay,
			Sun,
			Focus
		};

		GpuPhase SlicePhase(SliceKind a_kind, uint32_t a_slice)
		{
			const int i = static_cast<int>(std::min<uint32_t>(a_slice, 2u));
			switch (a_kind) {
			case SliceKind::GodRay:
				return static_cast<GpuPhase>(static_cast<int>(GpuPhase::ShadowGodRay1) + i);
			case SliceKind::Sun:
				return static_cast<GpuPhase>(static_cast<int>(GpuPhase::ShadowSun1) + i);
			default:
				return GpuPhase::ShadowFocus;
			}
		}

		template <SliceKind Kind>
		struct RenderShadowmapCall
		{
			static void thunk(RE::BSShadowLight* a_light, RE::BSShadowLight::ShadowmapDescriptor* a_desc, uintptr_t a3, uintptr_t a4)
			{
				auto* timeline = GpuPhaseTimeline::GetSingleton();
				if (!timeline->IsRecording() || !a_desc) {
					func(a_light, a_desc, a3, a4);
					return;
				}
				if constexpr (Kind == SliceKind::Focus)
					timeline->NoteFocusShadowMap();
				if constexpr (Kind == SliceKind::GodRay)
					timeline->NoteGodRayPass();
				timeline->PushPhase(GpuScope::ShadowSlice, SlicePhase(Kind, a_desc->shadowmapIndex));
				func(a_light, a_desc, a3, a4);
				timeline->Pop(GpuScope::ShadowSlice);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/// True if a_site is a rel32 CALL to a_target (the bytes about to be patched).
		bool IsCallTo(uintptr_t a_site, uintptr_t a_target)
		{
			const auto* p = reinterpret_cast<const uint8_t*>(a_site);
			if (p[0] != 0xE8)
				return false;
			int32_t rel = 0;
			std::memcpy(&rel, p + 1, sizeof(rel));
			return a_site + 5 + static_cast<intptr_t>(rel) == a_target;
		}

		int64_t QpcNow()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcToMs()
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return toMs;
		}
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

		// (batch 37a) Per-shadow-map brackets. The offsets were read off SkyrimSE.exe 1.5.97
		// only, so anything else keeps the per-draw classification (same rows, but the clear
		// before a map's first draw is billed to the map before it).
		bool sliceHooks = false;
		if (REL::Module::IsSE()) {
			const uintptr_t body = REL::ID(101495).address();
			const uintptr_t target = REL::ID(100820).address();
			const uintptr_t godRay = body + 0x6F, sun = body + 0xC6, focus = body + 0x12B;
			if (IsCallTo(godRay, target) && IsCallTo(sun, target) && IsCallTo(focus, target)) {
				stl::write_thunk_call<RenderShadowmapCall<SliceKind::GodRay>>(godRay);
				stl::write_thunk_call<RenderShadowmapCall<SliceKind::Sun>>(sun);
				stl::write_thunk_call<RenderShadowmapCall<SliceKind::Focus>>(focus);
				sliceHooks = true;
			} else {
				logger::warn("[GpuPhaseTimeline] RenderShadowmap call sites do not match SE 1.5.97; per-shadow-map brackets off");
			}
		}
		GetSingleton()->SetSliceHooksInstalled(sliceHooks);

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
		frameDraws.fill(0);
		frameUntrackedTypes.fill(0);
		frameCpuTicks.fill(0);
		lastSwitchTicks = 0;
		reseedCpu = true;
		frameShadow = {};
		report = {};
	}

	void GpuPhaseTimeline::SwitchTo(GpuPhase a_phase)
	{
		if (a_phase == current)
			return;
		// (batch 37a) CPU side: the render thread's wall time since the last switch belongs to
		// the stage that was active, the same exclusive rule as the GPU timestamps.
		const int64_t now = QpcNow();
		frameCpuTicks[static_cast<size_t>(current)] += now - lastSwitchTicks;
		lastSwitchTicks = now;
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
		const GpuPhase phase = InitialPhase(a_scope);
		stack[depth++] = StackEntry{ a_scope, phase };
		SwitchTo(phase);
	}

	void GpuPhaseTimeline::PushPhase(GpuScope a_scope, GpuPhase a_phase)
	{
		if (!frameActive)
			return;
		if (depth >= kMaxDepth) {
			++overflowDepth;
			return;
		}
		stack[depth++] = StackEntry{ a_scope, a_phase };
		SwitchTo(a_phase);
	}

	void GpuPhaseTimeline::NoteDirectionalLight(void* a_light)
	{
		// Plain reads of the light the engine is about to render. SE/AE layout only: VR's
		// descriptor array differs and is not read.
		if (!a_light || REL::Module::IsVR())
			return;
		auto* light = static_cast<RE::BSShadowDirectionalLight*>(a_light);
		ShadowInfo& s = frameShadow;
		s.valid = true;
		s.sunCascades = light->shadowMapCount;
		s.drawFocusShadows = *reinterpret_cast<const bool*>(reinterpret_cast<const uint8_t*>(light) + 0x558);
		const auto& dir = light->GetShadowDirectionalLightRuntimeData();
		const auto& descs = light->GetRuntimeData().shadowmapDescriptors;
		for (uint32_t i = 0; i < 3; ++i) {
			s.startSplit[i] = dir.startSplitDistances[i];
			s.endSplit[i] = dir.endSplitDistances[i];
			if (i < descs.size() && i < s.sunCascades) {
				const auto& d = descs[i];
				float upt = 0.0f;
				std::memcpy(&upt, &d.unitsPerTexel, sizeof(upt));  // a float in 1.5.97 (reset to 1.0f)
				s.unitsPerTexel[i] = upt;
				std::memcpy(s.port[i], &d.port, sizeof(s.port[i]));  // left, right, top, bottom (protected in NiRect)
			}
		}
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

	GpuPhase GpuPhaseTimeline::ClassifyDirectionalShadowDraw() const
	{
		// (batch 37a) The shadow map this draw renders into, read off the engine's render
		// target state: target 3 = god-ray maps, 2 = sun cascades (slice = cascade, 0 nearest),
		// 4 = character maps. Where the per-map call-site hooks are installed they push the
		// same phase first, and this is not consulted.
		auto shadowState = globals::game::shadowState;
		if (!shadowState)
			return GpuPhase::ShadowOther;
		GET_INSTANCE_MEMBER(depthStencil, shadowState)
		GET_INSTANCE_MEMBER(depthStencilSlice, shadowState)
		const uint32_t slice = std::min<uint32_t>(static_cast<uint32_t>(depthStencilSlice), 2u);
		switch (static_cast<int>(depthStencil)) {
		case RE::RENDER_TARGETS_DEPTHSTENCIL::kVOLUMETRIC_LIGHTING_SHADOWMAPS_ESRAM:
			return static_cast<GpuPhase>(static_cast<int>(GpuPhase::ShadowGodRay1) + slice);
		case RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS_ESRAM:
			return static_cast<GpuPhase>(static_cast<int>(GpuPhase::ShadowSun1) + slice);
		case RE::RENDER_TARGETS_DEPTHSTENCIL::kSHADOWMAPS:
			return GpuPhase::ShadowFocus;
		default:
			return GpuPhase::ShadowOther;
		}
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
				top.phase = ClassifyDirectionalShadowDraw();
				break;
			case GpuScope::Opaque:
				top.phase = ClassifyOpaque(a_shader, a_vertexDescriptor, a_pixelDescriptor);
				break;
			case GpuScope::World:
				top.phase = ClassifyWorld(a_shader);
				break;
			case GpuScope::DepthPrepass:
				top.phase = ClassifyDepth(a_shader, a_vertexDescriptor, a_pixelDescriptor);
				break;
			case GpuScope::Reflections:
				top.phase = ClassifyReflection(a_shader, a_vertexDescriptor, a_pixelDescriptor);
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
			reseedCpu = true;
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
		current = GpuPhase::Untracked;
		frameDraws.fill(0);
		frameUntrackedTypes.fill(0);
		frameCpuTicks.fill(0);
		lastSwitchTicks = QpcNow();
		frameShadow = {};

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

		// Draw counts and CPU times are known now; the GPU times arrive a few frames later.
		{
			const int64_t now = QpcNow();
			frameCpuTicks[static_cast<size_t>(current)] += now - lastSwitchTicks;
			lastSwitchTicks = now;
		}
		const double toMs = QpcToMs();
		for (size_t i = 0; i < kPhaseCount; ++i) {
			const float v = static_cast<float>(frameDraws[i]);
			report.draws[i] = reseedDraws ? v : report.draws[i] * kSmoothingOld + v * kSmoothingNew;
			report.lastDraws[i] = v;
			const float c = static_cast<float>(static_cast<double>(frameCpuTicks[i]) * toMs);
			report.cpuMs[i] = reseedCpu ? c : report.cpuMs[i] * kSmoothingOld + c * kSmoothingNew;
			report.lastCpuMs[i] = c;
		}
		reseedCpu = false;
		report.hasCpuSample = true;
		frameShadow.sliceHooks = sliceHooksInstalled;
		report.shadow = frameShadow;
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
				report.lastMs[i] = v;
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
