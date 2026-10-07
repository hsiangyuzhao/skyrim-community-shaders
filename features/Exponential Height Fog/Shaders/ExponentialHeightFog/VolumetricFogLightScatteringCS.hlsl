// (batch 38, A1) In-scattered light of every froxel: sun (cascaded shadow map + terrain and
// cloud shadow), sky (IBL or vanilla ambient, occluded by Skylighting) and Light Limit Fix's
// clustered point lights, then blended with the reprojected previous frame.
//
// Ported from upstream 87d4e6c2a (#2361) + 27df66181 (phase) + 08faccd51 (#2831, far
// volume). Adapted to our tree:
// - Sun shadow: the cascade array and our SharedShadowData (t19, VR-aware per-eye camera-
//   relative matrices, post-projection split depths) captured by Deferred::CopyShadowData;
//   upstream reads a world-space DirectionalShadowLights buffer at t98 that we do not have.
// - Lights: our LLF Light layout (positionWS[eye]); Inverse Square Lighting when installed.
// - VR (removed upstream in c9d7c0524) restored: per-cell eye index, per-eye matrices.
// - Local lights use the same phase convention as the sun (forward scattering toward the
//   viewer when looking at the light).

SamplerState LinearSampler : register(s0);
SamplerComparisonState ShadowSampler : register(s1);
Texture3D<float4> VBufferA : register(t0);
Texture2DArray<float4> DirectionalShadowMap : register(t1);
Texture3D<float4> LightScatteringHistory : register(t2);
Texture2D<float> ConservativeDepthTexture : register(t3);
Texture2D<float> PrevConservativeDepthTexture : register(t4);
RWTexture3D<float4> LightScattering : register(u0);

#include "Common/Color.hlsli"
#include "Common/Random.hlsli"
#include "ExponentialHeightFog/VolumetricFogCSCommon.hlsli"
#include "IBL/IBL.hlsli"
#if defined(TERRAIN_SHADOWS)
#	include "TerrainShadows/TerrainShadows.hlsli"
#endif
#if defined(CLOUD_SHADOWS)
#	include "CloudShadows/CloudShadows.hlsli"
#endif
#include "Common/ShadowSampling.hlsli"
#if defined(LIGHT_LIMIT_FIX)
#	include "LightLimitFix/LightLimitFix.hlsli"
#	if defined(ISL)
#		include "InverseSquareLighting/InverseSquareLighting.hlsli"
#	endif
#endif
#include "Skylighting/Skylighting.hlsli"

Texture3D<sh2> FogSkylightingProbeArray : register(t50);

// 4D PCG hash matching UE's Rand4DPCG32 (jcgt.org/published/0009/03/02/)
uint4 Rand4DPCG32(int4 p)
{
	uint4 v = uint4(p);
	v = v * 1664525u + 1013904223u;
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	v ^= (v >> 16u);
	v.x += v.y * v.w;
	v.y += v.z * v.x;
	v.z += v.x * v.y;
	v.w += v.y * v.z;
	return v;
}

// Matches UE's MakePositiveFinite - ensures no NaN/Inf propagates into the history chain
float4 MakePositiveFinite(float4 v)
{
	v = max(v, 0.0f.xxxx);
	v.x = isfinite(v.x) ? v.x : 0.0f;
	v.y = isfinite(v.y) ? v.y : 0.0f;
	v.z = isfinite(v.z) ? v.z : 0.0f;
	v.w = isfinite(v.w) ? v.w : 0.0f;
	return v;
}

bool IsFroxelBehindSceneDepth(uint3 coord)
{
	float frontDepth = ExponentialHeightFog::ComputeVolumetricSliceDepth(max(float(coord.z) - 0.5f, 0.0f));
	float sceneDepth = ConservativeDepthTexture[coord.xy];
	return sceneDepth < frontDepth;
}

