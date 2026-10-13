#include "CasterFilter.h"

namespace RayTracingCasterFilter
{
	namespace
	{
		// Geometry -> number of captures holding it (a cell and its replacement can overlap).
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> g_geometries;
		bool g_latched = false;
		bool g_watch = false;
		bool g_inSunCascade = false;
		float g_rect[4]{};
		std::uint32_t g_skipped = 0;
		std::uint32_t g_skippedLast = 0;

		// (45f) Missed casters: seen once each (the set), handed over a few at a time (the queue).
		constexpr std::size_t kMissedSetLimit = 16384;
		constexpr std::size_t kMissedQueueLimit = 64;
		ankerl::unordered_dense::set<const RE::BSGeometry*> g_missed;
		std::vector<RE::NiPointer<RE::BSGeometry>> g_missedQueue;

		bool InsideRect(const RE::NiBound& a_bound)
		{
			return a_bound.center.x - a_bound.radius >= g_rect[0] && a_bound.center.y - a_bound.radius >= g_rect[1] &&
			       a_bound.center.x + a_bound.radius <= g_rect[2] && a_bound.center.y + a_bound.radius <= g_rect[3];
		}

		void NoteMissed(RE::BSGeometry* a_geometry)
		{
			const auto& b = a_geometry->worldBound;
			// A full queue leaves the geometry unmarked: it is noted again on a later frame.
			if (!(b.radius >= kMissedMinRadius) || !InsideRect(b) || g_missed.size() >= kMissedSetLimit || g_missedQueue.size() >= kMissedQueueLimit)
				return;
			if (!g_missed.insert(a_geometry).second)
				return;
			g_missedQueue.emplace_back(a_geometry);
		}
	}

	void Latch(bool a_active, const float a_rectMinMax[4], bool a_watch)
	{
		g_skippedLast = g_skipped;
		g_skipped = 0;
		g_latched = a_active && !g_geometries.empty();
		g_watch = a_watch;
		if (a_rectMinMax)
			std::copy_n(a_rectMinMax, 4, g_rect);
	}

	bool IsLatched() { return g_latched; }

	void SetInSunCascade(bool a_inside) { g_inSunCascade = a_inside; }

	bool ShouldSkip(const RE::BSRenderPass* a_pass)
	{
		if (!g_inSunCascade || !a_pass || !a_pass->geometry || (!g_latched && !g_watch))
			return false;
		if (!g_geometries.contains(a_pass->geometry)) {
			if (g_watch)
				NoteMissed(a_pass->geometry);
			return false;
		}
		if (!g_latched)
			return false;
		// (45e) The live bound: objects that move (doors, items) are tested where they are now.
		// Whole bound inside the rect: its shadow on anything nearby is traced, not blended.
		if (!InsideRect(a_pass->geometry->worldBound))
			return false;
		++g_skipped;
		return true;
	}

	void Add(const RE::BSGeometry* a_geometry)
	{
		if (a_geometry)
			++g_geometries[a_geometry];
	}

	void Remove(const RE::BSGeometry* a_geometry)
	{
		const auto it = g_geometries.find(a_geometry);
		if (it != g_geometries.end() && --it->second == 0)
			g_geometries.erase(it);
	}

	void Clear()
	{
		g_geometries.clear();
		g_latched = false;
		g_watch = false;
		g_missedQueue.clear();
	}

	std::uint32_t RegisteredCount() { return static_cast<std::uint32_t>(g_geometries.size()); }

	std::uint32_t SkippedLastFrame() { return g_skippedLast; }

	std::uint32_t MissedCount() { return static_cast<std::uint32_t>(g_missed.size()); }

	std::vector<RE::NiPointer<RE::BSGeometry>> TakeMissed()
	{
		std::vector<RE::NiPointer<RE::BSGeometry>> out;
		out.swap(g_missedQueue);
		return out;
	}

	void ResetMissed()
	{
		g_missed.clear();
		g_missedQueue.clear();
	}
}
