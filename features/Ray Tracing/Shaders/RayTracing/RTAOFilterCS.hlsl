// (46t) Ray-traced ambient occlusion: its own spatial filters (this frame's rays only, nothing of NRD; the clamped
// history, RTAOTemporalCS, comes after them -- hard constraint #3 with its RTAO exception of 2026-10-16).
//
// Used instead of the SSRT confidence filter's passes (ssrt_conf_*, the 46s path, still used with every 46t switch
// off) when any of these is on:
//   Two scales     TraceAOCS writes the far sample in .r with its weight in .b (kFlagValidity) and the near visibility
//                  in .a: the far term is a weighted mean (sum w v / sum w: the share of open far segments among the
//                  directions whose near segment was open), the detail term is filtered on its own (RTAO_DETAIL).
//   Normal-Guided  (always since 47n; (47q) the Detail.y 0 path is gone) besides the 45 degree gate every tap is weighted by dot(N, N_tap)^Detail.y with the
//                  G-buffer normal (normal-mapped), so the sides of a crevice or a door frame and the wall next to it
//                  are not averaged together.
//   Full Res       the large-scale term filtered at full render resolution (RTAO_FULL, separable, edge-aware) instead
//                  of the quarter-resolution chain (RTAO_DOWN, RTAO_BLUR, RTAO_UP).
//
// Permutations (one per dispatch):
//   RTAO_DOWN                  Raw (t0) -> quarter-resolution (value, weight) u0, linear depth u1, normal u2
//   RTAO_BLUR [RTAO_VERTICAL]  quarter (value, weight) t5 -> u0, guides t3 / t4: 15 taps
//   RTAO_UP                    quarter (value, weight) t5 + Raw t0 -> full-resolution far / large visibility u0
//   RTAO_FULL [RTAO_VERTICAL]  horizontal: Raw t0 -> full (value, weight) u0; vertical: t5 -> visibility u0: 21 taps
//   RTAO_DETAIL                Raw .a t0 -> full-resolution detail visibility u0: 5x5
//   (47r) RTAO_TILE_MARK       Raw .g t0 -> per 8x8 tile 0 traced / 255 not (R8_UINT u0)
//   (47r) RTAO_TILE_DIST [RTAO_VERTICAL]  t6 -> the Chebyshev distance in tiles to a traced tile (u0), separable
//   (47r) every pass above: with kFlagTileSkip (RTAO: Filter Only Where Traced) a group farther from every traced tile than the reach its
//   readers need (kReach*) returns at once; t10 = the tile distances (RTAOCommon.hlsli TilesFar).
// (47l) kFlagTemporalV2 (RTAO: Temporal v2): RTAO_BLUR / RTAO_FULL take their Gaussian sigma from a world size (Temporal.y units at the
// pixel's distance, clamped), taps spread up to 4 pixels apart past the tile (read directly). Off: the fixed pixel sigma (47k).
// Guides: t1 the depth after the prepass (the game's), t2 NORMALROUGHNESS (view-space normal, octahedral).

#include "Common/GBuffer.hlsli"
#include "RayTracing/RTAOCommon.hlsli"

Texture2D<float4> Raw : register(t0);  // TraceAOCS (47h: RGBA16F, or RGBA8 where the GPU cannot share it)
Texture2D<float> DepthTexture : register(t1);
Texture2D<float4> NormalRoughness : register(t2);
Texture2D<float> LoDepth : register(t3);
Texture2D<float4> LoNormal : register(t4);
Texture2D<float2> Signal : register(t5);  // (value, weight) of the previous pass

