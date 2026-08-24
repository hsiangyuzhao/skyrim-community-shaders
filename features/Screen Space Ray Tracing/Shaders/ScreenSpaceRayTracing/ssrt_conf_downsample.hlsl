// (batch 6) Stage 1 of 3 of the ambient-reinjection confidence filter: a depth- and
// normal-aware 2x2 average of the raw per-pixel hit confidence onto a half-width,
// half-height grid, publishing the low-resolution geometry guide the two stages after it
// need. See ssrt_conf_filter.hlsli for why the chain exists and why it is purely spatial.
//
// The 2x2 average is not an approximation of anything: confidence is a coverage fraction
// built from independent ray directions per pixel, so averaging four pixels is averaging
// four times the rays. This stage alone halves the noise, and it is what makes the wide
// kernel in stage 2 cheaper than the 7x7 window the chain replaces rather than dearer.
#include "ScreenSpaceRayTracing/ssrt_conf_filter.hlsli"

// Raw per-pixel hit confidence straight from the ray march, and the full-resolution depth
// buffer. NormalRoughnessTexture comes from ssrt_common.hlsli at t2, which is where every
// other pass in this feature reads it.
Texture2D<float> SSRTConfidenceTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);

// Quarter-resolution confidence, and the guide the blur and upsample read: linear view depth
// and the averaged view-space normal. Two surfaces rather than one packed RGBA16F, because
// R32_FLOAT costs the depth nothing in precision (distant LOD terrain runs past fp16's 65504
// ceiling, and the plane gradient in stage 2 is a difference of near-equal reciprocals) while
// RGBA8_SNORM carries the normal to well under half a degree, which is two orders of magnitude
// finer than the 45 degree gate that reads it. Together they are 8 bytes per quarter-resolution
// texel, i.e. 2 bytes per full-resolution pixel.
RWTexture2D<float> ConfidenceLoRW : register(u0);
RWTexture2D<float> DepthLoRW : register(u1);
RWTexture2D<float4> NormalLoRW : register(u2);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	// The render extent, matching every other pass in this feature: the dispatch, the ray
	// march's traversal grid and the pyramid's valid area all agree on it.
	const int2 hiSize = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
	// Round up, so the last odd column/row of an odd render extent still has a low-resolution
	// texel covering it. Every consumer derives its own copy of this from the same two
	// values, so no constant buffer is needed to keep the stages in step.
	const int2 loSize = (hiSize + int2(1, 1)) >> 1;

	if (any(int2(dispatchID.xy) >= loSize))
		return;

	const int2 blockOrigin = int2(dispatchID.xy) << 1;
	const int2 maxCoord = max(hiSize - int2(1, 1), int2(0, 0));

	// The block's top-left pixel is the reference every tap is judged against. A fixed corner
	// rather than a computed centre because a 2x2 block has no centre texel, and picking one
	// deterministically is what makes the degenerate case below well defined.
	const int2 referencePixel = min(blockOrigin, maxCoord);
	const float referenceDepth = SharedData::GetScreenDepth(DepthTexture[referencePixel]);
	float3 referenceNormal;
	float referenceRoughness;
	GetNormalRoughness(uint2(referencePixel), referenceNormal, referenceRoughness);

	float confidenceSum = 0.0f;
	float depthSum = 0.0f;
	float3 normalSum = 0.0f;
	float weightSum = 0.0f;

	[unroll] for (int tap = 0; tap < 4; tap++)
	{
		const int2 pixel = min(blockOrigin + int2(tap & 1, tap >> 1), maxCoord);
		const float tapDepth = SharedData::GetScreenDepth(DepthTexture[pixel]);
		float3 tapNormal;
		float tapRoughness;
		GetNormalRoughness(uint2(pixel), tapNormal, tapRoughness);

		const float weight = SSRTConfAccept(referenceDepth, referenceNormal, tapDepth, tapNormal, SSRT_CONF_DS_DEPTH_TOL) ? 1.0f : 0.0f;

		confidenceSum += SSRTConfidenceTexture[pixel] * weight;
		depthSum += tapDepth * weight;
		normalSum += tapNormal * weight;
		weightSum += weight;
	}

	// tap 0 is the reference itself, so it passes its own test (relative depth 0, cosine 1)
	// and weightSum is normally at least 1. The fallback is the spec'd degenerate rule made
	// explicit rather than left to that argument: if no source pixel agrees with the
	// reference -- which needs a non-finite depth or a NaN normal, since nothing else can make
	// a texel disagree with itself -- the block reports the reference pixel alone. Noisier,
	// but never a value borrowed from across a geometry boundary.
	const bool degenerate = !(weightSum > 0.0f);
	const float invWeight = degenerate ? 1.0f : rcp(weightSum);

	ConfidenceLoRW[dispatchID.xy] = degenerate ? saturate(SSRTConfidenceTexture[referencePixel]) : saturate(confidenceSum * invWeight);
	DepthLoRW[dispatchID.xy] = degenerate ? referenceDepth : depthSum * invWeight;
	NormalLoRW[dispatchID.xy] = float4(degenerate ? referenceNormal : SSRTConfNormaliseGuide(normalSum, referenceNormal), 0.0f);
}
