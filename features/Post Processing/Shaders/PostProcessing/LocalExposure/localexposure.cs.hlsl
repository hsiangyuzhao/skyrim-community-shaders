/// (batch 38, item 4) Local Exposure. Port of upstream PostProcessing/LocalExposure (final form
/// after 8bb3247a3 "rewrite local exposure" and 5ac69b21b) plus the local-exposure part of
/// upstream's Composite pass (composite.ps.hlsl: RemapBaseContrast, ComputeLocalExposure).
///
/// Builds an edge-aware base in log-luminance space (bilateral grid blended with a wide blur),
/// then compresses the base around middle grey while keeping the detail layer. Upstream applies
/// the result in Composite together with the global exposure; we run after Histogram Auto
/// Exposure has already exposed the image, so CSApplyLocalExposure resolves the base and applies
/// the adjustment in one pass and writes the new image.

#include "Common/Color.hlsli"

#define GRID_DEPTH 32
#define GRID_TILE_SIZE 64
#define GRID_THREAD_SIZE 8
#define GRID_SAMPLE_STRIDE 8
#define GRID_QUANTIZATION 4096
#define MAX_BLUR_RADIUS 64

cbuffer LocalExposureCB : register(b1)
{
	float GlobalExposure;  // 1 when auto exposure already exposed the image
	float Strength;
	float HighlightContrast;
	float ShadowContrast;

	float DetailStrength;
	float BaseBlend;
	float BlurRadius;
	float MiddleGreyBias;

	float HighlightThreshold;
	float ShadowThreshold;
	float HighlightThresholdStrength;
	float ShadowThresholdStrength;

	uint InputWidth;
	uint InputHeight;
	uint BlurredWidth;
	uint BlurredHeight;

	float LogLuminanceMin;
	float LogLuminanceMax;
	float MiddleGreyCompensation;  // auto exposure's Exposure Compensation (linear), else 1
	uint Flags;                    // (49j) Physical Brightness: 1 pivot = 0.18 x AEAdaptation[3] (t4), 2 Minimum Visibility floor, 4 night vision

	float FloorLog;      // (49j) log2(Minimum Visibility x 0.18) + the vanilla night offset: no area's exposed base below it
	float NightVision;   // (49j) night vision colour strength (per-pixel mesopic desaturation and Cao blue shift)
	float2 DepthUvScale; // (49j) output UV -> the render-resolution depth's UV (t5); 0 = no depth (the sky is not left out of the floor)
};

Texture2D<float4> TexColor : register(t0);
Texture2D<float> TexLogLuminance : register(t1);
Texture3D<float2> TexLuminanceGrid : register(t2);
Texture2D<float> TexBlurredLuminance : register(t3);
SamplerState LinearSampler : register(s0);
SamplerState MirrorSampler : register(s1);

// (49j) Physical Brightness: the auto exposure's adaptation buffer (t4; also bound without the fold while Flags & 1) and the scene depth
// (t5, render resolution) for the sky test of the Minimum Visibility floor. The Cao model (exposure.hlsli) for the night vision colour.
StructuredBuffer<float> AEAdaptation : register(t4);
Texture2D<float> TexDepth : register(t5);
#include "PostProcessing/HistogramAutoExposure/exposure.hlsli"

#if defined(FOLD_AUTO_EXPOSURE)
// (44e) Histogram Auto Exposure's adapt pass folded in: its per-pixel exposure (and Purkinje shift)
// is applied here on load instead of being written to a full-resolution copy first. Same math
// (exposure.hlsli), same alpha (1) as the adapt pass wrote.
cbuffer AutoExposureFoldCB : register(b2)
{
	float2 AEAdaptArea;
	float2 AEAdaptationRange;
	float AEAdaptLerp;
	float AEExposureCompensation;
	float AEPurkinjeStartEV;
	float AEPurkinjeMaxEV;
	float AEPurkinjeStrength;
};

float4 LoadSceneColor(uint2 tid)
{
	return float4(AutoExposureMath::ExposeColor(TexColor[tid].rgb, AEAdaptation[0], AEAdaptation[1], AEExposureCompensation, AEAdaptationRange, AEPurkinjeStartEV, AEPurkinjeMaxEV, AEPurkinjeStrength), 1);
}
#else
float4 LoadSceneColor(uint2 tid)
{
	return TexColor[tid];
}
#endif