static const float kNormalCos = 0.707;   // the 45 degree gate (as the confidence filter)
static const float kMinDepth = 1e-5;
static const float kDownDepthTol = 0.02;  // 2x2 block, relative linear depth
static const float kUpDepthTol = 0.1;     // upsample: neighbours up to 3 full-resolution pixels away
static const float kPlaneTol = 0.02;      // 1/z against the centre's plane prediction (relative)
static const float kGradTolLo = 0.06;     // the plane gradient's neighbours, 2 full-resolution pixels away
static const float kGradTolFull = 0.03;   // ... 1 pixel away
// (47l) Temporal v2: the large-scale blur's sigma from a world size, clamped (quarter-resolution / full-resolution pixels), and the
// largest tap spacing (15 / 21 taps: at most 4 x their 47k reach).
static const float kWorldSigmaMinLo = 1.0, kWorldSigmaMaxLo = 16.0;
static const float kWorldSigmaMinFull = 2.0, kWorldSigmaMaxFull = 24.0;
static const int kWorldStrideMax = 4;

// (47r) RTAO: Filter Only Where Traced (kFlagTileSkip): how far (in 8x8 tiles) from a traced tile each pass is still read. The history pass
// runs within 1 tile (its 16x16 groups span 2: 2); a pass is needed 1 tile (6 / 4 / 3 pixels) beyond its reader, the blurs 7 tiles (28
// quarter-resolution pixels) more, the full-resolution horizontal pass 5 (40 pixels) more; quarter-resolution groups span 2 tiles (+1).
static const uint kReachUp = 3u, kReachDetail = 3u, kReachDown = 20u;
#if defined(RTAO_VERTICAL)
static const uint kReachBlur = 4u, kReachFull = 3u;
#else
static const uint kReachBlur = 12u, kReachFull = 8u;
#endif
static const int kTileDistReach = 20;  // RTAO_TILE_DIST: the largest reach above

float LinearDepth(float a_depth)
{
	return Camera.w / max(Camera.x - a_depth * Camera.z, 1e-6);
}

float3 GNormal(int2 a_p)
{
	return float3(GBuffer::DecodeNormal(NormalRoughness[a_p].xy));
}

// The 45 degree gate and the Normal-Guided weight dot^power ((47q) always: the gate-only weights, Detail.y 0, are gone).
float NormalWeight(float3 a_centre, float3 a_tap)
{
	const float c = dot(a_centre, a_tap);
	if (!(c >= kNormalCos))
		return 0.0;
	return pow(saturate(c), Detail.y);
}

bool DepthNear(float a_centre, float a_tap, float a_tol)
{
	return abs(a_tap - a_centre) / max(a_centre, kMinDepth) < a_tol;
}

// The large-scale sample of a pixel: (value, weight). Two scales: the far sample and its weight (TraceAOCS .b; (47n) decoded: it
// also carries the moving-occluder flag).
float2 LargeSample(float4 a_raw)
{
	return (Size.z & kFlagValidity) ? float2(a_raw.r, RawFarWeight(a_raw.b)) : float2(a_raw.r, 1.0);
}

// Central difference of 1/z from the two neighbours along an axis (zero where either belongs to another surface): on a
// plane 1/z is affine in screen position, so the prediction keeps the kernel wide on grazing ground (ssrt_conf_filter).
float PlaneGradient(float a_c, float3 a_cn, float a_m, float3 a_mn, float a_p, float3 a_pn, float a_tol)
{
	const bool usable = DepthNear(a_c, a_m, a_tol) && DepthNear(a_c, a_p, a_tol) && dot(a_cn, a_mn) >= kNormalCos && dot(a_cn, a_pn) >= kNormalCos;
	const float g = 0.5 * (rcp(max(a_p, kMinDepth)) - rcp(max(a_m, kMinDepth)));
	return (usable && abs(g) < 1e30) ? g : 0.0;
}

bool PlaneAgrees(float a_centreInv, float a_predictedInv, float a_tap)
{
	return abs(rcp(max(a_tap, kMinDepth)) - a_predictedInv) <= kPlaneTol * a_centreInv;
}

