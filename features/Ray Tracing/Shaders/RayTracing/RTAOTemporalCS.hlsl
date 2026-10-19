// (46f) Ray-traced ambient occlusion: the optional history blend, AFTER the spatial filter (hard constraint #3:
// the AO multiplies the ambient light, so it comes from this frame's rays through a spatial bilateral filter; a
// history may only refine that, clamped to this frame's filtered neighbourhood -- never a per-pixel average of
// unfiltered values, never anything of NRD).
//
// Current: this frame's filtered visibility (full render size). History: last frame's output of this pass,
// reprojected with the game's motion vectors (render-extent screen space, as Screen Space GI's contact AO).
// The history is clamped into the min / max of the current 3x3 neighbourhood (plus a small slack) before the
// blend, so a moving occluder leaves no dark trail beyond one frame's filtered range.
// (46t) The same pass for the detail term (RTAO_DETAIL_HISTORY: its own history, its own flag).
//
// (47l) RTAO_TEMPORAL_V2 "RTAO: Temporal v2": one pass for both terms (the large-scale one and, with kFlagDetail, the detail one):
//   - per pixel and term a history length n (u2 / t5, R8G8: n / 255); this frame's weight a = max(1 / (n + 1), Temporal.x);
//   - disocclusion: our own last-frame geometry (u3 / t6: linear depth f16 | octahedral world normal 8:8, written here every
//     frame for the next) against where this pixel was last frame (the camera's PrevViewProj: clip.w = the depth it must have had)
//     and this frame's world normal; the 2x2 bilinear history taps are weighted by that agreement (a tap of another surface counts
//     0); nothing usable = a new pixel (n = 0), a partial match lowers n;
//   - a statistical clamp of each term: mean +- Clamp.x x the standard deviation of this frame's filtered 5x5 (at least +- Clamp.z / Clamp.y),
//     widened to include this frame's own value (the large-scale term too: with the longer history its 47k min / max + 0.08 let an
//     occluder that left -- same surface, no disocclusion -- linger twice as long offline; the slack Params.z is unused here);
//   - history fix: while n < Temporal.z the current value is replaced (fading out as n grows) by a wider edge-aware blur of this
//     frame's filtered result (5x5 taps, 3 / 2 pixels apart): a fresh pixel starts from a smooth value, never from older frames.
//   Nothing is blurred after this blend (the composite reads it as it is).
// (47n) kFlagDynamicHistory "RTAO: Tight History Near Moving Things": a pixel whose own rays or a 3x3 neighbour's were blocked by a
//   character or a moving object this frame (TraceAOCS .b, t9), or that was such a pixel last frame (bit 7 of its stored large-scale
//   length), keeps at most 1 frame of history (n <= 1) and is clamped to mean +- 1 sd of this frame's 3x3 (at least the usual
//   minimum half width): the dark trail a walking character or a swinging door left behind is gone within a frame or two.
// (47n) Without Temporal v2 (the 47k history) kFlagDynamicHistory makes a pixel with the flag in its 3x3 take half of this frame and
//   no clamp slack (t3: the raw trace).
// (47n) Debug view "Temporal v2: History" (Size.w == kViewHistory, u4): red = no usable history (rejected), green = the clamp moved
//   the history, blue = history length / 32, whitened = the moving-occluder flag of this frame.

#include "RayTracing/RTAOCommon.hlsli"

#if defined(RTAO_TEMPORAL_V2)
// ---------------------------------------------------------------------------------------------------------------------
#	include "Common/GBuffer.hlsli"

