// (batch 38, A1) Farthest scene depth inside each froxel column (upstream 87d4e6c2a +
// 27df66181). Froxels entirely behind it are skipped by the light-scattering pass. In VR the
// volume and the depth buffer are both double-wide, so the stereo volume UV maps straight
// onto the depth buffer.
// (47g) One thread group per froxel column: the 64 threads stride over the column's depth
// pixels and reduce their maxima in groupshared memory (was one thread looping serially over
// every pixel of the column). Same pixel set, max is order independent: identical output.
#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"

RWTexture2D<float> ConservativeDepthTexture : register(u0);

#define CONSERVATIVE_DEPTH_THREADS_X 8
#define CONSERVATIVE_DEPTH_THREADS_Y 8
#define CONSERVATIVE_DEPTH_THREADS (CONSERVATIVE_DEPTH_THREADS_X * CONSERVATIVE_DEPTH_THREADS_Y)

groupshared float ConservativeDepthShared[CONSERVATIVE_DEPTH_THREADS];

[numthreads(CONSERVATIVE_DEPTH_THREADS_X, CONSERVATIVE_DEPTH_THREADS_Y, 1)] void main(uint3 groupID : SV_GroupID, uint3 threadID : SV_GroupThreadID, uint threadIndex : SV_GroupIndex) {
	// The dispatch is one group per column (VolumetricFogGridSize.xy groups). No early return:
	// the barriers below must stay in uniform control flow.
	const uint2 cell = groupID.xy;
	const bool validCell = all(cell < VolumetricFogGridSize.xy);

	float2 volumeUVMin = saturate((float2(cell) - 0.5f.xx) * VolumetricFogInvGridSize.xy);
	float2 volumeUVMax = saturate((float2(cell + 1u) + 0.5f.xx) * VolumetricFogInvGridSize.xy);

#if defined(VR)
	// Do not let a column straddling the eye seam read the other eye's depth.
	uint eyeIndex = ExponentialHeightFog::GetVolumeEyeIndex((float2(cell) + 0.5f) * VolumetricFogInvGridSize.xy);
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
	if (!validCell)
		maxCoord = minCoord - 1;  // no pixels

	float conservativeDepth = 0.0f;
	[loop] for (int y = minCoord.y + (int)threadID.y; y <= maxCoord.y; y += CONSERVATIVE_DEPTH_THREADS_Y)
	{
		[loop] for (int x = minCoord.x + (int)threadID.x; x <= maxCoord.x; x += CONSERVATIVE_DEPTH_THREADS_X)
		{
			float rawDepth = SharedData::DepthTexture.Load(int3(x, y, 0)).x;
			conservativeDepth = max(conservativeDepth, SharedData::GetScreenDepth(rawDepth));
		}
	}

	ConservativeDepthShared[threadIndex] = conservativeDepth;
	GroupMemoryBarrierWithGroupSync();
	[unroll] for (uint stride = CONSERVATIVE_DEPTH_THREADS / 2u; stride > 0u; stride >>= 1u)
	{
		if (threadIndex < stride)
			ConservativeDepthShared[threadIndex] = max(ConservativeDepthShared[threadIndex], ConservativeDepthShared[threadIndex + stride]);
		GroupMemoryBarrierWithGroupSync();
	}

	if (threadIndex == 0u && validCell)
		ConservativeDepthTexture[cell] = ConservativeDepthShared[0];
}
