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
#include "ExponentialHeightFog/WeatherFog.hlsli"  // (47d)

Texture3D<float4> ExponentialHeightFogIntegratedLightScattering : register(t21);
Texture3D<float4> ExponentialHeightFogIntegratedLightScatteringFar : register(t22);

namespace ExponentialHeightFog
{
    // (47b) "EHF Follows Weather" / "EHF Restores Room Fog". ExponentialHeightFog::GetVolumetricPSData() fills
    // VolumetricFogSettings.pad0 (SharedData.hlsli is untouched): x = w, how much of the game's own fog comes back
    // under the height fog (outdoors from the weather's fog distance, 1 indoors with "Restores Room Fog", 0 = off);
    // y (bits) = flags. Both 0 = the 47a2 fog exactly. (47d) Bit 1 = WeatherFog::kFlagTakeover ("CS Fog Takes Over"
    // applies: GetFog() replaces ComposeVanillaFog with the weather's fog layer).
    static const uint kFogFlagOpacityFix = 1u;  // "EHF Follows Weather" on: the fog colour is no longer weighted by its opacity twice

    float VanillaFogWeight()
    {
        return saturate(SharedData::volumetricFogSettings.pad0.x);
    }

    bool FogOpacityFix()
    {
        return (asuint(SharedData::volumetricFogSettings.pad0.y) & kFogFlagOpacityFix) != 0;
    }

    /// The game's fog (amount a_vanillaAmount after Color::FogAlpha, colour a_vanillaColor x the game's fog fade
    /// a_fade) x VanillaFogWeight() under the height fog a_heightFog (rgb colour, a opacity): one layer of opacity
    /// 1 - (1 - aw)(1 - ae). a_weatherAmount = aw, the share of the view the game's fog covers (for the physical sky's
    /// aerial perspective). With no weight: a_heightFog untouched, a_weatherAmount 0.
    float4 ComposeVanillaFog(float4 a_heightFog, float3 a_vanillaColor, float a_vanillaAmount, float a_fade, out float a_weatherAmount)
    {
        a_weatherAmount = 0.0f;
        float4 result = a_heightFog;
        const float w = VanillaFogWeight();
        [branch] if (w > 0.0f)
        {
            const float aw = w * saturate(a_vanillaAmount);
            const float ae = saturate(a_heightFog.w);
            const float opacity = 1.0f - (1.0f - aw) * (1.0f - ae);
            const float3 premultiplied = aw * (1.0f - ae) * a_vanillaColor * a_fade + ae * a_heightFog.rgb;
            a_weatherAmount = aw;
            result = float4(opacity > 1e-5f ? premultiplied / opacity : a_heightFog.rgb, opacity);
        }
        return result;
    }

