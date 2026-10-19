// (47l) Foliage sway on the ray tracer's D3D12 side.
//
// A wind mesh is a tree-animated mesh (leaves, bushes, swaying branches) with the shapes that share its engine mesh and its
// leaf amplitude / frequency. It is uploaded once (a few per frame): its rest vertices (position, packed normal and vertex
// alpha), its indices, a vertex buffer the posing writes and a BLAS range sized for ALLOW_UPDATE builds. Its first build poses
// it at the mean of the sway (WindFoliageCS mode 0) so every later in-place refit stays within half the sway of the BVH it
// was built for (no periodic rebuild: the sway never drifts). Every frame the wind pose changes, WindFoliageCS poses it with
// this frame's wind (the game's own formula) and its BLAS is refit in place (PERFORM_UPDATE). First builds are capped per
// frame; refits are not. Its ranges come from the character pools (rest + index: skinStaticPool_, posed vertices:
// skinOutputPool_, BLAS: asPool_) and go back behind the fence (RetireValue) like every other range. The CPU never waits.

#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <bit>

// Generated at build time from Shaders/WindFoliageCS.hlsl by the Windows SDK's dxc (CMakeLists.txt).
#include "RayTracing/WindFoliageCS.dxil.h"

namespace RayTracingRT
{
	namespace
	{
		std::string WindHr(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		std::uint64_t WindAlign(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) & ~(a_alignment - 1); }

		D3D12_RESOURCE_BARRIER WindTransition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			return barrier;
		}

