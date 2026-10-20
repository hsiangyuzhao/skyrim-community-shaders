// (45) Ray Tracing (prototype): merges the traced sun visibility into the engine's shadow mask.
//
//   mask.r = lerp(mask.r, min(mask.r, rt), w)
//
// rt is the traced visibility after the denoiser; w is 1 inside the traced area and fades to 0
// over the hand-off band at its edge, where the cascades / terrain shadows take over again.
// mask.gba (local shadowed lights) are copied unchanged.
// Screen Space Shadows inside the traced area (Lighting multiplies them with the mask):
//   combine (45j) / non-RT casters only (45k): kept, rewritten as min(mask, sss) / mask so that
//     mask * sss' = min(mask, sss): contact shadows of everything the ray tracer does not see
//     (grass, leaves, characters left to the shadow map) stay, and where both see the same
//     shadow it is not darkened twice. (45l) The 45i hand-off is gone (non-RT casters only with
//     contact length 0 is what it was meant to be).
//
// Denoisers (45l):
//   Off / DLSS RR: the small depth-aware filter of 45j (radius Band.y; 0 for DLSS RR).
//   Temporal: that small filter, then the clamped history of Penumbra (the 45j blend had no
//     visible effect on world-locked noise: it averaged identical frames).
//   Penumbra: the penumbra filter of this frame (full or half resolution, upsampled here), then
//     the clamped history: reprojected with the camera (bilinear, each tap checked against the
//     expected distance), clamped to this frame's 3x3 mean +- k sigma, blended with 1 / length.
//   NRD SIGMA: SIGMA's denoised shadow, nothing of ours.
// The debug views are written as colour + opacity for OverlayCS.

#include "RayTracing/Common.hlsli"

Texture2D<float4> Mask : register(t0);
Texture2D<unorm float4> Visibility : register(t1);  // r visibility, g coverage, w flags (R8G8B8A8_UNORM)
Texture2D<float> DepthMask : register(t2);  // signed depth, see PrepareCS (R32_FLOAT)
Texture2D<float> SigmaShadow : register(t3);  // (45l) NRD SIGMA OUT_SHADOW_TRANSLUCENCY (sqrt of the shadow)
Texture2D<float2> Denoised : register(t4);   // (45k) Penumbra: spatially filtered visibility, radius (px); (45l) half size with kFlagDenoiseHalf
Texture2D<float4> History4In : register(t5); // (45k) visibility, distance (-1 = none), history length (frames)

RWTexture2D<unorm float4> OutMask : register(u0);
RWTexture2D<unorm float> ScreenSpaceShadows : register(u1);
RWTexture2D<unorm float4> DebugColor : register(u2);
RWTexture2D<float4> History4Out : register(u4);  // (45k)

bool TracedPixel(int2 q, uint flags)
{
	const float sq = DepthMask[q];
	const float dq = abs(sq);
	if (RTIsSky(dq) || ((flags & kFlagTraceSkipped) && sq < 0.0))
		return false;
	const float3 Pq = RTPosition(q, dq);
	return Pq.x >= Rect.x && Pq.y >= Rect.y && Pq.x <= Rect.z && Pq.y <= Rect.w;
}

uint VisFlags(int2 q)
{
	return uint(Visibility[q].w * 255.0 + 0.5);
}

int2 HalfSize()
{
	return int2((Size.xy + 1u) / 2u);
}

// The small depth-aware filter of the traced visibility (45j; radius Band.y, 0 = the pixel itself).
// Pixels the trace skipped (engine mask already 0) hold a placeholder 0 and are left out.
float SmallFilter(int2 p, float dist, uint flags)
{
	const bool skipped = (flags & kFlagTraceSkipped) != 0u;
	const int radius = int(Band.y);
	const float sigma = float(radius * radius) + 0.5;
	float sum = 0.0, weights = 0.0;
	for (int dy = -radius; dy <= radius; ++dy) {
		for (int dx = -radius; dx <= radius; ++dx) {
			const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1);
			const float sq = DepthMask[q];
			const float dq = abs(sq);
			if (RTIsSky(dq) || (skipped && sq < 0.0))
				continue;
			const float distQ = length(RTPosition(q, dq));
			const float wt = exp(-abs(distQ - dist) / (dist * 0.01 + 2.0)) * exp(-float(dx * dx + dy * dy) / sigma);
			sum += Visibility[q].x * wt;
			weights += wt;
		}
	}
	return weights > 0.0 ? sum / weights : Visibility[p].x;
}

