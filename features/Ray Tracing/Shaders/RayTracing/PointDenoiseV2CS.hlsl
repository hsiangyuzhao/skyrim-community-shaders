// (50b) Ray-traced point-light shadows: "Penumbra v2", the history-length-aware denoiser (Point Light Shadows > Denoiser), and the
// passes of the Off and DLSS RR choices. The old Penumbra denoiser (PointDenoiseCS.hlsl) is left exactly as it was.
//
// Same inputs as PointDenoiseCS (TracePointLightsCS: up to six slots per pixel, a source id and one ray's result each; the fractional
// encoding of Soft Hand-Off for Weak Lights), same outputs (OutVis / OutIdx / OutDist / OutWide: Lighting's t40 / t41 / t46 and next
// frame's history), plus per pixel and slot the history length n (OutLen, 4 bits per slot, slots 0 - 5; ping-pong with HistLen).
//
// Penumbra v2, per slot (SVGF / ReBLUR idea: the filter knows how many frames each pixel and light has accumulated):
//   - n comes with the history (the bilinear history taps that pass the distance test, the same source id); a slot whose source is new
//     this frame (PLMotion byte 255: a new or reassigned slot, or a light that jumped) has none. n counts up to kLenMax (15);
//   - youth y = 1 - n / kYouthFrames. The spatial pass: radius = min(the physical penumbra width (2 emitter q / (1 - q) for a point
//     light, blocker distance x 2 tan for a soft window beam; q = the blocked samples' distance ratio), lerp(PLParams.w, kYoungMaxRadius,
//     y)), Vogel-disc taps rotated by a per-pixel / per-frame hash (decorrelated from the trace's sampling): kYoungTaps for y >= 0.75,
//     kMidTaps for y >= 0.375, else kOldTaps (12, as the old filter). Never wider than the physical penumbra: contact shadows stay sharp.
//     Where the 3x3 agrees the wide pass runs only for a young slot or a partial history (0.02 .. 0.98); without a blocked sample in the
//     3x3 its blocker distance comes from the probe (kPoissonTaps taps at lerp(kProbeRadiusOld, kProbeRadiusYoung, y) px); none found = no
//     wide pass (the pixel is outside every penumbra). A young slot therefore starts from a wide, smooth estimate (the history fix of
//     RTAOTemporalCS: fading out as n grows, because the radius cap and the tap count shrink with y);
//   - temporal: the history is clamped to m +- kClampK sigma, sigma^2 = (m (1 - m) + 1 / Neff) / Neff (binary samples, m the spatial mean,
//     Neff its effective sample count; the 1 / Neff term keeps a band at m = 0 / 1 so a chance all-lit / all-blocked estimate does not snap
//     a penumbra history); n is decayed by how far the clamp moved the history (moved = |clamped - history| / band, n x (1 - moved));
//     this frame's weight a = max(1 / (n + 1), kMinWeight). This one rule replaces the old filter's HistoryDrop weighting, its 0.5 strong-
//     change rule, the moving-receiver exact snap and its minimum-radius special cases.
// Off (kPLFlagV2Off): the 3x3 plane-weighted mean only, no history (a reference: grainy).
// DLSS RR (kPLFlagV2RR): Lighting gets this frame's decoded rays (OutRaw / OutRawWide, nothing of ours: Ray Reconstruction cleans them);
//   OutVis / OutWide get a cheap filtered copy (the 3x3 mean with the clamped history, no wide pass) for the lit effects (Effect.hlsl reads
//   t40 / t46 there: RayTracing binds this copy for every effect draw and the raw rays again after it).
// Variants: PL_WIDE = slots 4 - 5 (K > 4 this frame), as PointDenoiseCS.

#include "RayTracing/Common.hlsli"

