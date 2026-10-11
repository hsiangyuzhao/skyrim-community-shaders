// (batch 38c) Brings the network's answer back onto the frame. Replaces 38a's ColorTransformCS decode.
//
// Per frame pixel p (frame extent, top-left of the scene):
//
//   Model at the frame's size (Scaled = 0) -- the network's texel p is the frame's texel p:
//     after upscaling:  result = network output                                   (37c / 38a)
//     before:           result = decode(output) + (original - decode(encoded input))   (38a)
//   (unchanged in 42a: Residual Mode only acts below 100%)
//
//   Model smaller (Scaled = 1, our own working scale) -- only the network's edit is carried up.
//
//   ResidualMode 0, additive (38c-41b):
//     edit   = joint-bilateral upsample of (output - encoded input), in the encoded space, with
//              depth deciding which of the four nearest model texels may contribute (an edge does
//              not bleed the sky's edit onto a roof or the other way round)
//     after:   result = original + edit
//     before:  result = decode(encode(original) + edit) + (original - decode(encode(original)))
//
//   ResidualMode 1, log-ratio split (42a):
//     The edit is a per-channel ratio in light, not a difference in display values:
//       r(q) = log2((lin(output) + eps) / (lin(input) + eps)),   lin(x) = x^2.2, eps = 2e-3,
//     clamped to +-RatioClamp stops. A difference that brightens a dark texel by 0.05 brightens a
//     neighbouring highlight by the same 0.05, which is where the additive mode's light and dark
//     fringes at contrasty edges come from; a ratio scales with the light it is applied to.
//     The RATIO variant of this file computes r once per model texel (plus the model input's log
//     luminance, the guide), so the frame pass reads one texture per tap.
//     At frame pixel p the ratio is then built from two parts:
//       broad  -- 4x4 model texels around p, weighted by a Gaussian in model texels (sigma 1), by
//                 depth (as the additive mode), and by colour: how close the model input's log
//                 luminance at q is to the full-resolution original's at p (joint-bilateral
//                 upsampling, guided by the full-resolution input; GuideSigma stops). The smooth
//                 lighting part, spread without crossing depth or brightness edges.
//       fine   -- the inner 2x2, bilinear x depth (the additive mode's footprint), on the ratio.
//       detail -- fine - broad, added only where the edges agree: where the model input,
//                 interpolated to p, has the original's luminance at p (within DetailSigma stops)
//                 and the depth weights did not collapse. Where the full-resolution picture has
//                 structure the small one lacks (a thin branch, a sub-texel edge), the detail the
//                 network drew sits in the wrong place, so only the broad part is used there.
//       ratio  = clamp(broad + agreement * (fine - broad))
//     after:   lin(result) = (lin(original) + eps) * 2^ratio - eps
//     before:  the same on encode(original), decoded, plus what the encode lost
//   Either way a pixel the network left alone is still the original, and the picture's own detail
//   stays full resolution; only what the network changed is lower resolution.
//
// Tone preservation (ToneEnabled, strength 0..1): the network may also drift the picture's overall
// brightness and colour. The STATS variant of this file averages, over a coarse grid (about 32 x 18
// cells), the log of the original and of the result, per channel; the main pass then multiplies the
// result by 2^(strength * (blur(log original) - blur(log result))). That locks the low-frequency
// luminance and chroma to the input and keeps everything the network did at smaller scales: local
// relighting, contrast and detail. The gain is clamped to +-2 stops.

#define NR_TONE_CURVE_REGISTER t3
#include "Upscaling/NeuralRendering/Common.hlsli"

Texture2D<float4> EncodedInput : register(t1);   // what the network read (network extent)
Texture2D<float4> NetworkOutput : register(t2);  // what it returned (network extent)

