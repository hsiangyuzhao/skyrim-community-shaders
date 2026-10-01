#include "Integration.h"

#include "Renderer.h"
#include "Runtime.h"

#include "Features/Upscaling.h"
#include "Globals.h"

#include <d3d11.h>

#include <string_view>

namespace NeuralRendering
{
	namespace
	{
		Diagnostics g_diagnostics;
		bool g_hadSuccess = false;
		bool g_historyContinuous = false;
		bool g_lastSuccessfulFgRequest = false;
		bool g_lastLoggedFgRequest = false;
		Tuning g_lastSuccessfulTuning;

		struct Layout
		{
			std::uint32_t sceneWidth = 0, sceneHeight = 0, depthWidth = 0, depthHeight = 0;
			std::uint32_t motionWidth = 0, motionHeight = 0;
			DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN, depthFormat = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
			bool operator==(const Layout&) const = default;
		};
		Layout g_lastLoggedLayout;

		bool SameTuning(const Tuning& left, const Tuning& right)
		{
			return left.intensity == right.intensity && left.localToneStrength == right.localToneStrength &&
			       left.localStructureStrength == right.localStructureStrength &&
			       left.skinStructureStrength == right.skinStructureStrength && left.style == right.style &&
			       left.useAutoMask == right.useAutoMask && left.uiCorrection == right.uiCorrection;
		}

		// Log state edges, including a live FG switch while NR keeps returning "applied".
		// The resource identities let an A/B log correlate the NR source with HUD-less capture.
		void SetStatus(const char* status, bool warning = false)
		{
			const bool changed = std::string_view(g_diagnostics.status) != status ||
				g_diagnostics.frameGenerationRequested != g_lastLoggedFgRequest;
			g_diagnostics.status = status;
			g_diagnostics.failureLatched = Renderer::Instance().IsFailureLatched();
			g_diagnostics.ngxResult = Renderer::Instance().NgxResult();
			if (!changed)
				return;
			if (warning)
				logger::warn("[DLSSNR][AIO] status={} fgRequested={} attempts={} successes={} failures={} ngx=0x{:08X} source=0x{:X} depth=0x{:X} motion=0x{:X}",
					status, g_diagnostics.frameGenerationRequested, g_diagnostics.attempts,
					g_diagnostics.successes, g_diagnostics.failures, g_diagnostics.ngxResult,
					g_diagnostics.sceneResource, g_diagnostics.depthResource, g_diagnostics.motionResource);
			else
				logger::info("[DLSSNR][AIO] status={} fgRequested={} attempts={} successes={} failures={} source=0x{:X} depth=0x{:X} motion=0x{:X}",
					status, g_diagnostics.frameGenerationRequested, g_diagnostics.attempts,
					g_diagnostics.successes, g_diagnostics.failures,
					g_diagnostics.sceneResource, g_diagnostics.depthResource, g_diagnostics.motionResource);
			g_lastLoggedFgRequest = g_diagnostics.frameGenerationRequested;
		}

		bool Skip(const char* reason, bool warning = true)
		{
			g_historyContinuous = false;
			SetStatus(reason, warning);
			return false;
		}

		Tuning MakeTuning(const Upscaling::NeuralRenderingSettings& settings)
		{
			Tuning tuning;
			tuning.intensity = settings.intensity;
			tuning.localToneStrength = settings.localToneStrength;
			tuning.localStructureStrength = settings.localStructureStrength;
			tuning.skinStructureStrength = settings.skinStructureStrength;
			tuning.style = settings.style;
			tuning.useAutoMask = settings.useAutoMask;
			tuning.uiCorrection = settings.uiCorrection;
			return tuning;
		}

