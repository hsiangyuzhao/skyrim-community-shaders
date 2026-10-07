// (batch 38c) Builds the tone curve the before-upscaling encode uses (see Common.hlsli).
//
// One group of kCurveSize threads. Thread i takes the exposed scene value x_i = 2^(min + i * step)
// through what the post-processing chain does to a neutral pixel of that value -- Color Grading's
// cinematic pre-steps, its baked LUT (which holds every HDR grading step and the tone mapper), its
// output gamma and the game tint -- and then the final gamma the image-space pass applies with
// Linear Lighting's gamma correction on. Saturation is a no-op on a neutral pixel and the game fade
// is left out on purpose: it is a transition, and a faded-out curve would be flat, not invertible.
//
// The result is made strictly increasing per channel (an exact inverse needs that), its cap found,
// and the exposure the chain will apply this frame stored next to it, so encode and decode can never
// use two different exposures.

#include "Upscaling/NeuralRendering/Common.hlsli"

Texture3D<float4> GradingLUT : register(t0);
StructuredBuffer<float> Adaptation : register(t1);  // Histogram Auto Exposure's adapted average luminance
SamplerState LinearClamp : register(s0);

RWStructuredBuffer<float4> Curve : register(u0);

cbuffer ToneCurveCB : register(b0)
{
	float4 Tint;  // rgb, amount
	float InputGamma;
	float OutputGamma;
	float CinematicBrightness;
	float CinematicContrast;
	float ExposureCompensation;  // linear multiplier (exp2 of the EV setting)
	float AdaptationMin;         // linear luminance, as Histogram Auto Exposure clamps it
	float AdaptationMax;
	float FixedExposure;  // used when there is no auto exposure
	uint UseGrading;
	uint UseAutoExposure;
	uint GammaCorrect;
	uint Pad0;
};

static const float3 kLuma = float3(0.2125, 0.7154, 0.0721);  // Color::RGBToLuminance

// Color Grading's LUT addressing (colorgrading.cs.hlsl, LogToLinear / LinearToLog).
float3 GradingLogToLinear(float3 logColor)
{
	return exp2((logColor - 444.0 / 1023.0) * 14.0) * 0.18;
}

float3 GradingLinearToLog(float3 linearColor)
{
	return saturate(log2(linearColor) / 14.0 - log2(0.18) / 14.0 + 444.0 / 1023.0);
}

float3 GradingLinearContrast(float3 col, float contrast, float pivot)
{
	col = col / pivot;
	const float3 sgn = sign(col);
	return pow(abs(col), contrast) * pivot * sgn;
}

groupshared float3 gCurve[kCurveSize];

[numthreads(256, 1, 1)] void main(uint index : SV_GroupIndex) {
	const float x = exp2(kCurveLog2Min + float(index) * kCurveStep);

	float3 color;
	[branch] if (UseGrading)
	{
		color = pow(x, InputGamma) * CinematicBrightness;
		color = GradingLinearContrast(color, CinematicContrast, 0.18);
		color = GradingLUT.SampleLevel(LinearClamp, GradingLinearToLog(color + GradingLogToLinear(0.0)), 0).rgb;
		color = pow(abs(color), OutputGamma);
		const float luma = dot(color, kLuma);
		color = lerp(color, luma * Tint.rgb, Tint.w);
	}
	else
	{
		color = x / (1.0 + x);
	}
	if (GammaCorrect)
		color = pow(abs(color), 1.0 / 2.2);

	gCurve[index] = saturate(color);
	GroupMemoryBarrierWithGroupSync();

	if (index == 0) {
		float3 previous = -1.0;
		[loop] for (uint i = 0; i < kCurveSize; ++i)
		{
			const float3 value = max(gCurve[i], previous + 1e-6);
			gCurve[i] = value;
			previous = value;
		}

		const float3 top = gCurve[kCurveSize - 1];
		float3 cap = float(kCurveSize - 1);
		[unroll] for (uint channel = 0; channel < 3; ++channel)
		{
			[loop] for (uint i = 1; i < kCurveSize; ++i)
			{
				if (gCurve[i][channel] >= top[channel] - 0.5 / 255.0) {
					cap[channel] = float(i);
					break;
				}
			}
		}

		float exposure = FixedExposure;
		if (UseAutoExposure)
			exposure = 0.18 * ExposureCompensation / clamp(Adaptation[0], AdaptationMin, AdaptationMax);
		if (!(exposure > 1e-6 && exposure < 1e6))
			exposure = 1.0;

		Curve[kCurveSize] = float4(cap, exposure);
	}
	GroupMemoryBarrierWithGroupSync();

	Curve[index] = float4(gCurve[index], 0.0);
}