cbuffer FinalizeCB : register(b0)
{
	uint2 FullExtent;  // frame extent
	uint2 WorkExtent;  // model content extent (top-left of the network extent)
	uint EncodeMode;
	uint Scaled;
	uint ToneEnabled;
	uint DisplayEncoded;  // after upscaling: colour is display-encoded, linearised for the tone statistics
	float ToneStrength;
	float DepthSigma;  // relative depth difference at which a model texel's weight falls to 1/e
	float CameraNear;
	float CameraFar;
	uint2 Grid;
	float2 GridTexel;
	// (batch 42a)
	uint ResidualMode;  // 0 additive (41b), 1 log-ratio split
	float DetailSigma;  // stops: model input vs original luminance difference at which detail falls to 1/e
	float GuideSigma;   // stops: colour similarity of the broad part's joint-bilateral weights
	float RatioClamp;   // stops: largest brightening or darkening the edit may apply
};

static const float kRatioEpsilon = 2e-3;

float3 RatioLinear(float3 encoded)
{
	return pow(max(encoded, 0.0), 2.2);
}

float3 RatioEncode(float3 linearColor)
{
	return pow(max(linearColor, 0.0), 1.0 / 2.2);
}

float RatioLogLuma(float3 linearColor)
{
	return log2(dot(linearColor, float3(0.2126, 0.7152, 0.0722)) + kRatioEpsilon);
}

#if defined(RATIO)

// (batch 42a) One thread per model texel: the network's edit as a log2 ratio (rgb) and the log
// luminance of what the network read (a), the guide the frame pass compares against.
RWTexture2D<float4> RatioOut : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 texel = dispatchThreadID.xy;
	if (any(texel >= WorkExtent))
		return;
	const float3 inputLinear = RatioLinear(EncodedInput[texel].rgb);
	const float3 outputLinear = RatioLinear(NetworkOutput[texel].rgb);
	const float3 ratio = clamp(log2((outputLinear + kRatioEpsilon) / (inputLinear + kRatioEpsilon)), -RatioClamp, RatioClamp);
	RatioOut[texel] = float4(ratio, RatioLogLuma(inputLinear));
}

#else

Texture2D<float4> Original : register(t0);  // the frame as it was before the pass (frame extent)
Texture2D<float> DepthFull : register(t4);  // frame-resolution depth, 1:1 with Original
Texture2D<float> DepthWork : register(t5);  // the depth guide the network read
Texture2D<float4> Ratio : register(t8);     // (batch 42a) RATIO variant's output (model extent)

#	if defined(STATS)
RWTexture2D<float4> StatsOriginalOut : register(u0);
RWTexture2D<float4> StatsResultOut : register(u1);
#	else
Texture2D<float4> StatsOriginal : register(t6);
Texture2D<float4> StatsResult : register(t7);
RWTexture2D<float4> Destination : register(u0);
SamplerState LinearClamp : register(s0);
#	endif

float LinearDepth(float depth)
{
	return CameraNear * CameraFar / max(CameraFar - depth * (CameraFar - CameraNear), 1e-6);
}

float3 UpsampledEdit(uint2 pixel)
{
	const float2 position = (float2(pixel) + 0.5) * float2(WorkExtent) / float2(FullExtent) - 0.5;
	const float2 base = floor(position);
	const float2 fraction = position - base;
	const float pixelDepth = LinearDepth(DepthFull[pixel]);

	float3 sum = 0.0;
	float weightSum = 0.0;
	float3 nearest = 0.0;
	float nearestDifference = 1e30;
	[unroll] for (uint i = 0; i < 4; ++i)
	{
		const int2 offset = int2(i & 1, i >> 1);
		const uint2 texel = uint2(clamp(int2(base) + offset, int2(0, 0), int2(WorkExtent) - 1));
		const float3 edit = NetworkOutput[texel].rgb - EncodedInput[texel].rgb;
		const float difference = abs(LinearDepth(DepthWork[texel]) - pixelDepth) / max(pixelDepth, 1e-3);
		const float bilinear = (offset.x ? fraction.x : 1.0 - fraction.x) * (offset.y ? fraction.y : 1.0 - fraction.y);
		const float ratio = difference / DepthSigma;
		const float weight = bilinear * exp(-ratio * ratio);
		sum += weight * edit;
		weightSum += weight;
		if (difference < nearestDifference) {
			nearestDifference = difference;
			nearest = edit;
		}
	}
	// Every neighbour across a depth edge: take the one closest in depth rather than average across it.
	return weightSum > 1e-3 ? sum / weightSum : nearest;
}