float3 ComputeHistoryVolumeUVAndDepth(float3 positionWS, uint eyeIndex, out bool validHistory, out float previousViewDepth)
{
	float3 previousPositionWS = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPreviousPosAdjust[eyeIndex].xyz;
	float4 previousClip = mul(FrameBuffer::CameraPreviousViewProjUnjittered[eyeIndex], float4(previousPositionWS, 1.0f));

	previousViewDepth = abs(previousClip.w);
	validHistory = previousClip.w > 0.0f;
	if (!validHistory)
		return 0.0f.xxx;

	float2 historyUV = previousClip.xy / previousClip.w * float2(0.5f, -0.5f) + 0.5f;
	validHistory = all(historyUV >= 0.0f) && all(historyUV < 1.0f);
#if defined(VR)
	historyUV = Stereo::ConvertToStereoUV(historyUV, eyeIndex);
#endif

	float historyZ = ExponentialHeightFog::ComputeVolumetricNormalizedSlice(previousViewDepth);
	float3 volumeUV = float3(historyUV, historyZ);
	validHistory = validHistory && !any(volumeUV < 0.0f) && !any(volumeUV >= 1.0f);
	return saturate(volumeUV);
}

float2 FixupHistoryUV(float2 uv, float previousCellDepth, out bool validHistory)
{
	float2 size = float2(VolumetricFogGridSize.xy);
	float2 fullResUV = uv * size;
	float2 screenCoord = floor(fullResUV - 0.5f);
	float2 fullResOffset = fullResUV - screenCoord;
	float2 gatherUV = (screenCoord + 1.0f) / size;

	float4 previousSceneDepths = PrevConservativeDepthTexture.Gather(LinearSampler, gatherUV);
	bool4 validSamples = previousSceneDepths >= previousCellDepth;

	validHistory = true;
	if (all(validSamples))
		return uv;

	if (all(validSamples.wz))
		return (screenCoord + float2(fullResOffset.x, 0.5f)) / size;
	if (all(validSamples.xy))
		return (screenCoord + float2(fullResOffset.x, 1.5f)) / size;
	if (all(validSamples.wx))
		return (screenCoord + float2(0.5f, fullResOffset.y)) / size;
	if (all(validSamples.zy))
		return (screenCoord + float2(1.5f, fullResOffset.y)) / size;

	if (validSamples.x)
		return (screenCoord + float2(0.5f, 1.5f)) / size;
	if (validSamples.y)
		return (screenCoord + float2(1.5f, 1.5f)) / size;
	if (validSamples.w)
		return (screenCoord + float2(0.5f, 0.5f)) / size;
	if (validSamples.z)
		return (screenCoord + float2(1.5f, 0.5f)) / size;

	validHistory = false;
	return uv;
}

float SampleDirectionalShadowPCF(float3 positionLS, uint cascadeIndex)
{
	uint shadowWidth;
	uint shadowHeight;
	uint shadowSlices;
	DirectionalShadowMap.GetDimensions(shadowWidth, shadowHeight, shadowSlices);
	if (cascadeIndex >= shadowSlices)
		return 1.0f;

	float2 texelSize = rcp(float2(max(shadowWidth, 1), max(shadowHeight, 1)));
	float compareDepth = positionLS.z - VolumetricFogMisc.x;

	float2 uvMin = texelSize * 1.5f;
	float2 uvMax = 1.0f.xx - uvMin;
	if (any(positionLS.xy < uvMin) || any(positionLS.xy > uvMax))
		return DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(saturate(positionLS.xy), cascadeIndex), compareDepth).x;

	float center = DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy, cascadeIndex), compareDepth).x;
	float cross =
		DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy + float2(texelSize.x, 0.0f), cascadeIndex), compareDepth).x +
		DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy - float2(texelSize.x, 0.0f), cascadeIndex), compareDepth).x +
		DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy + float2(0.0f, texelSize.y), cascadeIndex), compareDepth).x +
		DirectionalShadowMap.SampleCmpLevelZero(ShadowSampler, float3(positionLS.xy - float2(0.0f, texelSize.y), cascadeIndex), compareDepth).x;

	return (center * 4.0f + cross) * rcp(8.0f);
}

float3 ToShadowLightSpace(ShadowSampling::ShadowData sD, uint eyeIndex, uint cascadeIndex, float3 positionWS)
{
	return mul(transpose(sD.ShadowMapProj[eyeIndex][cascadeIndex]), float4(positionWS, 1.0f)).xyz;
}

