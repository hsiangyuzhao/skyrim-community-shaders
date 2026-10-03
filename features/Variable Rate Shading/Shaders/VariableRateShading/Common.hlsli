#ifndef __VARIABLE_RATE_SHADING_COMMON_HLSLI__
#define __VARIABLE_RATE_SHADING_COMMON_HLSLI__

// Shading-rate tiles are fixed by the hardware: NV_VARIABLE_PIXEL_SHADING_TILE_WIDTH/HEIGHT.
#define VRS_TILE 16

#define VRS_MODE_ADAPTIVE 0
#define VRS_MODE_PERIPHERY 1
#define VRS_MODE_ADAPTIVE_PERIPHERY 2

// Rate-image texels are an index into the per-viewport shading-rate table that the plugin
// hands to NVAPI: index = log2(width) * 3 + log2(height), so 0 = 1x1, 4 = 2x2, 8 = 4x4.
// The build pass never emits 2 (1x4) or 6 (4x1); the table maps them to 1x2 / 2x1 anyway.

// Rate-image index -> log2 of the coarse-pixel size along each axis (exact for every index the
// build pass emits).
uint2 RateLog2(uint index)
{
	index = min(index, 8u);
	return uint2(index / 3, index % 3);
}

// Mirrors VariableRateShading::RateCB on the C++ side. 80 bytes.
cbuffer RateCB : register(b0)
{
	uint2 RenderSize;       // render-resolution extent in pixels (the dynamic-resolution sub-rectangle)
	uint2 RenderTiles;      // tiles that cover RenderSize
	uint2 ImageTiles;       // rate-image size: the full render target divided by VRS_TILE
	uint Mode;              // VRS_MODE_*
	uint MaxRateLog2;       // 1 = cap at 2x2, 2 = cap at 4x4
	float Threshold;        // relative error a tile may have and still drop to half rate on an axis
	float QuarterFactor;    // how much larger the error gets going from half to quarter rate
	float MotionPixels;     // speed (pixels/frame) at which Threshold doubles; 0 = no motion boost
	float NormalWeight;     // weight of G-buffer normal detail in the error; 0 = ignore normals
	float PeripheryRadius;  // radius of the full-rate centre, in half screen heights
	float EnvLuminance;     // luminance floor added to the tile mean (Weber-law denominator)
	uint HistoryValid;      // 1 when last frame's tile statistics describe this view
	float Hysteresis;       // threshold multiplier for getting coarser than last frame (< 1)
	uint CoarsenFrames;     // consecutive frames a tile must qualify before it gets coarser; 0 = at once
	uint Pad0;
	uint RateAwareAnalysis;  // 1 = measure at the spacing this frame was actually shaded at
	uint AppliedRatesValid;  // 1 = RateImage (AnalyzeCS t3) is what this frame's opaque draws used
};

#endif  // __VARIABLE_RATE_SHADING_COMMON_HLSLI__