// (batch 42a) The log-ratio split (see the top of the file). guideLogLuma: the full-resolution
// original's log2 luminance, in the network's colour space.
float3 SplitRatio(uint2 pixel, float guideLogLuma)
{
	const float2 position = (float2(pixel) + 0.5) * float2(WorkExtent) / float2(FullExtent) - 0.5;
	const float2 base = floor(position);
	const float2 fraction = position - base;
	const float pixelDepth = LinearDepth(DepthFull[pixel]);
	const float inverseGuideSigma = 1.0 / max(GuideSigma, 1e-3);

	float3 broadSum = 0.0;
	float broadWeight = 0.0;
	float3 fineSum = 0.0;
	float fineWeight = 0.0;
	float lowLogLuma = 0.0;  // the model input, bilinear at p
	float3 nearest = 0.0;
	float nearestDifference = 1e30;
	[unroll] for (int y = -1; y <= 2; ++y)
	{
		[unroll] for (int x = -1; x <= 2; ++x)
		{
			const int2 offset = int2(x, y);
			const uint2 texel = uint2(clamp(int2(base) + offset, int2(0, 0), int2(WorkExtent) - 1));
			const float4 tap = Ratio[texel];
			const float difference = abs(LinearDepth(DepthWork[texel]) - pixelDepth) / max(pixelDepth, 1e-3);
			const float depthRatio = difference / DepthSigma;
			const float depthWeight = exp(-depthRatio * depthRatio);

			const float2 delta = float2(offset) - fraction;
			const float spatial = exp(-0.5 * dot(delta, delta));
			const float guide = (tap.a - guideLogLuma) * inverseGuideSigma;
			const float broad = spatial * depthWeight * exp(-guide * guide);
			broadSum += broad * tap.rgb;
			broadWeight += broad;

			if (x >= 0 && x <= 1 && y >= 0 && y <= 1) {
				const float bilinear = (x ? fraction.x : 1.0 - fraction.x) * (y ? fraction.y : 1.0 - fraction.y);
				const float fine = bilinear * depthWeight;
				fineSum += fine * tap.rgb;
				fineWeight += fine;
				lowLogLuma += bilinear * tap.a;
				if (difference < nearestDifference) {
					nearestDifference = difference;
					nearest = tap.rgb;
				}
			}
		}
	}
	const float3 fine = fineWeight > 1e-3 ? fineSum / fineWeight : nearest;
	const float3 broad = broadWeight > 1e-4 ? broadSum / broadWeight : fine;
	// Edges agree: the small picture, interpolated here, has the full picture's luminance, and the
	// depth weights of the 2x2 footprint held (they fall when p is across a depth edge from them).
	const float lumaDifference = (guideLogLuma - lowLogLuma) / max(DetailSigma, 1e-3);
	const float agreement = exp(-lumaDifference * lumaDifference) * saturate(fineWeight * 2.0);
	return clamp(broad + agreement * (fine - broad), -RatioClamp, RatioClamp);
}

float3 ApplyRatio(float3 encoded, float3 ratio)
{
	return RatioEncode((RatioLinear(encoded) + kRatioEpsilon) * exp2(ratio) - kRatioEpsilon);
}

