#include "DX12SwapChain.h"

#include <FidelityFX/api/include/dx12/ffx_api_dx12.hpp>
#include <dxgi1_6.h>

#include "../../Deferred.h"
#include "../Upscaling.h"
#include "FidelityFX.h"
#include "Streamline.h"

void DX12SwapChain::CreateD3D12Device(IDXGIAdapter* a_adapter)
{
	DX::ThrowIfFailed(D3D12CreateDevice(a_adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&d3d12Device)));

	auto& upscaling = globals::features::upscaling;
	const bool useDLSSG = upscaling.IsDLSSGBackend() && upscaling.streamline.featureDLSS_G && upscaling.streamline.featureReflex && upscaling.streamline.featurePCL;
	ID3D12Device* deviceForQueueCreation = d3d12Device.get();
	if (useDLSSG) {
		// Streamline requires the native device immediately after creation and
		// before any hooked queue/swap-chain APIs are used.
		upscaling.SetBackendD3DDevice(d3d12Device.get());
		upscaling.PostBackendDevice();

		// Manual hooking requires the proxy device before CreateCommandQueue;
		// all host resources below continue to use the native device.
		upscaling.UpgradeBackendInterface(reinterpret_cast<void**>(&deviceForQueueCreation));
	}
	if (deviceForQueueCreation == d3d12Device.get())
		d3d12DeviceProxy.copy_from(deviceForQueueCreation);
	else
		d3d12DeviceProxy.attach(deviceForQueueCreation);

	D3D12_COMMAND_QUEUE_DESC queueDesc = {};
	queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
	queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
	queueDesc.NodeMask = 0;

	DX::ThrowIfFailed(d3d12DeviceProxy->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&commandQueueProxy)));
	if (useDLSSG) {
		void* nativeQueue = nullptr;
		const auto nativeQueueResult = upscaling.streamline.GetNativeInterface(commandQueueProxy.get(), &nativeQueue);
		if (nativeQueueResult != sl::Result::eOk) {
			logger::critical("[Streamline] Failed to retrieve the native D3D12 command queue ({})", magic_enum::enum_name(nativeQueueResult));
			DX::ThrowIfFailed(E_FAIL);
		}
		commandQueue.attach(static_cast<ID3D12CommandQueue*>(nativeQueue));
	} else {
		commandQueue.copy_from(commandQueueProxy.get());
	}

	for (int i = 0; i < 2; i++) {
		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&commandAllocators[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocators[i].get(), nullptr, IID_PPV_ARGS(&commandLists[i])));
		commandLists[i]->Close();

		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&dlssCommandAllocator[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, dlssCommandAllocator[i].get(), nullptr, IID_PPV_ARGS(&dlssCommandList[i])));
		dlssCommandList[i]->Close();

		DX::ThrowIfFailed(d3d12Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&nisSharpenerCommandAllocator[i])));
		DX::ThrowIfFailed(d3d12Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, nisSharpenerCommandAllocator[i].get(), nullptr, IID_PPV_ARGS(&nisSharpenerCommandList[i])));
		nisSharpenerCommandList[i]->Close();
	}
}

