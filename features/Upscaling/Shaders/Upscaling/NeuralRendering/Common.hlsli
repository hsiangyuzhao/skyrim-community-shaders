// (batch 38c) Colour wrappers shared by the Batch 38 Neural Rendering passes.
//
// The network is trained on finished, display-referred frames. After upscaling it is given exactly
// that (identity). Before upscaling our colour is the HDR scene in linear light, so it is wrapped in
// an invertible map on the way in and unwrapped with its exact inverse on the way out:
//
//   NR_ENCODE_REINHARD (38a)  e = (c / (1 + max(c)))^(1/2.2). No exposure, a curve unlike the
//                             game's: the network was shown a dark, flat picture (~1.3 EV under
//                             the real one at mid grey with the user's settings) and "fixed" it.
//   NR_ENCODE_CURVE    (38c)  e = T(E * c), per channel. E is the exposure the post-processing
//                             chain will apply to this frame (Histogram Auto Exposure), T the
//                             game's own colour grading and tone curve (the Color Grading LUT,
//                             read along its neutral axis) plus the final gamma. So the network
//                             sees, to within the grading's cross-channel effects, the image the
//                             after-upscaling placement would see. T is a strictly increasing
//                             table, so its inverse is exact; ToneCurveCS.hlsl builds it.
//
// Every decode adds back what the encode lost (original - decode(encode(original))), so a pixel the
// network leaves alone comes back bit-exact whatever the curve; only the network's edit goes through
// the inverse.

#ifndef NR_COMMON_HLSLI
#define NR_COMMON_HLSLI

#define NR_ENCODE_IDENTITY 0
#define NR_ENCODE_REINHARD 1
#define NR_ENCODE_CURVE 2

// ---- 38a: max-channel Reinhard + 2.2 -------------------------------------------------------------

static const float kReinhardGamma = 2.2;
// Largest decodable display value. 1 / (1 - 0.996) = 250 times the encoded value: anything brighter
// encodes as 1 and comes back through the correction term instead.
static const float kReinhardMaxDisplay = 0.996;

float3 EncodeReinhard(float3 color)
{
	color = max(color, 0.0);
	const float peak = max(max(color.r, color.g), color.b);
	return pow(color / (1.0 + peak), 1.0 / kReinhardGamma);
}

float3 DecodeReinhard(float3 encoded)
{
	const float3 display = pow(saturate(encoded), kReinhardGamma);
	const float peak = min(max(max(display.r, display.g), display.b), kReinhardMaxDisplay);
	return display / (1.0 - peak);
}

// ---- 38c: exposure + the game's tone curve ------------------------------------------------------

// The table: kCurveSize display values per channel at exposed scene values spaced evenly in log2
// between kCurveLog2Min and kCurveLog2Max, then one extra entry: (cap r, cap g, cap b, exposure).
// "cap" is the last index the inverse uses: past it the curve is within half an 8-bit step of its
// top, where the inverse would turn rounding noise into whole stops of light.
static const uint kCurveSize = 256;
static const float kCurveLog2Min = -14.0;
static const float kCurveLog2Max = 8.0;
static const float kCurveStep = (kCurveLog2Max - kCurveLog2Min) / float(kCurveSize - 1);

#if defined(NR_TONE_CURVE_REGISTER)
StructuredBuffer<float4> ToneCurve : register(NR_TONE_CURVE_REGISTER);

float CurveExposure()
{
	return max(ToneCurve[kCurveSize].w, 1e-6);
}

float CurveForward(uint channel, float x)
{
	x = max(x, 0.0);
	const float xMin = exp2(kCurveLog2Min);
	if (x <= xMin)
		return ToneCurve[0][channel] * x / xMin;
	const float u = clamp((log2(x) - kCurveLog2Min) / kCurveStep, 0.0, float(kCurveSize - 1));
	const uint i = min(uint(u), kCurveSize - 2);
	return lerp(ToneCurve[i][channel], ToneCurve[i + 1][channel], u - float(i));
}

// Exact inverse of CurveForward up to the cap (piecewise linear in log2 both ways).
float CurveInverse(uint channel, float y)
{
	const uint cap = clamp(uint(ToneCurve[kCurveSize][channel]), 1u, kCurveSize - 1);
	const float t0 = ToneCurve[0][channel];
	y = clamp(y, 0.0, ToneCurve[cap][channel]);
	if (y <= t0)
		return exp2(kCurveLog2Min) * y / max(t0, 1e-8);
	uint lo = 0;
	uint hi = cap;
	[loop] while (hi - lo > 1) {
		const uint mid = (lo + hi) >> 1;
		if (ToneCurve[mid][channel] <= y)
			lo = mid;
		else
			hi = mid;
	}
	const float a = ToneCurve[lo][channel];
	const float b = ToneCurve[hi][channel];
	const float f = saturate((y - a) / max(b - a, 1e-12));
	return exp2(kCurveLog2Min + (float(lo) + f) * kCurveStep);
}

float3 EncodeCurve(float3 color)
{
	const float3 exposed = max(color, 0.0) * CurveExposure();
	return saturate(float3(CurveForward(0, exposed.r), CurveForward(1, exposed.g), CurveForward(2, exposed.b)));
}

float3 DecodeCurve(float3 encoded)
{
	encoded = saturate(encoded);
	return float3(CurveInverse(0, encoded.r), CurveInverse(1, encoded.g), CurveInverse(2, encoded.b)) / CurveExposure();
}
#endif
// ---- dispatch on the mode -------------------------------------------------------------------------

float3 NrEncode(float3 color, uint mode)
{
	float3 result = color;
	[branch] if (mode == NR_ENCODE_REINHARD)
	{
		result = EncodeReinhard(color);
	}
#if defined(NR_TONE_CURVE_REGISTER)
	else if (mode == NR_ENCODE_CURVE)
	{
		result = EncodeCurve(color);
	}
#endif
	return result;
}

float3 NrDecode(float3 encoded, uint mode)
{
	float3 result = encoded;
	[branch] if (mode == NR_ENCODE_REINHARD)
	{
		result = DecodeReinhard(encoded);
	}
#if defined(NR_TONE_CURVE_REGISTER)
	else if (mode == NR_ENCODE_CURVE)
	{
		result = DecodeCurve(encoded);
	}
#endif
	return result;
}

#endif  // NR_COMMON_HLSLI
