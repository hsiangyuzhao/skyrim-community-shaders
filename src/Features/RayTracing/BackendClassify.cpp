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

//
// (47t) Opacity Micromaps (NVAPI, RTX 40; switch OpacityMicromaps): the same bake also cuts each mixed triangle into micro-triangles
// (OC1, 4-state, level by its texel footprint, at most kOmmMaxLevel) and sorts each one the same way: opaque / transparent / unknown.
// The mixed triangles then get an OMM array on geometry 0 (built through NvAPI_D3D12_BuildRaytracingOpacityMicromapArray, attached by
// NvAPI_D3D12_BuildRaytracingAccelerationStructureEx; static, swaying and character BLASes alike, refits with the same link): the
// hardware commits opaque micro-triangles and skips transparent ones without the candidate loop; unknown ones come back as candidates and
// are tested exactly as before. A mixed triangle whose micro-triangles are all unknown gets the special index "fully unknown". Every ray
// the alpha test decides keeps its result (the 47t harness: identical closest / any hits on 1 M random and 1 M micro-edge rays). Inline
// ray queries need no HLSL change (the trace pipelines are created with the NVAPI OMM pipeline option). Without the caps, or off: 47s.

#include "Backend.h"

#include "Utils/D3D.h"
#include "Utils/Nvapi.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstring>

