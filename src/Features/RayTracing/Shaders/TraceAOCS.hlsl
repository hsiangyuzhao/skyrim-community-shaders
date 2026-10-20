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
// Output (shared with D3D11; (47h) R16G16B16A16_FLOAT, or R8G8B8A8_UNORM where the GPU cannot share that -- kFlagDither8: the
// weight .g, the only value with many levels, then gets a +-0.5 LSB triangular dither so its distance fade has no 8-bit steps):
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
//   (46y) kFlagSingleRay: the long direction is one closest-hit ray over [0, L], classified by its hit distance (the same
//   near / far values with one ray instead of two; past the alpha budget the two segments are traced instead). kFlagHalfFar: the
//   far part only on a checkerboard that swaps every frame; elsewhere .b = 0 (no far sample), filled by the filter's weighted mean.
//
// Rays: the scene (instance mask 0x01) and, when characters are in the TLAS, a second ray against them (0x06, TMin =
// Extra.x against their own coarse polygons) where the segment passes a character's bounds. Without RT_ALPHA the
// alpha-tested instances (traced leaves / cut-outs) are culled; with it they are tested against their cut-out, at
// most kAlphaCandidateBudget candidates per ray (past it the ray counts as blocked: bounded cost). (46t) AO Alpha
// Budget: past the budget the ray may instead count as half open (kFlagAlphaHalf) or open (kFlagAlphaOpen).
//
// (47n) Fine Detail (kFlagDetail), Single Long Ray, the Character Self-Occlusion Fix and Smooth Normals v2 are always on (their
// off paths are gone; the flags stay as they were set). The 47l blue-noise rays (t6) are gone.
// (47n) kFlagDynamic "RTAO: Tight History Near Moving Things": .b carries whether any of the pixel's rays was blocked by a
// character (the character rays) or a moving scene instance (InstanceContributionToHitGroupIndex == kMovingKey): the far weight
// w (0, 0.5 or 1) is stored as 0.4 w, plus 0.6 for such a pixel (exact in 8 bits too; RTAOCommon.hlsli RawFarWeight / RawDynamic).
//
// (47h) kFlagSelfFix "RTAO: Character Self-Occlusion Fix": the character ray already starts Extra.x (6 units) along, yet near a
// character's silhouette the ray-traced (GPU-skinned) mesh is a little off the drawn one, so a ray from a body pixel can start just
// inside its own traced mesh and leave it through the inside of the surface (a dark band along the sides of the legs, strongest
// with the short detail rays). At a pixel that is a character (CharacterAt: a short probe from the camera side finds that
// character's traced mesh; it gives the owner key and which side of its triangles faces outward), the character rays skip that
// character's opaque triangles met from their inner side when the ray did not enter that character first (before the TMin): such a
// ray started inside its own traced mesh. The ray start also moves out by up to kSelfGrazeOffset more along the normal, the more the
// ray grazes the surface. A real contact (an arm against the torso, the other leg, also within the TMin) still counts, as in 47g;
// static geometry and every other pixel are unchanged. (Hits on the same character closer than 1.5-2 units are
// already ignored by the 6-unit character TMin; the band came from rays leaving their own mesh beyond it.)

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
RWTexture2D<float4> AOOut : register(u0);  // (47h) float: RGBA16F, or RGBA8 (kFlagDither8)
RWByteAddressBuffer RayCounter : register(u1);  // rays traced (monotonic, read back for the F7 trace)
Texture2D<float> AONormals : register(t5);   // (47j) kFlagSmoothNormals: AONormalPrepCS output (packed octahedral world normal, 0 = none)