void DX12SwapChain::CreateSwapChain(IDXGIAdapter* adapter, DXGI_SWAP_CHAIN_DESC a_swapChainDesc)
{
	CreateD3D12Device(adapter);

	IDXGIFactory4* dxgiFactory;
	DX::ThrowIfFailed(adapter->GetParent(IID_PPV_ARGS(&dxgiFactory)));

	swapChainDesc = {};
	swapChainDesc.Width = a_swapChainDesc.BufferDesc.Width;
	swapChainDesc.Height = a_swapChainDesc.BufferDesc.Height;
	swapChainDesc.Format = a_swapChainDesc.BufferDesc.Format;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.SwapEffect = a_swapChainDesc.SwapEffect;
	swapChainDesc.Flags = a_swapChainDesc.Flags;

	auto& upscaling = globals::features::upscaling;
	const bool useDLSSG = upscaling.IsDLSSGBackend() && upscaling.streamline.featureDLSS_G && upscaling.streamline.featureReflex && upscaling.streamline.featurePCL;
	if (useDLSSG) {
		// DLSS-G owns the presentation path through Streamline. The factory
		// must be upgraded before CreateSwapChainForHwnd; creating an FFX
		// swapchain first and upgrading it afterwards would nest two owners.
		IDXGIFactory4* proxyFactory = dxgiFactory;
		upscaling.UpgradeBackendInterface(reinterpret_cast<void**>(&proxyFactory));

		winrt::com_ptr<IDXGISwapChain1> proxySwapChain;
		DX::ThrowIfFailed(proxyFactory->CreateSwapChainForHwnd(
			commandQueueProxy.get(),
			a_swapChainDesc.OutputWindow,
			&swapChainDesc,
			nullptr,
			nullptr,
			proxySwapChain.put()));
		DX::ThrowIfFailed(proxySwapChain->QueryInterface(IID_PPV_ARGS(&swapChain)));
		void* nativeInterface = nullptr;
		if (upscaling.streamline.GetNativeInterface(swapChain, &nativeInterface) == sl::Result::eOk) {
			auto* nativeBase = static_cast<IDXGISwapChain*>(nativeInterface);
			DX::ThrowIfFailed(nativeBase->QueryInterface(IID_PPV_ARGS(&nativeSwapChain)));
			nativeBase->Release();
		} else {
			logger::warn("[Streamline] Failed to retrieve the native DLSS-G swap chain interface");
		}
		logger::info("[Upscaling] Created Streamline DLSS-G proxy swap chain");
	} else {
		ffx::CreateContextDescFrameGenerationSwapChainForHwndDX12 ffxSwapChainDesc{};

		ffxSwapChainDesc.desc = &swapChainDesc;
		ffxSwapChainDesc.dxgiFactory = dxgiFactory;
		ffxSwapChainDesc.fullscreenDesc = nullptr;
		ffxSwapChainDesc.gameQueue = commandQueue.get();
		ffxSwapChainDesc.hwnd = a_swapChainDesc.OutputWindow;
		ffxSwapChainDesc.swapchain = &swapChain;

		auto& fidelityFX = upscaling.fidelityFX;
		if (ffx::CreateContext(fidelityFX.swapChainContext, nullptr, ffxSwapChainDesc) != ffx::ReturnCode::Ok)
			logger::critical("[FidelityFX] Failed to create swap chain context!");
		nativeSwapChain.copy_from(swapChain);
	}

	DX::ThrowIfFailed(swapChain->GetBuffer(0, IID_PPV_ARGS(&swapChainBuffers[0])));
	DX::ThrowIfFailed(swapChain->GetBuffer(1, IID_PPV_ARGS(&swapChainBuffers[1])));

	frameIndex = swapChain->GetCurrentBackBufferIndex();

	if (!useDLSSG)
		upscaling.fidelityFX.SetupFrameGeneration();
}

void DX12SwapChain::CreateInterop()
{
	HANDLE sharedFenceHandle;
	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&d3d12Fence)));
	DX::ThrowIfFailed(d3d12Device->CreateSharedHandle(d3d12Fence.get(), nullptr, GENERIC_ALL, nullptr, &sharedFenceHandle));
	DX::ThrowIfFailed(d3d11Device->OpenSharedFence(sharedFenceHandle, IID_PPV_ARGS(&d3d11Fence)));
	CloseHandle(sharedFenceHandle);

	DX::ThrowIfFailed(d3d12Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&upscalingFence)));

	swapChainProxy = new DXGISwapChainProxy(swapChain);

	D3D11_TEXTURE2D_DESC texDesc11{};
	texDesc11.Width = swapChainDesc.Width;
	texDesc11.Height = swapChainDesc.Height;
	texDesc11.MipLevels = 1;
	texDesc11.ArraySize = 1;
	texDesc11.Format = swapChainDesc.Format;
	texDesc11.SampleDesc.Count = 1;
	texDesc11.SampleDesc.Quality = 0;
	texDesc11.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

	swapChainBufferWrapped = new WrappedResource(texDesc11, d3d11Device.get(), d3d12Device.get());

	// FSR uses this as a premultiplied UI target. DLSS-G uses it as a
	// HUD-less snapshot, so CopyResource requires the final-color format.
	if (!globals::features::upscaling.IsDLSSGBackend())
		texDesc11.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	uiBufferWrapped = new WrappedResource(texDesc11, d3d11Device.get(), d3d12Device.get());
}

