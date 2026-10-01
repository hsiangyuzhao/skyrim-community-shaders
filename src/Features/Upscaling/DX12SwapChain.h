#pragma once

#include <Windows.Foundation.h>
#include <stdio.h>
#include <winrt/base.h>
#include <wrl\client.h>
#include <wrl\wrappers\corewrappers.h>

#include <d3d11_4.h>
#include <d3d12.h>
#include <atomic>
#include <cstdint>

#include <directx/d3dx12.h>

class WrappedResource
{
public:
	WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device);
	~WrappedResource();

	ID3D11Texture2D* resource11 = nullptr;
	ID3D11ShaderResourceView* srv = nullptr;
	ID3D11UnorderedAccessView* uav = nullptr;
	ID3D11RenderTargetView* rtv = nullptr;
	winrt::com_ptr<ID3D12Resource> resource;
};

struct DXGISwapChainProxy : IDXGISwapChain
{
public:
	DXGISwapChainProxy(IDXGISwapChain4* a_swapChain);

	IDXGISwapChain4* swapChain;
	// The game owns proxy references independently of DX12SwapChain's native swap chain.
	std::atomic<ULONG> refCount{ 1 };

	/****IUnknown****/
	virtual HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
	virtual ULONG STDMETHODCALLTYPE AddRef() override;
	virtual ULONG STDMETHODCALLTYPE Release() override;

	/****IDXGIObject****/
	virtual HRESULT STDMETHODCALLTYPE SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown) override;
	virtual HRESULT STDMETHODCALLTYPE GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData) override;
	virtual HRESULT STDMETHODCALLTYPE GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent) override;

	/****IDXGIDeviceSubObject****/
	virtual HRESULT STDMETHODCALLTYPE GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice) override;

	/****IDXGISwapChain****/
	virtual HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags);
	virtual HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, _In_ REFIID riid, _COM_Outptr_ void** ppSurface);
	virtual HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget);
	virtual HRESULT STDMETHODCALLTYPE GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget);
	virtual HRESULT STDMETHODCALLTYPE GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc);
	virtual HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	virtual HRESULT STDMETHODCALLTYPE ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters);
	virtual HRESULT STDMETHODCALLTYPE GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput);
	virtual HRESULT STDMETHODCALLTYPE GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats);
	virtual HRESULT STDMETHODCALLTYPE GetLastPresentCount(_Out_ UINT* pLastPresentCount);
};

class DX12SwapChain
{
public:
	winrt::com_ptr<ID3D12Device> d3d12Device;
	// Native device remains the owner for host resources and is the device
	// passed to slSetD3DDevice. DLSS-G uses this upgraded interface only for
	// intercepted command-queue creation.
	winrt::com_ptr<ID3D12Device> d3d12DeviceProxy;
	winrt::com_ptr<ID3D12CommandQueue> commandQueueProxy;
	winrt::com_ptr<ID3D12CommandQueue> commandQueue;
	winrt::com_ptr<ID3D12CommandAllocator> commandAllocators[2];
	winrt::com_ptr<ID3D12GraphicsCommandList4> commandLists[2];

	winrt::com_ptr<ID3D12CommandAllocator> dlssCommandAllocator[2];
	winrt::com_ptr<ID3D12GraphicsCommandList4> dlssCommandList[2];

	winrt::com_ptr<ID3D12CommandAllocator> nisSharpenerCommandAllocator[2];
	winrt::com_ptr<ID3D12GraphicsCommandList4> nisSharpenerCommandList[2];

	IDXGISwapChain4* swapChain = nullptr;
	// slGetNativeInterface returns an AddRef'd pointer. Keep it owned for
	// non-intercepted DXGI calls while Present/GetBuffer/back-buffer-index use
	// the Streamline proxy stored above.
	winrt::com_ptr<IDXGISwapChain4> nativeSwapChain;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc;

	std::unique_ptr<WrappedResource> swapChainBufferWrapped;
	std::unique_ptr<WrappedResource> uiBufferWrapped;

	// D3D12 interop resources for frame generation
	std::unique_ptr<WrappedResource> depthBufferShared12;
	// DLSS SR keeps its existing depth-aware MV buffer below. DLSS-G consumes
	// this separate untouched engine MV copy so the two features cannot change
	// each other's motion-vector interpretation.
	std::unique_ptr<WrappedResource> motionVectorFrameGenerationShared12;
	std::unique_ptr<WrappedResource> motionVectorBufferShared12;

	// for sr/rr
	std::unique_ptr<WrappedResource> reactiveMaskShared12;
	std::unique_ptr<WrappedResource> transparencyCompositionMaskShared12;
	std::unique_ptr<WrappedResource> inputColorBufferShared12;
	std::unique_ptr<WrappedResource> outputColorBufferShared12;
	std::unique_ptr<WrappedResource> albedoShared12;
	std::unique_ptr<WrappedResource> reflectanceShared12;
	std::unique_ptr<WrappedResource> packedNormalShared12;
	std::unique_ptr<WrappedResource> specHitDistanceShared12;
	std::unique_ptr<WrappedResource> colorBeforeTransparencySnapshot;
	std::unique_ptr<WrappedResource> sssGuide;

	std::unique_ptr<WrappedResource> nisSharpenerInputShared12;
	std::unique_ptr<WrappedResource> nisSharpenerOutputShared12;

