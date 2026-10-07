// (batch 38a) Depth and motion guides for DLSS Neural Rendering, written at exactly the extent
// the network is given its colour at, so that colour, depth and motion line up texel for texel.
//
// Depth is read 1:1. Before upscaling it is the render-resolution depth in the top-left of the
// output-sized depth target; after upscaling it is the output-resolution depth that Upscaling's
// depth upscale wrote there.
//
// Motion vectors only ever exist at render resolution (top-left region of the output-sized
// motion target), so after upscaling they are point-sampled up to the output grid. The engine
// stores them in normalised screen units (previous UV minus current UV), which do not depend on
// the resolution they are stored at -- resampling changes where they are read, not their value.
//
// MotionOffset adds the camera-jitter difference between this frame and the previous one, in
// the same normalised units. It is zero after upscaling (the DLSS output is unjittered). Before
// upscaling the colour the network sees is jittered, and the engine's vectors are not, so
// without it the network's history would be misaligned by the jitter step on every frame.

cbuffer PrepareGuidesCB : register(b0)
{
	float2 MotionSourceScale;  // destination texel centre -> motion source texel (render / destination extent)
	float2 MotionOffset;       // added to every vector, normalised screen units
	uint2 Extent;              // destination extent
	uint2 MotionSourceMax;     // last valid motion source texel (render extent - 1)
};

Texture2D<float> SourceDepth : register(t0);
Texture2D<float2> SourceMotion : register(t1);

RWTexture2D<float> DestinationDepth : register(u0);
RWTexture2D<float2> DestinationMotion : register(u1);

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 pixel = dispatchThreadID.xy;
	if (any(pixel >= Extent))
		return;

	DestinationDepth[pixel] = SourceDepth[pixel];

	const uint2 motionPixel = min(uint2((float2(pixel) + 0.5) * MotionSourceScale), MotionSourceMax);
	DestinationMotion[pixel] = SourceMotion[motionPixel] + MotionOffset;
}
