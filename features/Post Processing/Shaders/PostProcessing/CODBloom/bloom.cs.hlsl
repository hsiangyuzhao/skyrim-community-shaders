/// By ProfJack/五脚猫, 2024-2-28 UTC
/// ref:
/// http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare

#include "PostProcessing/common.hlsli"

Texture2D<float4> TexColor : register(t0);
Texture2D<float4> TexBloomIn : register(t1);

RWTexture2D<float4> RWTexBloomOut : register(u0);

cbuffer BloomCB : register(b1)
{
	// threshold
	float Threshold : packoffset(c0.x);
	// upsample
	float UpsampleRadius : packoffset(c0.y);
	float UpsampleMult : packoffset(c0.z);  // in composite: bloom mult
	float CurrentMipMult : packoffset(c0.w);
};

SamplerState SampColor : register(s0);

bool3 IsNaN(float3 x)
{
	return !(x < 0.f || x > 0.f || x == 0.f);
}

float3 Sanitise(float3 v)
{
	bool3 err = IsNaN(v) || (v < 0);
	v.x = err.x ? 0 : v.x;
	v.y = err.y ? 0 : v.y;
	v.z = err.z ? 0 : v.z;
	return v;
}

float3 ThresholdColor(float3 col, float threshold)
{
	float luma = Color::RGBToLuminance(col);
	if (luma < 1e-3)
		return 0;
	return col * (max(0, luma - threshold) / luma);
}

// (batch 23) Four bilinear taps in place of the nine-tap tent, which is the standard
// bilinear-tent upsample. The two heaviest passes in this effect are the ones that call this:
// CS_Composite runs it at full resolution and accounts for roughly half of the whole effect's
// memory traffic, and the mip-1 CS_Upsample for another ninth of it, so 9 fetches to 4 is the
// single largest saving available here.
//
// Not an identity. The nine-tap samples the bilinearly filtered source at spacing `radius` and
// four taps at half that spacing give a slightly narrower kernel; they would coincide only if
// every centre tap landed exactly on a source texel centre, which depends on thread parity and
// so does not hold. Bloom does not need kernel exactness -- and Upsampling Radius is right
// there if the narrower support reads as too tight.
float4 UpsampleCOD(Texture2D tex, float2 uv, float2 radius)
{
	float4 retval = 0;
	[unroll] for (int x = -1; x <= 1; x += 2)
		[unroll] for (int y = -1; y <= 1; y += 2)
			retval += 0.25 * tex.SampleLevel(SampColor, uv + float2(x, y) * 0.5 * radius, 0);
	return retval;
}

// (batch 23) The first-mip downsample with the threshold folded in, so the separate full-screen
// threshold pass disappears: it read the whole frame and wrote a whole full-resolution mip that
// existed only to be read once by this kernel.
//
// Mirrors DownsampleCODFirstMip in PostProcessing/common.hlsli, thresholding each fetch instead
// of reading a pre-thresholded surface. That reverses the order of two operations -- it is now
// threshold(bilinear(colour)) rather than bilinear(threshold(colour)) -- which is not the same
// value where a fetch straddles the threshold. Sanitise still runs per fetch, so the NaN and
// negative guard the threshold pass provided is not lost.
float4 DownsampleFirstMipThresholded(Texture2D tex, float2 uv, float2 out_px_size, float threshold)
{
	int x, y;

	float4 retval = 0;
	float4 fetches2x2[4];
	float4 fetches3x3[9];

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			fetches2x2[x * 2 + y] = float4(ThresholdColor(Sanitise(tex.SampleLevel(SampColor, uv + (int2(x, y) * 2 - 1) * out_px_size, 0).rgb), threshold), 1);
	[unroll] for (x = 0; x < 3; ++x)
		[unroll] for (y = 0; y < 3; ++y)
			fetches3x3[x * 3 + y] = float4(ThresholdColor(Sanitise(tex.SampleLevel(SampColor, uv + (int2(x, y) - 1) * 2 * out_px_size, 0).rgb), threshold), 1);

	retval += 0.5 * KarisAverage(fetches2x2[0], fetches2x2[1], fetches2x2[2], fetches2x2[3]);

	[unroll] for (x = 0; x < 2; ++x)
		[unroll] for (y = 0; y < 2; ++y)
			retval += 0.125 * KarisAverage(fetches3x3[x * 3 + y], fetches3x3[(x + 1) * 3 + y], fetches3x3[x * 3 + y + 1], fetches3x3[(x + 1) * 3 + y + 1]);

	return retval;
}

// (batch 23) 8x8 rather than 32x32. A 1024-thread group is the D3D11 maximum and the worst
// case for how many groups a multiprocessor can hold at once, and these are plain
// bandwidth-bound passes with nothing to gain from a group that large. The coarse mips suffered
// twice over: the dispatch is sized in whole groups, so a 32x32 granularity rounded a mip only
// a few dozen pixels across up to thousands of threads, nearly all of them idle.
[numthreads(8, 8, 1)] void CS_Downsample(uint2 tid
										 : SV_DispatchThreadID) {
	uint2 dims;
	RWTexBloomOut.GetDimensions(dims.x, dims.y);

	float2 px_size = rcp(dims);
	float2 uv = (tid + .5) * px_size;

#ifdef FIRST_MIP
	// Reads the frame directly and thresholds as it fetches -- see
	// DownsampleFirstMipThresholded. There is no longer a thresholded full-resolution mip 0 to
	// read from; mip 0 is written once, at the end, by CS_Composite.
	float3 col = DownsampleFirstMipThresholded(TexColor, uv, px_size, Threshold.x).rgb;
#else
	float3 col = DownsampleCOD(TexBloomIn, SampColor, uv, px_size).rgb;
#endif
	RWTexBloomOut[tid] = float4(col, 1);
};

[numthreads(8, 8, 1)] void CS_Upsample(uint2 tid
										 : SV_DispatchThreadID) {
	uint2 dims;
	RWTexBloomOut.GetDimensions(dims.x, dims.y);

	float2 px_size = rcp(dims);
	float2 uv = (tid + .5) * px_size;

	float3 col = RWTexBloomOut[tid].rgb * CurrentMipMult + UpsampleCOD(TexBloomIn, uv, px_size * UpsampleRadius).rgb * UpsampleMult;
	RWTexBloomOut[tid] = float4(col, 1);
};

[numthreads(8, 8, 1)] void CS_Composite(uint2 tid
										  : SV_DispatchThreadID) {
	uint2 dims;
	RWTexBloomOut.GetDimensions(dims.x, dims.y);

	float2 px_size = rcp(dims);
	float2 uv = (tid + .5) * px_size;

	float3 col = TexColor[tid].rgb + UpsampleCOD(TexBloomIn, uv, px_size * UpsampleRadius).rgb * UpsampleMult;

	RWTexBloomOut[tid] = float4(col, 1);
};