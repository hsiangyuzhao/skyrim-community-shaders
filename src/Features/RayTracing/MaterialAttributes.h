#pragma once

// (48f) RT reflections, material data: the per-triangle attribute words the scene capture writes (ChunkGeometry::attributes) and
// TraceReflectCS (RT_MATERIAL) reads, and the 47u staged-chunk packing they must follow. Pure functions without engine types, so the
// offline sanity check (tools\sanity48f) compiles this very header.
//
// One triangle = 4 uints (16 bytes):
//   [0] [1] [2]  corner 0 / 1 / 2: the vertex's half2 texcoord 0 as the engine stores it (a non-finite half becomes 0);
//                terrain: the corner's 6 layer weights, 5 bits each (PackTerrainWeights);
//   [3]          bits 0-15 the material id (kNoMaterial = none: the instance's own material, or no texture), bits 16-31 the face
//                normal, octahedral 8 + 8 bits (in the BLAS's own space; the trace rotates it with the instance).
// The decode in TraceReflectCS.hlsl mirrors UnpackOct8 / UnpackTerrainWeights.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace RayTracingRT
{
	/// (48f) Material id of a triangle / instance without one (also the "use the instance's" marker in the attributes).
	inline constexpr std::uint16_t kNoMaterial = 0xFFFF;
	/// (48f) Uints per triangle in ChunkGeometry::attributes.
	inline constexpr std::uint32_t kAttributeWords = 4;

	namespace Attributes
	{
		/// A half2 texcoord as stored, with a non-finite (inf / NaN) half replaced by 0 (as the alpha path does).
		inline std::uint32_t SanitizeHalf2(std::uint32_t a_uv)
		{
			if ((a_uv & 0x7C00u) == 0x7C00u)
				a_uv &= 0xFFFF0000u;
			if ((a_uv & 0x7C000000u) == 0x7C000000u)
				a_uv &= 0x0000FFFFu;
			return a_uv;
		}

		/// Octahedral encode of a unit vector into 8 + 8 bits (unorm, 0..255 = -1..1); a zero vector becomes +z.
		inline std::uint16_t PackOct8(float a_x, float a_y, float a_z)
		{
			const float sum = std::abs(a_x) + std::abs(a_y) + std::abs(a_z);
			if (!(sum > 1e-20f) || !std::isfinite(sum)) {
				a_x = 0.0f;
				a_y = 0.0f;
				a_z = 1.0f;
			} else {
				a_x /= sum;
				a_y /= sum;
				a_z /= sum;
			}
			float ex = a_x, ey = a_y;
			if (a_z < 0.0f) {
				ex = (1.0f - std::abs(a_y)) * (a_x >= 0.0f ? 1.0f : -1.0f);
				ey = (1.0f - std::abs(a_x)) * (a_y >= 0.0f ? 1.0f : -1.0f);
			}
			auto q = [](float a_v) { return static_cast<std::uint32_t>(std::lround(std::clamp(a_v * 0.5f + 0.5f, 0.0f, 1.0f) * 255.0f)); };
			return static_cast<std::uint16_t>(q(ex) | (q(ey) << 8));
		}

		/// The inverse of PackOct8 (TraceReflectCS OctDecode8).
		inline void UnpackOct8(std::uint16_t a_packed, float o_n[3])
		{
			const float ex = static_cast<float>(a_packed & 0xFFu) / 255.0f * 2.0f - 1.0f;
			const float ey = static_cast<float>(a_packed >> 8) / 255.0f * 2.0f - 1.0f;
			float n[3] = { ex, ey, 1.0f - std::abs(ex) - std::abs(ey) };
			const float t = std::clamp(-n[2], 0.0f, 1.0f);
			n[0] += n[0] >= 0.0f ? -t : t;
			n[1] += n[1] >= 0.0f ? -t : t;
			const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			for (int a = 0; a < 3; ++a)
				o_n[a] = len > 0.0f ? n[a] / len : (a == 2 ? 1.0f : 0.0f);
		}

		/// The face normal of a triangle (positions xyz), octahedral 8 + 8; a degenerate triangle gives +z.
		inline std::uint16_t PackFaceNormal(const float* a_p0, const float* a_p1, const float* a_p2)
		{
			const float e1[3] = { a_p1[0] - a_p0[0], a_p1[1] - a_p0[1], a_p1[2] - a_p0[2] };
			const float e2[3] = { a_p2[0] - a_p0[0], a_p2[1] - a_p0[1], a_p2[2] - a_p0[2] };
			return PackOct8(e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]);
		}

		/// Six terrain layer weights (any non-negative scale; normalized here) into 5 bits each (bits 0-29).
		inline std::uint32_t PackTerrainWeights(const float a_w[6])
		{
			float sum = 0.0f;
			for (int i = 0; i < 6; ++i)
				sum += std::isfinite(a_w[i]) ? std::max(a_w[i], 0.0f) : 0.0f;
			std::uint32_t packed = 0;
			for (int i = 0; i < 6; ++i) {
				const float w = sum > 1e-6f ? (std::isfinite(a_w[i]) ? std::max(a_w[i], 0.0f) : 0.0f) / sum : (i == 0 ? 1.0f : 0.0f);
				packed |= static_cast<std::uint32_t>(std::lround(std::clamp(w, 0.0f, 1.0f) * 31.0f)) << (5 * i);
			}
			return packed;
		}

		inline void UnpackTerrainWeights(std::uint32_t a_packed, float o_w[6])
		{
			for (int i = 0; i < 6; ++i)
				o_w[i] = static_cast<float>((a_packed >> (5 * i)) & 31u) / 31.0f;
		}

		/// The last word of a triangle: material id | face normal << 16.
		inline std::uint32_t MaterialWord(std::uint16_t a_material, std::uint16_t a_normal)
		{
			return static_cast<std::uint32_t>(a_material) | (static_cast<std::uint32_t>(a_normal) << 16);
		}

		/// Appends the attributes of a mesh's triangles (a_indices: 3 per triangle into a_positions xyz / a_vertexBytes); a_uvOffset ==
		/// UINT32_MAX: no texcoords (0). Returns false (nothing appended) when an index is out of range.
		inline bool AppendMeshAttributes(const std::uint8_t* a_vertexBytes, std::uint32_t a_stride, std::uint32_t a_uvOffset, std::uint32_t a_vertexCount,
			const float* a_positions, const std::uint32_t* a_indices, std::size_t a_indexCount, std::uint16_t a_material, std::vector<std::uint32_t>& o_attributes)
		{
			const std::size_t triangles = a_indexCount / 3;
			for (std::size_t k = 0; k < triangles * 3; ++k)
				if (a_indices[k] >= a_vertexCount)
					return false;
			const std::size_t base = o_attributes.size();
			o_attributes.resize(base + triangles * kAttributeWords);
			std::uint32_t* out = o_attributes.data() + base;
			for (std::size_t t = 0; t < triangles; ++t) {
				const std::uint32_t i0 = a_indices[t * 3], i1 = a_indices[t * 3 + 1], i2 = a_indices[t * 3 + 2];
				const std::uint32_t corner[3] = { i0, i1, i2 };
				for (int c = 0; c < 3; ++c) {
					std::uint32_t uv = 0;
					if (a_uvOffset != UINT32_MAX && a_vertexBytes)
						std::memcpy(&uv, a_vertexBytes + static_cast<std::size_t>(corner[c]) * a_stride + a_uvOffset, sizeof(uv));
					out[t * kAttributeWords + c] = SanitizeHalf2(uv);
				}
				const std::uint16_t normal = PackFaceNormal(a_positions + static_cast<std::size_t>(i0) * 3, a_positions + static_cast<std::size_t>(i1) * 3,
					a_positions + static_cast<std::size_t>(i2) * 3);
				out[t * kAttributeWords + 3] = MaterialWord(a_material, normal);
			}
			return true;
		}

		/// (47u) The staged shapes' order: by side, then by the shape's Morton key (ties: staging order). a_keys[i] = (side << 63) | (morton
		/// >> 1), as Scene::FlushStaged builds them.
		inline std::vector<std::uint32_t> StagedOrder(const std::vector<std::uint64_t>& a_keys)
		{
			std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
			order.reserve(a_keys.size());
			for (std::uint32_t i = 0; i < a_keys.size(); ++i)
				order.emplace_back(a_keys[i], i);
			std::sort(order.begin(), order.end());
			std::vector<std::uint32_t> result;
			result.reserve(order.size());
			for (const auto& entry : order)
				result.push_back(entry.second);
			return result;
		}

		/// (48f) One staged shape appended to a chunk: its positions, its indices (rebased onto the chunk's vertices) and, when the cell
		/// staged attributes, exactly its triangles' attributes in the same order (triangle t of the shape -> its t-th attribute block).
		/// a_firstTriangle: the shape's first triangle in a_stageAttributes (UINT32_MAX: none staged).
		inline void AppendStagedShape(const float* a_stagePositions, std::uint32_t a_firstVertex, std::uint32_t a_vertices, const std::uint32_t* a_stageIndices,
			std::uint32_t a_firstIndex, std::uint32_t a_indices, const std::uint32_t* a_stageAttributes, std::uint32_t a_firstTriangle, std::vector<float>& o_positions,
			std::vector<std::uint32_t>& o_indices, std::vector<std::uint32_t>* o_attributes)
		{
			const auto base = static_cast<std::uint32_t>(o_positions.size() / 3);
			const float* positions = a_stagePositions + static_cast<std::size_t>(a_firstVertex) * 3;
			o_positions.insert(o_positions.end(), positions, positions + static_cast<std::size_t>(a_vertices) * 3);
			for (std::uint32_t k = 0; k < a_indices; ++k)
				o_indices.push_back(base + a_stageIndices[a_firstIndex + k]);
			if (o_attributes && a_stageAttributes && a_firstTriangle != UINT32_MAX) {
				const std::uint32_t* attributes = a_stageAttributes + static_cast<std::size_t>(a_firstTriangle) * kAttributeWords;
				o_attributes->insert(o_attributes->end(), attributes, attributes + static_cast<std::size_t>(a_indices / 3) * kAttributeWords);
			}
		}
	}
}
