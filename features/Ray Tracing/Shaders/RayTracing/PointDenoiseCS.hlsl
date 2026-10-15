// (46b) Ray-traced point-light shadows: the denoiser, per slot (the Penumbra denoiser's idea per light).
//
// Input: this frame's trace (TracePointLightsCS): per pixel up to four slots, each a source slot id and
// one ray's result (1 = lit, < 1 = blocked, the value encoding sqrt(blocker / light distance)).
// Per slot of the pixel, only samples of the SAME source count (its id in any slot of the neighbour):
//   1. 3x3 neighbourhood: where every matching sample agrees (fully lit, fully shadowed) nothing is
//      filtered (contact shadows stay sharp, lit areas stay lit);
//   2. otherwise a 5x5 pattern whose step follows the penumbra width (emitter size x blocker distance /
//      (light distance - blocker distance), in pixels), Gaussian x plane distance weights;
//   3. the history: last frame's result reprojected with the camera (bilinear, each tap checked against
//      the distance it should have, its slot found by the source id), clamped to this frame's 3x3 range,
//      blended in (Params.y = this frame's weight).
// Output: Lighting reads OutVis / OutIdx (t40 / t41 of the pixel shaders, RayTracing/PointLightShadows.hlsli);
// they are also next frame's history (ping-pong), with OutDist.
// The debug views of the point lights are written here (after CombineCS cleared DebugColor).

#include "RayTracing/Common.hlsli"

cbuffer PointDenoiseCB : register(b1)
{
	float4 PLParams;  // x emitter radius, y this frame's weight in the history blend, z flags (asuint), w max filter radius (px)
	float4 PLExtra;   // x K, (46o) y / z window-beam source slots 0-31 / 32-63 (uint bits), w tan of the beams' angular size (0 = 46n)
	uint4 PLMotion[4];  // (46d) per source slot one byte (slot s: byte s % 4 of uint s / 4): history dropped this frame, 0 keep .. 255 none
};

Texture2D<unorm float4> PLVis : register(t0);
Texture2D<unorm float4> PLIdx : register(t1);
Texture2D<float> DepthMask : register(t2);
Texture2D<unorm float4> HistVis : register(t3);
Texture2D<unorm float4> HistIdx : register(t4);
Texture2D<float> HistDist : register(t5);

RWTexture2D<unorm float4> OutVis : register(u0);
RWTexture2D<unorm float4> OutIdx : register(u1);
RWTexture2D<float> OutDist : register(u2);
RWTexture2D<unorm float4> DebugColor : register(u3);

static const uint kPLFlagHistory = 1u;  // the history textures hold last frame
static const uint kPLFlagDebug = 2u;
// (46o) Soft window beams (TracePointLightsCS, same constant): a blocked sample is sqrt(blocker distance / kBeamBlockerRange).
static const float kBeamBlockerRange = 2048.0;
static const float kBeamNoHistoryRadiusPx = 4.0;  // a beam slot without usable history (moving character) is filtered at least this wide

// Point-light debug views (RayTracing::DebugView, after the sun's).
static const uint kViewPLCandidates = 12u;
static const uint kViewPLSources = 13u;
static const uint kViewPLRaw = 14u;
static const uint kViewPLDenoised = 15u;

uint4 Ids(float4 a_value)
{
	return uint4(a_value * 255.0 + 0.5) & 0x7Fu;
}

