// (48f) RT reflections, Capture Materials: the data half of the material step (the shading half is 48g).
//
// D3D12: per-triangle attributes (ChunkGeometry::attributes, 16 bytes each: three corner texcoords or terrain layer weights, material id,
// octahedral face normal; MaterialAttributes.h) in one buffer, a range per BLAS for its life, uploaded with its build (BuildQueued) inside
// the BLAS upload budget; a per-context instance table written with the TLAS (ExecuteFrame) tells TraceReflectCS (RT_MATERIAL) where an
// instance's attributes start, its triangle count, its own material and its type.
//
// D3D11: a material = a game texture (+ texcoord transform), or a terrain quadrant's six layer textures (+ its average vertex colour). Every
// distinct game texture gets one 128x128 RGBA8 copy with all mips in a layer of a texture-copy page (Texture2DArray of 256 layers), made by
// MaterialCopyCS within a per-frame budget (more in the black screen / warm-up after a loading screen; never while the loading screen itself
// is presented: it owns the immediate context from another thread). Pages are created on a worker thread (the D3D11 device is free-threaded)
// when the layers run out: never a creation on the render thread, never per frame. The material table (StructuredBuffer, 48 bytes per id) is
// rewritten when something changed. Ids are recycled only kMaterialRetireFrames after their last user went (a frame's hit record may name it).
//
// Nothing of this exists while RT Reflections is Off or Capture Materials is off; what was made goes once nothing uses it.

#include "Backend.h"

#include "Utils/D3D.h"

#include <bit>

namespace RayTracingRT
{
	namespace
	{
		std::int64_t MatQpc()
		{
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			return now.QuadPart;
		}

		double MatQpcToMs(std::int64_t a_ticks)
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return static_cast<double>(a_ticks) * toMs;
		}

		std::int64_t MatMsToQpc(double a_ms) { return static_cast<std::int64_t>(a_ms / std::max(MatQpcToMs(1), 1e-12)); }