Texture2D<unorm float> Current : register(t0);        // this frame's filtered large-scale visibility
Texture2D<unorm float> History : register(t1);        // last frame's output (large-scale)
Texture2D<float2> MotionVectors : register(t2);
Texture2D<unorm float> CurrentDetail : register(t3);  // this frame's filtered detail visibility (kFlagDetail)
Texture2D<unorm float> HistoryDetail : register(t4);  // last frame's detail output (kFlagDetailHistory)
Texture2D<unorm float2> HistoryLength : register(t5); // last frame's n / 255 (x large, y detail)
Texture2D<uint> PrevGeometry : register(t6);          // last frame's linear depth (f16, low half) and octahedral world normal (8:8, high half)
Texture2D<float> DepthTexture : register(t7);         // this frame's depth (after the prepass)
Texture2D<float4> NormalRoughness : register(t8);     // this frame's G-buffer normal (view space, octahedral)
RWTexture2D<unorm float> Output : register(u0);
RWTexture2D<unorm float> OutputDetail : register(u1);
RWTexture2D<unorm float2> LengthOut : register(u2);
RWTexture2D<uint> GeometryOut : register(u3);
Texture2D<float4> Raw : register(t9);                 // (47n) TraceAOCS' output: .b carries the moving-occluder flag (RawDynamic)
RWTexture2D<unorm float4> HistoryDebug : register(u4); // (47n) the debug view (only bound while it is on)

static const int kTileRadius = 6;  // the history fix reaches 2 x 3 pixels
static const int kTile = 8 + 2 * kTileRadius;
static const int kTileCount = kTile * kTile;
static const float kDepthTol = 0.02;      // relative, plus the slope and kDepthTolUnits
static const float kDepthTolUnits = 6.0;  // a moving surface moves a few units a frame along the view (a running character)
static const float kFixDepthTol = 0.02;

groupshared float g_large[kTileCount];
groupshared float g_detail[kTileCount];
groupshared float g_z[kTileCount];
groupshared float3 g_n[kTileCount];
groupshared uint g_dyn[kTileCount];  // (47n) this frame's moving-occluder flag per tile pixel

float LinearDepth(float a_depth)
{
	return Camera.w / max(Camera.x - a_depth * Camera.z, 1e-6);
}

float2 OctEncode(float3 n)
{
	n /= abs(n.x) + abs(n.y) + abs(n.z);
	float2 e = n.xy;
	if (n.z < 0.0)
		e = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return e;
}

float3 OctDecode(float2 e)
{
	float3 n = float3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0)
		n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
	return normalize(n);
}

uint PackGeometry(float a_z, float3 a_n)
{
	const uint2 e = uint2(round(saturate(OctEncode(a_n) * 0.5 + 0.5) * 255.0));
	return (f32tof16(min(a_z, 65000.0)) & 0xFFFFu) | (e.x << 16) | (e.y << 24);
}

void UnpackGeometry(uint a_packed, out float o_z, out float3 o_n)
{
	o_z = f16tof32(a_packed & 0xFFFFu);
	o_n = OctDecode(float2((a_packed >> 16) & 0xFFu, a_packed >> 24) / 255.0 * 2.0 - 1.0);
}