static const uint kFlagCharacters = 1u;   // characters are in the TLAS: the character ray
static const uint kFlagFirstPerson = 2u;  // no character ray for pixels near the camera
static const uint kFlagDetail = 4u;       // (46t) two scales (see the top)
static const uint kFlagAlphaHalf = 8u;    // (46t) past the alpha candidate budget: half open
static const uint kFlagAlphaOpen = 16u;   // (46t) past the alpha candidate budget: open
static const uint kFlagSingleRay = 32u;   // (46y) two scales: the long direction as one closest-hit ray (see ClosestBlocker)
static const uint kFlagHalfFar = 64u;     // (46y) two scales: the far part only on a checkerboard that swaps every frame
static const uint kFlagDither8 = 128u;    // (47h) the output is RGBA8: dither the weight (.g)
static const uint kFlagSelfFix = 256u;    // (47h) RTAO: Character Self-Occlusion Fix (see the top)
static const uint kFlagSmoothNormals = 512u;  // (47j) RTAO: Smooth Normals (see main)
static const uint kFlagSmoothNormals2 = 1024u;  // (47k) RTAO: Smooth Normals v2 (with kFlagSmoothNormals): offsets along Ns, projection
static const uint kFlagDynamic = 2048u;         // (47n) RTAO: Tight History Near Moving Things (see the top)
static const uint kMovingKey = 0x800000u;       // (47n) Backend kMovingInstanceKey: a scene instance that moved since last frame
static const uint kFlagQuarterFar = 4096u;      // (47q) two scales: the far part on one pixel of each 2x2, rotating every frame
static const uint kMaxDetailRays = 4u;
static const float kSelfProbe = 4.0;          // (47h) CharacterAt: the probe reaches this far to either side of the pixel (units)
static const float kSelfGrazeOffset = 1.5;    // (47h) extra ray start offset at a character pixel for a ray along the surface (units)
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
	float TextureSize;    // (47q) the bindless texture's larger side and mip count (instead of GetDimensions; unused for copy layers)
	float TextureLevels;
	float4 UvTransform;
};

StructuredBuffer<AlphaInfo> AlphaInfos : register(t2);
StructuredBuffer<uint2> AlphaUVs : register(t3);  // (47q) per triangle: (uv0, uv1), (uv2, vertex alpha 0 | 1 << 8 | 2 << 16, 8 bits each)
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

float InterleavedGradientNoise(float2 p, uint frame)
{
	p += float(frame % 64u) * 5.588238;
	return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

// One ray against the scene (or the characters: a_mask), any hit ends it. 1 = open, 0 = blocked; (46t) past the alpha
// candidate budget 0 (46s), 0.5 (kFlagAlphaHalf) or 1 (kFlagAlphaOpen). tHit = where it was blocked (else TMax).
float Visibility(float3 origin, float3 dir, float tMin, float tMax, uint mask, float footprint, uint flags, out float tHit, inout bool dyn)
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
		dyn = dyn || (mask & kCharacterMasks) != 0u || q.CommittedInstanceContributionToHitGroupIndex() == kMovingKey;  // (47n)
		return 0.0;
	}
	return exceeded ? ((flags & kFlagAlphaOpen) ? 1.0 : 0.5) : 1.0;
#else
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	q.Proceed();
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		tHit = q.CommittedRayT();
		dyn = dyn || (mask & kCharacterMasks) != 0u || q.CommittedInstanceContributionToHitGroupIndex() == kMovingKey;  // (47n)
		return 0.0;
	}
	return 1.0;
#endif
}

// (46y, kFlagSingleRay) The nearest blocker on [tMin, tMax] (closest hit; cut-outs tested against their alpha as in Visibility):
// o_hit / o_tHit. Returns false when the ray went past the alpha candidate budget (twice Visibility's: it covers both segments);
// the caller then traces the two segments instead, so the near / far result is always the one the segments define.
bool ClosestBlocker(float3 origin, float3 dir, float tMin, float tMax, uint mask, float footprint, out bool o_hit, out float o_tHit, inout bool dyn)
{
	o_hit = false;
	o_tHit = tMax;
	if (!(tMin < tMax))
		return true;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = tMin;
	ray.TMax = tMax;
	bool complete = true;
#if defined(RT_ALPHA)
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	uint budget = kAlphaCandidateBudget * 2u;
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		if (budget == 0u) {
			complete = false;
			q.Abort();
		} else if (AlphaBlocks(q.CandidateInstanceID(), q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint)) {
			q.CommitNonOpaqueTriangleHit();
		} else {
			budget -= 1u;
		}
	}
