#include "Common/Color.hlsli"
#include "Common/Math.hlsli"

#include "PostProcessing/common.hlsli"
#include "PostProcessing/aces.hlsli"
#include "PostProcessing/ColourTransforms/GT7ToneMapping.hlsli"

#define LUT_SIZE 64

RWTexture2D<float4> RWTexOut : register(u0);

Texture2D<float4> TexColor : register(t0);
Texture3D<float4> TexLUT : register(t1);

SamplerState LinearSampler : register(s0);

#if defined(BLOOM_FUSED)
// (44e) COD Bloom's composite folded in: the bloom (its mip 1, upsampled with the same four taps,
// sampler and radius) is added to the input here instead of in a separate full-resolution pass.
cbuffer FusedBloomCB : register(b2)
{
	float2 BloomPxSize;  // 1 / the bloom's full-resolution size
	float BloomRadius;   // Upsampling Radius
	float BloomMult;     // Blend Factor
};
Texture2D<float4> TexBloomMip1 : register(t2);
SamplerState BloomSampler : register(s1);

float3 LoadInput(uint2 id)
{
	const float2 uv = (id + .5) * BloomPxSize;
	const float2 radius = BloomPxSize * BloomRadius;
	float4 bloom = 0;
	[unroll] for (int x = -1; x <= 1; x += 2)
		[unroll] for (int y = -1; y <= 1; y += 2)
			bloom += 0.25 * TexBloomMip1.SampleLevel(BloomSampler, uv + float2(x, y) * 0.5 * radius, 0);
	return TexColor[id].rgb + bloom.rgb * BloomMult;
}
#else
float3 LoadInput(uint2 id)
{
	return TexColor[id].xyz;
}
#endif

cbuffer ColorCB : register(b1) {
    float4 asccdl[3];
    float4 liftgammagain[3]; // lift，gamma，gain
    float4 saturationHueInOutGamma;
    float4 oklchSaturation;
    float4 oklchColorMixer[7];
	float4 contrast;
	float4 pivot;
	float4 exposureTemperatureTint;
	float4 shadows;
	float4 midtones;
	float4 highlights;
	float4 shadowsHighlightsRange; // shadowBegin, shadowEnd, highlightBegin, highlightEnd

	float4 tonemapParams[2];
	float4 colorSpaceTransform[3];
    float4 invColorSpaceTransform[3];

    // game value
    float4 cinematic; // saturation, brightness, contrast
    float4 fade; // color
    float4 tint; // color

    uint logType;
    uint skipLDR;
    uint enableTonemap;
    uint enableColorSpaceTransform;

	// (batch 43, 44a) Native HDR output. x: 0 = SDR (unchanged), 1 = Extend the tone mapper,
	// 2 = the tone mapper's Native HDR form; y: peak / paper white; z: roll-off start (knee);
	// w: 1 when the chain's output is display gamma (Linear Lighting's gamma correction off).
	float4 hdrParams;
	// (44a) x: Native option (GT7: upstream reference brightness); y: 1 = output BT.2020 (HDR
	// working colour space; hdrOutputTransform replaces the inverse transform); z, w: unused.
	float4 hdrParams2;
	float4 hdrOutputTransform[3];  // (44a) processing space -> BT.2020, (709 -> 2020) x (processing -> 709)
};

#if defined(HDR_OUTPUT)
// (44a) BT.709 <-> BT.2020 (both D65).
static const float3x3 kHDR709To2020 = {
	0.6274039, 0.3292830, 0.0433131,
	0.0690973, 0.9195404, 0.0113623,
	0.0163914, 0.0880133, 0.8955953
};
static const float3x3 kHDR2020To709 = {
	1.6604910, -0.5876411, -0.0728499,
	-0.1245505, 1.1328999, -0.0083494,
	-0.0181508, -0.1005789, 1.1187297
};
static const float3 kHDRLuma2020 = float3(0.2627, 0.6780, 0.0593);

bool HDRNative()
{
	return hdrParams.x > 1.5;
}

bool HDROutputBT2020()
{
	return hdrParams2.y > 0.5;
}
#endif

namespace LogType {
	static const uint ACEScct = (1 << 0);
	static const uint ARRIlogC4 = (1 << 1);
	static const uint SonySLog3 = (1 << 2);
	static const uint Invert = (1 << 3);
};

// https://www.shadertoy.com/view/ss23DD
float3 LiftGammaGain(float3 rgb, float4 lift, float4 gamma, float4 gain)
{
	float4 liftt = 1.0 - pow(max(1.0 - lift, 0.0), log2(gain + 1.0));

	float4 gammat = gamma.rgba - float4(0.0, 0.0, 0.0, Color::RGBToLuminance(gamma.rgb));
	float4 gammatTemp = 1.0 + 4.0 * abs(gammat);
	gammat = lerp(gammatTemp, 1.0 / gammatTemp, step(0.0, gammat));

	float3 col = rgb;
	float luma = Color::RGBToLuminance(col);

	col = pow(max(col, 0.0), gammat.rgb);
	col *= pow(abs(gain.rgb), gammat.rgb);
	col = max(lerp(2.0 * liftt.rgb, 1.0, col), 0.0);

	luma = pow(abs(luma), gammat.a);
	luma *= pow(abs(gain.a), gammat.a);
	luma = max(lerp(2.0 * liftt.a, 1.0, luma), 0.0);

	col += luma - Color::RGBToLuminance(col);

	return col;
}

float3 OklchSaturation(float3 val)
{
	float3 oklab = RgbToOklab(val);

	float c = length(oklab.yz);
	float h = atan2(oklab.z, oklab.y);

	c = min(0.37, c * oklchSaturation.x);
	c = (1 - pow(abs(1 - c / 0.37), oklchSaturation.y)) * 0.37;
	h += oklchSaturation.z * Math::PI;

	sincos(h, oklab.z, oklab.y);
	oklab.yz *= c;

	return max(0, OklabToRgb(oklab));
}

