#include "CasterFilter.h"

namespace RayTracingCasterFilter
{
	namespace
	{
		struct Bound
		{
			float x, y, z, r;
		};

		ankerl::unordered_dense::map<const RE::BSGeometry*, Bound> g_geometries;
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
		const auto it = g_geometries.find(a_pass->geometry);
		if (it == g_geometries.end())
			return false;
		const Bound& b = it->second;
		// Whole bound inside the rect: its shadow on anything nearby is traced, not blended.
		if (b.x - b.r < g_rect[0] || b.y - b.r < g_rect[1] || b.x + b.r > g_rect[2] || b.y + b.r > g_rect[3])
			return false;
		++g_skipped;
		return true;
	}

	void Add(const RE::BSGeometry* a_geometry, float a_x, float a_y, float a_z, float a_radius)
	{
		if (a_geometry)
			g_geometries[a_geometry] = Bound{ a_x, a_y, a_z, a_radius };
	}

	void Remove(const RE::BSGeometry* a_geometry) { g_geometries.erase(a_geometry); }

	void Clear()
	{
		g_geometries.clear();
		g_latched = false;
	}

	std::uint32_t RegisteredCount() { return static_cast<std::uint32_t>(g_geometries.size()); }

	std::uint32_t SkippedLastFrame() { return g_skippedLast; }
}