// Sun visibility from the captured cascades. Cascade choice, overlap blend and distance fade
// follow ShadowSampling::Get2DFilteredShadow (the vanilla split depths are post-projection).
float SampleDirectionalShadow(float3 positionWS, uint eyeIndex)
{
	if (SharedData::InInterior || SharedData::HideSky || SharedData::InMapMenu)
		return 1.0f;
	if (!VolumetricFogHasDirectionalShadowMap)
		return 1.0f;

	ShadowSampling::ShadowData sD = ShadowSampling::SharedShadowData[0];
	float shadowMapDepth = ShadowSampling::GetShadowDepth(positionWS, eyeIndex);
	if (sD.EndSplitDistances.z < shadowMapDepth)
		return 1.0f;

	uint primaryCascade = sD.EndSplitDistances.x < shadowMapDepth ? 1u : 0u;
	float3 positionLS = ToShadowLightSpace(sD, eyeIndex, primaryCascade, positionWS);
	if (any(positionLS.xy < 0.0f) || any(positionLS.xy > 1.0f))
		return 1.0f;

	float shadow = SampleDirectionalShadowPCF(positionLS, primaryCascade);

	[branch] if (primaryCascade == 0u && sD.StartSplitDistances.y < shadowMapDepth)
	{
		float3 secondaryLS = ToShadowLightSpace(sD, eyeIndex, 1u, positionWS);
		if (!any(secondaryLS.xy < 0.0f) && !any(secondaryLS.xy > 1.0f)) {
			float blend = smoothstep(0.0f, 1.0f, (shadowMapDepth - sD.StartSplitDistances.y) / max(sD.EndSplitDistances.x - sD.StartSplitDistances.y, 1e-6f));
			shadow = lerp(shadow, SampleDirectionalShadowPCF(secondaryLS, 1u), blend);
		}
	}

	float fadeFactor = 1.0f - pow(saturate(dot(positionWS, positionWS) / max(sD.ShadowLightParam.z, 1.0f)), 8.0f);
	return lerp(1.0f, shadow, fadeFactor);
}

float3 GetDirectionalLightColor()
{
	// Same conversion as Effect.hlsl's lit effects: the scene's directional light, linearised
	// under Linear Lighting exactly as the surfaces receive it.
	float llDirLightMult = (SharedData::linearLightingSettings.enableLinearLighting && !SharedData::linearLightingSettings.isDirLightLinear) ? SharedData::linearLightingSettings.dirLightMult : 1.0f;
	return Color::DirectionalLight(SharedData::DirLightColor.xyz / max(llDirLightMult, 1e-5), SharedData::linearLightingSettings.isDirLightLinear) * llDirLightMult;
}

float3 ComputeSkyLightScattering(float3 positionWS, float3 viewDirection, uint eyeIndex)
{
	float phaseG = VolumetricFogLighting.w;
	float3 skyDirection = abs(phaseG) > 0.001f ? normalize(-viewDirection * phaseG) : float3(0.0f, 0.0f, 1.0f);
	float skyVisibility = 1.0f;
	if (VolumetricFogHasSkylighting && !SharedData::InInterior) {
		float3 skylightingPosition = positionWS + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
		sh2 skylightingSH = Skylighting::sampleNoBias(SharedData::skylightingSettings, FogSkylightingProbeArray, skylightingPosition);
		float visibility = SphericalHarmonics::FuncProductIntegral(skylightingSH, SphericalHarmonics::EvaluateCosineLobe(float3(0, 0, 1))) / Math::PI;
		visibility = lerp(1.0, saturate(visibility), Skylighting::getFadeOutFactor(skylightingPosition));
		skyVisibility = Skylighting::mixDiffuse(SharedData::skylightingSettings, visibility);
	}

	float3 skyLighting;
	[branch] if (VolumetricFogHasIBL)
		skyLighting = Color::IrradianceToGamma(ImageBasedLighting::GetIBLColor(skyDirection));
	else
		skyLighting = Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(skyDirection, 1.0f))));

	return skyLighting * skyVisibility * VolumetricFogLighting.y;
}

