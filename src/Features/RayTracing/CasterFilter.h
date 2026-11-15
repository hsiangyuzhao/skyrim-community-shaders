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
 * Render thread only. With Ray Tracing off nothing is registered and Decide() is one branch.
 *
 * (47z) "Keep Buildings in the Game's Sun Shadow Map" (Latch a_keep): forward (blended) surfaces that are not the depth-buffer surface
 * (glass, blend-only arches such as Solitude's strellis01, banners, particles, water) read the cascades at their own position
 * (Lighting.hlsl's 47h depth check, Effect / Water / Volumetric Shadows / fog), so the casters RT took over were missing for them. With
 * the switch, a registered keepable caster (Add a_keepable: captured statics / landscape / doors / movable statics and non-foliage
 * cut-outs; never foliage, trees or characters) whose live bound radius is at least kKeepMinRadius is not skipped in the near cascades
 * (slices < kKeepSlices) but drawn into a second depth array of the same size (the DSV swapped around its draw: Decide() kRedirect,
 * BeginRedirect / EndRedirect). The engine's own cascades -- so its shadow mask, CombineCS's min(mask, rt) and every deferred
 * (depth-buffer) surface -- stay exactly as in 47y. Deferred::CopyShadowData then merges both (RayTracing::ForwardShadowMap: per texel
 * the nearer depth) into the texture bound for the forward readers (t18 and the shared capture).
 */
namespace RayTracingCasterFilter
{
	/// Called once per frame before the engine draws any shadow map.
	/// a_watch (45f): Ray Tracing is running; big casters drawn into the sun cascades inside the
	/// rect that are NOT registered are noted (TakeMissed) so the scene can say why it lacks them.
	/// a_keep (47z): Keep Buildings in the Game's Sun Shadow Map (only while the filter latches).
	void Latch(bool a_active, const float a_rectMinMax[4], bool a_watch, bool a_keep = false);
	[[nodiscard]] bool IsLatched();

	/// Bracket of the sun-cascade RenderShadowmap call (GpuPhaseTimeline hook).
	void SetInSunCascade(bool a_inside);

	/// The per-pass test (BSBatchRenderer::RenderPassImmediately hook).
	enum class Decision
	{
		kDraw,
		kSkip,
		kRedirect  // (47z) draw it into the forward cascades (BeginRedirect / EndRedirect around the draw), not the engine's
	};
	[[nodiscard]] Decision Decide(const RE::BSRenderPass* a_pass);

	/// (47z) Around a kRedirect draw: false = it cannot be redirected (not a kept slice, an unexpected target, creation failed); the
	/// caller then skips it (as 47y). EndRedirect only after a true BeginRedirect.
	inline constexpr float kKeepMinRadius = 128.0f;
	inline constexpr std::uint32_t kKeepSlices = 2;  // the cascades forward Lighting reads (ShadowSampling: 0 and 1)
	[[nodiscard]] bool BeginRedirect();
	void EndRedirect();
	/// (47z) The forward cascades for the merge: null unless draws were redirected this frame into the texture a_cascades views.
	/// Kept slices not written this frame are cleared here, so every slice of the returned SRV belongs to this frame.
	[[nodiscard]] ID3D11ShaderResourceView* KeptCascades(ID3D11ShaderResourceView* a_cascades);
	/// (47z) Diagnostics: draws redirected / that could not be (skipped instead) during the last completed frame; bytes held.
	[[nodiscard]] std::uint32_t KeptLastFrame();
	[[nodiscard]] std::uint32_t RedirectFailedLastFrame();
	[[nodiscard]] std::uint32_t RedirectLostLastFrame();  // redirected draws whose target was found changed afterwards (expected 0)
	[[nodiscard]] std::uint64_t KeptBytes();
	void ReleaseKept();

	/// Registration of covered geometry, counted (two captures may hold the same geometry while a
	/// cell is re-captured). Callers guarantee that a registered geometry's triangles are in every
	/// TLAS traced until its Remove(): registration follows the BLAS build, removal happens after
	/// the frame's trace was submitted (45e).
	/// (47z) a_keepable: may stay in the near cascades with Keep Buildings in the Game's Sun Shadow Map (any capture holding it as
	/// keepable makes it keepable).
	void Add(const RE::BSGeometry* a_geometry, bool a_keepable = false);
	void Remove(const RE::BSGeometry* a_geometry);
	void Clear();
	/// (45i) Changes on every Clear(): holders of registrations (characters) register again.
	[[nodiscard]] std::uint32_t Epoch();

	[[nodiscard]] std::uint32_t RegisteredCount();
	/// Passes skipped during the last completed frame, and during the frame in progress.
	[[nodiscard]] std::uint32_t SkippedLastFrame();

	/// (45f) Self-check: distinct big geometries (bound radius >= kMissedMinRadius, whole bound
	/// inside the rect) the engine drew into the sun cascades while they were not in the ray
	/// tracer's scene. TakeMissed() hands over the ones not yet described (a few per frame).
	inline constexpr float kMissedMinRadius = 256.0f;
	[[nodiscard]] std::uint32_t MissedCount();
	[[nodiscard]] std::vector<RE::NiPointer<RE::BSGeometry>> TakeMissed();
	/// (45g) The scene could not judge this miss yet (its cell is still being captured, its BLAS is
	/// being built, ...): the geometry is noted again the next time it is drawn into the cascades.
	void ForgetMissed(const RE::BSGeometry* a_geometry);
	void ResetMissed();
}
