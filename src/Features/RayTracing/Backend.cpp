#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <dxgi1_4.h>

// Generated at build time from Shaders/TraceShadowsCS.hlsl by the Windows SDK's dxc (CMakeLists.txt).
#include "RayTracing/TraceShadowsCS.dxil.h"

namespace RayTracingRT
{
	namespace
	{
		constexpr UINT kRootConstants = sizeof(TraceConstants) / 4;

		std::string HrString(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			return barrier;
		}

		D3D12_RESOURCE_BARRIER UavBarrier(ID3D12Resource* a_resource = nullptr)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = a_resource;
			return barrier;
		}

		std::uint64_t Align(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) & ~(a_alignment - 1); }
	}

	// Process exit: no GPU wait (the GPU work of a closing game does not matter any more).
	Backend::~Backend() { Shutdown(false); }

	bool Backend::Fail(const char* a_operation, HRESULT a_result)
	{
		lastError_ = std::format("{} failed ({})", a_operation, HrString(a_result));
		if (device_) {
			const HRESULT removed = device_->GetDeviceRemovedReason();
			if (FAILED(removed))
				lastError_ += std::format(", device removed reason {}", HrString(removed));
		}
		logger::error("[RayTracing] {}", lastError_);
		failed_ = true;
		return false;
	}

	bool Backend::Initialize(ID3D11Device* a_device, ID3D11DeviceContext* a_context, std::string& o_reason)
	{
		Shutdown(false);
		failed_ = false;
		lastError_.clear();
		auto fail = [&](std::string a_reason) {
			o_reason = std::move(a_reason);
			logger::warn("[RayTracing] Not available: {}", o_reason);
			Shutdown(false);
			return false;
		};

		if (!a_device || !a_context)
			return fail("no D3D11 device");
		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(&device11_))) || FAILED(a_context->QueryInterface(IID_PPV_ARGS(&context11_))))
			return fail("D3D11.4 device / context interfaces missing (needs Windows 10 1703 or newer)");

		ComPtr<IDXGIDevice> dxgiDevice;
		ComPtr<IDXGIAdapter> adapter;
		if (FAILED(a_device->QueryInterface(IID_PPV_ARGS(&dxgiDevice))) || FAILED(dxgiDevice->GetAdapter(&adapter)))
			return fail("could not get the game's DXGI adapter");
		DXGI_ADAPTER_DESC adapterDesc{};
		adapter->GetDesc(&adapterDesc);
		{
			char name[128]{};
			WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
			adapterDescription_ = name;
		}

		// Same adapter as the game (the LUID check guards against a hybrid-GPU surprise).
		ComPtr<ID3D12Device> device;
		HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
		if (FAILED(hr))
			return fail(std::format("D3D12CreateDevice failed ({})", HrString(hr)));
		const LUID luid = device->GetAdapterLuid();
		if (luid.LowPart != adapterDesc.AdapterLuid.LowPart || luid.HighPart != adapterDesc.AdapterLuid.HighPart)
			return fail("D3D12 device is on a different adapter than the game");
		if (FAILED(device.As(&device_)))
			return fail("ID3D12Device5 not available (driver too old for DXR)");

		D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5{};
		if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5))) ||
			options5.RaytracingTier < D3D12_RAYTRACING_TIER_1_1)
			return fail(std::format("GPU / driver reports DXR tier {} (needs 1.1 for inline ray queries)",
				options5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED ? std::string("none") :
				options5.RaytracingTier == D3D12_RAYTRACING_TIER_1_0           ? std::string("1.0") :
				                                                                  std::to_string(static_cast<int>(options5.RaytracingTier))));
		D3D12_FEATURE_DATA_SHADER_MODEL shaderModel{ D3D_SHADER_MODEL_6_5 };
		if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &shaderModel, sizeof(shaderModel))) ||
			shaderModel.HighestShaderModel < D3D_SHADER_MODEL_6_5)
			return fail("shader model 6.5 not supported");

		D3D12_COMMAND_QUEUE_DESC queueDesc{};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		if (FAILED(hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_))))
			return fail(std::format("CreateCommandQueue failed ({})", HrString(hr)));
		queue_->SetName(L"CS RayTracing Queue");

		for (auto& context : contexts_) {
			if (FAILED(hr = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&context.allocator))))
				return fail(std::format("CreateCommandAllocator failed ({})", HrString(hr)));
			ComPtr<ID3D12GraphicsCommandList> list;
			if (FAILED(hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.Get(), nullptr, IID_PPV_ARGS(&list))))
				return fail(std::format("CreateCommandList failed ({})", HrString(hr)));
			if (FAILED(list.As(&context.list)))
				return fail("ID3D12GraphicsCommandList4 not available");
			context.list->Close();
		}

		if (FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence_))))
			return fail(std::format("CreateFence failed ({})", HrString(hr)));
		HANDLE sharedFence = nullptr;
		if (FAILED(hr = device_->CreateSharedHandle(fence_.Get(), nullptr, GENERIC_ALL, nullptr, &sharedFence)))
			return fail(std::format("shared fence handle failed ({})", HrString(hr)));
		hr = device11_->OpenSharedFence(sharedFence, IID_PPV_ARGS(&fence11_));
		CloseHandle(sharedFence);
		if (FAILED(hr))
			return fail(std::format("D3D11 could not open the shared fence ({})", HrString(hr)));
		fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

		// Diagnostics only: the GPU time of our own queue's submissions.
		{
			D3D12_QUERY_HEAP_DESC heapDesc{};
			heapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
			heapDesc.Count = static_cast<UINT>(kContexts * 2);
			UINT64 frequency = 0;
			timestampReadback_ = CreateBuffer(sizeof(std::uint64_t) * kContexts * 2, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
			if (FAILED(device_->CreateQueryHeap(&heapDesc, IID_PPV_ARGS(&timestampHeap_))) || !timestampReadback_ ||
				FAILED(queue_->GetTimestampFrequency(&frequency)) || frequency == 0) {
				timestampHeap_.Reset();
				timestampReadback_.Reset();
				timestampFrequency_ = 0;
			} else {
				timestampFrequency_ = frequency;
			}
		}

		postbuild_ = CreateBuffer(sizeof(std::uint64_t) * kPostbuildSlots, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
		postbuildReadback_ = CreateBuffer(sizeof(std::uint64_t) * kPostbuildSlots, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
		if (!postbuild_ || !postbuildReadback_)
			return fail("could not create the BLAS compaction buffers");
		{
			void* mapped = nullptr;
			if (FAILED(postbuildReadback_->Map(0, nullptr, &mapped)) || !mapped)
				return fail("could not map the BLAS compaction readback buffer");
			postbuildMapped_ = static_cast<const std::uint64_t*>(mapped);
		}

		std::string pipelineReason;
		if (!CreatePipeline(pipelineReason))
			return fail(pipelineReason);

		ready_ = true;
		failed_ = false;
		logger::info("[RayTracing] D3D12 ready on {} (DXR tier 1.1, SM 6.5, {} command contexts, timestamps {})",
			adapterDescription_, kContexts, timestampFrequency_ != 0);
		return true;
	}

	bool Backend::CreatePipeline(std::string& o_reason)
	{
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 1;
		ranges[0].BaseShaderRegister = 1;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].OffsetInDescriptorsFromTableStart = 1;

		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = kRootConstants;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 2;
		params[2].DescriptorTable.pDescriptorRanges = ranges;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 3;
		rootDesc.pParameters = params;
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (FAILED(hr)) {
			o_reason = std::format("root signature serialization failed ({}): {}", HrString(hr),
				error ? std::string(static_cast<const char*>(error->GetBufferPointer()), error->GetBufferSize()) : std::string());
			return false;
		}
		if (FAILED(hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rootSignature_)))) {
			o_reason = std::format("CreateRootSignature failed ({})", HrString(hr));
			return false;
		}

		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = rootSignature_.Get();
		psoDesc.CS.pShaderBytecode = g_TraceShadowsCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TraceShadowsCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pipeline_)))) {
			o_reason = std::format("the ray query shader was rejected by the driver ({})", HrString(hr));
			return false;
		}

		D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
		heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		heapDesc.NumDescriptors = 2;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)))) {
			o_reason = std::format("CreateDescriptorHeap failed ({})", HrString(hr));
			return false;
		}
		descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		return true;
	}

	void Backend::Shutdown(bool a_waitForGpu)
	{
		if (a_waitForGpu && fence_ && fenceEvent_ && lastSubmitted_ && fence_->GetCompletedValue() < lastSubmitted_) {
			if (SUCCEEDED(fence_->SetEventOnCompletion(lastSubmitted_, fenceEvent_)))
				WaitForSingleObject(fenceEvent_, 2000);
		}
		ready_ = false;
		if (postbuildReadback_ && postbuildMapped_)
			postbuildReadback_->Unmap(0, nullptr);
		postbuildMapped_ = nullptr;
		blas_.clear();
		freeHandles_.clear();
		buildQueue_.clear();
		retired_.clear();
		for (auto& context : contexts_) {
			if (context.instanceBuffer && context.instanceMapped)
				context.instanceBuffer->Unmap(0, nullptr);
			context = {};
		}
		depthMask_ = {};
		visibility_ = {};
		textureWidth_ = textureHeight_ = 0;
		tlas_.Reset();
		scratch_.Reset();
		tlasBytes_ = scratchBytes_ = 0;
		tlasCapacity_ = 0;
		postbuild_.Reset();
		postbuildReadback_.Reset();
		timestampHeap_.Reset();
		timestampReadback_.Reset();
		descriptorHeap_.Reset();
		pipeline_.Reset();
		rootSignature_.Reset();
		fence11_.Reset();
		fence_.Reset();
		queue_.Reset();
		device_.Reset();
		context11_.Reset();
		device11_.Reset();
		if (fenceEvent_)
			CloseHandle(fenceEvent_);
		fenceEvent_ = nullptr;
		fenceValue_ = lastSubmitted_ = 0;
		cursor_ = 0;
		stats_ = {};
	}

	ComPtr<ID3D12Resource> Backend::CreateBuffer(std::uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, bool a_uav)
	{
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = a_heap;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Width = std::max<std::uint64_t>(a_bytes, 256);
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = a_uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
		ComPtr<ID3D12Resource> resource;
		if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, a_state, nullptr, IID_PPV_ARGS(&resource))))
			return nullptr;
		return resource;
	}

	bool Backend::CreateSharedTexture(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, SharedTexture& o_texture, const char* a_name)
	{
		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = a_width;
		desc.Height = a_height;
		desc.MipLevels = 1;
		desc.ArraySize = 1;
		desc.Format = a_format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		SharedTexture texture;
		HRESULT hr = device11_->CreateTexture2D(&desc, nullptr, &texture.resource11);
		if (FAILED(hr))
			return Fail("CreateTexture2D (shared)", hr);
		Util::SetResourceName(texture.resource11.Get(), a_name);
		if (FAILED(hr = device11_->CreateShaderResourceView(texture.resource11.Get(), nullptr, &texture.srv11)) ||
			FAILED(hr = device11_->CreateUnorderedAccessView(texture.resource11.Get(), nullptr, &texture.uav11)))
			return Fail("shared texture views", hr);
		ComPtr<IDXGIResource1> dxgiResource;
		if (FAILED(hr = texture.resource11.As(&dxgiResource)))
			return Fail("IDXGIResource1", hr);
		HANDLE handle = nullptr;
		if (FAILED(hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
			return Fail("CreateSharedHandle (texture)", hr);
		hr = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&texture.resource12));
		CloseHandle(handle);
		if (FAILED(hr))
			return Fail("OpenSharedHandle (texture)", hr);
		o_texture = std::move(texture);
		return true;
	}

	bool Backend::EnsureTextures(std::uint32_t a_width, std::uint32_t a_height, std::string& o_reason)
	{
		if (!ready_)
			return false;
		if (a_width == textureWidth_ && a_height == textureHeight_ && depthMask_.resource12 && visibility_.resource12)
			return true;
		// The old pair may still be read by a submission in flight: D3D12 keeps them until it is done.
		if (depthMask_.resource12)
			Retire(depthMask_.resource12, 0, nullptr);
		if (visibility_.resource12)
			Retire(visibility_.resource12, 0, nullptr);
		depthMask_ = {};
		visibility_ = {};
		textureWidth_ = textureHeight_ = 0;
		if (!CreateSharedTexture(a_width, a_height, DXGI_FORMAT_R32G32_FLOAT, depthMask_, "RayTracing DepthMask") ||
			!CreateSharedTexture(a_width, a_height, DXGI_FORMAT_R8G8_UNORM, visibility_, "RayTracing Visibility")) {
			o_reason = lastError_;
			return false;
		}
		textureWidth_ = a_width;
		textureHeight_ = a_height;
		stats_.sharedTextureBytes = static_cast<std::uint64_t>(a_width) * a_height * (8 + 2);
		WriteDescriptors();
		logger::info("[RayTracing] Shared textures {}x{}", a_width, a_height);
		return true;
	}

	void Backend::WriteDescriptors()
	{
		auto cpu = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(depthMask_.resource12.Get(), &srvDesc, cpu);
		cpu.ptr += descriptorSize_;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = DXGI_FORMAT_R8G8_UNORM;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(visibility_.resource12.Get(), nullptr, &uavDesc, cpu);
	}

	void Backend::Retire(ComPtr<ID3D12Pageable> a_object, std::uint64_t a_bytes, std::uint64_t* a_counter)
	{
		if (!a_object)
			return;
		// The next value anyone signals: the done value of the list being recorded, or the next
		// frame's D3D11 signal, which the D3D11 queue only reaches after waiting for every
		// D3D12 submission so far. Either way everything that may use the object is finished.
		retired_.push_back(Retired{ fenceValue_ + 1, std::move(a_object), a_bytes, a_counter });
	}

	void Backend::CollectRetired()
	{
		if (!fence_)
			return;
		const std::uint64_t completed = fence_->GetCompletedValue();
		std::erase_if(retired_, [&](Retired& a_entry) {
			if (a_entry.fence > completed)
				return false;
			if (a_entry.counter)
				*a_entry.counter -= std::min(*a_entry.counter, a_entry.bytes);
			return true;
		});
	}

	void Backend::CollectTimestamps(std::size_t a_index)
	{
		auto& context = contexts_[a_index];
		if (!context.timestampsPending || !timestampReadback_ || timestampFrequency_ == 0)
			return;
		context.timestampsPending = false;
		const SIZE_T offset = sizeof(std::uint64_t) * 2 * a_index;
		D3D12_RANGE range{ offset, offset + sizeof(std::uint64_t) * 2 };
		void* mapped = nullptr;
		if (FAILED(timestampReadback_->Map(0, &range, &mapped)) || !mapped)
			return;
		const auto* ticks = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(mapped) + offset);
		const std::uint64_t begin = ticks[0], end = ticks[1];
		D3D12_RANGE written{ 0, 0 };
		timestampReadback_->Unmap(0, &written);
		if (end <= begin)
			return;
		stats_.lastGpuMs = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(timestampFrequency_));
		stats_.smoothedGpuMs = stats_.smoothedGpuMs > 0.0f ? stats_.smoothedGpuMs * 0.95f + stats_.lastGpuMs * 0.05f : stats_.lastGpuMs;
	}

	BlasHandle Backend::AllocateHandle()
	{
		if (!freeHandles_.empty()) {
			const BlasHandle handle = freeHandles_.back();
			freeHandles_.pop_back();
			return handle;
		}
		blas_.emplace_back();
		return static_cast<BlasHandle>(blas_.size() - 1);
	}

	BlasHandle Backend::CreateBlas(std::unique_ptr<ChunkGeometry> a_geometry)
	{
		if (!ready_ || !a_geometry || a_geometry->Triangles() == 0)
			return kInvalidBlas;
		const BlasHandle handle = AllocateHandle();
		auto& blas = blas_[handle];
		blas = {};
		blas.state = BlasState::Queued;
		blas.triangles = a_geometry->Triangles();
		blas.geometry = std::move(a_geometry);
		buildQueue_.push_back(handle);
		++stats_.blasQueued;
		return handle;
	}

	void Backend::ReleaseBlas(BlasHandle a_handle)
	{
		if (a_handle >= blas_.size() || blas_[a_handle].state == BlasState::Free)
			return;
		auto& blas = blas_[a_handle];
		if (blas.state == BlasState::Queued) {
			std::erase(buildQueue_, a_handle);
			stats_.blasQueued -= std::min<std::uint32_t>(stats_.blasQueued, 1);
		} else {
			stats_.blasTriangles -= std::min<std::uint64_t>(stats_.blasTriangles, blas.triangles);
			stats_.blasCount -= std::min<std::uint32_t>(stats_.blasCount, 1);
		}
		if (blas.resource)
			Retire(blas.resource, blas.bytes, &stats_.blasBytes);
		blas = {};
		freeHandles_.push_back(a_handle);
	}

	void Backend::ReleaseAllBlas()
	{
		for (BlasHandle handle = 0; handle < blas_.size(); ++handle)
			ReleaseBlas(handle);
	}

	bool Backend::IsBlasBuilt(BlasHandle a_handle) const
	{
		return a_handle < blas_.size() && (blas_[a_handle].state == BlasState::Built || blas_[a_handle].state == BlasState::Ready);
	}

	bool Backend::EnsureScratch(std::uint64_t a_bytes)
	{
		if (scratch_ && scratchBytes_ >= a_bytes)
			return true;
		if (scratch_)
			Retire(scratch_, scratchBytes_, &stats_.scratchBytes);
		const std::uint64_t bytes = Align(std::max<std::uint64_t>(a_bytes, 8ull << 20), 64 * 1024);
		scratch_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
		if (!scratch_) {
			scratchBytes_ = 0;
			return Fail("scratch buffer", E_OUTOFMEMORY);
		}
		scratch_->SetName(L"CS RayTracing Scratch");
		scratchBytes_ = bytes;
		stats_.scratchBytes += bytes;
		return true;
	}

	bool Backend::EnsureTlas(std::uint32_t a_instances)
	{
		if (tlas_ && tlasCapacity_ >= a_instances)
			return true;
		const std::uint32_t capacity = std::max<std::uint32_t>(256, std::bit_ceil(a_instances));
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
		inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		inputs.NumDescs = capacity;
		inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
		device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
		if (tlas_)
			Retire(tlas_, tlasBytes_, &stats_.tlasBytes);
		const std::uint64_t bytes = Align(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		tlas_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
		if (!tlas_ || !EnsureScratch(info.ScratchDataSizeInBytes)) {
			tlasBytes_ = 0;
			tlasCapacity_ = 0;
			return Fail("TLAS buffer", E_OUTOFMEMORY);
		}
		tlas_->SetName(L"CS RayTracing TLAS");
		tlasBytes_ = bytes;
		stats_.tlasBytes += bytes;
		tlasCapacity_ = capacity;
		return true;
	}

	bool Backend::EnsureInstanceBuffer(CommandContext& a_context, std::uint32_t a_instances)
	{
		if (a_context.instanceBuffer && a_context.instanceCapacity >= a_instances)
			return true;
		const std::uint32_t capacity = std::max<std::uint32_t>(256, std::bit_ceil(a_instances));
		const std::uint64_t bytes = static_cast<std::uint64_t>(capacity) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
		if (a_context.instanceBuffer) {
			a_context.instanceBuffer->Unmap(0, nullptr);
			// This context's previous submission has completed (the context is free), so it can go now.
			stats_.uploadBytes -= std::min<std::uint64_t>(stats_.uploadBytes, static_cast<std::uint64_t>(a_context.instanceCapacity) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
			a_context.instanceBuffer.Reset();
			a_context.instanceMapped = nullptr;
			a_context.instanceCapacity = 0;
		}
		a_context.instanceBuffer = CreateBuffer(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		if (!a_context.instanceBuffer || FAILED(a_context.instanceBuffer->Map(0, nullptr, &a_context.instanceMapped)))
			return Fail("instance buffer", E_OUTOFMEMORY);
		a_context.instanceCapacity = capacity;
		stats_.uploadBytes += bytes;
		return true;
	}

	void Backend::ProcessCompaction(ID3D12GraphicsCommandList4* a_list, std::uint64_t a_completed)
	{
		stats_.compactionsLastFrame = 0;
		for (auto& blas : blas_) {
			if (blas.state != BlasState::Built || blas.buildFence == 0 || blas.buildFence > a_completed)
				continue;
			const std::uint64_t compactedSize = blas.postbuildSlot < kPostbuildSlots ? postbuildMapped_[blas.postbuildSlot] : 0;
			blas.postbuildSlot = UINT32_MAX;
			const std::uint64_t bytes = Align(compactedSize, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			// Not worth a second buffer when it saves under 10 %.
			if (compactedSize == 0 || bytes * 10 >= blas.bytes * 9) {
				blas.state = BlasState::Ready;
				continue;
			}
			auto compacted = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
			if (!compacted) {
				blas.state = BlasState::Ready;
				continue;
			}
			compacted->SetName(L"CS RayTracing BLAS (compacted)");
			a_list->CopyRaytracingAccelerationStructure(compacted->GetGPUVirtualAddress(), blas.resource->GetGPUVirtualAddress(),
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
			stats_.compactedSavedBytes += blas.bytes - bytes;
			Retire(blas.resource, blas.bytes, &stats_.blasBytes);
			blas.resource = std::move(compacted);
			blas.bytes = bytes;
			stats_.blasBytes += bytes;
			blas.state = BlasState::Ready;
			++stats_.compactionsLastFrame;
		}
	}

	std::uint32_t Backend::BuildQueued(ID3D12GraphicsCommandList4* a_list, std::uint32_t a_budget)
	{
		std::uint32_t built = 0;
		std::uint32_t triangles = 0;
		std::vector<std::uint32_t> slots;
		while (!buildQueue_.empty() && (built == 0 || triangles < a_budget)) {
			const BlasHandle handle = buildQueue_.front();
			auto& blas = blas_[handle];
			if (blas.state != BlasState::Queued || !blas.geometry) {
				buildQueue_.pop_front();
				continue;
			}
			const ChunkGeometry& geometry = *blas.geometry;

			// Geometry upload (freed once this submission completes).
			const std::uint64_t vertexBytes = geometry.positions.size() * sizeof(float);
			const std::uint64_t indexOffset = Align(vertexBytes, 256);
			const std::uint64_t uploadBytes = indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
			auto upload = CreateBuffer(uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
			void* mapped = nullptr;
			if (!upload || FAILED(upload->Map(0, nullptr, &mapped)) || !mapped) {
				Fail("BLAS geometry upload", E_OUTOFMEMORY);
				break;
			}
			std::memcpy(mapped, geometry.positions.data(), vertexBytes);
			std::memcpy(static_cast<std::byte*>(mapped) + indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			upload->Unmap(0, nullptr);

			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			geometryDesc.Triangles.VertexBuffer.StartAddress = upload->GetGPUVirtualAddress();
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
			geometryDesc.Triangles.VertexCount = geometry.Vertices();
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexBuffer = upload->GetGPUVirtualAddress() + indexOffset;
			geometryDesc.Triangles.IndexCount = static_cast<UINT>(geometry.indices.size());
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
			               D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &geometryDesc;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			if (info.ResultDataMaxSizeInBytes == 0) {
				// Degenerate input: drop it rather than retry forever.
				buildQueue_.pop_front();
				blas.geometry.reset();
				blas.state = BlasState::Ready;
				stats_.blasQueued -= std::min<std::uint32_t>(stats_.blasQueued, 1);
				++stats_.blasCount;
				continue;
			}
			if (!EnsureScratch(info.ScratchDataSizeInBytes))
				break;
			const std::uint64_t bytes = Align(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			auto resource = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
			if (!resource) {
				Fail("BLAS buffer", E_OUTOFMEMORY);
				break;
			}
			resource->SetName(L"CS RayTracing BLAS");

			if (built > 0) {
				// The builds share one scratch buffer.
				auto barrier = UavBarrier(scratch_.Get());
				a_list->ResourceBarrier(1, &barrier);
			}

			const std::uint32_t slot = postbuildCursor_;
			postbuildCursor_ = (postbuildCursor_ + 1) % kPostbuildSlots;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuild{};
			postbuild.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
			postbuild.DestBuffer = postbuild_->GetGPUVirtualAddress() + sizeof(std::uint64_t) * slot;

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
			buildDesc.Inputs = inputs;
			buildDesc.DestAccelerationStructureData = resource->GetGPUVirtualAddress();
			buildDesc.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress();
			a_list->BuildRaytracingAccelerationStructure(&buildDesc, 1, &postbuild);

			Retire(upload, uploadBytes, &stats_.uploadBytes);
			stats_.uploadBytes += uploadBytes;

			buildQueue_.pop_front();
			blas.geometry.reset();
			blas.resource = std::move(resource);
			blas.bytes = bytes;
			blas.state = BlasState::Built;
			blas.postbuildSlot = slot;
			blas.buildFence = 0;  // set at submit
			slots.push_back(slot);
			stats_.blasBytes += bytes;
			stats_.blasTriangles += blas.triangles;
			stats_.blasQueued -= std::min<std::uint32_t>(stats_.blasQueued, 1);
			++stats_.blasCount;
			++stats_.buildsTotal;
			triangles += blas.triangles;
			++built;
		}

		if (!slots.empty()) {
			D3D12_RESOURCE_BARRIER barriers[2] = { UavBarrier(), Transition(postbuild_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
			a_list->ResourceBarrier(2, barriers);
			for (const std::uint32_t slot : slots)
				a_list->CopyBufferRegion(postbuildReadback_.Get(), sizeof(std::uint64_t) * slot, postbuild_.Get(), sizeof(std::uint64_t) * slot, sizeof(std::uint64_t));
			auto back = Transition(postbuild_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			a_list->ResourceBarrier(1, &back);
		}
		return built;
	}

	bool Backend::ExecuteFrame(const FrameInput& a_input)
	{
		if (!ready_ || failed_)
			return false;
		const HRESULT removed = device_->GetDeviceRemovedReason();
		if (FAILED(removed))
			return Fail("D3D12 device", removed);

		CollectRetired();
		const std::uint64_t completed = fence_->GetCompletedValue();

		// A free command context, without waiting on the CPU.
		std::size_t index = kContexts;
		for (std::size_t offset = 0; offset < kContexts; ++offset) {
			const std::size_t candidate = (cursor_ + offset) % kContexts;
			if (contexts_[candidate].fenceValue == 0 || contexts_[candidate].fenceValue <= completed) {
				index = candidate;
				break;
			}
		}
		for (std::size_t i = 0; i < kContexts; ++i)
			if (contexts_[i].fenceValue != 0 && contexts_[i].fenceValue <= completed)
				CollectTimestamps(i);
		if (index == kContexts) {
			++stats_.skippedFramesBusy;
			return false;
		}
		auto& context = contexts_[index];
		cursor_ = (index + 1) % kContexts;

		static const std::vector<Instance> kNoInstances;
		const std::vector<Instance>& instances = a_input.instances ? *a_input.instances : kNoInstances;

		// GPU-side ordering only: the D3D12 queue waits for everything D3D11 recorded so far
		// (the depth / mask copy), the D3D11 queue later waits for the trace.
		const std::uint64_t readyValue = ++fenceValue_;
		HRESULT hr = context11_->Signal(fence11_.Get(), readyValue);
		if (FAILED(hr))
			return Fail("D3D11 Signal", hr);
		context11_->Flush();
		if (FAILED(hr = queue_->Wait(fence_.Get(), readyValue)))
			return Fail("D3D12 queue Wait", hr);
		if (FAILED(hr = context.allocator->Reset()))
			return Fail("command allocator Reset", hr);
		auto* list = context.list.Get();
		if (FAILED(hr = list->Reset(context.allocator.Get(), nullptr)))
			return Fail("command list Reset", hr);
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index * 2));

		ProcessCompaction(list, completed);
		const std::uint32_t builtNow = BuildQueued(list, a_input.buildTriangleBudget);
		stats_.buildsLastFrame = builtNow;

		// TLAS over every built BLAS that has an instance this frame.
		std::uint32_t instanceCount = 0;
		if (!EnsureTlas(static_cast<std::uint32_t>(instances.size())) || !EnsureInstanceBuffer(context, static_cast<std::uint32_t>(instances.size()))) {
			list->Close();
			return false;
		}
		auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(context.instanceMapped);
		for (const auto& instance : instances) {
			if (instance.blas >= blas_.size())
				continue;
			const auto& blas = blas_[instance.blas];
			if ((blas.state != BlasState::Built && blas.state != BlasState::Ready) || !blas.resource)
				continue;
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1.0f;
			desc.Transform[0][3] = static_cast<float>(instance.x - a_input.cameraX);
			desc.Transform[1][3] = static_cast<float>(instance.y - a_input.cameraY);
			desc.Transform[2][3] = static_cast<float>(instance.z - a_input.cameraZ);
			desc.InstanceMask = 0xFF;
			desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
			desc.AccelerationStructure = blas.resource->GetGPUVirtualAddress();
			descs[instanceCount++] = desc;
		}
		stats_.instancesLastFrame = instanceCount;

		{
			// Builds and compaction copies above must be finished before the TLAS reads them.
			auto barrier = UavBarrier();
			list->ResourceBarrier(1, &barrier);
		}
		D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasDesc{};
		tlasDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
		tlasDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
		tlasDesc.Inputs.NumDescs = instanceCount;
		tlasDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
		tlasDesc.Inputs.InstanceDescs = context.instanceBuffer->GetGPUVirtualAddress();
		tlasDesc.DestAccelerationStructureData = tlas_->GetGPUVirtualAddress();
		tlasDesc.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress();
		list->BuildRaytracingAccelerationStructure(&tlasDesc, 0, nullptr);

		D3D12_RESOURCE_BARRIER pre[3] = {
			UavBarrier(tlas_.Get()),
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		list->ResourceBarrier(3, pre);

		ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
		list->SetDescriptorHeaps(1, heaps);
		list->SetComputeRootSignature(rootSignature_.Get());
		list->SetPipelineState(pipeline_.Get());
		list->SetComputeRoot32BitConstants(0, kRootConstants, &a_input.constants, 0);
		list->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
		list->SetComputeRootDescriptorTable(2, descriptorHeap_->GetGPUDescriptorHandleForHeapStart());
		list->Dispatch((a_input.renderWidth + 7) / 8, (a_input.renderHeight + 7) / 8, 1);

		D3D12_RESOURCE_BARRIER post[2] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
		};
		list->ResourceBarrier(2, post);

		if (timestampHeap_ && timestampReadback_) {
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index * 2 + 1));
			list->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(index * 2), 2,
				timestampReadback_.Get(), sizeof(std::uint64_t) * 2 * index);
			context.timestampsPending = true;
		}
		if (FAILED(hr = list->Close()))
			return Fail("command list Close", hr);
		ID3D12CommandList* lists[] = { list };
		queue_->ExecuteCommandLists(1, lists);
		const std::uint64_t doneValue = ++fenceValue_;
		if (FAILED(hr = queue_->Signal(fence_.Get(), doneValue)))
			return Fail("D3D12 queue Signal", hr);
		context.fenceValue = doneValue;
		lastSubmitted_ = doneValue;
		for (auto& blas : blas_)
			if (blas.state == BlasState::Built && blas.buildFence == 0)
				blas.buildFence = doneValue;
		if (FAILED(hr = context11_->Wait(fence11_.Get(), doneValue)))
			return Fail("D3D11 Wait", hr);
		return true;
	}
}
