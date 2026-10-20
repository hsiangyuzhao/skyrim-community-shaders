// (45h) Alpha-tested casters on the ray tracer's D3D12 side: the textures their cut-outs come
// from, their texcoords and the per-shape records the alpha variant of TraceShadowsCS reads.
//
// Textures: every cut-out texture gets a 256x256 alpha copy (5 mips) in a texture array shared from
// D3D11, made by AlphaMaskCS on D3D11 before the frame's trace (the D3D12 queue waits for D3D11 on the GPU, so the trace sees it).
// Texcoords: one buffer, a range per alpha BLAS ((47q) 2 uint2 per triangle: uv0, uv1 / uv2, the 3 vertex alphas; PackAlphaCorners).
// Records: a CPU table; each command context keeps its own upload copy, rewritten when the table
// changed, so a record can be reused at once without touching a submission in flight.
// Descriptor slots, copy layers and texcoord ranges are reused only after the GPU passed the
// fence of the last submission that could read them.

#include "Backend.h"

#include "Utils/D3D.h"

#include <bit>

#include <dxgi1_2.h>

namespace RayTracingRT
{
	namespace
	{
		std::string Hr(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }
	}

	bool PackAlphaCorners(std::vector<std::uint32_t>& a_corners, std::uint32_t a_triangles)
	{
		// (47q) Captured as 3 corners per triangle, (half2 texcoord, vertex alpha float bits) each; traced as 2 entries per triangle:
		// (uv0, uv1), (uv2, alpha0 | alpha1 << 8 | alpha2 << 16). Every vertex alpha the captures write is byte / 255 or 1, exact in 8 bits.
		if (a_triangles == 0 || a_corners.size() != static_cast<std::size_t>(a_triangles) * 6)
			return false;
		for (std::size_t t = 0; t < a_triangles; ++t) {
			const std::uint32_t* in = a_corners.data() + t * 6;
			std::uint32_t alphas = 0;
			for (int c = 0; c < 3; ++c) {
				const float a = std::bit_cast<float>(in[c * 2 + 1]);
				const float clamped = std::isfinite(a) ? std::clamp(a, 0.0f, 1.0f) : 1.0f;
				alphas |= static_cast<std::uint32_t>(std::lround(clamped * 255.0f)) << (8 * c);
			}
			const std::uint32_t packed[4] = { in[0], in[2], in[4], alphas };
			std::copy_n(packed, 4, a_corners.data() + t * 4);  // t * 4 + 3 < t * 6 + 4 for t >= 0: never overwrites unread input
		}
		a_corners.resize(static_cast<std::size_t>(a_triangles) * 4);
		return true;
	}

	void Backend::SetAlphaFault(std::string a_reason)
	{
		if (!alphaFault_.empty())
			return;
		alphaFault_ = std::move(a_reason);
		logger::error("[RayTracing] Alpha-tested casters: {} -- switched off", alphaFault_);
	}

