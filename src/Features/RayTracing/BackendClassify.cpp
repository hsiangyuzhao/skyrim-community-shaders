// (47s) Alpha Classify (OMM-lite, DXR 1.1) on the ray tracer's D3D12 side.
//
// Per alpha-tested mesh and material (a "variant": the base BLAS or character part, the cut-out texture, threshold, material alpha and
// texcoord transform) a GPU bake (AlphaClassifyCS) sorts every triangle against its cut-out exactly as the trace's alpha test would see
// it, for every ray (conservatively: any doubt is "mixed"):
//   fully opaque     -> an OPAQUE geometry of the same BLAS (the hardware commits it without returning to the shader),
//   fully transparent -> left out of the BLAS,
//   mixed            -> the non-opaque geometry 0 the candidate loop tests as before (its record lists only these, in order).
// The classes are read back (one uint per triangle, a few frames later) and the classified copies are made from data kept on the CPU:
// a static BLAS from its kept geometry (built through the usual queue), a swaying copy's index list from its wind source's indices, a
// character part's index list and record from the part's. The TLAS then names the classified BLAS with the classified record
// (instance ID = record + 1). Results are those of the unclassified trace for every ray the alpha test decides (the triangles it would
// have kept or rejected are kept or left out as such); the per-ray candidate budgets now count only the mixed triangles.
// A pass that culls cut-outs (the point lights' / RTAO's non-alpha variants: CULL_NON_OPAQUE) would see the opaque geometry: on such
// frames the classified instances are forced non-opaque (the opaque geometry then reaches AlphaBlocks, which keeps it at once).
//
// Off (SetAlphaClassify(false)): no bake, no classified instance; the variants go once nothing uses them (swaying copies and character
// parts are rebuilt unclassified over the next frames).

#include "Backend.h"

#include "Utils/D3D.h"

#include <algorithm>
#include <bit>
#include <cstring>

// Generated at build time from Shaders/AlphaClassifyCS.hlsl by the Windows SDK's dxc (CMakeLists.txt).
#include "RayTracing/AlphaClassifyCS.dxil.h"

namespace RayTracingRT
{
	namespace
	{
		std::string BakeHr(HRESULT a_hr) { return std::format("0x{:08X}", static_cast<std::uint32_t>(a_hr)); }

		std::uint64_t BakeAlign(std::uint64_t a_value, std::uint64_t a_alignment) { return (a_value + a_alignment - 1) & ~(a_alignment - 1); }

		D3D12_RESOURCE_BARRIER BakeTransition(ID3D12Resource* a_resource, D3D12_RESOURCE_STATES a_before, D3D12_RESOURCE_STATES a_after)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = a_resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = a_before;
			barrier.Transition.StateAfter = a_after;
			return barrier;
		}

		D3D12_RESOURCE_BARRIER BakeUav(ID3D12Resource* a_resource)
		{
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			barrier.UAV.pResource = a_resource;
			return barrier;
		}

		constexpr std::uint32_t kBakeGroupsX = 1024;  // AlphaClassifyCS: one group per triangle, rows of this many

		float HalfToFloat(std::uint16_t a_h)
		{
			const std::uint32_t sign = static_cast<std::uint32_t>(a_h & 0x8000u) << 16;
			std::uint32_t exp = (a_h >> 10) & 0x1Fu;
			std::uint32_t man = a_h & 0x3FFu;
			if (exp == 0x1Fu)
				return std::bit_cast<float>(sign | 0x7F800000u | (man << 13));
			if (exp == 0) {
				if (man == 0)
					return std::bit_cast<float>(sign);
				exp = 1;
				while (!(man & 0x400u)) {
					man <<= 1;
					--exp;
				}
				man &= 0x3FFu;
			}
			return std::bit_cast<float>(sign | ((exp + 112u) << 23) | (man << 13));
		}