// mimicking lightroom colour mixer
float3 OklchColourMixer(float3 val)
{
	static const float redHue = 0.08120523664;  //0xff0000

	float3 oklab = RgbToOklab(val);

	float l = oklab.x;
	float c = length(oklab.yz);
	float h = atan2(oklab.z, oklab.y);

	float lerpFactor = (h / (2 * Math::PI) - redHue) * 7;
	int leftHue = floor(lerpFactor);
	lerpFactor = lerpFactor - leftHue;
	leftHue += (leftHue < 0) * 7;
	int rightHue = (leftHue + 1) % 7;
	float effect = saturate(c / 0.37);

	// hue shift
	h = h + lerp(oklchColorMixer[leftHue].x, oklchColorMixer[rightHue].x, lerpFactor) * Math::PI / 4;
	// vibrance
	float c1 = (1 - pow(max(0, 1 - c / 0.37), oklchColorMixer[leftHue].y)) * 0.37;
	float c2 = (1 - pow(max(0, 1 - c / 0.37), oklchColorMixer[rightHue].y)) * 0.37;
	c = lerp(c1, c2, lerpFactor);
	// brightness
	l = l + lerp(oklchColorMixer[leftHue].z, oklchColorMixer[rightHue].z, lerpFactor) * effect;

	oklab.x = l;
	sincos(h, oklab.z, oklab.y);
	oklab.yz *= c;

	return max(0, OklabToRgb(oklab));
}

float3 ShadowsMidtonesHighlights(float3 color, float3 shadowsColor, float3 midtonesColor, float3 highlightsColor, float shadowBegin, float shadowEnd, float highlightBegin, float highlightEnd)
{
    float luma = Color::RGBToLuminance(color);
    
    float shadowWeight = 1.0 - smoothstep(shadowBegin, shadowEnd, luma);
    
    float highlightWeight = smoothstep(highlightBegin, highlightEnd, luma);
    
    float midtoneWeight = 1.0 - shadowWeight - highlightWeight;

    float3 result = color * shadowsColor * shadowWeight
                  +	color * midtonesColor * midtoneWeight
                  +	color * highlightsColor * highlightWeight;

    return result;
}

float2 IlluminantChromaticity(float temp)
{
	temp *= 1.4388 / 1.438;
	float x = temp <= 7000 ? 0.244063 + (0.09911e3 + (2.9678e6 - 4.6070e9 / temp) / temp) / temp : 0.237040 + (0.24748e3 + (1.9018e6 - 2.0064e9 / temp) / temp) / temp;
	float y = -3 * x * x + 2.87 * x - 0.275;
	return float2(x, y);
}

// Accurate for 1000K < temp < 15000K
// [Krystek 1985, "An algorithm to calculate correlated colour temperature"]
float2 PlanckianLocusChromaticity(float temp)
{
	float u = (0.860117757f + 1.54118254e-4f * temp + 1.28641212e-7f * temp * temp) / (1.0f + 8.42420235e-4f * temp + 7.08145163e-7f * temp * temp);
	float v = (0.317398726f + 4.22806245e-5f * temp + 4.20481691e-8f * temp * temp) / (1.0f - 2.89741816e-5f * temp + 1.61456053e-7f * temp * temp);
	float x = 3 * u / (2 * u - 8 * v + 4);
	float y = 2 * v / (2 * u - 8 * v + 4);
	return float2(x, y);
}

// [McCamy 1992, "Correlated color temperature as an explicit function of chromaticity coordinates"]
float2 PlanckianIsothermal(float temp, float tint)
{
	float u = (0.860117757f + 1.54118254e-4f * temp + 1.28641212e-7f * temp * temp) / (1.0f + 8.42420235e-4f * temp + 7.08145163e-7f * temp * temp);
	float v = (0.317398726f + 4.22806245e-5f * temp + 4.20481691e-8f * temp * temp) / (1.0f - 2.89741816e-5f * temp + 1.61456053e-7f * temp * temp);
	float ud = (-1.13758118e9f - 1.91615621e6f * temp - 1.53177f * temp * temp) / pow(1.41213984e6f + 1189.62f * temp + temp * temp, 2);
	float vd = (1.97471536e9f - 705674.0f * temp - 308.607f * temp * temp) / pow(6.19363586e6f - 179.456f * temp + temp * temp, 2);
	float2 uvd = normalize(float2(u, v));
	u += -uvd.y * tint * 0.05;
	v +=  uvd.x * tint * 0.05;
	float x = 3 * u / (2 * u - 8 * v + 4);
	float y = 2 * v / (2 * u - 8 * v + 4);
	return float2(x, y);
}

float3 WhiteBalance(float3 linearColor)
{
	if (abs(exposureTemperatureTint.y - 65) < 0.1 && abs(exposureTemperatureTint.z) < 0.001)
		return linearColor;

	float temp = exposureTemperatureTint.y * 100;
	float tint = exposureTemperatureTint.z;
	float2 srcWhiteDaylight = IlluminantChromaticity(temp);
	float2 srcWhitePlankian = PlanckianLocusChromaticity(temp);

	float2 srcWhite = exposureTemperatureTint.y < 4000 ? srcWhitePlankian : srcWhiteDaylight;
	float2 d65White = float2(0.31270, 0.32900);

	float2 isothermal = PlanckianIsothermal(temp, tint) - srcWhitePlankian;
	srcWhite += isothermal;

	float3x3 whiteBalance = ChromaticAdaptation(srcWhite, d65White);
	whiteBalance = mul(XYZ_2_sRGB_MAT, mul(whiteBalance, sRGB_2_XYZ_MAT));

	return mul(whiteBalance, linearColor);
}

float3 LogToLinear(float3 logColor)
{
    const float linearRange = 14.0f;
    const float linearGrey = 0.18f;
    const float exposureGrey = 444.0f;
    return exp2((logColor - exposureGrey / 1023.0) * linearRange) * linearGrey;
}

float3 LinearToLog(float3 linearColor)
{
    const float linearRange = 14.0f;
    const float linearGrey = 0.18f;
    const float exposureGrey = 444.0f;
    return saturate(log2(linearColor) / linearRange - log2(linearGrey) / linearRange + exposureGrey / 1023.0f);
}

/////////////////////////////////////////////////////////////////////////////////

