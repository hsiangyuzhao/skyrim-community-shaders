// (batch 38c) The colour the network reads, written at the network's extent.
//
// Destination texel p shows the content texel c = min(p, ContentExtent - 1): everything past the
// content is the edge repeated (the padding to the network grid, see Integration.cpp), so the network
// never reads an uninitialised or stale margin.
//
// Resample = 0: the model works at the frame's own size; c is read 1:1 from the source.
// Resample = 1: the model works smaller (Model Resolution < 100%); c's centre is mapped onto the
//               source and read bilinearly -- an exact 2x2 average at 50%. Before upscaling the
//               average is taken in linear light, then encoded.
//
// Then the colour is wrapped for the network (Common.hlsli): identity after upscaling, the 38a
// Reinhard or the 38c tone curve before.

#define NR_TONE_CURVE_REGISTER t1
#include "Upscaling/NeuralRendering/Common.hlsli"

Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);

RWTexture2D<float4> Destination : register(u0);

cbuffer PrepareColorCB : register(b0)
{
	uint2 DestinationExtent;  // network extent (padded)
	uint2 ContentExtent;      // model content extent
	uint2 SourceExtent;       // frame content extent in the source (top-left)
	float2 SourceTexelSize;   // 1 / source texture dimensions
	uint EncodeMode;
	uint Resample;
	uint2 Pad0;
};

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 pixel = dispatchThreadID.xy;
	if (any(pixel >= DestinationExtent))
		return;

	const uint2 content = min(pixel, ContentExtent - 1);

	float4 source;
	[branch] if (Resample == 0)
	{
		source = Source[content];
	}
	else
	{
		float2 position = (float2(content) + 0.5) * float2(SourceExtent) / float2(ContentExtent);
		position = clamp(position, 0.5, float2(SourceExtent) - 0.5);
		source = Source.SampleLevel(LinearClamp, position * SourceTexelSize, 0);
	}

	// Identity keeps the source's own alpha, as the 37c / 38a copy did; the wrapped HDR input has none.
	Destination[pixel] = EncodeMode == NR_ENCODE_IDENTITY ? source : float4(NrEncode(source.rgb, EncodeMode), 1.0);
}
