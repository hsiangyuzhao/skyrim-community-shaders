
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
// (directional env) Full-resolution bent normal + aperture from Screen Space GI's pipeline:
// RG = octahedral world-space bent normal, B = aperture (0 = pinhole, 1 = open hemisphere),
// A = spare. t20 is the composite's first free SRV slot (t14/t15 IBL, t16/t19 SSRT, t17/t18
// Physical Sky, t17 also SharedData's DepthTexture).
//
// (directional env v2) No longer read here -- the v2 channel consumes the pre-integrated
// irradiance below instead of sampling the cubemap along one direction. The surface, its
// binding and its decoder stay: the whole vector-domain pipeline still produces it every
// frame, reserved for the planned specular-occlusion consumer.
Texture2D<unorm float4> SsgiBentNormalTexture : register(t20);
// (directional env v2) Full-resolution hemisphere environment irradiance from Screen Space
// GI's bitmask sweep: RGB = linear irradiance integrated over the UNOCCLUDED directions (each
// direction cubemap-sampled at the SSRT-fallback mip, DALC-ratio normalised, skylighting
// visibility applied), PREMULTIPLIED by A = confidence (march coverage; 0 = no data). t21 is
// the next free slot after t20 -- see the map above.
Texture2D<float4> SsgiEnvIrradianceTexture : register(t21);

// Decoder duplicated from features/Screen Space GI/Shaders/ScreenSpaceGI/common.hlsli
// (SSGI_DecodeBentNormal) on purpose: package shaders must not #include across a feature
// directory that may be absent at runtime. Any change there must be mirrored here.
void SSGI_DecodeBentNormal(float4 enc, out float3 o_dir, out float o_aperture)
{
	float2 f = enc.xy * 2.0 - 1.0;
	float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
	float t = saturate(-n.z);
	n.xy += n.xy >= 0.0 ? float2(-t, -t) : float2(t, t);
	o_dir = normalize(n);
	o_aperture = enc.z;
}

void SampleSSGI(uint2 pixCoord, float3 normalWS, out float ao, out float3 il)
{
	ao = 1 - SsgiAoTexture[pixCoord].x;
	float4 ssgiIlYSh = SsgiYTexture[pixCoord];
	// without ZH hallucination
	// float ssgiIlY = SphericalHarmonics::FuncProductIntegral(ssgiIlYSh, SphericalHarmonics::EvaluateCosineLobe(normalWS));
	float ssgiIlY = SphericalHarmonics::SHHallucinateZH3Irradiance(ssgiIlYSh, normalWS);
	float2 ssgiIlCoCg = SsgiCoCgTexture[pixCoord].xy;
	il = max(0, Color::YCoCgToRGB(float3(ssgiIlY, ssgiIlCoCg)));
}

