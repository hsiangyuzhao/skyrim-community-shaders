
#include "Common/Color.hlsli"
#include "Common/FrameBuffer.hlsli"
#include "Common/GBuffer.hlsli"
#include "Common/MotionBlur.hlsli"
#include "Common/SharedData.hlsli"
#include "Common/Spherical Harmonics/SphericalHarmonics.hlsli"
#include "Common/VR.hlsli"

Texture2D<float3> SpecularTexture : register(t0);
Texture2D<unorm float3> AlbedoTexture : register(t1);
Texture2D<unorm float3> NormalRoughnessTexture : register(t2);
Texture2D<float3> MasksTexture : register(t3);

RWTexture2D<float4> MainRW : register(u0);
RWTexture2D<float4> NormalTAAMaskSpecularMaskRW : register(u1);
RWTexture2D<float2> MotionVectorsRW : register(u2);
Texture2D<float> DepthTexture : register(t4);

#if defined(DYNAMIC_CUBEMAPS)
Texture2D<float3> ReflectanceTexture : register(t5);
TextureCube<float3> EnvTexture : register(t6);
TextureCube<float3> EnvReflectionsTexture : register(t7);

SamplerState LinearSampler : register(s0);
#endif

#if defined(SKYLIGHTING)
#	include "Skylighting/Skylighting.hlsli"

Texture3D<sh2> SkylightingProbeArray : register(t8);
Texture2DArray<float3> stbn_vec3_2Dx1D_128x128x64 : register(t9);

#endif

#if defined(SSGI)
Texture2D<float4> SsgiAoTexture : register(t10);
Texture2D<float4> SsgiYTexture : register(t11);
Texture2D<float4> SsgiCoCgTexture : register(t12);
Texture2D<float4> SsgiSpecularTexture : register(t13);

void SampleSSGI(uint2 pixCoord, float3 normalWS, out float ao, out float3 il)
{
	ao = 1 - SsgiAoTexture[pixCoord];
	float4 ssgiIlYSh = SsgiYTexture[pixCoord];
	// without ZH hallucination
	// float ssgiIlY = SphericalHarmonics::FuncProductIntegral(ssgiIlYSh, SphericalHarmonics::EvaluateCosineLobe(normalWS));
	float ssgiIlY = SphericalHarmonics::SHHallucinateZH3Irradiance(ssgiIlYSh, normalWS);
	float2 ssgiIlCoCg = SsgiCoCgTexture[pixCoord];
	il = max(0, Color::YCoCgToRGB(float3(ssgiIlY, ssgiIlCoCg)));
}

void SampleSSGISpecular(uint2 pixCoord, sh2 lobe, out float ao, out float3 il, in float3 normal, in float3 view, in float roughness)
{
	ao = 1 - SsgiAoTexture[pixCoord].x;
	float NdotV = dot(normal, view);
	ao = Color::SpecularAOLagarde(saturate(NdotV), ao, roughness);
#	if defined(SSRT)
	if (SharedData::ssrtSettings.EnableSpecular) {
		il = 0;
		return;
	}
#	else
	float4 ssgiIlYSh = SsgiYTexture[pixCoord];
	float ssgiIlY = SphericalHarmonics::FuncProductIntegral(ssgiIlYSh, lobe);
	float2 ssgiIlCoCg = SsgiCoCgTexture[pixCoord].xy;

	// pi to compensate for the /pi in specularLobe
	// i don't think there really should be a 1/PI but without it the specular is too strong
	// reflectance being ambient reflectance doesn't help either
	il = max(0, Color::YCoCgToRGB(float3(ssgiIlY, ssgiIlCoCg / Math::PI)));

	// HQ spec
	float4 hq_spec = SsgiSpecularTexture[pixCoord];
	ao *= 1 - hq_spec.a;
	il += hq_spec.rgb;
#	endif
}
#endif

#if defined(IBL)
#	if !defined(DYNAMIC_CUBEMAPS)
#		undef IBL
#	else
#		define IBL_DEFERRED
#		include "IBL/IBL.hlsli"
#	endif
#endif