// (45l) The half-resolution penumbra filter brought back to this pixel: bilinear over the four
// nearest half samples, each weighted by how close its depth is to this pixel's. Where the filter
// radius is under a pixel (contact shadows) this pixel's own traced value is kept.
float PenumbraUpsample(int2 p, float dist, out float radiusFull)
{
	radiusFull = 0.0;
	const float raw = Visibility[p].x;
	const float2 hp = (float2(p) + 0.5) * 0.5 - 0.5;
	const int2 base = int2(floor(hp));
	const float2 f = hp - float2(base);
	const int2 maxH = HalfSize() - 1;
	float sum = 0.0, weights = 0.0;
	[unroll] for (int t = 0; t < 4; ++t)
	{
		const int2 o = int2(t & 1, t >> 1);
		const int2 q = clamp(base + o, int2(0, 0), maxH);
		const float2 v = Denoised[q];
		if (v.x < 0.0)
			continue;
		const int2 fq = q * 2;
		const float distQ = length(RTPosition(fq, abs(DepthMask[fq])));
		const float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
		const float w = max(bw, 1e-3) * exp(-abs(distQ - dist) / (dist * 0.02 + 4.0));
		sum += v.x * w;
		weights += w;
		radiusFull = max(radiusFull, v.y * 2.0);
	}
	if (weights < 1e-4)
		return raw;
	return lerp(raw, sum / weights, saturate(radiusFull - 0.5));
}

// (45k) This frame's 3x3 statistics for the history clamp: the penumbra filter's values (full or
// half resolution), or (Temporal) the traced values themselves.
void TemporalStats(int2 p, uint flags, out float mean, out float sigma, out float n)
{
	mean = 0.0;
	n = 0.0;
	float m2 = 0.0;
	if ((flags & kFlagSpatial) && (flags & kFlagDenoiseHalf)) {
		const int2 h = int2(uint2(p) / 2u);
		const int2 maxH = HalfSize() - 1;
		[unroll] for (int dy = -1; dy <= 1; ++dy)
		{
			[unroll] for (int dx = -1; dx <= 1; ++dx)
			{
				const float v = Denoised[clamp(h + int2(dx, dy), int2(0, 0), maxH)].x;
				if (v < 0.0)
					continue;
				mean += v;
				m2 += v * v;
				n += 1.0;
			}
		}
	} else {
		[unroll] for (int dy = -1; dy <= 1; ++dy)
		{
			[unroll] for (int dx = -1; dx <= 1; ++dx)
			{
				const int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1);
				if (!TracedPixel(q, flags))
					continue;
				const float v = (flags & kFlagSpatial) ? Denoised[q].x : Visibility[q].x;
				mean += v;
				m2 += v * v;
				n += 1.0;
			}
		}
	}
	if (n > 0.0) {
		mean /= n;
		sigma = sqrt(max(m2 / n - mean * mean, 0.0));
	} else {
		sigma = 0.0;
	}
}

// (45k) The clamped history: reprojected with the camera (bilinear, each tap checked against the
// expected distance), clamped to this frame's 3x3 mean +- k sigma, blended with 1 / length; the
// length grows to Denoise.z frames, less while the camera moves fast and where a character casts
// the shadow.
// (47r) With this frame's 3x3 statistics given (ClampedHistory computes them; the uniform-tile fast path knows them).
float ClampedHistoryWith(int2 p, float3 P, float rt, uint flags, uint anyFlags, float mean, float sigma, float n, out float historyLength)
{
	historyLength = 1.0;
	if (!(flags & kFlagHistoryValid))
		return rt;
	if (n <= 0.0)
		return rt;
	const float lo = mean - Denoise.w * sigma - 0.02, hi = mean + Denoise.w * sigma + 0.02;

	const float4 prevClip = mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0));
	if (prevClip.w <= 1e-4)
		return rt;
	const float2 prevUv = float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5);
	if (any(prevUv < 0.0) || any(prevUv >= 1.0))
		return rt;
	const float2 prevPos = prevUv * float2(Size.xy) - 0.5;
	const int2 base = int2(floor(prevPos));
	const float2 f = prevPos - float2(base);
	const float expected = length(P + CameraDelta.xyz);
	const float tolerance = expected * 0.02 + 8.0;
	float hv = 0.0, hl = 0.0, hw = 0.0;
	[unroll] for (int t = 0; t < 4; ++t)
	{
		const int2 o = int2(t & 1, t >> 1);
		const int2 q = clamp(base + o, int2(0, 0), int2(Size.xy) - 1);
		const float4 h = History4In[q];
		const float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
		if (h.y > 0.0 && abs(h.y - expected) <= tolerance && bw > 0.0) {
			hv += h.x * bw;
			hl += h.z * bw;
			hw += bw;
		}
	}
	if (hw < 0.05)
		return rt;  // disoccluded: this frame alone
	hv = clamp(hv / hw, lo, hi);
	hl /= hw;
	const float motion = length(prevPos + 0.5 - (float2(p) + 0.5));
	float maxLength = lerp(Denoise.z, 3.0, saturate((motion - 1.0) / 24.0));
	if (anyFlags & kOutCharacter)
		maxLength = min(maxLength, 3.0);
	historyLength = clamp(hl + 1.0, 1.0, max(maxLength, 1.0));
	return lerp(hv, rt, 1.0 / historyLength);
}