#pragma warning(push, 0)
#include <nvapi.h>
#pragma warning(pop)

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
		// (47t) a_ommLevel > 0: plus the micro-triangles' boxes at that level (the triangle may turn out mixed), times a_ommScale (kOmmCostScale).
		std::uint64_t BakeCost(const std::uint32_t* a_packed, const float a_uvTransform[4], std::uint32_t a_mips, std::uint32_t a_ommLevel, std::uint64_t a_ommScale)
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
			if (a_ommLevel > 0) {
				const float n = static_cast<float>(1u << a_ommLevel);
				std::uint64_t micro = 0;
				for (std::uint32_t mip = 0; mip < a_mips; ++mip) {
					const float s = static_cast<float>(256u >> mip);
					const float ex = du * s / n + 3.0f, ey = dv * s / n + 3.0f;
					micro += (ex > s + 2.0f || ey > s + 2.0f) ? 9u : static_cast<std::uint64_t>(ex * ey);
				}
				cost += (micro << (2 * a_ommLevel)) * a_ommScale;
			}
			return cost;
		}

		// (47t) AlphaClassifyCS OmmLevel on the CPU: micro-triangles of about 4 texels of mip 2 (64 x 64). The bake clamps its own
		// result to the variant's maximum of this one (which sized the data), so a rounding difference can never overflow a slot.
		std::uint32_t OmmLevelOf(const std::uint32_t* a_packed, const float a_uvTransform[4], std::uint32_t a_max)
		{
			float u[3], v[3];
			for (int c = 0; c < 3; ++c) {
				u[c] = HalfToFloat(static_cast<std::uint16_t>(a_packed[c] & 0xFFFFu)) * a_uvTransform[2] + a_uvTransform[0];
				v[c] = HalfToFloat(static_cast<std::uint16_t>(a_packed[c] >> 16)) * a_uvTransform[3] + a_uvTransform[1];
				if (!std::isfinite(u[c]) || !std::isfinite(v[c]) || std::abs(u[c]) > 4096.0f || std::abs(v[c]) > 4096.0f)
					return 0;
			}
			const float e1x = (u[1] - u[0]) * 64.0f, e1y = (v[1] - v[0]) * 64.0f, e2x = (u[2] - u[0]) * 64.0f, e2y = (v[2] - v[0]) * 64.0f;
			const float area = 0.5f * std::abs(e1x * e2y - e1y * e2x);
			if (!(area > 4.0f))
				return 0;
			const float level = std::round(0.5f * std::log2(area / 4.0f));
			return static_cast<std::uint32_t>(std::clamp(level, 0.0f, static_cast<float>(a_max)));
		}

		// (47t) Bytes of one 4-state OMM of a level (2 bits per micro-triangle), as the bake stores it (whole uints).
		std::uint32_t OmmStrideOf(std::uint32_t a_level)
		{
			const std::uint32_t bytes = ((1u << (2 * a_level)) * 2u + 7u) / 8u;
			return std::max<std::uint32_t>((bytes + 3u) & ~3u, 4u);
		}

		constexpr std::uint32_t kClassMask = 3u;  // (47t) bits 0-1 of a bake entry: the class; 2-4 the OMM level; 5-15 the known micro-triangles
	}

	// ---------------------------------------------------------------------------------------------
	// Pipeline and buffers
	// ---------------------------------------------------------------------------------------------

	bool Backend::CreateBakePipeline()
	{
		// Constants b0, texcoords t0 (root SRV), classes u0 (root UAV), table: copy layers t1 (the alpha table's first slot), (47t) OMM data u1
		// (root UAV).
		D3D12_DESCRIPTOR_RANGE range{};
		range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
		range.NumDescriptors = 1;
		range.BaseShaderRegister = 1;
		range.OffsetInDescriptorsFromTableStart = 0;
		D3D12_ROOT_PARAMETER params[5]{};
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
		params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
		params[4].Descriptor.ShaderRegister = 1;
		params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
		D3D12_ROOT_SIGNATURE_DESC rootDesc{};
		rootDesc.NumParameters = 5;
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
		SetOpacityMicromaps(ommWanted_);  // (47t) in effect only with Alpha Classify on
		UpdateClassifyStats();
	}

	void Backend::SetOpacityMicromaps(bool a_on)
	{
		ommWanted_ = a_on;
		const bool effective = a_on && ommCaps_ && classifyOn_ && bakePipeline_;
		if (effective == ommOn_)
			return;
		ommOn_ = effective;
		if (classifyOn_) {
			// The variants are keyed by it: the records take new ones (made and baked over the next frames); the old ones go once the
			// swaying copies and character parts built with them were rebuilt (as when Alpha Classify goes off). Until a new one is
			// ready an instance is traced unclassified.
			for (std::uint32_t slot = 0; slot < slotVariant_.size(); ++slot) {
				if (const std::uint32_t variant = slotVariant_[slot]; variant != UINT32_MAX) {
					slotVariant_[slot] = UINT32_MAX;
					ReleaseVariantRef(variant);
				}
			}
		}
		logger::info("[RayTracing] Opacity Micromaps {}", effective ? "on" : "off");
	}

	void Backend::SetVariantState(AlphaVariant& a_variant, VariantState a_state)
	{
		const bool wasReady = a_variant.state == VariantState::Ready;
		const bool ready = a_state == VariantState::Ready;
		a_variant.state = a_state;
		auto& s = classifyStats_;
		if (ready && !wasReady && !a_variant.counted) {
			s.opaqueTriangles += a_variant.opaque;
			s.droppedTriangles += a_variant.dropped;
			s.mixedTriangles += a_variant.mixed;
			// (47t)
			s.ommArrays += a_variant.ommLink.Valid() ? 1u : 0u;
			s.ommTriangles += a_variant.ommCount;
			s.ommMicroTriangles += a_variant.ommMicro;
			s.ommKnownMicro += a_variant.ommKnown;
			a_variant.counted = true;
		} else if (!ready && a_variant.counted) {
			s.opaqueTriangles -= std::min<std::uint64_t>(s.opaqueTriangles, a_variant.opaque);
			s.droppedTriangles -= std::min<std::uint64_t>(s.droppedTriangles, a_variant.dropped);
			s.mixedTriangles -= std::min<std::uint64_t>(s.mixedTriangles, a_variant.mixed);
			s.ommArrays -= std::min<std::uint32_t>(s.ommArrays, a_variant.ommLink.Valid() ? 1u : 0u);
			s.ommTriangles -= std::min<std::uint64_t>(s.ommTriangles, a_variant.ommCount);
			s.ommMicroTriangles -= std::min<std::uint64_t>(s.ommMicroTriangles, a_variant.ommMicro);
			s.ommKnownMicro -= std::min<std::uint64_t>(s.ommKnownMicro, a_variant.ommKnown);
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
			std::bit_cast<std::uint32_t>(info.uvTransform[2]), std::bit_cast<std::uint32_t>(info.uvTransform[3]), ommOn_ ? 1u : 0u };
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
			variant.omm = ommOn_;  // (47t)
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
		if (variant.ommQueued)
			std::erase(ommBuildQueue_, a_variant);  // (47t)
		std::erase_if(variantUploads_, [&](const auto& a_entry) { return a_entry.first == a_variant; });
		// Everything behind the fence (a TLAS / list in flight may still use it).
		const BlasHandle blas = variant.blas;
		const std::uint32_t record = variant.record;
		if (variant.skinIndex.Valid())
			skinStaticPool_.Free(variant.skinIndex, RetireValue());
		// (47t) The OMM array is read while tracing (as long as a BLAS built with it may be traced: the same fence).
		if (variant.ommInput.Valid())
			skinOutputPool_.Free(variant.ommInput, RetireValue());
		if (variant.ommArray.Valid())
			asPool_.Free(variant.ommArray, RetireValue());
		if (variant.ommIndex.Valid())
			skinStaticPool_.Free(variant.ommIndex, RetireValue());
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
				DropVariantOmm(variant);  // (47t) its bake output (a bake in flight: behind the fence)
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
			DropVariantOmm(variant);
			SetVariantState(variant, variant.base == UINT32_MAX ? VariantState::Dead : VariantState::NoGain);
			return;
		}
		std::uint32_t mixed = 0, opaque = 0;
		for (std::uint32_t t = 0; t < a_triangles; ++t) {
			const std::uint32_t c = a_classes[t] & kClassMask;
			mixed += c == 0u ? 1u : 0u;
			opaque += c == 1u ? 1u : 0u;
		}
		const std::uint32_t dropped = a_triangles - mixed - opaque;
		// The classified order: the mixed triangles in their order (geometry 0, the record), then the opaque ones (geometry 1).
		std::vector<std::uint32_t> order;
		order.reserve(static_cast<std::size_t>(mixed) + opaque);
		for (std::uint32_t t = 0; t < a_triangles; ++t)
			if ((a_classes[t] & kClassMask) == 0u)
				order.push_back(t);
		for (std::uint32_t t = 0; t < a_triangles; ++t)
			if ((a_classes[t] & kClassMask) == 1u)
				order.push_back(t);
		// (47t) Opacity Micromaps of the mixed triangles: an OMM for each one with a micro-triangle the hardware can decide (its data at
		// ommStride x base triangle in the bake output), the special index "fully unknown" for the others.
		variant.ommDescs.clear();
		variant.ommIndexCpu.clear();
		variant.ommCount = 0;
		variant.ommMicro = variant.ommKnown = 0;
		if (variant.omm && variant.ommInput.Valid() && variant.ommStride > 0 && mixed > 0) {
			variant.ommIndexCpu.reserve(mixed);
			for (std::uint32_t k = 0; k < mixed; ++k) {
				const std::uint32_t t = order[k];
				const std::uint32_t level = (a_classes[t] >> 2) & 7u;
				const std::uint32_t known = (a_classes[t] >> 5) & 0x7FFu;
				if (level > 0 && level <= variant.ommMaxLevel && known > 0) {
					const std::uint64_t offset = static_cast<std::uint64_t>(t) * variant.ommStride;
					variant.ommIndexCpu.push_back(static_cast<std::int32_t>(variant.ommDescs.size()));
					variant.ommDescs.push_back(offset | (static_cast<std::uint64_t>(level) << 32) |
											   (static_cast<std::uint64_t>(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE) << 48));
					variant.ommMicro += 1ull << (2 * level);
					variant.ommKnown += known;
				} else {
					variant.ommIndexCpu.push_back(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_SPECIAL_INDEX_FULLY_UNKNOWN_TRANSPARENT);
				}
			}
			variant.ommCount = static_cast<std::uint32_t>(variant.ommDescs.size());
		}
		const bool withOmm = variant.ommCount > 0;
		// Nothing to gain (every triangle mixed and no OMM), or nothing left (every triangle see-through: no BLAS can hold that; the
		// original, whose every candidate the alpha test rejects, stays).
		if ((mixed == a_triangles && !withOmm) || mixed + opaque == 0) {
			DropVariantOmm(variant);
			SetVariantState(variant, VariantState::NoGain);
			return;
		}
		variant.order = std::move(order);
		variant.mixed = mixed;
		variant.opaque = opaque;
		variant.dropped = dropped;
		SetVariantState(variant, VariantState::Building);
		if (withOmm) {
			// The OMM array first (RecordOmmBuilds, the next prep lists), then the classified BLAS / the character part's upload.
			if (!variant.ommQueued) {
				variant.ommQueued = true;
				ommBuildQueue_.push_back(a_variant);
			}
			return;
		}
		DropVariantOmm(variant);
		ContinueClassified(a_variant);
	}

	void Backend::ContinueClassified(std::uint32_t a_variant)
	{
		auto& variant = variants_[a_variant];
		if (variant.base == UINT32_MAX) {
			SetVariantState(variant, VariantState::Dead);
			return;
		}
		if (variant.kind == VariantKind::Skin) {
			// The index list and record go up in RecordVariantUploads (the part's next frames).
			variantUploads_.emplace_back(a_variant, UINT32_MAX);
			return;
		}
		// Static: a classified copy of the base BLAS (the same vertices), built through the usual queue.
		const std::uint32_t triangles = variant.info.triangles;
		const std::uint32_t mixed = variant.mixed;
		const ChunkGeometry* source = variant.base < blas_.size() ? blas_[variant.base].source.get() : nullptr;
		if (!source || source->Triangles() != triangles || source->alphaUV.size() != static_cast<std::size_t>(triangles) * 4) {
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
		geometry->omm = variant.ommLink;  // (47t) invalid without OMM
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
	}

	// ---------------------------------------------------------------------------------------------
	// Bakes
	// ---------------------------------------------------------------------------------------------

	void Backend::RecordAlphaBakes(ID3D12GraphicsCommandList4* a_list, CommandContext& a_context, std::size_t a_index)
	{
		a_context.bakeRecorded.clear();
		a_context.bakeTimed = false;
		a_context.bakeOmm = false;
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
			// (47t) Opacity Micromaps: the variant's highest level (it sizes the data: ommStride bytes per base triangle) and the bake output
			// in skinOutputPool_ (kept until the OMM array is built). Without a level (tiny triangles) or memory: no OMM (as 47s).
			if (!resume && variant.omm) {
				std::uint32_t maxLevel = 0;
				for (std::uint32_t t = 0; t < info.triangles && maxLevel < kOmmMaxLevel; ++t)
					maxLevel = std::max(maxLevel, OmmLevelOf(packed + static_cast<std::size_t>(t) * 4, info.uvTransform, kOmmMaxLevel));
				if (maxLevel == 0) {
					DropVariantOmm(variant);
				} else {
					const std::uint32_t stride = OmmStrideOf(maxLevel);
					const std::uint64_t bytes = BakeAlign(static_cast<std::uint64_t>(info.triangles) * stride, 256);
					if (variant.ommInput.Valid() && variant.ommInput.size < bytes)
						skinOutputPool_.Free(variant.ommInput, RetireValue());
					if (!variant.ommInput.Valid() || variant.ommInput.size < bytes) {
						variant.ommInput = {};
						if (!skinOutputPool_.Allocate(bytes, variant.ommInput)) {
							variant.ommInput = {};
							++stats_.pools.allocFailsLastFrame;
							if (skinOutputPool_.failed) {
								++classifyStats_.ommFailed;
								DropVariantOmm(variant);
							} else {
								variant.queued = true;
								bakeQueue_.push_back(index);  // the pool's next block
								continue;
							}
						}
					}
					if (variant.omm) {
						variant.ommMaxLevel = maxLevel;
						variant.ommStride = stride;
					}
				}
			}
			const std::uint32_t ommMax = variant.omm && variant.ommInput.Valid() ? variant.ommMaxLevel : 0u;
			// This frame's part: as many triangles as the budget allows (at least one in the frame).
			const std::uint32_t first = resume ? variant.nextBake : 0u;
			std::uint32_t count = 0;
			while (first + count < info.triangles && total + count < kBakeCapacity) {
				const std::uint32_t* triangle = packed + static_cast<std::size_t>(first + count) * 4;
				const std::uint64_t cost = BakeCost(triangle, info.uvTransform, kBakeMips, ommMax > 0 ? OmmLevelOf(triangle, info.uvTransform, ommMax) : 0u, kOmmCostScale);
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
		// (47t) The OMM data ranges written below: their blocks UAV for the bake, then back to the state they were in (a list dropped
		// later would otherwise leave the tracked states wrong).
		std::vector<std::pair<std::uint32_t, D3D12_RESOURCE_STATES>> ommBlocks;
		for (const auto& job : jobs) {
			const auto& variant = variants_[job.variant];
			if (!variant.omm || !variant.ommInput.Valid())
				continue;
			const std::uint32_t block = variant.ommInput.block;
			if (block < skinOutputPool_.blocks.size() && std::none_of(ommBlocks.begin(), ommBlocks.end(), [&](const auto& a_entry) { return a_entry.first == block; }))
				ommBlocks.emplace_back(block, skinOutputPool_.blocks[block].state);
		}
		if (!ommBlocks.empty()) {
			std::vector<std::uint32_t> blocks;
			for (const auto& entry : ommBlocks)
				blocks.push_back(entry.first);
			TransitionBlocks(a_list, skinOutputPool_, blocks, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
		}
		bool anyOmm = false;
		for (const auto& job : jobs) {
			const auto& variant = variants_[job.variant];
			const AlphaInfoGpu& info = variant.info;
			const bool omm = variant.omm && variant.ommInput.Valid() && variant.ommStride > 0;
			const std::uint32_t groupsX = std::min(job.triangles, kBakeGroupsX);
			const std::uint32_t groupsY = (job.triangles + groupsX - 1) / groupsX;
			const std::uint32_t constants[kBakeConstants] = { info.uvBase + job.first * 2u, job.triangles, info.texture & 0xFFFFu, job.outOffset,
				std::bit_cast<std::uint32_t>(info.threshold), std::bit_cast<std::uint32_t>(info.alphaScale), groupsX, kBakeMips,
				std::bit_cast<std::uint32_t>(info.uvTransform[0]), std::bit_cast<std::uint32_t>(info.uvTransform[1]),
				std::bit_cast<std::uint32_t>(info.uvTransform[2]), std::bit_cast<std::uint32_t>(info.uvTransform[3]), omm ? variant.ommStride : 0u,
				omm ? variant.ommMaxLevel : 0u };
			a_list->SetComputeRoot32BitConstants(0, kBakeConstants, constants, 0);
			// u1: the variant's OMM data from this part's first triangle (never written without OMM: the class output stands in).
			a_list->SetComputeRootUnorderedAccessView(4, omm ? skinOutputPool_.Address(variant.ommInput) + static_cast<std::uint64_t>(job.first) * variant.ommStride :
			                                                   bakeOut_->GetGPUVirtualAddress() + region);
			a_list->Dispatch(groupsX, groupsY, 1);
			anyOmm = anyOmm || omm;
		}
		if (!ommBlocks.empty()) {
			std::vector<D3D12_RESOURCE_BARRIER> barriers;
			for (const auto& entry : ommBlocks)
				barriers.push_back(BakeUav(skinOutputPool_.blocks[entry.first].resource.Get()));
			a_list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
			for (const auto& [block, state] : ommBlocks)
				TransitionBlocks(a_list, skinOutputPool_, { block }, state);
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
		a_context.bakeOmm = anyOmm;
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
		a_context.bakeOmm = false;
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
					complete = (classes[t] & 0xFFFF0000u) == kBakeMarker && (classes[t] & kClassMask) <= 2u;
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
				if (end >= begin) {
					classifyStats_.lastBakeGpuMs = static_cast<float>(static_cast<double>(end - begin) * 1000.0 / static_cast<double>(timestampFrequency_));
					if (context.bakeOmm)
						classifyStats_.lastOmmBakeGpuMs = classifyStats_.lastBakeGpuMs;  // (47t)
				}
			}
		}
		context.bakeJobs.clear();
		context.bakeTimed = false;
		context.bakeOmm = false;
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
		std::uint64_t bytes = 0, ommBytes = 0, ommInputBytes = 0;
		for (const auto& variant : variants_) {
			if (!variant.live)
				continue;
			++s.variants;
			ommBytes += (variant.ommArray.Valid() ? variant.ommArray.size : 0) + (variant.ommIndex.Valid() ? variant.ommIndex.size : 0);  // (47t)
			ommInputBytes += variant.ommInput.Valid() ? variant.ommInput.size : 0;
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
		s.ommBytes = ommBytes;
		s.ommInputBytes = ommInputBytes;
		std::uint64_t cpu = 0;
		for (const auto& blas : blas_)
			cpu += blas.sourceBytes;
		for (const auto& source : windSources_)
			cpu += source.indicesCpu.size() * sizeof(std::uint32_t);
		for (const auto& shape : skins_)
			cpu += (shape.indicesCpu.size() + shape.uvCpu.size()) * sizeof(std::uint32_t);
		s.cpuBytes = cpu;
	}

	// ---------------------------------------------------------------------------------------------
	// (47t) Opacity Micromaps (NVAPI)
	// ---------------------------------------------------------------------------------------------

	namespace
	{
		// NVAPI ids (nvapi_interface.h, the same vendored SDK as nvapi.h).
		constexpr std::uint32_t kIdGetRaytracingCaps = 0x85a6c2a0;
		constexpr std::uint32_t kIdSetCreatePipelineStateOptions = 0x5c607a27;
		constexpr std::uint32_t kIdOmmPrebuild = 0x4726d180;
		constexpr std::uint32_t kIdBuildOmm = 0x814f8d11;
		constexpr std::uint32_t kIdAsPrebuildEx = 0x8d025b77;
		constexpr std::uint32_t kIdBuildAsEx = 0xe24ead45;

		using GetCapsFn = decltype(&NvAPI_D3D12_GetRaytracingCaps);
		using SetPipelineOptionsFn = decltype(&NvAPI_D3D12_SetCreatePipelineStateOptions);
		using OmmPrebuildFn = decltype(&NvAPI_D3D12_GetRaytracingOpacityMicromapArrayPrebuildInfo);
		using BuildOmmFn = decltype(&NvAPI_D3D12_BuildRaytracingOpacityMicromapArray);
		using AsPrebuildExFn = decltype(&NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx);
		using BuildAsExFn = decltype(&NvAPI_D3D12_BuildRaytracingAccelerationStructureEx);

		static_assert(sizeof(OmmUsage) == sizeof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT));
		static_assert(offsetof(OmmUsage, count) == offsetof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT, count));
		static_assert(offsetof(OmmUsage, level) == offsetof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT, subdivisionLevel));
		static_assert(offsetof(OmmUsage, format) == offsetof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT, format));
		static_assert(sizeof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_DESC) == sizeof(std::uint64_t));

		const NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT* NvUsage(const OmmLink& a_link)
		{
			return reinterpret_cast<const NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_USAGE_COUNT*>(a_link.usage.data());
		}

		// The BLAS inputs through NVAPI: geometry 0 (the mixed triangles: non-opaque) with the OMM attached, the rest as they are. False:
		// not a shape this applies to (the plain D3D12 call is used).
		bool ToNvInputs(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& a_inputs, const OmmLink& a_omm, NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX (&o_descs)[2],
			NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX& o_inputs)
		{
			if (a_inputs.Type != D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL || a_inputs.DescsLayout != D3D12_ELEMENTS_LAYOUT_ARRAY ||
				a_inputs.NumDescs == 0 || a_inputs.NumDescs > 2 || !a_inputs.pGeometryDescs)
				return false;
			const auto& first = a_inputs.pGeometryDescs[0];
			if (first.Type != D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES || (first.Flags & D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE))
				return false;
			for (UINT i = 0; i < a_inputs.NumDescs; ++i) {
				const auto& src = a_inputs.pGeometryDescs[i];
				auto& dst = o_descs[i];
				dst = {};
				dst.flags = src.Flags;
				if (i == 0) {
					dst.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_OMM_TRIANGLES_EX;
					dst.ommTriangles.triangles = src.Triangles;
					auto& attachment = dst.ommTriangles.ommAttachment;
					attachment.opacityMicromapIndexBuffer.StartAddress = a_omm.index;
					attachment.opacityMicromapIndexBuffer.StrideInBytes = sizeof(std::int32_t);
					attachment.opacityMicromapIndexFormat = DXGI_FORMAT_R32_UINT;
					attachment.opacityMicromapBaseLocation = 0;
					attachment.opacityMicromapArray = a_omm.array;
					attachment.numOMMUsageCounts = a_omm.usages;
					attachment.pOMMUsageCounts = NvUsage(a_omm);
				} else {
					dst.type = NVAPI_D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES_EX;
					dst.triangles = src.Triangles;
				}
			}
			o_inputs = {};
			o_inputs.type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			o_inputs.flags = static_cast<NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS_EX>(a_inputs.Flags);  // the D3D12 bits are the same
			o_inputs.numDescs = a_inputs.NumDescs;
			o_inputs.descsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			o_inputs.geometryDescStrideInBytes = sizeof(NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX);
			o_inputs.pGeometryDescs = o_descs;
			return true;
		}
	}

	void Backend::InitOpacityMicromaps()
	{
		ommCaps_ = false;
		ommOn_ = false;
		nvOmm_ = {};
		ommBuildQueue_.clear();
		if (!Util::Nvapi::Loaded()) {
			logger::info("[RayTracing] Opacity Micromaps unavailable: {}", Util::Nvapi::LoadError());
			return;
		}
		nvOmm_.getCaps = Util::Nvapi::QueryInterface(kIdGetRaytracingCaps);
		nvOmm_.setPipelineOptions = Util::Nvapi::QueryInterface(kIdSetCreatePipelineStateOptions);
		nvOmm_.ommPrebuild = Util::Nvapi::QueryInterface(kIdOmmPrebuild);
		nvOmm_.buildOmm = Util::Nvapi::QueryInterface(kIdBuildOmm);
		nvOmm_.asPrebuildEx = Util::Nvapi::QueryInterface(kIdAsPrebuildEx);
		nvOmm_.buildAsEx = Util::Nvapi::QueryInterface(kIdBuildAsEx);
		if (!nvOmm_.getCaps || !nvOmm_.setPipelineOptions || !nvOmm_.ommPrebuild || !nvOmm_.buildOmm || !nvOmm_.asPrebuildEx || !nvOmm_.buildAsEx) {
			logger::info("[RayTracing] Opacity Micromaps unavailable: the driver's NVAPI lacks an entry point (needs R520 or newer)");
			nvOmm_ = {};
			return;
		}
		NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS caps = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_NONE;
		const NvAPI_Status status = reinterpret_cast<GetCapsFn>(nvOmm_.getCaps)(device_.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP, &caps, sizeof(caps));
		ommCaps_ = status == NVAPI_OK && (caps & NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD) != 0;
		logger::info("[RayTracing] Opacity Micromaps: {} (NvAPI_D3D12_GetRaytracingCaps status {}, caps 0x{:X})", ommCaps_ ? "supported" : "not supported",
			static_cast<int>(status), static_cast<unsigned>(caps));
	}

	void Backend::SetOmmPipelineOption(bool a_on)
	{
		if (!ommCaps_ || !nvOmm_.setPipelineOptions)
			return;
		NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS params{};
		params.version = NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS_VER;
		params.flags = a_on ? NVAPI_D3D12_PIPELINE_CREATION_STATE_FLAGS_ENABLE_OMM_SUPPORT : 0u;
		const NvAPI_Status status = reinterpret_cast<SetPipelineOptionsFn>(nvOmm_.setPipelineOptions)(device_.Get(), &params);
		if (status != NVAPI_OK) {
			logger::warn("[RayTracing] NvAPI_D3D12_SetCreatePipelineStateOptions({}) failed ({}){}", a_on ? "OMM" : "none", static_cast<int>(status),
				a_on ? "; Opacity Micromaps off" : "");
			if (a_on)
				ommCaps_ = false;  // the trace pipelines are not made for them (nothing was built with an OMM yet)
		}
	}

	const OmmLink* Backend::VariantOmm(std::uint32_t a_variant) const
	{
		if (a_variant >= variants_.size())
			return nullptr;
		const auto& variant = variants_[a_variant];
		return variant.live && variant.mixed > 0 && variant.ommLink.Valid() ? &variant.ommLink : nullptr;
	}

	const OmmLink* Backend::WindOmm(const WindMesh& a_mesh, std::uint32_t a_layout) const
	{
		if (a_layout == UINT32_MAX)
			return nullptr;
		const WindIndex* entry = FindWindIndex(a_layout, a_mesh.source);
		return entry && entry->range.Valid() ? VariantOmm(a_layout) : nullptr;
	}

	const OmmLink* Backend::SkinOmm(std::uint32_t a_layout) const
	{
		if (a_layout == UINT32_MAX || a_layout >= variants_.size() || !variants_[a_layout].skinIndex.Valid())
			return nullptr;
		return VariantOmm(a_layout);
	}

	void Backend::DropVariantOmm(AlphaVariant& a_variant)
	{
		if (a_variant.ommInput.Valid())
			skinOutputPool_.Free(a_variant.ommInput, RetireValue());  // a bake in flight may still write it
		a_variant.ommInput = {};
		a_variant.omm = false;
		a_variant.ommDescs.clear();
		a_variant.ommDescs.shrink_to_fit();
		a_variant.ommIndexCpu.clear();
		a_variant.ommIndexCpu.shrink_to_fit();
		a_variant.ommCount = 0;
		a_variant.ommMicro = a_variant.ommKnown = 0;
	}

	void Backend::PrebuildBlas(const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS& a_inputs, const OmmLink* a_omm,
		D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO& o_info)
	{
		NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX descs[2]{};
		NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS_EX inputs{};
		if (!a_omm || !a_omm->Valid() || !ommCaps_ || !nvOmm_.asPrebuildEx || !ToNvInputs(a_inputs, *a_omm, descs, inputs)) {
			device_->GetRaytracingAccelerationStructurePrebuildInfo(&a_inputs, &o_info);
			return;
		}
		NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS params{};
		params.version = NVAPI_GET_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO_EX_PARAMS_VER;
		params.pDesc = &inputs;
		params.pInfo = &o_info;
		const NvAPI_Status status = reinterpret_cast<AsPrebuildExFn>(nvOmm_.asPrebuildEx)(device_.Get(), &params);
		if (status != NVAPI_OK) {
			o_info = {};  // the caller drops / defers it (never built without its OMM in a size made for another layout)
			Fail(std::format("NvAPI_D3D12_GetRaytracingAccelerationStructurePrebuildInfoEx (status {})", static_cast<int>(status)), E_FAIL);
		}
	}

	void Backend::RecordBlasBuild(ID3D12GraphicsCommandList4* a_list, const D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC& a_desc, UINT a_postbuildCount,
		const D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC* a_postbuild, const OmmLink* a_omm)
	{
		NVAPI_D3D12_RAYTRACING_GEOMETRY_DESC_EX descs[2]{};
		NVAPI_D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC_EX desc{};
		if (!a_omm || !a_omm->Valid() || !ommCaps_ || !nvOmm_.buildAsEx || !ToNvInputs(a_desc.Inputs, *a_omm, descs, desc.inputs)) {
			a_list->BuildRaytracingAccelerationStructure(&a_desc, a_postbuildCount, a_postbuild);
			return;
		}
		desc.destAccelerationStructureData = a_desc.DestAccelerationStructureData;
		desc.sourceAccelerationStructureData = a_desc.SourceAccelerationStructureData;
		desc.scratchAccelerationStructureData = a_desc.ScratchAccelerationStructureData;
		NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS params{};
		params.version = NVAPI_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_EX_PARAMS_VER;
		params.pDesc = &desc;
		params.numPostbuildInfoDescs = a_postbuildCount;
		params.pPostbuildInfoDescs = a_postbuild;
		const NvAPI_Status status = reinterpret_cast<BuildAsExFn>(nvOmm_.buildAsEx)(a_list, &params);
		if (status != NVAPI_OK)
			Fail(std::format("NvAPI_D3D12_BuildRaytracingAccelerationStructureEx (status {})", static_cast<int>(status)), E_FAIL);
	}

	void Backend::RecordOmmBuilds(ID3D12GraphicsCommandList4* a_list)
	{
		classifyStats_.ommBuildsLastFrame = 0;
		if (ommBuildQueue_.empty() || failed_)
			return;
		struct Pending
		{
			std::uint32_t variant = UINT32_MAX;
			ArenaAlloc upload, index, array;
			std::uint64_t indexBytes = 0, descsAt = 0;
			NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_INPUTS inputs{};
			OmmLink link{};
		};
		std::vector<Pending> batch;
		std::deque<std::uint32_t> later;
		std::uint64_t inputBytes = 0, scratchNeed = 0;
		// The variant goes on without OMM (as 47s): its BLAS / upload follows at once.
		auto withoutOmm = [&](std::uint32_t a_index) {
			DropVariantOmm(variants_[a_index]);
			ContinueClassified(a_index);
		};
		while (!ommBuildQueue_.empty()) {
			const std::uint32_t index = ommBuildQueue_.front();
			ommBuildQueue_.pop_front();
			if (index >= variants_.size() || !variants_[index].live)
				continue;
			auto& variant = variants_[index];
			variant.ommQueued = false;
			if (variant.state != VariantState::Building)
				continue;
			if (variant.base == UINT32_MAX) {
				DropVariantOmm(variant);
				SetVariantState(variant, VariantState::Dead);
				continue;
			}
			if (!variant.omm || !variant.ommInput.Valid() || variant.ommDescs.empty() || variant.ommIndexCpu.size() != variant.mixed || !ommCaps_) {
				withoutOmm(index);
				continue;
			}
			if (batch.size() >= kOmmBuildsPerFrame || (inputBytes > 0 && inputBytes + variant.ommInput.size > kOmmBuildBytesPerFrame)) {
				variant.ommQueued = true;
				later.push_back(index);
				continue;
			}
			Pending entry;
			entry.variant = index;
			// Usage counts per level (every OMM is referenced once: the array's and the geometry's are the same).
			for (const std::uint64_t desc : variant.ommDescs) {
				const std::uint32_t level = static_cast<std::uint32_t>((desc >> 32) & 0xFFFFu);
				std::uint32_t u = 0;
				while (u < entry.link.usages && entry.link.usage[u].level != level)
					++u;
				if (u == entry.link.usages) {
					if (u >= entry.link.usage.size())
						break;  // cannot happen (levels 1..kOmmMaxLevel)
					entry.link.usage[u] = OmmUsage{ 0, level, NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_FORMAT_OC1_4_STATE };
					++entry.link.usages;
				}
				++entry.link.usage[u].count;
			}
			entry.inputs.flags = NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_BUILD_FLAG_PREFER_FAST_TRACE;
			entry.inputs.numOMMUsageCounts = entry.link.usages;
			entry.inputs.pOMMUsageCounts = nullptr;  // set below (entry moves into the batch)
			entry.inputs.inputBuffer = skinOutputPool_.Address(variant.ommInput);  // 256-aligned (pool alignment)
			entry.inputs.perOMMDescs.StrideInBytes = sizeof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_DESC);
			NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_INPUTS sizing = entry.inputs;
			sizing.pOMMUsageCounts = NvUsage(entry.link);
			NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO info{};
			NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS params{};
			params.version = NVAPI_GET_RAYTRACING_OPACITY_MICROMAP_ARRAY_PREBUILD_INFO_PARAMS_VER;
			params.pDesc = &sizing;
			params.pInfo = &info;
			const NvAPI_Status status = reinterpret_cast<OmmPrebuildFn>(nvOmm_.ommPrebuild)(device_.Get(), &params);
			if (status != NVAPI_OK || info.resultDataMaxSizeInBytes == 0) {
				logger::warn("[RayTracing] Opacity Micromaps: the OMM array size query failed (status {}); this mesh goes on without", static_cast<int>(status));
				++classifyStats_.ommFailed;
				withoutOmm(index);
				continue;
			}
			entry.indexBytes = static_cast<std::uint64_t>(variant.ommIndexCpu.size()) * sizeof(std::int32_t);
			entry.descsAt = BakeAlign(entry.indexBytes, 256);
			const std::uint64_t uploadBytes = entry.descsAt + variant.ommDescs.size() * sizeof(std::uint64_t);
			const bool ok = asPool_.Allocate(BakeAlign(info.resultDataMaxSizeInBytes, 256), entry.array) &&
			                skinStaticPool_.Allocate(BakeAlign(std::max<std::uint64_t>(entry.indexBytes, 4), 256), entry.index) &&
			                uploadPool_.Allocate(uploadBytes, entry.upload);
			if (!ok) {
				asPool_.Free(entry.array, 0);  // never used
				skinStaticPool_.Free(entry.index, 0);
				uploadPool_.Free(entry.upload, 0);
				++stats_.pools.allocFailsLastFrame;
				if (asPool_.failed || skinStaticPool_.failed || uploadPool_.failed) {
					++classifyStats_.ommFailed;
					withoutOmm(index);
				} else {
					variant.ommQueued = true;
					later.push_back(index);  // the pools' next block
				}
				continue;
			}
			std::byte* mapped = uploadPool_.Mapped(entry.upload);
			std::memcpy(mapped, variant.ommIndexCpu.data(), entry.indexBytes);
			std::memcpy(mapped + entry.descsAt, variant.ommDescs.data(), variant.ommDescs.size() * sizeof(std::uint64_t));
			scratchNeed = std::max(scratchNeed, info.scratchDataSizeInBytes);
			inputBytes += variant.ommInput.size;
			batch.push_back(entry);
		}
		for (auto it = later.rbegin(); it != later.rend(); ++it)
			ommBuildQueue_.push_front(*it);
		if (batch.empty())
			return;
		if (!scratch_ || !TryGrowScratch(std::max<std::uint64_t>(scratchNeed, 256))) {
			for (auto it = batch.rbegin(); it != batch.rend(); ++it) {
				asPool_.Free(it->array, 0);
				skinStaticPool_.Free(it->index, 0);
				uploadPool_.Free(it->upload, 0);
				variants_[it->variant].ommQueued = true;
				ommBuildQueue_.push_front(it->variant);
			}
			return;
		}

		// The index lists into skinStaticPool_ (read by every BLAS build / refit with the OMM), the bake output as build input.
		std::vector<std::uint32_t> indexBlocks, inputBlocks;
		for (const auto& entry : batch) {
			indexBlocks.push_back(entry.index.block);
			inputBlocks.push_back(variants_[entry.variant].ommInput.block);
		}
		for (auto* blocks : { &indexBlocks, &inputBlocks }) {
			std::sort(blocks->begin(), blocks->end());
			blocks->erase(std::unique(blocks->begin(), blocks->end()), blocks->end());
		}
		TransitionBlocks(a_list, skinStaticPool_, indexBlocks, D3D12_RESOURCE_STATE_COPY_DEST);
		for (const auto& entry : batch)
			a_list->CopyBufferRegion(skinStaticPool_.Resource(entry.index), entry.index.offset, uploadPool_.Resource(entry.upload), entry.upload.offset, entry.indexBytes);
		TransitionBlocks(a_list, skinStaticPool_, indexBlocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		TransitionBlocks(a_list, skinOutputPool_, inputBlocks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
		{
			// The BLAS builds above used the scratch buffer (the bakes' OMM data is complete: earlier lists on this queue).
			const auto barrier = BakeUav(nullptr);
			a_list->ResourceBarrier(1, &barrier);
		}
		const auto buildOmm = reinterpret_cast<BuildOmmFn>(nvOmm_.buildOmm);
		for (std::size_t i = 0; i < batch.size(); ++i) {
			auto& entry = batch[i];
			if (i > 0) {
				const auto barrier = BakeUav(scratch_.Get());  // one scratch buffer for all
				a_list->ResourceBarrier(1, &barrier);
			}
			entry.inputs.pOMMUsageCounts = NvUsage(entry.link);
			entry.inputs.perOMMDescs.StartAddress = uploadPool_.Address(entry.upload) + entry.descsAt;
			NVAPI_D3D12_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_DESC desc{};
			desc.destOpacityMicromapArrayData = asPool_.Address(entry.array);
			desc.inputs = entry.inputs;
			desc.scratchOpacityMicromapArrayData = scratch_->GetGPUVirtualAddress();
			NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS params{};
			params.version = NVAPI_BUILD_RAYTRACING_OPACITY_MICROMAP_ARRAY_PARAMS_VER;
			params.pDesc = &desc;
			if (const NvAPI_Status status = buildOmm(a_list, &params); status != NVAPI_OK) {
				// Work recorded before it may have gone in: the list cannot be trusted (the feature turns itself off).
				Fail(std::format("NvAPI_D3D12_BuildRaytracingOpacityMicromapArray (status {})", static_cast<int>(status)), E_FAIL);
				return;
			}
		}
		{
			// The OMM arrays are complete before any BLAS build that names them (later in this list or in later lists).
			const auto barrier = BakeUav(nullptr);
			a_list->ResourceBarrier(1, &barrier);
		}
		variantMustSubmit_ = true;  // the variants below take these arrays: this list must run
		for (auto& entry : batch) {
			uploadPool_.Free(entry.upload, RetireValue());  // read by this submission only
			auto& variant = variants_[entry.variant];
			skinOutputPool_.Free(variant.ommInput, RetireValue());  // the OMM array holds the data now
			variant.ommInput = {};
			variant.ommArray = entry.array;
			variant.ommIndex = entry.index;
			entry.link.array = asPool_.Address(entry.array);
			entry.link.index = skinStaticPool_.Address(entry.index);
			variant.ommLink = entry.link;
			variant.ommDescs.clear();
			variant.ommDescs.shrink_to_fit();
			variant.ommIndexCpu.clear();
			variant.ommIndexCpu.shrink_to_fit();
			++classifyStats_.ommBuildsLastFrame;
			++classifyStats_.ommBuildsTotal;
			ContinueClassified(entry.variant);
		}
	}
}
