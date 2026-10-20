// (45k) Ray Tracing, Denoiser "Penumbra" step 1: per 8x8 tile of the traced visibility
//   x = average blocker distance of the tile's shadowed rays (world units, 0 = none)
//   y = number of pixels that found a blocker
//   z / w = lowest / highest visibility of the tile's traced pixels (2 / -1 = none)
// The spatial filter reads the 3x3 tiles around a pixel: uniform tiles are not filtered at all,
// the others get a radius from the penumbra width the blockers' distance gives.
// (45l, kFlagDenoiseHalf) Also the half-resolution input of the filter: per 2x2 pixels the average
// visibility of the traced ones (x, -1 = none) and how many there were (y).

#include "RayTracing/Common.hlsli"

#if defined(SSS_CASTER_TILES)
// (47r) "Screen Space Shadows: Tile Mask for RT Casters": per 8x8 tile of the visibility texture (its whole size: the contact-shadow pass
// addresses it so) bit 0 = some pixel has kOutRtSurface (a traced caster), bit 1 = some pixel has not. bend_sss_gpu IsRtCaster answers
// a tile of only one kind without reading the mask (exact: the same bit, read the same way).
Texture2D<unorm float4> Visibility : register(t0);
RWTexture2D<uint> CasterTiles : register(u0);
groupshared uint gsKinds;

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint gi : SV_GroupIndex) {
	if (gi == 0u)
		gsKinds = 0u;
	GroupMemoryBarrierWithGroupSync();
	uint w, h;
	Visibility.GetDimensions(w, h);
	if (all(dtid.xy < uint2(w, h))) {
		const bool caster = (uint(Visibility.Load(int3(dtid.xy, 0)).w * 255.0 + 0.5) & kOutRtSurface) != 0u;
		InterlockedOr(gsKinds, caster ? 1u : 2u);
	}
	GroupMemoryBarrierWithGroupSync();
	if (gi == 0u)
		CasterTiles[gid.xy] = gsKinds;
}
#else
Texture2D<unorm float4> Visibility : register(t0);  // r visibility, z blocker distance (sqrt(t / range))
Texture2D<float> DepthMask : register(t1);          // signed depth, see PrepareCS
RWTexture2D<float4> Tiles : register(u0);
RWTexture2D<float2> Half : register(u1);            // (45l)

groupshared float gsSum[64];
groupshared float gsCount[64];
groupshared float gsMin[64];
groupshared float gsMax[64];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
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
	[branch] if (flags & kFlagDenoiseHalf)
	{
		// One thread per 2x2: the traced pixels' average (gsMin holds the pixel's own visibility, 2 = none).
		if (((gtid.x | gtid.y) & 1u) == 0u) {
			float hv = 0.0, hn = 0.0;
			[unroll] for (uint k = 0; k < 4; ++k)
			{
				const float v = gsMin[gi + (k & 1u) + (k >> 1u) * 8u];
				if (v <= 1.0) {
					hv += v;
					hn += 1.0;
				}
			}
			const uint2 h = dtid.xy / 2u;
			if (all(h < (Size.xy + 1u) / 2u))
				Half[h] = float2(hn > 0.0 ? hv / hn : -1.0, hn);
		}
	}
	GroupMemoryBarrierWithGroupSync();  // the reduction below overwrites gsMin
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
#endif
