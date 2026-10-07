// (batch 38, item 3) Skin SSS upgrade: shared constant buffer and albedo handling.
// Port of upstream 98ffc7f66 (diffuse extraction pre-pass, scatter modes) with the albedo fix
// 2f93cb9a1 (skin turning dark under Linear Lighting). Used only by the "v2" shaders
// (DiffuseExtractionCS, SeparableSSSV2CS); the 37c shaders keep their own cbuffer, which has the
// same layout (ScatterMode / PrepassMaskOnly sit in what was padding there).
#ifndef SSS_COMMON_HLSLI
#define SSS_COMMON_HLSLI

#include "Common/Color.hlsli"
#include "Common/Math.hlsli"
#include "Common/SharedData.hlsli"

#define SSSS_N_SAMPLES 21

#define SSS_SCATTER_MODE_PRE 0
#define SSS_SCATTER_MODE_POST 1
#define SSS_SCATTER_MODE_PRE_POST 2

cbuffer PerFrameSSS : register(b1)
{
	float4 Kernels[SSSS_N_SAMPLES + SSSS_N_SAMPLES];
	float4 BaseProfile;
	float4 HumanProfile;
	float SSSS_FOVY;
	uint BurleySamples;
	uint ScatterMode;
	uint PrepassMaskOnly;  // (ours) 1 = the pre-pass only writes skin pixels (Burley never reads the rest)
	float4 MeanFreePathBase;
	float4 MeanFreePathHuman;
};

float3 SSSDecodeAlbedo(float3 encodedAlbedo)
{
	float3 albedo = encodedAlbedo / Color::PBRLightingScale;
	return max(Color::IrradianceToLinear(albedo), 0.0f);
}

float3 SSSGetAlbedoFactor(float3 albedo, uint mode)
{
	if (mode == SSS_SCATTER_MODE_PRE)
		return 1.0f;
	return (mode == SSS_SCATTER_MODE_PRE_POST) ? sqrt(max(albedo, 0.0f)) : max(albedo, 0.0f);
}

float3 SSSGetAlbedoParticipation(float3 albedo)
{
	// Keep the low-albedo protection, but transition all channels continuously instead of
	// changing semantics at one quantized UNORM code. The threshold is converted to the same
	// linear domain as albedo so non-linear lighting keeps its previous effective cutoff.
	float threshold = Color::IrradianceToLinear(EPSILON_SSS_ALBEDO);
	return smoothstep(threshold, threshold * 4.0f, albedo);
}

float3 SSSRemoveAlbedo(float3 linearColor, float3 albedo, uint mode)
{
	if (mode == SSS_SCATTER_MODE_PRE)
		return linearColor;

	float3 factor = SSSGetAlbedoFactor(albedo, mode);
	float3 participation = SSSGetAlbedoParticipation(albedo);
	return linearColor * participation / max(factor, EPSILON_DIVISION);
}

float3 SSSApplyAlbedo(float3 irradiance, float3 originalLinearColor, float3 albedo, uint mode)
{
	if (mode == SSS_SCATTER_MODE_PRE)
		return irradiance;

	float3 factor = SSSGetAlbedoFactor(albedo, mode);
	float3 participation = SSSGetAlbedoParticipation(albedo);

	// Participation gates both emission into and reception from the blur. Squaring it here makes
	// the operation an exact identity without blur, while zero-albedo channels keep the original
	// colour and cannot pick up unpremultiplied energy.
	return irradiance * factor * participation + originalLinearColor * (1.0f - participation * participation);
}

/// Clamp DR-space UVs to the rendered rectangle; in VR also to the current eye's half, so a
/// blur tap never reads the other eye. Mirror of upstream's
/// FrameBuffer::ClampDynamicResolutionAdjustedScreenPosition as it was before VR was removed.
/// @param uvDR UVs in dynamic-resolution space (what DTid / BufferDim gives).
/// @param uvNonDR the same position in full-screen UVs (used only to pick the eye in VR).
float2 SSSClampToRenderedRegion(float2 uvDR, float2 uvNonDR)
{
	float2 minValue = 0;
	float2 maxValue = float2(FrameBuffer::DynamicResolutionParams2.z, FrameBuffer::DynamicResolutionParams1.y);
#if defined(VR)
	bool isRight = uvNonDR.x >= 0.5;
	minValue.x = isRight ? 0.5 * FrameBuffer::DynamicResolutionParams2.z : 0.0;
	maxValue.x = isRight ? FrameBuffer::DynamicResolutionParams2.z : 0.5 * FrameBuffer::DynamicResolutionParams2.z;
#endif
	return clamp(uvDR, minValue, maxValue);
}

#endif
