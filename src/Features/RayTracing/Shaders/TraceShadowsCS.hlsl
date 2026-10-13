// (45, RT prototype) Ray-traced sun visibility, inline RayQuery (DXR 1.1), cs_6_5.
//
// Compiled at build time with the Windows SDK's dxc (see CMakeLists.txt, "Ray Tracing DXIL")
// and embedded in the DLL; it runs on the feature's own D3D12 queue, never on the game's D3D11
// device. Inputs are D3D11 textures shared with D3D12:
//   t1 DepthMask (R32_FLOAT): |value| = the game's depth after the depth prepass (the value the
//      engine's shadow mask pass read); negative where the engine's sun shadow mask is already 0.
// Output u0 (R8G8B8A8_UNORM): .x = sun visibility (1 lit, 0 shadow), .y = 1 where the surface is in the
// BLAS set (only computed for the coverage debug view).
//
// Positions are camera-relative (the game's CameraPosAdjust space): the TLAS is built with
// instance translations relative to the same camera position, so all ray math stays near 0.

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

	float coverage = 0.0;
	if (flags & kFlagCoverage) {
		const float probe = bias * 4.0 + C.Bias.z;
		coverage = AnyHit(P + V * probe, -V, probe * 2.0) ? 1.0 : 0.0;
	}

	if ((flags & kFlagSkipShadowed) && dm < 0.0) {
		Output[p] = float4(0.0, coverage, 0.0, 0.0);
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
		hits += AnyHit(origin, dir, C.Params.w) ? 1u : 0u;
	}

	Output[p] = float4(1.0 - float(hits) / float(rays), coverage, 0.0, 0.0);
}
