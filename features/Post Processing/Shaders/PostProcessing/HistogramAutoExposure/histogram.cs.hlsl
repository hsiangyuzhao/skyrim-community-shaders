/// By ProfJack/五脚猫, 2024-2-17 UTC
/// ref:
/// https://bruop.github.io/exposure/
/// https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/

#include "PostProcessing/HistogramAutoExposure/common.hlsli"

#include "Common/Color.hlsli"

RWStructuredBuffer<uint> RWBufferHistogram : register(u0);
RWStructuredBuffer<float> RWBufferAdaptation : register(u1);

Texture2D<float4> TexColor : register(t0);

const static float MinLogLum = -8;    // -5 EV
const static float LogLumRange = 21;  // -5 to 16 EV
const static float RcpLogLumRange = rcp(LogLumRange);

// Increased thread count per group for better occupancy
groupshared uint histogramShared[256];

// Optimized hash function using fewer operations
float2 hash2D(float2 p)
{
	float3 p3 = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
	p3 += dot(p3, p3.yzx + 33.33);
	return frac((p3.xx + p3.yz) * p3.zy);
}

// Precompute box bounds to avoid per-pixel calculations
float4 ComputeBoxBounds(float2 dims)
{
	float4 box = float4(.5 - AdaptArea * .5, .5 + AdaptArea * .5);
	return float4(
		dims.x * box.r,
		dims.y * box.g,
		dims.x * box.b,
		dims.y * box.a);
}

[numthreads(32, 32, 1)] void CS_Histogram(uint2 tid
										  : SV_DispatchThreadID, uint gidx
										  : SV_GroupIndex) {
	uint2 dims;
	TexColor.GetDimensions(dims.x, dims.y);

	// Initialize shared memory - only need to do this once per group
	if (gidx < 256) {
		histogramShared[gidx] = 0;
	}
	GroupMemoryBarrierWithGroupSync();

	// Sample spacing - take fewer samples (1 out of N pixels)
	const uint SAMPLE_SPACING = 8;  // Sample every 8th pixel for 64x reduction in samples

	// Compute base pixel coordinate with spacing
	uint2 pxCoord = tid * (2 * SAMPLE_SPACING);

	// Calculate jitter offset using blue noise distribution
	float2 jitter = hash2D(tid) * (SAMPLE_SPACING - 0.5) * 2.0;
	int2 jitterOffset = int2(jitter);

	// Optimized bounds checking
	pxCoord = uint2(max(0, min(int2(pxCoord) + jitterOffset, int2(dims) - 1)));

	// Precompute box bounds
	float4 boxBounds = ComputeBoxBounds(dims);

	// Optimized box check using precomputed bounds
	bool inBox = (pxCoord.x > boxBounds.x) && (pxCoord.x < boxBounds.z) &&
	             (pxCoord.y > boxBounds.y) && (pxCoord.y < boxBounds.w);

	if (inBox) {
		float3 color = TexColor[pxCoord].rgb;
		float luma = Color::RGBToLuminance(color) * SceneScaleInv;  // (52a) the buffer is pre-exposed by S: meter buffer / S

		// Optimized bin calculation - avoid unnecessary saturate
		if (luma > 1e-10) {
#if defined(EYE_MODE)
			// (50a) Eye Model: the wider range; the Scene Scale Wobble self-test scales the frame here.
			float logLuma = (log2(luma * EyeWobble) - kEyeMinLogLum) * rcp(kEyeLogLumRange);
#else
			float logLuma = (log2(luma) - MinLogLum) * RcpLogLumRange;
#endif
			uint bin = uint(lerp(1, 255, min(1.0, max(0.0, logLuma))));
			InterlockedAdd(histogramShared[bin], SAMPLE_SPACING * SAMPLE_SPACING);
		}
	}

	GroupMemoryBarrierWithGroupSync();

	// Save to texture - only need to do this once per group
	if (gidx < 256) {
		InterlockedAdd(RWBufferHistogram[gidx], histogramShared[gidx]);
	}
};

#if !defined(EYE_MODE)
[numthreads(256, 1, 1)] void CS_Average(uint gidx
										: SV_GroupIndex) {
	uint2 dims;
	TexColor.GetDimensions(dims.x, dims.y);
	uint numPixels = dims.x * dims.y * AdaptArea.x * AdaptArea.y * 0.25;

	// Optimized initialization
	if (gidx < 256) {
		uint pixelsInBin = RWBufferHistogram[gidx];
		histogramShared[gidx] = pixelsInBin * gidx;
		RWBufferHistogram[gidx] = 0;  // for next frame
	}
	GroupMemoryBarrierWithGroupSync();

	// Optimized reduction using bit shifts
	[unroll] for (uint offset = 128; offset > 0; offset >>= 1)
	{
		if (gidx < offset)
			histogramShared[gidx] += histogramShared[gidx + offset];
		GroupMemoryBarrierWithGroupSync();
	}

	// Optimized average calculation
	if (gidx == 0) {
		float logAvgLum = float(histogramShared[0]) / max(numPixels, 1.0) - 1.0;
		float avgLum = exp2(((logAvgLum / 254.0) * LogLumRange) + MinLogLum);
		float adaptedLum = lerp(max(1e-5, RWBufferAdaptation[0]), avgLum, AdaptLerp);
		RWBufferAdaptation[0] = adaptedLum;
	}
}
#else
// (50a) Eye Model (physical lighting redo, stage 1; design review section 4). One group: the bin counts are copied to groupshared, then
// one thread walks the 254 bins (cheap: once per frame) and writes the adaptation buffer:
//   [0] the divisor the exposure uses (0.18 x compensation / [0]; Local Exposure's fold and Neural Rendering read it as before, with the
//       pass-through range the C++ uploads in eye mode), [1] the adapted level L_a (the Purkinje key), [2] this frame's measured level,
//       [3] the level change Night Darkness / Minimum Visibility applied (EV, + = brighter).
// Metering: the darkest 5 % of the counted pixels are trimmed (fractionally inside the boundary bin), the rest is winsorized at the median
// + 3 EV (brighter bins count at that level), then log-averaged. All in log2 of scene luminance, so a scale of the frame (the self-test)
// moves every quantity by the same amount; it is divided back out (EyeWobble) before the eye's rules apply.
static const float kEyeTrimDarkest = 0.05;
static const float kEyeWinsorEV = 3.0;

