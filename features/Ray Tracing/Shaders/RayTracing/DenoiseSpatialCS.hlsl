// (45k) Ray Tracing, Denoiser "Penumbra" steps 2 and 3: a separable, geometry-aware blur of this
// frame's traced visibility whose radius follows the penumbra width (built twice: horizontal,
// and with DENOISE_VERTICAL).
//
//   radius (px) = 0.5 * penumbra width / pixel footprint,  penumbra width = 2 * blocker distance * tan(sun cone)
//
// The blocker distance is the average over the 3x3 tiles around the pixel (DenoiseTileCS), so the
// radius is smooth; where those tiles are flat (deep shadow, open light) nothing is filtered,
// and contact shadows (blocker near the receiver) keep a radius below a pixel: they stay sharp.
// Weights: a Gaussian over the radius times a plane-distance test against the depth normal, so
// shadows do not bleed across depth edges. Sky, untraced and skipped pixels are left out.
//   Output: x visibility, y radius in pixels (the vertical pass reuses it; the debug view shows it).
//
// (45l) With DENOISE_HALF (two more variants) it runs on the half-resolution input the tile pass
// wrote (one value per 2x2, -1 = none): a quarter of the pixels, radius and taps in half pixels;
// CombineCS upsamples it. The taps per side come from Extra.w (quality), the radius shrinks with
// distance (Falloff) when kFlagFalloffDenoise is set; the flat-tile early-out is tested first.

#include "RayTracing/Common.hlsli"

#if defined(DENOISE_VERTICAL) || defined(DENOISE_HALF)
Texture2D<float2> Source : register(t0);  // the horizontal pass, or (half, horizontal) the tile pass's half input
#else
Texture2D<unorm float4> Source : register(t0);  // Visibility
#endif
Texture2D<float> DepthMask : register(t1);
Texture2D<float4> Tiles : register(t2);
RWTexture2D<float2> Output : register(u0);

#if defined(DENOISE_HALF)
static const int kScale = 2;
#else
static const int kScale = 1;
#endif

// Full-resolution pixel of a filter sample (its top-left pixel at half resolution).
int2 FullPixel(int2 q)
{
	return q * kScale;
}

bool Traced(int2 q, out float3 Pq)
{
	Pq = 0.0;
#if defined(DENOISE_HALF)
	if (Source[q].x < 0.0)
		return false;
#endif
	const int2 f = FullPixel(q);
	const float sd = DepthMask[f];
	const float d = abs(sd);
#if defined(DENOISE_HALF)
	// The half sample stands for its traced pixels; its top-left one may be the skipped / sky one.
	if (RTIsSky(d))
		return false;
	Pq = RTPosition(f, d);
	return true;
#else
	if (RTIsSky(d) || ((asuint(Band.z) & kFlagTraceSkipped) && sd < 0.0))
		return false;
	Pq = RTPosition(f, d);
	return Pq.x >= Rect.x && Pq.y >= Rect.y && Pq.x <= Rect.z && Pq.y <= Rect.w;
#endif
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	const int2 size = int2((Size.xy + uint(kScale - 1)) / uint(kScale));
	if (any(p >= size))
		return;
	const float center = Source[p].x;
	const int2 maxP = size - 1;
	const uint flags = asuint(Band.z);

#if defined(DENOISE_VERTICAL)
	const float radius0 = Source[p].y;
	const int2 axis = int2(0, 1);
	if (radius0 < 0.5) {
		Output[p] = float2(center, radius0);
		return;
	}
#else
	const int2 axis = int2(1, 0);
	// (45l) Uniform tiles first (cheap): nothing to filter, no position needed.
	float tileBlocker = 0.0;
	bool flat = true;
	{
		const int2 tile = int2(uint2(FullPixel(p)) / 8u);
		const int2 maxTile = int2((Size.xy + 7u) / 8u) - 1;
		float sum = 0.0, count = 0.0, lo = 2.0, hi = -1.0;
		[unroll] for (int ty = -1; ty <= 1; ++ty)
		{
			[unroll] for (int tx = -1; tx <= 1; ++tx)
			{
				const float4 t = Tiles[clamp(tile + int2(tx, ty), int2(0, 0), maxTile)];
				sum += t.x * t.y;
				count += t.y;
				lo = min(lo, t.z);
				hi = max(hi, t.w);
			}
		}
		flat = !(hi - lo > 0.02 && count > 0.0);
		tileBlocker = count > 0.0 ? sum / count : 0.0;
	}
	if (flat) {
		Output[p] = float2(center, 0.0);
		return;
	}
#endif
	float3 P;
	if (!Traced(p, P)) {
		Output[p] = float2(center, 0.0);
		return;
	}
	const float dist = length(P);

#if defined(DENOISE_VERTICAL)
	const float radius = radius0;
#else
	// Penumbra radius (in filter pixels) from the 3x3 tiles' blocker distance.
	float radius;
	{
		const float width = 2.0 * tileBlocker * Denoise.x;
		const float3 Px = RTPosition(FullPixel(p) + int2(kScale, 0), abs(DepthMask[FullPixel(p)]));
		const float footprint = max(length(Px - P), 1e-3);
		float maxRadius = Denoise.y / float(kScale);
		if (flags & kFlagFalloffDenoise)
			maxRadius *= lerp(1.0, Falloff.z, saturate((dist - Falloff.x) / max(Falloff.y - Falloff.x, 1.0)));
		radius = clamp(0.5 * width / footprint, 0.0, maxRadius);
	}
	if (radius < 0.5) {
		Output[p] = float2(center, radius);
		return;
	}
#endif

	// Plane of this pixel (depth normal, smaller step per axis as in the trace), at full resolution.
	float3 N;
	{
		const int2 f = FullPixel(p);
		const int2 maxF = int2(Size.xy) - 1;
		const int2 pl = int2(max(f.x - 1, 0), f.y), pr = int2(min(f.x + 1, maxF.x), f.y);
		const int2 pu = int2(f.x, max(f.y - 1, 0)), pd = int2(f.x, min(f.y + 1, maxF.y));
		const float d0 = abs(DepthMask[f]);
		const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
		const float3 dx = abs(dl - d0) < abs(dr - d0) ? P - RTPosition(pl, dl) : RTPosition(pr, dr) - P;
		const float3 dy = abs(du - d0) < abs(dd - d0) ? P - RTPosition(pu, du) : RTPosition(pd, dd) - P;
		N = cross(dy, dx);
		const float len = length(N);
		N = len > 1e-12 ? N / len : (dist > 0.0 ? -P / dist : float3(0, 0, 1));
	}
	const float planeTolerance = dist * 0.01 + 2.0 * float(kScale);

	const int maxTaps = clamp(int(Extra.w), 1, 8);
	const int taps = min(int(ceil(radius)), maxTaps);
	const float stepPx = radius / float(taps);
	float sum = center, weights = 1.0;
	[loop] for (int k = -taps; k <= taps; ++k)
	{
		if (k == 0)
			continue;
		const float o = float(k) * stepPx;
		const int2 q = clamp(p + axis * int(round(o)), int2(0, 0), maxP);
		float3 Pq;
		if (!Traced(q, Pq))
			continue;
		const float x = o / radius;
		const float w = exp(-2.0 * x * x) * exp(-abs(dot(N, Pq - P)) / planeTolerance);
		sum += Source[q].x * w;
		weights += w;
	}
	Output[p] = float2(sum / weights, radius);
}
