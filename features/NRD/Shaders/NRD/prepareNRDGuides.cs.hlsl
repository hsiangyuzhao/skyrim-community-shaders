// Builds the three guide surfaces every NRD denoiser instance consumes:
//   u0 IN_VIEWZ            — linear view-space depth (R32_FLOAT)
//   u1 IN_NORMAL_ROUGHNESS — world-space normal + linear roughness, packed with
//                            NRD_FrontEnd_PackNormalAndRoughness (R10G10B10A2_UNORM,
//                            matching NRD_NORMAL_ENCODING=2 / NRD_ROUGHNESS_ENCODING=1)
// IN_MV is a plain CopyResource of the game's motion-vector target, done on the CPU side.
//
// Ported from the upstream reference integration; the only adaptation is the [0]
// eye index on the FrameBuffer matrices (this fork's headers declare per-eye arrays;
// the NRD feature does not load in VR, so eye 0 is always the whole frame here).

#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"
#include "NRD/NRDReblurSH.hlsli"

Texture2D<float> srcDepth : register(t0);
Texture2D<float4> srcNormalRoughness : register(t1);

RWTexture2D<float> outViewZ : register(u0);
RWTexture2D<float4> outNormalRoughness : register(u1);

float ScreenToViewDepth(float screenDepth)
{
	// Far plane / sky must land outside NRD's denoisingRange so the denoiser
	// treats it as "no surface"; 0 would read as the near plane instead.
	if (screenDepth >= 1.0 - 1e-6 || screenDepth <= 0.0)
		return 3.402823466e+38;
	return (SharedData::CameraData.w / (-screenDepth * SharedData::CameraData.z + SharedData::CameraData.x));
}

[numthreads(8, 8, 1)] void main(uint2 dtid : SV_DispatchThreadID) {
	float depth = srcDepth[dtid];
	float viewZ = ScreenToViewDepth(depth);

	outViewZ[dtid] = viewZ;

	float4 normalGloss = srcNormalRoughness[dtid];
	float3 normalVS = GBuffer::DecodeNormal(normalGloss.xy);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[0], float4(normalVS, 0)).xyz);

	float roughness = 1.0 - normalGloss.z;

	outNormalRoughness[dtid] = NRD_FrontEnd_PackNormalAndRoughness(normalWS, roughness, 0.0);
}
