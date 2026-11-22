/// By ProfJack/五脚猫, 2024-2-17 UTC

#include "PostProcessing/HistogramAutoExposure/common.hlsli"

#include "PostProcessing/HistogramAutoExposure/exposure.hlsli"

RWTexture2D<float4> RWTexOut : register(u0);

Texture2D<float4> TexColor : register(t0);
StructuredBuffer<float> RWTexAdaptation : register(t1);

[numthreads(32, 32, 1)] void main(uint2 tid
								  : SV_DispatchThreadID) {
	// Early out for out-of-bounds threads
	uint2 dims;
	TexColor.GetDimensions(dims.x, dims.y);
	if (tid.x >= dims.x || tid.y >= dims.y)
		return;

#if defined(EYE_MODE)
	// (50a) Eye Model: the eye's divisor and adapted level (CS_Average's eye variant), mesopic or Classic's Purkinje.
	float3 color = AutoExposureMath::ExposeColorEye(TexColor[tid].rgb * SceneScaleInv, RWTexAdaptation[0], RWTexAdaptation[1], ExposureCompensation, PurkinjeStartEV, PurkinjeMaxEV,
		PurkinjeStrength, EyeMesopic);
#else
	// (44e) The math lives in exposure.hlsli, so Local Exposure can apply it on load instead.
	// (52a) Physical Lighting: x 1 / S, the frame's pre-exposure divided out (x 1.0 with the master off).
	float3 color = AutoExposureMath::ExposeColor(TexColor[tid].rgb * SceneScaleInv, RWTexAdaptation[0], ExposureCompensation, AdaptationRange, PurkinjeStartEV, PurkinjeMaxEV, PurkinjeStrength);
#endif

	RWTexOut[tid] = float4(color, 1);
}