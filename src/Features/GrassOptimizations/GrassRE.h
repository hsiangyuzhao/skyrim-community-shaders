#pragma once

// Engine layouts Grass Optimizations needs that this fork's CommonLibSSE-NG still declares as
// unknown members. They mirror the definitions upstream added to CommonLib alongside the feature
// (BSMultiStreamInstanceTriShape / BSGrassShaderProperty), kept local so the CommonLib submodule
// does not have to move. Flat (SE/AE) only: Grass Optimizations does not load in VR.
namespace GrassRE
{
	/** @brief BSGraphics::VertexBuffer, the per-instance-group stream. */
	struct VertexBuffer
	{
		ID3D11Buffer* buffer;  // 00
		void* data;            // 08
		size_t byteWidth;      // 10
	};
	static_assert(sizeof(VertexBuffer) == 0x18);

	/** @brief BSMultiStreamInstanceTriShape::InstanceGroup. The first 0x40 bytes are its BSMultiBoundAABB base. */
	struct InstanceGroup
	{
		std::uint8_t multiBoundAABB[0x40];  // 00
		VertexBuffer* vertexBuffer;         // 40
		std::uint32_t triCount;             // 48
		std::uint32_t instanceCount;        // 4C
		bool isVisible;                     // 50
		std::uint8_t pad51[7];              // 51
	};
	static_assert(sizeof(InstanceGroup) == 0x58);
	static_assert(offsetof(InstanceGroup, vertexBuffer) == 0x40);
	static_assert(offsetof(InstanceGroup, isVisible) == 0x50);

	/** @brief BSMultiStreamInstanceTriShape::GroupHeader, as streamed from a .gid file or buffer. */
	struct GroupHeader
	{
		RE::NiPoint3 center;                 // 00
		RE::NiPoint3 size;                   // 0C
		std::uint32_t triCount;              // 18
		std::uint32_t groupInstanceCount;    // 1C
		std::uint32_t numShortsPerInstance;  // 20
	};
	static_assert(sizeof(GroupHeader) == 0x24);

	/** @brief BSMultiStreamInstanceTriShape::MULTISTREAM_TRISHAPE_RUNTIME_DATA with its members named. */
	struct MultiStreamRuntimeData
	{
		RE::BSTArray<InstanceGroup*> instanceGroups;  // 00
		std::uint32_t meshTriCount;                   // 18
		std::uint32_t maxInstancesPerGroup;           // 1C
		float renderDistance;                         // 20
		std::uint32_t unk184;                         // 24
		void* groupAlloc;                             // 28
		std::uint32_t instanceCount;                  // 30
		std::uint32_t instanceSize;                   // 34
		std::uint32_t activeGroupCount;               // 38
		std::uint32_t pad3C;                          // 3C
	};
	static_assert(sizeof(MultiStreamRuntimeData) == sizeof(RE::BSMultiStreamInstanceTriShape::MULTISTREAM_TRISHAPE_RUNTIME_DATA));
	static_assert(offsetof(MultiStreamRuntimeData, groupAlloc) == 0x28);
	static_assert(offsetof(MultiStreamRuntimeData, instanceSize) == 0x34);

	inline MultiStreamRuntimeData& GetRuntimeData(RE::BSMultiStreamInstanceTriShape* a_shape)
	{
		return reinterpret_cast<MultiStreamRuntimeData&>(a_shape->GetMultiStreamTrishapeRuntimeData());
	}

	/** @brief The geometry's shader property (BSGeometry::properties[kEffect]; upstream's CommonLib names it shaderProperty). */
	inline RE::BSShaderProperty* GetShaderProperty(RE::BSGeometry* a_geometry)
	{
		return static_cast<RE::BSShaderProperty*>(a_geometry->GetGeometryRuntimeData().properties[RE::BSGeometry::States::kEffect].get());
	}

	/** @brief BSGrassShaderProperty::wavePeriod (offset 0x184 on flat runtimes). */
	inline float GetWavePeriod(const RE::BSGrassShaderProperty* a_property)
	{
		return *reinterpret_cast<const float*>(reinterpret_cast<std::uintptr_t>(a_property) + 0x184);
	}
}
