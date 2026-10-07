#ifndef __VOLUMETRIC_SHADOWS_HLSLI__
#define __VOLUMETRIC_SHADOWS_HLSLI__

// (batch 38, item A2) Variance Shadow Maps (VSM) of the sun for see-through things.
// Ported from upstream Volumetric Shadows (0f79d567a + df08ea281 + e214c451f + 7d3e6a7c7).
//
// Differences from upstream:
// - Cascade matrices and splits come from our ShadowSampling::SharedShadowData (t19): the
//   vanilla PerGeometry constants, per eye and camera-relative, so VR keeps working and no
//   world-space t98 buffer is needed. Split distances there are post-projection depths, so
//   the cascade choice and fade follow ShadowSampling::Get2DFilteredShadow exactly.
// - The VSM sits at t23 (upstream t18 is our raw cascade array).
// - Every entry point is behind a runtime flag in SharedData::volumetricShadowsSettings.
//
// Included from Common/ShadowSampling.hlsli after the ShadowSampling namespace.

namespace VolumetricShadows
{
	// mip 0 = cascade 1 (512x512), mip 1 = cascade 0 (256x256); moments (E[z], E[z^2]).
	Texture2D<float2> SharedVSM : register(t23);

	static const float VSM_MIN_VARIANCE = 0.00001;
	static const float VSM_BLEEDING_REDUCTION = 0.2;

	bool ParticleShadowsEnabled()
	{
		return SharedData::volumetricShadowsSettings.ParticleShadows != 0;
	}

	bool ForwardSoftShadowsEnabled()
	{
		return SharedData::volumetricShadowsSettings.ForwardSoftShadows != 0;
	}

	// Chebyshev upper bound on P(X >= t); moments.x = mean(z), moments.y = mean(z^2)
	float ComputeVSM(float2 moments, float depth)
	{
		float variance = max(moments.y - moments.x * moments.x, VSM_MIN_VARIANCE);
		float d = depth - moments.x;
		float pMax = variance / (variance + d * d);
		return (depth <= moments.x) ? 1.0 : pMax;
	}

	// Remaps shadow values below a threshold to zero (reduces VSM light bleeding)
	float ReduceBleeding(float shadow, float amount)
	{
		return saturate((shadow - amount) / (1.0 - amount));
	}

	float3 ToLightSpace(ShadowSampling::ShadowData sD, uint eyeIndex, uint cascadeIndex, float3 positionWS)
	{
		float3 positionLS = mul(transpose(sD.ShadowMapProj[eyeIndex][cascadeIndex]), float4(positionWS, 1)).xyz;
		positionLS.xy = saturate(positionLS.xy);
		return positionLS;
	}

	float SampleVSMCascade2D(uint cascadeIndex, float3 positionLS)
	{
		float2 moments = SharedVSM.SampleLevel(LinearSampler, positionLS.xy, 1u - cascadeIndex);
		return ComputeVSM(moments, positionLS.z);
	}

	float SampleVSMCascade3D(uint cascadeIndex, float noise, uint sampleCount, float rcpSampleCount, float3 startLS, float3 endLS, out float firstSample)
	{
		float shadow = 0.0;
		firstSample = 1.0;

		[loop] for (uint k = 0; k < sampleCount; k++)
		{
			float t = (float(k) + noise) * rcpSampleCount;
			float3 samplePosLS = lerp(endLS, startLS, t);
			float lit = ComputeVSM(SharedVSM.SampleLevel(LinearSampler, samplePosLS.xy, 1u - cascadeIndex), samplePosLS.z);
			firstSample = lit;  // the last one written is the start of the segment
			shadow += lit;
		}

		return shadow * rcpSampleCount;
	}

	// Cascade selection shared by the 2D and 3D lookups: same rules as
	// ShadowSampling::Get2DFilteredShadow (post-projection depth vs. the vanilla splits),
	// with upstream 7d3e6a7c7's smoothstep blend across the cascade 0/1 overlap.
	bool SelectCascades(ShadowSampling::ShadowData sD, float3 positionWS, uint eyeIndex, out uint primaryCascade, out float blend, out float fadeFactor)
	{
		float shadowMapDepth = ShadowSampling::GetShadowDepth(positionWS, eyeIndex);
		primaryCascade = 0;
		blend = 0;
		fadeFactor = 0;
		if (sD.EndSplitDistances.z < shadowMapDepth)
			return false;

		fadeFactor = 1.0 - pow(saturate(dot(positionWS, positionWS) / sD.ShadowLightParam.z), 8);
		if (sD.EndSplitDistances.x < shadowMapDepth) {
			primaryCascade = 1;
		} else if (sD.StartSplitDistances.y < shadowMapDepth) {
			blend = smoothstep(0, 1, (shadowMapDepth - sD.StartSplitDistances.y) / max(sD.EndSplitDistances.x - sD.StartSplitDistances.y, 1e-6));
		}
		return true;
	}