/*
    tizian/tonemapper
        url:    https://github.com/tizian/tonemapper
        license:
            The MIT License (MIT)

            Copyright (c) 2022 Tizian Zeltner

            Permission is hereby granted, free of charge, to any person obtaining a copy
            of this software and associated documentation files (the "Software"), to deal
            in the Software without restriction, including without limitation the rights
            to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
            copies of the Software, and to permit persons to whom the Software is
            furnished to do so, subject to the following conditions:

            The above copyright notice and this permission notice shall be included in all
            copies or substantial portions of the Software.

            THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
            IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
            FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
            AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
            LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
            OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
            SOFTWARE.
*/

float3 Reinhard(float3 val)
{
	val *= tonemapParams[0].x;
	float luma = Color::RGBToLuminance(val);
	float lumaOut = luma / (1 + luma);
	val = val / (luma + 1e-10) * lumaOut;
	val = saturate(val);
	return val;
}

float3 ReinhardExt(float3 val)
{
	val *= tonemapParams[0].x;
	float luma = Color::RGBToLuminance(val);
	float lumaOut = luma * (1 + luma / (tonemapParams[0].y * tonemapParams[0].y)) / (1 + luma);
	val = val / (luma + 1e-10) * lumaOut;
	val = saturate(val);
	return val;
}

float3 HejlBurgessDawsonFilmic(float3 val)
{
	val *= tonemapParams[0].x;
	val = max(0, val - 0.004);
	val = (val * (6.2 * val + .5)) / (val * (6.2 * val + 1.7) + 0.06);
	val = pow(saturate(val), 2.2);
	return val;
}

float3 AldridgeFilmic(float3 val)
{
	val *= tonemapParams[0].x;
	float tmp = 2.0 * tonemapParams[0].y;
	val = val + (tmp - val) * clamp(tmp - val, 0.0, 1.0) * (0.25 / tonemapParams[0].y) - tonemapParams[0].y;
	val = (val * (6.2 * val + 0.5)) / (val * (6.2 * val + 1.7) + 0.06);
	val = pow(saturate(val), 2.2);
	return val;
}

float3 AcesHill(float3 val)
{
	static const float3x3 g_sRGBToACEScg = float3x3(
		0.613117812906440, 0.341181995855625, 0.045787344282337,
		0.069934082307513, 0.918103037508582, 0.011932775530201,
		0.020462992637737, 0.106768663382511, 0.872715910619442);
	static const float3x3 g_ACEScgToSRGB = float3x3(
		1.704887331049502, -0.624157274479025, -0.080886773895704,
		-0.129520935348888, 1.138399326040076, -0.008779241755018,
		-0.024127059936902, -0.124620612286390, 1.148822109913262);

	val *= tonemapParams[0].x;

	val = mul(g_sRGBToACEScg, val);
	float3 a = val * (val + 0.0245786f) - 0.000090537f;
	float3 b = val * (0.983729f * val + 0.4329510f) + 0.238081f;
	val = a / b;
	val = mul(g_ACEScgToSRGB, val);

	val = saturate(val);

	return val;
}

float3 AcesNarkowicz(float3 val)
{
	val *= tonemapParams[0].x;

	static const float A = 2.51;
	static const float B = 0.03;
	static const float C = 2.43;
	static const float D = 0.59;
	static const float E = 0.14;
	val *= 0.6;
	val = (val * (A * val + B)) / (val * (C * val + D) + E);
	val = saturate(val);
	return val;
}

float3 AcesGuy(float3 val)
{
	val *= tonemapParams[0].x;
	val = val / (val + 0.155f) * 1.019;

	val = pow(saturate(val), 2.2);
	return val;
}

float3 LottesFilmic(float3 val)
{
	val *= tonemapParams[0].x;
#if defined(HDR_OUTPUT)
	// (44a) Native HDR form (upstream's): b and c re-solved so f(maxHDR) = peak / paper white while
	// f(midIn) = midOut stays; then clamped to the peak.
	[branch] if (HDRNative())
	{
		const float na = tonemapParams[0].y, nd = tonemapParams[0].z;
		const float maxHDR = tonemapParams[0].w, midIn = tonemapParams[1].x, midOut = tonemapParams[1].y;
		const float peakOutput = hdrParams.y;
		const float denom = (pow(maxHDR, na * nd) - pow(midIn, na * nd)) * midOut * peakOutput;
		const float nb = (pow(maxHDR, na) * midOut - pow(midIn, na) * peakOutput) / denom;
		const float nc = (pow(maxHDR, na * nd) * pow(midIn, na) * peakOutput - pow(maxHDR, na) * pow(midIn, na * nd) * midOut) / denom;
		val = clamp(pow(max(val, 0.0), na) / (pow(max(val, 0.0), na * nd) * nb + nc), 0.0, peakOutput);
	} else {
#endif
	float a = tonemapParams[0].y,
		  d = tonemapParams[0].z,
		  b = (-pow(tonemapParams[1].x, a) + pow(tonemapParams[0].w, a) * tonemapParams[1].y) /
	          ((pow(tonemapParams[0].w, a * d) - pow(tonemapParams[1].x, a * d)) * tonemapParams[1].y),
		  c = (pow(tonemapParams[0].w, a * d) * pow(tonemapParams[1].x, a) - pow(tonemapParams[0].w, a) * pow(tonemapParams[1].x, a * d) * tonemapParams[1].y) /
	          ((pow(tonemapParams[0].w, a * d) - pow(tonemapParams[1].x, a * d)) * tonemapParams[1].y);

	val = pow(val, a) / (pow(val, a * d) * b + c);
	val = saturate(val);
#if defined(HDR_OUTPUT)
	}
#endif
	return val;
}

float DayCurve(float x, float k)
{
	const float b = tonemapParams[0].y;
	const float w = tonemapParams[0].z;
	const float c = tonemapParams[0].w;
	const float s = tonemapParams[1].x;
	const float t = tonemapParams[1].y;

	if (x < c) {
		return k * (1.0 - t) * (x - b) / (c - (1.0 - t) * b - t * x);
	} else {
		return (1.0 - k) * (x - c) / (s * x + (1.0 - s) * w - c) + k;
	}
}

