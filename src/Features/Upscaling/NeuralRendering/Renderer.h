// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#pragma once

#include "Runtime.h"

#include <array>
#include <cstdint>

#include <dxgiformat.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;
struct ID3D11ShaderResourceView;

namespace NeuralRendering
{
	class Renderer
	{
	public:
		struct StereoEyeInput
		{
			ID3D11Resource* depth = nullptr;
			ID3D11ShaderResourceView* depthSRV = nullptr;
			ID3D11Resource* motionVectors = nullptr;
			std::uint32_t sourceX = 0;
			std::uint32_t sourceY = 0;
			float motionVectorScaleX = 1.0f;
			float motionVectorScaleY = 1.0f;
		};

		static Renderer& Instance();
		~Renderer();

		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		bool Apply(ID3D11Device* device, ID3D11DeviceContext* context, std::uint32_t eyeIndex,
			ID3D11Resource* color, ID3D11Resource* depth, ID3D11ShaderResourceView* depthSRV,
			ID3D11Resource* motionVectors,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight,
			float motionVectorScaleX, float motionVectorScaleY, const Tuning& tuning);
		bool ApplyStereo(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Resource* color,
			const std::array<StereoEyeInput, 2>& eyes,
			std::uint32_t guideWidth, std::uint32_t guideHeight,
			std::uint32_t colorWidth, std::uint32_t colorHeight, const Tuning& tuning);
		/// (batch 38a) Where in the frame the Batch 38 pass runs.
		enum class Placement : std::uint8_t
		{
			AfterUpscaling,   ///< on the finished, tonemapped output-resolution image, before UI (37c's place)
			BeforeUpscaling,  ///< on the HDR render-resolution scene, which DLSS then upscales
		};

		/// (batch 38c) How the before-upscaling colour is wrapped for the network (Common.hlsli).
		enum class Encode : std::uint32_t
		{
			Identity = 0,  ///< after upscaling: the finished image as it is
			Reinhard = 1,  ///< 38a: max-channel Reinhard + 2.2, no exposure
			Curve = 2,     ///< 38c: the frame's exposure + the game's own grading / tone curve
		};

		/// (batch 38c) Where the tone curve comes from (ToneCurveCS.hlsl). All optional: without a
		/// grading LUT the curve is a per-channel Reinhard, without auto exposure the exposure is fixed.
		struct ToneCurveSource
		{
			ID3D11ShaderResourceView* gradingLUT = nullptr;   ///< Color Grading's baked 3D LUT
			ID3D11ShaderResourceView* adaptation = nullptr;   ///< Histogram Auto Exposure's adapted luminance
			float tint[4]{ 1.0f, 1.0f, 1.0f, 0.0f };
			float inputGamma = 1.0f;
			float outputGamma = 1.0f;
			float cinematicBrightness = 1.0f;
			float cinematicContrast = 1.0f;
			float exposureCompensation = 1.0f;  ///< linear
			float adaptationMin = 0.0f;
			float adaptationMax = 1e6f;
			float fixedExposure = 1.0f;
			bool gammaCorrect = false;  ///< Linear Lighting's final gamma
		};

		/// (batch 38a) Everything the Batch 38 pass needs for one frame. Colour and guides are given
		/// to the network at one extent, so their texels line up exactly.
		///
		/// (batch 38c) Three extents now. The frame: width x height, read from and written back into.
		/// The model's content: workWidth x workHeight, smaller than the frame below 100% Model
		/// Resolution. The network's: paddedWidth x paddedHeight, the content padded (edge repeated) to
		/// the network grid. At 100% with no padding all three are the same, as in 38a.
		struct PassInput
		{
			Placement placement = Placement::AfterUpscaling;
			/// Read from and written back into, top-left width x height. After upscaling: the bound
			/// render target. Before upscaling: kMAIN.
			ID3D11Resource* sceneColor = nullptr;
			/// Before upscaling only: SRV of sceneColor, read by the encode and decode passes.
			ID3D11ShaderResourceView* sceneColorSRV = nullptr;
			/// Depth, read 1:1 in the top-left width x height.
			ID3D11ShaderResourceView* depthSRV = nullptr;
			/// The engine's motion vectors (render resolution, top-left of the target).
			ID3D11ShaderResourceView* motionSRV = nullptr;
			std::uint32_t width = 0;
			std::uint32_t height = 0;
			/// Render extent of the motion source, to resample it onto the model's grid.
			std::uint32_t motionSourceWidth = 0;
			std::uint32_t motionSourceHeight = 0;
			/// Added to every motion vector, normalised screen units (the jitter step, before upscaling).
			float motionOffsetX = 0.0f;
			float motionOffsetY = 0.0f;
			/// (batch 38c) The model's content extent (Model Resolution), and the network's (padded).
			std::uint32_t workWidth = 0;
			std::uint32_t workHeight = 0;
			std::uint32_t paddedWidth = 0;
			std::uint32_t paddedHeight = 0;
			/// (batch 38c) Before upscaling: the wrap, and the format the network reads it in.
			Encode encode = Encode::Identity;
			DXGI_FORMAT networkFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
			ToneCurveSource toneCurve{};
			/// (batch 38c) Tone preservation strength, 0 = off.
			float toneStrength = 0.0f;
			/// (batch 38c) For the depth-aware upsample of the edit.
			float cameraNear = 1.0f;
			float cameraFar = 10000.0f;
			/// (batch 38c) Give the model its tuning when the feature is created (and rebuild it when the
			/// tuning changes).
			bool tuningAtCreate = false;
			/// Ask the network to drop its history this frame.
			bool reset = false;
			Tuning tuning{};
		};

		/// (batch 38a) The Batch 38 pass. Leaves the colour untouched and returns false on any
		/// failure, so a false return is never a partially applied frame.
		bool Run(ID3D11Device* device, ID3D11DeviceContext* context, const PassInput& input);
		/// (batch 38a) Releases the NGX feature and every texture, keeping the D3D12 device and the
		/// NGX initialisation. Waits for the GPU first. Used when placement, extent or model
		/// resolution changes.
		void ReleaseWorkingSet();
		/// (batch 38a) True while the D3D12 device exists (from the first pass until Reset).
		[[nodiscard]] bool IsInitialized() const;
		/// (batch 38a) GPU time of the network on its D3D12 queue, smoothed; 0 before the first reading.
		[[nodiscard]] float ModelGpuMs() const;
		[[nodiscard]] float LastModelGpuMs() const;
		[[nodiscard]] std::uint32_t MaxInFlight() const;
		[[nodiscard]] std::uint32_t BackpressureWaits() const;
		/// (batch 38c) How the last Run carried colour in and out: "direct copy" (the 37c / 38a after-
		/// upscaling copy) or "prepare + finalize" (the shader path); empty before the first Run.
		[[nodiscard]] const char* LastRoute() const;

		/// @param unloadRuntime false = (batch 38a) keep nvngx_dlssnr.dll mapped (it holds no video memory).
		void Reset(bool unloadRuntime = true);
		void ResetHistory();

		[[nodiscard]] bool IsFailureLatched() const;
		[[nodiscard]] std::uint32_t NgxResult() const;
		[[nodiscard]] std::uint64_t SuccessfulFrames() const;
		[[nodiscard]] const char* StatusText() const;

	private:
		Renderer();
		class State;
		State* state_ = nullptr;
	};
}