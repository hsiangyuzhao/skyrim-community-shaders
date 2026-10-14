#include "Scene.h"

#include "CasterFilter.h"

#include <DirectXPackedVector.h>
#include <bit>

namespace RayTracingScene
{
	namespace
	{
		constexpr std::uint32_t kChunkTriangles = 131072;
		constexpr std::size_t kValidatePerFrame = 384;
		constexpr std::uint32_t kSettleFrames = 30;     // a new cell waits this long before its capture starts
		constexpr std::uint32_t kRecaptureFrames = 90;  // a changed cell is captured again at most this often

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

		enum class RefKind
		{
			Capture,  // baked into the cell's chunks (never moves)
			Actor,
			Movable,  // (45e) doors, activators, movable statics, lights: own instances, moved per frame
			Item,     // (45e) loose items: own instances, moved per frame
			Tree,     // (45e) trees and plants: own instances, opaque parts only
			Other     // markers, sounds, hazards, projectiles, ...
		};

		RefKind Classify(RE::TESObjectREFR* a_ref)
		{
			if (a_ref->GetFormType() == RE::FormType::ActorCharacter)
				return RefKind::Actor;
			auto* base = a_ref->GetBaseObject();
			if (!base)
				return RefKind::Other;
			switch (base->GetFormType()) {
			case RE::FormType::Static:
			case RE::FormType::Furniture:
			case RE::FormType::Container:
				return RefKind::Capture;
			case RE::FormType::NPC:
				return RefKind::Actor;
			case RE::FormType::Door:
			case RE::FormType::Activator:
			case RE::FormType::TalkingActivator:
			case RE::FormType::MovableStatic:
			case RE::FormType::Light:
				return RefKind::Movable;
			case RE::FormType::Tree:
			case RE::FormType::Flora:
				return RefKind::Tree;
			case RE::FormType::Misc:
			case RE::FormType::Weapon:
			case RE::FormType::Armor:
			case RE::FormType::Book:
			case RE::FormType::AlchemyItem:
			case RE::FormType::Ingredient:
			case RE::FormType::KeyMaster:
			case RE::FormType::Ammo:
			case RE::FormType::SoulGem:
			case RE::FormType::Scroll:
			case RE::FormType::Note:
			case RE::FormType::Apparatus:
				return RefKind::Item;
			default:
				return RefKind::Other;
			}
		}

		bool LandscapeCaptured() { return kCaptureLandscape; }

		ObjectKind ToObjectKind(RefKind a_kind)
		{
			return a_kind == RefKind::Item ? ObjectKind::Item : (a_kind == RefKind::Tree ? ObjectKind::Tree : ObjectKind::Movable);
		}

		/// Copies engine-owned mesh memory; false instead of a crash if a mod left a bad pointer.
		bool SafeCopy(void* a_destination, const void* a_source, std::size_t a_bytes)
		{
			__try {
				std::memcpy(a_destination, a_source, a_bytes);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool IsEditorMarker(RE::NiAVObject* a_root)
		{
			const auto* bsx = a_root->GetExtraData<RE::BSXFlags>("BSX");
			return bsx && (static_cast<std::int32_t>(bsx->value) & static_cast<std::int32_t>(RE::BSXFlags::Flag::kEditorMarker));
		}

		/// (45g) This node's own userData (CommonLib's GetUserData() falls back to the parents).
		RE::TESObjectREFR* NodeOwner(RE::NiAVObject* a_node)
		{
			return REL::RelocateMember<RE::TESObjectREFR*>(a_node, 0x0F8, 0x110);
		}

		/// (45j) A plant word in a texture path, shape name or model path (case-insensitive).
		bool HasVegetationWord(const char* a_text)
		{
			if (!a_text || !*a_text)
				return false;
			static constexpr std::string_view kWords[] = { "plant", "tree", "bush", "shrub", "fern", "leaf", "leaves", "ivy", "vine", "flower", "grass",
				"hedge", "branch", "foliage", "moss", "reed", "weed", "thistle", "sapling", "bramble", "juniper", "lavender", "heather", "cattail",
				"creeper", "frond", "twig", "nirnroot", "deathbell", "mountainflower", "snowberr", "blisterwort", "canis", "elves ear", "tundra" };
			char lower[512];
			std::size_t n = 0;
			for (; a_text[n] && n + 1 < sizeof(lower); ++n)
				lower[n] = static_cast<char>(std::tolower(static_cast<unsigned char>(a_text[n])));
			const std::string_view text(lower, n);
			for (const auto word : kWords) {
				for (std::size_t at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1)) {
					// "street", "ravine": not plants.
					if (word == "tree" && at > 0 && text[at - 1] == 's')
						continue;
					if (word == "vine" && at > 0 && text[at - 1] == 'a')
						continue;
					return true;
				}
			}
			return false;
		}

		/// The diffuse texture path of a Lighting-shader shape (SEH: a mod can leave it dangling).
		bool ReadDiffusePath(RE::BSLightingShaderProperty* a_lighting, char (&o_path)[512])
		{
			o_path[0] = 0;
			__try {
				auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_lighting->material);
				if (!material)
					return false;
				RE::NiSourceTexture* source = material->diffuseTexture.get();
				const char* name = source ? source->name.c_str() : nullptr;
				if (!name)
					return false;
				strncpy_s(o_path, name, _TRUNCATE);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				o_path[0] = 0;
				return false;
			}
		}
	}

