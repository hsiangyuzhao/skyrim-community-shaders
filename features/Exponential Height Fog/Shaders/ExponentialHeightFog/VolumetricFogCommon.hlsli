#ifndef __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_COMMON_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_COMMON_HLSLI__

// (batch 38, A1) Volumetric fog helpers shared by the froxel compute passes and the pixel
// shaders that look the volumes up. Ported from upstream 87d4e6c2a (#2361) and 08faccd51
// (#2831). Unlike upstream, the depth distribution of each volume is computed once on the
// CPU (double precision) and handed over as zParams = (scale, offset, distribution): the
// compute passes get it in their own constant buffer, pixel shaders in
// SharedData::volumetricFogSettings. Nothing here reads a volumetric setting directly.

#include "Common/Math.hlsli"

namespace ExponentialHeightFog
{
	float HenyeyGreenstein(float cosTheta, float g)
	{
		float g2 = g * g;
		float denom = 1.0f + g2 - 2.0f * g * cosTheta;
		return (1.0f - g2) / (4.0f * Math::PI * pow(max(denom, 1e-5f), 1.5f));
	}

	/// View depth at a (fractional) slice index. zParams = (scale, offset, distribution).
	float VolumetricSliceToDepth(float slice, float3 zParams)
	{
		float sliceExp = exp2(min(slice / max(zParams.z, 1e-4f), 120.0f));
		return (sliceExp - zParams.y) / max(zParams.x, 1e-20f);
	}

	/// Normalized [0,1] slice coordinate of a view depth.
	float VolumetricDepthToNormalizedSlice(float viewDepth, float3 zParams, float sliceCount)
	{
		return log2(max(viewDepth * zParams.x + zParams.y, 1e-6f)) * zParams.z / max(sliceCount, 1.0f);
	}
}

#endif