#else
	RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_CULL_NON_OPAQUE> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, mask, ray);
	q.Proceed();
#endif
	if (complete && q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		o_hit = true;
		o_tHit = q.CommittedRayT();
		dyn = dyn || (mask & kCharacterMasks) != 0u || q.CommittedInstanceContributionToHitGroupIndex() == kMovingKey;  // (47n)
	}
	return complete;
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

// (47h) The character this pixel shows, for the self-occlusion fix (kFlagSelfFix): valid = the pixel is on a traced character,
// owner = its key (the instances' InstanceContributionToHitGroupIndex, Backend), outerFront = CandidateTriangleFrontFace() of its
// triangles seen from outside (the side the probe met first). Only the opaque instances (ID 0) count: hair cards are two-sided.
struct SelfInfo
{
	bool valid;
	uint owner;
	bool outerFront;
};

bool ProbeCharacter(float3 origin, float3 dir, out uint o_owner, out bool o_front)
{
	o_owner = 0u;
	o_front = true;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.0;
	ray.TMax = 2.0 * kSelfProbe;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMasks, ray);
	while (q.Proceed()) {
		if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && q.CandidateInstanceID() == 0u)
			q.CommitNonOpaqueTriangleHit();  // the nearest from the probe's start (the search goes on with a shorter ray)
	}
	if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
		return false;
	o_owner = q.CommittedInstanceContributionToHitGroupIndex();
	o_front = q.CommittedTriangleFrontFace();
	return true;
}

SelfInfo CharacterAt(float3 P, float3 V, float3 N)
{
	SelfInfo s;
	s.valid = false;
	s.owner = 0u;
	s.outerFront = true;
	// Along the view ray from the camera side first (the surface the camera sees), else along the normal (silhouettes, where the
	// traced mesh is off the drawn one sideways).
	if (ProbeCharacter(P + V * kSelfProbe, -V, s.owner, s.outerFront) || ProbeCharacter(P + N * kSelfProbe, -N, s.owner, s.outerFront))
		s.valid = true;
	return s;
}

bool PointInSphere(float3 P, float4 sphere)
{
	const float3 d = P - sphere.xyz;
	return sphere.w > 0.0 && dot(d, d) <= sphere.w * sphere.w;
}

// (47h) Visibility() / ClosestBlocker() against the characters for a pixel that is a character (SelfInfo), over [tStart, tMax] as
// the 47g ray (tStart >= Extra.x, the character TMin). The query starts at 0 so the ray's entries before the TMin are seen too;
// every candidate comes back (forced non-opaque). Rules, as 47g except one: an opaque hit (ID 0) in [tStart, tMax] blocks -- unless it
// is the pixel's own character seen from its inner side and the ray did not enter that character before the TMin (Extra.x): then the
// ray started inside its own traced mesh (the traced copy sits a little off the drawn one) and is leaving it, a false hit. With an
// entry before the TMin it is the far side of a nearby part of the same body (the other leg, an arm), real contact, as in 47g. The
// candidates come in no order, so such inner-side hits wait for the end. Alpha-tested instances as in Visibility / ClosestBlocker.
float CharacterVisibilitySelf(float3 origin, float3 dir, float tStart, float tMax, float footprint, uint flags, SelfInfo self, out float tHit)
{
	tHit = tMax;
	if (!(tStart < tMax))
		return 1.0;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMasks, ray);
	const float charTMin = C.Extra.x;
	bool enteredSelf = false;  // an outer-side hit on the own character before the TMin
	float innerT = tMax + 1.0; // the nearest own inner-side hit in [tStart, tMax]
