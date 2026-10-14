// (45k) Ray Tracing, Denoiser "Penumbra" step 1: per 8x8 tile of the traced visibility
//   x = average blocker distance of the tile's shadowed rays (world units, 0 = none)
//   y = number of pixels that found a blocker
//   z / w = lowest / highest visibility of the tile's traced pixels (2 / -1 = none)
// The spatial filter reads the 3x3 tiles around a pixel: uniform tiles are not filtered at all,
// the others get a radius from the penumbra width the blockers' distance gives.

#include "RayTracing/Common.hlsli"

Texture2D<unorm float4> Visibility : register(t0);  // r visibility, z blocker distance (sqrt(t / range))
Texture2D<float> DepthMask : register(t1);          // signed depth, see PrepareCS
RWTexture2D<float4> Tiles : register(u0);

groupshared float gsSum[64];
groupshared float gsCount[64];
groupshared float gsMin[64];
groupshared float gsMax[64];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
	const int2 p = int2(dtid.xy);
	const uint flags = asuint(Band.z);
	float sum = 0.0, count = 0.0, lo = 2.0, hi = -1.0;
	if (all(dtid.xy < Size.xy)) {
		const float sd = DepthMask[p];
		const float d = abs(sd);
		const bool skipped = (flags & kFlagTraceSkipped) && sd < 0.0;
		if (!RTIsSky(d) && !skipped) {
			const float3 P = RTPosition(p, d);
			if (P.x >= Rect.x && P.y >= Rect.y && P.x <= Rect.z && P.y <= Rect.w) {
				const float4 v = Visibility[p];
				lo = v.x;
				hi = v.x;
				if ((flags & kFlagBlockerDist) && v.z > 0.0) {
					sum = v.z * v.z * kBlockerRange;
					count = 1.0;
				}
			}
		}
	}
	gsSum[gi] = sum;
	gsCount[gi] = count;
	gsMin[gi] = lo;
	gsMax[gi] = hi;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint s = 32u; s > 0u; s >>= 1u)
	{
		if (gi < s) {
			gsSum[gi] += gsSum[gi + s];
			gsCount[gi] += gsCount[gi + s];
			gsMin[gi] = min(gsMin[gi], gsMin[gi + s]);
			gsMax[gi] = max(gsMax[gi], gsMax[gi + s]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi == 0u) {
		const float c = gsCount[0];
		Tiles[gid.xy] = float4(c > 0.0 ? gsSum[0] / c : 0.0, c, gsMin[0], gsMax[0]);
	}
}
