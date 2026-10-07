// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#include "Renderer.h"

#include "D3D12Interop.h"
#include "Utils/D3D.h"
#include "Utils/GpuTimers.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <initializer_list>
#include <utility>
#include <vector>

#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	namespace
	{
		// (port) Open Shaders' Util::LazyShader has no counterpart in this tree. Same contract --
		// compile on first use, hold the result, drop it on Reset -- expressed through
		// Util::CompileShader, which is how every other feature here builds its compute shaders.
		// The failure latch matters because Get() is called every frame the pass runs: without it
		// a missing or broken shader file would attempt a fresh D3DCompileFromFile per frame.
		class LazyComputeShader
		{
		public:
			ID3D11ComputeShader* Get(const wchar_t* path,
				const std::vector<std::pair<const char*, const char*>>& defines,
				const char* profile, const char* entry, const char*)
			{
				if (!shader_ && !failed_) {
					auto* raw = reinterpret_cast<ID3D11ComputeShader*>(Util::CompileShader(path, defines, profile, entry));
					if (raw)
						shader_.Attach(raw);
					else
						failed_ = true;
				}
				return shader_.Get();
			}

			void Reset()
			{
				shader_.Reset();
				failed_ = false;
			}

		private:
			Microsoft::WRL::ComPtr<ID3D11ComputeShader> shader_;
			bool failed_ = false;
		};
	}

	namespace
	{
		bool GetTextureDesc(ID3D11Resource* resource, D3D11_TEXTURE2D_DESC& desc)
		{
			Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
			if (!resource || FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture))))
				return false;
			texture->GetDesc(&desc);
			return true;
		}

		bool Matches(const SharedTexture& texture, const D3D11_TEXTURE2D_DESC& desc)
		{
			return texture.resource11 && texture.desc.Width == desc.Width && texture.desc.Height == desc.Height &&
			       texture.desc.Format == desc.Format && texture.desc.ArraySize == desc.ArraySize &&
			       texture.desc.MipLevels == desc.MipLevels && texture.desc.SampleDesc.Count == desc.SampleDesc.Count;
		}

		D3D11_TEXTURE2D_DESC MakeSharedDesc(const D3D11_TEXTURE2D_DESC& source, std::uint32_t width,
			std::uint32_t height, UINT bindFlags)
		{
			auto desc = source;
			desc.Width = width;
			desc.Height = height;
			desc.MipLevels = 1;
			desc.ArraySize = 1;
			desc.SampleDesc.Count = 1;
			desc.SampleDesc.Quality = 0;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = bindFlags;
			desc.CPUAccessFlags = 0;
			desc.MiscFlags = 0;
			return desc;
		}
	}

	class Renderer::State
	{
	public:
		struct EyeResources
		{
			SharedTexture color;
			SharedTexture depth;
			SharedTexture motionVectors;
			SharedTexture output;
		};

		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
			ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning)
		{
			if (failureLatched || !device || !context || eyeIndex >= eyes.size() || !color || !depth || !depthSRV || !motionVectors)
				return false;
			Util::CpuPassScope evaluateScope("NeuralRendering::Evaluate");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (Runtime::Instance().Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;
			if (!EnsureResources(eyeIndex, color, depth, motionVectors, guideWidth, guideHeight, colorWidth, colorHeight))
				return LatchFailure("shared resource creation", interop.LastError());

			auto& eye = eyes[eyeIndex];
			context->CopyResource(eye.color.resource11.Get(), color);
			if (!CopyDepthGuide(context, depthSRV, eye.depth.uav11.Get(), guideWidth, guideHeight))
				return LatchFailure("depth guide conversion", E_FAIL);
			context->CopyResource(eye.motionVectors.resource11.Get(), motionVectors);

			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginD3D12(&commandList))
				return LatchFailure("BeginD3D12", interop.LastError());
			D3D12_RESOURCE_BARRIER barriers[4]{};
			ID3D12Resource* resources[4]{
				eye.color.resource12.Get(), eye.depth.resource12.Get(),
				eye.motionVectors.resource12.Get(), eye.output.resource12.Get()
			};
			for (std::size_t index = 0; index < std::size(barriers); ++index) {
				barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[index].Transition.pResource = resources[index];
				barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
				barriers[index].Transition.StateAfter = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
				                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
			const bool succeeded = Runtime::Instance().Execute(commandList, eyeIndex,
				eye.color.resource12.Get(), eye.depth.resource12.Get(), eye.motionVectors.resource12.Get(),
				eye.output.resource12.Get(), guideWidth, guideHeight, colorWidth, colorHeight,
				motionVectorScaleX, motionVectorScaleY, tuning, resetPending[eyeIndex]);
			for (auto& barrier : barriers)
				std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
			if (!interop.EndD3D12())
				return LatchFailure("EndD3D12", interop.LastError());
			if (!succeeded)
				return LatchFailure("Feature 18", static_cast<HRESULT>(Runtime::Instance().NgxResult()));

			context->CopyResource(color, eye.output.resource11.Get());
			resetPending[eyeIndex] = false;
			return true;
		}

		bool ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
			const std::array<StereoEyeInput, 2>& inputs,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning)
		{
			if (failureLatched || !device || !context || !color)
				return false;
			Util::CpuPassScope evaluateScope("NeuralRendering::EvaluateStereo");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (Runtime::Instance().Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;

			D3D11_TEXTURE2D_DESC colorDesc{};
			if (!GetTextureDesc(color, colorDesc))
				return false;
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				const auto& input = inputs[eyeIndex];
				if (!input.depth || !input.depthSRV || !input.motionVectors)
					return false;
				if (!EnsureResources(eyeIndex, color, input.depth, input.motionVectors,
						guideWidth, guideHeight, colorWidth, colorHeight))
					return LatchFailure("shared resource creation", interop.LastError());

				D3D11_BOX sourceBox{
					input.sourceX, input.sourceY, 0,
					input.sourceX + colorWidth, input.sourceY + colorHeight, 1
				};
				auto& eye = eyes[eyeIndex];
				context->CopySubresourceRegion(eye.color.resource11.Get(), 0, 0, 0, 0, color, 0, &sourceBox);
				if (!CopyDepthGuide(context, input.depthSRV, eye.depth.uav11.Get(), guideWidth, guideHeight))
					return LatchFailure("depth guide conversion", E_FAIL);
				context->CopyResource(eye.motionVectors.resource11.Get(), input.motionVectors);
			}

			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginD3D12(&commandList))
				return LatchFailure("BeginD3D12 stereo", interop.LastError());

			bool succeeded = true;
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				auto& eye = eyes[eyeIndex];
				D3D12_RESOURCE_BARRIER barriers[4]{};
				ID3D12Resource* resources[4]{
					eye.color.resource12.Get(), eye.depth.resource12.Get(),
					eye.motionVectors.resource12.Get(), eye.output.resource12.Get()
				};
				for (std::size_t index = 0; index < std::size(barriers); ++index) {
					barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
					barriers[index].Transition.pResource = resources[index];
					barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
					barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
					barriers[index].Transition.StateAfter = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
					                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
				}
				commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
				const auto& input = inputs[eyeIndex];
				const bool eyeSucceeded = Runtime::Instance().Execute(commandList, eyeIndex,
					eye.color.resource12.Get(), eye.depth.resource12.Get(), eye.motionVectors.resource12.Get(),
					eye.output.resource12.Get(), guideWidth, guideHeight, colorWidth, colorHeight,
					input.motionVectorScaleX, input.motionVectorScaleY, tuning, resetPending[eyeIndex]);
				for (auto& barrier : barriers)
					std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
				commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
				if (!eyeSucceeded) {
					succeeded = false;
					break;
				}
			}

			if (!interop.EndD3D12())
				return LatchFailure("EndD3D12 stereo", interop.LastError());
			if (!succeeded)
				return LatchFailure("Feature 18 stereo", static_cast<HRESULT>(Runtime::Instance().NgxResult()));

			D3D11_BOX outputBox{ 0, 0, 0, colorWidth, colorHeight, 1 };
			for (std::uint32_t eyeIndex = 0; eyeIndex < inputs.size(); ++eyeIndex) {
				const auto& input = inputs[eyeIndex];
				context->CopySubresourceRegion(color, 0, input.sourceX, input.sourceY, 0,
					eyes[eyeIndex].output.resource11.Get(), 0, &outputBox);
				resetPending[eyeIndex] = false;
			}
			return true;
		}

		void Reset(bool unloadRuntime = true)
		{
			interop.WaitForIdle();
			Runtime::Instance().Shutdown(unloadRuntime);
			// (batch 38a) Textures before the device, so what is left holding the device is not ours.
			eyes = {};
			workingSet = {};
			const bool hadDevice = interop.IsInitialized();
			interop.Shutdown();
			if (hadDevice)
				logger::info("[DLSSNR] D3D12 device teardown: {} other reference(s) left (0 = all its video memory is returned)",
					interop.LastShutdownOtherReferences());
			resetPending = { true, true };
			failureLatched = false;
			copyDepthGuideCS.Reset();
			prepareGuidesCS.Reset();
			prepareColorCS.Reset();
			finalizeCS.Reset();
			toneStatsCS.Reset();
			toneCurveCS.Reset();
			linearClamp.Reset();
			lastRoute = "";
		}

		void ResetHistory()
		{
			interop.WaitForIdle();
			Runtime::Instance().ResetFeatures();
			resetPending = { true, true };
		}

		// ---- (batch 38a) ------------------------------------------------------------------

		bool Run(ID3D11Device* device, ID3D11DeviceContext* context, const Renderer::PassInput& in)
		{
			const bool before = in.placement == Renderer::Placement::BeforeUpscaling;
			if (failureLatched || !device || !context || !in.sceneColor || !in.depthSRV || !in.motionSRV ||
				in.width == 0 || in.height == 0 || in.motionSourceWidth == 0 || in.motionSourceHeight == 0 ||
				in.workWidth == 0 || in.workHeight == 0 || in.workWidth > in.width || in.workHeight > in.height ||
				in.paddedWidth < in.workWidth || in.paddedHeight < in.workHeight ||
				(before && !in.sceneColorSRV))
				return false;
			Util::CpuPassScope evaluateScope("NeuralRendering::Evaluate");

			if (!interop.IsInitialized() && !InitializeInterop(device, context))
				return false;
			if (Runtime::Instance().Status() != RuntimeStatus::Initialized && !InitializeRuntime())
				return false;

			D3D11_TEXTURE2D_DESC sceneDesc{}, motionDesc{};
			Microsoft::WRL::ComPtr<ID3D11Resource> motionResource;
			in.motionSRV->GetResource(&motionResource);
			if (!GetTextureDesc(in.sceneColor, sceneDesc) || !GetTextureDesc(motionResource.Get(), motionDesc))
				return LatchFailure("texture description", E_INVALIDARG);
			if (in.width > sceneDesc.Width || in.height > sceneDesc.Height)
				return LatchFailure("extent larger than the scene", E_INVALIDARG);

			// (batch 38c) After upscaling, at the frame's own size, unpadded and without tone
			// preservation, the colour goes in and out by plain copies exactly as in 37c / 38a.
			// Everything else goes through PrepareColorCS and FinalizeCS.
			const bool direct = !before && in.workWidth == in.width && in.workHeight == in.height &&
			                    in.paddedWidth == in.width && in.paddedHeight == in.height && in.toneStrength <= 0.0f;
			if (!EnsureWorkingSet(in, sceneDesc, motionDesc.Format, direct))
				return LatchFailure("shared resource creation", interop.LastError());

			// A feature that has to be replaced -- one 38a made with a model-resolution ratio, or (batch
			// 38c) one whose tuning, read only at creation, is no longer what the menu says -- is
			// released here, once the queue has drained, rather than by Runtime::Execute from inside a
			// command list while earlier frames may still be using it on the GPU.
			auto& runtime = Runtime::Instance();
			const float liveScale = runtime.FeatureScalingRatio(0);
			const bool tuningStale = !runtime.FeatureTuningMatches(0, in.tuning, in.tuningAtCreate);
			if ((liveScale > 0.0f && liveScale != 1.0f) || tuningStale) {
				if (!interop.WaitForIdle())
					return LatchFailure("WaitForIdle before rebuilding the feature", interop.LastError());
				runtime.ResetFeature(0);
				resetPending[0] = true;
				if (tuningStale)
					logger::info("[DLSSNR] tuning changed (intensity {:.2f}, local tone {:.2f}, local structure {:.2f}, skin {:.2f}, style {}): rebuilding the feature",
						in.tuning.intensity, in.tuning.localToneStrength, in.tuning.localStructureStrength,
						in.tuning.skinStructureStrength, in.tuning.style);
			}

			auto& ws = workingSet;
			if (!PrepareGuides(context, in))
				return LatchFailure("guide preparation", E_FAIL);

			const D3D11_BOX box{ 0, 0, 0, in.width, in.height, 1 };
			ID3D11ShaderResourceView* originalSRV = before ? in.sceneColorSRV : ws.sceneCopySRV.Get();
			if (direct) {
				context->CopySubresourceRegion(ws.color.resource11.Get(), 0, 0, 0, 0, in.sceneColor, 0, &box);
			} else {
				if (!before)
					context->CopySubresourceRegion(ws.sceneCopy.Get(), 0, 0, 0, 0, in.sceneColor, 0, &box);
				if (in.encode == Renderer::Encode::Curve && !BuildToneCurve(context, in.toneCurve))
					return LatchFailure("tone curve", E_FAIL);
				const std::uint32_t sourceWidth = before ? sceneDesc.Width : in.width;
				const std::uint32_t sourceHeight = before ? sceneDesc.Height : in.height;
				if (!PrepareColor(context, in, originalSRV, sourceWidth, sourceHeight))
					return LatchFailure("colour preparation", E_FAIL);
			}

			ID3D12GraphicsCommandList* commandList = nullptr;
			if (!interop.BeginD3D12(&commandList, D3D12Interop::kCommandContextCount))
				return LatchFailure("BeginD3D12", interop.LastError());
			D3D12_RESOURCE_BARRIER barriers[4]{};
			ID3D12Resource* resources[4]{
				ws.color.resource12.Get(), ws.depth.resource12.Get(),
				ws.motionVectors.resource12.Get(), ws.output.resource12.Get()
			};
			for (std::size_t index = 0; index < std::size(barriers); ++index) {
				barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barriers[index].Transition.pResource = resources[index];
				barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
				barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
				barriers[index].Transition.StateAfter = index == 3 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS :
				                                                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
			}
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
			// Colour and guides share the network extent, padding included, so the network reads no
			// margin it was not given. The vectors are in normalised screen units and the model wants
			// pixels of the content it is looking at: the content extent, not the padded one.
			const bool succeeded = runtime.Execute(commandList, 0,
				ws.color.resource12.Get(), ws.depth.resource12.Get(), ws.motionVectors.resource12.Get(),
				ws.output.resource12.Get(), in.paddedWidth, in.paddedHeight, in.paddedWidth, in.paddedHeight,
				static_cast<float>(in.workWidth), static_cast<float>(in.workHeight), in.tuning,
				in.reset || resetPending[0], 1.0f, in.tuningAtCreate);
			for (auto& barrier : barriers)
				std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			commandList->ResourceBarrier(static_cast<UINT>(std::size(barriers)), barriers);
			if (!interop.EndD3D12())
				return LatchFailure("EndD3D12", interop.LastError());
			if (!succeeded)
				return LatchFailure("Feature 18", static_cast<HRESULT>(runtime.NgxResult()));

			// EndD3D12 queued a GPU-side wait on the D3D11 timeline, so everything below -- and
			// everything the frame does after it: DLSS super resolution, the HUD-less copy frame
			// generation reads, the backbuffer -- sees the finished network output.
			if (direct) {
				context->CopySubresourceRegion(in.sceneColor, 0, 0, 0, 0, ws.output.resource11.Get(), 0, &box);
				lastRoute = "direct copy";
			} else {
				if (in.toneStrength > 0.0f && !Finalize(context, in, originalSRV, true))
					return LatchFailure("tone statistics", E_FAIL);
				if (!Finalize(context, in, originalSRV, false))
					return LatchFailure("finalize", E_FAIL);
				context->CopySubresourceRegion(in.sceneColor, 0, 0, 0, 0, ws.finalScratch.Get(), 0, &box);
				lastRoute = "prepare + finalize";
			}
			resetPending[0] = false;
			return true;
		}

		void ReleaseWorkingSet()
		{
			interop.WaitForIdle();
			Runtime::Instance().ResetFeature(0);
			workingSet = {};
			// The 37c path's textures too: whichever path runs next rebuilds what it needs.
			eyes = {};
			resetPending = { true, true };
		}

		[[nodiscard]] bool IsInitialized() const { return interop.IsInitialized(); }
		[[nodiscard]] float ModelGpuMs() const { return interop.SmoothedGpuMs(); }
		[[nodiscard]] float LastModelGpuMs() const { return interop.LastGpuMs(); }
		[[nodiscard]] std::uint32_t MaxInFlight() const { return interop.MaxInFlight(); }
		[[nodiscard]] std::uint32_t BackpressureWaits() const { return interop.BackpressureWaits(); }
		[[nodiscard]] const char* LastRoute() const { return lastRoute; }

		[[nodiscard]] bool IsFailureLatched() const { return failureLatched; }

	private:
		// (batch 38c) Grid the tone statistics are averaged over: 18 cells high, as many wide as keeps
		// them square.
		static constexpr std::uint32_t kToneGridHeight = 18;
		static constexpr std::uint32_t kToneCurveEntries = 257;  // Common.hlsli: kCurveSize + 1

		// (batch 38a) Everything the Batch 38 pass owns. Kept apart from `eyes`, which stays the 37c
		// path's, so the master switch can flip between the two without either one reusing a
		// texture the other sized.
		struct WorkingSet
		{
			// Shared with the network, at the network extent (padded).
			SharedTexture color;
			SharedTexture depth;
			SharedTexture motionVectors;
			SharedTexture output;
			// (batch 38c) Frame extent, scene format: FinalizeCS writes here, then it is copied into the
			// scene (38a's decode scratch).
			Microsoft::WRL::ComPtr<ID3D11Texture2D> finalScratch;
			Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> finalScratchUAV;
			// (batch 38c) After upscaling, shader route: the frame as it was, readable (the bound target
			// may not be).
			Microsoft::WRL::ComPtr<ID3D11Texture2D> sceneCopy;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sceneCopySRV;
			// (batch 38c) The tone curve (ToneCurveCS.hlsl) and the tone statistics grid.
			Microsoft::WRL::ComPtr<ID3D11Buffer> curve;
			Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> curveSRV;
			Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> curveUAV;
			std::array<Microsoft::WRL::ComPtr<ID3D11Texture2D>, 2> stats;
			std::array<Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>, 2> statsSRV;
			std::array<Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView>, 2> statsUAV;
			std::uint32_t gridWidth = 0;
			std::uint32_t gridHeight = 0;

			Renderer::Placement placement = Renderer::Placement::AfterUpscaling;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			std::uint32_t workWidth = 0;
			std::uint32_t workHeight = 0;
			std::uint32_t paddedWidth = 0;
			std::uint32_t paddedHeight = 0;
			bool direct = false;
			DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT networkFormat = DXGI_FORMAT_UNKNOWN;
		};

		// Layouts match the cbuffers in PrepareGuidesCS.hlsl, PrepareColorCS.hlsl, ToneCurveCS.hlsl
		// and FinalizeCS.hlsl.
		struct PrepareGuidesConstants
		{
			float motionSourceScale[2];
			float motionOffset[2];
			std::uint32_t extent[2];
			std::uint32_t motionSourceMax[2];
			float depthSourceScale[2];
			std::uint32_t contentExtent[2];
			std::uint32_t depthSourceMax[2];
			std::uint32_t pad[2];
		};

		struct PrepareColorConstants
		{
			std::uint32_t destinationExtent[2];
			std::uint32_t contentExtent[2];
			std::uint32_t sourceExtent[2];
			float sourceTexelSize[2];
			std::uint32_t encodeMode;
			std::uint32_t resample;
			std::uint32_t pad[2];
		};

		struct ToneCurveConstants
		{
			float tint[4];
			float inputGamma;
			float outputGamma;
			float cinematicBrightness;
			float cinematicContrast;
			float exposureCompensation;
			float adaptationMin;
			float adaptationMax;
			float fixedExposure;
			std::uint32_t useGrading;
			std::uint32_t useAutoExposure;
			std::uint32_t gammaCorrect;
			std::uint32_t pad;
		};

		struct FinalizeConstants
		{
			std::uint32_t fullExtent[2];
			std::uint32_t workExtent[2];
			std::uint32_t encodeMode;
			std::uint32_t scaled;
			std::uint32_t toneEnabled;
			std::uint32_t displayEncoded;
			float toneStrength;
			float depthSigma;
			float cameraNear;
			float cameraFar;
			std::uint32_t grid[2];
			float gridTexel[2];
		};

		static_assert(sizeof(PrepareGuidesConstants) % 16 == 0);
		static_assert(sizeof(PrepareColorConstants) % 16 == 0);
		static_assert(sizeof(ToneCurveConstants) % 16 == 0);
		static_assert(sizeof(FinalizeConstants) % 16 == 0);

		bool CreatePlainTexture(const D3D11_TEXTURE2D_DESC& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture, const char* name)
		{
			const HRESULT result = interop.device11_->CreateTexture2D(&desc, nullptr, &texture);
			if (FAILED(result)) {
				interop.RecordFailure(result);
				return false;
			}
			Util::SetResourceName(texture.Get(), name);
			return true;
		}

		bool EnsureWorkingSet(const Renderer::PassInput& in, const D3D11_TEXTURE2D_DESC& sceneDesc, DXGI_FORMAT motionFormat, bool direct)
		{
			const bool before = in.placement == Renderer::Placement::BeforeUpscaling;
			// Before upscaling the network reads the wrapped HDR image in the chosen format. After
			// upscaling it reads the finished frame in the target's own format, as in 37c.
			const DXGI_FORMAT networkFormat = before ? in.networkFormat : sceneDesc.Format;
			auto& ws = workingSet;
			if (ws.color.resource11 && ws.placement == in.placement && ws.width == in.width && ws.height == in.height &&
				ws.workWidth == in.workWidth && ws.workHeight == in.workHeight && ws.paddedWidth == in.paddedWidth &&
				ws.paddedHeight == in.paddedHeight && ws.direct == direct && ws.sceneFormat == sceneDesc.Format &&
				ws.motionFormat == motionFormat && ws.networkFormat == networkFormat)
				return true;

			if (!interop.WaitForIdle())
				return false;
			Runtime::Instance().ResetFeature(0);
			ws = {};

			const UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto colorDesc = MakeSharedDesc(sceneDesc, in.paddedWidth, in.paddedHeight, sharedFlags);
			colorDesc.Format = networkFormat;
			auto depthDesc = MakeSharedDesc(sceneDesc, in.paddedWidth, in.paddedHeight, sharedFlags);
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			auto motionDesc = MakeSharedDesc(sceneDesc, in.paddedWidth, in.paddedHeight, sharedFlags);
			motionDesc.Format = motionFormat;
			if (!interop.CreateSharedTexture(colorDesc, ws.color, "NeuralRendering::Color") ||
				!interop.CreateSharedTexture(colorDesc, ws.output, "NeuralRendering::Output") ||
				!interop.CreateSharedTexture(depthDesc, ws.depth, "NeuralRendering::Depth") ||
				!interop.CreateSharedTexture(motionDesc, ws.motionVectors, "NeuralRendering::Motion")) {
				ws = {};
				return false;
			}

			if (!direct) {
				ID3D11Device* device11 = interop.device11_.Get();
				if (!ws.color.srv11 || !ws.output.srv11 || !ws.depth.srv11) {
					ws = {};
					interop.RecordFailure(E_FAIL);
					return false;
				}

				// The finished result, in the scene's own format so it copies straight back. Before
				// upscaling the view takes the format the scene's own SRV reads it as, in case the
				// texture is typeless.
				DXGI_FORMAT viewFormat = sceneDesc.Format;
				if (before) {
					D3D11_SHADER_RESOURCE_VIEW_DESC sceneViewDesc{};
					in.sceneColorSRV->GetDesc(&sceneViewDesc);
					viewFormat = sceneViewDesc.Format;
				}
				auto scratchDesc = MakeSharedDesc(sceneDesc, in.width, in.height, D3D11_BIND_UNORDERED_ACCESS);
				D3D11_UNORDERED_ACCESS_VIEW_DESC scratchViewDesc{};
				scratchViewDesc.Format = viewFormat;
				scratchViewDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				HRESULT result = S_OK;
				if (!CreatePlainTexture(scratchDesc, ws.finalScratch, "NeuralRendering::FinalScratch") ||
					FAILED(result = device11->CreateUnorderedAccessView(ws.finalScratch.Get(), &scratchViewDesc, &ws.finalScratchUAV))) {
					if (FAILED(result))
						interop.RecordFailure(result);
					ws = {};
					return false;
				}

				if (!before) {
					auto copyDesc = MakeSharedDesc(sceneDesc, in.width, in.height, D3D11_BIND_SHADER_RESOURCE);
					if (!CreatePlainTexture(copyDesc, ws.sceneCopy, "NeuralRendering::SceneCopy") ||
						FAILED(result = device11->CreateShaderResourceView(ws.sceneCopy.Get(), nullptr, &ws.sceneCopySRV))) {
						if (FAILED(result))
							interop.RecordFailure(result);
						ws = {};
						return false;
					}
				}

				D3D11_BUFFER_DESC curveDesc{};
				curveDesc.ByteWidth = static_cast<UINT>(sizeof(float) * 4 * kToneCurveEntries);
				curveDesc.Usage = D3D11_USAGE_DEFAULT;
				curveDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
				curveDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
				curveDesc.StructureByteStride = sizeof(float) * 4;
				D3D11_SHADER_RESOURCE_VIEW_DESC curveSRVDesc{};
				curveSRVDesc.Format = DXGI_FORMAT_UNKNOWN;
				curveSRVDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
				curveSRVDesc.Buffer.NumElements = kToneCurveEntries;
				D3D11_UNORDERED_ACCESS_VIEW_DESC curveUAVDesc{};
				curveUAVDesc.Format = DXGI_FORMAT_UNKNOWN;
				curveUAVDesc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
				curveUAVDesc.Buffer.NumElements = kToneCurveEntries;
				if (FAILED(result = device11->CreateBuffer(&curveDesc, nullptr, &ws.curve)) ||
					FAILED(result = device11->CreateShaderResourceView(ws.curve.Get(), &curveSRVDesc, &ws.curveSRV)) ||
					FAILED(result = device11->CreateUnorderedAccessView(ws.curve.Get(), &curveUAVDesc, &ws.curveUAV))) {
					interop.RecordFailure(result);
					ws = {};
					return false;
				}
				Util::SetResourceName(ws.curve.Get(), "NeuralRendering::ToneCurve");

				ws.gridHeight = kToneGridHeight;
				ws.gridWidth = std::clamp<std::uint32_t>(static_cast<std::uint32_t>(std::lround(
															 static_cast<double>(kToneGridHeight) * in.width / in.height)),
					1u, 64u);
				D3D11_TEXTURE2D_DESC statsDesc = MakeSharedDesc(sceneDesc, ws.gridWidth, ws.gridHeight, sharedFlags);
				statsDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
				for (std::size_t index = 0; index < ws.stats.size(); ++index) {
					if (!CreatePlainTexture(statsDesc, ws.stats[index], index == 0 ? "NeuralRendering::ToneStatsOriginal" : "NeuralRendering::ToneStatsResult") ||
						FAILED(result = device11->CreateShaderResourceView(ws.stats[index].Get(), nullptr, &ws.statsSRV[index])) ||
						FAILED(result = device11->CreateUnorderedAccessView(ws.stats[index].Get(), nullptr, &ws.statsUAV[index]))) {
						if (FAILED(result))
							interop.RecordFailure(result);
						ws = {};
						return false;
					}
				}
			}

			ws.placement = in.placement;
			ws.width = in.width;
			ws.height = in.height;
			ws.workWidth = in.workWidth;
			ws.workHeight = in.workHeight;
			ws.paddedWidth = in.paddedWidth;
			ws.paddedHeight = in.paddedHeight;
			ws.direct = direct;
			ws.sceneFormat = sceneDesc.Format;
			ws.motionFormat = motionFormat;
			ws.networkFormat = networkFormat;
			resetPending = { true, true };
			// (batch 38c) The exact extents, every rebuild: frame, model content, network (with the
			// padding), and the scale the motion vectors are given in.
			logger::info("[DLSSNR] extents: placement={} frame={}x{} model={}x{} ({}%) network={}x{} (padding +{} +{}) "
						 "mvScale={}x{} route={} sceneFormat={} networkInputFormat={}",
				before ? "before-upscaling" : "after-upscaling", in.width, in.height, in.workWidth, in.workHeight,
				std::lround(100.0 * in.workWidth / in.width), in.paddedWidth, in.paddedHeight,
				in.paddedWidth - in.workWidth, in.paddedHeight - in.workHeight, in.workWidth, in.workHeight,
				direct ? "direct copy" : "prepare + finalize", static_cast<std::uint32_t>(sceneDesc.Format),
				static_cast<std::uint32_t>(networkFormat));
			return true;
		}

		/// Binds and unbinds one compute dispatch. Up to 8 SRVs and 2 UAVs, one sampler, one cbuffer.
		void Dispatch(ID3D11DeviceContext* context, ID3D11ComputeShader* shader, ID3D11Buffer* cb,
			std::initializer_list<ID3D11ShaderResourceView*> srvs, std::initializer_list<ID3D11UnorderedAccessView*> uavs,
			bool sampler, UINT groupsX, UINT groupsY)
		{
			ID3D11ShaderResourceView* srvArray[8]{};
			ID3D11UnorderedAccessView* uavArray[2]{};
			UINT srvCount = 0;
			UINT uavCount = 0;
			for (auto* srv : srvs)
				srvArray[srvCount++] = srv;
			for (auto* uav : uavs)
				uavArray[uavCount++] = uav;
			ID3D11SamplerState* samplerState = sampler ? linearClamp.Get() : nullptr;
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &cb);
			if (srvCount)
				context->CSSetShaderResources(0, srvCount, srvArray);
			context->CSSetUnorderedAccessViews(0, uavCount, uavArray, nullptr);
			if (sampler)
				context->CSSetSamplers(0, 1, &samplerState);
			context->Dispatch(groupsX, groupsY, 1);
			ID3D11ShaderResourceView* nullSRVs[8]{};
			ID3D11UnorderedAccessView* nullUAVs[2]{};
			ID3D11Buffer* nullCB = nullptr;
			ID3D11SamplerState* nullSampler = nullptr;
			if (srvCount)
				context->CSSetShaderResources(0, srvCount, nullSRVs);
			context->CSSetUnorderedAccessViews(0, uavCount, nullUAVs, nullptr);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			if (sampler)
				context->CSSetSamplers(0, 1, &nullSampler);
			context->CSSetShader(nullptr, nullptr, 0);
		}

		bool EnsureSampler()
		{
			if (linearClamp)
				return true;
			D3D11_SAMPLER_DESC desc{};
			desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
			desc.MaxLOD = D3D11_FLOAT32_MAX;
			desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
			return SUCCEEDED(interop.device11_->CreateSamplerState(&desc, &linearClamp));
		}

		bool PrepareGuides(ID3D11DeviceContext* context, const Renderer::PassInput& in)
		{
			auto* shader = prepareGuidesCS.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\PrepareGuidesCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::PrepareGuidesCS");
			if (!shader)
				return false;
			if (!guidesCB)
				guidesCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PrepareGuidesConstants>());
			// Content texel centre -> source texel. Depth sits 1:1 with the frame; the vectors at render
			// resolution. At 100% the depth scale is 1 and the read is 38a's 1:1.
			PrepareGuidesConstants constants{};
			constants.motionSourceScale[0] = static_cast<float>(in.motionSourceWidth) / static_cast<float>(in.workWidth);
			constants.motionSourceScale[1] = static_cast<float>(in.motionSourceHeight) / static_cast<float>(in.workHeight);
			constants.motionOffset[0] = in.motionOffsetX;
			constants.motionOffset[1] = in.motionOffsetY;
			constants.extent[0] = in.paddedWidth;
			constants.extent[1] = in.paddedHeight;
			constants.motionSourceMax[0] = in.motionSourceWidth - 1;
			constants.motionSourceMax[1] = in.motionSourceHeight - 1;
			constants.depthSourceScale[0] = static_cast<float>(in.width) / static_cast<float>(in.workWidth);
			constants.depthSourceScale[1] = static_cast<float>(in.height) / static_cast<float>(in.workHeight);
			constants.contentExtent[0] = in.workWidth;
			constants.contentExtent[1] = in.workHeight;
			constants.depthSourceMax[0] = in.width - 1;
			constants.depthSourceMax[1] = in.height - 1;
			guidesCB->Update(constants);

			Dispatch(context, shader, guidesCB->CB(), { in.depthSRV, in.motionSRV },
				{ workingSet.depth.uav11.Get(), workingSet.motionVectors.uav11.Get() }, false,
				(in.paddedWidth + 7) / 8, (in.paddedHeight + 7) / 8);
			return true;
		}

		bool BuildToneCurve(ID3D11DeviceContext* context, const Renderer::ToneCurveSource& source)
		{
			auto* shader = toneCurveCS.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\ToneCurveCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::ToneCurveCS");
			if (!shader || !workingSet.curveUAV || !EnsureSampler())
				return false;
			if (!curveCB)
				curveCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ToneCurveConstants>());
			ToneCurveConstants constants{};
			std::copy(std::begin(source.tint), std::end(source.tint), constants.tint);
			constants.inputGamma = source.inputGamma;
			constants.outputGamma = source.outputGamma;
			constants.cinematicBrightness = source.cinematicBrightness;
			constants.cinematicContrast = source.cinematicContrast;
			constants.exposureCompensation = source.exposureCompensation;
			constants.adaptationMin = source.adaptationMin;
			constants.adaptationMax = source.adaptationMax;
			constants.fixedExposure = source.fixedExposure;
			constants.useGrading = source.gradingLUT ? 1u : 0u;
			constants.useAutoExposure = source.adaptation ? 1u : 0u;
			constants.gammaCorrect = source.gammaCorrect ? 1u : 0u;
			curveCB->Update(constants);

			Dispatch(context, shader, curveCB->CB(), { source.gradingLUT, source.adaptation }, { workingSet.curveUAV.Get() }, true, 1, 1);
			return true;
		}

		bool PrepareColor(ID3D11DeviceContext* context, const Renderer::PassInput& in, ID3D11ShaderResourceView* source,
			std::uint32_t sourceTextureWidth, std::uint32_t sourceTextureHeight)
		{
			auto* shader = prepareColorCS.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\PrepareColorCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::PrepareColorCS");
			if (!shader || !source || !EnsureSampler())
				return false;
			if (!colorCB)
				colorCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PrepareColorConstants>());
			PrepareColorConstants constants{};
			constants.destinationExtent[0] = in.paddedWidth;
			constants.destinationExtent[1] = in.paddedHeight;
			constants.contentExtent[0] = in.workWidth;
			constants.contentExtent[1] = in.workHeight;
			constants.sourceExtent[0] = in.width;
			constants.sourceExtent[1] = in.height;
			constants.sourceTexelSize[0] = 1.0f / static_cast<float>(sourceTextureWidth);
			constants.sourceTexelSize[1] = 1.0f / static_cast<float>(sourceTextureHeight);
			constants.encodeMode = static_cast<std::uint32_t>(in.encode);
			constants.resample = (in.workWidth != in.width || in.workHeight != in.height) ? 1u : 0u;
			colorCB->Update(constants);

			Dispatch(context, shader, colorCB->CB(), { source, workingSet.curveSRV.Get() }, { workingSet.color.uav11.Get() }, true,
				(in.paddedWidth + 7) / 8, (in.paddedHeight + 7) / 8);
			return true;
		}

		/// @param stats true = the STATS variant (tone statistics grid), false = the frame pass.
		bool Finalize(ID3D11DeviceContext* context, const Renderer::PassInput& in, ID3D11ShaderResourceView* original, bool stats)
		{
			auto& lazy = stats ? toneStatsCS : finalizeCS;
			std::vector<std::pair<const char*, const char*>> defines;
			if (stats)
				defines.emplace_back("STATS", "");
			auto* shader = lazy.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\FinalizeCS.hlsl", defines, "cs_5_0",
				"main", stats ? "NeuralRendering::ToneStatsCS" : "NeuralRendering::FinalizeCS");
			auto& ws = workingSet;
			if (!shader || !original || !ws.finalScratchUAV || !ws.curveSRV || !ws.statsSRV[0] || !EnsureSampler())
				return false;
			if (!finalizeCB)
				finalizeCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<FinalizeConstants>());
			FinalizeConstants constants{};
			constants.fullExtent[0] = in.width;
			constants.fullExtent[1] = in.height;
			constants.workExtent[0] = in.workWidth;
			constants.workExtent[1] = in.workHeight;
			constants.encodeMode = static_cast<std::uint32_t>(in.encode);
			constants.scaled = (in.workWidth != in.width || in.workHeight != in.height) ? 1u : 0u;
			constants.toneEnabled = in.toneStrength > 0.0f ? 1u : 0u;
			constants.displayEncoded = in.placement == Renderer::Placement::AfterUpscaling ? 1u : 0u;
			constants.toneStrength = std::clamp(in.toneStrength, 0.0f, 1.0f);
			constants.depthSigma = 0.05f;
			constants.cameraNear = in.cameraNear;
			constants.cameraFar = std::max(in.cameraFar, in.cameraNear + 1.0f);
			constants.grid[0] = ws.gridWidth;
			constants.grid[1] = ws.gridHeight;
			constants.gridTexel[0] = 1.0f / static_cast<float>(ws.gridWidth);
			constants.gridTexel[1] = 1.0f / static_cast<float>(ws.gridHeight);
			finalizeCB->Update(constants);

			if (stats) {
				Dispatch(context, shader, finalizeCB->CB(),
					{ original, ws.color.srv11.Get(), ws.output.srv11.Get(), ws.curveSRV.Get(), in.depthSRV, ws.depth.srv11.Get() },
					{ ws.statsUAV[0].Get(), ws.statsUAV[1].Get() }, false, ws.gridWidth, ws.gridHeight);
			} else {
				Dispatch(context, shader, finalizeCB->CB(),
					{ original, ws.color.srv11.Get(), ws.output.srv11.Get(), ws.curveSRV.Get(), in.depthSRV, ws.depth.srv11.Get(),
						ws.statsSRV[0].Get(), ws.statsSRV[1].Get() },
					{ ws.finalScratchUAV.Get() }, true, (in.width + 7) / 8, (in.height + 7) / 8);
			}
			return true;
		}

		bool CopyDepthGuide(ID3D11DeviceContext* context, ID3D11ShaderResourceView* source,
			ID3D11UnorderedAccessView* destination, std::uint32_t width, std::uint32_t height)
		{
			auto* shader = copyDepthGuideCS.Get(
				L"Data\\Shaders\\Upscaling\\NeuralRendering\\CopyDepthGuideCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::CopyDepthGuideCS");
			if (!shader || !destination)
				return false;
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetShaderResources(0, 1, &source);
			context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRV = nullptr;
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			context->CSSetShaderResources(0, 1, &nullSRV);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool InitializeInterop(ID3D11Device* device, ID3D11DeviceContext* context)
		{
			Microsoft::WRL::ComPtr<IDXGIDevice> dxgiDevice;
			Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
			HRESULT result = device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
			if (SUCCEEDED(result)) result = dxgiDevice->GetAdapter(&adapter);
			if (FAILED(result) || !interop.Initialize(adapter.Get(), device, context))
				return LatchFailure("D3D12 interop initialization", FAILED(result) ? result : interop.LastError());
			return true;
		}

		bool InitializeRuntime()
		{
			auto& runtime = Runtime::Instance();
			// (batch 38a) A runtime still mapped from an earlier probe or a keep-module shutdown is reused
			// rather than unloaded and mapped again.
			if ((runtime.Status() != RuntimeStatus::Ready && !runtime.Probe()) || !runtime.Initialize(interop.Device()))
				return LatchFailure("runtime initialization", static_cast<HRESULT>(runtime.NgxResult()));
			logger::info("[DLSSNR] initialized version={} appId=0x{:08X} api=0x{:X}",
				runtime.Version(), runtime.ApplicationId(), runtime.ApiVersion());
			return true;
		}

		bool EnsureResources(std::uint32_t eyeIndex, ID3D11Resource* color, ID3D11Resource* depth,
			ID3D11Resource* motionVectors, std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight)
		{
			D3D11_TEXTURE2D_DESC colorSource{}, depthSource{}, motionSource{};
			if (!GetTextureDesc(color, colorSource) || !GetTextureDesc(depth, depthSource) ||
				!GetTextureDesc(motionVectors, motionSource))
				return false;
			const UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			auto colorDesc = MakeSharedDesc(colorSource, colorWidth, colorHeight, sharedFlags);
			auto outputDesc = MakeSharedDesc(colorSource, colorWidth, colorHeight, D3D11_BIND_UNORDERED_ACCESS);
			auto depthDesc = MakeSharedDesc(depthSource, guideWidth, guideHeight, sharedFlags);
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			auto motionDesc = MakeSharedDesc(motionSource, guideWidth, guideHeight, sharedFlags);
			auto& eye = eyes[eyeIndex];
			if (Matches(eye.color, colorDesc) && Matches(eye.output, outputDesc) &&
				Matches(eye.depth, depthDesc) && Matches(eye.motionVectors, motionDesc))
				return true;

			if (!interop.WaitForIdle())
				return false;
			Runtime::Instance().ResetFeature(eyeIndex);
			eye = {};
			const std::string suffix = eyeIndex == 0 ? "Left" : "Right";
			if (!interop.CreateSharedTexture(colorDesc, eye.color, ("NeuralRendering::Color" + suffix).c_str()) ||
				!interop.CreateSharedTexture(outputDesc, eye.output, ("NeuralRendering::Output" + suffix).c_str()) ||
				!interop.CreateSharedTexture(depthDesc, eye.depth, ("NeuralRendering::Depth" + suffix).c_str()) ||
				!interop.CreateSharedTexture(motionDesc, eye.motionVectors, ("NeuralRendering::Motion" + suffix).c_str()))
				return false;
			resetPending = { true, true };
			logger::info("[DLSSNR] resources eye={} guides={}x{} color={}x{}", eyeIndex, guideWidth, guideHeight, colorWidth, colorHeight);
			return true;
		}

		bool LatchFailure(const char* operation, HRESULT error)
		{
			failureLatched = true;
			logger::error("[DLSSNR] {} failed hr/ngx=0x{:08X} status={} detail={}",
				operation, static_cast<std::uint32_t>(error), ToString(Runtime::Instance().Status()), Runtime::Instance().Detail());
			return false;
		}

		D3D12Interop interop;
		LazyComputeShader copyDepthGuideCS;
		LazyComputeShader prepareGuidesCS;
		LazyComputeShader prepareColorCS;
		LazyComputeShader finalizeCS;
		LazyComputeShader toneStatsCS;
		LazyComputeShader toneCurveCS;
		Microsoft::WRL::ComPtr<ID3D11SamplerState> linearClamp;
		std::unique_ptr<ConstantBuffer> guidesCB;
		std::unique_ptr<ConstantBuffer> colorCB;
		std::unique_ptr<ConstantBuffer> curveCB;
		std::unique_ptr<ConstantBuffer> finalizeCB;
		const char* lastRoute = "";
		WorkingSet workingSet;
		std::array<EyeResources, 2> eyes;
		std::array<bool, 2> resetPending{ true, true };
		bool failureLatched = false;
	};

	Renderer::Renderer() : state_(new State()) {}
	Renderer::~Renderer() { delete state_; }
	Renderer& Renderer::Instance() { static Renderer instance; return instance; }

	bool Renderer::Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
		ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
		ID3D11Resource* motionVectors,
		std::uint32_t guideWidth, std::uint32_t guideHeight, std::uint32_t colorWidth, std::uint32_t colorHeight,
		float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning)
	{
		return state_->Apply(device, context, eyeIndex, color, depth, depthSRV, motionVectors,
			guideWidth, guideHeight, colorWidth, colorHeight, motionVectorScaleX, motionVectorScaleY, tuning);
	}

	bool Renderer::ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
		const std::array<StereoEyeInput, 2>& eyes,
		std::uint32_t guideWidth, std::uint32_t guideHeight,
		std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning)
	{
		return state_->ApplyStereo(device, context, color, eyes,
			guideWidth, guideHeight, colorWidth, colorHeight, tuning);
	}

	bool Renderer::Run(ID3D11Device* device, ID3D11DeviceContext* context, const PassInput& input) { return state_->Run(device, context, input); }
	void Renderer::ReleaseWorkingSet() { state_->ReleaseWorkingSet(); }
	bool Renderer::IsInitialized() const { return state_->IsInitialized(); }
	float Renderer::ModelGpuMs() const { return state_->ModelGpuMs(); }
	float Renderer::LastModelGpuMs() const { return state_->LastModelGpuMs(); }
	std::uint32_t Renderer::MaxInFlight() const { return state_->MaxInFlight(); }
	std::uint32_t Renderer::BackpressureWaits() const { return state_->BackpressureWaits(); }
	void Renderer::Reset(bool unloadRuntime) { state_->Reset(unloadRuntime); }
	void Renderer::ResetHistory() { state_->ResetHistory(); }
	const char* Renderer::LastRoute() const { return state_->LastRoute(); }
	bool Renderer::IsFailureLatched() const { return state_->IsFailureLatched(); }
	std::uint32_t Renderer::NgxResult() const { return Runtime::Instance().NgxResult(); }
	std::uint64_t Renderer::SuccessfulFrames() const { return Runtime::Instance().SuccessfulFrames(); }
	const char* Renderer::StatusText() const { return ToString(Runtime::Instance().Status()); }
}