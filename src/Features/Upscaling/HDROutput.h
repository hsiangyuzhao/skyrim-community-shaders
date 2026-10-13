#pragma once

#include <nlohmann/json.hpp>

struct ID3D12Resource;
class WrappedResource;

/**
 * @brief (batch 43) Native HDR10 output for the D3D12 / DLSS-G presentation path.
 *
 * (44a) Settings belong to the standalone "HDR Output" feature (category Display,
 * HDROutputFeature). The swap chain is 10-bit by default whenever Windows reports an
 * HDR-capable display ("Display format: Auto"), so HDR switches on and off live; with HDR off
 * the frame takes the lean path (no redirect, no resolve, no composite). "8-bit (legacy)" keeps
 * the game's own format and makes HDR need a restart, as in 43b. The tone curve belongs to Post
 * Processing > Color Grading (per tone mapper "HDR mode"); in HDR it outputs BT.2020.
 *
 * Pipeline (HDR on, gameplay):
 *  1. Main_PostProcessing: the game's frame-buffer slot is pointed at an fp16 texture (hdrScene) for
 *     the image-space chain, and the post-processing LDR targets were upgraded to fp16 at startup, so
 *     ISHDR's output keeps values above 1.0. Color Grading extends highlights (its LUT carries the
 *     HDR factor in alpha). ISHDR is unchanged: it still writes display gamma.
 *  2. UI start (MenuManager draw-interface hook): the scene is encoded to HDR10 (PQ, BT.2020) -- the
 *     DLSS-G HUD-less image; the game's UI is drawn into its own 8-bit layer.
 *  3. Present: ImGui joins the UI layer; then, at the bottom of the Present chain, one compute pass
 *     puts the UI layer and anything Present-hook mods drew into the back buffer over the scene at
 *     the HUD brightness and writes HDR10 into a separate output texture, which the D3D12 side copies
 *     to the swap chain.
 * Main menu, loading screens and paused menus are shown whole as SDR at paper white.
 */
namespace HDROutput
{
	struct Settings
	{
		/// Runtime switch. With "Display format: Auto" the chain is already 10-bit on an HDR-capable
		/// display, so this switches live; with "8-bit (legacy)" turning it on takes a restart.
		bool enabled = false;
		/// (44a, restart) 0 = Auto: 10-bit swap chain when Windows reports an HDR-capable display;
		/// 1 = 8-bit (legacy): the game's own format, 10-bit only when HDR is saved on (43b).
		uint displayFormat = 0;
		/// (44a) HDR off in a 10-bit session: 0 = lean (the game draws straight into the back buffer,
		/// nothing of ours runs), 1 = as 43b (fp16 scene target + SDR resolve).
		uint sdrPath = 0;
		/// 0 = auto (the display's reported maximum, 400-1000 nits, 450 if it reports nothing), 1 = manual.
		uint peakMode = 0;
		float peakNitsManual = 450.0f;
		/// Brightness of SDR white / the game's "white" (BT.2408 reference).
		float paperWhiteNits = 203.0f;
		/// HUD and menu text brightness during gameplay (43b).
		float hudNits = 160.0f;
		// (44f) The 43b "Fix Mod UI Transparency" (uiBlendFix) was removed: no visible effect for the
		// user and its counter kept rising. The old key is ignored.
		/// (43b) Paused menus: 0 = whole frame shown as SDR (safe), 1 = UI on its own layer like gameplay.
		uint menuMode = 0;
		/// (43c) NIS sharpening while HDR is on: 0 = off, 1 = linear mode.
		uint nisMode = 0;
		/// (43c, restart) Which swap chain gets SetColorSpace1: 0 = Streamline proxy (falls back to the
		/// native one if it refuses), 1 = native chain below Streamline, 2 = both.
		uint colorSpaceChain = 0;
		/// (44d) How the end of the image-space chain is judged. 0 = by the chain's own target: when the
		/// game drew the chain into the frame buffer (whose slot points at the fp16 scene), the scene is
		/// there even if a last helper pass left another target bound (the depth-of-field effect clears the
		/// underwater mask after it at the water surface). 1 = as 44c: only the target bound at the end
		/// counts (the water-surface black screen).
		uint chainEndMode = 0;
		/// (44d) Watch CopyResource / CopySubresourceRegion for copies between an fp16-upgraded target (or
		/// the 10-bit back buffer) and a target of another format, which D3D11 silently drops. Log only.
		bool copyCheck = true;
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		Settings,
		enabled,
		displayFormat,
		sdrPath,
		peakMode,
		peakNitsManual,
		paperWhiteNits,
		hudNits,
		menuMode,
		nisMode,
		colorSpaceChain,
		chainEndMode,
		copyCheck)

	/// (44a) The settings, owned by the "HDR Output" feature (HDROutputFeature) and stored here.
	Settings& GetSettings();