void SampleSSGISpecular(uint2 pixCoord, sh2 lobe, out float ao, out float3 il, in float3 normal, in float3 view, in float roughness)
{
	ao = 1 - SsgiAoTexture[pixCoord].x;
	float NdotV = dot(normal, view);
	ao = Color::SpecularAOLagarde(saturate(NdotV), ao, roughness);
	// Write the out parameter unconditionally at entry: in the SSRT permutation with
	// EnableSpecular off, no branch below ever assigned it, so the caller consumed an
	// uninitialised value. Single assignment, single exit - no early return that fxc's
	// dead-code elimination could fold away.
	il = 0;
#	if defined(SSRT)
	// SSRT owns the specular path in this permutation; the SSGI specular IL below is not
	// compiled in, so il stays 0 whether EnableSpecular is on or off.
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

#if defined(SSRT)
Texture2D<float4> SSRTexture : register(t16);
// (ambient reinjection) Spatially smoothed SSRT diffuse hit confidence, written by
// ssrt_diffuse_composite.hlsl earlier in the same frame (Deferred::DeferredPasses calls
// DrawSSRTDiffuse before this dispatch). R8_UNORM, so a read is a [0,1] value by
// construction and needs no finiteness test of its own; the write side is covered by the
// G2 sanitisation the ray march already applies to the channel it comes from.
Texture2D<float> SSRTConfidenceTexture : register(t19);
// (batch 36b, deviation 2) The accumulated miss bent normal: the mean direction the rays that resolved
// nothing escaped through, weighted by how much they missed (R8G8B8A8_SNORM, xyz world space). Read
// only when ssrtSettings.AmbientReinjection has bit 1 set.
Texture2D<float4> SSRTMissBentTexture : register(t22);
#endif

// The ambient separation further down has two consumers, and only one of them is SSGI:
//
//  * SSGI needs the ambient MAIN already contains taken back out so it can shape it with
//    its own albedo-tinted MultiBounceAO and re-add it (the "F-A" mechanism -- the
//    estimate, then the subtract/re-add pair);
//  * SSRT's confidence-guided ambient reinjection needs the same reconstruction so it can
//    re-add only the fraction its rays did *not* resolve.
//
// features/Screen Space GI and features/Screen Space Ray Tracing are both non-CORE feature
// folders, so "SSRT installed, SSGI not" is a shipping configuration and the reconstruction
// has to exist there too. Hoisting the shared half out of the SSGI gate is the only way to
// get that without a second copy of the IBL-aware estimate, and that estimate is the one
// piece of this file a pure-SSGI user depends on being exactly right.
//
// Nothing inside the hoisted region is changed by the hoist. In an SSGI build the
// `#if defined(SSGI)` islands hold the ssgiAo / ssgiIl / multiBounceAO code verbatim and
// `ambientKeep` is a compile-time 1.0 without SSRT, so an SSGI permutation emits what it
// emitted before.
#if defined(SSGI) || defined(SSRT)
#	define COMPOSITE_AMBIENT_SEPARATION
#endif

// Skylighting's diffuse visibility is needed by the ambient estimate's IBL term. Computed once per
// pixel where it is compiled in - a plain SSGI build without IBL must not pay for a probe fetch
// nothing reads. IBL is #undef'd above without DYNAMIC_CUBEMAPS, so inside an IBL build this
// condition is exactly IBL.hlsli's own `SKYLIGHTING && !INTERIOR` and the GetIBLColor overload
// always lines up.
#if defined(SKYLIGHTING) && defined(DYNAMIC_CUBEMAPS) && !defined(INTERIOR) && defined(IBL)
#	define COMPOSITE_AMBIENT_SKY
#endif

#if defined(SSGI) && defined(DYNAMIC_CUBEMAPS)
// (directional env) "Is this value usable arithmetic?" - the same explicit exponent bit test
// the SSGI/SSRT guards use instead of isfinite(), which fxc may fold away without /Gis (see
// isFiniteSafe in ScreenSpaceGI/common.hlsli for the full argument). True iff v is neither NaN
// nor +-Inf.
bool DirEnvIsFinite(float v)
{
	return (asuint(v) & 0x7F800000u) != 0x7F800000u;
}

// (directional env v2) v1's DirectionalEnvAnchorLuma helper (DALC + IBL-probe luminance along
// the bent direction) is gone: the luminance anchor now rides INSIDE the integral -- gi.cs.hlsl
// normalises every sampled direction by its own DALC-luminance / cubemap-mean-luminance ratio,
// the same construction the SSRT diffuse fallback uses with CubemapNormalization = 1
// (ssrt_raymarch.hlsl:1020/:1060). That is what "EnvLevel = 1 matches the level of the ambient
// this replaces" now rests on, and it is per-direction, so shadowed openings keep their own
// (dimmer, cooler) level instead of being re-lit to the anchor.
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

#if defined(COMPOSITE_AMBIENT_SEPARATION)

#	if defined(SSGI)
	float ssgiAo;
	float3 ssgiIl;
	SampleSSGI(dispatchID.xy, normalWS, ssgiAo, ssgiIl);
#	endif

	// Skylighting diffuse visibility, shared by every consumer below.
	//
	// ambientSkyVisibility: what the forward path (Lighting.hlsl:3230-3236) and the SSRT fallback
	//     (ssrt_raymarch.hlsl:602-612) attenuate with - upward-normal boost and MinDiffuseVisibility
	//     floor applied, so it can exceed 1. Reproduced verbatim; parity includes the quirks.
	//
	// Declared only where a consumer exists, so a build without IBL compiles to the same
	// instructions it did before this block was introduced.
#	if defined(IBL)
	float ambientSkyVisibility = 1.0;
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
	}
