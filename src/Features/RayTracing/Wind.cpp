#include "Wind.h"

#include "Globals.h"
#include "State.h"

#include <atomic>
#include <bit>
#include <mutex>

namespace RayTracingWind
{
	namespace
	{
		constexpr std::uint32_t kDiagSamples = 32;          // the one diagnostic line after this many samples
		constexpr std::uint32_t kDiagMaxFrames = 36000;     // ... or (with fewer) after this many frames of sampling
		constexpr std::size_t kDiagMaxEarly = 8192;         // nodes remembered from Early Prep for the comparison
		// SE 1.5.97: what the IDs must resolve to (BSLightingShader::SetupGeometry's tree branch reads exactly these).
		constexpr std::uintptr_t kRvaMagnitudeOwner = 0x1E0DED0;  // ID 513211: a pointer; TreeParams.y at +0x304
		constexpr std::uintptr_t kRvaFadeStart = 0x1E0DF70;       // ID 513234
		constexpr std::uintptr_t kRvaFadeEnd = 0x1E0DF74;         // ID 513235
		constexpr std::uintptr_t kMagnitudeOffset = 0x304;
		// BSLeafAnimNode (BSFadeNode 0x158 + ...): the engine's per-plant values (BSTreeManager::Update writes them every frame).
		constexpr std::uintptr_t kNodeDistanceSq = 0x158;
		constexpr std::uintptr_t kNodeAmplitude = 0x15C;
		constexpr std::uintptr_t kNodeFrequency = 0x160;
		constexpr std::uintptr_t kNodeTimer = 0x164;
		constexpr float kTimerScale = 6.0f;  // 0x14164DD68

		struct Addresses
		{
			bool resolved = false;
			bool ok = false;
			std::uintptr_t magnitudeOwner = 0, fadeStart = 0, fadeEnd = 0;
			std::uintptr_t leafVtable = 0, treeVtable = 0;
		};
		Addresses g_addresses;
		std::once_flag g_resolveOnce;

		void Resolve()
		{
			std::call_once(g_resolveOnce, [] {
				g_addresses.resolved = true;
				if (!REL::Module::IsSE()) {
					logger::warn("[RayTracing] Foliage sway: per-plant wind needs Skyrim SE 1.5.97 (its engine addresses); unavailable");
					return;
				}
				try {
					const std::uintptr_t base = REL::Module::get().base();
					g_addresses.magnitudeOwner = REL::ID(513211).address();
					g_addresses.fadeStart = REL::ID(513234).address();
					g_addresses.fadeEnd = REL::ID(513235).address();
					g_addresses.leafVtable = RE::VTABLE_BSLeafAnimNode[0].address();
					g_addresses.treeVtable = RE::VTABLE_BSTreeNode[0].address();
					g_addresses.ok = g_addresses.magnitudeOwner - base == kRvaMagnitudeOwner && g_addresses.fadeStart - base == kRvaFadeStart &&
					                 g_addresses.fadeEnd - base == kRvaFadeEnd && g_addresses.leafVtable && g_addresses.treeVtable;
					if (!g_addresses.ok)
						logger::warn("[RayTracing] Foliage sway: engine addresses not as expected (0x{:X} 0x{:X} 0x{:X}); per-plant wind unavailable",
							g_addresses.magnitudeOwner - base, g_addresses.fadeStart - base, g_addresses.fadeEnd - base);
				} catch (const std::exception& e) {
					logger::warn("[RayTracing] Foliage sway: engine addresses not found ({}); per-plant wind unavailable", e.what());
					g_addresses.ok = false;
				}
			});
		}

