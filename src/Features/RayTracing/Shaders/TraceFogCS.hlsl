// (49b) RT fog visibility, inline RayQuery (DXR 1.1), cs_6_5. Compiled at build time like TraceShadowsCS (CMakeLists.txt) and
// run on Ray Tracing's own D3D12 queue in its own list, after the reflection list (a fourth fence). The volumetric fog reads the
// result one frame later (reprojected with its own history matrices, VolumetricFogRTVisibilityCS), so no D3D11 pass waits for it in
// the frame it is traced.
//
// The froxel lattice is the near grid of Exponential Height Fog's volumetric fog (VolumetricFogCSCommon.hlsli): Grid.xyz cells, the
// grid uv spanning the render, slice depths from ZParams (VolumetricSliceToDepth), device depth from the camera's clip constants.
// Positions are camera-relative (the TLAS is built around the same camera position).
//
// Sun (default): per froxel one any-hit ray toward the sun from a jittered point inside the cell (a new point every frame; the fog's
// history averages them). Froxels behind the opaque surface of their tile (the farthest depth of five samples in the tile) are
// skipped. Output u0 (R8G8B8A8_UNORM, shared with D3D11), a 2D atlas: texel (x, y + Grid.y * (z / 2)), rg = slice z even, ba = slice z
// odd; r / b = visibility (1 lit, 0 blocked), g / a = how much to trust it (the hand-off band to the edge of the traced area x the RT
// fade-in; 0 = no data: behind the surface, outside the traced area). Alpha-tested casters (RT_ALPHA): at most kAlphaCandidateBudget
// candidates per ray, beyond that the ray counts as blocked (leaf canopies stay bounded).
//
// FOG_LIGHTS: half resolution in x / y (each texel is a 2x2 block of froxels), full z. Per texel the four point-light sources (the RT
// point-light shadow sources, RayTracing/PointLights.h; window beams included) that light its centre most (attenuation x intensity
// x spot cone, no facing term) get one short occlusion ray each (scene instances only, characters not tested). Output u1
// (R8G8B8A8_UNORM): visibility per slot k; u2 (R8G8B8A8_UNORM): source slot + 1 per k (byte; 0 = empty). Atlas texel
// (x, y + ceil(Grid.y / 2) * z).

struct FogConstants
{
	row_major float4x4 InvViewProj;  // FrameBuffer::CameraViewProjInverse (jittered, as the depth)
	float4 ToSun;                    // xyz to the sun (world axes), w max ray length
	uint4 Grid;                      // full-resolution near grid x, y, z; w flags
	float4 ZParams;                  // slice distribution: scale, offset, distribution; w the RT fade (0..1)
	float4 Camera;                   // SharedData::CameraData x, z, w (device depth from view depth); w frame index
	float4 Rect;                     // camera-relative xy min / max of the traced area
	float4 Lights;                   // x sources to scan, y hand-off band (units), z ray start offset, w beam exit margin
	float4 Bounds;                   // x / y camera-relative z min / max of the interior (beams), z / w unused
	float4 RenderSize;               // xy render size, zw 1 / size
};

ConstantBuffer<FogConstants> C : register(b0);
RaytracingAccelerationStructure Scene : register(t0);
Texture2D<float> DepthMask : register(t5);  // |value| = the game's depth after the depth prepass (PrepareCS)
RWTexture2D<unorm float4> FogSun : register(u0);
RWTexture2D<unorm float4> FogLightVis : register(u1);
RWTexture2D<unorm float4> FogLightId : register(u2);

static const uint kFlagInterior = 1u;  // the beams' rays end where they leave the interior box (Rect xy, Bounds xy as z)
static const uint kSlicesPerThread = 8u;
static const uint kSunMask = 0xF7u;    // every instance but terrain (TraceShadowsCS RT_SCENE_MASK)
static const uint kSceneMask = 0x01u;  // every instance but characters (TracePointLightsCS kSceneMask)

