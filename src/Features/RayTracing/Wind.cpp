#include "Wind.h"

#include "Globals.h"
#include "State.h"

#include <atomic>
#include <mutex>

namespace RayTracingWind
{
	namespace
	{
		constexpr std::uint32_t kSpotCheckFrames = 900;   // a sampled shape is read again after this many frames (when drawn)
		constexpr std::uint32_t kRetryFrames = 60;        // a refused read is tried again after this many frames
		constexpr std::uint32_t kFallbackMaxAge = 120;    // the fallback's last sample is used for at most this many frames
		constexpr std::uint32_t kLogMeshes = 48;          // diagnostic lines per session: first samples of a mesh
		constexpr std::uint32_t kLogCopies = 48;          // ... second copies of a mesh
		constexpr std::size_t kMaxMeshDiag = 8192;        // meshes remembered for the copy comparison
		constexpr std::uint64_t kDistrustMismatches = 4;  // BSTreeManager is not used once this many samples disagreed (and most did)

		struct Entry
		{
			Params params;
			float predicted[2]{};
			bool hasPrediction = false;
			std::uint32_t failedFrame = UINT32_MAX;
			std::uint32_t refs = 0;  // captures holding the shape (a recaptured cell's old and new capture both, for a moment)
		};

		struct MeshDiag
		{
			const RE::BSGeometry* first = nullptr;
			float amplitude = 0.0f, frequency = 0.0f, distance = 0.0f;
			bool copySeen = false;
		};

		struct Manager
		{
			float magnitude;
			float timer;
		};

		std::mutex g_mutex;
		std::atomic<bool> g_active{ false };
		ankerl::unordered_dense::map<const RE::BSGeometry*, Entry> g_requests;
		ankerl::unordered_dense::map<const void*, MeshDiag> g_meshes;
		Stats g_stats{};
		std::uint32_t g_loggedMeshes = 0, g_loggedCopies = 0;  // whole session
		// The fallback: the last per-frame sample of any tree-animated draw (taken only while BSTreeManager is not used).
		bool g_haveGlobal = false;
		float g_globalTree[4]{};
		float g_globalTimers[2]{};
		std::uint32_t g_globalFrame = 0;
		std::uint32_t g_globalTriedFrame = UINT32_MAX;
		std::uint32_t g_computedFrame = UINT32_MAX;
		FrameWind g_frameWind{};

