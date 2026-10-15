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
// (46b) Point-light shadows (TracePointLightsCS).
#include "RayTracing/TracePointLightsCS.dxil.h"
// (46c) The same with RT_ALPHA: alpha-tested instances (Window Shadows Ultimate's window masks) tested against their cut-out.
#include "RayTracing/TracePointLightsAlphaCS.dxil.h"
// (46f) RTAO (TraceAOCS) and its alpha-tested variant.
#include "RayTracing/TraceAOAlphaCS.dxil.h"
#include "RayTracing/TraceAOCS.dxil.h"

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

		// (46g) A CPU budget in QPC ticks.
		std::int64_t MsToQpc(double a_ms)
		{
			static const double perMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart) / 1000.0;
			}();
			return static_cast<std::int64_t>(a_ms * perMs);
		}
	}

	// Process exit: no GPU wait (the GPU work of a closing game does not matter any more).
	// (45n) The release thread is gone at process exit (and may have died holding its lock): not joined.
	Backend::~Backend()
	{
		if (releaseThread_.joinable())
			releaseThread_.detach();
		if (poolThread_.joinable())
			poolThread_.detach();  // (46g) likewise
		Shutdown(false);
	}

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
		// (46p) RT Queue High Priority (an experiment; read at start-up only).
		queueDesc.Priority = queuePriorityHigh_ ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
		hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
		if (FAILED(hr) && queuePriorityHigh_) {
			logger::warn("[RayTracing] A high-priority queue was refused ({}); normal priority", HrString(hr));
			queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
			hr = device_->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue_));
		}
		if (FAILED(hr))
			return fail(std::format("CreateCommandQueue failed ({})", HrString(hr)));
		queueCreatedHigh_ = queueDesc.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
		logger::info("[RayTracing] Queue priority {}", queueCreatedHigh_ ? "high (46p experiment)" : "normal");
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
			// (46f) RTAO's own list (same allocator, recorded after the trace list is closed).
			ComPtr<ID3D12GraphicsCommandList> aoList;
			if (FAILED(hr = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.Get(), nullptr, IID_PPV_ARGS(&aoList))))
				return fail(std::format("CreateCommandList failed ({})", HrString(hr)));
			if (FAILED(aoList.As(&context.aoList)))
				return fail("ID3D12GraphicsCommandList4 not available");
			context.aoList->Close();
			// (46m) Split Trace Sync: the point lights' own list (same allocator, recorded after the sun list is closed).
			// Optional: without it the trace is never split.
			ComPtr<ID3D12GraphicsCommandList> pointList;
			if (SUCCEEDED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, context.allocator.Get(), nullptr, IID_PPV_ARGS(&pointList))) &&
				SUCCEEDED(pointList.As(&context.pointList)))
				context.pointList->Close();
			else
				context.pointList.Reset();
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
			heapDesc.Count = static_cast<UINT>(kContexts * 8);  // (45l) per context: prep begin / end, trace begin / end; (46b) + point lights begin / end after them; (46f) + RTAO
			UINT64 frequency = 0;
			timestampReadback_ = CreateBuffer(sizeof(std::uint64_t) * kContexts * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
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
		InitPools();  // (46g)

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
		// (46f) + one block of the run-time descriptors per command context (PublishDescriptors).
		heapDesc.NumDescriptors = kDynamicBase + static_cast<UINT>(kContexts) * kDynamicDescriptors;
		heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (FAILED(hr = device_->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&descriptorHeap_)))) {
			o_reason = std::format("CreateDescriptorHeap failed ({})", HrString(hr));
			return false;
		}
		descriptorSize_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		// (46f) The CPU-only staging heap the run-time descriptors are written to (null views until their textures exist).
		{
			D3D12_DESCRIPTOR_HEAP_DESC stagingDesc{};
			stagingDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
			stagingDesc.NumDescriptors = kDynamicDescriptors;
			stagingDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
			if (FAILED(hr = device_->CreateDescriptorHeap(&stagingDesc, IID_PPV_ARGS(&stagingHeap_)))) {
				o_reason = std::format("CreateDescriptorHeap (staging) failed ({})", HrString(hr));
				return false;
			}
			D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
			nullSrv.Format = kDepthMaskFormat;
			nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			nullSrv.Texture2D.MipLevels = 1;
			D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
			nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device_->CreateShaderResourceView(nullptr, &nullSrv, StagingCpu(kDynSun));
			nullUav.Format = kVisibilityFormat;
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynSun + 1));
			device_->CreateShaderResourceView(nullptr, &nullSrv, StagingCpu(kDynPoint));
			nullSrv.Format = kVisibilityFormat;
			device_->CreateShaderResourceView(nullptr, &nullSrv, StagingCpu(kDynPoint + 1));
			nullUav.Format = kPointFormat;
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynPoint + 2));
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynPoint + 3));
			nullSrv.Format = kDepthMaskFormat;
			device_->CreateShaderResourceView(nullptr, &nullSrv, StagingCpu(kDynAO));
			nullUav.Format = kAOFormat;
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynAO + 1));
		}

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
		// (46b) The point-light trace: not fatal either (point-light shadows then stay unavailable).
		if (!CreatePointPipeline())
			pointPipeline_.Reset();
		// (46f) RTAO: not fatal either (RTAO then stays unavailable).
		if (!CreateAOPipeline())
			aoPipeline_.Reset();
		return true;
	}

	bool Backend::CreatePointPipeline()
	{
		// Its own root signature: constants b0, TLAS t0, table (depth t1, sun visibility t2, PLVis u0, PLIdx u1), sources t3, ray counter u2;
		// (46c) the alpha-tested variant also: records t4, texcoords t5, table (copy layers t6, bindless game textures t0 space1), s0.
		// 52 + 2 + 1 + 2 + 2 + 2 + 2 + 1 = 64 DWORDs (the limit).
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 2;
		ranges[0].BaseShaderRegister = 1;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 2;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].OffsetInDescriptorsFromTableStart = 2;
		D3D12_DESCRIPTOR_RANGE alphaRanges[2]{};
		alphaRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		alphaRanges[0].NumDescriptors = 1;
		alphaRanges[0].BaseShaderRegister = 6;
		alphaRanges[0].OffsetInDescriptorsFromTableStart = 0;
		alphaRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		alphaRanges[1].NumDescriptors = kAlphaTextureSlots;
		alphaRanges[1].BaseShaderRegister = 0;
		alphaRanges[1].RegisterSpace = 1;
		alphaRanges[1].OffsetInDescriptorsFromTableStart = 1;
		D3D12_ROOT_PARAMETER params[8]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = sizeof(PointTraceConstants) / 4;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 2;
		params[2].DescriptorTable.pDescriptorRanges = ranges;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[3].Descriptor.ShaderRegister = 3;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[4].Descriptor.ShaderRegister = 2;
		params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[5].Descriptor.ShaderRegister = 4;
		params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[6].Descriptor.ShaderRegister = 5;
		params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[7].DescriptorTable.NumDescriptorRanges = 2;
		params[7].DescriptorTable.pDescriptorRanges = alphaRanges;
		params[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 8;
		rootDesc.pParameters = params;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (FAILED(hr) || FAILED(hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&pointRootSignature_)))) {
			logger::warn("[RayTracing] Point-light root signature failed ({}); point-light shadows unavailable", HrString(hr));
			pointRootSignature_.Reset();
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = pointRootSignature_.Get();
		psoDesc.CS.pShaderBytecode = g_TracePointLightsCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TracePointLightsCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pointPipeline_)))) {
			logger::warn("[RayTracing] The point-light ray query shader was rejected by the driver ({}); point-light shadows unavailable", HrString(hr));
			pointPipeline_.Reset();
			pointRootSignature_.Reset();
			return false;
		}
		// (46c) The alpha-tested variant: not fatal (window masks are then left out of the interior scene).
		psoDesc.CS.pShaderBytecode = g_TracePointLightsAlphaCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TracePointLightsAlphaCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pointAlphaPipeline_)))) {
			logger::warn("[RayTracing] The alpha-tested point-light ray query shader was rejected by the driver ({}); window masks unavailable", HrString(hr));
			pointAlphaPipeline_.Reset();
		}
		// (46f) Its table's null views were written into the staging heap (CreatePipeline) until the textures exist.
		return true;
	}

	void Backend::SetPointFault(std::string a_reason)
	{
		if (pointFault_.empty()) {
			pointFault_ = std::move(a_reason);
			logger::warn("[RayTracing] Point-light shadows: {}", pointFault_);
		}
	}

	bool Backend::EnsurePointTextures(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!PointAvailable() || !depthMask_.resource12)
			return false;
		if (a_width == pointWidth_ && a_height == pointHeight_ && pointVis_.resource12 && pointIdx_.resource12 && pointCounter_ && pointCounterMapped_)
			return true;
		ReleasePointResources();
		if (!CreateSharedTexture(a_width, a_height, kPointFormat, false, true, pointVis_, "RayTracing PointVisibility", false) ||
			!CreateSharedTexture(a_width, a_height, kPointFormat, false, true, pointIdx_, "RayTracing PointSources", false)) {
			pointVis_ = {};
			pointIdx_ = {};
			SetPointFault("could not create its shared textures (see the log)");
			return false;
		}
		pointCounter_ = CreateBuffer(kPointCounterWords * sizeof(std::uint32_t), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
		pointCounterReadback_ = CreateBuffer(sizeof(std::uint32_t) * kPointCounterWords * kContexts, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
		void* mapped = nullptr;
		if (!pointCounter_ || !pointCounterReadback_ || FAILED(pointCounterReadback_->Map(0, nullptr, &mapped)) || !mapped) {
			pointVis_ = {};
			pointIdx_ = {};
			pointCounter_.Reset();
			pointCounterReadback_.Reset();
			SetPointFault("could not create its ray counter");
			return false;
		}
		pointCounterMapped_ = static_cast<const std::uint32_t*>(mapped);
		pointCounterValid_ = false;
		pointWidth_ = a_width;
		pointHeight_ = a_height;
		pointBytes_ = static_cast<std::uint64_t>(a_width) * a_height * 8;
		WritePointDescriptors();
		logger::info("[RayTracing] Point-light textures {}x{}", a_width, a_height);
		return true;
	}

	void Backend::ReleasePointResources()
	{
		// A submission in flight may still use them: D3D12 keeps them until it is done.
		if (pointVis_.resource12)
			Retire(pointVis_.resource12, 0, nullptr);
		if (pointIdx_.resource12)
			Retire(pointIdx_.resource12, 0, nullptr);
		if (pointCounterReadback_ && pointCounterMapped_)
			pointCounterReadback_->Unmap(0, nullptr);
		pointCounterMapped_ = nullptr;
		if (pointCounter_)
			Retire(pointCounter_, 0, nullptr);
		if (pointCounterReadback_)
			Retire(pointCounterReadback_, 0, nullptr);
		pointCounter_.Reset();
		pointCounterReadback_.Reset();
		pointVis_ = {};
		pointIdx_ = {};
		pointWidth_ = pointHeight_ = 0;
		pointBytes_ = 0;
		pointCounterValid_ = false;
		for (auto& context : contexts_)
			context.pointPending = false;
		stats_.pointRaysPerFrame = -1;
		stats_.lastPointGpuMs = -1.0f;
		if (stagingHeap_ && device_) {
			// Back to null views (staging: the blocks of submissions in flight keep what they were recorded with).
			D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
			nullUav.Format = kPointFormat;
			nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynPoint + 2));
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynPoint + 3));
		}
	}

	void Backend::WritePointDescriptors()
	{
		if (!stagingHeap_ || !pointPipeline_)
			return;
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = kDepthMaskFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(depthMask_.resource12.Get(), &srvDesc, StagingCpu(kDynPoint));
		srvDesc.Format = kVisibilityFormat;
		device_->CreateShaderResourceView(visibility_.resource12.Get(), &srvDesc, StagingCpu(kDynPoint + 1));  // the sun trace (the sun gate)
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = kPointFormat;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(pointVis_.resource12.Get(), nullptr, &uavDesc, StagingCpu(kDynPoint + 2));
		device_->CreateUnorderedAccessView(pointIdx_.resource12.Get(), nullptr, &uavDesc, StagingCpu(kDynPoint + 3));
	}

	bool Backend::EnsurePointBuffer(CommandContext& a_context, std::uint32_t a_bytes)
	{
		if (a_context.pointBuffer && a_context.pointMapped && a_context.pointCapacity >= a_bytes)
			return true;
		if (a_context.pointBuffer) {
			if (a_context.pointMapped)
				a_context.pointBuffer->Unmap(0, nullptr);
			Retire(a_context.pointBuffer, 0, nullptr);
		}
		a_context.pointBuffer.Reset();
		a_context.pointMapped = nullptr;
		a_context.pointCapacity = 0;
		auto buffer = CreateBuffer(a_bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		void* mapped = nullptr;
		if (!buffer || FAILED(buffer->Map(0, nullptr, &mapped)) || !mapped)
			return false;
		a_context.pointBuffer = buffer;
		a_context.pointMapped = mapped;
		a_context.pointCapacity = a_bytes;
		return true;
	}

	void Backend::RecordPointTrace(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, std::size_t a_index, const FrameInput& a_input, bool a_alphaFrame,
		D3D12_GPU_DESCRIPTOR_HANDLE a_table)
	{
		// (46b) After the sun trace, in the same list: depth stays readable, the point textures are in UAV state
		// (the caller's barriers). The sources were copied into this context's upload buffer.
		const auto& pl = *a_input.pointLights;
		const UINT queryBase = static_cast<UINT>(kContexts * 4 + a_index * 2);
		if (timestampHeap_)
			a_list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);
		// (46c) The alpha-tested variant only when asked (interiors), the frame holds alpha-tested instances and their
		// records are in this context's copy (a_alphaFrame); every other frame runs exactly the 46b shader (cut-outs culled).
		const bool alpha = pl.alpha && a_alphaFrame && pointAlphaPipeline_ && alphaUv_ && a_context.alphaInfoBuffer;
		pointAlphaLastFrame_ = alpha;
		a_list->SetComputeRootSignature(pointRootSignature_.Get());
		a_list->SetPipelineState(alpha ? pointAlphaPipeline_.Get() : pointPipeline_.Get());
		a_list->SetComputeRoot32BitConstants(0, sizeof(PointTraceConstants) / 4, &pl.constants, 0);
		a_list->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
		a_list->SetComputeRootDescriptorTable(2, a_table);  // (46f) this context's copy of the point table
		a_list->SetComputeRootShaderResourceView(3, a_context.pointBuffer->GetGPUVirtualAddress());
		a_list->SetComputeRootUnorderedAccessView(4, pointCounter_->GetGPUVirtualAddress());
		if (alpha) {
			a_list->SetComputeRootShaderResourceView(5, a_context.alphaInfoBuffer->GetGPUVirtualAddress());
			a_list->SetComputeRootShaderResourceView(6, alphaUv_->GetGPUVirtualAddress());
			auto alphaTable = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
			alphaTable.ptr += static_cast<UINT64>(descriptorSize_) * (kAlphaDescriptorBase - 1);
			a_list->SetComputeRootDescriptorTable(7, alphaTable);
		}
		a_list->Dispatch((a_input.renderWidth + 7) / 8, (a_input.renderHeight + 7) / 8, 1);
		if (timestampHeap_)
			a_list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);
		// The ray counter's value after this frame, into this context's read-back slot.
		D3D12_RESOURCE_BARRIER toCopy = Transition(pointCounter_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		a_list->ResourceBarrier(1, &toCopy);
		a_list->CopyBufferRegion(pointCounterReadback_.Get(), sizeof(std::uint32_t) * kPointCounterWords * a_index, pointCounter_.Get(), 0,
			sizeof(std::uint32_t) * kPointCounterWords);  // (46g) rays, top-K churn, per-slot beam pixels
		D3D12_RESOURCE_BARRIER toUav = Transition(pointCounter_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		a_list->ResourceBarrier(1, &toUav);
		if (timestampHeap_ && timestampReadback_)
			a_list->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase, 2, timestampReadback_.Get(), sizeof(std::uint64_t) * queryBase);
		a_context.pointPending = true;
		a_context.pointFrame = ++pointFrame_;
	}

	// ---------------------------------------------------------------------------------------------
	// (46f) RTAO
	// ---------------------------------------------------------------------------------------------

	bool Backend::CreateAOPipeline()
	{
		// Its own root signature: constants b0, TLAS t0, table (depth t1, AO u0), ray counter u1;
		// the alpha-tested variant also: records t2, texcoords t3, table (copy layers t4, bindless game textures t0 space1), s0.
		// 44 + 2 + 1 + 2 + 2 + 2 + 1 = 54 DWORDs.
		D3D12_DESCRIPTOR_RANGE ranges[2]{};
		ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		ranges[0].NumDescriptors = 1;
		ranges[0].BaseShaderRegister = 1;
		ranges[0].OffsetInDescriptorsFromTableStart = 0;
		ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
		ranges[1].NumDescriptors = 1;
		ranges[1].BaseShaderRegister = 0;
		ranges[1].OffsetInDescriptorsFromTableStart = 1;
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
		D3D12_ROOT_PARAMETER params[7]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = sizeof(AOTraceConstants) / 4;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[2].DescriptorTable.NumDescriptorRanges = 2;
		params[2].DescriptorTable.pDescriptorRanges = ranges;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[3].Descriptor.ShaderRegister = 1;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[4].Descriptor.ShaderRegister = 2;
		params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[5].Descriptor.ShaderRegister = 3;
		params[5].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[6].DescriptorTable.NumDescriptorRanges = 2;
		params[6].DescriptorTable.pDescriptorRanges = alphaRanges;
		params[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_STATIC_SAMPLER_DESC sampler{};
		sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
		sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
		sampler.MaxLOD = D3D12_FLOAT32_MAX;
		sampler.ShaderRegister = 0;
		sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 7;
		rootDesc.pParameters = params;
		rootDesc.NumStaticSamplers = 1;
		rootDesc.pStaticSamplers = &sampler;
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (FAILED(hr) || FAILED(hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&aoRootSignature_)))) {
			logger::warn("[RayTracing] RTAO root signature failed ({}); ray-traced ambient occlusion unavailable", HrString(hr));
			aoRootSignature_.Reset();
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = aoRootSignature_.Get();
		psoDesc.CS.pShaderBytecode = g_TraceAOCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TraceAOCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&aoPipeline_)))) {
			logger::warn("[RayTracing] The RTAO ray query shader was rejected by the driver ({}); ray-traced ambient occlusion unavailable", HrString(hr));
			aoPipeline_.Reset();
			aoRootSignature_.Reset();
			return false;
		}
		psoDesc.CS.pShaderBytecode = g_TraceAOAlphaCS;
		psoDesc.CS.BytecodeLength = sizeof(g_TraceAOAlphaCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&aoAlphaPipeline_)))) {
			logger::warn("[RayTracing] The alpha-tested RTAO ray query shader was rejected by the driver ({}); RTAO ignores cut-outs", HrString(hr));
			aoAlphaPipeline_.Reset();
		}
		return true;
	}

	void Backend::SetAOFault(std::string a_reason)
	{
		if (aoFault_.empty()) {
			aoFault_ = std::move(a_reason);
			logger::warn("[RayTracing] RTAO: {}", aoFault_);
		}
	}

	bool Backend::EnsureAOTextures(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!AOAvailable() || !depthMask_.resource12)
			return false;
		HRESULT hr;
		if (!aoFence_ || !aoFence11_) {
			// The second fence: shared like the first (D3D12 signals, D3D11 waits on the GPU).
			aoFence_.Reset();
			aoFence11_.Reset();
			if (FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&aoFence_)))) {
				aoFence_.Reset();
				SetAOFault(std::format("could not create its fence ({})", HrString(hr)));
				return false;
			}
			HANDLE shared = nullptr;
			if (FAILED(hr = device_->CreateSharedHandle(aoFence_.Get(), nullptr, GENERIC_ALL, nullptr, &shared))) {
				aoFence_.Reset();
				SetAOFault(std::format("could not share its fence ({})", HrString(hr)));
				return false;
			}
			hr = device11_->OpenSharedFence(shared, IID_PPV_ARGS(&aoFence11_));
			CloseHandle(shared);
			if (FAILED(hr)) {
				aoFence_.Reset();
				aoFence11_.Reset();
				SetAOFault(std::format("D3D11 could not open its fence ({})", HrString(hr)));
				return false;
			}
			aoFenceValue_ = 0;
			pendingAOWait_ = 0;
		}
		if (a_width == aoWidth_ && a_height == aoHeight_ && aoRaw_.resource12 && aoCounter_ && aoCounterMapped_)
			return true;
		ReleaseAOResources();
		if (!CreateSharedTexture(a_width, a_height, kAOFormat, false, true, aoRaw_, "RayTracing RTAO", false)) {
			aoRaw_ = {};
			SetAOFault("could not create its shared texture (see the log)");
			return false;
		}
		aoCounter_ = CreateBuffer(256, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
		aoCounterReadback_ = CreateBuffer(sizeof(std::uint32_t) * kContexts, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
		void* mapped = nullptr;
		if (!aoCounter_ || !aoCounterReadback_ || FAILED(aoCounterReadback_->Map(0, nullptr, &mapped)) || !mapped) {
			aoRaw_ = {};
			aoCounter_.Reset();
			aoCounterReadback_.Reset();
			SetAOFault("could not create its ray counter");
			return false;
		}
		aoCounterMapped_ = static_cast<const std::uint32_t*>(mapped);
		aoCounterValid_ = false;
		aoWidth_ = a_width;
		aoHeight_ = a_height;
		aoBytes_ = static_cast<std::uint64_t>(a_width) * a_height * 4;
		WriteAODescriptors();
		logger::info("[RayTracing] RTAO texture {}x{}", a_width, a_height);
		return true;
	}

	void Backend::ReleaseAOResources()
	{
		// A submission in flight may still use them: D3D12 keeps them until it is done (Retire).
		if (aoRaw_.resource12)
			Retire(aoRaw_.resource12, 0, nullptr);
		if (aoCounterReadback_ && aoCounterMapped_)
			aoCounterReadback_->Unmap(0, nullptr);
		aoCounterMapped_ = nullptr;
		if (aoCounter_)
			Retire(aoCounter_, 0, nullptr);
		if (aoCounterReadback_)
			Retire(aoCounterReadback_, 0, nullptr);
		aoCounter_.Reset();
		aoCounterReadback_.Reset();
		aoRaw_ = {};
		aoWidth_ = aoHeight_ = 0;
		aoBytes_ = 0;
		aoCounterValid_ = false;
		for (auto& context : contexts_)
			context.aoPending = false;
		stats_.aoRaysPerFrame = -1;
		stats_.lastAOGpuMs = -1.0f;
		stats_.aoTracedLastFrame = false;
		if (stagingHeap_ && device_) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC nullUav{};
			nullUav.Format = kAOFormat;
			nullUav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			device_->CreateUnorderedAccessView(nullptr, nullptr, &nullUav, StagingCpu(kDynAO + 1));
		}
	}

	void Backend::WriteAODescriptors()
	{
		if (!stagingHeap_ || !aoPipeline_)
			return;
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = kDepthMaskFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(depthMask_.resource12.Get(), &srvDesc, StagingCpu(kDynAO));
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = kAOFormat;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(aoRaw_.resource12.Get(), nullptr, &uavDesc, StagingCpu(kDynAO + 1));
	}

	bool Backend::RecordAOTrace(CommandContext& a_context, std::size_t a_index, const FrameInput& a_input, bool a_alphaFrame, D3D12_GPU_DESCRIPTOR_HANDLE a_table)
	{
		if (!a_input.ao || !AOAvailable() || !aoRaw_.resource12 || !aoCounter_ || !aoCounterMapped_ || !aoFence_ || !aoFence11_ || !a_context.aoList ||
			!depthMask_.resource12 || !tlas_)
			return false;
		// Same allocator as the prep and trace lists (both closed by now); reset with it once the context is reused.
		auto* list = a_context.aoList.Get();
		HRESULT hr;
		if (FAILED(hr = list->Reset(a_context.allocator.Get(), nullptr))) {
			Fail("RTAO command list Reset", hr);
			return false;
		}
		ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
		list->SetDescriptorHeaps(1, heaps);
		D3D12_RESOURCE_BARRIER pre[2] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(aoRaw_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		list->ResourceBarrier(2, pre);
		const UINT queryBase = static_cast<UINT>(kContexts * 6 + a_index * 2);
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);
		// The alpha-tested variant only when asked, the frame holds alpha-tested instances and their records are in this
		// context's copy; otherwise the cut-outs are culled (as the point lights' 46b shader).
		const bool alpha = a_input.ao->alpha && a_alphaFrame && aoAlphaPipeline_ && alphaUv_ && a_context.alphaInfoBuffer;
		stats_.aoAlphaLastFrame = alpha;
		list->SetComputeRootSignature(aoRootSignature_.Get());
		list->SetPipelineState(alpha ? aoAlphaPipeline_.Get() : aoPipeline_.Get());
		list->SetComputeRoot32BitConstants(0, sizeof(AOTraceConstants) / 4, &a_input.ao->constants, 0);
		list->SetComputeRootShaderResourceView(1, tlas_->GetGPUVirtualAddress());
		list->SetComputeRootDescriptorTable(2, a_table);
		list->SetComputeRootUnorderedAccessView(3, aoCounter_->GetGPUVirtualAddress());
		if (alpha) {
			list->SetComputeRootShaderResourceView(4, a_context.alphaInfoBuffer->GetGPUVirtualAddress());
			list->SetComputeRootShaderResourceView(5, alphaUv_->GetGPUVirtualAddress());
			auto alphaTable = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
			alphaTable.ptr += static_cast<UINT64>(descriptorSize_) * (kAlphaDescriptorBase - 1);
			list->SetComputeRootDescriptorTable(6, alphaTable);
		}
		list->Dispatch((a_input.renderWidth + 7) / 8, (a_input.renderHeight + 7) / 8, 1);
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);
		D3D12_RESOURCE_BARRIER post[2] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(aoRaw_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
		};
		list->ResourceBarrier(2, post);
		// The ray counter's value after this frame, into this context's read-back slot.
		D3D12_RESOURCE_BARRIER toCopy = Transition(aoCounter_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
		list->ResourceBarrier(1, &toCopy);
		list->CopyBufferRegion(aoCounterReadback_.Get(), sizeof(std::uint32_t) * a_index, aoCounter_.Get(), 0, sizeof(std::uint32_t));
		D3D12_RESOURCE_BARRIER toUav = Transition(aoCounter_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		list->ResourceBarrier(1, &toUav);
		if (timestampHeap_ && timestampReadback_)
			list->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase, 2, timestampReadback_.Get(), sizeof(std::uint64_t) * queryBase);
		if (FAILED(hr = list->Close())) {
			Fail("RTAO command list Close", hr);
			return false;
		}
		return true;
	}

	float Backend::WaitForAO()
	{
		// (46f) The second fence (GPU-side): right before the first D3D11 pass that reads the RTAO result.
		if (pendingAOWait_ == 0 || !context11_ || !aoFence11_)
			return 0.0f;
		const std::int64_t start = QpcNow();
		const std::uint64_t value = pendingAOWait_;
		pendingAOWait_ = 0;
		if (FAILED(context11_->Wait(aoFence11_.Get(), value)))
			Fail("D3D11 Wait (RTAO)", E_FAIL);
		return static_cast<float>(QpcToMs(QpcNow() - start));
	}

	void Backend::Shutdown(bool a_waitForGpu)
	{
		if (a_waitForGpu && fence_ && fenceEvent_ && lastSubmitted_ && fence_->GetCompletedValue() < lastSubmitted_) {
			if (SUCCEEDED(fence_->SetEventOnCompletion(lastSubmitted_, fenceEvent_)))
				WaitForSingleObject(fenceEvent_, 2000);
		}
		// (46f) The last RTAO list (behind the second fence) too.
		if (a_waitForGpu && aoFence_ && fenceEvent_ && aoFenceValue_ && aoFence_->GetCompletedValue() < aoFenceValue_) {
			if (SUCCEEDED(aoFence_->SetEventOnCompletion(aoFenceValue_, fenceEvent_)))
				WaitForSingleObject(fenceEvent_, 2000);
		}
		ready_ = false;
		// (45n) Whatever the release thread still holds goes first (it never touches anything else of ours).
		StopReleaseThread();
		StopPoolThread();  // (46g) before the device goes
		for (auto* pool : Pools())
			pool->Reset();
		postbuildOwner_.clear();
		transition_ = false;
		prepared_ = false;
		preparedContext_ = 0;
		retireAhead_ = 1;
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
			if (context.pointBuffer && context.pointMapped)
				context.pointBuffer->Unmap(0, nullptr);
			context = {};
		}
		// (46b)
		if (pointCounterReadback_ && pointCounterMapped_)
			pointCounterReadback_->Unmap(0, nullptr);
		pointCounterMapped_ = nullptr;
		pointCounter_.Reset();
		pointCounterReadback_.Reset();
		pointVis_ = {};
		pointIdx_ = {};
		pointWidth_ = pointHeight_ = 0;
		pointBytes_ = 0;
		pointCounterValid_ = false;
		pointFrame_ = 0;
		pointFault_.clear();
		pointPipeline_.Reset();
		pointAlphaPipeline_.Reset();
		pointAlphaLastFrame_ = false;
		pointRootSignature_.Reset();
		// (46f)
		if (aoCounterReadback_ && aoCounterMapped_)
			aoCounterReadback_->Unmap(0, nullptr);
		aoCounterMapped_ = nullptr;
		aoCounter_.Reset();
		aoCounterReadback_.Reset();
		aoRaw_ = {};
		aoWidth_ = aoHeight_ = 0;
		aoBytes_ = 0;
		aoCounterValid_ = false;
		aoFrame_ = 0;
		aoFault_.clear();
		aoPipeline_.Reset();
		aoAlphaPipeline_.Reset();
		aoRootSignature_.Reset();
		aoFence11_.Reset();
		aoFence_.Reset();
		aoFenceValue_ = 0;
		pendingAOWait_ = 0;
		stagingHeap_.Reset();
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
		freeAlphaLayers_.clear();
		nextAlphaLayer_ = 0;
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
		// (46m) Merged BLASes (their pool is gone with the pools above) and the sun fence.
		skinGroups_.clear();
		skinGroupFrame_.clear();
		sunFence11_.Reset();
		sunFence_.Reset();
		sunFenceValue_ = 0;
		pendingSunWait_ = 0;
		sunFenceFailed_ = false;
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

	ComPtr<ID3D12Resource> Backend::CreateBuffer(std::uint64_t a_bytes, D3D12_HEAP_TYPE a_heap, D3D12_RESOURCE_STATES a_state, bool a_uav,
		Util::FrameTrace::VramOwner a_owner)
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
		// (46p) Counted per owner (upload / read-back heaps are system memory: their own owners).
		const auto owner = a_heap == D3D12_HEAP_TYPE_UPLOAD ? Util::FrameTrace::VramOwner::RtUpload :
		                   a_heap == D3D12_HEAP_TYPE_READBACK ? Util::FrameTrace::VramOwner::RtReadback :
		                                                         a_owner;
		Util::FrameTrace::TagVram(resource.Get(), owner, (desc.Width + 0xFFFFull) & ~0xFFFFull);
		if (a_heap == D3D12_HEAP_TYPE_DEFAULT && ResidencyHigh())
			ApplyResidency(resource.Get());  // (46p)
		return resource;
	}

	void Backend::ApplyResidency(ID3D12Pageable* const* a_objects, std::size_t a_count)
	{
		// (46p) Keep Our GPU Memory Resident. The device is free-threaded (the pool thread calls this too).
		ComPtr<ID3D12Device1> device1;
		if (!device_ || FAILED(device_.As(&device1)))
			return;
		std::vector<ID3D12Pageable*> objects;
		objects.reserve(a_count);
		for (std::size_t i = 0; i < a_count; ++i)
			if (a_objects[i])
				objects.push_back(a_objects[i]);
		if (objects.empty())
			return;
		const D3D12_RESIDENCY_PRIORITY priority = ResidencyHigh() ? D3D12_RESIDENCY_PRIORITY_HIGH : D3D12_RESIDENCY_PRIORITY_NORMAL;
		const std::vector<D3D12_RESIDENCY_PRIORITY> priorities(objects.size(), priority);
		const HRESULT hr = device1->SetResidencyPriority(static_cast<UINT>(objects.size()), objects.data(), priorities.data());
		static bool logged = false;
		if (FAILED(hr) && !logged) {
			logged = true;
			logger::warn("[RayTracing] SetResidencyPriority failed ({})", HrString(hr));
		}
	}

	void Backend::SetResidencyHigh(bool a_high)
	{
		if (residencyHigh_.load(std::memory_order_relaxed) == a_high)
			return;
		residencyHigh_.store(a_high, std::memory_order_relaxed);
		if (!ready_)
			return;
		// What exists now (upload / read-back heaps are system memory: not listed).
		std::vector<ID3D12Pageable*> objects;
		for (auto* pool : Pools()) {
			if (pool->heap == D3D12_HEAP_TYPE_UPLOAD)
				continue;
			for (const auto& block : pool->blocks)
				if (block.live && block.resource)
					objects.push_back(block.resource.Get());
		}
		for (auto* resource : { tlas_.Get(), scratch_.Get(), skinScratch_.Get(), alphaUv_.Get(), alphaLayers12_.Get(), postbuild_.Get(), pointCounter_.Get(),
				 aoCounter_.Get(), depthMask_.resource12.Get(), visibility_.resource12.Get(), pointVis_.resource12.Get(), pointIdx_.resource12.Get(),
				 aoRaw_.resource12.Get() })
			if (resource)
				objects.push_back(resource);
		ApplyResidency(objects.data(), objects.size());
		logger::info("[RayTracing] Residency priority {} for {} resources", a_high ? "high" : "normal", objects.size());
	}

	void Backend::ReleaseIdleResources()
	{
		// (46p) Free When Off. Retire keeps each D3D12 object until the GPU is done with it.
		if (!ready_)
			return;
		if (scratch_) {
			Retire(scratch_, scratchBytes_, &stats_.scratchBytes);
			scratch_.Reset();
			scratchBytes_ = 0;
		}
		if (tlas_) {
			Retire(tlas_, tlasBytes_, &stats_.tlasBytes);
			tlas_.Reset();
			tlasBytes_ = 0;
			tlasCapacity_ = 0;
		}
		if (skinScratch_) {
			Retire(skinScratch_, skinScratchBytes_, &skinStats_.scratchBytes);
			skinScratch_.Reset();
			skinScratchBytes_ = 0;
		}
		if (depthMask_.resource12)
			Retire(depthMask_.resource12, 0, nullptr);
		if (visibility_.resource12)
			Retire(visibility_.resource12, 0, nullptr);
		depthMask_ = {};
		visibility_ = {};
		textureWidth_ = textureHeight_ = 0;
		stats_.sharedTextureBytes = 0;
		logger::info("[RayTracing] Switched off: shared textures, scratch and TLAS released (46p Free When Off)");
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
		SharedTexture& o_texture, const char* a_name, bool a_fatal)
	{
		if (!CheckSharedFormat(a_format, a_d3d11Store, a_d3d12Store, a_name, a_fatal))
			return false;
		// (46b) Not fatal (point lights): a failure is logged and only that part goes off.
		auto fail = [&](const std::string& a_operation, HRESULT a_result) {
			if (a_fatal)
				return this->Fail(a_operation, a_result);
			logger::warn("[RayTracing] {} failed ({})", a_operation, HrString(a_result));
			return false;
		};
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
			return fail(std::format("CreateTexture2D (shared) {}", what), hr);
		Util::SetResourceName(texture.resource11.Get(), a_name);
		if (FAILED(hr = device11_->CreateShaderResourceView(texture.resource11.Get(), nullptr, &texture.srv11)) ||
			FAILED(hr = device11_->CreateUnorderedAccessView(texture.resource11.Get(), nullptr, &texture.uav11)))
			return fail(std::format("shared texture views {}", what), hr);
		ComPtr<IDXGIResource1> dxgiResource;
		if (FAILED(hr = texture.resource11.As(&dxgiResource)))
			return fail("IDXGIResource1", hr);
		HANDLE handle = nullptr;
		if (FAILED(hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
			return fail(std::format("CreateSharedHandle {}", what), hr);
		hr = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&texture.resource12));
		CloseHandle(handle);
		if (FAILED(hr))
			return fail(std::format("OpenSharedHandle {}", what), hr);
		if (ResidencyHigh())
			ApplyResidency(texture.resource12.Get());  // (46p) the D3D11 side: FrameTrace's create hook (eviction priority)
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
		if (pointVis_.resource12)
			WritePointDescriptors();  // (46b) the depth SRV of its table
		if (aoRaw_.resource12)
			WriteAODescriptors();  // (46f) likewise
		logger::info("[RayTracing] Shared textures {}x{}", a_width, a_height);
		return true;
	}

	void Backend::WriteDescriptors()
	{
		// (46f) Into the staging heap; each submission records with its own copy (PublishDescriptors).
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = kDepthMaskFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = 1;
		device_->CreateShaderResourceView(depthMask_.resource12.Get(), &srvDesc, StagingCpu(kDynSun));
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = kVisibilityFormat;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(visibility_.resource12.Get(), nullptr, &uavDesc, StagingCpu(kDynSun + 1));
	}

	D3D12_CPU_DESCRIPTOR_HANDLE Backend::StagingCpu(std::uint32_t a_slot) const
	{
		auto cpu = stagingHeap_->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(descriptorSize_) * a_slot;
		return cpu;
	}

	D3D12_GPU_DESCRIPTOR_HANDLE Backend::PublishDescriptors(std::size_t a_index)
	{
		// (46f) The context's block is used only by its own submissions, and a context is recorded only once its last
		// submission (and RTAO list) completed: this copy never rewrites a descriptor the GPU may still read.
		const UINT block = kDynamicBase + static_cast<UINT>(a_index) * kDynamicDescriptors;
		auto dst = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
		dst.ptr += static_cast<SIZE_T>(descriptorSize_) * block;
		device_->CopyDescriptorsSimple(kDynamicDescriptors, dst, stagingHeap_->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		auto gpu = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
		gpu.ptr += static_cast<UINT64>(descriptorSize_) * block;
		return gpu;
	}

	void Backend::Retire(ComPtr<ID3D12Pageable> a_object, std::uint64_t a_bytes, std::uint64_t* a_counter)
	{
		if (!a_object)
			return;
		// The next value anyone signals: the done value of the list being recorded, or the next
		// frame's D3D11 signal, which the D3D11 queue only reaches after waiting for every
		// D3D12 submission so far. Either way everything that may use the object is finished.
		// (45n) RetireValue: +2 while builds / skinning go out without the D3D11 ready signal behind them.
		retired_.push_back(Retired{ RetireValue(), std::move(a_object), a_bytes, a_counter });
	}

	void Backend::CollectRetired()
	{
		if (!fence_)
			return;
		const std::int64_t start = QpcNow();
		const std::uint64_t completed = fence_->GetCompletedValue();
		// (45n) Release Thread: the objects are only moved out here; their (costly) final Release runs on the
		// worker. The D3D12 device is free-threaded and nothing else of ours references a retired object.
		std::vector<ComPtr<ID3D12Pageable>> handOff;
		std::uint32_t count = 0;
		std::erase_if(retired_, [&](Retired& a_entry) {
			if (a_entry.fence > completed)
				return false;
			if (a_entry.counter)
				*a_entry.counter -= std::min(*a_entry.counter, a_entry.bytes);
			++count;
			if (a_entry.object)
				handOff.push_back(std::move(a_entry.object));
			return true;
		});
		if (!handOff.empty()) {
			{
				std::scoped_lock lock(releaseMutex_);
				if (!releaseThread_.joinable()) {
					releaseStop_ = false;
					releaseThread_ = std::thread([this] { ReleaseThreadMain(); });
				}
				for (auto& object : handOff)
					releaseQueue_.push_back(std::move(object));
			}
			releaseCv_.notify_one();
		}
		CollectAlphaRetired(completed);
		stats_.retiredLastCollect = count;
		stats_.retireLastMs = static_cast<float>(QpcToMs(QpcNow() - start));
		PumpPools();  // (46g) deferred pool frees, finished blocks, growth
	}

	void Backend::ReleaseThreadMain()
	{
		SetThreadDescription(GetCurrentThread(), L"CS RayTracing release");
		std::vector<ComPtr<ID3D12Pageable>> batch;
		for (;;) {
			{
				std::unique_lock lock(releaseMutex_);
				releaseCv_.wait(lock, [this] { return releaseStop_ || !releaseQueue_.empty(); });
				batch.swap(releaseQueue_);
				if (batch.empty() && releaseStop_)
					return;
			}
			const std::int64_t start = QpcNow();
			const auto n = batch.size();
			batch.clear();  // the final Release of each object
			workerReleaseMs_.store(static_cast<float>(QpcToMs(QpcNow() - start)), std::memory_order_relaxed);
			workerReleased_.fetch_add(n, std::memory_order_relaxed);
		}
	}

	void Backend::StopReleaseThread()
	{
		if (!releaseThread_.joinable()) {
			releaseQueue_.clear();
			return;
		}
		{
			std::scoped_lock lock(releaseMutex_);
			releaseStop_ = true;
		}
		releaseCv_.notify_one();
		if (releaseThread_.joinable())
			releaseThread_.join();  // drains the queue first
		std::scoped_lock lock(releaseMutex_);
		releaseQueue_.clear();
		releaseStop_ = false;
	}

	void Backend::CollectTimestamps(std::size_t a_index)
	{
		auto& context = contexts_[a_index];
		// (46b) The point-light trace of this submission: its ray counter (rays per frame since the last read)
		// and its two timestamps (read below with the others).
		const bool pointPending = context.pointPending;
		context.pointPending = false;
		if (pointPending && pointCounterMapped_) {
			const std::uint32_t* words = pointCounterMapped_ + static_cast<std::size_t>(kPointCounterWords) * a_index;
			const std::uint32_t value = words[0];
			if (pointCounterValid_ && context.pointFrame > pointCounterLastFrame_) {
				const auto frames = static_cast<std::uint32_t>(context.pointFrame - pointCounterLastFrame_);
				stats_.pointRaysPerFrame = static_cast<std::int64_t>(static_cast<std::uint32_t>(value - pointCounterLast_) / frames);
				// (46g) Top-K churn and the window beams' sampled pixels (traced / blocked), per frame.
				stats_.pointChurnPerFrame = static_cast<std::int64_t>(static_cast<std::uint32_t>(words[1] - pointCounterWords_[1]) / frames);
				for (std::uint32_t slot = 0; slot < kPointBeamSlots; ++slot) {
					const std::uint32_t w = kPointBeamBase + slot * 2;
					stats_.beamTraced[slot] = static_cast<std::uint32_t>(words[w] - pointCounterWords_[w]) / frames;
					stats_.beamBlocked[slot] = static_cast<std::uint32_t>(words[w + 1] - pointCounterWords_[w + 1]) / frames;
				}
				++stats_.beamSamples;
			}
			if (!pointCounterValid_ || context.pointFrame > pointCounterLastFrame_) {
				pointCounterLast_ = value;
				std::copy_n(words, kPointCounterWords, pointCounterWords_.begin());
				pointCounterLastFrame_ = context.pointFrame;
				pointCounterValid_ = true;
			}
		}
		// (46f) This submission's RTAO list: its ray counter and its two timestamps (own pair, after the point lights').
		if (context.aoPending) {
			context.aoPending = false;
			if (aoCounterMapped_) {
				const std::uint32_t value = aoCounterMapped_[a_index];
				if (aoCounterValid_ && context.aoFrame > aoCounterLastFrame_)
					stats_.aoRaysPerFrame = static_cast<std::int64_t>(static_cast<std::uint32_t>(value - aoCounterLast_) / (context.aoFrame - aoCounterLastFrame_));
				if (!aoCounterValid_ || context.aoFrame > aoCounterLastFrame_) {
					aoCounterLast_ = value;
					aoCounterLastFrame_ = context.aoFrame;
					aoCounterValid_ = true;
				}
			}
			if (timestampReadback_ && timestampFrequency_ != 0) {
				const SIZE_T aoOffset = sizeof(std::uint64_t) * (kContexts * 6 + a_index * 2);
				D3D12_RANGE aoRange{ aoOffset, aoOffset + sizeof(std::uint64_t) * 2 };
				void* aoMapped = nullptr;
				if (SUCCEEDED(timestampReadback_->Map(0, &aoRange, &aoMapped)) && aoMapped) {
					const auto* t = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(aoMapped) + aoOffset);
					const std::uint64_t ab = t[0], ae = t[1];
					D3D12_RANGE none{ 0, 0 };
					timestampReadback_->Unmap(0, &none);
					if (ae >= ab)
						stats_.lastAOGpuMs = static_cast<float>(static_cast<double>(ae - ab) * 1000.0 / static_cast<double>(timestampFrequency_));
				}
			}
		}
		if (!context.timestampsPending || !timestampReadback_ || timestampFrequency_ == 0)
			return;
		context.timestampsPending = false;
		float pointMs = 0.0f;
		if (pointPending) {
			const SIZE_T pointOffset = sizeof(std::uint64_t) * (kContexts * 4 + a_index * 2);
			D3D12_RANGE pointRange{ pointOffset, pointOffset + sizeof(std::uint64_t) * 2 };
			void* pointMapped = nullptr;
			if (SUCCEEDED(timestampReadback_->Map(0, &pointRange, &pointMapped)) && pointMapped) {
				const auto* t = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(pointMapped) + pointOffset);
				const std::uint64_t pb = t[0], pe = t[1];
				D3D12_RANGE none{ 0, 0 };
				timestampReadback_->Unmap(0, &none);
				if (pe >= pb)
					pointMs = static_cast<float>(static_cast<double>(pe - pb) * 1000.0 / static_cast<double>(timestampFrequency_));
			}
			stats_.lastPointGpuMs = pointMs;
		} else {
			stats_.lastPointGpuMs = -1.0f;
		}
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
		stats_.lastPrepWork = context.work;  // (46p)
		// (46b) The trace interval holds the point-light dispatch: the sun's part alone, the point lights on their own.
		// (46m) Split Trace Sync: the interval ended with the sun already.
		stats_.lastTraceGpuMs = std::max(static_cast<float>(static_cast<double>(end - begin) * toMs) - (context.splitTrace ? 0.0f : pointMs), 0.0f);
		stats_.lastGpuMs = stats_.lastPrepGpuMs + stats_.lastTraceGpuMs + pointMs;
		// (45n) Our queue idle between the builds / skinning and the trace (waiting for the game's depth copy).
		stats_.lastQueueGapMs = begin >= prepEnd ? static_cast<float>(static_cast<double>(begin - prepEnd) * toMs) : 0.0f;
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
		if (blas.alloc.Valid())
			asPool_.Free(blas.alloc, RetireValue(), &stats_.blasBytes, blas.bytes);
		if (blas.postbuildSlot < kPostbuildSlots && blas.postbuildSlot < postbuildOwner_.size())
			postbuildOwner_[blas.postbuildSlot] = 0;
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
		scratch_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, Util::FrameTrace::VramOwner::RtScratch);
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
		tlas_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true, Util::FrameTrace::VramOwner::RtTlas);
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

	void Backend::ProcessCompaction(ID3D12GraphicsCommandList4* a_list, std::uint64_t a_completed, std::uint32_t a_max, std::int64_t a_deadline)
	{
		stats_.compactionsLastFrame = 0;
		std::uint32_t created = 0;
		for (std::size_t h = 0; h < blas_.size(); ++h) {
			auto& blas = blas_[h];
			if (blas.state != BlasState::Built || blas.buildFence == 0 || blas.buildFence > a_completed)
				continue;
			// (45j) A bounded number per frame; the rest stay usable (Built) until a later frame. (46g) And a CPU budget.
			if (created >= a_max || (created > 0 && QpcNow() >= a_deadline))
				break;
			// (46g) The slot may have been taken by a newer build since (owner check): no compaction then.
			const bool slotOwned = blas.postbuildSlot < kPostbuildSlots && blas.postbuildSlot < postbuildOwner_.size() &&
			                       postbuildOwner_[blas.postbuildSlot] == static_cast<std::uint64_t>(h) + 1;
			const std::uint64_t compactedSize = slotOwned ? postbuildMapped_[blas.postbuildSlot] : 0;
			if (slotOwned)
				postbuildOwner_[blas.postbuildSlot] = 0;
			blas.postbuildSlot = UINT32_MAX;
			const std::uint64_t bytes = Align(compactedSize, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			// Not worth a second range when it saves under 10 %.
			if (compactedSize == 0 || bytes * 10 >= blas.bytes * 9) {
				blas.state = BlasState::Ready;
				continue;
			}
			ArenaAlloc compacted;
			if (!asPool_.Allocate(bytes, compacted)) {
				// (46g) The pool grows (next frames); this BLAS stays usable uncompacted.
				++stats_.pools.allocFailsLastFrame;
				blas.state = BlasState::Ready;
				continue;
			}
			++created;
			a_list->CopyRaytracingAccelerationStructure(asPool_.Address(compacted), asPool_.Address(blas.alloc),
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
			stats_.compactedSavedBytes += blas.bytes - bytes;
			asPool_.Free(blas.alloc, RetireValue(), &stats_.blasBytes, blas.bytes);
			blas.alloc = compacted;
			blas.bytes = bytes;
			stats_.blasBytes += bytes;
			blas.state = BlasState::Ready;
			++stats_.compactionsLastFrame;
			++recWork_.compactions;  // (46p)
			recWork_.compactBytes += bytes;
		}
	}

	std::uint32_t Backend::BuildQueued(ID3D12GraphicsCommandList4* a_list, std::uint32_t a_budget, std::size_t a_maxBuilds, std::int64_t a_deadline)
	{
		std::uint32_t built = 0;
		std::vector<std::uint32_t> slots;
		if (!buildQueue_.empty() && (asPool_.failed || uploadPool_.failed)) {
			// Out of video memory for a new block: as when a BLAS buffer could not be created before 46g.
			Fail(asPool_.failed ? "BLAS pool block" : "upload pool block", E_OUTOFMEMORY);
			return 0;
		}

		// (45e) This frame's batch: up to the triangle budget (one submission stays TDR-safe) and a_maxBuilds builds.
		// (46g) Each BLAS is a range of asPool_ and its geometry a range of uploadPool_ (no resource creation): the
		// render-thread cost is the size query and the copy, bounded by a_deadline. A BLAS whose range does not fit
		// waits in the queue for the pool's next block.
		struct Pending
		{
			BlasHandle handle;
			ArenaAlloc result;
			ArenaAlloc upload;
			std::uint64_t vertexOffset;  // inside the upload range
			std::uint64_t indexOffset;
			std::uint64_t uvOffset;  // (45h) alpha texcoords (alpha BLASes only)
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info;
		};
		std::vector<Pending> batch;
		std::uint32_t batchTriangles = 0;
		std::uint64_t scratchNeed = 0;
		std::uint64_t scratchSum = 0;  // (46p) every build its own slice
		while (!buildQueue_.empty() && batch.size() < a_maxBuilds && (batch.empty() || (batchTriangles < a_budget && QpcNow() < a_deadline))) {
			const BlasHandle handle = buildQueue_.front();
			auto& blas = blas_[handle];
			if (blas.state != BlasState::Queued || !blas.geometry) {
				buildQueue_.pop_front();
				continue;
			}
			const ChunkGeometry& geometry = *blas.geometry;
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometryDesc.Flags = geometry.Opaque() ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
			geometryDesc.Triangles.VertexCount = geometry.Vertices();
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexCount = static_cast<UINT>(geometry.indices.size());
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
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
			Pending entry{ handle, {}, {}, 0, 0, 0, info };
			if (!asPool_.Allocate(info.ResultDataMaxSizeInBytes, entry.result)) {
				++stats_.pools.allocFailsLastFrame;
				break;
			}
			entry.indexOffset = Align(geometry.positions.size() * sizeof(float), 256);
			std::uint64_t uploadBytes = entry.indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
			if (!geometry.Opaque()) {
				entry.uvOffset = Align(uploadBytes, 256);
				uploadBytes = entry.uvOffset + geometry.alphaUV.size() * sizeof(std::uint32_t);
			}
			if (!uploadPool_.Allocate(uploadBytes, entry.upload)) {
				asPool_.Free(entry.result, 0);  // never used
				++stats_.pools.allocFailsLastFrame;
				break;
			}
			std::byte* mapped = uploadPool_.Mapped(entry.upload);
			std::memcpy(mapped, geometry.positions.data(), geometry.positions.size() * sizeof(float));
			std::memcpy(mapped + entry.indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			if (!geometry.Opaque())
				std::memcpy(mapped + entry.uvOffset, geometry.alphaUV.data(), geometry.alphaUV.size() * sizeof(std::uint32_t));
			scratchNeed = std::max(scratchNeed, info.ScratchDataSizeInBytes);
			scratchSum += Align(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);  // (46p)
			batchTriangles += geometry.Triangles();
			buildQueue_.pop_front();
			batch.push_back(entry);
		}
		if (batch.empty())
			return 0;
		// Freed once this submission completes.
		for (const auto& entry : batch)
			uploadPool_.Free(entry.upload, RetireValue());
		// (46p) Parallel BLAS Builds: each build its own scratch slice, no barrier between them (one at the batch end, below),
		// while the slices fit kParallelScratchBytes; else one shared scratch and a barrier before each build (46o).
		const bool parallel = parallelBuilds_ && batch.size() > 1 && scratchSum <= kParallelScratchBytes;
		stats_.parallelBuildsLastFrame = parallel;
		if (!EnsureScratch(parallel ? scratchSum : scratchNeed)) {
			for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
				asPool_.Free(it->result, 0);
				buildQueue_.push_front(it->handle);
			}
			return 0;
		}

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
					a_list->CopyBufferRegion(alphaUv_.Get(), static_cast<std::uint64_t>(blas.uvOffset) * 8, uploadPool_.Resource(entry.upload),
						entry.upload.offset + entry.uvOffset, static_cast<std::uint64_t>(blas.uvCount) * 8);
				}
				auto back = Transition(alphaUv_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
				a_list->ResourceBarrier(1, &back);
			}
		}

		std::uint64_t scratchOffset = 0;  // (46p) Parallel BLAS Builds: this build's slice
		for (const auto& entry : batch) {
			auto& blas = blas_[entry.handle];
			const ChunkGeometry& geometry = *blas.geometry;
			const D3D12_GPU_VIRTUAL_ADDRESS upload = uploadPool_.Address(entry.upload);

			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			// (45h) Alpha-tested meshes are non-opaque: the trace tests each candidate against its cut-out.
			geometryDesc.Flags = geometry.Opaque() ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			geometryDesc.Triangles.VertexBuffer.StartAddress = upload;
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
			geometryDesc.Triangles.VertexCount = geometry.Vertices();
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexBuffer = upload + entry.indexOffset;
			geometryDesc.Triangles.IndexCount = static_cast<UINT>(geometry.indices.size());
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &geometryDesc;

			if (built > 0 && !parallel) {
				// The builds share one scratch buffer.
				auto barrier = UavBarrier(scratch_.Get());
				a_list->ResourceBarrier(1, &barrier);
			}

			const std::uint32_t slot = postbuildCursor_;
			postbuildCursor_ = (postbuildCursor_ + 1) % kPostbuildSlots;
			// (46g) A BLAS still waiting for its compaction size in this slot loses its compaction (never reads another's).
			if (slot < postbuildOwner_.size()) {
				if (const std::uint64_t owner = postbuildOwner_[slot]; owner != 0 && owner - 1 < blas_.size() && blas_[owner - 1].postbuildSlot == slot)
					blas_[owner - 1].postbuildSlot = UINT32_MAX;
				postbuildOwner_[slot] = static_cast<std::uint64_t>(entry.handle) + 1;
			}
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC postbuild{};
			postbuild.InfoType = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE;
			postbuild.DestBuffer = postbuild_->GetGPUVirtualAddress() + sizeof(std::uint64_t) * slot;

			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
			buildDesc.Inputs = inputs;
			buildDesc.DestAccelerationStructureData = asPool_.Address(entry.result);
			buildDesc.ScratchAccelerationStructureData = scratch_->GetGPUVirtualAddress() + (parallel ? scratchOffset : 0);
			scratchOffset += Align(entry.info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			a_list->BuildRaytracingAccelerationStructure(&buildDesc, 1, &postbuild);

			const std::uint64_t bytes = entry.result.size;
			// (46p) This submission's prep work.
			++recWork_.builds;
			recWork_.buildsAlpha += geometry.Opaque() ? 0u : 1u;
			recWork_.buildTriangles += blas.triangles;
			recWork_.buildBytes += bytes;
			blas.geometry.reset();
			blas.alloc = entry.result;
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
		const std::uint64_t aoCompleted = AOCompleted();  // (46f)
		// (45n) Early Prep: the builds and skinning of this frame are already on the GPU, recorded on the
		// context PrepareFrame kept; only the TLAS and the trace are recorded here.
		const bool early = prepared_;
		split.early = early;
		split.retire = stats_.retireLastMs;
		split.retiredCount = stats_.retiredLastCollect;
		// While this frame records (and, with Early Prep, since PrepareFrame) retired objects wait for the
		// trace's done value. Back to the ready value once the frame is submitted or dropped.
		struct AheadReset
		{
			Backend* backend;
			~AheadReset()
			{
				if (!backend->prepared_)
					backend->retireAhead_ = 1;
			}
		} aheadReset{ this };
		retireAhead_ = 2;

		// A free command context, without waiting on the CPU.
		std::size_t index = kContexts;
		if (early) {
			index = preparedContext_;
		} else {
			for (std::size_t offset = 0; offset < kContexts; ++offset) {
				const std::size_t candidate = (cursor_ + offset) % kContexts;
				if (ContextFree(contexts_[candidate], completed, aoCompleted)) {
					index = candidate;
					break;
				}
			}
		}
		lap(split.collect);
		for (std::size_t i = 0; i < kContexts; ++i)
			if (contexts_[i].fenceValue != 0 && contexts_[i].fenceValue <= completed && contexts_[i].aoFenceValue <= aoCompleted)
				CollectTimestamps(i);
		lap(split.timestamps);  // (46g)
		if (index == kContexts) {
			++stats_.skippedFramesBusy;
			return false;
		}
		auto& context = contexts_[index];
		if (!early)
			cursor_ = (index + 1) % kContexts;
		lap(split.collect);

		static const std::vector<Instance> kNoInstances;
		const std::vector<Instance>& instances = a_input.instances ? *a_input.instances : kNoInstances;

		// (45l) Recording first; the D3D11 ready Signal and the D3D12 queue Wait come after it. The
		// builds, skinning and the TLAS go out at once (they read nothing D3D11 writes)
		// and only the trace list waits for D3D11's depth copy, so the D3D11 queue later waits for the
		// trace alone instead of everything (GPU-side ordering only; the CPU never waits).
		// (45n) Early Prep: the allocator holds the prep list (in flight): not reset; the list is reset
		// for the TLAS alone (a list may be reset once it was submitted).
		HRESULT hr;
		if (!early && FAILED(hr = context.allocator->Reset()))
			return Fail("command allocator Reset", hr);
		auto* list = context.list.Get();
		if (FAILED(hr = list->Reset(context.allocator.Get(), nullptr)))
			return Fail("command list Reset", hr);
		lap(split.reset);
		const UINT queryBase = static_cast<UINT>(index * 4);

		// (45i) Skinned characters: uploads, skinning, BLAS builds / refits (none without draws).
		static const std::vector<SkinDraw> kNoSkinDraws;
		const std::vector<SkinDraw>& skinDraws = a_input.skinDraws && SkinAvailable() ? *a_input.skinDraws : kNoSkinDraws;
		if (early) {
			// skinReady_ is PrepareFrame's, for the same draws.
			if (skinReady_.size() != skinDraws.size()) {
				list->Close();
				return false;
			}
		} else {
			if (timestampHeap_)
				list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);

			const std::int64_t buildStart = QpcNow();
			const std::uint32_t maxBuilds = a_input.maxBuilds ? a_input.maxBuilds : static_cast<std::uint32_t>(kMaxBuildsPerFrame);
			const std::int64_t buildDeadline = buildStart + MsToQpc(a_input.buildCpuMs > 0.0f ? a_input.buildCpuMs : kPlayBuildCpuMs);  // (46g)
			recWork_ = {};  // (46p)
			ProcessCompaction(list, completed, a_input.maxBuilds ? a_input.maxBuilds : kMaxCompactionsPerFrame, buildDeadline);
			const std::uint32_t builtNow = BuildQueued(list, a_input.buildTriangleBudget, maxBuilds, buildDeadline);
			stats_.buildsLastFrame = builtNow;
			const std::int64_t buildEnd = QpcNow();

			skinReady_.assign(skinDraws.size(), 0);
			skinRecorded_ = false;
			skinGroupFrame_.clear();  // (46m)
			if (!skinDraws.empty() || !skinUploadQueue_.empty()) {
				const std::uint64_t uploadsBefore = skinUploadsTotal_;
				RecordSkinning(list, context, a_input, skinReady_);
				NoteSkinWork(uploadsBefore);  // (46p)
			} else if (!skinGroups_.empty())
				ReleaseSkinGroups();  // (46m) no character drawn
			const std::int64_t skinEnd = QpcNow();
			stats_.cpuBuildMs = static_cast<float>(QpcToMs(buildEnd - buildStart));
			stats_.cpuSkinMs = static_cast<float>(QpcToMs(skinEnd - buildEnd));
			if (failed_) {
				list->Close();
				return false;
			}
		}
		mark = QpcNow();

		// TLAS over every built BLAS that has an instance this frame.
		std::uint32_t instanceCount = 0;
		const auto maxInstances = static_cast<std::uint32_t>(instances.size() + skinDraws.size());
		if (!EnsureTlas(maxInstances) || !EnsureInstanceBuffer(context, maxInstances)) {
			list->Close();
			return false;
		}
		lap(split.ensure);  // (46g)
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
			if ((blas.state != BlasState::Built && blas.state != BlasState::Ready) || !blas.alloc.Valid()) {
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
			desc.Flags = instance.twoSided ? kInstanceFlags : D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;  // (46g)
			if (alpha) {
				// The shader finds the record through the instance ID (0 = opaque); no force-opaque flag.
				desc.InstanceID = instance.alphaInfo + 1;
				desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
				++alphaInstances;
			}
			desc.AccelerationStructure = asPool_.Address(blas.alloc);
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
		std::uint32_t charactersTraced = 0;  // (46m) shapes (a merged BLAS counts its members)
		for (std::size_t d = 0; d < skinDraws.size(); ++d) {
			if (!skinReady_[d] || !charactersTraceable) {
				++check_.notBuilt[kCategoryCharacter];
				continue;
			}
			const auto& draw = skinDraws[d];
			if (draw.group != 0)
				continue;  // (46m) in its character's merged BLAS (below)
			// (45n) With Early Prep a shape may have been released since it was skinned (switch change).
			if (draw.shape >= skins_.size() || !skins_[draw.shape].live || !skins_[draw.shape].blas.Valid()) {
				++check_.notBuilt[kCategoryCharacter];
				continue;
			}
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
			// (46e) Inline ray queries have no hit groups: the field carries the character's key (TracePointLightsCS).
			desc.InstanceContributionToHitGroupIndex = draw.owner & 0xFFFFFFu;
			desc.Flags = kInstanceFlags;
			if (alpha) {
				desc.InstanceID = draw.alphaInfo + 1;
				desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
				++alphaInstances;
			}
			desc.AccelerationStructure = asPool_.Address(skin.blas);
			++check_.written[kCategoryCharacter];
			descs[instanceCount++] = desc;
			++characterInstances;
			++charactersTraced;
		}
		// (46m) Merged BLASes: one opaque instance per character (its opaque shapes), same mask / key / origin rules.
		for (const auto& group : skinGroupFrame_) {
			if (!charactersTraceable || group.blas == 0) {
				check_.notBuilt[kCategoryCharacter] += group.members;
				continue;
			}
			D3D12_RAYTRACING_INSTANCE_DESC desc{};
			desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1.0f;
			desc.Transform[0][3] = static_cast<float>(group.x - a_input.cameraX);
			desc.Transform[1][3] = static_cast<float>(group.y - a_input.cameraY);
			desc.Transform[2][3] = static_cast<float>(group.z - a_input.cameraZ);
			desc.InstanceMask = group.npc ? kNpcMask : kCharacterMask;
			desc.InstanceContributionToHitGroupIndex = group.owner & 0xFFFFFFu;
			desc.Flags = kInstanceFlags;
			desc.AccelerationStructure = group.blas;
			check_.written[kCategoryCharacter] += group.members;
			descs[instanceCount++] = desc;
			++characterInstances;
			charactersTraced += group.members;
		}
		skinStats_.drawsLastFrame = charactersTraced;
		skinStats_.instancesLastFrame = characterInstances;

		stats_.instancesLastFrame = instanceCount;
		recWork_.tlasInstances = instanceCount;  // (46p)
		lap(split.tlas);
		// (45h) The alpha pipeline only on frames that hold alpha-tested instances (and their records
		// are in this context's copy); every other frame runs exactly the opaque pipeline.
		bool alphaFrame = alphaInstances > 0;
		if (alphaFrame && !EnsureAlphaInfoBuffer(context)) {
			// (45i) Character uploads / builds recorded in this list must run (later refits rely on
			// them): a list that cannot be submitted takes the whole feature down instead.
			if (skinRecorded_ && !early)
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
		// (45n) Early Prep: prep begin / end were written by PrepareFrame (the TLAS list is not timed).
		if (timestampHeap_ && !early)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);

		// The trace: in its own list behind the wait.
		if (FAILED(hr = list->Close()))
			return Fail("command list Close", hr);
		lap(split.record);
		{
			// (46g) This first ExecuteCommandLists (builds / TLAS) counts as execute too (it was inside record before).
			ID3D12CommandList* prepLists[] = { list };
			queue_->ExecuteCommandLists(1, prepLists);
		}
		lap(split.execute);
		ID3D12GraphicsCommandList4* traceList = context.traceList.Get();
		if (FAILED(hr = traceList->Reset(context.allocator.Get(), nullptr)))
			return Fail("trace command list Reset", hr);
		if (timestampHeap_)
			traceList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 2);

		// (46b) Point lights this frame: textures, counter and this context's copy of the sources.
		const bool pointFrame = a_input.pointLights && a_input.pointLights->scan > 0 && a_input.pointLights->sources && PointAvailable() && pointVis_.resource12 &&
		                        pointIdx_.resource12 && pointCounter_ && pointCounterMapped_ && EnsurePointBuffer(context, a_input.pointLights->sourceBytes);
		if (pointFrame)
			std::memcpy(context.pointMapped, a_input.pointLights->sources, a_input.pointLights->sourceBytes);
		stats_.pointTracedLastFrame = pointFrame;
		// (46m) Split Trace Sync: the sun alone in this list (the sun fence is signalled behind it), the point lights in
		// their own list after it; the D3D11 queue can start on the sun's result while the point lights still run.
		const bool splitSun = a_input.splitSun && a_input.sun && pointFrame && context.pointList && EnsureSunFence();
		context.splitTrace = splitSun;
		stats_.sunSplitLastFrame = splitSun;
		const bool pointsInTraceList = pointFrame && !splitSun;

		D3D12_RESOURCE_BARRIER pre[4] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
			Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			Transition(pointVis_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			Transition(pointIdx_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
		};
		traceList->ResourceBarrier(pointsInTraceList ? 4 : 2, pre);

		lap(split.record);
		ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
		traceList->SetDescriptorHeaps(1, heaps);
		// (46f) This context's copy of the run-time descriptors (sun, point and RTAO tables).
		const D3D12_GPU_DESCRIPTOR_HANDLE block = PublishDescriptors(index);
		lap(split.publish);  // (46g)
		if (a_input.sun) {
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
			traceList->SetComputeRootDescriptorTable(2, Offset(block, kDynSun, descriptorSize_));
			if (alphaFrame) {
				traceList->SetComputeRootShaderResourceView(3, context.alphaInfoBuffer->GetGPUVirtualAddress());
				traceList->SetComputeRootShaderResourceView(4, alphaUv_->GetGPUVirtualAddress());
				auto table = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
				table.ptr += static_cast<UINT64>(descriptorSize_) * (kAlphaDescriptorBase - 1);
				traceList->SetComputeRootDescriptorTable(5, table);
			}
			traceList->Dispatch((a_input.renderWidth + 7) / 8, (a_input.renderHeight + 7) / 8, 1);
		}
		if (pointsInTraceList) {
			// (46b) The point lights read the sun's result (sunlit pixels skip weak point lights).
			// (46c) Without a sun trace (interiors) the texture is only transitioned; the shader does not read it (no sun gate).
			D3D12_RESOURCE_BARRIER toRead = Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			traceList->ResourceBarrier(1, &toRead);
			RecordPointTrace(traceList, context, index, a_input, alphaFrame, Offset(block, kDynPoint, descriptorSize_));
		} else if (!pointFrame) {
			pointAlphaLastFrame_ = false;
		}

		D3D12_RESOURCE_BARRIER post[4] = {
			Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
			Transition(visibility_.resource12.Get(), pointsInTraceList ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
				D3D12_RESOURCE_STATE_COMMON),
			Transition(pointVis_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
			Transition(pointIdx_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
		};
		traceList->ResourceBarrier(pointsInTraceList ? 4 : 2, post);

		if (timestampHeap_ && timestampReadback_) {
			// (46m) Split: this interval ends with the sun (the point lights keep their own pair).
			traceList->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 3);
			traceList->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase, 4, timestampReadback_.Get(),
				sizeof(std::uint64_t) * 4 * index);
			context.timestampsPending = true;
			// (46p) The prep work these timestamps measure, with the pool blocks adopted / released since the last one.
			const std::uint64_t blocks = stats_.pools.asyncBlocksTotal + stats_.pools.syncBlocksTotal;
			recWork_.poolBlocks = static_cast<std::uint32_t>(blocks - std::min(blocks, workBlocksSeen_));
			recWork_.poolReleased = static_cast<std::uint32_t>(stats_.pools.blocksReleased - std::min(stats_.pools.blocksReleased, workReleasedSeen_));
			workBlocksSeen_ = blocks;
			workReleasedSeen_ = stats_.pools.blocksReleased;
			recWork_.early = early ? 1 : 0;
			recWork_.valid = 1;
			recWork_.submission = ++submissions_;
			context.work = recWork_;
		}
		if (FAILED(hr = traceList->Close()))
			return Fail("command list Close", hr);
		if (splitSun) {
			// (46m) The point lights' list: the sun's result (the sun gate) and the depth are read-only on both queues now.
			auto* pointList = context.pointList.Get();
			if (FAILED(hr = pointList->Reset(context.allocator.Get(), nullptr)))
				return Fail("point-light command list Reset", hr);
			pointList->SetDescriptorHeaps(1, heaps);
			D3D12_RESOURCE_BARRIER pointPre[4] = {
				Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
				Transition(pointVis_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
				Transition(pointIdx_.resource12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
			};
			pointList->ResourceBarrier(4, pointPre);
			RecordPointTrace(pointList, context, index, a_input, alphaFrame, Offset(block, kDynPoint, descriptorSize_));
			D3D12_RESOURCE_BARRIER pointPost[4] = {
				Transition(depthMask_.resource12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				Transition(visibility_.resource12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
				Transition(pointVis_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
				Transition(pointIdx_.resource12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
			};
			pointList->ResourceBarrier(4, pointPost);
			if (FAILED(hr = pointList->Close()))
				return Fail("point-light command list Close", hr);
		}
		lap(split.record);
		// (46f) RTAO: its own list on this context (recorded now, submitted right behind the trace).
		const bool aoFrame = a_input.ao && RecordAOTrace(context, index, a_input, alphaFrame, Offset(block, kDynAO, descriptorSize_));
		if (failed_)
			return false;
		stats_.aoTracedLastFrame = aoFrame;
		lap(split.ao);  // (46g)

		// (46f) A second-fence wait never issued last frame (the AO was not used): the D3D11 queue takes it before this
		// ready signal, so every value of the shared fence still orders after all earlier D3D12 work (Retire relies on it).
		if (pendingAOWait_ != 0) {
			const std::uint64_t aoValue = pendingAOWait_;
			pendingAOWait_ = 0;
			if (FAILED(hr = context11_->Wait(aoFence11_.Get(), aoValue)))
				return Fail("D3D11 Wait (RTAO)", hr);
		}
		// (46h) Likewise a trace wait never issued (an interior frame with RTAO only leaves it to the AO pass; the callers take it
		// before the next trace, this is the guard): the D3D11 ready signal below must follow the last D3D12 done value.
		if (pendingWait_ != 0) {
			const std::uint64_t traceValue = pendingWait_;
			pendingWait_ = 0;
			if (FAILED(hr = context11_->Wait(fence11_.Get(), traceValue)))
				return Fail("D3D11 Wait", hr);
		}

		// GPU-side ordering only: the trace waits for everything D3D11 recorded so far (the depth /
		// mask copy, the alpha copies); the D3D11 queue later waits for the trace.
		const std::uint64_t readyValue = ++fenceValue_;
		if (FAILED(hr = context11_->Signal(fence11_.Get(), readyValue)))
			return Fail("D3D11 Signal", hr);
		lap(split.signal11);
		// (45k) The Signal submits the D3D11 work recorded so far by itself (no explicit Flush).
		split.flushSkipped = true;
		lap(split.flush11);
		if (FAILED(hr = queue_->Wait(fence_.Get(), readyValue)))
			return Fail("D3D12 queue Wait", hr);
		lap(split.wait12);
		ID3D12CommandList* lists[] = { traceList };
		queue_->ExecuteCommandLists(1, lists);
		pendingSunWait_ = 0;
		if (splitSun) {
			// (46m) The sun fence between the sun and the point lights (only our queue signals it, in increasing order).
			const std::uint64_t sunValue = ++sunFenceValue_;
			if (FAILED(hr = queue_->Signal(sunFence_.Get(), sunValue)))
				return Fail("D3D12 queue Signal (sun)", hr);
			ID3D12CommandList* pointLists[] = { context.pointList.Get() };
			queue_->ExecuteCommandLists(1, pointLists);
			pendingSunWait_ = sunValue;
		}
		lap(split.execute);
		const std::uint64_t doneValue = ++fenceValue_;
		if (FAILED(hr = queue_->Signal(fence_.Get(), doneValue)))
			return Fail("D3D12 queue Signal", hr);
		lap(split.signal12);
		context.fenceValue = doneValue;
		lastSubmitted_ = doneValue;
		prepared_ = false;  // (45n) the kept context now has its done value
		// (46f) RTAO right behind the trace, signalling the second fence (the D3D11 queue waits for it only right
		// before the AO is used: WaitForAO). Not part of doneValue: rt_sync waits for the sun / point lights alone.
		if (aoFrame) {
			ID3D12CommandList* aoLists[] = { context.aoList.Get() };
			queue_->ExecuteCommandLists(1, aoLists);
			const std::uint64_t aoValue = ++aoFenceValue_;
			if (FAILED(hr = queue_->Signal(aoFence_.Get(), aoValue)))
				return Fail("D3D12 queue Signal (RTAO)", hr);
			context.aoFenceValue = aoValue;
			context.aoPending = true;
			context.aoFrame = ++aoFrame_;
			pendingAOWait_ = aoValue;
		}
		for (auto& blas : blas_)
			if (blas.state == BlasState::Built && blas.buildFence == 0)
				blas.buildFence = doneValue;
		if (a_input.deferWait) {
			pendingWait_ = doneValue;
		} else {
			pendingWait_ = 0;
			pendingSunWait_ = 0;
			if (FAILED(hr = context11_->Wait(fence11_.Get(), doneValue)))
				return Fail("D3D11 Wait", hr);
		}
		lap(split.wait11);
		stats_.split = split;
		stats_.cpuRecordMs = static_cast<float>(QpcToMs(QpcNow() - cpuStart)) - (early ? 0.0f : stats_.cpuBuildMs + stats_.cpuSkinMs);
		return true;
	}

	bool Backend::PrepareFrame(const FrameInput& a_input)
	{
		// (45n) Early Prep. Same work and order as the first half of ExecuteFrame (split submission), only
		// earlier in the frame: the D3D12 queue gets the builds and skinning while the game's queue still
		// draws the shadow maps, so by the time the depth copy is ready only the TLAS and the trace remain.
		// The context stays reserved (fenceValue UINT64_MAX: never "free") until ExecuteFrame or
		// AbandonPrepared gives it a real done value. No fence value is signalled here: the shared fence is
		// only ever signalled in increasing order (D3D11 ready, then D3D12 done behind a wait for it).
		// (46g) Every step is timed on its own (stats_.prep, F7 rt_e_*).
		if (!ready_ || failed_ || prepared_)
			return false;
		const HRESULT removed = device_->GetDeviceRemovedReason();
		if (FAILED(removed))
			return Fail("D3D12 device", removed);
		const std::int64_t cpuStart = QpcNow();
		PrepSplit split{};
		std::int64_t mark = cpuStart;
		auto lap = [&](float& a_field) {
			const std::int64_t now = QpcNow();
			a_field = std::max(a_field, 0.0f) + static_cast<float>(QpcToMs(now - mark));
			mark = now;
		};
		struct SplitStore
		{
			Backend* backend;
			PrepSplit* split;
			~SplitStore() { backend->stats_.prep = *split; }
		} splitStore{ this, &split };
		CollectRetired();
		const std::uint64_t completed = fence_->GetCompletedValue();
		const std::uint64_t aoCompleted = AOCompleted();  // (46f)
		std::size_t index = kContexts;
		for (std::size_t offset = 0; offset < kContexts; ++offset) {
			const std::size_t candidate = (cursor_ + offset) % kContexts;
			if (ContextFree(contexts_[candidate], completed, aoCompleted)) {
				index = candidate;
				break;
			}
		}
		lap(split.collect);
		for (std::size_t i = 0; i < kContexts; ++i)
			if (contexts_[i].fenceValue != 0 && contexts_[i].fenceValue <= completed && contexts_[i].aoFenceValue <= aoCompleted)
				CollectTimestamps(i);
		lap(split.timestamps);
		if (index == kContexts)
			return false;
		auto& context = contexts_[index];
		HRESULT hr;
		if (FAILED(hr = context.allocator->Reset()))
			return Fail("command allocator Reset", hr);
		auto* list = context.list.Get();
		if (FAILED(hr = list->Reset(context.allocator.Get(), nullptr)))
			return Fail("command list Reset", hr);
		cursor_ = (index + 1) % kContexts;
		context.fenceValue = UINT64_MAX;
		context.timestampsPending = false;
		prepared_ = true;
		preparedContext_ = index;
		retireAhead_ = 2;  // everything retired from now until the trace waits for its done value
		lap(split.reset);

		const UINT queryBase = static_cast<UINT>(index * 4);
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);
		const std::int64_t buildStart = QpcNow();
		const std::uint32_t maxBuilds = a_input.maxBuilds ? a_input.maxBuilds : static_cast<std::uint32_t>(kMaxBuildsPerFrame);
		const std::int64_t buildDeadline = buildStart + MsToQpc(a_input.buildCpuMs > 0.0f ? a_input.buildCpuMs : kPlayBuildCpuMs);
		recWork_ = {};  // (46p)
		ProcessCompaction(list, completed, a_input.maxBuilds ? a_input.maxBuilds : kMaxCompactionsPerFrame, buildDeadline);
		stats_.buildsLastFrame = BuildQueued(list, a_input.buildTriangleBudget, maxBuilds, buildDeadline);
		const std::int64_t buildEnd = QpcNow();
		mark = buildEnd;
		split.build = static_cast<float>(QpcToMs(buildEnd - buildStart));
		static const std::vector<SkinDraw> kNoSkinDraws;
		const std::vector<SkinDraw>& skinDraws = a_input.skinDraws && SkinAvailable() ? *a_input.skinDraws : kNoSkinDraws;
		skinReady_.assign(skinDraws.size(), 0);
		skinRecorded_ = false;
		skinGroupFrame_.clear();  // (46m)
		if (!skinDraws.empty() || !skinUploadQueue_.empty()) {
			const std::uint64_t uploadsBefore = skinUploadsTotal_;
			RecordSkinning(list, context, a_input, skinReady_);
			NoteSkinWork(uploadsBefore);  // (46p)
		} else if (!skinGroups_.empty())
			ReleaseSkinGroups();  // (46m) no character drawn
		const std::int64_t skinEnd = QpcNow();
		split.skin = static_cast<float>(QpcToMs(skinEnd - buildEnd));
		mark = skinEnd;
		stats_.cpuBuildMs = split.build;
		stats_.cpuSkinMs = split.skin;
		if (failed_) {
			list->Close();
			return false;  // the feature turns itself off (Shutdown drops the reservation)
		}
		{
			// Builds and compaction copies are complete before anything later on the queue reads them.
			auto barrier = UavBarrier();
			list->ResourceBarrier(1, &barrier);
		}
		if (timestampHeap_)
			list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);
		if (FAILED(hr = list->Close()))
			return Fail("command list Close", hr);
		lap(split.close);
		ID3D12CommandList* lists[] = { list };
		queue_->ExecuteCommandLists(1, lists);
		lap(split.execute);
		stats_.earlyCpuMs = static_cast<float>(QpcToMs(QpcNow() - cpuStart));
		return true;
	}

	void Backend::AbandonPrepared()
	{
		if (!prepared_)
			return;
		prepared_ = false;
		retireAhead_ = 1;
		if (!ready_ || failed_ || !queue_ || !fence_)
			return;
		// Two values at once: everything retired since PrepareFrame waits for "+2" (RetireValue). Only the
		// D3D12 queue signals here, behind the prep list; no D3D11 signal with a lower value is outstanding
		// (every earlier ready value was waited for by an earlier trace before its done value).
		fenceValue_ += 2;
		const HRESULT hr = queue_->Signal(fence_.Get(), fenceValue_);
		if (FAILED(hr)) {
			Fail("D3D12 queue Signal (abandoned frame)", hr);
			return;
		}
		contexts_[preparedContext_].fenceValue = fenceValue_;
		lastSubmitted_ = fenceValue_;
		for (auto& blas : blas_)
			if (blas.state == BlasState::Built && blas.buildFence == 0)
				blas.buildFence = fenceValue_;
	}

	float Backend::WaitForTrace()
	{
		// (45l) The deferred D3D11 wait (GPU-side): right before the first D3D11 pass that reads the trace.
		// (46m) The done value follows the sun fence's value on our queue: a sun wait not taken is covered.
		pendingSunWait_ = 0;
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

	float Backend::WaitForSun()
	{
		// (46m) Split Trace Sync: the D3D11 queue waits (GPU-side) for the sun list alone; the point lights may still run.
		if (pendingSunWait_ == 0 || !context11_ || !sunFence11_)
			return 0.0f;
		const std::int64_t start = QpcNow();
		const std::uint64_t value = pendingSunWait_;
		pendingSunWait_ = 0;
		if (FAILED(context11_->Wait(sunFence11_.Get(), value)))
			Fail("D3D11 Wait (sun)", E_FAIL);
		const float ms = static_cast<float>(QpcToMs(QpcNow() - start));
		stats_.split.wait11 += ms;
		return ms;
	}

	bool Backend::EnsureSunFence()
	{
		// (46m) Shared like the others: the D3D12 queue signals it after the sun list, the D3D11 queue waits for it on the GPU.
		if (sunFence_ && sunFence11_)
			return true;
		if (sunFenceFailed_ || !device_ || !device11_)
			return false;
		sunFence_.Reset();
		sunFence11_.Reset();
		HRESULT hr;
		if (FAILED(hr = device_->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&sunFence_)))) {
			sunFence_.Reset();
			sunFenceFailed_ = true;
			logger::warn("[RayTracing] Split Trace Sync unavailable: could not create the sun fence ({})", HrString(hr));
			return false;
		}
		HANDLE shared = nullptr;
		if (FAILED(hr = device_->CreateSharedHandle(sunFence_.Get(), nullptr, GENERIC_ALL, nullptr, &shared))) {
			sunFence_.Reset();
			sunFenceFailed_ = true;
			logger::warn("[RayTracing] Split Trace Sync unavailable: could not share the sun fence ({})", HrString(hr));
			return false;
		}
		hr = device11_->OpenSharedFence(shared, IID_PPV_ARGS(&sunFence11_));
		CloseHandle(shared);
		if (FAILED(hr)) {
			sunFence_.Reset();
			sunFence11_.Reset();
			sunFenceFailed_ = true;
			logger::warn("[RayTracing] Split Trace Sync unavailable: D3D11 could not open the sun fence ({})", HrString(hr));
			return false;
		}
		sunFenceValue_ = 0;
		pendingSunWait_ = 0;
		return true;
	}

	bool Backend::BuildOnly(std::uint32_t a_triangleBudget, std::uint32_t a_maxBuilds, float a_cpuMs)
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
		const std::uint64_t aoCompleted = AOCompleted();  // (46f)
		bool compactionDue = false;
		for (const auto& blas : blas_) {
			if (blas.state == BlasState::Built && blas.buildFence != 0 && blas.buildFence <= completed) {
				compactionDue = true;
				break;
			}
		}
		// (46g) Character shapes created behind a loading / black screen are uploaded here too (no draws needed).
		const bool uploadsDue = !skinUploadQueue_.empty() && SkinAvailable();
		if (buildQueue_.empty() && !compactionDue && !uploadsDue)
			return false;
		std::size_t index = kContexts;
		for (std::size_t offset = 0; offset < kContexts; ++offset) {
			const std::size_t candidate = (cursor_ + offset) % kContexts;
			if (ContextFree(contexts_[candidate], completed, aoCompleted)) {
				index = candidate;
				break;
			}
		}
		if (index == kContexts)
			return false;
		for (std::size_t i = 0; i < kContexts; ++i)
			if (contexts_[i].fenceValue != 0 && contexts_[i].fenceValue <= completed && contexts_[i].aoFenceValue <= aoCompleted)
				CollectTimestamps(i);
		auto& context = contexts_[index];
		cursor_ = (index + 1) % kContexts;
		HRESULT hr;
		if (FAILED(hr = context.allocator->Reset()))
			return Fail("command allocator Reset", hr);
		auto* list = context.list.Get();
		if (FAILED(hr = list->Reset(context.allocator.Get(), nullptr)))
			return Fail("command list Reset", hr);
		const std::int64_t buildStart = QpcNow();
		const std::int64_t deadline = buildStart + MsToQpc(a_cpuMs);
		ProcessCompaction(list, completed, a_maxBuilds, deadline);
		const std::uint32_t built = BuildQueued(list, a_triangleBudget, a_maxBuilds, deadline);
		stats_.loadingBuilds += built;
		const std::int64_t buildEnd = QpcNow();
		stats_.cpuBuildMs = static_cast<float>(QpcToMs(buildEnd - buildStart));
		if (uploadsDue) {
			UploadSkinShapes(list, std::max(a_cpuMs * 0.5f, 1.0f));
			// Builds / copies done before anything later on the queue reads them.
			auto barrier = UavBarrier();
			list->ResourceBarrier(1, &barrier);
		}
		stats_.cpuSkinMs = static_cast<float>(QpcToMs(QpcNow() - buildEnd));
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
