#pragma once

#include <cstdint>
#include <d3d11.h>

#include "Utils/Batch39.h"

namespace RE
{
	class BSRenderPass;
	class BSShader;
	class NiAVObject;
}

/**
 * @brief Batch 39, items 1-4: engine-level changes that belong to no single feature.
 *
 *  1. Water reflection cubemap relief (BSCubeMapCamera::RenderCubemap, Deferred.cpp).
 *  2. Depth prepass breakdown and slimming switches.
 *  3. Temporal LOD dither (the fade-in/out screen door re-thresholded every frame).
 *  4. Texture clarity: DLSS mip bias on the remaining material textures, optional 16x AF.
 *
 * Every switch is ANDed with Batch39::IsOn() (see the *Active() helpers); with the master off
 * every path is the 38b one. The breakdowns (overlay rows, F12 JSON) are diagnostics and run
 * regardless. Settings are saved under Advanced."Batch 39 Engine".
 *
 * What the engine does (SkyrimSE.exe 1.5.97, read off the binary for batch 39):
 * - TESWaterReflections::Update (ID 31373) renders the water cubemap only through
 *   BSCubeMapCamera::RenderCubemap (vfunc 0x35, ID 100778). Its second argument is a BIT MASK
 *   of the faces to draw (bit i = face i); the fourth clears the camera's scene list after
 *   the call. With the map menu closed the engine draws 2 faces per call: sides[k] and
 *   sides[5-k] for k = 0,1,2 on three consecutive frames (sides sorted by measured cost
 *   after every full turn), so a whole cube every 3 frames. With the map menu open it draws
 *   all 6 in one frame. Between full turns it waits fCubeMapRefreshRate:Water SECONDS (the
 *   timer advances by the frame time, timerValues[1]); the default is 0.0 = never waits.
 * - The scene list holds only: LOD terrain (bReflectLODLand), LOD objects
 *   (bReflectLODObjects), LOD trees (bReflectLODTrees) and the sky (bReflectSky). All four
 *   are read every update, so changing them in memory takes effect on the next frame.
 *   They live in Skyrim.ini's collection (INISettingCollection), which the game never
 *   writes back, so in-memory changes do not leak into the user's ini.
 * - bLodZPrepass:Display (default 1) is read every frame while the depth passes are built:
 *   0 keeps LOD terrain (kLODLandscape) out of the depth prepass.
 * - bEnableFrontToBackPrepass:Display (default 0) is read every frame at the end of
 *   Main::RenderShadowMaps: 1 sorts two accumulator groups front to back before the prepass.
 * - bDisableZPrepassOutput:Display changes how the depth target is bound; not exposed.
 * - bEnableStippleFade:Display (default 1) adds the 4x4 screen-door flag to the depth pass of
 *   a fading object; item 3 only changes the threshold pattern, not when it is used.
 */
namespace Batch39Engine
{
	struct Settings
	{
		// 1. Water reflection cubemap. All off by default: this package measures first.
		bool ReflThrottle = false;                  ///< draw ReflFacesPerUpdate faces every ReflEveryNFrames frames
		int ReflFacesPerUpdate = 1;                 ///< 1, 2, 3 or 6
		int ReflEveryNFrames = 1;                   ///< 1..8
		bool ReflSkipLODTrees = false;              ///< bReflectLODTrees = 0
		bool ReflSkipLODObjects = false;            ///< bReflectLODObjects = 0
		bool ReflHandOffToDynamicCubemaps = false;  ///< no engine cubemap; water uses Dynamic Cubemaps at every distance

		// 2. Depth prepass slimming. All off by default.
		bool PrepassSkipGrass = false;    ///< grass skips the prepass; alpha-tested + depth-writing in the main pass
		bool PrepassSkipLODLand = false;  ///< bLodZPrepass = 0
		bool PrepassFrontToBack = false;  ///< bEnableFrontToBackPrepass = 1

		// 3. Temporal LOD dither.
		bool TemporalLODDither = true;