		bool GetTextureDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& out)
		{
			if (!resource)
				return false;
			ID3D11Texture2D* texture = nullptr;
			if (FAILED(resource->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&texture))) || !texture)
				return false;
			texture->GetDesc(&out);
			texture->Release();
			return true;
		}
	}

	bool ApplyLdr()
	{
		auto& upscaling = globals::features::upscaling;
		g_diagnostics.appliedThisFrame = false;
		g_diagnostics.historyResetThisFrame = false;
		g_diagnostics.requested = upscaling.loaded && upscaling.settings.neuralRendering.enabled;
		g_diagnostics.frameGenerationRequested = upscaling.d3d12SwapChainActive &&
			upscaling.IsFrameGenerationRequestedNow();
		g_diagnostics.sceneWidth = g_diagnostics.sceneHeight = 0;
		g_diagnostics.depthWidth = g_diagnostics.depthHeight = 0;
		g_diagnostics.motionWidth = g_diagnostics.motionHeight = 0;
		g_diagnostics.sceneFormat = g_diagnostics.depthFormat = g_diagnostics.motionFormat = 0;
		g_diagnostics.sceneResource = g_diagnostics.depthResource = g_diagnostics.motionResource = 0;
		g_diagnostics.sizeContractSatisfied = false;
		if (!g_diagnostics.requested)
			return Skip("disabled", false);

		// The upscaler must be DLSS. Neural Rendering is an NGX feature and shares that
		// machinery; the reference integration gates on the same thing. It is also the only
		// configuration where the motion vector copy this pass reads is guaranteed to exist.
		if (upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS)
			return Skip("DLSS-upscaler-required");

		// NR writes back through D3D11 before the HUD-less capture/FG present path. Interop
		// fences order its separate D3D12 queue against that D3D11 write. Coexistence is an
		// experiment until the FG input-lifetime fence is checked in the presenting path.
		if (g_diagnostics.frameGenerationRequested && !upscaling.IsDLSSGBackend())
			return Skip("NR-plus-FSR3FG-not-in-this-experiment");
		if (Renderer::Instance().IsFailureLatched())
			return Skip("NR-failure-latched");

		// Every path out of here used to be a bare return, which is how a blocked frame ends up
		// indistinguishable from a working one that simply did nothing -- the exact failure this
		// feature's diagnostics exist to avoid. Each now names itself once.
		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		auto* device = globals::d3d::device;
		if (!renderer || !context || !device || !upscaling.motionVectorCopyTexture)
			return Skip("missing-renderer-context-device-or-motion-copy");

		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto* motionVectors = upscaling.motionVectorCopyTexture->resource.get();
		if (!depth.texture || !depth.depthSRV || !motionVectors)
			return Skip("missing-depth-depthSRV-or-motion-resource");
		g_diagnostics.depthResource = reinterpret_cast<std::uintptr_t>(depth.texture);
		g_diagnostics.motionResource = reinterpret_cast<std::uintptr_t>(motionVectors);

		// The scene is taken from whatever render target is bound right now, not from a named
		// slot. The first attempt read RENDER_TARGETS::kFRAMEBUFFER on the strength of the
		// reference integration's comment, and it is empty here -- nothing else in this tree
		// touches that slot. ApplyNISSharpening, a few lines below the call site, has always
		// done it this way, and it is the only method that does not depend on guessing which
		// slot the engine happens to be using at this point in its own chain.
		//
		// The depth-stencil view comes along only so the binding can be put back exactly as it
		// was found. ApplyNISSharpening runs immediately after this and reads the bound target
		// without checking it for null, so leaving the pipeline unbound here is an access
		// violation in that function -- which is exactly what the first attempt did.
		ID3D11RenderTargetView* renderTargetView = nullptr;
		ID3D11DepthStencilView* depthStencilView = nullptr;
		context->OMGetRenderTargets(1, &renderTargetView, &depthStencilView);
		if (!renderTargetView) {
			if (depthStencilView)
				depthStencilView->Release();
			return Skip("no-bound-render-target");
		}

		ID3D11Resource* color = nullptr;
		renderTargetView->GetResource(&color);
		g_diagnostics.sceneResource = reinterpret_cast<std::uintptr_t>(color);

		D3D11_TEXTURE2D_DESC colorDesc{}, depthDesc{}, motionDesc{};
		const bool descsRead = GetTextureDesc(color, colorDesc) &&
			GetTextureDesc(depth.texture, depthDesc) && GetTextureDesc(motionVectors, motionDesc);
		if (descsRead) {
			g_diagnostics.sceneWidth = colorDesc.Width;
			g_diagnostics.sceneHeight = colorDesc.Height;
			g_diagnostics.depthWidth = depthDesc.Width;
			g_diagnostics.depthHeight = depthDesc.Height;
			g_diagnostics.motionWidth = motionDesc.Width;
			g_diagnostics.motionHeight = motionDesc.Height;
			g_diagnostics.sceneFormat = static_cast<std::uint32_t>(colorDesc.Format);
			g_diagnostics.depthFormat = static_cast<std::uint32_t>(depthDesc.Format);
			g_diagnostics.motionFormat = static_cast<std::uint32_t>(motionDesc.Format);
			g_diagnostics.sizeContractSatisfied = colorDesc.Width >= motionDesc.Width &&
				colorDesc.Height >= motionDesc.Height && depthDesc.Width == motionDesc.Width &&
				depthDesc.Height == motionDesc.Height;

			const Layout layout{ colorDesc.Width, colorDesc.Height, depthDesc.Width, depthDesc.Height,
				motionDesc.Width, motionDesc.Height, colorDesc.Format, depthDesc.Format, motionDesc.Format };
			if (!(layout == g_lastLoggedLayout)) {
				logger::info("[DLSSNR][AIO] input-layout scene={}x{} fmt={} depth={}x{} fmt={} motion={}x{} fmt={} contract={}",
					colorDesc.Width, colorDesc.Height, static_cast<unsigned>(colorDesc.Format),
					depthDesc.Width, depthDesc.Height, static_cast<unsigned>(depthDesc.Format),
					motionDesc.Width, motionDesc.Height, static_cast<unsigned>(motionDesc.Format),
					g_diagnostics.sizeContractSatisfied);
				g_lastLoggedLayout = layout;
				if (!g_diagnostics.sizeContractSatisfied)
					logger::warn("[DLSSNR][AIO] input layout violates the expected output-scene / render-resolution-guide contract; compare visual output before treating NR+FG artifacts as an FG issue");
			}
		}

		bool applied = false;
		bool historyReady = true;
		if (descsRead && colorDesc.Width && colorDesc.Height && motionDesc.Width && motionDesc.Height &&
			colorDesc.ArraySize == 1 && colorDesc.MipLevels == 1 && colorDesc.SampleDesc.Count == 1 &&
			motionDesc.ArraySize == 1 && motionDesc.MipLevels == 1 && motionDesc.SampleDesc.Count == 1) {
			const Tuning tuning = MakeTuning(upscaling.settings.neuralRendering);
			if (g_hadSuccess && !Renderer::Instance().IsFailureLatched() &&
				(!g_historyContinuous || g_lastSuccessfulFgRequest != g_diagnostics.frameGenerationRequested ||
				 !SameTuning(g_lastSuccessfulTuning, tuning))) {
				const char* reason = !g_historyContinuous ? "interrupted" :
					g_lastSuccessfulFgRequest != g_diagnostics.frameGenerationRequested ? "FG-switch" : "tuning-switch";
				historyReady = Renderer::Instance().ResetHistory();
				if (historyReady) {
					g_diagnostics.historyResetThisFrame = true;
					++g_diagnostics.historyResets;
					logger::info("[DLSSNR][AIO] temporal history reset reason={} fgRequested={}",
						reason, g_diagnostics.frameGenerationRequested);
				} else {
					++g_diagnostics.failures;
				}
			}
			if (historyReady) {
				++g_diagnostics.attempts;
				// Renderer copies into and back out of this resource, so unbind it while it runs.
				context->OMSetRenderTargets(0, nullptr, nullptr);

				// Engine vectors are in normalised screen units; the model needs guide pixels.
				applied = Renderer::Instance().Apply(device, context, 0,
					color, depth.texture, depth.depthSRV, motionVectors,
					motionDesc.Width, motionDesc.Height,
					colorDesc.Width, colorDesc.Height,
					static_cast<float>(motionDesc.Width), static_cast<float>(motionDesc.Height), tuning);

				// Sharpening follows this pass and reads the bound target. Restore it on both
				// success and failure instead of relying on the engine's dirty state.
				context->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
				if (applied) {
					++g_diagnostics.successes;
					g_diagnostics.appliedThisFrame = true;
					g_hadSuccess = true;
					g_historyContinuous = true;
					g_lastSuccessfulFgRequest = g_diagnostics.frameGenerationRequested;
					g_lastSuccessfulTuning = tuning;
				} else {
					++g_diagnostics.failures;
					g_historyContinuous = false;
				}
			}
		}

		if (color)
			color->Release();
		if (depthStencilView)
			depthStencilView->Release();
		renderTargetView->Release();

		if (!descsRead)
			return Skip("scene-depth-or-motion-description-unavailable");
		if (!colorDesc.Width || !colorDesc.Height || !motionDesc.Width || !motionDesc.Height ||
			colorDesc.ArraySize != 1 || colorDesc.MipLevels != 1 || colorDesc.SampleDesc.Count != 1 ||
			motionDesc.ArraySize != 1 || motionDesc.MipLevels != 1 || motionDesc.SampleDesc.Count != 1)
			return Skip("copy-resource-incompatible-source");
		if (!historyReady)
			return Skip("NR-history-reset-failed");
		SetStatus(applied ? "applied" : "NR-renderer-failed", !applied);
		return applied;
	}

	Diagnostics GetDiagnostics()
	{
		return g_diagnostics;
	}

	void Reset()
	{
		if (!Renderer::Instance().Reset()) {
			SetStatus("NR-reset-failed", true);
			return;
		}
		g_diagnostics = {};
		g_hadSuccess = false;
		g_historyContinuous = false;
		g_lastSuccessfulFgRequest = false;
		g_lastLoggedFgRequest = false;
		g_lastSuccessfulTuning = {};
		g_lastLoggedLayout = {};
		logger::info("[DLSSNR][AIO] renderer and diagnostics reset");
	}
}
