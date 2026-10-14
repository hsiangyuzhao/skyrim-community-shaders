#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <bit>
#include <cstring>

#include <dxgi1_4.h>

// Generated at build time from Shaders/TraceShadowsCS.hlsl by the Windows SDK's dxc (CMakeLists.txt).
#include "RayTracing/TraceShadowsCS.dxil.h"
// (45h) The same shader with RT_ALPHA: non-opaque (alpha-tested) triangles are tested against their cut-out.
#include "RayTracing/TraceShadowsAlphaCS.dxil.h"
// (45i) With RT_CHARACTERS (and RT_ALPHA): skinned characters traced by a second, character-only ray.
#include "RayTracing/TraceShadowsCharAlphaCS.dxil.h"
#include "RayTracing/TraceShadowsCharCS.dxil.h"

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

		std::int64_t QpcNow()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcToMs(std::int64_t a_ticks)
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return static_cast<double>(a_ticks) * toMs;
		}
	}

	// Process exit: no GPU wait (the GPU work of a closing game does not matter any more).
	Backend::~Backend() { Shutdown(false); }

	bool Backend::Fail(const std::string& a_operation, HRESULT a_result)
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
			// (45l) The trace's own list (same allocator, recorded after the first one is closed).
			ComPtr<ID3D12GraphicsCommandList> traceList;
			if (FAILED(hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.Get(), nullptr, IID_PPV_ARGS(&traceList))))
				return fail(std::format("CreateCommandList failed ({})", HrString(hr)));
			if (FAILED(traceList.As(&context.traceList)))
				return fail("ID3D12GraphicsCommandList4 not available");
			context.traceList->Close();
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
			heapDesc.Count = static_cast<UINT>(kContexts * 4);  // (45l) per context: prep begin / end, trace begin / end
			UINT64 frequency = 0;
			timestampReadback_ = CreateBuffer(sizeof(std::uint64_t) * kContexts * 4, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
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

		// (45h) Alpha-tested casters (only the RT_ALPHA variant uses them): t2 per-shape records,
		// t3 texcoords, t4 the 256x256 copy layers, t0 space1 the bindless game textures, s0.
		D3D12_DESCRIPTOR_RANGE alphaRanges[2]{};
		alphaRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		alphaRanges[0].NumDescriptors = 1;
		alphaRanges[0].BaseShaderRegister = 4;
		alphaRanges[0].OffsetInDescriptorsFromTableStart = 0;
		alphaRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		alphaRanges[1].NumDescriptors = kAlphaTextureSlots;
		alphaRanges[1].BaseShaderRegister = 0;
		alphaRanges[1].RegisterSpace = 1;
		alphaRanges[1].OffsetInDescriptorsFromTableStart = 1;

		D3D12_ROOT_PARAMETER params[6]{};
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
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[3].Descriptor.ShaderRegister = 2;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[4].Descriptor.ShaderRegister = 3;
		params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[5].DescriptorTable.NumDescriptorRanges = 2;
		params[5].DescriptorTable.pDescriptorRanges = alphaRanges;
		params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 6;
		rootDesc.pParameters = params;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;
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
		heapDesc.NumDescriptors = kAlphaDescriptorBase + kAlphaTextureSlots;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)))) {
			o_reason = std::format("CreateDescriptorHeap failed ({})", HrString(hr));
			return false;
		}
		descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

		// (45h) Null views in every alpha slot (the copy layers and the bindless table are filled on use).
		{
			auto cpu = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
			cpu.ptr += static_cast<SIZE_T>(descriptorSize_) * (kAlphaDescriptorBase - 1);
			D3D12_SHADER_RESOURCE_VIEW_DESC nullArray{};
			nullArray.Format = DXGI_FORMAT_R8_UNORM;
			nullArray.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
			nullArray.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			nullArray.Texture2DArray.MipLevels = 1;
			nullArray.Texture2DArray.ArraySize = 1;
			device_->CreateShaderResourceView(nullptr, &nullArray, cpu);
			D3D12_SHADER_RESOURCE_VIEW_DESC nullTexture{};
			nullTexture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			nullTexture.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			nullTexture.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			nullTexture.Texture2D.MipLevels = 1;
			for (std::uint32_t slot = 0; slot < kAlphaTextureSlots; ++slot) {
				cpu.ptr += descriptorSize_;
				device_->CreateShaderResourceView(nullptr, &nullTexture, cpu);
			}
		}

		// (45h) The alpha variant: not fatal when the driver refuses it (the switch then stays unavailable).
		psoDesc.CS.pShaderBytecode = g_TraceShadowsAlphaCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TraceShadowsAlphaCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&alphaPipeline_)))) {
			alphaPipeline_.Reset();
			logger::warn("[RayTracing] The alpha-tested ray query shader was rejected by the driver ({}); Alpha-tested casters unavailable", HrString(hr));
		}

		// (45i) The character variants and the skinning shader: not fatal either (Characters then stays unavailable).
		psoDesc.CS.pShaderBytecode = g_TraceShadowsCharCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TraceShadowsCharCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&charPipeline_)))) {
			charPipeline_.Reset();
			logger::warn("[RayTracing] The character ray query shader was rejected by the driver ({}); Characters unavailable", HrString(hr));
		}
		psoDesc.CS.pShaderBytecode = g_TraceShadowsCharAlphaCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TraceShadowsCharAlphaCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&charAlphaPipeline_)))) {
			charAlphaPipeline_.Reset();
			logger::warn("[RayTracing] The character + alpha ray query shader was rejected by the driver ({}); alpha-tested character parts unavailable",
				HrString(hr));
		}
		if (charPipeline_ && !CreateSkinPipelines())
			charPipeline_.Reset();
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
			if (context.alphaInfoBuffer && context.alphaInfoMapped)
				context.alphaInfoBuffer->Unmap(0, nullptr);
			if (context.boneBuffer && context.boneMapped)
				context.boneBuffer->Unmap(0, nullptr);
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
		alphaPipeline_.Reset();
		pipeline_.Reset();
		// (45h)
		alphaTextures_.clear();
		freeAlphaTextures_.clear();
		alphaTextureIndex_.clear();
		alphaCopyQueue_.clear();
		freeAlphaSlots_.clear();
		freeAlphaLayers_.clear();
		nextAlphaSlot_ = nextAlphaLayer_ = 0;
		retiredAlphaSlots_.clear();
		retiredAlphaLayers_.clear();
		retiredAlphaUv_.clear();
		alphaInfos_.clear();
		freeAlphaInfos_.clear();
		alphaInfoLive_ = 0;
		alphaInfoVersion_ = 1;
		alphaUv_.Reset();
		alphaUvFree_.clear();
		alphaUvUsed_ = 0;
		for (auto& uav : alphaLayerUavs_)
			uav.Reset();
		alphaLayers11_.Reset();
		alphaLayers12_.Reset();
		alphaCopyCB_.Reset();
		alphaCopySampler_.Reset();
		alphaLayerFormat_ = DXGI_FORMAT_UNKNOWN;
		alphaLayerCount_ = 0;
		alphaLayersFailed_ = false;
		alphaSharedOpened_ = 0;
		alphaFault_.clear();
		alphaStats_ = {};
		// (45i)
		skins_.clear();
		freeSkins_.clear();
		skinUploadQueue_.clear();
		skinScratch_.Reset();
		skinScratchBytes_ = 0;
		skinLive_ = 0;
		skinFault_.clear();
		skinStats_ = {};
		skinReady_.clear();
		charAlphaPipeline_.Reset();
		charPipeline_.Reset();
		skinPipeline_.Reset();
		skinRootSignature_.Reset();
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
		check_ = {};
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

	bool Backend::CheckSharedFormat(DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store, const char* a_name, bool a_fatal)
	{
		// A D3D11 texture shared with D3D12 needs a format the driver marks as shareable: the
		// two-channel formats (R32G32, R8G8, ...) are not on the guaranteed list and the driver
		// refuses them with E_INVALIDARG. Every bit this texture relies on is checked here so a
		// refusal names the exact reason instead of failing inside CreateTexture2D.
		UINT support11 = 0;
		if (FAILED(device11_->CheckFormatSupport(a_format, &support11)))
			support11 = 0;
		D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support11b{ a_format, 0 };
		if (FAILED(device11_->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support11b, sizeof(support11b))))
			support11b.OutFormatSupport2 = 0;
		D3D12_FEATURE_DATA_FORMAT_SUPPORT support12{ a_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };
		if (FAILED(device_->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support12, sizeof(support12))))
			support12 = { a_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE };

		std::string missing;
		auto need = [&](bool a_ok, const char* a_what) {
			if (!a_ok)
				missing += missing.empty() ? std::string(a_what) : std::string(", ") + a_what;
		};
		need((support11b.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE) != 0, "D3D11 shareable");
		need((support11 & D3D11_FORMAT_SUPPORT_TEXTURE2D) != 0, "D3D11 texture2D");
		need((support11 & D3D11_FORMAT_SUPPORT_SHADER_LOAD) != 0, "D3D11 shader load");
		need((support11 & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) != 0, "D3D11 typed UAV");
		need(!a_d3d11Store || (support11b.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0, "D3D11 typed UAV store");
		need((support12.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) != 0, "D3D12 texture2D");
		need((support12.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0, "D3D12 shader load");
		need(!a_d3d12Store || (support12.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) != 0, "D3D12 typed UAV");
		need(!a_d3d12Store || (support12.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) != 0, "D3D12 typed UAV store");

		const std::string bits = std::format("format {}, D3D11 support 0x{:08X} / 0x{:08X}, D3D12 support 0x{:08X} / 0x{:08X}",
			static_cast<int>(a_format), support11, support11b.OutFormatSupport2, static_cast<std::uint32_t>(support12.Support1),
			static_cast<std::uint32_t>(support12.Support2));
		if (!missing.empty()) {
			const std::string error = std::format("{}: the GPU / driver lacks {} ({})", a_name, missing, bits);
			if (!a_fatal) {
				logger::info("[RayTracing] {}", error);
				return false;
			}
			lastError_ = error;
			logger::error("[RayTracing] {}", lastError_);
			failed_ = true;
			return false;
		}
		logger::info("[RayTracing] {}: {}", a_name, bits);
		return true;
	}

	bool Backend::CreateSharedTexture(std::uint32_t a_width, std::uint32_t a_height, DXGI_FORMAT a_format, bool a_d3d11Store, bool a_d3d12Store,
		SharedTexture& o_texture, const char* a_name)
	{
		if (!CheckSharedFormat(a_format, a_d3d11Store, a_d3d12Store, a_name))
			return false;
		// Same creation path as Neural Rendering's D3D12Interop (proven in game): D3D11 creates the
		// texture with an NT handle, D3D12 opens it (state COMMON, no simultaneous access).
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
		const std::string what = std::format("{} ({}x{}, format {})", a_name, a_width, a_height, static_cast<int>(a_format));
		HRESULT hr = device11_->CreateTexture2D(&desc, nullptr, &texture.resource11);
		if (FAILED(hr))
			return Fail(std::format("CreateTexture2D (shared) {}", what), hr);
		Util::SetResourceName(texture.resource11.Get(), a_name);
		if (FAILED(hr = device11_->CreateShaderResourceView(texture.resource11.Get(), nullptr, &texture.srv11)) ||
			FAILED(hr = device11_->CreateUnorderedAccessView(texture.resource11.Get(), nullptr, &texture.uav11)))
			return Fail(std::format("shared texture views {}", what), hr);
		ComPtr<IDXGIResource1> dxgiResource;
		if (FAILED(hr = texture.resource11.As(&dxgiResource)))
			return Fail("IDXGIResource1", hr);
		HANDLE handle = nullptr;
		if (FAILED(hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
			return Fail(std::format("CreateSharedHandle {}", what), hr);
		hr = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&texture.resource12));
		CloseHandle(handle);
		if (FAILED(hr))
			return Fail(std::format("OpenSharedHandle {}", what), hr);
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
		// Only formats on D3D11's guaranteed-shareable list (R32_FLOAT and R8G8B8A8_UNORM are also
		// what Neural Rendering shares in game). DepthMask: depth, sign bit = engine mask already 0
		// (written by D3D11, read by D3D12). Visibility: r visibility, g coverage (written by D3D12).
		if (!CreateSharedTexture(a_width, a_height, kDepthMaskFormat, true, false, depthMask_, "RayTracing DepthMask") ||
			!CreateSharedTexture(a_width, a_height, kVisibilityFormat, false, true, visibility_, "RayTracing Visibility")) {
			o_reason = lastError_;
			return false;
		}
		textureWidth_ = a_width;
		textureHeight_ = a_height;
		stats_.sharedTextureBytes = static_cast<std::uint64_t>(a_width) * a_height * (4 + 4);
		WriteDescriptors();
		logger::info("[RayTracing] Shared textures {}x{}", a_width, a_height);
		return true;
	}

	void Backend::WriteDescriptors()
	{
		auto cpu = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = kDepthMaskFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(depthMask_.resource12.Get(), &srvDesc, cpu);
		cpu.ptr += descriptorSize_;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = kVisibilityFormat;
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
		CollectAlphaRetired(completed);
	}

	void Backend::CollectTimestamps(std::size_t a_index)
	{
		auto& context = contexts_[a_index];
		if (!context.timestampsPending || !timestampReadback_ || timestampFrequency_ == 0)
			return;
		context.timestampsPending = false;
		// (45l) Four per context: the prep submission (builds, skinning, TLAS) and the trace. Their sum is the
		// GPU work of our queue; the gap between them (the wait for the D3D11 depth copy) is not counted.
		const SIZE_T offset = sizeof(std::uint64_t) * 4 * a_index;
		D3D12_RANGE range{ offset, offset + sizeof(std::uint64_t) * 4 };
		void* mapped = nullptr;
		if (FAILED(timestampReadback_->Map(0, &range, &mapped)) || !mapped)
			return;
		const auto* ticks = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(mapped) + offset);
		const std::uint64_t prepBegin = ticks[0], prepEnd = ticks[1], begin = ticks[2], end = ticks[3];
		D3D12_RANGE written{ 0, 0 };
		timestampReadback_->Unmap(0, &written);
		if (end <= begin || prepEnd < prepBegin)
			return;
		const double toMs = 1000.0 / static_cast<double>(timestampFrequency_);
		stats_.lastPrepGpuMs = static_cast<float>(static_cast<double>(prepEnd - prepBegin) * toMs);
		stats_.lastTraceGpuMs = static_cast<float>(static_cast<double>(end - begin) * toMs);
		stats_.lastGpuMs = stats_.lastPrepGpuMs + stats_.lastTraceGpuMs;
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
		// (45h) An alpha BLAS keeps its texcoords (3 corners per triangle) in the shared buffer.
		std::uint32_t uvOffset = UINT32_MAX, uvCount = 0;
		if (!a_geometry->Opaque()) {
			uvCount = a_geometry->Triangles() * 3;
			if (a_geometry->alphaUV.size() != static_cast<std::size_t>(uvCount) * 2 || !AlphaAvailable() || !AllocateAlphaUv(uvCount, uvOffset))
				return kInvalidBlas;
		}
		const BlasHandle handle = AllocateHandle();
		auto& blas = blas_[handle];
		blas = {};
		blas.state = BlasState::Queued;
		blas.triangles = a_geometry->Triangles();
		blas.uvOffset = uvOffset;
		blas.uvCount = uvCount;
		// (45f) Local box of the vertices the triangles use (for the TLAS self-check).
		for (int a = 0; a < 3; ++a) {
			blas.boundsMin[a] = FLT_MAX;
			blas.boundsMax[a] = -FLT_MAX;
		}
		const auto& positions = a_geometry->positions;
		for (const auto index : a_geometry->indices) {
			const std::size_t v = static_cast<std::size_t>(index) * 3;
			if (v + 2 >= positions.size())
				continue;
			for (int a = 0; a < 3; ++a) {
				blas.boundsMin[a] = std::min(blas.boundsMin[a], positions[v + a]);
				blas.boundsMax[a] = std::max(blas.boundsMax[a], positions[v + a]);
			}
		}
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
		if (blas.uvOffset != UINT32_MAX)
			FreeAlphaUv(blas.uvOffset, blas.uvCount);
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

	void Backend::ProcessCompaction(ID3D12GraphicsCommandList4* a_list, std::uint64_t a_completed, std::uint32_t a_max)
	{
		stats_.compactionsLastFrame = 0;
		std::uint32_t created = 0;
		for (auto& blas : blas_) {
			if (blas.state != BlasState::Built || blas.buildFence == 0 || blas.buildFence > a_completed)
				continue;
			// (45j) A bounded number of new buffers per frame; the rest stay usable (Built) until a later frame.
			if (created >= a_max)
				break;
			const std::uint64_t compactedSize = blas.postbuildSlot < kPostbuildSlots ? postbuildMapped_[blas.postbuildSlot] : 0;
			blas.postbuildSlot = UINT32_MAX;
			const std::uint64_t bytes = Align(compactedSize, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			// Not worth a second buffer when it saves under 10 %.
			if (compactedSize == 0 || bytes * 10 >= blas.bytes * 9) {
				blas.state = BlasState::Ready;
				continue;
			}
			++created;
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

	std::uint32_t Backend::BuildQueued(ID3D12GraphicsCommandList4* a_list, std::uint32_t a_budget, std::size_t a_maxBuilds)
	{
		std::uint32_t built = 0;
		std::vector<std::uint32_t> slots;

		// (45e) This frame's batch: up to the triangle budget and kMaxBuildsPerFrame builds (many
		// small object meshes arrive at once), with ONE upload buffer for all of their geometry
		// instead of a committed resource per build.
		struct Pending
		{
			BlasHandle handle;
			std::uint64_t vertexOffset;
			std::uint64_t indexOffset;
			std::uint64_t uvOffset;  // (45h) alpha texcoords in the upload buffer (alpha BLASes only)
		};
		std::vector<Pending> batch;
		std::uint64_t uploadBytes = 0;
		{
			std::uint32_t batchTriangles = 0;
			while (!buildQueue_.empty() && batch.size() < a_maxBuilds && (batch.empty() || batchTriangles < a_budget)) {
				const BlasHandle handle = buildQueue_.front();
				buildQueue_.pop_front();
				auto& blas = blas_[handle];
				if (blas.state != BlasState::Queued || !blas.geometry)
					continue;
				const ChunkGeometry& geometry = *blas.geometry;
				Pending entry{ handle, Align(uploadBytes, 256), 0, 0 };
				entry.indexOffset = Align(entry.vertexOffset + geometry.positions.size() * sizeof(float), 256);
				uploadBytes = entry.indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
				if (!geometry.Opaque()) {
					entry.uvOffset = Align(uploadBytes, 256);
					uploadBytes = entry.uvOffset + geometry.alphaUV.size() * sizeof(std::uint32_t);
				}
				batchTriangles += geometry.Triangles();
				batch.push_back(entry);
			}
		}
		if (batch.empty())
			return 0;
		auto upload = CreateBuffer(uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		std::byte* mapped = nullptr;
		if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped) {
			for (auto it = batch.rbegin(); it != batch.rend(); ++it)
				buildQueue_.push_front(it->handle);
			Fail("BLAS geometry upload", E_OUTOFMEMORY);
			return 0;
		}
		for (const auto& entry : batch) {
			const ChunkGeometry& geometry = *blas_[entry.handle].geometry;
			std::memcpy(mapped + entry.vertexOffset, geometry.positions.data(), geometry.positions.size() * sizeof(float));
			std::memcpy(mapped + entry.indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			if (!geometry.Opaque())
				std::memcpy(mapped + entry.uvOffset, geometry.alphaUV.data(), geometry.alphaUV.size() * sizeof(std::uint32_t));
		}
		upload->Unmap(0, nullptr);
		upload->SetName(L"CS RayTracing BLAS upload");
		// Freed once this submission completes.
		Retire(upload, uploadBytes, &stats_.uploadBytes);
		stats_.uploadBytes += uploadBytes;

		// (45h) Texcoords of alpha BLASes into the shared buffer (a range per BLAS, fixed for its life).
		{
			bool any = false;
			for (const auto& entry : batch)
				any = any || (!blas_[entry.handle].geometry->Opaque() && blas_[entry.handle].uvOffset != UINT32_MAX);
			if (any && alphaUv_) {
				auto toCopy = Transition(alphaUv_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
				a_list->ResourceBarrier(1, &toCopy);
				for (const auto& entry : batch) {
					const auto& blas = blas_[entry.handle];
					if (blas.geometry->Opaque() || blas.uvOffset == UINT32_MAX)
						continue;
					a_list->CopyBufferRegion(alphaUv_.Get(), static_cast<std::uint64_t>(blas.uvOffset) * 8, upload.Get(), entry.uvOffset,
						static_cast<std::uint64_t>(blas.uvCount) * 8);
				}
				auto back = Transition(alphaUv_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				a_list->ResourceBarrier(1, &back);
			}
		}

		for (std::size_t b = 0; b < batch.size(); ++b) {
			const BlasHandle handle = batch[b].handle;
			auto& blas = blas_[handle];
			const ChunkGeometry& geometry = *blas.geometry;
			auto requeueRest = [&]() {
				for (std::size_t r = batch.size(); r-- > b;)
					buildQueue_.push_front(batch[r].handle);
			};

			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			// (45h) Alpha-tested meshes are non-opaque: the trace tests each candidate against its cut-out.
			geometryDesc.Flags = geometry.Opaque() ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			geometryDesc.Triangles.VertexBuffer.StartAddress = upload->GetGPUVirtualAddress() + batch[b].vertexOffset;
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
			geometryDesc.Triangles.VertexCount = geometry.Vertices();
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexBuffer = upload->GetGPUVirtualAddress() + batch[b].indexOffset;
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
				blas.geometry.reset();
				blas.state = BlasState::Ready;
				stats_.blasQueued -= std::min<std::uint32_t>(stats_.blasQueued, 1);
				++stats_.blasCount;
				continue;
			}
			if (!EnsureScratch(info.ScratchDataSizeInBytes)) {
				requeueRest();
				break;
			}
			const std::uint64_t bytes = Align(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			auto resource = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
			if (!resource) {
				requeueRest();
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

	void Backend::CheckInstance(const D3D12_RAYTRACING_INSTANCE_DESC& a_desc, const Instance& a_instance, const Blas& a_blas, const FrameInput& a_input)
	{
		// (45f) The instance exactly as the GPU will see it (camera-relative transform as written,
		// local box of the BLAS vertices), back in world units, against the box of the engine's
		// own live bound spheres of the shapes it was made from. Every vertex lies inside its
		// shape's bound sphere, so a correct instance can never stick out of that box.
		++check_.checked;
		const double camera[3] = { a_input.cameraX, a_input.cameraY, a_input.cameraZ };
		double worldMin[3] = { DBL_MAX, DBL_MAX, DBL_MAX }, worldMax[3] = { -DBL_MAX, -DBL_MAX, -DBL_MAX };
		const bool empty = a_blas.boundsMin[0] > a_blas.boundsMax[0];
		if (!empty) {
			for (int corner = 0; corner < 8; ++corner) {
				const float local[3] = { (corner & 1) ? a_blas.boundsMax[0] : a_blas.boundsMin[0], (corner & 2) ? a_blas.boundsMax[1] : a_blas.boundsMin[1],
					(corner & 4) ? a_blas.boundsMax[2] : a_blas.boundsMin[2] };
				for (int r = 0; r < 3; ++r) {
					const double p = static_cast<double>(a_desc.Transform[r][0]) * local[0] + static_cast<double>(a_desc.Transform[r][1]) * local[1] +
					                 static_cast<double>(a_desc.Transform[r][2]) * local[2] + static_cast<double>(a_desc.Transform[r][3]) + camera[r];
					worldMin[r] = std::min(worldMin[r], p);
					worldMax[r] = std::max(worldMax[r], p);
				}
			}
		}
		double worst = 0.0;
		for (int a = 0; a < 3; ++a) {
			if (empty)
				break;
			worst = std::max(worst, a_instance.expectMin[a] - worldMin[a]);
			worst = std::max(worst, worldMax[a] - a_instance.expectMax[a]);
		}
		const double extent = std::max({ a_instance.expectMax[0] - a_instance.expectMin[0], a_instance.expectMax[1] - a_instance.expectMin[1],
			a_instance.expectMax[2] - a_instance.expectMin[2] });
		const double margin = 64.0 + extent * 0.05;
		if (!empty && worst <= margin)
			return;
		const auto category = std::min<std::uint32_t>(a_instance.category, kInstanceCategories - 1);
		++check_.mismatched[category];
		++check_.mismatchedTotal;
		// (45g) Logged again every 30 s per reference + BLAS (an animated or camera-dependent
		// transform then shows as a drifting box), 200 lines in total.
		if (check_.logged >= 200)
			return;
		{
			LARGE_INTEGER now, frequency;
			QueryPerformanceCounter(&now);
			QueryPerformanceFrequency(&frequency);
			const std::uint64_t key = (static_cast<std::uint64_t>(a_instance.refID) << 32) ^ a_instance.blas;
			const auto it = check_.lastLogged.find(key);
			if (it != check_.lastLogged.end() && now.QuadPart - it->second < frequency.QuadPart * 30)
				return;
			check_.lastLogged[key] = now.QuadPart;
		}
		++check_.logged;
		if (empty) {
			logger::warn("[RayTracing] TLAS self-check: {} instance (BLAS {}, {} triangles) has no vertices in use", kInstanceCategoryNames[category],
				a_instance.blas, a_blas.triangles);
			return;
		}
		logger::warn("[RayTracing] TLAS self-check: {} instance (BLAS {}, {} triangles{}) spans ({:.0f}, {:.0f}, {:.0f}) .. ({:.0f}, {:.0f}, {:.0f}) "
					 "in the world (size {:.0f} x {:.0f} x {:.0f}, origin ({:.1f}, {:.1f}, {:.1f})), the engine's bounds of its shapes span ({:.0f}, {:.0f}, {:.0f}) .. "
					 "({:.0f}, {:.0f}, {:.0f}): sticks out by {:.0f} units (camera ({:.0f}, {:.0f}, {:.0f}), mask 0x{:02X}, flags 0x{:X})",
			kInstanceCategoryNames[category], a_instance.blas, a_blas.triangles,
			a_instance.refID ? std::format(", reference {:08X} shape '{}'", a_instance.refID, a_instance.label ? a_instance.label : "") : std::string(),
			worldMin[0], worldMin[1], worldMin[2], worldMax[0], worldMax[1], worldMax[2], worldMax[0] - worldMin[0], worldMax[1] - worldMin[1],
			worldMax[2] - worldMin[2], a_instance.x, a_instance.y, a_instance.z, a_instance.expectMin[0], a_instance.expectMin[1], a_instance.expectMin[2],
			a_instance.expectMax[0], a_instance.expectMax[1], a_instance.expectMax[2], worst, camera[0], camera[1], camera[2],
			static_cast<std::uint32_t>(a_desc.InstanceMask), static_cast<std::uint32_t>(a_desc.Flags));
	}

	bool Backend::ExecuteFrame(const FrameInput& a_input)
	{
		if (!ready_ || failed_)
			return false;
		const HRESULT removed = device_->GetDeviceRemovedReason();
		if (FAILED(removed))
			return Fail("D3D12 device", removed);

		const std::int64_t cpuStart = QpcNow();
		// (45k) Every call that can wait for the GPU is timed on its own (stats_.split, F7 trace).
		ExecuteSplit split{};
		std::int64_t mark = cpuStart;
		auto lap = [&](float& a_field) {
			const std::int64_t now = QpcNow();
			a_field += static_cast<float>(QpcToMs(now - mark));
			mark = now;
		};
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
		lap(split.collect);

		static const std::vector<Instance> kNoInstances;
		const std::vector<Instance>& instances = a_input.instances ? *a_input.instances : kNoInstances;

		// (45l) Recording first; the D3D11 ready Signal and the D3D12 queue Wait come after it. With
		// splitSubmit the builds, skinning and the TLAS go out at once (they read nothing D3D11 writes)
		// and only the trace list waits for D3D11's depth copy, so the D3D11 queue later waits for the
		// trace alone instead of everything (GPU-side ordering only; the CPU never waits).
		HRESULT hr;
		if (FAILED(hr = context.allocator->Reset()))
			return Fail("command allocator Reset", hr);
		auto* list = context.list.Get();
		if (FAILED(hr = list->Reset(context.allocator.Get(), nullptr)))
			return Fail("command list Reset", hr);
		lap(split.reset);
		const UINT queryBase = static_cast<UINT>(index * 4);
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);

		const std::int64_t buildStart = QpcNow();
		const std::uint32_t maxBuilds = a_input.maxBuilds ? a_input.maxBuilds : static_cast<std::uint32_t>(kMaxBuildsPerFrame);
		ProcessCompaction(list, completed, a_input.maxBuilds ? a_input.maxBuilds : kMaxCompactionsPerFrame);
		const std::uint32_t builtNow = BuildQueued(list, a_input.buildTriangleBudget, maxBuilds);
		stats_.buildsLastFrame = builtNow;
		const std::int64_t buildEnd = QpcNow();

		// (45i) Skinned characters: uploads, skinning, BLAS builds / refits (none without draws).
		static const std::vector<SkinDraw> kNoSkinDraws;
		const std::vector<SkinDraw>& skinDraws = a_input.skinDraws && SkinAvailable() ? *a_input.skinDraws : kNoSkinDraws;
		skinReady_.assign(skinDraws.size(), 0);
		skinRecorded_ = false;
		if (!skinDraws.empty() || !skinUploadQueue_.empty())
			RecordSkinning(list, context, a_input, skinReady_);
		const std::int64_t skinEnd = QpcNow();
		stats_.cpuBuildMs = static_cast<float>(QpcToMs(buildEnd - buildStart));
		stats_.cpuSkinMs = static_cast<float>(QpcToMs(skinEnd - buildEnd));
		if (failed_) {
			list->Close();
			return false;
		}
		mark = QpcNow();

		// TLAS over every built BLAS that has an instance this frame.
		std::uint32_t instanceCount = 0;
		const auto maxInstances = static_cast<std::uint32_t>(instances.size() + skinDraws.size());
		if (!EnsureTlas(maxInstances) || !EnsureInstanceBuffer(context, maxInstances)) {
			list->Close();
			return false;
		}
		auto* descs = static_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(context.instanceMapped);
		for (auto& count : check_.written)
			count = 0;
		for (auto& count : check_.notBuilt)
			count = 0;
		if (a_input.validate) {
			check_.checked = 0;
			for (auto& count : check_.mismatched)
				count = 0;
		}
		std::uint32_t alphaInstances = 0;
		for (const auto& instance : instances) {
			const auto category = std::min<std::uint32_t>(instance.category, kInstanceCategories - 1);
			if (instance.blas >= blas_.size()) {
				++check_.notBuilt[category];
				continue;
			}
			const auto& blas = blas_[instance.blas];
			if ((blas.state != BlasState::Built && blas.state != BlasState::Ready) || !blas.resource) {
				++check_.notBuilt[category];
				continue;
			}
			// (45h) Alpha-tested: only with the alpha pipeline and its buffers, and a live record.
			const bool alpha = instance.alphaInfo != kInvalidAlphaInfo;
			if (alpha && (!alphaPipeline_ || !alphaUv_ || instance.alphaInfo >= alphaInfos_.size() || instance.alphaInfo + 1 >= (1u << 24))) {
				++check_.notBuilt[category];
				continue;
			}
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			for (int r = 0; r < 3; ++r)
				for (int c = 0; c < 3; ++c)
					desc.Transform[r][c] = instance.m[r][c];
			desc.Transform[0][3] = static_cast<float>(instance.x - a_input.cameraX);
			desc.Transform[1][3] = static_cast<float>(instance.y - a_input.cameraY);
			desc.Transform[2][3] = static_cast<float>(instance.z - a_input.cameraZ);
			desc.InstanceMask = kInstanceMask;
			desc.Flags = kInstanceFlags;
			if (alpha) {
				// The shader finds the record through the instance ID (0 = opaque); no force-opaque flag.
				desc.InstanceID = instance.alphaInfo + 1;
				desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
				++alphaInstances;
			}
			desc.AccelerationStructure = blas.resource->GetGPUVirtualAddress();
			if (a_input.validate && instance.hasExpected)
				CheckInstance(desc, instance, blas, a_input);
			++check_.written[category];
			descs[instanceCount++] = desc;
		}
		// (45i) Skinned characters: identity rotation (the skinning put the vertices in world axes),
		// translated to their origin, instance mask kCharacterMask (the player) or kNpcMask (45l, the
		// other actors): the scene ray skips them.
		std::uint32_t characterInstances = 0;
		// A frame with alpha-tested scene instances needs the character + alpha variant.
		const bool charactersTraceable = charPipeline_ && (alphaInstances == 0 || charAlphaPipeline_);
		for (std::size_t d = 0; d < skinDraws.size(); ++d) {
			if (!skinReady_[d] || !charactersTraceable) {
				++check_.notBuilt[kCategoryCharacter];
				continue;
			}
			const auto& draw = skinDraws[d];
			const auto& skin = skins_[draw.shape];
			const bool alpha = skin.uvOffset != UINT32_MAX;
			if (alpha && (!charAlphaPipeline_ || !alphaPipeline_ || !alphaUv_ || draw.alphaInfo >= alphaInfos_.size() || draw.alphaInfo + 1 >= (1u << 24))) {
				++check_.notBuilt[kCategoryCharacter];
				continue;
			}
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1.0f;
			desc.Transform[0][3] = static_cast<float>(draw.x - a_input.cameraX);
			desc.Transform[1][3] = static_cast<float>(draw.y - a_input.cameraY);
			desc.Transform[2][3] = static_cast<float>(draw.z - a_input.cameraZ);
			desc.InstanceMask = draw.npc ? kNpcMask : kCharacterMask;
			desc.Flags = kInstanceFlags;
			if (alpha) {
				desc.InstanceID = draw.alphaInfo + 1;
				desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
				++alphaInstances;
			}
			desc.AccelerationStructure = skin.blas->GetGPUVirtualAddress();
			++check_.written[kCategoryCharacter];
			descs[instanceCount++] = desc;
			++characterInstances;
		}
		skinStats_.drawsLastFrame = characterInstances;

		stats_.instancesLastFrame = instanceCount;
		lap(split.tlas);
		// (45h) The alpha pipeline only on frames that hold alpha-tested instances (and their records
		// are in this context's copy); every other frame runs exactly the opaque pipeline.
		bool alphaFrame = alphaInstances > 0;
		if (alphaFrame && !EnsureAlphaInfoBuffer(context)) {
			// (45i) Character uploads / builds recorded in this list must run (later refits rely on
			// them): a list that cannot be submitted takes the whole feature down instead.
			if (skinRecorded_)
				return Fail("alpha record buffer (with character work recorded)", E_OUTOFMEMORY);
			// Nothing written above can be traced without its record: drop the frame's trace.
			list->Close();
			return false;
		}
		alphaStats_.instancesLastFrame = alphaInstances;
		lap(split.alpha);

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
		{
			auto barrier = UavBarrier(tlas_.Get());
			list->ResourceBarrier(1, &barrier);
		}
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);

		// The trace: in its own list behind the wait (split), or appended to the same one (45k).
		ID3D12GraphicsCommandList4* traceList = list;
		if (a_input.splitSubmit) {
			if (FAILED(hr = list->Close()))
				return Fail("command list Close", hr);
			ID3D12CommandList* prepLists[] = { list };
			queue_->ExecuteCommandLists(1, prepLists);
			traceList = context.traceList.Get();
			if (FAILED(hr = traceList->Reset(context.allocator.Get(), nullptr)))
				return Fail("trace command list Reset", hr);
		}
		if (timestampHeap_)
			traceList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 2);

		D3D12_RESOURCE_BARRIER pre[2] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		traceList->ResourceBarrier(2, pre);

		ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
		traceList->SetDescriptorHeaps(1, heaps);
		traceList->SetComputeRootSignature(rootSignature_.Get());
		// (45i) The character variants only on frames with character instances: every other frame
		// runs exactly the 45h pipelines.
		const bool characterFrame = characterInstances > 0;
		if (characterFrame)
			traceList->SetPipelineState(alphaFrame ? charAlphaPipeline_.Get() : charPipeline_.Get());
		else
			traceList->SetPipelineState(alphaFrame ? alphaPipeline_.Get() : pipeline_.Get());
		traceList->SetComputeRoot32BitConstants(0, kRootConstants, &a_input.constants, 0);
		traceList->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
		traceList->SetComputeRootDescriptorTable(2, descriptorHeap_->GetGPUDescriptorHandleForHeapStart());
		if (alphaFrame) {
			traceList->SetComputeRootShaderResourceView(3, context.alphaInfoBuffer->GetGPUVirtualAddress());
			traceList->SetComputeRootShaderResourceView(4, alphaUv_->GetGPUVirtualAddress());
			auto table = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
			table.ptr += static_cast<UINT64>(descriptorSize_) * (kAlphaDescriptorBase - 1);
			traceList->SetComputeRootDescriptorTable(5, table);
		}
		traceList->Dispatch((a_input.renderWidth + 7) / 8, (a_input.renderHeight + 7) / 8, 1);

		D3D12_RESOURCE_BARRIER post[2] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
		};
		traceList->ResourceBarrier(2, post);

		if (timestampHeap_ && timestampReadback_) {
			traceList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 3);
			traceList->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase, 4, timestampReadback_.Get(),
				sizeof(std::uint64_t) * 4 * index);
			context.timestampsPending = true;
		}
		if (FAILED(hr = traceList->Close()))
			return Fail("command list Close", hr);
		lap(split.record);

		// GPU-side ordering only: the trace waits for everything D3D11 recorded so far (the depth /
		// mask copy, the alpha copies); the D3D11 queue later waits for the trace.
		const std::uint64_t readyValue = ++fenceValue_;
		if (FAILED(hr = context11_->Signal(fence11_.Get(), readyValue)))
			return Fail("D3D11 Signal", hr);
		lap(split.signal11);
		// (45k) The Signal submits the D3D11 work recorded so far by itself; the extra Flush of 45j
		// is a switch (Debug: Flush D3D11 Before the Trace) so a trace can tell whether it blocks.
		if (explicitFlush_)
			context11_->Flush();
		else
			split.flushSkipped = true;
		lap(split.flush11);
		if (FAILED(hr = queue_->Wait(fence_.Get(), readyValue)))
			return Fail("D3D12 queue Wait", hr);
		lap(split.wait12);
		ID3D12CommandList* lists[] = { traceList };
		queue_->ExecuteCommandLists(1, lists);
		lap(split.execute);
		const std::uint64_t doneValue = ++fenceValue_;
		if (FAILED(hr = queue_->Signal(fence_.Get(), doneValue)))
			return Fail("D3D12 queue Signal", hr);
		lap(split.signal12);
		context.fenceValue = doneValue;
		lastSubmitted_ = doneValue;
		for (auto& blas : blas_)
			if (blas.state == BlasState::Built && blas.buildFence == 0)
				blas.buildFence = doneValue;
		if (a_input.deferWait) {
			pendingWait_ = doneValue;
		} else {
			pendingWait_ = 0;
			if (FAILED(hr = context11_->Wait(fence11_.Get(), doneValue)))
				return Fail("D3D11 Wait", hr);
		}
		lap(split.wait11);
		stats_.split = split;
		stats_.cpuRecordMs = static_cast<float>(QpcToMs(QpcNow() - cpuStart)) - stats_.cpuBuildMs - stats_.cpuSkinMs;
		return true;
	}

	float Backend::WaitForTrace()
	{
		// (45l) The deferred D3D11 wait (GPU-side): right before the first D3D11 pass that reads the trace.
		if (pendingWait_ == 0 || !context11_ || !fence11_)
			return 0.0f;
		const std::int64_t start = QpcNow();
		const std::uint64_t value = pendingWait_;
		pendingWait_ = 0;
		if (FAILED(context11_->Wait(fence11_.Get(), value)))
			Fail("D3D11 Wait", E_FAIL);
		const float ms = static_cast<float>(QpcToMs(QpcNow() - start));
		stats_.split.wait11 += ms;
		return ms;
	}

	bool Backend::BuildOnly(std::uint32_t a_triangleBudget, std::uint32_t a_maxBuilds)
	{
		// (45k) Loading screens: the queued BLAS builds (and due compactions) go out on our own queue
		// with a larger cap; nothing D3D11 is involved (no shared texture, no fence wait on either
		// side), and nothing waits on the CPU. The usual per-submission cap keeps it TDR-safe.
		if (!ready_ || failed_)
			return false;
		const HRESULT removed = device_->GetDeviceRemovedReason();
		if (FAILED(removed))
			return Fail("D3D12 device", removed);
		CollectRetired();
		const std::uint64_t completed = fence_->GetCompletedValue();
		bool compactionDue = false;
		for (const auto& blas : blas_) {
			if (blas.state == BlasState::Built && blas.buildFence != 0 && blas.buildFence <= completed) {
				compactionDue = true;
				break;
			}
		}
		if (buildQueue_.empty() && !compactionDue)
			return false;
		std::size_t index = kContexts;
		for (std::size_t offset = 0; offset < kContexts; ++offset) {
			const std::size_t candidate = (cursor_ + offset) % kContexts;
			if (contexts_[candidate].fenceValue == 0 || contexts_[candidate].fenceValue <= completed) {
				index = candidate;
				break;
			}
		}
		if (index == kContexts)
			return false;
		for (std::size_t i = 0; i < kContexts; ++i)
			if (contexts_[i].fenceValue != 0 && contexts_[i].fenceValue <= completed)
				CollectTimestamps(i);
		auto& context = contexts_[index];
		cursor_ = (index + 1) % kContexts;
		HRESULT hr;
		if (FAILED(hr = context.allocator->Reset()))
			return Fail("command allocator Reset", hr);
		auto* list = context.list.Get();
		if (FAILED(hr = list->Reset(context.allocator.Get(), nullptr)))
			return Fail("command list Reset", hr);
		ProcessCompaction(list, completed, a_maxBuilds);
		const std::uint32_t built = BuildQueued(list, a_triangleBudget, a_maxBuilds);
		stats_.loadingBuilds += built;
		if (FAILED(hr = list->Close()))
			return Fail("command list Close", hr);
		if (failed_)
			return false;
		ID3D12CommandList* lists[] = { list };
		queue_->ExecuteCommandLists(1, lists);
		const std::uint64_t doneValue = ++fenceValue_;
		if (FAILED(hr = queue_->Signal(fence_.Get(), doneValue)))
			return Fail("D3D12 queue Signal", hr);
		context.fenceValue = doneValue;
		context.timestampsPending = false;
		lastSubmitted_ = doneValue;
		for (auto& blas : blas_)
			if (blas.state == BlasState::Built && blas.buildFence == 0)
				blas.buildFence = doneValue;
		return true;
	}
}
