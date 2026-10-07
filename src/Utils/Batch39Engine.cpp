// Batch 39, items 1-4: water reflection cubemap relief, depth prepass breakdown + slimming,
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
						logger::warn("[Batch 39] engine setting {} not found", name);
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

		bool IsGrassPass(const RE::BSRenderPass* a_pass)
		{
			return a_pass && a_pass->shader && a_pass->shader->shaderType.get() == RE::BSShader::Type::Grass;
		}

		struct BSBatchRenderer_RenderPassImmediately
		{
			static void thunk(RE::BSRenderPass* a_pass, uint32_t a_technique, bool a_alphaTest, uint32_t a_renderFlags)
			{
				// 2. Grass skips the depth prepass: nothing is submitted for it there. In the
				// main pass it is drawn with the alpha-test reference set (the engine only sets
				// it for alpha-tested submissions) so the GRASS_MAIN_ALPHA_TEST shader cuts the
				// blades out exactly as the prepass would have.
				if (IsGrassPass(a_pass) && PrepassSkipGrassActive()) {
					if (g_inDepthPrepass)
						return;
					if (globals::deferred && globals::deferred->deferredPass)
						a_alphaTest = true;
				}

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

		// ---------------------------------------------------------------------------------
		// 2. Depth refresh after the opaque pass.
		// ---------------------------------------------------------------------------------
		ID3D11ComputeShader* g_depthRefreshCS = nullptr;
		bool g_depthRefreshCSFailed = false;
		uint32_t g_depthRefreshFrame = UINT32_MAX;
		std::string g_depthRefreshPath = "-";

		ID3D11ComputeShader* GetDepthRefreshCS()
		{
			if (!g_depthRefreshCS && !g_depthRefreshCSFailed) {
				g_depthRefreshCS = static_cast<ID3D11ComputeShader*>(Util::CompileShader(L"Data\\Shaders\\Batch39DepthRefreshCS.hlsl", {}, "cs_5_0"));
				if (!g_depthRefreshCS) {
					g_depthRefreshCSFailed = true;
					logger::error("[Batch 39] Batch39DepthRefreshCS.hlsl failed to compile; Terrain Blending's depth is not refreshed after the opaque pass");
				}
			}
			return g_depthRefreshCS;
		}

		// Depth states for draws that skipped the prepass: the engine's state with the test
		// relaxed to LESS_EQUAL and depth writes on (stencil kept).
		std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> g_depthWriteStates;
		uint32_t g_depthOverridesThisFrame = 0, g_depthOverridesLast = 0;

		ID3D11DepthStencilState* DepthWriteVariant(ID3D11DepthStencilState* a_state)
		{
			if (auto it = g_depthWriteStates.find(a_state); it != g_depthWriteStates.end())
				return it->second.get();

			winrt::com_ptr<ID3D11DepthStencilState> variant;
			D3D11_DEPTH_STENCIL_DESC desc{};
			a_state->GetDesc(&desc);
			const bool alreadyWrites = desc.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL &&
			                           (desc.DepthFunc == D3D11_COMPARISON_LESS_EQUAL || desc.DepthFunc == D3D11_COMPARISON_LESS);
			if (desc.DepthEnable && !alreadyWrites) {
				desc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
				desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
				if (FAILED(globals::d3d::device->CreateDepthStencilState(&desc, variant.put())))
					variant = nullptr;
			}
			// Keyed by the engine's own (persistent) state object; a null value means "leave as is".
			auto* raw = variant.get();
			g_depthWriteStates.emplace(a_state, std::move(variant));
			return raw;
		}

		// Same state with the depth test failing everywhere: the draw writes nothing.
		std::unordered_map<ID3D11DepthStencilState*, winrt::com_ptr<ID3D11DepthStencilState>> g_hiddenStates;
		uint32_t g_hiddenDrawsThisFrame = 0, g_hiddenDrawsLast = 0;

		ID3D11DepthStencilState* HiddenVariant(ID3D11DepthStencilState* a_state)
		{
			if (auto it = g_hiddenStates.find(a_state); it != g_hiddenStates.end())
				return it->second.get();
			winrt::com_ptr<ID3D11DepthStencilState> variant;
			D3D11_DEPTH_STENCIL_DESC desc{};
			a_state->GetDesc(&desc);
			desc.DepthEnable = TRUE;
			desc.DepthFunc = D3D11_COMPARISON_NEVER;
			desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
			desc.StencilEnable = FALSE;
			if (FAILED(globals::d3d::device->CreateDepthStencilState(&desc, variant.put())))
				variant = nullptr;
			auto* raw = variant.get();
			g_hiddenStates.emplace(a_state, std::move(variant));
			return raw;
		}

		static_assert(kGrassMainAlphaTestFlag == static_cast<uint32_t>(SIE::ShaderCache::GrassShaderFlags::MainAlphaTest));
	}

	// =====================================================================================
	// Settings
	// =====================================================================================

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		Settings,
		ReflThrottle,
		ReflFacesPerUpdate,
		ReflEveryNFrames,
		ReflSkipLODTrees,
		ReflSkipLODObjects,
		ReflHandOffToDynamicCubemaps,
		PrepassSkipGrass,
		PrepassSkipLODLand,
		PrepassFrontToBack,
		TemporalLODDither,
		MipBiasMaterials,
		MipBiasSpecular,
		Anisotropic16x)

	void Load(const json& a_json)
	{
		if (!a_json.is_object())
			return;
		try {
			settings = a_json.get<Settings>();
		} catch (const std::exception& e) {
			logger::warn("[Batch 39] engine settings unreadable ({}); using defaults", e.what());
			settings = {};
		}
		if (settings.ReflFacesPerUpdate != 1 && settings.ReflFacesPerUpdate != 2 && settings.ReflFacesPerUpdate != 3 && settings.ReflFacesPerUpdate != 6)
			settings.ReflFacesPerUpdate = 1;
		settings.ReflEveryNFrames = std::clamp(settings.ReflEveryNFrames, 1, 8);
	}

	json Save()
	{
		return settings;
	}

	// =====================================================================================
	// Effective state
	// =====================================================================================

	bool ReflHandOffActive()
	{
		return Batch39::IsOn() && settings.ReflHandOffToDynamicCubemaps && globals::features::dynamicCubemaps.loaded;
	}
	bool ReflThrottleActive() { return Batch39::IsOn() && settings.ReflThrottle && !ReflHandOffActive(); }
	bool ReflSkipLODTreesActive() { return Batch39::IsOn() && settings.ReflSkipLODTrees; }
	bool ReflSkipLODObjectsActive() { return Batch39::IsOn() && settings.ReflSkipLODObjects; }
	bool PrepassSkipGrassActive()
	{
		// Our grass pixel shaders must be the ones in use: the engine's own grass shader has no
		// alpha test in the main pass and would draw solid quads.
		return Batch39::IsOn() && settings.PrepassSkipGrass && g_passHookInstalled && g_depthHookInstalled &&
		       globals::shaderCache && globals::shaderCache->IsEnabled() && globals::state &&
		       globals::state->enabledClasses[RE::BSShader::Type::Grass - 1];
	}
	bool PrepassSkipLODLandActive() { return Batch39::IsOn() && settings.PrepassSkipLODLand; }
	bool PrepassFrontToBackActive() { return Batch39::IsOn() && settings.PrepassFrontToBack; }
	bool TemporalLODDitherActive() { return Batch39::IsOn() && settings.TemporalLODDither; }
	bool MipBiasMaterialsActive() { return Batch39::IsOn() && settings.MipBiasMaterials; }
	bool MipBiasSpecularActive() { return Batch39::IsOn() && settings.MipBiasSpecular; }
	bool Anisotropic16xActive() { return Batch39::IsOn() && settings.Anisotropic16x && g_samplerHookInstalled; }
	bool PrepassSlimmingActive() { return PrepassSkipGrassActive() || PrepassSkipLODLandActive(); }

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
			logger::warn("[Batch 39] RenderPassImmediately not hooked: no per-object prepass breakdown, grass cannot skip the prepass");

		// Main_RenderDepth (same function the overlay's timeline and Terrain Blending bracket).
		stl::detour_thunk<Main_RenderDepth>(REL::RelocationID(100421, 107139));
		g_depthHookInstalled = true;

		logger::info("[Batch 39] Installed engine hooks");
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
		g_reflectLODTrees.Apply(ReflSkipLODTreesActive(), false);
		g_reflectLODObjects.Apply(ReflSkipLODObjectsActive(), false);
		g_lodZPrepass.Apply(PrepassSkipLODLandActive(), false);
		g_frontToBackPrepass.Apply(PrepassFrontToBackActive(), true);

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

		g_depthOverridesLast = g_depthOverridesThisFrame;
		g_depthOverridesThisFrame = 0;
		g_hiddenDrawsLast = g_hiddenDrawsThisFrame;
		g_hiddenDrawsThisFrame = 0;
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

		// Dynamic Cubemaps takes over: the engine cubemap is never drawn. The calls still go
		// through with no faces so the camera's scene list is cleared as usual.
		if (ReflHandOffActive())
			return 0;

		if (!ReflThrottleActive() || !globals::state)
			return a_faceMask;

		// Our own round robin, independent of which faces the engine asked for: any face can be
		// drawn from this frame's scene list. The first call of a frame (per camera) draws the
		// faces; the engine's second call of the frame then only clears the scene list.
		auto& cam = CameraSlot(a_camera);
		const uint32_t frame = globals::state->frameCount;
		if (cam.lastFrame == frame)
			return 0;
		cam.lastFrame = frame;
		const uint32_t everyN = static_cast<uint32_t>(std::clamp(settings.ReflEveryNFrames, 1, 8));
		if (frame % everyN != 0)
			return 0;
		const uint32_t faces = static_cast<uint32_t>(std::clamp(settings.ReflFacesPerUpdate, 1, 6));
		int mask = 0;
		for (uint32_t i = 0; i < faces; ++i)
			mask |= 1 << ((cam.cursor + i) % 6);
		cam.cursor = (cam.cursor + faces) % 6;
		return mask;
	}

	void NoteCubemapCall(int, int a_drawnMask)
	{
		g_cubeThis.facesDrawn += std::popcount(static_cast<uint32_t>(a_drawnMask) & 0x3Fu);
	}

	// =====================================================================================
	// 2. Depth prepass
	// =====================================================================================

	void OnDraw(RE::BSShader* a_shader, uint32_t, uint32_t a_pixelDescriptor)
	{
		if (!a_shader || !globals::deferred || !globals::deferred->deferredPass)
			return;

		bool needsDepthWrite = false;
		bool hide = false;
		switch (a_shader->shaderType.get()) {
		case RE::BSShader::Type::Grass:
			needsDepthWrite = PrepassSkipGrassActive();
			// While the GRASS_MAIN_ALPHA_TEST shader is still compiling the engine's own (no alpha
			// test) shader is bound: hide the draw rather than show solid quads for a moment.
			if (needsDepthWrite)
				hide = !globals::shaderCache->GetPixelShader(*a_shader, globals::state->modifiedPixelDescriptor);
			break;
		case RE::BSShader::Type::Lighting:
			if (PrepassSkipLODLandActive()) {
				using Tech = SIE::ShaderCache::LightingShaderTechniques;
				const auto tech = static_cast<Tech>(0x3F & (a_pixelDescriptor >> 24));
				needsDepthWrite = tech == Tech::LODLand || tech == Tech::LODLandNoise;
			}
			break;
		default:
			break;
		}
		if (!needsDepthWrite)
			return;

		auto* context = globals::d3d::context;
		winrt::com_ptr<ID3D11DepthStencilState> current;
		UINT stencilRef = 0;
		context->OMGetDepthStencilState(current.put(), &stencilRef);
		if (!current)
			return;
		if (auto* variant = hide ? HiddenVariant(current.get()) : DepthWriteVariant(current.get())) {
			context->OMSetDepthStencilState(variant, stencilRef);
			// The engine re-applies its own depth state before its next draw.
			globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_DEPTH_MODE);
			++g_depthOverridesThisFrame;
			if (hide)
				++g_hiddenDrawsThisFrame;
		}
	}

	void AfterOpaquePass()
	{
		if (!PrepassSlimmingActive())
			return;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context)
			return;

		auto& mainDepth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto& zPrepassCopy = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kPOST_ZPREPASS_COPY];
		if (!mainDepth.texture || !zPrepassCopy.texture)
			return;

		auto* timers = Util::GpuPassTimers::GetSingleton();
		timers->Begin(Util::GpuBucket::Batch39DepthRefresh);

		// Everything after the opaque pass that reads "the prepass depth" (SSAO/SSGI, SSRT,
		// screen-space shadows, SSS, fog, the deferred composite, water) sees the complete
		// opaque depth again, including the geometry that skipped the prepass.
		context->CopyResource(zPrepassCopy.texture, mainDepth.texture);
		g_depthRefreshPath = "copy";

		// With Terrain Blending, those readers get its blended copy instead (made right after
		// the prepass): fold the finished depth into it. min() keeps its blended terrain.
		auto& tb = globals::features::terrainBlending;
		if (tb.loaded && tb.blendedDepthTexture && tb.blendedDepthTexture16 && tb.depthSRVBackup) {
			if (auto* cs = GetDepthRefreshCS()) {
				ID3D11ShaderResourceView* srv = tb.depthSRVBackup;
				ID3D11UnorderedAccessView* uavs[2] = { tb.blendedDepthTexture->uav.get(), tb.blendedDepthTexture16->uav.get() };
				context->CSSetShaderResources(0, 1, &srv);
				context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
				context->CSSetShader(cs, nullptr, 0);
				const auto dispatch = Util::GetScreenDispatchCount();
				context->Dispatch(dispatch.x, dispatch.y, 1);

				ID3D11ShaderResourceView* nullSrv = nullptr;
				ID3D11UnorderedAccessView* nullUavs[2] = { nullptr, nullptr };
				context->CSSetShaderResources(0, 1, &nullSrv);
				context->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
				context->CSSetShader(nullptr, nullptr, 0);

				// Writing it as a UAV unbinds it from the pixel shader slot State binds it to.
				ID3D11ShaderResourceView* blended16 = tb.blendedDepthTexture16->srv.get();
				context->PSSetShaderResources(17, 1, &blended16);
				g_depthRefreshPath = "copy + Terrain Blending merge";
			}
		}

		timers->End(Util::GpuBucket::Batch39DepthRefresh);
		g_depthRefreshFrame = globals::state ? globals::state->frameCount : 0;
	}

	void ClearShaderCache()
	{
		if (g_depthRefreshCS) {
			g_depthRefreshCS->Release();
			g_depthRefreshCS = nullptr;
		}
		g_depthRefreshCSFailed = false;
	}

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
		j["master"] = Batch39::IsOn();
		j["settings"] = settings;
		j["active"] = {
			{ "refl_throttle", ReflThrottleActive() },
			{ "refl_handoff_dynamic_cubemaps", ReflHandOffActive() },
			{ "refl_skip_lod_trees", ReflSkipLODTreesActive() },
			{ "refl_skip_lod_objects", ReflSkipLODObjectsActive() },
			{ "prepass_skip_grass", PrepassSkipGrassActive() },
			{ "prepass_skip_lod_land", PrepassSkipLODLandActive() },
			{ "prepass_front_to_back", PrepassFrontToBackActive() },
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
			{ "bReflectLODLand:Water", g_reflectLODLand.Current() },
			{ "bReflectSky:Water", g_reflectSky.Current() },
			{ "bLodZPrepass:Display", { { "now", g_lodZPrepass.Current() }, { "user", g_lodZPrepass.UserValue() } } },
			{ "bEnableFrontToBackPrepass:Display", { { "now", g_frontToBackPrepass.Current() }, { "user", g_frontToBackPrepass.UserValue() } } },
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
			{ "depth_write_overrides_last_frame", g_depthOverridesLast },
			{ "grass_draws_hidden_while_compiling_last_frame", g_hiddenDrawsLast },
			{ "depth_refresh", { { "ran_last_frame", globals::state && g_depthRefreshFrame + 1 >= globals::state->frameCount }, { "path", g_depthRefreshPath } } },
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

// =========================================================================================
// Batch 39 tab: rows and section (items 1-4)
// =========================================================================================

namespace Batch39
{
	using namespace Batch39Engine;

	namespace
	{
		Row Toggle(const char* a_group, const char* a_name, bool a_installed, bool& a_value, std::string a_nowText, const char* a_where, bool a_defaultOff = false)
		{
			std::string own = a_value ? "On" : "Off";
			if (a_defaultOff)
				own += " (default Off)";
			Row r{ a_group, a_name, a_installed, a_value, std::move(own), std::move(a_nowText), a_where };
			r.toggle = &a_value;
			return r;
		}

		Row Info(const char* a_group, const char* a_name, std::string a_text)
		{
			Row r{ a_group, a_name, true, true, "(diagnostic)", std::move(a_text), "Performance overlay > Engine passes; F12 / Save frame (JSON)" };
			r.governed = false;
			return r;
		}
	}

	void RowsEngine(std::vector<Row>& a_rows)
	{
		auto& s = Batch39Engine::settings;
		const bool dcLoaded = globals::features::dynamicCubemaps.loaded;

		// ---- 1. Water reflection cubemap ----
		{
			const char* g = "1. Water reflection cubemap";
			const char* where = "Advanced > Batch 39 (below)";
			a_rows.push_back(Info(g, "Breakdown by object type (LOD terrain / objects / trees, sky)",
				std::format("{:.1f} faces/frame, {:.1f} calls/frame", g_cubeDrawnAvg, g_cubeCallsAvg)));

			std::string now;
			if (s.ReflThrottle && ReflHandOffActive())
				now = "Idle: Dynamic Cubemaps hand-off is on";
			else if (ReflThrottleActive())
				now = std::format("On ({} face{} every {} frame{})", s.ReflFacesPerUpdate, s.ReflFacesPerUpdate == 1 ? "" : "s", s.ReflEveryNFrames, s.ReflEveryNFrames == 1 ? "" : "s");
			a_rows.push_back(Toggle(g, "Fewer face updates (round robin)", true, s.ReflThrottle, now, where, true));
			a_rows.push_back(Toggle(g, "Leave LOD trees out (bReflectLODTrees = 0)", true, s.ReflSkipLODTrees, "", where, true));
			a_rows.push_back(Toggle(g, "Leave LOD objects out (bReflectLODObjects = 0)", true, s.ReflSkipLODObjects, "", where, true));
			a_rows.push_back(Toggle(g, "Hand off to Dynamic Cubemaps (no engine cubemap)", dcLoaded, s.ReflHandOffToDynamicCubemaps, "", where, true));
		}

		// ---- 2. Depth prepass ----
		{
			const char* g = "2. Depth prepass";
			const char* where = "Advanced > Batch 39 (below)";
			const auto depth = SumPhases(kDepthPhases);
			a_rows.push_back(Info(g, "Breakdown by object type (terrain, objects, characters, trees, grass, LOD)",
				std::format("{:.2f} ms, {:.0f} draws (overlay open)", depth.ms, depth.draws)));

			std::string grassNow;
			if (s.PrepassSkipGrass && !g_passHookInstalled)
				grassNow = "Unavailable: render-pass hook missing";
			a_rows.push_back(Toggle(g, "Grass skips the prepass", g_passHookInstalled, s.PrepassSkipGrass, grassNow, where, true));
			a_rows.push_back(Toggle(g, "LOD terrain skips the prepass (bLodZPrepass = 0)", true, s.PrepassSkipLODLand, "", where, true));
			a_rows.push_back(Toggle(g, "Sort prepass front to back (bEnableFrontToBackPrepass = 1)", true, s.PrepassFrontToBack, "", where, true));
		}

		// ---- 3. Temporal LOD dither ----
		{
			const char* g = "3. Temporal LOD dither";
			std::string now;
			if (TemporalLODDitherActive() && !g_stippleFade.Current())
				now = "Idle: bEnableStippleFade is 0";
			a_rows.push_back(Toggle(g, "Fade-in/out pattern changes every frame (DLSS blends it)", true, s.TemporalLODDither, now, "Advanced > Batch 39"));
		}

		// ---- 4. Texture clarity ----
		{
			const char* g = "4. Texture clarity";
			a_rows.push_back(Toggle(g, "DLSS mip bias on more material textures", true, s.MipBiasMaterials, "", "Advanced > Batch 39"));
			a_rows.push_back(Toggle(g, "DLSS mip bias on specular / gloss / env-mask textures", true, s.MipBiasSpecular, "", "Advanced > Batch 39"));
			std::string afNow;
			bool noTwins = false;
			{
				std::shared_lock lock(g_samplerMutex);
				noTwins = g_samplerTwins.empty();
			}
			if (s.Anisotropic16x && !g_samplerHookInstalled)
				afNow = "Unavailable: sampler hook missing";
			else if (Anisotropic16xActive() && noTwins)
				afNow = "Idle: no sampler asked for more than 8x";
			a_rows.push_back(Toggle(g, "Anisotropic filtering up to 16x (was clamped to 8x)", true, s.Anisotropic16x, afNow, "Advanced > Batch 39"));
		}
	}

	void DrawEngineSection()
	{
		auto& s = Batch39Engine::settings;
		const auto& palette = Menu::GetSingleton()->GetTheme().StatusPalette;

		if (ImGui::CollapsingHeader("1. Water reflection cubemap", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::TextWrapped(
				"The game redraws a small cubemap around you for the reflections on distant water. It only holds far-away "
				"terrain, objects, trees and the sky (\"LOD\"), and normally redraws 2 of its 6 sides every frame.");
			ImGui::Checkbox("Fewer face updates", &s.ReflThrottle);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Redraw fewer sides per frame, in turn. The sides that are not redrawn keep last frame's picture.\n"
					"Moving fast, far-away water reflections can lag a few frames behind.");
			Batch39::MasterNote();
			ImGui::BeginDisabled(!s.ReflThrottle);
			static constexpr int kFaces[] = { 1, 2, 3, 6 };
			static constexpr const char* kFaceNames[] = { "1 side", "2 sides (as the game)", "3 sides", "all 6 sides" };
			int faceIndex = 0;
			for (int i = 0; i < 4; ++i)
				if (kFaces[i] == s.ReflFacesPerUpdate)
					faceIndex = i;
			ImGui::SetNextItemWidth(180.0f);
			if (ImGui::Combo("Sides per update", &faceIndex, kFaceNames, 4))
				s.ReflFacesPerUpdate = kFaces[faceIndex];
			ImGui::SetNextItemWidth(180.0f);
			ImGui::SliderInt("Update every N frames", &s.ReflEveryNFrames, 1, 8, "%d", ImGuiSliderFlags_AlwaysClamp);
			ImGui::Text("Whole cube refreshed every %d frames (the game: every 3).", (6 + s.ReflFacesPerUpdate - 1) / s.ReflFacesPerUpdate * s.ReflEveryNFrames);
			ImGui::EndDisabled();

			ImGui::Checkbox("Leave LOD trees out of the reflection", &s.ReflSkipLODTrees);
			ImGui::Checkbox("Leave LOD objects out of the reflection", &s.ReflSkipLODObjects);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("LOD objects include distant buildings and many mountain and cliff meshes.");
			ImGui::BeginDisabled(!globals::features::dynamicCubemaps.loaded);
			ImGui::Checkbox("Hand off to Dynamic Cubemaps (skip the game's cubemap)", &s.ReflHandOffToDynamicCubemaps);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"The game's cubemap is not drawn at all; distant water reflects Dynamic Cubemaps' capture instead, as near water\n"
					"already does. Saves the whole Reflections row. Distant mountains are then reflected only as well as Dynamic\n"
					"Cubemaps sees them. Water screen-space reflections are unaffected.");
			ImGui::EndDisabled();
			if (!globals::features::dynamicCubemaps.loaded)
				ImGui::TextDisabled("(needs Dynamic Cubemaps)");

			ImGui::Spacing();
			const auto refl = SumPhases(kReflPhases);
			ImGui::Text("Now: %.2f calls, %.2f sides requested, %.2f sides drawn per frame%s", g_cubeCallsAvg, g_cubeRequestedAvg, g_cubeDrawnAvg, g_cubeLast.mapMenu ? " (map open: all 6)" : "");
			if (refl.ms > 0.0f || refl.draws > 0.0f)
				ImGui::Text("GPU %.2f ms, %.0f draws (%.0f per side)", refl.ms, refl.draws, g_cubeDrawnAvg > 0.01f ? refl.draws / g_cubeDrawnAvg : 0.0f);
			else
				ImGui::TextDisabled("GPU time and draws: open the performance overlay with its draw-call table.");
			ImGui::TextDisabled("fCubeMapRefreshRate = %.2f s (pause between full turns; 0 = never pause). bReflectLODLand %d, bReflectSky %d.",
				ReadEngineFloat("fCubeMapRefreshRate:Water", -1.0f), g_reflectLODLand.Current() ? 1 : 0, g_reflectSky.Current() ? 1 : 0);
		}

		if (ImGui::CollapsingHeader("2. Depth prepass", ImGuiTreeNodeFlags_DefaultOpen)) {
			ImGui::TextWrapped(
				"Before drawing the scene properly, the game draws it once as depth only, so hidden pixels are skipped later. "
				"The overlay's \"Depth prepass\" group now shows what that costs per object type.");
			ImGui::BeginDisabled(!g_passHookInstalled);
			ImGui::Checkbox("Grass skips the prepass", &s.PrepassSkipGrass);
			ImGui::EndDisabled();
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Grass is no longer drawn into the depth prepass; in the main pass it cuts its blades out itself and writes depth.\n"
					"Saves the prepass grass row, but every hidden blade is then fully shaded in the main pass: compare the\n"
					"\"Depth prepass\" and \"Opaque geometry > Grass\" rows together.\n"
					"Kept correct: SSAO/SSGI, SSRT, fog, water and the composite get the full depth (refreshed after the main pass;\n"
					"see the \"Depth refresh (39)\" GPU row). Grass Optimizations' occlusion culling builds from the depth when\n"
					"grass is first drawn, which then already has the whole prepass in it.\n"
					"Not kept: contact shadows (Screen-Space Shadows) computed while the main pass runs do not see grass.\n"
					"The first time it is switched on, grass may vanish for a moment while its shader compiles.");
			ImGui::Checkbox("LOD terrain skips the prepass (bLodZPrepass = 0)", &s.PrepassSkipLODLand);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"The game's own switch. Far-away terrain then writes its depth in the main pass (forced here, so it cannot vanish),\n"
					"and the same depth refresh keeps fog and the screen-space effects correct.");
			ImGui::Checkbox("Sort the prepass front to back (bEnableFrontToBackPrepass = 1)", &s.PrepassFrontToBack);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"The game's own switch, off by default. Sorts the prepass so near objects hide far ones sooner.\n"
					"Costs a little CPU for the sort; changes no picture.");
			Batch39::MasterNote();

			const bool slim = PrepassSlimmingActive();
			ImGui::TextColored(slim ? palette.SuccessColor : palette.Disable, "Depth refresh after the main pass: %s%s",
				slim ? "runs" : "not needed", slim ? std::format(" ({})", g_depthRefreshPath).c_str() : "");
			ImGui::TextDisabled("Engine values now: bLodZPrepass %d (yours %d), bEnableFrontToBackPrepass %d (yours %d), depth-write overrides last frame: %u",
				g_lodZPrepass.Current() ? 1 : 0, g_lodZPrepass.UserValue() ? 1 : 0, g_frontToBackPrepass.Current() ? 1 : 0, g_frontToBackPrepass.UserValue() ? 1 : 0, g_depthOverridesLast);
		}

		if (ImGui::CollapsingHeader("3-4. LOD dither, texture clarity")) {
			ImGui::Checkbox("Temporal LOD dither", &s.TemporalLODDither);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Objects fading in or out at the edge of the view distance used a fixed 4x4 screen-door pattern.\n"
					"It now changes every frame (interleaved gradient noise, golden-ratio step), and DLSS blends it into a\n"
					"smooth see-through fade. The depth prepass and the main pass use the same pattern, so no holes.\n"
					"Takes effect immediately. Off = the old fixed pattern.");
			ImGui::Checkbox("DLSS mip bias on more material textures", &s.MipBiasMaterials);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"DLSS renders at a lower resolution, so textures get a sharper mip level (negative bias) to look native.\n"
					"Base colour, normal and RMAOS already had it. Now also: the snow/moss layer on rocks and mountains\n"
					"(directional projection), glow maps, detail/tint, skin and hair extras, back/rim light, multi-layer\n"
					"parallax and the PBR coat/fuzz/subsurface maps.");
			ImGui::Checkbox("DLSS mip bias on specular / gloss / env-mask textures", &s.MipBiasSpecular);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted("Sharper shine maps. Separate because it can add a little sparkle on distant shiny surfaces.");
			ImGui::Checkbox("Anisotropic filtering up to 16x", &s.Anisotropic16x);
			if (auto _tt = Util::HoverTooltipWrapper())
				ImGui::TextUnformatted(
					"Community Shaders clamped every game sampler to 8x. With this on, samplers the game created with 16x get 16x.\n"
					"Only changes ground seen at a very flat angle (beyond 8:1 stretch); on an RTX 4070 Ti Super the cost is\n"
					"typically well under 0.1 ms at 1440p.");
			{
				std::shared_lock lock(g_samplerMutex);
				ImGui::TextDisabled("Samplers that asked for more than 8x: %zu", g_samplerTwins.size());
			}
		}
	}
}
