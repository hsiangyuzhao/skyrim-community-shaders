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
//
// (45i) And twice more with RT_CHARACTERS (with and without RT_ALPHA): skinned characters are
// instances with mask kCharacterMask only (every other instance keeps mask 0xFF), so the scene
// ray skips them (mask kSceneMask) and a second ray (mask kCharacterMask, TMin = Bias.w) tests
// them alone: hits closer than Bias.w are ignored, which keeps a character's own coarse polygons
// from shadowing the smooth surface the game draws (terminator acne). In first person the
// character ray is not traced for pixels nearer than kFirstPersonNear (the arms and weapon sit
// inside the player's third-person body). Without RT_CHARACTERS the source is exactly 45h's.

struct TraceConstants
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse (jittered, as the shadow mask used)
	float4 ToSun;                    // xyz direction to the light, w = tan(cone half angle)
	float4 Params;                   // x rays per pixel, y frame index, z flags (asuint), w max ray length
	float4 RenderSize;               // xy render size in pixels, zw 1/size
	float4 Rect;                     // camera-relative xy min / max of the traced area
	float4 Bias;                     // x constant bias, y bias per unit of distance, z coverage probe length
	float4 World;                    // (45j) xyz camera world position (world-locked noise), w unused
	float4 Extra;                    // (45k) x adaptive extra rays, yzw unused
};

ConstantBuffer<TraceConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthMask : register(t1);
RWTexture2D<unorm float4> Output : register(u0);

static const uint kFlagSkipShadowed = 1u;  // engine mask already 0: min() keeps 0, skip the rays
static const uint kFlagCoverage = 2u;      // also probe whether the surface itself is ray-traceable
// (45j) Runtime switches (RayTracing.cpp); each one off = the 45i behaviour.
static const uint kFlagWorldNoise = 8u;        // ray jitter seeded by the world position (stable while the camera moves)
static const uint kFlagStableNormal = 16u;     // depth-edge pixels use the view direction as normal
static const uint kFlagAlphaFootprint = 32u;   // alpha mip from the hit distance only (camera independent), finer mips
// (45k) Denoiser support (RayTracing.cpp); each one off = the 45j behaviour.
static const uint kFlagR2Noise = 64u;          // animated noise: per-pixel IGN + an R2 step per ray (decorrelated rays)
static const uint kFlagAdaptive = 128u;        // extra rays (Extra.x) where the pixel's rays disagree or a neighbour differs
static const uint kFlagBlockerDist = 256u;     // Output.z = blocker distance (first ray closest hit) instead of hitDelta
static const uint kFlagRtSurface = 512u;       // Output.w bit kOutRtSurface: the visible surface belongs to a traced caster
// Output.w (45j): bit flags as unorm8 (value / 255).
static const uint kOutAlphaCandidate = 1u;     // a sun ray met an alpha-tested (cut-out) candidate
static const uint kOutCharacter = 2u;          // a sun ray was blocked by a character
static const uint kOutRtSurface = 4u;          // (45k) the visible surface is a ray-traced caster (Screen Space Shadows mask)
static const uint kOutExtraRays = 8u;          // (45k) adaptive extra rays were traced here
static const float kBlockerRange = 4096.0;     // (45k) Output.z = sqrt(t / range), 0 = no blocker
#if defined(RT_CHARACTERS)
static const uint kFlagFirstPerson = 4u;             // (45i) player in first person: no character ray for near pixels
static const uint kSceneMask = 0x01u;                // every instance but characters
static const uint kCharacterMask = 0x02u;            // characters only (Backend::kCharacterMask)
static const float kFirstPersonNear = 96.0;
#	define RT_SCENE_MASK kSceneMask
#else
#	define RT_SCENE_MASK 0xFF
#endif

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

