#ifndef __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_CS_COMMON_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_VOLUMETRIC_CS_COMMON_HLSLI__

// (batch 38, A1) Constant buffer and froxel helpers of the volumetric fog compute passes.
// Upstream 08faccd51 with the VR handling of 87d4e6c2a restored (upstream c9d7c0524 removed
// it): in VR the volume spans the double-wide buffer, the left half is eye 0 and the right
// half eye 1, and every cell is reconstructed with its own eye's inverse view-projection.

#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

cbuffer VolumetricFogCB : register(b0)
{
	uint4 VolumetricFogGridSizeAndFlags;         // near volume
	float4 VolumetricFogInvGridSizeAndNearFade;  // near volume
	float4 VolumetricFogGridZParams;             // near volume: scale, offset, distribution
	row_major float4x4 VolumetricFogClipToWorld[2];
	float4 VolumetricFogFrameJitterOffsets[16];
	float4 VolumetricFogHistoryParameters;  // x = history weight, y = samples on a history miss
	float4 VolumetricFogJitterParameters;   // x = per-cell sample jitter, y = frame index mod 8
	uint4 VolumetricFogFarGridSizeAndFlags;  // far volume (coarser lattice)
	float4 VolumetricFogFarInvGridSizeAndNearFade;
	float4 VolumetricFogFarGridZParams;
	float4 VolumetricFogFarRange;           // x = far volume start depth, y = far volume end depth
	float4 VolumetricFogAlbedo;             // rgb albedo, a = scale
	float4 VolumetricFogEmissive;           // rgb, a = scale
	float4 VolumetricFogLighting;           // x = sun, y = sky, z = local lights intensity, w = phase g
	float4 VolumetricFogMisc;               // x = shadow bias, y = extinction scale, z = noise scale, w = noise threshold
	float4 VolumetricFogNoiseVelocity;      // xyz, noise cells per second
};

#if defined(VOLUMETRIC_FOG_FAR_GRID)
// The far volume variant reads its own grid parameters from the same constant buffer.
#	define VOLUMETRIC_FOG_FLAGS VolumetricFogFarGridSizeAndFlags.w
#	define VOLUMETRIC_FOG_GRID_SIZE VolumetricFogFarGridSizeAndFlags.xyz
#	define VOLUMETRIC_FOG_INV_GRID_SIZE VolumetricFogFarInvGridSizeAndNearFade.xyz
#	define VOLUMETRIC_FOG_NEAR_FADE_INV VolumetricFogFarInvGridSizeAndNearFade.w
#	define VOLUMETRIC_FOG_GRID_Z_PARAMS VolumetricFogFarGridZParams.xyz
#else
#	define VOLUMETRIC_FOG_FLAGS VolumetricFogGridSizeAndFlags.w
#	define VOLUMETRIC_FOG_GRID_SIZE VolumetricFogGridSizeAndFlags.xyz
#	define VOLUMETRIC_FOG_INV_GRID_SIZE VolumetricFogInvGridSizeAndNearFade.xyz
#	define VOLUMETRIC_FOG_NEAR_FADE_INV VolumetricFogInvGridSizeAndNearFade.w
#	define VOLUMETRIC_FOG_GRID_Z_PARAMS VolumetricFogGridZParams.xyz
#endif

#define VolumetricFogGridSize VOLUMETRIC_FOG_GRID_SIZE
#define VolumetricFogHasDirectionalShadowMap ((VOLUMETRIC_FOG_FLAGS & 1u) != 0u)
#define VolumetricFogHasConservativeDepth ((VOLUMETRIC_FOG_FLAGS & 2u) != 0u)
#define VolumetricFogHasIBL ((VOLUMETRIC_FOG_FLAGS & 4u) != 0u)
#define VolumetricFogHasSkylighting ((VOLUMETRIC_FOG_FLAGS & 8u) != 0u)
#define VolumetricFogHasPrevConservativeDepth ((VOLUMETRIC_FOG_FLAGS & 16u) != 0u)
#define VolumetricFogHasLocalLights ((VOLUMETRIC_FOG_FLAGS & 32u) != 0u)
#define VolumetricFogInvGridSize VOLUMETRIC_FOG_INV_GRID_SIZE
#define VolumetricFogNearFadeInDistanceInv VOLUMETRIC_FOG_NEAR_FADE_INV
#define VolumetricFogHistoryWeight VolumetricFogHistoryParameters.x
#define VolumetricFogHistoryMissSampleCount max(1u, min(16u, (uint)(VolumetricFogHistoryParameters.y + 0.5f)))
#define VolumetricFogSampleJitterMultiplier VolumetricFogJitterParameters.x
#define VolumetricFogStateFrameIndexMod8 ((uint)(VolumetricFogJitterParameters.y + 0.5f))

#include "ExponentialHeightFog/VolumetricFogCommon.hlsli"

namespace ExponentialHeightFog
{
	bool IsInsideVolumetricGrid(uint3 coord)
	{
		return all(coord < VolumetricFogGridSize);
	}

	float ComputeVolumetricSliceDepth(float slice)
	{
		return VolumetricSliceToDepth(slice, VOLUMETRIC_FOG_GRID_Z_PARAMS);
	}

