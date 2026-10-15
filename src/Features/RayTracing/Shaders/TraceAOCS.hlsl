// (46f) Ray-traced ambient occlusion (RTAO), inline RayQuery (DXR 1.1), cs_6_5.
//
// Runs on Ray Tracing's own D3D12 queue in its own command list, submitted right after the sun trace (and its
// point lights) and signalling a second fence: the game's queue waits for it only right before the AO is used
// (after Screen Space GI, in the deferred passes), never before the opaque pass (rt_sync is not extended).
// Compiled at build time like TraceShadowsCS (see CMakeLists.txt).
//
// Per pixel: the surface from the depth (DepthMask: |value| = the depth after the prepass) and a normal from the
// depth (per axis the neighbour with the smaller depth step, as the sun and point-light traces), one cosine-weighted
// ray into the hemisphere above it, at most Params.z units long, any hit ends it. The noise changes every frame:
// the D3D11 side averages the rays spatially (1/4-resolution bilateral filter) and then, optionally, with a clamped
// history. Nothing of NRD.
//
// Output (R8G8B8A8_UNORM, shared with D3D11):
//   r = this frame's ray: 1 = open, 0 = blocked (1 where not traced)
//   g = weight of the ray-traced result (1 inside the traced area and nearer than Fade.x, to 0 at Fade.y; 0 = not traced)
//   b = the hit distance / ray length where blocked (1 = open; debug)
//   a = unused (1)
//
// (46t) Two scales (kFlagDetail; off = the 46s output above, exactly). Detail length D = Extra.y (< ray length L).
//   The long direction is traced in two segments: [0, D] (the near part) and, only where that is open, [D, L] (the
//   far part, a conditional sample: "is the far range open, given the near range is"). Extra.z more directions are
//   traced over [0, D] only. Then
//     r = the far sample (1 open, 0 blocked; 1 where it was not traced)
//     b = the far sample's weight: the near segment of the long direction (1 open, 0 blocked: no far sample here)
//     a = near visibility: the open share of the 1 + Extra.z near segments
//   The D3D11 side filters r weighted by b (a ratio estimator of the conditional far visibility) and a on its own, and
//   multiplies them: V = V(near) x V(far | near open). An occluder is counted once: by the near term if it is closer
//   than D along that direction, by the far term otherwise. (A single long ray with TMin = D would count a direction
//   blocked near AND far twice -- in crevices and next to walls the long ray passes through the near wall and hits the
//   same mesh's other side -- darkening exactly the places the detail term is for.)
//
// Rays: the scene (instance mask 0x01) and, when characters are in the TLAS, a second ray against them (0x06, TMin =
// Extra.x against their own coarse polygons) where the segment passes a character's bounds. Without RT_ALPHA the
// alpha-tested instances (traced leaves / cut-outs) are culled; with it they are tested against their cut-out, at
// most kAlphaCandidateBudget candidates per ray (past it the ray counts as blocked: bounded cost). (46t) AO Alpha
// Budget: past the budget the ray may instead count as half open (kFlagAlphaHalf) or open (kFlagAlphaOpen).

struct AOConstants
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse (jittered, as the depth)
	float4 RenderSize;               // xy render size, zw 1 / size
	float4 Rect;                     // camera-relative xy min / max of the traced area
	float4 Params;                   // x frame, y flags (asuint), z ray length (units), w unused
	float4 Fade;                     // x / y distance where the weight starts to fall / reaches 0, z constant ray offset, w per unit of distance
	float4 Extra;                    // x character ray TMin, (46t) y detail length (units, < Params.z), z detail directions (extra), w unused
	float4 Spheres[2];               // character bounds (camera-relative xyz, radius; 0 = none): [0] the player, [1] the others
};

ConstantBuffer<AOConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthMask : register(t1);
RWTexture2D<unorm float4> AOOut : register(u0);
RWByteAddressBuffer RayCounter : register(u1);  // rays traced (monotonic, read back for the F7 trace)

static const uint kFlagCharacters = 1u;   // characters are in the TLAS: the character ray
static const uint kFlagFirstPerson = 2u;  // no character ray for pixels near the camera
static const uint kFlagDetail = 4u;       // (46t) two scales (see the top)
static const uint kFlagAlphaHalf = 8u;    // (46t) past the alpha candidate budget: half open
static const uint kFlagAlphaOpen = 16u;   // (46t) past the alpha candidate budget: open
static const uint kMaxDetailRays = 4u;
static const uint kSceneMask = 0x01u;
static const uint kCharacterMasks = 0x06u;
static const float kFirstPersonNear = 96.0;

