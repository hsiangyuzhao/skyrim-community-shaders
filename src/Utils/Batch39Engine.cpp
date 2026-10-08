// Engine-level changes: water reflection cubemap relief, depth prepass breakdown,
// temporal LOD dither, texture clarity. See Batch39Engine.h for what the engine does and why
// each switch is built the way it is.
#include "Utils/Batch39Engine.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <bit>
#include <format>
#include <imgui.h>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Deferred.h"
#include "Features/DynamicCubemaps.h"
#include "Features/TerrainBlending.h"
#include "Globals.h"
#include "Menu.h"
#include "ShaderCache.h"
#include "State.h"
#include "Util.h"
#include "Utils/GpuPhaseTimeline.h"
#include "Utils/GpuTimers.h"
#include "Utils/UI.h"

namespace Batch39Engine
{
	namespace
	{
		// ---------------------------------------------------------------------------------
		// Engine INI values overridden in memory, restored when the override is switched off.
		// ---------------------------------------------------------------------------------
		struct EngineBool
		{
			const char* name;
			RE::Setting* setting = nullptr;
			bool lookedUp = false;
			bool overridden = false;
			bool original = false;  ///< the value before our override (the user's ini / the engine default)

			RE::Setting* Get()
			{
				if (!lookedUp) {
					setting = RE::GetINISetting(name);
					lookedUp = true;
					if (!setting)
						logger::warn("[Engine] engine setting {} not found", name);
				}
				return setting;
			}

			/// Value the engine reads right now.
			bool Current()
			{
				auto* s = Get();
				return s ? s->data.b : false;
			}

			/// The user's own value (what is restored when our override is off).
			bool UserValue()
			{
				return overridden ? original : Current();
			}

			void Apply(bool a_override, bool a_value)
			{
				auto* s = Get();
				if (!s)
					return;
				if (a_override) {
					if (!overridden) {
						original = s->data.b;
						overridden = true;
					}
					s->data.b = a_value;
				} else if (overridden) {
					s->data.b = original;
					overridden = false;
				}
			}
		};

		EngineBool g_reflectLODTrees{ "bReflectLODTrees:Water" };
		EngineBool g_reflectLODObjects{ "bReflectLODObjects:Water" };
		EngineBool g_reflectLODLand{ "bReflectLODLand:Water" };
		EngineBool g_reflectSky{ "bReflectSky:Water" };
		EngineBool g_lodZPrepass{ "bLodZPrepass:Display" };
		EngineBool g_frontToBackPrepass{ "bEnableFrontToBackPrepass:Display" };
		EngineBool g_stippleFade{ "bEnableStippleFade:Display" };

		float ReadEngineFloat(const char* a_name, float a_fallback)
		{
			auto* s = RE::GetINISetting(a_name);
			return s ? s->data.f : a_fallback;
		}

