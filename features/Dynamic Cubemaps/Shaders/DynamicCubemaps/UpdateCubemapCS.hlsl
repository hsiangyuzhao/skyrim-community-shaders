#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/PhysicalLighting.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/VR.hlsli"

RWTexture2DArray<float4> DynamicCubemap : register(u0);
RWTexture2DArray<float4> DynamicCubemapRaw : register(u1);
RWTexture2DArray<float4> DynamicCubemapPosition : register(u2);

Texture2D<float> DepthTexture : register(t0);
Texture2D<float4> ColorTexture : register(t1);
// (49h stage 1, S3a) the G-buffer albedo and masks (Masks.z = the forward ambient's luminance), bound by UpdateCubemapCapture.
// Unbound (other callers): zeros, nothing is taken out.
Texture2D<float4> AlbedoTexture : register(t2);
Texture2D<float4> MasksTexture : register(t3);

// (49h stage 1, S3a) "Screen Light Probe Without Feedback": the IBL part of the ambient MAIN already holds (forward Lighting added
// DALC x DALCAmount + IBL, x albedo; Masks.z is its luminance) is taken out of the captured colour, so the probe the IBL is built
// from no longer contains its own light (probe -> IBL -> MAIN -> probe ran away on bright snow). The weather ambient part stays.
SamplerState LinearSampler : register(s0);

float3 RemoveIblFeedback(float3 a_color, float2 a_uv)
{
	float3 result = a_color;
	[branch] if (PhysLight::On(PhysLight::kIblNoFeedback) && SharedData::iblSettings.EnableDiffuseIBL && (!SharedData::InInterior || SharedData::iblSettings.EnableInterior))
	{
		const float3 albedo = max(AlbedoTexture.SampleLevel(LinearSampler, a_uv, 0).rgb, 0.0);
		const float ambientY = max(MasksTexture.SampleLevel(LinearSampler, a_uv, 0).z, 0.0);
		const float3 dalc = Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(0, 0, 0, 1)))) * SharedData::iblSettings.DALCAmount;
		const float iblY = max(ambientY - Color::RGBToLuminance(dalc * albedo), 0.0);
		const float albedoY = Color::RGBToLuminance(albedo);
		const float3 iblPart = albedoY > 1e-4 ? albedo * (iblY / albedoY) : 0.0;
		result = max(a_color - iblPart, 0.0);
	}
	return result;
}

// Calculate normalized sampling direction vector based on current fragment coordinates.
// This is essentially "inverse-sampling": we reconstruct what the sampling vector would be if we wanted it to "hit"
// this particular fragment in a cubemap.
float3 GetSamplingVector(uint3 ThreadID, in RWTexture2DArray<float4> OutputTexture)
{
	float width = 0.0f;
	float height = 0.0f;
	float depth = 0.0f;
	OutputTexture.GetDimensions(width, height, depth);

	float2 st = ThreadID.xy / float2(width, height);
	float2 uv = 2.0 * float2(st.x, 1.0 - st.y) - 1.0;

	// Select vector based on cubemap face index.
	float3 result = float3(0.0f, 0.0f, 0.0f);
	switch (ThreadID.z) {
	case 0:
		result = float3(1.0, uv.y, -uv.x);
		break;
	case 1:
		result = float3(-1.0, uv.y, uv.x);
		break;
	case 2:
		result = float3(uv.x, 1.0, -uv.y);
		break;
	case 3:
		result = float3(uv.x, -1.0, uv.y);
		break;
	case 4:
		result = float3(uv.x, uv.y, 1.0);
		break;
	case 5:
		result = float3(-uv.x, uv.y, -1.0);
		break;
	}
	return normalize(result);
}

cbuffer UpdateData : register(b0)
{
	float3 CameraPreviousPosAdjust2;
	float CaptureWeight;  // (batch 39b) share of a new capture; Dynamic Cubemaps always used 0.5
	// (49i) "Probe Forgets Out-of-View Light" (plain capture, while Physical Ambient is in effect; DynamicCubemaps::UpdateCubemapCB):
	// x = multiplier on out-of-view texels' colour this capture (1 = none), y = 1: sky pixels in view are captured as no bounce.
	float4 ProbeForget;
}

float smoothbumpstep(float edge0, float edge1, float x)
{
	x = 1.0 - abs(saturate((x - edge0) / (edge1 - edge0)) - 0.5) * 2.0;
	return x * x * (3.0 - x - x);
}