#if defined(RTAO_DOWN)
// ---------------------------------------------------------------------------------------------------------------------
RWTexture2D<float2> LoOut : register(u0);
RWTexture2D<float> LoDepthOut : register(u1);
RWTexture2D<float4> LoNormalOut : register(u2);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
	// (47r) Down: read by the horizontal blur 28 quarter-resolution pixels (7 tiles) away.
	if (TilesFar(int2(gid.xy) * 2, int2(gid.xy) * 2 + 1, kReachDown))
		return;
	const int2 hiSize = int2(Size.xy);
	const int2 loSize = (hiSize + 1) >> 1;
	if (any(int2(dtid.xy) >= loSize))
		return;
	const int2 block = int2(dtid.xy) << 1;
	const int2 maxP = max(hiSize - 1, int2(0, 0));
	const int2 ref = min(block, maxP);
	const float refDepth = LinearDepth(DepthTexture[ref]);
	const float3 refNormal = GNormal(ref);
	float sumV = 0.0, sumW = 0.0, sumA = 0.0, sumD = 0.0;
	float3 sumN = 0.0;
	[unroll] for (int tap = 0; tap < 4; ++tap)
	{
		const int2 px = min(block + int2(tap & 1, tap >> 1), maxP);
		const float d = LinearDepth(DepthTexture[px]);
		const float3 n = GNormal(px);
		const float a = DepthNear(refDepth, d, kDownDepthTol) ? NormalWeight(refNormal, n) : 0.0;
		const float2 s = LargeSample(Raw[px]);
		sumV += a * s.y * s.x;
		sumW += a * s.y;
		sumA += a;
		sumD += a * d;
		sumN += a * n;
	}
	const bool degenerate = !(sumA > 0.0);
	const float2 ownSample = LargeSample(Raw[ref]);
	float2 result;
	if (degenerate)
		result = float2(ownSample.y > 0.0 ? ownSample.x : 1.0, ownSample.y);
	else
		result = float2(sumW > 1e-6 ? sumV / sumW : 1.0, sumW / sumA);
	LoOut[dtid.xy] = saturate(result);
	LoDepthOut[dtid.xy] = degenerate ? refDepth : sumD / sumA;
	const float nn = dot(sumN, sumN);
	LoNormalOut[dtid.xy] = float4(degenerate || !(nn > 1e-8) ? refNormal : sumN * rsqrt(nn), 0.0);
}

#elif defined(RTAO_BLUR)
// ---------------------------------------------------------------------------------------------------------------------
RWTexture2D<float2> SignalOut : register(u0);

#	define BLUR_RADIUS 7
#	define BLUR_SIGMA 4.0
#	if defined(RTAO_VERTICAL)
#		define TAP_STEP int2(0, 1)
#		define TILE_W 8
#		define TILE_H (8 + 2 * BLUR_RADIUS)
#	else
#		define TAP_STEP int2(1, 0)
#		define TILE_W (8 + 2 * BLUR_RADIUS)
#		define TILE_H 8
#	endif
#	define TILE_COUNT (TILE_W * TILE_H)