// A wider edge-aware blur of this frame's filtered value (the history fix): 5x5 taps a_step pixels apart, on the centre's
// surface (depth within the slope's reach, normal within ~35 degrees).
float FixBlur(bool a_detail, int a_centre, float a_z, float a_slope, float3 a_n, int a_step)
{
	float sum = 0.0, sumW = 0.0;
	[unroll] for (int dy = -2; dy <= 2; ++dy)
	{
		[unroll] for (int dx = -2; dx <= 2; ++dx)
		{
			const int slot = a_centre + (dy * kTile + dx) * a_step;
			const float reach = (float)(max(abs(dx), abs(dy)) * a_step);
			const float tol = kFixDepthTol * a_z + a_slope * reach * 1.5 + 1.0;
			const float spatial = exp(-(float)(dx * dx + dy * dy) / 4.5);
			const float w = (abs(g_z[slot] - a_z) <= tol && dot(g_n[slot], a_n) > 0.82) ? spatial : 0.0;
			sum += w * (a_detail ? g_detail[slot] : g_large[slot]);
			sumW += w;
		}
	}
	return sumW > 0.0 ? sum / sumW : (a_detail ? g_detail[a_centre] : g_large[a_centre]);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID)
{
	const int2 maxP = max(int2(Size.xy) - 1, int2(0, 0));
	const bool detail = (Size.z & kFlagDetail) != 0u;
	{
		const int2 tileOrigin = int2(gid.xy) * 8 - kTileRadius;
		const uint lane = gtid.y * 8 + gtid.x;
		for (uint slot = lane; slot < (uint)kTileCount; slot += 64) {
			const int2 q = clamp(tileOrigin + int2(slot % kTile, slot / kTile), int2(0, 0), maxP);
			g_large[slot] = Current[q];
			g_detail[slot] = detail ? CurrentDetail[q] : 1.0;
			const float d = DepthTexture[q];
			g_z[slot] = (d > 0.0 && d < 1.0) ? LinearDepth(d) : 0.0;
			g_n[slot] = float3(GBuffer::DecodeNormal(NormalRoughness[q].xy));
			g_dyn[slot] = ((Size.z & kFlagDynamicHistory) != 0u && RawDynamic(Raw[q].b)) ? 1u : 0u;
		}
	}
	GroupMemoryBarrierWithGroupSync();
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const int c = (int(gtid.y) + kTileRadius) * kTile + int(gtid.x) + kTileRadius;
	const float z = g_z[c];
	const float3 nView = g_n[c];
	float3 n = mul((float3x3)ViewInverse, nView);
	const float nLen = length(n);
	n = nLen > 1e-4 ? n / nLen : float3(0.0, 0.0, 1.0);
	// The depth slope (units per pixel; per axis the smaller one-sided step: an edge does not count).
	const float slope = max(min(abs(g_z[c - 1] - z), abs(g_z[c + 1] - z)), min(abs(g_z[c - kTile] - z), abs(g_z[c + kTile] - z)));

	// This frame's 5x5 mean and standard deviation of each term: the history is held within mean +- Clamp.x sd (at least
	// +- Clamp.z for the large-scale term, Clamp.y for the detail term), widened to include this frame's own value.
	const float current = g_large[c];
	const float currentDetail = g_detail[c];
	float m1 = 0.0, m2 = 0.0, d1 = 0.0, d2 = 0.0;
	[unroll] for (int ey = -2; ey <= 2; ++ey)
	{
		[unroll] for (int ex = -2; ex <= 2; ++ex)
		{
			const float s = g_large[c + ey * kTile + ex];
			m1 += s;
			m2 += s * s;
			const float t = g_detail[c + ey * kTile + ex];
			d1 += t;
			d2 += t * t;
		}
	}
	m1 /= 25.0;
	d1 /= 25.0;
	float halfWidth = max(Clamp.x * sqrt(max(m2 / 25.0 - m1 * m1, 0.0)), Clamp.z);
	float halfWidthDetail = max(Clamp.x * sqrt(max(d2 / 25.0 - d1 * d1, 0.0)), Clamp.y);
	// (47n) The moving-occluder flag of this frame in the 3x3 around the pixel, and the 3x3 statistics for its tight clamp.
	bool dynNow = false;
	float s1 = 0.0, s2 = 0.0, t1 = 0.0, t2 = 0.0;
	[unroll] for (int fy = -1; fy <= 1; ++fy)
	{
		[unroll] for (int fx = -1; fx <= 1; ++fx)
		{
			const int slot = c + fy * kTile + fx;
			dynNow = dynNow || g_dyn[slot] != 0u;
			s1 += g_large[slot];
			s2 += g_large[slot] * g_large[slot];
			t1 += g_detail[slot];
			t2 += g_detail[slot] * g_detail[slot];
		}
	}

	// Last frame: the 2x2 bilinear taps around where the motion vectors put this pixel, each weighted by how well its stored
	// geometry matches this surface (depth where the camera says it must have been, normal).
	float histLarge = current, histDetail = currentDetail;
	float nLarge = 0.0, nDetail = 0.0;
	bool dynPrev = false;  // (47n) last frame's flag at an agreeing history tap
	// This point (camera-relative) and its view depth by the matrices (1 / w of the inverse: the same measure as last frame's
	// clip.w below, so no near / far convention can bias the comparison). Stored for next frame.
	const float2 uv = (float2(p) + 0.5) / float2(Size.xy);
	const float4 h = mul(InvViewProj, float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, DepthTexture[p], 1.0));
	const float zView = (z > 0.0 && abs(h.w) > 1e-12) ? abs(1.0 / h.w) : 0.0;
	const bool geometryValid = (Size.z & kFlagGeometryValid) != 0u && zView > 0.0;
	[branch] if (geometryValid)
	{
		const float2 prevUv = uv + MotionVectors[p];
		// Where this point was relative to last frame's camera: its depth then (clip.w).
		const float3 P = h.xyz / h.w;
		const float zExpected = abs(mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0)).w);
		const float tol = kDepthTol * zExpected + 2.0 * slope + kDepthTolUnits;
		const float2 prevPix = prevUv * float2(Size.xy) - 0.5;
		const int2 base = int2(floor(prevPix));
		const float2 f = prevPix - float2(base);
		float sumW = 0.0, sumL = 0.0, sumD = 0.0, sumNL = 0.0, sumND = 0.0;
		[branch] if (all(prevUv > 0.0) && all(prevUv < 1.0) && zExpected > 0.0)
		{
			[unroll] for (int t = 0; t < 4; ++t)
			{
				const int2 o = int2(t & 1, t >> 1);
				const int2 q = base + o;
				if (any(q < 0) || any(q > maxP))
					continue;
				float zPrev;
				float3 nPrev;
				UnpackGeometry(PrevGeometry[q], zPrev, nPrev);
				const float bilinear = (o.x != 0 ? f.x : 1.0 - f.x) * (o.y != 0 ? f.y : 1.0 - f.y);
				const float agree = (1.0 - smoothstep(tol, 2.0 * tol, abs(zPrev - zExpected))) * smoothstep(0.6, 0.85, dot(nPrev, n));
				const float w = bilinear * agree;
				float2 len = HistoryLength[q] * 255.0;
				// (47n) Bit 7 of the large-scale length: that pixel had a moving occluder last frame.
				const bool tapDyn = len.x >= 127.5;
				len.x = tapDyn ? len.x - 128.0 : len.x;
				dynPrev = dynPrev || (tapDyn && w > 0.0);
				sumW += w;
				sumL += w * History[q];
				sumNL += w * len.x;
				if (detail) {
					sumD += w * HistoryDetail[q];
					sumND += w * len.y;
				}
			}
		}
		[branch] if (sumW > 0.02)
		{
			// A partial match (an edge of the surface, half the taps on another one) lowers the history's length.
			const float confidence = smoothstep(0.02, 0.6, sumW);
			histLarge = sumL / sumW;
			nLarge = (Size.z & kFlagHistory) ? sumNL / sumW * confidence : 0.0;
			histDetail = detail ? sumD / sumW : currentDetail;
			nDetail = (detail && (Size.z & kFlagDetailHistory)) ? sumND / sumW * confidence : 0.0;
		}
	}

	// (47n) Tight history near moving things: at most 1 frame of history and mean +- 1 sd of the 3x3 (at least the minimum widths).
	const bool dynamicPixel = (Size.z & kFlagDynamicHistory) != 0u && (dynNow || dynPrev);
	float lo, hi, dLo, dHi;
	[branch] if (dynamicPixel)
	{
		nLarge = min(nLarge, 1.0);
		nDetail = min(nDetail, 1.0);
		const float mean = s1 / 9.0, meanD = t1 / 9.0;
		halfWidth = max(sqrt(max(s2 / 9.0 - mean * mean, 0.0)), Clamp.z);
		halfWidthDetail = max(sqrt(max(t2 / 9.0 - meanD * meanD, 0.0)), Clamp.y);
		lo = min(mean - halfWidth, current);
		hi = max(mean + halfWidth, current);
		dLo = min(meanD - halfWidthDetail, currentDetail);
		dHi = max(meanD + halfWidthDetail, currentDetail);
	}
	else
	{
		lo = min(m1 - halfWidth, current);
		hi = max(m1 + halfWidth, current);
		dLo = min(d1 - halfWidthDetail, currentDetail);
		dHi = max(d1 + halfWidthDetail, currentDetail);
	}
	const bool rejected = nLarge <= 0.0;

	// History fix: a short history starts from a wider blur of this frame (fading out by Temporal.z frames).
	const float fixLarge = saturate(1.0 - nLarge / Temporal.z);
	float useLarge = current;
	[branch] if (fixLarge > 0.0)
		useLarge = lerp(current, FixBlur(false, c, z, slope, nView, 3), fixLarge);
	const float aLarge = max(1.0 / (nLarge + 1.0), Temporal.x);
	const float clampedLarge = clamp(histLarge, lo, hi);
	Output[p] = lerp(clampedLarge, useLarge, aLarge);

	float nDetailNew = 0.0;
	[branch] if (detail)
	{
		const float fixDetail = saturate(1.0 - nDetail / Temporal.z);
		float useDetail = currentDetail;
		[branch] if (fixDetail > 0.0)
			useDetail = lerp(currentDetail, FixBlur(true, c, z, slope, nView, 2), fixDetail);
		const float aDetail = max(1.0 / (nDetail + 1.0), Temporal.x);
		OutputDetail[p] = lerp(clamp(histDetail, dLo, dHi), useDetail, aDetail);
		nDetailNew = min(nDetail + 1.0, Temporal.w);
	}
	// (47n) Bit 7 of the large-scale length: this pixel's moving-occluder flag (n stays <= Temporal.w = 32).
	LengthOut[p] = float2(min(nLarge + 1.0, Temporal.w) + (dynNow ? 128.0 : 0.0), nDetailNew) / 255.0;
	[branch] if (Size.w == kViewHistory)
	{
		const bool clampActive = abs(histLarge - clampedLarge) > 0.002;
		float3 colour = float3(rejected ? 1.0 : 0.0, clampActive ? 1.0 : 0.0, saturate(nLarge / 32.0));
		if (dynNow)
			colour = lerp(colour, 1.0, 0.6);
		HistoryDebug[p] = float4(colour, 1.0);
	}
	GeometryOut[p] = zView > 0.0 ? PackGeometry(zView, n) : 0u;
}