// (45j) World-locked jitter: a hash of the world position snapped to a cell about one pixel wide
// (power-of-two cell sizes, so the pattern only changes where the distance crosses a band). The
// same surface point keeps its rays while the camera moves: no crawling noise for the upscaler.
uint HashUint(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

float2 WorldNoise(float3 P, float dist)
{
	const float footprint = max(dist * C.RenderSize.w * 1.2, 0.25);
	const float band = ceil(log2(footprint));
	const float cell = exp2(band);
	const float3 world = P + C.World.xyz;
	const int3 q = int3(floor(world / cell));
	uint h = HashUint(asuint(q.x) ^ HashUint(asuint(q.y) ^ HashUint(asuint(q.z) ^ asuint(int(band) + 64))));
	const float a = float(h & 0xFFFFu) / 65536.0;
	const float b = float(h >> 16) / 65536.0;
	return float2(a, b);
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
	// (45j, kFlagAlphaFootprint) Never below 256 texels, the 256x256 copies always at their top
	// mip, and the threshold eased a little with the mip so thin leaves keep their coverage.
	const bool fineMips = (asuint(C.Params.z) & kFlagAlphaFootprint) != 0u;
	const float minTexels = fineMips ? 256.0 : 64.0;
	const float footprintUV = max(footprint, 1e-3) / kAlphaTextureWorld;
	float alpha;
	float lod;
	[branch] if (info.Texture & kAlphaFallbackBit)
	{
		lod = fineMips ? 0.0 : clamp(log2(256.0 * footprintUV) + 0.5, 0.0, 2.0);
		alpha = AlphaLayers.SampleLevel(AlphaSampler, float3(uv, float(info.Texture & 0xFFFFu)), lod);
	}
	else
	{
		uint width, height, levels;
		AlphaTextures[NonUniformResourceIndex(info.Texture)].GetDimensions(0u, width, height, levels);
		const float size = float(max(width, height));
		const float maxLod = clamp(log2(size / minTexels), 0.0, max(float(levels) - 1.0, 0.0));
		lod = clamp(log2(size * footprintUV) + 0.5, 0.0, maxLod);
		alpha = AlphaTextures[NonUniformResourceIndex(info.Texture)].SampleLevel(AlphaSampler, uv, lod).a;
	}
	const float threshold = fineMips ? info.Threshold * saturate(1.0 - 0.06 * lod) : info.Threshold;
	return alpha * info.AlphaScale * vertexAlpha >= threshold;
}

bool AnyHit(float3 origin, float3 direction, float tMax, float footprint, inout uint outFlags, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, RT_SCENE_MASK, ray);
	uint budget = kAlphaCandidateBudget;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		outFlags |= kOutAlphaCandidate;
		// Opaque triangles end the search on their own; a kept cut-out triangle ends it too.
		if (budget == 0u || AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
								footprint + q.CandidateTriangleRayT() * C.ToSun.w))
			q.CommitNonOpaqueTriangleHit();
		else
			budget -= 1u;
	}
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool ClosestHit(float3 origin, float3 direction, float tMax, float footprint, uint mask, out float tHit, out bool alphaHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
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
bool AnyHit(float3 origin, float3 direction, float tMax, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, RT_SCENE_MASK, ray);
	q.Proceed();
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

bool ClosestHit(float3 origin, float3 direction, float tMax, uint mask, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	while (q.Proceed()) {
	}
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#endif


#if defined(RT_CHARACTERS)
// (45i) The character ray: characters only, hits nearer than tMin ignored (self-shadow acne).
#	if defined(RT_ALPHA)
// Alpha-tested character parts (hair) are non-opaque: tested against their cut-out like AnyHit.
bool CharacterAnyHit(float3 origin, float3 direction, float tMin, float tMax, float footprint, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = tMin;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMask, ray);
	uint budget = kAlphaCandidateBudget;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (budget == 0u || AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(),
								footprint + q.CandidateTriangleRayT() * C.ToSun.w))
			q.CommitNonOpaqueTriangleHit();
		else
			budget -= 1u;
	}
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#	else
bool CharacterAnyHit(float3 origin, float3 direction, float tMin, float tMax, float footprint, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = tMin;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMask, ray);
	q.Proceed();
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#	endif

// Coverage view only: the nearest character surface along the probe (cut-outs ignored).
bool CharacterClosestHit(float3 origin, float3 direction, float tMax, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = direction;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMask, ray);
	while (q.Proceed()) {
	}
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}
#endif

// (45k, kFlagRtSurface) Is the visible surface at P part of the ray-traced scene? A short segment
// through P along the view ray, any hit (cut-outs taken as solid), every instance (characters too).
// The tolerance is tight so that grass standing on traced ground is not taken for the ground.
bool SurfaceProbe(float3 P, float3 V, float3 N, float dist)
{
	const float cosTheta = max(abs(dot(N, V)), 0.25);
	const float tolerance = min(clamp(1.5 + dist * 0.002, 1.5, 24.0) / cosTheta, 48.0);
	RayDesc ray;
	ray.Origin = P + V * tolerance;
	ray.Direction = -V;
	ray.TMin = 0.0;
	ray.TMax = tolerance * 2.0;
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, 0xFF, ray);
	q.Proceed();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// (45k) The direction of sun ray i of this pixel (a point on the sun disc).