DXGISwapChainProxy* DX12SwapChain::GetSwapChainProxy()
{
	return swapChainProxy;
}

void DX12SwapChain::SetD3D11Device(ID3D11Device* a_d3d11Device)
{
	DX::ThrowIfFailed(a_d3d11Device->QueryInterface(IID_PPV_ARGS(&d3d11Device)));
}

void DX12SwapChain::SetD3D11DeviceContext(ID3D11DeviceContext* a_d3d11Context)
{
	DX::ThrowIfFailed(a_d3d11Context->QueryInterface(IID_PPV_ARGS(&d3d11Context)));
}

HRESULT DX12SwapChain::GetBuffer(void** ppSurface)
{
	*ppSurface = swapChainBufferWrapped->resource11;
	return S_OK;
}

HRESULT DX12SwapChain::Present(UINT SyncInterval, UINT Flags)
{
	auto& upscaling = globals::features::upscaling;
	auto* ui = globals::game::ui;
	const bool mapMenuOpen = ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
	const bool mapRenderingContext = upscaling.IsDLSSGMapRenderingContext();
	// MapMenu normally pauses the game. Staged map recovery makes it safe to
	// request DLSS-G after native and DLSS-SR-only warm-up frames.
	const bool frameGenerationRequested = upscaling.IsFrameGenerationEnabled() &&
		(!ui->GameIsPaused() || mapRenderingContext);
	bool useFrameGeneration = frameGenerationRequested;

	if (upscaling.IsDLSSGBackend() && upscaling.IsDLSSGAvailable()) {
		// DLSS-G intercepts Present asynchronously. Apply a menu/off transition
		// before selecting or writing the next native back buffer; doing it after
		// the copy allows one more buffer to enter the old presentation mode.
		useFrameGeneration = UpdateDLSSGPresentationState(frameGenerationRequested, mapMenuOpen, mapRenderingContext);
		// SR inputs are evaluate-local now, so a short FG suspension can retain
		// resources without exposing DLSS-G to SR's depth/MV tags.
		upscaling.PresentFrameGeneration(useFrameGeneration, true);

		// The interposer can advance the native swap-chain index when its mode
		// changes. Re-query it after SetOptions instead of carrying the index from
		// the preceding generated Present into the first non-generated frame.
		frameIndex = swapChain->GetCurrentBackBufferIndex();
	}

	// Wait for D3D11 to finish
	DX::ThrowIfFailed(d3d11Context->Signal(d3d11Fence.get(), fenceValue));
	DX::ThrowIfFailed(commandQueue->Wait(d3d12Fence.get(), fenceValue));
	fenceValue++;

	// New frame, reset
	DX::ThrowIfFailed(commandAllocators[frameIndex]->Reset());
	DX::ThrowIfFailed(commandLists[frameIndex]->Reset(commandAllocators[frameIndex].get(), nullptr));

	// Copy shared texture to swap chain buffer
	{
		auto fakeSwapChain = swapChainBufferWrapped->resource.get();
		auto realSwapChain = swapChainBuffers[frameIndex].get();
		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}

		commandLists[frameIndex]->CopyResource(realSwapChain, fakeSwapChain);

		{
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(fakeSwapChain, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON));
			barriers.push_back(CD3DX12_RESOURCE_BARRIER::Transition(realSwapChain, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT));
			commandLists[frameIndex]->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		}
	}

	// FSR configures and dispatches frame generation on this command list, so
	// preserve its established ordering. DLSS-G only changes Streamline options
	// and was handled above before the back-buffer copy.
	if (!upscaling.IsDLSSGBackend())
		upscaling.PresentFrameGeneration(useFrameGeneration);

	if (upscaling.IsDLSSGBackend() && upscaling.IsDLSSGAvailable() && useFrameGeneration) {
		// Deferred normally submits constants after the current per-frame camera
		// buffer has been cached.  This is the late fallback for render paths
		// which skip that pass, using the already-latched frame token.
		upscaling.streamline.CheckFrameConstantsForLatchedFrame();

		// DLSS-G consumes these resources from the presenting command list.
		// Streamline owns the tag construction; this call keeps the manual
		// hooking state explicit without routing FSR resources through SL.
		Streamline::DLSSGFrameResources frameResources{};
		frameResources.depth = depthBufferShared12->resource.get();
		frameResources.motionVectors = motionVectorBufferShared12->resource.get();
		// FinalColor remains scene plus UI. This snapshot is only the HUD-less
		// input consumed by DLSS-G's normal gameplay path.
		frameResources.hudless = uiBufferWrapped->resource.get();
		// Depth and motion vectors only contain valid data in the active render
		// region. Their resources are output-sized for interop, so using the full
		// resource extent here makes DLSS-G sample stale pixels outside that
		// region whenever an upscaler renders below output resolution.
		frameResources.depthExtent = { 0, 0, GetDLSSGInputWidth(), GetDLSSGInputHeight() };
		frameResources.motionVectorsExtent = frameResources.depthExtent;
		frameResources.hudlessExtent = { 0, 0, swapChainDesc.Width, swapChainDesc.Height };
		upscaling.streamline.TagDLSSGResources(frameResources, commandLists[frameIndex].get());
	}

	DX::ThrowIfFailed(commandLists[frameIndex]->Close());

	if (upscaling.IsDLSSGBackend())
		upscaling.streamline.SetPCLMarker(sl::PCLMarker::eRenderSubmitStart);

	ID3D12CommandList* commandListsToExecute[] = { commandLists[frameIndex].get() };
	commandQueue->ExecuteCommandLists(1, commandListsToExecute);

	if (upscaling.IsDLSSGBackend())
		upscaling.streamline.SetPCLMarker(sl::PCLMarker::eRenderSubmitEnd);

	// Present the frame
	if (upscaling.IsDLSSGBackend())
		upscaling.streamline.SetPCLMarker(sl::PCLMarker::ePresentStart);
	const auto presentResult = swapChain->Present(SyncInterval, Flags);
	if (upscaling.IsDLSSGBackend()) {
		upscaling.streamline.SetPCLMarker(sl::PCLMarker::ePresentEnd);
		if (SUCCEEDED(presentResult) && upscaling.IsDLSSGAvailable() &&
			(useFrameGeneration || dlssGPresentationState == DLSSGPresentationState::kMapSuspended)) {
			sl::DLSSGState dlssGState{};
			if (upscaling.streamline.GetDLSSGState(dlssGState) &&
				dlssGPresentationState == DLSSGPresentationState::kMapSuspended &&
				dlssGState.numFramesActuallyPresented > 1 &&
				!dlssGMapUnexpectedGeneratedFramesLogged) {
				logger::warn("[DLSS-G] Streamline presented {} frames while MapMenu generation was disabled", dlssGState.numFramesActuallyPresented);
				dlssGMapUnexpectedGeneratedFramesLogged = true;
			}
		}
	}
	DX::ThrowIfFailed(presentResult);

	// Wait for D3D12 to finish
	DX::ThrowIfFailed(commandQueue->Signal(d3d12Fence.get(), fenceValue));
	DX::ThrowIfFailed(d3d11Context->Wait(d3d11Fence.get(), fenceValue));
	fenceValue++;

	// Update the frame index
	frameIndex = swapChain->GetCurrentBackBufferIndex();

	float clearColor[4]{ 0, 0, 0, 0 };
	d3d11Context->ClearRenderTargetView(uiBufferWrapped->rtv, clearColor);

	// If VSync is disabled, use frame limiter to prevent tearing and optimise pacing
	if (SyncInterval == 0)
		upscaling.FrameLimiter();

	return S_OK;
}