#if defined(ENV_AMBIENT)
#	if !defined(DYNAMIC_CUBEMAPS)
#		undef ENV_AMBIENT
#	else
#		include "EnvironmentAmbient/EnvAmbient.hlsli"
#	endif
#endif

#if defined(SSRT)
Texture2D<float4> SSRTexture : register(t16);
#endif

// Skylighting's diffuse visibility is needed by the ambient estimate's IBL term and by Environment
// Ambient. Computed once per pixel where either is compiled in - a plain SSGI build with neither
// must not pay for a probe fetch nothing reads. IBL is #undef'd above without DYNAMIC_CUBEMAPS, so
// inside an IBL build this condition is exactly IBL.hlsli's own `SKYLIGHTING && !INTERIOR` and the
// GetIBLColor overload always lines up.
#if defined(SKYLIGHTING) && defined(DYNAMIC_CUBEMAPS) && !defined(INTERIOR) && (defined(IBL) || defined(ENV_AMBIENT))
#	define COMPOSITE_AMBIENT_SKY
#endif

#if defined(PHYSICAL_SKY)
#	define PS_DEFERRED_RSRCS
#	define PS_DEFERRED_SAMPLERS
#	include "PhysicalSky/Common.hlsli"
// Texture3D<float4> TexApLut : register(t15);
// SamplerState SampSv : register(s2); 
#endif

[numthreads(8, 8, 1)] void main(uint3 dispatchID : SV_DispatchThreadID) {
	// Early exit if dispatch thread is outside screen bounds
	if (any(dispatchID.xy >= uint2(SharedData::BufferDim.xy)))
		return;

	float2 uv = float2(dispatchID.xy + 0.5) * SharedData::BufferDim.zw;
	uv *= FrameBuffer::DynamicResolutionParams2.xy;  // adjust for dynamic res

	uint eyeIndex = Stereo::GetEyeIndexFromTexCoord(uv);
	uv = Stereo::ConvertFromStereoUV(uv, eyeIndex);

	float3 normalGlossiness = NormalRoughnessTexture[dispatchID.xy];
	float3 normalVS = GBuffer::DecodeNormal(normalGlossiness.xy);

	float3 diffuseColor = MainRW[dispatchID.xy].xyz;
	float3 specularColor = SpecularTexture[dispatchID.xy];
	float3 albedo = AlbedoTexture[dispatchID.xy];

	float depth = DepthTexture[dispatchID.xy];
	float4 positionWS = float4(2 * float2(uv.x, -uv.y + 1) - 1, depth, 1);
	positionWS = mul(FrameBuffer::CameraViewProjInverse[eyeIndex], positionWS);
	positionWS.xyz = positionWS.xyz / positionWS.w;

	if (depth == 1.0)
		MotionVectorsRW[dispatchID.xy] = MotionBlur::GetSSMotionVector(positionWS, positionWS, eyeIndex);  // Apply sky motion vectors

	float glossiness = normalGlossiness.z;

	float3 linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
	float3 normalWS = normalize(mul(FrameBuffer::CameraViewInverse[eyeIndex], float4(normalVS, 0)).xyz);

#if defined(SSGI) || defined(ENV_AMBIENT)

#	if defined(SSGI)
	float ssgiAo;
	float3 ssgiIl;
	SampleSSGI(dispatchID.xy, normalWS, ssgiAo, ssgiIl);
#	else
	// No Screen Space GI: MultiBounceAO(albedo, 1) is the identity, so the ambient separation below
	// is a round trip and the block only exists to let Environment Ambient replace the term.
	float ssgiAo = 1.0;
#	endif

	// Skylighting diffuse visibility, shared by every consumer below.
	//
	// ambientSkyVisibility: what the forward path (Lighting.hlsl:3230-3236) and the SSRT fallback
	//     (ssrt_raymarch.hlsl:602-612) attenuate with - upward-normal boost and MinDiffuseVisibility
	//     floor applied, so it can exceed 1. Reproduced verbatim; parity includes the quirks.
	// ambientEnclosure: how closed-off the point is, ~1 in the open for every orientation. The raw
	//     integral answers "what fraction of my hemisphere is sky", which on open flat ground is
	//     only about 0.5 * (1 + n.z) - a vertical wall in the middle of a field reads 0.5. Dividing
	//     that reference out turns the signal from orientation into enclosure, which is what both
	//     the occlusion proxy and the hue fallback actually want.
	//
	// Declared only where a consumer exists, so a build with neither IBL nor Environment Ambient
	// compiles to the same instructions it did before this block was introduced.
#	if defined(IBL) || defined(ENV_AMBIENT)
	float ambientSkyVisibility = 1.0;
	float ambientEnclosure = 1.0;
#	endif
#	ifdef COMPOSITE_AMBIENT_SKY
	{
#		if defined(VR)
		float3 ambientPositionMS = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
		float3 ambientPositionMS = positionWS.xyz;
#		endif

		sh2 ambientSkylightingSH = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, dispatchID.xy, ambientPositionMS, normalWS);

		// Fold the lower hemisphere up, as the forward path and the fallback both do.
		float3 ambientFoldedNormal = normalize(float3(normalWS.xy, max(0, normalWS.z)));

		float rawVisibility = SphericalHarmonics::FuncProductIntegral(ambientSkylightingSH, SphericalHarmonics::EvaluateCosineLobe(ambientFoldedNormal)) / Math::PI;
		rawVisibility = saturate(rawVisibility);
		rawVisibility = lerp(1.0, rawVisibility, Skylighting::getFadeOutFactor(ambientPositionMS));

		ambientSkyVisibility = rawVisibility * (1.0 + saturate(normalWS.z) * (1.0 - SharedData::skylightingSettings.MinDiffuseVisibility));
		ambientSkyVisibility = Skylighting::mixDiffuse(SharedData::skylightingSettings, ambientSkyVisibility);

		// The open-sky reference is taken from normalWS.z rather than the folded normal, which is
		// NaN for a straight-down normal (normalize of a zero vector).
		ambientEnclosure = saturate(rawVisibility / max(0.5 * (1.0 + saturate(normalWS.z)), 1e-3));
	}