float3 DayFilmic(float3 val)
{
	const float b = tonemapParams[0].y;
	const float w = tonemapParams[0].z;
	const float c = tonemapParams[0].w;
	const float s = tonemapParams[1].x;
	const float t = tonemapParams[1].y;

	val *= tonemapParams[0].x;
	float k = (1.0 - t) * (c - b) / ((1.0 - s) * (w - c) + (1.0 - t) * (c - b));
	val = float3(DayCurve(val.r, k), DayCurve(val.g, k), DayCurve(val.b, k));

	val = saturate(val);
	return val;
}

float3 UchimuraFilmic(float3 val)
{
	const float P = tonemapParams[0].y;
	const float a = tonemapParams[0].z;
	const float m = tonemapParams[0].w;
	const float l = tonemapParams[1].x;
	const float c = tonemapParams[1].y;
	const float b = tonemapParams[1].z;

	val *= tonemapParams[0].x;

	float l0 = ((P - m) * l) / a,
		  S0 = m + l0,
		  S1 = m + a * l0,
		  C2 = (a * P) / (P - S1),
		  CP = -C2 / P;

	float3 w0 = 1.0 - smoothstep(0.0, m, val),
		   w2 = step(m + l0, val),
		   w1 = 1.0 - w0 - w2;

	float3 T = m * pow(val / m, c) + b,           // toe
		L = m + a * (val - m),                    // linear
		S = P - (P - S1) * exp(CP * (val - S0));  // shoulder

	val = T * w0 + L * w1 + S * w2;

	val = saturate(val);
	return val;
}

/*  AgX Reference:
 *  AgX by longbool https://www.shadertoy.com/view/dtSGD1
 *  AgX Minimal by bwrensch https://www.shadertoy.com/view/cd3XWr
 *  Fork AgX Minima troy_s 342 by troy_s https://www.shadertoy.com/view/mdcSDH
 */

// Mean error^2: 3.6705141e-06
float3 AgxDefaultContrastApprox5(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;

	return +15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4 - 6.868 * x2 * x +
	       0.4298 * x2 + 0.1191 * x - 0.00232;
}

// Mean error^2: 1.85907662e-06
float3 AgxDefaultContrastApprox6(float3 x)
{
	float3 x2 = x * x;
	float3 x4 = x2 * x2;

	return -17.86 * x4 * x2 * x + 78.01 * x4 * x2 - 126.7 * x4 * x + 92.06 * x4 -
	       28.72 * x2 * x + 4.361 * x2 - 0.1718 * x + 0.002857;
}

float3 Agx(float3 val)
{
	const float3x3 agx_mat = transpose(
		float3x3(0.842479062253094, 0.0423282422610123, 0.0423756549057051,
			0.0784335999999992, 0.878468636469772, 0.0784336,
			0.0792237451477643, 0.0791661274605434, 0.879142973793104));

	const float min_ev = -12.47393f;
	const float max_ev = 4.026069f;

	// Input transform
	val = mul(agx_mat, val);

	// Log2 space encoding
	val = clamp(log2(val), min_ev, max_ev);
	val = (val - min_ev) / (max_ev - min_ev);

	// Apply sigmoid function approximation
	val = AgxDefaultContrastApprox6(val);

	return val;
}

float3 AgxEotf(float3 val)
{
	const float3x3 agx_mat_inv = transpose(
		float3x3(1.19687900512017, -0.0528968517574562, -0.0529716355144438,
			-0.0980208811401368, 1.15190312990417, -0.0980434501171241,
			-0.0990297440797205, -0.0989611768448433, 1.15107367264116));

	// Undo input transform
	val = mul(agx_mat_inv, val);

	// sRGB IEC 61966-2-1 2.2 Exponent Reference EOTF Display
	// NOTE: We're linearizing the output here. Comment/adjust when
	// *not* using a sRGB render target
	val = pow(saturate(val), 2.2);

	return val;
}

float3 AgxMinimal(float3 val)
{
	val *= tonemapParams[0].x;

	val = Agx(val);
	val = ASC_CDL(val, tonemapParams[0].y, tonemapParams[0].z, tonemapParams[0].w);
	val = Saturation(val, tonemapParams[1].x);
	val = AgxEotf(val);

	return val;
}

// src: https://github.com/ltmx/Melon-Tonemapper
// GPL-3.0 license
float3 MelonHueShift(float3 In)
{
	float A = max(In.x, In.y);
	return float3(A, max(A, In.z), In.z);
}

float3 MelonTonemap(float3 color)
{
	color *= tonemapParams[0].r;

	// remaps the colors to [0-1] range
	// tested to be as close ti ACES contrast levels as possible
	color = pow(color, float3(1.56, 1.56, 1.56));
	color = color / (color + 0.84);

	// governs the transition to white for high color intensities
	float factor = max(color.r, max(color.g, color.b)) * 0.15;  // multiply by 0.15 to get a similar look to ACES
	factor = factor / (factor + 1);                             // remaps the factor to [0-1] range
	factor *= factor;                                           // smooths the transition to white

	// shift the hue for high intensities (for a more pleasing look).
	color = lerp(color, MelonHueShift(color), factor);   // can be removed for more neutral colors
	color = lerp(color, float3(1.0, 1.0, 1.0), factor);  // shift to white for high intensities

	// clamp to [0-1] range
	return clamp(color, float3(0.0, 0.0, 0.0), float3(1.0, 1.0, 1.0));
}

/* 
    EmbarkStudios/kajiya
        url:    https://github.com/EmbarkStudios/kajiya	
        license:
			Copyright (c) 2019 Embark Studios

			Permission is hereby granted, free of charge, to any
			person obtaining a copy of this software and associated
			documentation files (the "Software"), to deal in the
			Software without restriction, including without
			limitation the rights to use, copy, modify, merge,
			publish, distribute, sublicense, and/or sell copies of
			the Software, and to permit persons to whom the Software
			is furnished to do so, subject to the following
			conditions:

			The above copyright notice and this permission notice
			shall be included in all copies or substantial portions
			of the Software.

			THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
			ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
			TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
			PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
			SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
			CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
			OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
			IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
			DEALINGS IN THE SOFTWARE.
*/