HRESULT DX12SwapChain::GetDevice(REFIID uuid, void** ppDevice)
{
	if (uuid == __uuidof(ID3D11Device) || uuid == __uuidof(ID3D11Device1) || uuid == __uuidof(ID3D11Device2) || uuid == __uuidof(ID3D11Device3) || uuid == __uuidof(ID3D11Device4) || uuid == __uuidof(ID3D11Device5)) {
		*ppDevice = d3d11Device.get();
		return S_OK;
	}

	return GetNativeSwapChain()->GetDevice(uuid, ppDevice);
}

HANDLE DX12SwapChain::GetFrameLatencyWaitableObject()
{
	return GetNativeSwapChain()->GetFrameLatencyWaitableObject();
}

IDXGISwapChain4* DX12SwapChain::GetNativeSwapChain() const
{
	return nativeSwapChain ? nativeSwapChain.get() : swapChain;
}

float DX12SwapChain::GetFrameTime() const
{
	// Calculate frame time based on swap chain presentation
	static float lastPresentTime = 0.0f;
	static float frameTime = 1.0f / 60.0f;  // Default to 60 fps
	static LARGE_INTEGER frequency = {};
	static LARGE_INTEGER currentTime = {};

	if (frequency.QuadPart == 0) {
		QueryPerformanceFrequency(&frequency);
	}

	QueryPerformanceCounter(&currentTime);
	float time = static_cast<float>(currentTime.QuadPart) / static_cast<float>(frequency.QuadPart);

	if (lastPresentTime > 0.0f) {
		frameTime = time - lastPresentTime;
	}
	lastPresentTime = time;

	return frameTime;
}