float4 Reconstruct(uint2 pixel)
{
	const float4 original = Original[pixel];
	float3 result;
	float alpha = original.a;

	[branch] if (Scaled == 0)
	{
		const float4 network = NetworkOutput[pixel];
		if (EncodeMode == NR_ENCODE_IDENTITY) {
			result = network.rgb;
			alpha = network.a;
		} else {
			const float3 lostOnTheWayIn = original.rgb - NrDecode(EncodedInput[pixel].rgb, EncodeMode);
			result = NrDecode(network.rgb, EncodeMode) + lostOnTheWayIn;
		}
	}
	else if (ResidualMode == 0)
	{
		const float3 edit = UpsampledEdit(pixel);
		if (EncodeMode == NR_ENCODE_IDENTITY) {
			result = saturate(original.rgb + edit);
		} else {
			const float3 encoded = NrEncode(original.rgb, EncodeMode);
			result = NrDecode(saturate(encoded + edit), EncodeMode) + (original.rgb - NrDecode(encoded, EncodeMode));
		}
	}
	else
	{
		const float3 encoded = EncodeMode == NR_ENCODE_IDENTITY ? original.rgb : NrEncode(original.rgb, EncodeMode);
		const float3 ratio = SplitRatio(pixel, RatioLogLuma(RatioLinear(encoded)));
		const float3 edited = saturate(ApplyRatio(encoded, ratio));
		if (EncodeMode == NR_ENCODE_IDENTITY)
			result = edited;
		else
			result = NrDecode(edited, EncodeMode) + (original.rgb - NrDecode(encoded, EncodeMode));
	}
	return float4(max(result, 0.0), alpha);
}

float3 ToneSpace(float3 color)
{
	color = max(color, 0.0);
	if (DisplayEncoded)
		color = pow(color, 2.2);
	return log2(color + 1e-3);
}

#	if defined(STATS)

groupshared float4 gOriginal[256];
groupshared float3 gResult[256];

// One group per grid cell. Every second pixel each way is enough for an average this coarse.
[numthreads(16, 16, 1)] void main(uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID, uint index : SV_GroupIndex) {
	const uint2 cell = groupID.xy;
	const uint2 low = cell * FullExtent / Grid;
	const uint2 high = (cell + 1) * FullExtent / Grid;

	float3 sumOriginal = 0.0;
	float3 sumResult = 0.0;
	float count = 0.0;
	[loop] for (uint y = low.y + threadID.y * 2; y < high.y; y += 32)
	{
		[loop] for (uint x = low.x + threadID.x * 2; x < high.x; x += 32)
		{
			const uint2 pixel = uint2(x, y);
			sumOriginal += ToneSpace(Original[pixel].rgb);
			sumResult += ToneSpace(Reconstruct(pixel).rgb);
			count += 1.0;
		}
	}
	gOriginal[index] = float4(sumOriginal, count);
	gResult[index] = sumResult;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = 128; stride > 0; stride >>= 1)
	{
		if (index < stride) {
			gOriginal[index] += gOriginal[index + stride];
			gResult[index] += gResult[index + stride];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (index == 0) {
		const float total = gOriginal[0].w;
		const float3 meanOriginal = total > 0.0 ? gOriginal[0].rgb / total : 0.0;
		const float3 meanResult = total > 0.0 ? gResult[0] / total : 0.0;
		StatsOriginalOut[cell] = float4(meanOriginal, 1.0);
		StatsResultOut[cell] = float4(meanResult, 1.0);
	}
}

#	else

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 pixel = dispatchThreadID.xy;
	if (any(pixel >= FullExtent))
		return;

	float4 result = Reconstruct(pixel);

	[branch] if (ToneEnabled)
	{
		// A 2x2 bilinear footprint, half a cell either way: a tent over the coarse grid, so the gain has
		// no cell-shaped steps.
		const float2 uv = (float2(pixel) + 0.5) / float2(FullExtent);
		float3 logOriginal = 0.0;
		float3 logResult = 0.0;
		[unroll] for (uint i = 0; i < 4; ++i)
		{
			const float2 offset = float2((i & 1) ? 0.5 : -0.5, (i >> 1) ? 0.5 : -0.5) * GridTexel;
			logOriginal += StatsOriginal.SampleLevel(LinearClamp, uv + offset, 0).rgb;
			logResult += StatsResult.SampleLevel(LinearClamp, uv + offset, 0).rgb;
		}
		const float3 gainLog2 = clamp((logOriginal - logResult) * 0.25, -2.0, 2.0) * ToneStrength;

		float3 value = max(result.rgb, 0.0);
		if (DisplayEncoded)
			value = pow(value, 2.2);
		value *= exp2(gainLog2);
		if (DisplayEncoded)
			value = pow(saturate(value), 1.0 / 2.2);
		result.rgb = value;
	}

	Destination[pixel] = result;
}

#	endif

#endif