	float ComputeVolumetricNormalizedSlice(float viewDepth)
	{
		return VolumetricDepthToNormalizedSlice(viewDepth, VOLUMETRIC_FOG_GRID_Z_PARAMS, float(VolumetricFogGridSize.z));
	}

	/// Eye of a volume UV (always 0 outside VR).
	uint GetVolumeEyeIndex(float2 volumeUV)
	{
		return Stereo::GetEyeIndexFromTexCoord(volumeUV);
	}

	/// Camera-relative (to the cell's eye) position of a cell. volumeUV is in the stereo
	/// layout; cellOffset in [0,1]^3 picks a point inside the cell.
	float3 ComputeCellWorldPosition(uint3 coord, float3 cellOffset, out uint eyeIndex, out float viewDepth)
	{
		float2 volumeUV = (float2(coord.xy) + cellOffset.xy) * VolumetricFogInvGridSize.xy;
		eyeIndex = GetVolumeEyeIndex((float2(coord.xy) + 0.5f) * VolumetricFogInvGridSize.xy);
		float2 eyeUV = Stereo::ConvertFromStereoUV(volumeUV, eyeIndex);

		viewDepth = ComputeVolumetricSliceDepth(max(float(coord.z) + cellOffset.z, 0.0f));

		float2 ndc = eyeUV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f);
		float deviceZ = (SharedData::CameraData.x - SharedData::CameraData.w / viewDepth) / SharedData::CameraData.z;
		float4 worldPosition = mul(VolumetricFogClipToWorld[eyeIndex], float4(ndc, deviceZ, 1.0f));
		return worldPosition.xyz / worldPosition.w;
	}

	// Deterministic 3D hash for value noise in [0,1].
	float HashValueNoise3D(float3 p)
	{
		p = frac(p * 0.1031f);
		p += dot(p, p.zyx + 31.32f);
		return frac((p.x + p.y) * p.z);
	}

	// Smooth trilinear value noise in [0,1].
	float ValueNoise3D(float3 p)
	{
		const float3 cell = floor(p);
		const float3 f = frac(p);
		const float3 u = f * f * (3.0f - 2.0f * f);

		const float n000 = HashValueNoise3D(cell);
		const float n100 = HashValueNoise3D(cell + float3(1, 0, 0));
		const float n010 = HashValueNoise3D(cell + float3(0, 1, 0));
		const float n110 = HashValueNoise3D(cell + float3(1, 1, 0));
		const float n001 = HashValueNoise3D(cell + float3(0, 0, 1));
		const float n101 = HashValueNoise3D(cell + float3(1, 0, 1));
		const float n011 = HashValueNoise3D(cell + float3(0, 1, 1));
		const float n111 = HashValueNoise3D(cell + float3(1, 1, 1));

		return lerp(
			lerp(lerp(n000, n100, u.x), lerp(n010, n110, u.x), u.y),
			lerp(lerp(n001, n101, u.x), lerp(n011, n111, u.x), u.y),
			u.z);
	}

	// Multiplicative density modulation from a world-space value noise field with a soft
	// cutoff (upstream #2831). Returns 1 when the noise is off.
	float EvaluateHeightFogNoiseModulation(float3 absolutePositionWS)
	{
		const float noiseScale = VolumetricFogMisc.z;
		if (noiseScale <= 0.0f)
			return 1.0f;

		const float3 noisePos = absolutePositionWS * noiseScale - VolumetricFogNoiseVelocity.xyz * SharedData::Timer;

		float noise = ValueNoise3D(noisePos);
		noise = lerp(noise, ValueNoise3D(noisePos * 4.17f + 71.3f), 0.35f);  // detail octave

		const float threshold = saturate(VolumetricFogMisc.w);
		float t = saturate((noise - threshold) / max(1.0f - threshold, 1e-4f));
		return t * t * (3.0f - 2.0f * t);
	}

	/// Extinction of the height fog medium at a camera-relative position (two stacked layers).
	float EvaluateHeightFogExtinction(float3 positionWS, float3 cameraWS)
	{
		const float fogDensity = SharedData::exponentialHeightFogSettings.fogDensity * 0.001f;
		const float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
		const float fogDensity2 = SharedData::exponentialHeightFogSettings.fogDensity2 * 0.001f;
		const float fogHeightFalloff2 = SharedData::exponentialHeightFogSettings.fogHeightFalloff2 * 0.001f;
		const float3 absolutePositionWS = positionWS + cameraWS;
		const float worldHeight = absolutePositionWS.z;
		const float exponent = fogHeightFalloff * max(worldHeight - SharedData::exponentialHeightFogSettings.fogHeight, 0.0f);
		const float exponent2 = fogHeightFalloff2 * max(worldHeight - SharedData::exponentialHeightFogSettings.fogHeight2, 0.0f);
		const float localDensity =
			(fogDensity * exp2(-exponent) + fogDensity2 * exp2(-exponent2)) *
			EvaluateHeightFogNoiseModulation(absolutePositionWS);
		return max(localDensity * VolumetricFogMisc.y * 0.5f, 0.0f);
	}
}

#endif