groupshared float2 g_signal[TILE_COUNT];
groupshared float g_depth[TILE_COUNT];
groupshared float3 g_normal[TILE_COUNT];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
	const int2 loSize = (int2(Size.xy) + 1) >> 1;
	// (47r) Blur: the vertical one is read by the upsample (1 tile), the horizontal one by the vertical blur 28 pixels (7 tiles) away.
	if (TilesFar(int2(gid.xy) * 2, int2(gid.xy) * 2 + 1, kReachBlur))
		return;
	const int2 maxP = max(loSize - 1, int2(0, 0));
	{
		const int2 tileOrigin = int2(gid.xy) * 8 - BLUR_RADIUS * TAP_STEP;
		const uint lane = gtid.y * 8 + gtid.x;
		for (uint slot = lane; slot < TILE_COUNT; slot += 64) {
			const int2 p = clamp(tileOrigin + int2(slot % TILE_W, slot / TILE_W), int2(0, 0), maxP);
			g_signal[slot] = Signal[p];
			g_depth[slot] = LoDepth[p];
			g_normal[slot] = LoNormal[p].xyz;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (any(int2(dtid.xy) >= loSize))
		return;
	const int2 centre = int2(gtid.xy) + BLUR_RADIUS * TAP_STEP;
	const int centreSlot = centre.y * TILE_W + centre.x;
	const int slotStep = TAP_STEP.y * TILE_W + TAP_STEP.x;
	const float cDepth = g_depth[centreSlot];
	const float3 cNormal = g_normal[centreSlot];
	const float cInv = rcp(max(cDepth, kMinDepth));
	const float gradient = PlaneGradient(cDepth, cNormal, g_depth[centreSlot - slotStep], g_normal[centreSlot - slotStep], g_depth[centreSlot + slotStep],
		g_normal[centreSlot + slotStep], kGradTolLo);
	float sumV = 0.0, sumW = 0.0, sumG = 0.0;
	[branch] if (Size.z & kFlagTemporalV2)
	{
		// (47l) Temporal v2: the Gaussian's sigma is a world size (Temporal.y units) at this pixel's distance instead of a fixed
		// 4 quarter-resolution pixels: far surfaces are no longer blurred over metres, near ones no longer over a few centimetres.
		// Past the tile's reach the 15 taps are spread `stride` pixels apart and read directly (out-of-screen taps skipped).
		const float sigma = clamp(Temporal.y * CameraDelta.w * 0.5 / max(cDepth, kMinDepth), kWorldSigmaMinLo, kWorldSigmaMaxLo);
		const int stride = clamp((int)ceil(sigma / BLUR_SIGMA), 1, kWorldStrideMax);
		[loop] for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; ++i)
		{
			const int o = i * stride;
			const int2 q = int2(dtid.xy) + o * TAP_STEP;
			if (any(q < 0) || any(q > maxP))
				continue;
			const float spatial = exp(-(float)(o * o) / (2.0 * sigma * sigma));
			const float w = PlaneAgrees(cInv, cInv + (float)o * gradient, LoDepth[q]) ? spatial * NormalWeight(cNormal, LoNormal[q].xyz) : 0.0;
			const float2 s = Signal[q];
			sumV += w * s.y * s.x;
			sumW += w * s.y;
			sumG += w;
		}
	}
	else
	{
		[unroll] for (int i = -BLUR_RADIUS; i <= BLUR_RADIUS; ++i)
		{
			const float spatial = exp(-(float)(i * i) / (2.0 * BLUR_SIGMA * BLUR_SIGMA));
			const int slot = centreSlot + i * slotStep;
			const float w = PlaneAgrees(cInv, cInv + (float)i * gradient, g_depth[slot]) ? spatial * NormalWeight(cNormal, g_normal[slot]) : 0.0;
			const float2 s = g_signal[slot];
			sumV += w * s.y * s.x;
			sumW += w * s.y;
			sumG += w;
		}
	}
	const float2 own = g_signal[centreSlot];
	SignalOut[dtid.xy] = sumG > 0.0 ? saturate(float2(sumW > 1e-6 ? sumV / sumW : 1.0, sumW / sumG)) : own;
}

