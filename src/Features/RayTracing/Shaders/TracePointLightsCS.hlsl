// (46b) Ray-traced point-light shadows, inline RayQuery (DXR 1.1), cs_6_5.
//
// Runs on Ray Tracing's own D3D12 queue right after the sun trace, in the same command list and against
// the same TLAS (compiled at build time like TraceShadowsCS, see CMakeLists.txt). Per 8x8 group the
// shadow sources (RayTracing/PointLights.h, at most 64, one bit each) whose sphere reaches the group's
// surfaces are found first; per pixel the K sources that light it most (attenuation x N.L x intensity,
// as Lighting.hlsl attenuates them) are picked and one ray is traced toward a point on each source's
// small emitter sphere (soft edges, cleaned by PointDenoiseCS). Sources under Extra.x of the pixel's
// total point light are not traced.
//
// Inputs: t0 TLAS, t1 DepthMask (|value| = depth after the prepass, see PrepareCS), t2 this frame's sun trace, t3 the sources.
// Sun gate: at a sunlit pixel a point light under Extra.z of the sun's light there is not traced (its shadow would not
// show): in daylight only the surroundings of strong fixtures are traced.
// Outputs (R8G8B8A8_UNORM, shared with D3D11), per slot s = 0..3 (slots past K are empty):
//   PLVis[s]: 1 = lit; < 1 = blocked, the value is sqrt(blocker distance / light distance) * 254/255
//             (the denoiser's penumbra width);
//   PLIdx[s]: byte = source slot + 1 (0 = empty slot), bit 7 of byte s = bit s of the pixel's candidate
//             count (0..15, the debug view).
// Positions are camera-relative (the TLAS is built around the same camera position).
//
// Rays: the scene (instance mask 0x01, every opaque instance) and, when characters are in the TLAS, a
// second ray against them (masks 0x06, TMin = Bias.z against their own coarse polygons, as the sun's).
// Non-opaque (alpha-tested) instances are culled: leaves and cut-outs cast no point-light shadow in 46b.
// (46c) With RT_ALPHA (interiors) they are tested against their cut-out instead (Window Shadows Ultimate's
// window masks: the window pattern of its beams), at most kAlphaCandidateBudget candidates per ray.
// The ray ends the source's clearance short of the light: the light's own fixture (lantern frame,
// brazier bowl, the torch in a hand) does not shadow it.
// (46c) Spot sources (the engine's spot shadow lights indoors): only surfaces inside the cone are candidates,
// weighted like the engine's spot mask (1 - (tan angle / tan half width)^falloff). Window beams (Window Shadows
// Ultimate's huge spots far outside the windows): no attenuation in the ranking, no clearance, and the ray stops
// where it leaves the captured interior (Rect xy, Bounds z): nothing beyond it is in the scene.

struct PLConstants
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse (jittered, as the shadow mask used)
	float4 RenderSize;               // xy render size, zw 1 / size
	float4 Rect;                     // camera-relative xy min / max of the traced area
	float4 Params;                   // x sources to scan, y frame, z flags (asuint), w K (1..4)
	float4 Bias;                     // x constant ray offset, y per unit of distance, z character ray TMin, w emitter radius
	float4 Extra;                    // x least share of the pixel's point light that is traced, y least absolute contribution,
	                                 // z least share of the sun's light at a sunlit pixel (the sun gate), (46q) w carrier clearance
	                                 // (units; < 0 = 46p: a carried light never tests its carrier)
	float4 Spheres[2];               // character bounds (camera-relative xyz, radius; 0 = none): [0] the player, [1] the others
	float4 Sun;                      // xyz to the sun (world axes), w its strength (brightest channel; 0 = no sun gate)
	float4 Bounds;                   // (46c) x / y camera-relative z min / max of the interior, z ray margin past it,
	                                 // (46o) w tan of the window beams' angular size (half angle; 0 = 46n: the emitter sphere)
};

