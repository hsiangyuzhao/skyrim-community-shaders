// (52a) Physical Lighting > Light Meter: the deferred composite's buffers at the crosshair (a 3x3 average around the centre of the
// rendered area), written into 8 float4 slots for a CPU readback. Only dispatched while the meter is on (one group, 3x3 threads).
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/SharedData.hlsli"

Texture2D<float4> MainTexture : register(t0);             // lit scene after the composite (diffuse + specular + reflections)
Texture2D<float4> AlbedoTexture : register(t1);           // G-buffer albedo (R10G10B10A2)
Texture2D<float4> NormalRoughnessTexture : register(t2);  // encoded view-space normal, glossiness
Texture2D<float4> MasksTexture : register(t3);            // z = forward ambient luminance (DALC + IBL, x albedo)
Texture2D<float4> SpecularTexture : register(t4);         // direct specular before the composite
Texture2D<float> DepthTexture : register(t5);
Texture2D<float4> ShadowMaskTexture : register(t6);       // x = the sun's shadow visibility (engine shadow mask)

RWStructuredBuffer<float4> MeterOut : register(u0);

groupshared float4 gLit[9];
groupshared float4 gAlbedo[9];
groupshared float4 gSpec[9];
groupshared float4 gMasks[9];

[numthreads(3, 3, 1)] void main(uint3 gtid : SV_GroupThreadID) {
	const uint index = gtid.y * 3 + gtid.x;
	const float2 centreUv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(float2(0.5, 0.5));
	const int2 centre = int2(centreUv * SharedData::BufferDim.xy);
	const int2 pixel = clamp(centre + int2(gtid.xy) - 1, int2(0, 0), int2(SharedData::BufferDim.xy) - 1);

	gLit[index] = float4(max(MainTexture.Load(int3(pixel, 0)).rgb, 0.0), 1.0);
	gAlbedo[index] = float4(AlbedoTexture.Load(int3(pixel, 0)).rgb, 1.0);
	gSpec[index] = float4(max(SpecularTexture.Load(int3(pixel, 0)).rgb, 0.0), 1.0);
	gMasks[index] = MasksTexture.Load(int3(pixel, 0));
	GroupMemoryBarrierWithGroupSync();

	if (index != 0)
		return;

	float4 lit = 0.0, albedo = 0.0, spec = 0.0, masks = 0.0;
	[unroll] for (uint i = 0; i < 9; ++i)
	{
		lit += gLit[i];
		albedo += gAlbedo[i];
		spec += gSpec[i];
		masks += gMasks[i];
	}
	lit /= 9.0;
	albedo /= 9.0;
	spec /= 9.0;
	masks /= 9.0;

	const float3 normalGlossiness = NormalRoughnessTexture.Load(int3(centre, 0)).xyz;
	const float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);
	const float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[0], float4(normalVS, 0)).xyz);

	const float depth = DepthTexture.Load(int3(centre, 0));
	float4 positionWS = float4(2.0 * float2(centreUv.x, 1.0 - centreUv.y) - 1.0, depth, 1.0);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[0], positionWS);
	positionWS.xyz /= positionWS.w;

	const float shadow = ShadowMaskTexture.Load(int3(centre, 0)).x;

	MeterOut[0] = lit;
	MeterOut[1] = albedo;
	MeterOut[2] = float4(normalWS, normalGlossiness.z);
	MeterOut[3] = masks;
	MeterOut[4] = spec;
	MeterOut[5] = float4(positionWS.xyz, depth);
	MeterOut[6] = float4(shadow, SharedData::GetScreenDepth(depth), 0.0, 0.0);
	MeterOut[7] = float4(float2(centre), SharedData::BufferDim.xy);
}
