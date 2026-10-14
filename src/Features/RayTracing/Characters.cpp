#include "Characters.h"

#include "CasterFilter.h"

#include <DirectXPackedVector.h>
#include <bit>

namespace RayTracingCharacters
{
	using RayTracingRT::SkinVertex;

	namespace
	{
		constexpr std::uint32_t kMaxNodesPerActor = 4096;
		constexpr std::uint32_t kCapturesPerFrame = 6;     // new shapes read per frame
		constexpr double kCaptureBudgetMs = 1.0;           // CPU for new shapes per frame (at least one)
		constexpr std::uint32_t kMaxPartitions = 64;
		constexpr std::uint32_t kPlacementSamples = 64;
		constexpr std::uint32_t kLogLimit = 16;
		constexpr float kStaleDistance = 512.0f;          // skeleton root this far from its actor: not animated, not traced

		std::int64_t Qpc()
		{
			LARGE_INTEGER t;
			QueryPerformanceCounter(&t);
			return t.QuadPart;
		}

		double QpcMs()
		{
			static const double toMs = [] {
				LARGE_INTEGER f;
				QueryPerformanceFrequency(&f);
				return f.QuadPart > 0 ? 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
			}();
			return toMs;
		}

		/// Copies engine-owned memory; false instead of a crash if a mod left a bad pointer.
		bool SafeCopy(void* a_destination, const void* a_source, std::size_t a_bytes)
		{
			if (!a_source || a_bytes == 0)
				return false;
			__try {
				std::memcpy(a_destination, a_source, a_bytes);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// The skin instance's pointers, read once (SEH: a mod can leave them dangling).
		struct SkinRefs
		{
			RE::NiSkinData* data = nullptr;
			RE::NiSkinPartition* partition = nullptr;
			RE::NiAVObject** bones = nullptr;
			std::uint32_t boneCount = 0;
			RE::NiSkinData::BoneData* boneData = nullptr;
			std::uint32_t partitions = 0;
			RE::NiSkinPartition::Partition* partitionData = nullptr;
			std::uint32_t vertexCount = 0;
		};

		bool ReadSkinRefs(RE::NiSkinInstance* a_skin, SkinRefs& o_refs)
		{
			__try {
				o_refs.data = a_skin->skinData.get();
				o_refs.partition = a_skin->skinPartition.get();
				o_refs.bones = a_skin->bones;
				if (!o_refs.data || !o_refs.partition || !o_refs.bones)
					return false;
				o_refs.boneCount = o_refs.data->bones;
				o_refs.boneData = o_refs.data->boneData;
				o_refs.partitions = o_refs.partition->numPartitions;
				o_refs.partitionData = o_refs.partition->partitions.data();
				o_refs.vertexCount = o_refs.partition->vertexCount;
				return o_refs.boneData != nullptr && o_refs.partitionData != nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// Bit i = partition i drawn (BSDismemberSkinInstance: dismembered partitions are not).
		std::uint64_t PartitionMask(RE::NiSkinInstance* a_skin, std::uint32_t a_partitions)
		{
			const std::uint32_t count = std::min(a_partitions, kMaxPartitions);
			std::uint64_t mask = count >= 64 ? ~0ull : ((1ull << count) - 1);
			auto* dismember = netimmerse_cast<RE::BSDismemberSkinInstance*>(a_skin);
			if (!dismember)
				return mask;
			__try {
				const auto& runtime = dismember->GetRuntimeData();
				if (runtime.partitions && static_cast<std::uint32_t>(runtime.numPartitions) == a_partitions) {
					for (std::uint32_t i = 0; i < count; ++i)
						if (!runtime.partitions[i].editorVisible)
							mask &= ~(1ull << i);
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
			}
			return mask;
		}

		std::uint64_t Mix(std::uint64_t a_hash, std::uint64_t a_value)
		{
			a_hash ^= a_value + 0x9E3779B97F4A7C15ull + (a_hash << 6) + (a_hash >> 2);
			return a_hash;
		}

		/// Identity of what a shape looks like now: a change (equip, dismember, new skin) recaptures it.
		std::uint64_t Signature(RE::BSGeometry* a_geometry)
		{
			const auto& runtime = a_geometry->GetGeometryRuntimeData();
			std::uint64_t hash = Mix(0, reinterpret_cast<std::uintptr_t>(a_geometry));
			hash = Mix(hash, reinterpret_cast<std::uintptr_t>(runtime.rendererData));
			if (auto* skin = runtime.skinInstance.get()) {
				hash = Mix(hash, reinterpret_cast<std::uintptr_t>(skin));
				SkinRefs refs;
				if (ReadSkinRefs(skin, refs)) {
					hash = Mix(hash, reinterpret_cast<std::uintptr_t>(refs.partition));
					hash = Mix(hash, refs.vertexCount);
					hash = Mix(hash, PartitionMask(skin, refs.partitions));
				}
			} else if (a_geometry->GetType().get() == RE::BSGeometry::Type::kTriShape) {
				if (auto* triShape = a_geometry->AsTriShape())
					hash = Mix(hash, triShape->GetTrishapeRuntimeData().vertexCount);
			}
			return hash;
		}

		bool HasHdtData(RE::NiAVObject* a_node)
		{
			const std::uint16_t count = a_node->GetExtraDataSize();
			for (std::uint16_t i = 0; i < count && i < 64; ++i) {
				auto* extra = a_node->GetExtraDataAt(i);
				const char* name = extra ? extra->GetName().c_str() : nullptr;
				if (name && (std::strncmp(name, "HDT", 3) == 0 || std::strncmp(name, "hdt", 3) == 0))
					return true;
			}
			return false;
		}

		bool ContainsHair(const char* a_name)
		{
			if (!a_name)
				return false;
			std::string_view name(a_name);
			for (std::size_t i = 0; i + 4 <= name.size(); ++i) {
				if ((name[i] | 0x20) == 'h' && (name[i + 1] | 0x20) == 'a' && (name[i + 2] | 0x20) == 'i' && (name[i + 3] | 0x20) == 'r')
					return true;
			}
			return false;
		}

		/// HDT-SMP / HDT-PE parts (their 3D carries the HDT extra data).
		bool IsPhysicsPart(RE::BSGeometry* a_geometry, RE::NiAVObject* a_root)
		{
			int depth = 0;
			for (RE::NiAVObject* node = a_geometry; node && depth < 64; node = node->parent, ++depth) {
				if (HasHdtData(node))
					return true;
				if (node == a_root)
					break;
			}
			return false;
		}

		/// (45j) How a shape was recognised as hair: 0 = not hair, 1 = its material is the hair tint
		/// material (what the game's and Community Shaders' hair shading key on), 2 = "hair" in its name
		/// or in its diffuse texture path (wigs, hair added as armour without the hair tint material).
		int HairKind(RE::BSGeometry* a_geometry, RE::BSLightingShaderProperty* a_lighting)
		{
			if (a_lighting) {
				__try {
					auto* material = a_lighting->material;
					if (material && material->GetFeature() == RE::BSShaderMaterial::Feature::kHairTint)
						return 1;
				} __except (EXCEPTION_EXECUTE_HANDLER) {
				}
			}
			if (ContainsHair(a_geometry->name.c_str()))
				return 2;
			if (a_lighting) {
				char path[512]{};
				__try {
					auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_lighting->material);
					RE::NiSourceTexture* source = material ? material->diffuseTexture.get() : nullptr;
					const char* name = source ? source->name.c_str() : nullptr;
					if (name)
						strncpy_s(path, name, _TRUNCATE);
				} __except (EXCEPTION_EXECUTE_HANDLER) {
					path[0] = 0;
				}
				if (ContainsHair(path))
					return 2;
			}
			return 0;
		}

		struct RawAlpha
		{
			ID3D11Texture2D* texture;
			ID3D11ShaderResourceView* view;
			float materialAlpha;
			float uv[4];
		};

		bool ReadAlpha(RE::BSLightingShaderProperty* a_lighting, RawAlpha& o_source)
		{
			__try {
				auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_lighting->material);
				if (!material)
					return false;
				RE::NiSourceTexture* source = material->diffuseTexture.get();
				if (!source || !source->rendererTexture)
					return false;
				o_source.texture = source->rendererTexture->texture;
				o_source.view = source->rendererTexture->resourceView;
				o_source.materialAlpha = material->materialAlpha;
				o_source.uv[0] = material->texCoordOffset[0].x;
				o_source.uv[1] = material->texCoordOffset[0].y;
				o_source.uv[2] = material->texCoordScale[0].x;
				o_source.uv[3] = material->texCoordScale[0].y;
				return o_source.texture != nullptr && o_source.view != nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// Rotation * scale and translation (relative to a_origin) of a bone matrix: 12 floats.
		bool StoreMatrix(const RE::NiTransform& a_m, const double a_origin[3], float* o_out)
		{
			for (int r = 0; r < 3; ++r) {
				for (int c = 0; c < 3; ++c)
					o_out[r * 4 + c] = a_m.rotate.entry[r][c] * a_m.scale;
			}
			o_out[3] = static_cast<float>(static_cast<double>(a_m.translate.x) - a_origin[0]);
			o_out[7] = static_cast<float>(static_cast<double>(a_m.translate.y) - a_origin[1]);
			o_out[11] = static_cast<float>(static_cast<double>(a_m.translate.z) - a_origin[2]);
			for (int i = 0; i < 12; ++i)
				if (!std::isfinite(o_out[i]) || std::abs(o_out[i]) > 1.0e6f)
					return false;
			return true;
		}

		/// Bone world * skin-to-bone for every bone; SEH: false on a bad read.
		bool ReadBoneWorlds(RE::NiAVObject* const* a_nodes, const RE::NiTransform* a_skinToBone, std::size_t a_count, RE::NiTransform* o_out)
		{
			__try {
				for (std::size_t i = 0; i < a_count; ++i)
					o_out[i] = a_nodes[i]->world * a_skinToBone[i];
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		RE::NiPoint3 Apply(const float* a_m, const float a_p[3])
		{
			return { a_m[0] * a_p[0] + a_m[1] * a_p[1] + a_m[2] * a_p[2] + a_m[3], a_m[4] * a_p[0] + a_m[5] * a_p[1] + a_m[6] * a_p[2] + a_m[7],
				a_m[8] * a_p[0] + a_m[9] * a_p[1] + a_m[10] * a_p[2] + a_m[11] };
		}

		enum class Reject
		{
			None,
			Shader,
			GeometryType,
			PhysicsOff,
			AlphaOff,
		};
	}

	Characters::~Characters()
	{
		new std::vector<std::unique_ptr<Actor>>(std::move(actors_));
		new std::vector<std::unique_ptr<Actor>>(std::move(retiring_));
	}

	void Characters::Configure(bool a_physics, bool a_hair, bool a_alpha, bool a_firstPersonPlayer)
	{
		if (a_physics != physics_ || a_hair != hair_ || a_alpha != alpha_)
			RequestFlush();
		physics_ = a_physics;
		hair_ = a_hair;
		alpha_ = a_alpha;
		firstPersonPlayer_ = a_firstPersonPlayer;
	}

	void Characters::ReleaseShape(RayTracingRT::Backend& a_backend, Shape& a_shape)
	{
		if (a_shape.registered)
			RayTracingCasterFilter::Remove(a_shape.geometry.get());
		a_shape.registered = false;
		if (a_shape.info != RayTracingRT::kInvalidAlphaInfo)
			a_backend.ReleaseAlphaInfo(a_shape.info);
		if (a_shape.texture != RayTracingRT::kInvalidAlphaTexture)
			a_backend.ReleaseAlphaTexture(a_shape.texture);
		a_shape.info = RayTracingRT::kInvalidAlphaInfo;
		a_shape.texture = RayTracingRT::kInvalidAlphaTexture;
		if (a_shape.handle != RayTracingRT::kInvalidSkin)
			a_backend.ReleaseSkinShape(a_shape.handle);
		a_shape.handle = RayTracingRT::kInvalidSkin;
	}

	void Characters::ReleaseActor(RayTracingRT::Backend& a_backend, Actor& a_actor)
	{
		for (auto* shapes : { &a_actor.shapes, &a_actor.retiringShapes }) {
			for (auto& shape : *shapes)
				ReleaseShape(a_backend, *shape);
			shapes->clear();
		}
	}

	void Characters::ReleaseAll(RayTracingRT::Backend& a_backend)
	{
		for (auto* list : { &actors_, &retiring_ }) {
			for (auto& actor : *list)
				ReleaseActor(a_backend, *actor);
			list->clear();
		}
		const float maxMs = stats_.maxMs;
		stats_ = {};
		stats_.maxMs = maxMs;
	}

	void Characters::EndFrame(RayTracingRT::Backend& a_backend)
	{
		for (auto& actor : retiring_)
			ReleaseActor(a_backend, *actor);
		retiring_.clear();
		for (auto& actor : actors_) {
			for (auto& shape : actor->retiringShapes)
				ReleaseShape(a_backend, *shape);
			actor->retiringShapes.clear();
		}
	}

	void Characters::Enumerate(Actor& a_actor, std::vector<Found>& o_found)
	{
		o_found.clear();
		std::vector<std::pair<RE::NiAVObject*, int>> stack;
		stack.emplace_back(a_actor.root.get(), 0);
		std::uint32_t visited = 0;
		while (!stack.empty() && visited < kMaxNodesPerActor) {
			auto [object, depth] = stack.back();
			stack.pop_back();
			++visited;
			if (!object || depth > 64)
				continue;
			// Hidden subtrees are not drawn (nor cast) by the game. The player's third-person root
			// is taken in first person too: the game casts the player's shadow with that model.
			if (object->GetFlags().any(RE::NiAVObject::Flag::kHidden) && !(a_actor.player && object == a_actor.root.get()))
				continue;
			if (auto* node = object->AsNode()) {
				if (netimmerse_cast<RE::NiBillboardNode*>(node))
					continue;
				for (auto& child : node->GetChildren())
					if (child)
						stack.emplace_back(child.get(), depth + 1);
				continue;
			}
			auto* geometry = object->AsGeometry();
			if (!geometry)
				continue;
			const auto type = geometry->GetType().get();
			if (type != RE::BSGeometry::Type::kTriShape && type != RE::BSGeometry::Type::kDynamicTriShape)
				continue;
			o_found.push_back(Found{ geometry, Signature(geometry) });
		}
	}

	bool Characters::ReadBones(const Shape& a_shape, const double a_origin[3], std::vector<float>& o_matrices)
	{
		const std::size_t count = a_shape.bones.size();
		static thread_local std::vector<RE::NiAVObject*> nodes;
		static thread_local std::vector<RE::NiTransform> skinToBone, worlds;
		nodes.resize(count);
		skinToBone.resize(count);
		worlds.resize(count);
		for (std::size_t i = 0; i < count; ++i) {
			nodes[i] = a_shape.bones[i].node.get();
			skinToBone[i] = a_shape.bones[i].skinToBone;
			if (!nodes[i])
				return false;
		}
		if (!ReadBoneWorlds(nodes.data(), skinToBone.data(), count, worlds.data()))
			return false;
		o_matrices.resize(count * 12);
		for (std::size_t i = 0; i < count; ++i)
			if (!StoreMatrix(worlds[i], a_origin, o_matrices.data() + i * 12))
				return false;
		return true;
	}

	std::unique_ptr<Characters::Shape> Characters::Capture(RayTracingRT::Backend& a_backend, Actor& a_actor, RE::BSGeometry* a_geometry,
		std::uint64_t a_signature)
	{
		auto logOther = [&](const char* a_what) {
			if (loggedOther_ < kLogLimit) {
				++loggedOther_;
				logger::info("[RayTracing] Characters: '{}' of {:08X} left to the shadow map: {}", a_geometry->name.c_str() ? a_geometry->name.c_str() : "",
					a_actor.ref ? a_actor.ref->GetFormID() : 0u, a_what);
			}
		};
		const auto& runtime = a_geometry->GetGeometryRuntimeData();
		// Shader rules: Lighting shader, casts shadows, no decal / refraction / LOD, not only blended.
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		using F = RE::BSShaderProperty::EShaderPropertyFlag;
		if (!lighting || lighting->flags.any(F::kDecal, F::kDynamicDecal, F::kRefraction, F::kLODObjects) || lighting->flags.none(F::kCastShadows)) {
			++stats_.skipShader;
			return nullptr;
		}
		auto* alphaProperty = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get());
		const bool alphaTested = alphaProperty && alphaProperty->GetAlphaTesting();
		if (alphaProperty && !alphaTested && alphaProperty->GetAlphaBlending()) {
			++stats_.skipShader;
			return nullptr;
		}
		// (45j) Hair is its own category (its cards are not a solid surface: the hair shader's own
		// shadowing looks better), the other physics parts (capes, skirts) another one.
		const int hairKind = HairKind(a_geometry, lighting);
		const bool hair = hairKind != 0;
		const bool physics = !hair && IsPhysicsPart(a_geometry, a_actor.root.get());
		if (hair && !hair_) {
			++stats_.skipHairOff;
			return nullptr;
		}
		if (physics && !physics_) {
			++stats_.skipPhysicsOff;
			return nullptr;
		}
		if (hair)
			++(hairKind == 1 ? stats_.hairByTint : stats_.hairByName);
		if (alphaTested && (!alpha_ || !a_backend.SkinAlphaAvailable())) {
			++stats_.skipAlphaOff;
			return nullptr;
		}

		auto shape = std::make_unique<Shape>();
		shape->geometry = RE::NiPointer<RE::BSGeometry>(a_geometry);
		shape->signature = a_signature;
		shape->physics = physics;
		shape->hair = hair;

		// --- Vertex data: the skin partition's (skinned) or the shape's own CPU copy (rigid).
		auto* skin = runtime.skinInstance.get();
		const bool dynamic = a_geometry->GetType().get() == RE::BSGeometry::Type::kDynamicTriShape;
		std::uint32_t vertexCount = 0;
		RE::BSGraphics::VertexDesc vertexDesc{};
		const std::uint8_t* rawVertices = nullptr;
		std::vector<RE::NiSkinPartition::Partition> partitions;
		SkinRefs refs;
		std::uint64_t partitionMask = ~0ull;
		if (skin) {
			if (!ReadSkinRefs(skin, refs) || refs.partitions == 0 || refs.partitions > kMaxPartitions) {
				++stats_.skipNoCpuData;
				logOther("no skin data / partitions");
				return nullptr;
			}
			if (refs.boneCount == 0 || refs.boneCount > 256) {
				++stats_.skipBadData;
				logOther("bone count out of range");
				return nullptr;
			}
			partitions.resize(refs.partitions);
			if (!SafeCopy(partitions.data(), refs.partitionData, partitions.size() * sizeof(RE::NiSkinPartition::Partition)) || !partitions[0].buffData) {
				++stats_.skipNoCpuData;
				logOther("skin partitions unreadable");
				return nullptr;
			}
			RE::BSGraphics::TriShape buffer{};
			if (!SafeCopy(&buffer, partitions[0].buffData, sizeof(buffer))) {
				++stats_.skipNoCpuData;
				return nullptr;
			}
			vertexDesc = buffer.vertexDesc;
			rawVertices = buffer.rawVertexData;
			vertexCount = refs.vertexCount;
			partitionMask = PartitionMask(skin, refs.partitions);
			shape->skin = RE::NiPointer<RE::NiSkinInstance>(skin);
		} else {
			if (dynamic || !runtime.rendererData) {
				++stats_.skipGeometryType;
				return nullptr;
			}
			vertexDesc = runtime.rendererData->vertexDesc;
			rawVertices = runtime.rendererData->rawVertexData;
			vertexCount = a_geometry->AsTriShape() ? a_geometry->AsTriShape()->GetTrishapeRuntimeData().vertexCount : 0;
		}
		if (vertexCount == 0 || vertexCount > 65535) {
			++stats_.skipBadData;
			logOther("vertex count out of range");
			return nullptr;
		}

		std::uint64_t descBits = 0;
		std::memcpy(&descBits, &vertexDesc, sizeof(descBits));
		const std::uint32_t stride = static_cast<std::uint32_t>(descBits & 0xF) * 4;
		const auto flags = vertexDesc.GetFlags();
		if (stride == 0 || (!rawVertices && !dynamic)) {
			++stats_.skipNoCpuData;
			logOther("no CPU vertex copy");
			return nullptr;
		}
		static thread_local std::vector<std::uint8_t> bytes;
		bytes.resize(static_cast<std::size_t>(vertexCount) * stride);
		if (rawVertices && !SafeCopy(bytes.data(), rawVertices, bytes.size())) {
			++stats_.skipBadData;
			logOther("vertex copy unreadable");
			return nullptr;
		}
		// Next attribute offset after a given one: the size of that attribute's block.
		auto blockSize = [&](RE::BSGraphics::Vertex::Attribute a_attribute) {
			const std::uint32_t offset = vertexDesc.GetAttributeOffset(a_attribute);
			std::uint32_t next = stride;
			for (std::uint32_t attribute = 0; attribute < RE::BSGraphics::Vertex::VA_COUNT; ++attribute) {
				if (!(flags & (1u << attribute)))
					continue;
				const std::uint32_t o = vertexDesc.GetAttributeOffset(static_cast<RE::BSGraphics::Vertex::Attribute>(attribute));
				if (o > offset && o < next)
					next = o;
			}
			return next > offset ? next - offset : 0u;
		};

		// Positions: the morphed ones of a BSDynamicTriShape (face), else from the vertex data.
		std::vector<float> positions(static_cast<std::size_t>(vertexCount) * 3);
		bool havePositions = false;
		if (dynamic) {
			auto& dynamicData = static_cast<RE::BSDynamicTriShape*>(a_geometry)->GetDynamicTrishapeRuntimeData();
			static thread_local std::vector<float> morphed;
			morphed.resize(static_cast<std::size_t>(vertexCount) * 4);
			bool copied = false;
			{
				RE::BSSpinLockGuard lock(dynamicData.lock);
				if (dynamicData.dynamicData && dynamicData.dataSize >= morphed.size() * sizeof(float))
					copied = SafeCopy(morphed.data(), dynamicData.dynamicData, morphed.size() * sizeof(float));
			}
			if (copied) {
				for (std::uint32_t v = 0; v < vertexCount; ++v)
					std::copy_n(morphed.data() + v * 4, 3, positions.data() + v * 3);
				havePositions = true;
				++stats_.dynamicShapes;
			}
		}
		if (!havePositions) {
			if (!(flags & RE::BSGraphics::Vertex::VF_VERTEX) || !rawVertices) {
				++stats_.skipNoCpuData;
				logOther("no vertex positions");
				return nullptr;
			}
			const std::uint32_t offset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
			const bool full = blockSize(RE::BSGraphics::Vertex::VA_POSITION) >= 12;
			if (offset + (full ? 12u : 6u) > stride) {
				++stats_.skipBadData;
				logOther("unknown vertex layout");
				return nullptr;
			}
			for (std::uint32_t v = 0; v < vertexCount; ++v) {
				const std::uint8_t* p = bytes.data() + static_cast<std::size_t>(v) * stride + offset;
				if (full) {
					std::memcpy(positions.data() + v * 3, p, sizeof(float) * 3);
				} else {
					DirectX::PackedVector::HALF h[3];
					std::memcpy(h, p, sizeof(h));
					for (int a = 0; a < 3; ++a)
						positions[v * 3 + a] = DirectX::PackedVector::XMConvertHalfToFloat(h[a]);
				}
			}
		}
		for (const float value : positions) {
			if (!std::isfinite(value) || std::abs(value) > 1.0e6f) {
				++stats_.skipBadData;
				logOther("non-finite vertex positions");
				return nullptr;
			}
		}

		// --- Indices (visible partitions only) and the partition of every vertex (palette mode).
		std::vector<std::uint32_t> indices;
		std::vector<std::uint8_t> vertexPartition(vertexCount, 0xFF);
		std::vector<std::vector<std::uint16_t>> palettes;
		if (skin) {
			palettes.resize(partitions.size());
			for (std::size_t p = 0; p < partitions.size(); ++p) {
				const auto& partition = partitions[p];
				if (partition.numBones > 0 && partition.numBones <= 256 && partition.bones) {
					palettes[p].resize(partition.numBones);
					if (!SafeCopy(palettes[p].data(), partition.bones, palettes[p].size() * sizeof(std::uint16_t)))
						palettes[p].clear();
				}
				if (!(partitionMask & (1ull << p)) || partition.triangles == 0 || !partition.buffData)
					continue;
				RE::BSGraphics::TriShape buffer{};
				if (!SafeCopy(&buffer, partition.buffData, sizeof(buffer)) || !buffer.rawIndexData) {
					++stats_.skipNoCpuData;
					logOther("no CPU index copy");
					return nullptr;
				}
				static thread_local std::vector<std::uint16_t> partIndices;
				partIndices.resize(static_cast<std::size_t>(partition.triangles) * 3);
				if (!SafeCopy(partIndices.data(), buffer.rawIndexData, partIndices.size() * sizeof(std::uint16_t))) {
					++stats_.skipBadData;
					return nullptr;
				}
				for (const auto index : partIndices) {
					if (index >= vertexCount) {
						++stats_.skipBadData;
						logOther("index out of range");
						return nullptr;
					}
					indices.push_back(index);
					if (vertexPartition[index] == 0xFF)
						vertexPartition[index] = static_cast<std::uint8_t>(p);
				}
			}
		} else {
			const std::uint32_t triangles = a_geometry->AsTriShape()->GetTrishapeRuntimeData().triangleCount;
			if (triangles == 0 || !runtime.rendererData->rawIndexData) {
				++stats_.skipNoCpuData;
				return nullptr;
			}
			static thread_local std::vector<std::uint16_t> rigidIndices;
			rigidIndices.resize(static_cast<std::size_t>(triangles) * 3);
			if (!SafeCopy(rigidIndices.data(), runtime.rendererData->rawIndexData, rigidIndices.size() * sizeof(std::uint16_t))) {
				++stats_.skipBadData;
				return nullptr;
			}
			for (const auto index : rigidIndices) {
				if (index >= vertexCount) {
					++stats_.skipBadData;
					logOther("index out of range");
					return nullptr;
				}
				indices.push_back(index);
			}
		}
		if (indices.empty() || indices.size() / 3 > 131072) {
			++stats_.skipBadData;
			return nullptr;
		}

		// --- Bones: the skin's list (world * skin-to-bone) or the shape itself (rigid).
		if (skin) {
			static thread_local std::vector<RE::NiAVObject*> boneNodes;
			static thread_local std::vector<RE::NiSkinData::BoneData> boneData;
			boneNodes.resize(refs.boneCount);
			boneData.resize(refs.boneCount);
			if (!SafeCopy(boneNodes.data(), refs.bones, boneNodes.size() * sizeof(RE::NiAVObject*)) ||
				!SafeCopy(boneData.data(), refs.boneData, boneData.size() * sizeof(RE::NiSkinData::BoneData))) {
				++stats_.skipBadData;
				logOther("bone list unreadable");
				return nullptr;
			}
			shape->bones.resize(refs.boneCount);
			for (std::uint32_t i = 0; i < refs.boneCount; ++i) {
				if (!boneNodes[i]) {
					++stats_.skipBadData;
					logOther("a bone is missing");
					return nullptr;
				}
				shape->bones[i].node = RE::NiPointer<RE::NiAVObject>(boneNodes[i]);
				shape->bones[i].skinToBone = boneData[i].skinToBone;
			}
		} else {
			shape->bones.resize(1);
			shape->bones[0].node = RE::NiPointer<RE::NiAVObject>(a_geometry);
		}

		// --- Weights and bone indices (as stored: partition-local or global, decided below).
		struct Influence
		{
			std::uint8_t bones[4];
			float weights[4];
		};
		std::vector<Influence> influences(vertexCount);
		if (skin) {
			if (!(flags & RE::BSGraphics::Vertex::VF_SKINNED) || !rawVertices) {
				++stats_.skipBadData;
				logOther("skinned shape without skinning data");
				return nullptr;
			}
			const std::uint32_t offset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_SKINNING);
			if (offset + 12 > stride) {
				++stats_.skipBadData;
				logOther("unknown skinning layout");
				return nullptr;
			}
			for (std::uint32_t v = 0; v < vertexCount; ++v) {
				const std::uint8_t* p = bytes.data() + static_cast<std::size_t>(v) * stride + offset;
				DirectX::PackedVector::HALF h[4];
				std::memcpy(h, p, sizeof(h));
				auto& influence = influences[v];
				std::memcpy(influence.bones, p + 8, 4);
				float sum = 0.0f;
				for (int i = 0; i < 4; ++i) {
					const float w = DirectX::PackedVector::XMConvertHalfToFloat(h[i]);
					influence.weights[i] = std::isfinite(w) && w > 0.0f ? w : 0.0f;
					sum += influence.weights[i];
				}
				if (sum > 1.0e-6f) {
					for (float& w : influence.weights)
						w /= sum;
				} else {
					influence.weights[0] = 1.0f;
					influence.weights[1] = influence.weights[2] = influence.weights[3] = 0.0f;
				}
			}
		} else {
			for (auto& influence : influences) {
				influence = Influence{ { 0, 0, 0, 0 }, { 1.0f, 0.0f, 0.0f, 0.0f } };
			}
		}

		// --- Placement check: skinned sample vertices must lie in the engine's bounds. For skinned
		// shapes this also decides how the stored bone indices are read: partition-local through the
		// partition's bone palette (how the game's vertex shader reads them), or global.
		const auto rootTranslate = a_actor.root->world.translate;
		const double origin[3] = { rootTranslate.x, rootTranslate.y, rootTranslate.z };
		std::vector<float> matrices;
		if (!ReadBones(*shape, origin, matrices)) {
			++stats_.skipBadData;
			logOther("bone transforms unreadable");
			return nullptr;
		}
		std::vector<std::uint32_t> sample;
		{
			std::vector<std::uint8_t> used(vertexCount, 0);
			for (const auto index : indices)
				used[index] = 1;
			std::vector<std::uint32_t> all;
			for (std::uint32_t v = 0; v < vertexCount; ++v)
				if (used[v])
					all.push_back(v);
			const std::size_t step = std::max<std::size_t>(1, all.size() / kPlacementSamples);
			for (std::size_t i = 0; i < all.size() && sample.size() < kPlacementSamples; i += step)
				sample.push_back(all[i]);
		}
		auto boundOk = [](const RE::NiBound& a_bound) {
			return std::isfinite(a_bound.radius) && a_bound.radius > 1.0f && a_bound.radius < 1.0e5f && std::isfinite(a_bound.center.x) &&
			       std::isfinite(a_bound.center.y) && std::isfinite(a_bound.center.z);
		};
		const RE::NiBound shapeBound = a_geometry->worldBound;
		const RE::NiBound rootBound = a_actor.root->worldBound;
		auto inside = [&](const RE::NiBound& a_bound, const RE::NiPoint3& a_p) {
			if (!boundOk(a_bound))
				return false;
			const double dx = a_p.x - (a_bound.center.x - origin[0]), dy = a_p.y - (a_bound.center.y - origin[1]), dz = a_p.z - (a_bound.center.z - origin[2]);
			const double limit = a_bound.radius * 1.5 + 64.0;
			return dx * dx + dy * dy + dz * dz <= limit * limit;
		};
		const std::uint32_t boneCount = static_cast<std::uint32_t>(shape->bones.size());
		// Mode 0: palette (partition-local), mode 1: global. Rigid shapes: one bone, mode 1.
		auto globalBone = [&](int a_mode, std::uint32_t a_vertex, std::uint8_t a_local) -> std::uint32_t {
			if (a_mode == 1)
				return a_local;
			const std::uint8_t p = vertexPartition[a_vertex];
			if (p == 0xFF || p >= palettes.size() || a_local >= palettes[p].size())
				return UINT32_MAX;
			return palettes[p][a_local];
		};
		auto evaluate = [&](int a_mode, std::uint32_t& o_outside) -> bool {
			o_outside = 0;
			for (const auto v : sample) {
				RE::NiPoint3 p{};
				for (int i = 0; i < 4; ++i) {
					const float w = influences[v].weights[i];
					if (w == 0.0f)
						continue;
					const std::uint32_t bone = globalBone(a_mode, v, influences[v].bones[i]);
					if (bone >= boneCount)
						return false;
					const RE::NiPoint3 q = Apply(matrices.data() + static_cast<std::size_t>(bone) * 12, positions.data() + static_cast<std::size_t>(v) * 3);
					p += q * w;
				}
				if (!inside(shapeBound, p) && !inside(rootBound, p))
					++o_outside;
			}
			return true;
		};
		int mode = -1;
		std::uint32_t outside[2] = { UINT32_MAX, UINT32_MAX };
		const bool checkable = boundOk(shapeBound) || boundOk(rootBound);
		for (const int candidate : { 0, 1 }) {
			if (!skin && candidate == 0)
				continue;
			if (!evaluate(candidate, outside[candidate]))
				continue;
			if (!checkable || outside[candidate] * 10 <= sample.size()) {
				mode = candidate;
				break;
			}
		}
		if (mode < 0) {
			++stats_.skipPlacement;
			if (loggedPlacement_ < kLogLimit) {
				++loggedPlacement_;
				logger::warn("[RayTracing] Characters: '{}' of {:08X} left to the shadow map: skinned vertices outside the engine's bounds "
							 "({} bones, {} partitions, {} vertices; outside of {} samples: palette {}, global {}; shape bound r {:.0f}, actor bound r {:.0f})",
					a_geometry->name.c_str() ? a_geometry->name.c_str() : "", a_actor.ref ? a_actor.ref->GetFormID() : 0u, boneCount, partitions.size(),
					vertexCount, sample.size(), outside[0] == UINT32_MAX ? std::string("bad index") : std::to_string(outside[0]),
					outside[1] == UINT32_MAX ? std::string("bad index") : std::to_string(outside[1]), shapeBound.radius, rootBound.radius);
			}
			return nullptr;
		}
		if (skin)
			++(mode == 0 ? stats_.paletteMode : stats_.globalMode);

		// --- The geometry for the backend (vertices of unused partitions are kept but never indexed).
		auto geometry = std::make_unique<RayTracingRT::SkinGeometry>();
		geometry->boneCount = boneCount;
		geometry->vertices.resize(vertexCount);
		for (std::uint32_t v = 0; v < vertexCount; ++v) {
			auto& out = geometry->vertices[v];
			std::copy_n(positions.data() + static_cast<std::size_t>(v) * 3, 3, out.position);
			out.bones = 0;
			for (int i = 0; i < 4; ++i) {
				float w = influences[v].weights[i];
				std::uint32_t bone = w != 0.0f ? globalBone(mode, v, influences[v].bones[i]) : 0u;
				if (bone >= boneCount) {
					// An unused vertex (no visible partition) or a stray index: anchored to bone 0.
					bone = 0;
					w = i == 0 ? 1.0f : 0.0f;
				}
				out.bones |= bone << (8 * i);
				out.weights[i] = w;
			}
		}
		geometry->indices = std::move(indices);

		// --- Alpha-tested parts: texcoords per triangle corner, texture and record.
		RayTracingRT::AlphaMaterial material{};
		RawAlpha rawAlpha{};
		bool vertexAlpha = false;
		if (alphaTested) {
			const auto test = static_cast<RE::NiAlphaProperty::TestFunction>((alphaProperty->alphaFlags >> 10) & 7);
			const std::uint32_t uvOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			if ((test != RE::NiAlphaProperty::TestFunction::kGreater && test != RE::NiAlphaProperty::TestFunction::kGreaterEqual) ||
				!(flags & RE::BSGraphics::Vertex::VF_UV) || uvOffset + 4 > stride || !rawVertices || !ReadAlpha(lighting, rawAlpha)) {
				++stats_.skipAlphaFailed;
				return nullptr;
			}
			material.threshold = static_cast<float>(alphaProperty->alphaThreshold) / 255.0f;
			material.alphaScale = rawAlpha.materialAlpha;
			std::copy_n(rawAlpha.uv, 4, material.uvTransform);
			const std::uint32_t colorOffset = vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
			vertexAlpha = (flags & RE::BSGraphics::Vertex::VF_COLORS) && colorOffset + 4 <= stride && lighting->flags.any(F::kVertexColors) &&
			              lighting->flags.none(F::kTreeAnim, F::kLODObjects, F::kHDLODObjects);
			geometry->alphaUV.resize(geometry->indices.size() * 2);
			for (std::size_t k = 0; k < geometry->indices.size(); ++k) {
				const std::uint8_t* vertex = bytes.data() + static_cast<std::size_t>(geometry->indices[k]) * stride;
				std::uint32_t uv;
				std::memcpy(&uv, vertex + uvOffset, sizeof(uv));
				if ((uv & 0x7C00u) == 0x7C00u)
					uv &= 0xFFFF0000u;
				if ((uv & 0x7C000000u) == 0x7C000000u)
					uv &= 0x0000FFFFu;
				const float a = vertexAlpha ? static_cast<float>(vertex[colorOffset + 3]) / 255.0f : 1.0f;
				geometry->alphaUV[k * 2] = uv;
				geometry->alphaUV[k * 2 + 1] = std::bit_cast<std::uint32_t>(a);
			}
		}

		shape->vertices = vertexCount;
		shape->handle = a_backend.CreateSkinShape(std::move(geometry));
		if (shape->handle == RayTracingRT::kInvalidSkin) {
			++stats_.skipBackend;
			return nullptr;
		}
		if (alphaTested) {
			shape->alpha = true;
			shape->texture = a_backend.AcquireAlphaTexture(rawAlpha.texture, rawAlpha.view);
			if (shape->texture != RayTracingRT::kInvalidAlphaTexture)
				shape->info = a_backend.CreateSkinAlphaInfo(shape->handle, shape->texture, material);
			if (shape->texture == RayTracingRT::kInvalidAlphaTexture || shape->info == RayTracingRT::kInvalidAlphaInfo) {
				++stats_.skipAlphaFailed;
				ReleaseShape(a_backend, *shape);
				return nullptr;
			}
		}
		shape->matrices = std::move(matrices);
		std::copy_n(origin, 3, shape->origin);
		return shape;
	}

	void Characters::Update(RayTracingRT::Backend& a_backend, const float a_rect[4])
	{
		const std::int64_t start = Qpc();
		if (flushRequested_.exchange(false))
			ReleaseAll(a_backend);
		// The scene dropped the caster filter (its own flush): registrations are made again.
		if (const std::uint32_t epoch = RayTracingCasterFilter::Epoch(); epoch != filterEpoch_) {
			filterEpoch_ = epoch;
			for (auto* list : { &actors_, &retiring_ })
				for (auto& actor : *list)
					for (auto* shapes : { &actor->shapes, &actor->retiringShapes })
						for (auto& shape : *shapes)
							shape->registered = false;
		}
		if (!a_backend.SkinAvailable()) {
			if (!Empty())
				ReleaseAll(a_backend);
			return;
		}

		// 1. The actors to trace: in the traced area, nearest first, at most kMaxActors.
		RE::NiPoint3 eye{};
		if (auto* camera = RE::Main::WorldRootCamera())
			eye = camera->world.translate;
		std::vector<std::pair<float, RE::Actor*>> candidates;
		auto consider = [&](RE::Actor* a_actor) {
			if (!a_actor || !a_actor->Is3DLoaded() || a_actor->IsDisabled() || a_actor->IsDeleted() || !a_actor->Get3D(false))
				return;
			const auto position = a_actor->GetPosition();
			if (position.x < a_rect[0] || position.y < a_rect[1] || position.x > a_rect[2] || position.y > a_rect[3])
				return;
			const float d2 = eye.GetSquaredDistance(position);
			const bool tracked = std::any_of(actors_.begin(), actors_.end(), [&](const std::unique_ptr<Actor>& a_entry) { return a_entry->ref.get() == a_actor; });
			const float limit = tracked ? kLeaveDistance : kEnterDistance;
			if (!std::isfinite(d2) || d2 > limit * limit)
				return;
			candidates.emplace_back(d2, a_actor);
		};
		auto* player = RE::PlayerCharacter::GetSingleton();
		consider(player);
		if (auto* processLists = RE::ProcessLists::GetSingleton()) {
			for (auto& handle : processLists->highActorHandles) {
				auto actor = handle.get();
				if (actor && actor.get() && actor.get() != player)
					consider(actor.get());
			}
		}
		std::sort(candidates.begin(), candidates.end(), [](const auto& a_lhs, const auto& a_rhs) { return a_lhs.first < a_rhs.first; });
		stats_.inRange = static_cast<std::uint32_t>(candidates.size());
		if (candidates.size() > kMaxActors)
			candidates.resize(kMaxActors);

		// 2. Actors that left (or got new 3D) retire: traced this frame with their last matrices.
		{
			std::vector<std::unique_ptr<Actor>> keep;
			for (auto& actor : actors_) {
				auto* ref = actor->ref.get();
				const bool wanted = std::any_of(candidates.begin(), candidates.end(), [&](const auto& a_candidate) { return a_candidate.second == ref; });
				if (wanted && ref->Get3D(false) == actor->root.get())
					keep.push_back(std::move(actor));
				else
					retiring_.push_back(std::move(actor));
			}
			actors_ = std::move(keep);
		}
		for (const auto& [d2, actorPtr] : candidates) {
			if (std::any_of(actors_.begin(), actors_.end(), [&](const std::unique_ptr<Actor>& a_entry) { return a_entry->ref.get() == actorPtr; }))
				continue;
			auto entry = std::make_unique<Actor>();
			entry->ref = RE::NiPointer<RE::TESObjectREFR>(actorPtr);
			entry->root = RE::NiPointer<RE::NiAVObject>(actorPtr->Get3D(false));
			entry->player = actorPtr == player;
			actors_.push_back(std::move(entry));
		}

		// 3. Shapes: new ones captured (within the budget), changed / gone ones retire.
		const std::int64_t deadline = start + static_cast<std::int64_t>(kCaptureBudgetMs / std::max(QpcMs(), 1e-9));
		std::uint32_t captures = 0;
		static thread_local std::vector<Found> found;
		for (auto& actor : actors_) {
			Enumerate(*actor, found);
			// Gone or changed shapes.
			{
				std::vector<std::unique_ptr<Shape>> keep;
				for (auto& shape : actor->shapes) {
					const auto it = std::find_if(found.begin(), found.end(), [&](const Found& a_found) { return a_found.geometry == shape->geometry.get(); });
					if (it != found.end() && it->signature == shape->signature) {
						keep.push_back(std::move(shape));
					} else {
						++stats_.retiredShapes;
						actor->retiringShapes.push_back(std::move(shape));
					}
				}
				actor->shapes = std::move(keep);
			}
			// New shapes.
			for (const auto& entry : found) {
				if (std::any_of(actor->shapes.begin(), actor->shapes.end(), [&](const std::unique_ptr<Shape>& a_shape) { return a_shape->geometry.get() == entry.geometry; }))
					continue;
				if (actor->refused.contains(entry.signature))
					continue;
				if (actor->shapes.size() >= kMaxShapesPerActor) {
					++stats_.skipShapeCap;
					actor->refused.insert(entry.signature);
					continue;
				}
				if (captures >= kCapturesPerFrame || (captures > 0 && Qpc() >= deadline))
					break;
				++captures;
				auto shape = Capture(a_backend, *actor, entry.geometry, entry.signature);
				if (shape)
					actor->shapes.push_back(std::move(shape));
				else
					actor->refused.insert(entry.signature);  // judged once (a changed shape has a new signature)
			}
			// A refused shape is judged again only when its geometry changes identity.
			if (actor->refused.size() > 1024)
				actor->refused.clear();
		}

		// 4. This frame's bone matrices; a bone that cannot be read keeps the last ones.
		stillReused_ = 0;
		for (auto& actor : actors_) {
			const auto translate = actor->root->world.translate;
			const double origin[3] = { translate.x, translate.y, translate.z };
			for (auto& shape : actor->shapes) {
				shape->reuse = false;
				static thread_local std::vector<float> fresh;
				if (ReadBones(*shape, origin, fresh)) {
					// (45n) Still pose: bit-identical to the matrices of last frame's draw (drawable then, so
					// skinned with them or itself reused from identical ones): last frame's skinned BLAS is
					// exact. The matrices are relative to the origin, which still moves the instance.
					const bool still = shape->drawable && !shape->matrices.empty() && fresh.size() == shape->matrices.size() &&
					                   std::memcmp(fresh.data(), shape->matrices.data(), fresh.size() * sizeof(float)) == 0 &&
					                   a_backend.IsSkinShapeReady(shape->handle);
					shape->matrices.swap(fresh);
					std::copy_n(origin, 3, shape->origin);
					if (still) {
						shape->reuse = true;
						++stillReused_;
					}
				}
			}
		}

		// 5. Caster filter: a shape leaves the sun cascades once it is traced.
		stats_.actors = static_cast<std::uint32_t>(actors_.size());
		stats_.shapes = stats_.registered = stats_.skinnedShapes = stats_.rigidShapes = stats_.physicsShapes = stats_.alphaShapes = 0;
		stats_.vertices = 0;
		stats_.hairShapes = 0;
		// (45j) In first person (weapons drawn especially) camera mods hide or fold the third-person
		// arms outside the shadow pass: the traced player then lost its arms. By default the player
		// stays in the shadow map in first person (still traced on the frame it switches).
		auto* playerCamera = RE::PlayerCamera::GetSingleton();
		const bool firstPerson = playerCamera && playerCamera->IsInFirstPerson();
		for (auto& actor : actors_) {
			// A skeleton that is not where its actor is (not animated: stale) is not traced: its
			// shapes go back to the shadow map (still traced this frame if they had left it).
			const auto position = actor->ref->GetPosition();
			const auto translate = actor->root->world.translate;
			const bool stale = !(position.GetSquaredDistance(translate) <= kStaleDistance * kStaleDistance) ||
			                   (actor->player && firstPerson && !firstPersonPlayer_);
			for (auto& shape : actor->shapes) {
				if (stale) {
					if (shape->registered) {
						RayTracingCasterFilter::Remove(shape->geometry.get());
						shape->registered = false;
					} else {
						shape->drawable = false;
					}
				} else {
					if (!shape->registered && a_backend.IsSkinShapeReady(shape->handle) &&
						(!shape->alpha || a_backend.IsAlphaTextureReady(shape->texture))) {
						RayTracingCasterFilter::Add(shape->geometry.get());
						shape->registered = true;
					}
					shape->drawable = !shape->alpha || a_backend.IsAlphaTextureReady(shape->texture);
				}
				++stats_.shapes;
				stats_.registered += shape->registered ? 1 : 0;
				++(shape->skin ? stats_.skinnedShapes : stats_.rigidShapes);
				stats_.physicsShapes += shape->physics ? 1 : 0;
				stats_.hairShapes += shape->hair ? 1 : 0;
				stats_.alphaShapes += shape->alpha ? 1 : 0;
				stats_.vertices += shape->vertices;
			}
		}
		stats_.lastMs = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
		stats_.maxMs = std::max(stats_.maxMs, stats_.lastMs);
	}

	void Characters::CollectDraws(std::vector<RayTracingRT::SkinDraw>& o_draws, std::vector<float>& o_bones) const
	{
		o_draws.clear();
		o_bones.clear();
		owners_.clear();
		auto add = [&](const Shape& a_shape, bool a_npc, std::uint32_t a_owner) {
			if (a_shape.handle == RayTracingRT::kInvalidSkin || a_shape.matrices.empty() || a_shape.matrices.size() != a_shape.bones.size() * 12)
				return;
			if (!a_shape.drawable || (a_shape.alpha && a_shape.info == RayTracingRT::kInvalidAlphaInfo))
				return;
			RayTracingRT::SkinDraw draw;
			draw.shape = a_shape.handle;
			draw.x = a_shape.origin[0];
			draw.y = a_shape.origin[1];
			draw.z = a_shape.origin[2];
			draw.boneOffset = static_cast<std::uint32_t>(o_bones.size() / 12);
			draw.alphaInfo = a_shape.alpha ? a_shape.info : RayTracingRT::kInvalidAlphaInfo;
			draw.npc = a_npc;
			draw.owner = a_owner;
			draw.reuse = a_shape.reuse;  // (45l) last frame's BLAS: no bones uploaded
			if (!draw.reuse)
				o_bones.insert(o_bones.end(), a_shape.matrices.begin(), a_shape.matrices.end());
			o_draws.push_back(draw);
		};
		for (const auto* list : { &actors_, &retiring_ }) {
			for (const auto& actor : *list) {
				// (46e) One key per reference (an actor with a new 3D may also be retiring with its old one).
				const void* ref = actor->ref.get();
				std::uint32_t owner = 0;
				if (ref) {
					for (const auto& [known, key] : owners_)
						if (known == ref)
							owner = key;
					if (owner == 0) {
						owner = static_cast<std::uint32_t>(owners_.size()) + 1;
						owners_.emplace_back(ref, owner);
					}
				}
				for (const auto* shapes : { &actor->shapes, &actor->retiringShapes })
					for (const auto& shape : *shapes)
						add(*shape, !actor->player, owner);
			}
		}
		std::sort(owners_.begin(), owners_.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
	}
}

namespace RayTracingCharacters
{
	void Characters::GetBounds(const double a_camera[3], float o_player[4], float o_others[4]) const
	{
		// (45l) The engine's world bound of each actor's 3D, with a margin (weapons, hair, a pose that
		// is a frame ahead of the bound); the other actors are merged into one sphere.
		constexpr float kMargin = 96.0f;
		constexpr float kFallbackRadius = 160.0f;
		std::fill_n(o_player, 4, 0.0f);
		std::fill_n(o_others, 4, 0.0f);
		bool haveOthers = false;
		double oc[3]{};
		double orad = 0.0;
		for (const auto* list : { &actors_, &retiring_ }) {
			for (const auto& actor : *list) {
				if (!actor || !actor->root)
					continue;
				const auto& bound = actor->root->worldBound;
				const auto& translate = actor->root->world.translate;
				double c[3];
				double r;
				if (std::isfinite(bound.radius) && bound.radius > 1.0f && bound.radius < 4096.0f) {
					c[0] = bound.center.x;
					c[1] = bound.center.y;
					c[2] = bound.center.z;
					r = bound.radius + kMargin;
				} else {
					c[0] = translate.x;
					c[1] = translate.y;
					c[2] = translate.z + 64.0;
					r = kFallbackRadius;
				}
				if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2]))
					continue;
				if (actor->player) {
					o_player[0] = static_cast<float>(c[0] - a_camera[0]);
					o_player[1] = static_cast<float>(c[1] - a_camera[1]);
					o_player[2] = static_cast<float>(c[2] - a_camera[2]);
					o_player[3] = static_cast<float>(std::max(r, static_cast<double>(o_player[3])));
					continue;
				}
				if (!haveOthers) {
					std::copy_n(c, 3, oc);
					orad = r;
					haveOthers = true;
					continue;
				}
				// Smallest sphere holding both.
				const double d[3] = { c[0] - oc[0], c[1] - oc[1], c[2] - oc[2] };
				const double dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
				if (dist + r <= orad)
					continue;
				if (dist + orad <= r) {
					std::copy_n(c, 3, oc);
					orad = r;
					continue;
				}
				const double newRadius = 0.5 * (dist + orad + r);
				const double k = (newRadius - orad) / std::max(dist, 1e-6);
				for (int i = 0; i < 3; ++i)
					oc[i] += d[i] * k;
				orad = newRadius;
			}
		}
		if (haveOthers) {
			o_others[0] = static_cast<float>(oc[0] - a_camera[0]);
			o_others[1] = static_cast<float>(oc[1] - a_camera[1]);
			o_others[2] = static_cast<float>(oc[2] - a_camera[2]);
			o_others[3] = static_cast<float>(orad);
		}
	}
}