#if defined(RT_ALPHA)
	uint budget = kAlphaCandidateBudget;
	bool exceeded = false;
#endif
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		const float t = q.CandidateTriangleRayT();
		const uint id = q.CandidateInstanceID();
		if (id == 0u) {
			const bool own = q.CandidateInstanceContributionToHitGroupIndex() == self.owner;
			const bool front = q.CandidateTriangleFrontFace();
			if (own && front == self.outerFront && t < charTMin)
				enteredSelf = true;
			if (t < tStart)
				continue;
			if (own && front != self.outerFront) {
				innerT = min(innerT, t);
				continue;
			}
			q.CommitNonOpaqueTriangleHit();
			q.Abort();
			continue;
		}
		if (t < tStart)
			continue;  // the 47g ray started at tStart: not a candidate there, no budget used
#if defined(RT_ALPHA)
		if (budget == 0u) {
			if (flags & (kFlagAlphaHalf | kFlagAlphaOpen)) {
				exceeded = true;
			} else {
				q.CommitNonOpaqueTriangleHit();
			}
			q.Abort();
		} else if (AlphaBlocks(id, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint)) {
			q.CommitNonOpaqueTriangleHit();
			q.Abort();
		} else {
			budget -= 1u;
		}
#endif
	}
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) {
		tHit = q.CommittedRayT();
		return 0.0;
	}
	if (enteredSelf && innerT <= tMax) {
		tHit = innerT;
		return 0.0;
	}
#if defined(RT_ALPHA)
	return exceeded ? ((flags & kFlagAlphaOpen) ? 1.0 : 0.5) : 1.0;
#else
	return 1.0;
#endif
}

bool CharacterClosestSelf(float3 origin, float3 dir, float tStart, float tMax, float footprint, SelfInfo self, out bool o_hit, out float o_tHit)
{
	o_hit = false;
	o_tHit = tMax;
	if (!(tStart < tMax))
		return true;
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.0;
	ray.TMax = tMax;
	bool complete = true;
	RayQuery<RAY_FLAG_FORCE_NON_OPAQUE | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
	q.TraceRayInline(Scene, RAY_FLAG_NONE, kCharacterMasks, ray);
	const float charTMin = C.Extra.x;
	bool enteredSelf = false;
	float innerT = tMax + 1.0;
#if defined(RT_ALPHA)
	uint budget = kAlphaCandidateBudget * 2u;
#endif
	while (q.Proceed()) {
		if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE)
			continue;
		const float t = q.CandidateTriangleRayT();
		const uint id = q.CandidateInstanceID();
		if (id == 0u) {
			const bool own = q.CandidateInstanceContributionToHitGroupIndex() == self.owner;
			const bool front = q.CandidateTriangleFrontFace();
			if (own && front == self.outerFront && t < charTMin)
				enteredSelf = true;
			if (t < tStart)
				continue;
			if (own && front != self.outerFront)
				innerT = min(innerT, t);
			else
				q.CommitNonOpaqueTriangleHit();  // the search goes on with a shorter ray (closest hit)
			continue;
		}
		if (t < tStart)
			continue;
#if defined(RT_ALPHA)
		if (budget == 0u) {
			complete = false;
			q.Abort();
		} else if (AlphaBlocks(id, q.CandidatePrimitiveIndex(), q.CandidateTriangleBarycentrics(), footprint)) {
			q.CommitNonOpaqueTriangleHit();
		} else {
			budget -= 1u;
		}
#endif
	}
	if (!complete)
		return false;
	float best = tMax + 1.0;
	if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
		best = q.CommittedRayT();
	if (enteredSelf)
		best = min(best, innerT);
	if (best <= tMax) {
		o_hit = true;
		o_tHit = best;
	}
	return true;
}