cbuffer PointDenoiseCB : register(b1)
{
	float4 PLParams;  // x emitter radius, y unused here, z flags (asuint), w max filter radius of a converged slot (px)
	float4 PLExtra;   // x K, y / z window-beam source slots 0-31 / 32-63 (uint bits), w tan of the beams' angular size (0 = none soft)
	uint4 PLMotion[4];  // per source slot one byte: history dropped this frame (255 = new / reassigned / jumped: no history)
	uint4 PLEmitter;  // x / y source slots 0-31 / 32-63 with their own emitter radius (uint bits), z that radius (float bits), w frame
	uint4 PLFillEmitter[4];  // (50a) Soft fill lights: per source slot one byte = emitter radius / 2 units, 0 = none
};

Texture2D<unorm float4> PLVis : register(t0);
Texture2D<unorm float4> PLIdx : register(t1);
Texture2D<float> DepthMask : register(t2);
Texture2D<unorm float4> HistVis : register(t3);
Texture2D<unorm float4> HistIdx : register(t4);
Texture2D<float> HistDist : register(t5);
Texture2D<float2> PrevMotion : register(t6);
Texture2D<unorm float4> PLWide : register(t7);
Texture2D<unorm float4> HistWide : register(t8);
Texture2D<uint> HistLen : register(t9);

RWTexture2D<unorm float4> OutVis : register(u0);
RWTexture2D<unorm float4> OutIdx : register(u1);
RWTexture2D<float> OutDist : register(u2);
RWTexture2D<unorm float4> DebugColor : register(u3);
RWTexture2D<unorm float4> OutWide : register(u4);
RWTexture2D<uint> OutLen : register(u5);
RWTexture2D<unorm float4> OutRaw : register(u6);
RWTexture2D<unorm float4> OutRawWide : register(u7);

// Flags (PLParams.z; the values PointDenoiseCS uses where they mean the same).
static const uint kPLFlagHistory = 1u;
static const uint kPLFlagDebug = 2u;
static const uint kPLFlagMotion = 8u;          // (47y) last frame's motion vectors at t6 (Keep Shadow History)
static const uint kPLFlagWide = 32u;
static const uint kPLFlagWideHistory = 64u;
static const uint kPLFlagFractional = 65536u;  // (49m) the trace's fractional encoding
static const uint kPLFlagV2Off = 131072u;      // (50b) Off
static const uint kPLFlagV2RR = 262144u;       // (50b) DLSS RR
static const uint kPLFlagLenHistory = 524288u; // (50b) t9 holds last frame's history lengths (else a history found counts kAssumedLen)

static const float kLenMax = 15.0;
static const float kAssumedLen = 4.0;      // a history without its length (the frame after a switch from Penumbra)
static const float kYouthFrames = 8.0;
static const float kYoungMaxRadius = 32.0;
static const uint kYoungTaps = 32u, kMidTaps = 24u, kOldTaps = 12u;
static const float kProbeRadiusOld = 12.0, kProbeRadiusYoung = 24.0;
static const float kClampK = 2.0;
static const float kMinWeight = 0.1;
static const float kHistLo = 0.02, kHistHi = 0.98;
static const float kMotionTryBelow = 0.5;
static const float kMotionTolerance = 8.0;
static const float kBeamBlockerRange = 2048.0;  // TracePointLightsCS: a soft beam's blocked sample is sqrt(blocker distance / this)
static const float2 kGoldenTurn = float2(-0.7373688, 0.6754903);  // cos / sin of the golden angle (2.3999632 rad)
static const uint kPoissonTaps = 12u;
static const float2 kPoisson[12] = {
	float2(-0.326212, -0.405810), float2(-0.840144, -0.073580), float2(-0.695914, 0.457137), float2(-0.203345, 0.620716),
	float2(0.962340, -0.194983), float2(0.473434, -0.480026), float2(0.519456, 0.767022), float2(0.185461, -0.893124),
	float2(0.507431, 0.064425), float2(0.896420, 0.412458), float2(-0.321940, -0.932615), float2(-0.791559, -0.597710)
};
static const uint kViewPLCandidates = 12u;
static const uint kViewPLSources = 13u;
static const uint kViewPLRaw = 14u;
static const uint kViewPLDenoised = 15u;
static const uint kViewPLFilterPath = 18u;