#	endif

	// A_est: this block's estimate of the ambient light MAIN already contains, subtracted back out
	// below. Only its LUMINANCE is corrected from the G-buffer (Masks.z), so its chroma has to be
	// built the way the forward path built it (Lighting.hlsl:3217-3252) or the difference survives
	// as an additive tint - see the ENV_AMBIENT note further down.
	float3 directionalAmbientColor = Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(normalWS, 1.0))));

#	if defined(IBL)
	// With Image Based Lighting loaded the forward ambient is not DALC at all: DALC is scaled by
	// DALCAmount (0.33 by default, Lighting.hlsl:3224) and an SH probe term is added on top
	// (:3245-3252). Estimating it as 100% DALC and no probe therefore got the chroma badly wrong -
	// about three times too much DALC, none of the probe - which is why the residual tint tracked
	// the IBL toggle. The UseStaticIBL branch at :3220 is gated on !inWorld, so it cannot apply to
	// the deferred world G-buffer and is deliberately not reproduced.
	[branch] if (SharedData::iblSettings.EnableDiffuseIBL != 0 &&
		(!SharedData::InInterior || SharedData::iblSettings.EnableInterior != 0)) {
		directionalAmbientColor *= SharedData::iblSettings.DALCAmount;

#		ifdef COMPOSITE_AMBIENT_SKY
		float3 iblColor = ImageBasedLighting::GetIBLColor(-normalWS, ambientSkyVisibility);
#		else
		float3 iblColor = ImageBasedLighting::GetIBLColor(-normalWS);
#		endif
		iblColor = Color::IrradianceToGamma(Color::Saturation(iblColor, SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale);

		// The forward path shapes only the DALC half with Skylighting's albedo-tinted MultiBounceAO
		// (Skylighting.hlsli:53, reached from Lighting.hlsl:3639 after :3633 has taken the probe
		// term back out) and re-adds the probe term unshaped at :3643. Mirroring that split is what
		// sets the DALC-to-probe ratio in the enclosed places where the tint was worst.
		float3 dalcPart = directionalAmbientColor * albedo;
#		ifdef COMPOSITE_AMBIENT_SKY
		dalcPart = Color::IrradianceToGamma(Color::IrradianceToLinear(dalcPart) *
			Color::MultiBounceAO(Color::IrradianceToLinear(albedo / Color::PBRLightingScale), ambientSkyVisibility));
#		endif

		directionalAmbientColor = dalcPart + iblColor * albedo;
	} else {
		directionalAmbientColor *= albedo;
	}
#	else
	directionalAmbientColor *= albedo;
#	endif

	directionalAmbientColor = Color::RGBToYCoCg(directionalAmbientColor);
	directionalAmbientColor.x = MasksTexture[dispatchID.xy].z;
	directionalAmbientColor = Color::YCoCgToRGB(directionalAmbientColor);
	directionalAmbientColor = max(0, directionalAmbientColor);

	float maxScale = 1.0;
	if (directionalAmbientColor.x > 0.0)
		maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
	if (directionalAmbientColor.y > 0.0)
		maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
	if (directionalAmbientColor.z > 0.0)
		maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
	directionalAmbientColor *= maxScale;

	diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);

	linDiffuseColor = Color::IrradianceToLinear(diffuseColor);

	float3 linAlbedo = Color::IrradianceToLinear(albedo / Color::PBRLightingScale);

