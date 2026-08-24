// (batch 6) Stage 2 of 3 of the ambient-reinjection confidence filter: a 15-tap joint
// bilateral blur of the quarter-resolution confidence along one axis. Two permutations, one
// per axis, dispatched back to back -- SSRT_CONF_BLUR_VERTICAL selects the second.
//
// Separable, and that is the whole point of the stage. A 15x15 non-separable window is 225
// taps; two 15-tap passes are 30, for the same spatial reach. The reach itself is what buys
// the sample count: 7 quarter-resolution texels is 14 full-resolution pixels, so each output
// value averages about 1274 independent ray samples where the 7x7 full-resolution window it
// replaces averaged 98.
//
// Nothing here reads a previous frame. See ssrt_conf_filter.hlsli for why that is permanent.
#include "ScreenSpaceRayTracing/ssrt_conf_filter.hlsli"

Texture2D<float> ConfidenceLoTexture : register(t0);
Texture2D<float> DepthLoTexture : register(t1);
Texture2D<float4> NormalLoTexture : register(t3);

RWTexture2D<float> ConfidenceLoRW : register(u0);

// One code path for both axes. The LDS tile grows along the blur axis only, which is the
// whole saving a separable pass offers over a square one: 22x8 entries instead of 22x22.
#ifdef SSRT_CONF_BLUR_VERTICAL
#	define SSRT_CONF_TAP_STEP int2(0, 1)
#	define SSRT_CONF_TILE_W 8
#	define SSRT_CONF_TILE_H (8 + 2 * SSRT_CONF_LO_RADIUS)
#else
#	define SSRT_CONF_TAP_STEP int2(1, 0)
#	define SSRT_CONF_TILE_W (8 + 2 * SSRT_CONF_LO_RADIUS)
#	define SSRT_CONF_TILE_H 8
#endif

#define SSRT_CONF_TILE_COUNT (SSRT_CONF_TILE_W * SSRT_CONF_TILE_H)

// The 15-tap neighbourhoods of an 8x8 group overlap almost completely -- 64 lanes want 960
// taps out of 176 distinct texels -- so each texel is read once into LDS. 176 entries x 20
// bytes is 3.5 KB of the 32 KB a group may hold.
groupshared float g_confidence[SSRT_CONF_TILE_COUNT];
groupshared float g_depth[SSRT_CONF_TILE_COUNT];
groupshared float3 g_normal[SSRT_CONF_TILE_COUNT];

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID, uint3 groupThreadID : SV_GroupThreadID, uint3 groupID : SV_GroupID)
{
	const int2 hiSize = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
	const int2 loSize = (hiSize + int2(1, 1)) >> 1;
	const int2 maxCoord = max(loSize - int2(1, 1), int2(0, 0));

	// ---- tile prefetch. No lane may leave before the barrier below, which is why the bounds
	// ---- early-out sits underneath it (the same shape as the firefly prefetch in
	// ---- ssrt_temporal.hlsl and the window this chain replaces).
	{
		const int2 tileOrigin = int2(groupID.xy) * 8 - SSRT_CONF_LO_RADIUS * SSRT_CONF_TAP_STEP;
		// One flat stride over the tile rather than the nested pair the square windows in this
		// feature use. The tile is 22x8 or 8x22, so one of the two nested bounds would be the
		// group size itself and fxc would (correctly) report the corresponding loop as executing
		// once -- a warning per permutation, on a build that runs at zero tolerance for them. A
		// linear index over 176 slots at a stride of 64 lanes has no such degenerate axis, and the
		// divide and modulo are by a compile-time constant.
		const uint lane = groupThreadID.y * 8 + groupThreadID.x;
		for (uint slot = lane; slot < SSRT_CONF_TILE_COUNT; slot += 64) {
			const uint tx = slot % SSRT_CONF_TILE_W;
			const uint ty = slot / SSRT_CONF_TILE_W;
			// Clamp to edge. Duplicating a border texel is free here because the statistic
			// is a local mean of a slowly varying field, not an energy-preserving integral.
			const int2 p = clamp(tileOrigin + int2(tx, ty), int2(0, 0), maxCoord);
			g_confidence[slot] = ConfidenceLoTexture[p];
			g_depth[slot] = DepthLoTexture[p];
			g_normal[slot] = NormalLoTexture[p].xyz;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	// ---- prefetch complete; early returns are safe from here on ----

	if (any(int2(dispatchID.xy) >= loSize))
		return;

	const int2 centre = int2(groupThreadID.xy) + SSRT_CONF_LO_RADIUS * SSRT_CONF_TAP_STEP;
	const int centreSlot = centre.y * SSRT_CONF_TILE_W + centre.x;
	const int slotStep = SSRT_CONF_TAP_STEP.y * SSRT_CONF_TILE_W + SSRT_CONF_TAP_STEP.x;

	const float centreDepth = g_depth[centreSlot];
	const float3 centreNormal = g_normal[centreSlot];
	const float centreInvDepth = rcp(max(centreDepth, SSRT_CONF_MIN_DEPTH));

	// Slope of 1/z along this axis, taken from the two immediate neighbours and zeroed
	// wherever they cannot be trusted. This is what keeps the full 15-tap width on grazing
	// ground; see SSRTConfPlaneGradient.
	const float gradient = SSRTConfPlaneGradient(
		centreDepth, centreNormal,
		g_depth[centreSlot - slotStep], g_normal[centreSlot - slotStep],
		g_depth[centreSlot + slotStep], g_normal[centreSlot + slotStep]);

	float sum = 0.0f;
	float weightSum = 0.0f;

	[unroll] for (int i = -SSRT_CONF_LO_RADIUS; i <= SSRT_CONF_LO_RADIUS; i++)
	{
		// exp() of a literal: fxc constant-folds this inside the unrolled loop, so the
		// Gaussian costs nothing at runtime.
		const float spatialWeight = exp(-(float)(i * i) / (2.0f * SSRT_CONF_LO_SIGMA * SSRT_CONF_LO_SIGMA));

		const int slot = centreSlot + i * slotStep;
		const float tapDepth = g_depth[slot];
		const float3 tapNormal = g_normal[slot];

		// Plane-predicted 1/z rather than a flat depth comparison. On a plane the prediction
		// is exact at any slope, so a grazing surface keeps every tap; at a silhouette the
		// gradient is zero and this degrades to the flat 2% test, which rejects the far
		// surface outright.
		const float predictedInvDepth = centreInvDepth + (float)i * gradient;
		const float tapInvDepth = rcp(max(tapDepth, SSRT_CONF_MIN_DEPTH));
		const bool depthAgrees = abs(tapInvDepth - predictedInvDepth) <= SSRT_CONF_LO_PLANE_TOL * centreInvDepth;
		const bool normalAgrees = dot(tapNormal, centreNormal) >= SSRT_CONF_LO_NORMAL_COS;

		const float weight = (depthAgrees && normalAgrees) ? spatialWeight : 0.0f;
		sum += g_confidence[slot] * weight;
		weightSum += weight;
	}

	// i == 0 predicts itself exactly and agrees with its own normal, so weightSum is at least
	// 1. The fallback is arithmetic insurance rather than a real branch, and it publishes the
	// unfiltered centre -- never a value from across a boundary.
	ConfidenceLoRW[dispatchID.xy] = weightSum > 0.0f ? saturate(sum / weightSum) : saturate(g_confidence[centreSlot]);
}