RWTexture2D<float> RWTexOutput : register(u0);
RWTexture3D<float2> RWTexLuminanceGrid : register(u1);

float SceneLogLuminance(float3 color)
{
	float luminance = Color::RGBToLuminance(max(color, 0.0));
	return clamp(log2(max(luminance, exp2(LogLuminanceMin))), LogLuminanceMin, LogLuminanceMax);
}

[numthreads(8, 8, 1)] void CSSetupLogLuminance(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= InputWidth || tid.y >= InputHeight)
		return;

	RWTexOutput[tid] = SceneLogLuminance(LoadSceneColor(tid).rgb);
}

[numthreads(8, 8, 1)] void CSDownsampleLogLuminance(uint2 tid : SV_DispatchThreadID) {
	uint2 outputSize;
	RWTexOutput.GetDimensions(outputSize.x, outputSize.y);
	if (any(tid >= outputSize))
		return;

	uint2 inputSize;
	TexLogLuminance.GetDimensions(inputSize.x, inputSize.y);

	float2 uv = (float2(tid) + 0.5) / float2(outputSize);
	float2 radius = 0.5 / float2(inputSize);
	float result = 0.0;
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(-radius.x, -radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(radius.x, -radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(-radius.x, radius.y), 0);
	result += TexLogLuminance.SampleLevel(LinearSampler, uv + float2(radius.x, radius.y), 0);
	RWTexOutput[tid] = result * 0.25;
}

float BlurLogLuminance(uint2 tid, float2 direction)
{
	uint2 outputSize;
	RWTexOutput.GetDimensions(outputSize.x, outputSize.y);
	float2 uv = (float2(tid) + 0.5) / float2(outputSize);
	float2 texelSize = rcp(float2(outputSize));
	float sigma = max(BlurRadius * 0.173, 0.5);
	float result = 0.0;
	float weightSum = 0.0;
	int kernelRadius = min((int)ceil(BlurRadius), MAX_BLUR_RADIUS);

	[loop] for (int offset = -kernelRadius; offset <= kernelRadius; offset++)
	{
		float weight = exp2(-0.72134752 * offset * offset / (sigma * sigma));
		result += TexLogLuminance.SampleLevel(MirrorSampler, uv + direction * texelSize * offset, 0) * weight;
		weightSum += weight;
	}

	return result / max(weightSum, 1e-5);
}

[numthreads(8, 8, 1)] void CSBlurHorizontal(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= BlurredWidth || tid.y >= BlurredHeight)
		return;

	RWTexOutput[tid] = BlurLogLuminance(tid, float2(1.0, 0.0));
}

[numthreads(8, 8, 1)] void CSBlurVertical(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= BlurredWidth || tid.y >= BlurredHeight)
		return;

	RWTexOutput[tid] = BlurLogLuminance(tid, float2(0.0, 1.0));
}

groupshared uint ThreadGridWeights[GRID_DEPTH][GRID_THREAD_SIZE * GRID_THREAD_SIZE];
groupshared uint ThreadGridLogSums[GRID_DEPTH][GRID_THREAD_SIZE * GRID_THREAD_SIZE];