		/// The shared values (SEH: a pointer that is not set yet is a refused read, never a crash).
		bool ReadGlobals(Globals& o_globals)
		{
			__try {
				const auto owner = *reinterpret_cast<const std::uintptr_t*>(g_addresses.magnitudeOwner);
				if (!owner)
					return false;
				o_globals.magnitude = *reinterpret_cast<const float*>(owner + kMagnitudeOffset);
				o_globals.fadeStart = *reinterpret_cast<const float*>(g_addresses.fadeStart);
				o_globals.fadeEnd = *reinterpret_cast<const float*>(g_addresses.fadeEnd);
				return std::isfinite(o_globals.magnitude) && std::isfinite(o_globals.fadeStart) && std::isfinite(o_globals.fadeEnd);
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		RE::NiNode* LeafNodeOfProperty(const RE::BSShaderProperty* a_property)
		{
			__try {
				auto* node = a_property ? a_property->fadeNode : nullptr;
				if (!node)
					return nullptr;
				const std::uintptr_t vtable = *reinterpret_cast<const std::uintptr_t*>(node);
				return (vtable == g_addresses.leafVtable || vtable == g_addresses.treeVtable) ? node : nullptr;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return nullptr;
			}
		}

		struct NodeValues
		{
			float distanceSq, amplitude, frequency, timer;
		};
		bool ReadNode(const RE::NiNode* a_node, NodeValues& o_values)
		{
			__try {
				const auto base = reinterpret_cast<std::uintptr_t>(a_node);
				o_values.distanceSq = *reinterpret_cast<const float*>(base + kNodeDistanceSq);
				o_values.amplitude = *reinterpret_cast<const float*>(base + kNodeAmplitude);
				o_values.frequency = *reinterpret_cast<const float*>(base + kNodeFrequency);
				o_values.timer = *reinterpret_cast<const float*>(base + kNodeTimer);
				return true;
			} __except (EXCEPTION_EXECUTE_HANDLER) {
				return false;
			}
		}

		/// The engine's one-step reciprocal square root, times the value: its distance (bit for bit).
		float EngineSqrt(float a_value)
		{
			const float half = a_value * 0.5f;
			const std::int32_t bits = 0x5F3759DF - (std::bit_cast<std::int32_t>(a_value) >> 1);
			const float y = std::bit_cast<float>(bits);
			float t = half * y;
			t = t * y;
			float r = 1.5f - t;
			r = r * y;
			return r * a_value;
		}

		/// TreeParams.z: the amplitude faded by the distance (the engine's order of operations and comparisons).
		float FadedAmplitude(float a_amplitude, float a_distance, const Globals& a_globals)
		{
			const float range = a_globals.fadeEnd - a_globals.fadeStart;
			const float t = (a_distance - a_globals.fadeStart) / range;
			float faded = (1.0f - t) * a_amplitude;
			if (!(faded >= 0.0f))
				faded = 0.0f;
			return (a_amplitude < faded || std::isnan(a_amplitude)) ? a_amplitude : faded;
		}

		/// The vertex shader's per-geometry constants (TreeParams, WindTimers) as SetupGeometry just wrote them (ShaderCache's
		/// Lighting VS constant table entries 4 and 5). SEH: a fault is a refused read.
		bool ReadConstants(float o_tree[4], float o_timers[2])
		{
			__try {
				auto** current = globals::game::currentVertexShader;
				auto* shader = current ? *current : nullptr;
				if (!shader)
					return false;
				const std::int8_t treeOffset = shader->constantTable[4];
				const std::int8_t timerOffset = shader->constantTable[5];
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

		bool Same(float a_value, float a_reference) { return std::abs(a_value - a_reference) <= 1.0e-5f * std::max(1.0f, std::abs(a_reference)); }

		struct EarlyEntry
		{
			std::uint32_t frame;
			Pose pose;
		};

		std::mutex g_mutex;
		std::atomic<bool> g_active{ false };
		Stats g_stats{};
		std::uint32_t g_globalsFrame = UINT32_MAX;
		Globals g_globals{};
		// Diagnostics (one line per session).
		bool g_diagDone = false;
		std::uint32_t g_diagStartFrame = UINT32_MAX;
		std::uint32_t g_earlyChecked = 0;
		ankerl::unordered_dense::map<const void*, EarlyEntry> g_early;
		ankerl::unordered_dense::set<const void*> g_sampled;
		std::string g_firstMismatch;

		void LogDiagnostics()
		{
			g_diagDone = true;
			logger::info("[RayTracing] Foliage sway: per-plant wind vs the vertex shader's constants: {} samples, the formula at draw time matches {}, "
						 "the Early Prep values of the same frame match {} of {}{}{} (magnitude {:.4f}, fade {:.0f}-{:.0f})",
				g_stats.compared, g_stats.formulaMatches, g_stats.earlyMatches, g_earlyChecked, g_firstMismatch.empty() ? "" : "; first difference: ", g_firstMismatch,
				g_globals.magnitude, g_globals.fadeStart, g_globals.fadeEnd);
			g_early.clear();
			g_sampled.clear();
		}
	}

	bool Available()
	{
		Resolve();
		return g_addresses.ok;
	}

	Globals FrameGlobals(std::uint32_t a_frame)
	{
		std::scoped_lock lock(g_mutex);
		if (a_frame == g_globalsFrame)
			return g_globals;
		g_globalsFrame = a_frame;
		Globals globals{};
		if (Available())
			globals.valid = ReadGlobals(globals);
		g_globals = globals;
		g_stats.available = g_addresses.ok;
		g_stats.magnitude = globals.magnitude;
		g_stats.fadeStart = globals.fadeStart;
		g_stats.fadeEnd = globals.fadeEnd;
		return globals;
	}

	RE::NiNode* LeafNodeOf(RE::BSGeometry* a_geometry)
	{
		if (!a_geometry || !Available())
			return nullptr;
		const auto& runtime = a_geometry->GetGeometryRuntimeData();
		auto* property = netimmerse_cast<RE::BSShaderProperty*>(runtime.properties[RE::BSGeometry::States::kEffect].get());
		return LeafNodeOfProperty(property);
	}

	bool Evaluate(const RE::NiNode* a_node, const Globals& a_globals, Pose& o_pose)
	{
		++g_stats.nodeReads;
		if (!a_globals.valid) {
			++g_stats.nodeFaults;
			return false;
		}
		Pose pose;
		pose.magnitude = a_globals.magnitude;
		if (a_node) {
			NodeValues values{};
			if (!ReadNode(a_node, values) || !std::isfinite(values.distanceSq) || !std::isfinite(values.amplitude) || !std::isfinite(values.frequency) ||
				!std::isfinite(values.timer) || std::abs(values.timer) > 1.0e6f) {
				++g_stats.nodeFaults;
				return false;
			}
			pose.amplitude = FadedAmplitude(values.amplitude, EngineSqrt(values.distanceSq), a_globals);
			pose.frequency = values.frequency;
			pose.timer = values.timer * kTimerScale;
		} else {
			pose.amplitude = FadedAmplitude(1.0f, 0.0f, a_globals);
			pose.frequency = 1.0f;
			pose.timer = 0.0f;
		}
		if (!std::isfinite(pose.amplitude) || std::abs(pose.amplitude) > 10000.0f || std::abs(pose.frequency) > 10000.0f) {
			++g_stats.nodeFaults;
			return false;
		}
		o_pose = pose;
		return true;
	}

	void NoteEarly(const RE::NiNode* a_node, const Pose& a_pose, std::uint32_t a_frame)
	{
		if (g_diagDone || !a_node)
			return;
		std::scoped_lock lock(g_mutex);
		if (g_early.size() >= kDiagMaxEarly && !g_early.contains(a_node))
			return;
		g_early[a_node] = EarlyEntry{ a_frame, a_pose };
	}

	void SetActive(bool a_active)
	{
		g_active.store(a_active, std::memory_order_relaxed);
	}

	bool IsActive() { return g_active.load(std::memory_order_relaxed); }

	void OnSetupGeometry(RE::BSRenderPass* a_pass)
	{
		if (g_diagDone || !g_active.load(std::memory_order_relaxed) || !a_pass)
			return;
		auto* property = a_pass->shaderProperty;
		if (!property || property->flags.none(RE::BSShaderProperty::EShaderPropertyFlag::kTreeAnim))
			return;
		std::unique_lock lock(g_mutex, std::try_to_lock);
		if (!lock.owns_lock() || g_diagDone)
			return;
		const std::uint32_t frame = globals::state ? globals::state->frameCount : 0u;
		if (g_diagStartFrame == UINT32_MAX)
			g_diagStartFrame = frame;
		auto* node = LeafNodeOfProperty(property);
		const void* key = node ? static_cast<const void*>(node) : static_cast<const void*>(a_pass->geometry);
		if (g_sampled.contains(key) || frame != g_globalsFrame || !g_globals.valid)
			return;
		float tree[4], timers[2];
		if (!ReadConstants(tree, timers))
			return;
		g_sampled.insert(key);
		Pose atDraw;
		const bool evaluated = Evaluate(node, g_globals, atDraw);
		++g_stats.compared;
		const bool formula = evaluated && Same(tree[1], atDraw.magnitude) && Same(tree[2], atDraw.amplitude) && Same(tree[3], atDraw.frequency) && Same(timers[0], atDraw.timer);
		if (formula)
			++g_stats.formulaMatches;
		bool early = false;
		if (const auto it = g_early.find(node); node && it != g_early.end() && it->second.frame == frame) {
			++g_earlyChecked;
			const Pose& p = it->second.pose;
			early = Same(tree[1], p.magnitude) && Same(tree[2], p.amplitude) && Same(tree[3], p.frequency) && Same(timers[0], p.timer);
			if (early)
				++g_stats.earlyMatches;
		}
		if ((!formula || (node && !early)) && g_firstMismatch.empty()) {
			const auto it = node ? g_early.find(node) : g_early.end();
			g_firstMismatch = std::format("'{}': TreeParams ({:.4f}, {:.4f}, {:.4f}, {:.4f}) WindTimers ({:.4f}, {:.4f}); formula at draw ({:.4f}, {:.4f}, {:.4f}, {:.4f}){}",
				a_pass->geometry ? a_pass->geometry->name.c_str() : "?", tree[0], tree[1], tree[2], tree[3], timers[0], timers[1], atDraw.magnitude, atDraw.amplitude,
				atDraw.frequency, atDraw.timer,
				it != g_early.end() ? std::format("; Early Prep (frame {} of {}) ({:.4f}, {:.4f}, {:.4f}, {:.4f})", it->second.frame, frame, it->second.pose.magnitude,
										  it->second.pose.amplitude, it->second.pose.frequency, it->second.pose.timer) :
									  std::string(node ? "; no Early Prep value" : "; no leaf node"));
		}
		if (g_stats.compared >= kDiagSamples || frame - g_diagStartFrame > kDiagMaxFrames)
			LogDiagnostics();
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
		return g_stats;
	}
}
