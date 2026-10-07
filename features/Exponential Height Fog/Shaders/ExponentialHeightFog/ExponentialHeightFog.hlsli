#ifndef __EXPONENTIAL_HEIGHT_FOG_HLSLI__
#define __EXPONENTIAL_HEIGHT_FOG_HLSLI__

#include "Common/SharedData.hlsli"

#   if defined(DYNAMIC_CUBEMAPS)
#       include "DynamicCubemaps/DynamicCubemaps.hlsli"
#   endif

#   if defined(PHYSICAL_SKY) && defined(ISSAO_COMPOSITE)
#       undef CLOUD_SHADOWS
#       include "PhysicalSky/Common.hlsli"
#   endif

// (batch 38, A1) Volumetric fog (upstream #2361 + #2831): the integrated froxel volumes,
// built by ExponentialHeightFog::Prepass. t21/t22 here (upstream t19/t22; our t19 is
// ShadowSampling's SharedShadowData).
#include "Common/Random.hlsli"
#include "ExponentialHeightFog/VolumetricFogCommon.hlsli"

Texture3D<float4> ExponentialHeightFogIntegratedLightScattering : register(t21);
Texture3D<float4> ExponentialHeightFogIntegratedLightScatteringFar : register(t22);

namespace ExponentialHeightFog
{
    /// The analytical height fog. excludeDistance replaces Start Distance as the length of the
    /// ray that gets no fog; with volumetric fog off it is Start Distance, i.e. the 37c fog.
    float4 GetAnalyticalHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor, float excludeDistance)
    {
        float fogHeightFalloff = SharedData::exponentialHeightFogSettings.fogHeightFalloff * 0.001f;
        float fogDensity = SharedData::exponentialHeightFogSettings.fogDensity * 0.001f;
        // (batch 38, upstream #2831) second stacked layer; density 0 by default.
        float fogHeightFalloff2 = SharedData::exponentialHeightFogSettings.fogHeightFalloff2 * 0.001f;
        float fogDensity2 = SharedData::exponentialHeightFogSettings.fogDensity2 * 0.001f;
        if (fogDensity <= 0.0f && fogDensity2 <= 0.0f)
        {
            return 0.0f;
        }
        float3 viewToPos = positionWS;
        float viewToPosLength = length(viewToPos);
        float viewToPosLengthInv = rcp(viewToPosLength);

        float rayOriginTerms = fogDensity * exp2(-fogHeightFalloff * max(cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight, 0));
        float rayOriginTerms2 = fogDensity2 * exp2(-fogHeightFalloff2 * max(cameraWS.z - SharedData::exponentialHeightFogSettings.fogHeight2, 0));
        float rayLength = viewToPosLength;
        float rayDirectionZ = viewToPos.z;

        if (excludeDistance > 0)
        {
            float excludeIntersectionTime = excludeDistance * viewToPosLengthInv;
            float cameraToExclusionIntersectionZ = excludeIntersectionTime * viewToPos.z;
            float exclusionIntersectionZ = cameraWS.z + cameraToExclusionIntersectionZ;
            rayLength = (1.0f - excludeIntersectionTime) * viewToPosLength;
            rayDirectionZ = viewToPos.z - cameraToExclusionIntersectionZ;
            float exponent = fogHeightFalloff * max(exclusionIntersectionZ - SharedData::exponentialHeightFogSettings.fogHeight, 0);
            rayOriginTerms = fogDensity * exp2(-exponent);
            float exponent2 = fogHeightFalloff2 * max(exclusionIntersectionZ - SharedData::exponentialHeightFogSettings.fogHeight2, 0);
            rayOriginTerms2 = fogDensity2 * exp2(-exponent2);
        }

        float falloff = fogHeightFalloff * rayDirectionZ;
        float lineIntegral = (1.0f - exp2(-falloff)) / falloff;
        float lineIntegralTaylor = 0.69314718056f - 0.24022650695f * falloff;  // log(2) - (0.5 * (log(2)^2)) * falloff
        float exponentialHeightLineIntegralCalc = rayOriginTerms * (abs(falloff) > 0.01f ? lineIntegral : lineIntegralTaylor);
        [branch] if (fogDensity2 > 0.0f)
        {
            float falloff2 = fogHeightFalloff2 * rayDirectionZ;
            float lineIntegral2 = (1.0f - exp2(-falloff2)) / falloff2;
            float lineIntegralTaylor2 = 0.69314718056f - 0.24022650695f * falloff2;
            exponentialHeightLineIntegralCalc += rayOriginTerms2 * (abs(falloff2) > 0.01f ? lineIntegral2 : lineIntegralTaylor2);
        }
        float exponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * rayLength;

        float expFogFactor = saturate(exp2(-exponentialHeightLineIntegral));

#   if defined(DYNAMIC_CUBEMAPS)
        if (SharedData::exponentialHeightFogSettings.useDynamicCubemaps > 0)
        {
            float3 tintColor = lerp(fogColor, SharedData::exponentialHeightFogSettings.inscatteringTint.xyz, SharedData::exponentialHeightFogSettings.inscatteringTint.w);
            float3 cubemapColor = DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampColorSampler, normalize(positionWS), SharedData::exponentialHeightFogSettings.cubemapMipLevel).xyz;
            fogColor = tintColor * cubemapColor * (1.0f - expFogFactor);
        }
