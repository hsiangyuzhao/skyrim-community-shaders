// (47l) Foliage sway on the ray tracer's D3D12 side. (47n) Per instance.
//
// A wind source is a tree-animated engine mesh (leaves, bushes, swaying branches): its rest vertices (position, packed normal and
// vertex alpha) and its indices, uploaded once (a few per frame) and shared by every instance of that mesh. A wind instance is one
// placed shape over a source: its own posed vertex buffer and a BLAS range sized for ALLOW_UPDATE builds. Its first build poses it
// at the mean of its sway (WindFoliageCS mode 0) so the later in-place refits stay near the BVH it was built for. Every frame the
// scene hands each instance its pose (SetWindPose: the engine's own per-plant TreeParams / WindTimers, see Wind.h); an instance whose
// pose changed since its last refit is posed by WindFoliageCS and its BLAS refit in place (PERFORM_UPDATE). A plant whose timer
// does not move (no leaf data) is posed once. First builds are capped per frame; refits are not. The ranges come from the character
// pools (rest + index: skinStaticPool_, posed vertices: skinOutputPool_, BLAS: asPool_) and go back behind the fence (RetireValue)
// like every other range. The CPU never waits.

#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <bit>
#include <cstring>

// Generated at build time from Shaders/WindFoliageCS.hlsl by the Windows SDK's dxc (CMakeLists.txt).
#include "RayTracing/WindFoliageCS.dxil.h"

namespace RayTracingRT
{
	namespace
	{
		std::string WindHr(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		std::uint64_t WindAlign(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) & ~(a_alignment - 1); }

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

		bool SamePose(const float a_a[4], const float a_b[4]) { return std::memcmp(a_a, a_b, sizeof(float) * 4) == 0; }
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

	std::uint32_t Backend::CreateWindSource(std::unique_ptr<WindGeometry> a_geometry)
	{
		if (!WindAvailable() || !a_geometry) {
			++windStats_.refused;
			return UINT32_MAX;
		}
		const WindGeometry& geometry = *a_geometry;
		const std::uint32_t vertices = geometry.Vertices();
		const std::uint32_t triangles = geometry.Triangles();
		if (vertices == 0 || triangles == 0 || vertices > kWindMaxVertices || triangles > kWindMaxTriangles || geometry.indices.size() % 3 != 0) {
			++windStats_.refused;
			return UINT32_MAX;
		}
		for (const auto index : geometry.indices) {
			if (index >= vertices) {
				++windStats_.refused;
				return UINT32_MAX;
			}
		}
		WindSource source;
		for (int a = 0; a < 3; ++a) {
			source.boundsMin[a] = FLT_MAX;
			source.boundsMax[a] = -FLT_MAX;
		}
		for (const auto& vertex : geometry.vertices) {
			for (int a = 0; a < 3; ++a) {
				if (!std::isfinite(vertex.position[a])) {
					++windStats_.refused;
					return UINT32_MAX;
				}
				source.boundsMin[a] = std::min(source.boundsMin[a], vertex.position[a]);
				source.boundsMax[a] = std::max(source.boundsMax[a], vertex.position[a]);
			}
		}
		source.live = true;
		source.vertices = vertices;
		source.triangles = triangles;
		source.opaque = geometry.opaque;
		source.restBytes = WindAlign(static_cast<std::uint64_t>(vertices) * kWindVertexBytes, 256);
		source.indexBytes = WindAlign(static_cast<std::uint64_t>(triangles) * 3 * sizeof(std::uint32_t), 256);
		if (windStats_.bytes + source.Bytes() > kWindBudgetBytes) {
			++windStats_.overBudget;
			return UINT32_MAX;
		}
		source.refs = 1;  // the caller's
		source.geometry = std::move(a_geometry);
		std::uint32_t index;
		if (!freeWindSources_.empty()) {
			index = freeWindSources_.back();
			freeWindSources_.pop_back();
		} else {
			index = static_cast<std::uint32_t>(windSources_.size());
			windSources_.emplace_back();
		}
		windStats_.bytes += source.Bytes();
		windStats_.sourceBytes += source.Bytes();
		windSources_[index] = std::move(source);
		++windSourcesLive_;
		windStats_.sources = windSourcesLive_;
		windUploadQueue_.push_back(index);
		return index;
	}

