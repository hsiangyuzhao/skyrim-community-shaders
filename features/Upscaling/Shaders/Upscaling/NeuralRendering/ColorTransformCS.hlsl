// (batch 38a) Colour wrapper for running DLSS Neural Rendering before upscaling.
//
// The network is trained on finished, display-referred frames: values in [0, 1], perceptually
// encoded. Before upscaling our colour is the HDR scene in linear light, open-ended, which is what
// DLSS super resolution wants and what the network does not. So the input is wrapped in an
// invertible tone map, and the output unwrapped with its exact inverse:
//
//   encode  e = (c / (1 + max(c.r, c.g, c.b)))^(1/2.2)     -> RGBA8 UNORM, what the network reads
//   decode  c = d / (1 - max(d.r, d.g, d.b)),  d = e^2.2
//
// Reinhard on the largest channel keeps hue (all three channels share one scale) and is exactly
// invertible below the clamp; the 2.2 power puts the result in the perceptual encoding an 8-bit
// display image has.
//
// Precision: 8 bits cannot hold a highlight's value well (the inverse amplifies a step near 1).
// The decode therefore adds back what the encode lost on the way in:
//
//   result = decode(network output) + (original - decode(encoded input))
//
// A pixel the network left alone comes back bit-exact, at any brightness; only what the network
// changed carries 8-bit rounding, and DLSS accumulates over many frames on top of that.

Texture2D<float4> Original : register(t0);

#if defined(DECODE)
Texture2D<unorm float4> EncodedInput : register(t1);
Texture2D<unorm float4> NetworkOutput : register(t2);
RWTexture2D<float4> Destination : register(u0);
#else
RWTexture2D<unorm float4> Destination : register(u0);
#endif

cbuffer ColorTransformCB : register(b0)
{
	uint2 Extent;
	uint2 Pad0;
};

static const float kGamma = 2.2;
// Largest decodable display value. 1 / (1 - 0.996) = 250 times the encoded value: anything brighter
// encodes as 1 and comes back through the correction term instead.
static const float kMaxDisplay = 0.996;

float3 EncodeHdr(float3 color)
{
	color = max(color, 0.0);
	const float peak = max(max(color.r, color.g), color.b);
	return pow(color / (1.0 + peak), 1.0 / kGamma);
}

float3 DecodeHdr(float3 encoded)
{
	const float3 display = pow(saturate(encoded), kGamma);
	const float peak = min(max(max(display.r, display.g), display.b), kMaxDisplay);
	return display / (1.0 - peak);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 pixel = dispatchThreadID.xy;
	if (any(pixel >= Extent))
		return;

	const float4 original = Original[pixel];

#if defined(DECODE)
	const float3 lostOnTheWayIn = original.rgb - DecodeHdr(EncodedInput[pixel].rgb);
	const float3 result = DecodeHdr(NetworkOutput[pixel].rgb) + lostOnTheWayIn;
	Destination[pixel] = float4(max(result, 0.0), original.a);
#else
	Destination[pixel] = float4(EncodeHdr(original.rgb), 1.0);
#endif
}
