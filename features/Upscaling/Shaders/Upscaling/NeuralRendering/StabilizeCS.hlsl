// (49h) "Stabilise Neural Rendering" (Upscaling > Neural Rendering, default off). Runs after FinalizeCS's frame pass, on the
// shader route (not the direct copy), before the result goes back into the scene.
//
// The model's change at a frame pixel, as a log2 ratio per channel in light:
//   r = clamp(log2((lin(result) + eps) / (lin(original) + eps)), +-RatioClamp)
// (lin = the colour as is before upscaling, the HDR scene; x^2.2 of the display-encoded image after upscaling). Last frame's
// stabilised r (History .rgb, with its linear depth in .a) is fetched where the motion vectors say this pixel was, kept only
// where the depth there matches this pixel's (relative DepthTolerance), clamped into the range of this frame's r over the 3x3
// neighbourhood, and blended in with weight MaxWeight x (1 - |motion| / MotionPixels) (0.65, 12 px). The result is the
// original times 2^r_stable; where no history is used, the network's result exactly. The stabilised r becomes the next history.
// Only the model's change is smoothed over time: the frame's own content, its motion and its edges are this frame's.

cbuffer StabilizeCB : register(b0)
{
	uint2 Extent;              // frame extent (Original, Result, Depth, History and both outputs share it, top-left)
	uint2 MotionSourceMax;     // last valid motion source texel
	float2 MotionSourceScale;  // frame pixel centre -> motion source texel
	float2 MotionOffset;       // jitter step added to every vector (normalised screen units; 0 after upscaling)
	uint DisplayEncoded;       // after upscaling: the colours are display-encoded
	uint HistoryValid;         // History holds last frame's values at this extent
	float CameraNear;
	float CameraFar;
	float MaxWeight;           // 0.65
	float MotionPixels;        // 12: the history's weight reaches 0 at this motion (frame pixels per frame)
	float DepthTolerance;      // relative linear-depth difference past which the history is dropped
	float RatioClamp;          // stops
};

Texture2D<float4> Original : register(t0);  // the frame the model was given (before the pass)
Texture2D<float4> Result : register(t1);    // FinalizeCS's result
Texture2D<float> Depth : register(t2);      // frame-resolution depth, 1:1
Texture2D<float2> Motion : register(t3);    // the engine's vectors (previous UV - current UV), render resolution
Texture2D<float4> History : register(t4);   // last frame: stabilised r (rgb), linear depth (a)
SamplerState LinearClamp : register(s0);

RWTexture2D<float4> Destination : register(u0);
RWTexture2D<float4> HistoryOut : register(u1);

static const float kEpsilon = 2e-3;

float3 ToLight(float3 color)
{
	color = max(color, 0.0);
	return DisplayEncoded ? pow(color, 2.2) : color;
}

float3 FromLight(float3 light)
{
	light = max(light, 0.0);
	return DisplayEncoded ? pow(saturate(light), 1.0 / 2.2) : light;
}

float3 Residual(uint2 pixel)
{
	const float3 original = ToLight(Original[pixel].rgb);
	const float3 result = ToLight(Result[pixel].rgb);
	return clamp(log2((result + kEpsilon) / (original + kEpsilon)), -RatioClamp, RatioClamp);
}

float LinearDepth(float depth)
{
	return CameraNear * CameraFar / max(CameraFar - depth * (CameraFar - CameraNear), 1e-6);
}

[numthreads(8, 8, 1)] void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
	const uint2 pixel = dispatchThreadID.xy;
	if (any(pixel >= Extent))
		return;

	const float4 result = Result[pixel];
	const float3 current = Residual(pixel);
	const float depth = LinearDepth(Depth[pixel]);

	// This frame's range of the change around the pixel.
	float3 low = current;
	float3 high = current;
	[unroll] for (int y = -1; y <= 1; ++y)
	{
		[unroll] for (int x = -1; x <= 1; ++x)
		{
			if (x == 0 && y == 0)
				continue;
			const uint2 tap = uint2(clamp(int2(pixel) + int2(x, y), int2(0, 0), int2(Extent) - 1));
			const float3 r = Residual(tap);
			low = min(low, r);
			high = max(high, r);
		}
	}

	float3 stable = current;
	float weight = 0.0;
	[branch] if (HistoryValid)
	{
		const float2 extent = float2(Extent);
		const uint2 motionTexel = min(uint2((float2(pixel) + 0.5) * MotionSourceScale), MotionSourceMax);
		const float2 motion = Motion[motionTexel] + MotionOffset;
		const float2 previousUV = (float2(pixel) + 0.5) / extent + motion;
		[branch] if (all(previousUV > 0.0) && all(previousUV < 1.0))
		{
			const float4 history = History.SampleLevel(LinearClamp, previousUV, 0);
			const float depthDifference = abs(history.a - depth) / max(depth, 1e-3);
			const float motionPixels = length(motion * extent);
			weight = depthDifference <= DepthTolerance ? MaxWeight * saturate(1.0 - motionPixels / max(MotionPixels, 1e-3)) : 0.0;
			[branch] if (weight > 0.0)
				stable = lerp(current, clamp(history.rgb, low, high), weight);
		}
	}

	float3 color = result.rgb;
	[branch] if (weight > 0.0)
		color = FromLight((ToLight(Original[pixel].rgb) + kEpsilon) * exp2(stable) - kEpsilon);
	Destination[pixel] = float4(color, result.a);
	HistoryOut[pixel] = float4(stable, depth);
}