WrappedResource::WrappedResource(D3D11_TEXTURE2D_DESC a_texDesc, ID3D11Device5* a_d3d11Device, ID3D12Device* a_d3d12Device)
{
	// Create D3D11 shared texture directly instead of wrapping D3D12 resource
	a_texDesc.MiscFlags |= D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
	DX::ThrowIfFailed(a_d3d11Device->CreateTexture2D(&a_texDesc, nullptr, &resource11));

	// Get shared handle from D3D11 texture to enable D3D12 access
	winrt::com_ptr<IDXGIResource1> dxgiResource;
	DX::ThrowIfFailed(resource11->QueryInterface(IID_PPV_ARGS(dxgiResource.put())));
	HANDLE sharedHandle = nullptr;
	DX::ThrowIfFailed(dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedHandle));

	// Open the shared D3D11 texture as D3D12 resource
	DX::ThrowIfFailed(a_d3d12Device->OpenSharedHandle(sharedHandle, IID_PPV_ARGS(resource.put())));
	CloseHandle(sharedHandle);

	if (a_texDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
		D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = a_texDesc.Format;
		srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Texture2D.MipLevels = 1;

		DX::ThrowIfFailed(a_d3d11Device->CreateShaderResourceView(resource11, &srvDesc, &srv));
	}

	if (a_texDesc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
		if (a_texDesc.ArraySize > 1) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = a_texDesc.ArraySize;

			DX::ThrowIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav));
		} else {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
			uavDesc.Format = a_texDesc.Format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
			uavDesc.Texture2D.MipSlice = 0;

			DX::ThrowIfFailed(a_d3d11Device->CreateUnorderedAccessView(resource11, &uavDesc, &uav));
		}
	}

	if (a_texDesc.BindFlags & D3D11_BIND_RENDER_TARGET) {
		D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = {};
		rtvDesc.Format = a_texDesc.Format;
		rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
		rtvDesc.Texture2D.MipSlice = 0;
		DX::ThrowIfFailed(a_d3d11Device->CreateRenderTargetView(resource11, &rtvDesc, &rtv));
	}
}

WrappedResource::~WrappedResource()
{
	if (resource11) {
		resource11->Release();
		resource11 = nullptr;
	}
	if (srv) {
		srv->Release();
		srv = nullptr;
	}
	if (uav) {
		uav->Release();
		uav = nullptr;
	}
	if (rtv) {
		rtv->Release();
		rtv = nullptr;
	}
	// resource (winrt::com_ptr) will be automatically released
}

DXGISwapChainProxy::DXGISwapChainProxy(IDXGISwapChain4* a_swapChain)
{
	swapChain = a_swapChain;
}