    /// The physical sky's aerial perspective behind the game's fog: what the fog covers loses its share of the haze.
    float4 ScaleAerialPerspective(float4 a_ap, float a_weatherAmount)
    {
        const float keep = 1.0f - saturate(a_weatherAmount);
        return float4(a_ap.rgb * keep, lerp(1.0f, a_ap.a, keep));
    }

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
            float3 cubemapColor = DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampColorSampler, HorizonFix::ClampToHorizon(normalize(positionWS)) /* (47f) */, SharedData::exponentialHeightFogSettings.cubemapMipLevel).xyz;
            // (47b) With "EHF Follows Weather" the colour is not pre-weighted by the opacity the callers blend with
            // (upstream 22ac9859c, #2776); off = 47a2.
            fogColor = tintColor * cubemapColor * (FogOpacityFix() ? 1.0f : (1.0f - expFogFactor));
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
            directionalInscattering = directionalLightInscattering * (FogOpacityFix() ? 1.0f : (1 - dirExpFogFactor)) * SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier;  // (47b) as above
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

    /// (47d) The height fog's colour path at this pixel, as GetAnalyticalHeightFog builds it with FogOpacityFix() on
    /// (no opacity weighting): the fog colour (or the tinted cubemap), and in a_directional the sun / moon glow.
    float3 GetInscatteringColor(float3 positionWS, float3 fogColor, out float3 a_directional)
    {
#   if defined(DYNAMIC_CUBEMAPS)
        if (SharedData::exponentialHeightFogSettings.useDynamicCubemaps > 0)
        {
            float3 tintColor = lerp(fogColor, SharedData::exponentialHeightFogSettings.inscatteringTint.xyz, SharedData::exponentialHeightFogSettings.inscatteringTint.w);
            fogColor = tintColor * DynamicCubemaps::EnvReflectionsTexture.SampleLevel(SampColorSampler, HorizonFix::ClampToHorizon(normalize(positionWS)) /* (47f) */, SharedData::exponentialHeightFogSettings.cubemapMipLevel).xyz;
        }
#   endif
        a_directional = 0.0f;
        if (SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier > 0)
        {
            float3 dirLightColor = SharedData::DirLightColor.xyz;
#   if defined(PHYSICAL_SKY) && defined(COMMON_PHYS_SKY_HLSLI)
            if (SharedData::physSkyData.enabled)
                dirLightColor *= saturate(PhysSky::SampleTr(normalize(SharedData::DirLightDirection.xyz), SampColorSampler));
#   endif
            a_directional = dirLightColor * pow(saturate((dot(normalize(positionWS), SharedData::DirLightDirection.xyz) + 1) / 2), SharedData::exponentialHeightFogSettings.directionalInscatteringExponent) / Math::TAU *
                            SharedData::exponentialHeightFogSettings.directionalInscatteringMultiplier;
        }
        return fogColor;
    }

    /// (47d) "CS Fog Takes Over": the height fog and the weather's fog layer (WeatherFog.hlsli) as one fog, the
    /// volumetric fog (which carries the layer inside its range) in front. The layer takes the height fog's colour
    /// path; its sun glow is dimmed by the layer's own transmittance toward the sun; a_fade is the game's fog fade
    /// (black for additive effects). a_weatherAmount = the layer's opacity over the whole ray.
    float4 GetTakeoverFog(float3 positionWS, float3 cameraWS, float3 fogColor, float a_fade, bool a_useVolume, uint eyeIndex, float2 pixelPosition, out float a_weatherAmount)
    {
        float excludeDistance = SharedData::exponentialHeightFogSettings.startDistance;
        float weatherStart = 0.0f;
        float4 volumetricFog = float4(0.0f, 0.0f, 0.0f, 1.0f);
        bool applied = false;
        [branch] if (a_useVolume && ShouldApplyVolumetricFog())
        {
            float sceneDepth;
            volumetricFog = SampleVolumetricFog(positionWS, eyeIndex, pixelPosition, sceneDepth, applied);
            float rayDistance = length(positionWS);
            float cosAngle = sceneDepth / max(rayDistance, 1e-4f);
            if (applied && cosAngle > 0.001f)
            {
                const float volumeEnd = min(SharedData::volumetricFogSettings.EndDistance / cosAngle, rayDistance);
                excludeDistance = min(max(excludeDistance, volumeEnd), rayDistance);
                weatherStart = volumeEnd;
            }
        }

        const float4 heightFog = GetAnalyticalHeightFog(positionWS, cameraWS, fogColor, excludeDistance);
        const float weatherOpacity = WeatherFog::Opacity(positionWS, weatherStart, a_weatherAmount);

        float3 directional;
        const float3 baseColor = GetInscatteringColor(positionWS, fogColor, directional);
        const float3 weatherColor = (baseColor + directional * WeatherFog::SunTransmittance(0.0f)) * a_fade;

        // One fog: opacities combine as 1 - (1 - aw)(1 - ae), colours by each layer's share.
        const float ae = saturate(heightFog.w);
        const float opacity = 1.0f - (1.0f - weatherOpacity) * (1.0f - ae);
        const float share = weatherOpacity + ae;
        float4 result = float4(share > 1e-5f ? (weatherOpacity * weatherColor + ae * heightFog.rgb) / share : heightFog.rgb, opacity);

        [branch] if (applied)
        {
            float combinedOpacity = saturate(1.0f - volumetricFog.a * (1.0f - result.w));
            float3 combinedPremultiplied = volumetricFog.rgb + volumetricFog.a * result.rgb * result.w;
            result = float4(combinedOpacity > 1e-4f ? combinedPremultiplied / combinedOpacity : float3(0.0f, 0.0f, 0.0f), combinedOpacity);
        }
        return result;
    }

    /// (47d) Every fog of a pixel, for the five fog sites (ISSAOComposite, forward Lighting, Effect, Water x2): the
    /// height fog (with the volumetric fog in front when a_useVolume) and the game's own fog (amount a_vanillaAmount
    /// after Color::FogAlpha, colour fogColor, fade a_fade). Without the weather layer (Off, Blend Vanilla Fog,
    /// interiors, clear weather) exactly 47b: GetExponentialHeightFog + ComposeVanillaFog.
    float4 GetFog(float3 positionWS, float3 cameraWS, float3 fogColor, float a_vanillaAmount, float a_fade, bool a_useVolume, uint eyeIndex, float2 pixelPosition, out float a_weatherAmount)
    {
        float4 result;
        [branch] if (WeatherFog::Active())
        {
            result = GetTakeoverFog(positionWS, cameraWS, fogColor, a_fade, a_useVolume, eyeIndex, pixelPosition, a_weatherAmount);
        }
        else
        {
            float4 heightFog;
            [branch] if (a_useVolume)
                heightFog = GetExponentialHeightFog(positionWS, cameraWS, fogColor, eyeIndex, pixelPosition);
            else
                heightFog = GetExponentialHeightFog(positionWS, cameraWS, fogColor);
            result = ComposeVanillaFog(heightFog, fogColor, a_vanillaAmount, a_fade, a_weatherAmount);
        }
        return result;
    }
}
#endif