		D3D12_RESOURCE_BARRIER WindUavBarrier()
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = nullptr;  // every UAV / acceleration-structure write before it
			return barrier;
		}

		std::int64_t WindQpc()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		std::int64_t WindMsToQpc(double a_ms)
		{
			static const double perMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart) / 1000.0;
			}();
			return static_cast<std::int64_t>(a_ms * perMs);
		}

		constexpr std::uint32_t kWindVertexBytes = sizeof(WindVertex);
		constexpr std::uint32_t kWindOutputStride = sizeof(float) * 3;
		constexpr std::uint32_t kWindGroupSize = 64;  // WindFoliageCS numthreads
		constexpr std::uint32_t kWindConstants = 8;   // WindFoliageCS WindConstants
		// A wind pose whose phase moved by less than this (in cycles of the faster wave) since the last refit is not refit again:
		// the vertices would move by less than a ten-thousandth of the sway (a paused game, no wind).
		constexpr double kWindStillCycles = 1.0e-5;

		D3D12_RAYTRACING_GEOMETRY_DESC WindGeometryDesc(bool a_opaque, std::uint32_t a_vertices, std::uint32_t a_triangles)
		{
			D3D12_RAYTRACING_GEOMETRY_DESC desc{};
			desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			// An alpha-tested mesh is non-opaque (the trace tests its cut-out through the shape's record, as for the frozen mesh).
			desc.Flags = a_opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			desc.Triangles.VertexBuffer.StrideInBytes = kWindOutputStride;
			desc.Triangles.VertexCount = a_vertices;
			desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			desc.Triangles.IndexCount = a_triangles * 3;
			desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			return desc;
		}

		constexpr D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS kWindBuildFlags =
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
	}

	bool Backend::CreateWindPipeline()
	{
		D3D12_ROOT_PARAMETER params[3]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = kWindConstants;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[2].Descriptor.ShaderRegister = 0;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 3;
		rootDesc.pParameters = params;
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (FAILED(hr) || FAILED(hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&windRootSignature_)))) {
			windRootSignature_.Reset();
			logger::warn("[RayTracing] Foliage sway unavailable: the wind root signature failed ({})", WindHr(hr));
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = windRootSignature_.Get();
		psoDesc.CS.pShaderBytecode = g_WindFoliageCS;
		psoDesc.CS.BytecodeLength = sizeof(g_WindFoliageCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&windPipeline_)))) {
			windPipeline_.Reset();
			windRootSignature_.Reset();
			logger::warn("[RayTracing] Foliage sway unavailable: the wind shader was rejected by the driver ({})", WindHr(hr));
			return false;
		}
		return true;
	}

	void Backend::SetWindFault(std::string a_reason)
	{
		if (!windFault_.empty())
			return;
		windFault_ = std::move(a_reason);
		logger::error("[RayTracing] Foliage sway: {} -- switched off", windFault_);
	}

	BlasHandle Backend::CreateWindBlas(std::unique_ptr<WindGeometry> a_geometry)
	{
		if (!WindAvailable() || !a_geometry) {
			++windStats_.refused;
			return kInvalidBlas;
		}
		const WindGeometry& geometry = *a_geometry;
		const std::uint32_t vertices = geometry.Vertices();
		const std::uint32_t triangles = geometry.Triangles();
		auto refuse = [&]() {
			++windStats_.refused;
			return kInvalidBlas;
		};
		if (vertices == 0 || triangles == 0 || vertices > kWindMaxVertices || triangles > kWindMaxTriangles || geometry.indices.size() % 3 != 0 ||
			!std::isfinite(geometry.amplitude) || !std::isfinite(geometry.frequency) || geometry.amplitude <= 0.0f || geometry.amplitude > 10000.0f ||
			std::abs(geometry.frequency) > 10000.0f)
			return refuse();
		for (const auto index : geometry.indices)
			if (index >= vertices)
				return refuse();
		for (const auto& vertex : geometry.vertices)
			if (!std::isfinite(vertex.position[0]) || !std::isfinite(vertex.position[1]) || !std::isfinite(vertex.position[2]))
				return refuse();

		WindMesh mesh;
		mesh.live = true;
		mesh.vertices = vertices;
		mesh.triangles = triangles;
		mesh.amplitude = geometry.amplitude;
		mesh.frequency = geometry.frequency;
		mesh.opaque = geometry.opaque;
		mesh.restBytes = WindAlign(static_cast<std::uint64_t>(vertices) * kWindVertexBytes, 256);
		mesh.indexBytes = WindAlign(static_cast<std::uint64_t>(triangles) * 3 * sizeof(std::uint32_t), 256);
		mesh.outputBytes = WindAlign(static_cast<std::uint64_t>(vertices) * kWindOutputStride, 256);
		{
			const D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = WindGeometryDesc(mesh.opaque, vertices, triangles);
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = kWindBuildFlags;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &geometryDesc;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
				return refuse();
			mesh.blasBytes = WindAlign(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			mesh.buildScratch = WindAlign(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			mesh.updateScratch = WindAlign(std::max<std::uint64_t>(info.UpdateScratchDataSizeInBytes, 256), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		}
		if (windStats_.bytes + mesh.Bytes() > kWindBudgetBytes) {
			++windStats_.overBudget;
			return kInvalidBlas;
		}

		std::uint32_t index;
		if (!freeWinds_.empty()) {
			index = freeWinds_.back();
			freeWinds_.pop_back();
		} else {
			index = static_cast<std::uint32_t>(winds_.size());
			winds_.emplace_back();
		}
		const BlasHandle handle = AllocateHandle();
		auto& blas = blas_[handle];
		blas = {};
		blas.state = BlasState::Queued;  // never in buildQueue_: RecordWind builds it
		blas.triangles = triangles;
		blas.wind = index;
		// (45f) The TLAS self-check box: every pose of the sway (the offset runs from 0 to 1.1 amplitude x alpha along +normal).
		for (int a = 0; a < 3; ++a) {
			blas.boundsMin[a] = FLT_MAX;
			blas.boundsMax[a] = -FLT_MAX;
		}
		for (const auto i : geometry.indices) {
			const auto& vertex = geometry.vertices[i];
			const float reach = 1.1f * geometry.amplitude * static_cast<float>(vertex.normalAlpha >> 24) / 255.0f;
			for (int a = 0; a < 3; ++a) {
				const float n = static_cast<float>((vertex.normalAlpha >> (8 * a)) & 0xFFu) * (2.0f / 255.0f) - 1.0f;
				const float p = vertex.position[a];
				const float q = p + n * reach;
				blas.boundsMin[a] = std::min({ blas.boundsMin[a], p, q });
				blas.boundsMax[a] = std::max({ blas.boundsMax[a], p, q });
			}
		}
		mesh.handle = handle;
		mesh.geometry = std::move(a_geometry);
		windStats_.bytes += mesh.Bytes();
		windStats_.vertices += vertices;
		windStats_.triangles += triangles;
		winds_[index] = std::move(mesh);
		++windLive_;
		windStats_.meshes = windLive_;
		windUploadQueue_.push_back(index);
		return handle;
	}

	void Backend::ReleaseWindMesh(std::uint32_t a_index)
	{
		if (a_index >= winds_.size() || !winds_[a_index].live)
			return;
		auto& mesh = winds_[a_index];
		if (!mesh.uploaded)
			std::erase(windUploadQueue_, a_index);
		// Back once every submission that may use them completed (the BLAS may be in a TLAS still in flight).
		skinStaticPool_.Free(mesh.rest, RetireValue());
		skinStaticPool_.Free(mesh.index, RetireValue());
		skinOutputPool_.Free(mesh.output, RetireValue());
		asPool_.Free(mesh.blas, RetireValue());
		windStats_.bytes -= std::min(windStats_.bytes, mesh.Bytes());
		windStats_.vertices -= std::min<std::uint64_t>(windStats_.vertices, mesh.vertices);
		windStats_.triangles -= std::min<std::uint64_t>(windStats_.triangles, mesh.triangles);
		mesh = {};
		freeWinds_.push_back(a_index);
		windLive_ -= std::min<std::uint32_t>(windLive_, 1);
		windStats_.meshes = windLive_;
	}

	void Backend::ReleaseWindResources()
	{
		if (!WindIdle())
			return;
		if (windScratch_) {
			Retire(windScratch_, windScratchBytes_, &windStats_.scratchBytes);
			windScratch_.Reset();
			windScratchBytes_ = 0;
			logger::info("[RayTracing] Foliage sway: scratch released");
		}
		windUploadQueue_.clear();
		windStats_.pending = 0;
	}

	bool Backend::WindWorkDue() const
	{
		if (windLive_ == 0 || !WindAvailable())
			return false;
		if (!windUploadQueue_.empty())
			return true;
		for (const auto& mesh : winds_)
			if (mesh.live && mesh.uploaded && !mesh.built)
				return true;
		return false;
	}

	void Backend::UploadWindMeshes(ID3D12GraphicsCommandList4* a_list, float a_cpuMs)
	{
		if (windUploadQueue_.empty())
			return;
		// As the character uploads (BackendSkin.cpp): pool ranges, no resource creation; a mesh whose ranges do not fit waits at
		// the front of the queue for the pools' next block.
		const std::int64_t deadline = WindQpc() + WindMsToQpc(a_cpuMs);
		struct Pending
		{
			std::uint32_t index;
			ArenaAlloc upload;
			std::uint64_t indexOffset;  // inside the upload range
		};
		std::vector<Pending> batch;
		std::uint32_t batchVertices = 0;
		while (!windUploadQueue_.empty() && batch.size() < kWindUploadsPerFrame && (batch.empty() || (batchVertices < kWindUploadVerticesPerFrame && WindQpc() < deadline))) {
			const std::uint32_t index = windUploadQueue_.front();
			if (index >= winds_.size() || !winds_[index].live || winds_[index].uploaded || !winds_[index].geometry) {
				windUploadQueue_.pop_front();
				continue;
			}
			auto& mesh = winds_[index];
			const auto& geometry = *mesh.geometry;
			Pending entry{ index, {}, 0 };
			entry.indexOffset = WindAlign(geometry.vertices.size() * sizeof(WindVertex), 256);
			const std::uint64_t uploadBytes = entry.indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
			ArenaAlloc rest, indexRange, output, blas;
			const bool ok = skinStaticPool_.Allocate(mesh.restBytes, rest) && skinStaticPool_.Allocate(mesh.indexBytes, indexRange) &&
			                skinOutputPool_.Allocate(mesh.outputBytes, output) && asPool_.Allocate(mesh.blasBytes, blas) &&
			                uploadPool_.Allocate(uploadBytes, entry.upload);
			if (!ok) {
				// Nothing recorded with them: back at once.
				skinStaticPool_.Free(rest, 0);
				skinStaticPool_.Free(indexRange, 0);
				skinOutputPool_.Free(output, 0);
				asPool_.Free(blas, 0);
				uploadPool_.Free(entry.upload, 0);
				++stats_.pools.allocFailsLastFrame;
				for (auto* pool : { &skinStaticPool_, &skinOutputPool_, &asPool_, &uploadPool_ }) {
					if (pool->failed) {
						SetWindFault(std::format("could not grow the {} pool for a wind mesh ({} vertices, {} triangles, {:.1f} MB)", pool->label,
							mesh.vertices, mesh.triangles, static_cast<double>(mesh.Bytes()) / (1024.0 * 1024.0)));
						break;
					}
				}
				break;
			}
			std::byte* mapped = uploadPool_.Mapped(entry.upload);
			std::memcpy(mapped, geometry.vertices.data(), geometry.vertices.size() * sizeof(WindVertex));
			std::memcpy(mapped + entry.indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			mesh.rest = rest;
			mesh.index = indexRange;
			mesh.output = output;
			mesh.blas = blas;
			windUploadQueue_.pop_front();
			batchVertices += geometry.Vertices();
			batch.push_back(entry);
		}
		if (batch.empty())
			return;

		// The copies: every touched static-pool block to copy dest, the copies, back to shader resource (the builds read them).
		std::vector<std::uint32_t> blocks;
		for (const auto& entry : batch) {
			blocks.push_back(winds_[entry.index].rest.block);
			blocks.push_back(winds_[entry.index].index.block);
		}
		std::sort(blocks.begin(), blocks.end());
		blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_COPY_DEST);
		for (const auto& entry : batch) {
			auto& mesh = winds_[entry.index];
			const auto& geometry = *mesh.geometry;
			ID3D12Resource* upload = uploadPool_.Resource(entry.upload);
			a_list->CopyBufferRegion(skinStaticPool_.Resource(mesh.rest), mesh.rest.offset, upload, entry.upload.offset, geometry.vertices.size() * sizeof(WindVertex));
			a_list->CopyBufferRegion(skinStaticPool_.Resource(mesh.index), mesh.index.offset, upload, entry.upload.offset + entry.indexOffset,
				geometry.indices.size() * sizeof(std::uint32_t));
		}
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		windRecorded_ = true;
		windMustSubmit_ = true;  // the uploads must reach the GPU (the meshes are marked uploaded)
		for (const auto& entry : batch) {
			auto& mesh = winds_[entry.index];
			uploadPool_.Free(entry.upload, RetireValue());  // read by this submission only
			mesh.uploaded = true;
			mesh.geometry.reset();
		}
	}

	void Backend::RecordWind(ID3D12GraphicsCommandList4* a_list, const FrameInput* a_input, std::uint32_t a_maxBuilds, float a_uploadCpuMs)
	{
		windRecorded_ = false;
		windMustSubmit_ = false;
		windStats_.buildsLastFrame = windStats_.refitsLastFrame = windStats_.deferredLastFrame = 0;
		windStats_.refitVerticesLastFrame = 0;
		windStats_.stillLastFrame = false;
		if (windLive_ == 0) {
			windStats_.pending = 0;
			return;
		}
		if (!windFault_.empty() || failed_ || !windPipeline_ || !windRootSignature_)
			return;
		UploadWindMeshes(a_list, a_uploadCpuMs);
		if (!windFault_.empty() || failed_)
			return;

		// This frame's wind pose (none: only first builds, at the mean pose).
		const bool timerPose = a_input && a_input->windValid && std::isfinite(a_input->windMagnitude) && std::isfinite(a_input->windTimer);
		const float magnitude = timerPose ? a_input->windMagnitude : 0.0f;
		const float timer = timerPose ? a_input->windTimer : 0.0f;
		const double phase = static_cast<double>(magnitude) * static_cast<double>(timer);

		struct Op
		{
			std::uint32_t mesh;
			std::uint64_t scratchOffset;
		};
		std::vector<Op> builds, refits;
		std::uint64_t scratchTotal = 0;
		std::uint32_t pending = 0;
		bool anyBuilt = false;
		for (std::uint32_t i = 0; i < winds_.size(); ++i) {
			auto& mesh = winds_[i];
			if (!mesh.live)
				continue;
			if (!mesh.uploaded) {
				++pending;
				continue;
			}
			if (!mesh.built) {
				if (builds.size() >= a_maxBuilds) {
					++windStats_.deferredLastFrame;
					++pending;
					continue;
				}
				builds.push_back(Op{ i, scratchTotal });
				scratchTotal += WindAlign(mesh.buildScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			} else {
				anyBuilt = true;
			}
		}
		if (timerPose) {
			// Every built mesh (and every mesh built just now) whose pose moved since its last refit: in place, from this frame's pose.
			auto wanted = [&](const WindMesh& a_mesh) {
				return !a_mesh.posed || std::abs(0.25 * static_cast<double>(a_mesh.frequency) * (phase - a_mesh.posedPhase)) >= kWindStillCycles;
			};
			for (const auto& op : builds) {
				refits.push_back(Op{ op.mesh, scratchTotal });
				scratchTotal += WindAlign(winds_[op.mesh].updateScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			}
			for (std::uint32_t i = 0; i < winds_.size(); ++i) {
				const auto& mesh = winds_[i];
				if (!mesh.live || !mesh.uploaded || !mesh.built || !wanted(mesh))
					continue;
				refits.push_back(Op{ i, scratchTotal });
				scratchTotal += WindAlign(mesh.updateScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			}
			windStats_.stillLastFrame = anyBuilt && refits.size() == builds.size();
		}
		windStats_.pending = pending;
		if (builds.empty() && refits.empty())
			return;

		if (!windScratch_ || windScratchBytes_ < scratchTotal) {
			// A region per build / refit: no barriers between them. The new buffer first; the old one goes behind the fence.
			const std::uint64_t bytes = WindAlign(std::max<std::uint64_t>(scratchTotal + scratchTotal / 4, 4ull << 20), 64 * 1024);
			auto fresh = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, Util::FrameTrace::VramOwner::RtScratch);
			if (!fresh) {
				SetWindFault(std::format("could not create a {:.1f} MB scratch buffer", static_cast<double>(bytes) / (1024.0 * 1024.0)));
				return;
			}
			if (windScratch_)
				Retire(windScratch_, windScratchBytes_, &windStats_.scratchBytes);
			windScratch_ = std::move(fresh);
			windScratch_->SetName(L"CS RayTracing wind scratch");
			windScratchBytes_ = bytes;
			windStats_.scratchBytes += bytes;
		}
		const D3D12_GPU_VIRTUAL_ADDRESS scratch = windScratch_->GetGPUVirtualAddress();
		windRecorded_ = true;

		a_list->SetComputeRootSignature(windRootSignature_.Get());
		a_list->SetPipelineState(windPipeline_.Get());
		auto pose = [&](const WindMesh& a_mesh, std::uint32_t a_mode) {
			const std::uint32_t constants[kWindConstants] = { a_mesh.vertices, a_mode, std::bit_cast<std::uint32_t>(a_mesh.amplitude),
				std::bit_cast<std::uint32_t>(a_mesh.frequency), std::bit_cast<std::uint32_t>(magnitude), std::bit_cast<std::uint32_t>(timer), 0u, 0u };
			a_list->SetComputeRoot32BitConstants(0, kWindConstants, constants, 0);
			a_list->SetComputeRootShaderResourceView(1, skinStaticPool_.Address(a_mesh.rest));
			a_list->SetComputeRootUnorderedAccessView(2, skinOutputPool_.Address(a_mesh.output));
			a_list->Dispatch((a_mesh.vertices + kWindGroupSize - 1) / kWindGroupSize, 1, 1);
		};
		auto outputBlocks = [&](const std::vector<Op>& a_ops) {
			std::vector<std::uint32_t> blocks;
			blocks.reserve(a_ops.size());
			for (const auto& op : a_ops)
				blocks.push_back(winds_[op.mesh].output.block);
			std::sort(blocks.begin(), blocks.end());
			blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
			return blocks;
		};
		auto record = [&](WindMesh& a_mesh, std::uint64_t a_scratchOffset, bool a_update) {
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = WindGeometryDesc(a_mesh.opaque, a_mesh.vertices, a_mesh.triangles);
			geometryDesc.Triangles.VertexBuffer.StartAddress = skinOutputPool_.Address(a_mesh.output);
			geometryDesc.Triangles.IndexBuffer = skinStaticPool_.Address(a_mesh.index);
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
			buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			buildDesc.Inputs.Flags = kWindBuildFlags;
			buildDesc.Inputs.NumDescs = 1;
			buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			buildDesc.Inputs.pGeometryDescs = &geometryDesc;
			buildDesc.DestAccelerationStructureData = asPool_.Address(a_mesh.blas);
			buildDesc.ScratchAccelerationStructureData = scratch + a_scratchOffset;
			if (a_update) {
				buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
				buildDesc.SourceAccelerationStructureData = buildDesc.DestAccelerationStructureData;  // in place
			}
			a_list->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
		};

		// 1. First builds at the mean pose: posed vertices (UAV), then read by the builds (non-pixel shader resource).
		if (!builds.empty()) {
			const auto blocks = outputBlocks(builds);
			TransitionBlocks(a_list, skinOutputPool_, blocks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			for (const auto& op : builds)
				pose(winds_[op.mesh], 0u);
			TransitionBlocks(a_list, skinOutputPool_, blocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			for (const auto& op : builds) {
				auto& mesh = winds_[op.mesh];
				record(mesh, op.scratchOffset, false);
				mesh.built = true;
				mesh.posed = false;
				if (mesh.handle < blas_.size() && blas_[mesh.handle].wind == op.mesh) {
					auto& blas = blas_[mesh.handle];
					blas.alloc = mesh.blas;
					blas.bytes = mesh.blasBytes;
					blas.state = BlasState::Ready;  // usable from this list on; never compacted
				}
				++windStats_.buildsLastFrame;
				++windStats_.buildsTotal;
				++recWork_.windBuilds;  // (46p) the build-cost model skips such submissions
				windMustSubmit_ = true;  // marked built: this list must run
			}
		}

		// 2. Refits in place at this frame's pose. The posed-vertex blocks go back to UAV (this transition waits for the builds'
		//    reads above); the global UAV barrier orders the first builds before their refit.
		if (!refits.empty()) {
			if (!builds.empty()) {
				const auto barrier = WindUavBarrier();
				a_list->ResourceBarrier(1, &barrier);
			}
			const auto blocks = outputBlocks(refits);
			TransitionBlocks(a_list, skinOutputPool_, blocks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			for (const auto& op : refits)
				pose(winds_[op.mesh], 1u);
			TransitionBlocks(a_list, skinOutputPool_, blocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			for (const auto& op : refits) {
				auto& mesh = winds_[op.mesh];
				record(mesh, op.scratchOffset, true);
				mesh.posed = true;
				mesh.posedPhase = phase;
				++windStats_.refitsLastFrame;
				windStats_.refitVerticesLastFrame += mesh.vertices;
				++recWork_.windRefits;
				recWork_.windVertices += mesh.vertices;
			}
		}
		// The caller's barrier (end of the prep list / before the TLAS) orders these writes before any read of the BLASes.
	}
}
