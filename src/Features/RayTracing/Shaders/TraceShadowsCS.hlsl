// (45, RT prototype) Ray-traced sun visibility, inline RayQuery (DXR 1.1), cs_6_5.
//
// Compiled at build time with the Windows SDK's dxc (see CMakeLists.txt, "Ray Tracing DXIL")
// and embedded in the DLL; it runs on the feature's own D3D12 queue, never on the game's D3D11
// device. Inputs are D3D11 textures shared with D3D12:
//   t1 DepthMask (R32_FLOAT): |value| = the game's depth after the depth prepass (the value the
//      engine's shadow mask pass read); negative where the engine's sun shadow mask is already 0.
// Output u0 (R8G8B8A8_UNORM): .x = sun visibility (1 lit, 0 shadow); .y / .z = coverage class and
// ray-traced-vs-game depth difference (only computed for the coverage debug views, see below).
//
// Positions are camera-relative (the game's CameraPosAdjust space): the TLAS is built with
// instance translations relative to the same camera position, so all ray math stays near 0.
//
// (45h) Built twice: as is (every instance opaque; the pipeline of every frame without
// alpha-tested instances, identical to 45g) and with RT_ALPHA (non-opaque alpha-tested instances
// are tested against their cut-out, see AlphaBlocks).

struct TraceConstants
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse (jittered, as the shadow mask used)
	float4 ToSun;                    // xyz direction to the light, w = tan(cone half angle)
	float4 Params;                   // x rays per pixel, y frame index, z flags (asuint), w max ray length
	float4 RenderSize;               // xy render size in pixels, zw 1/size
	float4 Rect;                     // camera-relative xy min / max of the traced area
	float4 Bias;                     // x constant bias, y bias per unit of distance, z coverage probe length
};

ConstantBuffer<TraceConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthMask : register(t1);
RWTexture2D<unorm float4> Output : register(u0);

static const uint kFlagSkipShadowed = 1u;  // engine mask already 0: min() keeps 0, skip the rays
static const uint kFlagCoverage = 2u;      // also probe whether the surface itself is ray-traceable

float3 PositionAt(int2 p, float depth)
{
	float2 uv = (float2(p) + 0.5) * C.RenderSize.zw;
	float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
	float4 pos = mul(C.InvViewProj, clip);
	return pos.xyz / pos.w;
}