		// ---------------------------------------------------------------------------------
		// Render pass / depth prepass tracking (always-on hooks).
		// ---------------------------------------------------------------------------------
		RE::BSRenderPass* g_currentPass = nullptr;
		bool g_inDepthPrepass = false;
		bool g_passHookInstalled = false;
		bool g_depthHookInstalled = false;
		bool g_samplerHookInstalled = false;

		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
			{
				RE::BSRenderPass* previous = g_currentPass;
				g_currentPass = a_pass;
				func(a_pass, a_technique, a_alphaTest, a_renderFlags);
				g_currentPass = previous;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		struct Main_RenderDepth
		{
			static void thunk(bool a1, bool a2)
			{
				const bool previous = g_inDepthPrepass;
				g_inDepthPrepass = true;
				func(a1, a2);
				g_inDepthPrepass = previous;
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		// ---------------------------------------------------------------------------------
		// 4. 16x anisotropic filtering: an unclamped twin of every sampler that asked for more
		// than 8x, swapped in at PSSetSamplers while the switch is on.
		// ---------------------------------------------------------------------------------
		std::shared_mutex g_samplerMutex;
		std::unordered_map<ID3D11SamplerState*, winrt::com_ptr<ID3D11SamplerState>> g_samplerTwins;  // clamped -> unclamped
		std::unordered_map<ID3D11SamplerState*, ID3D11SamplerState*> g_samplerOriginals;             // unclamped -> clamped
		std::array<uint32_t, 17> g_samplerRequests{};                                                ///< samplers created, by requested MaxAnisotropy (16 = 16 or more)
		bool g_anisoApplied = false;

		// The hot path (every PSSetSamplers the game makes) reads a published, never-modified
		// snapshot of the pairs without taking a lock. A new snapshot is published after each
		// twin is made (the game makes its samplers once, at start-up); old ones are kept alive.
		struct TwinSnapshot
		{
			std::vector<std::pair<ID3D11SamplerState*, ID3D11SamplerState*>> pairs;  // clamped, unclamped
		};
		std::vector<std::unique_ptr<TwinSnapshot>> g_twinSnapshots;  // guarded by g_samplerMutex
		std::atomic<const TwinSnapshot*> g_twinSnapshot{ nullptr };

		ID3D11SamplerState* FindTwin(const TwinSnapshot* a_snapshot, ID3D11SamplerState* a_sampler)
		{
			for (const auto& [clamped, unclamped] : a_snapshot->pairs)
				if (clamped == a_sampler)
					return unclamped;
			return nullptr;
		}

		bool IsAnisotropicFilter(D3D11_FILTER a_filter)
		{
			return a_filter == D3D11_FILTER_ANISOTROPIC || a_filter == D3D11_FILTER_COMPARISON_ANISOTROPIC ||
			       a_filter == D3D11_FILTER_MINIMUM_ANISOTROPIC || a_filter == D3D11_FILTER_MAXIMUM_ANISOTROPIC;
		}

		struct ID3D11DeviceContext_PSSetSamplers
		{
			static void STDMETHODCALLTYPE thunk(ID3D11DeviceContext* This, UINT StartSlot, UINT NumSamplers, ID3D11SamplerState* const* ppSamplers)
			{
				const TwinSnapshot* snapshot = g_anisoApplied ? g_twinSnapshot.load(std::memory_order_acquire) : nullptr;
				if (snapshot && ppSamplers && NumSamplers > 0 && NumSamplers <= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
					std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> swapped{};
					bool any = false;
					for (UINT i = 0; i < NumSamplers; ++i) {
						swapped[i] = ppSamplers[i];
						if (auto* twin = ppSamplers[i] ? FindTwin(snapshot, ppSamplers[i]) : nullptr) {
							swapped[i] = twin;
							any = true;
						}
					}
					if (any) {
						func(This, StartSlot, NumSamplers, swapped.data());
						return;
					}
				}
				func(This, StartSlot, NumSamplers, ppSamplers);
			}
			static inline REL::Relocation<decltype(thunk)> func;
		};

		/// Re-points the pixel shader slots that are bound right now: the engine only rebinds a
		/// slot when its own choice changes, so a switch would otherwise wait for that.
		void RebindPixelSamplers(bool a_toTwins)
		{
			auto* context = globals::d3d::context;
			if (!context)
				return;
			std::array<ID3D11SamplerState*, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> bound{};
			context->PSGetSamplers(0, static_cast<UINT>(bound.size()), bound.data());
			bool any = false;
			{
				std::shared_lock lock(g_samplerMutex);
				for (auto& s : bound) {
					if (!s)
						continue;
					if (a_toTwins) {
						if (auto it = g_samplerTwins.find(s); it != g_samplerTwins.end()) {
							s->Release();
							s = it->second.get();
							s->AddRef();
							any = true;
						}
					} else if (auto it = g_samplerOriginals.find(s); it != g_samplerOriginals.end()) {
						s->Release();
						s = it->second;
						s->AddRef();
						any = true;
					}
				}
			}
			if (any)
				ID3D11DeviceContext_PSSetSamplers::func(context, 0, static_cast<UINT>(bound.size()), bound.data());
			for (auto* s : bound)
				if (s)
					s->Release();
		}

		// ---------------------------------------------------------------------------------
		// 1. Reflection cubemap scheduler and diagnostics.
		// ---------------------------------------------------------------------------------
		struct CubeCamera
		{
			const void* camera = nullptr;
			uint32_t lastFrame = UINT32_MAX;
			uint32_t cursor = 0;
		};
		std::array<CubeCamera, 4> g_cubeCameras{};

		CubeCamera& CameraSlot(const void* a_camera)
		{
			for (auto& c : g_cubeCameras)
				if (c.camera == a_camera)
					return c;
			for (auto& c : g_cubeCameras)
				if (!c.camera) {
					c.camera = a_camera;
					return c;
				}
			// More water cubemaps than slots (uMaxExteriorWaterReflections is 2 by default):
			// reuse the last slot; its faces then advance a little faster, nothing breaks.
			g_cubeCameras.back().camera = a_camera;
			return g_cubeCameras.back();
		}

		struct CubemapFrame
		{
			uint32_t calls = 0;
			uint32_t facesRequested = 0;
			uint32_t facesDrawn = 0;
			uint32_t cameras = 0;
			bool mapMenu = false;
		};
		CubemapFrame g_cubeThis{}, g_cubeLast{};
		float g_cubeCallsAvg = 0.0f, g_cubeRequestedAvg = 0.0f, g_cubeDrawnAvg = 0.0f;
		bool g_cubeSeeded = false;
		const void* g_cubeLastCamera = nullptr;
		uint64_t g_cubeTotalCalls = 0;


	}

	// =====================================================================================
	// Settings
	// =====================================================================================

	// The three water reflection options are loaded, saved and drawn by Water Effects
	// (Water > Water Effects > Reflection Cubemap). Everything else is fixed.

	// =====================================================================================
	// Effective state
	// =====================================================================================

	bool ReflHandOffActive()
	{
		return settings.ReflHandOffToDynamicCubemaps && globals::features::dynamicCubemaps.loaded;
	}
	// Fixed: one face per frame round robin unless handed off; while handed off the game's
	// cubemap keeps the sky only and Dynamic Cubemaps captures every frame (39b lag fixes).
	bool ReflThrottleActive() { return !ReflHandOffActive(); }
	bool ReflHandOffKeepSkyActive() { return ReflHandOffActive(); }
	bool ReflHandOffFastCaptureActive() { return ReflHandOffActive(); }
	float DynamicCubemapCaptureWeight() { return ReflHandOffFastCaptureActive() ? std::clamp(settings.ReflHandOffCaptureWeight, 0.25f, 1.0f) : 0.5f; }
	bool ReflSkipLODTreesActive() { return true; }
	bool ReflSkipLODObjectsActive() { return settings.ReflSkipLODObjects; }
	bool TemporalLODDitherActive() { return true; }
	bool MipBiasMaterialsActive() { return true; }
	bool MipBiasSpecularActive() { return true; }
	bool Anisotropic16xActive() { return g_samplerHookInstalled; }

	uint32_t ShaderFlags()
	{
		uint32_t flags = 0;
		if (TemporalLODDitherActive())
			flags |= ShaderFlag::TemporalLODDither;
		if (MipBiasMaterialsActive())
			flags |= ShaderFlag::MipBiasMaterials;
		if (MipBiasSpecularActive())
			flags |= ShaderFlag::MipBiasSpecular;
		if (ReflHandOffActive())
			flags |= ShaderFlag::WaterDynamicCubemapOnly;
		return flags;
	}

	RE::BSRenderPass* CurrentPass() { return g_currentPass; }
	bool InDepthPrepass() { return g_inDepthPrepass; }

	// =====================================================================================
	// Hooks
	// =====================================================================================

	void InstallHooks()
	{
		// BSBatchRenderer::RenderPassImmediately (SE ID 100854, checked against SkyrimSE.exe
		// 1.5.97: the call at RenderBatches+0x29E lands on it). On AE and VR the target is read off
		// that same call site, the one Terrain Blending and Interior Sun already hook, before they
		// do, and only used if it points into the game's own code.
		uintptr_t target = 0;
		if (!REL::Module::IsSE()) {
			const uintptr_t site = REL::RelocationID(100852, 107642).address() + REL::Relocate(0x29E, 0x28F);
			const auto* p = reinterpret_cast<const uint8_t*>(site);
			if (p[0] == 0xE8) {
				int32_t rel = 0;
				std::memcpy(&rel, p + 1, sizeof(rel));
				const uintptr_t candidate = site + 5 + static_cast<intptr_t>(rel);
				const auto& module = REL::Module::get();
				if (candidate >= module.base() && candidate < module.base() + module.segment(REL::Segment::textx).offset() + module.segment(REL::Segment::textx).size())
					target = candidate;
			}
		} else {
			target = REL::ID(100854).address();
		}
		if (target) {
			BSBatchRenderer_RenderPassImmediately::func = target;
			DetourTransactionBegin();
			DetourUpdateThread(GetCurrentThread());
			DetourAttach(reinterpret_cast<PVOID*>(&BSBatchRenderer_RenderPassImmediately::func), reinterpret_cast<PVOID>(BSBatchRenderer_RenderPassImmediately::thunk));
			g_passHookInstalled = DetourTransactionCommit() == NO_ERROR;
		}
		if (!g_passHookInstalled)
			logger::warn("[Engine] RenderPassImmediately not hooked: no per-object prepass breakdown");

		// Main_RenderDepth (same function the overlay's timeline and Terrain Blending bracket).
		stl::detour_thunk<Main_RenderDepth>(REL::RelocationID(100421, 107139));
		g_depthHookInstalled = true;

		logger::info("[Engine] Installed engine hooks");
	}

	void InstallContextHooks(ID3D11DeviceContext* a_context)
	{
		if (!a_context || g_samplerHookInstalled)
			return;
		stl::detour_vfunc<10, ID3D11DeviceContext_PSSetSamplers>(a_context);
		g_samplerHookInstalled = true;
	}

	void OnSamplerCreated(ID3D11Device* a_device, const D3D11_SAMPLER_DESC& a_requested, ID3D11SamplerState* a_clamped, CreateSamplerFn a_create)
	{
		{
			std::unique_lock lock(g_samplerMutex);
			++g_samplerRequests[std::min<UINT>(a_requested.MaxAnisotropy, 16u)];
		}
		if (!a_clamped || !a_create || !IsAnisotropicFilter(a_requested.Filter) || a_requested.MaxAnisotropy <= 8)
			return;
		{
			std::shared_lock lock(g_samplerMutex);
			if (g_samplerTwins.contains(a_clamped))
				return;  // the runtime handed back an existing object for an identical description
		}
		D3D11_SAMPLER_DESC desc = a_requested;
		desc.MaxAnisotropy = std::min<UINT>(desc.MaxAnisotropy, 16u);
		winrt::com_ptr<ID3D11SamplerState> twin;
		if (FAILED(a_create(a_device, &desc, twin.put())) || !twin)
			return;
		std::unique_lock lock(g_samplerMutex);
		// Hold the clamped object too, so its address can never be reused by an unrelated
		// sampler while it is a key here. The game creates its samplers once and keeps them.
		a_clamped->AddRef();
		g_samplerOriginals[twin.get()] = a_clamped;
		g_samplerTwins.emplace(a_clamped, std::move(twin));

		auto snapshot = std::make_unique<TwinSnapshot>();
		snapshot->pairs.reserve(g_samplerTwins.size());
		for (const auto& [clamped, unclamped] : g_samplerTwins)
			snapshot->pairs.emplace_back(clamped, unclamped.get());
		g_twinSnapshot.store(snapshot.get(), std::memory_order_release);
		g_twinSnapshots.push_back(std::move(snapshot));
	}

	// =====================================================================================
	// Frame boundary
	// =====================================================================================

	void OnFrameStart()
	{
		// Engine INI overrides: read by the engine every frame (see the header).
		// (39b) Hand-off keeping the sky: the game's cubemap holds the sky alone.
		const bool skyOnly = ReflHandOffKeepSkyActive();
		g_reflectLODTrees.Apply(ReflSkipLODTreesActive() || skyOnly, false);
		g_reflectLODObjects.Apply(ReflSkipLODObjectsActive() || skyOnly, false);
		g_reflectLODLand.Apply(skyOnly, false);

		// 16x AF: swap what is bound right now when the effective state flips.
		const bool aniso = Anisotropic16xActive();
		if (aniso != g_anisoApplied) {
			g_anisoApplied = aniso;
			RebindPixelSamplers(aniso);
		}

		// Reflection cubemap statistics.
		constexpr float kOld = 0.95f, kNew = 0.05f;
		g_cubeLast = g_cubeThis;
		if (!g_cubeSeeded) {
			g_cubeCallsAvg = static_cast<float>(g_cubeLast.calls);
			g_cubeRequestedAvg = static_cast<float>(g_cubeLast.facesRequested);
			g_cubeDrawnAvg = static_cast<float>(g_cubeLast.facesDrawn);
			g_cubeSeeded = true;
		} else {
			g_cubeCallsAvg = g_cubeCallsAvg * kOld + static_cast<float>(g_cubeLast.calls) * kNew;
			g_cubeRequestedAvg = g_cubeRequestedAvg * kOld + static_cast<float>(g_cubeLast.facesRequested) * kNew;
			g_cubeDrawnAvg = g_cubeDrawnAvg * kOld + static_cast<float>(g_cubeLast.facesDrawn) * kNew;
		}
		g_cubeThis = {};
		g_cubeLastCamera = nullptr;

	}

	// =====================================================================================
	// 1. Reflection cubemap
	// =====================================================================================

	int FilterCubemapFaces(RE::NiAVObject* a_camera, int a_faceMask, bool)
	{
		++g_cubeThis.calls;
		++g_cubeTotalCalls;
		g_cubeThis.facesRequested += std::popcount(static_cast<uint32_t>(a_faceMask) & 0x3Fu);
		if (a_camera != g_cubeLastCamera) {
			++g_cubeThis.cameras;
			g_cubeLastCamera = a_camera;
		}

		// The map view draws all six faces at once; leave it to the engine.
		const bool mapMenu = globals::game::ui && globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
		g_cubeThis.mapMenu |= mapMenu;
		if (mapMenu)
			return a_faceMask;

		// Dynamic Cubemaps takes over: the engine cubemap is not drawn. The calls still go
		// through with no faces so the camera's scene list is cleared as usual. (39b) Unless it
		// keeps the sky: then it draws the sky alone (OnFrameStart turns the LOD lists off), in
		// the same round robin, for Dynamic Cubemaps to fill the directions it has not seen.
		if (ReflHandOffActive() && !ReflHandOffKeepSkyActive())
			return 0;

		if (!(ReflThrottleActive() || ReflHandOffKeepSkyActive()) || !globals::state)
			return a_faceMask;

		// Our own round robin, independent of which faces the engine asked for: any face can be
		// drawn from this frame's scene list. The first call of a frame (per camera) draws the
		// faces; the engine's second call of the frame then only clears the scene list.
		auto& cam = CameraSlot(a_camera);
		const uint32_t frame = globals::state->frameCount;
		if (cam.lastFrame == frame)
			return 0;
		cam.lastFrame = frame;
		// One face per frame: a whole cube every 6 frames.
		const int mask = 1 << cam.cursor;
		cam.cursor = (cam.cursor + 1) % 6;
		return mask;
	}

	void NoteCubemapCall(int, int a_drawnMask)
	{
		g_cubeThis.facesDrawn += std::popcount(static_cast<uint32_t>(a_drawnMask) & 0x3Fu);
	}

	void ClearShaderCache() {}

	// =====================================================================================
	// Diagnostics
	// =====================================================================================

	namespace
	{
		struct PhaseSum
		{
			float ms = 0.0f;
			float draws = 0.0f;
			float lastMs = 0.0f;
			float lastDraws = 0.0f;
		};

		PhaseSum SumPhases(std::initializer_list<Util::GpuPhase> a_phases)
		{
			const auto& r = Util::GpuPhaseTimeline::GetSingleton()->Get();
			PhaseSum s;
			for (auto p : a_phases) {
				const auto i = static_cast<size_t>(p);
				s.ms += r.ms[i];
				s.draws += r.draws[i];
				s.lastMs += r.lastMs[i];
				s.lastDraws += r.lastDraws[i];
			}
			return s;
		}

		constexpr std::initializer_list<Util::GpuPhase> kDepthPhases = {
			Util::GpuPhase::DepthTerrain, Util::GpuPhase::DepthObjects, Util::GpuPhase::DepthCharacters,
			Util::GpuPhase::DepthTrees, Util::GpuPhase::DepthGrass, Util::GpuPhase::DepthLODLand,
			Util::GpuPhase::DepthLODObjects, Util::GpuPhase::DepthLODTrees, Util::GpuPhase::DepthPrepass
		};
		constexpr std::initializer_list<Util::GpuPhase> kReflPhases = {
			Util::GpuPhase::ReflLODLand, Util::GpuPhase::ReflLODObjects, Util::GpuPhase::ReflLODTrees,
			Util::GpuPhase::ReflSky, Util::GpuPhase::Reflections
		};

		json PhaseJson(Util::GpuPhase a_phase)
		{
			const auto& r = Util::GpuPhaseTimeline::GetSingleton()->Get();
			const auto i = static_cast<size_t>(a_phase);
			return { { "gpu_ms", r.ms[i] }, { "draws", r.draws[i] }, { "gpu_ms_last_frame", r.lastMs[i] }, { "draws_last_frame", r.lastDraws[i] }, { "cpu_ms", r.cpuMs[i] } };
		}
	}

	json DiagnosticsJson()
	{
		json j;
		j["settings"] = {
			{ "refl_skip_lod_objects", settings.ReflSkipLODObjects },
			{ "refl_handoff_dynamic_cubemaps", settings.ReflHandOffToDynamicCubemaps },
			{ "refl_handoff_capture_weight", settings.ReflHandOffCaptureWeight },
		};
		j["active"] = {
			{ "refl_throttle", ReflThrottleActive() },
			{ "refl_handoff_dynamic_cubemaps", ReflHandOffActive() },
			{ "refl_skip_lod_trees", ReflSkipLODTreesActive() },
			{ "refl_skip_lod_objects", ReflSkipLODObjectsActive() },
			{ "refl_handoff_keep_sky", ReflHandOffKeepSkyActive() },
			{ "refl_handoff_fast_capture", ReflHandOffFastCaptureActive() },
			{ "dynamic_cubemaps_capture_weight", DynamicCubemapCaptureWeight() },
			{ "temporal_lod_dither", TemporalLODDitherActive() },
			{ "mip_bias_materials", MipBiasMaterialsActive() },
			{ "mip_bias_specular", MipBiasSpecularActive() },
			{ "anisotropic_16x", Anisotropic16xActive() },
			{ "shader_flags", ShaderFlags() },
		};
		j["hooks"] = { { "render_pass", g_passHookInstalled }, { "depth_prepass", g_depthHookInstalled }, { "ps_samplers", g_samplerHookInstalled } };

		j["engine_ini"] = {
			{ "fCubeMapRefreshRate:Water_seconds", ReadEngineFloat("fCubeMapRefreshRate:Water", -1.0f) },
			{ "bReflectLODTrees:Water", { { "now", g_reflectLODTrees.Current() }, { "user", g_reflectLODTrees.UserValue() } } },
			{ "bReflectLODObjects:Water", { { "now", g_reflectLODObjects.Current() }, { "user", g_reflectLODObjects.UserValue() } } },
			{ "bReflectLODLand:Water", { { "now", g_reflectLODLand.Current() }, { "user", g_reflectLODLand.UserValue() } } },
			{ "bReflectSky:Water", g_reflectSky.Current() },
			{ "bLodZPrepass:Display", g_lodZPrepass.Current() },
			{ "bEnableFrontToBackPrepass:Display", g_frontToBackPrepass.Current() },
			{ "bEnableStippleFade:Display", g_stippleFade.Current() },
		};

		const auto refl = SumPhases(kReflPhases);
		j["reflection_cubemap"] = {
			{ "calls_per_frame", g_cubeCallsAvg },
			{ "faces_requested_per_frame", g_cubeRequestedAvg },
			{ "faces_drawn_per_frame", g_cubeDrawnAvg },
			{ "last_frame", { { "calls", g_cubeLast.calls }, { "faces_requested", g_cubeLast.facesRequested }, { "faces_drawn", g_cubeLast.facesDrawn }, { "cameras", g_cubeLast.cameras }, { "map_menu", g_cubeLast.mapMenu } } },
			{ "gpu_ms", refl.ms },
			{ "draws", refl.draws },
			{ "draws_per_face", g_cubeDrawnAvg > 0.01f ? json(refl.draws / g_cubeDrawnAvg) : json(nullptr) },
			{ "gpu_ms_per_face", g_cubeDrawnAvg > 0.01f ? json(refl.ms / g_cubeDrawnAvg) : json(nullptr) },
			{ "by_type", {
							 { "lod_terrain", PhaseJson(Util::GpuPhase::ReflLODLand) },
							 { "lod_objects", PhaseJson(Util::GpuPhase::ReflLODObjects) },
							 { "lod_trees", PhaseJson(Util::GpuPhase::ReflLODTrees) },
							 { "sky", PhaseJson(Util::GpuPhase::ReflSky) },
							 { "other", PhaseJson(Util::GpuPhase::Reflections) },
						 } },
		};

		const auto depth = SumPhases(kDepthPhases);
		j["depth_prepass"] = {
			{ "gpu_ms", depth.ms },
			{ "draws", depth.draws },
			{ "by_type", {
							 { "terrain", PhaseJson(Util::GpuPhase::DepthTerrain) },
							 { "objects", PhaseJson(Util::GpuPhase::DepthObjects) },
							 { "characters", PhaseJson(Util::GpuPhase::DepthCharacters) },
							 { "trees", PhaseJson(Util::GpuPhase::DepthTrees) },
							 { "grass", PhaseJson(Util::GpuPhase::DepthGrass) },
							 { "lod_terrain", PhaseJson(Util::GpuPhase::DepthLODLand) },
							 { "lod_objects", PhaseJson(Util::GpuPhase::DepthLODObjects) },
							 { "lod_trees", PhaseJson(Util::GpuPhase::DepthLODTrees) },
							 { "other", PhaseJson(Util::GpuPhase::DepthPrepass) },
						 } },
			{ "opaque_grass", PhaseJson(Util::GpuPhase::OpaqueGrass) },
			{ "opaque_distant", PhaseJson(Util::GpuPhase::OpaqueDistant) },
		};

		json samplers = json::object();
		{
			std::shared_lock lock(g_samplerMutex);
			for (size_t i = 0; i < g_samplerRequests.size(); ++i)
				if (g_samplerRequests[i])
					samplers[std::format("{}{}", i, i == 16 ? "+" : "")] = g_samplerRequests[i];
			j["anisotropy"] = { { "samplers_created_by_requested_max", samplers }, { "samplers_with_16x_twin", g_samplerTwins.size() }, { "applied", g_anisoApplied } };
		}
		return j;
	}
}