// One segment [tMin, tMax] of a direction: the scene, then (still open and passing a character's bounds) the characters
// (their ray starts at least Extra.x along). The darker of the two; tHit = where it was blocked. Counts its rays.
float Segment(float3 origin, float3 dir, float tMin, float tMax, float footprint, uint flags, bool characters, SelfInfo self, inout uint rays, out float tHit,
	inout bool dyn)
{
	float v = Visibility(origin, dir, tMin, tMax, kSceneMask, footprint, flags, tHit, dyn);
	rays += 1u;
	[branch] if (v > 0.0 && characters && (SphereOnSegment(origin, dir, tMax, C.Spheres[0]) || SphereOnSegment(origin, dir, tMax, C.Spheres[1])))
	{
		float tCharacter;
		bool unused = false;
		const float vc = self.valid ? CharacterVisibilitySelf(origin, dir, max(tMin, C.Extra.x), tMax, footprint, flags, self, tCharacter) :
		                              Visibility(origin, dir, max(tMin, C.Extra.x), tMax, kCharacterMasks, footprint, flags, tCharacter, unused);
		rays += 1u;
		if (vc < v) {
			v = vc;
			tHit = tCharacter;
		}
		dyn = dyn || vc < 1.0;  // (47n) a character blocked it

	}
	return v;
}

// (47j, 47k) Smooth Normals: a direction below the depth normal's plane laid onto it (lifted to 0.02 above it; the 47j mirror is gone
// since 47n: it flipped the directions next to the plane between the two sides of a facet edge). With Ns = Ng nothing changes.
float3 AboveNg(float3 d, float3 Ng, bool a_on)
{
	const float k = dot(d, Ng);
	return (a_on && k < 0.02) ? normalize(d - Ng * (k - 0.02)) : d;
}

// Cosine-weighted direction in the hemisphere around N (T, B: its tangent frame) from two uniform numbers.
float3 CosineDirection(float2 u, float3 T, float3 B, float3 N)
{
	const float r = sqrt(u.x);
	float sn, cs;
	sincos(6.2831853 * u.y, sn, cs);
	return normalize(T * (r * cs) + B * (r * sn) + N * sqrt(saturate(1.0 - u.x)));
}

// (47q) Thread groups of 16 x 8 (one wave of 32 covers 16 x 2 pixels: rays of neighbouring rows stay together) in a swizzled order:
// the groups are walked in vertical strips kTileGroups groups wide (NVIDIA's ThreadGroupTilingX), so groups running at the same time
// trace neighbouring screen areas (shared BVH nodes in the caches). Every pixel is still visited exactly once.
static const uint2 kGroupSize = uint2(16u, 8u);
static const uint kTileGroups = 8u;

uint2 SwizzledGroup(uint2 a_group)
{
	const uint2 grid = (uint2(C.RenderSize.xy) + kGroupSize - 1u) / kGroupSize;
	const uint flat = a_group.y * grid.x + a_group.x;
	const uint perStrip = kTileGroups * grid.y;
	const uint strip = flat / perStrip;
	const uint inStrip = flat - strip * perStrip;
	const uint lastWidth = grid.x - (grid.x / kTileGroups) * kTileGroups;  // the last strip may be narrower
	const uint width = (strip == grid.x / kTileGroups && lastWidth != 0u) ? lastWidth : kTileGroups;
	return uint2(strip * kTileGroups + inStrip % width, inStrip / width);
}

