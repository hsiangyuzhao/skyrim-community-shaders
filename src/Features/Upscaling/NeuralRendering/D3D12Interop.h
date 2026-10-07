// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#pragma once

#include <array>
#include <cstdint>

#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace NeuralRendering
{
	struct SharedTexture
	{
		Microsoft::WRL::ComPtr<ID3D11Texture2D> resource11;
		Microsoft::WRL::ComPtr<ID3D11UnorderedAccessView> uav11;
		// (batch 38a) Created whenever the texture is bindable as a shader resource. The
		// before-upscaling path reads the encoded input and the network's output back on the
		// D3D11 side to decode them; nothing in the 37c path reads it.
		Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv11;
		Microsoft::WRL::ComPtr<ID3D12Resource> resource12;
		D3D11_TEXTURE2D_DESC desc{};
	};

	class D3D12Interop
	{
	public:
		/// Command contexts the 37c path cycles through. Kept so the Batch 38 master switch can
		/// put the old ring back exactly.
		static constexpr std::size_t kLegacyCommandContextCount = 3;
		/// (batch 38a) Contexts allocated, and cycled through while Batch 38 is on. See
		/// BeginD3D12 for why three was too few.
		static constexpr std::size_t kCommandContextCount = 8;

		~D3D12Interop();

		D3D12Interop(const D3D12Interop&) = delete;
		D3D12Interop& operator=(const D3D12Interop&) = delete;

		bool Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context);
		void Shutdown();
		bool CreateSharedTexture(const D3D11_TEXTURE2D_DESC& desc, SharedTexture& texture, const char* name);
		/// @param activeContexts how many of the kCommandContextCount contexts to cycle through.
		bool BeginD3D12(ID3D12GraphicsCommandList** commandList, std::size_t activeContexts = kLegacyCommandContextCount);
		bool EndD3D12();
		bool WaitForIdle();

		[[nodiscard]] bool IsInitialized() const { return initialized_; }
		[[nodiscard]] HRESULT LastError() const { return lastError_; }
		[[nodiscard]] const char* LastOperation() const { return lastOperation_; }
		[[nodiscard]] std::uint32_t LastResourceFlags() const { return lastResourceFlags_; }
		[[nodiscard]] ID3D12Device* Device() const { return device12_.Get(); }

		/// (batch 38a) GPU time of the last completed D3D12 submission (the network itself plus
		/// its barriers), from timestamps on the D3D12 queue. 0 until one has come back.
		[[nodiscard]] float LastGpuMs() const { return lastGpuMs_; }
		/// (batch 38a) Exponentially smoothed LastGpuMs (same 0.95/0.05 weights as the overlay).
		[[nodiscard]] float SmoothedGpuMs() const { return smoothedGpuMs_; }
		/// (batch 38a) Most submissions found still executing on the GPU when a new one began.
		[[nodiscard]] std::uint32_t MaxInFlight() const { return maxInFlight_; }
		/// (batch 38a) Times BeginD3D12 had to block the CPU because every context was in flight.
		[[nodiscard]] std::uint32_t BackpressureWaits() const { return backpressureWaits_; }
		/// (batch 38a) References to the D3D12 device other than ours. Everything a device allocated is
		/// freed when its last reference goes, so after a teardown this must be 0 for the memory to come
		/// back; anything else is a reference someone (NGX, a texture) still holds.
		[[nodiscard]] std::uint32_t OtherDeviceReferences() const;
		/// (batch 38a) OtherDeviceReferences as counted by the last Shutdown, just before the device went.
		[[nodiscard]] std::uint32_t LastShutdownOtherReferences() const { return lastShutdownOtherReferences_; }

	private:
		struct CommandContext
		{
			Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
			Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList;
			std::uint64_t fenceValue = 0;
			bool timestampsPending = false;
		};

		D3D12Interop() = default;
		friend class Renderer;
		bool RecordFailure(HRESULT result);
		bool WaitForFence(std::uint64_t value, DWORD timeoutMs = 250);
		void CollectTimestamps(std::size_t contextIndex);

		Microsoft::WRL::ComPtr<ID3D11Device5> device11_;
		Microsoft::WRL::ComPtr<ID3D11DeviceContext4> context11_;
		Microsoft::WRL::ComPtr<ID3D11Fence> fence11_;
		Microsoft::WRL::ComPtr<ID3D12Device> device12_;
		Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue12_;
		std::array<CommandContext, kCommandContextCount> commandContexts_;
		Microsoft::WRL::ComPtr<ID3D12Fence> fence12_;
		Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestampHeap_;
		Microsoft::WRL::ComPtr<ID3D12Resource> timestampReadback_;
		std::uint64_t timestampFrequency_ = 0;
		HANDLE fenceEvent_ = nullptr;
		std::uint64_t fenceValue_ = 0;
		HRESULT lastError_ = S_OK;
		const char* lastOperation_ = "none";
		std::uint32_t lastResourceFlags_ = 0;
		std::size_t commandContextCursor_ = 0;
		std::size_t recordingContext_ = kCommandContextCount;
		float lastGpuMs_ = 0.0f;
		float smoothedGpuMs_ = 0.0f;
		std::uint32_t maxInFlight_ = 0;
		std::uint32_t backpressureWaits_ = 0;
		std::uint32_t lastShutdownOtherReferences_ = 0;
		bool backpressureLogged_ = false;
		bool initialized_ = false;
		bool recording_ = false;
	};
}