#if defined(FOG_LIGHTS)
struct PLSource
{
	float4 PosRadius;  // camera-relative position, radius
	float4 Atten;      // inverse radius, ISL fade zone, ISL size bias, intensity
	float4 Spot;       // spot: direction (light to surface), tan of the larger half width; aim target; room box (half floats)
	uint4 Info;        // flags, clearance (float bits), spot falloff exponent (float bits), carrier key
};
StructuredBuffer<PLSource> Sources : register(t1);
static const uint kSourceISL = 1u;
static const uint kSourceSpot = 8u;
static const uint kSourceBeam = 16u;
static const uint kSourceAim = 64u;
static const uint kSourceRoomBox = 256u;
static const float kScaledUnitsSq = 0.8 * 70.0 * 70.0;  // InverseSquareLighting SCALED_UNITS_SQ
static const uint kBeamCull = RAY_FLAG_CULL_FRONT_FACING_TRIANGLES;
#endif

#if defined(RT_ALPHA)
// As TraceShadowsCS RT_ALPHA (same registers and records).
static const uint kAlphaTextureSlots = 4096u;
static const uint kAlphaFallbackBit = 0x80000000u;
static const uint kAlphaCandidateBudget = 8u;  // per ray; past it the ray counts as blocked (the fog is coarse; bounded cost)
static const float kAlphaTextureWorld = 128.0;

struct AlphaInfo
{
	uint UvBase;
	uint Triangles;
	uint Texture;
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
		const float maxLod = clamp(log2(size / 64.0), 0.0, max(info.TextureLevels - 1.0, 0.0));
		const float lod = clamp(log2(size * footprintUV) + 0.5, 0.0, maxLod);
		alpha = AlphaTextures[NonUniformResourceIndex(info.Texture)].SampleLevel(AlphaSampler, uv, lod).a;
	}
	return alpha * info.AlphaScale * vertexAlpha >= info.Threshold;
}
#endif