	/// Soft sun shadow at a point. detailedShadow has the bleeding reduction applied.
	float GetVSMShadow2D(float3 positionWS, uint eyeIndex, out float detailedShadow)
	{
		ShadowSampling::ShadowData sD = ShadowSampling::SharedShadowData[0];

		uint primaryCascade;
		float blend;
		float fadeFactor;
		if (!SelectCascades(sD, positionWS, eyeIndex, primaryCascade, blend, fadeFactor)) {
			detailedShadow = 1.0;
			return 1.0;
		}

		float shadow = SampleVSMCascade2D(primaryCascade, ToLightSpace(sD, eyeIndex, primaryCascade, positionWS));
		[branch] if (blend > 0.0)
		{
			float shadowBlend = SampleVSMCascade2D(1, ToLightSpace(sD, eyeIndex, 1, positionWS));
			shadow = lerp(shadow, shadowBlend, blend);
		}

		detailedShadow = lerp(1.0, ReduceBleeding(shadow, VSM_BLEEDING_REDUCTION), fadeFactor);
		return lerp(1.0, shadow, fadeFactor);
	}

	/// Average sun visibility along a segment (camera-relative positions).
	float GetVSMShadow3D(float3 startPosition, float3 endPosition, float noise, uint baseSampleCount, uint eyeIndex, out float surfaceShadow)
	{
		ShadowSampling::ShadowData sD = ShadowSampling::SharedShadowData[0];

		float3 midPosition = (startPosition + endPosition) * 0.5;
		uint primaryCascade;
		float blend;
		float fadeFactor;
		if (!SelectCascades(sD, midPosition, eyeIndex, primaryCascade, blend, fadeFactor)) {
			surfaceShadow = 1.0;
			return 1.0;
		}

		uint sampleCount = max(1u, (uint)ceil(float(baseSampleCount) * fadeFactor));
		float rcpSampleCount = rcp((float)sampleCount);

		float firstSample;
		float shadow = SampleVSMCascade3D(primaryCascade, noise, sampleCount, rcpSampleCount,
			ToLightSpace(sD, eyeIndex, primaryCascade, startPosition), ToLightSpace(sD, eyeIndex, primaryCascade, endPosition), firstSample);
		surfaceShadow = firstSample;

		[branch] if (blend > 0.0)
		{
			float firstSampleBlend;
			float shadowBlend = SampleVSMCascade3D(1, noise, sampleCount, rcpSampleCount,
				ToLightSpace(sD, eyeIndex, 1, startPosition), ToLightSpace(sD, eyeIndex, 1, endPosition), firstSampleBlend);
			shadow = lerp(shadow, shadowBlend, blend);
			surfaceShadow = lerp(surfaceShadow, firstSampleBlend, blend);
		}

		surfaceShadow = lerp(1.0, surfaceShadow, fadeFactor);
		return lerp(1.0, shadow, fadeFactor);
	}

	/// Port of upstream ShadowSampling::Get3DFilteredShadow for effects: world shadow (terrain,
	/// clouds) and the VSM, both averaged along a short segment of the view ray through the
	/// effect, so a puff of smoke fades into a shadow instead of switching at one plane.
	/// Upstream sizes the segment from the effect's bound radius (a permutation constant we do
	/// not have); a fixed +/-128 units (about 1.8 m) is used here, 4 samples.
	float GetEffectShadow(float3 positionWS, float3 viewDirection, float2 screenPosition, uint eyeIndex)
	{
		const float viewRayLength = 128.0;
		float3 startPosition = positionWS - viewDirection * viewRayLength;
		float3 endPosition = positionWS + viewDirection * viewRayLength;

		const uint sampleCount = 4;
		const float rcpSampleCount = 0.25;

		float noise = Random::InterleavedGradientNoise(screenPosition, SharedData::FrameCount);

		float worldShadow = 0.0;
		[unroll] for (uint i = 0; i < sampleCount; i++)
		{
			float t = (float(i) + noise) * rcpSampleCount;
			worldShadow += ShadowSampling::GetWorldShadow(lerp(endPosition, startPosition, t), FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);
		}
		worldShadow *= rcpSampleCount;

		if (worldShadow == 0.0)
			return 0.0;

		float unusedSurfaceShadow;
		return worldShadow * GetVSMShadow3D(startPosition, endPosition, noise, sampleCount, eyeIndex, unusedSurfaceShadow);
	}
}

#endif  // __VOLUMETRIC_SHADOWS_HLSLI__