[numthreads(8, 8, 1)] void main(uint3 ThreadID
								: SV_DispatchThreadID) {
	float3 captureDirection = -GetSamplingVector(ThreadID, DynamicCubemap);
	float3 viewDirection = FrameBuffer::WorldToView(captureDirection, false);
	float2 uv = FrameBuffer::ViewToUV(viewDirection, false);

	if (!FrameBuffer::IsOutsideFrame(uv) && viewDirection.z < 0.0) {  // Check that the view direction exists in screenspace and that it is in front of the camera
		float3 color = 0.0;
		float3 position = 0.0;
		float weight = 0.0;

		uv = FrameBuffer::GetDynamicResolutionAdjustedScreenPosition(uv);
		uv = Stereo::ConvertToStereoUV(uv, 0);

		float depth = DepthTexture.SampleLevel(LinearSampler, uv, 0);
		float linearDepth = SharedData::GetScreenDepth(depth);

#if defined(REFLECTIONS)
		if (linearDepth > 16.5) {  // Ignore objects which are too close
#else
		if (linearDepth > 16.5 && depth != 1.0) {  // Ignore objects which are too close or the sky
#endif
			half4 positionCS = half4(2 * half2(uv.x, -uv.y + 1) - 1, depth, 1);
			positionCS = mul(FrameBuffer::CameraViewProjInverse[0], positionCS);
			positionCS.xyz = positionCS.xyz / positionCS.w;

			position += positionCS.xyz;

			color += RemoveIblFeedback(ColorTexture.SampleLevel(LinearSampler, uv, 0).rgb, uv);  // (49h stage 1, S3a)
			weight++;
		}
#if !defined(REFLECTIONS)
		// (49i) The sky in view: no bounce (black, alpha 1: known, so the inference does not fill it with ground colour from the coarser
		// mips). Its position 10 km out along the direction (out of view later: distance factor 0, colour 0 anyway).
		else if (ProbeForget.y > 0.5 && depth == 1.0) {
			const float lerpFactor = CaptureWeight;
			DynamicCubemapPosition[ThreadID] = lerp(DynamicCubemapPosition[ThreadID], float4(captureDirection * 10.0, 0.0), lerpFactor);
			DynamicCubemapRaw[ThreadID] = max(0, lerp(DynamicCubemapRaw[ThreadID], float4(0.0, 0.0, 0.0, 1.0), lerpFactor));
			DynamicCubemap[ThreadID] = max(0, lerp(DynamicCubemap[ThreadID], float4(0.0, 0.0, 0.0, 1.0), lerpFactor));
			return;
		}
#endif

		if (weight > 0.0) {
			position /= weight;
			color /= weight;

			float4 positionFinal = float4(position.xyz * 0.001, length(position) < (4096.0 * 2.5));
			float4 colorFinal = float4(Color::IrradianceToLinear(color), 1.0);

			float lerpFactor = CaptureWeight;

			DynamicCubemapPosition[ThreadID] = lerp(DynamicCubemapPosition[ThreadID], positionFinal, lerpFactor);
			DynamicCubemapRaw[ThreadID] = max(0, lerp(DynamicCubemapRaw[ThreadID], colorFinal, lerpFactor));

			colorFinal *= sqrt(saturate(0.5 * length(position.xyz)));

			DynamicCubemap[ThreadID] = max(0, lerp(DynamicCubemap[ThreadID], colorFinal, lerpFactor));

			return;
		}
	}

	float4 position = DynamicCubemapPosition[ThreadID];
	position.xyz = (position.xyz + (CameraPreviousPosAdjust2.xyz * 0.001)) - (FrameBuffer::CameraPosAdjust[0].xyz * 0.001);  // Remove adjustment, add new adjustment
	DynamicCubemapPosition[ThreadID] = position;

	float4 color = DynamicCubemapRaw[ThreadID];
#if !defined(REFLECTIONS)
	// (49i) Out of view: the colour fades (alpha kept: a known direction, now without bounce).
	[branch] if (ProbeForget.x < 1.0)
	{
		color.rgb *= ProbeForget.x;
		DynamicCubemapRaw[ThreadID] = color;
	}
#endif

	float distance = length(position.xyz);
	float distanceFactor = smoothbumpstep(0.0, 2.0, distance);

	if (distance < 1.0)
		distanceFactor = sqrt(distanceFactor);

#if defined(FAKEREFLECTIONS)
	distanceFactor = max(distanceFactor, smoothstep(0.0, 2.0, distance));
#endif

	color *= distanceFactor;

	DynamicCubemap[ThreadID] = max(0, color);
}
