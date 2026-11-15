// (48a) RT reflections step 1: reflection rays, trace only (no shading), inline RayQuery (DXR 1.1), cs_6_5.
//
// Recorded into the RTAO list right behind the RTAO dispatch (Backend::RecordAOTrace): it runs where RTAO runs and the game's queue takes
// it with the RTAO wait (WaitForAO); no fence, no wait of its own. Compiled at build time like TraceAOCS (see CMakeLists.txt).
//
// Per pixel: the surface from this frame's depth (DepthMask: |value| = the depth after the prepass); the normal, roughness and whether it
// reflects at all from LAST frame's G-buffer, reprojected by AONormalPrepCS (RT_REFLECT variant) into ReflectPrep (the trace runs
// before this frame's G-buffer exists, as RTAO). Pixels without a reflection there (REFLECTANCE 0, sky, no data) are not traced.
// The direction: one GGX half vector per pixel and frame (Karis' ImportanceSampleGGX with a2 = roughness^4, as Screen Space Ray
// Tracing's specular rays), the view ray reflected about it (mirror where the roughness is ~0, and where the sample would point below
// the surface). The noise is a static IGN pattern stepped along R2 per frame (SSRT's blue-noise texture does not exist here).
// One closest-hit ray against the scene (mask 0x01, plus kTerrainMask with terrain), the cut-outs tested against their alpha (RT_ALPHA,
// as RTAO; without it they are culled), and one closest-hit ray against the characters (0x06, TMin Bias.z) where it passes their
// bounds; the nearer hit wins.
//
// Output (shared R32_UINT, debug views only: RayTracing ReflectDebugCS): 0 = not traced; else bit 7 set, bits 0-2 the hit type
// (kHitMiss .. kHitTerrain), bits 3-5 the prep's normal confidence (0..7), bits 16-31 the hit distance (f16, units; 0 for a miss).
// RayCounter[0]: pixels traced (monotonic, read back for F7).

struct RFConstants
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse (jittered, as the depth)
	float4 RenderSize;               // xy render size, zw 1 / size
	float4 Params;                   // x frame, y flags (asuint), z max ray length (units), w unused
	float4 Bias;                     // x constant ray offset, y per unit of distance, z character ray TMin, w unused
	float4 Spheres[2];               // character bounds (camera-relative xyz, radius; 0 = none): [0] the player, [1] the others
};

ConstantBuffer<RFConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthMask : register(t1);
Texture2D<uint> ReflectPrep : register(t5);
RWTexture2D<uint> ReflectOut : register(u0);
RWByteAddressBuffer RayCounter : register(u1);

static const uint kFlagCharacters = 1u;   // characters are in the TLAS: the character ray
static const uint kFlagFirstPerson = 2u;  // no character ray for pixels near the camera
static const uint kFlagTerrain = 4u;      // terrain instances exist (kTerrainMask): include them
static const uint kSceneMask = 0x01u;
static const uint kCharacterMasks = 0x06u;
static const uint kTerrainMask = 0x08u;      // Backend::kTerrainMask
static const uint kTerrainKey = 0x400000u;   // Backend kTerrainInstanceKey
static const uint kObjectKey = 0x200000u;    // Backend kObjectInstanceKey
static const uint kMovingKey = 0x800000u;    // Backend kMovingInstanceKey (moved objects)
static const float kFirstPersonNear = 96.0;
static const uint kHitMiss = 0u, kHitStatic = 1u, kHitObject = 2u, kHitCutout = 3u, kHitCharacter = 4u, kHitTerrain = 5u;
static const uint kTraced = 0x80u;

#if defined(RT_ALPHA)
static const uint kAlphaTextureSlots = 4096u;   // Backend::kAlphaTextureSlots
static const uint kAlphaFallbackBit = 0x80000000u;
static const uint kAlphaCandidateBudget = 16u;  // per ray; past it the ray stops (counts as a cut-out hit there: bounded cost)
static const float kAlphaTextureWorld = 128.0;  // assumed world size of one texture repeat (mip estimate only)

struct AlphaInfo
{
	uint UvBase;
	uint Triangles;
	uint Texture;  // bindless slot, or kAlphaFallbackBit | layer of AlphaLayers
	uint Flags;
	float Threshold;
	float AlphaScale;
	float TextureSize;
	float TextureLevels;
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
	const uint base = info.UvBase + primitive * 2u;
	const uint2 c0 = AlphaUVs[base];
	const uint2 c1 = AlphaUVs[base + 1u];
	const float w0 = 1.0 - bary.x - bary.y;
	float2 uv = UnpackTexcoord(c0.x) * w0 + UnpackTexcoord(c0.y) * bary.x + UnpackTexcoord(c1.x) * bary.y;
	const float3 cornerAlpha = float3(uint3(c1.y, c1.y >> 8u, c1.y >> 16u) & 0xFFu) / 255.0;
	const float vertexAlpha = cornerAlpha.x * w0 + cornerAlpha.y * bary.x + cornerAlpha.z * bary.y;
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
		const float size = info.TextureSize;
		const float maxLod = max(info.TextureLevels - 1.0, 0.0);
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

float3 OctDecode(float2 e)
{
	float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	const float t = saturate(-n.z);
	n.xy += float2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
	return normalize(n);
}

// Karis, "Real Shading in Unreal Engine 4" (ssrt_common.hlsli ImportanceSampleGGX): a half vector around +z.
float3 ImportanceSampleGGX(float2 E, float a2)
{
	const float phi = 6.2831853 * E.x;
	const float cosTheta = sqrt((1.0 - E.y) / (1.0 + (a2 - 1.0) * E.y));
	const float sinTheta = sqrt(saturate(1.0 - cosTheta * cosTheta));
	return float3(sinTheta * cos(phi), sinTheta * sin(phi), cosTheta);
}

// Closest hit against a_mask; o_type = the hit's type (static / object / cut-out / terrain), o_t = its distance.
bool ClosestHit(float3 origin, float3 dir, float tMin, float tMax, uint mask, float footprint, out uint o_type, out float o_t)
{
	o_type = kHitMiss;
	o_t = tMax;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = tMax;
#if defined(RT_ALPHA)
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	uint budget = kAlphaCandidateBudget;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (budget == 0u || AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint + q.CandidateTriangleRayT() * 0.002))
			q.CommitNonOpaqueTriangleHit();
		else
			budget -= 1u;
	}