float3 SunRay(float3 L, float3 T, float3 B, float n0, float n1, uint i, bool r2)
{
	// 45j: golden-ratio steps per ray. R2 (kFlagR2Noise): the 2D R2 sequence, so the extra rays of
	// a pixel and the rays of neighbouring frames stay decorrelated in both dimensions.
	const float2 step = r2 ? float2(0.7548776662, 0.5698402910) : float2(0.6180340, 0.7548777);
	const float u1 = frac(n0 + float(i) * step.x);
	const float u2 = frac(n1 + float(i) * step.y);
	const float r = sqrt(u1) * C.ToSun.w;
	float s, c;
	sincos(6.2831853 * u2, s, c);
	return normalize(L + (T * c + B * s) * r);
}

// (45k) One sun ray: blocked or not, and the blocker's distance. closest: the nearest blocker
// along the ray (the first ray when the denoiser needs the penumbra width), else any blocker.
bool SunRayBlocked(float3 origin, float3 dir, float footprint, bool characterRay, bool closest, inout uint outFlags, out float tHit)
{
	tHit = 0.0;
	bool blocked;
#if defined(RT_ALPHA)
	[branch] if (closest)
	{
		bool alphaHit;
		blocked = ClosestHit(origin, dir, C.Params.w, footprint, RT_SCENE_MASK, tHit, alphaHit);
	}
	else
	{
		blocked = AnyHit(origin, dir, C.Params.w, footprint, outFlags, tHit);
	}
#else
	[branch] if (closest)
		blocked = ClosestHit(origin, dir, C.Params.w, RT_SCENE_MASK, tHit);
	else
		blocked = AnyHit(origin, dir, C.Params.w, tHit);
#endif
#if defined(RT_CHARACTERS)
	if (!blocked && characterRay) {
		blocked = CharacterAnyHit(origin, dir, C.Bias.w, C.Params.w, footprint, tHit);
		outFlags |= blocked ? kOutCharacter : 0u;
	}
#endif
	return blocked;
}