float KajiyaCurve(float v)
{
	return 1.0 - exp(-v);
}

float3 KajiyaCurve(float3 v)
{
	return 1.0 - exp(-v);
}

float3 KajiyaTonemap(float3 col)
{
	col *= tonemapParams[0].r;

	float3 ycbcr = RgbToYCbCr(col);

	float bt = KajiyaCurve(length(ycbcr.yz) * 2.4);
	float desat = max((bt - 0.7) * 0.8, 0);
	desat = desat * desat;

	float3 desat_col = lerp(col, ycbcr.x, desat);

	float tm_luma = KajiyaCurve(ycbcr.x);
	float3 tm0 = col * max(tm_luma / max(Color::RGBToLuminance(col), 1e-5), 0);
	float final_mult = 0.97;
	float3 tm1 = KajiyaCurve(desat_col);

	return lerp(tm0, tm1, bt * bt) * final_mult;
}

float3 GT7ToneMapping(float3 color)
{
    color *= tonemapParams[0].x;
	if (tonemapParams[0].y == 0)
		color = GT7ToneMappingSDR(color);
	else
		color = GT7ToneMappingHDR(color, tonemapParams[0].z);
    return color;
}

////////////////////////////////////////////////////////////////////////
// (44a) Neutwo, by Carlos Lopez (RenoDX), ported from upstream's renodx/tonemap/neutwo.hlsl.
//
//   Copyright (C) 2026 Carlos Lopez. SPDX-License-Identifier: MIT
//   MIT License, Copyright (c) 2025 Carlos Lopez Jr. Permission is hereby granted, free of charge,
//   to any person obtaining a copy of this software and associated documentation files (the
//   "Software"), to deal in the Software without restriction, including without limitation the
//   rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the
//   Software, and to permit persons to whom the Software is furnished to do so, subject to the
//   following conditions: The above copyright notice and this permission notice shall be included
//   in all copies or substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
//   WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// A neutral curve on luminance only, y' = c p y / sqrt(y^2 (c^2 - p^2) + c^2 p^2) (p = peak, c = clip
// point): hue and saturation are kept. SDR: p = 1; Native HDR: p = peak / paper white.
float NeutwoCurve(float x, float peak, float clip)
{
	const float cc = clip * clip;
	const float pp = peak * peak;
	return clip * peak * x * rsqrt(max(x * x * (cc - pp) + cc * pp, 1e-12));
}

float3 NeutwoTonemap(float3 val)
{
	val = max(val * tonemapParams[0].x, 0.0);
	float peak = 1.0;
#if defined(HDR_OUTPUT)
	if (HDRNative())
		peak = hdrParams.y;
#endif
	const float clip = max(tonemapParams[0].y, peak);
	const float y = Color::RGBToLuminance(val);
	val *= y > 0.0 ? NeutwoCurve(y, peak, clip) / y : 1.0;
	// Saturated colours can put a channel above the peak at the same luminance: scale the whole
	// colour down (keeps the hue) rather than clipping a channel.
	const float m = max(max(val.r, val.g), val.b);
	return m > peak ? val * (peak / m) : val;
}

////////////////////////////////////////////////////////////////////////
// (batch 43) Native HDR output: highlight extension.
//
// The LUT keeps the SDR result in rgb, exactly as before, and the HDR brightness factor for that
// texel in alpha (1 in SDR). CSColorGrading multiplies by it only while HDR output is on, so the SDR
// picture, and everything else that reads this LUT (DLSS 5's tone-matched curve), is unchanged.
//
// Mode 1, extend the selected tone mapper: below the knee the picture is the SDR one. The knee is
// where the tone mapper's neutral curve reaches hdrParams.z of SDR white. Above it the brightest
// channel follows an exponential shoulder that leaves the knee with the tone mapper's own slope and
// approaches the peak (hdrParams.y, in paper whites) instead of SDR white.
//
// Mode 2, Native (44a): the tone mapper's own HDR form. Lottes and Neutwo compute it in their
// functions (rgb is HDR, alpha 1). GT7 (TONEMAP_GT7): Gran Turismo 7's curve with its target at the
// peak and its linear section ending at hdrParams.z of the peak; the SDR result is GT7's SDR curve,
// the same scale-the-SDR-colour approach, so mid-tones are GT7 SDR's exactly.
//
// All of it is compiled only into the HDR_OUTPUT variants, which Color Grading uses only while HDR
// output is on: the SDR variants compile to the same code as before batch 43.
#if defined(HDR_OUTPUT)

float HDRMax3(float3 v)
{
	return max(max(v.r, v.g), v.b);
}

float HDRToDisplayLinear(float v)
{
	return hdrParams.w > 0.5 ? pow(max(v, 0.0), 2.2) : max(v, 0.0);
}

float HDRGreyCurve(float s)
{
	return HDRToDisplayLinear(HDRMax3(TONEMAP_FUNC(s.xxx)));
}

struct HDRKnee
{
	float x;
	float y;
	float slope;
	float valid;
};

HDRKnee HDRNoKnee()
{
	HDRKnee k;
	k.x = 0.0;
	k.y = 0.0;
	k.slope = 0.0;
	k.valid = 0.0;
	return k;
}

HDRKnee HDRFindKnee()
{
	HDRKnee k = HDRNoKnee();
	k.y = hdrParams.z;
	if (k.y <= 0.0 || hdrParams.y <= k.y)
		return k;

	// Bisection on log2 of the tone mapper's input for the point where it reaches the knee.
	float lo = -16.0;
	float hi = 16.0;
	if (HDRGreyCurve(exp2(hi)) < k.y || HDRGreyCurve(exp2(lo)) >= k.y)
		return k;
	[loop] for (int i = 0; i < 24; ++i)
	{
		const float mid = 0.5 * (lo + hi);
		if (HDRGreyCurve(exp2(mid)) < k.y)
			lo = mid;
		else
			hi = mid;
	}
	k.x = exp2(hi);
	const float a = k.x * 0.97;
	const float b = k.x * 1.03;
	k.slope = max((HDRGreyCurve(b) - HDRGreyCurve(a)) / (b - a), 1e-4);
	k.valid = 1.0;
	return k;
}

