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
// Output per triangle: kMarker | class (0 mixed, 1 opaque, 2 transparent); the CPU rejects a job with any entry missing the marker.

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
};

ConstantBuffer<BakeConstants> C : register(b0);
StructuredBuffer<uint2> AlphaUVs : register(t0);
Texture2DArray<float> AlphaLayers : register(t1);
RWStructuredBuffer<uint> Classes : register(u0);

static const uint kMixed = 0u, kOpaque = 1u, kTransparent = 2u;
static const uint kMarker = 0xC1A50000u;
static const uint kLayerSize = 256u;
static const float kFilterMargin = 2.0 / 255.0;  // fixed-point filter weights: the result may stray a little past its texels
static const float kVertexAlphaMargin = 1.0e-4;  // the trace's barycentric mix of the corner alphas (w0 = 1 - b1 - b2 in float)
static const float kThresholdMargin = 1.0e-5;    // the product's rounding
static const float kMaxUv = 4096.0;              // larger texcoords: mixed (float precision)

groupshared float gsLo[64];
groupshared float gsHi[64];

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
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
	{
		if (gi < stride) {
			gsLo[gi] = min(gsLo[gi], gsLo[gi + stride]);
			gsHi[gi] = max(gsHi[gi], gsHi[gi + stride]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi == 0u)
		Classes[C.OutBase + tri] = kMarker | ClassOf(gsLo[0], gsHi[0], vaLo, vaHi);
}