	void Backend::FreeWindSource(std::uint32_t a_source)
	{
		if (a_source >= windSources_.size() || !windSources_[a_source].live)
			return;
		auto& source = windSources_[a_source];
		if (!source.uploaded)
			std::erase(windUploadQueue_, a_source);
		// Back once every submission that may use them completed (an instance refit in flight may still read them).
		skinStaticPool_.Free(source.rest, RetireValue());
		skinStaticPool_.Free(source.index, RetireValue());
		windStats_.bytes -= std::min(windStats_.bytes, source.Bytes());
		windStats_.sourceBytes -= std::min(windStats_.sourceBytes, source.Bytes());
		source = {};
		freeWindSources_.push_back(a_source);
		windSourcesLive_ -= std::min<std::uint32_t>(windSourcesLive_, 1);
		windStats_.sources = windSourcesLive_;
	}

	void Backend::ReleaseWindSource(std::uint32_t a_source)
	{
		if (a_source >= windSources_.size() || !windSources_[a_source].live)
			return;
		auto& source = windSources_[a_source];
		if (source.refs <= 1)
			FreeWindSource(a_source);
		else
			--source.refs;
	}

	BlasHandle Backend::CreateWindBlas(std::uint32_t a_source, float a_amplitude, std::uint32_t a_alphaInfo)
	{
		if (!WindAvailable() || a_source >= windSources_.size() || !windSources_[a_source].live || !std::isfinite(a_amplitude) ||
			std::abs(a_amplitude) > 10000.0f) {
			++windStats_.refused;
			return kInvalidBlas;
		}
		auto& source = windSources_[a_source];
		WindMesh mesh;
		mesh.live = true;
		mesh.source = a_source;
		mesh.vertices = source.vertices;
		mesh.triangles = source.triangles;
		mesh.opaque = source.opaque;
		mesh.meanAmplitude = a_amplitude;
		mesh.alphaSlot = source.opaque ? UINT32_MAX : a_alphaInfo;  // (47s)
		mesh.outputBytes = WindAlign(static_cast<std::uint64_t>(source.vertices) * kWindOutputStride, 256);
		{
			const D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = WindGeometryDesc(mesh.opaque, mesh.vertices, mesh.triangles);
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = kWindBuildFlags;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &geometryDesc;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0) {
				++windStats_.refused;
				return kInvalidBlas;
			}
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
		blas.triangles = mesh.triangles;
		blas.wind = index;
		// (45f) The TLAS self-check box: the rest box grown by the sway's reach (1.1 x |amplitude| along any normal, vertex alpha <= 1).
		const float reach = 1.1f * std::abs(a_amplitude);
		for (int a = 0; a < 3; ++a) {
			blas.boundsMin[a] = source.boundsMin[a] - reach;
			blas.boundsMax[a] = source.boundsMax[a] + reach;
		}
		++source.refs;
		mesh.handle = handle;
		windStats_.bytes += mesh.Bytes();
		windStats_.vertices += mesh.vertices;
		windStats_.triangles += mesh.triangles;
		winds_[index] = std::move(mesh);
		++windLive_;
		windStats_.meshes = windLive_;
		return handle;
	}

	void Backend::SetWindPose(BlasHandle a_handle, float a_magnitude, float a_amplitude, float a_frequency, float a_timer)
	{
		if (a_handle >= blas_.size() || blas_[a_handle].wind == UINT32_MAX)
			return;
		const std::uint32_t index = blas_[a_handle].wind;
		if (index >= winds_.size() || !winds_[index].live || winds_[index].handle != a_handle)
			return;
		auto& mesh = winds_[index];
		const float pose[4] = { a_magnitude, a_amplitude, a_frequency, a_timer };
		for (const float v : pose)
			if (!std::isfinite(v) || std::abs(v) > 1.0e7f)
				return;  // an implausible read keeps the last pose
		std::copy_n(pose, 4, mesh.pose);
		mesh.hasPose = true;
	}

