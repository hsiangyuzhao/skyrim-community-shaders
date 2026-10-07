// (batch 38, item 3) Skin SSS upgrade: blur passes reading the pre-pass output.
// Upstream 98ffc7f66 + 2f93cb9a1, with VR (eyeIndex, per-eye clamp/step) kept.
// HORIZONTAL writes the intermediate texture. The last pass (vertical / Burley) writes skin pixels
// straight into MAIN, reading each one's original colour back from it first (as upstream does),
// plus the DLSS-RR SSS guide for every pixel (what the 37c composite pass produced).
#if defined(HORIZONTAL)
RWTexture2D<float4> SSSRW : register(u0);  // intermediate (H-blurred) texture
#else
RWTexture2D<float4> SSSRW : register(u0);   // MAIN
RWTexture2D<float> SSSGuide : register(u1);  // luminance of the change SSS made, for DLSS-RR
#endif

Texture2D<float4> ColorTexture : register(t0);  // pre-pass output (H pass, Burley) or H pass output (V pass)
Texture2D<float4> DepthTexture : register(t1);
Texture2D<float4> MaskTexture : register(t2);
Texture2D<float4> AlbedoTexture : register(t3);
Texture2D<float4> NormalTexture : register(t4);

SamplerState PointSampler : register(s0);

#include "Common/Color.hlsli"
#include "Common/Random.hlsli"
#include "Common/SharedData.hlsli"
#include "SubsurfaceScattering/SSSCommon.hlsli"

#if defined(BURLEY)
#	include "SubsurfaceScattering/BurleyV2.hlsli"
#else
#	include "SubsurfaceScattering/SeparableSSSV2.hlsli"
#endif

#if !defined(HORIZONTAL)
// Same weighting as SSSCompositeCS.hlsl.
float SSSGuideLuminance(float3 color)
{
	return (color.x + 2 * color.y + color.z) / 4.0;
}
#endif

[numthreads(8, 8, 1)] void main(uint3 DTid : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(DTid.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 texCoord = (DTid.xy + 0.5) * SharedData::BufferDim.zw;

#if defined(BURLEY)

	float sssAmount = MaskTexture[DTid.xy].x;
	float guide = 0.0;

	if (sssAmount > 0.0) {
		bool humanProfile = MaskTexture[DTid.xy].y > 0.0;
		uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(texCoord * FrameBuffer::DynamicResolutionParams2.xy);

		float4 originalColor = SSSRW[DTid.xy];
		float4 color = max(0, BurleyNormalizedSSV2(DTid.xy, texCoord, eyeIndex, sssAmount, humanProfile, originalColor));
		SSSRW[DTid.xy] = color;
		guide = SSSGuideLuminance(color.rgb - originalColor.rgb);
	}
	SSSGuide[DTid.xy] = guide;

#elif defined(HORIZONTAL)

	float sssAmount = MaskTexture[DTid.xy].x;
	bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

	float4 color = SSSSBlurCSV2(texCoord, float2(1.0, 0.0), sssAmount, humanProfile);
	SSSRW[DTid.xy] = max(0, color);

#else

	float sssAmount = MaskTexture[DTid.xy].x;
	float guide = 0.0;

	if (sssAmount > 0.0) {
		bool humanProfile = MaskTexture[DTid.xy].y > 0.0;

		float4 originalColor = SSSRW[DTid.xy];
		float4 color = SSSSBlurCSV2(texCoord, float2(0.0, 1.0), sssAmount, humanProfile);
		float3 albedo = SSSDecodeAlbedo(AlbedoTexture[DTid.xy].rgb);
		color.rgb = SSSApplyAlbedo(color.rgb, Color::IrradianceToLinear(originalColor.rgb), albedo, ScatterMode);
		color.rgb = Color::IrradianceToGamma(color.rgb);
		SSSRW[DTid.xy] = float4(color.rgb, originalColor.a);
		guide = SSSGuideLuminance(color.rgb - originalColor.rgb);
	}
	SSSGuide[DTid.xy] = guide;

#endif
}
