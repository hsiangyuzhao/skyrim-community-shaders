#pragma once

#include <nlohmann/json.hpp>

struct ID3D12Resource;
class WrappedResource;

/**
 * @brief (batch 43) Native HDR10 output for the D3D12 / DLSS-G presentation path.
 *
 * Settings live in Upscaling::Settings::hdrOutput (Upscaling > HDR Output). Default off; with it
 * off at startup nothing here runs and the swap chain keeps the game's own 8-bit format.
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
		/// Runtime switch. The 10-bit swap chain it needs is made at startup only when this is on,
		/// so turning it on for the first time takes a restart.
		bool enabled = false;
		/// 0 = auto (the display's reported maximum, 400-1000 nits, 450 if it reports nothing), 1 = manual.
		uint peakMode = 0;
		float peakNitsManual = 450.0f;
		/// Brightness of SDR white / the game's "white" (BT.2408 reference).
		float paperWhiteNits = 203.0f;
		/// HUD and menu text brightness during gameplay (43b).
		float hudNits = 160.0f;
		/// Where highlight compression starts. Extend mode: fraction of SDR white kept exactly as in
		/// SDR; GT7 mode: fraction of the peak where GT7's linear section ends.
		float rolloffStart = 0.6f;
		/// 1 = extend the selected tone mapper (keeps the SDR look), 2 = GT7 HDR curve.
		uint toneCurve = 1;
		/// (43b) Rewrite third-party UI alpha blending so their panels cover the scene in the HUD layer.
		bool uiBlendFix = true;
		/// (43b) Paused menus: 0 = whole frame shown as SDR (safe), 1 = UI on its own layer like gameplay.
		uint menuMode = 0;
		/// (43c) NIS sharpening while HDR is on: 0 = off, 1 = linear mode.
		uint nisMode = 0;
		/// (43c, restart) Which swap chain gets SetColorSpace1: 0 = Streamline proxy (falls back to the
		/// native one if it refuses), 1 = native chain below Streamline, 2 = both.
		uint colorSpaceChain = 0;
	};

	NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE_WITH_DEFAULT(
		Settings,
		enabled,
		peakMode,
		peakNitsManual,
		paperWhiteNits,
		hudNits,
		rolloffStart,
		toneCurve,
		uiBlendFix,
		menuMode,
		nisMode,
		colorSpaceChain)

	// ---- startup (device / swap-chain creation, render thread) -----------------------------------

	/// Whether the D3D12 swap chain being created should be R10G10B10A2: HDR saved on, DLSS-G path.
	bool WantsTenBitSwapChain();
	/// After CreateSwapChainForHwnd. Records the chain, sets the initial colour space.
	void OnSwapChainCreated();
	/// True for the whole session when the swap chain is 10-bit (HDR was on at startup).
	bool IsSession();

	/// Upscaling::SetupResources, after the shared D3D12 resources exist.
	void SetupResources();
	void ClearShaderCache();

	// ---- per frame ---------------------------------------------------------------------------------

	/// Main_PostProcessing, around the game's image-space chain.
	void BeginPostProcessing();
	void EndPostProcessing();

	/// Color Grading's hdrParams for this frame's scene: zero unless HDR output tone-maps it.
	float4 GetColorGradingParams();
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

	void DrawSettings();
	/// Read-only status for Upscaling > Display / HDR info.
	void DrawStatus();
	nlohmann::json GetMetaJson();
	/// A/B Compare: restart note for a flattened Upscaling key, empty if none.
	std::string RestartNote(const std::string& a_key);
}