		// 4. Texture clarity.
		bool MipBiasMaterials = true;  ///< DLSS mip bias on the remaining material textures
		bool MipBiasSpecular = true;   ///< ... and on specular / gloss / environment-mask textures
		bool Anisotropic16x = true;    ///< stop clamping requested 16x anisotropy to 8x
	};

	inline Settings settings{};

	void Load(const json& a_json);
	json Save();

	// ---- effective state (setting && master && prerequisites) -----------------------------
	bool ReflThrottleActive();
	bool ReflHandOffActive();
	bool ReflSkipLODTreesActive();
	bool ReflSkipLODObjectsActive();
	bool PrepassSkipGrassActive();
	bool PrepassSkipLODLandActive();
	bool PrepassFrontToBackActive();
	bool TemporalLODDitherActive();
	bool MipBiasMaterialsActive();
	bool MipBiasSpecularActive();
	bool Anisotropic16xActive();
	/// @brief Any switch that leaves geometry out of the prepass (needs the after-opaque depth refresh).
	bool PrepassSlimmingActive();

	/// @brief SharedData::Batch39Flags (bit layout mirrored in Common/SharedData.hlsli).
	uint32_t ShaderFlags();
	namespace ShaderFlag
	{
		inline constexpr uint32_t TemporalLODDither = 1u << 0;
		inline constexpr uint32_t MipBiasMaterials = 1u << 2;
		inline constexpr uint32_t MipBiasSpecular = 1u << 3;
		inline constexpr uint32_t WaterDynamicCubemapOnly = 1u << 4;
	}

	/// @brief Grass pixel-descriptor bit (CS only, never set by the engine): selects the
	/// GRASS_MAIN_ALPHA_TEST permutation, which alpha-tests in the main pass.
	inline constexpr uint32_t kGrassMainAlphaTestFlag = 0x20000000;

	// ---- engine hooks -----------------------------------------------------------------------
	/// @brief Installs the always-on hooks (render pass entry, depth prepass bracket, PSSetSamplers).
	void InstallHooks();
	/// @brief Installs the context hook (called once the D3D11 context exists).
	void InstallContextHooks(ID3D11DeviceContext* a_context);

	/// @brief Frame boundary (State::Reset): engine setting overrides, diagnostics roll-over.
	void OnFrameStart();

	/// @brief The BSRenderPass being submitted right now (nullptr outside a pass).
	RE::BSRenderPass* CurrentPass();
	/// @brief True inside Main_RenderDepth (the engine's depth prepass).
	bool InDepthPrepass();

	// ---- 1. reflection cubemap -------------------------------------------------------------
	/// @brief Decides which faces this RenderCubemap call draws. Returns the face mask to pass on.
	int FilterCubemapFaces(RE::NiAVObject* a_camera, int a_faceMask, bool a_clearScenesAfter);
	/// @brief Bookkeeping after the call (faces actually drawn).
	void NoteCubemapCall(int a_requestedMask, int a_drawnMask);

	// ---- 2. depth prepass ------------------------------------------------------------------
	/// @brief Right after the opaque pass: brings the post-prepass depth copies up to date when
	/// a slimming switch kept geometry out of the prepass.
	void AfterOpaquePass();
	/// @brief Per draw (State::Draw), after the engine applied its state: depth writes for
	/// geometry that skipped the prepass.
	void OnDraw(RE::BSShader* a_shader, uint32_t a_vertexDescriptor, uint32_t a_pixelDescriptor);

	// ---- 4. anisotropic filtering ----------------------------------------------------------
	/// @brief Called by the CreateSamplerState hook after the (8x-clamped) sampler was created.
	using CreateSamplerFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
	void OnSamplerCreated(ID3D11Device* a_device, const D3D11_SAMPLER_DESC& a_requested, ID3D11SamplerState* a_clamped, CreateSamplerFn a_create);

	// ---- diagnostics -----------------------------------------------------------------------
	/// @brief Batch 39 section of the F12 frame JSON.
	json DiagnosticsJson();

	/// @brief Releases resources (shader cache clear / device reset).
	void ClearShaderCache();
}
