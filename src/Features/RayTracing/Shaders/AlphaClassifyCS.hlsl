// (47s) Alpha Classify (OMM-lite, DXR 1.1): sorts each triangle of an alpha-tested shape against its cut-out, once per mesh and
// material, on the ray tracer's D3D12 queue. Compiled at build time with the Windows SDK's dxc (CMakeLists.txt) and embedded in the DLL.
//
// One group of 64 threads per triangle. The trace's alpha test (TraceShadowsCS / TracePointLightsCS / TraceAOCS AlphaBlocks, copy-layer
// branch) keeps a candidate when SampleLevel(copy layer, uv, lod) x AlphaScale x vertexAlpha >= Threshold, with uv the barycentric mix of
// the three corner texcoords (half2) times the material's scale plus its offset, lod in [0, 2] (the sun without fine mips; 0 otherwise),
// trilinear filtering with wrap addressing. A bilinear sample at mip m is a convex combination of the texels whose centre lies less than
// one texel (in each axis) from the sample point; a trilinear one mixes two such samples. So over the whole triangle and every lod in
// [0, 2] the filtered alpha lies between the lowest and the highest texel of mips 0-2 whose 2 x 2-texel support square meets the triangle
// (texel space, wrapped). Opaque: even that lowest value (less a filtering margin) times the lowest vertex alpha passes the threshold;
// transparent: even the highest (plus the margin) times the highest vertex alpha does not; everything else (and any doubt) is mixed.
// The support squares are grown by a margin for the rounding of the texcoords (the trace interpolates in float).
//
// (47t) Opacity Micromaps (OmmStride != 0): a mixed triangle is also cut into 4^level micro-triangles (the OC1 order of NVAPI / the DXR
// opacity micromaps, level from its texel footprint, at most OmmMaxLevel) and each one is sorted the same way over its own part of the
// texture: opaque (state 1), transparent (state 0) or unknown (state 2, unknown-transparent: the hardware hands it to the candidate loop,
// which tests it exactly as before). 4-state, 2 bits per micro-triangle, written at OmmStride x triangle into the OMM data (u1). Each
// micro-triangle is grown before it is sorted (the hardware's choice of micro-triangle near an edge, the trace's float barycentrics).
//
// Output per triangle: kMarker | class (bits 0-1: 0 mixed, 1 opaque, 2 transparent) | OMM level << 2 (bits 2-4) | known micro-triangles
// << 5 (bits 5-15: decided without the shader; 0 = no OMM data); the CPU rejects a job with any entry missing the marker.

struct BakeConstants
{
	uint UvBase;       // first record entry of triangle 0 (AlphaUVs, 2 entries per triangle: (uv0, uv1), (uv2, vertex alphas))
	uint Triangles;
	uint Layer;        // copy layer (AlphaLayers slice)
	uint OutBase;      // first output entry
	float Threshold;   // the record's (AlphaInfo) values, exactly as the trace reads them
	float AlphaScale;
	uint GroupsX;      // groups per row of the dispatch (triangle = group.y * GroupsX + group.x)
	uint Mips;         // copy mips the trace may sample (3: lod 0-2)
	float4 UvTransform;  // xy offset, zw scale
	uint OmmStride;    // (47t) bytes of OMM data per triangle (0 = no OMM)
	uint OmmMaxLevel;  // (47t) highest subdivision level
};

ConstantBuffer<BakeConstants> C : register(b0);
StructuredBuffer<uint2> AlphaUVs : register(t0);
Texture2DArray<float> AlphaLayers : register(t1);
RWStructuredBuffer<uint> Classes : register(u0);
RWByteAddressBuffer OmmData : register(u1);