#	if defined(SSGI)
	// Indirect lighting albedo must keep the upstream (2ea3b3adc) calibration, which divided
	// albedo by the then-unconditional PBRLightingScale of 0.65 to compensate for lowering the
	// default GIStrength from 1.5 to 1.0. PBRLightingScale is now 1.0 under Linear Lighting, so
	// reusing it here silently darkened SSGI IL by 1/0.65 (~1.54x) in every LL configuration.
	// Use the literal 0.65 so IL magnitude is identical with and without Linear Lighting.
	// The AO path above intentionally keeps Color::PBRLightingScale (behaviour unchanged).
	float3 linAlbedoIl = Color::IrradianceToLinear(albedo / 0.65);
#	endif

#	if defined(ENV_AMBIENT)
	// Environment Ambient replaces the ambient term that was just separated out of MAIN, so it can
	// never double-light: the vanilla contribution is subtracted whether or not L1 is active.
	//
	// On the separation above: with Color::YCoCgToRGB, replacing Y while keeping Co/Cg adds the same
	// delta to all three channels (R = Y - Cg + Co, G = Y + Cg, B = Y - Cg - Co), so the subtracted
	// term is exactly `A_est + (Masks.z - Y(A_est)) * (1,1,1)`. Its chroma is A_est's, NOT the
	// blended pixel's - the pixel only enters through the maxScale clamp. The subtraction therefore
	// cannot hue-shift the direct-light residual by scaling pixel chroma.
	//
	// It does leave `chroma(real forward ambient) - chroma(A_est)` as an additive term, because only
	// the luminance is corrected. That residual is why the observed tint tracked the IBL toggle, and
	// building A_est the way the forward path builds it is what removes the bulk of it. What stays
	// is second order: the forward pass evaluates DALC along its own ambientNormal (hair and skin
	// override it) and applies maxScale before the shaping, neither of which is reconstructible
	// here.
	float3 envAmbient = 0.0;
	float envAmbientOcclusion = 1.0;

	bool envAmbientActive = SharedData::envAmbientSettings.Enabled != 0 && depth < 1.0;
#		if defined(SSRT)
	// SSRT diffuse drives the forward directional ambient to zero via AmbientMult (so Masks.z and
	// therefore directionalAmbientColor collapse to ~0) and adds its own cubemap ambient in
	// ssrt_diffuse_composite.hlsl. Running both would double-light, so SSRT wins. Read from the
	// cbuffer at runtime (DiffuseMult is already gated on EnableDiffuse in
	// ScreenSpaceRayTracing::GetCommonBufferData) so toggling SSRT needs no composite recompile.
	envAmbientActive = envAmbientActive && !(SharedData::ssrtSettings.DiffuseMult > 0.0);
#		endif
#		if defined(INTERIOR)
	envAmbientActive = envAmbientActive && SharedData::envAmbientSettings.EnableInterior != 0;
#		endif

	[branch] if (envAmbientActive)
		envAmbient = EnvironmentAmbient::Evaluate(normalWS, albedo, dispatchID.xy, ambientSkyVisibility, ambientEnclosure, envAmbientOcclusion);
