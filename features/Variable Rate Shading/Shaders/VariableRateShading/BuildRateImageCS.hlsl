// Variable Rate Shading, pass 2 of 2: build this frame's shading-rate image.
//
// Runs at the start of the opaque (G-buffer) pass, one thread per 16x16 tile of the full
// render target. Tiles outside the dynamic-resolution sub-rectangle are written as 1x1; the
// rate image is indexed by render-target pixel, so the sub-rectangle needs no remapping.
//
// Adaptive mode reads the tile statistics AnalyzeCS wrote at the end of the previous frame.
// Content now under a tile was, a frame ago, at (tile centre + motion), so the statistics are
// fetched from there (constant-velocity reprojection, as in NVIDIA Adaptive Shading). Faster
// motion raises the threshold, since moving content is blurred by the eye, by motion blur and
// by the temporal upscaler anyway. Getting coarser than last frame requires clearing a
// slightly stricter threshold (hysteresis) so tiles sitting near the threshold do not flicker.

#include "VariableRateShading/Common.hlsli"

Texture2D<float4> TileStats : register(t0);
Texture2D<uint> PreviousRateImage : register(t1);  // last frame's rates (the other half of a ping-pong pair)
RWTexture2D<uint> RateImageRW : register(u0);
RWByteAddressBuffer RateCountsRW : register(u1);  // tiles per rate index, for the settings panel

// 0 = full rate, 1 = half rate, 2 = quarter rate along one axis.
uint AxisLevel(float error, float threshold, uint previousLevel)
{
	const float halfThreshold = threshold * (previousLevel >= 1 ? 1.0 : Hysteresis);
	const float quarterThreshold = threshold / QuarterFactor * (previousLevel >= 2 ? 1.0 : Hysteresis);
	if (error < quarterThreshold)
		return 2;
	if (error < halfThreshold)
		return 1;
	return 0;
}

[numthreads(8, 8, 1)] void main(uint3 dispatchId : SV_DispatchThreadID) {
	const uint2 tile = dispatchId.xy;
	if (any(tile >= ImageTiles))
		return;

	const uint previous = min(PreviousRateImage[tile], 8u);
	const uint previousX = previous / 3;
	const uint previousY = previous % 3;

	uint levelX = 0;
	uint levelY = 0;
	const bool inRender = all(tile < RenderTiles);
	if (inRender) {
		const float2 centre = (float2(tile) + 0.5) * VRS_TILE;

		if (Mode != VRS_MODE_PERIPHERY && HistoryValid != 0) {
			const float2 motion = TileStats[tile].zw;
			const int2 sourceTile = clamp(int2(floor((centre + motion) / VRS_TILE)), int2(0, 0), int2(RenderTiles) - 1);
			const float4 stats = TileStats[sourceTile];
			const float speed = length(stats.zw);
			const float threshold = Threshold * (MotionPixels > 0.0 ? 1.0 + speed / MotionPixels : 1.0);
			levelX = AxisLevel(stats.x, threshold, previousX);
			levelY = AxisLevel(stats.y, threshold, previousY);
		}

		if (Mode != VRS_MODE_ADAPTIVE) {
			// Distance from the screen centre in half screen heights, so the full-rate area
			// is a circle regardless of aspect ratio.
			const float2 offset = (centre - float2(RenderSize) * 0.5) / (float(RenderSize.y) * 0.5);
			const float radius = length(offset);
			const uint level = radius < PeripheryRadius ? 0 : (radius < PeripheryRadius + 0.5 ? 1 : 2);
			levelX = max(levelX, level);
			levelY = max(levelY, level);
		}

		levelX = min(levelX, MaxRateLog2);
		levelY = min(levelY, MaxRateLog2);
		// 4x1 and 1x4 do not exist in hardware; fall back to the finer neighbour.
		if (levelX == 2 && levelY == 0)
			levelX = 1;
		if (levelY == 2 && levelX == 0)
			levelY = 1;
	}

	const uint index = levelX * 3 + levelY;
	RateImageRW[tile] = index;
	if (inRender)
		RateCountsRW.InterlockedAdd(index * 4, 1);
}
