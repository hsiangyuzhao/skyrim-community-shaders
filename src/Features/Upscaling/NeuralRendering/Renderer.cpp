// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#include "Renderer.h"

#include "D3D12Interop.h"
#include "Utils/D3D.h"
#include "Utils/GpuTimers.h"

#include <array>
#include <utility>

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
			encodeCS.Reset();
			decodeCS.Reset();
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
			if (!EnsureWorkingSet(in, sceneDesc, motionDesc.Format))
				return LatchFailure("shared resource creation", interop.LastError());

			// A model-resolution change keeps every texture but needs a new NGX feature. Runtime::Execute
			// would release the old one itself, but from inside a command list while earlier frames may
			// still be using it on the GPU; release it here, once the queue has drained, instead.
			auto& runtime = Runtime::Instance();
			const float liveScale = runtime.FeatureScalingRatio(0);
			if (liveScale > 0.0f && liveScale != in.modelScale) {
				if (!interop.WaitForIdle())
					return LatchFailure("WaitForIdle before model resolution change", interop.LastError());
				runtime.ResetFeature(0);
				resetPending[0] = true;
			}

			auto& ws = workingSet;
			if (!PrepareGuides(context, in))
				return LatchFailure("guide preparation", E_FAIL);

			const D3D11_BOX box{ 0, 0, 0, in.width, in.height, 1 };
			if (before) {
				if (!TransformColor(context, encodeCS, {}, in.sceneColorSRV, nullptr, nullptr, ws.color.uav11.Get(), in.width, in.height))
					return LatchFailure("colour encode", E_FAIL);
			} else {
				context->CopySubresourceRegion(ws.color.resource11.Get(), 0, 0, 0, 0, in.sceneColor, 0, &box);
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
			// Colour and guides share one extent, so the vectors are scaled to that extent's pixels:
			// the engine stores normalised screen units and the model wants pixels.
			const bool succeeded = runtime.Execute(commandList, 0,
				ws.color.resource12.Get(), ws.depth.resource12.Get(), ws.motionVectors.resource12.Get(),
				ws.output.resource12.Get(), in.width, in.height, in.width, in.height,
				static_cast<float>(in.width), static_cast<float>(in.height), in.tuning,
				in.reset || resetPending[0], in.modelScale);
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
			if (before) {
				if (!TransformColor(context, decodeCS, { { "DECODE", "" } }, in.sceneColorSRV, ws.color.srv11.Get(),
						ws.output.srv11.Get(), ws.decodeScratchUAV.Get(), in.width, in.height))
					return LatchFailure("colour decode", E_FAIL);
				context->CopySubresourceRegion(in.sceneColor, 0, 0, 0, 0, ws.decodeScratch.Get(), 0, &box);
			} else {
				context->CopySubresourceRegion(in.sceneColor, 0, 0, 0, 0, ws.output.resource11.Get(), 0, &box);
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

		[[nodiscard]] bool IsFailureLatched() const { return failureLatched; }

	private:
		// (batch 38a) Everything the Batch 38 pass owns. Kept apart from `eyes`, which stays the 37c
		// path's, so the master switch can flip between the two without either one reusing a
		// texture the other sized.
		struct WorkingSet
		{
			SharedTexture color;
			SharedTexture depth;
			SharedTexture motionVectors;
			SharedTexture output;
			Microsoft::WRL::ComPtr<ID3D11Texture2D> decodeScratch;
			Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> decodeScratchUAV;
			Renderer::Placement placement = Renderer::Placement::AfterUpscaling;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			DXGI_FORMAT sceneFormat = DXGI_FORMAT_UNKNOWN;
			DXGI_FORMAT motionFormat = DXGI_FORMAT_UNKNOWN;
		};

		// Layouts match the cbuffers in PrepareGuidesCS.hlsl and ColorTransformCS.hlsl.
		struct PrepareGuidesConstants
		{
			float motionSourceScale[2];
			float motionOffset[2];
			std::uint32_t extent[2];
			std::uint32_t motionSourceMax[2];
		};

		struct ColorTransformConstants
		{
			std::uint32_t extent[2];
			std::uint32_t pad[2];
		};

		bool EnsureWorkingSet(const Renderer::PassInput& in, const D3D11_TEXTURE2D_DESC& sceneDesc, DXGI_FORMAT motionFormat)
		{
			auto& ws = workingSet;
			if (ws.color.resource11 && ws.placement == in.placement && ws.width == in.width && ws.height == in.height &&
				ws.sceneFormat == sceneDesc.Format && ws.motionFormat == motionFormat)
				return true;

			if (!interop.WaitForIdle())
				return false;
			Runtime::Instance().ResetFeature(0);
			ws = {};

			const bool before = in.placement == Renderer::Placement::BeforeUpscaling;
			const UINT sharedFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			// Before upscaling the network reads the encoded RGBA8 image (see ColorTransformCS.hlsl).
			// After upscaling it reads the finished frame in the target's own format, as in 37c.
			auto colorDesc = MakeSharedDesc(sceneDesc, in.width, in.height, sharedFlags);
			if (before)
				colorDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			auto depthDesc = MakeSharedDesc(sceneDesc, in.width, in.height, sharedFlags);
			depthDesc.Format = DXGI_FORMAT_R32_FLOAT;
			auto motionDesc = MakeSharedDesc(sceneDesc, in.width, in.height, sharedFlags);
			motionDesc.Format = motionFormat;
			if (!interop.CreateSharedTexture(colorDesc, ws.color, "NeuralRendering::Color") ||
				!interop.CreateSharedTexture(colorDesc, ws.output, "NeuralRendering::Output") ||
				!interop.CreateSharedTexture(depthDesc, ws.depth, "NeuralRendering::Depth") ||
				!interop.CreateSharedTexture(motionDesc, ws.motionVectors, "NeuralRendering::Motion")) {
				ws = {};
				return false;
			}
			if (before && (!ws.color.srv11 || !ws.output.srv11)) {
				ws = {};
				interop.RecordFailure(E_FAIL);
				return false;
			}
			if (before) {
				// The decoded HDR result, in the scene's own format so it copies straight back. The view
				// takes the format the scene's own SRV reads it as, in case the texture is typeless.
				auto scratchDesc = MakeSharedDesc(sceneDesc, in.width, in.height, D3D11_BIND_UNORDERED_ACCESS);
				D3D11_SHADER_RESOURCE_VIEW_DESC sceneViewDesc{};
				in.sceneColorSRV->GetDesc(&sceneViewDesc);
				D3D11_UNORDERED_ACCESS_VIEW_DESC scratchViewDesc{};
				scratchViewDesc.Format = sceneViewDesc.Format;
				scratchViewDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
				scratchViewDesc.Texture2D.MipSlice = 0;
				ID3D11Device* device11 = interop.device11_.Get();
				HRESULT result = device11->CreateTexture2D(&scratchDesc, nullptr, &ws.decodeScratch);
				if (SUCCEEDED(result))
					result = device11->CreateUnorderedAccessView(ws.decodeScratch.Get(), &scratchViewDesc, &ws.decodeScratchUAV);
				if (FAILED(result)) {
					ws = {};
					interop.RecordFailure(result);
					return false;
				}
				Util::SetResourceName(ws.decodeScratch.Get(), "NeuralRendering::DecodeScratch");
			}
			ws.placement = in.placement;
			ws.width = in.width;
			ws.height = in.height;
			ws.sceneFormat = sceneDesc.Format;
			ws.motionFormat = motionFormat;
			resetPending = { true, true };
			logger::info("[DLSSNR] resources placement={} extent={}x{} sceneFormat={} networkInputFormat={}",
				before ? "before-upscaling" : "after-upscaling", in.width, in.height,
				static_cast<std::uint32_t>(sceneDesc.Format), static_cast<std::uint32_t>(colorDesc.Format));
			return true;
		}

		bool PrepareGuides(ID3D11DeviceContext* context, const Renderer::PassInput& in)
		{
			auto* shader = prepareGuidesCS.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\PrepareGuidesCS.hlsl", {}, "cs_5_0",
				"main", "NeuralRendering::PrepareGuidesCS");
			if (!shader)
				return false;
			if (!guidesCB)
				guidesCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<PrepareGuidesConstants>());
			PrepareGuidesConstants constants{};
			constants.motionSourceScale[0] = static_cast<float>(in.motionSourceWidth) / static_cast<float>(in.width);
			constants.motionSourceScale[1] = static_cast<float>(in.motionSourceHeight) / static_cast<float>(in.height);
			constants.motionOffset[0] = in.motionOffsetX;
			constants.motionOffset[1] = in.motionOffsetY;
			constants.extent[0] = in.width;
			constants.extent[1] = in.height;
			constants.motionSourceMax[0] = in.motionSourceWidth - 1;
			constants.motionSourceMax[1] = in.motionSourceHeight - 1;
			guidesCB->Update(constants);

			ID3D11Buffer* cb = guidesCB->CB();
			ID3D11ShaderResourceView* srvs[2]{ in.depthSRV, in.motionSRV };
			ID3D11UnorderedAccessView* uavs[2]{ workingSet.depth.uav11.Get(), workingSet.motionVectors.uav11.Get() };
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &cb);
			context->CSSetShaderResources(0, 2, srvs);
			context->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
			context->Dispatch((in.width + 7) / 8, (in.height + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRVs[2]{};
			ID3D11UnorderedAccessView* nullUAVs[2]{};
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetShaderResources(0, 2, nullSRVs);
			context->CSSetUnorderedAccessViews(0, 2, nullUAVs, nullptr);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			context->CSSetShader(nullptr, nullptr, 0);
			return true;
		}

		bool TransformColor(ID3D11DeviceContext* context, LazyComputeShader& lazyShader,
			const std::vector<std::pair<const char*, const char*>>& defines,
			ID3D11ShaderResourceView* original, ID3D11ShaderResourceView* encodedInput, ID3D11ShaderResourceView* networkOutput,
			ID3D11UnorderedAccessView* destination, std::uint32_t width, std::uint32_t height)
		{
			auto* shader = lazyShader.Get(L"Data\\Shaders\\Upscaling\\NeuralRendering\\ColorTransformCS.hlsl", defines, "cs_5_0",
				"main", "NeuralRendering::ColorTransformCS");
			if (!shader || !destination)
				return false;
			if (!colorCB)
				colorCB = std::make_unique<ConstantBuffer>(ConstantBufferDesc<ColorTransformConstants>());
			ColorTransformConstants constants{};
			constants.extent[0] = width;
			constants.extent[1] = height;
			colorCB->Update(constants);

			ID3D11Buffer* cb = colorCB->CB();
			ID3D11ShaderResourceView* srvs[3]{ original, encodedInput, networkOutput };
			context->CSSetShader(shader, nullptr, 0);
			context->CSSetConstantBuffers(0, 1, &cb);
			context->CSSetShaderResources(0, 3, srvs);
			context->CSSetUnorderedAccessViews(0, 1, &destination, nullptr);
			context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
			ID3D11ShaderResourceView* nullSRVs[3]{};
			ID3D11UnorderedAccessView* nullUAV = nullptr;
			ID3D11Buffer* nullCB = nullptr;
			context->CSSetShaderResources(0, 3, nullSRVs);
			context->CSSetUnorderedAccessViews(0, 1, &nullUAV, nullptr);
			context->CSSetConstantBuffers(0, 1, &nullCB);
			context->CSSetShader(nullptr, nullptr, 0);
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
		LazyComputeShader encodeCS;
		LazyComputeShader decodeCS;
		std::unique_ptr<ConstantBuffer> guidesCB;
		std::unique_ptr<ConstantBuffer> colorCB;
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
	bool Renderer::IsFailureLatched() const { return state_->IsFailureLatched(); }
	std::uint32_t Renderer::NgxResult() const { return Runtime::Instance().NgxResult(); }
	std::uint64_t Renderer::SuccessfulFrames() const { return Runtime::Instance().SuccessfulFrames(); }
	const char* Renderer::StatusText() const { return ToString(Runtime::Instance().Status()); }
}