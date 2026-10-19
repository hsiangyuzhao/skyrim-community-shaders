// (45i) Skinned characters on the ray tracer's D3D12 side.
//
// A skinned shape is uploaded once (a few per frame): its rest vertices with bone indices and
// weights, its indices, a vertex buffer the skinning writes and a BLAS buffer sized for builds
// with ALLOW_UPDATE. Every frame it is drawn, SkinCharactersCS skins it with this frame's bone
// matrices (FrameInput::skinBones, copied into the command context's own upload buffer) and its
// BLAS is refit in place (PERFORM_UPDATE); the first build and a periodic rebuild (every
// kSkinRebuildFrames refits, a few per frame) use a full build. First builds are capped per
// frame; refits are not, so a shape that was traced once is traced on every frame it is drawn
// (the caster filter relies on that). Every resource goes through Retire(): released once the GPU
// passed the fence of the last submission that may use it. The CPU never waits.

#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <bit>

// Generated at build time from Shaders/SkinCharactersCS.hlsl by the Windows SDK's dxc (CMakeLists.txt).
#include "RayTracing/SkinCharactersCS.dxil.h"

namespace RayTracingRT
{
	namespace
	{
		std::string Hr(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		std::uint64_t AlignUp(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) & ~(a_alignment - 1); }

		D3D12_RESOURCE_BARRIER SkinTransition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			return barrier;
		}

		constexpr std::uint32_t kSkinVertexBytes = sizeof(SkinVertex);
		constexpr std::uint32_t kSkinOutputStride = sizeof(float) * 3;
		constexpr std::uint32_t kSkinGroupSize = 64;  // SkinCharactersCS numthreads

