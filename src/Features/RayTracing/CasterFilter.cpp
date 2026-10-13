#include "CasterFilter.h"

namespace RayTracingCasterFilter
{
	namespace
	{
		// Geometry -> number of captures holding it (a cell and its replacement can overlap).
		ankerl::unordered_dense::map<const RE::BSGeometry*, std::uint32_t> g_geometries;
		bool g_latched = false;
		bool g_inSunCascade = false;
		float g_rect[4]{};
		std::uint32_t g_skipped = 0;
		std::uint32_t g_skippedLast = 0;
	}

	void Latch(bool a_active, const float a_rectMinMax[4])
	{
		g_skippedLast = g_skipped;
		g_skipped = 0;
		g_latched = a_active && !g_geometries.empty();
		if (a_rectMinMax)
			std::copy_n(a_rectMinMax, 4, g_rect);
	}

	bool IsLatched() { return g_latched; }

	void SetInSunCascade(bool a_inside) { g_inSunCascade = a_inside; }

	bool ShouldSkip(const RE::BSRenderPass* a_pass)
	{
		if (!g_latched || !g_inSunCascade || !a_pass || !a_pass->geometry)
			return false;
		if (!g_geometries.contains(a_pass->geometry))
			return false;
		// (45e) The live bound: objects that move (doors, items) are tested where they are now.
		// Whole bound inside the rect: its shadow on anything nearby is traced, not blended.
		const auto& b = a_pass->geometry->worldBound;
		if (b.center.x - b.radius < g_rect[0] || b.center.y - b.radius < g_rect[1] || b.center.x + b.radius > g_rect[2] ||
			b.center.y + b.radius > g_rect[3])
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
	}

	std::uint32_t RegisteredCount() { return static_cast<std::uint32_t>(g_geometries.size()); }

	std::uint32_t SkippedLastFrame() { return g_skippedLast; }
}
