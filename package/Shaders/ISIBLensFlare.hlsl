#include "Common/DummyVSTexCoord.hlsl"
#include "Common/FrameBuffer.hlsli"
#include "Common/SharedData.hlsli"  // (52a) Physical Lighting's frame scale

typedef VS_OUTPUT PS_INPUT;

struct PS_OUTPUT
{
	float4 Color : SV_Target0;
};

#if defined(PSHADER)
SamplerState DownScaledBlurredHDRSampler : register(s0);
SamplerState RampsSampler : register(s1);
SamplerState MasksSampler : register(s2);

Texture2D<float4> DownScaledBlurredHDRTex : register(t0);
Texture2D<float4> RampsTex : register(t1);
Texture2D<float4> MasksTex : register(t2);

cbuffer PerGeometry : register(b2)
{
	float lensCount : packoffset(c0.x);
	float flareDispersal : packoffset(c0.y);
	float lightsRangeDownshift : packoffset(c0.z);
	float invLightsRangeDownshift : packoffset(c0.w);
	float lateralRepeat : packoffset(c1.x);
	float channelsDistortionRed : packoffset(c1.y);
	float channelsDistortionGreen : packoffset(c1.z);
	float channelsDistortionBlue : packoffset(c1.w);
	float texelOffsetX : packoffset(c2.x);
	float texelOffsetY : packoffset(c2.y);
	float haloFetch : packoffset(c2.z);
	float haloWidthPow : packoffset(c2.w);
	float dynamicSource : packoffset(c3.x);
	float globalIntensity : packoffset(c4.x);
};

float4 GetSampleColor(float2 texCoord)
{
	// (52a) Physical Lighting: the source is pre-exposed by S; the threshold and the saturate work on the classic scale (x 1 / S,
	// VolumetricShadowsSettings.pad0.y, unread before 52a; exactly 1.0 off).
	float4 color = DownScaledBlurredHDRTex.Sample(DownScaledBlurredHDRSampler, texCoord) * asfloat(SharedData::volumetricShadowsSettings.pad0.y);
	if (saturate(color.x - lightsRangeDownshift) + saturate(color.y - lightsRangeDownshift) +
			saturate(color.z - lightsRangeDownshift) <=
		0) {
		return 0;
	}
	return saturate(color);
}

PS_OUTPUT main(PS_INPUT input)
{
	PS_OUTPUT psout;

	float4 lensColor = 0;
	for (int sampleIndex = -asint(lateralRepeat); sampleIndex <= asint(lateralRepeat); ++sampleIndex) {
		float4 sampleColor;
		if (dynamicSource > 0.5) {
			sampleColor = GetSampleColor(FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(
				input.TexCoord + float2(texelOffsetX * sampleIndex, 0)));
		} else {
			sampleColor = GetSampleColor(input.TexCoord + float2(texelOffsetX * sampleIndex, 0));
		}
		lensColor += sampleColor;
	}
	// (52a) Back to the frame's scale as the composite reads it (through Color::GammaToLinear with Linear Lighting: S^(1/1.6);
	// PostProcessingSettings.pad0.y, unread before 52a; exactly 1.0 off).
	psout.Color = globalIntensity * lensColor * asfloat(SharedData::postProcessingSettings.pad0.y);

	return psout;
}
#endif
