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
// (47k) "RTAO: Smooth Normals v2" (Flags.x): each candidate takes the best match in the 3x3 pixels around its spot in last frame's
// depth; the tolerances grow with the pixel footprint / max(N.V, 0.2) (slanted surfaces reconstruct less exactly); the fallback
// at low confidence is a depth-only smoothed normal (a sparse ring of depth normals around the pixel, weighted by how well they
// agree with its plane: the same surface, not across a real corner or a depth edge) instead of the raw facet. Off = 47j exactly.
// Motion vectors: B validates its own guess against last frame's depth, so whether kMOTION_VECTOR still holds last frame's vectors
// at this point (before the G-buffer pass) or was cleared, a wrong guess only lowers B's confidence (the debug view shows it).
//
// Output (shared R32_FLOAT, read by TraceAOCS kFlagSmoothNormals): two f16 of the octahedral world normal mapped to
// 0.25..0.75 (so the high half is always a normal float: no denormal flush, no NaN), packed as a uint; 0 = no smooth normal.
// (47k) Debug (Flags.y, u1 RGBA8): 1 confidence (green used, red fell back, blue no data), 2 Ns, 3 Ng (RGB = xyz * 0.5 + 0.5).

#include "Common/GBuffer.hlsli"

cbuffer AONormalCB : register(b0)
{
	row_major float4x4 InvViewProj;      // this frame's (jittered, camera-relative), as the trace's
	row_major float4x4 PrevViewProj;     // last frame's (jittered), relative to its own camera
	row_major float4x4 PrevInvViewProj;  // last frame's inverse
	row_major float4x4 PrevViewInverse;  // last frame's view-to-world (rotation part used)
	float4 CameraDelta;                  // xyz: this camera minus last frame's camera (world units), w unused
	float4 RenderSize;                   // xy size, zw 1 / size (the same this frame and last)
	uint4 Flags;                         // (47k) x 1 = v2; y debug view (0 none, 1 confidence, 2 Ns, 3 Ng)
	float4 SkipRect;                     // (47p) the trace's rect (camera-relative xy min / max)
	float4 SkipFade;                     // (47p) x: the trace's fade end (units), y: 1 = skip the pixels the trace never reads
};

