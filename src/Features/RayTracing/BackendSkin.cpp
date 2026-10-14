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

		SkinShape shape;
		shape.live = true;
		shape.vertices = vertices;
		shape.triangles = triangles;
		shape.boneCount = geometry.boneCount;
		shape.restBytes = AlignUp(static_cast<std::uint64_t>(vertices) * kSkinVertexBytes, 256);
		shape.indexBytes = AlignUp(static_cast<std::uint64_t>(triangles) * 3 * sizeof(std::uint32_t), 256);
		shape.outputBytes = AlignUp(static_cast<std::uint64_t>(vertices) * kSkinOutputStride, 256);
		shape.blasBytes = AlignUp(info.ResultDataMaxSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		shape.buildScratch = AlignUp(info.ScratchDataSizeInBytes, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
		shape.updateScratch = AlignUp(std::max<std::uint64_t>(info.UpdateScratchDataSizeInBytes, 256), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
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
		for (ComPtr<ID3D12Resource>* resource : { std::addressof(shape.rest), std::addressof(shape.index), std::addressof(shape.output), std::addressof(shape.blas) }) {
			if (*resource)
				Retire(*resource, 0, nullptr);
		}
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

	void Backend::UploadSkinShapes(ID3D12GraphicsCommandList4* a_list)
	{
		if (skinUploadQueue_.empty())
			return;
		// This frame's batch, with one upload buffer for all of it.
		struct Pending
		{
			SkinHandle handle;
			std::uint64_t restOffset, indexOffset, uvOffset;
		};
		std::vector<Pending> batch;
		std::uint64_t uploadBytes = 0;
		std::uint32_t batchVertices = 0;
		while (!skinUploadQueue_.empty() && batch.size() < kSkinUploadsPerFrame && (batch.empty() || batchVertices < kSkinUploadVerticesPerFrame)) {
			const SkinHandle handle = skinUploadQueue_.front();
			skinUploadQueue_.pop_front();
			if (handle >= skins_.size() || !skins_[handle].live || skins_[handle].uploaded || !skins_[handle].geometry)
				continue;
			const auto& geometry = *skins_[handle].geometry;
			Pending entry{ handle, AlignUp(uploadBytes, 256), 0, 0 };
			entry.indexOffset = AlignUp(entry.restOffset + geometry.vertices.size() * sizeof(SkinVertex), 256);
			uploadBytes = entry.indexOffset + geometry.indices.size() * sizeof(std::uint32_t);
			if (!geometry.Opaque()) {
				entry.uvOffset = AlignUp(uploadBytes, 256);
				uploadBytes = entry.uvOffset + geometry.alphaUV.size() * sizeof(std::uint32_t);
			}
			batchVertices += geometry.Vertices();
			batch.push_back(entry);
		}
		skinStats_.pending = static_cast<std::uint32_t>(skinUploadQueue_.size());
		if (batch.empty())
			return;
		auto requeue = [&](std::size_t a_from) {
			for (std::size_t i = batch.size(); i-- > a_from;)
				skinUploadQueue_.push_front(batch[i].handle);
			skinStats_.pending = static_cast<std::uint32_t>(skinUploadQueue_.size());
		};

		auto upload = CreateBuffer(uploadBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		std::byte* mapped = nullptr;
		if (!upload || FAILED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))) || !mapped) {
			requeue(0);
			SetSkinFault(std::format("could not create a {:.1f} MB upload buffer", static_cast<double>(uploadBytes) / (1024.0 * 1024.0)));
			return;
		}
		for (const auto& entry : batch) {
			const auto& geometry = *skins_[entry.handle].geometry;
			std::memcpy(mapped + entry.restOffset, geometry.vertices.data(), geometry.vertices.size() * sizeof(SkinVertex));
			std::memcpy(mapped + entry.indexOffset, geometry.indices.data(), geometry.indices.size() * sizeof(std::uint32_t));
			if (!geometry.Opaque())
				std::memcpy(mapped + entry.uvOffset, geometry.alphaUV.data(), geometry.alphaUV.size() * sizeof(std::uint32_t));
		}
		upload->Unmap(0, nullptr);
		upload->SetName(L"CS RayTracing character upload");
		Retire(upload, uploadBytes, &stats_.uploadBytes);
		stats_.uploadBytes += uploadBytes;

		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(batch.size() * 2);
		bool anyUv = false;
		// (45k) Four committed resources per shape are created here on the render thread (the 5-9 ms
		// rt_exec_skin_ms frames when characters came into range): past the CPU budget the rest of the
		// batch waits for the next frame (at least one shape per frame).
		LARGE_INTEGER frequency, start;
		QueryPerformanceFrequency(&frequency);
		QueryPerformanceCounter(&start);
		const auto deadline = start.QuadPart + static_cast<std::int64_t>(static_cast<double>(frequency.QuadPart) * kSkinUploadBudgetMs / 1000.0);
		for (std::size_t b = 0; b < batch.size(); ++b) {
			if (b > 0) {
				LARGE_INTEGER now;
				QueryPerformanceCounter(&now);
				if (now.QuadPart > deadline) {
					requeue(b);
					batch.resize(b);
					break;
				}
			}
			auto& shape = skins_[batch[b].handle];
			shape.rest = CreateBuffer(shape.restBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false);
			shape.index = CreateBuffer(shape.indexBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false);
			shape.output = CreateBuffer(shape.outputBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
			shape.blas = CreateBuffer(shape.blasBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, true);
			if (!shape.rest || !shape.index || !shape.output || !shape.blas) {
				// Nothing recorded for this one yet: its partial buffers can go at once.
				shape.rest.Reset();
				shape.index.Reset();
				shape.output.Reset();
				shape.blas.Reset();
				requeue(b);
				SetSkinFault(std::format("could not create the buffers of a skinned shape ({} vertices, {} triangles, {:.1f} MB)", shape.vertices,
					shape.triangles, static_cast<double>(shape.Bytes()) / (1024.0 * 1024.0)));
				break;
			}
			shape.rest->SetName(L"CS RayTracing character rest vertices");
			shape.index->SetName(L"CS RayTracing character indices");
			shape.output->SetName(L"CS RayTracing character skinned vertices");
			shape.blas->SetName(L"CS RayTracing character BLAS");
			const auto& geometry = *shape.geometry;
			skinRecorded_ = true;
			a_list->CopyBufferRegion(shape.rest.Get(), 0, upload.Get(), batch[b].restOffset, geometry.vertices.size() * sizeof(SkinVertex));
			a_list->CopyBufferRegion(shape.index.Get(), 0, upload.Get(), batch[b].indexOffset, geometry.indices.size() * sizeof(std::uint32_t));
			barriers.push_back(SkinTransition(shape.rest.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			barriers.push_back(SkinTransition(shape.index.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			anyUv = anyUv || (!geometry.Opaque() && shape.uvOffset != UINT32_MAX);
			shape.uploaded = true;
			shape.outputInUav = true;
		}
		if (anyUv && alphaUv_) {
			auto toCopy = SkinTransition(alphaUv_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			a_list->ResourceBarrier(1, &toCopy);
			for (const auto& entry : batch) {
				const auto& shape = skins_[entry.handle];
				if (!shape.uploaded || !shape.geometry || shape.geometry->Opaque() || shape.uvOffset == UINT32_MAX)
					continue;
				a_list->CopyBufferRegion(alphaUv_.Get(), static_cast<std::uint64_t>(shape.uvOffset) * 8, upload.Get(), entry.uvOffset,
					static_cast<std::uint64_t>(shape.uvCount) * 8);
			}
			barriers.push_back(SkinTransition(alphaUv_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
		}
		if (!barriers.empty())
			a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		for (const auto& entry : batch) {
			auto& shape = skins_[entry.handle];
			if (shape.uploaded)
				shape.geometry.reset();
		}
	}

	void Backend::RecordSkinning(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, const FrameInput& a_input,
		std::vector<std::uint8_t>& o_ready)
	{
		skinStats_.refitsLastFrame = skinStats_.buildsLastFrame = skinStats_.notReadyLastFrame = 0;
		skinStats_.verticesLastFrame = 0;
		UploadSkinShapes(a_list);
		if (!skinFault_.empty() || failed_ || !a_input.skinDraws || a_input.skinDraws->empty())
			return;
		const auto& draws = *a_input.skinDraws;
		const std::uint64_t boneFloats = a_input.skinBones ? a_input.skinBones->size() : 0;
		if (boneFloats == 0 || boneFloats > kSkinMaxBoneFloats || boneFloats % 12 != 0) {
			skinStats_.notReadyLastFrame = static_cast<std::uint32_t>(draws.size());
			return;
		}
		const std::uint64_t submission = fenceValue_ + 1;  // this frame's done value: unique per recorded frame

		struct Op
		{
			std::size_t draw;
			bool build;
			std::uint64_t scratchOffset;
		};
		std::vector<Op> ops;
		ops.reserve(draws.size());
		std::uint32_t builds = 0, rebuilds = 0;
		std::uint64_t scratchTotal = 0;
		for (std::size_t d = 0; d < draws.size(); ++d) {
			const auto& draw = draws[d];
			if (draw.shape >= skins_.size() || !skins_[draw.shape].live || !skins_[draw.shape].uploaded) {
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
		if (ops.empty())
			return;

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
				for (const auto& op : ops)
					skins_[draws[op.draw].shape].lastSubmission = 0;
				SetSkinFault("could not create the bone matrix buffer");
				return;
			}
			a_context.boneBuffer->SetName(L"CS RayTracing character bones");
			a_context.boneCapacity = capacity;
			skinStats_.boneBytes += capacity;
		}
		std::memcpy(a_context.boneMapped, a_input.skinBones->data(), boneBytes);

		// Scratch: a region per build / refit, so they need no barriers between them.
		if (!skinScratch_ || skinScratchBytes_ < scratchTotal) {
			if (skinScratch_)
				Retire(skinScratch_, skinScratchBytes_, &skinStats_.scratchBytes);
			skinScratch_.Reset();
			skinScratchBytes_ = 0;
			const std::uint64_t bytes = AlignUp(std::max<std::uint64_t>(scratchTotal + scratchTotal / 4, 4ull << 20), 64 * 1024);
			skinScratch_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
			if (!skinScratch_) {
				for (const auto& op : ops)
					skins_[draws[op.draw].shape].lastSubmission = 0;
				SetSkinFault(std::format("could not create a {:.1f} MB scratch buffer", static_cast<double>(bytes) / (1024.0 * 1024.0)));
				return;
			}
			skinScratch_->SetName(L"CS RayTracing character scratch");
			skinScratchBytes_ = bytes;
			skinStats_.scratchBytes += bytes;
		}

		skinRecorded_ = true;
		// Skinning.
		std::vector<D3D12_RESOURCE_BARRIER> barriers;
		barriers.reserve(ops.size());
		for (const auto& op : ops) {
			auto& shape = skins_[draws[op.draw].shape];
			if (!shape.outputInUav)
				barriers.push_back(SkinTransition(shape.output.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
		}
		if (!barriers.empty())
			a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
		a_list->SetComputeRootSignature(skinRootSignature_.Get());
		a_list->SetPipelineState(skinPipeline_.Get());
		const D3D12_GPU_VIRTUAL_ADDRESS bones = a_context.boneBuffer->GetGPUVirtualAddress();
		for (const auto& op : ops) {
			const auto& draw = draws[op.draw];
			const auto& shape = skins_[draw.shape];
			const std::uint32_t constants[4] = { shape.vertices, draw.boneOffset, shape.boneCount, 0 };
			a_list->SetComputeRoot32BitConstants(0, 4, constants, 0);
			a_list->SetComputeRootShaderResourceView(1, shape.rest->GetGPUVirtualAddress());
			a_list->SetComputeRootShaderResourceView(2, bones);
			a_list->SetComputeRootUnorderedAccessView(3, shape.output->GetGPUVirtualAddress());
			a_list->Dispatch((shape.vertices + kSkinGroupSize - 1) / kSkinGroupSize, 1, 1);
		}
		barriers.clear();
		for (const auto& op : ops) {
			auto& shape = skins_[draws[op.draw].shape];
			barriers.push_back(SkinTransition(shape.output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
			shape.outputInUav = false;
		}
		a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

		// BLAS builds and in-place refits.
		const D3D12_GPU_VIRTUAL_ADDRESS scratch = skinScratch_->GetGPUVirtualAddress();
		for (const auto& op : ops) {
			auto& shape = skins_[draws[op.draw].shape];
			D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc{};
			geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			geometryDesc.Flags = shape.uvOffset == UINT32_MAX ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			geometryDesc.Triangles.VertexBuffer.StartAddress = shape.output->GetGPUVirtualAddress();
			geometryDesc.Triangles.VertexBuffer.StrideInBytes = kSkinOutputStride;
			geometryDesc.Triangles.VertexCount = shape.vertices;
			geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			geometryDesc.Triangles.IndexBuffer = shape.index->GetGPUVirtualAddress();
			geometryDesc.Triangles.IndexCount = shape.triangles * 3;
			geometryDesc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC buildDesc{};
			buildDesc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			buildDesc.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			buildDesc.Inputs.NumDescs = 1;
			buildDesc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			buildDesc.Inputs.pGeometryDescs = &geometryDesc;
			buildDesc.DestAccelerationStructureData = shape.blas->GetGPUVirtualAddress();
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
			skinStats_.verticesLastFrame += shape.vertices;
			o_ready[op.draw] = 1;
		}
	}
}
