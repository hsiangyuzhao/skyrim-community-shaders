#pragma once

/**
 * @brief (48n) Engine fixes hand-ported from the official Community Shaders repository (no common history, ported by hand).
 *
 *  - Sun Occlusion Poll Fix (#2901): the sun glare's occlusion query is polled without D3D11's flush flag, so a pending
 *    poll no longer submits the queued command buffer mid-frame. The glare factor then updates at most one frame later.
 *  - Light List Search Fix (#3031): BSShaderPropertyLightData::AddLight's common "already listed" case is answered with
 *    an AVX2 search (scalar when the CPU lacks AVX2); misses and list rebuilds still run the engine routine.
 *  - Empty Overlay Cull Fix (#3050): RaceMenu overlay slots that still show the transparent default texture get no
 *    render passes (they blend to nothing but ran the full lighting shader).
 *  - Sun Shadow Mask Fix (#2925): geometry entering view is given the sun shadow bits before its render passes are
 *    built, so it no longer draws fully sunlit (no shadow map, no ray-traced sun shadow) for one frame.
 *  - LOD Blend Ring Fix (#2894): the terrain LOD blend ring follows the frame clock (0.25 s eased slide, leaning toward
 *    the player) instead of game time, so it no longer sticks at timescale 0 or jumps at midnight / cell changes.
 *
 * (48o) Every fix is always on (the 48n switches and the Advanced."Engine Fixes" block are gone; an old block is ignored).
 * Each one still verifies the engine code it touches and stays off when it does not match; the log says which.
 * Flat runtimes only: in VR nothing is installed.
 */
namespace UpstreamEngineFixes
{
	/** @brief Installs every fix whose code matches and logs each one's status. kPostPostLoad. */
	void Install();
	/** @brief Per-frame work (sun shadow mask cache, LOD ring). State::Reset, once per frame. */
	void OnFrame();
}