#if defined(LIGHT_LIMIT_FIX)
float ComputeLocalLightAttenuation(float distanceSqr, float cellRadius, LightLimitFix::Light light)
{
	float distance = sqrt(max(distanceSqr, 1e-6f));
#	if defined(ISL)
	// UE biases local light integration by froxel size to avoid singular bright voxels close to the light.
	if (light.lightFlags & LightLimitFix::LightFlags::InverseSquare)
		distance = sqrt(max(distanceSqr, cellRadius * cellRadius));
	return InverseSquareLighting::GetAttenuation(distance, light);
#	else
	float intensityFactor = saturate(distance / max(light.radius, 1e-4f));
	return 1.0f - intensityFactor * intensityFactor;
#	endif
}

float3 AccumulateLocalLightScattering(uint3 coord, float3 cellOffset, float3 positionWS, float3 viewDirection, uint eyeIndex, float3 materialScattering)
{
	if (!VolumetricFogHasLocalLights)
		return 0.0f.xxx;

	// Same cluster lookup as lit effects (Effect.hlsl): eye-local UV from the eye's projection.
	float3 viewPosition = mul(FrameBuffer::CameraView[eyeIndex], float4(positionWS, 1)).xyz;
	float2 screenUV = FrameBuffer::ViewToUV(viewPosition, true, eyeIndex);

	uint clusterIndex = 0;
	if (!LightLimitFix::GetClusterIndex(screenUV, viewPosition.z, clusterIndex))
		return 0.0f.xxx;

	LightLimitFix::LightGrid grid = LightLimitFix::lightGrid[clusterIndex];
	uint lightCount = min(grid.lightCount, (uint)MAX_CLUSTER_LIGHTS);

	uint cornerEyeIndex;
	float cornerViewDepth;
	float3 cellCornerWS = ExponentialHeightFog::ComputeCellWorldPosition(coord + uint3(1, 1, 1), cellOffset, cornerEyeIndex, cornerViewDepth);
	float cellRadius = max(length(cellCornerWS - positionWS), 1.0f);

	float phaseG = VolumetricFogLighting.w;
	float3 localScattering = 0.0f.xxx;
	[loop] for (uint lightIndex = 0; lightIndex < lightCount; lightIndex++)
	{
		uint clusteredLightIndex = LightLimitFix::lightList[grid.offset + lightIndex];
		LightLimitFix::Light light = LightLimitFix::lights[clusteredLightIndex];

		if (light.lightFlags & LightLimitFix::LightFlags::Disabled)
			continue;

		float3 toLight = light.positionWS[eyeIndex].xyz - positionWS;
		float distanceSqr = dot(toLight, toLight);
		if (distanceSqr < 1e-6f)
			continue;

		float attenuation = ComputeLocalLightAttenuation(distanceSqr, cellRadius, light);
		if (attenuation < 1e-5f)
			continue;

		float3 L = toLight * rsqrt(distanceSqr);
		float phase = ExponentialHeightFog::HenyeyGreenstein(dot(L, viewDirection), phaseG);

		const bool isPointLightLinear = light.lightFlags & LightLimitFix::LightFlags::Linear;
		float3 lightColor = Color::PointLight(light.color.xyz, isPointLightLinear) * attenuation * light.fade;
		localScattering += lightColor * phase;
	}

	return localScattering * VolumetricFogLighting.z * materialScattering;
}
#endif

