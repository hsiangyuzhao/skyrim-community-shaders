#pragma once

#include <cstdint>
#include <d3d11.h>


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
 *  2. Depth prepass breakdown (diagnostics only; the slimming switches were removed in 44f).
 *  3. Temporal LOD dither (the fade-in/out screen door re-thresholded every frame).
 *  4. Texture clarity: DLSS mip bias on the remaining material textures, optional 16x AF.
 *
 * Only three options are left (Water > Water Effects > Reflection Cubemap), saved under
 * Advanced."Engine". Everything else is permanent since 44f (specs/ui-cleanup-44f.md): one
 * cubemap face per frame, no LOD trees in the reflection, temporal LOD dither, DLSS mip bias on
 * every material texture, 16x anisotropy. The breakdowns (overlay rows, F12 JSON) are diagnostics.
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
		// Water reflection cubemap. LOD objects stay in by default (they visibly carry distant
		// mountains and buildings), the hand-off stays off (it lags).
		bool ReflSkipLODObjects = false;            ///< bReflectLODObjects = 0
		bool ReflHandOffToDynamicCubemaps = false;  ///< no engine cubemap; water uses Dynamic Cubemaps at every distance
		float ReflHandOffCaptureWeight = 0.75f;     ///< share of a new capture while handed off (Dynamic Cubemaps: 0.5)
	};

	inline Settings settings{};

	void Load(const json& a_json);
	json Save();

	// ---- effective state -------------------------------------------------------------------
	bool ReflHandOffActive();
	/// @brief (39b) Dynamic Cubemaps' reflection capture runs every frame while it feeds water.
	bool ReflHandOffFastCaptureActive();
	/// @brief (39b) Weight of a new Dynamic Cubemaps capture (0.5 unless handed off).
	float DynamicCubemapCaptureWeight();
	bool ReflSkipLODObjectsActive();

	/// @brief SharedData::Batch39Flags (bit layout mirrored in Common/SharedData.hlsli).
	uint32_t ShaderFlags();
	namespace ShaderFlag
	{
		inline constexpr uint32_t TemporalLODDither = 1u << 0;
		inline constexpr uint32_t MipBiasMaterials = 1u << 2;
		inline constexpr uint32_t MipBiasSpecular = 1u << 3;
		inline constexpr uint32_t WaterDynamicCubemapOnly = 1u << 4;
	}

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

	/// @brief (47p, A) "Sun shadows: small far objects cast none". GpuPhaseTimeline's RenderShadowmap bracket: the sun cascade
	/// being drawn (its shadowmapIndex, 0 nearest), -1 outside the sun cascades. In cascade 1 and beyond, RenderPassImmediately
	/// leaves out a pass whose geometry's bound radius < kSmallCasterSlope x its distance to the camera (characters always cast).
	void SetSunCascade(int a_index);
	inline constexpr float kSmallCasterSlope = 0.03f;
	/// Passes left out by that rule during the last completed frame.
	uint32_t SmallCastersCulledLastFrame();

	// ---- 1. reflection cubemap -------------------------------------------------------------
	/// @brief Decides which faces this RenderCubemap call draws. Returns the face mask to pass on.
	int FilterCubemapFaces(RE::NiAVObject* a_camera, int a_faceMask, bool a_clearScenesAfter);
	/// @brief Bookkeeping after the call (faces actually drawn).
	void NoteCubemapCall(int a_requestedMask, int a_drawnMask);

	// ---- 4. anisotropic filtering ----------------------------------------------------------
	/// @brief Called by the CreateSamplerState hook after the (8x-clamped) sampler was created.
	using CreateSamplerFn = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, D3D11_SAMPLER_DESC*, ID3D11SamplerState**);
	void OnSamplerCreated(ID3D11Device* a_device, const D3D11_SAMPLER_DESC& a_requested, ID3D11SamplerState* a_clamped, CreateSamplerFn a_create);

	// ---- diagnostics -----------------------------------------------------------------------
	/// @brief "engine" section of the F12 frame JSON.
	json DiagnosticsJson();

	/// @brief Water > Water Effects > Reflection Cubemap: the options that stayed in the menu.
	void DrawWaterReflectionSettings();
}
