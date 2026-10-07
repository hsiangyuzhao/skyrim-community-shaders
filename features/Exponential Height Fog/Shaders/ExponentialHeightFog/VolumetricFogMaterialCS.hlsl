// (batch 38, A1) Participating medium of each froxel: extinction from the height fog layers
// (+ noise), scattering = extinction x albedo (upstream 87d4e6c2a / 08faccd51).
#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

RWTexture3D<float4> VBufferA : register(u0);

[numthreads(8, 8, 4)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (!ExponentialHeightFog::IsInsideVolumetricGrid(dispatchID))
		return;

	uint eyeIndex;
	float viewDepth;
	float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, 0.5f.xxx, eyeIndex, viewDepth);

	float extinction = ExponentialHeightFog::EvaluateHeightFogExtinction(positionWS, FrameBuffer::CameraPosAdjust[eyeIndex].xyz);
	float3 albedo = saturate(VolumetricFogAlbedo.rgb);
	float3 scattering = extinction * albedo * VolumetricFogAlbedo.a;

	VBufferA[dispatchID] = float4(scattering, extinction);
}