	winrt::com_ptr<ID3D11Device5> d3d11Device;
	winrt::com_ptr<ID3D11DeviceContext4> d3d11Context;

	winrt::com_ptr<ID3D11Fence> d3d11Fence;
	winrt::com_ptr<ID3D12Fence> d3d12Fence;

	winrt::com_ptr<ID3D12Fence> upscalingFence;

	winrt::com_ptr<ID3D12Resource> swapChainBuffers[2];

	UINT frameIndex = 0;
	UINT64 fenceValue = 0;

	UINT64 upscalingFenceValue = 0;

	// Active depth/motion-vector region for the rendered frame. The backing
	// resources can be output-sized, but DLSS-G must only consume the region
	// populated at the current fixed upscaling ratio.
	uint32_t dlssGInputWidth = 0;
	uint32_t dlssGInputHeight = 0;
	uint32_t dlssGInputExtentFrameIndex = UINT32_MAX;

	LARGE_INTEGER qpf;

	double refreshRate = 0;

	DXGISwapChainProxy* swapChainProxy = nullptr;

	// Returns the current frame time (in seconds) for accurate FPS calculation when frame generation is active
	float GetFrameTime() const;

	/**
	 * @brief Measured presentation multiplier reported by DLSS-G, smoothed.
	 *
	 * DLSS-G's `numFramesActuallyPresented` counts the frames the swap chain put on
	 * screen since the previous state query, i.e. presented frames per rendered frame.
	 * Present() already performs exactly one state query per rendered frame, so this
	 * is the real presentation cadence rather than an assumed 2x.
	 *
	 * @return >0 once DLSS-G has reported a cadence; 0 when no measurement exists
	 *         (frame generation off, FSR 3 frame generation, or DLSS-G not reporting).
	 *         Callers must fall back to an estimate and label it as such.
	 */
	float GetMeasuredPresentMultiplier() const { return measuredPresentMultiplier; }
	struct DLSSGCadenceTotals
	{
		uint64_t samples = 0;
		uint64_t presented = 0;
	};
	DLSSGCadenceTotals GetDLSSGCadenceTotals() const { return { dlssGTotalSamples, dlssGTotalPresented }; }
	void ResetDLSSGCadenceMeasurement();

	void CreateD3D12Device(IDXGIAdapter* a_adapter);
	void CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC swapChainDesc);

	void CreateInterop();

	DXGISwapChainProxy* GetSwapChainProxy();
	void SetD3D11Device(ID3D11Device* a_d3d11Device);
	void SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context);

	HRESULT GetBuffer(UINT buffer, REFIID riid, void** ppSurface);
	HRESULT Present(UINT SyncInterval, UINT Flags);
	HRESULT GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice);
	HANDLE GetFrameLatencyWaitableObject();
	IDXGISwapChain4* GetNativeSwapChain() const;

	void SetUIBuffer();
	void MarkDLSSGSceneResourcesReady(uint32_t a_frameIndex);
	void SetDLSSGInputExtent(uint32_t a_width, uint32_t a_height, uint32_t a_frameIndex);
	uint32_t GetDLSSGInputWidth() const;
	uint32_t GetDLSSGInputHeight() const;
	bool HasValidDLSSGInputExtent() const;

	// D3D12 interop resource management
	void CreateSharedResources();

private:
	void WaitForDLSSGInputsBeforeResourceRecreation(const char* a_reason);
	void LogDLSSGCadenceWindow();

	enum class DLSSGPresentationState
	{
		kGameplay,
		kMapWarmup,
		kMapGenerating,
		kMapSuspended,
		kResumePending
	};

	bool UpdateDLSSGPresentationState(bool a_frameGenerationRequested, bool a_mapMenuOpen, bool a_mapRenderingContext);
	bool DLSSGResourcesReadyForFrame(uint32_t a_frameIndex) const;

	DLSSGPresentationState dlssGPresentationState = DLSSGPresentationState::kGameplay;
	uint32_t dlssGSceneResourcesFrameIndex = UINT32_MAX;
	uint32_t dlssGHUDLessFrameIndex = UINT32_MAX;
	uint32_t dlssGResumeWarmupFrameIndex = UINT32_MAX;
	bool dlssGMapUnexpectedGeneratedFramesLogged = false;
	bool dlssGWaitingForResources = false;

	// Smoothed presented-frames-per-rendered-frame, fed from the DLSS-G state query
	// already issued once per Present. 0 means "never measured".
	float measuredPresentMultiplier = 0.0f;
	bool dlssGPreviousStateQueryValid = false;
	bool dlssGInputsFenceObserved = false;
	bool dlssGInputsFenceMissingLogged = false;
	uint32_t dlssGCadenceSamples = 0;
	uint32_t dlssGCadencePresented = 0;
	uint64_t dlssGTotalSamples = 0;
	uint64_t dlssGTotalPresented = 0;
	uint32_t dlssGCadenceBuckets[6]{};  // 0, 1, 2, 3, 4, 5+ presented.
	uint32_t dlssGCadenceRequested = 1;
	uint32_t dlssGCadenceApplied = 1;
	uint32_t dlssGCadenceFirstToken = UINT32_MAX;
	uint32_t dlssGCadenceLastToken = UINT32_MAX;
};