static const uint kMixed = 0u, kOpaque = 1u, kTransparent = 2u;
static const uint kMarker = 0xC1A50000u;
static const uint kLayerSize = 256u;
static const float kFilterMargin = 2.0 / 255.0;  // fixed-point filter weights: the result may stray a little past its texels
static const float kVertexAlphaMargin = 1.0e-4;  // the trace's barycentric mix of the corner alphas (w0 = 1 - b1 - b2 in float)
static const float kThresholdMargin = 1.0e-5;    // the product's rounding
static const float kMaxUv = 4096.0;              // larger texcoords: mixed (float precision)
// (47t) OMM.
static const uint kOmmTransparent = 0u, kOmmOpaque = 1u, kOmmUnknown = 2u;  // NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_STATE (2 = unknown-transparent)
static const float kOmmMicroTexels = 4.0;         // aimed micro-triangle area in texels of the coarsest mip sorted (mip 2: 64 x 64)
static const float kOmmBaryGrow = 2.0e-3;         // micro-triangles grown by this much barycentric, per axis

groupshared float gsLo[64];
groupshared float gsHi[64];
groupshared uint gsBits[64];  // (47t) the OMM data of the group's triangle (1024 micro-triangles x 2 bits at level 5)
groupshared uint gsKnown;

float2 UnpackTexcoord(uint packed)
{
	return f16tof32(uint2(packed & 0xFFFFu, packed >> 16));
}

// Does the axis-aligned square of half size h around c meet the triangle p0 p1 p2 (separating axes: the square's two, the triangle's
// three edge normals)? A degenerate triangle only gets the box test (conservative).
bool SquareMeetsTriangle(float2 c, float h, float2 p0, float2 p1, float2 p2)
{
	const float2 mn = min(p0, min(p1, p2));
	const float2 mx = max(p0, max(p1, p2));
	if (any(mx < c - h) || any(mn > c + h))
		return false;
	float2 v[3] = { p0, p1, p2 };
	[unroll] for (uint e = 0u; e < 3u; ++e)
	{
		const float2 a = v[e];
		const float2 b = v[(e + 1u) % 3u];
		const float2 o = v[(e + 2u) % 3u];
		const float2 n = float2(a.y - b.y, b.x - a.x);
		const float da = dot(n, a);
		const float dO = dot(n, o);
		const float r = h * (abs(n.x) + abs(n.y));
		const float dc = dot(n, c);
		if (dc + r < min(da, dO) || dc - r > max(da, dO))
			return false;
	}
	return true;
}

// The class from the range of filtered alpha [lo, hi] and of vertex alpha [vaLo, vaHi] (margins applied here).
uint ClassOf(float lo, float hi, float vaLo, float vaHi)
{
	if (!(lo <= hi))
		return kMixed;  // no texel (cannot happen: every triangle meets some support square)
	const float lowest = max(lo - kFilterMargin, 0.0) * C.AlphaScale * max(vaLo, 0.0);
	const float highest = (hi + kFilterMargin) * C.AlphaScale * vaHi;
	if (lowest >= C.Threshold + kThresholdMargin)
		return kOpaque;
	if (highest < C.Threshold - kThresholdMargin)
		return kTransparent;
	return kMixed;
}

// ---- (47t) OC1 micro-triangle order (the "bird curve" of VK_EXT_opacity_micromap / NVAPI; verified on the GPU by the 47t probe).
uint ExtractEvenBits(uint x)
{
	x &= 0x55555555u;
	x = (x | (x >> 1)) & 0x33333333u;
	x = (x | (x >> 2)) & 0x0f0f0f0fu;
	x = (x | (x >> 4)) & 0x00ff00ffu;
	x = (x | (x >> 8)) & 0x0000ffffu;
	return x;
}

uint PrefixEor(uint x)
{
	x ^= x >> 1;
	x ^= x >> 2;
	x ^= x >> 4;
	x ^= x >> 8;
	return x;
}

