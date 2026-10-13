#pragma once

#include <cstdint>

namespace RE
{
	class BSRenderPass;
	class BSGeometry;
}

/**
 * @brief (45, RT prototype) Keeps geometry the ray tracer covers out of the sun's shadow cascades.
 *
 * While Ray Tracing is active, static objects and landscape whose triangles are in a built BLAS
 * cast their sun shadow through the ray tracer, so drawing them into the cascades as well would
 * only double the shadow (and keep the shadow-map softness and acne the ray tracer replaces).
 * BSBatchRenderer::RenderPassImmediately asks ShouldSkip() for every pass; it says yes only
 *  - while the engine is rendering the SUN cascades (not the god-ray maps, not the character
 *    focus maps, not any local light), which GpuPhaseTimeline's RenderShadowmap call-site
 *    hook brackets with SetInSunCascade(), and
 *  - when this frame's latch (Latch(), taken before the shadow maps are drawn) says the ray
 *    tracer will run this frame, and
 *  - for a geometry registered by the scene capture whose current world bound lies inside the filter rect
 *    (the ray-traced area minus the hand-off band, so the band keeps its shadow-map shadow).
 *
 * Render thread only. With Ray Tracing off nothing is registered and ShouldSkip() is one branch.
 */
namespace RayTracingCasterFilter
{
	/// Called once per frame before the engine draws any shadow map.
	void Latch(bool a_active, const float a_rectMinMax[4]);
	[[nodiscard]] bool IsLatched();

	/// Bracket of the sun-cascade RenderShadowmap call (GpuPhaseTimeline hook).
	void SetInSunCascade(bool a_inside);

	/// The per-pass test (BSBatchRenderer::RenderPassImmediately hook).
	[[nodiscard]] bool ShouldSkip(const RE::BSRenderPass* a_pass);

	/// Registration of covered geometry, counted (two captures may hold the same geometry while a
	/// cell is re-captured). Callers guarantee that a registered geometry's triangles are in every
	/// TLAS traced until its Remove(): registration follows the BLAS build, removal happens after
	/// the frame's trace was submitted (45e).
	void Add(const RE::BSGeometry* a_geometry);
	void Remove(const RE::BSGeometry* a_geometry);
	void Clear();

	[[nodiscard]] std::uint32_t RegisteredCount();
	/// Passes skipped during the last completed frame, and during the frame in progress.
	[[nodiscard]] std::uint32_t SkippedLastFrame();
}