#if defined(PL_WIDE)
static const bool kVariantWide = true;
#else
static const bool kVariantWide = false;
#endif

uint Pcg(uint a_v)
{
	const uint state = a_v * 747796405u + 2891336453u;
	const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}
float Hash(uint2 a_p, uint a_frame)
{
	return float(Pcg(a_p.x + Pcg(a_p.y + Pcg(a_frame))) >> 8) * (1.0 / 16777216.0);
}

uint4 Ids(float4 a_value)
{
	return uint4(a_value * 255.0 + 0.5) & 0x7Fu;
}

float3 Hue(uint a_id)
{
	const float h = frac(float(a_id) * 0.618034);
	return saturate(abs(frac(h + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
}

float Lit(float a_raw)
{
	const bool partial = (asuint(PLParams.z) & kPLFlagFractional) != 0u && a_raw >= (127.5 / 255.0);
	return a_raw >= 0.999 ? 1.0 : (partial ? saturate((a_raw * 255.0 - 128.0) / 126.0) : 0.0);
}

bool BlockerT(float a_raw, out float o_t)
{
	const bool fractional = (asuint(PLParams.z) & kPLFlagFractional) != 0u;
	o_t = a_raw * (fractional ? (255.0 / 127.0) : (255.0 / 254.0));
	return a_raw < (fractional ? (127.5 / 255.0) : 0.999);
}

// The slot (0 - 5) of a sample that holds this source, -1 = none. a_wide: its slots 4 - 5 ((0, 0, 1, 1) without them).
int FindSlot(uint4 a_ids, float4 a_wide, uint a_id)
{
	int slot = -1;
	if (a_id != 0u) {
		[unroll] for (int s = 3; s >= 0; --s)
			if (a_ids[s] == a_id)
				slot = s;
		if (kVariantWide && slot < 0) {
			const uint2 wideIds = uint2(a_wide.xy * 255.0 + 0.5) & 0x7Fu;
			slot = wideIds.x == a_id ? 4 : (wideIds.y == a_id ? 5 : -1);
		}
	}
	return slot;
}

float SlotValue(float4 a_vis, float4 a_wide, int a_slot)
{
	return a_slot < 4 ? a_vis[min(a_slot, 3)] : (a_slot == 4 ? a_wide.z : a_wide.w);
}

float4 WideAt(int2 a_q, uint a_flags)
{
	float4 w = float4(0.0, 0.0, 1.0, 1.0);
	[branch] if (kVariantWide && (a_flags & kPLFlagWide))
		w = PLWide[a_q];
	return w;
}

uint SourceByte(uint4 a_table[4], uint a_id)
{
	const uint s = a_id - 1u;
	return (a_table[s >> 4u][(s >> 2u) & 3u] >> ((s & 3u) * 8u)) & 0xFFu;
}

bool SoftBeam(uint a_id)
{
	if (a_id == 0u || PLExtra.w <= 0.0)
		return false;
	const uint s = a_id - 1u;
	const uint word = s < 32u ? asuint(PLExtra.y) : asuint(PLExtra.z);
	return ((word >> (s & 31u)) & 1u) != 0u;
}

float SlotEmitter(uint a_id)
{
	const uint s = a_id - 1u;
	const uint word = s < 32u ? PLEmitter.x : PLEmitter.y;
	float emitter = ((word >> (s & 31u)) & 1u) != 0u ? asfloat(PLEmitter.z) : PLParams.x;
	const uint fill = SourceByte(PLFillEmitter, a_id);
	if (fill != 0u)
		emitter = float(fill) * 2.0;
	return emitter;
}

// The physical penumbra radius (px) of a slot from its blocked samples' distance ratio q (sqrt-encoded ratio squared), not capped
// below kYoungMaxRadius.
float PhysicalRadius(uint a_id, float a_q, float a_footprint)
{
	float width;
	[branch] if (SoftBeam(a_id))
		width = saturate(a_q) * kBeamBlockerRange * 2.0 * PLExtra.w;
	else {
		const float q = clamp(a_q, 0.0, 0.95);
		width = 2.0 * SlotEmitter(a_id) * q / (1.0 - q);
	}
	return clamp(0.5 * width / a_footprint, 1.0, kYoungMaxRadius);
}

// The history taps at prevPos: bilinear, each tap checked against the distance it should have; per slot of ids the sum of matching
// values (hv), weights (hw) and history lengths (hn). Returns the weight of the taps that passed the distance test.
float HistoryTaps(float2 prevPos, float expected, float tolerance, uint4 ids, bool a_wideHistory, bool a_len, int2 maxP, out float4 hv, out float4 hw,
	out float4 hn)
{
	hv = 0.0;
	hw = 0.0;
	hn = 0.0;
	float valid = 0.0;
	const int2 base = int2(floor(prevPos));
	const float2 f = prevPos - float2(base);
	[unroll] for (int t = 0; t < 4; ++t)
	{
		const int2 o = int2(t & 1, t >> 1);
		const int2 q = clamp(base + o, int2(0, 0), maxP);
		const float hd = HistDist[q];
		const float bw = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
		[branch] if (hd > 0.0 && abs(hd - expected) <= tolerance && bw > 0.0)
		{
			valid += bw;
			const uint4 hid = Ids(HistIdx[q]);
			const float4 hvis = HistVis[q];
			float4 hwide = float4(0.0, 0.0, 1.0, 1.0);
			[branch] if (kVariantWide && a_wideHistory)
				hwide = HistWide[q];
			const uint lenBits = a_len ? HistLen[q] : 0u;
			[unroll] for (uint s = 0u; s < 4u; ++s)
			{
				const int slot = FindSlot(hid, hwide, ids[s]);
				if (slot >= 0) {
					hv[s] += SlotValue(hvis, hwide, slot) * bw;
					hw[s] += bw;
					hn[s] += (a_len ? float((lenBits >> (4u * uint(slot))) & 15u) : kAssumedLen) * bw;
				}
			}
		}
	}
	return valid;
}

struct PixelFrame
{
	int2 p;
	int2 maxP;
	float3 P;
	float3 N;
	float footprint;
	float planeTolerance;
	uint flags;
	float rotation;  // the taps' rotation (radians), per pixel and frame
};

// One set of four slots (slots 0 - 3, or 4 - 5 with ids.zw = 0). Returns the visibility; o_len = the new history lengths, o_radius /
// o_weight for the Filter Path view.
float4 DenoiseSet(PixelFrame a_px, uint4 ids, float4 raw, float4 hv, float4 hw, float4 hn, out float4 o_len, out float4 o_radius, out float4 o_weight)
{
	const int2 p = a_px.p;
	const int2 maxP = a_px.maxP;
	const float3 P = a_px.P;
	const float3 N = a_px.N;
	const uint flags = a_px.flags;
	const bool offMode = (flags & kPLFlagV2Off) != 0u;
	const bool rrMode = (flags & kPLFlagV2RR) != 0u;
	// Per-slot results in arrays (indexable in the slot loop below), packed at the end.
	float resA[4] = { 1.0, 1.0, 1.0, 1.0 }, lenA[4] = { 0.0, 0.0, 0.0, 0.0 }, radA[4] = { 0.0, 0.0, 0.0, 0.0 }, wA[4] = { 1.0, 1.0, 1.0, 1.0 };

	// 1. 3x3: matching samples (plane weighted), their range and the blocked ones' distance ratio.
	float4 sum = 0.0, weights = 0.0, weights2 = 0.0, lo = 2.0, hi = -1.0, qSum = 0.0, qCount = 0.0;
	[unroll] for (int k = 0; k < 9; ++k)
	{
		const int2 o = int2(k % 3 - 1, k / 3 - 1);
		const int2 q = clamp(p + o, int2(0, 0), maxP);
		const float dq = abs(DepthMask[q]);
		float wPlane = 0.0;
		if (!RTIsSky(dq))
			wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / a_px.planeTolerance);
		[branch] if (wPlane >= 0.05)
		{
			const uint4 idq = Ids(PLIdx[q]);
			const float4 vq = PLVis[q];
			const float4 wq = WideAt(q, flags);
			const float w = wPlane / (1.0 + float(o.x * o.x + o.y * o.y));
			[unroll] for (uint s = 0u; s < 4u; ++s)
			{
				const int slot = FindSlot(idq, wq, ids[s]);
				if (slot >= 0) {
					const float r = SlotValue(vq, wq, slot);
					const float v = Lit(r);
					sum[s] += v * w;
					weights[s] += w;
					weights2[s] += w * w;
					lo[s] = min(lo[s], v);
					hi[s] = max(hi[s], v);
					float t;
					if (BlockerT(r, t)) {
						qSum[s] += t * t;
						qCount[s] += 1.0;
					}
				}
			}
		}
	}

	float sn, cs;
	sincos(a_px.rotation, sn, cs);
	[loop] for (uint s = 0u; s < 4u; ++s)
	{
		const uint id = ids[s];
		[branch] if (id == 0u)
		{
			continue;
		}
		float m = weights[s] > 0.0 ? sum[s] / weights[s] : Lit(raw[s]);
		float neff = weights2[s] > 0.0 ? weights[s] * weights[s] / weights2[s] : 1.0;
		[branch] if (offMode)
		{
			resA[s] = m;
			continue;
		}
		const bool hasHist = (flags & kPLFlagHistory) != 0u && hw[s] >= 0.05 && SourceByte(PLMotion, id) < 255u;
		const float h = hasHist ? hv[s] / hw[s] : 0.0;
		const float n0 = hasHist ? clamp(hn[s] / hw[s], 0.0, kLenMax) : 0.0;
		const float youth = saturate(1.0 - n0 / kYouthFrames);

		// 2. The wide pass (not in the DLSS RR copy): where the 3x3 disagrees, the history is partial or the slot is young.
		[branch] if (!rrMode)
		{
			const bool disagree = hi[s] > lo[s];
			const bool partial = hasHist && h > kHistLo && h < kHistHi;
			const bool young = youth > 0.0;
			float q = -1.0;
			[branch] if (disagree || partial || young)
			{
				if (qCount[s] > 0.0) {
					q = qSum[s] / qCount[s];
				} else {
					// The probe: kPoissonTaps taps; the blocked ones' distance ratio gives the radius (none: outside every penumbra).
					const float probeRadius = lerp(kProbeRadiusOld, kProbeRadiusYoung, youth);
					float pqSum = 0.0, pqCount = 0.0;
					[loop] for (uint k = 0u; k < kPoissonTaps; ++k)
					{
						const float2 d = kPoisson[k];
						const int2 o = int2(round(float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) * probeRadius));
						const int2 qp = clamp(p + o, int2(0, 0), maxP);
						const float dq = abs(DepthMask[qp]);
						[branch] if (!RTIsSky(dq))
						{
							const float wPlane = exp(-abs(dot(N, RTPosition(qp, dq) - P)) / a_px.planeTolerance);
							const float4 wq = WideAt(qp, flags);
							const int slot = wPlane >= 0.05 ? FindSlot(Ids(PLIdx[qp]), wq, id) : -1;
							float t;
							if (slot >= 0 && BlockerT(SlotValue(PLVis[qp], wq, slot), t)) {
								pqSum += t * t;
								pqCount += 1.0;
							}
						}
					}
					q = pqCount > 0.0 ? pqSum / pqCount : -1.0;
				}
			}
			[branch] if (q >= 0.0)
			{
				const float radius = min(PhysicalRadius(id, q, a_px.footprint), lerp(PLParams.w, kYoungMaxRadius, youth));
				radA[s] = radius;
				[branch] if (radius >= 1.5)
				{
					const uint taps = youth >= 0.75 ? kYoungTaps : (youth >= 0.375 ? kMidTaps : kOldTaps);
					const float invR2 = 1.0 / (radius * radius);
					float wsum = Lit(raw[s]), wwt = 1.0, wwt2 = 1.0;
					// Vogel disc: tap k at radius sqrt((k + 0.5) / taps), angle k x the golden angle, the whole disc turned per pixel / frame.
					float2 dir = float2(cs, sn);
					[loop] for (uint k = 0u; k < taps; ++k)
					{
						const float2 r = dir * (sqrt((float(k) + 0.5) / float(taps)) * radius);
						dir = float2(dir.x * kGoldenTurn.x - dir.y * kGoldenTurn.y, dir.x * kGoldenTurn.y + dir.y * kGoldenTurn.x);
						const int2 o = int2(round(r));
						const int2 qp = clamp(p + o, int2(0, 0), maxP);
						const float dq = abs(DepthMask[qp]);
						[branch] if (any(o != 0) && !RTIsSky(dq))
						{
							const float wPlane = exp(-abs(dot(N, RTPosition(qp, dq) - P)) / a_px.planeTolerance);
							const float4 wq = WideAt(qp, flags);
							const int slot = wPlane >= 0.05 ? FindSlot(Ids(PLIdx[qp]), wq, id) : -1;
							if (slot >= 0) {
								const float w = wPlane * exp(-1.5 * dot(r, r) * invR2);
								wsum += Lit(SlotValue(PLVis[qp], wq, slot)) * w;
								wwt += w;
								wwt2 += w * w;
							}
						}
					}
					m = wsum / wwt;
					neff = wwt * wwt / wwt2;
				}
			}
		}

		// 3. The history: the binomial clamp; n decayed by how far it moved the history; this frame's weight max(1 / (n + 1), kMinWeight).
		float n = 0.0;
		float value = m;
		[branch] if (hasHist)
		{
			const float ne = max(neff, 1.0);
			const float band = kClampK * sqrt(saturate((saturate(m * (1.0 - m)) + 1.0 / ne) / ne));
			const float clamped = clamp(h, m - band, m + band);
			const float moved = abs(clamped - h) / max(band, 1e-3);
			n = n0 * saturate(1.0 - moved);
			const float a = max(1.0 / (n + 1.0), kMinWeight);
			value = lerp(clamped, m, a);
			wA[s] = a;
		}
		resA[s] = value;
		lenA[s] = min(n + 1.0, kLenMax);
	}
	o_len = float4(lenA[0], lenA[1], lenA[2], lenA[3]);
	o_radius = float4(radA[0], radA[1], radA[2], radA[3]);
	o_weight = float4(wA[0], wA[1], wA[2], wA[3]);
	return float4(resA[0], resA[1], resA[2], resA[3]);
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;
	const uint flags = asuint(PLParams.z);
	const bool wide = kVariantWide && (flags & kPLFlagWide) != 0u;
	const bool wideHistory = wide && (flags & kPLFlagWideHistory) != 0u;
	const bool lenHistory = (flags & kPLFlagLenHistory) != 0u;
	const bool rrMode = (flags & kPLFlagV2RR) != 0u;
	const float4 idxRaw = PLIdx[p];
	const uint4 ids = Ids(idxRaw);
	const float4 wideRaw = WideAt(p, flags);
	const uint4 wideIds = uint4(Ids(float4(wideRaw.xy, 0.0, 0.0)).xy, 0u, 0u);
	const float depth = abs(DepthMask[p]);
	const float4 raw = PLVis[p];
	float4 result = 1.0;
	float2 wideResult = 1.0;
	float4 len = 0.0;
	float2 wideLen = 0.0;
	float dist = -1.0;
	float4 dbgRadius = 0.0, dbgWeight = 1.0;

	[branch] if (!RTIsSky(depth) && (any(ids != 0u) || any(wideIds != 0u)))
	{
		PixelFrame px;
		px.p = p;
		px.flags = flags;
		px.P = RTPosition(p, depth);
		dist = length(px.P);
		px.maxP = int2(Size.xy) - 1;
		px.rotation = 6.2831853 * Hash(uint2(p), PLEmitter.w);
		const float3 P = px.P;
		const int2 maxP = px.maxP;
		{
			const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
			const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
			const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
			const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - RTPosition(pl, dl) : RTPosition(pr, dr) - P;
			const float3 dy = abs(du - depth) < abs(dd - depth) ? P - RTPosition(pu, du) : RTPosition(pd, dd) - P;
			float3 N = cross(dy, dx);
			const float nl = length(N);
			N = nl > 1e-12 ? N / nl : (dist > 0.0 ? -P / dist : float3(0, 0, 1));
			px.N = N;
			px.footprint = max(length(RTPosition(pr, depth) - P), 1e-3);
		}
		px.planeTolerance = dist * 0.01 + 2.0;

		// History taps: the camera reprojection, (47y) last frame's motion vectors where it finds (almost) nothing.
		float4 hv = 0.0, hw = 0.0, hn = 0.0;
		float2 histPos = 0.0;
		float histExpected = 0.0, histTolerance = 0.0;
		bool histTaken = false;
		[branch] if ((flags & kPLFlagHistory) && !(flags & kPLFlagV2Off))
		{
			const float4 prevClip = mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0));
			const float2 prevUv = prevClip.w > 1e-4 ? float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5) : float2(-1.0, -1.0);
			const float expected = length(P + CameraDelta.xyz);
			const float tolerance = expected * 0.02 + 8.0;
			histExpected = expected;
			float tapWeight = 0.0;
			[branch] if (all(prevUv >= 0.0) && all(prevUv < 1.0))
			{
				histPos = prevUv * float2(Size.xy) - 0.5;
				histTolerance = tolerance;
				histTaken = true;
				tapWeight = HistoryTaps(histPos, expected, tolerance, ids, wideHistory, lenHistory, maxP, hv, hw, hn);
			}
			[branch] if ((flags & kPLFlagMotion) && tapWeight < kMotionTryBelow)
			{
				const float2 uv = (float2(p) + 0.5) / float2(Size.xy);
				float2 m = (all(prevUv >= 0.0) && all(prevUv < 1.0)) ? prevUv : uv;
				[unroll] for (int i = 0; i < 2; ++i)
				{
					const int2 q = int2(clamp(m, 0.0, 0.999999) * float2(Size.xy));
					m = uv + PrevMotion[q];
				}
				[branch] if (all(m >= 0.0) && all(m < 1.0))
				{
					float4 mv, mw, mn;
					const float2 motionPos = m * float2(Size.xy) - 0.5;
					const float motionWeight = HistoryTaps(motionPos, expected, tolerance + kMotionTolerance, ids, wideHistory, lenHistory, maxP, mv, mw, mn);
					if (motionWeight > tapWeight) {
						hv = mv;
						hw = mw;
						hn = mn;
						histPos = motionPos;
						histTolerance = tolerance + kMotionTolerance;
						histTaken = true;
					}
				}
			}
		}

		result = DenoiseSet(px, ids, raw, hv, hw, hn, len, dbgRadius, dbgWeight);

		[branch] if (wide && any(wideIds != 0u))
		{
			float4 hv2 = 0.0, hw2 = 0.0, hn2 = 0.0;
			[branch] if (histTaken)
				HistoryTaps(histPos, histExpected, histTolerance, wideIds, wideHistory, lenHistory, maxP, hv2, hw2, hn2);
			float4 wLen, wRadius, wWeight;
			wideResult = DenoiseSet(px, wideIds, float4(wideRaw.zw, 1.0, 1.0), hv2, hw2, hn2, wLen, wRadius, wWeight).xy;
			wideLen = wLen.xy;
		}
	}

	OutVis[p] = saturate(result);
	OutIdx[p] = idxRaw;
	OutDist[p] = dist;
	[branch] if (wide)
		OutWide[p] = float4(wideRaw.xy, saturate(wideResult));
	{
		const uint4 l4 = uint4(round(len));
		const uint2 l2 = uint2(round(wideLen));
		OutLen[p] = (l4.x & 15u) | ((l4.y & 15u) << 4u) | ((l4.z & 15u) << 8u) | ((l4.w & 15u) << 12u) | ((l2.x & 15u) << 16u) | ((l2.y & 15u) << 20u);
	}
	// DLSS RR: Lighting reads this frame's decoded rays (empty slots lit).
	[branch] if (rrMode)
	{
		float4 lit = 1.0;
		[unroll] for (uint s = 0u; s < 4u; ++s)
			lit[s] = ids[s] != 0u ? Lit(raw[s]) : 1.0;
		OutRaw[p] = lit;
		[branch] if (wide)
			OutRawWide[p] = float4(wideRaw.xy, wideIds.x != 0u ? Lit(wideRaw.z) : 1.0, wideIds.y != 0u ? Lit(wideRaw.w) : 1.0);
	}

	[branch] if (flags & kPLFlagDebug)
	{
		const uint view = Size.z;
		float4 debug = 0.0;
		if (view == kViewPLCandidates) {
			const uint4 bytes = uint4(idxRaw * 255.0 + 0.5);
			const uint n = ((bytes.x >> 7) & 1u) | (((bytes.y >> 7) & 1u) << 1) | (((bytes.z >> 7) & 1u) << 2);
			if (n == 1u)
				debug = float4(0.1, 1.0, 0.2, 0.45);
			else if (n == 2u)
				debug = float4(1.0, 0.9, 0.1, 0.5);
			else if (n >= 3u && n <= 4u)
				debug = float4(1.0, 0.5, 0.0, 0.55);
			else if (n > 4u)
				debug = float4(1.0, 0.1, 0.05, 0.6);
		} else if (view == kViewPLSources) {
			if (ids.x != 0u) {
				debug = float4(Hue(ids.x), 0.55);
				const uint d = (uint(p.x) + uint(p.y)) % 12u;
				if (ids.y != 0u && d < 3u)
					debug = float4(Hue(ids.y), 0.6);
				else if (ids.z != 0u && d == 6u)
					debug = float4(Hue(ids.z), 0.6);
				else if (ids.w != 0u && d == 9u)
					debug = float4(Hue(ids.w), 0.6);
				else if (wideIds.x != 0u && d == 4u)
					debug = float4(Hue(wideIds.x), 0.6);
				else if (wideIds.y != 0u && d == 11u)
					debug = float4(Hue(wideIds.y), 0.6);
			}
		} else if (view == kViewPLRaw || view == kViewPLDenoised) {
			if (any(ids != 0u) || any(wideIds != 0u)) {
				float v = 1.0;
				[unroll] for (uint s = 0u; s < 4u; ++s)
					if (ids[s] != 0u)
						v *= view == kViewPLRaw ? Lit(raw[s]) : saturate(result[s]);
				[unroll] for (uint s2 = 0u; s2 < 2u; ++s2)
					if (wideIds[s2] != 0u)
						v *= view == kViewPLRaw ? Lit(wideRaw[2u + s2]) : saturate(wideResult[s2]);
				debug = float4(v.xxx, 1.0);
			}
		} else if (view == kViewPLFilterPath) {
			// Slot 0. Even rows: green, brighter = wider spatial pass (full at 32 px), blue tint = young (short history); odd rows: this
			// frame's weight in the history blend (dark = 0.1, white = 1 or no history).
			if (ids.x != 0u) {
				if ((p.y & 1) == 0)
					debug = float4(0.1, 0.25 + 0.75 * saturate(dbgRadius.x / kYoungMaxRadius), saturate(1.0 - len.x / kYouthFrames), 1.0);
				else
					debug = float4(saturate(dbgWeight.x).xxx, 1.0);
			}
		}
		DebugColor[p] = debug;
	}
}