[numthreads(16, 8, 1)] void main(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
	const int2 p = int2(SwizzledGroup(gid.xy) * kGroupSize + gtid.xy);
	if (any(float2(p) >= C.RenderSize.xy))
		return;
	const uint flags = asuint(C.Params.y);
	const float depth = abs(DepthMask[p]);
	uint rays = 0u;
	float4 result = float4(1.0, 0.0, 0.4, 1.0);  // (47n) .b: far weight 1 (x 0.4), not traced
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
			// (47j) kFlagSmoothNormals: the hemisphere (every cosine direction, the Fine Detail ones too) is built around the smooth
			// normal Ns (AONormalPrepCS: last frame's G-buffer normal reprojected, already blended toward Ng by its confidence), bent
			// smoothly toward the depth normal N (= Ng) where they disagree; (47k, always since 47n) a direction below the Ng plane is
			// laid onto it and the ray start offsets follow Ns. Off: Ns = Ng (47i exactly).
			float3 Ns = N;
			const bool smoothOn = (flags & kFlagSmoothNormals) != 0u;
			[branch] if (smoothOn)
			{
				const uint packed = asuint(AONormals[p]);
				[branch] if (packed != 0u)
				{
					const float2 e = (f16tof32(uint2(packed >> 16, packed & 0xFFFFu)) - 0.5) * 4.0;
					float3 s = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
					if (s.z < 0.0)
						s.xy = (1.0 - abs(s.yx)) * float2(s.x >= 0.0 ? 1.0 : -1.0, s.y >= 0.0 ? 1.0 : -1.0);
					const float sl = length(s);
					[branch] if (sl > 1e-4)
					{
						s /= sl;
						const float keep = smoothstep(0.1, 0.3, dot(s, N));
						const float3 b = lerp(N, s, keep);
						const float bl = length(b);
						Ns = bl > 1e-4 ? b / bl : N;
					}
				}
			}
			const float3 helper = abs(Ns.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
			const float3 T = normalize(cross(helper, Ns));
			const float3 B = cross(Ns, T);
			const float3 dir = AboveNg(CosineDirection(float2(u1, u2), T, B, Ns), N, smoothOn);

			const float rayLength = C.Params.z;
			const float bias = C.Fade.z + C.Fade.w * dist;
			const float footprint = dist * 2.0 * C.RenderSize.w;  // about a pixel's size there (alpha mip choice)
			const bool characters = (flags & kFlagCharacters) && !((flags & kFlagFirstPerson) && dist < kFirstPersonNear);
			// (47h) Character Self-Occlusion Fix (always, 47n): is this pixel a character (only inside the character bounds)?
			SelfInfo self;
			self.valid = false;
			self.owner = 0u;
			self.outerFront = true;
			[branch] if (characters && (PointInSphere(P, C.Spheres[0]) || PointInSphere(P, C.Spheres[1])))
			{
				self = CharacterAt(P, V, N);
				rays += 1u;
			}
			// The ray start: the 47g offset along the normal ((47k) the smooth one; Ns = N without data); (47h) at a character pixel up
			// to kSelfGrazeOffset more for a grazing ray.
			const float3 Noff = Ns;
			const float3 origin = P + Noff * bias;
			const float3 origin0 = self.valid ? P + Noff * (bias + kSelfGrazeOffset * (1.0 - abs(dot(Noff, dir)))) : origin;
			// (46t) Two scales (always, 47n): the long direction's near and (where that is open) far segment, more near-only directions.
			const float shortLength = clamp(C.Extra.y, 1.0, rayLength);
			// (46y, kFlagHalfFar) The far part on half the pixels (checkerboard, swapped every frame); the others write far weight 0
			// (.b), which the filter's weighted mean already reads as "no far sample here" (as where the near part was blocked).
			// (47q, kFlagQuarterFar) One pixel of each 2x2 block (the down filter's block), a different one every frame (all four in 4
			// frames, in a crossing order); the others write far weight 0 as above.
			const uint quad = (uint(p.x) & 1u) | ((uint(p.y) & 1u) << 1);
			static const uint kQuarterOrder[4] = { 0u, 3u, 1u, 2u };
			const bool farHere = (flags & kFlagQuarterFar) ? quad == kQuarterOrder[frame & 3u] :
			                                               (!(flags & kFlagHalfFar) || (((uint(p.x) + uint(p.y) + frame) & 1u) == 0u));
			float near0 = 1.0;
			float far = 1.0;
			bool traced = false;
			bool dyn = false;  // (47n) a ray of this pixel was blocked by a character or a moving object
			// (46y, Single Long Ray: always, 47n) One closest-hit ray over [0, L]: blocked before D = near blocked (no far sample),
			// between D and L = near open and far blocked, nothing = both open: the two segments' result with one ray. The character ray
			// likewise, over [Extra.x, D] when the scene already blocks the far part, else over [Extra.x, L].
			[branch] if (farHere && shortLength < rayLength)
			{
				bool sceneHit, charHit = false;
				float tScene, tChar = rayLength;
				bool sceneDyn = false;
				bool complete = ClosestBlocker(origin0, dir, 0.0, rayLength, kSceneMask, footprint, sceneHit, tScene, sceneDyn);
				rays += 1u;
				const bool nearBlockedByScene = sceneHit && tScene < shortLength;
				const float charEnd = sceneHit ? shortLength : rayLength;
				[branch] if (complete && !nearBlockedByScene && characters && (SphereOnSegment(origin0, dir, charEnd, C.Spheres[0]) || SphereOnSegment(origin0, dir, charEnd, C.Spheres[1])))
				{
					bool unused = false;
					complete = self.valid ? CharacterClosestSelf(origin0, dir, max(0.0, C.Extra.x), charEnd, footprint, self, charHit, tChar) :
					                        ClosestBlocker(origin0, dir, max(0.0, C.Extra.x), charEnd, kCharacterMasks, footprint, charHit, tChar, unused);
					rays += 1u;
				}
				[branch] if (complete)
				{
					const bool nearBlocked = nearBlockedByScene || (charHit && tChar < shortLength);
					near0 = nearBlocked ? 0.0 : 1.0;
					far = (!nearBlocked && (sceneHit || charHit)) ? 0.0 : 1.0;
					dyn = sceneDyn || charHit;
					traced = true;
				}
			}
			[branch] if (!traced)
			{
				float tNear;
				near0 = Segment(origin0, dir, 0.0, shortLength, footprint, flags, characters, self, rays, tNear, dyn);
				far = 1.0;
				[branch] if (near0 > 0.0 && shortLength < rayLength && farHere)
				{
					float tFar;
					far = Segment(origin0, dir, shortLength, rayLength, footprint, flags, characters, self, rays, tFar, dyn);
				}
			}
			float nearSum = near0;
			const uint extra = min(uint(C.Extra.z), kMaxDetailRays);
			[loop] for (uint i = 1u; i <= extra; ++i)
			{
				// Offsets of the R2 sequence: the extra directions stay well spread around the first one.
				const float2 u = frac(float2(u1, u2) + float(i) * float2(0.7548776662, 0.5698402910));
				const float3 dirExtra = AboveNg(CosineDirection(u, T, B, Ns), N, smoothOn);
				const float3 originExtra = self.valid ? P + Noff * (bias + kSelfGrazeOffset * (1.0 - abs(dot(Noff, dirExtra)))) : origin;
				float tExtra;
				nearSum += Segment(originExtra, dirExtra, 0.0, shortLength, footprint, flags, characters, self, rays, tExtra, dyn);
			}
			// .b: the far weight x 0.4, + 0.6 for a pixel with a moving occluder (kFlagDynamic; see the top).
			const bool dynamicHere = dyn && (flags & kFlagDynamic) != 0u;
			result = float4(far, weight, (farHere ? near0 : 0.0) * 0.4 + (dynamicHere ? 0.6 : 0.0), nearSum / float(extra + 1u));			// (47h) RGBA8 output: the weight's distance fade gets a +-0.5 LSB triangular dither (two uniform numbers), only inside
			// the fade (exactly 0 and 1 stay exact).
			[branch] if ((flags & kFlagDither8) && result.g > 0.0 && result.g < 1.0)
			{
				const float d0 = InterleavedGradientNoise(float2(p) + float2(47.0, 17.0), frame);
				const float d1 = InterleavedGradientNoise(float2(p.yx) + float2(5.0, 91.0), frame * 3u + 1u);
				result.g = saturate(result.g + (d0 + d1 - 1.0) * (0.5 / 255.0));
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