#else
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	q.Proceed();
#endif
	const bool hit = q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
	if (hit) {
		o_t = q.CommittedRayT();
		const uint key = q.CommittedInstanceContributionToHitGroupIndex();
		if (q.CommittedInstanceID() != 0u)
			o_type = kHitCutout;  // an alpha-tested instance (its record + 1)
		else if (key == kTerrainKey)
			o_type = kHitTerrain;
		else if (key == kObjectKey || key == kMovingKey)
			o_type = kHitObject;
		else
			o_type = kHitStatic;
	}
	return hit;
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

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	const bool inside = all(float2(p) < C.RenderSize.xy);
	const uint flags = asuint(C.Params.y);
	uint result = 0u;
	uint traced = 0u;
	[branch] if (inside)
	{
		const float depth = abs(DepthMask[p]);
		const uint prep = ReflectPrep[p];
		[branch] if ((prep & 0x80000000u) != 0u && depth > 0.0 && depth < 0.999999)
		{
			const float3 P = PositionAt(p, depth);
			const float dist = length(P);
			const float3 D = dist > 0.0 ? P / dist : float3(0, 0, -1);  // camera to surface
			// This frame's depth normal (the ray start's offset, as the other traces).
			const int2 maxP = int2(C.RenderSize.xy) - 1;
			const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
			const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
			const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
			const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - PositionAt(pl, dl) : PositionAt(pr, dr) - P;
			const float3 dy = abs(du - depth) < abs(dd - depth) ? P - PositionAt(pu, du) : PositionAt(pd, dd) - P;
			float3 Ng = cross(dy, dx);
			const float nLen = length(Ng);
			Ng = nLen > 1e-12 ? Ng / nLen : -D;
			if (dot(Ng, D) > 0.0)
				Ng = -Ng;
			// Last frame's smooth normal and roughness (AONormalPrepCS RT_REFLECT): 11 + 11 bits octahedral (0..1 = -1..1), 6 bits
			// roughness, 3 bits confidence, bit 31 valid.
			const float2 e = float2(prep & 0x7FFu, (prep >> 11) & 0x7FFu) / 2047.0 * 2.0 - 1.0;
			float3 N = OctDecode(e);
			if (dot(N, Ng) < 0.1)
				N = Ng;  // a normal that disagrees with the surface (stale, wrong reprojection): the facet
			const float roughness = float((prep >> 22) & 0x3Fu) / 63.0;
			const uint confidence = (prep >> 28) & 0x7u;
			float3 R = reflect(D, N);
			[branch] if (roughness >= 0.001)
			{
				// The GGX half vector around N (a basis built from N; any rotation about N is as good for one isotropic sample).
				const float3 helper = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
				const float3 T = normalize(cross(helper, N));
				const float3 B = cross(N, T);
				const uint frame = uint(C.Params.x);
				const float a = frac(52.9829189 * frac(dot(float2(p) + float2(23.0, 71.0), float2(0.06711056, 0.00583715))));
				const float b = frac(52.9829189 * frac(dot(float2(p) + float2(47.0, 13.0), float2(0.00583715, 0.06711056))));
				const float2 E = frac(float2(a, b) + float(frame % 1024u) * float2(0.7548776662, 0.5698402910));
				const float r2 = roughness * roughness;
				const float3 H = ImportanceSampleGGX(E, r2 * r2);
				const float3 Rg = reflect(D, normalize(T * H.x + B * H.y + N * H.z));
				if (dot(Rg, Ng) > 0.0)
					R = Rg;
			}
			R = normalize(R);
			const float3 origin = P + Ng * (C.Bias.x + C.Bias.y * dist);
			const float tMax = C.Params.z;
			const float footprint = dist * 2.0 * C.RenderSize.w;
			uint type = kHitMiss;
			float tHit = tMax;
			const uint sceneMask = kSceneMask | ((flags & kFlagTerrain) ? kTerrainMask : 0u);
			ClosestHit(origin, R, 0.0, tMax, sceneMask, footprint, type, tHit);
			const bool characters = (flags & kFlagCharacters) && !((flags & kFlagFirstPerson) && dist < kFirstPersonNear);
			[branch] if (characters && (SphereOnSegment(origin, R, tHit, C.Spheres[0]) || SphereOnSegment(origin, R, tHit, C.Spheres[1])))
			{
				RayDesc ray;
				ray.Origin = origin;
				ray.Direction = R;
				ray.TMin = C.Bias.z;
				ray.TMax = tHit;
				RayQuery<RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
				q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMasks, ray);
				q.Proceed();
				if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
					type = kHitCharacter;
					tHit = q.CommittedRayT();
				}
			}
			const float distance = type == kHitMiss ? 0.0 : min(tHit, 65000.0);
			result = kTraced | type | (min(confidence, 7u) << 3) | (f32tof16(distance) << 16);
			traced = 1u;
		}
		ReflectOut[p] = result;
	}
	const uint waveTraced = WaveActiveSum(traced);
	if (WaveIsFirstLane() && waveTraced > 0u) {
		uint previous;
		RayCounter.InterlockedAdd(0, waveTraced, previous);
	}
}
