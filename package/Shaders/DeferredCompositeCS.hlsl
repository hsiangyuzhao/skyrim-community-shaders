
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
Texture2D<unorm float4> SsgiBentNormalTexture : register(t20);

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

// (directional env) Anchor luminance for the directional environment channel: the brightness
// the game's own ambient would have along the bent direction, i.e. DALC luminance plus - when
// Image Based Lighting drives the ambient - the IBL probe luminance, mirroring exactly how the
// diffuse ambient estimate in main() splits the two (DALCAmount scale, probe added on top).
// Used ONLY by the new channel; E1's specular normalisation and SSRT's internal fallbacks keep
// their own established copies untouched.
float DirectionalEnvAnchorLuma(float3 bentNormalWS
#	if defined(COMPOSITE_AMBIENT_SKY)
	,
	float skyVisibility
#	endif
)
{
	float luma = Color::RGBToLuminance(Color::Ambient(max(0, mul(SharedData::DirectionalAmbient, float4(bentNormalWS, 1.0)))));
#	if defined(IBL)
	[branch] if (SharedData::iblSettings.EnableDiffuseIBL != 0 &&
		(!SharedData::InInterior || SharedData::iblSettings.EnableInterior != 0)) {
		luma *= SharedData::iblSettings.DALCAmount;
#		if defined(COMPOSITE_AMBIENT_SKY)
		float3 iblColor = ImageBasedLighting::GetIBLColor(-bentNormalWS, skyVisibility);
#		else
		float3 iblColor = ImageBasedLighting::GetIBLColor(-bentNormalWS);
#		endif
		iblColor = Color::IrradianceToGamma(Color::Saturation(iblColor, SharedData::iblSettings.IBLSaturation) * SharedData::iblSettings.DiffuseIBLScale);
		luma += Color::RGBToLuminance(iblColor);
	}
#	endif
	return luma;
}
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
		[branch] if (SharedData::ssrtSettings.AmbientReinjection != 0) {
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

#	if defined(SSGI) && defined(DYNAMIC_CUBEMAPS)
	// (directional env) B2: replace the CHROMA SOURCE of the re-added ambient. Instead of the
	// flat DALC/IBL tint, sample the dynamic cubemap along SSGI's bent normal - the mean
	// unoccluded direction - so a doorway wall facing the warm outside and one facing the cool
	// interior pick up different tints from the actual scene. The luminance scale comes from
	// anchoring the cubemap sample (normalised by its own coarsest-mip mean luminance, the same
	// trick the specular env path uses) to the DALC+IBL luminance along that direction, so at
	// EnvLevel = 1 the overall level matches the ambient this replaces.
	//
	// Runtime gates, no recompile needed for any of them:
	//  * EnableDirectionalEnv is pre-multiplied on the C++ side with "SSGI loaded and enabled".
	//  * With SSRT diffuse active (DiffuseMult > 0) this channel steps aside, exactly like the
	//    SSGI IL gate further down: SSRT's traced radiance already answers "what arrives from
	//    the environment", and this is the A/B switch between the two approaches.
	//
	// The APERTURE is used for mip selection only, NOT as an occlusion factor: ssgiAo already
	// darkens this very term through multiBounceAO below, and Skylighting/contact occlusion
	// enter through their own established points, so letting the aperture attenuate as well
	// would count the same occlusion twice. Trade-off accepted for this version: a sealed pixel
	// gets a sharp but heavily AO-darkened sample instead of a second darkening.
	[branch] if (SharedData::ssgiSettings.EnableDirectionalEnv != 0
#		if defined(SSRT)
				 && !(SharedData::ssrtSettings.DiffuseMult > 0.0)
#		endif
	) {
		float3 bentNormalWS;
		float bentAperture;
		SSGI_DecodeBentNormal(SsgiBentNormalTexture[dispatchID.xy], bentNormalWS, bentAperture);

		// Aperture to mip: wide opening -> coarse mip (wide prefiltered cone), narrow opening ->
		// finer mip. The cubemap carries 8 mips (DynamicCubemaps MIPLEVELS); 6 is the 2x2 level,
		// deliberately never the 1x1 mean so the direction always matters, and 2 (32x32) is
		// blurred enough for what is still a diffuse gather through a narrow opening.
		float envMip = lerp(2.0, 6.0, bentAperture);
		float3 envSample = EnvTexture.SampleLevel(LinearSampler, bentNormalWS, envMip);
		// Mean luminance via the coarsest mip, same fetch the E1-style specular normalisation
		// uses (level 15 clamps to the top of the chain).
		float cubemapMeanLuma = Color::RGBToLuminance(EnvTexture.SampleLevel(LinearSampler, bentNormalWS, 15));

#		if defined(COMPOSITE_AMBIENT_SKY)
		float anchorLuma = DirectionalEnvAnchorLuma(bentNormalWS, ambientSkyVisibility);
#		else
		float anchorLuma = DirectionalEnvAnchorLuma(bentNormalWS);
#		endif

		// Numeric defence: the mean-luma division is only meaningful for a finite, positive
		// denominator, and the product must come out finite (bit test - fxc cannot fold it).
		// Anything else bypasses the whole channel and falls back to the existing ambient
		// chroma, so a black or poisoned cubemap face can never turn into a black screen or a
		// NaN in MAIN.
		float3 envColor = envSample * (anchorLuma * SharedData::ssgiSettings.EnvLevel / max(cubemapMeanLuma, 1e-4));
		bool envValid = DirEnvIsFinite(cubemapMeanLuma) && cubemapMeanLuma > 1e-4;
		envValid = envValid && DirEnvIsFinite(envColor.x) && DirEnvIsFinite(envColor.y) && DirEnvIsFinite(envColor.z);
		// albedo and maxScale mirror the estimate's own construction, so the re-add stays on
		// the same footing as the term it replaces.
		[flatten] if (envValid)
			ambientReAddColor = max(0, envColor) * albedo * maxScale;
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