#else
// ---------------------------------------------------------------------------------------------------------------------
#	if defined(RTAO_DETAIL_HISTORY)
static const uint kHistoryFlag = kFlagDetailHistory;
#	else
static const uint kHistoryFlag = kFlagHistory;
#	endif

Texture2D<unorm float> Current : register(t0);
Texture2D<unorm float> History : register(t1);
Texture2D<float2> MotionVectors : register(t2);
Texture2D<float4> Raw : register(t3);  // (47n) TraceAOCS' output: its moving-occluder flag (kFlagDynamicHistory)
SamplerState LinearClamp : register(s0);
RWTexture2D<unorm float> Output : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	if (any(dtid.xy >= Size.xy))
		return;
	const int2 p = int2(dtid.xy);
	const int2 maxP = int2(Size.xy) - 1;
	const float current = Current[p];
	float lo = current;
	float hi = current;
	bool dyn = false;  // (47n) a moving occluder in the 3x3 this frame: half of this frame, no clamp slack
	[unroll] for (int dy = -1; dy <= 1; ++dy)
	{
		[unroll] for (int dx = -1; dx <= 1; ++dx)
		{
			const int2 q = clamp(p + int2(dx, dy), int2(0, 0), maxP);
			const float s = Current[q];
			lo = min(lo, s);
			hi = max(hi, s);
			dyn = dyn || ((Size.z & kFlagDynamicHistory) != 0u && RawDynamic(Raw[q].b));
		}
	}
	float result = current;
	[branch] if (Size.z & kHistoryFlag)
	{
		const float2 uv = (float2(p) + 0.5) / float2(Size.xy);
		const float2 prevUv = uv + MotionVectors[p];
		if (all(prevUv >= 0.0) && all(prevUv <= 1.0)) {
			const float history = History.SampleLevel(LinearClamp, prevUv, 0);
			const float slack = dyn ? 0.0 : Params.z;
			const float clamped = clamp(history, max(lo - slack, 0.0), min(hi + slack, 1.0));
			result = lerp(clamped, current, dyn ? max(Params.y, 0.5) : Params.y);
		}
	}
	Output[p] = result;
}
#endif
