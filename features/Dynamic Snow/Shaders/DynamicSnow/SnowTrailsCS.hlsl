// (batch 39, item 6) Snow footprint / trail map maintenance, after Barré-Brisebois,
// "Deformable Snow Rendering in Batman: Arkham Origins" (GDC 2014): a top-down map around the
// player that scrolls with them, written by the actors' feet and slowly filled back in.
//
// One R32_UINT texel per TrailTexelSize x TrailTexelSize patch of ground, addressed
// toroidally: physical = (window texel + Wrap) & (MapSize - 1), so scrolling the window never
// moves data, it only clears the strip that comes into view.
//   hi 16 bits: print code (batch 39c). 0 = untouched; 1..32767 = a pushed-up rim, height =
//               code / 32767; 32768..65535 = a dent, depth = (code - 32768) / 32767.
//   lo 16 bits: the foot's height modulo 1024 units (UNORM)
// The code sits in the high half so InterlockedMax keeps the deepest dent (any dent beats any
// rim) together with the height of the foot that made it.
//
// Three entry points, chosen by define: CLEAR (a window-relative rectangle), DECAY (whole map,
// refill), STAMP (one thread group per print).

cbuffer SnowTrailCB : register(b0)
{
	int2 RectMin;    // CLEAR: first window texel of the rectangle
	int2 RectSize;   // CLEAR: rectangle size in texels
	int2 Wrap;       // window texel -> physical texel offset
	uint MapSize;    // texels per side, power of two
	uint DecayStep;  // DECAY: code units (of 32767) to remove
	uint StampCount; // STAMP: number of valid stamps
	uint ShapeCount; // STAMP: slices in StampShapes (0 = none loaded)
	uint2 pad0;
};

// Mirrors DynamicSnow::Stamp.
struct Stamp
{
	float2 Center;   // window texel coordinates (capsule: one end)
	float2 Axis;     // unit vector along the print's length (towards the toe)
	float2 Radii;    // ellipse: semi-axes (length, width); shape: half extents (length, width); capsule: radius
	uint Z16;        // foot height modulo 1024, UNORM16
	float Strength;  // [0,1]
	float2 End;      // capsule: the other end, window texels
	uint Shape;      // kShapeEllipse, kShapeCapsule, or a StampShapes slice (bit 16: mirror sideways)
	float Rim;       // ellipse / capsule: rim height relative to the depth
};

static const uint kShapeEllipse = 0xFFFF;
static const uint kShapeCapsule = 0xFFFE;

StructuredBuffer<Stamp> Stamps : register(t0);
// (batch 39c) Print shapes from the installed footprint textures, one per slice: signed height,
// -1 = the bottom of the print, 0 = untouched snow, > 0 = the rim pushed up around it.
Texture2DArray<float> StampShapes : register(t1);
SamplerState LinearClamp : register(s0);
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
	uint code = v >> 16;
	if (code >= 32768) {
		// Dent: fill back in; a filled dent is untouched snow, not a rim.
		uint d = code - 32768;
		d = d > DecayStep ? d - DecayStep : 0;
		code = d ? 32768 + d : 0;
	} else {
		code = code > DecayStep ? code - DecayStep : 0;
	}
	TrailMap[id.xy] = code ? ((code << 16) | (v & 0xFFFF)) : 0;
}

#elif defined(STAMP)

uint EncodeHeight(float h)
{
	// h < 0: dent of depth -h; h > 0: rim of height h.
	return h < 0.0 ? 32768 + uint(saturate(-h) * 32767.0 + 0.5) : uint(saturate(h) * 32767.0 + 0.5);
}

[numthreads(8, 8, 1)] void main(uint3 gid
								: SV_GroupID, uint3 gtid
								: SV_GroupThreadID) {
	if (gid.x >= StampCount)
		return;
	Stamp st = Stamps[gid.x];
	const uint shape = st.Shape & 0xFFFF;
	const bool capsule = shape == kShapeCapsule;
	const bool textured = shape < kShapeCapsule && shape < ShapeCount;

	// Bounds: the rim of procedural prints reaches 1.5x the radius.
	float reach = max(st.Radii.x, st.Radii.y) * (textured ? 1.0 : 1.5) + 1.0;
	float2 lo2 = capsule ? min(st.Center, st.End) : st.Center;
	float2 hi2 = capsule ? max(st.Center, st.End) : st.Center;
	int2 lo = int2(floor(lo2 - reach));
	int2 hi = int2(ceil(hi2 + reach));
	float2 side = float2(-st.Axis.y, st.Axis.x);
	float2 invRadii = rcp(max(st.Radii, 0.25));

	// Shapes: sample the mip whose texels match the trail map's.
	float shapeLod = 0.0;
	if (textured) {
		float w, h, slices, mips;
		StampShapes.GetDimensions(0, w, h, slices, mips);
		shapeLod = clamp(log2(w / max(2.0 * st.Radii.y, 1.0)), 0.0, mips - 1.0);
	}

	for (int y = lo.y + int(gtid.y); y <= hi.y; y += 8) {
		for (int x = lo.x + int(gtid.x); x <= hi.x; x += 8) {
			if (x < 0 || y < 0 || x >= int(MapSize) || y >= int(MapSize))
				continue;
			float2 pos = float2(x, y) + 0.5;
			float height;  // signed: < 0 dent, > 0 rim
			if (textured) {
				float2 d = pos - st.Center;
				// Texture v runs heel -> toe (the image's top is the heel), u to the walker's left.
				float2 local = float2(dot(d, side) * invRadii.y, dot(d, st.Axis) * invRadii.x);
				if (st.Shape & 0x10000)
					local.x = -local.x;
				if (any(abs(local) >= 1.0))
					continue;
				float2 uv = local * 0.5 + 0.5;
				height = StampShapes.SampleLevel(LinearClamp, float3(uv, float(shape)), shapeLod);
			} else {
				float e;
				if (capsule) {
					float2 ab = st.End - st.Center;
					float t = saturate(dot(pos - st.Center, ab) / max(dot(ab, ab), 1e-4));
					e = length(pos - (st.Center + ab * t)) * invRadii.x;
				} else {
					float2 d = pos - st.Center;
					e = length(float2(dot(d, st.Axis), dot(d, side)) * invRadii);
				}
				// (batch 39b) Soft wall. (39c) Plus a low rim just outside it.
				float dent = 1.0 - smoothstep(0.3, 1.0, e);
				float rim = st.Rim * smoothstep(0.85, 1.1, e) * (1.0 - smoothstep(1.1, 1.5, e));
				height = rim - dent;
			}
			height *= st.Strength;
			if (abs(height) < 1.0 / 32767.0)
				continue;
			uint v = (EncodeHeight(height) << 16) | (st.Z16 & 0xFFFF);
			uint2 p = uint2(int2(x, y) + Wrap) & (MapSize - 1);
			InterlockedMax(TrailMap[p], v);
		}
	}
}

#endif