struct PLSource
{
	float4 PosRadius;  // camera-relative position, radius
	float4 Atten;      // inverse radius, ISL fade zone, ISL size bias, intensity
	float4 Spot;       // (46c) spot: direction (light to surface, world axes), tan of the larger half width
	uint4 Info;        // flags, clearance (float bits), (46c) spot falloff exponent (float bits), (46e) carrier key (0 = none)
};

ConstantBuffer<PLConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthMask : register(t1);
Texture2D<unorm float4> SunVisibility : register(t2);  // this frame's sun trace (.x: 1 lit, 0 shadow)
StructuredBuffer<PLSource> Sources : register(t3);
RWTexture2D<unorm float4> PLVis : register(u0);
RWTexture2D<unorm float4> PLIdx : register(u1);
RWByteAddressBuffer RayCounter : register(u2);  // rays traced (monotonic, read back for the F7 trace)

static const uint kFlagCharacters = 1u;   // characters are in the TLAS: the character ray
static const uint kFlagFirstPerson = 2u;  // no character ray for pixels near the camera
static const uint kFlagCharCull = 4u;     // the character ray only where the segment passes a character's bounds
static const uint kFlagSticky = 8u;          // (46g) last frame's sources of the pixel rank x kStickyBonus and pass at half the share cut
static const uint kFlagBeamCullFront = 16u;  // (46g) window beam rays cull triangles facing the ray start (the room shell from inside)
static const uint kFlagBeamCullBack = 32u;   // (46g) ... or the ones facing away
static const float kStickyBonus = 1.5;
static const uint kChurnWord = 1u;           // (46g) RayCounter words: [1] pixels whose source set changed
static const uint kBeamWordBase = 64u;       // ... from 64: per slot, sampled beam cone pixels traced / blocked
static const uint kSourceISL = 1u;
static const uint kSourceSpot = 8u;   // (46c) RayTracingPointLights::kSourceSpot
static const uint kSourceBeam = 16u;  // (46c) RayTracingPointLights::kSourceBeam
static const uint kSourceNoPlayer = 32u;  // (46d) RayTracingPointLights::kSourceNoPlayer: the player's own light
static const uint kSceneMask = 0x01u;
static const uint kCharacterMasks = 0x06u;
static const uint kNpcMask = 0x04u;  // (46d) the other actors only (Backend::kNpcMask): rays of the player's own lights
static const float kFirstPersonNear = 96.0;
static const float kScaledUnitsSq = 0.8 * 70.0 * 70.0;  // InverseSquareLighting SCALED_UNITS_SQ
// (46o) Soft window beams: their blocked samples encode sqrt(blocker distance / kBeamBlockerRange) instead of the ratio to the
// (far away) light; PointDenoiseCS turns it into the penumbra width blocker distance x tan(angle). Same constant there.
static const float kBeamBlockerRange = 2048.0;

#if defined(RT_ALPHA)
// (46c) Alpha-tested instances (instance ID = 1 + record), as TraceShadowsCS RT_ALPHA: a candidate blocks when the
// alpha of its texture at the hit passes the shape's test.
static const uint kAlphaTextureSlots = 4096u;   // Backend::kAlphaTextureSlots
static const uint kAlphaFallbackBit = 0x80000000u;
static const uint kAlphaCandidateBudget = 24u;  // per ray; past it the ray counts as blocked (bounded cost)
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

StructuredBuffer<AlphaInfo> AlphaInfos : register(t4);
StructuredBuffer<uint2> AlphaUVs : register(t5);
Texture2DArray<float> AlphaLayers : register(t6);
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
		// Never below 256 texels across (coarser mips average a window's lattice away).
		const float maxLod = clamp(log2(size / 256.0), 0.0, max(float(levels) - 1.0, 0.0));
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

// Lighting.hlsl's attenuation (InverseSquareLighting::GetAttenuation, or 1 - (d / r)^2).
float Attenuation(PLSource s, float d)
{
	if (d >= s.PosRadius.w)
		return 0.0;
	[branch] if (s.Info.x & kSourceISL)
	{
		const float t = saturate((s.PosRadius.w - d) * s.Atten.y);
		return kScaledUnitsSq / (d * d + s.Atten.z) * t * t * (3.0 - 2.0 * t);
	}
	const float f = d * s.Atten.x;
	return 1.0 - f * f;
}

