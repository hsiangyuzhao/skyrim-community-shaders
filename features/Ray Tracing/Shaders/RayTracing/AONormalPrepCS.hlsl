// (47j) "RTAO: Smooth Normals": the smooth (vertex-interpolated, normal-mapped) surface normal for this frame's RTAO trace.
//
// The trace runs right after the depth prepass, before this frame's G-buffer exists, so the normal comes from LAST frame's
// G-buffer (NORMALROUGHNESS still holds it until StartDeferred clears it), reprojected to this frame's pixels:
//   A. by the camera alone (exact for anything that does not move), and
//   B. by last frame's motion vectors at constant velocity (characters, moving objects): prevUv = uv + Motion[prevUv].
// Each candidate is checked against last frame's depth (the trace's DepthMask, |value|, before PrepareCS overwrites it) and
// against this frame's depth normal Ng; their confidences (smooth, never a hard switch) weight the two normals, and the result
// is blended toward Ng by that confidence: N = normalize(lerp(Ng, Ns, c)). Disoccluded, cleared or fast-moving pixels drift to
// Ng (the 47i look) instead of flickering between the two.
//
// Output (shared R32_FLOAT, read by TraceAOCS kFlagSmoothNormals): two f16 of the octahedral world normal mapped to
// 0.25..0.75 (so the high half is always a normal float: no denormal flush, no NaN), packed as a uint; 0 = no smooth normal.

#include "Common/GBuffer.hlsli"

cbuffer AONormalCB : register(b0)
{
	row_major float4x4 InvViewProj;      // this frame's (jittered, camera-relative), as the trace's
	row_major float4x4 PrevViewProj;     // last frame's (jittered), relative to its own camera
	row_major float4x4 PrevInvViewProj;  // last frame's inverse
	row_major float4x4 PrevViewInverse;  // last frame's view-to-world (rotation part used)
	float4 CameraDelta;                  // xyz: this camera minus last frame's camera (world units), w unused
	float4 RenderSize;                   // xy size, zw 1 / size (the same this frame and last)
};

Texture2D<float> Depth : register(t0);         // this frame's depth (after the prepass)
Texture2D<float> PrevDepth : register(t1);     // last frame's DepthMask (|value| = depth)
Texture2D<float4> PrevNormal : register(t2);   // last frame's NORMALROUGHNESS (view-space octahedral normal in xy)
Texture2D<float2> PrevMotion : register(t3);   // last frame's motion vectors (uv: previous = uv + motion)
RWTexture2D<float> Output : register(u0);

float3 PositionAt(float2 uv, float depth, float4x4 invViewProj)
{
	const float4 pos = mul(invViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0));
	return pos.xyz / pos.w;
}

float3 PositionAtPixel(int2 p, float depth)
{
	return PositionAt((float2(p) + 0.5) * RenderSize.zw, depth, InvViewProj);
}

float2 OctEncode(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 e = n.xy;
	if (n.z < 0.0)
		e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return e;
}

// One reprojection candidate at last frame's uv: its world normal and confidence (0..1). a_expected: this pixel's point in last
// frame's camera space (the camera candidate: where it must be); a_tolerance: allowed distance (units).
float Candidate(float2 prevUv, float3 a_expected, float a_tolerance, float3 Ng, out float3 o_normal)
{
	o_normal = Ng;
	if (any(prevUv <= 0.0) || any(prevUv >= 1.0))
		return 0.0;
	const int2 q = int2(prevUv * RenderSize.xy);
	const float prevDepth = abs(PrevDepth[q]);
	const float4 raw = PrevNormal[q];
	if (prevDepth <= 0.0 || prevDepth >= 0.999999 || all(raw.xy == 0.0))
		return 0.0;  // sky, or a cleared / never written G-buffer
	const float3 prevPos = PositionAt((float2(q) + 0.5) * RenderSize.zw, prevDepth, PrevInvViewProj);
	const float err = length(prevPos - a_expected);
	const float depthConf = 1.0 - smoothstep(a_tolerance, 3.0 * a_tolerance, err);
	const float3 nVS = GBuffer::DecodeNormal(raw.xy);
	float3 n = mul((float3x3)PrevViewInverse, nVS);
	const float len = length(n);
	if (!(len > 1e-4))
		return 0.0;
	n /= len;
	o_normal = n;
	// Normal agreement with the depth normal: a smooth normal differs from its facet by tens of degrees at most.
	const float agree = smoothstep(0.1, 0.4, dot(n, Ng));
	return depthConf * agree;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(float2(p) >= RenderSize.xy))
		return;
	const float depth = Depth[p];
	uint packed = 0u;
	[branch] if (depth > 0.0 && depth < 0.999999)
	{
		const float3 P = PositionAtPixel(p, depth);
		const float dist = length(P);
		// This frame's depth normal, exactly as TraceAOCS builds Ng (per axis the neighbour with the smaller depth step).
		const int2 maxP = int2(RenderSize.xy) - 1;
		const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
		const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
		const float dl = Depth[pl], dr = Depth[pr], du = Depth[pu], dd = Depth[pd];
		const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - PositionAtPixel(pl, dl) : PositionAtPixel(pr, dr) - P;
		const float3 dy = abs(du - depth) < abs(dd - depth) ? P - PositionAtPixel(pu, du) : PositionAtPixel(pd, dd) - P;
		const float3 V = dist > 0.0 ? -P / dist : float3(0, 0, 1);
		float3 Ng = cross(dy, dx);
		const float nLen = length(Ng);
		Ng = nLen > 1e-12 ? Ng / nLen : V;
		if (dot(Ng, V) < 0.0)
			Ng = -Ng;

		// A. Camera only: this point in last frame's camera space and screen.
		const float3 prevP = P + CameraDelta.xyz;
		const float4 clip = mul(PrevViewProj, float4(prevP, 1.0));
		float3 nA = Ng, nB = Ng;
		float cA = 0.0, cB = 0.0;
		[branch] if (clip.w > 1e-4)
		{
			const float2 uvA = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
			cA = Candidate(uvA, prevP, 0.004 * dist + 1.5, Ng, nA);
			// B. Constant velocity: last frame's motion at the point it moved from (two fixed-point steps from the camera guess).
			const float2 uv = (float2(p) + 0.5) * RenderSize.zw;
			float2 uvB = uvA;
			[unroll] for (int i = 0; i < 2; ++i)
			{
				const int2 q = int2(clamp(uvB, 0.0, 0.999999) * RenderSize.xy);
				uvB = uv + PrevMotion[q];
			}
			// A moving surface moved: allow its motion in a frame (a walking character: a few units) on top of the depth noise.
			cB = Candidate(uvB, prevP, 0.008 * dist + 6.0, Ng, nB);
		}
		const float c = max(cA, cB);
		[branch] if (c > 0.0)
		{
			float3 ns = nA * cA + nB * cB;
			const float l = length(ns);
			ns = l > 1e-5 ? ns / l : Ng;
			float3 n = normalize(lerp(Ng, ns, c));
			const float2 e = OctEncode(n) * 0.25 + 0.5;
			packed = (f32tof16(e.x) << 16) | f32tof16(e.y);
		}
	}
	Output[p] = asfloat(packed);
}
