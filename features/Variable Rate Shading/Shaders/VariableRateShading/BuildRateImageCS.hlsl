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
// by the temporal upscaler anyway.
//
// Two guards keep a tile from flickering between rates. Getting coarser than last frame
// requires clearing a slightly stricter threshold (hysteresis). And it is asymmetric in time:
// a tile gets finer at once, but only gets coarser after CoarsenFrames consecutive frames that
// all asked for it, so content that crosses the threshold now and then (a flickering fire,
// a moving shadow) settles at the finer rate instead of switching back and forth.

#include "VariableRateShading/Common.hlsli"

Texture2D<float4> TileStats : register(t0);
Texture2D<uint> PreviousRateImage : register(t1);  // last frame's rates (the other half of a ping-pong pair)
RWTexture2D<uint> RateImageRW : register(u0);
RWByteAddressBuffer RateCountsRW : register(u1);  // tiles per rate index, for the settings panel
RWTexture2D<uint> TileStateRW : register(u2);     // frames in a row this tile has asked to get coarser

// 0 = full rate, 1 = half rate, 2 = quarter rate along one axis.
uint AxisLevel(float error, float threshold, uint previousLevel)
{
	const float halfThreshold = threshold * (previousLevel >= 1 ? 1.0 : Hysteresis);
	const float quarterThreshold = threshold / QuarterFactor * (previousLevel >= 2 ? 1.0 : Hysteresis);
	return error < quarterThreshold ? 2 : (error < halfThreshold ? 1 : 0);
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
	uint streak = 0;
	const bool inRender = all(tile < RenderTiles);
	if (inRender) {
		const float2 centre = (float2(tile) + 0.5) * VRS_TILE;
		const bool adaptive = Mode != VRS_MODE_PERIPHERY && HistoryValid != 0;

		if (adaptive) {
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

		if (adaptive && CoarsenFrames > 0) {
			// Finer on an axis takes effect now; coarser on any axis waits for a full streak.
			// The per-axis minimum of two valid rates is itself valid (never 4x1 / 1x4).
			if (levelX > previousX || levelY > previousY) {
				streak = TileStateRW[tile] + 1;
				if (streak >= CoarsenFrames)
					streak = 0;
				else {
					levelX = min(levelX, previousX);
					levelY = min(levelY, previousY);
				}
			}
		}
	}

	const uint index = levelX * 3 + levelY;
	RateImageRW[tile] = index;
	TileStateRW[tile] = streak;
	if (inRender)
		RateCountsRW.InterlockedAdd(index * 4, 1);
}
