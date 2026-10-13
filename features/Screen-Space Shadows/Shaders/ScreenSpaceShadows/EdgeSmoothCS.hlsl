// (45 SSS) Edge Smoothing: a 3x3 depth-aware blur of the Screen Space Shadows mask.
// Per frame only -- no history, so nothing smears or lags when the camera or the sun moves
// (specs/ssrt-denoise-hard-constraints.md rule 3: visibility terms are per-frame + spatial).
// It takes the stair-steps and the DLSS-jitter shimmer off the 1-pixel shadow edges.

#if defined(TERRAIN_BLENDING)
Texture2D<float> DepthTexture : register(t0);  // R32_FLOAT blended depth
#else
Texture2D<unorm float> DepthTexture : register(t0);  // R24_UNORM_X8 game depth
#endif
Texture2D<unorm float> RawShadowTexture : register(t1);  // ray march output (R8_UNORM)
RWTexture2D<unorm float> OutputTexture : register(u0);   // ScreenSpaceShadows texture (R8_UNORM)

#include "ScreenSpaceShadows/RaymarchCB.hlsli"

float LinearDepth(float depth)
{
	return CameraData.w / (-depth * CameraData.z + CameraData.x);
}

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	const int2 size = (int2)ProjInfo.zw;
	if (any((int2)dtid >= size))
		return;

	const float centerShadow = RawShadowTexture.Load(int3(dtid, 0));
	const float centerDepth = DepthTexture.Load(int3(dtid, 0));
	if (centerDepth <= 0.0 || centerDepth >= 1.0) {
		OutputTexture[dtid] = centerShadow;
		return;
	}

	const float centerZ = LinearDepth(centerDepth);
	// Neighbours more than ~3% of the distance in front of / behind this pixel are another surface.
	const float invTolerance = 1.0 / (0.03 * centerZ);

	float sum = centerShadow * 4.0;
	float weightSum = 4.0;

	[unroll] for (int y = -1; y <= 1; y++)
	{
		[unroll] for (int x = -1; x <= 1; x++)
		{
			if (x == 0 && y == 0)
				continue;
			const int2 p = clamp((int2)dtid + int2(x, y), 0, size - 1);
			const float d = DepthTexture.Load(int3(p, 0));
			const float s = RawShadowTexture.Load(int3(p, 0));
			const float kernel = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0);
			const float w = kernel * saturate(1.0 - abs(LinearDepth(d) - centerZ) * invTolerance);
			sum += s * w;
			weightSum += w;
		}
	}

	OutputTexture[dtid] = sum / weightSum;
}
