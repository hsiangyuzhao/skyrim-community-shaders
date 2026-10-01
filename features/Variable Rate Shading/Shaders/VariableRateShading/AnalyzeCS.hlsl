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
// The result feeds BuildRateImageCS at the start of the NEXT frame's opaque pass, which
// reprojects it with the motion stored here.

#include "Common/GBuffer.hlsli"
#include "VariableRateShading/Common.hlsli"

Texture2D<float4> ColorTexture : register(t0);            // main scene colour after the deferred composite
Texture2D<float2> MotionVectorTexture : register(t1);     // prevUV - currUV, render-resolution UV units
Texture2D<float4> NormalRoughnessTexture : register(t2);  // CS G-buffer: octahedral normal in xy

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
		// can actually merge, and it keeps the tile independent of its neighbours.
		if (groupThreadId.x + 1 < VRS_TILE && pixel.x + 1 < RenderSize.x) {
			const float d = gsLuma[groupThreadId.y][groupThreadId.x + 1] - luma;
			const float3 dn = gsNormal[groupThreadId.y][groupThreadId.x + 1] - normal;
			colourSums.x = d * d;
			normalSums.x = dot(dn, dn);
			normalSums.z = 1.0;
		}
		if (groupThreadId.y + 1 < VRS_TILE && pixel.y + 1 < RenderSize.y) {
			const float d = gsLuma[groupThreadId.y + 1][groupThreadId.x] - luma;
			const float3 dn = gsNormal[groupThreadId.y + 1][groupThreadId.x] - normal;
			colourSums.y = d * d;
			normalSums.y = dot(dn, dn);
			normalSums.w = 1.0;
		}
	}
	gsColour[groupIndex] = colourSums;
	gsNormalErr[groupIndex] = normalSums;
	gsMotion[groupIndex] = motion;
	GroupMemoryBarrierWithGroupSync();

	[unroll] for (uint stride = VRS_THREADS / 2; stride > 0; stride >>= 1)
	{
		if (groupIndex < stride) {
			gsColour[groupIndex] += gsColour[groupIndex + stride];
			gsNormalErr[groupIndex] += gsNormalErr[groupIndex + stride];
			gsMotion[groupIndex] += gsMotion[groupIndex + stride];
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