// The corners of micro-triangle `index` of `level` as barycentrics (x = weight of vertex 1, y = weight of vertex 2).
void MicroCorners(uint index, uint level, out float2 b0, out float2 b1, out float2 b2)
{
	if (level == 0u) {
		b0 = float2(0, 0);
		b1 = float2(1, 0);
		b2 = float2(0, 1);
		return;
	}
	const uint e0 = ExtractEvenBits(index);
	const uint e1 = ExtractEvenBits(index >> 1);
	const uint fx = PrefixEor(e0);
	const uint fy = PrefixEor(e0 & ~e1);
	const uint t = fy ^ e1;
	const uint mask = (1u << level) - 1u;
	uint iu = ((fx & ~t) | (e0 & ~t) | (~e0 & ~fx & t)) & mask;
	uint iv = (fy ^ e0) & mask;
	const uint iw = ((~fx & ~t) | (e0 & ~t) | (~e0 & fx & t)) & mask;
	const bool upright = ((iu ^ iv ^ iw) & 1u) != 0u;
	if (!upright) {
		iu += 1u;
		iv += 1u;
	}
	const float scale = 1.0 / float(1u << level);
	const float2 p = float2(float(iu), float(iv)) * scale;
	const float d = upright ? scale : -scale;
	b0 = p;
	b1 = p + float2(d, 0.0);
	b2 = p + float2(0.0, d);
}

// The subdivision level of a triangle with these texcoords: micro-triangles of about kOmmMicroTexels texels of mip 2 (finer ones are
// mostly unknown: the support squares of mip 2 are 4 texels of mip 0 wide). The CPU's estimate (BackendClassify OmmLevelOf) is the
// same formula; this one is clamped to the CPU's maximum, which sized the data.
uint OmmLevel(float2 uv0, float2 uv1, float2 uv2)
{
	const float2 e1 = (uv1 - uv0) * 64.0;
	const float2 e2 = (uv2 - uv0) * 64.0;
	const float area = 0.5 * abs(e1.x * e2.y - e1.y * e2.x);
	if (!(area > kOmmMicroTexels))
		return 0u;
	const float level = round(0.5 * log2(area / kOmmMicroTexels));
	return uint(clamp(level, 0.0, float(C.OmmMaxLevel)));
}

// The state of the micro-triangle q0 q1 q2 (texcoords) for the texels of mips 0..mips-1 its grown support meets; ga: the growth of its
// support in texcoord units (the barycentric growth over the triangle's longest edge), va: its vertex alpha range (margins applied).
// Coarse mips first, stopping as soon as it is unknown; a micro-triangle over more than one repeat is unknown at once.
uint MicroState(float2 q0, float2 q1, float2 q2, float ga, float maxAbs, uint mips, float vaLo, float vaHi)
{
	float lo = 1.0e30;
	float hi = -1.0e30;
	for (uint step = 0u; step < mips; ++step) {
		const uint mip = mips - 1u - step;
		const int s = int(kLayerSize >> mip);
		const float fs = float(s);
		const float2 p0 = q0 * fs - 0.5;
		const float2 p1 = q1 * fs - 0.5;
		const float2 p2 = q2 * fs - 0.5;
		const float h = 1.0 + 0.03 + 5.0e-7 * maxAbs * fs + ga * fs;
		const float2 mn = min(p0, min(p1, p2));
		const float2 mx = max(p0, max(p1, p2));
		const float2 origin = floor(mn);
		const float2 r0 = p0 - origin, r1 = p1 - origin, r2 = p2 - origin;
		const int2 tMin = int2(ceil(mn - origin - h));
		const int2 tMax = int2(floor(mx - origin + h));
		const int2 extent = tMax - tMin + 1;
		if (any(extent > s + 2))
			return kOmmUnknown;
		const int2 base = int2(origin);
		const uint width = uint(max(extent.x, 1));
		const uint count = uint(max(extent.x, 0) * max(extent.y, 0));
		for (uint k = 0u; k < count; ++k) {
			int2 t = int2(int(k % width), int(k / width)) + tMin;
			if (!SquareMeetsTriangle(float2(t), h, r0, r1, r2))
				continue;
			t = ((base + t) % s + s) % s;  // wrap addressing
			const float a = AlphaLayers.Load(int4(t, int(C.Layer), int(mip)));
			lo = min(lo, a);
			hi = max(hi, a);
			if (ClassOf(lo, hi, vaLo, vaHi) == kMixed && lo <= hi)
				return kOmmUnknown;
		}
	}
	const uint c = ClassOf(lo, hi, vaLo, vaHi);
	return c == kOpaque ? kOmmOpaque : (c == kTransparent ? kOmmTransparent : kOmmUnknown);
}