/****IUknown****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::QueryInterface(REFIID riid, void** ppvObj)
{
	auto ret = swapChain->QueryInterface(riid, ppvObj);
	if (*ppvObj)
		*ppvObj = this;
	return ret;
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::AddRef()
{
	return swapChain->AddRef();
}

ULONG STDMETHODCALLTYPE DXGISwapChainProxy::Release()
{
	return swapChain->Release();
}

/****IDXGIObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateData(_In_ REFGUID Name, UINT DataSize, _In_reads_bytes_(DataSize) const void* pData)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->SetPrivateData(Name, DataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetPrivateDataInterface(_In_ REFGUID Name, _In_opt_ const IUnknown* pUnknown)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->SetPrivateDataInterface(Name, pUnknown);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetPrivateData(_In_ REFGUID Name, _Inout_ UINT* pDataSize, _Out_writes_bytes_(*pDataSize) void* pData)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->GetPrivateData(Name, pDataSize, pData);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetParent(_In_ REFIID riid, _COM_Outptr_ void** ppParent)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->GetParent(riid, ppParent);
}

/****IDXGIDeviceSubObject****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDevice(_In_ REFIID riid, _COM_Outptr_ void** ppDevice)
{
	return globals::features::upscaling.dx12SwapChain.GetDevice(riid, ppDevice);
}

/****IDXGISwapChain****/
HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::Present(UINT SyncInterval, UINT Flags)
{
	return globals::features::upscaling.dx12SwapChain.Present(SyncInterval, Flags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetBuffer(UINT, _In_ REFIID, _COM_Outptr_ void** ppSurface)
{
	return globals::features::upscaling.dx12SwapChain.GetBuffer(ppSurface);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::SetFullscreenState(BOOL Fullscreen, _In_opt_ IDXGIOutput* pTarget)
{
	return swapChain->SetFullscreenState(Fullscreen, pTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFullscreenState(_Out_opt_ BOOL* pFullscreen, _COM_Outptr_opt_result_maybenull_ IDXGIOutput** ppTarget)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->GetFullscreenState(pFullscreen, ppTarget);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetDesc(_Out_ DXGI_SWAP_CHAIN_DESC* pDesc)
{
	return swapChain->GetDesc(pDesc);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags)
{
	return swapChain->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::ResizeTarget(_In_ const DXGI_MODE_DESC* pNewTargetParameters)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->ResizeTarget(pNewTargetParameters);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetContainingOutput(_COM_Outptr_ IDXGIOutput** ppOutput)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->GetContainingOutput(ppOutput);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetFrameStatistics(_Out_ DXGI_FRAME_STATISTICS* pStats)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->GetFrameStatistics(pStats);
}

HRESULT STDMETHODCALLTYPE DXGISwapChainProxy::GetLastPresentCount(_Out_ UINT* pLastPresentCount)
{
	return globals::features::upscaling.dx12SwapChain.GetNativeSwapChain()->GetLastPresentCount(pLastPresentCount);
}

void DX12SwapChain::SetUIBuffer()
{
	auto& upscaling = globals::features::upscaling;

	if (upscaling.IsDLSSGBackend()) {
		dlssGHUDLessFrameIndex = UINT32_MAX;
		auto* ui = globals::game::ui;
		const bool mapMenuOpen = ui->IsMenuOpen(RE::MapMenu::MENU_NAME);
		const bool mapRenderingContext = upscaling.IsDLSSGMapRenderingContext();
		const bool sceneSupportsFrameGeneration = mapRenderingContext ||
			(!ui->GameIsPaused() && !mapMenuOpen);
		if (sceneSupportsFrameGeneration &&
			upscaling.IsFrameGenerationEnabled() &&
			upscaling.IsDLSSGAvailable()) {
			// Capture the scene immediately before UI rendering. Keep the game
			// framebuffer bound so FinalColor remains scene plus UI.
			d3d11Context->CopyResource(uiBufferWrapped->resource11, swapChainBufferWrapped->resource11);
			dlssGHUDLessFrameIndex = upscaling.streamline.GetLatchedFrameTokenIndex();
		}
		return;
	}

	if (globals::game::ui->GameIsPaused())
		return;

	auto& data = globals::game::renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGET::kFRAMEBUFFER];
	data.RTV = uiBufferWrapped->rtv;
	d3d11Context->OMSetRenderTargets(1, &data.RTV, nullptr);
}

void DX12SwapChain::MarkDLSSGSceneResourcesReady(uint32_t a_frameIndex)
{
	auto& upscaling = globals::features::upscaling;
	const bool unsupportedMapFrame = globals::game::ui->IsMenuOpen(RE::MapMenu::MENU_NAME) &&
		!upscaling.IsDLSSGMapRenderingContext();
	dlssGSceneResourcesFrameIndex = unsupportedMapFrame ? UINT32_MAX : a_frameIndex;
}

bool DX12SwapChain::DLSSGResourcesReadyForFrame(uint32_t a_frameIndex) const
{
	return a_frameIndex != UINT32_MAX &&
	       dlssGInputExtentFrameIndex == a_frameIndex &&
	       dlssGSceneResourcesFrameIndex == a_frameIndex &&
	       dlssGHUDLessFrameIndex == a_frameIndex;
}

bool DX12SwapChain::UpdateDLSSGPresentationState(bool a_frameGenerationRequested, bool a_mapMenuOpen, bool a_mapRenderingContext)
{
	auto& streamline = globals::features::upscaling.streamline;
	auto resourcesReadyForFrame = [&](uint32_t a_frameIndex) {
		if (DLSSGResourcesReadyForFrame(a_frameIndex)) {
			if (dlssGWaitingForResources) {
				logger::info("[DLSS-G] Token {} has aligned extent, scene, and HUD-less resources; frame generation may resume", a_frameIndex);
				dlssGWaitingForResources = false;
			}
			return true;
		}

		if (!dlssGWaitingForResources) {
			logger::info("[DLSS-G] Holding frame generation at token {} until extent={}, scene={}, and HUD-less={} resources align",
				a_frameIndex,
				dlssGInputExtentFrameIndex,
				dlssGSceneResourcesFrameIndex,
				dlssGHUDLessFrameIndex);
			dlssGWaitingForResources = true;
		}
		return false;
	};

	if (a_mapMenuOpen && a_mapRenderingContext) {
		if (dlssGPresentationState != DLSSGPresentationState::kMapWarmup &&
			dlssGPresentationState != DLSSGPresentationState::kMapGenerating) {
			dlssGPresentationState = DLSSGPresentationState::kMapWarmup;
			dlssGResumeWarmupFrameIndex = streamline.GetLatchedFrameTokenIndex();
			logger::info("[DLSS-G] MapMenu DLSS SR reset frame {} captured; retaining resources while generation remains off for this Present", dlssGResumeWarmupFrameIndex);
			return false;
		}

		if (!a_frameGenerationRequested)
			return false;

		const uint32_t tokenFrameIndex = streamline.GetLatchedFrameTokenIndex();
		if (!resourcesReadyForFrame(tokenFrameIndex))
			return false;

		if (dlssGPresentationState == DLSSGPresentationState::kMapWarmup) {
			if (tokenFrameIndex == dlssGResumeWarmupFrameIndex)
				return false;

			dlssGPresentationState = DLSSGPresentationState::kMapGenerating;
			dlssGResumeWarmupFrameIndex = UINT32_MAX;
			logger::info("[DLSS-G] Resuming MapMenu frame generation at aligned token {}", tokenFrameIndex);
			return true;
		}

		return true;
	}

	if (a_mapMenuOpen) {
		if (dlssGPresentationState != DLSSGPresentationState::kMapSuspended) {
			logger::info("[DLSS-G] Suspending frame generation for MapMenu at token {}", streamline.GetLatchedFrameTokenIndex());
			dlssGPresentationState = DLSSGPresentationState::kMapSuspended;
			dlssGResumeWarmupFrameIndex = UINT32_MAX;
			dlssGMapUnexpectedGeneratedFramesLogged = false;
		}
		return false;
	}

	if (dlssGPresentationState == DLSSGPresentationState::kMapSuspended ||
		dlssGPresentationState == DLSSGPresentationState::kMapWarmup ||
		dlssGPresentationState == DLSSGPresentationState::kMapGenerating) {
		dlssGPresentationState = DLSSGPresentationState::kResumePending;
		dlssGResumeWarmupFrameIndex = UINT32_MAX;
		logger::info("[DLSS-G] MapMenu closed; waiting for aligned world-frame resources before resuming");
	}

	if (!a_frameGenerationRequested) {
		dlssGWaitingForResources = false;
		return false;
	}

	const uint32_t tokenFrameIndex = streamline.GetLatchedFrameTokenIndex();
	if (!resourcesReadyForFrame(tokenFrameIndex))
		return false;

	if (dlssGPresentationState == DLSSGPresentationState::kResumePending) {
		if (dlssGResumeWarmupFrameIndex == UINT32_MAX) {
			dlssGResumeWarmupFrameIndex = tokenFrameIndex;
			logger::info("[DLSS-G] Captured warm-up world frame {}; keeping generation disabled for this Present", tokenFrameIndex);
			return false;
		}

		if (tokenFrameIndex == dlssGResumeWarmupFrameIndex)
			return false;

		dlssGPresentationState = DLSSGPresentationState::kGameplay;
		dlssGResumeWarmupFrameIndex = UINT32_MAX;
		logger::info("[DLSS-G] Resuming frame generation on aligned world frame {}", tokenFrameIndex);
	}

	return true;
}

void DX12SwapChain::SetDLSSGInputExtent(uint32_t a_width, uint32_t a_height, uint32_t a_frameIndex)
{
	const auto inputWidth = std::max(1u, a_width);
	const auto inputHeight = std::max(1u, a_height);
	if (dlssGInputWidth != inputWidth || dlssGInputHeight != inputHeight) {
		logger::info("[DLSS-G] Input extent changed from {}x{} to {}x{} at token {}",
			dlssGInputWidth,
			dlssGInputHeight,
			inputWidth,
			inputHeight,
			a_frameIndex);
	}
	dlssGInputWidth = inputWidth;
	dlssGInputHeight = inputHeight;
	dlssGInputExtentFrameIndex = a_frameIndex;
}

uint32_t DX12SwapChain::GetDLSSGInputWidth() const
{
	return dlssGInputWidth != 0 ? dlssGInputWidth : swapChainDesc.Width;
}

uint32_t DX12SwapChain::GetDLSSGInputHeight() const
{
	return dlssGInputHeight != 0 ? dlssGInputHeight : swapChainDesc.Height;
}

bool DX12SwapChain::HasValidDLSSGInputExtent() const
{
	return dlssGInputWidth != 0 && dlssGInputHeight != 0 && dlssGInputExtentFrameIndex != UINT32_MAX;
}

void DX12SwapChain::CreateSharedResources()
{
	dlssGInputWidth = 0;
	dlssGInputHeight = 0;
	dlssGInputExtentFrameIndex = UINT32_MAX;
	dlssGSceneResourcesFrameIndex = UINT32_MAX;
	dlssGHUDLessFrameIndex = UINT32_MAX;
	dlssGWaitingForResources = false;

	auto renderer = globals::game::renderer;

	// Create depth buffer
	auto& main = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMAIN];
	D3D11_TEXTURE2D_DESC texDesc{};
	main.texture->GetDesc(&texDesc);
	inputColorBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	outputColorBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	packedNormalShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	nisSharpenerInputShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	nisSharpenerOutputShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	colorBeforeTransparencySnapshot = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());

	texDesc.Format = DXGI_FORMAT_R32_FLOAT;
	depthBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	specHitDistanceShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());

	texDesc.Format = DXGI_FORMAT_R16_FLOAT;
	sssGuide = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());

	texDesc.Format = DXGI_FORMAT_R8_UNORM;
	reactiveMaskShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
	transparencyCompositionMaskShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());

	texDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
	albedoShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());

	texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	reflectanceShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());

	// Create motion vector buffer
	auto& motionVector = renderer->GetRuntimeData().renderTargets[RE::RENDER_TARGETS::kMOTION_VECTOR];
	motionVector.texture->GetDesc(&texDesc);
	motionVectorBufferShared12 = new WrappedResource(texDesc, d3d11Device.get(), d3d12Device.get());
}
