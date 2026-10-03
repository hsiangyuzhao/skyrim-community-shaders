// (batch 6) Stage 3 of 3 of the ambient-reinjection confidence filter: joint bilateral
// upsample of the filtered quarter-resolution confidence back to full resolution, guided by
// the full-resolution depth and normal. Writes texSSRTDiffuseConfidenceSmooth, i.e. exactly
// the surface the depth-aware 7x7 window in ssrt_diffuse_composite.hlsl used to write, so
// DeferredCompositeCS is unchanged and sees the same semantics -- hit coverage in [0,1].
//
// Nothing here reads a previous frame. See ssrt_conf_filter.hlsli for why that is permanent.
#include "ScreenSpaceRayTracing/ssrt_conf_filter.hlsli"

// (batch 36g) SSRT_CONF_PATTERN: the degeneracy fallback below must not read a pixel that did not
// trace diffuse this frame (36b's fallback read last frame's value there). See the end of main.
#if defined(SSRT_CONF_PATTERN)
#	include "ScreenSpaceRayTracing/ssrt_b36g.hlsli"
#endif

// The raw per-pixel confidence is bound for one reason: it is what a pixel with no usable
// low-resolution neighbour falls back to. See the degeneracy rule at the bottom.
Texture2D<float> SSRTConfidenceTexture : register(t0);
Texture2D<float> DepthTexture : register(t1);
// NormalRoughnessTexture is at t2, from ssrt_common.hlsli.
Texture2D<float> ConfidenceLoTexture : register(t3);
Texture2D<float> DepthLoTexture : register(t4);
Texture2D<float4> NormalLoTexture : register(t5);

RWTexture2D<float> SSRTConfidenceSmoothRW : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID)
{
	// The same guard and the same extent the 7x7 window used, so the written region is
	// texel-for-texel the region the old path wrote: SharedData::BufferDim.xy is the full
	// allocation, the dispatch is sized to the dynamic-resolution sub-rect rounded up to the
	// group size, and the handful of lanes in between still publish a value.
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	const int2 hiSize = int2(SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy);
	const int2 loSize = (hiSize + int2(1, 1)) >> 1;
	const int2 loMaxCoord = max(loSize - int2(1, 1), int2(0, 0));

	const int2 pixel = clamp(int2(dispatchID.xy), int2(0, 0), max(hiSize - int2(1, 1), int2(0, 0)));

	const float centreDepth = SharedData::GetScreenDepth(DepthTexture[pixel]);
	float3 centreNormal;
	float centreRoughness;
	GetNormalRoughness(uint2(pixel), centreNormal, centreRoughness);

	// Bilinear footprint of this pixel in the quarter-resolution grid. Low-resolution texel l
	// covers full-resolution pixels 2l and 2l+1, so its centre sits at full-resolution
	// coordinate 2l + 0.5, and the fractional position of pixel p is p/2 - 0.25. That is
	// always 0.25 or 0.75, never 0 or 1, so no bilinear weight is ever zero -- which matters,
	// because a zero-weight tap could not rescue a pixel whose only geometrically valid
	// neighbour it was.
	const float2 loCoord = (float2(pixel) + 0.5f) * 0.5f - 0.5f;
	const int2 loBase = int2(floor(loCoord));
	const float2 loFrac = loCoord - float2(loBase);

	float sum = 0.0f;
	float weightSum = 0.0f;

	[unroll] for (int tap = 0; tap < 4; tap++)
	{
		const int2 offset = int2(tap & 1, tap >> 1);
		const int2 loPixel = clamp(loBase + offset, int2(0, 0), loMaxCoord);

		const float bilinearWeight =
			(offset.x != 0 ? loFrac.x : 1.0f - loFrac.x) *
			(offset.y != 0 ? loFrac.y : 1.0f - loFrac.y);

		const float tapDepth = DepthLoTexture[loPixel];
		const float3 tapNormal = NormalLoTexture[loPixel].xyz;

		const float weight = SSRTConfAccept(centreDepth, centreNormal, tapDepth, tapNormal, SSRT_CONF_US_DEPTH_TOL) ? bilinearWeight : 0.0f;

		sum += ConfidenceLoTexture[loPixel] * weight;
		weightSum += weight;
	}

	// The degeneracy rule, and the one place this chain is allowed to be noisy. If not one of
	// the four low-resolution neighbours passes the geometric test -- a one-pixel-wide fin, a
	// pixel isolated between two silhouettes, a depth the G-buffer disagrees with -- the pixel
	// publishes its own *unfiltered* confidence. That value is noisy but it is correct for this
	// pixel, which is strictly better than the alternative: borrowing a neighbour's coverage
	// across a geometry boundary would put a halo of wrong ambient around every thin object,
	// and unlike noise a halo does not average away.
	//
	// weightSum can only reach zero through rejection, never through the weights themselves;
	// see the note on the fractional position above.
#if defined(SSRT_CONF_PATTERN)
	// A pixel that traced diffuse this frame falls back to its own value, as before. One that did
	// not takes the mean of its geometrically accepted, traced 4-neighbours (under A and C all four
	// traced); with none of those, 0 -- "nothing resolved", i.e. the vanilla ambient stays.
	float rawConfidence = 0.0f;
	if (B36G_DiffuseTraced(uint2(pixel))) {
		rawConfidence = SSRTConfidenceTexture[pixel];
	} else {
		const int2 hiMax = max(hiSize - int2(1, 1), int2(0, 0));
		float nSum = 0.0f;
		float nCount = 0.0f;
		[unroll] for (int k = 0; k < 4; k++)
		{
			const int2 n = pixel + int2(k == 0 ? -1 : (k == 1 ? 1 : 0), k == 2 ? -1 : (k == 3 ? 1 : 0));
			if (any(n < int2(0, 0)) || any(n > hiMax) || !B36G_DiffuseTraced(uint2(n)))
				continue;
			float3 nNormal;
			float nRoughness;
			GetNormalRoughness(uint2(n), nNormal, nRoughness);
			if (!SSRTConfAccept(centreDepth, centreNormal, SharedData::GetScreenDepth(DepthTexture[n]), nNormal, SSRT_CONF_US_DEPTH_TOL))
				continue;
			nSum += SSRTConfidenceTexture[n];
			nCount += 1.0f;
		}
		rawConfidence = nCount > 0.0f ? nSum / nCount : 0.0f;
	}
#else
	const float rawConfidence = SSRTConfidenceTexture[pixel];
#endif
	SSRTConfidenceSmoothRW[dispatchID.xy] = weightSum > 0.0f ? saturate(sum / weightSum) : saturate(rawConfidence);
}