Texture2D<float> Depth : register(t0);         // this frame's depth (after the prepass)
Texture2D<float> PrevDepth : register(t1);     // last frame's DepthMask (|value| = depth)
Texture2D<float4> PrevNormal : register(t2);   // last frame's NORMALROUGHNESS (view-space octahedral normal in xy)
Texture2D<float2> PrevMotion : register(t3);   // last frame's motion vectors (uv: previous = uv + motion)
RWTexture2D<float> Output : register(u0);
RWTexture2D<unorm float4> DebugOut : register(u1);  // (47k) only bound while a Smooth Normals debug view is on

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
// frame's camera space (the camera candidate: where it must be); a_tolerance: allowed distance (units). (47k) a_search: the best
// match (smallest distance) of the 3x3 pixels around it. o_data: last frame had a surface there (in screen, not sky, written).
float Candidate(float2 prevUv, float3 a_expected, float a_tolerance, float3 Ng, bool a_search, out float3 o_normal, out bool o_data)
{
	o_normal = Ng;
	o_data = false;
	if (any(prevUv <= 0.0) || any(prevUv >= 1.0))
		return 0.0;
	const int2 q0 = int2(prevUv * RenderSize.xy);
	int2 q = q0;
	float err = 1e30;
	const int2 maxQ = int2(RenderSize.xy) - 1;
	const int reach = a_search ? 1 : 0;
	[loop] for (int j = -reach; j <= reach; ++j)
	{
		[loop] for (int i = -reach; i <= reach; ++i)
		{
			const int2 qi = clamp(q0 + int2(i, j), int2(0, 0), maxQ);
			const float d = abs(PrevDepth[qi]);
			if (d <= 0.0 || d >= 0.999999)
				continue;
			const float e = length(PositionAt((float2(qi) + 0.5) * RenderSize.zw, d, PrevInvViewProj) - a_expected);
			if (e < err) {
				err = e;
				q = qi;
			}
		}
	}
	if (!(err < 1e29))
		return 0.0;  // sky
	const float4 raw = PrevNormal[q];
	if (all(raw.xy == 0.0))
		return 0.0;  // a cleared / never written G-buffer
	o_data = true;
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

// (47k) A forward-difference depth normal at pixel q (facing the camera); false where there is no surface.
bool RingNormal(int2 q, int2 maxP, out float3 o_pos, out float3 o_normal)
{
	o_pos = 0.0;
	o_normal = float3(0.0, 0.0, 1.0);
	const int2 qc = clamp(q, int2(1, 1), maxP - 1);
	const float d0 = Depth[qc];
	if (d0 <= 0.0 || d0 >= 0.999999)
		return false;
	const float dx = Depth[qc + int2(1, 0)], dy = Depth[qc + int2(0, 1)];
	o_pos = PositionAtPixel(qc, d0);
	const float3 n = cross(PositionAtPixel(qc + int2(0, 1), dy) - o_pos, PositionAtPixel(qc + int2(1, 0), dx) - o_pos);
	const float l = length(n);
	if (!(l > 1e-12))
		return false;
	o_normal = n / l;
	if (dot(o_normal, -o_pos) < 0.0)
		o_normal = -o_normal;
	return true;
}

// (47k) Depth-only smoothed normal: Ng plus a sparse ring (8 at 12 px) of depth normals that lie on the same smooth surface (close to
// this pixel's plane for their distance, and within ~45 degrees of Ng); a real corner (90 degrees) or a depth edge is left out.
float3 RingSmoothedNormal(int2 p, float3 P, float3 Ng, float footprint)
{
	static const float2 kRing[8] = { float2(1.0, 0.0), float2(0.7071, 0.7071), float2(0.0, 1.0), float2(-0.7071, 0.7071), float2(-1.0, 0.0),
		float2(-0.7071, -0.7071), float2(0.0, -1.0), float2(0.7071, -0.7071) };
	const int2 maxP = int2(RenderSize.xy) - 1;
	float3 sum = Ng;
	[loop] for (uint k = 0u; k < 8u; ++k)
	{
		float3 Q, Nq;
		[branch] if (RingNormal(p + int2(round(kRing[k] * 12.0)), maxP, Q, Nq))
		{
			const float reach = length(Q - P) + 2.0 * footprint;
			const float plane = abs(dot(Q - P, Ng));
			const float w = (1.0 - smoothstep(0.5 * reach, reach, plane)) * smoothstep(0.5, 0.8, dot(Nq, Ng));
			sum += Nq * w;
		}
	}
	const float l = length(sum);
	return l > 1e-5 ? sum / l : Ng;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(float2(p) >= RenderSize.xy))
		return;
	const bool v2 = (Flags.x & 1u) != 0u;
	const uint debugView = Flags.y;
	const float depth = Depth[p];
	uint packed = 0u;
	float4 debug = 0.0;
	[branch] if (depth > 0.0 && depth < 0.999999)
	{
		const float3 P = PositionAtPixel(p, depth);
		const float dist = length(P);
		// (47p) The trace reads this only where its weight is above 0 (inside its rect, nearer than its fade end): elsewhere "no
		// smooth normal", as for the sky. 64 units / 1 % of margin cover the two passes' float differences.
		const bool unused = SkipFade.y > 0.0 && debugView == 0u &&
		                    (dist >= SkipFade.x * 1.01 + 64.0 || P.x < SkipRect.x - 64.0 || P.y < SkipRect.y - 64.0 || P.x > SkipRect.z + 64.0 || P.y > SkipRect.w + 64.0);
		[branch] if (unused)
		{
			Output[p] = 0.0;
			return;
		}
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
		// (47k) v2: tolerances by the pixel's world size over the view angle (47j: by the distance only).
		const float footprint = length(PositionAt((float2(p) + float2(1.5, 0.5)) * RenderSize.zw, depth, InvViewProj) - P);
		const float slant = footprint / max(dot(Ng, V), 0.2);
		const float tolA = v2 ? 1.5 + 4.0 * slant : 0.004 * dist + 1.5;
		const float tolB = v2 ? 6.0 + 8.0 * slant : 0.008 * dist + 6.0;

		// A. Camera only: this point in last frame's camera space and screen.
		const float3 prevP = P + CameraDelta.xyz;
		const float4 clip = mul(PrevViewProj, float4(prevP, 1.0));
		float3 nA = Ng, nB = Ng;
		float cA = 0.0, cB = 0.0;
		bool dataA = false, dataB = false;
		[branch] if (clip.w > 1e-4)
		{
			const float2 uvA = float2(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5);
			cA = Candidate(uvA, prevP, tolA, Ng, v2, nA, dataA);
			// B. Constant velocity: last frame's motion at the point it moved from (two fixed-point steps from the camera guess).
			const float2 uv = (float2(p) + 0.5) * RenderSize.zw;
			float2 uvB = uvA;
			[unroll] for (int i = 0; i < 2; ++i)
			{
				const int2 q = int2(clamp(uvB, 0.0, 0.999999) * RenderSize.xy);
				uvB = uv + PrevMotion[q];
			}
			// A moving surface moved: allow its motion in a frame (a walking character: a few units) on top of the depth noise.
			cB = Candidate(uvB, prevP, tolB, Ng, v2, nB, dataB);
		}
		const float c = max(cA, cB);
		// (47k) v2: below full confidence the fallback is the ring-smoothed depth normal (only computed there).
		float3 fallback = Ng;
		[branch] if (v2 && c < 0.999)
			fallback = RingSmoothedNormal(p, P, Ng, footprint);
		float3 n = Ng;
		[branch] if (c > 0.0)
		{
			float3 ns = nA * cA + nB * cB;
			const float l = length(ns);
			ns = l > 1e-5 ? ns / l : Ng;
			n = normalize(lerp(fallback, ns, c));
		}
		else
			n = fallback;
		[branch] if (c > 0.0 || dot(n, Ng) < 0.99999)
		{
			const float2 e = OctEncode(n) * 0.25 + 0.5;
			packed = (f32tof16(e.x) << 16) | f32tof16(e.y);
		}
		[branch] if (debugView != 0u)
		{
			if (debugView == 1u) {
				debug = (dataA || dataB) ? float4(lerp(float3(1.0, 0.15, 0.1), float3(0.1, 1.0, 0.2), c), 1.0) : float4(0.1, 0.2, 1.0, 1.0);
			} else if (debugView == 2u) {
				// As TraceAOCS uses it: bent back toward Ng where they disagree by more than ~73 degrees.
				const float keep = smoothstep(0.1, 0.3, dot(n, Ng));
				const float3 b = normalize(lerp(Ng, n, keep));
				debug = float4(b * 0.5 + 0.5, 1.0);
			} else {
				debug = float4(Ng * 0.5 + 0.5, 1.0);
			}
		}
	}
	Output[p] = asfloat(packed);
	[branch] if (debugView != 0u)
		DebugOut[p] = debug;
}