float3 Hue(uint a_id)
{
	// A distinct colour per source slot.
	const float h = frac(float(a_id) * 0.618034);
	return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

// The slot of a sample that holds this source: its raw value (false = the sample did not trace it).
bool Match(uint4 a_ids, float4 a_vis, uint a_id, out float o_raw)
{
	o_raw = 1.0;
	if (a_id == 0u)
		return false;
	if (a_ids.x == a_id) {
		o_raw = a_vis.x;
		return true;
	}
	if (a_ids.y == a_id) {
		o_raw = a_vis.y;
		return true;
	}
	if (a_ids.z == a_id) {
		o_raw = a_vis.z;
		return true;
	}
	if (a_ids.w == a_id) {
		o_raw = a_vis.w;
		return true;
	}
	return false;
}

// (46d) How much of a source's history is dropped this frame (0 keep .. 1 none): a new or moving source.
float HistoryDrop(uint a_id)
{
	const uint s = a_id - 1u;
	const uint word = PLMotion[s >> 4u][(s >> 2u) & 3u];
	return float((word >> ((s & 3u) * 8u)) & 0xFFu) / 255.0;
}

float Lit(float a_raw)
{
	return a_raw >= 0.999 ? 1.0 : 0.0;
}

// (46o) Is this source (id = slot + 1) a soft window beam this frame?
bool SoftBeam(uint a_id)
{
	if (a_id == 0u || PLExtra.w <= 0.0)
		return false;
	const uint s = a_id - 1u;
	const uint word = s < 32u ? asuint(PLExtra.y) : asuint(PLExtra.z);
	return ((word >> (s & 31u)) & 1u) != 0u;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;
	const uint flags = asuint(PLParams.z);
	const float4 idxRaw = PLIdx[p];
	const uint4 ids = Ids(idxRaw);
	const float depth = abs(DepthMask[p]);
	const float4 raw = PLVis[p];
	float4 result = 1.0;
	float dist = -1.0;

	[branch] if (!RTIsSky(depth) && any(ids != 0u))
	{
		const float3 P = RTPosition(p, depth);
		dist = length(P);
		const int2 maxP = int2(Size.xy) - 1;

		// Plane of this pixel (depth normal, smaller step per axis).
		float3 N;
		float footprint;
		{
			const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
			const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
			const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
			const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - RTPosition(pl, dl) : RTPosition(pr, dr) - P;
			const float3 dy = abs(du - depth) < abs(dd - depth) ? P - RTPosition(pu, du) : RTPosition(pd, dd) - P;
			N = cross(dy, dx);
			const float len = length(N);
			N = len > 1e-12 ? N / len : (dist > 0.0 ? -P / dist : float3(0, 0, 1));
			// World size of one pixel across the view ray (not the slanted surface: the penumbra is measured on it).
			footprint = max(length(RTPosition(pr, depth) - P), 1e-3);
		}
		const float planeTolerance = dist * 0.01 + 2.0;

		// 1. 3x3: matching samples, their range and the blocked ones' distance ratio.
		float4 sum = 0.0, weights = 0.0, lo = 2.0, hi = -1.0, qSum = 0.0, qCount = 0.0;
		[unroll] for (int k = 0; k < 9; ++k)
		{
			const int2 o = int2(k % 3 - 1, k / 3 - 1);
			const int2 q = clamp(p + o, int2(0, 0), maxP);
			const float dq = abs(DepthMask[q]);
			float wPlane = 0.0;
			if (!RTIsSky(dq))
				wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / planeTolerance);
			[branch] if (wPlane >= 0.05)
			{
				const uint4 idq = Ids(PLIdx[q]);
				const float4 vq = PLVis[q];
				const float w = wPlane / (1.0 + float(o.x * o.x + o.y * o.y));
				[unroll] for (uint s = 0u; s < 4u; ++s)
				{
					float r;
					if (Match(idq, vq, ids[s], r)) {
						const float v = Lit(r);
						sum[s] += v * w;
						weights[s] += w;
						lo[s] = min(lo[s], v);
						hi[s] = max(hi[s], v);
						if (v < 0.5) {
							const float t = r * (255.0 / 254.0);
							qSum[s] += t * t;
							qCount[s] += 1.0;
						}
					}
				}
			}
		}

		// 3a. (46o) The history taps first (read only; blended in after the spatial pass as before), so that a soft beam slot
		// without usable history (a moving character: rejected by distance) gets a wide spatial filter instead of 1 spp grain.
		float4 hv = 0.0, hw = 0.0;
		[branch] if (flags & kPLFlagHistory)
		{
			const float4 prevClip = mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0));
			const float2 prevUv = prevClip.w > 1e-4 ? float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5) : float2(-1.0, -1.0);
			[branch] if (all(prevUv >= 0.0) && all(prevUv < 1.0))
			{
				const float2 prevPos = prevUv * float2(Size.xy) - 0.5;
				const int2 base = int2(floor(prevPos));
				const float2 f = prevPos - float2(base);
				const float expected = length(P + CameraDelta.xyz);
				const float tolerance = expected * 0.02 + 8.0;
				[unroll] for (int t = 0; t < 4; ++t)
				{
					const int2 o = int2(t & 1, t >> 1);
					const int2 q = clamp(base + o, int2(0, 0), maxP);
					const float hd = HistDist[q];
					const float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
					[branch] if (hd > 0.0 && abs(hd - expected) <= tolerance && bw > 0.0)
					{
						const uint4 hid = Ids(HistIdx[q]);
						const float4 hvis = HistVis[q];
						[unroll] for (uint s = 0u; s < 4u; ++s)
						{
							float h;
							if (Match(hid, hvis, ids[s], h)) {
								hv[s] += h * bw;
								hw[s] += bw;
							}
						}
					}
				}
			}
		}

		// 2. Penumbra-wide pass for the slots whose neighbourhood disagrees.
		float4 radiusPx = 0.0;
		float4 spatial = 1.0;
		[unroll] for (uint s = 0u; s < 4u; ++s)
		{
			if (ids[s] != 0u && hi[s] > lo[s]) {
				[branch] if (SoftBeam(ids[s]))
				{
					// (46o) A window beam: the light is far away, the penumbra is blocker distance x 2 tan(half angle).
					const float qb = qCount[s] > 0.0 ? saturate(qSum[s] / qCount[s]) : 0.25;
					radiusPx[s] = clamp(qb * kBeamBlockerRange * PLExtra.w / footprint, 1.0, PLParams.w);
					if (hw[s] < 0.05)
						radiusPx[s] = max(radiusPx[s], min(kBeamNoHistoryRadiusPx, PLParams.w));
				}
				else
				{
					const float q = qCount[s] > 0.0 ? clamp(qSum[s] / qCount[s], 0.0, 0.95) : 0.25;
					const float width = 2.0 * PLParams.x * q / (1.0 - q);
					radiusPx[s] = clamp(0.5 * width / footprint, 1.0, PLParams.w);
				}
			}
			spatial[s] = weights[s] > 0.0 ? sum[s] / weights[s] : Lit(raw[s]);
		}
		const float maxRadius = max(max(radiusPx.x, radiusPx.y), max(radiusPx.z, radiusPx.w));
		[branch] if (maxRadius > 1.5)
		{
			const int stepPx = max(int(round(maxRadius * 0.5)), 1);
			float4 wsum = 0.0, wwt = 0.0;
			[loop] for (uint k = 0u; k < 25u; ++k)
			{
				const int2 o = int2(int(k % 5u) - 2, int(k / 5u) - 2);
				const int2 q = clamp(p + o * stepPx, int2(0, 0), maxP);
				const float dq = abs(DepthMask[q]);
				float wPlane = 0.0;
				if (!RTIsSky(dq))
					wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / planeTolerance);
				[branch] if (wPlane >= 0.05)
				{
					const uint4 idq = Ids(PLIdx[q]);
					const float4 vq = PLVis[q];
					const float r2 = float(o.x * o.x + o.y * o.y) * float(stepPx * stepPx);
					[unroll] for (uint s = 0u; s < 4u; ++s)
					{
						float r;
						if (radiusPx[s] > 1.5 && Match(idq, vq, ids[s], r)) {
							const float w = wPlane * exp(-2.0 * r2 / (radiusPx[s] * radiusPx[s]));
							wsum[s] += Lit(r) * w;
							wwt[s] += w;
						}
					}
				}
			}
			[unroll] for (uint s = 0u; s < 4u; ++s)
				if (radiusPx[s] > 1.5 && wwt[s] > 0.0)
					spatial[s] = wsum[s] / wwt[s];
		}

		// 3b. History blend (taps read in 3a; hw = 0 without usable history).
		result = spatial;
		[unroll] for (uint sh = 0u; sh < 4u; ++sh)
		{
			if (ids[sh] != 0u && hw[sh] >= 0.05) {
				const float lo3 = (lo[sh] <= 1.0 ? lo[sh] : spatial[sh]) - 0.1, hi3 = (hi[sh] >= 0.0 ? hi[sh] : spatial[sh]) + 0.1;
				// (46d) A moving source (a carried torch) keeps less history: its shadows move with it, nothing smears.
				result[sh] = lerp(clamp(hv[sh] / hw[sh], lo3, hi3), spatial[sh], lerp(PLParams.y, 1.0, HistoryDrop(ids[sh])));
			}
		}
		[unroll] for (uint e = 0u; e < 4u; ++e)
			if (ids[e] == 0u)
				result[e] = 1.0;
	}

	OutVis[p] = saturate(result);
	OutIdx[p] = idxRaw;
	OutDist[p] = dist;

	[branch] if (flags & kPLFlagDebug)
	{
		const uint view = Size.z;
		float4 debug = 0.0;
		if (view == kViewPLCandidates) {
			// Shadow sources that reach this pixel (its lit side), before the top K and the share cut.
			const uint4 bytes = uint4(idxRaw * 255.0 + 0.5);
			const uint n = ((bytes.x >> 7) & 1u) | (((bytes.y >> 7) & 1u) << 1) | (((bytes.z >> 7) & 1u) << 2) | (((bytes.w >> 7) & 1u) << 3);
			if (n == 1u)
				debug = float4(0.1, 1.0, 0.2, 0.45);
			else if (n == 2u)
				debug = float4(1.0, 0.9, 0.1, 0.5);
			else if (n >= 3u && n <= 4u)
				debug = float4(1.0, 0.5, 0.0, 0.55);
			else if (n > 4u)
				debug = float4(1.0, 0.1, 0.05, 0.6);
		} else if (view == kViewPLSources) {
			// Colour of the source in slot 0; diagonal stripes in the colour of slot 1 (slots 2-3: thinner stripes).
			if (ids.x != 0u) {
				debug = float4(Hue(ids.x), 0.55);
				const uint d = (uint(p.x) + uint(p.y)) % 12u;
				if (ids.y != 0u && d < 3u)
					debug = float4(Hue(ids.y), 0.6);
				else if (ids.z != 0u && d == 6u)
					debug = float4(Hue(ids.z), 0.6);
				else if (ids.w != 0u && d == 9u)
					debug = float4(Hue(ids.w), 0.6);
			}
		} else if (view == kViewPLRaw) {
			if (any(ids != 0u)) {
				float v = 1.0;
				[unroll] for (uint s = 0u; s < 4u; ++s)
					if (ids[s] != 0u)
						v *= raw[s] >= 0.999 ? 1.0 : 0.0;
				debug = float4(v.xxx, 1.0);
			}
		} else if (view == kViewPLDenoised) {
			if (any(ids != 0u)) {
				float v = 1.0;
				[unroll] for (uint s = 0u; s < 4u; ++s)
					if (ids[s] != 0u)
						v *= saturate(result[s]);
				debug = float4(v.xxx, 1.0);
			}
		}
		DebugColor[p] = debug;
	}
}