		std::uint64_t PageBytes()
		{
			std::uint64_t texels = 0;
			for (std::uint32_t mip = 0; mip < kMaterialTextureMips; ++mip) {
				const std::uint64_t side = kMaterialTextureSize >> mip;
				texels += side * side;
			}
			return texels * 4 * kMaterialPageLayers;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Attributes (D3D12)
	// ---------------------------------------------------------------------------------------------

	bool Backend::EnsureAttributes()
	{
		if (attr_)
			return true;
		if (!materialsEnabled_ || attrFailed_ || !ready_ || failed_)
			return false;
		attr_ = CreateBuffer(kAttrBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
		if (!attr_) {
			attrFailed_ = true;
			logger::warn("[RayTracing] Reflection materials: could not create the {} MB attribute buffer; shapes are captured without attributes",
				kAttrBytes >> 20);
			return false;
		}
		attr_->SetName(L"CS RayTracing reflection attributes");
		attrFree_.clear();
		attrFree_[0] = static_cast<std::uint32_t>(kAttrBytes / 16);
		attrUsed_ = 0;
		matStats_.attrBytes = kAttrBytes;
		matStats_.attrBytesUsed = 0;
		logger::info("[RayTracing] Reflection materials: attribute buffer {} MB", kAttrBytes >> 20);
		return true;
	}

	bool Backend::AllocateAttributes(std::uint32_t a_count, std::uint32_t& o_offset)
	{
		if (a_count == 0 || !EnsureAttributes())
			return false;
		// First fit (the ranges are whole BLASes: few, large, freed whole).
		for (auto it = attrFree_.begin(); it != attrFree_.end(); ++it) {
			if (it->second < a_count)
				continue;
			o_offset = it->first;
			const std::uint32_t rest = it->second - a_count;
			attrFree_.erase(it);
			if (rest)
				attrFree_.emplace(o_offset + a_count, rest);
			attrUsed_ += a_count;
			matStats_.attrBytesUsed = attrUsed_ * 16;
			return true;
		}
		++matStats_.attrFull;
		return false;
	}

	void Backend::FreeAttributes(std::uint32_t a_offset, std::uint32_t a_count)
	{
		if (a_count)
			retiredAttr_.push_back(DeferredFree{ RetireValue(), a_offset, a_count });
	}

	void Backend::CollectAttrRetired(std::uint64_t a_completed)
	{
		std::erase_if(retiredAttr_, [&](const DeferredFree& a_entry) {
			if (a_entry.fence > a_completed)
				return false;
			attrUsed_ -= std::min<std::uint64_t>(attrUsed_, a_entry.count);
			if (attr_) {
				auto [it, inserted] = attrFree_.emplace(a_entry.value, a_entry.count);
				if (inserted) {
					if (auto next = std::next(it); next != attrFree_.end() && it->first + it->second == next->first) {
						it->second += next->second;
						attrFree_.erase(next);
					}
					if (it != attrFree_.begin()) {
						auto previous = std::prev(it);
						if (previous->first + previous->second == it->first) {
							previous->second += it->second;
							attrFree_.erase(it);
						}
					}
				}
			}
			return true;
		});
		matStats_.attrBytesUsed = attrUsed_ * 16;
	}

	bool Backend::EnsureInstanceAttrBuffer(CommandContext& a_context, std::uint32_t a_instances)
	{
		if (a_context.instanceAttrBuffer && a_context.instanceAttrCapacity >= a_instances)
			return true;
		const std::uint32_t capacity = std::max<std::uint32_t>(256, std::bit_ceil(a_instances));
		if (a_context.instanceAttrBuffer) {
			// This context's previous submission has completed (it is the free one).
			a_context.instanceAttrBuffer->Unmap(0, nullptr);
			matStats_.instanceTableBytes -= std::min<std::uint64_t>(matStats_.instanceTableBytes, static_cast<std::uint64_t>(a_context.instanceAttrCapacity) * 16);
			a_context.instanceAttrBuffer.Reset();
			a_context.instanceAttrMapped = nullptr;
			a_context.instanceAttrCapacity = 0;
		}
		a_context.instanceAttrBuffer = CreateBuffer(static_cast<std::uint64_t>(capacity) * 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
		if (!a_context.instanceAttrBuffer || FAILED(a_context.instanceAttrBuffer->Map(0, nullptr, &a_context.instanceAttrMapped)) || !a_context.instanceAttrMapped) {
			a_context.instanceAttrBuffer.Reset();
			a_context.instanceAttrMapped = nullptr;
			return false;  // the frame's reflection rays run the 48e variant (no material record)
		}
		a_context.instanceAttrBuffer->SetName(L"CS RayTracing reflection instance table");
		a_context.instanceAttrCapacity = capacity;
		matStats_.instanceTableBytes += static_cast<std::uint64_t>(capacity) * 16;
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Surface record (shared textures)
	// ---------------------------------------------------------------------------------------------

	bool Backend::EnsureReflectSurfaceTextures(std::uint32_t a_width, std::uint32_t a_height)
	{
		if (!MaterialTraceAvailable() || !stagingHeap_)
			return false;
		if (ReflectSurfaceReady() && reflectWidth_ == a_width && reflectHeight_ == a_height && reflectSurfBytes_ != 0)
			return true;
		ReleaseReflectSurfaceTextures();
		if (a_width == 0 || a_height == 0 || a_width != reflectWidth_ || a_height != reflectHeight_)
			return false;  // sized like the hit record (EnsureReflectTextures first)
		if (!CreateSharedTexture(a_width, a_height, kReflectFormat, false, true, reflectSurf0_, "RayTracing ReflectSurf0", false) ||
			!CreateSharedTexture(a_width, a_height, kReflectFormat, false, true, reflectSurf1_, "RayTracing ReflectSurf1", false)) {
			reflectSurf0_ = {};
			reflectSurf1_ = {};
			logger::warn("[RayTracing] Reflection materials: could not create the surface record textures; the rays run without materials");
			return false;
		}
		reflectSurfBytes_ = static_cast<std::uint64_t>(a_width) * a_height * 8;
		WriteReflectSurfaceDescriptors();
		logger::info("[RayTracing] Reflection materials: surface record {}x{} ({:.1f} MB)", a_width, a_height,
			static_cast<double>(reflectSurfBytes_) / (1024.0 * 1024.0));
		return true;
	}

	void Backend::ReleaseReflectSurfaceTextures()
	{
		if (reflectSurf0_.resource12)
			Retire(reflectSurf0_.resource12, 0, nullptr);
		if (reflectSurf1_.resource12)
			Retire(reflectSurf1_.resource12, 0, nullptr);
		reflectSurf0_ = {};
		reflectSurf1_ = {};
		reflectSurfBytes_ = 0;
		reflectMatLastFrame_ = false;
		WriteReflectSurfaceDescriptors();  // null views
	}

	void Backend::WriteReflectSurfaceDescriptors()
	{
		if (!stagingHeap_ || !device_)
			return;
		D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
		uavDesc.Format = kReflectFormat;
		uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
		device_->CreateUnorderedAccessView(reflectSurf0_.resource12.Get(), nullptr, &uavDesc, StagingCpu(kDynReflect + 4));
		device_->CreateUnorderedAccessView(reflectSurf1_.resource12.Get(), nullptr, &uavDesc, StagingCpu(kDynReflect + 5));
	}

	// ---------------------------------------------------------------------------------------------
	// Materials (D3D11)
	// ---------------------------------------------------------------------------------------------

	std::uint32_t Backend::AcquireMatTexture(ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_view)
	{
		if (!a_texture || !a_view)
			return UINT32_MAX;
		if (const auto it = matTextureIndex_.find(a_texture); it != matTextureIndex_.end()) {
			++matTextures_[it->second].users;
			return it->second;
		}
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		// Placeholders (a texture still loading, a missing file) and anything but a plain 2D texture are not copied.
		if (desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) || desc.Width < 4 || desc.Height < 4) {
			++matStats_.texturesRefused;
			return UINT32_MAX;
		}
		MatTexture entry;
		entry.texture11 = a_texture;
		entry.view11 = a_view;
		entry.sourceSize = std::max(desc.Width, desc.Height);
		entry.sourceMips = std::max(desc.MipLevels, 1u);
		entry.users = 1;
		entry.live = true;
		std::uint32_t handle;
		if (!freeMatTextures_.empty()) {
			handle = freeMatTextures_.back();
			freeMatTextures_.pop_back();
		} else {
			handle = static_cast<std::uint32_t>(matTextures_.size());
			matTextures_.emplace_back();
		}
		matTextures_[handle] = std::move(entry);
		matTextureIndex_[a_texture] = handle;
		matCopyQueue_.push_back(handle);
		return handle;
	}

	void Backend::ReleaseMatTexture(std::uint32_t a_texture)
	{
		if (a_texture >= matTextures_.size() || !matTextures_[a_texture].live)
			return;
		auto& entry = matTextures_[a_texture];
		if (--entry.users > 0)
			return;
		matTextureIndex_.erase(entry.texture11.Get());
		if (entry.layer != UINT32_MAX)
			freeMatLayers_.push_back(entry.layer);  // its materials were retired kMaterialRetireFrames ago: nothing reads the layer any more
		std::erase(matCopyQueue_, a_texture);
		entry = {};
		freeMatTextures_.push_back(a_texture);
	}

	std::uint16_t Backend::AllocateMaterial()
	{
		std::uint16_t id;
		if (!freeMaterials_.empty()) {
			id = freeMaterials_.back();
			freeMaterials_.pop_back();
		} else if (materials_.size() < kMaterialMaxIds) {
			id = static_cast<std::uint16_t>(materials_.size());
			materials_.emplace_back();
		} else {
			++matStats_.materialsFull;
			return kNoMaterial;
		}
		materials_[id] = MatEntry{};
		materials_[id].live = true;
		materials_[id].users = 1;
		return id;
	}

	std::uint16_t Backend::AcquireMaterial(ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_view, const float a_uvTransform[4])
	{
		if (!materialsEnabled_ || !ready_ || failed_ || !a_texture || !a_view)
			return kNoMaterial;
		float uv[4];
		for (int i = 0; i < 4; ++i)
			uv[i] = std::isfinite(a_uvTransform[i]) ? a_uvTransform[i] : (i < 2 ? 0.0f : 1.0f);
		// Shared by every shape with the same texture and transform (the texture's handle is its key while it lives).
		const auto known = matTextureIndex_.find(a_texture);
		if (known != matTextureIndex_.end()) {
			MatKey key{ known->second, { std::bit_cast<std::uint32_t>(uv[0]), std::bit_cast<std::uint32_t>(uv[1]), std::bit_cast<std::uint32_t>(uv[2]),
										   std::bit_cast<std::uint32_t>(uv[3]) } };
			if (const auto it = materialIndex_.find(key); it != materialIndex_.end()) {
				auto& entry = materials_[it->second];
				if (entry.retired) {
					// Revived before its id was reused: its table entry never changed.
					entry.retired = false;
					std::erase_if(retiredMaterials_, [&](const auto& a_retired) { return a_retired.second == it->second; });
				}
				++entry.users;
				return it->second;
			}
		}
		const std::uint32_t texture = AcquireMatTexture(a_texture, a_view);
		if (texture == UINT32_MAX)
			return kNoMaterial;
		const std::uint16_t id = AllocateMaterial();
		if (id == kNoMaterial) {
			ReleaseMatTexture(texture);
			return kNoMaterial;
		}
		auto& entry = materials_[id];
		entry.textures[0] = texture;
		std::copy_n(uv, 4, entry.uvTransform);
		materialIndex_[MatKey{ texture, { std::bit_cast<std::uint32_t>(uv[0]), std::bit_cast<std::uint32_t>(uv[1]), std::bit_cast<std::uint32_t>(uv[2]),
											 std::bit_cast<std::uint32_t>(uv[3]) } }] = id;
		matTableDirty_ = true;
		return id;
	}

	std::uint16_t Backend::AcquireTerrainMaterial(const TerrainLayerSource& a_layers, std::uint32_t a_tint)
	{
		if (!materialsEnabled_ || !ready_ || failed_)
			return kNoMaterial;
		const std::uint16_t id = AllocateMaterial();
		if (id == kNoMaterial)
			return kNoMaterial;
		auto& entry = materials_[id];
		entry.terrain = true;
		entry.tint = a_tint;
		for (int i = 0; i < 6; ++i)
			entry.textures[i] = AcquireMatTexture(a_layers.texture[i], a_layers.view[i]);
		matTableDirty_ = true;
		return id;
	}

	void Backend::ReleaseMaterial(std::uint16_t a_material)
	{
		if (a_material >= materials_.size() || !materials_[a_material].live || materials_[a_material].retired)
			return;
		auto& entry = materials_[a_material];
		if (entry.users > 1) {
			--entry.users;
			return;
		}
		entry.users = 0;
		entry.retired = true;
		retiredMaterials_.emplace_back(matFrame_, a_material);
	}

	void Backend::RecycleMaterials(std::uint32_t a_frame, bool a_all)
	{
		std::erase_if(retiredMaterials_, [&](const std::pair<std::uint32_t, std::uint16_t>& a_retired) {
			if (!a_all && a_frame - a_retired.first < kMaterialRetireFrames)
				return false;
			const std::uint16_t id = a_retired.second;
			if (id >= materials_.size() || !materials_[id].live || !materials_[id].retired)
				return true;
			auto& entry = materials_[id];
			if (!entry.terrain) {
				MatKey key{ entry.textures[0], { std::bit_cast<std::uint32_t>(entry.uvTransform[0]), std::bit_cast<std::uint32_t>(entry.uvTransform[1]),
												   std::bit_cast<std::uint32_t>(entry.uvTransform[2]), std::bit_cast<std::uint32_t>(entry.uvTransform[3]) } };
				if (const auto it = materialIndex_.find(key); it != materialIndex_.end() && it->second == id)
					materialIndex_.erase(it);
			}
			for (const auto texture : entry.textures)
				if (texture != UINT32_MAX)
					ReleaseMatTexture(texture);
			entry = MatEntry{};
			freeMaterials_.push_back(id);
			matTableDirty_ = true;
			return true;
		});
	}

	void Backend::StartMaterialPage()
	{
		if (matPageJob_ || matPageFailed_ || matPages_.size() >= kMaterialMaxPages || !device11_)
			return;
		auto job = std::make_shared<MatPageJob>();
		matPageJob_ = job;
		ComPtr<ID3D11Device> device = device11_;
		std::thread([job, device]() {
			// Below normal: a page is 22 MB of D3D11 texture plus its views; the device is free-threaded (as the shader cache's compile workers).
			SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
			D3D11_TEXTURE2D_DESC desc{};
			desc.Width = desc.Height = kMaterialTextureSize;
			desc.MipLevels = kMaterialTextureMips;
			desc.ArraySize = kMaterialPageLayers;
			desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
			desc.SampleDesc.Count = 1;
			desc.Usage = D3D11_USAGE_DEFAULT;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
			bool ok = SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &job->page.texture));
			if (ok) {
				Util::SetResourceName(job->page.texture.Get(), "RayTracing MaterialCopies");
				D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
				srvDesc.Format = desc.Format;
				srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
				srvDesc.Texture2DArray.MipLevels = kMaterialTextureMips;
				srvDesc.Texture2DArray.ArraySize = kMaterialPageLayers;
				ok = SUCCEEDED(device->CreateShaderResourceView(job->page.texture.Get(), &srvDesc, &job->page.srv));
			}
			for (std::uint32_t mip = 0; ok && mip < kMaterialTextureMips; ++mip) {
				D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
				uavDesc.Format = desc.Format;
				uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
				uavDesc.Texture2DArray.MipSlice = mip;
				uavDesc.Texture2DArray.ArraySize = kMaterialPageLayers;
				ok = SUCCEEDED(device->CreateUnorderedAccessView(job->page.texture.Get(), &uavDesc, &job->page.uavs[mip]));
			}
			if (!ok)
				job->page = {};
			job->state.store(ok ? 1 : 2, std::memory_order_release);
		}).detach();
	}

	void Backend::UploadMaterialTable()
	{
		if (!matTableDirty_ || !device11_ || !context11_)
			return;
		const auto count = static_cast<std::uint32_t>(materials_.size());
		if (count == 0)
			return;
		if (!matTable_ || matTableCapacity_ < count) {
			// Small (48 bytes an id): grown in powers of two, at most a few times per session.
			matTableSrv_.Reset();
			matTable_.Reset();
			matTableCapacity_ = 0;
			const std::uint32_t capacity = std::max<std::uint32_t>(1024, std::bit_ceil(count));
			D3D11_BUFFER_DESC desc{};
			desc.ByteWidth = capacity * sizeof(MaterialGpu);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
			desc.StructureByteStride = sizeof(MaterialGpu);
			if (FAILED(device11_->CreateBuffer(&desc, nullptr, &matTable_)))
				return;
			Util::SetResourceName(matTable_.Get(), "RayTracing MaterialTable");
			D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
			srvDesc.Format = DXGI_FORMAT_UNKNOWN;
			srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
			srvDesc.Buffer.FirstElement = 0;
			srvDesc.Buffer.NumElements = capacity;
			if (FAILED(device11_->CreateShaderResourceView(matTable_.Get(), &srvDesc, &matTableSrv_))) {
				matTable_.Reset();
				return;
			}
			matTableCapacity_ = capacity;
			matStats_.tableBytes = static_cast<std::uint64_t>(capacity) * sizeof(MaterialGpu);
		}
		D3D11_MAPPED_SUBRESOURCE mapped{};
		if (FAILED(context11_->Map(matTable_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
			return;
		auto* out = static_cast<MaterialGpu*>(mapped.pData);
		auto layerOf = [&](std::uint32_t a_texture) -> std::uint32_t {
			if (a_texture >= matTextures_.size() || !matTextures_[a_texture].live || !matTextures_[a_texture].ready)
				return 0xFFFFu;
			return matTextures_[a_texture].layer & 0xFFFFu;
		};
		for (std::uint32_t id = 0; id < count; ++id) {
			const auto& entry = materials_[id];
			MaterialGpu gpu{};
			gpu.layer = 0xFFFFu;
			gpu.terrain[0] = gpu.terrain[1] = gpu.terrain[2] = 0xFFFFFFFFu;
			gpu.tint = 0xFFFFFFFFu;
			gpu.uvTransform[0] = 0.0f;
			gpu.uvTransform[1] = 0.0f;
			gpu.uvTransform[2] = 1.0f;
			gpu.uvTransform[3] = 1.0f;
			if (entry.live) {
				std::copy_n(entry.uvTransform, 4, gpu.uvTransform);
				gpu.tint = entry.tint;
				if (entry.terrain) {
					gpu.flags = 1u;
					bool pending = false;
					for (int i = 0; i < 6; ++i) {
						const std::uint32_t layer = layerOf(entry.textures[i]);
						pending = pending || (entry.textures[i] != UINT32_MAX && layer == 0xFFFFu);
						gpu.terrain[i / 2] = (gpu.terrain[i / 2] & ~(0xFFFFu << (16 * (i & 1)))) | (layer << (16 * (i & 1)));
					}
					gpu.flags |= pending ? 4u : 0u;
				} else if (entry.textures[0] != UINT32_MAX) {
					gpu.layer = layerOf(entry.textures[0]);
					gpu.flags = 2u | (gpu.layer == 0xFFFFu ? 4u : 0u);
				}
			}
			out[id] = gpu;
		}
		context11_->Unmap(matTable_.Get(), 0);
		matTableDirty_ = false;
	}

	void Backend::UpdateMaterialStats()
	{
		matStats_.materials = 0;
		for (const auto& entry : materials_)
			matStats_.materials += entry.live && !entry.retired ? 1u : 0u;
		matStats_.textures = matStats_.texturesReady = matStats_.texturesPending = 0;
		for (const auto& entry : matTextures_) {
			if (!entry.live)
				continue;
			++matStats_.textures;
			++(entry.ready ? matStats_.texturesReady : matStats_.texturesPending);
		}
		matStats_.pages = static_cast<std::uint32_t>(matPages_.size());
		matStats_.pageCreating = matPageJob_ != nullptr;
		matStats_.textureBytes = PageBytes() * matPages_.size();
	}

	void Backend::UpdateMaterials(ID3D11ComputeShader* a_shader, std::uint32_t a_maxCopies, float a_cpuMs, std::uint32_t a_frame)
	{
		const std::int64_t start = MatQpc();
		matFrame_ = a_frame;
		matStats_.copiesLastFrame = 0;
		if (!context11_ || !device11_) {
			matStats_.copyCpuMs = 0.0f;
			return;
		}
		RecycleMaterials(a_frame, false);
		// A page made on the worker thread joins the free layers.
		if (matPageJob_) {
			const int state = matPageJob_->state.load(std::memory_order_acquire);
			if (state == 1) {
				const auto base = static_cast<std::uint32_t>(matPages_.size()) * kMaterialPageLayers;
				matPages_.push_back(std::move(matPageJob_->page));
				matPageJob_.reset();
				for (std::uint32_t i = kMaterialPageLayers; i-- > 0;)
					freeMatLayers_.push_back(base + i);
				logger::info("[RayTracing] Reflection materials: texture-copy page {} ready ({} layers of {}x{} RGBA8, {:.1f} MB)", matPages_.size(),
					kMaterialPageLayers, kMaterialTextureSize, kMaterialTextureSize, static_cast<double>(PageBytes()) / (1024.0 * 1024.0));
			} else if (state == 2) {
				matPageJob_.reset();
				matPageFailed_ = true;
				logger::warn("[RayTracing] Reflection materials: a texture-copy page could not be created; textures past the last page stay uncopied");
			}
		}
		// Layers for waiting textures; a new page (worker thread) when they run out, ahead by a quarter page.
		std::uint32_t waiting = 0;
		for (const auto handle : matCopyQueue_) {
			if (handle >= matTextures_.size())
				continue;
			auto& entry = matTextures_[handle];
			if (!entry.live || entry.layer != UINT32_MAX)
				continue;
			if (!freeMatLayers_.empty()) {
				entry.layer = freeMatLayers_.back();
				freeMatLayers_.pop_back();
			} else {
				++waiting;
			}
		}
		matStats_.texturesFull = (matPages_.size() >= kMaterialMaxPages && !matPageJob_) ? waiting : 0;  // waiting with no page to come
		if (materialsEnabled_ && matPages_.size() < kMaterialMaxPages && (waiting > 0 || (freeMatLayers_.size() < kMaterialPageLayers / 4 && !matCopyQueue_.empty())))
			StartMaterialPage();
		// The copies (layers assigned, source still held).
		if (a_shader && a_maxCopies > 0 && !matCopyQueue_.empty() && !matPages_.empty()) {
			if (!matCopyCB_) {
				D3D11_BUFFER_DESC cbDesc{};
				cbDesc.ByteWidth = 16;
				cbDesc.Usage = D3D11_USAGE_DYNAMIC;
				cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
				cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
				device11_->CreateBuffer(&cbDesc, nullptr, &matCopyCB_);
			}
			if (!matCopySampler_) {
				D3D11_SAMPLER_DESC samplerDesc{};
				samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
				samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
				samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
				samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
				device11_->CreateSamplerState(&samplerDesc, &matCopySampler_);
			}
			if (matCopyCB_ && matCopySampler_) {
				auto* context = context11_.Get();
				ID3D11Buffer* cb = matCopyCB_.Get();
				ID3D11SamplerState* sampler = matCopySampler_.Get();
				ComPtr<ID3D11SamplerState> previousSampler;  // the game's compute sampler 0 is put back afterwards
				context->CSGetSamplers(0, 1, previousSampler.GetAddressOf());
				context->CSSetShader(a_shader, nullptr, 0);
				context->CSSetConstantBuffers(0, 1, &cb);
				context->CSSetSamplers(0, 1, &sampler);
				const std::int64_t deadline = start + MatMsToQpc(a_cpuMs);
				std::uint32_t made = 0;
				std::uint32_t boundPage = UINT32_MAX;
				for (auto it = matCopyQueue_.begin(); it != matCopyQueue_.end() && made < a_maxCopies && (made == 0 || MatQpc() < deadline);) {
					const std::uint32_t handle = *it;
					if (handle >= matTextures_.size() || !matTextures_[handle].live || matTextures_[handle].ready) {
						it = matCopyQueue_.erase(it);
						continue;
					}
					auto& entry = matTextures_[handle];
					if (entry.layer == UINT32_MAX || !entry.view11 || entry.layer / kMaterialPageLayers >= matPages_.size()) {
						++it;  // no layer yet (waits for a page)
						continue;
					}
					const std::uint32_t page = entry.layer / kMaterialPageLayers;
					if (page != boundPage) {
						ID3D11UnorderedAccessView* uavs[kMaterialTextureMips];
						for (std::uint32_t mip = 0; mip < kMaterialTextureMips; ++mip)
							uavs[mip] = matPages_[page].uavs[mip].Get();
						context->CSSetUnorderedAccessViews(0, kMaterialTextureMips, uavs, nullptr);
						boundPage = page;
					}
					D3D11_MAPPED_SUBRESOURCE mapped{};
					if (FAILED(context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
						break;
					const float lod0 = std::log2(std::max(static_cast<float>(entry.sourceSize) / static_cast<float>(kMaterialTextureSize), 1.0f));
					const std::uint32_t values[4] = { entry.layer % kMaterialPageLayers, std::bit_cast<std::uint32_t>(lod0),
						std::bit_cast<std::uint32_t>(static_cast<float>(entry.sourceMips - 1)), 0 };
					std::memcpy(mapped.pData, values, sizeof(values));
					context->Unmap(cb, 0);
					ID3D11ShaderResourceView* source = entry.view11.Get();
					context->CSSetShaderResources(0, 1, &source);
					// One dispatch for every mip: z = mip, 16 x 16 groups of 8 x 8 cover the 128 x 128 level (smaller levels exit early).
					context->Dispatch(kMaterialTextureSize / 8, kMaterialTextureSize / 8, kMaterialTextureMips);
					entry.ready = true;
					entry.view11.Reset();
					it = matCopyQueue_.erase(it);
					++made;
				}
				ID3D11ShaderResourceView* nullSrv = nullptr;
				ID3D11UnorderedAccessView* nullUavs[kMaterialTextureMips]{};
				ID3D11Buffer* nullCb = nullptr;
				ID3D11SamplerState* restoreSampler = previousSampler.Get();
				context->CSSetShaderResources(0, 1, &nullSrv);
				context->CSSetUnorderedAccessViews(0, kMaterialTextureMips, nullUavs, nullptr);
				context->CSSetConstantBuffers(0, 1, &nullCb);
				context->CSSetSamplers(0, 1, &restoreSampler);
				context->CSSetShader(nullptr, nullptr, 0);
				if (made) {
					matTableDirty_ = true;
					matStats_.copiesLastFrame = made;
					matStats_.copiesTotal += made;
				}
			}
		}
		UploadMaterialTable();
		UpdateMaterialStats();
		matStats_.copyCpuMs = static_cast<float>(MatQpcToMs(MatQpc() - start));
	}

	void Backend::MaterialHousekeeping(std::uint32_t a_frame)
	{
		if (!materialsEnabled_) {
			// Everything retired can go at once (nothing is traced with materials any more: no hit record names them).
			RecycleMaterials(a_frame, true);
			bool used = false;
			for (const auto& entry : materials_)
				used = used || entry.live;
			if (!used && attrUsed_ == 0 && retiredAttr_.empty() && (attr_ || !matPages_.empty() || matTable_ || matPageJob_ || !materials_.empty()))
				ReleaseMaterialResources();
			attrFailed_ = false;  // a later switch-on tries again
			UpdateMaterialStats();
		}
	}

	void Backend::ReleaseMaterialResources()
	{
		if (attr_)
			Retire(attr_, 0, nullptr);
		attr_.Reset();
		attrFree_.clear();
		retiredAttr_.clear();
		attrUsed_ = 0;
		matTextures_.clear();
		freeMatTextures_.clear();
		matTextureIndex_.clear();
		materials_.clear();
		freeMaterials_.clear();
		retiredMaterials_.clear();
		materialIndex_.clear();
		matCopyQueue_.clear();
		matPages_.clear();  // D3D11 keeps them alive while queued work uses them
		freeMatLayers_.clear();
		nextMatLayer_ = 0;
		matPageJob_.reset();  // a page still being made is dropped when its thread finishes
		matPageFailed_ = false;
		matTableSrv_.Reset();
		matTable_.Reset();
		matTableCapacity_ = 0;
		matTableDirty_ = false;
		matCopyCB_.Reset();
		matCopySampler_.Reset();
		const std::uint64_t instanceTables = matStats_.instanceTableBytes;
		const std::uint64_t copies = matStats_.copiesTotal;
		const std::uint64_t full = matStats_.attrFull, refused = matStats_.texturesRefused;
		matStats_ = {};
		matStats_.instanceTableBytes = instanceTables;
		matStats_.copiesTotal = copies;
		matStats_.attrFull = full;
		matStats_.texturesRefused = refused;
		logger::info("[RayTracing] Reflection materials: buffers released");
	}
}