#elif defined(RTAO_UP)
// ---------------------------------------------------------------------------------------------------------------------
RWTexture2D<float> VisibilityOut : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
	// (47r) Up: read by the history pass 6 pixels away (1 tile) of its own reach (1).
	if (TilesFar(int2(gid.xy), int2(gid.xy), kReachUp))
		return;
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 hiSize = int2(Size.xy);
	const int2 loSize = (hiSize + 1) >> 1;
	const int2 loMax = max(loSize - 1, int2(0, 0));
	const int2 p = int2(dtid.xy);
	const float cDepth = LinearDepth(DepthTexture[p]);
	const float3 cNormal = GNormal(p);
	const float2 loCoord = (float2(p) + 0.5) * 0.5 - 0.5;
	const int2 loBase = int2(floor(loCoord));
	const float2 f = loCoord - float2(loBase);
	float sumV = 0.0, sumW = 0.0, sumA = 0.0;
	[unroll] for (int tap = 0; tap < 4; ++tap)
	{
		const int2 o = int2(tap & 1, tap >> 1);
		const int2 lp = clamp(loBase + o, int2(0, 0), loMax);
		const float bilinear = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
		const float a = DepthNear(cDepth, LoDepth[lp], kUpDepthTol) ? bilinear * NormalWeight(cNormal, LoNormal[lp].xyz) : 0.0;
		const float2 s = Signal[lp];
		sumV += a * s.y * s.x;
		sumW += a * s.y;
		sumA += a;
	}
	float v;
	if (sumA > 0.0) {
		v = sumW > 1e-6 ? sumV / sumW : 1.0;  // no far sample around: open (the detail term holds this pixel's occlusion)
	} else {
		// No quarter-resolution neighbour of this surface: this pixel's own (noisy, but never borrowed across an edge).
		const float2 own = LargeSample(Raw[p]);
		v = own.y > 0.0 ? own.x : 1.0;
	}
	VisibilityOut[p] = saturate(v);
}

#elif defined(RTAO_FULL)
// ---------------------------------------------------------------------------------------------------------------------
#	define FULL_RADIUS 10
#	define FULL_SIGMA 6.0
#	if defined(RTAO_VERTICAL)
#		define TAP_STEP int2(0, 1)
#		define TILE_W 8
#		define TILE_H (8 + 2 * FULL_RADIUS)
RWTexture2D<float> VisibilityOut : register(u0);
#	else
#		define TAP_STEP int2(1, 0)
#		define TILE_W (8 + 2 * FULL_RADIUS)
#		define TILE_H 8
RWTexture2D<float2> SignalOut : register(u0);
#	endif
#	define TILE_COUNT (TILE_W * TILE_H)