		/// BSTreeManager's wind magnitude and tree timer (SEH: a missing singleton is not a crash).
		bool ReadManager(Manager& o_manager)
		{
			__try {
				auto* manager = RE::BSTreeManager::GetSingleton();
				if (!manager)
					return false;
				o_manager.magnitude = manager->windMagnitude;
				o_manager.timer = manager->treeUpdateTimer;
				return std::isfinite(o_manager.magnitude) && std::isfinite(o_manager.timer);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// TreeParams / WindTimers of the current vertex shader's per-geometry constants (the Lighting VS constant table entries 4
		/// and 5, ShaderCache's lightingVS map), as BSLightingShader::SetupGeometry just wrote them. SEH: the group was unmapped
		/// by then (its pointer is the dynamic buffer's mapping); a fault is a refused read, never a crash.
		bool ReadConstants(float o_tree[4], float o_timers[2])
		{
			__try {
				auto** current = globals::game::currentVertexShader;
				auto* shader = current ? *current : nullptr;
				if (!shader)
					return false;
				const std::int8_t treeOffset = shader->constantTable[4];
				const std::int8_t timerOffset = shader->constantTable[5];
				// 0 = not in the shader (offset 0 is World). An int8 offset (< 128 floats) stays inside the per-geometry buffer.
				if (treeOffset <= 0 || timerOffset <= 0)
					return false;
				const auto& group = shader->constantBuffers[2];  // PerGeometry
				if (!group.buffer || !group.data)
					return false;
				const float* data = static_cast<const float*>(group.data);
				for (int i = 0; i < 4; ++i)
					o_tree[i] = data[treeOffset + i];
				o_timers[0] = data[timerOffset];
				o_timers[1] = data[timerOffset + 1];
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		bool Plausible(const float a_tree[4], const float a_timers[2])
		{
			for (int i = 0; i < 4; ++i)
				if (!std::isfinite(a_tree[i]))
					return false;
			if (!std::isfinite(a_timers[0]) || !std::isfinite(a_timers[1]))
				return false;
			// Amplitude >= 0 (0 = this shape does not sway), frequency and magnitude of a sane size, a timer below float's integer range.
			return a_tree[2] >= 0.0f && a_tree[2] <= 1000.0f && std::abs(a_tree[3]) <= 1000.0f && std::abs(a_tree[1]) <= 1000.0f &&
			       std::abs(a_timers[0]) < 1.0e7f && std::abs(a_timers[1]) < 1.0e7f;
		}

		bool Same(float a_value, float a_reference) { return std::abs(a_value - a_reference) <= 1.0e-4f * std::max(1.0f, std::abs(a_reference)); }

		bool Distrusted() { return g_stats.managerMismatches >= kDistrustMismatches && g_stats.managerMismatches > g_stats.managerMatches; }

		void Diagnose(const RE::BSGeometry* a_geometry, const Entry& a_entry, const Manager& a_manager, bool a_managerOk)
		{
			const auto& runtime = a_geometry->GetGeometryRuntimeData();
			const void* mesh = runtime.rendererData;
			if (!mesh)
				return;
			float distance = -1.0f;
			if (auto* camera = RE::Main::WorldRootCamera()) {
				const auto d = a_geometry->worldBound.center - camera->world.translate;
				distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
			}
			const float* tree = a_entry.params.treeParams;
			const float* timers = a_entry.params.windTimers;
			auto found = g_meshes.find(mesh);
			if (found == g_meshes.end()) {
				if (g_meshes.size() >= kMaxMeshDiag)
					return;
				g_meshes.emplace(mesh, MeshDiag{ a_geometry, tree[2], tree[3], distance, false });
				if (g_loggedMeshes >= kLogMeshes)
					return;
				++g_loggedMeshes;
				auto* triShape = const_cast<RE::BSGeometry*>(a_geometry)->AsTriShape();
				const std::uint32_t vertices = triShape ? triShape->GetTrishapeRuntimeData().vertexCount : 0;
				const std::string manager = a_managerOk ?
				                                std::format("BSTreeManager magnitude {:.4f} timer {:.4f} ({})", a_manager.magnitude, a_manager.timer,
													Same(tree[1], a_manager.magnitude) && Same(timers[0], a_manager.timer) ? "match" : "MISMATCH") :
				                                std::string("BSTreeManager not readable");
				const std::string predicted = a_entry.hasPrediction ?
				                                  std::format("TREE leaf amplitude {:.4f} frequency {:.4f} ({})", a_entry.predicted[0], a_entry.predicted[1],
													  Quantize(a_entry.predicted[0]) == Quantize(tree[2]) && Quantize(a_entry.predicted[1]) == Quantize(tree[3]) ? "match" : "differs") :
				                                  std::string("no TREE form");
				logger::info("[RayTracing] Foliage sway: '{}' ({} vertices, {:.0f} units away): TreeParams ({:.4f}, {:.4f}, {:.4f}, {:.4f}) WindTimers ({:.4f}, {:.4f}); {}; {}",
					a_geometry->name.c_str(), vertices, distance, tree[0], tree[1], tree[2], tree[3], timers[0], timers[1], manager, predicted);
				return;
			}
			auto& diag = found->second;
			if (diag.first == a_geometry || diag.copySeen)
				return;
			// A second copy of the mesh: the same amplitude / frequency? (If not, keying wind meshes by them keeps both right.)
			diag.copySeen = true;
			const bool same = Quantize(diag.amplitude) == Quantize(tree[2]) && Quantize(diag.frequency) == Quantize(tree[3]);
			++(same ? g_stats.copiesSame : g_stats.copiesDifferent);
			if (g_loggedCopies >= kLogCopies)
				return;
			++g_loggedCopies;
			logger::info("[RayTracing] Foliage sway: second copy of '{}' at {:.0f} units (first at {:.0f}): amplitude / frequency {} ({:.4f} / {:.4f} vs {:.4f} / {:.4f})",
				a_geometry->name.c_str(), distance, diag.distance, same ? "the same" : "DIFFERENT", tree[2], tree[3], diag.amplitude, diag.frequency);
		}
	}

	void SetActive(bool a_active)
	{
		if (g_active.load(std::memory_order_relaxed) == a_active)
			return;
		std::scoped_lock lock(g_mutex);
		g_active.store(a_active, std::memory_order_relaxed);
		if (a_active)
			return;
		g_requests.clear();
		g_meshes.clear();
		g_haveGlobal = false;
		g_globalTriedFrame = UINT32_MAX;
		g_computedFrame = UINT32_MAX;
		g_frameWind = {};
		const auto matches = g_stats.managerMatches, mismatches = g_stats.managerMismatches;  // the verdict on BSTreeManager stays
		g_stats = {};
		g_stats.managerMatches = matches;
		g_stats.managerMismatches = mismatches;
	}

	bool IsActive() { return g_active.load(std::memory_order_relaxed); }

	std::unique_lock<std::mutex> LockForBatch() { return std::unique_lock<std::mutex>(g_mutex); }

	void Request(const RE::BSGeometry* a_geometry, const float* a_predicted)
	{
		std::scoped_lock lock(g_mutex);
		RequestLocked(a_geometry, a_predicted);
	}

	void Forget(const RE::BSGeometry* a_geometry)
	{
		std::scoped_lock lock(g_mutex);
		ForgetLocked(a_geometry);
	}

	bool Get(const RE::BSGeometry* a_geometry, Params& o_params)
	{
		std::scoped_lock lock(g_mutex);
		return GetLocked(a_geometry, o_params);
	}

	void RequestLocked(const RE::BSGeometry* a_geometry, const float* a_predicted)
	{
		if (!a_geometry)
			return;
		auto [it, inserted] = g_requests.try_emplace(a_geometry);
		++it->second.refs;
		if (inserted && a_predicted) {
			it->second.predicted[0] = a_predicted[0];
			it->second.predicted[1] = a_predicted[1];
			it->second.hasPrediction = true;
		}
	}

	void ForgetLocked(const RE::BSGeometry* a_geometry)
	{
		if (!a_geometry)
			return;
		const auto it = g_requests.find(a_geometry);
		if (it != g_requests.end() && (it->second.refs <= 1 || --it->second.refs == 0))
			g_requests.erase(it);
	}

	bool GetLocked(const RE::BSGeometry* a_geometry, Params& o_params)
	{
		const auto it = g_requests.find(a_geometry);
		if (it == g_requests.end() || it->second.params.samples == 0)
			return false;
		o_params = it->second.params;
		return true;
	}

	void OnSetupGeometry(RE::BSRenderPass* a_pass)
	{
		if (!g_active.load(std::memory_order_relaxed) || !a_pass)
			return;
		auto* property = a_pass->shaderProperty;
		const auto* geometry = a_pass->geometry;
		if (!property || !geometry || property->flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim))
			return;
		std::unique_lock lock(g_mutex, std::try_to_lock);
		if (!lock.owns_lock())
			return;
		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0u;
		auto it = g_requests.find(geometry);
		bool due = false;
		if (it != g_requests.end()) {
			const auto& entry = it->second;
			const bool retryOk = entry.failedFrame == UINT32_MAX || frame - entry.failedFrame >= kRetryFrames;
			due = retryOk && (entry.params.samples == 0 || frame - entry.params.frame >= kSpotCheckFrames);
		}
		// The fallback's per-frame sample, only while BSTreeManager is not used (one read per frame at most).
		const bool global = g_globalTriedFrame != frame && (Distrusted() || !g_stats.managerReadable);
		if (!due && !global)
			return;
		if (global)
			g_globalTriedFrame = frame;
		float tree[4], timers[2];
		if (!ReadConstants(tree, timers) || !Plausible(tree, timers)) {
			++g_stats.readFailures;
			if (due)
				it->second.failedFrame = frame;
			return;
		}
		++g_stats.samplesTotal;
		Manager manager{};
		const bool managerOk = ReadManager(manager);
		if (managerOk)
			++(Same(tree[1], manager.magnitude) && Same(timers[0], manager.timer) ? g_stats.managerMatches : g_stats.managerMismatches);
		if (global) {
			std::copy_n(tree, 4, g_globalTree);
			std::copy_n(timers, 2, g_globalTimers);
			g_globalFrame = frame;
			g_haveGlobal = true;
		}
		if (!due)
			return;
		auto& entry = it->second;
		if (entry.params.samples > 0 &&
			(Quantize(entry.params.treeParams[2]) != Quantize(tree[2]) || Quantize(entry.params.treeParams[3]) != Quantize(tree[3])))
			++g_stats.spotChanges;
		const bool first = entry.params.samples == 0;
		std::copy_n(tree, 4, entry.params.treeParams);
		std::copy_n(timers, 2, entry.params.windTimers);
		entry.params.frame = frame;
		++entry.params.samples;
		entry.failedFrame = UINT32_MAX;
		if (first)
			Diagnose(geometry, entry, manager, managerOk);
	}

	FrameWind Compute(std::uint32_t a_frame)
	{
		std::scoped_lock lock(g_mutex);
		if (a_frame == g_computedFrame)
			return g_frameWind;
		g_computedFrame = a_frame;
		FrameWind wind{};
		Manager manager{};
		const bool managerOk = ReadManager(manager);
		g_stats.managerReadable = managerOk;
		if (managerOk && !Distrusted()) {
			wind = FrameWind{ true, manager.magnitude, manager.timer, true };
		} else if (g_haveGlobal && a_frame - g_globalFrame <= kFallbackMaxAge) {
			// The shaders' last values (normally last frame's draws), one frame on per frame since: x + (x - y).
			float step = g_globalTimers[0] - g_globalTimers[1];
			if (!std::isfinite(step) || std::abs(step) > 10.0f)
				step = 0.0f;
			const float timer = g_globalTimers[0] + step * static_cast<float>(a_frame - g_globalFrame);
			wind = FrameWind{ std::isfinite(timer), g_globalTree[1], timer, false };
		}
		g_stats.usingManager = wind.valid && wind.fromManager;
		g_stats.magnitude = wind.magnitude;
		g_stats.timer = wind.timer;
		g_frameWind = wind;
		return wind;
	}

	bool TreeAnimsEnabled()
	{
		__try {
			auto* manager = RE::BSTreeManager::GetSingleton();
			return !manager || manager->enableTreeAnims;
		} __except (EXCEPTION_EXECUTE_HANDLER) {
			return true;
		}
	}

	Stats GetStats()
	{
		std::scoped_lock lock(g_mutex);
		Stats stats = g_stats;
		stats.requested = static_cast<std::uint32_t>(g_requests.size());
		stats.sampled = 0;
		for (const auto& [geometry, entry] : g_requests)
			stats.sampled += entry.params.samples > 0 ? 1u : 0u;
		return stats;
	}
}