	/// (44a) Settings migration from 43b (Upscaling > hdrOutput). Upscaling::LoadSettings hands over
	/// its raw JSON; the HDR Output feature takes it when it has no settings of its own, and Color
	/// Grading takes the old tone-curve choice (toneCurve, rolloffStart) when its own JSON has none.
	void StashLegacyUpscalingJson(const nlohmann::json& a_upscalingJson);
	bool TakeLegacySettings(Settings& o_settings);
	bool TakeLegacyCurve(uint& o_toneCurve, float& o_rolloffStart);
	void ClearLegacy();

	// ---- startup (device / swap-chain creation, render thread) -----------------------------------

	/// Whether the D3D12 swap chain being created should be R10G10B10A2 (DLSS-G path): "Auto" and
	/// an HDR-capable display (a_window's monitor), or "8-bit (legacy)" with HDR saved on.
	bool WantsTenBitSwapChain(HWND a_window);
	/// (44a) The 10-bit chain could not be used (format not displayable, creation failed, DLSS-G
	/// refused it): remembered on disk, the next start uses 8-bit until "Retry 10-bit".
	void NoteTenBitFailure(const char* a_reason);
	/// After CreateSwapChainForHwnd. Records the chain, sets the initial colour space.
	void OnSwapChainCreated();
	/// True for the whole session when the swap chain is 10-bit (HDR was on at startup).
	bool IsSession();

	/// Upscaling::SetupResources, after the shared D3D12 resources exist.
	void SetupResources();
	void ClearShaderCache();

	// ---- per frame ---------------------------------------------------------------------------------

	/// Main_PostProcessing, around the game's image-space chain. a_target is the chain's own final
	/// target (RE::RENDER_TARGET; kFRAMEBUFFER in gameplay).
	void BeginPostProcessing();
	void EndPostProcessing(int a_target);

	/// (44a) What Color Grading needs for this frame: whether the scene is produced for HDR, and the
	/// levels. hdr = false: grade as SDR.
	struct SceneLevels
	{
		bool hdr = false;
		float peakRel = 1.0f;         ///< peak / paper white
		float peakNits = 0.0f;
		float paperWhiteNits = 0.0f;
		bool displayGamma = false;    ///< the chain's output is display gamma (Linear Lighting gamma correction off)
	};
	SceneLevels GetSceneLevels();
	/// (44a) Peak and paper white as they would be used now (read-only display on the Tone Mapping page).
	SceneLevels GetDisplayLevels();
	/// (44a) Color Grading wrote this frame's scene in BT.2020 (HDR variant): the output passes then
	/// leave the scene's primaries alone and convert only the UI / overlays.
	void NoteSceneBT2020();
	/// True when this frame's scene is being produced for HDR (DLSS 5, NIS decide from it).
	bool IsSceneHDRThisFrame();
	/// True while HDR10 is what the swap chain shows (colour space applied).
	bool IsOutputActive();

	/// DX12SwapChain::SetUIBuffer, before its DLSS-G HUD-less capture.
	void OnUIStartBeforeHUDLess();
	/// DX12SwapChain::SetUIBuffer, after its DLSS-G HUD-less capture (or where it would be).
	void OnUIStartAfterHUDLess();

	/// (43c) DLSS-G's HUD-less capture: with HDR output on, write the HDR10-encoded scene (the same
	/// encoding FinalColor has) into it. False = not handled, the caller copies the back buffer.
	bool WriteHUDLess(WrappedResource* a_hudless);

	/// (43c) Whether this frame's scene will be produced for HDR; usable before Main_PostProcessing
	/// has started (DLSS 5's placement decision).
	bool WillSceneBeHDR();

	/// (43c) NIS in an HDR session. The image is the fp16 scene target: copy it into the sharpener's
	/// input (display gamma for SDR frames; display-linear for HDR frames in "Linear" mode). False =
	/// do not sharpen this frame. a_linear tells the caller which NIS HDR mode to use.
	bool PrepareNISInput(ID3D11Resource* a_bound, ID3D11UnorderedAccessView* a_nisInput, bool& a_linear);
	/// (43c) The sharpener's output back into the fp16 scene target.
	void FinishNIS(ID3D11ShaderResourceView* a_nisOutput, bool a_linear);

	/// (43c) DLSS-G status bits from a failed state query, for the HDR status lines.
	void NoteDLSSGStatus(uint32_t a_status);

	/// The Present hook, around menu->DrawOverlay(): ImGui goes to the UI layer when there is one.
	void BeginImGui();
	void EndImGui();

	/// Bottom of the Present chain (DX12SwapChain::Present, before the D3D11 -> D3D12 fence).
	/// Returns the D3D12 resource to copy to the swap chain, or nullptr for the normal back buffer.
	ID3D12Resource* CompositeForPresent();
	/// After the real Present: per-frame reset, colour-space changes for the next frame.
	void AfterPresent();

	// ---- UI ------------------------------------------------------------------------------------------

	/// The HDR Output feature page (settings, status, Display / HDR info).
	void DrawSettings();
	/// Read-only status lines.
	void DrawStatus();
	nlohmann::json GetMetaJson();
	/// A/B Compare: restart note for a flattened "HDR Output" key, empty if none.
	std::string RestartNote(const std::string& a_key);
}