		// (46q) QPC ticks per ms (the grace period of merged character BLASes).
		double QpcPerMs()
		{
			static const double perMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return static_cast<double>(f.QuadPart) / 1000.0;
			}();
			return perMs;
		}
	}

	bool Backend::CreateSkinPipelines()
	{
		D3D12_ROOT_PARAMETER params[4]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = 4;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[2].Descriptor.ShaderRegister = 1;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[3].Descriptor.ShaderRegister = 0;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 4;
		rootDesc.pParameters = params;
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (FAILED(hr) || FAILED(hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&skinRootSignature_)))) {
			skinRootSignature_.Reset();
			logger::warn("[RayTracing] Characters unavailable: the skinning root signature failed ({})", Hr(hr));
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = skinRootSignature_.Get();
		psoDesc.CS.pShaderBytecode = g_SkinCharactersCS;
		psoDesc.CS.BytecodeLength = sizeof(g_SkinCharactersCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&skinPipeline_)))) {
			skinPipeline_.Reset();
			skinRootSignature_.Reset();
			logger::warn("[RayTracing] Characters unavailable: the skinning shader was rejected by the driver ({})", Hr(hr));
			return false;
		}
		return true;
	}

	void Backend::SetSkinFault(std::string a_reason)
	{
		if (!skinFault_.empty())
			return;
		skinFault_ = std::move(a_reason);
		logger::error("[RayTracing] Characters: {} -- switched off", skinFault_);
	}

	SkinHandle Backend::CreateSkinShape(std::unique_ptr<SkinGeometry> a_geometry)
	{
		if (!SkinAvailable() || !a_geometry)
			return kInvalidSkin;
		const SkinGeometry& geometry = *a_geometry;
		const std::uint32_t vertices = geometry.Vertices();
		const std::uint32_t triangles = geometry.Triangles();
		const bool opaque = geometry.Opaque();
		auto refuse = [&]() {
			++skinStats_.refused;
			return kInvalidSkin;
		};
		if (vertices == 0 || triangles == 0 || vertices > kSkinMaxVertices || triangles > kSkinMaxTriangles || geometry.indices.size() % 3 != 0 ||
			geometry.boneCount == 0 || geometry.boneCount > kSkinMaxBones)
			return refuse();
		for (const auto index : geometry.indices)
			if (index >= vertices)
				return refuse();
		for (const auto& vertex : geometry.vertices) {
			for (int i = 0; i < 4; ++i) {
				if (vertex.weights[i] != 0.0f && ((vertex.bones >> (8 * i)) & 0xFFu) >= geometry.boneCount)
					return refuse();
				if (!std::isfinite(vertex.weights[i]))
					return refuse();
			}
			if (!std::isfinite(vertex.position[0]) || !std::isfinite(vertex.position[1]) || !std::isfinite(vertex.position[2]))
				return refuse();
		}
		if (!opaque && (geometry.alphaUV.size() != static_cast<std::size_t>(triangles) * 3 * 2 || !SkinAlphaAvailable()))
			return refuse();
		// (46m) Merge Character Parts: an opaque shape traced inside its character's merged BLAS has none of its own.
		const bool grouped = geometry.grouped && opaque;

		SkinShape shape;
		shape.live = true;
		shape.grouped = grouped;
		shape.serial = ++skinSerial_;
		shape.vertices = vertices;
		shape.triangles = triangles;
		shape.boneCount = geometry.boneCount;
		shape.restBytes = AlignUp(static_cast<std::uint64_t>(vertices) * kSkinVertexBytes, 256);
		shape.indexBytes = AlignUp(static_cast<std::uint64_t>(triangles) * 3 * sizeof(std::uint32_t), 256);
		shape.outputBytes = AlignUp(static_cast<std::uint64_t>(vertices) * kSkinOutputStride, 256);
		if (!grouped) {
			// Sizes up front (the budget is checked before anything is created).
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometryDesc.Flags = opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = kSkinOutputStride;
			geometryDesc.Triangles.VertexCount = vertices;
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexCount = triangles * 3;
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &geometryDesc;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			if (info.ResultDataMaxSizeInBytes == 0 || info.ScratchDataSizeInBytes == 0)
				return refuse();
			shape.blasBytes = AlignUp(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			shape.buildScratch = AlignUp(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			shape.updateScratch = AlignUp(std::max<std::uint64_t>(info.UpdateScratchDataSizeInBytes, 256), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		}
		if (skinStats_.bytes + shape.Bytes() > kSkinBudgetBytes) {
			++skinStats_.overBudget;
			return kInvalidSkin;
		}
		if (!opaque) {
			shape.uvCount = triangles * 3;
			if (!AllocateAlphaUv(shape.uvCount, shape.uvOffset))
				return refuse();
		}
		shape.geometry = std::move(a_geometry);

		SkinHandle handle;
		if (!freeSkins_.empty()) {
			handle = freeSkins_.back();
			freeSkins_.pop_back();
		} else {
			handle = static_cast<SkinHandle>(skins_.size());
			skins_.emplace_back();
		}
		skinStats_.bytes += shape.Bytes();
		skinStats_.vertices += vertices;
		skinStats_.triangles += triangles;
		skins_[handle] = std::move(shape);
		++skinLive_;
		skinStats_.shapes = skinLive_;
		skinUploadQueue_.push_back(handle);
		skinStats_.pending = static_cast<std::uint32_t>(skinUploadQueue_.size());
		return handle;
	}

	void Backend::ReleaseSkinShape(SkinHandle a_shape)
	{
		if (a_shape >= skins_.size() || !skins_[a_shape].live)
			return;
		auto& shape = skins_[a_shape];
		if (!shape.uploaded) {
			std::erase(skinUploadQueue_, a_shape);
			skinStats_.pending = static_cast<std::uint32_t>(skinUploadQueue_.size());
		}
		// (46g) Pool ranges, back once every submission that may use them completed.
		skinStaticPool_.Free(shape.rest, RetireValue());
		skinStaticPool_.Free(shape.index, RetireValue());
		skinOutputPool_.Free(shape.output, RetireValue());
		asPool_.Free(shape.blas, RetireValue());
		if (shape.uvOffset != UINT32_MAX)
			FreeAlphaUv(shape.uvOffset, shape.uvCount);
		skinStats_.bytes -= std::min(skinStats_.bytes, shape.Bytes());
		skinStats_.vertices -= std::min<std::uint64_t>(skinStats_.vertices, shape.vertices);
		skinStats_.triangles -= std::min<std::uint64_t>(skinStats_.triangles, shape.triangles);
		shape = {};
		freeSkins_.push_back(a_shape);
		skinLive_ -= std::min<std::uint32_t>(skinLive_, 1);
		skinStats_.shapes = skinLive_;
	}

	bool Backend::IsSkinShapeReady(SkinHandle a_shape) const
	{
		return a_shape < skins_.size() && skins_[a_shape].live && skins_[a_shape].built;
	}

	std::uint32_t Backend::CreateSkinAlphaInfo(SkinHandle a_shape, AlphaTexture a_texture, const AlphaMaterial& a_material)
	{
		if (a_shape >= skins_.size() || !skins_[a_shape].live || skins_[a_shape].uvOffset == UINT32_MAX)
			return kInvalidAlphaInfo;
		return CreateAlphaInfoAt(skins_[a_shape].uvOffset, skins_[a_shape].triangles, a_texture, a_material);
	}

	void Backend::ReleaseSkinResources()
	{
		if (!SkinIdle())
			return;
		ReleaseSkinGroups();  // (46m)
		bool any = false;
		if (skinScratch_) {
			Retire(skinScratch_, skinScratchBytes_, &skinStats_.scratchBytes);
			skinScratch_.Reset();
			skinScratchBytes_ = 0;
			any = true;
		}
		for (auto& context : contexts_) {
			if (!context.boneBuffer)
				continue;
			if (context.boneMapped)
				context.boneBuffer->Unmap(0, nullptr);
			// The context may still be in flight: the buffer goes with its fence.
			Retire(context.boneBuffer, context.boneCapacity, &skinStats_.boneBytes);
			context.boneBuffer.Reset();
			context.boneMapped = nullptr;
			context.boneCapacity = 0;
			any = true;
		}
		skinUploadQueue_.clear();
		skinStats_.pending = 0;
		if (any)
			logger::info("[RayTracing] Characters: buffers released");
	}

	void Backend::UploadSkinShapes(ID3D12GraphicsCommandList4* a_list, float a_cpuMs)
	{
		if (skinUploadQueue_.empty())
			return;
		// (46g) Each shape takes four pool ranges (rest + index, skinned vertices, BLAS) and one upload range: no
		// resource creation on the render thread (four committed resources per shape were the 40-125 ms frames after
		// a transition, with the old "at least one shape" rule). A CPU budget bounds the copies; a shape whose ranges
		// do not fit waits at the front of the queue for the pools' next block.
		LARGE_INTEGER frequency, start;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);
		const auto deadline = start.QuadPart + static_cast<std::int64_t>(static_cast<double>(frequency.QuadPart) * a_cpuMs / 1000.0);
		struct Pending
		{
			SkinHandle handle;
			ArenaAlloc upload;
			std::uint64_t indexOffset, uvOffset;  // inside the upload range
		};
		std::vector<Pending> batch;
		std::uint32_t batchVertices = 0;
		while (!skinUploadQueue_.empty() && batch.size() < kSkinUploadsPerFrame && (batch.empty() || batchVertices < kSkinUploadVerticesPerFrame)) {
			if (!batch.empty()) {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				if (now.QuadPart > deadline)
					break;
			}
			const SkinHandle handle = skinUploadQueue_.front();
			if (handle >= skins_.size() || !skins_[handle].live || skins_[handle].uploaded || !skins_[handle].geometry) {
				skinUploadQueue_.pop_front();
				continue;
			}
			auto& shape = skins_[handle];
			const auto& geometry = *shape.geometry;
			Pending entry{ handle, {}, 0, 0 };
			entry.indexOffset = AlignUp(geometry.vertices.size() * sizeof(SkinVertex), 256);
			std::uint64_t uploadBytes = entry.indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
			if (!geometry.Opaque()) {
				entry.uvOffset = AlignUp(uploadBytes, 256);
				uploadBytes = entry.uvOffset + geometry.alphaUV.size() * sizeof(std::uint32_t);
			}
			ArenaAlloc rest, index, output, blas;
			// (46m) A merged shape has no BLAS of its own (its character's merged BLAS holds it).
			const bool ok = skinStaticPool_.Allocate(shape.restBytes, rest) && skinStaticPool_.Allocate(shape.indexBytes, index) &&
			                skinOutputPool_.Allocate(shape.outputBytes, output) && (shape.grouped || asPool_.Allocate(shape.blasBytes, blas)) &&
			                uploadPool_.Allocate(uploadBytes, entry.upload);
			if (!ok) {
				// Nothing recorded with them: back at once.
				skinStaticPool_.Free(rest, 0);
				skinStaticPool_.Free(index, 0);
				skinOutputPool_.Free(output, 0);
				asPool_.Free(blas, 0);
				uploadPool_.Free(entry.upload, 0);
				++stats_.pools.allocFailsLastFrame;
				for (auto* pool : { &skinStaticPool_, &skinOutputPool_, &asPool_, &uploadPool_ }) {
					if (pool->failed) {
						SetSkinFault(std::format("could not grow the {} pool for a skinned shape ({} vertices, {} triangles, {:.1f} MB)", pool->label,
							shape.vertices, shape.triangles, static_cast<double>(shape.Bytes()) / (1024.0 * 1024.0)));
						break;
					}
				}
				break;
			}
			std::byte* mapped = uploadPool_.Mapped(entry.upload);
			std::memcpy(mapped, geometry.vertices.data(), geometry.vertices.size() * sizeof(SkinVertex));
			std::memcpy(mapped + entry.indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			if (!geometry.Opaque())
				std::memcpy(mapped + entry.uvOffset, geometry.alphaUV.data(), geometry.alphaUV.size() * sizeof(std::uint32_t));
			shape.rest = rest;
			shape.index = index;
			shape.output = output;
			shape.blas = blas;
			skinUploadQueue_.pop_front();
			batchVertices += geometry.Vertices();
			batch.push_back(entry);
		}
		skinStats_.pending = static_cast<std::uint32_t>(skinUploadQueue_.size());
		if (batch.empty())
			return;

		// The copies: every touched vertex-pool block to copy dest, the copies, back to shader resource.
		std::vector<std::uint32_t> blocks;
		for (const auto& entry : batch) {
			blocks.push_back(skins_[entry.handle].rest.block);
			blocks.push_back(skins_[entry.handle].index.block);
		}
		std::sort(blocks.begin(), blocks.end());
		blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_COPY_DEST);
		bool anyUv = false;
		for (const auto& entry : batch) {
			auto& shape = skins_[entry.handle];
			const auto& geometry = *shape.geometry;
			ID3D12Resource* upload = uploadPool_.Resource(entry.upload);
			a_list->CopyBufferRegion(skinStaticPool_.Resource(shape.rest), shape.rest.offset, upload, entry.upload.offset,
				geometry.vertices.size() * sizeof(SkinVertex));
			a_list->CopyBufferRegion(skinStaticPool_.Resource(shape.index), shape.index.offset, upload, entry.upload.offset + entry.indexOffset,
				geometry.indices.size() * sizeof(std::uint32_t));
			anyUv = anyUv || (!geometry.Opaque() && shape.uvOffset != UINT32_MAX);
		}
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		if (anyUv && alphaUv_) {
			auto toCopy = SkinTransition(alphaUv_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			a_list->ResourceBarrier(1, &toCopy);
			for (const auto& entry : batch) {
				const auto& shape = skins_[entry.handle];
				if (shape.geometry->Opaque() || shape.uvOffset == UINT32_MAX)
					continue;
				a_list->CopyBufferRegion(alphaUv_.Get(), static_cast<std::uint64_t>(shape.uvOffset) * 8, uploadPool_.Resource(entry.upload),
					entry.upload.offset + entry.uvOffset, static_cast<std::uint64_t>(shape.uvCount) * 8);
			}
			auto back = SkinTransition(alphaUv_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			a_list->ResourceBarrier(1, &back);
		}
		skinRecorded_ = true;
		for (const auto& entry : batch) {
			auto& shape = skins_[entry.handle];
			uploadPool_.Free(entry.upload, RetireValue());  // read by this submission only
			shape.uploaded = true;
			shape.geometry.reset();
			++skinUploadsTotal_;
		}
	}

	void Backend::ReleaseSkinGroup(SkinGroup& a_group)
	{
		// (46m) The merged BLAS may still be read by a submission in flight: back to the pool behind the fence.
		if (a_group.blas.Valid()) {
			asPool_.Free(a_group.blas, RetireValue());
			skinStats_.bytes -= std::min(skinStats_.bytes, a_group.blasBytes);
		}
		a_group.blas = {};
		a_group.blasBytes = 0;
		a_group.serials.clear();
		a_group.built = false;
		a_group.refits = 0;
	}

	void Backend::ReleaseSkinGroups()
	{
		for (auto& group : skinGroups_)
			ReleaseSkinGroup(group);
		skinGroups_.clear();
		skinGroupFrame_.clear();
	}

	void Backend::NoteSkinWork(std::uint64_t a_uploadsBefore)
	{
		// (46p) RecordSkinning just ran: its per-frame counts are this submission's.
		recWork_.skinUploads += static_cast<std::uint32_t>(skinUploadsTotal_ - std::min(skinUploadsTotal_, a_uploadsBefore));
		recWork_.skinBuilds += skinStats_.buildsLastFrame;
		recWork_.skinRefits += skinStats_.refitsLastFrame;
		recWork_.groupBuilds += skinStats_.groupBuildsLastFrame;
		recWork_.groupRefits += skinStats_.groupRefitsLastFrame;
		recWork_.skinDispatches += skinStats_.dispatchesLastFrame;
		recWork_.skinVertices += skinStats_.verticesLastFrame;
	}

	void Backend::RecordSkinning(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, const FrameInput& a_input,
		std::vector<std::uint8_t>& o_ready)
	{
		skinStats_.refitsLastFrame = skinStats_.buildsLastFrame = skinStats_.notReadyLastFrame = skinStats_.reusedLastFrame = 0;
		skinStats_.verticesLastFrame = 0;
		skinStats_.groupsLastFrame = skinStats_.groupBuildsLastFrame = skinStats_.groupRefitsLastFrame = skinStats_.groupKeptLastFrame = 0;
		skinStats_.groupShapesLastFrame = skinStats_.dispatchesLastFrame = 0;
		skinGroupFrame_.clear();
		UploadSkinShapes(a_list, a_input.skinUploadCpuMs > 0.0f ? a_input.skinUploadCpuMs : kPlaySkinUploadCpuMs);
		if (!skinFault_.empty() || failed_) {
			if (!skinGroups_.empty())
				ReleaseSkinGroups();
			return;
		}
		budget_.groupsDeferred = 0;  // (46q)
		budget_.groupsKept = 0;
		if (!a_input.skinDraws || a_input.skinDraws->empty()) {
			if (!skinGroups_.empty())
				ReleaseUnseenSkinGroups();  // (46m) no character drawn: no merged BLAS kept; (46q) with the budget on, after a grace period
			return;
		}
		const auto& draws = *a_input.skinDraws;
		// (45l) Draws that reuse last frame's skinned BLAS (other actors on their off frame, Characters
		// "reduced"): nothing to record, traced as built. (46m) A merged shape's still pose: its skinned vertices
		// are kept (its merged BLAS is ready once the group is, below).
		std::size_t reused = 0;
		for (std::size_t d = 0; d < draws.size(); ++d) {
			const auto& draw = draws[d];
			if (!draw.reuse)
				continue;
			const bool valid = draw.shape < skins_.size() && skins_[draw.shape].live && skins_[draw.shape].uploaded;
			const bool usable = valid && (skins_[draw.shape].grouped ? (draw.group != 0 && skins_[draw.shape].skinned) : skins_[draw.shape].built);
			if (usable) {
				if (!skins_[draw.shape].grouped)
					o_ready[d] = 1;
				++reused;
			} else {
				++skinStats_.notReadyLastFrame;
			}
		}
		skinStats_.reusedLastFrame = static_cast<std::uint32_t>(reused);
		const std::uint64_t boneFloats = a_input.skinBones ? a_input.skinBones->size() : 0;
		const bool bonesOk = boneFloats != 0 && boneFloats <= kSkinMaxBoneFloats && boneFloats % 12 == 0;
		const std::uint64_t submission = fenceValue_ + 1;  // this frame's done value: unique per recorded frame

		struct Op
		{
			std::size_t draw;
			bool build;
			std::uint64_t scratchOffset;
		};
		std::vector<Op> ops;                    // shapes with a BLAS of their own: skinned, then built / refit
		std::vector<std::size_t> groupSkins;    // (46m) merged shapes skinned this frame (their group's BLAS is built / refit below)
		ops.reserve(draws.size());
		groupSkins.reserve(draws.size());
		std::uint32_t builds = 0, rebuilds = 0;
		std::uint64_t scratchTotal = 0;
		for (std::size_t d = 0; d < draws.size(); ++d) {
			const auto& draw = draws[d];
			if (draw.reuse)
				continue;
			if (!bonesOk || draw.shape >= skins_.size() || !skins_[draw.shape].live || !skins_[draw.shape].uploaded) {
				++skinStats_.notReadyLastFrame;
				continue;
			}
			auto& shape = skins_[draw.shape];
			if (shape.lastSubmission == submission)
				continue;  // the same shape twice: skinned once
			if ((static_cast<std::uint64_t>(draw.boneOffset) + shape.boneCount) * 12 > boneFloats) {
				++skinStats_.notReadyLastFrame;
				continue;
			}
			if (shape.grouped) {
				// (46m) Only traced inside its character's merged BLAS.
				if (draw.group == 0) {
					++skinStats_.notReadyLastFrame;
					continue;
				}
				groupSkins.push_back(d);
				shape.lastSubmission = submission;
				continue;
			}
			bool build = !shape.built;
			if (build) {
				if (builds >= kSkinBuildsPerFrame) {
					++skinStats_.notReadyLastFrame;
					continue;
				}
				++builds;
			} else if (shape.refits >= kSkinRebuildFrames && rebuilds < kSkinRebuildsPerFrame) {
				build = true;
				++rebuilds;
			}
			ops.push_back(Op{ d, build, scratchTotal });
			scratchTotal += AlignUp(build ? shape.buildScratch : shape.updateScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			shape.lastSubmission = submission;
		}

		// (46m) Merged BLASes. Members: the group's uploaded shapes skinned now, or before with a still pose (their skinned
		// vertices are this frame's). Built when the member list changed (by shape serial: handles are reused), refit in place
		// when a member was skinned now, kept as it is otherwise. A member list of another origin than the group's first
		// member (cannot happen: Characters rebases) is left out.
		struct GroupOp
		{
			std::size_t group;
			bool build, update;
			std::uint64_t scratchOffset;
			std::size_t firstMember, memberCount;
		};
		std::vector<GroupOp> groupOps;
		// (46q) Builds budgeted by GPU time: merged builds this frame and the triangles the whole budget allows.
		std::uint32_t groupBuildsNow = 0;
		const double groupPerTri = msPerTriEma_ > 0.0 ? msPerTriEma_ : 8.0e-6;
		const std::uint64_t groupTriBudget = static_cast<std::uint64_t>(std::max(16384.0, static_cast<double>(kBudgetGpuMs) / groupPerTri));
		for (auto& group : skinGroups_)
			group.seen = false;
		skinGroupDraws_.clear();
		skinGroupMembers_.clear();
		for (std::size_t d = 0; d < draws.size(); ++d)
			if (draws[d].group != 0)
				skinGroupDraws_.emplace_back(draws[d].group, static_cast<std::uint32_t>(d));
		std::sort(skinGroupDraws_.begin(), skinGroupDraws_.end());
		auto groupGeometry = [&](const SkinShape& a_shape) {
			D3D12_RAYTRACING_GEOMETRY_DESC desc{};
			desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			desc.Triangles.VertexBuffer.StartAddress = skinOutputPool_.Address(a_shape.output);
			desc.Triangles.VertexBuffer.StrideInBytes = kSkinOutputStride;
			desc.Triangles.VertexCount = a_shape.vertices;
			desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			desc.Triangles.IndexBuffer = skinStaticPool_.Address(a_shape.index);
			desc.Triangles.IndexCount = a_shape.triangles * 3;
			desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			return desc;
		};
		for (std::size_t i = 0; i < skinGroupDraws_.size();) {
			const std::uint32_t key = skinGroupDraws_[i].first;
			std::size_t end = i;
			while (end < skinGroupDraws_.size() && skinGroupDraws_[end].first == key)
				++end;
			std::size_t g = 0;
			while (g < skinGroups_.size() && skinGroups_[g].key != key)
				++g;
			if (g == skinGroups_.size()) {
				skinGroups_.emplace_back();
				skinGroups_.back().key = key;
			}
			skinGroups_[g].seen = true;
			const std::size_t first = skinGroupMembers_.size();
			const SkinDraw* lead = nullptr;
			bool anySkinned = false;
			for (std::size_t k = i; k < end; ++k) {
				const std::uint32_t d = skinGroupDraws_[k].second;
				const auto& draw = draws[d];
				if (draw.shape >= skins_.size())
					continue;
				const auto& shape = skins_[draw.shape];
				if (!shape.live || !shape.uploaded || !shape.grouped)
					continue;
				const bool skinnedNow = !draw.reuse && shape.lastSubmission == submission;
				if (!skinnedNow && !(draw.reuse && shape.skinned))
					continue;  // counted above (not ready)
				if (std::find_if(skinGroupMembers_.begin() + first, skinGroupMembers_.end(), [&](std::uint32_t a_member) { return draws[a_member].shape == draw.shape; }) !=
					skinGroupMembers_.end())
					continue;  // the same shape twice: one geometry
				if (lead && (draw.x != lead->x || draw.y != lead->y || draw.z != lead->z)) {
					++skinStats_.notReadyLastFrame;
					continue;
				}
				if (!lead)
					lead = &draw;
				anySkinned = anySkinned || skinnedNow;
				skinGroupMembers_.push_back(d);
			}
			i = end;
			const std::size_t count = skinGroupMembers_.size() - first;
			if (count == 0)
				continue;
			// By serial: a member that only moved in the character's list (equip changes the order) keeps the BLAS.
			std::sort(skinGroupMembers_.begin() + static_cast<std::ptrdiff_t>(first), skinGroupMembers_.end(),
				[&](std::uint32_t a_lhs, std::uint32_t a_rhs) { return skins_[draws[a_lhs].shape].serial < skins_[draws[a_rhs].shape].serial; });
			auto& group = skinGroups_[g];
			bool same = group.built && group.serials.size() == count;
			for (std::size_t m = 0; same && m < count; ++m)
				same = skins_[draws[skinGroupMembers_[first + m]].shape].serial == group.serials[m];
			bool build = !same;
			if (!build && anySkinned && group.refits >= kSkinRebuildFrames && rebuilds < kSkinRebuildsPerFrame) {
				build = true;  // periodic rebuild (a refit loosens the BVH as the pose drifts)
				++rebuilds;
			}
			if (build) {
				// (46q) Builds budgeted by GPU time: at most kGroupBuildCap merged builds a frame, within the GPU-time budget left after the
				// static builds (the first one always goes). A periodic rebuild put off is a refit; a changed member list waits (its
				// character is not traced this frame, as a shape over kSkinBuildsPerFrame).
				std::uint64_t triangles = 0;
				for (std::size_t m = 0; m < count; ++m)
					triangles += skins_[draws[skinGroupMembers_[first + m]].shape].triangles;
				const bool fits = groupBuildsNow == 0 || (groupBuildsNow < kGroupBuildCap && frameTrisUsed_ + triangles <= groupTriBudget);
				if (fits) {
					++groupBuildsNow;
					frameTrisUsed_ += triangles;
				} else {
					++budget_.groupsDeferred;
					if (!same) {
						skinStats_.notReadyLastFrame += static_cast<std::uint32_t>(count);
						skinGroupMembers_.resize(first);
						continue;
					}
					build = false;
				}
			}
			const bool update = !build && anySkinned;
			if (build) {
				skinGroupDescs_.clear();
				for (std::size_t m = 0; m < count; ++m)
					skinGroupDescs_.push_back(groupGeometry(skins_[draws[skinGroupMembers_[first + m]].shape]));
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
				inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				inputs.NumDescs = static_cast<UINT>(count);
				inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				inputs.pGeometryDescs = skinGroupDescs_.data();
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
				device_->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
				const std::uint64_t need = AlignUp(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
				if (need == 0 || info.ScratchDataSizeInBytes == 0) {
					skinStats_.notReadyLastFrame += static_cast<std::uint32_t>(count);
					skinGroupMembers_.resize(first);
					continue;
				}
				if (!group.blas.Valid() || group.blasBytes < need) {
					const std::uint64_t bytes = AlignUp(static_cast<std::uint64_t>(static_cast<double>(need) * kSkinGroupSlack),
						D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
					ArenaAlloc fresh;
					if (!asPool_.Allocate(bytes, fresh)) {
						// The pool grows on its thread: tried again next frame (the old BLAS, if any, stays allocated).
						++stats_.pools.allocFailsLastFrame;
						skinStats_.notReadyLastFrame += static_cast<std::uint32_t>(count);
						skinGroupMembers_.resize(first);
						continue;
					}
					if (group.blas.Valid()) {
						asPool_.Free(group.blas, RetireValue());
						skinStats_.bytes -= std::min(skinStats_.bytes, group.blasBytes);
					}
					group.blas = fresh;
					group.blasBytes = bytes;
					group.built = false;
					skinStats_.bytes += bytes;
				}
				group.buildScratch = AlignUp(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
				group.updateScratch =
					AlignUp(std::max<std::uint64_t>(info.UpdateScratchDataSizeInBytes, 256), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			}
			groupOps.push_back(GroupOp{ g, build, update, scratchTotal, first, count });
			if (build || update)
				scratchTotal += AlignUp(build ? group.buildScratch : group.updateScratch, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		}

		auto finishGroups = [&]() {
			// Groups no character drew this frame: their BLAS goes (behind the fence). (46q) Only once it was not drawn for the
			// grace period (a character out of view for a moment keeps it: no rebuild when seen again).
			const std::int64_t now = Util::FrameTrace::Now();
			const std::int64_t grace = static_cast<std::int64_t>(static_cast<double>(kGroupGraceMs) * QpcPerMs());
			for (std::size_t g = 0; g < skinGroups_.size();) {
				if (skinGroups_[g].seen) {
					skinGroups_[g].lastSeenQpc = now;
					++g;
					continue;
				}
				if (skinGroups_[g].lastSeenQpc != 0 && now - skinGroups_[g].lastSeenQpc < grace) {
					++budget_.groupsKept;
					++g;
					continue;
				}
				ReleaseSkinGroup(skinGroups_[g]);
				skinGroups_[g] = std::move(skinGroups_.back());
				skinGroups_.pop_back();
			}
		};
		auto dropRecorded = [&]() {
			for (const auto& op : ops)
				skins_[draws[op.draw].shape].lastSubmission = 0;
			for (const auto d : groupSkins)
				skins_[draws[d].shape].lastSubmission = 0;
		};

		const bool skinning = !ops.empty() || !groupSkins.empty();
		if (skinning) {
			// This frame's bone matrices into the context's own upload buffer (the context is free).
			const std::uint64_t boneBytes = boneFloats * sizeof(float);
			if (!a_context.boneBuffer || a_context.boneCapacity < boneBytes) {
				if (a_context.boneBuffer) {
					if (a_context.boneMapped)
						a_context.boneBuffer->Unmap(0, nullptr);
					skinStats_.boneBytes -= std::min(skinStats_.boneBytes, a_context.boneCapacity);
					a_context.boneBuffer.Reset();
					a_context.boneMapped = nullptr;
					a_context.boneCapacity = 0;
				}
				const std::uint64_t capacity = std::bit_ceil(std::max<std::uint64_t>(boneBytes, 64 * 1024));
				a_context.boneBuffer = CreateBuffer(capacity, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
				if (!a_context.boneBuffer || FAILED(a_context.boneBuffer->Map(0, nullptr, &a_context.boneMapped)) || !a_context.boneMapped) {
					a_context.boneBuffer.Reset();
					a_context.boneMapped = nullptr;
					dropRecorded();
					SetSkinFault("could not create the bone matrix buffer");
					return;
				}
				a_context.boneBuffer->SetName(L"CS RayTracing character bones");
				a_context.boneCapacity = capacity;
				skinStats_.boneBytes += capacity;
			}
			std::memcpy(a_context.boneMapped, a_input.skinBones->data(), boneBytes);
		}

		if (scratchTotal > 0 && (!skinScratch_ || skinScratchBytes_ < scratchTotal)) {
			// Scratch: a region per build / refit, so they need no barriers between them.
			if (skinScratch_)
				Retire(skinScratch_, skinScratchBytes_, &skinStats_.scratchBytes);
			skinScratch_.Reset();
			skinScratchBytes_ = 0;
			const std::uint64_t bytes = AlignUp(std::max<std::uint64_t>(scratchTotal + scratchTotal / 4, 4ull << 20), 64 * 1024);
			skinScratch_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true, Util::FrameTrace::VramOwner::RtScratch);
			if (!skinScratch_) {
				dropRecorded();
				SetSkinFault(std::format("could not create a {:.1f} MB scratch buffer", static_cast<double>(bytes) / (1024.0 * 1024.0)));
				return;
			}
			skinScratch_->SetName(L"CS RayTracing character scratch");
			skinScratchBytes_ = bytes;
			skinStats_.scratchBytes += bytes;
		}

		if (skinning) {
			skinRecorded_ = true;
			// Skinning. (46g) The skinned-vertex pool blocks these shapes live in go to UAV as a whole (the other shapes in
			// them are not read meanwhile: a BLAS holds its own copy of the vertices; the builds / refits below, merged ones
			// too, read the skinned vertices only after the blocks went back).
			std::vector<std::uint32_t> outputBlocks;
			outputBlocks.reserve(ops.size() + groupSkins.size());
			for (const auto& op : ops)
				outputBlocks.push_back(skins_[draws[op.draw].shape].output.block);
			for (const auto d : groupSkins)
				outputBlocks.push_back(skins_[draws[d].shape].output.block);
			std::sort(outputBlocks.begin(), outputBlocks.end());
			outputBlocks.erase(std::unique(outputBlocks.begin(), outputBlocks.end()), outputBlocks.end());
			TransitionBlocks(a_list, skinOutputPool_, outputBlocks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			a_list->SetComputeRootSignature(skinRootSignature_.Get());
			a_list->SetPipelineState(skinPipeline_.Get());
			a_list->SetComputeRootShaderResourceView(2, a_context.boneBuffer->GetGPUVirtualAddress());  // (46m) once: the same for every shape
			auto dispatch = [&](std::size_t a_draw) {
				const auto& draw = draws[a_draw];
				auto& shape = skins_[draw.shape];
				const std::uint32_t constants[4] = { shape.vertices, draw.boneOffset, shape.boneCount, 0 };
				a_list->SetComputeRoot32BitConstants(0, 4, constants, 0);
				a_list->SetComputeRootShaderResourceView(1, skinStaticPool_.Address(shape.rest));
				a_list->SetComputeRootUnorderedAccessView(3, skinOutputPool_.Address(shape.output));
				a_list->Dispatch((shape.vertices + kSkinGroupSize - 1) / kSkinGroupSize, 1, 1);
				shape.skinned = true;
				skinStats_.verticesLastFrame += shape.vertices;
				++skinStats_.dispatchesLastFrame;
			};
			for (const auto& op : ops)
				dispatch(op.draw);
			for (const auto d : groupSkins)
				dispatch(d);
			TransitionBlocks(a_list, skinOutputPool_, outputBlocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		}

		// BLAS builds and in-place refits (shapes with a BLAS of their own).
		const D3D12_GPU_VIRTUAL_ADDRESS scratch = skinScratch_ ? skinScratch_->GetGPUVirtualAddress() : 0;
		for (const auto& op : ops) {
			auto& shape = skins_[draws[op.draw].shape];
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometryDesc.Flags = shape.uvOffset == UINT32_MAX ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			geometryDesc.Triangles.VertexBuffer.StartAddress = skinOutputPool_.Address(shape.output);
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = kSkinOutputStride;
			geometryDesc.Triangles.VertexCount = shape.vertices;
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexBuffer = skinStaticPool_.Address(shape.index);
			geometryDesc.Triangles.IndexCount = shape.triangles * 3;
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
			buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			buildDesc.Inputs.NumDescs = 1;
			buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			buildDesc.Inputs.pGeometryDescs = &geometryDesc;
			buildDesc.DestAccelerationStructureData = asPool_.Address(shape.blas);
			buildDesc.ScratchAccelerationStructureData = scratch + op.scratchOffset;
			if (!op.build) {
				buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
				buildDesc.SourceAccelerationStructureData = buildDesc.DestAccelerationStructureData;  // in place
			}
			a_list->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
			if (op.build) {
				shape.built = true;
				shape.refits = 0;
				++skinStats_.buildsLastFrame;
				++skinStats_.buildsTotal;
			} else {
				++shape.refits;
				++skinStats_.refitsLastFrame;
			}
			o_ready[op.draw] = 1;
		}

		// (46m) Merged BLASes: built / refit in place (one call per character), then this frame's instances.
		for (const auto& op : groupOps) {
			auto& group = skinGroups_[op.group];
			if (op.build || op.update) {
				skinRecorded_ = true;
				skinGroupDescs_.clear();
				for (std::size_t m = 0; m < op.memberCount; ++m)
					skinGroupDescs_.push_back(groupGeometry(skins_[draws[skinGroupMembers_[op.firstMember + m]].shape]));
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
				buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				buildDesc.Inputs.NumDescs = static_cast<UINT>(op.memberCount);
				buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				buildDesc.Inputs.pGeometryDescs = skinGroupDescs_.data();
				buildDesc.DestAccelerationStructureData = asPool_.Address(group.blas);
				buildDesc.ScratchAccelerationStructureData = scratch + op.scratchOffset;
				if (op.update) {
					buildDesc.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
					buildDesc.SourceAccelerationStructureData = buildDesc.DestAccelerationStructureData;  // in place
				}
				a_list->BuildRaytracingAccelerationStructure(&buildDesc, 0, nullptr);
				if (op.build) {
					group.built = true;
					group.refits = 0;
					group.serials.clear();
					for (std::size_t m = 0; m < op.memberCount; ++m) {
						auto& shape = skins_[draws[skinGroupMembers_[op.firstMember + m]].shape];
						group.serials.push_back(shape.serial);
						shape.built = true;  // traced from now on (the caster filter may take it out of the shadow map)
						recWork_.groupTriangles += shape.triangles;  // (46p)
					}
					++skinStats_.groupBuildsLastFrame;
					++skinStats_.buildsTotal;
				} else {
					++group.refits;
					++skinStats_.groupRefitsLastFrame;
				}
			} else {
				++skinStats_.groupKeptLastFrame;
			}
			for (std::size_t m = 0; m < op.memberCount; ++m)
				o_ready[skinGroupMembers_[op.firstMember + m]] = 1;
			const auto& lead = draws[skinGroupMembers_[op.firstMember]];
			SkinGroupInstance instance;
			instance.blas = asPool_.Address(group.blas);
			instance.x = lead.x;
			instance.y = lead.y;
			instance.z = lead.z;
			instance.npc = lead.npc;
			instance.owner = lead.owner;
			instance.members = static_cast<std::uint32_t>(op.memberCount);
			skinGroupFrame_.push_back(instance);
			++skinStats_.groupsLastFrame;
			skinStats_.groupShapesLastFrame += instance.members;
		}
		finishGroups();
	}
}