[numthreads(64, 1, 1)] void main(uint3 gid : SV_GroupID, uint gi : SV_GroupIndex)
{
	const uint tri = gid.y * C.GroupsX + gid.x;
	if (tri >= C.Triangles)
		return;  // the whole group

	const uint2 c0 = AlphaUVs[C.UvBase + tri * 2u];
	const uint2 c1 = AlphaUVs[C.UvBase + tri * 2u + 1u];
	float2 uv[3];
	uv[0] = UnpackTexcoord(c0.x) * C.UvTransform.zw + C.UvTransform.xy;
	uv[1] = UnpackTexcoord(c0.y) * C.UvTransform.zw + C.UvTransform.xy;
	uv[2] = UnpackTexcoord(c1.x) * C.UvTransform.zw + C.UvTransform.xy;
	const float3 cornerAlpha = float3(uint3(c1.y, c1.y >> 8u, c1.y >> 16u) & 0xFFu) / 255.0;
	const float vaLo = min(cornerAlpha.x, min(cornerAlpha.y, cornerAlpha.z)) - kVertexAlphaMargin;
	const float vaHi = max(cornerAlpha.x, max(cornerAlpha.y, cornerAlpha.z)) + kVertexAlphaMargin;

	bool valid = isfinite(C.Threshold) && isfinite(C.AlphaScale) && C.AlphaScale >= 0.0;
	float maxAbs = 0.0;
	[unroll] for (uint i = 0u; i < 3u; ++i)
	{
		valid = valid && all(isfinite(uv[i]));
		maxAbs = max(maxAbs, max(abs(uv[i].x), abs(uv[i].y)));
	}
	valid = valid && maxAbs <= kMaxUv;
	if (!valid) {
		if (gi == 0u)
			Classes[C.OutBase + tri] = kMarker | kMixed;
		return;  // the whole group (the same values in every thread)
	}

	float lo = 1.0e30;
	float hi = -1.0e30;
	const uint mips = min(C.Mips, 5u);
	// Coarse mips first, and a wave stops as soon as the texels its lanes read already make the triangle mixed (more texels only
	// widen the range, so the group's result is mixed then too): most mixed triangles cost a fraction of a full scan.
	bool decided = false;
	for (uint step = 0u; step < mips && !decided; ++step) {
		const uint mip = mips - 1u - step;
		const int s = int(kLayerSize >> mip);
		const float fs = float(s);
		// Texel-centre space of this mip: texel t's centre is at t; the bilinear sample at x reads the texels with |x - t| < 1.
		const float2 p0 = uv[0] * fs - 0.5;
		const float2 p1 = uv[1] * fs - 0.5;
		const float2 p2 = uv[2] * fs - 0.5;
		const float h = 1.0 + 0.03 + 5.0e-7 * maxAbs * fs;
		const float2 mn = min(p0, min(p1, p2));
		const float2 mx = max(p0, max(p1, p2));
		// Relative to a whole texel near the triangle (exact shift): small coordinates for the overlap test.
		const float2 origin = floor(mn);
		const float2 q0 = p0 - origin, q1 = p1 - origin, q2 = p2 - origin;
		const int2 tMin = int2(ceil(mn - origin - h));
		const int2 tMax = int2(floor(mx - origin + h));
		const int2 extent = tMax - tMin + 1;
		const int2 base = int2(origin);
		// More than one repeat of the texture: every texel of the mip (conservative).
		const bool whole = any(extent > s + 2);
		const uint count = whole ? uint(s * s) : uint(max(extent.x, 0) * max(extent.y, 0));
		const uint width = whole ? uint(s) : uint(max(extent.x, 1));
		uint round = 0u;
		for (uint k = gi; k < count; k += 64u) {
			int2 t = int2(int(k % width), int(k / width));
			bool meets = true;
			if (!whole) {
				t += tMin;
				meets = SquareMeetsTriangle(float2(t), h, q0, q1, q2);
				t = ((base + t) % s + s) % s;  // wrap addressing
			}
			if (meets) {
				const float a = AlphaLayers.Load(int4(t, int(C.Layer), int(mip)));
				lo = min(lo, a);
				hi = max(hi, a);
			}
			if ((++round & 7u) == 0u && ClassOf(WaveActiveMin(lo), WaveActiveMax(hi), vaLo, vaHi) == kMixed && WaveActiveMin(lo) <= WaveActiveMax(hi)) {
				decided = true;
				break;
			}
		}
		if (!decided && ClassOf(WaveActiveMin(lo), WaveActiveMax(hi), vaLo, vaHi) == kMixed && WaveActiveMin(lo) <= WaveActiveMax(hi))
			decided = true;
	}

	gsLo[gi] = lo;
	gsHi[gi] = hi;
	gsBits[gi] = 0u;
	if (gi == 0u)
		gsKnown = 0u;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (gi < stride) {
			gsLo[gi] = min(gsLo[gi], gsLo[gi + stride]);
			gsHi[gi] = max(gsHi[gi], gsHi[gi + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	const uint cls = ClassOf(gsLo[0], gsHi[0], vaLo, vaHi);

	// (47t) Opacity Micromaps of a mixed triangle (the same decision in every thread of the group).
	uint level = 0u;
	if (C.OmmStride != 0u && cls == kMixed) {
		level = OmmLevel(uv[0], uv[1], uv[2]);
		if (level > 0u) {
			const float2 e1 = uv[1] - uv[0];
			const float2 e2 = uv[2] - uv[0];
			const float2 e3 = uv[2] - uv[1];
			// The barycentric growth (both axes) over the longest edge, in texcoord units.
			const float ga = 2.0 * kOmmBaryGrow * sqrt(max(dot(e1, e1), max(dot(e2, e2), dot(e3, e3))));
			const float vaSpread = max(abs(cornerAlpha.x - cornerAlpha.y), max(abs(cornerAlpha.x - cornerAlpha.z), abs(cornerAlpha.y - cornerAlpha.z)));
			const float vaGrow = kVertexAlphaMargin + 2.0 * kOmmBaryGrow * vaSpread;
			const uint micros = 1u << (2u * level);
			for (uint m = gi; m < micros; m += 64u) {
				float2 b0, b1, b2;
				MicroCorners(m, level, b0, b1, b2);
				const float2 q0 = uv[0] + e1 * b0.x + e2 * b0.y;
				const float2 q1 = uv[0] + e1 * b1.x + e2 * b1.y;
				const float2 q2 = uv[0] + e1 * b2.x + e2 * b2.y;
				const float a0 = cornerAlpha.x + (cornerAlpha.y - cornerAlpha.x) * b0.x + (cornerAlpha.z - cornerAlpha.x) * b0.y;
				const float a1 = cornerAlpha.x + (cornerAlpha.y - cornerAlpha.x) * b1.x + (cornerAlpha.z - cornerAlpha.x) * b1.y;
				const float a2 = cornerAlpha.x + (cornerAlpha.y - cornerAlpha.x) * b2.x + (cornerAlpha.z - cornerAlpha.x) * b2.y;
				const float mLo = min(a0, min(a1, a2)) - vaGrow;
				const float mHi = max(a0, max(a1, a2)) + vaGrow;
				const uint state = MicroState(q0, q1, q2, ga, maxAbs, mips, mLo, mHi);
				if (state != kOmmUnknown)
					InterlockedAdd(gsKnown, 1u);
				InterlockedOr(gsBits[m >> 4], state << ((m & 15u) * 2u));
			}
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (level > 0u) {
		const uint words = max((1u << (2u * level)) / 16u, 1u);
		if (gi < words)
			OmmData.Store(tri * C.OmmStride + gi * 4u, gsBits[gi]);
	}
	if (gi == 0u)
		Classes[C.OutBase + tri] = kMarker | cls | (level << 2) | (min(gsKnown, 2047u) << 5);
}
