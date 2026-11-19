/// By ProfJack/五脚猫, 2024-2-17 UTC
/// ref:
/// https://bruop.github.io/exposure/
/// https://knarkowicz.wordpress.com/2016/01/09/automatic-exposure/

#include "PostProcessing/HistogramAutoExposure/common.hlsli"

#include "Common/Color.hlsli"

RWStructuredBuffer<uint> RWBufferHistogram : register(u0);
// (49h stage 2) Four floats: [0] the divisor the adapt pass uses (Classic: the adapted luminance), [1] the adapted scene
// luminance (Purkinje, readouts), [2] the filtered target, [3] the compensation curve (linear; Classic 1).
RWStructuredBuffer<float> RWBufferAdaptation : register(u1);

Texture2D<float4> TexColor : register(t0);

const static float MinLogLum = -8;    // -5 EV
const static float LogLumRange = 21;  // -5 to 16 EV
const static float RcpLogLumRange = rcp(LogLumRange);

// Increased thread count per group for better occupancy
groupshared uint histogramShared[256];
// (49h stage 2) Physical mode: prefix sums and the trimmed mean's two sums.
groupshared float scanShared[256];
groupshared float sumShared[256];
groupshared float weightShared[256];

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
		float luma = Color::RGBToLuminance(color);

		[branch] if (PhysMode == 0u)
		{
			// Optimized bin calculation - avoid unnecessary saturate
			if (luma > 1e-10) {
				float logLuma = (log2(luma) - MinLogLum) * RcpLogLumRange;
				uint bin = uint(lerp(1, 255, min(1.0, max(0.0, logLuma))));
				InterlockedAdd(histogramShared[bin], SAMPLE_SPACING * SAMPLE_SPACING);
			}
		}
		else
		{
			// (49h stage 2) Physical: the wide range from the cbuffer; black counts as the darkest bin (the trim drops it); an
			// optional centre weight (x1 at the corners up to x3 in the middle).
			const float logLuma = (log2(max(luma, 1e-30)) - PhysMinLogLum) / PhysLogLumRange;
			const uint bin = uint(lerp(1, 255, saturate(logLuma)));
			const float2 centre = (float2(pxCoord) / float2(dims) - 0.5) * 2.0;
			const float weight = 1.0 + 2.0 * PhysCentreWeight * exp(-dot(centre, centre) / (2.0 * 0.3 * 0.3 * 2.0));
			InterlockedAdd(histogramShared[bin], uint(SAMPLE_SPACING * SAMPLE_SPACING * weight + 0.5));
		}
	}

	GroupMemoryBarrierWithGroupSync();

	// Save to texture - only need to do this once per group
	if (gidx < 256) {
		InterlockedAdd(RWBufferHistogram[gidx], histogramShared[gidx]);
	}
};

float Softplus(float x)
{
	return x > 20.0 ? x : log(1.0 + exp(x));
}

/// (49h stage 2) Physical mode: the trimmed log mean of this frame's histogram, the filtered target, the asymmetric log-domain
/// adaptation and the compensation curve. One group of 256 threads, one bin each.
void PhysicalAverage(uint gidx)
{
	const float count = float(RWBufferHistogram[gidx]);
	RWBufferHistogram[gidx] = 0;  // for next frame

	// Inclusive prefix sum of the bin counts.
	scanShared[gidx] = count;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint offset = 1; offset < 256; offset <<= 1)
	{
		const float v = gidx >= offset ? scanShared[gidx - offset] : 0.0;
		GroupMemoryBarrierWithGroupSync();
		scanShared[gidx] += v;
		GroupMemoryBarrierWithGroupSync();
	}
	const float total = scanShared[255];
	const float lo = total * PhysTrimLow;
	const float hi = total * (1.0 - PhysTrimHigh);
	const float binEnd = scanShared[gidx];
	const float binStart = binEnd - count;
	const float w = max(min(binEnd, hi) - max(binStart, lo), 0.0);
	// Bin i holds log luminance in [(i - 1) / 254, i / 254) of the range: its middle.
	const float binLog = PhysMinLogLum + ((float(gidx) - 0.5) / 254.0) * PhysLogLumRange;
	sumShared[gidx] = gidx > 0u ? w * binLog : 0.0;
	weightShared[gidx] = gidx > 0u ? w : 0.0;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (gidx < stride) {
			sumShared[gidx] += sumShared[gidx + stride];
			weightShared[gidx] += weightShared[gidx + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (gidx == 0) {
		const float logLumMin = log2(0.125) + PhysAdaptMinEV;
		const float logLumMax = log2(0.125) + PhysAdaptMaxEV;
		const float previousAdapted = RWBufferAdaptation[1];
		const float previousTarget = RWBufferAdaptation[2];
		const bool historyValid = previousAdapted > 0.0 && previousTarget > 0.0 && !isinf(previousAdapted) && !isinf(previousTarget) &&
		                          !isnan(previousAdapted) && !isnan(previousTarget);
		const bool haveTarget = weightShared[0] > 0.0;
		float target = haveTarget ? clamp(sumShared[0] / weightShared[0], logLumMin, logLumMax) : (historyValid ? log2(previousTarget) : log2(0.125));

		float adapted;
		float filtered;
		[branch] if ((PhysFlags & 1u) != 0u || !historyValid)
		{
			adapted = target;
			filtered = target;
		}
		else if ((PhysFlags & 2u) != 0u)
		{
			adapted = log2(previousAdapted);
			filtered = log2(previousTarget);
		}
		else
		{
			const float dt = max(PhysDeltaTime, 0.0);
			filtered = log2(previousTarget);
			filtered += (target - filtered) * (1.0 - exp(-dt / 0.2));
			adapted = log2(previousAdapted);
			const float delta = filtered - adapted;
			const bool intoDark = delta < 0.0;
			float step = delta * (1.0 - exp(-dt / max(intoDark ? PhysTauDark : PhysTauBright, 1e-3)));
			step *= PhysDeadband > 0.5 ? smoothstep(0.10, 0.35, abs(delta)) : 1.0;
			const float slew = (intoDark ? PhysSlewDark : PhysSlewBright) * dt;
			adapted += clamp(step, -slew, slew);
		}
		adapted = clamp(adapted, logLumMin, logLumMax);
		filtered = clamp(filtered, logLumMin, logLumMax);

		// Compensation curve on the adapted scene EV (log2 of luminance / 0.125), the adaptation held at EV_min.
		const float ev = max(adapted - log2(0.125), PhysEVMin);
		const float compensation = -PhysAlpha * Softplus(PhysEVRef - ev) + PhysBeta * Softplus(ev - PhysEVHi);
		RWBufferAdaptation[0] = exp2(ev + log2(0.125) - compensation);
		RWBufferAdaptation[1] = exp2(adapted);
		RWBufferAdaptation[2] = exp2(filtered);
		RWBufferAdaptation[3] = exp2(compensation);
	}
}

[numthreads(256, 1, 1)] void CS_Average(uint gidx
										: SV_GroupIndex) {
	[branch] if (PhysMode != 0u)
	{
		PhysicalAverage(gidx);
		return;
	}

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
		// (49h stage 2) The other three slots for the readers that take them (Purkinje reads [1]).
		RWBufferAdaptation[1] = adaptedLum;
		RWBufferAdaptation[2] = avgLum;
		RWBufferAdaptation[3] = 1.0;
	}
}
