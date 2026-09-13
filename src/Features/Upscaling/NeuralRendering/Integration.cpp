#include "Integration.h"

#include "Renderer.h"
#include "Runtime.h"

#include "Features/Upscaling.h"
#include "Globals.h"

#include <d3d11.h>

#include <format>
#include <utility>

namespace NeuralRendering
{
	namespace
	{
		// Logged once per reason so a blocked frame says why exactly once, rather than either
		// staying silent (the failure mode that cost seven rounds on the eye defect) or filling
		// the log at frame rate.
		bool g_loggedFrameGenerationBlock = false;
		bool g_loggedUpscalerBlock = false;
		bool g_loggedResourceBlock = false;
		Counters g_counters;

		// Always returns false, so a blocked path reads as `return LogBlockOnce(...)` and cannot
		// accidentally fall through to the pass. One line per session per reason: this runs at
		// frame rate, and a reason that repeats is not more informative the thousandth time.
		template <class... Args>
		bool LogBlockOnce(std::format_string<Args...> format, Args&&... args)
		{
			if (!g_loggedResourceBlock) {
				logger::warn("[DLSSNR] Blocked: {}", std::format(format, std::forward<Args>(args)...));
				g_loggedResourceBlock = true;
			}
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

	Counters GetCounters() { return g_counters; }

	bool ApplyLdr()
	{
		auto& upscaling = globals::features::upscaling;
		if (!upscaling.loaded || !upscaling.settings.neuralRendering.enabled)
			return false;

		// Counted after the enabled check and before every gate, so it answers exactly one
		// question: is this function being reached at all while the feature is on.
		++g_counters.attempts;

		// The upscaler must be DLSS. Neural Rendering is an NGX feature and shares that
		// machinery; the reference integration gates on the same thing. It is also the only
		// configuration where the motion vector copy this pass reads is guaranteed to exist.
		if (upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS) {
			if (!g_loggedUpscalerBlock) {
				logger::warn("[DLSSNR] Blocked: the upscaler must be DLSS (currently {})",
					magic_enum::enum_name(upscaling.GetUpscaleMethod()));
				g_loggedUpscalerBlock = true;
			}
			return false;
		}

		// Frame generation, either backend. Both route the frame through the D3D11-to-D3D12
		// proxy swapchain, and this pass owns a separate D3D12 device whose shared resources
		// cannot be sequenced against a swapchain it does not control. The gate is on the proxy
		// being active rather than on which backend was chosen, because the proxy is the thing
		// that conflicts -- FSR 3.1 hits it exactly as DLSS-G does.
		if (upscaling.d3d12SwapChainActive && upscaling.IsFrameGenerationEnabled()) {
			if (!g_loggedFrameGenerationBlock) {
				logger::warn("[DLSSNR] Blocked: disable Frame Generation and restart the game");
				g_loggedFrameGenerationBlock = true;
			}
			return false;
		}

		// Every path out of here used to be a bare return, which is how a blocked frame ends up
		// indistinguishable from a working one that simply did nothing -- the exact failure this
		// feature's diagnostics exist to avoid. Each now names itself once.
		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		auto* device = globals::d3d::device;
		if (!renderer || !context || !device || !upscaling.motionVectorCopyTexture)
			return LogBlockOnce("prerequisites missing: renderer={} context={} device={} motionVectorCopy={}",
				renderer != nullptr, context != nullptr, device != nullptr,
				upscaling.motionVectorCopyTexture != nullptr);

		// kFRAMEBUFFER is where the engine's post chain leaves the finished tonemapped scene.
		auto& framebuffer = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kFRAMEBUFFER];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		auto* motionVectors = upscaling.motionVectorCopyTexture->resource.get();
		if (!framebuffer.texture || !depth.texture || !depth.depthSRV || !motionVectors)
			return LogBlockOnce("render targets missing: framebuffer={} depth={} depthSRV={} motionVectors={}",
				framebuffer.texture != nullptr, depth.texture != nullptr,
				depth.depthSRV != nullptr, motionVectors != nullptr);

		D3D11_TEXTURE2D_DESC colorDesc{}, motionDesc{};
		if (!GetTextureDesc(framebuffer.texture, colorDesc) || !GetTextureDesc(motionVectors, motionDesc))
			return LogBlockOnce("could not read a texture description for the framebuffer or the motion vectors");

		// The guides run at the motion vector extent and the colour at the framebuffer extent --
		// the two differ whenever DLSS is upscaling, which is the normal case. Motion vector
		// scale is the guide extent because the engine stores its vectors in normalised screen
		// units and the model wants pixels.
		const bool applied = Renderer::Instance().Apply(device, context, 0,
			framebuffer.texture, depth.texture, depth.depthSRV, motionVectors,
			motionDesc.Width, motionDesc.Height,
			colorDesc.Width, colorDesc.Height,
			static_cast<float>(motionDesc.Width), static_cast<float>(motionDesc.Height),
			MakeTuning(upscaling.settings.neuralRendering));
		g_counters.applications += applied ? 1 : 0;
		return applied;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		g_loggedFrameGenerationBlock = false;
		g_loggedUpscalerBlock = false;
		g_loggedResourceBlock = false;
		g_counters = {};
	}
}