float4 ComputeLightScattering(uint3 coord, float3 cellOffset)
{
	uint eyeIndex;
	float viewDepth;
	float3 positionWS = ExponentialHeightFog::ComputeCellWorldPosition(coord, cellOffset, eyeIndex, viewDepth);

	float4 materialScatteringAndExtinction = VBufferA[coord];
	float extinction = materialScatteringAndExtinction.w;

	float3 viewDirection = normalize(positionWS);
	float phase = ExponentialHeightFog::HenyeyGreenstein(dot(normalize(SharedData::DirLightDirection.xyz), viewDirection), VolumetricFogLighting.w);

	float directionalShadow = SampleDirectionalShadow(positionWS, eyeIndex) *
	                          ShadowSampling::GetWorldShadow(positionWS, FrameBuffer::CameraPosAdjust[eyeIndex].xyz, eyeIndex);
	float3 directionalScattering =
		GetDirectionalLightColor() *
		VolumetricFogLighting.x *
		directionalShadow *
		phase *
		materialScatteringAndExtinction.rgb;

	float3 skyScattering = ComputeSkyLightScattering(positionWS, viewDirection, eyeIndex) * materialScatteringAndExtinction.rgb;

#if defined(LIGHT_LIMIT_FIX) && !defined(VOLUMETRIC_FOG_FAR_GRID)
	float3 localScattering = AccumulateLocalLightScattering(coord, cellOffset, positionWS, viewDirection, eyeIndex, materialScatteringAndExtinction.rgb);
#else
	float3 localScattering = 0.0f.xxx;
#endif

	float3 emissive = VolumetricFogEmissive.rgb * VolumetricFogEmissive.a * extinction;

	return float4(max(directionalScattering + skyScattering + localScattering + emissive, 0.0f.xxx), extinction);
}

[numthreads(8, 8, 4)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	if (!ExponentialHeightFog::IsInsideVolumetricGrid(dispatchID))
		return;

	uint eyeIndex;
	float viewDepth;
	float3 centerPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, 0.5f.xxx, eyeIndex, viewDepth);
	if (VolumetricFogHasConservativeDepth && IsFroxelBehindSceneDepth(dispatchID)) {
		LightScattering[dispatchID] = 0.0f.xxxx;
		return;
	}

	bool validHistory;
	float previousCenterDepth;
	float3 historyUV = ComputeHistoryVolumeUVAndDepth(centerPositionWS, eyeIndex, validHistory, previousCenterDepth);
	if (VolumetricFogHasPrevConservativeDepth && validHistory) {
		uint frontEyeIndex;
		float frontDepth;
		float3 frontPositionWS = ExponentialHeightFog::ComputeCellWorldPosition(dispatchID, float3(0.5f, 0.5f, -0.5f), frontEyeIndex, frontDepth);
		bool validFrontHistory;
		float previousFrontDepth;
		ComputeHistoryVolumeUVAndDepth(frontPositionWS, eyeIndex, validFrontHistory, previousFrontDepth);
		if (validFrontHistory) {
			historyUV.xy = saturate(FixupHistoryUV(historyUV.xy, previousFrontDepth, validHistory));
		} else {
			validHistory = false;
		}
	}

	float historyAlpha = VolumetricFogHistoryWeight;
	[flatten] if (!validHistory || any(historyUV < 0.0f) || any(historyUV >= 1.0f))
	{
		historyAlpha = 0.0f;
	}

	uint sampleCount = historyAlpha < 0.001f ? VolumetricFogHistoryMissSampleCount : 1u;
	float4 scatteringAndExtinction = 0.0f.xxxx;
	[loop] for (uint sampleIndex = 0; sampleIndex < sampleCount; sampleIndex++)
	{
		// Per-voxel random offset (UE LightScatteringCS): decorrelates the Halton jitter across
		// voxels so the temporal pattern does not crawl coherently.
		uint3 rand32Bits = Rand4DPCG32(int4(dispatchID.xyz, VolumetricFogStateFrameIndexMod8 + 8 * sampleIndex)).xyz;
		float3 rand3D = (float3(rand32Bits) / float(uint(0xffffffff))) * 2.0f - 1.0f;
		float3 cellOffset = VolumetricFogFrameJitterOffsets[sampleIndex].xyz + VolumetricFogSampleJitterMultiplier * rand3D;

		scatteringAndExtinction += ComputeLightScattering(dispatchID, cellOffset);
	}
	scatteringAndExtinction *= rcp(float(sampleCount));

	[branch] if (historyAlpha > 0.0f)
	{
		float4 history = LightScatteringHistory.SampleLevel(LinearSampler, historyUV, 0);
		history = MakePositiveFinite(history);
		scatteringAndExtinction = lerp(scatteringAndExtinction, history, historyAlpha);
	}

	LightScattering[dispatchID] = MakePositiveFinite(scatteringAndExtinction);
}