	bool Scene::IsVegetation(RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return false;
		const auto& runtime = a_geometry->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		if (lighting && lighting->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim))
			return true;
		// The owning reference: trees, flora and grass are plants whatever their textures say.
		int depth = 0;
		for (RE::NiAVObject* node = a_geometry; node && depth < 64; node = node->parent, ++depth) {
			if (auto* owner = NodeOwner(node)) {
				if (auto* base = owner->GetBaseObject()) {
					const auto type = base->GetFormType();
					if (type == RE::FormType::Tree || type == RE::FormType::Flora || type == RE::FormType::Grass)
						return true;
					if (auto* model = base->As<RE::TESModel>(); model && HasVegetationWord(model->GetModel()))
						return true;
				}
				break;
			}
		}
		if (HasVegetationWord(a_geometry->name.c_str()))
			return true;
		char path[512];
		return lighting && ReadDiffusePath(lighting, path) && HasVegetationWord(path);
	}

	const void* Scene::LandSignature(RE::TESObjectCELL* a_cell)
	{
		auto* land = a_cell->GetRuntimeData().cellLand;
		if (!land || !land->loadedData)
			return nullptr;
		// The quadrant meshes change identity whenever the land 3D is rebuilt.
		return land->loadedData->geom[0].get();
	}

	void Scene::IndexGeometry(const RE::BSGeometry* a_geometry, bool a_add)
	{
		if (!a_geometry)
			return;
		if (a_add) {
			++capturedGeometries_[a_geometry];
			return;
		}
		const auto it = capturedGeometries_.find(a_geometry);
		if (it != capturedGeometries_.end() && --it->second == 0)
			capturedGeometries_.erase(it);
	}

	void Scene::Bury(Cell& a_cell)
	{
		// (45j) The engine objects this capture held: dropped a bounded number per frame.
		for (auto& geometry : a_cell.geometries) {
			IndexGeometry(geometry.get(), false);
			if (geometry)
				geometryGrave_.push_back(std::move(geometry));
		}
		for (auto& tracked : a_cell.tracked) {
			const auto it = trackedRefs_.find(tracked.ref.get());
			if (it != trackedRefs_.end() && --it->second.count == 0)
				trackedRefs_.erase(it);
			if (tracked.ref)
				refGrave_.push_back(std::move(tracked.ref));
		}
		for (auto& ref : a_cell.pending)
			if (ref)
				refGrave_.push_back(std::move(ref));
	}

	void Scene::DrainGraves(double a_budgetMs)
	{
		// At least a few hundred per frame (most drops are plain reference count decrements), more
		// while the budget lasts. Leftovers wait for the next frame.
		const std::int64_t start = Qpc();
		const std::int64_t deadline = start + static_cast<std::int64_t>(a_budgetMs / std::max(QpcMs(), 1e-9));
		std::size_t dropped = 0;
		while ((!geometryGrave_.empty() || !refGrave_.empty()) && (dropped < 256 || Qpc() < deadline)) {
			for (int i = 0; i < 64 && !geometryGrave_.empty(); ++i, ++dropped)
				geometryGrave_.pop_back();
			for (int i = 0; i < 64 && !refGrave_.empty(); ++i, ++dropped)
				refGrave_.pop_back();
		}
		times_.release = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
		times_.releasePending = static_cast<std::uint32_t>(geometryGrave_.size() + refGrave_.size());
	}

	void Scene::Release(RayTracingRT::Backend& a_backend, Cell& a_cell)
	{
		for (const auto handle : a_cell.blas)
			a_backend.ReleaseBlas(handle);
		a_cell.blas.clear();
		a_cell.chunkGeometries.clear();
		a_cell.chunkStart = 0;
		if (a_cell.filterRegistered) {
			for (const auto& geometry : a_cell.geometries)
				RayTracingCasterFilter::Remove(geometry.get());
			a_cell.filterRegistered = false;
		}
		for (auto& shape : a_cell.alphaShapes) {  // (45h)
			IndexGeometry(shape.geometry.get(), false);
			ReleaseShape(a_backend, shape);
		}
		a_cell.alphaShapes.clear();
		Bury(a_cell);
		a_cell.geometries.clear();
		a_cell.tracked.clear();
		a_cell.pending.clear();
		a_cell.current.reset();
	}

	void Scene::ReleaseAll(RayTracingRT::Backend& a_backend)
	{
		for (auto& cell : cells_)
			Release(a_backend, *cell);
		cells_.clear();
		for (auto& cell : retiring_)
			Release(a_backend, *cell);
		retiring_.clear();
		for (auto* list : { &objects_, &retiringObjects_ }) {
			for (auto& object : *list)
				ReleaseObject(a_backend, *object);
			list->clear();
		}
		objectIndex_.clear();
		capturedGeometries_.clear();
		trackedRefs_.clear();
		persistent_ = {};
		for (auto& [key, mesh] : meshes_)  // every user released above; anything left is a bookkeeping slip
			a_backend.ReleaseBlas(mesh.blas);
		meshes_.clear();
		scanList_.clear();
		scanCursor_ = 0;
		RayTracingCasterFilter::Clear();
		cellsReady_ = 0;
		validationCursor_ = 0;
		const auto recaptures = stats_.recaptures;
		const auto maxGather = stats_.maxGatherMs;
		const auto logged = stats_.placementLogged;
		const auto missedDescribed = stats_.missedDescribed, missedCapturable = stats_.missedCapturable, missedRejected = stats_.missedRejected;
		const auto missedPending = stats_.missedPending, missedRecaptures = stats_.missedRecaptures;
		const auto alphaFailedMesh = stats_.alpha.failedMesh, alphaFailedTexture = stats_.alpha.failedTexture, alphaFailedRecord = stats_.alpha.failedRecord;
		stats_ = {};
		stats_.alpha.failedMesh = alphaFailedMesh;
		stats_.alpha.failedTexture = alphaFailedTexture;
		stats_.alpha.failedRecord = alphaFailedRecord;
		stats_.recaptures = recaptures;
		stats_.maxGatherMs = maxGather;
		stats_.placementLogged = logged;
		stats_.missedDescribed = missedDescribed;
		stats_.missedCapturable = missedCapturable;
		stats_.missedRejected = missedRejected;
		stats_.missedPending = missedPending;
		stats_.missedRecaptures = missedRecaptures;
		// A new area: its misses are counted afresh (the log's dedup and cap stay for the session).
		RayTracingCasterFilter::ResetMissed();
		missedJudged_.clear();
		recaptureAsked_.clear();
		physicsRefs_.clear();
	}

	bool Scene::Validate(const Cell& a_cell) const
	{
		// Still the same attached exterior cell. (Reference counts are not compared: characters walking
		// between cells change them all the time; captured references are re-checked one by one instead.)
		return a_cell.cell && a_cell.cell->GetFormID() == a_cell.formID && a_cell.cell->IsAttached();
	}

	void Scene::CloseChunk(RayTracingRT::Backend& a_backend, Cell& a_cell)
	{
		if (!a_cell.current || a_cell.current->Triangles() == 0)
			return;
		const auto handle = a_backend.CreateBlas(std::move(a_cell.current));
		const auto end = static_cast<std::uint32_t>(a_cell.geometries.size());
		if (handle != RayTracingRT::kInvalidBlas) {
			a_cell.blas.push_back(handle);
			a_cell.chunkGeometries.emplace_back(a_cell.chunkStart, end);
		}
		a_cell.chunkStart = end;
		a_cell.current.reset();
	}

	bool Scene::PlaceVertices(RE::BSGeometry* a_shape, const std::uint8_t* a_bytes, std::uint32_t a_count, std::uint32_t a_stride,
		std::uint32_t a_positionOffset, bool a_fullPrecision, double a_originX, double a_originY, std::vector<float>& o_positions, bool a_local)
	{
		auto decode = [&](std::uint32_t a_index, bool a_full) -> RE::NiPoint3 {
			const std::uint8_t* vertex = a_bytes + static_cast<std::size_t>(a_index) * a_stride + a_positionOffset;
			if (a_full) {
				float p[3];
				std::memcpy(p, vertex, sizeof(p));
				return { p[0], p[1], p[2] };
			}
			DirectX::PackedVector::HALF h[3];
			std::memcpy(h, vertex, sizeof(h));
			return { DirectX::PackedVector::XMConvertHalfToFloat(h[0]), DirectX::PackedVector::XMConvertHalfToFloat(h[1]),
				DirectX::PackedVector::XMConvertHalfToFloat(h[2]) };
		};
		const RE::NiTransform& world = a_shape->world;
		const auto& bound = a_shape->worldBound;

		// (45e) Placement check: the shape's vertices, placed with its world transform, must lie in
		// the bound sphere the engine itself computed for it (worldBound, used for its culling).
		// Shapes that fail with the vertex format read from the descriptor are tried with the other
		// position format; if neither fits they are left out (and keep their shadow-map shadow)
		// instead of putting triangles in the wrong place.
		auto maxDistance = [&](bool a_full) -> float {
			float worst = 0.0f;
			for (std::uint32_t i = 0; i < a_count; ++i) {
				const RE::NiPoint3 p = world * decode(i, a_full);
				const float dx = p.x - bound.center.x, dy = p.y - bound.center.y, dz = p.z - bound.center.z;
				const float d2 = dx * dx + dy * dy + dz * dz;
				if (!std::isfinite(d2))
					return FLT_MAX;
				worst = std::max(worst, d2);
			}
			return std::sqrt(worst);
		};
		const bool checkable = std::isfinite(bound.radius) && bound.radius > 0.0f && bound.radius < 1.0e6f &&
		                       std::isfinite(bound.center.x) && std::isfinite(bound.center.y) && std::isfinite(bound.center.z);
		const float limit = bound.radius * 1.5f + 32.0f;
		bool full = a_fullPrecision;
		if (checkable) {
			++stats_.placementChecked;
			const float primary = maxDistance(full);
			if (primary > limit) {
				const bool alternateFits = !full ? a_positionOffset + 12u <= a_stride : true;
				const float alternate = alternateFits ? maxDistance(!full) : FLT_MAX;
				if (alternate <= limit) {
					full = !full;
					++stats_.placementFixed;
				} else {
					++stats_.placementRejected;
				}
				if (stats_.placementLogged < 24) {
					++stats_.placementLogged;
					std::uint64_t descBits = 0;
					if (auto* data = a_shape->GetGeometryRuntimeData().rendererData)
						std::memcpy(&descBits, &data->vertexDesc, sizeof(descBits));
					logger::warn("[RayTracing] Placement check: '{}' {} (desc 0x{:016X}, stride {}, position at {}, {} positions): "
								 "bound centre ({:.0f}, {:.0f}, {:.0f}) radius {:.0f}, farthest vertex {:.0f} ({} {:.0f})",
						a_shape->name.c_str(), alternate <= limit ? "fixed with the other position format" : "left out",
						descBits, a_stride, a_positionOffset, a_fullPrecision ? "float" : "half", bound.center.x, bound.center.y,
						bound.center.z, bound.radius, primary, a_fullPrecision ? "as half:" : "as float:", alternate);
				}
				if (alternate > limit)
					return false;
			}
		} else {
			++stats_.placementUnchecked;
		}

		o_positions.clear();
		o_positions.reserve(static_cast<std::size_t>(a_count) * 3);
		for (std::uint32_t i = 0; i < a_count; ++i) {
			const RE::NiPoint3 local = decode(i, full);
			if (a_local) {
				o_positions.push_back(local.x);
				o_positions.push_back(local.y);
				o_positions.push_back(local.z);
				continue;
			}
			const RE::NiPoint3 p = world * local;
			o_positions.push_back(static_cast<float>(static_cast<double>(p.x) - a_originX));
			o_positions.push_back(static_cast<float>(static_cast<double>(p.y) - a_originY));
			o_positions.push_back(p.z);
		}
		return true;
	}

	bool Scene::ReadMesh(RE::BSTriShape* a_shape, bool a_landscape, RawMesh& o_mesh)
	{
		auto& skipped = stats_.skipped;
		auto* rendererData = a_shape->GetGeometryRuntimeData().rendererData;
		if (!rendererData || !rendererData->rawVertexData) {
			++skipped.noCpuData;
			return false;
		}
		const auto& counts = a_shape->GetTrishapeRuntimeData();
		const std::uint32_t vertexCount = counts.vertexCount;
		std::uint32_t triangleCount = counts.triangleCount;
		if (vertexCount == 0) {
			++skipped.badData;
			return false;
		}

		std::uint64_t descBits = 0;
		std::memcpy(&descBits, &rendererData->vertexDesc, sizeof(descBits));
		const std::uint32_t stride = static_cast<std::uint32_t>(descBits & 0xF) * 4;
		const auto flags = rendererData->vertexDesc.GetFlags();
		if (!(flags & RE::BSGraphics::Vertex::VF_VERTEX) || stride == 0) {
			++skipped.badData;
			return false;
		}
		// Size of the position block = distance to the next attribute: 16 bytes (float3 + w) for
		// full-precision meshes, 8 (half3 + w) otherwise.
		const std::uint32_t positionOffset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_POSITION);
		std::uint32_t next = stride;
		for (std::uint32_t attribute = 1; attribute < RE::BSGraphics::Vertex::VA_COUNT; ++attribute) {
			if (!(flags & (1u << attribute)))
				continue;
			const std::uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(static_cast<RE::BSGraphics::Vertex::Attribute>(attribute));
			if (offset > positionOffset && offset < next)
				next = offset;
		}
		const std::uint32_t positionBytes = next - positionOffset;
		const bool fullPrecision = positionBytes >= 12;
		if (positionOffset + (fullPrecision ? 12u : 6u) > stride) {
			++skipped.badData;
			return false;
		}
		if (!fullPrecision && positionBytes < 6) {
			++skipped.badData;
			return false;
		}

		// The engine's CPU copy of the vertices, copied out first (bad pointers from mods are skipped).
		o_mesh.vertexBytes.resize(static_cast<std::size_t>(vertexCount) * stride);
		if (!SafeCopy(o_mesh.vertexBytes.data(), rendererData->rawVertexData, o_mesh.vertexBytes.size())) {
			++skipped.badData;
			return false;
		}

		// Indices: the mesh's own, or the 16x16 quad grid every landscape quadrant uses. (45e) A
		// landscape quadrant always gets the generated grid, whatever index data it carries: the
		// game draws land with its own shared index buffer, and the quadrant's CPU index copy is not
		// a plain triangle list of these 289 vertices (the upstream ray tracer ignores it as well).
		o_mesh.indices.clear();
		if (a_landscape && vertexCount == 289) {
			o_mesh.indices.reserve(16 * 16 * 6);
			for (std::uint32_t y = 0; y < 16; ++y) {
				for (std::uint32_t x = 0; x < 16; ++x) {
					const std::uint32_t v0 = y * 17 + x, v1 = v0 + 1, v2 = v0 + 17, v3 = v2 + 1;
					o_mesh.indices.insert(o_mesh.indices.end(), { v0, v1, v2, v1, v3, v2 });
				}
			}
			triangleCount = 16 * 16 * 2;
		} else if (rendererData->rawIndexData && triangleCount > 0) {
			static thread_local std::vector<std::uint16_t> indexCopy;
			indexCopy.resize(static_cast<std::size_t>(triangleCount) * 3);
			if (!SafeCopy(indexCopy.data(), rendererData->rawIndexData, indexCopy.size() * sizeof(std::uint16_t))) {
				++skipped.badData;
				return false;
			}
			o_mesh.indices.reserve(indexCopy.size());
			for (const auto index : indexCopy) {
				if (index >= vertexCount) {
					++skipped.badData;
					return false;
				}
				o_mesh.indices.push_back(index);
			}
		} else {
			++skipped.noCpuData;
			return false;
		}

		o_mesh.vertexCount = vertexCount;
		o_mesh.triangleCount = triangleCount;
		o_mesh.stride = stride;
		o_mesh.positionOffset = positionOffset;
		o_mesh.fullPrecision = fullPrecision;
		// (45h) Texcoord 0 (half2) and vertex colour (ubyte4) for alpha-tested shapes.
		o_mesh.uvOffset = UINT32_MAX;
		o_mesh.colorOffset = UINT32_MAX;
		if (flags & RE::BSGraphics::Vertex::VF_UV) {
			const std::uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_TEXCOORD0);
			if (offset + 4 <= stride)
				o_mesh.uvOffset = offset;
		}
		if (flags & RE::BSGraphics::Vertex::VF_COLORS) {
			const std::uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_COLOR);
			if (offset + 4 <= stride)
				o_mesh.colorOffset = offset;
		}
		return true;
	}

	bool Scene::AppendShape(Cell& a_cell, RE::BSTriShape* a_shape, bool a_landscape)
	{
		static thread_local RawMesh mesh;
		if (!ReadMesh(a_shape, a_landscape, mesh))
			return false;

		const double originX = static_cast<double>(a_cell.x) * kCellUnits;
		const double originY = static_cast<double>(a_cell.y) * kCellUnits;
		static thread_local std::vector<float> placed;
		if (!PlaceVertices(a_shape, mesh.vertexBytes.data(), mesh.vertexCount, mesh.stride, mesh.positionOffset, mesh.fullPrecision, originX,
				originY, placed, false))
			return false;

		if (!a_cell.current)
			a_cell.current = std::make_unique<RayTracingRT::ChunkGeometry>();
		auto& chunk = *a_cell.current;
		const std::uint32_t base = chunk.Vertices();
		chunk.positions.insert(chunk.positions.end(), placed.begin(), placed.end());
		chunk.indices.reserve(chunk.indices.size() + mesh.indices.size());
		for (const auto index : mesh.indices)
			chunk.indices.push_back(base + index);

		a_cell.geometries.emplace_back(a_shape);
		IndexGeometry(a_shape, true);
		a_cell.triangles += mesh.triangleCount;
		if (a_landscape)
			++a_cell.landQuads;
		else
			++a_cell.shapes;

		if (chunk.Triangles() >= kChunkTriangles && gatherBackend_)
			CloseChunk(*gatherBackend_, a_cell);
		return true;
	}

	namespace
	{
		struct RawAlphaSource
		{
			ID3D11Texture2D* texture;
			ID3D11ShaderResourceView* view;
			float materialAlpha;
			float uv[4];
		};

		/// (45h) The material's diffuse texture and texcoord transform; false instead of a crash if a
		/// mod left a bad pointer.
		bool ReadAlphaSource(RE::BSLightingShaderProperty* a_lighting, RawAlphaSource& o_source)
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
	}

	bool Scene::GetAlphaSource(RE::BSGeometry* a_geometry, AlphaSource& o_source)
	{
		if (!a_geometry)
			return false;
		const auto& runtime = a_geometry->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		auto* alpha = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get());
		if (!lighting || !alpha || !alpha->GetAlphaTesting())
			return false;
		// The Lighting shader keeps a pixel when alpha >= its threshold: only the "greater" tests map to that.
		const auto test = static_cast<RE::NiAlphaProperty::TestFunction>((alpha->alphaFlags >> 10) & 7);
		if (test != RE::NiAlphaProperty::TestFunction::kGreater && test != RE::NiAlphaProperty::TestFunction::kGreaterEqual)
			return false;
		auto* rendererData = runtime.rendererData;
		if (!rendererData)
			return false;
		const auto vertexFlags = rendererData->vertexDesc.GetFlags();
		if (!(vertexFlags & RE::BSGraphics::Vertex::VF_UV))
			return false;
		RawAlphaSource raw{};
		if (!ReadAlphaSource(lighting, raw))
			return false;
		using F = RE::BSShaderProperty::EShaderPropertyFlag;
		o_source.texture = raw.texture;
		o_source.view = raw.view;
		o_source.material.threshold = static_cast<float>(alpha->alphaThreshold) / 255.0f;
		o_source.material.alphaScale = raw.materialAlpha;
		std::copy_n(raw.uv, 4, o_source.material.uvTransform);
		// Lighting.hlsl: alpha *= vertex colour alpha, except for tree-animated and LOD shapes.
		o_source.vertexAlpha = (vertexFlags & RE::BSGraphics::Vertex::VF_COLORS) && lighting->flags.any(F::kVertexColors) &&
		                       lighting->flags.none(F::kTreeAnim, F::kLODObjects, F::kHDLODObjects);
		return true;
	}

	const char* Scene::ShapeRejectReason(RE::NiAVObject* a_object, bool a_allowTreeAnim, bool a_allowCutout, SkipCounts* o_counts,
		MissReason* o_reason, std::uint8_t a_alphaKinds, bool* o_alpha, bool* o_foliage)
	{
		SkipCounts ignored;
		MissReason ignoredReason;
		auto& skipped = o_counts ? *o_counts : ignored;
		auto& reason = o_reason ? *o_reason : ignoredReason;
		reason = MissReason::Other;
		if (o_alpha)
			*o_alpha = false;
		if (o_foliage)
			*o_foliage = false;
		auto* geometry = a_object->AsGeometry();
		if (!geometry)
			return "not geometry";
		if (geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
			++skipped.otherGeometry;
			switch (geometry->GetType().get()) {
			case RE::BSGeometry::Type::kDynamicTriShape:
				return "not a plain BSTriShape (BSDynamicTriShape)";
			case RE::BSGeometry::Type::kMeshLODTriShape:
				return "not a plain BSTriShape (BSMeshLODTriShape)";
			case RE::BSGeometry::Type::kMultiIndexTriShape:
			case RE::BSGeometry::Type::kLODMultiIndexTriShape:
				return "not a plain BSTriShape (multi-index, snow)";
			case RE::BSGeometry::Type::kSubIndexTriShape:
				return "not a plain BSTriShape (sub-index)";
			case RE::BSGeometry::Type::kParticles:
			case RE::BSGeometry::Type::kStripParticles:
				return "not a plain BSTriShape (particles)";
			default:
				return "not a plain BSTriShape";
			}
		}
		const auto& runtime = geometry->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		if (!lighting) {
			++skipped.notLighting;
			reason = MissReason::NotLighting;
			return "not drawn with the Lighting shader";
		}
		using F = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto& flags = lighting->flags;
		if (flags.any(F::kSkinned) || runtime.skinInstance) {
			++skipped.skinned;
			reason = MissReason::Skinned;
			return "skinned";
		}
		if (flags.any(F::kDecal, F::kDynamicDecal)) {
			++skipped.decals;
			reason = MissReason::Decal;
			return "decal";
		}
		// (45h) Alpha-tested casters: a large enough alpha-tested shape whose cut-out can be traced
		// (diffuse texture, texcoords, a "greater" test) goes in as a non-opaque shape. Tree-animated
		// ones too (leaves; their wind sway is not followed). The other rules below still apply.
		// (45j) Vegetation (leaves, bushes) follows the Foliage setting, other cut-outs (fences,
		// banners) the Cut-outs setting; vegetation is never taken as an opaque occluder.
		bool anyHit = false;
		bool vegetation = false;
		auto* alphaTestProperty = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get());
		if (alphaTestProperty && alphaTestProperty->GetAlphaTesting()) {
			vegetation = IsVegetation(geometry);
			if (o_foliage)
				*o_foliage = vegetation;
			if (a_alphaKinds & (vegetation ? kAlphaFoliage : kAlphaCutouts)) {
				AlphaSource source;
				anyHit = std::isfinite(geometry->worldBound.radius) && geometry->worldBound.radius >= kAlphaMinRadius &&
				         GetAlphaSource(geometry, source);
			}
		}
		// Tree-animated (wind) shapes: only in trees / plants, whose opaque trunk and branch shapes
		// are separate from the alpha-tested leaf cards (those are rejected just below).
		if (flags.any(F::kTreeAnim) && !a_allowTreeAnim && !anyHit) {
			++skipped.foliage;
			return "tree-animated";
		}
		if (flags.any(F::kLODObjects, F::kLODLandscape)) {
			++skipped.otherGeometry;
			return "LOD shader flag";
		}
		if (flags.none(F::kCastShadows)) {
			++skipped.noShadow;
			return "does not cast shadows";
		}
		// (45g) Heat-haze planes of fires (refraction, usually under a billboard node): invisible.
		if (flags.any(F::kRefraction)) {
			++skipped.refraction;
			return "refraction (heat haze)";
		}
		// (45g) A shape under a camera-facing billboard turns towards the MAIN camera every frame:
		// its ray-traced shadow would move with the view. The shadow map renders it facing the
		// light, so it keeps its shadow-map shadow.
		for (auto* parent = geometry->parent; parent; parent = parent->parent) {
			if (netimmerse_cast<RE::NiBillboardNode*>(parent)) {
				++skipped.billboard;
				reason = MissReason::Billboard;
				return "under a camera-facing billboard node";
			}
		}
		if (auto* alpha = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get())) {
			if (alpha->GetAlphaTesting() && anyHit) {
				++skipped.alphaAnyHit;
				if (o_alpha)
					*o_alpha = true;
			} else if (alpha->GetAlphaTesting()) {
				if (a_alphaKinds & (vegetation ? kAlphaFoliage : kAlphaCutouts))
					++skipped.alphaUnusable;
				// (45g) CutoutAsOpaque: an alpha test that only trims edges (roof tiles, plank ends):
				// one-sided (cloth banners, leaves and fences with holes are two-sided), a large
				// shape, a "keep alpha above a low threshold" test and no blending on top.
				const auto test = static_cast<RE::NiAlphaProperty::TestFunction>((alpha->alphaFlags >> 10) & 7);
				const bool edgeOnly = (test == RE::NiAlphaProperty::TestFunction::kGreater || test == RE::NiAlphaProperty::TestFunction::kGreaterEqual) &&
				                      alpha->alphaThreshold <= kCutoutMaxThreshold && !alpha->GetAlphaBlending() && flags.none(F::kTwoSided, F::kTreeAnim) &&
				                      !vegetation &&
				                      std::isfinite(geometry->worldBound.radius) && geometry->worldBound.radius >= kCutoutMinRadius;
				if (a_allowCutout && edgeOnly) {
					++skipped.cutoutAsOpaque;
				} else {
					++skipped.alphaTested;
					reason = MissReason::AlphaTested;
					if (vegetation)
						return "alpha-tested (vegetation: leaves, bushes; follows the Foliage setting)";
					if (!a_allowCutout)
						return edgeOnly ? "alpha-tested (edge-only cutout; Treat Mostly-Opaque Alpha-Tested Shapes as Opaque is off)" : "alpha-tested";
					return flags.any(F::kTwoSided) ? "alpha-tested (two-sided)" :
					       !(geometry->worldBound.radius >= kCutoutMinRadius) ? "alpha-tested (small)" :
					                                                          "alpha-tested (threshold / test function)";
				}
			} else if (alpha->GetAlphaBlending()) {
				++skipped.alphaBlended;
				return "alpha-blended";
			}
		}
		if (!geometry->AsTriShape()) {
			++skipped.otherGeometry;
			return "not a BSTriShape";
		}
		return nullptr;
	}

	RE::BSTriShape* Scene::CheckShape(RE::NiAVObject* a_object, bool a_allowTreeAnim, bool a_allowCutout, std::uint8_t a_alphaKinds, bool* o_alpha,
		bool* o_foliage)
	{
		if (o_alpha)
			*o_alpha = false;
		if (o_foliage)
			*o_foliage = false;
		if (!a_object->AsGeometry() || ShapeRejectReason(a_object, a_allowTreeAnim, a_allowCutout, &stats_.skipped, nullptr, a_alphaKinds, o_alpha, o_foliage))
			return nullptr;
		return a_object->AsGeometry()->AsTriShape();
	}

	bool Scene::HasPhysicsPart(RE::NiAVObject* a_object, int a_depth)
	{
		auto* node = a_object ? a_object->AsNode() : nullptr;
		if (!node || a_depth > 16)
			return false;
		if (a_depth > 0 && node->collisionObject)
			return true;
		for (auto& child : node->GetChildren())
			if (HasPhysicsPart(child.get(), a_depth + 1))
				return true;
		return false;
	}

	bool Scene::VisitChildren(RE::NiNode* a_node)
	{
		if (netimmerse_cast<RE::NiBillboardNode*>(a_node)) {
			++stats_.skipped.billboard;
			return false;
		}
		return true;
	}

	namespace
	{
		/// (45g) The child of a NiSwitchNode the game draws; -1 = draw all (not a switch node / unknown).
		std::int32_t ActiveSwitchChild(RE::NiNode* a_node)
		{
			if (!netimmerse_cast<RE::NiSwitchNode*>(a_node))
				return -1;
			// NiSwitchNode::index (0x12C SE/AE, 0x154 VR): CommonLib's member offset is not valid in the multi-runtime build.
			return REL::RelocateMember<std::int32_t>(a_node, 0x12C, 0x154);
		}
	}

	bool Scene::CaptureResumable(Cell& a_cell, RE::NiAVObject* a_root, std::int64_t a_deadline)
	{
		leafIndex_ = 0;
		leafSkip_ = a_cell.shapeSkip;
		gatherAborted_ = false;
		gatherDeadline_ = a_deadline;
		CaptureObject(a_cell, a_root, 0);
		if (gatherAborted_) {
			gatherAborted_ = false;
			a_cell.shapeSkip = leafIndex_;
			a_cell.resumeRoot = a_root;
			return false;
		}
		a_cell.shapeSkip = 0;
		a_cell.resumeRoot = nullptr;
		return true;
	}

	void Scene::CaptureObject(Cell& a_cell, RE::NiAVObject* a_object, int a_depth)
	{
		if (!a_object || a_depth > 64 || gatherAborted_)
			return;
		if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
			++stats_.skipped.hidden;
			return;
		}
		if (auto* node = a_object->AsNode()) {
			if (!VisitChildren(node))
				return;
			const std::int32_t active = ActiveSwitchChild(node);
			std::int32_t index = 0;
			for (auto& child : node->GetChildren()) {
				if (active >= 0 && index++ != active) {
					if (child)
						++stats_.skipped.switchedOff;
					continue;
				}
				CaptureObject(a_cell, child.get(), a_depth + 1);
			}
			return;
		}
		// (45j) Resumable capture: shapes before the resume point were taken last frame; past the
		// deadline (after at least one shape) the rest waits for the next frame.
		const std::uint32_t leaf = leafIndex_++;
		if (leaf < leafSkip_)
			return;
		if (leaf > leafSkip_ && Qpc() >= gatherDeadline_) {
			gatherAborted_ = true;
			leafIndex_ = leaf;
			return;
		}
		bool alpha = false, foliage = false;
		if (auto* shape = CheckShape(a_object, false, config_.cutoutAsOpaque, AlphaKinds(), &alpha, &foliage)) {
			if (alpha) {
				// (45h) Its own non-opaque BLAS (shared per mesh) and instance, like an object.
				ObjectShape alphaShape;
				if (gatherBackend_ && CaptureAlphaShape(*gatherBackend_, shape, true, alphaShape)) {
					alphaShape.foliage = foliage;
					IndexGeometry(shape, true);
					a_cell.alphaShapes.push_back(std::move(alphaShape));
					++a_cell.shapes;
					return;
				}
				// Its cut-out cannot be traced: the 45g rules decide (opaque edge-only cut-out, or left out).
				if (ShapeRejectReason(shape, false, config_.cutoutAsOpaque, nullptr))
					return;
			}
			// (45j) Buildings (opaque statics, edge-only cut-outs) only when that category is ray traced.
			if (!config_.buildings)
				return;
			AppendShape(a_cell, shape, false);
		}
	}

	bool Scene::CaptureAlphaShape(RayTracingRT::Backend& a_backend, RE::BSTriShape* a_shape, bool a_allowNewMeshes, ObjectShape& o_shape)
	{
		auto& alphaStats = stats_.alpha;
		AlphaSource source;
		if (!GetAlphaSource(a_shape, source)) {
			++alphaStats.failedTexture;
			return false;
		}
		auto* data = a_shape->GetGeometryRuntimeData().rendererData;
		const auto& counts = a_shape->GetTrishapeRuntimeData();
		const MeshKey key{ data, counts.vertexCount, counts.triangleCount, static_cast<std::uint8_t>(source.vertexAlpha ? 2 : 1) };
		auto releaseIfUnused = [&](decltype(meshes_)::iterator a_it) {
			if (a_it->second.users == 0) {
				a_backend.ReleaseBlas(a_it->second.blas);
				alphaStats.meshTriangles -= std::min<std::uint64_t>(alphaStats.meshTriangles, a_it->second.triangles);
				meshes_.erase(a_it);
			}
		};
		auto it = meshes_.find(key);
		if (it == meshes_.end()) {
			if (!a_allowNewMeshes) {
				++stats_.objects.overBudget;
				return false;
			}
			static thread_local RawMesh mesh;
			static thread_local std::vector<float> local;
			if (counts.triangleCount == 0 || counts.triangleCount > kAlphaMaxTriangles || !ReadMesh(a_shape, false, mesh) || mesh.uvOffset == UINT32_MAX ||
				mesh.triangleCount > kAlphaMaxTriangles) {
				++alphaStats.failedMesh;
				return false;
			}
			if (!PlaceVertices(a_shape, mesh.vertexBytes.data(), mesh.vertexCount, mesh.stride, mesh.positionOffset, mesh.fullPrecision, 0.0, 0.0,
					local, true)) {
				++alphaStats.failedMesh;
				return false;
			}
			auto geometry = std::make_unique<RayTracingRT::ChunkGeometry>();
			geometry->positions = local;
			geometry->indices = mesh.indices;
			// Per triangle corner: the vertex's half2 texcoord as stored (non-finite halves -> 0) and
			// its vertex alpha (1 when the Lighting shader ignores it).
			geometry->alphaUV.resize(mesh.indices.size() * 2);
			const bool vertexAlpha = key.kind == 2 && mesh.colorOffset != UINT32_MAX;
			for (std::size_t k = 0; k < mesh.indices.size(); ++k) {
				const std::uint8_t* vertex = mesh.vertexBytes.data() + static_cast<std::size_t>(mesh.indices[k]) * mesh.stride;
				std::uint32_t uv;
				std::memcpy(&uv, vertex + mesh.uvOffset, sizeof(uv));
				if ((uv & 0x7C00u) == 0x7C00u)
					uv &= 0xFFFF0000u;
				if ((uv & 0x7C000000u) == 0x7C000000u)
					uv &= 0x0000FFFFu;
				const float alpha = vertexAlpha ? static_cast<float>(vertex[mesh.colorOffset + 3]) / 255.0f : 1.0f;
				geometry->alphaUV[k * 2] = uv;
				geometry->alphaUV[k * 2 + 1] = std::bit_cast<std::uint32_t>(alpha);
			}
			const auto handle = a_backend.CreateBlas(std::move(geometry));
			if (handle == RayTracingRT::kInvalidBlas) {
				++alphaStats.failedRecord;
				return false;
			}
			it = meshes_.emplace(key, SharedMesh{ handle, 0, mesh.triangleCount }).first;
			alphaStats.meshTriangles += mesh.triangleCount;
		}
		const auto texture = a_backend.AcquireAlphaTexture(source.texture, source.view);
		if (texture == RayTracingRT::kInvalidAlphaTexture) {
			++alphaStats.failedTexture;
			releaseIfUnused(it);
			return false;
		}
		const auto info = a_backend.CreateAlphaInfo(it->second.blas, texture, source.material);
		if (info == RayTracingRT::kInvalidAlphaInfo) {
			++alphaStats.failedRecord;
			a_backend.ReleaseAlphaTexture(texture);
			releaseIfUnused(it);
			return false;
		}
		++it->second.users;
		o_shape = ObjectShape{};
		o_shape.geometry = RE::NiPointer<RE::BSGeometry>(a_shape);
		o_shape.key = key;
		o_shape.blas = it->second.blas;
		o_shape.alpha = true;
		o_shape.texture = texture;
		o_shape.info = info;
		return true;
	}

	void Scene::ReleaseShape(RayTracingRT::Backend& a_backend, ObjectShape& a_shape)
	{
		if (a_shape.registered)
			RayTracingCasterFilter::Remove(a_shape.geometry.get());
		a_shape.registered = false;
		const auto it = meshes_.find(a_shape.key);
		if (it != meshes_.end() && --it->second.users == 0) {
			a_backend.ReleaseBlas(it->second.blas);
			auto& triangles = a_shape.key.kind ? stats_.alpha.meshTriangles : stats_.objects.meshTriangles;
			triangles -= std::min<std::uint64_t>(triangles, it->second.triangles);
			meshes_.erase(it);
		}
		if (a_shape.info != RayTracingRT::kInvalidAlphaInfo)
			a_backend.ReleaseAlphaInfo(a_shape.info);
		if (a_shape.texture != RayTracingRT::kInvalidAlphaTexture)
			a_backend.ReleaseAlphaTexture(a_shape.texture);
		a_shape.info = RayTracingRT::kInvalidAlphaInfo;
		a_shape.texture = RayTracingRT::kInvalidAlphaTexture;
		a_shape.ready = false;
		// (45j) The engine object is dropped later, a bounded number per frame (DrainGraves).
		if (a_shape.geometry)
			geometryGrave_.push_back(std::move(a_shape.geometry));
	}

	void Scene::AppendShapeInstance(const ObjectShape& a_shape, RayTracingRT::InstanceCategory a_category, std::uint32_t a_refID, bool a_expected,
		std::vector<RayTracingRT::Instance>& o_instances)
	{
		const auto* geometry = a_shape.geometry.get();
		// Not drawn by the game either: detached from the scene graph or hidden.
		if (!geometry->parent || geometry->GetFlags().any(RE::NiAVObject::Flag::kHidden))
			return;
		// (45h) An alpha shape only once its BLAS and texture are ready.
		if (a_shape.alpha && !a_shape.ready)
			return;
		// This frame's transform: doors swing, items fall, activators animate.
		const RE::NiTransform& world = geometry->world;
		RayTracingRT::Instance instance{ a_shape.blas, world.translate.x, world.translate.y, world.translate.z };
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				instance.m[r][c] = world.rotate.entry[r][c] * world.scale;
		instance.category = a_shape.alpha ? RayTracingRT::kCategoryAlpha : a_category;
		if (a_shape.alpha)
			instance.alphaInfo = a_shape.info;
		if (a_expected) {
			instance.refID = a_refID;
			instance.label = geometry->name.c_str();
			const auto& b = geometry->worldBound;
			if (std::isfinite(b.radius) && b.radius > 0.0f) {
				instance.hasExpected = true;
				const float center[3] = { b.center.x, b.center.y, b.center.z };
				for (int a = 0; a < 3; ++a) {
					instance.expectMin[a] = static_cast<double>(center[a]) - b.radius;
					instance.expectMax[a] = static_cast<double>(center[a]) + b.radius;
				}
			}
		}
		o_instances.push_back(instance);
	}

	// ---------------------------------------------------------------------------------------------
	// (45e) Objects: doors / activators / movable statics, items, opaque parts of trees and plants.
	// ---------------------------------------------------------------------------------------------

	void Scene::CollectObjectShapes(RE::NiAVObject* a_object, bool a_allowTreeAnim, bool a_allowCutout, int a_depth, std::vector<ObjectCandidate>& o_shapes)
	{
		if (!a_object || a_depth > 64)
			return;
		if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
			++stats_.skipped.hidden;
			return;
		}
		if (auto* node = a_object->AsNode()) {
			// (45g) Billboards turn towards the main camera (the fire's heat-haze plane gave a
			// ray-traced shadow that moved with the view).
			if (!VisitChildren(node))
				return;
			for (auto& child : node->GetChildren())
				CollectObjectShapes(child.get(), a_allowTreeAnim, a_allowCutout, a_depth + 1, o_shapes);
			return;
		}
		bool alpha = false, foliage = false;
		if (auto* shape = CheckShape(a_object, a_allowTreeAnim, a_allowCutout, AlphaKinds(), &alpha, &foliage))
			o_shapes.push_back(ObjectCandidate{ shape, alpha, foliage });
	}

	void Scene::CaptureObjectRef(RayTracingRT::Backend& a_backend, const RE::NiPointer<RE::TESObjectREFR>& a_ref, RE::NiAVObject* a_root,
		ObjectKind a_kind, bool a_allowNewMeshes)
	{
		auto entry = std::make_unique<ObjectRef>();
		entry->ref = a_ref;
		entry->root = a_root;
		entry->kind = a_kind;

		static thread_local std::vector<ObjectCandidate> shapes;
		shapes.clear();
		const bool allowTreeAnim = a_kind == ObjectKind::Tree;
		const bool allowCutout = a_kind != ObjectKind::Tree && config_.cutoutAsOpaque;
		// (45j) Opaque shapes by category: tree trunks only with Trees, doors / items only with Objects.
		const bool opaqueWanted = a_kind == ObjectKind::Tree ? config_.trees : config_.objects;
		CollectObjectShapes(a_root, allowTreeAnim, allowCutout, 0, shapes);

		static thread_local RawMesh mesh;
		static thread_local std::vector<float> local;
		for (const auto& [shape, alpha, foliage] : shapes) {
			if (alpha) {
				// (45h) Traced with its cut-out; if that is not possible the 45g rules decide.
				ObjectShape alphaShape;
				if (CaptureAlphaShape(a_backend, shape, a_allowNewMeshes, alphaShape)) {
					alphaShape.foliage = foliage;
					entry->shapes.push_back(std::move(alphaShape));
					continue;
				}
				if (ShapeRejectReason(shape, allowTreeAnim, allowCutout, nullptr))
					continue;
			}
			if (!opaqueWanted)
				continue;
			auto* data = shape->GetGeometryRuntimeData().rendererData;
			const auto& counts = shape->GetTrishapeRuntimeData();
			const MeshKey key{ data, counts.vertexCount, counts.triangleCount };
			auto it = meshes_.find(key);
			if (it == meshes_.end()) {
				// A mesh seen for the first time: its BLAS in the shape's own space, shared by every
				// object that uses the same engine mesh (all copies of a tree, a door, a plate).
				if (!a_allowNewMeshes) {
					++stats_.objects.overBudget;
					continue;
				}
				if (!ReadMesh(shape, false, mesh))
					continue;
				if (!PlaceVertices(shape, mesh.vertexBytes.data(), mesh.vertexCount, mesh.stride, mesh.positionOffset, mesh.fullPrecision, 0.0,
						0.0, local, true))
					continue;
				auto geometry = std::make_unique<RayTracingRT::ChunkGeometry>();
				geometry->positions = local;
				geometry->indices = mesh.indices;
				const auto handle = a_backend.CreateBlas(std::move(geometry));
				if (handle == RayTracingRT::kInvalidBlas)
					continue;
				it = meshes_.emplace(key, SharedMesh{ handle, 0, mesh.triangleCount }).first;
				stats_.objects.meshTriangles += mesh.triangleCount;
			}
			++it->second.users;
			ObjectShape objectShape;
			objectShape.geometry = RE::NiPointer<RE::BSGeometry>(shape);
			objectShape.key = key;
			objectShape.blas = it->second.blas;
			entry->shapes.push_back(std::move(objectShape));
		}

		++stats_.objects.refs[static_cast<int>(a_kind)];
		stats_.objects.shapes += static_cast<std::uint32_t>(entry->shapes.size());
		objectIndex_[a_ref.get()] = entry.get();
		objects_.push_back(std::move(entry));
	}

	void Scene::ReleaseObject(RayTracingRT::Backend& a_backend, ObjectRef& a_object)
	{
		for (auto& shape : a_object.shapes)
			ReleaseShape(a_backend, shape);
		a_object.shapes.clear();
		if (a_object.ref)
			refGrave_.push_back(std::move(a_object.ref));
	}

	void Scene::UpdateObjects(RayTracingRT::Backend& a_backend, std::int64_t a_deadline, bool a_allowNewMeshes)
	{
		// 1. Every captured object, every frame: still enabled, same 3D, still in a captured cell?
		//    Otherwise it retires (traced this frame, released in EndFrame); a changed one is
		//    captured again by the scan below.
		{
			std::vector<std::unique_ptr<ObjectRef>> keep;
			keep.reserve(objects_.size());
			for (auto& object : objects_) {
				auto* ref = object->ref.get();
				// (45f) Persistent references (doors, ...) belong to the worldspace's persistent
				// cell: they are in the scene while the cell of their square is captured.
				const bool inScene = CapturedCellOf(ref) != nullptr;
				bool valid = inScene && !ref->IsDisabled() && !ref->IsDeleted() && ref->Get3D() == object->root;
				if (valid && Classify(ref) == RefKind::Capture) {
					// (45g) A static with a Havok-driven part: only while its cell's capture says so.
					const auto it = physicsRefs_.find(ref);
					valid = it != physicsRefs_.end() && it->second == object->root;
				}
				if (valid) {
					keep.push_back(std::move(object));
				} else {
					objectIndex_.erase(ref);
					retiringObjects_.push_back(std::move(object));
				}
			}
			objects_ = std::move(keep);
		}

		// 2. Scan the captured cells' references (one cell after the other, a slice per frame) for
		//    objects not captured yet: the first capture of a cell, items dropped later, objects
		//    whose 3D changed.
		// (45j) 512 -> 128 references per frame: the scan never stops (items dropped later are found by
		// it), so its cost is paid every frame.
		constexpr std::size_t kScanPerFrame = 128;
		std::size_t scanned = 0;
		while (scanned < kScanPerFrame && Qpc() < a_deadline) {
			if (scanCursor_ >= scanList_.size()) {
				scanList_.clear();
				scanCursor_ = 0;
				// Next complete cell (round robin).
				Cell* next = nullptr;
				for (std::size_t i = 0; i < cells_.size() && !next; ++i) {
					auto& candidate = cells_[(scanCell_ + i) % cells_.size()];
					if (!candidate->gathering) {
						next = candidate.get();
						scanCell_ = (scanCell_ + i + 1) % cells_.size();
					}
				}
				if (!next)
					break;
				CollectCellRefs(next->cell, next->x, next->y, scanList_, nullptr);
				if (scanList_.empty())
					break;
			}
			const auto& refPtr = scanList_[scanCursor_++];
			++scanned;
			auto* ref = refPtr.get();
			if (!ref || objectIndex_.contains(ref))
				continue;
			RefKind kind = Classify(ref);
			if (kind == RefKind::Capture) {
				// (45g) A static with a Havok-driven part, judged while its cell was captured.
				const auto it = physicsRefs_.find(ref);
				if (it == physicsRefs_.end() || it->second != ref->Get3D())
					continue;
				kind = RefKind::Movable;
			}
			if (kind != RefKind::Movable && kind != RefKind::Item && kind != RefKind::Tree)
				continue;
			// (45j) Categories: doors / items with Objects, trees with Trees or ray-traced Foliage,
			// statics with a Havok-driven part with Buildings.
			if (kind == RefKind::Tree ? !(config_.trees || config_.foliage != FoliageMode::ShadowMap) :
			                            (Classify(ref) == RefKind::Capture ? !config_.buildings : !config_.objects))
				continue;
			if (ref->IsDisabled() || ref->IsDeleted())
				continue;
			auto* root = ref->Get3D();
			if (!root || IsEditorMarker(root))
				continue;
			const std::int64_t captureStart = Qpc();
			CaptureObjectRef(a_backend, refPtr, root, ToObjectKind(kind), a_allowNewMeshes);
			times_.objectCapture += static_cast<float>(static_cast<double>(Qpc() - captureStart) * QpcMs());
			++times_.objectsCaptured;
			auto* parent = ref->GetParentCell();
			if (!parent || std::none_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == parent; }))
				++stats_.persistentObjects;
		}

		// 3. Registration with the caster filter once a shape's BLAS is built (it is then in this
		//    frame's TLAS and in every one until EndFrame releases it).
		//    (45h) An alpha shape also waits for its texture (the 256x256 copy is made on D3D11 first).
		stats_.objects.registeredShapes = 0;
		for (auto& object : objects_) {
			for (auto& shape : object->shapes) {
				if (!shape.ready && a_backend.IsBlasBuilt(shape.blas) && (!shape.alpha || a_backend.IsAlphaTextureReady(shape.texture)))
					shape.ready = true;
				// (45j) Foliage "Both": traced, and still drawn into the cascades (the darker of both wins).
				if (shape.ready && !shape.registered && !(shape.foliage && config_.foliage == FoliageMode::Both)) {
					RayTracingCasterFilter::Add(shape.geometry.get());
					shape.registered = true;
				}
				if (shape.registered)
					++stats_.objects.registeredShapes;
			}
		}
		std::uint32_t alphaMeshes = 0;
		for (const auto& [key, mesh] : meshes_)
			alphaMeshes += key.kind != 0 ? 1 : 0;
		stats_.objects.meshes = static_cast<std::uint32_t>(meshes_.size()) - alphaMeshes;
		stats_.alpha.meshes = alphaMeshes;
		stats_.objects.captured = static_cast<std::uint32_t>(objects_.size());
	}

	void Scene::AppendObjectInstances(const std::vector<std::unique_ptr<ObjectRef>>& a_objects, std::vector<RayTracingRT::Instance>& o_instances,
		bool a_expected) const
	{
		for (const auto& object : a_objects) {
			const auto category = object->kind == ObjectKind::Item ? RayTracingRT::kCategoryItem :
			                      (object->kind == ObjectKind::Tree ? RayTracingRT::kCategoryTree : RayTracingRT::kCategoryMovable);
			const std::uint32_t refID = a_expected ? object->ref->GetFormID() : 0;
			for (const auto& shape : object->shapes)
				AppendShapeInstance(shape, category, refID, a_expected, o_instances);
		}
	}

	bool Scene::Gather(RayTracingRT::Backend& a_backend, Cell& a_cell, std::int64_t a_deadline)
	{
		gatherBackend_ = &a_backend;
		const std::int64_t start = Qpc();
		auto& skipped = stats_.skipped;

		if (!a_cell.snapshotTaken) {
			a_cell.snapshotTaken = true;
			CollectCellRefs(a_cell.cell, a_cell.x, a_cell.y, a_cell.pending, &stats_.persistentRefs);
			if constexpr (!kCaptureLandscape)
				a_cell.landDone = true;  // (45f) landscape only receives (kCaptureLandscape)
		}

		while (a_cell.cursor < a_cell.pending.size()) {
			if (Qpc() >= a_deadline)
				break;
			auto& refPtr = a_cell.pending[a_cell.cursor];
			auto* ref = refPtr.get();
			// (45j) A reference whose capture ran out of time last frame resumes at its next shape.
			if (a_cell.shapeSkip > 0) {
				auto* root = ref ? ref->Get3D() : nullptr;
				if (!root || root != a_cell.resumeRoot) {
					// Its 3D changed meanwhile: the tracked entry triggers a recapture of the cell.
					a_cell.shapeSkip = 0;
					a_cell.resumeRoot = nullptr;
					++a_cell.cursor;
					continue;
				}
				if (!CaptureResumable(a_cell, root, a_deadline))
					break;
				++a_cell.cursor;
				continue;
			}
			++a_cell.cursor;
			if (!ref)
				continue;
			switch (Classify(ref)) {
			case RefKind::Actor:
				++skipped.actors;
				continue;
			case RefKind::Movable:
			case RefKind::Item:
			case RefKind::Tree:
				continue;  // (45e) objects: own instances, captured by UpdateObjects' scan
			case RefKind::Other:
				++skipped.markers;
				continue;
			default:
				break;
			}
			const bool disabled = ref->IsDisabled() || ref->IsDeleted();
			auto* root = ref->Get3D();
			// Tracked either way: a reference that gets its 3D or is enabled later triggers a recapture.
			a_cell.tracked.push_back(TrackedRef{ refPtr, root, disabled });
			{
				auto& entry = trackedRefs_[ref];  // (45j) missed-caster index
				++entry.count;
				entry.root = root;
			}
			if (disabled || !root) {
				++skipped.disabled;
				continue;
			}
			if (IsEditorMarker(root)) {
				++skipped.hidden;
				continue;
			}
			// (45g) Hinged signs and other statics with a Havok-driven part move: the object scan
			// gives them per-frame instances instead of baking them where they were at capture.
			if (HasPhysicsPart(root, 0)) {
				physicsRefs_[ref] = root;
				++skipped.physicsParts;
				continue;
			}
			physicsRefs_.erase(ref);
			// (45j) Nothing of a static is wanted when Buildings and every cut-out kind are off.
			if (!config_.buildings && AlphaKinds() == 0)
				continue;
			if (!CaptureResumable(a_cell, root, a_deadline)) {
				--a_cell.cursor;  // resumed next frame at the shape that did not fit
				break;
			}
		}

		if (a_cell.cursor >= a_cell.pending.size() && !a_cell.landDone && Qpc() < a_deadline) {
			a_cell.landDone = true;
			a_cell.landSignature = LandSignature(a_cell.cell);
			if (auto* land = a_cell.cell->GetRuntimeData().cellLand; land && land->loadedData) {
				for (auto& quad : land->loadedData->geom) {
					auto* shape = quad.get();
					if (!shape || shape->GetFlags().any(RE::NiAVObject::Flag::kHidden))
						continue;
					AppendShape(a_cell, shape, true);
				}
			}
		}

		a_cell.gatherMs += static_cast<double>(Qpc() - start) * QpcMs();
		if (a_cell.cursor < a_cell.pending.size() || !a_cell.landDone)
			return false;

		CloseChunk(a_backend, a_cell);
		a_cell.pending.clear();
		a_cell.pending.shrink_to_fit();
		a_cell.gathering = false;
		a_cell.completedFrame = currentFrame_;
		stats_.lastCellMs = static_cast<float>(a_cell.gatherMs);
		return true;
	}

	void Scene::Update(RayTracingRT::Backend& a_backend, float a_budgetMs, std::uint32_t a_frame)
	{
		const std::int64_t frameStart = Qpc();
		times_.objectCapture = 0.0f;
		times_.objectsCaptured = 0;
		times_.recaptures = 0;
		times_.gridDiff = times_.gather = times_.registration = times_.objects = 0.0f;
		currentFrame_ = a_frame;
		if (flushRequested_.exchange(false)) {
			ReleaseAll(a_backend);
		}

		auto* tes = RE::TES::GetSingleton();
		if (!tes || tes->interiorCell || !tes->gridCells || tes->gridCells->length == 0 || tes->gridCells->length > 63) {
			if (!cells_.empty())
				ReleaseAll(a_backend);
			stats_.lastGatherMs = 0.0f;
			times_.update = 0.0f;
			return;
		}

		// 1. Grid diff: cells that left the grid (or are no longer the same attached cell) go now.
		auto* grid = tes->gridCells;
		std::vector<RE::TESObjectCELL*> attached;
		attached.reserve(static_cast<std::size_t>(grid->length) * grid->length);
		for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
			for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
				auto* cell = grid->GetCell(gx, gy);
				if (cell && cell->IsAttached() && cell->IsExteriorCell() && cell->GetCoordinates())
					attached.push_back(cell);
			}
		}
		// (45e) Retired, not released: still traced this frame (the shadow map may have left their
		// casters out already), released in EndFrame.
		RetireIf([&](const Cell& a_cell) {
			const bool inGrid = std::find(attached.begin(), attached.end(), a_cell.cell) != attached.end();
			return !(inGrid && Validate(a_cell));
		});

		// 2. Changes inside captured cells (landscape rebuilt, a captured reference got new 3D or
		// was enabled / disabled) mark the cell dirty; a sample of references is checked per frame.
		{
			std::size_t total = 0;
			for (const auto& cell : cells_) {
				total += cell->tracked.size();
				if (LandscapeCaptured() && !cell->gathering && LandSignature(cell->cell) != cell->landSignature)
					cell->dirty = true;
			}
			if (total > 0) {
				validationCursor_ %= total;
				const std::size_t window = std::min(kValidatePerFrame, total);
				std::size_t index = 0;
				for (const auto& cell : cells_) {
					for (const auto& tracked : cell->tracked) {
						const std::size_t g = index++;
						if (cell->gathering || cell->dirty || cell->replacing || (g + total - validationCursor_) % total >= window)
							continue;
						auto* ref = tracked.ref.get();
						const bool disabled = ref->IsDisabled() || ref->IsDeleted();
						if (disabled != tracked.disabled || (!disabled && ref->Get3D() != tracked.root))
							cell->dirty = true;
					}
				}
				validationCursor_ += window;
			}
			// Recaptured no more often than every kRecaptureFrames per cell: a cell whose 3D is still
			// streaming in settles first instead of being rebuilt for every object that appears.
			// (45e) The old capture stays live (traced, its casters left out of the shadow map) until
			// the replacement's BLASes are all built; step 5 then retires it. Releasing it at once
			// left a hole -- no ray-traced shadow and, where the filter had already dropped the
			// casters, no shadow at all -- for the settle + capture + build time of the cell.
			std::vector<std::unique_ptr<Cell>> replacements;
			for (auto& cell : cells_) {
				if (!cell->dirty || cell->replacing || a_frame - cell->completedFrame < kRecaptureFrames)
					continue;
				++stats_.recaptures;
				++times_.recaptures;
				cell->dirty = false;
				cell->replacing = true;
				auto entry = std::make_unique<Cell>();
				entry->cell = cell->cell;
				entry->formID = cell->formID;
				entry->x = cell->x;
				entry->y = cell->y;
				entry->seenFrame = a_frame - kSettleFrames;  // settled already: capture right away
				entry->replaces = true;
				replacements.push_back(std::move(entry));
			}
			for (auto& entry : replacements)
				cells_.push_back(std::move(entry));
		}

		// 3. New cells (captured once they have been in the grid for kSettleFrames).
		for (auto* cell : attached) {
			const bool known = std::any_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == cell; });
			if (known)
				continue;
			auto entry = std::make_unique<Cell>();
			entry->cell = cell;
			entry->formID = cell->GetFormID();
			entry->x = cell->GetCoordinates()->cellX;
			entry->y = cell->GetCoordinates()->cellY;
			entry->seenFrame = a_frame;
			cells_.push_back(std::move(entry));
		}

		const std::int64_t gatherStart = Qpc();
		times_.gridDiff = static_cast<float>(static_cast<double>(gatherStart - frameStart) * QpcMs());
		// 4. Capture, nearest cells first, within the budget.
		RE::NiPoint3 eye{};
		if (auto* camera = RE::Main::WorldRootCamera())
			eye = camera->world.translate;
		const double qpcPerMs = 1.0 / QpcMs();
		const std::int64_t deadline = frameStart + static_cast<std::int64_t>(static_cast<double>(a_budgetMs) * qpcPerMs);
		for (;;) {
			Cell* nearest = nullptr;
			float best = FLT_MAX;
			for (auto& cell : cells_) {
				if (!cell->gathering || a_frame - cell->seenFrame < kSettleFrames)
					continue;
				const float dx = (static_cast<float>(cell->x) + 0.5f) * kCellUnits - eye.x;
				const float dy = (static_cast<float>(cell->y) + 0.5f) * kCellUnits - eye.y;
				const float d = dx * dx + dy * dy;
				if (d < best) {
					best = d;
					nearest = cell.get();
				}
			}
			if (!nearest || Qpc() >= deadline)
				break;
			if (!Gather(a_backend, *nearest, deadline))
				break;
		}

		const std::int64_t registrationStart = Qpc();
		times_.gather = static_cast<float>(static_cast<double>(registrationStart - gatherStart) * QpcMs());
		// 5. Caster filter: a cell's shapes leave the cascades once all its BLASes are built.
		// Registration happens here, before this frame's trace: a registered cell is built, so it is
		// in this frame's TLAS and in every later one until EndFrame releases it.
		cellsReady_ = 0;
		stats_.cellsTracked = 0;
		stats_.cellsComplete = stats_.cellsGathering = 0;
		stats_.shapes = stats_.landQuads = 0;
		stats_.triangles = 0;
		std::vector<RE::TESObjectCELL*> replaced;
		for (auto& cell : cells_) {
			const bool built = !cell->gathering &&
			                   std::all_of(cell->blas.begin(), cell->blas.end(), [&](auto a_handle) { return a_backend.IsBlasBuilt(a_handle); });
			if (built && !cell->filterRegistered) {
				for (const auto& geometry : cell->geometries)
					RayTracingCasterFilter::Add(geometry.get());
				cell->filterRegistered = true;
			}
			if (built && cell->replaces) {
				cell->replaces = false;
				replaced.push_back(cell->cell);
			}
			// (45h) The cell's alpha shapes, each once its BLAS and texture are ready (then traced).
			for (auto& shape : cell->alphaShapes) {
				if (!shape.ready && a_backend.IsBlasBuilt(shape.blas) && a_backend.IsAlphaTextureReady(shape.texture))
					shape.ready = true;
				if (shape.ready && !shape.registered && !(shape.foliage && config_.foliage == FoliageMode::Both)) {
					RayTracingCasterFilter::Add(shape.geometry.get());
					shape.registered = true;
				}
			}
		}
		if (!replaced.empty()) {
			RetireIf([&](const Cell& a_cell) {
				return a_cell.replacing && std::find(replaced.begin(), replaced.end(), a_cell.cell) != replaced.end();
			});
		}
		for (auto& cell : cells_) {
			if (cell->replaces)
				continue;  // counted once its old capture is gone
			++stats_.cellsTracked;
			stats_.shapes += cell->shapes;
			stats_.landQuads += cell->landQuads;
			stats_.triangles += cell->triangles;
			if (cell->gathering) {
				++stats_.cellsGathering;
				continue;
			}
			++stats_.cellsComplete;
			if (cell->filterRegistered)
				++cellsReady_;
		}

		const std::int64_t objectsStart = Qpc();
		times_.registration = static_cast<float>(static_cast<double>(objectsStart - registrationStart) * QpcMs());
		// 6. (45e) Objects with their own instances (doors, items, trees): validation, scan, filter.
		UpdateObjects(a_backend, deadline, a_budgetMs > 0.0f);
		times_.objects = static_cast<float>(static_cast<double>(Qpc() - objectsStart) * QpcMs());

		// (45h) Alpha shapes held / traced now.
		stats_.alpha.captured = stats_.alpha.registered = 0;
		for (const auto& cell : cells_) {
			for (const auto& shape : cell->alphaShapes) {
				++stats_.alpha.captured;
				stats_.alpha.registered += shape.ready ? 1 : 0;
			}
		}
		for (const auto& object : objects_) {
			for (const auto& shape : object->shapes) {
				if (!shape.alpha)
					continue;
				++stats_.alpha.captured;
				stats_.alpha.registered += shape.ready ? 1 : 0;
			}
		}

		stats_.lastGatherMs = static_cast<float>(static_cast<double>(Qpc() - frameStart) * QpcMs());
		times_.update = stats_.lastGatherMs;
		stats_.maxGatherMs = std::max(stats_.maxGatherMs, stats_.lastGatherMs);
	}

	void Scene::CollectInstances(std::vector<RayTracingRT::Instance>& o_instances, bool a_expected) const
	{
		const std::int64_t start = Qpc();
		o_instances.clear();
		// Retiring captures too: they are traced this frame and released after it.
		for (const auto* list : { &cells_, &retiring_ }) {
			for (const auto& cell : *list) {
				const double x = static_cast<double>(cell->x) * kCellUnits;
				const double y = static_cast<double>(cell->y) * kCellUnits;
				for (std::size_t i = 0; i < cell->blas.size(); ++i) {
					RayTracingRT::Instance instance{ cell->blas[i], x, y, 0.0 };
					instance.category = RayTracingRT::kCategoryCell;
					if (a_expected && i < cell->chunkGeometries.size()) {
						// (45f) The box of the live bound spheres of the shapes baked into this chunk.
						const auto [first, end] = cell->chunkGeometries[i];
						for (int a = 0; a < 3; ++a) {
							instance.expectMin[a] = DBL_MAX;
							instance.expectMax[a] = -DBL_MAX;
						}
						for (std::uint32_t g = first; g < end && g < cell->geometries.size(); ++g) {
							const auto& b = cell->geometries[g]->worldBound;
							if (!std::isfinite(b.radius) || b.radius <= 0.0f)
								continue;
							const float center[3] = { b.center.x, b.center.y, b.center.z };
							for (int a = 0; a < 3; ++a) {
								instance.expectMin[a] = std::min(instance.expectMin[a], static_cast<double>(center[a]) - b.radius);
								instance.expectMax[a] = std::max(instance.expectMax[a], static_cast<double>(center[a]) + b.radius);
							}
							instance.hasExpected = true;
						}
					}
					o_instances.push_back(instance);
				}
				// (45h) The cell's alpha shapes: an instance each (registered ones only).
				for (const auto& shape : cell->alphaShapes)
					AppendShapeInstance(shape, RayTracingRT::kCategoryAlpha, 0, a_expected, o_instances);
			}
		}
		AppendObjectInstances(objects_, o_instances, a_expected);
		AppendObjectInstances(retiringObjects_, o_instances, a_expected);
		times_.instances = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
	}

	void Scene::RefreshPersistent(bool a_force) const
	{
		auto* tes = RE::TES::GetSingleton();
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		auto* sky = worldSpace ? worldSpace->GetSkyCell() : nullptr;
		if (!sky) {
			persistent_ = {};
			return;
		}
		auto& data = sky->GetRuntimeData();
		const std::size_t count = data.references.size();
		// Rebuilt when the persistent cell is another one, or its reference list changed (at most
		// every 4 s for the object scan; a cell capture asks with a_force).
		if (persistent_.sky == sky && persistent_.references == count)
			return;
		if (persistent_.sky == sky && !a_force && currentFrame_ - persistent_.builtFrame < 240)
			return;
		persistent_.sky = sky;
		persistent_.references = count;
		persistent_.builtFrame = currentFrame_;
		persistent_.bySquare.clear();
		RE::BSSpinLockGuard lock(data.spinLock);
		for (const auto& ref : data.references) {
			if (!ref)
				continue;
			const auto position = ref->GetPosition();
			if (!std::isfinite(position.x) || !std::isfinite(position.y))
				continue;
			const auto x = static_cast<std::int32_t>(std::floor(position.x / kCellUnits));
			const auto y = static_cast<std::int32_t>(std::floor(position.y / kCellUnits));
			const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
			persistent_.bySquare[key].push_back(ref);
		}
	}

	void Scene::CollectCellRefs(RE::TESObjectCELL* a_cell, std::int32_t a_x, std::int32_t a_y, std::vector<RE::NiPointer<RE::TESObjectREFR>>& o_refs,
		std::uint32_t* o_persistent) const
	{
		{
			auto& data = a_cell->GetRuntimeData();
			RE::BSSpinLockGuard lock(data.spinLock);
			o_refs.reserve(o_refs.size() + data.references.size());
			for (const auto& ref : data.references)
				if (ref)
					o_refs.push_back(ref);
		}
		if constexpr (!kCapturePersistent)
			return;
		// (45f) Persistent references of an exterior worldspace are held by its persistent ("sky")
		// cell, not by the grid cell they stand in (CommonLib's TES::ForEachReferenceInRange walks
		// both for that reason). They are taken with the grid cell whose square holds them.
		// (45j) From an index by square (a cell capture refreshes it, the object scan reuses it).
		RefreshPersistent(o_persistent != nullptr);
		if (!persistent_.sky || persistent_.sky == a_cell)
			return;
		const std::uint64_t key = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(a_x)) << 32) | static_cast<std::uint32_t>(a_y);
		const auto it = persistent_.bySquare.find(key);
		if (it == persistent_.bySquare.end())
			return;
		for (const auto& ref : it->second) {
			o_refs.push_back(ref);
			if (o_persistent)
				++*o_persistent;
		}
	}

	const Scene::Cell* Scene::CapturedCellOf(RE::TESObjectREFR* a_ref) const
	{
		if (auto* parent = a_ref->GetParentCell()) {
			for (const auto& cell : cells_)
				if (cell->cell == parent)
					return cell.get();
		}
		if constexpr (!kCapturePersistent)
			return nullptr;
		const auto position = a_ref->GetPosition();
		const auto cx = static_cast<std::int32_t>(std::floor(position.x / kCellUnits));
		const auto cy = static_cast<std::int32_t>(std::floor(position.y / kCellUnits));
		auto* tes = RE::TES::GetSingleton();
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		auto* sky = worldSpace ? worldSpace->GetSkyCell() : nullptr;
		if (!sky || a_ref->GetParentCell() != sky)
			return nullptr;
		for (const auto& cell : cells_)
			if (cell->x == cx && cell->y == cy)
				return cell.get();
		return nullptr;
	}

	namespace
	{
		/// (45g) This node's own userData. CommonLib's NiAVObject::GetUserData() falls back to the
		/// parents, so the 45f walk stopped at the shape itself ("3D root differs" on every line).
		RE::TESObjectREFR* OwnUserData(RE::NiAVObject* a_node)
		{
			return REL::RelocateMember<RE::TESObjectREFR*>(a_node, 0x0F8, 0x110);
		}

		std::uint64_t MissKey(RE::FormID a_ref, std::string_view a_shape)
		{
			return (static_cast<std::uint64_t>(a_ref) << 32) ^ static_cast<std::uint64_t>(std::hash<std::string_view>()(a_shape) & 0xFFFFFFFFu);
		}
	}

	void Scene::DescribeMissedCasters()
	{
		const std::int64_t now = Qpc();
		struct ScopeTimer
		{
			float& out;
			std::int64_t start;
			~ScopeTimer() { out = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs()); }
		} scopeTimer{ times_.missed, now };
		times_.missedJudged = 0;
		auto flushLog = [&] {
			if (static_cast<double>(now - missedWindowStart_) * QpcMs() > kMissedLogWindowMs) {
				missedWindowStart_ = now;
				missedWindowCount_ = 0;
			}
			while (!missedBacklog_.empty() && missedWindowCount_ < kMissedLogBurst && stats_.missedDescribed < kMissedLogCap) {
				logger::warn("{}", missedBacklog_.front());
				missedBacklog_.pop_front();
				++missedWindowCount_;
				if (++stats_.missedDescribed == kMissedLogCap)
					logger::warn("[RayTracing] Missed caster: {} described, no further ones are logged this session (the page keeps counting)", kMissedLogCap);
			}
		};

		auto missed = RayTracingCasterFilter::TakeMissed();
		if (missed.empty()) {
			flushLog();
			return;
		}
		auto* tes = RE::TES::GetSingleton();
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		auto* sky = worldSpace ? worldSpace->GetSkyCell() : nullptr;
		auto requestRecapture = [&](const Cell* a_cell) {
			for (auto& cell : cells_) {
				if (cell.get() == a_cell && !cell->dirty) {
					cell->dirty = true;
					++stats_.missedRecaptures;
				}
			}
		};

		constexpr std::uint32_t kMissedPerFrame = 8;
		const std::int64_t deadline = now + static_cast<std::int64_t>(0.25 / std::max(QpcMs(), 1e-9));
		std::uint32_t examined = 0;
		for (const auto& pointer : missed) {
			auto* geometry = pointer.get();
			if (!geometry || missedJudged_.contains(geometry))
				continue;
			// (45j) A few per frame: the rest are noted again the next time they are drawn.
			if (examined >= kMissedPerFrame || (examined > 0 && Qpc() >= deadline)) {
				RayTracingCasterFilter::ForgetMissed(geometry);
				continue;
			}
			++examined;
			++times_.missedJudged;
			// The reference that owns the shape: the nearest node carrying it as its own user data.
			RE::TESObjectREFR* ref = nullptr;
			RE::NiAVObject* root = nullptr;
			int depth = 0;
			for (RE::NiAVObject* node = geometry; node && depth < 64; node = node->parent, ++depth) {
				if (auto* owner = OwnUserData(node)) {
					ref = owner;
					root = node;
					break;
				}
			}
			const RefKind kind = ref ? Classify(ref) : RefKind::Other;
			bool physics = false;
			if (kind == RefKind::Capture) {
				const auto it = physicsRefs_.find(ref);
				physics = it != physicsRefs_.end() && it->second == ref->Get3D();
			}
			const bool objectKind = kind == RefKind::Movable || kind == RefKind::Item || kind == RefKind::Tree || physics;
			MissReason bucket = MissReason::Other;
			const char* shapeReason = ShapeRejectReason(geometry, kind == RefKind::Tree, kind != RefKind::Tree && config_.cutoutAsOpaque, nullptr, &bucket, AlphaKinds());
			// (45j) Kinds left to the shadow map on purpose (Ray Tracing > Advanced).
			const bool byChoice = (kind == RefKind::Tree && !config_.trees) || ((kind == RefKind::Movable || kind == RefKind::Item) && !config_.objects) ||
			                      ((kind == RefKind::Capture || physics) && !config_.buildings);
			auto* parent = ref ? ref->GetParentCell() : nullptr;
			const bool persistentCell = parent && parent == sky;
			const Cell* square = ref ? CapturedCellOf(ref) : nullptr;

			bool pending = false;
			std::string verdict;
			if (!ref) {
				if (!shapeReason)
					bucket = MissReason::NoOwner;
				verdict = "no owning reference found (not part of a placed object)";
			} else if (kind == RefKind::Actor) {
				bucket = MissReason::Character;
				verdict = "character (not traced)";
			} else if (byChoice && !shapeReason) {
				bucket = MissReason::ByChoice;
				verdict = "left to the shadow map by the Advanced settings";
			} else if (shapeReason) {
				verdict = std::format("shape rejected: {}", shapeReason);
			} else if (kind == RefKind::Other) {
				bucket = MissReason::Other;
				verdict = "a reference type that is not traced (markers, hazards, ...)";
			} else if (!square) {
				bucket = persistentCell ? MissReason::PersistentUnmatched : MissReason::NeverCaptured;
				verdict = persistentCell ? "persistent reference whose square has no captured cell" : "its cell is not captured";
			} else if (square->gathering || square->dirty || square->replacing ||
					   std::any_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == square->cell && a_cell->replaces; })) {
				pending = true;  // its cell is being (re)captured
			} else {
				// In a cell chunk already (its BLAS still building, or it moved out of its old bound)?
				// (45h) An alpha shape whose texture copy or BLAS is still pending counts too.
				// (45j) Looked up in an index instead of walking every captured shape.
				const bool inChunk = capturedGeometries_.contains(geometry);
				if (inChunk) {
					pending = true;
				} else if (objectKind) {
					if (const auto found = objectIndex_.find(ref); found != objectIndex_.end()) {
						const ObjectRef* entry = found->second;
						bool hasShape = false;
						if (entry)
							for (const auto& s : entry->shapes)
								hasShape = hasShape || s.geometry.get() == geometry;
						if (!entry || entry->root != ref->Get3D() || hasShape) {
							pending = true;  // re-validated next frame, or its BLAS is being built
						} else {
							bucket = MissReason::ShapeSkipped;
							auto* data = geometry->GetGeometryRuntimeData().rendererData;
							verdict = !data || !data->rawVertexData ? "the object was captured, this shape was not: no CPU vertex copy" :
							                                          "the object was captured, this shape was not (hidden then, VRAM budget or placement check)";
						}
					} else if (currentFrame_ - square->completedFrame < 600) {
						pending = true;  // the object scan has not reached it yet
					} else {
						bucket = MissReason::NeverCaptured;
						verdict = "the object scan never took the reference";
					}
				} else {
					const auto trackedIt = trackedRefs_.find(ref);
					const bool tracked = trackedIt != trackedRefs_.end();
					const bool trackedSameRoot = tracked && trackedIt->second.root == ref->Get3D();
					if (tracked && trackedSameRoot) {
						bucket = MissReason::ShapeSkipped;
						auto* data = geometry->GetGeometryRuntimeData().rendererData;
						verdict = !data || !data->rawVertexData ? "the reference was captured, this shape was not: no CPU vertex copy" :
						          (root != ref->Get3D())        ? "the reference was captured, this shape hangs under other 3D than the reference's" :
						                                          "the reference was captured, this shape was not (hidden or switched off then, or placement check)";
					} else if (tracked) {
						bucket = MissReason::RootMismatch;
						verdict = "the reference was captured with older 3D: recapture requested";
						requestRecapture(square);
					} else if (recaptureAsked_.insert(ref).second) {
						// Not in its cell's reference list when the cell was captured (added later):
						// one recapture of the cell; judged only if that does not take it either.
						requestRecapture(square);
						pending = true;
					} else {
						bucket = persistentCell ? MissReason::PersistentUnmatched : MissReason::NeverCaptured;
						verdict = "the reference was never captured, not even by a recapture of its cell";
					}
				}
			}

			if (pending) {
				RayTracingCasterFilter::ForgetMissed(geometry);
				++stats_.missedPending;
				continue;
			}
			missedJudged_.insert(geometry);
			if (bucket != MissReason::ByChoice)
				++stats_.missedConfirmed;
			++stats_.missedByReason[static_cast<int>(bucket)];
			if (shapeReason)
				++stats_.missedRejected;
			else
				++stats_.missedCapturable;

			// The log: every distinct reference + shape once per session, except characters and the
			// leaves of trees (expected), rate-limited through a short backlog.
			if (bucket == MissReason::Character || bucket == MissReason::ByChoice || bucket == MissReason::AlphaTested || (kind == RefKind::Tree && shapeReason) || stats_.missedDescribed >= kMissedLogCap)
				continue;
			const std::string_view shapeName = geometry->name.c_str() ? std::string_view(geometry->name.c_str()) : std::string_view();
			if (missedBacklog_.size() >= 64 || !missedLogged_.insert(MissKey(ref ? ref->GetFormID() : 0, shapeName)).second)
				continue;
			const auto& b = geometry->worldBound;
			auto* base = ref ? ref->GetBaseObject() : nullptr;
			const bool parentCaptured = parent && std::any_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == parent; });
			missedBacklog_.push_back(std::format(
				"[RayTracing] Missed caster [{}]: '{}' (radius {:.0f} at {:.0f}, {:.0f}, {:.0f}); {}; reference {:08X} base {:08X} ({}{}{}){}; parent cell {:08X} ({}); square {}",
				kMissReasonNames[static_cast<int>(bucket)], shapeName, b.radius, b.center.x, b.center.y, b.center.z, verdict, ref ? ref->GetFormID() : 0u,
				base ? base->GetFormID() : 0u, base ? RE::FormTypeToString(base->GetFormType()) : std::string_view("no base"),
				physics ? ", Havok-driven part" : "", ref && ref->IsPersistent() ? ", persistent" : "",
				ref && root && root != ref->Get3D() ? ", shape not under the reference's current 3D" : "", parent ? parent->GetFormID() : 0u,
				persistentCell ? "the worldspace's persistent cell" : (parentCaptured ? "captured" : "not captured"),
				square ? std::format("{},{} captured", square->x, square->y) : std::string("not captured")));
		}
		flushLog();
	}

	void Scene::EndFrame(RayTracingRT::Backend& a_backend)
	{
		const std::int64_t start = Qpc();
		for (auto& cell : retiring_)
			Release(a_backend, *cell);
		retiring_.clear();
		for (auto& object : retiringObjects_)
			ReleaseObject(a_backend, *object);
		retiringObjects_.clear();
		times_.endFrame = static_cast<float>(static_cast<double>(Qpc() - start) * QpcMs());
		DrainGraves(0.3);
	}

	bool Scene::GetLoadedRect(float o_rect[4], bool a_includeRetiring) const
	{
		bool any = false;
		std::int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
		auto add = [&](const std::vector<std::unique_ptr<Cell>>& a_list) {
			for (const auto& cell : a_list) {
				minX = std::min(minX, cell->x);
				minY = std::min(minY, cell->y);
				maxX = std::max(maxX, cell->x);
				maxY = std::max(maxY, cell->y);
				any = true;
			}
		};
		add(cells_);
		if (a_includeRetiring)
			add(retiring_);
		if (!any)
			return false;
		o_rect[0] = static_cast<float>(minX) * kCellUnits;
		o_rect[1] = static_cast<float>(minY) * kCellUnits;
		o_rect[2] = static_cast<float>(maxX + 1) * kCellUnits;
		o_rect[3] = static_cast<float>(maxY + 1) * kCellUnits;
		return true;
	}
}
