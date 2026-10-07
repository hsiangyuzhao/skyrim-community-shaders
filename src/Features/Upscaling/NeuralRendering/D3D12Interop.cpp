// Ported from https://github.com/YtzyFvra/skyrim-community-shaders (branch feature/dlssnr-vr),
// an experimental DLSS Neural Rendering integration on the Open Shaders dev branch. Both that
// tree and this one are forks of Community Shaders. See NOTICE.md in this directory for the
// list of changes made during the port.
#include "D3D12Interop.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <utility>

#include <Windows.h>

namespace NeuralRendering
{
	D3D12Interop::~D3D12Interop() { Shutdown(); }

	bool D3D12Interop::RecordFailure(HRESULT result)
	{
		lastError_ = result;
		return false;
	}

	bool D3D12Interop::Initialize(IDXGIAdapter* adapter, ID3D11Device* device, ID3D11DeviceContext* context)
	{
		Shutdown();
		if (!adapter || !device || !context)
			return RecordFailure(E_INVALIDARG);

		HRESULT result = device->QueryInterface(IID_PPV_ARGS(&device11_));
		if (FAILED(result)) return RecordFailure(result);
		result = context->QueryInterface(IID_PPV_ARGS(&context11_));
		if (FAILED(result)) return RecordFailure(result);
		result = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device12_));
		if (FAILED(result)) return RecordFailure(result);

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		result = device12_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue12_));
		if (FAILED(result)) return RecordFailure(result);
		for (auto& commandContext : commandContexts_) {
			result = device12_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
				IID_PPV_ARGS(&commandContext.allocator));
			if (FAILED(result)) return RecordFailure(result);
			result = device12_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
				commandContext.allocator.Get(), nullptr, IID_PPV_ARGS(&commandContext.commandList));
			if (FAILED(result)) return RecordFailure(result);
			result = commandContext.commandList->Close();
			if (FAILED(result)) return RecordFailure(result);
		}

		result = device12_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12_));
		if (FAILED(result)) return RecordFailure(result);
		HANDLE sharedFence = nullptr;
		result = device12_->CreateSharedHandle(fence12_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence);
		if (FAILED(result)) return RecordFailure(result);
		result = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&fence11_));
		CloseHandle(sharedFence);
		if (FAILED(result)) return RecordFailure(result);

		fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (!fenceEvent_) return RecordFailure(HRESULT_FROM_WIN32(GetLastError()));

		// (batch 38a) Timestamps on the network's own queue, so the cost of the model itself can be
		// told apart from the copies and the cross-device wait that the D3D11 overlay row also
		// contains. Purely diagnostic: a failure here only means no model-only number.
		{
			D3D12_QUERY_HEAP_DESC heapDesc{};
			heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			heapDesc.Count = static_cast<UINT>(kCommandContextCount * 2);
			D3D12_HEAP_PROPERTIES readbackHeap{};
			readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
			D3D12_RESOURCE_DESC bufferDesc{};
			bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			bufferDesc.Width = sizeof(std::uint64_t) * kCommandContextCount * 2;
			bufferDesc.Height = 1;
			bufferDesc.DepthOrArraySize = 1;
			bufferDesc.MipLevels = 1;
			bufferDesc.SampleDesc.Count = 1;
			bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			UINT64 frequency = 0;
			if (FAILED(device12_->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&timestampHeap_))) ||
				FAILED(device12_->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
					D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&timestampReadback_))) ||
				FAILED(queue12_->GetTimestampFrequency(&frequency)) || frequency == 0) {
				timestampHeap_.Reset();
				timestampReadback_.Reset();
				timestampFrequency_ = 0;
				logger::warn("[DLSSNR] D3D12 timestamps unavailable; the model-only GPU time will not be shown");
			} else {
				timestampFrequency_ = frequency;
			}
		}

		initialized_ = true;
		lastError_ = S_OK;
		logger::info("[DLSSNR] D3D12 interop initialized commandContexts={} cpuFenceWait=backpressure-only timestamps={}",
			kCommandContextCount, timestampFrequency_ != 0);
		return true;
	}

	void D3D12Interop::Shutdown()
	{
		if (fenceEvent_) CloseHandle(fenceEvent_);
		fenceEvent_ = nullptr;
		recording_ = false;
		recordingContext_ = kCommandContextCount;
		commandContextCursor_ = 0;
		backpressureLogged_ = false;
		initialized_ = false;
		fenceValue_ = 0;
		lastGpuMs_ = 0.0f;
		smoothedGpuMs_ = 0.0f;
		maxInFlight_ = 0;
		backpressureWaits_ = 0;
		fence11_.Reset();
		fence12_.Reset();
		commandContexts_ = {};
		timestampHeap_.Reset();
		timestampReadback_.Reset();
		timestampFrequency_ = 0;
		queue12_.Reset();
		// Counted after every child of ours is gone, right before our own reference goes.
		lastShutdownOtherReferences_ = OtherDeviceReferences();
		device12_.Reset();
		context11_.Reset();
		device11_.Reset();
	}

	bool D3D12Interop::CreateSharedTexture(const D3D11_TEXTURE2D_DESC& sourceDesc, SharedTexture& texture, const char* name)
	{
		if (!initialized_ || sourceDesc.Width == 0 || sourceDesc.Height == 0 ||
			sourceDesc.ArraySize == 0 || sourceDesc.ArraySize > UINT16_MAX || sourceDesc.MipLevels > UINT16_MAX)
			return RecordFailure(E_INVALIDARG);

		D3D11_TEXTURE2D_DESC desc = sourceDesc;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.CPUAccessFlags = 0;
		desc.MiscFlags = 0;
		if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) == 0) {
			lastOperation_ = "CreateSharedTextureRequiresUAV";
			return RecordFailure(E_INVALIDARG);
		}

		SharedTexture replacement;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT result = device11_->CreateTexture2D(&desc, nullptr, &replacement.resource11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateTexture2D";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.resource11.Get(), name);
		result = device11_->CreateUnorderedAccessView(replacement.resource11.Get(), nullptr, &replacement.uav11);
		if (FAILED(result)) {
			lastOperation_ = "D3D11CreateUnorderedAccessView";
			return RecordFailure(result);
		}
		Util::SetResourceName(replacement.uav11.Get(), "%s UAV", name);
		// Best effort: only the before-upscaling decode reads these views, and its textures are typed
		// RGBA8. A format with no default view (typeless) must not fail the textures the 37c path
		// has always created without one.
		if (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
			if (SUCCEEDED(device11_->CreateShaderResourceView(replacement.resource11.Get(), nullptr, &replacement.srv11)))
				Util::SetResourceName(replacement.srv11.Get(), "%s SRV", name);
			else
				replacement.srv11.Reset();
		}

		Microsoft::WRL::ComPtr<IDXGIResource1> dxgiResource;
		result = replacement.resource11.As(&dxgiResource);
		if (FAILED(result)) return RecordFailure(result);
		HANDLE sharedTexture = nullptr;
		result = dxgiResource->CreateSharedHandle(nullptr,
			DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sharedTexture);
		if (FAILED(result)) return RecordFailure(result);
		result = device12_->OpenSharedHandle(sharedTexture, IID_PPV_ARGS(&replacement.resource12));
		CloseHandle(sharedTexture);
		if (FAILED(result)) return RecordFailure(result);

		lastResourceFlags_ = static_cast<std::uint32_t>(replacement.resource12->GetDesc().Flags);
		replacement.desc = desc;
		texture = std::move(replacement);
		lastOperation_ = "CreateSharedTexture(D3D11First)";
		lastError_ = S_OK;
		return true;
	}

	void D3D12Interop::CollectTimestamps(std::size_t contextIndex)
	{
		auto& commandContext = commandContexts_[contextIndex];
		if (!commandContext.timestampsPending || !timestampReadback_ || timestampFrequency_ == 0)
			return;
		commandContext.timestampsPending = false;
		const SIZE_T offset = sizeof(std::uint64_t) * 2 * contextIndex;
		D3D12_RANGE readRange{ offset, offset + sizeof(std::uint64_t) * 2 };
		void* mapped = nullptr;
		if (FAILED(timestampReadback_->Map(0, &readRange, &mapped)) || !mapped)
			return;
		const auto* ticks = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(mapped) + offset);
		const std::uint64_t begin = ticks[0];
		const std::uint64_t end = ticks[1];
		D3D12_RANGE writtenRange{ 0, 0 };
		timestampReadback_->Unmap(0, &writtenRange);
		if (end <= begin)
			return;
		lastGpuMs_ = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(timestampFrequency_));
		smoothedGpuMs_ = smoothedGpuMs_ > 0.0f ? smoothedGpuMs_ * 0.95f + lastGpuMs_ * 0.05f : lastGpuMs_;
	}

	bool D3D12Interop::BeginD3D12(ID3D12GraphicsCommandList** commandList, std::size_t activeContexts)
	{
		if (!initialized_ || recording_ || !commandList) return RecordFailure(E_UNEXPECTED);
		activeContexts = std::clamp<std::size_t>(activeContexts, 1, kCommandContextCount);

		// Every submission is fenced. A context whose submission has finished can be reused, and its
		// timestamps read, without the CPU waiting on anything.
		const std::uint64_t completedValue = fence12_->GetCompletedValue();
		std::uint32_t inFlight = 0;
		for (std::size_t index = 0; index < kCommandContextCount; ++index) {
			const auto pendingValue = commandContexts_[index].fenceValue;
			if (pendingValue != 0 && completedValue < pendingValue)
				++inFlight;
			else
				CollectTimestamps(index);
		}
		maxInFlight_ = std::max(maxInFlight_, inFlight);

		std::size_t contextIndex = kCommandContextCount;
		const std::size_t cursor = commandContextCursor_ % activeContexts;
		for (std::size_t offset = 0; offset < activeContexts; ++offset) {
			const std::size_t candidate = (cursor + offset) % activeContexts;
			const auto pendingValue = commandContexts_[candidate].fenceValue;
			if (pendingValue == 0 || completedValue >= pendingValue) {
				contextIndex = candidate;
				break;
			}
		}
		if (contextIndex == kCommandContextCount) {
			// Every context is still executing: the CPU is a whole ring of frames ahead of the
			// network. The 37c ring was three, which is as many frames as a GPU-bound game keeps
			// queued under DXGI's default frame latency, so this fired in normal play and became a
			// CPU stall in the middle of the frame -- ahead of Present, where Reflex and DLSS-G
			// expect the throttle to be. With the larger ring it means the GPU really fell that far
			// behind, and waiting is the right thing to do.
			contextIndex = cursor;
			++backpressureWaits_;
			if (!backpressureLogged_) {
				logger::warn("[DLSSNR] D3D12 command contexts saturated; applying CPU backpressure (ring={} inFlight={} completed={} waitingFor={})",
					activeContexts, inFlight, completedValue, commandContexts_[contextIndex].fenceValue);
				backpressureLogged_ = true;
			}
			if (!WaitForFence(commandContexts_[contextIndex].fenceValue, activeContexts > kLegacyCommandContextCount ? 2000 : 250))
				return false;
			CollectTimestamps(contextIndex);
		}

		auto& commandContext = commandContexts_[contextIndex];
		commandContext.fenceValue = 0;
		const std::uint64_t readyValue = ++fenceValue_;
		HRESULT result = context11_->Signal(fence11_.Get(), readyValue);
		if (FAILED(result)) return RecordFailure(result);
		result = queue12_->Wait(fence12_.Get(), readyValue);
		if (FAILED(result)) return RecordFailure(result);
		result = commandContext.allocator->Reset();
		if (FAILED(result)) return RecordFailure(result);
		result = commandContext.commandList->Reset(commandContext.allocator.Get(), nullptr);
		if (FAILED(result)) return RecordFailure(result);
		if (timestampHeap_)
			commandContext.commandList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(contextIndex * 2));
		recording_ = true;
		recordingContext_ = contextIndex;
		commandContextCursor_ = (contextIndex + 1) % activeContexts;
		*commandList = commandContext.commandList.Get();
		return true;
	}

	bool D3D12Interop::EndD3D12()
	{
		if (!initialized_ || !recording_ || recordingContext_ >= kCommandContextCount)
			return RecordFailure(E_UNEXPECTED);
		recording_ = false;
		const std::size_t contextIndex = recordingContext_;
		auto& commandContext = commandContexts_[contextIndex];
		recordingContext_ = kCommandContextCount;
		if (timestampHeap_ && timestampReadback_) {
			commandContext.commandList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(contextIndex * 2 + 1));
			commandContext.commandList->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
				static_cast<UINT>(contextIndex * 2), 2, timestampReadback_.Get(), sizeof(std::uint64_t) * 2 * contextIndex);
			commandContext.timestampsPending = true;
		}
		HRESULT result = commandContext.commandList->Close();
		if (FAILED(result)) return RecordFailure(result);
		ID3D12CommandList* lists[] = { commandContext.commandList.Get() };
		queue12_->ExecuteCommandLists(1, lists);
		const std::uint64_t completeValue = ++fenceValue_;
		result = queue12_->Signal(fence12_.Get(), completeValue);
		if (FAILED(result)) return RecordFailure(result);
		commandContext.fenceValue = completeValue;

		// This queues a GPU-side dependency. Subsequent D3D11 output copies wait
		// for Feature 18 without stalling the render thread on the CPU.
		result = context11_->Wait(fence11_.Get(), completeValue);
		if (FAILED(result)) return RecordFailure(result);
		lastError_ = S_OK;
		return true;
	}

	bool D3D12Interop::WaitForFence(std::uint64_t value, DWORD timeoutMs)
	{
		if (!value || fence12_->GetCompletedValue() >= value)
			return true;
		const HRESULT result = fence12_->SetEventOnCompletion(value, fenceEvent_);
		if (FAILED(result)) return RecordFailure(result);
		const DWORD waitResult = WaitForSingleObject(fenceEvent_, timeoutMs);
		if (waitResult != WAIT_OBJECT_0)
			return RecordFailure(waitResult == WAIT_TIMEOUT ? HRESULT_FROM_WIN32(ERROR_TIMEOUT) :
			                                                        HRESULT_FROM_WIN32(GetLastError()));
		lastError_ = S_OK;
		return true;
	}

	std::uint32_t D3D12Interop::OtherDeviceReferences() const
	{
		if (!device12_)
			return 0;
		device12_->AddRef();
		const ULONG references = device12_->Release();
		return references > 1 ? static_cast<std::uint32_t>(references - 1) : 0u;
	}

	bool D3D12Interop::WaitForIdle()
	{
		if (!initialized_)
			return true;
		std::uint64_t lastSubmittedValue = 0;
		for (const auto& commandContext : commandContexts_)
			lastSubmittedValue = std::max(lastSubmittedValue, commandContext.fenceValue);
		// A teardown waits for everything, not a ring's worth, so it gets a longer timeout than the
		// per-frame wait: the network on its own can take tens of milliseconds.
		return WaitForFence(lastSubmittedValue, 2000);
	}
}