	void Backend::ReleaseWindMesh(std::uint32_t a_index)
	{
		if (a_index >= winds_.size() || !winds_[a_index].live)
			return;
		auto& mesh = winds_[a_index];
		// Back once every submission that may use them completed (the BLAS may be in a TLAS still in flight).
		if (mesh.allocated) {
			skinOutputPool_.Free(mesh.output, RetireValue());
			asPool_.Free(mesh.blas, RetireValue());
		}
		windStats_.bytes -= std::min(windStats_.bytes, mesh.Bytes());
		windStats_.vertices -= std::min<std::uint64_t>(windStats_.vertices, mesh.vertices);
		windStats_.triangles -= std::min<std::uint64_t>(windStats_.triangles, mesh.triangles);
		const std::uint32_t source = mesh.source;
		const std::uint32_t layout = mesh.layout;
		mesh = {};
		if (layout != UINT32_MAX)
			ReleaseVariantRef(layout);  // (47s) its classified layout's user
		freeWinds_.push_back(a_index);
		windLive_ -= std::min<std::uint32_t>(windLive_, 1);
		windStats_.meshes = windLive_;
		ReleaseWindSource(source);  // the instance's reference
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
		if (windLive_ == 0 && windSourcesLive_ == 0)
			return false;
		if (!WindAvailable())
			return false;
		if (!windUploadQueue_.empty())
			return true;
		for (const auto& mesh : winds_)
			if (mesh.live && !mesh.built)
				return true;
		return false;
	}

	void Backend::UploadWindMeshes(ID3D12GraphicsCommandList4* a_list, float a_cpuMs)
	{
		if (windUploadQueue_.empty())
			return;
		// As the character uploads (BackendSkin.cpp): pool ranges, no resource creation; a source whose ranges do not fit waits at
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
			if (index >= windSources_.size() || !windSources_[index].live || windSources_[index].uploaded || !windSources_[index].geometry) {
				windUploadQueue_.pop_front();
				continue;
			}
			auto& source = windSources_[index];
			const auto& geometry = *source.geometry;
			Pending entry{ index, {}, 0 };
			entry.indexOffset = WindAlign(geometry.vertices.size() * sizeof(WindVertex), 256);
			const std::uint64_t uploadBytes = entry.indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
			ArenaAlloc rest, indexRange;
			const bool ok = skinStaticPool_.Allocate(source.restBytes, rest) && skinStaticPool_.Allocate(source.indexBytes, indexRange) &&
			                uploadPool_.Allocate(uploadBytes, entry.upload);
			if (!ok) {
				// Nothing recorded with them: back at once.
				skinStaticPool_.Free(rest, 0);
				skinStaticPool_.Free(indexRange, 0);
				uploadPool_.Free(entry.upload, 0);
				++stats_.pools.allocFailsLastFrame;
				for (auto* pool : { &skinStaticPool_, &uploadPool_ }) {
					if (pool->failed) {
						SetWindFault(std::format("could not grow the {} pool for a wind mesh ({} vertices, {} triangles)", pool->label, source.vertices,
							source.triangles));
						break;
					}
				}
				break;
			}
			std::byte* mapped = uploadPool_.Mapped(entry.upload);
			std::memcpy(mapped, geometry.vertices.data(), geometry.vertices.size() * sizeof(WindVertex));
			std::memcpy(mapped + entry.indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			source.rest = rest;
			source.index = indexRange;
			windUploadQueue_.pop_front();
			batchVertices += geometry.Vertices();
			batch.push_back(entry);
		}
		if (batch.empty())
			return;

		// The copies: every touched static-pool block to copy dest, the copies, back to shader resource (the poses / builds read them).
		std::vector<std::uint32_t> blocks;
		for (const auto& entry : batch) {
			blocks.push_back(windSources_[entry.index].rest.block);
			blocks.push_back(windSources_[entry.index].index.block);
		}
		std::sort(blocks.begin(), blocks.end());
		blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_COPY_DEST);
		for (const auto& entry : batch) {
			auto& source = windSources_[entry.index];
			const auto& geometry = *source.geometry;
			ID3D12Resource* upload = uploadPool_.Resource(entry.upload);
			a_list->CopyBufferRegion(skinStaticPool_.Resource(source.rest), source.rest.offset, upload, entry.upload.offset, geometry.vertices.size() * sizeof(WindVertex));
			a_list->CopyBufferRegion(skinStaticPool_.Resource(source.index), source.index.offset, upload, entry.upload.offset + entry.indexOffset,
				geometry.indices.size() * sizeof(std::uint32_t));
		}
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		windRecorded_ = true;
		windMustSubmit_ = true;  // the uploads must reach the GPU (the sources are marked uploaded)
		for (const auto& entry : batch) {
			auto& source = windSources_[entry.index];
			uploadPool_.Free(entry.upload, RetireValue());  // read by this submission only
			source.uploaded = true;
			// (47s) An alpha-tested source keeps its indices: Alpha Classify's index lists of its swaying copies.
			if (!source.opaque && bakePipeline_)
				source.indicesCpu = std::move(source.geometry->indices);
			source.geometry.reset();
		}
	}

