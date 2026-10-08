#include "Integration.h"

#include "Renderer.h"
#include "Runtime.h"

#include "Features/LinearLighting.h"
#include "Features/PostProcessing.h"
#include "Features/Upscaling.h"
#include "Globals.h"
#include "State.h"
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
			bool pad = false;  ///< (batch 38c) pad the network extent to kNetworkAlign
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
			// (batch 38c)
			bool pad = false;
			bool toneMatched = false;
			std::uint32_t inputPrecision = 0;
			bool toneOn = false;
			bool tuningAtCreate = false;
			bool operator==(const LogKey&) const = default;
		};

		/// Fields whose change needs the working set rebuilt.
		struct ResourceKey
		{
			bool before = false;
			std::uint32_t width = 0, height = 0;
			std::uint32_t modelPercent = 0;
			// (batch 38c) Padding, the network's input format and tone preservation on/off change the
			// textures (or, after upscaling, whether the colour goes by copy or through the shaders).
			bool pad = false;
			std::uint32_t inputPrecision = 0;
			bool toneOn = false;
			bool operator==(const ResourceKey&) const = default;
		};

		FrameStatus g_status{};
		Decision g_decision{};
		LogKey g_lastLogKey{};
		bool g_haveLogKey = false;
		ResourceKey g_lastResourceKey{};
		bool g_haveResourceKey = false;
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

		// ---- (batch 38c) -----------------------------------------------------------------------

		/// The network's grid. Every DLSS preset but Balanced hands the network an extent that is a
		/// multiple of 8 each way (3840x2160, 2560x1440, 1920x1080, 1280x720); Balanced's 2227x1253 is
		/// odd both ways, and it is the one preset the user saw in a different tone. Everything on our
		/// side is exact at odd extents (1:1 reads, per-pixel passes with bounds checks, dispatches
		/// rounded up, copy boxes and texture sizes equal to the extent, the same extent given to NGX as
		/// size and subrects), so the difference is inside the network: a U-Net whose kernels (Swin
		/// blocks with _ds / _upsample stages from 32 up to 1024 channels) halve the extent at every
		/// level. Padding to 8, edge repeated, then cropping gives every preset the shape the others
		/// already had.
		constexpr std::uint32_t kNetworkAlign = 8;

		struct Extents
		{
			std::uint32_t width = 0, height = 0;              ///< the frame
			std::uint32_t workWidth = 0, workHeight = 0;      ///< the model's content (Model Resolution)
			std::uint32_t paddedWidth = 0, paddedHeight = 0;  ///< the network's
		};

		Extents ComputeExtents(std::uint32_t width, std::uint32_t height, std::uint32_t modelPercent, bool pad)
		{
			Extents e{ width, height, width, height, width, height };
			if (modelPercent < 100) {
				e.workWidth = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(width * (modelPercent / 100.0))), std::min(width, 16u), width);
				e.workHeight = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(height * (modelPercent / 100.0))), std::min(height, 16u), height);
			}
			e.paddedWidth = pad ? (e.workWidth + kNetworkAlign - 1) / kNetworkAlign * kNetworkAlign : e.workWidth;
			e.paddedHeight = pad ? (e.workHeight + kNetworkAlign - 1) / kNetworkAlign * kNetworkAlign : e.workHeight;
			return e;
		}

		DXGI_FORMAT NetworkInputFormat(std::uint32_t precision)
		{
			return precision == 2 ? DXGI_FORMAT_R16G16B16A16_FLOAT :
			       precision == 1 ? DXGI_FORMAT_R10G10B10A2_UNORM :
			                        DXGI_FORMAT_R8G8B8A8_UNORM;
		}

		const char* InputPrecisionName(std::uint32_t precision)
		{
			return precision == 2 ? "16-bit float" : precision == 1 ? "10-bit" : "8-bit";
		}

		/// What the post-processing chain will do to this frame, for the tone curve (ToneCurveCS.hlsl).
		/// With "Disable Vanilla Tonemapping" on (the user's setup), CS's chain is the tone mapper:
		/// Histogram Auto Exposure scales the scene by 0.18 * compensation / clamp(adapted luminance),
		/// then Color Grading applies its cinematic steps and its baked LUT, and with Linear Lighting's
		/// gamma correction the image-space pass ends in pow(1/2.2). Each piece is used only when it is
		/// actually running; the rest falls back (fixed exposure 1, per-channel Reinhard).
		Renderer::ToneCurveSource GatherToneCurve(const char*& description)
		{
			Renderer::ToneCurveSource source;
			auto& pp = globals::features::postProcessing;
			const bool chain = pp.loaded && !pp.bypass;

			auto* autoExposure = chain ? static_cast<HistogramAutoExposure*>(
											 pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::AutoExposure)].get()) :
			                             nullptr;
			if (autoExposure && autoExposure->enabled && autoExposure->adaptationSB) {
				float compensation = 0.0f;
				float2 range{};
				autoExposure->GetExposureParameters(compensation, range);
				source.adaptation = autoExposure->adaptationSB->SRV();
				source.exposureCompensation = std::exp2(compensation);
				source.adaptationMin = std::exp2(range.x) * 0.125f;
				source.adaptationMax = std::exp2(range.y) * 0.125f;
			}

			auto* grading = chain && pp.settings.DisableVanillaTonemapping ?
			                    static_cast<ColorGrading*>(pp.pipeline[static_cast<size_t>(PostProcessing::FeaturePipelineIndex::ColorGrading)].get()) :
			                    nullptr;
			if (grading && grading->enabled && grading->texLUT && grading->lutValid) {
				const auto& cb = grading->bakedColorCBData;
				source.gradingLUT = grading->texLUT->srv.get();
				source.inputGamma = cb.saturationHueInOutGamma.z;
				source.outputGamma = cb.saturationHueInOutGamma.w;
				source.cinematicBrightness = cb.cinematic.y;
				source.cinematicContrast = cb.cinematic.z;
				source.tint[0] = cb.tint.x;
				source.tint[1] = cb.tint.y;
				source.tint[2] = cb.tint.z;
				source.tint[3] = cb.tint.w;
			}

			const auto& linearLighting = globals::features::linearLighting.settings;
			source.gammaCorrect = linearLighting.enableLinearLighting && linearLighting.enableGammaCorrection;

			description = source.gradingLUT ? (source.adaptation ? "game exposure + colour grading" : "colour grading (no auto exposure)") :
			                                  (source.adaptation ? "game exposure + fallback curve" : "fallback curve");
			return source;
		}

		/// The model reads its tuning when the feature is built, so a change means a new feature. A slider
		/// being dragged would rebuild it every frame (OptiScaler found that exhausts the driver's latches);
		/// the tuning given to the model moves only once the menu's value has held still for 30 frames.
		Tuning g_pendingTuning{};
		Tuning g_settledTuning{};
		std::uint32_t g_pendingTuningSince = 0;
		bool g_haveTuning = false;

		const Tuning& SettledTuning(const Tuning& current, std::uint32_t frame)
		{
			if (!g_haveTuning) {
				g_pendingTuning = g_settledTuning = current;
				g_haveTuning = true;
			} else if (!(current == g_pendingTuning)) {
				g_pendingTuning = current;
				g_pendingTuningSince = frame;
			} else if (!(g_settledTuning == g_pendingTuning) && frame - g_pendingTuningSince >= 30) {
				g_settledTuning = g_pendingTuning;
			}
			return g_settledTuning;
		}

		const char* g_toneSource = "";

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

		/// @param width, height the frame extent actually processed (after upscaling: clamped to the
		///        bound target).
		Renderer::PassInput MakeInput(const Decision& decision, std::uint32_t width, std::uint32_t height)
		{
			const auto& nr = globals::features::upscaling.settings.neuralRendering;
			Renderer::PassInput input;
			input.placement = decision.before ? Renderer::Placement::BeforeUpscaling : Renderer::Placement::AfterUpscaling;
			const auto extents = ComputeExtents(width, height, decision.modelPercent, decision.pad);
			input.width = extents.width;
			input.height = extents.height;
			input.workWidth = extents.workWidth;
			input.workHeight = extents.workHeight;
			input.paddedWidth = extents.paddedWidth;
			input.paddedHeight = extents.paddedHeight;
			input.motionSourceWidth = decision.renderWidth;
			input.motionSourceHeight = decision.renderHeight;
			input.encode = !decision.before ? Renderer::Encode::Identity :
			               nr.toneMatchedInput ? Renderer::Encode::Curve :
			                                     Renderer::Encode::Reinhard;
			input.networkFormat = NetworkInputFormat(nr.inputPrecision);
			if (input.encode == Renderer::Encode::Curve)
				input.toneCurve = GatherToneCurve(g_toneSource);
			else
				g_toneSource = decision.before ? "38a wrap (Reinhard, no exposure)" : "the finished image";
			input.toneStrength = std::clamp(decision.before ? nr.tonePreservationBefore : nr.tonePreservationAfter, 0.0f, 1.0f);
			if (globals::game::cameraNear && globals::game::cameraFar) {
				input.cameraNear = std::max(*globals::game::cameraNear, 0.01f);
				input.cameraFar = std::max(*globals::game::cameraFar, input.cameraNear + 1.0f);
			}
			input.reset = decision.reset;
			input.tuningAtCreate = nr.tuningAtCreate;
			const auto current = MakeTuning(nr);
			input.tuning = nr.tuningAtCreate ? SettledTuning(current, decision.frame) : current;
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

		g_status.enabled = upscaling.loaded && nr.enabled;
		g_decision = {};
		g_decision.frame = frame;

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
		// (batch 38c)
		const bool pad = nr.padToNetworkGrid;
		const bool toneMatched = before && nr.toneMatchedInput;
		const std::uint32_t inputPrecision = before ? std::min(nr.inputPrecision, 2u) : 0u;
		const float toneStrength = std::clamp(before ? nr.tonePreservationBefore : nr.tonePreservationAfter, 0.0f, 1.0f);
		const bool toneOn = toneStrength > 0.0f;
		{
			const auto extents = ComputeExtents(g_status.width, g_status.height, modelPercent, pad);
			g_status.workWidth = extents.workWidth;
			g_status.workHeight = extents.workHeight;
			g_status.paddedWidth = extents.paddedWidth;
			g_status.paddedHeight = extents.paddedHeight;
		}
		g_status.toneMatched = toneMatched;
		g_status.inputPrecision = inputPrecision;
		g_status.toneStrength = toneStrength;
		g_status.tuningAtCreate = nr.tuningAtCreate;
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
			frameGeneration, frameGeneration ? upscaling.GetFrameGenerationAppliedMultiplier() : 0u, nr.allowWithFrameGeneration,
			pad, toneMatched, inputPrecision, toneOn, nr.tuningAtCreate };
		if (!g_haveLogKey || !(key == g_lastLogKey)) {
			const auto vram = QueryVramMB();
			g_status.vramMB = vram;
			const std::string fg = frameGeneration ?
			                           std::format("on ({} {}x, {})", upscaling.IsDLSSGBackend() ? "DLSS-G" : "FSR 3",
										   upscaling.GetFrameGenerationAppliedMultiplier(), nr.allowWithFrameGeneration ? "allowed" : "not allowed") :
			                           std::string("off");
			if (run) {
				logger::info("[DLSSNR] state: on, {} at {}x{} (render {}x{}, output {}x{}), model {}% = {}x{}, network {}x{}{}, frame generation {}, video memory {} MB",
					before ? "before upscaling" : "after upscaling", g_status.width, g_status.height, renderWidth, renderHeight,
					outputWidth, outputHeight, modelPercent, g_status.workWidth, g_status.workHeight, g_status.paddedWidth,
					g_status.paddedHeight, pad ? " (padded to 8)" : " (no padding)", fg, vram);
				logger::info("[DLSSNR] state: input {}{}, tone preservation {:.2f}, tuning at creation {}",
					before ? (toneMatched ? "tone-matched" : "38a wrap") : "finished image",
					before ? std::format(" ({})", InputPrecisionName(inputPrecision)) : std::string(), toneStrength,
					nr.tuningAtCreate ? "on" : "off");
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
		const ResourceKey resourceKey{ before, g_status.width, g_status.height, modelPercent, pad, inputPrecision, toneOn };
		if (!g_haveResourceKey || !(resourceKey == g_lastResourceKey)) {
			// (batch 38c) A mode change after a failure tries again (a format the network refuses, say,
			// latches the pass off; switching back to another must not need the Reset button).
			if (Renderer::Instance().IsFailureLatched())
				ReleaseEverything("retrying after a failure: the mode changed");
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
		g_decision.pad = pad;
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

		auto input = MakeInput(g_decision, g_decision.width, g_decision.height);
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
			auto input = MakeInput(g_decision, std::min(g_decision.width, colorDesc.Width), std::min(g_decision.height, colorDesc.Height));
			input.sceneColor = color;
			// Depth after upscaling is Upscaling's output-resolution depth; the engine's vectors are
			// still at render resolution and get resampled onto the output grid in the guide pass.
			input.depthSRV = depth.depthSRV;
			input.motionSRV = motion.SRV;
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

	const char* ToneSourceText() { return g_toneSource; }

	json StatusJson()
	{
		const auto& renderer = Renderer::Instance();
		const auto& s = g_status;
		json o;
		o["enabled"] = s.enabled;
		o["running"] = s.running;
		o["blocked_reason"] = std::string(s.blockedReason);
		o["placement"] = s.beforeUpscaling ? "before-upscaling" : "after-upscaling";
		o["frame_generation"] = s.frameGeneration;
		o["extent"] = { s.width, s.height };
		o["render"] = { s.renderWidth, s.renderHeight };
		o["output"] = { s.outputWidth, s.outputHeight };
		o["model_percent"] = s.modelPercent;
		o["model_extent"] = { s.workWidth, s.workHeight };
		o["network_extent"] = { s.paddedWidth, s.paddedHeight };
		o["input"] = s.beforeUpscaling ? (s.toneMatched ? "tone-matched" : "38a wrap") : "finished image";
		o["input_precision"] = s.beforeUpscaling ? InputPrecisionName(s.inputPrecision) : "scene format";
		o["tone_source"] = std::string(g_toneSource);
		o["tone_preservation"] = s.toneStrength;
		o["tuning_at_creation"] = s.tuningAtCreate;
		o["route"] = std::string(renderer.LastRoute());
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