groupshared float2 g_signal[TILE_COUNT];
groupshared float g_depth[TILE_COUNT];
groupshared float3 g_normal[TILE_COUNT];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
	// (47r) Full: the vertical one is read by the history pass (1 tile), the horizontal one by the vertical one 40 pixels (5 tiles) away.
	if (TilesFar(int2(gid.xy), int2(gid.xy), kReachFull))
		return;
	const int2 maxP = max(int2(Size.xy) - 1, int2(0, 0));
	{
		const int2 tileOrigin = int2(gid.xy) * 8 - FULL_RADIUS * TAP_STEP;
		const uint lane = gtid.y * 8 + gtid.x;
		for (uint slot = lane; slot < TILE_COUNT; slot += 64) {
			const int2 p = clamp(tileOrigin + int2(slot % TILE_W, slot / TILE_W), int2(0, 0), maxP);
#	if defined(RTAO_VERTICAL)
			g_signal[slot] = Signal[p];
#	else
			g_signal[slot] = LargeSample(Raw[p]);
#	endif
			g_depth[slot] = LinearDepth(DepthTexture[p]);
			g_normal[slot] = GNormal(p);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 centre = int2(gtid.xy) + FULL_RADIUS * TAP_STEP;
	const int centreSlot = centre.y * TILE_W + centre.x;
	const int slotStep = TAP_STEP.y * TILE_W + TAP_STEP.x;
	const float cDepth = g_depth[centreSlot];
	const float3 cNormal = g_normal[centreSlot];
	const float cInv = rcp(max(cDepth, kMinDepth));
	const float gradient = PlaneGradient(cDepth, cNormal, g_depth[centreSlot - slotStep], g_normal[centreSlot - slotStep], g_depth[centreSlot + slotStep],
		g_normal[centreSlot + slotStep], kGradTolFull);
	float sumV = 0.0, sumW = 0.0, sumG = 0.0;
	[branch] if (Size.z & kFlagTemporalV2)
	{
		// (47l) Temporal v2: sigma in world units (as the quarter-resolution blur above), taps `stride` pixels apart, read directly.
		const float sigma = clamp(Temporal.y * CameraDelta.w / max(cDepth, kMinDepth), kWorldSigmaMinFull, kWorldSigmaMaxFull);
		const int stride = clamp((int)ceil(sigma / FULL_SIGMA), 1, kWorldStrideMax);
		[loop] for (int i = -FULL_RADIUS; i <= FULL_RADIUS; ++i)
		{
			const int o = i * stride;
			const int2 q = int2(dtid.xy) + o * TAP_STEP;
			if (any(q < 0) || any(q > maxP))
				continue;
			const float spatial = exp(-(float)(o * o) / (2.0 * sigma * sigma));
			const float w = PlaneAgrees(cInv, cInv + (float)o * gradient, LinearDepth(DepthTexture[q])) ? spatial * NormalWeight(cNormal, GNormal(q)) : 0.0;
#	if defined(RTAO_VERTICAL)
			const float2 s = Signal[q];
#	else
			const float2 s = LargeSample(Raw[q]);
#	endif
			sumV += w * s.y * s.x;
			sumW += w * s.y;
			sumG += w;
		}
	}
	else
	{
		[unroll] for (int i = -FULL_RADIUS; i <= FULL_RADIUS; ++i)
		{
			const float spatial = exp(-(float)(i * i) / (2.0 * FULL_SIGMA * FULL_SIGMA));
			const int slot = centreSlot + i * slotStep;
			const float w = PlaneAgrees(cInv, cInv + (float)i * gradient, g_depth[slot]) ? spatial * NormalWeight(cNormal, g_normal[slot]) : 0.0;
			const float2 s = g_signal[slot];
			sumV += w * s.y * s.x;
			sumW += w * s.y;
			sumG += w;
		}
	}
	const float2 own = g_signal[centreSlot];
#	if defined(RTAO_VERTICAL)
	VisibilityOut[dtid.xy] = saturate(sumW > 1e-6 ? sumV / sumW : (own.y > 0.0 ? own.x : 1.0));
#	else
	SignalOut[dtid.xy] = sumG > 0.0 ? saturate(float2(sumW > 1e-6 ? sumV / sumW : 1.0, sumW / sumG)) : own;
#	endif
}

#elif defined(RTAO_DETAIL)
// ---------------------------------------------------------------------------------------------------------------------
// 5x5 edge-aware filter of the near visibility at full render resolution: a Gaussian (sigma 1.5 px) x the centre's plane
// (1/z predicted from both axes' gradients) x the normal weight. Small on purpose: the near rays are short, their
// occlusion changes over a few pixels (a stone joint, the inside of a door frame); the clamped history does the rest.
RWTexture2D<float> VisibilityOut : register(u0);

#	define DETAIL_RADIUS 2
#	define DETAIL_SIGMA 1.5
#	define DTILE (8 + 2 * DETAIL_RADIUS)
#	define DTILE_COUNT (DTILE * DTILE)

groupshared float g_value[DTILE_COUNT];
groupshared float g_depth[DTILE_COUNT];
groupshared float3 g_normal[DTILE_COUNT];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
	// (47r) Detail: read by the history pass 4 pixels away (1 tile) of its own reach (1).
	if (TilesFar(int2(gid.xy), int2(gid.xy), kReachDetail))
		return;
	const int2 maxP = max(int2(Size.xy) - 1, int2(0, 0));
	{
		const int2 tileOrigin = int2(gid.xy) * 8 - DETAIL_RADIUS;
		const uint lane = gtid.y * 8 + gtid.x;
		for (uint slot = lane; slot < DTILE_COUNT; slot += 64) {
			const int2 p = clamp(tileOrigin + int2(slot % DTILE, slot / DTILE), int2(0, 0), maxP);
			g_value[slot] = Raw[p].a;
			g_depth[slot] = LinearDepth(DepthTexture[p]);
			g_normal[slot] = GNormal(p);
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 centre = int2(gtid.xy) + DETAIL_RADIUS;
	const int c = centre.y * DTILE + centre.x;
	const float cDepth = g_depth[c];
	const float3 cNormal = g_normal[c];
	const float cInv = rcp(max(cDepth, kMinDepth));
	const float gx = PlaneGradient(cDepth, cNormal, g_depth[c - 1], g_normal[c - 1], g_depth[c + 1], g_normal[c + 1], kGradTolFull);
	const float gy = PlaneGradient(cDepth, cNormal, g_depth[c - DTILE], g_normal[c - DTILE], g_depth[c + DTILE], g_normal[c + DTILE], kGradTolFull);
	float sum = 0.0, sumW = 0.0;
	[unroll] for (int dy = -DETAIL_RADIUS; dy <= DETAIL_RADIUS; ++dy)
	{
		[unroll] for (int dx = -DETAIL_RADIUS; dx <= DETAIL_RADIUS; ++dx)
		{
			const int slot = c + dy * DTILE + dx;
			const float spatial = exp(-(float)(dx * dx + dy * dy) / (2.0 * DETAIL_SIGMA * DETAIL_SIGMA));
			const float predicted = cInv + (float)dx * gx + (float)dy * gy;
			const float w = PlaneAgrees(cInv, predicted, g_depth[slot]) ? spatial * NormalWeight(cNormal, g_normal[slot]) : 0.0;
			sum += w * g_value[slot];
			sumW += w;
		}
	}
	VisibilityOut[dtid.xy] = saturate(sumW > 0.0 ? sum / sumW : g_value[c]);
}

#elif defined(RTAO_TILE_MARK)
// ---------------------------------------------------------------------------------------------------------------------
// (47r) One group per 8x8 tile: 0 where any pixel of it was traced (TraceAOCS .g, the RT weight, above 0), else 255.
RWTexture2D<uint> TileOut : register(u0);
groupshared uint g_traced;

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	if (gi == 0u)
		g_traced = 0u;
	GroupMemoryBarrierWithGroupSync();
	if (all(dtid.xy < Size.xy) && Raw[dtid.xy].g > 0.0)
		InterlockedOr(g_traced, 1u);
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0u)
		TileOut[gid.xy] = g_traced != 0u ? 0u : 255u;
}

