// (batch 38a) Depth and motion guides for DLSS Neural Rendering, written at exactly the extent
// the network is given its colour at, so that colour, depth and motion line up texel for texel.
//
// (batch 38c) That extent can now be smaller than the frame (Model Resolution < 100%, our own
// working scale) and padded past the content to the network grid. Destination texel p shows content
// texel c = min(p, ContentExtent - 1) -- the padding repeats the edge, as the colour does -- and c's
// centre is mapped onto each source and point-sampled. At 100% with no padding the depth mapping is
// 1:1 and everything below reads exactly what 38a read.
//
// Depth: before upscaling the render-resolution depth in the top-left of the output-sized depth
// target; after upscaling the output-resolution depth that Upscaling's depth upscale wrote there.
//
// Motion vectors only ever exist at render resolution (top-left region of the output-sized
// motion target). The engine stores them in normalised screen units (previous UV minus current UV),
// which do not depend on the resolution they are stored at -- resampling changes where they are
// read, not their value. The network is told the content extent as their scale.
//
// MotionOffset adds the camera-jitter difference between this frame and the previous one, in
// the same normalised units. It is zero after upscaling (the DLSS output is unjittered). Before
// upscaling the colour the network sees is jittered, and the engine's vectors are not, so
// without it the network's history would be misaligned by the jitter step on every frame.

cbuffer PrepareGuidesCB : register(b0)
{
	float2 MotionSourceScale;  // content texel centre -> motion source texel (render / content extent)
	float2 MotionOffset;       // added to every vector, normalised screen units
	uint2 Extent;              // destination extent (network extent, padded)
	uint2 MotionSourceMax;     // last valid motion source texel (render extent - 1)
	float2 DepthSourceScale;   // (batch 38c) content texel centre -> depth texel (frame / content extent)
	uint2 ContentExtent;       // (batch 38c) model content extent; texels past it repeat the edge
	uint2 DepthSourceMax;      // (batch 38c) last valid depth texel (frame extent - 1)
	uint2 Pad0;
};

Texture2D<float> SourceDepth : register(t0);
Texture2D<float2> SourceMotion : register(t1);

RWTexture2D<float> DestinationDepth : register(u0);
RWTexture2D<float2> DestinationMotion : register(u1);

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 pixel = dispatchThreadID.xy;
	if (any(pixel >= Extent))
		return;

	const float2 centre = float2(min(pixel, ContentExtent - 1)) + 0.5;

	const uint2 depthPixel = min(uint2(centre * DepthSourceScale), DepthSourceMax);
	DestinationDepth[pixel] = SourceDepth[depthPixel];

	const uint2 motionPixel = min(uint2(centre * MotionSourceScale), MotionSourceMax);
	DestinationMotion[pixel] = SourceMotion[motionPixel] + MotionOffset;
}