float HDRExtendScale(float3 preTonemap, HDRKnee knee)
{
	if (knee.valid < 0.5)
		return 1.0;
	const float m = HDRMax3(preTonemap);
	if (m <= knee.x)
		return 1.0;
	const float room = hdrParams.y - knee.y;
	const float h = knee.y + room * (1.0 - exp(-knee.slope * (m - knee.x) / room));
	return max(h / max(HDRGreyCurve(m), 1e-4), 1.0);
}

float HDRGT7Scale(float3 preTonemap, out float3 sdr)
{
	sdr = GT7ToneMappingSDR(preTonemap);
	const float target = physicalValueToFrameBufferValue(GRAN_TURISMO_SDR_PAPER_WHITE) * hdrParams.y;
	const float3 hdr = GT7ToneMappingHDRRolloff(preTonemap, target, clamp(hdrParams.z, 0.3, 0.9)) /
	                   physicalValueToFrameBufferValue(GRAN_TURISMO_SDR_PAPER_WHITE);
	const float s = HDRToDisplayLinear(HDRMax3(sdr));
	const float h = HDRToDisplayLinear(HDRMax3(hdr));
	return max(h / max(s, 1e-4), 1.0);
}
#endif  // HDR_OUTPUT

////////////////////////////////////////////////////////////////////////

#if defined(HDR_OUTPUT)
// (44a) Luma-Framework (Filippo Tarpini), as upstream: basic gamut mapping. Desaturates toward the
// colour's own luminance until no channel is negative. Only in the HDR variants (BT.2020 output).
float3 CorrectOutOfRangeColor(float3 color, float3 luminanceVec)
{
	const float minChannel = min(color.r, min(color.g, color.b));
	if (minChannel < 0.0) {
		const float colorLuminance = dot(color, luminanceVec);
		if (colorLuminance >= 0.0) {
			const float desaturateAlpha = (minChannel - colorLuminance) != 0.0 ? (minChannel / (minChannel - colorLuminance)) : 0.0;
			color = lerp(color, colorLuminance, desaturateAlpha);
		}
	}
	return color;
}

// (44a) The post-curve ("LDR") adjustments with a BT.2020 output. They are SDR tools built on BT.709
// and Oklab (whose chroma limit 0.37 assumes SDR values), so they run on the BT.709, non-negative,
// at-most-1 version of the colour and only their change is added back: with neutral settings the
// colour passes through untouched, and colours outside BT.709 keep what BT.709 cannot hold. Lift /
// Gamma / Gain is skipped in Native mode (it is a [0, 1] grading tool).
float3 HDRPostCurve(float3 color2020)
{
	const float3 c709 = max(mul(kHDR2020To709, color2020), 0.0);
	const float n = max(1.0, max(max(c709.r, c709.g), c709.b));
	const float3 base = c709 / n;
	float3 g = base;
	[branch] if (!HDRNative())
		g = LiftGammaGain(g, liftgammagain[0].gbar, liftgammagain[1].gbar, liftgammagain[2].gbar);
	g = OklchSaturation(g);
	g = OklchColourMixer(g);
	return color2020 + mul(kHDR709To2020, (g - base) * n);
}
#endif

// Linear to Log
float3 ACEScct(float3 linearColor, bool inverse)
{
	const float a = 10.5402377416545;
	const float b = 0.0729055341958355;
	const float cutoff = 0.0078125;
	const float cutoff2 = 0.155251141552511;

	float3 cct = linearColor;

	if (!inverse) {
		cct = float3(
			linearColor.x > cutoff ? (log2(linearColor.x) + 9.72) / 17.52 : linearColor.x * a + b,
			linearColor.y > cutoff ? (log2(linearColor.y) + 9.72) / 17.52 : linearColor.y * a + b,
			linearColor.z > cutoff ? (log2(linearColor.z) + 9.72) / 17.52 : linearColor.z * a + b
		);
	} else {
		cct = float3(
			linearColor.x >= cutoff2 ? pow(2, linearColor.x * 17.52 - 9.72) : (linearColor.x - b) / a,
			linearColor.y >= cutoff2 ? pow(2, linearColor.y * 17.52 - 9.72) : (linearColor.y - b) / a,
			linearColor.z >= cutoff2 ? pow(2, linearColor.z * 17.52 - 9.72) : (linearColor.z - b) / a
		);
	}

	return cct;
}

float3 ARRIlogC4(float3 linearColor, bool inverse)
{
	const float a = 2231.8263;
	const float b = 0.9071359;
	const float c = 0.0928641;
	const float s = 0.6816768;
	const float t = -0.0180570;

	float3 logColor = linearColor;

	if (!inverse) {
		logColor = float3(
			(linearColor.x > t ? (log2(linearColor.x * a + 64) - 6) / 14 * b + c : (linearColor.x - t) / s),
			(linearColor.y > t ? (log2(linearColor.y * a + 64) - 6) / 14 * b + c : (linearColor.y - t) / s),
			(linearColor.z > t ? (log2(linearColor.z * a + 64) - 6) / 14 * b + c : (linearColor.z - t) / s)
		);
	} else {
		logColor = float3(
			(linearColor.x >= 0 ? (pow(2, 14 * (linearColor.x - c) / b + 6) - 64) / a : linearColor.x * s + t),
			(linearColor.y >= 0 ? (pow(2, 14 * (linearColor.y - c) / b + 6) - 64) / a : linearColor.y * s + t),
			(linearColor.z >= 0 ? (pow(2, 14 * (linearColor.z - c) / b + 6) - 64) / a : linearColor.z * s + t)
		);
	}

	return logColor;
}

