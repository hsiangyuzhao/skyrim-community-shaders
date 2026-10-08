// (batch 39, item 6) Snow footprint / trail map maintenance, after Barré-Brisebois,
// "Deformable Snow Rendering in Batman: Arkham Origins" (GDC 2014): a top-down map around the
// player that scrolls with them, written by the actors' feet and slowly filled back in.
//
// One R32_UINT texel per TrailTexelSize x TrailTexelSize patch of ground, addressed
// toroidally: physical = (window texel + Wrap) & (MapSize - 1), so scrolling the window never
// moves data, it only clears the strip that comes into view.
//   hi 16 bits: print strength (UNORM, 0 = untouched)
//   lo 16 bits: the foot's height modulo 1024 units (UNORM)
// Strength sits in the high half so InterlockedMax keeps the deepest print together with the
// height of the foot that made it.
//
// Three entry points, chosen by define: CLEAR (a window-relative rectangle), DECAY (whole map,
// refill), STAMP (one thread group per foot print).

cbuffer SnowTrailCB : register(b0)
{
	int2 RectMin;    // CLEAR: first window texel of the rectangle
	int2 RectSize;   // CLEAR: rectangle size in texels
	int2 Wrap;       // window texel -> physical texel offset
	uint MapSize;    // texels per side, power of two
	uint DecayStep;  // DECAY: strength units (of 65535) to remove
	uint StampCount; // STAMP: number of valid stamps
	uint3 pad0;
};

struct Stamp
{
	float2 Center;  // window texel coordinates
	float2 Axis;    // unit vector along the print's length
	float2 Radii;   // semi-axes in texels (length, width)
	uint Z16;       // foot height modulo 1024, UNORM16
	float Strength; // [0,1]
};

StructuredBuffer<Stamp> Stamps : register(t0);
RWTexture2D<uint> TrailMap : register(u0);

#if defined(CLEAR)

[numthreads(8, 8, 1)] void main(uint3 id
								: SV_DispatchThreadID) {
	if (any(id.xy >= (uint2)RectSize))
		return;
	int2 w = RectMin + int2(id.xy);
	uint2 p = uint2(w + Wrap) & (MapSize - 1);
	TrailMap[p] = 0;
}

#elif defined(DECAY)

[numthreads(8, 8, 1)] void main(uint3 id
								: SV_DispatchThreadID) {
	if (any(id.xy >= MapSize))
		return;
	uint v = TrailMap[id.xy];
	uint s = v >> 16;
	s = s > DecayStep ? s - DecayStep : 0;
	TrailMap[id.xy] = s ? ((s << 16) | (v & 0xFFFF)) : 0;
}

#elif defined(STAMP)

[numthreads(8, 8, 1)] void main(uint3 gid
								: SV_GroupID, uint3 gtid
								: SV_GroupThreadID) {
	if (gid.x >= StampCount)
		return;
	Stamp st = Stamps[gid.x];
	float r = max(st.Radii.x, st.Radii.y) + 1.0;
	int2 lo = int2(floor(st.Center - r));
	int2 hi = int2(ceil(st.Center + r));
	float2 side = float2(-st.Axis.y, st.Axis.x);
	float2 invRadii = rcp(max(st.Radii, 0.25));

	for (int y = lo.y + int(gtid.y); y <= hi.y; y += 8) {
		for (int x = lo.x + int(gtid.x); x <= hi.x; x += 8) {
			if (x < 0 || y < 0 || x >= int(MapSize) || y >= int(MapSize))
				continue;
			float2 d = float2(x, y) + 0.5 - st.Center;
			float2 local = float2(dot(d, st.Axis), dot(d, side)) * invRadii;
			float e = length(local);
			// (batch 39b) Softer wall: 39a went from full depth to none over the outer half of the
			// radius (one or two texels on a human print), a step the smooth read-out then had to hide.
			float w = (1.0 - smoothstep(0.3, 1.0, e)) * st.Strength;
			if (w <= 0.0)
				continue;
			uint v = (uint(saturate(w) * 65535.0 + 0.5) << 16) | (st.Z16 & 0xFFFF);
			uint2 p = uint2(int2(x, y) + Wrap) & (MapSize - 1);
			InterlockedMax(TrailMap[p], v);
		}
	}
}

#endif
