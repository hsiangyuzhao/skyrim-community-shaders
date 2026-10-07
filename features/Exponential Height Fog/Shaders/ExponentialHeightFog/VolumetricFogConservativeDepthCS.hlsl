// (batch 38, A1) Farthest scene depth inside each froxel column (upstream 87d4e6c2a +
// 27df66181). Froxels entirely behind it are skipped by the light-scattering pass. In VR the
// volume and the depth buffer are both double-wide, so the stereo volume UV maps straight
// onto the depth buffer.
#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

RWTexture2D<float> ConservativeDepthTexture : register(u0);

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (any(dispatchID.xy >= VolumetricFogGridSize.xy))
		return;

	float2 volumeUVMin = saturate((float2(dispatchID.xy) - 0.5f.xx) * VolumetricFogInvGridSize.xy);
	float2 volumeUVMax = saturate((float2(dispatchID.xy + 1u) + 0.5f.xx) * VolumetricFogInvGridSize.xy);

#if defined(VR)
	// Do not let a column straddling the eye seam read the other eye's depth.
	uint eyeIndex = ExponentialHeightFog::GetVolumeEyeIndex((float2(dispatchID.xy) + 0.5f) * VolumetricFogInvGridSize.xy);
	volumeUVMin.x = max(volumeUVMin.x, eyeIndex == 0u ? 0.0f : 0.5f);
	volumeUVMax.x = min(volumeUVMax.x, eyeIndex == 0u ? 0.5f : 1.0f);
#endif

	// The volume spans the whole view, while the depth buffer occupies the active
	// dynamic-resolution region. Include every depth pixel touched by this froxel.
	float2 renderSize = SharedData::BufferDim.xy * FrameBuffer::DynamicResolutionParams1.xy;
	float2 sampleCoordMin = volumeUVMin * renderSize;
	float2 sampleCoordMax = volumeUVMax * renderSize;

	int2 minCoord = int2(floor(sampleCoordMin));
	int2 maxCoord = int2(ceil(sampleCoordMax)) - 1;
	maxCoord = max(maxCoord, minCoord);

	int2 bufferMax = max(int2(ceil(renderSize)) - 1, int2(0, 0));
	minCoord = clamp(minCoord, int2(0, 0), bufferMax);
	maxCoord = clamp(maxCoord, int2(0, 0), bufferMax);

	float conservativeDepth = 0.0f;
	for (int y = minCoord.y; y <= maxCoord.y; y++) {
		for (int x = minCoord.x; x <= maxCoord.x; x++) {
			float rawDepth = SharedData::DepthTexture.Load(int3(x, y, 0)).x;
			conservativeDepth = max(conservativeDepth, SharedData::GetScreenDepth(rawDepth));
		}
	}

	ConservativeDepthTexture[dispatchID.xy] = conservativeDepth;
}