#	endif

	float3 multiBounceAO = Color::MultiBounceAO(linAlbedo, ssgiAo);

	linDiffuseColor *= sqrt(multiBounceAO);

#	if defined(ENV_AMBIENT)
	// Vanilla ambient contribution, in linear space, bit-identical to the #else branch below.
	float3 ambientIrradiance = Color::IrradianceToLinear(directionalAmbientColor) * multiBounceAO;

	// The environment term is added *linearly* (below), so it is kept out of ambientIrradiance and
	// the vanilla term is faded out by the same Blend instead of being lerped against it. Blend = 0
	// therefore still reproduces vanilla exactly, and Blend = 1 reproduces the reference exactly.
	float3 envIrradianceAdd = 0.0;

	[branch] if (envAmbientActive) {
		float blend = saturate(SharedData::envAmbientSettings.Blend);

		// ssrt_raymarch.hlsl:632-641: ao = occlusion * ssgiVisibility, then the whole env colour is
		// multiplied by MultiBounceAO taken on the *gamma* g-buffer albedo (ssrt_raymarch.hlsl:470
		// reads AlbedoTexture raw). Kept in that space on purpose: parity includes the quirks.
		float envAo = saturate(envAmbientOcclusion * ssgiAo);
		float3 envIrradiance = Color::IrradianceToLinear(envAmbient) * Color::MultiBounceAO(albedo, envAo);

		// Enclosure-driven hue fallback. The fallback this feature ports does not merely dim the
		// cubemap where the sky is blocked: rays that hit nearby geometry take that geometry's
		// screen radiance instead of the cubemap (ssrt_raymarch.hlsl:642), so an enclosed hemisphere
		// is lit by bounced local colour rather than by a darkened sky. Attenuating a sky-blue
		// cubemap achromatically produces exactly what the A/B reported - dark, but still blue.
		//
		// Falling back to the vanilla ambient term fixes hue and level in one step: it carries the
		// true forward ambient luminance (via Masks.z, so it is already dark in closed-off places)
		// and DALC-plus-probe chroma with no sky-blue in it. It is also the term L1 is replacing, so
		// at full enclosure L1 degrades to vanilla rather than to something invented.
		//
		// Endpoints: openWeight 1 leaves open areas on pure cubemap env, unchanged; openWeight 0 is
		// the vanilla ambient. Because the occluded cubemap term is lerped away at the enclosed end,
		// Occlusion Strength no longer compounds with this and can stay at the fallback's 1.0.
		[branch] if (SharedData::envAmbientSettings.EnclosureFallback != 0) {
			float enclosure = ambientEnclosure;
#	if defined(SSGI)
			// Screen Space GI resolves contact and creases far finer than the Skylighting probe
			// grid, and it is the same signal the fallback multiplied in (ssrt_raymarch.hlsl:634).
			enclosure *= ssgiAo;
#	endif
			float openWeight = pow(saturate(enclosure), max(SharedData::envAmbientSettings.HueFalloff, 1e-3));
			envIrradiance = lerp(ambientIrradiance, envIrradiance, openWeight);
		}

		[branch] if (SharedData::envAmbientSettings.LinearComposite != 0) {
			// PARITY PATH. ssrt_diffuse_composite.hlsl:20 adds the cubemap ambient to MAIN inside
			// linear space. The gamma path below instead converts both sides to gamma, adds, and
			// converts back; without Linear Lighting that is pow(a^(1/1.6) + b^(1/1.6), 1.6), a
			// soft-add which both lifts the sum (up to 2^0.375 = 1.30x where the two terms are
			// equal) and pulls each channel towards the ambient's hue. Worked example, direct
			// (1.0, 0.2, 0.1) plus ambient (0.2, 0.2, 0.3): linear gives (1.047, 0.307, 0.323),
			// R/G = 3.41; gamma gives (1.2, 0.4, 0.4), R/G = 3.00 at 1.23x the luminance. That is
			// symptom S1 exactly - brighter, less saturated, tinted towards the ambient - and it is
			// why L1 read washed out against the reference. (Identity when Linear Lighting is on,
			// where IrradianceToGamma/Linear are no-ops.)
			ambientIrradiance *= 1.0 - blend;
			envIrradianceAdd = envIrradiance * blend;
		} else {
			// Legacy/diagnostic: fold the environment term into the vanilla gamma soft-add.
			ambientIrradiance = lerp(ambientIrradiance, envIrradiance, blend);
		}
	}

	diffuseColor = Color::IrradianceToGamma(linDiffuseColor);
	diffuseColor += Color::IrradianceToGamma(ambientIrradiance);
	linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
	linDiffuseColor += envIrradianceAdd;
