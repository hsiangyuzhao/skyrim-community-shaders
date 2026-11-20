// (batch 38, A1) Front-to-back integration of in-scattered light and transmittance along
// each froxel column (upstream 87d4e6c2a + 08faccd51 far-volume start).
Texture3D<float4> LightScattering : register(t0);
RWTexture3D<float4> IntegratedLightScattering : register(u0);
// (49i) S6 "Far Fog Continues the Near Fog": the medium's own radiance at the volume's far end, per froxel column: scattering / extinction of
// the last layer in fp32 (what the integrated volume approaches; 49h took it as the difference of the last two fp16 slices: banding),
// a = a smooth weight on that layer's optical depth (0 without medium there, 1 from kEndOpticalDepthFull up). Read by the pixel shaders at
// t24 (ExponentialHeightFog.hlsli GetVolumeEndColor). Unbound (null UAV): the write is dropped.
RWTexture2D<float4> VolumeEndColor : register(u1);
static const float kEndOpticalDepthFull = 1e-3;

#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= VolumetricFogGridSize.xy))
		return;

	float3 accumulatedLighting = 0.0f.xxx;
	float accumulatedTransmittance = 1.0f;
	float accumulatedDepth = 0.0f;

	uint eyeIndex;
	float previousDepth;
	float3 previousPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(uint3(dispatchID.xy, 0), float3(0.5f, 0.5f, 0.0f), eyeIndex, previousDepth);
#if defined(VOLUMETRIC_FOG_FAR_GRID)
	// The far volume continues the near one: its near fade-in has long finished.
	float nearGridStartDepth = (1.0f - VolumetricFogGridZParams.y) / max(VolumetricFogGridZParams.x, 1e-20f);
	accumulatedDepth = length(previousPositionWS) * max(previousDepth - nearGridStartDepth, 0.0f) / max(previousDepth, 1e-5f);
#endif

	[loop] for (uint layerIndex = 0; layerIndex < VolumetricFogGridSize.z; layerIndex++)
	{
		uint3 layerCoordinate = uint3(dispatchID.xy, layerIndex);
		float4 scatteringAndExtinction = LightScattering[layerCoordinate];

		uint layerEyeIndex;
		float layerDepth;
		float3 layerPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(layerCoordinate, 0.5f.xxx, layerEyeIndex, layerDepth);
		float stepLength = length(layerPositionWS - previousPositionWS);
		previousPositionWS = layerPositionWS;

		float extinction = max(scatteringAndExtinction.w, 0.0f);
		float transmittance = exp(-extinction * stepLength);

		accumulatedDepth += stepLength;
		float fadeIn = saturate(accumulatedDepth * VolumetricFogNearFadeInDistanceInv);

		float3 scatteringIntegratedOverSlice =
			fadeIn * (scatteringAndExtinction.rgb - scatteringAndExtinction.rgb * transmittance) / max(extinction, 1e-5f);
		accumulatedLighting += scatteringIntegratedOverSlice * accumulatedTransmittance;
		accumulatedTransmittance *= lerp(1.0f, transmittance, fadeIn);

		IntegratedLightScattering[layerCoordinate] = float4(max(accumulatedLighting, 0.0f.xxx), saturate(accumulatedTransmittance));

		[branch] if (layerIndex + 1u == VolumetricFogGridSize.z)
		{
			const float3 endColor = max(scatteringAndExtinction.rgb, 0.0f.xxx) / max(extinction, 1e-30f);
			const float endWeight = smoothstep(0.0f, kEndOpticalDepthFull, extinction * stepLength);
			VolumeEndColor[dispatchID.xy] = (endWeight > 0.0f && all(isfinite(endColor))) ? float4(min(endColor, 60000.0f), endWeight) : float4(0.0f, 0.0f, 0.0f, 0.0f);
		}
	}
}