#if defined(RT_ALPHA)
static const uint kAlphaTextureSlots = 4096u;   // Backend::kAlphaTextureSlots
static const uint kAlphaFallbackBit = 0x80000000u;
static const uint kAlphaCandidateBudget = 8u;   // per ray; past it the ray counts as blocked (bounded cost)
static const float kAlphaTextureWorld = 128.0;  // assumed world size of one texture repeat (mip estimate only)

struct AlphaInfo
{
	uint UvBase;
	uint Triangles;
	uint Texture;  // bindless slot, or kAlphaFallbackBit | layer of AlphaLayers
	uint Flags;
	float Threshold;
	float AlphaScale;
	float2 Pad;
	float4 UvTransform;
};

StructuredBuffer<AlphaInfo> AlphaInfos : register(t2);
StructuredBuffer<uint2> AlphaUVs : register(t3);
Texture2DArray<float> AlphaLayers : register(t4);
Texture2D<float4> AlphaTextures[kAlphaTextureSlots] : register(t0, space1);
SamplerState AlphaSampler : register(s0);

float2 UnpackTexcoord(uint packed)
{
	return f16tof32(uint2(packed & 0xFFFFu, packed >> 16));
}

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
	const float footprintUV = max(footprint, 1e-3) / kAlphaTextureWorld;
	float alpha;
	[branch] if (info.Texture & kAlphaFallbackBit)
	{
		alpha = AlphaLayers.SampleLevel(AlphaSampler, float3(uv, float(info.Texture & 0xFFFFu)), 0.0);
	}
	else
	{
		uint width, height, levels;
		AlphaTextures[NonUniformResourceIndex(info.Texture)].GetDimensions(0u, width, height, levels);
		const float size = float(max(width, height));
		const float maxLod = max(float(levels) - 1.0, 0.0);
		const float lod = clamp(log2(size * footprintUV) + 0.5, 0.0, maxLod);
		alpha = AlphaTextures[NonUniformResourceIndex(info.Texture)].SampleLevel(AlphaSampler, uv, lod).a;
	}
	return alpha * info.AlphaScale * vertexAlpha >= info.Threshold;
}
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

// One ray against the scene (or the characters: a_mask), any hit ends it. 1 = open, 0 = blocked; (46t) past the alpha
// candidate budget 0 (46s), 0.5 (kFlagAlphaHalf) or 1 (kFlagAlphaOpen). tHit = where it was blocked (else TMax).
float Visibility(float3 origin, float3 dir, float tMin, float tMax, uint mask, float footprint, uint flags, out float tHit)
{
	tHit = tMax;
	if (!(tMin < tMax))
		return 1.0;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = tMax;
#if defined(RT_ALPHA)
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	uint budget = kAlphaCandidateBudget;
	bool exceeded = false;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (budget == 0u) {
			// Past the budget: blocked (46s), or (46t) stop here and count the ray as half open / open.
			if (flags & (kFlagAlphaHalf | kFlagAlphaOpen)) {
				exceeded = true;
				q.Abort();
			} else {
				q.CommitNonOpaqueTriangleHit();
			}
		} else if (AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint)) {
			q.CommitNonOpaqueTriangleHit();
		} else {
			budget -= 1u;
		}
	}
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		tHit = q.CommittedRayT();
		return 0.0;
	}
	return exceeded ? ((flags & kFlagAlphaOpen) ? 1.0 : 0.5) : 1.0;
#else
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	q.Proceed();
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		tHit = q.CommittedRayT();
		return 0.0;
	}
	return 1.0;
#endif
}

// Does the segment origin -> origin + dir * tMax pass the sphere?
bool SphereOnSegment(float3 origin, float3 dir, float tMax, float4 sphere)
{
	if (sphere.w <= 0.0)
		return false;
	const float3 oc = sphere.xyz - origin;
	const float along = clamp(dot(oc, dir), 0.0, tMax);
	const float3 closest = origin + dir * along - sphere.xyz;
	return dot(closest, closest) <= sphere.w * sphere.w;
}


// One segment [tMin, tMax] of a direction: the scene, then (still open and passing a character's bounds) the characters
// (their ray starts at least Extra.x along). The darker of the two; tHit = where it was blocked. Counts its rays.
float Segment(float3 origin, float3 dir, float tMin, float tMax, float footprint, uint flags, bool characters, inout uint rays, out float tHit)
{
	float v = Visibility(origin, dir, tMin, tMax, kSceneMask, footprint, flags, tHit);
	rays += 1u;
	[branch] if (v > 0.0 && characters && (SphereOnSegment(origin, dir, tMax, C.Spheres[0]) || SphereOnSegment(origin, dir, tMax, C.Spheres[1])))
	{
		float tCharacter;
		const float vc = Visibility(origin, dir, max(tMin, C.Extra.x), tMax, kCharacterMasks, footprint, flags, tCharacter);
		rays += 1u;
		if (vc < v) {
			v = vc;
			tHit = tCharacter;
		}
	}
	return v;
}