#	else
	diffuseColor = Color::IrradianceToGamma(linDiffuseColor);
	diffuseColor += Color::IrradianceToGamma(Color::IrradianceToLinear(directionalAmbientColor) * multiBounceAO);
	linDiffuseColor = Color::IrradianceToLinear(diffuseColor);
#	endif

#	if defined(SSGI)
	linDiffuseColor += ssgiIl * linAlbedoIl;
#	endif
#endif

	float3 color = linDiffuseColor + specularColor;

#if defined(DYNAMIC_CUBEMAPS)

	float3 reflectance = ReflectanceTexture[dispatchID.xy];

	if (reflectance.x > 0.0 || reflectance.y > 0.0 || reflectance.z > 0.0) {
		float3 V = normalize(positionWS.xyz);
		float3 R = reflect(V, normalWS);

		float roughness = 1.0 - glossiness;
		float level = roughness * 7.0;

		sh2 specularLobe = SphericalHarmonics::FauxSpecularLobe(normalWS, -V, roughness);

		float3 finalIrradiance = 0;

        float directionalAmbientColorSpecular = Color::RGBToLuminance(max(0, mul(SharedData::DirectionalAmbient, float4(R, 1.0)))) * Color::ReflectionNormalisationScale;

#	if defined(INTERIOR)
		float3 specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);

        float specularIrradianceLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));

#		if defined(IBL)
		float3 iblColor = 0;
		if (SharedData::iblSettings.EnableDiffuseIBL && SharedData::iblSettings.EnableInterior) {
			directionalAmbientColorSpecular *= SharedData::iblSettings.DALCAmount;
			iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-R), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
			float iblColorLuminance = Color::RGBToLuminance(Color::IrradianceToGamma(iblColor));
			directionalAmbientColorSpecular += iblColorLuminance;
		}
#		endif
        specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;

        finalIrradiance = Color::IrradianceToLinear(specularIrradiance);
#	elif defined(SKYLIGHTING)
#		if defined(VR)
		float3 positionMS = positionWS.xyz + FrameBuffer::CameraPosAdjust[eyeIndex].xyz - FrameBuffer::CameraPosAdjust[0].xyz;
#		else
		float3 positionMS = positionWS.xyz;
#		endif

		sh2 skylighting = Skylighting::sample(SharedData::skylightingSettings, SkylightingProbeArray, stbn_vec3_2Dx1D_128x128x64, dispatchID.xy, positionMS.xyz, R);

		float skylightingSpecular = SphericalHarmonics::FuncProductIntegral(skylighting, specularLobe);
		skylightingSpecular = saturate(skylightingSpecular);
		skylightingSpecular = Skylighting::mixSpecular(SharedData::skylightingSettings, skylightingSpecular);

#		if defined(IBL)
		float3 iblColor = 0;
		if (SharedData::iblSettings.EnableDiffuseIBL) {
			directionalAmbientColorSpecular *= SharedData::iblSettings.DALCAmount;
			iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-R, skylightingSpecular), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
			float iblColorLuminance = Color::RGBToLuminance(Color::IrradianceToGamma(iblColor));
			directionalAmbientColorSpecular += iblColorLuminance;
		}
