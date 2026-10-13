#include "Scene.h"

#include "CasterFilter.h"

#include <DirectXPackedVector.h>

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
	}

	const void* Scene::LandSignature(RE::TESObjectCELL* a_cell)
	{
		auto* land = a_cell->GetRuntimeData().cellLand;
		if (!land || !land->loadedData)
			return nullptr;
		// The quadrant meshes change identity whenever the land 3D is rebuilt.
		return land->loadedData->geom[0].get();
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
		stats_ = {};
		stats_.recaptures = recaptures;
		stats_.maxGatherMs = maxGather;
		stats_.placementLogged = logged;
		stats_.missedDescribed = missedDescribed;
		stats_.missedCapturable = missedCapturable;
		stats_.missedRejected = missedRejected;
		// A new area: its misses are counted afresh (the log limit above stays).
		RayTracingCasterFilter::ResetMissed();
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
		a_cell.triangles += mesh.triangleCount;
		if (a_landscape)
			++a_cell.landQuads;
		else
			++a_cell.shapes;

		if (chunk.Triangles() >= kChunkTriangles && gatherBackend_)
			CloseChunk(*gatherBackend_, a_cell);
		return true;
	}

	const char* Scene::ShapeRejectReason(RE::NiAVObject* a_object, bool a_allowTreeAnim, SkipCounts* o_counts)
	{
		SkipCounts ignored;
		auto& skipped = o_counts ? *o_counts : ignored;
		auto* geometry = a_object->AsGeometry();
		if (!geometry)
			return "not geometry";
		if (geometry->GetType().get() != RE::BSGeometry::Type::kTriShape) {
			++skipped.otherGeometry;
			return "not a plain BSTriShape";
		}
		const auto& runtime = geometry->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		if (!lighting) {
			++skipped.notLighting;
			return "not drawn with the Lighting shader";
		}
		using F = RE::BSShaderProperty::EShaderPropertyFlag;
		const auto& flags = lighting->flags;
		if (flags.any(F::kSkinned) || runtime.skinInstance) {
			++skipped.skinned;
			return "skinned";
		}
		if (flags.any(F::kDecal, F::kDynamicDecal)) {
			++skipped.decals;
			return "decal";
		}
		// Tree-animated (wind) shapes: only in trees / plants, whose opaque trunk and branch shapes
		// are separate from the alpha-tested leaf cards (those are rejected just below).
		if (flags.any(F::kTreeAnim) && !a_allowTreeAnim) {
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
		if (auto* alpha = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get())) {
			if (alpha->GetAlphaTesting()) {
				++skipped.alphaTested;
				return "alpha-tested";
			}
			if (alpha->GetAlphaBlending()) {
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

	RE::BSTriShape* Scene::CheckShape(RE::NiAVObject* a_object, bool a_allowTreeAnim)
	{
		if (!a_object->AsGeometry() || ShapeRejectReason(a_object, a_allowTreeAnim, &stats_.skipped))
			return nullptr;
		return a_object->AsGeometry()->AsTriShape();
	}

	void Scene::CaptureObject(Cell& a_cell, RE::NiAVObject* a_object, int a_depth)
	{
		if (!a_object || a_depth > 64)
			return;
		if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
			++stats_.skipped.hidden;
			return;
		}
		if (auto* node = a_object->AsNode()) {
			for (auto& child : node->GetChildren())
				CaptureObject(a_cell, child.get(), a_depth + 1);
			return;
		}
		if (auto* shape = CheckShape(a_object, false))
			AppendShape(a_cell, shape, false);
	}

	// ---------------------------------------------------------------------------------------------
	// (45e) Objects: doors / activators / movable statics, items, opaque parts of trees and plants.
	// ---------------------------------------------------------------------------------------------

	void Scene::CollectObjectShapes(RE::NiAVObject* a_object, bool a_allowTreeAnim, int a_depth, std::vector<RE::BSTriShape*>& o_shapes)
	{
		if (!a_object || a_depth > 64)
			return;
		if (a_object->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
			++stats_.skipped.hidden;
			return;
		}
		if (auto* node = a_object->AsNode()) {
			for (auto& child : node->GetChildren())
				CollectObjectShapes(child.get(), a_allowTreeAnim, a_depth + 1, o_shapes);
			return;
		}
		if (auto* shape = CheckShape(a_object, a_allowTreeAnim))
			o_shapes.push_back(shape);
	}

	void Scene::CaptureObjectRef(RayTracingRT::Backend& a_backend, const RE::NiPointer<RE::TESObjectREFR>& a_ref, RE::NiAVObject* a_root,
		ObjectKind a_kind, bool a_allowNewMeshes)
	{
		auto entry = std::make_unique<ObjectRef>();
		entry->ref = a_ref;
		entry->root = a_root;
		entry->kind = a_kind;

		static thread_local std::vector<RE::BSTriShape*> shapes;
		shapes.clear();
		CollectObjectShapes(a_root, a_kind == ObjectKind::Tree, 0, shapes);

		static thread_local RawMesh mesh;
		static thread_local std::vector<float> local;
		for (auto* shape : shapes) {
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
			entry->shapes.push_back(ObjectShape{ RE::NiPointer<RE::BSGeometry>(shape), key, it->second.blas, false });
		}

		++stats_.objects.refs[static_cast<int>(a_kind)];
		stats_.objects.shapes += static_cast<std::uint32_t>(entry->shapes.size());
		objectIndex_.insert(a_ref.get());
		objects_.push_back(std::move(entry));
	}

	void Scene::ReleaseObject(RayTracingRT::Backend& a_backend, ObjectRef& a_object)
	{
		for (auto& shape : a_object.shapes) {
			if (shape.registered)
				RayTracingCasterFilter::Remove(shape.geometry.get());
			shape.registered = false;
			const auto it = meshes_.find(shape.key);
			if (it != meshes_.end() && --it->second.users == 0) {
				a_backend.ReleaseBlas(it->second.blas);
				stats_.objects.meshTriangles -= std::min<std::uint64_t>(stats_.objects.meshTriangles, it->second.triangles);
				meshes_.erase(it);
			}
		}
		a_object.shapes.clear();
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
				const bool valid = inScene && !ref->IsDisabled() && !ref->IsDeleted() && ref->Get3D() == object->root;
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
		constexpr std::size_t kScanPerFrame = 512;
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
			const RefKind kind = Classify(ref);
			if (kind != RefKind::Movable && kind != RefKind::Item && kind != RefKind::Tree)
				continue;
			if (ref->IsDisabled() || ref->IsDeleted())
				continue;
			auto* root = ref->Get3D();
			if (!root || IsEditorMarker(root))
				continue;
			CaptureObjectRef(a_backend, refPtr, root, ToObjectKind(kind), a_allowNewMeshes);
			auto* parent = ref->GetParentCell();
			if (!parent || std::none_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == parent; }))
				++stats_.persistentObjects;
		}

		// 3. Registration with the caster filter once a shape's BLAS is built (it is then in this
		//    frame's TLAS and in every one until EndFrame releases it).
		stats_.objects.registeredShapes = 0;
		for (auto& object : objects_) {
			for (auto& shape : object->shapes) {
				if (!shape.registered && a_backend.IsBlasBuilt(shape.blas)) {
					RayTracingCasterFilter::Add(shape.geometry.get());
					shape.registered = true;
				}
				if (shape.registered)
					++stats_.objects.registeredShapes;
			}
		}
		stats_.objects.meshes = static_cast<std::uint32_t>(meshes_.size());
		stats_.objects.captured = static_cast<std::uint32_t>(objects_.size());
	}

	void Scene::AppendObjectInstances(const std::vector<std::unique_ptr<ObjectRef>>& a_objects, std::vector<RayTracingRT::Instance>& o_instances,
		bool a_expected) const
	{
		for (const auto& object : a_objects) {
			for (const auto& shape : object->shapes) {
				const auto* geometry = shape.geometry.get();
				// Not drawn by the game either: detached from the scene graph or hidden.
				if (!geometry->parent || geometry->GetFlags().any(RE::NiAVObject::Flag::kHidden))
					continue;
				// This frame's transform: doors swing, items fall, activators animate.
				const RE::NiTransform& world = geometry->world;
				RayTracingRT::Instance instance{ shape.blas, world.translate.x, world.translate.y, world.translate.z };
				for (int r = 0; r < 3; ++r)
					for (int c = 0; c < 3; ++c)
						instance.m[r][c] = world.rotate.entry[r][c] * world.scale;
				instance.category = object->kind == ObjectKind::Item ? RayTracingRT::kCategoryItem :
				                    (object->kind == ObjectKind::Tree ? RayTracingRT::kCategoryTree : RayTracingRT::kCategoryMovable);
				if (a_expected) {
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
			auto& refPtr = a_cell.pending[a_cell.cursor++];
			auto* ref = refPtr.get();
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
			if (disabled || !root) {
				++skipped.disabled;
				continue;
			}
			if (IsEditorMarker(root)) {
				++skipped.hidden;
				continue;
			}
			CaptureObject(a_cell, root, 0);
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
		currentFrame_ = a_frame;
		if (flushRequested_.exchange(false)) {
			ReleaseAll(a_backend);
		}

		auto* tes = RE::TES::GetSingleton();
		if (!tes || tes->interiorCell || !tes->gridCells || tes->gridCells->length == 0 || tes->gridCells->length > 63) {
			if (!cells_.empty())
				ReleaseAll(a_backend);
			stats_.lastGatherMs = 0.0f;
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

		// 6. (45e) Objects with their own instances (doors, items, trees): validation, scan, filter.
		UpdateObjects(a_backend, deadline, a_budgetMs > 0.0f);

		stats_.lastGatherMs = static_cast<float>(static_cast<double>(Qpc() - frameStart) * QpcMs());
		stats_.maxGatherMs = std::max(stats_.maxGatherMs, stats_.lastGatherMs);
	}

	void Scene::CollectInstances(std::vector<RayTracingRT::Instance>& o_instances, bool a_expected) const
	{
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
			}
		}
		AppendObjectInstances(objects_, o_instances, a_expected);
		AppendObjectInstances(retiringObjects_, o_instances, a_expected);
	}

	void Scene::CollectCellRefs(RE::TESObjectCELL* a_cell, std::int32_t a_x, std::int32_t a_y, std::vector<RE::NiPointer<RE::TESObjectREFR>>& o_refs,
		std::uint32_t* o_persistent) const
	{
		const std::size_t start = o_refs.size();
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
		auto* tes = RE::TES::GetSingleton();
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		auto* sky = worldSpace ? worldSpace->GetSkyCell() : nullptr;
		if (!sky || sky == a_cell)
			return;
		ankerl::unordered_dense::set<const RE::TESObjectREFR*> own;
		own.reserve(o_refs.size() - start);
		for (std::size_t i = start; i < o_refs.size(); ++i)
			own.insert(o_refs[i].get());
		const double minX = static_cast<double>(a_x) * kCellUnits, minY = static_cast<double>(a_y) * kCellUnits;
		auto& data = sky->GetRuntimeData();
		RE::BSSpinLockGuard lock(data.spinLock);
		for (const auto& ref : data.references) {
			if (!ref || own.contains(ref.get()))
				continue;
			const auto position = ref->GetPosition();
			const double px = position.x, py = position.y;
			if (px < minX || px >= minX + kCellUnits || py < minY || py >= minY + kCellUnits)
				continue;
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

	void Scene::DescribeMissedCasters()
	{
		auto missed = RayTracingCasterFilter::TakeMissed();
		if (missed.empty())
			return;
		auto* tes = RE::TES::GetSingleton();
		auto* worldSpace = tes ? tes->GetRuntimeData2().worldSpace : nullptr;
		auto* sky = worldSpace ? worldSpace->GetSkyCell() : nullptr;
		for (const auto& pointer : missed) {
			auto* geometry = pointer.get();
			if (!geometry)
				continue;
			// The reference that owns the shape: the nearest ancestor carrying it as user data.
			RE::TESObjectREFR* ref = nullptr;
			RE::NiAVObject* root = nullptr;
			int depth = 0;
			for (RE::NiAVObject* node = geometry; node && depth < 64; node = node->parent, ++depth) {
				if (auto* owner = node->GetUserData()) {
					ref = owner;
					root = node;
					break;
				}
			}
			const RefKind kind = ref ? Classify(ref) : RefKind::Other;
			const bool objectKind = kind == RefKind::Movable || kind == RefKind::Item || kind == RefKind::Tree;
			const char* reason = ShapeRejectReason(geometry, kind == RefKind::Tree, nullptr);
			if (reason)
				++stats_.missedRejected;
			else
				++stats_.missedCapturable;
			// The log is for the surprises: an acceptable shape the scene lacks. Rejected shapes
			// (leaves, grass, characters, ...) are expected and only a few of them are described.
			if (stats_.missedDescribed >= kMissedLogLimit || (reason && (missedRejectedLogged_ >= 4 || !ref)))
				continue;
			if (reason)
				++missedRejectedLogged_;
			++stats_.missedDescribed;
			const auto& b = geometry->worldBound;
			if (!ref) {
				logger::warn("[RayTracing] Missed caster: '{}' (radius {:.0f} at {:.0f}, {:.0f}, {:.0f}) is drawn into the sun shadow map inside the ray-traced "
							 "area but is not in the scene; shape: {}; no owning reference found (not part of a placed object)",
					geometry->name.c_str(), b.radius, b.center.x, b.center.y, b.center.z, reason ? reason : "accepted");
				continue;
			}
			auto* base = ref->GetBaseObject();
			auto* parent = ref->GetParentCell();
			const bool parentCaptured = parent && std::any_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == parent; });
			const Cell* square = CapturedCellOf(ref);
			bool tracked = false, trackedSameRoot = false;
			for (const auto* list : { &cells_, &retiring_ }) {
				for (const auto& cell : *list) {
					for (const auto& entry : cell->tracked) {
						if (entry.ref.get() == ref) {
							tracked = true;
							trackedSameRoot = trackedSameRoot || entry.root == ref->Get3D();
						}
					}
				}
			}
			const bool captured = std::any_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) {
				return std::any_of(a_cell->geometries.begin(), a_cell->geometries.end(), [&](const RE::NiPointer<RE::BSGeometry>& a_g) { return a_g.get() == geometry; });
			});
			const char* kindName = kind == RefKind::Capture ? "baked into cell chunks" :
			                       kind == RefKind::Actor   ? "character (not traced)" :
			                       objectKind               ? "own instance (object)" :
			                                                  "not a traced reference type";
			logger::warn("[RayTracing] Missed caster: '{}' (radius {:.0f} at {:.0f}, {:.0f}, {:.0f}) is drawn into the sun shadow map inside the ray-traced "
						 "area but is not in the scene; shape: {}; reference {:08X} base {:08X} ({}, {}){}{}, 3D root {}; parent cell {:08X} ({}); "
						 "square's cell {}; {}",
				geometry->name.c_str(), b.radius, b.center.x, b.center.y, b.center.z, reason ? reason : "accepted", ref->GetFormID(),
				base ? base->GetFormID() : 0u, base ? RE::FormTypeToString(base->GetFormType()) : std::string_view("no base"), kindName,
				ref->IsPersistent() ? ", persistent" : "", ref->IsDisabled() ? ", disabled" : "",
				ref->Get3D() == root ? "is the reference's 3D" : (ref->Get3D() ? "differs from the reference's 3D" : "but the reference has no 3D"),
				parent ? parent->GetFormID() : 0u, parent == sky && sky ? "the worldspace's persistent cell" : (parentCaptured ? "captured" : "not captured"),
				square ? std::format("{},{} captured{}", square->x, square->y, square->gathering ? " (still capturing)" : "") : std::string("not captured"),
				captured ? "the shape IS in a cell chunk (its BLAS may not be built yet)" :
				           (tracked ? (trackedSameRoot ? "the reference was captured, this shape was not" : "the reference was captured with older 3D") :
				                      (objectIndex_.contains(ref) ? "the reference is a captured object" : "the reference was never captured")));
		}
	}

	void Scene::EndFrame(RayTracingRT::Backend& a_backend)
	{
		for (auto& cell : retiring_)
			Release(a_backend, *cell);
		retiring_.clear();
		for (auto& object : retiringObjects_)
			ReleaseObject(a_backend, *object);
		retiringObjects_.clear();
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