// (45k) Per group: each pixel's class after its first rays (adaptive rays) and whether its
// surface is a traced caster (mask dilation), with its distance to the camera.
static const uint kClassNone = 0u, kClassLit = 1u, kClassShadow = 2u, kClassMixed = 3u, kClassRtSurface = 4u;
groupshared uint gsClass[64];
groupshared float gsDist[64];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex)
{
	// (45k) No early return before the group barrier below: every thread reaches it.
	const int2 p = int2(dtid.xy);
	const uint flags = asuint(C.Params.z);
	const bool inside = all(float2(p) < C.RenderSize.xy);
	const float dm = inside ? DepthMask[p] : 0.0;
	const float depth = abs(dm);
	bool valid = inside && depth < 0.999999 && depth > 0.0;
	float3 P = 0.0;
	if (valid) {
		P = PositionAt(p, depth);
		valid = !(P.x < C.Rect.x || P.y < C.Rect.y || P.x > C.Rect.z || P.y > C.Rect.w);
	}

	float coverage = 0.0;
	float hitDelta = 0.5;
	float dist = 0.0;
	float3 N = float3(0, 0, 1);
	float3 V = float3(0, 0, 1);
	bool rtSurface = false;
	bool traced = false;
	bool skipped = false;
	uint hits = 0u, rays = 0u, outFlags = 0u;
	float tBlocker = 0.0;
	float3 origin = 0.0, L = 0.0, T = 0.0, B = 0.0;
	float n0 = 0.0, n1 = 0.0, footprint0 = 0.0;
	bool characterRay = false;
	const bool r2 = (flags & kFlagR2Noise) != 0u;
	const bool wantBlocker = (flags & kFlagBlockerDist) != 0u && (flags & kFlagCoverage) == 0u;

	[branch] if (valid)
	{
		// Normal from depth: per axis the neighbour with the smaller depth step (avoids edges).
		const int2 maxP = int2(C.RenderSize.xy) - 1;
		const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
		const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
		const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
		const float3 Pl = PositionAt(pl, dl), Pr = PositionAt(pr, dr), Pu = PositionAt(pu, du), Pd = PositionAt(pd, dd);
		const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - Pl : Pr - P;
		const float3 dy = abs(du - depth) < abs(dd - depth) ? P - Pu : Pd - P;
		N = cross(dy, dx);
		const float nLen = length(N);
		dist = length(P);
		V = dist > 0.0 ? -P / dist : float3(0, 0, 1);
		N = nLen > 1e-12 ? N / nLen : V;
		if (dot(N, V) < 0.0)
			N = -N;
		// (45j, kFlagStableNormal) A pixel whose neighbours on one axis BOTH jump in distance (a thin
		// leaf, a blade, the silhouette of a branch) has no usable depth normal: the one picked flips
		// with the jitter from frame to frame. The view direction is used there instead.
		if (flags & kFlagStableNormal) {
			const float tol = dist * 0.02 + 4.0;
			const bool edgeX = abs(length(Pl) - dist) > tol && abs(length(Pr) - dist) > tol;
			const bool edgeY = abs(length(Pu) - dist) > tol && abs(length(Pd) - dist) > tol;
			if (edgeX || edgeY)
				N = V;
		}

		const float bias = C.Bias.x + C.Bias.y * dist;
		L = normalize(C.ToSun.xyz);
		// (45j, kFlagAlphaFootprint) The cut-out mip depends on the hit distance only, not on how far
		// the receiving pixel is from the camera.
		footprint0 = (flags & kFlagAlphaFootprint) ? 0.5 : dist * C.RenderSize.w * 1.2;

		// (45e) Coverage = a primary ray along this pixel's view ray, compared with the game's depth.
		// The segment starts `reach` in front of the visible surface (so first-person arms and the
		// player, which are not in the scene, never block it) and ends `reach` behind it.
		//   .y class: 0 nothing hit, 0.25 the ray tracer's first surface lies BEHIND the visible one
		//             (the visible surface is not in the scene), 0.5 it lies IN FRONT of the visible one
		//             (geometry the game does not draw there: misplaced or hidden), 1 match.
		//   .z signed distance (ray tracer - game) mapped to [0, 1]: 0.5 + d / 512, 0.5 = match.
		// The match tolerance grows at grazing angles: a surface seen edge-on moves a long way along
		// the view ray for a tiny offset across it.
		if (flags & kFlagCoverage) {
			const float cosTheta = max(abs(dot(N, V)), 0.05);
			const float reach = min(dist * 0.5, max(64.0, C.Bias.z * 16.0));
			const float tolerance = min((bias * 2.0 + 2.0) / cosTheta, reach * 0.75);
			float tHit;
#if defined(RT_ALPHA)
			// (45h) A match on an alpha-tested (cut-out traced) surface is its own class: 0.8.
			bool alphaHit;
			if (ClosestHit(P + V * reach, -V, reach * 2.0, dist * C.RenderSize.w * 1.2, 0xFF, tHit, alphaHit)) {
				const float d = reach - tHit;  // > 0: the ray tracer's surface is behind the visible one
				coverage = abs(d) <= tolerance ? (alphaHit ? 0.8 : 1.0) : (d > 0.0 ? 0.25 : 0.5);
#else
			if (ClosestHit(P + V * reach, -V, reach * 2.0, 0xFF, tHit)) {
				const float d = reach - tHit;  // > 0: the ray tracer's surface is behind the visible one
				coverage = abs(d) <= tolerance ? 1.0 : (d > 0.0 ? 0.25 : 0.5);
#endif
				hitDelta = saturate(0.5 - d / 512.0);
			} else {
				hitDelta = 0.0;  // with coverage 0: nothing within reach
			}
#if defined(RT_CHARACTERS)
			// (45i) A match on a ray-traced character is its own class: 0.65.
			float tCharacter;
			if (CharacterClosestHit(P + V * reach, -V, reach * 2.0, tCharacter) && abs(reach - tCharacter) <= tolerance)
				coverage = 0.65;
#endif
		}

		// (45k) Screen Space Shadows "non-RT casters only": is this surface a traced caster?
		if (flags & kFlagRtSurface)
			rtSurface = SurfaceProbe(P, V, N, dist);

		skipped = (flags & kFlagSkipShadowed) && dm < 0.0;
		[branch] if (!skipped)
		{
			traced = true;
			origin = P + N * bias + L * (bias * 0.5);
			// Cone around the light direction (sun disc softness), jittered per pixel (and per frame, or
			// per world position with kFlagWorldNoise).
			const float3 helper = abs(L.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
			T = normalize(cross(helper, L));
			B = cross(L, T);
			const uint frame = uint(C.Params.y);
			if (flags & kFlagWorldNoise) {
				const float2 n = WorldNoise(P, dist);
				n0 = n.x;
				n1 = n.y;
			} else if (r2) {
				// (45k) Animated: interleaved gradient noise per pixel and frame for each dimension, from
				// two decorrelated pixel patterns (transposed and offset).
				n0 = InterleavedGradientNoise(float2(p), frame);
				n1 = InterleavedGradientNoise(float2(p.yx) + float2(29.0, 61.0), frame * 7u + 3u);
			} else {
				n0 = InterleavedGradientNoise(float2(p), frame);
				n1 = InterleavedGradientNoise(float2(p.yx) + 17.0, frame + 7u);
			}
#if defined(RT_CHARACTERS)
			characterRay = !((flags & kFlagFirstPerson) && dist < kFirstPersonNear);
#endif
			const uint baseRays = max(uint(C.Params.x), 1u);
			uint tCount = 0u;
			float tSum = 0.0;
			[loop] for (uint i = 0; i < baseRays; ++i)
			{
				float tHit;
				const bool blocked = SunRayBlocked(origin, SunRay(L, T, B, n0, n1, i, r2), footprint0, characterRay, wantBlocker && i == 0u, outFlags, tHit);
				if (blocked) {
					hits += 1u;
					// The first ray's blocker is the nearest one (closest hit): it alone sets the distance.
					if (i == 0u && wantBlocker) {
						tBlocker = tHit;
						tCount = 1000u;
					} else if (tCount < 1000u) {
						tSum += tHit;
						tCount += 1u;
					}
				}
			}
			rays = baseRays;
			if (tCount > 0u && tCount < 1000u)
				tBlocker = tSum / float(tCount);
		}
	}

	// (45k) Group exchange: classes for the adaptive rays, traced surfaces for the mask dilation.
	uint cls = kClassNone;
	if (traced)
		cls = hits == 0u ? kClassLit : (hits == rays ? kClassShadow : kClassMixed);
	gsClass[gi] = cls | (rtSurface ? kClassRtSurface : 0u);
	gsDist[gi] = dist;
	GroupMemoryBarrierWithGroupSync();

	const uint extraRays = uint(C.Extra.x);
	[branch] if (traced && (flags & kFlagAdaptive) && extraRays > 0u)
	{
		// Extra rays only where the first rays disagree or a traced neighbour (in this group) differs:
		// shadow edges and penumbrae. Deep shadow and open light keep their first rays.
		bool edge = cls == kClassMixed;
		[unroll] for (int oy = -1; oy <= 1; ++oy)
		{
			[unroll] for (int ox = -1; ox <= 1; ++ox)
			{
				const int2 g = int2(gtid.xy) + int2(ox, oy);
				if (all(g >= 0) && all(g < 8)) {
					const uint c = gsClass[g.y * 8 + g.x] & 3u;
					edge = edge || (c != kClassNone && c != cls);
				}
			}
		}
		[branch] if (edge)
		{
			float tSum = 0.0;
			uint tCount = 0u;
			[loop] for (uint e = 0; e < extraRays; ++e)
			{
				float tHit;
				const bool blocked = SunRayBlocked(origin, SunRay(L, T, B, n0, n1, rays + e, r2), footprint0, characterRay, false, outFlags, tHit);
				if (blocked) {
					hits += 1u;
					tSum += tHit;
					tCount += 1u;
				}
			}
			rays += extraRays;
			outFlags |= kOutExtraRays;
			// No blocker distance from the first rays yet: the extra rays' average.
			if (tBlocker <= 0.0 && tCount > 0u)
				tBlocker = tSum / float(tCount);
		}
	}

	// (45k) Mask dilation by one pixel inside the group: a silhouette pixel the probe missed (thin
	// posts, trims) is a traced surface when a traced neighbour lies at about the same distance.
	[branch] if ((flags & kFlagRtSurface) && valid && !rtSurface)
	{
		const float tol = dist * 0.03 + 4.0;
		[unroll] for (int oy = -1; oy <= 1; ++oy)
		{
			[unroll] for (int ox = -1; ox <= 1; ++ox)
			{
				const int2 g = int2(gtid.xy) + int2(ox, oy);
				if (all(g >= 0) && all(g < 8)) {
					const uint k = g.y * 8 + g.x;
					rtSurface = rtSurface || ((gsClass[k] & kClassRtSurface) && abs(gsDist[k] - dist) <= tol);
				}
			}
		}
	}

	if (!inside)
		return;
	outFlags |= rtSurface ? kOutRtSurface : 0u;
	if (!valid) {
		Output[p] = float4(1.0, 0.0, 0.0, 0.0);
		return;
	}
	if (!traced) {
		// Engine mask already 0 (skipped): min() keeps it.
		Output[p] = float4(0.0, coverage, hitDelta, float(outFlags) / 255.0);
		return;
	}
	float z = hitDelta;
	if (wantBlocker)
		z = tBlocker > 0.0 ? max(sqrt(saturate(tBlocker / kBlockerRange)), 1.0 / 255.0) : 0.0;
	Output[p] = float4(1.0 - float(hits) / float(rays), coverage, z, float(outFlags) / 255.0);
}
