#include "Scene.h"

#include "CasterFilter.h"
#include "Wind.h"
#include "Utils/FrameTrace.h"

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
		// (46k) Behind the loading screen, once the player has arrived: a cell without a fully-loaded event is captured after it
		// has been attached for this many frames (a cell kept loaded from before fires no event).
		constexpr std::uint32_t kGateArrivedFrames = 20;

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

		/// (47g) NIF material alpha <= 1/255 (not the fade alpha): an invisible helper mesh. SEH-guarded.
		bool MaterialAlphaZero(RE::BSLightingShaderProperty* a_lighting)
		{
			__try {
				auto* material = static_cast<RE::BSLightingShaderMaterialBase*>(a_lighting->material);
				return material && material->materialAlpha <= 1.0f / 255.0f;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	bool Scene::IsWindowMask(RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return false;
		const auto& runtime = a_geometry->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		char path[512];
		if (!lighting || !ReadDiffusePath(lighting, path))
			return false;
		for (char* c = path; *c; ++c)
			*c = *c == '/' ? '\\' : static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
		const std::string_view text(path);
		return text.starts_with("textures\\masks\\") || text.find("\\textures\\masks\\") != std::string_view::npos || text.starts_with("masks\\");
	}

	bool Scene::IsTwoSided(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return true;
		const auto& runtime = const_cast<RE::BSGeometry*>(a_geometry)->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		return !lighting || lighting->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kTwoSided);
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
		a_cell.chunkTwoSided.clear();
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
		for (int side = 0; side < 2; ++side) {  // (46n)
			a_cell.current[side].reset();
			a_cell.currentGeometries[side].clear();
		}
		a_cell.stagePositions = {};  // (47u)
		a_cell.stageIndices = {};
		a_cell.staged = {};
	}

	std::uint64_t Scene::MortonKey(const float a_lo[3], const float a_hi[3])
	{
		// (47u) 2D Morton code of the box centre within the 4096-unit cell (x, y; 16 bits each over 8192 units: shapes may stick out), then
		// the centre's height as the low bits: neighbours in the cell end up next to each other in the order.
		auto spread = [](std::uint64_t a_v) {
			a_v &= 0xFFFFull;
			a_v = (a_v | (a_v << 8)) & 0x00FF00FFull;
			a_v = (a_v | (a_v << 4)) & 0x0F0F0F0Full;
			a_v = (a_v | (a_v << 2)) & 0x33333333ull;
			a_v = (a_v | (a_v << 1)) & 0x55555555ull;
			return a_v;
		};
		auto quantize = [](float a_value, float a_lo, float a_span) {
			const float t = std::clamp((a_value - a_lo) / a_span, 0.0f, 1.0f);
			return static_cast<std::uint64_t>(t * 65535.0f);
		};
		const float cx = 0.5f * (a_lo[0] + a_hi[0]), cy = 0.5f * (a_lo[1] + a_hi[1]), cz = 0.5f * (a_lo[2] + a_hi[2]);
		const std::uint64_t morton = spread(quantize(cx, -2048.0f, 8192.0f)) | (spread(quantize(cy, -2048.0f, 8192.0f)) << 1);
		return (morton << 16) | quantize(cz, -32768.0f, 65536.0f);
	}

	void Scene::FlushStaged(RayTracingRT::Backend& a_backend, Cell& a_cell)
	{
		// (47u) Spatial Static Chunks: the cell's shapes in Morton order of their box centres, filled into chunks of kChunkTriangles as
		// before (a chunk closes once it holds at least that many). One- and two-sided shapes stay apart indoors (the window beams' face
		// culling); outdoors they were staged on one side (no ray culls faces there).
		std::vector<std::pair<std::uint64_t, std::uint32_t>> order;
		order.reserve(a_cell.staged.size());
		for (std::uint32_t i = 0; i < a_cell.staged.size(); ++i) {
			const auto& shape = a_cell.staged[i];
			order.emplace_back((static_cast<std::uint64_t>(shape.side) << 63) | (MortonKey(shape.lo, shape.hi) >> 1), i);
		}
		std::sort(order.begin(), order.end());
		for (const auto& [key, i] : order) {
			const auto& shape = a_cell.staged[i];
			const int side = shape.side;
			if (!a_cell.current[side])
				a_cell.current[side] = std::make_unique<RayTracingRT::ChunkGeometry>();
			auto& chunk = *a_cell.current[side];
			const std::uint32_t base = chunk.Vertices();
			const auto* positions = a_cell.stagePositions.data() + static_cast<std::size_t>(shape.firstVertex) * 3;
			chunk.positions.insert(chunk.positions.end(), positions, positions + static_cast<std::size_t>(shape.vertices) * 3);
			// No per-shape reserve (an exact reserve per shape would copy the whole chunk again for every shape): geometric growth.
			for (std::uint32_t k = 0; k < shape.indices; ++k)
				chunk.indices.push_back(base + a_cell.stageIndices[shape.firstIndex + k]);
			a_cell.currentGeometries[side].push_back(shape.geometry);
			if (chunk.Triangles() >= kChunkTriangles)
				CloseChunk(a_backend, a_cell, side);
		}
		a_cell.stagePositions = {};
		a_cell.stageIndices = {};
		a_cell.staged = {};
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
		for (auto& [handle, instance] : windInstances_)  // (47n) every shape released above; anything left is a bookkeeping slip
			a_backend.ReleaseBlas(handle);
		windInstances_.clear();
		for (auto& [key, source] : windSources_)
			a_backend.ReleaseWindSource(source.source);
		windSources_.clear();
		RayTracingWind::SetActive(false);  // (47l) the next UpdateWind switches it on again
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
		if (!a_cell.cell || a_cell.cell->GetFormID() != a_cell.formID)
			return false;
		return a_cell.interior ? InteriorLoaded(a_cell.cell) : a_cell.cell->IsAttached();
	}

	bool Scene::TransformsSettled(RE::TESObjectCELL* a_cell)
	{
		// (46k) Behind the loading screen nothing of the new place has been drawn yet. A cell is captured there only when the
		// 3D of a sample of its static references sits where the references stand (the root's world translate is the
		// reference's position for a static placed in a cell); otherwise it waits (at the latest for the loading screen to close).
		std::vector<RE::NiPointer<RE::TESObjectREFR>> refs;
		{
			auto& data = a_cell->GetRuntimeData();
			RE::BSSpinLockGuard lock(data.spinLock);
			refs.reserve(std::min<std::size_t>(data.references.size(), 96));
			for (const auto& ref : data.references) {
				if (ref)
					refs.push_back(ref);
				if (refs.size() >= 96)
					break;
			}
		}
		std::uint32_t sampled = 0, off = 0;
		for (const auto& ref : refs) {
			if (sampled >= 16)
				break;
			auto* base = ref->GetBaseObject();
			if (!base || !base->Is(RE::FormType::Static) || ref->IsDisabled() || ref->IsDeleted())
				continue;
			auto* root = ref->Get3D();
			if (!root)
				continue;
			++sampled;
			const auto position = ref->GetPosition();
			const auto& at = root->world.translate;
			const float dx = at.x - position.x, dy = at.y - position.y, dz = at.z - position.z;
			const float d2 = dx * dx + dy * dy + dz * dz;
			if (!std::isfinite(d2) || d2 > 16.0f * 16.0f)
				++off;
		}
		return off * 4 <= sampled;  // at most a quarter off (nothing sampled: nothing static to capture yet either way)
	}

	bool Scene::Covers(float a_x, float a_y) const
	{
		for (const auto& cell : cells_) {
			if (cell->interior) {
				if (!cell->hasBounds)
					return true;
				constexpr double kMargin = 1024.0;
				if (a_x >= cell->boundsMin[0] - kMargin && a_x <= cell->boundsMax[0] + kMargin && a_y >= cell->boundsMin[1] - kMargin &&
					a_y <= cell->boundsMax[1] + kMargin)
					return true;
				continue;
			}
			const float x0 = static_cast<float>(cell->x) * kCellUnits, y0 = static_cast<float>(cell->y) * kCellUnits;
			if (a_x >= x0 && a_x < x0 + kCellUnits && a_y >= y0 && a_y < y0 + kCellUnits)
				return true;
		}
		return false;
	}

	bool Scene::InteriorLoaded(const RE::TESObjectCELL* a_cell)
	{
		// (46d) The interior the player stands in counts as loaded even if its state is not "attached" (yet).
		if (a_cell->IsAttached())
			return true;
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player && player->GetParentCell() == a_cell && player->Is3DLoaded();
	}

	void Scene::CloseChunk(RayTracingRT::Backend& a_backend, Cell& a_cell, int a_side)
	{
		// (46n) One side's chunk: one-sided shapes (a_side 0) and two-sided ones (1) never share a BLAS. 46g marked a whole chunk
		// two-sided (triangle-cull-disable) if any shape in it was, which switched the face culling of the window-beam rays off
		// for the one-sided glass pane Window Shadows Ultimate puts in front of its window mask: every beam was fully blocked.
		auto& current = a_cell.current[a_side];
		auto& shapes = a_cell.currentGeometries[a_side];
		if (!current || current->Triangles() == 0) {
			current.reset();
			shapes.clear();
			return;
		}
		const auto handle = a_backend.CreateBlas(std::move(current));
		if (handle != RayTracingRT::kInvalidBlas) {
			a_cell.blas.push_back(handle);
			a_cell.chunkGeometries.push_back(std::move(shapes));
			a_cell.chunkTwoSided.push_back(a_side == 1 ? 1 : 0);
		}
		shapes.clear();
		current.reset();
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
		// (47l) The normal (ubyte4, xyz UNORM) for the foliage sway's wind mesh.
		o_mesh.normalOffset = UINT32_MAX;
		if (flags & RE::BSGraphics::Vertex::VF_NORMAL) {
			const std::uint32_t offset = rendererData->vertexDesc.GetAttributeOffset(RE::BSGraphics::Vertex::VA_NORMAL);
			if (offset + 4 <= stride)
				o_mesh.normalOffset = offset;
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

		const int side = (!a_landscape && IsTwoSided(a_shape)) ? 1 : 0;  // (46n) one-sided and two-sided shapes in separate chunks
		if (a_cell.spatial) {
			// (47u) Spatial Static Chunks: staged; packed by place when the cell is complete (FlushStaged).
			Cell::StagedShape staged;
			staged.geometry = static_cast<std::uint32_t>(a_cell.geometries.size());
			staged.firstVertex = static_cast<std::uint32_t>(a_cell.stagePositions.size() / 3);
			staged.vertices = static_cast<std::uint32_t>(placed.size() / 3);
			staged.firstIndex = static_cast<std::uint32_t>(a_cell.stageIndices.size());
			staged.indices = static_cast<std::uint32_t>(mesh.indices.size());
			// Outdoors no ray culls faces (window beams are interior only): one- and two-sided shapes may share a chunk there.
			staged.side = a_cell.interior ? static_cast<std::uint8_t>(side) : 0u;
			for (int a = 0; a < 3; ++a) {
				staged.lo[a] = FLT_MAX;
				staged.hi[a] = -FLT_MAX;
			}
			for (std::size_t v = 0; v + 2 < placed.size(); v += 3)
				for (int a = 0; a < 3; ++a) {
					staged.lo[a] = std::min(staged.lo[a], placed[v + a]);
					staged.hi[a] = std::max(staged.hi[a], placed[v + a]);
				}
			a_cell.stagePositions.insert(a_cell.stagePositions.end(), placed.begin(), placed.end());
			a_cell.stageIndices.insert(a_cell.stageIndices.end(), mesh.indices.begin(), mesh.indices.end());
			a_cell.staged.push_back(staged);
			a_cell.geometries.emplace_back(a_shape);
			IndexGeometry(a_shape, true);
			a_cell.triangles += mesh.triangleCount;
			if (a_cell.interior)
				a_cell.AddBounds(a_shape->worldBound);
			if (a_landscape)
				++a_cell.landQuads;
			else
				++a_cell.shapes;
			return true;
		}
		if (!a_cell.current[side])
			a_cell.current[side] = std::make_unique<RayTracingRT::ChunkGeometry>();
		auto& chunk = *a_cell.current[side];
		const std::uint32_t base = chunk.Vertices();
		chunk.positions.insert(chunk.positions.end(), placed.begin(), placed.end());
		chunk.indices.reserve(chunk.indices.size() + mesh.indices.size());
		for (const auto index : mesh.indices)
			chunk.indices.push_back(base + index);

		a_cell.currentGeometries[side].push_back(static_cast<std::uint32_t>(a_cell.geometries.size()));
		a_cell.geometries.emplace_back(a_shape);
		IndexGeometry(a_shape, true);
		a_cell.triangles += mesh.triangleCount;
		if (a_cell.interior)
			a_cell.AddBounds(a_shape->worldBound);  // (46c)
		if (a_landscape)
			++a_cell.landQuads;
		else
			++a_cell.shapes;

		if (chunk.Triangles() >= kChunkTriangles && gatherBackend_)
			CloseChunk(*gatherBackend_, a_cell, side);
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

		// (47h) Blend-only shapes traced as cut-outs (Cut-outs setting): a material at least this opaque, and this alpha threshold.
		constexpr float kBlendCutoutMinMaterialAlpha = 254.0f / 255.0f;
		constexpr float kBlendCutoutThreshold = 0.5f;

		/// (47h) The diffuse texture keeps an alpha channel (a blend-only shape's shape is in it). BC1 counts as none.
		bool TextureHasAlpha(ID3D11Texture2D* a_texture)
		{
			__try {
				D3D11_TEXTURE2D_DESC desc{};
				a_texture->GetDesc(&desc);
				switch (desc.Format) {
				case DXGI_FORMAT_BC2_TYPELESS:
				case DXGI_FORMAT_BC2_UNORM:
				case DXGI_FORMAT_BC2_UNORM_SRGB:
				case DXGI_FORMAT_BC3_TYPELESS:
				case DXGI_FORMAT_BC3_UNORM:
				case DXGI_FORMAT_BC3_UNORM_SRGB:
				case DXGI_FORMAT_BC7_TYPELESS:
				case DXGI_FORMAT_BC7_UNORM:
				case DXGI_FORMAT_BC7_UNORM_SRGB:
				case DXGI_FORMAT_R8G8B8A8_TYPELESS:
				case DXGI_FORMAT_R8G8B8A8_UNORM:
				case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
				case DXGI_FORMAT_B8G8R8A8_TYPELESS:
				case DXGI_FORMAT_B8G8R8A8_UNORM:
				case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
					return true;
				default:
					return false;
				}
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}
	}

	bool Scene::GetAlphaSource(RE::BSGeometry* a_geometry, AlphaSource& o_source, bool a_allowBlend)
	{
		if (!a_geometry)
			return false;
		const auto& runtime = a_geometry->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		auto* alpha = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get());
		if (!lighting || !alpha)
			return false;
		// (47h) a_allowBlend: a blend-only shape (no alpha test) whose material is fully opaque and whose diffuse texture has an alpha
		// channel: that alpha is its shape (a trellis, a carved arch), traced as a cut-out at half alpha.
		const bool blendOnly = a_allowBlend && !alpha->GetAlphaTesting() && alpha->GetAlphaBlending();
		if (!alpha->GetAlphaTesting() && !blendOnly)
			return false;
		if (!blendOnly) {
			// The Lighting shader keeps a pixel when alpha >= its threshold: only the "greater" tests map to that.
			const auto test = static_cast<RE::NiAlphaProperty::TestFunction>((alpha->alphaFlags >> 10) & 7);
			if (test != RE::NiAlphaProperty::TestFunction::kGreater && test != RE::NiAlphaProperty::TestFunction::kGreaterEqual)
				return false;
		}
		auto* rendererData = runtime.rendererData;
		if (!rendererData)
			return false;
		const auto vertexFlags = rendererData->vertexDesc.GetFlags();
		if (!(vertexFlags & RE::BSGraphics::Vertex::VF_UV))
			return false;
		RawAlphaSource raw{};
		if (!ReadAlphaSource(lighting, raw))
			return false;
		if (blendOnly && (!(raw.materialAlpha >= kBlendCutoutMinMaterialAlpha) || !TextureHasAlpha(raw.texture)))
			return false;
		using F = RE::BSShaderProperty::EShaderPropertyFlag;
		o_source.texture = raw.texture;
		o_source.view = raw.view;
		o_source.material.threshold = blendOnly ? kBlendCutoutThreshold : static_cast<float>(alpha->alphaThreshold) / 255.0f;
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
		// (47g) Invisible helper meshes (material alpha 0) are not traced as solid geometry.
		if (MaterialAlphaZero(lighting)) {
			++skipped.otherGeometry;
			return "material alpha 0 (invisible helper)";
		}
		// (45h) Alpha-tested casters: a large enough alpha-tested shape whose cut-out can be traced
		// (diffuse texture, texcoords, a "greater" test) goes in as a non-opaque shape. Tree-animated
		// ones too (leaves; their wind sway is not followed). The other rules below still apply.
		// (45j) Vegetation (leaves, bushes) follows the Foliage setting, other cut-outs (fences,
		// banners) the Cut-outs setting; vegetation is never taken as an opaque occluder.
		bool anyHit = false;
		bool vegetation = false;
		bool blendTraced = false;  // (47h)
		auto* alphaTestProperty = netimmerse_cast<RE::NiAlphaProperty*>(runtime.properties[RE::BSGeometry::States::kProperty].get());
		if (alphaTestProperty && !alphaTestProperty->GetAlphaTesting() && alphaTestProperty->GetAlphaBlending() && (a_alphaKinds & kAlphaCutouts) &&
			!IsVegetation(geometry)) {
			// (47h) Blend-only with a fully opaque material and an alpha channel (a trellis arch): traced as a cut-out (Cut-outs).
			AlphaSource source;
			blendTraced = std::isfinite(geometry->worldBound.radius) && geometry->worldBound.radius >= kAlphaMinRadius && GetAlphaSource(geometry, source, true);
			anyHit = blendTraced;
		}
		if (alphaTestProperty && alphaTestProperty->GetAlphaTesting()) {
			vegetation = IsVegetation(geometry);
			if (o_foliage)
				*o_foliage = vegetation;
			// (46c) Interiors: Window Shadows Ultimate's window masks always with their cut-out (the window pattern).
			const bool windowMask = !vegetation && (a_alphaKinds & kAlphaMasks) && IsWindowMask(geometry);
			if ((a_alphaKinds & (vegetation ? kAlphaFoliage : kAlphaCutouts)) || windowMask) {
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
			} else if (alpha->GetAlphaBlending() && blendTraced) {
				++skipped.alphaBlendedTraced;  // (47h) traced as a cut-out, not a skip
				if (o_alpha)
					*o_alpha = true;
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
					if (a_cell.interior) {
						a_cell.AddBounds(shape->worldBound);  // (46c)
						if (IsWindowMask(shape))
							++stats_.windowMasks;
					}
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
		// (47h) true: a blend-only shape reaches here only through ShapeRejectReason under the Cut-outs setting.
		if (!GetAlphaSource(a_shape, source, true)) {
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
		MarkWindCandidate(o_shape, a_shape);  // (47l)
		return true;
	}

	void Scene::ReleaseShape(RayTracingRT::Backend& a_backend, ObjectShape& a_shape)
	{
		if (a_shape.registered)
			RayTracingCasterFilter::Remove(a_shape.geometry.get());
		a_shape.registered = false;
		// (47l) Its wind BLAS (before the frozen mesh: an alpha wind BLAS traces with the frozen mesh's texcoords).
		ReleaseWind(a_backend, a_shape);
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
		a_shape.shown = false;
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
		// (45h) An alpha shape only once its BLAS and texture are ready. (46g) Any shape only once handed off.
		if (!a_shape.shown || (a_shape.alpha && !a_shape.ready))
			return;
		// This frame's transform: doors swing, items fall, activators animate.
		const RE::NiTransform& world = geometry->world;
		// (47l) Foliage sway: its wind BLAS once built (same triangles in the same order: the alpha record stays valid), else the frozen one.
		const auto blas = a_shape.windReady && a_shape.windBlas != RayTracingRT::kInvalidBlas ? a_shape.windBlas : a_shape.blas;
		RayTracingRT::Instance instance{ blas, world.translate.x, world.translate.y, world.translate.z };
		for (int r = 0; r < 3; ++r)
			for (int c = 0; c < 3; ++c)
				instance.m[r][c] = world.rotate.entry[r][c] * world.scale;
		instance.category = a_shape.alpha ? RayTracingRT::kCategoryAlpha : a_category;
		instance.twoSided = a_shape.alpha || a_shape.twoSided;  // (46g)
		{
			// (47n) Moved since the last frame it was traced (a door swinging, an item falling): RTAO keeps a tight history where its
			// rays hit it. Compared with our own copy of its last transform (the engine's previousWorld is not refreshed for idle objects).
			const float now[12] = { world.translate.x, world.translate.y, world.translate.z, world.scale, world.rotate.entry[0][0], world.rotate.entry[0][1],
				world.rotate.entry[0][2], world.rotate.entry[1][0], world.rotate.entry[1][1], world.rotate.entry[1][2], world.rotate.entry[2][0], world.rotate.entry[2][2] };
			bool moved = false;
			if (a_shape.lastWorldValid) {
				for (int i = 0; i < 12 && !moved; ++i)
					moved = std::abs(now[i] - a_shape.lastWorld[i]) > (i < 3 ? 0.01f : 1e-4f);
			}
			std::copy_n(now, 12, a_shape.lastWorld);
			a_shape.lastWorldValid = true;
			instance.moving = moved;
		}
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
			objectShape.twoSided = IsTwoSided(shape);  // (46g)
			MarkWindCandidate(objectShape, shape);     // (47l) swaying branches of trees and plants
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
				if (shape.ready && !shape.shown && !hold_)
					shape.shown = true;  // (46g) else at HandOff
				// (45j) Foliage "Both": traced, and still drawn into the cascades (the darker of both wins).
				if (shape.shown && !shape.registered && !(shape.foliage && config_.foliage == FoliageMode::Both)) {
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
		gatherInterior_ = a_cell.interior;  // (46c) window masks with their cut-out (AlphaKinds)
		const std::int64_t start = Qpc();
		auto& skipped = stats_.skipped;

		if (!a_cell.snapshotTaken) {
			a_cell.snapshotTaken = true;
			a_cell.spatial = config_.spatialChunks;  // (47u) fixed for this capture
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

		if (a_cell.spatial)
			FlushStaged(a_backend, a_cell);  // (47u)
		CloseChunk(a_backend, a_cell, 0);
		CloseChunk(a_backend, a_cell, 1);  // (46n)
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
			// (46g) Behind a loading screen the place being left is still attached: not captured again.
			if (transition_) {
				departingPending_ = true;
				departingSkipped_ = 0;
			}
		}

		auto* tes = RE::TES::GetSingleton();
		// (46c) Interiors: the player's interior cell is the one "grid cell" (when Ray Tracing's point lights want it).
		auto* interior = tes ? tes->interiorCell : nullptr;
		// (46d) Behind a door's loading screen the player is already in the interior while TES still lists the old
		// exterior grid: the interior is taken from the player then (captured once loaded), the exterior is not
		// captured again for nothing (46c spent the loading screen re-capturing the street it was leaving).
		if (!interior) {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* parent = player ? player->GetParentCell() : nullptr;
			if (parent && parent->IsInteriorCell())
				interior = parent;
		}
		const bool interiorWanted = interior && interiorEnabled_;
		if (!tes || (interior && !interiorWanted) ||
			(!interior && (!tes->gridCells || tes->gridCells->length == 0 || tes->gridCells->length > 63))) {
			if (!cells_.empty() || !objects_.empty())
				ReleaseAll(a_backend);
			stats_.lastGatherMs = 0.0f;
			times_.update = 0.0f;
			return;
		}

		// 1. Grid diff: cells that left the grid (or are no longer the same attached cell) go now.
		std::vector<RE::TESObjectCELL*> attached;
		std::uint32_t notAttached = 0;  // (46k)
		if (interiorWanted) {
			if (interior->IsInteriorCell() && InteriorLoaded(interior))
				attached.push_back(interior);
			else if (interior->IsInteriorCell())
				++notAttached;
		} else {
			auto* grid = tes->gridCells;
			attached.reserve(static_cast<std::size_t>(grid->length) * grid->length);
			for (std::uint32_t gx = 0; gx < grid->length; ++gx) {
				for (std::uint32_t gy = 0; gy < grid->length; ++gy) {
					auto* cell = grid->GetCell(gx, gy);
					if (cell && cell->IsExteriorCell() && cell->GetCoordinates()) {
						if (cell->IsAttached())
							attached.push_back(cell);
						else
							++notAttached;
					}
				}
			}
		}
		if (departingPending_) {
			departingPending_ = false;
			departing_.assign(attached.begin(), attached.end());
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
				entry->interior = cell->interior;  // (46c)
				entry->seenFrame = a_frame - kSettleFrames;  // settled already: capture right away
				entry->replaces = true;
				replacements.push_back(std::move(entry));
			}
			for (auto& entry : replacements)
				cells_.push_back(std::move(entry));
		}

		// 3. New cells (captured once they have been in the grid for kSettleFrames).
		gateStats_.waiting = gateStats_.unsettled = 0;
		std::uint32_t untracked = 0;
		for (auto* cell : attached) {
			const bool known = std::any_of(cells_.begin(), cells_.end(), [&](const std::unique_ptr<Cell>& a_cell) { return a_cell->cell == cell; });
			if (known)
				continue;
			bool gateOpened = false;
			if (transition_ && loadGate_) {
				// (46k) Behind the loading screen (the loading pump): only cells that finished loading, and only once their 3D is
				// in place; the place being left never fires the event (it does not load again), so it is not captured again.
				const bool loaded = std::find(fullyLoaded_.begin(), fullyLoaded_.end(), cell) != fullyLoaded_.end();
				auto seen = std::find_if(gateSeen_.begin(), gateSeen_.end(), [&](const auto& a_entry) { return a_entry.first == cell; });
				if (seen == gateSeen_.end()) {
					gateSeen_.emplace_back(cell, a_frame);
					seen = std::prev(gateSeen_.end());
				}
				if (!loaded && !(arrived_ && a_frame - seen->second >= kGateArrivedFrames)) {
					++gateStats_.waiting;
					++untracked;
					continue;
				}
				if (!TransformsSettled(cell)) {
					++gateStats_.unsettled;
					++gateStats_.unsettledTotal;
					++untracked;
					continue;
				}
				++gateStats_.captured;
				if (loaded)
					++gateStats_.byEvent;
				gateOpened = true;
			} else if (transition_ && std::find(departing_.begin(), departing_.end(), cell) != departing_.end()) {
				++departingSkipped_;
				++untracked;
				continue;
			}
			auto entry = std::make_unique<Cell>();
			entry->cell = cell;
			entry->formID = cell->GetFormID();
			if (cell->IsInteriorCell()) {
				// (46c) Chunk origin: the 4096 square the player stands in (positions stay small floats).
				entry->interior = true;
				RE::NiPoint3 at{};
				if (auto* player = RE::PlayerCharacter::GetSingleton())
					at = player->GetPosition();
				entry->x = std::isfinite(at.x) ? static_cast<std::int32_t>(std::floor(at.x / kCellUnits)) : 0;
				entry->y = std::isfinite(at.y) ? static_cast<std::int32_t>(std::floor(at.y / kCellUnits)) : 0;
				// A shorter settle (an interior arrives behind a loading screen); references whose 3D comes later
				// make the cell dirty and it is captured again (as outdoors).
				entry->seenFrame = a_frame - kSettleFrames + 10;
			} else {
				entry->x = cell->GetCoordinates()->cellX;
				entry->y = cell->GetCoordinates()->cellY;
				entry->seenFrame = a_frame;
			}
			if (gateOpened)
				entry->seenFrame = a_frame - kSettleFrames;  // (46k) loaded and in place: captured right away
			cells_.push_back(std::move(entry));
		}
		stats_.cellsUntracked = untracked;
		stats_.cellsNotAttached = notAttached;

		const std::int64_t gatherStart = Qpc();
		times_.gridDiff = static_cast<float>(static_cast<double>(gatherStart - frameStart) * QpcMs());
		// 4. Capture, nearest cells first, within the budget.
		RE::NiPoint3 eye{};
		auto* eyePlayer = transition_ ? RE::PlayerCharacter::GetSingleton() : nullptr;
		if (eyePlayer)
			eye = eyePlayer->GetPosition();  // (46k) behind the loading screen the camera is still at the place left
		else if (auto* camera = RE::Main::WorldRootCamera())
			eye = camera->world.translate;
		const double qpcPerMs = 1.0 / QpcMs();
		const std::int64_t deadline = frameStart + static_cast<std::int64_t>(static_cast<double>(a_budgetMs) * qpcPerMs);
		for (;;) {
			Cell* nearest = nullptr;
			float best = FLT_MAX;
			for (auto& cell : cells_) {
				// (46g) While a transition holds the hand-off the destination is loaded already: a short settle.
				if (!cell->gathering || a_frame - cell->seenFrame < (hold_ ? 3u : kSettleFrames))
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
			if (built && !cell->shown && !hold_)
				cell->shown = true;  // (46g) else at HandOff
			if (built && cell->shown && !cell->filterRegistered) {
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
				if (shape.ready && !shape.shown && !hold_)
					shape.shown = true;  // (46g) else at HandOff
				if (shape.shown && !shape.registered && !(shape.foliage && config_.foliage == FoliageMode::Both)) {
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
		stats_.interior = false;
		stats_.interiorShapes = 0;
		stats_.interiorTriangles = 0;
		stats_.interiorGatherMs = 0.0f;
		for (auto& cell : cells_) {
			if (cell->replaces)
				continue;  // counted once its old capture is gone
			if (cell->interior) {
				// (46c)
				stats_.interior = true;
				stats_.interiorShapes += cell->shapes;
				stats_.interiorTriangles += cell->triangles;
				stats_.interiorGatherMs += static_cast<float>(cell->gatherMs);
				if (cell->hasBounds) {
					for (int a = 0; a < 3; ++a) {
						stats_.interiorBounds[a] = static_cast<float>(cell->boundsMin[a]);
						stats_.interiorBounds[3 + a] = static_cast<float>(cell->boundsMax[a]);
					}
				}
			}
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

		// 7. (47l) Foliage sway: the candidates' wind meshes (their own small CPU budget: the capture may have used all of its own).
		{
			const double windMs = (transition_ || hold_) ? 4.0 : 0.5;
			UpdateWind(a_backend, Qpc() + static_cast<std::int64_t>(windMs / std::max(QpcMs(), 1e-9)), a_budgetMs > 0.0f);
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
				// (46g) A cell's chunks only once the whole cell is handed off (never chunk by chunk).
				for (std::size_t i = 0; cell->shown && i < cell->blas.size(); ++i) {
					RayTracingRT::Instance instance{ cell->blas[i], x, y, 0.0 };
					instance.category = RayTracingRT::kCategoryCell;
					instance.twoSided = i >= cell->chunkTwoSided.size() || cell->chunkTwoSided[i] != 0;  // (46g)
					if (a_expected && i < cell->chunkGeometries.size()) {
						// (45f) The box of the live bound spheres of the shapes baked into this chunk.
						for (int a = 0; a < 3; ++a) {
							instance.expectMin[a] = DBL_MAX;
							instance.expectMax[a] = -DBL_MAX;
						}
						for (const std::uint32_t g : cell->chunkGeometries[i]) {
							if (g >= cell->geometries.size())
								continue;
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
		Util::FrameTrace::NoteJob(Util::FrameTrace::Job::RtPersistent);  // (47p)
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
		// (46c) An interior holds all of its references itself (the worldspace pointer may still name the last exterior).
		if (a_cell->IsInteriorCell())
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
				verdict = "left to the shadow map by Objects in Ray Tracing";
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

	bool Scene::GetInteriorBounds(double o_min[3], double o_max[3]) const
	{
		// (46c) The box of every interior capture traced this frame (a recapture and the one it replaces alike).
		constexpr double kMargin = 256.0;
		bool any = false;
		for (const auto* list : { &cells_, &retiring_ }) {
			for (const auto& cell : *list) {
				if (!cell->interior || !cell->hasBounds)
					continue;
				for (int a = 0; a < 3; ++a) {
					o_min[a] = any ? std::min(o_min[a], cell->boundsMin[a] - kMargin) : cell->boundsMin[a] - kMargin;
					o_max[a] = any ? std::max(o_max[a], cell->boundsMax[a] + kMargin) : cell->boundsMax[a] + kMargin;
				}
				any = true;
			}
		}
		return any;
	}

	bool Scene::GetInteriorRect(float o_rect[4]) const
	{
		for (const auto& cell : cells_) {
			if (!cell->interior)
				continue;
			double lo[3], hi[3];
			if (!GetInteriorBounds(lo, hi))
				return false;
			o_rect[0] = static_cast<float>(lo[0]);
			o_rect[1] = static_cast<float>(lo[1]);
			o_rect[2] = static_cast<float>(hi[0]);
			o_rect[3] = static_cast<float>(hi[1]);
			return std::isfinite(o_rect[0]) && std::isfinite(o_rect[1]) && std::isfinite(o_rect[2]) && std::isfinite(o_rect[3]) && o_rect[2] > o_rect[0] &&
			       o_rect[3] > o_rect[1];
		}
		return false;
	}

	bool Scene::GetLoadedRect(float o_rect[4], bool a_includeRetiring) const
	{
		// (46c) An interior: the box of what was captured (or, before anything is, a wide square around its origin).
		for (const auto& cell : cells_) {
			if (!cell->interior)
				continue;
			double lo[3], hi[3];
			if (GetInteriorBounds(lo, hi)) {
				o_rect[0] = static_cast<float>(lo[0]);
				o_rect[1] = static_cast<float>(lo[1]);
				o_rect[2] = static_cast<float>(hi[0]);
				o_rect[3] = static_cast<float>(hi[1]);
			} else {
				constexpr float kReach = 65536.0f;
				o_rect[0] = static_cast<float>(cell->x) * kCellUnits - kReach;
				o_rect[1] = static_cast<float>(cell->y) * kCellUnits - kReach;
				o_rect[2] = static_cast<float>(cell->x + 1) * kCellUnits + kReach;
				o_rect[3] = static_cast<float>(cell->y + 1) * kCellUnits + kReach;
			}
			return true;
		}
		bool any = false;
		std::int32_t minX = INT32_MAX, minY = INT32_MAX, maxX = INT32_MIN, maxY = INT32_MIN;
		auto add = [&](const std::vector<std::unique_ptr<Cell>>& a_list) {
			for (const auto& cell : a_list) {
				if (cell->interior)
					continue;  // (46c) an interior that is leaving
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

	Scene::HandOffCounts Scene::HeldCounts(const RayTracingRT::Backend& a_backend) const
	{
		HandOffCounts counts;  // shown = ready but held
		for (const auto& cell : cells_) {
			if (!cell->shown) {
				const bool built = !cell->gathering &&
				                   std::all_of(cell->blas.begin(), cell->blas.end(), [&](auto a_handle) { return a_backend.IsBlasBuilt(a_handle); });
				++(built ? counts.shown : counts.pending);
			}
			for (const auto& shape : cell->alphaShapes)
				if (!shape.shown)
					++(shape.ready ? counts.shown : counts.pending);
		}
		for (const auto& object : objects_)
			for (const auto& shape : object->shapes)
				if (!shape.shown)
					++(shape.ready ? counts.shown : counts.pending);
		return counts;
	}

	Scene::HandOffCounts Scene::HandOff(const RayTracingRT::Backend& a_backend)
	{
		// (46g) Every ready item at once; the caster filter takes them on the next Update (before that frame's trace).
		HandOffCounts counts;
		for (auto& cell : cells_) {
			if (!cell->shown) {
				const bool built = !cell->gathering &&
				                   std::all_of(cell->blas.begin(), cell->blas.end(), [&](auto a_handle) { return a_backend.IsBlasBuilt(a_handle); });
				if (built) {
					cell->shown = true;
					++counts.shown;
				} else {
					++counts.pending;
				}
			}
			for (auto& shape : cell->alphaShapes) {
				if (shape.shown)
					continue;
				if (shape.ready) {
					shape.shown = true;
					++counts.shown;
				} else {
					++counts.pending;
				}
			}
		}
		for (auto& object : objects_) {
			for (auto& shape : object->shapes) {
				if (shape.shown)
					continue;
				if (shape.ready) {
					shape.shown = true;
					++counts.shown;
				} else {
					++counts.pending;
				}
			}
		}
		return counts;
	}

	// ---------------------------------------------------------------------------------------------
	// (47l, 47n) Foliage sway: tree-animated shapes traced with the game's wind, each plant with its own
	// ---------------------------------------------------------------------------------------------

	void Scene::MarkWindCandidate(ObjectShape& a_shape, RE::BSTriShape* a_triShape)
	{
		a_shape.windCandidate = false;
		if (!a_triShape)
			return;
		const auto& runtime = a_triShape->GetGeometryRuntimeData();
		auto* lighting = netimmerse_cast<RE::BSLightingShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		if (!lighting || lighting->flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim) || !runtime.rendererData)
			return;
		// The Lighting vertex shader sways along the vertex normal by the vertex alpha: both must be in the mesh.
		const auto flags = runtime.rendererData->vertexDesc.GetFlags();
		if (!(flags & RE::BSGraphics::Vertex::VF_COLORS) || !(flags & RE::BSGraphics::Vertex::VF_NORMAL))
			return;
		a_shape.windCandidate = true;
	}

	void Scene::ReleaseWind(RayTracingRT::Backend& a_backend, ObjectShape& a_shape)
	{
		a_shape.windReady = false;
		if (a_shape.windBlas == RayTracingRT::kInvalidBlas)
			return;
		// Its BLAS and posed vertices go behind the fence (a TLAS in flight may still name it); its source when no shape uses it.
		a_backend.ReleaseBlas(a_shape.windBlas);
		windInstances_.erase(a_shape.windBlas);
		a_shape.windBlas = RayTracingRT::kInvalidBlas;
		const auto it = windSources_.find(a_shape.key);
		if (it != windSources_.end() && (it->second.users <= 1 || --it->second.users == 0)) {
			a_backend.ReleaseWindSource(it->second.source);
			windSources_.erase(it);
		}
	}

	bool Scene::AcquireWind(RayTracingRT::Backend& a_backend, ObjectShape& a_shape, bool a_allowNewSource)
	{
		auto it = windSources_.find(a_shape.key);
		if (it == windSources_.end()) {
			if (!a_allowNewSource)
				return false;
			const auto frozen = meshes_.find(a_shape.key);
			if (frozen == meshes_.end())
				return false;
			if (frozen->second.windAlpha < 0) {
				a_shape.windNone = true;  // known: no vertex alpha, nothing sways
				return false;
			}
			++windNewSourcesThisFrame_;  // attempts (the CPU of the read and copy is what the cap bounds)
			auto* shape = a_shape.geometry ? a_shape.geometry->AsTriShape() : nullptr;
			static thread_local RawMesh mesh;
			static thread_local std::vector<float> local;
			// The same read and placement as the frozen mesh (same positions, same triangles in the same order).
			if (!shape || !ReadMesh(shape, false, mesh) || mesh.colorOffset == UINT32_MAX || mesh.normalOffset == UINT32_MAX ||
				!PlaceVertices(shape, mesh.vertexBytes.data(), mesh.vertexCount, mesh.stride, mesh.positionOffset, mesh.fullPrecision, 0.0, 0.0, local, true) ||
				local.size() != static_cast<std::size_t>(mesh.vertexCount) * 3) {
				a_shape.windNone = true;
				return false;
			}
			auto geometry = std::make_unique<RayTracingRT::WindGeometry>();
			geometry->vertices.resize(mesh.vertexCount);
			bool anyAlpha = false;
			for (std::uint32_t v = 0; v < mesh.vertexCount; ++v) {
				const std::uint8_t* vertex = mesh.vertexBytes.data() + static_cast<std::size_t>(v) * mesh.stride;
				auto& out = geometry->vertices[v];
				out.position[0] = local[static_cast<std::size_t>(v) * 3];
				out.position[1] = local[static_cast<std::size_t>(v) * 3 + 1];
				out.position[2] = local[static_cast<std::size_t>(v) * 3 + 2];
				std::uint32_t normal;
				std::memcpy(&normal, vertex + mesh.normalOffset, sizeof(normal));
				const std::uint8_t alpha = vertex[mesh.colorOffset + 3];
				out.normalAlpha = (normal & 0x00FFFFFFu) | (static_cast<std::uint32_t>(alpha) << 24);
				anyAlpha = anyAlpha || alpha != 0;
			}
			if (!anyAlpha) {
				frozen->second.windAlpha = -1;  // every copy of this mesh stays frozen
				a_shape.windNone = true;
				return false;
			}
			frozen->second.windAlpha = 1;
			geometry->indices = mesh.indices;
			geometry->opaque = a_shape.key.kind == 0;
			const std::uint32_t source = a_backend.CreateWindSource(std::move(geometry));
			if (source == UINT32_MAX) {
				a_shape.windNone = true;  // refused (size, VRAM budget): stays frozen until it is captured again
				return false;
			}
			it = windSources_.emplace(a_shape.key, WindSourceRef{ source, 0 }).first;
			++windStats_.newSources;
		}
		// Its own BLAS, first built at the mean of its sway now.
		const auto globals = RayTracingWind::FrameGlobals(currentFrame_);
		RE::NiNode* node = RayTracingWind::LeafNodeOf(a_shape.geometry.get());
		RayTracingWind::Pose pose;
		const float amplitude = RayTracingWind::Evaluate(node, globals, pose) ? pose.amplitude : 0.0f;
		const std::uint64_t overBudgetBefore = a_backend.GetWindStats().overBudget;
		const auto handle = a_backend.CreateWindBlas(it->second.source, amplitude);
		if (handle == RayTracingRT::kInvalidBlas) {
			if (it->second.users == 0) {
				a_backend.ReleaseWindSource(it->second.source);
				windSources_.erase(it);
			}
			if (a_backend.GetWindStats().overBudget != overBudgetBefore)
				windPauseUntil_ = currentFrame_ + kWindBudgetPauseFrames;  // the VRAM budget: no new wind BLAS for a while (it stays frozen)
			else
				a_shape.windNone = true;  // refused for good (bad data): stays frozen until it is captured again
			return false;
		}
		++it->second.users;
		++windNewInstancesThisFrame_;
		++windStats_.newInstances;
		a_shape.windBlas = handle;
		windInstances_[handle] = WindInstance{ a_shape.geometry, RE::NiPointer<RE::NiNode>(node), UINT32_MAX };
		return true;
	}

	void Scene::VisitWind(RayTracingRT::Backend& a_backend, ObjectShape& a_shape, std::int64_t a_deadline, bool a_allowNew, const double a_camera[3], float a_radius)
	{
		if (!a_shape.windCandidate || !a_shape.geometry)
			return;
		if (a_shape.windNone) {
			++windStats_.still;
			return;
		}
		++windStats_.candidates;
		// Distance from the camera to the shape's bound (the engine fades the sway out with its node's distance; beyond the fade end
		// the plant does not move and the frozen BLAS is exact).
		const auto& bound = a_shape.geometry->worldBound;
		const double dx = bound.center.x - a_camera[0], dy = bound.center.y - a_camera[1], dz = bound.center.z - a_camera[2];
		const double distance = std::sqrt(dx * dx + dy * dy + dz * dz) - static_cast<double>(std::isfinite(bound.radius) ? std::max(bound.radius, 0.0f) : 0.0f);
		if (a_shape.windBlas != RayTracingRT::kInvalidBlas) {
			if (distance > static_cast<double>(a_radius) * kWindReleaseScale + kWindRadiusMargin) {
				// Left the radius: back to the frozen BLAS (it does not move out there).
				ReleaseWind(a_backend, a_shape);
				++windStats_.released;
				return;
			}
		} else {
			if (distance > a_radius)
				return;
			const bool transition = transition_ || hold_;
			const std::uint32_t sourceCap = transition ? kWindNewSourcesTransition : kWindNewSourcesPerFrame;
			const std::uint32_t instanceCap = transition ? kWindNewInstancesTransition : kWindNewInstancesPerFrame;
			if (a_allowNew && windNewInstancesThisFrame_ < instanceCap && static_cast<std::int32_t>(currentFrame_ - windPauseUntil_) >= 0 && Qpc() < a_deadline)
				AcquireWind(a_backend, a_shape, windNewSourcesThisFrame_ < sourceCap);
		}
		++windStats_.inRadius;
		a_shape.windReady = a_shape.windBlas != RayTracingRT::kInvalidBlas && a_shape.ready && a_backend.IsBlasBuilt(a_shape.windBlas);
		if (a_shape.windReady)
			++windStats_.swaying;
		else if (!a_shape.windNone)
			++windStats_.waiting;
	}

	void Scene::UpdateWind(RayTracingRT::Backend& a_backend, std::int64_t a_deadline, bool a_allowNew)
	{
		windNewSourcesThisFrame_ = windNewInstancesThisFrame_ = 0;
		windStats_.candidates = windStats_.inRadius = windStats_.swaying = windStats_.waiting = windStats_.still = 0;
		windStats_.newInstances = windStats_.newSources = windStats_.released = 0;
		const bool on = SwayActive() && a_backend.WindAvailable() && RayTracingWind::Available() && RayTracingWind::TreeAnimsEnabled();
		if (!on) {
			if (windWasOn_) {
				// Every shape back to its frozen BLAS (released behind the fence; retiring captures too: traced frozen this frame).
				auto reset = [&](ObjectShape& a_shape) {
					ReleaseWind(a_backend, a_shape);
					a_shape.windNone = false;
				};
				for (const auto* list : { &cells_, &retiring_ })
					for (const auto& cell : *list)
						for (auto& shape : cell->alphaShapes)
							reset(shape);
				for (const auto* list : { &objects_, &retiringObjects_ })
					for (const auto& object : *list)
						for (auto& shape : object->shapes)
							reset(shape);
				for (auto& [handle, instance] : windInstances_)  // every user released above; anything left is a bookkeeping slip
					a_backend.ReleaseBlas(handle);
				windInstances_.clear();
				for (auto& [key, source] : windSources_)
					a_backend.ReleaseWindSource(source.source);
				windSources_.clear();
				windWasOn_ = false;
			}
			RayTracingWind::SetActive(false);
			windStats_.sources = windStats_.instances = 0;
			windStats_.radius = 0.0f;
			return;
		}
		windWasOn_ = true;
		RayTracingWind::SetActive(true);
		// The radius: the engine's own fade end (beyond it every plant stands still: the frozen BLAS is exact), at most kWindMaxRadius.
		const auto globals = RayTracingWind::FrameGlobals(currentFrame_);
		float radius = kWindMaxRadius;
		if (globals.valid && globals.fadeEnd > 0.0f && globals.fadeEnd > globals.fadeStart)
			radius = std::min(globals.fadeEnd + kWindRadiusMargin, kWindMaxRadius);
		windStats_.radius = radius;
		double camera[3]{};
		if (auto* worldCamera = RE::Main::WorldRootCamera()) {
			camera[0] = worldCamera->world.translate.x;
			camera[1] = worldCamera->world.translate.y;
			camera[2] = worldCamera->world.translate.z;
		}
		for (const auto& cell : cells_)
			for (auto& shape : cell->alphaShapes)
				VisitWind(a_backend, shape, a_deadline, a_allowNew, camera, radius);
		for (const auto& object : objects_)
			for (auto& shape : object->shapes)
				VisitWind(a_backend, shape, a_deadline, a_allowNew, camera, radius);
		windStats_.sources = static_cast<std::uint32_t>(windSources_.size());
		windStats_.instances = static_cast<std::uint32_t>(windInstances_.size());
	}

	void Scene::ChunkOverlapStats(const RayTracingRT::Backend& a_backend, std::uint32_t& o_cells, float& o_chunksPerCell, float& o_overlap) const
	{
		// (47q) A statistic for a later re-chunking decision (analysis-rt-ada-efficiency #5): nothing is changed.
		o_cells = 0;
		o_chunksPerCell = 0.0f;
		o_overlap = 0.0f;
		std::uint64_t chunks = 0;
		double summed = 0.0, intersections = 0.0;
		struct Box
		{
			float lo[3], hi[3];
		};
		std::vector<Box> boxes;
		for (const auto& cell : cells_) {
			if (cell->interior || cell->blas.empty())
				continue;
			++o_cells;
			boxes.clear();
			for (const auto handle : cell->blas) {
				Box b;
				if (!a_backend.GetBlasBounds(handle, b.lo, b.hi))
					continue;
				boxes.push_back(b);
			}
			chunks += boxes.size();
			auto volume = [](const float a_lo[3], const float a_hi[3]) {
				double v = 1.0;
				for (int a = 0; a < 3; ++a)
					v *= std::max(0.0, static_cast<double>(a_hi[a]) - static_cast<double>(a_lo[a]));
				return v;
			};
			for (std::size_t i = 0; i < boxes.size(); ++i) {
				summed += volume(boxes[i].lo, boxes[i].hi);
				for (std::size_t k = i + 1; k < boxes.size(); ++k) {
					float lo[3], hi[3];
					for (int a = 0; a < 3; ++a) {
						lo[a] = std::max(boxes[i].lo[a], boxes[k].lo[a]);
						hi[a] = std::min(boxes[i].hi[a], boxes[k].hi[a]);
					}
					intersections += volume(lo, hi);
				}
			}
		}
		if (o_cells > 0)
			o_chunksPerCell = static_cast<float>(static_cast<double>(chunks) / static_cast<double>(o_cells));
		if (summed > 0.0)
			o_overlap = static_cast<float>(intersections / summed);
	}

	std::uint32_t Scene::MarkLateWind(RayTracingRT::Backend& a_backend, const RayTracingRelevance::Region* a_exact)
	{
		// (47u) Every plant whose refit this frame was skipped (culled, not at this frame's pose): reachable by this frame's rays -> late.
		std::uint32_t late = 0;
		for (auto& [handle, instance] : windInstances_) {
			if (!a_backend.WindCulledStale(handle))
				continue;
			bool reachable = true;
			if (a_exact && instance.geometry) {
				float lo[3], hi[3];
				if (a_backend.GetWindReachBox(handle, lo, hi)) {
					const RE::NiTransform& world = instance.geometry->world;
					float m[3][3];
					for (int r = 0; r < 3; ++r)
						for (int c = 0; c < 3; ++c)
							m[r][c] = world.rotate.entry[r][c] * world.scale;
					const double t[3] = { world.translate.x, world.translate.y, world.translate.z };
					double wlo[3], whi[3];
					RayTracingRelevance::TransformBox(m, t, lo, hi, wlo, whi);
					reachable = a_exact->Meets(wlo, whi);
				}
			}
			if (reachable) {
				a_backend.SetWindLate(handle);
				instance.outsideFrames = 0;  // relevant again: not culled before it has been out for a while
				++late;
			}
		}
		return late;
	}

	void Scene::UpdateWindPoses(RayTracingRT::Backend& a_backend, std::uint32_t a_frame, bool a_keepMoving, float a_dt, bool a_learnAnyFrame,
		const RayTracingRelevance::Region* a_relevance)
	{
		if (windInstances_.empty()) {
			windClocks_.clear();
			return;
		}
		if (a_frame != windPoseStatsFrame_) {
			windPoseStatsFrame_ = a_frame;
			windCulled_ = 0;  // (47u)
			windStats_.poses = windStats_.poseFaults = 0;
			windStats_.catchups = windStats_.frozenUnlearned = 0;
			windStats_.catchupsShared = 0;  // (47r)
			// Nodes not visited for a while (their plants left the radius) are forgotten.
			if (windClocks_.size() > windInstances_.size() * 2 + 256) {
				for (auto it = windClocks_.begin(); it != windClocks_.end();) {
					if (a_frame - it->second.frame > 600u)
						it = windClocks_.erase(it);
					else
						++it;
				}
			}
		}
		// (47q) Keep Leaves Moving on Long Frames. The game advances a leaf node's timer before the frame's Early Prep, except on a frame of
		// 35 ms or more (then it stays as it was: leaves, their shadow maps and the RT shadow freeze). Per node (once per frame): an advance
		// since our last visit is the game's own (our writes are in `last`): its rate per second of game time is learned there (frames
		// under kFreezeDt only); a timer still at `last` on a frame of at least kFreezeDt is advanced by rate x dt here, before the draws.
		constexpr float kFreezeDt = 0.0345f;
		constexpr float kLearnMinDt = 0.001f;
		const bool longFrame = a_keepMoving && a_dt >= kFreezeDt && a_dt < 1.0f;
		// (47r) "Learn on Any Frame": every advance the game makes is learned (whatever the frame time: at a steady 35-37 ms the game
		// advances only on the odd shorter frame, and 47q learned only below 34.5 ms, so those plants never learned). Off = 47q.
		const bool learnFrame = a_learnAnyFrame ? (a_dt >= kLearnMinDt && a_dt < 1.0f) : (a_dt >= kLearnMinDt && a_dt < kFreezeDt);
		// (47r) The shared rate (from last frame's learned plants) for plants without their own: 1 = k x the node's frequency, 2 one rate.
		const int sharedMode = a_learnAnyFrame ? windSharedMode_ : 0;
		const float sharedValue = windSharedValue_;
		windStats_.sharedMode = sharedMode;
		windStats_.sharedValue = sharedMode != 0 ? sharedValue : 0.0f;
		const auto globals = RayTracingWind::FrameGlobals(a_frame);
		for (auto& [handle, instance] : windInstances_) {
			if (instance.poseFrame == a_frame)
				continue;
			instance.poseFrame = a_frame;
			// The node the draw uses this frame (re-read: the shape's fade node may change; the one in use is held).
			RE::NiNode* node = RayTracingWind::LeafNodeOf(instance.geometry.get());
			if (node != instance.node.get())
				instance.node.reset(node);
			float timer;
			if (node && RayTracingWind::ReadTimer(node, timer)) {
				auto& clock = windClocks_[node];
				if (clock.frame != a_frame) {
					const bool consecutive = clock.frame != UINT32_MAX && clock.frame + 1 == a_frame;
					if (consecutive && timer != clock.last) {
						const float rate = (timer - clock.last) / std::max(a_dt, kLearnMinDt);
						const bool plausible = std::isfinite(rate) && rate > 0.0f && rate < 1000.0f &&
						                       (clock.learned < 3 || (rate > clock.rate * 0.5f && rate < clock.rate * 2.0f));
						if (learnFrame && plausible) {
							clock.rate = clock.learned == 0 ? rate : clock.rate * 0.8f + rate * 0.2f;
							clock.learned = std::min(clock.learned + 1, 1000u);
							if (a_learnAnyFrame) {
								float frequency = 0.0f;
								clock.frequency = RayTracingWind::ReadFrequency(node, frequency) ? frequency : 0.0f;
							}
						}
					} else if (consecutive && longFrame) {
						float useRate = clock.learned > 0 && clock.rate > 0.0f ? clock.rate : 0.0f;
						bool shared = false;
						if (useRate <= 0.0f && sharedMode != 0) {
							// (47r) No rate of its own yet: the one the other plants share (only while they agree; see the end).
							float frequency = 0.0f;
							if (sharedMode == 2)
								useRate = sharedValue;
							else if (RayTracingWind::ReadFrequency(node, frequency) && frequency > 0.0f)
								useRate = sharedValue * frequency;
							shared = useRate > 0.0f && std::isfinite(useRate) && useRate < 1000.0f;
							if (!shared)
								useRate = 0.0f;
						}
						if (useRate > 0.0f) {
							const float advanced = timer + useRate * a_dt;
							if (RayTracingWind::WriteTimer(node, advanced)) {
								timer = advanced;
								++windStats_.catchups;
								if (shared)
									++windStats_.catchupsShared;
							}
						} else {
							++windStats_.frozenUnlearned;
						}
					}
					clock.last = timer;
					clock.frame = a_frame;
				}
			}
			RayTracingWind::Pose pose;
			if (!RayTracingWind::Evaluate(node, globals, pose)) {
				++windStats_.poseFaults;
				instance.outsideFrames = 0;
				a_backend.SetWindCull(handle, false);  // (47u) never culled on a frame without its pose
				continue;  // the last pose stays
			}
			a_backend.SetWindPose(handle, pose.magnitude, pose.amplitude, pose.frequency, pose.timer);
			RayTracingWind::NoteEarly(node, pose, a_frame);
			++windStats_.poses;
			// (47u) Skip Updates That Cannot Affect the Image: outside the early region (last frame's rays, widened) for kCullAfterFrames
			// frames in a row -> no refit this frame (checked again against this frame's exact rays before the trace: MarkLateWind).
			bool cull = false;
			if (a_relevance && a_relevance->Valid() && instance.geometry) {
				bool reachable = true;
				float lo[3], hi[3];
				if (a_backend.GetWindReachBox(handle, lo, hi)) {
					const RE::NiTransform& world = instance.geometry->world;
					float m[3][3];
					for (int r = 0; r < 3; ++r)
						for (int c = 0; c < 3; ++c)
							m[r][c] = world.rotate.entry[r][c] * world.scale;
					const double t[3] = { world.translate.x, world.translate.y, world.translate.z };
					double wlo[3], whi[3];
					RayTracingRelevance::TransformBox(m, t, lo, hi, wlo, whi);
					reachable = a_relevance->Meets(wlo, whi);
				}
				instance.outsideFrames = reachable ? 0u : std::min(instance.outsideFrames + 1u, 1000000u);
				cull = instance.outsideFrames >= kCullAfterFrames;
			} else {
				instance.outsideFrames = 0;
			}
			a_backend.SetWindCull(handle, cull);
			windCulled_ += cull ? 1u : 0u;
		}
		if (a_learnAnyFrame)
			UpdateWindSharedRate();
	}

	void Scene::UpdateWindSharedRate()
	{
		// (47r) The shared rate for plants that never learned their own, from the plants that did (3 learns or more): k = rate / frequency
		// when those agree within 10 % (interquartile range over the median), else one rate for all when the rates agree, else none (such
		// plants stay frozen on long frames, as in 47q). At least 4 plants. Robust: medians and quartiles, not means.
		windShareK_.clear();
		windShareRate_.clear();
		for (const auto& [node, clock] : windClocks_) {
			if (clock.learned < 3 || !(clock.rate > 0.0f))
				continue;
			windShareRate_.push_back(clock.rate);
			if (clock.frequency > 0.0f)
				windShareK_.push_back(clock.rate / clock.frequency);
		}
		constexpr std::size_t kMinPlants = 4;
		constexpr float kAgree = 0.1f;
		auto spread = [](std::vector<float>& a_v, float& o_median) {
			std::sort(a_v.begin(), a_v.end());
			const std::size_t n = a_v.size();
			o_median = a_v[n / 2];
			const float q1 = a_v[n / 4], q3 = a_v[(3 * n) / 4];
			return o_median > 0.0f ? (q3 - q1) / o_median : 1e30f;
		};
		int mode = 0;
		float value = 0.0f;
		float kMedian = 0.0f, rMedian = 0.0f;
		const float kSpread = windShareK_.size() >= kMinPlants ? spread(windShareK_, kMedian) : 1e30f;
		const float rSpread = windShareRate_.size() >= kMinPlants ? spread(windShareRate_, rMedian) : 1e30f;
		if (kSpread <= kAgree && kSpread <= rSpread) {
			mode = 1;
			value = kMedian;
		} else if (rSpread <= kAgree) {
			mode = 2;
			value = rMedian;
		}
		windSharedMode_ = mode;
		windSharedValue_ = value;
	}
}