#		endif

		float3 specularIrradianceReflections = 0.0;

		if (skylightingSpecular > 0.0){
			specularIrradianceReflections = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);

			float specularIrradianceLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));

			specularIrradianceReflections = (specularIrradianceReflections / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;

			specularIrradianceReflections = Color::IrradianceToLinear(specularIrradianceReflections);

		}

		float3 specularIrradiance = 0.0;

		if (skylightingSpecular < 1.0){
			specularIrradiance = EnvTexture.SampleLevel(LinearSampler, R, level);

			float specularIrradianceLuminance = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, R, 15));

			directionalAmbientColorSpecular = Color::IrradianceToLinear(directionalAmbientColorSpecular);
			directionalAmbientColorSpecular *= skylightingSpecular;
			directionalAmbientColorSpecular = Color::IrradianceToGamma(directionalAmbientColorSpecular);

			specularIrradiance = (specularIrradiance / max(specularIrradianceLuminance, 0.001)) * directionalAmbientColorSpecular;

			specularIrradiance = Color::IrradianceToLinear(specularIrradiance);
		}

		finalIrradiance = lerp(specularIrradiance, specularIrradianceReflections, skylightingSpecular);
#	else
#		if defined(IBL)
		float3 iblColor = 0;
		if (SharedData::iblSettings.EnableDiffuseIBL) {
			directionalAmbientColorSpecular *= SharedData::iblSettings.DALCAmount;
			iblColor += Color::Saturation(ImageBasedLighting::GetIBLColor(-R), SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale;
			float iblColorLuminance = Color::RGBToLuminance(Color::IrradianceToGamma(iblColor));
			directionalAmbientColorSpecular += iblColorLuminance;
		}
#		endif
		float3 specularIrradianceReflections = EnvReflectionsTexture.SampleLevel(LinearSampler, R, level);

        float specularIrradianceReflectionsLuminance = Color::RGBToLuminance(EnvReflectionsTexture.SampleLevel(LinearSampler, R, 15));

	    specularIrradianceReflections = (specularIrradianceReflections / max(specularIrradianceReflectionsLuminance, 0.001)) * directionalAmbientColorSpecular;

		finalIrradiance = Color::IrradianceToLinear(specularIrradianceReflections);
#	endif

#	if defined(SSGI)
		float ssgiAo;
		float3 ssgiIlSpecular;
		SampleSSGISpecular(dispatchID.xy, specularLobe, ssgiAo, ssgiIlSpecular, normalWS, V, roughness);

		finalIrradiance = (finalIrradiance * ssgiAo);

		ssgiIlSpecular = Color::RGBToYCoCg(ssgiIlSpecular);
		ssgiIlSpecular = max(0, Color::YCoCgToRGB(float3(ssgiIlSpecular.x, lerp(ssgiIlSpecular.yz, Color::RGBToYCoCg(finalIrradiance).yz, 0.5))));

		finalIrradiance += ssgiIlSpecular;
#	endif

#	if defined(SSRT)
		if (SharedData::ssrtSettings.EnableSpecular) {
			float4 ssrIrradiance = SSRTexture[dispatchID.xy];
			finalIrradiance = any(ssrIrradiance.rgb > 0) ? ssrIrradiance.rgb : finalIrradiance;
		}
#	endif

		color += reflectance * finalIrradiance;
	}

#endif

	color = Color::IrradianceToGamma(color);

#if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled && depth < 1 - 1e-6) {
		const float4 apSample = PhysSky::SampleAp(normalize(positionWS.xyz), dispatchID.xy, length(positionWS.xyz), PhysSky::SampSv);
		color.xyz = color.xyz * apSample.w + apSample.xyz;
	}
#endif

#if defined(PHYSICAL_SKY)
	if (SharedData::physSkyData.enabled && depth < 1 - 1e-6) {
		const float4 apSample = PhysSky::SampleAp(normalize(positionWS.xyz), dispatchID.xy, length(positionWS.xyz), PhysSky::SampSv);
		color.xyz = color.xyz * apSample.w + apSample.xyz;
	}
#endif

#if defined(DEBUG)

#	if defined(VR)
	uv.x += (eyeIndex ? 0.1 : -0.1);
#	endif  // VR

	if (uv.x < 0.5 && uv.y < 0.5) {
		color = color;
	} else if (uv.x < 0.5) {
		color = albedo;
	} else if (uv.y < 0.5) {
		color = normalVS;
	} else {
		color = glossiness;
	}

#endif

	MainRW[dispatchID.xy] = float4(color, 1.0);
	NormalTAAMaskSpecularMaskRW[dispatchID.xy] = float4(GBuffer::EncodeNormalVanilla(normalVS), 0.0, 0.0);
}