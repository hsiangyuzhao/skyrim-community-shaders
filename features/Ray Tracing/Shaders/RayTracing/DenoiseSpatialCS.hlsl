// (45k) Ray Tracing, Denoiser "Penumbra" steps 2 and 3: a separable, geometry-aware blur of this
// frame's traced visibility whose radius follows the penumbra width (built twice: horizontal,
// and with DENOISE_VERTICAL).
//
//   radius (px) = 0.5 * penumbra width / pixel footprint,  penumbra width = 2 * blocker distance * tan(sun cone)
//
// The blocker distance is the average over the 3x3 tiles around the pixel (DenoiseTileCS), so the
// radius is smooth; where those tiles are uniform (deep shadow, open light) nothing is filtered,
// and contact shadows (blocker near the receiver) keep a radius below a pixel: they stay sharp.
// Weights: a Gaussian over the radius times a plane-distance test against the depth normal, so
// shadows do not bleed across depth edges. Sky, untraced and skipped pixels are left out.
//   Output: x visibility, y radius in pixels (the vertical pass reuses it; the debug view shows it).

#include "RayTracing/Common.hlsli"

#if defined(DENOISE_VERTICAL)
Texture2D<float2> Source : register(t0);  // the horizontal pass
#else
Texture2D<unorm float4> Source : register(t0);  // Visibility
#endif
Texture2D<float> DepthMask : register(t1);
Texture2D<float4> Tiles : register(t2);
RWTexture2D<float2> Output : register(u0);

static const int kMaxTapsPerSide = 6;

bool Traced(int2 q, out float3 Pq)
{
	Pq = 0.0;
	const float sd = DepthMask[q];
	const float d = abs(sd);
	if (RTIsSky(d) || ((asuint(Band.z) & kFlagTraceSkipped) && sd < 0.0))
		return false;
	Pq = RTPosition(q, d);
	return Pq.x >= Rect.x && Pq.y >= Rect.y && Pq.x <= Rect.z && Pq.y <= Rect.w;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;
	const float center = Source[p].x;
	float3 P;
	if (!Traced(p, P)) {
		Output[p] = float2(center, 0.0);
		return;
	}
	const int2 maxP = int2(Size.xy) - 1;

#if defined(DENOISE_VERTICAL)
	const float radius = Source[p].y;
	const int2 axis = int2(0, 1);
#else
	const int2 axis = int2(1, 0);
	// Penumbra radius from the 3x3 tiles around this pixel.
	float radius = 0.0;
	{
		const int2 tile = int2(dtid.xy / 8u);
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
		if (hi - lo > 0.02 && count > 0.0) {
			const float blocker = sum / count;
			const float width = 2.0 * blocker * Denoise.x;
			const float3 Px = RTPosition(p + int2(1, 0), abs(DepthMask[p]));
			const float footprint = max(length(Px - P), 1e-3);
			radius = clamp(0.5 * width / footprint, 0.0, Denoise.y);
		}
	}
#endif
	if (radius < 0.5) {
		Output[p] = float2(center, radius);
		return;
	}

	// Plane of this pixel (depth normal, smaller step per axis as in the trace).
	const float dist = length(P);
	float3 N;
	{
		const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
		const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
		const float d0 = abs(DepthMask[p]);
		const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
		const float3 dx = abs(dl - d0) < abs(dr - d0) ? P - RTPosition(pl, dl) : RTPosition(pr, dr) - P;
		const float3 dy = abs(du - d0) < abs(dd - d0) ? P - RTPosition(pu, du) : RTPosition(pd, dd) - P;
		N = cross(dy, dx);
		const float len = length(N);
		N = len > 1e-12 ? N / len : (dist > 0.0 ? -P / dist : float3(0, 0, 1));
	}
	const float planeTolerance = dist * 0.01 + 2.0;

	const int taps = min(int(ceil(radius)), kMaxTapsPerSide);
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