// Cosine-weighted direction in the hemisphere around N (T, B: its tangent frame) from two uniform numbers.
float3 CosineDirection(float2 u, float3 T, float3 B, float3 N)
{
	const float r = sqrt(u.x);
	float sn, cs;
	sincos(6.2831853 * u.y, sn, cs);
	return normalize(T * (r * cs) + B * (r * sn) + N * sqrt(saturate(1.0 - u.x)));
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID)
{
	const int2 p = int2(dtid.xy);
	if (any(float2(p) >= C.RenderSize.xy))
		return;
	const uint flags = asuint(C.Params.y);
	const float depth = abs(DepthMask[p]);
	uint rays = 0u;
	float4 result = float4(1.0, 0.0, 1.0, 1.0);
	[branch] if (depth < 0.999999 && depth > 0.0)
	{
		const float3 P = PositionAt(p, depth);
		const float dist = length(P);
		const bool insideRect = !(P.x < C.Rect.x || P.y < C.Rect.y || P.x > C.Rect.z || P.y > C.Rect.w);
		const float weight = insideRect ? 1.0 - smoothstep(C.Fade.x, C.Fade.y, dist) : 0.0;
		[branch] if (weight > 0.0)
		{
			// Normal from depth: per axis the neighbour with the smaller depth step.
			const int2 maxP = int2(C.RenderSize.xy) - 1;
			const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
			const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
			const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
			const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - PositionAt(pl, dl) : PositionAt(pr, dr) - P;
			const float3 dy = abs(du - depth) < abs(dd - depth) ? P - PositionAt(pu, du) : PositionAt(pd, dd) - P;
			const float3 V = dist > 0.0 ? -P / dist : float3(0, 0, 1);
			float3 N = cross(dy, dx);
			const float nLen = length(N);
			N = nLen > 1e-12 ? N / nLen : V;
			if (dot(N, V) < 0.0)
				N = -N;

			// Cosine-weighted direction (noise changes every frame; the D3D11 filter averages it).
			const uint frame = uint(C.Params.x);
			const float u1 = InterleavedGradientNoise(float2(p), frame);
			const float u2 = frac(InterleavedGradientNoise(float2(p.yx) + float2(29.0, 61.0), frame * 7u + 3u) + float(frame % 64u) * 0.6180339887);
			const float3 helper = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
			const float3 T = normalize(cross(helper, N));
			const float3 B = cross(N, T);
			const float3 dir = CosineDirection(float2(u1, u2), T, B, N);

			const float rayLength = C.Params.z;
			const float bias = C.Fade.z + C.Fade.w * dist;
			const float3 origin = P + N * bias;
			const float footprint = dist * 2.0 * C.RenderSize.w;  // about a pixel's size there (alpha mip choice)
			const bool characters = (flags & kFlagCharacters) && !((flags & kFlagFirstPerson) && dist < kFirstPersonNear);
			[branch] if (flags & kFlagDetail)
			{
				// (46t) Two scales: the long direction's near and (where that is open) far segment, more near-only directions.
				const float shortLength = clamp(C.Extra.y, 1.0, rayLength);
				float tNear;
				const float near0 = Segment(origin, dir, 0.0, shortLength, footprint, flags, characters, rays, tNear);
				float far = 1.0;
				[branch] if (near0 > 0.0 && shortLength < rayLength)
				{
					float tFar;
					far = Segment(origin, dir, shortLength, rayLength, footprint, flags, characters, rays, tFar);
				}
				float nearSum = near0;
				const uint extra = min(uint(C.Extra.z), kMaxDetailRays);
				[loop] for (uint i = 1u; i <= extra; ++i)
				{
					// Offsets of the R2 sequence: the extra directions stay well spread around the first one.
					const float2 u = frac(float2(u1, u2) + float(i) * float2(0.7548776662, 0.5698402910));
					float tExtra;
					nearSum += Segment(origin, CosineDirection(u, T, B, N), 0.0, shortLength, footprint, flags, characters, rays, tExtra);
				}
				result = float4(far, weight, near0, nearSum / float(extra + 1u));
			}
			else
			{
				float tHit;
				const float v = Segment(origin, dir, 0.0, rayLength, footprint, flags, characters, rays, tHit);
				result = float4(v, weight, v < 1.0 ? saturate(tHit / max(rayLength, 1.0)) : 1.0, 1.0);
			}
		}
	}
	AOOut[p] = result;
	// One atomic per wave.
	const uint waveRays = WaveActiveSum(rays);
	if (WaveIsFirstLane() && waveRays > 0u) {
		uint previous;
		RayCounter.InterlockedAdd(0, waveRays, previous);
	}
}