#	endif

	// A_est: this block's estimate of the ambient light MAIN already contains, subtracted back out
	// below. Only its LUMINANCE is corrected from the G-buffer (Masks.z), so its chroma has to be
	// built the way the forward path built it (Lighting.hlsl:3217-3252) or the difference survives
	// as an additive tint.
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

	// Fraction of the reconstructed ambient that survives into the frame. 1 everywhere the
	// SSRT ambient reinjection is not running, which is a compile-time constant without SSRT
	// and folds the multiply below away.
	float ambientKeep = 1.0;

#	if defined(SSRT)
	// Two mutually exclusive energy models, selected at runtime so toggling either one needs
	// no composite recompile (DiffuseMult is already gated on EnableDiffuse in
	// ScreenSpaceRayTracing::GetCommonBufferData).
	//
	// Legacy (AmbientReinjection == 0, and what every configuration did before this change):
	// SSRT diffuse drives the forward directional ambient to zero through AmbientMult, so
	// Masks.z arrives at ~0 and there is nothing left to separate. Overwriting Y with 0 does
	// NOT make the term zero, though: YCoCgToRGB(0, Co, Cg) is (Co - Cg, Cg, -Co - Cg), a pure
	// chroma vector built from A_est's chroma, and max(0, ...) keeps whichever channels came
	// out positive. The subtraction below would then take that residue out of MAIN, which
	// desaturates the SSRT path for no reason. Skip the separation instead.
	//
	// Reinjection (AmbientReinjection != 0): the forward ambient is *not* zeroed
	// (GetCommonBufferData pins AmbientMult to 1), so Masks.z carries a real luminance and the
	// reconstruction above is the honest vanilla+IBL ambient this pixel already received. The
	// SSRT diffuse composite has likewise already added its own conf-weighted radiance to MAIN.
	// Removing conf * A is what turns the two of them into one term instead of two:
	//
	//     MAIN = direct + A + sum_i conf_i * L_i / N        (before this pass)
	//     out  = direct + (1 - conf) * A + conf * Lbar      with conf = sum_i conf_i / N
	//                                                       and Lbar the conf-weighted mean
	//          = direct + lerp(A, Lbar, conf)
	//
	// i.e. exactly the ambient in the directions the rays could not resolve, plus exactly the
	// traced radiance in the directions they could. No direction is counted twice and none is
	// dropped. The ray march is what makes the pairing exact: in this mode it weights its
	// radiance by the same confidence it reports (ssrt_raymarch.hlsl), rather than emitting
	// full radiance for any hit that merely passed the validation threshold.
	//
	// AmbientReinjectionStrength scales only the removal, so 0 is a deliberately additive
	// GI mode and 1 is the energy-conserving one.
	[branch] if (SharedData::ssrtSettings.DiffuseMult > 0.0) {
		// (batch 36b) AmbientReinjection is a bit field now: bit 0 = reinjection, bit 1 = deviation 2.
		[branch] if ((SharedData::ssrtSettings.AmbientReinjection & 1u) != 0) {
			float ssrtConfidence = SSRTConfidenceTexture[dispatchID.xy];
			ambientKeep = saturate(1.0 - ssrtConfidence * SharedData::ssrtSettings.AmbientReinjectionStrength);
		} else {
			directionalAmbientColor = 0;
		}
	}
#	endif

	float maxScale = 1.0;
	if (directionalAmbientColor.x > 0.0)
		maxScale = min(maxScale, diffuseColor.x / directionalAmbientColor.x);
	if (directionalAmbientColor.y > 0.0)
		maxScale = min(maxScale, diffuseColor.y / directionalAmbientColor.y);
	if (directionalAmbientColor.z > 0.0)
		maxScale = min(maxScale, diffuseColor.z / directionalAmbientColor.z);
	directionalAmbientColor *= maxScale;

	// (directional env) The colour RE-ADDED below. The subtract side must stay
	// directionalAmbientColor - it is the estimate of what MAIN already contains and changing
	// it would unbalance the separation - so the directional environment channel swaps only
	// this, the re-add.
	float3 ambientReAddColor = directionalAmbientColor;

