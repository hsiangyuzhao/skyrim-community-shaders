#include "Integration.h"

#include "Renderer.h"
#include "Runtime.h"

#include "Features/Upscaling.h"
#include "Globals.h"
#include "State.h"
#include "Utils/Batch38.h"
#include "Utils/GpuTimers.h"

#include <d3d11.h>
#include <dxgi1_4.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>
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

		/// The 37c pass, unchanged. Batch 38 off runs exactly this.
		bool ApplyLdrLegacy()
		{
			auto& upscaling = globals::features::upscaling;
			if (!upscaling.loaded || !upscaling.settings.neuralRendering.enabled)
				return false;

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

			// Frame generation, either backend, and asked of the frame rather than of the session:
			// DLSS-G intercepts Present asynchronously, and this pass writes into the scene through
			// its own D3D12 device, so the two cannot both be operating on the same image. What
			// matters is whether generation is running right now, not whether it was configured at
			// boot -- the proxy swapchain can sit idle without conflicting, which is what lets the
			// two features share a session.
			if (upscaling.d3d12SwapChainActive && upscaling.IsFrameGenerationRequestedNow()) {
				if (!g_loggedFrameGenerationBlock) {
					logger::warn("[DLSSNR] Blocked: Frame Generation is running. Switching it off takes effect immediately -- no restart.");
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

			auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
			auto* motionVectors = upscaling.motionVectorCopyTexture->resource.get();
			if (!depth.texture || !depth.depthSRV || !motionVectors)
				return LogBlockOnce("guides missing: depth={} depthSRV={} motionVectors={}",
					depth.texture != nullptr, depth.depthSRV != nullptr, motionVectors != nullptr);

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
				return LogBlockOnce("no render target is bound at the call site");
			}

			ID3D11Resource* color = nullptr;
			renderTargetView->GetResource(&color);

			D3D11_TEXTURE2D_DESC colorDesc{}, motionDesc{};
			const bool descsRead = GetTextureDesc(color, colorDesc) && GetTextureDesc(motionVectors, motionDesc);

			// (batch 38a) This comment had the cause backwards. The scene here is the finished
			// output-resolution image (the log says scene=3840x2160 at any DLSS preset), and the
			// "motion vector copy" is a texture nothing ever writes: the network was being fed
			// zeros, declared at the full output extent, while the engine's real vectors sit in the
			// render-resolution corner of another target. Batch 38 feeds the real vectors, resampled
			// onto the output grid (PrepareGuidesCS.hlsl). Kept as it was for the 37c path.
			static bool loggedExtents = false;
			if (descsRead && !loggedExtents) {
				logger::info("[DLSSNR] extents: scene={}x{} guides={}x{} scale={:.3f}",
					colorDesc.Width, colorDesc.Height, motionDesc.Width, motionDesc.Height,
					motionDesc.Width ? static_cast<float>(colorDesc.Width) / motionDesc.Width : 0.f);
				loggedExtents = true;
			}

			bool applied = false;
			if (descsRead) {
				// Unbind first: Renderer copies into and back out of this resource, which D3D11 will
				// not do while it is bound as a render target. Same reason ApplyNISSharpening unbinds.
				context->OMSetRenderTargets(0, nullptr, nullptr);

				// The guides run at the motion vector extent and the colour at the scene extent --
				// the two differ whenever DLSS is upscaling, which is the normal case. Motion vector
				// scale is the guide extent because the engine stores its vectors in normalised
				// screen units and the model wants pixels.
				Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::NeuralRendering);
				applied = Renderer::Instance().Apply(device, context, 0,
					color, depth.texture, depth.depthSRV, motionVectors,
					motionDesc.Width, motionDesc.Height,
					colorDesc.Width, colorDesc.Height,
					static_cast<float>(motionDesc.Width), static_cast<float>(motionDesc.Height),
					MakeTuning(upscaling.settings.neuralRendering));
				Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::NeuralRendering);

				// Put the binding back rather than leaving it to the engine's dirty flag. The
				// sharpening pass gets away with not restoring because nothing else reads the
				// binding before the engine's next draw; this pass is not last, so it has to leave
				// the pipeline as it found it.
				context->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
			}

			if (color)
				color->Release();
			if (depthStencilView)
				depthStencilView->Release();
			renderTargetView->Release();

			if (!descsRead)
				return LogBlockOnce("could not read a texture description for the scene or the motion vectors");

			return applied;
		}

		// ---- (batch 38a) ----------------------------------------------------------------------

		/// This process's use of the adapter's local video memory, in MB, or 0 when unavailable.
		/// Covers every device the process has on that adapter -- the game's, the frame-generation
		/// proxy's and the network's own -- which is what a leak would show up in.
		std::uint64_t QueryVramMB()
		{
			static Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter;
			if (!adapter) {
				auto* device = globals::d3d::device;
				if (!device)
					return 0;
				Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
				Microsoft::WRL::ComPtr<IDXGIAdapter> baseAdapter;
				if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&baseAdapter)) ||
					FAILED(baseAdapter.As(&adapter)))
					return 0;
			}
			DXGI_QUERY_VIDEO_MEMORY_INFO info{};
			if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
				return 0;
			return info.CurrentUsage / (1024ull * 1024ull);
		}

		std::string SignedMB(std::uint64_t from, std::uint64_t to)
		{
			const auto delta = static_cast<std::int64_t>(to) - static_cast<std::int64_t>(from);
			return std::format("{} MB -> {} MB ({:+} MB)", from, to, delta);
		}

		/// What this frame asks of the pass. Decided once, in BeforeUpscaling, so the two call
		/// sites can never disagree within a frame.
		struct Decision
		{
			bool run = false;
			bool before = false;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t renderWidth = 0;
			std::uint32_t renderHeight = 0;
			std::uint32_t modelPercent = 100;
			bool reset = false;
			std::uint32_t frame = 0;
		};

		/// Fields whose change gets a state line in the log.
		struct LogKey
		{
			bool batch38 = false;
			bool run = false;
			std::string blockedReason;
			bool before = false;
			std::uint32_t width = 0, height = 0, renderWidth = 0, renderHeight = 0;
			std::uint32_t modelPercent = 0;
			bool frameGeneration = false;
			std::uint32_t multiplier = 0;
			bool allowWithFrameGeneration = false;
			bool operator==(const LogKey&) const = default;
		};

		/// Fields whose change needs the working set rebuilt.
		struct ResourceKey
		{
			bool before = false;
			std::uint32_t width = 0, height = 0;
			std::uint32_t modelPercent = 0;
			bool operator==(const ResourceKey&) const = default;
		};

		FrameStatus g_status{};
		Decision g_decision{};
		LogKey g_lastLogKey{};
		bool g_haveLogKey = false;
		ResourceKey g_lastResourceKey{};
		bool g_haveResourceKey = false;
		bool g_wasBatch38 = true;
		bool g_ranLastFrame = false;
		std::uint32_t g_lastRunFrame = 0;
		float g_previousJitter[2]{};
		bool g_mapContext = false;
		// A (re)build happened and its memory has not been reported yet: the first frame that runs
		// afterwards logs the process's video memory next to the figure from before the build.
		bool g_reportVramAfterRun = false;
		std::uint64_t g_vramBeforeBuild = 0;

		std::uint32_t ClampModelPercent(std::uint32_t percent)
		{
			// Only the three steps the menu offers; anything else in a hand-edited file snaps to 100.
			return percent == 50 || percent == 75 ? percent : 100;
		}

		void ReleaseEverything(const char* why)
		{
			// A latched failure counts as holding state too: releasing is what clears the latch, so the
			// next time the pass is wanted it tries again instead of staying silently off.
			if (!Renderer::Instance().IsInitialized() && !Renderer::Instance().IsFailureLatched())
				return;
			const auto before = QueryVramMB();
			Renderer::Instance().Reset(false);
			const auto after = QueryVramMB();
			g_status.vramMB = after;
			g_haveResourceKey = false;
			g_reportVramAfterRun = false;
			logger::info("[DLSSNR] released everything ({}): video memory {}", why, SignedMB(before, after));
		}

		bool RunPass(const Decision& decision, const Renderer::PassInput& input)
		{
			Util::GpuPassTimers::GetSingleton()->Begin(Util::GpuBucket::NeuralRendering);
			const bool ran = Renderer::Instance().Run(globals::d3d::device, globals::d3d::context, input);
			Util::GpuPassTimers::GetSingleton()->End(Util::GpuBucket::NeuralRendering);

			g_status.running = ran;
			g_ranLastFrame = ran;
			if (ran) {
				g_lastRunFrame = decision.frame;
				const auto& jitter = globals::features::upscaling.jitter;
				g_previousJitter[0] = jitter.x;
				g_previousJitter[1] = jitter.y;
				if (g_reportVramAfterRun) {
					g_reportVramAfterRun = false;
					const auto now = QueryVramMB();
					g_status.vramMB = now;
					logger::info("[DLSSNR] running: {} {}x{} model={}% video memory {} (before it was built)",
						decision.before ? "before-upscaling" : "after-upscaling", decision.width, decision.height,
						decision.modelPercent, SignedMB(g_vramBeforeBuild, now));
				}
			}
			return ran;
		}

		Renderer::PassInput MakeInput(const Decision& decision)
		{
			Renderer::PassInput input;
			input.placement = decision.before ? Renderer::Placement::BeforeUpscaling : Renderer::Placement::AfterUpscaling;
			input.width = decision.width;
			input.height = decision.height;
			input.motionSourceWidth = decision.renderWidth;
			input.motionSourceHeight = decision.renderHeight;
			input.modelScale = static_cast<float>(decision.modelPercent) / 100.0f;
			input.reset = decision.reset;
			input.tuning = MakeTuning(globals::features::upscaling.settings.neuralRendering);
			return input;
		}
	}

	void BeforeUpscaling()
	{
		if (globals::game::isVR)
			return;
		auto& upscaling = globals::features::upscaling;
		const auto& nr = upscaling.settings.neuralRendering;
		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;

		g_status.batch38 = Batch38::IsOn();
		g_status.enabled = upscaling.loaded && nr.enabled;
		g_decision = {};
		g_decision.frame = frame;

		if (!Batch38::IsOn()) {
			// 37c has no hook here. The one thing done is to give back the Batch 38 working set the
			// moment the switch goes off, so the old path does not run with a second set of textures
			// sitting unused beside its own.
			if (g_wasBatch38) {
				if (Renderer::Instance().IsInitialized()) {
					const auto before = QueryVramMB();
					Renderer::Instance().ReleaseWorkingSet();
					logger::info("[DLSSNR] state: Batch 38 off -- 37c pass (after upscaling, blocked under frame generation); video memory {}",
						SignedMB(before, QueryVramMB()));
				} else {
					logger::info("[DLSSNR] state: Batch 38 off -- 37c pass (after upscaling, blocked under frame generation)");
				}
				g_wasBatch38 = false;
				g_haveLogKey = false;
				g_haveResourceKey = false;
			}
			g_status.blockedReason = "";
			return;
		}
		g_wasBatch38 = true;

		const bool frameGeneration = upscaling.d3d12SwapChainActive && upscaling.IsFrameGenerationRequestedNow();
		const char* blockedReason = "";
		if (!g_status.enabled)
			blockedReason = "switched off";
		else if (upscaling.GetUpscaleMethod() != Upscaling::UpscaleMethod::kDLSS || !upscaling.d3d12SwapChainActive)
			blockedReason = "the upscaler must be DLSS";
		else if (frameGeneration && !nr.allowWithFrameGeneration)
			blockedReason = "Frame Generation is running and \"Allow with Frame Generation\" is off";
		const bool run = blockedReason[0] == '\0';

		const auto screen = globals::state->screenSize;
		const auto scale = upscaling.resolutionScale;
		const std::uint32_t outputWidth = static_cast<std::uint32_t>(screen.x);
		const std::uint32_t outputHeight = static_cast<std::uint32_t>(screen.y);
		// Same rounding as the DLSS-G input extent (CopySharedD3D12Resources); at Quality on 4K this
		// is 2560x1440.
		const std::uint32_t renderWidth = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(screen.x * scale.x)), 1u, std::max(outputWidth, 1u));
		const std::uint32_t renderHeight = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(screen.y * scale.y)), 1u, std::max(outputHeight, 1u));
		const bool before = nr.runBeforeUpscaling;
		const std::uint32_t modelPercent = ClampModelPercent(nr.modelResolutionPercent);

		g_status.blockedReason = blockedReason;
		g_status.frameGeneration = frameGeneration;
		g_status.beforeUpscaling = before;
		g_status.renderWidth = renderWidth;
		g_status.renderHeight = renderHeight;
		g_status.outputWidth = outputWidth;
		g_status.outputHeight = outputHeight;
		g_status.width = before ? renderWidth : outputWidth;
		g_status.height = before ? renderHeight : outputHeight;
		g_status.modelPercent = modelPercent;
		if (!run)
			g_status.running = false;

		// Off or blocked: give the GPU memory back now rather than holding a model nobody is using.
		// 37c kept everything until the Reset button; the community reports of video memory
		// filling up on repeated toggling are exactly that pattern.
		if (!run) {
			ReleaseEverything(blockedReason);
			g_ranLastFrame = false;
		}

		LogKey key{ true, run, blockedReason, before, g_status.width, g_status.height, renderWidth, renderHeight, modelPercent,
			frameGeneration, frameGeneration ? upscaling.GetFrameGenerationAppliedMultiplier() : 0u, nr.allowWithFrameGeneration };
		if (!g_haveLogKey || !(key == g_lastLogKey)) {
			const auto vram = QueryVramMB();
			g_status.vramMB = vram;
			const std::string fg = frameGeneration ?
			                           std::format("on ({} {}x, {})", upscaling.IsDLSSGBackend() ? "DLSS-G" : "FSR 3",
										   upscaling.GetFrameGenerationAppliedMultiplier(), nr.allowWithFrameGeneration ? "allowed" : "not allowed") :
			                           std::string("off");
			if (run) {
				logger::info("[DLSSNR] state: on, {} at {}x{} (render {}x{}, output {}x{}), model {}%, frame generation {}, video memory {} MB",
					before ? "before upscaling" : "after upscaling", g_status.width, g_status.height, renderWidth, renderHeight,
					outputWidth, outputHeight, modelPercent, fg, vram);
			} else {
				logger::info("[DLSSNR] state: not running ({}), frame generation {}, video memory {} MB", blockedReason, fg, vram);
			}
			g_lastLogKey = std::move(key);
			g_haveLogKey = true;
		}
		if (!run)
			return;

		// Placement, extent or model resolution moved: drop the old feature and textures before the
		// new ones are made, with the queue drained first, and say what that did to video memory.
		const ResourceKey resourceKey{ before, g_status.width, g_status.height, modelPercent };
		if (!g_haveResourceKey || !(resourceKey == g_lastResourceKey)) {
			const auto vramBefore = QueryVramMB();
			if (Renderer::Instance().IsInitialized()) {
				Renderer::Instance().ReleaseWorkingSet();
				const auto vramAfter = QueryVramMB();
				logger::info("[DLSSNR] rebuilding for the new mode: released the old feature and textures, video memory {}",
					SignedMB(vramBefore, vramAfter));
				g_vramBeforeBuild = vramAfter;
			} else {
				g_vramBeforeBuild = vramBefore;
			}
			g_reportVramAfterRun = true;
			g_lastResourceKey = resourceKey;
			g_haveResourceKey = true;
			g_ranLastFrame = false;
		}

		// The network keeps a history; anything that breaks the frame-to-frame chain resets it: a
		// skipped frame, a rebuild, or the map's different camera opening or closing.
		const bool mapContext = upscaling.IsDLSSGMapRenderingContext();
		const bool continuous = g_ranLastFrame && g_lastRunFrame + 1 == frame && mapContext == g_mapContext;
		g_mapContext = mapContext;

		g_decision.run = true;
		g_decision.before = before;
		g_decision.width = g_status.width;
		g_decision.height = g_status.height;
		g_decision.renderWidth = renderWidth;
		g_decision.renderHeight = renderHeight;
		g_decision.modelPercent = modelPercent;
		g_decision.reset = !continuous;

		if (!before)
			return;  // ApplyLdr runs it, after the post chain

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context) {
			g_status.running = false;
			return;
		}
		auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
		auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (!main.texture || !main.SRV || !motion.SRV || !depth.depthSRV) {
			g_status.running = false;
			g_ranLastFrame = false;
			LogBlockOnce("before upscaling: scene, motion or depth view missing: main={} mainSRV={} motionSRV={} depthSRV={}",
				main.texture != nullptr, main.SRV != nullptr, motion.SRV != nullptr, depth.depthSRV != nullptr);
			return;
		}

		auto input = MakeInput(g_decision);
		input.sceneColor = main.texture;
		input.sceneColorSRV = main.SRV;
		input.depthSRV = depth.depthSRV;
		input.motionSRV = motion.SRV;
		// The colour here is jittered and the engine's vectors are not (they are built from the
		// unjittered matrices). The jitter moves the image by -jitter render pixels, so the
		// image-space motion from this frame back to the last is the engine's vector plus
		// (jitter_now - jitter_previous) / render extent. Without it the network's history would
		// be off by the jitter step every frame, which it would smooth away -- taking from DLSS the
		// sub-pixel variation it reconstructs from.
		if (nr.jitterAwareMotion && !g_decision.reset) {
			const auto& jitter = upscaling.jitter;
			input.motionOffsetX = (jitter.x - g_previousJitter[0]) / static_cast<float>(renderWidth);
			input.motionOffsetY = (jitter.y - g_previousJitter[1]) / static_cast<float>(renderHeight);
		}

		// kMAIN is read through an SRV below, so nothing may hold it as a render target. Upscale()
		// unbinds them right after this anyway; the dirty flag makes the engine rebind its own.
		context->OMSetRenderTargets(0, nullptr, nullptr);
		RunPass(g_decision, input);
		globals::game::stateUpdateFlags->set(RE::BSGraphics::ShaderFlags::DIRTY_RENDERTARGET);
	}

	bool ApplyLdr()
	{
		if (!Batch38::IsOn())
			return ApplyLdrLegacy();

		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0;
		if (!g_decision.run || g_decision.before || g_decision.frame != frame)
			return false;

		auto* renderer = globals::game::renderer;
		auto* context = globals::d3d::context;
		if (!renderer || !context)
			return false;
		auto& motion = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
		auto& depth = renderer->GetDepthStencilData().depthStencils[RE::RENDER_TARGETS_DEPTHSTENCIL::kMAIN];
		if (!motion.SRV || !depth.depthSRV) {
			g_status.running = false;
			g_ranLastFrame = false;
			return LogBlockOnce("after upscaling: motion or depth view missing: motionSRV={} depthSRV={}",
				motion.SRV != nullptr, depth.depthSRV != nullptr);
		}

		// Same reasoning as the 37c path: the finished image is whatever is bound now, and the
		// binding is put back exactly as found because ApplyNISSharpening reads it next.
		ID3D11RenderTargetView* renderTargetView = nullptr;
		ID3D11DepthStencilView* depthStencilView = nullptr;
		context->OMGetRenderTargets(1, &renderTargetView, &depthStencilView);
		if (!renderTargetView) {
			if (depthStencilView)
				depthStencilView->Release();
			g_status.running = false;
			g_ranLastFrame = false;
			return LogBlockOnce("no render target is bound at the call site");
		}
		ID3D11Resource* color = nullptr;
		renderTargetView->GetResource(&color);
		D3D11_TEXTURE2D_DESC colorDesc{};
		bool ran = false;
		if (GetTextureDesc(color, colorDesc)) {
			auto input = MakeInput(g_decision);
			input.sceneColor = color;
			// Depth after upscaling is Upscaling's output-resolution depth; the engine's vectors are
			// still at render resolution and get resampled onto the output grid in the guide pass.
			input.depthSRV = depth.depthSRV;
			input.motionSRV = motion.SRV;
			input.width = std::min(input.width, colorDesc.Width);
			input.height = std::min(input.height, colorDesc.Height);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			ran = RunPass(g_decision, input);
			context->OMSetRenderTargets(1, &renderTargetView, depthStencilView);
		} else {
			g_status.running = false;
			g_ranLastFrame = false;
			LogBlockOnce("could not read the scene's texture description");
		}
		if (color)
			color->Release();
		if (depthStencilView)
			depthStencilView->Release();
		renderTargetView->Release();
		return ran;
	}

	void Reset()
	{
		Renderer::Instance().Reset();
		g_loggedFrameGenerationBlock = false;
		g_loggedUpscalerBlock = false;
		g_loggedResourceBlock = false;
		g_haveResourceKey = false;
		g_haveLogKey = false;
		g_ranLastFrame = false;
		g_reportVramAfterRun = false;
		g_status.running = false;
	}

	const FrameStatus& GetFrameStatus() { return g_status; }

	json StatusJson()
	{
		const auto& renderer = Renderer::Instance();
		const auto& s = g_status;
		json o;
		o["batch38"] = s.batch38;
		o["enabled"] = s.enabled;
		o["running"] = s.running;
		o["blocked_reason"] = std::string(s.blockedReason);
		o["placement"] = s.batch38 ? (s.beforeUpscaling ? "before-upscaling" : "after-upscaling") : "after-upscaling (37c)";
		o["frame_generation"] = s.frameGeneration;
		o["extent"] = { s.width, s.height };
		o["render"] = { s.renderWidth, s.renderHeight };
		o["output"] = { s.outputWidth, s.outputHeight };
		o["model_percent"] = s.batch38 ? s.modelPercent : 100u;
		o["model_gpu_ms"] = renderer.ModelGpuMs();
		o["model_gpu_ms_last"] = renderer.LastModelGpuMs();
		o["max_in_flight"] = renderer.MaxInFlight();
		o["backpressure_waits"] = renderer.BackpressureWaits();
		o["frames"] = renderer.SuccessfulFrames();
		o["failure_latched"] = renderer.IsFailureLatched();
		o["runtime"] = std::string(renderer.StatusText());
		o["vram_mb_at_last_change"] = s.vramMB;
		o["vram_mb_now"] = QueryVramMB();
		return o;
	}
}
