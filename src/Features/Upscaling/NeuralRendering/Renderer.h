// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#pragma once

#include "Runtime.h"

#include <array>
#include <cstdint>

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

		/// (batch 38a) Everything the Batch 38 pass needs for one frame. Colour and guides are given
		/// to the network at one extent (width x height), so their texels line up exactly.
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
			/// Render extent of the motion source, to resample it onto width x height.
			std::uint32_t motionSourceWidth = 0;
			std::uint32_t motionSourceHeight = 0;
			/// Added to every motion vector, normalised screen units (the jitter step, before upscaling).
			float motionOffsetX = 0.0f;
			float motionOffsetY = 0.0f;
			/// DLSSNR.ScalingRatio: the network's working resolution as a fraction of width x height.
			float modelScale = 1.0f;
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