#	if defined(SSRT)
	// (batch 36b, deviation 2) Direction-aware reinjection. What is re-added is the forward ambient for the
	// part of the hemisphere the SSRT rays did not resolve, but evaluated along the surface normal -- as
	// if the unresolved directions were spread around it evenly. The ray march knows which directions
	// actually escaped; their accumulated mean (the miss bent normal) re-aims the DALC lookup, and the
	// re-add is scaled by DALC(bent) / DALC(normal) in luminance, so its chroma and its balance with
	// the removal above are untouched. Clamped to [0.5, 2] and faded in over short bent vectors
	// (pixels whose rays almost all hit something), so it corrects the direction and never decides
	// the amount -- that stays ambientKeep's job.
	[branch] if ((SharedData::ssrtSettings.AmbientReinjection & 2u) != 0 && SharedData::ssrtSettings.DiffuseMult > 0.0) {
		const float3 bent = SSRTMissBentTexture[dispatchID.xy].xyz;
		const float bentLength = length(bent);
		const float3 bentDir = bent / max(bentLength, 1e-4);
		const float lumNormal = Color::RGBToLuminance(Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(normalWS, 1.0)))));
		const float lumBent = Color::RGBToLuminance(Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(bentDir, 1.0)))));
		const float scale = lerp(1.0, clamp(lumBent / max(lumNormal, 1e-4), 0.5, 2.0), smoothstep(0.02, 0.1, bentLength));
		if ((asuint(scale) & 0x7F800000u) != 0x7F800000u)
			ambientReAddColor *= scale;
	}
#	endif

#	if defined(SSGI) && defined(DYNAMIC_CUBEMAPS)
	// (directional env v2) Swap the re-added ambient for SSGI's pre-integrated hemisphere
	// environment irradiance. v1 sampled the cubemap along ONE direction (the bent normal) at
	// an aperture-picked coarse mip, which made the chroma the cubemap's scene-average tint and
	// pushed it over the whole frame; the integral replaces that with "what this pixel's OPEN
	// directions actually see", each direction normalised against the game's own ambient level
	// and attenuated by the sky's visibility -- so shadowed openings go dimmer and sky-toned,
	// exactly like the SSRT diffuse fallback this integral is copied from.
	//
	// Runtime gates, no recompile needed for any of them (unchanged from v1):
	//  * EnableDirectionalEnv is pre-multiplied on the C++ side with "SSGI loaded and enabled".
	//  * With SSRT diffuse active (DiffuseMult > 0) this channel steps aside, exactly like the
	//    SSGI IL gate further down - the A/B switch between the two approaches.
	[branch] if (SharedData::ssgiSettings.EnableDirectionalEnv != 0
#		if defined(SSRT)
				 && !(SharedData::ssrtSettings.DiffuseMult > 0.0)
#		endif
	) {
		float4 envIrradiance = SsgiEnvIrradianceTexture[dispatchID.xy];
		// A is the march's coverage: RGB is premultiplied by it, so un-premultiply before use
		// and let A itself blend the channel against the flat ambient below. That one blend
		// covers every "no data" state at once - sky, first person, beyond the depth fade, the
		// feature warming up after a toggle - all of which degrade to the vanilla ambient
		// rather than to black or to a fabricated direction.
		float envWeight = saturate(envIrradiance.w);
		// EnvLevel scales the LINEAR irradiance, so the slider brightens the visible result
		// linearly without touching its chroma; the gamma conversion puts the value in the same
		// units as the gamma-space re-add slot it fills (the forward path added its ambient in
		// gamma - see the note at the re-add below).
		float3 envColor = Color::IrradianceToGamma(envIrradiance.rgb / max(envIrradiance.w, 1e-3) * SharedData::ssgiSettings.EnvLevel);

		// Numeric defence, same shape as v1: everything consumed must be finite (bit test -
		// fxc cannot fold it), or the whole pixel bypasses the channel and keeps the existing
		// ambient chroma. The upstream chain clamps its writes, but this is the last line
		// before MAIN and it stays.
		bool envValid = DirEnvIsFinite(envIrradiance.x) && DirEnvIsFinite(envIrradiance.y) &&
		                DirEnvIsFinite(envIrradiance.z) && DirEnvIsFinite(envIrradiance.w);
		envValid = envValid && DirEnvIsFinite(envColor.x) && DirEnvIsFinite(envColor.y) && DirEnvIsFinite(envColor.z);
		// albedo and maxScale mirror the estimate's own construction, so the re-add stays on
		// the same footing as the term it replaces.
		[flatten] if (envValid && envWeight > 1e-3)
			ambientReAddColor = lerp(directionalAmbientColor, max(0, envColor) * albedo * maxScale, envWeight);
	}
