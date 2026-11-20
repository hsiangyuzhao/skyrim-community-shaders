// (46b) Ray-traced point-light shadows: the denoiser, per slot (the Penumbra denoiser's idea per light).
//
// Input: this frame's trace (TracePointLightsCS): per pixel up to four slots, each a source slot id and
// one ray's result (1 = lit, < 1 = blocked, the value encoding sqrt(blocker / light distance)).
// (48a) Lights per pixel 5 - 6 (kPLFlagWide): slots 4 - 5 in PLWide (xy source + 1, zw values); the same filter runs a second
// time for them (DenoiseSet) and writes OutWide; neighbours / history are matched over all six slots.
// Per slot of the pixel, only samples of the SAME source count (its id in any slot of the neighbour):
//   1. 3x3 neighbourhood: where every matching sample agrees (fully lit, fully shadowed) nothing is
//      filtered (contact shadows stay sharp, lit areas stay lit);
//   2. otherwise a 5x5 pattern whose step follows the penumbra width (emitter size x blocker distance /
//      (light distance - blocker distance), in pixels), Gaussian x plane distance weights; (47x, kPLFlagSmooth) instead 12
//      Poisson-disc taps per slot at that slot's own width, rotated per pixel and per frame;
//   3. the history: last frame's result reprojected with the camera (bilinear, each tap checked against
//      the distance it should have, its slot found by the source id), clamped to this frame's 3x3 range,
//      blended in (Params.y = this frame's weight); (47y, kPLFlagMotion) where the camera's taps fail, last frame's motion vectors at
//      constant velocity; (kPLFlagNoHistorySmooth) slots still without history are filtered at least 2.5 px with a fixed pattern.
// (48a, kPLFlagBeamSmooth) Window Beams: Smooth Edges: soft window-beam slots take their own path (BeamRadius / the beam pass below).
// Output: Lighting reads OutVis / OutIdx (t40 / t41 of the pixel shaders, RayTracing/PointLightShadows.hlsli; (48a) OutWide t46);
// they are also next frame's history (ping-pong), with OutDist.
// The debug views of the point lights are written here (after CombineCS cleared DebugColor).

#include "RayTracing/Common.hlsli"

