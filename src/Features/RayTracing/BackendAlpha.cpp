// (45h) Alpha-tested casters on the ray tracer's D3D12 side: the textures their cut-outs come
// from, their texcoords and the per-shape records the alpha variant of TraceShadowsCS reads.
//
// Textures: a game texture created shareable (TextureSharing.h) is opened on D3D12 and sampled at
// full resolution through a bindless table (no copy, no VRAM). Any other one gets a 256x256 alpha
// copy (5 mips) in a texture array shared from D3D11, made by AlphaMaskCS on D3D11 before the
// frame's trace (the D3D12 queue waits for D3D11 on the GPU, so the trace sees it).
// Texcoords: one buffer, a range per alpha BLAS (3 corners per triangle, uint2 each).
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

		/// Formats a game texture may be sampled as on D3D12 (typeless and video formats are not).
		bool SampleableFormat(DXGI_FORMAT a_format)
		{
			switch (a_format) {
			case DXGI_FORMAT_BC1_UNORM:
			case DXGI_FORMAT_BC1_UNORM_SRGB:
			case DXGI_FORMAT_BC2_UNORM:
			case DXGI_FORMAT_BC2_UNORM_SRGB:
			case DXGI_FORMAT_BC3_UNORM:
			case DXGI_FORMAT_BC3_UNORM_SRGB:
			case DXGI_FORMAT_BC4_UNORM:
			case DXGI_FORMAT_BC5_UNORM:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
			case DXGI_FORMAT_R8G8B8A8_UNORM:
			case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8A8_UNORM:
			case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
			case DXGI_FORMAT_B8G8R8X8_UNORM:
			case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
			case DXGI_FORMAT_R8_UNORM:
			case DXGI_FORMAT_A8_UNORM:
			case DXGI_FORMAT_R8G8_UNORM:
			case DXGI_FORMAT_R16G16B16A16_FLOAT:
			case DXGI_FORMAT_R16G16B16A16_UNORM:
			case DXGI_FORMAT_R10G10B10A2_UNORM:
			case DXGI_FORMAT_B5G6R5_UNORM:
			case DXGI_FORMAT_B5G5R5A1_UNORM:
			case DXGI_FORMAT_B4G4R4A4_UNORM:
				return true;
			default:
				return false;
			}
		}
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
		alphaStats_.texturesShared = alphaStats_.texturesFallback = alphaStats_.texturesPending = 0;
		for (const auto& entry : alphaTextures_) {
			if (!entry.live)
				continue;
			if (entry.resource12)
				++alphaStats_.texturesShared;
			else
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
		std::erase_if(retiredAlphaSlots_, [&](const DeferredFree& a_entry) {
			if (a_entry.fence > a_completed)
				return false;
			freeAlphaSlots_.push_back(a_entry.value);
			return true;
		});
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

		// 1. The game's texture itself (created shareable while the switch was on).
		bool shared = false;
		if (!alphaStats_.sharedPathOff && (desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED) && !(desc.MiscFlags & D3D11_RESOURCE_MISC_SHARED_NTHANDLE)) {
			ComPtr<IDXGIResource> dxgiResource;
			HANDLE handle = nullptr;
			if (SUCCEEDED(a_texture->QueryInterface(IID_PPV_ARGS(&dxgiResource))) && SUCCEEDED(dxgiResource->GetSharedHandle(&handle)) && handle) {
				// A legacy (non-NT) handle, as upstream uses it: it is not closed.
				ComPtr<ID3D12Resource> resource;
				const HRESULT hr = device_->OpenSharedHandle(handle, IID_PPV_ARGS(&resource));
				if (SUCCEEDED(hr) && resource) {
					const auto rd = resource->GetDesc();
					if (rd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && rd.DepthOrArraySize == 1 && SampleableFormat(rd.Format)) {
						std::uint32_t slot = UINT32_MAX;
						if (!freeAlphaSlots_.empty()) {
							slot = freeAlphaSlots_.back();
							freeAlphaSlots_.pop_back();
						} else if (nextAlphaSlot_ < kAlphaTextureSlots) {
							slot = nextAlphaSlot_++;
						}
						if (slot == UINT32_MAX) {
							++alphaStats_.slotsFull;
						} else {
							auto cpu = descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
							cpu.ptr += static_cast<SIZE_T>(descriptorSize_) * (kAlphaDescriptorBase + slot);
							D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc{};
							srvDesc.Format = rd.Format;
							srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
							srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
							srvDesc.Texture2D.MipLevels = rd.MipLevels;
							device_->CreateShaderResourceView(resource.Get(), &srvDesc, cpu);
							entry.resource12 = std::move(resource);
							entry.gpuIndex = slot;
							entry.ready = true;
							shared = true;
							++alphaSharedOpened_;
						}
					} else {
						++alphaStats_.notShareable;
					}
				} else {
					++alphaStats_.openFailed;
					if (alphaStats_.openFailed <= 4)
						logger::warn("[RayTracing] Alpha-tested casters: D3D12 could not open a shared game texture ({}x{}, format {}): {}", desc.Width,
							desc.Height, static_cast<int>(desc.Format), Hr(hr));
					if (alphaSharedOpened_ == 0 && alphaStats_.openFailed >= 32) {
						alphaStats_.sharedPathOff = true;
						logger::warn("[RayTracing] Alpha-tested casters: no shared texture could be opened; 256x256 copies only this session");
					}
				}
			} else {
				++alphaStats_.notShareable;
			}
		} else {
			++alphaStats_.notShareable;
		}

		// 2. Otherwise a 256x256 alpha copy, made on D3D11 by RecordAlphaCopies.
		if (!shared) {
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
		}

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
		if (entry.resource12) {
			Retire(entry.resource12, 0, nullptr);
			retiredAlphaSlots_.push_back(DeferredFree{ RetireValue(), entry.gpuIndex, 0 });
		} else if (entry.gpuIndex != UINT32_MAX) {
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
		return CreateAlphaInfoAt(blas_[a_blas].uvOffset, blas_[a_blas].triangles, a_texture, a_material);
	}

	std::uint32_t Backend::CreateAlphaInfoAt(std::uint32_t a_uvOffset, std::uint32_t a_triangles, AlphaTexture a_texture, const AlphaMaterial& a_material)
	{
		if (!AlphaAvailable() || a_uvOffset == UINT32_MAX || a_triangles == 0 || a_texture >= alphaTextures_.size() || !alphaTextures_[a_texture].live)
			return kInvalidAlphaInfo;
		std::uint32_t slot;
		if (!freeAlphaInfos_.empty()) {
			slot = freeAlphaInfos_.back();
			freeAlphaInfos_.pop_back();
		} else if (alphaInfos_.size() < kAlphaMaxInfos) {
			slot = static_cast<std::uint32_t>(alphaInfos_.size());
			alphaInfos_.emplace_back();
		} else {
			++alphaStats_.infosFull;
			return kInvalidAlphaInfo;
		}
		auto finite = [](float a_value, float a_fallback) { return std::isfinite(a_value) ? a_value : a_fallback; };
		AlphaInfoGpu info{};
		info.uvBase = a_uvOffset;
		info.triangles = a_triangles;
		info.texture = alphaTextures_[a_texture].gpuIndex;
		info.flags = 0;
		info.threshold = std::clamp(finite(a_material.threshold, 0.5f), 0.0f, 1.0f);
		info.alphaScale = std::clamp(finite(a_material.alphaScale, 1.0f), 0.0f, 4.0f);
		info.uvTransform[0] = finite(a_material.uvTransform[0], 0.0f);
		info.uvTransform[1] = finite(a_material.uvTransform[1], 0.0f);
		info.uvTransform[2] = finite(a_material.uvTransform[2], 1.0f);
		info.uvTransform[3] = finite(a_material.uvTransform[3], 1.0f);
		alphaInfos_[slot] = info;
		++alphaInfoLive_;
		++alphaInfoVersion_;
		alphaStats_.infos = alphaInfoLive_;
		return slot;
	}

	void Backend::ReleaseAlphaInfo(std::uint32_t a_info)
	{
		if (a_info >= alphaInfos_.size() || alphaInfos_[a_info].triangles == 0)
			return;
		// Each command context traces with its own copy of the table: the slot can be reused at once.
		alphaInfos_[a_info] = {};
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
		freeAlphaInfos_.clear();
		++alphaInfoVersion_;
		alphaStats_.uvBytes = alphaStats_.uvBytesUsed = alphaStats_.layerBytes = 0;
		logger::info("[RayTracing] Alpha-tested casters: buffers released");
	}
}