float ClampedHistory(int2 p, float3 P, float rt, uint flags, uint anyFlags, out float historyLength)
{
	historyLength = 1.0;
	if (!(flags & kFlagHistoryValid))
		return rt;
	float mean, sigma, n;
	TemporalStats(p, flags, mean, sigma, n);
	return ClampedHistoryWith(p, P, rt, flags, anyFlags, mean, sigma, n, historyLength);
}

// (47r) "Sun: Fast Path for Uniform Tiles" (kFlagUniformTiles, Penumbra): the denoiser's tiles (DenoiseTileCS: lowest / highest traced
// visibility per 8x8 tile) of the 5x5 tiles around this group all equal and 0 or 1 (or without traced pixels): the penumbra filter left
// every traced pixel within 1 tile of this group as it was (its 3x3-tile test was flat there), so the spatial result of a traced pixel is
// its own visibility (full resolution; at half resolution the upsample keeps the raw value at radius 0) and the 3x3 statistics are
// mean = that value, sigma = 0, n >= 1. Exactly what the per-pixel path computes, without its positions and loads.
// Returns 1 + the value (1 or 2) for such a group, 0 otherwise.
Texture2D<float4> Tiles : register(t6);
groupshared uint gsTileLo, gsTileHiP1;

uint UniformTiles(int2 a_tile, uint a_gi)
{
	if (a_gi == 0u) {
		gsTileLo = asuint(2.0);
		gsTileHiP1 = 0u;
	}
	GroupMemoryBarrierWithGroupSync();
	if (a_gi < 25u) {
		const int2 maxTile = int2((Size.xy + 7u) / 8u) - 1;
		const float4 t = Tiles[clamp(a_tile + int2(int(a_gi % 5u) - 2, int(a_gi / 5u) - 2), int2(0, 0), maxTile)];
		InterlockedMin(gsTileLo, asuint(clamp(t.z, 0.0, 2.0)));
		InterlockedMax(gsTileHiP1, asuint(clamp(t.w, -1.0, 1.0) + 1.0));
	}
	GroupMemoryBarrierWithGroupSync();
	const float lo = asfloat(gsTileLo), hi = asfloat(gsTileHiP1) - 1.0;
	return (lo == hi && (lo == 0.0 || lo == 1.0)) ? 1u + uint(lo) : 0u;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
	const int2 p = int2(dtid.xy);
	// (47r) Before the bounds test (it has barriers); the flag is the same for the whole dispatch.
	uint uniformTiles = 0u;
	[branch] if ((asuint(Band.z) & (kFlagUniformTiles | kFlagSpatial | kFlagPenumbra | kFlagSigma)) == (kFlagUniformTiles | kFlagSpatial | kFlagPenumbra))
		uniformTiles = UniformTiles(int2(gid.xy), gi);
	if (any(dtid.xy >= Size.xy))
		return;

	float4 mask = Mask[p];
	const float engineMask = mask.x;
	const float signedDepth = DepthMask[p];
	const float depth = abs(signedDepth);
	const uint flags = asuint(Band.z);
	const uint view = Size.z;

	float4 debug = 0;
	float w = 0;
	float rt = 1;
	float sss = 1;
	float4 history4 = float4(1.0, -1.0, 0.0, 0.0);
	float historyLength = 0.0;
	float radiusShown = 0.0;
	float dist = 0.0;

	if (!RTIsSky(depth)) {
		const float3 P = RTPosition(p, depth);
		dist = length(P);
		const float edge = min(min(P.x - Rect.x, Rect.z - P.x), min(P.y - Rect.y, Rect.w - P.y));
		// (45k) Extra.x fades the traced result in after a loading screen (1 otherwise).
		w = saturate(edge / max(Band.x, 1.0)) * saturate(Extra.x);

		[branch] if (w > 0.0)
		{
			[branch] if (flags & kFlagSigma)
			{
				// (45l) NRD SIGMA's result (it stores the square root of the shadow).
				const float s = SigmaShadow[p];
				rt = saturate(s * s);
			}
			else
			{
				// This frame's spatial stage.
				// (47r) Uniform tiles (see UniformTiles), a traced (not skipped) pixel: its own visibility, the known statistics.
				const bool fast = uniformTiles != 0u && !(signedDepth < 0.0 && (flags & kFlagTraceSkipped));
				if (fast) {
					rt = Visibility[p].x;
				} else if (flags & kFlagSpatial) {
					if (flags & kFlagDenoiseHalf)
						rt = PenumbraUpsample(p, dist, radiusShown);
					else {
						const float2 d = Denoised[p];
						rt = d.x;
						radiusShown = d.y;
					}
				} else {
					rt = SmallFilter(p, dist, flags);
				}
				// The clamped history (Penumbra, Temporal).
				[branch] if (flags & kFlagPenumbra)
				{
					uint anyFlags = 0u;
					[unroll] for (int dy = -1; dy <= 1; ++dy)
					{
						[unroll] for (int dx = -1; dx <= 1; ++dx)
						{
							anyFlags |= VisFlags(clamp(p + int2(dx, dy), int2(0, 0), int2(Size.xy) - 1));
						}
					}
					if (fast)
						rt = ClampedHistoryWith(p, P, rt, flags, anyFlags, float(uniformTiles - 1u), 0.0, 1.0, historyLength);
					else
						rt = ClampedHistory(p, P, rt, flags, anyFlags, historyLength);
					history4 = float4(rt, dist, historyLength, 0.0);
				}
			}

			if (signedDepth < 0.0 && (flags & kFlagTraceSkipped))
				rt = 0.0;  // the engine's mask is 0 here: min() keeps it, the debug views show the shadow
			mask.x = lerp(engineMask, min(engineMask, rt), w);
			if (flags & kFlagCombineSSS) {
				sss = ScreenSpaceShadows[p];
				const float m = mask.x;
				const float ratio = m > (1.0 / 255.0) ? saturate(min(m, sss) / m) : 1.0;
				ScreenSpaceShadows[p] = lerp(sss, ratio, w);
			}
		}
	}

	OutMask[p] = mask;
	if (flags & kFlagPenumbra)
		History4Out[p] = history4;
	if (flags & kFlagDebug) {
		if (view == kViewShadowSource) {
			if (w <= 0.0)
				debug = engineMask < 0.5 ? float4(0.15, 0.35, 1.0, 0.55) : float4(0.5, 0.5, 0.5, 0.2);  // outside: cascades / terrain
			else if (rt < 0.5)
				debug = float4(1.0, 0.1, 0.05, 0.6);  // ray-traced shadow
			else if (engineMask < 0.5)
				debug = float4(0.15, 0.35, 1.0, 0.6);  // shadow map: casters the ray tracer does not see
			else
				debug = float4(0.1, 1.0, 0.2, 0.15);  // lit
			if (w > 0.0 && w < 1.0)
				debug = lerp(float4(1.0, 0.85, 0.0, 0.5), debug, w);  // hand-off band
		} else if (view == kViewDisagreement) {
			if (w > 0.0) {
				const bool rtShadow = rt < 0.5, smShadow = engineMask < 0.5;
				if (rtShadow && !smShadow)
					debug = float4(1.0, 0.1, 0.05, 0.65);  // only RT says shadow
				else if (!rtShadow && smShadow)
					debug = float4(0.15, 0.35, 1.0, 0.65);  // only the shadow map says shadow
				else if (rtShadow)
					debug = float4(0.1, 0.1, 0.1, 0.3);  // both shadow
			}
		} else if (view == kViewCoverage) {
			// (45e) Primary-ray test (TraceShadowsCS): green = the visible surface is in the ray-traced
			// scene, magenta = it is not, orange = the scene has a surface in FRONT of the visible one
			// (geometry in the wrong place, or something the game hides).
			// (45h) cyan = an alpha-tested surface traced with its cut-out (0.8, Alpha-tested casters).
			// (45i) purple = a ray-traced character surface (0.65, Characters).
			if (w > 0.0) {
				const float c = Visibility[p].y;
				debug = c > 0.9 ? float4(0.1, 1.0, 0.2, 0.45) :
				        (c > 0.7 ? float4(0.0, 0.85, 1.0, 0.5) :
				        (c > 0.6 ? float4(0.55, 0.35, 1.0, 0.6) : (c > 0.375 ? float4(1.0, 0.55, 0.0, 0.6) : float4(1.0, 0.1, 1.0, 0.45))));
			}
		} else if (view == kViewHitDepth) {
			// (45e) Ray-traced first hit minus the game's depth along the view ray: white = match,
			// red = the ray tracer's surface is in front (full red at 256 units), blue = behind,
			// black = nothing within reach.
			if (w > 0.0) {
				const float2 vis = Visibility[p].yz;
				if (vis.x <= 0.0)
					debug = float4(0.0, 0.0, 0.0, 0.7);
				else {
					const float d = (vis.y - 0.5) * 2.0;  // -1 .. 1 = -256 .. 256 units, > 0 in front
					debug = d >= 0.0 ? float4(1.0, 1.0 - d, 1.0 - d, 0.7) : float4(1.0 + d, 1.0 + d, 1.0, 0.7);
				}
			}
		} else if (view == kViewRaw) {
			if (w > 0.0)
				debug = float4(Visibility[p].xxx, 1.0);
		} else if (view == kViewPenumbra) {
			// (45k) Penumbra filter radius: none (clear), small = blue, up to the maximum = red.
			if (w > 0.0 && (flags & kFlagSpatial)) {
				const float r = radiusShown / max(Denoise.y, 1.0);
				if (r > 0.0)
					debug = float4(saturate(r * 2.0), saturate(1.0 - abs(r - 0.5) * 2.0), saturate(1.0 - r * 2.0), 0.6);
			}
		} else if (view == kViewHistory) {
			// (45k) History length: red = this frame only (new / disoccluded), green = full history.
			if (w > 0.0 && (flags & kFlagPenumbra)) {
				const float h = saturate((historyLength - 1.0) / max(Denoise.z - 1.0, 1.0));
				debug = float4(1.0 - h, h, 0.1, 0.55);
			}
		} else if (view == kViewAdaptive) {
			// (45k) Magenta = adaptive extra rays traced here.
			if (w > 0.0 && (VisFlags(p) & kOutExtraRays))
				debug = float4(1.0, 0.1, 1.0, 0.6);
		} else if (view == kViewRtSurface) {
			// (45k) Screen Space Shadows mask: yellow = a ray-traced caster (its contact shadow is limited).
			if (VisFlags(p) & kOutRtSurface)
				debug = float4(1.0, 0.9, 0.1, 0.5);
		} else if (view == kViewDistance) {
			// (45l) Quality by distance: green = full quality, towards red = fewer extra rays and a smaller
			// filter (fall-off), blue = pixels filled from their neighbours this frame (checkerboard / half).
			if (w > 0.0) {
				const float fall = Falloff.y > Falloff.x ? saturate((dist - Falloff.x) / (Falloff.y - Falloff.x)) : 0.0;
				debug = (VisFlags(p) & kOutFilled) ? float4(0.15, 0.35, 1.0, 0.55) : float4(fall, 1.0 - fall, 0.1, 0.4);
			}
		} else if (view == kViewCharRays) {
			// (45l) Cyan = the character ray ran here (the sun ray passes a character's bounds), purple = it was blocked by one.
			if (w > 0.0) {
				const uint vf = VisFlags(p);
				if (vf & kOutCharacter)
					debug = float4(0.6, 0.2, 1.0, 0.6);
				else if (vf & kOutCharRay)
					debug = float4(0.0, 0.85, 1.0, 0.4);
			}
		}
		DebugColor[p] = debug;
	}
}