cbuffer PointDenoiseCB : register(b1)
{
	float4 PLParams;  // x emitter radius, y this frame's weight in the history blend, z flags (asuint), w max filter radius (px)
	float4 PLExtra;   // x K, (46o) y / z window-beam source slots 0-31 / 32-63 (uint bits), w tan of the beams' angular size (0 = 46n)
	uint4 PLMotion[4];  // (46d) per source slot one byte (slot s: byte s % 4 of uint s / 4): history dropped this frame, 0 keep .. 255 none
	uint4 PLEmitter;  // (46x) x / y source slots 0-31 / 32-63 with their own emitter radius (uint bits), z that radius (float bits)
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
// (47x) Smoother Shadow Edges: the wide pass takes, per slot, kPoissonTaps Poisson-disc taps scaled by that slot's own radius and
// rotated per pixel and per frame (the history averages the rotations). Without it (47w): one 5x5 grid, step = half the largest
// radius of the pixel's slots, shared by all slots (stair steps every step pixels; small-radius slots weighed ~only the pixel itself).
static const uint kPLFlagSmooth = 4u;
static const uint kPoissonTaps = 12u;
static const float2 kPoisson[12] = {
	float2(-0.326212, -0.405810), float2(-0.840144, -0.073580), float2(-0.695914, 0.457137), float2(-0.203345, 0.620716),
	float2(0.962340, -0.194983), float2(0.473434, -0.480026), float2(0.519456, 0.767022), float2(0.185461, -0.893124),
	float2(0.507431, 0.064425), float2(0.896420, 0.412458), float2(-0.321940, -0.932615), float2(-0.791559, -0.597710)
};

// (47y) "Moving Characters: Keep Shadow History": where the camera-only reprojection has (almost) no valid taps, last frame's motion vectors
// (t6; this pass runs before this frame's G-buffer, as RTAO's AONormalPrepCS candidate B) at constant velocity; validated by the stored
// history distance like the camera taps (plus kMotionTolerance: the surface itself moved).
// "Moving Characters: Smooth When No History": a slot still without history is filtered at least kNoHistoryRadiusPx wide (where its 3x3
// disagrees) and its Poisson taps are rotated per pixel only (a fixed pattern: no per-frame shimmer that no history would average).
static const uint kPLFlagMotion = 8u;
static const uint kPLFlagNoHistorySmooth = 16u;
static const float kMotionTryBelow = 0.5;
static const float kMotionTolerance = 8.0;
static const float kNoHistoryRadiusPx = 2.5;
Texture2D<float2> PrevMotion : register(t6);

// (48a) Lights per pixel 5 - 6: this frame's slots 4 - 5 (t7, TracePointLightsCS PLWide: xy source + 1, zw raw), last frame's denoised ones
// (t8, OutWide of last frame) and this frame's (u4: xy source + 1 as read, zw denoised). kPLFlagWideHistory: t8 holds last frame (its
// trace was wide too); without it last frame's slots 4 - 5 count as empty.
static const uint kPLFlagWide = 32u;
static const uint kPLFlagWideHistory = 64u;
Texture2D<unorm float4> PLWide : register(t7);
Texture2D<unorm float4> HistWide : register(t8);
RWTexture2D<unorm float4> OutWide : register(u4);

// (48a) "Window Beams: Smooth Edges" (soft window-beam slots only; the other slots exactly as before):
//   - the wide pass runs wherever the beam's penumbra can be: the 3x3 disagrees (at least kBeamMinRadiusPx: hard 0.05-degree edges too) or
//     the history is partial (kBeamHistLo .. kBeamHistHi: a penumbra pixel whose 3x3 happened to agree). Converged lit / umbra pixels skip
//     it (the cost stays on the penumbra). Without a blocked sample in the 3x3 the blocker distance comes from a probe of kPoissonTaps taps
//     at kBeamProbeRadiusPx; none found there = no wide pass (well outside any wide penumbra);
//   - radius up to kBeamMaxRadiusPx; above kPoissonTaps px a second ring (the disc turned by the golden angle): 24 taps;
//   - the taps' plane test kBeamPlaneScale times tighter (a character's lowest rows lie in the floor's plane: they no longer bleed across
//     its shadow on the floor);
//   - history clamp: the filtered mean +- kBeamClampK sigma of that mean (sigma of the binary samples / sqrt(effective tap count)), at
//     least kBeamClampMin (47z: the 1-sample 3x3 min / max +- 0.1, which snapped chance-agreeing penumbra pixels to 1: crawling specks);
//   - a static beam (HistoryDrop 0) keeps more history: this frame weighs kBeamStaticWeight instead of Params.y.
static const uint kPLFlagBeamSmooth = 128u;
static const float kBeamMaxRadiusPx = 24.0;
static const float kBeamMinRadiusPx = 2.0;
static const float kBeamProbeRadiusPx = 12.0;
static const float kBeamHistLo = 0.02;
static const float kBeamHistHi = 0.98;
static const float kBeamPlaneScale = 0.5;
static const float kBeamClampK = 2.0;
static const float kBeamClampMin = 0.05;
static const float kBeamStaticWeight = 0.1;
static const float2 kGoldenTurn = float2(-0.7373688, 0.6754903);  // cos / sin of the golden angle (2.3999632 rad)

// (48a) Variants (RayTracing.cpp picks one per frame): PL_WIDE = slots 4 - 5 (K > 4 this frame), PL_BEAM = Window Beams: Smooth Edges on. Without
// them their code folds away at compile time: the plain variant is the 47z shader (K <= 4 with the switch off costs what it did).
#if defined(PL_WIDE)
static const bool kVariantWide = true;
#else
static const bool kVariantWide = false;
#endif
#if defined(PL_BEAM)
static const bool kVariantBeam = true;
#else
static const bool kVariantBeam = false;
#endif
// (48k) PL_FIX = Point Lights: Noise Fix on (RayTracing.cpp picks the PL_FIX twin of the variant). Without it the fix's code folds away: the
// 48j shaders, byte for byte. Its parts (runtime flags, each 0 with the master off):
//   kPLFlagFixDecorrelate "Decorrelated Sampling": the taps' rotation from a hash of (pixel, frame) instead of IGN(pixel, frame), the IGN the
//     trace sampled the emitter disc with (48j: the rotation was nearly a fixed function of the pixel's own sample: a dotted lattice);
//   kPLFlagFixStable "Stable Penumbra": every point slot (not the soft window beams: they keep Window Beams: Smooth Edges) takes the 48a beam
//     path: the wide pass where the 3x3 disagrees or the history is partial (kBeamHistLo .. kBeamHistHi), at least kFixMinRadiusPx, radius
//     from the point-light penumbra formula (a probe of kPoissonTaps taps when the 3x3 has no blocked sample), history clamp to the filtered
//     mean +- kBeamClampK sigma, a static light (HistoryDrop 0) whose history lies in that band weighs this frame kBeamStaticWeight (0.1);
//     the plane test stays the normal one (not the beams' tighter one);
//   kPLFlagFixCloseBlur "Close Blockers: Minimum Blur": a slot whose 3x3 disagrees and whose blocked samples' distance ratio q is under
//     kFixCloseQ (blocker within ~12 % of the light distance: hair on a face, contact shadows) is filtered at least kFixCloseRadiusPx wide
//     (the formula gave 1 px there: 3x3 + history only).
#if defined(PL_FIX)
static const bool kVariantFix = true;
#else
static const bool kVariantFix = false;
#endif
static const uint kPLFlagFixDecorrelate = 256u;
static const uint kPLFlagFixStable = 512u;
static const uint kPLFlagFixCloseBlur = 1024u;
static const float kFixMinRadiusPx = 2.0;
static const float kFixCloseRadiusPx = 3.0;
static const float kFixCloseQ = 0.35;
// (49i) Two edge switches (Point Lights > Advanced), read by the PL_FIX variant only, on Stable Penumbra's slots (not on a moving receiver:
// Clean Shadow History keeps its strict path there). Both look at the slot's 5x5 neighbourhood, loaded only where the 3x3 disagrees or the
// history is partial (kBeamHistLo .. kBeamHistHi), i.e. on and next to edges:
//   kPLFlagSmoothHard "Smooth Hard Edges": where the 5x5 holds lit and blocked samples and the penumbra formula gives at most kHardEdgeMaxPx
//     (a sharp shadow edge, e.g. a contact shadow: the formula's width is below a pixel there), the slot's spatial value is the fixed 5x5
//     Gaussian (sigma kHardEdgeSigmaPx, plane-weighted) instead of the 2-px rotated Poisson disc: every pixel within 2 px of the edge gets a
//     filtered value (48k: only the pixels whose 3x3 disagreed, the next ones stayed 0 / 1), and no per-frame tap rotation (12 taps
//     rounded to whole pixels at a 2 px radius landed on other pixels every frame: the value at the edge jumped frame to frame);
//   kPLFlagSteadyEdges "Steady Edges": on those edge pixels the history clamp is the 5x5 Gaussian mean +- max(2 sigma of that mean,
//     kSteadyBandMin) (48k: the filtered mean +- 2 sigma, at least 0.05, which snapped a pixel whose 3x3 happened to agree to 0 / 1:
//     the camera jitter moves the 1-sample edge by up to half a pixel per frame, so edge pixels flipped between snapped and filtered),
//     so the jittered samples average over frames (the temporal anti-aliasing of the edge); a static light weighs this frame
//     kBeamStaticWeight there. Clean Shadow History's strong-change rule (a history more than kCleanStrongChange from the 3x3 mean takes
//     the 3x3 clamp) still applies after it: a moving caster's shadow arriving / leaving replaces the history at once.
static const uint kPLFlagSmoothHard = 16384u;
static const uint kPLFlagSteadyEdges = 32768u;
static const float kHardEdgeMaxPx = 2.5;
static const float kHardEdgeSigmaPx = 1.0;
static const float kSteadyBandMin = 0.25;
// (48m) PL_CLEAN = "Moving Characters: Clean Shadow History" (Point Lights > Advanced, outside the Noise Fix group; any combination with
// PL_WIDE / PL_BEAM / PL_FIX). All its code is inside #if defined(PL_CLEAN) blocks: the preprocessed source of the other variants is the
// 48k one (48j with the Noise Fix master off). kPLFlagCleanDetect: the motion vectors are bound at t6 (whatever Keep Shadow History says) and
// the moving test runs; kPLFlagClean: its result is used (the toggle; without it the variant only feeds the two debug views).
//   Moving receiver: last frame's motion vectors (two fixed-point steps from this pixel) land more than max(kCleanResidualPx,
//     kCleanResidualShare x the camera's own motion) from the camera-only reprojection (1.5 px: the camera reprojection is jittered, the
//     vectors are not; the relative term: camera acceleration, the vectors being a frame old). There, on every path (46b, beams, Stable
//     Penumbra):
//     - temporal: only the motion-vector history, with a strict distance test (kCleanTolScale x distance + kCleanTolBase) and at least
//       kCleanMinTapWeight of valid taps; otherwise no history (the camera candidate and Keep Shadow History's are not taken). Where the
//       3x3 agrees the history is clamped to it exactly (fully lit / fully blocked: snaps at once), else to the filtered mean +- max(2 sigma,
//       kCleanBandMin); this frame weighs at least kCleanWeight; no partial-history wide pass or probe;
//     - spatial: a strict plane test (kCleanPlaneScale x distance + kCleanPlaneBase, 3x3 and wide pass: taps of the floor, the background
//       or another limb off this pixel's plane do not count) and the wide pass at most kCleanMaxRadiusPx.
//   Static receiver (band paths): a history more than kCleanStrongChange from this frame's 3x3 mean (a moving caster's shadow arrived / left)
//     takes the 3x3 clamp and kCleanStaticWeight; the 0.1 only while the history is in the band's inner half (no trailing shadow).
#if defined(PL_CLEAN)
static const uint kPLFlagClean = 4096u;
static const uint kPLFlagCleanDetect = 8192u;
static const float kCleanResidualPx = 1.5;
static const float kCleanResidualShare = 0.25;
static const float kCleanTolScale = 0.005;
static const float kCleanTolBase = 3.0;
static const float kCleanMinTapWeight = 0.75;
static const float kCleanPlaneScale = 0.002;
static const float kCleanPlaneBase = 1.0;
static const float kCleanMaxRadiusPx = 3.0;
static const float kCleanWeight = 0.5;
static const float kCleanBandMin = 0.1;
static const float kCleanStaticWeight = 0.35;
static const float kCleanStrongChange = 0.5;
static const uint kViewPLMoving = 17u;      // (48m) RayTracing::DebugView::PLMoving
static const uint kViewPLFilterPath = 18u;  // (48m) RayTracing::DebugView::PLFilterPath
#endif

// (48k) Decorrelated Sampling: a uniform [0, 1) value per (pixel, frame) from the PCG hash (independent of the trace's patterns).
uint FixPcg(uint a_v)
{
	const uint state = a_v * 747796405u + 2891336453u;
	const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}
float FixHash(uint2 a_p, uint a_frame)
{
	return float(FixPcg(a_p.x + FixPcg(a_p.y + FixPcg(a_frame))) >> 8) * (1.0 / 16777216.0);
}

float InterleavedGradientNoise(float2 a_p, uint a_frame)
{
	a_p += float(a_frame % 64u) * 5.588238;
	return frac(52.9829189 * frac(dot(a_p, float2(0.06711056, 0.00583715))));
}
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

// (48a) Match over all six slots of a sample: a_wide = its slots 4 - 5 (xy source + 1 as stored, zw values; (0, 0, 1, 1) without them).
bool MatchWide(uint4 a_ids, float4 a_vis, float4 a_wide, uint a_id, out float o_raw)
{
	bool found = Match(a_ids, a_vis, a_id, o_raw);
	if (kVariantWide && !found && a_id != 0u) {
		const uint2 wideIds = uint2(a_wide.xy * 255.0 + 0.5) & 0x7Fu;
		if (wideIds.x == a_id) {
			o_raw = a_wide.z;
			found = true;
		} else if (wideIds.y == a_id) {
			o_raw = a_wide.w;
			found = true;
		}
	}
	return found;
}

// (48a) A sample's slots 4 - 5 when this frame is wide (else none: no read).
float4 WideAt(int2 a_q, uint a_flags)
{
	float4 w = float4(0.0, 0.0, 1.0, 1.0);
	[branch] if (kVariantWide && (a_flags & kPLFlagWide))
		w = PLWide[a_q];
	return w;
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

// (46x) Does this source (id = slot + 1) have its own emitter radius (a direct fill without a window spot)?
bool BigEmitter(uint a_id)
{
	if (a_id == 0u)
		return false;
	const uint s = a_id - 1u;
	const uint word = s < 32u ? PLEmitter.x : PLEmitter.y;
	return ((word >> (s & 31u)) & 1u) != 0u;
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

// (48k) Stable Penumbra: a point slot's penumbra radius in px from its blocked samples' distance ratio q (the 46b formula of step 2).
float FixPointRadius(uint a_id, float a_q, float a_footprint)
{
	const float q = clamp(a_q, 0.0, 0.95);
	const float emitter = BigEmitter(a_id) ? asfloat(PLEmitter.z) : PLParams.x;
	const float width = 2.0 * emitter * q / (1.0 - q);
	return clamp(0.5 * width / a_footprint, 1.0, PLParams.w);
}

// The history taps at prevPos (last frame's pixel, centre at +0.5): bilinear, each tap checked against the distance it should have, its
// slot found by the source id ((47y) split out: the camera and the motion candidate share it). Returns the bilinear weight of the taps
// that passed the distance test (whatever their sources). (48a) a_wideHistory: last frame's slots 4 - 5 (HistWide) are searched too.
float HistoryTaps(float2 prevPos, float expected, float tolerance, uint4 ids, bool a_wideHistory, int2 maxP, out float4 hv, out float4 hw)
{
	hv = 0.0;
	hw = 0.0;
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
			[unroll] for (uint s = 0u; s < 4u; ++s)
			{
				float h;
				if (MatchWide(hid, hvis, hwide, ids[s], h)) {
					hv[s] += h * bw;
					hw[s] += bw;
				}
			}
		}
	}
	return valid;
}

// The pixel's geometry, shared by both slot sets.
struct PixelFrame
{
	int2 p;
	int2 maxP;
	float3 P;
	float3 N;
	float footprint;
	float planeTolerance;
	uint flags;
#if defined(PL_CLEAN)
	bool moving;  // (48m) a moving receiver, used (kPLFlagClean)
#endif
};

// Steps 1, 2 and 3b for four slots (ids, raw: this frame's; hv / hw: their history taps). (48a) Called for slots 0 - 3 and, with K > 4,
// for slots 4 - 5 (ids.zw = 0). With kPLFlagBeamSmooth off this is the 47z code, step for step.
#if defined(PL_CLEAN)
float4 DenoiseSet(PixelFrame a_px, uint4 ids, float4 raw, float4 hv, float4 hw, out float4 o_radius, out float4 o_weight, out uint4 o_path)
#else
float4 DenoiseSet(PixelFrame a_px, uint4 ids, float4 raw, float4 hv, float4 hw)
#endif
{
	const int2 p = a_px.p;
	const int2 maxP = a_px.maxP;
	const float3 P = a_px.P;
	const float3 N = a_px.N;
	const float footprint = a_px.footprint;
	const float planeTolerance = a_px.planeTolerance;
	const uint flags = a_px.flags;
	// (48a) Soft window-beam slots on the Smooth Edges path, and their tighter plane tolerance.
	const bool beamSmooth = kVariantBeam && (flags & kPLFlagBeamSmooth) != 0u;
	bool4 beamSlot = false;
	[unroll] for (uint b = 0u; b < 4u; ++b)
		beamSlot[b] = beamSmooth && SoftBeam(ids[b]);
	const float beamPlaneTolerance = planeTolerance * kBeamPlaneScale;
	// (48k) Noise fix: Stable Penumbra's slots (every point slot that is not a soft window beam) and Close Blockers: Minimum Blur.
	const bool fixStable = kVariantFix && (flags & kPLFlagFixStable) != 0u;
	const bool fixClose = kVariantFix && (flags & kPLFlagFixCloseBlur) != 0u;
	bool4 fixSlot = false;
	[unroll] for (uint fb = 0u; fb < 4u; ++fb)
		fixSlot[fb] = fixStable && ids[fb] != 0u && !SoftBeam(ids[fb]);
#if defined(PL_CLEAN)
	// (48m) Moving Characters: Clean Shadow History: this pixel's receiver moves (the plane tolerance is already the strict one then).
	const bool cleanOn = (flags & kPLFlagClean) != 0u;
	const bool cleanMoving = cleanOn && a_px.moving;
#endif

	// 1. 3x3: matching samples, their range and the blocked ones' distance ratio. (48a) + the sum of squared weights (beam slots' clamp).
	float4 sum = 0.0, weights = 0.0, lo = 2.0, hi = -1.0, qSum = 0.0, qCount = 0.0, weights2 = 0.0;
	[unroll] for (int k = 0; k < 9; ++k)
	{
		const int2 o = int2(k % 3 - 1, k / 3 - 1);
		const int2 q = clamp(p + o, int2(0, 0), maxP);
		const float dq = abs(DepthMask[q]);
		float wPlane = 0.0, wPlaneBeam = 0.0;
		if (!RTIsSky(dq)) {
			const float planeDist = abs(dot(N, RTPosition(q, dq) - P));
			wPlane = exp(-planeDist / planeTolerance);
			wPlaneBeam = exp(-planeDist / beamPlaneTolerance);
		}
		[branch] if (wPlane >= 0.05)
		{
			const uint4 idq = Ids(PLIdx[q]);
			const float4 vq = PLVis[q];
			const float4 wq = WideAt(q, flags);
			[unroll] for (uint s = 0u; s < 4u; ++s)
			{
				const float wp = beamSlot[s] ? wPlaneBeam : wPlane;
				const float w = wp / (1.0 + float(o.x * o.x + o.y * o.y));
				float r;
				if ((!beamSlot[s] || wp >= 0.05) && MatchWide(idq, vq, wq, ids[s], r)) {
					const float v = Lit(r);
					sum[s] += v * w;
					weights[s] += w;
					weights2[s] += w * w;
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

	// 2. Penumbra-wide pass for the slots whose neighbourhood disagrees.
	float4 radiusPx = 0.0;
	float4 beamRadius = 0.0;  // (48a) Smooth Edges beam slots: their wide pass radius (0 = none)
	bool4 beamProbe = false;  // (48a) ... the radius still to be found by the probe
	float4 spatial = 1.0;
	[unroll] for (uint s = 0u; s < 4u; ++s)
	{
		[branch] if (beamSlot[s])
		{
			// (48a) Window Beams: Smooth Edges.
			const bool disagree = hi[s] > lo[s];
			const float hMean = hw[s] >= 0.05 ? hv[s] / hw[s] : -1.0;
#if defined(PL_CLEAN)
			const bool partial = !cleanMoving && hMean > kBeamHistLo && hMean < kBeamHistHi;  // (48m) not on a moving receiver
#else
			const bool partial = hMean > kBeamHistLo && hMean < kBeamHistHi;
#endif
			if (disagree || partial) {
				if (qCount[s] > 0.0) {
					const float qb = saturate(qSum[s] / qCount[s]);
					beamRadius[s] = clamp(qb * kBeamBlockerRange * PLExtra.w / footprint, 1.0, kBeamMaxRadiusPx);
					if (disagree) {
						beamRadius[s] = max(beamRadius[s], kBeamMinRadiusPx);
						if (hw[s] < 0.05)
							beamRadius[s] = max(beamRadius[s], kBeamNoHistoryRadiusPx);
					}
				} else {
					beamProbe[s] = true;  // 3x3 all lit, partial history: the blocker distance from a wider look
				}
			}
		}
		else if (fixSlot[s])
		{
			// (48k) Stable Penumbra: the beam path's conditions with the point-light radius, at least kFixMinRadiusPx (a penumbra pixel whose
			// 3x3 happened to agree is filtered too instead of its history snapping to 0 / 1).
			const bool disagree = hi[s] > lo[s];
			const float hMean = hw[s] >= 0.05 ? hv[s] / hw[s] : -1.0;
#if defined(PL_CLEAN)
			const bool partial = !cleanMoving && hMean > kBeamHistLo && hMean < kBeamHistHi;  // (48m) not on a moving receiver
#else
			const bool partial = hMean > kBeamHistLo && hMean < kBeamHistHi;
#endif
			if (disagree || partial) {
				if (qCount[s] > 0.0) {
					const float q = qSum[s] / qCount[s];
					beamRadius[s] = max(FixPointRadius(ids[s], q, footprint), min(kFixMinRadiusPx, PLParams.w));
					if (disagree) {
						if (fixClose && q < kFixCloseQ)
							beamRadius[s] = max(beamRadius[s], min(kFixCloseRadiusPx, PLParams.w));
						if ((flags & kPLFlagNoHistorySmooth) && hw[s] < 0.05)
							beamRadius[s] = max(beamRadius[s], min(kNoHistoryRadiusPx, PLParams.w));
					}
				} else {
					beamProbe[s] = true;  // 3x3 all lit, partial history: the blocker distance from a wider look
				}
			}
		}
		else if (ids[s] != 0u && hi[s] > lo[s])
		{
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
				const float emitter = BigEmitter(ids[s]) ? asfloat(PLEmitter.z) : PLParams.x;  // (46x)
				const float width = 2.0 * emitter * q / (1.0 - q);
				radiusPx[s] = clamp(0.5 * width / footprint, 1.0, PLParams.w);
				// (48k) Close Blockers: Minimum Blur (Stable Penumbra off: this path).
				if (fixClose && q < kFixCloseQ)
					radiusPx[s] = max(radiusPx[s], min(kFixCloseRadiusPx, PLParams.w));
			}
			// (47y) Smooth When No History: every slot (the beams keep their 4 px) at least kNoHistoryRadiusPx where no history came.
			if ((flags & kPLFlagNoHistorySmooth) && hw[s] < 0.05)
				radiusPx[s] = max(radiusPx[s], min(kNoHistoryRadiusPx, PLParams.w));
		}
		spatial[s] = weights[s] > 0.0 ? sum[s] / max(weights[s], 1e-6) : Lit(raw[s]);
	}
#if defined(PL_CLEAN)
	const float4 spatial3 = spatial;  // (48m) this frame's 3x3 mean
	if (cleanMoving)
		radiusPx = min(radiusPx, kCleanMaxRadiusPx);  // (48m) the 46b wide pass at most kCleanMaxRadiusPx on a moving receiver
#endif
	// (49i) Smooth Hard Edges / Steady Edges: the 5x5 of Stable Penumbra's slots on and next to edges (see kPLFlagSmoothHard).
	const bool smoothHard = kVariantFix && (flags & kPLFlagSmoothHard) != 0u;
	const bool steadyEdges = kVariantFix && (flags & kPLFlagSteadyEdges) != 0u;
	bool4 edge5 = false;     // the slot's 5x5 holds lit and blocked samples (and was loaded)
	bool4 hardSlot = false;  // ... and its edge is sharp: the 5x5 Gaussian is its spatial value
	float4 mean5 = 0.0, neff5 = 1.0;
	[branch] if (smoothHard || steadyEdges)
	{
		bool4 want5 = false;
		[unroll] for (uint ws = 0u; ws < 4u; ++ws)
		{
			const float hMean5 = hw[ws] >= 0.05 ? hv[ws] / max(hw[ws], 0.05) : -1.0;
			want5[ws] = fixSlot[ws] && (hi[ws] > lo[ws] || (hMean5 > kBeamHistLo && hMean5 < kBeamHistHi));
#if defined(PL_CLEAN)
			want5[ws] = want5[ws] && !cleanMoving;
#endif
		}
		[branch] if (any(want5))
		{
			float4 gSum = 0.0, gW = 0.0, gW2 = 0.0, lo5 = 2.0, hi5 = -1.0, q5Sum = 0.0, q5Count = 0.0;
			const float invTwoSigma2 = 1.0 / (2.0 * kHardEdgeSigmaPx * kHardEdgeSigmaPx);
			[loop] for (uint k5 = 0u; k5 < 25u; ++k5)
			{
				const int2 o5 = int2(int(k5 % 5u) - 2, int(k5 / 5u) - 2);
				const int2 q = clamp(p + o5, int2(0, 0), maxP);
				const float dq = abs(DepthMask[q]);
				float wPlane = 0.0;
				if (!RTIsSky(dq))
					wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / planeTolerance);
				[branch] if (wPlane >= 0.05)
				{
					const uint4 idq = Ids(PLIdx[q]);
					const float4 vq = PLVis[q];
					const float4 wq = WideAt(q, flags);
					const float wG = wPlane * exp(-float(o5.x * o5.x + o5.y * o5.y) * invTwoSigma2);
					[unroll] for (uint s5 = 0u; s5 < 4u; ++s5)
					{
						float r;
						if (want5[s5] && MatchWide(idq, vq, wq, ids[s5], r)) {
							const float v = Lit(r);
							gSum[s5] += v * wG;
							gW[s5] += wG;
							gW2[s5] += wG * wG;
							lo5[s5] = min(lo5[s5], v);
							hi5[s5] = max(hi5[s5], v);
							if (v < 0.5) {
								const float t = r * (255.0 / 254.0);
								q5Sum[s5] += t * t;
								q5Count[s5] += 1.0;
							}
						}
					}
				}
			}
			[unroll] for (uint e5 = 0u; e5 < 4u; ++e5)
			{
				[branch] if (want5[e5] && gW[e5] > 0.0 && hi5[e5] > lo5[e5])
				{
					edge5[e5] = true;
					mean5[e5] = gSum[e5] / gW[e5];
					neff5[e5] = gW[e5] * gW[e5] / max(gW2[e5], 1e-6);
					const float qEdge = q5Count[e5] > 0.0 ? q5Sum[e5] / q5Count[e5] : 0.0;
					[branch] if (smoothHard && FixPointRadius(ids[e5], qEdge, footprint) <= kHardEdgeMaxPx)
					{
						hardSlot[e5] = true;
						spatial[e5] = mean5[e5];
						beamRadius[e5] = 0.0;  // no Poisson pass and no probe for this slot
						beamProbe[e5] = false;
					}
				}
			}
		}
	}
	// The per-pixel rotation of the taps (per frame), and the fixed one (47y Smooth When No History).
	// (48k) Decorrelated Sampling: both from the hash instead (not the trace's IGN).
	const bool fixDecorrelate = kVariantFix && (flags & kPLFlagFixDecorrelate) != 0u;
	float sn, cs;
	sincos(6.2831853 * (fixDecorrelate ? FixHash(uint2(p), PLEmitter.w) : InterleavedGradientNoise(float2(p), PLEmitter.w)), sn, cs);
	float snStill = sn, csStill = cs;
	[branch] if (flags & kPLFlagNoHistorySmooth)
		sincos(6.2831853 * (fixDecorrelate ? FixHash(uint2(p), 0x9E3779B9u) : InterleavedGradientNoise(float2(p), 0u)), snStill, csStill);
	const float maxRadius = max(max(radiusPx.x, radiusPx.y), max(radiusPx.z, radiusPx.w));
	[branch] if ((flags & kPLFlagSmooth) && maxRadius > 1.5)
	{
		// (47x) Per slot at its own radius: the pixel itself plus kPoissonTaps rotated disc taps (plane x Gaussian weights as before).
		[unroll] for (uint s = 0u; s < 4u; ++s)
		{
			[branch] if (radiusPx[s] > 1.5)
			{
				const float radius = radiusPx[s];
				const bool still = (flags & kPLFlagNoHistorySmooth) && hw[s] < 0.05;
				const float sns = still ? snStill : sn, css = still ? csStill : cs;
				const float invR2 = 1.0 / (radius * radius);
				float wsum = Lit(raw[s]), wwt = 1.0;
				[loop] for (uint k = 0u; k < kPoissonTaps; ++k)
				{
					const float2 d = kPoisson[k];
					const float2 r = float2(d.x * css - d.y * sns, d.x * sns + d.y * css) * radius;
					const int2 o = int2(round(r));
					const int2 q = clamp(p + o, int2(0, 0), maxP);
					const float dq = abs(DepthMask[q]);
					[branch] if (any(o != 0) && !RTIsSky(dq))
					{
						const float wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / planeTolerance);
						float rq;
						if (wPlane >= 0.05 && MatchWide(Ids(PLIdx[q]), PLVis[q], WideAt(q, flags), ids[s], rq)) {
							const float w = wPlane * exp(-1.5 * dot(r, r) * invR2);
							wsum += Lit(rq) * w;
							wwt += w;
						}
					}
				}
				spatial[s] = wsum / wwt;
			}
		}
	}
	else if (maxRadius > 1.5)
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
				const float4 wq = WideAt(q, flags);
				const float r2 = float(o.x * o.x + o.y * o.y) * float(stepPx * stepPx);
				[unroll] for (uint s = 0u; s < 4u; ++s)
				{
					float r;
					if (radiusPx[s] > 1.5 && MatchWide(idq, vq, wq, ids[s], r)) {
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

	// (48a) Window Beams: Smooth Edges: the beam slots' own pass (independent of Smoother Shadow Edges).
	float4 beamNeff = weights2 > 0.0 ? weights * weights / max(weights2, 1e-6) : 1.0;  // effective sample count of the mean in spatial
	// (48k) Stable Penumbra's slots take this pass too (their own radius, the normal plane test).
	[branch] if (any(beamSlot) || any(fixSlot))
	{
		[unroll] for (uint s = 0u; s < 4u; ++s)
		{
			const float slotPlaneTolerance = (kVariantFix && fixSlot[s]) ? planeTolerance : beamPlaneTolerance;
			// The probe: kPoissonTaps taps at kBeamProbeRadiusPx; the blocked ones' distance ratio gives the radius (none: no wide pass).
			[branch] if (beamProbe[s])
			{
				float pqSum = 0.0, pqCount = 0.0;
				[loop] for (uint k = 0u; k < kPoissonTaps; ++k)
				{
					const float2 d = kPoisson[k];
					const float2 r = float2(d.x * cs - d.y * sn, d.x * sn + d.y * cs) * kBeamProbeRadiusPx;
					const int2 o = int2(round(r));
					const int2 q = clamp(p + o, int2(0, 0), maxP);
					const float dq = abs(DepthMask[q]);
					[branch] if (!RTIsSky(dq))
					{
						const float wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / slotPlaneTolerance);
						float rq;
						if (wPlane >= 0.05 && MatchWide(Ids(PLIdx[q]), PLVis[q], WideAt(q, flags), ids[s], rq)) {
							if (rq < 0.999) {
								const float t = rq * (255.0 / 254.0);
								pqSum += t * t;
								pqCount += 1.0;
							}
						}
					}
				}
				if (pqCount > 0.0) {
					// (48k) Stable Penumbra's slots: the point-light radius, at least kFixMinRadiusPx.
					if (kVariantFix && fixSlot[s])
						beamRadius[s] = max(FixPointRadius(ids[s], pqSum / pqCount, footprint), min(kFixMinRadiusPx, PLParams.w));
					else
						beamRadius[s] = clamp(saturate(pqSum / pqCount) * kBeamBlockerRange * PLExtra.w / footprint, 1.0, kBeamMaxRadiusPx);
				}
			}
			[branch] if (beamRadius[s] >= 1.5)
			{
#if defined(PL_CLEAN)
				const float radius = cleanMoving ? min(beamRadius[s], kCleanMaxRadiusPx) : beamRadius[s];  // (48m)
#else
				const float radius = beamRadius[s];
#endif
				const bool still = (flags & kPLFlagNoHistorySmooth) && hw[s] < 0.05;
				const float sns = still ? snStill : sn, css = still ? csStill : cs;
				const float invR2 = 1.0 / (radius * radius);
				const uint taps = radius > float(kPoissonTaps) ? 2u * kPoissonTaps : kPoissonTaps;
				float wsum = Lit(raw[s]), wwt = 1.0, wwt2 = 1.0;
				[loop] for (uint k = 0u; k < taps; ++k)
				{
					const float2 d0 = kPoisson[k % kPoissonTaps];
					// The second ring: the same disc turned by the golden angle.
					const float2 d = k < kPoissonTaps ? d0 : float2(d0.x * kGoldenTurn.x - d0.y * kGoldenTurn.y, d0.x * kGoldenTurn.y + d0.y * kGoldenTurn.x);
					const float2 r = float2(d.x * css - d.y * sns, d.x * sns + d.y * css) * radius;
					const int2 o = int2(round(r));
					const int2 q = clamp(p + o, int2(0, 0), maxP);
					const float dq = abs(DepthMask[q]);
					[branch] if (any(o != 0) && !RTIsSky(dq))
					{
						const float wPlane = exp(-abs(dot(N, RTPosition(q, dq) - P)) / slotPlaneTolerance);
						float rq;
						if (wPlane >= 0.05 && MatchWide(Ids(PLIdx[q]), PLVis[q], WideAt(q, flags), ids[s], rq)) {
							const float w = wPlane * exp(-1.5 * dot(r, r) * invR2);
							wsum += Lit(rq) * w;
							wwt += w;
							wwt2 += w * w;
						}
					}
				}
				spatial[s] = wsum / wwt;
				beamNeff[s] = wwt * wwt / wwt2;
			}
		}
	}

	// (49i) Smooth Hard Edges: the effective sample count of the 5x5 Gaussian for the clamp band.
	[unroll] for (uint hn = 0u; hn < 4u; ++hn)
		if (hardSlot[hn])
			beamNeff[hn] = neff5[hn];

	// 3b. History blend (taps read by the caller; hw = 0 without usable history).
	float4 result = spatial;
#if defined(PL_CLEAN)
	o_weight = 1.0;
	o_radius = max(radiusPx, cleanMoving ? min(beamRadius, kCleanMaxRadiusPx) : beamRadius);
	[unroll] for (uint op = 0u; op < 4u; ++op)
		o_path[op] = ids[op] == 0u ? 0u : (beamSlot[op] ? 1u : (fixSlot[op] ? 2u : 3u));
#endif
	[unroll] for (uint sh = 0u; sh < 4u; ++sh)
	{
		if (ids[sh] != 0u && hw[sh] >= 0.05) {
			const float drop = HistoryDrop(ids[sh]);
			float lo3, hi3, current;
#if defined(PL_CLEAN)
			[branch] if (cleanMoving)
			{
				// (48m) A moving receiver, any path: the 3x3 exactly where it agrees, else the filtered mean +- max(2 sigma, kCleanBandMin);
				// this frame at least kCleanWeight.
				if (lo[sh] <= 1.0 && hi[sh] <= lo[sh]) {
					lo3 = lo[sh];
					hi3 = hi[sh];
				} else {
					const float mc = spatial[sh];
					const float bandc = max(kBeamClampK * sqrt(saturate(mc * (1.0 - mc)) / max(beamNeff[sh], 1.0)), kCleanBandMin);
					lo3 = mc - bandc;
					hi3 = mc + bandc;
				}
				current = lerp(max(PLParams.y, kCleanWeight), 1.0, drop);
			}
			else
#endif
			if (beamSlot[sh] || (kVariantFix && fixSlot[sh])) {  // (48k) Stable Penumbra's slots alike
				// (48a) The filtered mean +- k sigma of that mean (binary samples: sigma^2 = m (1 - m)); static beams keep more history,
				// but only while it lies inside that band (a history that had to be clamped, e.g. a character's shadow moving over the
				// floor, is replaced at the normal rate: no longer trails).
				// (49i) Steady Edges: on an edge pixel the clamp is the 5x5 Gaussian mean +- max(2 sigma of it, kSteadyBandMin).
				const bool steadySlot = steadyEdges && edge5[sh];
				const float m = steadySlot ? mean5[sh] : spatial[sh];
				const float neffBand = steadySlot ? neff5[sh] : beamNeff[sh];
				const float band = max(kBeamClampK * sqrt(saturate(m * (1.0 - m)) / max(neffBand, 1.0)), steadySlot ? kSteadyBandMin : kBeamClampMin);
				lo3 = m - band;
				hi3 = m + band;
				const float hRaw = hv[sh] / hw[sh];
				const bool inBand = hRaw >= lo3 && hRaw <= hi3;
#if defined(PL_CLEAN)
				float weight = drop > 0.0 || !inBand ? PLParams.y : kBeamStaticWeight;
				[branch] if (cleanOn)
				{
					// (48m) A static receiver: a history far from this frame's 3x3 mean (a moving caster) takes the 3x3 clamp and
					// kCleanStaticWeight; otherwise the 0.1 only while the history is in the band's inner half.
					if (abs(hRaw - spatial3[sh]) > kCleanStrongChange) {
						lo3 = (lo[sh] <= 1.0 ? lo[sh] : spatial[sh]) - 0.1;
						hi3 = (hi[sh] >= 0.0 ? hi[sh] : spatial[sh]) + 0.1;
						weight = max(PLParams.y, kCleanStaticWeight);
					} else if (drop <= 0.0 && inBand) {
						// (49i) Steady Edges: the jittered edge samples average at the static weight across the whole band.
						weight = steadySlot ? kBeamStaticWeight : lerp(kBeamStaticWeight, PLParams.y, saturate(abs(hRaw - m) / band * 2.0 - 1.0));
					}
				}
				current = lerp(weight, 1.0, drop);
#else
				current = lerp(drop > 0.0 || !inBand ? PLParams.y : kBeamStaticWeight, 1.0, drop);
#endif
			} else {
				lo3 = (lo[sh] <= 1.0 ? lo[sh] : spatial[sh]) - 0.1;
				hi3 = (hi[sh] >= 0.0 ? hi[sh] : spatial[sh]) + 0.1;
				// (46d) A moving source (a carried torch) keeps less history: its shadows move with it, nothing smears.
				current = lerp(PLParams.y, 1.0, drop);
			}
			result[sh] = lerp(clamp(hv[sh] / hw[sh], lo3, hi3), spatial[sh], current);
#if defined(PL_CLEAN)
			o_weight[sh] = current;
#endif
		}
	}
	[unroll] for (uint e = 0u; e < 4u; ++e)
		if (ids[e] == 0u)
			result[e] = 1.0;
	return result;
}

[numthreads(8, 8, 1)] void main(uint3 dtid : SV_DispatchThreadID) {
	const int2 p = int2(dtid.xy);
	if (any(dtid.xy >= Size.xy))
		return;
	const uint flags = asuint(PLParams.z);
	const bool wide = kVariantWide && (flags & kPLFlagWide) != 0u;
	const bool wideHistory = wide && (flags & kPLFlagWideHistory) != 0u;
	const float4 idxRaw = PLIdx[p];
	const uint4 ids = Ids(idxRaw);
	const float4 wideRaw = WideAt(p, flags);  // (48a) (0, 0, 1, 1) unless K > 4
	const uint4 wideIds = uint4(Ids(float4(wideRaw.xy, 0.0, 0.0)).xy, 0u, 0u);
	const float depth = abs(DepthMask[p]);
	const float4 raw = PLVis[p];
	float4 result = 1.0;
	float2 wideResult = 1.0;
	float dist = -1.0;
#if defined(PL_CLEAN)
	uint movingState = 0u;  // (48m) Moving Pixels view: 1 moving + history taken, 2 moving + no history, 3 moving (toggle off: detection only)
	float4 dbgRadius = 0.0, dbgWeight = 1.0;
	uint4 dbgPath = 0u;
#endif

	[branch] if (!RTIsSky(depth) && (any(ids != 0u) || any(wideIds != 0u)))
	{
		PixelFrame px;
		px.p = p;
		px.flags = flags;
		px.P = RTPosition(p, depth);
		dist = length(px.P);
		px.maxP = int2(Size.xy) - 1;
		const float3 P = px.P;
		const int2 maxP = px.maxP;

		// Plane of this pixel (depth normal, smaller step per axis).
		{
			const int2 pl = int2(max(p.x - 1, 0), p.y), pr = int2(min(p.x + 1, maxP.x), p.y);
			const int2 pu = int2(p.x, max(p.y - 1, 0)), pd = int2(p.x, min(p.y + 1, maxP.y));
			const float dl = abs(DepthMask[pl]), dr = abs(DepthMask[pr]), du = abs(DepthMask[pu]), dd = abs(DepthMask[pd]);
			const float3 dx = abs(dl - depth) < abs(dr - depth) ? P - RTPosition(pl, dl) : RTPosition(pr, dr) - P;
			const float3 dy = abs(du - depth) < abs(dd - depth) ? P - RTPosition(pu, du) : RTPosition(pd, dd) - P;
			float3 N = cross(dy, dx);
			const float len = length(N);
			N = len > 1e-12 ? N / len : (dist > 0.0 ? -P / dist : float3(0, 0, 1));
			px.N = N;
			// World size of one pixel across the view ray (not the slanted surface: the penumbra is measured on it).
			px.footprint = max(length(RTPosition(pr, depth) - P), 1e-3);
		}
		px.planeTolerance = dist * 0.01 + 2.0;
#if defined(PL_CLEAN)
		px.moving = false;
#endif

		// 3a. (46o) The history taps first (read only; blended in after the spatial pass as before), so that a soft beam slot
		// without usable history (a moving character: rejected by distance) gets a wide spatial filter instead of 1 spp grain.
		// (48a) The chosen candidate (position, tolerance) is kept for slots 4 - 5.
		float4 hv = 0.0, hw = 0.0;
		float2 histPos = 0.0;
		float histExpected = 0.0, histTolerance = 0.0;
		bool histTaken = false;
		[branch] if (flags & kPLFlagHistory)
		{
			const float4 prevClip = mul(PrevViewProj, float4(P + CameraDelta.xyz, 1.0));
			const float2 prevUv = prevClip.w > 1e-4 ? float2(prevClip.x / prevClip.w * 0.5 + 0.5, 0.5 - prevClip.y / prevClip.w * 0.5) : float2(-1.0, -1.0);
			const float expected = length(P + CameraDelta.xyz);
			const float tolerance = expected * 0.02 + 8.0;
			histExpected = expected;
			float tapWeight = 0.0;
#if defined(PL_CLEAN)
			// (48m) Moving Characters: Clean Shadow History: is the receiver moving (last frame's motion vectors vs the camera reprojection)?
			bool movingClean = false;
			[branch] if ((flags & kPLFlagCleanDetect) && all(prevUv >= 0.0) && all(prevUv < 1.0))
			{
				const float2 uvc = (float2(p) + 0.5) / float2(Size.xy);
				float2 mc = uvc;  // from this pixel (the camera guess may lie off a moving body: its background's vector would hide the motion)
				[unroll] for (int ic = 0; ic < 2; ++ic)
				{
					const int2 qc = int2(clamp(mc, 0.0, 0.999999) * float2(Size.xy));
					mc = uvc + PrevMotion[qc];
				}
				const float residualPx = length((mc - prevUv) * float2(Size.xy));
				const float cameraPx = length((prevUv - uvc) * float2(Size.xy));
				const bool moving = residualPx > max(kCleanResidualPx, kCleanResidualShare * cameraPx);
				movingState = moving ? 3u : 0u;
				[branch] if (moving && (flags & kPLFlagClean))
				{
					// Only the motion-vector history, strictly; none otherwise. The strict plane test for the spatial passes.
					movingClean = true;
					px.moving = true;
					px.planeTolerance = dist * kCleanPlaneScale + kCleanPlaneBase;
					movingState = 2u;
					[branch] if (all(mc >= 0.0) && all(mc < 1.0))
					{
						const float tolc = expected * kCleanTolScale + kCleanTolBase;
						const float2 motionPos = mc * float2(Size.xy) - 0.5;
						float4 mv, mw;
						const float motionWeight = HistoryTaps(motionPos, expected, tolc, ids, wideHistory, maxP, mv, mw);
						if (motionWeight >= kCleanMinTapWeight) {
							hv = mv;
							hw = mw;
							histPos = motionPos;
							histTolerance = tolc;
							histTaken = true;
							movingState = 1u;
						}
					}
				}
			}
			[branch] if (!movingClean && all(prevUv >= 0.0) && all(prevUv < 1.0))
#else
			[branch] if (all(prevUv >= 0.0) && all(prevUv < 1.0))
#endif
			{
				histPos = prevUv * float2(Size.xy) - 0.5;
				histTolerance = tolerance;
				histTaken = true;
				tapWeight = HistoryTaps(histPos, expected, tolerance, ids, wideHistory, maxP, hv, hw);
			}
			// (47y) Moving Characters: Keep Shadow History: the camera found (almost) no valid taps -> last frame's motion vectors at
			// constant velocity (two fixed-point steps from the camera guess); the candidate with more valid taps wins.
#if defined(PL_CLEAN)
			[branch] if (!movingClean && (flags & kPLFlagMotion) && tapWeight < kMotionTryBelow)
#else
			[branch] if ((flags & kPLFlagMotion) && tapWeight < kMotionTryBelow)
#endif
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
					float4 mv, mw;
					const float2 motionPos = m * float2(Size.xy) - 0.5;
					const float motionWeight = HistoryTaps(motionPos, expected, tolerance + kMotionTolerance, ids, wideHistory, maxP, mv, mw);
					if (motionWeight > tapWeight) {
						hv = mv;
						hw = mw;
						histPos = motionPos;
						histTolerance = tolerance + kMotionTolerance;
						histTaken = true;
					}
				}
			}
		}

#if defined(PL_CLEAN)
		result = DenoiseSet(px, ids, raw, hv, hw, dbgRadius, dbgWeight, dbgPath);
#else
		result = DenoiseSet(px, ids, raw, hv, hw);
#endif

		// (48a) Slots 4 - 5 (K > 4): the same steps, history at the candidate chosen above.
		[branch] if (wide && any(wideIds != 0u))
		{
			float4 hv2 = 0.0, hw2 = 0.0;
			[branch] if (histTaken)
				HistoryTaps(histPos, histExpected, histTolerance, wideIds, wideHistory, maxP, hv2, hw2);
#if defined(PL_CLEAN)
			float4 wRadius, wWeight;
			uint4 wPath;
			wideResult = DenoiseSet(px, wideIds, float4(wideRaw.zw, 1.0, 1.0), hv2, hw2, wRadius, wWeight, wPath).xy;
#else
			wideResult = DenoiseSet(px, wideIds, float4(wideRaw.zw, 1.0, 1.0), hv2, hw2).xy;
#endif
		}
	}

	OutVis[p] = saturate(result);
	OutIdx[p] = idxRaw;
	OutDist[p] = dist;
	[branch] if (wide)
		OutWide[p] = float4(wideRaw.xy, saturate(wideResult));

	[branch] if (flags & kPLFlagDebug)
	{
		const uint view = Size.z;
		float4 debug = 0.0;
		if (view == kViewPLCandidates) {
			// Shadow sources that reach this pixel (its lit side), before the top K and the share cut.
			const uint4 bytes = uint4(idxRaw * 255.0 + 0.5);
			// (46y) 3 bits (0..7); byte 3's bit 7 is the traced-caster mark (Screen-Space Shadows for Non-RT Casters).
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
			// Colour of the source in slot 0; diagonal stripes in the colour of slot 1 (slots 2-3: thinner stripes; (48a) 4-5 likewise).
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
		} else if (view == kViewPLRaw) {
			if (any(ids != 0u) || any(wideIds != 0u)) {
				float v = 1.0;
				[unroll] for (uint s = 0u; s < 4u; ++s)
					if (ids[s] != 0u)
						v *= raw[s] >= 0.999 ? 1.0 : 0.0;
				[unroll] for (uint s2 = 0u; s2 < 2u; ++s2)
					if (wideIds[s2] != 0u)
						v *= wideRaw[2u + s2] >= 0.999 ? 1.0 : 0.0;
				debug = float4(v.xxx, 1.0);
			}
		} else if (view == kViewPLDenoised) {
			if (any(ids != 0u) || any(wideIds != 0u)) {
				float v = 1.0;
				[unroll] for (uint s = 0u; s < 4u; ++s)
					if (ids[s] != 0u)
						v *= saturate(result[s]);
				[unroll] for (uint s2 = 0u; s2 < 2u; ++s2)
					if (wideIds[s2] != 0u)
						v *= saturate(wideResult[s2]);
				debug = float4(v.xxx, 1.0);
			}
		}
#if defined(PL_CLEAN)
		if (view == kViewPLMoving) {
			// (48m) Red: moving receiver, motion-vector history taken; yellow: moving, no usable history (this frame only); magenta: moving
			// with Clean Shadow History off (detection only). Nothing: not moving (or nothing traced here).
			if (movingState == 1u)
				debug = float4(1.0, 0.1, 0.1, 0.6);
			else if (movingState == 2u)
				debug = float4(1.0, 0.9, 0.1, 0.6);
			else if (movingState == 3u)
				debug = float4(1.0, 0.2, 1.0, 0.6);
		} else if (view == kViewPLFilterPath) {
			// (48m) Slot 0 (the strongest traced light). Even rows: its path (red = window beam path, green = Stable Penumbra, blue = 46b),
			// brighter = wider wide pass (full at 12 px); odd rows: this frame's weight in the history blend (dark = 0.1, white = 1 or no history).
			if (dbgPath.x != 0u) {
				const float3 pathColor = dbgPath.x == 1u ? float3(1.0, 0.2, 0.1) : (dbgPath.x == 2u ? float3(0.1, 1.0, 0.2) : float3(0.2, 0.4, 1.0));
				if ((p.y & 1) == 0)
					debug = float4(pathColor * (0.25 + 0.75 * saturate(dbgRadius.x / 12.0)), 1.0);
				else
					debug = float4(saturate(dbgWeight.x).xxx, 1.0);
			}
		}
#endif
		DebugColor[p] = debug;
	}
}