bool Blocked(float3 origin, float3 dir, float tMax, uint mask, uint rayFlags, float footprint)
{
	RayDesc ray;
	ray.Origin = origin;
	ray.Direction = dir;
	ray.TMin = 0.0;
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
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> q;
	q.TraceRayInline(Scene, rayFlags, mask, ray);
	q.Proceed();
#endif
	return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

uint HashUint(uint x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// Three values in [0, 1) per cell and frame (a new point inside the cell every frame).
float3 CellJitter(uint3 cell, uint frame)
{
	const uint h0 = HashUint(cell.x * 73856093u ^ cell.y * 19349663u ^ cell.z * 83492791u ^ frame * 2654435761u);
	const uint h1 = HashUint(h0 + 0x9E3779B9u);
	return float3(float(h0 & 0xFFFFu), float(h0 >> 16), float(h1 & 0xFFFFu)) / 65536.0;
}

// VolumetricFogCommon.hlsli VolumetricSliceToDepth.
float SliceDepth(float slice)
{
	const float sliceExp = exp2(min(slice / max(C.ZParams.z, 1e-4), 120.0));
	return (sliceExp - C.ZParams.y) / max(C.ZParams.x, 1e-20);
}

float DeviceDepth(float viewDepth)
{
	return (C.Camera.x - C.Camera.z / max(viewDepth, 1e-3)) / C.Camera.y;
}

// Camera-relative position at grid uv and view depth (VolumetricFogCSCommon.hlsli ComputeCellWorldPosition, eye 0).
float3 GridPosition(float2 gridUV, float viewDepth)
{
	const float2 ndc = gridUV * float2(2.0, -2.0) + float2(-1.0, 1.0);
	const float4 pos = mul(C.InvViewProj, float4(ndc, DeviceDepth(viewDepth), 1.0));
	return pos.xyz / pos.w;
}

// The farthest opaque depth (device) of the screen tile [uv0, uv1): four inset corners and the centre. Sky = 1.
float TileFarDepth(float2 uv0, float2 uv1)
{
	const float2 inset = (uv1 - uv0) * 0.1;
	const float2 a = uv0 + inset, b = uv1 - inset, m = (uv0 + uv1) * 0.5;
	const int2 maxP = int2(C.RenderSize.xy) - 1;
	float d = 0.0;
	d = max(d, abs(DepthMask[clamp(int2(a * C.RenderSize.xy), 0, maxP)]));
	d = max(d, abs(DepthMask[clamp(int2(float2(b.x, a.y) * C.RenderSize.xy), 0, maxP)]));
	d = max(d, abs(DepthMask[clamp(int2(float2(a.x, b.y) * C.RenderSize.xy), 0, maxP)]));
	d = max(d, abs(DepthMask[clamp(int2(b * C.RenderSize.xy), 0, maxP)]));
	d = max(d, abs(DepthMask[clamp(int2(m * C.RenderSize.xy), 0, maxP)]));
	return d <= 0.0 ? 1.0 : d;
}

// The trust of a ray-traced result at P: 1 inside the traced area, fading to 0 over the hand-off band at its edge, x the RT fade-in.
float Coverage(float3 P)
{
	const float edge = min(min(P.x - C.Rect.x, C.Rect.z - P.x), min(P.y - C.Rect.y, C.Rect.w - P.y));
	return saturate(edge / max(C.Lights.y, 1.0)) * saturate(C.ZParams.w);
}

#if defined(FOG_LIGHTS)
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

// How much a source lights the air at P (TracePointLightsCS Contribution without the facing term; window beams: no fall-off, nothing
// past their radius).
float Rank(PLSource src, float3 P)
{
	[branch] if (src.Info.x & kSourceRoomBox)
	{
		const uint3 b = asuint(src.Spot.xyz);
		const float3 lo = float3(f16tof32(b.x), f16tof32(b.x >> 16), f16tof32(b.y));
		const float3 hi = float3(f16tof32(b.y >> 16), f16tof32(b.z), f16tof32(b.z >> 16));
		if (any(P < lo) || any(P > hi))
			return 0.0;
	}
	const float d = length(src.PosRadius.xyz - P);
	const bool beam = (src.Info.x & kSourceBeam) != 0u;
	if (beam && d >= src.PosRadius.w)
		return 0.0;
	const float atten = beam ? 1.0 : Attenuation(src, d);
	return max(src.Atten.w, 0.0) * atten * SpotFactor(src, P);
}

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
	return max(tExit, 0.0) + C.Lights.w;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const uint2 halfGrid = (C.Grid.xy + 1u) / 2u;
	if (any(dtid.xy >= halfGrid))
		return;
	const uint z0 = dtid.z * kSlicesPerThread;
	if (z0 >= C.Grid.z)
		return;
	const float2 invGrid = 1.0 / float2(C.Grid.xy);
	const float2 uv0 = float2(dtid.xy * 2u) * invGrid;
	const float2 uv1 = min(float2(dtid.xy * 2u + 2u) * invGrid, 1.0);
	const float farDepth = TileFarDepth(uv0, uv1);
	const uint frame = uint(C.Camera.w);
	const uint scan = min(uint(C.Lights.x), 64u);
	const bool interior = (C.Grid.w & kFlagInterior) != 0u;

	[loop] for (uint i = 0u; i < kSlicesPerThread; ++i)
	{
		const uint z = z0 + i;
		if (z >= C.Grid.z)
			break;
		const uint2 outP = uint2(dtid.x, dtid.y + halfGrid.y * z);
		float4 vis = 1.0.xxxx;
		uint4 ids = 0u.xxxx;
		const float frontDepth = SliceDepth(float(z));
		[branch] if (farDepth >= 0.999999 || DeviceDepth(frontDepth) <= farDepth)
		{
			const float3 j = CellJitter(uint3(dtid.xy, z), frame);
			const float2 gridUV = lerp(uv0, uv1, j.xy);
			const float viewDepth = SliceDepth(float(z) + j.z);
			const float3 P = GridPosition(gridUV, viewDepth);
			const float3 Pc = GridPosition((uv0 + uv1) * 0.5, SliceDepth(float(z) + 0.5));
			// The four sources that light the texel's centre most (insertion into a sorted list).
			float4 best = 0.0.xxxx;
			uint4 bestIdx = 0u.xxxx;
			[loop] for (uint s = 0u; s < scan; ++s)
			{
				const PLSource src = Sources[s];
				if (src.PosRadius.w <= 0.0)
					continue;
				const float r = Rank(src, Pc);
				if (r <= 1e-4 || r <= best.w)
					continue;
				uint at = 3u;
				[unroll] for (uint k = 3u; k > 0u; --k)
				{
					if (r > best[k - 1u]) {
						best[k] = best[k - 1u];
						bestIdx[k] = bestIdx[k - 1u];
						at = k - 1u;
					}
				}
				best[at] = r;
				bestIdx[at] = s + 1u;
			}
			const float footprint = max(viewDepth * 32.0 * C.RenderSize.w, 1.0);
			[loop] for (uint k = 0u; k < 4u; ++k)
			{
				if (bestIdx[k] == 0u)
					break;
				const PLSource src = Sources[bestIdx[k] - 1u];
				const bool beamLike = (src.Info.x & (kSourceBeam | kSourceAim)) != 0u;
				const float3 target = (src.Info.x & kSourceAim) ? src.Spot.xyz : src.PosRadius.xyz;
				const float3 L = target - P;
				const float len = length(L);
				const float3 dir = L / max(len, 1e-3);
				const float3 origin = P + dir * C.Lights.z;
				float tMax = len - C.Lights.z - max(asfloat(src.Info.y), 0.0);
				if (beamLike && interior)
					tMax = min(tMax, ExitDistance(origin, dir));
				ids[k] = bestIdx[k];
				if (tMax <= 1.0)
					continue;  // inside the fixture's clearance: lit
				if (Blocked(origin, dir, tMax, kSceneMask, beamLike ? kBeamCull : RAY_FLAG_NONE, footprint))
					vis[k] = 0.0;
			}
		}
		FogLightVis[outP] = vis;
		FogLightId[outP] = float4(ids) / 255.0;
	}
}
#else
[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= C.Grid.xy))
		return;
	const uint z0 = dtid.z * kSlicesPerThread;
	if (z0 >= C.Grid.z)
		return;
	const float2 invGrid = 1.0 / float2(C.Grid.xy);
	const float2 uv0 = float2(dtid.xy) * invGrid;
	const float2 uv1 = float2(dtid.xy + 1u) * invGrid;
	const float farDepth = TileFarDepth(uv0, uv1);
	const uint frame = uint(C.Camera.w);
	const float3 toSun = normalize(C.ToSun.xyz);

	[loop] for (uint i = 0u; i < kSlicesPerThread; i += 2u)
	{
		const uint zPair = z0 + i;
		if (zPair >= C.Grid.z)
			break;
		float4 outValue = float4(1.0, 0.0, 1.0, 0.0);
		[unroll] for (uint h = 0u; h < 2u; ++h)
		{
			const uint z = zPair + h;
			if (z >= C.Grid.z)
				break;
			const float frontDepth = SliceDepth(float(z));
			if (farDepth < 0.999999 && DeviceDepth(frontDepth) > farDepth)
				continue;  // behind the opaque surface of the tile: no data
			const float3 j = CellJitter(uint3(dtid.xy, z), frame);
			const float viewDepth = SliceDepth(float(z) + j.z);
			const float3 P = GridPosition(lerp(uv0, uv1, j.xy), viewDepth);
			const float trust = Coverage(P);
			if (trust <= 0.0)
				continue;
			const float footprint = max(viewDepth * 16.0 * C.RenderSize.w, 1.0);
			const bool blocked = Blocked(P + toSun * C.Lights.z, toSun, C.ToSun.w, kSunMask, RAY_FLAG_NONE, footprint);
			if (h == 0u)
				outValue.xy = float2(blocked ? 0.0 : 1.0, trust);
			else
				outValue.zw = float2(blocked ? 0.0 : 1.0, trust);
		}
		FogSun[uint2(dtid.x, dtid.y + C.Grid.y * (zPair / 2u))] = outValue;
	}
}
#endif