	void Backend::UpdateAlphaTextureStats()
	{
		alphaStats_.texturesFallback = alphaStats_.texturesPending = 0;
		for (const auto& entry : alphaTextures_) {
			if (!entry.live)
				continue;
			++alphaStats_.texturesFallback;
			if (!entry.ready)
				++alphaStats_.texturesPending;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Texcoord buffer
	// ---------------------------------------------------------------------------------------------

	bool Backend::EnsureAlphaUv()
	{
		if (alphaUv_)
			return true;
		if (!ready_ || failed_ || !alphaFault_.empty())
			return false;
		alphaUv_ = CreateBuffer(kAlphaUvBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, false);
		if (!alphaUv_) {
			SetAlphaFault(std::format("could not create the {} MB texcoord buffer", kAlphaUvBytes >> 20));
			return false;
		}
		alphaUv_->SetName(L"CS RayTracing alpha texcoords");
		alphaUvFree_.clear();
		alphaUvFree_[0] = static_cast<std::uint32_t>(kAlphaUvBytes / 8);
		alphaUvUsed_ = 0;
		alphaStats_.uvBytes = kAlphaUvBytes;
		alphaStats_.uvBytesUsed = 0;
		logger::info("[RayTracing] Alpha-tested casters: texcoord buffer {} MB", kAlphaUvBytes >> 20);
		return true;
	}

	bool Backend::AllocateAlphaUv(std::uint32_t a_count, std::uint32_t& o_offset)
	{
		if (a_count == 0 || !EnsureAlphaUv())
			return false;
		for (auto it = alphaUvFree_.begin(); it != alphaUvFree_.end(); ++it) {
			if (it->second < a_count)
				continue;
			o_offset = it->first;
			const std::uint32_t rest = it->second - a_count;
			alphaUvFree_.erase(it);
			if (rest)
				alphaUvFree_.emplace(o_offset + a_count, rest);
			alphaUvUsed_ += a_count;
			alphaStats_.uvBytesUsed = alphaUvUsed_ * 8;
			return true;
		}
		++alphaStats_.uvFull;
		return false;
	}

	void Backend::FreeAlphaUv(std::uint32_t a_offset, std::uint32_t a_count)
	{
		if (a_count)
			retiredAlphaUv_.push_back(DeferredFree{ RetireValue(), a_offset, a_count });
	}

	void Backend::CollectAlphaRetired(std::uint64_t a_completed)
	{
		std::erase_if(retiredAlphaLayers_, [&](const DeferredFree& a_entry) {
			if (a_entry.fence > a_completed)
				return false;
			freeAlphaLayers_.push_back(a_entry.value);
			return true;
		});
		std::erase_if(retiredAlphaUv_, [&](const DeferredFree& a_entry) {
			if (a_entry.fence > a_completed)
				return false;
			alphaUvUsed_ -= std::min<std::uint64_t>(alphaUvUsed_, a_entry.count);
			if (alphaUv_) {
				// Back into the free list, merged with its neighbours.
				auto [it, inserted] = alphaUvFree_.emplace(a_entry.value, a_entry.count);
				if (inserted) {
					if (auto next = std::next(it); next != alphaUvFree_.end() && it->first + it->second == next->first) {
						it->second += next->second;
						alphaUvFree_.erase(next);
					}
					if (it != alphaUvFree_.begin()) {
						auto previous = std::prev(it);
						if (previous->first + previous->second == it->first) {
							previous->second += it->second;
							alphaUvFree_.erase(it);
						}
					}
				}
			}
			return true;
		});
		alphaStats_.uvBytesUsed = alphaUvUsed_ * 8;
	}

	// ---------------------------------------------------------------------------------------------
	// Textures
	// ---------------------------------------------------------------------------------------------

	bool Backend::EnsureAlphaLayers()
	{
		if (alphaLayers12_)
			return true;
		if (alphaLayersFailed_ || !ready_ || failed_ || !alphaFault_.empty())
			return false;
		// R8 where the driver shares it; four channels (and half the layers) otherwise.
		DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
		std::uint32_t layers = 0;
		if (CheckSharedFormat(DXGI_FORMAT_R8_UNORM, true, false, "RayTracing alpha copies", false)) {
			format = DXGI_FORMAT_R8_UNORM;
			layers = 512;
		} else if (CheckSharedFormat(DXGI_FORMAT_R8G8B8A8_UNORM, true, false, "RayTracing alpha copies", false)) {
			format = DXGI_FORMAT_R8G8B8A8_UNORM;
			layers = 256;
		} else {
			alphaLayersFailed_ = true;
			logger::warn("[RayTracing] Alpha-tested casters: no shareable format for the 256x256 copies; only shareable textures are used");
			return false;
		}

		auto fail = [&](const char* a_what, HRESULT a_hr) {
			for (auto& uav : alphaLayerUavs_)
				uav.Reset();
			alphaLayers11_.Reset();
			alphaLayers12_.Reset();
			alphaLayersFailed_ = true;
			SetAlphaFault(std::format("{} failed ({})", a_what, Hr(a_hr)));
			return false;
		};

		D3D11_TEXTURE2D_DESC desc{};
		desc.Width = desc.Height = kAlphaLayerSize;
		desc.MipLevels = kAlphaLayerMips;
		desc.ArraySize = layers;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Usage = D3D11_USAGE_DEFAULT;
		desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
		desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
		HRESULT hr = device11_->CreateTexture2D(&desc, nullptr, &alphaLayers11_);
		if (FAILED(hr))
			return fail("CreateTexture2D (alpha copies)", hr);
		Util::SetResourceName(alphaLayers11_.Get(), "RayTracing AlphaCopies");
		for (std::uint32_t mip = 0; mip < kAlphaLayerMips; ++mip) {
			D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc{};
			uavDesc.Format = format;
			uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
			uavDesc.Texture2DArray.MipSlice = mip;
			uavDesc.Texture2DArray.FirstArraySlice = 0;
			uavDesc.Texture2DArray.ArraySize = layers;
			if (FAILED(hr = device11_->CreateUnorderedAccessView(alphaLayers11_.Get(), &uavDesc, &alphaLayerUavs_[mip])))
				return fail("alpha copy views", hr);
		}
		ComPtr<IDXGIResource1> dxgiResource;
		if (FAILED(hr = alphaLayers11_.As(&dxgiResource)))
			return fail("IDXGIResource1 (alpha copies)", hr);
		HANDLE handle = nullptr;
		if (FAILED(hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
			return fail("CreateSharedHandle (alpha copies)", hr);
		hr = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&alphaLayers12_));
		CloseHandle(handle);
		if (FAILED(hr))
			return fail("OpenSharedHandle (alpha copies)", hr);

		D3D11_BUFFER_DESC cbDesc{};
		cbDesc.ByteWidth = 16;
		cbDesc.Usage = D3D11_USAGE_DYNAMIC;
		cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
		cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
		if (FAILED(hr = device11_->CreateBuffer(&cbDesc, nullptr, &alphaCopyCB_)))
			return fail("alpha copy constants", hr);
		D3D11_SAMPLER_DESC samplerDesc{};
		samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
		samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
		samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
		samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
		if (FAILED(hr = device11_->CreateSamplerState(&samplerDesc, &alphaCopySampler_)))
			return fail("alpha copy sampler", hr);

		auto cpu = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
		cpu.ptr += static_cast<SIZE_T>(descriptorSize_) * (kAlphaDescriptorBase - 1);
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
		srvDesc.Format = format;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2DArray.MipLevels = kAlphaLayerMips;
		srvDesc.Texture2DArray.ArraySize = layers;
		device_->CreateShaderResourceView(alphaLayers12_.Get(), &srvDesc, cpu);

		alphaLayerFormat_ = format;
		alphaLayerCount_ = layers;
		nextAlphaLayer_ = 0;
		freeAlphaLayers_.clear();
		std::uint64_t bytes = 0;
		for (std::uint32_t mip = 0; mip < kAlphaLayerMips; ++mip) {
			const std::uint64_t side = kAlphaLayerSize >> mip;
			bytes += side * side;
		}
		alphaStats_.layerBytes = bytes * layers * (format == DXGI_FORMAT_R8_UNORM ? 1 : 4);
		logger::info("[RayTracing] Alpha-tested casters: {} copy layers of 256x256 ({}, {:.1f} MB)", layers,
			format == DXGI_FORMAT_R8_UNORM ? "R8" : "RGBA8", static_cast<double>(alphaStats_.layerBytes) / (1024.0 * 1024.0));
		return true;
	}

	AlphaTexture Backend::AcquireAlphaTexture(ID3D11Texture2D* a_texture, ID3D11ShaderResourceView* a_view)
	{
		if (!AlphaAvailable() || !a_texture || !a_view)
			return kInvalidAlphaTexture;
		if (const auto it = alphaTextureIndex_.find(a_texture); it != alphaTextureIndex_.end()) {
			++alphaTextures_[it->second].users;
			return it->second;
		}
		D3D11_TEXTURE2D_DESC desc{};
		a_texture->GetDesc(&desc);
		// Tiny textures are placeholders (a texture still loading, a missing file): a cut-out read from
		// them would turn the whole card into a solid quad, so the shape keeps its shadow-map shadow.
		if (desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE) || desc.Width < 16 || desc.Height < 16)
			return kInvalidAlphaTexture;

		AlphaTextureEntry entry;
		entry.texture11 = a_texture;
		entry.sourceSize = std::max(desc.Width, desc.Height);
		entry.sourceMips = std::max(desc.MipLevels, 1u);  // (47q)

		// A 256x256 alpha copy, made on D3D11 by RecordAlphaCopies.
		if (!EnsureAlphaLayers())
			return kInvalidAlphaTexture;
		std::uint32_t layer = UINT32_MAX;
		if (!freeAlphaLayers_.empty()) {
			layer = freeAlphaLayers_.back();
			freeAlphaLayers_.pop_back();
		} else if (nextAlphaLayer_ < alphaLayerCount_) {
			layer = nextAlphaLayer_++;
		}
		if (layer == UINT32_MAX) {
			++alphaStats_.layersFull;
			return kInvalidAlphaTexture;
		}
		entry.view11 = a_view;
		entry.gpuIndex = kAlphaFallbackBit | layer;
		entry.ready = false;

		entry.live = true;
		entry.users = 1;
		AlphaTexture handle;
		if (!freeAlphaTextures_.empty()) {
			handle = freeAlphaTextures_.back();
			freeAlphaTextures_.pop_back();
		} else {
			handle = static_cast<AlphaTexture>(alphaTextures_.size());
			alphaTextures_.emplace_back();
		}
		const bool pending = !entry.ready;
		alphaTextures_[handle] = std::move(entry);
		alphaTextureIndex_[a_texture] = handle;
		if (pending)
			alphaCopyQueue_.push_back(handle);
		UpdateAlphaTextureStats();
		return handle;
	}

	void Backend::ReleaseAlphaTexture(AlphaTexture a_texture)
	{
		if (a_texture >= alphaTextures_.size() || !alphaTextures_[a_texture].live)
			return;
		auto& entry = alphaTextures_[a_texture];
		if (--entry.users > 0)
			return;
		alphaTextureIndex_.erase(entry.texture11.Get());
		if (entry.gpuIndex != UINT32_MAX) {
			retiredAlphaLayers_.push_back(DeferredFree{ RetireValue(), entry.gpuIndex & ~kAlphaFallbackBit, 0 });
			std::erase(alphaCopyQueue_, a_texture);
		}
		entry = {};
		freeAlphaTextures_.push_back(a_texture);
		UpdateAlphaTextureStats();
	}

	bool Backend::IsAlphaTextureReady(AlphaTexture a_texture) const
	{
		return a_texture < alphaTextures_.size() && alphaTextures_[a_texture].live && alphaTextures_[a_texture].ready;
	}

	void Backend::RecordAlphaCopies(ID3D11ComputeShader* a_shader, std::uint32_t a_max)
	{
		if (!a_shader || alphaCopyQueue_.empty() || !alphaLayers11_ || !alphaCopyCB_ || !alphaCopySampler_ || !context11_)
			return;
		auto* context = context11_.Get();
		ID3D11Buffer* cb = alphaCopyCB_.Get();
		ID3D11SamplerState* sampler = alphaCopySampler_.Get();
		ComPtr<ID3D11SamplerState> previousSampler;  // the game's compute sampler 0 is put back afterwards
		context->CSGetSamplers(0, 1, previousSampler.GetAddressOf());
		context->CSSetShader(a_shader, nullptr, 0);
		context->CSSetConstantBuffers(0, 1, &cb);
		context->CSSetSamplers(0, 1, &sampler);
		std::uint32_t made = 0;
		while (!alphaCopyQueue_.empty() && made < a_max) {
			const AlphaTexture handle = alphaCopyQueue_.front();
			alphaCopyQueue_.pop_front();
			if (handle >= alphaTextures_.size())
				continue;
			auto& entry = alphaTextures_[handle];
			if (!entry.live || entry.ready || !entry.view11 || !(entry.gpuIndex & kAlphaFallbackBit))
				continue;
			const std::uint32_t layer = entry.gpuIndex & ~kAlphaFallbackBit;
			ID3D11ShaderResourceView* source = entry.view11.Get();
			context->CSSetShaderResources(0, 1, &source);
			for (std::uint32_t mip = 0; mip < kAlphaLayerMips; ++mip) {
				const std::uint32_t side = kAlphaLayerSize >> mip;
				// The source mip closest to this copy mip's size (bilinear from there).
				const float lod = std::max(0.0f, std::log2(static_cast<float>(entry.sourceSize) / static_cast<float>(side)));
				D3D11_MAPPED_SUBRESOURCE mapped{};
				if (FAILED(context->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
					continue;
				const std::uint32_t values[4] = { layer, side, std::bit_cast<std::uint32_t>(lod), 0 };
				std::memcpy(mapped.pData, values, sizeof(values));
				context->Unmap(cb, 0);
				ID3D11UnorderedAccessView* uav = alphaLayerUavs_[mip].Get();
				context->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
				context->Dispatch((side + 7) / 8, (side + 7) / 8, 1);
			}
			entry.ready = true;
			entry.readySerial = waitSerial_;  // (47s) the D3D12 queue sees it behind the next ready wait
			entry.view11.Reset();
			++made;
		}
		ID3D11ShaderResourceView* nullSrv = nullptr;
		ID3D11UnorderedAccessView* nullUav = nullptr;
		ID3D11Buffer* nullCb = nullptr;
		ID3D11SamplerState* restoreSampler = previousSampler.Get();
		context->CSSetShaderResources(0, 1, &nullSrv);
		context->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
		context->CSSetConstantBuffers(0, 1, &nullCb);
		context->CSSetSamplers(0, 1, &restoreSampler);
		context->CSSetShader(nullptr, nullptr, 0);
		if (made)
			UpdateAlphaTextureStats();
	}

	// ---------------------------------------------------------------------------------------------
	// Per-shape records
	// ---------------------------------------------------------------------------------------------

	std::uint32_t Backend::CreateAlphaInfo(BlasHandle a_blas, AlphaTexture a_texture, const AlphaMaterial& a_material)
	{
		if (a_blas >= blas_.size() || blas_[a_blas].uvOffset == UINT32_MAX)
			return kInvalidAlphaInfo;
		const std::uint32_t slot = CreateAlphaInfoAt(blas_[a_blas].uvOffset, blas_[a_blas].triangles, a_texture, a_material);
		if (slot != kInvalidAlphaInfo && slot < slotBase_.size())
			slotBase_[slot] = SlotBase{ VariantKind::Static, a_blas, a_texture };  // (47s)
		return slot;
	}

	std::uint32_t Backend::AllocateAlphaInfoSlot(const AlphaInfoGpu& a_info)
	{
		std::uint32_t slot;
		if (!freeAlphaInfos_.empty()) {
			slot = freeAlphaInfos_.back();
			freeAlphaInfos_.pop_back();
		} else if (alphaInfos_.size() < kAlphaMaxInfos) {
			slot = static_cast<std::uint32_t>(alphaInfos_.size());
			alphaInfos_.emplace_back();
			alphaInfoUsed_.push_back(0);
			slotVariant_.push_back(UINT32_MAX);
			slotBase_.emplace_back();
		} else {
			++alphaStats_.infosFull;
			return kInvalidAlphaInfo;
		}
		alphaInfos_[slot] = a_info;
		alphaInfoUsed_[slot] = 1;
		slotVariant_[slot] = UINT32_MAX;
		slotBase_[slot] = SlotBase{};
		++alphaInfoLive_;
		++alphaInfoVersion_;
		alphaStats_.infos = alphaInfoLive_;
		return slot;
	}

	std::uint32_t Backend::CreateAlphaInfoAt(std::uint32_t a_uvOffset, std::uint32_t a_triangles, AlphaTexture a_texture, const AlphaMaterial& a_material)
	{
		if (!AlphaAvailable() || a_uvOffset == UINT32_MAX || a_triangles == 0 || a_texture >= alphaTextures_.size() || !alphaTextures_[a_texture].live)
			return kInvalidAlphaInfo;
		auto finite = [](float a_value, float a_fallback) { return std::isfinite(a_value) ? a_value : a_fallback; };
		AlphaInfoGpu info{};
		info.uvBase = a_uvOffset;
		info.triangles = a_triangles;
		info.texture = alphaTextures_[a_texture].gpuIndex;
		info.flags = 0;
		info.threshold = std::clamp(finite(a_material.threshold, 0.5f), 0.0f, 1.0f);
		info.alphaScale = std::clamp(finite(a_material.alphaScale, 1.0f), 0.0f, 4.0f);
		info.textureSize = static_cast<float>(alphaTextures_[a_texture].sourceSize);  // (47q)
		info.textureLevels = static_cast<float>(alphaTextures_[a_texture].sourceMips);
		info.uvTransform[0] = finite(a_material.uvTransform[0], 0.0f);
		info.uvTransform[1] = finite(a_material.uvTransform[1], 0.0f);
		info.uvTransform[2] = finite(a_material.uvTransform[2], 1.0f);
		info.uvTransform[3] = finite(a_material.uvTransform[3], 1.0f);
		return AllocateAlphaInfoSlot(info);
	}

	void Backend::ReleaseAlphaInfo(std::uint32_t a_info)
	{
		if (a_info >= alphaInfos_.size() || !alphaInfoUsed_[a_info])
			return;
		// (47s) Its classified variant loses this user.
		if (const std::uint32_t variant = slotVariant_[a_info]; variant != UINT32_MAX) {
			slotVariant_[a_info] = UINT32_MAX;
			ReleaseVariantRef(variant);
		}
		slotBase_[a_info] = SlotBase{};
		// Each command context traces with its own copy of the table: the slot can be reused at once.
		alphaInfos_[a_info] = {};
		alphaInfoUsed_[a_info] = 0;
		freeAlphaInfos_.push_back(a_info);
		alphaInfoLive_ -= std::min<std::uint32_t>(alphaInfoLive_, 1);
		++alphaInfoVersion_;
		alphaStats_.infos = alphaInfoLive_;
	}

	bool Backend::EnsureAlphaInfoBuffer(CommandContext& a_context)
	{
		const auto count = static_cast<std::uint32_t>(alphaInfos_.size());
		if (count == 0)
			return false;
		if (!a_context.alphaInfoBuffer || a_context.alphaInfoCapacity < count) {
			if (a_context.alphaInfoBuffer) {
				// This context's previous submission has completed (it is the free one).
				a_context.alphaInfoBuffer->Unmap(0, nullptr);
				alphaStats_.infoBytes -= std::min<std::uint64_t>(alphaStats_.infoBytes, static_cast<std::uint64_t>(a_context.alphaInfoCapacity) * sizeof(AlphaInfoGpu));
				a_context.alphaInfoBuffer.Reset();
				a_context.alphaInfoMapped = nullptr;
				a_context.alphaInfoCapacity = 0;
			}
			const std::uint32_t capacity = std::max<std::uint32_t>(256, std::bit_ceil(count));
			a_context.alphaInfoBuffer = CreateBuffer(static_cast<std::uint64_t>(capacity) * sizeof(AlphaInfoGpu), D3D12_HEAP_TYPE_UPLOAD,
				D3D12_RESOURCE_STATE_GENERIC_READ, false);
			if (!a_context.alphaInfoBuffer || FAILED(a_context.alphaInfoBuffer->Map(0, nullptr, &a_context.alphaInfoMapped)) || !a_context.alphaInfoMapped) {
				a_context.alphaInfoBuffer.Reset();
				a_context.alphaInfoMapped = nullptr;
				SetAlphaFault("could not create the per-shape record buffer");
				return false;
			}
			a_context.alphaInfoBuffer->SetName(L"CS RayTracing alpha records");
			a_context.alphaInfoCapacity = capacity;
			a_context.alphaInfoVersion = 0;
			alphaStats_.infoBytes += static_cast<std::uint64_t>(capacity) * sizeof(AlphaInfoGpu);
		}
		if (a_context.alphaInfoVersion != alphaInfoVersion_) {
			std::memcpy(a_context.alphaInfoMapped, alphaInfos_.data(), static_cast<std::size_t>(count) * sizeof(AlphaInfoGpu));
			a_context.alphaInfoVersion = alphaInfoVersion_;
		}
		return true;
	}

	void Backend::ReleaseAlphaResources()
	{
		if (!AlphaIdle())
			return;
		if (!alphaUv_ && !alphaLayers12_ && alphaInfos_.empty())
			return;
		if (alphaUv_)
			Retire(alphaUv_, 0, nullptr);
		alphaUv_.Reset();
		alphaUvFree_.clear();
		retiredAlphaUv_.clear();
		alphaUvUsed_ = 0;
		if (alphaLayers12_)
			Retire(alphaLayers12_, 0, nullptr);
		alphaLayers12_.Reset();
		for (auto& uav : alphaLayerUavs_)
			uav.Reset();
		alphaLayers11_.Reset();
		alphaCopyCB_.Reset();
		alphaCopySampler_.Reset();
		alphaLayerFormat_ = DXGI_FORMAT_UNKNOWN;
		alphaLayerCount_ = 0;
		alphaLayersFailed_ = false;
		nextAlphaLayer_ = 0;
		freeAlphaLayers_.clear();
		retiredAlphaLayers_.clear();
		alphaCopyQueue_.clear();
		alphaInfos_.clear();
		alphaInfoUsed_.clear();
		slotVariant_.clear();
		slotBase_.clear();
		freeAlphaInfos_.clear();
		++alphaInfoVersion_;
		alphaStats_.uvBytes = alphaStats_.uvBytesUsed = alphaStats_.layerBytes = 0;
		// (47s) Alpha Classify: no record left, so no variant either; its bake buffers go too (behind the fence).
		bakeQueue_.clear();
		ommBuildQueue_.clear();  // (47t)
		variantUploads_.clear();
		if (bakeReadback_ && bakeMapped_)
			bakeReadback_->Unmap(0, nullptr);
		bakeMapped_ = nullptr;
		if (bakeReadback_)
			Retire(bakeReadback_, 0, nullptr);
		if (bakeOut_)
			Retire(bakeOut_, 0, nullptr);
		bakeReadback_.Reset();
		bakeOut_.Reset();
		classifyStats_.bakeBufferBytes = 0;
		logger::info("[RayTracing] Alpha-tested casters: buffers released");
	}
}