// (46c) Spot sources: the share of the light the engine's spot mask lets through at P (0 outside the cone).
float SpotFactor(PLSource s, float3 P)
{
	[branch] if (!(s.Info.x & kSourceSpot))
		return 1.0;
	const float3 v = P - s.PosRadius.xyz;
	const float len = length(v);
	const float c = len > 1e-3 ? dot(v / len, s.Spot.xyz) : 0.0;
	if (c <= 1e-4)
		return 0.0;
	const float tanAngle = sqrt(saturate(1.0 - c * c)) / c;
	const float r = tanAngle / max(s.Spot.w, 1e-4);
	if (r >= 1.0)
		return 0.0;
	return saturate(1.0 - pow(max(r, 1e-6), max(asfloat(s.Info.z), 0.01)));
}

// (46c) Distance along dir from origin to where the ray leaves the interior box (Rect xy, Bounds xy as z).
float ExitDistance(float3 origin, float3 dir)
{
	const float3 lo = float3(C.Rect.x, C.Rect.y, C.Bounds.x);
	const float3 hi = float3(C.Rect.z, C.Rect.w, C.Bounds.y);
	float tExit = 1e30;
	[unroll] for (uint a = 0u; a < 3u; ++a)
	{
		if (dir[a] > 1e-6)
			tExit = min(tExit, (hi[a] - origin[a]) / dir[a]);
		else if (dir[a] < -1e-6)
			tExit = min(tExit, (lo[a] - origin[a]) / dir[a]);
	}
	return max(tExit, 0.0) + C.Bounds.z;
}

bool Blocked(float3 origin, float3 dir, float tMax, uint mask, float tMin, float footprint, uint rayFlags, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = tMax;
#if defined(RT_ALPHA)
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, rayFlags, mask, ray);
	uint budget = kAlphaCandidateBudget;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (budget == 0u || AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint))
			q.CommitNonOpaqueTriangleHit();
		else
			budget -= 1u;
	}
#else
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline(Scene, rayFlags, mask, ray);
	q.Proceed();
#endif
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// (46e) The character ray of a light a character carries (an NPC's torch): that character's instances (their
// InstanceContributionToHitGroupIndex = its key, Backend) are not tested. Every candidate comes back (forced
// non-opaque); the opaque instances (ID 0) block as usual, the alpha-tested ones are tested against their cut-out
// (RT_ALPHA) or ignored (without it they are culled in Blocked() too).
// (46q) ownerTMax >= 0 (Carrier Casts Shadows from Its Own Light): the carrier's own instances do block, but only where the ray
// meets them before ownerTMax (farther than the carrier clearance from the light): the hand and arm holding the torch do not.
bool BlockedExcept(float3 origin, float3 dir, float tMax, uint mask, float tMin, float footprint, uint owner, float ownerTMax, out float tHit)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
#if defined(RT_ALPHA)
	uint budget = kAlphaCandidateBudget;
#endif
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (q.CandidateInstanceContributionToHitGroupIndex() == owner && (ownerTMax < 0.0 || q.CandidateTriangleRayT() >= ownerTMax))
			continue;
		const uint id = q.CandidateInstanceID();
		bool blocks = id == 0u;
#if defined(RT_ALPHA)
		if (!blocks) {
			if (budget == 0u || AlphaBlocks(id, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint))
				blocks = true;
			else
				budget -= 1u;
		}
#endif
		if (blocks) {
			q.CommitNonOpaqueTriangleHit();
			q.Abort();
		}
	}
	tHit = q.CommittedRayT();
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
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