#	endif

	diffuseColor = max(0.0, diffuseColor - directionalAmbientColor);

	linDiffuseColor = Color::IrradianceToLinear(diffuseColor);

#	if defined(SSGI)
	float3 linAlbedo = Color::IrradianceToLinear(albedo / Color::PBRLightingScale);
#	endif

#	if defined(SSGI)
	float3 multiBounceAO = Color::MultiBounceAO(linAlbedo, ssgiAo);

	linDiffuseColor *= sqrt(multiBounceAO);
#	else
	// No SSGI occlusion signal in this build, so the shaping the separation exists to apply is the
	// identity and fxc folds it out. What is left of the subtract/re-add pair is
	// `diffuseColor - (1 - ambientKeep) * A`, exactly inverting the forward path's gamma-space
	// `diffuseColor += directionalAmbientColor`.
	const float3 multiBounceAO = 1.0;
#	endif

	diffuseColor = Color::IrradianceToGamma(linDiffuseColor);
	// `ambientKeep` multiplies in gamma space, i.e. on the same side of the transfer function
	// the forward path added the term on (Lighting.hlsl `diffuseColor += directionalAmbientColor`
	// with both operands gamma-encoded). That makes the removal the exact inverse of the
	// addition; the MultiBounceAO shaping stays in linear space, where it was derived. The
	// `multiBounceAO` factor is where the SSGI AO channel (contact term included) enters.
	// ambientReAddColor equals directionalAmbientColor except when the directional environment
	// channel above swapped the chroma source; without SSGI it is a compile-time alias and the
	// emitted code is unchanged.
	diffuseColor += Color::IrradianceToGamma(Color::IrradianceToLinear(ambientReAddColor) * multiBounceAO) * ambientKeep;
	linDiffuseColor = Color::IrradianceToLinear(diffuseColor);

#	if defined(SSGI)
	// ssgiIl is now analytically normalised diffuse illumination divided by PI (see
	// ScreenSpaceGI/gi.cs.hlsl), so the term it wants is exactly one true reflectance. `linAlbedo`
	// is that: the G-buffer stores TRUE_PBR albedo pre-multiplied by PBRLightingScale
	// (Lighting.hlsl:3601, `indirectDiffuseLobeWeight *= Color::PBRLightingScale`, then :3612
	// writes it out), so dividing by PBRLightingScale is the correct de-scale and is the same
	// term the AO path uses. The separate 1/0.65 that used to sit here was never an albedo
	// de-scale - it was upstream's (2ea3b3adc) energy fudge for lowering the default GIStrength
	// from 1.5 to 1.0, and with the integrator normalised it has nothing left to compensate for.
#		if defined(SSRT)
	// SSGI IL and SSRT diffuse are competing
	// answers to "what arrives from the environment", and ssrt_diffuse_composite.hlsl has
	// already added confidence-weighted traced radiance for this pixel wherever DiffuseMult
	// is non-zero. Adding the SSGI estimate of the same hemisphere on top double-counts.
	// Runtime check, not compile-time: the SSGI and SSRT defines are independent (both
	// features can be loaded at once - Deferred.cpp composite define lists), and DiffuseMult
	// is already gated on EnableDiffuse in ScreenSpaceRayTracing::GetCommonBufferData, so
	// toggling SSRT diffuse needs no composite recompile. Only IL is gated - the SSGI AO
	// channel (contact term included) enters through multiBounceAO above and stays active.
	if (!(SharedData::ssrtSettings.DiffuseMult > 0.0))
		linDiffuseColor += ssgiIl * linAlbedo;
#		else
	linDiffuseColor += ssgiIl * linAlbedo;
#		endif
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
