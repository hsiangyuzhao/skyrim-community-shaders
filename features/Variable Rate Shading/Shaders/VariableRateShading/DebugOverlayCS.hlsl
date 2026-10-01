// Variable Rate Shading debug view: tints the opaque scene with the rate each 16x16 tile was
// shaded at this frame. Full-rate tiles are left untouched. Applied to the main target right
// after AnalyzeCS has read it, so the tint never feeds back into the next frame's rates.

#include "VariableRateShading/Common.hlsli"

Texture2D<uint> RateImage : register(t0);
RWTexture2D<float4> MainRW : register(u0);

float3 RateColour(uint index)
{
	switch (index) {
	case 1:
		return float3(1.0, 0.55, 0.0);  // 1x2  orange
	case 3:
		return float3(1.0, 1.0, 0.0);  // 2x1  yellow
	case 4:
		return float3(0.0, 1.0, 0.0);  // 2x2  green
	case 5:
		return float3(0.1, 0.3, 1.0);  // 2x4  blue
	case 7:
		return float3(0.0, 0.9, 1.0);  // 4x2  cyan
	case 8:
		return float3(1.0, 0.0, 0.8);  // 4x4  magenta
	default:
		return 0.0;
	}
}

[numthreads(8, 8, 1)] void main(uint3 dispatchId : SV_DispatchThreadID) {
	const uint2 pixel = dispatchId.xy;
	if (any(pixel >= RenderSize))
		return;

	const uint index = RateImage[pixel / VRS_TILE];
	if (index == 0)
		return;

	float4 colour = MainRW[pixel];
	const float luma = max(dot(colour.rgb, float3(0.2126, 0.7152, 0.0722)), 0.0);
	// Keep the scene readable underneath: the tint follows the local brightness.
	const float3 tint = RateColour(index) * (luma + 0.05);
	colour.rgb = lerp(colour.rgb, tint, 0.45);

	// Thin outline on coarse tiles so neighbouring tiles of the same rate stay distinguishable.
	const uint2 inTile = pixel % VRS_TILE;
	if (inTile.x == 0 || inTile.y == 0)
		colour.rgb *= 0.6;

	MainRW[pixel] = colour;
}
