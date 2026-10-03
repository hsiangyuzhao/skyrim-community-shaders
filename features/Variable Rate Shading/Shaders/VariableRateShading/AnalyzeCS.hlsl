// Variable Rate Shading, pass 1 of 2: per-tile content analysis.
//
// Runs once per frame right after the deferred composite, on this frame's lit opaque scene at
// render resolution. One thread group per 16x16 shading-rate tile. For every tile it estimates
// how much error dropping to half rate along each axis would introduce (NVIDIA Adaptive
// Shading style): the RMS luminance difference between horizontally / vertically adjacent
// pixels, divided by the tile's mean luminance (Weber's law: the same absolute error is far
// less visible on a bright surface than on a dark one). Optionally the RMS difference between
// adjacent G-buffer normals is folded in, so tiles whose normals carry detail the screen-space
// effects (SSGI / SSRT) rely on stay at full rate even when their lit colour is flat.
//
// The image measured here was itself shaded at this frame's rates. Inside a coarse pixel every
// pixel holds the same value, so adjacent-pixel differences are zero inside a block and twice
// the real step across block edges. On anything that varies over more than a few pixels (a
// magnified texture or normal-map bumps under a nearby torch) that reads up to ~1.4x higher
// than the same surface at full rate. A tile near the threshold then measures
// "too detailed" whenever it is coarse and "flat" whenever it is fine, and flips every frame.
// So differences are taken at the spacing the tile was shaded at and divided by it: a coarse
// tile can never measure higher than the same content at full rate, and the loop is stable.
//
// The result feeds BuildRateImageCS at the start of the NEXT frame's opaque pass, which
// reprojects it with the motion stored here.

#include "Common/GBuffer.hlsli"
#include "VariableRateShading/Common.hlsli"

Texture2D<float4> ColorTexture : register(t0);            // main scene colour after the deferred composite
Texture2D<float2> MotionVectorTexture : register(t1);     // prevUV - currUV, render-resolution UV units
Texture2D<float4> NormalRoughnessTexture : register(t2);  // CS G-buffer: octahedral normal in xy
Texture2D<uint> RateImage : register(t3);                 // the rates this frame's opaque draws were shaded at

// x: relative error at half rate horizontally, y: vertically, zw: mean motion (pixels, prev - curr)
RWTexture2D<float4> TileStatsRW : register(u0);

#define VRS_THREADS (VRS_TILE * VRS_TILE)

groupshared float gsLuma[VRS_TILE][VRS_TILE];
groupshared float3 gsNormal[VRS_TILE][VRS_TILE];
groupshared float4 gsColour[VRS_THREADS];     // sum dx^2, sum dy^2, sum luma, valid pixels
groupshared float4 gsNormalErr[VRS_THREADS];  // sum normal dx^2, sum normal dy^2, x pairs, y pairs
groupshared float2 gsMotion[VRS_THREADS];     // sum motion (pixels)

[numthreads(VRS_TILE, VRS_TILE, 1)] void main(uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID, uint groupIndex : SV_GroupIndex) {
	const uint2 pixel = groupId.xy * VRS_TILE + groupThreadId.xy;
	const bool valid = all(pixel < RenderSize);

	// Pixel spacing of one shading result in this tile (1, 2 or 4 per axis). Draws the table
	// keeps finer (ground, blended, cut-outs capped at 2x2) are measured at this spacing too,
	// which only under-reads them; their own rate does not depend on this tile's.
	uint2 stride = 1;
	if (RateAwareAnalysis != 0 && AppliedRatesValid != 0)
		stride = 1u << RateLog2(RateImage[groupId.xy]);

	float luma = 0.0;
	float3 normal = 0.0;
	float2 motion = 0.0;
	if (valid) {
		const float3 colour = ColorTexture[pixel].rgb;
		luma = dot(colour, float3(0.2126, 0.7152, 0.0722));
		// A single NaN/Inf pixel would poison the whole tile's sums; treat it as black.
		luma = isfinite(luma) ? max(luma, 0.0) : 0.0;
		normal = GBuffer::DecodeNormal(NormalRoughnessTexture[pixel].xy);
		motion = MotionVectorTexture[pixel] * float2(RenderSize);
		motion = isfinite(motion) ? motion : 0.0;
	}
	gsLuma[groupThreadId.y][groupThreadId.x] = luma;
	gsNormal[groupThreadId.y][groupThreadId.x] = normal;
	GroupMemoryBarrierWithGroupSync();

	float4 colourSums = float4(0.0, 0.0, luma, valid ? 1.0 : 0.0);
	float4 normalSums = 0.0;
	if (valid) {
		// Pairs are only formed inside the tile: that is the neighbourhood a coarse pixel
		// can actually merge, and it keeps the tile independent of its neighbours. Dividing
		// by the spacing turns a difference over `stride` pixels into a per-pixel step.
		if (groupThreadId.x + stride.x < VRS_TILE && pixel.x + stride.x < RenderSize.x) {
			const float d = (gsLuma[groupThreadId.y][groupThreadId.x + stride.x] - luma) / stride.x;
			const float3 dn = (gsNormal[groupThreadId.y][groupThreadId.x + stride.x] - normal) / stride.x;
			colourSums.x = d * d;
			normalSums.x = dot(dn, dn);
			normalSums.z = 1.0;
		}
		if (groupThreadId.y + stride.y < VRS_TILE && pixel.y + stride.y < RenderSize.y) {
			const float d = (gsLuma[groupThreadId.y + stride.y][groupThreadId.x] - luma) / stride.y;
			const float3 dn = (gsNormal[groupThreadId.y + stride.y][groupThreadId.x] - normal) / stride.y;
			colourSums.y = d * d;
			normalSums.y = dot(dn, dn);
			normalSums.w = 1.0;
		}
	}
	gsColour[groupIndex] = colourSums;
	gsNormalErr[groupIndex] = normalSums;
	gsMotion[groupIndex] = motion;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint s = VRS_THREADS / 2; s > 0; s >>= 1)
	{
		if (groupIndex < s) {
			gsColour[groupIndex] += gsColour[groupIndex + s];
			gsNormalErr[groupIndex] += gsNormalErr[groupIndex + s];
			gsMotion[groupIndex] += gsMotion[groupIndex + s];
		}
		GroupMemoryBarrierWithGroupSync();
	}

	if (groupIndex == 0) {
		const float4 c = gsColour[0];
		const float4 n = gsNormalErr[0];
		const float pixels = max(c.w, 1.0);
		const float denominator = c.z / pixels + EnvLuminance;

		// No pairs along an axis (a 1-pixel sliver at the render edge) means nothing can be
		// measured there; report a large error so that axis stays at full rate.
		float errorX = n.z > 0.0 ? sqrt(c.x / n.z) / denominator : 1e3;
		float errorY = n.w > 0.0 ? sqrt(c.y / n.w) / denominator : 1e3;

		if (NormalWeight > 0.0) {
			// Chord length between unit normals, which is ~the angle in radians for small angles.
			errorX = max(errorX, n.z > 0.0 ? sqrt(n.x / n.z) * NormalWeight : 0.0);
			errorY = max(errorY, n.w > 0.0 ? sqrt(n.y / n.w) * NormalWeight : 0.0);
		}

		TileStatsRW[groupId.xy] = float4(errorX, errorY, gsMotion[0] / pixels);
	}
}