#elif defined(RTAO_TILE_DIST)
// ---------------------------------------------------------------------------------------------------------------------
// (47r) The Chebyshev distance (tiles) to the nearest traced tile, separable: horizontal over the marks, then vertical over the
// horizontal distances (max(|dy|, h)); 255 beyond kTileDistReach.
Texture2D<uint> TileIn : register(t6);
RWTexture2D<uint> TileOut : register(u0);
#	if defined(RTAO_VERTICAL)
#		define TILE_STEP int2(0, 1)
#	else
#		define TILE_STEP int2(1, 0)
#	endif

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	const int2 tiles = int2((Size.xy + 7u) / 8u);
	const int2 t = int2(dtid.xy);
	if (any(t >= tiles))
		return;
	uint d = 255u;
	[loop] for (int i = -kTileDistReach; i <= kTileDistReach; ++i)
	{
		const int2 q = t + i * TILE_STEP;
		if (any(q < 0) || any(q >= tiles))
			continue;
		d = min(d, max(uint(abs(i)), TileIn[q]));
	}
	TileOut[t] = d;
}

#else
#	error "RTAOFilterCS: define one of RTAO_DOWN, RTAO_BLUR, RTAO_UP, RTAO_FULL, RTAO_DETAIL, RTAO_TILE_MARK, RTAO_TILE_DIST"
#endif