float EyeBinLog(uint a_bin)
{
	return kEyeMinLogLum + (float(a_bin) - 0.5) / 254.0 * kEyeLogLumRange;  // bins 1 .. 255 (CS_Histogram: lerp(1, 255, x) truncated)
}

[numthreads(256, 1, 1)] void CS_Average(uint gidx
										: SV_GroupIndex) {
	if (gidx < 256) {
		histogramShared[gidx] = RWBufferHistogram[gidx];
		RWBufferHistogram[gidx] = 0;  // for next frame
	}
	GroupMemoryBarrierWithGroupSync();
	if (gidx != 0)
		return;

	float total = 0.0;
	for (uint b = 1; b < 256; ++b)
		total += float(histogramShared[b]);

	float state = RWBufferAdaptation[1] > 0.0 ? log2(RWBufferAdaptation[1]) : 0.0;
	const bool stateValid = RWBufferAdaptation[1] > 0.0 && !isnan(state) && !isinf(state);
	float measured = state;
	[branch] if (total > 0.0)
	{
		// Median (fractional within its bin) and the trim boundary.
		float median = EyeBinLog(255u), cumulative = 0.0;
		for (uint m = 1; m < 256; ++m) {
			const float c = float(histogramShared[m]);
			if (c > 0.0 && cumulative + c >= 0.5 * total) {
				median = EyeBinLog(m) + ((0.5 * total - cumulative) / c - 0.5) * (kEyeLogLumRange / 254.0);
				break;
			}
			cumulative += c;
		}
		const float winsor = median + kEyeWinsorEV;
		const float trim = kEyeTrimDarkest * total;
		float trimmed = 0.0, sum = 0.0, kept = 0.0;
		for (uint k = 1; k < 256; ++k) {
			float c = float(histogramShared[k]);
			const float cut = min(c, max(trim - trimmed, 0.0));
			trimmed += cut;
			c -= cut;
			sum += c * min(EyeBinLog(k), winsor);
			kept += c;
		}
		measured = kept > 0.0 ? sum / kept - log2(max(EyeWobble, 1e-6)) : state;
	}

	// Adaptation in log2: deadband, then toward the measured level with its own time constant into dark / bright, at most EyeMaxRate EV/s.
	[branch] if ((EyeFlags & (kEyeSnap | kEyeInstant)) || !stateValid)
	{
		state = measured;
	}
	else
	{
		float diff = measured - state;
		const float excess = max(abs(diff) - EyeDeadband, 0.0);
		diff = diff < 0.0 ? -excess : excess;
		const float tau = max(diff > 0.0 ? EyeTauBright : EyeTauDark, 1e-3);
		float step = diff * (1.0 - exp(-max(EyeDt, 0.0) / tau));
		if (EyeMaxRate > 0.0)
			step = clamp(step, -EyeMaxRate * EyeDt, EyeMaxRate * EyeDt);
		state += step;
	}

	// The key (stage 1, R = R_day: no physical input yet). Within the user's Adaptation Range the eye adapts as Classic; below it Classic keeps
	// the divisor at the minimum (the scene's own darkness is shown): Night Darkness = the share of that darkness kept (1 = Classic).
	const float clamped = clamp(state, EyeLogLo, EyeLogHi);
	float logDivisor = clamped - (1.0 - saturate(EyeNightDarkness)) * max(EyeLogLo - state, 0.0);
	// Minimum Visibility, global form (Local Exposure off; with it on, its local base gets the floor): the shown average relative to middle
	// grey, e = state - logDivisor, gets a soft floor: 2^e' = 2^e + Minimum Visibility.
	[branch] if ((EyeFlags & kEyeGlobalFloor) && EyeMinVisibility > 0.0)
	{
		const float e = state - logDivisor;
		const float lifted = log2(exp2(e) + EyeMinVisibility);
		logDivisor -= lifted - e;
	}
	RWBufferAdaptation[0] = exp2(logDivisor);
	RWBufferAdaptation[1] = exp2(state);
	RWBufferAdaptation[2] = exp2(measured);
	RWBufferAdaptation[3] = clamped - logDivisor;
}
#endif