	void Backend::RecordWind(ID3D12GraphicsCommandList4* a_list, const FrameInput* a_input, std::uint32_t a_maxBuilds, float a_uploadCpuMs)
	{
		windRecorded_ = false;
		windMustSubmit_ = false;
		windStats_.buildsLastFrame = windStats_.refitsLastFrame = windStats_.deferredLastFrame = 0;
		windStats_.refitVerticesLastFrame = 0;
		windStats_.stillLastFrame = false;
		if (windLive_ == 0 && windSourcesLive_ == 0) {
			windStats_.pending = 0;
			return;
		}
		if (!windFault_.empty() || failed_ || !windPipeline_ || !windRootSignature_)
			return;
		UploadWindMeshes(a_list, a_uploadCpuMs);
		if (!windFault_.empty() || failed_)
			return;
		const bool refitsWanted = a_input != nullptr;  // BuildOnly (no frame): first builds only

		struct Op
		{
			std::uint32_t mesh;
			std::uint64_t scratchOffset;
		};
		std::vector<Op> builds, refits;
		std::uint64_t scratchTotal = 0;
		std::uint32_t pending = 0;
		bool anyBuilt = false;
		// (47s) Alpha Classify: the sizes of a mesh's BLAS in a layout (unclassified, or a classified variant's two geometries).
		auto layoutSizes = [&](const WindMesh& a_mesh, std::uint32_t a_layout, std::uint64_t& o_blas, std::uint64_t& o_build, std::uint64_t& o_update) {
			D3D12_RAYTRACING_GEOMETRY_DESC descs[2]{};
			const UINT count = WindGeometryDescs(a_mesh, a_layout, descs);
			if (count == 0)
				return false;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = kWindBuildFlags;
			inputs.NumDescs = count;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = descs;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
				return false;
			o_blas = WindAlign(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			o_build = WindAlign(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			o_update = WindAlign(std::max<std::uint64_t>(info.UpdateScratchDataSizeInBytes, 256), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			return true;
		};
		// The mesh is (re)built in a_layout: its BLAS range is grown when that layout needs more, the scratch sizes are the layout's, and
		// the variant gets its reference. False: not this frame (the pool's next block) or never (no triangles left).
		auto takeLayout = [&](WindMesh& a_mesh, std::uint32_t a_layout) {
			std::uint64_t blasBytes = 0, buildScratch = 0, updateScratch = 0;
			if (!layoutSizes(a_mesh, a_layout, blasBytes, buildScratch, updateScratch))
				return false;
			if (a_mesh.allocated && blasBytes > a_mesh.blasBytes) {
				ArenaAlloc fresh;
				if (!asPool_.Allocate(blasBytes, fresh)) {
					++stats_.pools.allocFailsLastFrame;
					return false;
				}
				asPool_.Free(a_mesh.blas, RetireValue());  // a TLAS in flight may still name it
				windStats_.bytes += blasBytes - a_mesh.blasBytes;
				a_mesh.blas = fresh;
				a_mesh.blasBytes = blasBytes;
			} else if (!a_mesh.allocated && blasBytes > a_mesh.blasBytes) {
				windStats_.bytes += blasBytes - a_mesh.blasBytes;
				a_mesh.blasBytes = blasBytes;
			}
			a_mesh.buildScratch = buildScratch;
			a_mesh.updateScratch = updateScratch;
			if (a_layout != a_mesh.layout) {
				const std::uint32_t old = a_mesh.layout;
				if (a_layout != UINT32_MAX)
					AddVariantRef(a_layout);
				a_mesh.layout = a_layout;
				if (old != UINT32_MAX)
					ReleaseVariantRef(old);
			}
			return true;
		};
		for (std::uint32_t i = 0; i < winds_.size(); ++i) {
			auto& mesh = winds_[i];
			if (!mesh.live)
				continue;
			mesh.inBuild = false;
			const bool sourceReady = mesh.source < windSources_.size() && windSources_[mesh.source].live && windSources_[mesh.source].uploaded;
			if (!sourceReady) {
				++pending;
				continue;
			}
			// (47s) The layout it should have now (a variant in use and its index list over this source uploaded; else unclassified).
			const std::uint32_t want = mesh.opaque ? UINT32_MAX : WantedWindLayout(mesh);
			if (!mesh.built) {
				if (builds.size() >= a_maxBuilds) {
					++windStats_.deferredLastFrame;
					++pending;
					continue;
				}
				if (want != mesh.layout && !takeLayout(mesh, want) && mesh.layout != UINT32_MAX && !takeLayout(mesh, UINT32_MAX)) {
					++pending;
					continue;
				}
				if (!mesh.allocated) {
					ArenaAlloc output, blas;
					if (!skinOutputPool_.Allocate(mesh.outputBytes, output) || !asPool_.Allocate(mesh.blasBytes, blas)) {
						skinOutputPool_.Free(output, 0);
						asPool_.Free(blas, 0);
						++stats_.pools.allocFailsLastFrame;
						for (auto* pool : { &skinOutputPool_, &asPool_ }) {
							if (pool->failed) {
								SetWindFault(std::format("could not grow the {} pool for a wind instance ({} vertices, {} triangles)", pool->label, mesh.vertices,
									mesh.triangles));
								return;
							}
						}
						++pending;
						continue;  // the pools' next block (grown asynchronously)
					}
					mesh.output = output;
					mesh.blas = blas;
					mesh.allocated = true;
				}
				mesh.inBuild = true;
				builds.push_back(Op{ i, scratchTotal });
				scratchTotal += WindAlign(mesh.buildScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			} else {
				anyBuilt = true;
				// (47s) Built in another layout (classified now, or Alpha Classify switched off): rebuilt at once in the new one (a first
				// build's work, inside the per-frame cap); until then it keeps tracing in its old layout with that layout's record.
				if (want != mesh.layout && builds.size() < a_maxBuilds && takeLayout(mesh, want)) {
					mesh.inBuild = true;
					builds.push_back(Op{ i, scratchTotal });
					scratchTotal += WindAlign(mesh.buildScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
				}
			}
		}
		if (refitsWanted) {
			// Every built instance (and every one built just now) whose pose changed since its last refit: in place, at this frame's pose.
			for (const auto& op : builds) {
				if (!winds_[op.mesh].hasPose)
					continue;
				refits.push_back(Op{ op.mesh, scratchTotal });
				scratchTotal += WindAlign(winds_[op.mesh].updateScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			}
			for (std::uint32_t i = 0; i < winds_.size(); ++i) {
				const auto& mesh = winds_[i];
				// (47s) inBuild: (re)built this frame (its refit is in the list above).
				if (!mesh.live || !mesh.built || mesh.inBuild || !mesh.hasPose || (mesh.posed && SamePose(mesh.pose, mesh.posedPose)))
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
		// Mode 0: the mean pose of the first build (amplitude only); mode 1: the pose (magnitude, amplitude, frequency, timer).
		auto pose = [&](const WindMesh& a_mesh, std::uint32_t a_mode) {
			const auto& source = windSources_[a_mesh.source];
			const float amplitude = a_mode == 0 ? a_mesh.meanAmplitude : a_mesh.pose[1];
			const std::uint32_t constants[kWindConstants] = { a_mesh.vertices, a_mode, std::bit_cast<std::uint32_t>(amplitude), std::bit_cast<std::uint32_t>(a_mesh.pose[2]),
				std::bit_cast<std::uint32_t>(a_mesh.pose[0]), std::bit_cast<std::uint32_t>(a_mesh.pose[3]), 0u, 0u };
			a_list->SetComputeRoot32BitConstants(0, kWindConstants, constants, 0);
			a_list->SetComputeRootShaderResourceView(1, skinStaticPool_.Address(source.rest));
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
			// (47s) In its layout: one geometry (unclassified), or a classified variant's mixed (non-opaque) and opaque ones (its index
			// list over this source); a refit uses the descriptions of the build.
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDescs[2]{};
			const UINT descCount = WindGeometryDescs(a_mesh, a_mesh.layout, geometryDescs);
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
			buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			buildDesc.Inputs.Flags = kWindBuildFlags;
			buildDesc.Inputs.NumDescs = descCount;
			buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			buildDesc.Inputs.pGeometryDescs = geometryDescs;
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
				std::copy_n(mesh.pose, 4, mesh.posedPose);
				++windStats_.refitsLastFrame;
				windStats_.refitVerticesLastFrame += mesh.vertices;
				++recWork_.windRefits;
				recWork_.windVertices += mesh.vertices;
			}
		}
		// The caller's barrier (end of the prep list / before the TLAS) orders these writes before any read of the BLASes.
	}
}