groupshared float3 gsMin[64];
groupshared float3 gsMax[64];
groupshared uint gsMask[2];

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint gi : SV_GroupIndex)
{
	// No early return before the barriers: every thread reaches them.
	const int2 p = int2(dtid.xy);
	const uint flags = asuint(C.Params.z);
	const uint scan = min(uint(C.Params.x), 64u);
	const uint K = clamp(uint(C.Params.w), 1u, 4u);
	const bool inside = all(float2(p) < C.RenderSize.xy);
	const float depth = inside ? abs(DepthMask[p]) : 0.0;
	bool valid = inside && depth < 0.999999 && depth > 0.0;
	float3 P = 0.0;
	if (valid) {
		P = PositionAt(p, depth);
		valid = !(P.x < C.Rect.x || P.y < C.Rect.y || P.x > C.Rect.z || P.y > C.Rect.w);
	}

	// The group's box, then the sources whose sphere reaches it.
	gsMin[gi] = valid ? P : float3(1e30, 1e30, 1e30);
	gsMax[gi] = valid ? P : float3(-1e30, -1e30, -1e30);
	if (gi < 2u)
		gsMask[gi] = 0u;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint s = 32u; s > 0u; s >>= 1u)
	{
		if (gi < s) {
			gsMin[gi] = min(gsMin[gi], gsMin[gi + s]);
			gsMax[gi] = max(gsMax[gi], gsMax[gi + s]);
		}
		GroupMemoryBarrierWithGroupSync();
	}
	if (gi < scan && gsMin[0].x <= gsMax[0].x) {
		const PLSource src = Sources[gi];
		const float3 c = clamp(src.PosRadius.xyz, gsMin[0], gsMax[0]);
		const float3 e = c - src.PosRadius.xyz;
		if (src.Atten.w > 0.0 && dot(e, e) < src.PosRadius.w * src.PosRadius.w)
			InterlockedOr(gsMask[gi >> 5], 1u << (gi & 31u));
	}
	GroupMemoryBarrierWithGroupSync();

	if (!inside)
		return;
	// (46g) Last frame's sources of this pixel (read before it is overwritten): stickiness and the churn count.
	const uint4 prevIdx = uint4(PLIdx[p] * 255.0 + 0.5) & 0x7Fu;
	const bool sticky = (flags & kFlagSticky) != 0u;
	const uint2 mask = uint2(gsMask[0], gsMask[1]);
	if (!valid || all(mask == 0u)) {
		PLVis[p] = 1.0;
		PLIdx[p] = 0.0;
		const uint emptied = WaveActiveCountBits(any(prevIdx != 0u));
		if (WaveIsFirstLane() && emptied > 0u) {
			uint previousChurn;
			RayCounter.InterlockedAdd(kChurnWord * 4u, emptied, previousChurn);
		}
		return;
	}

	// Normal from depth: per axis the neighbour with the smaller depth step (as the sun trace).
	const int2 maxP = int2(C.RenderSize.xy) - 1;
	const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
	const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
	const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
	const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - PositionAt(pl, dl) : PositionAt(pr, dr) - P;
	const float3 dy = abs(du - depth) < abs(dd - depth) ? P - PositionAt(pu, du) : PositionAt(pd, dd) - P;
	const float dist = length(P);
	const float3 V = dist > 0.0 ? -P / dist : float3(0, 0, 1);
	float3 N = cross(dy, dx);
	const float nLen = length(N);
	N = nLen > 1e-12 ? N / nLen : V;
	if (dot(N, V) < 0.0)
		N = -N;

	// Top K by contribution.
	float best[4] = { 0.0, 0.0, 0.0, 0.0 };      // ranking value (x kStickyBonus for last frame's sources)
	float bestC[4] = { 0.0, 0.0, 0.0, 0.0 };     // (46g) the contribution itself
	uint bestIdx[4] = { 0u, 0u, 0u, 0u };
	float total = 0.0;
	uint count = 0u;
	[loop] for (uint word = 0u; word < 2u; ++word)
	{
		uint bits = mask[word];
		[loop] while (bits != 0u)
		{
			const uint bit = firstbitlow(bits);
			bits &= bits - 1u;
			const uint i = word * 32u + bit;
			const PLSource src = Sources[i];
			const float3 toLight = src.PosRadius.xyz - P;
			const float d = length(toLight);
			const float ndl = d > 1e-3 ? dot(N, toLight / d) : 0.0;
			if (ndl <= 0.0)
				continue;
			// (46c) Window beams: the sun through a window, no fall-off with distance; spots: inside their cone only.
			const float atten = (src.Info.x & kSourceBeam) ? 1.0 : Attenuation(src, d);
			const float contribution = src.Atten.w * atten * ndl * SpotFactor(src, P);
			if (contribution <= 0.0)
				continue;
			total += contribution;
			count += 1u;
			// Insert into the sorted top K. (46g) Last frame's sources of the pixel rank higher (stable selection).
			const bool wasThere = sticky && any(prevIdx == (i + 1u));
			float v = contribution * (wasThere ? kStickyBonus : 1.0);
			float vc = contribution;
			uint vi = i + 1u;
			[unroll] for (uint k = 0u; k < 4u; ++k)
			{
				if (k < K && v > best[k]) {
					const float tv = best[k];
					const float tc = bestC[k];
					const uint ti = bestIdx[k];
					best[k] = v;
					bestC[k] = vc;
					bestIdx[k] = vi;
					v = tv;
					vc = tc;
					vi = ti;
				}
			}
		}
	}

	const float bias = C.Bias.x + C.Bias.y * dist;
	const float3 origin = P + N * bias;
	const float n0 = InterleavedGradientNoise(float2(p), uint(C.Params.y));
	const float n1 = InterleavedGradientNoise(float2(p.yx) + float2(29.0, 61.0), uint(C.Params.y) * 7u + 3u);
	const bool characters = (flags & kFlagCharacters) && !((flags & kFlagFirstPerson) && dist < kFirstPersonNear);
	float threshold = max(total * C.Extra.x, C.Extra.y);
	float stickyThreshold = max(total * C.Extra.x * 0.5, C.Extra.y);  // (46g) last frame's sources: half the share cut
	// The sun gate (outside the traced area the sun trace wrote 1: lit).
	[branch] if (C.Sun.w > 0.0)
	{
		const float sunTerm = C.Sun.w * saturate(dot(N, C.Sun.xyz)) * SunVisibility[p].x;
		threshold = max(threshold, sunTerm * C.Extra.z);
		stickyThreshold = max(stickyThreshold, sunTerm * C.Extra.z);
	}
	const uint beamCull = (flags & kFlagBeamCullFront) ? RAY_FLAG_CULL_FRONT_FACING_TRIANGLES : ((flags & kFlagBeamCullBack) ? RAY_FLAG_CULL_BACK_FACING_TRIANGLES : RAY_FLAG_NONE);
	const bool beamSample = ((p.x & 3) | (p.y & 3)) == 0;  // (46g) 1 of 16 pixels feeds the beam counters
	float4 vis = 1.0;
	uint rays = 0u;
	uint4 idx = 0u;
	[unroll] for (uint k = 0u; k < 4u; ++k)
	{
		if (bestIdx[k] == 0u || bestC[k] < ((sticky && any(prevIdx == bestIdx[k])) ? stickyThreshold : threshold))
			continue;
		const PLSource src = Sources[bestIdx[k] - 1u];
		const bool isBeam = (src.Info.x & kSourceBeam) != 0u;
		// (46o) A soft window beam: the disc subtends the beam's angular size seen from the surface (like the sun's cone).
		const bool softBeam = isBeam && C.Bounds.w > 0.0;
		// A point on the emitter's disc facing the surface (R2 step per slot: the slots decorrelate).
		const float3 L0 = src.PosRadius.xyz - origin;
		const float len0 = length(L0);
		const float3 Ln = L0 / max(len0, 1e-3);
		const float3 helper = abs(Ln.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
		const float3 T = normalize(cross(helper, Ln));
		const float3 B = cross(Ln, T);
		const float u1 = frac(n0 + float(k) * 0.7548776662);
		const float u2 = frac(n1 + float(k) * 0.5698402910);
		float sn, cs;
		sincos(6.2831853 * u2, sn, cs);
		const float emitter = softBeam ? len0 * C.Bounds.w : C.Bias.w;
		const float3 target = src.PosRadius.xyz + (T * cs + B * sn) * (sqrt(u1) * emitter);
		const float3 L = target - origin;
		const float len = length(L);
		const float3 dir = L / max(len, 1e-3);
		float tMax = len - asfloat(src.Info.y);
		// (46c) A window beam's light is far outside: the ray ends where it leaves the captured interior.
		if (src.Info.x & kSourceBeam)
			tMax = min(tMax, ExitDistance(origin, dir));
		idx[k] = bestIdx[k];
		if (tMax <= 1.0)
			continue;  // the surface is inside the clearance: lit
		float tHit;
		const float footprint = dist * 2.0 * C.RenderSize.w;  // about a pixel's size there (alpha mip choice)
		bool blocked = Blocked(origin, dir, tMax, kSceneMask, 0.0, footprint, isBeam ? beamCull : RAY_FLAG_NONE, tHit);
		rays += 1u;
		if (!blocked && characters) {
			// (46d) The player's own light: the player's body (torch, arm, torso) is not tested, only the other actors.
			const bool ownLight = (src.Info.x & kSourceNoPlayer) != 0u;
			bool test = true;
			if (flags & kFlagCharCull)
				test = (!ownLight && SphereOnSegment(origin, dir, tMax, C.Spheres[0])) || SphereOnSegment(origin, dir, tMax, C.Spheres[1]);
			if (test) {
				// (46e) A light carried by a traced character (Info.w = its key): that character's body is not tested.
				const uint owner = src.Info.w & 0xFFFFFFu;
				// (46q) Carrier Casts Shadows (Extra.w >= 0): its body blocks except within the carrier clearance of the light.
				const float ownerTMax = C.Extra.w >= 0.0 ? max(len - C.Extra.w, 0.0) : -1.0;
				if (owner != 0u)
					blocked = BlockedExcept(origin, dir, tMax, ownLight ? kNpcMask : kCharacterMasks, C.Bias.z, footprint, owner, ownerTMax, tHit);
				else
					blocked = Blocked(origin, dir, tMax, ownLight ? kNpcMask : kCharacterMasks, C.Bias.z, footprint, RAY_FLAG_NONE, tHit);
				rays += 1u;
			}
		}
		if (blocked)
			vis[k] = min(sqrt(saturate(tHit / (softBeam ? kBeamBlockerRange : max(len, 1.0)))), 1.0) * (254.0 / 255.0);
		if (isBeam && beamSample) {
			const uint word = kBeamWordBase + (bestIdx[k] - 1u) * 2u;
			uint previousBeam;
			RayCounter.InterlockedAdd(word * 4u, 1u, previousBeam);
			if (blocked)
				RayCounter.InterlockedAdd((word + 1u) * 4u, 1u, previousBeam);
		}
	}
	// (46g) Churn: the pixel's source set (order aside) differs from last frame's.
	bool changed = false;
	[unroll] for (uint a = 0u; a < 4u; ++a)
	{
		changed = changed || (idx[a] != 0u && !any(prevIdx == idx[a])) || (prevIdx[a] != 0u && !any(idx == prevIdx[a]));
	}
	const uint churned = WaveActiveCountBits(changed);
	if (WaveIsFirstLane() && churned > 0u) {
		uint previousChurn;
		RayCounter.InterlockedAdd(kChurnWord * 4u, churned, previousChurn);
	}
	count = min(count, 15u);
	[unroll] for (uint b = 0u; b < 4u; ++b)
		idx[b] |= ((count >> b) & 1u) << 7;
	PLVis[p] = vis;
	PLIdx[p] = float4(idx) / 255.0;
	// One atomic per wave (the lanes that returned early traced nothing).
	const uint waveRays = WaveActiveSum(rays);
	if (WaveIsFirstLane() && waveRays > 0u) {
		uint previous;
		RayCounter.InterlockedAdd(0, waveRays, previous);
	}
}