[numthreads(GRID_THREAD_SIZE, GRID_THREAD_SIZE, 1)] void CSBuildLuminanceGrid(
	uint3 groupID : SV_GroupID,
	uint3 groupThreadID : SV_GroupThreadID,
	uint groupIndex : SV_GroupIndex) {
	[unroll] for (uint bin = 0; bin < GRID_DEPTH; bin++)
	{
		ThreadGridWeights[bin][groupIndex] = 0;
		ThreadGridLogSums[bin][groupIndex] = 0;
	}

	const float inverseLogRange = rcp(LogLuminanceMax - LogLuminanceMin);
	const uint2 tileOrigin = groupID.xy * GRID_TILE_SIZE;

	[unroll] for (uint y = 0; y < GRID_SAMPLE_STRIDE; y++)
	{
		[unroll] for (uint x = 0; x < GRID_SAMPLE_STRIDE; x++)
		{
			uint2 pixel = tileOrigin + groupThreadID.xy + uint2(x, y) * GRID_THREAD_SIZE;
			if (pixel.x < InputWidth && pixel.y < InputHeight) {
				float normalizedLog = saturate((TexLogLuminance[pixel] - LogLuminanceMin) * inverseLogRange);
				float binPosition = normalizedLog * (GRID_DEPTH - 1);
				uint lowerBin = min((uint)binPosition, GRID_DEPTH - 1);
				uint upperBin = min(lowerBin + 1, GRID_DEPTH - 1);
				uint upperWeight = (uint)(frac(binPosition) * GRID_QUANTIZATION + 0.5);
				uint lowerWeight = GRID_QUANTIZATION - upperWeight;

				ThreadGridWeights[lowerBin][groupIndex] += lowerWeight;
				ThreadGridLogSums[lowerBin][groupIndex] += (uint)(normalizedLog * lowerWeight + 0.5);
				ThreadGridWeights[upperBin][groupIndex] += upperWeight;
				ThreadGridLogSums[upperBin][groupIndex] += (uint)(normalizedLog * upperWeight + 0.5);
			}
		}
	}

	GroupMemoryBarrierWithGroupSync();
	if (groupIndex < GRID_DEPTH) {
		uint gridWeight = 0;
		uint gridLogSum = 0;
		[unroll] for (uint threadIndex = 0; threadIndex < GRID_THREAD_SIZE * GRID_THREAD_SIZE; threadIndex++)
		{
			gridWeight += ThreadGridWeights[groupIndex][threadIndex];
			gridLogSum += ThreadGridLogSums[groupIndex][threadIndex];
		}

		const float decodeScale = rcp((float)GRID_QUANTIZATION);
		RWTexLuminanceGrid[uint3(groupID.xy, groupIndex)] = float2(gridLogSum, gridWeight) * decodeScale;
	}
}

// ---- Application (upstream Composite, HAS_LOCAL_EXPOSURE) ---------------------------------

float RemapBaseContrast(float centeredBase)
{
	bool isHighlight = centeredBase >= 0.0;
	float magnitude = abs(centeredBase);
	float contrast = isHighlight ? HighlightContrast : ShadowContrast;
	float threshold = isHighlight ? HighlightThreshold : ShadowThreshold;
	float thresholdStrength = isHighlight ? HighlightThresholdStrength : ShadowThresholdStrength;

	// Preserve tones near middle grey, then release smoothly into the selected base contrast. At
	// full threshold strength the protected shoulder persists.
	float transitionWidth = rcp(max(1.0 - thresholdStrength, 1e-3));
	float release = smoothstep(threshold, threshold + transitionWidth, magnitude);
	float protectedOffset = min(magnitude, threshold) * (1.0 - release);
	float remappedMagnitude = magnitude * contrast + protectedOffset * (1.0 - contrast);

	if (isHighlight && magnitude > threshold)
		remappedMagnitude = max(remappedMagnitude, threshold);

	return isHighlight ? remappedMagnitude : -remappedMagnitude;
}

float ComputeLocalExposure(float sceneLogLuminance, float baseLogLuminance, float globalExposure, float middleGreyCompensation, float floorLift = 0.0)
{
	float logGlobalExposure = log2(max(globalExposure, 1e-5));
	float exposedLogLuminance = sceneLogLuminance + logGlobalExposure;
	float exposedBase = baseLogLuminance + logGlobalExposure;
	float detail = exposedLogLuminance - exposedBase;
	// (49j, design 5.2) Physical Brightness: the pivot is the display key this frame (0.18 x 2^dK = AEAdaptation[3]), read on the GPU.
	float middleGrey = ((Flags & 1u) != 0u ? log2(0.18 * max(AEAdaptation[3], 1e-8)) : log2(0.18 * middleGreyCompensation)) + MiddleGreyBias;
	float remappedBase = middleGrey + RemapBaseContrast(exposedBase - middleGrey);
	float targetLogLuminance = remappedBase + detail * DetailStrength;
	float logAdjustment = (targetLogLuminance - exposedLogLuminance) * Strength;
	// (49j, design 5.4) Minimum Visibility: no area's displayed base below FloorLog, only lifted, at most 4 EV (independent of Strength).
	if ((Flags & 2u) != 0u)
		logAdjustment += clamp(FloorLog - (exposedBase + (remappedBase - exposedBase) * Strength), 0.0, 4.0) * floorLift;
	return exp2(clamp(logAdjustment, -8.0, 8.0));
}