float3 SonySLog3(float3 linearColor, bool inverse)
{
	float3 logColor = linearColor;

	if (!inverse) {
		logColor = float3(
			(linearColor.x >= 0.0112500 ? (420.0 + log10((linearColor.x + 0.01) / 0.19) * 261.5) / 1023.0 : (linearColor.x * (171.2102946929 - 95.0) / 0.0112500 + 95.0) / 1023.0),
			(linearColor.y >= 0.0112500 ? (420.0 + log10((linearColor.y + 0.01) / 0.19) * 261.5) / 1023.0 : (linearColor.y * (171.2102946929 - 95.0) / 0.0112500 + 95.0) / 1023.0),
			(linearColor.z >= 0.0112500 ? (420.0 + log10((linearColor.z + 0.01) / 0.19) * 261.5) / 1023.0 : (linearColor.z * (171.2102946929 - 95.0) / 0.0112500 + 95.0) / 1023.0)
		);
	} else {
		logColor = float3(
			(linearColor.x > 0.1712102946929 / 1023.0 ? pow(10, (linearColor.x * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01 : (linearColor.x * 1023.0 - 95.0) * 0.0112500 / (171.2102946929 - 95.0)),
			(linearColor.y > 0.1712102946929 / 1023.0 ? pow(10, (linearColor.y * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01 : (linearColor.y * 1023.0 - 95.0) * 0.0112500 / (171.2102946929 - 95.0)),
			(linearColor.z > 0.1712102946929 / 1023.0 ? pow(10, (linearColor.z * 1023.0 - 420.0) / 261.5) * 0.19 - 0.01 : (linearColor.z * 1023.0 - 95.0) * 0.0112500 / (171.2102946929 - 95.0))
		);
	}

	return logColor;
}

float3 LinearToLogSpace(float3 val, uint logType)
{
	float3 logColor = val;
	if (logType & LogType::ACEScct)
		logColor = ACEScct(val, false);
	else if (logType & LogType::ARRIlogC4)
		logColor = ARRIlogC4(val, false);
	else if (logType & LogType::SonySLog3)
		logColor = SonySLog3(val, false);
	return logColor;
}

float3 LogToLinearSpace(float3 val, uint logType)
{
    float3 linearColor = val;
    if (logType & LogType::ACEScct)
        linearColor = ACEScct(val, true);
    else if (logType & LogType::ARRIlogC4)
        linearColor = ARRIlogC4(val, true);
    else if (logType & LogType::SonySLog3)
        linearColor = SonySLog3(val, true);
    return linearColor;
}

////////////////////////////////////////////////////////////////////////

#if defined(HDR_OUTPUT)
float3 ColorGrading(float3 color, HDRKnee hdrKnee, out float hdrScale)
{
	hdrScale = 1.0;
#else
float3 ColorGrading(float3 color)
{
#endif

	// Color space transform (preferably sRGB to ACEScg)
    if (enableColorSpaceTransform) {
		const float3x3 colorSpaceTransformMat = float3x3(colorSpaceTransform[0].xyz, colorSpaceTransform[1].xyz, colorSpaceTransform[2].xyz);
        color = mul(colorSpaceTransformMat, color);
    }

	// HDR
	// Exposure/White Balance
	color *= exposureTemperatureTint.x;
	color = WhiteBalance(color);

    // Log
	if (logType)
    	color = LinearToLogSpace(color, logType); // Preferably ACEScct

    // ASC CDL
    color = ASC_CDL(color, asccdl[0].xyz, asccdl[1].xyz, asccdl[2].xyz);

	// Saturation and Hue
	color = Saturation(color, saturationHueInOutGamma.x);
	color = HueShift(color, saturationHueInOutGamma.y);

	// Shadows Midtones Highlights
	color = ShadowsMidtonesHighlights(color, shadows.xyz, midtones.xyz, highlights.xyz, shadowsHighlightsRange.x, shadowsHighlightsRange.y, shadowsHighlightsRange.z, shadowsHighlightsRange.w);

	// Contrast
	color = logType ? LogContrast(color, contrast.xyz, pivot.xyz) : LinearContrast(color, contrast.xyz, pivot.xyz);

    if (logType & LogType::Invert) {
        color = LogToLinearSpace(color, logType);
    }

    // Tonemap
    if (enableTonemap) {
#if defined(HDR_OUTPUT)
		[branch] if (HDRNative()) {
#	if defined(TONEMAP_GT7)
			// GT7 Native. Default: GT7's SDR curve here, its HDR brightness (target at the peak, linear
			// section to the knee) in hdrScale, so mid-tones are GT7 SDR's exactly. "Upstream reference
			// brightness": upstream's HDR curve itself (no /2.5, about 1.3 stops brighter mid-tones).
			const float3 exposed = color * tonemapParams[0].x;
			[branch] if (hdrParams2.x > 0.5)
				color = GT7ToneMappingHDRRolloff(exposed, hdrParams.y, clamp(hdrParams.z, 0.3, 0.9));
			else
				hdrScale = HDRGT7Scale(exposed, color);
#	else
			// Lottes, Neutwo: the tone mapper's own HDR form (see the function).
			color = TONEMAP_FUNC(color);
#	endif
		} else {
			const float3 preTonemap = color;
			color = TONEMAP_FUNC(color);
			[branch] if (hdrParams.x > 0.5)
				hdrScale = HDRExtendScale(preTonemap, hdrKnee);
		}
#else
        color = TONEMAP_FUNC(color);
#endif
    }

#if defined(HDR_OUTPUT)
	// (44a) HDR working colour space: processing space -> BT.2020 in one matrix, composed as
	// (709 -> 2020) x (processing -> 709) so neutral greys land exactly where the SDR picture puts them.
	// Then the post-curve adjustments (see HDRPostCurve) and the out-of-range correction.
	[branch] if (HDROutputBT2020()) {
		const float3x3 hdrOutputMat = float3x3(hdrOutputTransform[0].xyz, hdrOutputTransform[1].xyz, hdrOutputTransform[2].xyz);
		color = mul(hdrOutputMat, color);
		if (!skipLDR)
			color = HDRPostCurve(color);
		color = CorrectOutOfRangeColor(color, kHDRLuma2020);
	} else {
#endif

	// Inverse color space transform
    if (enableColorSpaceTransform) {
		const float3x3 invColorSpaceTransformMat = float3x3(invColorSpaceTransform[0].xyz, invColorSpaceTransform[1].xyz, invColorSpaceTransform[2].xyz);
        color = mul(invColorSpaceTransformMat, color);
    }

	// LDR
	if (!skipLDR) {
		// Lift Gamma Gain
		color = LiftGammaGain(color, liftgammagain[0].gbar, liftgammagain[1].gbar, liftgammagain[2].gbar);

		// Oklch Saturation
		color = OklchSaturation(color);

		// Oklch Colour Mixer
		color = OklchColourMixer(color);
	}
#if defined(HDR_OUTPUT)
	}
#endif

	return color;
}

#if defined(HDR_OUTPUT)
float4 ApplyLUT(float3 color)
{
	color = LinearToLog(color + LogToLinear(0));
	return TexLUT.SampleLevel(LinearSampler, color, 0);
}
#else
float3 ApplyLUT(float3 color)
{
	color = LinearToLog(color + LogToLinear(0));
	color = TexLUT.SampleLevel(LinearSampler, color, 0).xyz;
	return color;
}
#endif

#if defined(HDR_OUTPUT)
// (44b) HDR only: NaN / Inf / negative guard at the HDR entry and exit. With HDR on, everything from
// here to the swap chain is fp16 and keeps a bad pixel (SDR's 8-bit targets turn it into 0), so
// one NaN could spread until the screen was black. NaN and -Inf -> 0, +Inf -> the fp16 maximum.
float3 SanitizeHDRColor(float3 c, float lo)
{
	const uint3 bits = asuint(c);
	c = ((bits & 0x7F800000u) == 0x7F800000u) ? ((bits == 0x7F800000u) ? 65504.0 : 0.0) : c;
	return clamp(c, lo, 65504.0);
}
#endif

[numthreads(8, 8, 1)] void CSColorGrading(uint2 DTid : SV_DispatchThreadID) {
	// Game cinematic
#if defined(HDR_OUTPUT)
	float3 color = pow(abs(SanitizeHDRColor(LoadInput(DTid), -65504.0)), saturationHueInOutGamma.z) * cinematic.y;
#else
	float3 color = pow(abs(LoadInput(DTid)), saturationHueInOutGamma.z) * cinematic.y;
#endif
	color = Saturation(color, cinematic.x);
	color = LinearContrast(color, cinematic.z, 0.18);

	// Apply LUT or Color Grading
#if defined(HDR_OUTPUT)
	const float4 graded = ApplyLUT(color);
	color = graded.xyz;
#else
	color = ApplyLUT(color);
#endif

	color = pow(abs(color), saturationHueInOutGamma.w);

#if defined(HDR_OUTPUT)
	// (batch 43) HDR output: the texel's highlight factor (see the HDR block above).
	[branch] if (hdrParams.x > 0.5)
		color *= max(graded.w, 1.0);
#endif

#if defined(HDR_OUTPUT)
	// (44a) BT.2020 output: the game's tint and fade colours are BT.709, luminance uses BT.2020 weights.
	[branch] if (HDROutputBT2020()) {
		const float luma2020 = dot(color, kHDRLuma2020);
		color = lerp(color, luma2020 * mul(kHDR709To2020, tint.xyz), tint.w);
		color = lerp(color, mul(kHDR709To2020, fade.xyz), fade.w);
		RWTexOut[DTid] = float4(SanitizeHDRColor(color, -65504.0), 1);
		return;
	}
#endif

    // Game tint
    float luma = Color::RGBToLuminance(color);
    color = lerp(color, luma * tint.xyz, tint.w);

    // Game fade
    color = lerp(color, fade.xyz, fade.w);

#if defined(HDR_OUTPUT)
	color = SanitizeHDRColor(color, -65504.0);
#endif
	RWTexOut[DTid] = float4(color, 1);
}

RWTexture3D<float4> RWLUT : register(u0);

#if defined(HDR_OUTPUT)
// (batch 43) The knee depends only on the tone mapper and the HDR settings: found once per group.
groupshared float gHdrKnee[4];

[numthreads(8, 8, 8)] void CSLUTGen(uint3 DTid : SV_DispatchThreadID, uint groupIndex : SV_GroupIndex) {
	[branch] if (groupIndex == 0)
	{
		HDRKnee knee = HDRNoKnee();
		[branch] if (hdrParams.x > 0.5 && hdrParams.x < 1.5)
			knee = HDRFindKnee();
		gHdrKnee[0] = knee.x;
		gHdrKnee[1] = knee.y;
		gHdrKnee[2] = knee.slope;
		gHdrKnee[3] = knee.valid;
	}
	GroupMemoryBarrierWithGroupSync();
	HDRKnee hdrKnee;
	hdrKnee.x = gHdrKnee[0];
	hdrKnee.y = gHdrKnee[1];
	hdrKnee.slope = gHdrKnee[2];
	hdrKnee.valid = gHdrKnee[3];

	float3 uvw = float3(DTid + 0.5) / float3((LUT_SIZE - 1).xxx);
	float4 neutralColor = float4(uvw, 1);
	float3 linearColor = LogToLinear(neutralColor.xyz) - LogToLinear(0);
	float hdrScale = 1.0;
	linearColor = ColorGrading(linearColor, hdrKnee, hdrScale);
	// Display-gamma chain (Linear Lighting gamma correction off): the factor is applied to gamma values.
	if (hdrParams.w > 0.5)
		hdrScale = pow(hdrScale, 1.0 / 2.2);
	RWLUT[DTid] = float4(linearColor, hdrParams.x > 0.5 ? hdrScale : 1.0);
}
#else
[numthreads(8, 8, 8)] void CSLUTGen(uint3 DTid : SV_DispatchThreadID) {
	float3 uvw = float3(DTid + 0.5) / float3((LUT_SIZE - 1).xxx);
	float4 neutralColor = float4(uvw, 1);
	float3 linearColor = LogToLinear(neutralColor.xyz) - LogToLinear(0);
	linearColor = ColorGrading(linearColor);
	RWLUT[DTid] = float4(linearColor, 1);
}
#endif