		// The bake's work for one triangle (texel tests, an upper bound): per copy mip the box of its texcoords grown by the support
		// (the whole mip past one repeat), and a fixed part per group. a_packed: its record (uv0, uv1, uv2, alphas).
		std::uint64_t BakeCost(const std::uint32_t* a_packed, const float a_uvTransform[4], std::uint32_t a_mips)
		{
			float u[3], v[3];
			for (int c = 0; c < 3; ++c) {
				u[c] = HalfToFloat(static_cast<std::uint16_t>(a_packed[c] & 0xFFFFu)) * a_uvTransform[2] + a_uvTransform[0];
				v[c] = HalfToFloat(static_cast<std::uint16_t>(a_packed[c] >> 16)) * a_uvTransform[3] + a_uvTransform[1];
				if (!std::isfinite(u[c]) || !std::isfinite(v[c]) || std::abs(u[c]) > 4096.0f || std::abs(v[c]) > 4096.0f)
					return 64;  // mixed at once in the shader
			}
			const float du = std::max({ u[0], u[1], u[2] }) - std::min({ u[0], u[1], u[2] });
			const float dv = std::max({ v[0], v[1], v[2] }) - std::min({ v[0], v[1], v[2] });
			std::uint64_t cost = 64;
			for (std::uint32_t mip = 0; mip < a_mips; ++mip) {
				const float s = static_cast<float>(256u >> mip);
				const float ex = du * s + 3.0f, ey = dv * s + 3.0f;
				cost += (ex > s + 2.0f || ey > s + 2.0f) ? static_cast<std::uint64_t>(s * s) : static_cast<std::uint64_t>(ex * ey);
			}
			return cost;
		}
	}

	// ---------------------------------------------------------------------------------------------
	// Pipeline and buffers
	// ---------------------------------------------------------------------------------------------

	bool Backend::CreateBakePipeline()
	{
		// Constants b0, texcoords t0 (root SRV), classes u0 (root UAV), table: copy layers t1 (the alpha table's first slot).
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		range.BaseShaderRegister = 1;
		range.OffsetInDescriptorsFromTableStart = 0;
		D3D12_ROOT_PARAMETER params[4]{};
		params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
		params[0].Constants.ShaderRegister = 0;
		params[0].Constants.Num32BitValues = kBakeConstants;
		params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
		params[1].Descriptor.ShaderRegister = 0;
		params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[2].Descriptor.ShaderRegister = 0;
		params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
		params[3].DescriptorTable.NumDescriptorRanges = 1;
		params[3].DescriptorTable.pDescriptorRanges = &range;
		params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 4;
		rootDesc.pParameters = params;
		ComPtr<ID3DBlob> blob, error;
		HRESULT hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error);
		if (FAILED(hr) || FAILED(hr = device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&bakeRootSignature_)))) {
			bakeRootSignature_.Reset();
			logger::warn("[RayTracing] Alpha Classify unavailable: the bake root signature failed ({})", BakeHr(hr));
			return false;
		}
		D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
		psoDesc.pRootSignature = bakeRootSignature_.Get();
		psoDesc.CS.pShaderBytecode = g_AlphaClassifyCS;
		psoDesc.CS.BytecodeLength = sizeof(g_AlphaClassifyCS);
		if (FAILED(hr = device_->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&bakePipeline_)))) {
			bakePipeline_.Reset();
			bakeRootSignature_.Reset();
			logger::warn("[RayTracing] Alpha Classify unavailable: the bake shader was rejected by the driver ({})", BakeHr(hr));
			return false;
		}
		return true;
	}

	bool Backend::EnsureBakeBuffers()
	{
		if (bakeOut_ && bakeReadback_ && bakeMapped_)
			return true;
		if (bakeBuffersFailed_)
			return false;
		const std::uint64_t bytes = static_cast<std::uint64_t>(kContexts) * kBakeCapacity * sizeof(std::uint32_t);
		bakeOut_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
		bakeReadback_ = CreateBuffer(bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
		void* mapped = nullptr;
		if (!bakeOut_ || !bakeReadback_ || FAILED(bakeReadback_->Map(0, nullptr, &mapped)) || !mapped) {
			bakeOut_.Reset();
			bakeReadback_.Reset();
			bakeMapped_ = nullptr;
			bakeBuffersFailed_ = true;
			logger::warn("[RayTracing] Alpha Classify: could not create the {} MB bake buffers; classification off", bytes >> 20);
			return false;
		}
		bakeOut_->SetName(L"CS RayTracing alpha classify");
		bakeReadback_->SetName(L"CS RayTracing alpha classify read-back");
		bakeMapped_ = static_cast<const std::uint32_t*>(mapped);
		classifyStats_.bakeBufferBytes = bytes;
		logger::info("[RayTracing] Alpha Classify: bake buffers {} MB (+ the same in system memory)", bytes >> 20);
		return true;
	}

	// ---------------------------------------------------------------------------------------------
	// Variants
	// ---------------------------------------------------------------------------------------------

	void Backend::SetAlphaClassify(bool a_on)
	{
		if (a_on == classifyOn_)
			return;
		classifyOn_ = a_on;
		if (!a_on) {
			// The records' variants go once the swaying copies and character parts built with them were rebuilt.
			for (std::uint32_t slot = 0; slot < slotVariant_.size(); ++slot) {
				if (const std::uint32_t variant = slotVariant_[slot]; variant != UINT32_MAX) {
					slotVariant_[slot] = UINT32_MAX;
					ReleaseVariantRef(variant);
				}
			}
		}
		logger::info("[RayTracing] Alpha Classify {}", a_on ? "on" : "off");
		UpdateClassifyStats();
	}

	void Backend::SetVariantState(AlphaVariant& a_variant, VariantState a_state)
	{
		const bool wasReady = a_variant.state == VariantState::Ready;
		const bool ready = a_state == VariantState::Ready;
		a_variant.state = a_state;
		if (ready && !wasReady && !a_variant.counted) {
			classifyStats_.opaqueTriangles += a_variant.opaque;
			classifyStats_.droppedTriangles += a_variant.dropped;
			classifyStats_.mixedTriangles += a_variant.mixed;
			a_variant.counted = true;
		} else if (!ready && a_variant.counted) {
			classifyStats_.opaqueTriangles -= std::min<std::uint64_t>(classifyStats_.opaqueTriangles, a_variant.opaque);
			classifyStats_.droppedTriangles -= std::min<std::uint64_t>(classifyStats_.droppedTriangles, a_variant.dropped);
			classifyStats_.mixedTriangles -= std::min<std::uint64_t>(classifyStats_.mixedTriangles, a_variant.mixed);
			a_variant.counted = false;
		}
	}

	std::uint32_t Backend::VariantForSlot(std::uint32_t a_slot)
	{
		if (a_slot >= alphaInfos_.size() || !alphaInfoUsed_[a_slot])
			return UINT32_MAX;
		if (slotVariant_[a_slot] != UINT32_MAX)
			return slotVariant_[a_slot];
		if (!classifyOn_ || !bakePipeline_ || bakeBuffersFailed_)
			return UINT32_MAX;
		const SlotBase base = slotBase_[a_slot];
		if (base.handle == UINT32_MAX || base.texture >= alphaTextures_.size())
			return UINT32_MAX;
		const AlphaInfoGpu& info = alphaInfos_[a_slot];
		// The base must still be the one the record was made for.
		if (base.kind == VariantKind::Static) {
			if (base.handle >= blas_.size() || blas_[base.handle].state == BlasState::Free || blas_[base.handle].wind != UINT32_MAX ||
				blas_[base.handle].uvOffset != info.uvBase)
				return UINT32_MAX;
		} else if (base.handle >= skins_.size() || !skins_[base.handle].live || skins_[base.handle].uvOffset != info.uvBase) {
			return UINT32_MAX;
		}
		VariantKey key;
		key.words = { static_cast<std::uint32_t>(base.kind), base.handle, base.texture, info.texture, std::bit_cast<std::uint32_t>(info.threshold),
			std::bit_cast<std::uint32_t>(info.alphaScale), std::bit_cast<std::uint32_t>(info.uvTransform[0]), std::bit_cast<std::uint32_t>(info.uvTransform[1]),
			std::bit_cast<std::uint32_t>(info.uvTransform[2]), std::bit_cast<std::uint32_t>(info.uvTransform[3]) };
		std::uint32_t index;
		if (const auto it = variantIndex_.find(key); it != variantIndex_.end()) {
			index = it->second;
		} else {
			if (!freeVariants_.empty()) {
				index = freeVariants_.back();
				freeVariants_.pop_back();
			} else {
				index = static_cast<std::uint32_t>(variants_.size());
				variants_.emplace_back();
			}
			auto& variant = variants_[index];
			variant = {};
			variant.live = true;
			variant.serial = ++variantSerial_;
			variant.kind = base.kind;
			variant.base = base.handle;
			variant.texture = base.texture;
			variant.info = info;
			variant.key = key;
			variant.state = VariantState::Pending;
			variant.queued = true;
			bakeQueue_.push_back(index);
			variantIndex_.emplace(key, index);
			if (base.kind == VariantKind::Static)
				blas_[base.handle].variants.push_back(index);
		}
		slotVariant_[a_slot] = index;
		AddVariantRef(index);
		return index;
	}

	bool Backend::VariantReady(std::uint32_t a_variant)
	{
		if (a_variant >= variants_.size())
			return false;
		auto& variant = variants_[a_variant];
		if (!variant.live)
			return false;
		if (variant.state == VariantState::Ready)
			return true;
		if (variant.state != VariantState::Building)
			return false;
		// A static variant is in use once its BLAS build is recorded (its mixed texcoords go up in the same list, before the TLAS).
		if (variant.kind == VariantKind::Static && variant.blas != kInvalidBlas && variant.record != kInvalidAlphaInfo && IsBlasBuilt(variant.blas)) {
			SetVariantState(variant, VariantState::Ready);
			return true;
		}
		return false;
	}

	void Backend::AddVariantRef(std::uint32_t a_variant)
	{
		if (a_variant < variants_.size() && variants_[a_variant].live)
			++variants_[a_variant].refs;
	}

	void Backend::ReleaseVariantRef(std::uint32_t a_variant)
	{
		if (a_variant >= variants_.size() || !variants_[a_variant].live)
			return;
		auto& variant = variants_[a_variant];
		if (variant.refs > 1) {
			--variant.refs;
			return;
		}
		variant.refs = 0;
		FreeVariant(a_variant);
	}

	void Backend::FreeVariant(std::uint32_t a_variant)
	{
		auto& variant = variants_[a_variant];
		if (!variant.live)
			return;
		SetVariantState(variant, VariantState::Dead);  // out of the triangle counts
		if (const auto it = variantIndex_.find(variant.key); it != variantIndex_.end() && it->second == a_variant)
			variantIndex_.erase(it);
		if (variant.kind == VariantKind::Static && variant.base < blas_.size())
			std::erase(blas_[variant.base].variants, a_variant);
		if (variant.queued)
			std::erase(bakeQueue_, a_variant);
		std::erase_if(variantUploads_, [&](const auto& a_entry) { return a_entry.first == a_variant; });
		// Everything behind the fence (a TLAS / list in flight may still use it).
		const BlasHandle blas = variant.blas;
		const std::uint32_t record = variant.record;
		if (variant.skinIndex.Valid())
			skinStaticPool_.Free(variant.skinIndex, RetireValue());
		if (variant.uvOffset != UINT32_MAX)
			FreeAlphaUv(variant.uvOffset, variant.uvCount);
		for (const auto& entry : variant.wind)
			if (entry.range.Valid())
				skinStaticPool_.Free(entry.range, RetireValue());
		variant = {};
		freeVariants_.push_back(a_variant);
		if (blas != kInvalidBlas)
			ReleaseBlas(blas);
		if (record != kInvalidAlphaInfo)
			ReleaseAlphaInfo(record);
	}

	void Backend::OrphanVariants(std::vector<std::uint32_t>& a_variants)
	{
		for (const std::uint32_t index : a_variants) {
			if (index >= variants_.size() || !variants_[index].live)
				continue;
			auto& variant = variants_[index];
			// The handle may be reused by another BLAS / part: never found again.
			if (const auto it = variantIndex_.find(variant.key); it != variantIndex_.end() && it->second == index)
				variantIndex_.erase(it);
			variant.base = UINT32_MAX;
			if (variant.state == VariantState::Pending || variant.state == VariantState::Baking) {
				if (variant.queued) {
					std::erase(bakeQueue_, index);
					variant.queued = false;
				}
				SetVariantState(variant, VariantState::Dead);
			}
		}
		a_variants.clear();
	}

	void Backend::RequeueBake(std::uint32_t a_variant)
	{
		if (a_variant >= variants_.size() || !variants_[a_variant].live)
			return;
		auto& variant = variants_[a_variant];
		if (variant.state != VariantState::Baking)
			return;
		// A new generation: parts still in flight are ignored when they come back.
		variant.serial = ++variantSerial_;
		variant.classes.clear();
		variant.nextBake = variant.received = 0;
		SetVariantState(variant, VariantState::Pending);
		if (!variant.queued) {
			variant.queued = true;
			bakeQueue_.push_back(a_variant);
		}
	}

	void Backend::FinishBake(std::uint32_t a_variant, const std::uint32_t* a_classes, std::uint32_t a_triangles)
	{
		auto& variant = variants_[a_variant];
		if (a_triangles != variant.info.triangles || variant.base == UINT32_MAX) {
			SetVariantState(variant, variant.base == UINT32_MAX ? VariantState::Dead : VariantState::NoGain);
			return;
		}
		std::uint32_t mixed = 0, opaque = 0;
		for (std::uint32_t t = 0; t < a_triangles; ++t) {
			const std::uint32_t c = a_classes[t] & 0xFFFFu;
			mixed += c == 0u ? 1u : 0u;
			opaque += c == 1u ? 1u : 0u;
		}
		const std::uint32_t dropped = a_triangles - mixed - opaque;
		// Nothing to gain (every triangle mixed), or nothing left (every triangle see-through: no BLAS can hold that; the original,
		// whose every candidate the alpha test rejects, stays).
		if (mixed == a_triangles || mixed + opaque == 0) {
			SetVariantState(variant, VariantState::NoGain);
			return;
		}
		// The classified order: the mixed triangles in their order (geometry 0, the record), then the opaque ones (geometry 1).
		variant.order.clear();
		variant.order.reserve(static_cast<std::size_t>(mixed) + opaque);
		for (std::uint32_t t = 0; t < a_triangles; ++t)
			if ((a_classes[t] & 0xFFFFu) == 0u)
				variant.order.push_back(t);
		for (std::uint32_t t = 0; t < a_triangles; ++t)
			if ((a_classes[t] & 0xFFFFu) == 1u)
				variant.order.push_back(t);
		variant.mixed = mixed;
		variant.opaque = opaque;
		variant.dropped = dropped;

		if (variant.kind == VariantKind::Skin) {
			// The index list and record go up in RecordVariantUploads (the part's next frames).
			SetVariantState(variant, VariantState::Building);
			variantUploads_.emplace_back(a_variant, UINT32_MAX);
			return;
		}
		// Static: a classified copy of the base BLAS (the same vertices), built through the usual queue.
		const auto& base = blas_[variant.base];
		const ChunkGeometry* source = base.source.get();
		if (!source || source->Triangles() != a_triangles || source->alphaUV.size() != static_cast<std::size_t>(a_triangles) * 4) {
			SetVariantState(variant, VariantState::NoGain);
			return;
		}
		auto geometry = std::make_unique<ChunkGeometry>();
		geometry->positions = source->positions;
		geometry->indices.resize(variant.order.size() * 3);
		geometry->alphaUV.resize(static_cast<std::size_t>(mixed) * 4);
		for (std::size_t k = 0; k < variant.order.size(); ++k) {
			const std::size_t t = variant.order[k];
			std::copy_n(source->indices.data() + t * 3, 3, geometry->indices.data() + k * 3);
			if (k < mixed)
				std::copy_n(source->alphaUV.data() + t * 4, 4, geometry->alphaUV.data() + k * 4);
		}
		geometry->opaqueFrom = mixed;
		const BlasHandle handle = CreateBlas(std::move(geometry));
		if (handle == kInvalidBlas) {
			SetVariantState(variant, VariantState::NoGain);
			return;
		}
		AlphaInfoGpu info = variant.info;
		info.uvBase = mixed > 0 ? blas_[handle].uvOffset : 0u;
		info.triangles = mixed;
		info.flags = kAlphaInfoClassified;
		const std::uint32_t record = AllocateAlphaInfoSlot(info);
		if (record == kInvalidAlphaInfo) {
			ReleaseBlas(handle);
			SetVariantState(variant, VariantState::NoGain);
			return;
		}
		variant.blas = handle;
		variant.record = record;
		variant.bytes = static_cast<std::uint64_t>(mixed) * 16;
		SetVariantState(variant, VariantState::Building);
	}

	// ---------------------------------------------------------------------------------------------
	// Bakes
	// ---------------------------------------------------------------------------------------------

	void Backend::RecordAlphaBakes(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, std::size_t a_index)
	{
		a_context.bakeRecorded.clear();
		a_context.bakeTimed = false;
		classifyStats_.bakesLastFrame = 0;
		classifyStats_.bakeTrianglesLastFrame = 0;
		if (!classifyOn_ || !bakePipeline_ || bakeQueue_.empty() || !alphaUv_ || !alphaLayers12_ || !descriptorHeap_)
			return;
		if (!EnsureBakeBuffers())
			return;
		std::vector<BakeJob> jobs;
		std::uint32_t total = 0;
		const std::uint64_t budget = transition_ ? kBakeCostPerFrame * kBakeCostTransitionScale : kBakeCostPerFrame;
		std::uint64_t spent = 0;
		const std::size_t scan = std::min<std::size_t>(bakeQueue_.size(), kBakeScanPerFrame);
		std::vector<std::uint32_t> unfinished;  // baked in part this frame: first in the queue next frame
		for (std::size_t i = 0; i < scan && jobs.size() < kBakeJobsPerFrame && spent < budget && total < kBakeCapacity; ++i) {
			const std::uint32_t index = bakeQueue_.front();
			bakeQueue_.pop_front();
			if (index >= variants_.size() || !variants_[index].live)
				continue;
			auto& variant = variants_[index];
			variant.queued = false;
			const bool resume = variant.state == VariantState::Baking && variant.nextBake < variant.info.triangles;
			if (variant.state != VariantState::Pending && !resume)
				continue;
			const AlphaInfoGpu& info = variant.info;
			// Only the 256x256 copy layers are classified (the bindless branch is not used today).
			if (!(info.texture & kAlphaFallbackBit) || info.triangles == 0 || variant.base == UINT32_MAX) {
				SetVariantState(variant, variant.base == UINT32_MAX ? VariantState::Dead : VariantState::NoGain);
				continue;
			}
			// The copy recorded before a ready wait already queued (the prep list runs behind it); the base's record uploaded.
			const bool textureReady = variant.texture < alphaTextures_.size() && alphaTextures_[variant.texture].live &&
			                          alphaTextures_[variant.texture].gpuIndex == info.texture && alphaTextures_[variant.texture].ready &&
			                          alphaTextures_[variant.texture].readySerial < waitSerial_;
			const std::uint32_t* packed = nullptr;
			if (variant.kind == VariantKind::Static) {
				if (variant.base < blas_.size() && IsBlasBuilt(variant.base) && blas_[variant.base].uvOffset == info.uvBase && blas_[variant.base].source &&
					blas_[variant.base].source->alphaUV.size() == static_cast<std::size_t>(info.triangles) * 4)
					packed = blas_[variant.base].source->alphaUV.data();
			} else if (variant.base < skins_.size() && skins_[variant.base].live && skins_[variant.base].uploaded && skins_[variant.base].uvOffset == info.uvBase &&
					   skins_[variant.base].uvCpu.size() == static_cast<std::size_t>(info.triangles) * 4 && !skins_[variant.base].indicesCpu.empty()) {
				packed = skins_[variant.base].uvCpu.data();
			}
			if (!textureReady || !packed) {
				variant.queued = true;
				bakeQueue_.push_back(index);
				continue;
			}
			// This frame's part: as many triangles as the budget allows (at least one in the frame).
			const std::uint32_t first = resume ? variant.nextBake : 0u;
			std::uint32_t count = 0;
			while (first + count < info.triangles && total + count < kBakeCapacity) {
				const std::uint64_t cost = BakeCost(packed + static_cast<std::size_t>(first + count) * 4, info.uvTransform, kBakeMips);
				if (spent + cost > budget && (spent > 0 || count > 0))
					break;
				spent += cost;
				++count;
			}
			if (count == 0) {
				variant.queued = true;
				bakeQueue_.push_front(index);  // next frame, first
				break;
			}
			if (!resume) {
				variant.classes.assign(info.triangles, 0u);
				variant.nextBake = variant.received = 0;
				SetVariantState(variant, VariantState::Baking);
			}
			jobs.push_back(BakeJob{ index, variant.serial, total, first, count });
			total += count;
			variant.nextBake = first + count;
			if (variant.nextBake < info.triangles) {
				variant.queued = true;
				unfinished.push_back(index);
			}
		}
		for (auto it = unfinished.rbegin(); it != unfinished.rend(); ++it)
			bakeQueue_.push_front(*it);
		if (jobs.empty())
			return;

		ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
		a_list->SetDescriptorHeaps(1, heaps);
		const UINT queryBase = static_cast<UINT>(kContexts * 8 + a_index * 2);
		if (timestampHeap_)
			a_list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase);
		a_list->SetComputeRootSignature(bakeRootSignature_.Get());
		a_list->SetPipelineState(bakePipeline_.Get());
		const std::uint64_t region = static_cast<std::uint64_t>(a_index) * kBakeCapacity * sizeof(std::uint32_t);
		a_list->SetComputeRootShaderResourceView(1, alphaUv_->GetGPUVirtualAddress());
		a_list->SetComputeRootUnorderedAccessView(2, bakeOut_->GetGPUVirtualAddress() + region);
		auto table = descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
		table.ptr += static_cast<UINT64>(descriptorSize_) * (kAlphaDescriptorBase - 1);
		a_list->SetComputeRootDescriptorTable(3, table);
		for (const auto& job : jobs) {
			const AlphaInfoGpu& info = variants_[job.variant].info;
			const std::uint32_t groupsX = std::min(job.triangles, kBakeGroupsX);
			const std::uint32_t groupsY = (job.triangles + groupsX - 1) / groupsX;
			const std::uint32_t constants[kBakeConstants] = { info.uvBase + job.first * 2u, job.triangles, info.texture & 0xFFFFu, job.outOffset,
				std::bit_cast<std::uint32_t>(info.threshold), std::bit_cast<std::uint32_t>(info.alphaScale), groupsX, kBakeMips,
				std::bit_cast<std::uint32_t>(info.uvTransform[0]), std::bit_cast<std::uint32_t>(info.uvTransform[1]),
				std::bit_cast<std::uint32_t>(info.uvTransform[2]), std::bit_cast<std::uint32_t>(info.uvTransform[3]) };
			a_list->SetComputeRoot32BitConstants(0, kBakeConstants, constants, 0);
			a_list->Dispatch(groupsX, groupsY, 1);
		}
		if (timestampHeap_) {
			a_list->EndQuery(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase + 1);
			if (timestampReadback_)
				a_list->ResolveQueryData(timestampHeap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, queryBase, 2, timestampReadback_.Get(), sizeof(std::uint64_t) * queryBase);
		}
		D3D12_RESOURCE_BARRIER toCopy[2] = { BakeUav(bakeOut_.Get()),
			BakeTransition(bakeOut_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE) };
		a_list->ResourceBarrier(2, toCopy);
		a_list->CopyBufferRegion(bakeReadback_.Get(), region, bakeOut_.Get(), region, static_cast<std::uint64_t>(total) * sizeof(std::uint32_t));
		const auto back = BakeTransition(bakeOut_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		a_list->ResourceBarrier(1, &back);

		a_context.bakeRecorded = std::move(jobs);
		a_context.bakeTimed = timestampHeap_ && timestampReadback_;
		classifyStats_.bakesLastFrame = static_cast<std::uint32_t>(a_context.bakeRecorded.size());
		classifyStats_.bakeTrianglesLastFrame = total;
		classifyStats_.bakesTotal += a_context.bakeRecorded.size();
		classifyStats_.bakeTrianglesTotal += total;
	}

	void Backend::CommitBakes(CommandContext& a_context)
	{
		if (a_context.bakeRecorded.empty())
			return;
		a_context.bakeJobs.insert(a_context.bakeJobs.end(), a_context.bakeRecorded.begin(), a_context.bakeRecorded.end());
		a_context.bakeRecorded.clear();
	}

	void Backend::DropBakes(CommandContext& a_context)
	{
		for (const auto& job : a_context.bakeRecorded)
			if (job.variant < variants_.size() && variants_[job.variant].live && variants_[job.variant].serial == job.serial)
				RequeueBake(job.variant);
		a_context.bakeRecorded.clear();
		a_context.bakeTimed = false;
	}

	void Backend::CollectBakes(std::size_t a_index)
	{
		auto& context = contexts_[a_index];
		if (context.bakeJobs.empty())
			return;
		if (bakeMapped_) {
			const std::uint32_t* region = bakeMapped_ + static_cast<std::size_t>(a_index) * kBakeCapacity;
			for (const auto& job : context.bakeJobs) {
				if (job.variant >= variants_.size() || !variants_[job.variant].live || variants_[job.variant].serial != job.serial ||
					variants_[job.variant].state != VariantState::Baking || job.outOffset + job.triangles > kBakeCapacity)
					continue;
				auto& variant = variants_[job.variant];
				if (variant.classes.size() != variant.info.triangles || job.first + job.triangles > variant.info.triangles) {
					RequeueBake(job.variant);
					continue;
				}
				const std::uint32_t* classes = region + job.outOffset;
				bool complete = true;
				for (std::uint32_t t = 0; t < job.triangles && complete; ++t)
					complete = (classes[t] & 0xFFFF0000u) == kBakeMarker && (classes[t] & 0xFFFFu) <= 2u;
				if (!complete) {
					++classifyStats_.rejected;
					RequeueBake(job.variant);
					continue;
				}
				std::copy_n(classes, job.triangles, variant.classes.data() + job.first);
				variant.received += job.triangles;
				if (variant.received >= variant.info.triangles && variant.nextBake >= variant.info.triangles) {
					std::vector<std::uint32_t> all = std::move(variant.classes);
					variant.classes.clear();
					FinishBake(job.variant, all.data(), static_cast<std::uint32_t>(all.size()));
				}
			}
		} else {
			for (const auto& job : context.bakeJobs)
				if (job.variant < variants_.size() && variants_[job.variant].live && variants_[job.variant].serial == job.serial)
					RequeueBake(job.variant);
		}
		if (context.bakeTimed && timestampReadback_ && timestampFrequency_ != 0) {
			const SIZE_T offset = sizeof(std::uint64_t) * (kContexts * 8 + a_index * 2);
			D3D12_RANGE range{ offset, offset + sizeof(std::uint64_t) * 2 };
			void* mapped = nullptr;
			if (SUCCEEDED(timestampReadback_->Map(0, &range, &mapped)) && mapped) {
				const auto* t = reinterpret_cast<const std::uint64_t*>(static_cast<const std::byte*>(mapped) + offset);
				const std::uint64_t begin = t[0], end = t[1];
				D3D12_RANGE none{ 0, 0 };
				timestampReadback_->Unmap(0, &none);
				if (end >= begin)
					classifyStats_.lastBakeGpuMs = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(timestampFrequency_));
			}
		}
		context.bakeJobs.clear();
		context.bakeTimed = false;
	}

	// ---------------------------------------------------------------------------------------------
	// Swaying copies and character parts
	// ---------------------------------------------------------------------------------------------

	const Backend::WindIndex* Backend::FindWindIndex(std::uint32_t a_variant, std::uint32_t a_source) const
	{
		if (a_variant >= variants_.size())
			return nullptr;
		for (const auto& entry : variants_[a_variant].wind)
			if (entry.source == a_source)
				return &entry;
		return nullptr;
	}

	std::uint32_t Backend::WantedWindLayout(WindMesh& a_mesh)
	{
		if (a_mesh.opaque || !classifyOn_ || a_mesh.alphaSlot == UINT32_MAX)
			return UINT32_MAX;
		const std::uint32_t index = VariantForSlot(a_mesh.alphaSlot);
		if (index == UINT32_MAX || !VariantReady(index) || variants_[index].kind != VariantKind::Static)
			return UINT32_MAX;
		auto& variant = variants_[index];
		for (const auto& entry : variant.wind) {
			if (entry.source != a_mesh.source)
				continue;
			return entry.uploaded && !entry.failed ? index : UINT32_MAX;
		}
		// Its index list over this source: made in RecordVariantUploads (this frame's, before the wind builds, or the next).
		WindIndex entry;
		entry.source = a_mesh.source;
		variant.wind.push_back(entry);
		variantUploads_.emplace_back(index, static_cast<std::uint32_t>(variant.wind.size() - 1));
		return UINT32_MAX;
	}

	std::uint32_t Backend::WantedSkinLayout(const SkinShape& a_shape)
	{
		if (a_shape.uvOffset == UINT32_MAX || !classifyOn_ || a_shape.alphaSlot == UINT32_MAX)
			return UINT32_MAX;
		const std::uint32_t index = VariantForSlot(a_shape.alphaSlot);
		if (index == UINT32_MAX || variants_[index].kind != VariantKind::Skin || !VariantReady(index))
			return UINT32_MAX;
		return index;
	}

	UINT Backend::WindGeometryDescs(const WindMesh& a_mesh, std::uint32_t a_layout, D3D12_RAYTRACING_GEOMETRY_DESC (&o_descs)[2]) const
	{
		const auto& source = windSources_[a_mesh.source];
		D3D12_RAYTRACING_GEOMETRY_DESC desc{};
		desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		desc.Triangles.VertexBuffer.StartAddress = a_mesh.output.Valid() ? skinOutputPool_.Address(a_mesh.output) : 0;
		desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
		desc.Triangles.VertexCount = a_mesh.vertices;
		desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		const WindIndex* entry = a_layout != UINT32_MAX ? FindWindIndex(a_layout, a_mesh.source) : nullptr;
		if (!entry || !entry->range.Valid()) {
			desc.Flags = a_mesh.opaque ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			desc.Triangles.IndexBuffer = source.index.Valid() ? skinStaticPool_.Address(source.index) : 0;
			desc.Triangles.IndexCount = a_mesh.triangles * 3;
			o_descs[0] = desc;
			return 1;
		}
		const auto& variant = variants_[a_layout];
		const D3D12_GPU_VIRTUAL_ADDRESS indices = skinStaticPool_.Address(entry->range);
		UINT count = 0;
		if (variant.mixed > 0) {
			desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			desc.Triangles.IndexBuffer = indices;
			desc.Triangles.IndexCount = variant.mixed * 3;
			o_descs[count++] = desc;
		}
		if (variant.opaque > 0) {
			desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			desc.Triangles.IndexBuffer = indices + static_cast<std::uint64_t>(variant.mixed) * 3 * sizeof(std::uint32_t);
			desc.Triangles.IndexCount = variant.opaque * 3;
			o_descs[count++] = desc;
		}
		return count;
	}

	UINT Backend::SkinGeometryDescs(const SkinShape& a_shape, std::uint32_t a_layout, D3D12_RAYTRACING_GEOMETRY_DESC (&o_descs)[2]) const
	{
		D3D12_RAYTRACING_GEOMETRY_DESC desc{};
		desc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
		desc.Triangles.VertexBuffer.StartAddress = a_shape.output.Valid() ? skinOutputPool_.Address(a_shape.output) : 0;
		desc.Triangles.VertexBuffer.StrideInBytes = sizeof(float) * 3;
		desc.Triangles.VertexCount = a_shape.vertices;
		desc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
		desc.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
		if (a_layout == UINT32_MAX || a_layout >= variants_.size() || !variants_[a_layout].skinIndex.Valid()) {
			desc.Flags = a_shape.uvOffset == UINT32_MAX ? D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE : D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			desc.Triangles.IndexBuffer = a_shape.index.Valid() ? skinStaticPool_.Address(a_shape.index) : 0;
			desc.Triangles.IndexCount = a_shape.triangles * 3;
			o_descs[0] = desc;
			return 1;
		}
		const auto& variant = variants_[a_layout];
		const D3D12_GPU_VIRTUAL_ADDRESS indices = skinStaticPool_.Address(variant.skinIndex);
		UINT count = 0;
		if (variant.mixed > 0) {
			desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_NONE;
			desc.Triangles.IndexBuffer = indices;
			desc.Triangles.IndexCount = variant.mixed * 3;
			o_descs[count++] = desc;
		}
		if (variant.opaque > 0) {
			desc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			desc.Triangles.IndexBuffer = indices + static_cast<std::uint64_t>(variant.mixed) * 3 * sizeof(std::uint32_t);
			desc.Triangles.IndexCount = variant.opaque * 3;
			o_descs[count++] = desc;
		}
		return count;
	}

	void Backend::RecordVariantUploads(ID3D12GraphicsCommandList4* a_list)
	{
		if (variantUploads_.empty() || failed_)
			return;
		struct Pending
		{
			std::uint32_t variant;
			std::uint32_t wind;  // entry, or UINT32_MAX: the character part
			ArenaAlloc upload;
			ArenaAlloc range;
			std::uint64_t indexBytes;
			std::uint32_t uvOffset, uvCount;
			std::uint64_t uvAt;  // inside the upload range
		};
		std::vector<Pending> batch;
		std::vector<std::pair<std::uint32_t, std::uint32_t>> later;
		std::uint64_t batchBytes = 0;
		for (const auto& [index, windEntry] : variantUploads_) {
			if (index >= variants_.size() || !variants_[index].live)
				continue;
			auto& variant = variants_[index];
			if (batch.size() >= 32 || batchBytes >= (8ull << 20)) {
				later.emplace_back(index, windEntry);
				continue;
			}
			const std::size_t count = variant.order.size();
			const std::uint64_t indexBytes = static_cast<std::uint64_t>(count) * 3 * sizeof(std::uint32_t);
			std::vector<std::uint32_t> indices;
			std::uint32_t uvCount = 0;
			const std::uint32_t* uv = nullptr;
			if (windEntry == UINT32_MAX) {
				// A character part: its classified index list and its mixed triangles' texcoords.
				if (variant.state != VariantState::Building || variant.uploaded)
					continue;
				const bool ok = variant.kind == VariantKind::Skin && variant.base < skins_.size() && skins_[variant.base].live &&
				                skins_[variant.base].indicesCpu.size() == static_cast<std::size_t>(variant.info.triangles) * 3 &&
				                skins_[variant.base].uvCpu.size() == static_cast<std::size_t>(variant.info.triangles) * 4;
				if (!ok) {
					SetVariantState(variant, VariantState::NoGain);
					continue;
				}
				const auto& shape = skins_[variant.base];
				indices.resize(count * 3);
				for (std::size_t k = 0; k < count; ++k)
					std::copy_n(shape.indicesCpu.data() + static_cast<std::size_t>(variant.order[k]) * 3, 3, indices.data() + k * 3);
				uvCount = variant.mixed * 2;
				uv = shape.uvCpu.data();
			} else {
				if (windEntry >= variant.wind.size() || variant.wind[windEntry].uploaded || variant.wind[windEntry].failed)
					continue;
				auto& entry = variant.wind[windEntry];
				const bool ok = entry.source < windSources_.size() && windSources_[entry.source].live &&
				                windSources_[entry.source].indicesCpu.size() == static_cast<std::size_t>(variant.info.triangles) * 3;
				if (!ok) {
					entry.failed = true;
					continue;
				}
				const auto& sourceIndices = windSources_[entry.source].indicesCpu;
				indices.resize(count * 3);
				for (std::size_t k = 0; k < count; ++k)
					std::copy_n(sourceIndices.data() + static_cast<std::size_t>(variant.order[k]) * 3, 3, indices.data() + k * 3);
			}
			Pending entry{ index, windEntry, {}, {}, indexBytes, UINT32_MAX, 0, 0 };
			entry.uvAt = BakeAlign(indexBytes, 256);
			const std::uint64_t uploadBytes = entry.uvAt + static_cast<std::uint64_t>(uvCount) * 8;
			const bool pools = skinStaticPool_.Allocate(BakeAlign(std::max<std::uint64_t>(indexBytes, 4), 256), entry.range) && uploadPool_.Allocate(uploadBytes, entry.upload);
			const bool uvOk = !pools || uvCount == 0 || AllocateAlphaUv(uvCount, entry.uvOffset);
			if (!pools || !uvOk) {
				skinStaticPool_.Free(entry.range, 0);
				uploadPool_.Free(entry.upload, 0);
				if (!pools)
					++stats_.pools.allocFailsLastFrame;
				if (!uvOk || skinStaticPool_.failed || uploadPool_.failed) {
					if (windEntry == UINT32_MAX)
						SetVariantState(variant, VariantState::NoGain);
					else
						variant.wind[windEntry].failed = true;
				} else {
					later.emplace_back(index, windEntry);  // the pools' next block
				}
				continue;
			}
			entry.uvCount = uvCount;
			std::byte* mapped = uploadPool_.Mapped(entry.upload);
			std::memcpy(mapped, indices.data(), indexBytes);
			for (std::uint32_t k = 0; k < uvCount / 2; ++k)
				std::memcpy(mapped + entry.uvAt + static_cast<std::uint64_t>(k) * 16, uv + static_cast<std::size_t>(variant.order[k]) * 4, 16);
			batchBytes += uploadBytes;
			batch.push_back(entry);
		}
		variantUploads_ = std::move(later);
		if (batch.empty())
			return;

		std::vector<std::uint32_t> blocks;
		for (const auto& entry : batch)
			blocks.push_back(entry.range.block);
		std::sort(blocks.begin(), blocks.end());
		blocks.erase(std::unique(blocks.begin(), blocks.end()), blocks.end());
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_COPY_DEST);
		for (const auto& entry : batch)
			a_list->CopyBufferRegion(skinStaticPool_.Resource(entry.range), entry.range.offset, uploadPool_.Resource(entry.upload), entry.upload.offset, entry.indexBytes);
		TransitionBlocks(a_list, skinStaticPool_, blocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		const bool anyUv = std::any_of(batch.begin(), batch.end(), [](const Pending& a_entry) { return a_entry.uvCount > 0; });
		if (anyUv && alphaUv_) {
			auto toCopy = BakeTransition(alphaUv_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			a_list->ResourceBarrier(1, &toCopy);
			for (const auto& entry : batch)
				if (entry.uvCount > 0)
					a_list->CopyBufferRegion(alphaUv_.Get(), static_cast<std::uint64_t>(entry.uvOffset) * 8, uploadPool_.Resource(entry.upload),
						entry.upload.offset + entry.uvAt, static_cast<std::uint64_t>(entry.uvCount) * 8);
			auto back = BakeTransition(alphaUv_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			a_list->ResourceBarrier(1, &back);
		}
		variantMustSubmit_ = true;  // marked uploaded below: this list must run
		for (auto& entry : batch) {
			uploadPool_.Free(entry.upload, RetireValue());  // read by this submission only
			auto& variant = variants_[entry.variant];
			variant.bytes += entry.range.size + static_cast<std::uint64_t>(entry.uvCount) * 8;
			if (entry.wind != UINT32_MAX) {
				variant.wind[entry.wind].range = entry.range;
				variant.wind[entry.wind].uploaded = true;
				continue;
			}
			variant.skinIndex = entry.range;
			variant.uvOffset = entry.uvOffset;
			variant.uvCount = entry.uvCount;
			AlphaInfoGpu info = variant.info;
			info.uvBase = entry.uvOffset != UINT32_MAX ? entry.uvOffset : 0u;
			info.triangles = variant.mixed;
			info.flags = kAlphaInfoClassified;
			variant.record = AllocateAlphaInfoSlot(info);
			variant.uploaded = true;
			SetVariantState(variant, variant.record != kInvalidAlphaInfo ? VariantState::Ready : VariantState::NoGain);
		}
	}

	void Backend::UpdateClassifyStats()
	{
		auto& s = classifyStats_;
		s.variants = s.pending = s.baking = s.building = s.ready = s.noGain = 0;
		std::uint64_t bytes = 0;
		for (const auto& variant : variants_) {
			if (!variant.live)
				continue;
			++s.variants;
			switch (variant.state) {
			case VariantState::Pending:
				++s.pending;
				break;
			case VariantState::Baking:
				++s.baking;
				break;
			case VariantState::Building:
				++s.building;
				break;
			case VariantState::Ready:
				++s.ready;
				break;
			default:
				++s.noGain;
				break;
			}
			bytes += variant.bytes;
			if (variant.blas != kInvalidBlas && variant.blas < blas_.size())
				bytes += blas_[variant.blas].bytes;
		}
		s.bytes = bytes;
		std::uint64_t cpu = 0;
		for (const auto& blas : blas_)
			cpu += blas.sourceBytes;
		for (const auto& source : windSources_)
			cpu += source.indicesCpu.size() * sizeof(std::uint32_t);
		for (const auto& shape : skins_)
			cpu += (shape.indicesCpu.size() + shape.uvCpu.size()) * sizeof(std::uint32_t);
		s.cpuBytes = cpu;
	}
}