RWTexture2D<float4> RWTexColorOutput : register(u0);

[numthreads(8, 8, 1)] void CSApplyLocalExposure(uint2 tid : SV_DispatchThreadID) {
	if (tid.x >= InputWidth || tid.y >= InputHeight)
		return;

	float4 sceneColor = LoadSceneColor(tid);

	// Base layer (upstream CSResolveBaseLuminance)
	float logLuminance = TexLogLuminance.Load(int3(tid, 0));
	float normalizedLog = saturate((logLuminance - LogLuminanceMin) / (LogLuminanceMax - LogLuminanceMin));
	float2 uv = (float2(tid) + 0.5) / float2(InputWidth, InputHeight);

	uint gridWidth, gridHeight, gridDepth;
	TexLuminanceGrid.GetDimensions(gridWidth, gridHeight, gridDepth);
	float3 gridUV;
	gridUV.xy = (float2(tid) + 0.5) / (GRID_TILE_SIZE * float2(gridWidth, gridHeight));
	gridUV.z = (normalizedLog * (gridDepth - 1) + 0.5) / gridDepth;

	float2 gridMoments = TexLuminanceGrid.SampleLevel(LinearSampler, gridUV, 0);
	float broadBase = TexBlurredLuminance.SampleLevel(LinearSampler, uv, 0);
	float bilateralBase = broadBase;
	if (gridMoments.y >= 0.001)
		bilateralBase = lerp(LogLuminanceMin, LogLuminanceMax, gridMoments.x / gridMoments.y);

	float baseLogLuminance = lerp(bilateralBase, broadBase, BaseBlend);

	// (49j) Minimum Visibility leaves the sky out (a lifted night sky turns grey): the render-resolution depth at this UV, far plane = sky.
	float floorLift = 1.0;
	[branch] if ((Flags & 2u) != 0u && DepthUvScale.x > 0.0)
		floorLift = TexDepth.SampleLevel(LinearSampler, uv * DepthUvScale, 0) >= 0.99999 ? 0.0 : 1.0;

	// Local adjustment (upstream Composite), from the full-precision scene luminance as upstream does.
	float localExposure = ComputeLocalExposure(SceneLogLuminance(sceneColor.rgb), baseLogLuminance, GlobalExposure, MiddleGreyCompensation, floorLift);

	float3 color = sceneColor.rgb;
	// (49j, design 5.3) Night vision per pixel (replaces the global Purkinje): the physical luminance of the pixel and of its surroundings (the
	// base) from the exposed colour (L = exposed x L_a / (0.18 x 2^dK)); the rods' share by the mesopic range of CIE 191:2010 (0.005 - 5
	// cd/m^2) on the local adaptation luminance; the Cao (2008) blue shift fed the retinal illuminance (L x pupil area, Moon & Spencer 1944)
	// instead of a fixed x380; then a desaturation toward a slightly blue grey. Lamp-lit areas stay above the range: no change there.
	[branch] if ((Flags & 4u) != 0u)
	{
		const float toPhysical = max(AEAdaptation[1], 1e-12) / (0.18 * max(AEAdaptation[3], 1e-8));
		const float localLuminance = exp2(baseLogLuminance) * toPhysical;
		const float rods = 1.0 - smoothstep(log10(0.005), log10(5.0), log10(max(localLuminance, 1e-9)));
		const float rodMix = rods * NightVision;
		[branch] if (rodMix > 1e-3)
		{
			const float adapted = max(AEAdaptation[1], 1e-9);
			const float pupil = 4.9 - 3.0 * tanh(0.4 * log10(adapted));  // mm
			const float area = 3.14159265 * 0.25 * pupil * pupil;          // mm^2
			const float toTrolands = toPhysical * area / AutoExposureMath::logExposure;
			color = AutoExposureMath::PurkinjeShift(color * toTrolands, rodMix) / max(toTrolands, 1e-12);
			const float3 nightTint = float3(0.86, 0.96, 1.25);
			const float y = Color::RGBToLuminance(max(color, 0.0));
			color = lerp(color, y * nightTint / Color::RGBToLuminance(nightTint), rodMix);
		}
	}

	RWTexColorOutput[tid] = float4(max(color, 0.0) * localExposure, sceneColor.a);
}