float InterleavedGradientNoise(float2 p, uint frame)
{
	p += float(frame % 64u) * 5.588238;
	return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

#if defined(RT_ALPHA)
// (45h) Alpha-tested casters (leaves, banners, fences): their instances are non-opaque, with the
// instance ID = 1 + their record. A candidate triangle is kept when the alpha of its texture at
// the hit (times material and vertex alpha) passes the shape's alpha test, as the game's own
// Lighting shader decides it. Opaque instances (ID 0) never reach this code.
static const uint kAlphaTextureSlots = 4096u;    // Backend::kAlphaTextureSlots
static const uint kAlphaFallbackBit = 0x80000000u;
static const uint kAlphaCandidateBudget = 32u;   // per ray; past it the ray counts as blocked (bounded cost)
static const float kAlphaTextureWorld = 128.0;   // assumed world size of one texture repeat (mip estimate only)

struct AlphaInfo
{
	uint UvBase;         // first corner of triangle 0 in AlphaUVs (3 per triangle)
	uint Triangles;
	uint Texture;        // bindless slot, or kAlphaFallbackBit | layer of AlphaLayers
	uint Flags;
	float Threshold;     // keep when alpha >= threshold
	float AlphaScale;    // material alpha
	float2 Pad;
	float4 UvTransform;  // xy offset, zw scale (the material's texcoord offset / scale)
};

StructuredBuffer<AlphaInfo> AlphaInfos : register(t2);
StructuredBuffer<uint2> AlphaUVs : register(t3);   // x = half2 texcoord, y = vertex alpha (float bits)
Texture2DArray<float> AlphaLayers : register(t4);  // 256x256 alpha copies of textures D3D12 cannot open
Texture2D<float4> AlphaTextures[kAlphaTextureSlots] : register(t0, space1);
SamplerState AlphaSampler : register(s0);

float2 UnpackTexcoord(uint packed)
{
	return f16tof32(uint2(packed & 0xFFFFu, packed >> 16));
}

// True when the candidate blocks the ray. footprint: world size the ray covers there (mip choice;
// a coarser mip than the screen would pick keeps the texture reads cheap).
bool AlphaBlocks(uint instanceID, uint primitive, float2 bary, float footprint)
{
	if (instanceID == 0u)
		return true;
	const AlphaInfo info = AlphaInfos[instanceID - 1u];
	if (primitive >= info.Triangles)
		return true;
	const uint base = info.UvBase + primitive * 3u;
	const uint2 c0 = AlphaUVs[base];
	const uint2 c1 = AlphaUVs[base + 1u];
	const uint2 c2 = AlphaUVs[base + 2u];
	const float w0 = 1.0 - bary.x - bary.y;
	float2 uv = UnpackTexcoord(c0.x) * w0 + UnpackTexcoord(c1.x) * bary.x + UnpackTexcoord(c2.x) * bary.y;
	const float vertexAlpha = asfloat(c0.y) * w0 + asfloat(c1.y) * bary.x + asfloat(c2.y) * bary.y;
	uv = uv * info.UvTransform.zw + info.UvTransform.xy;
	if (!all(isfinite(uv)))
		return true;
	// Never below 64 texels across: coarser mips average the cut-out away (alpha-test fade).
	const float footprintUV = max(footprint, 1e-3) / kAlphaTextureWorld;
	float alpha;
	[branch] if (info.Texture & kAlphaFallbackBit)
	{
		const float lod = clamp(log2(256.0 * footprintUV) + 0.5, 0.0, 2.0);
		alpha = AlphaLayers.SampleLevel(AlphaSampler, float3(uv, float(info.Texture & 0xFFFFu)), lod);
	}
	else
	{
		uint width, height, levels;
		AlphaTextures[NonUniformResourceIndex(info.Texture)].GetDimensions(0u, width, height, levels);
		const float size = float(max(width, height));
		const float maxLod = clamp(log2(size / 64.0), 0.0, max(float(levels) - 1.0, 0.0));
		const float lod = clamp(log2(size * footprintUV) + 0.5, 0.0, maxLod);
		alpha = AlphaTextures[NonUniformResourceIndex(info.Texture)].SampleLevel(AlphaSampler, uv, lod).a;
	}
	return alpha * info.AlphaScale * vertexAlpha >= info.Threshold;
}

bool AnyHit(float3 origin, float3 direction, float tMax, float footprint)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
	uint budget = kAlphaCandidateBudget;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		// Opaque triangles end the search on their own; a kept cut-out triangle ends it too.
		if (budget == 0u || AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
								footprint + q.CandidateTriangleRayT() * C.ToSun.w))
			q.CommitNonOpaqueTriangleHit();
		else
			budget -= 1u;
	}
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool ClosestHit(float3 origin, float3 direction, float tMax, float footprint, out float tHit, out bool alphaHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
	uint budget = kAlphaCandidateBudget;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (budget == 0u) {
			q.CommitNonOpaqueTriangleHit();
			q.Abort();
			continue;
		}
		budget -= 1u;
		if (AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint + q.CandidateTriangleRayT() * C.ToSun.w))
			q.CommitNonOpaqueTriangleHit();
	}
	tHit = q.CommittedRayT();
	alphaHit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT && q.CommittedInstanceID() != 0u;
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#else
bool AnyHit(float3 origin, float3 direction, float tMax)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
	q.Proceed();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool ClosestHit(float3 origin, float3 direction, float tMax, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
	while (q.Proceed()) {
	}
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#endif

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	const int2 p = int2(dtid.xy);
	if (any(float2(p) >= C.RenderSize.xy))
		return;

	const float dm = DepthMask[p];
	const float depth = abs(dm);
	const uint flags = asuint(C.Params.z);
	if (depth >= 0.999999 || depth <= 0.0) {
		Output[p] = float4(1.0, 0.0, 0.0, 0.0);
		return;
	}

	const float3 P = PositionAt(p, depth);
	if (P.x < C.Rect.x || P.y < C.Rect.y || P.x > C.Rect.z || P.y > C.Rect.w) {
		Output[p] = float4(1.0, 0.0, 0.0, 0.0);
		return;
	}

	// Normal from depth: per axis the neighbour with the smaller depth step (avoids edges).
	const int2 maxP = int2(C.RenderSize.xy) - 1;
	const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
	const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
	const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
	const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - PositionAt(pl, dl) : PositionAt(pr, dr) - P;
	const float3 dy = abs(du - depth) < abs(dd - depth) ? P - PositionAt(pu, du) : PositionAt(pd, dd) - P;
	float3 N = cross(dy, dx);
	const float nLen = length(N);
	const float dist = length(P);
	const float3 V = dist > 0.0 ? -P / dist : float3(0, 0, 1);
	N = nLen > 1e-12 ? N / nLen : V;
	if (dot(N, V) < 0.0)
		N = -N;

	const float bias = C.Bias.x + C.Bias.y * dist;
	const float3 L = normalize(C.ToSun.xyz);

	// (45e) Coverage = a primary ray along this pixel's view ray, compared with the game's depth.
	// The segment starts `reach` in front of the visible surface (so first-person arms and the
	// player, which are not in the scene, never block it) and ends `reach` behind it.
	//   .y class: 0 nothing hit, 0.25 the ray tracer's first surface lies BEHIND the visible one
	//             (the visible surface is not in the scene), 0.5 it lies IN FRONT of the visible one
	//             (geometry the game does not draw there: misplaced or hidden), 1 match.
	//   .z signed distance (ray tracer - game) mapped to [0, 1]: 0.5 + d / 512, 0.5 = match.
	// The match tolerance grows at grazing angles: a surface seen edge-on moves a long way along
	// the view ray for a tiny offset across it.
	float coverage = 0.0;
	float hitDelta = 0.5;
	if (flags & kFlagCoverage) {
		const float cosTheta = max(abs(dot(N, V)), 0.05);
		const float reach = min(dist * 0.5, max(64.0, C.Bias.z * 16.0));
		const float tolerance = min((bias * 2.0 + 2.0) / cosTheta, reach * 0.75);
		float tHit;
#if defined(RT_ALPHA)
		// (45h) A match on an alpha-tested (cut-out traced) surface is its own class: 0.8.
		bool alphaHit;
		if (ClosestHit(P + V * reach, -V, reach * 2.0, dist * C.RenderSize.w * 1.2, tHit, alphaHit)) {
			const float d = reach - tHit;  // > 0: the ray tracer's surface is behind the visible one
			coverage = abs(d) <= tolerance ? (alphaHit ? 0.8 : 1.0) : (d > 0.0 ? 0.25 : 0.5);
#else
		if (ClosestHit(P + V * reach, -V, reach * 2.0, tHit)) {
			const float d = reach - tHit;  // > 0: the ray tracer's surface is behind the visible one
			coverage = abs(d) <= tolerance ? 1.0 : (d > 0.0 ? 0.25 : 0.5);
#endif
			hitDelta = saturate(0.5 - d / 512.0);
		} else {
			hitDelta = 0.0;  // with coverage 0: nothing within reach
		}
	}

	if ((flags & kFlagSkipShadowed) && dm < 0.0) {
		Output[p] = float4(0.0, coverage, hitDelta, 0.0);
		return;
	}

	const float3 origin = P + N * bias + L * (bias * 0.5);

	// Cone around the light direction (sun disc softness), jittered per pixel and per frame.
	const float3 helper = abs(L.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
	const float3 T = normalize(cross(helper, L));
	const float3 B = cross(L, T);
	const uint rays = max(uint(C.Params.x), 1u);
	const uint frame = uint(C.Params.y);
	const float n0 = InterleavedGradientNoise(float2(p), frame);
	const float n1 = InterleavedGradientNoise(float2(p.yx) + 17.0, frame + 7u);

	uint hits = 0;
	[loop] for (uint i = 0; i < rays; ++i)
	{
		const float u1 = frac(n0 + float(i) * 0.6180340);
		const float u2 = frac(n1 + float(i) * 0.7548777);
		const float r = sqrt(u1) * C.ToSun.w;
		float s, c;
		sincos(6.2831853 * u2, s, c);
		const float3 dir = normalize(L + (T * c + B * s) * r);
#if defined(RT_ALPHA)
		hits += AnyHit(origin, dir, C.Params.w, dist * C.RenderSize.w * 1.2) ? 1u : 0u;
#else
		hits += AnyHit(origin, dir, C.Params.w) ? 1u : 0u;
#endif
	}

	Output[p] = float4(1.0 - float(hits) / float(rays), coverage, hitDelta, 0.0);
}