#   endif

        float3 directionalInscattering = 0;

        // Calculate directional light inscattering
        if (SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier > 0)
        {
            float3 dirLightColor = SharedData::DirLightColor.xyz;
#   if defined(PHYSICAL_SKY) && defined(COMMON_PHYS_SKY_HLSLI)
            if (SharedData::physSkyData.enabled) {
                float3 physSkyTransmittance = PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampColorSampler);
                dirLightColor *= saturate(physSkyTransmittance);
            }
#   endif
            float3 directionalLightInscattering = dirLightColor * pow(saturate((dot(normalize(positionWS), SharedData::DirLightDirection.xyz) + 1) / 2), SharedData::exponentialHeightFogSettings.directionalInscatteringExponent) / Math::TAU;
            float dirExponentialHeightLineIntegral = exponentialHeightLineIntegralCalc * max(rayLength - SharedData::exponentialHeightFogSettings.startDistance, 0);
            float dirExpFogFactor = saturate(exp2(-dirExponentialHeightLineIntegral));
            directionalInscattering = directionalLightInscattering * (1 - dirExpFogFactor) * SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier;
        }

        fogColor += directionalInscattering;
        return float4(fogColor, 1.0f - expFogFactor);
    }

    /// The 37c height fog (no volumetric part). Every caller that has not opted in to the
    /// volumetric fog keeps using this one.
    float4 GetExponentialHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor)
    {
        return GetAnalyticalHeightFog(positionWS, cameraWS, fogColor, SharedData::exponentialHeightFogSettings.startDistance);
    }

    bool ShouldApplyVolumetricFog()
    {
        return SharedData::exponentialHeightFogSettings.enabled != 0 && SharedData::volumetricFogSettings.Enabled != 0;
    }

    /// Looks the froxel volumes up at a camera-relative position. Returns premultiplied
    /// in-scattered light in rgb and transmittance in a; (0,0,0,1) when there is no volume
    /// (then valid is false). pixelPosition only seeds the upsampling jitter.
    float4 SampleVolumetricFog(float3 positionWS, uint eyeIndex, float2 pixelPosition, out float sceneDepth, out bool valid)
    {
        float4 result = float4(0.0f, 0.0f, 0.0f, 1.0f);

        float4 clipPosition = mul(FrameBuffer::CameraViewProj[eyeIndex], float4(positionWS, 1.0f));
        sceneDepth = max(clipPosition.w, SharedData::CameraData.y);

        uint volumeWidth;
        uint volumeHeight;
        uint volumeDepth;
        ExponentialHeightFogIntegratedLightScattering.GetDimensions(volumeWidth, volumeHeight, volumeDepth);
        valid = clipPosition.w > 0.0f && volumeWidth != 0 && volumeHeight != 0 && volumeDepth != 0;

        [branch] if (valid)
        {
            const float2 volumeSize = float2(volumeWidth, volumeHeight);
            float2 volumeUV = saturate(clipPosition.xy / clipPosition.w * float2(0.5f, -0.5f) + 0.5f);
            volumeUV = Stereo::ConvertToStereoUV(volumeUV, eyeIndex);

            // Hide the froxel blocks: jitter the lookup by up to one froxel; DLSS/TAA averages it.
            [branch] if (SharedData::volumetricFogSettings.UpsampleJitter > 0.0f)
            {
                float2 noise = float2(
                    Random::InterleavedGradientNoise(pixelPosition, SharedData::FrameCount),
                    Random::InterleavedGradientNoise(pixelPosition.yx + 19.19f, SharedData::FrameCount));
                volumeUV += (noise * 2.0f - 1.0f) * SharedData::volumetricFogSettings.UpsampleJitter / volumeSize;
            }

            // Clamp to texel centres of this eye's half (all of it outside VR).
            float2 uvMin = 0.5f / volumeSize;
            float2 uvMax = 1.0f - uvMin;
#if defined(VR)
            uvMin.x += eyeIndex == 0 ? 0.0f : 0.5f;
            uvMax.x -= eyeIndex == 0 ? 0.5f : 0.0f;
#endif
            volumeUV = clamp(volumeUV, uvMin, uvMax);

            float nearZ = VolumetricDepthToNormalizedSlice(sceneDepth, SharedData::volumetricFogSettings.NearGridZParams.xyz, SharedData::volumetricFogSettings.NearGridZParams.w);
            float zTexel = 0.5f / float(volumeDepth);
            float4 nearFog = ExponentialHeightFogIntegratedLightScattering.SampleLevel(SampColorSampler, float3(volumeUV, clamp(nearZ, zTexel, 1.0f - zTexel)), 0);

            float4 farFog = float4(0.0f, 0.0f, 0.0f, 1.0f);
            uint farWidth;
            uint farHeight;
            uint farDepth;
            ExponentialHeightFogIntegratedLightScatteringFar.GetDimensions(farWidth, farHeight, farDepth);
            [branch] if (SharedData::volumetricFogSettings.FarEnabled != 0 && sceneDepth > SharedData::volumetricFogSettings.NearGridEndDistance &&
                         farWidth != 0 && farHeight != 0 && farDepth != 0)
            {
                float2 farSize = float2(farWidth, farHeight);
                float2 farUVMin = 0.5f / farSize;
                float2 farUVMax = 1.0f - farUVMin;
#if defined(VR)
                farUVMin.x += eyeIndex == 0 ? 0.0f : 0.5f;
                farUVMax.x -= eyeIndex == 0 ? 0.5f : 0.0f;
#endif
                float farZ = VolumetricDepthToNormalizedSlice(sceneDepth, SharedData::volumetricFogSettings.FarGridZParams.xyz, SharedData::volumetricFogSettings.FarGridZParams.w);
                float farZTexel = 0.5f / float(farDepth);
                farFog = ExponentialHeightFogIntegratedLightScatteringFar.SampleLevel(SampColorSampler, float3(clamp(volumeUV, farUVMin, farUVMax), clamp(farZ, farZTexel, 1.0f - farZTexel)), 0);
            }

            // Serial composition: the far volume's in-scatter is attenuated by the near transmittance.
            if (sceneDepth > SharedData::volumetricFogSettings.StartDistance)
                result = float4(nearFog.rgb + nearFog.a * farFog.rgb, nearFog.a * farFog.a);
        }

        return result;
    }

    /// Height fog with the volumetric fog in front of it. The analytical fog only covers the
    /// part of the ray beyond the volumetric fog's range, and the two are composited front
    /// (volume) to back (analytical). Exactly the 37c fog whenever the volumes are off.
    float4 GetExponentialHeightFog(float3 positionWS, float3 cameraWS, float3 fogColor, uint eyeIndex, float2 pixelPosition)
    {
        float excludeDistance = SharedData::exponentialHeightFogSettings.startDistance;
        float4 volumetricFog = float4(0.0f, 0.0f, 0.0f, 1.0f);
        bool applied = false;

        [branch] if (ShouldApplyVolumetricFog())
        {
            float sceneDepth;
            volumetricFog = SampleVolumetricFog(positionWS, eyeIndex, pixelPosition, sceneDepth, applied);
            // The volumes end at a view depth; along this ray that is a longer distance off-axis.
            float rayDistance = length(positionWS);
            float cosAngle = sceneDepth / max(rayDistance, 1e-4f);
            if (applied && cosAngle > 0.001f)
                excludeDistance = min(max(excludeDistance, SharedData::volumetricFogSettings.EndDistance / cosAngle), rayDistance);
        }

        float4 analyticalFog = GetAnalyticalHeightFog(positionWS, cameraWS, fogColor, excludeDistance);

        float4 result = analyticalFog;
        [branch] if (applied)
        {
            float analyticalTransmittance = 1.0f - analyticalFog.w;
            float combinedTransmittance = volumetricFog.a * analyticalTransmittance;
            float combinedOpacity = saturate(1.0f - combinedTransmittance);
            float3 combinedPremultiplied = volumetricFog.rgb + volumetricFog.a * analyticalFog.rgb * analyticalFog.w;
            result = float4(combinedOpacity > 1e-4f ? combinedPremultiplied / combinedOpacity : float3(0.0f, 0.0f, 0.0f), combinedOpacity);
        }
        return result;
    }
}
#endif
